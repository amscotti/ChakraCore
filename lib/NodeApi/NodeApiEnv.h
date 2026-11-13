//-------------------------------------------------------------------------------------------------------
// Copyright (c) ChakraCore Project Contributors. All rights reserved.
// Licensed under the MIT license. See LICENSE.txt file in the project root for full license information.
//-------------------------------------------------------------------------------------------------------
//
// NodeEnv core (lib/NodeApi/NodeApiEnv.*) -- minimal N1.1 skeleton for N1.4.
//
// Plan: doc/node-api-plan.md §3.3 (NodeEnv layout), §3.6 (destroy order),
// §5.1 (layout owned by N1.1). Semantics: doc/node-api-semantics.md §1
// (R-ENV-*), esp. R-ENV-5/6/7/9/10. Ownership: doc/memory-management.md §3.
//
// What this slice provides (and what it deliberately does not):
//   - Full §3.6 destroy steps 1 (state -> Closing) and 7 (Dead + free +
//     tombstone) plus step 2 / step 6 PLACEHOLDER markers so the ordering
//     probe (P1-1) can compare the whole sequence verbatim today.
//   - Steps 3-5 delegated to lib/NodeApi/NodeApiLifetime.* (N1.4).
//   - Minimal NodeEnv holding ONLY the N1.4 fields + create-time fields
//     (runtime/context borrows, owner thread, state, api version, filename
//     copy, services/host_ctx) plus placeholders for N1.2/N1.3/N3.2 state.
//     Scope, ref, wrap, callback, and last-error state land with their
//     tasks; they extend this layout, they must not reorder steps 3-5.
//
// Assumptions (N1.1 not yet landed in this worktree; recorded per task
// brief "design to plan §3.6 and state assumptions"):
//   - A1 Layout: future N1.1 must preserve the fields below and the
//     step 3-5 wiring. Adding scopes/refs/wraps later is additive.
//   - A2 Language: this slice is C++ (as is the N0.3 test stub). Plan D5
//     prescribes Zig 0.17 pending PR-4; the C++ here is the mechanical
//     stand-in so the N1.4 semantics + probe can be proved before the
//     Zig port. No provider code calls Js* (see A3), so the port is
//     surface-identical.
//   - A3 Context guard: lifetime entries are basic_env level and native
//     only -- they never call the engine, so they need no
//     JsSetCurrentContext enter/exit (§3.2). N1.1 now provides
//     nodeapi::ContextGuard (RAII save/set/restore); destroy uses it
//     for the teardown window (scopes teardown releases roots via
//     JsRelease and needs the env context current), and N1.5's shared
//     entry guard reuses it for every JS-calling entry. The host keeps
//     the context current for the env's life (as ch-nodeapi.cpp does),
//     so the guard is normally a no-op save/restore.
//   - A4 Pending gate: omitted (N1.5 owns it). N0.6 classifies all six
//     N1.4 functions as "executes with pending", so omitting the gate
//     is behaviorally equivalent here.
//   - A5 Tombstone + generation + address-reuse (Wave-1 review
//     HIGH-2 fix; semantics R-ENV-9/10):
//     * Live registry: pointer value -> {NodeEnv*, generation}.
//       Generation is a process-monotonic 64-bit counter issued at
//       create (never 0), stored both in the registry record and in
//       NodeEnv::generation. LookupLiveEnv verifies the two agree
//       after the map hit; mismatch (heap corruption / use-after-free
//       reuse for a non-Env object) fails without returning the env.
//     * Dead registry (tombstones): pointer value -> last live
//       generation, BOUNDED to kDeadEnvCap (256) entries with FIFO
//       eviction. On create at a tombstoned address the old tombstone
//       is displaced (erased) and the new generation supersedes it.
//       Evicted tombstones are forgotten: a later double-destroy of
//       an evicted address still fails (live-map miss), but the old
//       generation is no longer recoverable for diagnostics.
//     * Irreducible ABA (documented limit, plan A5): without a
//       host-passed generation, a caller holding only the raw pointer
//       cannot distinguish destroy-old + create-new-at-same-address
//       from a live call on the new env; pointer-only lookup then
//       succeeds on the NEW env by construction (required so 1000x
//       sequential create/destroy churn stays usable). It is made
//       DETECTABLE via LookupLiveEnvWithGeneration and the
//       chakra_nodeapi_test_env_generation hook: a caller that
//       remembers the old generation sees a mismatch, and the
//       generation-aware lookup fails WITHOUT dereferencing the new
//       env. Pointer-only callers remain aliased; that residual is
//       the plan-A5 limit, not a silent success.

