//-------------------------------------------------------------------------------------------------------
// Copyright (c) ChakraCore Project Contributors. All rights reserved.
// Licensed under the MIT license. See LICENSE.txt file in the project root for full license information.
//-------------------------------------------------------------------------------------------------------
//
// N1.3 references probes (plan task N1.3, semantics section 3.8 P3-*).
//
// Direct C++ probe: creates a Chakra runtime + context, a refs-only env
// (nodeapi_refs_test_create_env), and exercises the five napi_*
// reference functions in lib/NodeApi/NodeApiRefs.cpp in-process (no
// addon, no ch-nodeapi host). Run via run_refs_probe.sh, which builds
// with `zig c++` through /tmp/opencode/cc-gate-lock.sh and links the
// engine libChakraCore.dylib produced by `mise exec -- zig build`.
//
// GC methodology (R-REF-12 / AMB-REF-2): JsCollectGarbage is a
// synchronous full GC (ChakraCommon.h:822-831). Probe objects are
// created via JsCreate* with no explicit JsAddRef except through the
// napi_ref under test, so after the local is dropped + GC, survival
// can only come from the strong root. Saved pointers are kept as
// uintptr_t (+ overwritten JsValueRef locals) so no native stack
// slot can act as an unintended root; JsStrictEquals is only called
// when the referent is known live (strong case).
//
// Output: one line per assertion ("PASS <case>" / "FAIL <case> ..."),
// then "REFS_PROBE: PASS (n/n)" exit 0, or "REFS_PROBE: FAIL (k/n)"
// exit 1.

#include "NodeApiRefs.h"
#include "NodeApiScopes.h" /* unified total + open/close handle scope (HIGH-4) */

#include "ChakraCore.h" /* JsCreateString, JsCreateFunction, JsRun (BigInt probe uses JsRun) */

#include <cstdint>
#include <cstdio>
#include <cstdlib>

