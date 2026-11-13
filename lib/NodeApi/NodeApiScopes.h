//-------------------------------------------------------------------------------------------------------
// Copyright (c) ChakraCore Project Contributors. All rights reserved.
// Licensed under the MIT license. See LICENSE.txt file in the project root for full license information.
//-------------------------------------------------------------------------------------------------------
//
// Handle scopes + escapable scopes (plan doc/node-api-plan.md §3.5, task N1.2;
// normative rules doc/node-api-semantics.md §2, R-SC-1..10).
//
// Scope model (R-SC-1): per-env stack of scopes; each scope is a list of
// rooted JsValueRef slots (JsAddRef on store, JsRelease on scope close, plan
// §3.4 R1/R2) plus escapable/escaped flags. The bottom stack entry is the
// default scope (R-SC-7), created with the env and closed only at destroy
// step 6 (NodeApiScopes_DestroyForEnv). Every API returning a new napi_value
// roots it into the innermost open scope (R-SC-1); this module provides
// NodeApiScopes_RootIntoInnermost for those lanes (N2.x) and the RootedValue
// RAII helper for code that needs an explicit, movable root handle.
//
// Public napi_* entries implemented here (§3.5, §4 Table A3): open/close
// handle scope, open/close escapable handle scope, escape handle. They are
// EXEMPT from the pending-exception gate (R-SC-8, plan §3.5: the header docs
// list scope close among entries callable with a pending exception) and
// therefore run without the N1.5 shared guard's pending check. They DO
// validate env state via the reconciled N1.1 guard (nodeapi::CheckAlive:
// live + Alive + owner thread, R-ENV-4/R-ENV-11; Closing => generic
// failure, wrong thread => debug assert + release napi_invalid_arg) and
// never mutate scope state cross-thread (HIGH-3). Last-error recording
// for OOM generic-failure paths goes through nodeapi::SetLastError
// (N1.1 storage); the N1.5 shared guard still owns the pending check.
//
// RECONCILED N1.1 INTERFACE (lib/NodeApi/NodeApiEnv.* has landed):
// This module deliberately knows nothing about NodeEnv's layout. N1.1
// provides (any storage layout behind them):
//
//   nodeapi::CheckAlive(napi_env env);      // N1.1 thread/state guard
//   void nodeapi::SetLastError(env, status, message);  // N1.1 storage
//   NodeApiScopeState* NodeApiEnv_ScopeState(napi_env env);
//     Returns the per-env scope state (created once at env creation,
//     destroyed at env destroy). NULL for NULL/dead env (checked by
//     pointer VALUE via the live registry, R-ENV-10, never dereferenced).
//   int NodeApiEnv_IsAlive(napi_env env);   // retained for compatibility;
//     ValidateEnv now uses CheckAlive (which subsumes the state check);
//     direct callers of Depth/IsOpen do not go through ValidateEnv.
//   NodeApiScopes_CreateState();      // defined here
//   void NodeApiScopes_DestroyForEnv(napi_env env);     // defined here
//
// NodeApiScopeState is opaque to N1.1 (it stores one pointer). The
// scopes probe shim (test/nodeapi/probes/scopes_shim.*) implements the
// two env accessors plus shim nodeapi::CheckAlive/SetLastError over a
// probe env struct (owner_thread + live-set by value) for self-contained
// driver runs that do not link NodeApiEnv.cpp. When the scopes probe
// migrates to the real env (chakra_nodeapi_create_env), the shim
// accessors are deleted (see scopes_shim.h); the real NodeApiEnv.cpp
// implementations take over with identical signatures.
//
// Contract any entry-guard (N1.5) must uphold for this module:
//   - env thread only (R3 / R-ENV-11). Rooting (JsAddRef/JsRelease) needs a
//     thread context on the calling thread; wrong-thread calls fail in the
//     guard before reaching here.
//   - the env's Chakra context is current on entry (plan §3.2). This module
//     does not enter/restore contexts; it asserts in debug builds.
//
// Ownership record (memory-management.md §3; semantics Appendix A.1):
//
//   Resource / creation call: Scope (opaque struct, one per open scope)
//     heap-allocated by NodeApiScopes_OpenImpl, stored in the env's stack.
//   Domain and allocator: provider native heap (new/delete via the C++
//     allocator bound to the provider; backing lifetime == env lifetime).
//     Never Recycler memory; never Zig/CRT-freed across modules.
//   Physical allocation: one Scope per napi_open_* call plus its roots vector.
//   Owner; transfer point: NodeEnv stack owns scopes until close. Value roots
//     transfer API-return -> innermost scope at creation (RootIntoInnermost);
//     escape transfers inner-scope -> parent-scope without AddRef/Release
//     (entry moves lists; JsAddRef count unchanged).
//   Borrow invalidation: scope close (LIFO or mismatch-with-release, R-SC-6),
//     env destroy step 6 (all scopes, incl. default), env Closing (new public
//     entries fail), runtime disposal (R5; roots die with the runtime).
//   Release on close (all paths reach it): close releases every remaining
//     root (JsRelease); mismatch close likewise releases (no leak on misuse).
//     Destroy (step 6) closes the whole stack top-down incl. default.
//   Thread/context: env thread with the env context entered (see above).
//   Reentrancy: scope lists are not touched by user code except through
//     napi_* entries on the env thread. RootedValue destruction and scope
//     close both consult the live stack before releasing, so a RootedValue
//     destroyed after its scope closed is a no-op (no double release).
//   Accounting: slot count is part of the R-REF-14 TOTAL
//     (chakra_nodeapi_test_root_count = scope roots + live ref-record
//     roots; see test-hook comment below). Baseline recorded at state
//     creation. After HIGH-4 unification, napi_get_reference_value roots
//     live in this scope stack (not in a refs-owned list), so the total's
//     scope part already includes ref-get results; the refs TU contributes
//     only its per-ref strong/weak roots via NodeApiRefs_RefRootCount.
//   Tests: test/nodeapi/probes/scopes_probe.cpp (P2-*); root counter asserts
//     per probe. Probe runner: tools/nodeapi/run_probe.sh.
//   Remaining uncertainty: AMB-SC-1/2 status names (N0.6 to confirm from
//     header docs; implemented as spec choices); AMB-SC-4 (escape of a value
//     not rooted in the escapable scope => invalid_arg, no flag set).
//   Language note: implemented in C++ (GNU++17 via `zig c++`, matching the
//     N0.3 stub/host) until PR-4 finalizes the provider language; the C
//     surface (napi_* + helpers) is language-agnostic, so a Zig N1.1 can
//     embed NodeApiScopeState* opaquely. If PR-4 lands Zig, this TU ports
//     mechanically with no ABI change.

