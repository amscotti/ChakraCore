//-------------------------------------------------------------------------------------------------------
// Copyright (c) ChakraCore Project Contributors. All rights reserved.
// Licensed under the MIT license. See LICENSE.txt file in the project root for full license information.
//-------------------------------------------------------------------------------------------------------
//
// N1.4 implementation: napi_set/get_instance_data, sync cleanup hooks
// (add/remove, LIFO), async cleanup hooks (add/remove). See
// NodeApiLifetime.h for the ownership record and Closing policy.
// Status mapping for these six functions per doc/node-api-status-mapping.md
// (N0.6, C-implementation column): all six EXECUTE with a pending
// exception (no pending gate); type-mismatch is n/a (no JS values --
// only NULL checks, which yield invalid_arg). Pending checks are
// therefore omitted here (N1.5 owns the guard; omitting it for these
// six is behaviorally equivalent).

#include "NodeApiLifetime.h"
#include "NodeApiEnv.h"
#include "js_native_api.h"
#include "node_api.h"

#include <algorithm>
#include <cassert>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <new>
#include <thread>
#include <unordered_map>

// Opaque async-hook handle layout (provider-defined). The public type
// is `typedef struct napi_async_cleanup_hook_handle__*
// napi_async_cleanup_hook_handle` (node_api_types.h). The handle IS
// the record: hook(handle, data) receives its own handle for
// self-remove. Deferred-free until destroy end (see header record (a));
// global live map (by value) precedes any dereference.
struct napi_async_cleanup_hook_handle__ {
  nodeapi::NodeEnv* owner = nullptr;
  napi_async_cleanup_hook fn = nullptr;
  void* data = nullptr;
  bool completed = false;
};