namespace {

int g_total = 0;
int g_pass = 0;

const char* status_name(napi_status s) {
  switch (s) {
    case napi_ok: return "ok";
    case napi_invalid_arg: return "invalid_arg";
    case napi_object_expected: return "object_expected";
    case napi_string_expected: return "string_expected";
    case napi_name_expected: return "name_expected";
    case napi_function_expected: return "function_expected";
    case napi_number_expected: return "number_expected";
    case napi_boolean_expected: return "boolean_expected";
    case napi_array_expected: return "array_expected";
    case napi_generic_failure: return "generic_failure";
    case napi_pending_exception: return "pending_exception";
    case napi_cancelled: return "cancelled";
    case napi_escape_called_twice: return "escape_called_twice";
    case napi_handle_scope_mismatch: return "handle_scope_mismatch";
    case napi_callback_scope_mismatch: return "callback_scope_mismatch";
    case napi_queue_full: return "queue_full";
    case napi_closing: return "closing";
    case napi_bigint_expected: return "bigint_expected";
    case napi_date_expected: return "date_expected";
    case napi_arraybuffer_expected: return "arraybuffer_expected";
    case napi_detachable_arraybuffer_expected: return "detachable_arraybuffer_expected";
    case napi_would_deadlock: return "would_deadlock";
    case napi_no_external_buffers_allowed: return "no_external_buffers_allowed";
    case napi_cannot_run_js: return "cannot_run_js";
    default: return "?";
  }
}

void expect(bool cond, const char* name) {
  g_total++;
  if (cond) {
    g_pass++;
    std::fprintf(stdout, "PASS %s\n", name);
  } else {
    std::fprintf(stdout, "FAIL %s\n", name);
  }
  std::fflush(stdout);
}

void expect_status(napi_status got, napi_status want, const char* name) {
  g_total++;
  if (got == want) {
    g_pass++;
    std::fprintf(stdout, "PASS %s (status=%s)\n", name, status_name(got));
  } else {
    std::fprintf(stdout, "FAIL %s (got=%s want=%s)\n", name, status_name(got),
                 status_name(want));
  }
  std::fflush(stdout);
}

JsValueRef make_object_assert() {
  JsValueRef v = JS_INVALID_REFERENCE;
  if (JsCreateObject(&v) != JsNoError || v == JS_INVALID_REFERENCE) {
    std::fprintf(stderr, "FATAL: JsCreateObject failed\n");
    std::exit(2);
  }
  return v;
}

void collect(JsRuntimeHandle rt) {
  if (JsCollectGarbage(rt) != JsNoError) {
    std::fprintf(stderr, "FATAL: JsCollectGarbage failed\n");
    std::exit(2);
  }
}

/* HIGH-4 scoped-get helpers. Every get that returns a live value roots
 * into the innermost scope (R-SC-1). Tests open one scope per get
 * (or per group), assert the R-REF-14 total *inside* the scope (+1),
 * then close (releasing the slot) and re-assert. Naive repeated gets
 * without close would accumulate in the default scope until destroy
 * (unbounded); the probe always bounds them with close. */
__attribute__((noinline)) void ClearStack() {
  constexpr int kN = 256;
  volatile void* dummy[kN];
  volatile unsigned char* bp =
      reinterpret_cast<volatile unsigned char*>(dummy);
  while (bp < reinterpret_cast<volatile unsigned char*>(dummy) +
             sizeof(dummy)) {
    *bp++ = 0;
  }
}

void force_gc(JsRuntimeHandle rt) {
  for (int i = 0; i < 10; i++) collect(rt);
}

/* HIGH-4 probe (task 4a): get in inner scope -> close inner scope ->
 * value collected (root released) while the weak ref stays alive.
 * optnone + ClearStack per the scopes probe (conservative scan). */
__attribute__((noinline, optnone)) void Probe_ScopeCloseCollect(
    napi_env env, JsRuntimeHandle rt, size_t baseline) {
  std::fprintf(stdout, "-- case scope-close-collects --\n");
  JsValueRef obj = make_object_assert();
  napi_ref ref = nullptr;
  expect_status(napi_create_reference(env, reinterpret_cast<napi_value>(obj), 0, &ref),
                napi_ok, "scopcol: create weak");
  expect(chakra_nodeapi_test_root_count(env) == baseline + 1,
         "scopcol: weak handle counted");
  napi_handle_scope inner = nullptr;
  expect_status(napi_open_handle_scope(env, &inner), napi_ok,
                "scopcol: open inner");
  napi_value got = nullptr;
  expect_status(napi_get_reference_value(env, ref, &got), napi_ok,
                "scopcol: get in inner scope");
  expect(got != nullptr, "scopcol: live get non-null");
  expect(chakra_nodeapi_test_root_count(env) == baseline + 2,
         "scopcol: inside inner (handle + scope root)");
  expect_status(napi_close_handle_scope(env, inner), napi_ok,
                "scopcol: close inner releases root");
  expect(chakra_nodeapi_test_root_count(env) == baseline + 1,
         "scopcol: after close (handle only)");
  /* The weak referent's only root was the scope root (now released).
   * Drop every native slot holding it, then GC: it must collect while
   * the weak ref itself stays alive. */
  got = nullptr;
  obj = JS_INVALID_REFERENCE;
  ClearStack();
  force_gc(rt);
  napi_handle_scope check = nullptr;
  expect_status(napi_open_handle_scope(env, &check), napi_ok,
                "scopcol: open check scope");
  napi_value sentinel =
      reinterpret_cast<napi_value>(static_cast<std::uintptr_t>(0xdeadbeef));
  napi_value got2 = sentinel;
  expect_status(napi_get_reference_value(env, ref, &got2), napi_ok,
                "scopcol: get after close+GC");
  expect(got2 == nullptr,
         "scopcol: collected while weak ref still alive");
  expect(chakra_nodeapi_test_root_count(env) == baseline + 1,
         "scopcol: null get adds no scope root");
  expect_status(napi_close_handle_scope(env, check), napi_ok,
                "scopcol: close check scope");
  expect_status(napi_delete_reference(env, ref), napi_ok,
                "scopcol: delete still works (ref was alive)");
  expect(chakra_nodeapi_test_root_count(env) == baseline,
         "scopcol: counter at baseline");
}

/* HIGH-4 probe (task 4b): nested scopes with ref-get roots at each
 * level. Strong nesting proves per-level counter release; weak nesting
 * proves per-level collection (inner's object collects after inner
 * close while outer's object stays alive via outer's root). */
__attribute__((noinline, optnone)) void Probe_NestedScopeGets(
    napi_env env, JsRuntimeHandle rt, size_t baseline) {
  std::fprintf(stdout, "-- case nested-scope-gets --\n");
  /* ---- Strong nesting: counter per level ---- */
  {
    JsValueRef o1 = make_object_assert();
    JsValueRef o2 = make_object_assert();
    napi_ref r1 = nullptr;
    napi_ref r2 = nullptr;
    expect_status(napi_create_reference(env, reinterpret_cast<napi_value>(o1), 1, &r1),
                  napi_ok, "nested: create strong r1");
    expect_status(napi_create_reference(env, reinterpret_cast<napi_value>(o2), 1, &r2),
                  napi_ok, "nested: create strong r2");
    expect(chakra_nodeapi_test_root_count(env) == baseline + 2,
           "nested: 2 strong roots");
    napi_handle_scope outer = nullptr;
    expect_status(napi_open_handle_scope(env, &outer), napi_ok,
                  "nested: open outer");
    napi_value g1 = nullptr;
    expect_status(napi_get_reference_value(env, r1, &g1), napi_ok,
                  "nested: get r1 in outer");
    expect(g1 != nullptr, "nested: outer get non-null");
    expect(chakra_nodeapi_test_root_count(env) == baseline + 3,
           "nested: outer holds one root (2 refs + 1 outer)");
    napi_handle_scope inner = nullptr;
    expect_status(napi_open_handle_scope(env, &inner), napi_ok,
                  "nested: open inner");
    napi_value g2 = nullptr;
    expect_status(napi_get_reference_value(env, r2, &g2), napi_ok,
                  "nested: get r2 in inner");
    expect(g2 != nullptr, "nested: inner get non-null");
    expect(chakra_nodeapi_test_root_count(env) == baseline + 4,
           "nested: inner holds one root (2 refs + outer + inner)");
    expect_status(napi_close_handle_scope(env, inner), napi_ok,
                  "nested: close inner");
    expect(chakra_nodeapi_test_root_count(env) == baseline + 3,
           "nested: inner released, outer intact");
    expect_status(napi_close_handle_scope(env, outer), napi_ok,
                  "nested: close outer");
    expect(chakra_nodeapi_test_root_count(env) == baseline + 2,
           "nested: outer released, refs only");
    o1 = JS_INVALID_REFERENCE;
    o2 = JS_INVALID_REFERENCE;
    expect_status(napi_delete_reference(env, r1), napi_ok, "nested: delete r1");
    expect_status(napi_delete_reference(env, r2), napi_ok, "nested: delete r2");
    expect(chakra_nodeapi_test_root_count(env) == baseline,
           "nested: strong part at baseline");
  }
  /* ---- Weak nesting: collection per level ---- */
  {
    JsValueRef a = make_object_assert();
    JsValueRef b = make_object_assert();
    std::uintptr_t savedA = reinterpret_cast<std::uintptr_t>(a);
    std::uintptr_t savedB = reinterpret_cast<std::uintptr_t>(b);
    napi_ref wa = nullptr;
    napi_ref wb = nullptr;
    expect_status(napi_create_reference(env, reinterpret_cast<napi_value>(a), 0, &wa),
                  napi_ok, "nested-weak: create wa");
    expect_status(napi_create_reference(env, reinterpret_cast<napi_value>(b), 0, &wb),
                  napi_ok, "nested-weak: create wb");
    expect(chakra_nodeapi_test_root_count(env) == baseline + 2,
           "nested-weak: 2 weak handles");
    napi_handle_scope outer = nullptr;
    expect_status(napi_open_handle_scope(env, &outer), napi_ok,
                  "nested-weak: open outer");
    napi_value gotA = nullptr;
    expect_status(napi_get_reference_value(env, wa, &gotA), napi_ok,
                  "nested-weak: get wa in outer");
    expect(gotA != nullptr &&
               reinterpret_cast<std::uintptr_t>(gotA) == savedA,
           "nested-weak: outer get is a");
    expect(chakra_nodeapi_test_root_count(env) == baseline + 3,
           "nested-weak: outer holds a's root");
    napi_handle_scope inner = nullptr;
    expect_status(napi_open_handle_scope(env, &inner), napi_ok,
                  "nested-weak: open inner");
    napi_value gotB = nullptr;
    expect_status(napi_get_reference_value(env, wb, &gotB), napi_ok,
                  "nested-weak: get wb in inner");
    expect(gotB != nullptr &&
               reinterpret_cast<std::uintptr_t>(gotB) == savedB,
           "nested-weak: inner get is b");
    expect(chakra_nodeapi_test_root_count(env) == baseline + 4,
           "nested-weak: inner holds b's root");
    expect_status(napi_close_handle_scope(env, inner), napi_ok,
                  "nested-weak: close inner");
    expect(chakra_nodeapi_test_root_count(env) == baseline + 3,
           "nested-weak: inner released, outer holds a");
    /* b's only root was inner's (released); a survives via outer's root.
     * Drop EVERY native slot holding either referent before GC (JsValueRef
     * locals + saved uintptr_t bit patterns are all conservative-scan
     * roots): afterwards outer's root is the sole holder for a, nothing
     * holds b. Pointer-equality for a is therefore checked by non-null
     * below (saved copies would keep a alive and mask a missing root). */
    gotB = nullptr;
    b = JS_INVALID_REFERENCE;
    savedB = 0;
    gotA = nullptr;
    a = JS_INVALID_REFERENCE;
    savedA = 0;
    ClearStack();
    force_gc(rt);
    napi_handle_scope t1 = nullptr;
    expect_status(napi_open_handle_scope(env, &t1), napi_ok,
                  "nested-weak: open t1 for wb check");
    napi_value wb_check =
        reinterpret_cast<napi_value>(static_cast<std::uintptr_t>(0xdeadbeef));
    expect_status(napi_get_reference_value(env, wb, &wb_check), napi_ok,
                  "nested-weak: wb get after inner close+GC");
    expect(wb_check == nullptr, "nested-weak: b collected (inner root gone)");
    expect(chakra_nodeapi_test_root_count(env) == baseline + 3,
           "nested-weak: null get adds no root (2 handles + outer)");
    expect_status(napi_close_handle_scope(env, t1), napi_ok,
                  "nested-weak: close t1");
    /* a is still alive via outer's root: a temp-scope get hits it. */
    napi_handle_scope t2 = nullptr;
    expect_status(napi_open_handle_scope(env, &t2), napi_ok,
                  "nested-weak: open t2 for wa check");
    napi_value wa_check = nullptr;
    expect_status(napi_get_reference_value(env, wa, &wa_check), napi_ok,
                  "nested-weak: wa get while outer open");
    expect(wa_check != nullptr, "nested-weak: a still alive via outer root");
    expect(chakra_nodeapi_test_root_count(env) == baseline + 4,
           "nested-weak: temp root on top of outer (2 handles + outer + t2)");
    expect_status(napi_close_handle_scope(env, t2), napi_ok,
                  "nested-weak: close t2");
    expect(chakra_nodeapi_test_root_count(env) == baseline + 3,
           "nested-weak: t2 released, outer holds a");
    expect_status(napi_close_handle_scope(env, outer), napi_ok,
                  "nested-weak: close outer");
    expect(chakra_nodeapi_test_root_count(env) == baseline + 2,
           "nested-weak: handles only after outer close");
    /* Drop every native slot for a (gotA/a/savedA already nulled before
     * the first GC; wa_check from the t2 check still holds it). */
    gotA = nullptr;
    a = JS_INVALID_REFERENCE;
    wa_check = nullptr;
    ClearStack();
    force_gc(rt);
    napi_handle_scope t3 = nullptr;
    expect_status(napi_open_handle_scope(env, &t3), napi_ok,
                  "nested-weak: open t3 for final wa check");
    napi_value wa_final =
        reinterpret_cast<napi_value>(static_cast<std::uintptr_t>(0xdeadbeef));
    expect_status(napi_get_reference_value(env, wa, &wa_final), napi_ok,
                  "nested-weak: wa get after outer close+GC");
    expect(wa_final == nullptr, "nested-weak: a collected (outer root gone)");
    expect_status(napi_close_handle_scope(env, t3), napi_ok,
                  "nested-weak: close t3");
    expect_status(napi_delete_reference(env, wa), napi_ok, "nested-weak: delete wa");
    expect_status(napi_delete_reference(env, wb), napi_ok, "nested-weak: delete wb");
    expect(chakra_nodeapi_test_root_count(env) == baseline,
           "nested-weak: counter at baseline");
  }
  (void)rt;
}

} /* namespace */

