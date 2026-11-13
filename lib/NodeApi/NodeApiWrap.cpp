//-------------------------------------------------------------------------------------------------------
// Copyright (c) ChakraCore Project Contributors. All rights reserved.
// Licensed under the MIT license. See LICENSE.txt file in the project root for full license information.
//-------------------------------------------------------------------------------------------------------
//
// N3.2/N3.3 implementation: externals + finalizer queue + wrap/tags/
// add-finalizer. See NodeApiWrap.h for the design summary, the
// before-collect reliability evidence, and the pending/closing
// discipline. This file is the ownership record (plan §3.7 + semantics
// §4; memory-management.md §3, §5).
//
// Ownership record (memory-management.md §3):
//
//   Resource / creation call / source location:
//     1) WrapRecord (one per napi_create_external / napi_wrap /
//        napi_add_finalizer / tag-only napi_type_tag_object).
//        lib/NodeApi/NodeApiWrap.cpp CreateRecord (C++ new, nothrow).
//     2) WrapState -- per-env finalizer queue (std::mutex +
//        std::deque), side-table map (object pointer -> record) and
//        external sets. Lazy-created by HandleState on first wrap call
//        for the env, erased by nodeapi_wrap_teardown.
//     3) Weak handle: single JsWeakRef + R6 JsAddRef on the handle
//        (JsCreateWeakReference + JsAddRef), held while the record is
//        live. Fallback when JsCreateWeakReference reports
//        JsNoWeakRefRequired: a single JsAddRef on the object itself
//        (strong fallback), held while live.
//     4) Scope root: every returned napi_value (create_external) is
//        rooted into the innermost N1.2 scope via
//        NodeApiScopes_RootIntoInnermost (R-SC-1). Owned by the scope
//        stack, released by scope close / destroy step 6b, never by
//        this TU. Wrap/add_finalizer return no JsValueRef (the
//        optional napi_ref out-param, when provided, is a strong ref
//        from the N1.3 refs lane, owned by that lane until the addon
//        deletes it).
//   Domain and allocator instance (including backing allocator):
//     (1) and (2) live in provider native memory (C++ new/delete).
//     Never Recycler memory (plan §3.3) and never freed across the
//     C++/CRT boundary. (3) lives in the Recycler heap, released only
//     via JsRelease (never freed directly). Destroy order (plan §3.6):
//     step 2 drain and step 6 backstop run before refs (6a) and
//     scopes (6b) teardown end, both before runtime disposal (R5).
//   Physical allocation base, extent, alignment:
//     One heap allocation per WrapRecord / WrapState; deleted via the
//     matching native delete. Js handles are opaque pointers.
//   Logical length / borrowed views:
//     Provider state is authoritative; JsAddRef return counts are
//     never used as logic (plan §3.4 R2). The user_data pointer is
//     user-owned: the provider never allocates/frees it, only stores
//     it and passes it to napi_finalize. Type tags are 16 bytes
//     copied BY VALUE into the record at tag time (stack temporaries
//     are never retained; cf. vendored TypeTaggedInstance).
//   Owner; transfer/publication point; remaining aliases:
//     The provider (this TU) owns all four resources. The WrapRecord
//     is published to the GC at trampoline registration (external
//     object data for externals; before-collect callback state for
//     wraps/tags/add-finalizers) and to the addon via the napi_value /
//     napi_ref out-params. Ownership leaves the env only via drain,
//     remove_wrap, or the destroy backstop. No cross-env aliasing:
//     every record belongs to exactly one WrapState.
//   Borrow invalidation: GC / detach / growth / mutation / reset /
//     close / disposal:
//     (1)/(2) are invalidated by drain (pop), remove_wrap (suppress),
//     or destroy teardown (drain + backstop + state erase). (3) is
//     invalidated by drain/remove/teardown (JsRelease). The JS object
//     itself is invalidated by GC only while weakly held; strong
//     references (scope roots, napi_ref result refs) keep it alive.
//     Runtime disposal invalidates everything; all roots must be
//     released before JsDisposeRuntime (R5), which destroy guarantees
//     (scopes/refs teardown after this TU's steps).
//   Potential user-code or reentrancy points:
//     GC trampolines (JsFinalizeCallback /
//     JsObjectBeforeCollectCallback) run during GC on the collection
//     thread with IsInObjectBeforeCollectCallback set. They touch ONLY
//     the record's atomic exactly-once state and the env's native
//     queue (mutex + deque). No JS, no user finalizer, no side-table
//     mutation (D4). The deque push_back may allocate provider-heap
//     memory (the admitted exception to "no allocation on GC": the
//     plan's mutex-protected MPSC queue is a C++ container); failure
//     there is caught, logged, and leaves the record leaked without
//     crashing GC (OOM-only path). Drain runs napi_finalize on the
//     env thread with a live env; the queue lock is never held across
//     it (R-ENV-12), so a finalizer may reenter (call APIs, force GC,
//     chain further finalizers).
//   Thread/context allowed for use, root changes and destruction:
//     GC trampolines may run on an arbitrary GC thread with no current
//     context and touch only native queue state (R3: no JsAddRef /
//     JsRelease / napi_* there -- the only JsRef access in a
//     trampoline is none; weak-handle roots are released on drain,
//     on the env thread). All other entries (lookups, drain, teardown)
//     run on the env thread with the env context current (EntryGuard
//     / ContextGuard). Rental threading forbids concurrent runtime use
//     (mem §2), so teardown concurrent with another thread inside GC
//     is host misuse. Strong-fallback records and external records
//     still live at destroy backstop keep their allocation alive for
//     a possible late GC trampoline (self-delete, see destructor
//     note in code); no other late trampoline can fire (wraps clear
//     their before-collect registration before being freed).
//   Release on success, partial failure, exception, OOM, cancel and
//     shutdown:
//     Success: each record has exactly one release point (drain pop,
//     remove suppress, or teardown backstop; R-WRAP-6). Partial
//     failure (JsRT error / OOM mid-create) rolls back to the pre-call
//     state: before-collect registrations are cleared, container
//     entries erased, handle roots released, records freed (or, for
//     the external-created-but-weak-failed OOM path, detached to a
//     self-deleting leak so a future GC finalize cannot dereference
//     freed memory; it never runs the user finalizer). Foreign errors
//     map per plan §3.10. Shutdown: nodeapi_wrap_teardown runs the
//     step-6 backstop for every remaining live record exactly once
//     before NodeEnv delete (step 7); already-enqueued records were
//     drained at step 2 and skipped by the backstop.
//   Memory accounting, capacity/retention bound and exhaustion:
//     Live records = side-table + external counts (exposed as
//     chakra_nodeapi_test_wrap_live_count). No task-imposed cap; each
//     record is O(1) small native memory. The queue is bounded by the
//     number of collected records awaiting drain. Probe P7-11 asserts
//     live count returns to baseline after 10k create/collect/drain
//     cycles (no retention beyond drain).
//   Tests / commands / measured results / remaining uncertainty:
//     test/nodeapi/probes/wrap_probe.cpp (run_wrap_probe.sh), probes
//     P7-1/P7-2/P7-3/P7-6/P7-7/P7-8/P7-10/P7-11 (+ P7-4/P7-5/P7-9
//     reentrancy/chaining/add-finalizer arms inside P7-2-style
//     helpers). Remaining uncertainty: full EntryGuard drain for
//     non-wrap TUs (N1.5's shared guard) is wired only for this TU's
//     entries plus destroy steps 2/6 and the test hook; other lanes'
//     entries drain only when they happen to call a wrap API (see
//     NodeApiWrap.h). The unified R-REF-14 root counter
//     (NodeApiScopes.cpp) does not yet include wrap roots; the probe
//     asserts this TU's live count instead. Single data slot per
//     object: a second data finalizer (wrap after wrap, add_finalizer
//     after wrap, double add_finalizer) is napi_invalid_arg
//     (provider choice; plan §3.7 record has one finalize slot).

