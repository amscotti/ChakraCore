//-------------------------------------------------------------------------------------------------------
// Copyright (c) ChakraCore Project Contributors. All rights reserved.
// Licensed under the MIT license. See LICENSE.txt file in the project root for full license information.
//-------------------------------------------------------------------------------------------------------
//
// Node-API scalar + string values (N2.1/N2.2) plus coercion, type checks,
// symbols, Date, BigInt (N2.6). See NodeApiValues.h for the contract,
// conversion semantics (typeof/coerce/symbol/date/bigint), and the
// ownership record.
//
// Implementation notes (Node v22.22.3 src/js_native_api_v8.cc + doc/api/n-api.md):
//   - All 19 functions use CHECK_ENV_NOT_IN_GC (no pending gate): the shared
//     EntryGuard runs with allowPending=true. Success clears last_error via
//     napi_clear_last_error (AMB-STATUS-1); failure records it.
//   - Type pre-checks use JsGetValueType (JsNumber/JsBoolean/JsString) and
//     return the specific *_expected status before any JsRT conversion call,
//     so JsRT's collapsed JsErrorInvalidArgument never masks the code (plan
//     section 3.10 / PR-12).
//   - Number conversion is via double only (never JsNumberToInt / ToInt32):
//       create: (double) of the C value -> JsDoubleToNumber. int64 loses
//         precision outside +-(2**53-1) (doc napi_create_int64).
//       get int32:  non-finite => 0; trunc -> bottom 32 bits (doc + Int32Value).
//       get uint32: non-finite => 0; trunc -> modulo 2**32 (Uint32Value).
//       get int64:  non-finite => 0 (impl special case: IntegerValue maps
//         non-finite to INT64_MIN, so Node returns 0); finite trunc with
//         saturation to [INT64_MIN, INT64_MAX] (IntegerValue behaviour;
//         wrapping modulo 2**64 is NOT used - differential: 1e20 -> INT64_MAX).
//       get double: exact Number::Value.
//     All three integer getters truncate toward zero (C++ trunc), matching
//     Int32Value/Uint32Value/IntegerValue and the differential:
//     i32(-1.5) => -1, u32(-1.5) => UINT32_MAX, i32(2**31) => INT32_MIN,
//     u32(2**32) => 0. Differential was taken against Node v26.5.0.
//   - Strings: JsCreateString / JsCreateStringUtf16 for create;
//     JsCopyString (UTF-8) / JsCopyStringUtf16 (UTF-16) for get, with
//     Latin1 transcoded through UTF-16 in both directions. JsCopyString
//     does not null-terminate (WideStringToNarrowNoAlloc -> EncodeInto
//     without AndNullTerminate), so get paths null-terminate explicitly.
//     JsCopyString -> EncodeInto asserts (does NOT truncate) when the
//     destination is too small, so UTF-8 truncation goes via a
//     full-length temporary (size query first, exact copy, manual
//     truncation to bufsize-1 without splitting a sequence - V8 WriteUtf8
//     "Will not write partial UTF-8 sequences").
//   - Latin1 get maps each UTF-16 code unit to its low byte (truncation),
//     matching V8 WriteOneByte (differential: U+0100 -> 0x00, U+1F600 ->
//     0x3D 0x00). JsCopyStringOneByte (also (char) cast) is therefore NOT
//     called directly; the transcoding is explicit via JsCopyStringUtf16.

#include "NodeApiValues.h"

#include "NodeApiEnv.h"
#include "NodeApiScopes.h"
#include "ChakraCommon.h"
#include "ChakraCore.h"

#include <climits>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <limits>
#include <new>
#include <string>
#include <vector>

namespace {

napi_status map_js(napi_env env, JsErrorCode ec, const char* what) {
  if (ec == JsNoError) return napi_ok;
  if (ec == JsErrorInExceptionState || ec == JsErrorScriptException) {
    nodeapi::SetLastError(env, napi_pending_exception, what);
    return napi_pending_exception;
  }
  if (ec == JsErrorInvalidArgument || ec == JsErrorNullArgument) {
    nodeapi::SetLastError(env, napi_invalid_arg, what);
    return napi_invalid_arg;
  }
  nodeapi::SetLastError(env, napi_generic_failure, what);
  return napi_generic_failure;
}

/* Root into innermost scope (R1) and publish. On rooting failure the
 * caller's out-param is left untouched and generic_failure is returned. */
napi_status root_publish(napi_env env, JsValueRef v, napi_value* result,
                         const char* what) {
  napi_value nv = reinterpret_cast<napi_value>(v);
  napi_status rs = NodeApiScopes_RootIntoInnermost(env, nv);
  if (rs != napi_ok) {
    return rs;
  }
  *result = nv;
  nodeapi::ClearLastError(env);
  return napi_ok;
}

/* --- double -> integer conversions (Node Int32Value/Uint32Value/IntegerValue) --- */

int32_t to_int32(double d) {
  if (!std::isfinite(d)) return 0;
  double t = std::trunc(d);
  double m = std::fmod(t, 4294967296.0);
  if (m < 0) m += 4294967296.0;
  if (m >= 2147483648.0) return static_cast<int32_t>(m - 4294967296.0);
  return static_cast<int32_t>(m);
}

uint32_t to_uint32(double d) {
  if (!std::isfinite(d)) return 0;
  double t = std::trunc(d);
  double m = std::fmod(t, 4294967296.0);
  if (m < 0) m += 4294967296.0;
  return static_cast<uint32_t>(m);
}

int64_t to_int64(double d) {
  if (!std::isfinite(d)) return 0;
  double t = std::trunc(d);
  if (t >= 9223372036854775808.0) {
    return std::numeric_limits<int64_t>::max();
  }
  if (t <= -9223372036854775808.0) {
    return std::numeric_limits<int64_t>::min();
  }
  return static_cast<int64_t>(t);
}

size_t utf16_auto_len(const char16_t* s) {
  size_t n = 0;
  while (s[n] != 0) ++n;
  return n;
}

/* Create path: validate create-string length + str (Node
 * CHECK_NEW_STRING_ARGS). Returns napi_ok and sets *out_len to the
 * resolved length on success; otherwise sets last_error and returns the
 * failure status (out-param untouched). */
napi_status check_create_string(napi_env env, const void* str, size_t length,
                                size_t* out_len) {
  if (length != NAPI_AUTO_LENGTH && length > static_cast<size_t>(INT_MAX)) {
    nodeapi::SetLastError(env, napi_invalid_arg,
                          "create_string: length exceeds INT_MAX");
    return napi_invalid_arg;
  }
  if (length > 0 && str == nullptr) {
    nodeapi::SetLastError(env, napi_invalid_arg,
                          "create_string: str is NULL with non-zero length");
    return napi_invalid_arg;
  }
  if (length == NAPI_AUTO_LENGTH) {
    *out_len = std::strlen(static_cast<const char*>(str));
  } else {
    *out_len = length;
  }
  return napi_ok;
}

/* --- N2.6 helpers: transient roots, intrinsics, date/bigint pieces --- */

/* Transient root (R1): JsAddRef on construction, JsRelease at the end of
 * the napi_* call. For intrinsic lookups and intermediate values that
 * must survive a following JsRT/JsCallFunction call but are neither
 * published nor rooted into the scope stack. Mirrors NodeApiObjects'
 * ScratchRoot (which lives in that TU). Never spans a scope close. */
class TmpRoot {
 public:
  explicit TmpRoot(JsValueRef v) : v_(v), owned_(false) {
    if (v_ != JS_INVALID_REFERENCE) {
      if (JsAddRef(v_, nullptr) == JsNoError) owned_ = true;
    }
  }
  ~TmpRoot() { reset(); }
  TmpRoot(const TmpRoot&) = delete;
  TmpRoot& operator=(const TmpRoot&) = delete;
  TmpRoot(TmpRoot&& other) noexcept : v_(other.v_), owned_(other.owned_) {
    other.v_ = JS_INVALID_REFERENCE;
    other.owned_ = false;
  }
  TmpRoot& operator=(TmpRoot&& other) noexcept {
    if (this != &other) {
      reset();
      v_ = other.v_;
      owned_ = other.owned_;
      other.v_ = JS_INVALID_REFERENCE;
      other.owned_ = false;
    }
    return *this;
  }
  JsValueRef get() const { return v_; }
  void reset() {
    if (owned_ && v_ != JS_INVALID_REFERENCE) {
      (void)JsRelease(v_, nullptr);
      owned_ = false;
    }
    v_ = JS_INVALID_REFERENCE;
  }

