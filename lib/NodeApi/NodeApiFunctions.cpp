//-------------------------------------------------------------------------------------------------------
// Copyright (c) ChakraCore Project Contributors. All rights reserved.
// Licensed under the MIT license. See LICENSE.txt file in the project root for full license information.
//-------------------------------------------------------------------------------------------------------
//
// Node-API functions/callbacks (N2.4). See NodeApiFunctions.h for the
// surface list, the function-length deviation (name-length, not
// arity), and the ownership record.
//
// Status discipline (§3.10 / doc/node-api-status-mapping.md C column):
// validated-first (NULL binding args -> napi_invalid_arg before
// anything else), then the shared EntryGuard (Closing ->
// generic_failure, gated functions -> napi_pending_exception without
// executing, context entered via nodeapi::ContextGuard). Creates are
// C-column "No" for create_function/define_class/call/new (gated);
// get_cb_info/get_new_target are C-column "Yes" (execute with a
// pending exception, e.g. a callback that already threw can still
// inspect its own info). Type pre-checks produce napi_invalid_arg
// for non-function callees (CHECK_TO_FUNCTION in Node, NOT
// function_expected) and napi_name_expected for descriptor names
// via napi_define_properties. JsRT errors map per the §3.10 table.
// Success clears last_error (ClearLastError); failures SetLastError.
// Out-params are written only on success.
//
// Thunk (§3.8, semantics §5, R-CB-1..6):
//   - Invocation layout per bin/NativeTests/JsRTApiTest.cpp:734-866:
//     arguments[0] == info->thisArg, argumentCount includes `this`.
//     Hence argc = argumentCount-1, argv = arguments+1.
//   - newTarget: non-construct -> C NULL (Chakra passes undefined;
//     Node semantics want no-value, R-CB-5); construct ->
//     info->newTargetArg (explicit target from Reflect.construct,
//     not assumed to be the callee).
//   - Nesting: per-env callback stack, LIFO push/pop (R-CB-3).
//   - Exception restore (R-CB-4/AMB-CB-2): a pending exception
//     after the user callback wins; the return value is discarded
//     (even if garbage, cf. vendored TestBadReturnExceptionPending),
//     the record is popped, the implicit scope is closed, and the
//     exception is left pending for the JS caller.
//   - Balance (R-CB-6): EnforceImplicitBalance verifies
//     Depth == entry_depth+1. On mismatch the last_error is
//     napi_handle_scope_mismatch, extra scopes are unwound by the
//     helper, an Error is set as the pending exception (the
//     JS-visible surfacing for a thunk, which has no napi_status
//     return), the return is discarded, and cleanup proceeds.
//   - No C++ exception crosses extern "C": the user-callback call
//     is guarded (catch (...) => napi_generic_failure recorded,
//     pending preserved; return undefined).
//
// Thunk ownership (§3.7, see header record): each function owns a
// heap ThunkRecord carried by a hidden-symbol-anchored external
// object (JsCreateExternalObject + GC finalizer trampoline that
// only does `delete`). The callback stack itself is transient
// (in-flight invocations only) in a provider-side table keyed by
// env pointer value.

#include "NodeApiFunctions.h"

#include "NodeApiEnv.h"
#include "NodeApiScopes.h"
#include "ChakraCommon.h"
#include "ChakraCore.h"

#include <cstdint>
#include <cstring>
#include <limits>
#include <mutex>
#include <new>
#include <unordered_map>
#include <vector>

