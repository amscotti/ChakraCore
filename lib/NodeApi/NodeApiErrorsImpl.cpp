//-------------------------------------------------------------------------------------------------------
// Copyright (c) ChakraCore Project Contributors. All rights reserved.
// Licensed under the MIT license. See LICENSE.txt file in the project root for full license information.
//-------------------------------------------------------------------------------------------------------
//
// Errors and throw helpers (plan §6 card N2.5). See NodeApiErrorsImpl.h for
// the v8 surface and the realm caveat. C++17 (matching Wave-1 provider);
// the extern "C" napi_* surface is language-agnostic.
//
// Vendored oracles (do not edit planning docs; these are the citations):
//   - Declarations: include/nodeapi/js_native_api.h:121-136 (create_*),
//     :381-405 (throw/is_error/pending/clear), node_api.h:92-96
//     (fatal_error), :185-186 (fatal_exception). NAPI_VERSION=8 includes
//     all of these; the v9 syntax pair (:133-136, :392-396) is excluded.
//   - code-property semantics: test/nodeapi/vendor/js-native-api/
//     test_error/test_error.c createErrorCode/throwErrorCode etc. pass a
//     caller-supplied string value ("ERR_TEST_CODE") as `code` and
//     test_error/test.js asserts error.code == 'ERR_TEST_CODE',
//     error.name/message. Neither header nor vendor tests define a
//     status-name code string; the provider sets error.code to the
//     caller-supplied value verbatim (or leaves it unset/undefined when
//     the caller passes NULL). There is no "TestNapiError" mapping in the
//     vendored tree (grep finds no such symbol); the oracle is the
//     test_error pair above.
//   - Pending gates: doc/node-api-status-mapping.md table (C column):
//     create_error/type/range = Yes (execute with pending, msg/code
//     non-string => napi_string_expected); throw/throw_error/type/range
//     = No (gated, napi_pending_exception without executing, original
//     preserved); is_error = Yes (ok+false on wrong type);
//     fatal_exception = No (gated); fatal_error = Yes noreturn.
//     Headers alone are silent on gates (status-mapping §0); the C
//     column is the provider recommendation.
//   - Throw discipline: plan §3.10: throw helpers set pending via
//     JsSetException and return napi_ok when none pending; if pending,
//     return napi_pending_exception and leave it. JsSetException itself
//     (lib/Jsrt/Jsrt.cpp:3249 RecordException) overwrites via
//     SetRecordedException, so the provider gate must not call it when
//     pending (see report "pending-gate surprise").
//   - get_and_clear: already landed in NodeApiEnv.cpp (N1.5). It matches
//     the vendored semantics: JsGetAndClearException + RootIntoInnermost
//     (returns null + napi_ok when none pending). This TU does not
//     reimplement it; the probe verifies the landed behaviour against
//     throw helpers.

#include "NodeApiErrorsImpl.h"

#include "NodeApiEnv.h"
#include "NodeApiHostApi.h"
#include "NodeApiScopes.h"
#include "ChakraCommon.h"
#include "ChakraCore.h"

#include <cassert>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace {

enum class ErrorKind : int { Error = 0, TypeError = 1, RangeError = 2 };

/* JsErrorCode -> napi_status per plan §3.10 table (semantics §6).
 * Records last_error (provider-owned buffer) and returns the mapped status.
 * Callers that already produced a more specific status (string_expected,
 * pending gate) do not go through here. */
napi_status MapJs(napi_env env, JsErrorCode ec, const char* what) {
  if (ec == JsNoError) return napi_ok;
  if (ec == JsErrorInExceptionState || ec == JsErrorScriptException) {
    nodeapi::SetLastError(env, napi_pending_exception, what);
    return napi_pending_exception;
  }
  if (ec == JsErrorInvalidArgument || ec == JsErrorNullArgument) {
    nodeapi::SetLastError(env, napi_invalid_arg, what);
    return napi_invalid_arg;
  }
  if (ec == JsErrorOutOfMemory) {
    nodeapi::SetLastError(env, napi_generic_failure, what);
    return napi_generic_failure;
  }
  if (ec == JsErrorScriptCompile || ec == JsErrorFatal) {
    /* JsErrorScriptCompile maps to pending only for JsRun (plan §3.10);
     * here (create/throw/is_error) no compile occurs, so treat as
     * generic (do not fabricate a JS exception per OOM rule). */
    nodeapi::SetLastError(env, napi_generic_failure, what);
    return napi_generic_failure;
  }
  nodeapi::SetLastError(env, napi_generic_failure, what);
  return napi_generic_failure;
}