 private:
  JsValueRef v_;
  bool owned_;
};

/* Conversion cap for the words->decimal path (PR-9 string-mediated
 * BigInt): the base-1e9 division loop is O(words^2), so an uncapped
 * word_count (up to INT_MAX, e.g. the vendored TestWordsThrow case)
 * would try to materialize gigabytes of digits. 16384 words (128 KiB
 * of limbs, ~315 k digits) keeps the worst case bounded at well under
 * a second while covering every realistic use (the vendored corpus
 * tops out at 4 words). Beyond the cap: pending RangeError
 * "Maximum BigInt size exceeded" (V8's message; exact text is not
 * conformance per PR-12). V8 itself accepts larger values — a
 * documented v1 limit of the JS-mediated path, not a semantic claim. */
constexpr size_t kMaxBigIntWords = 16384;

napi_status MakePid(napi_env env, const char* name, JsPropertyIdRef* out,
                    const char* what) {
  JsErrorCode e = JsCreatePropertyId(name, std::strlen(name), out);
  if (e != JsNoError) return map_js(env, e, what);
  return napi_ok;
}

/* Fetch global.<name> as a borrowed JsValueRef (no root). The caller
 * roots with TmpRoot immediately if it must survive a following call. */
napi_status GetGlobalProp(napi_env env, const char* name, JsValueRef* out,
                          const char* what) {
  JsValueRef global = JS_INVALID_REFERENCE;
  JsErrorCode e = JsGetGlobalObject(&global);
  if (e != JsNoError) return map_js(env, e, what);
  JsPropertyIdRef pid = JS_INVALID_REFERENCE;
  napi_status s = MakePid(env, name, &pid, what);
  if (s != napi_ok) return s;
  JsValueRef v = JS_INVALID_REFERENCE;
  e = JsGetProperty(global, pid, &v);
  if (e != JsNoError) return map_js(env, e, what);
  *out = v;
  return napi_ok;
}

/* True iff the value is a JsCreateExternalObject value (napi_external).
 * JsHasExternalData is external-object-ness (true even for NULL data).
 * Empirically a proxy wrapping an external reports false (this build
 * does not expose the target), matching Node's IsExternal on proxies. */
bool IsExternalValue(JsValueRef jsv) {
  bool has = false;
  if (JsHasExternalData(jsv, &has) != JsNoError) return false;
  return has;
}

/* Primitive-BigInt test (PR-9 + MED-1 fix): Object.prototype.toString
 * tag == "[object BigInt]" AND JsConvertValueToObject identity
 * (fresh wrapper => primitive; same object => boxed). Runs the
 * standard toString call with this=jsv (Object.prototype.toString.call
 * semantics). Caller has already established JsGetValueType==JsObject,
 * so primitives never pay this. Pending-world failures
 * (InExceptionState/ScriptException from the tag call) degrade to
 * *out=false so C-Yes callers keep their napi_ok contract; genuine
 * JsRT failures (OOM/invalid-arg) are mapped. (Defensive: this build's
 * JsCallFunction tolerates a pending exception — verified in the probe
 * — so the degradation branch is cold here.) */
napi_status TagIsBigInt(napi_env env, JsValueRef jsv, bool* out) {
  *out = false;
  JsValueRef objCtor = JS_INVALID_REFERENCE;
  napi_status s = GetGlobalProp(env, "Object", &objCtor, "bigint-tag: global Object");
  if (s != napi_ok) return s;
  TmpRoot ctorRoot(objCtor);
  JsPropertyIdRef protoPid = JS_INVALID_REFERENCE;
  s = MakePid(env, "prototype", &protoPid, "bigint-tag: prototype id");
  if (s != napi_ok) return s;
  JsValueRef proto = JS_INVALID_REFERENCE;
  JsErrorCode ec = JsGetProperty(objCtor, protoPid, &proto);
  if (ec != JsNoError) return map_js(env, ec, "bigint-tag: Object.prototype");
  TmpRoot protoRoot(proto);
  JsPropertyIdRef tsPid = JS_INVALID_REFERENCE;
  s = MakePid(env, "toString", &tsPid, "bigint-tag: toString id");
  if (s != napi_ok) return s;
  JsValueRef fn = JS_INVALID_REFERENCE;
  ec = JsGetProperty(proto, tsPid, &fn);
  if (ec != JsNoError) return map_js(env, ec, "bigint-tag: toString fn");
  TmpRoot fnRoot(fn);
  JsValueType ft = JsUndefined;
  ec = JsGetValueType(fn, &ft);
  if (ec != JsNoError) return map_js(env, ec, "bigint-tag: toString type");
  if (ft != JsFunction) {
    nodeapi::SetLastError(env, napi_generic_failure,
                          "bigint-tag: Object.prototype.toString tampered");
    return napi_generic_failure;
  }
  JsValueRef args[1] = {jsv};
  JsValueRef res = JS_INVALID_REFERENCE;
  ec = JsCallFunction(fn, args, 1, &res);
  if (ec != JsNoError) {
    if (ec == JsErrorInExceptionState || ec == JsErrorScriptException) {
      return napi_ok; /* degraded: not bigint; caller clears on success */
    }
    return map_js(env, ec, "bigint-tag: toString call");
  }
  TmpRoot resRoot(res);
  JsValueType rt = JsUndefined;
  ec = JsGetValueType(res, &rt);
  if (ec != JsNoError) return map_js(env, ec, "bigint-tag: tag type");
  if (rt != JsString) {
    nodeapi::SetLastError(env, napi_generic_failure,
                          "bigint-tag: toString tag not a string");
    return napi_generic_failure;
  }
  size_t need = 0;
  ec = JsCopyString(res, nullptr, 0, &need);
  if (ec != JsNoError) return map_js(env, ec, "bigint-tag: tag size");
  if (need == SIZE_MAX) {
    nodeapi::SetLastError(env, napi_generic_failure,
                          "bigint-tag: tag size overflow");
    return napi_generic_failure;
  }
  std::vector<char> buf;
  try {
    buf.assign(need + 1, '\0');
  } catch (...) {
    nodeapi::SetLastError(env, napi_generic_failure,
                          "bigint-tag: tag buffer OOM");
    return napi_generic_failure;
  }
  size_t written = 0;
  ec = JsCopyString(res, buf.data(), buf.size(), &written);
  if (ec != JsNoError) return map_js(env, ec, "bigint-tag: tag copy");
  if (!(written == 15 && std::memcmp(buf.data(), "[object BigInt]", 15) == 0)) {
    return napi_ok; /* *out == false: not BigInt */
  }
  // MED-1: the tag alone cannot split primitive BigInt from a boxed
  // BigInt (Object(1n) also tags "[object BigInt]" but Node reports
  // napi_object). Discriminate via ToObject identity: JsConvertValue
  // ToObject returns the SAME object for objects (boxed) and a fresh
  // wrapper for primitives. StrictEquals checks spec identity, not
  // handle-pointer equality. No user code runs (pure boxing), so this
  // is safe for C-Yes callers. Verified empirically (2026-11-12):
  // prim==wrapper? false; boxed==boxed? true (pointer + StrictEquals).
  JsValueRef asObj = JS_INVALID_REFERENCE;
  ec = JsConvertValueToObject(jsv, &asObj);
  if (ec != JsNoError) {
    if (ec == JsErrorInExceptionState || ec == JsErrorScriptException) {
      return napi_ok; /* degraded: *out == false (object) */
    }
    return map_js(env, ec, "bigint-tag: ToObject");
  }
  TmpRoot asObjRoot(asObj);
  bool ident = false;
  ec = JsStrictEquals(jsv, asObj, &ident);
  if (ec != JsNoError) {
    if (ec == JsErrorInExceptionState || ec == JsErrorScriptException) {
      return napi_ok; /* degraded: *out == false (object) */
    }
    return map_js(env, ec, "bigint-tag: identity");
  }
  *out = !ident; /* primitive (fresh wrapper) => true; boxed => false */
  return napi_ok;
}

/* Shared bigint pre-check for the get_value_bigint_* getters:
 * napi_bigint_expected unless the value classifies as a PRIMITIVE
 * BigInt via the toString tag + ToObject-identity discrimination
 * (MED-1 fix; boxed BigInt Object(1n) is napi_bigint_expected, as
 * Node returns). TagIsBigInt reports true only for primitives. */
napi_status RequireBigInt(napi_env env, JsValueRef jsv, const char* what) {
  JsValueType t = JsUndefined;
  JsErrorCode ec = JsGetValueType(jsv, &t);
  if (ec != JsNoError) return map_js(env, ec, what);
  if (t != JsObject) {
    nodeapi::SetLastError(env, napi_bigint_expected, what);
    return napi_bigint_expected;
  }
  if (IsExternalValue(jsv)) {
    nodeapi::SetLastError(env, napi_bigint_expected, what);
    return napi_bigint_expected;
  }
  bool big = false;
  napi_status s = TagIsBigInt(env, jsv, &big);
  if (s != napi_ok) return s;
  if (!big) {
    nodeapi::SetLastError(env, napi_bigint_expected, what);
    return napi_bigint_expected;
  }
  return napi_ok;
}

/* Prototype-chain test for the context's Date.prototype (N2.5 pattern,
 * shared with IsErrorImpl there): no user code (unlike JsInstanceOf),
 * so it is safe for C-Yes callers. A missing/tampered global Date
 * reports false rather than failing. */
napi_status IsDateImpl(napi_env env, JsValueRef v, bool* out) {
  *out = false;
  JsValueType t = JsObject;
  JsErrorCode ec = JsGetValueType(v, &t);
  if (ec != JsNoError) return map_js(env, ec, "IsDate: type query");
  switch (t) {
    case JsUndefined:
    case JsNull:
    case JsNumber:
    case JsString:
    case JsBoolean:
    case JsSymbol:
      nodeapi::ClearLastError(env);
      return napi_ok; /* primitives cannot be dates */
    case JsObject:
    case JsFunction:
    case JsArray:
    case JsArrayBuffer:
    case JsTypedArray:
    case JsDataView:
      break; /* walk below */
    default:
      nodeapi::ClearLastError(env);
      return napi_ok;
  }
  JsValueRef dateCtor = JS_INVALID_REFERENCE;
  napi_status s = GetGlobalProp(env, "Date", &dateCtor, "is_date: global Date");
  if (s != napi_ok) return s;
  TmpRoot ctorRoot(dateCtor);
  JsValueType ctorType = JsObject;
  if (JsGetValueType(dateCtor, &ctorType) != JsNoError ||
      ctorType != JsFunction) {
    nodeapi::ClearLastError(env);
    return napi_ok; /* tampered global: report false, do not fail */
  }
  JsPropertyIdRef protoPid = JS_INVALID_REFERENCE;
  s = MakePid(env, "prototype", &protoPid, "is_date: prototype id");
  if (s != napi_ok) return s;
  JsValueRef dateProto = JS_INVALID_REFERENCE;
  ec = JsGetProperty(dateCtor, protoPid, &dateProto);
  if (ec != JsNoError) return map_js(env, ec, "is_date: Date.prototype");
  if (dateProto == JS_INVALID_REFERENCE) {
    nodeapi::ClearLastError(env);
    return napi_ok;
  }
  TmpRoot protoRoot(dateProto);
  JsValueRef cur = v;
  for (int i = 0; i < 100; ++i) {
    JsValueRef next = JS_INVALID_REFERENCE;
    ec = JsGetPrototype(cur, &next);
    if (ec != JsNoError) return map_js(env, ec, "is_date: prototype walk");
    if (next == JS_INVALID_REFERENCE) break;
    if (next == dateProto) {
      *out = true;
      nodeapi::ClearLastError(env);
      return napi_ok;
    }
    JsValueType nt = JsObject;
    ec = JsGetValueType(next, &nt);
    if (ec != JsNoError) return map_js(env, ec, "is_date: proto type");
    if (nt == JsNull || nt == JsUndefined) break;
    cur = next;
  }
  nodeapi::ClearLastError(env);
  return napi_ok;
}

/* Decimal string of a BigInt value via ToString (primitive BigInt:
 * pure decimal; boxed: user toString, validated by the parser). */
napi_status BigIntDecimal(napi_env env, JsValueRef jsv, std::string* out,
                          const char* what) {
  JsValueRef sv = JS_INVALID_REFERENCE;
  JsErrorCode ec = JsConvertValueToString(jsv, &sv);
  if (ec != JsNoError) return map_js(env, ec, what);
  TmpRoot svRoot(sv);
  size_t need = 0;
  ec = JsCopyString(sv, nullptr, 0, &need);
  if (ec != JsNoError) return map_js(env, ec, what);
  if (need == SIZE_MAX) {
    nodeapi::SetLastError(env, napi_generic_failure,
                          "bigint-decimal: size overflow");
    return napi_generic_failure;
  }
  std::vector<char> buf;
  try {
    buf.assign(need + 1, '\0');
  } catch (...) {
    nodeapi::SetLastError(env, napi_generic_failure,
                          "bigint-decimal: buffer OOM");
    return napi_generic_failure;
  }
  size_t written = 0;
  ec = JsCopyString(sv, buf.data(), buf.size(), &written);
  if (ec != JsNoError) return map_js(env, ec, what);
  try {
    out->assign(buf.data(), written);
  } catch (...) {
    nodeapi::SetLastError(env, napi_generic_failure,
                          "bigint-decimal: string OOM");
    return napi_generic_failure;
  }
  return napi_ok;
}

/* Parse "-?digits" into sign + minimal little-endian limbs (zero =>
 * empty limbs, *neg false). Returns false on non-decimal input (only
 * reachable via a tampered boxed-BigInt toString). OOM sets *oom_out
 * (when non-NULL) and also returns false; callers must check oom_out
 * first and map to napi_generic_failure (H1: bad_alloc/length_error
 * must not escape the extern "C" boundary). */
bool ParseDecimal(const std::string& s, bool* neg,
                  std::vector<uint64_t>* limbs, bool* oom_out = nullptr) {
  *neg = false;
  if (oom_out) *oom_out = false;
  limbs->clear();
  size_t i = 0;
  if (!s.empty() && s[0] == '-') {
    *neg = true;
    i = 1;
  }
  if (i >= s.size()) return false;
  try {
    for (; i < s.size(); ++i) {
      char c = s[i];
      if (c < '0' || c > '9') return false;
      unsigned __int128 carry = static_cast<unsigned>(c - '0');
      for (size_t j = 0; j < limbs->size(); ++j) {
        unsigned __int128 cur =
            static_cast<unsigned __int128>((*limbs)[j]) * 10 + carry;
        (*limbs)[j] = static_cast<uint64_t>(cur);
        carry = cur >> 64;
      }
      while (carry != 0) {
        limbs->push_back(static_cast<uint64_t>(carry));
        carry >>= 64;
      }
    }
  } catch (...) {
    if (oom_out) *oom_out = true;
    return false;
  }
  while (!limbs->empty() && limbs->back() == 0) limbs->pop_back();
  if (limbs->empty()) *neg = false; /* normalize "-0" */
  return true;
}

/* Little-endian limbs + sign -> decimal string (BigInt(str) input).
 * Input may carry leading zero limbs; zero magnitude => "0".
 * Returns true on success with *out set; false on OOM (bad_alloc /
 * length_error caught internally, H1: must not escape extern "C").
 * Callers map false to napi_generic_failure. */
bool WordsToDecimal(bool neg, const uint64_t* words, size_t count,
                    std::string* out) {
  try {
    std::vector<uint64_t> cur;
    if (count > 0) cur.assign(words, words + count);
    while (!cur.empty() && cur.back() == 0) cur.pop_back();
    if (cur.empty()) {
      out->assign("0");
      return true;
    }
    const uint32_t kBase = 1000000000u;
    std::vector<uint32_t> groups;
    while (!cur.empty()) {
      unsigned __int128 rem = 0;
      for (size_t i = cur.size(); i-- > 0;) {
        unsigned __int128 v = (rem << 64) + cur[i];
        cur[i] = static_cast<uint64_t>(v / kBase);
        rem = v % kBase;
      }
      groups.push_back(static_cast<uint32_t>(rem));
      while (!cur.empty() && cur.back() == 0) cur.pop_back();
    }
    std::string s;
    if (neg) s.push_back('-');
    s += std::to_string(groups.back());
    for (size_t i = groups.size() - 1; i-- > 0;) {
      char g[16];
      std::snprintf(g, sizeof(g), "%09u", groups[i]);
      s += g;
    }
    *out = std::move(s);
    return true;
  } catch (...) {
    return false;
  }
}

/* Create a BigInt from a decimal string via the global BigInt function
 * (PR-9/D8) and publish rooted. */
napi_status CreateBigIntFromDecimal(napi_env env, const std::string& decimal,
                                    napi_value* result, const char* what) {
  JsValueRef fn = JS_INVALID_REFERENCE;
  napi_status s = GetGlobalProp(env, "BigInt", &fn, what);
  if (s != napi_ok) return s;
  TmpRoot fnRoot(fn);
  JsValueType ft = JsUndefined;
  JsErrorCode ec = JsGetValueType(fn, &ft);
  if (ec != JsNoError) return map_js(env, ec, what);
  if (ft != JsFunction) {
    nodeapi::SetLastError(env, napi_generic_failure,
                          "bigint: global BigInt not a function");
    return napi_generic_failure;
  }
  JsValueRef str = JS_INVALID_REFERENCE;
  ec = JsCreateString(decimal.data(), decimal.size(), &str);
  if (ec != JsNoError) return map_js(env, ec, what);
  TmpRoot strRoot(str);
  JsValueRef args[2] = {fn, str}; /* this = fn (ignored by BigInt) */
  JsValueRef v = JS_INVALID_REFERENCE;
  ec = JsCallFunction(fn, args, 2, &v);
  if (ec != JsNoError) return map_js(env, ec, what);
  return root_publish(env, v, result, what);
}

/* Shared int64/uint64 get body. Result is the low 64 bits modulo 2^64;
 * lossless encodes the fit check (signed: -2^63 <= v < 2^63; unsigned:
 * 0 <= v < 2^64). Node-differential verified. */
napi_status GetBigInt64Common(napi_env env, napi_value value, bool signed_goal,
                              uint64_t* bits_out, bool* lossless_out,
                              const char* what) {
  JsValueRef jsv = reinterpret_cast<JsValueRef>(value);
  napi_status s = RequireBigInt(env, jsv, what);
  if (s != napi_ok) return s;
  std::string decimal;
  s = BigIntDecimal(env, jsv, &decimal, what);
  if (s != napi_ok) return s;
  bool neg = false;
  std::vector<uint64_t> limbs;
  bool parse_oom = false;
  if (!ParseDecimal(decimal, &neg, &limbs, &parse_oom)) {
    if (parse_oom) {
      nodeapi::SetLastError(env, napi_generic_failure,
                            "bigint: decimal-parse OOM");
      return napi_generic_failure;
    }
    nodeapi::SetLastError(env, napi_bigint_expected,
                          "bigint: ToString is not a decimal integer");
    return napi_bigint_expected;
  }
  uint64_t low = limbs.empty() ? 0 : limbs[0];
  *bits_out = neg ? static_cast<uint64_t>(0 - low) : low;
  if (signed_goal) {
    if (limbs.size() > 1) {
      *lossless_out = false;
    } else if (!neg) {
      *lossless_out = (low <= 9223372036854775807ull);
    } else {
      *lossless_out = (low <= 9223372036854775808ull);
    }
  } else {
    *lossless_out = (!neg && limbs.size() <= 1);
  }
  return napi_ok;
}

} /* namespace */

