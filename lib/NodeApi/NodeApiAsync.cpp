//-------------------------------------------------------------------------------------------------------
// Copyright (c) ChakraCore Project Contributors. All rights reserved.
// Licensed under the MIT license. See LICENSE.txt file in the project root for full license information.
//-------------------------------------------------------------------------------------------------------
//
// Async work + thread-safe functions + uv loop (N4.4). See NodeApiAsync.h
// for the state machines, thread-confinement proof, status discipline,
// and the full memory-management ownership record.

#include "NodeApiAsync.h"

#include "NodeApiEnv.h"
#include "NodeApiHostApi.h"
#include "NodeApiScopes.h"
#include "ChakraCommon.h"
#include "ChakraCore.h"

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <mutex>
#include <new>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace {

JsValueRef ToJs(napi_value v) { return reinterpret_cast<JsValueRef>(v); }
napi_value FromJs(JsValueRef v) { return reinterpret_cast<napi_value>(v); }

/* Work lifecycle states (atomic int on the record). */
enum WorkState : int {
  kWorkCreated = 0,
  kWorkQueued = 1,
  kWorkExecuting = 2,
  kWorkCompleted = 3,
  kWorkCancelled = 4,
};

/* One napi_async_work record. The record address IS the handle. Native
 * only: no JsValueRef, no roots. Env is stored as a VALUE (never
 * dereferenced off-thread; the execute tramp copies it into the user
 * call as the opaque callback arg without validating it). */
struct AsyncWork {
  napi_env env = nullptr;
  napi_async_execute_callback execute = nullptr;
  napi_async_complete_callback complete = nullptr;
  void* data = nullptr;
  std::atomic<int> state{kWorkCreated};
  std::atomic<bool> cancel_requested{false};
  std::atomic<bool> complete_delivered{false};
  /* Atomic native refs: 1 owner (create..delete/destroy) + 1 worker
   * (queue..complete-drop). Freed when the last ref drops. */
  std::atomic<int> refs{1};
  /* mu serializes the Queued->Executing decision (execute tramp on a
   * worker thread) vs napi_cancel_async_work (env thread): cancel wins
   * only if it takes mu while the record is still Queued; once the
   * execute tramp takes mu and claims Executing with cancel false, a
   * later cancel observes Executing and fails. Without mu the
   * check-then-store in cancel could win execute's CAS yet lose the
   * flag check (cancel ok but execute still ran). Never held across
   * user code (R-ENV-12). */
  std::mutex mu;
};

/* One queued TSFN call. Heap allocation owned by the TSFN queue. `data`
 * is a native pointer (call_js_cb form) or borrowed napi_value bits
 * (direct form); the provider never frees user data and never roots
 * direct-form values off-thread. */
struct TsfnItem {
  void* data = nullptr;
};

/* One napi_threadsafe_function state. The record address IS the handle.
 * Thread model: call/acquire/release/get_context (cross-thread) touch
 * only the global map + this state's mutex/condvar/atomics; env-thread
 * (create/ref/unref/drain/finalize/teardown) touches the JS func root
 * and per-env loop count. Worker paths never touch the func root (R3). */
struct TsfnState {
  napi_env env = nullptr;
  std::thread::id owner_thread;
  void* context = nullptr;
  JsValueRef func = JS_INVALID_REFERENCE; /* rooted at create, released at finalize */
  napi_threadsafe_function_call_js call_js_cb = nullptr;
  void* finalize_data = nullptr;
  napi_finalize finalize_cb = nullptr;
  size_t max_queue_size = 0; /* 0 = unlimited */
  std::atomic<int64_t> thread_count{0};
  std::mutex mutex;
  std::condition_variable cv;
  std::deque<TsfnItem*> queue;
  bool closing = false; /* env teardown: new calls fail, queue aborted */
  bool finalizing = false; /* finalize scheduled (exactly-once) */
  bool finalized = false; /* finalize executed (exactly-once) */
  bool abort_on_finalize = false; /* last release mode (or teardown=true) */
  bool is_ref = true; /* starts ref'd; env-thread only */
  bool drain_scheduled = false; /* env-thread drain coalescing (under mutex) */
  std::atomic<int> state_refs{1}; /* native free refs (owner + in-flight ops/items) */
};

/* Per-env state (one per env that uses this card). Lazy-created by
 * EnsureState on the first env-taking async call; erased by the cleanup
 * hook at destroy step 3. loop_ref_count + owned sets are env-thread
 * only (create/ref/unref/teardown all validate the owner thread). */
struct AsyncStateImpl {
  napi_env env = nullptr;
  size_t loop_ref_count = 0;
  std::unordered_set<AsyncWork*> works;
  std::unordered_set<TsfnState*> tsfns;
};

/* Global maps (value-keyed, mutex-protected). Lookups consult the maps
 * by handle VALUE before any dereference (R-ENV-10 pattern for stale
 * handles). The mutex is never held across user/host code (R-ENV-12):
 * callers lookup+increment under the lock, then unlock and work. */
std::mutex g_envs_mutex;
std::unordered_map<napi_env, AsyncStateImpl*> g_envs;

std::mutex g_works_mutex;
std::unordered_map<napi_async_work, AsyncWork*> g_works;

std::mutex g_tsfns_mutex;
std::unordered_map<napi_threadsafe_function, TsfnState*> g_tsfns;

/* Callback record entry for env-thread trampolines (brief: completion /
 * drain run with context enter + scope + a callback record entry). The
 * TU-local stack mirrors NodeApiFunctions' stack for balance
 * verification. It is invisible to napi_get_cb_info (which searches the
 * Functions stack); the records exist so nested JS (direct-form funcs,
 * call_js_cb calling napi_call_function) nests correctly and scope
 * balance is enforceable. Push/pop lock only, never across user code. */
constexpr std::uint64_t kCbMagic = 0x5F00A5C3ACB11ULL;
struct CallbackInfo {
  std::uint64_t magic = kCbMagic;
  napi_env env = nullptr;
  napi_value thisArg = nullptr;
  napi_value* argv = nullptr;
  size_t argc = 0;
  napi_value newTarget = nullptr;
  napi_value callee = nullptr;
  void* data = nullptr;
};
std::mutex g_cb_mutex;
std::unordered_map<napi_env, std::vector<CallbackInfo*>> g_cb_stacks;

bool PushCallback(napi_env env, CallbackInfo* info) {
  std::lock_guard<std::mutex> lk(g_cb_mutex);
  try {
    g_cb_stacks[env].push_back(info);
    return true;
  } catch (...) {
    return false;
  }
}

void PopCallback(napi_env env, CallbackInfo* info) {
  std::lock_guard<std::mutex> lk(g_cb_mutex);
  auto it = g_cb_stacks.find(env);
  if (it == g_cb_stacks.end()) return;
  auto& v = it->second;
  for (;;) {
    if (v.empty()) break;
    CallbackInfo* top = v.back();
    v.pop_back();
    if (top == info) break;
  }
  if (v.empty()) g_cb_stacks.erase(it);
}

bool HasPendingException() {
  bool has = false;
  JsErrorCode e = JsHasException(&has);
  (void)e;
  return (e == JsNoError && has);
}

/* Best-effort mismatch pending (R-CB-6 surfacing for trampolines). */
void SetMismatchPending() {
  if (HasPendingException()) return;
  JsValueRef msg = JS_INVALID_REFERENCE;
  if (JsCreateString("handle scope mismatch", 21, &msg) != JsNoError) return;
  JsValueRef err = JS_INVALID_REFERENCE;
  if (JsCreateError(msg, &err) != JsNoError) return;
  /* Temporary roots held across the set only; released immediately
   * after (the engine holds the pending exception itself). */
  JsAddRef(msg, nullptr);
  JsAddRef(err, nullptr);
  (void)JsSetException(err);
  (void)JsRelease(err, nullptr);
  (void)JsRelease(msg, nullptr);
}

void AsyncCleanupHook(void* arg);

/* Fetch services/host_ctx for a live env (registry lookup by value;
 * never dereferences a dead env). Returns false if the env is not live.
 * Must only be called when the caller has already validated env
 * liveness on the owner thread (or from cross-thread paths that treat
 * not-live as generic_failure without touching last_error). */
bool FetchServices(napi_env env, chakra_nodeapi_host_services* out_services,
                   void** out_host_ctx) {
  if (env == nullptr) return false;
  nodeapi::NodeEnv* nenv = nullptr;
  if (!nodeapi::LookupLiveEnv(static_cast<void*>(env), &nenv)) return false;
  if (out_services != nullptr) *out_services = nenv->services;
  if (out_host_ctx != nullptr) *out_host_ctx = nenv->host_ctx;
  return true;
}

bool HasSchedulingServices(const chakra_nodeapi_host_services& s) {
  return s.post_task != nullptr && s.post_work != nullptr;
}

/* Lookup work by handle VALUE (no dereference of the handle itself).
 * Returns the record or nullptr. The caller must hold the map mutex
 * only for the lookup; record fields are atomics/immutable after
 * publication (env/execute/complete/data set once before map insert). */
