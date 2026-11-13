//-------------------------------------------------------------------------------------------------------
// Copyright (c) ChakraCore Project Contributors. All rights reserved.
// Licensed under the MIT license. See LICENSE.txt file in the project root for full license information.
//-------------------------------------------------------------------------------------------------------
//
// Node-API host surface: deprecated registration + fatal routing (plan §4
// Table B1, task N4.1; §3.2 host contract; §1.3 registration facts).
//
// Owns lib/NodeApi/NodeApiHost.* (N4.1 registration half + B1 fatal full
// wiring; Buffer/async/TSFN remain future N4.2-N4.4 in this TU per §5.2).
//
// What this TU provides:
//   - napi_module_register(napi_module*): deprecated self-registration path
//     (plan §1.3). Appends to the process-lifetime pending-module registry
//     (see below). Callable from any thread (including addon static
//     constructors at dlopen, before any env exists).
//   - Private host helpers (§3.2 extension, CHOSEN DESIGN: accessor +
//     direct-invoke hybrid, see "Host invocation design" below):
//       chakra_nodeapi_pending_module_count()
//       chakra_nodeapi_get_pending_module(index, out)
//       chakra_nodeapi_run_pending_registrations(env, exports, result)
//     The test host (tools/nodeapi/ch-nodeapi) is NOT yet wired to the
//     deprecated fallback (it still requires napi_register_module_v1 and
//     errors otherwise); this TU exposes the provider side so a future
//     host update can call run_pending_* without further provider changes.
//     C++ probes (test/nodeapi/probes/host_probe.cpp) exercise the full
//     path directly.
//   - napi_fatal_exception / napi_fatal_error (Table B B1, full host wiring
//     moved here from NodeApiErrorsImpl.cpp N2.5 interface). Fatal routing
//     design is documented below.
//
// Pending-module registry ownership record (memory-management.md §3):
//
//   Resource / creation call: PendingModule record { nm_version, nm_flags,
//     nm_filename copy, nm_register_func, nm_modname copy, nm_priv } created
//     by napi_module_register(mod) on the caller thread (often a dlopen
//     static constructor thread).
//   Domain and allocator: provider native heap (std::string + std::deque
//     element allocated via the C++ allocator bound to the provider).
//     Never Recycler memory; never freed by the addon or host.
//   Physical allocation: one deque element + two heap string buffers per
//     registration (empty SSO buffer when the caller string is short).
//   Owner; transfer/publication point: provider owns the record from push
//     until process exit. The caller's napi_module struct and its string
//     buffers are borrowed at call time only and must outlive the call.
//     String contents are copied (see string-copy rule); pointer fields
//     (nm_register_func, nm_priv) are borrowed values, never dereferenced
//     or freed by the provider except invoking nm_register_func inside
//     run_pending (see invocation rules).
//   Borrow invalidation: none within v1 lifetime. The list is
//     process-lifetime and never cleared, so records stay valid until
//     process exit. Borrowed nm_register_func / nm_priv values stay valid
//     only while the .node / addon image stays loaded (see unload limits).
//     String copies stay valid even if the caller frees its buffers.
//   Potential user-code or reentrancy points: nm_register_func is user
//     code (may run JS, allocate, reenter napi_module_register, or call
//     run_pending on the same env -> double-rejection path). The pending
//     mutex is NEVER held across user code (snapshot copies string data
//     under lock, then invokes without the lock).
//   Thread/context allowed: napi_module_register from any thread (mutex-
//     protected). Inspection helpers from any thread. run_pending only on
//     the env owner thread (validated via EntryGuard; wrong thread =>
//     napi_invalid_arg without entering a context, R-ENV-11). Invocation
//     runs inside the env context (EntryGuard needContext).
//   Release on success / failure / OOM / shutdown: no per-record release
//     in v1. OOM during register (string copy throws) drops the record
//     silently (void signature has no status; see napi_module_register
//     notes). OOM during snapshot in run_pending returns
//     napi_generic_failure without marking the env consumed.
//   Memory accounting / bound: unbounded in v1 (one record per
//     napi_module_register call; each addon registers once). No capacity
//     limit; a malicious loop registering millions of records grows the
//     list until OOM. Bounding / dedup is future work (documented limit).
//   Tests: test/nodeapi/probes/host_probe.cpp (direct-register phases),
//     plus dlopen static-constructor timing note in the report (ch-nodeapi
//     host fallback intentionally deferred; probe drives register +
//     run_pending directly as the task's allowed fallback).
//   Remaining uncertainty: whether nm_priv should be passed to the
//     register func in some Node versions (v8 abi: nm_priv is opaque and
//     not passed; recorded here as stored-but-unused).
//
// String-copy rule (task requirement):
//   nm_filename / nm_modname callers often pass string literals or
//   stack/heap buffers owned by the addon image. The provider copies the
//   bytes via std::string at register time. Later freeing, overwriting,
//   or dlclose-ing the caller's buffers cannot corrupt the stored copy.
//   NULL caller strings are stored as "" (empty) so inspectors never
//   dereference NULL. Inspection returns provider-owned c_str() pointers
//   (see get_pending_module borrow rule).
//
// Unload limits (v1, task requirement):
//   - The registry does NOT unload the .node and does NOT remove records
//     on dlclose. Removal on unload is NOT supported in v1.
//   - A record's nm_register_func / nm_priv remain only as borrowed code/
//     data pointers. If the host dlclose()s the addon while records
//     remain, a later run_pending on a new env would call freed code
//     (use-after-free). Hosts MUST NOT dlclose addons that self-registered
//     via napi_module_register while the process still creates new envs.
//     The test host exits after one run and never unloads (its dlclose
//     order is addon-then-provider-then-exit; records die with the process).
//     Production hosts must either treat deprecated-path addons as
//     unload-never or add an explicit unregister path (future lane).
//
// Host invocation design (CHOSEN: accessor + direct-invoke hybrid):
//   Rationale: the host glue for the symbol path (ch-nodeapi.cpp) already
//   owns scope + fresh exports + return-replaces-exports + global binding.
//   A pure accessor (host loops and calls each func itself) duplicates the
//   pending-gate / exception / rooting policy in every host. A pure
//   direct-invoke that creates exports internally would hide the exports
//   object from the host and force all deprecated modules to share one
//   exports (wrong for multi-module addons, see below). The hybrid keeps
//   exports creation in the host (fresh object per env, inside the host's
//   registration scope) and moves the policy (gating, per-env consumption,
//   chaining, rooting) into the provider:
//     1. Host creates env via chakra_nodeapi_create_env.
//     2. If chakra_nodeapi_pending_module_count() == 0 (or host prefers to
//        check), host uses the symbol path as today. Otherwise host opens
//        a registration handle scope, creates a fresh exports object, and
//        calls chakra_nodeapi_run_pending_registrations(env, exports,
//        &result) once.
//     3. Provider snapshots the registry, marks the env consumed BEFORE
//        invoking (reentrancy-safe), then invokes each snapshotted
//        nm_register_func(env, exports) in registration order, chaining
//        replacements: non-NULL return != input replaces exports for the
//        next module and becomes the final result; NULL keeps input.
//     4. Host applies return-replaces-exports uniformly (result is already
//        the chained value), binds result to the global, closes scope.
//   Multi-module case: one dlopen may register several napi_module records
//   (e.g. a file with two NAPI_MODULE_X expansions in older headers).
//   They share the single host exports object in registration order, each
//   seeing the previous module's replacement. This matches "one env per
//   context x addon registration" for the single-addon-per-process test
//   hosts; hosts needing isolated exports per module must iterate via
//   get_pending_module themselves (accessor is provided for that).
//   Inspection (count/get) is a pure read: it never marks consumption and
//   may be called repeatedly. Only run_pending consumes (per-env).
//
// Double-registration rejection (documented choice):
//   chakra_nodeapi_run_pending_registrations on the SAME env twice
//   returns napi_invalid_arg (not napi_generic_failure) with last_error
//   recorded. Reason: the misuse is a host caller error (same remedy as
//   double-delete / double-wrap in N1.3/N3.3 which use invalid_arg), not
//   an engine failure. The second call invokes nothing, allocates
//   nothing, and preserves any pending exception. Fresh envs (new
//   NodeEnv) are NOT double: each new env runs the snapshot once, so the
//   same deprecated addon can register into multiple sequential envs.
//   The consumed flag lives in NodeEnv (NodeApiEnv.h,
//   pending_registrations_consumed, owner-thread only); it is cleared
//   only by destroying the env.
//
// Fatal routing design (B1):
//   - napi_fatal_exception(env, err): gated on pending (status-mapping C
//     column: No; plan §3.10 throw discipline). If a JS exception is
//     pending, returns napi_pending_exception WITHOUT firing the service
//     and preserves the original. Strict Alive-only (Closing =>
//     napi_generic_failure via EntryGuard). NULL err => napi_invalid_arg
//     without firing. On success with a host fatal_exception service:
//     the exception value is JsAddRef-rooted for the duration of the
//     service call (R1: the host callback may run JS/GC; the provider's
//     scope root alone is not sufficient because the host may call JS
//     before reading the value), then JsRelease, ClearLastError,
//     napi_ok. The call does NOT set a pending exception. Without a
//     service (NULL) => print to stderr + std::abort() (never returns),
//     the plan §3.2 "NULL = abort policy".
//   - napi_fatal_error(location, len, message, len): noreturn, no env
//     parameter, no pending check (C column Yes). Prints bounded
//     "%.*s: %.*s" to stderr (cap 1<<20 each, NULL => "") + std::abort().
//     It does NOT consult any per-env fatal_exception service because
//     the signature carries no env to look the service up in; hosts
//     needing custom fatal handling must trap via the fatal_exception
//     service path (Node's napi_fatal_error analogously aborts). This
//     preserves Node's noreturn contract and the plan's "host fatal
//     path or abort; never returns" disposition. Pending-exception
//     semantics: fatal_error ignores pending state entirely (executes
//     with pending); fatal_exception gates as above. Both keep the
//     pending exception untouched (neither clears nor overwrites it on
//     the success path; abort path terminates the process).
//
// Thread-safety summary:
//   - napi_module_register, count, get_pending: thread-safe (mutex).
//   - run_pending, fatal_*: env-thread only (via EntryGuard / worker-
//     thread rejection per R-ENV-11). The registry mutex is not held
//     across user code.