#ifndef CHAKRA_NODEAPI_SCOPES_H_
#define CHAKRA_NODEAPI_SCOPES_H_

#include "js_native_api.h" /* napi_status, napi_handle_scope, ... (NAPI_VERSION=8) */
#include "ChakraCommon.h" /* JsValueRef, JsRef */

#ifdef __cplusplus
extern "C" {
#endif

/* --------------------------------------------------------------------------
 * Per-env scope state (opaque to N1.1; see assumed interface above).
 * Created by NodeApiScopes_CreateState() at env creation (bottom entry is
 * the default scope, R-SC-7). Freed by the env owner AFTER calling
 * NodeApiScopes_DestroyForEnv() (which releases every root and frees every
 * scope struct). Env owner may also consult NodeApiScopes_Depth() before
 * destroy for diagnostics.
 * ------------------------------------------------------------------------ */
struct NodeApiScopeState;

/* Creates fresh scope state with the default scope pushed (R-SC-7).
 * The returned pointer is provider-owned; see DestroyForEnv. Never NULL
 * on success (aborts on allocation failure in debug; the provider has no
 * recoverable OOM path for env creation). */
NodeApiScopeState* NodeApiScopes_CreateState(void);

/* Destroy step 6 (plan §3.6): releases every root in every scope
 * (top-down, including the default scope) and frees every Scope struct.
 * The NodeApiScopeState itself is NOT freed here (freed by the env owner
 * immediately after via NodeApiScopes_FreeState). Idempotent-safe on
 * already-destroyed state.
 * Must be called on the env thread with the env context entered. */
void NodeApiScopes_DestroyForEnv(napi_env env);

/* Frees the NodeApiScopeState itself (after DestroyForEnv has released
 * every root). The env owner calls this exactly once at destroy step 6
 * (N1.1 integration: the scope state lives in NodeEnv::scope_state).
 * Null-safe. Must be called on the env thread. */
void NodeApiScopes_FreeState(NodeApiScopeState* state);

/* Depth of the scope stack, INCLUDING the default scope (>= 1 for a live
 * env). Returns 0 for a NULL/dead env (no state). This is the value the
 * N2.4 thunk records at callback entry as its entry_depth (R-SC-10). */
size_t NodeApiScopes_Depth(napi_env env);

/* ----- N1.1 hooks (implemented by NodeApiEnv.cpp; the probe shim
 * implements the same signatures over its probe env struct). ----- */
NodeApiScopeState* NodeApiEnv_ScopeState(napi_env env);
int /*bool*/ NodeApiEnv_IsAlive(napi_env env);

/* --------------------------------------------------------------------------
 * napi_* entries (N1.2). All are allowlisted w.r.t. pending exceptions
 * (R-SC-8): they never early-return napi_pending_exception. NULL env /
 * NULL out-param / NULL or foreign scope handle => napi_invalid_arg.
 * ------------------------------------------------------------------------ */

/* Pushes a non-escapable scope (R-SC-2). */
napi_status NAPI_CDECL napi_open_handle_scope(napi_env env,
                                              napi_handle_scope* result);

/* Closes the innermost scope. Out-of-order (named scope not innermost) or
 * wrong-kind (named scope is escapable) => napi_handle_scope_mismatch, and
 * the named scope's roots are STILL released (R-SC-6). Never unwinds the
 * rest of the stack. */
napi_status NAPI_CDECL napi_close_handle_scope(napi_env env,
                                               napi_handle_scope scope);

/* Pushes an escapable scope (R-SC-3). */
napi_status NAPI_CDECL napi_open_escapable_handle_scope(
    napi_env env, napi_escapable_handle_scope* result);

/* Closes the escapable scope, releasing its REMAINING roots; the escaped
 * value (if any) lives in the parent and is not released (R-SC-5).
 * Wrong-kind (named scope is a plain scope) => mismatch, with release
 * (AMB-SC-2 spec choice, pending N0.6 confirmation). */
napi_status NAPI_CDECL napi_close_escapable_handle_scope(
    napi_env env, napi_escapable_handle_scope scope);

/* Moves the escapee's root entry from the escapable scope to its parent
 * (R-SC-3). Escape-once (R-SC-4): a second escape on the same scope =>
 * napi_escape_called_twice and moves nothing; the first escape stands.
 * On success *result == escapee. Non-escapable or foreign scope, NULL
 * escapee/result => napi_invalid_arg (AMB-SC-1 spec choice); escapee not
 * rooted in the named scope => napi_invalid_arg with no flag set, so a
 * later valid escape can still succeed (AMB-SC-4 resolution in N1.2). */
napi_status NAPI_CDECL napi_escape_handle(napi_env env,
                                          napi_escapable_handle_scope scope,
                                          napi_value escapee,
                                          napi_value* result);

/* --------------------------------------------------------------------------
 * Internal helpers for later lanes (all C-linkage; Zig-safe).
 * ------------------------------------------------------------------------ */

/* Roots `value` into the innermost open scope (R-SC-1) and returns the
 * value unchanged. Every value-returning API lane (N2.x) calls this
 * exactly once per returned value before returning. Returns napi_ok on
 * success; napi_invalid_arg for NULL env/value. Requires the env context
 * entered (plan §3.2). The caller must have just produced `value` with no
 * intervening JS execution (R1 ordering: create -> root, synchronously). */
napi_status NodeApiScopes_RootIntoInnermost(napi_env env, napi_value value);

/* Roots `value` into the caller's scope (the scope that was innermost when
 * the current innermost... "implicit" scope was opened). Used by the N2.4
 * thunk for the callback return (R-SC-9): the return must survive the
 * implicit scope's close, so it is rooted into the scope BELOW the
 * implicit one BEFORE the implicit scope closes. Returns napi_ok; if the
 * stack is too shallow (< 2 scopes) returns napi_handle_scope_mismatch
 * (a provider bug: surfaced, never a leak/crash). */
napi_status NodeApiScopes_RootIntoCallerScope(napi_env env, napi_value value);

/* Opens a plain implicit scope (thunk + host registration glue, R-SC-10).
 * The `implicit` flag exists for diagnostics only; close/balance semantics
 * are the plain-scope ones. The caller owns the returned handle and MUST
 * close it on every return path (see EnforceImplicitBalance). */
napi_status NodeApiScopes_OpenImplicit(napi_env env, napi_handle_scope* result);

/* True iff `scope` is currently on the env's scope stack (pointer-identity
 * scan; never dereferences the handle, so this stays valid even after the
 * scope was closed and its struct freed, unless the freed address was
 * reallocated as a new Scope, which next_scope_id disambiguates for the
 * RootedValue path). The thunk uses this to avoid double-closing an
 * implicit scope the callback already closed (underflow arm, R-SC-10). */
int /*bool*/ NodeApiScopes_IsOpen(napi_env env, napi_handle_scope scope);

/* Balance verification for the N2.4 thunk (R-SC-10, probe P8-5).
 * `entry_depth` is Depth() recorded at callback entry, BEFORE opening the
 * implicit scope. Balanced means Depth == entry_depth + 1 (the implicit
 * scope still open on top). Returns napi_ok if balanced.
 * If unbalanced (extra scopes left open by the callback) returns
 * napi_handle_scope_mismatch and unwinds (closing + releasing) any scopes
 * above the implicit one, so the thunk can still close its implicit scope
 * afterwards. If the callback closed the implicit scope itself (underflow),
 * returns napi_handle_scope_mismatch and leaves the stack otherwise
 * untouched; the thunk reports and continues (never leaks/crashes). */
napi_status NodeApiScopes_EnforceImplicitBalance(napi_env env,
                                                 size_t entry_depth);

/* Root-counter test hooks (semantics R-REF-14, unified total).
 * Debug/probe builds only: these symbols exist ONLY when the provider
 * is compiled with CHAKRA_NODEAPI_TEST_HOOKS defined (run_probe.sh /
 * run_refs_probe.sh define it; N1.6 must keep them out of release
 * exports). Never napi_* symbols.
 *
 * chakra_nodeapi_test_root_count(env): size_t
 *   THE normative R-REF-14 total: every scope root-list entry across
 *   the whole scope stack (default scope included, ref-get results
 *   included via HIGH-4 unification) PLUS every live ref-record root
 *   (one per strong ref: JsAddRef on the referent; one per weak ref:
 *   R6 JsAddRef on the weak handle; immortal-at-0 counts as strong).
 *   N3.x will extend the same total additively with wrap/deferred/
 *   callback roots (probes assert equality across a probe, never
 *   absolutes). Returns (size_t)-1 for a NULL/dead env so probe
 *   misuse is visible. Per-TU helper NodeApiRefs_RefRootCount (refs
 *   TU) supplies the ref-record part; the scopes probe shim stubs it
 *   to 0 (no refs in that env). The refs TU's
 *   chakra_nodeapi_test_ref_root_count is debugging-only (refs part);
 *   probes assert against THIS total.
 * chakra_nodeapi_test_root_baseline(env): size_t
 *   Total count recorded immediately after env creation (0: default
 *   scope starts empty, no refs yet). Assertions compare an env's total
 *   against its own baseline, not against constants.
 * chakra_nodeapi_test_scope_depth(env): size_t  (extra probe hook)
 *   Same as NodeApiScopes_Depth (0 for dead env). */
#ifdef CHAKRA_NODEAPI_TEST_HOOKS
size_t chakra_nodeapi_test_root_count(napi_env env);
size_t chakra_nodeapi_test_root_baseline(napi_env env);
size_t chakra_nodeapi_test_scope_depth(napi_env env);
#endif /* CHAKRA_NODEAPI_TEST_HOOKS */

#ifdef __cplusplus
} /* extern "C" */
#endif

