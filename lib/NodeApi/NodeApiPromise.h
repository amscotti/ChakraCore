//-------------------------------------------------------------------------------------------------------
// Copyright (c) ChakraCore Project Contributors. All rights reserved.
// Licensed under the MIT license. See LICENSE.txt file in the project root for full license information.
//-------------------------------------------------------------------------------------------------------
//
// Node-API promises (plan doc/node-api-plan.md task N3.4, A6 promise
// half; PR-8/PR-10 host-pump contract; status rows in
// doc/node-api-status-mapping.md C column).
//
// V8 surface implemented here:
//   napi_create_promise / napi_resolve_deferred / napi_reject_deferred /
//   napi_is_promise
// No other promise symbol is defined here (names not in the v8 set).
//
// Continuation-callback detection story (PR-8, normative):
//   The engine stores the callback as a single slot
//   `JavascriptLibrary::nativeHostPromiseContinuationFunction` plus its
//   state pointer (`lib/Runtime/Library/JavascriptLibrary.h:469-470`).
//   `JsSetPromiseContinuationCallback` (`lib/Jsrt/Jsrt.cpp:3553`) writes
//   it; `EnqueueTask` reads it (`JavascriptLibrary.cpp:5627`); when it
//   is null the engine falls back to
//   `HostScriptContext::EnqueuePromiseTask` and throws
//   `JSERR_HostMaybeMissingPromiseContinuationCallback` on failure.
//   The internal predicate `HasNativeHostPromiseContinuationCallback()`
//   (`JavascriptLibrary.h:891`) is C++ engine-internal and is NOT
//   reachable through the public JsRT C API: there is no
//   `JsGetPromiseContinuationCallback` in `ChakraCommon.h` /
//   `ChakraCore.h`, and the provider is restricted to the public JsRT
//   C API by plan SS3.1 (no engine internals, no C++ engine headers).
//   The provider therefore cannot detect a host-installed callback
//   through its allowed surface.
//   Fallback (task brief): install-only-if-provider-absent via a
//   provider-side per-context flag set at env create. The provider
//   installs its own trampoline-queuing callback the first time an env
//   is created on a context; later envs on the same context share the
//   queue and do not reinstall. Documented host-clobber limitation: if
//   the host installed BEFORE the first env create, the provider's
//   install overwrites (clobbers) it, because the host install is
//   undetectable. Hosts that want their own queue must therefore
//   install (or re-install) AFTER `chakra_nodeapi_create_env` returns,
//   so the host's callback is the last writer and owns the queue.
//   Hosts that never install keep the provider's queue and must drain
//   it via the provider pump entry point
//   (`NodeApiPromise_Pump`, hidden visibility, probe/host link time):
//   the provider never spins its own loop (PR-8) and never blocks.
//
// Pump contract (PR-8, normative; plan SS3.2 host API doc):
//   - Settling a deferred (resolve/reject) only transitions the promise
//     state and enqueues reaction tasks through the current
//     continuation callback (host or provider). The `.then` handlers do
//     NOT run until the queue owner pumps: the host pumps its own queue
//     with `JsCallFunction(task, {undefined}, 1, ...)` until quiet, or
//     (provider-queue case) calls `NodeApiPromise_Pump` on the env
//     thread with the env context current. `JsIdle` is a separate
//     GC/idle pump and does not drain the promise queue.
//   - The test host (`tools/nodeapi/ch-nodeapi.cpp`) installs its pump
//     after env creation so the host owns the queue (see host comment
//     at the install site). The probe (`promise_script_probe.cpp`)
//     does the same for its host-queue phases and uses
//     `NodeApiPromise_Pump` for its provider-queue phase.
//
// Ownership record (memory-management.md SS3; plan N3.4 "required"):
//
//   Resource / creation call / source location:
//     1) PromiseState (opaque struct, one per env), created by
//        `NodeApiPromise_CreateState` from `chakra_nodeapi_create_env`
//        (NodeApiEnv.cpp), freed by `NodeApiPromise_FreeState` at
//        destroy step 6 after `NodeApiPromise_DestroyForEnv`.
//     2) DeferredRecord (one per `napi_create_promise`), heap-allocated
//        in `NodeApiPromise.cpp`; the record address IS the
//        `napi_deferred` handle. Holds the resolve/reject function
//        roots from `JsCreatePromise` (`ChakraCore.h:963-967`).
//     3) PromiseQueue (one per JsContextRef on which the provider
//        installed), provider-global, holds rooted continuation tasks
//        (`JsAddRef` at enqueue, `JsRelease` after the pump's
//        `JsCallFunction` returns).
//     4) The promise `napi_value` returned to the caller (rooted into
//        the innermost scope by `NodeApiScopes_RootIntoInnermost`,
//        like every other value-returning lane).
//
//   Domain and allocator instance (including backing allocator lifetime):
//     (1)+(2) Provider native heap (C++ allocator bound to the provider;
//        backing lifetime == env lifetime for (1), deferred lifetime
//        for (2)). (3) Provider native heap (process lifetime per
//        context entry in the global map; the queue object itself is
//        never freed at env destroy, only its tasks are released).
//     (4) Recycler heap (engine value); scope-slot lifetime.
//
//   Physical allocation base, extent, alignment:
//     One heap allocation per PromiseState / DeferredRecord /
//     PromiseQueue; one scope slot per returned promise; each queued
//     task is one engine function value rooted once.
//
//   Logical length / borrowed views:
//     DeferredRecord holds exactly two borrowed-at-first function
//     pointers (resolve/reject) that become rooted handles; after
//     settling both are released (nullptr). The queue holds borrowed
//     task pointers that are independently rooted (no aliasing with
//     deferred roots or scope roots).
//
//   Owner; transfer/publication point; remaining aliases:
//     (1) Owned by the env (NodeEnv::promise_state); created at env
//        create, published to the four napi_* entries via the
//        registry-resolved `NodeApiEnv_PromiseState` accessor (pointer
//        VALUE lookup, R-ENV-10). (2) Owned by (1)'s deferred map; the
//        handle is published as the `napi_deferred` out-param (borrowed
//        to the caller; meaningful only while the owning env is live
//        and the record is in the map). After env destroy the handle
//        value is dead (clean `napi_invalid_arg` without dereference).
//        (3) Owned by the provider-global map keyed by context VALUE;
//        the trampoline's `callbackState` points at it. (4) Owned by
//        the caller's scope until scope close / destroy step 6b.
//
//   Borrow invalidation: GC / detach / growth / mutation / reset / close /
//     disposal:
//     (2) dies at first settle (roots released, record retained as a
//        settled tombstone so double-settle is a JS no-op returning
//        `napi_ok` without calling) or at destroy step 6 (all remaining
//        records released + freed). After the owner env is destroyed
//        the handle is invalid; any use returns `napi_invalid_arg`
//        from the env-liveness check before touching the map (no UAF).
//        Cross-env use (deferred from env A with env B) fails the
//        per-env map lookup and returns `napi_invalid_arg`.
//     (3) tasks die when pumped (JsRelease after JsCallFunction) or at
//        env destroy (remaining tasks released). The queue object
//        outlives envs on that context (shared); runtime disposal
//        invalidates queued tasks (R5: host must pump to quiet and/or
//        release before dispose).
//     (4) dies at scope close / destroy.
//
//   Potential user-code or reentrancy points:
//     `JsCreatePromise` (allocates, no user code), `JsCallFunction` of
//     the resolve/reject functions (may run arbitrary JS: `.then`
//     reactions are enqueued, not run synchronously; a thenable's
//     `then` getter may throw synchronously inside the resolve call
//     itself, surfacing as `JsErrorScriptException` -> pending), and
//     pump `JsCallFunction` of queued tasks (arbitrary JS, may create
//     new promises/deferreds, may settle other deferreds, may enqueue
//     more tasks). Deferred map mutation is never held across these
//     calls: the callee function value is loaded and the settled flag
//     is set BEFORE the call; roots are released AFTER.
//
//   Thread/context allowed for use, root changes and destruction:
//     Owner thread only, env context entered (plan SS3.2/R3). All
//     four napi_* entries use `nodeapi::EntryGuard` (validates first,
//     enters context, then the pending gate). The deferred-map and
//     queue accesses check owner thread implicitly via the guard.
//     The continuation trampoline runs on the thread executing JS
//     (the owner thread in the provider model); it only does
//     `JsAddRef` + push (mutex-protected) and never calls JS.
//
//   Release on success, partial failure, exception, OOM, cancel and shutdown:
//     Create: if `JsCreatePromise` fails, nothing is allocated; if
//        rooting a function fails, the other root is released and the
//        record freed; if scope-rooting the promise fails, the record
//        is removed and both roots released (nothing published).
//     Settle (first call): mark settled, call the function with
//        `this=undefined`, then release both roots exactly once (even
//        if the call throws). Second+ calls: return `napi_ok` without
//        calling (JS-level no-op; Node's behaviour for double settle).
//        A throwing settle leaves a pending exception and returns
//        `napi_pending_exception` (never cleared implicitly); the
//        record is still settled (roots released).
//     Destroy (step 6): release every unsent() record's roots and free
//        every record; release every queued task's root (queue may
//        still be shared, but its tasks must not survive destroy).
//
//   Memory accounting, capacity/retention bound and exhaustion behavior:
//     No provider-side bound on deferred count or queue depth beyond
//     addon use (each deferred is O(1) small native memory + two
//     engine roots; each queued task is one engine root). OOM in
//     record allocation maps to `napi_generic_failure` with last-error
//     recorded, nothing published. Deferred roots (2 per unsettled
//     deferred) are NOT part of the R-REF-14 scope total
//     (`chakra_nodeapi_test_root_count` counts scope + ref roots
//     only); probes assert scope-total equality after scope close and
//     deferred-count equality via `NodeApiPromise_DeferredCount`.
//     Queue depth is observable via `NodeApiPromise_QueueDepth`.
//
//   Tests / commands / measured results / remaining uncertainty:
//     test/nodeapi/probes/promise_script_probe.cpp (native-resolve
//     settles after host pump, native-reject, double-settle no-op,
//     deferred-after-destroy clean failure, cross-env invalid_arg,
//     is_promise true/false, settle-thenable-throw pending) run via
//     test/nodeapi/probes/run_promise_script_probe.sh (test + jit).
//     Residuals: host-queue phases install the host callback after env
//     create (last-writer-wins); a host that installs before create
//     will be clobbered (documented limitation, not probed as a
//     separate phase because the probe must pass). Provider-queue
//     phase uses the hidden pump; a future exported host pump is a
//     follow-up, not required when the host owns its own queue.
//     Linux x64 not claimed (Mac only, N5.5 receipt pass).
//
// Language: C++17 (GNU++17 via `zig c++`, matching Wave 1/2) behind
// the extern "C" napi_* surface (PR-4 decision record: C++ through
// provider v1). No C++ exception crosses the extern "C" surface.