#ifndef CHAKRA_NODEAPI_ENV_H_
#define CHAKRA_NODEAPI_ENV_H_

#include "NodeApiHostApi.h" /* napi_env, hook/finalize types, host services */
#include "js_native_api.h" /* napi_status, napi_finalize (NAPI_VERSION=8) */
#include "js_native_api_types.h" /* napi_extended_error_info */

#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

/* Opaque scope-state pointer (defined by NodeApiScopes.*, N1.2).
 * Env stores it without including NodeApiScopes.h in this header
 * (avoids a header cycle); NodeApiEnv.cpp includes it to call
 * NodeApiScopes_CreateState / DestroyForEnv / FreeState. */
struct NodeApiScopeState;
/* Opaque refs-state pointer (defined by NodeApiRefs.*, N1.3).
 * Same additive pattern as scope_state (N1.1 assumption A1):
 * stored here without including NodeApiRefs.h; NodeApiEnv.cpp
 * includes it to call NodeApiRefs_CreateState / DestroyForEnv /
 * FreeState for create + destroy step 6a. */
struct NodeApiRefsState;
/* Opaque promise-state pointer (defined by NodeApiPromise.*, N3.4).
 * Same additive pattern as scope/refs state (N1.1 assumption A1):
 * stored here without including NodeApiPromise.h; NodeApiEnv.cpp
 * includes it to call NodeApiPromise_CreateState / DestroyForEnv /
 * FreeState for create + destroy step 6. */
struct NodeApiPromiseState;
/* Opaque script-state pointer (defined by NodeApiScript.*, N3.5).
 * Same additive pattern; NodeApiEnv.cpp includes it to call
 * NodeApiScript_CreateState / DestroyForEnv / FreeState for create
 * + destroy step 6. */
struct NodeApiScriptState;

namespace nodeapi {

// Env lifecycle state (plan §3.3 / §3.6 step 1,7). Only Alive accepts new
// public calls; Closing is the teardown window; Dead-before-free is only
// observable via the debug state query (P1-4b) because the pointer is
// freed immediately after (R-ENV-10).
enum class EnvState : std::uint8_t { Alive = 0, Closing = 1, Dead = 2 };

// Minimal NodeEnv (plan §3.3 sketch, N1.4 subset + create-time fields).
//
// Ownership record (memory-management.md §3):
//   Resource: NodeEnv (this struct as one allocation).
//   Domain/allocator: provider native heap, allocated by
//     chakra_nodeapi_create_env (new), freed only by
//     chakra_nodeapi_destroy_env step 7 (delete). Never in Recycler
//     memory and never a JsValueRef root.
//   Owner/transfer: provider owns the env; host owns the
//     JsRuntimeHandle/JsContextRef (borrowed here, never disposed by
//     provider). Host calls create/destroy; provider holds the env
//     between them.
//   Invalidation: Alive -> Closing -> Dead at destroy; pointer is dead
//     after destroy step 7 (host must not pass it again, except the
//     destroy-twice probe which passes the saved value to prove the
//     registry check runs without dereferencing it).
//   Thread/context: owner_thread only; no use from worker threads (R3).
//     Lifetime entries reject cross-thread use with napi_invalid_arg
//     (R-ENV-11); destroy from wrong thread is assert + no-op (R-ENV-6).
//   Release: destroy step 7 is the sole release authority; every
//     provider sub-container (hook lists, retired handles) is drained
//     before delete. Leak accounting is via the probe's create/destroy
//     churn (no separate allocator accounting in this slice;
//     N1.1 G1 adds it).
struct NodeEnv {
  JsRuntimeHandle runtime = nullptr;
  JsContextRef context = nullptr;
  std::thread::id owner_thread;
  EnvState state = EnvState::Alive;
  std::uint32_t module_api_version = 8;
  std::string module_filename; /* provider-owned copy */
  chakra_nodeapi_host_services services{};
  void* host_ctx = nullptr;

