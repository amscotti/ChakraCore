//-------------------------------------------------------------------------------------------------------
// Copyright (c) ChakraCore Project Contributors. All rights reserved.
// Licensed under the MIT license. See LICENSE.txt file in the project root for full license information.
//-------------------------------------------------------------------------------------------------------
//
// N2.3 objects probes (plan task N2.3; status rows per
// doc/node-api-status-mapping.md, C column).
//
// Direct C++ probe: creates a Chakra runtime + context, a real env
// (nodeapi_refs_test_create_env adapter over chakra_nodeapi_create_env),
// and exercises every lib/NodeApi/NodeApiObjects.cpp entry in-process
// (no addon, no ch-nodeapi host). Run via run_objects_probe.sh, which
// builds with `zig c++` through /tmp/opencode/cc-gate-lock.sh and links
// the engine libChakraCore.dylib produced by `mise exec -- zig build`.
//
// napi_create_function is the real N2.4 implementation
// (lib/NodeApi/NodeApiFunctions.cpp, JsCreateEnhancedFunction
// + §3.8 thunk). Descriptor method/getter/setter callbacks below
// are intentionally info-oblivious (return constants / count
// invocations, ignoring the now-real cb_info); the full argv/this
// data-flow proof lives in functions_probe.cpp. Scalar/string/symbol *values* are
// built with direct JsRT calls (N2.1's napi_* scalar lane is likewise
// not linked) -- the resulting JsValueRefs are the same pointers the
// N2.1 lane would root.
//
// Rooting methodology: each case runs in its own handle scope; the
// probe asserts after every case the R-REF-14 total (scope roots +
// ref-record roots) is back at baseline, so provider scratch roots
// (JsAddRef pairs, counter-neutral) and scope roots are both
// accounted. Within-scope deltas are asserted exactly where a case
// produces a known number of returned values/functions.
//
// Output: one line per assertion ("PASS <case>" / "FAIL <case> ..."),
// then "OBJECTS_PROBE: PASS (n/n)" exit 0, or "OBJECTS_PROBE: FAIL
// (k/n)" exit 1.

#include "js_native_api.h"
#include "NodeApiEnv.h"
#include "NodeApiRefs.h"
#include "NodeApiScopes.h"
#include "NodeApiObjects.h"
#include "ChakraCore.h"
#include "ChakraCommon.h"

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#ifndef CHAKRA_NODEAPI_TEST_HOOKS
#error "objects_probe requires -DCHAKRA_NODEAPI_TEST_HOOKS (root counter)"
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
JsValueRef Sym(const char* desc) {
  JsValueRef d = JS_INVALID_REFERENCE;
  JsCreateString(desc, std::strlen(desc), &d);
  JsValueRef v = JS_INVALID_REFERENCE;
  if (JsCreateSymbol(d, &v) != JsNoError || v == JS_INVALID_REFERENCE) { std::fprintf(stderr, "FATAL: Sym\n"); std::exit(2); }
  return v;
}
napi_value NewObj() {
  napi_value v = nullptr;
  if (napi_create_object(g_env, &v) != napi_ok || v == nullptr) { std::fprintf(stderr, "FATAL: NewObj\n"); std::exit(2); }
  return v;
}
napi_value NewArr(size_t n) {
  napi_value v = nullptr;
  if (napi_create_array_with_length(g_env, n, &v) != napi_ok || v == nullptr) { std::fprintf(stderr, "FATAL: NewArr\n"); std::exit(2); }
  return v;
}

double AsNum(napi_value v) { double d = 0; JsNumberToDouble(JsV(v), &d); return d; }
bool AsBool(napi_value v) { bool b = false; JsBooleanToBool(JsV(v), &b); return b; }
std::string AsStr(napi_value v) {
  size_t len = 0, w = 0;
  JsCopyString(JsV(v), nullptr, 0, &len);
  std::string s(len + 1, '\0');
  JsCopyString(JsV(v), &s[0], s.size(), &w);
  s.resize(w);
  return s;
}
int JType(napi_value v) { JsValueType t = JsUndefined; JsGetValueType(JsV(v), &t); return (int)t; }
bool StrictEq(napi_value a, napi_value b) {
  bool r = false;
  return JsStrictEquals(JsV(a), JsV(b), &r) == JsNoError && r;
}

// Run a JS snippet; returns its result (unrooted borrow). On error, prints
// and returns JS_INVALID_REFERENCE (caller must clear nothing; exception is
// fetched+cleared here so one failure cannot cascade into the next case).
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

bool HasPending() {
  bool has = false;
  JsHasException(&has);
  return has;
}
void SetPendingThrow() {
  // Raw JsRun: leaves the exception PENDING (Run() would clear it).
  const char* code = "throw new Error('probe-pending')";
  JsValueRef src = JS_INVALID_REFERENCE, url = JS_INVALID_REFERENCE,
             out = JS_INVALID_REFERENCE;
  JsCreateString(code, std::strlen(code), &src);
  JsCreateString("probe.js", 8, &url);
  JsRun(src, JS_SOURCE_CONTEXT_NONE, url, JsParseScriptAttributeNone, &out);
  JsRelease(src, nullptr);
  JsRelease(url, nullptr);
}
void ClearPending() {
  JsValueRef ex = JS_INVALID_REFERENCE;
  JsGetAndClearException(&ex);
}

// Publish/fetch a value under a JS global name (so JS assertions can
// reach C++ objects/symbols with identity intact).
void StoreGlobal(const char* name, napi_value v) {
  JsValueRef g = JS_INVALID_REFERENCE;
  JsGetGlobalObject(&g);
  JsPropertyIdRef id = JS_INVALID_REFERENCE;
  JsCreatePropertyId(name, std::strlen(name), &id);
  JsSetProperty(g, id, JsV(v), false);
}

// ---------------------------------------------------------------
// Descriptor method/accessor callbacks (info-oblivious; they ignore
// the real cb_info the N2.4 thunk passes). Count invocations.
// ---------------------------------------------------------------
int g_method_calls = 0;
int g_getter_calls = 0;
int g_setter_calls = 0;
double g_cell = 7.0;

napi_value MethodCb(napi_env, napi_callback_info) {
  g_method_calls++;
  JsValueRef v = JS_INVALID_REFERENCE;
  JsDoubleToNumber(4242.0, &v);
  return reinterpret_cast<napi_value>(v);
}
napi_value GetterCb(napi_env, napi_callback_info) {
  g_getter_calls++;
  JsValueRef v = JS_INVALID_REFERENCE;
  JsDoubleToNumber(g_cell, &v);
  return reinterpret_cast<napi_value>(v);
}
napi_value SetterCb(napi_env, napi_callback_info) {
  g_setter_calls++;
  return nullptr;  // N2.4 thunk maps NULL return to JS undefined
}

// Extract key list from a names/symbols array napi_value for comparison.
struct Extracted {
  bool is_symbol = false;
  std::string str;
  double num = 0;
};

unsigned ArrLenJs(JsValueRef a) {
  JsPropertyIdRef id = JS_INVALID_REFERENCE;
  JsCreatePropertyId("length", 6, &id);
  JsValueRef len = JS_INVALID_REFERENCE;
  JsGetProperty(a, id, &len);
  double d = 0;
  JsNumberToDouble(len, &d);
  return (unsigned)d;
}

// Run a JS array-builder-free element extraction: returns elements of a JS
// array (from get_all_property_names) as Extracted structs.
std::vector<Extracted> ExtractArray(napi_value arr) {
  std::vector<Extracted> out;
  unsigned n = ArrLenJs(JsV(arr));
  for (unsigned i = 0; i < n; ++i) {
    JsValueRef idx = JS_INVALID_REFERENCE;
    JsDoubleToNumber((double)i, &idx);
    JsValueRef el = JS_INVALID_REFERENCE;
    JsGetIndexedProperty(JsV(arr), idx, &el);
    Extracted e;
    JsValueType t = JsUndefined;
    JsGetValueType(el, &t);
    if (t == JsSymbol) {
      e.is_symbol = true;
      e.str = "<symbol>";
    } else if (t == JsNumber) {
      e.str = "<number>";
      JsNumberToDouble(el, &e.num);
    } else {
      size_t len = 0, w = 0;
      JsCopyString(el, nullptr, 0, &len);
      e.str.assign(len + 1, '\0');
      JsCopyString(el, &e.str[0], e.str.size(), &w);
      e.str.resize(w);
    }
    out.push_back(e);
  }
  return out;
}

bool ArrayMatches(const std::vector<Extracted>& got,
                  const std::vector<const char*>& want) {
  if (got.size() != want.size()) return false;
  for (size_t i = 0; i < want.size(); ++i) {
    if (got[i].is_symbol) return false;
    if (got[i].str != want[i]) return false;
  }
  return true;
}

