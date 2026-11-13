//-------------------------------------------------------------------------------------------------------
// Copyright (c) ChakraCore Project Contributors. All rights reserved.
// Licensed under the MIT license. See LICENSE.txt file in the project root for full license information.
//-------------------------------------------------------------------------------------------------------
//
// Instance data + cleanup hooks (lib/NodeApi/NodeApiLifetime.*) -- N1.4.
//
// Plan: doc/node-api-plan.md §6 card N1.4, §3.6 steps 3-5, §3.3 layout.
// Semantics: doc/node-api-semantics.md §1 (R-ENV-5/7/8/9/10; probes
// P1-1/P1-2/P1-7) + §3.3-§3.4 rooting (no JS roots here). Vendored
// oracles checked: test_instance_data, test_finalizer,
// test_general/testEnvCleanup.js (see report). No dedicated
// test_cleanup_hook / test_async_cleanup_hook exists in the vendored
// js-native-api tree; §3.6 order stands (see report).
//
// Ownership record (memory-management.md §3):
//
//   (a) Async pump config + violation (MEDIUM-2, new).
//     Resource: AsyncPumpConfig {pump, host_ctx, max_iters,
//       used_iters, violated} in a provider-global map
//       keyed by env pointer value (g_pump_configs).
//     Domain/allocator: provider native heap (unordered_map buckets);
//       no JS values, no roots. Env itself is the NodeEnv (provider
//       heap); this map holds a COPY of the config, not the env.
//     Owner/transfer: provider (Lifetime TU) owns the map; host sets
//       via Lifetime_SetAsyncPump(env, pump, ctx, max) before destroy.
//       The pump callback + ctx are HOST-owned (provider only invokes
//       pump(ctx) on the owner thread; never frees ctx).
//     Invalidation: next Set for the same env value (address reuse)
//       overwrites; Lifetime_ClearAsyncPump erases; retained after
//       destroy so the probe can query Lifetime_AsyncViolation(env_value)
//       post-destroy by pointer value WITHOUT dereferencing the freed
//       NodeEnv. No invalidation by GC/detach (no JS).
//     Thread/context: Set on owner thread (checked by caller before
//       destroy); RunAsyncCleanupHooks calls pump on the owner thread
//       with no global lock held (R-ENV-12) and may call
//       napi_remove_async_cleanup_hook; context does NOT need to be
//       entered for the pump itself (host pump enters its own context).
//     Release: erase on Clear or overwrite; map entries are O(1) small
//       native memory per env ever seen, freed at process exit. If a
//       host never clears, entries accumulate per env value (probe
//       always clears; real host should clear after destroy or rely
//       on address-reuse overwrite). No root count contribution.
//     Tests: gate_probe phases A-C (self-remove 0 iters, non-remove
//       bound 5 + violation + timeout event, deferred 2 iters).
//
//   (b) Hook lists (sync + async) + retired async handles.
//     Resource: NodeEnv::cleanup_hooks / async_cleanup_hooks /
//       retired_async_handles vectors + each heap
//       napi_async_cleanup_hook_handle__ record.
//     Domain/allocator: provider native heap (std::vector buckets +
//       new for handle records); handle records outlive the vectors
//       entries' removal until destroy end (deferred-free, below).
//     Owner/transfer: NodeEnv owns the vectors; the provider owns each
//       handle record. napi_add_* appends (env owner thread only);
//       napi_remove_* unlinks (sync: erase one (fun,arg) pair; async:
//       erase + retire + erase from global live map); destroy steps
//       3/4 fire and retire; Lifetime_DiscardEnvState frees retired +
//       any remaining handle records and clears the vectors before
//       NodeEnv delete (step 7).
//     Aliases: user holds only the (fun,arg) pair / remove_handle value;
//       the handle value is meaningful only while in the live map
//       (remove consults the map by value before dereferencing).
//       After remove/destroy the handle bytes remain readable until
//       destroy end but are NOT a valid handle (second remove =>
//       napi_invalid_arg without dereferencing state).
//     Invalidation: remove (unlink), destroy step 3/4 (fire),
//       step 7 free. A removed sync hook never fires; a removed async
//       hook never fires; a fired hook is unlinked before/around the
//       call so it fires exactly once (pop-before-call for sync;
//       peek-then-unlink for async self-remove).
//     Thread/context: owner thread only (checked per entry). No Js*
//       calls, no JS roots; no context enter needed (basic_env level).
//     Release on failure: add with NULL fun/hook/handle => invalid_arg
//       + no allocation; OOM on handle new => generic_failure + no
//       registration; remove-unknown => invalid_arg + no mutation.
//     Accounting: unbounded hook count bounded by addon use; each hook
//       is O(1) small native memory, freed at destroy. No root count
//       contribution (no JS values held).
//
//   (c) Instance data (ptr + finalize + hint per env).
//     Resource: NodeEnv::instance_data triple.
//     Domain/allocator: triple lives inside NodeEnv (provider heap).
//       The DATA POINTER itself is USER-owned: provider never
//       allocates/frees it, only stores it and calls finalize.
//     Owner/transfer: user allocates data; napi_set_instance_data
//       transfers only the triple (overwrite WITHOUT calling the old
//       finalizer -- the documented Node wart, R-ENV-8/P1-7); the
//       user remains responsible for the old data's storage (leak on
//       replacement is by design, matching Node). Destroy step 5
//       calls the CURRENT finalize once, then clears the triple.
//     Invalidation: replacement (old triple dropped, never finalized),
//       destroy step 5 (finalize once + clear), NodeEnv delete.
//       data==nullptr is a storable value (indistinguishable from
//       unset on get -- matches Node).
//     Thread/context: owner thread only; finalize runs on the env
//       thread with a live (Closing) env, no global locks held
//       (R-ENV-12).
//     Tests: P1-7 wart probe (replace then destroy: old 0 calls, new
//       1 call); P1-1 ordering probe (finalizer after hooks).
//
// Closing policy (plan §3.6 step 1 + R-ENV-4, choice recorded here):
//   While Closing (destroy steps 2-6), get_instance_data and the two
//   remove functions succeed (hooks/finalizers need get to read the
//   data being torn down, and async hooks need remove as their
//   completion signal); set/add functions return
//   napi_generic_failure (no new registration during teardown). The
//   probe asserts this from inside a sync hook.
//   Parity (MEDIUM-3): refs delete_reference and get_reference_value
//   now also succeed in Closing (NodeApiRefs.cpp
//   validate_env_allow_closing), matching lifetime get/remove.
//   create_reference stays strict Alive-only. The shared EntryGuard
//   (NodeApiEnv.h, needContext true for JS ops) centralizes the
//   validate -> context -> pending gate; Lifetime native entries
//   use ValidateLifetime (no context) because they issue no Js* calls.
//   ContextGuard is reused once via EntryGuard (no duplication).