  // --- N1.4 lifetime state (plan §3.3 fields) ---
  struct InstanceData {
    void* ptr = nullptr; /* user-owned, provider never frees it */
    napi_finalize finalize = nullptr;
    void* hint = nullptr;
    bool has = false;
  };
  InstanceData instance_data;

  struct CleanupHook {
    napi_cleanup_hook fun = nullptr;
    void* arg = nullptr;
  };
  std::vector<CleanupHook> cleanup_hooks; /* registration order */
  std::vector<napi_async_cleanup_hook_handle> async_cleanup_hooks;
  std::vector<napi_async_cleanup_hook_handle> retired_async_handles;

  // Debug/test teardown log sink. nullptr in any non-probe build use.
  // Set only via chakra_nodeapi_test_set_teardown_log (below). The
  // destroy sequence emits "closing"/"wrap_drain"/
  // "refs_scopes_release"/"dead" markers through it; hooks/finalizers
  // log their own events through the same probe log (see probe source).
  void (*teardown_sink)(void* ctx, const char* event) = nullptr;
  void* teardown_ctx = nullptr;

  // Generation issued at create from the registry's monotonic
  // 64-bit counter (never 0). Mirrors the live-registry record;
  // LookupLiveEnv verifies the mirror after a map hit. Set once
  // before publication, never mutated afterwards (except delete).
  std::uint64_t generation = 0;

  // --- N1.1 env core (added by N1.1; §3.3 remainder) ---
  // Opaque pointer to NodeApiScopes' ScopeState (N1.2). Created at
  // env creation (default scope, R-SC-7), destroyed at destroy
  // step 6 (NodeApiScopes_DestroyForEnv + FreeState). nullptr until
  // create completes; never null for a live env after create.
  NodeApiScopeState* scope_state = nullptr;

  // Opaque pointer to NodeApiRefs' RefsState (N1.3). Created at env
  // creation, destroyed at destroy step 6a (NodeApiRefs_DestroyForEnv +
  // FreeState). nullptr until create completes; never null for a live
  // env after create. Additive field (assumption A1); no reorder of
  // the create-time or N1.4 fields above.
  NodeApiRefsState* refs_state = nullptr;

  // Opaque pointer to NodeApiPromise's PromiseState (N3.4). Created at
  // env creation, destroyed at destroy step 6 (NodeApiPromise_DestroyForEnv
  // + FreeState). nullptr until create completes; never null for a live
  // env after create. Additive field (assumption A1).
  NodeApiPromiseState* promise_state = nullptr;

  // Opaque pointer to NodeApiScript's ScriptState (N3.5). Created at env
  // creation, destroyed at destroy step 6 (NodeApiScript_DestroyForEnv +
  // FreeState). nullptr until create completes; never null for a live
  // env after create. Additive field (assumption A1).
  NodeApiScriptState* script_state = nullptr;

  // --- N4.1 deprecated-path registration consumption (NodeApiHost) ---
  // False at create; set true on the first successful entry into
  // chakra_nodeapi_run_pending_registrations for this env (before any
  // nm_register_func invocation, so reentrant calls see the flag).
  // Second call on the same env returns napi_invalid_arg
  // (double-registration rejected, NodeApiHost.h design). Owner-thread
  // only; no lock (env is single-threaded). Additive field (A1).
  bool pending_registrations_consumed = false;

