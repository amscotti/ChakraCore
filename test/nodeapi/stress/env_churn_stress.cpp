//-------------------------------------------------------------------------------------------------------
// Copyright (c) ChakraCore Project Contributors. All rights reserved.
// Licensed under the MIT license. See LICENSE.txt file in the project root for full license information.
//-------------------------------------------------------------------------------------------------------
//
// N5.4 env create/destroy churn (plan §6 N5.4; extends N1.1 P1-6).
//
// 1200 cycles (plan: >=1000), each cycle:
//   - create env on a shared runtime+context (generations must be
//     strictly monotonic and nonzero across cycles);
//   - create one droppable wrapped object + one droppable external,
//     GC-collect them (finalizers PENDING: no drain before destroy);
//   - create one live wrapped object + one live strong ref + one
//     unsettled deferred + one sync cleanup hook + instance data
//     (destroy must run every teardown step exactly once);
//   - assert per-env root counter + design counts, destroy, assert
//     total finalizer counts exact per cycle.
// Records destroyed-then-reused address evidence: how often the new
// env reuses the previous cycle's address, and that a stale-generation
// lookup fails without touching the new env (new env stays usable).
//
// No leak expectation: finalizer total == cycles*per-cycle-known,
// generations strictly increasing, dead tombstones <= cap, root
// counters at baseline before every destroy.
//
// Output: progress every 200 cycles, then "ENV_CHURN_STRESS: ...".
// Exit 0 = green; 1 = mismatch; 2 = fatal setup error.

#include "NodeApiHostApi.h"
#include "NodeApiWrap.h"
#include "NodeApiEnv.h"
#include "NodeApiPromise.h"
#include "NodeApiScopes.h"

#include "js_native_api.h"
#include "node_api.h"
#include "ChakraCommon.h"
#include "ChakraCore.h"

#include <cstdint>
#include <cstdio>
#include <cstdlib>

