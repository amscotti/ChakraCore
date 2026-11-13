//-------------------------------------------------------------------------------------------------------
// Copyright (c) ChakraCore Project Contributors. All rights reserved.
// Licensed under the MIT license. See LICENSE.txt file in the project root for full license information.
//-------------------------------------------------------------------------------------------------------
//
// Node-API references (N1.3). See NodeApiRefs.h for the reconciled
// NodeEnv interface, ownership record, and v8 restriction summary.
//
// Semantics notes (semantics section 3):
//   R-REF-1: provider counts are authoritative; JsAddRef/JsRelease are
//     crossed only on 0<->1 transitions, via TransitionToStrong /
//     TransitionToWeak below. No call site touches JsAddRef directly
//     except the single counted helper + the N1.2 scope-root store
//     (NodeApiScopes_RootIntoInnermost, HIGH-4).
//   R-REF-2 / plan 3.4 R6: the JsWeakRef handle is itself rooted
//     (JsAddRef on the handle) while stored; released on transition /
//     delete / teardown.
//   R-REF-5: 0->1 creates the strong root exactly once; 1->0 releases
//     it exactly once and creates the weak handle exactly once. The
//     1->0 order is create-weak-while-strong-held, then release-strong,
//     so the referent can never be collected between the two steps.
//     The 0->1 order is addref-while-weak-held, then release-weak.
//   R-REF-7 (HIGH-4 unified): get_reference_value roots live values
//     into the N1.2 scope stack via NodeApiScopes_RootIntoInnermost
//     (R-SC-1) and returns napi_ok + null for a collected weak
//     referent (header docs, js_native_api.h:359-364). The out-param
//     is always written on the collected path (AMB-REF-3: sentinel
//     test). Scope lifecycle (close / destroy step 6b) releases these
//     roots; the refs TU keeps its own strong/weak roots separate.
//   R-REF-8/9: delete releases exactly once regardless of count and
//     marks the record dead (retained until teardown) so a second
//     delete or any use returns napi_invalid_arg instead of a UAF.
//   R-REF-10/12 caveats: tagged numbers no-op in JsAddRef (v8 rejects
//     numbers at create, so this path is unreachable via create; the
//     scope rooting for object/function/symbol/external never relies
//     on the returned count). Arena/silent-unrooted objects cannot be
//     detected via the API (AMB-REF-2); mitigation is the 10x GC
//     churn arm of the probe plus the rule that only JsCreate* / JS
//     execution values are ever JsAddRef'd here, never a native cast.
//     GAP-REF-BIGINT: BigInt is not yet discriminated (reports as
//     JsObject via JsGetValueType, audit/PR-9). Until N2.6 lands, a
//     BigInt referent would be treated as an object and allowed.
//     The probe does not create a BigInt reference; the gap is
//     documented, not faked. Re-verify against the vendored header
//     docs at include/nodeapi/js_native_api.h:330 (which does not
//     repeat the type allowlist, so the discriminating evidence is
//     the upstream implementation IsObject||IsFunction||IsSymbol
//     check cited in doc/node-api-status-mapping.md) -- N2.6 must
//     add a BigInt referent probe once JsGetValueType discriminates
//     it (or the toString-tag path lands).
//
// Header-anchored II / error choices (js_native_api.h:342-364):
//   napi_reference_ref on a collected weak referent is an error
//     (header: "Calling this when the refcount is 0 and the object is
//     unavailable results in an error") -> napi_invalid_arg.
//   napi_reference_unref at count 0 is an error (header: "Calling
//     this when the refcount is already 0 results in an error")
//     -> napi_invalid_arg. This overrides semantics AMB-REF-1's
//     provisional napi_ok choice: header docs are the oracle for
//     error-ness; the exact code (invalid_arg here) is the provider
//     choice recorded in the N1.3 task brief (upstream C uses
//     generic_failure for unref and ok for ref-collected -- see
//     doc/node-api-status-mapping.md AMB-REF-1; diverge deliberately,
//     expect N1.5 to re-confirm once B/C are vendored).
//   Pending-exception allowlist: napi_delete_reference is the
//     documented allowlist lane (semantics section 6.4, plan 3.5);
//     the other four functions are gated (return
//     napi_pending_exception without executing, exception preserved).
//     This is the task-brief-authoritative B-list gate; the upstream
//     C implementation executes all five with pending (status-mapping
//     "Yes" column). N1.5 owns the B-vs-C decision once B/C are
//     vendored; N1.3 does not silently flip.
//
// N1.1 integration: this TU never casts napi_env. Every entry
//   validates via nodeapi::CheckAlive (live/alive/owner-thread,
//   R-ENV-4/11), enters via nodeapi::ContextGuard (plan 3.2), and
//   fetches the refs state via NodeApiEnv_RefsState (registry
//   VALUE lookup, R-ENV-10). Last-error recording goes through
//   nodeapi::SetLastError (real LastError buffer). Teardown entry
//   points (DestroyForEnv / nodeapi_refs_teardown_all) bypass the
//   Alive check because destroy sets Closing before step 6a; they
//   still enter the context themselves (standalone caller) or run
//   inside the destroy guard (step-6a caller) and release every root.