extern "C" {

/* --- Getters for defined singletons --- */

napi_status NAPI_CDECL napi_get_undefined(napi_env env, napi_value* result) {
  if (env == nullptr || result == nullptr) {
    if (env != nullptr) {
      nodeapi::SetLastError(env, napi_invalid_arg, "napi_get_undefined: null arg");
    }
    return napi_invalid_arg;
  }
  nodeapi::EntryGuard guard(env, /*allowClosing=*/false,
                             /*allowPending=*/true, /*needContext=*/true);
  if (!guard.ok()) return guard.status();
  JsValueRef v = JS_INVALID_REFERENCE;
  JsErrorCode ec = JsGetUndefinedValue(&v);
  if (ec != JsNoError) return map_js(env, ec, "JsGetUndefinedValue");
  return root_publish(env, v, result, "napi_get_undefined: root failed");
}

napi_status NAPI_CDECL napi_get_null(napi_env env, napi_value* result) {
  if (env == nullptr || result == nullptr) {
    if (env != nullptr) {
      nodeapi::SetLastError(env, napi_invalid_arg, "napi_get_null: null arg");
    }
    return napi_invalid_arg;
  }
  nodeapi::EntryGuard guard(env, false, true, true);
  if (!guard.ok()) return guard.status();
  JsValueRef v = JS_INVALID_REFERENCE;
  JsErrorCode ec = JsGetNullValue(&v);
  if (ec != JsNoError) return map_js(env, ec, "JsGetNullValue");
  return root_publish(env, v, result, "napi_get_null: root failed");
}

napi_status NAPI_CDECL napi_get_global(napi_env env, napi_value* result) {
  if (env == nullptr || result == nullptr) {
    if (env != nullptr) {
      nodeapi::SetLastError(env, napi_invalid_arg, "napi_get_global: null arg");
    }
    return napi_invalid_arg;
  }
  nodeapi::EntryGuard guard(env, false, true, true);
  if (!guard.ok()) return guard.status();
  JsValueRef v = JS_INVALID_REFERENCE;
  JsErrorCode ec = JsGetGlobalObject(&v);
  if (ec != JsNoError) return map_js(env, ec, "JsGetGlobalObject");
  return root_publish(env, v, result, "napi_get_global: root failed");
}

napi_status NAPI_CDECL napi_get_boolean(napi_env env, bool value,
                                        napi_value* result) {
  if (env == nullptr || result == nullptr) {
    if (env != nullptr) {
      nodeapi::SetLastError(env, napi_invalid_arg, "napi_get_boolean: null arg");
    }
    return napi_invalid_arg;
  }
  nodeapi::EntryGuard guard(env, false, true, true);
  if (!guard.ok()) return guard.status();
  JsValueRef v = JS_INVALID_REFERENCE;
  JsErrorCode ec = value ? JsGetTrueValue(&v) : JsGetFalseValue(&v);
  if (ec != JsNoError) return map_js(env, ec, "JsGetTrue/FalseValue");
  return root_publish(env, v, result, "napi_get_boolean: root failed");
}

/* --- Create numbers (via double) --- */

napi_status NAPI_CDECL napi_create_double(napi_env env, double value,
                                            napi_value* result) {
  if (env == nullptr || result == nullptr) {
    if (env != nullptr) {
      nodeapi::SetLastError(env, napi_invalid_arg, "napi_create_double: null arg");
    }
    return napi_invalid_arg;
  }
  nodeapi::EntryGuard guard(env, false, true, true);
  if (!guard.ok()) return guard.status();
  JsValueRef v = JS_INVALID_REFERENCE;
  JsErrorCode ec = JsDoubleToNumber(value, &v);
  if (ec != JsNoError) return map_js(env, ec, "JsDoubleToNumber");
  return root_publish(env, v, result, "napi_create_double: root failed");
}

napi_status NAPI_CDECL napi_create_int32(napi_env env, int32_t value,
                                           napi_value* result) {
  if (env == nullptr || result == nullptr) {
    if (env != nullptr) {
      nodeapi::SetLastError(env, napi_invalid_arg, "napi_create_int32: null arg");
    }
    return napi_invalid_arg;
  }
  nodeapi::EntryGuard guard(env, false, true, true);
  if (!guard.ok()) return guard.status();
  JsValueRef v = JS_INVALID_REFERENCE;
  JsErrorCode ec = JsDoubleToNumber(static_cast<double>(value), &v);
  if (ec != JsNoError) return map_js(env, ec, "JsDoubleToNumber (int32)");
  return root_publish(env, v, result, "napi_create_int32: root failed");
}

napi_status NAPI_CDECL napi_create_uint32(napi_env env, uint32_t value,
                                            napi_value* result) {
  if (env == nullptr || result == nullptr) {
    if (env != nullptr) {
      nodeapi::SetLastError(env, napi_invalid_arg, "napi_create_uint32: null arg");
    }
    return napi_invalid_arg;
  }
  nodeapi::EntryGuard guard(env, false, true, true);
  if (!guard.ok()) return guard.status();
  JsValueRef v = JS_INVALID_REFERENCE;
  JsErrorCode ec = JsDoubleToNumber(static_cast<double>(value), &v);
  if (ec != JsNoError) return map_js(env, ec, "JsDoubleToNumber (uint32)");
  return root_publish(env, v, result, "napi_create_uint32: root failed");
}

napi_status NAPI_CDECL napi_create_int64(napi_env env, int64_t value,
                                           napi_value* result) {
  if (env == nullptr || result == nullptr) {
    if (env != nullptr) {
      nodeapi::SetLastError(env, napi_invalid_arg, "napi_create_int64: null arg");
    }
    return napi_invalid_arg;
  }
  nodeapi::EntryGuard guard(env, false, true, true);
  if (!guard.ok()) return guard.status();
  JsValueRef v = JS_INVALID_REFERENCE;
  JsErrorCode ec = JsDoubleToNumber(static_cast<double>(value), &v);
  if (ec != JsNoError) return map_js(env, ec, "JsDoubleToNumber (int64)");
  return root_publish(env, v, result, "napi_create_int64: root failed");
}

/* --- Get numbers/bool --- */

napi_status NAPI_CDECL napi_get_value_double(napi_env env, napi_value value,
                                               double* result) {
  if (env == nullptr || value == nullptr || result == nullptr) {
    if (env != nullptr) {
      nodeapi::SetLastError(env, napi_invalid_arg, "napi_get_value_double: null arg");
    }
    return napi_invalid_arg;
  }
  nodeapi::EntryGuard guard(env, false, true, true);
  if (!guard.ok()) return guard.status();
  JsValueRef jsv = reinterpret_cast<JsValueRef>(value);
  JsValueType t = JsUndefined;
  JsErrorCode ec = JsGetValueType(jsv, &t);
  if (ec != JsNoError) return map_js(env, ec, "JsGetValueType");
  if (t != JsNumber) {
    nodeapi::SetLastError(env, napi_number_expected,
                          "napi_get_value_double: not a number");
    return napi_number_expected;
  }
  double d = 0;
  ec = JsNumberToDouble(jsv, &d);
  if (ec != JsNoError) return map_js(env, ec, "JsNumberToDouble");
  *result = d;
  nodeapi::ClearLastError(env);
  return napi_ok;
}

napi_status NAPI_CDECL napi_get_value_int32(napi_env env, napi_value value,
                                              int32_t* result) {
  if (env == nullptr || value == nullptr || result == nullptr) {
    if (env != nullptr) {
      nodeapi::SetLastError(env, napi_invalid_arg, "napi_get_value_int32: null arg");
    }
    return napi_invalid_arg;
  }
  nodeapi::EntryGuard guard(env, false, true, true);
  if (!guard.ok()) return guard.status();
  JsValueRef jsv = reinterpret_cast<JsValueRef>(value);
  JsValueType t = JsUndefined;
  JsErrorCode ec = JsGetValueType(jsv, &t);
  if (ec != JsNoError) return map_js(env, ec, "JsGetValueType");
  if (t != JsNumber) {
    nodeapi::SetLastError(env, napi_number_expected,
                          "napi_get_value_int32: not a number");
    return napi_number_expected;
  }
  double d = 0;
  ec = JsNumberToDouble(jsv, &d);
  if (ec != JsNoError) return map_js(env, ec, "JsNumberToDouble");
  *result = to_int32(d);
  nodeapi::ClearLastError(env);
  return napi_ok;
}

napi_status NAPI_CDECL napi_get_value_uint32(napi_env env, napi_value value,
                                               uint32_t* result) {
  if (env == nullptr || value == nullptr || result == nullptr) {
    if (env != nullptr) {
      nodeapi::SetLastError(env, napi_invalid_arg, "napi_get_value_uint32: null arg");
    }
    return napi_invalid_arg;
  }
  nodeapi::EntryGuard guard(env, false, true, true);
  if (!guard.ok()) return guard.status();
  JsValueRef jsv = reinterpret_cast<JsValueRef>(value);
  JsValueType t = JsUndefined;
  JsErrorCode ec = JsGetValueType(jsv, &t);
  if (ec != JsNoError) return map_js(env, ec, "JsGetValueType");
  if (t != JsNumber) {
    nodeapi::SetLastError(env, napi_number_expected,
                          "napi_get_value_uint32: not a number");
    return napi_number_expected;
  }
  double d = 0;
  ec = JsNumberToDouble(jsv, &d);
  if (ec != JsNoError) return map_js(env, ec, "JsNumberToDouble");
  *result = to_uint32(d);
  nodeapi::ClearLastError(env);
  return napi_ok;
}

napi_status NAPI_CDECL napi_get_value_int64(napi_env env, napi_value value,
                                              int64_t* result) {
  if (env == nullptr || value == nullptr || result == nullptr) {
    if (env != nullptr) {
      nodeapi::SetLastError(env, napi_invalid_arg, "napi_get_value_int64: null arg");
    }
    return napi_invalid_arg;
  }
  nodeapi::EntryGuard guard(env, false, true, true);
  if (!guard.ok()) return guard.status();
  JsValueRef jsv = reinterpret_cast<JsValueRef>(value);
  JsValueType t = JsUndefined;
  JsErrorCode ec = JsGetValueType(jsv, &t);
  if (ec != JsNoError) return map_js(env, ec, "JsGetValueType");
  if (t != JsNumber) {
    nodeapi::SetLastError(env, napi_number_expected,
                          "napi_get_value_int64: not a number");
    return napi_number_expected;
  }
  double d = 0;
  ec = JsNumberToDouble(jsv, &d);
  if (ec != JsNoError) return map_js(env, ec, "JsNumberToDouble");
  *result = to_int64(d);
  nodeapi::ClearLastError(env);
  return napi_ok;
}

napi_status NAPI_CDECL napi_get_value_bool(napi_env env, napi_value value,
                                             bool* result) {
  if (env == nullptr || value == nullptr || result == nullptr) {
    if (env != nullptr) {
      nodeapi::SetLastError(env, napi_invalid_arg, "napi_get_value_bool: null arg");
    }
    return napi_invalid_arg;
  }
  nodeapi::EntryGuard guard(env, false, true, true);
  if (!guard.ok()) return guard.status();
  JsValueRef jsv = reinterpret_cast<JsValueRef>(value);
  JsValueType t = JsUndefined;
  JsErrorCode ec = JsGetValueType(jsv, &t);
  if (ec != JsNoError) return map_js(env, ec, "JsGetValueType");
  if (t != JsBoolean) {
    nodeapi::SetLastError(env, napi_boolean_expected,
                          "napi_get_value_bool: not a boolean");
    return napi_boolean_expected;
  }
  bool b = false;
  ec = JsBooleanToBool(jsv, &b);
  if (ec != JsNoError) return map_js(env, ec, "JsBooleanToBool");
  *result = b;
  nodeapi::ClearLastError(env);
  return napi_ok;
}

/* --- Create strings --- */

napi_status NAPI_CDECL napi_create_string_utf8(napi_env env, const char* str,
                                                 size_t length,
                                                 napi_value* result) {
  if (env == nullptr || result == nullptr) {
    if (env != nullptr) {
      nodeapi::SetLastError(env, napi_invalid_arg, "napi_create_string_utf8: null arg");
    }
    return napi_invalid_arg;
  }
  nodeapi::EntryGuard guard(env, false, true, true);
  if (!guard.ok()) return guard.status();
  size_t len = 0;
  napi_status vs = check_create_string(env, str, length, &len);
  if (vs != napi_ok) return vs;
  JsValueRef v = JS_INVALID_REFERENCE;
  JsErrorCode ec;
  if (len == 0) {
    ec = JsCreateString("", 0, &v);
  } else {
    ec = JsCreateString(str, len, &v);
  }
  if (ec != JsNoError) return map_js(env, ec, "JsCreateString");
  return root_publish(env, v, result, "napi_create_string_utf8: root failed");
}

napi_status NAPI_CDECL napi_create_string_utf16(napi_env env,
                                                  const char16_t* str,
                                                  size_t length,
                                                  napi_value* result) {
  if (env == nullptr || result == nullptr) {
    if (env != nullptr) {
      nodeapi::SetLastError(env, napi_invalid_arg, "napi_create_string_utf16: null arg");
    }
    return napi_invalid_arg;
  }
  nodeapi::EntryGuard guard(env, false, true, true);
  if (!guard.ok()) return guard.status();
  if (length != NAPI_AUTO_LENGTH && length > static_cast<size_t>(INT_MAX)) {
    nodeapi::SetLastError(env, napi_invalid_arg,
                          "napi_create_string_utf16: length exceeds INT_MAX");
    return napi_invalid_arg;
  }
  if (length > 0 && str == nullptr) {
    nodeapi::SetLastError(env, napi_invalid_arg,
                          "napi_create_string_utf16: str is NULL");
    return napi_invalid_arg;
  }
  size_t len = 0;
  if (length == NAPI_AUTO_LENGTH) {
    len = utf16_auto_len(str);
  } else {
    len = length;
  }
  JsValueRef v = JS_INVALID_REFERENCE;
  JsErrorCode ec;
  if (len == 0) {
    JsErrorCode e0 = JsCreateString("", 0, &v);
    ec = e0;
  } else {
    ec = JsCreateStringUtf16(reinterpret_cast<const uint16_t*>(str), len, &v);
  }
  if (ec != JsNoError) return map_js(env, ec, "JsCreateStringUtf16");
  return root_publish(env, v, result, "napi_create_string_utf16: root failed");
}

napi_status NAPI_CDECL napi_create_string_latin1(napi_env env, const char* str,
                                                   size_t length,
                                                   napi_value* result) {
  if (env == nullptr || result == nullptr) {
    if (env != nullptr) {
      nodeapi::SetLastError(env, napi_invalid_arg, "napi_create_string_latin1: null arg");
    }
    return napi_invalid_arg;
  }
  nodeapi::EntryGuard guard(env, false, true, true);
  if (!guard.ok()) return guard.status();
  size_t len = 0;
  napi_status vs = check_create_string(env, str, length, &len);
  if (vs != napi_ok) return vs;
  JsValueRef v = JS_INVALID_REFERENCE;
  JsErrorCode ec;
  if (len == 0) {
    ec = JsCreateString("", 0, &v);
  } else {
    std::vector<uint16_t> tmp;
    try {
      tmp.resize(len);
    } catch (...) {
      nodeapi::SetLastError(env, napi_generic_failure,
                            "napi_create_string_latin1: transcode OOM");
      return napi_generic_failure;
    }
    for (size_t i = 0; i < len; ++i) {
      tmp[i] = static_cast<uint16_t>(static_cast<unsigned char>(str[i]));
    }
    ec = JsCreateStringUtf16(tmp.data(), len, &v);
  }
  if (ec != JsNoError) return map_js(env, ec, "JsCreateStringUtf16 (latin1)");
  return root_publish(env, v, result, "napi_create_string_latin1: root failed");
}

/* --- Get strings --- */

napi_status NAPI_CDECL napi_get_value_string_utf8(napi_env env, napi_value value,
                                                    char* buf, size_t bufsize,
                                                    size_t* result) {
  if (env == nullptr || value == nullptr) {
    if (env != nullptr) {
      nodeapi::SetLastError(env, napi_invalid_arg, "napi_get_value_string_utf8: null arg");
    }
    return napi_invalid_arg;
  }
  nodeapi::EntryGuard guard(env, false, true, true);
  if (!guard.ok()) return guard.status();
  JsValueRef jsv = reinterpret_cast<JsValueRef>(value);
  JsValueType t = JsUndefined;
  JsErrorCode ec = JsGetValueType(jsv, &t);
  if (ec != JsNoError) return map_js(env, ec, "JsGetValueType");
  if (t != JsString) {
    nodeapi::SetLastError(env, napi_string_expected,
                          "napi_get_value_string_utf8: not a string");
    return napi_string_expected;
  }
  // Type check precedes buf/result validation (Node order): a wrong-type
  // value reports string_expected even when buf/result are NULL.
  if (buf == nullptr && result == nullptr) {
    nodeapi::SetLastError(env, napi_invalid_arg,
                          "napi_get_value_string_utf8: result NULL for size query");
    return napi_invalid_arg;
  }
  if (buf == nullptr) {
    size_t len = 0;
    ec = JsCopyString(jsv, nullptr, 0, &len);
    if (ec != JsNoError) return map_js(env, ec, "JsCopyString (size)");
    *result = len;
    nodeapi::ClearLastError(env);
    return napi_ok;
  }
  if (bufsize == 0) {
    if (result != nullptr) *result = 0;
    nodeapi::ClearLastError(env);
    return napi_ok;
  }
  /* JsCopyString -> EncodeInto asserts (CodexAssertOrFailFast) when the
   * destination is too small for the next sequence (it does NOT truncate),
   * so never call it with cap = bufsize-1 < full length. Two-step instead:
   * size query first, then exact copy via a full temporary when truncating. */
  size_t full_len = 0;
  ec = JsCopyString(jsv, nullptr, 0, &full_len);
  if (ec != JsNoError) return map_js(env, ec, "JsCopyString (size for copy)");
  if (full_len + 1 <= bufsize) {
    size_t copied = 0;
    ec = JsCopyString(jsv, buf, full_len, &copied);
    if (ec != JsNoError) return map_js(env, ec, "JsCopyString");
    if (copied > full_len) copied = full_len;
    buf[copied] = '\0';
    if (result != nullptr) *result = copied;
    nodeapi::ClearLastError(env);
    return napi_ok;
  }
  size_t max_bytes = bufsize - 1;
  std::vector<char> tmp;
  try {
    tmp.resize(full_len == 0 ? 1 : full_len);
  } catch (...) {
    nodeapi::SetLastError(env, napi_generic_failure,
                          "napi_get_value_string_utf8: transcode OOM");
    return napi_generic_failure;
  }
  size_t tmp_copied = 0;
  ec = JsCopyString(jsv, full_len == 0 ? nullptr : tmp.data(), full_len,
                    &tmp_copied);
  if (ec != JsNoError) return map_js(env, ec, "JsCopyString (full temp)");
  if (tmp_copied > full_len) tmp_copied = full_len;
  if (tmp_copied != full_len) return map_js(env, JsErrorInvalidArgument, "JsCopyString (size mismatch)");
  size_t trunc_len = (max_bytes < full_len) ? max_bytes : full_len;
  /* Never split a multi-byte sequence (V8 WriteUtf8 / vendored test_string
   * utf8InsufficientIdx: 2-byte '¡' with cap 3 copies only 1 char). */
  while (trunc_len > 0 && trunc_len < full_len) {
    unsigned char next = static_cast<unsigned char>(tmp[trunc_len]);
    if ((next & 0xC0) != 0x80) break;
    --trunc_len;
  }
  if (trunc_len > 0) std::memcpy(buf, tmp.data(), trunc_len);
  buf[trunc_len] = '\0';
  if (result != nullptr) *result = trunc_len;
  nodeapi::ClearLastError(env);
  return napi_ok;
}

napi_status NAPI_CDECL napi_get_value_string_utf16(napi_env env, napi_value value,
                                                     char16_t* buf, size_t bufsize,
                                                     size_t* result) {
  if (env == nullptr || value == nullptr) {
    if (env != nullptr) {
      nodeapi::SetLastError(env, napi_invalid_arg, "napi_get_value_string_utf16: null arg");
    }
    return napi_invalid_arg;
  }
  nodeapi::EntryGuard guard(env, false, true, true);
  if (!guard.ok()) return guard.status();
  JsValueRef jsv = reinterpret_cast<JsValueRef>(value);
  JsValueType t = JsUndefined;
  JsErrorCode ec = JsGetValueType(jsv, &t);
  if (ec != JsNoError) return map_js(env, ec, "JsGetValueType");
  if (t != JsString) {
    nodeapi::SetLastError(env, napi_string_expected,
                          "napi_get_value_string_utf16: not a string");
    return napi_string_expected;
  }
  if (buf == nullptr && result == nullptr) {
    nodeapi::SetLastError(env, napi_invalid_arg,
                          "napi_get_value_string_utf16: result NULL for size query");
    return napi_invalid_arg;
  }
  int str_len = 0;
  ec = JsGetStringLength(jsv, &str_len);
  if (ec != JsNoError) return map_js(env, ec, "JsGetStringLength");
  if (str_len < 0) str_len = 0;
  if (buf == nullptr) {
    *result = static_cast<size_t>(str_len);
    nodeapi::ClearLastError(env);
    return napi_ok;
  }
  if (bufsize == 0) {
    if (result != nullptr) *result = 0;
    nodeapi::ClearLastError(env);
    return napi_ok;
  }
  size_t cap = bufsize - 1;
  size_t to_copy = (cap < static_cast<size_t>(str_len))
                       ? cap
                       : static_cast<size_t>(str_len);
  int req = (to_copy > static_cast<size_t>(INT_MAX))
                ? INT_MAX
                : static_cast<int>(to_copy);
  size_t written = 0;
  ec = JsCopyStringUtf16(jsv, 0, req, reinterpret_cast<uint16_t*>(buf),
                         &written);
  if (ec != JsNoError) return map_js(env, ec, "JsCopyStringUtf16");
  buf[written] = u'\0';
  if (result != nullptr) *result = written;
  nodeapi::ClearLastError(env);
  return napi_ok;
}

napi_status NAPI_CDECL napi_get_value_string_latin1(napi_env env, napi_value value,
                                                      char* buf, size_t bufsize,
                                                      size_t* result) {
  if (env == nullptr || value == nullptr) {
    if (env != nullptr) {
      nodeapi::SetLastError(env, napi_invalid_arg, "napi_get_value_string_latin1: null arg");
    }
    return napi_invalid_arg;
  }
  nodeapi::EntryGuard guard(env, false, true, true);
  if (!guard.ok()) return guard.status();
  JsValueRef jsv = reinterpret_cast<JsValueRef>(value);
  JsValueType t = JsUndefined;
  JsErrorCode ec = JsGetValueType(jsv, &t);
  if (ec != JsNoError) return map_js(env, ec, "JsGetValueType");
  if (t != JsString) {
    nodeapi::SetLastError(env, napi_string_expected,
                          "napi_get_value_string_latin1: not a string");
    return napi_string_expected;
  }
  if (buf == nullptr && result == nullptr) {
    nodeapi::SetLastError(env, napi_invalid_arg,
                          "napi_get_value_string_latin1: result NULL for size query");
    return napi_invalid_arg;
  }
  int str_len = 0;
  ec = JsGetStringLength(jsv, &str_len);
  if (ec != JsNoError) return map_js(env, ec, "JsGetStringLength");
  if (str_len < 0) str_len = 0;
  if (buf == nullptr) {
    *result = static_cast<size_t>(str_len);
    nodeapi::ClearLastError(env);
    return napi_ok;
  }
  if (bufsize == 0) {
    if (result != nullptr) *result = 0;
    nodeapi::ClearLastError(env);
    return napi_ok;
  }
  size_t cap = bufsize - 1;
  size_t to_copy = (cap < static_cast<size_t>(str_len))
                       ? cap
                       : static_cast<size_t>(str_len);
  std::vector<uint16_t> tmp;
  try {
    tmp.resize(to_copy);
  } catch (...) {
    nodeapi::SetLastError(env, napi_generic_failure,
                          "napi_get_value_string_latin1: transcode OOM");
    return napi_generic_failure;
  }
  size_t written = 0;
  int req = (to_copy > static_cast<size_t>(INT_MAX))
                ? INT_MAX
                : static_cast<int>(to_copy);
  ec = JsCopyStringUtf16(jsv, 0, req, tmp.data(), &written);
  if (ec != JsNoError) return map_js(env, ec, "JsCopyStringUtf16 (latin1)");
  if (written > to_copy) written = to_copy;
  for (size_t i = 0; i < written; ++i) {
    buf[i] = static_cast<char>(tmp[i]);
  }
  buf[written] = '\0';
  if (result != nullptr) *result = written;
  nodeapi::ClearLastError(env);
  return napi_ok;
}

/* --- N2.6: coercion, type checks, symbols, Date, BigInt --- */

/* --- napi_typeof (A2, C Yes: execute with pending) --- */

napi_status NAPI_CDECL napi_typeof(napi_env env, napi_value value,
                                    napi_valuetype* result) {
  if (env == nullptr || value == nullptr || result == nullptr) {
    if (env != nullptr) {
      nodeapi::SetLastError(env, napi_invalid_arg, "napi_typeof: null arg");
    }
    return napi_invalid_arg;
  }
  nodeapi::EntryGuard guard(env, false, true, true);
  if (!guard.ok()) return guard.status();
  JsValueRef jsv = reinterpret_cast<JsValueRef>(value);
  JsValueType t = JsUndefined;
  JsErrorCode ec = JsGetValueType(jsv, &t);
  if (ec != JsNoError) return map_js(env, ec, "JsGetValueType (typeof)");
  napi_valuetype vt = napi_object;
  switch (t) {
    case JsUndefined: vt = napi_undefined; break;
    case JsNull: vt = napi_null; break;
    case JsNumber: vt = napi_number; break;
    case JsString: vt = napi_string; break;
    case JsBoolean: vt = napi_boolean; break;
    case JsSymbol: vt = napi_symbol; break;
    case JsFunction: vt = napi_function; break;
    default:
      if (t != JsObject) {
        vt = napi_object; /* Error/Array/ArrayBuffer/TypedArray/DataView */
        break;
      }
      if (IsExternalValue(jsv)) {
        vt = napi_external;
        break;
      }
      {
        bool big = false;
        napi_status s = TagIsBigInt(env, jsv, &big);
        if (s != napi_ok) return s;
        vt = big ? napi_bigint : napi_object;
      }
      break;
  }
  *result = vt;
  nodeapi::ClearLastError(env);
  return napi_ok;
}

/* --- napi_strict_equals (A2, C No: gated) --- */

napi_status NAPI_CDECL napi_strict_equals(napi_env env, napi_value lhs,
                                           napi_value rhs, bool* result) {
  if (env == nullptr || lhs == nullptr || rhs == nullptr || result == nullptr) {
    if (env != nullptr) {
      nodeapi::SetLastError(env, napi_invalid_arg, "napi_strict_equals: null arg");
    }
    return napi_invalid_arg;
  }
  nodeapi::EntryGuard guard(env, false, false, true);
  if (!guard.ok()) return guard.status();
  bool eq = false;
  JsErrorCode ec = JsStrictEquals(reinterpret_cast<JsValueRef>(lhs),
                                  reinterpret_cast<JsValueRef>(rhs), &eq);
  if (ec != JsNoError) return map_js(env, ec, "JsStrictEquals");
  *result = eq;
  nodeapi::ClearLastError(env);
  return napi_ok;
}

/* --- Coercion helpers --- */

/* Shared coerce body. `convert` is one of JsConvertValueTo*. The result
 * is published rooted (root_publish). */
napi_status CoerceCommon(napi_env env, napi_value value, napi_value* result,
                         JsErrorCode (*convert)(JsValueRef, JsValueRef*),
                         const char* what) {
  JsValueRef jsv = reinterpret_cast<JsValueRef>(value);
  JsValueRef v = JS_INVALID_REFERENCE;
  JsErrorCode ec = convert(jsv, &v);
  if (ec != JsNoError) return map_js(env, ec, what);
  return root_publish(env, v, result, what);
}

/* --- napi_coerce_to_bool (A2, C No: gated) --- */

napi_status NAPI_CDECL napi_coerce_to_bool(napi_env env, napi_value value,
                                             napi_value* result) {
  if (env == nullptr || value == nullptr || result == nullptr) {
    if (env != nullptr) {
      nodeapi::SetLastError(env, napi_invalid_arg, "napi_coerce_to_bool: null arg");
    }
    return napi_invalid_arg;
  }
  nodeapi::EntryGuard guard(env, false, false, true);
  if (!guard.ok()) return guard.status();
  return CoerceCommon(env, value, result, JsConvertValueToBoolean,
                      "napi_coerce_to_bool: JsConvertValueToBoolean");
}

/* --- napi_coerce_to_number (A2, C No: gated) --- */

napi_status NAPI_CDECL napi_coerce_to_number(napi_env env, napi_value value,
                                               napi_value* result) {
  if (env == nullptr || value == nullptr || result == nullptr) {
    if (env != nullptr) {
      nodeapi::SetLastError(env, napi_invalid_arg, "napi_coerce_to_number: null arg");
    }
    return napi_invalid_arg;
  }
  nodeapi::EntryGuard guard(env, false, false, true);
  if (!guard.ok()) return guard.status();
  return CoerceCommon(env, value, result, JsConvertValueToNumber,
                      "napi_coerce_to_number: JsConvertValueToNumber");
}

/* --- napi_coerce_to_string (A2, C No: gated) --- */

napi_status NAPI_CDECL napi_coerce_to_string(napi_env env, napi_value value,
                                               napi_value* result) {
  if (env == nullptr || value == nullptr || result == nullptr) {
    if (env != nullptr) {
      nodeapi::SetLastError(env, napi_invalid_arg, "napi_coerce_to_string: null arg");
    }
    return napi_invalid_arg;
  }
  nodeapi::EntryGuard guard(env, false, false, true);
  if (!guard.ok()) return guard.status();
  return CoerceCommon(env, value, result, JsConvertValueToString,
                      "napi_coerce_to_string: JsConvertValueToString");
}

/* --- napi_coerce_to_object (A2, C No: gated) ---
 * null/undefined => napi_object_expected WITH the engine TypeError left
 * pending (Node via ToObject; Node-differential verified). The convert
 * runs for its side effect; a synthesized TypeError covers engines that
 * fail to leave pending. */
napi_status NAPI_CDECL napi_coerce_to_object(napi_env env, napi_value value,
                                               napi_value* result) {
  if (env == nullptr || value == nullptr || result == nullptr) {
    if (env != nullptr) {
      nodeapi::SetLastError(env, napi_invalid_arg, "napi_coerce_to_object: null arg");
    }
    return napi_invalid_arg;
  }
  nodeapi::EntryGuard guard(env, false, false, true);
  if (!guard.ok()) return guard.status();
  JsValueRef jsv = reinterpret_cast<JsValueRef>(value);
  JsValueType t = JsUndefined;
  JsErrorCode ec = JsGetValueType(jsv, &t);
  if (ec != JsNoError) return map_js(env, ec, "JsGetValueType (coerce_to_object)");
  if (t == JsNull || t == JsUndefined) {
    JsValueRef tmp = JS_INVALID_REFERENCE;
    (void)JsConvertValueToObject(jsv, &tmp); /* produce the pending TypeError */
    bool pending = false;
    (void)JsHasException(&pending);
    if (!pending) {
      const char* text = "Cannot convert undefined or null to object";
      JsValueRef msg = JS_INVALID_REFERENCE;
      ec = JsCreateString(text, std::strlen(text), &msg);
      TmpRoot msgRoot(msg);
      JsValueRef err = JS_INVALID_REFERENCE;
      ec = JsCreateTypeError(msg, &err);
      TmpRoot errRoot(err);
      (void)JsSetException(err);
    }
    nodeapi::SetLastError(env, napi_object_expected,
                          "napi_coerce_to_object: null/undefined");
    return napi_object_expected;
  }
  return CoerceCommon(env, value, result, JsConvertValueToObject,
                      "napi_coerce_to_object: JsConvertValueToObject");
}

/* --- napi_is_array / is_typedarray / is_dataview / is_arraybuffer
 * (A2/A5, C Yes: execute with pending, ok+false on wrong type) --- */

napi_status NAPI_CDECL napi_is_array(napi_env env, napi_value value,
                                      bool* result) {
  if (env == nullptr || value == nullptr || result == nullptr) {
    if (env != nullptr) {
      nodeapi::SetLastError(env, napi_invalid_arg, "napi_is_array: null arg");
    }
    return napi_invalid_arg;
  }
  nodeapi::EntryGuard guard(env, false, true, true);
  if (!guard.ok()) return guard.status();
  JsValueType t = JsUndefined;
  JsErrorCode ec = JsGetValueType(reinterpret_cast<JsValueRef>(value), &t);
  if (ec != JsNoError) return map_js(env, ec, "JsGetValueType (is_array)");
  *result = (t == JsArray);
  nodeapi::ClearLastError(env);
  return napi_ok;
}

napi_status NAPI_CDECL napi_is_typedarray(napi_env env, napi_value value,
                                           bool* result) {
  if (env == nullptr || value == nullptr || result == nullptr) {
    if (env != nullptr) {
      nodeapi::SetLastError(env, napi_invalid_arg, "napi_is_typedarray: null arg");
    }
    return napi_invalid_arg;
  }
  nodeapi::EntryGuard guard(env, false, true, true);
  if (!guard.ok()) return guard.status();
  JsValueType t = JsUndefined;
  JsErrorCode ec = JsGetValueType(reinterpret_cast<JsValueRef>(value), &t);
  if (ec != JsNoError) return map_js(env, ec, "JsGetValueType (is_typedarray)");
  *result = (t == JsTypedArray);
  nodeapi::ClearLastError(env);
  return napi_ok;
}

napi_status NAPI_CDECL napi_is_dataview(napi_env env, napi_value value,
                                         bool* result) {
  if (env == nullptr || value == nullptr || result == nullptr) {
    if (env != nullptr) {
      nodeapi::SetLastError(env, napi_invalid_arg, "napi_is_dataview: null arg");
    }
    return napi_invalid_arg;
  }
  nodeapi::EntryGuard guard(env, false, true, true);
  if (!guard.ok()) return guard.status();
  JsValueType t = JsUndefined;
  JsErrorCode ec = JsGetValueType(reinterpret_cast<JsValueRef>(value), &t);
  if (ec != JsNoError) return map_js(env, ec, "JsGetValueType (is_dataview)");
  *result = (t == JsDataView);
  nodeapi::ClearLastError(env);
  return napi_ok;
}

napi_status NAPI_CDECL napi_is_arraybuffer(napi_env env, napi_value value,
                                            bool* result) {
  if (env == nullptr || value == nullptr || result == nullptr) {
    if (env != nullptr) {
      nodeapi::SetLastError(env, napi_invalid_arg, "napi_is_arraybuffer: null arg");
    }
    return napi_invalid_arg;
  }
  nodeapi::EntryGuard guard(env, false, true, true);
  if (!guard.ok()) return guard.status();
  JsValueType t = JsUndefined;
  JsErrorCode ec = JsGetValueType(reinterpret_cast<JsValueRef>(value), &t);
  if (ec != JsNoError) return map_js(env, ec, "JsGetValueType (is_arraybuffer)");
  *result = (t == JsArrayBuffer);
  nodeapi::ClearLastError(env);
  return napi_ok;
}

/* --- napi_is_date (A2, C Yes) --- */

napi_status NAPI_CDECL napi_is_date(napi_env env, napi_value value,
                                     bool* result) {
  if (env == nullptr || value == nullptr || result == nullptr) {
    if (env != nullptr) {
      nodeapi::SetLastError(env, napi_invalid_arg, "napi_is_date: null arg");
    }
    return napi_invalid_arg;
  }
  nodeapi::EntryGuard guard(env, false, true, true);
  if (!guard.ok()) return guard.status();
  bool is_date = false;
  napi_status s = IsDateImpl(env, reinterpret_cast<JsValueRef>(value),
                             &is_date);
  if (s != napi_ok) return s;
  *result = is_date;
  return napi_ok;
}

/* --- napi_create_symbol (A1, C Yes) --- */

napi_status NAPI_CDECL napi_create_symbol(napi_env env, napi_value description,
                                           napi_value* result) {
  if (env == nullptr || result == nullptr) {
    if (env != nullptr) {
      nodeapi::SetLastError(env, napi_invalid_arg, "napi_create_symbol: null arg");
    }
    return napi_invalid_arg;
  }
  nodeapi::EntryGuard guard(env, false, true, true);
  if (!guard.ok()) return guard.status();
  JsValueRef v = JS_INVALID_REFERENCE;
  if (description == nullptr) {
    /* Symbol() via the Symbol intrinsic (NOT JsCreateSymbol(NULL), whose
     * NULL path yields an empty-string description). Note: this build's
     * Symbol() also stores "" (Chakra engine deviation — no symbol has
     * a JS-visible undefined description here), but delegating to
     * Symbol() is still the faithful route per plan/N2.6. */
    JsValueRef symFn = JS_INVALID_REFERENCE;
    napi_status s = GetGlobalProp(env, "Symbol", &symFn, "create_symbol: Symbol");
    if (s != napi_ok) return s;
    TmpRoot fnRoot(symFn);
    JsValueType ft = JsUndefined;
    JsErrorCode ec = JsGetValueType(symFn, &ft);
    if (ec != JsNoError) return map_js(env, ec, "create_symbol: Symbol type");
    if (ft != JsFunction) {
      nodeapi::SetLastError(env, napi_generic_failure,
                            "create_symbol: global Symbol not a function");
      return napi_generic_failure;
    }
    JsValueRef args[1] = {symFn}; /* this = fn (ignored by Symbol) */
    ec = JsCallFunction(symFn, args, 1, &v);
    if (ec != JsNoError) return map_js(env, ec, "create_symbol: Symbol()");
  } else {
    JsValueRef descJs = reinterpret_cast<JsValueRef>(description);
    JsValueType t = JsUndefined;
    JsErrorCode ec = JsGetValueType(descJs, &t);
    if (ec != JsNoError) return map_js(env, ec, "create_symbol: description type");
    if (t != JsString) {
      nodeapi::SetLastError(env, napi_string_expected,
                            "napi_create_symbol: description not a string");
      return napi_string_expected;
    }
    ec = JsCreateSymbol(descJs, &v);
    if (ec != JsNoError) return map_js(env, ec, "JsCreateSymbol");
  }
  return root_publish(env, v, result, "napi_create_symbol: root failed");
}

/* --- napi_create_date (A1, C No: gated) --- */

napi_status NAPI_CDECL napi_create_date(napi_env env, double time,
                                         napi_value* result) {
  if (env == nullptr || result == nullptr) {
    if (env != nullptr) {
      nodeapi::SetLastError(env, napi_invalid_arg, "napi_create_date: null arg");
    }
    return napi_invalid_arg;
  }
  nodeapi::EntryGuard guard(env, false, false, true);
  if (!guard.ok()) return guard.status();
  JsValueRef ctor = JS_INVALID_REFERENCE;
  napi_status s = GetGlobalProp(env, "Date", &ctor, "create_date: Date");
  if (s != napi_ok) return s;
  TmpRoot ctorRoot(ctor);
  JsValueType ct = JsUndefined;
  JsErrorCode ec = JsGetValueType(ctor, &ct);
  if (ec != JsNoError) return map_js(env, ec, "create_date: Date type");
  if (ct != JsFunction) {
    nodeapi::SetLastError(env, napi_generic_failure,
                          "create_date: global Date not a function");
    return napi_generic_failure;
  }
  JsValueRef num = JS_INVALID_REFERENCE;
  ec = JsDoubleToNumber(time, &num);
  if (ec != JsNoError) return map_js(env, ec, "create_date: JsDoubleToNumber");
  TmpRoot numRoot(num);
  JsValueRef args[2] = {ctor, num};
  JsValueRef v = JS_INVALID_REFERENCE;
  ec = JsConstructObject(ctor, args, 2, &v);
  if (ec != JsNoError) return map_js(env, ec, "create_date: JsConstructObject");
  return root_publish(env, v, result, "napi_create_date: root failed");
}

/* --- napi_get_date_value (A1, C No: gated) --- */

napi_status NAPI_CDECL napi_get_date_value(napi_env env, napi_value value,
                                            double* result) {
  if (env == nullptr || value == nullptr || result == nullptr) {
    if (env != nullptr) {
      nodeapi::SetLastError(env, napi_invalid_arg, "napi_get_date_value: null arg");
    }
    return napi_invalid_arg;
  }
  nodeapi::EntryGuard guard(env, false, false, true);
  if (!guard.ok()) return guard.status();
  JsValueRef jsv = reinterpret_cast<JsValueRef>(value);
  bool is_date = false;
  napi_status s = IsDateImpl(env, jsv, &is_date);
  if (s != napi_ok) return s;
  if (!is_date) {
    nodeapi::SetLastError(env, napi_date_expected,
                          "napi_get_date_value: not a date");
    return napi_date_expected;
  }
  JsPropertyIdRef gtPid = JS_INVALID_REFERENCE;
  s = MakePid(env, "getTime", &gtPid, "get_date_value: getTime id");
  if (s != napi_ok) return s;
  JsValueRef gt = JS_INVALID_REFERENCE;
  JsErrorCode ec = JsGetProperty(jsv, gtPid, &gt);
  if (ec != JsNoError) return map_js(env, ec, "get_date_value: getTime");
  TmpRoot gtRoot(gt);
  JsValueType gtt = JsUndefined;
  ec = JsGetValueType(gt, &gtt);
  if (ec != JsNoError) return map_js(env, ec, "get_date_value: getTime type");
  if (gtt != JsFunction) {
    nodeapi::SetLastError(env, napi_generic_failure,
                          "get_date_value: getTime not a function");
    return napi_generic_failure;
  }
  JsValueRef args[1] = {jsv};
  JsValueRef ret = JS_INVALID_REFERENCE;
  ec = JsCallFunction(gt, args, 1, &ret);
  if (ec != JsNoError) return map_js(env, ec, "get_date_value: getTime call");
  TmpRoot retRoot(ret);
  double d = 0.0;
  ec = JsNumberToDouble(ret, &d);
  if (ec != JsNoError) return map_js(env, ec, "get_date_value: JsNumberToDouble");
  *result = d;
  nodeapi::ClearLastError(env);
  return napi_ok;
}

/* --- BigInt create (A1: int64/uint64 C Yes, words C No) --- */

napi_status NAPI_CDECL napi_create_bigint_int64(napi_env env, int64_t value,
                                                 napi_value* result) {
  if (env == nullptr || result == nullptr) {
    if (env != nullptr) {
      nodeapi::SetLastError(env, napi_invalid_arg, "napi_create_bigint_int64: null arg");
    }
    return napi_invalid_arg;
  }
  nodeapi::EntryGuard guard(env, false, true, true);
  if (!guard.ok()) return guard.status();
  std::string decimal;
  try {
    decimal = std::to_string(value);
  } catch (...) {
    nodeapi::SetLastError(env, napi_generic_failure,
                          "napi_create_bigint_int64: decimal OOM");
    return napi_generic_failure;
  }
  return CreateBigIntFromDecimal(env, decimal, result,
                                 "napi_create_bigint_int64");
}

napi_status NAPI_CDECL napi_create_bigint_uint64(napi_env env, uint64_t value,
                                                  napi_value* result) {
  if (env == nullptr || result == nullptr) {
    if (env != nullptr) {
      nodeapi::SetLastError(env, napi_invalid_arg, "napi_create_bigint_uint64: null arg");
    }
    return napi_invalid_arg;
  }
  nodeapi::EntryGuard guard(env, false, true, true);
  if (!guard.ok()) return guard.status();
  std::string decimal;
  try {
    decimal = std::to_string(value);
  } catch (...) {
    nodeapi::SetLastError(env, napi_generic_failure,
                          "napi_create_bigint_uint64: decimal OOM");
    return napi_generic_failure;
  }
  return CreateBigIntFromDecimal(env, decimal, result,
                                 "napi_create_bigint_uint64");
}

napi_status NAPI_CDECL napi_create_bigint_words(napi_env env, int sign_bit,
                                                 size_t word_count,
                                                 const uint64_t* words,
                                                 napi_value* result) {
  if (env == nullptr || result == nullptr) {
    if (env != nullptr) {
      nodeapi::SetLastError(env, napi_invalid_arg, "napi_create_bigint_words: null arg");
    }
    return napi_invalid_arg;
  }
  if (word_count > static_cast<size_t>(INT_MAX)) {
    nodeapi::SetLastError(env, napi_invalid_arg,
                          "napi_create_bigint_words: word_count exceeds INT_MAX");
    return napi_invalid_arg;
  }
  if (words == nullptr) {
    nodeapi::SetLastError(env, napi_invalid_arg,
                          "napi_create_bigint_words: words is NULL");
    return napi_invalid_arg;
  }
  nodeapi::EntryGuard guard(env, false, false, true);
  if (!guard.ok()) return guard.status();
  if (word_count > kMaxBigIntWords) {
    const char* text = "Maximum BigInt size exceeded";
    JsValueRef msg = JS_INVALID_REFERENCE;
    JsErrorCode ec = JsCreateString(text, std::strlen(text), &msg);
    TmpRoot msgRoot(msg);
    JsValueRef err = JS_INVALID_REFERENCE;
    ec = JsCreateRangeError(msg, &err);
    TmpRoot errRoot(err);
    if (ec != JsNoError) {
      nodeapi::SetLastError(env, napi_generic_failure,
                            "create_bigint_words: RangeError alloc");
      return napi_generic_failure;
    }
    ec = JsSetException(err);
    if (ec != JsNoError) return map_js(env, ec, "create_bigint_words: RangeError set");
    nodeapi::SetLastError(env, napi_pending_exception,
                          "create_bigint_words: Maximum BigInt size exceeded");
    return napi_pending_exception;
  }
  std::string decimal;
  if (!WordsToDecimal(sign_bit != 0, words, word_count, &decimal)) {
    nodeapi::SetLastError(env, napi_generic_failure,
                          "napi_create_bigint_words: decimal OOM");
    return napi_generic_failure;
  }
  return CreateBigIntFromDecimal(env, decimal, result,
                                 "napi_create_bigint_words");
}

/* --- BigInt get (A1, C Yes) --- */

napi_status NAPI_CDECL napi_get_value_bigint_int64(napi_env env, napi_value value,
                                                    int64_t* result,
                                                    bool* lossless) {
  if (env == nullptr || value == nullptr || result == nullptr || lossless == nullptr) {
    if (env != nullptr) {
      nodeapi::SetLastError(env, napi_invalid_arg,
                            "napi_get_value_bigint_int64: null arg");
    }
    return napi_invalid_arg;
  }
  nodeapi::EntryGuard guard(env, false, true, true);
  if (!guard.ok()) return guard.status();
  uint64_t bits = 0;
  bool loss = false;
  napi_status s = GetBigInt64Common(env, value, /*signed_goal=*/true, &bits, &loss,
                                    "napi_get_value_bigint_int64: not a bigint");
  if (s != napi_ok) return s;
  *result = static_cast<int64_t>(bits);
  *lossless = loss;
  nodeapi::ClearLastError(env);
  return napi_ok;
}

napi_status NAPI_CDECL napi_get_value_bigint_uint64(napi_env env, napi_value value,
                                                     uint64_t* result,
                                                     bool* lossless) {
  if (env == nullptr || value == nullptr || result == nullptr || lossless == nullptr) {
    if (env != nullptr) {
      nodeapi::SetLastError(env, napi_invalid_arg,
                            "napi_get_value_bigint_uint64: null arg");
    }
    return napi_invalid_arg;
  }
  nodeapi::EntryGuard guard(env, false, true, true);
  if (!guard.ok()) return guard.status();
  uint64_t bits = 0;
  bool loss = false;
  napi_status s = GetBigInt64Common(env, value, /*signed_goal=*/false, &bits, &loss,
                                    "napi_get_value_bigint_uint64: not a bigint");
  if (s != napi_ok) return s;
  *result = bits;
  *lossless = loss;
  nodeapi::ClearLastError(env);
  return napi_ok;
}

napi_status NAPI_CDECL napi_get_value_bigint_words(napi_env env, napi_value value,
                                                    int* sign_bit,
                                                    size_t* word_count,
                                                    uint64_t* words) {
  if (env == nullptr || value == nullptr || word_count == nullptr) {
    if (env != nullptr) {
      nodeapi::SetLastError(env, napi_invalid_arg,
                            "napi_get_value_bigint_words: null arg");
    }
    return napi_invalid_arg;
  }
  nodeapi::EntryGuard guard(env, false, true, true);
  if (!guard.ok()) return guard.status();
  JsValueRef jsv = reinterpret_cast<JsValueRef>(value);
  napi_status s = RequireBigInt(env, jsv, "napi_get_value_bigint_words: not a bigint");
  if (s != napi_ok) return s;
  std::string decimal;
  s = BigIntDecimal(env, jsv, &decimal, "napi_get_value_bigint_words: ToString");
  if (s != napi_ok) return s;
  bool neg = false;
  std::vector<uint64_t> limbs;
  bool parse_oom = false;
  if (!ParseDecimal(decimal, &neg, &limbs, &parse_oom)) {
    if (parse_oom) {
      nodeapi::SetLastError(env, napi_generic_failure,
                            "napi_get_value_bigint_words: decimal-parse OOM");
      return napi_generic_failure;
    }
    nodeapi::SetLastError(env, napi_bigint_expected,
                          "napi_get_value_bigint_words: ToString is not a decimal integer");
    return napi_bigint_expected;
  }
  size_t provided = *word_count;
  size_t required = limbs.size(); /* 0 for 0n */
  if (sign_bit != nullptr) *sign_bit = neg ? 1 : 0;
  *word_count = required;
  if (words != nullptr) {
    size_t copy = (required < provided) ? required : provided;
    for (size_t i = 0; i < copy; ++i) words[i] = limbs[i];
  }
  nodeapi::ClearLastError(env);
  return napi_ok;
}

} /* extern "C" */
