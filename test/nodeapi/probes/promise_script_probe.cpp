// promise_script_probe.cpp — N3.4 promises + N3.5 script/version/memory
// (plan doc/node-api-plan.md cards N3.4/N3.5, A6 + B7).
//
// Covers (each MUST fail probe on mismatch):
//   Promise (N3.4 Done-when):
//     R1. Host-queue resolve: napi_create_promise -> JS attaches
//         .then -> napi_resolve_deferred -> host pump (JsCallFunction
//         until quiet) -> .then handler ran with the resolved value.
//         Before the pump the handler has NOT run (queue depth >0).
//     R2. Host-queue reject: napi_reject_deferred -> .catch ran with
//         the rejected value; unhandled rejection does not crash.
//     R3. Provider-queue resolve (no host install): create+resolve
//         with the provider trampoline as the effective callback,
//         drained via NodeApiPromise_Pump -> handler ran. Proves the
//         provider-alone path when the host never installs.
//     R4. Double-settle no-op: resolve then resolve/reject -> second
//         returns napi_ok, first value wins (JS-level no-op). Also
//         reject-then-resolve in the other order.
//     R5. is_promise: true for provider promise + JS Promise.race
//         result; false (napi_ok) for object/number/string/null/
//         undefined/function/array. NULL args => invalid_arg.
//     R6. Deferred-after-destroy: create, destroy env, then resolve
//         with the dead env + saved deferred => napi_invalid_arg
//         (clean failure, no UAF, no crash). Cross-env (deferred
//         from env A, resolve on env B) => napi_invalid_arg.
//     R7. Thenable assimilation rejects: resolve with a thenable
//         whose `then` getter throws => napi_ok (rejects, not throws),
//         handler observes TypeError after pump, promise rejected,
//         second settle ok (engine truth; the throw is queued, not
//         synchronous).
//     R8. Invalid args: NULL deferred/promise out, NULL
//         resolution/rejection => invalid_arg, output untouched.
//   Script (N3.5 Done-when):
//     S1. RunScript value: "1+2" => 3; sees globals (globalThis
//         probe var set via provider create/set, read by script);
//         returns completion value of last statement.
//     S2. Syntax error: "var x = ;" => napi_pending_exception,
//         pending true, get_and_clear name == SyntaxError.
//     S3. Runtime error class: "throw new TypeError('boom')" =>
//         pending, name == TypeError, message preserved; RangeError
//         similarly.
//     S4. Non-string input (object/number) => napi_string_expected,
//         no pending fabricated. Pending-gate: throw then
//         run_script => napi_pending_exception without running
//         (global unchanged).
//   Version/memory (N3.5 Done-when):
//     V1. napi_get_version => 8; NULL result => invalid_arg.
//     V2. napi_get_node_version => {1,0,0,"libChakraNodeApi/1.0.0"}
//         (provider truth per B7, already landed Wave 1 -- shape
//         verification only, no duplicate implementation).
//     M1. napi_adjust_external_memory accumulates: +100=>100,
//         +200=>300, -50=>250, +100=>350; NULL adjusted => invalid_arg
//         with no mutation; per-env (second env starts at 0).
//
// Build (repo root, via gate lock — see run_promise_script_probe.sh):
//   /tmp/opencode/cc-gate-lock.sh mise exec -- zig c++ \
//     -std=gnu++17 -DNAPI_VERSION=8 -DCHAKRA_NODEAPI_TEST_HOOKS \
//     -Iinclude/nodeapi -Ilib/Jsrt -Ilib/NodeApi \
//     test/nodeapi/probes/promise_script_probe.cpp \
//     lib/NodeApi/NodeApiEnv.cpp lib/NodeApi/NodeApiLifetime.cpp \
//     lib/NodeApi/NodeApiScopes.cpp lib/NodeApi/NodeApiRefs.cpp \
//     lib/NodeApi/NodeApiValues.cpp lib/NodeApi/NodeApiErrorsImpl.cpp \
//     lib/NodeApi/NodeApiObjects.cpp lib/NodeApi/NodeApiFunctions.cpp \
//     lib/NodeApi/NodeApiPromise.cpp lib/NodeApi/NodeApiScript.cpp \
//     -o /tmp/promise_script_probe -Lzig-out/lib -lChakraCore ...
// Exit 0 = green; 1 = any mismatch.

#include "NodeApiHostApi.h"
#include "js_native_api.h"
#include "node_api.h"
#include "NodeApiEnv.h"
#include "NodeApiPromise.h"
#include "NodeApiScript.h"
#include "NodeApiScopes.h"
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

/* --- Host pump queue (mirrors ch-nodeapi.cpp PR-8 host pump) --- */

struct HostPump {
  std::vector<JsValueRef> tasks;
};

void HostContinuation(JsValueRef task, void* state) {
  auto* pump = static_cast<HostPump*>(state);
  if (JsAddRef(task, nullptr) == JsNoError) {
    pump->tasks.push_back(task);
  }
}

/* Drain host queue via JsCallFunction until quiet. Returns napi_ok on
 * quiet, napi_pending_exception/generic on task error. */
napi_status PumpHost(HostPump* pump, size_t limit = 10000) {
  size_t iter = 0;
  while (!pump->tasks.empty()) {
    if (++iter > limit) {
      std::printf("FAIL: host pump exceeded limit %zu tasks\n", limit);
      return napi_generic_failure;
    }
    JsValueRef task = pump->tasks.front();
    pump->tasks.erase(pump->tasks.begin());
    JsValueRef undef = JS_INVALID_REFERENCE;
    JsGetUndefinedValue(&undef);
    JsValueRef arr[1] = {undef};
    JsValueRef res = JS_INVALID_REFERENCE;
    JsErrorCode ec = JsCallFunction(task, arr, 1, &res);
    JsRelease(task, nullptr);
    if (ec != JsNoError) {
      bool has = false;
      if (JsHasException(&has) == JsNoError && has) {
        return napi_pending_exception;
      }
      return napi_generic_failure;
    }
  }
  return napi_ok;
}

/* --- Js helpers --- */