#include "NodeApiRefs.h"

#include "NodeApiEnv.h"
#include "NodeApiHostApi.h"
#include "NodeApiScopes.h" /* HIGH-4: RootIntoInnermost for get values */
#include "ChakraCore.h" /* JsWeakRef, JsCreateWeakReference */

#include <cassert>
#include <cstdint>
#include <new>
#include <unordered_map>

/* One napi_ref record. `count` is authoritative (R2); `is_strong`
 * tracks which Js handle is currently held. Dead records are retained
 * (root already released) until teardown so double-delete /
 * use-after-delete is a clean napi_invalid_arg. */
struct NodeApiRefRecord {
  uint32_t count = 0;
  bool dead = false;
  bool is_strong = false;
  bool immortal = false;
  JsValueRef strong_value = JS_INVALID_REFERENCE;
  JsRef weak_handle = JS_INVALID_REFERENCE;
};

/* Per-env refs state (opaque to NodeApiEnv.h, stored as
 * NodeEnv::refs_state). Completes the forward-declared global
 * ::NodeApiRefsState (defined at global scope, NOT inside the
 * anonymous namespace below, so the extern "C" API signatures name
 * the canonical type). After HIGH-4 there is no refs-owned scope list:
 * get_reference_value roots live in the N1.2 scope stack. */
struct NodeApiRefsState {
  std::unordered_map<napi_ref, NodeApiRefRecord*> refs;
  size_t baseline = 0;
};

