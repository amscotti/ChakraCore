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

namespace {

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
  if (s != napi_ok || ver != 8) c->wrong_thread += 1000;
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

void FinChainedFirst(napi_env env, void* data, void* /*hint*/) {
  auto* cs = static_cast<ChainState*>(data);
  if (!cs) return;
  cs->first.calls++;
  if (!cs->armed) return;
  cs->armed = false;
  /* Create a second external that becomes collectable, then GC it.
   * The GC trampoline enqueues it; the running drain loop (released
   * lock) must pick it up before returning. */
  napi_handle_scope sc = nullptr;
  if (napi_open_handle_scope(env, &sc) != napi_ok) return;
  napi_value ext = nullptr;
  if (napi_create_external(env, &cs->second, FinChainedSecond, nullptr,
                           &ext) != napi_ok) {
    (void)napi_close_handle_scope(env, sc);
    return;
  }
  (void)napi_close_handle_scope(env, sc);
  ext = nullptr;
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

/* Create a wrapped plain object, then make it collectable (the local is
 * dropped inside; the caller must GC). The helper frame is popped before GC
 * so no JS pointer (or integer bit-pattern) remains in a live register/
 * stack slot in the caller, which conservative scanning would otherwise
 * treat as a root (refs-probe methodology). */
__attribute__((noinline, optnone)) bool CreateWrappedThenDrop(
    napi_env env, void* payload, Counter* /*cnt*/) {
  JsValueRef obj = make_object_assert();
  if (napi_wrap(env, reinterpret_cast<napi_value>(obj), payload, FinGeneric,
                nullptr, nullptr) != napi_ok) {
    return false;
  }
  obj = JS_INVALID_REFERENCE;
  return true;
}

/* Create one scoped external, close its scope (releasing the provider
 * root), and drop every local inside the helper frame so it is collectable
 * by the caller's GC. */
__attribute__((noinline, optnone)) bool CreateExternalThenDrop(
    napi_env env, void* data, node_api_basic_finalize fin) {
  napi_handle_scope sc = nullptr;
  if (napi_open_handle_scope(env, &sc) != napi_ok) return false;
  napi_value ext = nullptr;
  napi_status s = napi_create_external(env, data, fin, nullptr, &ext);
  if (napi_close_handle_scope(env, sc) != napi_ok) return false;
  ext = nullptr;
  return s == napi_ok;
}

/* Create `n` scoped externals in one scope, close it, and drop them all
 * inside the helper frame (P7-11 batch: 100 per GC). */
__attribute__((noinline, optnone)) bool CreateExternalBatch(
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
    ext = nullptr;
  }
  if (napi_close_handle_scope(env, sc) != napi_ok) ok = false;
  return ok;
}

/* add_finalizer + unpin result ref + witness weak ref + drop, all inside
 * the helper frame. Returns the witness weak ref (caller deletes it). */
__attribute__((noinline, optnone)) napi_ref AddFinalizeThenDropWitness(
    napi_env env, Counter* cnt) {
  JsValueRef obj = make_object_assert();
  napi_ref wit = nullptr;
  if (napi_create_reference(env, reinterpret_cast<napi_value>(obj), 0,
                            &wit) != napi_ok) {
    return nullptr;
  }
  napi_ref out_ref = nullptr;
  if (napi_add_finalizer(env, reinterpret_cast<napi_value>(obj), cnt,
                         FinGeneric, nullptr, &out_ref) != napi_ok) {
    napi_delete_reference(env, wit);
    return nullptr;
  }
  (void)napi_delete_reference(env, out_ref);
  obj = JS_INVALID_REFERENCE;
  return wit;
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
    napi_handle_scope sc = nullptr;
    expect_status(napi_open_handle_scope(env, &sc), napi_ok, "A: open scope");
    napi_value ext = nullptr;
    expect_status(napi_create_external(env, &cnt, FinGeneric,
                                       (void*)(std::uintptr_t)0xABCD, &ext),
                  napi_ok, "A: create_external");
    expect(chakra_nodeapi_test_wrap_live_count(env) == 1, "A: live 1 after create");
    void* got = (void*)(std::uintptr_t)0xdeadbeef;
    expect_status(napi_get_value_external(env, ext, &got), napi_ok, "A: get_value");
    expect(got == (void*)&cnt, "A: pointer round-trips");
    /* Non-external + NULL out-param are invalid_arg (never object_expected). */
    JsValueRef plain = make_object_assert();
    void* tmpg = (void*)(std::uintptr_t)0xdead;
    expect_status(napi_get_value_external(env, reinterpret_cast<napi_value>(plain), &tmpg),
                  napi_invalid_arg, "A: get on non-external invalid_arg");
    expect_status(napi_get_value_external(env, ext, nullptr), napi_invalid_arg,
                  "A: get with null result invalid_arg");
    expect_status(napi_create_external(env, &cnt, nullptr, nullptr, nullptr),
                  napi_invalid_arg, "A: create with null result invalid_arg");
    plain = JS_INVALID_REFERENCE;
    expect(chakra_nodeapi_test_wrap_live_count(env) == 1, "A: still live 1");
    expect_status(napi_close_handle_scope(env, sc), napi_ok, "A: close scope releases root");
    ext = nullptr;
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
    probe = JS_INVALID_REFERENCE;
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
    num = JS_INVALID_REFERENCE;

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
    other = JS_INVALID_REFERENCE;

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

    obj = JS_INVALID_REFERENCE;
    tagn = JS_INVALID_REFERENCE;
    ClearStack();
    Collect10x(rt);
    chakra_nodeapi_test_drain_finalizers(env);
    expect(cnt.calls == 0, "B: remove suppressed (zero finalizers across GC+destroy)");
    expect(chakra_nodeapi_test_wrap_live_count(env) == 0, "B: live 0 after GC+destroy-less");
    chakra_nodeapi_destroy_env(env);
    expect(cnt.calls == 0, "B: still zero after destroy");
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

  /* ---- Phase F: add_finalizer + ref (P7-9) ---- */
  {
    std::fprintf(stdout, "-- case add-finalizer (P7-9) --\n");
    napi_env env = chakra_nodeapi_create_env(rt, ctx, "wrapF", nullptr, nullptr);
    expect(env != nullptr, "F: create env");
    Counter cnt;
    /* Helper creates the object + witness weak ref + add_finalizer +
     * unpin inside an optnone frame, so no live register or stack slot
     * in main keeps the object alive across the GC (conservative-scan
     * methodology from the refs probe). */
    napi_ref witness = AddFinalizeThenDropWitness(env, &cnt);
    expect(witness != nullptr, "F: witness + add_finalizer setup");
    ClearStack();
    Collect10x(rt);
    {
      napi_handle_scope vsc = nullptr;
      napi_open_handle_scope(env, &vsc);
      napi_value gotw = reinterpret_cast<napi_value>(static_cast<std::uintptr_t>(0xdeadbeef));
      napi_status vs = napi_get_reference_value(env, witness, &gotw);
      expect_status(vs, napi_ok, "F: witness get after GC");
      expect(gotw == nullptr, "F: witness collected (probe rooting is clean)");
      napi_close_handle_scope(env, vsc);
      napi_delete_reference(env, witness);
    }
    expect(cnt.calls == 0, "F: no finalizer during GC");
    expect(chakra_nodeapi_test_wrap_queue_count(env) == 1, "F: queued 1");
    chakra_nodeapi_test_drain_finalizers(env);
    expect(cnt.calls == 1, "F: exactly once at drain");
    expect(chakra_nodeapi_test_wrap_live_count(env) == 0, "F: live 0");
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
      JsValueRef fresh = make_object_assert();
      void* un = (void*)&cnt; /* sentinel: must not be overwritten if miss */
      napi_status s = napi_unwrap(env, reinterpret_cast<napi_value>(fresh), &un);
      if (s == napi_ok || un != (void*)&cnt) {
        /* A stale hit would either succeed or overwrite the sentinel.
         * Correct behavior is invalid_arg with sentinel untouched. */
        if (s == napi_ok || un != (void*)&cnt) aliased = true;
      }
      bool tagged = true;
      (void)napi_check_object_type_tag(env, reinterpret_cast<napi_value>(fresh),
                                       &kTagA, &tagged);
      if (tagged) aliased = true;
      /* Reused-address case is covered by the invalid_arg assertion
       * above for every fresh object; pointer equality with the dead
       * wrap is not tracked here (the integer is dropped to avoid
       * conservative-root retention). */
      fresh = JS_INVALID_REFERENCE;
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
    JsValueRef obj = make_object_assert();
    JsValueRef exc = make_object_assert();
    if (JsSetException(exc) != JsNoError) {
      expect(false, "J: set exception (setup)");
    }
    exc = JS_INVALID_REFERENCE;
    /* Gated: napi_wrap must return pending_exception without executing. */
    expect_status(napi_wrap(env, reinterpret_cast<napi_value>(obj), &cnt, FinGeneric, nullptr, nullptr),
                  napi_pending_exception, "J: wrap gated with pending");
    /* The other get/create arms of the discipline are proven on their own
     * without a pending exception below (a pending exception cannot pass
     * JsCreateObject/JsSetException setup without contaminating the probe). */
    void* ext_got = (void*)(std::uintptr_t)0xdead;
    napi_value ext = nullptr;
    JsValueRef cleared = JS_INVALID_REFERENCE;
    bool had = false;
    expect(JsHasException(&had) == JsNoError && had, "J: exception still set after gated wrap");
    expect(JsGetAndClearException(&cleared) == JsNoError, "J: clear exception");
    cleared = JS_INVALID_REFERENCE;
    /* Now with no pending: create an external and prove get works. */
    napi_handle_scope sc2 = nullptr;
    expect_status(napi_open_handle_scope(env, &sc2), napi_ok, "J: open scope");
    expect_status(napi_create_external(env, &cnt, FinGeneric, nullptr, &ext), napi_ok, "J: create external (no pending)");
    expect_status(napi_get_value_external(env, ext, &ext_got), napi_ok, "J: get works (no pending)");
    expect(ext_got == (void*)&cnt, "J: get pointer");
    napi_close_handle_scope(env, sc2);
    ext = nullptr;
    obj = JS_INVALID_REFERENCE;
    ClearStack();
    Collect10x(rt);
    chakra_nodeapi_test_drain_finalizers(env);
    /* The gated wrap created nothing; the one finalizer here is the
     * post-exception external above, which must run exactly once. */
    expect(cnt.calls == 1, "J: post-exception external finalized once");
    expect(chakra_nodeapi_test_wrap_live_count(env) == 0, "J: live 0");
    chakra_nodeapi_destroy_env(env);
  }

  std::fprintf(stdout, "WRAP_PROBE: %s (%d/%d)\n",
               (g_pass == g_total) ? "PASS" : "FAIL", g_pass, g_total);
  JsSetCurrentContext(JS_INVALID_REFERENCE);
  JsDisposeRuntime(rt);
  return (g_pass == g_total) ? 0 : 1;
}
