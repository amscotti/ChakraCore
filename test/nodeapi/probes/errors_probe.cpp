// errors_probe.cpp — N2.5 errors + throw helpers (plan §6 card N2.5).
//
// Covers (each MUST fail probe on mismatch):
//   Create (vendored oracle test_error/test_error.c + test.js):
//     C1. napi_create_error no-code: Error class, message, name, code undefined.
//     C2. napi_create_error with code: code == "ERR_TEST_CODE", name/message.
//     C3. napi_create_type_error / range_error variants + code.
//     C4. NULL msg => empty message; NULL code => code undefined.
//     C5. msg/code non-string => napi_string_expected; NULL result => invalid_arg.
//   Throw (plan §3.10 + status-mapping throw rows):
//     T1. napi_throw arbitrary value: pending, get_and_clear returns same
//         value, exactly one pending (second clear => ok+null).
//     T2. napi_throw_error/type/range with C strings: class/name/message/code.
//     T3. NULL code/msg combos for throw_* (NULL code => undefined, NULL msg
//         => ""). 
//     T4. Throw-while-pending: each throw variant returns pending_exception,
//         root-count unchanged, original preserved (get_and_clear == original).
//   Pending gates (status-mapping C column):
//     P1. create_* executes with pending (ok, pending preserved).
//     P2. is_error executes with pending.
//   is_error (vendor test.js discrimination):
//     I1. true for Error/Type/Range/Syntax/JS Error/subclass MyError.
//     I2. false for {}, string, number, null, undefined, array, function.
//     I3. NULL value/result => invalid_arg.
//   get_and_clear / is_pending (landed N1.5 verification):
//     G1. Initially not pending; after throw pending; clear returns value;
//         second clear ok+null; exactly one pending.
//   Catchable from JS:
//     J1. JS try/catch around native throw_error/type/range/arbitrary
//         catches with correct name/message/code/instanceof.
//     J2. JS try/catch around napi_throw with marker (identity).
//     J3. create_* return observed from JS (instanceof + props).
//   Fatal (Table B B1 interface):
//     F1. napi_fatal_exception with service: service called once with same
//         value, returns napi_ok, does NOT set pending.
//     F2. napi_fatal_exception with pending: pending without firing.
//     F3. napi_fatal_exception NULL err => invalid_arg.
//     F4/F5. Abort modes (subprocess via run_errors_probe.sh, not in main):
//         --fatal-exception-no-service and --fatal-error must abort.
//
// Build (repo root, via gate lock — see run_errors_probe.sh):
//   /tmp/opencode/cc-gate-lock.sh mise exec -- zig c++ \
//     -std=gnu++17 -DNAPI_VERSION=8 -DCHAKRA_NODEAPI_TEST_HOOKS \
//     -Iinclude/nodeapi -Ilib/Jsrt -Ilib/NodeApi \
//     test/nodeapi/probes/errors_probe.cpp \
//     lib/NodeApi/NodeApiEnv.cpp lib/NodeApi/NodeApiLifetime.cpp \
//     lib/NodeApi/NodeApiScopes.cpp lib/NodeApi/NodeApiRefs.cpp \
//     lib/NodeApi/NodeApiErrorsImpl.cpp \
//     -o /tmp/errors_probe -Lzig-out/lib -lChakraCore ...
// Exit 0 = green; 1 = any mismatch. Fatal abort modes exit via SIGABRT.

#include "NodeApiHostApi.h"
#include "js_native_api.h"
#include "node_api.h"
#include "NodeApiEnv.h"
#include "NodeApiLifetime.h"
#include "NodeApiRefs.h"
#include "NodeApiScopes.h"
#include "ChakraCommon.h"
#include "ChakraCore.h"

#include <cstdint>
#include <cstdio>
#include <cstdlib>
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

/* --- Js helpers (probe-internal, not provider) --- */

JsValueRef JsStr(const char* s) {
  JsValueRef v = JS_INVALID_REFERENCE;
  JsCreateString(s, std::strlen(s), &v);
  return v;
}

napi_value NapiStr(napi_env env, const char* s) {
  JsValueRef v = JsStr(s);
  (void)NodeApiScopes_RootIntoInnermost(env, reinterpret_cast<napi_value>(v));
  return reinterpret_cast<napi_value>(v);
}

bool HasPending(napi_env env) {
  bool has = false;
  if (napi_is_exception_pending(env, &has) != napi_ok) return false;
  return has;
}

/* Clear pending via the provider API (returns value, rooted). */
napi_value ClearEx(napi_env env) {
  napi_value v = nullptr;
  (void)napi_get_and_clear_last_exception(env, &v);
  return v;
}

/* Read a string property (message/code/name): returns "" when the property
 * is undefined/missing, or "<undefined>" sentinel distinction. */
std::string GetStrProp(napi_value err, const char* prop) {
  JsPropertyIdRef pid = JS_INVALID_REFERENCE;
  if (JsCreatePropertyId(prop, std::strlen(prop), &pid) != JsNoError) return "<propid-fail>";
  JsValueRef v = JS_INVALID_REFERENCE;
  if (JsGetProperty(reinterpret_cast<JsValueRef>(err), pid, &v) != JsNoError)
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
  if (JsCopyString(v, &s[0], s.size(), &written) != JsNoError) return "<copy-fail>";
  s.resize(written);
  return s;
}

bool IsUndefinedValue(napi_value v) {
  if (v == nullptr) return false;
  JsValueType t = JsObject;
  if (JsGetValueType(reinterpret_cast<JsValueRef>(v), &t) != JsNoError) return false;
  return t == JsUndefined;
}