  // Per-env last error (plan §3.10 storage half; N1.5 owns the
  // mapping discipline). `message` is owned (fixed buffer);
  // `info.error_message` borrows it (refresh on every record).
  // N1.1 never clears on success (see N1.5 disagreement with
  // N0.6/C in the N1.1 report).
  struct LastError {
    napi_status status = napi_ok;
    char message[256] = {};
    napi_extended_error_info info{};
  };
  LastError last_error{};
};

// Registry lookup by pointer value WITHOUT dereferencing the pointer
// first (R-ENV-10: freed env must never be validated through). Returns
// true + *out (live env) iff the value is a currently-live NodeEnv
// and the registry generation matches NodeEnv::generation (the two
// mirrors agree). Never crashes on a dead/fabricated value: it only
// consults the provider-global live set (mutex-protected).
bool LookupLiveEnv(void* env_value, NodeEnv** out);

// Generation-aware lookup: same as LookupLiveEnv but additionally
// requires the live-registry generation to equal expected_generation.
// A mismatch (the classic ABA: caller remembers the pre-destroy
// generation, address has since been reused by a new env) returns
// false WITHOUT dereferencing the new env (the failure is decided
// from the registry record alone, before any NodeEnv read).
// Pointer-only LookupLiveEnv cannot provide this check (the caller
// supplies no generation) and remains aliased after reuse per the
// recorded irreducible limit (A5); this entry point makes the reuse
// DETECTABLE for callers/probes that remembered the generation.
// Hidden visibility: not exported from the release dylib; probes
// link NodeApiEnv.cpp directly.
#if defined(__GNUC__)
__attribute__((visibility("hidden")))
#endif
bool LookupLiveEnvWithGeneration(void* env_value,
                                 std::uint64_t expected_generation,
                                 NodeEnv** out);

// Bounded tombstone capacity (HIGH-2). The dead registry never
// holds more than this many pointer->generation entries; FIFO
// eviction keeps it bounded (see .cpp). Exposed for probes.
inline constexpr size_t kDeadEnvCap = 256;

// Emit a teardown marker through the env's sink (no-op if none).
// Called with no provider-global lock held.
void EmitTeardownEvent(NodeEnv* env, const char* event);

// Test/probe introspection (debug-only, declared here, defined in
// NodeApiEnv.cpp). Never used for control flow by the provider.
inline const char* DescribeState(EnvState s) {
  switch (s) {
  case EnvState::Alive: return "alive";
  case EnvState::Closing: return "closing";
  case EnvState::Dead: return "dead";
  }
  return "unknown";
}

/* Context enter/exit guard (plan §3.2, N1.1): saves the caller's
 * current context (JsGetCurrentContext), sets the env's context
 * (JsSetCurrentContext) if different, and restores the previous
 * on destruction. The caller must have validated the env (alive +
 * owner thread) BEFORE constructing the guard so the wrong-thread
 * path never enters a context (semantics R-ENV-11). If the env is
 * NULL or JsGetCurrentContext fails, the guard is inert
 * (entered() == false). N1.5's shared entry guard reuses this
 * RAII helper; N1.1 uses it in destroy and exposes it for the
 * JS-calling lanes (N2.x). */
class ContextGuard {
 public:
  explicit ContextGuard(napi_env env);
  ~ContextGuard();
  ContextGuard(const ContextGuard&) = delete;
  ContextGuard& operator=(const ContextGuard&) = delete;
  bool entered() const { return entered_; }

