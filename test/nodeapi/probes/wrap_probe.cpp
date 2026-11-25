//-------------------------------------------------------------------------------------------------------
// Copyright (c) ChakraCore Project Contributors. All rights reserved.
// Licensed under the MIT license. See LICENSE.txt file in the project root for full license information.
//-------------------------------------------------------------------------------------------------------
//
// N3.2/N3.3 wrap/finalizer probes (plan tasks N3.2/N3.3, semantics §4
// probes P7-*). Direct C++ probe: creates a Chakra runtime + context and a
// real env (chakra_nodeapi_create_env), exercises the eight wrap napi_*
// entries in lib/NodeApi/NodeApiWrap.cpp in-process (no addon, no ch-nodeapi
// host). Run via run_wrap_probe.sh on both profiles (test + jit).
//
// Asserts (each MUST be able to fail the probe, never merely print):
//   P7-1 external round-trip (pointer identity, invalid_arg on non-external /
//     NULL out-param).
//   P7-2 finalizer exactly once after collect + drain; never during GC
//     (g_in_gc trap + thread-id + live-env checks); deferred until a later
//     public entry (entry-drain arm) or explicit host-pump drain hook.
//   P7-6 remove_wrap returns data + suppresses across GC + drain (zero
//     finalizer calls; unwrap after remove => invalid_arg; second remove
//     => invalid_arg). Also NULL result allowed.
//   P7-7 type tags: match/mismatch/double-tag invalid_arg; tag on unwrapped
//     => check false; tag-only record (plain object, no wrap) works.
//   P7-8 ABA: collect wrapped object, churn recreations, unwrap on fresh
//     objects => invalid_arg, never the stale pointer.
//   P7-9 add_finalizer exactly once + napi_ref out-param; unwrap/remove on an
//     add-finalizer object => invalid_arg (not a wrap).
//   P7-10 teardown with a live wrapped object: finalizer runs once at
//     destroy (Closing state + generic_failure for new JS work inside).
//   P7-3 collected-then-immediate-destroy (no explicit drain) => exactly once.
//   P7-11 10k create/collect cycles (100 x 100 batches): finalizer count
//     == 10000 exactly, wrap live count back to baseline (no leak).
//   Double-wrap => invalid_arg; unwrap of unwrapped => invalid_arg; wrap of
//     non-object => invalid_arg.
//   Pending discipline (status-mapping C): gated napi_wrap => pending_exception
//     without executing; executing napi_get_value_external still runs.
//   Reentrancy/chaining (P7-4/P7-5 lite): finalizer calls a public API; chained
//     finalizer (create + collect inside) runs within the same drain.
//   Drain-order characterization oracle (Phase K): one wrapped object with
//     head + two chained add_finalizer riders drains head-then-riders in
//     push order. Upstream finalizer_order.js semantics are NOT vendored
//     (test/nodeapi/run_node_addon_api_matrix.py:273-288 records it as
//     NEEDS_GC), so this pins current NodeApiWrap DrainState behavior,
//     not upstream spec; do not "fix" the order to match upstream without
//     re-vendoring that driver.
//
// GC methodology: JsCollectGarbage is a synchronous full GC
// (ChakraCommon.h). Objects are dropped (locals nulled + ClearStack) and
// their provider scope roots closed before the 10x GC-stress burn, so
// survival can only come from an intended strong root/reference. After
// each GC burn the probe asserts the deferral contract (queued count /
// zero finalizer calls) before draining.
//
// Output: one line per check, then "WRAP_PROBE: PASS (n/n)" exit 0 or
// "WRAP_PROBE: FAIL (k/n)" exit 1.

#include "NodeApiHostApi.h"
#include "NodeApiWrap.h"
#include "NodeApiEnv.h"

#include "js_native_api.h"
#include "node_api.h"
#include "ChakraCommon.h"
#include "ChakraCore.h"

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <thread>
#include <vector>