JsValueRef AsJs(napi_value v) {
  return reinterpret_cast<JsValueRef>(v);
}

napi_value AsNapi(JsValueRef v) {
  return reinterpret_cast<napi_value>(v);
}

/* Validate a caller-supplied napi_value that must be a string when present.
 * `v == nullptr` (C NULL) means absent => napi_ok (no check). Otherwise
 * JsGetValueType must be JsString; otherwise napi_string_expected with
 * last_error recorded (status-mapping create_* rows: msg non-string
 * => string_expected; code non-string => string_expected for create_error,
 * applied uniformly to all three create_* here). JsGetValueType itself
 * does not run user code and works with a pending exception
 * (BEGIN_JSRT_NO_EXCEPTION). */
napi_status CheckPresentString(napi_env env, napi_value v, const char* what) {
  if (v == nullptr) return napi_ok;
  JsValueType t = JsObject;
  JsErrorCode ec = JsGetValueType(AsJs(v), &t);
  if (ec != JsNoError) return MapJs(env, ec, what);
  if (t != JsString) {
    nodeapi::SetLastError(env, napi_string_expected, what);
    return napi_string_expected;
  }
  return napi_ok;
}

/* Create a JS string from a C string (NULL => ""). Length is strlen
 * (NUL-terminated input, per throw_error signatures). Roots into the
 * innermost scope when root=true (throw-helper intermediates): the value
 * must survive the following JsCreateError / JsSetProperty allocations
 * (GC can run on allocation; a bare JsValueRef local is not a root, R1).
 * Callers pass root=false only when the caller already rooted the value
 * (create_* with caller msg/code, never newly created here). */
napi_status CreateCxxString(napi_env env, const char* s, JsValueRef* out) {
  assert(out != nullptr);
  *out = JS_INVALID_REFERENCE;
  const char* str = (s != nullptr) ? s : "";
  size_t len = std::strlen(str);
  JsValueRef v = JS_INVALID_REFERENCE;
  JsErrorCode ec = JsCreateString(str, len, &v);
  if (ec != JsNoError) return MapJs(env, ec, "JsCreateString");
  *out = v;
  return napi_ok;
}

/* Set error.code = codeVal (JsValueRef already validated string).
 * Creates the "code" property id (interned, non-allocating for GC) and
 * JsSetProperty with useStrictRules=false (matches the stub/host pattern
 * JsSetProperty(global, pid, fn, false)). Fresh error objects have no own
 * code property and no setter, so this runs no user code. */
napi_status SetCodeProperty(napi_env env, JsValueRef errorJs,
                            JsValueRef codeJs) {
  JsPropertyIdRef pid = JS_INVALID_REFERENCE;
  JsErrorCode ec = JsCreatePropertyId("code", 4, &pid);
  if (ec != JsNoError) return MapJs(env, ec, "JsCreatePropertyId(code)");
  ec = JsSetProperty(errorJs, pid, codeJs, false);
  if (ec != JsNoError) return MapJs(env, ec, "JsSetProperty(code)");
  return napi_ok;
}

/* JsCreateError/TypeError/RangeError dispatch on kind. msgJs must be a
 * valid JsValueRef (never JS_INVALID_REFERENCE): callers pass the caller
 * msg (already validated string) or a freshly created "" string (NULL msg
 * case). JsCreateError's SetErrorMessage (Jsrt.cpp:2998) coerces non-string
 * via ToString; the provider has already validated string, so the coercion
 * never runs user code here. */