#include "NodeApiWrap.h"

#include "NodeApiEnv.h"
#include "NodeApiScopes.h"
#include "NodeApiRefs.h" /* napi_create_reference for wrap result refs */
#include "ChakraCommon.h" /* JsValueRef, JsRef, JsFinalizeCallback, GC */
#include "ChakraCore.h"   /* JsWeakRef, JsCreateWeakReference */

#include <atomic>
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <deque>
#include <mutex>
#include <new>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace {

/* ------------------------------------------------------------------ */
/* Record + state layouts (TU-private; NodeEnv layout untouched).       */
/* ------------------------------------------------------------------ */

constexpr uint64_t kRecordMagic = 0x4E4150495762B35Aull; /* "NAPIWz.." */

/* Exactly-once state (R-WRAP-6). Only the GC trampoline (LIVE->QUEUED)
 * and env-thread code (LIVE->SUPPRESSED at remove, LIVE->DONE at
 * backstop) race on this word. Load/compare-exchange decisions here are
 * the entire GC/env-thread synchronization for records; the queue lock
 * orders enqueue vs drain. */
enum RecordState : int {
  kLive = 0,      /* owned by env containers; GC may claim it */
  kQueued = 1,   /* GC claimed it and enqueued (or is enqueueing) */
  kSuppressed = 2, /* remove_wrap won the race; finalizer suppressed */
  kDone = 3,      /* backstop claimed it; finalizer ran (or was leaked) */
};

struct WrapState;

struct WrapRecord {
  std::atomic<int> state{kLive};
  uint64_t magic = kRecordMagic;
  nodeapi::NodeEnv* env = nullptr;
  WrapState* st = nullptr; /* owning state (queue lives here) */
  void* user_data = nullptr; /* user-owned, never freed by provider */
  node_api_basic_finalize finalize = nullptr;
  void* hint = nullptr;
  bool has_data_wrap = false; /* data+finalize slot occupied (wrap or
                                 * add_finalizer; externals too)? */
  bool has_wrap = false;      /* napi_wrap present (vs add_finalizer
                                 * only, which occupies the data slot but
                                 * is NOT unwrappable). */
  bool has_tag = false;       /* tag slot occupied? (side-table only) */
  napi_type_tag tag{};        /* by value */
  JsRef weak_handle = JS_INVALID_REFERENCE; /* R6-rooted while live */
  JsValueRef strong_fallback = JS_INVALID_REFERENCE; /* strong pin if
                                                       * weak creation was
                                                       * NoWeakRefRequired */
  bool before_collect = false; /* before-collect registered? */
  bool is_external = false;    /* external object record? */
  JsValueRef external_obj = JS_INVALID_REFERENCE; /* JS value if external */
  bool leak_to_gc = false;   /* destroyed with env; self-delete on late
                              * GC trampoline (externals + clear-failed
                              * wraps). Trampolines check state first and
                              * never touch st once DONE/SUPPRESSED+leak. */
  // N5.1 multi-finalizer chain (Node allows multiple add_finalizer /
  // wrap+add on one object; JsRT allows ONE before-collect callback per
  // object via JsSetObjectBeforeCollectCallback, so the head record owns
  // the single callback and chained records ride along on the head's
  // GC delivery). Chained records share the head's object lifetime:
  // they have no weak_handle/before_collect of their own and are run by
  // DrainState/teardown together with the head (env thread only; the
  // chained vector is only mutated on the env thread at add time and
  // read on the env thread at drain time -- the GC trampoline only ever
  // enqueues the head).
  std::vector<WrapRecord*> chained;
};

struct WrapState {
  nodeapi::NodeEnv* env = nullptr;
  /* MPSC queue: producers are GC trampolines (any GC thread),
   * consumer is the env thread drain. std::mutex + std::deque: the
   * brief's C++ choice (std::mutex; Zig 0.17 has no std.Thread.Mutex).
   * The lock is held only to push/pop, never across user finalizers. */
  std::mutex queue_mutex;
  std::deque<WrapRecord*> queue;
  std::atomic<size_t> queued_count{0}; /* cheap-empty check (R-WRAP-8) */
  /* Side table: object pointer bits -> record. Keyed by object, ABA-
   * guarded by weak deref validation (R-WRAP-11), mutated only on the
   * env thread (trampolines never touch it). */
  std::unordered_map<JsValueRef, WrapRecord*> wraps;
  /* External records (by JS object) + ownership set for membership
   * checks in get_value_external. */
  std::unordered_map<JsValueRef, WrapRecord*> external_by_obj;
  std::unordered_set<WrapRecord*> externals;
};

/* Provider-global state map: env pointer value -> state. Keyed by
 * value (never dereferencing dead values to create/lookup); erased at
 * teardown before NodeEnv delete so address reuse cannot alias a stale
 * state. Mirrors the AsyncPumpConfig map in NodeApiLifetime.cpp. */
std::mutex g_wrap_mutex;
std::unordered_map<nodeapi::NodeEnv*, WrapState*> g_wrap_states;

nodeapi::NodeEnv* ToNodeEnv(napi_env env_value) {
  if (!env_value) return nullptr;
  nodeapi::NodeEnv* env = nullptr;
  if (!nodeapi::LookupLiveEnv(static_cast<void*>(env_value), &env)) {
    return nullptr;
  }
  return env;
}

WrapState* LookupState(nodeapi::NodeEnv* env, bool create) {
  if (!env) return nullptr;
  std::lock_guard<std::mutex> lk(g_wrap_mutex);
  auto it = g_wrap_states.find(env);
  if (it != g_wrap_states.end()) return it->second;
  if (!create) return nullptr;
  WrapState* st = new (std::nothrow) WrapState();
  if (!st) return nullptr;
  st->env = env;
  g_wrap_states[env] = st;
  return st;
}

void ForgetState(nodeapi::NodeEnv* env) {
  std::lock_guard<std::mutex> lk(g_wrap_mutex);
  g_wrap_states.erase(env);
}

/* ------------------------------------------------------------------ */
/* Trampolines (GC thread). ENQUEUE ONLY.                               */
/* ------------------------------------------------------------------ */

/* GC-safe enqueue: CAS claim then lock + push + counted increment.
 * No JS, no user code, no container mutation beyond the queue. Must
 * not throw and must not touch st once the record is not LIVE (late
 * trampoline for a destroyed record self-deletes without dereferencing
 * the freed state). */