namespace {

#if defined(__clang__)
#define PROBE_NOINLINE_OPTNONE __attribute__((noinline, optnone))
#elif defined(__GNUC__)
#define PROBE_NOINLINE_OPTNONE __attribute__((noinline, optimize("O0")))
#else
#define PROBE_NOINLINE_OPTNONE
#endif

template <typename T>
inline void ClearDeadRef(T& ref, T invalid_val) {
  volatile T* slot = &ref;
  *slot = invalid_val;
#if defined(__clang__) || defined(__GNUC__)
  __asm__ volatile("" : : "r"(slot) : "memory");
#endif
}

int g_total = 0;
int g_pass = 0;

bool g_in_gc = false;          /* set around every JsCollectGarbage burn */
bool g_gc_violation = false;   /* a finalizer ran while g_in_gc */
std::thread::id g_owner_thread;
int g_finalizer_rng = 0;       /* monotonic run number for ordering */

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

const char* StatusName(napi_status s) {
  switch (s) {
    case napi_ok: return "ok";
    case napi_invalid_arg: return "invalid_arg";
    case napi_object_expected: return "object_expected";
    case napi_generic_failure: return "generic_failure";
    case napi_pending_exception: return "pending_exception";
    default: return "?";
  }
}

void expect_status(napi_status got, napi_status want, const char* name) {
  g_total++;
  if (got == want) {
    g_pass++;
    std::fprintf(stdout, "PASS %s (status=%s)\n", name, StatusName(got));
  } else {
    std::fprintf(stdout, "FAIL %s (got=%s want=%s)\n", name, StatusName(got),
                 StatusName(want));
  }
  std::fflush(stdout);
}

struct Counter {
  int calls = 0;
  int wrong_thread = 0;
  int during_gc = 0;
  int not_alive = 0;     /* runs with state != "alive" (normal drain) */
  int was_closing = 0;
  int create_status = -999; /* status of a nested create attempt */
};

void Observe(napi_env env, Counter* c) {
  c->calls++;
  if (std::this_thread::get_id() != g_owner_thread) c->wrong_thread++;
  if (g_in_gc) {
    c->during_gc++;
    g_gc_violation = true;
  }
  const char* st = chakra_nodeapi_test_state_name(env);
  if (st == nullptr || std::strcmp(st, "alive") != 0) c->not_alive++;
  if (st != nullptr && std::strcmp(st, "closing") == 0) c->was_closing++;
}

void FinGeneric(napi_env env, void* data, void* /*hint*/) {
  (void)env;
  auto* c = static_cast<Counter*>(data);
  if (c) c->calls++;
}

/* Drain-order oracle (Phase K): records finalizer ids in run order.
 * data carries the id as an integer cast (no allocation, GC-safe). */
std::vector<int> g_drain_order;
void FinOrdered(napi_env /*env*/, void* data, void* /*hint*/) {
  g_drain_order.push_back(static_cast<int>(reinterpret_cast<std::uintptr_t>(data)));
}

/* Closing-window finalizer (P7-10): records Closing state and attempts a
 * new JS-creating call, which must fail with generic_failure while Closing. */
void FinClosing(napi_env env, void* data, void* /*hint*/) {
  auto* c = static_cast<Counter*>(data);
  if (!c) return;
  c->calls++;
  const char* st = chakra_nodeapi_test_state_name(env);
  if (st != nullptr && std::strcmp(st, "closing") == 0) c->was_closing++;
  napi_value tmp = nullptr;
  napi_status s = napi_create_external(env, (void*)(std::uintptr_t)1, nullptr,
                                       nullptr, &tmp);
  c->create_status = (int)s;
}

/* Reentrant finalizer (P7-4): calls public APIs from inside the drain. */
void FinReentrant(napi_env env, void* data, void* /*hint*/) {
  auto* c = static_cast<Counter*>(data);
  if (!c) return;
  /* Observe() increments calls exactly once (thread/gc/state checks). */
  Observe(env, c);
  /* Reentrant reads must work while the drain is running (no lock held). */
  uint32_t ver = 0;
  napi_status s = napi_get_version(env, &ver);
  if (s != napi_ok || ver != 10) c->wrong_thread += 1000;
  bool tag_ok = false;
  napi_type_tag dummy{0, 0};
  /* The dummy object must be live: use the global object (no wrap). */
  napi_value global = nullptr;
  if (napi_get_global(env, &global) == napi_ok && global != nullptr) {
    (void)napi_check_object_type_tag(env, global, &dummy, &tag_ok);
  }
}

/* Chained finalizer (P7-5): creates + collects a second external from
 * inside the first drain; drain-until-empty must run it too. */
PROBE_NOINLINE_OPTNONE void ClearStack();
PROBE_NOINLINE_OPTNONE bool CreateExternalThenDrop(
    napi_env env, void* data, node_api_basic_finalize fin);

struct ChainState {
  napi_env env = nullptr;
  JsRuntimeHandle rt = JS_INVALID_REFERENCE;
  Counter first;
  Counter second;
  bool armed = false;
};

void FinChainedSecond(napi_env /*env*/, void* data, void* /*hint*/) {
  auto* c = static_cast<Counter*>(data);
  if (c) c->calls++;
}

PROBE_NOINLINE_OPTNONE void FinChainedFirst(
    napi_env env, void* data, void* /*hint*/) {
  auto* cs = static_cast<ChainState*>(data);
  if (!cs) return;
  cs->first.calls++;
  if (!cs->armed) return;
  cs->armed = false;
  /* Create a second external inside a popped optnone helper frame so no
   * dead JS pointer remains in FinChainedFirst's stack/registers across
   * ClearStack() + GC. The GC trampoline enqueues it; the running drain
   * loop (released lock) must pick it up before returning. */
  if (!CreateExternalThenDrop(env, &cs->second, FinChainedSecond)) return;
  ClearStack();
  bool prev_gc = g_in_gc;
  g_in_gc = true;
  for (int i = 0; i < 10; i++) JsCollectGarbage(cs->rt);
  g_in_gc = prev_gc;
}

JsValueRef make_object_assert() {
  JsValueRef v = JS_INVALID_REFERENCE;
  if (JsCreateObject(&v) != JsNoError || v == JS_INVALID_REFERENCE) {
    std::fprintf(stderr, "FATAL: JsCreateObject failed\n");
    std::exit(2);
  }
  return v;
}

void Collect10x(JsRuntimeHandle rt) {
  bool prev = g_in_gc;
  g_in_gc = true;
  for (int i = 0; i < 10; i++) {
    if (JsCollectGarbage(rt) != JsNoError) {
      std::fprintf(stderr, "FATAL: JsCollectGarbage failed\n");
      std::exit(2);
    }
  }
  g_in_gc = prev;
}

PROBE_NOINLINE_OPTNONE void ClearStack() {
  constexpr int kN = 512;
  volatile void* dummy[kN];
  volatile unsigned char* bp =
      reinterpret_cast<volatile unsigned char*>(dummy);
  while (bp < reinterpret_cast<volatile unsigned char*>(dummy) +
             sizeof(dummy)) {
    *bp++ = 0;
  }
#if defined(__clang__) || defined(__GNUC__)
  __asm__ volatile("" : : "r"(dummy) : "memory");
#endif
}

/* Create a wrapped plain object, then make it collectable (the local is
 * dropped inside; the caller must GC). The helper frame is popped before GC
 * so no JS pointer (or integer bit-pattern) remains in a live register/
 * stack slot in the caller, which conservative scanning would otherwise
 * treat as a root (refs-probe methodology). */
PROBE_NOINLINE_OPTNONE bool CreateWrappedThenDrop(
    napi_env env, void* payload, Counter* /*cnt*/) {
  JsValueRef obj = make_object_assert();
  if (napi_wrap(env, reinterpret_cast<napi_value>(obj), payload, FinGeneric,
                nullptr, nullptr) != napi_ok) {
    ClearDeadRef(obj, JS_INVALID_REFERENCE);
    return false;
  }
  ClearDeadRef(obj, JS_INVALID_REFERENCE);
  return true;
}

/* Create one scoped external, close its scope (releasing the provider
 * root), and drop every local inside the helper frame so it is collectable
 * by the caller's GC. */
PROBE_NOINLINE_OPTNONE bool CreateExternalThenDrop(
    napi_env env, void* data, node_api_basic_finalize fin) {
  napi_handle_scope sc = nullptr;
  if (napi_open_handle_scope(env, &sc) != napi_ok) return false;
  napi_value ext = nullptr;
  napi_status s = napi_create_external(env, data, fin, nullptr, &ext);
  ClearDeadRef(ext, static_cast<napi_value>(nullptr));
  if (napi_close_handle_scope(env, sc) != napi_ok) return false;
  return s == napi_ok;
}

/* Create `n` scoped externals in one scope, close it, and drop them all
 * inside the helper frame (P7-11 batch: 100 per GC). */
PROBE_NOINLINE_OPTNONE bool CreateExternalBatch(
    napi_env env, void* data, int n) {
  napi_handle_scope sc = nullptr;
  if (napi_open_handle_scope(env, &sc) != napi_ok) return false;
  bool ok = true;
  for (int i = 0; i < n; i++) {
    napi_value ext = nullptr;
    if (napi_create_external(env, data, FinGeneric, nullptr, &ext) !=
        napi_ok) {
      ok = false;
    }
    ClearDeadRef(ext, static_cast<napi_value>(nullptr));
  }
  if (napi_close_handle_scope(env, sc) != napi_ok) ok = false;
  return ok;
}

/* Wrap head (id 1) + two add_finalizer riders (ids 2, 3), then drop the
 * object inside the helper frame so it is collectable by the caller's GC.
 * Mirrors CreateWrappedThenDrop rooting discipline for Phase K. */
PROBE_NOINLINE_OPTNONE bool WrapThenAddRidersThenDrop(napi_env env) {
  JsValueRef obj = make_object_assert();
  if (napi_wrap(env, reinterpret_cast<napi_value>(obj),
                reinterpret_cast<void*>(static_cast<std::uintptr_t>(1)), FinOrdered, nullptr,
                nullptr) != napi_ok) {
    ClearDeadRef(obj, JS_INVALID_REFERENCE);
    return false;
  }
  if (napi_add_finalizer(env, reinterpret_cast<napi_value>(obj),
                         reinterpret_cast<void*>(static_cast<std::uintptr_t>(2)), FinOrdered,
                         nullptr, nullptr) != napi_ok) {
    ClearDeadRef(obj, JS_INVALID_REFERENCE);
    return false;
  }
  if (napi_add_finalizer(env, reinterpret_cast<napi_value>(obj),
                         reinterpret_cast<void*>(static_cast<std::uintptr_t>(3)), FinOrdered,
                         nullptr, nullptr) != napi_ok) {
    ClearDeadRef(obj, JS_INVALID_REFERENCE);
    return false;
  }
  ClearDeadRef(obj, JS_INVALID_REFERENCE);
  return true;
}

/* add_finalizer + witness weak ref + optional weak out_ref + drop, all inside
 * the helper frame. Keeps out_ref weak (caller deletes it after drain or
 * before GC to test cancellation). */
PROBE_NOINLINE_OPTNONE napi_ref AddFinalizeThenDropWitness(
    napi_env env, Counter* cnt, napi_ref* out_ref_p) {
  if (out_ref_p) *out_ref_p = nullptr;
  JsValueRef obj = make_object_assert();
  napi_ref wit = nullptr;
  if (napi_create_reference(env, reinterpret_cast<napi_value>(obj), 0,
                            &wit) != napi_ok) {
    ClearDeadRef(obj, JS_INVALID_REFERENCE);
    return nullptr;
  }
  napi_ref out_ref = nullptr;
  if (napi_add_finalizer(env, reinterpret_cast<napi_value>(obj), cnt,
                         FinGeneric, nullptr, &out_ref) != napi_ok) {
    napi_delete_reference(env, wit);
    ClearDeadRef(obj, JS_INVALID_REFERENCE);
    return nullptr;
  }
  if (out_ref_p) *out_ref_p = out_ref;
  ClearDeadRef(obj, JS_INVALID_REFERENCE);
  return wit;
}

PROBE_NOINLINE_OPTNONE void PhaseA_CreateExternalTestAndDrop(
    napi_env env, Counter* cnt) {
  napi_handle_scope sc = nullptr;
  expect_status(napi_open_handle_scope(env, &sc), napi_ok, "A: open scope");
  napi_value ext = nullptr;
  expect_status(napi_create_external(env, cnt, FinGeneric,
                                     (void*)(std::uintptr_t)0xABCD, &ext),
                napi_ok, "A: create_external");
  expect(chakra_nodeapi_test_wrap_live_count(env) == 1, "A: live 1 after create");
  void* got = (void*)(std::uintptr_t)0xdeadbeef;
  expect_status(napi_get_value_external(env, ext, &got), napi_ok, "A: get_value");
  expect(got == (void*)cnt, "A: pointer round-trips");
  /* Non-external + NULL out-param are invalid_arg (never object_expected). */
  JsValueRef plain = make_object_assert();
  void* tmpg = (void*)(std::uintptr_t)0xdead;
  expect_status(napi_get_value_external(env, reinterpret_cast<napi_value>(plain), &tmpg),
                napi_invalid_arg, "A: get on non-external invalid_arg");
  expect_status(napi_get_value_external(env, ext, nullptr), napi_invalid_arg,
                "A: get with null result invalid_arg");
  expect_status(napi_create_external(env, cnt, nullptr, nullptr, nullptr),
                napi_invalid_arg, "A: create with null result invalid_arg");
  ClearDeadRef(plain, JS_INVALID_REFERENCE);
  expect(chakra_nodeapi_test_wrap_live_count(env) == 1, "A: still live 1");
  expect_status(napi_close_handle_scope(env, sc), napi_ok, "A: close scope releases root");
  ClearDeadRef(ext, static_cast<napi_value>(nullptr));
}

PROBE_NOINLINE_OPTNONE void PhaseB_WrapSuppressTags(
    napi_env env, JsRuntimeHandle rt, const napi_type_tag& kTagA,
    const napi_type_tag& kTagB, const napi_type_tag& kTagC) {
  Counter cnt;
  JsValueRef obj = make_object_assert();
  expect_status(napi_wrap(env, reinterpret_cast<napi_value>(obj), &cnt, FinGeneric, nullptr, nullptr),
                napi_ok, "B: wrap");
  void* un = nullptr;
  expect_status(napi_unwrap(env, reinterpret_cast<napi_value>(obj), &un), napi_ok, "B: unwrap");
  expect(un == (void*)&cnt, "B: unwrap pointer");
  expect_status(napi_wrap(env, reinterpret_cast<napi_value>(obj), &cnt, FinGeneric, nullptr, nullptr),
                napi_invalid_arg, "B: double wrap invalid_arg");
  expect_status(napi_wrap(env, nullptr, &cnt, nullptr, nullptr, nullptr),
                napi_invalid_arg, "B: wrap null object invalid_arg");
  JsValueRef num = JS_INVALID_REFERENCE;
  JsDoubleToNumber(3.0, &num);
  napi_ref dummy_ref = nullptr;
  expect_status(napi_wrap(env, reinterpret_cast<napi_value>(num), &cnt, nullptr, nullptr, &dummy_ref),
                napi_invalid_arg, "B: wrap non-object invalid_arg");
  ClearDeadRef(num, JS_INVALID_REFERENCE);

  bool tagged = false;
  expect_status(napi_type_tag_object(env, reinterpret_cast<napi_value>(obj), &kTagA), napi_ok, "B: tag A");
  expect_status(napi_check_object_type_tag(env, reinterpret_cast<napi_value>(obj), &kTagA, &tagged), napi_ok, "B: check A ok");
  expect(tagged, "B: check A true");
  expect_status(napi_check_object_type_tag(env, reinterpret_cast<napi_value>(obj), &kTagB, &tagged), napi_ok, "B: check B ok");
  expect(!tagged, "B: check B false (mismatch)");
  expect_status(napi_type_tag_object(env, reinterpret_cast<napi_value>(obj), &kTagB),
                napi_invalid_arg, "B: double tag invalid_arg");
  /* Unwrapped object: check false (not an error). */
  JsValueRef other = make_object_assert();
  tagged = false;
  expect_status(napi_check_object_type_tag(env, reinterpret_cast<napi_value>(other), &kTagA, &tagged), napi_ok, "B: check on unwrapped ok");
  expect(!tagged, "B: check on unwrapped false");
  expect_status(napi_unwrap(env, reinterpret_cast<napi_value>(other), &un), napi_invalid_arg, "B: unwrap of unwrapped invalid_arg");
  ClearDeadRef(other, JS_INVALID_REFERENCE);

  /* remove_wrap returns data and suppresses. */
  void* removed = (void*)(std::uintptr_t)0xdead;
  expect_status(napi_remove_wrap(env, reinterpret_cast<napi_value>(obj), &removed), napi_ok, "B: remove ok");
  expect(removed == (void*)&cnt, "B: remove returns data");
  expect_status(napi_unwrap(env, reinterpret_cast<napi_value>(obj), &un), napi_invalid_arg, "B: unwrap after remove invalid_arg");
  expect_status(napi_remove_wrap(env, reinterpret_cast<napi_value>(obj), &removed), napi_invalid_arg, "B: second remove invalid_arg");
  tagged = false;
  expect_status(napi_check_object_type_tag(env, reinterpret_cast<napi_value>(obj), &kTagA, &tagged), napi_ok, "B: check after remove ok");
  expect(!tagged, "B: tag dies with wrap (check false after remove)");

  /* Tag-only record on a plain object (vendored TypeTaggedInstance). */
  JsValueRef tagn = make_object_assert();
  expect_status(napi_type_tag_object(env, reinterpret_cast<napi_value>(tagn), &kTagC), napi_ok, "B: tag-only (plain object)");
  tagged = false;
  expect_status(napi_check_object_type_tag(env, reinterpret_cast<napi_value>(tagn), &kTagC, &tagged), napi_ok, "B: tag-only check ok");
  expect(tagged, "B: tag-only check true");
  expect_status(napi_unwrap(env, reinterpret_cast<napi_value>(tagn), &un), napi_invalid_arg, "B: unwrap of tag-only invalid_arg");
  expect_status(napi_wrap(env, reinterpret_cast<napi_value>(tagn), &cnt, FinGeneric, nullptr, nullptr), napi_ok, "B: wrap over tag-only upgrades");
  expect_status(napi_unwrap(env, reinterpret_cast<napi_value>(tagn), &un), napi_ok, "B: unwrap upgraded ok");
  expect(un == (void*)&cnt, "B: upgraded pointer");
  tagged = false;
  expect_status(napi_check_object_type_tag(env, reinterpret_cast<napi_value>(tagn), &kTagC, &tagged), napi_ok, "B: tag survives upgrade ok");
  expect(tagged, "B: tag survives wrap upgrade");
  /* NULL result allowed for remove. */
  expect_status(napi_remove_wrap(env, reinterpret_cast<napi_value>(tagn), nullptr), napi_ok, "B: remove with null result ok");

  ClearDeadRef(obj, JS_INVALID_REFERENCE);
  ClearDeadRef(tagn, JS_INVALID_REFERENCE);
  ClearStack();
  Collect10x(rt);
  chakra_nodeapi_test_drain_finalizers(env);
  expect(cnt.calls == 0, "B: remove suppressed (zero finalizers across GC+destroy)");
  expect(chakra_nodeapi_test_wrap_live_count(env) == 0, "B: live 0 after GC+destroy-less");
  chakra_nodeapi_destroy_env(env);
  expect(cnt.calls == 0, "B: still zero after destroy");
}

PROBE_NOINLINE_OPTNONE bool PhaseH_CheckFreshNotAliased(
    napi_env env, Counter* cnt, const napi_type_tag& kTagA) {
  bool aliased = false;
  JsValueRef fresh = make_object_assert();
  void* un = (void*)cnt; /* sentinel: must not be overwritten if miss */
  napi_status s = napi_unwrap(env, reinterpret_cast<napi_value>(fresh), &un);
  if (s == napi_ok || un != (void*)cnt) {
    aliased = true;
  }
  bool tagged = true;
  (void)napi_check_object_type_tag(env, reinterpret_cast<napi_value>(fresh),
                                   &kTagA, &tagged);
  if (tagged) aliased = true;
  ClearDeadRef(fresh, JS_INVALID_REFERENCE);
  return aliased;
}

PROBE_NOINLINE_OPTNONE void PhaseJ_PendingThenExternalAndDrop(
    napi_env env, Counter* cnt) {
  JsValueRef obj = make_object_assert();
  JsValueRef exc = make_object_assert();
  if (JsSetException(exc) != JsNoError) {
    expect(false, "J: set exception (setup)");
  }
  ClearDeadRef(exc, JS_INVALID_REFERENCE);
  /* Gated: napi_wrap must return pending_exception without executing. */
  expect_status(napi_wrap(env, reinterpret_cast<napi_value>(obj), cnt, FinGeneric, nullptr, nullptr),
                napi_pending_exception, "J: wrap gated with pending");
  void* ext_got = (void*)(std::uintptr_t)0xdead;
  napi_value ext = nullptr;
  JsValueRef cleared = JS_INVALID_REFERENCE;
  bool had = false;
  expect(JsHasException(&had) == JsNoError && had, "J: exception still set after gated wrap");
  expect(JsGetAndClearException(&cleared) == JsNoError, "J: clear exception");
  ClearDeadRef(cleared, JS_INVALID_REFERENCE);
  /* Now with no pending: create an external and prove get works. */
  napi_handle_scope sc2 = nullptr;
  expect_status(napi_open_handle_scope(env, &sc2), napi_ok, "J: open scope");
  expect_status(napi_create_external(env, cnt, FinGeneric, nullptr, &ext), napi_ok, "J: create external (no pending)");
  expect_status(napi_get_value_external(env, ext, &ext_got), napi_ok, "J: get works (no pending)");
  expect(ext_got == (void*)cnt, "J: get pointer");
  napi_close_handle_scope(env, sc2);
  ClearDeadRef(ext, static_cast<napi_value>(nullptr));
  ClearDeadRef(obj, JS_INVALID_REFERENCE);
}

struct TeardownCancelState {
  napi_env env = nullptr;
  napi_ref ref_a = nullptr;
  napi_ref ref_b = nullptr;
  napi_ref keep_a = nullptr;
  napi_ref keep_b = nullptr;
  int calls_a = 0;
  int calls_b = 0;
  bool use_remove_wrap = false;
  bool cancelled_sibling = false;
};

void FinTeardownCancelA(napi_env env, void* data, void* /*hint*/) {
  auto* st = static_cast<TeardownCancelState*>(data);
  if (!st) return;
  st->calls_a++;
  if (st->cancelled_sibling) return;
  st->cancelled_sibling = true;
  if (st->use_remove_wrap) {
    if (st->keep_b != nullptr) {
      napi_value obj_b = nullptr;
      if (napi_get_reference_value(env, st->keep_b, &obj_b) == napi_ok &&
          obj_b != nullptr) {
        void* removed = nullptr;
        if (napi_remove_wrap(env, obj_b, &removed) == napi_ok) {
          (void)removed;
        }
      }
    }
  } else if (st->ref_b != nullptr) {
    (void)napi_delete_reference(env, st->ref_b);
    st->ref_b = nullptr;
  }
}

void FinTeardownCancelB(napi_env env, void* data, void* /*hint*/) {
  auto* st = static_cast<TeardownCancelState*>(data);
  if (!st) return;
  st->calls_b++;
  if (st->cancelled_sibling) return;
  st->cancelled_sibling = true;
  if (st->use_remove_wrap) {
    if (st->keep_a != nullptr) {
      napi_value obj_a = nullptr;
      if (napi_get_reference_value(env, st->keep_a, &obj_a) == napi_ok &&
          obj_a != nullptr) {
        void* removed = nullptr;
        if (napi_remove_wrap(env, obj_a, &removed) == napi_ok) {
          (void)removed;
        }
      }
    }
  } else if (st->ref_a != nullptr) {
    (void)napi_delete_reference(env, st->ref_a);
    st->ref_a = nullptr;
  }
}

} /* namespace */

