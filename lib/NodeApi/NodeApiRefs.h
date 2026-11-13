//-------------------------------------------------------------------------------------------------------
// Copyright (c) ChakraCore Project Contributors. All rights reserved.
// Licensed under the MIT license. See LICENSE.txt file in the project root for full license information.
//-------------------------------------------------------------------------------------------------------
//
// Node-API references (plan doc/node-api-plan.md task N1.3, semantics
// doc/node-api-semantics.md section 3, R-REF-1..R-REF-14).
//
// Implements the v8 A3 reference surface:
//   napi_create_reference / napi_delete_reference / napi_reference_ref /
//   napi_reference_unref / napi_get_reference_value
//
// Language: C++17 (gnu++17), matching the N0.3 test host and Wave-1
// provider (lib/NodeApi/NodeApiEnv.cpp, NodeApiScopes.cpp,
// NodeApiLifetime.cpp) compiled with `zig c++`. The plan D5 default is
// Zig 0.17 (PR-4); Wave 1 links against the C++ host today, so the
// refs TU is C++ behind the extern "C" surface. The C surface is
// language-agnostic: a later Zig port moves the TU behind the same
// signatures without changing hook names or counters.
//
// N1.1 reconciliation (reconciled, not assumed):
//   The interrupted stand-in used `NodeApiRefsEnv` as a
//   prefix-compatible stand-in for the plan section 3.3 NodeEnv
//   ({runtime, context, owner_thread, state} first). The real N1.1
//   NodeEnv (lib/NodeApi/NodeApiEnv.h) has since landed as a namespace
//   struct `nodeapi::NodeEnv` with that prefix plus later fields
//   (module_api_version, module_filename, services, instance data,
//   hook lists, scope_state, refs_state, last_error). The stand-in
//   struct is removed. Refs state now follows the exact pattern
//   N1.2 (NodeApiScopes) already established: an opaque
//   `NodeApiRefsState` heap object stored in `NodeEnv::refs_state`,
//   created by `NodeApiRefs_CreateState()` inside
//   `chakra_nodeapi_create_env`, destroyed by
//   `NodeApiRefs_DestroyForEnv()` (inside `nodeapi_refs_teardown_all`)
//   + `NodeApiRefs_FreeState()` at destroy step 6a
//   (`nodeapi::TeardownRefs` in NodeApiEnv.cpp). The napi_* functions
//   below never cast `napi_env` directly; they validate with
//   `nodeapi::CheckAlive` / enter with `nodeapi::ContextGuard` and
//   fetch the state with `NodeApiEnv_RefsState` (defined in
//   NodeApiEnv.cpp via the provider-global live registry, so a
//   dead/fabricated env value is never dereferenced, R-ENV-10/11).
//
// V8 type restriction (plan section 1.2; semantics R-REF-3; status
// mapping doc/node-api-status-mapping.md create_reference row):
// v8 allows only object/function/symbol/external; v10's all-types rule
// MUST NOT leak in. The vendored js_native_api.h header docs for
// napi_create_reference state only the weak/strong refcount rule and do
// not repeat the allowlist (include/nodeapi/js_native_api.h:330), so
// the allowlist is encoded from plan 1.2 and the upstream
// implementation check (`IsObject||IsFunction||IsSymbol`, external
// counts as object, per status-mapping.md). Disallowed types return
// napi_invalid_arg with no partial root (P3-7). BigInt discrimination
// is N2.6 (PR-9): JsGetValueType reports BigInt as JsObject today, so
// a BigInt referent is currently treated as an object; see
// GAP-REF-BIGINT in the .cpp. Documented, not faked.
//
// Header-anchored II / error choices (js_native_api.h:342-364; verify
// wording there):
//   napi_reference_ref on a collected weak referent is an error
//     (header: "Calling this when the refcount is 0 and the object is
//     unavailable results in an error") -> napi_invalid_arg.
//   napi_reference_unref at count 0 is an error (header: "Calling
//     this when the refcount is already 0 results in an error")
//     -> napi_invalid_arg. Both override semantics AMB-REF-1's
//     provisional napi_ok choice: header docs are the oracle for that
//     N0.6 decided the functions error; exact code naming is a
//     provider choice (see status-mapping.md AMB-REF-1: upstream C
//     uses generic_failure for unref and ok for ref-collected; this
//     provider encodes invalid_arg for both per the N1.3 task brief).
//   napi_get_reference_value on a collected weak referent succeeds
//     with NULL (header: "the call is still successful but the result
//     is NULL") -> napi_ok + *result = nullptr (AMB-REF-3 resolved).
//   Pending-exception discipline: napi_delete_reference is the
//     documented allowlist lane (upstream docs list it; semantics
//     section 6.4, plan 3.5); the other four functions are gated
//     (return napi_pending_exception without executing, exception
//     preserved). This is the semantics-section-6 provisional gate
//     (B-list: only delete is callable-with-pending among the five).
//     Per the task brief R-REF-1..14 are authoritative; the upstream
//     C implementation executes all five with pending (status-mapping
//     "Yes" column), which is N1.5's follow-up decision once B/C are
//     vendored -- until then N1.3 keeps the B-list gate and the probe
//     asserts it (P5-3 subset). Do not silently flip to C here.
//
// Ownership record (memory-management.md section 3):
//
//   Resource / creation call / source location:
//     1) NodeApiRefsState (the refs_state heap object, one per env).
//        NodeApiRefs.cpp NodeApiRefs_CreateState, called from
//        chakra_nodeapi_create_env (NodeApiEnv.cpp).
//     2) napi_ref (NodeApiRefRecord, one per napi_create_reference).
//        lib/NodeApi/NodeApiRefs.cpp napi_create_reference.
//     3) Strong root: single JsAddRef on the referent, held while the
//        provider count >= 1 (plan 3.4 R2; semantics R-REF-1/5).
//     4) Weak handle: single JsWeakRef + R6 JsAddRef on the handle,
//        held while the provider count == 0 (semantics R-REF-2).
//     5) Scope root: one JsAddRef per napi_get_reference_value success,
//        stored in the N1.2 scope stack via
//        NodeApiScopes_RootIntoInnermost (HIGH-4 unification, R-REF-7 /
//        R-SC-1). Owned by the per-env scope stack (NodeApiScopes TU),
//        released by scope close (or destroy step 6b for the default
//        scope), never by the refs TU. The refs TU keeps its own
//        strong/weak roots separate (3/4 above); ref-get scope roots
//        are counted in the R-REF-14 total's scope part, not here.
//   Domain and allocator instance (including backing allocator lifetime):
//     (1), (2) live in provider native memory (C++ new/delete). They
//     are never Recycler memory (plan 3.3) and never Zig-freed. (3), (4)
//     live in the Recycler heap and are released only via JsRelease
//     (never freed directly). (5) lives in the N1.2 scope stack's
//     provider heap (Scope vectors) + Recycler JsAddRef count; its
//     allocator lifetime == scope lifetime (see NodeApiScopes.h record).
//     Destroy order (plan 3.6): refs step 6a (this TU) before scopes
//     step 6b (N1.2), both before runtime disposal (R5).
//   Physical allocation base, extent, alignment:
//     One heap allocation per (1) and per (2); deleted via the matching
//     native delete (never C free). Js handles are opaque pointers.
//   Logical length / borrowed views:
//     Provider counts are authoritative; JsAddRef return counts are
//     never used as logic (plan 3.4 R2 tagging caveat). The referent
//     JsValueRef is held only while strong; weak state holds only the
//     weak handle, never a raw referent pointer. The JsValueRef
//     borrowed by napi_get_reference_value is re-rooted into the
//     innermost N1.2 scope before return (R-SC-1, HIGH-4); callers
//     keep no borrow across scope close/teardown (see NodeApiScopes).
//   Owner; transfer/publication point; remaining aliases:
//     The provider (this TU) owns all five resources. The napi_ref
//     handle is published to the addon at create (out-param). Deleted
//     records are retained (dead flag, root released) until teardown
//     so double-delete / use-after-delete is a clean napi_invalid_arg,
//     not a use-after-free; the structs are freed once at teardown.
//     The host never owns runtime/context (hands them to N1.1 env at
//     create; provider never disposes them).
//   Borrow invalidation: GC / detach / growth / mutation / reset / close /
//     disposal:
//     (3) is invalidated by the 1->0 transition, delete, or teardown
//     (destroy step 6a). (4) is invalidated by the 0->1 transition,
//     delete, or teardown. The referent itself is invalidated by GC
//     only while weak (then get_reference_value returns ok + null).
//     Strong referents survive GC. Scope roots (5) are invalidated by
//     N1.2 scope close (per-scope) and destroy step 6b (whole stack).
//     Runtime disposal invalidates everything; all roots are released
//     before JsDisposeRuntime (R5).
//   Potential user-code or reentrancy points:
//     No JS execution, no user callbacks, no provider-container mutation
//     inside GC (D4). JsCreateWeakReference/JsAddRef/JsRelease do not
//     run JS. Transitions never invoke addon code, so no reentrancy.
//     napi_delete_reference is allowlisted with pending (above) but
//     still runs no JS.
//   Thread/context allowed for use, root changes and destruction:
//     Owner thread only, with the env context entered (plan 3.2/3.4
//     R3). Wrong-thread use asserts in debug and returns
//     napi_invalid_arg in release (semantics R-ENV-11) via
//     nodeapi::CheckAlive. No napi_env/napi_value/napi_ref use from
//     worker threads. Teardown (DestroyForEnv / nodeapi_refs_teardown_all)
//     runs inside the destroy's ContextGuard (env context current);
//     the standalone nodeapi_refs_teardown_all enters the context
//     itself via nodeapi::ContextGuard.
//   Release on success, partial failure, exception, OOM, cancel and shutdown:
//     Success: each ref-record root has exactly one release point
//     (1->0 / 0->1 / delete / teardown step 6a); each scope root has
//     exactly one release point in the scopes TU (scope close / step
//     6b). Partial failure (JsRT error mid-transition) rolls back to
//     the pre-call state: no half-published ref, no leaked root.
//     Foreign errors map per plan 3.10. Shutdown: nodeapi_refs_teardown_all
//     runs at destroy step 6a (all ref-record roots released, records
//     freed) before the env memory itself is freed (step 7); scope
//     roots are released at step 6b by the scopes TU. After refs
//     teardown every ref handle from this env is invalid
//     (unknown-handle lookup yields napi_invalid_arg while the state
//     lives; after the env is freed the pointer is host use-after-free,
//     same rule as a freed env, R-ENV-10).
//   Memory accounting, capacity/retention bound and exhaustion behavior:
//     This TU's ref-record contribution is live strong roots (1 per
//     strong ref) + live weak handles (1 per weak ref, the R6 handle
//     root); dead refs contribute 0. The normative R-REF-14 total
//     (scope roots + this contribution) is chakra_nodeapi_test_root_count
//     in NodeApiScopes.h (scope part) + NodeApiRefs_RefRootCount below
//     (this TU's part). Probes assert against the total. OOM during
//     create returns napi_generic_failure with last-error recorded,
//     nothing published. No unbounded native growth beyond one struct
//     per live-or-dead ref until teardown. GC roots always return to
//     baseline.
//   Tests / commands / measured results / remaining uncertainty:
//     test/nodeapi/probes/refs_probe.cpp (see run_refs_probe.sh).
//     Remaining uncertainty: BigInt allowlist (GAP-REF-BIGINT, N2.6);
//     exact pending-allowlist membership for the four gated functions
//     pending N0.6 confirmation (delete is the documented allowlist
//     lane, semantics section 6.4); silent arena non-root (AMB-REF-2)
//     observable only by GC churn, covered by the 10x GC-stress arm.

