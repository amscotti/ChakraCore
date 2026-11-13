//-------------------------------------------------------------------------------------------------------
// Copyright (c) ChakraCore Project Contributors. All rights reserved.
// Licensed under the MIT license. See LICENSE.txt file in the project root for full license information.
//-------------------------------------------------------------------------------------------------------
//
// Async work + thread-safe functions + uv loop (plan doc/node-api-plan.md card N4.4, B4/B5; PR-7 host
// services discipline; status rows in doc/node-api-status-mapping.md C column).
//
// Implements the v8 host surface:
//   B4: napi_create_async_work / napi_queue_async_work / napi_cancel_async_work /
//       napi_delete_async_work / napi_get_uv_event_loop
//   B5: napi_create_threadsafe_function / napi_get_threadsafe_function_context /
//       napi_call_threadsafe_function / napi_acquire_threadsafe_function /
//       napi_release_threadsafe_function / napi_ref_threadsafe_function /
//       napi_unref_threadsafe_function
//
// Host contract (plan SS3.2, PR-7): execution goes through the injected
// chakra_nodeapi_host_services (post_work / post_task / cancel_work /
// ref_loop / unref_loop / uv_loop). The provider keeps no internal thread
// pool in v1. Without scheduling services (post_task == NULL or
// post_work == NULL) every env-taking entry in this card returns
// napi_generic_failure (D7, never fake success). The three following
// rules make that verifiable and keep the status mapping honest:
//   * G1 (binding validation precedes services check): NULL binding
//     args (NULL out-param, NULL work/func handle, NULL execute callback,
//     initial_thread_count == 0) return napi_invalid_arg even when
//     services are absent. The no-services probes use valid binding
//     args (or valid handles created with services) so the services
//     check is the deciding failure and returns napi_generic_failure.
//   * G2 (services check precedes state checks): with valid binding
//     args, the services check runs before cross-env/state/size checks
//     so a no-services failure is generic_failure, not a state error.
//   * G3 (func-only stale handles): napi_get_threadsafe_function_context /
//     napi_call_threadsafe_function / napi_acquire_threadsafe_function /
//     napi_release_threadsafe_function take no env and therefore cannot
//     consult services directly. A NULL handle is napi_invalid_arg (G1).
//     A non-NULL unknown/stale handle (never existed, already finalized,
//     or freed) is napi_generic_failure (Node's TSFN post-finalize
//     status; cf. call after finalize => generic_failure). The
//     no-services matrix exercises them with stale handles and asserts
//     generic_failure, which is also the with-services stale status.
//
// Async-work state machine (brief): created -> queued -> executing ->
// completed/cancelled -> deleted.
//   Created:     napi_create_async_work succeeded; owner holds the record.
//   Queued:      napi_queue_async_work accepted via services.post_work;
//                worker ref held; execute has not yet been entered.
//   Executing:   the host thread entered the provider execute trampoline
//                (state Queued -> Executing); the user execute callback is
//                running off-thread (R3, native-only). Completion is still
//                pending (host will post the completion trampoline).
//   Completed:   the completion trampoline ran on the env thread and
//                delivered napi_ok to the user complete callback. The
//                record is deletable. Completed works may be re-queued
//                (Completed -> Queued); the cycle restarts with a fresh
//                worker ref and cancel flag.
//   Cancelled:   the completion trampoline ran and delivered
//                napi_cancelled (cancel won before user execute ran).
//                Deletable; may be re-queued like Completed.
//   Deleted:     napi_delete_async_work (or env teardown for in-flight
//                records via refcount drain) erased the map entry and
//                dropped the owner ref; the record is freed when the last
//                ref drops (worker + owner). Deleted handles are
//                napi_invalid_arg (double delete / use after delete).
// Cancel rules: napi_cancel_async_work succeeds only in Queued (it sets
// the cancel flag and calls services.cancel_work best-effort); in
// Executing / Completed / Cancelled / Created it returns
// napi_generic_failure (already started or nothing to cancel). The flag
// is checked by the execute trampoline before invoking user execute: set
// => user execute is skipped, completion delivers napi_cancelled. The
// claim (Queued->Executing + flag read) and the cancel decision are
// serialized by the record mutex, so "cancel ok => execute never runs"
// is atomic under concurrent cancel-vs-start, not sequential-only.
// Delete rules (vendored headers are silent on state; upstream docs /
// Node implementation require the work to be idle): delete succeeds
// only in Created / Completed / Cancelled and returns
// napi_generic_failure in Queued / Executing. Null work is
// napi_invalid_arg; unknown/double-deleted is napi_invalid_arg; cross-env
// (work from env A used on env B) is napi_invalid_arg.
//
// TSFN state (brief): refcounted state with holders = {JS side, acquirers,
// queued items, pending finalize} plus a separate native state_refs for
// free-after-finalize safety (see ownership record).
//   JS side: 1 at create iff func != NULL (JsAddRef root held until
//            finalize). Drops at finalize.
//   Acquirers: thread_count, starting at initial_thread_count (never 0
//            at create; 0 is napi_invalid_arg). Acquire ++, release --.
//            New calls/acquires fail with napi_generic_failure once
//            thread_count reaches 0 (finalizing/finalized/closing).
//   Queued items: 1 per queued TsfnItem (state_refs increment at enqueue,
//            decrement at drain/abort delivery). Graceful release
//            (napi_tsfn_release) delivers queued items before finalize;
//            abort release (napi_tsfn_abort) drops them without JS.
//   Pending finalize: 1 while a finalize task is posted/queued but not
//            yet executed on the env thread (exactly-once guard).
// Teardown (env destroy) aborts queued items and runs
// thread_finalize_cb on the env thread exactly once, even if threads
// have not yet all released (the JS-side holder is dropped by teardown).
// Ref/unref: per-TSFN is_ref (starts true) coalesced per env into
// AsyncState::loop_ref_count; only the 0->1 transition calls
// services.ref_loop and only the 1->0 transition calls
// services.unref_loop. Null ref_loop/unref_loop are no-ops (keep-alive is
// best effort), but missing post_task still fails ref/unref with
// napi_generic_failure per G1/G2 (scheduling services are the gate).
// Queue-full: max_queue_size == 0 means unlimited. Otherwise size >= max
// is full: nonblocking mode returns napi_queue_full; blocking mode waits
// on a condition variable UNLESS the caller is the env (owner) thread, in
// which case it returns napi_queue_full immediately (waiting would
// deadlock against the only drainer, the env thread). This matches the
// Hermes/Node precedent cited in the brief.
//
// Thread confinement (R3, enforced by design):
//   * Worker (execute trampoline): touches ONLY the AsyncWork fields
//     (execute/complete/data pointers, atomic state/cancel flags,
//     atomic refs) + the user execute callback. It never dereferences
//     napi_env (the stored env VALUE is copied into the user call as an
//     opaque pointer for the callback signature, never validated),
//     never calls Js* / napi_* that enter the env, never touches
//     NodeEnv, AsyncState, TSFN state, or the callback stack. The only
//     shared writes are atomics, so R3 holds even if the env is destroyed
//     concurrently.
//   * Cross-thread TSFN (call/acquire/release/get_context): touches ONLY
//     the TSFN state via the global handle map (lookup+increment under
//     the map mutex) + the TSFN mutex/condvar + atomic counters. It never
//     enters the env context, never calls Js*, never roots values, never
//     touches NodeEnv/AsyncState/callback stack. Direct-form values
//     (napi_value data) are stored BORROWED (no off-thread JsAddRef) and
//     used only on the env drain thread; the caller must keep them alive
//     until delivery (probe roots them in a scope/ref).
//   * Env-thread (create/queue/cancel/delete/ref/unref/get_uv + completion /
//     drain / finalize trampolines): owner-thread only (EntryGuard
//     except the four func-only entries). Trampolines run via
//     services.post_task on the env thread; each validates env liveness
//     BY VALUE (registry) before dereferencing, enters the context
//     (ContextGuard), opens an implicit handle scope, pushes a callback
//     record entry (TU-local stack, LIFO, for balance verification),
//     invokes user code without holding any provider-global or TSFN
//     mutex, then closes the scope, pops, and restores the context.
//     Post-destroy trampolines drop silently (no-callback-after-destroy).
//     The callback record stack is never held across user code with its
//     mutex held (pop/push lock only, R-ENV-12).
//
// Ownership record (memory-management.md SS3; plan N4.4 "required"):
//
//   Resource / creation call / source location:
//     1) AsyncState (one per env that uses this card, lazy-created by
//        EnsureState on the first env-taking async call, erased by the
//        env cleanup hook at destroy step 3). Holds loop_ref_count +
//        owned work/TSFN sets for teardown enumeration. Provider native
//        heap (C++ new), process lifetime == env lifetime (lazy).
//     2) AsyncWork (one per napi_create_async_work; address IS the
//        napi_async_work handle). lib/NodeApi/NodeApiAsync.cpp.
//        Holds owner ref + worker ref (atomic refs), atomic state,
//        atomic cancel flag, record mutex (serializes the
//        Queued->Executing decision vs cancel). No JS roots
//        (async_resource/name are coerced-and-discarded at create;
//        data is user-owned).
//     3) TsfnState (one per napi_create_threadsafe_function; address IS
//        the napi_threadsafe_function handle). Holds thread_count
//        (atomic), mutex+condvar+queue, closing/finalizing/finalized,
//        is_ref, drain_scheduled, JS func root (if func != NULL), native
//        state_refs. Provider native heap.
//     4) TsfnItem (one per napi_call_threadsafe_function enqueue; heap
//        allocation owned by the TSFN queue). Holds void* data (native
//        pointer for call_js_cb form; borrowed napi_value bits for direct
//        form, never rooted off-thread). No JS roots held by provider.
//     5) The TSFN JS func root (single JsAddRef at create, JS-side
//        holder). Recycler heap; released only on the env thread at
//        finalize (JsRelease, R5). Never touched off-thread.
//
//   Domain and allocator instance (including backing allocator lifetime):
//     (1)+(2)+(3)+(4) provider native heap via C++ new/delete (backing
//     lifetime == process; per-env (1) == env lifetime). (5) Recycler
//     heap via JsAddRef/JsRelease.
//
//   Physical allocation base, extent, alignment:
//     One heap allocation per (1)/(2)/(3)/(4); one scope-independent
//     engine root per (5).
//
//   Logical length / borrowed views (kept separate from allocation):
//     Loop ref count (kept-alive TSFNs per env; 0..#TSFNs). Queue
//     capacity (max_queue_size; 0 = unlimited) vs queue size (items).
//     Work/TSFN data pointers are borrowed (provider never frees user
//     data; direct-form napi_value bits are borrowed, caller must keep
//     alive until delivery).
//
//   Owner; transfer/publication point; remaining aliases:
//     (1) Owned by the global env map (value-keyed, mutex-protected);
//         published to env-taking entries by value lookup.
//     (2) Owned by the global work map (value-keyed) + AsyncState set;
//         published as the napi_async_work out-param (borrowed to the
//         addon while the owning env is live and the entry exists).
//         Cross-env use fails the owner-env check (invalid_arg).
//     (3) Owned by the global TSFN map (value-keyed) + AsyncState set;
//         published as the napi_threadsafe_function out-param.
//     (4) Owned by (3)'s queue; transferred caller -> queue at enqueue;
//         transferred queue -> drain/abort at dequeue.
//     (5) Owned by (3) from create to finalize; never published to the
//         addon (the func value itself remains caller-rooted by its own
//         scope/ref; the provider takes a SEPARATE root).
//
//   Borrow invalidation: GC / detach / growth / mutation / reset / close /
//     disposal:
//     (2) dies at delete/destroy-freed (refs -> 0, map erased first by
//         value; stale handle lookups miss). After destroy the handle is
//         dead (clean invalid_arg/generic_failure without dereference).
//     (3) dies at finalize-freed (map erased, owner ref dropped, last
//         state_ref drops). Stale handles miss the map (generic_failure
//         for func-only; invalid_arg for cross-env env-taking).
//     (4) dies at drain delivery or abort (both decrement state_refs).
//     (5) dies at finalize (JsRelease on env thread). R5: all (5) roots
//         release before JsDisposeRuntime (destroy step 3 hook runs
//         before refs/scopes teardown; finalize releases (5) there).
//     Queue drain tasks / completion tasks posted before destroy that
//     run after destroy miss the liveness check and drop the item/record
//     without user JS (no-callback-after-destroy).
//
//   Potential user-code or reentrancy points:
//     Worker execute (user napi_async_execute_callback, off-thread): may
//     run arbitrary native code; must not call JS APIs (R3). Reentrancy
//     into napi_call_threadsafe_function from execute is supported
//     (cross-thread queue). Env-thread: user complete callback, TSFN
//     call_js_cb, TSFN direct func call (JS), thread_finalize_cb, and the
//     async_resource/name coercions (ToObject/ToString, valueOf/toString
//     may run) all run with NO provider-global/TSFN mutex held. Queue
//     mutation is never held across these calls; callees/flags are
//     loaded before calling (cf. deferred settle in NodeApiPromise.cpp).
//
//   Thread/context allowed for use, root changes and destruction:
//     Worker execute: any host worker thread, NO context (R3).
//     Cross-thread TSFN: any thread, NO context.
//     All other entries/trampolines: env (owner) thread with the env
//     context entered (plan SS3.2/R3) via EntryGuard/ContextGuard.
//     Root change (JsAddRef/JsRelease): env thread only.
//
//   Release on success, partial failure, exception, OOM, cancel and shutdown:
//     Create work: OOM in record/map/set => generic_failure, nothing
//     published (map/set rolled back). Coercion failure =>
//     object_expected / string_expected, nothing allocated.
//     Create TSFN: invalid binding (func shape, thread_count 0) =>
//     invalid_arg before allocation. Func root failure => generic_failure
//     with rollback (set/map removed, record freed, loop ref reverted).
//     Queue: already queued/executing => generic_failure. post_work
//     false => generic_failure with worker ref dropped and state
//     returned to Created (re-queuable). Otherwise callback trampolines
//     own the record until completion (refs).
//     Cancel: Queued => ok (flag + best-effort cancel_work). Other
//     states => generic_failure. Never frees the record.
//     Delete: Queued/Executing => generic_failure (record retained).
//     Created/Completed/Cancelled => erase + owner drop (free iff refs
//     0, i.e. no worker outstanding, which holds by state).
//     TSFN call: finalized/closing => generic_failure. Full +
//     nonblocking (or full + blocking-from-env) => queue_full WITHOUT
//     enqueuing (item not allocated / freed). Enqueued => ok (drain
//     scheduled via post_task; post_task false => generic_failure with
//     rollback dequeue + state_ref drop).
//     Release: underflow (thread_count already 0) => generic_failure.
//     Abort mode drops queued items (state_ref drops, items freed).
//     Reaching 0 schedules finalize exactly once (pending-finalize
//     holder). Teardown forces finalize exactly once (aborts queue,
//     releases (5), runs thread_finalize_cb, drops owner ref).
//     Shutdown with in-flight workers: destroy drops owner refs only;
//     worker refs keep records alive; completion/drain trampolines after
//     destroy miss liveness and drop refs (free by last drop, native
//     delete only, no JS). No record is freed while a worker/drain may
//     still dereference it (refs + by-value liveness checks).
//     C++ exceptions from user callbacks: caught in every trampoline
//     (execute => generic_failure delivered to complete; env-thread =>
//     last_error recorded, pending preserved, no unwind across C ABI).
//
//   Memory accounting, capacity/retention bound and exhaustion behavior:
//     Per-env loop_ref_count <= #live TSFNs. TSFN queue bound is
//     caller-chosen max_queue_size (0 = unlimited; unbounded growth is
//     caller-bounded via max). Work/TSFN record counts are caller-scoped
//     (probe asserts NodeApiAsync_WorkCount / TsfnCount return to 0
//     after delete/destroy; root-count: TSFN func roots (1 per live
//     func-form TSFN) are NOT in the R-REF-14 scope total (they are
//     provider-held, like deferred roots in N3.4); probe asserts scope
//     total equality after scope close separately).
//
//   Tests / commands / measured results / remaining uncertainty:
//     test/nodeapi/probes/async_probe.cpp via run_async_probe.sh
//     (test + jit): round-trip values (work + both TSFN forms),
//     cancel-before/after-start, queue-full nonblocking +
//     blocking-from-env-thread queue_full, 4-producer stress (12
//     repeats x 4 threads x 50 items), teardown with in-flight items,
//     no-callback-after-destroy, no-services matrix (env-taking valid
//     args + stale func-only handles => generic_failure; null binding
//     args => invalid_arg per G1, documented). Residuals: host
//     cancel_work is best-effort (provider flag is authoritative);
//     host must still post completion after cancel for cancelled
//     delivery (probe host honors it). ABA on handle reuse after free
//     mirrors the env ABA limit (A5, plan SS3.3): stale pointer-only
//     handles at a reused address alias the new record by construction.
//     Linux x64 not claimed (Mac only, N5.5 receipt pass).
//
// Language: C++17 (GNU++17 via `zig c++`, matching Wave 1/2) behind
// the extern "C" napi_* surface (PR-4 decision record: C++ through
// provider v1). No C++ exception crosses the extern "C" surface.