napi_value NapiStr(napi_env env, const char* s) {
  JsValueRef v = JS_INVALID_REFERENCE;
  JsCreateString(s, std::strlen(s), &v);
  (void)NodeApiScopes_RootIntoInnermost(env, reinterpret_cast<napi_value>(v));
  return reinterpret_cast<napi_value>(v);
}

bool HasPending(napi_env env) {
  bool has = false;
  if (napi_is_exception_pending(env, &has) != napi_ok) return false;
  return has;
}

std::string GetStrProp(napi_value obj, const char* prop) {
  JsPropertyIdRef pid = JS_INVALID_REFERENCE;
  if (JsCreatePropertyId(prop, std::strlen(prop), &pid) != JsNoError)
    return "<propid-fail>";
  JsValueRef v = JS_INVALID_REFERENCE;
  if (JsGetProperty(reinterpret_cast<JsValueRef>(obj), pid, &v) != JsNoError)
    return "<get-fail>";
  JsValueType t = JsObject;
  if (JsGetValueType(v, &t) != JsNoError) return "<type-fail>";
  if (t == JsUndefined) return "<undefined>";
  if (t == JsNull) return "<null>";
  if (t != JsString) return "<non-string>";
  size_t len = 0;
  JsCopyString(v, nullptr, 0, &len);
  std::string s(len + 1, '\0');
  size_t written = 0;
  if (JsCopyString(v, &s[0], s.size(), &written) != JsNoError)
    return "<copy-fail>";
  s.resize(written);
  return s;
}

/* Read a global's numeric value via JsGetProperty(*). Returns false if
 * missing/non-number. */
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

void SetGlobalNumber(napi_env env, const char* name, double val) {
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

/* Evaluate JS code (host-side, not via napi_run_script). Returns result
 * rooted, or nullptr on failure (exception stays pending). */
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

JsErrorCode RunJSMaybe(napi_env env, const char* code, JsValueRef* out) {
  (void)env;
  JsValueRef src = JS_INVALID_REFERENCE;
  JsValueRef url = JS_INVALID_REFERENCE;
  JsValueRef res = JS_INVALID_REFERENCE;
  JsCreateString(code, std::strlen(code), &src);
  JsCreateString("probe.js", std::strlen("probe.js"), &url);
  JsErrorCode ec = JsRun(src, JS_SOURCE_CONTEXT_NONE, url,
                         JsParseScriptAttributeNone, &res);
  if (out) *out = res;
  JsRelease(src, nullptr);
  JsRelease(url, nullptr);
  if (ec != JsNoError) {
    if (res != JS_INVALID_REFERENCE) JsRelease(res, nullptr);
    if (out) *out = JS_INVALID_REFERENCE;
    return ec;
  }
  return JsNoError;
}

} /* namespace */

