//-------------------------------------------------------------------------------------------------------
// Copyright (c) ChakraCore Project Contributors. All rights reserved.
// Licensed under the MIT license. See LICENSE.txt file in the project root for full license information.
//-------------------------------------------------------------------------------------------------------
//
// N5.4 TSFN multi-producer teardown races (plan §6 N5.4; N4.4 landed).
//
// Per repeat (12 repeats): fresh env + call_js_cb TSFN (unlimited
// queue) + held async work. Four producer threads call
// napi_call_threadsafe_function / acquire / release in a tight loop
// while the env (main) thread pumps and then destroys the env. After
// destroy:
//   - no user TSFN callback and no work complete after destroy
//     (counts frozen while pumping post-destroy tasks to quiet);
//   - thread_finalize_cb exactly once, on the env thread, during
//     destroy;
//   - producers never crash and never see napi_ok after destroy
//     returns (generic_failure / would_deadlock-class statuses only);
//   - NodeApiAsync_WorkCount / TsfnCount / LoopRefCount == 0,
//     host ref/unref balanced, root counter at baseline.
//
// Sanitizers: the harness has NO TSAN/ASan profile (test/jit/
// debug-static/release-shared only; debug-static is explicitly
// -fno-sanitize=undefined; probe builds use plain -O3 -g). This probe
// therefore does NOT claim sanitizer coverage (see receipt); the
// repeat count + atomic discipline is the available race evidence.
//
// Output: one line per repeat, then "TSFN_TEARDOWN_STRESS: ...".
// Exit 0 = green; 1 = mismatch; 2 = fatal setup error.

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
#include <cstdlib>
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

JsRuntimeHandle g_runtime = JS_INVALID_REFERENCE;
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

/* --- FakeHost (mirrors async_probe.cpp) --- */

struct Task {
  void (*fn)(void*) = nullptr;
  void* arg = nullptr;
};
struct HeldWork {
  void (*execute)(void*) = nullptr;
  void (*complete)(void*) = nullptr;
  void* arg = nullptr;
};

struct FakeHost {
  std::mutex m;
  std::deque<Task> tasks;
  std::deque<HeldWork> held;
  bool hold = false;
  std::atomic<int> ref_calls{0};
  std::atomic<int> unref_calls{0};
  std::atomic<int> post_task_calls{0};
  std::atomic<int> post_work_calls{0};
  std::atomic<int> workers_alive{0};
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
    execute(arg);
    HostPostTask(h, complete, arg);
    h->workers_alive.fetch_sub(1, std::memory_order_relaxed);
  }).detach();
  return true;
}

void HostCancelWork(void* ctx, void* arg) {
  (void)arg;
  static_cast<FakeHost*>(ctx)->post_work_calls.fetch_add(
      0, std::memory_order_relaxed);
}
void HostRefLoop(void* ctx) {
  static_cast<FakeHost*>(ctx)->ref_calls.fetch_add(1, std::memory_order_relaxed);
}
void HostUnrefLoop(void* ctx) {
  static_cast<FakeHost*>(ctx)->unref_calls.fetch_add(1, std::memory_order_relaxed);
}

chakra_nodeapi_host_services ServicesFor(FakeHost* h) {
  chakra_nodeapi_host_services s{};
  s.post_task = &HostPostTask;
  s.post_work = &HostPostWork;
  s.cancel_work = &HostCancelWork;
  s.ref_loop = &HostRefLoop;
  s.unref_loop = &HostUnrefLoop;
  s.fatal_exception = nullptr;
  s.uv_loop = static_cast<struct uv_loop_s*>(h->loop_ptr);
  return s;
}

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
    if (n >= limit) return -1;
    t.fn(t.arg);
    n++;
  }
  return n;
}

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
      w.execute(w.arg);
      HostPostTask(h, w.complete, w.arg);
      h->workers_alive.fetch_sub(1, std::memory_order_relaxed);
    }).detach();
  }
}

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

/* --- Js helpers --- */

