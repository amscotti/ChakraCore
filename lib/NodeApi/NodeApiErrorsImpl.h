//-------------------------------------------------------------------------------------------------------
// Copyright (c) ChakraCore Project Contributors. All rights reserved.
// Licensed under the MIT license. See LICENSE.txt file in the project root for full license information.
//-------------------------------------------------------------------------------------------------------
//
// Errors and throw helpers (plan doc/node-api-plan.md §6 card N2.5, §3.10,
// §4.1 Table A4 + Table B1).
//
// Owns lib/NodeApi/NodeApiErrorsImpl.* (A4 create/throw half + B1 fatal
// interface; full B1 host wiring in N4.1 lands separately). A4 plumbing
// already landed in lib/NodeApi/NodeApiEnv.* (napi_get_last_error_info,
// napi_is_exception_pending, napi_get_and_clear_last_exception, N1.5) is
// NOT reimplemented here.
//
// V8 surface implemented here (verify against include/nodeapi/js_native_api.h
// and include/nodeapi/node_api.h at NAPI_VERSION=8):
//   napi_create_error, napi_create_type_error, napi_create_range_error
//   napi_throw, napi_throw_error, napi_throw_type_error, napi_throw_range_error
//   napi_is_error, napi_fatal_exception, napi_fatal_error
// NodeApiErrorsImpl does NOT implement the v9 syntax-error pair
// (node_api_create_syntax_error / node_api_throw_syntax_error, js_native_api.h
// NAPI_VERSION>=9): adding v9 symbols without the v9 semantics is a
// conformance bug (plan §1.2). No syntax-error symbol is defined here.
//
// Ownership record (memory-management.md §3; plan N2.5 "not required"):
//   Resource: error JsValueRefs created by napi_create_* (returned to the
//     caller rooted in the innermost scope) plus throw-helper intermediates
//     (msg/code strings, error object) rooted in the innermost scope before
//     JsSetException.
//   Domain/allocator: Recycler heap (JsCreateError / JsCreateString); scope
//     roots are provider-native vectors in NodeApiScopes (see
//     NodeApiScopes.h for the scope ownership record).
//   Owner/transfer: napi_create_* transfers the error root to the caller's
//     scope (R1); napi_throw* transfers the error into the recorded exception
//     (held by the runtime's JavascriptExceptionObject after JsSetException).
//     The scope root (throw path) is released at scope close while the
//     exception still holds the object.
//   Invalidation: scope close (scope roots), exception clear
//     (napi_get_and_clear_last_exception), env destroy step 6b (default scope),
//     runtime disposal (R5).
//   Thread/context: owner thread with env context entered (all entries use
//     nodeapi::EntryGuard); no worker-thread use (R3).
//   Release: scope close / get_and_clear / destroy step 6; throw path's
//     scope roots die with the scope, exception hold dies with
//     get_and_clear (JsGetAndClearException) or script unwind.
//   Tests: test/nodeapi/probes/errors_probe.cpp (+ run_errors_probe.sh).
//   Realm caveat for napi_is_error: the prototype-chain fallback uses the
//     current context's global Error.prototype, so it is realm-local. The
//     intrinsic JsError check is realm-independent (TypeIds_Error). An error
//     from another context is still recognised via the intrinsic branch;
//     a plain object inheriting a foreign Error.prototype but not of Error
//     type is reported false (documented, not faked).

#ifndef CHAKRA_NODEAPI_ERRORS_IMPL_H_
#define CHAKRA_NODEAPI_ERRORS_IMPL_H_

#include "js_native_api.h" /* napi_env, napi_value, NAPI_VERSION=8 */
#include "node_api.h" /* napi_fatal_error / napi_fatal_exception (B1) */

#endif /* CHAKRA_NODEAPI_ERRORS_IMPL_H_ */
