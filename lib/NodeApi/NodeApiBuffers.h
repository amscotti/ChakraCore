//-------------------------------------------------------------------------------------------------------
// Copyright (c) ChakraCore Project Contributors. All rights reserved.
// Licensed under the MIT license. See LICENSE.txt file in the project root for full license information.
//-------------------------------------------------------------------------------------------------------
//
// Node-API Buffer policy (plan doc/node-api-plan.md task N4.2, B2 group;
// §3.9 PR-6 design; §4 B2 Buffer disposition; doc/node-api-status-mapping.md
// Buffer rows).
//
// Implements the v8 B2 Buffer surface:
//   napi_create_buffer / napi_create_buffer_copy /
//   napi_create_external_buffer / napi_is_buffer / napi_get_buffer_info
//
// Marker design (§3.9 / PR-6):
//   A Buffer is a Uint8Array (JsArrayTypeUint8) view over an ArrayBuffer
//   (GC'd for create/copy, external for create_external) carrying a
//   provider-private hidden Symbol property set to `true`. The Symbol is
//   created per env via JsCreateSymbol (description
//   "chakra-nodeapi-buffer"), never exposed, cached in a provider-owned
//   global map keyed by env generation (see .cpp). The property is
//   defined with {writable:false, enumerable:false, configurable:false}
//   so it is non-enumerable/hidden (not visible via Object.keys,
//   Object.getOwnPropertyNames, etc.). napi_is_buffer is the marker check
//   (own-property + boolean true); napi_get_buffer_info composes the
//   N3.1 typed-array info (napi_get_typedarray_info) after the marker
//   check. A plain Uint8Array created via napi_create_typedarray has no
//   marker and therefore is_buffer == false (probe-verified).
//
// Explicit non-conformances (PR-6, documented in the file header per
// task N4.2; do not edit planning docs):
//   - No JS `Buffer` global is provided. Addons needing the global or
//     require('buffer') are runtime scope (§0 C) and will see no global.
//     This is a recorded non-conformance vs Node.
//   - No Buffer.prototype methods (alloc, from, concat, slice helpers,
//     etc.) are provided. The Buffer is a plain Uint8Array view.
//   - No pooling / sharing of ArrayBuffers. Each create allocates a
//     fresh ArrayBuffer (JsCreateArrayBuffer or
//     JsCreateExternalArrayBuffer). Zero-length buffers allocate an
//     empty ArrayBuffer as well.
//   - Buffers are not instances of a Buffer constructor; instanceof
//     Buffer checks (if addon JS expects Buffer) will fail. This is
//     expected per PR-6.
//   - The marker Symbol is provider-private: it is not added to the
//     global Symbol registry (not Symbol.for), so it cannot be
//     obtained via Symbol.for("..."). JS code enumerating
//     Object.getOwnPropertySymbols could discover it, but the provider
//     treats is_buffer as a marker check only (any object with the
//     marker true is a Buffer); a forged marker on a non-TypedArray is
//     possible in theory but not guarded against (documented residual).
//
// External buffer finalizer (§3.7 WrapRecord/trampoline path, N3.2):
//   napi_create_external_buffer uses JsCreateExternalArrayBuffer with
//   NULL finalizeCallback (storage aliases the caller's `data`), creates
//   the Uint8Array view + marker, then attaches the user's
//   node_api_basic_finalize via napi_add_finalizer on the view (which
//   creates a WrapRecord in NodeApiWrap.* with a before-collect
//   trampoline). Drain points are the same as N3.2: entry to every
//   Buffer napi_* entry (cheap via nodeapi_wrap_drain), host pump via
//   chakra_nodeapi_test_drain_finalizers, and destroy steps 2/6 (via
//   nodeapi_wrap_drain / nodeapi_wrap_teardown). The trampoline only
//   enqueues; user finalizers run on the env thread with a live env
//   (D4). The ArrayBuffer itself carries no finalizer, so detach does
//   not drop the user's finalizer (unlike N3.1's external ArrayBuffer
//   detached-leak residual; see .cpp ownership record). If
//   finalize_cb == NULL, no WrapRecord is created (no finalizer to
//   call; the external storage is still aliased).
//
// Status discipline (doc/node-api-status-mapping.md C column):
//   - Gated (No -> napi_pending_exception without executing):
//     napi_create_buffer, napi_create_buffer_copy,
//     napi_create_external_buffer (all creates).
//   - Executes (Yes -> own status, pending preserved):
//     napi_is_buffer (ok+false on wrong type, never an error),
//     napi_get_buffer_info (invalid_arg if not a Buffer).
//   Validated-first: NULL binding/out args => napi_invalid_arg before
//   the entry guard. Success clears last_error; failure sets it.
//   Out-params are written only on success.

#ifndef CHAKRA_NODEAPI_BUFFERS_H_
#define CHAKRA_NODEAPI_BUFFERS_H_

#include "node_api.h" /* napi_status, napi_value, NAPI_VERSION=8 */

#ifdef __cplusplus
extern "C" {
#endif

/* Destroy cleanup for the per-env marker Symbol state. Primary
 * teardown is the per-env sync cleanup hook installed on first
 * Buffer use (GetOrCreateState via napi_add_env_cleanup_hook, step 3;
 * self-contained, no NodeApiEnv.cpp edit needed). This helper is the
 * idempotent explicit teardown (removes the generation entry and
 * JsRelease's the Symbol root); NodeApiEnv.cpp may call it at step 6
 * alongside nodeapi_wrap_teardown if desired (double-call is a no-op
 * after the hook already ran). No-op for NULL/dead envs (registry
 * lookup by value). The caller/hook must hold the env context
 * (destroy's ContextGuard does). Hidden visibility: not exported from
 * release libChakraNodeApi (the build uses -fvisibility=hidden and
 * this helper carries no NAPI_EXTERN default attribute, so it stays
 * hidden; the N1.6 exports check forbids extras). */
void nodeapi_buffers_teardown(napi_env env);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* CHAKRA_NODEAPI_BUFFERS_H_ */
