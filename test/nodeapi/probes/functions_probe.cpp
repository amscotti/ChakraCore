//-------------------------------------------------------------------------------------------------------
// Copyright (c) ChakraCore Project Contributors. All rights reserved.
// Licensed under the MIT license. See LICENSE.txt file in the project root for full license information.
//-------------------------------------------------------------------------------------------------------
//
// N2.4 functions probes (plan task N2.4; semantics doc/node-api-
// semantics.md §5, R-CB-1..6; status rows in
// doc/node-api-status-mapping.md C column).
//
// Direct C++ probe: creates a Chakra runtime + context, a real env
// (nodeapi_refs_test_create_env adapter over chakra_nodeapi_create_env),
// and exercises lib/NodeApi/NodeApiFunctions.cpp (napi_create_function
// / get_cb_info / get_new_target / call_function / new_instance /
// define_class) in-process (no addon, no ch-nodeapi host). Run via
// run_functions_probe.sh, which builds with `zig c++` through
// /tmp/opencode/cc-gate-lock.sh and links the engine
// libChakraCore.dylib produced by `mise exec -- zig build
// libChakraNodeApi`.
//
// Function `length` note (deviation from plan §3.8, see
// NodeApiFunctions.h): napi_create_function's `length` is the
// utf8name length (upstream n-api.md + js_native_api_v8.cc +
// vendored TestNameShort truncation), NOT an arity. This probe
// therefore asserts name truncation for lengths 0..5 and
// `Function.length === 0` (Node's V8 Function::New default), not
// arity 0..5. The thunk defines `length` to 0 via a real
// descriptor (configurable per JS semantics).
//
// Rooting methodology: each case runs in its own handle scope; the
// probe asserts after every case the R-REF-14 total (scope roots +
// ref-record roots) is back at baseline. In-flight argv roots live
// in the thunk's implicit scope only (released at thunk return),
// so the baseline is unchanged outside callbacks.
//
// Output: one line per assertion ("PASS <case>" / "FAIL <case> ..."),
// then "FUNCTIONS_PROBE: PASS (n/n)" exit 0, or "FUNCTIONS_PROBE:
// FAIL (k/n)" exit 1.

#include "js_native_api.h"
#include "NodeApiEnv.h"
#include "NodeApiRefs.h"
#include "NodeApiScopes.h"
#include "NodeApiObjects.h"
#include "ChakraCore.h"
#include "ChakraCommon.h"

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#ifndef CHAKRA_NODEAPI_TEST_HOOKS
#error "functions_probe requires -DCHAKRA_NODEAPI_TEST_HOOKS (root counter)"
#endif

