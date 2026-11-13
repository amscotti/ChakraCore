//-------------------------------------------------------------------------------------------------------
// Copyright (c) ChakraCore Project Contributors. All rights reserved.
// Licensed under the MIT license. See LICENSE.txt file in the project root for full license information.
//-------------------------------------------------------------------------------------------------------
//
// Env-registry address-reuse probe (Wave-1 review HIGH-2).
//
// Asserts (each MUST fail the probe on mismatch):
//   - Generations are monotonic and nonzero; live lookup verifies
//     the registry mirror; dead lookup fails without dereferencing.
//   - Destroy inserts a bounded tombstone (<= nodeapi::kDeadEnvCap)
//     with the destroyed generation; FIFO eviction bounds it.
//   - Create at a tombstoned address displaces the old tombstone
//     and supersedes it with a fresh, larger generation.
//   - Generation-aware lookup with the OLD generation fails WITHOUT
//     dereferencing the new env (new env stays alive and usable);
//     with the NEW generation it succeeds. Pointer-only lookup
//     succeeds on the new env by construction (irreducible ABA,
//     header A5) -- asserted to document the limit, not as a pass.
//
// Build (from repo root, via the shared gate lock), mirroring
// teardown_probe.cpp:
//   sh /tmp/opencode/cc-gate-lock.sh mise exec -- zig c++ \
//     -std=gnu++17 -DNAPI_VERSION=8 -DNDEBUG \
//     -Iinclude/nodeapi -Ilib/Jsrt -Ilib/NodeApi \
//     test/nodeapi/probes/env_addr_reuse_probe.cpp \
//     lib/NodeApi/NodeApiEnv.cpp lib/NodeApi/NodeApiLifetime.cpp \
//     lib/NodeApi/NodeApiScopes.cpp lib/NodeApi/NodeApiRefs.cpp \
//     -o /tmp/env_addr_reuse_probe \
//     -Lzig-out/lib -lChakraCore \
//     -Wl,-rpath,/Users/ascotti/.local/share/opencode/worktree/b41ea0/node-api/zig-out/lib
//   /tmp/env_addr_reuse_probe
//
// Compiles NodeApiEnv.cpp directly (same binary), so hidden-visibility
// registry entries are still linkable. -DNDEBUG selects release
// destroy-twice semantics (assert is debug-only per R-ENV-9).
// Exit 0 = all checks green; 1 = mismatch (names the check).

#include "NodeApiHostApi.h"
#include "js_native_api.h"
#include "node_api.h"
#include "NodeApiEnv.h"
#include "ChakraCommon.h"
#include "ChakraCore.h"

#include <cstdint>
#include <cstdio>
#include <vector>

namespace {

int g_failures = 0;
int g_checks = 0;

void Check(bool cond, const char* what) {
  g_checks++;
  if (!cond) {
    g_failures++;
    std::printf("FAIL: %s\n", what);
  }
}

JsRuntimeHandle g_runtime = JS_INVALID_RUNTIME_HANDLE;
JsContextRef g_context = JS_INVALID_REFERENCE;

bool InitEngine() {
  if (JsCreateRuntime(JsRuntimeAttributeNone, nullptr, &g_runtime) != JsNoError) {
    std::fprintf(stderr, "FATAL: JsCreateRuntime failed\n");
    return false;
  }
  if (JsCreateContext(g_runtime, &g_context) != JsNoError) {
    std::fprintf(stderr, "FATAL: JsCreateContext failed\n");
    JsDisposeRuntime(g_runtime);
    g_runtime = JS_INVALID_RUNTIME_HANDLE;
    return false;
  }
  if (JsSetCurrentContext(g_context) != JsNoError) {
    std::fprintf(stderr, "FATAL: JsSetCurrentContext failed\n");
    JsDisposeRuntime(g_runtime);
    g_runtime = JS_INVALID_RUNTIME_HANDLE;
    g_context = JS_INVALID_REFERENCE;
    return false;
  }
  return true;
}

void ShutdownEngine() {
  if (g_context != JS_INVALID_REFERENCE) {
    JsSetCurrentContext(JS_INVALID_REFERENCE);
    g_context = JS_INVALID_REFERENCE;
  }
  if (g_runtime != JS_INVALID_RUNTIME_HANDLE) {
    JsDisposeRuntime(g_runtime);
    g_runtime = JS_INVALID_RUNTIME_HANDLE;
  }
}

} /* namespace */

