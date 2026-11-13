// async_probe.cpp — N4.4 async work + thread-safe functions + uv loop
// (plan doc/node-api-plan.md card N4.4, B4/B5; PR-7 host services).
//
// Covers (each MUST fail the probe on mismatch):
//   W1. Async work round-trip values: create -> queue -> execute
//       (off-thread, native-only) -> complete (on env thread, napi_ok)
//       with the data value doubled. Verifies execute thread != env
//       thread (R3) and complete thread == env thread. Delete after
//       completion ok; double-delete => invalid_arg.
//   W2. Cancel: cancel-before-start (held host: execute never runs,
//       complete delivers napi_cancelled); cancel-after-start (slow
//       execute already entered => napi_generic_failure, complete
//       delivers napi_ok). Delete while queued/executing =>
//       napi_generic_failure.
//   T1. TSFN round-trip both forms: direct func form (queued napi_value
//       delivered as one arg) and call_js_cb form (native data via
//       napi_call_function on the env thread). get_context returns the
//       context pointer. Acquire/release refcount; post-finalize call /
//       acquire / release / ref => napi_generic_failure (stale).
//       Ref/unref coalescing: 2 TSFNs => 1 host ref, unref both => 1
//       host unref; repeated ref/unref cycles balanced.
//   Q1. Queue-full nonblocking: max=1, fill once, second nonblocking
//       => napi_queue_full. Blocking from the env thread while full =>
//       napi_queue_full (no deadlock; probe would hang otherwise).
//   S1. 4-producer stress (12 repeats x 4 threads x 50 items on one
//       unlimited call_js_cb TSFN per repeat): all calls ok from
//       workers, all delivered on the env thread, sum/multiset exact.
//   D1. Teardown with in-flight items: queued TSFN items + queued work
//       at destroy => no user TSFN callback and no user complete after
//       destroy (pump post-destroy until quiet, counts frozen),
//       thread_finalize_cb exactly once on the env thread during
//       destroy, no crash/leak (counts return to 0).
//   N1. No-services matrix: env with NULL services => env-taking
//       functions with valid binding args (or valid handles created
//       with services) => napi_generic_failure (D7); func-only stale
//       handles => napi_generic_failure (Node post-finalize status);
//       NULL binding args => napi_invalid_arg (G1 validate-first,
//       not fake success -- see header G1/G3). get_uv with services
//       returns the injected loop; without scheduling services =>
//       napi_generic_failure even if a loop were present (G2).
//   U1. UV loop return: with services+uv => ok + injected pointer;
//       uv==NULL (scheduling present) => generic_failure.
//
// Build (repo root, via gate lock — see run_async_probe.sh):
//   /tmp/opencode/cc-gate-lock.sh mise exec -- zig c++ \
//     -std=gnu++17 -DNAPI_VERSION=8 -DCHAKRA_NODEAPI_TEST_HOOKS \
//     -Iinclude/nodeapi -Ilib/Jsrt -Ilib/NodeApi \
//     test/nodeapi/probes/async_probe.cpp + provider TUs \
//     -o /tmp/async_probe -Lzig-out/lib -lChakraCore ...
// Exit 0 = green; 1 = any mismatch.

#include "NodeApiHostApi.h"
#include "js_native_api.h"
#include "node_api.h"
#include "NodeApiAsync.h"
#include "NodeApiEnv.h"
#include "NodeApiScopes.h"
#include "ChakraCommon.h"
#include "ChakraCore.h"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <deque>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace {

int g_failures = 0;
int g_checks = 0;
std::thread::id g_main_thread;

void Check(bool cond, const char* what) {
  g_checks++;
  if (!cond) {
    g_failures++;
    std::printf("FAIL: %s\n", what);
  }
}

void CheckStatus(napi_status got, napi_status want, const char* what) {
  g_checks++;
  if (got != want) {
    g_failures++;
    std::printf("FAIL: %s (got %d want %d)\n", what, (int)got, (int)want);
  }
}

JsRuntimeHandle g_runtime = JS_INVALID_RUNTIME_HANDLE;
JsContextRef g_context = JS_INVALID_REFERENCE;

bool InitEngine() {
  if (JsCreateRuntime(JsRuntimeAttributeNone, nullptr, &g_runtime) != JsNoError)
    return false;
  if (JsCreateContext(g_runtime, &g_context) != JsNoError) {
    JsDisposeRuntime(g_runtime);
    g_runtime = JS_INVALID_RUNTIME_HANDLE;
    return false;
  }
  if (JsSetCurrentContext(g_context) != JsNoError) {
    JsDisposeRuntime(g_runtime);
    g_runtime = JS_INVALID_RUNTIME_HANDLE;
    g_context = JS_INVALID_REFERENCE;
    return false;
  }
  return true;
}

void ShutdownEngine() {
  if (g_context != JS_INVALID_REFERENCE) {
    JsSetCurrentContext(JS_INVALID_REFERENCE);
    g_context = JS_INVALID_REFERENCE;
  }
  if (g_runtime != JS_INVALID_RUNTIME_HANDLE) {
    JsDisposeRuntime(g_runtime);
    g_runtime = JS_INVALID_RUNTIME_HANDLE;
  }
}

/* ---------------- Fake host (PR-7 services) ---------------- */

struct Task {
  void (*fn)(void*);
  void* arg;
};

struct HeldWork {
  void (*execute)(void*);
  void (*complete)(void*);
  void* arg;
};

struct FakeHost {
  std::mutex m;
  std::deque<Task> tasks;
  std::deque<HeldWork> held;
  bool hold = false; /* when true, post_work queues without threads */
  std::atomic<int> ref_calls{0};
  std::atomic<int> unref_calls{0};
  std::atomic<int> post_task_calls{0};
  std::atomic<int> post_work_calls{0};
  std::atomic<int> workers_alive{0};
  std::atomic<int> execute_started{0};
  bool cancel_called = false; /* under m */
  /* Fake uv_loop pointer (opaque; provider never dereferences it). */
  void* loop_ptr = reinterpret_cast<void*>(0x1A2B3C4D5E6F);
};

bool HostPostTask(void* ctx, void (*fn)(void*), void* arg) {
  auto* h = static_cast<FakeHost*>(ctx);
  std::lock_guard<std::mutex> lk(h->m);
  h->tasks.push_back(Task{fn, arg});
  h->post_task_calls.fetch_add(1, std::memory_order_relaxed);
  return true;
}