namespace {

JsValueRef ToJs(napi_value v) { return reinterpret_cast<JsValueRef>(v); }
napi_value FromJs(JsValueRef v) { return reinterpret_cast<napi_value>(v); }

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

JsValueRef UndefinedVal() {
  JsValueRef v = JS_INVALID_REFERENCE;
  JsGetUndefinedValue(&v);
  return v;
}
JsValueRef NullVal() {
  JsValueRef v = JS_INVALID_REFERENCE;
  JsGetNullValue(&v);
  return v;
}
JsValueRef BoolVal(bool b) {
  JsValueRef v = JS_INVALID_REFERENCE;
  JsBoolToBoolean(b, &v);
  return v;
}
JsValueRef NumberVal(double d) {
  JsValueRef v = JS_INVALID_REFERENCE;
  JsDoubleToNumber(d, &v);
  return v;
}

JsErrorCode MakeId(const char* name, JsPropertyIdRef* out) {
  return JsCreatePropertyId(name, std::strlen(name), out);
}

// ---------------------------------------------------------------------
// Thunk record and external anchor (§3.7)
// ---------------------------------------------------------------------

struct ThunkRecord {
  napi_env env = nullptr;
  napi_callback cb = nullptr;
  void* data = nullptr;
};

// GC-side trampoline (§3.7/D4): runs on an arbitrary GC thread with
// no current context. Only deletes the native record. No JS calls,
// no user finalizer, no provider-container mutation (the record is
// owned solely by the external object; no table entry to update).
void ThunkExternalFinalizer(void* data) {
  delete static_cast<ThunkRecord*>(data);
}

// ---------------------------------------------------------------------
// Per-env callback stack (§3.8/R-CB-1..3)
// ---------------------------------------------------------------------

constexpr std::uint64_t kCbMagic = 0xC0FFEE5F00DCA11ULL;

struct CallbackInfo {
  std::uint64_t magic = kCbMagic;
  napi_env env = nullptr;
  napi_value thisArg = nullptr;
  // Borrowed pointer into the engine arguments array (arguments+1);
  // each element is additionally rooted in the implicit scope (R1).
  napi_value* argv = nullptr;
  size_t argc = 0;
  // C NULL for non-construct; otherwise newTargetArg (R-CB-5).
  napi_value newTarget = nullptr;
  napi_value callee = nullptr;
  void* data = nullptr;
};

// Provider-side table: env pointer value -> stack of in-flight
// CallbackInfo addresses. Values are stack addresses in thunk
// frames; entries exist only during the callback. The map entry is
// erased when the stack empties, so no entry outlives a callback
// (no leak across env destroy for the transient stack itself).
// The mutex is held only to push/pop/lookup, never across user
// code (reentrancy, memo R-ENV-12).
std::mutex g_cb_mutex;
std::unordered_map<napi_env, std::vector<CallbackInfo*>> g_cb_stacks;

bool PushInfo(napi_env env, CallbackInfo* info) {
  std::lock_guard<std::mutex> lk(g_cb_mutex);
  try {
    g_cb_stacks[env].push_back(info);
    return true;
  } catch (...) {
    return false;
  }
}

void PopInfo(napi_env env, CallbackInfo* info) {
  std::lock_guard<std::mutex> lk(g_cb_mutex);
  auto it = g_cb_stacks.find(env);
  if (it == g_cb_stacks.end()) return;
  auto& v = it->second;
  // LIFO: the top must be `info`. If a callback misbehaved and the
  // top differs, unwind back to `info` (best-effort restore;
  // the mismatch is already surfaced as handle_scope_mismatch).
  for (;;) {
    if (v.empty()) break;
    CallbackInfo* top = v.back();
    v.pop_back();
    if (top == info) break;
  }
  if (v.empty()) g_cb_stacks.erase(it);
}

CallbackInfo* FindInfo(napi_env env, napi_callback_info h) {
  if (h == nullptr) return nullptr;
  auto* raw = reinterpret_cast<CallbackInfo*>(h);
  std::lock_guard<std::mutex> lk(g_cb_mutex);
  auto it = g_cb_stacks.find(env);
  if (it == g_cb_stacks.end()) return nullptr;
  for (CallbackInfo* p : it->second) {
    if (p == raw) {
      // Pointer-identity hit: safe to read. Verify magic as a
      // belt-and-braces (stack address reuse across frames is
      // impossible while both frames are live).
      if (p->magic != kCbMagic) return nullptr;
      return p;
    }
  }
  return nullptr;
}

bool HasPending() {
  bool has = false;
  JsErrorCode e = JsHasException(&has);
  (void)e;
  return (e == JsNoError && has);
}

// Set a pending Error("handle scope mismatch") if none is pending
// (R-CB-6 surfacing). Best-effort: ignores JsRT failures (at most
// last_error already records the mismatch).
void SetMismatchPending(napi_env env) {
  if (HasPending()) return;
  JsValueRef msg = JS_INVALID_REFERENCE;
  if (JsCreateString("handle scope mismatch", 21, &msg) != JsNoError) return;
  ScratchRoot msgRoot(msg);
  JsValueRef err = JS_INVALID_REFERENCE;
  if (JsCreateError(msg, &err) != JsNoError) return;
  ScratchRoot errRoot(err);
  (void)JsSetException(err);
}

// ---------------------------------------------------------------------
// Thunk (§3.8)
// ---------------------------------------------------------------------

JsValueRef ThunkInvoke(JsValueRef callee, JsValueRef* arguments,
                                unsigned short argumentCount,
                                JsNativeFunctionInfo* info, void* state) {
  ThunkRecord* rec = static_cast<ThunkRecord*>(state);
  JsValueRef undef = UndefinedVal();
  if (rec == nullptr || rec->cb == nullptr || rec->env == nullptr) {
    return undef;
  }
  napi_env env = rec->env;
  // Validate env/state (owner thread + Alive). On failure the
  // function returns undefined without invoking user code.
  if (nodeapi::CheckAlive(env) != napi_ok) return undef;
  nodeapi::ContextGuard guard(env);

  // ---- Derive the callback record (arguments[0]==this, oracle) ----
  JsValueRef thisJs = JS_INVALID_REFERENCE;
  bool isConstruct = false;
  napi_value newTarget = nullptr;
  if (info != nullptr) {
    thisJs = info->thisArg;
    isConstruct = info->isConstructCall;
    if (isConstruct) {
      JsValueRef nt = info->newTargetArg;
      newTarget = (nt == JS_INVALID_REFERENCE) ? nullptr : FromJs(nt);
    } else {
      // Chakra passes undefined for non-construct; Node wants C
      // NULL (R-CB-5). Never store the undefined value.
      newTarget = nullptr;
    }
  } else {
    if (argumentCount > 0 && arguments != nullptr) {
      thisJs = arguments[0];
    } else {
      thisJs = undef;
    }
    isConstruct = false;
    newTarget = nullptr;
  }
  if (thisJs == JS_INVALID_REFERENCE) thisJs = undef;

  size_t argc = 0;
  JsValueRef* argvPtr = nullptr;
  if (argumentCount == 0 || arguments == nullptr) {
    argc = 0;
    argvPtr = nullptr;
  } else {
    argc = static_cast<size_t>(argumentCount - 1);
    argvPtr = arguments + 1;
  }

  size_t entry_depth = NodeApiScopes_Depth(env);
  napi_handle_scope implicit = nullptr;
  napi_status openSt = NodeApiScopes_OpenImplicit(env, &implicit);
  if (openSt != napi_ok || implicit == nullptr) {
    nodeapi::SetLastError(env, openSt != napi_ok ? openSt : napi_generic_failure,
                          "thunk: implicit scope");
    return undef;
  }

  CallbackInfo cbInfo;
  cbInfo.magic = kCbMagic;
  cbInfo.env = env;
  cbInfo.thisArg = FromJs(thisJs);
  cbInfo.argv = reinterpret_cast<napi_value*>(argvPtr);
  cbInfo.argc = argc;
  cbInfo.newTarget = newTarget;
  cbInfo.callee = (callee == JS_INVALID_REFERENCE) ? nullptr : FromJs(callee);
  cbInfo.data = rec->data;

  // Root the record's values into the implicit scope (R1: provider
  // storage must be rooted). These roots live until implicit close.
  bool rootOk = true;
  if (NodeApiScopes_RootIntoInnermost(env, cbInfo.thisArg) != napi_ok) rootOk = false;
  if (rootOk && argvPtr != nullptr) {
    for (size_t i = 0; i < argc; ++i) {
      if (NodeApiScopes_RootIntoInnermost(env, FromJs(argvPtr[i])) != napi_ok) {
        rootOk = false;
        break;
      }
    }
  }
  if (rootOk && newTarget != nullptr) {
    if (NodeApiScopes_RootIntoInnermost(env, newTarget) != napi_ok) rootOk = false;
  }
  if (rootOk && cbInfo.callee != nullptr) {
    if (NodeApiScopes_RootIntoInnermost(env, cbInfo.callee) != napi_ok) rootOk = false;
  }
  if (!rootOk) {
    (void)napi_close_handle_scope(env, implicit);
    return undef;
  }

  if (!PushInfo(env, &cbInfo)) {
    nodeapi::SetLastError(env, napi_generic_failure, "thunk: callback stack OOM");
    (void)napi_close_handle_scope(env, implicit);
    return undef;
  }

  // ---- Invoke the user callback (C++-exception guarded) ----
  napi_value result = nullptr;
  bool cppEx = false;
  try {
    result = rec->cb(env, reinterpret_cast<napi_callback_info>(&cbInfo));
  } catch (...) {
    cppEx = true;
    nodeapi::SetLastError(env, napi_generic_failure,
                          "thunk: user callback C++ exception");
    result = nullptr;
  }

  bool hasPending = HasPending();
  JsValueRef toReturn = undef;
  const bool discardResult = hasPending || cppEx;
  if (!discardResult) {
    if (result == nullptr || result == reinterpret_cast<napi_value>(JS_INVALID_REFERENCE)) {
      toReturn = undef;
    } else {
      // Root the return into the caller's scope BEFORE closing
      // the implicit scope (R-SC-9 / §3.8 step 4), EXCEPT when the
      // caller is the default scope (outermost JS call). A JS-returned
      // value must be owned by the JS call frame / caller's variable
      // (collectible), not pinned in the provider default scope until
      // destroy -- otherwise napi_wrap finalizers on JS-created tracked
      // functions never fire (vendored test_function). Nested napi
      // returns (entry_depth > 1) still root into the outer implicit.
      if (entry_depth <= 1) {
        toReturn = ToJs(result);
      } else {
        napi_status rootSt = NodeApiScopes_RootIntoCallerScope(env, result);
        if (rootSt == napi_ok) {
          toReturn = ToJs(result);
        } else {
          nodeapi::SetLastError(env, rootSt, "thunk: caller-scope root");
          toReturn = undef;
          if (!hasPending) SetMismatchPending(env);
        }
      }
    }
  }

  // Balance verification (R-CB-6). Never a leak/crash.
  napi_status bal = NodeApiScopes_EnforceImplicitBalance(env, entry_depth);
  if (bal != napi_ok) {
    nodeapi::SetLastError(env, bal, "thunk: balance");
    if (!HasPending()) SetMismatchPending(env);
    toReturn = undef;
  }

  if (NodeApiScopes_IsOpen(env, implicit)) {
    (void)napi_close_handle_scope(env, implicit);
  }
  PopInfo(env, &cbInfo);
  // JS-boundary last_error discipline (vendored testNapiStatus):
  // a callback that returns without a pending exception leaves
  // last_error cleared (Node's JS-call wrapper clears on clean
  // return), so the *next* callback's get_last_error_info sees
  // napi_ok. Internal failing calls inside the callback still
  // record last_error for get_last_error_info reads before return.
  if (!hasPending && !cppEx) {
    nodeapi::ClearLastError(env);
  }
  return toReturn;
}

// ---------------------------------------------------------------------
// Descriptor plumbing (N2.3 real-descriptor pattern)
// ---------------------------------------------------------------------

napi_status SetField(napi_env env, JsValueRef desc, JsPropertyIdRef id,
                     JsValueRef value, const char* what) {
  JsErrorCode e = JsSetProperty(desc, id, value, /*useStrictRules=*/false);
  return MapJsError(env, e, what);
}

// Creates a real descriptor object {value,writable,enumerable,
// configurable}. Never uses JsObjectDefinePropertyFull (audit #7).
napi_status CreateDescriptor(napi_env env, JsValueRef value, bool writable,
                             bool enumerable, bool configurable,
                             JsValueRef* outDesc) {
  JsValueRef desc = JS_INVALID_REFERENCE;
  JsErrorCode e = JsCreateObject(&desc);
  if (e != JsNoError) return MapJsError(env, e, "descriptor");
  ScratchRoot descRoot(desc);
  JsPropertyIdRef idValue = JS_INVALID_REFERENCE;
  JsPropertyIdRef idWritable = JS_INVALID_REFERENCE;
  JsPropertyIdRef idEnum = JS_INVALID_REFERENCE;
  JsPropertyIdRef idConf = JS_INVALID_REFERENCE;
  e = MakeId("value", &idValue);
  if (e == JsNoError) e = MakeId("writable", &idWritable);
  if (e == JsNoError) e = MakeId("enumerable", &idEnum);
  if (e == JsNoError) e = MakeId("configurable", &idConf);
  if (e != JsNoError) return MapJsError(env, e, "descriptor ids");
  napi_status s = SetField(env, desc, idValue, value, "descriptor value");
  if (s != napi_ok) return s;
  s = SetField(env, desc, idWritable, BoolVal(writable), "descriptor writable");
  if (s != napi_ok) return s;
  s = SetField(env, desc, idEnum, BoolVal(enumerable), "descriptor enumerable");
  if (s != napi_ok) return s;
  s = SetField(env, desc, idConf, BoolVal(configurable), "descriptor configurable");
  if (s != napi_ok) return s;
  // Transfer without leaking: the helper's ScratchRoot releases
  // on return, leaving `desc` unrooted for the brief synchronous
  // window until the caller's ScratchRoot re-roots it (no JsRT
  // call intervenes, so no GC point exists in the window; the
  // engine also tracks stack locals). No extra AddRef is taken
  // here.
  *outDesc = desc;
  return napi_ok;
}

napi_status DefineOnId(napi_env env, JsValueRef obj, JsPropertyIdRef id,
                       JsValueRef desc, const char* what) {
  bool defined = false;
  JsErrorCode e = JsDefineProperty(obj, id, desc, &defined);
  if (e != JsNoError) return MapJsError(env, e, what);
  if (!defined) {
    nodeapi::SetLastError(env, napi_generic_failure, what);
    return napi_generic_failure;
  }
  return napi_ok;
}

// Attach the external anchor to `fn` as a hidden symbol property.
// The symbol is fresh per function (unique, unforgeable from JS
// except by symbol enumeration, which only yields the opaque
// external). Non-enumerable: invisible to for...in/Object.keys;
// visible only via getOwnPropertySymbols (documented residual,
// same class as the N4.2 buffer marker).
napi_status AttachAnchor(napi_env env, JsValueRef fn, JsValueRef external) {
  JsValueRef descStr = JS_INVALID_REFERENCE;
  JsErrorCode e = JsCreateString("__chakraNodeApiThunk", 21, &descStr);
  if (e != JsNoError) return MapJsError(env, e, "anchor: desc string");
  ScratchRoot descStrRoot(descStr);
  JsValueRef sym = JS_INVALID_REFERENCE;
  e = JsCreateSymbol(descStr, &sym);
  if (e != JsNoError) return MapJsError(env, e, "anchor: symbol");
  ScratchRoot symRoot(sym);
  JsValueRef desc = JS_INVALID_REFERENCE;
  napi_status s = CreateDescriptor(env, external, /*writable=*/false,
                                   /*enumerable=*/false,
                                   /*configurable=*/true, &desc);
  if (s != napi_ok) return s;
  // CreateDescriptor transferred one extra AddRef to us; hold it
  // until the define completes, then release exactly once.
  ScratchRoot descRoot(desc);
  bool defined = false;
  e = JsObjectDefineProperty(fn, sym, desc, &defined);
  if (e != JsNoError) return MapJsError(env, e, "anchor: define");
  if (!defined) {
    nodeapi::SetLastError(env, napi_generic_failure, "anchor: not defined");
    return napi_generic_failure;
  }
  return napi_ok;
}

// Define function.length to 0 (Node truth: V8 Function::New
// defaults length to 0; the napi `length` argument is the name
// length, not an arity). Real descriptor, configurable per JS
// semantics (writable:false, enumerable:false, configurable:true).
napi_status DefineLengthZero(napi_env env, JsValueRef fn) {
  JsValueRef zero = NumberVal(0.0);
  JsValueRef desc = JS_INVALID_REFERENCE;
  napi_status s = CreateDescriptor(env, zero, /*writable=*/false,
                                   /*enumerable=*/false,
                                   /*configurable=*/true, &desc);
  if (s != napi_ok) return s;
  ScratchRoot descRoot(desc);
  JsPropertyIdRef id = JS_INVALID_REFERENCE;
  JsErrorCode e = MakeId("length", &id);
  if (e != JsNoError) return MapJsError(env, e, "length id");
  return DefineOnId(env, fn, id, desc, "length define");
}

napi_status DefineNameValue(napi_env env, JsValueRef fn, JsValueRef nameVal) {
  JsValueRef desc = JS_INVALID_REFERENCE;
  napi_status s = CreateDescriptor(env, nameVal, /*writable=*/false,
                                   /*enumerable=*/false,
                                   /*configurable=*/true, &desc);
  if (s != napi_ok) return s;
  ScratchRoot descRoot(desc);
  JsPropertyIdRef id = JS_INVALID_REFERENCE;
  JsErrorCode e = MakeId("name", &id);
  if (e != JsNoError) return MapJsError(env, e, "name id");
  return DefineOnId(env, fn, id, desc, "name define");
}

// Ensure `fn.prototype` is an object whose `constructor` is `fn`.
// Chakra external functions may already have one; only create it
// when absent (undefined).
napi_status EnsurePrototype(napi_env env, JsValueRef fn) {
  JsPropertyIdRef pidProto = JS_INVALID_REFERENCE;
  JsErrorCode e = MakeId("prototype", &pidProto);
  if (e != JsNoError) return MapJsError(env, e, "prototype id");
  JsValueRef proto = JS_INVALID_REFERENCE;
  e = JsGetProperty(fn, pidProto, &proto);
  if (e != JsNoError) return MapJsError(env, e, "prototype get");
  JsValueType t = JsUndefined;
  JsErrorCode te = JsGetValueType(proto, &t);
  if (te != JsNoError) return MapJsError(env, te, "prototype type");
  if (t == JsObject) {
    JsPropertyIdRef pidCtor = JS_INVALID_REFERENCE;
    e = MakeId("constructor", &pidCtor);
    if (e != JsNoError) return MapJsError(env, e, "constructor id");
    JsValueRef ctorVal = JS_INVALID_REFERENCE;
    e = JsGetProperty(proto, pidCtor, &ctorVal);
    if (e != JsNoError) return MapJsError(env, e, "constructor get");
    bool eq = false;
    JsErrorCode ee = JsStrictEquals(ctorVal, fn, &eq);
    if (ee == JsNoError && eq) return napi_ok;
    // (Re)point constructor at the function.
    JsValueRef desc = JS_INVALID_REFERENCE;
    napi_status s = CreateDescriptor(env, fn, /*writable=*/true,
                                     /*enumerable=*/false,
                                     /*configurable=*/true, &desc);
    if (s != napi_ok) return s;
    ScratchRoot descRoot(desc);
    return DefineOnId(env, proto, pidCtor, desc, "constructor define");
  }
  if (t != JsUndefined) {
    nodeapi::SetLastError(env, napi_generic_failure,
                          "prototype is not an object");
    return napi_generic_failure;  // unexpected engine shape
  }
  // Absent: create a fresh prototype object.
  JsValueRef newProto = JS_INVALID_REFERENCE;
  e = JsCreateObject(&newProto);
  if (e != JsNoError) return MapJsError(env, e, "prototype create");
  ScratchRoot newProtoRoot(newProto);
  // newProto.constructor = fn
  {
    JsValueRef desc = JS_INVALID_REFERENCE;
    napi_status s = CreateDescriptor(env, fn, /*writable=*/true,
                                     /*enumerable=*/false,
                                     /*configurable=*/true, &desc);
    if (s != napi_ok) return s;
    ScratchRoot descRoot(desc);
    JsPropertyIdRef pidCtor = JS_INVALID_REFERENCE;
    e = MakeId("constructor", &pidCtor);
    if (e != JsNoError) return MapJsError(env, e, "constructor id");
    s = DefineOnId(env, newProto, pidCtor, desc, "constructor define");
    if (s != napi_ok) return s;
  }
  // fn.prototype = newProto (writable, non-enumerable,
  // non-configurable per JS function semantics).
  {
    JsValueRef desc = JS_INVALID_REFERENCE;
    napi_status s = CreateDescriptor(env, newProto, /*writable=*/true,
                                     /*enumerable=*/false,
                                     /*configurable=*/false, &desc);
    if (s != napi_ok) return s;
    ScratchRoot descRoot(desc);
    s = DefineOnId(env, fn, pidProto, desc, "prototype define");
    if (s != napi_ok) return s;
  }
  return napi_ok;
}

// Shared function factory. Caller has validated binding args and
// holds the EntryGuard (context entered). Does NOT root the
// returned function (the caller roots exactly once, so
// root-count deltas are one per function).
napi_status CreateFunctionInternal(napi_env env, const char* utf8name,
                                   size_t length, napi_callback cb,
                                   void* data, JsValueRef* outFn) {
  // Name-length validation (Node CHECK_NEW_FROM_UTF8_LEN): only
  // when a name is present; length must be AUTO or <= INT_MAX.
  if (utf8name != nullptr && length != NAPI_AUTO_LENGTH &&
      length > static_cast<size_t>(std::numeric_limits<int>::max())) {
    nodeapi::SetLastError(env, napi_invalid_arg,
                          "create_function: name length exceeds INT_MAX");
    return napi_invalid_arg;
  }
  ThunkRecord* rec = nullptr;
  try {
    rec = new ThunkRecord{env, cb, data};
  } catch (...) {
    nodeapi::SetLastError(env, napi_generic_failure,
                          "create_function: thunk OOM");
    return napi_generic_failure;
  }
  if (rec == nullptr) {
    nodeapi::SetLastError(env, napi_generic_failure,
                          "create_function: thunk OOM");
    return napi_generic_failure;
  }

  // Metadata (name) string. Held alive across JsCreateEnhancedFunction.
  JsValueRef meta = JS_INVALID_REFERENCE;
  JsValueRef nameVal = JS_INVALID_REFERENCE;
  bool hasName = (utf8name != nullptr);
  size_t nameLen = 0;
  if (hasName) {
    nameLen = (length == NAPI_AUTO_LENGTH) ? std::strlen(utf8name) : length;
    JsErrorCode e = JsCreateString(utf8name, nameLen, &nameVal);
    if (e != JsNoError) {
      delete rec;
      return MapJsError(env, e, "create_function: name string");
    }
    meta = nameVal;
  }
  // ScratchRoot for the name string (released after the call).
  ScratchRoot metaRoot(hasName ? nameVal : JS_INVALID_REFERENCE);

  JsValueRef fn = JS_INVALID_REFERENCE;
  JsErrorCode e = JsCreateEnhancedFunction(ThunkInvoke, meta, rec, &fn);
  if (e != JsNoError) {
    delete rec;
    return MapJsError(env, e, "JsCreateEnhancedFunction");
  }
  ScratchRoot fnRoot(fn);

  // External anchor (transfer point for thunk ownership).
  JsValueRef external = JS_INVALID_REFERENCE;
  e = JsCreateExternalObject(rec, ThunkExternalFinalizer, &external);
  if (e != JsNoError) {
    delete rec;
    return MapJsError(env, e, "JsCreateExternalObject");
  }
  // Ownership of `rec` transfers to the external object here.
  // Any later failure must NOT delete `rec` inline (the finalizer
  // owns it; fn/external become garbage together).
  ScratchRoot extRoot(external);

  napi_status s = AttachAnchor(env, fn, external);
  if (s != napi_ok) return s;
  s = DefineLengthZero(env, fn);
  if (s != napi_ok) return s;
  if (hasName) {
    s = DefineNameValue(env, fn, nameVal);
    if (s != napi_ok) return s;
  }
  s = EnsurePrototype(env, fn);
  if (s != napi_ok) return s;

  *outFn = fn;
  return napi_ok;
}

napi_status RootAndPublish(napi_env env, JsValueRef value, napi_value* result) {
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

bool IsFunctionValue(napi_value v) {
  JsValueType t = JsUndefined;
  if (JsGetValueType(ToJs(v), &t) != JsNoError) return false;
  return t == JsFunction;
}

}  // namespace

extern "C" {

// ---------------------------------------------------------------------
// napi_create_function (C-column No: gated)
// ---------------------------------------------------------------------

napi_status NAPI_CDECL napi_create_function(napi_env env, const char* utf8name,
                                            size_t length, napi_callback cb,
                                            void* data, napi_value* result) {
  if (cb == nullptr) {
    if (env) nodeapi::SetLastError(env, napi_invalid_arg, "create_function: cb is NULL");
    return napi_invalid_arg;
  }
  if (result == nullptr) {
    if (env) nodeapi::SetLastError(env, napi_invalid_arg, "create_function: result is NULL");
    return napi_invalid_arg;
  }
  // Name-length validation precedes the guard when the name is
  // present (Node CHECK_NEW_FROM_UTF8_LEN order: preamble, then
  // arg checks; provider validates binding args first, §3.10).
  if (utf8name != nullptr && length != NAPI_AUTO_LENGTH &&
      length > static_cast<size_t>(std::numeric_limits<int>::max())) {
    if (env) nodeapi::SetLastError(env, napi_invalid_arg,
                                    "create_function: name length exceeds INT_MAX");
    return napi_invalid_arg;
  }
  nodeapi::EntryGuard guard(env, /*allowClosing=*/false,
                            /*allowPending=*/false, /*needContext=*/true);
  if (!guard.ok()) return guard.status();
  JsValueRef fn = JS_INVALID_REFERENCE;
  napi_status s = CreateFunctionInternal(env, utf8name, length, cb, data, &fn);
  if (s != napi_ok) return s;
  s = RootAndPublish(env, fn, result);
  if (s != napi_ok) return s;
  nodeapi::ClearLastError(env);
  return napi_ok;
}

// ---------------------------------------------------------------------
// napi_get_cb_info / napi_get_new_target (C-column Yes: execute)
// ---------------------------------------------------------------------

napi_status NAPI_CDECL napi_get_cb_info(napi_env env, napi_callback_info cbinfo,
                                        size_t* argc, napi_value* argv,
                                        napi_value* this_arg, void** data) {
  if (cbinfo == nullptr) {
    if (env) nodeapi::SetLastError(env, napi_invalid_arg, "get_cb_info: cbinfo is NULL");
    return napi_invalid_arg;
  }
  if (argv != nullptr && argc == nullptr) {
    if (env) nodeapi::SetLastError(env, napi_invalid_arg,
                                    "get_cb_info: argv without argc");
    return napi_invalid_arg;
  }
  nodeapi::EntryGuard guard(env, /*allowClosing=*/false,
                            /*allowPending=*/true, /*needContext=*/true);
  if (!guard.ok()) return guard.status();
  CallbackInfo* info = FindInfo(env, cbinfo);
  if (info == nullptr) {
    nodeapi::SetLastError(env, napi_invalid_arg, "get_cb_info: unknown cbinfo");
    return napi_invalid_arg;
  }
  const size_t actual = info->argc;
  if (argv != nullptr) {
    // argc is non-null here (validated above).
    size_t provided = *argc;
    size_t ncopy = provided < actual ? provided : actual;
    // Copy min(provided, actual).
    if (info->argv != nullptr) {
      for (size_t i = 0; i < ncopy; ++i) argv[i] = info->argv[i];
    } else {
      // No argument storage (zero-arg call with a null argv base):
      // treat as no args to copy (ncopy must be 0 when actual==0).
    }
    // If provided > actual, fill the excess with undefined (Node
    // Args() + docs: "the rest of argv is filled with undefined").
    if (provided > actual) {
      napi_value undef = FromJs(UndefinedVal());
      for (size_t i = actual; i < provided; ++i) argv[i] = undef;
    }
  }
  if (argc != nullptr) *argc = actual;
  if (this_arg != nullptr) *this_arg = info->thisArg;
  if (data != nullptr) *data = info->data;
  nodeapi::ClearLastError(env);
  return napi_ok;
}

napi_status NAPI_CDECL napi_get_new_target(napi_env env,
                                           napi_callback_info cbinfo,
                                           napi_value* result) {
  if (cbinfo == nullptr) {
    if (env) nodeapi::SetLastError(env, napi_invalid_arg, "get_new_target: cbinfo is NULL");
    return napi_invalid_arg;
  }
  if (result == nullptr) {
    if (env) nodeapi::SetLastError(env, napi_invalid_arg, "get_new_target: result is NULL");
    return napi_invalid_arg;
  }
  nodeapi::EntryGuard guard(env, /*allowClosing=*/false,
                            /*allowPending=*/true, /*needContext=*/true);
  if (!guard.ok()) return guard.status();
  CallbackInfo* info = FindInfo(env, cbinfo);
  if (info == nullptr) {
    nodeapi::SetLastError(env, napi_invalid_arg, "get_new_target: unknown cbinfo");
    return napi_invalid_arg;
  }
  // Non-construct -> C NULL; construct -> stored newTargetArg.
  *result = info->newTarget;
  nodeapi::ClearLastError(env);
  return napi_ok;
}

// ---------------------------------------------------------------------
// napi_call_function / napi_new_instance (C-column No: gated)
// ---------------------------------------------------------------------

napi_status NAPI_CDECL napi_call_function(napi_env env, napi_value recv,
                                          napi_value func, size_t argc,
                                          const napi_value* argv,
                                          napi_value* result) {
  if (recv == nullptr) {
    if (env) nodeapi::SetLastError(env, napi_invalid_arg, "call_function: recv is NULL");
    return napi_invalid_arg;
  }
  if (func == nullptr) {
    if (env) nodeapi::SetLastError(env, napi_invalid_arg, "call_function: func is NULL");
    return napi_invalid_arg;
  }
  if (argc > 0 && argv == nullptr) {
    if (env) nodeapi::SetLastError(env, napi_invalid_arg, "call_function: argv is NULL");
    return napi_invalid_arg;
  }
  nodeapi::EntryGuard guard(env, /*allowClosing=*/false,
                            /*allowPending=*/false, /*needContext=*/true);
  if (!guard.ok()) return guard.status();
  if (!IsFunctionValue(func)) {
    nodeapi::SetLastError(env, napi_invalid_arg, "call_function: not a function");
    return napi_invalid_arg;  // CHECK_TO_FUNCTION in Node (NOT function_expected)
  }
  // HIGH-1: `argc + 1 > 65535` wraps at SIZE_MAX (argc+1 == 0 passes
  // the check and then OOB-reads argv). Guard argc alone so
  // SIZE_MAX returns generically at once. The subsequent
  // `static_cast<unsigned short>(argc + 1)` is then exact.
  if (argc > 65534) {
    nodeapi::SetLastError(env, napi_generic_failure, "call_function: too many args");
    return napi_generic_failure;
  }
  // Chakra convention: arguments[0] == this (oracle). Build the
  // native array {recv, argv[0..]}.
  std::vector<JsValueRef> args;
  try {
    args.reserve(argc + 1);
    args.push_back(ToJs(recv));
    for (size_t i = 0; i < argc; ++i) args.push_back(ToJs(argv[i]));
  } catch (...) {
    nodeapi::SetLastError(env, napi_generic_failure, "call_function: args OOM");
    return napi_generic_failure;
  }
  JsValueRef jsResult = JS_INVALID_REFERENCE;
  JsValueRef* resultPtr = nullptr;
  if (result != nullptr) resultPtr = &jsResult;
  JsErrorCode e = JsCallFunction(ToJs(func), args.data(),
                                 static_cast<unsigned short>(argc + 1),
                                 resultPtr);
  if (e != JsNoError) return MapJsError(env, e, "call_function");
  if (result != nullptr) {
    napi_status s = RootAndPublish(env, jsResult, result);
    if (s != napi_ok) return s;
  }
  nodeapi::ClearLastError(env);
  return napi_ok;
}

napi_status NAPI_CDECL napi_new_instance(napi_env env, napi_value constructor,
                                         size_t argc, const napi_value* argv,
                                         napi_value* result) {
  if (constructor == nullptr) {
    if (env) nodeapi::SetLastError(env, napi_invalid_arg, "new_instance: constructor is NULL");
    return napi_invalid_arg;
  }
  if (argc > 0 && argv == nullptr) {
    if (env) nodeapi::SetLastError(env, napi_invalid_arg, "new_instance: argv is NULL");
    return napi_invalid_arg;
  }
  if (result == nullptr) {
    if (env) nodeapi::SetLastError(env, napi_invalid_arg, "new_instance: result is NULL");
    return napi_invalid_arg;
  }
  nodeapi::EntryGuard guard(env, /*allowClosing=*/false,
                            /*allowPending=*/false, /*needContext=*/true);
  if (!guard.ok()) return guard.status();
  if (!IsFunctionValue(constructor)) {
    nodeapi::SetLastError(env, napi_invalid_arg, "new_instance: not a constructor");
    return napi_invalid_arg;  // CHECK_TO_FUNCTION in Node (NOT function_expected)
  }
  // HIGH-1 (see napi_call_function): argc-only guard, no wrap at
  // SIZE_MAX. `static_cast<unsigned short>(argc + 1)` is exact below.
  if (argc > 65534) {
    nodeapi::SetLastError(env, napi_generic_failure, "new_instance: too many args");
    return napi_generic_failure;
  }
  // JsConstructObject uses the same arguments[0]==this layout as
  // JsCallFunction (oracle): slot 0 is the `this` the engine
  // allocates for the new object (the passed value is the slot,
  // replaced by the fresh object). Supply undefined as the slot.
  std::vector<JsValueRef> args;
  try {
    args.reserve(argc + 1);
    args.push_back(UndefinedVal());
    for (size_t i = 0; i < argc; ++i) args.push_back(ToJs(argv[i]));
  } catch (...) {
    nodeapi::SetLastError(env, napi_generic_failure, "new_instance: args OOM");
    return napi_generic_failure;
  }
  JsValueRef jsResult = JS_INVALID_REFERENCE;
  JsErrorCode e = JsConstructObject(ToJs(constructor), args.data(),
                                    static_cast<unsigned short>(argc + 1),
                                    &jsResult);
  if (e != JsNoError) return MapJsError(env, e, "new_instance");
  napi_status s = RootAndPublish(env, jsResult, result);
  if (s != napi_ok) return s;
  nodeapi::ClearLastError(env);
  return napi_ok;
}

// ---------------------------------------------------------------------
// napi_define_class (C-column No: gated)
// ---------------------------------------------------------------------

napi_status NAPI_CDECL napi_define_class(napi_env env, const char* utf8name,
                                         size_t length,
                                         napi_callback constructor, void* data,
                                         size_t property_count,
                                         const napi_property_descriptor* properties,
                                         napi_value* result) {
  if (utf8name == nullptr) {
    if (env) nodeapi::SetLastError(env, napi_invalid_arg, "define_class: utf8name is NULL");
    return napi_invalid_arg;
  }
  if (length != NAPI_AUTO_LENGTH &&
      length > static_cast<size_t>(std::numeric_limits<int>::max())) {
    if (env) nodeapi::SetLastError(env, napi_invalid_arg,
                                    "define_class: name length exceeds INT_MAX");
    return napi_invalid_arg;
  }
  if (constructor == nullptr) {
    if (env) nodeapi::SetLastError(env, napi_invalid_arg, "define_class: constructor is NULL");
    return napi_invalid_arg;
  }
  if (property_count > 0 && properties == nullptr) {
    if (env) nodeapi::SetLastError(env, napi_invalid_arg, "define_class: properties is NULL");
    return napi_invalid_arg;
  }
  if (result == nullptr) {
    if (env) nodeapi::SetLastError(env, napi_invalid_arg, "define_class: result is NULL");
    return napi_invalid_arg;
  }
  nodeapi::EntryGuard guard(env, /*allowClosing=*/false,
                            /*allowPending=*/false, /*needContext=*/true);
  if (!guard.ok()) return guard.status();

  JsValueRef ctor = JS_INVALID_REFERENCE;
  napi_status s = CreateFunctionInternal(env, utf8name, length, constructor,
                                         data, &ctor);
  if (s != napi_ok) return s;
  ScratchRoot ctorRoot(ctor);

  // Partition by napi_static (enhanced ctor + prototype split).
  std::vector<napi_property_descriptor> statics;
  std::vector<napi_property_descriptor> protos;
  try {
    for (size_t i = 0; i < property_count; ++i) {
      if (properties[i].attributes & napi_static) {
        statics.push_back(properties[i]);
      } else {
        protos.push_back(properties[i]);
      }
    }
  } catch (...) {
    nodeapi::SetLastError(env, napi_generic_failure, "define_class: split OOM");
    return napi_generic_failure;
  }

  if (!protos.empty()) {
    JsPropertyIdRef pidProto = JS_INVALID_REFERENCE;
    JsErrorCode e = MakeId("prototype", &pidProto);
    if (e != JsNoError) return MapJsError(env, e, "define_class: prototype id");
    JsValueRef proto = JS_INVALID_REFERENCE;
    e = JsGetProperty(ctor, pidProto, &proto);
    if (e != JsNoError) return MapJsError(env, e, "define_class: prototype get");
    JsValueType t = JsUndefined;
    if (JsGetValueType(proto, &t) != JsNoError || t != JsObject) {
      nodeapi::SetLastError(env, napi_generic_failure,
                            "define_class: prototype is not an object");
      return napi_generic_failure;
    }
    // `proto` is reachable from `ctor` (held by ctorRoot), so no
    // separate root is needed for the define duration.
    s = napi_define_properties(env, FromJs(proto), protos.size(),
                               protos.data());
    if (s != napi_ok) return s;
  }
  if (!statics.empty()) {
    s = napi_define_properties(env, FromJs(ctor), statics.size(),
                               statics.data());
    if (s != napi_ok) return s;
  }

  s = RootAndPublish(env, ctor, result);
  if (s != napi_ok) return s;
  nodeapi::ClearLastError(env);
  return napi_ok;
}

} /* extern "C" */