#ifndef CHAKRA_NODEAPI_ASYNC_H_
#define CHAKRA_NODEAPI_ASYNC_H_

#include "js_native_api.h" /* napi_env, napi_value, NAPI_VERSION=8 */
#include "node_api.h" /* napi_async_work, napi_threadsafe_function, ... */

#ifdef __cplusplus
extern "C" {
#endif

/* Per-env async state (opaque; owned by lib/NodeApi/NodeApiAsync.cpp).
 * Created lazily on the first env-taking async call for an env, erased by
 * the env cleanup hook at destroy step 3. Exposed only for N1.1-style
 * diagnostics; the napi_* entries are the public surface. */
struct NodeApiAsyncState;

/* Creates fresh async state for the env (loop_ref_count 0, empty owned
 * sets). Registers the destroy cleanup hook exactly once. Provider-owned;
 * see the .cpp DestroyForEnv hook. Never returns null on success (throws
 * on OOM; callers catch and map to napi_generic_failure). */
NodeApiAsyncState* NodeApiAsync_CreateState(napi_env env);

/* Destroy step 3 hook (runs in Closing, env thread, env context current):
 * aborts every owned TSFN queue (no JS delivery), runs every owned
 * thread_finalize_cb exactly once on the env thread, cancels every owned
 * in-flight work (marks cancelled, drops owner refs), releases every
 * idle work/TSFN record, unrefs the loop if refs remain, erases the
 * env map entry. Null/dead env is a no-op (registry lookup by value).
 * Does NOT free the AsyncState struct itself (provider-global map owns
 * it until erase; see FreeState). Idempotent-safe. */
void NodeApiAsync_DestroyForEnv(napi_env env);

/* Frees the AsyncState struct itself, after DestroyForEnv. Null-safe.
 * Must be called on the env thread (map erase path calls it after hook). */
void NodeApiAsync_FreeState(NodeApiAsyncState* state);

/* Hidden probe helpers (never exported from release libChakraNodeApi:
 * the build uses -fvisibility=hidden and these carry no default
 * attribute, so they stay hidden; the N1.6 exports check forbids
 * extras). All consult by env/handle VALUE and never dereference
 * dead/fabricated values. 0 for null/dead/unknown. */
/* Live async-work records owned by the env (created, not deleted). */
size_t NodeApiAsync_WorkCount(napi_env env);
/* Live TSFN states owned by the env (created, not finalized). */
size_t NodeApiAsync_TsfnCount(napi_env env);
/* Work lifecycle state for a handle owned by the env:
 * 0 Created, 1 Queued, 2 Executing, 3 Completed, 4 Cancelled, -1 unknown
 * (null/cross-env/unknown/deleted). */
int NodeApiAsync_WorkState(napi_env env, napi_async_work work);
/* Queued item count for a TSFN handle (0 for null/unknown). */
size_t NodeApiAsync_TsfnQueueDepth(napi_threadsafe_function func);
/* Current TSFN thread holder count (-1 for null/unknown). */
int64_t NodeApiAsync_TsfnThreadCount(napi_threadsafe_function func);
/* Per-env loop ref count (#TSFNs currently keeping the loop alive). */
size_t NodeApiAsync_LoopRefCount(napi_env env);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* CHAKRA_NODEAPI_ASYNC_H_ */