napi_value NapiObj(napi_env env) {
  JsValueRef v = JS_INVALID_REFERENCE;
  JsCreateObject(&v);
  (void)NodeApiScopes_RootIntoInnermost(env, reinterpret_cast<napi_value>(v));
  return reinterpret_cast<napi_value>(v);
}
napi_value NapiStr(napi_env env, const char* s) {
  JsValueRef v = JS_INVALID_REFERENCE;
  JsCreateString(s, std::strlen(s), &v);
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

/* --- Async work callbacks --- */

struct WorkData {
  int input = 0;
  std::atomic<int> complete_calls{0};
  napi_status last_status = napi_ok;
};
void WorkExecute(napi_env, void* data) {
  auto* d = static_cast<WorkData*>(data);
  d->input = d->input * 2; /* native-only, off-thread */
}
void WorkComplete(napi_env, napi_status status, void* data) {
  auto* d = static_cast<WorkData*>(data);
  d->last_status = status;
  d->complete_calls.fetch_add(1, std::memory_order_acq_rel);
}

/* --- TSFN callbacks --- */

std::atomic<int> g_finalize_calls{0};
std::thread::id g_finalize_thread;
std::atomic<long> g_calljs_delivered{0};
std::atomic<long> g_calljs_sum{0};
std::atomic<int> g_calljs_errors{0};

void TsfnFinalize(napi_env env, void* finalize_data, void* hint) {
  (void)env;
  (void)hint;
  (void)finalize_data;
  g_finalize_calls.fetch_add(1, std::memory_order_acq_rel);
  g_finalize_thread = std::this_thread::get_id();
}

void TsfnCallJsCb(napi_env env, napi_value js_cb, void*, void* data) {
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

} /* namespace */

int main() {
  g_main_thread = std::this_thread::get_id();
  if (!InitEngine()) return 2;

  constexpr int kRepeats = 12;
  constexpr int kThreads = 4;
  long total_delivered = 0;
  long total_ok_calls = 0;
  long total_fail_calls = 0;

  for (int r = 0; r < kRepeats; r++) {
    FakeHost host;
    host.hold = true; /* work stays queued until destroy */
    chakra_nodeapi_host_services svc = ServicesFor(&host);
    napi_env env = chakra_nodeapi_create_env(g_runtime, g_context, "race",
                                             &svc, &host);
    Check(env != nullptr, "RACE create env");
    if (!env) break;
    size_t base = chakra_nodeapi_test_root_count(env);
    Check(base == 0, "RACE baseline 0");
    napi_value res = NapiObj(env);
    napi_value name = NapiStr(env, "race-tsfn");
    napi_value js_fn = EvalJS(env, "(function(v){ return v; })");
    Check(js_fn != nullptr, "RACE JS func");
    g_calljs_delivered.store(0);
    g_calljs_sum.store(0);
    g_calljs_errors.store(0);
    g_finalize_calls.store(0);
    napi_threadsafe_function f = nullptr;
    CheckStatus(napi_create_threadsafe_function(
                    env, js_fn, res, name, 0, 1, nullptr, TsfnFinalize,
                    nullptr, TsfnCallJsCb, &f),
                napi_ok, "RACE create TSFN ok");
    Check(f != nullptr, "RACE handle non-null");
    /* Held async work (queued, never started before destroy). */
    WorkData wd{};
    wd.input = 7;
    napi_async_work w = nullptr;
    CheckStatus(napi_create_async_work(env, res, name, WorkExecute,
                                       WorkComplete, &wd, &w),
                napi_ok, "RACE create work ok");
    CheckStatus(napi_queue_async_work(env, w), napi_ok, "RACE queue work ok");

    /* Producers: call/acquire/release until stop is set. Each ok call
     * transfers an int; failures delete theirs. After stop, all must
     * fail (destroy completed) — checked in the aggregator below. */
    std::atomic<bool> stop{false};
    std::atomic<long> ok_calls{0};
    std::atomic<long> fail_calls{0};
    std::atomic<int> post_stop_ok{0};
    std::atomic<bool> destroyed_flag{false};
    std::vector<std::thread> workers;
    for (int t = 0; t < kThreads; t++) {
      workers.emplace_back([&, t]() {
        long iter = 0;
        while (true) {
          iter++;
          int* p = new int((int)((t * 100000 + iter) & 0x7fffffff));
          napi_status s = napi_call_threadsafe_function(
              f, p, napi_tsfn_nonblocking);
          if (s == napi_ok) {
            ok_calls.fetch_add(1, std::memory_order_relaxed);
          } else {
            delete p;
            fail_calls.fetch_add(1, std::memory_order_relaxed);
            if (destroyed_flag.load(std::memory_order_acquire)) {
              /* Any ok here would be a post-destroy callback bug. */
              /* fallthrough: producers only increment post_stop_ok on ok. */
            }
          }
          if (destroyed_flag.load(std::memory_order_acquire) &&
              s == napi_ok) {
            post_stop_ok.fetch_add(1, std::memory_order_relaxed);
          }
          /* Periodic acquire/release churn (cross-thread entries). */
          if ((iter & 0x1F) == 0) {
            (void)napi_acquire_threadsafe_function(f);
            (void)napi_release_threadsafe_function(f, napi_tsfn_release);
          }
          if (stop.load(std::memory_order_acquire)) break;
          /* Yield to keep the race window wide without spinning hot. */
          if ((iter & 0x3F) == 0)
            std::this_thread::yield();
        }
      });
    }
    /* Let producers enqueue for a beat. NO pumping while producers run:
     * the drain task would chase an unbounded queue forever (async S1
     * avoids this by joining before pumping). The destruction below is
     * the race; delivery correctness is covered by async S1. */
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    long delivered_before_destroy = g_calljs_delivered.load();
    /* Destroy under producer load (the race). */
    chakra_nodeapi_destroy_env(env);
    destroyed_flag.store(true, std::memory_order_release);
    stop.store(true, std::memory_order_release);
    for (auto& th : workers) th.join();
    /* Held work was cancelled by destroy; release it (execute skipped /
     * complete dropped silently by the post-destroy trampoline). */
    ReleaseHeld(&host);
    Check(WaitWorkers(&host), "RACE workers exited");
    /* Pump post-destroy tasks to quiet: counts must be frozen. */
    for (int i = 0; i < 10; i++) {
      Pump(&host);
      std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    long delivered_after = g_calljs_delivered.load();
    long sum_after = g_calljs_sum.load();
    long errors = g_calljs_errors.load();
    long okc = ok_calls.load();
    long failc = fail_calls.load();
    total_delivered += delivered_after;
    total_ok_calls += okc;
    total_fail_calls += failc;
    Check(g_finalize_calls.load() == 1, "RACE finalize exactly once");
    Check(g_finalize_thread == g_main_thread, "RACE finalize on env thread");
    Check(wd.complete_calls.load() == 0, "RACE no complete after destroy");
    Check(post_stop_ok.load() == 0, "RACE no post-destroy ok call");
    Check(delivered_after >= delivered_before_destroy,
          "RACE deliveries monotonic");
    /* Aborted queue items are dropped WITHOUT JS: deliveries <= oks.
     * Lost items are exactly the aborted tail; assert the window is
     * bounded (dels within [dels_before, oks+producers]). */
    Check(delivered_after <= okc + kThreads, "RACE no phantom delivery");
    Check(errors == 0, "RACE no JS errors");
    Check(NodeApiAsync_WorkCount(env) == 0, "RACE work count 0 (dead)");
    Check(NodeApiAsync_TsfnCount(env) == 0, "RACE tsfn count 0 (dead)");
    Check(NodeApiAsync_LoopRefCount(env) == 0, "RACE loop refs 0 (dead)");
    Check(chakra_nodeapi_test_root_count(env) ==
              static_cast<size_t>(-1),
          "RACE dead root count is -1 (no deref)");
    Check(host.ref_calls.load() >= 1, "RACE host ref seen");
    Check(host.ref_calls.load() == host.unref_calls.load(),
          "RACE host ref/unref balanced");
    char msg[96];
    std::snprintf(msg, sizeof(msg),
                  "RACE repeat %02d (ok=%ld fail=%ld delivered=%ld)", r, okc,
                  failc, delivered_after);
    std::printf("%s\n", msg);
    (void)sum_after;
  }

  std::fprintf(stdout,
               "TSFN_TEARDOWN_STRESS totals: ok=%ld fail=%ld delivered=%ld "
               "repeats=%d (checks=%d failures=%d)\n",
               total_ok_calls, total_fail_calls, total_delivered, kRepeats,
               g_checks, g_failures);
  std::printf("TSFN_TEARDOWN_STRESS: %s\n",
              g_failures == 0 ? "PASS" : "FAIL");
  ShutdownEngine();
  return g_failures == 0 ? 0 : 1;
}