int main() {
  if (!InitEngine()) return 1;

  // --- Phase 1: create -> generation issued, live lookup ---
  napi_env envA = chakra_nodeapi_create_env(g_runtime, g_context, "probeA", nullptr, nullptr);
  Check(envA != nullptr, "A: create");
  std::uint64_t genA = chakra_nodeapi_test_env_generation(envA);
  Check(genA != 0, "A: generation nonzero");
  Check(chakra_nodeapi_test_is_live(envA), "A: is_live true");
  {
    nodeapi::NodeEnv* out = nullptr;
    Check(nodeapi::LookupLiveEnv(static_cast<void*>(envA), &out) && out != nullptr,
          "A: pointer lookup succeeds");
    Check(out != nullptr && out->generation == genA,
          "A: registry mirror == env field == generation hook");
    nodeapi::NodeEnv* out2 = nullptr;
    Check(nodeapi::LookupLiveEnvWithGeneration(static_cast<void*>(envA), genA, &out2) && out2 == out,
          "A: generation-aware lookup with correct gen succeeds");
    nodeapi::NodeEnv* out3 = nullptr;
    Check(!nodeapi::LookupLiveEnvWithGeneration(static_cast<void*>(envA), genA + 1000000, &out3),
          "A: generation-aware lookup with wrong gen fails");
    Check(chakra_nodeapi_test_dead_count() == 0, "A: dead map empty while live");
  }
  std::uintptr_t ptrA = reinterpret_cast<std::uintptr_t>(envA);
  std::printf("phase1: A ptr=%p gen=%llu\n", static_cast<void*>(envA),
              static_cast<unsigned long long>(genA));

  // --- Phase 2: destroy -> tombstone recorded, lookups fail ---
  chakra_nodeapi_destroy_env(envA);
  Check(!chakra_nodeapi_test_is_live(envA), "A destroyed: not live");
  Check(chakra_nodeapi_test_env_generation(envA) == 0,
        "A destroyed: generation query 0 (no deref)");
  Check(chakra_nodeapi_test_dead_count() == 1, "A destroyed: one tombstone");
  Check(chakra_nodeapi_test_dead_generation(envA) == genA,
        "A destroyed: tombstone has last generation");
  {
    nodeapi::NodeEnv* out = nullptr;
    Check(!nodeapi::LookupLiveEnv(static_cast<void*>(envA), &out),
          "A destroyed: pointer lookup fails (no deref)");
    nodeapi::NodeEnv* out2 = nullptr;
    Check(!nodeapi::LookupLiveEnvWithGeneration(static_cast<void*>(envA), genA, &out2),
          "A destroyed: generation-aware lookup fails (not live)");
    // Fabricated values also fail without crashing.
    napi_env fake = reinterpret_cast<napi_env>(static_cast<std::uintptr_t>(0xDEAD));
    Check(!nodeapi::LookupLiveEnv(static_cast<void*>(fake), &out),
          "fabricated: lookup fails no crash");
    Check(chakra_nodeapi_test_env_generation(fake) == 0,
          "fabricated: generation 0");
  }
  std::printf("phase2: destroy tombstone ok\n");

  // --- Phase 3: address-reuse sequence ---
  // Create B. Same-size NodeEnv allocations reuse the freed slot
  // under the system allocator (LIFO tcache); loop until reuse is
  // observed (bounded) so the probe is not flaky AND proves the
  // sequence when it does reuse. If the allocator never reuses,
  // the generation checks still run (non-reuse path).
  napi_env envB = nullptr;
  std::uint64_t genB = 0;
  std::uintptr_t ptrB = 0;
  bool reused = false;
  for (int attempt = 0; attempt < 200; attempt++) {
    envB = chakra_nodeapi_create_env(g_runtime, g_context, "probeB", nullptr, nullptr);
    Check(envB != nullptr, "B: create");
    genB = chakra_nodeapi_test_env_generation(envB);
    Check(genB != 0, "B: generation nonzero");
    Check(genB > genA, "B: generation strictly greater than A");
    ptrB = reinterpret_cast<std::uintptr_t>(envB);
    if (ptrB == ptrA) {
      reused = true;
      break;
    }
    // No reuse this round: destroy and retry (B's tombstones evict
    // as needed; A was already displaced only on reuse).
    chakra_nodeapi_destroy_env(envB);
    genA = genB;
    ptrA = ptrB;
    envB = nullptr;
  }
  Check(envB != nullptr, "B: eventually created");
  std::printf("phase3: B ptr=%p gen=%llu reused=%d\n",
              static_cast<void*>(envB),
              static_cast<unsigned long long>(genB), reused ? 1 : 0);
  if (reused) {
    // Reuse: create displaced the old tombstone; new generation
    // superseded it. Stale-generation lookup must fail WITHOUT
    // touching the new env; pointer-only lookup aliases the new env.
    Check(chakra_nodeapi_test_dead_count() == 0 || chakra_nodeapi_test_dead_generation(envB) != genA,
          "reuse: old tombstone displaced (not still GEN_A)");
    nodeapi::NodeEnv* out = nullptr;
    Check(nodeapi::LookupLiveEnv(static_cast<void*>(envB), &out) && out != nullptr,
          "reuse: pointer-only lookup succeeds on NEW env (irreducible alias)");
    nodeapi::NodeEnv* stale = nullptr;
    bool stale_ok = nodeapi::LookupLiveEnvWithGeneration(
        static_cast<void*>(envB), genA, &stale);
    Check(!stale_ok, "reuse: stale-generation lookup FAILS (detectable)");
    Check(stale == nullptr, "reuse: stale lookup returned no env");
    // New env must be unharmed by the failed stale lookup.
    Check(chakra_nodeapi_test_is_live(envB), "reuse: new env still live after stale lookup");
    nodeapi::NodeEnv* cur = nullptr;
    Check(nodeapi::LookupLiveEnvWithGeneration(static_cast<void*>(envB), genB, &cur) && cur != nullptr,
          "reuse: current-generation lookup succeeds");
    // A live napi_* (get_version) still works on the new env.
    uint32_t ver = 0;
    Check(napi_get_version(envB, &ver) == napi_ok && ver == 8,
          "reuse: new env usable (napi_get_version == 8)");
  } else {
    // Allocator never reused in 200 tries: the displacement path was
    // not exercised, but generation monotonicity was. Still verify
    // B is live and its tombstone story is consistent.
    std::printf("phase3 note: allocator never reused address in 200 tries; "
                "non-reuse path only\n");
    nodeapi::NodeEnv* out = nullptr;
    Check(nodeapi::LookupLiveEnv(static_cast<void*>(envB), &out) && out != nullptr,
          "non-reuse: B live");
  }
  chakra_nodeapi_destroy_env(envB);
  Check(!chakra_nodeapi_test_is_live(envB), "B destroyed: not live");
  Check(chakra_nodeapi_test_dead_generation(envB) == genB,
        "B destroyed: tombstone has B generation");

  // --- Phase 4: double destroy (no reuse) fails without deref ---
  napi_env envC = chakra_nodeapi_create_env(g_runtime, g_context, "probeC", nullptr, nullptr);
  Check(envC != nullptr, "C: create");
  std::uintptr_t ptrC = reinterpret_cast<std::uintptr_t>(envC);
  chakra_nodeapi_destroy_env(envC);
  size_t dead_after_c = chakra_nodeapi_test_dead_count();
  // Second destroy with saved value: clean no-op (NDEBUG release).
  chakra_nodeapi_destroy_env(reinterpret_cast<napi_env>(ptrC));
  Check(chakra_nodeapi_test_dead_count() == dead_after_c,
        "C: second destroy adds no tombstone (no double free)");
  Check(!chakra_nodeapi_test_is_live(reinterpret_cast<napi_env>(ptrC)),
        "C: still dead after second destroy");
  std::printf("phase4: double-destroy ok\n");

  // --- Phase 5: tombstone bound (FIFO cap) ---
  const size_t cap = nodeapi::kDeadEnvCap;
  // Sequential single-env churn reuses one address, so displacement
  // (not eviction) bounds the count: assert it never exceeds cap.
  for (size_t i = 0; i < cap + 32; i++) {
    napi_env e = chakra_nodeapi_create_env(g_runtime, g_context, "probeCap", nullptr, nullptr);
    Check(e != nullptr, "cap churn: create");
    chakra_nodeapi_destroy_env(e);
    size_t c = chakra_nodeapi_test_dead_count();
    if (c > cap) {
      Check(false, "cap churn: dead count exceeds cap");
      break;
    }
  }
  Check(chakra_nodeapi_test_dead_count() <= cap,
        "cap churn: bounded (displacement, not growth)");
  // Distinct-address batch proves FIFO eviction: create cap+32 live
  // simultaneously (distinct addresses), destroy all; dead count
  // must be exactly cap, oldest evicted, newest retained.
  {
    std::vector<napi_env> batch;
    batch.reserve(cap + 32);
    for (size_t i = 0; i < cap + 32; i++) {
      napi_env e = chakra_nodeapi_create_env(g_runtime, g_context, "probeCapBatch", nullptr, nullptr);
      Check(e != nullptr, "cap batch: create");
      batch.push_back(e);
    }
    // All batch addresses distinct (simultaneously live).
    bool distinct = true;
    for (size_t i = 0; i < batch.size() && distinct; i++) {
      for (size_t j = i + 1; j < batch.size(); j++) {
        if (batch[i] == batch[j]) distinct = false;
      }
    }
    Check(distinct, "cap batch: distinct addresses while live");
    std::uint64_t first_gen = chakra_nodeapi_test_env_generation(batch[0]);
    std::uint64_t last_gen = chakra_nodeapi_test_env_generation(batch.back());
    Check(first_gen != 0 && last_gen != 0 && last_gen > first_gen,
          "cap batch: generations monotonic across batch");
    napi_env first_ptr = batch[0];
    napi_env last_ptr = batch.back();
    for (auto e : batch) chakra_nodeapi_destroy_env(e);
    Check(chakra_nodeapi_test_dead_count() == cap,
          "cap batch: bounded at cap after distinct destroys");
    Check(chakra_nodeapi_test_dead_generation(last_ptr) == last_gen,
          "cap batch: newest tombstone retained");
    Check(chakra_nodeapi_test_dead_generation(first_ptr) != first_gen,
          "cap batch: oldest evicted (FIFO)");
    // Evicted double-destroy still fails safely (live-map miss).
    chakra_nodeapi_destroy_env(first_ptr);
    Check(chakra_nodeapi_test_dead_count() == cap,
          "cap batch: evicted double-destroy adds no tombstone");
    std::printf("phase5: bound cap=%zu ok (churn <= cap, batch == cap, FIFO)\n", cap);
  }

  std::printf("env_addr_reuse_probe: %d checks, %d failures (reused=%d)\n",
              g_checks, g_failures, reused ? 1 : 0);
  ShutdownEngine();
  return g_failures == 0 ? 0 : 1;
}