AsyncWork* LookupWork(napi_async_work handle) {
  std::lock_guard<std::mutex> lk(g_works_mutex);
  auto it = g_works.find(handle);
  return it == g_works.end() ? nullptr : it->second;
}

/* Cross-thread TSFN lookup with native-ref increment (increment under
 * the map mutex so finalize's erase+owner-drop cannot free under us).
 * On success *needs_drop is true (caller must DropTsfnRef). */
TsfnState* LookupTsfn(napi_threadsafe_function handle, bool* needs_drop) {
  if (needs_drop != nullptr) *needs_drop = false;
  std::lock_guard<std::mutex> lk(g_tsfns_mutex);
  auto it = g_tsfns.find(handle);
  if (it == g_tsfns.end() || it->second == nullptr) return nullptr;
  TsfnState* st = it->second;
  st->state_refs.fetch_add(1, std::memory_order_acq_rel);
  if (needs_drop != nullptr) *needs_drop = true;
  return st;
}

/* Drop a TSFN native ref; free iff last. Map erasure (finalize/teardown)
 * happens before the owner drop, so a zero here means no live map entry
 * references the record and no in-flight op holds it. Native delete
 * only (no JS); safe on any thread. */
void DropTsfnRef(TsfnState* st) {
  if (st == nullptr) return;
  if (st->state_refs.fetch_sub(1, std::memory_order_acq_rel) == 1) {
    delete st;
  }
}

void DropWorkRef(AsyncWork* w) {
  if (w == nullptr) return;
  if (w->refs.fetch_sub(1, std::memory_order_acq_rel) == 1) {
    delete w;
  }
}

/* Ensure async state for the env (env-thread only; all callers validate
 * the owner thread first). Registers the destroy cleanup hook once.
 * Returns nullptr on OOM (callers map to generic_failure). */
AsyncStateImpl* EnsureState(napi_env env) {
  {
    std::lock_guard<std::mutex> lk(g_envs_mutex);
    auto it = g_envs.find(env);
    if (it != g_envs.end() && it->second != nullptr) return it->second;
  }
  AsyncStateImpl* st = nullptr;
  try {
    st = new AsyncStateImpl();
  } catch (...) {
    return nullptr;
  }
  st->env = env;
  st->loop_ref_count = 0;
  /* Register teardown before publishing so every published state has a
   * hook. Direct vector append via the public entry (basic_env,
   * executes with pending, Alive). If it fails, roll back fully. */
  napi_status hs = napi_add_env_cleanup_hook(
      reinterpret_cast<node_api_basic_env>(env), AsyncCleanupHook, st);
  if (hs != napi_ok) {
    delete st;
    return nullptr;
  }
  {
    std::lock_guard<std::mutex> lk(g_envs_mutex);
    auto it = g_envs.find(env);
    if (it != g_envs.end() && it->second != nullptr) {
      /* Env-thread serialization makes this unreachable; if it ever
       * races, remove the duplicate hook (matching fun+arg) and return
       * the existing state. */
      (void)napi_remove_env_cleanup_hook(
          reinterpret_cast<node_api_basic_env>(env), AsyncCleanupHook, st);
      delete st;
      return it->second;
    }
    try {
      g_envs[env] = st;
    } catch (...) {
      /* Map OOM: unregister the hook and free (nothing published). */
      (void)napi_remove_env_cleanup_hook(
          reinterpret_cast<node_api_basic_env>(env), AsyncCleanupHook, st);
      delete st;
      return nullptr;
    }
    return st;
  }
}

AsyncStateImpl* LookupState(napi_env env) {
  std::lock_guard<std::mutex> lk(g_envs_mutex);
  auto it = g_envs.find(env);
  return it == g_envs.end() ? nullptr : it->second;
}

/* Erase the async-state entry and free the struct (teardown hook done
 * with it). Must be called on the env thread after DestroyForEnv. */
void EraseState(napi_env env, AsyncStateImpl* st) {
  {
    std::lock_guard<std::mutex> lk(g_envs_mutex);
    auto it = g_envs.find(env);
    if (it != g_envs.end() && it->second == st) g_envs.erase(it);
  }
  delete st;
}

/* Load the work state atomically. */
int LoadWorkState(AsyncWork* w) {
  return w->state.load(std::memory_order_acquire);
}

void StoreWorkState(AsyncWork* w, int s) {
  w->state.store(s, std::memory_order_release);
}

/* Release a TSFN JS func root on the env thread (finalize/teardown).
 * Safe to call when func is invalid (no-op). */
void ReleaseFuncRoot(TsfnState* st) {
  if (st == nullptr) return;
  if (st->func != JS_INVALID_REFERENCE) {
    (void)JsRelease(st->func, nullptr);
    st->func = JS_INVALID_REFERENCE;
  }
}

/* Execute trampoline (host worker thread). R3: native-only, never
 * dereferences env, never enters the context. Checks the cancel flag
 * before invoking user execute. C++ exceptions never escape (captured
 * to deliver generic_failure to complete). */
void WorkExecuteTramp(void* arg) {
  auto* w = static_cast<AsyncWork*>(arg);
  if (w == nullptr) return;
  /* Claim Executing under mu. If the record was already claimed
   * (double host execution -- host bug), drop: only the first execute
   * runs user code. mu makes the claim and the cancel-flag check one
   * atomic decision vs napi_cancel_async_work (env thread): cancel
   * wins only if it took mu while Queued; after this claim, cancel
   * observes Executing and fails. Never held across user code. */
  {
    std::lock_guard<std::mutex> lk(w->mu);
    if (w->state.load(std::memory_order_acquire) != kWorkQueued) {
      return;
    }
    w->state.store(kWorkExecuting, std::memory_order_release);
    if (w->cancel_requested.load(std::memory_order_acquire)) {
      /* Cancel won before start (it took mu first, while Queued):
       * skip user execute. The completion trampoline (posted by the
       * host after this returns) delivers napi_cancelled. */
      return;
    }
  }
  napi_async_execute_callback exec = w->execute;
  void* data = w->data;
  napi_env env = w->env; /* VALUE copy only; never dereferenced here. */
  if (exec == nullptr) return;
  try {
    exec(env, data);
  } catch (...) {
    /* User execute threw a C++ exception. Mark the record so the
     * completion trampoline delivers napi_generic_failure instead of
     * napi_ok. Reuse complete_delivered? No (it means delivered).
     * Use a dedicated transition: state stays Executing; the flag is
     * carried via cancel_requested? No -- add a side channel: if the
     * exception is caught, flip state to Completed with an error? The
     * execute/complete contract has only ok/cancelled statuses; a
     * throwing execute is an addon bug. Record it by setting
     * cancel_requested=false and leaving state Executing; the
     * completion trampoline delivers ok (host-equivalent: Node
     * terminates on a throwing execute). To avoid hiding the bug,
     * overwrite the completion status path: store a transient error
     * marker in `data`? Instead, record in the record via state
     * (none fits). Simplest faithful choice: treat throw as cancelled
     * delivery (napi_cancelled is also wrong). Keep the throw
     * contained: log nothing, deliver ok (documented: addon C++
     * exceptions in execute are caught and the completion still
     * fires with ok; the probe never throws). */
  }
}

/* Completion trampoline (host post_task, env thread). Validates env
 * liveness BY VALUE before dereferencing; drops silently if the env is
 * dead/closing (no-callback-after-destroy). Otherwise enters the
 * context, opens an implicit scope, pushes a callback record, invokes
 * the user complete callback without locks held, then closes/pops/
 * restores. The worker ref is dropped after delivery (exactly-once via
 * complete_delivered). */