int main() {
  JsRuntimeHandle rt = JS_INVALID_REFERENCE;
  if (JsCreateRuntime(JsRuntimeAttributeNone, nullptr, &rt) != JsNoError) {
    std::fprintf(stderr, "FATAL: JsCreateRuntime failed\n");
    return 2;
  }
  JsContextRef ctx = JS_INVALID_REFERENCE;
  if (JsCreateContext(rt, &ctx) != JsNoError) {
    std::fprintf(stderr, "FATAL: JsCreateContext failed\n");
    JsDisposeRuntime(rt);
    return 2;
  }
  if (JsSetCurrentContext(ctx) != JsNoError) {
    std::fprintf(stderr, "FATAL: JsSetCurrentContext failed\n");
    JsDisposeRuntime(rt);
    return 2;
  }

  napi_env env = nodeapi_refs_test_create_env(rt, ctx);
  if (!env) {
    std::fprintf(stderr, "FATAL: nodeapi_refs_test_create_env failed\n");
    JsSetCurrentContext(JS_INVALID_REFERENCE);
    JsDisposeRuntime(rt);
    return 2;
  }
  const size_t baseline = chakra_nodeapi_test_root_count(env);
  expect(baseline == 0, "baseline root count is zero");
  expect(chakra_nodeapi_test_root_baseline(env) == 0,
         "baseline helper is zero");

  /* ---- 1. Strong ref survives GC of local (task probe; P3-3) ---- */
  {
    std::fprintf(stdout, "-- case strong-survives-gc --\n");
    JsValueRef obj = make_object_assert();
    std::uintptr_t saved = reinterpret_cast<std::uintptr_t>(obj);
    napi_ref ref = nullptr;
    expect_status(napi_create_reference(env, reinterpret_cast<napi_value>(obj), 1, &ref),
                  napi_ok, "strong: create");
    expect(ref != nullptr, "strong: handle non-null");
    expect(chakra_nodeapi_test_root_count(env) == baseline + 1,
           "strong: root count +1");
    obj = JS_INVALID_REFERENCE; /* drop the only caller's reference */
    collect(rt);
    napi_value got =
        reinterpret_cast<napi_value>(static_cast<std::uintptr_t>(-1));
    /* HIGH-4: get roots into the innermost N1.2 scope. Open one scope
     * for the get, assert the total inside it (+2: ref + scope root),
     * then close (releasing the scope root, keeping the ref root). */
    napi_handle_scope get_scope = nullptr;
    expect_status(napi_open_handle_scope(env, &get_scope), napi_ok,
                  "strong: open scope for get");
    expect_status(napi_get_reference_value(env, ref, &got), napi_ok,
                  "strong: get after GC");
    expect(got != nullptr, "strong: get non-null after GC");
    expect(reinterpret_cast<std::uintptr_t>(got) == saved,
           "strong: get is the same object");
    expect(chakra_nodeapi_test_root_count(env) == baseline + 2,
           "strong: get adds a scope root");
    expect_status(napi_close_handle_scope(env, get_scope), napi_ok,
                  "strong: close scope releases root");
    expect(chakra_nodeapi_test_root_count(env) == baseline + 1,
           "strong: close keeps only the ref root");
    expect_status(napi_delete_reference(env, ref), napi_ok,
                  "strong: delete");
    expect(chakra_nodeapi_test_root_count(env) == baseline,
           "strong: counter back to baseline after delete");
    /* Repeated GC churn (N5.4 >=10x): a fresh strong ref per round. */
    bool ok_churn = true;
    for (int i = 0; i < 10; i++) {
      JsValueRef o = make_object_assert();
      napi_ref r = nullptr;
      if (napi_create_reference(env, reinterpret_cast<napi_value>(o), 1, &r) != napi_ok) ok_churn = false;
      o = JS_INVALID_REFERENCE;
      collect(rt);
      /* Bound the loop: open/get/close per iteration (naive growth
       * without close would accumulate in the default scope). */
      napi_handle_scope churn_scope = nullptr;
      if (napi_open_handle_scope(env, &churn_scope) != napi_ok) ok_churn = false;
      napi_value gv = nullptr;
      if (napi_get_reference_value(env, r, &gv) != napi_ok || gv == nullptr) ok_churn = false;
      if (napi_close_handle_scope(env, churn_scope) != napi_ok) ok_churn = false;
      if (napi_delete_reference(env, r) != napi_ok) ok_churn = false;
    }
    expect(ok_churn, "strong: 10x GC churn survives");
    expect(chakra_nodeapi_test_root_count(env) == baseline,
           "strong: churn ends at baseline");
  }

  /* ---- 2. Unref-to-0, collect, get => ok+null; ref-on-collected ---- */
  {
    std::fprintf(stdout, "-- case weak-collects --\n");
    JsValueRef obj = make_object_assert();
    napi_ref ref = nullptr;
    expect_status(napi_create_reference(env, reinterpret_cast<napi_value>(obj), 1, &ref),
                  napi_ok, "weak: create (strong)");
    obj = JS_INVALID_REFERENCE;
    uint32_t rc = 12345;
    expect_status(napi_reference_unref(env, ref, &rc), napi_ok, "weak: unref to 0");
    expect(rc == 0, "weak: unref reports 0");
    expect(chakra_nodeapi_test_root_count(env) == baseline + 1,
           "weak: handle root still counted at 0");
    collect(rt);
    napi_value sentinel = reinterpret_cast<napi_value>(static_cast<std::uintptr_t>(0xdeadbeef));
    napi_value got = sentinel;
    expect_status(napi_get_reference_value(env, ref, &got), napi_ok,
                  "weak: get on collected");
    expect(got == nullptr, "weak: collected get is ok+null (sentinel overwritten)");
    uint32_t rc2 = 999;
    expect_status(napi_reference_ref(env, ref, &rc2), napi_invalid_arg,
                  "weak: ref on collected is invalid_arg");
    expect_status(napi_delete_reference(env, ref), napi_ok, "weak: delete");
    expect(chakra_nodeapi_test_root_count(env) == baseline,
           "weak: counter back to baseline after delete");
  }

  /* ---- 3. Weak from birth collects (P3-2 equivalent) ---- */
  {
    std::fprintf(stdout, "-- case weak-from-birth --\n");
    JsValueRef obj = make_object_assert();
    napi_ref ref = nullptr;
    expect_status(napi_create_reference(env, reinterpret_cast<napi_value>(obj), 0, &ref),
                  napi_ok, "weak0: create (weak)");
    expect(chakra_nodeapi_test_root_count(env) == baseline + 1,
           "weak0: handle root counted");
    /* Referent still live: get returns it (and scope-roots it). */
    napi_value got = nullptr;
    napi_handle_scope live_scope = nullptr;
    expect_status(napi_open_handle_scope(env, &live_scope), napi_ok,
                  "weak0: open scope for live get");
    expect_status(napi_get_reference_value(env, ref, &got), napi_ok,
                  "weak0: get while live");
    expect(got != nullptr, "weak0: live get non-null");
    expect_status(napi_close_handle_scope(env, live_scope), napi_ok,
                  "weak0: close releases scope root");
    /* Drop every native JsValueRef slot holding the referent before GC:
     * a native local holding the raw pointer must not act as an
     * unintended root (header methodology). The scope root is already
     * released above, so the weak referent is now collectable. */
    got = nullptr;
    obj = JS_INVALID_REFERENCE;
    collect(rt);
    napi_value got2 = reinterpret_cast<napi_value>(static_cast<std::uintptr_t>(0xdeadbeef));
    expect_status(napi_get_reference_value(env, ref, &got2), napi_ok,
                  "weak0: get after collect");
    expect(got2 == nullptr, "weak0: collected get is ok+null");
    expect_status(napi_delete_reference(env, ref), napi_ok, "weak0: delete");
    expect(chakra_nodeapi_test_root_count(env) == baseline,
           "weak0: counter back to baseline after delete");
  }

  /* ---- 4. Double-delete => invalid_arg (P3-5) ---- */
  {
    std::fprintf(stdout, "-- case double-delete --\n");
    JsValueRef obj = make_object_assert();
    napi_ref ref = nullptr;
    expect_status(napi_create_reference(env, reinterpret_cast<napi_value>(obj), 1, &ref),
                  napi_ok, "dbl: create");
    obj = JS_INVALID_REFERENCE;
    collect(rt);
    expect_status(napi_delete_reference(env, ref), napi_ok, "dbl: first delete ok");
    expect_status(napi_delete_reference(env, ref), napi_invalid_arg,
                  "dbl: second delete invalid_arg");
    expect(chakra_nodeapi_test_root_count(env) == baseline,
           "dbl: counter at baseline");
  }

  /* ---- 5. Use-after-delete => invalid_arg each (P3-4) ---- */
  {
    std::fprintf(stdout, "-- case use-after-delete --\n");
    JsValueRef obj = make_object_assert();
    napi_ref ref = nullptr;
    expect_status(napi_create_reference(env, reinterpret_cast<napi_value>(obj), 1, &ref),
                  napi_ok, "uad: create");
    obj = JS_INVALID_REFERENCE;
    /* Keep the referent alive behind the strong ref so the delete
     * releases a live root (not a collected handle). */
    collect(rt);
    expect_status(napi_delete_reference(env, ref), napi_ok, "uad: delete ok");
    napi_value got = reinterpret_cast<napi_value>(static_cast<std::uintptr_t>(0xdeadbeef));
    expect_status(napi_get_reference_value(env, ref, &got), napi_invalid_arg,
                  "uad: get invalid_arg");
    expect(got == reinterpret_cast<napi_value>(static_cast<std::uintptr_t>(0xdeadbeef)),
           "uad: get leaves out-param untouched");
    uint32_t rc = 77;
    expect_status(napi_reference_ref(env, ref, &rc), napi_invalid_arg,
                  "uad: ref invalid_arg");
    expect_status(napi_reference_unref(env, ref, &rc), napi_invalid_arg,
                  "uad: unref invalid_arg");
    expect(chakra_nodeapi_test_root_count(env) == baseline,
           "uad: counter at baseline");
  }

  /* ---- 6. Unref at 0 => invalid_arg (header docs; AMB-REF-1) ---- */
  {
    std::fprintf(stdout, "-- case unref-at-0 --\n");
    JsValueRef obj = make_object_assert();
    napi_ref ref = nullptr;
    expect_status(napi_create_reference(env, reinterpret_cast<napi_value>(obj), 0, &ref),
                  napi_ok, "u0: create weak");
    uint32_t rc = 42;
    expect_status(napi_reference_unref(env, ref, &rc), napi_invalid_arg,
                  "u0: unref at 0 invalid_arg");
    /* Still weak and still live: get returns the object. */
    napi_value got = nullptr;
    napi_handle_scope u0_scope = nullptr;
    expect_status(napi_open_handle_scope(env, &u0_scope), napi_ok,
                  "u0: open scope for get");
    expect_status(napi_get_reference_value(env, ref, &got), napi_ok,
                  "u0: get still works after refused unref");
    expect(got != nullptr, "u0: get non-null");
    expect_status(napi_close_handle_scope(env, u0_scope), napi_ok,
                  "u0: close scope");
    obj = JS_INVALID_REFERENCE;
    expect_status(napi_delete_reference(env, ref), napi_ok, "u0: delete");
    expect(chakra_nodeapi_test_root_count(env) == baseline,
           "u0: counter at baseline");
  }

  /* ---- 7. v8 type restriction (P3-7) ---- */
  {
    std::fprintf(stdout, "-- case v8-type-restriction --\n");
    /* Disallowed: undefined/null/number/string/boolean -> invalid_arg. */
    {
      JsValueRef marker = make_object_assert();
      JsValueRef vals[5];
      vals[0] = JS_INVALID_REFERENCE; /* filled below */
      JsGetUndefinedValue(&vals[0]);
      JsGetNullValue(&vals[1]);
      JsDoubleToNumber(42.0, &vals[2]);
      JsValueRef sv = JS_INVALID_REFERENCE;
      JsCreateString("s", 1, &sv);
      vals[3] = sv;
      JsBoolToBoolean(true, &vals[4]);
      const char* kinds[5] = {"undefined", "null", "number", "string", "boolean"};
      for (int i = 0; i < 5; i++) {
        napi_ref r = reinterpret_cast<napi_ref>(static_cast<std::uintptr_t>(0xbeef));
        napi_status st = napi_create_reference(env, reinterpret_cast<napi_value>(vals[i]), 1, &r);
        char name[64];
        std::snprintf(name, sizeof(name), "v8: reject %s", kinds[i]);
        expect_status(st, napi_invalid_arg, name);
        char name2[64];
        std::snprintf(name2, sizeof(name2), "v8: reject %s leaves out-param untouched", kinds[i]);
        expect(r == reinterpret_cast<napi_ref>(static_cast<std::uintptr_t>(0xbeef)), name2);
      }
      expect(chakra_nodeapi_test_root_count(env) == baseline,
             "v8: rejected creates leave counter at baseline");
      (void)marker;
      marker = JS_INVALID_REFERENCE;
      collect(rt);
    }
    /* Allowed: object/function/symbol/external -> ok. */
    {
      JsValueRef o = make_object_assert();
      napi_ref r1 = nullptr;
      expect_status(napi_create_reference(env, reinterpret_cast<napi_value>(o), 1, &r1),
                    napi_ok, "v8: allow object");
      napi_value got = nullptr;
      napi_handle_scope o_scope = nullptr;
      expect_status(napi_open_handle_scope(env, &o_scope), napi_ok,
                    "v8: object open");
      expect_status(napi_get_reference_value(env, r1, &got), napi_ok,
                    "v8: object get");
      expect(got != nullptr, "v8: object get non-null");
      expect_status(napi_close_handle_scope(env, o_scope), napi_ok,
                    "v8: object close");
      expect_status(napi_delete_reference(env, r1), napi_ok, "v8: object delete");
      o = JS_INVALID_REFERENCE;
      collect(rt);
    }
    {
      JsValueRef f = JS_INVALID_REFERENCE;
      auto nil_cb = [](JsValueRef, bool, JsValueRef*, unsigned short, void*) -> JsValueRef {
        JsValueRef u = JS_INVALID_REFERENCE;
        JsGetUndefinedValue(&u);
        return u;
      };
      if (JsCreateFunction(nil_cb, nullptr, &f) != JsNoError || f == JS_INVALID_REFERENCE) {
        expect(false, "v8: make function (setup)");
      } else {
        napi_ref r = nullptr;
        expect_status(napi_create_reference(env, reinterpret_cast<napi_value>(f), 1, &r),
                      napi_ok, "v8: allow function");
        napi_value got = nullptr;
        napi_handle_scope f_scope = nullptr;
        expect_status(napi_open_handle_scope(env, &f_scope), napi_ok,
                      "v8: function open");
        expect_status(napi_get_reference_value(env, r, &got), napi_ok, "v8: function get");
        expect(got != nullptr, "v8: function get non-null");
        expect_status(napi_close_handle_scope(env, f_scope), napi_ok,
                      "v8: function close");
        expect_status(napi_delete_reference(env, r), napi_ok, "v8: function delete");
        f = JS_INVALID_REFERENCE;
      }
      collect(rt);
    }
    {
      JsValueRef sym = JS_INVALID_REFERENCE;
      JsValueRef desc = JS_INVALID_REFERENCE;
      JsCreateString("sym", 3, &desc);
      if (JsCreateSymbol(desc, &sym) != JsNoError || sym == JS_INVALID_REFERENCE) {
        /* Symbol shape may differ across engine builds: report as
         * failure rather than skipping silently (plan: skips carry
         * reasons; this probe has no skip path). */
        expect(false, "v8: make symbol (setup)");
      } else {
        napi_ref r = nullptr;
        expect_status(napi_create_reference(env, reinterpret_cast<napi_value>(sym), 1, &r),
                      napi_ok, "v8: allow symbol (strong)");
        napi_value got = nullptr;
        napi_handle_scope sym_scope = nullptr;
        expect_status(napi_open_handle_scope(env, &sym_scope), napi_ok,
                      "v8: symbol open");
        expect_status(napi_get_reference_value(env, r, &got), napi_ok, "v8: symbol get");
        expect(got != nullptr, "v8: symbol get non-null");
        expect_status(napi_close_handle_scope(env, sym_scope), napi_ok,
                      "v8: symbol close");
        /* Weak symbol: covers the JsNoWeakRefRequired fallback arm. */
        uint32_t rc = 0;
        expect_status(napi_reference_unref(env, r, &rc), napi_ok, "v8: symbol unref to weak");
        expect(rc == 0, "v8: symbol unref reports 0");
        napi_value got2 = nullptr;
        napi_handle_scope sym2_scope = nullptr;
        expect_status(napi_open_handle_scope(env, &sym2_scope), napi_ok,
                      "v8: weak symbol open");
        expect_status(napi_get_reference_value(env, r, &got2), napi_ok, "v8: weak symbol get");
        /* A symbol may be immortal (never collected) or weak-collected;
         * either way the call succeeds. Liveness itself is not
         * asserted here; GC survival is covered by the object arms. */
        expect_status(napi_close_handle_scope(env, sym2_scope), napi_ok,
                      "v8: weak symbol close");
        expect_status(napi_delete_reference(env, r), napi_ok, "v8: symbol delete");
        sym = JS_INVALID_REFERENCE;
      }
      collect(rt);
    }
    {
      JsValueRef ext = JS_INVALID_REFERENCE;
      if (JsCreateExternalObject(nullptr, nullptr, &ext) != JsNoError ||
          ext == JS_INVALID_REFERENCE) {
        expect(false, "v8: make external (setup)");
      } else {
        napi_ref r = nullptr;
        expect_status(napi_create_reference(env, reinterpret_cast<napi_value>(ext), 1, &r),
                      napi_ok, "v8: allow external");
        napi_value got = nullptr;
        napi_handle_scope ext_scope = nullptr;
        expect_status(napi_open_handle_scope(env, &ext_scope), napi_ok,
                      "v8: external open");
        expect_status(napi_get_reference_value(env, r, &got), napi_ok, "v8: external get");
        expect(got != nullptr, "v8: external get non-null");
        expect_status(napi_close_handle_scope(env, ext_scope), napi_ok,
                      "v8: external close");
        expect_status(napi_delete_reference(env, r), napi_ok, "v8: external delete");
        ext = JS_INVALID_REFERENCE;
      }
      collect(rt);
    }
    expect(chakra_nodeapi_test_root_count(env) == baseline,
           "v8: allowed arms end at baseline");
  }

  /* ---- 8. ref/unref cycles (P3-1 light: 100 rounds) ---- */
  {
    std::fprintf(stdout, "-- case ref-unref-cycles --\n");
    JsValueRef obj = make_object_assert();
    napi_ref ref = nullptr;
    expect_status(napi_create_reference(env, reinterpret_cast<napi_value>(obj), 1, &ref),
                  napi_ok, "cyc: create");
    bool ok = true;
    for (int i = 0; i < 100; i++) {
      uint32_t rc = 0;
      /* obj is still held by the caller, so weak -> strong always has
       * a live referent (no collection mid-cycle). */
      if (napi_reference_ref(env, ref, &rc) != napi_ok || rc != 2) ok = false;
      if (napi_reference_unref(env, ref, &rc) != napi_ok || rc != 1) ok = false;
      if (napi_reference_unref(env, ref, &rc) != napi_ok || rc != 0) ok = false;
      if (napi_reference_ref(env, ref, &rc) != napi_ok || rc != 1) ok = false;
      if (chakra_nodeapi_test_root_count(env) != baseline + 1) ok = false;
    }
    expect(ok, "cyc: 100x ref/unref cycles keep root count +1");
    napi_value got = nullptr;
    napi_handle_scope cyc_scope = nullptr;
    expect_status(napi_open_handle_scope(env, &cyc_scope), napi_ok,
                  "cyc: open scope for get");
    expect_status(napi_get_reference_value(env, ref, &got), napi_ok, "cyc: get after cycles");
    expect(got != nullptr, "cyc: get non-null after cycles");
    expect_status(napi_close_handle_scope(env, cyc_scope), napi_ok,
                  "cyc: close scope");
    obj = JS_INVALID_REFERENCE;
    expect_status(napi_delete_reference(env, ref), napi_ok, "cyc: delete");
    expect(chakra_nodeapi_test_root_count(env) == baseline,
           "cyc: counter at baseline");
  }

  /* ---- 9. Pending-exception discipline (P5-3 subset, section 6.4) ---- */
  {
    std::fprintf(stdout, "-- case pending-discipline --\n");
    JsValueRef obj = make_object_assert();
    napi_ref target = nullptr;
    expect_status(napi_create_reference(env, reinterpret_cast<napi_value>(obj), 1, &target),
                  napi_ok, "pend: create target");
    /* Create the fresh object BEFORE setting the pending exception
     * (JsCreateObject while pending is not guaranteed to succeed).
     * The gated create call below never reaches the type check, so
     * the pending status is what is observed. */
    JsValueRef fresh = make_object_assert();
    /* Set a pending exception. JsSetException consumes the value as
     * the pending exception; no extra root is taken here. */
    JsValueRef exc = make_object_assert();
    if (JsSetException(exc) != JsNoError) {
      expect(false, "pend: set exception (setup)");
    }
    exc = JS_INVALID_REFERENCE;
    obj = JS_INVALID_REFERENCE;
    /* Gated functions: pending_exception, no state change. */
    napi_ref scratch = reinterpret_cast<napi_ref>(static_cast<std::uintptr_t>(0xbeef));
    expect_status(napi_create_reference(env, reinterpret_cast<napi_value>(fresh), 1, &scratch),
                  napi_pending_exception, "pend: create gated");
    napi_value got = reinterpret_cast<napi_value>(static_cast<std::uintptr_t>(0xdeadbeef));
    expect_status(napi_get_reference_value(env, target, &got), napi_pending_exception,
                  "pend: get gated");
    expect(got == reinterpret_cast<napi_value>(static_cast<std::uintptr_t>(0xdeadbeef)),
           "pend: gated get leaves out-param untouched");
    uint32_t rc = 5;
    expect_status(napi_reference_ref(env, target, &rc), napi_pending_exception, "pend: ref gated");
    expect_status(napi_reference_unref(env, target, &rc), napi_pending_exception,
                  "pend: unref gated");
    expect(chakra_nodeapi_test_root_count(env) == baseline + 1,
           "pend: gated calls create no root");
    /* Delete is allowlisted: executes with the exception pending. */
    expect_status(napi_delete_reference(env, target), napi_ok,
                  "pend: delete executes (allowlist)");
    /* Clear the exception; gated calls resume. */
    JsValueRef cleared = JS_INVALID_REFERENCE;
    bool had = false;
    expect(JsHasException(&had) == JsNoError && had, "pend: exception still set");
    expect(JsGetAndClearException(&cleared) == JsNoError, "pend: clear exception");
    cleared = JS_INVALID_REFERENCE;
    fresh = JS_INVALID_REFERENCE;
    collect(rt);
    expect(chakra_nodeapi_test_root_count(env) == baseline,
           "pend: counter at baseline after delete+clear");
  }

  /* ---- 10. Null-argument validation (plan 3.10 validate step) ---- */
  {
    std::fprintf(stdout, "-- case null-args --\n");
    napi_ref r = reinterpret_cast<napi_ref>(static_cast<std::uintptr_t>(0xbeef));
    JsValueRef obj = make_object_assert();
    expect_status(napi_create_reference(nullptr, reinterpret_cast<napi_value>(obj), 1, &r),
                  napi_invalid_arg, "null: create null env");
    expect_status(napi_create_reference(env, nullptr, 1, &r), napi_invalid_arg,
                  "null: create null value");
    expect_status(napi_create_reference(env, reinterpret_cast<napi_value>(obj), 1, nullptr),
                  napi_invalid_arg, "null: create null result");
    expect(r == reinterpret_cast<napi_ref>(static_cast<std::uintptr_t>(0xbeef)),
           "null: failed creates leave out-param untouched");
    expect_status(napi_delete_reference(nullptr, nullptr), napi_invalid_arg,
                  "null: delete null args");
    napi_value got = nullptr;
    expect_status(napi_get_reference_value(env, nullptr, &got), napi_invalid_arg,
                  "null: get null ref");
    uint32_t rc = 0;
    expect_status(napi_reference_ref(env, nullptr, &rc), napi_invalid_arg,
                  "null: ref null ref");
    expect_status(napi_reference_unref(env, nullptr, &rc), napi_invalid_arg,
                  "null: unref null ref");
    /* NULL result out-param for ref/unref is legal (header: optional). */
    napi_ref live = nullptr;
    expect_status(napi_create_reference(env, reinterpret_cast<napi_value>(obj), 1, &live),
                  napi_ok, "null: create live (setup)");
    expect_status(napi_reference_ref(env, live, nullptr), napi_ok,
                  "null: ref with null result is ok");
    expect_status(napi_reference_unref(env, live, nullptr), napi_ok,
                  "null: unref with null result is ok");
    expect_status(napi_delete_reference(env, live), napi_ok, "null: delete live");
    obj = JS_INVALID_REFERENCE;
    collect(rt);
    expect(chakra_nodeapi_test_root_count(env) == baseline,
           "null: ends at baseline");
  }

  /* ---- 11. Teardown (step 6a) releases live refs incl. weak handles ---- */
  {
    std::fprintf(stdout, "-- case teardown-live-refs --\n");
    napi_env env2 = nodeapi_refs_test_create_env(rt, ctx);
    expect(env2 != nullptr, "teardown: env2 created");
    if (env2 != nullptr) {
      const size_t base2 = chakra_nodeapi_test_root_count(env2);
      expect(base2 == 0, "teardown: env2 baseline zero");
      JsValueRef o1 = make_object_assert();
      JsValueRef o2 = make_object_assert();
      napi_ref strong = nullptr;
      napi_ref weak = nullptr;
      expect_status(napi_create_reference(env2, reinterpret_cast<napi_value>(o1), 1, &strong),
                    napi_ok, "teardown: create strong");
      expect_status(napi_create_reference(env2, reinterpret_cast<napi_value>(o2), 0, &weak),
                    napi_ok, "teardown: create weak");
      /* HIGH-4: get roots into the N1.2 stack (step 6b), not the refs
       * state (step 6a). To prove step 6a alone releases ref-record
       * roots, the get's scope root is closed BEFORE teardown_all, so
       * teardown_all only needs to release the 2 ref roots. (Step 6b
       * release of a live default-scope root is covered by env3 +
       * the scope probes / destroy-with-live-refs below.) */
      napi_value got = nullptr;
      napi_handle_scope teardown_scope = nullptr;
      expect_status(napi_open_handle_scope(env2, &teardown_scope), napi_ok,
                    "teardown: open scope for get");
      expect_status(napi_get_reference_value(env2, strong, &got), napi_ok,
                    "teardown: get (scope root)");
      expect(chakra_nodeapi_test_root_count(env2) == base2 + 3,
             "teardown: counter +3 inside scope (2 refs + 1 scope root)");
      expect_status(napi_close_handle_scope(env2, teardown_scope), napi_ok,
                    "teardown: close scope");
      expect(chakra_nodeapi_test_root_count(env2) == base2 + 2,
             "teardown: counter +2 after close (2 refs only)");
      got = nullptr;
      o1 = JS_INVALID_REFERENCE;
      o2 = JS_INVALID_REFERENCE;
      nodeapi_refs_teardown_all(env2);
      expect(chakra_nodeapi_test_root_count(env2) == base2,
             "teardown_all: counter back to baseline (weak handle released)");
      /* Handles from env2 are now invalid: use is clean invalid_arg,
       * not a crash (single map lookup, no dereference). */
      napi_value tmp = reinterpret_cast<napi_value>(static_cast<std::uintptr_t>(0xdeadbeef));
      expect_status(napi_get_reference_value(env2, strong, &tmp), napi_invalid_arg,
                    "teardown_all: get on torn-down ref invalid_arg");
      expect(tmp == reinterpret_cast<napi_value>(static_cast<std::uintptr_t>(0xdeadbeef)),
             "teardown_all: out-param untouched");
      nodeapi_refs_test_destroy_env(env2);
      expect(true, "teardown: env2 destroyed after teardown_all");
    }
    /* Destroy-with-live-refs (no manual delete): the full step-6a
     * path must release strong roots + weak handles without crash.
     * env3 is destroyed here; the env value is not used after. */
    napi_env env3 = nodeapi_refs_test_create_env(rt, ctx);
    expect(env3 != nullptr, "teardown: env3 created");
    if (env3 != nullptr) {
      JsValueRef o3 = make_object_assert();
      JsValueRef o4 = make_object_assert();
      napi_ref r3 = nullptr;
      napi_ref r4 = nullptr;
      expect_status(napi_create_reference(env3, reinterpret_cast<napi_value>(o3), 1, &r3),
                    napi_ok, "teardown: env3 create strong");
      expect_status(napi_create_reference(env3, reinterpret_cast<napi_value>(o4), 0, &r4),
                    napi_ok, "teardown: env3 create weak");
      o3 = JS_INVALID_REFERENCE;
      o4 = JS_INVALID_REFERENCE;
      nodeapi_refs_test_destroy_env(env3);
      expect(true, "teardown: destroy-with-live-refs clean");
      (void)r3;
      (void)r4;
    }
    collect(rt);
  }

  /* ---- 12/13. HIGH-4: scope-close collection + nested ref-get roots ---- */
  Probe_ScopeCloseCollect(env, rt, baseline);
  Probe_NestedScopeGets(env, rt, baseline);

  nodeapi_refs_test_destroy_env(env);
  expect(true, "teardown: env destroyed");
  JsSetCurrentContext(JS_INVALID_REFERENCE);
  JsDisposeRuntime(rt);

  std::fprintf(stdout, "REFS_PROBE: %s (%d/%d)\n",
               (g_pass == g_total) ? "PASS" : "FAIL", g_pass, g_total);
  return (g_pass == g_total) ? 0 : 1;
}
