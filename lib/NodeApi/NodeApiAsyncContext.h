//-------------------------------------------------------------------------------------------------------
// Copyright (c) ChakraCore Project Contributors. All rights reserved.
// Licensed under the MIT license. See LICENSE.txt file in the project root for full license information.
//-------------------------------------------------------------------------------------------------------
//
// Async context + callback scopes (lib/NodeApi/NodeApiAsyncContext.*) -- N4.3.
//
// Plan: doc/node-api-plan.md §6 card N4.3, §4 Table B B3, §3.2 host
// services, §3.6 step 4 (async cleanup completion). Semantics:
// doc/node-api-semantics.md §1 (lifetime rules R-ENV-4/5/11/12) +
// §5 (callback discipline, applied by analogy to make_callback).
// Status mapping: doc/node-api-status-mapping.md C column (B3 rows).
//
// =====================================================================
// LOUD NO-OP WARNING (plan §4 B3, card N4.3 Do step 1):
//   napi_async_init / napi_async_destroy / napi_open_callback_scope
//   and the async_context parameter of napi_make_callback are
//   PROVIDER BOOKKEEPING ONLY. This provider implements NO
//   async_hooks propagation, NO AsyncContext stack, NO
//   AsyncLocalStorage semantics, and NO context switching across
//   awaits / callbacks / threads. Runtime scope (§0 C) is out of
//   scope permanently in this plan. Addons that call
//   async_init and expect a value stored by an earlier native call
//   to flow through async_hooks into a later JS callback will NOT
//   observe it here: the provider records the handle identity and
//   validates liveness, then discards the resource/name JS values
//   (no JS roots held, see record (d)). napi_make_callback validates
//   the context handle but does NOT enter it for ALS purposes; it
//   does enter the ENV context (JsSetCurrentContext via EntryGuard)
//   and open a HANDLE scope, exactly like napi_call_function.
//   Do not add ALS semantics in a fork without a plan amendment:
//   see doc/node-api-plan.md §10 for the deferred-lane rules.
// =====================================================================
//
// Surface implemented here (B3, v8):
//   napi_async_init / napi_async_destroy / napi_make_callback /
//   napi_open_callback_scope / napi_close_callback_scope
//
// Status mapping (C column, per function):
//   napi_async_init: Yes (executes with pending). Result/resource/
//     name NULL (C NULL) => napi_invalid_arg. resource JS
//     null/undefined => napi_object_expected (ToObject pre-check,
//     no pending synthesized, mirroring NodeApiObjects.cpp). Else
//     JsConvertValueToObject; conversion throw => pending (MapJs).
//     name JS null/undefined => napi_string_expected (pre-check).
//     Else JsConvertValueToString; conversion throw => pending.
//     OOM => napi_generic_failure. Success clears last_error.
//   napi_async_destroy: Yes (executes). Context NULL => invalid_arg.
//     Unknown/foreign/dead/double-destroy => invalid_arg (registry
//     by value, never dereferenced). Teardown-compatible (Closing
//     succeeds, like napi_remove_async_cleanup_hook).
//   napi_open_callback_scope: Yes (executes). resource_object /
//     result NULL => invalid_arg. Non-null context must be a live
//     record of THIS env (else invalid_arg); NULL context = default
//     (allowed). No JS type check on resource_object (C column).
//     Strict Alive-only (creates).
//   napi_close_callback_scope: Yes (executes). Scope NULL =>
//     invalid_arg. Scope not in this env's callback-scope stack
//     (foreign / already-closed / fabricated) => invalid_arg.
//     Scope in stack but not innermost (top) =>
//     napi_callback_scope_mismatch AND the named scope's record is
//     still released (retired) -- stack otherwise intact. Mirrors
//     the N1.2 discipline (NodeApiScopes CloseImpl): only the named
//     scope is removed; no whole-stack unwind. Teardown-compatible
//     (Closing succeeds, like other removers).
//   napi_make_callback: No (gated, returns napi_pending_exception
//     without executing when a JS exception is pending).
//     recv/func NULL (C NULL) => invalid_arg. func not a function
//     => napi_invalid_arg (CHECK_TO_FUNCTION, NOT function_expected).
//     recv JS null/undefined => napi_object_expected (ToObject
//     pre-check). Non-null context must be live in this env (else
//     invalid_arg); NULL context = default (allowed). argc > 65534
//     => napi_generic_failure (HIGH-1 wrap guard, as in
//     napi_call_function). Callee throw => napi_pending_exception,
//     result untouched. Pump throw => napi_pending_exception,
//     result untouched.
//
// Closing policy (plan §3.6 step 1 + R-ENV-4; choice recorded here):
//   Strict Alive-only (Closing => generic_failure): async_init,
//   open_callback_scope, make_callback (all create/invoke).
//   Teardown-compatible (Closing succeeds): async_destroy,
//   close_callback_scope (both release bookkeeping; hooks/
//   finalizers during destroy steps 2-6 may clean up, parity with
//   Lifetime get/remove and refs delete/get). Probe asserts this
//   from inside a sync cleanup hook (async_destroy + close ok,
//   init/open/make generic_failure).
//
// Callback-scope stack is DISTINCT from handle scopes: opening a
// callback scope does not change NodeApiScopes_Depth, and opening a
// handle scope does not change callback depth. The two stacks are
// implemented in different TUs (this TU vs NodeApiScopes.cpp) and
// never share entries. Do not conflate them (task brief).
//
// Async cleanup completion wiring (§3.6 step 4, B6): the provider
// does NOT drain async cleanup hooks here; NodeApiLifetime does
// (Lifetime_RunAsyncCleanupHooks, pump-until-remove via
// Lifetime_SetAsyncPump). The HOST pump callback passed to
// Lifetime_SetAsyncPump drives hooks to self-remove during pump
// turns. For the Alive phase, the host/provider pump entry point
// is NodeApiPromise_Pump (provider continuation queue). For the
// destroy (Closing) phase, the pump must be a HOST queue drain
// (JsCallFunction loop over the host's continuation queue, after
// installing the host callback post-create): NodeApiPromise_Pump
// gates on Alive (EntryGuard allowClosing=false) and therefore
// cannot be used as the step-4 pump during Closing. The test host
// and the asyncctx probe follow this split; see probe comments.
// This TU's make_callback drain uses NodeApiPromise_Pump (Alive
// phase only, PR-8 provider-queue case); teardown draining is
// host-owned.
//
// Ownership record (memory-management.md §3; plan N4.3 "required"):
//
//   (a) Async context records (napi_async_context__).
//     Resource: one heap record per napi_async_init ({owner value,
//       env generation, per-env id, destroyed flag}). The record
//       address IS the handle value.
//     Domain/allocator: provider native heap (new/delete via the C++
//       allocator bound to the provider). Never Recycler memory,
//       never freed across modules.
//     Owner/transfer: per-env AsyncState (this TU's global map) owns
//       live records; the addon holds only the handle value (borrowed,
//       meaningful only while in the live list). napi_async_init
//       transfers ownership to the live list; napi_async_destroy
//       unlinks and retires (deferred-free until teardown, see (c)).
//     Borrow invalidation: napi_async_destroy (unlink+retire), env
//       destroy teardown DestroyForEnv (free live+retired + state),
//       address-reuse generation mismatch (stale state discarded).
//       Double-destroy is invalid_arg without dereferencing freed
//       memory (live-list search by value).
//     Thread/context: env owner thread only (EntryGuard checks).
//       No Js* calls, no JS roots; no context enter needed beyond
//       the guard's (only async_init needs the context for
//       conversions and uses needContext=true).
//
//   (b) Callback scope records (napi_callback_scope__).
//     Resource: one heap record per napi_open_callback_scope
//       ({owner value, env generation, async_context snapshot,
//       per-env id}). The record address IS the handle value.
//     Domain/allocator: provider native heap, owned by AsyncState's
//       scope stack (LIFO vector).
//     Owner/transfer: AsyncState stack owns the record until close
//       or teardown. Close (clean or mismatch) unlinks ONLY the named
//       record and retires it (deferred-free, (c)); return value or
//       mismatch status is the publication; the record address is
//       dead as a handle after close (second close => invalid_arg).
//     Borrow invalidation: close (unlink+retire), teardown
//       DestroyForEnv, generation mismatch. The stored
//       async_context snapshot is never dereferenced (bookkeeping
//       only, no ALS); it may dangle after the context is destroyed,
//       which is safe here.
//
//   (c) Per-env AsyncState + retired lists + global map.
//     Resource: AsyncState {owner value, generation, next ids,
//       live contexts vector, retired contexts vector, scope stack,
//       retired scopes vector} + one map entry per env value.
//     Domain/allocator: provider native heap (unordered_map buckets +
//       vectors + per-record new). Env itself is the NodeEnv
//       (provider heap); the map holds a COPY pointer to AsyncState,
//       never the NodeEnv (mirrors Lifetime pump map, Wrap lazy
//       state; NodeEnv layout untouched).
//     Owner/transfer: provider (this TU) owns the map and every state.
//       Lazily created on first async_init / open scope for the env,
//       erased by DestroyForEnv (called from NodeApiEnv destroy
//       step 6 via TeardownPromiseScript). host_ctx / env values are
//       compared by pointer VALUE only (R-ENV-10): lookups never
//       dereference dead/fabricated env values.
//     Invalidation: DestroyForEnv (erase+free), generation mismatch
//       on next use (address-reuse stale state freed + fresh state
//       created; matches HIGH-2 tombstone logic in NodeApiEnv).
//       No GC/detach invalidation (no JS values held).
//     Thread/context: owner thread only. The map mutex is held only
//       to consult/update it, never across user code / Js* calls
//       (R-ENV-12). AsyncState vectors are touched only on the owner
//       thread.
//     Release on failure: init/open with NULL binding args =>
//       invalid_arg + no allocation; record new fails (OOM) =>
//       generic_failure + no registration; vector push throws =>
//       record deleted inline + generic_failure; destroy/close
//       unknown => invalid_arg + no mutation.
//     Accounting: no cap beyond addon use; each context/scope is O(1)
//       small native memory, freed at DestroyForEnv. No contribution
//       to the R-REF-14 scope total (no JS roots held; probe asserts
//       the scope total is unchanged by async_init/open). Retained
//       retired records are bounded by open/close churn until
//       teardown (deferred-free prevents ABA aliasing of dead
//       handle values; the alternative -- immediate free -- would
//       let a new record reuse a dead handle's address and alias).
//
//   (d) make_callback handle scope + pump (transient).
//     Resource: one implicit handle scope (NodeApiScopes, R1) opened
//       for the call duration + JsCallFunction args vector + the
//       provider promise queue tasks drained by NodeApiPromise_Pump.
//     Domain/allocator: handle scope + args live in provider native
//       memory / scope slots; queue tasks are Recycler values rooted
//       by the Promise TU (JsAddRef at enqueue, released at pump).
//     Owner/transfer: this TU owns the handle scope handle from open
//       to close (closed exactly once on every path); the result
//       value is rooted into the hidden scope, then moved (an extra
//       root) into the caller's scope via RootIntoCallerScope BEFORE
//       the hidden scope closes (R-SC-9 / §3.8 step 4 pattern), so
//       the published result survives. Async context is validated
//       but NOT entered for ALS (see loud no-op warning); ENV
//       context is entered via EntryGuard (plan §3.2).
//     Borrow invalidation: hidden scope close (releases its roots),
//       pump (tasks released after each JsCallFunction). Result is
//       invalidated by caller-scope close / destroy step 6b (R1).
//     Thread/context: owner thread, env context entered (guard).
//       Thunk/callee JS may reenter the provider (nested callbacks,
//       promise reactions) -- no provider lock held across Js* calls.
//     Release on success / failure / exception / OOM: callee throw
//       or pump throw => close scope, result untouched (no parent
//       root taken), pending preserved, return pending (never
//       cleared implicitly). OOM in args/scope => close scope (if
//       opened), generic_failure, nothing published. Pump bound hit
//       (max_tasks) => pump returns generic_failure, make_callback
//       returns generic_failure (see .cpp for bound choice).
//     Tests: test/nodeapi/probes/asyncctx_probe.cpp (run via
//       run_asyncctx_probe.sh, test + jit). See probe header for the
//       phase list (pairing, scope mismatch, make_callback
//       this/args, pump drain, teardown release, Closing policy).
//       Existing teardown/gate probes re-run to prove step 4 still
//       completes when hooks self-remove during pump turns.
//
// Visibility: napi_* declarations carry NAPI_EXTERN default
// visibility from the vendored headers; helpers below are C-linkage
// but NOT exported from release libChakraNodeApi (provider builds
// with -fvisibility=hidden; these helpers carry no default
// attribute, so they stay hidden; the N1.6 check-nodeapi forbids
// extras). Probes link this TU directly and call them.
//
// Language: C++17 (GNU++17 via `zig c++`, matching Wave 1/2) behind
// the extern "C" napi_* surface (PR-4: C++ through provider v1).
// No C++ exception crosses the extern "C" surface.