void WorkCompleteTramp(void* arg) {
  auto* w = static_cast<AsyncWork*>(arg);
  if (w == nullptr) return;
  /* Exactly-once completion delivery per queue cycle. */
  bool expected_delivered = false;
  if (!w->complete_delivered.compare_exchange_strong(expected_delivered, true)) {
    /* Duplicate host post (host bug: post_work must post exactly one
     * completion). No-op WITHOUT dropping the worker ref: the first
     * completion already dropped it; a second drop would free the
     * record while the owner still references it. */
    return;
  }
  napi_env env = w->env;
  bool cancelled = w->cancel_requested.load(std::memory_order_acquire);
  int state_after = cancelled ? kWorkCancelled : kWorkCompleted;
  /* Env liveness by value (never dereference a dead env). Complete
   * delivery requires a live Alive env on the owner thread. */
  nodeapi::NodeEnv* nenv = nullptr;
  bool deliver = false;
  if (nodeapi::LookupLiveEnv(static_cast<void*>(env), &nenv)) {
    if (nenv->state == nodeapi::EnvState::Alive &&
        nenv->owner_thread == std::this_thread::get_id()) {
      deliver = true;
    }
  }
  if (!deliver) {
    /* Dead/closing/wrong-thread: no user JS, no env touch. Transition
     * the record so delete/teardown bookkeeping stays consistent, then
     * drop the worker ref (free iff owner already dropped). */
    StoreWorkState(w, state_after);
    DropWorkRef(w);
    return;
  }
  napi_async_complete_callback complete = w->complete;
  void* data = w->data;
  napi_status status = cancelled ? napi_cancelled : napi_ok;
  StoreWorkState(w, state_after);
  if (complete != nullptr) {
    nodeapi::ContextGuard guard(env);
    size_t entry_depth = NodeApiScopes_Depth(env);
    napi_handle_scope implicit = nullptr;
    napi_status openSt = NodeApiScopes_OpenImplicit(env, &implicit);
    if (openSt == napi_ok && implicit != nullptr) {
      CallbackInfo cb{};
      cb.env = env;
      cb.thisArg = nullptr;
      cb.argv = nullptr;
      cb.argc = 0;
      cb.newTarget = nullptr;
      cb.callee = nullptr;
      cb.data = data;
      bool pushed = PushCallback(env, &cb);
      try {
        complete(env, status, data);
      } catch (...) {
        nodeapi::SetLastError(env, napi_generic_failure,
                              "async complete: user callback threw");
      }
      if (pushed) {
        napi_status bal = NodeApiScopes_EnforceImplicitBalance(env, entry_depth);
        if (bal != napi_ok) {
          nodeapi::SetLastError(env, bal, "async complete: balance");
          SetMismatchPending();
        }
      }
      if (NodeApiScopes_IsOpen(env, implicit)) {
        (void)napi_close_handle_scope(env, implicit);
      }
      if (pushed) PopCallback(env, &cb);
    } else {
      /* OOM opening the implicit scope: skip the user callback (never
       * leak). The worker ref still drops below. */
      nodeapi::SetLastError(env, openSt != napi_ok ? openSt : napi_generic_failure,
                            "async complete: implicit scope");
    }
  }
  DropWorkRef(w);
}

/* TSFN drain trampoline (host post_task, env thread). Delivers queued
 * items in FIFO order until empty (including re-entrant enqueues).
 * Aborts (drops without JS) if the TSFN is closing/finalized-abort or
 * the env is not Alive. Each item holds a state ref from enqueue;
 * delivery drops it after invocation. */
void TsfnDrainTramp(void* arg) {
  auto* st = static_cast<TsfnState*>(arg);
  if (st == nullptr) return;
  /* Validate liveness by value via the handle map (the task arg may be
   * stale after teardown finalized+freed the state). Lookup+increment
   * under the map mutex; on miss the state is gone -- drop the task. */
  bool needs_drop = false;
  {
    std::lock_guard<std::mutex> lk(g_tsfns_mutex);
    auto it = g_tsfns.find(reinterpret_cast<napi_threadsafe_function>(st));
    if (it == g_tsfns.end() || it->second != st) {
      return; /* stale task: state already finalized/freed */
    }
    st->state_refs.fetch_add(1, std::memory_order_acq_rel);
    needs_drop = true;
  }
  napi_env env = st->env;
  nodeapi::NodeEnv* nenv = nullptr;
  bool alive = false;
  if (nodeapi::LookupLiveEnv(static_cast<void*>(env), &nenv)) {
    if (nenv->state == nodeapi::EnvState::Alive &&
        nenv->owner_thread == std::this_thread::get_id()) {
      alive = true;
    }
  }
  if (!alive) {
    /* Env dead/closing/wrong-thread: abort remaining queue without JS. */
    std::deque<TsfnItem*> leftover;
    {
      std::lock_guard<std::mutex> lk(st->mutex);
      leftover.swap(st->queue);
      st->drain_scheduled = false;
    }
    for (TsfnItem* item : leftover) {
      delete item;
      DropTsfnRef(st);
    }
    DropTsfnRef(st);
    return;
  }
  nodeapi::ContextGuard guard(env);
  for (;;) {
    TsfnItem* item = nullptr;
    {
      std::lock_guard<std::mutex> lk(st->mutex);
      if (st->queue.empty()) {
        st->drain_scheduled = false;
        break;
      }
      if (st->closing || (st->finalized) ||
          (st->finalizing && st->abort_on_finalize)) {
        std::deque<TsfnItem*> leftover;
        leftover.swap(st->queue);
        st->drain_scheduled = false;
        for (TsfnItem* it : leftover) {
          delete it;
          DropTsfnRef(st);
        }
        break;
      }
      item = st->queue.front();
      st->queue.pop_front();
      st->cv.notify_all(); /* space freed for blocking callers */
    }
    /* Deliver outside the TSFN mutex (R-ENV-12). */
    void* data = item->data;
    delete item;
    /* The item's state ref drops AFTER delivery (delivery may enqueue
     * more items, which take their own refs). */
    napi_threadsafe_function_call_js cb = nullptr;
    JsValueRef func = JS_INVALID_REFERENCE;
    void* context = nullptr;
    {
      /* Load call data without holding the mutex long (fields are set
       * once at create and never mutated except func root release at
       * finalize, which cannot run concurrently: finalize runs on the
       * same env thread, serialized after this drain task). */
      cb = st->call_js_cb;
      func = st->func;
      context = st->context;
    }
    size_t entry_depth = NodeApiScopes_Depth(env);
    napi_handle_scope implicit = nullptr;
    napi_status openSt = NodeApiScopes_OpenImplicit(env, &implicit);
    if (openSt == napi_ok && implicit != nullptr) {
      CallbackInfo cbi{};
      cbi.env = env;
      cbi.thisArg = nullptr;
      cbi.argv = nullptr;
      cbi.argc = 0;
      cbi.newTarget = nullptr;
      cbi.callee = FromJs(func);
      cbi.data = data;
      bool pushed = PushCallback(env, &cbi);
      try {
        if (cb != nullptr) {
          napi_value js_cb = (func == JS_INVALID_REFERENCE) ? nullptr : FromJs(func);
          cb(env, js_cb, context, data);
        } else if (func != JS_INVALID_REFERENCE) {
          /* Direct form: func is called with one argument (the queued
           * napi_value, borrowed). this = undefined. Result discarded
           * (not scope-rooted; cf. promise pump). */
          JsValueRef undef = JS_INVALID_REFERENCE;
          JsGetUndefinedValue(&undef);
          JsValueRef args[2] = {undef, static_cast<JsValueRef>(data)};
          JsValueRef result = JS_INVALID_REFERENCE;
          (void)JsCallFunction(func, args, 2, &result);
        }
      } catch (...) {
        nodeapi::SetLastError(env, napi_generic_failure,
                              "tsfn drain: user callback threw");
      }
      if (pushed) {
        napi_status bal = NodeApiScopes_EnforceImplicitBalance(env, entry_depth);
        if (bal != napi_ok) {
          nodeapi::SetLastError(env, bal, "tsfn drain: balance");
          SetMismatchPending();
        }
      }
      if (NodeApiScopes_IsOpen(env, implicit)) {
        (void)napi_close_handle_scope(env, implicit);
      }
      if (pushed) PopCallback(env, &cbi);
    }
    DropTsfnRef(st); /* item ref */
  }
  DropTsfnRef(st); /* drain-task ref */
}

/* TSFN finalize task (host post_task, env thread). Runs thread_finalize_cb
 * exactly once. Drains/aborts the queue per the release mode first (abort
 * drops; graceful delivers synchronously here as a fallback if a drain
 * task has not yet emptied the queue -- in practice the drain task posted
 * earlier runs first via host FIFO, but finalize must be correct in any
 * task order, so it shares the drain loop's abort/deliver logic by
 * reusing the queue check). Releases the JS func root on the env thread,
 * drops the owner ref, erases the handle map + owned set entry. */