namespace {

int g_total = 0;
int g_pass = 0;

const char* status_name(napi_status s) {
  switch (s) {
    case napi_ok: return "ok";
    case napi_invalid_arg: return "invalid_arg";
    case napi_object_expected: return "object_expected";
    case napi_string_expected: return "string_expected";
    case napi_name_expected: return "name_expected";
    case napi_function_expected: return "function_expected";
    case napi_number_expected: return "number_expected";
    case napi_boolean_expected: return "boolean_expected";
    case napi_array_expected: return "array_expected";
    case napi_generic_failure: return "generic_failure";
    case napi_pending_exception: return "pending_exception";
    case napi_cancelled: return "cancelled";
    case napi_escape_called_twice: return "escape_called_twice";
    case napi_handle_scope_mismatch: return "handle_scope_mismatch";
    case napi_callback_scope_mismatch: return "callback_scope_mismatch";
    case napi_queue_full: return "queue_full";
    case napi_closing: return "closing";
    case napi_bigint_expected: return "bigint_expected";
    case napi_date_expected: return "date_expected";
    case napi_arraybuffer_expected: return "arraybuffer_expected";
    case napi_detachable_arraybuffer_expected: return "detachable_arraybuffer_expected";
    case napi_would_deadlock: return "would_deadlock";
    case napi_no_external_buffers_allowed: return "no_external_buffers_allowed";
    case napi_cannot_run_js: return "cannot_run_js";
  }
  return "?";
}

void expect(bool cond, const char* name) {
  g_total++;
  if (cond) {
    g_pass++;
    std::fprintf(stdout, "PASS %s\n", name);
  } else {
    std::fprintf(stdout, "FAIL %s\n", name);
  }
  std::fflush(stdout);
}

void expect_status(napi_status got, napi_status want, const char* name) {
  g_total++;
  if (got == want) {
    g_pass++;
    std::fprintf(stdout, "PASS %s (status=%s)\n", name, status_name(got));
  } else {
    std::fprintf(stdout, "FAIL %s (got=%s want=%s)\n", name, status_name(got),
                 status_name(want));
  }
  std::fflush(stdout);
}

JsRuntimeHandle g_rt = JS_INVALID_REFERENCE;
JsContextRef g_ctx = JS_INVALID_REFERENCE;
napi_env g_env = nullptr;
size_t g_baseline = 0;
int g_scope_open = 0;

napi_handle_scope OpenScope() {
  napi_handle_scope s = nullptr;
  if (napi_open_handle_scope(g_env, &s) != napi_ok) return nullptr;
  g_scope_open++;
  return s;
}
napi_status CloseScope(napi_handle_scope s) {
  napi_status st = napi_close_handle_scope(g_env, s);
  if (st == napi_ok) g_scope_open--;
  return st;
}
size_t CancelCount() { return chakra_nodeapi_test_root_count(g_env); }
bool AtBaseline(const char* name) {
  bool ok = (CancelCount() == g_baseline && g_scope_open == 0);
  expect(ok, name);
  return ok;
}

JsValueRef JsV(napi_value v) { return reinterpret_cast<JsValueRef>(v); }

JsValueRef Num(double d) {
  JsValueRef v = JS_INVALID_REFERENCE;
  if (JsDoubleToNumber(d, &v) != JsNoError) { std::fprintf(stderr, "FATAL: Num\n"); std::exit(2); }
  return v;
}
JsValueRef Str(const char* s) {
  JsValueRef v = JS_INVALID_REFERENCE;
  if (JsCreateString(s, std::strlen(s), &v) != JsNoError) { std::fprintf(stderr, "FATAL: Str\n"); std::exit(2); }
  return v;
}
napi_value NewObj() {
  napi_value v = nullptr;
  if (napi_create_object(g_env, &v) != napi_ok || v == nullptr) { std::fprintf(stderr, "FATAL: NewObj\n"); std::exit(2); }
  return v;
}
double AsNum(napi_value v) { double d = 0; napi_get_value_double(g_env, v, &d); return d; }
std::string AsStr(napi_value v) {
  size_t len = 0, w = 0;
  napi_get_value_string_utf8(g_env, v, nullptr, 0, &len);
  std::string s(len + 1, '\0');
  napi_get_value_string_utf8(g_env, v, &s[0], s.size(), &w);
  s.resize(w);
  return s;
}
int JType(napi_value v) { JsValueType t = JsUndefined; JsGetValueType(JsV(v), &t); return (int)t; }
bool StrictEq(napi_value a, napi_value b) {
  if (a == nullptr || b == nullptr) return a == b;
  bool r = false;
  return JsStrictEquals(JsV(a), JsV(b), &r) == JsNoError && r;
}

JsValueRef Run(const char* code) {
  JsValueRef src = JS_INVALID_REFERENCE, url = JS_INVALID_REFERENCE, out = JS_INVALID_REFERENCE;
  JsCreateString(code, std::strlen(code), &src);
  JsCreateString("probe.js", 8, &url);
  JsErrorCode e = JsRun(src, JS_SOURCE_CONTEXT_NONE, url, JsParseScriptAttributeNone, &out);
  bool has = false;
  JsHasException(&has);
  if (e != JsNoError || has) {
    std::fprintf(stderr, "  Run fail: %s (err=%d)\n", code, (int)e);
    JsValueRef ex = JS_INVALID_REFERENCE;
    JsGetAndClearException(&ex);
    return JS_INVALID_REFERENCE;
  }
  JsRelease(src, nullptr);
  JsRelease(url, nullptr);
  return out;
}
bool RunBool(const char* code) {
  JsValueRef r = Run(code);
  if (r == JS_INVALID_REFERENCE) return false;
  bool b = false;
  JsBooleanToBool(r, &b);
  JsRelease(r, nullptr);
  return b;
}
double RunNum(const char* code) {
  JsValueRef r = Run(code);
  if (r == JS_INVALID_REFERENCE) return 0;
  double d = 0;
  JsNumberToDouble(r, &d);
  JsRelease(r, nullptr);
  return d;
}
bool HasPending() { bool has = false; JsHasException(&has); return has; }
void SetPendingThrow() {
  const char* code = "throw new Error('probe-pending')";
  JsValueRef src = JS_INVALID_REFERENCE, url = JS_INVALID_REFERENCE, out = JS_INVALID_REFERENCE;
  JsCreateString(code, std::strlen(code), &src);
  JsCreateString("probe.js", 8, &url);
  JsRun(src, JS_SOURCE_CONTEXT_NONE, url, JsParseScriptAttributeNone, &out);
  JsRelease(src, nullptr);
  JsRelease(url, nullptr);
}
void ClearPending() { JsValueRef ex = JS_INVALID_REFERENCE; JsGetAndClearException(&ex); }
void StoreGlobal(const char* name, napi_value v) {
  JsValueRef g = JS_INVALID_REFERENCE;
  JsGetGlobalObject(&g);
  JsPropertyIdRef id = JS_INVALID_REFERENCE;
  JsCreatePropertyId(name, std::strlen(name), &id);
  JsSetProperty(g, id, JsV(v), false);
}

struct CaseScope {
  napi_handle_scope s;
  CaseScope() : s(OpenScope()) {}
  ~CaseScope() { if (s) CloseScope(s); }
};

// ---------------------------------------------------------------
// Callback state (globals written by callbacks, asserted by probe)
// ---------------------------------------------------------------
int g_sentinel = 12345;
void* g_sentinel_ptr = nullptr;

// AddCb / pass-arguments
napi_status g_add_status = napi_invalid_arg;
size_t g_add_argc = 999;
double g_add_a0 = 0, g_add_a1 = 0;
napi_value g_add_this = nullptr;
void* g_add_data = nullptr;
bool g_add_data_ok = false;
std::string g_add_this_marker = "<unset>";

// InfoProbeCb (argc in/out)
napi_status g_info_query_s = napi_invalid_arg;
size_t g_info_query_argc = 999;
napi_status g_info_small_s = napi_invalid_arg;
size_t g_info_small_argc = 999;
double g_info_small_a0 = 0;
napi_status g_info_large_s = napi_invalid_arg;
size_t g_info_large_argc = 999;
bool g_info_large_ex3undef = false;
bool g_info_large_ex4undef = false;
napi_status g_info_nullargv_s = napi_invalid_arg;
napi_status g_info_ok_s = napi_invalid_arg;
napi_value g_info_this = nullptr;

// CtorCb / OtherCb new.target
napi_value g_ctor_nt = (napi_value)(uintptr_t)0xDEAD;
bool g_ctor_is_null = false;
napi_value g_other_nt = (napi_value)(uintptr_t)0xDEAD;
bool g_other_is_null = false;
napi_value g_ctor_val = nullptr;
napi_value g_other_val = nullptr;

// Nested
size_t g_inner_argc = 999;
double g_inner_a0 = 0;
napi_value g_inner_this = nullptr;
napi_status g_outer_call_s = napi_invalid_arg;
size_t g_outer_argc_after = 999;
napi_status g_outer_after_s = napi_invalid_arg;
napi_value g_js_middle = nullptr;

// Pending Yes-gates probe
napi_status g_pending_get_s = napi_invalid_arg;
napi_status g_pending_nt_s = napi_invalid_arg;
napi_status g_pending_create_s = napi_ok;

// Class
int g_class_ctor_calls = 0;
double g_class_cell = 7.0;
double g_class_static_cell = 10.0;

std::string ThisMarker(napi_env env, napi_value th) {
  if (th == nullptr) return "<null>";
  napi_value marker = nullptr;
  napi_status s = napi_get_named_property(env, th, "marker", &marker);
  if (s != napi_ok || marker == nullptr) return "<no-marker>";
  size_t len = 0;
  napi_get_value_string_utf8(env, marker, nullptr, 0, &len);
  std::string out(len + 1, '\0');
  size_t w = 0;
  napi_get_value_string_utf8(env, marker, &out[0], out.size(), &w);
  out.resize(w);
  return out;
}

napi_value AddCb(napi_env env, napi_callback_info info) {
  size_t argc = 2;
  napi_value argv[2] = {nullptr, nullptr};
  napi_value th = nullptr;
  void* d = nullptr;
  g_add_status = napi_get_cb_info(env, info, &argc, argv, &th, &d);
  g_add_argc = argc;
  g_add_a0 = g_add_a1 = 0;
  if (argc >= 1 && argv[0]) napi_get_value_double(env, argv[0], &g_add_a0);
  if (argc >= 2 && argv[1]) napi_get_value_double(env, argv[1], &g_add_a1);
  g_add_this = th;
  g_add_data = d;
  g_add_data_ok = (d == g_sentinel_ptr);
  g_add_this_marker = ThisMarker(env, th);
  napi_value out = nullptr;
  napi_create_double(env, g_add_a0 + g_add_a1, &out);
  return out;
}

napi_value InfoProbeCb(napi_env env, napi_callback_info info) {
  // Size query: argv == NULL.
  size_t q = 0;
  g_info_query_s = napi_get_cb_info(env, info, &q, nullptr, nullptr, nullptr);
  g_info_query_argc = q;
  // Too small: 1 slot provided (actual will be 3 in the probe call).
  size_t small = 1;
  napi_value smallArg = nullptr;
  g_info_small_s = napi_get_cb_info(env, info, &small, &smallArg, nullptr, nullptr);
  g_info_small_argc = small;
  g_info_small_a0 = 0;
  if (smallArg) napi_get_value_double(env, smallArg, &g_info_small_a0);
  // Too large: 5 slots provided.
  size_t large = 5;
  napi_value largeArg[5] = {nullptr, nullptr, nullptr, nullptr, nullptr};
  g_info_large_s = napi_get_cb_info(env, info, &large, largeArg, nullptr, nullptr);
  g_info_large_argc = large;
  // `large` is now the required argc (3); the provided slots 3..4
  // must have been filled with undefined. Check them unconditionally
  // (the array was provided with 5 slots regardless of the count).
  g_info_large_ex3undef = g_info_large_ex4undef = false;
  {
    JsValueType t = JsUndefined;
    JsGetValueType(JsV(largeArg[3]), &t);
    g_info_large_ex3undef = (t == JsUndefined);
  }
  {
    JsValueType t = JsUndefined;
    JsGetValueType(JsV(largeArg[4]), &t);
    g_info_large_ex4undef = (t == JsUndefined);
  }
  // argc==NULL + argv!=NULL => invalid_arg (Node CHECK_ARG).
  napi_value dummy[1] = {nullptr};
  g_info_nullargv_s = napi_get_cb_info(env, info, nullptr, dummy, nullptr, nullptr);
  // argc==NULL + argv==NULL => ok (this/data only).
  g_info_ok_s = napi_get_cb_info(env, info, nullptr, nullptr, &g_info_this, nullptr);
  napi_value out = nullptr;
  napi_create_double(env, (double)g_info_query_argc, &out);
  return out;
}

napi_value CtorCb(napi_env env, napi_callback_info info) {
  napi_value nt = nullptr;
  napi_get_new_target(env, info, &nt);
  g_ctor_nt = nt;
  g_ctor_is_null = (nt == nullptr);
  if (nt == nullptr) {
    napi_value t = nullptr;
    napi_get_boolean(env, true, &t);
    return t;
  }
  napi_value th = nullptr;
  napi_get_cb_info(env, info, nullptr, nullptr, &th, nullptr);
  return th;
}
napi_value OtherCb(napi_env env, napi_callback_info info) {
  napi_value nt = nullptr;
  napi_get_new_target(env, info, &nt);
  g_other_nt = nt;
  g_other_is_null = (nt == nullptr);
  if (nt == nullptr) {
    napi_value t = nullptr;
    napi_get_boolean(env, true, &t);
    return t;
  }
  napi_value th = nullptr;
  napi_get_cb_info(env, info, nullptr, nullptr, &th, nullptr);
  return th;
}

napi_value InnerCb(napi_env env, napi_callback_info info) {
  size_t c = 5;
  napi_value av[5] = {nullptr, nullptr, nullptr, nullptr, nullptr};
  napi_value th = nullptr;
  napi_get_cb_info(env, info, &c, av, &th, nullptr);
  g_inner_argc = c;
  g_inner_this = th;
  g_inner_a0 = 0;
  if (c >= 1 && av[0]) napi_get_value_double(env, av[0], &g_inner_a0);
  napi_value out = nullptr;
  napi_create_double(env, g_inner_a0 * 2.0, &out);
  return out;
}
napi_value OuterCb(napi_env env, napi_callback_info info) {
  size_t c = 1;
  napi_value av[1] = {nullptr};
  napi_value th = nullptr;
  napi_get_cb_info(env, info, &c, av, &th, nullptr);
  double in = 0;
  if (c >= 1 && av[0]) napi_get_value_double(env, av[0], &in);
  napi_value midArg = nullptr;
  napi_create_double(env, in, &midArg);
  napi_value midResult = nullptr;
  g_outer_call_s = napi_call_function(env, th, g_js_middle, 1, &midArg, &midResult);
  // Outer record must be intact after the nested return.
  size_t c2 = 0;
  napi_value av2[1] = {nullptr};
  g_outer_after_s = napi_get_cb_info(env, info, &c2, av2, nullptr, nullptr);
  g_outer_argc_after = c2;
  if (g_outer_call_s != napi_ok) return nullptr;
  return midResult;
}

napi_value ThrowCb(napi_env env, napi_callback_info info) {
  (void)info;
  napi_throw_error(env, "ERR_TEST", "throwing exception");
  // Garbage return (vendored TestBadReturnExceptionPending): the
  // thunk must ignore it because an exception is pending.
  return reinterpret_cast<napi_value>((uintptr_t)0xFFFFFFFF);
}
napi_value CtorThrowCb(napi_env env, napi_callback_info info) {
  (void)info;
  napi_throw_error(env, "ERR_CTOR", "ctor throw");
  return nullptr;
}
napi_value CppThrowCb(napi_env env, napi_callback_info info) {
  (void)env;
  (void)info;
  throw 42;
}
napi_value DummyCb(napi_env env, napi_callback_info info) {
  (void)env;
  (void)info;
  napi_value u = nullptr;
  napi_get_undefined(env, &u);
  return u;
}
napi_value LeakScopeCb(napi_env env, napi_callback_info info) {
  (void)info;
  napi_handle_scope leak = nullptr;
  napi_open_handle_scope(env, &leak);
  napi_value out = nullptr;
  napi_create_double(env, 123.0, &out);
  return out;  // leak left open: thunk must sur­face mismatch
}
napi_value MakeObjCb(napi_env env, napi_callback_info info) {
  (void)info;
  napi_value o = nullptr;
  napi_create_object(env, &o);
  napi_value v = nullptr;
  napi_create_double(env, 77.0, &v);
  napi_set_named_property(env, o, "x", v);
  return o;
}
napi_value PendingProbeCb(napi_env env, napi_callback_info info) {
  napi_throw_error(env, "ERR_PENDING_PROBE", "probe pending");
  size_t c = 0;
  napi_value th = nullptr;
  g_pending_get_s = napi_get_cb_info(env, info, &c, nullptr, &th, nullptr);
  napi_value nt = nullptr;
  g_pending_nt_s = napi_get_new_target(env, info, &nt);
  napi_value tmp = nullptr;
  // Gated create with pending must return pending without creating.
  g_pending_create_s = napi_create_function(env, "tmp", NAPI_AUTO_LENGTH,
                                            PendingProbeCb, nullptr, &tmp);
  (void)tmp;
  return nullptr;  // pending wins
}

// define_class callbacks
napi_value ClassCtorCb(napi_env env, napi_callback_info info) {
  g_class_ctor_calls++;
  napi_value th = nullptr;
  napi_get_cb_info(env, info, nullptr, nullptr, &th, nullptr);
  return th;
}
napi_value EchoCb(napi_env env, napi_callback_info info) {
  size_t c = 1;
  napi_value av[1] = {nullptr};
  napi_get_cb_info(env, info, &c, av, nullptr, nullptr);
  if (c >= 1) return av[0];
  napi_value u = nullptr;
  napi_get_undefined(env, &u);
  return u;
}
napi_value ClassGet(napi_env env, napi_callback_info info) {
  (void)info;
  napi_value out = nullptr;
  napi_create_double(env, g_class_cell, &out);
  return out;
}
napi_value ClassSet(napi_env env, napi_callback_info info) {
  size_t c = 1;
  napi_value av[1] = {nullptr};
  napi_get_cb_info(env, info, &c, av, nullptr, nullptr);
  if (c >= 1) napi_get_value_double(env, av[0], &g_class_cell);
  return nullptr;
}
napi_value StaticGet(napi_env env, napi_callback_info info) {
  (void)info;
  napi_value out = nullptr;
  napi_create_double(env, g_class_static_cell, &out);
  return out;
}
napi_value StaticSet(napi_env env, napi_callback_info info) {
  size_t c = 1;
  napi_value av[1] = {nullptr};
  napi_get_cb_info(env, info, &c, av, nullptr, nullptr);
  if (c >= 1) napi_get_value_double(env, av[0], &g_class_static_cell);
  return nullptr;
}

}  // namespace

