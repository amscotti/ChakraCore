//-------------------------------------------------------------------------------------------------------
// Copyright (c) ChakraCore Project Contributors. All rights reserved.
// Licensed under the MIT license. See LICENSE.txt file in the project root for full license information.
//-------------------------------------------------------------------------------------------------------
//
// NodeEnv core: private create/destroy + full §3.6 destroy sequence with
// steps 3-5 delegated to NodeApiLifetime (N1.4). N1.1 additions:
// context guard, version/last-error entries, scope-state lifecycle,
// allocator accounting (see NodeApiEnv.h). See NodeApiEnv.h for the
// layout, assumptions A1-A5, and the ownership record.
//
// Destroy order implemented (plan §3.6 / semantics R-ENV-5/R-ENV-7):
//   1. state -> Closing (+ log "closing"). New public calls return
//      napi_generic_failure (lifetime gates in NodeApiLifetime.cpp).
//   2. Drain the finalizer queue (N3.2, real: nodeapi_wrap_drain,
//      drain until empty, R-WRAP-8), after the "wrap_drain" marker so
//      the P1-1 verbatim ordering probe still compares.
//   3. Sync cleanup hooks, LIFO (NodeApiLifetime).
//   4. Async cleanup hooks (NodeApiLifetime; completion pump = N4.3).
//   5. Instance-data finalizer (NodeApiLifetime).
//   6. (a) Delete refs (N1.3, real: nodeapi_refs_teardown_all +
//      NodeApiRefs_FreeState), (b) close scopes (N1.2, real:
//      NodeApiScopes_DestroyForEnv + FreeState), (c) promise/script
//      (N3.4/N3.5) + async-context/scope bookkeeping (N4.3,
//      NodeApiAsyncContext_DestroyForEnv, native-only), (d) remaining
//      wrap finalizers exactly once + free records (N3.3, real:
//      nodeapi_wrap_teardown). The "refs_scopes_release" marker is
//      emitted by TeardownRefs so the P1-1 ordering probe still
//      compares the whole sequence verbatim (R-ENV-7 a-d).
//   7. state -> Dead (+ log "dead"), free hook state, unregister,
//      delete NodeEnv. Only now may the host dispose runtime/context.
//
// Lock discipline (semantics R-ENV-12 / mem §7): the provider-global
// registry mutex is held ONLY to consult/update the global maps -- never
// across user code (hook/finalizer invocation) and never while emitting
// log markers. Env-owned vectors are touched only on the owner thread.

#include "NodeApiEnv.h"
#include "NodeApiAsyncContext.h"
#include "NodeApiLifetime.h"
#include "NodeApiPromise.h"
#include "NodeApiRefs.h"
#include "NodeApiScopes.h"
#include "NodeApiScript.h"
#include "NodeApiWrap.h"
#include "NodeApiHostApi.h"
#include "node_api.h"
#include "ChakraCommon.h"
#include "ChakraCore.h"

#include <atomic>
#include <cassert>
#include <cstdio>
#include <cstring>
#include <deque>
#include <functional>
#include <mutex>
#include <new>
#include <thread>
#include <unordered_map>
#include <unordered_set>

// Provider-global env registry (file-static; visible to both
// nodeapi::LookupLiveEnv and the extern "C" entry points below).
// Consulted BY VALUE before any dereference (R-ENV-10). See header
// assumption A5 for the {pointer, generation} design, the bound,
// and the irreducible-ABA limit.
//
// Live map value carries the generation issued at create (the
// registry's copy). The NodeEnv also carries a mirror in
// NodeEnv::generation; lookups verify the mirror agrees. The dead
// map (tombstones) maps a destroyed pointer value to its last live
// generation and is capped at nodeapi::kDeadEnvCap entries with
// FIFO eviction (g_dead_order). All three are protected by
// g_registry_mutex; the mutex is never held across user code.
struct LiveEntry {
  nodeapi::NodeEnv* env;
  std::uint64_t generation;
};
static std::mutex g_registry_mutex;
static std::unordered_map<void*, LiveEntry> g_live_envs;
static std::unordered_map<void*, std::uint64_t> g_dead_envs;
static std::deque<void*> g_dead_order;
static std::uint64_t g_next_generation = 1;

// Issue the next generation (caller holds g_registry_mutex). Skips
// 0 (0 means "no generation" for lookups). Wraps after 2^64-1.
static std::uint64_t NextGenerationLocked() {
  std::uint64_t g = g_next_generation++;
  if (g == 0) g = g_next_generation++; // wrapped: skip 0
  return g;
}

// Remove key from the FIFO order queue (caller holds the mutex).
static void DeadOrderRemoveLocked(void* key) {
  for (auto it = g_dead_order.begin(); it != g_dead_order.end(); ++it) {
    if (*it == key) {
      g_dead_order.erase(it);
      return;
    }
  }
}

// Insert a tombstone (caller holds the mutex) with FIFO eviction
// at kDeadEnvCap. Re-insert of an already-present key refreshes
// its order slot (moves to back as newest) with the new value.
static void DeadInsertLocked(void* key, std::uint64_t generation) {
  if (g_dead_envs.find(key) != g_dead_envs.end()) {
    DeadOrderRemoveLocked(key);
  }
  g_dead_envs[key] = generation;
  g_dead_order.push_back(key);
  while (g_dead_envs.size() > nodeapi::kDeadEnvCap &&
         !g_dead_order.empty()) {
    void* old = g_dead_order.front();
    g_dead_order.pop_front();
    g_dead_envs.erase(old);
  }
}