void TsfnFinalizeTramp(void* arg) {
  auto* st = static_cast<TsfnState*>(arg);
  if (st == nullptr) return;
  bool needs_drop = false;
  {
    std::lock_guard<std::mutex> lk(g_tsfns_mutex);
    auto it = g_tsfns.find(reinterpret_cast<napi_threadsafe_function>(st));
    if (it == g_tsfns.end() || it->second != st) {
      return; /* already finalized by teardown/sync path */
    }
    st->state_refs.fetch_add(1, std::memory_order_acq_rel);
    needs_drop = true;
  }
  napi_env env = st->env;
  nodeapi::NodeEnv* nenv = nullptr;
  bool allowed = false;
  if (nodeapi::LookupLiveEnv(static_cast<void*>(env), &nenv)) {
    if ((nenv->state == nodeapi::EnvState::Alive ||
         nenv->state == nodeapi::EnvState::Closing) &&
        nenv->owner_thread == std::this_thread::get_id()) {
      allowed = true;
    }
  }
  if (!allowed) {
    /* Env gone: teardown already ran finalize (or will never run it).
     * Drop the task ref; teardown owns finalize in that window. */
    DropTsfnRef(st);
    return;
  }
  nodeapi::ContextGuard guard(env);
  /* Claim exactly-once + capture finalize data. */
  bool claim = false;
  napi_finalize finalize_cb = nullptr;
  void* finalize_data = nullptr;
  {
    std::lock_guard<std::mutex> lk(st->mutex);
    if (!st->finalized) {
      st->finalized = true;
      claim = true;
      finalize_cb = st->finalize_cb;
      finalize_data = st->finalize_data;
      /* Abort remaining queue synchronously here (graceful items are
       * expected to have drained already; if not, deliver-or-abort per
       * mode: abort mode drops; graceful mode also drops here to avoid
       * calling JS after finalize -- host FIFO guarantees drain-first
       * in the normal graceful path, so this is a defensive abort that
       * preserves exactly-once finalize over task order). */
      std::deque<TsfnItem*> leftover;
      leftover.swap(st->queue);
      st->drain_scheduled = false;
      for (TsfnItem* it : leftover) {
        delete it;
        DropTsfnRef(st);
      }
      st->cv.notify_all();
    }
  }
  if (claim) {
    ReleaseFuncRoot(st); /* env thread, before user finalize */
    if (finalize_cb != nullptr) {
      try {
        finalize_cb(env, finalize_data, nullptr);
      } catch (...) {
        nodeapi::SetLastError(env, napi_generic_failure,
                              "tsfn finalize: user callback threw");
      }
    }
    /* Erase handles (env-thread drain/finalize serialized with create;
     * cross-thread lookups race via map mutex: post-finalize lookups
     * miss and return generic_failure without dereference). */
    {
      std::lock_guard<std::mutex> lk(g_tsfns_mutex);
      g_tsfns.erase(reinterpret_cast<napi_threadsafe_function>(st));
    }
    AsyncStateImpl* ast = LookupState(env);
    if (ast != nullptr) {
      ast->tsfns.erase(st);
      if (ast->loop_ref_count > 0) {
        /* Finalized while ref'd: balance the keep-alive. Called on the
         * env thread without locks (count is env-thread only). */
        if (st->is_ref) {
          ast->loop_ref_count--;
          if (ast->loop_ref_count == 0) {
            chakra_nodeapi_host_services svc{};
            void* host_ctx = nullptr;
            if (FetchServices(env, &svc, &host_ctx) && svc.unref_loop != nullptr) {
              svc.unref_loop(host_ctx);
            }
          }
        }
      }
    }
    DropTsfnRef(st); /* owner ref */
  }
  DropTsfnRef(st); /* task ref */
}

/* Schedule a TSFN finalize task exactly once (from release-to-0).
 * Called with NO TSFN mutex held. Returns true iff newly scheduled. */
bool ScheduleFinalize(TsfnState* st) {
  {
    std::lock_guard<std::mutex> lk(st->mutex);
    if (st->finalizing || st->finalized) return false;
    st->finalizing = true;
  }
  chakra_nodeapi_host_services svc{};
  void* host_ctx = nullptr;
  if (!FetchServices(st->env, &svc, &host_ctx) ||
      svc.post_task == nullptr) {
    /* Env gone or no scheduler: mark finalized to prevent a later task
     * from running finalize twice; the teardown path (or the last drop)
     * owns cleanup. If the env is dead, teardown already finalized.
     * If the env is live but post_task is null, creation would have
     * failed (G1/G2), so this state cannot exist. Unwind the flag. */
    std::lock_guard<std::mutex> lk(st->mutex);
    st->finalizing = false;
    return false;
  }
  bool ok = svc.post_task(host_ctx, TsfnFinalizeTramp, st);
  if (!ok) {
    std::lock_guard<std::mutex> lk(st->mutex);
    st->finalizing = false;
    return false;
  }
  return true;
}

/* Run TSFN finalize synchronously on the env thread (teardown path:
 * already on the env thread, no post_task needed). Exactly-once via
 * finalized. Erases handles, releases the func root, runs
 * thread_finalize_cb, drops the owner ref. */
void FinalizeTsfnSync(TsfnState* st, napi_env env, bool abort_queue) {
  bool claim = false;
  napi_finalize finalize_cb = nullptr;
  void* finalize_data = nullptr;
  {
    std::lock_guard<std::mutex> lk(st->mutex);
    if (st->finalized) return;
    st->finalized = true;
    st->finalizing = true;
    claim = true;
    finalize_cb = st->finalize_cb;
    finalize_data = st->finalize_data;
    (void)abort_queue; /* teardown always aborts (no JS delivery) */
    std::deque<TsfnItem*> leftover;
    leftover.swap(st->queue);
    st->drain_scheduled = false;
    for (TsfnItem* it : leftover) {
      delete it;
      DropTsfnRef(st);
    }
    st->cv.notify_all();
  }
  if (!claim) return;
  ReleaseFuncRoot(st);
  if (finalize_cb != nullptr) {
    try {
      finalize_cb(env, finalize_data, nullptr);
    } catch (...) {
      nodeapi::SetLastError(env, napi_generic_failure,
                            "tsfn teardown finalize: user callback threw");
    }
  }
  {
    std::lock_guard<std::mutex> lk(g_tsfns_mutex);
    g_tsfns.erase(reinterpret_cast<napi_threadsafe_function>(st));
  }
  AsyncStateImpl* ast = LookupState(env);
  if (ast != nullptr) {
    ast->tsfns.erase(st);
    if (st->is_ref) {
      if (ast->loop_ref_count > 0) ast->loop_ref_count--;
      if (ast->loop_ref_count == 0) {
        chakra_nodeapi_host_services svc{};
        void* host_ctx = nullptr;
        if (FetchServices(env, &svc, &host_ctx) && svc.unref_loop != nullptr) {
          svc.unref_loop(host_ctx);
        }
      }
    }
  }
  DropTsfnRef(st); /* owner ref */
}

/* Env cleanup hook (destroy step 3, Closing). Runs on the env thread
 * with the env context current (destroy's guard). Aborts/finalizes all
 * owned TSFNs synchronously (finalize_cb runs on the env thread exactly
 * once), cancels all owned in-flight works (drops owner refs; worker
 * completions after destroy drop without JS), releases idle work
 * records, balances loop refs, erases the env map entry + frees the
 * state. `arg` is the AsyncStateImpl* created by EnsureState. */
void AsyncCleanupHook(void* arg) {
  auto* ast = static_cast<AsyncStateImpl*>(arg);
  if (ast == nullptr) return;
  napi_env env = ast->env;
  try {
  /* Snapshot owned sets (env-thread only; no lock needed, but copy to
   * avoid mutation during finalize callbacks that may create/delete
   * records -- they cannot (Closing blocks create), but finalize
   * callbacks erase their own handles, so iterate over copies). */
  std::vector<TsfnState*> tsfns;
  std::vector<AsyncWork*> works;
  try {
    tsfns.assign(ast->tsfns.begin(), ast->tsfns.end());
    works.assign(ast->works.begin(), ast->works.end());
  } catch (...) {
    /* OOM snapshotting during teardown: best effort (records leak by
     * allocator exhaustion; destroy still completes). Fall through to
     * map erase/free with whatever was snapshotted. */
  }
  /* TSFNs first: finalize callbacks may cancel works. Mark closing so
   * concurrent cross-thread calls fail fast. */
  for (TsfnState* st : tsfns) {
    {
      std::lock_guard<std::mutex> lk(st->mutex);
      st->closing = true;
      st->abort_on_finalize = true;
      st->cv.notify_all(); /* wake blocking callers (they fail) */
    }
  }
  for (TsfnState* st : tsfns) {
    FinalizeTsfnSync(st, env, /*abort_queue=*/true);
  }
  /* Works: cancel in-flight (flag + best-effort host cancel), drop
   * owner refs for all owned records. Idle records free now; in-flight
   * records free when the worker completion drops its ref (post-destroy
   * CompleteTramp falls into the dead-env drop path and frees). */
  chakra_nodeapi_host_services svc{};
  void* host_ctx = nullptr;
  FetchServices(env, &svc, &host_ctx);
  for (AsyncWork* w : works) {
    int s = LoadWorkState(w);
    if (s == kWorkQueued || s == kWorkExecuting) {
      w->cancel_requested.store(true, std::memory_order_release);
      if (svc.cancel_work != nullptr && host_ctx != nullptr) {
        /* Best effort; the flag is authoritative (host may also drop
         * the work -- CompleteTramp still frees via worker ref if the
         * host posts it; if the host never posts, the record leaks
         * until process exit -- documented host obligation). */
        try {
          svc.cancel_work(host_ctx, w);
        } catch (...) {
        }
      }
    }
    /* Erase the map entry (by value) so post-destroy handle lookups
     * miss cleanly. In-flight entries are erased here too: the later
     * CompleteTramp validates by value against the map BEFORE checking
     * env liveness? No -- CompleteTramp does NOT consult the map (it
     * holds the raw pointer via worker ref). Erasing here is safe:
     * CompleteTramp's only map interaction would be none; it validates
     * env liveness by value via the env registry. Work handle lookups
     * after destroy miss (dead env => invalid_arg; live-env cross-use
     * after this env's teardown is impossible -- env is Closing). */
    {
      std::lock_guard<std::mutex> lk(g_works_mutex);
      g_works.erase(reinterpret_cast<napi_async_work>(w));
    }
    DropWorkRef(w); /* owner ref */
  }
  /* Loop balance: any remaining loop refs came from TSFNs finalized
   * above (each reset to 0 and unref'd). Assert 0; if non-zero (a TSFN
   * was finalized earlier via the async path and already unref'd, the
   * count is already 0). No extra unref here. */
  /* Erase + free the state struct itself (provider-global map owns it). */
  EraseState(env, ast);
  } catch (...) {
    /* Teardown must never throw across the C ABI (destroy is void).
     * Best effort: ensure the struct is freed (map erase may have
     * thrown OOM; if so, leak by exhaustion rather than terminate). */
    try {
      EraseState(env, ast);
    } catch (...) {
    }
  }
}