napi_status CreateErrorObject(napi_env env, ErrorKind kind,
                              JsValueRef msgJs, JsValueRef* errOut) {
  assert(errOut != nullptr);
  *errOut = JS_INVALID_REFERENCE;
  JsValueRef err = JS_INVALID_REFERENCE;
  JsErrorCode ec = JsNoError;
  switch (kind) {
    case ErrorKind::Error: ec = JsCreateError(msgJs, &err); break;
    case ErrorKind::TypeError: ec = JsCreateTypeError(msgJs, &err); break;
    case ErrorKind::RangeError: ec = JsCreateRangeError(msgJs, &err); break;
    default:
      nodeapi::SetLastError(env, napi_generic_failure,
                            "create_error: unknown kind");
      return napi_generic_failure;
  }
  if (ec != JsNoError) return MapJs(env, ec, "JsCreateError");
  *errOut = err;
  return napi_ok;
}

/* Shared create_* body (kind dispatch). Validate-first order (§3.10):
 * result null already checked by caller; here validate code/msg string
 * discipline, then JsCreateError, then code property, then root + publish.
 * EntryGuard was already constructed with allowPending=true (C Yes) so
 * this executes with a pending exception and preserves it: JsCreateError
 * / JsCreateString / JsSetProperty all use verifyRuntimeState=false on
 * this build (JSRT_MAYBE_TRUE false without NTBUILD, JsrtInternal.h:18-25)
 * and do not clear the recorded exception. Probe PCreatePending asserts
 * preservation. */
napi_status CreateErrorCommon(napi_env env, ErrorKind kind, napi_value code,
                              napi_value msg, napi_value* result) {
  assert(result != nullptr);
  napi_status st = CheckPresentString(env, msg, "create_error: msg");
  if (st != napi_ok) return st;
  st = CheckPresentString(env, code, "create_error: code");
  if (st != napi_ok) return st;

  JsValueRef msgJs = JS_INVALID_REFERENCE;
  bool msgOwned = false;
  if (msg != nullptr) {
    msgJs = AsJs(msg); /* caller-rooted; not re-rooted */
  } else {
    /* NULL msg => empty message. Create "" and root it so it survives
     * the error allocation. */
    st = CreateCxxString(env, "", &msgJs);
    if (st != napi_ok) return st;
    msgOwned = true;
    st = NodeApiScopes_RootIntoInnermost(env, AsNapi(msgJs));
    if (st != napi_ok) {
      nodeapi::SetLastError(
          env, napi_generic_failure, "create_error: root msg");
      return napi_generic_failure;
    }
    (void)msgOwned;
  }

  JsValueRef errJs = JS_INVALID_REFERENCE;
  st = CreateErrorObject(env, kind, msgJs, &errJs);
  if (st != napi_ok) return st;

  if (code != nullptr) {
    st = SetCodeProperty(env, errJs, AsJs(code));
    if (st != napi_ok) return st;
  }

  st = NodeApiScopes_RootIntoInnermost(env, AsNapi(errJs));
  if (st != napi_ok) {
    nodeapi::SetLastError(
        env, napi_generic_failure, "create_error: root result");
    return napi_generic_failure;
  }
  *result = AsNapi(errJs);
  nodeapi::ClearLastError(env);
  return napi_ok;
}

/* Shared throw_* body (kind dispatch, C-string code/msg, NULL allowed).
 * EntryGuard was already constructed with allowPending=false, so a pending
 * exception returns napi_pending_exception without executing and without
 * allocating/roots (probe asserts root-count unchanged). This is load-
 * bearing: JsSetException (Jsrt.cpp:3249) calls RecordException which
 * overwrites threadContext->exceptionObject (ThreadContext.h:1570) with
 * no pending check, so calling it when pending would destroy the original.
 * Create order is root-then-create: msg string rooted before error alloc,
 * error rooted before code-string alloc, so no intermediate is GC'd. */