namespace {

using RefsState = NodeApiRefsState;

RefsState* AsState(NodeApiRefsState* state) {
  return state;
}

/* Entry-guard light (see NodeApiScopes.cpp pattern): NULL/dead env
 * => invalid_arg; not Alive => generic_failure (R-ENV-4); wrong
 * thread => debug assert + invalid_arg (R-ENV-11). Delegates to the
 * real N1.1 guard so refs cannot drift from env discipline. */
napi_status validate_env(napi_env env, RefsState** out) {
  if (env == nullptr || out == nullptr) return napi_invalid_arg;
  napi_status v = nodeapi::CheckAlive(env);
  if (v != napi_ok) return v;
  *out = AsState(NodeApiEnv_RefsState(env));
  if (*out == nullptr) return napi_invalid_arg; /* env without refs state: destroy window/alloc bug */
  return napi_ok;
}

/* Teardown-compatible variant: allows Closing (only Dead fails).
 * Used for napi_delete_reference and napi_get_reference_value,
 * which must succeed during destroy steps 2-6 so finalizers
 * and hooks can clean up refs while the env is Closing. This
 * aligns ref Closing parity with lifetime get/remove (see
 * NodeApiLifetime.h Closing policy). Creation (create_reference)
 * and ref/unref stay strict Alive-only (no new registration
 * during teardown). The guard reuses CheckAliveAllowClosing
 * (N1.1) once N1.5 lands; here it is a minimal local helper to
 * avoid a full redesign (task brief). */
napi_status validate_env_allow_closing(napi_env env, RefsState** out) {
  if (env == nullptr || out == nullptr) return napi_invalid_arg;
  napi_status v = nodeapi::CheckAliveAllowClosing(env);
  if (v != napi_ok) return v;
  *out = AsState(NodeApiEnv_RefsState(env));
  if (*out == nullptr) return napi_invalid_arg;
  return napi_ok;
}

/* Must be called with the env context current (ContextGuard). */
bool has_pending() {
  bool has = false;
  if (JsHasException(&has) != JsNoError) return false;
  return has;
}

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

/* V8 allowlist (plan 1.2): object/function/symbol/external only.
 * External reports as JsObject via JsGetValueType and is covered.
 * Object-like kinds (array/error/arraybuffer/typedarray/dataview)
 * are napi_object in Node's napi_typeof and are allowed. BigInt
 * currently reports as JsObject (JsValueType has no BigInt case,
 * ChakraCommon.h:595-649) -- the GAP-REF-BIGINT hole: a BigInt
 * referent would be allowed as an object. N2.6 closes it. */
bool is_allowed_type(JsValueType t) {
  switch (t) {
    case JsObject:
    case JsFunction:
    case JsSymbol:
    case JsArray:
    case JsError:
    case JsArrayBuffer:
    case JsTypedArray:
    case JsDataView:
      return true;
    case JsUndefined:
    case JsNull:
    case JsNumber:
    case JsString:
    case JsBoolean:
    default:
      return false;
  }
}

/* Lookup by handle equality; never dereferences a foreign pointer.
 * Deleted records are retained in the map (dead flag) so use-after-
 * delete is a clean napi_invalid_arg. After teardown the map is
 * cleared and handles are gone; pointer comparison is still safe. */
NodeApiRefRecord* find_ref(RefsState* st, napi_ref ref) {
  if (!ref || !st) return nullptr;
  auto it = st->refs.find(ref);
  if (it == st->refs.end()) return nullptr;
  return it->second;
}

/* 0 -> 1 transition (R-REF-5). Caller guarantees rec is weak
 * (is_strong == false) or immortal-weak (is_strong == true with
 * immortal == true and count == 0). Sets count to 1 on success.
 * Failure leaves the record in its pre-call state. */
napi_status transition_to_strong(napi_env env, NodeApiRefRecord* rec) {
  if (rec->is_strong) {
    /* Immortal weak-at-0: already rooted, just repin. */
    rec->count = 1;
    return napi_ok;
  }
  JsValueRef v = JS_INVALID_REFERENCE;
  JsErrorCode err = JsGetWeakReferenceValue(rec->weak_handle, &v);
  if (err != JsNoError) return map_js(env, err, "JsGetWeakReferenceValue");
  if (v == JS_INVALID_REFERENCE) {
    /* Collected: napi_reference_ref on a dead weak ref is an error. */
    nodeapi::SetLastError(env, napi_invalid_arg,
                          "reference_ref on collected weak ref");
    return napi_invalid_arg;
  }
  /* Root while the weak handle is still held. */
  err = JsAddRef(v, nullptr);
  if (err != JsNoError) return map_js(env, err, "JsAddRef (ref)");
  /* Release the weak handle root exactly once. */
  err = JsRelease(rec->weak_handle, nullptr);
  if (err != JsNoError) {
    (void)JsRelease(v, nullptr); /* rollback, weak stays intact */
    return map_js(env, err, "JsRelease weak handle (ref)");
  }
  rec->strong_value = v;
  rec->weak_handle = JS_INVALID_REFERENCE;
  rec->is_strong = true;
  rec->count = 1;
  return napi_ok;
}

/* 1 -> 0 transition (R-REF-5). Caller guarantees rec is strong with
 * count == 1 and not immortal (immortal 1 -> 0 stays rooted).
 * Sets count to 0 on success. Creates the weak handle while the
 * strong root is still held so the referent cannot be lost. */
napi_status transition_to_weak(napi_env env, NodeApiRefRecord* rec) {
  if (!rec->is_strong) {
    rec->count = 0;
    return napi_ok;
  }
  if (rec->immortal) {
    rec->count = 0;
    return napi_ok;
  }
  JsWeakRef w = JS_INVALID_REFERENCE;
  JsErrorCode err = JsCreateWeakReference(rec->strong_value, &w);
  if (err == JsNoWeakRefRequired) {
    /* Value never collected: keep the root, mark immortal. */
    rec->immortal = true;
    rec->count = 0;
    return napi_ok;
  }
  if (err != JsNoError) return map_js(env, err, "JsCreateWeakReference");
  err = JsAddRef(w, nullptr); /* R6: root the handle */
  if (err != JsNoError) {
    (void)JsRelease(w, nullptr);
    return map_js(env, err, "JsAddRef weak handle");
  }
  JsValueRef sv = rec->strong_value;
  err = JsRelease(sv, nullptr);
  if (err != JsNoError) {
    (void)JsRelease(w, nullptr); /* rollback, strong stays intact */
    return map_js(env, err, "JsRelease (unref)");
  }
  rec->weak_handle = w;
  rec->strong_value = JS_INVALID_REFERENCE;
  rec->is_strong = false;
  rec->count = 0;
  return napi_ok;
}

/* HIGH-4 unified rooting: live get values go into the N1.2 scope stack
 * via NodeApiScopes_RootIntoInnermost (R-SC-1/R-REF-7). The scopes TU
 * owns the slot (JsAddRef + vector) and its OOM rollback; this TU owns
 * only the ref-record strong/weak roots. On success the value is
 * published; on OOM/generic_failure the caller's out-param is untouched
 * (Scopes never writes it). */
napi_status root_into_innermost_scope(napi_env env, JsValueRef v,
                                      napi_value* result) {
  napi_value nv = reinterpret_cast<napi_value>(v);
  napi_status rs = NodeApiScopes_RootIntoInnermost(env, nv);
  if (rs != napi_ok) return rs;
  *result = nv;
  return napi_ok;
}

/* Release every ref-record root, then free all records. Entries are
 * released exactly once; dead records' roots were already released
 * at delete and are skipped. Scope roots are owned by the N1.2 stack
 * (released at scope close / step 6b, not here). Requires the env
 * context current (the caller entered ContextGuard). State struct is
 * NOT freed here. */
void teardown_state(RefsState* st) {
  if (!st) return;
  for (auto& kv : st->refs) {
    NodeApiRefRecord* rec = kv.second;
    if (rec && !rec->dead) {
      if (rec->is_strong) {
        (void)JsRelease(rec->strong_value, nullptr);
      } else {
        (void)JsRelease(rec->weak_handle, nullptr);
      }
    }
    delete rec;
  }
  st->refs.clear();
}

} /* namespace */

