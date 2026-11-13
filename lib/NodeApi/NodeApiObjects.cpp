//-------------------------------------------------------------------------------------------------------
// Copyright (c) ChakraCore Project Contributors. All rights reserved.
// Licensed under the MIT license. See LICENSE.txt file in the project root for full license information.
//-------------------------------------------------------------------------------------------------------
//
// Node-API objects/arrays/properties (N2.3). See NodeApiObjects.h for the
// surface list, N2.4/N2.1 coordination seams, and the ownership record.
//
// Status discipline (§3.10 / doc/node-api-status-mapping.md C column):
// validated-first (NULL binding args -> napi_invalid_arg before anything
// else), then the shared EntryGuard (Closing -> generic_failure, gated
// functions -> napi_pending_exception without executing, context
// entered via nodeapi::ContextGuard). Creates are C-column "Yes"
// (execute with a pending exception; JsRT creation calls tolerate a
// pending exception, verified empirically). Every other entry here is
// C-column "No" (gated). Type pre-checks produce the specific
// napi_*_expected codes (object_expected via ToObject coercion,
// name_expected, array_expected, function_expected); JsRT errors map
// per the §3.10 table. Success clears last_error (ClearLastError);
// failures SetLastError. Out-params are written only on success.

#include "NodeApiObjects.h"

#include "NodeApiEnv.h"
#include "NodeApiScopes.h"
#include "ChakraCommon.h"
#include "ChakraCore.h"

#include <cstdint>
#include <new>
#include <cstdio>
#include <cstring>
#include <limits>
#include <vector>

namespace {

// ---------------------------------------------------------------------
// Small casts
// ---------------------------------------------------------------------

JsValueRef ToJs(napi_value v) { return reinterpret_cast<JsValueRef>(v); }
napi_value FromJs(JsValueRef v) { return reinterpret_cast<napi_value>(v); }

// ---------------------------------------------------------------------
// JsErrorCode -> napi_status (§3.10 table). Sets last_error on failure.
// NoError maps to napi_ok without touching last_error (the caller
// clears it on its success path); exceptions map to
// napi_pending_exception without clearing (exception stays pending).
// ---------------------------------------------------------------------
napi_status MapJsError(napi_env env, JsErrorCode err, const char* what) {
  switch (err) {
    case JsNoError:
      return napi_ok;
    case JsErrorInExceptionState:
    case JsErrorScriptException:
      nodeapi::SetLastError(env, napi_pending_exception, what);
      return napi_pending_exception;
    case JsErrorInvalidArgument:
    case JsErrorNullArgument:
      nodeapi::SetLastError(env, napi_invalid_arg, what);
      return napi_invalid_arg;
    case JsErrorOutOfMemory:
      nodeapi::SetLastError(env, napi_generic_failure, what);
      return napi_generic_failure;
    default:
      nodeapi::SetLastError(env, napi_generic_failure, what);
      return napi_generic_failure;
  }
}

// ---------------------------------------------------------------------
// ScratchRoot: transient JsAddRef/JsRelease pair for values that live
// exactly one napi_* call (descriptor objects, names arrays,
// intrinsic lookups). Owns its own root (R1: JsAddRef at store,
// JsRelease at release) and never touches the scope stack, so the
// R-REF-14 counter is unaffected. Release on destruction, i.e. every
// return path.
// ---------------------------------------------------------------------
class ScratchRoot {
 public:
  explicit ScratchRoot(JsValueRef v) : v_(v), owned_(false) {
    if (v_ != JS_INVALID_REFERENCE) {
      if (JsAddRef(v_, nullptr) == JsNoError) owned_ = true;
    }
  }
  ~ScratchRoot() { reset(); }
  ScratchRoot(const ScratchRoot&) = delete;
  ScratchRoot& operator=(const ScratchRoot&) = delete;
  ScratchRoot(ScratchRoot&& other) noexcept : v_(other.v_), owned_(other.owned_) {
    other.v_ = JS_INVALID_REFERENCE;
    other.owned_ = false;
  }
  ScratchRoot& operator=(ScratchRoot&& other) noexcept {
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
    if (owned_) {
      (void)JsRelease(v_, nullptr);
      owned_ = false;
    }
    v_ = JS_INVALID_REFERENCE;
  }

 private:
  JsValueRef v_;
  bool owned_;
};

// ScratchVector: native vector whose elements are EACH JsAddRef'd at
// push and JsRelease'd at destruction (R1 for provider memory
// storage). Used for collected property keys.
class ScratchVector {
 public:
  ScratchVector() = default;
  ~ScratchVector() {
    for (JsValueRef v : items_) {
      if (v != JS_INVALID_REFERENCE) (void)JsRelease(v, nullptr);
    }
  }
  ScratchVector(const ScratchVector&) = delete;
  ScratchVector& operator=(const ScratchVector&) = delete;
  // Returns false on OOM (caller maps to generic_failure).
  bool push(JsValueRef v) {
    if (v == JS_INVALID_REFERENCE) return false;
    if (JsAddRef(v, nullptr) != JsNoError) return false;
    try {
      items_.push_back(v);
    } catch (...) {
      (void)JsRelease(v, nullptr);
      return false;
    }
    return true;
  }
  size_t size() const { return items_.size(); }
  JsValueRef operator[](size_t i) const { return items_[i]; }