/* --------------------------------------------------------------------------
 * RootedValue: RAII rooted-slot handle for later lanes (N2.x/N3.x).
 *
 * Use when a value must stay rooted across C++ calls that are not tied to
 * a scope lifetime (e.g. thunk argv copies, deferred records). Prefer
 * NodeApiScopes_RootIntoInnermost for ordinary API returns (lifetime ==
 * scope). RootedValue lifetime must not span a scope close or an escape
 * of the same value (the value's slot lives in the scope; the wrapper
 * tracks the slot's identity and becomes a no-op once the slot is gone).
 *
 *   napi_value v = ...;
 *   NodeApiScopes::RootedValue root(env, v);  // JsAddRef + slot in innermost
 *   ... may run JS (slots persist) ...
 *   root.reset();  // unroots; wrapper inert afterwards. Destructor unroots
 *                  // if reset() was not called.
 *
 * Thread/context: construct/destroy on the env thread with the env context
 * entered (JsAddRef/JsRelease requirements, R3). Destruction after the
 * scope closed (or after escape moved the slot) is safe: no-op, no double
 * release (the slot's absence is detected by identity, not by dereferencing
 * freed structs). Move-only (copying would double-release).
 * Zig equivalent: root via NodeApiScopes_RootIntoInnermost and track the
 * value + scope depth at root time; unroot is implicit at scope close.
 * (Zig code keeping a slot across a scope close needs an explicit
 * reference lane (N1.3), never a dangling RootedValue.)
 * ------------------------------------------------------------------------ */
#ifdef __cplusplus
namespace NodeApiScopes {

class RootedValue {
 public:
  RootedValue(napi_env env, napi_value value);
  ~RootedValue();

  RootedValue(const RootedValue&) = delete;
  RootedValue& operator=(const RootedValue&) = delete;
  RootedValue(RootedValue&& other) noexcept;
  RootedValue& operator=(RootedValue&& other) noexcept;

  napi_value get() const { return value_; }
  bool active() const { return active_; }

  /* Unroots now (remove slot + JsRelease) and returns the raw value.
   * Afterwards this wrapper is inert; the returned value is unrooted
   * (caller must re-root or drop it). */
  napi_value release();

  /* Unroots now; wrapper inert afterwards. */
  void reset();

 private:
  napi_env env_ = nullptr;
  napi_value value_ = nullptr;
  void* scope_ = nullptr; /* erased Scope* for the impl TU */
  unsigned long long scope_id_ = 0;
  unsigned long long entry_id_ = 0;
  bool active_ = false;
};

} /* namespace NodeApiScopes */
#endif /* __cplusplus */

#endif /* CHAKRA_NODEAPI_SCOPES_H_ */