#ifndef CHAKRA_NODEAPI_PROMISE_H_
#define CHAKRA_NODEAPI_PROMISE_H_

#include "js_native_api.h" /* napi_status, napi_env, napi_deferred, ... (NAPI_VERSION=8) */

#ifdef __cplusplus
extern "C" {
#endif

/* --------------------------------------------------------------------------
 * Per-env promise state (opaque to N1.1; see NodeApiRefs.h pattern).
 * Created by NodeApiPromise_CreateState() at env creation, destroyed at
 * destroy step 6 (NodeApiPromise_DestroyForEnv + FreeState). Env owner may
 * consult NodeApiPromise_DeferredCount() before destroy for diagnostics.
 * ------------------------------------------------------------------------ */
struct NodeApiPromiseState;

/* Creates fresh (empty) promise state. Provider-owned; see
 * NodeApiPromise_FreeState. Never returns null on success (aborts via
 * bad_alloc only if the provider cannot allocate env state at all;
 * chakra_nodeapi_create_env treats that as fatal OOM rollback). */
NodeApiPromiseState* NodeApiPromise_CreateState(void);

/* Destroy step 6 (plan SS3.6): releases resolve/reject roots of every
 * unsettled deferred, frees every deferred record, releases every queued
 * continuation task's root. Must be called on the env thread with the env
 * context entered. Does NOT free the NodeApiPromiseState struct itself
 * (see NodeApiPromise_FreeState). Idempotent-safe. */
void NodeApiPromise_DestroyForEnv(napi_env env);

/* Frees the NodeApiPromiseState struct itself, after
 * NodeApiPromise_DestroyForEnv has released every root. Null-safe.
 * Must be called on the env thread. */
void NodeApiPromise_FreeState(NodeApiPromiseState* state);

/* Installs the provider continuation trampoline for the env's context
 * iff the provider has not already installed one for that context
 * (provider-side flag, PR-8 fallback; undetectable host installs are
 * clobbered -- host must install after env create to win). Called
 * once from chakra_nodeapi_create_env. No-op if already installed.
 * Never fails the create (install errors are ignored; the env is
 * still usable, and a host-installed callback, when present, is the
 * effective one). */
void NodeApiPromise_EnsureContinuationInstalled(napi_env env);

/* ----- N1.1 accessor (implemented by NodeApiEnv.cpp via the live
 * registry). Returns the per-env promise state (created once at env
 * creation, destroyed at destroy step 6). NULL for NULL/dead env
 * (checked by pointer VALUE only, never dereferencing). ----- */
NodeApiPromiseState* NodeApiEnv_PromiseState(napi_env env);

/* Drain the provider's continuation queue for this env's context by
 * calling each queued task with this=undefined via JsCallFunction,
 * until quiet or until `max_tasks` tasks have been pumped. Returns
 * the number of tasks pumped in `*out_pumped` (when non-NULL) and
 * napi_ok on quiet. Returns napi_generic_failure (with the bound in
 * last_error) when the bound is hit before quiet, and
 * napi_pending_exception when a pumped task throws (exception stays
 * pending). `max_tasks==0` means no bound (keep going until quiet).
 * Hidden visibility: probes that link the TUs directly call this;
 * release libChakraNodeApi does not export it (hosts that install
 * their own callback pump their own queue instead). */
napi_status NodeApiPromise_Pump(napi_env env, size_t max_tasks,
                                size_t* out_pumped);

/* Number of deferred records currently in the env's map (settled
 * tombstones included). 0 for NULL/dead env. Hidden (probe link only). */
size_t NodeApiPromise_DeferredCount(napi_env env);

/* Number of tasks currently queued in the provider queue for the env's
 * context (0 if the provider never installed / host owns the queue).
 * Hidden (probe link only). */
size_t NodeApiPromise_QueueDepth(napi_env env);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* CHAKRA_NODEAPI_PROMISE_H_ */