 private:
  std::vector<JsValueRef> items_;
};

// JS primitives needed as scratch (numbers, booleans, undefined).
JsValueRef UndefinedScratch() {
  JsValueRef v = JS_INVALID_REFERENCE;
  JsGetUndefinedValue(&v);
  return v;
}
JsValueRef BoolScratch(bool b) {
  JsValueRef v = JS_INVALID_REFERENCE;
  JsBoolToBoolean(b, &v);
  return v;
}
// Element indices: uint32_t -> JS number value (brief requirement).
// Doubles represent uint32 exactly, including 2^32-1.
JsValueRef IndexNumber(uint32_t index) {
  JsValueRef v = JS_INVALID_REFERENCE;
  JsDoubleToNumber(static_cast<double>(index), &v);
  return v;
}

// ToObject coercion for object parameters: null/undefined produce
// napi_object_expected WITHOUT calling JsConvertValueToObject (which
// would throw a TypeError and surface as pending_exception -- the
// wrong status). Every other type boxes (Node's CHECK_TO_OBJECT).
napi_status ToObject(napi_env env, napi_value in, napi_value* out) {
  JsValueType t = JsUndefined;
  JsErrorCode te = JsGetValueType(ToJs(in), &t);
  if (te != JsNoError) return MapJsError(env, te, "ToObject: type query");
  if (t == JsUndefined || t == JsNull) {
    nodeapi::SetLastError(env, napi_object_expected,
                          "ToObject: null/undefined is not an object");
    return napi_object_expected;
  }
  JsValueRef obj = JS_INVALID_REFERENCE;
  JsErrorCode e = JsConvertValueToObject(ToJs(in), &obj);
  if (e != JsNoError) return MapJsError(env, e, "ToObject");
  // Borrow: the converted object is reachable from `in` (identity)
  // or freshly boxed. Callers that run user code before use (the
  // value-key APIs, via CoercePropertyKey's ToString) hold a
  // ScratchRoot over *out; other callers use out immediately.
  *out = FromJs(obj);
  return napi_ok;
}

// Name pre-check for value-key APIs (string or symbol). The message
// text matches the vendored test expectation ("A string or symbol was
// expected", test_object/test.js HasOwn assertion via last_error).
// Only napi_has_own_property and descriptor names use this strict
// check in Node (k->IsName() in js_native_api_v8.cc); the other
// value-key APIs (get/set/has/delete) coerce via ToPropertyKey.
napi_status CheckName(napi_env env, napi_value key) {
  JsValueType t = JsUndefined;
  JsErrorCode te = JsGetValueType(ToJs(key), &t);
  if (te != JsNoError) return MapJsError(env, te, "key type query");
  if (t != JsString && t != JsSymbol) {
    nodeapi::SetLastError(env, napi_name_expected,
                          "A string or symbol was expected");
    return napi_name_expected;
  }
  return napi_ok;
}

bool IsNameValue(napi_value key) {
  JsValueType t = JsUndefined;
  if (JsGetValueType(ToJs(key), &t) != JsNoError) return false;
  return t == JsString || t == JsSymbol;
}

// ToPropertyKey coercion for the permissive value-key APIs
// (napi_get_property / set / has / delete). Mirrors Node's
// V8 Object::Get/Set/Has/Delete with a Value key, which runs
// ToPropertyKey (numbers/objects/booleans/null/undefined coerce
// to strings; symbols pass through). JsObject* (Jsrt.cpp
// InternalGetPropertyRecord) only accepts String/Symbol, so the
// provider must coerce non-Names via JsConvertValueToString.
// Known limitation: an object key whose Symbol.toPrimitive
// returns a symbol would be coerced to a string here (Node
// would return the symbol); ToString throws for Symbol
// results. This edge is not exercised by the vendored suites.
// The caller must hold *key_root alive for the JsObject* call;
// on direct Names *key_root stays empty.
napi_status CoercePropertyKey(napi_env env, napi_value key,
                              ScratchRoot* key_root, JsValueRef* out_key) {
  JsValueType t = JsUndefined;
  JsErrorCode te = JsGetValueType(ToJs(key), &t);
  if (te != JsNoError) return MapJsError(env, te, "key type query");
  if (t == JsString || t == JsSymbol) {
    *out_key = ToJs(key);
    return napi_ok;
  }
  JsValueRef converted = JS_INVALID_REFERENCE;
  JsErrorCode ce = JsConvertValueToString(ToJs(key), &converted);
  if (ce != JsNoError) return MapJsError(env, ce, "ToPropertyKey");
  *key_root = ScratchRoot(converted);
  *out_key = converted;
  return napi_ok;
}

// Root helpers: every returned napi_value goes through
// NodeApiScopes_RootIntoInnermost (R-SC-1). On rooting failure the
// out-param is NOT written (N1.3 convention) and the failure is
// recorded.
napi_status RootAndPublish(napi_env env, JsValueRef value,
                          napi_value* result) {
  if (value == JS_INVALID_REFERENCE) {
    nodeapi::SetLastError(env, napi_generic_failure, "null value to publish");
    return napi_generic_failure;
  }
  napi_status rs = NodeApiScopes_RootIntoInnermost(env, FromJs(value));
  if (rs != napi_ok) {
    nodeapi::SetLastError(env, rs, "rooting returned value failed");
    return rs;
  }
  *result = FromJs(value);
  return napi_ok;
}

// PropertyId helpers.
JsErrorCode MakeId(const char* name, JsPropertyIdRef* out) {
  return JsCreatePropertyId(name, std::strlen(name), out);
}

JsErrorCode IdFromValue(napi_env /*env*/, napi_value key, JsPropertyIdRef* out) {
  // Caller has already run CheckName.
  JsValueType t = JsUndefined;
  JsErrorCode te = JsGetValueType(ToJs(key), &t);
  if (te != JsNoError) return te;
  if (t == JsSymbol) {
    return JsGetPropertyIdFromSymbol(ToJs(key), out);
  }
  // String key: copy UTF-8 bytes, then JsCreatePropertyId (the
  // JsGetPropertyIdFromName spelling exists only in
  // ChakraCommonWindows.h on this platform; JsCreatePropertyId is
  // the portable equivalent for arbitrary string property names,
  // including all-digit names per its contract).
  size_t needed = 0;
  JsErrorCode ce = JsCopyString(ToJs(key), nullptr, 0, &needed);
  if (ce != JsNoError) return ce;
  if (needed == SIZE_MAX) return JsErrorOutOfMemory;
  std::vector<char> buf;
  try {
    buf.assign(needed + 1, '\0');
  } catch (...) {
    /* H1: bad_alloc/length_error must not escape extern "C";
     * MapJsError maps JsErrorOutOfMemory to napi_generic_failure. */
    return JsErrorOutOfMemory;
  }
  size_t written = 0;
  ce = JsCopyString(ToJs(key), buf.data(), buf.size(), &written);
  if (ce != JsNoError) return ce;
  buf[written] = '\0';
  return JsCreatePropertyId(buf.data(), written, out);
}

// Descriptor field read for filtering (data: writable present;
// accessor: writable is undefined per engine behavior -- verified by
// scratch probe: accessor descriptors carry get/set but no writable
// boolean; IsWritable for accessors is therefore false, matching
// Node's V8-attribute check `attributes & WRITABLE` which is unset
// for accessors).
struct FieldFlags {
  bool enumerable = false;
  bool configurable = false;
  bool writable = false;
  bool accessor = false;
};

bool ReadDescBool(JsValueRef desc, JsPropertyIdRef id, bool* out) {
  JsValueRef v = JS_INVALID_REFERENCE;
  if (JsGetProperty(desc, id, &v) != JsNoError) return false;
  JsValueType t = JsUndefined;
  if (JsGetValueType(v, &t) != JsNoError) return false;
  if (t != JsBoolean) return false;
  bool b = false;
  if (JsBooleanToBool(v, &b) != JsNoError) return false;
  *out = b;
  return true;
}

bool DescIsUndefined(JsValueRef desc, JsPropertyIdRef id) {
  JsValueRef v = JS_INVALID_REFERENCE;
  if (JsGetProperty(desc, id, &v) != JsNoError) return false;
  JsValueType t = JsUndefined;
  if (JsGetValueType(v, &t) != JsNoError) return false;
  return t == JsUndefined;
}

} /* namespace */
namespace {



napi_status NamedId(napi_env env, const char* utf8name, JsPropertyIdRef* out,
                    const char* what) {
  if (utf8name == nullptr) {
    nodeapi::SetLastError(env, napi_invalid_arg, what);
    return napi_invalid_arg;
  }
  JsErrorCode e = MakeId(utf8name, out);
  if (e != JsNoError) return MapJsError(env, e, what);
  return napi_ok;
}




// Descriptor-object field setter (JsSetProperty with a pre-made id).
// Returns napi_ok or the mapped failure.
napi_status SetField(napi_env env, JsValueRef desc, JsPropertyIdRef id,
                     JsValueRef value, const char* what) {
  JsErrorCode e = JsSetProperty(desc, id, value, /*useStrictRules=*/false);
  return MapJsError(env, e, what);
}




// True iff `s` (NUL-terminated UTF-8) is a canonical array index:
// all digits, no leading zero unless exactly "0", value < 2^32-1
// (spec Array Index bound; 2^32-1 itself is not an array index).
bool ParseArrayIndexString(const char* s, uint32_t* out) {
  if (s == nullptr || s[0] == '\0') return false;
  uint64_t n = 0;
  bool leading_zero_restricted = false;
  (void)leading_zero_restricted;
  size_t len = std::strlen(s);
  for (size_t i = 0; i < len; ++i) {
    if (s[i] < '0' || s[i] > '9') return false;
    n = n * 10 + static_cast<uint64_t>(s[i] - '0');
    if (n > 4294967295ull) return false;
  }
  // Canonical: no leading zeros (except "0" itself).
  if (len > 1 && s[0] == '0') return false;
  if (n >= 4294967295ull) return false;  // 2^32-1 is not an array index
  *out = static_cast<uint32_t>(n);
  return true;
}

// Copy a JS string to a NUL-terminated buffer. Returns false on JsRT
// failure (caller maps to generic_failure). On allocation failure
// (bad_alloc/length_error) returns false with *oom_out=true (H1);
// callers must map oom_out to napi_generic_failure, not silent skip.
bool CopyKeyString(JsValueRef key, std::vector<char>* out,
                   bool* oom_out = nullptr) {
  if (oom_out) *oom_out = false;
  size_t needed = 0;
  if (JsCopyString(key, nullptr, 0, &needed) != JsNoError) return false;
  if (needed == SIZE_MAX) {
    if (oom_out) *oom_out = true;
    return false;
  }
  try {
    out->assign(needed + 1, '\0');
  } catch (...) {
    if (oom_out) *oom_out = true;
    return false;
  }
  size_t written = 0;
  if (JsCopyString(key, out->data(), out->size(), &written) != JsNoError)
    return false;
  (*out)[written] = '\0';
  return true;
}

// Consider one key from one level's listing. Applies dedup, the
// skip/writable/enumerable/configurable filters (via the level's own
// descriptor), and numbers<->strings conversion, then appends the
// emitted value to `out` (each push JsAddRef's into the
// ScratchVector).
napi_status ConsiderKey(napi_env env, napi_value level, JsValueRef key,
                        napi_key_filter filter, napi_key_conversion conversion,
                        JsPropertyIdRef id_writable, JsPropertyIdRef id_enum,
                        JsPropertyIdRef id_conf, JsPropertyIdRef id_get,
                        JsPropertyIdRef id_set,
                        ScratchVector* out) {
  JsValueType kt = JsUndefined;
  JsErrorCode te = JsGetValueType(key, &kt);
  if (te != JsNoError) return MapJsError(env, te, "property-names: key type");
  const bool is_symbol = (kt == JsSymbol);
  const bool is_string = (kt == JsString);
  if (!is_symbol && !is_string) return napi_ok;  // unexpected; skip
  // Dedup across levels (own first wins, shadowing proto keys).
  for (size_t i = 0; i < out->size(); ++i) {
    bool eq = false;
    JsErrorCode ee = JsStrictEquals((*out)[i], key, &eq);
    if (ee != JsNoError) return MapJsError(env, ee, "property-names: dedup");
    if (eq) return napi_ok;
  }
  JsValueRef desc = JS_INVALID_REFERENCE;
  JsErrorCode e = JsObjectGetOwnPropertyDescriptor(ToJs(level), key, &desc);
  if (e != JsNoError) return MapJsError(env, e, "property-names: descriptor");
  // A missing own descriptor (proxy trap raced the listing) skips.
  JsValueType dt = JsUndefined;
  if (JsGetValueType(desc, &dt) != JsNoError || dt == JsUndefined) {
    return napi_ok;
  }
  ScratchRoot desc_root(desc);
  bool enum_b = false, conf_b = false, writable_b = false;
  if (!ReadDescBool(desc, id_enum, &enum_b)) {
    nodeapi::SetLastError(env, napi_generic_failure,
                          "property-names: enumerable flag");
    return napi_generic_failure;
  }
  if (!ReadDescBool(desc, id_conf, &conf_b)) {
    nodeapi::SetLastError(env, napi_generic_failure,
                          "property-names: configurable flag");
    return napi_generic_failure;
  }
  bool is_accessor = !DescIsUndefined(desc, id_get) || !DescIsUndefined(desc, id_set);
  if (!is_accessor) {
    if (!ReadDescBool(desc, id_writable, &writable_b)) {
      nodeapi::SetLastError(env, napi_generic_failure,
                            "property-names: writable flag");
      return napi_generic_failure;
    }
  } else {
    writable_b = false;  // accessors carry no writable bit (see FieldFlags)
  }
  if ((filter & napi_key_skip_strings) && is_string) return napi_ok;
  if ((filter & napi_key_skip_symbols) && is_symbol) return napi_ok;
  if ((filter & napi_key_enumerable) && !enum_b) return napi_ok;
  if ((filter & napi_key_configurable) && !conf_b) return napi_ok;
  if ((filter & napi_key_writable) && !writable_b) return napi_ok;

  JsValueRef emitted = key;
  if (conversion == napi_key_keep_numbers && is_string) {
    std::vector<char> buf;
    bool copy_oom = false;
    if (CopyKeyString(key, &buf, &copy_oom)) {
      uint32_t idx = 0;
      if (ParseArrayIndexString(buf.data(), &idx)) {
        emitted = IndexNumber(idx);
      }
    } else if (copy_oom) {
      nodeapi::SetLastError(env, napi_generic_failure,
                            "property-names: key copy OOM");
      return napi_generic_failure;
    }
  }
  if (!out->push(emitted)) {
    nodeapi::SetLastError(env, napi_generic_failure,
                          "property-names: scratch OOM");
    return napi_generic_failure;
  }
  return napi_ok;
}

// Shared core for get_property_names / get_all_property_names. `object`
// is already ToObject-coerced by the caller. The returned array is
// NOT rooted (the public entry roots it via RootAndPublish).
napi_status CollectPropertyNames(napi_env env, napi_value object,
                                 napi_key_collection_mode mode,
                                 napi_key_filter filter,
                                 napi_key_conversion conversion,
                                 JsValueRef* out_array) {
  JsPropertyIdRef id_writable = JS_INVALID_REFERENCE;
  JsPropertyIdRef id_enum = JS_INVALID_REFERENCE;
  JsPropertyIdRef id_conf = JS_INVALID_REFERENCE;
  JsPropertyIdRef id_get = JS_INVALID_REFERENCE;
  JsPropertyIdRef id_set = JS_INVALID_REFERENCE;
  JsErrorCode e = JsNoError;
  e = MakeId("writable", &id_writable);
  if (e == JsNoError) e = MakeId("enumerable", &id_enum);
  if (e == JsNoError) e = MakeId("configurable", &id_conf);
  if (e == JsNoError) e = MakeId("get", &id_get);
  if (e == JsNoError) e = MakeId("set", &id_set);
  if (e != JsNoError) return MapJsError(env, e, "property-names: flag ids");

  ScratchVector keys;
  napi_value level = object;
  ScratchRoot level_root(FromJs(ToJs(object)));  // holds proto chain refs
  int depth_guard = 0;
  while (level != nullptr && ++depth_guard <= 1024) {
    JsValueRef names = JS_INVALID_REFERENCE;
    e = JsGetOwnPropertyNames(ToJs(level), &names);
    if (e != JsNoError) return MapJsError(env, e, "property-names: names");
    ScratchRoot names_root(names);
    JsValueRef syms = JS_INVALID_REFERENCE;
    e = JsGetOwnPropertySymbols(ToJs(level), &syms);
    if (e != JsNoError) return MapJsError(env, e, "property-names: symbols");
    ScratchRoot syms_root(syms);
    // String keys (indices come back as strings, per the engine).
    {
      JsPropertyIdRef id_len = JS_INVALID_REFERENCE;
      JsErrorCode le = MakeId("length", &id_len);
      if (le != JsNoError) return MapJsError(env, le, "property-names: length id");
      JsValueRef len_v = JS_INVALID_REFERENCE;
      le = JsGetProperty(names, id_len, &len_v);
      if (le != JsNoError) return MapJsError(env, le, "property-names: length");
      double cnt = 0.0;
      le = JsNumberToDouble(len_v, &cnt);
      if (le != JsNoError) return MapJsError(env, le, "property-names: count");
      uint32_t n = (cnt <= 0.0) ? 0u : static_cast<uint32_t>(cnt);
      for (uint32_t i = 0; i < n; ++i) {
        JsValueRef idx = IndexNumber(i);
        JsValueRef el = JS_INVALID_REFERENCE;
        le = JsGetIndexedProperty(names, idx, &el);
        if (le != JsNoError) return MapJsError(env, le, "property-names: element");
        napi_status s = ConsiderKey(env, level, el, filter, conversion,
                                   id_writable, id_enum, id_conf, id_get,
                                   id_set, &keys);
        if (s != napi_ok) return s;
      }
    }
    // Symbol keys.
    {
      JsPropertyIdRef id_len = JS_INVALID_REFERENCE;
      JsErrorCode le = MakeId("length", &id_len);
      if (le != JsNoError) return MapJsError(env, le, "property-names: length id");
      JsValueRef len_v = JS_INVALID_REFERENCE;
      le = JsGetProperty(syms, id_len, &len_v);
      if (le != JsNoError) return MapJsError(env, le, "property-names: length");
      double cnt = 0.0;
      le = JsNumberToDouble(len_v, &cnt);
      if (le != JsNoError) return MapJsError(env, le, "property-names: count");
      uint32_t n = (cnt <= 0.0) ? 0u : static_cast<uint32_t>(cnt);
      for (uint32_t i = 0; i < n; ++i) {
        JsValueRef idx = IndexNumber(i);
        JsValueRef el = JS_INVALID_REFERENCE;
        le = JsGetIndexedProperty(syms, idx, &el);
        if (le != JsNoError) return MapJsError(env, le, "property-names: element");
        napi_status s = ConsiderKey(env, level, el, filter, conversion,
                                   id_writable, id_enum, id_conf, id_get,
                                   id_set, &keys);
        if (s != napi_ok) return s;
      }
    }
    if (mode == napi_key_own_only) break;
    JsValueRef proto = JS_INVALID_REFERENCE;
    e = JsGetPrototype(ToJs(level), &proto);
    if (e != JsNoError) return MapJsError(env, e, "property-names: prototype");
    if (proto == JS_INVALID_REFERENCE) break;
    JsValueType pt = JsUndefined;
    JsErrorCode pe = JsGetValueType(proto, &pt);
    if (pe != JsNoError) return MapJsError(env, pe, "property-names: proto type");
    if (pt == JsNull) break;
    level_root.reset();
    // Root the next level for its iteration; the ScratchVector owns
    // extracted keys, so releasing the previous level is safe.
    level_root = ScratchRoot(proto);
    level = FromJs(proto);
  }

  // Exact-sized result array, filled by indexed set.
  JsValueRef arr = JS_INVALID_REFERENCE;
  if (keys.size() > static_cast<size_t>(std::numeric_limits<unsigned int>::max())) {
    nodeapi::SetLastError(env, napi_generic_failure,
                          "property-names: too many keys");
    return napi_generic_failure;
  }
  e = JsCreateArray(static_cast<unsigned int>(keys.size()), &arr);
  if (e != JsNoError) return MapJsError(env, e, "property-names: result array");
  ScratchRoot arr_root(arr);
  for (size_t i = 0; i < keys.size(); ++i) {
    JsValueRef idx = IndexNumber(static_cast<uint32_t>(i));
    e = JsSetIndexedProperty(arr, idx, keys[i]);
    if (e != JsNoError) return MapJsError(env, e, "property-names: fill");
  }
  *out_array = arr;
  return napi_ok;
}




// Object.freeze / Object.seal via intrinsic calls (plan 4.3, D8):
// fetch globalThis.Object.<name> fresh each call (no cached roots --
// see header ownership note) and JsCallFunction it on the coerced
// object. thisArg == Object (the intrinsics ignore this).
napi_status CallIntrinsic(napi_env env, napi_value object, const char* prop,
                          const char* what) {
  JsValueRef global = JS_INVALID_REFERENCE;
  JsErrorCode e = JsGetGlobalObject(&global);
  if (e != JsNoError) return MapJsError(env, e, what);
  ScratchRoot global_root(global);
  JsPropertyIdRef id_obj = JS_INVALID_REFERENCE;
  e = MakeId("Object", &id_obj);
  if (e != JsNoError) return MapJsError(env, e, what);
  JsValueRef obj_ctor = JS_INVALID_REFERENCE;
  e = JsGetProperty(global, id_obj, &obj_ctor);
  if (e != JsNoError) return MapJsError(env, e, what);
  ScratchRoot ctor_root(obj_ctor);
  JsPropertyIdRef id_fn = JS_INVALID_REFERENCE;
  e = MakeId(prop, &id_fn);
  if (e != JsNoError) return MapJsError(env, e, what);
  JsValueRef fn = JS_INVALID_REFERENCE;
  e = JsGetProperty(obj_ctor, id_fn, &fn);
  if (e != JsNoError) return MapJsError(env, e, what);
  ScratchRoot fn_root(fn);
  JsValueType t = JsUndefined;
  JsErrorCode te = JsGetValueType(fn, &t);
  if (te != JsNoError) return MapJsError(env, te, what);
  if (t != JsFunction) {
    nodeapi::SetLastError(env, napi_generic_failure, what);
    return napi_generic_failure;
  }
  JsValueRef args[2] = {obj_ctor, ToJs(object)};
  JsValueRef res = JS_INVALID_REFERENCE;
  e = JsCallFunction(fn, args, 2, &res);
  if (e != JsNoError) return MapJsError(env, e, what);
  return napi_ok;
}

// MED-2 helper: Object.isExtensible probe (D8 intrinsic call).
// Exception-neutral: any failure (including a tampered global) clears
// any exception the probe itself set and reports check_ok=false, so
// the caller keeps its original mapping. On success *out_is_ext holds
// the verdict. No retention; fresh fetch per call (same policy as
// CallIntrinsic).
bool ProbeIsExtensible(JsValueRef obj, bool* out_is_ext) {
  *out_is_ext = true;
  JsValueRef global = JS_INVALID_REFERENCE;
  JsErrorCode e = JsGetGlobalObject(&global);
  if (e != JsNoError) return false;
  ScratchRoot global_root(global);
  JsPropertyIdRef id_obj = JS_INVALID_REFERENCE;
  e = MakeId("Object", &id_obj);
  if (e != JsNoError) return false;
  JsValueRef obj_ctor = JS_INVALID_REFERENCE;
  e = JsGetProperty(global, id_obj, &obj_ctor);
  if (e != JsNoError) return false;
  ScratchRoot ctor_root(obj_ctor);
  JsPropertyIdRef id_fn = JS_INVALID_REFERENCE;
  e = MakeId("isExtensible", &id_fn);
  if (e != JsNoError) return false;
  JsValueRef fn = JS_INVALID_REFERENCE;
  e = JsGetProperty(obj_ctor, id_fn, &fn);
  if (e != JsNoError) return false;
  ScratchRoot fn_root(fn);
  JsValueType t = JsUndefined;
  e = JsGetValueType(fn, &t);
  if (e != JsNoError || t != JsFunction) return false;
  JsValueRef args[2] = {obj_ctor, obj};
  JsValueRef res = JS_INVALID_REFERENCE;
  e = JsCallFunction(fn, args, 2, &res);
  if (e != JsNoError) {
    bool has = false;
    JsHasException(&has);
    if (has) {
      JsValueRef ex = JS_INVALID_REFERENCE;
      JsGetAndClearException(&ex);
    }
    return false;
  }
  bool b = true;
  e = JsBooleanToBool(res, &b);
  if (e != JsNoError) return false;
  *out_is_ext = b;
  return true;
}

} /* namespace */