#ifndef CHAKRA_NODEAPI_REFS_H_
#define CHAKRA_NODEAPI_REFS_H_

#include "js_native_api.h" /* napi_env, napi_value, napi_ref, NAPI_VERSION=8 */
#include "ChakraCommon.h"  /* JsRuntimeHandle, JsContextRef */

#include <cstddef>

#ifdef __cplusplus
extern "C" {
#endif

/* ------------------------------------------------------------------ */
/* Opaque refs state (defined in NodeApiRefs.cpp). Stored in the      */
/* real NodeEnv as NodeEnv::refs_state (additive field, N1.1 A1).     */
/* N1.1/N1.2 do not need to know its layout.                          */
/* ------------------------------------------------------------------ */
struct NodeApiRefsState;

/* Creates fresh (empty) refs state. Provider-owned; see
 * NodeApiRefs_FreeState. Never returns null on success (aborts via
 * bad_alloc only if the provider cannot allocate env state at all;
 * chakra_nodeapi_create_env treats that as fatal OOM rollback). */
NodeApiRefsState* NodeApiRefs_CreateState(void);

/* Destroy step 6a (plan 3.6, R-REF-8, R6): releases every strong root
 * and every weak handle (+ R6 root) exactly once, frees every ref
 * record. Scope roots (HIGH-4, owned by the N1.2 stack) are NOT
 * released here; they are released by scope close / step 6b. Must be
 * called on the env thread with the env context entered (destroy
 * enters ContextGuard; the standalone nodeapi_refs_teardown_all
 * enters it itself). Idempotent: a second call finds an empty state.
 * Does NOT free the NodeApiRefsState struct itself (see
 * NodeApiRefs_FreeState); does NOT touch the env registry. */
void NodeApiRefs_DestroyForEnv(napi_env env);

/* Frees the NodeApiRefsState struct itself, after
 * NodeApiRefs_DestroyForEnv has released every root. Null-safe.
 * Must be called on the env thread. */
void NodeApiRefs_FreeState(NodeApiRefsState* state);

/* ----- N1.1 accessor (implemented by NodeApiEnv.* once it lands;
 * the provider's NodeApiEnv.cpp implements it today via the live
 * registry). Returns the per-env refs state (created once at env
 * creation, destroyed at destroy step 6a). NULL for NULL/dead env
 * (checked by pointer VALUE only, never dereferencing). ----- */
NodeApiRefsState* NodeApiEnv_RefsState(napi_env env);

/* ------------------------------------------------------------------ */
/* Teardown / test entry points (never a napi_* symbol).              */
/* ------------------------------------------------------------------ */

/* Release all ref-record roots inside the env's refs state without
 * freeing the state struct or the env. Called by N1.1 destroy step 6a
 * (plan 3.6) via nodeapi::TeardownRefs. Frees all ref records (live
 * and dead). Scope roots are owned by the N1.2 stack and are released
 * separately at step 6b. After this, every ref handle from this env
 * is invalid (unknown-handle lookup returns napi_invalid_arg while
 * the env is live; after the env is freed the pointer is host
 * use-after-free, same rule as a freed env, R-ENV-10). Null env is a
 * no-op (teardown helper). */
void nodeapi_refs_teardown_all(napi_env env);

/* Create a refs-enabled env for (runtime, context) on the calling
 * thread. Probe-only factory: implemented as a thin adapter over the
 * real private host API (chakra_nodeapi_create_env with NULL
 * filename/services/host_ctx) so probes exercise the reconciled
 * env + destroy-step-6a wiring end to end. Returns NULL on null
 * runtime/context or OOM. The runtime/context must outlive the env;
 * this never disposes them. */
napi_env nodeapi_refs_test_create_env(JsRuntimeHandle runtime,
                                       JsContextRef context);

/* Destroy an env created by nodeapi_refs_test_create_env. Thin
 * adapter over the real provider (chakra_nodeapi_destroy_env):
 * runs the full plan 3.6 sequence (including refs teardown at step
 * 6a and scopes teardown at step 6b) then frees the env. Must run on
 * the env thread before JsDisposeRuntime. Null is a no-op (test helper). */
void nodeapi_refs_test_destroy_env(napi_env env);

/* Test-hook helpers (semantics R-REF-14, unified total).
 * Debug/probe builds only: declared only when
 * CHAKRA_NODEAPI_TEST_HOOKS is defined (run_refs_probe.sh defines
 * it), mirroring the scopes root counter so release libChakraNodeApi
 * never exports them (checked by the N1.6 exports inventory). */
/* NodeApiRefs_RefRootCount: this TU's ref-record contribution to the
 * R-REF-14 total (live strong roots + live weak handles; dead refs
 * contribute 0). Consumed by chakra_nodeapi_test_root_count
 * (NodeApiScopes.h) to form the normative total (scope roots +
 * this). The scopes probe shim stubs it to 0 (no refs there). */
#ifdef CHAKRA_NODEAPI_TEST_HOOKS
size_t NodeApiRefs_RefRootCount(napi_env env);

/* chakra_nodeapi_test_ref_root_count: DEBUG-ONLY per-TU counter
 * (refs part only, same value as NodeApiRefs_RefRootCount). Kept for
 * debugging; probes assert against the total
 * (chakra_nodeapi_test_root_count in NodeApiScopes.h), not this.
 * Null/dead env returns 0. */
size_t chakra_nodeapi_test_ref_root_count(napi_env env);

/* Root count recorded at env creation for the refs part (0).
 * Assertions compare total equality to the total baseline
 * (chakra_nodeapi_test_root_baseline in NodeApiScopes.h, also 0);
 * this is retained for symmetry with the scopes baseline. */
size_t chakra_nodeapi_test_ref_root_baseline(napi_env env);
#endif /* CHAKRA_NODEAPI_TEST_HOOKS */

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* CHAKRA_NODEAPI_REFS_H_ */