#ifndef CHAKRA_NODEAPI_HOST_H_
#define CHAKRA_NODEAPI_HOST_H_

#include "js_native_api.h" /* napi_env, napi_value, napi_status (NAPI_VERSION=8) */
#include "node_api_types.h" /* napi_addon_register_func */

#include <stddef.h> /* size_t for the private host API */

#ifdef __cplusplus
extern "C" {
#endif

/* Snapshot of one pending-module record for host inspection.
 * nm_filename / nm_modname point into provider-owned string copies
 * (valid for process lifetime; host must NOT free them). All other
 * fields are value copies.
 *
 * Borrow rule: the returned pointers stay valid until process exit
 * even across further registrations (the registry uses deque storage,
 * whose element addresses are stable). The host must copy the strings
 * if it needs them beyond its current call sequence; no JsAddRef-style
 * rooting applies (these are native copies, not JS values). */
typedef struct chakra_nodeapi_pending_module_info {
  int nm_version;
  unsigned int nm_flags;
  const char* nm_filename;
  napi_addon_register_func nm_register_func;
  const char* nm_modname;
  void* nm_priv;
} chakra_nodeapi_pending_module_info;

/* Total pending-module records registered process-wide (monotonic,
 * thread-safe). 0 until the first napi_module_register. */
size_t chakra_nodeapi_pending_module_count(void);

/* Inspect record `index` (0-based, registration order).
 * Returns napi_ok + fills *out, or napi_invalid_arg if out==NULL or
 * index out of range. Pure read: never marks consumption, never requires
 * an env, never touches last_error (no env to record into). Thread-safe.
 * out->nm_filename / out->nm_modname borrow provider copies (see above).
 * NULL caller strings at register time appear here as "" (non-NULL). */
napi_status chakra_nodeapi_get_pending_module(
    size_t index, chakra_nodeapi_pending_module_info* out);

/* Invoke all snapshotted nm_register_func values for `env`.
 *
 * Preconditions (validate-first, plan §3.10):
 *   - env must be a live Alive env on the owner thread (else
 *     napi_invalid_arg for NULL/dead/wrong-thread, or
 *     napi_generic_failure when Closing, via EntryGuard).
 *   - exports and result must be non-NULL (else napi_invalid_arg).
 *   - No JS exception may be pending (else napi_pending_exception
 *     WITHOUT invoking and WITHOUT marking consumed, so the host can
 *     clear and retry).
 *   - The env must not have consumed pending registrations before
 *     (else napi_invalid_arg; see double-registration rule).
 *
 * On success: marks the env consumed BEFORE the first invocation
 * (reentrant calls from inside a register func see the invalid_arg),
 * invokes each snapshotted func as func(env, exports) inside the env
 * context, chaining replacements (non-NULL return != input becomes the
 * next input and the final *result; NULL keeps input). The final
 * replacement (if any) is rooted into the innermost scope before
 * returning. *result is always set on the ok path. Returns napi_ok
 * (even when the snapshot is empty: *result == exports, nothing
 * invoked). If any func leaves a JS exception pending, invocation
 * stops, *result is set to the current exports, and returns
 * napi_pending_exception (the exception stays pending; the env stays
 * consumed). C++ exceptions from addon code are caught and reported as
 * napi_generic_failure (no throw crosses the extern "C" boundary).
 *
 * The host must have opened a registration handle scope and created
 * `exports` inside it (as ch-nodeapi does for the symbol path) so the
 * input and the chained result stay rooted across binding.
 */
napi_status chakra_nodeapi_run_pending_registrations(napi_env env,
                                                     napi_value exports,
                                                     napi_value* result);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* CHAKRA_NODEAPI_HOST_H_ */
