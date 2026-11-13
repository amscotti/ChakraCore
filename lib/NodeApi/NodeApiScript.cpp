//-------------------------------------------------------------------------------------------------------
// Copyright (c) ChakraCore Project Contributors. All rights reserved.
// Licensed under the MIT license. See LICENSE.txt file in the project root for full license information.
//-------------------------------------------------------------------------------------------------------
//
// Node-API script execution + external memory (N3.5). See
// NodeApiScript.h for the contract, audit-#6 note, and ownership
// record.
//
// Implementation notes (Node v22.22.3 doc/api/n-api.md +
// src/js_native_api_v8.cc via doc/node-api-status-mapping.md):
//   - napi_run_script: No (gated: napi_pending_exception without
//     running when already pending). Non-string script =>
//     napi_string_expected (impl explicit IsString check; no
//     coercion). Compile/runtime throw => napi_pending_exception with
//     the exception left pending (try-catch equivalent). Single JsRun
//     (ChakraCore.h:733) with provider sourceUrl "nodeapi" and a
//     fixed NAPI-tagged source-context cookie (not
//     JS_SOURCE_CONTEXT_NONE, which the host uses for drivers).
//     The script sees globals (global-scope evaluation, not module
//     scope, per Node docs) and returns the completion value.
//   - napi_adjust_external_memory: Yes (executes with pending,
//     basic_env). Accounting-only (audit #6): never touches
//     JsSetRuntimeMemoryLimit, never calls JsCollectGarbage.
//     NULL adjusted_value => napi_invalid_arg with no mutation.
//     Allows Closing (teardown-compatible for external finalizers).
//   - napi_get_version / napi_get_node_version: NOT here (landed in
//     NodeApiEnv.cpp Wave 1; verified shapes in NodeApiScript.h).

#include "NodeApiScript.h"

#include "NodeApiEnv.h"
#include "NodeApiHostApi.h"
#include "NodeApiScopes.h"
#include "ChakraCommon.h"
#include "ChakraCore.h"

#include <cstdint>
#include <new>

