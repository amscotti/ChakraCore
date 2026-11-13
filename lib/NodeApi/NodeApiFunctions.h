//-------------------------------------------------------------------------------------------------------
// Copyright (c) ChakraCore Project Contributors. All rights reserved.
// Licensed under the MIT license. See LICENSE.txt file in the project root for full license information.
//-------------------------------------------------------------------------------------------------------
//
// Node-API functions/callbacks (plan doc/node-api-plan.md task N2.4, A2
// function half; normative §3.8 thunk, doc/node-api-semantics.md §5,
// status rows in doc/node-api-status-mapping.md C column).
//
// Implements the v8 function surface:
//   napi_create_function / napi_get_cb_info / napi_get_new_target /
//   napi_call_function / napi_new_instance / napi_define_class
//
// This is the SOLE provider definition of napi_create_function. N2.3's
// joint stub (plain JsCreateFunction in NodeApiObjects.cpp) was deleted
// when this TU landed; N2.3's napi_define_properties now calls this
// TU's napi_create_function for method/getter/setter values.
//
// Deviation from plan §3.8 (function `length`, verified 2026-11-12):
// The plan §3.8 + N2.4 card treat napi_create_function's `length`
// parameter as the function arity (`Function.length`). Upstream Node
// v22.22.3 proves it is the utf8name length:
//   - doc/api/n-api.md napi_create_function: "length: The length of
//     the utf8name in bytes, or NAPI_AUTO_LENGTH if it is
//     null-terminated" (same for napi_define_class).
//   - src/js_native_api_v8.cc napi_create_function: uses `length`
//     only to build the name string (CHECK_NEW_FROM_UTF8_LEN) and
//     creates the function via Function::New(ctx, Invoke, cbdata)
//     with no length argument, so V8 defaults `Function.length` to 0.
//   - vendored test_function/test_function.c: "Name_extra",5 =>
//     `fn.name === 'Name_'` (truncation), with no assertion on
//     `Function.length`.
// The provider therefore does NOT define `Function.length` to the
// name-length value. It defines `Function.length` to 0 (Node truth)
// via a real descriptor object (writable:false, enumerable:false,
// configurable:true, per JS semantics) after creation, so the value
// is engine-independent. The functions probe tests name truncation
// for lengths 0..5 plus `Length===0`, not arity 0..5.
//
// Ownership record (memory-management.md §3; plan §3.7/§3.8):
//
//   Resource / creation call / source location:
//     1) ThunkRecord ({env, napi_callback cb, void* data}), one per
//        JS function created by napi_create_function /
//        napi_define_class (ctor or method/getter/setter) in
//        NodeApiFunctions.cpp CreateFunctionInternal.
//     2) External anchor object (JsCreateExternalObject) holding
//        (1), attached to the function as a provider-private hidden
//        symbol property (non-enumerable, Symbol-keyed). The anchor
//        keeps (1) alive exactly as long as the function object.
//     3) CallbackRecord / CallbackInfo (per-invocation, {thisArg,
//        argv, argc, newTarget, callee, data}), stack-allocated in
//        the thunk frame and pushed on the per-env callback stack
//        for the duration of the user callback only.
//
//   Domain and allocator instance (including backing allocator lifetime):
//     (1) Provider native heap (new/delete via the C++ allocator
//         bound to the provider; backing lifetime == process).
//     (2) Recycler heap (JsCreateExternalObject); released via GC
//         finalization only (never freed directly).
//     (3) Stack (one CallbackInfo per in-flight invocation), plus
//         provider-heap vector slots in the per-env callback-stack
//         side table. The side-table vectors are provider native
//         heap, bounded by nesting depth (see below).
//
//   Physical allocation base, extent, alignment:
//     One heap allocation per (1); one Js external object per (2);
//     one stack record per in-flight callback (3).
//
//   Logical length / borrowed views:
//     (3) argv borrows the engine's arguments array (arguments+1)
//         for the call duration only; the stored JsValueRefs are
//         additionally rooted into the thunk's implicit handle
//         scope (R1) so provider storage never holds an unrooted
//         JS value. The argv pointer itself is never retained
//         beyond the callback return.
//
//   Owner; transfer/publication point; remaining aliases:
//     (1) Owned by (2): created by CreateFunctionInternal,
//         transferred to the external object at
//         JsCreateExternalObject; released only by the external
//         finalizer trampoline. The function (published via the
//         out-param, rooted in the caller's scope) and the anchor
//         are distinct JS values; the anchor holds no scope root
//         (its lifetime is via the hidden property edge function ->
//         anchor, GC-tracked).
//     (3) Owned by the thunk frame (stack); published to the user
//         callback as the opaque napi_callback_info for the call
//         duration; never published beyond the return.
//
//   Borrow invalidation: GC / detach / growth / mutation / reset / close /
//     disposal:
//     (1) dies when the function (hence the anchor) becomes
//         unreachable and GC runs finalization. (2) dies with (1).
//         Env destroy does NOT free live (1): functions still
//         reachable from JS keep their records; after Destroy the
//         anchor finalizer only does `delete` (no env dereference),
//         so post-destroy collection is safe but the callback
//         itself becomes unusable (env dead; thunk returns
//         undefined without invoking user code beyond validation).
//     (3) dies at callback return (pop + implicit-scope close
//         releases the argv roots).
//
//   Potential user-code or reentrancy points:
//     User callback execution; property defines on fresh function
//     objects (no proxy traps; fresh objects have no setters);
//     JsCallFunction / JsConstructObject (callee may run JS which
//     may re-enter the provider). Thunk argv roots are taken
//     BEFORE the user callback, and post-callback state is
//     re-read only from rooted roots (no borrowed Js pointers
//     are dereferenced across a user-code call except values
//     held by the implicit scope).
//
//   Thread/context allowed for use, root changes and destruction:
//     Owner thread only, env context entered (plan §3.2/R3).
//     EntryGuard enforces both for the six public entries.
//     The thunk validates via nodeapi::CheckAlive + ContextGuard
//     (owner-thread JS invocation); wrong-thread validation fails
//     without entering a context. The external finalizer runs on
//     an arbitrary GC thread with no context and touches only
//     native memory (`delete`), never provider containers.
//
//   Release on success, partial failure, exception, OOM, cancel and shutdown:
//     (1) exactly once via the external finalizer (GC path). On
//         creation failure before the transfer point (before
//         JsCreateExternalObject), the record is deleted inline
//         and the unpublished function is discarded (its
//         callbackState dangles but is unreachable, hence never
//         invoked). After the transfer point, failure lets GC
//         free the record via the finalizer (no inline delete,
//         no double-free). Shutdown: records for functions still
//         live at destroy die with the runtime (roots released
//         before JsDisposeRuntime, R5); no extra env-destroy
//         sweep of records is needed because the finalizer needs
//         no env.
//     (3) pop + scope close on every thunk return path (success,
//         pending, C++ exception, imbalance, OOM).
//
//   Memory accounting, capacity/retention bound and exhaustion behavior:
//     No provider-side retention bound beyond one record per live
//     function (dies with the function). The per-env callback
//     stack vector holds at most nesting-depth entries (bounded
//     by JS call depth); OOM in push/rooting maps to
//     napi_generic_failure (public entries) or undefined return
//     (thunk) with last_error recorded, nothing published.
//     Root-counter contribution: the function itself contributes
//     one scope slot in the caller's scope (like every other
//     value); the anchor contributes none (property edge, not a
//     scope slot); in-flight argv roots contribute argc+2 slots
//     in the implicit scope for the call duration only (released
//     at implicit close, so baseline is unchanged outside
//     callbacks).
//
//   Tests / commands / measured results / remaining uncertainty:
//     test/nodeapi/probes/functions_probe.cpp via
//     test/nodeapi/probes/run_functions_probe.sh (test + jit),
//     plus re-run of run_objects_probe.sh (stub callbacks now
//     exercise this thunk; info-oblivious descriptor callbacks
//     still pass, see the objects probe report note).
//     Residuals: the external finalizer's GC-thread guarantee
//     (delete-only, no JS) is Probed by a collect-churn case
//     (functions dropped + GC, no crash/leak growth); exactly-once
//     is by construction (one external per function, one
//     finalizer invocation per external). Linux x64 not claimed
//     (Mac only, N5.5 receipt pass).
//
// Language: C++17 (GNU++17 via `zig c++`, matching Wave 1/2) behind
// the extern "C" napi_* surface (PR-4 decision record: C++ through
// provider v1). No C++ exception crosses the extern "C" surface:
// every provider allocation is nothrow/try-catch wrapped and the
// user-callback invocation is guarded (catch (...) =>
// napi_generic_failure recorded, pending preserved).

#ifndef CHAKRA_NODEAPI_FUNCTIONS_H_
#define CHAKRA_NODEAPI_FUNCTIONS_H_

#include "js_native_api.h" /* napi_status, napi_value, NAPI_VERSION=8 */

#endif /* CHAKRA_NODEAPI_FUNCTIONS_H_ */