// N1.1 allocator accounting (P1-6): live == created and not yet
// destroyed; total == successful creates since process start.
static std::atomic<size_t> g_live_count{0};
static std::atomic<size_t> g_total_created{0};

// Provider version struct (plan §4 B7): provider truth, not a Node
// runtime claim. Fields: major=1, minor=0, patch=0; release
// identifies the Chakra provider. See the N1.1 report.
static const napi_node_version g_provider_version = {
    1, 0, 0, "libChakraNodeApi/1.0.0"};

namespace nodeapi {

namespace {
size_t TidHash(std::thread::id id) {
  return std::hash<std::thread::id>{}(id);
}
} /* namespace */

bool LookupLiveEnv(void* env_value, NodeEnv** out) {
  if (env_value == nullptr || out == nullptr) return false;
  std::lock_guard<std::mutex> lk(g_registry_mutex);
  auto it = g_live_envs.find(env_value);
  if (it == g_live_envs.end()) return false;
  // Generation mirror check: the registry copy must equal the
  // NodeEnv's field. After the map hit the memory is live, so the
  // single-field read is safe; a mismatch means the bytes at that
  // address are no longer the published NodeEnv (corruption / ABA
  // without registry update) and must fail without returning it.
  // Pointer-only callers cannot detect routine reuse (same pointer,
  // new generation, both mirrors updated together); see A5.
  if (it->second.env == nullptr ||
      it->second.env->generation != it->second.generation) {
    return false;
  }
  *out = it->second.env;
  return true;
}

bool LookupLiveEnvWithGeneration(void* env_value,
                                 std::uint64_t expected_generation,
                                 NodeEnv** out) {
  if (env_value == nullptr || out == nullptr) return false;
  std::lock_guard<std::mutex> lk(g_registry_mutex);
  auto it = g_live_envs.find(env_value);
  if (it == g_live_envs.end()) return false;
  // Decide from the registry record alone: on mismatch fail
  // WITHOUT dereferencing the (possibly new) env.
  if (it->second.generation != expected_generation) return false;
  if (it->second.env == nullptr ||
      it->second.env->generation != expected_generation) {
    return false;
  }
  *out = it->second.env;
  return true;
}

void EmitTeardownEvent(NodeEnv* env, const char* event) {
  if (env == nullptr || event == nullptr) return;
  if (env->teardown_sink != nullptr) {
    env->teardown_sink(env->teardown_ctx, event);
  }
}

ContextGuard::ContextGuard(napi_env env_value)
    : prev_(JS_INVALID_REFERENCE), entered_(false) {
  if (!env_value) return;
  NodeEnv* env = nullptr;
  if (!LookupLiveEnv(static_cast<void*>(env_value), &env)) return;
  if (!env->runtime || !env->context) return;
  JsContextRef cur = JS_INVALID_REFERENCE;
  if (JsGetCurrentContext(&cur) != JsNoError) return;
  prev_ = cur;
  if (env->context != prev_) {
    if (JsSetCurrentContext(env->context) == JsNoError) entered_ = true;
  }
}

ContextGuard::~ContextGuard() {
  if (entered_) {
    JsSetCurrentContext(prev_);
  }
}

napi_status CheckAlive(napi_env env_value) {
  if (!env_value) return napi_invalid_arg;
  NodeEnv* env = nullptr;
  if (!LookupLiveEnv(static_cast<void*>(env_value), &env)) {
    // Dead or fabricated value: never validate through the
    // pointer (R-ENV-10). Public entries treat this as an
    // invalid argument (no env to record into).
    return napi_invalid_arg;
  }
  std::thread::id cur = std::this_thread::get_id();
  if (cur != env->owner_thread) {
    // Wrong thread (semantics R-ENV-11): debug assert with
    // env pointer + threads in the diagnostic, release
    // napi_invalid_arg WITHOUT entering the wrong context and
    // WITHOUT mutating env state cross-thread.
#if !defined(NDEBUG)
    std::fprintf(stderr,
                 "nodeapi: wrong-thread call env=%p owner=%zu current=%zu\n",
                 static_cast<void*>(env_value), TidHash(env->owner_thread),
                 TidHash(cur));
    assert(false);
#endif
    return napi_invalid_arg;
  }
  if (env->state != EnvState::Alive) {
    // Closing or the Dead-before-free window (semantics
    // R-ENV-4). Record and fail cleanly.
    SetLastError(env_value, napi_generic_failure,
                 "generic failure: env is closing or dead");
    return napi_generic_failure;
  }
  return napi_ok;
}

napi_status CheckAliveAllowClosing(napi_env env_value) {
  // Teardown-compatible variant: only Dead-before-free fails.
  // Closing is permitted for getters/removers that finalizers
  // and destroy steps 2-6 need (lifetime get/remove, ref
  // delete/get, exception queries). See header for parity.
  if (!env_value) return napi_invalid_arg;
  NodeEnv* env = nullptr;
  if (!LookupLiveEnv(static_cast<void*>(env_value), &env)) {
    return napi_invalid_arg;
  }
  std::thread::id cur = std::this_thread::get_id();
  if (cur != env->owner_thread) {
#if !defined(NDEBUG)
    std::fprintf(stderr,
                 "nodeapi: wrong-thread call (allow-closing) env=%p "
                 "owner=%zu current=%zu\n",
                 static_cast<void*>(env_value), TidHash(env->owner_thread),
                 TidHash(cur));
    assert(false);
#endif
    return napi_invalid_arg;
  }
  if (env->state == EnvState::Dead) {
    SetLastError(env_value, napi_generic_failure,
                 "generic failure: env is dead");
    return napi_generic_failure;
  }
  return napi_ok;
}

void SetLastError(napi_env env_value, napi_status status,
                  const char* message) {
  if (!env_value) return;
  NodeEnv* env = nullptr;
  if (!LookupLiveEnv(static_cast<void*>(env_value), &env)) return;
  // Caller guarantees Alive + owner thread (checked before).
  // NOTE: during Closing, SetLastError still records (for
  // teardown-compatible entries that fail in Closing). The
  // pointer is still live until step 7, so this is safe.
  // Normalize argument-validation messages to the Node (V8) spellings
  // the vendored js-native-api drivers assert on (PR-12: statuses are
  // conformance; the text below matches Node so those drivers pass
  // without changing any status). Call sites that already pass a
  // Node-literal message (containing "was expected") keep it verbatim.
  // (napi_string_expected call sites for property keys intentionally
  // pass "A string or symbol was expected" and survive this map.)
  const char* stored = message;
  if (!message || message[0] == '\0') {
    stored = "";
  } else if (std::strstr(message, "was expected") != nullptr) {
    stored = message;
  } else if (status == napi_invalid_arg) {
    stored = "Invalid argument";
  } else if (status == napi_boolean_expected) {
    stored = "A boolean was expected";
  } else if (status == napi_number_expected) {
    stored = "A number was expected";
  } else if (status == napi_string_expected) {
    stored = "A string was expected";
  } else if (status == napi_object_expected) {
    stored = "An object was expected";
  } else if (status == napi_array_expected) {
    stored = "An array was expected";
  } else if (status == napi_function_expected) {
    stored = "A function was expected";
  } else if (status == napi_bigint_expected) {
    stored = "A bigint was expected";
  } else if (status == napi_date_expected) {
    stored = "A date was expected";
  } else if (status == napi_arraybuffer_expected) {
    stored = "An arraybuffer was expected";
  }
  env->last_error.status = status;
  if (!stored) stored = "";
  std::strncpy(env->last_error.message, stored,
               sizeof(env->last_error.message) - 1);
  env->last_error.message[sizeof(env->last_error.message) - 1] = 0;
  env->last_error.info.error_message = env->last_error.message;
  env->last_error.info.engine_reserved = nullptr;
  env->last_error.info.engine_error_code = 0;
  env->last_error.info.error_code = status;
}

void ClearLastError(napi_env env_value) {
  // AMB-STATUS-1 decided rule: success clears. This is the sole
  // write of napi_ok into last_error (other than create).
  // No-op for NULL/dead/wrong-thread (same checks as SetLastError).
  if (!env_value) return;
  NodeEnv* env = nullptr;
  if (!LookupLiveEnv(static_cast<void*>(env_value), &env)) return;
  if (std::this_thread::get_id() != env->owner_thread) return;
  // Closing still allows clearing (teardown-compatible getters
  // that succeed in Closing clear as well); Dead-before-free is
  // still live until step 7, so clearing is safe there too.
  env->last_error.status = napi_ok;
  // Lifetime fix (vendored common-inl.h add_last_status): it copies
  // error_message (a pointer into this buffer) then calls further
  // napi entries (is_exception_pending etc., which clear on success)
  // BEFORE consuming the pointer. Mutating the buffer here would
  // wipe the still-held pointer to "". Node's literals survive
  // clearing because they are static; we emulate that by leaving
  // the buffer untouched and repointing info to NULL (Node's
  // after-success spelling; add_last_status maps NULL -> "napi_ok").
  // Previously handed-out pointers stay valid across clears (until
  // the next Set overwrites the buffer, which is exactly the spec's
  // "valid until next API call" window).
  env->last_error.info.error_message = nullptr;
  env->last_error.info.engine_reserved = nullptr;
  env->last_error.info.engine_error_code = 0;
  env->last_error.info.error_code = napi_ok;
}

EntryGuard::EntryGuard(napi_env env_value, bool allowClosing,
                       bool allowPending, bool needContext)
    : env_(env_value), allowClosing_(allowClosing),
      allowPending_(allowPending) {
  if (!env_value) {
    status_ = napi_invalid_arg;
    return;
  }
  NodeEnv* env = nullptr;
  if (!LookupLiveEnv(static_cast<void*>(env_value), &env)) {
    status_ = napi_invalid_arg;
    return;
  }
  std::thread::id cur = std::this_thread::get_id();
  if (cur != env->owner_thread) {
#if !defined(NDEBUG)
    std::fprintf(stderr,
                 "nodeapi: wrong-thread entry-guard env=%p owner=%zu "
                 "current=%zu\n",
                 static_cast<void*>(env_value), TidHash(env->owner_thread),
                 TidHash(cur));
    assert(false);
#endif
    status_ = napi_invalid_arg;
    return;
  }
  if (!allowClosing && env->state != EnvState::Alive) {
    SetLastError(env_value, napi_generic_failure,
                 "generic failure: env is closing or dead");
    status_ = napi_generic_failure;
    return;
  }
  if (allowClosing && env->state == EnvState::Dead) {
    SetLastError(env_value, napi_generic_failure,
                 "generic failure: env is dead");
    status_ = napi_generic_failure;
    return;
  }
  if (needContext) {
    guard_ = std::make_unique<ContextGuard>(env_value);
    // JsHasException requires an active script context; if the
    // guard did not enter (no runtime/context or already current),
    // the pending check below will surface JsErrorNoCurrentContext.
    // Plan 3.7 drain point: on entry to any JS-calling public API
    // (cheap-empty check inside), so GC-enqueued finalizers run
    // promptly without waiting for a Buffer entry or destroy.
    // Reentrancy-safe: DrainState pops one record before running
    // its finalizer, so a finalizer's own napi entry sees an
    // (empty or chained) queue, not the record being run.
    ::nodeapi_wrap_drain(env_value);
  }
  if (!allowPending) {
    // Need the env context current for JsHasException (plan §3.2).
    // When needContext false but !allowPending, we must enter
    // the context just for the check; preserve the guard for the
    // caller's window so the checked context stays current.
    if (!needContext) {
      guard_ = std::make_unique<ContextGuard>(env_value);
    }
    bool has = false;
    JsErrorCode ec = JsHasException(&has);
    if (ec == JsErrorNoCurrentContext || ec == JsErrorInDisabledState ||
        ec == JsErrorHeapEnumInProgress ||
        ec == JsErrorInObjectBeforeCollectCallback ||
        ec == JsErrorInThreadServiceCallback) {
      // No active script context to query: treat as no pending
      // (do not fabricate) and do not gate; the subsequent work
      // will surface its own JsRT error. This keeps native-only
      // allowPending==false callers from failing spuriously when
      // invoked outside a script context (e.g. in a hook).
      // We do not record last_error here; the gate is only for a
      // real pending exception.
    } else if (ec == JsNoError && has) {
      SetLastError(env_value, napi_pending_exception,
                   "pending exception (entry gate)");
      status_ = napi_pending_exception;
      // Keep guard_ alive so the caller can still unwind; the
      // caller must not proceed with work when !ok().
      return;
    } else if (ec != JsNoError) {
      // Other JsHasException failure (should not happen with a
      // valid context): gate conservatively as pending only when
      // we know there is an exception; otherwise do not block.
    }
  }
}

EntryGuard::~EntryGuard() = default;

void TeardownScopes(NodeEnv* env) {
  // Step 6b (N1.2, real): close every scope incl. the default
  // scope, releasing all roots (JsRelease). Must run with the
  // env context current (destroy enters ContextGuard).
  if (!env) return;
  NodeApiScopes_DestroyForEnv(reinterpret_cast<napi_env>(env));
  NodeApiScopes_FreeState(env->scope_state);
  env->scope_state = nullptr;
}

void TeardownRefs(NodeEnv* env) {
  // Step 6a (N1.3, real): delete all references -- strong roots
  // released, weak handles (+ R6 roots) released (R-REF-8, R6).
  // nodeapi_refs_teardown_all is the destroy-step-6 entry point the
  // task brief wires here; it releases every root and frees every
  // record inside the refs state. The state struct itself is freed
  // immediately after (mirrors TeardownScopes' DestroyForEnv +
  // FreeState pair). The "refs_scopes_release" marker is emitted
  // here so the P1-1 ordering probe still compares verbatim
  // (R-ENV-7 sub-order a-d: refs before scopes).
  if (env != nullptr) {
    NodeApiRefsState* st = env->refs_state;
    nodeapi_refs_teardown_all(reinterpret_cast<napi_env>(env));
    NodeApiRefs_FreeState(st);
    env->refs_state = nullptr;
  }
  EmitTeardownEvent(env, "refs_scopes_release");
}

void TeardownPromiseScript(NodeEnv* env) {
  // Step 6 (N3.4/N3.5, real): deferred roots + queued tasks released
  // (NodeApiPromise_DestroyForEnv) and script state freed. Runs with
  // the env context current (destroy's guard). Promise teardown must
  // run BEFORE promise values are released by scope teardown? No:
  // deferred roots are independent of scope roots (two fns vs the
  // promise object), so order vs scopes is not load-bearing; this
  // teardown runs after refs/scopes (the destroy call site orders
  // refs -> scopes -> promise/script -> wraps) while the context is
  // still current, satisfying R5 (all roots released before
  // JsDisposeRuntime).
  // N4.3: async-context + callback-scope bookkeeping (native-only,
  // no JS roots) is freed here as well (DestroyForEnv, lazy global
  // state in NodeApiAsyncContext.cpp; NodeEnv layout untouched).
  // Order vs promise/script is not load-bearing (no shared JS roots).
  if (env == nullptr) return;
  napi_env ev = reinterpret_cast<napi_env>(env);
  if (env->promise_state != nullptr) {
    NodeApiPromise_DestroyForEnv(ev);
    NodeApiPromise_FreeState(env->promise_state);
    env->promise_state = nullptr;
  }
  if (env->script_state != nullptr) {
    NodeApiScript_DestroyForEnv(ev);
    NodeApiScript_FreeState(env->script_state);
    env->script_state = nullptr;
  }
  NodeApiAsyncContext_DestroyForEnv(ev);
}

void TeardownWraps(NodeEnv* env) {
  // Steps 6c-d (N3.2/N3.3, real): drain any already-enqueued finalizers,
  // run every remaining live wrap/external finalizer exactly once,
  // then free the wrap state (nodeapi_wrap_teardown in NodeApiWrap.cpp).
  // Tick order (R-ENV-7) is refs -> scopes -> promise/script -> wraps;
  // the wrap backstop runs last within step 6 with the env context
  // still current. State lookup is by env pointer value (lazy state in
  // the Wrap TU); a null env is a no-op.
  if (env == nullptr) return;
  ::nodeapi_wrap_teardown(reinterpret_cast<napi_env>(env));
}

size_t DebugLiveEnvCount() noexcept {
  return g_live_count.load(std::memory_order_relaxed);
}

size_t DebugTotalEnvCount() noexcept {
  return g_total_created.load(std::memory_order_relaxed);
}

size_t DebugDeadEnvCount() noexcept {
  std::lock_guard<std::mutex> lk(g_registry_mutex);
  return g_dead_envs.size();
}

} /* namespace nodeapi */