namespace {

/* Per-env state (opaque to NodeApiEnv.h, stored as
 * NodeEnv::script_state). Completes the forward-declared global
 * ::NodeApiScriptState (defined at global scope, NOT inside this
 * anonymous namespace, so the extern "C" API signatures name the
 * canonical type). */
struct ScriptState {
  int64_t external_total = 0;
};

/* Provider sourceUrl (fixed) and cookie (fixed, NAPI-tagged, distinct
 * from JS_SOURCE_CONTEXT_NONE == -1 used by host drivers). Reusing one
 * SourceContextInfo is intentional: same URL, fresh source text per
 * run (RunScriptCore creates the info once per cookie). */
constexpr const char* kSourceUrl = "nodeapi";
constexpr JsSourceContext kSourceCookie =
    static_cast<JsSourceContext>(0x4E415049); /* "NAPI" */

napi_status MapRun(napi_env env, JsErrorCode ec, const char* what) {
  if (ec == JsNoError) return napi_ok;
  if (ec == JsErrorInExceptionState || ec == JsErrorScriptException ||
      ec == JsErrorScriptCompile) {
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

} /* namespace */

extern "C" {

NodeApiScriptState* NodeApiScript_CreateState(void) {
  return reinterpret_cast<NodeApiScriptState*>(new ScriptState());
}

void NodeApiScript_DestroyForEnv(napi_env env) {
  (void)env;
  /* No roots to release (native total only). Teardown hook for
   * symmetry/explicit destroy order; the struct is freed by
   * NodeApiScript_FreeState immediately after. */
}

void NodeApiScript_FreeState(NodeApiScriptState* state) {
  if (state == nullptr) return;
  delete reinterpret_cast<ScriptState*>(state);
}

int64_t NodeApiScript_ExternalTotal(napi_env env) {
  if (env == nullptr) return 0;
  NodeApiScriptState* raw = NodeApiEnv_ScriptState(env);
  if (raw == nullptr) return 0;
  return reinterpret_cast<ScriptState*>(raw)->external_total;
}

napi_status NAPI_CDECL napi_run_script(napi_env env, napi_value script,
                                        napi_value* result) {
  if (script == nullptr || result == nullptr) {
    if (env != nullptr) {
      nodeapi::SetLastError(env, napi_invalid_arg,
                            "napi_run_script: null arg");
    }
    return napi_invalid_arg;
  }
  nodeapi::EntryGuard guard(env, /*allowClosing=*/false,
                            /*allowPending=*/false, /*needContext=*/true);
  if (!guard.ok()) return guard.status();
  JsValueRef jsv = reinterpret_cast<JsValueRef>(script);
  JsValueType t = JsUndefined;
  JsErrorCode ec = JsGetValueType(jsv, &t);
  if (ec != JsNoError) {
    if (ec == JsErrorInExceptionState || ec == JsErrorScriptException ||
        ec == JsErrorScriptCompile) {
      nodeapi::SetLastError(env, napi_pending_exception,
                            "napi_run_script: type query");
      return napi_pending_exception;
    }
    if (ec == JsErrorInvalidArgument || ec == JsErrorNullArgument) {
      nodeapi::SetLastError(env, napi_invalid_arg,
                            "napi_run_script: type query");
      return napi_invalid_arg;
    }
    nodeapi::SetLastError(env, napi_generic_failure,
                          "napi_run_script: type query");
    return napi_generic_failure;
  }
  if (t != JsString) {
    nodeapi::SetLastError(env, napi_string_expected,
                          "napi_run_script: script is not a string");
    return napi_string_expected;
  }
  JsValueRef url = JS_INVALID_REFERENCE;
  ec = JsCreateString(kSourceUrl, 7, &url);
  if (ec != JsNoError) {
    return MapRun(env, ec, "napi_run_script: JsCreateString (sourceUrl)");
  }
  JsValueRef run_result = JS_INVALID_REFERENCE;
  ec = JsRun(jsv, kSourceCookie, url, JsParseScriptAttributeNone,
             &run_result);
  (void)JsRelease(url, nullptr);
  if (ec != JsNoError) {
    /* Exception stays pending (never cleared implicitly). *result is
     * left untouched (nothing published). */
    return MapRun(env, ec, "napi_run_script: JsRun");
  }
  napi_value nv = reinterpret_cast<napi_value>(run_result);
  napi_status rs = NodeApiScopes_RootIntoInnermost(env, nv);
  if (rs != napi_ok) return rs;
  *result = nv;
  nodeapi::ClearLastError(env);
  return napi_ok;
}

napi_status NAPI_CDECL napi_adjust_external_memory(
    node_api_basic_env env_raw, int64_t change_in_bytes,
    int64_t* adjusted_value) {
  napi_env env = reinterpret_cast<napi_env>(env_raw);
  if (adjusted_value == nullptr) {
    if (env != nullptr) {
      nodeapi::SetLastError(env, napi_invalid_arg,
                            "napi_adjust_external_memory: adjusted_value is NULL");
    }
    return napi_invalid_arg;
  }
  /* Basic_env, native-only (no Js* call): allow Closing so
   * external finalizers during teardown can release accounting.
   * Executes with pending (C-Yes). */
  nodeapi::EntryGuard guard(env, /*allowClosing=*/true,
                            /*allowPending=*/true, /*needContext=*/false);
  if (!guard.ok()) return guard.status();
  NodeApiScriptState* raw = NodeApiEnv_ScriptState(env);
  if (raw == nullptr) {
    nodeapi::SetLastError(env, napi_invalid_arg,
                          "napi_adjust_external_memory: no script state");
    return napi_invalid_arg;
  }
  auto* st = reinterpret_cast<ScriptState*>(raw);
  st->external_total += change_in_bytes;
  *adjusted_value = st->external_total;
  nodeapi::ClearLastError(env);
  return napi_ok;
}

} /* extern "C" */