namespace nodeapi {

// Global async-handle live registry: handle value -> owner env.
// Consulted BY VALUE before dereferencing any handle (mirrors the env
// registry in NodeApiEnv.cpp). Entries are added on add, removed on
// remove (including self-remove during destroy step 4) and at discard.
// Never held across user callbacks. `static` = internal linkage (used
// only in this TU) while remaining in namespace nodeapi so the
// extern "C" entries below can name them qualified.
static std::mutex g_async_mutex;
static std::unordered_map<void*, NodeEnv*> g_async_live;

static bool LookupAsyncHandle(void* handle_value, NodeEnv** out_owner) {
  if (handle_value == nullptr || out_owner == nullptr) return false;
  std::lock_guard<std::mutex> lk(g_async_mutex);
  auto it = g_async_live.find(handle_value);
  if (it == g_async_live.end()) return false;
  *out_owner = it->second;
  return true;
}

static void RegisterAsyncHandle(void* handle_value, NodeEnv* owner) {
  std::lock_guard<std::mutex> lk(g_async_mutex);
  g_async_live[handle_value] = owner;
}

static void UnregisterAsyncHandle(void* handle_value) {
  std::lock_guard<std::mutex> lk(g_async_mutex);
  g_async_live.erase(handle_value);
}

// Per-env async pump config (MEDIUM-2). Keyed by env POINTER VALUE
// (never dereferencing dead values). The record is created by
// Lifetime_SetAsyncPump and retained after destroy (so the probe
// can query violation post-destroy via pointer value) until
// Lifetime_ClearAsyncPump or the next set for the same value
// (address reuse) overwrites it. The pump runs on the owner
// thread during step 4 with no global lock held.
struct AsyncPumpConfig {
  void (*pump)(void* host_ctx) = nullptr;
  void* host_ctx = nullptr;
  int max_iters = 1000;
  int used_iters = 0;
  bool violated = false;
};
static std::mutex g_pump_mutex;
static std::unordered_map<const void*, AsyncPumpConfig> g_pump_configs;

static AsyncPumpConfig GetPumpConfig(NodeEnv* env) {
  std::lock_guard<std::mutex> lk(g_pump_mutex);
  const void* key = static_cast<const void*>(env);
  auto it = g_pump_configs.find(key);
  if (it != g_pump_configs.end()) return it->second;
  AsyncPumpConfig d;
  d.pump = nullptr;
  d.host_ctx = nullptr;
  d.max_iters = 1000;
  return d;
}

static void SetPumpUsed(NodeEnv* env, int used) {
  std::lock_guard<std::mutex> lk(g_pump_mutex);
  const void* key = static_cast<const void*>(env);
  auto& cfg = g_pump_configs[key];
  if (cfg.max_iters == 0 && cfg.pump == nullptr && cfg.host_ctx == nullptr &&
      cfg.used_iters == 0 && !cfg.violated) {
    cfg.max_iters = 1000;
  }
  cfg.used_iters = used;
}

static void MarkViolation(NodeEnv* env) {
  std::lock_guard<std::mutex> lk(g_pump_mutex);
  const void* key = static_cast<const void*>(env);
  auto& cfg = g_pump_configs[key];
  if (cfg.max_iters == 0 && cfg.pump == nullptr && cfg.host_ctx == nullptr &&
      cfg.used_iters == 0 && !cfg.violated) {
    cfg.max_iters = 1000;
  }
  cfg.violated = true;
}

// Resolve a node_api_basic_env value to a live NodeEnv without
// dereferencing dead/fabricated values (R-ENV-10). Also checks the
// owner thread (R-ENV-11). Returns nullptr if not live or wrong
// thread; caller maps to napi_invalid_arg (debug asserts).
static NodeEnv* ResolveEnv(node_api_basic_env env_raw) {
  if (env_raw == nullptr) return nullptr;
  NodeEnv* env = nullptr;
  if (!LookupLiveEnv(const_cast<void*>(static_cast<const void*>(env_raw)),
                     &env)) {
    return nullptr;
  }
  if (env->owner_thread != std::this_thread::get_id()) {
    assert(false && "NodeApiLifetime: napi_* call from wrong thread (R-ENV-11)");
    return nullptr;
  }
  return env;
}

// Validate for lifetime entries with Closing parity.
// allowClosing=true for getters/removers (teardown-compatible);
// false for setters/adders. Never dereferences dead values.
// Records last_error on failure when an env is available.
static napi_status ValidateLifetime(node_api_basic_env env_raw,
                                    bool allowClosing, NodeEnv** out_env) {
  if (out_env) *out_env = nullptr;
  if (env_raw == nullptr) return napi_invalid_arg;
  NodeEnv* env = nullptr;
  if (!LookupLiveEnv(const_cast<void*>(static_cast<const void*>(env_raw)),
                     &env)) {
    return napi_invalid_arg;
  }
  if (env->owner_thread != std::this_thread::get_id()) {
    // Wrong thread (R-ENV-11): debug assert, release invalid_arg.
#if !defined(NDEBUG)
    std::fprintf(stderr,
                 "NodeApiLifetime: wrong thread env=%p owner=%zu cur=%zu\n",
                 static_cast<void*>(env), std::hash<std::thread::id>{}(env->owner_thread), std::hash<std::thread::id>{}(std::this_thread::get_id()));
    assert(false);
#endif
    return napi_invalid_arg;
  }
  if (allowClosing) {
    if (env->state == EnvState::Dead) {
      SetLastError(reinterpret_cast<napi_env>(env), napi_generic_failure,
                   "generic failure: env is dead");
      return napi_generic_failure;
    }
  } else {
    if (env->state != EnvState::Alive) {
      SetLastError(reinterpret_cast<napi_env>(env), napi_generic_failure,
                   "generic failure: env is closing or dead");
      return napi_generic_failure;
    }
  }
  if (out_env) *out_env = env;
  return napi_ok;
}

// --- Teardown steps ---

void Lifetime_RunSyncCleanupHooks(NodeEnv* env) {
  // Pop-back LIFO, exactly once. No lock held across the call; the
  // vector is owner-thread only (add/remove assert owner thread, so
  // no concurrent mutation is possible here).
  while (!env->cleanup_hooks.empty()) {
    NodeEnv::CleanupHook hook = env->cleanup_hooks.back();
    env->cleanup_hooks.pop_back();
    if (hook.fun != nullptr) {
      hook.fun(hook.arg);
    }
  }
}

void Lifetime_SetAsyncPump(NodeEnv* env, void (*pump)(void* host_ctx),
                           void* host_ctx, int max_iters) {
  if (env == nullptr) return;
  // Consult by pointer value only (no dereference of a dead value is
  // needed to record config; the env value was just validated by the
  // caller as live when Set is called before destroy).
  std::lock_guard<std::mutex> lk(g_pump_mutex);
  const void* key = static_cast<const void*>(env);
  AsyncPumpConfig cfg;
  cfg.pump = pump;
  cfg.host_ctx = host_ctx;
  cfg.max_iters = max_iters > 0 ? max_iters : 1000;
  cfg.used_iters = 0;
  cfg.violated = false;
  g_pump_configs[key] = cfg;
}

bool Lifetime_AsyncViolation(const void* env_value) {
  if (env_value == nullptr) return false;
  std::lock_guard<std::mutex> lk(g_pump_mutex);
  auto it = g_pump_configs.find(env_value);
  if (it == g_pump_configs.end()) return false;
  return it->second.violated;
}

int Lifetime_AsyncPumpIterations(const void* env_value) {
  if (env_value == nullptr) return 0;
  std::lock_guard<std::mutex> lk(g_pump_mutex);
  auto it = g_pump_configs.find(env_value);
  if (it == g_pump_configs.end()) return 0;
  return it->second.used_iters;
}

void Lifetime_ClearAsyncPump(const void* env_value) {
  if (env_value == nullptr) return;
  std::lock_guard<std::mutex> lk(g_pump_mutex);
  g_pump_configs.erase(env_value);
}

void Lifetime_RunAsyncCleanupHooks(NodeEnv* env) {
  // Pump-until-remove (plan §3.6 step 4, MEDIUM-2). Reverse-
  // registration order is preserved (always take .back()).
  // Per hook: fire once as fn(handle, data), then while it has
  // not self-removed, call the host pump (bounded) until it
  // does. The bound is per-hook max_iters (default 1000; probe
  // sets 5-10 to fail fast). A hook that never removes hits the
  // bound: violation is recorded (never silently retired as
  // success), a teardown event "async_pump_timeout" is emitted,
  // and the handle is then force-retired so destroy completes
  // without hanging. Self-removing hooks (sync or via pump-
  // scheduled tasks) pass with 0..max pump iterations.
  while (!env->async_cleanup_hooks.empty()) {
    napi_async_cleanup_hook_handle current =
        env->async_cleanup_hooks.back();
    // current is live (in the vector, hence in the map): reading
    // its fields is safe. Copy before the call because the hook
    // may remove (unlink/retire) it during the call -- but NOT
    // free it (deferred-free), so the bytes stay valid.
    napi_async_cleanup_hook fn =
        current == nullptr ? nullptr : current->fn;
    void* data = current == nullptr ? nullptr : current->data;
    if (fn != nullptr) {
      fn(current, data);
    }
    // If the hook self-removed synchronously, it is already gone
    // from the vector (unlinked/retired/unregistered by
    // napi_remove_async_cleanup_hook). Done with this hook.
    auto it = std::find(env->async_cleanup_hooks.begin(),
                        env->async_cleanup_hooks.end(), current);
    if (it == env->async_cleanup_hooks.end()) {
      continue;
    }
    // Otherwise pump until it self-removes or the bound hits.
    AsyncPumpConfig cfg = GetPumpConfig(env);
    int max_iters = cfg.max_iters > 0 ? cfg.max_iters : 1000;
    int iter = 0;
    while (iter < max_iters) {
      // Re-check before each pump iteration: another iteration
      // may have removed it via a scheduled task the previous
      // pump drained.
      it = std::find(env->async_cleanup_hooks.begin(),
                     env->async_cleanup_hooks.end(), current);
      if (it == env->async_cleanup_hooks.end()) break;
      if (cfg.pump != nullptr) {
        cfg.pump(cfg.host_ctx);
      } else {
        // Default no-op pump: no host work injected. The loop
        // still bounds (hook that only checks removal without
        // host work would have already self-removed; a hook
        // relying on external work with no pump will violate
        // after the bound, which is the documented host-contract
        // failure, not a hang).
      }
      ++iter;
      // Re-find after pump: the pump may have called
      // napi_remove_async_cleanup_hook (directly or via a task).
      it = std::find(env->async_cleanup_hooks.begin(),
                     env->async_cleanup_hooks.end(), current);
      if (it == env->async_cleanup_hooks.end()) break;
    }
    SetPumpUsed(env, iter);
    if (std::find(env->async_cleanup_hooks.begin(),
                  env->async_cleanup_hooks.end(), current) !=
        env->async_cleanup_hooks.end()) {
      // Bound hit: host contract violation. Record, emit, force
      // retire (so destroy proceeds), but NOT as silent success.
      MarkViolation(env);
      EmitTeardownEvent(env, "async_pump_timeout");
      SetLastError(reinterpret_cast<napi_env>(env),
                   napi_generic_failure,
                   "generic failure: async cleanup hook did not self-remove within pump bound");
      auto it2 = std::find(env->async_cleanup_hooks.begin(),
                           env->async_cleanup_hooks.end(), current);
      if (it2 != env->async_cleanup_hooks.end()) {
        env->async_cleanup_hooks.erase(it2);
        if (current != nullptr) {
          current->completed = true;
          UnregisterAsyncHandle(static_cast<void*>(current));
          env->retired_async_handles.push_back(current);
        }
      }
    }
    // Else: self-removed during pumping (already handled above).
  }
}

void Lifetime_RunInstanceFinalizer(NodeEnv* env) {
  const bool has = env->instance_data.has;
  napi_finalize finalize = env->instance_data.finalize;
  void* data = env->instance_data.ptr;
  void* hint = env->instance_data.hint;
  if (has && finalize != nullptr) {
    // Live (Closing) env; no locks held (R-ENV-12).
    finalize(reinterpret_cast<napi_env>(env), data, hint);
  }
  env->instance_data.ptr = nullptr;
  env->instance_data.finalize = nullptr;
  env->instance_data.hint = nullptr;
  env->instance_data.has = false;
}

void Lifetime_DiscardEnvState(NodeEnv* env) {
  // Free retired + any remaining handle records. Calls no user code.
  // Pump config/violation are retained by pointer value after
  // discard (map keyed by env value) so the probe can query
  // Lifetime_AsyncViolation(env_value) post-destroy without
  // dereferencing the freed NodeEnv. The next Set for a reused
  // address overwrites, and Lifetime_ClearAsyncPump erases.
  for (auto* h : env->retired_async_handles) {
    UnregisterAsyncHandle(static_cast<void*>(h));
    delete h;
  }
  for (auto* h : env->async_cleanup_hooks) {
    UnregisterAsyncHandle(static_cast<void*>(h));
    delete h;
  }
  env->retired_async_handles.clear();
  env->async_cleanup_hooks.clear();
  env->cleanup_hooks.clear();
}

} /* namespace nodeapi */

