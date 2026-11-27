//-------------------------------------------------------------------------------------------------------
// Copyright (c) ChakraCore Project Contributors. All rights reserved.
// Licensed under the MIT license. See LICENSE.txt file in the project root for full license information.
//-------------------------------------------------------------------------------------------------------
//
// Node-API promises (N3.4). See NodeApiPromise.h for the contract,
// detection story, pump contract, and ownership record.
//
// Implementation notes (Node v22.22.3 doc/api/n-api.md +
// src/js_native_api_v8.cc via doc/node-api-status-mapping.md):
//   - napi_create_promise: CHECK_ENV + NAPI_PREAMBLE (No: gated).
//     Alloc failure => generic_failure. Out-params must be non-NULL
//     (invalid_arg). The promise value is rooted into the innermost
//     scope (R1); the deferred record roots the resolve/reject fns.
//   - napi_resolve_deferred / napi_reject_deferred: No (gated).
//     deferred==NULL => invalid_arg; resolution/rejection any type
//     (NULL napi_value => invalid_arg, provider validation-first).
//     First settle calls the corresponding fn with this=undefined and
//     releases the record's roots after; second+ settles are JS
//     no-ops returning napi_ok without calling (the record is
//     retained as a settled tombstone until env destroy). A throwing
//     settle (e.g. thenable assimilation) returns
//     napi_pending_exception with the exception left pending.
//   - napi_is_promise: Yes (executes with pending). Wrong type is NOT
//     an error: napi_ok + false (is_* never errors on type; status
//     mapping col "type-mismatch status": none). The provider maps
//     JsGetPromiseState's JsErrorInvalidArgument (non-promise) to
//     ok+false; genuine JsRT failures map via SS3.10.
//   - napi_run_script / napi_adjust_external_memory / versions are NOT
//     here (N3.5, NodeApiScript.cpp / NodeApiEnv.cpp).

#include "NodeApiPromise.h"

#include "NodeApiEnv.h"
#include "NodeApiHostApi.h"
#include "NodeApiScopes.h"
#include "ChakraCommon.h"
#include "ChakraCore.h"

#include <cstdint>
#include <deque>
#include <mutex>
#include <new>
#include <unordered_map>
#include <vector>

