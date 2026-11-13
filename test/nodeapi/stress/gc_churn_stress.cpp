//-------------------------------------------------------------------------------------------------------
// Copyright (c) ChakraCore Project Contributors. All rights reserved.
// Licensed under the MIT license. See LICENSE.txt file in the project root for full license information.
//-------------------------------------------------------------------------------------------------------
//
// N5.4 GC churn stress (plan §6 N5.4; G3 gate validation).
//
// Externals / wraps / deferreds / refs created and collected under
// JsCollectGarbage (the -CollectGarbage equivalent; see receipt), each
// for kRepeats=12 repeats (plan: >=10). Every repeat asserts:
//   - finalizer counts exact (no drift, no double-fire, none during GC);
//   - chakra_nodeapi_test_root_count returns to the env baseline;
//   - wrap live/queue counts and deferred counts are exact.
// Single shared env for externals/wraps/refs (a leak accumulates visibly
// across repeats); fresh env per deferred repeat (settled deferreds are
// retained as tombstones until destroy by design, NodeApiPromise.cpp, so
// a per-repeat fresh env is the only way the count returns to 0).
//
// Conservative-stack methodology (refs/wrap probes): droppable objects
// are created inside optnone helpers and dropped with ClearStack before
// the GC burn, so survival can only come from an intended root.
//
// Output: one line per repeat, then "GC_CHURN_STRESS: PASS/FAIL (n/n)".
// Exit 0 = all repeats green; 1 = any mismatch; 2 = fatal setup error.

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
#include <vector>