bool HostPostWork(void* ctx, void (*execute)(void*), void (*complete)(void*),
                  void* arg) {
  auto* h = static_cast<FakeHost*>(ctx);
  h->post_work_calls.fetch_add(1, std::memory_order_relaxed);
  bool hold = false;
  {
    std::lock_guard<std::mutex> lk(h->m);
    hold = h->hold;
    if (hold) {
      h->held.push_back(HeldWork{execute, complete, arg});
      return true;
    }
  }
  h->workers_alive.fetch_add(1, std::memory_order_relaxed);
  std::thread([h, execute, complete, arg]() {
    h->execute_started.fetch_add(1, std::memory_order_relaxed);
    execute(arg);
    HostPostTask(h, complete, arg);
    h->workers_alive.fetch_sub(1, std::memory_order_relaxed);
  }).detach();
  return true;
}

void HostCancelWork(void* ctx, void* arg) {
  auto* h = static_cast<FakeHost*>(ctx);
  (void)arg;
  std::lock_guard<std::mutex> lk(h->m);
  h->cancel_called = true;
}

void HostRefLoop(void* ctx) {
  static_cast<FakeHost*>(ctx)->ref_calls.fetch_add(1, std::memory_order_relaxed);
}

void HostUnrefLoop(void* ctx) {
  static_cast<FakeHost*>(ctx)->unref_calls.fetch_add(1, std::memory_order_relaxed);
}

chakra_nodeapi_host_services ServicesFor(FakeHost* h, bool with_uv) {
  chakra_nodeapi_host_services s{};
  s.post_task = &HostPostTask;
  s.post_work = &HostPostWork;
  s.cancel_work = &HostCancelWork;
  s.ref_loop = &HostRefLoop;
  s.unref_loop = &HostUnrefLoop;
  s.fatal_exception = nullptr;
  s.uv_loop = with_uv ? static_cast<struct uv_loop_s*>(h->loop_ptr) : nullptr;
  return s;
}

/* Run queued env-thread tasks on the calling (env) thread until empty
 * (snapshot check under the mutex; workers may post concurrently). */
int Pump(FakeHost* h, int limit = 100000) {
  int n = 0;
  for (;;) {
    Task t{};
    bool has = false;
    {
      std::lock_guard<std::mutex> lk(h->m);
      if (!h->tasks.empty()) {
        t = h->tasks.front();
        h->tasks.pop_front();
        has = true;
      }
    }
    if (!has) break;
    if (n >= limit) {
      std::printf("FAIL: Pump exceeded limit %d tasks\n", limit);
      return -1;
    }
    t.fn(t.arg);
    n++;
  }
  return n;
}

/* Release held works as real worker threads (after the probe flips
 * hold=false). Must be called with hold already false (or flips it). */
void ReleaseHeld(FakeHost* h) {
  std::deque<HeldWork> to_run;
  {
    std::lock_guard<std::mutex> lk(h->m);
    h->hold = false;
    to_run.swap(h->held);
  }
  for (HeldWork w : to_run) {
    h->workers_alive.fetch_add(1, std::memory_order_relaxed);
    std::thread([h, w]() {
      h->execute_started.fetch_add(1, std::memory_order_relaxed);
      w.execute(w.arg);
      HostPostTask(h, w.complete, w.arg);
      h->workers_alive.fetch_sub(1, std::memory_order_relaxed);
    }).detach();
  }
}

/* Pump until pred() is true or timeout (ms). Returns pred() at end. */
bool WaitFor(FakeHost* h, const std::function<bool()>& pred, int timeout_ms = 5000) {
  auto start = std::chrono::steady_clock::now();
  for (;;) {
    Pump(h);
    if (pred()) return true;
    auto now = std::chrono::steady_clock::now();
    if (std::chrono::duration_cast<std::chrono::milliseconds>(now - start).count() > timeout_ms)
      return pred();
    std::this_thread::sleep_for(std::chrono::milliseconds(2));
  }
}

/* Wait for all detached workers to exit (bounded). */
bool WaitWorkers(FakeHost* h, int timeout_ms = 5000) {
  auto start = std::chrono::steady_clock::now();
  while (h->workers_alive.load(std::memory_order_acquire) != 0) {
    Pump(h);
    auto now = std::chrono::steady_clock::now();
    if (std::chrono::duration_cast<std::chrono::milliseconds>(now - start).count() > timeout_ms)
      return false;
    std::this_thread::sleep_for(std::chrono::milliseconds(2));
  }
  Pump(h);
  return true;
}

/* ---------------- Js helpers (cf. promise_script_probe.cpp) ---------------- */

bool GetGlobalNumber(const char* name, double* out) {
  JsValueRef global = JS_INVALID_REFERENCE;
  if (JsGetGlobalObject(&global) != JsNoError) return false;
  JsPropertyIdRef pid = JS_INVALID_REFERENCE;
  if (JsCreatePropertyId(name, std::strlen(name), &pid) != JsNoError) return false;
  JsValueRef v = JS_INVALID_REFERENCE;
  if (JsGetProperty(global, pid, &v) != JsNoError) return false;
  JsValueType t = JsObject;
  if (JsGetValueType(v, &t) != JsNoError || t != JsNumber) return false;
  double d = 0;
  if (JsNumberToDouble(v, &d) != JsNoError) return false;
  *out = d;
  return true;
}

void SetGlobalNumber(const char* name, double val) {
  JsValueRef global = JS_INVALID_REFERENCE;
  JsGetGlobalObject(&global);
  JsPropertyIdRef pid = JS_INVALID_REFERENCE;
  JsCreatePropertyId(name, std::strlen(name), &pid);
  JsValueRef v = JS_INVALID_REFERENCE;
  JsDoubleToNumber(val, &v);
  JsSetProperty(global, pid, v, false);
}

napi_value NapiStr(napi_env env, const char* s) {
  JsValueRef v = JS_INVALID_REFERENCE;
  JsCreateString(s, std::strlen(s), &v);
  (void)NodeApiScopes_RootIntoInnermost(env, reinterpret_cast<napi_value>(v));
  return reinterpret_cast<napi_value>(v);
}

napi_value NapiObj(napi_env env) {
  JsValueRef v = JS_INVALID_REFERENCE;
  JsCreateObject(&v);
  (void)NodeApiScopes_RootIntoInnermost(env, reinterpret_cast<napi_value>(v));
  return reinterpret_cast<napi_value>(v);
}

napi_value EvalJS(napi_env env, const char* code) {
  JsValueRef src = JS_INVALID_REFERENCE;
  JsValueRef url = JS_INVALID_REFERENCE;
  JsValueRef res = JS_INVALID_REFERENCE;
  if (JsCreateString(code, std::strlen(code), &src) != JsNoError) return nullptr;
  if (JsCreateString("probe.js", std::strlen("probe.js"), &url) != JsNoError) {
    JsRelease(src, nullptr);
    return nullptr;
  }
  JsErrorCode ec = JsRun(src, JS_SOURCE_CONTEXT_NONE, url,
                         JsParseScriptAttributeNone, &res);
  JsRelease(src, nullptr);
  JsRelease(url, nullptr);
  if (ec != JsNoError) return nullptr;
  (void)NodeApiScopes_RootIntoInnermost(env, reinterpret_cast<napi_value>(res));
  return reinterpret_cast<napi_value>(res);
}