extern "C" {

NodeApiRefsState* NodeApiRefs_CreateState(void) {
  auto* st = new RefsState();
  st->baseline = 0;
  return reinterpret_cast<NodeApiRefsState*>(st);
}

void NodeApiRefs_DestroyForEnv(napi_env env) {
  if (env == nullptr) return;
  NodeApiRefsState* raw = NodeApiEnv_RefsState(env);
  RefsState* st = AsState(raw);
  if (!st) return;
  /* No Alive check: destroy sets Closing before step 6a, so CheckAlive
   * would (correctly) reject the teardown-internal call. The caller
   * (TeardownRefs) guarantees the env context is current and the
   * ptr is still live (not yet freed). */
  teardown_state(st);
}

void NodeApiRefs_FreeState(NodeApiRefsState* state) {
  if (state == nullptr) return;
  delete AsState(state);
}

void nodeapi_refs_teardown_all(napi_env env) {
  if (!env) return;
  NodeApiRefsState* raw = NodeApiEnv_RefsState(env);
  if (raw == nullptr) return;
  /* Standalone entry (N1.1 destroy step 6a wiring): enter the context
   * itself. When called from TeardownRefs, the destroy's outer guard
   * already made the context current, so this guard is a no-op save/
   * restore pair (ContextGuard handles already-current correctly). */
  nodeapi::ContextGuard guard(env);
  (void)guard.entered();
  teardown_state(AsState(raw));
}

napi_env nodeapi_refs_test_create_env(JsRuntimeHandle runtime,
                                       JsContextRef context) {
  if (runtime == JS_INVALID_REFERENCE || context == JS_INVALID_REFERENCE) {
    return nullptr;
  }
  /* Adapter over the real private host API: the probe then exercises
   * the reconciled env + step-6a wiring (refs_state creation in
   * create, refs teardown in TeardownRefs / destroy). */
  return chakra_nodeapi_create_env(runtime, context, nullptr, nullptr, nullptr);
}

void nodeapi_refs_test_destroy_env(napi_env env) {
  if (!env) return;
  chakra_nodeapi_destroy_env(env);
}

#ifdef CHAKRA_NODEAPI_TEST_HOOKS
size_t NodeApiRefs_RefRootCount(napi_env env) {
  if (!env) return 0;
  NodeApiRefsState* raw = NodeApiEnv_RefsState(env);
  RefsState* st = AsState(raw);
  if (!st) return 0;
  /* Refs-record contribution only (scope roots are counted in the
   * scopes TU). One per live ref: strong (JsAddRef on referent) or
   * weak (R6 JsAddRef on handle, immortal counts as strong). */
  size_t n = 0;
  for (auto& kv : st->refs) {
    if (kv.second && !kv.second->dead) n += 1;
  }
  return n;
}

size_t chakra_nodeapi_test_ref_root_count(napi_env env) {
  /* Debugging-only per-TU counter (refs part). Probes assert
   * against the unified total (chakra_nodeapi_test_root_count). */
  return NodeApiRefs_RefRootCount(env);
}

size_t chakra_nodeapi_test_ref_root_baseline(napi_env env) {
  if (!env) return 0;
  NodeApiRefsState* raw = NodeApiEnv_RefsState(env);
  RefsState* st = AsState(raw);
  if (!st) return 0;
  return st->baseline;
}
#endif /* CHAKRA_NODEAPI_TEST_HOOKS */

/* ---------------------------------------------------------------- */
/* napi_* reference functions (exact header signatures)             */
/* ---------------------------------------------------------------- */

napi_status NAPI_CDECL napi_create_reference(napi_env env, napi_value value,
                                             uint32_t initial_refcount,
                                             napi_ref* result) {
  if (!env || !value || !result) {
    if (env) nodeapi::SetLastError(env, napi_invalid_arg, "create_reference null arg");
    return napi_invalid_arg;
  }
  RefsState* st = nullptr;
  napi_status vs = validate_env(env, &st);
  if (vs != napi_ok) return vs;
  nodeapi::ContextGuard guard(env);
  if (has_pending()) {
    nodeapi::SetLastError(env, napi_pending_exception,
                          "create_reference with pending exception");
    return napi_pending_exception;
  }
  JsValueRef jsv = reinterpret_cast<JsValueRef>(value);
  JsValueType type = JsUndefined;
  JsErrorCode te = JsGetValueType(jsv, &type);
  if (te != JsNoError) return map_js(env, te, "JsGetValueType");
  if (!is_allowed_type(type)) {
    /* v8 restriction: object/function/symbol/external only. No
     * partial root is created (P3-7 counter unchanged). */
    nodeapi::SetLastError(env, napi_invalid_arg, "create_reference disallowed v8 type");
    return napi_invalid_arg;
  }
  NodeApiRefRecord* rec = new (std::nothrow) NodeApiRefRecord();
  if (!rec) {
    nodeapi::SetLastError(env, napi_generic_failure, "ref record OOM");
    return napi_generic_failure;
  }
  rec->count = initial_refcount;
  rec->dead = false;
  rec->is_strong = false;
  rec->immortal = false;
  rec->strong_value = JS_INVALID_REFERENCE;
  rec->weak_handle = JS_INVALID_REFERENCE;
  if (initial_refcount > 0) {
    JsErrorCode ae = JsAddRef(jsv, nullptr);
    if (ae != JsNoError) {
      delete rec;
      return map_js(env, ae, "JsAddRef (create)");
    }
    rec->is_strong = true;
    rec->strong_value = jsv;
  } else {
    JsWeakRef w = JS_INVALID_REFERENCE;
    JsErrorCode we = JsCreateWeakReference(jsv, &w);
    if (we == JsNoWeakRefRequired) {
      JsErrorCode ae = JsAddRef(jsv, nullptr);
      if (ae != JsNoError) {
        delete rec;
        return map_js(env, ae, "JsAddRef (create, immortal)");
      }
      rec->is_strong = true;
      rec->immortal = true;
      rec->strong_value = jsv;
    } else if (we != JsNoError) {
      delete rec;
      return map_js(env, we, "JsCreateWeakReference (create)");
    } else {
      JsErrorCode ae = JsAddRef(w, nullptr); /* R6 */
      if (ae != JsNoError) {
        (void)JsRelease(w, nullptr);
        delete rec;
        return map_js(env, ae, "JsAddRef weak handle (create)");
      }
      rec->weak_handle = w;
    }
  }
  napi_ref handle = reinterpret_cast<napi_ref>(rec);
  try {
    st->refs.emplace(handle, rec);
  } catch (...) {
    if (rec->is_strong) {
      (void)JsRelease(rec->strong_value, nullptr);
    } else {
      (void)JsRelease(rec->weak_handle, nullptr);
    }
    delete rec;
    nodeapi::SetLastError(env, napi_generic_failure, "refs map OOM");
    return napi_generic_failure;
  }
  *result = handle;
  nodeapi::ClearLastError(env);
  return napi_ok;
}

napi_status NAPI_CDECL napi_reference_ref(napi_env env, napi_ref ref,
                                          uint32_t* result) {
  if (!env || !ref) {
    if (env) nodeapi::SetLastError(env, napi_invalid_arg, "reference_ref null arg");
    return napi_invalid_arg;
  }
  RefsState* st = nullptr;
  napi_status vs = validate_env(env, &st);
  if (vs != napi_ok) return vs;
  nodeapi::ContextGuard guard(env);
  if (has_pending()) {
    nodeapi::SetLastError(env, napi_pending_exception,
                          "reference_ref with pending exception");
    return napi_pending_exception;
  }
  NodeApiRefRecord* rec = find_ref(st, ref);
  if (!rec || rec->dead) {
    nodeapi::SetLastError(env, napi_invalid_arg, "reference_ref on deleted/unknown ref");
    return napi_invalid_arg;
  }
  if (rec->count == UINT32_MAX) {
    nodeapi::SetLastError(env, napi_invalid_arg, "reference_ref overflow");
    return napi_invalid_arg;
  }
  if (rec->count == 0) {
    napi_status ts = transition_to_strong(env, rec);
    if (ts != napi_ok) return ts;
  } else {
    rec->count += 1;
  }
  if (result) *result = rec->count;
  nodeapi::ClearLastError(env);
  return napi_ok;
}

napi_status NAPI_CDECL napi_reference_unref(napi_env env, napi_ref ref,
                                            uint32_t* result) {
  if (!env || !ref) {
    if (env) nodeapi::SetLastError(env, napi_invalid_arg, "reference_unref null arg");
    return napi_invalid_arg;
  }
  RefsState* st = nullptr;
  napi_status vs = validate_env(env, &st);
  if (vs != napi_ok) return vs;
  nodeapi::ContextGuard guard(env);
  if (has_pending()) {
    nodeapi::SetLastError(env, napi_pending_exception,
                          "reference_unref with pending exception");
    return napi_pending_exception;
  }
  NodeApiRefRecord* rec = find_ref(st, ref);
  if (!rec || rec->dead) {
    nodeapi::SetLastError(env, napi_invalid_arg, "reference_unref on deleted/unknown ref");
    return napi_invalid_arg;
  }
  if (rec->count == 0) {
    /* Header docs: unref at 0 is an error
     * (js_native_api.h:353-354, "results in an error"). */
    nodeapi::SetLastError(env, napi_invalid_arg, "reference_unref at count 0");
    return napi_invalid_arg;
  }
  if (rec->count > 1) {
    rec->count -= 1;
  } else if (rec->immortal) {
    rec->count = 0;
  } else {
    napi_status ts = transition_to_weak(env, rec);
    if (ts != napi_ok) return ts;
  }
  if (result) *result = rec->count;
  nodeapi::ClearLastError(env);
  return napi_ok;
}

napi_status NAPI_CDECL napi_get_reference_value(napi_env env, napi_ref ref,
                                                napi_value* result) {
  if (!env || !ref || !result) {
    if (env) nodeapi::SetLastError(env, napi_invalid_arg, "get_reference_value null arg");
    return napi_invalid_arg;
  }
  RefsState* st = nullptr;
  // Teardown-compatible getter: Closing succeeds (parity with
  // lifetime get_instance_data; see Closing policy note above).
  // Strict Alive-only for create/ref/unref remains.
  napi_status vs = validate_env_allow_closing(env, &st);
  if (vs != napi_ok) return vs;
  nodeapi::ContextGuard guard(env);
  if (has_pending()) {
    nodeapi::SetLastError(env, napi_pending_exception,
                          "get_reference_value with pending exception");
    return napi_pending_exception;
  }
  NodeApiRefRecord* rec = find_ref(st, ref);
  if (!rec || rec->dead) {
    nodeapi::SetLastError(env, napi_invalid_arg, "get_reference_value on deleted/unknown ref");
    return napi_invalid_arg;
  }
  JsValueRef v = JS_INVALID_REFERENCE;
  if (rec->is_strong) {
    v = rec->strong_value;
  } else {
    JsErrorCode ge = JsGetWeakReferenceValue(rec->weak_handle, &v);
    if (ge != JsNoError) return map_js(env, ge, "JsGetWeakReferenceValue");
    if (v == JS_INVALID_REFERENCE) {
      /* Collected weak referent: ok + null (header docs). */
      *result = nullptr;
      nodeapi::ClearLastError(env);
      return napi_ok;
    }
  }
  /* HIGH-4: root into the innermost N1.2 scope (R-SC-1/R-REF-7), not a
   * refs-owned list. Scope close (or destroy step 6b) releases it;
   * the ref-record roots above stay separate. The root helper uses
   * AllowClosing validation so teardown-compatible gets during
   * Closing still root (released at step 6b). */
  napi_status rs = root_into_innermost_scope(env, v, result);
  if (rs == napi_ok) nodeapi::ClearLastError(env);
  return rs;
}

napi_status NAPI_CDECL napi_delete_reference(node_api_basic_env env_raw,
                                             napi_ref ref) {
  if (!env_raw || !ref) {
    if (env_raw) nodeapi::SetLastError(reinterpret_cast<napi_env>(env_raw), napi_invalid_arg,
                                      "delete_reference null arg");
    return napi_invalid_arg;
  }
  napi_env env = reinterpret_cast<napi_env>(env_raw);
  RefsState* st = nullptr;
  // Teardown-compatible remover: Closing succeeds (parity with
  // lifetime remove hooks; see Closing policy). Previously
  // returned generic_failure in Closing via CheckAlive, which
  // broke finalizer/hook cleanup of refs during teardown.
  napi_status vs = validate_env_allow_closing(env, &st);
  if (vs != napi_ok) return vs;
  nodeapi::ContextGuard guard(env);
  /* Allowlisted lane: no pending check (semantics section 6.4).
   * State/thread checks above still apply. */
  NodeApiRefRecord* rec = find_ref(st, ref);
  if (!rec || rec->dead) {
    nodeapi::SetLastError(env, napi_invalid_arg, "delete_reference on deleted/unknown ref");
    return napi_invalid_arg;
  }
  if (rec->is_strong) {
    /* Best-effort: InExceptionState from JsRelease must not mask the
     * successful delete (section 6.4 precedence); the delete has
     * happened once the record is marked dead. */
    (void)JsRelease(rec->strong_value, nullptr);
  } else {
    (void)JsRelease(rec->weak_handle, nullptr);
  }
  rec->dead = true;
  rec->count = 0;
  rec->strong_value = JS_INVALID_REFERENCE;
  rec->weak_handle = JS_INVALID_REFERENCE;
  nodeapi::ClearLastError(env);
  return napi_ok;
}

} /* extern "C" */