int main() {
  if (!InitEngine()) return 2;
  napi_env env = nullptr;

  /* ========== R1: host-queue resolve ========== */
  {
    std::printf("phaseR1 host-queue resolve: ...\n");
    env = chakra_nodeapi_create_env(g_runtime, g_context, "r1", nullptr, nullptr);
    Check(env != nullptr, "R1 create env");
    /* Host installs AFTER env create (last-writer-wins contract). */
    HostPump pump;
    JsSetPromiseContinuationCallback(HostContinuation, &pump);
    napi_deferred def = nullptr;
    napi_value prom = nullptr;
    CheckStatus(napi_create_promise(env, &def, &prom), napi_ok, "R1 create ok");
    Check(def != nullptr && prom != nullptr, "R1 non-null");
    bool is_p = false;
    CheckStatus(napi_is_promise(env, prom, &is_p), napi_ok, "R1 is_promise ok");
    Check(is_p, "R1 is_promise true");
    Check(NodeApiPromise_DeferredCount(env) == 1, "R1 deferred count 1");
    /* Attach .then that records the value into a global. */
    SetGlobalNumber(env, "__r1_seen", -1);
    napi_value then_fn = nullptr;
    {
      JsPropertyIdRef pid = JS_INVALID_REFERENCE;
      JsCreatePropertyId("then", 4, &pid);
      JsValueRef f = JS_INVALID_REFERENCE;
      JsGetProperty(reinterpret_cast<JsValueRef>(prom), pid, &f);
      (void)NodeApiScopes_RootIntoInnermost(env, reinterpret_cast<napi_value>(f));
      then_fn = reinterpret_cast<napi_value>(f);
    }
    napi_value handler = EvalJS(
        env,
        "(function(v){ globalThis.__r1_seen = v; return v; })");
    Check(handler != nullptr, "R1 handler created");
    {
      /* Call then(handler) with this=promise. */
      JsValueRef fn = reinterpret_cast<JsValueRef>(then_fn);
      JsValueRef args[2] = {reinterpret_cast<JsValueRef>(prom),
                            reinterpret_cast<JsValueRef>(handler)};
      JsValueRef res = JS_INVALID_REFERENCE;
      JsErrorCode ec = JsCallFunction(fn, args, 2, &res);
      Check(ec == JsNoError, "R1 then call ok");
      (void)res;
    }
    /* Resolve with 42 (host number via provider create would also
     * work; use a rooted double directly for clarity). */
    JsValueRef num = JS_INVALID_REFERENCE;
    JsDoubleToNumber(42.0, &num);
    (void)NodeApiScopes_RootIntoInnermost(env, reinterpret_cast<napi_value>(num));
    CheckStatus(napi_resolve_deferred(env, def, reinterpret_cast<napi_value>(num)),
                napi_ok, "R1 resolve ok");
    /* Handler must NOT have run yet (needs pump). */
    double seen = -2;
    Check(GetGlobalNumber("__r1_seen", &seen), "R1 seen readable");
    Check(seen == -1, "R1 handler not yet run before pump");
    CheckStatus(PumpHost(&pump), napi_ok, "R1 pump ok");
    Check(GetGlobalNumber("__r1_seen", &seen), "R1 seen readable after pump");
    Check(seen == 42, "R1 handler saw 42 after pump");
    /* Promise state is fulfilled. */
    JsPromiseState ps = JsPromiseStatePending;
    Check(JsGetPromiseState(reinterpret_cast<JsValueRef>(prom), &ps) == JsNoError,
          "R1 state query ok");
    Check(ps == JsPromiseStateFulfilled, "R1 fulfilled");
    chakra_nodeapi_destroy_env(env);
    std::printf("phaseR1: done\n");
  }

  /* ========== R2: host-queue reject ========== */
  {
    std::printf("phaseR2 host-queue reject: ...\n");
    env = chakra_nodeapi_create_env(g_runtime, g_context, "r2", nullptr, nullptr);
    Check(env != nullptr, "R2 create env");
    HostPump pump;
    JsSetPromiseContinuationCallback(HostContinuation, &pump);
    napi_deferred def = nullptr;
    napi_value prom = nullptr;
    CheckStatus(napi_create_promise(env, &def, &prom), napi_ok, "R2 create ok");
    SetGlobalString(env, "__r2_seen", "none");
    /* Attach .catch that records the reason. */
    napi_value catch_fn = nullptr;
    {
      JsPropertyIdRef pid = JS_INVALID_REFERENCE;
      JsCreatePropertyId("catch", 5, &pid);
      JsValueRef f = JS_INVALID_REFERENCE;
      JsGetProperty(reinterpret_cast<JsValueRef>(prom), pid, &f);
      (void)NodeApiScopes_RootIntoInnermost(env, reinterpret_cast<napi_value>(f));
      catch_fn = reinterpret_cast<napi_value>(f);
    }
    napi_value handler = EvalJS(
        env,
        "(function(e){ globalThis.__r2_seen = String(e); return e; })");
    Check(handler != nullptr, "R2 handler created");
    {
      JsValueRef fn = reinterpret_cast<JsValueRef>(catch_fn);
      JsValueRef args[2] = {reinterpret_cast<JsValueRef>(prom),
                            reinterpret_cast<JsValueRef>(handler)};
      JsValueRef res = JS_INVALID_REFERENCE;
      Check(JsCallFunction(fn, args, 2, &res) == JsNoError, "R2 catch call ok");
      (void)res;
    }
    napi_value reason = NapiStr(env, "r2-boom");
    CheckStatus(napi_reject_deferred(env, def, reason), napi_ok, "R2 reject ok");
    std::string seen;
    Check(GetGlobalString("__r2_seen", &seen), "R2 seen readable");
    Check(seen == "none", "R2 handler not yet run before pump");
    CheckStatus(PumpHost(&pump), napi_ok, "R2 pump ok");
    Check(GetGlobalString("__r2_seen", &seen), "R2 seen readable after");
    Check(seen == "r2-boom", "R2 handler saw reason after pump");
    JsPromiseState ps = JsPromiseStatePending;
    Check(JsGetPromiseState(reinterpret_cast<JsValueRef>(prom), &ps) == JsNoError,
          "R2 state query ok");
    Check(ps == JsPromiseStateRejected, "R2 rejected");
    chakra_nodeapi_destroy_env(env);
    std::printf("phaseR2: done\n");
  }

  /* ========== R3: provider-queue resolve (no host install) ========== */
  {
    std::printf("phaseR3 provider-queue resolve: ...\n");
    env = chakra_nodeapi_create_env(g_runtime, g_context, "r3", nullptr, nullptr);
    Check(env != nullptr, "R3 create env");
    /* Do NOT install host callback: the provider's trampoline (from
     * Ensure at create) is the effective callback. But earlier phases
     * installed host callbacks on the same context (same runtime+context
     * across phases!) -- the last host install wins and still points
     * at a dead HostPump. Re-installing the provider trampoline is
     * impossible without a public API, so this phase creates a FRESH
     * context with no prior host install, where the provider's install
     * is the only one. Fresh runtime+context keeps this phase
     * isolated. */
    {
      JsRuntimeHandle rt2 = JS_INVALID_RUNTIME_HANDLE;
      JsContextRef ctx2 = JS_INVALID_REFERENCE;
      Check(JsCreateRuntime(JsRuntimeAttributeNone, nullptr, &rt2) == JsNoError,
            "R3 rt2 created");
      Check(JsCreateContext(rt2, &ctx2) == JsNoError, "R3 ctx2 created");
      Check(JsSetCurrentContext(ctx2) == JsNoError, "R3 ctx2 current");
      napi_env env2 = chakra_nodeapi_create_env(rt2, ctx2, "r3b", nullptr, nullptr);
      Check(env2 != nullptr, "R3 env2 created");
      napi_deferred def = nullptr;
      napi_value prom = nullptr;
      CheckStatus(napi_create_promise(env2, &def, &prom), napi_ok, "R3 create ok");
      SetGlobalNumber(env2, "__r3_seen", -1);
      napi_value then_fn = nullptr;
      {
        JsPropertyIdRef pid = JS_INVALID_REFERENCE;
        JsCreatePropertyId("then", 4, &pid);
        JsValueRef f = JS_INVALID_REFERENCE;
        JsGetProperty(reinterpret_cast<JsValueRef>(prom), pid, &f);
        (void)NodeApiScopes_RootIntoInnermost(env2, reinterpret_cast<napi_value>(f));
        then_fn = reinterpret_cast<napi_value>(f);
      }
      napi_value handler = EvalJS(
          env2,
          "(function(v){ globalThis.__r3_seen = v; return v; })");
      Check(handler != nullptr, "R3 handler created");
      {
        JsValueRef fn = reinterpret_cast<JsValueRef>(then_fn);
        JsValueRef args[2] = {reinterpret_cast<JsValueRef>(prom),
                              reinterpret_cast<JsValueRef>(handler)};
        JsValueRef res = JS_INVALID_REFERENCE;
        Check(JsCallFunction(fn, args, 2, &res) == JsNoError, "R3 then ok");
        (void)res;
      }
      JsValueRef num = JS_INVALID_REFERENCE;
      JsDoubleToNumber(7.0, &num);
      (void)NodeApiScopes_RootIntoInnermost(env2, reinterpret_cast<napi_value>(num));
      CheckStatus(napi_resolve_deferred(env2, def, reinterpret_cast<napi_value>(num)),
                  napi_ok, "R3 resolve ok");
      double seen = -2;
      Check(GetGlobalNumber("__r3_seen", &seen), "R3 seen readable");
      Check(seen == -1, "R3 handler not yet run before provider pump");
      Check(NodeApiPromise_QueueDepth(env2) > 0, "R3 provider queue non-empty");
      size_t pumped = 0;
      CheckStatus(NodeApiPromise_Pump(env2, 0, &pumped), napi_ok, "R3 provider pump ok");
      Check(pumped > 0, "R3 provider pumped >0");
      Check(GetGlobalNumber("__r3_seen", &seen), "R3 seen readable after");
      Check(seen == 7, "R3 handler saw 7 after provider pump");
      Check(NodeApiPromise_QueueDepth(env2) == 0, "R3 provider queue quiet");
      chakra_nodeapi_destroy_env(env2);
      JsSetCurrentContext(g_context);
      JsDisposeRuntime(rt2);
      /* g_runtime/g_context still current for later phases. */
    }
    chakra_nodeapi_destroy_env(env);
    std::printf("phaseR3: done\n");
  }

  /* ========== R4: double-settle no-op ========== */
  {
    std::printf("phaseR4 double-settle: ...\n");
    env = chakra_nodeapi_create_env(g_runtime, g_context, "r4", nullptr, nullptr);
    Check(env != nullptr, "R4 create env");
    HostPump pump;
    JsSetPromiseContinuationCallback(HostContinuation, &pump);
    /* Resolve-then-resolve/reject. */
    napi_deferred def = nullptr;
    napi_value prom = nullptr;
    CheckStatus(napi_create_promise(env, &def, &prom), napi_ok, "R4 create ok");
    SetGlobalNumber(env, "__r4_a", -1);
    {
      napi_value handler = EvalJS(
          env,
          "(function(v){ globalThis.__r4_a = v; return v; })");
      napi_value then_fn = nullptr;
      {
        JsPropertyIdRef pid = JS_INVALID_REFERENCE;
        JsCreatePropertyId("then", 4, &pid);
        JsValueRef f = JS_INVALID_REFERENCE;
        JsGetProperty(reinterpret_cast<JsValueRef>(prom), pid, &f);
        (void)NodeApiScopes_RootIntoInnermost(env, reinterpret_cast<napi_value>(f));
        then_fn = reinterpret_cast<napi_value>(f);
      }
      JsValueRef fn = reinterpret_cast<JsValueRef>(then_fn);
      JsValueRef args[2] = {reinterpret_cast<JsValueRef>(prom),
                            reinterpret_cast<JsValueRef>(handler)};
      JsValueRef res = JS_INVALID_REFERENCE;
      Check(JsCallFunction(fn, args, 2, &res) == JsNoError, "R4 then ok");
      (void)res;
    }
    JsValueRef n1 = JS_INVALID_REFERENCE;
    JsDoubleToNumber(11.0, &n1);
    (void)NodeApiScopes_RootIntoInnermost(env, reinterpret_cast<napi_value>(n1));
    JsValueRef n2 = JS_INVALID_REFERENCE;
    JsDoubleToNumber(22.0, &n2);
    (void)NodeApiScopes_RootIntoInnermost(env, reinterpret_cast<napi_value>(n2));
    CheckStatus(napi_resolve_deferred(env, def, reinterpret_cast<napi_value>(n1)),
                napi_ok, "R4 first resolve ok");
    CheckStatus(napi_resolve_deferred(env, def, reinterpret_cast<napi_value>(n2)),
                napi_ok, "R4 second resolve ok (no-op)");
    napi_value rej = NapiStr(env, "late-reject");
    CheckStatus(napi_reject_deferred(env, def, rej), napi_ok,
                "R4 reject after resolve ok (no-op)");
    Check(NodeApiPromise_DeferredCount(env) == 1, "R4 count still 1 (tombstone)");
    CheckStatus(PumpHost(&pump), napi_ok, "R4 pump ok");
    double seen = -2;
    Check(GetGlobalNumber("__r4_a", &seen), "R4 seen readable");
    Check(seen == 11, "R4 first value wins (11, not 22)");
    /* Reject-then-resolve in reverse order. */
    napi_deferred def2 = nullptr;
    napi_value prom2 = nullptr;
    CheckStatus(napi_create_promise(env, &def2, &prom2), napi_ok, "R4 create2 ok");
    SetGlobalString(env, "__r4_b", "none");
    {
      napi_value handler = EvalJS(
          env,
          "(function(e){ globalThis.__r4_b = String(e); return e; })");
      napi_value catch_fn = nullptr;
      {
        JsPropertyIdRef pid = JS_INVALID_REFERENCE;
        JsCreatePropertyId("catch", 5, &pid);
        JsValueRef f = JS_INVALID_REFERENCE;
        JsGetProperty(reinterpret_cast<JsValueRef>(prom2), pid, &f);
        (void)NodeApiScopes_RootIntoInnermost(env, reinterpret_cast<napi_value>(f));
        catch_fn = reinterpret_cast<napi_value>(f);
      }
      JsValueRef fn = reinterpret_cast<JsValueRef>(catch_fn);
      JsValueRef args[2] = {reinterpret_cast<JsValueRef>(prom2),
                            reinterpret_cast<JsValueRef>(handler)};
      JsValueRef res = JS_INVALID_REFERENCE;
      Check(JsCallFunction(fn, args, 2, &res) == JsNoError, "R4 catch ok");
      (void)res;
    }
    napi_value r1 = NapiStr(env, "first-reject");
    napi_value v2 = NapiStr(env, "late-resolve");
    CheckStatus(napi_reject_deferred(env, def2, r1), napi_ok, "R4 first reject ok");
    CheckStatus(napi_resolve_deferred(env, def2, v2), napi_ok,
                "R4 resolve after reject ok (no-op)");
    CheckStatus(PumpHost(&pump), napi_ok, "R4 pump2 ok");
    std::string sb;
    Check(GetGlobalString("__r4_b", &sb), "R4 seen b readable");
    Check(sb == "first-reject", "R4 first reject wins");
    chakra_nodeapi_destroy_env(env);
    std::printf("phaseR4: done\n");
  }

  /* ========== R5: is_promise discrimination ========== */
  {
    std::printf("phaseR5 is_promise: ...\n");
    env = chakra_nodeapi_create_env(g_runtime, g_context, "r5", nullptr, nullptr);
    Check(env != nullptr, "R5 create env");
    napi_deferred def = nullptr;
    napi_value prom = nullptr;
    CheckStatus(napi_create_promise(env, &def, &prom), napi_ok, "R5 create ok");
    bool b = false;
    CheckStatus(napi_is_promise(env, prom, &b), napi_ok, "R5 promise ok");
    Check(b, "R5 provider promise true");
    /* JS Promise via Promise.race (settles after pump only).
     * Promise.resolve is a promise immediately. */
    {
      napi_value p = EvalJS(env, "Promise.resolve(1)");
      Check(p != nullptr, "R5 Promise.resolve created");
      b = false;
      CheckStatus(napi_is_promise(env, p, &b), napi_ok, "R5 JS promise ok");
      Check(b, "R5 JS promise true");
    }
    {
      JsValueRef o = JS_INVALID_REFERENCE;
      JsCreateObject(&o);
      (void)NodeApiScopes_RootIntoInnermost(env, reinterpret_cast<napi_value>(o));
      b = true;
      CheckStatus(napi_is_promise(env, reinterpret_cast<napi_value>(o), &b),
                  napi_ok, "R5 object ok");
      Check(!b, "R5 object false (ok, not error)");
    }
    {
      JsValueRef n = JS_INVALID_REFERENCE;
      JsDoubleToNumber(42.0, &n);
      (void)NodeApiScopes_RootIntoInnermost(env, reinterpret_cast<napi_value>(n));
      b = true;
      CheckStatus(napi_is_promise(env, reinterpret_cast<napi_value>(n), &b),
                  napi_ok, "R5 number ok");
      Check(!b, "R5 number false");
    }
    {
      napi_value s = NapiStr(env, "hi");
      b = true;
      CheckStatus(napi_is_promise(env, s, &b), napi_ok, "R5 string ok");
      Check(!b, "R5 string false");
    }
    {
      JsValueRef nv = JS_INVALID_REFERENCE;
      JsGetNullValue(&nv);
      b = true;
      CheckStatus(napi_is_promise(env, reinterpret_cast<napi_value>(nv), &b),
                  napi_ok, "R5 null ok");
      Check(!b, "R5 null false");
    }
    {
      JsValueRef uv = JS_INVALID_REFERENCE;
      JsGetUndefinedValue(&uv);
      b = true;
      CheckStatus(napi_is_promise(env, reinterpret_cast<napi_value>(uv), &b),
                  napi_ok, "R5 undefined ok");
      Check(!b, "R5 undefined false");
    }
    {
      napi_value f = EvalJS(env, "(function(){})");
      Check(f != nullptr, "R5 function created");
      b = true;
      CheckStatus(napi_is_promise(env, f, &b), napi_ok, "R5 function ok");
      Check(!b, "R5 function false");
    }
    {
      napi_value a = EvalJS(env, "[]");
      Check(a != nullptr, "R5 array created");
      b = true;
      CheckStatus(napi_is_promise(env, a, &b), napi_ok, "R5 array ok");
      Check(!b, "R5 array false");
    }
    bool rb = false;
    CheckStatus(napi_is_promise(env, nullptr, &rb), napi_invalid_arg,
                "R5 NULL value => invalid_arg");
    napi_value okv = NapiStr(env, "x");
    CheckStatus(napi_is_promise(env, okv, nullptr), napi_invalid_arg,
                "R5 NULL result => invalid_arg");
    /* Executes with pending (C-Yes): throw then is_promise still ok. */
    Check(!HasPending(env), "R5 no pending before");
    {
      JsValueRef m = JS_INVALID_REFERENCE;
      JsCreateObject(&m);
      (void)NodeApiScopes_RootIntoInnermost(env, reinterpret_cast<napi_value>(m));
      CheckStatus(napi_throw(env, reinterpret_cast<napi_value>(m)), napi_ok,
                  "R5 seed throw");
      b = false;
      CheckStatus(napi_is_promise(env, okv, &b), napi_ok,
                  "R5 is_promise executes with pending");
      Check(!b, "R5 result with pending");
      Check(HasPending(env), "R5 pending preserved");
      napi_value cleared = nullptr;
      (void)napi_get_and_clear_last_exception(env, &cleared);
      Check(cleared == reinterpret_cast<napi_value>(m), "R5 original preserved");
    }
    chakra_nodeapi_destroy_env(env);
    std::printf("phaseR5: done\n");
  }

  /* ========== R6: deferred-after-destroy + cross-env ========== */
  {
    std::printf("phaseR6 after-destroy/cross-env: ...\n");
    env = chakra_nodeapi_create_env(g_runtime, g_context, "r6a", nullptr, nullptr);
    Check(env != nullptr, "R6 env A created");
    napi_deferred def = nullptr;
    napi_value prom = nullptr;
    CheckStatus(napi_create_promise(env, &def, &prom), napi_ok, "R6 create ok");
    napi_env saved_env = env;
    napi_deferred saved_def = def;
    /* Capture a resolution value before destroy (scope-rooted in env A
     * would die with the env; use a fresh global string instead for the
     * post-destroy call? The post-destroy call passes `resolution` as a
     * JsValueRef that must not be dereferenced (env check fails first).
     * Use a static-looking null-terminated value created on the same
     * context (still live JS heap, even though env A is dead). */
    JsValueRef res_val = JS_INVALID_REFERENCE;
    JsCreateString("after", 5, &res_val);
    /* res_val is unrooted but the call must fail on env check BEFORE
     * touching it; that is exactly the no-UAF assertion. */
    napi_value res_napi = reinterpret_cast<napi_value>(res_val);
    chakra_nodeapi_destroy_env(env);
    env = nullptr;
    /* Deferred-after-destroy: clean invalid_arg, no crash. */
    CheckStatus(napi_resolve_deferred(saved_env, saved_def, res_napi),
                napi_invalid_arg, "R6 resolve on dead env => invalid_arg");
    CheckStatus(napi_reject_deferred(saved_env, saved_def, res_napi),
                napi_invalid_arg, "R6 reject on dead env => invalid_arg");
    /* Cross-env: deferred from dead env A used on live env B. */
    env = chakra_nodeapi_create_env(g_runtime, g_context, "r6b", nullptr, nullptr);
    Check(env != nullptr, "R6 env B created");
    napi_value live_res = NapiStr(env, "cross");
    CheckStatus(napi_resolve_deferred(env, saved_def, live_res),
                napi_invalid_arg, "R6 cross-env resolve => invalid_arg");
    /* A fresh deferred on B still works (proves B usable). */
    napi_deferred defB = nullptr;
    napi_value promB = nullptr;
    CheckStatus(napi_create_promise(env, &defB, &promB), napi_ok,
                "R6 fresh create on B ok");
    CheckStatus(napi_resolve_deferred(env, defB, live_res), napi_ok,
                "R6 fresh resolve on B ok");
    chakra_nodeapi_destroy_env(env);
    env = nullptr;
    std::printf("phaseR6: done\n");
  }

  /* ========== R7: thenable assimilation rejects (engine truth) ========== */
  {
    std::printf("phaseR7 thenable assimilation: ...\n");
    env = chakra_nodeapi_create_env(g_runtime, g_context, "r7", nullptr, nullptr);
    Check(env != nullptr, "R7 create env");
    HostPump pump;
    JsSetPromiseContinuationCallback(HostContinuation, &pump);
    napi_deferred def = nullptr;
    napi_value prom = nullptr;
    CheckStatus(napi_create_promise(env, &def, &prom), napi_ok, "R7 create ok");
    SetGlobalString(env, "__r7_seen", "none");
    {
      napi_value handler = EvalJS(
          env,
          "(function(e){ globalThis.__r7_seen = (e && e.name) ? (e.name+':'+e.message) : String(e); return e; })");
      napi_value catch_fn = nullptr;
      {
        JsPropertyIdRef pid = JS_INVALID_REFERENCE;
        JsCreatePropertyId("catch", 5, &pid);
        JsValueRef f = JS_INVALID_REFERENCE;
        JsGetProperty(reinterpret_cast<JsValueRef>(prom), pid, &f);
        (void)NodeApiScopes_RootIntoInnermost(env, reinterpret_cast<napi_value>(f));
        catch_fn = reinterpret_cast<napi_value>(f);
      }
      JsValueRef fn = reinterpret_cast<JsValueRef>(catch_fn);
      JsValueRef args[2] = {reinterpret_cast<JsValueRef>(prom),
                            reinterpret_cast<JsValueRef>(handler)};
      JsValueRef res = JS_INVALID_REFERENCE;
      Check(JsCallFunction(fn, args, 2, &res) == JsNoError, "R7 catch ok");
      (void)res;
    }
    /* Thenable whose `then` getter throws. Per ECMAScript promise
     * resolution, resolve(thenable) does NOT throw to the caller: it
     * rejects the promise with the thrown value. Engine truth
     * (verified): napi_resolve_deferred returns napi_ok, the promise
     * becomes rejected after the host pump drains the assimilation
     * task. Assert that (not a pending at resolve time). */
    napi_value thenable = EvalJS(
        env,
        "({ get then() { throw new TypeError('then-getter'); } })");
    Check(thenable != nullptr, "R7 thenable created");
    CheckStatus(napi_resolve_deferred(env, def, thenable),
                napi_ok, "R7 resolve thenable ok (rejects, not throws)");
    Check(!HasPending(env), "R7 no pending at resolve (rejection queued)");
    CheckStatus(PumpHost(&pump), napi_ok, "R7 pump ok (assimilation)");
    std::string seen;
    Check(GetGlobalString("__r7_seen", &seen), "R7 seen readable");
    Check(seen == "TypeError:then-getter", "R7 rejected with TypeError");
    JsPromiseState ps = JsPromiseStatePending;
    Check(JsGetPromiseState(reinterpret_cast<JsValueRef>(prom), &ps) == JsNoError,
          "R7 state query ok");
    Check(ps == JsPromiseStateRejected, "R7 rejected after pump");
    /* Record is settled: second settle is ok no-op. */
    napi_value okval = NapiStr(env, "after");
    CheckStatus(napi_resolve_deferred(env, def, okval), napi_ok,
                "R7 second settle ok (tombstone)");
    chakra_nodeapi_destroy_env(env);
    std::printf("phaseR7: done\n");
  }

  /* ========== R8: invalid args ========== */
  {
    std::printf("phaseR8 invalid args: ...\n");
    env = chakra_nodeapi_create_env(g_runtime, g_context, "r8", nullptr, nullptr);
    Check(env != nullptr, "R8 create env");
    napi_deferred def = nullptr;
    napi_value prom = reinterpret_cast<napi_value>(reinterpret_cast<void*>(0xBAD));
    CheckStatus(napi_create_promise(env, nullptr, &prom), napi_invalid_arg,
                "R8 NULL deferred out => invalid_arg");
    Check(prom == reinterpret_cast<napi_value>(reinterpret_cast<void*>(0xBAD)),
          "R8 promise untouched on NULL deferred");
    CheckStatus(napi_create_promise(env, &def, nullptr), napi_invalid_arg,
                "R8 NULL promise out => invalid_arg");
    prom = nullptr;
    CheckStatus(napi_create_promise(env, &def, &prom), napi_ok, "R8 create ok");
    napi_value v = NapiStr(env, "x");
    CheckStatus(napi_resolve_deferred(env, nullptr, v), napi_invalid_arg,
                "R8 NULL deferred resolve => invalid_arg");
    CheckStatus(napi_reject_deferred(env, nullptr, v), napi_invalid_arg,
                "R8 NULL deferred reject => invalid_arg");
    CheckStatus(napi_resolve_deferred(env, def, nullptr), napi_invalid_arg,
                "R8 NULL resolution => invalid_arg");
    CheckStatus(napi_reject_deferred(env, def, nullptr), napi_invalid_arg,
                "R8 NULL rejection => invalid_arg");
    /* Deferred still usable after invalid-arg attempts (no partial
     * settle marked). */
    CheckStatus(napi_resolve_deferred(env, def, v), napi_ok,
                "R8 resolve after invalid ok");
    chakra_nodeapi_destroy_env(env);
    std::printf("phaseR8: done\n");
  }

  /* ========== S1: run_script value + globals ========== */
  {
    std::printf("phaseS1 run_script value/globals: ...\n");
    env = chakra_nodeapi_create_env(g_runtime, g_context, "s1", nullptr, nullptr);
    Check(env != nullptr, "S1 create env");
    SetGlobalNumber(env, "__s1_base", 10);
    napi_value script = NapiStr(env, "globalThis.__s1_base + 2");
    napi_value res = nullptr;
    CheckStatus(napi_run_script(env, script, &res), napi_ok, "S1 run ok");
    Check(res != nullptr, "S1 result non-null");
    {
      JsValueType t = JsObject;
      JsGetValueType(reinterpret_cast<JsValueRef>(res), &t);
      Check(t == JsNumber, "S1 result is number");
      double d = 0;
      JsNumberToDouble(reinterpret_cast<JsValueRef>(res), &d);
      Check(d == 12, "S1 10+2 == 12 (globals visible)");
    }
    /* Completion value of last statement. */
    napi_value script2 = NapiStr(env, "1; 2; 3");
    res = nullptr;
    CheckStatus(napi_run_script(env, script2, &res), napi_ok, "S1 run2 ok");
    {
      double d = 0;
      JsNumberToDouble(reinterpret_cast<JsValueRef>(res), &d);
      Check(d == 3, "S1 last statement wins (3)");
    }
    chakra_nodeapi_destroy_env(env);
    std::printf("phaseS1: done\n");
  }

  /* ========== S2/S3: run_script errors ========== */
  {
    std::printf("phaseS2 syntax error: ...\n");
    env = chakra_nodeapi_create_env(g_runtime, g_context, "s2", nullptr, nullptr);
    Check(env != nullptr, "S2 create env");
    napi_value script = NapiStr(env, "var x = ;");
    napi_value res = reinterpret_cast<napi_value>(reinterpret_cast<void*>(0xBAD));
    CheckStatus(napi_run_script(env, script, &res), napi_pending_exception,
                "S2 syntax => pending");
    Check(res == reinterpret_cast<napi_value>(reinterpret_cast<void*>(0xBAD)),
          "S2 result untouched on failure");
    Check(HasPending(env), "S2 pending true (stays pending)");
    napi_value cleared = nullptr;
    CheckStatus(napi_get_and_clear_last_exception(env, &cleared), napi_ok,
                "S2 clear ok");
    Check(cleared != nullptr, "S2 cleared non-null");
    Check(GetStrProp(cleared, "name") == "SyntaxError", "S2 name SyntaxError");
    Check(!HasPending(env), "S2 cleared (no longer pending)");
    chakra_nodeapi_destroy_env(env);
    std::printf("phaseS2: done\n");
  }
  {
    std::printf("phaseS3 runtime error class: ...\n");
    env = chakra_nodeapi_create_env(g_runtime, g_context, "s3", nullptr, nullptr);
    Check(env != nullptr, "S3 create env");
    {
      napi_value script = NapiStr(env, "throw new TypeError('boom')");
      napi_value res = nullptr;
      CheckStatus(napi_run_script(env, script, &res), napi_pending_exception,
                  "S3 throw => pending");
      Check(res == nullptr, "S3 result null on failure (untouched)");
      Check(HasPending(env), "S3 pending true");
      napi_value cleared = nullptr;
      (void)napi_get_and_clear_last_exception(env, &cleared);
      Check(cleared != nullptr, "S3 cleared non-null");
      Check(GetStrProp(cleared, "name") == "TypeError", "S3 name TypeError");
      Check(GetStrProp(cleared, "message") == "boom", "S3 message boom");
    }
    {
      napi_value script = NapiStr(env, "throw new RangeError('rng')");
      napi_value res = nullptr;
      CheckStatus(napi_run_script(env, script, &res), napi_pending_exception,
                  "S3 range throw => pending");
      napi_value cleared = nullptr;
      (void)napi_get_and_clear_last_exception(env, &cleared);
      Check(cleared != nullptr, "S3 range cleared non-null");
      Check(GetStrProp(cleared, "name") == "RangeError", "S3 name RangeError");
      Check(GetStrProp(cleared, "message") == "rng", "S3 message rng");
    }
    /* Non-string input => string_expected, no pending. */
    {
      JsValueRef num = JS_INVALID_REFERENCE;
      JsDoubleToNumber(42.0, &num);
      (void)NodeApiScopes_RootIntoInnermost(env, reinterpret_cast<napi_value>(num));
      napi_value res = nullptr;
      CheckStatus(napi_run_script(env, reinterpret_cast<napi_value>(num), &res),
                  napi_string_expected, "S3 number => string_expected");
      Check(res == nullptr, "S3 result untouched on string_expected");
      Check(!HasPending(env), "S3 no pending on string_expected");
    }
    {
      JsValueRef o = JS_INVALID_REFERENCE;
      JsCreateObject(&o);
      (void)NodeApiScopes_RootIntoInnermost(env, reinterpret_cast<napi_value>(o));
      napi_value res = nullptr;
      CheckStatus(napi_run_script(env, reinterpret_cast<napi_value>(o), &res),
                  napi_string_expected, "S3 object => string_expected");
    }
    /* Pending-gate: already pending => gated without running. */
    {
      SetGlobalNumber(env, "__s3_gate", 0);
      napi_value marker_str = NapiStr(env, "gate-marker");
      JsValueRef m = JS_INVALID_REFERENCE;
      JsCreateObject(&m);
      (void)NodeApiScopes_RootIntoInnermost(env, reinterpret_cast<napi_value>(m));
      CheckStatus(napi_throw(env, reinterpret_cast<napi_value>(m)), napi_ok,
                  "S3 seed throw");
      napi_value script = NapiStr(env, "globalThis.__s3_gate = 99; 99");
      napi_value res = nullptr;
      CheckStatus(napi_run_script(env, script, &res), napi_pending_exception,
                  "S3 gated while pending");
      Check(res == nullptr, "S3 gated result untouched");
      double g = -1;
      Check(GetGlobalNumber("__s3_gate", &g), "S3 gate readable");
      Check(g == 0, "S3 gated run did not execute (global unchanged)");
      napi_value cleared = nullptr;
      (void)napi_get_and_clear_last_exception(env, &cleared);
      Check(cleared == reinterpret_cast<napi_value>(m), "S3 original preserved");
      /* Now unblocked: same script runs. */
      CheckStatus(napi_run_script(env, script, &res), napi_ok,
                  "S3 run after clear ok");
      Check(GetGlobalNumber("__s3_gate", &g), "S3 gate readable after");
      Check(g == 99, "S3 ran after clear (99)");
    }
    {
      napi_value ok = NapiStr(env, "1");
      CheckStatus(napi_run_script(env, nullptr, &ok), napi_invalid_arg,
                  "S3 NULL script => invalid_arg");
      CheckStatus(napi_run_script(env, ok, nullptr), napi_invalid_arg,
                  "S3 NULL result => invalid_arg");
    }
    chakra_nodeapi_destroy_env(env);
    std::printf("phaseS3: done\n");
  }

  /* ========== V1/V2: versions (landed Wave 1, shape verification) ========== */
  {
    std::printf("phaseV versions: ...\n");
    env = chakra_nodeapi_create_env(g_runtime, g_context, "v1", nullptr, nullptr);
    Check(env != nullptr, "V create env");
    uint32_t ver = 0;
    CheckStatus(napi_get_version(env, &ver), napi_ok, "V get_version ok");
    Check(ver == 8, "V version == 8");
    CheckStatus(napi_get_version(env, nullptr), napi_invalid_arg,
                "V NULL result => invalid_arg");
    const napi_node_version* nv = nullptr;
    CheckStatus(napi_get_node_version(env, &nv), napi_ok, "V get_node_version ok");
    Check(nv != nullptr, "V version struct non-null");
    if (nv != nullptr) {
      Check(nv->major == 1 && nv->minor == 0 && nv->patch == 0,
            "V provider 1.0.0");
      Check(nv->release != nullptr &&
                std::strcmp(nv->release, "libChakraNodeApi/1.0.0") == 0,
            "V release identifies Chakra provider (B7)");
    }
    CheckStatus(napi_get_node_version(env, nullptr), napi_invalid_arg,
                "V NULL version => invalid_arg");
    chakra_nodeapi_destroy_env(env);
    std::printf("phaseV: done\n");
  }

  /* ========== M1: adjust_external_memory accumulates ========== */
  {
    std::printf("phaseM adjust_external_memory: ...\n");
    env = chakra_nodeapi_create_env(g_runtime, g_context, "m1", nullptr, nullptr);
    Check(env != nullptr, "M create env");
    Check(NodeApiScript_ExternalTotal(env) == 0, "M initial total 0");
    int64_t adj = -777;
    CheckStatus(napi_adjust_external_memory(env, 100, &adj), napi_ok, "M +100 ok");
    Check(adj == 100, "M total 100");
    CheckStatus(napi_adjust_external_memory(env, 200, &adj), napi_ok, "M +200 ok");
    Check(adj == 300, "M total 300");
    CheckStatus(napi_adjust_external_memory(env, -50, &adj), napi_ok, "M -50 ok");
    Check(adj == 250, "M total 250");
    CheckStatus(napi_adjust_external_memory(env, 100, &adj), napi_ok, "M +100 ok");
    Check(adj == 350, "M total 350");
    Check(NodeApiScript_ExternalTotal(env) == 350, "M view total 350");
    /* NULL adjusted => invalid_arg with NO mutation. */
    CheckStatus(napi_adjust_external_memory(env, 999, nullptr), napi_invalid_arg,
                "M NULL adjusted => invalid_arg");
    Check(NodeApiScript_ExternalTotal(env) == 350, "M total unchanged on NULL");
    /* Executes with pending (C-Yes): throw then adjust still ok. */
    {
      JsValueRef m = JS_INVALID_REFERENCE;
      JsCreateObject(&m);
      (void)NodeApiScopes_RootIntoInnermost(env, reinterpret_cast<napi_value>(m));
      CheckStatus(napi_throw(env, reinterpret_cast<napi_value>(m)), napi_ok,
                  "M seed throw");
      CheckStatus(napi_adjust_external_memory(env, 10, &adj), napi_ok,
                  "M adjust executes with pending");
      Check(adj == 360, "M total 360 with pending");
      Check(HasPending(env), "M pending preserved");
      napi_value cleared = nullptr;
      (void)napi_get_and_clear_last_exception(env, &cleared);
      Check(cleared == reinterpret_cast<napi_value>(m), "M original preserved");
    }
    chakra_nodeapi_destroy_env(env);
    /* Per-env: second env starts at 0. */
    env = chakra_nodeapi_create_env(g_runtime, g_context, "m2", nullptr, nullptr);
    Check(env != nullptr, "M second env created");
    Check(NodeApiScript_ExternalTotal(env) == 0, "M second env starts 0");
    adj = -1;
    CheckStatus(napi_adjust_external_memory(env, 5, &adj), napi_ok, "M second +5 ok");
    Check(adj == 5, "M second total 5 (per-env)");
    chakra_nodeapi_destroy_env(env);
    std::printf("phaseM: done\n");
  }

  std::printf("promise_script_probe: %d checks, %d failures\n", g_checks, g_failures);
  ShutdownEngine();
  return g_failures == 0 ? 0 : 1;
}