 private:
  JsContextRef prev_;
  bool entered_;
};

/* Validation helper used by every public entry (N1.1; N1.5 reuses).
 * Returns napi_ok when the env is live + Alive + on the owner
 * thread. Otherwise returns the failing status WITHOUT entering a
 * context:
 *   - NULL / not-live (dead or fabricated value, checked via
 *     LookupLiveEnv by pointer VALUE only) => napi_invalid_arg.
 *   - wrong thread (semantics R-ENV-11) => debug assert with env
 *     pointer + owner/current thread in the diagnostic, release
 *     napi_invalid_arg. Never mutates env state cross-thread and
 *     never records last_error cross-thread.
 *   - Closing/Dead (state != Alive) => napi_generic_failure with
 *     last_error recorded (semantics R-ENV-4).
 * Callers that also validate output pointers should check those
 * first (plan §3.10 validate-first) and then call this. */
napi_status CheckAlive(napi_env env);

/* Teardown-compatible validation: like CheckAlive but allows
 * Closing (only Dead-before-free and invalid thread / dead-env
 * fail). Used by teardown-compatible getters/removers that must
 * succeed during destroy steps 2-6 (plan §3.6 + R-ENV-4 exception,
 * lifetime get/remove, ref delete/get, exception queries).
 * Callers that need strict Alive-only use CheckAlive above. The
 * EntryGuard below wraps this + context + pending check. */
napi_status CheckAliveAllowClosing(napi_env env);

/* Last-error storage (N1.1). Records `status` + `message`
 * (truncated to fit) into the env's owned buffer and refreshes
 * info.error_message to borrow it. The caller must hold a
 * validated Alive env on the owner thread. N1.5 owns which calls
 * record and when success clears. */
void SetLastError(napi_env env, napi_status status, const char* message);

/* Last-error clearing (N0.6/C column, AMB-STATUS-1 decided):
 * a successful call CLEARS last_error (writes napi_ok). Evidence:
 * doc/node-api-status-mapping.md §4 AMB-STATUS-1 (B "up until a
 * Node-API function is called" + C napi_clear_last_error on every
 * success path) + vendored test_general.c createNapiError /
 * testNapiErrorCleanup (failure then get_last_error_info ==
 * status, then after a success get_last_error_info == napi_ok).
 * The ONLY exception is napi_get_last_error_info itself: its
 * success does NOT clear the error it returns (otherwise the
 * caller could not read it); the next success after the read
 * clears. ClearLastError acquires the same liveness/thread
 * checks as SetLastError: no-op for NULL/dead/wrong-thread. */
void ClearLastError(napi_env env);

/* Shared N1.5-style entry guard (N1.1 -> N1.5 handoff):
 * reuses ContextGuard (no duplication of JsSetCurrentContext
 * enter/exit) + thread/state validation + pending gate.
 *   allowClosing: when true, Closing is permitted (teardown-
 *     compatible getters/removers); when false, Closing => generic
 *     failure (strict Alive-only).
 *   allowPending: when true, the entry executes with a pending
 *     exception (C-column Yes rows, 90 functions); when false, it
 *     returns napi_pending_exception without executing (C-column
 *     No rows, 55 functions). The check uses JsHasException and
 *     records napi_pending_exception in last_error.
 *   needContext: whether the entry needs the env context entered
 *     (JS-calling entries / exception queries). Basic_env native
 *     entries pass false and the guard is inert (no Js calls).
 * Construction validates FIRST (never enters a context on the
 * wrong-thread / dead-env path). On success ok()==true and
 * entered() reports whether the context was entered; on failure
 * status() holds the napi_status to return. The caller must check
 * ok() before doing any work; the guard's destructor restores the
 * previous context. */
class EntryGuard {
 public:
  EntryGuard(napi_env env, bool allowClosing = false,
             bool allowPending = true, bool needContext = false);
  ~EntryGuard();
  EntryGuard(const EntryGuard&) = delete;
  EntryGuard& operator=(const EntryGuard&) = delete;
  bool ok() const { return status_ == napi_ok; }
  napi_status status() const { return status_; }
  bool entered() const { return guard_ && guard_->entered(); }