napi_status ThrowErrorCommon(napi_env env, ErrorKind kind, const char* code,
                             const char* msg) {
  JsValueRef msgJs = JS_INVALID_REFERENCE;
  napi_status st = CreateCxxString(env, msg, &msgJs);
  if (st != napi_ok) return st;
  st = NodeApiScopes_RootIntoInnermost(env, AsNapi(msgJs));
  if (st != napi_ok) {
    nodeapi::SetLastError(env, napi_generic_failure, "throw_error: root msg");
    return napi_generic_failure;
  }

  JsValueRef errJs = JS_INVALID_REFERENCE;
  st = CreateErrorObject(env, kind, msgJs, &errJs);
  if (st != napi_ok) return st;
  st = NodeApiScopes_RootIntoInnermost(env, AsNapi(errJs));
  if (st != napi_ok) {
    nodeapi::SetLastError(env, napi_generic_failure, "throw_error: root err");
    return napi_generic_failure;
  }

  if (code != nullptr) {
    JsValueRef codeJs = JS_INVALID_REFERENCE;
    st = CreateCxxString(env, code, &codeJs);
    if (st != napi_ok) return st;
    st = NodeApiScopes_RootIntoInnermost(env, AsNapi(codeJs));
    if (st != napi_ok) {
      nodeapi::SetLastError(env, napi_generic_failure,
                            "throw_error: root code");
      return napi_generic_failure;
    }
    st = SetCodeProperty(env, errJs, codeJs);
    if (st != napi_ok) return st;
  }

  JsErrorCode ec = JsSetException(errJs);
  if (ec == JsNoError) {
    nodeapi::ClearLastError(env);
    return napi_ok;
  }
  if (ec == JsErrorOutOfMemory) {
    nodeapi::SetLastError(env, napi_generic_failure, "JsSetException OOM");
    return napi_generic_failure;
  }
  return MapJs(env, ec, "JsSetException");
}

/* Helper for napi_is_error: true iff JsGetValueType already failed
 * handled by caller. Returns napi_ok + *isErr set, or a mapped failure. */
napi_status IsErrorImpl(napi_env env, napi_value value, bool* isErr) {
  assert(isErr != nullptr);
  *isErr = false;
  JsValueType t = JsObject;
  JsErrorCode ec = JsGetValueType(AsJs(value), &t);
  if (ec != JsNoError) return MapJs(env, ec, "JsGetValueType(is_error)");
  if (t == JsError) {
    /* Intrinsic: TypeIds_Error in JsGetValueType (Jsrt.cpp:1041).
     * Realm-independent: covers direct Error/Type/Range/Syntax/etc. and
     * subclass instances the engine types as Error. */
    *isErr = true;
    nodeapi::ClearLastError(env);
    return napi_ok;
  }
  switch (t) {
    case JsUndefined:
    case JsNull:
    case JsNumber:
    case JsString:
    case JsBoolean:
    case JsSymbol:
      /* Primitives cannot be errors (JS instanceof also false). No
       * JsInstanceOf call, so no user code, no pending mutation. */
      *isErr = false;
      nodeapi::ClearLastError(env);
      return napi_ok;
    case JsObject:
    case JsFunction:
    case JsArray:
    case JsArrayBuffer:
    case JsTypedArray:
    case JsDataView:
      break; /* prototype-chain fallback below */
    default:
      *isErr = false;
      nodeapi::ClearLastError(env);
      return napi_ok;
  }

  /* Prototype-chain fallback (plan N2.5: prototype-chain/intrinsic check):
   * walk the value's prototype chain looking for the current context's
   * Error.prototype via JsGetPrototype (no user code; unlike JsInstanceOf
   * this does not invoke a tampered Symbol.hasInstance on global.Error).
   * Realm caveat (see header): this walk is realm-local. */
  JsValueRef global = JS_INVALID_REFERENCE;
  ec = JsGetGlobalObject(&global);
  if (ec != JsNoError) return MapJs(env, ec, "JsGetGlobalObject(is_error)");
  JsPropertyIdRef errPid = JS_INVALID_REFERENCE;
  ec = JsCreatePropertyId("Error", 5, &errPid);
  if (ec != JsNoError) return MapJs(env, ec, "JsCreatePropertyId(Error)");
  JsValueRef errCtor = JS_INVALID_REFERENCE;
  ec = JsGetProperty(global, errPid, &errCtor);
  if (ec != JsNoError) return MapJs(env, ec, "JsGetProperty(Error)");
  /* global.Error deleted/tampered to non-function: cannot walk; the
   * intrinsic branch above already covered engine-typed errors. Report
   * false rather than failing. */
  JsValueType ctorType = JsObject;
  if (JsGetValueType(errCtor, &ctorType) != JsNoError ||
      ctorType != JsFunction) {
    *isErr = false;
    nodeapi::ClearLastError(env);
    return napi_ok;
  }
  JsPropertyIdRef protoPid = JS_INVALID_REFERENCE;
  ec = JsCreatePropertyId("prototype", 9, &protoPid);
  if (ec != JsNoError) return MapJs(env, ec, "JsCreatePropertyId(prototype)");
  JsValueRef errProto = JS_INVALID_REFERENCE;
  ec = JsGetProperty(errCtor, protoPid, &errProto);
  if (ec != JsNoError) return MapJs(env, ec, "JsGetProperty(prototype)");
  if (errProto == JS_INVALID_REFERENCE) {
    *isErr = false;
    nodeapi::ClearLastError(env);
    return napi_ok;
  }

  JsValueRef cur = AsJs(value);
  for (int i = 0; i < 100; i++) {
    JsValueRef next = JS_INVALID_REFERENCE;
    ec = JsGetPrototype(cur, &next);
    if (ec != JsNoError) return MapJs(env, ec, "JsGetPrototype(is_error)");
    if (next == JS_INVALID_REFERENCE) break;
    if (next == errProto) {
      *isErr = true;
      nodeapi::ClearLastError(env);
      return napi_ok;
    }
    JsValueType nt = JsObject;
    JsErrorCode ec2 = JsGetValueType(next, &nt);
    if (ec2 != JsNoError) {
      return MapJs(env, ec2, "JsGetValueType(proto)");
    }
    if (nt == JsNull || nt == JsUndefined) break; /* end of chain */
    cur = next;
  }
  *isErr = false;
  nodeapi::ClearLastError(env);
  return napi_ok;
}

} /* namespace */