void GcEnqueue(WrapRecord* rec) {
  if (!rec) return;
  int expected = kLive;
  if (!rec->state.compare_exchange_strong(expected, kQueued,
                                          std::memory_order_acq_rel,
                                          std::memory_order_relaxed)) {
    /* Already claimed / suppressed / done: late or duplicate delivery.
     * For destroyed (leak-to-GC) records, self-delete here so the leak
     * is bounded by the JS object's lifetime. The record memory is
     * retained until this point precisely for this check. */
    if (rec->leak_to_gc) delete rec;
    return;
  }
  WrapState* st = rec->st;
  if (!st) return; /* should not happen; record stays queued-without-home */
  try {
    std::lock_guard<std::mutex> lk(st->queue_mutex);
    st->queue.push_back(rec);
    st->queued_count.fetch_add(1, std::memory_order_relaxed);
  } catch (...) {
    /* Provider-heap OOM on GC: log and leak (never crash GC). The
     * record stays kQueued but is never drained; live-count will show
     * the leak, which is the correct OOM signal. */
    std::fprintf(stderr, "nodeapi: finalizer queue OOM; record leaked\n");
  }
}

/* External finalizer trampoline: data is the WrapRecord*. Mirrors
 * JsrtExternalObject::Finalize (which fires with the slot data and no
 * current context). */
void ExternalTrampoline(void* data) noexcept {
  GcEnqueue(static_cast<WrapRecord*>(data));
}

/* Before-collect trampoline for wrapped arbitrary objects: callback
 * state is the WrapRecord*. The JsRef (object) argument is unused here
 * -- the record already knows its containers; the object pointer is
 * only needed at unregister time (env thread). */
void ObjectBeforeCollectTrampoline(JsRef /*ref*/, void* state) noexcept {
  GcEnqueue(static_cast<WrapRecord*>(state));
}

/* ------------------------------------------------------------------ */
/* Small env-thread helpers.                                            */
/* ------------------------------------------------------------------ */

napi_status map_js(napi_env env, JsErrorCode err, const char* what) {
  if (err == JsNoError) return napi_ok;
  if (err == JsErrorInExceptionState || err == JsErrorScriptException) {
    nodeapi::SetLastError(env, napi_pending_exception, what);
    return napi_pending_exception;
  }
  if (err == JsErrorInvalidArgument || err == JsErrorNullArgument) {
    nodeapi::SetLastError(env, napi_invalid_arg, what);
    return napi_invalid_arg;
  }
  nodeapi::SetLastError(env, napi_generic_failure, what);
  return napi_generic_failure;
}

/* Object-like definition for wrap/add/type-tag (strict IsObject side,
 * per status-mapping.md: wrap uses IsObject, NOT ToObject coercion).
 * Functions/symbols are NOT wrapped-objects here (Symbol is excluded
 * even though refs allow it: a symbol cannot carry a side-table entry
 * with a before-collect callback). Native externals report JsObject. */
bool is_wrapable_type(JsValueType t) {
  switch (t) {
    case JsObject:
    case JsFunction:
    case JsArray:
    case JsError:
    case JsArrayBuffer:
    case JsTypedArray:
    case JsDataView:
      return true;
    default:
      return false;
  }
}

/* JsGetValueType precheck helper: object-like value type or null. */
bool get_type_ok(napi_env env, napi_value value, JsValueType* out_type) {
  JsValueType t = JsUndefined;
  JsErrorCode ec = JsGetValueType(reinterpret_cast<JsValueRef>(value), &t);
  if (ec != JsNoError) return false;
  if (out_type) *out_type = t;
  (void)env;
  return true;
}

/* ABA-validated side-table lookup (R-WRAP-11). Returns the live record
 * for `obj`, or nullptr. Stale entries (weak deref != obj, i.e. the
 * recorded object was collected and its address may have been reused)
 * are erased and treated as a miss. Env thread only. */
WrapRecord* FindWrap(WrapState* st, JsValueRef obj) {
  if (!st || obj == JS_INVALID_REFERENCE) return nullptr;
  auto it = st->wraps.find(obj);
  if (it == st->wraps.end()) return nullptr;
  WrapRecord* rec = it->second;
  if (rec->strong_fallback != JS_INVALID_REFERENCE) {
    /* Strong fallback: the object is pinned, address reuse cannot
     * have happened, so the entry is trustworthy. */
    return rec;
  }
  if (rec->weak_handle == JS_INVALID_REFERENCE) return rec; /* no handle
                                                              * (should not
                                                              * happen) */
  JsValueRef cur = JS_INVALID_REFERENCE;
  JsErrorCode ec = JsGetWeakReferenceValue(rec->weak_handle, &cur);
  if (ec != JsNoError || cur != obj) {
    /* Stale (collected). Erase so address reuse by a new object does
     * not alias this record; the collected record itself is either
     * already queued (GC fired) or (env-thread creation not yet
     * finished). Erasing here is env-thread map mutation, safe. */
    WrapRecord* stale = it->second;
    st->wraps.erase(it);
    /* If the stale record is still LIVE (GC has not yet claimed it,
     * e.g. object collected but before-collect delivery pending), keep
     * its memory owned by the queue path: it is only reachable via the
     * trampoline's record pointer, not via the map, and will be freed
     * at drain. Do not delete here. */
    (void)stale;
    return nullptr;
  }
  return rec;
}

/* Release the record's weak handle / strong root (env thread). */
void ReleaseHandle(WrapRecord* rec) {
  if (!rec) return;
  if (rec->strong_fallback != JS_INVALID_REFERENCE) {
    (void)JsRelease(rec->strong_fallback, nullptr);
    rec->strong_fallback = JS_INVALID_REFERENCE;
  } else if (rec->weak_handle != JS_INVALID_REFERENCE) {
    (void)JsRelease(rec->weak_handle, nullptr);
    rec->weak_handle = JS_INVALID_REFERENCE;
  }
}

/* Create the weak handle (or strong fallback) for a wrap record.
 * Returns napi_ok on success. On generic_failure the record has no
 * handle to release. */
napi_status CreateHandle(napi_env env, JsValueRef obj, WrapRecord* rec) {
  JsWeakRef w = JS_INVALID_REFERENCE;
  JsErrorCode ec = JsCreateWeakReference(obj, &w);
  if (ec == JsNoWeakRefRequired) {
    /* Immortal / non-collectable object: pin strongly. The finalizer
     * then only fires via the destroy backstop (record never enqueued
     * by GC). Correct and bounded (the pin is released at drain/
     * remove/teardown). */
    ec = JsAddRef(obj, nullptr);
    if (ec != JsNoError) return map_js(env, ec, "JsAddRef (wrap pin)");
    rec->strong_fallback = obj;
    rec->weak_handle = JS_INVALID_REFERENCE;
    return napi_ok;
  }
  if (ec != JsNoError) return map_js(env, ec, "JsCreateWeakReference");
  ec = JsAddRef(w, nullptr); /* R6 */
  if (ec != JsNoError) {
    (void)JsRelease(w, nullptr);
    return map_js(env, ec, "JsAddRef weak handle");
  }
  rec->weak_handle = w;
  return napi_ok;
}

/* Register the before-collect trampoline (env thread, context current). */
napi_status RegisterBeforeCollect(napi_env env, JsValueRef obj,
                                  WrapRecord* rec) {
  if (rec->strong_fallback != JS_INVALID_REFERENCE) {
    rec->before_collect = false;
    return napi_ok; /* pinned: nothing to register */
  }
  JsErrorCode ec = JsSetObjectBeforeCollectCallback(
      obj, static_cast<void*>(rec),
      reinterpret_cast<JsObjectBeforeCollectCallback>(
          ObjectBeforeCollectTrampoline));
  if (ec != JsNoError) {
    return map_js(env, ec, "JsSetObjectBeforeCollectCallback");
  }
  rec->before_collect = true;
  return napi_ok;
}