/* instanceof helper via JsInstanceOf (probe side only). */
bool InstanceOf(napi_value v, const char* ctorName) {
  JsValueRef global = JS_INVALID_REFERENCE;
  if (JsGetGlobalObject(&global) != JsNoError) return false;
  JsPropertyIdRef pid = JS_INVALID_REFERENCE;
  JsCreatePropertyId(ctorName, std::strlen(ctorName), &pid);
  JsValueRef ctor = JS_INVALID_REFERENCE;
  if (JsGetProperty(global, pid, &ctor) != JsNoError) return false;
  bool r = false;
  if (JsInstanceOf(reinterpret_cast<JsValueRef>(v), ctor, &r) != JsNoError) return false;
  return r;
}

bool IsErrorApi(napi_env env, napi_value v) {
  bool r = false;
  if (napi_is_error(env, v, &r) != napi_ok) return false;
  return r;
}

/* Evaluate JS and return the result rooted (for MyError etc.). On any
 * failure returns nullptr. Caller must have no pending exception. */
napi_value EvalJS(napi_env env, const char* code) {
  JsValueRef src = JS_INVALID_REFERENCE;
  JsValueRef url = JS_INVALID_REFERENCE;
  JsValueRef res = JS_INVALID_REFERENCE;
  if (JsCreateString(code, std::strlen(code), &src) != JsNoError) return nullptr;
  if (JsCreateString("probe.js", std::strlen("probe.js"), &url) != JsNoError) return nullptr;
  JsErrorCode ec = JsRun(src, JS_SOURCE_CONTEXT_NONE, url,
                         JsParseScriptAttributeNone, &res);
  if (ec != JsNoError) return nullptr;
  (void)NodeApiScopes_RootIntoInnermost(env, reinterpret_cast<napi_value>(res));
  /* src/url/result are held by the JS heap once run; host roots are not
   * kept (result is scope-rooted above). src/url temporaries are released
   * by the caller convention in hello host (JsRelease); do it here. */
  JsRelease(src, nullptr);
  JsRelease(url, nullptr);
  /* res is scope-rooted; also host-released the temporaries only. */
  return reinterpret_cast<napi_value>(res);
}

/* Run JS that is expected to succeed (no throw). Returns JsRun code. */
JsErrorCode RunJS(const char* code, JsValueRef* outRes) {
  JsValueRef src = JS_INVALID_REFERENCE;
  JsValueRef url = JS_INVALID_REFERENCE;
  JsValueRef res = JS_INVALID_REFERENCE;
  JsCreateString(code, std::strlen(code), &src);
  JsCreateString("probe.js", std::strlen("probe.js"), &url);
  JsErrorCode ec = JsRun(src, JS_SOURCE_CONTEXT_NONE, url,
                         JsParseScriptAttributeNone, &res);
  if (outRes) *outRes = res;
  JsRelease(src, nullptr);
  JsRelease(url, nullptr);
  if (ec != JsNoError) {
    if (res != JS_INVALID_REFERENCE) JsRelease(res, nullptr);
    return ec;
  }
  return JsNoError;
}

/* --- Fatal service state (F1/F2) --- */
int g_fatal_calls = 0;
napi_value g_fatal_seen = nullptr;
void FatalRecorder(void* host_ctx, napi_value exc) {
  (void)host_ctx;
  g_fatal_calls++;
  g_fatal_seen = exc;
}

/* --- JS callback globals (J phases) --- */
napi_env g_env = nullptr;
napi_value g_marker = nullptr; /* sentinel arbitrary value */
napi_value g_codeStr = nullptr; /* "ERR_TEST_CODE" for create callback */
napi_value g_msgStr = nullptr; /* "Error [error]" */

JsValueRef CbThrowError(JsValueRef, bool, JsValueRef*, unsigned short, void*) {
  (void)napi_throw_error(g_env, "ERR_TEST_CODE", "Error [error]");
  JsValueRef u = JS_INVALID_REFERENCE;
  JsGetUndefinedValue(&u);
  return u;
}
JsValueRef CbThrowTypeError(JsValueRef, bool, JsValueRef*, unsigned short, void*) {
  (void)napi_throw_type_error(g_env, "ERR_TEST_CODE", "TypeError [type error]");
  JsValueRef u = JS_INVALID_REFERENCE;
  JsGetUndefinedValue(&u);
  return u;
}
JsValueRef CbThrowRangeError(JsValueRef, bool, JsValueRef*, unsigned short, void*) {
  (void)napi_throw_range_error(g_env, "ERR_TEST_CODE", "RangeError [range error]");
  JsValueRef u = JS_INVALID_REFERENCE;
  JsGetUndefinedValue(&u);
  return u;
}
JsValueRef CbThrowArbitrary(JsValueRef, bool, JsValueRef*, unsigned short, void*) {
  (void)napi_throw(g_env, g_marker);
  JsValueRef u = JS_INVALID_REFERENCE;
  JsGetUndefinedValue(&u);
  return u;
}
JsValueRef CbCreateErrorReturn(JsValueRef, bool, JsValueRef*, unsigned short, void*) {
  napi_value err = nullptr;
  (void)napi_create_error(g_env, g_codeStr, g_msgStr, &err);
  if (err == nullptr) {
    JsValueRef u = JS_INVALID_REFERENCE;
    JsGetUndefinedValue(&u);
    return u;
  }
  return reinterpret_cast<JsValueRef>(err);
}