extern "C" {

// --- Private host API (§3.2) ---
// The provider compiles with -fvisibility=hidden (HIGH-5); the 145
// napi_* entries stay exported via NAPI_EXTERN's default visibility
// on their vendored declarations, while these two host entries need
// an explicit default on the definition because NodeApiHostApi.h
// carries no visibility macro.
#if defined(__GNUC__)
__attribute__((visibility("default")))
#endif
napi_env chakra_nodeapi_create_env(
    JsRuntimeHandle runtime, JsContextRef context,
    const char* module_filename,
    const chakra_nodeapi_host_services* services, void* host_ctx) {
  if (runtime == nullptr || context == nullptr) return nullptr;

  nodeapi::NodeEnv* env = new (std::nothrow) nodeapi::NodeEnv();
  if (env == nullptr) return nullptr;
  env->runtime = runtime;
  env->context = context;
  env->owner_thread = std::this_thread::get_id();
  env->state = nodeapi::EnvState::Alive;
  env->module_api_version = 8;
  // Owned last-error buffer: point info at it now so the record
  // is well-formed before the first failure.
  env->last_error.info.error_message = env->last_error.message;
  env->last_error.info.engine_reserved = nullptr;
  env->last_error.info.engine_error_code = 0;
  env->last_error.info.error_code = napi_ok;
  try {
    if (module_filename != nullptr) {
      env->module_filename = module_filename;
    }
    if (services != nullptr) {
      env->services = *services;
    } else {
      env->services = chakra_nodeapi_host_services{};
    }
    env->host_ctx = host_ctx;
    // Scope state (N1.2): default scope pushed (R-SC-7). Created
    // here so every live env has a scope stack; destroyed at
    // destroy step 6 (TeardownScopes). No JS calls needed.
    env->scope_state = NodeApiScopes_CreateState();
    // Refs state (N1.3): empty. Created here so every live env has
    // a refs map; destroyed at destroy step 6a (TeardownRefs).
    // No JS calls needed (no roots taken at creation).
    env->refs_state = NodeApiRefs_CreateState();
    // Promise state (N3.4): empty deferred map. Created here so every
    // live env has a promise map; destroyed at destroy step 6
    // (TeardownPromiseScript). No JS calls, no roots.
    env->promise_state = NodeApiPromise_CreateState();
    // Script state (N3.5): external total 0. Created here; destroyed
    // at destroy step 6. Native-only, no JS calls.
    env->script_state = NodeApiScript_CreateState();
  } catch (...) {
    // OOM rollback: publish nothing (memory-management.md §5).
    if (env->script_state != nullptr) {
      NodeApiScript_FreeState(env->script_state);
      env->script_state = nullptr;
    }
    if (env->promise_state != nullptr) {
      NodeApiPromise_FreeState(env->promise_state);
      env->promise_state = nullptr;
    }
    if (env->refs_state != nullptr) {
      NodeApiRefs_FreeState(env->refs_state);
      env->refs_state = nullptr;
    }
    if (env->scope_state != nullptr) {
      NodeApiScopes_FreeState(env->scope_state);
      env->scope_state = nullptr;
    }
    delete env;
    return nullptr;
  }

  {
    std::lock_guard<std::mutex> lk(g_registry_mutex);
    // Issue a fresh generation and displace any tombstone at this
    // address (R-ENV-9, A5): the new live generation supersedes the
    // old dead one. The old generation remains observable only via
    // a generation-aware lookup with the remembered value (which
    // then fails); pointer-only callers now alias the new env by
    // construction (irreducible without host-passed generations).
    void* key = static_cast<void*>(env);
    env->generation = NextGenerationLocked();
    if (g_dead_envs.find(key) != g_dead_envs.end()) {
      g_dead_envs.erase(key);
      DeadOrderRemoveLocked(key);
    }
    g_live_envs[key] = LiveEntry{env, env->generation};
  }
  g_live_count.fetch_add(1, std::memory_order_relaxed);
  g_total_created.fetch_add(1, std::memory_order_relaxed);
  napi_env published = reinterpret_cast<napi_env>(env);
  /* N3.4/PR-8: install the provider continuation trampoline for this
   * context iff the provider has not already (provider-side flag;
   * undetectable host installs are clobbered -- hosts that want their
   * own queue must install after create returns). Best-effort: never
   * fails the create. */
  NodeApiPromise_EnsureContinuationInstalled(published);
  return published;
}

#if defined(__GNUC__)
__attribute__((visibility("default")))
#endif
void chakra_nodeapi_destroy_env(napi_env env_value) {
  if (env_value == nullptr) {
    assert(false && "chakra_nodeapi_destroy_env(null)");
    std::fprintf(stderr, "chakra_nodeapi_destroy_env(null): ignored\n");
    return;
  }
  void* key = static_cast<void*>(env_value);

  // R-ENV-9/10 firewall: consult the registry by VALUE before any
  // dereference. A dead/fabricated/post-free value returns here
  // without touching freed memory (debug assert + release no-op).
  nodeapi::NodeEnv* env = nullptr;
  std::uint64_t live_generation = 0;
  {
    std::lock_guard<std::mutex> lk(g_registry_mutex);
    auto it = g_live_envs.find(key);
    if (it == g_live_envs.end()) {
      assert(false && "chakra_nodeapi_destroy_env on dead/unknown env (double destroy)");
      std::fprintf(stderr,
                   "chakra_nodeapi_destroy_env: unknown or already-destroyed "
                   "env %p: ignored (no partial teardown)\n",
                   key);
      return;
    }
    // Mirror check before dereferencing beyond this point: a live
    // entry whose memory no longer matches its registry generation
    // is not a valid env (corruption) -- treat as dead/unknown.
    if (it->second.env == nullptr ||
        it->second.env->generation != it->second.generation) {
      assert(false && "chakra_nodeapi_destroy_env on corrupt live entry");
      return;
    }
    env = it->second.env;
    live_generation = it->second.generation;
  }
  // Live: dereference is now safe.
  if (env->owner_thread != std::this_thread::get_id()) {
    assert(false && "chakra_nodeapi_destroy_env from wrong thread (R-ENV-6)");
    std::fprintf(stderr,
                 "chakra_nodeapi_destroy_env: wrong thread for env %p: "
                 "ignored (no partial teardown)\n",
                 key);
    return;
  }
  if (env->state != nodeapi::EnvState::Alive) {
    // Reentrant destroy from a hook/finalizer during Closing.
    assert(false && "chakra_nodeapi_destroy_env reentrant during Closing");
    std::fprintf(stderr,
                 "chakra_nodeapi_destroy_env: env %p already %s: ignored\n",
                 key, nodeapi::DescribeState(env->state));
    return;
  }

  // Step 1: Closing.
  env->state = nodeapi::EnvState::Closing;
  nodeapi::EmitTeardownEvent(env, "closing");

  // Steps 2-6 run with the env's context current (plan §3.2).
  // Lifetime entries are native-only (no JS calls) so the guard
  // is a no-op for them, but scopes teardown (step 6b) releases
  // roots via JsRelease and requires the env context.
  {
    nodeapi::ContextGuard guard(env_value);

    // Step 2 (N3.2, real): drain the finalizer queue until empty
    // (R-WRAP-8). The "wrap_drain" marker is emitted first so the P1-1
    // verbatim ordering probe still compares the whole sequence;
    // the drain itself runs wrap finalizers with a live (Closing) env,
    // without holding the queue lock across user finalizers.
    nodeapi::EmitTeardownEvent(env, "wrap_drain");
    ::nodeapi_wrap_drain(env_value);

    // Step 3-5 (N1.4). NOTE (N5.1 vendored-oracle amendment):
    // the instance finalizer historically ran here (plan 3.6 step 5),
    // BEFORE step 6 wraps. Vendored 6_object_wrap proves that wrong:
    // wrap backstop finalizers call napi_get_instance_data and
    // dereference it during destroy; freeing at step 5 leaves NULL
    // and crashes (SIGSEGV at destroy). Node keeps instance data
    // alive across wrap finalizers. Instance therefore runs AFTER
    // wraps backstop (step 6d), still before Dead (step 7). Refs/
    // scopes/promise are already gone by then; instance finalizers
    // that need JS are already invalid in Closing per plan, so this
    // ordering is Closing-safe. The P1-1 verbatim probe encodes the
    // old order and needs a follow-up amendment.
    nodeapi::Lifetime_RunSyncCleanupHooks(env);
    nodeapi::Lifetime_RunAsyncCleanupHooks(env);

    // Step 6 (R-ENV-7 sub-order a-d): (a) refs real, (b) scopes
    // real, (c) promise/script real (N3.4/N3.5), (d) wraps
    // placeholder.
    nodeapi::TeardownRefs(env);
    nodeapi::TeardownScopes(env);
    nodeapi::TeardownPromiseScript(env);
    nodeapi::TeardownWraps(env);
    nodeapi::Lifetime_RunInstanceFinalizer(env);
  }

  // Step 7: Dead + free.
  env->state = nodeapi::EnvState::Dead;
  nodeapi::EmitTeardownEvent(env, "dead");

  nodeapi::Lifetime_DiscardEnvState(env);
  {
    std::lock_guard<std::mutex> lk(g_registry_mutex);
    g_live_envs.erase(key);
    DeadInsertLocked(key, live_generation);
  }
  g_live_count.fetch_sub(1, std::memory_order_relaxed);
  delete env;
}

// --- Debug/test-only introspection ---

void chakra_nodeapi_test_set_teardown_log(
    napi_env env_value, void (*sink)(void* ctx, const char* event),
    void* ctx) {
  nodeapi::NodeEnv* env = nullptr;
  if (!nodeapi::LookupLiveEnv(static_cast<void*>(env_value), &env)) return;
  env->teardown_sink = sink;
  env->teardown_ctx = ctx;
}

bool chakra_nodeapi_test_is_live(napi_env env_value) {
  nodeapi::NodeEnv* env = nullptr;
  return nodeapi::LookupLiveEnv(static_cast<void*>(env_value), &env);
}

const char* chakra_nodeapi_test_state_name(napi_env env_value) {
  nodeapi::NodeEnv* env = nullptr;
  if (!nodeapi::LookupLiveEnv(static_cast<void*>(env_value), &env)) {
    return "dead";
  }
  switch (env->state) {
  case nodeapi::EnvState::Alive: return "alive";
  case nodeapi::EnvState::Closing: return "closing";
  case nodeapi::EnvState::Dead: return "dead-before-free";
  }
  return "unknown";
}

std::uint64_t chakra_nodeapi_test_env_generation(napi_env env_value) {
  nodeapi::NodeEnv* env = nullptr;
  if (!nodeapi::LookupLiveEnv(static_cast<void*>(env_value), &env)) {
    return 0;
  }
  return env->generation;
}

size_t chakra_nodeapi_test_dead_count(void) {
  return nodeapi::DebugDeadEnvCount();
}

std::uint64_t chakra_nodeapi_test_dead_generation(napi_env env_value) {
  if (env_value == nullptr) return 0;
  std::lock_guard<std::mutex> lk(g_registry_mutex);
  auto it = g_dead_envs.find(static_cast<void*>(env_value));
  if (it == g_dead_envs.end()) return 0;
  return it->second;
}

// --- N1.2 scope-state accessors (assumed interface from
// NodeApiScopes.h; defined here, inside extern "C" to match the
// declaration). Both consult the live registry by VALUE first so
// dead/fabricated values never crash (R-ENV-10). ---

NodeApiScopeState* NodeApiEnv_ScopeState(napi_env env_value) {
  nodeapi::NodeEnv* env = nullptr;
  if (!nodeapi::LookupLiveEnv(static_cast<void*>(env_value), &env)) {
    return nullptr;
  }
  return env->scope_state;
}

NodeApiRefsState* NodeApiEnv_RefsState(napi_env env_value) {
  nodeapi::NodeEnv* env = nullptr;
  if (!nodeapi::LookupLiveEnv(static_cast<void*>(env_value), &env)) {
    return nullptr;
  }
  return env->refs_state;
}

NodeApiPromiseState* NodeApiEnv_PromiseState(napi_env env_value) {
  nodeapi::NodeEnv* env = nullptr;
  if (!nodeapi::LookupLiveEnv(static_cast<void*>(env_value), &env)) {
    return nullptr;
  }
  return env->promise_state;
}

NodeApiScriptState* NodeApiEnv_ScriptState(napi_env env_value) {
  nodeapi::NodeEnv* env = nullptr;
  if (!nodeapi::LookupLiveEnv(static_cast<void*>(env_value), &env)) {
    return nullptr;
  }
  return env->script_state;
}

int /*bool*/ NodeApiEnv_IsAlive(napi_env env_value) {
  nodeapi::NodeEnv* env = nullptr;
  if (!nodeapi::LookupLiveEnv(static_cast<void*>(env_value), &env)) {
    return 0;
  }
  return env->state == nodeapi::EnvState::Alive ? 1 : 0;
}

// --- N1.1 napi_* subset (everything else is N1.5/N2-N4) ---
// basic_env entries that touch only native memory: they validate
// (null -> thread -> state) but do NOT enter the Chakra context
// (no Js* calls). Validate-first (§3.10): a NULL output pointer
// is napi_invalid_arg even if the env is Closing.

napi_status NAPI_CDECL napi_get_version(node_api_basic_env env_raw,
                                        uint32_t* result) {
  // node_api_basic_env and napi_env are the same pointer type.
  napi_env env = reinterpret_cast<napi_env>(env_raw);
  if (!result) {
    nodeapi::NodeEnv* te = nullptr;
    if (env &&
        nodeapi::LookupLiveEnv(static_cast<void*>(env), &te) &&
        te->owner_thread == std::this_thread::get_id() &&
        (te->state == nodeapi::EnvState::Alive ||
         te->state == nodeapi::EnvState::Closing)) {
      nodeapi::SetLastError(env, napi_invalid_arg,
                            "napi_get_version: result is NULL");
    }
    return napi_invalid_arg;
  }
  napi_status v = nodeapi::CheckAlive(env);
  if (v != napi_ok) return v;
  *result = 8;
  nodeapi::ClearLastError(env);
  return napi_ok;
}

napi_status NAPI_CDECL napi_get_node_version(
    node_api_basic_env env_raw, const napi_node_version** version) {
  napi_env env = reinterpret_cast<napi_env>(env_raw);
  if (!version) {
    nodeapi::NodeEnv* te = nullptr;
    if (env &&
        nodeapi::LookupLiveEnv(static_cast<void*>(env), &te) &&
        te->owner_thread == std::this_thread::get_id() &&
        (te->state == nodeapi::EnvState::Alive ||
         te->state == nodeapi::EnvState::Closing)) {
      nodeapi::SetLastError(env, napi_invalid_arg,
                            "napi_get_node_version: version is NULL");
    }
    return napi_invalid_arg;
  }
  napi_status v = nodeapi::CheckAlive(env);
  if (v != napi_ok) return v;
  *version = &g_provider_version;
  nodeapi::ClearLastError(env);
  return napi_ok;
}

napi_status NAPI_CDECL napi_get_last_error_info(
    node_api_basic_env env_raw,
    const napi_extended_error_info** result) {
  napi_env env = reinterpret_cast<napi_env>(env_raw);
  if (!result) return napi_invalid_arg;
  // Teardown-compatible: finalizers running during Closing legitimately
  // call get_last_error_info (vendored common-inl.h error path inside
  // NODE_API_CALL_RETURN_VOID in a wrap finalizer). Alive-only would
  // return generic_failure WITHOUT setting *result, leaving the
  // addon's error_info uninitialized (the double-free crash at destroy).
  // Allow Alive or Closing on the owner thread (Dead fails).
  nodeapi::NodeEnv* te = nullptr;
  {
    napi_status vs = nodeapi::CheckAlive(env);
    if (vs != napi_ok) {
      // Re-check Closing-tolerant (same live/thread/state test as
      // CheckAlive but Closing allowed).
      if (!env || !nodeapi::LookupLiveEnv(static_cast<void*>(env), &te) ||
          te->owner_thread != std::this_thread::get_id() ||
          (te->state != nodeapi::EnvState::Alive &&
           te->state != nodeapi::EnvState::Closing)) {
        return vs;
      }
    } else {
      nodeapi::LookupLiveEnv(static_cast<void*>(env), &te);
    }
    if (!te) return napi_invalid_arg;
  }
  // Reading last_error does NOT clear it (AMB-STATUS-1 exception).
  // The returned info stays valid until the next API call; the
  // next successful call (any other entry) clears it. This matches
  // vendored test_general.c (failure -> read -> error_code==status)
  // and Node's C implementation (get_last_error_info does not
  // call napi_clear_last_error itself).
  *result = &te->last_error.info;
  // Do NOT ClearLastError here.
  return napi_ok;
}

// --- Missing symbols (N1.5 plumbing, task brief): exception queries ---
// Status mapping (C column): both are "Yes" (execute with pending),
// allowClosing=true (teardown-compatible getters: they need no JS
// beyond JsHasException and must be callable from finalizers/hooks
// during Closing). Last-error: success clears (AMB-STATUS-1).

napi_status NAPI_CDECL napi_is_exception_pending(napi_env env,
                                                 bool* result) {
  if (result == nullptr) {
    if (env) nodeapi::SetLastError(env, napi_invalid_arg,
                                     "napi_is_exception_pending: result is NULL");
    return napi_invalid_arg;
  }
  nodeapi::EntryGuard guard(env, /*allowClosing=*/true,
                            /*allowPending=*/true, /*needContext=*/true);
  if (!guard.ok()) return guard.status();
  bool has = false;
  JsErrorCode ec = JsHasException(&has);
  if (ec != JsNoError) {
    // Best-effort mapping per §3.10: disabled / no-context do not
    // indicate a pending JS exception the caller can handle; report
    // generic_failure without fabricating a pending state.
    if (ec == JsErrorNoCurrentContext ||
        ec == JsErrorInDisabledState) {
      nodeapi::SetLastError(env, napi_generic_failure,
                            "napi_is_exception_pending: no current context");
      return napi_generic_failure;
    }
    if (ec == JsErrorInExceptionState || ec == JsErrorScriptException) {
      // HasException itself does not throw; this arm is defensive.
      nodeapi::SetLastError(env, napi_pending_exception,
                            "napi_is_exception_pending: pending");
      return napi_pending_exception;
    }
    nodeapi::SetLastError(env, napi_generic_failure,
                          "napi_is_exception_pending: JsHasException failed");
    return napi_generic_failure;
  }
  *result = has;
  nodeapi::ClearLastError(env);
  return napi_ok;
}

napi_status NAPI_CDECL napi_get_and_clear_last_exception(
    napi_env env, napi_value* result) {
  if (result == nullptr) {
    if (env) nodeapi::SetLastError(env, napi_invalid_arg,
                                     "napi_get_and_clear_last_exception: result is NULL");
    return napi_invalid_arg;
  }
  nodeapi::EntryGuard guard(env, /*allowClosing=*/true,
                            /*allowPending=*/true, /*needContext=*/true);
  if (!guard.ok()) return guard.status();
  // Fast path: no pending -> ok + NULL (C: null result when no
  // pending, not error; doc/node-api-status-mapping.md table).
  bool has = false;
  JsErrorCode hq = JsHasException(&has);
  if (hq == JsNoError && !has) {
    *result = nullptr;
    nodeapi::ClearLastError(env);
    return napi_ok;
  }
  JsValueRef ex = nullptr;
  JsErrorCode ec = JsGetAndClearException(&ex);
  if (ec == JsNoError) {
    // JsGetAndClearException returns the thrown object without
    // rooting (plain JsValueRef). R1 requires every returned
    // napi_value be rooted into the innermost scope before the
    // next scope close / GC. Root it now (no intervening JS).
    // RootIntoInnermost does JsAddRef; on failure it returns
    // generic_failure and we surface it without publishing the
    // unrooted value.
    napi_value v = reinterpret_cast<napi_value>(ex);
    napi_status rs = NodeApiScopes_RootIntoInnermost(env, v);
    if (rs != napi_ok) {
      // Rooting failed (OOM / no scope state): do not publish the
      // unrooted exception; the exception is already cleared, so
      // the caller sees absence. Record and fail.
      nodeapi::SetLastError(env, napi_generic_failure,
                            "napi_get_and_clear_last_exception: root failed");
      return napi_generic_failure;
    }
    *result = v;
    nodeapi::ClearLastError(env);
    return napi_ok;
  }
  if (ec == JsErrorInvalidArgument) {
    // No pending exception (lost the race, or HasException and
    // GetAndClear disagree because the exception was cleared
    // between the two calls): treat as ok + null, not error.
    *result = nullptr;
    nodeapi::ClearLastError(env);
    return napi_ok;
  }
  if (ec == JsErrorNoCurrentContext || ec == JsErrorInDisabledState) {
    nodeapi::SetLastError(env, napi_generic_failure,
                          "napi_get_and_clear_last_exception: no current context");
    return napi_generic_failure;
  }
  if (ec == JsErrorInExceptionState || ec == JsErrorScriptException) {
    nodeapi::SetLastError(env, napi_pending_exception,
                          "napi_get_and_clear_last_exception: pending");
    return napi_pending_exception;
  }
  nodeapi::SetLastError(env, napi_generic_failure,
                        "napi_get_and_clear_last_exception: JsGetAndClearException failed");
  return napi_generic_failure;
}

} /* extern "C" */