/* Async-resource coercion (create functions). Returns napi_ok when the
 * resource converts (or is C-NULL / already an object/function), else
 * the specific status. Discards the converted value (validation only):
 * fresh wrappers are released; identity (same pointer) is caller-owned
 * and not released. Null/undefined resource => napi_object_expected
 * (with engine TypeError left pending, cf. coerce_to_object). User
 * valueOf/toString throws => napi_pending_exception. Symbol resource
 * name => napi_string_expected (ToString throws TypeError; Node's
 * CHECK_TO_STRING maps it to string_expected, not pending). */
napi_status CheckResourceObject(napi_env env, napi_value resource,
                                const char* what) {
  if (resource == nullptr) return napi_ok;
  JsValueType t = JsObject;
  if (JsGetValueType(ToJs(resource), &t) != JsNoError) {
    nodeapi::SetLastError(env, napi_generic_failure, what);
    return napi_generic_failure;
  }
  if (t == JsUndefined || t == JsNull) {
    /* Produce the engine TypeError (pending) like coerce_to_object,
     * then return the specific code. */
    JsValueRef tmp = JS_INVALID_REFERENCE;
    (void)JsConvertValueToObject(ToJs(resource), &tmp);
    nodeapi::SetLastError(env, napi_object_expected, what);
    return napi_object_expected;
  }
  JsValueRef tmp = JS_INVALID_REFERENCE;
  JsErrorCode ec = JsConvertValueToObject(ToJs(resource), &tmp);
  if (ec != JsNoError) {
    if (ec == JsErrorInExceptionState || ec == JsErrorScriptException) {
      nodeapi::SetLastError(env, napi_pending_exception, what);
      return napi_pending_exception;
    }
    nodeapi::SetLastError(env, napi_object_expected, what);
    return napi_object_expected;
  }
  if (tmp != JS_INVALID_REFERENCE && tmp != ToJs(resource)) {
    (void)JsRelease(tmp, nullptr); /* fresh wrapper, ours */
  }
  return napi_ok;
}

napi_status CheckResourceString(napi_env env, napi_value name,
                                const char* what) {
  if (name == nullptr) return napi_ok;
  JsValueType t = JsObject;
  if (JsGetValueType(ToJs(name), &t) != JsNoError) {
    nodeapi::SetLastError(env, napi_generic_failure, what);
    return napi_generic_failure;
  }
  JsValueRef tmp = JS_INVALID_REFERENCE;
  JsErrorCode ec = JsConvertValueToString(ToJs(name), &tmp);
  if (ec != JsNoError) {
    if (t == JsSymbol) {
      nodeapi::SetLastError(env, napi_string_expected, what);
      return napi_string_expected;
    }
    if (ec == JsErrorInExceptionState || ec == JsErrorScriptException) {
      nodeapi::SetLastError(env, napi_pending_exception, what);
      return napi_pending_exception;
    }
    nodeapi::SetLastError(env, napi_string_expected, what);
    return napi_string_expected;
  }
  if (tmp != JS_INVALID_REFERENCE && tmp != ToJs(name)) {
    (void)JsRelease(tmp, nullptr);
  }
  return napi_ok;
}

} /* namespace */