/* Clear the before-collect registration. Best-effort at teardown. */
JsErrorCode ClearBeforeCollect(JsRef obj) {
  return JsSetObjectBeforeCollectCallback(obj, nullptr, nullptr);
}

/* Remove a record's container entries (env thread). Leaves the record
 * itself for the caller to finalize/delete. */
void RemoveContainers(WrapState* st, WrapRecord* rec) {
  if (!st || !rec) return;
  if (rec->is_external) {
    st->external_by_obj.erase(rec->external_obj);
    st->externals.erase(rec);
  } else {
    /* Erase by record identity (not by key): a stale-address lookup
     * may have already erased this key for a NEW record; only erase
     * when the entry still points at this record. */
    for (auto it = st->wraps.begin(); it != st->wraps.end(); ++it) {
      if (it->second == rec) {
        st->wraps.erase(it);
        break;
      }
    }
  }
}

/* Run one drained record's user finalizer (env thread, no lock held).
 * Captures fields first so a reentrant finalizer cannot mutate the
 * record out from under the call. */
void RunFinalizer(napi_env env, WrapRecord* rec) {
  if (!rec) return;
  node_api_basic_finalize fin = rec->finalize;
  void* data = rec->user_data;
  void* hint = rec->hint;
  if (fin != nullptr) {
    fin(env, data, hint);
  }
}

/* Pop one record from the queue (env thread). nullptr when empty. */
WrapRecord* PopQueue(WrapState* st) {
  std::lock_guard<std::mutex> lk(st->queue_mutex);
  if (st->queue.empty()) return nullptr;
  WrapRecord* rec = st->queue.front();
  st->queue.pop_front();
  st->queued_count.fetch_sub(1, std::memory_order_relaxed);
  return rec;
}

/* Drain until empty (env thread). Each iteration's lock is released
 * before the user finalizer runs (R-ENV-12); finalizers enqueued by
 * reentrant GC inside a finalizer are picked up by later iterations. */
void DrainState(napi_env env, WrapState* st) {
  if (!st) return;
  for (;;) {
    WrapRecord* rec = PopQueue(st);
    if (!rec) break;
    /* Only QUEUED records are drained with a finalizer. A record can
     * only be queued after CAS LIVE->QUEUED, and remove_wrap can only
     * suppress from LIVE, so a queued record here is exactly-once
     * owned by this drain. Defensive: skip non-queued without calling
     * the finalizer (and free it) rather than double-firing. */
    int expected = kQueued;
    bool own = rec->state.compare_exchange_strong(
        expected, kDone, std::memory_order_acq_rel,
        std::memory_order_relaxed);
    RemoveContainers(st, rec);
    ReleaseHandle(rec);
    // N5.1: run chained rider finalizers with the head (same
    // collection event). Snapshot first (finalizers may enqueue).
    std::vector<WrapRecord*> riders;
    if (!rec->chained.empty()) riders = rec->chained;
    if (own) RunFinalizer(env, rec);
    delete rec;
    for (WrapRecord* rider : riders) {
      int wexp = kQueued;
      // Riders were never queued (no callback of their own); they
      // were LIVE and are claimed here to DONE (exactly-once with
      // the head's collection). A rider already claimed (e.g. by a
      // teardown race) is skipped.
      bool rownd = rider->state.compare_exchange_strong(
          wexp, kDone, std::memory_order_acq_rel,
          std::memory_order_relaxed);
      if (!rownd) {
        int lexp = kLive;
        rownd = rider->state.compare_exchange_strong(
            lexp, kDone, std::memory_order_acq_rel,
            std::memory_order_relaxed);
      }
      if (rownd) RunFinalizer(env, rider);
      delete rider;
    }
  }
}

/* Cheap-empty checked drain (R-WRAP-8 entry point). */
void MaybeDrain(napi_env env, WrapState* st) {
  if (!st) return;
  if (st->queued_count.load(std::memory_order_relaxed) == 0) return;
  DrainState(env, st);
}

/* Create the optional napi_ref result for wrap/add_finalizer. On
 * failure the caller rolls the wrap back. */
napi_status CreateOptionalRef(napi_env env, napi_value value,
                              napi_ref* result) {
  if (result == nullptr) return napi_ok;
  /* Pre-validated object: create_reference will succeed barring OOM.
   * It re-validates (Alive + owner thread) and roots the object; the
   * ref keeps the object alive until the addon deletes it, delaying
   * any GC finalizer -- exactly the documented result semantics.
   * Note (status-mapping wrap row): examples pass both finalize_cb
   * and result non-NULL, so both are accepted here despite the
   * mapping's tentative "finalize_cb==NULL required when result!=NULL"
   * note, which the vendored addons contradict. */
  return napi_create_reference(env, value, 1, result);
}

/* Roll back a just-created wrap record (env thread, before publication
 * of the out-params). Clears registration, containers and handle,
 * then deletes the record. Safe only before any queued delivery for
 * this record can be pending (true on the env thread creation path). */
void RollbackWrap(WrapState* st, JsValueRef obj, WrapRecord* rec) {
  if (rec->before_collect) (void)ClearBeforeCollect(obj);
  RemoveContainers(st, rec);
  ReleaseHandle(rec);
  delete rec;
}

} /* namespace */