 private:
  napi_status status_ = napi_ok;
  napi_env env_ = nullptr;
  std::unique_ptr<ContextGuard> guard_;
  bool allowClosing_ = false;
  bool allowPending_ = true;
};

/* Teardown entry points (plan §3.6 steps 2-6). Destroy calls
 * these in §3.6 order; later tasks fill the bodies without
 * reordering. Steps 3-5 are Lifetime (N1.4). Step 2 (finalizer
 * drain, N3.2) is wired in chakra_nodeapi_destroy_env and step
 * 6c-d (wraps, N3.3) via TeardownWraps; step 6a/6b are real:
 *   - scopes: real (NodeApiScopes, N1.2) via TeardownScopes;
 *   - refs: real (NodeApiRefs, N1.3) via TeardownRefs +
 *     nodeapi_refs_teardown_all.
 *   - promise/script: real (N3.4/N3.5) via TeardownPromiseScript.
 *   - wraps: real (NodeApiWrap, N3.2/N3.3) via TeardownWraps ->
 *     nodeapi_wrap_teardown. */
void TeardownScopes(NodeEnv* env); /* step 6b */
void TeardownRefs(NodeEnv* env); /* step 6a (real, N1.3) */
void TeardownPromiseScript(NodeEnv* env); /* step 6 (real, N3.4/N3.5) */
void TeardownWraps(NodeEnv* env); /* steps 6c-d (real, N3.2/N3.3) */

/* Provider allocator accounting (N1.1, semantics P1-6).
 * Hidden visibility: NOT exported from release
 * libChakraNodeApi (mirrors R-REF-14 for the debug hook).
 * Probes that need accounting link NodeApiEnv.cpp directly
 * (same binary) or query via the private test hook build. */
#if defined(__GNUC__)
__attribute__((visibility("hidden")))
#endif
size_t DebugLiveEnvCount() noexcept;
#if defined(__GNUC__)
__attribute__((visibility("hidden")))
#endif
size_t DebugTotalEnvCount() noexcept;
// Dead-registry size (tombstones retained, <= kDeadEnvCap).
// Hidden visibility: not exported from the release dylib.
#if defined(__GNUC__)
__attribute__((visibility("hidden")))
#endif
size_t DebugDeadEnvCount() noexcept;

} /* namespace nodeapi */

#ifdef __cplusplus
extern "C" {
#endif

// --- Debug/test-only introspection (P1-4b / P1-2 / P1-1 support) ---
// These are NOT napi_* symbols and must not be listed in the N1.6
// exports inventory; they exist solely so probes can observe the
// state machine without dereferencing freed memory or UAF.
//   - set_teardown_log: attach the ordering-probe sink. No-op if the
//     env value is not live.
//   - is_live: true iff the pointer value names a currently-live env
//     (checked without dereferencing). False after destroy (dead) and
//     for fabricated values.
//   - state_name: "alive"/"closing"/"dead-before-free" for a live env;
//     "dead"/"unknown" for a non-live value (no dereference).
//   - env_generation / dead_count / dead_generation: registry
//     generation + tombstone introspection for the HIGH-2 ABA
//     probe (test/nodeapi/probes/env_addr_reuse_probe.cpp).
//     env_generation returns the live generation for a live env,
//     0 for dead/fabricated values (no dereference). dead_generation
//     returns the tombstoned generation for a dead pointer value,
//     0 if absent/evicted. dead_count is the current tombstone
//     count (<= nodeapi::kDeadEnvCap).
void chakra_nodeapi_test_set_teardown_log(
    napi_env env, void (*sink)(void* ctx, const char* event), void* ctx);
bool chakra_nodeapi_test_is_live(napi_env env);
const char* chakra_nodeapi_test_state_name(napi_env env);
std::uint64_t chakra_nodeapi_test_env_generation(napi_env env);
size_t chakra_nodeapi_test_dead_count(void);
std::uint64_t chakra_nodeapi_test_dead_generation(napi_env env);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* CHAKRA_NODEAPI_ENV_H_ */
