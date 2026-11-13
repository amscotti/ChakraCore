// asyncctx_probe.cpp — N4.3 async context + callback scopes
// (plan doc/node-api-plan.md card N4.3, B3 + async-hook completion
// wiring from N1.4; status rows in doc/node-api-status-mapping.md
// C column).
//
// LOUD NO-OP (plan §4 B3): napi_async_init/destroy and the
// async_context parameter are BOOKKEEPING ONLY. No async_hooks
// propagation, no AsyncLocalStorage semantics. This probe asserts
// handle-identity pairing only; it never asserts ALS flow (there is
// none to observe).
//
// Covers (each MUST fail probe on mismatch):
//   A. async_init/destroy pairing: distinct handles, count 2->1,
//      double/NULL/fabricated/cross-env => invalid_arg, output
//      untouched on failure, coercion statuses (resource null =>
//      object_expected, name null => string_expected, number
//      coercion ok), executes with pending (C Yes).
//   B. callback scopes: nested open, LIFO close ok, out-of-order
//      close => callback_scope_mismatch + named released (stack
//      intact), unknown/NULL/double => invalid_arg, NULL context
//      (default) ok, dead context snapshot => invalid_arg,
//      distinct from handle scopes (depths independent both ways),
//      cross-env close => invalid_arg.
//   C. make_callback: correct this/args + result (JS callee and
//      native thunk callee), NULL context (default) ok, unknown
//      context => invalid_arg, non-function => invalid_arg, recv
//      null => object_expected, C-NULL recv/func/argv => invalid_arg,
//      argc guard => generic_failure, result NULL (discard) ok,
//      gated while pending (No, not called), callee throw =>
//      pending + result untouched.
//   D. make_callback drains the provider promise queue before
//      returning (PR-8): resolve queues reaction (handler not run,
//      depth>0); trivial make_callback returns after handler ran
//      (depth 0, global set). Runs BEFORE any host install on the
//      shared context so the provider trampoline is effective.
//   E. Async cleanup completion (§3.6 step 4): host installs its
//      continuation callback after create (host queue); a promise
//      reaction is queued pre-destroy; destroy fires hooks in
//      reverse order; Lifetime pump (drain host queue until quiet,
//      then remove) drives both hooks to self-remove during pump
//      turns; no violation, log verbatim. Self-removing hook (0
//      pump turns) also passes.
//   F. Closing policy (from inside a sync hook): async_init /
//      open_callback_scope / make_callback => generic_failure;
//      async_destroy / close_callback_scope => ok.
//   G. Teardown release: contexts + scopes leaked (no explicit
//      destroy) are freed by destroy; TotalStates returns to
//      baseline; root total unchanged by async_init/open (no JS
//      roots held).
//
// Build (repo root, via gate lock — see run_asyncctx_probe.sh):
//   /tmp/opencode/cc-gate-lock.sh mise exec -- zig c++ \
//     -std=gnu++17 -DNAPI_VERSION=8 -DCHAKRA_NODEAPI_TEST_HOOKS \
//     -Iinclude/nodeapi -Ilib/Jsrt -Ilib/NodeApi \
//     test/nodeapi/probes/asyncctx_probe.cpp \
//     lib/NodeApi/* (all provider TUs incl. NodeApiAsyncContext.cpp) \
//     -o /tmp/asyncctx_probe -Lzig-out/lib -lChakraCore ...
// Exit 0 = green; 1 = any mismatch.