extern "C" {

// ---------------------------------------------------------------------
// Creates (C-column Yes: execute with pending)
// ---------------------------------------------------------------------

napi_status NAPI_CDECL napi_create_object(napi_env env, napi_value* result) {
  if (result == nullptr) {
    if (env) nodeapi::SetLastError(env, napi_invalid_arg,
                                    "create_object: result is NULL");
    return napi_invalid_arg;
  }
  nodeapi::EntryGuard guard(env, /*allowClosing=*/false,
                            /*allowPending=*/true, /*needContext=*/true);
  if (!guard.ok()) return guard.status();
  JsValueRef obj = JS_INVALID_REFERENCE;
  JsErrorCode e = JsCreateObject(&obj);
  if (e != JsNoError) return MapJsError(env, e, "JsCreateObject");
  napi_status s = RootAndPublish(env, obj, result);
  if (s != napi_ok) return s;
  nodeapi::ClearLastError(env);
  return napi_ok;
}

napi_status NAPI_CDECL napi_create_array(napi_env env, napi_value* result) {
  if (result == nullptr) {
    if (env) nodeapi::SetLastError(env, napi_invalid_arg,
                                    "create_array: result is NULL");
    return napi_invalid_arg;
  }
  nodeapi::EntryGuard guard(env, /*allowClosing=*/false,
                            /*allowPending=*/true, /*needContext=*/true);
  if (!guard.ok()) return guard.status();
  JsValueRef arr = JS_INVALID_REFERENCE;
  JsErrorCode e = JsCreateArray(0, &arr);
  if (e != JsNoError) return MapJsError(env, e, "JsCreateArray");
  napi_status s = RootAndPublish(env, arr, result);
  if (s != napi_ok) return s;
  nodeapi::ClearLastError(env);
  return napi_ok;
}

napi_status NAPI_CDECL napi_create_array_with_length(napi_env env,
                                                      size_t length,
                                                      napi_value* result) {
  if (result == nullptr) {
    if (env) nodeapi::SetLastError(env, napi_invalid_arg,
                                    "create_array_with_length: result is NULL");
    return napi_invalid_arg;
  }
  nodeapi::EntryGuard guard(env, /*allowClosing=*/false,
                            /*allowPending=*/true, /*needContext=*/true);
  if (!guard.ok()) return guard.status();
  // N5.1 vendored-oracle (Node behavior): NewWithLength(4294967295)
  // does get_value_int32 -> -1 -> (size_t)SIZE_MAX -> create_array_with_length.
  // Node returns an empty array (length 0, instanceof Array) with napi_ok,
  // not generic_failure, so the vendored driver passes on Node. The
  // provider mirrors that here: over-max lengths yield a length-0 array.
  // (The documented max 2^32-1 is still honored for in-range lengths.)
  unsigned int len32 = 0;
  if (length > static_cast<size_t>(std::numeric_limits<unsigned int>::max())) {
    len32 = 0;
  } else {
    len32 = static_cast<unsigned int>(length);
  }
  JsValueRef arr = JS_INVALID_REFERENCE;
  JsErrorCode e = JsCreateArray(len32, &arr);
  if (e != JsNoError) return MapJsError(env, e, "JsCreateArray");
  napi_status s = RootAndPublish(env, arr, result);
  if (s != napi_ok) return s;
  nodeapi::ClearLastError(env);
  return napi_ok;
}

// ---------------------------------------------------------------------
// Array length (via length property read, brief requirement)
// ---------------------------------------------------------------------

napi_status NAPI_CDECL napi_get_array_length(napi_env env, napi_value value,
                                             uint32_t* result) {
  if (value == nullptr) {
    if (env) nodeapi::SetLastError(env, napi_invalid_arg,
                                    "get_array_length: value is NULL");
    return napi_invalid_arg;
  }
  if (result == nullptr) {
    if (env) nodeapi::SetLastError(env, napi_invalid_arg,
                                    "get_array_length: result is NULL");
    return napi_invalid_arg;
  }
  nodeapi::EntryGuard guard(env, /*allowClosing=*/false,
                            /*allowPending=*/false, /*needContext=*/true);
  if (!guard.ok()) return guard.status();
  JsValueType t = JsUndefined;
  JsErrorCode te = JsGetValueType(ToJs(value), &t);
  if (te != JsNoError) return MapJsError(env, te, "get_array_length: type query");
  if (t != JsArray) {
    nodeapi::SetLastError(env, napi_array_expected,
                          "get_array_length: not an array");
    return napi_array_expected;
  }
  JsPropertyIdRef length_id = JS_INVALID_REFERENCE;
  JsErrorCode e = MakeId("length", &length_id);
  if (e != JsNoError) return MapJsError(env, e, "get_array_length: length id");
  JsValueRef len = JS_INVALID_REFERENCE;
  e = JsGetProperty(ToJs(value), length_id, &len);
  if (e != JsNoError) return MapJsError(env, e, "get_array_length: length read");
  double d = 0.0;
  e = JsNumberToDouble(len, &d);
  if (e != JsNoError) return MapJsError(env, e, "get_array_length: length convert");
  uint32_t out = 0;
  // N2.1-style uint32 conversion of a number double: NaN -> 0,
  // negative -> 0, large -> clamp to 2^32-1, else truncate toward zero.
  // A genuine array length is always a small integer, so the clamp is
  // defensive only.
  if (d != d) {  // NaN
    out = 0;
  } else if (d <= 0.0) {
    out = 0;
  } else if (d >= 4294967295.0) {
    out = 4294967295u;
  } else {
    out = static_cast<uint32_t>(d);
  }
  *result = out;
  nodeapi::ClearLastError(env);
  return napi_ok;
}

// ---------------------------------------------------------------------
// Value-key property get/set/has/delete (+ has-own)
// ---------------------------------------------------------------------

napi_status NAPI_CDECL napi_get_property(napi_env env, napi_value object,
                                         napi_value key, napi_value* result) {
  if (object == nullptr || key == nullptr) {
    if (env) nodeapi::SetLastError(env, napi_invalid_arg, "get_property: NULL arg");
    return napi_invalid_arg;
  }
  if (result == nullptr) {
    if (env) nodeapi::SetLastError(env, napi_invalid_arg,
                                    "get_property: result is NULL");
    return napi_invalid_arg;
  }
  nodeapi::EntryGuard guard(env, /*allowClosing=*/false,
                            /*allowPending=*/false, /*needContext=*/true);
  if (!guard.ok()) return guard.status();
  napi_value obj = nullptr;
  napi_status s = ToObject(env, object, &obj);
  if (s != napi_ok) return s;
  ScratchRoot obj_root(ToJs(obj));
  ScratchRoot key_root(JS_INVALID_REFERENCE);
  JsValueRef key_js = JS_INVALID_REFERENCE;
  s = CoercePropertyKey(env, key, &key_root, &key_js);
  if (s != napi_ok) return s;
  JsValueRef v = JS_INVALID_REFERENCE;
  JsErrorCode e = JsObjectGetProperty(ToJs(obj), key_js, &v);
  if (e != JsNoError) return MapJsError(env, e, "get_property");
  s = RootAndPublish(env, v, result);
  if (s != napi_ok) return s;
  nodeapi::ClearLastError(env);
  return napi_ok;
}

napi_status NAPI_CDECL napi_set_property(napi_env env, napi_value object,
                                         napi_value key, napi_value value) {
  if (object == nullptr || key == nullptr || value == nullptr) {
    if (env) nodeapi::SetLastError(env, napi_invalid_arg, "set_property: NULL arg");
    return napi_invalid_arg;
  }
  nodeapi::EntryGuard guard(env, /*allowClosing=*/false,
                            /*allowPending=*/false, /*needContext=*/true);
  if (!guard.ok()) return guard.status();
  napi_value obj = nullptr;
  napi_status s = ToObject(env, object, &obj);
  if (s != napi_ok) return s;
  ScratchRoot obj_root(ToJs(obj));
  ScratchRoot key_root(JS_INVALID_REFERENCE);
  JsValueRef key_js = JS_INVALID_REFERENCE;
  s = CoercePropertyKey(env, key, &key_root, &key_js);
  if (s != napi_ok) return s;
  JsErrorCode e = JsObjectSetProperty(ToJs(obj), key_js, ToJs(value),
                                     /*useStrictRules=*/false);
  if (e != JsNoError) return MapJsError(env, e, "set_property");
  nodeapi::ClearLastError(env);
  return napi_ok;
}

napi_status NAPI_CDECL napi_has_property(napi_env env, napi_value object,
                                         napi_value key, bool* result) {
  if (object == nullptr || key == nullptr) {
    if (env) nodeapi::SetLastError(env, napi_invalid_arg, "has_property: NULL arg");
    return napi_invalid_arg;
  }
  if (result == nullptr) {
    if (env) nodeapi::SetLastError(env, napi_invalid_arg,
                                    "has_property: result is NULL");
    return napi_invalid_arg;
  }
  nodeapi::EntryGuard guard(env, /*allowClosing=*/false,
                            /*allowPending=*/false, /*needContext=*/true);
  if (!guard.ok()) return guard.status();
  napi_value obj = nullptr;
  napi_status s = ToObject(env, object, &obj);
  if (s != napi_ok) return s;
  ScratchRoot obj_root(ToJs(obj));
  ScratchRoot key_root(JS_INVALID_REFERENCE);
  JsValueRef key_js = JS_INVALID_REFERENCE;
  s = CoercePropertyKey(env, key, &key_root, &key_js);
  if (s != napi_ok) return s;
  bool has = false;
  JsErrorCode e = JsObjectHasProperty(ToJs(obj), key_js, &has);
  if (e != JsNoError) return MapJsError(env, e, "has_property");
  *result = has;
  nodeapi::ClearLastError(env);
  return napi_ok;
}

napi_status NAPI_CDECL napi_delete_property(napi_env env, napi_value object,
                                            napi_value key, bool* result) {
  if (object == nullptr || key == nullptr) {
    if (env) nodeapi::SetLastError(env, napi_invalid_arg, "delete_property: NULL arg");
    return napi_invalid_arg;
  }
  // NOTE: result == NULL is legal for deletion (vendored
  // test_object/test_null.js: "It's OK not to want the result").
  nodeapi::EntryGuard guard(env, /*allowClosing=*/false,
                            /*allowPending=*/false, /*needContext=*/true);
  if (!guard.ok()) return guard.status();
  napi_value obj = nullptr;
  napi_status s = ToObject(env, object, &obj);
  if (s != napi_ok) return s;
  ScratchRoot obj_root(ToJs(obj));
  ScratchRoot key_root(JS_INVALID_REFERENCE);
  JsValueRef key_js = JS_INVALID_REFERENCE;
  s = CoercePropertyKey(env, key, &key_root, &key_js);
  if (s != napi_ok) return s;
  JsValueRef deleted = JS_INVALID_REFERENCE;
  JsErrorCode e = JsObjectDeleteProperty(ToJs(obj), key_js,
                                        /*useStrictRules=*/false, &deleted);
  if (e != JsNoError) return MapJsError(env, e, "delete_property");
  if (result != nullptr) {
    bool b = false;
    if (JsBooleanToBool(deleted, &b) != JsNoError) {
      nodeapi::SetLastError(env, napi_generic_failure,
                            "delete_property: bad delete result");
      return napi_generic_failure;
    }
    *result = b;
  }
  nodeapi::ClearLastError(env);
  return napi_ok;
}

napi_status NAPI_CDECL napi_has_own_property(napi_env env, napi_value object,
                                             napi_value key, bool* result) {
  if (object == nullptr || key == nullptr) {
    if (env) nodeapi::SetLastError(env, napi_invalid_arg, "has_own_property: NULL arg");
    return napi_invalid_arg;
  }
  if (result == nullptr) {
    if (env) nodeapi::SetLastError(env, napi_invalid_arg,
                                    "has_own_property: result is NULL");
    return napi_invalid_arg;
  }
  nodeapi::EntryGuard guard(env, /*allowClosing=*/false,
                            /*allowPending=*/false, /*needContext=*/true);
  if (!guard.ok()) return guard.status();
  napi_value obj = nullptr;
  napi_status s = ToObject(env, object, &obj);
  if (s != napi_ok) return s;
  s = CheckName(env, key);
  if (s != napi_ok) return s;
  bool has = false;
  JsErrorCode e = JsObjectHasOwnProperty(ToJs(obj), ToJs(key), &has);
  if (e != JsNoError) return MapJsError(env, e, "has_own_property");
  *result = has;
  nodeapi::ClearLastError(env);
  return napi_ok;
}

// ---------------------------------------------------------------------
// Named (C-string) property get/set/has -- via JsCreatePropertyId
// ---------------------------------------------------------------------




napi_status NAPI_CDECL napi_get_named_property(napi_env env, napi_value object,
                                               const char* utf8name,
                                               napi_value* result) {
  if (object == nullptr) {
    if (env) nodeapi::SetLastError(env, napi_invalid_arg, "get_named_property: NULL arg");
    return napi_invalid_arg;
  }
  if (result == nullptr) {
    if (env) nodeapi::SetLastError(env, napi_invalid_arg,
                                    "get_named_property: result is NULL");
    return napi_invalid_arg;
  }
  nodeapi::EntryGuard guard(env, /*allowClosing=*/false,
                            /*allowPending=*/false, /*needContext=*/true);
  if (!guard.ok()) return guard.status();
  napi_value obj = nullptr;
  napi_status s = ToObject(env, object, &obj);
  if (s != napi_ok) return s;
  JsPropertyIdRef id = JS_INVALID_REFERENCE;
  s = NamedId(env, utf8name, &id, "get_named_property: utf8name is NULL");
  if (s != napi_ok) return s;
  JsValueRef v = JS_INVALID_REFERENCE;
  JsErrorCode e = JsGetProperty(ToJs(obj), id, &v);
  if (e != JsNoError) return MapJsError(env, e, "get_named_property");
  s = RootAndPublish(env, v, result);
  if (s != napi_ok) return s;
  nodeapi::ClearLastError(env);
  return napi_ok;
}

napi_status NAPI_CDECL napi_set_named_property(napi_env env, napi_value object,
                                               const char* utf8name,
                                               napi_value value) {
  if (object == nullptr || value == nullptr) {
    if (env) nodeapi::SetLastError(env, napi_invalid_arg, "set_named_property: NULL arg");
    return napi_invalid_arg;
  }
  nodeapi::EntryGuard guard(env, /*allowClosing=*/false,
                            /*allowPending=*/false, /*needContext=*/true);
  if (!guard.ok()) return guard.status();
  napi_value obj = nullptr;
  napi_status s = ToObject(env, object, &obj);
  if (s != napi_ok) return s;
  JsPropertyIdRef id = JS_INVALID_REFERENCE;
  s = NamedId(env, utf8name, &id, "set_named_property: utf8name is NULL");
  if (s != napi_ok) return s;
  JsErrorCode e = JsSetProperty(ToJs(obj), id, ToJs(value),
                                /*useStrictRules=*/false);
  if (e != JsNoError) return MapJsError(env, e, "set_named_property");
  nodeapi::ClearLastError(env);
  return napi_ok;
}

napi_status NAPI_CDECL napi_has_named_property(napi_env env, napi_value object,
                                               const char* utf8name,
                                               bool* result) {
  if (object == nullptr) {
    if (env) nodeapi::SetLastError(env, napi_invalid_arg, "has_named_property: NULL arg");
    return napi_invalid_arg;
  }
  if (utf8name == nullptr) {
    if (env) nodeapi::SetLastError(env, napi_invalid_arg,
                                    "has_named_property: utf8name is NULL");
    return napi_invalid_arg;
  }
  if (result == nullptr) {
    if (env) nodeapi::SetLastError(env, napi_invalid_arg,
                                    "has_named_property: result is NULL");
    return napi_invalid_arg;
  }
  nodeapi::EntryGuard guard(env, /*allowClosing=*/false,
                            /*allowPending=*/false, /*needContext=*/true);
  if (!guard.ok()) return guard.status();
  napi_value obj = nullptr;
  napi_status s = ToObject(env, object, &obj);
  if (s != napi_ok) return s;
  JsPropertyIdRef id = JS_INVALID_REFERENCE;
  s = NamedId(env, utf8name, &id, "has_named_property: utf8name is NULL");
  if (s != napi_ok) return s;
  bool has = false;
  JsErrorCode e = JsHasProperty(ToJs(obj), id, &has);
  if (e != JsNoError) return MapJsError(env, e, "has_named_property");
  *result = has;
  nodeapi::ClearLastError(env);
  return napi_ok;
}

// ---------------------------------------------------------------------
// Element get/set/has/delete -- uint32_t wrapped to a JS number
// ---------------------------------------------------------------------

napi_status NAPI_CDECL napi_get_element(napi_env env, napi_value object,
                                        uint32_t index, napi_value* result) {
  if (object == nullptr) {
    if (env) nodeapi::SetLastError(env, napi_invalid_arg, "get_element: NULL arg");
    return napi_invalid_arg;
  }
  if (result == nullptr) {
    if (env) nodeapi::SetLastError(env, napi_invalid_arg,
                                    "get_element: result is NULL");
    return napi_invalid_arg;
  }
  nodeapi::EntryGuard guard(env, /*allowClosing=*/false,
                            /*allowPending=*/false, /*needContext=*/true);
  if (!guard.ok()) return guard.status();
  napi_value obj = nullptr;
  napi_status s = ToObject(env, object, &obj);
  if (s != napi_ok) return s;
  JsValueRef idx = IndexNumber(index);
  JsValueRef v = JS_INVALID_REFERENCE;
  JsErrorCode e = JsGetIndexedProperty(ToJs(obj), idx, &v);
  if (e != JsNoError) return MapJsError(env, e, "get_element");
  s = RootAndPublish(env, v, result);
  if (s != napi_ok) return s;
  nodeapi::ClearLastError(env);
  return napi_ok;
}

napi_status NAPI_CDECL napi_set_element(napi_env env, napi_value object,
                                        uint32_t index, napi_value value) {
  if (object == nullptr || value == nullptr) {
    if (env) nodeapi::SetLastError(env, napi_invalid_arg, "set_element: NULL arg");
    return napi_invalid_arg;
  }
  nodeapi::EntryGuard guard(env, /*allowClosing=*/false,
                            /*allowPending=*/false, /*needContext=*/true);
  if (!guard.ok()) return guard.status();
  napi_value obj = nullptr;
  napi_status s = ToObject(env, object, &obj);
  if (s != napi_ok) return s;
  JsValueRef idx = IndexNumber(index);
  JsErrorCode e = JsSetIndexedProperty(ToJs(obj), idx, ToJs(value));
  if (e != JsNoError) return MapJsError(env, e, "set_element");
  nodeapi::ClearLastError(env);
  return napi_ok;
}

napi_status NAPI_CDECL napi_has_element(napi_env env, napi_value object,
                                        uint32_t index, bool* result) {
  if (object == nullptr) {
    if (env) nodeapi::SetLastError(env, napi_invalid_arg, "has_element: NULL arg");
    return napi_invalid_arg;
  }
  if (result == nullptr) {
    if (env) nodeapi::SetLastError(env, napi_invalid_arg,
                                    "has_element: result is NULL");
    return napi_invalid_arg;
  }
  nodeapi::EntryGuard guard(env, /*allowClosing=*/false,
                            /*allowPending=*/false, /*needContext=*/true);
  if (!guard.ok()) return guard.status();
  napi_value obj = nullptr;
  napi_status s = ToObject(env, object, &obj);
  if (s != napi_ok) return s;
  JsValueRef idx = IndexNumber(index);
  bool has = false;
  JsErrorCode e = JsHasIndexedProperty(ToJs(obj), idx, &has);
  if (e != JsNoError) return MapJsError(env, e, "has_element");
  *result = has;
  nodeapi::ClearLastError(env);
  return napi_ok;
}

napi_status NAPI_CDECL napi_delete_element(napi_env env, napi_value object,
                                           uint32_t index, bool* result) {
  if (object == nullptr) {
    if (env) nodeapi::SetLastError(env, napi_invalid_arg, "delete_element: NULL arg");
    return napi_invalid_arg;
  }
  // NOTE: result == NULL is legal (see delete_property).
  nodeapi::EntryGuard guard(env, /*allowClosing=*/false,
                            /*allowPending=*/false, /*needContext=*/true);
  if (!guard.ok()) return guard.status();
  napi_value obj = nullptr;
  napi_status s = ToObject(env, object, &obj);
  if (s != napi_ok) return s;
  JsValueRef idx = IndexNumber(index);
  bool need_bool = (result != nullptr);
  bool had_before = false;
  if (need_bool) {
    JsErrorCode e = JsHasIndexedProperty(ToJs(obj), idx, &had_before);
    if (e != JsNoError) return MapJsError(env, e, "delete_element: has-before");
  }
  JsErrorCode e = JsDeleteIndexedProperty(ToJs(obj), idx);
  if (e != JsNoError) return MapJsError(env, e, "delete_element");
  if (need_bool) {
    bool has_after = true;
    e = JsHasIndexedProperty(ToJs(obj), idx, &has_after);
    if (e != JsNoError) return MapJsError(env, e, "delete_element: has-after");
    // JsDeleteIndexedProperty yields no deleted flag; the documented
    // bool is "deleted iff present before and absent after".
    *result = (had_before && !has_after);
  }
  nodeapi::ClearLastError(env);
  return napi_ok;
}

// ---------------------------------------------------------------------
// napi_define_properties -- real descriptor objects + JsDefineProperty
// ---------------------------------------------------------------------




napi_status NAPI_CDECL napi_define_properties(
    napi_env env, napi_value object, size_t property_count,
    const napi_property_descriptor* properties) {
  if (object == nullptr) {
    if (env) nodeapi::SetLastError(env, napi_invalid_arg, "define_properties: NULL arg");
    return napi_invalid_arg;
  }
  if (property_count > 0 && properties == nullptr) {
    if (env) nodeapi::SetLastError(env, napi_invalid_arg,
                                    "define_properties: NULL descriptor list");
    return napi_invalid_arg;
  }
  nodeapi::EntryGuard guard(env, /*allowClosing=*/false,
                            /*allowPending=*/false, /*needContext=*/true);
  if (!guard.ok()) return guard.status();
  napi_value obj = nullptr;
  napi_status s = ToObject(env, object, &obj);
  if (s != napi_ok) return s;

  // Descriptor field ids, created once for the whole call.
  JsPropertyIdRef id_value = JS_INVALID_REFERENCE;
  JsPropertyIdRef id_get = JS_INVALID_REFERENCE;
  JsPropertyIdRef id_set = JS_INVALID_REFERENCE;
  JsPropertyIdRef id_writable = JS_INVALID_REFERENCE;
  JsPropertyIdRef id_enumerable = JS_INVALID_REFERENCE;
  JsPropertyIdRef id_configurable = JS_INVALID_REFERENCE;
  JsErrorCode e = JsNoError;
  e = MakeId("value", &id_value);
  if (e == JsNoError) e = MakeId("get", &id_get);
  if (e == JsNoError) e = MakeId("set", &id_set);
  if (e == JsNoError) e = MakeId("writable", &id_writable);
  if (e == JsNoError) e = MakeId("enumerable", &id_enumerable);
  if (e == JsNoError) e = MakeId("configurable", &id_configurable);
  if (e != JsNoError) return MapJsError(env, e, "define_properties: field ids");

  JsValueRef undef = UndefinedScratch();
  JsValueRef btrue = BoolScratch(true);
  JsValueRef bfalse = BoolScratch(false);

  for (size_t i = 0; i < property_count; ++i) {
    const napi_property_descriptor& d = properties[i];
    // --- Name resolution (exactly one of utf8name / name) ---
    JsPropertyIdRef pid = JS_INVALID_REFERENCE;
    if (d.utf8name != nullptr) {
      e = MakeId(d.utf8name, &pid);
      if (e != JsNoError) return MapJsError(env, e, "define_properties: name id");
    } else if (d.name != nullptr) {
      s = CheckName(env, d.name);
      if (s != napi_ok) return s;
      e = IdFromValue(env, d.name, &pid);
      if (e != JsNoError) return MapJsError(env, e, "define_properties: name id");
    } else {
      nodeapi::SetLastError(env, napi_invalid_arg,
                            "define_properties: descriptor has no name");
      return napi_invalid_arg;
    }
    // --- Descriptor kind: method > accessor > value ---
    // (matches the vendored test_properties pattern: method fns become
    // data values, getter+setter become accessors, value is verbatim.
    // Priority method-first mirrors the Node implementation.)
    const bool has_method = (d.method != nullptr);
    const bool has_accessor = (!has_method && (d.getter != nullptr || d.setter != nullptr));
    napi_value data_fn = nullptr;
    napi_value get_fn = nullptr;
    napi_value set_fn = nullptr;
    const char* fn_name = (d.utf8name != nullptr) ? d.utf8name : "";
    size_t fn_name_len = (d.utf8name != nullptr) ? NAPI_AUTO_LENGTH : 0;
    if (has_method) {
      s = napi_create_function(env, fn_name, fn_name_len, d.method, d.data,
                               &data_fn);
      if (s != napi_ok) return s;
    } else if (has_accessor) {
      if (d.getter != nullptr) {
        s = napi_create_function(env, fn_name, fn_name_len, d.getter, d.data,
                                 &get_fn);
        if (s != napi_ok) return s;
      }
      if (d.setter != nullptr) {
        s = napi_create_function(env, fn_name, fn_name_len, d.setter, d.data,
                                 &set_fn);
        if (s != napi_ok) return s;
      }
    } else {
      data_fn = (d.value != nullptr) ? d.value : FromJs(undef);
    }
    // --- Build the REAL descriptor object ---
    // AUDIT #7 ASYMMETRY (do not "simplify"): JsObjectDefinePropertyFull
    // (ChakraCore.h) cannot force false writable/enumerable/configurable
    // on properties that already exist -- it only ORs flags and drops
    // data out of the descriptor, so redefining an existing property
    // with false flags would silently keep true. JsDefineProperty on a
    // real descriptor object ({value,get,set,writable,enumerable,
    // configurable}) goes through the engine's DefineOwnProperty and
    // honors every bit on every define, new or existing. That is why
    // this function never calls JsObjectDefinePropertyFull: descriptor
    // fidelity (the probe's redefine-to-false case) is only achievable
    // via the real-object path. Accessor descriptors deliberately omit
    // value/writable and data descriptors omit get/set: putting the
    // other kind's fields on a descriptor object makes
    // ToPropertyDescriptor throw.
    JsValueRef desc_obj = JS_INVALID_REFERENCE;
    e = JsCreateObject(&desc_obj);
    if (e != JsNoError) return MapJsError(env, e, "define_properties: descriptor");
    ScratchRoot desc_root(desc_obj);
    JsValueRef v_writable = (d.attributes & napi_writable) ? btrue : bfalse;
    JsValueRef v_enum = (d.attributes & napi_enumerable) ? btrue : bfalse;
    JsValueRef v_conf = (d.attributes & napi_configurable) ? btrue : bfalse;
    if (has_accessor) {
      s = SetField(env, desc_obj, id_get,
                   (get_fn != nullptr) ? ToJs(get_fn) : undef,
                   "define_properties: get");
      if (s != napi_ok) return s;
      s = SetField(env, desc_obj, id_set,
                   (set_fn != nullptr) ? ToJs(set_fn) : undef,
                   "define_properties: set");
      if (s != napi_ok) return s;
      s = SetField(env, desc_obj, id_enumerable, v_enum,
                   "define_properties: enumerable");
      if (s != napi_ok) return s;
      s = SetField(env, desc_obj, id_configurable, v_conf,
                   "define_properties: configurable");
      if (s != napi_ok) return s;
    } else {
      s = SetField(env, desc_obj, id_value, ToJs(data_fn),
                   "define_properties: value");
      if (s != napi_ok) return s;
      s = SetField(env, desc_obj, id_writable, v_writable,
                   "define_properties: writable");
      if (s != napi_ok) return s;
      s = SetField(env, desc_obj, id_enumerable, v_enum,
                   "define_properties: enumerable");
      if (s != napi_ok) return s;
      s = SetField(env, desc_obj, id_configurable, v_conf,
                   "define_properties: configurable");
      if (s != napi_ok) return s;
    }
    bool defined = false;
    e = JsDefineProperty(ToJs(obj), pid, desc_obj, &defined);
    if (e == JsErrorScriptException || e == JsErrorInExceptionState) {
      // MED-2: JsRT throws a pending TypeError for a failing define on
      // a non-extensible (frozen/sealed) object, where V8 in Node's
      // define_properties reports false without throwing and Node maps
      // that to napi_invalid_arg with no pending exception. Convert
      // ONLY that case: capture the pending exception, probe
      // extensibility, and either clear+invalid_arg (non-extensible)
      // or restore the exception + pending (extensible or probe
      // failure). This narrow clear is the documented exception to
      // S3.10's "never cleared implicitly" for this Node-shape probe;
      // every other failure keeps the S3.10 mapping.
      JsValueRef pendingExc = JS_INVALID_REFERENCE;
      bool has = false;
      JsHasException(&has);
      if (has) {
        JsGetAndClearException(&pendingExc);
        bool is_ext = true;
        bool probe_ok = ProbeIsExtensible(ToJs(obj), &is_ext);
        if (probe_ok && !is_ext) {
          nodeapi::SetLastError(env, napi_invalid_arg,
                                "define_properties: object is not extensible");
          return napi_invalid_arg;
        }
        if (pendingExc != JS_INVALID_REFERENCE) {
          (void)JsSetException(pendingExc);
        }
        return MapJsError(env, e, "define_properties");
      }
      return MapJsError(env, e, "define_properties");
    }
    if (e != JsNoError) return MapJsError(env, e, "define_properties");
    if (!defined) {
      // No-error false (DontThrow shape): same Node mapping when the
      // object is non-extensible; otherwise keep the prior generic
      // failure (fail-safe: only the proven Node shape converts).
      bool is_ext = true;
      bool probe_ok = ProbeIsExtensible(ToJs(obj), &is_ext);
      if (probe_ok && !is_ext) {
        nodeapi::SetLastError(env, napi_invalid_arg,
                              "define_properties: object is not extensible");
        return napi_invalid_arg;
      }
      nodeapi::SetLastError(env, napi_generic_failure,
                            "define_properties: property not defined");
      return napi_generic_failure;
    }
    // desc_root releases at loop iteration end; function values live
    // on in the object's property (GC-tracked), scope-rooted if
    // napi_create_function rooted them (R-SC-1).
  }
  nodeapi::ClearLastError(env);
  return napi_ok;
}

// ---------------------------------------------------------------------
// Property names: own keys from JsGetOwnPropertyNames +
// JsGetOwnPropertySymbols; modes/filters/conversion via own-
// descriptor checks.
// ---------------------------------------------------------------------




napi_status NAPI_CDECL napi_get_property_names(napi_env env, napi_value object,
                                               napi_value* result) {
  if (object == nullptr) {
    if (env) nodeapi::SetLastError(env, napi_invalid_arg, "get_property_names: NULL arg");
    return napi_invalid_arg;
  }
  if (result == nullptr) {
    if (env) nodeapi::SetLastError(env, napi_invalid_arg,
                                    "get_property_names: result is NULL");
    return napi_invalid_arg;
  }
  nodeapi::EntryGuard guard(env, /*allowClosing=*/false,
                            /*allowPending=*/false, /*needContext=*/true);
  if (!guard.ok()) return guard.status();
  napi_value obj = nullptr;
  napi_status s = ToObject(env, object, &obj);
  if (s != napi_ok) return s;
  // napi_get_property_names == get_all(include prototypes, enumerable,
  // skip symbols, numbers to strings) per the vendored test_object
  // expectation (includes inherited, excludes symbols and
  // non-enumerable, indices as strings).
  JsValueRef arr = JS_INVALID_REFERENCE;
  s = CollectPropertyNames(env, obj, napi_key_include_prototypes,
                           static_cast<napi_key_filter>(napi_key_enumerable |
                                                        napi_key_skip_symbols),
                           napi_key_numbers_to_strings, &arr);
  if (s != napi_ok) return s;
  s = RootAndPublish(env, arr, result);
  if (s != napi_ok) return s;
  nodeapi::ClearLastError(env);
  return napi_ok;
}

napi_status NAPI_CDECL napi_get_all_property_names(
    napi_env env, napi_value object, napi_key_collection_mode key_mode,
    napi_key_filter key_filter, napi_key_conversion key_conversion,
    napi_value* result) {
  if (object == nullptr) {
    if (env) nodeapi::SetLastError(env, napi_invalid_arg,
                                    "get_all_property_names: NULL arg");
    return napi_invalid_arg;
  }
  if (result == nullptr) {
    if (env) nodeapi::SetLastError(env, napi_invalid_arg,
                                    "get_all_property_names: result is NULL");
    return napi_invalid_arg;
  }
  if (key_mode != napi_key_include_prototypes &&
      key_mode != napi_key_own_only) {
    if (env) nodeapi::SetLastError(env, napi_invalid_arg,
                                    "get_all_property_names: bad key_mode");
    return napi_invalid_arg;
  }
  if (key_conversion != napi_key_keep_numbers &&
      key_conversion != napi_key_numbers_to_strings) {
    if (env) nodeapi::SetLastError(env, napi_invalid_arg,
                                    "get_all_property_names: bad conversion");
    return napi_invalid_arg;
  }
  // Filter must be a subset of the five defined bits.
  {
    const int kKnown = static_cast<int>(napi_key_all_properties) |
                       static_cast<int>(napi_key_writable) |
                       static_cast<int>(napi_key_enumerable) |
                       static_cast<int>(napi_key_configurable) |
                       static_cast<int>(napi_key_skip_strings) |
                       static_cast<int>(napi_key_skip_symbols);
    if (key_filter < 0 || (static_cast<int>(key_filter) & ~kKnown) != 0) {
      if (env) nodeapi::SetLastError(env, napi_invalid_arg,
                                      "get_all_property_names: bad filter");
      return napi_invalid_arg;
    }
  }
  nodeapi::EntryGuard guard(env, /*allowClosing=*/false,
                            /*allowPending=*/false, /*needContext=*/true);
  if (!guard.ok()) return guard.status();
  napi_value obj = nullptr;
  napi_status s = ToObject(env, object, &obj);
  if (s != napi_ok) return s;
  JsValueRef arr = JS_INVALID_REFERENCE;
  s = CollectPropertyNames(env, obj, key_mode, key_filter, key_conversion, &arr);
  if (s != napi_ok) return s;
  s = RootAndPublish(env, arr, result);
  if (s != napi_ok) return s;
  nodeapi::ClearLastError(env);
  return napi_ok;
}

// ---------------------------------------------------------------------
// Prototype / freeze / seal / instanceof
// ---------------------------------------------------------------------

napi_status NAPI_CDECL napi_get_prototype(napi_env env, napi_value object,
                                          napi_value* result) {
  if (object == nullptr) {
    if (env) nodeapi::SetLastError(env, napi_invalid_arg, "get_prototype: NULL arg");
    return napi_invalid_arg;
  }
  if (result == nullptr) {
    if (env) nodeapi::SetLastError(env, napi_invalid_arg,
                                    "get_prototype: result is NULL");
    return napi_invalid_arg;
  }
  nodeapi::EntryGuard guard(env, /*allowClosing=*/false,
                            /*allowPending=*/false, /*needContext=*/true);
  if (!guard.ok()) return guard.status();
  napi_value obj = nullptr;
  napi_status s = ToObject(env, object, &obj);
  if (s != napi_ok) return s;
  JsValueRef proto = JS_INVALID_REFERENCE;
  JsErrorCode e = JsGetPrototype(ToJs(obj), &proto);
  if (e != JsNoError) return MapJsError(env, e, "get_prototype");
  s = RootAndPublish(env, proto, result);
  if (s != napi_ok) return s;
  nodeapi::ClearLastError(env);
  return napi_ok;
}

napi_status NAPI_CDECL NodeApiObjects_SetPrototype(napi_env env,
                                                   napi_value object,
                                                   napi_value prototype) {
  if (object == nullptr || prototype == nullptr) {
    if (env) nodeapi::SetLastError(env, napi_invalid_arg,
                                    "set_prototype: NULL arg");
    return napi_invalid_arg;
  }
  nodeapi::EntryGuard guard(env, /*allowClosing=*/false,
                            /*allowPending=*/false, /*needContext=*/true);
  if (!guard.ok()) return guard.status();
  napi_value obj = nullptr;
  napi_status s = ToObject(env, object, &obj);
  if (s != napi_ok) return s;
  JsErrorCode e = JsSetPrototype(ToJs(obj), ToJs(prototype));
  if (e != JsNoError) return MapJsError(env, e, "set_prototype");
  nodeapi::ClearLastError(env);
  return napi_ok;
}




napi_status NAPI_CDECL napi_object_freeze(napi_env env, napi_value object) {
  if (object == nullptr) {
    if (env) nodeapi::SetLastError(env, napi_invalid_arg, "object_freeze: NULL arg");
    return napi_invalid_arg;
  }
  nodeapi::EntryGuard guard(env, /*allowClosing=*/false,
                            /*allowPending=*/false, /*needContext=*/true);
  if (!guard.ok()) return guard.status();
  napi_value obj = nullptr;
  napi_status s = ToObject(env, object, &obj);
  if (s != napi_ok) return s;
  s = CallIntrinsic(env, obj, "freeze", "object_freeze");
  if (s != napi_ok) return s;
  nodeapi::ClearLastError(env);
  return napi_ok;
}

napi_status NAPI_CDECL napi_object_seal(napi_env env, napi_value object) {
  if (object == nullptr) {
    if (env) nodeapi::SetLastError(env, napi_invalid_arg, "object_seal: NULL arg");
    return napi_invalid_arg;
  }
  nodeapi::EntryGuard guard(env, /*allowClosing=*/false,
                            /*allowPending=*/false, /*needContext=*/true);
  if (!guard.ok()) return guard.status();
  napi_value obj = nullptr;
  napi_status s = ToObject(env, object, &obj);
  if (s != napi_ok) return s;
  s = CallIntrinsic(env, obj, "seal", "object_seal");
  if (s != napi_ok) return s;
  nodeapi::ClearLastError(env);
  return napi_ok;
}

napi_status NAPI_CDECL napi_instanceof(napi_env env, napi_value object,
                                       napi_value constructor, bool* result) {
  if (object == nullptr || constructor == nullptr) {
    if (env) nodeapi::SetLastError(env, napi_invalid_arg, "instanceof: NULL arg");
    return napi_invalid_arg;
  }
  if (result == nullptr) {
    if (env) nodeapi::SetLastError(env, napi_invalid_arg,
                                    "instanceof: result is NULL");
    return napi_invalid_arg;
  }
  nodeapi::EntryGuard guard(env, /*allowClosing=*/false,
                            /*allowPending=*/false, /*needContext=*/true);
  if (!guard.ok()) return guard.status();
  // Constructor: null/undefined -> object_expected (ToObject would
  // fail there); any other non-function -> function_expected. The
  // `object` operand takes no pre-check (5 instanceof F is false).
  JsValueType ct = JsUndefined;
  JsErrorCode te = JsGetValueType(ToJs(constructor), &ct);
  if (te != JsNoError) return MapJsError(env, te, "instanceof: ctor type");
  if (ct == JsUndefined || ct == JsNull) {
    nodeapi::SetLastError(env, napi_object_expected, "instanceof: ctor null/undefined");
    return napi_object_expected;
  }
  if (ct != JsFunction) {
    // Node (js_native_api_v8.cc napi_instanceof): throws TypeError
    // ERR_NAPI_CONS_FUNCTION AND returns napi_function_expected.
    // Both are observable (pending + last_error). The threw error
    // satisfies the vendored doInstanceOf(e, TypeError) check.
    napi_status ts = napi_throw_type_error(env, "ERR_NAPI_CONS_FUNCTION",
                                           "Constructor must be a function");
    if (ts != napi_ok) return ts;
    nodeapi::SetLastError(env, napi_function_expected,
                          "instanceof: constructor must be a function");
    return napi_function_expected;
  }
  bool r = false;
  JsErrorCode e = JsInstanceOf(ToJs(object), ToJs(constructor), &r);
  if (e != JsNoError) return MapJsError(env, e, "instanceof");
  *result = r;
  nodeapi::ClearLastError(env);
  return napi_ok;
}

} /* extern "C" */