namespace {

int g_total = 0;
int g_pass = 0;

void expect(bool cond, const char* name) {
  g_total++;
  if (cond) {
    g_pass++;
    if (!cond)
      std::fprintf(stdout, "FAIL %s\n", name);
  } else {
    std::fprintf(stdout, "FAIL %s\n", name);
  }
  std::fflush(stdout);
}

void expect_quiet(bool cond, const char* name) {
  g_total++;
  if (cond) {
    g_pass++;
  } else {
    std::fprintf(stdout, "FAIL %s\n", name);
    std::fflush(stdout);
  }
}

/* Global per-cycle accounting (process lifetime). */
struct CycleCounters {
  int wrap_droppable_finalizers = 0;
  int ext_droppable_finalizers = 0;
  int wrap_live_finalizers = 0;
  int instance_finalizers = 0;
  int sync_hooks = 0;
};

CycleCounters g_cnt;
bool g_in_gc = false;

void WrapFin(napi_env, void* data, void*) {
  static_cast<CycleCounters*>(data)->wrap_droppable_finalizers++;
  if (g_in_gc) g_cnt.wrap_live_finalizers += 1000000; /* poison */
}
void ExtFin(napi_env, void* data, void*) {
  static_cast<CycleCounters*>(data)->ext_droppable_finalizers++;
}
void LiveWrapFin(napi_env, void* data, void*) {
  static_cast<CycleCounters*>(data)->wrap_live_finalizers++;
}
void InstanceFin(napi_env, void* data, void*) {
  /* Instance data is user payload (data_pool), NOT the counter struct:
   * the counter lives in the global (teardown-probe pattern). */
  (void)data;
  g_cnt.instance_finalizers++;
}
void SyncHook(void* arg) {
  static_cast<CycleCounters*>(arg)->sync_hooks++;
}

JsRuntimeHandle g_rt = JS_INVALID_REFERENCE;
JsContextRef g_ctx = JS_INVALID_REFERENCE;

JsValueRef make_object_assert() {
  JsValueRef v = JS_INVALID_REFERENCE;
  if (JsCreateObject(&v) != JsNoError || v == JS_INVALID_REFERENCE) {
    std::fprintf(stderr, "FATAL: JsCreateObject failed\n");
    std::exit(2);
  }
  return v;
}

void Collect3x(JsRuntimeHandle rt) {
  bool prev = g_in_gc;
  g_in_gc = true;
  for (int i = 0; i < 3; i++) {
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

/* Create one droppable wrap + one droppable external; both are
 * unrooted on return (scope closed, JS locals dropped in-frame). */
__attribute__((noinline, optnone)) bool CreateDroppables(napi_env env) {
  JsValueRef obj = make_object_assert();
  if (napi_wrap(env, reinterpret_cast<napi_value>(obj), &g_cnt, WrapFin,
                nullptr, nullptr) != napi_ok)
    return false;
  obj = JS_INVALID_REFERENCE;
  napi_handle_scope sc = nullptr;
  if (napi_open_handle_scope(env, &sc) != napi_ok) return false;
  napi_value ext = nullptr;
  if (napi_create_external(env, &g_cnt, ExtFin, nullptr, &ext) != napi_ok) {
    napi_close_handle_scope(env, sc);
    return false;
  }
  if (napi_close_handle_scope(env, sc) != napi_ok) return false;
  ext = nullptr;
  return true;
}

} /* namespace */

int main() {
  if (JsCreateRuntime(JsRuntimeAttributeNone, nullptr, &g_rt) != JsNoError) {
    std::fprintf(stderr, "FATAL: JsCreateRuntime failed\n");
    return 2;
  }
  if (JsCreateContext(g_rt, &g_ctx) != JsNoError) {
    std::fprintf(stderr, "FATAL: JsCreateContext failed\n");
    JsDisposeRuntime(g_rt);
    return 2;
  }
  if (JsSetCurrentContext(g_ctx) != JsNoError) {
    std::fprintf(stderr, "FATAL: JsSetCurrentContext failed\n");
    JsDisposeRuntime(g_rt);
    return 2;
  }

  constexpr int kCycles = 1200;
  std::uint64_t prev_gen = 0;
  std::uintptr_t prev_ptr = 0;
  int reuse_hits = 0;
  int stale_checks_passed = 0;
  size_t max_dead = 0;
  bool all_ok = true;
  int total_wrap_drop = 0;
  int total_ext_drop = 0;
  int total_live_wrap = 0;
  int total_instance = 0;
  int total_sync = 0;

  int data_pool[1] = {0};

  for (int i = 0; i < kCycles; i++) {
    napi_env env =
        chakra_nodeapi_create_env(g_rt, g_ctx, "churn", nullptr, nullptr);
    std::uintptr_t ptr = reinterpret_cast<std::uintptr_t>(env);
    bool cycle_ok = (env != nullptr);
    std::uint64_t gen =
        env ? chakra_nodeapi_test_env_generation(env) : 0;
    /* Generations monotonic + nonzero. */
    cycle_ok = cycle_ok && (gen != 0) && (gen > prev_gen);
    /* Address-reuse bookkeeping + stale-generation safety. */
    if (i > 0) {
      if (ptr == prev_ptr) reuse_hits++;
      /* Stale lookup with prev generation must fail cleanly and leave
       * the new env usable (env_addr_reuse HIGH-2 contract). */
      nodeapi::NodeEnv* stale = nullptr;
      bool stale_ok = nodeapi::LookupLiveEnvWithGeneration(
          static_cast<void*>(env), prev_gen, &stale);
      cycle_ok = cycle_ok && !stale_ok && stale == nullptr &&
                 chakra_nodeapi_test_is_live(env);
      {
        uint32_t ver = 0;
        if (napi_get_version(env, &ver) != napi_ok || ver != 8)
          cycle_ok = false;
      }
      if (!stale_ok && stale == nullptr) stale_checks_passed++;
    }
    size_t base = env ? chakra_nodeapi_test_root_count(env) : 0;
    cycle_ok = cycle_ok && (base == 0);

    int wrap_drop0 = g_cnt.wrap_droppable_finalizers;
    int ext_drop0 = g_cnt.ext_droppable_finalizers;
    int live0 = g_cnt.wrap_live_finalizers;
    int inst0 = g_cnt.instance_finalizers;
    int sync0 = g_cnt.sync_hooks;

    /* All napi state first (EntryGuard drains the finalizer queue on
     * entry to JS-calling APIs, plan §3.7, so the GC burn must be LAST:
     * any napi call between the burn and destroy would drain the
     * pending pair and destroy would see nothing pending -- the exact
     * P7-3 condition this probe must exercise).
     * Live wrap object + strong ref that roots it across the burn. */
    JsValueRef live = make_object_assert();
    napi_ref strong = nullptr;
    cycle_ok = cycle_ok && (napi_create_reference(
                                env, reinterpret_cast<napi_value>(live), 1,
                                &strong) == napi_ok) &&
               (strong != nullptr);
    cycle_ok = cycle_ok && (napi_wrap(env, reinterpret_cast<napi_value>(live),
                                      &g_cnt, LiveWrapFin, nullptr,
                                      nullptr) == napi_ok);
    /* Droppable pair -> pending finalizers (queued, undrained). */
    cycle_ok = cycle_ok && CreateDroppables(env);
    /* Unsettled deferred (tombstone released at destroy; the promise
     * survives the burn via its default-scope root). */
    napi_deferred def = nullptr;
    napi_value prom = nullptr;
    cycle_ok = cycle_ok && (napi_create_promise(env, &def, &prom) == napi_ok) &&
               (def != nullptr);
    /* Sync hook + instance data (run once at destroy). */
    cycle_ok = cycle_ok && (napi_add_env_cleanup_hook(env, SyncHook,
                                                      &g_cnt) == napi_ok);
    cycle_ok = cycle_ok && (napi_set_instance_data(env, data_pool, InstanceFin,
                                                   nullptr) == napi_ok);
    /* Root total before the burn: wrap/deferred roots are
     * provider-held (not in the R-REF-14 total); only the strong ref
     * (+1) and the promise's default-scope root (+1) count. */
    size_t before = chakra_nodeapi_test_root_count(env);
    cycle_ok = cycle_ok && (before == base + 2) &&
               (chakra_nodeapi_test_wrap_live_count(env) == 3) &&
               (NodeApiPromise_DeferredCount(env) == 1);
    /* GC burn LAST: droppables collect + enqueue (pending), the live
     * wrap survives via its strong ref, the deferred promise via its
     * default-scope root. Test-hook reads do not drain. */
    ClearStack();
    Collect3x(g_rt);
    /* No finalizer may run during GC; both must be queued. */
    size_t q_after_gc = chakra_nodeapi_test_wrap_queue_count(env);
    size_t l_after_gc = chakra_nodeapi_test_wrap_live_count(env);
    cycle_ok = cycle_ok && (g_cnt.wrap_droppable_finalizers == wrap_drop0) &&
               (g_cnt.ext_droppable_finalizers == ext_drop0) &&
               (q_after_gc == 2) && (l_after_gc == 3);
    size_t l_before_destroy = l_after_gc;
    size_t def_before_destroy = NodeApiPromise_DeferredCount(env);

    live = JS_INVALID_REFERENCE;
    prom = nullptr;
    ClearStack();
    /* Destroy with pending finalizers + live wrap + live ref +
     * unsettled deferred. */
    chakra_nodeapi_destroy_env(env);
    total_wrap_drop += g_cnt.wrap_droppable_finalizers - wrap_drop0;
    total_ext_drop += g_cnt.ext_droppable_finalizers - ext_drop0;
    total_live_wrap += g_cnt.wrap_live_finalizers - live0;
    total_instance += g_cnt.instance_finalizers - inst0;
    total_sync += g_cnt.sync_hooks - sync0;
    bool cycle_done = cycle_ok &&
                      (g_cnt.wrap_droppable_finalizers - wrap_drop0 == 1) &&
                      (g_cnt.ext_droppable_finalizers - ext_drop0 == 1) &&
                      (g_cnt.wrap_live_finalizers - live0 == 1) &&
                      (g_cnt.instance_finalizers - inst0 == 1) &&
                      (g_cnt.sync_hooks - sync0 == 1) &&
                      !chakra_nodeapi_test_is_live(env) &&
                      (chakra_nodeapi_test_wrap_live_count(env) == 0);
    if (chakra_nodeapi_test_dead_count() > max_dead)
      max_dead = chakra_nodeapi_test_dead_count();
    if (!cycle_done) {
      if (i < 5 || i % 100 == 0) {
        std::fprintf(stderr,
                     "cycle %d FAILED: gen=%llu ptr=%p base=%zu before=%zu "
                     "q_after_gc=%zu l_after_gc=%zu "
                     "l_before_destroy=%zu "
                     "def_before_destroy=%zu wrapdrop=%d extdrop=%d live=%d "
                     "inst=%d sync=%d cycle_ok=%d live_after=%zu\n",
                     i, (unsigned long long)gen, static_cast<void*>(env), base,
                     before, q_after_gc, l_after_gc,
                     l_before_destroy,
                     def_before_destroy,
                     g_cnt.wrap_droppable_finalizers - wrap_drop0,
                     g_cnt.ext_droppable_finalizers - ext_drop0,
                     g_cnt.wrap_live_finalizers - live0,
                     g_cnt.instance_finalizers - inst0,
                     g_cnt.sync_hooks - sync0, (int)cycle_ok,
                     chakra_nodeapi_test_wrap_live_count(env));
      }
      all_ok = false;
    }
    expect_quiet(cycle_done, "churn cycle");
    if ((i + 1) % 200 == 0) {
      std::fprintf(stdout, "churn %d/%d ok (reuse=%d dead<=%zu)\n", i + 1,
                   kCycles, reuse_hits, max_dead);
      std::fflush(stdout);
    }
    /* Dead-value probes (no dereference). Double-destroy is covered by
     * teardown_probe/env_addr_reuse_probe (asserts in debug builds abort
     * by design, R-ENV-9), so it is not re-exercised here. */
    cycle_ok = cycle_ok && !chakra_nodeapi_test_is_live(env) &&
               chakra_nodeapi_test_env_generation(env) == 0;
    if (cycle_ok && ptr != 0) {
      napi_env dead = reinterpret_cast<napi_env>(ptr);
      cycle_ok = cycle_ok && !chakra_nodeapi_test_is_live(dead) &&
                 chakra_nodeapi_test_env_generation(dead) == 0;
    }
    prev_gen = gen;
    prev_ptr = ptr;
  }

  std::fprintf(stdout,
               "churn: cycles=%d reuse_hits=%d stale_checks=%d "
               "wrapdrop=%d extdrop=%d livewrap=%d instance=%d sync=%d "
               "max_dead=%zu\n",
               kCycles, reuse_hits, stale_checks_passed, total_wrap_drop,
               total_ext_drop, total_live_wrap, total_instance, total_sync,
               max_dead);
  expect(all_ok, "churn: all cycles green");
  expect(total_wrap_drop == kCycles, "churn: droppable wrap total exact");
  expect(total_ext_drop == kCycles, "churn: droppable external total exact");
  expect(total_live_wrap == kCycles, "churn: live wrap total exact");
  expect(total_instance == kCycles, "churn: instance total exact");
  expect(total_sync == kCycles, "churn: sync hook total exact");
  expect(stale_checks_passed == kCycles - 1,
         "churn: stale-gen lookup failed every cycle");
  expect(max_dead <= 256,
         "churn: tombstones bounded (cap=256, reuse displaces)");
  /* reuse_hits is environmental (allocator-dependent): recorded, not
   * asserted. env_addr_reuse_probe proves the reuse contract; here the
   * stale-check pass is the load-bearing evidence. */
  std::fprintf(stdout, "address reuse observed in %d/%d transitions "
                       "(informational)\n",
               reuse_hits, kCycles - 1);

  std::fprintf(stdout, "ENV_CHURN_STRESS: %s (%d/%d)\n",
               (g_pass == g_total) ? "PASS" : "FAIL", g_pass, g_total);
  if (g_rt != JS_INVALID_REFERENCE) {
    JsSetCurrentContext(JS_INVALID_REFERENCE);
    JsDisposeRuntime(g_rt);
  }
  return (g_pass == g_total) ? 0 : 1;
}
