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

/* Per-env state (opaque to NodeApiEnv.h, stored as
 * NodeEnv::promise_state). Completes the forward-declared global
 * ::NodeApiPromiseState (defined at global scope, NOT inside this
 * anonymous namespace, so the extern "C" API signatures name the
 * canonical type). */
struct PromiseState {
  std::unordered_map<napi_deferred, DeferredRecord*> deferreds;
};

/* Provider-owned continuation queue per context. The trampoline's
 * callbackState points at one of these. Tasks are rooted (JsAddRef at
 * enqueue) until pumped (JsRelease after JsCallFunction). */
struct PromiseQueue {
  std::vector<JsValueRef> tasks;
  std::mutex mutex;
};

/* Global map: context VALUE -> queue. Consulted by VALUE before any
 * dereference (registry pattern). Entries are never erased (queue
 * objects are process-lifetime per context; only tasks are released).
 * The mutex is never held across user code (the trampoline releases
 * it before returning; the pump holds it only to pop one task). */
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
    nodeapi::SetLastError(env, napi_generic_failure, what);
    return napi_generic_failure;
  }
  JsValueRef undef = JS_INVALID_REFERENCE;
  JsErrorCode ec = JsGetUndefinedValue(&undef);
  if (ec != JsNoError) {
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
  if (ec != JsNoError) {
    return MapJs(env, ec, what);
  }
  nodeapi::ClearLastError(env);
  return napi_ok;
}

} /* namespace */

extern "C" {

NodeApiPromiseState* NodeApiPromise_CreateState(void) {
  return reinterpret_cast<NodeApiPromiseState*>(new PromiseState());
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
  /* Release every queued continuation task for this env's context.
   * The queue object itself stays (shared, process-lifetime); only
   * its tasks are dropped. This upholds R5 (no root survives runtime
   * disposal) even when the host did not pump to quiet. */
  nodeapi::NodeEnv* nenv = nullptr;
  if (!nodeapi::LookupLiveEnv(static_cast<void*>(env), &nenv)) return;
  JsContextRef ctx = nenv->context;
  if (ctx == JS_INVALID_REFERENCE) return;
  PromiseQueue* q = nullptr;
  {
    std::lock_guard<std::mutex> lk(g_queue_mutex);
    q = GetQueueLocked(ctx);
  }
  if (q == nullptr) return;
  std::vector<JsValueRef> leftover;
  {
    std::lock_guard<std::mutex> lk(q->mutex);
    leftover.swap(q->tasks);
  }
  for (JsValueRef t : leftover) {
    (void)JsRelease(t, nullptr);
  }
}

void NodeApiPromise_FreeState(NodeApiPromiseState* state) {
  if (state == nullptr) return;
  delete reinterpret_cast<PromiseState*>(state);
}

void NodeApiPromise_EnsureContinuationInstalled(napi_env env) {
  if (env == nullptr) return;
  nodeapi::NodeEnv* nenv = nullptr;
  if (!nodeapi::LookupLiveEnv(static_cast<void*>(env), &nenv)) return;
  JsContextRef ctx = nenv->context;
  if (ctx == JS_INVALID_REFERENCE) return;
  PromiseQueue* q = nullptr;
  bool need_install = false;
  {
    std::lock_guard<std::mutex> lk(g_queue_mutex);
    q = GetQueueLocked(ctx);
    if (q == nullptr) {
      try {
        q = new PromiseQueue();
      } catch (...) {
        return;
      }
      g_queues[ctx] = q;
      need_install = true;
    }
  }
  if (!need_install) return;
  /* Install with the env context current (host sets it before create;
   * the guard is not constructible here because the env is already
   * live and we are inside create's registry block -- the caller
   * guarantees the current-context discipline per plan SS3.2). */
  JsContextRef cur = JS_INVALID_REFERENCE;
  if (JsGetCurrentContext(&cur) != JsNoError) return;
  if (cur != ctx) {
    /* Current context is not the env's: install would target the
     * wrong script context. Skip (a later napi_create_promise with
     * the guard entered will still work when the host installs). */
    return;
  }
  (void)JsSetPromiseContinuationCallback(ProviderContinuation, q);
}

napi_status NodeApiPromise_Pump(napi_env env, size_t max_tasks,
                                size_t* out_pumped) {
  if (out_pumped != nullptr) *out_pumped = 0;
  if (env == nullptr) return napi_invalid_arg;
  nodeapi::EntryGuard guard(env, /*allowClosing=*/false,
                            /*allowPending=*/true, /*needContext=*/true);
  if (!guard.ok()) return guard.status();
  nodeapi::NodeEnv* nenv = nullptr;
  if (!nodeapi::LookupLiveEnv(static_cast<void*>(env), &nenv)) {
    return napi_invalid_arg;
  }
  JsContextRef ctx = nenv->context;
  PromiseQueue* q = nullptr;
  {
    std::lock_guard<std::mutex> lk(g_queue_mutex);
    q = GetQueueLocked(ctx);
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
  nodeapi::NodeEnv* nenv = nullptr;
  if (!nodeapi::LookupLiveEnv(static_cast<void*>(env), &nenv)) return 0;
  PromiseQueue* q = nullptr;
  {
    std::lock_guard<std::mutex> lk(g_queue_mutex);
    q = GetQueueLocked(nenv->context);
  }
  if (q == nullptr) return 0;
  std::lock_guard<std::mutex> lk(q->mutex);
  return q->tasks.size();
}

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
  DeferredRecord* rec = nullptr;
  try {
    rec = new DeferredRecord();
  } catch (...) {
    nodeapi::SetLastError(env, napi_generic_failure,
                          "napi_create_promise: record OOM");
    return napi_generic_failure;
  }
  if (JsAddRef(jr, nullptr) != JsNoError ||
      JsAddRef(jrej, nullptr) != JsNoError) {
    /* JsAddRef failure is unrooted: release whatever was taken and
     * fail. (In practice AddRef on fresh call-roots cannot fail
     * except OOM; the check keeps the partial state valid.) */
    (void)JsRelease(jr, nullptr);
    (void)JsRelease(jrej, nullptr);
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
  st->deferreds[handle] = rec;
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