extern "C" {

// --- Instance data (R-ENV-8 / P1-7 wart) ---

napi_status NAPI_CDECL napi_set_instance_data(node_api_basic_env env_raw,
                                               void* data,
                                               napi_finalize finalize_cb,
                                               void* finalize_hint) {
  nodeapi::NodeEnv* env = nullptr;
  napi_status vv = nodeapi::ValidateLifetime(env_raw, /*allowClosing=*/false, &env);
  if (vv != napi_ok) return vv;
  if (env == nullptr) return napi_invalid_arg;
  // Overwrite WITHOUT calling the previous finalizer (Node wart).
  // The old user data pointer is never freed by the provider.
  env->instance_data.ptr = data;
  env->instance_data.finalize = finalize_cb;
  env->instance_data.hint = finalize_hint;
  env->instance_data.has = true;
  nodeapi::ClearLastError(reinterpret_cast<napi_env>(env));
  return napi_ok;
}

napi_status NAPI_CDECL napi_get_instance_data(node_api_basic_env env_raw,
                                              void** data) {
  nodeapi::NodeEnv* env = nullptr;
  // Teardown-compatible getter: Closing succeeds (finalizers/hooks
  // read the data being torn down; see header Closing policy and
  // keep parity with ref getters below).
  napi_status vv = nodeapi::ValidateLifetime(env_raw, /*allowClosing=*/true, &env);
  if (vv != napi_ok) return vv;
  if (env == nullptr) return napi_invalid_arg;
  if (data == nullptr) {
    nodeapi::SetLastError(reinterpret_cast<napi_env>(env), napi_invalid_arg,
                          "get_instance_data: data is NULL");
    return napi_invalid_arg;
  }
  // Alive and Closing both readable (finalizers/hooks read the data
  // being torn down; see header Closing policy).
  *data = env->instance_data.ptr;
  nodeapi::ClearLastError(reinterpret_cast<napi_env>(env));
  return napi_ok;
}

// --- Sync cleanup hooks (LIFO at destroy step 3) ---

napi_status NAPI_CDECL napi_add_env_cleanup_hook(
    node_api_basic_env env_raw, napi_cleanup_hook fun, void* arg) {
  nodeapi::NodeEnv* env = nullptr;
  napi_status vv = nodeapi::ValidateLifetime(env_raw, /*allowClosing=*/false, &env);
  if (vv != napi_ok) return vv;
  if (env == nullptr) return napi_invalid_arg;
  if (fun == nullptr) {
    nodeapi::SetLastError(reinterpret_cast<napi_env>(env), napi_invalid_arg,
                          "add_env_cleanup_hook: fun is NULL");
    return napi_invalid_arg;
  }
  try {
    env->cleanup_hooks.push_back({fun, arg});
  } catch (...) {
    nodeapi::SetLastError(reinterpret_cast<napi_env>(env), napi_generic_failure,
                          "add_env_cleanup_hook: OOM");
    return napi_generic_failure;
  }
  nodeapi::ClearLastError(reinterpret_cast<napi_env>(env));
  return napi_ok;
}

napi_status NAPI_CDECL napi_remove_env_cleanup_hook(
    node_api_basic_env env_raw, napi_cleanup_hook fun, void* arg) {
  nodeapi::NodeEnv* env = nullptr;
  // Teardown-compatible remover: Closing succeeds (hooks may
  // suppress a still-pending peer during step 3).
  napi_status vv = nodeapi::ValidateLifetime(env_raw, /*allowClosing=*/true, &env);
  if (vv != napi_ok) return vv;
  if (env == nullptr) return napi_invalid_arg;
  if (fun == nullptr) {
    nodeapi::SetLastError(reinterpret_cast<napi_env>(env), napi_invalid_arg,
                          "remove_env_cleanup_hook: fun is NULL");
    return napi_invalid_arg;
  }
  // Exact (fun, arg) match; erase ONE instance (first in
  // registration order -- matches "remove one hook" semantics;
  // duplicate (fun,arg) registrations need one remove each).
  for (auto it = env->cleanup_hooks.begin();
       it != env->cleanup_hooks.end(); ++it) {
    if (it->fun == fun && it->arg == arg) {
      env->cleanup_hooks.erase(it);
      nodeapi::ClearLastError(reinterpret_cast<napi_env>(env));
      return napi_ok;
    }
  }
  nodeapi::SetLastError(reinterpret_cast<napi_env>(env), napi_invalid_arg,
                        "remove_env_cleanup_hook: unknown hook");
  return napi_invalid_arg;
}

// --- Async cleanup hooks (registration/removal; completion at step 4) ---

napi_status NAPI_CDECL napi_add_async_cleanup_hook(
    node_api_basic_env env_raw, napi_async_cleanup_hook hook, void* arg,
    napi_async_cleanup_hook_handle* remove_handle) {
  nodeapi::NodeEnv* env = nullptr;
  napi_status vv = nodeapi::ValidateLifetime(env_raw, /*allowClosing=*/false, &env);
  if (vv != napi_ok) return vv;
  if (env == nullptr) return napi_invalid_arg;
  if (hook == nullptr || remove_handle == nullptr) {
    nodeapi::SetLastError(reinterpret_cast<napi_env>(env), napi_invalid_arg,
                          "add_async_cleanup_hook: NULL arg");
    return napi_invalid_arg;
  }
  auto* handle =
      new (std::nothrow) napi_async_cleanup_hook_handle__();
  if (handle == nullptr) {
    nodeapi::SetLastError(reinterpret_cast<napi_env>(env), napi_generic_failure,
                          "add_async_cleanup_hook: OOM");
    return napi_generic_failure;
  }
  handle->owner = env;
  handle->fn = hook;
  handle->data = arg;
  handle->completed = false;
  try {
    env->async_cleanup_hooks.push_back(handle);
  } catch (...) {
    delete handle;
    nodeapi::SetLastError(reinterpret_cast<napi_env>(env), napi_generic_failure,
                          "add_async_cleanup_hook: OOM");
    return napi_generic_failure;
  }
  nodeapi::RegisterAsyncHandle(static_cast<void*>(handle), env);
  *remove_handle = handle;
  nodeapi::ClearLastError(reinterpret_cast<napi_env>(env));
  return napi_ok;
}

napi_status NAPI_CDECL napi_remove_async_cleanup_hook(
    napi_async_cleanup_hook_handle remove_handle) {
  if (remove_handle == nullptr) return napi_invalid_arg;
  // Consult the live map by value BEFORE dereferencing: unknown /
  // fabricated / double-removed handles return invalid_arg safely.
  nodeapi::NodeEnv* owner = nullptr;
  if (!nodeapi::LookupAsyncHandle(static_cast<void*>(remove_handle),
                                  &owner)) {
    return napi_invalid_arg;
  }
  // Handle is live (in the map): dereferencing to find the owner is
  // safe. Owner must be live (handle unregistered before env free).
  // Thread check on the owner (R-ENV-11).
  if (owner->owner_thread != std::this_thread::get_id()) {
    assert(false && "napi_remove_async_cleanup_hook wrong thread (R-ENV-11)");
    return napi_invalid_arg;
  }
  // Alive and Closing both allow removal (completion signal).
  if (owner->state == nodeapi::EnvState::Dead) {
    nodeapi::SetLastError(reinterpret_cast<napi_env>(owner), napi_generic_failure,
                          "remove_async_cleanup_hook: env is dead");
    return napi_generic_failure;
  }
  auto& list = owner->async_cleanup_hooks;
  auto it = std::find(list.begin(), list.end(), remove_handle);
  if (it == list.end()) {
    // In the map but not in the owner's list (should not happen --
    // map and list are kept in sync under the same operations).
    // Treat as unknown.
    nodeapi::SetLastError(reinterpret_cast<napi_env>(owner), napi_invalid_arg,
                          "remove_async_cleanup_hook: unknown handle");
    return napi_invalid_arg;
  }
  list.erase(it);
  remove_handle->completed = true;
  nodeapi::UnregisterAsyncHandle(static_cast<void*>(remove_handle));
  owner->retired_async_handles.push_back(remove_handle);
  nodeapi::ClearLastError(reinterpret_cast<napi_env>(owner));
  return napi_ok;
}

} /* extern "C" */
