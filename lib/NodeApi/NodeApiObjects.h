//-------------------------------------------------------------------------------------------------------
// Copyright (c) ChakraCore Project Contributors. All rights reserved.
// Licensed under the MIT license. See LICENSE.txt file in the project root for full license information.
//-------------------------------------------------------------------------------------------------------
//
// Node-API objects/arrays/properties (plan doc/node-api-plan.md task N2.3,
// A2 object half; normative status rows in doc/node-api-status-mapping.md).
//
// Implements the v8 object surface over JsRT:
//   napi_create_object / napi_create_array / napi_create_array_with_length /
//   napi_get_array_length / napi_get_property / napi_set_property /
//   napi_has_property / napi_delete_property / napi_has_own_property /
//   napi_get_named_property / napi_set_named_property /
//   napi_has_named_property / napi_get_element / napi_set_element /
//   napi_has_element / napi_delete_element / napi_define_properties /
//   napi_get_property_names / napi_get_all_property_names /
//   napi_get_prototype / napi_object_freeze / napi_object_seal /
//   napi_instanceof
//
// The v8 surface has no napi_set_prototype (verified against
// include/nodeapi/js_native_api.h and doc/node-api-inventory.md: only
// napi_get_prototype exists). Prototype mutation for N2.3 callers is
// therefore exposed as the hidden helper NodeApiObjects_SetPrototype
// below (JsSetPrototype, no public symbol invented); probes mutate
// prototypes with direct JsSetPrototype, exactly as a host embedding
// would.
//
// N2.4 coordination (method/getter/setter function values in
// napi_define_properties are created by calling napi_create_function;
// the N2.4 trunk owns that symbol):
//   - Joint stub (plan N2.3 card: "land N2.4's napi_create_function
//     first or stub-jointly"): NodeApiObjects.cpp DEFINES a temporary
//     napi_create_function (plain JsCreateFunction, thunk invokes the
//     napi_callback with NULL info, one process-lifetime record per
//     function) so libChakraNodeApi links while N2.4 is in flight.
//     When NodeApiFunctions.* lands, the stub block is DELETED from
//     NodeApiObjects.cpp and that TU becomes the sole definition.
//     Callbacks created through the stub must not call napi_get_cb_info
//     / napi_get_new_target (absent until N2.4); the objects probe
//     honors that.
//   - Scalars (create_double/string/uint32...) are N2.1's. This TU
//     uses JsRT value creation directly (JsDoubleToNumber etc.) and
//     never calls N2.1's napi_* symbols, so no values-TU link debt.
//
// Ownership record (memory-management.md section 3):
//
//   Resource / creation call / source location:
//     1) Scratch roots: descriptor objects (napi_define_properties),
//        property-names intermediate arrays + extracted elements
//        (napi_get_property_names / napi_get_all_property_names),
//        intrinsic lookups (Object + freeze/seal function), index
//        numbers, property-name strings. All are created and released
//        strictly inside one napi_* call (NodeApiObjects.cpp helpers
//        ScratchRoot / ScratchVector).
//     2) Returned values: created values and get-prototype /
//        get-property / get-element / property-names results, rooted
//        into the innermost handle scope via
//        NodeApiScopes_RootIntoInnermost (R-SC-1). Their owner is the
//        per-env scope stack (NodeApiScopes TU); release is scope
//        close / destroy step 6b, never this TU.
//     3) Cached intrinsic function refs: NONE. The plan's cache idea
//        (rooted per env, released at destroy) cannot be honored
//        without a NodeEnv field (layout owned by N1.1, plan 5.1) or a
//        process-lifetime side table that would leak across env churn.
//        Object.freeze / Object.seal / Object are re-fetched per call
//        (transient ScratchRoot pair, no retention). Semantically
//        identical (the builtins are stable per context); the cost is
//        three extra JsGetProperty calls per freeze/seal.
//   Domain and allocator instance (including backing allocator lifetime):
//     (1) lives in the Recycler heap and is released via JsRelease
//         only (never freed directly). Transient lifetime == the
//         calling napi_* frame.
//     (2) lives in the N1.2 scope stack's provider heap (Scope
//         vectors) + Recycler JsAddRef count; allocator lifetime ==
//         scope lifetime (see NodeApiScopes.h record).
//     (3) n/a (no retained state).
//   Physical allocation base, extent, alignment:
//     One Js object per scratch descriptor / names array; one
//     JsValueRef slot per collected property key. Deleted via the
//     matching JsRelease (never C free / delete).
//   Logical length / borrowed views:
//     Scratch lengths are call-local; consumer borrows (caller-passed
//     napi_value keys/objects) are used only while the call runs and
//     are never retained. The returned array's length is fixed at
//     creation (final element count known after collection).
//   Owner; transfer/publication point; remaining aliases:
//     The caller (this TU) owns (1) for the duration of the call.
//     Returned values (2) transfer to the caller's scope at creation
//     (RootIntoInnermost) and are published via the out-param.
//   Borrow invalidation: GC / detach / growth / mutation / reset / close /
//     disposal:
//     (1) is rooted for its whole lifetime, so GC cannot collect it.
//         Element indices / function names are created fresh each call.
//     (2) is invalidated by scope close and destroy step 6b.
//     Runtime disposal invalidates everything (R5); all returns are
//     released before JsDisposeRuntime by the scope teardown.
//   Potential user-code or reentrancy points:
//     Property get/delete/define, freeze/seal (Object.freeze runs no
//     user getters; proxy define traps CAN run user code), and
//     prototype walks (proxy ownKeys/getPrototypeOf traps) can
//     execute user JS. All scratch roots are taken BEFORE the call
//     that can run user code and the work after it re-reads only
//     rooted state (no borrowed Js pointers are dereferenced across
//     a call that can run JS except values held by ScratchRoot).
//   Thread/context allowed for use, root changes and destruction:
//     Owner thread only, with the env context entered (plan 3.2/3.4
//     R3). EntryGuard enforces both; wrong-thread calls fail with
//     napi_invalid_arg before any root is taken.
//   Release on success, partial failure, exception, OOM, cancel and shutdown:
//     Every ScratchRoot / ScratchVector is RAII, so every failure
//     path (pending gate, type pre-check, JsRT error, OOM in a
//     vector) releases exactly once. A failed call publishes nothing
//     through the out-param (out-param is written only on success,
//     per N1.3 convention). Shutdown: returned values die with the
//     scope stack at destroy step 6b.
//   Memory accounting, capacity/retention bound and exhaustion behavior:
//     Scratch memory is freed per call; there is no retained growth
//     (no caches, no side tables). OOM in scratch vectors maps to
//     napi_generic_failure with last-error recorded and the
//     partially built return value is never published.
//   Tests / commands / measured results / remaining uncertainty:
//     test/nodeapi/probes/objects_probe.cpp via
//     test/nodeapi/probes/run_objects_probe.sh (test + jit).
//     Residuals: descriptor-function fidelity (real thunk) is N2.4's;
//     define-on-frozen was napi_pending_exception (JsRT throws)
//     where Node returns napi_invalid_arg with no pending (V8
//     DontThrow false shape) -- FIXED (MED-2): a failing define on
//     a non-extensible object now clears the JsRT TypeError and
//     returns napi_invalid_arg (the documented S3.10 exception).
//     Freeze-on-proxy traps still map via S3.10 (pending).

#ifndef CHAKRA_NODEAPI_OBJECTS_H_
#define CHAKRA_NODEAPI_OBJECTS_H_

#include "js_native_api.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Hidden prototype setter (no v8 napi_set_prototype exists).
 * Honors the same discipline as napi_get_prototype: ToObject on the
 * object, any prototype value (object or null), JsSetPrototype.
 * Not a napi_* symbol and therefore never listed in the exports
 * inventory; later lanes (e.g. class/wrap prototype surgery) call
 * this instead of inventing a public symbol. Hidden visibility in
 * the provider dylib (the build uses -fvisibility=hidden and only
 * napi_* + chakra_nodeapi_* symbols are exported). */
napi_status NAPI_CDECL NodeApiObjects_SetPrototype(napi_env env,
                                                   napi_value object,
                                                   napi_value prototype);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* CHAKRA_NODEAPI_OBJECTS_H_ */