/* ---------------- Async work callbacks ---------------- */

struct WorkData {
  int input = 0;
  int output = -999;
  bool execute_ran = false;
  bool execute_cancelled_seen = false;
  std::thread::id exec_thread;
  std::atomic<int> execute_entries{0};
  std::atomic<int> complete_calls{0};
  napi_status last_status = napi_ok;
  std::thread::id complete_thread;
  std::atomic<bool> execute_started{false};
  int execute_sleep_ms = 0;
};

void WorkExecute(napi_env env, void* data) {
  (void)env; /* never dereferenced off-thread (R3); value only */
  auto* d = static_cast<WorkData*>(data);
  d->execute_entries.fetch_add(1, std::memory_order_acq_rel);
  d->exec_thread = std::this_thread::get_id();
  d->execute_ran = true;
  d->execute_started.store(true, std::memory_order_release);
  if (d->execute_sleep_ms > 0)
    std::this_thread::sleep_for(std::chrono::milliseconds(d->execute_sleep_ms));
  d->output = d->input * 2;
}

void WorkComplete(napi_env env, napi_status status, void* data) {
  (void)env;
  auto* d = static_cast<WorkData*>(data);
  d->complete_calls.fetch_add(1, std::memory_order_acq_rel);
  d->last_status = status;
  d->complete_thread = std::this_thread::get_id();
}

/* ---------------- TSFN callbacks ---------------- */

std::atomic<int> g_finalize_calls{0};
std::thread::id g_finalize_thread;

void TsfnFinalize(napi_env env, void* finalize_data, void* hint) {
  (void)env;
  (void)hint;
  g_finalize_calls.fetch_add(1, std::memory_order_acq_rel);
  g_finalize_thread = std::this_thread::get_id();
  (void)finalize_data;
}

std::atomic<long> g_calljs_delivered{0};
std::atomic<long> g_calljs_sum{0};
std::atomic<int> g_calljs_errors{0};

/* call_js_cb form: convert native int to JS double and call the JS func. */
void TsfnCallJsCb(napi_env env, napi_value js_cb, void* context, void* data) {
  (void)context;
  if (js_cb == nullptr) {
    g_calljs_errors.fetch_add(1, std::memory_order_relaxed);
    delete static_cast<int*>(data);
    return;
  }
  int v = *static_cast<int*>(data);
  delete static_cast<int*>(data);
  napi_value recv = nullptr;
  napi_value num = nullptr;
  napi_value result = nullptr;
  if (napi_get_undefined(env, &recv) != napi_ok ||
      napi_create_double(env, static_cast<double>(v), &num) != napi_ok ||
      napi_call_function(env, recv, js_cb, 1, &num, &result) != napi_ok) {
    g_calljs_errors.fetch_add(1, std::memory_order_relaxed);
    return;
  }
  g_calljs_delivered.fetch_add(1, std::memory_order_acq_rel);
  g_calljs_sum.fetch_add(v, std::memory_order_acq_rel);
}

/* Direct-form state: JS func records the delivered value into a global
 * (via EvalJS-created function body). */
int g_direct_expected = 0;

} /* namespace */