#ifndef CHAKRA_NODEAPI_ASYNCCONTEXT_H_
#define CHAKRA_NODEAPI_ASYNCCONTEXT_H_

#include "js_native_api.h" /* napi_env, napi_value, napi_status */
#include "node_api_types.h" /* napi_async_context, napi_callback_scope */

#ifdef __cplusplus
extern "C" {
#endif

/* Destroy step 6 (plan §3.6): frees every live + retired async-context
 * and callback-scope record and erases the per-env AsyncState. Called
 * from NodeApiEnv.cpp (TeardownPromiseScript) with the env in Closing
 * state on the owner thread and the env context entered. Calls no user
 * code, no Js* calls (native-only). Safe to call exactly once per env
 * at end of destroy; a second call (or a call on a dead/fabricated env
 * value) is a no-op: the map is consulted by env pointer VALUE only,
 * never dereferencing a dead env (R-ENV-10). After return, handle
 * values from the freed state are dead (use => napi_invalid_arg by
 * value without dereference). */
void NodeApiAsyncContext_DestroyForEnv(napi_env env);

/* ----- Hidden probe helpers (probe link only, never exported).
 * All consult state by env pointer VALUE and never dereference
 * dead/fabricated values (null/dead => 0). ----- */

/* Number of live (not destroyed) async-context records for the env.
 * 0 for NULL/dead env. Hidden (probe link only). */
size_t NodeApiAsyncContext_ContextCount(napi_env env);

/* Depth of the env's callback-scope stack (open scopes only).
 * Distinct from handle-scope depth (NodeApiScopes_Depth). 0 for
 * NULL/dead env. Hidden (probe link only). */
size_t NodeApiAsyncContext_ScopeDepth(napi_env env);

/* Number of per-env AsyncState entries currently in the provider-
 * global map (process-wide). Used by the probe to assert teardown
 * release: after destroy the count returns to its pre-create
 * baseline (no stale state retained). Hidden. */
size_t NodeApiAsyncContext_TotalStates(void);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* CHAKRA_NODEAPI_ASYNCCONTEXT_H_ */