extern "C" {

/* ------------------------------------------------------------------ */
/* napi_create_external / napi_get_value_external (N3.2).               */
/* ------------------------------------------------------------------ */

napi_status NAPI_CDECL napi_create_external(napi_env env, void* data,
                                            node_api_basic_finalize finalize_cb,
                                            void* finalize_hint,
                                            napi_value* result) {
  if (!result) {
    if (env) nodeapi::SetLastError(env, napi_invalid_arg, "create_external: result is NULL");
    return napi_invalid_arg;
  }
  nodeapi::EntryGuard guard(env, /*allowClosing=*/false,
                            /*allowPending=*/false, /*needContext=*/true);
  if (!guard.ok()) return guard.status();
  MaybeDrain(env, LookupState(ToNodeEnv(env), false));

  nodeapi::NodeEnv* nenv = ToNodeEnv(env);
  WrapState* st = LookupState(nenv, true);
  if (!st) {
    nodeapi::SetLastError(env, napi_generic_failure, "create_external: OOM (state)");
    return napi_generic_failure;
  }

  WrapRecord* rec = new (std::nothrow) WrapRecord();
  if (!rec) {
    nodeapi::SetLastError(env, napi_generic_failure, "create_external: OOM (record)");
    return napi_generic_failure;
  }
  rec->env = nenv;
  rec->st = st;
  rec->user_data = data;
  rec->finalize = finalize_cb;
  rec->hint = finalize_hint;
  rec->has_data_wrap = true;
  rec->is_external = true;

  /* The JS object is a finalized Recycler object carrying the record as
   * its data. The trampoline function address is shared by ALL provider
   * externals so their JsrtExternalType cache key is identical (one type
   * per prototype); records are distinguished by the data pointer. The
   * user finalize_cb is NOT passed to JsRT directly. */
  JsValueRef jsv = JS_INVALID_REFERENCE;
  JsErrorCode ec = JsCreateExternalObject(
      static_cast<void*>(rec),
      reinterpret_cast<JsFinalizeCallback>(ExternalTrampoline), &jsv);
  if (ec != JsNoError) {
    delete rec;
    return map_js(env, ec, "JsCreateExternalObject");
  }
  rec->external_obj = jsv;

  /* Weak handle for the external guard (ABA + membership validation in
   * get_value_external / tag lookups). A NoWeakRefRequired external is
   * treated as an error: we cannot guarantee exactly-once delivery for
   * an immortal external without pinning it (pinning would defeat the
   * finalizer). */
  napi_status hs = CreateHandle(env, jsv, rec);
  if (hs == napi_ok &&
      rec->strong_fallback != JS_INVALID_REFERENCE) {
    /* Externals must stay weakly held (a strong pin would defeat the
     * finalizer). Convert the pin into an error and release it. */
    nodeapi::SetLastError(env, napi_generic_failure,
                         "create_external: external is not weakly referenceable");
    hs = napi_generic_failure;
  }
  if (hs != napi_ok) {
    /* JS object already carries the record pointer. Do NOT free the
     * record: a future GC finalize would dereference it. Detach to a
     * self-deleting leak (suppressed future delivery). */
    ReleaseHandle(rec);
    rec->state.store(kSuppressed, std::memory_order_relaxed);
    rec->leak_to_gc = true;
    return hs;
  }
  /* External records need no before-collect: the finalize callback is
   * the collection signal. */
  rec->before_collect = false;

  try {
    st->external_by_obj[jsv] = rec;
    st->externals.insert(rec);
  } catch (...) {
    /* OOM after JS object creation: detach as above (the JS object
     * will self-clean via its finalize trampoline). */
    rec->state.store(kSuppressed, std::memory_order_relaxed);
    rec->leak_to_gc = true;
    ReleaseHandle(rec);
    st->external_by_obj.erase(jsv);
    st->externals.erase(rec);
    nodeapi::SetLastError(env, napi_generic_failure, "create_external: OOM (containers)");
    return napi_generic_failure;
  }

  napi_status rs = NodeApiScopes_RootIntoInnermost(env, reinterpret_cast<napi_value>(jsv));
  if (rs != napi_ok) {
    /* Rooting failed (OOM / no scope): do not publish the unrooted
     * value. Detach to the same self-deleting leak so the created JS
     * object (unreferenced, will collect) cannot dereference freed
     * memory. The user finalizer MUST NOT run on this failure path
     * (the caller did not publish the value); the detached record is
     * a pure no-op self-cleanup, so clear it before detaching. */
    rec->state.store(kSuppressed, std::memory_order_relaxed);
    rec->leak_to_gc = true;
    rec->finalize = nullptr;
    ec = JsSetExternalData(jsv, nullptr); /* best effort detach */
    (void)ec;
    ReleaseHandle(rec);
    st->external_by_obj.erase(jsv);
    st->externals.erase(rec);
    nodeapi::SetLastError(env, napi_generic_failure, "create_external: root failed");
    return napi_generic_failure;
  }
  *result = reinterpret_cast<napi_value>(jsv);
  nodeapi::ClearLastError(env);
  return napi_ok;
}

napi_status NAPI_CDECL napi_get_value_external(napi_env env, napi_value value,
                                               void** result) {
  if (!value || !result) {
    if (env) nodeapi::SetLastError(env, napi_invalid_arg, "get_value_external: null arg");
    return napi_invalid_arg;
  }
  nodeapi::EntryGuard guard(env, /*allowClosing=*/true,
                            /*allowPending=*/true, /*needContext=*/true);
  if (!guard.ok()) return guard.status();
  MaybeDrain(env, LookupState(ToNodeEnv(env), false));

  JsValueRef jsv = reinterpret_cast<JsValueRef>(value);
  bool has = false;
  JsErrorCode ec = JsHasExternalData(jsv, &has);
  if (ec != JsNoError) return map_js(env, ec, "JsHasExternalData");
  if (!has) {
    /* C status for non-external (docs+impl agree: invalid_arg, NOT
     * object_expected). */
    nodeapi::SetLastError(env, napi_invalid_arg, "get_value_external: not an external");
    return napi_invalid_arg;
  }
  void* raw = nullptr;
  ec = JsGetExternalData(jsv, &raw);
  if (ec != JsNoError) return map_js(env, ec, "JsGetExternalData");
  void* out = raw; /* foreign external: raw data is the user pointer */
  if (raw != nullptr) {
    /* NEVER dereference `raw` to test membership: a foreign external's
     * data may be any pointer (e.g. the vendored tests use 0x1), and
     * reading it would fault. Membership is by pointer equality in
     * this env's live set only; the record's fields are read only
     * after membership proves the pointer is a live provider record. */
    nodeapi::NodeEnv* nenv = ToNodeEnv(env);
    WrapState* st = LookupState(nenv, false);
    if (st != nullptr &&
        st->externals.find(static_cast<WrapRecord*>(raw)) !=
            st->externals.end()) {
      out = static_cast<WrapRecord*>(raw)->user_data;
    }
  }
  *result = out;
  nodeapi::ClearLastError(env);
  return napi_ok;
}

/* ------------------------------------------------------------------ */
/* napi_wrap / napi_unwrap / napi_remove_wrap (N3.3).                  */
/* ------------------------------------------------------------------ */

napi_status NAPI_CDECL napi_wrap(napi_env env, napi_value js_object,
                                 void* native_object,
                                 node_api_basic_finalize finalize_cb,
                                 void* finalize_hint, napi_ref* result) {
  if (!js_object) {
    if (env) nodeapi::SetLastError(env, napi_invalid_arg, "wrap: js_object is NULL");
    return napi_invalid_arg;
  }
  nodeapi::EntryGuard guard(env, /*allowClosing=*/false,
                            /*allowPending=*/false, /*needContext=*/true);
  if (!guard.ok()) return guard.status();
  MaybeDrain(env, LookupState(ToNodeEnv(env), false));

  JsValueRef obj = reinterpret_cast<JsValueRef>(js_object);
  JsValueType t = JsUndefined;
  if (!get_type_ok(env, js_object, &t) || !is_wrapable_type(t)) {
    /* C status: invalid_arg (IsObject strict check, NOT object_expected). */
    nodeapi::SetLastError(env, napi_invalid_arg, "wrap: not an object");
    return napi_invalid_arg;
  }

  nodeapi::NodeEnv* nenv = ToNodeEnv(env);
  WrapState* st = LookupState(nenv, true);
  if (!st) {
    nodeapi::SetLastError(env, napi_generic_failure, "wrap: OOM (state)");
    return napi_generic_failure;
  }
  WrapRecord* existing = FindWrap(st, obj);

  /* Upgrade path: wrap a tag-only record (data slot still free). */
  WrapRecord* rec = nullptr;
  bool fresh = false;
  if (existing) {
    if (existing->has_data_wrap) {
      nodeapi::SetLastError(env, napi_invalid_arg, "wrap: already wrapped");
      return napi_invalid_arg;
    }
    rec = existing;
    /* Save for rollback if the optional ref fails. */
  } else {
    rec = new (std::nothrow) WrapRecord();
    if (!rec) {
      nodeapi::SetLastError(env, napi_generic_failure, "wrap: OOM (record)");
      return napi_generic_failure;
    }
    rec->env = nenv;
    rec->st = st;
    fresh = true;
  }

  void* saved_data = rec->user_data;
  node_api_basic_finalize saved_fin = rec->finalize;
  void* saved_hint = rec->hint;
  bool saved_has = rec->has_data_wrap;
  rec->user_data = native_object;
  rec->finalize = finalize_cb;
  rec->hint = finalize_hint;
  rec->has_data_wrap = true;
  rec->has_wrap = true;

  if (fresh) {
    napi_status hs = CreateHandle(env, obj, rec);
    if (hs != napi_ok) { delete rec; return hs; }
    napi_status rs = RegisterBeforeCollect(env, obj, rec);
    if (rs != napi_ok) { ReleaseHandle(rec); delete rec; return rs; }
    try {
      st->wraps[obj] = rec;
    } catch (...) {
      (void)ClearBeforeCollect(obj);
      ReleaseHandle(rec);
      delete rec;
      nodeapi::SetLastError(env, napi_generic_failure, "wrap: OOM (containers)");
      return napi_generic_failure;
    }
  }

  napi_status refs = CreateOptionalRef(env, js_object, result);
  if (refs != napi_ok) {
    if (fresh) {
      RollbackWrap(st, obj, rec);
    } else {
      /* Restore the tag-only state. */
      rec->user_data = saved_data;
      rec->finalize = saved_fin;
      rec->hint = saved_hint;
      rec->has_data_wrap = saved_has;
      rec->has_wrap = false;
    }
    return refs;
  }
  nodeapi::ClearLastError(env);
  return napi_ok;
}

napi_status NAPI_CDECL napi_unwrap(napi_env env, napi_value js_object,
                                   void** result) {
  if (!js_object || !result) {
    if (env) nodeapi::SetLastError(env, napi_invalid_arg, "unwrap: null arg");
    return napi_invalid_arg;
  }
  nodeapi::EntryGuard guard(env, /*allowClosing=*/true,
                            /*allowPending=*/false, /*needContext=*/true);
  if (!guard.ok()) return guard.status();
  MaybeDrain(env, LookupState(ToNodeEnv(env), false));

  JsValueRef obj = reinterpret_cast<JsValueRef>(js_object);
  JsValueType t = JsUndefined;
  if (!get_type_ok(env, js_object, &t) || !is_wrapable_type(t)) {
    nodeapi::SetLastError(env, napi_invalid_arg, "unwrap: not an object");
    return napi_invalid_arg;
  }
  WrapState* st = LookupState(ToNodeEnv(env), false);
  if (!st) {
    nodeapi::SetLastError(env, napi_invalid_arg, "unwrap: unwrapped");
    return napi_invalid_arg;
  }
  WrapRecord* rec = FindWrap(st, obj);
  if (!rec || !rec->has_wrap) {
    nodeapi::SetLastError(env, napi_invalid_arg, "unwrap: unwrapped");
    return napi_invalid_arg;
  }
  *result = rec->user_data;
  nodeapi::ClearLastError(env);
  return napi_ok;
}

napi_status NAPI_CDECL napi_remove_wrap(napi_env env, napi_value js_object,
                                        void** result) {
  if (!js_object) {
    if (env) nodeapi::SetLastError(env, napi_invalid_arg, "remove_wrap: js_object is NULL");
    return napi_invalid_arg;
  }
  nodeapi::EntryGuard guard(env, /*allowClosing=*/true,
                            /*allowPending=*/false, /*needContext=*/true);
  if (!guard.ok()) return guard.status();
  MaybeDrain(env, LookupState(ToNodeEnv(env), false));

  JsValueRef obj = reinterpret_cast<JsValueRef>(js_object);
  JsValueType t = JsUndefined;
  if (!get_type_ok(env, js_object, &t) || !is_wrapable_type(t)) {
    nodeapi::SetLastError(env, napi_invalid_arg, "remove_wrap: not an object");
    return napi_invalid_arg;
  }
  WrapState* st = LookupState(ToNodeEnv(env), false);
  if (!st) {
    nodeapi::SetLastError(env, napi_invalid_arg, "remove_wrap: unwrapped");
    return napi_invalid_arg;
  }
  WrapRecord* rec = FindWrap(st, obj);
  if (!rec || !rec->has_wrap) {
    nodeapi::SetLastError(env, napi_invalid_arg, "remove_wrap: unwrapped");
    return napi_invalid_arg;
  }
  /* Suppression race (AMB-WRAP-1, spec choice invalid_arg): GC won
   * LIVE->QUEUED already, so the wrap is already gone from the side
   * table's perspective -- report as unwrapped rather than silently
   * suppressing an in-flight finalizer. */
  int expected = kLive;
  if (!rec->state.compare_exchange_strong(expected, kSuppressed,
                                          std::memory_order_acq_rel,
                                          std::memory_order_relaxed)) {
    nodeapi::SetLastError(env, napi_invalid_arg, "remove_wrap: already collected");
    return napi_invalid_arg;
  }
  void* data = rec->user_data;
  JsErrorCode cleared = ClearBeforeCollect(obj);
  RemoveContainers(st, rec);
  ReleaseHandle(rec);
  if (result != nullptr) *result = data; /* result==NULL allowed */
  nodeapi::ClearLastError(env);
  if (cleared == JsNoError || !rec->before_collect) {
    /* Registration definitely cleared (or never registered): no late
     * trampoline will dereference this record. */
    delete rec;
  } else {
    /* Clear failed (invalid object state): the callback may still
     * fire. Detach to the self-deleting leak (trampoline will see
     * SUPPRESSED + leak and delete without touching the state). */
    rec->leak_to_gc = true;
  }
  return napi_ok;
}

/* ------------------------------------------------------------------ */
/* napi_add_finalizer (N3.3, R-WRAP-12).                                */
/* ------------------------------------------------------------------ */

napi_status NAPI_CDECL napi_add_finalizer(napi_env env, napi_value js_object,
                                          void* finalize_data,
                                          node_api_basic_finalize finalize_cb,
                                          void* finalize_hint, napi_ref* result) {
  if (!js_object) {
    if (env) nodeapi::SetLastError(env, napi_invalid_arg, "add_finalizer: js_object is NULL");
    return napi_invalid_arg;
  }
  /* C status for pending: executes (Yes). Strict Alive-only. */
  nodeapi::EntryGuard guard(env, /*allowClosing=*/false,
                            /*allowPending=*/true, /*needContext=*/true);
  if (!guard.ok()) return guard.status();
  MaybeDrain(env, LookupState(ToNodeEnv(env), false));

  JsValueRef obj = reinterpret_cast<JsValueRef>(js_object);
  JsValueType t = JsUndefined;
  if (!get_type_ok(env, js_object, &t) || !is_wrapable_type(t)) {
    nodeapi::SetLastError(env, napi_invalid_arg, "add_finalizer: not an object");
    return napi_invalid_arg;
  }

  nodeapi::NodeEnv* nenv = ToNodeEnv(env);
  WrapState* st = LookupState(nenv, true);
  if (!st) {
    nodeapi::SetLastError(env, napi_generic_failure, "add_finalizer: OOM (state)");
    return napi_generic_failure;
  }
  WrapRecord* existing = FindWrap(st, obj);
  /* N5.1: Node allows multiple finalizers on one object (vendored
   * testFinalizer adds two; wrap+add both fire). The head record owns
   * the single JsRT before-collect callback; further finalizers chain
   * off the head (see WrapRecord::chained). Tag-only records are
   * upgraded in place (no chain needed). */
  WrapRecord* rec = nullptr;
  bool fresh = false;
  bool chained = false;
  WrapRecord* chained_head = nullptr;
  if (existing) {
    if (existing->has_data_wrap) {
      // Chain: allocate a rider record, no handle/callback of its own.
      WrapRecord* rider = new (std::nothrow) WrapRecord();
      if (!rider) {
        nodeapi::SetLastError(env, napi_generic_failure, "add_finalizer: OOM (record)");
        return napi_generic_failure;
      }
      rider->env = nenv;
      rider->st = st;
      rider->user_data = finalize_data;
      rider->finalize = finalize_cb;
      rider->hint = finalize_hint;
      rider->has_data_wrap = true;
      try {
        existing->chained.push_back(rider);
      } catch (...) {
        delete rider;
        nodeapi::SetLastError(env, napi_generic_failure, "add_finalizer: OOM (containers)");
        return napi_generic_failure;
      }
      napi_status refs = CreateOptionalRef(env, js_object, result);
      if (refs != napi_ok) {
        // Roll back the chain append.
        for (auto it = existing->chained.begin(); it != existing->chained.end(); ++it) {
          if (*it == rider) { existing->chained.erase(it); break; }
        }
        delete rider;
        return refs;
      }
      nodeapi::ClearLastError(env);
      return napi_ok;
    }
    rec = existing;
  } else {
    rec = new (std::nothrow) WrapRecord();
    if (!rec) {
      nodeapi::SetLastError(env, napi_generic_failure, "add_finalizer: OOM (record)");
      return napi_generic_failure;
    }
    rec->env = nenv;
    rec->st = st;
    fresh = true;
  }

  void* saved_data = rec->user_data;
  node_api_basic_finalize saved_fin = rec->finalize;
  void* saved_hint = rec->hint;
  bool saved_has = rec->has_data_wrap;
  rec->user_data = finalize_data;
  rec->finalize = finalize_cb;
  rec->hint = finalize_hint;
  rec->has_data_wrap = true;

  if (fresh) {
    napi_status hs = CreateHandle(env, obj, rec);
    if (hs != napi_ok) { delete rec; return hs; }
    napi_status rs = RegisterBeforeCollect(env, obj, rec);
    if (rs != napi_ok) { ReleaseHandle(rec); delete rec; return rs; }
    try {
      st->wraps[obj] = rec;
    } catch (...) {
      (void)ClearBeforeCollect(obj);
      ReleaseHandle(rec);
      delete rec;
      nodeapi::SetLastError(env, napi_generic_failure, "add_finalizer: OOM (containers)");
      return napi_generic_failure;
    }
  }

  napi_status refs = CreateOptionalRef(env, js_object, result);
  if (refs != napi_ok) {
    if (fresh) {
      RollbackWrap(st, obj, rec);
    } else {
      rec->user_data = saved_data;
      rec->finalize = saved_fin;
      rec->hint = saved_hint;
      rec->has_data_wrap = saved_has;
    }
    return refs;
  }
  nodeapi::ClearLastError(env);
  return napi_ok;
}

/* ------------------------------------------------------------------ */
/* napi_type_tag_object / napi_check_object_type_tag (N3.3).            */
/* ------------------------------------------------------------------ */

napi_status NAPI_CDECL napi_type_tag_object(napi_env env, napi_value value,
                                            const napi_type_tag* type_tag) {
  if (!value || !type_tag) {
    if (env) nodeapi::SetLastError(env, napi_invalid_arg, "type_tag_object: null arg");
    return napi_invalid_arg;
  }
  nodeapi::EntryGuard guard(env, /*allowClosing=*/false,
                            /*allowPending=*/false, /*needContext=*/true);
  if (!guard.ok()) return guard.status();
  MaybeDrain(env, LookupState(ToNodeEnv(env), false));

  JsValueRef obj = reinterpret_cast<JsValueRef>(value);
  JsValueType t = JsUndefined;
  if (!get_type_ok(env, value, &t) || !is_wrapable_type(t)) {
    /* C status: object_expected if ToObject fails. Provider binds
     * this strictly to object handles (primitives cannot carry a
     * side-table entry); null/undefined are the ToObject-failure cases
     * in practice, and other primitives degrade to the same code. */
    nodeapi::SetLastError(env, napi_object_expected, "type_tag_object: not an object");
    return napi_object_expected;
  }

  nodeapi::NodeEnv* nenv = ToNodeEnv(env);
  WrapState* st = LookupState(nenv, true);
  if (!st) {
    nodeapi::SetLastError(env, napi_generic_failure, "type_tag_object: OOM (state)");
    return napi_generic_failure;
  }
  WrapRecord* rec = FindWrap(st, obj);
  if (rec) {
    if (rec->has_tag) {
      nodeapi::SetLastError(env, napi_invalid_arg, "type_tag_object: already tagged");
      return napi_invalid_arg;
    }
    rec->tag = *type_tag; /* by value */
    rec->has_tag = true;
    nodeapi::ClearLastError(env);
    return napi_ok;
  }
  /* Tag-only record (vendored TypeTaggedInstance tags a plain
   * object): create, register for collection cleanup, store by
   * value. No data slot, no finalizer (drain frees silently). */
  rec = new (std::nothrow) WrapRecord();
  if (!rec) {
    nodeapi::SetLastError(env, napi_generic_failure, "type_tag_object: OOM (record)");
    return napi_generic_failure;
  }
  rec->env = nenv;
  rec->st = st;
  napi_status hs = CreateHandle(env, obj, rec);
  if (hs != napi_ok) { delete rec; return hs; }
  napi_status rs = RegisterBeforeCollect(env, obj, rec);
  if (rs != napi_ok) { ReleaseHandle(rec); delete rec; return rs; }
  rec->tag = *type_tag;
  rec->has_tag = true;
  try {
    st->wraps[obj] = rec;
  } catch (...) {
    (void)ClearBeforeCollect(obj);
    ReleaseHandle(rec);
    delete rec;
    nodeapi::SetLastError(env, napi_generic_failure, "type_tag_object: OOM (containers)");
    return napi_generic_failure;
  }
  nodeapi::ClearLastError(env);
  return napi_ok;
}

napi_status NAPI_CDECL napi_check_object_type_tag(napi_env env, napi_value value,
                                                  const napi_type_tag* type_tag,
                                                  bool* result) {
  if (!value || !type_tag || !result) {
    if (env) nodeapi::SetLastError(env, napi_invalid_arg, "check_object_type_tag: null arg");
    return napi_invalid_arg;
  }
  nodeapi::EntryGuard guard(env, /*allowClosing=*/true,
                            /*allowPending=*/false, /*needContext=*/true);
  if (!guard.ok()) return guard.status();
  MaybeDrain(env, LookupState(ToNodeEnv(env), false));

  JsValueType t = JsUndefined;
  if (!get_type_ok(env, value, &t)) {
    /* JsGetValueType failure: map it (boxed ToObject would fail the
     * same way for broken handles). */
    nodeapi::SetLastError(env, napi_generic_failure, "check_object_type_tag: JsGetValueType failed");
    return napi_generic_failure;
  }
  if (t == JsNull || t == JsUndefined) {
    /* C status: object_expected if js_object fails ToObject. */
    nodeapi::SetLastError(env, napi_object_expected, "check_object_type_tag: not an object");
    return napi_object_expected;
  }
  *result = false;
  if (!is_wrapable_type(t)) {
    /* Other primitives (string/number/bool/symbol): ToObject boxes
     * them, so this is NOT an error -- an untagged primitive simply
     * has no tag (ok + false). */
    nodeapi::ClearLastError(env);
    return napi_ok;
  }
  JsValueRef obj = reinterpret_cast<JsValueRef>(value);
  WrapState* st = LookupState(ToNodeEnv(env), false);
  WrapRecord* rec = st ? FindWrap(st, obj) : nullptr;
  if (rec && rec->has_tag) {
    *result = (rec->tag.lower == type_tag->lower &&
               rec->tag.upper == type_tag->upper);
  } else {
    *result = false;
  }
  nodeapi::ClearLastError(env);
  return napi_ok;
}

/* ------------------------------------------------------------------ */
/* Drain / teardown (destroy steps 2/6).                                */
/* ------------------------------------------------------------------ */

void nodeapi_wrap_drain(napi_env env) {
  nodeapi::NodeEnv* nenv = ToNodeEnv(env);
  if (!nenv) return;
  WrapState* st = LookupState(nenv, false);
  if (st) DrainState(env, st);
}

void nodeapi_wrap_teardown(napi_env env) {
  nodeapi::NodeEnv* nenv = ToNodeEnv(env);
  if (!nenv) return;
  WrapState* st = LookupState(nenv, false);
  if (!st) return;
  nodeapi::ContextGuard guard(env); /* backstop needs the env context */
  (void)guard.entered();

  /* Step 2 + 6drain: already-enqueued finalizers run before the
   * backstop (structs 3-5 then run outside this helper: they are no
   * ops for wraps, but the live objects' finalizers must not fire
   * before hooks in case of an ordering probe -- see NodeApiEnv.cpp:
   * this helper is called only at step 6, and step 2's drain is the
   * separate nodeapi_wrap_drain call there). */
  DrainState(env, st);

  /* Snapshot every remaining live record (env thread). Finalizers
   * running below may mutate the maps (Closing-tolerant removes), so
   * iterate a stable vector and re-check each record on arrival. */
  std::vector<WrapRecord*> wraps;
  std::vector<WrapRecord*> externals;
  for (auto& kv : st->wraps) {
    if (kv.second) wraps.push_back(kv.second);
  }
  for (WrapRecord* r : st->externals) {
    if (r) externals.push_back(r);
  }

  for (WrapRecord* rec : wraps) {
    /* Claim exactly once (R-WRAP-6). Queued records were already
     * drained above and freed, so no record here is queued; if one
     * was enqueued by GC from a hook between step 2 and now it is
     * still in the set live-marked QUEUED -- skip it here and let the
     * post-backstop drain run it. */
    int expected = kLive;
    if (!rec->state.compare_exchange_strong(expected, kDone,
                                            std::memory_order_acq_rel,
                                            std::memory_order_relaxed)) {
      continue;
    }
    /* Find the JS object for before-collect clearing: the wrap map
     * key is the object pointer and the record is still in the map
     * until RemoveContainers below. Capture the key first. */
    JsValueRef obj = JS_INVALID_REFERENCE;
    for (auto& kv : st->wraps) {
      if (kv.second == rec) { obj = kv.first; break; }
    }
    JsErrorCode cleared = JsNoError;
    if (rec->before_collect && obj != JS_INVALID_REFERENCE) {
      cleared = ClearBeforeCollect(obj);
    }
    RemoveContainers(st, rec);
    ReleaseHandle(rec);
    // N5.1: snapshot riders before running the head finalizer.
    std::vector<WrapRecord*> riders;
    if (!rec->chained.empty()) riders = rec->chained;
    RunFinalizer(env, rec);
    for (WrapRecord* rider : riders) {
      int rexp = kLive;
      if (rider->state.compare_exchange_strong(rexp, kDone,
              std::memory_order_acq_rel, std::memory_order_relaxed)) {
        RunFinalizer(env, rider);
      }
      delete rider;
    }
    /* Only free now when no late trampoline can fire: never registered
     * (strong-fallback / tag-only edge), or the registration was
     * definitely cleared. If the clear failed (invalid object state),
     * the callback may still be registered -- leak to the
     * self-deleting late trampoline (SUPPRESSED/DONE + leak path)
     * rather than risk a dangling dereference. Steady-state valid
     * objects take the delete path (the probe's teardown arm). */
    if (rec->before_collect && cleared != JsNoError) {
      rec->leak_to_gc = true;
    } else {
      delete rec;
    }
  }

  for (WrapRecord* rec : externals) {
    int expected = kLive;
    if (!rec->state.compare_exchange_strong(expected, kDone,
                                            std::memory_order_acq_rel,
                                            std::memory_order_relaxed)) {
      continue; /* enqueued by GC between step 2 and now; post-drain runs it */
    }
    RemoveContainers(st, rec);
    ReleaseHandle(rec);
    rec->leak_to_gc = true; /* so its late GC finalize self-deletes */
    RunFinalizer(env, rec);
    /* DO NOT delete: the JS external object is still live and may
     * still fire its finalize later (at runtime disposal). The late
     * trampoline sees DONE + leak_to_gc and deletes the record
     * without touching the (freed) state. */
  }

  /* Pick up anything enqueued while backstop finalizers ran (a
   * finalizer forcing GC). This is the step-6 final drain; normally
   * the queue is already empty here. */
  DrainState(env, st);

  /* Any record enqueued during the final drain's finalizers (chained)
   * is picked up by the loop above; one more pass is cheap and keeps
   * the "drain until empty" invariant total when chained finalizers
   * enqueue more during this teardown. */
  DrainState(env, st);

  /* Free the state itself (after drains, nothing can enqueue into it:
   * every live record is DONE now, so a late GC trampoline takes the
   * self-delete path without touching this state). */
  ForgetState(nenv);
  delete st;
}

#ifdef CHAKRA_NODEAPI_TEST_HOOKS

void chakra_nodeapi_test_drain_finalizers(napi_env env) {
  nodeapi::NodeEnv* nenv = ToNodeEnv(env);
  if (!nenv) return;
  nodeapi::ContextGuard guard(env);
  (void)guard.entered();
  nodeapi_wrap_drain(env);
}

size_t chakra_nodeapi_test_wrap_live_count(napi_env env) {
  nodeapi::NodeEnv* nenv = ToNodeEnv(env);
  if (!nenv) return 0;
  WrapState* st = LookupState(nenv, false);
  if (!st) return 0;
  return st->wraps.size() + st->externals.size();
}

size_t chakra_nodeapi_test_wrap_queue_count(napi_env env) {
  nodeapi::NodeEnv* nenv = ToNodeEnv(env);
  if (!nenv) return 0;
  WrapState* st = LookupState(nenv, false);
  if (!st) return 0;
  return st->queued_count.load(std::memory_order_relaxed);
}

#endif /* CHAKRA_NODEAPI_TEST_HOOKS */

} /* extern "C" */