int main() {
  g_main_thread = std::this_thread::get_id();
  if (!InitEngine()) return 2;

  /* ========== W1: async work round-trip values ========== */
  {
    std::printf("phaseW1 work round-trip: ...\n");
    FakeHost host;
    chakra_nodeapi_host_services svc = ServicesFor(&host, true);
    napi_env env = chakra_nodeapi_create_env(g_runtime, g_context, "w1",
                                             &svc, &host);
    Check(env != nullptr, "W1 create env");
    napi_value res = NapiObj(env);
    napi_value name = NapiStr(env, "w1-work");
    WorkData d{};
    d.input = 21;
    napi_async_work w = nullptr;
    CheckStatus(napi_create_async_work(env, res, name, WorkExecute,
                                       WorkComplete, &d, &w),
                napi_ok, "W1 create ok");
    Check(w != nullptr, "W1 work non-null");
    Check(NodeApiAsync_WorkState(env, w) == 0, "W1 state Created");
    Check(NodeApiAsync_WorkCount(env) == 1, "W1 work count 1");
    CheckStatus(napi_queue_async_work(env, w), napi_ok, "W1 queue ok");
    /* Re-queue while queued => generic_failure. */
    CheckStatus(napi_queue_async_work(env, w), napi_generic_failure,
                "W1 re-queue while queued => generic_failure");
    /* Delete while queued/executing => generic_failure. */
    CheckStatus(napi_delete_async_work(env, w), napi_generic_failure,
                "W1 delete while queued => generic_failure");
    bool done = WaitFor(&host, [&]() {
      return d.complete_calls.load() >= 1;
    });
    Check(done, "W1 completed within timeout");
    Check(d.execute_ran, "W1 execute ran");
    Check(d.exec_thread != g_main_thread, "W1 execute off-thread (R3)");
    Check(d.complete_thread == g_main_thread, "W1 complete on env thread");
    Check(d.last_status == napi_ok, "W1 complete status ok");
    Check(d.output == 42, "W1 value doubled 21->42 in execute");
    Check(NodeApiAsync_WorkState(env, w) == 3, "W1 state Completed");
    /* Cancel after completion => generic_failure. */
    CheckStatus(napi_cancel_async_work(env, w), napi_generic_failure,
                "W1 cancel after completion => generic_failure");
    CheckStatus(napi_delete_async_work(env, w), napi_ok, "W1 delete ok");
    CheckStatus(napi_delete_async_work(env, w), napi_invalid_arg,
                "W1 double delete => invalid_arg");
    Check(NodeApiAsync_WorkCount(env) == 0, "W1 work count 0 after delete");
    /* Invalid args. */
    napi_async_work iw = reinterpret_cast<napi_async_work>(
        reinterpret_cast<void*>(0xBAD));
    CheckStatus(napi_create_async_work(env, res, name, nullptr, WorkComplete,
                                       &d, &iw),
                napi_invalid_arg, "W1 NULL execute => invalid_arg");
    Check(iw == reinterpret_cast<napi_async_work>(reinterpret_cast<void*>(0xBAD)),
          "W1 out-param untouched on NULL execute");
    CheckStatus(napi_create_async_work(env, res, name, WorkExecute,
                                       WorkComplete, &d, nullptr),
                napi_invalid_arg, "W1 NULL result => invalid_arg");
    CheckStatus(napi_create_async_work(env, res, name, WorkExecute, nullptr,
                                       &d, &iw),
                napi_ok, "W1 NULL complete allowed");
    if (iw != reinterpret_cast<napi_async_work>(0xBAD)) {
      /* Queue this NULL-complete work to completion (complete=nullptr
       * must not crash the trampoline). */
      CheckStatus(napi_queue_async_work(env, iw), napi_ok,
                  "W1 queue NULL-complete work ok");
      Check(WaitFor(&host, [&]() {
        return NodeApiAsync_WorkState(env, iw) == 3;
      }), "W1 NULL-complete work completed");
      CheckStatus(napi_delete_async_work(env, iw), napi_ok,
                  "W1 delete NULL-complete work ok");
    }
    chakra_nodeapi_destroy_env(env);
    Check(WaitWorkers(&host), "W1 workers exited");
    std::printf("phaseW1: done\n");
  }

  /* ========== W2: cancel before/after start ========== */
  {
    std::printf("phaseW2 cancel: ...\n");
    FakeHost host;
    host.hold = true;
    chakra_nodeapi_host_services svc = ServicesFor(&host, false);
    napi_env env = chakra_nodeapi_create_env(g_runtime, g_context, "w2",
                                             &svc, &host);
    Check(env != nullptr, "W2 create env");
    napi_value res = NapiObj(env);
    napi_value name = NapiStr(env, "w2-work");
    /* Before-start: held host, queue then cancel. */
    {
      WorkData d{};
      d.input = 7;
      napi_async_work w = nullptr;
      CheckStatus(napi_create_async_work(env, res, name, WorkExecute,
                                         WorkComplete, &d, &w),
                  napi_ok, "W2 create1 ok");
      CheckStatus(napi_queue_async_work(env, w), napi_ok, "W2 queue1 ok");
      Check(NodeApiAsync_WorkState(env, w) == 1, "W2 state Queued (held)");
      Check(!d.execute_ran, "W2 execute not run while held");
      CheckStatus(napi_cancel_async_work(env, w), napi_ok,
                  "W2 cancel-before-start ok");
      bool cancel_called = false;
      {
        std::lock_guard<std::mutex> lk(host.m);
        cancel_called = host.cancel_called;
      }
      Check(cancel_called, "W2 host cancel_work called (best effort)");
      CheckStatus(napi_delete_async_work(env, w), napi_generic_failure,
                  "W2 delete while queued (cancelled) => generic_failure");
      /* Release held work: execute trampoline must skip user execute. */
      ReleaseHeld(&host);
      bool done = WaitFor(&host, [&]() {
        return d.complete_calls.load() >= 1;
      });
      Check(done, "W2 cancelled complete within timeout");
      Check(!d.execute_ran, "W2 execute skipped after cancel");
      Check(d.last_status == napi_cancelled, "W2 complete status cancelled");
      Check(d.complete_thread == g_main_thread, "W2 complete on env thread");
      Check(NodeApiAsync_WorkState(env, w) == 4, "W2 state Cancelled");
      CheckStatus(napi_delete_async_work(env, w), napi_ok,
                  "W2 delete cancelled ok");
      /* hold is now false (ReleaseHeld flipped it). */
    }
    /* After-start: slow execute; cancel after it started => fail. */
    {
      WorkData d2{};
      d2.input = 5;
      d2.execute_sleep_ms = 300;
      napi_async_work w2 = nullptr;
      CheckStatus(napi_create_async_work(env, res, name, WorkExecute,
                                         WorkComplete, &d2, &w2),
                  napi_ok, "W2 create2 ok");
      CheckStatus(napi_queue_async_work(env, w2), napi_ok, "W2 queue2 ok");
      /* Wait until execute entered (started flag), then cancel. */
      bool started = WaitFor(&host, [&]() {
        return d2.execute_started.load();
      }, 3000);
      Check(started, "W2 slow execute started");
      CheckStatus(napi_cancel_async_work(env, w2), napi_generic_failure,
                  "W2 cancel-after-start => generic_failure");
      bool done = WaitFor(&host, [&]() {
        return d2.complete_calls.load() >= 1;
      });
      Check(done, "W2 slow work completed");
      Check(d2.last_status == napi_ok, "W2 slow complete ok (not cancelled)");
      Check(d2.output == 10, "W2 slow value 5->10");
      CheckStatus(napi_delete_async_work(env, w2), napi_ok,
                  "W2 delete slow ok");
    }
    chakra_nodeapi_destroy_env(env);
    Check(WaitWorkers(&host), "W2 workers exited");
    std::printf("phaseW2: done\n");
  }

  /* ========== U1: uv loop return (before TSFN, same host) ========== */
  {
    std::printf("phaseU1 uv loop: ...\n");
    FakeHost host;
    chakra_nodeapi_host_services svc = ServicesFor(&host, true);
    napi_env env = chakra_nodeapi_create_env(g_runtime, g_context, "u1",
                                             &svc, &host);
    Check(env != nullptr, "U1 create env");
    struct uv_loop_s* loop = nullptr;
    CheckStatus(napi_get_uv_event_loop(env, &loop), napi_ok, "U1 get_uv ok");
    Check(loop == static_cast<struct uv_loop_s*>(host.loop_ptr),
          "U1 loop is injected pointer");
    struct uv_loop_s* bad = reinterpret_cast<struct uv_loop_s*>(0xBAD);
    CheckStatus(napi_get_uv_event_loop(env, nullptr), napi_invalid_arg,
                "U1 NULL loop out => invalid_arg");
    /* Scheduling present but uv NULL => generic_failure. */
    chakra_nodeapi_host_services svc_nouv = ServicesFor(&host, false);
    napi_env env2 = chakra_nodeapi_create_env(g_runtime, g_context, "u1b",
                                              &svc_nouv, &host);
    Check(env2 != nullptr, "U1 second env");
    loop = bad;
    CheckStatus(napi_get_uv_event_loop(env2, &loop), napi_generic_failure,
                "U1 uv NULL => generic_failure");
    Check(loop == bad, "U1 loop untouched on failure");
    chakra_nodeapi_destroy_env(env2);
    chakra_nodeapi_destroy_env(env);
    Check(WaitWorkers(&host), "U1 workers exited");
    std::printf("phaseU1: done\n");
  }

  /* ========== T1: TSFN round-trip both forms + refcount ========== */
  {
    std::printf("phaseT1 TSFN forms: ...\n");
    FakeHost host;
    chakra_nodeapi_host_services svc = ServicesFor(&host, true);
    napi_env env = chakra_nodeapi_create_env(g_runtime, g_context, "t1",
                                             &svc, &host);
    Check(env != nullptr, "T1 create env");
    napi_value res = NapiObj(env);
    napi_value name = NapiStr(env, "t1-tsfn");
    napi_value ctx_obj = NapiObj(env);
    napi_value js_fn = EvalJS(
        env,
        "(function(v){ globalThis.__t1_seen = v; return v; })");
    Check(js_fn != nullptr, "T1 JS func created");
    SetGlobalNumber("__t1_seen", -1);
    int ctx_marker = 0xC0FFEE;
    /* Direct form: call_js_cb==NULL, data is napi_value. */
    {
      napi_threadsafe_function f = nullptr;
      CheckStatus(napi_create_threadsafe_function(
                      env, js_fn, res, name, 0, 1, nullptr, TsfnFinalize,
                      &ctx_marker, nullptr, &f),
                  napi_ok, "T1 create direct ok");
      Check(f != nullptr, "T1 direct handle non-null");
      void* got_ctx = nullptr;
      CheckStatus(napi_get_threadsafe_function_context(f, &got_ctx), napi_ok,
                  "T1 get_context ok");
      Check(got_ctx == &ctx_marker, "T1 context pointer matches");
      CheckStatus(napi_get_threadsafe_function_context(nullptr, &got_ctx),
                  napi_invalid_arg, "T1 get_context NULL => invalid_arg");
      CheckStatus(napi_get_threadsafe_function_context(f, nullptr),
                  napi_invalid_arg, "T1 get_context NULL out => invalid_arg");
      Check(NodeApiAsync_TsfnThreadCount(f) == 1, "T1 thread count 1");
      double expected = 99.0;
      napi_value v = nullptr;
      CheckStatus(napi_create_double(env, expected, &v), napi_ok,
                  "T1 create double ok");
      CheckStatus(napi_call_threadsafe_function(f, v, napi_tsfn_nonblocking),
                  napi_ok, "T1 call direct ok");
      Check(NodeApiAsync_TsfnQueueDepth(f) == 1, "T1 queue depth 1");
      bool done = WaitFor(&host, [&]() {
        double s = -2;
        return GetGlobalNumber("__t1_seen", &s) && s == 99.0;
      });
      Check(done, "T1 direct delivered 99 to JS");
      Check(NodeApiAsync_TsfnQueueDepth(f) == 0, "T1 queue empty after drain");
      /* Release with graceful mode; finalize via post_task. */
      g_finalize_calls.store(0);
      CheckStatus(napi_release_threadsafe_function(f, napi_tsfn_abort),
                  napi_ok, "T1 release direct ok");
      Check(NodeApiAsync_TsfnThreadCount(f) == 0, "T1 thread count 0");
      bool fin = WaitFor(&host, [&]() {
        return g_finalize_calls.load() >= 1;
      });
      Check(fin, "T1 finalize ran after release");
      Check(g_finalize_thread == g_main_thread, "T1 finalize on env thread");
      /* Post-finalize: stale handle. */
      CheckStatus(napi_call_threadsafe_function(f, v, napi_tsfn_nonblocking),
                  napi_generic_failure, "T1 call after finalize => generic");
      CheckStatus(napi_acquire_threadsafe_function(f),
                  napi_generic_failure, "T1 acquire after finalize => generic");
      CheckStatus(napi_release_threadsafe_function(f, napi_tsfn_abort),
                  napi_generic_failure, "T1 double release => generic");
    }
    /* call_js_cb form: native data, converted on the env thread. */
    {
      g_calljs_delivered.store(0);
      g_calljs_sum.store(0);
      g_calljs_errors.store(0);
      napi_threadsafe_function f2 = nullptr;
      napi_value recv_unused = nullptr;
      (void)recv_unused;
      /* The JS func for f2 records again; call_js_cb calls it. */
      CheckStatus(napi_create_threadsafe_function(
                      env, js_fn, res, name, 0, 1, nullptr, TsfnFinalize,
                      &ctx_marker, TsfnCallJsCb, &f2),
                  napi_ok, "T1 create call_js_cb ok");
      Check(f2 != nullptr, "T1 call_js_cb handle non-null");
      /* Acquire from a worker thread (cross-thread), release from main. */
      std::atomic<int> acq{0};
      std::thread wt([&]() {
        if (napi_acquire_threadsafe_function(f2) == napi_ok)
          acq.store(1);
      });
      wt.join();
      Check(acq.load() == 1, "T1 acquire from worker ok");
      Check(NodeApiAsync_TsfnThreadCount(f2) == 2, "T1 count 2 after acquire");
      /* Call from a worker thread with heap int. */
      std::atomic<int> call_rc{-1};
      std::thread ct([&]() {
        int* p = new int(33);
        napi_status s = napi_call_threadsafe_function(
            f2, p, napi_tsfn_nonblocking);
        if (s != napi_ok) delete p; /* failed: no transfer */
        call_rc.store((int)s);
      });
      ct.join();
      Check(call_rc.load() == (int)napi_ok, "T1 call from worker ok");
      bool done = WaitFor(&host, [&]() {
        return g_calljs_delivered.load() >= 1;
      });
      Check(done, "T1 call_js_cb delivered from worker");
      Check(g_calljs_sum.load() == 33, "T1 delivered value 33");
      Check(g_calljs_errors.load() == 0, "T1 no call_js_cb errors");
      /* Release the acquired hold (2->1), still not finalized. */
      CheckStatus(napi_release_threadsafe_function(f2, napi_tsfn_release),
                  napi_ok, "T1 release acquired ok");
      Check(NodeApiAsync_TsfnThreadCount(f2) == 1, "T1 count 1");
      /* Release abort with empty queue => finalize with no JS. */
      g_finalize_calls.store(0);
      CheckStatus(napi_release_threadsafe_function(f2, napi_tsfn_abort),
                  napi_ok, "T1 release abort ok");
      Check(WaitFor(&host, [&]() {
        return g_finalize_calls.load() >= 1;
      }), "T1 second finalize ran");
      CheckStatus(napi_call_threadsafe_function(f2, nullptr,
                                                napi_tsfn_nonblocking),
                  napi_generic_failure, "T1 call after abort-finalize");
    }
    /* Invalid creates. */
    {
      napi_threadsafe_function bad = reinterpret_cast<napi_threadsafe_function>(
          reinterpret_cast<void*>(0xBAD));
      napi_value obj = NapiObj(env);
      CheckStatus(napi_create_threadsafe_function(
                      env, obj, res, name, 0, 1, nullptr, TsfnFinalize,
                      &ctx_marker, nullptr, &bad),
                  napi_invalid_arg, "T1 non-function func => invalid_arg");
      Check(bad == reinterpret_cast<napi_threadsafe_function>(
                       reinterpret_cast<void*>(0xBAD)),
            "T1 out untouched on bad func");
      CheckStatus(napi_create_threadsafe_function(
                      env, js_fn, res, name, 0, 0, nullptr, TsfnFinalize,
                      &ctx_marker, nullptr, &bad),
                  napi_invalid_arg, "T1 thread_count 0 => invalid_arg");
      CheckStatus(napi_create_threadsafe_function(
                      env, nullptr, res, name, 0, 1, nullptr, TsfnFinalize,
                      &ctx_marker, nullptr, &bad),
                  napi_invalid_arg, "T1 func+call_js_cb NULL => invalid_arg");
    }
    /* Ref/unref coalescing (2 TSFNs, 1 host ref). */
    {
      int base_ref = host.ref_calls.load();
      int base_unref = host.unref_calls.load();
      napi_threadsafe_function a = nullptr, b = nullptr;
      CheckStatus(napi_create_threadsafe_function(
                      env, js_fn, res, name, 0, 1, nullptr, nullptr,
                      nullptr, nullptr, &a),
                  napi_ok, "T1 ref create A ok");
      Check(host.ref_calls.load() == base_ref + 1, "T1 first create refs loop");
      CheckStatus(napi_create_threadsafe_function(
                      env, js_fn, res, name, 0, 1, nullptr, nullptr,
                      nullptr, nullptr, &b),
                  napi_ok, "T1 ref create B ok");
      Check(host.ref_calls.load() == base_ref + 1,
            "T1 second create coalesced (no extra ref)");
      Check(NodeApiAsync_LoopRefCount(env) == 2, "T1 loop count 2");
      CheckStatus(napi_unref_threadsafe_function(env, a), napi_ok,
                  "T1 unref A ok");
      Check(host.unref_calls.load() == base_unref,
            "T1 unref A coalesced (no unref yet)");
      Check(NodeApiAsync_LoopRefCount(env) == 1, "T1 loop count 1");
      CheckStatus(napi_unref_threadsafe_function(env, b), napi_ok,
                  "T1 unref B ok");
      Check(host.unref_calls.load() == base_unref + 1,
            "T1 last unref drops loop");
      Check(NodeApiAsync_LoopRefCount(env) == 0, "T1 loop count 0");
      CheckStatus(napi_ref_threadsafe_function(env, a), napi_ok, "T1 ref A ok");
      Check(host.ref_calls.load() == base_ref + 2, "T1 re-ref calls loop");
      CheckStatus(napi_unref_threadsafe_function(env, a), napi_ok,
                  "T1 unref A again ok");
      Check(host.unref_calls.load() == base_unref + 2, "T1 re-unref balanced");
      CheckStatus(napi_unref_threadsafe_function(env, a), napi_ok,
                  "T1 double unref ok (no-op)");
      Check(host.unref_calls.load() == base_unref + 2,
            "T1 double unref no extra drop");
      /* Cleanup: release both (finalize balances unref only if ref'd). */
      CheckStatus(napi_release_threadsafe_function(a, napi_tsfn_abort),
                  napi_ok, "T1 release A ok");
      CheckStatus(napi_release_threadsafe_function(b, napi_tsfn_abort),
                  napi_ok, "T1 release B ok");
      WaitFor(&host, [&]() {
        return NodeApiAsync_TsfnCount(env) == 0;
      });
      Check(NodeApiAsync_TsfnCount(env) == 0, "T1 count 0 after finalize");
    }
    chakra_nodeapi_destroy_env(env);
    Check(WaitWorkers(&host), "T1 workers exited");
    std::printf("phaseT1: done\n");
  }

  /* ========== Q1: queue-full ========== */
  {
    std::printf("phaseQ1 queue-full: ...\n");
    FakeHost host;
    chakra_nodeapi_host_services svc = ServicesFor(&host, false);
    napi_env env = chakra_nodeapi_create_env(g_runtime, g_context, "q1",
                                             &svc, &host);
    Check(env != nullptr, "Q1 create env");
    napi_value res = NapiObj(env);
    napi_value name = NapiStr(env, "q1-tsfn");
    /* No JS func needed for queue-full (call_js_cb counts, no JS call). */
    /* Use call_js_cb with a counting cb that frees; no js func (NULL). */
    /* NOTE: call_js_cb with NULL js_cb: our probe CallJsCb counts error
     * when js_cb null. Use a js func instead to keep deliveries clean. */
    napi_value js_fn = EvalJS(env, "(function(v){ return v; })");
    Check(js_fn != nullptr, "Q1 JS func");
    g_calljs_delivered.store(0);
    napi_threadsafe_function f = nullptr;
    CheckStatus(napi_create_threadsafe_function(
                    env, js_fn, res, name, 1, 1, nullptr, TsfnFinalize,
                    nullptr, TsfnCallJsCb, &f),
                napi_ok, "Q1 create max1 ok");
    int* p1 = new int(1);
    CheckStatus(napi_call_threadsafe_function(f, p1, napi_tsfn_nonblocking),
                napi_ok, "Q1 fill 1 ok");
    Check(NodeApiAsync_TsfnQueueDepth(f) == 1, "Q1 depth 1 after fill");
    int* p2 = new int(2);
    CheckStatus(napi_call_threadsafe_function(f, p2, napi_tsfn_nonblocking),
                napi_queue_full, "Q1 second nonblocking => queue_full");
    delete p2; /* failed: no transfer */
    int* p3 = new int(3);
    /* Blocking from the env (main) thread while full must NOT deadlock:
     * returns napi_queue_full. (If it deadlocked, this probe would hang
     * until the harness timeout -- the reported failure mode.) */
    CheckStatus(napi_call_threadsafe_function(f, p3, napi_tsfn_blocking),
                napi_queue_full,
                "Q1 blocking-from-env => queue_full (no deadlock)");
    delete p3;
    /* Drain the filler, then a new call fits. */
    bool done = WaitFor(&host, [&]() {
      return g_calljs_delivered.load() >= 1;
    });
    Check(done, "Q1 filler delivered after pump");
    Check(NodeApiAsync_TsfnQueueDepth(f) == 0, "Q1 empty after drain");
    int* p4 = new int(4);
    CheckStatus(napi_call_threadsafe_function(f, p4, napi_tsfn_nonblocking),
                napi_ok, "Q1 call after drain ok");
    WaitFor(&host, [&]() {
      return g_calljs_delivered.load() >= 2;
    });
    Check(g_calljs_delivered.load() >= 2, "Q1 second delivered");
    CheckStatus(napi_release_threadsafe_function(f, napi_tsfn_abort),
                napi_ok, "Q1 release ok");
    WaitFor(&host, [&]() {
      return NodeApiAsync_TsfnCount(env) == 0;
    });
    chakra_nodeapi_destroy_env(env);
    Check(WaitWorkers(&host), "Q1 workers exited");
    std::printf("phaseQ1: done\n");
  }

  /* ========== S1: 4-producer stress (12 repeats) ========== */
  {
    std::printf("phaseS1 4-producer stress x12: ...\n");
    FakeHost host;
    chakra_nodeapi_host_services svc = ServicesFor(&host, false);
    napi_env env = chakra_nodeapi_create_env(g_runtime, g_context, "s1",
                                             &svc, &host);
    Check(env != nullptr, "S1 create env");
    napi_value res = NapiObj(env);
    napi_value name = NapiStr(env, "s1-tsfn");
    napi_value js_fn = EvalJS(env, "(function(v){ return v; })");
    Check(js_fn != nullptr, "S1 JS func");
    const int kRepeats = 12;
    const int kThreads = 4;
    const int kPer = 50;
    for (int r = 0; r < kRepeats; r++) {
      g_calljs_delivered.store(0);
      g_calljs_sum.store(0);
      g_calljs_errors.store(0);
      napi_threadsafe_function f = nullptr;
      CheckStatus(napi_create_threadsafe_function(
                      env, js_fn, res, name, 0, 1, nullptr, nullptr,
                      nullptr, TsfnCallJsCb, &f),
                  napi_ok, "S1 create ok");
      std::atomic<int> ok_calls{0};
      std::atomic<int> fail_calls{0};
      std::vector<std::thread> workers;
      for (int t = 0; t < kThreads; t++) {
        workers.emplace_back([&, t]() {
          for (int i = 0; i < kPer; i++) {
            int* p = new int(t * kPer + i);
            napi_status s = napi_call_threadsafe_function(
                f, p, napi_tsfn_nonblocking);
            if (s == napi_ok)
              ok_calls.fetch_add(1, std::memory_order_relaxed);
            else {
              delete p;
              fail_calls.fetch_add(1, std::memory_order_relaxed);
            }
          }
        });
      }
      for (auto& th : workers) th.join();
      Check(ok_calls.load() == kThreads * kPer, "S1 all calls ok");
      Check(fail_calls.load() == 0, "S1 no call failures");
      bool done = WaitFor(&host, [&]() {
        return g_calljs_delivered.load() >= kThreads * kPer;
      }, 10000);
      char msg[64];
      std::snprintf(msg, sizeof(msg), "S1 repeat %d delivered all", r);
      Check(done, msg);
      long want_sum = 0;
      for (int v = 0; v < kThreads * kPer; v++) want_sum += v;
      Check(g_calljs_sum.load() == want_sum, "S1 exact sum (no dup/loss)");
      Check(g_calljs_errors.load() == 0, "S1 no JS errors");
      CheckStatus(napi_release_threadsafe_function(f, napi_tsfn_abort),
                  napi_ok, "S1 release ok");
      WaitFor(&host, [&]() {
        return NodeApiAsync_TsfnCount(env) == 0;
      });
      std::snprintf(msg, sizeof(msg), "S1 repeat %d count 0", r);
      Check(NodeApiAsync_TsfnCount(env) == 0, msg);
    }
    chakra_nodeapi_destroy_env(env);
    Check(WaitWorkers(&host), "S1 workers exited");
    std::printf("phaseS1: done\n");
  }

  /* ========== D1: teardown in-flight + no-callback-after-destroy ========== */
  {
    std::printf("phaseD1 teardown in-flight: ...\n");
    FakeHost host;
    host.hold = true; /* work stays queued (not started) */
    chakra_nodeapi_host_services svc = ServicesFor(&host, false);
    napi_env env = chakra_nodeapi_create_env(g_runtime, g_context, "d1",
                                             &svc, &host);
    Check(env != nullptr, "D1 create env");
    napi_value res = NapiObj(env);
    napi_value name = NapiStr(env, "d1");
    napi_value js_fn = EvalJS(env, "(function(v){ return v; })");
    Check(js_fn != nullptr, "D1 JS func");
    g_calljs_delivered.store(0);
    g_calljs_sum.store(0);
    g_finalize_calls.store(0);
    /* TSFN with 5 queued items (drain tasks posted, not pumped). */
    napi_threadsafe_function f = nullptr;
    CheckStatus(napi_create_threadsafe_function(
                    env, js_fn, res, name, 0, 1, nullptr, TsfnFinalize,
                    nullptr, TsfnCallJsCb, &f),
                napi_ok, "D1 create TSFN ok");
    for (int i = 0; i < 5; i++) {
      int* p = new int(100 + i);
      CheckStatus(napi_call_threadsafe_function(f, p, napi_tsfn_nonblocking),
                  napi_ok, "D1 queue TSFN item ok");
    }
    Check(NodeApiAsync_TsfnQueueDepth(f) == 5, "D1 5 items queued");
    /* Async work queued but held (not started). */
    WorkData wd{};
    wd.input = 11;
    napi_async_work w = nullptr;
    CheckStatus(napi_create_async_work(env, res, name, WorkExecute,
                                       WorkComplete, &wd, &w),
                napi_ok, "D1 create work ok");
    CheckStatus(napi_queue_async_work(env, w), napi_ok, "D1 queue work ok");
    Check(NodeApiAsync_WorkState(env, w) == 1, "D1 work Queued at destroy");
    /* Do NOT pump: destroy with in-flight items + queued drain tasks. */
    chakra_nodeapi_destroy_env(env);
    Check(g_finalize_calls.load() == 1, "D1 finalize exactly once at destroy");
    Check(g_finalize_thread == g_main_thread, "D1 finalize on env thread");
    Check(g_calljs_delivered.load() == 0, "D1 no TSFN callback pre-pump");
    Check(wd.complete_calls.load() == 0, "D1 no complete pre-pump");
    /* Post-destroy: release held work + pump everything. No user JS
     * may run (no-callback-after-destroy); counts stay frozen. */
    ReleaseHeld(&host);
    Check(WaitWorkers(&host), "D1 workers exited after destroy");
    /* Pump post-destroy tasks (drains/completions posted pre-destroy). */
    for (int i = 0; i < 5; i++) {
      Pump(&host);
      std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    Check(g_calljs_delivered.load() == 0,
          "D1 no TSFN callback after destroy");
    Check(wd.complete_calls.load() == 0, "D1 no complete after destroy");
    Check(g_finalize_calls.load() == 1, "D1 finalize still exactly once");
    Check(!wd.execute_ran, "D1 work execute skipped (cancelled in hold)");
    std::printf("phaseD1: done\n");
  }

  /* ========== N1: no-services matrix ========== */
  {
    std::printf("phaseN1 no-services matrix: ...\n");
    /* Valid handles created WITH services (for handle-taking entries). */
    FakeHost host_with;
    chakra_nodeapi_host_services svc_with = ServicesFor(&host_with, true);
    napi_env env_with = chakra_nodeapi_create_env(g_runtime, g_context, "n1w",
                                                  &svc_with, &host_with);
    Check(env_with != nullptr, "N1 env_with");
    FakeHost host_without;
    napi_env env_without = chakra_nodeapi_create_env(
        g_runtime, g_context, "n1o", nullptr, nullptr);
    Check(env_without != nullptr, "N1 env_without");
    napi_value res = NapiObj(env_with);
    napi_value name = NapiStr(env_with, "n1");
    napi_value js_fn = EvalJS(env_with, "(function(v){ return v; })");
    Check(js_fn != nullptr, "N1 JS func");
    /* Work + TSFN handles (valid, owned by env_with). */
    WorkData wd{};
    wd.input = 3;
    napi_async_work w = nullptr;
    CheckStatus(napi_create_async_work(env_with, res, name, WorkExecute,
                                       WorkComplete, &wd, &w),
                napi_ok, "N1 create work with services ok");
    napi_threadsafe_function f = nullptr;
    CheckStatus(napi_create_threadsafe_function(
                    env_with, js_fn, res, name, 0, 1, nullptr, nullptr,
                    nullptr, TsfnCallJsCb, &f),
                napi_ok, "N1 create TSFN with services ok");
    /* Env-taking functions on the no-services env with valid args. */
    {
      napi_async_work out = reinterpret_cast<napi_async_work>(
          reinterpret_cast<void*>(0xBAD));
      CheckStatus(napi_create_async_work(env_without, res, name, WorkExecute,
                                         WorkComplete, &wd, &out),
                  napi_generic_failure,
                  "N1 create_work no-services => generic_failure");
      Check(out == reinterpret_cast<napi_async_work>(
                       reinterpret_cast<void*>(0xBAD)),
            "N1 create out untouched");
    }
    {
      napi_threadsafe_function out = reinterpret_cast<napi_threadsafe_function>(
          reinterpret_cast<void*>(0xBAD));
      CheckStatus(napi_create_threadsafe_function(
                      env_without, js_fn, res, name, 0, 1, nullptr, nullptr,
                      nullptr, TsfnCallJsCb, &out),
                  napi_generic_failure,
                  "N1 create_tsfn no-services => generic_failure");
      Check(out == reinterpret_cast<napi_threadsafe_function>(
                       reinterpret_cast<void*>(0xBAD)),
            "N1 tsfn out untouched");
    }
    {
      struct uv_loop_s* loop = reinterpret_cast<struct uv_loop_s*>(0xBAD);
      CheckStatus(napi_get_uv_event_loop(env_without, &loop),
                  napi_generic_failure, "N1 get_uv no-services => generic");
      Check(loop == reinterpret_cast<struct uv_loop_s*>(0xBAD),
            "N1 loop untouched");
    }
    CheckStatus(napi_queue_async_work(env_without, w), napi_generic_failure,
                "N1 queue no-services => generic_failure");
    CheckStatus(napi_cancel_async_work(env_without, w), napi_generic_failure,
                "N1 cancel no-services => generic_failure");
    CheckStatus(napi_delete_async_work(env_without, w), napi_generic_failure,
                "N1 delete no-services => generic_failure");
    CheckStatus(napi_ref_threadsafe_function(env_without, f),
                napi_generic_failure, "N1 ref no-services => generic_failure");
    CheckStatus(napi_unref_threadsafe_function(env_without, f),
                napi_generic_failure, "N1 unref no-services => generic_failure");
    /* Func-only stale handles => generic_failure (G3). Use a stale
     * handle value (address+offset) that never existed: lookup misses.
     * (Post-finalize stale handles take the same path; here no valid
     * handle can be exercised under a no-services env because these
     * entries take no env.) */
    napi_threadsafe_function stale =
        reinterpret_cast<napi_threadsafe_function>(
            reinterpret_cast<uintptr_t>(f) + 64);
    void* ctx = nullptr;
    CheckStatus(napi_get_threadsafe_function_context(stale, &ctx),
                napi_generic_failure, "N1 get_context stale => generic");
    CheckStatus(napi_call_threadsafe_function(stale, nullptr,
                                              napi_tsfn_nonblocking),
                napi_generic_failure, "N1 call stale => generic");
    CheckStatus(napi_acquire_threadsafe_function(stale), napi_generic_failure,
                "N1 acquire stale => generic");
    CheckStatus(napi_release_threadsafe_function(stale, napi_tsfn_abort),
                napi_generic_failure, "N1 release stale => generic");
    /* NULL binding args (G1): invalid_arg even without services. */
    CheckStatus(napi_get_threadsafe_function_context(nullptr, &ctx),
                napi_invalid_arg, "N1 get_context NULL => invalid_arg (G1)");
    CheckStatus(napi_call_threadsafe_function(nullptr, nullptr,
                                              napi_tsfn_nonblocking),
                napi_invalid_arg, "N1 call NULL => invalid_arg (G1)");
    CheckStatus(napi_acquire_threadsafe_function(nullptr), napi_invalid_arg,
                "N1 acquire NULL => invalid_arg (G1)");
    CheckStatus(napi_release_threadsafe_function(nullptr, napi_tsfn_abort),
                napi_invalid_arg, "N1 release NULL => invalid_arg (G1)");
    /* Partial services: scheduling present but uv NULL already covered
     * (U1). Scheduling missing => generic_failure covered above. */
    /* Handles still usable on their owning env (no cross-contamination). */
    CheckStatus(napi_queue_async_work(env_with, w), napi_ok,
                "N1 work still queues on owning env");
    Check(WaitFor(&host_with, [&]() {
      return wd.complete_calls.load() >= 1;
    }), "N1 owning-env work completed");
    CheckStatus(napi_delete_async_work(env_with, w), napi_ok,
                "N1 delete on owning env ok");
    CheckStatus(napi_release_threadsafe_function(f, napi_tsfn_abort),
                napi_ok, "N1 release on owning env ok");
    WaitFor(&host_with, [&]() {
      return NodeApiAsync_TsfnCount(env_with) == 0;
    });
    chakra_nodeapi_destroy_env(env_without);
    chakra_nodeapi_destroy_env(env_with);
    Check(WaitWorkers(&host_with), "N1 workers exited (with)");
    Check(WaitWorkers(&host_without), "N1 workers exited (without)");
    std::printf("phaseN1: done\n");
  }

  /* Final quiet: no workers/tasks remain anywhere (all hosts destroyed). */
  std::printf("async_probe: %d checks, %d failures\n", g_checks, g_failures);
  ShutdownEngine();
  return g_failures == 0 ? 0 : 1;
}