namespace {

int g_total = 0;
int g_pass = 0;

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

void expect_repeat(bool cond, const char* cat, int rep) {
  char buf[96];
  std::snprintf(buf, sizeof(buf), "%s repeat %02d", cat, rep);
  expect(cond, buf);
}

struct Counter {
  int calls = 0;
  int during_gc = 0;
};

bool g_in_gc = false;

void FinGeneric(napi_env, void* data, void*) {
  auto* c = static_cast<Counter*>(data);
  if (c) {
    c->calls++;
    if (g_in_gc) c->during_gc++;
  }
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

/* --- optnone creation helpers (droppable locals die inside) --- */

__attribute__((noinline, optnone)) bool CreateExternalBatch(napi_env env,
                                                             Counter* c,
                                                             int n) {
  napi_handle_scope sc = nullptr;
  if (napi_open_handle_scope(env, &sc) != napi_ok) return false;
  bool ok = true;
  for (int i = 0; i < n; i++) {
    napi_value ext = nullptr;
    if (napi_create_external(env, c, FinGeneric, nullptr, &ext) != napi_ok)
      ok = false;
    ext = nullptr;
  }
  if (napi_close_handle_scope(env, sc) != napi_ok) ok = false;
  return ok;
}

__attribute__((noinline, optnone)) bool CreateWrapBatch(napi_env env,
                                                         Counter* c, int n) {
  bool ok = true;
  for (int i = 0; i < n; i++) {
    JsValueRef obj = make_object_assert();
    if (napi_wrap(env, reinterpret_cast<napi_value>(obj), c, FinGeneric,
                  nullptr, nullptr) != napi_ok)
      ok = false;
    obj = JS_INVALID_REFERENCE;
  }
  return ok;
}

/* Creates n objects + n refs (strength array: 1=strong, 0=weak),
 * drops the object locals inside this frame. Caller deletes refs. */
__attribute__((noinline, optnone)) std::vector<napi_ref> CreateRefBatch(
    napi_env env, const std::vector<int>& strengths, bool* ok) {
  std::vector<napi_ref> refs;
  refs.reserve(strengths.size());
  if (ok) *ok = true;
  for (size_t i = 0; i < strengths.size(); i++) {
    JsValueRef obj = make_object_assert();
    napi_ref r = nullptr;
    if (napi_create_reference(env, reinterpret_cast<napi_value>(obj),
                              strengths[i] ? 1 : 0, &r) != napi_ok) {
      if (ok) *ok = false;
    }
    refs.push_back(r);
    obj = JS_INVALID_REFERENCE;
  }
  if (ok) {
    for (auto r : refs)
      if (r == nullptr) *ok = false;
  }
  return refs;
}

/* --- deferred host pump (mirrors promise probe) --- */

struct HostPump {
  std::vector<JsValueRef> tasks;
};

void HostContinuation(JsValueRef task, void* state) {
  auto* pump = static_cast<HostPump*>(state);
  if (JsAddRef(task, nullptr) == JsNoError) pump->tasks.push_back(task);
}

napi_status PumpHost(HostPump* pump, size_t limit = 100000) {
  size_t iter = 0;
  while (!pump->tasks.empty()) {
    if (++iter > limit) return napi_generic_failure;
    JsValueRef task = pump->tasks.front();
    pump->tasks.erase(pump->tasks.begin());
    JsValueRef undef = JS_INVALID_REFERENCE;
    JsGetUndefinedValue(&undef);
    JsValueRef arr[1] = {undef};
    JsValueRef res = JS_INVALID_REFERENCE;
    JsErrorCode ec = JsCallFunction(task, arr, 1, &res);
    JsRelease(task, nullptr);
    if (ec != JsNoError) {
      bool has = false;
      if (JsHasException(&has) == JsNoError && has)
        return napi_pending_exception;
      return napi_generic_failure;
    }
  }
  return napi_ok;
}

struct ChainCounter {
  int n = 0;
};

JsValueRef ChainHandler(JsValueRef, bool, JsValueRef*, unsigned short,
                        void* state) {
  static_cast<ChainCounter*>(state)->n++;
  JsValueRef u = JS_INVALID_REFERENCE;
  JsGetUndefinedValue(&u);
  return u;
}

JsRuntimeHandle g_rt = JS_INVALID_REFERENCE;
JsContextRef g_ctx = JS_INVALID_REFERENCE;

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

  constexpr int kRepeats = 12;
  constexpr int kBatch = 50;

  /* ============ externals: shared env, per-repeat exactness ============ */
  napi_env env = chakra_nodeapi_create_env(g_rt, g_ctx, "gc-ext", nullptr,
                                           nullptr);
  if (!env) {
    std::fprintf(stderr, "FATAL: create env failed\n");
    return 2;
  }
  size_t base = chakra_nodeapi_test_root_count(env);
  expect(base == 0, "externals: baseline 0");
  int ext_total = 0;
  for (int r = 0; r < kRepeats; r++) {
    Counter c;
    bool ok = CreateExternalBatch(env, &c, kBatch);
    ClearStack();
    Collect10x(g_rt);
    bool pre = ok && c.calls == 0 && c.during_gc == 0 &&
               chakra_nodeapi_test_wrap_queue_count(env) ==
                   static_cast<size_t>(kBatch) &&
               chakra_nodeapi_test_wrap_live_count(env) ==
                   static_cast<size_t>(kBatch);
    chakra_nodeapi_test_drain_finalizers(env);
    ext_total += kBatch;
    bool post = c.calls == kBatch && c.during_gc == 0 &&
                chakra_nodeapi_test_wrap_queue_count(env) == 0 &&
                chakra_nodeapi_test_wrap_live_count(env) == 0 &&
                chakra_nodeapi_test_root_count(env) == base;
    expect_repeat(pre && post, "externals", r);
    if (!(pre && post))
      std::fprintf(stderr,
                   "  externals rep %d: pre ok=%d calls=%d queue=%zu live=%zu "
                   "post calls=%d queue=%zu live=%zu roots=%zu base=%zu\n",
                   r, (int)ok, c.calls,
                   chakra_nodeapi_test_wrap_queue_count(env),
                   chakra_nodeapi_test_wrap_live_count(env), c.calls,
                   chakra_nodeapi_test_wrap_queue_count(env),
                   chakra_nodeapi_test_wrap_live_count(env),
                   chakra_nodeapi_test_root_count(env), base);
  }
  std::fprintf(stdout, "externals total finalized: %d (want %d)\n", ext_total,
               kRepeats * kBatch);
  expect(ext_total == kRepeats * kBatch, "externals: total exact");

  /* ============ wraps: shared env, per-repeat exactness ============ */
  int wrap_total = 0;
  for (int r = 0; r < kRepeats; r++) {
    Counter c;
    bool ok = CreateWrapBatch(env, &c, kBatch);
    ClearStack();
    Collect10x(g_rt);
    bool pre = ok && c.calls == 0 && c.during_gc == 0 &&
               chakra_nodeapi_test_wrap_queue_count(env) ==
                   static_cast<size_t>(kBatch) &&
               chakra_nodeapi_test_wrap_live_count(env) ==
                   static_cast<size_t>(kBatch);
    chakra_nodeapi_test_drain_finalizers(env);
    wrap_total += kBatch;
    bool post = c.calls == kBatch && c.during_gc == 0 &&
                chakra_nodeapi_test_wrap_queue_count(env) == 0 &&
                chakra_nodeapi_test_wrap_live_count(env) == 0 &&
                chakra_nodeapi_test_root_count(env) == base;
    expect_repeat(pre && post, "wraps", r);
    if (!(pre && post))
      std::fprintf(stderr,
                   "  wraps rep %d: ok=%d calls=%d queue=%zu live=%zu roots=%zu\n",
                   r, (int)ok, c.calls,
                   chakra_nodeapi_test_wrap_queue_count(env),
                   chakra_nodeapi_test_wrap_live_count(env),
                   chakra_nodeapi_test_root_count(env));
  }
  std::fprintf(stdout, "wraps total finalized: %d (want %d)\n", wrap_total,
               kRepeats * kBatch);
  expect(wrap_total == kRepeats * kBatch, "wraps: total exact");
  expect(chakra_nodeapi_test_root_count(env) == base,
         "wraps: root baseline held after 12 repeats");

  /* ============ refs: shared env, strong survives / weak collects ============ */
  {
    std::vector<int> strengths;
    for (int i = 0; i < 30; i++) strengths.push_back(1);
    for (int i = 0; i < 20; i++) strengths.push_back(0);
    for (int r = 0; r < kRepeats; r++) {
      bool ok = false;
      std::vector<napi_ref> refs = CreateRefBatch(env, strengths, &ok);
      bool mid = ok && refs.size() == static_cast<size_t>(kBatch) &&
                 chakra_nodeapi_test_root_count(env) ==
                     base + static_cast<size_t>(kBatch);
      ClearStack();
      Collect10x(g_rt);
      /* Scoped get each: strong non-null, weak null. */
      bool gets_ok = true;
      {
        napi_handle_scope sc = nullptr;
        if (napi_open_handle_scope(env, &sc) != napi_ok) {
          gets_ok = false;
        } else {
          for (size_t i = 0; i < refs.size(); i++) {
            napi_value got =
                reinterpret_cast<napi_value>(static_cast<std::uintptr_t>(
                    0xdeadbeef));
            if (napi_get_reference_value(env, refs[i], &got) != napi_ok) {
              gets_ok = false;
              break;
            }
            if (strengths[i] == 1) {
              if (got == nullptr) {
                gets_ok = false;
                break;
              }
            } else {
              if (got != nullptr) {
                gets_ok = false;
                break;
              }
            }
          }
          napi_close_handle_scope(env, sc);
        }
      }
      for (auto ref : refs) {
        if (napi_delete_reference(env, ref) != napi_ok) gets_ok = false;
      }
      bool post = gets_ok &&
                  chakra_nodeapi_test_root_count(env) == base;
      expect_repeat(mid && post, "refs", r);
      if (!(mid && post))
        std::fprintf(stderr,
                     "  refs rep %d: ok=%d mid=%d roots=%zu base=%zu\n", r,
                     (int)ok, (int)mid, chakra_nodeapi_test_root_count(env),
                     base);
    }
  }
  chakra_nodeapi_destroy_env(env);
  expect(!chakra_nodeapi_test_is_live(env), "shared env destroyed");

  /* ============ deferreds: fresh env per repeat ============ */
  constexpr int kDefPer = 20;
  int def_total = 0;
  for (int r = 0; r < kRepeats; r++) {
    HostPump pump;
    JsSetPromiseContinuationCallback(HostContinuation, &pump);
    HostPump* pump_ptr = &pump;
    napi_env denv = chakra_nodeapi_create_env(g_rt, g_ctx, "gc-def", nullptr,
                                              nullptr);
    if (!denv) {
      expect(false, "deferreds: create env");
      continue;
    }
    /* Re-install after env create (host-clobber contract, PR-8). */
    JsSetPromiseContinuationCallback(HostContinuation, pump_ptr);
    size_t dbase = chakra_nodeapi_test_root_count(denv);
    ChainCounter chain;
    JsValueRef handler = JS_INVALID_REFERENCE;
    if (JsCreateFunction(ChainHandler, &chain, &handler) != JsNoError) {
      expect(false, "deferreds: handler fn");
      chakra_nodeapi_destroy_env(denv);
      continue;
    }
    /* Host-root the handler across the settle+GC window (R1). */
    JsAddRef(handler, nullptr);
    std::vector<napi_deferred> defs;
    bool setup_ok = true;
    {
      napi_handle_scope sc = nullptr;
      if (napi_open_handle_scope(denv, &sc) != napi_ok) {
        setup_ok = false;
      } else {
        for (int i = 0; i < kDefPer; i++) {
          napi_deferred d = nullptr;
          napi_value prom = nullptr;
          if (napi_create_promise(denv, &d, &prom) != napi_ok) {
            setup_ok = false;
            break;
          }
          defs.push_back(d);
          /* Attach the counting handler while rooted in scope. */
          napi_value then_fn = nullptr;
          if (napi_get_named_property(denv, prom, "then", &then_fn) !=
              napi_ok) {
            setup_ok = false;
            break;
          }
          napi_value res = nullptr;
          napi_value args[1] = {reinterpret_cast<napi_value>(handler)};
          if (napi_call_function(denv, prom, then_fn, 1, args, &res) !=
              napi_ok) {
            setup_ok = false;
            break;
          }
        }
        if (setup_ok) {
          for (int i = 0; i < kDefPer; i++) {
            JsValueRef num = JS_INVALID_REFERENCE;
            JsDoubleToNumber(static_cast<double>(i) + 0.5, &num);
            napi_value v = reinterpret_cast<napi_value>(num);
            if (napi_resolve_deferred(denv, defs[i], v) != napi_ok) {
              setup_ok = false;
              break;
            }
            num = JS_INVALID_REFERENCE;
          }
        }
        napi_close_handle_scope(denv, sc);
      }
    }
    bool mid = setup_ok && chain.n == 0 &&
               NodeApiPromise_DeferredCount(denv) ==
                   static_cast<size_t>(kDefPer) &&
               chakra_nodeapi_test_root_count(denv) == dbase;
    ClearStack();
    Collect10x(g_rt);
    bool pre_pump = chakra_nodeapi_test_root_count(denv) == dbase &&
                    chain.n == 0;
    napi_status pst = PumpHost(&pump);
    bool post = pst == napi_ok && chain.n == kDefPer &&
                NodeApiPromise_QueueDepth(denv) == 0 &&
                chakra_nodeapi_test_root_count(denv) == dbase;
    def_total += kDefPer;
    expect_repeat(mid && pre_pump && post, "deferreds", r);
    if (!(mid && pre_pump && post))
      std::fprintf(stderr,
                   "  deferreds rep %d: setup=%d mid=%d pre_pump=%d post=%d "
                   "chain=%d defcount=%zu roots=%zu dbase=%zu\n",
                   r, (int)setup_ok, (int)mid, (int)pre_pump, (int)post,
                   chain.n, NodeApiPromise_DeferredCount(denv),
                   chakra_nodeapi_test_root_count(denv), dbase);
    JsRelease(handler, nullptr);
    chakra_nodeapi_destroy_env(denv);
    expect(!chakra_nodeapi_test_is_live(denv), "deferreds: env destroyed");
    expect(NodeApiPromise_DeferredCount(denv) == 0,
           "deferreds: dead count 0");
  }
  std::fprintf(stdout, "deferreds total settled: %d (want %d)\n", def_total,
               kRepeats * kDefPer);
  expect(def_total == kRepeats * kDefPer, "deferreds: total exact");

  std::fprintf(stdout, "GC_CHURN_STRESS: %s (%d/%d)\n",
               (g_pass == g_total) ? "PASS" : "FAIL", g_pass, g_total);
  if (g_rt != JS_INVALID_REFERENCE) {
    JsSetCurrentContext(JS_INVALID_REFERENCE);
    JsDisposeRuntime(g_rt);
  }
  return (g_pass == g_total) ? 0 : 1;
}