// Holds a scope for the case duration.
struct CaseScope {
  napi_handle_scope s;
  CaseScope() : s(OpenScope()) {}
  ~CaseScope() { if (s) CloseScope(s); }
};

} /* namespace */

int main() {
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
  expect(chakra_nodeapi_test_root_baseline(g_env) == 0, "baseline helper is zero");

  // ------------------------------------------------------------------
  // 1. create_object / create_array / create_array_with_length
  // ------------------------------------------------------------------
  std::fprintf(stdout, "-- case create-object-array --\n");
  {
    CaseScope cs;
    napi_value o = nullptr;
    size_t c0 = CancelCount();
    expect_status(napi_create_object(g_env, &o), napi_ok, "create: object ok");
    expect(o != nullptr && JType(o) == JsObject, "create: object type");
    expect(CancelCount() == c0 + 1, "create: object roots +1");
    napi_value a = nullptr;
    expect_status(napi_create_array(g_env, &a), napi_ok, "create: array ok");
    expect(a != nullptr && JType(a) == JsArray, "create: array type");
    uint32_t len = 99;
    expect_status(napi_create_array_with_length(g_env, 3, &a), napi_ok,
                  "create: array_with_length ok");
    expect_status(napi_get_array_length(g_env, a, &len), napi_ok,
                  "create: length read ok");
    expect(len == 3, "create: length is 3");
    // Node oracle: over-max length wraps via get_value_int32 to an
    // empty (len 0) array with napi_ok (see provider comment).
    expect_status(napi_create_array_with_length(
                      g_env, (size_t)-1,     /* SIZE_MAX > UINT_MAX */
                      &a),
                  napi_ok, "create: huge length empty array (Node)");
    expect_status(napi_get_array_length(g_env, a, &len), napi_ok,
                  "create: huge length reads ok");
    expect(len == 0, "create: huge length is 0");
    StoreGlobal("__probeArr", a);
    expect(RunBool("Array.isArray(__probeArr)"), "create: JS isArray");
    // Element ops growing length.
    expect_status(napi_set_element(g_env, a, 2,
                                   reinterpret_cast<napi_value>(Num(20))),
                  napi_ok, "create: set_element ok");
    expect_status(napi_get_array_length(g_env, a, &len), napi_ok,
                  "create: length re-read ok");
    expect(len == 3, "create: length still 3");
  }
  AtBaseline("create: ends at baseline");

  // ------------------------------------------------------------------
  // 2. Named + property ops (chain walk, symbol keys, delete rules)
  // ------------------------------------------------------------------
  std::fprintf(stdout, "-- case property-ops --\n");
  {
    CaseScope cs;
    napi_value o = NewObj();
    napi_value proto = NewObj();
    napi_value v42 = reinterpret_cast<napi_value>(Num(42));
    napi_value inh = reinterpret_cast<napi_value>(Num(9));
    expect_status(napi_set_named_property(g_env, proto, "inh", inh), napi_ok,
                  "prop: proto set");
    // Chain the object to its prototype with direct JsSetPrototype
    // (no v8 napi_set_prototype exists).
    JsValueRef pv = JS_INVALID_REFERENCE;
    JsGetPrototype(JsV(o), &pv);
    expect_status(NodeApiObjects_SetPrototype(g_env, o, proto), napi_ok,
                  "prop: hidden set-prototype ok");
    expect_status(napi_set_named_property(g_env, o, "a", v42), napi_ok,
                  "prop: set_named ok");
    napi_value got = nullptr;
    expect_status(napi_get_named_property(g_env, o, "a", &got), napi_ok,
                  "prop: get_named ok");
    expect(got != nullptr && AsNum(got) == 42, "prop: get_named value 42");
    bool has = false;
    expect_status(napi_has_named_property(g_env, o, "a", &has), napi_ok,
                  "prop: has_named ok");
    expect(has, "prop: has_named true");
    expect_status(napi_has_named_property(g_env, o, "inh", &has), napi_ok,
                  "prop: has_named proto ok");
    expect(has, "prop: has_named walks proto");
    expect_status(napi_has_property(g_env, o, reinterpret_cast<napi_value>(Str("inh")), &has),
                  napi_ok, "prop: has_property proto ok");
    expect(has, "prop: has_property walks proto");
    napi_value kh = reinterpret_cast<napi_value>(Str("inh"));
    expect_status(napi_has_own_property(g_env, o, kh, &has), napi_ok,
                  "prop: has_own ok");
    expect(!has, "prop: has_own excludes proto");
    napi_value ka = reinterpret_cast<napi_value>(Str("a"));
    expect_status(napi_get_property(g_env, o, ka, &got), napi_ok,
                  "prop: get_property ok");
    expect(got != nullptr && AsNum(got) == 42, "prop: get_property value");
    expect_status(napi_get_property(g_env, o, kh, &got), napi_ok,
                  "prop: get_property proto ok");
    expect(got != nullptr && AsNum(got) == 9, "prop: get walks chain (9)");
    // Value-key set with a string key.
    napi_value kb = reinterpret_cast<napi_value>(Str("b"));
    napi_value vb = reinterpret_cast<napi_value>(Num(7));
    expect_status(napi_set_property(g_env, o, kb, vb), napi_ok, "prop: set_property ok");
    StoreGlobal("__probeObj", o);
    expect(RunBool("__probeObj.b === 7 && __probeObj.a === 42"),
           "prop: JS sees both");
    // Symbol key round-trip.
    JsValueRef sym = Sym("probe-sym");
    napi_value ks = reinterpret_cast<napi_value>(sym);
    napi_value vs = reinterpret_cast<napi_value>(Str("sym-val"));
    expect_status(napi_set_property(g_env, o, ks, vs), napi_ok, "prop: set sym ok");
    expect_status(napi_get_property(g_env, o, ks, &got), napi_ok, "prop: get sym ok");
    expect(got != nullptr && AsStr(got) == "sym-val", "prop: symbol value");
    expect_status(napi_has_own_property(g_env, o, ks, &has), napi_ok,
                  "prop: has_own sym ok");
    expect(has, "prop: symbol own true");
    // Delete: own present -> true, reveals proto; proto only -> true;
    // missing -> true (spec); then non-configurable -> false below.
    bool del = false;
    napi_value ka2 = reinterpret_cast<napi_value>(Str("a"));
    expect_status(napi_delete_property(g_env, o, ka2, &del), napi_ok,
                  "prop: delete ok");
    expect(del, "prop: delete own true");
    expect(!RunBool("'a' in __probeObj"), "prop: deleted from JS");
    // Shadowing revealed: proto 'inh' still visible.
    expect(RunBool("__probeObj.inh === 9"), "prop: proto value visible");
    expect_status(napi_delete_property(g_env, o, kh, &del), napi_ok,
                  "prop: delete proto-only ok");
    expect(del, "prop: delete proto-only true (no own deleted)");
    expect(RunBool("__probeObj.inh === 9"), "prop: proto still there");
    // NULL result for delete -> ok.
    expect_status(napi_delete_property(g_env, o, ka2, nullptr), napi_ok,
                  "prop: delete null-result ok");
    // Non-configurable property delete -> false.
    napi_value nc = NewObj();
    Run("Object.defineProperty(__probeNc = (function(){ return {}; })(), 'nc', {value: 1, configurable: false})");
    // Convert to napi_value for the delete call: use the global's value fetch via JS:
    napi_value ncObj = reinterpret_cast<napi_value>(Run("__probeNc"));
    napi_value knc = reinterpret_cast<napi_value>(Str("nc"));
    expect_status(napi_delete_property(g_env, ncObj, knc, &del), napi_ok,
                  "prop: delete noncfg ok");
    expect(!del, "prop: delete noncfg false");
    (void)nc;
  }
  AtBaseline("prop: ends at baseline");

  // ------------------------------------------------------------------
  // 3. Element ops incl. 2^32-1 boundary
  // ------------------------------------------------------------------
  std::fprintf(stdout, "-- case element-ops --\n");
  {
    CaseScope cs;
    napi_value arr = NewArr(0);
    napi_value v0 = reinterpret_cast<napi_value>(Str("zero"));
    expect_status(napi_set_element(g_env, arr, 0, v0), napi_ok, "elem: set 0 ok");
    bool has = false;
    expect_status(napi_has_element(g_env, arr, 0, &has), napi_ok, "elem: has 0 ok");
    expect(has, "elem: has 0 true");
    napi_value got = nullptr;
    expect_status(napi_get_element(g_env, arr, 0, &got), napi_ok, "elem: get 0 ok");
    expect(got != nullptr && AsStr(got) == "zero", "elem: value zero");
    expect_status(napi_has_element(g_env, arr, 1, &has), napi_ok, "elem: has 1 ok");
    expect(!has, "elem: hole at 1 false");
    napi_value v2 = reinterpret_cast<napi_value>(Str("two"));
    expect_status(napi_set_element(g_env, arr, 2, v2), napi_ok, "elem: set 2 ok");
    uint32_t len = 0;
    expect_status(napi_get_array_length(g_env, arr, &len), napi_ok, "elem: len ok");
    expect(len == 3, "elem: length 3 after sparse set");
    bool del = true;
    expect_status(napi_delete_element(g_env, arr, 2, &del), napi_ok, "elem: del ok");
    expect(del, "elem: deleted true");
    expect_status(napi_has_element(g_env, arr, 2, &has), napi_ok, "elem: has2 ok");
    expect(!has, "elem: deleted gone");
    expect_status(napi_delete_element(g_env, arr, 99, &del), napi_ok, "elem: del miss ok");
    expect(!del, "elem: delete missing false");
    expect_status(napi_delete_element(g_env, arr, 0, nullptr), napi_ok,
                  "elem: del null-result ok");
    // Ordinary object element access (property with numeric key).
    napi_value obj = NewObj();
    napi_value v5 = reinterpret_cast<napi_value>(Num(55));
    expect_status(napi_set_element(g_env, obj, 5, v5), napi_ok, "elem: obj set ok");
    expect_status(napi_has_element(g_env, obj, 5, &has), napi_ok, "elem: obj has ok");
    expect(has, "elem: obj has true");
    StoreGlobal("__probeObj5", obj);
    expect(RunBool("__probeObj5[5] === 55"), "elem: JS sees obj[5]");
    // 2^32-1 boundary (uint32 wrap to exact double).
    const uint32_t kMax = 4294967295u;
    napi_value vbig = reinterpret_cast<napi_value>(Str("big"));
    expect_status(napi_set_element(g_env, arr, kMax, vbig), napi_ok,
                  "elem: set 2^32-1 ok");
    expect_status(napi_has_element(g_env, arr, kMax, &has), napi_ok,
                  "elem: has 2^32-1 ok");
    expect(has, "elem: has 2^32-1 true");
    expect_status(napi_get_element(g_env, arr, kMax, &got), napi_ok,
                  "elem: get 2^32-1 ok");
    expect(got != nullptr && AsStr(got) == "big", "elem: value big");
    expect_status(napi_get_array_length(g_env, arr, &len), napi_ok,
                  "elem: len after big ok");
    expect(len == 3, "elem: 2^32-1 is not an array index (len stays 3)");
    expect_status(napi_delete_element(g_env, arr, kMax, &del), napi_ok,
                  "elem: del 2^32-1 ok");
    expect(del, "elem: deleted big true");
    expect_status(napi_has_element(g_env, arr, kMax, &has), napi_ok,
                  "elem: has big after del ok");
    expect(!has, "elem: big gone");
    // Plain object at 2^32-1.
    napi_value obig = reinterpret_cast<napi_value>(Num(11));
    expect_status(napi_set_element(g_env, obj, kMax, obig), napi_ok,
                  "elem: obj set max ok");
    expect_status(napi_get_element(g_env, obj, kMax, &got), napi_ok,
                  "elem: obj get max ok");
    expect(got != nullptr && AsNum(got) == 11, "elem: obj max value");
  }
  AtBaseline("elem: ends at baseline");

  // ------------------------------------------------------------------
  // 4. define_properties: descriptor round-trip, accessor pair,
  //    redefine-to-false (audit #7), method priority, failures
  // ------------------------------------------------------------------
  std::fprintf(stdout, "-- case define-properties --\n");
  {
    CaseScope cs;
    napi_value o = NewObj();
    StoreGlobal("__defObj", o);
    size_t c0 = CancelCount();
    // Data: all flags true.
    {
      napi_value v = reinterpret_cast<napi_value>(Num(123));
      napi_property_descriptor d{ "rw", nullptr, nullptr, nullptr, nullptr, v,
                                 static_cast<napi_property_attributes>(
                                     napi_writable | napi_enumerable | napi_configurable),
                                 nullptr };
      expect_status(napi_define_properties(g_env, o, 1, &d), napi_ok,
                    "def: rw ok");
      expect(CancelCount() == c0, "def: data adds no scope root");
    }
    expect(RunBool("Object.getOwnPropertyDescriptor(__defObj,'rw').writable===true && Object.getOwnPropertyDescriptor(__defObj,'rw').enumerable===true && Object.getOwnPropertyDescriptor(__defObj,'rw').configurable===true && Object.getOwnPropertyDescriptor(__defObj,'rw').value===123"),
           "def: rw descriptor round-trip");
    Run("__defObj.rw = 124");
    expect(RunBool("__defObj.rw === 124"), "def: rw writable in JS");
    // Data: all flags false (read-only, hidden, non-configurable).
    {
      napi_value v = reinterpret_cast<napi_value>(Num(5));
      napi_property_descriptor d{ "ro", nullptr, nullptr, nullptr, nullptr, v,
                                 napi_default, nullptr };
      expect_status(napi_define_properties(g_env, o, 1, &d), napi_ok, "def: ro ok");
    }
    expect(RunBool("Object.getOwnPropertyDescriptor(__defObj,'ro').writable===false && Object.getOwnPropertyDescriptor(__defObj,'ro').enumerable===false && Object.getOwnPropertyDescriptor(__defObj,'ro').configurable===false && Object.getOwnPropertyDescriptor(__defObj,'ro').value===5"),
           "def: ro descriptor round-trip");
    // Data: enumerable only (non-writable).
    {
      napi_value v = reinterpret_cast<napi_value>(Num(6));
      napi_property_descriptor d{ "eo", nullptr, nullptr, nullptr, nullptr, v,
                                 napi_enumerable, nullptr };
      expect_status(napi_define_properties(g_env, o, 1, &d), napi_ok, "def: eo ok");
    }
    expect(RunBool("Object.getOwnPropertyDescriptor(__defObj,'eo').writable===false && Object.getOwnPropertyDescriptor(__defObj,'eo').enumerable===true && Object.getOwnPropertyDescriptor(__defObj,'eo').configurable===false"),
           "def: eo descriptor round-trip");
    // Accessor pair: getter returns cell, setter counts. writable bit
    // (ignored for accessors per vendored test.js) set to prove it is
    // ignored.
    g_method_calls = 0; g_getter_calls = 0; g_setter_calls = 0; g_cell = 7.0;
    {
      napi_property_descriptor d{ "acc", nullptr, nullptr, GetterCb, SetterCb,
                                 nullptr,
                                 static_cast<napi_property_attributes>(
                                     napi_writable | napi_enumerable | napi_configurable),
                                 nullptr };
      size_t c1 = CancelCount();
      expect_status(napi_define_properties(g_env, o, 1, &d), napi_ok, "def: acc ok");
      expect(CancelCount() == c1 + 2, "def: accessor creates 2 functions (+2 roots)");
    }
    expect(RunBool("typeof Object.getOwnPropertyDescriptor(__defObj,'acc').get === 'function' && typeof Object.getOwnPropertyDescriptor(__defObj,'acc').set === 'function' && Object.getOwnPropertyDescriptor(__defObj,'acc').value === undefined && Object.getOwnPropertyDescriptor(__defObj,'acc').enumerable === true && Object.getOwnPropertyDescriptor(__defObj,'acc').configurable === true"),
           "def: accessor descriptor round-trip");
    expect(RunBool("__defObj.acc === 7"), "def: getter returns cell");
    expect(g_getter_calls >= 1, "def: getter invoked");
    Run("__defObj.acc = 99");
    expect(g_setter_calls == 1, "def: setter invoked once");
    expect(RunBool("__defObj.acc === 7"),
           "def: cell intact (setter ignores argv[0]=99 under the real thunk)");
    // Getter-only accessor.
    {
      napi_property_descriptor d{ "go", nullptr, nullptr, GetterCb, nullptr,
                                 nullptr, napi_default, nullptr };
      expect_status(napi_define_properties(g_env, o, 1, &d), napi_ok, "def: getonly ok");
    }
    expect(RunBool("Object.getOwnPropertyDescriptor(__defObj,'go').set === undefined && typeof Object.getOwnPropertyDescriptor(__defObj,'go').get === 'function'"),
           "def: getonly descriptor");
    expect(RunBool("(function(){ 'use strict'; try { __defObj.go = 1; return false; } catch (e) { return e instanceof TypeError; } })()"),
           "def: getonly set throws in JS");
    // Method descriptor (+ data-writable).
    g_method_calls = 0;
    {
      napi_property_descriptor d{ "m", nullptr, MethodCb, nullptr, nullptr,
                                 nullptr,
                                 static_cast<napi_property_attributes>(
                                     napi_writable | napi_enumerable | napi_configurable),
                                 nullptr };
      expect_status(napi_define_properties(g_env, o, 1, &d), napi_ok, "def: method ok");
    }
    expect(RunBool("typeof __defObj.m === 'function'"), "def: method is function");
    int before = g_method_calls;
    expect(RunBool("__defObj.m() === 4242"), "def: method returns constant");
    expect(g_method_calls == before + 1, "def: method invoked");
    // Method + value: method wins over value.
    {
      napi_value v = reinterpret_cast<napi_value>(Num(1));
      napi_property_descriptor d{ "mv", nullptr, MethodCb, nullptr, nullptr, v,
                                 napi_default, nullptr };
      expect_status(napi_define_properties(g_env, o, 1, &d), napi_ok, "def: method+value ok");
    }
    expect(RunBool("typeof __defObj.mv === 'function'"), "def: method wins over value");
    // Symbol-named data property.
    {
      JsValueRef sym = Sym("def-sym");
      napi_value v = reinterpret_cast<napi_value>(Num(8));
      napi_property_descriptor d{ nullptr,
                                  reinterpret_cast<napi_value>(sym),
                                  nullptr, nullptr, nullptr, v,
                                  napi_enumerable, nullptr };
      expect_status(napi_define_properties(g_env, o, 1, &d), napi_ok, "def: symbol ok");
      // Publish the symbol for the JS readback.
      StoreGlobal("__defSym", reinterpret_cast<napi_value>(sym));
    }
    expect(RunBool("Object.getOwnPropertyDescriptor(__defObj, __defSym).value === 8"),
           "def: symbol property round-trip");
    // Targeted setting method + no descriptor fields with wrong kind
    // left room for accessor/value mix? Covered by successful defines
    // above (ToPropertyDescriptor would have thrown otherwise).
    // Redefine with false flags (audit #7): full false after full true.
    {
      napi_value v1 = reinterpret_cast<napi_value>(Num(1));
      napi_property_descriptor d1{ "r", nullptr, nullptr, nullptr, nullptr, v1,
                                  static_cast<napi_property_attributes>(
                                      napi_writable | napi_enumerable | napi_configurable),
                                  nullptr };
      expect_status(napi_define_properties(g_env, o, 1, &d1), napi_ok, "def: r1 ok");
      napi_value v9 = reinterpret_cast<napi_value>(Num(9));
      napi_property_descriptor d2{ "r", nullptr, nullptr, nullptr, nullptr, v9,
                                  napi_default, nullptr };
      expect_status(napi_define_properties(g_env, o, 1, &d2), napi_ok, "def: r2 ok");
    }
    expect(RunBool("Object.getOwnPropertyDescriptor(__defObj,'r').writable===false && Object.getOwnPropertyDescriptor(__defObj,'r').enumerable===false && Object.getOwnPropertyDescriptor(__defObj,'r').configurable===false && Object.getOwnPropertyDescriptor(__defObj,'r').value===9"),
           "def: redefine forces false flags (audit #7)");
    // Multiple properties in one call.
    {
      napi_value v1 = reinterpret_cast<napi_value>(Num(11));
      napi_value v2 = reinterpret_cast<napi_value>(Num(22));
      napi_property_descriptor batch[2] = {
          { "b1", nullptr, nullptr, nullptr, nullptr, v1, napi_default, nullptr },
          { "b2", nullptr, nullptr, nullptr, nullptr, v2, napi_default, nullptr },
      };
      expect_status(napi_define_properties(g_env, o, 2, batch), napi_ok,
                    "def: batch ok");
    }
    expect(RunBool("__defObj.b1 === 11 && __defObj.b2 === 22"), "def: batch values");
    // Failures.
    {
      napi_property_descriptor d{ "x", nullptr, nullptr, nullptr, nullptr,
                                 nullptr, napi_default, nullptr };
      expect_status(napi_define_properties(g_env, o, 1, nullptr),
                    napi_invalid_arg, "def: null list invalid");
      expect_status(napi_define_properties(g_env, nullptr, 1, &d),
                    napi_invalid_arg, "def: null object invalid");
      napi_value v = reinterpret_cast<napi_value>(Num(1));
      napi_property_descriptor noname{ nullptr, nullptr, nullptr, nullptr,
                                       nullptr, v, napi_default, nullptr };
      expect_status(napi_define_properties(g_env, o, 1, &noname),
                    napi_invalid_arg, "def: nameless invalid");
      napi_value key = reinterpret_cast<napi_value>(Num(3));
      napi_property_descriptor badkey{ nullptr, key, nullptr, nullptr, nullptr,
                                       v, napi_default, nullptr };
      expect_status(napi_define_properties(g_env, o, 1, &badkey),
                    napi_name_expected, "def: numeric name name_expected");
      expect_status(napi_define_properties(g_env, o, 0, nullptr), napi_ok,
                    "def: empty ok");
    }
    // Define on a frozen object (MED-2 fix): JsRT throws a pending
    // TypeError, but Node maps the non-extensible failure to
    // napi_invalid_arg with NO pending exception. The provider now
    // clears and matches Node. Frozen SET, by contrast, stays
    // napi_ok with no pending (sloppy Set returns false without
    // throwing in both Node and JsRT) -- probed below.
    {
      napi_value fr = NewObj();
      StoreGlobal("__defFrz", fr);
      expect_status(napi_object_freeze(g_env, fr), napi_ok, "def: freeze for target ok");
      expect(RunBool("Object.isFrozen(__defFrz)"), "def: frozen observable");
      napi_value v = reinterpret_cast<napi_value>(Num(1));
      napi_property_descriptor d{ "k", nullptr, nullptr, nullptr, nullptr, v,
                                 napi_default, nullptr };
      expect_status(napi_define_properties(g_env, fr, 1, &d),
                    napi_invalid_arg, "def: frozen -> invalid_arg (Node)");
      expect(!HasPending(), "def: no pending after frozen define (Node)");
      expect(RunBool("__defFrz.k === undefined"), "def: frozen define added nothing");
      // Sealed define of a NEW property is the same non-extensible
      // failure -> invalid_arg, no pending.
      napi_value sl = NewObj();
      StoreGlobal("__defSealed", sl);
      expect_status(napi_object_seal(g_env, sl), napi_ok, "def: seal for target ok");
      expect(RunBool("Object.isSealed(__defSealed)"), "def: sealed observable");
      expect_status(napi_define_properties(g_env, sl, 1, &d),
                    napi_invalid_arg, "def: sealed new -> invalid_arg (Node)");
      expect(!HasPending(), "def: no pending after sealed define (Node)");
      expect(RunBool("__defSealed.k === undefined"), "def: sealed define added nothing");
      // Frozen SET (existing and new props): sloppy Set, no throw,
      // napi_ok, no pending, value unchanged (Node sloppy too).
      napi_value fz = NewObj();
      StoreGlobal("__defFzSet", fz);
      napi_value ten = reinterpret_cast<napi_value>(Num(10));
      expect_status(napi_set_named_property(g_env, fz, "x", ten), napi_ok,
                    "def: set setup x ok");
      expect_status(napi_object_freeze(g_env, fz), napi_ok, "def: freeze for set ok");
      napi_value v99 = reinterpret_cast<napi_value>(Num(99));
      expect_status(napi_set_named_property(g_env, fz, "x", v99), napi_ok,
                    "def: frozen set existing -> ok (sloppy, Node)");
      expect(!HasPending(), "def: no pending after frozen set");
      expect(RunBool("__defFzSet.x === 10"), "def: frozen set kept old value");
      expect_status(napi_set_named_property(g_env, fz, "newp", v99), napi_ok,
                    "def: frozen set new -> ok (sloppy, Node)");
      expect(!HasPending(), "def: no pending after frozen new set");
      expect(RunBool("__defFzSet.newp === undefined"), "def: frozen set added nothing");
      // Non-frozen failing define (extensible, non-configurable
      // conflict) must KEEP pending_exception (S3.10), proving the
      // MED-2 conversion is narrowly non-extensible-only. Define a
      // non-configurable prop, then redefine to a different kind.
      napi_value ex = NewObj();
      StoreGlobal("__defExt", ex);
      napi_value vv = reinterpret_cast<napi_value>(Num(1));
      napi_property_descriptor dnc{ "nc", nullptr, nullptr, nullptr, nullptr,
                                   vv, napi_default, nullptr };
      // napi_default (0) => writable/enumerable/configurable false.
      expect_status(napi_define_properties(g_env, ex, 1, &dnc), napi_ok,
                    "def: extensible setup non-configurable ok");
      napi_property_descriptor dconf{ "nc", nullptr, nullptr, nullptr, nullptr,
                                     vv, static_cast<napi_property_attributes>(napi_configurable),
                                     nullptr };
      // Trying to make a non-configurable prop configurable must throw.
      expect_status(napi_define_properties(g_env, ex, 1, &dconf),
                    napi_pending_exception, "def: extensible conflict keeps pending");
      expect(HasPending(), "def: pending preserved for extensible conflict");
      ClearPending();
    }
  }
  AtBaseline("def: ends at baseline");

  // ------------------------------------------------------------------
  // 5. Property names: vendored test_object scenario (all
  //    modes/filters/conversions) + accessor writable filter +
  //    empty/null-proto edges + bad enums
  // ------------------------------------------------------------------
  std::fprintf(stdout, "-- case property-names --\n");
  {
    CaseScope cs;
    // Build { __proto__: { inherited: 1 }, normal, [fooSymbol], plus
    // unenumerable/writable/configurable, [5] } -- exactly the
    // vendored test_object fixture.
    napi_value o = NewObj();
    napi_value proto = NewObj();
    napi_value inh = reinterpret_cast<napi_value>(Num(1));
    expect_status(napi_set_named_property(g_env, proto, "inherited", inh), napi_ok,
                  "names: proto ok");
    expect_status(NodeApiObjects_SetPrototype(g_env, o, proto), napi_ok, "names: chain ok");
    napi_value two = reinterpret_cast<napi_value>(Num(2));
    expect_status(napi_set_named_property(g_env, o, "normal", two), napi_ok,
                  "names: normal ok");
    JsValueRef foo = Sym("foo");
    napi_value three = reinterpret_cast<napi_value>(Num(3));
    expect_status(napi_set_property(g_env, o, reinterpret_cast<napi_value>(foo), three),
                  napi_ok, "names: symbol ok");
    // JS-side defineProperty for the three flagged variants.
    StoreGlobal("__namesObj", o);
    StoreGlobal("__namesSym", reinterpret_cast<napi_value>(foo));
    Run("Object.defineProperty(__namesObj, 'unenumerable', {value: 4, enumerable: false, writable: true, configurable: true})");
    Run("Object.defineProperty(__namesObj, 'writable', {value: 4, enumerable: true, writable: true, configurable: false})");
    Run("Object.defineProperty(__namesObj, 'configurable', {value: 4, enumerable: true, writable: false, configurable: true})");
    napi_value five = reinterpret_cast<napi_value>(Num(5));
    expect_status(napi_set_element(g_env, o, 5, five), napi_ok, "names: index 5 ok");

    napi_value res = nullptr;
    // get_property_names == enumerable, skip symbols, inherited, num->str.
    expect_status(napi_get_property_names(g_env, o, &res), napi_ok,
                  "names: get_property_names ok");
    auto got = ExtractArray(res);
    std::vector<const char*> want_names = { "5", "normal", "writable", "configurable", "inherited" };
    expect(ArrayMatches(got, want_names), "names: get_property_names exact");
    // include_prototypes + skip_strings + numbers_to_strings -> symbols.
    expect_status(napi_get_all_property_names(
                      g_env, o, napi_key_include_prototypes, napi_key_skip_strings,
                      napi_key_numbers_to_strings, &res),
                  napi_ok, "names: symbols-only ok");
    {
      auto syms = ExtractArray(res);
      bool ok = (syms.size() == 1 && syms[0].is_symbol);
      // Verify the symbol is actually foo: compare via JS.
      expect(ok, "names: symbols-only is the foo symbol");
      expect(RunBool("(function(){ var a = (function(){ return __namesObj; })(); return true; })()"),
             "names: sanity (no-op)");
    }
    // include_prototypes + enumerable|writable + num->str.
    expect_status(napi_get_all_property_names(
                      g_env, o, napi_key_include_prototypes,
                      static_cast<napi_key_filter>(napi_key_enumerable | napi_key_writable),
                      napi_key_numbers_to_strings, &res),
                  napi_ok, "names: enum+writable ok");
    got = ExtractArray(res);
    // Exact vendored expectation: ['5','normal','writable', fooSymbol,
    // 'inherited'].
    expect(got.size() == 5 && got[0].str == "5" && got[1].str == "normal" &&
           got[2].str == "writable" && got[3].is_symbol &&
           got[4].str == "inherited",
           "names: enum+writable exact");
    // own_only + writable + num->str: exact strings + symbol.
    expect_status(napi_get_all_property_names(
                      g_env, o, napi_key_own_only, napi_key_writable,
                      napi_key_numbers_to_strings, &res),
                  napi_ok, "names: own+writable ok");
    got = ExtractArray(res);
    expect(got.size() == 5 && !got[0].is_symbol && !got[1].is_symbol && !got[2].is_symbol &&
           !got[3].is_symbol && got[4].is_symbol,
           "names: own+writable shape (4 strings + symbol)");
    expect(got.size() == 5 && got[0].str == "5" && got[1].str == "normal" &&
           got[2].str == "unenumerable" && got[3].str == "writable",
           "names: own+writable exact strings");
    // include_prototypes + enumerable|configurable + num->str.
    expect_status(napi_get_all_property_names(
                      g_env, o, napi_key_include_prototypes,
                      static_cast<napi_key_filter>(napi_key_enumerable | napi_key_configurable),
                      napi_key_numbers_to_strings, &res),
                  napi_ok, "names: enum+config ok");
    got = ExtractArray(res);
    expect(got.size() == 5 && got[0].str == "5" && got[1].str == "normal" &&
           got[2].str == "configurable",
           "names: enum+config strings");
    expect(got.size() == 5 && got[3].is_symbol && got[4].str == "inherited",
           "names: enum+config symbol+inherited");
    // own_only + configurable + num->str.
    expect_status(napi_get_all_property_names(
                      g_env, o, napi_key_own_only, napi_key_configurable,
                      napi_key_numbers_to_strings, &res),
                  napi_ok, "names: own+config ok");
    got = ExtractArray(res);
    expect(got.size() == 5 && got[0].str == "5" && got[1].str == "normal" &&
           got[2].str == "unenumerable" && got[3].str == "configurable" &&
           got[4].is_symbol,
           "names: own+config exact");
    // own_only + all + keep_numbers: '5' stays a NUMBER.
    expect_status(napi_get_all_property_names(
                      g_env, o, napi_key_own_only, napi_key_all_properties,
                      napi_key_keep_numbers, &res),
                  napi_ok, "names: keep_numbers ok");
    got = ExtractArray(res);
    expect(got.size() == 6 && got[0].str == "<number>" && got[0].num == 5.0 &&
           got[1].str == "normal" && got[2].str == "unenumerable" &&
           got[3].str == "writable" && got[4].str == "configurable" &&
           got[5].is_symbol,
           "names: keep_numbers exact");
    // own_only + skip_symbols + num->str: strings only.
    expect_status(napi_get_all_property_names(
                      g_env, o, napi_key_own_only, napi_key_skip_symbols,
                      napi_key_numbers_to_strings, &res),
                  napi_ok, "names: skip_symbols ok");
    got = ExtractArray(res);
    expect(ArrayMatches(got, { "5", "normal", "unenumerable", "writable", "configurable" }),
           "names: skip_symbols exact");
    // Bad enums.
    expect_status(napi_get_all_property_names(
                      g_env, o, static_cast<napi_key_collection_mode>(99),
                      napi_key_all_properties, napi_key_keep_numbers, &res),
                  napi_invalid_arg, "names: bad mode invalid");
    expect_status(napi_get_all_property_names(
                      g_env, o, napi_key_own_only, napi_key_all_properties,
                      static_cast<napi_key_conversion>(99), &res),
                  napi_invalid_arg, "names: bad conversion invalid");
    expect_status(napi_get_all_property_names(
                      g_env, o, napi_key_own_only,
                      static_cast<napi_key_filter>(64), napi_key_keep_numbers, &res),
                  napi_invalid_arg, "names: bad filter invalid");
    expect_status(napi_get_all_property_names(
                      g_env, nullptr, napi_key_own_only, napi_key_all_properties,
                      napi_key_keep_numbers, &res),
                  napi_invalid_arg, "names: null object invalid");
    expect_status(napi_get_property_names(g_env, o, nullptr), napi_invalid_arg,
                  "names: null result invalid");

    // Accessor writable-filter lock (accessors never pass writable).
    napi_value ao = NewObj();
    {
      napi_value v = reinterpret_cast<napi_value>(Num(1));
      napi_property_descriptor d{ "data", nullptr, nullptr, nullptr, nullptr, v,
                                 napi_writable, nullptr };
      expect_status(napi_define_properties(g_env, ao, 1, &d), napi_ok, "names: acc data ok");
    }
    g_getter_calls = 0; g_setter_calls = 0;
    {
      napi_property_descriptor d{ "acc", nullptr, nullptr, GetterCb, SetterCb,
                                 nullptr, napi_writable, nullptr };
      expect_status(napi_define_properties(g_env, ao, 1, &d), napi_ok, "names: acc def ok");
    }
    expect_status(napi_get_all_property_names(
                      g_env, ao, napi_key_own_only, napi_key_writable,
                      napi_key_numbers_to_strings, &res),
                  napi_ok, "names: writable names ok");
    got = ExtractArray(res);
    expect(ArrayMatches(got, { "data" }),
           "names: writable filter excludes accessor");
    // Writable filter OFF: accessor present.
    expect_status(napi_get_all_property_names(
                      g_env, ao, napi_key_own_only, napi_key_all_properties,
                      napi_key_numbers_to_strings, &res),
                  napi_ok, "names: all names ok");
    got = ExtractArray(res);
    expect(ArrayMatches(got, { "data", "acc" }), "names: all includes accessor");

    // Empty object + null-proto object.
    napi_value empty = NewObj();
    expect_status(napi_get_property_names(g_env, empty, &res), napi_ok, "names: empty ok");
    expect(ArrLenJs(JsV(res)) == 0, "names: empty is []");
    StoreGlobal("__nullProto", empty);
    Run("Object.setPrototypeOf(__nullProto, null)");
    expect_status(napi_get_all_property_names(
                      g_env, empty, napi_key_include_prototypes,
                      napi_key_all_properties, napi_key_numbers_to_strings, &res),
                  napi_ok, "names: null-proto ok");
    expect(ArrLenJs(JsV(res)) == 0, "names: null-proto has no names");
  }
  AtBaseline("names: ends at baseline");

  // ------------------------------------------------------------------
  // 6. Prototype / freeze / seal / instanceof
  // ------------------------------------------------------------------
  std::fprintf(stdout, "-- case proto-freeze-instanceof --\n");
  {
    CaseScope cs;
    napi_value o = NewObj();
    StoreGlobal("__pfObj", o);
    napi_value proto = nullptr;
    expect_status(napi_get_prototype(g_env, o, &proto), napi_ok, "pf: get proto ok");
    expect(proto != nullptr, "pf: proto non-null");
    expect(RunBool("Object.getPrototypeOf(__pfObj) === Object.prototype"), "pf: {} has Object.prototype");
    // Instanceof chain via JS-built constructor.
    Run("function __Ctor() {} __protoCtor = __Ctor; __protoInst = new __Ctor();");
    napi_value ctor = reinterpret_cast<napi_value>(Run("__protoCtor"));
    napi_value inst = reinterpret_cast<napi_value>(Run("__protoInst"));
    StoreGlobal("__pfCtor", ctor);
    StoreGlobal("__pfInst", inst);
    bool r = true;
    expect_status(napi_instanceof(g_env, inst, ctor, &r), napi_ok, "pf: instanceof true ok");
    expect(r, "pf: instance is instanceof");
    expect_status(napi_instanceof(g_env, o, ctor, &r), napi_ok, "pf: instanceof false ok");
    expect(!r, "pf: plain object is not instanceof");
    // Node: non-function ctor throws TypeError AND returns
    // function_expected (both observable). Clear the pending so the
    // next calls do not gate to pending_exception.
    expect_status(napi_instanceof(g_env, inst, reinterpret_cast<napi_value>(Num(5)), &r),
                  napi_function_expected, "pf: number ctor function_expected");
    expect(HasPending(), "pf: number ctor leaves TypeError pending (Node)");
    {
      const napi_extended_error_info* ei = nullptr;
      expect_status(napi_get_last_error_info(g_env, &ei), napi_ok,
                    "pf: instanceof last_error fetch still pending-safe");
      // Reading last_error does NOT clear it (Node + provider agree),
      // so the code must still be function_expected: throw-then-Set
      // order pin (reversed order would be wiped by the throw's
      // ClearLastError to napi_ok).
      expect(ei != nullptr && ei->error_code == napi_function_expected,
             "pf: instanceof throw-then-Set order pin (function_expected + pending)");
    }
    ClearPending();
    expect_status(napi_instanceof(g_env, inst, o, &r), napi_function_expected,
                  "pf: object ctor function_expected");
    expect(HasPending(), "pf: object ctor leaves TypeError pending (Node)");
    ClearPending();
    {
      JsValueRef nul = JS_INVALID_REFERENCE;
      JsGetNullValue(&nul);
      expect_status(napi_instanceof(g_env, inst, reinterpret_cast<napi_value>(nul), &r),
                    napi_object_expected, "pf: null ctor object_expected");
      // Our ToObject pre-check leaves no pending for null/undefined
      // (documented divergence vs Node's CHECK_TO_OBJECT pending).
      expect(!HasPending(), "pf: null ctor leaves no pending (divergence pin)");
      ClearPending();
    }
    // Null-proto prototype value is JS null.
    napi_value npo = NewObj();
    expect_status(NodeApiObjects_SetPrototype(g_env, npo, reinterpret_cast<napi_value>([] {
                    JsValueRef nul = JS_INVALID_REFERENCE;
                    JsGetNullValue(&nul);
                    return nul;
                  }())),
                  napi_ok, "pf: set null proto ok");
    expect_status(napi_get_prototype(g_env, npo, &proto), napi_ok, "pf: get null proto ok");
    expect(proto != nullptr && JType(proto) == JsNull, "pf: prototype null value");
    expect_status(napi_get_prototype(g_env, nullptr, &proto), napi_invalid_arg,
                  "pf: null object invalid");
    {
      JsValueRef nul = JS_INVALID_REFERENCE;
      JsGetNullValue(&nul);
      expect_status(napi_get_prototype(g_env, reinterpret_cast<napi_value>(nul), &proto),
                    napi_object_expected, "pf: JS null object_expected");
    }
    // Freeze / seal observable from JS.
    napi_value fz = NewObj();
    StoreGlobal("__pfFz", fz);
    napi_value ten = reinterpret_cast<napi_value>(Num(10));
    expect_status(napi_set_named_property(g_env, fz, "x", ten), napi_ok, "pf: freeze setup x ok");
    expect_status(napi_object_freeze(g_env, fz), napi_ok, "pf: freeze ok");
    expect(RunBool("Object.isFrozen(__pfFz)"), "pf: isFrozen");
    expect(RunBool("(function(){ 'use strict'; try { __pfFz.x = 1; return false; } catch (e) { return e instanceof TypeError; } })()"),
           "pf: strict write throws when frozen");
    expect(RunBool("(function(){ return (delete __pfFz.x) === false; })()"),
           "pf: delete frozen returns false (no throw in sloppy)");
    napi_value sl = NewObj();
    StoreGlobal("__pfSl", sl);
    expect_status(napi_set_named_property(g_env, sl, "x", ten), napi_ok, "pf: seal setup x ok");
    expect_status(napi_object_seal(g_env, sl), napi_ok, "pf: seal ok");
    expect(RunBool("Object.isSealed(__pfSl)"), "pf: isSealed");
    expect(RunBool("(function(){ 'use strict'; try { __pfSl.w = 1; return false; } catch (e) { return e instanceof TypeError; } })()"),
           "pf: strict add throws when sealed");
    expect(RunBool("(__pfSl.x = 99) === 99"), "pf: sealed allows update");
    {
      JsValueRef nul = JS_INVALID_REFERENCE;
      JsGetNullValue(&nul);
      expect_status(napi_object_freeze(g_env, reinterpret_cast<napi_value>(nul)),
                    napi_object_expected, "pf: freeze null object_expected");
      JsValueRef uv = JS_INVALID_REFERENCE;
      JsGetUndefinedValue(&uv);
      expect_status(napi_object_seal(g_env, reinterpret_cast<napi_value>(uv)),
                    napi_object_expected, "pf: seal undefined object_expected");
    }
    // ToObject boxing: get_prototype on a number -> Number.prototype.
    expect_status(napi_get_prototype(g_env, reinterpret_cast<napi_value>(Num(5)), &proto),
                  napi_ok, "pf: boxed proto ok");
    StoreGlobal("__pfNumProto", proto);
    expect(RunBool("__pfNumProto === Number.prototype"), "pf: boxed is Number.prototype");
    // has_property on a boxed primitive does not throw (false).
    bool bh = true;
    expect_status(napi_has_property(g_env, reinterpret_cast<napi_value>(Num(5)), reinterpret_cast<napi_value>(Str("x")), &bh),
                  napi_ok, "pf: boxed has ok");
    expect(!bh, "pf: boxed has false");
  }
  AtBaseline("pf: ends at baseline");

  // ------------------------------------------------------------------
  // 7. Pending discipline for every gated entry (+ creates execute)
  // ------------------------------------------------------------------
  std::fprintf(stdout, "-- case pending --\n");
  {
    CaseScope cs;
    napi_value o = NewObj();
    napi_value k = reinterpret_cast<napi_value>(Str("k"));
    napi_value v = reinterpret_cast<napi_value>(Num(1));
    napi_value frozen = NewObj();
    expect_status(napi_set_named_property(g_env, o, "k", v), napi_ok, "pend: setup ok");
    SetPendingThrow();
    expect(HasPending(), "pend: pending set");
    // Creates execute (C-column Yes).
    napi_value tmp = nullptr;
    expect_status(napi_create_object(g_env, &tmp), napi_ok, "pend: create_object executes");
    napi_value tmpa = nullptr;
    expect_status(napi_create_array(g_env, &tmpa), napi_ok, "pend: create_array executes");
    napi_value tmpb = nullptr;
    expect_status(napi_create_array_with_length(g_env, 2, &tmpb), napi_ok,
                  "pend: create_array_with_length executes");
    expect(HasPending(), "pend: pending preserved through creates");
    // Every gated entry -> pending_exception, no state change.
    napi_value got = reinterpret_cast<napi_value>((uintptr_t)0xDEAD);
    uint32_t u32 = 0;
    bool b = false;
    expect_status(napi_get_array_length(g_env, tmpa, &u32), napi_pending_exception,
                  "pend: get_array_length gated");
    expect_status(napi_get_property(g_env, o, k, &got), napi_pending_exception,
                  "pend: get_property gated");
    expect(got == reinterpret_cast<napi_value>((uintptr_t)0xDEAD),
           "pend: gated get leaves out-param untouched");
    expect_status(napi_set_property(g_env, o, k, v), napi_pending_exception,
                  "pend: set_property gated");
    expect_status(napi_has_property(g_env, o, k, &b), napi_pending_exception,
                  "pend: has_property gated");
    expect_status(napi_delete_property(g_env, o, k, &b), napi_pending_exception,
                  "pend: delete_property gated");
    expect_status(napi_has_own_property(g_env, o, k, &b), napi_pending_exception,
                  "pend: has_own gated");
    expect_status(napi_get_named_property(g_env, o, "k", &got), napi_pending_exception,
                  "pend: get_named gated");
    expect_status(napi_set_named_property(g_env, o, "k", v), napi_pending_exception,
                  "pend: set_named gated");
    expect_status(napi_has_named_property(g_env, o, "k", &b), napi_pending_exception,
                  "pend: has_named gated");
    expect_status(napi_get_element(g_env, o, 0, &got), napi_pending_exception,
                  "pend: get_element gated");
    expect_status(napi_set_element(g_env, o, 0, v), napi_pending_exception,
                  "pend: set_element gated");
    expect_status(napi_has_element(g_env, o, 0, &b), napi_pending_exception,
                  "pend: has_element gated");
    expect_status(napi_delete_element(g_env, o, 0, &b), napi_pending_exception,
                  "pend: delete_element gated");
    napi_property_descriptor d{ "x", nullptr, nullptr, nullptr, nullptr, v,
                               napi_default, nullptr };
    expect_status(napi_define_properties(g_env, o, 1, &d), napi_pending_exception,
                  "pend: define_properties gated");
    expect_status(napi_get_property_names(g_env, o, &got), napi_pending_exception,
                  "pend: get_property_names gated");
    expect_status(napi_get_all_property_names(g_env, o, napi_key_own_only,
                                              napi_key_writable,
                                              napi_key_keep_numbers, &got),
                  napi_pending_exception, "pend: get_all gated");
    expect_status(napi_get_prototype(g_env, o, &got), napi_pending_exception,
                  "pend: get_prototype gated");
    expect_status(napi_object_freeze(g_env, frozen), napi_pending_exception,
                  "pend: freeze gated");
    expect_status(napi_object_seal(g_env, frozen), napi_pending_exception,
                  "pend: seal gated");
    expect_status(napi_instanceof(g_env, o, v, &b), napi_pending_exception,
                  "pend: instanceof gated");
    expect(HasPending(), "pend: exception still pending after all gated calls");
    ClearPending();
    expect(!HasPending(), "pend: cleared");
    // State was untouched: k still 1, frozen NOT frozen.
    StoreGlobal("__pendObj", o);
    StoreGlobal("__pendFrozen", frozen);
    expect(RunBool("__pendObj.k === 1"), "pend: value untouched");
    expect(RunBool("!Object.isFrozen(__pendFrozen) && !Object.isSealed(__pendFrozen)"),
           "pend: no freeze/seal happened");
    expect_status(napi_get_named_property(g_env, o, "k", &got), napi_ok,
                  "pend: gated resumes after clear");
    expect(got != nullptr && AsNum(got) == 1, "pend: value still 1");
    // Gate beats type check: pending + null object -> pending (not object_expected).
    SetPendingThrow();
    expect_status(napi_get_property(g_env, reinterpret_cast<napi_value>([] {
                    JsValueRef nul = JS_INVALID_REFERENCE;
                    JsGetNullValue(&nul);
                    return nul;
                  }()),
                  k, &got),
                  napi_pending_exception, "pend: gate beats object_expected");
    ClearPending();
  }
  AtBaseline("pend: ends at baseline");

  // ------------------------------------------------------------------
  // 8. Type pre-checks + last-error (AMB-STATUS-1) + NULL matrix
  // ------------------------------------------------------------------
  std::fprintf(stdout, "-- case types-errors-null --\n");
  {
    CaseScope cs;
    napi_value o = NewObj();
    napi_value k = reinterpret_cast<napi_value>(Str("k"));
    napi_value v = reinterpret_cast<napi_value>(Num(1));
    JsValueRef nul = JS_INVALID_REFERENCE;
    JsGetNullValue(&nul);
    napi_value nulv = reinterpret_cast<napi_value>(nul);
    JsValueRef uv = JS_INVALID_REFERENCE;
    JsGetUndefinedValue(&uv);
    napi_value unv = reinterpret_cast<napi_value>(uv);
    napi_value got = nullptr;
    bool b = false;
    uint32_t u32 = 0;
    napi_value arr = NewArr(0);
    // object_expected on JS null/undefined object args.
    expect_status(napi_get_property(g_env, nulv, k, &got), napi_object_expected,
                  "type: get_property null object_expected");
    expect_status(napi_get_property(g_env, unv, k, &got), napi_object_expected,
                  "type: get_property undef object_expected");
    expect_status(napi_set_property(g_env, nulv, k, v), napi_object_expected,
                  "type: set_property null object_expected");
    expect_status(napi_has_property(g_env, nulv, k, &b), napi_object_expected,
                  "type: has_property null object_expected");
    expect_status(napi_delete_property(g_env, nulv, k, &b), napi_object_expected,
                  "type: delete_property null object_expected");
    expect_status(napi_has_own_property(g_env, nulv, k, &b), napi_object_expected,
                  "type: has_own null object_expected");
    expect_status(napi_get_named_property(g_env, nulv, "k", &got), napi_object_expected,
                  "type: get_named null object_expected");
    expect_status(napi_set_named_property(g_env, nulv, "k", v), napi_object_expected,
                  "type: set_named null object_expected");
    expect_status(napi_has_named_property(g_env, nulv, "k", &b), napi_object_expected,
                  "type: has_named null object_expected");
    expect_status(napi_get_element(g_env, nulv, 0, &got), napi_object_expected,
                  "type: get_element null object_expected");
    expect_status(napi_set_element(g_env, nulv, 0, v), napi_object_expected,
                  "type: set_element null object_expected");
    expect_status(napi_has_element(g_env, nulv, 0, &b), napi_object_expected,
                  "type: has_element null object_expected");
    expect_status(napi_delete_element(g_env, nulv, 0, &b), napi_object_expected,
                  "type: delete_element null object_expected");
    napi_property_descriptor d{ "x", nullptr, nullptr, nullptr, nullptr, v,
                               napi_default, nullptr };
    expect_status(napi_define_properties(g_env, nulv, 1, &d), napi_object_expected,
                  "type: define_properties null object_expected");
    expect_status(napi_get_property_names(g_env, nulv, &got), napi_object_expected,
                  "type: get_property_names null object_expected");
    expect_status(napi_get_all_property_names(g_env, nulv, napi_key_own_only,
                                              napi_key_all_properties,
                                              napi_key_keep_numbers, &got),
                  napi_object_expected, "type: get_all null object_expected");
    expect_status(napi_get_prototype(g_env, nulv, &got), napi_object_expected,
                  "type: get_prototype null object_expected");
    expect_status(napi_object_freeze(g_env, nulv), napi_object_expected,
                  "type: freeze null object_expected");
    expect_status(napi_object_seal(g_env, nulv), napi_object_expected,
                  "type: seal null object_expected");
    // name_expected only on has_own (strict IsName in Node);
    // get/set/has/delete coerce numeric keys via ToPropertyKey.
    napi_value numk = reinterpret_cast<napi_value>(Num(5));
    expect_status(napi_has_own_property(g_env, o, numk, &b), napi_name_expected,
                  "type: has_own numeric name_expected");
    expect_status(napi_get_property(g_env, o, numk, &got), napi_ok,
                  "type: get_property numeric coerces (ToPropertyKey)");
    expect_status(napi_set_property(g_env, o, numk, v), napi_ok,
                  "type: set_property numeric coerces (ToPropertyKey)");
    expect_status(napi_get_property(g_env, o, numk, &got), napi_ok,
                  "type: get numeric after set ok");
    expect(got != nullptr && AsNum(got) == 1, "type: numeric key roundtrip 5->1");
    // Canonical: numeric key 5 aliases the string "5" in JS.
    expect_status(napi_get_named_property(g_env, o, "5", &got), napi_ok,
                  "type: named '5' aliases numeric 5 ok");
    expect(got != nullptr && AsNum(got) == 1, "type: '5' aliases numeric 5 value");
    expect_status(napi_has_property(g_env, o, numk, &b), napi_ok,
                  "type: has_property numeric coerces");
    expect(b, "type: has numeric true after set");
    expect_status(napi_delete_property(g_env, o, numk, &b), napi_ok,
                  "type: delete_property numeric coerces");
    expect(b, "type: delete numeric true");
    // array_expected on non-arrays.
    expect_status(napi_get_array_length(g_env, o, &u32), napi_array_expected,
                  "type: get_array_length object array_expected");
    expect_status(napi_get_array_length(g_env, reinterpret_cast<napi_value>(Str("s")), &u32),
                  napi_array_expected, "type: get_array_length string array_expected");
    expect_status(napi_get_array_length(g_env, arr, &u32), napi_ok,
                  "type: get_array_length array ok");

    // Last-error: failure records, success clears, reread does not clear.
    expect_status(napi_has_own_property(g_env, o, numk, &b), napi_name_expected,
                  "err: fail for last-error check");
    const napi_extended_error_info* ei = nullptr;
    expect_status(napi_get_last_error_info(g_env, &ei), napi_ok, "err: fetch ok");
    expect(ei != nullptr && ei->error_code == napi_name_expected,
           "err: recorded name_expected");
    expect(ei != nullptr && ei->error_message != nullptr &&
           std::strlen(ei->error_message) > 0,
           "err: message non-empty");
    const napi_extended_error_info* ei2 = nullptr;
    napi_get_last_error_info(g_env, &ei2);
    expect(ei2 != nullptr && ei2->error_code == napi_name_expected,
           "err: reread does not clear");
    napi_value okv = reinterpret_cast<napi_value>(Num(2));
    expect_status(napi_set_named_property(g_env, o, "ok", okv), napi_ok,
                  "err: success");
    expect_status(napi_get_last_error_info(g_env, &ei), napi_ok, "err: fetch2 ok");
    expect(ei != nullptr && ei->error_code == napi_ok,
           "err: success clears last_error");

    // NULL matrix (mirrors vendored test_null expectations).
    expect_status(napi_create_object(nullptr, &got), napi_invalid_arg,
                  "null: create_object null env");
    expect_status(napi_create_object(g_env, nullptr), napi_invalid_arg,
                  "null: create_object null result");
    expect_status(napi_create_array(nullptr, &got), napi_invalid_arg,
                  "null: create_array null env");
    expect_status(napi_create_array_with_length(g_env, 1, nullptr), napi_invalid_arg,
                  "null: create_array_with_length null result");
    expect_status(napi_get_array_length(nullptr, arr, &u32), napi_invalid_arg,
                  "null: get_array_length null env");
    expect_status(napi_get_array_length(g_env, nullptr, &u32), napi_invalid_arg,
                  "null: get_array_length null value");
    expect_status(napi_get_array_length(g_env, arr, nullptr), napi_invalid_arg,
                  "null: get_array_length null result");
    expect_status(napi_get_property(nullptr, o, k, &got), napi_invalid_arg,
                  "null: get_property null env");
    expect_status(napi_get_property(g_env, nullptr, k, &got), napi_invalid_arg,
                  "null: get_property null object");
    expect_status(napi_get_property(g_env, o, nullptr, &got), napi_invalid_arg,
                  "null: get_property null key");
    expect_status(napi_get_property(g_env, o, k, nullptr), napi_invalid_arg,
                  "null: get_property null result");
    expect_status(napi_set_property(g_env, o, k, nullptr), napi_invalid_arg,
                  "null: set_property null value");
    expect_status(napi_has_property(g_env, o, k, nullptr), napi_invalid_arg,
                  "null: has_property null result");
    expect_status(napi_has_own_property(g_env, o, k, nullptr), napi_invalid_arg,
                  "null: has_own null result");
    expect_status(napi_delete_property(g_env, o, k, nullptr), napi_ok,
                  "null: delete_property null result ok");
    expect_status(napi_get_named_property(g_env, o, nullptr, &got), napi_invalid_arg,
                  "null: get_named null name");
    expect_status(napi_set_named_property(g_env, o, nullptr, v), napi_invalid_arg,
                  "null: set_named null name");
    expect_status(napi_has_named_property(g_env, o, nullptr, &b), napi_invalid_arg,
                  "null: has_named null name");
    expect_status(napi_has_named_property(g_env, o, "k", nullptr), napi_invalid_arg,
                  "null: has_named null result");
    expect_status(napi_get_element(g_env, o, 0, nullptr), napi_invalid_arg,
                  "null: get_element null result");
    expect_status(napi_set_element(g_env, o, 0, nullptr), napi_invalid_arg,
                  "null: set_element null value");
    expect_status(napi_has_element(g_env, o, 0, nullptr), napi_invalid_arg,
                  "null: has_element null result");
    expect_status(napi_delete_element(g_env, o, 0, nullptr), napi_ok,
                  "null: delete_element null result ok");
    expect_status(napi_get_property_names(g_env, o, nullptr), napi_invalid_arg,
                  "null: get_property_names null result");
    expect_status(napi_get_all_property_names(g_env, o, napi_key_own_only,
                                              napi_key_all_properties,
                                              napi_key_keep_numbers, nullptr),
                  napi_invalid_arg, "null: get_all null result");
    expect_status(napi_get_prototype(g_env, o, nullptr), napi_invalid_arg,
                  "null: get_prototype null result");
    expect_status(napi_object_freeze(g_env, nullptr), napi_invalid_arg,
                  "null: freeze null object");
    expect_status(napi_object_seal(g_env, nullptr), napi_invalid_arg,
                  "null: seal null object");
    expect_status(napi_instanceof(g_env, o, v, nullptr), napi_invalid_arg,
                  "null: instanceof null result");
    expect_status(napi_instanceof(g_env, o, nullptr, &b), napi_invalid_arg,
                  "null: instanceof null ctor");
  }
  AtBaseline("types-errors-null: ends at baseline");

  // ------------------------------------------------------------------
  // 9. Root accounting deltas (explicit, incl. nested scopes).
  // ------------------------------------------------------------------
  std::fprintf(stdout, "-- case root-accounting --\n");
  {
    napi_handle_scope outer = OpenScope();
    napi_value a = NewObj();
    napi_value b = NewObj();
    expect(CancelCount() == g_baseline + 2, "roots: two objects +2");
    napi_handle_scope inner = OpenScope();
    napi_value c = NewObj();
    expect(CancelCount() == g_baseline + 3, "roots: inner +1");
    // A get_property result roots into the INNER scope.
    napi_value k = reinterpret_cast<napi_value>(Str("x"));
    napi_value got = nullptr;
    expect_status(napi_get_property(g_env, a, k, &got), napi_ok, "roots: get ok");
    expect(CancelCount() == g_baseline + 4, "roots: get result +1 in inner");
    expect_status(CloseScope(inner), napi_ok, "roots: close inner");
    expect(CancelCount() == g_baseline + 2, "roots: inner released");
    expect_status(CloseScope(outer), napi_ok, "roots: close outer");
    (void)b;
    (void)c;
    (void)got;
  }
  AtBaseline("roots: ends at baseline");
  // Nested scope helpers keep the probe's open-count in sync (CaseScope
  // already closed everything); assert the invariant explicitly.
  AtBaseline("roots: operator invariant");

  nodeapi_refs_test_destroy_env(g_env);
  expect(true, "teardown: env destroyed");
  JsSetCurrentContext(JS_INVALID_REFERENCE);
  JsDisposeRuntime(g_rt);

  std::fprintf(stdout, "OBJECTS_PROBE: %s (%d/%d)\n",
               (g_pass == g_total) ? "PASS" : "FAIL", g_pass, g_total);
  return (g_pass == g_total) ? 0 : 1;
}