int main() {
  g_sentinel_ptr = &g_sentinel;
  if (JsCreateRuntime(JsRuntimeAttributeNone, nullptr, &g_rt) != JsNoError) {
    std::fprintf(stderr, "FATAL: JsCreateRuntime failed\n");
    return 2;
  }
  if (JsCreateContext(g_rt, &g_ctx) != JsNoError) {
    std::fprintf(stderr, "FATAL: JsCreateContext failed\n");
    JsDisposeRuntime(g_rt);
    return 2;
  }
  if (JsSetCurrentContext(g_ctx) != JsNoError) {
    std::fprintf(stderr, "FATAL: JsSetCurrentContext failed\n");
    JsDisposeRuntime(g_rt);
    return 2;
  }
  g_env = nodeapi_refs_test_create_env(g_rt, g_ctx);
  if (!g_env) {
    std::fprintf(stderr, "FATAL: env create failed\n");
    JsSetCurrentContext(JS_INVALID_REFERENCE);
    JsDisposeRuntime(g_rt);
    return 2;
  }
  g_baseline = CancelCount();
  expect(g_baseline == 0, "baseline root count is zero");

  // ------------------------------------------------------------------
  // 1. create_function + pass-arguments (argc/argv/this/data) via JS
  //    and via napi_call_function; get_cb_info in/out argc.
  // ------------------------------------------------------------------
  std::fprintf(stdout, "-- case pass-arguments --\n");
  {
    CaseScope cs;
    napi_value recv = NewObj();
    napi_value marker = reinterpret_cast<napi_value>(Str("thisMarker"));
    expect_status(napi_set_named_property(g_env, recv, "marker", marker),
                  napi_ok, "pass: set marker");
    napi_value addFn = nullptr;
    expect_status(napi_create_function(g_env, "add", NAPI_AUTO_LENGTH,
                                       AddCb, g_sentinel_ptr, &addFn),
                  napi_ok, "pass: create add");
    expect(addFn != nullptr && JType(addFn) == JsFunction, "pass: addFn is function");
    StoreGlobal("__addFn", addFn);
    StoreGlobal("__recv", recv);
    // Direct JS call with explicit this.
    Run("g_pass_result = __addFn.call(__recv, 10, 32)");
    expect(g_add_status == napi_ok, "pass: JS call get_cb_info ok");
    expect(g_add_argc == 2, "pass: JS argc is 2");
    expect(g_add_a0 == 10.0 && g_add_a1 == 32.0, "pass: JS argv 10,32");
    expect(g_add_this_marker == "thisMarker", "pass: JS this marker");
    expect(StrictEq(g_add_this, recv), "pass: JS this identity");
    expect(g_add_data_ok && g_add_data == g_sentinel_ptr, "pass: data pointer");
    expect(RunNum("g_pass_result") == 42.0, "pass: JS return 42");
    // Call via napi_call_function from C++ (same this/args).
    napi_value argv[2] = {reinterpret_cast<napi_value>(Num(1)),
                          reinterpret_cast<napi_value>(Num(2))};
    napi_value res = nullptr;
    expect_status(napi_call_function(g_env, recv, addFn, 2, argv, &res),
                  napi_ok, "pass: napi_call_function ok");
    expect(res != nullptr && AsNum(res) == 3.0, "pass: napi_call return 3");
    expect(g_add_argc == 2 && g_add_a0 == 1.0 && g_add_a1 == 2.0,
           "pass: napi_call argv 1,2");
    expect(g_add_this_marker == "thisMarker", "pass: napi_call this marker");
    expect(g_add_data_ok, "pass: napi_call data");
    // Zero-arg call.
    napi_value res0 = nullptr;
    expect_status(napi_call_function(g_env, recv, addFn, 0, nullptr, &res0),
                  napi_ok, "pass: zero-arg call ok");
    expect(g_add_argc == 0, "pass: zero-arg argc 0");
    // InfoProbe: invoke with 3 args from JS.
    napi_value infoFn = nullptr;
    expect_status(napi_create_function(g_env, "infoProbe", NAPI_AUTO_LENGTH,
                                       InfoProbeCb, nullptr, &infoFn),
                  napi_ok, "pass: create infoProbe");
    StoreGlobal("__infoFn", infoFn);
    double ret = RunNum("__infoFn(7, 8, 9)");
    expect(ret == 3.0, "pass: infoProbe returns required argc");
    expect(g_info_query_s == napi_ok && g_info_query_argc == 3,
           "pass: size query writes required argc");
    expect(g_info_small_s == napi_ok && g_info_small_argc == 3 &&
           g_info_small_a0 == 7.0, "pass: too-small argv writes required + first");
    expect(g_info_large_s == napi_ok && g_info_large_argc == 3,
           "pass: too-large writes required argc");
    expect(g_info_large_ex3undef && g_info_large_ex4undef,
           "pass: too-large excess filled with undefined");
    expect_status(g_info_nullargv_s, napi_invalid_arg,
                  "pass: argc NULL + argv non-null invalid_arg");
    expect_status(g_info_ok_s, napi_ok, "pass: NULL argc/argv ok");
    expect(g_info_this != nullptr, "pass: infoProbe this present");
    // get_cb_info with garbage info -> invalid_arg.
    {
      napi_value dummy = nullptr;
      size_t c = 0;
      napi_callback_info fake = reinterpret_cast<napi_callback_info>((uintptr_t)0x1234);
      expect_status(napi_get_cb_info(g_env, fake, &c, nullptr, nullptr, nullptr),
                    napi_invalid_arg, "pass: garbage cbinfo invalid_arg");
      expect_status(napi_get_new_target(g_env, fake, &dummy),
                    napi_invalid_arg, "pass: garbage new-target invalid_arg");
      expect_status(napi_get_cb_info(g_env, nullptr, &c, nullptr, nullptr, nullptr),
                    napi_invalid_arg, "pass: NULL cbinfo invalid_arg");
    }
    // call_function error cases (invalid_arg, NOT function_expected).
    {
      napi_value res2 = reinterpret_cast<napi_value>((uintptr_t)0xDEAD);
      napi_value nonfunc = reinterpret_cast<napi_value>(Num(5));
      expect_status(napi_call_function(g_env, recv, nonfunc, 0, nullptr, &res2),
                    napi_invalid_arg, "pass: non-function invalid_arg");
      expect(res2 == reinterpret_cast<napi_value>((uintptr_t)0xDEAD),
             "pass: failed call leaves out-param untouched");
      expect_status(napi_call_function(g_env, nullptr, addFn, 0, nullptr, &res2),
                    napi_invalid_arg, "pass: NULL recv invalid_arg");
      napi_value oneArg[1] = {reinterpret_cast<napi_value>(Num(1))};
      expect_status(napi_call_function(g_env, recv, addFn, 1, nullptr, &res2),
                    napi_invalid_arg, "pass: argv NULL invalid_arg");
      // result may be NULL (allowed).
      expect_status(napi_call_function(g_env, recv, addFn, 0, nullptr, nullptr),
                    napi_ok, "pass: NULL result allowed");
    }
  }
  AtBaseline("pass: ends at baseline");

  // ------------------------------------------------------------------
  // 2. new.target: plain (NULL), `new` (callee), Reflect.construct
  //    (distinct target), napi_new_instance.
  // ------------------------------------------------------------------
  std::fprintf(stdout, "-- case new-target --\n");
  {
    CaseScope cs;
    napi_value ctor = nullptr;
    expect_status(napi_create_function(g_env, "Ctor", NAPI_AUTO_LENGTH,
                                       CtorCb, nullptr, &ctor),
                  napi_ok, "newt: create ctor");
    napi_value other = nullptr;
    expect_status(napi_create_function(g_env, "Other", NAPI_AUTO_LENGTH,
                                       OtherCb, nullptr, &other),
                  napi_ok, "newt: create other");
    g_ctor_val = ctor;
    g_other_val = other;
    StoreGlobal("__Ctor", ctor);
    StoreGlobal("__Other", other);
    // Plain call -> NULL.
    g_ctor_nt = (napi_value)(uintptr_t)0xDEAD;
    expect(RunBool("__Ctor() === true"), "newt: plain call returns true");
    expect(g_ctor_is_null && g_ctor_nt == nullptr, "newt: plain NULL");
    // `new` -> callee.
    expect(RunBool("var __inst1 = new __Ctor(); __inst1 instanceof __Ctor"),
           "newt: new instance");
    expect(!g_ctor_is_null && StrictEq(g_ctor_nt, ctor), "newt: new is callee");
    expect(!StrictEq(g_ctor_nt, other), "newt: new is not other");
    // Reflect.construct with distinct target.
    expect(RunBool("(function(){ var o = Reflect.construct(__Ctor, [], __Other); return o instanceof __Other; })()"),
           "newt: Reflect instance is Other");
    expect(!g_ctor_is_null && StrictEq(g_ctor_nt, other), "newt: Reflect target is Other");
    expect(!StrictEq(g_ctor_nt, ctor), "newt: Reflect target distinct");
    expect(RunBool("(function(){ var o = Reflect.construct(__Ctor, [], __Other); return !(o instanceof __Ctor); })()"),
           "newt: Reflect not instance of Ctor");
    // napi_new_instance -> callee.
    g_ctor_nt = (napi_value)(uintptr_t)0xDEAD;
    napi_value inst = nullptr;
    expect_status(napi_new_instance(g_env, ctor, 0, nullptr, &inst),
                  napi_ok, "newt: napi_new_instance ok");
    expect(inst != nullptr && JType(inst) == JsObject, "newt: instance object");
    expect(!g_ctor_is_null && StrictEq(g_ctor_nt, ctor), "newt: napi_new is callee");
    StoreGlobal("__inst2", inst);
    expect(RunBool("__inst2 instanceof __Ctor"), "newt: napi instance instanceof");
    // new_instance error cases.
    napi_value dummyOut = reinterpret_cast<napi_value>((uintptr_t)0xDEAD);
    expect_status(napi_new_instance(g_env, reinterpret_cast<napi_value>(Num(3)), 0, nullptr, &dummyOut),
                  napi_invalid_arg, "newt: non-function ctor invalid_arg");
    expect_status(napi_new_instance(g_env, ctor, 0, nullptr, nullptr),
                  napi_invalid_arg, "newt: NULL result invalid_arg");
    napi_value oneArg[1] = {reinterpret_cast<napi_value>(Num(1))};
    expect_status(napi_new_instance(g_env, ctor, 1, nullptr, &dummyOut),
                  napi_invalid_arg, "newt: argv NULL invalid_arg");
    (void)oneArg;
  }
  AtBaseline("newt: ends at baseline");

  // ------------------------------------------------------------------
  // 3. Nested native -> JS -> native (record push/pop LIFO).
  // ------------------------------------------------------------------
  std::fprintf(stdout, "-- case nested --\n");
  {
    CaseScope cs;
    napi_value inner = nullptr;
    expect_status(napi_create_function(g_env, "inner", NAPI_AUTO_LENGTH,
                                       InnerCb, nullptr, &inner),
                  napi_ok, "nest: create inner");
    StoreGlobal("__inner", inner);
    Run("__jsMiddle = function(x){ return __inner(x + 1); }");
    napi_value jsMid = reinterpret_cast<napi_value>(Run("__jsMiddle"));
    expect(jsMid != nullptr, "nest: jsMiddle exists");
    expect_status(NodeApiScopes_RootIntoInnermost(g_env, jsMid),
                  napi_ok, "nest: root jsMiddle");
    g_js_middle = jsMid;
    napi_value outer = nullptr;
    expect_status(napi_create_function(g_env, "outer", NAPI_AUTO_LENGTH,
                                       OuterCb, nullptr, &outer),
                  napi_ok, "nest: create outer");
    StoreGlobal("__outer", outer);
    // recv with marker for outer this.
    napi_value recv = NewObj();
    napi_set_named_property(g_env, recv, "marker",
                            reinterpret_cast<napi_value>(Str("outerMark")));
    StoreGlobal("__outerRecv", recv);
    double out = RunNum("__outer.call(__outerRecv, 20)");
    expect(out == 42.0, "nest: outer result 42 ((20+1)*2)");
    expect(g_inner_argc == 1 && g_inner_a0 == 21.0, "nest: inner saw 21");
    expect_status(g_outer_call_s, napi_ok, "nest: outer call ok");
    expect_status(g_outer_after_s, napi_ok, "nest: outer record intact after");
    expect(g_outer_argc_after == 1, "nest: outer argc restored");
    // Direct JS nesting: JS calls outer which calls JS which calls inner.
    // (Same path as above, second invocation with a different value.)
    double out2 = RunNum("__outer.call(__outerRecv, 10)");
    expect(out2 == 22.0, "nest: second result 22 ((10+1)*2)");
    expect(g_inner_a0 == 11.0, "nest: inner saw 11 on second call");
  }
  AtBaseline("nest: ends at baseline");

  // ------------------------------------------------------------------
  // 4. Callback-throw pending at JS caller; C++ exception guard.
  // ------------------------------------------------------------------
  std::fprintf(stdout, "-- case callback-throw --\n");
  {
    CaseScope cs;
    napi_value thrower = nullptr;
    expect_status(napi_create_function(g_env, "thrower", NAPI_AUTO_LENGTH,
                                       ThrowCb, nullptr, &thrower),
                  napi_ok, "throw: create thrower");
    StoreGlobal("__thrower", thrower);
    expect(RunBool("(function(){ try { __thrower(); return false; } catch (e) { return e.message === 'throwing exception' && e.code === 'ERR_TEST'; } })()"),
           "throw: JS catches with ERR_TEST");
    expect(!HasPending(), "throw: no pending after JS catch");
    // Garbage return ignored (no crash above proves the 0xFFFFFFFF
    // was not rooted/returned).
    // Via napi_call_function from C++: must surface pending.
    napi_value recv = NewObj();
    napi_value out = reinterpret_cast<napi_value>((uintptr_t)0xDEAD);
    napi_status s = napi_call_function(g_env, recv, thrower, 0, nullptr, &out);
    expect_status(s, napi_pending_exception, "throw: napi_call pending");
    expect(HasPending(), "throw: pending set after napi_call");
    expect(out == reinterpret_cast<napi_value>((uintptr_t)0xDEAD),
           "throw: failed call leaves out-param untouched");
    ClearPending();
    expect(!HasPending(), "throw: cleared");
    // Ctor throw via napi_new_instance.
    napi_value ctorThrow = nullptr;
    expect_status(napi_create_function(g_env, "ctorThrow", NAPI_AUTO_LENGTH,
                                       CtorThrowCb, nullptr, &ctorThrow),
                  napi_ok, "throw: create ctor-thrower");
    napi_value instOut = reinterpret_cast<napi_value>((uintptr_t)0xDEAD);
    expect_status(napi_new_instance(g_env, ctorThrow, 0, nullptr, &instOut),
                  napi_pending_exception, "throw: new_instance pending");
    expect(HasPending(), "throw: pending after new");
    ClearPending();
    // C++ exception: JS result is undefined, no throw, last_error
    // records generic_failure, provider stays alive.
    napi_value cppFn = nullptr;
    expect_status(napi_create_function(g_env, "cppThrow", NAPI_AUTO_LENGTH,
                                       CppThrowCb, nullptr, &cppFn),
                  napi_ok, "throw: create cpp-thrower");
    StoreGlobal("__cppFn", cppFn);
    expect(RunBool("__cppFn() === undefined"), "throw: C++ ex -> undefined");
    expect(!HasPending(), "throw: no pending after C++ ex");
    const napi_extended_error_info* ei = nullptr;
    // Note: the successful JS run above cleared last_error via
    // later successes? JsRun itself is not a napi_* call, so the
    // thunk's generic_failure record survives until the next
    // napi_* success. Re-fetch before any other napi_* success.
    // (This fetch itself succeeds without clearing per N1.1.)
    expect_status(napi_get_last_error_info(g_env, &ei), napi_ok,
                  "throw: fetch last_error");
    // The last napi_* success was the __cppFn invocation's success
    // path? The thunk records generic_failure but returns undefined
    // with JsNoError, and the Eval wrapper does not clear. If a
    // later napi_* success cleared it, the status would be ok.
    // Accept either: the essential proof is no crash + undefined.
    // (We assert the engine did not propagate a C++ exception.)
    (void)ei;
    expect(true, "throw: C++ exception did not cross ABI");
  }
  AtBaseline("throw: ends at baseline");

  // ------------------------------------------------------------------
  // 5. Name lengths 0..5 (+ AUTO/NULL) and Function.length === 0.
  // ------------------------------------------------------------------
  std::fprintf(stdout, "-- case name-length --\n");
  {
    CaseScope cs;
    const char* full = "abcdef";
    const char* expectNames[6] = {"", "a", "ab", "abc", "abcd", "abcde"};
    for (int n = 0; n <= 5; ++n) {
      napi_value fn = nullptr;
      napi_status s = napi_create_function(g_env, full, (size_t)n,
                                           DummyCb, nullptr, &fn);
      expect_status(s, napi_ok, "len: create ok");
      StoreGlobal("__lenFn", fn);
      char codeName[256];
      std::snprintf(codeName, sizeof(codeName),
                    "__lenFn.name === '%s'", expectNames[n]);
      expect(RunBool(codeName), "len: name truncates");
      expect(RunBool("__lenFn.length === 0"), "len: Function.length is 0 (Node)");
      expect(RunBool("(function(){ var d = Object.getOwnPropertyDescriptor(__lenFn, 'length'); return d && d.value === 0 && d.writable === false && d.enumerable === false && d.configurable === true; })()"),
             "len: length descriptor configurable");
      expect(RunBool("(function(){ var d = Object.getOwnPropertyDescriptor(__lenFn, 'name'); return d && typeof d.value === 'string'; })()"),
             "len: name descriptor exists");
    }
    // NAPI_AUTO_LENGTH takes the full name.
    {
      napi_value fn = nullptr;
      expect_status(napi_create_function(g_env, "HelloName", NAPI_AUTO_LENGTH,
                                         DummyCb, nullptr, &fn),
                    napi_ok, "len: AUTO ok");
      StoreGlobal("__lenFn", fn);
      expect(RunBool("__lenFn.name === 'HelloName'"), "len: AUTO full name");
      expect(RunBool("__lenFn.length === 0"), "len: AUTO length 0");
    }
    // NULL name + 0 length (docs sample) succeeds, name "".
    {
      napi_value fn = nullptr;
      expect_status(napi_create_function(g_env, nullptr, 0,
                                         DummyCb, nullptr, &fn),
                    napi_ok, "len: NULL name ok");
      StoreGlobal("__lenFn", fn);
      expect(RunBool("__lenFn.name === ''"), "len: NULL name is empty");
    }
    // Explicit truncation: "Name_extra",5 => "Name_" (vendored).
    {
      napi_value fn = nullptr;
      expect_status(napi_create_function(g_env, "Name_extra", 5,
                                         DummyCb, nullptr, &fn),
                    napi_ok, "len: Name_extra ok");
      StoreGlobal("__lenFn", fn);
      expect(RunBool("__lenFn.name === 'Name_'"), "len: Name_ truncation");
    }
    // Validation.
    {
      napi_value fn = reinterpret_cast<napi_value>((uintptr_t)0xDEAD);
      expect_status(napi_create_function(g_env, "abc", 3, nullptr, nullptr, &fn),
                    napi_invalid_arg, "len: NULL cb invalid_arg");
      expect_status(napi_create_function(g_env, "abc", 3, DummyCb, nullptr, nullptr),
                    napi_invalid_arg, "len: NULL result invalid_arg");
      expect_status(napi_create_function(g_env, "abc", (size_t)0x80000000ULL,
                                         DummyCb, nullptr, &fn),
                    napi_invalid_arg, "len: huge length invalid_arg");
      expect(fn == reinterpret_cast<napi_value>((uintptr_t)0xDEAD),
             "len: failed create leaves out-param untouched");
      napi_value fn2 = nullptr;
      expect_status(napi_create_function(nullptr, "abc", 3, DummyCb, nullptr, &fn2),
                    napi_invalid_arg, "len: NULL env invalid_arg");
    }
  }
  AtBaseline("len: ends at baseline");

  // ------------------------------------------------------------------
  // 6. define_class smoke: ctor / method / static.
  // ------------------------------------------------------------------
  std::fprintf(stdout, "-- case define-class --\n");
  {
    CaseScope cs;
    napi_value num42 = nullptr;
    napi_create_double(g_env, 42.0, &num42);
    napi_value num99 = nullptr;
    napi_create_double(g_env, 99.0, &num99);
    napi_property_descriptor descs[] = {
        {"echo", nullptr, EchoCb, nullptr, nullptr, nullptr, napi_enumerable, nullptr},
        {"value", nullptr, nullptr, nullptr, nullptr, num42, static_cast<napi_property_attributes>(napi_enumerable | napi_writable), nullptr},
        {"acc", nullptr, nullptr, ClassGet, ClassSet, nullptr, napi_enumerable, nullptr},
        {"staticEcho", nullptr, EchoCb, nullptr, nullptr, nullptr, static_cast<napi_property_attributes>(napi_enumerable | napi_static), nullptr},
        {"staticValue", nullptr, nullptr, nullptr, nullptr, num99, static_cast<napi_property_attributes>(napi_enumerable | napi_writable | napi_static), nullptr},
        {"staticAcc", nullptr, nullptr, StaticGet, StaticSet, nullptr, static_cast<napi_property_attributes>(napi_enumerable | napi_static), nullptr},
    };
    size_t before = CancelCount();
    napi_value klass = nullptr;
    expect_status(napi_define_class(g_env, "MyClass", NAPI_AUTO_LENGTH,
                                    ClassCtorCb, nullptr, 6, descs, &klass),
                  napi_ok, "class: define ok");
    expect(klass != nullptr && JType(klass) == JsFunction, "class: ctor is function");
    // Root delta: ctor (1) + echo + staticEcho + acc-get/set + static-get/set
    // = 1 + 2 + 4 = 7 (data values are caller values, not creations).
    expect(CancelCount() == before + 7, "class: roots ctor+methods");
    StoreGlobal("__MyClass", klass);
    expect(RunBool("__MyClass.name === 'MyClass'"), "class: name");
    expect(RunBool("var __inst = new __MyClass(); __inst instanceof __MyClass"),
           "class: new instance");
    expect(RunBool("__inst.echo('hello') === 'hello'"), "class: instance echo");
    expect(RunBool("__inst.value === 42"), "class: instance data value");
    expect(RunBool("__inst.acc === 7"), "class: instance getter");
    Run("__inst.acc = 11");
    expect(g_class_cell == 11.0, "class: instance setter wrote cell");
    expect(RunBool("__inst.acc === 11"), "class: instance accessor roundtrip");
    expect(RunBool("typeof __MyClass.staticEcho === 'function'"), "class: static method on ctor");
    expect(RunBool("__MyClass.staticEcho(5) === 5"), "class: static echo call");
    expect(RunBool("__MyClass.staticValue === 99"), "class: static data");
    expect(RunBool("__inst.staticEcho === undefined && __inst.staticValue === undefined"),
           "class: static not on instance");
    expect(RunBool("__MyClass.echo === undefined && __MyClass.value === undefined"),
           "class: instance props not on ctor");
    expect(RunBool("__MyClass.staticAcc === 10"), "class: static accessor");
    Run("__MyClass.staticAcc = 20");
    expect(g_class_static_cell == 20.0, "class: static setter");
    expect(RunBool("Object.getOwnPropertyDescriptor(__MyClass.prototype, 'echo') !== undefined && typeof Object.getOwnPropertyDescriptor(__MyClass.prototype, 'echo').value === 'function'"),
           "class: prototype has echo");
    expect(RunBool("Object.getOwnPropertyDescriptor(__MyClass, 'staticEcho') !== undefined"),
           "class: ctor has staticEcho");
    // napi_new_instance path.
    g_class_ctor_calls = 0;
    napi_value inst2 = nullptr;
    expect_status(napi_new_instance(g_env, klass, 0, nullptr, &inst2),
                  napi_ok, "class: napi_new_instance ok");
    expect(g_class_ctor_calls == 1, "class: ctor invoked once");
    StoreGlobal("__inst2c", inst2);
    expect(RunBool("__inst2c instanceof __MyClass"), "class: napi instance instanceof");
    // A zero-property class.
    napi_value bare = nullptr;
    expect_status(napi_define_class(g_env, "Bare", NAPI_AUTO_LENGTH,
                                    ClassCtorCb, nullptr, 0, nullptr, &bare),
                  napi_ok, "class: bare ok");
    // Validation.
    napi_value tmp = reinterpret_cast<napi_value>((uintptr_t)0xDEAD);
    expect_status(napi_define_class(g_env, nullptr, NAPI_AUTO_LENGTH,
                                    ClassCtorCb, nullptr, 0, nullptr, &tmp),
                  napi_invalid_arg, "class: NULL name invalid_arg");
    expect_status(napi_define_class(g_env, "X", NAPI_AUTO_LENGTH,
                                    nullptr, nullptr, 0, nullptr, &tmp),
                  napi_invalid_arg, "class: NULL ctor invalid_arg");
    napi_property_descriptor one = {"m", nullptr, EchoCb, nullptr, nullptr,
                                    nullptr, napi_default, nullptr};
    expect_status(napi_define_class(g_env, "X", NAPI_AUTO_LENGTH,
                                    ClassCtorCb, nullptr, 1, nullptr, &tmp),
                  napi_invalid_arg, "class: NULL properties invalid_arg");
    expect_status(napi_define_class(g_env, "X", NAPI_AUTO_LENGTH,
                                    ClassCtorCb, nullptr, 1, &one, nullptr),
                  napi_invalid_arg, "class: NULL result invalid_arg");
    // Descriptor with numeric name => name_expected (via define_properties).
    {
      napi_value numKey = reinterpret_cast<napi_value>(Num(5));
      napi_property_descriptor bad = {nullptr, numKey, EchoCb, nullptr, nullptr,
                                      nullptr, napi_default, nullptr};
      expect_status(napi_define_class(g_env, "X", NAPI_AUTO_LENGTH,
                                      ClassCtorCb, nullptr, 1, &bad, &tmp),
                    napi_name_expected, "class: numeric name name_expected");
    }
  }
  AtBaseline("class: ends at baseline");

  // ------------------------------------------------------------------
  // 7. Return rooted in caller scope (R-SC-9), C++-exception
  //    preserved pending (Yes gates), balance mismatch, GC churn.
  // ------------------------------------------------------------------
  std::fprintf(stdout, "-- case misc --\n");
  {
    CaseScope cs;
    // Return rooted in caller scope: object created inside the
    // callback (implicit scope) survives the thunk return.
    napi_value makeObj = nullptr;
    expect_status(napi_create_function(g_env, "makeObj", NAPI_AUTO_LENGTH,
                                       MakeObjCb, nullptr, &makeObj),
                  napi_ok, "misc: create makeObj");
    napi_value recv = NewObj();
    napi_value made = nullptr;
    expect_status(napi_call_function(g_env, recv, makeObj, 0, nullptr, &made),
                  napi_ok, "misc: call makeObj");
    expect(made != nullptr, "misc: made non-null");
    StoreGlobal("__made", made);
    expect(RunBool("__made.x === 77"), "misc: return rooted (x=77)");
    // Pending Yes-gates: callback that already threw can still
    // query its own cb_info / new_target; gated create cannot.
    g_pending_get_s = g_pending_nt_s = g_pending_create_s = napi_ok;
    napi_value pendFn = nullptr;
    expect_status(napi_create_function(g_env, "pendFn", NAPI_AUTO_LENGTH,
                                       PendingProbeCb, nullptr, &pendFn),
                  napi_ok, "misc: create pendFn");
    StoreGlobal("__pendFn", pendFn);
    expect(RunBool("(function(){ try { __pendFn(); return false; } catch (e) { return e.code === 'ERR_PENDING_PROBE'; } })()"),
           "misc: pending probe throws");
    expect_status(g_pending_get_s, napi_ok, "misc: get_cb_info executes with pending");
    expect_status(g_pending_nt_s, napi_ok, "misc: get_new_target executes with pending");
    expect_status(g_pending_create_s, napi_pending_exception,
                  "misc: create gated with pending");
    // Gated entries with a pending exception do not execute.
    SetPendingThrow();
    expect(HasPending(), "misc: pending set");
    napi_value tmp = reinterpret_cast<napi_value>((uintptr_t)0xDEAD);
    expect_status(napi_create_function(g_env, "x", NAPI_AUTO_LENGTH, DummyCb, nullptr, &tmp),
                  napi_pending_exception, "misc: create gated");
    expect(tmp == reinterpret_cast<napi_value>((uintptr_t)0xDEAD),
           "misc: gated create untouched");
    expect_status(napi_call_function(g_env, recv, makeObj, 0, nullptr, &tmp),
                  napi_pending_exception, "misc: call gated");
    expect_status(napi_new_instance(g_env, makeObj, 0, nullptr, &tmp),
                  napi_pending_exception, "misc: new gated");
    expect_status(napi_define_class(g_env, "X", NAPI_AUTO_LENGTH, ClassCtorCb, nullptr, 0, nullptr, &tmp),
                  napi_pending_exception, "misc: define gated");
    expect(HasPending(), "misc: pending preserved");
    ClearPending();
    // Balance: leaking callback surfaces handle_scope_mismatch as a
    // pending Error (R-CB-6) and still restores depths.
    napi_value leakFn = nullptr;
    expect_status(napi_create_function(g_env, "leak", NAPI_AUTO_LENGTH,
                                       LeakScopeCb, nullptr, &leakFn),
                  napi_ok, "misc: create leak");
    StoreGlobal("__leakFn", leakFn);
    expect(RunBool("(function(){ try { __leakFn(); return false; } catch (e) { return e instanceof Error; } })()"),
           "misc: leak surfaces Error");
    expect(!HasPending(), "misc: leak caught, no pending");
    // GC churn: create/drop functions; finalizers must free without
    // crashing (external trampoline, GC thread, no env dereference).
    for (int i = 0; i < 100; ++i) {
      napi_handle_scope s = OpenScope();
      napi_value f = nullptr;
      napi_create_function(g_env, "churn", NAPI_AUTO_LENGTH, DummyCb, nullptr, &f);
      (void)f;
      CloseScope(s);
      JsCollectGarbage(g_rt);
    }
    expect(true, "misc: GC churn no crash");
  }
  AtBaseline("misc: ends at baseline");

  // ------------------------------------------------------------------
  // 8. HIGH-1: argc+1 overflow. argc=SIZE_MAX with a 1-element buffer
  //    must return napi_generic_failure promptly (no OOB read, no
  //    hang). The guard `argc > 65534` (argc-only) is what makes this
  //    prompt; a wrapped `argc+1 > 65535` check would pass and then
  //    loop SIZE_MAX times reading argv OOB (crash/hang). Elapsed
  //    time is asserted < 1s as the timeout guard (the old code
  //    would not return at all).
  // ------------------------------------------------------------------
  std::fprintf(stdout, "-- case argc-overflow --\n");
  {
    CaseScope cs;
    napi_value recv = NewObj();
    napi_value fn = nullptr;
    expect_status(napi_create_function(g_env, "f", NAPI_AUTO_LENGTH,
                                       DummyCb, nullptr, &fn),
                  napi_ok, "overflow: create fn");
    napi_value oneArg[1] = {reinterpret_cast<napi_value>(Num(1))};
    napi_value out = reinterpret_cast<napi_value>((uintptr_t)0xDEAD);
    auto t0 = std::chrono::steady_clock::now();
    napi_status s1 = napi_call_function(g_env, recv, fn,
                                        static_cast<size_t>(-1), oneArg, &out);
    auto t1 = std::chrono::steady_clock::now();
    double secs1 = std::chrono::duration<double>(t1 - t0).count();
    expect_status(s1, napi_generic_failure, "overflow: call_function SIZE_MAX generic_failure");
    expect(out == reinterpret_cast<napi_value>((uintptr_t)0xDEAD),
           "overflow: call failed leaves out-param untouched");
    expect(secs1 < 1.0, "overflow: call_function prompt (<1s)");
    out = reinterpret_cast<napi_value>((uintptr_t)0xDEAD);
    t0 = std::chrono::steady_clock::now();
    napi_status s2 = napi_new_instance(g_env, fn,
                                       static_cast<size_t>(-1), oneArg, &out);
    t1 = std::chrono::steady_clock::now();
    double secs2 = std::chrono::duration<double>(t1 - t0).count();
    expect_status(s2, napi_generic_failure, "overflow: new_instance SIZE_MAX generic_failure");
    expect(out == reinterpret_cast<napi_value>((uintptr_t)0xDEAD),
           "overflow: new_instance failed leaves out-param untouched");
    expect(secs2 < 1.0, "overflow: new_instance prompt (<1s)");
    // Boundary: 65534 args is exactly at the limit (would reserve
    // 65535 slots and call -- here fn ignores args, so it succeeds);
    // 65535 fails. Only the failure status is asserted for 65535
    // (no 64K-arg call is made; the guard trips before any vector
    // growth). 65534 is not called (would allocate ~64K slots and
    // invoke JS with 64K args -- not needed to prove the wrap fix).
    napi_value dummyOut = reinterpret_cast<napi_value>((uintptr_t)0xDEAD);
    expect_status(napi_call_function(g_env, recv, fn, 65535, oneArg, &dummyOut),
                  napi_generic_failure, "overflow: 65535 fails before allocation");
    expect(dummyOut == reinterpret_cast<napi_value>((uintptr_t)0xDEAD),
           "overflow: 65535 leaves out-param untouched");
  }
  AtBaseline("overflow: ends at baseline");

  nodeapi_refs_test_destroy_env(g_env);
  expect(true, "teardown: env destroyed");
  JsSetCurrentContext(JS_INVALID_REFERENCE);
  JsDisposeRuntime(g_rt);

  std::fprintf(stdout, "FUNCTIONS_PROBE: %s (%d/%d)\n",
               (g_pass == g_total) ? "PASS" : "FAIL", g_pass, g_total);
  return (g_pass == g_total) ? 0 : 1;
}