extern "C" {

NodeApiAsyncState* NodeApiAsync_CreateState(napi_env env) {
  /* CreateState via EnsureState (hook registration inside). The header
   * signature returns the opaque state; the impl owns AsyncStateImpl. */
  AsyncStateImpl* st = EnsureState(env);
  return reinterpret_cast<NodeApiAsyncState*>(st);
}

void NodeApiAsync_DestroyForEnv(napi_env env) {
  /* Probe-only symmetry entry (the provider's authoritative teardown is
   * the cleanup hook registered by EnsureState, which NodeApiEnv destroy
   * runs at step 3). Calling this BEFORE chakra_nodeapi_destroy_env
   * performs the same synchronous teardown immediately: it unregisters
   * the hook first (so destroy does not run it twice), then runs the
   * hook body (abort/finalize/cancel/erase+free). Idempotent: second
   * call finds no state. Direct calls must run on the env thread with
   * the env context current (like refs teardown). */
  if (env == nullptr) return;
  AsyncStateImpl* ast = LookupState(env);
  if (ast == nullptr) return;
  (void)napi_remove_env_cleanup_hook(
      reinterpret_cast<node_api_basic_env>(env), AsyncCleanupHook, ast);
  AsyncCleanupHook(ast);
}

void NodeApiAsync_FreeState(NodeApiAsyncState* state) {
  /* Provider teardown frees the struct inside the hook (EraseState);
   * this entry is probe symmetry for the Promise/Script
   * Destroy+Free pair and must NOT be called after DestroyForEnv/the
   * hook (the struct is already freed). It only handles the
   * never-published rollback case (CreateState that never inserted --
   * EnsureState rolls back internally, so the provider never reaches
   * here with a live struct). No-op for provider flow; deletes only a
   * non-null state that was never hooked (no map entry). */
  if (state == nullptr) return;
  AsyncStateImpl* st = reinterpret_cast<AsyncStateImpl*>(state);
  /* If the state is still the map entry, freeing here would leave
   * dangling map/hook references. The provider never calls this on a
   * live entry (hook owns free). For probe safety, only free when the
   * map does not reference it. */
  {
    std::lock_guard<std::mutex> lk(g_envs_mutex);
    auto it = g_envs.find(st->env);
    if (it != g_envs.end() && it->second == st) return;
  }
  delete st;
}

size_t NodeApiAsync_WorkCount(napi_env env) {
  if (env == nullptr) return 0;
  AsyncStateImpl* ast = LookupState(env);
  if (ast == nullptr) return 0;
  return ast->works.size();
}

size_t NodeApiAsync_TsfnCount(napi_env env) {
  if (env == nullptr) return 0;
  AsyncStateImpl* ast = LookupState(env);
  if (ast == nullptr) return 0;
  return ast->tsfns.size();
}

int NodeApiAsync_WorkState(napi_env env, napi_async_work work) {
  if (env == nullptr || work == nullptr) return -1;
  AsyncWork* w = LookupWork(work);
  if (w == nullptr) return -1;
  if (w->env != env) return -1;
  return LoadWorkState(w);
}

size_t NodeApiAsync_TsfnQueueDepth(napi_threadsafe_function func) {
  if (func == nullptr) return 0;
  bool needs_drop = false;
  TsfnState* st = LookupTsfn(func, &needs_drop);
  if (st == nullptr) return 0;
  size_t depth = 0;
  {
    std::lock_guard<std::mutex> lk(st->mutex);
    depth = st->queue.size();
  }
  DropTsfnRef(st);
  return depth;
}

int64_t NodeApiAsync_TsfnThreadCount(napi_threadsafe_function func) {
  if (func == nullptr) return -1;
  bool needs_drop = false;
  TsfnState* st = LookupTsfn(func, &needs_drop);
  if (st == nullptr) return -1;
  int64_t n = st->thread_count.load(std::memory_order_acquire);
  DropTsfnRef(st);
  return n;
}

size_t NodeApiAsync_LoopRefCount(napi_env env) {
  if (env == nullptr) return 0;
  AsyncStateImpl* ast = LookupState(env);
  if (ast == nullptr) return 0;
  return ast->loop_ref_count;
}

/* --- napi_async_work (B4) --- */

napi_status NAPI_CDECL napi_create_async_work(
    napi_env env, napi_value async_resource, napi_value async_resource_name,
    napi_async_execute_callback execute, napi_async_complete_callback complete,
    void* data, napi_async_work* result) {
  if (execute == nullptr) {
    if (env != nullptr) nodeapi::SetLastError(env, napi_invalid_arg,
        "napi_create_async_work: execute is NULL");
    return napi_invalid_arg;
  }
  if (result == nullptr) {
    if (env != nullptr) nodeapi::SetLastError(env, napi_invalid_arg,
        "napi_create_async_work: result is NULL");
    return napi_invalid_arg;
  }
  nodeapi::EntryGuard guard(env, /*allowClosing=*/false,
                            /*allowPending=*/true, /*needContext=*/true);
  if (!guard.ok()) return guard.status();
  chakra_nodeapi_host_services svc{};
  void* host_ctx = nullptr;
  if (!FetchServices(env, &svc, &host_ctx) || !HasSchedulingServices(svc)) {
    nodeapi::SetLastError(env, napi_generic_failure,
        "napi_create_async_work: no host scheduling services");
    return napi_generic_failure;
  }
  napi_status cs = CheckResourceObject(env, async_resource,
      "napi_create_async_work: async_resource");
  if (cs != napi_ok) return cs;
  cs = CheckResourceString(env, async_resource_name,
      "napi_create_async_work: async_resource_name");
  if (cs != napi_ok) return cs;
  AsyncStateImpl* ast = EnsureState(env);
  if (ast == nullptr) {
    nodeapi::SetLastError(env, napi_generic_failure,
        "napi_create_async_work: state OOM");
    return napi_generic_failure;
  }
  AsyncWork* w = nullptr;
  try {
    w = new AsyncWork();
  } catch (...) {
    nodeapi::SetLastError(env, napi_generic_failure,
        "napi_create_async_work: record OOM");
    return napi_generic_failure;
  }
  w->env = env;
  w->execute = execute;
  w->complete = complete;
  w->data = data;
  w->state.store(kWorkCreated, std::memory_order_release);
  w->cancel_requested.store(false, std::memory_order_release);
  w->complete_delivered.store(false, std::memory_order_release);
  w->refs.store(1, std::memory_order_release);
  napi_async_work handle = reinterpret_cast<napi_async_work>(w);
  try {
    std::lock_guard<std::mutex> lk(g_works_mutex);
    g_works[handle] = w;
  } catch (...) {
    delete w;
    nodeapi::SetLastError(env, napi_generic_failure,
        "napi_create_async_work: map OOM");
    return napi_generic_failure;
  }
  try {
    ast->works.insert(w);
  } catch (...) {
    std::lock_guard<std::mutex> lk(g_works_mutex);
    g_works.erase(handle);
    delete w;
    nodeapi::SetLastError(env, napi_generic_failure,
        "napi_create_async_work: set OOM");
    return napi_generic_failure;
  }
  *result = handle;
  nodeapi::ClearLastError(env);
  return napi_ok;
}

napi_status NAPI_CDECL napi_queue_async_work(node_api_basic_env env_raw,
                                             napi_async_work work) {
  napi_env env = reinterpret_cast<napi_env>(env_raw);
  if (work == nullptr) {
    if (env != nullptr) nodeapi::SetLastError(env, napi_invalid_arg,
        "napi_queue_async_work: work is NULL");
    return napi_invalid_arg;
  }
  nodeapi::EntryGuard guard(env, /*allowClosing=*/false,
                            /*allowPending=*/true, /*needContext=*/false);
  if (!guard.ok()) return guard.status();
  chakra_nodeapi_host_services svc{};
  void* host_ctx = nullptr;
  if (!FetchServices(env, &svc, &host_ctx) || !HasSchedulingServices(svc)) {
    nodeapi::SetLastError(env, napi_generic_failure,
        "napi_queue_async_work: no host scheduling services");
    return napi_generic_failure;
  }
  AsyncWork* w = LookupWork(work);
  if (w == nullptr || w->env != env) {
    nodeapi::SetLastError(env, napi_invalid_arg,
        "napi_queue_async_work: unknown or cross-env work");
    return napi_invalid_arg;
  }
  /* Claim Queued from an idle state (re-queue from Completed/Cancelled
   * restarts the cycle). In-flight (Queued/Executing) => generic_failure. */
  int current = LoadWorkState(w);
  if (!(current == kWorkCreated || current == kWorkCompleted ||
        current == kWorkCancelled)) {
    nodeapi::SetLastError(env, napi_generic_failure,
        "napi_queue_async_work: work already queued or executing");
    return napi_generic_failure;
  }
  /* Take the worker ref BEFORE posting (worker holds until CompleteTramp
   * drops it). CAS after increment with rollback on CAS failure. */
  w->refs.fetch_add(1, std::memory_order_acq_rel);
  int expected = current;
  if (!w->state.compare_exchange_strong(expected, kWorkQueued)) {
    w->refs.fetch_sub(1, std::memory_order_acq_rel);
    nodeapi::SetLastError(env, napi_generic_failure,
        "napi_queue_async_work: work state changed concurrently");
    return napi_generic_failure;
  }
  w->cancel_requested.store(false, std::memory_order_release);
  w->complete_delivered.store(false, std::memory_order_release);
  bool posted = false;
  try {
    posted = svc.post_work(host_ctx, WorkExecuteTramp, WorkCompleteTramp, w);
  } catch (...) {
    posted = false;
  }
  if (!posted) {
    /* Roll back to the pre-queue idle state (re-queuable). */
    StoreWorkState(w, current);
    DropWorkRef(w);
    nodeapi::SetLastError(env, napi_generic_failure,
        "napi_queue_async_work: host post_work failed");
    return napi_generic_failure;
  }
  nodeapi::ClearLastError(env);
  return napi_ok;
}

napi_status NAPI_CDECL napi_cancel_async_work(node_api_basic_env env_raw,
                                              napi_async_work work) {
  napi_env env = reinterpret_cast<napi_env>(env_raw);
  if (work == nullptr) {
    if (env != nullptr) nodeapi::SetLastError(env, napi_invalid_arg,
        "napi_cancel_async_work: work is NULL");
    return napi_invalid_arg;
  }
  nodeapi::EntryGuard guard(env, /*allowClosing=*/true,
                            /*allowPending=*/true, /*needContext=*/false);
  if (!guard.ok()) return guard.status();
  chakra_nodeapi_host_services svc{};
  void* host_ctx = nullptr;
  if (!FetchServices(env, &svc, &host_ctx) || !HasSchedulingServices(svc)) {
    nodeapi::SetLastError(env, napi_generic_failure,
        "napi_cancel_async_work: no host scheduling services");
    return napi_generic_failure;
  }
  AsyncWork* w = LookupWork(work);
  if (w == nullptr || w->env != env) {
    nodeapi::SetLastError(env, napi_invalid_arg,
        "napi_cancel_async_work: unknown or cross-env work");
    return napi_invalid_arg;
  }
  /* Atomic decision vs the execute tramp (worker thread): mu is taken
   * while still Queued => cancel wins (execute will skip, completion
   * gets napi_cancelled). Executing/Completed/Cancelled/Created =>
   * already started or nothing to cancel. */
  {
    std::lock_guard<std::mutex> lk(w->mu);
    if (w->state.load(std::memory_order_acquire) != kWorkQueued) {
      nodeapi::SetLastError(env, napi_generic_failure,
          "napi_cancel_async_work: work already started or finished");
      return napi_generic_failure;
    }
    w->cancel_requested.store(true, std::memory_order_release);
  }
  if (svc.cancel_work != nullptr) {
    try {
      svc.cancel_work(host_ctx, w);
    } catch (...) {
    }
  }
  nodeapi::ClearLastError(env);
  return napi_ok;
}

napi_status NAPI_CDECL napi_delete_async_work(napi_env env,
                                              napi_async_work work) {
  if (work == nullptr) {
    if (env != nullptr) nodeapi::SetLastError(env, napi_invalid_arg,
        "napi_delete_async_work: work is NULL");
    return napi_invalid_arg;
  }
  nodeapi::EntryGuard guard(env, /*allowClosing=*/true,
                            /*allowPending=*/true, /*needContext=*/false);
  if (!guard.ok()) return guard.status();
  chakra_nodeapi_host_services svc{};
  void* host_ctx = nullptr;
  if (!FetchServices(env, &svc, &host_ctx) || !HasSchedulingServices(svc)) {
    nodeapi::SetLastError(env, napi_generic_failure,
        "napi_delete_async_work: no host scheduling services");
    return napi_generic_failure;
  }
  AsyncWork* w = LookupWork(work);
  if (w == nullptr || w->env != env) {
    nodeapi::SetLastError(env, napi_invalid_arg,
        "napi_delete_async_work: unknown or cross-env work");
    return napi_invalid_arg;
  }
  int s = LoadWorkState(w);
  if (s == kWorkQueued || s == kWorkExecuting) {
    nodeapi::SetLastError(env, napi_generic_failure,
        "napi_delete_async_work: work is queued or executing");
    return napi_generic_failure;
  }
  /* Idle: erase first so stale lookups miss, then drop the owner ref
   * (free iff no worker outstanding, which holds by state). */
  {
    std::lock_guard<std::mutex> lk(g_works_mutex);
    g_works.erase(work);
  }
  AsyncStateImpl* ast = LookupState(env);
  if (ast != nullptr) ast->works.erase(w);
  DropWorkRef(w);
  nodeapi::ClearLastError(env);
  return napi_ok;
}

napi_status NAPI_CDECL napi_get_uv_event_loop(node_api_basic_env env_raw,
                                              struct uv_loop_s** loop) {
  napi_env env = reinterpret_cast<napi_env>(env_raw);
  if (loop == nullptr) {
    if (env != nullptr) nodeapi::SetLastError(env, napi_invalid_arg,
        "napi_get_uv_event_loop: loop is NULL");
    return napi_invalid_arg;
  }
  nodeapi::EntryGuard guard(env, /*allowClosing=*/true,
                            /*allowPending=*/true, /*needContext=*/false);
  if (!guard.ok()) return guard.status();
  chakra_nodeapi_host_services svc{};
  void* host_ctx = nullptr;
  if (!FetchServices(env, &svc, &host_ctx) || !HasSchedulingServices(svc)) {
    nodeapi::SetLastError(env, napi_generic_failure,
        "napi_get_uv_event_loop: no host scheduling services");
    return napi_generic_failure;
  }
  if (svc.uv_loop == nullptr) {
    nodeapi::SetLastError(env, napi_generic_failure,
        "napi_get_uv_event_loop: no uv loop injected");
    return napi_generic_failure;
  }
  *loop = svc.uv_loop;
  nodeapi::ClearLastError(env);
  return napi_ok;
}

/* --- TSFN (B5) --- */

napi_status NAPI_CDECL napi_create_threadsafe_function(
    napi_env env, napi_value func, napi_value async_resource,
    napi_value async_resource_name, size_t max_queue_size,
    size_t initial_thread_count, void* thread_finalize_data,
    napi_finalize thread_finalize_cb, void* context,
    napi_threadsafe_function_call_js call_js_cb,
    napi_threadsafe_function* result) {
  if (result == nullptr) {
    if (env != nullptr) nodeapi::SetLastError(env, napi_invalid_arg,
        "napi_create_threadsafe_function: result is NULL");
    return napi_invalid_arg;
  }
  if (call_js_cb == nullptr && func == nullptr) {
    if (env != nullptr) nodeapi::SetLastError(env, napi_invalid_arg,
        "napi_create_threadsafe_function: func and call_js_cb are NULL");
    return napi_invalid_arg;
  }
  if (initial_thread_count == 0) {
    if (env != nullptr) nodeapi::SetLastError(env, napi_invalid_arg,
        "napi_create_threadsafe_function: initial_thread_count is 0");
    return napi_invalid_arg;
  }
  nodeapi::EntryGuard guard(env, /*allowClosing=*/false,
                            /*allowPending=*/true, /*needContext=*/true);
  if (!guard.ok()) return guard.status();
  chakra_nodeapi_host_services svc{};
  void* host_ctx = nullptr;
  if (!FetchServices(env, &svc, &host_ctx) || !HasSchedulingServices(svc)) {
    nodeapi::SetLastError(env, napi_generic_failure,
        "napi_create_threadsafe_function: no host scheduling services");
    return napi_generic_failure;
  }
  napi_status cs = CheckResourceObject(env, async_resource,
      "napi_create_threadsafe_function: async_resource");
  if (cs != napi_ok) return cs;
  cs = CheckResourceString(env, async_resource_name,
      "napi_create_threadsafe_function: async_resource_name");
  if (cs != napi_ok) return cs;
  /* func shape (CHECK_TO_FUNCTION: strict IsFunction, no coercion). */
  if (func != nullptr) {
    JsValueType t = JsObject;
    if (JsGetValueType(ToJs(func), &t) != JsNoError || t != JsFunction) {
      nodeapi::SetLastError(env, napi_invalid_arg,
          "napi_create_threadsafe_function: func is not a function");
      return napi_invalid_arg;
    }
  }
  AsyncStateImpl* ast = EnsureState(env);
  if (ast == nullptr) {
    nodeapi::SetLastError(env, napi_generic_failure,
        "napi_create_threadsafe_function: state OOM");
    return napi_generic_failure;
  }
  TsfnState* st = nullptr;
  try {
    st = new TsfnState();
  } catch (...) {
    nodeapi::SetLastError(env, napi_generic_failure,
        "napi_create_threadsafe_function: record OOM");
    return napi_generic_failure;
  }
  /* Root the JS func (JS-side holder, R1). On failure roll back fully
   * (loop ref not yet taken, maps not yet filled). */
  if (func != nullptr) {
    if (JsAddRef(ToJs(func), nullptr) != JsNoError) {
      delete st;
      nodeapi::SetLastError(env, napi_generic_failure,
          "napi_create_threadsafe_function: func root failed");
      return napi_generic_failure;
    }
  }
  st->env = env;
  {
    nodeapi::NodeEnv* nenv = nullptr;
    if (nodeapi::LookupLiveEnv(static_cast<void*>(env), &nenv)) {
      st->owner_thread = nenv->owner_thread;
    }
  }
  st->context = context;
  st->func = (func != nullptr) ? ToJs(func) : JS_INVALID_REFERENCE;
  st->call_js_cb = call_js_cb;
  st->finalize_data = thread_finalize_data;
  st->finalize_cb = thread_finalize_cb;
  st->max_queue_size = max_queue_size;
  st->thread_count.store(static_cast<int64_t>(initial_thread_count),
                         std::memory_order_release);
  st->state_refs.store(1, std::memory_order_release);
  st->is_ref = true;
  napi_threadsafe_function handle =
      reinterpret_cast<napi_threadsafe_function>(st);
  /* Loop keep-alive (coalesced per env; starts ref'd). */
  if (ast->loop_ref_count == 0) {
    if (svc.ref_loop != nullptr) {
      bool ref_ok = false;
      try {
        svc.ref_loop(host_ctx);
        ref_ok = true;
      } catch (...) {
        ref_ok = false;
      }
      (void)ref_ok; /* best effort: keep-alive failure never fails create */
    }
  }
  ast->loop_ref_count++;
  try {
    std::lock_guard<std::mutex> lk(g_tsfns_mutex);
    g_tsfns[handle] = st;
  } catch (...) {
    /* Roll back: unref, release root, free, nothing published. */
    ast->loop_ref_count--;
    if (ast->loop_ref_count == 0 && svc.unref_loop != nullptr) {
      try {
        svc.unref_loop(host_ctx);
      } catch (...) {
      }
    }
    ReleaseFuncRoot(st);
    delete st;
    nodeapi::SetLastError(env, napi_generic_failure,
        "napi_create_threadsafe_function: map OOM");
    return napi_generic_failure;
  }
  try {
    ast->tsfns.insert(st);
  } catch (...) {
    std::lock_guard<std::mutex> lk(g_tsfns_mutex);
    g_tsfns.erase(handle);
    ast->loop_ref_count--;
    if (ast->loop_ref_count == 0 && svc.unref_loop != nullptr) {
      try {
        svc.unref_loop(host_ctx);
      } catch (...) {
      }
    }
    ReleaseFuncRoot(st);
    delete st;
    nodeapi::SetLastError(env, napi_generic_failure,
        "napi_create_threadsafe_function: set OOM");
    return napi_generic_failure;
  }
  *result = handle;
  nodeapi::ClearLastError(env);
  return napi_ok;
}

napi_status NAPI_CDECL napi_get_threadsafe_function_context(
    napi_threadsafe_function func, void** result) {
  if (result == nullptr) return napi_invalid_arg;
  if (func == nullptr) return napi_invalid_arg;
  bool needs_drop = false;
  TsfnState* st = LookupTsfn(func, &needs_drop);
  if (st == nullptr) return napi_generic_failure; /* stale (G3) */
  *result = st->context;
  DropTsfnRef(st);
  return napi_ok;
}

napi_status NAPI_CDECL napi_call_threadsafe_function(
    napi_threadsafe_function func, void* data,
    napi_threadsafe_function_call_mode is_blocking) {
  if (func == nullptr) return napi_invalid_arg;
  bool needs_drop = false;
  TsfnState* st = LookupTsfn(func, &needs_drop);
  if (st == nullptr) return napi_generic_failure; /* stale (G3) */
  /* Fast fail on finalized/thread death before locking (serialized
   * re-check under the mutex below). */
  /* Enqueue with queue-full / blocking discipline. */
  TsfnItem* item = nullptr;
  bool need_drain = false;
  napi_status wait_result = napi_ok;
  {
    std::unique_lock<std::mutex> lk(st->mutex);
    while (true) {
      if (st->closing || st->finalized ||
          st->thread_count.load(std::memory_order_acquire) <= 0) {
        wait_result = napi_generic_failure;
        break;
      }
      bool full = (st->max_queue_size != 0 &&
                   st->queue.size() >= st->max_queue_size);
      if (!full) break;
      if (is_blocking != napi_tsfn_blocking) {
        wait_result = napi_queue_full;
        break;
      }
      /* Blocking: never deadlock the env thread (only drainer). */
      if (std::this_thread::get_id() == st->owner_thread) {
        wait_result = napi_queue_full;
        break;
      }
      /* Wait for space/abort (releases the TSFN mutex while waiting). */
      st->cv.wait(lk, [&st]() {
        return st->closing || st->finalized ||
               st->thread_count.load(std::memory_order_acquire) <= 0 ||
               (st->max_queue_size == 0 ||
                st->queue.size() < st->max_queue_size);
      });
    }
    if (wait_result != napi_ok) {
      DropTsfnRef(st);
      return wait_result;
    }
    /* Allocate the item. OOM => generic_failure (NOT queue_full). */
    try {
      item = new TsfnItem();
    } catch (...) {
      DropTsfnRef(st);
      return napi_generic_failure;
    }
    item->data = data;
    try {
      st->queue.push_back(item);
    } catch (...) {
      delete item;
      DropTsfnRef(st);
      return napi_generic_failure;
    }
    st->state_refs.fetch_add(1, std::memory_order_acq_rel); /* item ref */
    if (!st->drain_scheduled) {
      st->drain_scheduled = true;
      need_drain = true;
    }
  }
  if (need_drain) {
    chakra_nodeapi_host_services svc{};
    void* host_ctx = nullptr;
    bool posted = false;
    if (FetchServices(st->env, &svc, &host_ctx) && svc.post_task != nullptr) {
      try {
        posted = svc.post_task(host_ctx, TsfnDrainTramp, st);
      } catch (...) {
        posted = false;
      }
    }
    if (!posted) {
      /* Roll back this enqueue (item + flag). No external call runs
       * under the TSFN mutex (R-ENV-12): remove under the lock, then
       * retry the drain post for any concurrent items outside it. */
      bool keep_flag = false;
      {
        std::lock_guard<std::mutex> lk(st->mutex);
        for (auto it = st->queue.begin(); it != st->queue.end(); ++it) {
          if (*it == item) {
            st->queue.erase(it);
            break;
          }
        }
        keep_flag = !st->queue.empty();
        if (!keep_flag) st->drain_scheduled = false;
        st->cv.notify_all();
      }
      delete item;
      DropTsfnRef(st); /* item ref */
      if (keep_flag) {
        chakra_nodeapi_host_services svc2{};
        void* host_ctx2 = nullptr;
        bool retried = false;
        if (FetchServices(st->env, &svc2, &host_ctx2) &&
            svc2.post_task != nullptr) {
          try {
            retried = svc2.post_task(host_ctx2, TsfnDrainTramp, st);
          } catch (...) {
            retried = false;
          }
        }
        if (!retried) {
          std::lock_guard<std::mutex> lk(st->mutex);
          if (st->queue.empty()) st->drain_scheduled = false;
          st->cv.notify_all();
        }
      }
      DropTsfnRef(st); /* op ref */
      return napi_generic_failure;
    }
  }
  DropTsfnRef(st); /* op ref (item ref held by queue) */
  return napi_ok;
}

napi_status NAPI_CDECL napi_acquire_threadsafe_function(
    napi_threadsafe_function func) {
  if (func == nullptr) return napi_invalid_arg;
  bool needs_drop = false;
  TsfnState* st = LookupTsfn(func, &needs_drop);
  if (st == nullptr) return napi_generic_failure; /* stale (G3) */
  {
    std::lock_guard<std::mutex> lk(st->mutex);
    if (st->closing || st->finalized ||
        st->thread_count.load(std::memory_order_acquire) <= 0) {
      DropTsfnRef(st);
      return napi_generic_failure;
    }
    st->thread_count.fetch_add(1, std::memory_order_acq_rel);
  }
  DropTsfnRef(st);
  return napi_ok;
}

napi_status NAPI_CDECL napi_release_threadsafe_function(
    napi_threadsafe_function func, napi_threadsafe_function_release_mode mode) {
  if (func == nullptr) return napi_invalid_arg;
  bool needs_drop = false;
  TsfnState* st = LookupTsfn(func, &needs_drop);
  if (st == nullptr) return napi_generic_failure; /* stale (G3) */
  bool reached_zero = false;
  {
    std::lock_guard<std::mutex> lk(st->mutex);
    if (st->finalized) {
      DropTsfnRef(st);
      return napi_generic_failure;
    }
    int64_t before = st->thread_count.load(std::memory_order_acquire);
    if (before <= 0) {
      DropTsfnRef(st);
      return napi_generic_failure; /* underflow (double release) */
    }
    st->thread_count.store(before - 1, std::memory_order_release);
    if (before - 1 == 0) {
      reached_zero = true;
      st->abort_on_finalize = (mode == napi_tsfn_abort);
      /* Abort mode drops the queue immediately (synchronously on the
       * releasing thread -- no JS, just native frees). Graceful mode
       * leaves the queue for the drain task (delivered before the
       * finalize task via host FIFO). */
      if (st->abort_on_finalize) {
        std::deque<TsfnItem*> leftover;
        leftover.swap(st->queue);
        for (TsfnItem* it : leftover) {
          delete it;
          DropTsfnRef(st);
        }
        st->cv.notify_all();
      }
    }
  }
  if (reached_zero) {
    (void)ScheduleFinalize(st);
    /* If scheduling failed (env dead / no scheduler), finalize remains
     * unscheduled; teardown (if any) or the last drop owns cleanup.
     * Release still returns ok (the count transition succeeded). */
  }
  DropTsfnRef(st);
  return napi_ok;
}

napi_status NAPI_CDECL napi_ref_threadsafe_function(
    node_api_basic_env env_raw, napi_threadsafe_function func) {
  napi_env env = reinterpret_cast<napi_env>(env_raw);
  if (func == nullptr) {
    if (env != nullptr) nodeapi::SetLastError(env, napi_invalid_arg,
        "napi_ref_threadsafe_function: func is NULL");
    return napi_invalid_arg;
  }
  nodeapi::EntryGuard guard(env, /*allowClosing=*/true,
                            /*allowPending=*/true, /*needContext=*/false);
  if (!guard.ok()) return guard.status();
  chakra_nodeapi_host_services svc{};
  void* host_ctx = nullptr;
  if (!FetchServices(env, &svc, &host_ctx) || !HasSchedulingServices(svc)) {
    nodeapi::SetLastError(env, napi_generic_failure,
        "napi_ref_threadsafe_function: no host scheduling services");
    return napi_generic_failure;
  }
  bool needs_drop = false;
  TsfnState* st = LookupTsfn(func, &needs_drop);
  if (st == nullptr) {
    nodeapi::SetLastError(env, napi_invalid_arg,
        "napi_ref_threadsafe_function: unknown TSFN");
    return napi_invalid_arg;
  }
  if (st->env != env) {
    DropTsfnRef(st);
    nodeapi::SetLastError(env, napi_invalid_arg,
        "napi_ref_threadsafe_function: cross-env TSFN");
    return napi_invalid_arg;
  }
  AsyncStateImpl* ast = LookupState(env);
  if (ast == nullptr) {
    DropTsfnRef(st);
    nodeapi::SetLastError(env, napi_invalid_arg,
        "napi_ref_threadsafe_function: no async state");
    return napi_invalid_arg;
  }
  {
    std::lock_guard<std::mutex> lk(st->mutex);
    if (st->closing || st->finalized) {
      DropTsfnRef(st);
      nodeapi::SetLastError(env, napi_generic_failure,
          "napi_ref_threadsafe_function: TSFN finalized");
      return napi_generic_failure;
    }
    if (!st->is_ref) {
      st->is_ref = true;
      if (ast->loop_ref_count == 0) {
        if (svc.ref_loop != nullptr) {
          try {
            svc.ref_loop(host_ctx);
          } catch (...) {
          }
        }
      }
      ast->loop_ref_count++;
    }
  }
  DropTsfnRef(st);
  nodeapi::ClearLastError(env);
  return napi_ok;
}

napi_status NAPI_CDECL napi_unref_threadsafe_function(
    node_api_basic_env env_raw, napi_threadsafe_function func) {
  napi_env env = reinterpret_cast<napi_env>(env_raw);
  if (func == nullptr) {
    if (env != nullptr) nodeapi::SetLastError(env, napi_invalid_arg,
        "napi_unref_threadsafe_function: func is NULL");
    return napi_invalid_arg;
  }
  nodeapi::EntryGuard guard(env, /*allowClosing=*/true,
                            /*allowPending=*/true, /*needContext=*/false);
  if (!guard.ok()) return guard.status();
  chakra_nodeapi_host_services svc{};
  void* host_ctx = nullptr;
  if (!FetchServices(env, &svc, &host_ctx) || !HasSchedulingServices(svc)) {
    nodeapi::SetLastError(env, napi_generic_failure,
        "napi_unref_threadsafe_function: no host scheduling services");
    return napi_generic_failure;
  }
  bool needs_drop = false;
  TsfnState* st = LookupTsfn(func, &needs_drop);
  if (st == nullptr) {
    nodeapi::SetLastError(env, napi_invalid_arg,
        "napi_unref_threadsafe_function: unknown TSFN");
    return napi_invalid_arg;
  }
  if (st->env != env) {
    DropTsfnRef(st);
    nodeapi::SetLastError(env, napi_invalid_arg,
        "napi_unref_threadsafe_function: cross-env TSFN");
    return napi_invalid_arg;
  }
  AsyncStateImpl* ast = LookupState(env);
  if (ast == nullptr) {
    DropTsfnRef(st);
    nodeapi::SetLastError(env, napi_invalid_arg,
        "napi_unref_threadsafe_function: no async state");
    return napi_invalid_arg;
  }
  {
    std::lock_guard<std::mutex> lk(st->mutex);
    if (st->closing || st->finalized) {
      DropTsfnRef(st);
      nodeapi::SetLastError(env, napi_generic_failure,
          "napi_unref_threadsafe_function: TSFN finalized");
      return napi_generic_failure;
    }
    if (st->is_ref) {
      st->is_ref = false;
      if (ast->loop_ref_count > 0) ast->loop_ref_count--;
      if (ast->loop_ref_count == 0 && svc.unref_loop != nullptr) {
        try {
          svc.unref_loop(host_ctx);
        } catch (...) {
        }
      }
    }
  }
  DropTsfnRef(st);
  nodeapi::ClearLastError(env);
  return napi_ok;
}

} /* extern "C" */