namespace {

/* One napi_deferred record. The record address IS the handle. */
struct DeferredRecord {
  JsValueRef resolve_fn = JS_INVALID_REFERENCE;
  JsValueRef reject_fn = JS_INVALID_REFERENCE;
  bool settled = false;
};

/* Provider-owned continuation queue per context (ref-counted across sibling
 * envs bound to the same JsContextRef when host_manages_promise_queue is
 * false).
 *
 * Ownership / invalidation record (doc/memory-management.md):
 *   - Domain: C++ heap (`new (std::nothrow) PromiseQueue`), stored by pointer
 *     in `g_queues[ctx]` and `PromiseState::queue`.
 *   - Owner: Shared via `PromiseQueue::ref_count` across live envs whose
 *     `host_manages_promise_queue == false`. The Chakra engine's
 *     `JsSetPromiseContinuationCallback` callbackState holds a borrowed pointer
 *     for `ctx`.
 *   - Roots: Enqueued JS task functions are rooted via `JsAddRef` in
 *     `ProviderContinuation` and released via `JsRelease` after invocation in
 *     `NodeApiPromise_Pump` or on final teardown in `NodeApiPromise_DestroyForEnv`.
 *   - Invalidation & reclamation order (step 6c, `NodeApiPromise_DestroyForEnv`):
 *     When the last env referencing `q` destroys (`--q->ref_count == 0`) with
 *     the env's `JsContextRef` entered, we unlink `g_queues[ctx]`, clear the
 *     engine slot via `JsSetPromiseContinuationCallback(nullptr, nullptr)` BEFORE
 *     freeing `q` (preventing any late/teardown wrap finalizer on `ctx` from
 *     invoking `ProviderContinuation(task, freed_q)`), release all remaining
 *     queued task roots, and then `delete q` (closing A8-3 cleanly). */
struct PromiseQueue {
  std::vector<JsValueRef> tasks;
  std::mutex mutex;
  size_t ref_count = 0;
};

struct PromiseState {
  std::unordered_map<napi_deferred, DeferredRecord*> deferreds;
  std::deque<napi_deferred> retired_deferreds;
  PromiseQueue* queue = nullptr;
  JsContextRef installed_context = JS_INVALID_REFERENCE;
};

/* Global map: context VALUE -> queue. The mutex is never held across user
 * code or JsSetPromiseContinuationCallback. */
std::mutex g_queue_mutex;
std::unordered_map<JsContextRef, PromiseQueue*> g_queues;

/* Provider trampoline: queues the task for the host pump. Must not
 * throw, must not call JS. Mirrors the test host's
 * promise_continuation (ch-nodeapi.cpp). */
void ProviderContinuation(JsValueRef task, void* state) {
  auto* q = static_cast<PromiseQueue*>(state);
  if (q == nullptr || task == JS_INVALID_REFERENCE) return;
  /* Root before caching (R1). If AddRef fails, drop (engine
   * guarantees the task stays alive for the enqueue window; a failed
   * root means we cannot keep it, so dropping degrades to the same
   * pending behaviour as no callback). */
  if (JsAddRef(task, nullptr) != JsNoError) return;
  std::lock_guard<std::mutex> lk(q->mutex);
  try {
    q->tasks.push_back(task);
  } catch (...) {
    /* OOM: release the just-taken root, drop the task. */
    (void)JsRelease(task, nullptr);
  }
}

PromiseQueue* GetQueueLocked(JsContextRef ctx) {
  auto it = g_queues.find(ctx);
  return it == g_queues.end() ? nullptr : it->second;
}

napi_status MapJs(napi_env env, JsErrorCode ec, const char* what) {
  if (ec == JsNoError) return napi_ok;
  if (ec == JsErrorInExceptionState || ec == JsErrorScriptException) {
    nodeapi::SetLastError(env, napi_pending_exception, what);
    return napi_pending_exception;
  }
  if (ec == JsErrorInvalidArgument || ec == JsErrorNullArgument) {
    nodeapi::SetLastError(env, napi_invalid_arg, what);
    return napi_invalid_arg;
  }
  nodeapi::SetLastError(env, napi_generic_failure, what);
  return napi_generic_failure;
}

/* Root into innermost scope (R1) and publish. On rooting failure the
 * caller's out-param is left untouched and generic_failure is returned. */
napi_status RootPublish(napi_env env, JsValueRef v, napi_value* result,
                        const char* what) {
  napi_value nv = reinterpret_cast<napi_value>(v);
  napi_status rs = NodeApiScopes_RootIntoInnermost(env, nv);
  if (rs != napi_ok) return rs;
  *result = nv;
  nodeapi::ClearLastError(env);
  return napi_ok;
}

/* Release a deferred record's roots and free the record. Must be
 * called with the env context entered (JsRelease requirement). */
void ReleaseRecord(DeferredRecord* rec) {
  if (rec == nullptr) return;
  if (rec->resolve_fn != JS_INVALID_REFERENCE) {
    (void)JsRelease(rec->resolve_fn, nullptr);
    rec->resolve_fn = JS_INVALID_REFERENCE;
  }
  if (rec->reject_fn != JS_INVALID_REFERENCE) {
    (void)JsRelease(rec->reject_fn, nullptr);
    rec->reject_fn = JS_INVALID_REFERENCE;
  }
  delete rec;
}

void RetireSettledDeferred(PromiseState* st, napi_deferred deferred,
                           DeferredRecord* rec) {
  while (st->retired_deferreds.size() >= nodeapi::kDeadEnvCap) {
    napi_deferred oldest = st->retired_deferreds.front();
    st->retired_deferreds.pop_front();
    auto oit = st->deferreds.find(oldest);
    if (oit != st->deferreds.end()) {
      delete oit->second;
      st->deferreds.erase(oit);
    }
  }
  try {
    st->retired_deferreds.push_back(deferred);
  } catch (...) {
    st->deferreds.erase(deferred);
    delete rec;
  }
}

/* Shared settle body. `use_reject` selects reject vs resolve. */
napi_status SettleCommon(napi_env env, napi_deferred deferred,
                         napi_value value, bool use_reject,
                         const char* what) {
  /* Validate deferred BEFORE touching the map (caller already
   * validated env + pending gate + non-NULLs). The map lookup is by
   * handle VALUE; a foreign/freed handle misses and fails cleanly. */
  NodeApiPromiseState* raw = NodeApiEnv_PromiseState(env);
  if (raw == nullptr) {
    nodeapi::SetLastError(env, napi_invalid_arg, what);
    return napi_invalid_arg;
  }
  auto* st = reinterpret_cast<PromiseState*>(raw);
  auto it = st->deferreds.find(deferred);
  if (it == st->deferreds.end() || it->second == nullptr) {
    nodeapi::SetLastError(env, napi_invalid_arg, what);
    return napi_invalid_arg;
  }
  DeferredRecord* rec = it->second;
  if (rec->settled) {
    /* Double-settle: JS-level no-op (Node behaviour). Return ok
     * without calling (roots already released). Success clears
     * last_error (AMB-STATUS-1). */
    nodeapi::ClearLastError(env);
    return napi_ok;
  }
  /* First settle: mark settled and load the callee BEFORE calling,
   * so reentrant settles from inside the callee's JS see settled. */
  rec->settled = true;
  JsValueRef callee = use_reject ? rec->reject_fn : rec->resolve_fn;
  if (callee == JS_INVALID_REFERENCE) {
    /* Should be unreachable (roots held until settle), but treat as
     * generic_failure rather than calling an invalid value. */
    RetireSettledDeferred(st, deferred, rec);
    nodeapi::SetLastError(env, napi_generic_failure, what);
    return napi_generic_failure;
  }
  JsValueRef undef = JS_INVALID_REFERENCE;
  JsErrorCode ec = JsGetUndefinedValue(&undef);
  if (ec != JsNoError) {
    /* Guaranteed release: the record's roots must drop on every
     * exit (R1 balanced accounting), including this failure path
     * before JsCallFunction. Mirrors the release after the call
     * below (and the pump's release on its undefined failure). */
    if (rec->resolve_fn != JS_INVALID_REFERENCE) {
      (void)JsRelease(rec->resolve_fn, nullptr);
      rec->resolve_fn = JS_INVALID_REFERENCE;
    }
    if (rec->reject_fn != JS_INVALID_REFERENCE) {
      (void)JsRelease(rec->reject_fn, nullptr);
      rec->reject_fn = JS_INVALID_REFERENCE;
    }
    RetireSettledDeferred(st, deferred, rec);
    return MapJs(env, ec, what);
  }
  JsValueRef args[2] = {undef, reinterpret_cast<JsValueRef>(value)};
  JsValueRef result = JS_INVALID_REFERENCE;
  ec = JsCallFunction(callee, args, 2, &result);
  /* Release BOTH roots after settling (task brief), even on throw.
   * The promise value keeps the promise alive; the fns are now dead. */
  if (rec->resolve_fn != JS_INVALID_REFERENCE) {
    (void)JsRelease(rec->resolve_fn, nullptr);
    rec->resolve_fn = JS_INVALID_REFERENCE;
  }
  if (rec->reject_fn != JS_INVALID_REFERENCE) {
    (void)JsRelease(rec->reject_fn, nullptr);
    rec->reject_fn = JS_INVALID_REFERENCE;
  }
  RetireSettledDeferred(st, deferred, rec);
  if (ec != JsNoError) {
    return MapJs(env, ec, what);
  }
  nodeapi::ClearLastError(env);
  return napi_ok;
}

} /* namespace */