#ifndef CHAKRA_NODEAPI_LIFETIME_H_
#define CHAKRA_NODEAPI_LIFETIME_H_

#include "NodeApiEnv.h"

namespace nodeapi {

// Teardown steps 3-5, called from chakra_nodeapi_destroy_env in exact
// order. Each must be called with the env in Closing state, on the
// owner thread, with NO provider-global lock held, and must not hold
// any lock across user callbacks.
//   RunSyncCleanupHooks  (step 3): pop-back LIFO until empty; each
//     hook fires exactly once; a hook may remove a still-pending hook
//     (suppressing it) via napi_remove_env_cleanup_hook.
//   RunAsyncCleanupHooks (step 4): reverse-registration order; each
//     hook fires exactly once as fn(handle, data). Then pump-until-
//     remove per hook (plan §3.6 step 4): after firing, the provider
//     calls the host-injected pump (Lifetime_SetAsyncPump) up to
//     `max_iters` times (default 1000, probe may set 5-10 for fast
//     fail) until the hook calls napi_remove_async_cleanup_hook.
//     A hook that never removes hits the bound and is recorded as a
//     HOST CONTRACT VIOLATION (Lifetime_AsyncViolation true, teardown
//     event "async_pump_timeout"), never silently retired as success
//     and never hangs forever. The handle is then force-retired so
//     destroy completes, but the violation stays observable for the
//     probe to assert. Without an injected pump, a no-op pump with
//     the same bound is used (self-removing hooks still pass with
//     0 iterations; non-self-removing hooks violate).
//   RunInstanceFinalizer (step 5): call current finalize once if
//     present, then clear the triple.
//   DiscardEnvState (step 7 pre-free): free retired + remaining async
//     handle records and clear all three hook vectors. Calls no user
//     code. Safe to call exactly once per env at end of destroy.
//     Pump config + violation record are retained by pointer value
//     after discard so the probe can query via
//     Lifetime_AsyncViolation(env_value) post-destroy.
// ----------------------------------------------------------------
// Async pump host injection (MEDIUM-2):
//   Lifetime_SetAsyncPump(env, pump, ctx, max_iters):
//     env must be a live NodeEnv (checked by pointer value, no
//     dereference of dead values). pump==nullptr selects the
//     default no-op pump (still bounded). max_iters <=0 selects
//     the default 1000. The pump is called ONLY on the owner
//     thread during step 4, with no provider-global lock held,
//     and may call napi_remove_async_cleanup_hook (completion
//     signal) and/or run JS/uv work (host-owned event loop).
//   Lifetime_AsyncViolation(env_value): true iff step 4 hit the
//     bound for any hook of that env (pointer value used as key,
//     safe after destroy without dereferencing).
//   Lifetime_AsyncPumpIterations(env_value): total pump calls
//     made during step 4 for that env (0 if no pump needed).
//   Lifetime_ClearAsyncPump(env_value): erase the per-env pump
//     record (probe cleanup; also called implicitly on next set).
void Lifetime_SetAsyncPump(NodeEnv* env, void (*pump)(void* host_ctx),
                           void* host_ctx, int max_iters);
bool Lifetime_AsyncViolation(const void* env_value);
int Lifetime_AsyncPumpIterations(const void* env_value);
void Lifetime_ClearAsyncPump(const void* env_value);

void Lifetime_RunSyncCleanupHooks(NodeEnv* env);
void Lifetime_RunAsyncCleanupHooks(NodeEnv* env);
void Lifetime_RunInstanceFinalizer(NodeEnv* env);
void Lifetime_DiscardEnvState(NodeEnv* env);

} /* namespace nodeapi */

#endif /* CHAKRA_NODEAPI_LIFETIME_H_ */