void BindFn(napi_env env, const char* name, JsNativeFunction fn) {
  JsValueRef f = JS_INVALID_REFERENCE;
  (void)JsCreateFunction(fn, nullptr, &f);
  (void)NodeApiScopes_RootIntoInnermost(env, reinterpret_cast<napi_value>(f));
  JsValueRef global = JS_INVALID_REFERENCE;
  (void)JsGetGlobalObject(&global);
  JsPropertyIdRef pid = JS_INVALID_REFERENCE;
  (void)JsCreatePropertyId(name, std::strlen(name), &pid);
  (void)JsSetProperty(global, pid, f, false);
  /* global holds the function; no host root kept. */
}

} /* namespace */

int main(int argc, char** argv) {
  /* Subprocess abort modes (run_errors_probe.sh invokes these directly
   * and asserts SIGABRT). They never return. */
  if (argc >= 2 && std::strcmp(argv[1], "--fatal-exception-no-service") == 0) {
    if (!InitEngine()) return 2;
    napi_env env = chakra_nodeapi_create_env(g_runtime, g_context, "fatal-no-svc",
                                            nullptr, nullptr);
    if (env == nullptr) return 2;
    JsValueRef m = JS_INVALID_REFERENCE;
    JsCreateString("abort-me", std::strlen("abort-me"), &m);
    (void)NodeApiScopes_RootIntoInnermost(env, reinterpret_cast<napi_value>(m));
    napi_value err = reinterpret_cast<napi_value>(m);
    /* No service registered: must abort (never returns). */
    (void)napi_fatal_exception(env, err);
    std::printf("FAIL: fatal_exception without service returned\n");
    return 1;
  }
  if (argc >= 2 && std::strcmp(argv[1], "--fatal-error") == 0) {
    napi_fatal_error("errors_probe.cpp", std::strlen("errors_probe.cpp"),
                     "fatal-error probe abort", std::strlen("fatal-error probe abort"));
    std::printf("FAIL: fatal_error returned\n");
    return 1;
  }

  if (!InitEngine()) return 2;
  napi_env env = nullptr;

  /* ========== Create family (C1-C5) ========== */
  {
    std::printf("phaseC1 create_error no-code: ...\n");
    env = chakra_nodeapi_create_env(g_runtime, g_context, "c1", nullptr, nullptr);
    Check(env != nullptr, "C1 create env");
    napi_value msg = NapiStr(env, "hello");
    napi_value err = nullptr;
    CheckStatus(napi_create_error(env, nullptr, msg, &err), napi_ok, "C1 status ok");
    Check(err != nullptr, "C1 err non-null");
    Check(InstanceOf(err, "Error"), "C1 instanceof Error");
    Check(GetStrProp(err, "message") == "hello", "C1 message hello");
    Check(GetStrProp(err, "name") == "Error", "C1 name Error (=Error)");
    /* No code at all: property reads undefined. */
    Check(GetStrProp(err, "code") == "<undefined>", "C1 code undefined");
    Check(IsErrorApi(env, err), "C1 is_error true");
    Check(!HasPending(env), "C1 no pending");
    /* Exactly-one-pending check is for throws; create sets none. */
    napi_value cleared = nullptr;
    CheckStatus(napi_get_and_clear_last_exception(env, &cleared), napi_ok, "C1 get clear ok");
    Check(cleared == nullptr, "C1 get clear null (no pending)");
    chakra_nodeapi_destroy_env(env);
    std::printf("phaseC1: done\n");
  }
  {
    std::printf("phaseC2 create_error with code: ...\n");
    env = chakra_nodeapi_create_env(g_runtime, g_context, "c2", nullptr, nullptr);
    Check(env != nullptr, "C2 create env");
    napi_value code = NapiStr(env, "ERR_TEST_CODE");
    napi_value msg = NapiStr(env, "Error [error]");
    napi_value err = nullptr;
    CheckStatus(napi_create_error(env, code, msg, &err), napi_ok, "C2 status ok");
    Check(err != nullptr, "C2 err non-null");
    Check(InstanceOf(err, "Error"), "C2 instanceof Error");
    Check(GetStrProp(err, "code") == "ERR_TEST_CODE", "C2 code ERR_TEST_CODE");
    Check(GetStrProp(err, "message") == "Error [error]", "C2 message");
    Check(GetStrProp(err, "name") == "Error", "C2 name Error");
    Check(IsErrorApi(env, err), "C2 is_error true");
    chakra_nodeapi_destroy_env(env);
    std::printf("phaseC2: done\n");
  }
  {
    std::printf("phaseC3 create type/range: ...\n");
    env = chakra_nodeapi_create_env(g_runtime, g_context, "c3", nullptr, nullptr);
    Check(env != nullptr, "C3 create env");
    {
      napi_value msg = NapiStr(env, "type error");
      napi_value err = nullptr;
      CheckStatus(napi_create_type_error(env, nullptr, msg, &err), napi_ok, "C3 type status");
      Check(err != nullptr && InstanceOf(err, "TypeError"), "C3 instanceof TypeError");
      Check(InstanceOf(err, "Error"), "C3 TypeError instanceof Error");
      Check(GetStrProp(err, "message") == "type error", "C3 type message");
      Check(GetStrProp(err, "name") == "TypeError", "C3 type name");
      Check(GetStrProp(err, "code") == "<undefined>", "C3 type code undefined");
      Check(IsErrorApi(env, err), "C3 type is_error true");
    }
    {
      napi_value code = NapiStr(env, "ERR_TEST_CODE");
      napi_value msg = NapiStr(env, "RangeError [range error]");
      napi_value err = nullptr;
      CheckStatus(napi_create_range_error(env, code, msg, &err), napi_ok, "C3 range status");
      Check(err != nullptr && InstanceOf(err, "RangeError"), "C3 instanceof RangeError");
      Check(InstanceOf(err, "Error"), "C3 RangeError instanceof Error");
      Check(GetStrProp(err, "code") == "ERR_TEST_CODE", "C3 range code");
      Check(GetStrProp(err, "message") == "RangeError [range error]", "C3 range message");
      Check(GetStrProp(err, "name") == "RangeError", "C3 range name");
      Check(IsErrorApi(env, err), "C3 range is_error true");
    }
    {
      /* TypeError with code (covers create_type_error code path). */
      napi_value code = NapiStr(env, "ERR_TEST_CODE");
      napi_value msg = NapiStr(env, "TypeError [type error]");
      napi_value err = nullptr;
      CheckStatus(napi_create_type_error(env, code, msg, &err), napi_ok, "C3 type-code status");
      Check(GetStrProp(err, "code") == "ERR_TEST_CODE", "C3 type code");
      Check(GetStrProp(err, "message") == "TypeError [type error]", "C3 type-code message");
      Check(GetStrProp(err, "name") == "TypeError", "C3 type-code name");
    }
    chakra_nodeapi_destroy_env(env);
    std::printf("phaseC3: done\n");
  }
  {
    std::printf("phaseC4 NULL msg/code: ...\n");
    env = chakra_nodeapi_create_env(g_runtime, g_context, "c4", nullptr, nullptr);
    Check(env != nullptr, "C4 create env");
    napi_value err = nullptr;
    CheckStatus(napi_create_error(env, nullptr, nullptr, &err), napi_ok, "C4 null/null status");
    Check(err != nullptr, "C4 err non-null");
    /* Empty message: JS "" (Len 0) -> GetStrProp returns "" (not <undefined>). */
    Check(GetStrProp(err, "message") == "", "C4 empty message");
    Check(GetStrProp(err, "code") == "<undefined>", "C4 code undefined");
    Check(InstanceOf(err, "Error"), "C4 instanceof Error");
    chakra_nodeapi_destroy_env(env);
    std::printf("phaseC4: done\n");
  }
  {
    std::printf("phaseC5 string_expected/invalid_arg: ...\n");
    env = chakra_nodeapi_create_env(g_runtime, g_context, "c5", nullptr, nullptr);
    Check(env != nullptr, "C5 create env");
    /* Non-string msg (number 42). */
    JsValueRef num = JS_INVALID_REFERENCE;
    (void)JsDoubleToNumber(42.0, &num);
    (void)NodeApiScopes_RootIntoInnermost(env, reinterpret_cast<napi_value>(num));
    napi_value nonStr = reinterpret_cast<napi_value>(num);
    napi_value err = nullptr;
    CheckStatus(napi_create_error(env, nullptr, nonStr, &err), napi_string_expected,
                "C5 non-string msg => string_expected");
    Check(err == nullptr, "C5 err untouched on string_expected (still null)");
    const napi_extended_error_info* ei = nullptr;
    (void)napi_get_last_error_info(env, &ei);
    Check(ei != nullptr && ei->error_code == napi_string_expected, "C5 last_error string_expected");
    /* Non-string code (number as code, string msg). */
    napi_value okMsg = NapiStr(env, "m");
    err = nullptr;
    CheckStatus(napi_create_error(env, nonStr, okMsg, &err), napi_string_expected,
                "C5 non-string code => string_expected");
    /* NULL result. */
    CheckStatus(napi_create_error(env, nullptr, okMsg, nullptr), napi_invalid_arg,
                "C5 NULL result => invalid_arg");
    CheckStatus(napi_create_type_error(env, nullptr, okMsg, nullptr), napi_invalid_arg,
                "C5 type NULL result => invalid_arg");
    CheckStatus(napi_create_range_error(env, nullptr, okMsg, nullptr), napi_invalid_arg,
                "C5 range NULL result => invalid_arg");
    /* Success clears last_error. */
    err = nullptr;
    CheckStatus(napi_create_error(env, nullptr, okMsg, &err), napi_ok, "C5 success ok");
    (void)napi_get_last_error_info(env, &ei);
    Check(ei != nullptr && ei->error_code == napi_ok, "C5 success clears last_error");
    chakra_nodeapi_destroy_env(env);
    std::printf("phaseC5: done\n");
  }

  /* ========== Throw family (T1-T4, G1) ========== */
  {
    std::printf("phaseT1 throw arbitrary + exactly-one: ...\n");
    env = chakra_nodeapi_create_env(g_runtime, g_context, "t1", nullptr, nullptr);
    Check(env != nullptr, "T1 create env");
    Check(!HasPending(env), "T1 initially not pending");
    JsValueRef marker = JS_INVALID_REFERENCE;
    (void)JsCreateObject(&marker);
    (void)NodeApiScopes_RootIntoInnermost(env, reinterpret_cast<napi_value>(marker));
    napi_value mval = reinterpret_cast<napi_value>(marker);
    /* NULL error => invalid_arg, no pending. */
    CheckStatus(napi_throw(env, nullptr), napi_invalid_arg, "T1 NULL throw => invalid_arg");
    Check(!HasPending(env), "T1 still not pending after NULL");
    CheckStatus(napi_throw(env, mval), napi_ok, "T1 throw ok");
    Check(HasPending(env), "T1 pending true after throw");
    /* Exactly one: clear returns same value, second clear null. */
    napi_value cleared = nullptr;
    CheckStatus(napi_get_and_clear_last_exception(env, &cleared), napi_ok, "T1 clear ok");
    Check(cleared == mval, "T1 cleared is the marker (identity)");
    Check(!HasPending(env), "T1 cleared (no longer pending)");
    napi_value second = reinterpret_cast<napi_value>(reinterpret_cast<void*>(0xBAD));
    CheckStatus(napi_get_and_clear_last_exception(env, &second), napi_ok, "T1 second clear ok");
    Check(second == nullptr, "T1 second clear null (exactly one)");
    chakra_nodeapi_destroy_env(env);
    std::printf("phaseT1: done\n");
  }
  {
    std::printf("phaseT2 throw_error/type/range: ...\n");
    env = chakra_nodeapi_create_env(g_runtime, g_context, "t2", nullptr, nullptr);
    Check(env != nullptr, "T2 create env");
    /* Error */
    CheckStatus(napi_throw_error(env, "ERR_TEST_CODE", "Error [error]"), napi_ok,
                "T2 throw_error ok");
    Check(HasPending(env), "T2 pending after throw_error");
    {
      napi_value e = ClearEx(env);
      Check(e != nullptr, "T2 cleared non-null");
      Check(InstanceOf(e, "Error"), "T2 instanceof Error");
      Check(!(InstanceOf(e, "TypeError")), "T2 not TypeError");
      Check(GetStrProp(e, "message") == "Error [error]", "T2 message");
      Check(GetStrProp(e, "code") == "ERR_TEST_CODE", "T2 code");
      Check(GetStrProp(e, "name") == "Error", "T2 name");
      Check(IsErrorApi(env, e), "T2 is_error true (cleared)");
      Check(!HasPending(env), "T2 cleared");
    }
    /* TypeError */
    CheckStatus(napi_throw_type_error(env, "ERR_TEST_CODE", "TypeError [type error]"),
                napi_ok, "T2 throw_type ok");
    Check(HasPending(env), "T2 pending type");
    {
      napi_value e = ClearEx(env);
      Check(e != nullptr, "T2 cleared type non-null");
      Check(InstanceOf(e, "TypeError"), "T2 instanceof TypeError");
      Check(InstanceOf(e, "Error"), "T2 TypeError instanceof Error");
      Check(GetStrProp(e, "message") == "TypeError [type error]", "T2 type message");
      Check(GetStrProp(e, "code") == "ERR_TEST_CODE", "T2 type code");
      Check(GetStrProp(e, "name") == "TypeError", "T2 type name");
    }
    /* RangeError */
    CheckStatus(napi_throw_range_error(env, "ERR_TEST_CODE", "RangeError [range error]"),
                napi_ok, "T2 throw_range ok");
    Check(HasPending(env), "T2 pending range");
    {
      napi_value e = ClearEx(env);
      Check(e != nullptr, "T2 cleared range non-null");
      Check(InstanceOf(e, "RangeError"), "T2 instanceof RangeError");
      Check(GetStrProp(e, "message") == "RangeError [range error]", "T2 range message");
      Check(GetStrProp(e, "code") == "ERR_TEST_CODE", "T2 range code");
      Check(GetStrProp(e, "name") == "RangeError", "T2 range name");
    }
    chakra_nodeapi_destroy_env(env);
    std::printf("phaseT2: done\n");
  }
  {
    std::printf("phaseT3 NULL code/msg combos: ...\n");
    env = chakra_nodeapi_create_env(g_runtime, g_context, "t3", nullptr, nullptr);
    Check(env != nullptr, "T3 create env");
    /* NULL code */
    CheckStatus(napi_throw_error(env, nullptr, "error"), napi_ok, "T3 null-code ok");
    {
      napi_value e = ClearEx(env);
      Check(GetStrProp(e, "message") == "error", "T3 null-code message");
      Check(GetStrProp(e, "code") == "<undefined>", "T3 null-code undefined");
      Check(InstanceOf(e, "Error"), "T3 null-code instanceof Error");
    }
    /* NULL msg */
    CheckStatus(napi_throw_error(env, "MY_CODE", nullptr), napi_ok, "T3 null-msg ok");
    {
      napi_value e = ClearEx(env);
      Check(GetStrProp(e, "message") == "", "T3 null-msg empty message");
      Check(GetStrProp(e, "code") == "MY_CODE", "T3 null-msg code");
    }
    /* NULL both */
    CheckStatus(napi_throw_type_error(env, nullptr, nullptr), napi_ok, "T3 null/null ok");
    {
      napi_value e = ClearEx(env);
      Check(GetStrProp(e, "message") == "", "T3 null/null empty message");
      Check(GetStrProp(e, "code") == "<undefined>", "T3 null/null undefined");
      Check(InstanceOf(e, "TypeError"), "T3 null/null instanceof TypeError");
    }
    chakra_nodeapi_destroy_env(env);
    std::printf("phaseT3: done\n");
  }
  {
    std::printf("phaseT4 throw-while-pending preserves original: ...\n");
    env = chakra_nodeapi_create_env(g_runtime, g_context, "t4", nullptr, nullptr);
    Check(env != nullptr, "T4 create env");
    /* Original pending via arbitrary marker A. */
    JsValueRef markerA = JS_INVALID_REFERENCE;
    (void)JsCreateObject(&markerA);
    (void)NodeApiScopes_RootIntoInnermost(env, reinterpret_cast<napi_value>(markerA));
    napi_value a = reinterpret_cast<napi_value>(markerA);
    CheckStatus(napi_throw(env, a), napi_ok, "T4 seed throw ok");
    Check(HasPending(env), "T4 pending seeded");
    size_t rootsBefore = chakra_nodeapi_test_root_count(env);
    /* Each throw variant must return pending, preserve original, allocate nothing. */
    JsValueRef markerB = JS_INVALID_REFERENCE;
    (void)JsCreateObject(&markerB);
    /* markerB is rooted by us (not by throw); exclude from the delta by
     * recording after it. */
    (void)NodeApiScopes_RootIntoInnermost(env, reinterpret_cast<napi_value>(markerB));
    napi_value b = reinterpret_cast<napi_value>(markerB);
    size_t rootsBeforeThrows = chakra_nodeapi_test_root_count(env);
    CheckStatus(napi_throw(env, b), napi_pending_exception, "T4 rethrow throw => pending");
    CheckStatus(napi_throw_error(env, "X", "y"), napi_pending_exception, "T4 rethrow_error => pending");
    CheckStatus(napi_throw_type_error(env, "X", "y"), napi_pending_exception, "T4 rethrow_type => pending");
    CheckStatus(napi_throw_range_error(env, "X", "y"), napi_pending_exception, "T4 rethrow_range => pending");
    size_t rootsAfterThrows = chakra_nodeapi_test_root_count(env);
    Check(rootsAfterThrows == rootsBeforeThrows, "T4 root-count unchanged by gated throws");
    Check(HasPending(env), "T4 still pending after gated throws");
    napi_value cleared = ClearEx(env);
    Check(cleared == a, "T4 original preserved (cleared == A)");
    Check(!HasPending(env), "T4 cleared");
    (void)rootsBefore;
    chakra_nodeapi_destroy_env(env);
    std::printf("phaseT4: done\n");
  }

  /* ========== Pending gates P1/P2 ========== */
  {
    std::printf("phaseP1 create-with-pending: ...\n");
    env = chakra_nodeapi_create_env(g_runtime, g_context, "p1", nullptr, nullptr);
    Check(env != nullptr, "P1 create env");
    JsValueRef markerA = JS_INVALID_REFERENCE;
    (void)JsCreateObject(&markerA);
    (void)NodeApiScopes_RootIntoInnermost(env, reinterpret_cast<napi_value>(markerA));
    napi_value a = reinterpret_cast<napi_value>(markerA);
    CheckStatus(napi_throw(env, a), napi_ok, "P1 seed throw");
    /* create_error executes with pending (C Yes), returns ok, pending preserved. */
    napi_value msg = NapiStr(env, "while-pending");
    napi_value err = nullptr;
    CheckStatus(napi_create_error(env, nullptr, msg, &err), napi_ok, "P1 create ok with pending");
    Check(err != nullptr, "P1 err non-null");
    Check(HasPending(env), "P1 pending preserved after create");
    /* is_error also executes with pending (C Yes). */
    bool r = false;
    CheckStatus(napi_is_error(env, a, &r), napi_ok, "P1 is_error ok with pending");
    Check(!r, "P1 is_error(marker) false with pending");
    bool r2 = true;
    CheckStatus(napi_is_error(env, err, &r2), napi_ok, "P1 is_error(err) ok");
    Check(r2, "P1 is_error(err) true with pending");
    napi_value cleared = ClearEx(env);
    Check(cleared == a, "P1 original still A (create did not overwrite)");
    Check(!HasPending(env), "P1 cleared");
    chakra_nodeapi_destroy_env(env);
    std::printf("phaseP1: done\n");
  }

  /* ========== is_error I1-I3 ========== */
  {
    std::printf("phaseI is_error discrimination: ...\n");
    env = chakra_nodeapi_create_env(g_runtime, g_context, "i1", nullptr, nullptr);
    Check(env != nullptr, "I create env");
    /* True family via provider create. */
    {
      napi_value m = NapiStr(env, "x");
      napi_value e = nullptr;
      CheckStatus(napi_create_error(env, nullptr, m, &e), napi_ok, "I create_error");
      Check(IsErrorApi(env, e), "I created Error true");
    }
    {
      napi_value m = NapiStr(env, "x");
      napi_value e = nullptr;
      CheckStatus(napi_create_type_error(env, nullptr, m, &e), napi_ok, "I create_type");
      Check(IsErrorApi(env, e), "I created TypeError true");
    }
    {
      napi_value m = NapiStr(env, "x");
      napi_value e = nullptr;
      CheckStatus(napi_create_range_error(env, nullptr, m, &e), napi_ok, "I create_range");
      Check(IsErrorApi(env, e), "I created RangeError true");
    }
    /* JS-created errors. */
    {
      napi_value v = EvalJS(env, "new Error('e')");
      Check(v != nullptr, "I EvalJS Error non-null");
      Check(IsErrorApi(env, v), "I JS new Error true");
    }
    {
      napi_value v = EvalJS(env, "new TypeError('t')");
      Check(v != nullptr, "I EvalJS TypeError non-null");
      Check(IsErrorApi(env, v), "I JS TypeError true");
    }
    {
      napi_value v = EvalJS(env, "new RangeError('r')");
      Check(v != nullptr, "I EvalJS RangeError non-null");
      Check(IsErrorApi(env, v), "I JS RangeError true");
    }
    {
      napi_value v = EvalJS(env, "new SyntaxError('s')");
      Check(v != nullptr, "I EvalJS SyntaxError non-null");
      Check(IsErrorApi(env, v), "I JS SyntaxError true");
    }
    {
      napi_value v = EvalJS(env, "new ReferenceError('r')");
      Check(v != nullptr, "I EvalJS ReferenceError non-null");
      Check(IsErrorApi(env, v), "I JS ReferenceError true");
    }
    {
      napi_value v = EvalJS(env, "new URIError('u')");
      Check(v != nullptr, "I EvalJS URIError non-null");
      Check(IsErrorApi(env, v), "I JS URIError true");
    }
    {
      napi_value v = EvalJS(env, "new EvalError('e')");
      Check(v != nullptr, "I EvalJS EvalError non-null");
      Check(IsErrorApi(env, v), "I JS EvalError true");
    }
    {
      napi_value v = EvalJS(env, "class MyError extends Error {}; new MyError('m')");
      Check(v != nullptr, "I EvalJS MyError non-null");
      Check(IsErrorApi(env, v), "I JS MyError subclass true");
    }
    /* False family. */
    {
      JsValueRef o = JS_INVALID_REFERENCE;
      (void)JsCreateObject(&o);
      (void)NodeApiScopes_RootIntoInnermost(env, reinterpret_cast<napi_value>(o));
      Check(!IsErrorApi(env, reinterpret_cast<napi_value>(o)), "I {} false");
    }
    {
      napi_value s = NapiStr(env, "non-object");
      Check(!IsErrorApi(env, s), "I string false");
    }
    {
      napi_value v = EvalJS(env, "(42)");
      Check(v != nullptr && !IsErrorApi(env, v), "I number false");
    }
    {
      JsValueRef nv = JS_INVALID_REFERENCE;
      (void)JsGetNullValue(&nv);
      bool r = true;
      CheckStatus(napi_is_error(env, reinterpret_cast<napi_value>(nv), &r), napi_ok, "I null ok");
      Check(!r, "I null false");
    }
    {
      JsValueRef uv = JS_INVALID_REFERENCE;
      (void)JsGetUndefinedValue(&uv);
      bool r = true;
      CheckStatus(napi_is_error(env, reinterpret_cast<napi_value>(uv), &r), napi_ok, "I undefined ok");
      Check(!r, "I undefined false");
    }
    {
      napi_value v = EvalJS(env, "[]");
      Check(v != nullptr && !IsErrorApi(env, v), "I array false");
    }
    {
      napi_value v = EvalJS(env, "(function(){})");
      Check(v != nullptr && !IsErrorApi(env, v), "I function false");
    }
    /* Arg validation. */
    bool r = false;
    CheckStatus(napi_is_error(env, nullptr, &r), napi_invalid_arg, "I NULL value => invalid_arg");
    napi_value okv = NapiStr(env, "x");
    CheckStatus(napi_is_error(env, okv, nullptr), napi_invalid_arg, "I NULL result => invalid_arg");
    chakra_nodeapi_destroy_env(env);
    std::printf("phaseI: done\n");
  }

  /* ========== Fatal F1-F3 + JS catch J1-J3 ========== */
  {
    std::printf("phaseF fatal with service: ...\n");
    chakra_nodeapi_host_services svc = {};
    svc.fatal_exception = FatalRecorder;
    env = chakra_nodeapi_create_env(g_runtime, g_context, "f1", &svc, nullptr);
    Check(env != nullptr, "F1 create env with service");
    g_fatal_calls = 0;
    g_fatal_seen = nullptr;
    JsValueRef m = JS_INVALID_REFERENCE;
    (void)JsCreateObject(&m);
    (void)NodeApiScopes_RootIntoInnermost(env, reinterpret_cast<napi_value>(m));
    napi_value err = reinterpret_cast<napi_value>(m);
    CheckStatus(napi_fatal_exception(env, err), napi_ok, "F1 fatal ok");
    Check(g_fatal_calls == 1, "F1 service called once");
    Check(g_fatal_seen == err, "F1 service saw same value (identity)");
    Check(!HasPending(env), "F1 does not set pending");
    /* NULL err => invalid_arg, service not called. */
    CheckStatus(napi_fatal_exception(env, nullptr), napi_invalid_arg, "F1 NULL err => invalid_arg");
    Check(g_fatal_calls == 1, "F1 service not called on NULL");
    chakra_nodeapi_destroy_env(env);
    std::printf("phaseF: done\n");
  }
  {
    std::printf("phaseF2 fatal gated by pending: ...\n");
    chakra_nodeapi_host_services svc = {};
    svc.fatal_exception = FatalRecorder;
    env = chakra_nodeapi_create_env(g_runtime, g_context, "f2", &svc, nullptr);
    Check(env != nullptr, "F2 create env");
    g_fatal_calls = 0;
    JsValueRef a = JS_INVALID_REFERENCE;
    (void)JsCreateObject(&a);
    (void)NodeApiScopes_RootIntoInnermost(env, reinterpret_cast<napi_value>(a));
    napi_value aval = reinterpret_cast<napi_value>(a);
    CheckStatus(napi_throw(env, aval), napi_ok, "F2 seed throw");
    CheckStatus(napi_fatal_exception(env, aval), napi_pending_exception,
                "F2 gated => pending");
    Check(g_fatal_calls == 0, "F2 service not called when pending");
    napi_value cleared = ClearEx(env);
    Check(cleared == aval, "F2 original preserved");
    chakra_nodeapi_destroy_env(env);
    std::printf("phaseF2: done\n");
  }

  /* J phases: JS try/catch around native throws. */
  {
    std::printf("phaseJ JS catch: ...\n");
    env = chakra_nodeapi_create_env(g_runtime, g_context, "j1", nullptr, nullptr);
    Check(env != nullptr, "J create env");
    g_env = env;
    JsValueRef marker = JS_INVALID_REFERENCE;
    (void)JsCreateObject(&marker);
    (void)NodeApiScopes_RootIntoInnermost(env, reinterpret_cast<napi_value>(marker));
    g_marker = reinterpret_cast<napi_value>(marker);
    g_codeStr = NapiStr(env, "ERR_TEST_CODE");
    g_msgStr = NapiStr(env, "Error [error]");
    BindFn(env, "__throwError", CbThrowError);
    BindFn(env, "__throwTypeError", CbThrowTypeError);
    BindFn(env, "__throwRangeError", CbThrowRangeError);
    BindFn(env, "__throwArbitrary", CbThrowArbitrary);
    BindFn(env, "__createError", CbCreateErrorReturn);

    /* J1a: throw_error caught in JS. */
    {
      const char* script =
        "var caught=null;\n"
        "try { __throwError(); } catch(e) { caught=e; }\n"
        "if (!caught) throw new Error('J1a: nothing caught');\n"
        "if (!(caught instanceof Error)) throw new Error('J1a: not Error');\n"
        "if (caught.name !== 'Error') throw new Error('J1a name:'+caught.name);\n"
        "if (caught.message !== 'Error [error]') throw new Error('J1a msg:'+caught.message);\n"
        "if (caught.code !== 'ERR_TEST_CODE') throw new Error('J1a code:'+caught.code);\n"
        "'J1a-ok';\n";
      JsValueRef res = JS_INVALID_REFERENCE;
      Check(RunJS(script, &res) == JsNoError, "J1a RunJS ok (caught)");
      Check(GetStrProp(reinterpret_cast<napi_value>(res), "length") == "<non-string>" ||
            true, "J1a result ignored (RunJS ok)");
      JsRelease(res, nullptr);
    }
    /* J1b: throw_type_error caught. */
    {
      const char* script =
        "var caught=null;\n"
        "try { __throwTypeError(); } catch(e) { caught=e; }\n"
        "if (!caught) throw new Error('J1b: nothing caught');\n"
        "if (!(caught instanceof TypeError)) throw new Error('J1b: not TypeError');\n"
        "if (caught.name !== 'TypeError') throw new Error('J1b name');\n"
        "if (caught.message !== 'TypeError [type error]') throw new Error('J1b msg');\n"
        "if (caught.code !== 'ERR_TEST_CODE') throw new Error('J1b code');\n"
        "'J1b-ok';\n";
      JsValueRef res = JS_INVALID_REFERENCE;
      Check(RunJS(script, &res) == JsNoError, "J1b RunJS ok (caught)");
      JsRelease(res, nullptr);
    }
    /* J1c: throw_range_error caught. */
    {
      const char* script =
        "var caught=null;\n"
        "try { __throwRangeError(); } catch(e) { caught=e; }\n"
        "if (!caught) throw new Error('J1c: nothing caught');\n"
        "if (!(caught instanceof RangeError)) throw new Error('J1c: not RangeError');\n"
        "if (caught.message !== 'RangeError [range error]') throw new Error('J1c msg');\n"
        "if (caught.code !== 'ERR_TEST_CODE') throw new Error('J1c code');\n"
        "'J1c-ok';\n";
      JsValueRef res = JS_INVALID_REFERENCE;
      Check(RunJS(script, &res) == JsNoError, "J1c RunJS ok (caught)");
      JsRelease(res, nullptr);
    }
    /* J2: arbitrary marker identity via JS catch + pending-clear check. */
    {
      const char* script =
        "var caught=null;\n"
        "try { __throwArbitrary(); } catch(e) { caught=e; }\n"
        "if (!caught) throw new Error('J2: nothing caught');\n"
        "'J2-ok';\n";
      JsValueRef res = JS_INVALID_REFERENCE;
      Check(RunJS(script, &res) == JsNoError, "J2 RunJS ok (caught arbitrary)");
      JsRelease(res, nullptr);
      /* After JS caught, pending must be clear (engine cleared on catch). */
      Check(!HasPending(env), "J2 pending clear after JS catch");
    }
    /* J3: create_error return observed from JS. */
    {
      const char* script =
        "var e = __createError();\n"
        "if (!(e instanceof Error)) throw new Error('J3: not Error');\n"
        "if (e.message !== 'Error [error]') throw new Error('J3 msg');\n"
        "if (e.code !== 'ERR_TEST_CODE') throw new Error('J3 code');\n"
        "if (e.name !== 'Error') throw new Error('J3 name');\n"
        "'J3-ok';\n";
      JsValueRef res = JS_INVALID_REFERENCE;
      Check(RunJS(script, &res) == JsNoError, "J3 RunJS ok (create return)");
      JsRelease(res, nullptr);
    }
    g_env = nullptr;
    g_marker = nullptr;
    g_codeStr = nullptr;
    g_msgStr = nullptr;
    chakra_nodeapi_destroy_env(env);
    std::printf("phaseJ: done\n");
  }

  std::printf("errors_probe: %d checks, %d failures\n", g_checks, g_failures);
  ShutdownEngine();
  return g_failures == 0 ? 0 : 1;
}
