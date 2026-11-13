//-------------------------------------------------------------------------------------------------------
// Copyright (c) ChakraCore Project Contributors. All rights reserved.
// Licensed under the MIT license. See LICENSE.txt file in the project root for full license information.
//-------------------------------------------------------------------------------------------------------
//
// TEST-ONLY shim: implements the reconciled N1.1 interface
// (lib/NodeApi/NodeApiScopes.h: nodeapi::CheckAlive / CheckAliveAllowClosing /
// SetLastError + NodeApiEnv_ScopeState / IsAlive) over a probe env struct,
// plus the private host API entry points (create/destroy) and two minimal
// value creators (create_object / create_double, each rooted via
// NodeApiScopes_RootIntoInnermost exactly as the N2.1 lane will do).
//
// NEVER shipped in the provider: N1.1 (lib/NodeApi/NodeApiEnv.*) has landed
// and its real implementations are used in the provider library and the
// refs probe. This shim exists so the scopes probe can run self-contained
// (Scopes TU + shim, no NodeApiEnv.cpp linked). It mirrors the N1.1 guard
// semantics in miniature: live-set by pointer VALUE (no fabricated-env
// dereference, R-ENV-10), owner-thread check with debug assert + release
// napi_invalid_arg (HIGH-3 / R-ENV-11), Alive vs Closing/Dead state.
// When the scopes probe migrates to the real env
// (chakra_nodeapi_create_env), the shim accessors + guards below are
// deleted; the real NodeApiEnv.cpp implementations take over.
//
// Build: compiled with -DCHAKRA_NODEAPI_TEST_HOOKS together with
// lib/NodeApi/NodeApiScopes.cpp (see tools/nodeapi/run_probe.sh).

#ifndef TEST_SCOPES_SHIM_H_
#define TEST_SCOPES_SHIM_H_

#include "js_native_api.h"
#include "ChakraCommon.h"

/* Opaque probe env. Layout is private to this shim (the "real" NodeEnv
 * layout is N1.1's). The only contract points are the NodeApiEnv_*
 * accessors and the host-API entry points below. */
struct ScopesProbeEnv;

/* Private host API (§3.2 shape), test version. Records runtime/context
 * (host-owned), owner thread, creates scope state (default scope, R-SC-7),
 * enters the context, and sets Alive=true. Returns nullptr on invalid args. */
napi_env ScopesProbe_CreateEnv(JsRuntimeHandle runtime, JsContextRef context);

/* Destroy per §3.6 step 6 (scope teardown) + step 7 (free): closes every
 * scope (releasing all roots, incl. a possibly-nonempty default scope)
 * BEFORE restoring the previous context, then frees the env struct. */
void ScopesProbe_DestroyEnv(napi_env env);

#ifdef __cplusplus
extern "C" {
#endif

/* Minimal value creators standing in for N2.1 (create -> root, R-SC-1).
 * JsCreateDouble/JsCreateObject failures map per plan §3.10 (conservative
 * subset: NoError ok; else generic failure; NULL out-param invalid_arg).
 * Both root into the innermost scope via NodeApiScopes_RootIntoInnermost. */
napi_status NAPI_CDECL napi_create_object(napi_env env, napi_value* result);
napi_status NAPI_CDECL napi_create_double(napi_env env, double value,
                                          napi_value* result);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* TEST_SCOPES_SHIM_H_ */