extern "C" {

NodeApiPromiseState* NodeApiPromise_CreateState(void) {
  return reinterpret_cast<NodeApiPromiseState*>(new (std::nothrow) PromiseState());
}

void NodeApiPromise_DestroyForEnv(napi_env env) {
  if (env == nullptr) return;
  NodeApiPromiseState* raw = NodeApiEnv_PromiseState(env);
  if (raw == nullptr) return;
  auto* st = reinterpret_cast<PromiseState*>(raw);
  /* Release every remaining deferred's roots and free every record
   * (settled tombstones already released their roots; ReleaseRecord
   * is a no-op for those roots). Must run with the env context
   * entered (destroy's guard provides it). */
  for (auto& kv : st->deferreds) {
    ReleaseRecord(kv.second);
  }
  st->deferreds.clear();
  st->retired_deferreds.clear();

  PromiseQueue* q = st->queue;
  JsContextRef ctx = st->installed_context;
  st->queue = nullptr;
  st->installed_context = JS_INVALID_REFERENCE;
  if (q == nullptr || ctx == JS_INVALID_REFERENCE) return;

  bool last_owner = false;
  {
    std::lock_guard<std::mutex> lk(g_queue_mutex);
    if (q->ref_count > 0) q->ref_count--;
    if (q->ref_count == 0) {
      last_owner = true;
      auto it = g_queues.find(ctx);
      if (it != g_queues.end() && it->second == q) {
        g_queues.erase(it);
      }
    }
  }
  if (!last_owner) return;

  /* Invalidate the engine continuation callback BEFORE freeing `q` so no
   * subsequent promise settlement on `ctx` (e.g. teardown step 6d wraps or
   * caller work before JsDisposeRuntime) invokes ProviderContinuation with
   * a dangling `PromiseQueue*`. Only touch the context slot while `ctx` is
   * confirmed current. */
  JsContextRef cur = JS_INVALID_REFERENCE;
  if (JsGetCurrentContext(&cur) == JsNoError && cur == ctx) {
    (void)JsSetPromiseContinuationCallback(nullptr, nullptr);
  }
  std::vector<JsValueRef> leftover;
  {
    std::lock_guard<std::mutex> lk(q->mutex);
    leftover.swap(q->tasks);
  }
  for (JsValueRef t : leftover) {
    (void)JsRelease(t, nullptr);
  }
  delete q;
}

void NodeApiPromise_FreeState(NodeApiPromiseState* state) {
  if (state == nullptr) return;
  delete reinterpret_cast<PromiseState*>(state);
}

void NodeApiPromise_EnsureContinuationInstalled(napi_env env) {
  if (env == nullptr) return;
  nodeapi::NodeEnv* nenv = nullptr;
  if (!nodeapi::LookupLiveEnvForOwner(static_cast<void*>(env), &nenv)) return;
  NodeApiPromiseState* raw = NodeApiEnv_PromiseState(env);
  if (raw == nullptr) return;
  auto* st = reinterpret_cast<PromiseState*>(raw);
  if (nenv->services.host_manages_promise_queue) return;
  if (st->queue != nullptr) return;
  JsContextRef ctx = nenv->context;
  if (ctx == JS_INVALID_REFERENCE) return;

  PromiseQueue* q = nullptr;
  bool need_install = false;
  {
    std::lock_guard<std::mutex> lk(g_queue_mutex);
    q = GetQueueLocked(ctx);
    if (q != nullptr) {
      q->ref_count++;
      st->queue = q;
      st->installed_context = ctx;
      return;
    }
    q = new (std::nothrow) PromiseQueue();
    if (q == nullptr) return;
    q->ref_count = 1;
    try {
      g_queues.emplace(ctx, q);
    } catch (...) {
      delete q;
      return;
    }
    need_install = true;
  }
  (void)need_install;

  /* Install with the env context current (caller guarantees this per
   * plan SS3.2). If ctx is not current or JsSetPromiseContinuationCallback
   * fails, roll back the newly created queue so we never leave a dead
   * entry in g_queues. */
  JsContextRef cur = JS_INVALID_REFERENCE;
  if (JsGetCurrentContext(&cur) != JsNoError || cur != ctx ||
      JsSetPromiseContinuationCallback(ProviderContinuation, q) != JsNoError) {
    bool erase_and_delete = false;
    {
      std::lock_guard<std::mutex> lk(g_queue_mutex);
      if (q->ref_count > 0) q->ref_count--;
      if (q->ref_count == 0) {
        auto it = g_queues.find(ctx);
        if (it != g_queues.end() && it->second == q) {
          g_queues.erase(it);
        }
        erase_and_delete = true;
      }
    }
    if (erase_and_delete) delete q;
    return;
  }
  st->queue = q;
  st->installed_context = ctx;
}

napi_status NodeApiPromise_Pump(napi_env env, size_t max_tasks,
                                size_t* out_pumped) {
  if (out_pumped != nullptr) *out_pumped = 0;
  if (env == nullptr) return napi_invalid_arg;
  nodeapi::EntryGuard guard(env, /*allowClosing=*/false,
                            /*allowPending=*/true, /*needContext=*/true);
  if (!guard.ok()) return guard.status();
  nodeapi::NodeEnv* nenv = nullptr;
  if (!nodeapi::LookupLiveEnvForOwner(static_cast<void*>(env), &nenv)) {
    return napi_invalid_arg;
  }
  PromiseQueue* q = nullptr;
  if (NodeApiPromiseState* raw = NodeApiEnv_PromiseState(env)) {
    q = reinterpret_cast<PromiseState*>(raw)->queue;
  }
  if (q == nullptr) {
    /* Host owns the queue (or provider never installed): nothing to
     * pump here. Success (quiet) by definition. */
    nodeapi::ClearLastError(env);
    return napi_ok;
  }
  size_t pumped = 0;
  for (;;) {
    if (max_tasks != 0 && pumped >= max_tasks) {
      /* Bound hit before quiet. */
      if (out_pumped != nullptr) *out_pumped = pumped;
      nodeapi::SetLastError(env, napi_generic_failure,
                            "promise pump: bound exceeded");
      return napi_generic_failure;
    }
    JsValueRef task = JS_INVALID_REFERENCE;
    {
      std::lock_guard<std::mutex> lk(q->mutex);
      if (q->tasks.empty()) break;
      task = q->tasks.front();
      q->tasks.erase(q->tasks.begin());
    }
    JsValueRef undef = JS_INVALID_REFERENCE;
    if (JsGetUndefinedValue(&undef) != JsNoError) {
      (void)JsRelease(task, nullptr);
      if (out_pumped != nullptr) *out_pumped = pumped;
      nodeapi::SetLastError(env, napi_generic_failure,
                            "promise pump: undefined failed");
      return napi_generic_failure;
    }
    JsValueRef arr[1] = {undef};
    JsValueRef result = JS_INVALID_REFERENCE;
    JsErrorCode ec = JsCallFunction(task, arr, 1, &result);
    (void)JsRelease(task, nullptr);
    pumped++;
    if (ec != JsNoError) {
      if (out_pumped != nullptr) *out_pumped = pumped;
      return MapJs(env, ec, "promise pump: task threw");
    }
  }
  if (out_pumped != nullptr) *out_pumped = pumped;
  nodeapi::ClearLastError(env);
  return napi_ok;
}

size_t NodeApiPromise_DeferredCount(napi_env env) {
  if (env == nullptr) return 0;
  NodeApiPromiseState* raw = NodeApiEnv_PromiseState(env);
  if (raw == nullptr) return 0;
  return reinterpret_cast<PromiseState*>(raw)->deferreds.size();
}

size_t NodeApiPromise_QueueDepth(napi_env env) {
  if (env == nullptr) return 0;
  NodeApiPromiseState* raw = NodeApiEnv_PromiseState(env);
  if (raw == nullptr) return 0;
  PromiseQueue* q = reinterpret_cast<PromiseState*>(raw)->queue;
  if (q == nullptr) return 0;
  std::lock_guard<std::mutex> lk(q->mutex);
  return q->tasks.size();
}

#ifdef CHAKRA_NODEAPI_TEST_HOOKS
size_t NodeApiPromise_ActiveQueueCount(void) {
  std::lock_guard<std::mutex> lk(g_queue_mutex);
  return g_queues.size();
}
#endif

/* --- napi_* entries (C column per doc/node-api-status-mapping.md) --- */

napi_status NAPI_CDECL napi_create_promise(napi_env env,
                                            napi_deferred* deferred,
                                            napi_value* promise) {
  if (deferred == nullptr || promise == nullptr) {
    if (env != nullptr) {
      nodeapi::SetLastError(env, napi_invalid_arg,
                            "napi_create_promise: null out-param");
    }
    return napi_invalid_arg;
  }
  nodeapi::EntryGuard guard(env, /*allowClosing=*/false,
                            /*allowPending=*/false, /*needContext=*/true);
  if (!guard.ok()) return guard.status();
  NodeApiPromiseState* raw = NodeApiEnv_PromiseState(env);
  if (raw == nullptr) {
    nodeapi::SetLastError(env, napi_invalid_arg,
                          "napi_create_promise: no promise state");
    return napi_invalid_arg;
  }
  auto* st = reinterpret_cast<PromiseState*>(raw);
  JsValueRef jp = JS_INVALID_REFERENCE;
  JsValueRef jr = JS_INVALID_REFERENCE;
  JsValueRef jrej = JS_INVALID_REFERENCE;
  JsErrorCode ec = JsCreatePromise(&jp, &jr, &jrej);
  if (ec != JsNoError) return MapJs(env, ec, "JsCreatePromise");
  /* Root both fns into the record (R1). On failure, roll back. */
  auto* rec = new (std::nothrow) DeferredRecord();
  if (rec == nullptr) {
    nodeapi::SetLastError(env, napi_generic_failure,
                          "napi_create_promise: record OOM");
    return napi_generic_failure;
  }
  if (JsAddRef(jr, nullptr) != JsNoError) {
    delete rec;
    nodeapi::SetLastError(env, napi_generic_failure,
                          "napi_create_promise: root failed");
    return napi_generic_failure;
  }
  if (JsAddRef(jrej, nullptr) != JsNoError) {
    /* First root succeeded, second failed: release the first root cleanly. */
    (void)JsRelease(jr, nullptr);
    delete rec;
    nodeapi::SetLastError(env, napi_generic_failure,
                          "napi_create_promise: root failed");
    return napi_generic_failure;
  }
  rec->resolve_fn = jr;
  rec->reject_fn = jrej;
  rec->settled = false;
  napi_deferred handle = reinterpret_cast<napi_deferred>(rec);
  /* Insert before rooting the promise into the scope, so a scope-root
   * failure can remove the record cleanly (no leaked deferred). */
  try {
    st->deferreds.emplace(handle, rec);
  } catch (...) {
    ReleaseRecord(rec);
    nodeapi::SetLastError(env, napi_generic_failure,
                          "napi_create_promise: map OOM");
    return napi_generic_failure;
  }
  napi_value nv = reinterpret_cast<napi_value>(jp);
  napi_status rs = NodeApiScopes_RootIntoInnermost(env, nv);
  if (rs != napi_ok) {
    st->deferreds.erase(handle);
    ReleaseRecord(rec);
    return rs;
  }
  *deferred = handle;
  *promise = nv;
  nodeapi::ClearLastError(env);
  return napi_ok;
}

napi_status NAPI_CDECL napi_resolve_deferred(napi_env env,
                                              napi_deferred deferred,
                                              napi_value resolution) {
  if (deferred == nullptr || resolution == nullptr) {
    if (env != nullptr) {
      nodeapi::SetLastError(env, napi_invalid_arg,
                            "napi_resolve_deferred: null arg");
    }
    return napi_invalid_arg;
  }
  nodeapi::EntryGuard guard(env, /*allowClosing=*/false,
                            /*allowPending=*/false, /*needContext=*/true);
  if (!guard.ok()) return guard.status();
  return SettleCommon(env, deferred, resolution, /*use_reject=*/false,
                      "napi_resolve_deferred");
}

napi_status NAPI_CDECL napi_reject_deferred(napi_env env,
                                             napi_deferred deferred,
                                             napi_value rejection) {
  if (deferred == nullptr || rejection == nullptr) {
    if (env != nullptr) {
      nodeapi::SetLastError(env, napi_invalid_arg,
                            "napi_reject_deferred: null arg");
    }
    return napi_invalid_arg;
  }
  nodeapi::EntryGuard guard(env, /*allowClosing=*/false,
                            /*allowPending=*/false, /*needContext=*/true);
  if (!guard.ok()) return guard.status();
  return SettleCommon(env, deferred, rejection, /*use_reject=*/true,
                      "napi_reject_deferred");
}

napi_status NAPI_CDECL napi_is_promise(napi_env env, napi_value value,
                                        bool* is_promise) {
  if (env == nullptr || value == nullptr || is_promise == nullptr) {
    if (env != nullptr) {
      nodeapi::SetLastError(env, napi_invalid_arg, "napi_is_promise: null arg");
    }
    return napi_invalid_arg;
  }
  nodeapi::EntryGuard guard(env, /*allowClosing=*/false,
                            /*allowPending=*/true, /*needContext=*/true);
  if (!guard.ok()) return guard.status();
  JsPromiseState ps = JsPromiseStatePending;
  JsErrorCode ec =
      JsGetPromiseState(reinterpret_cast<JsValueRef>(value), &ps);
  if (ec == JsNoError) {
    *is_promise = true;
    nodeapi::ClearLastError(env);
    return napi_ok;
  }
  if (ec == JsErrorInvalidArgument || ec == JsErrorNullArgument) {
    /* Non-promise: ok+false (is_* never errors on type). */
    *is_promise = false;
    nodeapi::ClearLastError(env);
    return napi_ok;
  }
  return MapJs(env, ec, "napi_is_promise: JsGetPromiseState");
}

} /* extern "C" */