int main() {
  g_owner_thread = std::this_thread::get_id();

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

  const napi_type_tag kTagA{0xA111111111111111ull, 0xA222222222222222ull};
  const napi_type_tag kTagB{0xB111111111111111ull, 0xB222222222222222ull};
  const napi_type_tag kTagC{0xC111111111111111ull, 0xC222222222222222ull};

  /* ---- Phase A: external round-trip + exactly-once + entry drain ---- */
  {
    std::fprintf(stdout, "-- case external-entry-drain (P7-1/P7-2) --\n");
    napi_env env = chakra_nodeapi_create_env(rt, ctx, "wrapA", nullptr, nullptr);
    expect(env != nullptr, "A: create env");
    Counter cnt;
    PhaseA_CreateExternalTestAndDrop(env, &cnt);
    ClearStack();
    Collect10x(rt);
    /* Deferred: no finalizer during GC, but exactly one queued record. */
    expect(cnt.calls == 0, "A: no finalizer during GC (deferred)");
    expect(chakra_nodeapi_test_wrap_queue_count(env) == 1, "A: queued 1 after GC");
    expect(chakra_nodeapi_test_wrap_live_count(env) == 1, "A: live 1 (queued, undrained)");
    expect(!g_gc_violation, "A: no during-GC violation");
    /* Drain via a public entry (cheap-empty check inside every wrap entry). */
    JsValueRef probe = make_object_assert();
    bool tagged = false;
    expect_status(napi_check_object_type_tag(env, reinterpret_cast<napi_value>(probe),
                                             &kTagB, &tagged),
                  napi_ok, "A: entry drain call (check)");
    expect(!tagged, "A: untagged probe false");
    ClearDeadRef(probe, JS_INVALID_REFERENCE);
    expect(cnt.calls == 1, "A: finalizer ran exactly once at entry drain");
    expect(chakra_nodeapi_test_wrap_queue_count(env) == 0, "A: queue empty after drain");
    expect(chakra_nodeapi_test_wrap_live_count(env) == 0, "A: live 0 after drain");
    chakra_nodeapi_test_drain_finalizers(env);
    expect(cnt.calls == 1, "A: re-drain does not re-fire");
    chakra_nodeapi_destroy_env(env);
    expect(chakra_nodeapi_test_wrap_live_count(env) == 0, "A: live 0 after destroy (dead)");
  }

  /* ---- Phase B: wrap round-trip + suppression + tags ---- */
  {
    std::fprintf(stdout, "-- case wrap-suppress-tags (P7-6/P7-7) --\n");
    napi_env env = chakra_nodeapi_create_env(rt, ctx, "wrapB", nullptr, nullptr);
    expect(env != nullptr, "B: create env");
    PhaseB_WrapSuppressTags(env, rt, kTagA, kTagB, kTagC);
  }

  /* ---- Phase C: wrap collect exactly-once (before-collect) ---- */
  {
    std::fprintf(stdout, "-- case wrap-collect (P7-2 via before-collect) --\n");
    napi_env env = chakra_nodeapi_create_env(rt, ctx, "wrapC", nullptr, nullptr);
    expect(env != nullptr, "C: create env");
    Counter cnt;
    expect(CreateWrappedThenDrop(env, &cnt, &cnt), "C: wrapped-then-dropped");
    ClearStack();
    Collect10x(rt);
    expect(cnt.calls == 0, "C: no finalizer during GC");
    expect(chakra_nodeapi_test_wrap_queue_count(env) == 1, "C: queued 1 (before-collect fired)");
    chakra_nodeapi_test_drain_finalizers(env);
    expect(cnt.calls == 1, "C: finalizer exactly once at drain");
    expect(chakra_nodeapi_test_wrap_live_count(env) == 0, "C: live 0 after drain");
    chakra_nodeapi_destroy_env(env);
  }

  /* ---- Phase D: teardown with live wrap (P7-10) ---- */
  {
    std::fprintf(stdout, "-- case teardown-live-wrap (P7-10) --\n");
    napi_env env = chakra_nodeapi_create_env(rt, ctx, "wrapD", nullptr, nullptr);
    expect(env != nullptr, "D: create env");
    Counter cnt;
    JsValueRef obj = make_object_assert();
    expect_status(napi_wrap(env, reinterpret_cast<napi_value>(obj), &cnt, FinClosing, nullptr, nullptr),
                  napi_ok, "D: wrap live object");
    expect(chakra_nodeapi_test_wrap_live_count(env) == 1, "D: live 1 before destroy");
    /* No GC, object stays held by JS heap (local + runtime). Destroy must
     * run its finalizer exactly once as the step-6 backstop. */
    chakra_nodeapi_destroy_env(env);
    expect(cnt.calls == 1, "D: finalizer ran once at destroy");
    expect(cnt.was_closing == 1, "D: finalizer saw Closing env");
    expect(cnt.create_status == (int)napi_generic_failure, "D: create inside Closing finalizer generic_failure");
    obj = JS_INVALID_REFERENCE;
    ClearStack();
    Collect10x(rt);
    expect(chakra_nodeapi_test_wrap_live_count(env) == 0, "D: live 0 after destroy");
  }

  /* ---- Phase E: collected + immediate destroy (P7-3) ---- */
  {
    std::fprintf(stdout, "-- case collect+destroy (P7-3) --\n");
    napi_env env = chakra_nodeapi_create_env(rt, ctx, "wrapE", nullptr, nullptr);
    expect(env != nullptr, "E: create env");
    Counter cnt;
    expect(CreateWrappedThenDrop(env, &cnt, &cnt), "E: wrapped-then-dropped");
    ClearStack();
    Collect10x(rt);
    expect(cnt.calls == 0, "E: no finalizer before destroy");
    /* No explicit drain: destroy step 2 must run it exactly once. */
    chakra_nodeapi_destroy_env(env);
    expect(cnt.calls == 1, "E: exactly once via destroy step 2");
  }

  /* ---- Phase F: add_finalizer + weak optional out_ref (P7-9) ---- */
  {
    std::fprintf(stdout, "-- case add-finalizer (P7-9) --\n");
    napi_env env = chakra_nodeapi_create_env(rt, ctx, "wrapF", nullptr, nullptr);
    expect(env != nullptr, "F: create env");
    Counter cnt;
    /* Helper creates the object + witness weak ref + add_finalizer(..., &out_ref)
     * inside an optnone frame and keeps out_ref alive across GC so Phase F
     * proves optional out_ref starts WEAK (refcount == 0) and does NOT pin
     * the object across collection. */
    napi_ref out_ref = nullptr;
    napi_ref witness = AddFinalizeThenDropWitness(env, &cnt, &out_ref);
    expect(witness != nullptr && out_ref != nullptr,
           "F: witness + add_finalizer out_ref setup");
    ClearStack();
    Collect10x(rt);
    {
      napi_handle_scope vsc = nullptr;
      napi_open_handle_scope(env, &vsc);
      napi_value gotw = reinterpret_cast<napi_value>(static_cast<std::uintptr_t>(0xdeadbeef));
      napi_status vs = napi_get_reference_value(env, witness, &gotw);
      expect_status(vs, napi_ok, "F: witness get after GC");
      expect(gotw == nullptr, "F: witness collected (probe rooting is clean)");
      napi_value goto_ref = reinterpret_cast<napi_value>(static_cast<std::uintptr_t>(0xdeadbeef));
      napi_status os = napi_get_reference_value(env, out_ref, &goto_ref);
      expect_status(os, napi_ok, "F: optional out_ref get after GC");
      expect(goto_ref == nullptr, "F: optional out_ref is weak (did not pin object)");
      napi_close_handle_scope(env, vsc);
      expect_status(napi_delete_reference(env, witness), napi_ok, "F: delete witness");
    }
    expect(cnt.calls == 0, "F: no finalizer during GC");
    expect(chakra_nodeapi_test_wrap_queue_count(env) == 1, "F: queued 1");
    chakra_nodeapi_test_drain_finalizers(env);
    expect(cnt.calls == 1, "F: exactly once at drain");
    expect(chakra_nodeapi_test_wrap_live_count(env) == 0, "F: live 0");
    expect_status(napi_delete_reference(env, out_ref), napi_ok,
                  "F: delete optional out_ref after drain");
    chakra_nodeapi_destroy_env(env);
  }

  /* ---- Phase G: 10k create/collect cycles (P7-11) ---- */
  {
    std::fprintf(stdout, "-- case 10k-cycles (P7-11) --\n");
    napi_env env = chakra_nodeapi_create_env(rt, ctx, "wrapG", nullptr, nullptr);
    expect(env != nullptr, "G: create env");
    Counter cnt;
    const int kRounds = 100;
    const int kPer = 100;
    for (int r = 0; r < kRounds; r++) {
      /* Batch creation inside an optnone helper: all 100 external
       * pointers die in the helper frame so conservative scanning cannot
       * keep any of them alive across the GC. */
      expect(CreateExternalBatch(env, &cnt, kPer), "G: batch create ok");
      ClearStack();
      Collect10x(rt);
      chakra_nodeapi_test_drain_finalizers(env);
    }
    expect(cnt.calls == kRounds * kPer, "G: finalizer count 10000 exactly");
    expect(chakra_nodeapi_test_wrap_live_count(env) == 0, "G: live 0 (no leak)");
    expect(chakra_nodeapi_test_wrap_queue_count(env) == 0, "G: queue 0");
    chakra_nodeapi_destroy_env(env);
  }

  /* ---- Phase H: ABA churn (P7-8) ---- */
  {
    std::fprintf(stdout, "-- case aba-churn (P7-8) --\n");
    napi_env env = chakra_nodeapi_create_env(rt, ctx, "wrapH", nullptr, nullptr);
    expect(env != nullptr, "H: create env");
    Counter cnt;
    expect(CreateWrappedThenDrop(env, &cnt, &cnt), "H: wrapped-then-dropped");
    ClearStack();
    Collect10x(rt);
    chakra_nodeapi_test_drain_finalizers(env);
    expect(cnt.calls == 1, "H: old finalizer once");
    bool aliased = false;
    for (int i = 0; i < 200; i++) {
      if (PhaseH_CheckFreshNotAliased(env, &cnt, kTagA)) aliased = true;
      ClearStack();
    }
    expect(!aliased, "H: no stale alias across 200 recreations");
    expect(chakra_nodeapi_test_wrap_live_count(env) == 0, "H: live 0");
    chakra_nodeapi_destroy_env(env);
  }

  /* ---- Phase I: reentrant + chained drain (P7-4/P7-5 lite) ---- */
  {
    std::fprintf(stdout, "-- case reentrant-chained (P7-4/P7-5) --\n");
    napi_env env = chakra_nodeapi_create_env(rt, ctx, "wrapI", nullptr, nullptr);
    expect(env != nullptr, "I: create env");
    /* Reentrant arm: finalizer calls public APIs (version + tag checks)
     * while the drain queue lock is released (R-ENV-12). */
    {
      Counter cnt2;
      expect(CreateExternalThenDrop(env, &cnt2, FinReentrant), "I: create reentrant");
      ClearStack();
      Collect10x(rt);
      chakra_nodeapi_test_drain_finalizers(env);
      expect(cnt2.calls == 1, "I: reentrant finalizer once");
      expect(cnt2.wrong_thread == 0, "I: reentrant thread + version checks passed");
    }
    /* Chained arm: outer finalizer creates + collects inner; one drain
     * call must run both before returning. */
    {
      ChainState cs;
      cs.env = env;
      cs.rt = rt;
      cs.armed = true;
      expect(CreateExternalThenDrop(env, &cs, FinChainedFirst), "I: create chained outer");
      ClearStack();
      Collect10x(rt);
      expect(cs.first.calls == 0 && cs.second.calls == 0, "I: neither finalizer during GC");
      chakra_nodeapi_test_drain_finalizers(env);
      expect(cs.first.calls == 1, "I: outer once");
      expect(cs.second.calls == 1, "I: chained inner ran in same drain");
      expect(chakra_nodeapi_test_wrap_live_count(env) == 0, "I: live 0 after chained drain");
      expect(chakra_nodeapi_test_wrap_queue_count(env) == 0, "I: queue 0 after chained");
    }
    chakra_nodeapi_destroy_env(env);
  }

  /* ---- Phase J: pending discipline (status-mapping C, lite) ---- */
  {
    std::fprintf(stdout, "-- case pending-discipline --\n");
    napi_env env = chakra_nodeapi_create_env(rt, ctx, "wrapJ", nullptr, nullptr);
    expect(env != nullptr, "J: create env");
    Counter cnt;
    PhaseJ_PendingThenExternalAndDrop(env, &cnt);
    ClearStack();
    Collect10x(rt);
    chakra_nodeapi_test_drain_finalizers(env);
    /* The gated wrap created nothing; the one finalizer here is the
     * post-exception external above, which must run exactly once. */
    expect(cnt.calls == 1, "J: post-exception external finalized once");
    expect(chakra_nodeapi_test_wrap_live_count(env) == 0, "J: live 0");
    chakra_nodeapi_destroy_env(env);
  }

  /* ---- Phase K: drain-order characterization oracle (head-then-riders) ----
   * Pins CURRENT DrainState order (head, then riders in push order) as a
   * characterization oracle only. Upstream finalizer_order.js semantics
   * are NOT vendored (test/nodeapi/run_node_addon_api_matrix.py:273-288
   * records NEEDS_GC), so this is behavior-not-spec: if upstream is later
   * vendored, reconcile the oracle against it rather than treating a
   * mismatch here as a provider bug without evidence. */
  {
    std::fprintf(stdout, "-- case drain-order-oracle --\n");
    napi_env env = chakra_nodeapi_create_env(rt, ctx, "wrapK", nullptr, nullptr);
    expect(env != nullptr, "K: create env");
    g_drain_order.clear();
    expect(WrapThenAddRidersThenDrop(env), "K: wrap head + 2 riders setup");
    ClearStack();
    Collect10x(rt);
    expect(g_drain_order.empty(), "K: no finalizer during GC");
    expect(chakra_nodeapi_test_wrap_queue_count(env) == 1, "K: queued 1 (head only)");
    chakra_nodeapi_test_drain_finalizers(env);
    std::vector<int> want{1, 2, 3};
    expect(g_drain_order == want, "K: drain order head-then-riders [1,2,3]");
    expect(chakra_nodeapi_test_wrap_live_count(env) == 0, "K: live 0 after drain");
    expect(chakra_nodeapi_test_wrap_queue_count(env) == 0, "K: queue 0 after drain");
    chakra_nodeapi_test_drain_finalizers(env);
    expect(g_drain_order == want, "K: re-drain does not re-fire");
    chakra_nodeapi_destroy_env(env);
  }

  /* ---- Phase L: napi_delete_reference(out_ref) before GC cancels finalizer ---- */
  {
    std::fprintf(stdout, "-- case delete-out-ref-cancels-finalizer --\n");
    napi_env env = chakra_nodeapi_create_env(rt, ctx, "wrapL", nullptr, nullptr);
    expect(env != nullptr, "L: create env");
    Counter cnt;
    napi_ref out_ref = nullptr;
    napi_ref witness = AddFinalizeThenDropWitness(env, &cnt, &out_ref);
    expect(witness != nullptr && out_ref != nullptr,
           "L: witness + add_finalizer out_ref setup");
    expect(chakra_nodeapi_test_wrap_live_count(env) == 1,
           "L: live 1 before delete_reference(out_ref)");
    expect_status(napi_delete_reference(env, out_ref), napi_ok,
                  "L: delete_reference(out_ref) before GC");
    expect(chakra_nodeapi_test_wrap_live_count(env) == 0,
           "L: live 0 immediately after pre-GC cancel");
    ClearStack();
    Collect10x(rt);
    {
      napi_handle_scope vsc = nullptr;
      napi_open_handle_scope(env, &vsc);
      napi_value gotw = reinterpret_cast<napi_value>(static_cast<std::uintptr_t>(0xdeadbeef));
      napi_status vs = napi_get_reference_value(env, witness, &gotw);
      expect_status(vs, napi_ok, "L: witness get after GC");
      expect(gotw == nullptr, "L: witness collected");
      napi_close_handle_scope(env, vsc);
      expect_status(napi_delete_reference(env, witness), napi_ok, "L: delete witness");
    }
    expect(cnt.calls == 0, "L: zero finalizers during GC after cancel");
    expect(chakra_nodeapi_test_wrap_queue_count(env) == 0, "L: queue 0 after GC");
    chakra_nodeapi_test_drain_finalizers(env);
    expect(cnt.calls == 0, "L: zero finalizers at drain after cancel");
    chakra_nodeapi_destroy_env(env);
    expect(cnt.calls == 0, "L: zero finalizers at destroy after cancel");
  }

  /* ---- Phase M: sibling wrap cancel/remove during kClosing teardown ---- */
  {
    std::fprintf(stdout, "-- case sibling-cancel-during-teardown --\n");
    bool del_ok = true;
    bool rem_ok = true;
    for (int round = 0; round < 20; round++) {
      {
        napi_env env = chakra_nodeapi_create_env(rt, ctx, "wrapM_del", nullptr, nullptr);
        if (!env) { del_ok = false; break; }
        TeardownCancelState st{};
        st.env = env;
        st.use_remove_wrap = false;
        napi_handle_scope sc = nullptr;
        if (napi_open_handle_scope(env, &sc) != napi_ok) del_ok = false;
        JsValueRef obj_a = make_object_assert();
        JsValueRef obj_b = make_object_assert();
        if (napi_create_reference(env, reinterpret_cast<napi_value>(obj_a), 1, &st.keep_a) != napi_ok ||
            napi_create_reference(env, reinterpret_cast<napi_value>(obj_b), 1, &st.keep_b) != napi_ok ||
            napi_wrap(env, reinterpret_cast<napi_value>(obj_a), &st, FinTeardownCancelA, nullptr, &st.ref_a) != napi_ok ||
            napi_wrap(env, reinterpret_cast<napi_value>(obj_b), &st, FinTeardownCancelB, nullptr, &st.ref_b) != napi_ok) {
          del_ok = false;
        }
        (void)napi_close_handle_scope(env, sc);
        ClearDeadRef(obj_a, JS_INVALID_REFERENCE);
        ClearDeadRef(obj_b, JS_INVALID_REFERENCE);
        chakra_nodeapi_destroy_env(env);
        if ((st.calls_a + st.calls_b) != 1 || !st.cancelled_sibling) del_ok = false;
      }
      {
        napi_env env = chakra_nodeapi_create_env(rt, ctx, "wrapM_rem", nullptr, nullptr);
        if (!env) { rem_ok = false; break; }
        TeardownCancelState st{};
        st.env = env;
        st.use_remove_wrap = true;
        napi_handle_scope sc = nullptr;
        if (napi_open_handle_scope(env, &sc) != napi_ok) rem_ok = false;
        JsValueRef obj_a = make_object_assert();
        JsValueRef obj_b = make_object_assert();
        if (napi_create_reference(env, reinterpret_cast<napi_value>(obj_a), 1, &st.keep_a) != napi_ok ||
            napi_create_reference(env, reinterpret_cast<napi_value>(obj_b), 1, &st.keep_b) != napi_ok ||
            napi_wrap(env, reinterpret_cast<napi_value>(obj_a), &st, FinTeardownCancelA, nullptr, nullptr) != napi_ok ||
            napi_wrap(env, reinterpret_cast<napi_value>(obj_b), &st, FinTeardownCancelB, nullptr, nullptr) != napi_ok) {
          rem_ok = false;
        }
        (void)napi_close_handle_scope(env, sc);
        ClearDeadRef(obj_a, JS_INVALID_REFERENCE);
        ClearDeadRef(obj_b, JS_INVALID_REFERENCE);
        chakra_nodeapi_destroy_env(env);
        if ((st.calls_a + st.calls_b) != 1 || !st.cancelled_sibling) rem_ok = false;
      }
    }
    expect(del_ok, "M: delete_reference(sibling_ref) in teardown finalizer safe (20x)");
    expect(rem_ok, "M: remove_wrap(sibling_obj) in teardown finalizer safe (20x)");
  }

  std::fprintf(stdout, "WRAP_PROBE: %s (%d/%d)\n",
               (g_pass == g_total) ? "PASS" : "FAIL", g_pass, g_total);
  JsSetCurrentContext(JS_INVALID_REFERENCE);
  JsDisposeRuntime(rt);
  return (g_pass == g_total) ? 0 : 1;
}