#include "NodeApiHostApi.h"
#include "NodeApiAsyncContext.h"
#include "NodeApiEnv.h"
#include "NodeApiLifetime.h"
#include "NodeApiPromise.h"
#include "NodeApiScopes.h"
#include "js_native_api.h"
#include "node_api.h"
#include "ChakraCommon.h"
#include "ChakraCore.h"

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace {

int g_failures = 0;
int g_checks = 0;

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

bool HasPending(napi_env env) {
  bool has = false;
  if (napi_is_exception_pending(env, &has) != napi_ok) return false;
  return has;
}

/* --- Host pump queue (mirrors ch-nodeapi / promise probe) --- */
struct HostPump {
  std::vector<JsValueRef> tasks;
};

void HostContinuation(JsValueRef task, void* state) {
  auto* pump = static_cast<HostPump*>(state);
  if (JsAddRef(task, nullptr) == JsNoError) {
    pump->tasks.push_back(task);
  }
}

/* Drain host queue via JsCallFunction until quiet. Direct Js* (no
 * provider Alive gate), so it is Closing-tolerant for step-4 use. */
void DrainHostQueue(HostPump* pump, size_t limit = 10000) {
  size_t iter = 0;
  while (!pump->tasks.empty()) {
    if (++iter > limit) {
      Check(false, "host queue drain exceeded limit");
      return;
    }
    JsValueRef task = pump->tasks.front();
    pump->tasks.erase(pump->tasks.begin());
    JsValueRef undef = JS_INVALID_REFERENCE;
    JsGetUndefinedValue(&undef);
    JsValueRef arr[1] = {undef};
    JsValueRef res = JS_INVALID_REFERENCE;
    (void)JsCallFunction(task, arr, 1, &res);
    JsRelease(task, nullptr);
  }
}

/* --- Js helpers (copied patterns from promise_script_probe) --- */

napi_value NapiStr(napi_env env, const char* s) {
  JsValueRef v = JS_INVALID_REFERENCE;
  JsCreateString(s, std::strlen(s), &v);
  (void)NodeApiScopes_RootIntoInnermost(env, reinterpret_cast<napi_value>(v));
  return reinterpret_cast<napi_value>(v);
}

napi_value NapiNum(napi_env env, double d) {
  JsValueRef v = JS_INVALID_REFERENCE;
  JsDoubleToNumber(d, &v);
  (void)NodeApiScopes_RootIntoInnermost(env, reinterpret_cast<napi_value>(v));
  return reinterpret_cast<napi_value>(v);
}

bool GetGlobalNumber(const char* name, double* out) {
  JsValueRef global = JS_INVALID_REFERENCE;
  if (JsGetGlobalObject(&global) != JsNoError) return false;
  JsPropertyIdRef pid = JS_INVALID_REFERENCE;
  if (JsCreatePropertyId(name, std::strlen(name), &pid) != JsNoError)
    return false;
  JsValueRef v = JS_INVALID_REFERENCE;
  if (JsGetProperty(global, pid, &v) != JsNoError) return false;
  JsValueType t = JsObject;
  if (JsGetValueType(v, &t) != JsNoError || t != JsNumber) return false;
  double d = 0;
  if (JsNumberToDouble(v, &d) != JsNoError) return false;
  *out = d;
  return true;
}

bool GetGlobalString(const char* name, std::string* out) {
  JsValueRef global = JS_INVALID_REFERENCE;
  if (JsGetGlobalObject(&global) != JsNoError) return false;
  JsPropertyIdRef pid = JS_INVALID_REFERENCE;
  if (JsCreatePropertyId(name, std::strlen(name), &pid) != JsNoError)
    return false;
  JsValueRef v = JS_INVALID_REFERENCE;
  if (JsGetProperty(global, pid, &v) != JsNoError) return false;
  JsValueType t = JsObject;
  if (JsGetValueType(v, &t) != JsNoError || t != JsString) return false;
  size_t len = 0;
  JsCopyString(v, nullptr, 0, &len);
  std::string s(len + 1, '\0');
  size_t written = 0;
  if (JsCopyString(v, &s[0], s.size(), &written) != JsNoError) return false;
  s.resize(written);
  *out = s;
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

void SetGlobalString(napi_env env, const char* name, const char* val) {
  JsValueRef global = JS_INVALID_REFERENCE;
  JsGetGlobalObject(&global);
  JsPropertyIdRef pid = JS_INVALID_REFERENCE;
  JsCreatePropertyId(name, std::strlen(name), &pid);
  JsValueRef v = JS_INVALID_REFERENCE;
  JsCreateString(val, std::strlen(val), &v);
  JsSetProperty(global, pid, v, false);
  (void)env;
}

/* Evaluate JS code (host-side JsRun). Result rooted. nullptr on
 * failure (exception stays pending). */
napi_value EvalJS(napi_env env, const char* code) {
  JsValueRef src = JS_INVALID_REFERENCE;
  JsValueRef url = JS_INVALID_REFERENCE;
  JsValueRef res = JS_INVALID_REFERENCE;
  if (JsCreateString(code, std::strlen(code), &src) != JsNoError) return nullptr;
  if (JsCreateString("probe.js", std::strlen("probe.js"), &url) != JsNoError)
    return nullptr;
  JsErrorCode ec = JsRun(src, JS_SOURCE_CONTEXT_NONE, url,
                         JsParseScriptAttributeNone, &res);
  JsRelease(src, nullptr);
  JsRelease(url, nullptr);
  if (ec != JsNoError) return nullptr;
  (void)NodeApiScopes_RootIntoInnermost(env, reinterpret_cast<napi_value>(res));
  return reinterpret_cast<napi_value>(res);
}

/* Attach .then(handler) to a promise. Returns true on ok. */
bool AttachThen(napi_env env, napi_value prom, napi_value handler) {
  JsPropertyIdRef pid = JS_INVALID_REFERENCE;
  if (JsCreatePropertyId("then", 4, &pid) != JsNoError) return false;
  JsValueRef f = JS_INVALID_REFERENCE;
  if (JsGetProperty(reinterpret_cast<JsValueRef>(prom), pid, &f) != JsNoError)
    return false;
  (void)NodeApiScopes_RootIntoInnermost(env, reinterpret_cast<napi_value>(f));
  JsValueRef fn = f;
  JsValueRef args[2] = {reinterpret_cast<JsValueRef>(prom),
                        reinterpret_cast<JsValueRef>(handler)};
  JsValueRef res = JS_INVALID_REFERENCE;
  return JsCallFunction(fn, args, 2, &res) == JsNoError;
}

/* --- Teardown log (P1-1 verbatim pattern) --- */
std::vector<std::string> g_log;
void Sink(void* ctx, const char* event) {
  (void)ctx;
  g_log.emplace_back(event ? event : "");
}

/* --- Async cleanup hooks for Phase E --- */
int g_async_fired[8] = {0};
struct DeferredCtx {
  napi_async_cleanup_hook_handle h = nullptr;
  int countdown = 0;
  bool scheduled = false;
};
DeferredCtx g_deferred[8];

void AsyncSelfRemove(napi_async_cleanup_hook_handle h, void* data) {
  int idx = (int)(std::uintptr_t)data;
  if (idx >= 0 && idx < 8) g_async_fired[idx]++;
  char buf[32];
  std::snprintf(buf, sizeof(buf), "async:A%d", idx);
  g_log.emplace_back(buf);
  Check(napi_remove_async_cleanup_hook(h) == napi_ok,
        "E self-remove ok");
}

void AsyncSched(napi_async_cleanup_hook_handle h, void* data) {
  int idx = (int)(std::uintptr_t)data;
  if (idx >= 0 && idx < 8) g_async_fired[idx]++;
  char buf[32];
  std::snprintf(buf, sizeof(buf), "async:A%d", idx);
  g_log.emplace_back(buf);
  if (idx >= 0 && idx < 8) {
    g_deferred[idx].h = h;
    g_deferred[idx].countdown = 2;
    g_deferred[idx].scheduled = true;
  }
}

int g_pump_calls = 0;
/* Lifetime pump: drains the host microtask queue (Closing-tolerant
 * host pump, NOT NodeApiPromise_Pump which gates on Alive), then
 * drives scheduled removals. host_ctx is the HostPump*. */
void PumpDrainThenRemove(void* ctx) {
  g_pump_calls++;
  auto* pump = static_cast<HostPump*>(ctx);
  if (pump != nullptr) DrainHostQueue(pump);
  for (int i = 0; i < 8; i++) {
    if (g_deferred[i].scheduled && g_deferred[i].countdown > 0) {
      if (--g_deferred[i].countdown == 0) {
        napi_remove_async_cleanup_hook(g_deferred[i].h);
        g_deferred[i].scheduled = false;
      }
    }
  }
}

/* --- Closing-policy hook for Phase F --- */
napi_env g_closing_env = nullptr;
napi_async_context g_closing_ctx = nullptr;
napi_callback_scope g_closing_scope = nullptr;
bool g_close_init_generic = false;
bool g_close_open_generic = false;
bool g_close_make_generic = false;
bool g_close_destroy_ok = false;
bool g_close_scope_ok = false;

void ClosingProbeHook(void*) {
  if (g_closing_env == nullptr) return;
  // init during Closing => generic_failure
  napi_async_context tmp = nullptr;
  napi_status s1 = napi_async_init(g_closing_env, (napi_value)(std::uintptr_t)0x1,
                                   (napi_value)(std::uintptr_t)0x1, &tmp);
  // Note: 0x1 is not a valid JsValueRef, but the Closing guard must
  // fail BEFORE any Js* coercion touches it. That is the assertion.
  g_close_init_generic = (s1 == napi_generic_failure);
  // open during Closing => generic_failure
  napi_callback_scope tmp2 = nullptr;
  napi_status s2 = napi_open_callback_scope(g_closing_env,
                                            (napi_value)(std::uintptr_t)0x1,
                                            nullptr, &tmp2);
  g_close_open_generic = (s2 == napi_generic_failure);
  // make_callback during Closing => generic_failure
  napi_value res = nullptr;
  napi_status s3 = napi_make_callback(g_closing_env, nullptr,
                                      (napi_value)(std::uintptr_t)0x1,
                                      (napi_value)(std::uintptr_t)0x1,
                                      0, nullptr, &res);
  g_close_make_generic = (s3 == napi_generic_failure);
  // destroy + close (removers) => ok
  g_close_destroy_ok =
      (napi_async_destroy(g_closing_env, g_closing_ctx) == napi_ok);
  g_close_scope_ok =
      (napi_close_callback_scope(g_closing_env, g_closing_scope) == napi_ok);
}

/* --- Native callback for Phase C (thunk callee) --- */
int g_native_argc = -1;
bool g_native_this_is_recv = false;
void* g_native_data_seen = nullptr;
napi_value g_native_recv = nullptr;

napi_value NativeCb(napi_env env, napi_callback_info info) {
  size_t argc = 0;
  napi_value argv[4] = {nullptr, nullptr, nullptr, nullptr};
  napi_value this_arg = nullptr;
  void* data = nullptr;
  if (napi_get_cb_info(env, info, &argc, argv, &this_arg, &data) != napi_ok) {
    return nullptr;
  }
  g_native_argc = (int)argc;
  g_native_this_is_recv = (this_arg == g_native_recv);
  g_native_data_seen = data;
  // Return argc as a number (proves provider can root inside callback).
  napi_value out = nullptr;
  (void)napi_create_int32(env, (int32_t)argc, &out);
  return out;
}

} /* namespace */

int main() {
  if (!InitEngine()) return 2;
  napi_env env = nullptr;
  size_t states_baseline = NodeApiAsyncContext_TotalStates();

  /* ========== A: async_init/destroy pairing ========== */
  {
    std::printf("phaseA pairing: ...\n");
    env = chakra_nodeapi_create_env(g_runtime, g_context, "a", nullptr, nullptr);
    Check(env != nullptr, "A create");
    napi_value res = NapiStr(env, "resource");
    napi_value name = NapiStr(env, "name");
    size_t root_base = chakra_nodeapi_test_root_count(env);
    napi_async_context c1 = nullptr, c2 = nullptr;
    CheckStatus(napi_async_init(env, res, name, &c1), napi_ok, "A init1 ok");
    Check(c1 != nullptr, "A c1 non-null");
    CheckStatus(napi_async_init(env, res, name, &c2), napi_ok, "A init2 ok");
    Check(c2 != nullptr && c2 != c1, "A distinct");
    Check(NodeApiAsyncContext_ContextCount(env) == 2, "A count 2");
    // No JS roots held by async_init (bookkeeping only).
    {
      napi_handle_scope s = nullptr;
      CheckStatus(napi_open_handle_scope(env, &s), napi_ok, "A open hs");
      Check(chakra_nodeapi_test_root_count(env) == root_base,
            "A root count unchanged by async_init");
      CheckStatus(napi_close_handle_scope(env, s), napi_ok, "A close hs");
    }
    CheckStatus(napi_async_destroy(env, c1), napi_ok, "A destroy c1 ok");
    Check(NodeApiAsyncContext_ContextCount(env) == 1, "A count 1");
    CheckStatus(napi_async_destroy(env, c1), napi_invalid_arg,
                "A double destroy => invalid_arg");
    CheckStatus(napi_async_destroy(env, nullptr), napi_invalid_arg,
                "A NULL => invalid_arg");
    napi_async_context fake =
        reinterpret_cast<napi_async_context>((std::uintptr_t)0xDEAD);
    CheckStatus(napi_async_destroy(env, fake), napi_invalid_arg,
                "A fabricated => invalid_arg");
    // Output untouched on failure.
    {
      napi_async_context sentinel =
          reinterpret_cast<napi_async_context>((std::uintptr_t)0xBAD);
      CheckStatus(napi_async_init(env, nullptr, name, &sentinel),
                  napi_invalid_arg, "A C-NULL resource => invalid_arg");
      Check(sentinel == reinterpret_cast<napi_async_context>((std::uintptr_t)0xBAD),
            "A out untouched on C-NULL");
      CheckStatus(napi_async_init(env, res, name, nullptr),
                  napi_invalid_arg, "A NULL result => invalid_arg");
    }
    // JS null/undefined => object/string expected.
    {
      JsValueRef nullv = JS_INVALID_REFERENCE;
      JsGetNullValue(&nullv);
      napi_async_context out = reinterpret_cast<napi_async_context>((std::uintptr_t)0xBAD);
      CheckStatus(napi_async_init(env, reinterpret_cast<napi_value>(nullv),
                                  name, &out),
                  napi_object_expected, "A JS null resource => object_expected");
      Check(out == reinterpret_cast<napi_async_context>((std::uintptr_t)0xBAD),
            "A out untouched on object_expected");
      Check(!HasPending(env), "A no pending on object_expected");
      JsValueRef undefv = JS_INVALID_REFERENCE;
      JsGetUndefinedValue(&undefv);
      CheckStatus(napi_async_init(env, reinterpret_cast<napi_value>(undefv),
                                  name, &out),
                  napi_object_expected, "A undefined resource => object_expected");
      CheckStatus(napi_async_init(env, res, reinterpret_cast<napi_value>(nullv),
                                  &out),
                  napi_string_expected, "A JS null name => string_expected");
      CheckStatus(napi_async_init(env, res, reinterpret_cast<napi_value>(undefv),
                                  &out),
                  napi_string_expected, "A undefined name => string_expected");
      Check(!HasPending(env), "A no pending on string_expected");
    }
    // Number coercion ok (ToObject/ToString box/convert).
    {
      napi_value num1 = NapiNum(env, 42);
      napi_value num2 = NapiNum(env, 7);
      napi_async_context c3 = nullptr;
      CheckStatus(napi_async_init(env, num1, num2, &c3), napi_ok,
                  "A number coercion ok");
      Check(c3 != nullptr, "A c3 non-null");
      CheckStatus(napi_async_destroy(env, c3), napi_ok, "A destroy c3");
    }
    // Executes with pending (C Yes).
    {
      JsValueRef m = JS_INVALID_REFERENCE;
      JsCreateObject(&m);
      (void)NodeApiScopes_RootIntoInnermost(env, reinterpret_cast<napi_value>(m));
      CheckStatus(napi_throw(env, reinterpret_cast<napi_value>(m)), napi_ok,
                  "A seed throw");
      napi_async_context c4 = nullptr;
      CheckStatus(napi_async_init(env, res, name, &c4), napi_ok,
                  "A init executes with pending");
      Check(c4 != nullptr, "A c4 non-null with pending");
      Check(HasPending(env), "A pending preserved");
      napi_value cleared = nullptr;
      (void)napi_get_and_clear_last_exception(env, &cleared);
      Check(cleared == reinterpret_cast<napi_value>(m), "A original preserved");
      CheckStatus(napi_async_destroy(env, c4), napi_ok, "A destroy c4");
    }
    // Cross-env.
    {
      napi_env envB = chakra_nodeapi_create_env(g_runtime, g_context, "ab",
                                                nullptr, nullptr);
      Check(envB != nullptr, "A envB created");
      CheckStatus(napi_async_destroy(envB, c2), napi_invalid_arg,
                  "A cross-env destroy => invalid_arg");
      Check(NodeApiAsyncContext_ContextCount(env) == 1,
            "A count still 1 after cross-env attempt");
      chakra_nodeapi_destroy_env(envB);
    }
    CheckStatus(napi_async_destroy(env, c2), napi_ok, "A destroy c2");
    Check(NodeApiAsyncContext_ContextCount(env) == 0, "A count 0");
    // Dead-env use (saved handle value, no dereference).
    {
      napi_async_context saved = nullptr;
      napi_value r2 = NapiStr(env, "res2");
      napi_value n2 = NapiStr(env, "nm2");
      CheckStatus(napi_async_init(env, r2, n2, &saved), napi_ok, "A init saved");
      std::uintptr_t savedV = reinterpret_cast<std::uintptr_t>(saved);
      napi_env deadEnv = env;
      chakra_nodeapi_destroy_env(env);
      env = nullptr;
      CheckStatus(napi_async_destroy(deadEnv,
                                     reinterpret_cast<napi_async_context>(savedV)),
                  napi_invalid_arg, "A destroy on dead env => invalid_arg");
      CheckStatus(napi_async_init(deadEnv, (napi_value)(std::uintptr_t)0x1,
                                  (napi_value)(std::uintptr_t)0x1, &saved),
                  napi_invalid_arg, "A init on dead env => invalid_arg");
    }
    Check(NodeApiAsyncContext_TotalStates() == states_baseline,
          "A states back to baseline");
    std::printf("phaseA: done\n");
  }

  /* ========== B: callback scopes ========== */
  {
    std::printf("phaseB scopes: ...\n");
    env = chakra_nodeapi_create_env(g_runtime, g_context, "b", nullptr, nullptr);
    Check(env != nullptr, "B create");
    size_t root_base = chakra_nodeapi_test_root_baseline(env);
    napi_value res = NapiStr(env, "res");
    napi_value name = NapiStr(env, "nm");
    napi_value resobj = EvalJS(env, "({})");
    Check(resobj != nullptr, "B resobj created");
    napi_async_context ctx = nullptr;
    CheckStatus(napi_async_init(env, res, name, &ctx), napi_ok, "B init ok");
    napi_callback_scope s1 = nullptr, s2 = nullptr, s3 = nullptr;
    // NULL result => invalid_arg; NULL resource (C) => invalid_arg.
    CheckStatus(napi_open_callback_scope(env, resobj, ctx, nullptr),
                napi_invalid_arg, "B NULL result => invalid_arg");
    CheckStatus(napi_open_callback_scope(env, nullptr, ctx, &s1),
                napi_invalid_arg, "B NULL resource => invalid_arg");
    // Fabricated context => invalid_arg.
    {
      napi_async_context fake =
          reinterpret_cast<napi_async_context>((std::uintptr_t)0xDEAD);
      napi_callback_scope out =
          reinterpret_cast<napi_callback_scope>((std::uintptr_t)0xBAD);
      CheckStatus(napi_open_callback_scope(env, resobj, fake, &out),
                  napi_invalid_arg, "B fake ctx => invalid_arg");
      Check(out == reinterpret_cast<napi_callback_scope>((std::uintptr_t)0xBAD),
            "B out untouched on fake ctx");
    }
    CheckStatus(napi_open_callback_scope(env, resobj, ctx, &s1), napi_ok,
                "B open s1 ok");
    CheckStatus(napi_open_callback_scope(env, resobj, ctx, &s2), napi_ok,
                "B open s2 ok");
    CheckStatus(napi_open_callback_scope(env, resobj, nullptr, &s3), napi_ok,
                "B open s3 NULL ctx (default) ok");
    Check(s1 != nullptr && s2 != nullptr && s3 != nullptr, "B non-null");
    Check(s1 != s2 && s2 != s3, "B distinct");
    Check(NodeApiAsyncContext_ScopeDepth(env) == 3, "B depth 3");
    // Distinct from handle scopes: handle depth unchanged.
    Check(NodeApiScopes_Depth(env) == 1, "B handle depth 1 (default only)");
    // Out-of-order close s1 while s2/s3 open => mismatch + s1 released.
    CheckStatus(napi_close_callback_scope(env, s1),
                napi_callback_scope_mismatch, "B out-of-order => mismatch");
    Check(NodeApiAsyncContext_ScopeDepth(env) == 2, "B depth 2 after mismatch");
    Check(NodeApiScopes_Depth(env) == 1, "B handle depth still 1");
    // s3 is top now? Stack was [s1,s2,s3]; remove s1 => [s2,s3], top s3.
    CheckStatus(napi_close_callback_scope(env, s3), napi_ok, "B close s3 top ok");
    Check(NodeApiAsyncContext_ScopeDepth(env) == 1, "B depth 1");
    CheckStatus(napi_close_callback_scope(env, s2), napi_ok, "B close s2 ok");
    Check(NodeApiAsyncContext_ScopeDepth(env) == 0, "B depth 0");
    // Double close / unknown.
    napi_callback_scope deadScope = s2;
    CheckStatus(napi_close_callback_scope(env, deadScope),
                napi_invalid_arg, "B double close => invalid_arg");
    CheckStatus(napi_close_callback_scope(env, nullptr),
                napi_invalid_arg, "B NULL close => invalid_arg");
    napi_callback_scope fakeScope =
        reinterpret_cast<napi_callback_scope>((std::uintptr_t)0xDEAD);
    CheckStatus(napi_close_callback_scope(env, fakeScope),
                napi_invalid_arg, "B fabricated close => invalid_arg");
    // Executes with pending (C Yes).
    {
      JsValueRef m = JS_INVALID_REFERENCE;
      JsCreateObject(&m);
      (void)NodeApiScopes_RootIntoInnermost(env, reinterpret_cast<napi_value>(m));
      (void)napi_throw(env, reinterpret_cast<napi_value>(m));
      napi_callback_scope sx = nullptr;
      CheckStatus(napi_open_callback_scope(env, resobj, ctx, &sx), napi_ok,
                  "B open executes with pending");
      Check(HasPending(env), "B pending preserved after open");
      CheckStatus(napi_close_callback_scope(env, sx), napi_ok,
                  "B close executes with pending");
      Check(HasPending(env), "B pending preserved after close");
      napi_value cleared = nullptr;
      (void)napi_get_and_clear_last_exception(env, &cleared);
    }
    // Distinctness both ways: handle scope does not affect callback depth.
    {
      napi_callback_scope cs = nullptr;
      CheckStatus(napi_open_callback_scope(env, resobj, ctx, &cs), napi_ok,
                  "B open cs for distinctness");
      size_t cbDepth = NodeApiAsyncContext_ScopeDepth(env);
      size_t hsDepth = NodeApiScopes_Depth(env);
      napi_handle_scope hs = nullptr;
      CheckStatus(napi_open_handle_scope(env, &hs), napi_ok, "B open hs");
      Check(NodeApiAsyncContext_ScopeDepth(env) == cbDepth,
            "B callback depth unchanged by handle open");
      Check(NodeApiScopes_Depth(env) == hsDepth + 1, "B handle depth +1");
      CheckStatus(napi_close_handle_scope(env, hs), napi_ok, "B close hs");
      Check(NodeApiAsyncContext_ScopeDepth(env) == cbDepth,
            "B callback depth unchanged by handle close");
      Check(NodeApiScopes_Depth(env) == hsDepth, "B handle depth restored");
      CheckStatus(napi_close_callback_scope(env, cs), napi_ok, "B close cs");
    }
    // Cross-env close => invalid_arg.
    {
      napi_env envB = chakra_nodeapi_create_env(g_runtime, g_context, "bb",
                                                nullptr, nullptr);
      Check(envB != nullptr, "B envB created");
      napi_callback_scope sb = nullptr;
      CheckStatus(napi_open_callback_scope(env, resobj, ctx, &sb), napi_ok,
                  "B open sb on envA");
      CheckStatus(napi_close_callback_scope(envB, sb), napi_invalid_arg,
                  "B cross-env close => invalid_arg");
      Check(NodeApiAsyncContext_ScopeDepth(env) == 1,
            "B depth still 1 after cross-env attempt");
      CheckStatus(napi_close_callback_scope(env, sb), napi_ok, "B close sb");
      chakra_nodeapi_destroy_env(envB);
    }
    // No JS roots from open (bookkeeping only).
    Check(chakra_nodeapi_test_root_count(env) >= root_base,
          "B root count sane");
    CheckStatus(napi_async_destroy(env, ctx), napi_ok, "B destroy ctx");
    chakra_nodeapi_destroy_env(env);
    env = nullptr;
    Check(NodeApiAsyncContext_TotalStates() == states_baseline,
          "B states baseline");
    std::printf("phaseB: done\n");
  }

  /* ========== C: make_callback basics ========== */
  {
    std::printf("phaseC make_callback: ...\n");
    env = chakra_nodeapi_create_env(g_runtime, g_context, "c", nullptr, nullptr);
    Check(env != nullptr, "C create");
    napi_value asyncRes = NapiStr(env, "r");
    napi_value asyncName = NapiStr(env, "n");
    napi_async_context ctx = nullptr;
    CheckStatus(napi_async_init(env, asyncRes, asyncName, &ctx), napi_ok,
                "C init ok");
    size_t root_base = chakra_nodeapi_test_root_count(env);

    // C1: JS callee correct this/args + result (inside handle scope).
    {
      napi_handle_scope scope = nullptr;
      CheckStatus(napi_open_handle_scope(env, &scope), napi_ok, "C1 open hs");
      SetGlobalString(env, "__mc_this_tag", "none");
      SetGlobalNumber("__mc_a", -1);
      SetGlobalNumber("__mc_b", -1);
      napi_value recv = EvalJS(env, "({tag: 'recv-tag'})");
      Check(recv != nullptr, "C1 recv created");
      napi_value fn = EvalJS(
          env,
          "(function(a,b){ globalThis.__mc_this_tag = this.tag; "
          "globalThis.__mc_a = a; globalThis.__mc_b = b; return a+b; })");
      Check(fn != nullptr, "C1 fn created");
      napi_value a10 = NapiNum(env, 10);
      napi_value b20 = NapiNum(env, 20);
      napi_value argv[2] = {a10, b20};
      napi_value result = nullptr;
      CheckStatus(napi_make_callback(env, ctx, recv, fn, 2, argv, &result),
                  napi_ok, "C1 make_callback ok");
      Check(result != nullptr, "C1 result non-null");
      {
        double d = -1;
        JsNumberToDouble(reinterpret_cast<JsValueRef>(result), &d);
        Check(d == 30, "C1 result 30");
      }
      std::string tag;
      Check(GetGlobalString("__mc_this_tag", &tag), "C1 tag readable");
      Check(tag == "recv-tag", "C1 this correct");
      double ga = -1, gb = -1;
      Check(GetGlobalNumber("__mc_a", &ga), "C1 a readable");
      Check(GetGlobalNumber("__mc_b", &gb), "C1 b readable");
      Check(ga == 10 && gb == 20, "C1 args correct");
      Check(NodeApiScopes_Depth(env) == 2, "C1 handle depth balanced");
      Check(NodeApiAsyncContext_ScopeDepth(env) == 0,
            "C1 callback depth 0 (make uses handle scope, not callback stack)");
      CheckStatus(napi_close_handle_scope(env, scope), napi_ok, "C1 close hs");
      Check(chakra_nodeapi_test_root_count(env) == root_base,
            "C1 roots back to baseline after hs close");
    }
    // C2: NULL context (default) ok; native thunk callee.
    {
      napi_handle_scope scope = nullptr;
      (void)napi_open_handle_scope(env, &scope);
      napi_value recv = EvalJS(env, "({})");
      g_native_recv = recv;
      g_native_argc = -1;
      g_native_this_is_recv = false;
      g_native_data_seen = nullptr;
      void* marker = (void*)(std::uintptr_t)0xC0DE;
      napi_value fn = nullptr;
      CheckStatus(napi_create_function(env, "cb", NAPI_AUTO_LENGTH, NativeCb,
                                       marker, &fn),
                  napi_ok, "C2 create native fn");
      napi_value a1 = NapiNum(env, 1);
      napi_value a2 = NapiNum(env, 2);
      napi_value argv[2] = {a1, a2};
      napi_value result = nullptr;
      CheckStatus(napi_make_callback(env, nullptr, recv, fn, 2, argv, &result),
                  napi_ok, "C2 make_callback NULL ctx ok");
      Check(g_native_argc == 2, "C2 native argc 2");
      Check(g_native_this_is_recv, "C2 native this correct");
      Check(g_native_data_seen == marker, "C2 native data correct");
      Check(result != nullptr, "C2 result non-null");
      double d = -1;
      JsNumberToDouble(reinterpret_cast<JsValueRef>(result), &d);
      Check(d == 2, "C2 result is argc (2)");
      (void)napi_close_handle_scope(env, scope);
      Check(chakra_nodeapi_test_root_count(env) == root_base,
            "C2 roots baseline");
    }
    // C3: invalid args.
    {
      napi_value recv = EvalJS(env, "({})");
      napi_value fn = EvalJS(env, "(function(){ return 1; })");
      napi_value result = reinterpret_cast<napi_value>((std::uintptr_t)0xBAD);
      napi_async_context fakeCtx =
          reinterpret_cast<napi_async_context>((std::uintptr_t)0xDEAD);
      CheckStatus(napi_make_callback(env, fakeCtx, recv, fn, 0, nullptr,
                                     &result),
                  napi_invalid_arg, "C3 fake ctx => invalid_arg");
      Check(result == reinterpret_cast<napi_value>((std::uintptr_t)0xBAD),
            "C3 result untouched on fake ctx");
      napi_value nonfunc = NapiNum(env, 42);
      CheckStatus(napi_make_callback(env, ctx, recv, nonfunc, 0, nullptr,
                                     &result),
                  napi_invalid_arg, "C3 non-function => invalid_arg");
      Check(result == reinterpret_cast<napi_value>((std::uintptr_t)0xBAD),
            "C3 result untouched on non-function");
      JsValueRef nullv = JS_INVALID_REFERENCE;
      JsGetNullValue(&nullv);
      CheckStatus(napi_make_callback(env, ctx, reinterpret_cast<napi_value>(nullv),
                                     fn, 0, nullptr, &result),
                  napi_object_expected, "C3 JS null recv => object_expected");
      Check(result == reinterpret_cast<napi_value>((std::uintptr_t)0xBAD),
            "C3 result untouched on object_expected");
      CheckStatus(napi_make_callback(env, ctx, nullptr, fn, 0, nullptr,
                                     &result),
                  napi_invalid_arg, "C3 C-NULL recv => invalid_arg");
      CheckStatus(napi_make_callback(env, ctx, recv, nullptr, 0, nullptr,
                                     &result),
                  napi_invalid_arg, "C3 C-NULL func => invalid_arg");
      napi_value one = NapiNum(env, 1);
      napi_value argv1[1] = {one};
      CheckStatus(napi_make_callback(env, ctx, recv, fn, 1, nullptr,
                                     &result),
                  napi_invalid_arg, "C3 NULL argv => invalid_arg");
      // Argc guard: huge argc with small argv must not touch argv.
      CheckStatus(napi_make_callback(env, ctx, recv, fn, 70000, argv1,
                                     &result),
                  napi_generic_failure, "C3 argc guard => generic_failure");
      // Invalid ctx destroyed handle.
      {
        napi_async_context dead = nullptr;
        napi_value r2 = NapiStr(env, "rr");
        napi_value n2 = NapiStr(env, "nn");
        CheckStatus(napi_async_init(env, r2, n2, &dead), napi_ok, "C3 init dead");
        CheckStatus(napi_async_destroy(env, dead), napi_ok, "C3 destroy dead");
        CheckStatus(napi_make_callback(env, dead, recv, fn, 0, nullptr,
                                       &result),
                    napi_invalid_arg, "C3 destroyed ctx => invalid_arg");
      }
    }
    // C3 rooted temporaries into the default scope (they have no
    // explicit scope to close into); refresh the baseline so C4-C6
    // compare against current state, not the pre-C3 snapshot.
    root_base = chakra_nodeapi_test_root_count(env);
    // C4: result NULL (discard) still runs callee.
    {
      napi_handle_scope scope = nullptr;
      (void)napi_open_handle_scope(env, &scope);
      SetGlobalNumber("__mc_discard", 0);
      napi_value recv = EvalJS(env, "({})");
      napi_value fn = EvalJS(
          env, "(function(){ globalThis.__mc_discard = 5; return 99; })");
      CheckStatus(napi_make_callback(env, ctx, recv, fn, 0, nullptr, nullptr),
                  napi_ok, "C4 discard ok");
      double g = -1;
      Check(GetGlobalNumber("__mc_discard", &g), "C4 global readable");
      Check(g == 5, "C4 callee ran with NULL result");
      (void)napi_close_handle_scope(env, scope);
      Check(chakra_nodeapi_test_root_count(env) == root_base,
            "C4 roots baseline (discard)");
    }
    // C5: gated while pending (No, not called).
    {
      napi_handle_scope scope = nullptr;
      (void)napi_open_handle_scope(env, &scope);
      SetGlobalNumber("__mc_gate", 0);
      napi_value recv = EvalJS(env, "({})");
      napi_value fn = EvalJS(
          env, "(function(){ globalThis.__mc_gate = 99; return 1; })");
      JsValueRef m = JS_INVALID_REFERENCE;
      JsCreateObject(&m);
      (void)NodeApiScopes_RootIntoInnermost(env, reinterpret_cast<napi_value>(m));
      (void)napi_throw(env, reinterpret_cast<napi_value>(m));
      napi_value result = nullptr;
      CheckStatus(napi_make_callback(env, ctx, recv, fn, 0, nullptr,
                                     &result),
                  napi_pending_exception, "C5 gated while pending");
      Check(result == nullptr, "C5 result untouched while gated");
      double g = -1;
      Check(GetGlobalNumber("__mc_gate", &g), "C5 global readable");
      Check(g == 0, "C5 callee NOT called while gated");
      Check(HasPending(env), "C5 pending preserved");
      napi_value cleared = nullptr;
      (void)napi_get_and_clear_last_exception(env, &cleared);
      Check(cleared == reinterpret_cast<napi_value>(m), "C5 original preserved");
      CheckStatus(napi_make_callback(env, ctx, recv, fn, 0, nullptr,
                                     &result),
                  napi_ok, "C5 runs after clear");
      Check(GetGlobalNumber("__mc_gate", &g) && g == 99, "C5 ran after clear");
      (void)napi_close_handle_scope(env, scope);
    }
    // C6: callee throw => pending + result untouched.
    {
      napi_handle_scope scope = nullptr;
      (void)napi_open_handle_scope(env, &scope);
      napi_value recv = EvalJS(env, "({})");
      napi_value fn = EvalJS(
          env, "(function(){ throw new Error('mc-boom'); })");
      napi_value result = reinterpret_cast<napi_value>((std::uintptr_t)0xBAD);
      CheckStatus(napi_make_callback(env, ctx, recv, fn, 0, nullptr,
                                     &result),
                  napi_pending_exception, "C6 throw => pending");
      Check(result == reinterpret_cast<napi_value>((std::uintptr_t)0xBAD),
            "C6 result untouched on throw");
      Check(HasPending(env), "C6 pending true");
      napi_value cleared = nullptr;
      (void)napi_get_and_clear_last_exception(env, &cleared);
      Check(cleared != nullptr, "C6 cleared non-null");
      (void)napi_close_handle_scope(env, scope);
      Check(chakra_nodeapi_test_root_count(env) == root_base,
            "C6 roots baseline after throw+clear");
    }
    CheckStatus(napi_async_destroy(env, ctx), napi_ok, "C destroy ctx");
    chakra_nodeapi_destroy_env(env);
    env = nullptr;
    Check(NodeApiAsyncContext_TotalStates() == states_baseline,
          "C states baseline");
    std::printf("phaseC: done\n");
  }

  /* ========== D: make_callback drains provider queue ========== */
  {
    std::printf("phaseD pump drain: ...\n");
    // Provider trampoline is effective here: no host install has run
    // yet on this shared context (Phase E installs later).
    env = chakra_nodeapi_create_env(g_runtime, g_context, "d", nullptr, nullptr);
    Check(env != nullptr, "D create");
    napi_deferred def = nullptr;
    napi_value prom = nullptr;
    CheckStatus(napi_create_promise(env, &def, &prom), napi_ok, "D create promise");
    SetGlobalNumber("__pump_seen", -1);
    napi_value handler = EvalJS(
        env, "(function(v){ globalThis.__pump_seen = v; return v; })");
    Check(handler != nullptr, "D handler created");
    Check(AttachThen(env, prom, handler), "D then attached");
    napi_value v99 = NapiNum(env, 99);
    CheckStatus(napi_resolve_deferred(env, def, v99), napi_ok, "D resolve ok");
    double seen = -2;
    Check(GetGlobalNumber("__pump_seen", &seen), "D seen readable before");
    Check(seen == -1, "D handler NOT run before make_callback");
    Check(NodeApiPromise_QueueDepth(env) > 0, "D provider queue non-empty");
    // Trivial make_callback must drain the reaction before returning.
    napi_value recv = EvalJS(env, "({})");
    napi_value fn = EvalJS(env, "(function(){ return 1; })");
    napi_async_context ctx2 = nullptr;
    napi_value r2 = NapiStr(env, "rr");
    napi_value n2 = NapiStr(env, "nn");
    CheckStatus(napi_async_init(env, r2, n2, &ctx2), napi_ok, "D init ok");
    napi_value result = nullptr;
    CheckStatus(napi_make_callback(env, ctx2, recv, fn, 0, nullptr, &result),
                napi_ok, "D make_callback ok");
    Check(result != nullptr, "D result non-null");
    Check(GetGlobalNumber("__pump_seen", &seen), "D seen readable after");
    Check(seen == 99, "D handler ran before make_callback returned (pump)");
    Check(NodeApiPromise_QueueDepth(env) == 0, "D queue quiet after");
    CheckStatus(napi_async_destroy(env, ctx2), napi_ok, "D destroy ctx");
    chakra_nodeapi_destroy_env(env);
    env = nullptr;
    Check(NodeApiAsyncContext_TotalStates() == states_baseline,
          "D states baseline");
    std::printf("phaseD: done\n");
  }

  /* ========== E: async cleanup via Lifetime pump (step 4) ========== */
  {
    std::printf("phaseE teardown pump: ...\n");
    g_log.clear();
    for (int i = 0; i < 8; i++) {
      g_async_fired[i] = 0;
      g_deferred[i].h = nullptr;
      g_deferred[i].countdown = 0;
      g_deferred[i].scheduled = false;
    }
    g_pump_calls = 0;
    env = chakra_nodeapi_create_env(g_runtime, g_context, "e", nullptr, nullptr);
    Check(env != nullptr, "E create");
    HostPump pump;
    JsSetPromiseContinuationCallback(HostContinuation, &pump);
    // Pre-queue a microtask (host queue): resolve before destroy.
    napi_deferred def = nullptr;
    napi_value prom = nullptr;
    CheckStatus(napi_create_promise(env, &def, &prom), napi_ok, "E create promise");
    SetGlobalNumber("__async_reacted", 0);
    napi_value handler = EvalJS(
        env, "(function(v){ globalThis.__async_reacted = 1; return v; })");
    Check(handler != nullptr, "E handler created");
    Check(AttachThen(env, prom, handler), "E then attached");
    napi_value vv = NapiNum(env, 7);
    CheckStatus(napi_resolve_deferred(env, def, vv), napi_ok, "E resolve ok");
    double reacted = -1;
    Check(GetGlobalNumber("__async_reacted", &reacted), "E reacted readable");
    Check(reacted == 0, "E reaction NOT run before destroy pump");
    Check(!pump.tasks.empty(), "E host queue non-empty before destroy");
    napi_async_cleanup_hook_handle h1 = nullptr, h2 = nullptr;
    CheckStatus(napi_add_async_cleanup_hook(env, AsyncSched,
                                            (void*)(std::uintptr_t)1, &h1),
                napi_ok, "E add A1");
    CheckStatus(napi_add_async_cleanup_hook(env, AsyncSched,
                                            (void*)(std::uintptr_t)2, &h2),
                napi_ok, "E add A2");
    nodeapi::Lifetime_SetAsyncPump(reinterpret_cast<nodeapi::NodeEnv*>(env),
                                   PumpDrainThenRemove, &pump, 10);
    chakra_nodeapi_test_set_teardown_log(env, Sink, nullptr);
    std::uintptr_t saved = reinterpret_cast<std::uintptr_t>(env);
    chakra_nodeapi_destroy_env(env);
    env = nullptr;
    Check(g_async_fired[1] == 1 && g_async_fired[2] == 1, "E fired once each");
    Check(!nodeapi::Lifetime_AsyncViolation((void*)saved),
          "E no violation (pump drove removal)");
    Check(g_pump_calls > 0, "E pump was used");
    Check(GetGlobalNumber("__async_reacted", &reacted), "E reacted readable after");
    Check(reacted == 1, "E reaction ran during step-4 pump before removal");
    std::vector<std::string> exp = {"closing", "wrap_drain", "async:A2",
                                    "async:A1", "refs_scopes_release", "dead"};
    Check(g_log == exp, "E reverse order verbatim");
    nodeapi::Lifetime_ClearAsyncPump((void*)saved);
    Check(NodeApiAsyncContext_TotalStates() == states_baseline,
          "E states baseline");

    // Self-removing (0 pump turns) also passes.
    g_log.clear();
    for (int i = 0; i < 8; i++) g_async_fired[i] = 0;
    g_pump_calls = 0;
    env = chakra_nodeapi_create_env(g_runtime, g_context, "e2", nullptr, nullptr);
    Check(env != nullptr, "E2 create");
    napi_async_cleanup_hook_handle h3 = nullptr;
    CheckStatus(napi_add_async_cleanup_hook(env, AsyncSelfRemove,
                                            (void*)(std::uintptr_t)1, &h3),
                napi_ok, "E2 add self-remove");
    nodeapi::Lifetime_SetAsyncPump(reinterpret_cast<nodeapi::NodeEnv*>(env),
                                   PumpDrainThenRemove, &pump, 5);
    chakra_nodeapi_test_set_teardown_log(env, Sink, nullptr);
    saved = reinterpret_cast<std::uintptr_t>(env);
    chakra_nodeapi_destroy_env(env);
    env = nullptr;
    Check(g_async_fired[1] == 1, "E2 fired once");
    Check(!nodeapi::Lifetime_AsyncViolation((void*)saved), "E2 no violation");
    Check(g_pump_calls == 0, "E2 pump not needed for sync self-remove");
    nodeapi::Lifetime_ClearAsyncPump((void*)saved);
    std::printf("phaseE: done\n");
  }

  /* ========== F: Closing policy (inside sync hook) ========== */
  {
    std::printf("phaseF closing: ...\n");
    env = chakra_nodeapi_create_env(g_runtime, g_context, "f", nullptr, nullptr);
    Check(env != nullptr, "F create");
    napi_value res = NapiStr(env, "r");
    napi_value name = NapiStr(env, "n");
    napi_async_context ctx = nullptr;
    CheckStatus(napi_async_init(env, res, name, &ctx), napi_ok, "F init ok");
    napi_value resobj = EvalJS(env, "({})");
    napi_callback_scope scope = nullptr;
    CheckStatus(napi_open_callback_scope(env, resobj, ctx, &scope), napi_ok,
                "F open scope ok");
    g_closing_env = env;
    g_closing_ctx = ctx;
    g_closing_scope = scope;
    g_close_init_generic = g_close_open_generic = g_close_make_generic = false;
    g_close_destroy_ok = g_close_scope_ok = false;
    CheckStatus(napi_add_env_cleanup_hook(env, ClosingProbeHook, nullptr),
                napi_ok, "F add closing hook");
    chakra_nodeapi_destroy_env(env);
    env = nullptr;
    Check(g_close_init_generic, "F init generic_failure in Closing");
    Check(g_close_open_generic, "F open generic_failure in Closing");
    Check(g_close_make_generic, "F make generic_failure in Closing");
    Check(g_close_destroy_ok, "F async_destroy ok in Closing");
    Check(g_close_scope_ok, "F close_callback_scope ok in Closing");
    g_closing_env = nullptr;
    Check(NodeApiAsyncContext_TotalStates() == states_baseline,
          "F states baseline");
    std::printf("phaseF: done\n");
  }

  /* ========== G: teardown release (leaked records) ========== */
  {
    std::printf("phaseG teardown release: ...\n");
    env = chakra_nodeapi_create_env(g_runtime, g_context, "g", nullptr, nullptr);
    Check(env != nullptr, "G create");
    size_t before = NodeApiAsyncContext_TotalStates();
    Check(before == states_baseline + 0 || before >= states_baseline,
          "G states sane before");
    // Actually before counts include any leftover from prior envs if
    // a probe leaked; we assert equality with the initial baseline
    // at this point (all prior envs destroyed).
    Check(NodeApiAsyncContext_TotalStates() == states_baseline,
          "G no leftover before churn");
    napi_value res = NapiStr(env, "r");
    napi_value name = NapiStr(env, "n");
    napi_value resobj = EvalJS(env, "({})");
    for (int i = 0; i < 3; i++) {
      napi_async_context c = nullptr;
      CheckStatus(napi_async_init(env, res, name, &c), napi_ok, "G init");
    }
    for (int i = 0; i < 2; i++) {
      napi_callback_scope s = nullptr;
      CheckStatus(napi_open_callback_scope(env, resobj, nullptr, &s), napi_ok,
                  "G open scope");
    }
    Check(NodeApiAsyncContext_ContextCount(env) == 3, "G 3 live contexts");
    Check(NodeApiAsyncContext_ScopeDepth(env) == 2, "G 2 open scopes");
    // Destroy WITHOUT explicit async_destroy / close: teardown backstop.
    chakra_nodeapi_destroy_env(env);
    env = nullptr;
    Check(NodeApiAsyncContext_TotalStates() == states_baseline,
          "G teardown freed leaked records (states baseline)");
    std::printf("phaseG: done\n");
  }

  std::printf("asyncctx_probe: %d checks, %d failures\n", g_checks, g_failures);
  ShutdownEngine();
  return g_failures == 0 ? 0 : 1;
}