extern "C" {

// --- napi_create_error family (A4, C Yes: execute with pending) ---
// Vendored declarations: js_native_api.h:121-132. Syntax variant (v9) is
// intentionally absent (plan §1.2).

napi_status NAPI_CDECL napi_create_error(napi_env env, napi_value code,
                                          napi_value msg,
                                          napi_value* result) {
  if (result == nullptr) {
    if (env != nullptr) {
      nodeapi::SetLastError(env, napi_invalid_arg,
                            "napi_create_error: result is NULL");
    }
    return napi_invalid_arg;
  }
  nodeapi::EntryGuard guard(env, /*allowClosing=*/false,
                            /*allowPending=*/true, /*needContext=*/true);
  if (!guard.ok()) return guard.status();
  return CreateErrorCommon(env, ErrorKind::Error, code, msg, result);
}

napi_status NAPI_CDECL napi_create_type_error(napi_env env, napi_value code,
                                               napi_value msg,
                                               napi_value* result) {
  if (result == nullptr) {
    if (env != nullptr) {
      nodeapi::SetLastError(env, napi_invalid_arg,
                            "napi_create_type_error: result is NULL");
    }
    return napi_invalid_arg;
  }
  nodeapi::EntryGuard guard(env, /*allowClosing=*/false,
                            /*allowPending=*/true, /*needContext=*/true);
  if (!guard.ok()) return guard.status();
  return CreateErrorCommon(env, ErrorKind::TypeError, code, msg, result);
}

napi_status NAPI_CDECL napi_create_range_error(napi_env env, napi_value code,
                                                napi_value msg,
                                                napi_value* result) {
  if (result == nullptr) {
    if (env != nullptr) {
      nodeapi::SetLastError(env, napi_invalid_arg,
                            "napi_create_range_error: result is NULL");
    }
    return napi_invalid_arg;
  }
  nodeapi::EntryGuard guard(env, /*allowClosing=*/false,
                            /*allowPending=*/true, /*needContext=*/true);
  if (!guard.ok()) return guard.status();
  return CreateErrorCommon(env, ErrorKind::RangeError, code, msg, result);
}

// --- napi_throw family (A4, C No: gated) ---
// Vendored declarations: js_native_api.h:382-391. On success sets pending
// via JsSetException and returns napi_ok; if already pending returns
// napi_pending_exception and preserves the original (plan §3.10,
// status-mapping throw rows). Syntax throw (v9) intentionally absent.

napi_status NAPI_CDECL napi_throw(napi_env env, napi_value error) {
  if (error == nullptr) {
    if (env != nullptr) {
      nodeapi::SetLastError(env, napi_invalid_arg, "napi_throw: error is NULL");
    }
    return napi_invalid_arg;
  }
  nodeapi::EntryGuard guard(env, /*allowClosing=*/false,
                            /*allowPending=*/false, /*needContext=*/true);
  if (!guard.ok()) return guard.status();
  JsErrorCode ec = JsSetException(AsJs(error));
  if (ec == JsNoError) {
    nodeapi::ClearLastError(env);
    return napi_ok;
  }
  if (ec == JsErrorOutOfMemory) {
    nodeapi::SetLastError(env, napi_generic_failure, "JsSetException OOM");
    return napi_generic_failure;
  }
  return MapJs(env, ec, "JsSetException");
}

napi_status NAPI_CDECL napi_throw_error(napi_env env, const char* code,
                                         const char* msg) {
  nodeapi::EntryGuard guard(env, /*allowClosing=*/false,
                            /*allowPending=*/false, /*needContext=*/true);
  if (!guard.ok()) return guard.status();
  return ThrowErrorCommon(env, ErrorKind::Error, code, msg);
}

napi_status NAPI_CDECL napi_throw_type_error(napi_env env, const char* code,
                                              const char* msg) {
  nodeapi::EntryGuard guard(env, /*allowClosing=*/false,
                            /*allowPending=*/false, /*needContext=*/true);
  if (!guard.ok()) return guard.status();
  return ThrowErrorCommon(env, ErrorKind::TypeError, code, msg);
}

napi_status NAPI_CDECL napi_throw_range_error(napi_env env, const char* code,
                                               const char* msg) {
  nodeapi::EntryGuard guard(env, /*allowClosing=*/false,
                            /*allowPending=*/false, /*needContext=*/true);
  if (!guard.ok()) return guard.status();
  return ThrowErrorCommon(env, ErrorKind::RangeError, code, msg);
}

// --- napi_is_error (A4 also A2, C Yes: execute with pending) ---
// Vendored declaration: js_native_api.h:397-399. Wrong type => napi_ok +
// false, never an error (status-mapping is_error row).

napi_status NAPI_CDECL napi_is_error(napi_env env, napi_value value,
                                      bool* result) {
  if (result == nullptr) {
    if (env != nullptr) {
      nodeapi::SetLastError(env, napi_invalid_arg,
                            "napi_is_error: result is NULL");
    }
    return napi_invalid_arg;
  }
  if (value == nullptr) {
    if (env != nullptr) {
      nodeapi::SetLastError(env, napi_invalid_arg,
                            "napi_is_error: value is NULL");
    }
    return napi_invalid_arg;
  }
  nodeapi::EntryGuard guard(env, /*allowClosing=*/false,
                            /*allowPending=*/true, /*needContext=*/true);
  if (!guard.ok()) return guard.status();
  bool isErr = false;
  napi_status st = IsErrorImpl(env, value, &isErr);
  if (st != napi_ok) return st;
  *result = isErr;
  return napi_ok;
}

/* B1 fatal (napi_fatal_exception / napi_fatal_error) moved to
 * lib/NodeApi/NodeApiHost.cpp for N4.1 full host wiring (registration +
 * fatal in one TU per plan §5.2). This TU retains A4 create/throw only;
 * do not re-add fatal here (duplicate-symbol link error). */

} /* extern "C" */
