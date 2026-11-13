//-------------------------------------------------------------------------------------------------------
// Copyright (c) ChakraCore Project Contributors. All rights reserved.
// Licensed under the MIT license. See LICENSE.txt file in the project root for full license information.
//-------------------------------------------------------------------------------------------------------
//
// Teardown ordering probe for N1.4 (plan §3.6 steps 3-5, semantics
// P1-1/P1-2/P1-7). Uses a real Chakra runtime/context (via JsCreateRuntime /
// JsCreateContext) so the N1.1 destroy can enter the env context for
// step 6 (scopes/refs teardown) without crashing on fake handles.
// Lifetime entries themselves remain basic_env level and native-only.
//
// Asserts (each MUST fail the probe on mismatch, never merely print):
//   P1-7 instance-data wart: set A then replace with B calls the old
//     finalizer ZERO times; destroy calls ONLY the current (B) once.
//   P1-1 ordering: wrap_drain > sync LIFO > async reverse > instance
//     finalizer > refs_scopes_release, log compared VERBATIM (plus
//     closing/dead bookends).
//   Exactly-once: each hook/finalizer fires once across destroy.
//   Remove-unknown => napi_invalid_arg (sync pair, async NULL,
//     async double-remove, async post-destroy handle value).
//   Remove-existing suppresses (sync pair + async handle).
//   Closing window (from inside a sync hook): get_instance_data ok,
//     add hook generic_failure, set instance data generic_failure,
//     get state "closing".
//   P1-2 destroy-twice: second destroy with the saved pointer value
//     is a clean no-op (no crash, no double-free, no new log).
//   NULL-arg validation + wrong-thread rejection.
//
// Build (from repo root, via the shared gate lock):
//   /tmp/opencode/cc-gate-lock.sh mise exec -- zig c++ \
//     -std=gnu++17 -DNAPI_VERSION=8 \
//     -Iinclude/nodeapi -Ilib/Jsrt -Ilib/NodeApi \
//     test/nodeapi/probes/teardown_probe.cpp \
//     lib/NodeApi/NodeApiEnv.cpp lib/NodeApi/NodeApiLifetime.cpp \
//     lib/NodeApi/NodeApiScopes.cpp lib/NodeApi/NodeApiRefs.cpp \
//     -o /tmp/teardown_probe -Lout/jit/lib -lChakraCore \
//     -Wl,-rpath,/Users/ascotti/.local/share/opencode/worktree/b41ea0/node-api/out/jit/lib
//   /tmp/teardown_probe
//
// Links the real engine (libChakraCore.dylib) for the destroy-time
// ContextGuard + scopes/refs teardown. Exit 0 = all checks green;
// 1 = any mismatch (names the check).

#include "NodeApiHostApi.h"
#include "js_native_api.h"
#include "node_api.h"
#include "NodeApiEnv.h"
#include "ChakraCommon.h"
#include "ChakraCore.h"

#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

namespace {

// ---- Event log (single ordered vector; compared verbatim) ----

std::vector<std::string> g_log;

void Log(const char* s) {
  g_log.emplace_back(s == nullptr ? "" : s);
}

void Sink(void* ctx, const char* event) {
  (void)ctx;
  Log(event);
}

// ---- Check plumbing ----

int g_failures = 0;
int g_checks = 0;

void Check(bool cond, const char* what) {
  g_checks++;
  if (!cond) {
    g_failures++;
    std::printf("FAIL: %s\n", what);
  }
}

bool LogEquals(const std::vector<std::string>& expected, const char* what) {
  if (g_log == expected) {
    return true;
  }
  g_failures++;
  std::printf("FAIL: %s\n", what);
  std::printf("  expected (%zu):", expected.size());
  for (auto& e : expected) std::printf(" [%s]", e.c_str());
  std::printf("\n  actual   (%zu):", g_log.size());
  for (auto& a : g_log) std::printf(" [%s]", a.c_str());
  std::printf("\n");
  return false;
}

// ---- Hook/finalizer call counters ----

int g_finA_count = 0;
int g_finB_count = 0;
int g_sync_counts[8] = {0, 0, 0, 0, 0, 0, 0, 0};
int g_async_counts[8] = {0};
int g_async_remove_status[8];
napi_env g_current_env = nullptr; /* for Closing-window checks */

// Instance data payloads (user-owned storage; provider never frees).
int g_dataA = 0xA;
int g_dataB = 0xB;

void FinA(napi_env env, void* data, void* hint) {
  g_finA_count++;
  char buf[64];
  std::snprintf(buf, sizeof(buf), "instance:FinA");
  Log(buf);
  Check(data == &g_dataA, "FinA receives A data pointer");
  Check(hint == (void*)(std::uintptr_t)0xA1, "FinA receives A hint");
  (void)env;
}

void FinB(napi_env env, void* data, void* hint) {
  g_finB_count++;
  Log("instance:FinB");
  Check(data == &g_dataB, "FinB receives B data pointer");
  Check(hint == (void*)(std::uintptr_t)0xB1, "FinB receives B hint");
  // During the finalizer the env is Closing: get must still read back
  // the current triple (Closing policy for get).
  void* got = nullptr;
  napi_status st = napi_get_instance_data(env, &got);
  Check(st == napi_ok, "get_instance_data ok inside instance finalizer (Closing)");
  Check(got == &g_dataB, "get inside finalizer returns current (B) pointer");
}

void SyncHook(void* arg) {
  int idx = (int)(std::uintptr_t)arg;
  if (idx >= 0 && idx < 8) g_sync_counts[idx]++;
  char buf[32];
  std::snprintf(buf, sizeof(buf), "sync:S%d", idx);
  Log(buf);
}

void AsyncHook(napi_async_cleanup_hook_handle handle, void* data) {
  int idx = (int)(std::uintptr_t)data;
  if (idx >= 0 && idx < 8) g_async_counts[idx]++;
  char buf[32];
  std::snprintf(buf, sizeof(buf), "async:A%d", idx);
  Log(buf);
  // Proper async completion: signal done via remove (must succeed).
  napi_status st = napi_remove_async_cleanup_hook(handle);
  if (idx >= 0 && idx < 8) g_async_remove_status[idx] = (int)st;
  Check(st == napi_ok, "async hook self-remove returns napi_ok");
}

// Sync hook that probes the Closing window (P1-4). Runs at destroy.
int g_closing_probe_ran = 0;
bool g_closing_get_ok = false;
bool g_closing_get_value = false;
bool g_closing_add_generic = false;
bool g_closing_set_generic = false;
bool g_closing_state_ok = false;

void ClosingProbeSyncHook(void* /*arg*/) {
  g_log.emplace_back("sync:closingprobe");
  g_closing_probe_ran++;
  if (g_current_env == nullptr) return;
  const char* state = chakra_nodeapi_test_state_name(g_current_env);
  g_closing_state_ok = (std::strcmp(state, "closing") == 0);
  void* got = nullptr;
  napi_status st = napi_get_instance_data(g_current_env, &got);
  g_closing_get_ok = (st == napi_ok);
  g_closing_get_value = (got == &g_dataB);
  napi_async_cleanup_hook_handle bad_handle = nullptr;
  (void)bad_handle;
  napi_status st_add = napi_add_env_cleanup_hook(g_current_env, SyncHook,
                                                  (void*)(std::uintptr_t)7);
  g_closing_add_generic = (st_add == napi_generic_failure);
  napi_status st_set =
      napi_set_instance_data(g_current_env, &g_dataA, FinA, nullptr);
  g_closing_set_generic = (st_set == napi_generic_failure);
}

// ---- Real runtime/context (engine-linked) ----
// Created once in main; all phases share it (env = context x registration).

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

JsRuntimeHandle FakeRuntime() { return g_runtime; }
JsContextRef FakeContext() { return g_context; }

void ResetLog() { g_log.clear(); }

void ResetCounts() {
  g_finA_count = g_finB_count = 0;
  for (int i = 0; i < 8; i++) {
    g_sync_counts[i] = 0;
    g_async_counts[i] = 0;
    g_async_remove_status[i] = -1;
  }
  g_closing_probe_ran = 0;
  g_closing_get_ok = g_closing_get_value = false;
  g_closing_add_generic = g_closing_set_generic = false;
  g_closing_state_ok = false;
}

} /* namespace */

int main() {
  if (!InitEngine()) {
    return 1;
  }

  // ===== Phase 1: P1-7 wart + get/set basics (no hooks) =====
  ResetLog();
  ResetCounts();
  {
    napi_env env = chakra_nodeapi_create_env(
        FakeRuntime(), FakeContext(), "phase1", nullptr, nullptr);
    Check(env != nullptr, "P1-7: create env");
    g_current_env = env;
    Check(chakra_nodeapi_test_state_name(env) != nullptr &&
              std::strcmp(chakra_nodeapi_test_state_name(env), "alive") == 0,
          "P1-7: state alive after create");

    void* got = (void*)(std::uintptr_t)0xDEAD;
    Check(napi_get_instance_data(env, &got) == napi_ok, "get before set -> ok");
    Check(got == nullptr, "get before set -> null");

    Check(napi_set_instance_data(env, &g_dataA, FinA,
                                 (void*)(std::uintptr_t)0xA1) == napi_ok,
          "set A -> ok");
    Check(g_finA_count == 0, "set A does not call FinA");
    Check(napi_get_instance_data(env, &got) == napi_ok && got == &g_dataA,
          "get returns A");

    // Replace: wart = old finalizer NOT called.
    Check(napi_set_instance_data(env, &g_dataB, FinB,
                                 (void*)(std::uintptr_t)0xB1) == napi_ok,
          "replace with B -> ok");
    Check(g_finA_count == 0, "replacement does NOT call old FinA (wart)");
    Check(napi_get_instance_data(env, &got) == napi_ok && got == &g_dataB,
          "get returns B after replace");

    chakra_nodeapi_test_set_teardown_log(env, Sink, nullptr);
    chakra_nodeapi_destroy_env(env);
    Check(g_finA_count == 0, "destroy never calls replaced FinA");
    Check(g_finB_count == 1, "destroy calls current FinB exactly once");
    Check(!chakra_nodeapi_test_is_live(env), "env not live after destroy");
    g_current_env = nullptr;
  }
  std::printf("phase1 (P1-7 wart): done\n");

  // ===== Phase 2: P1-2 destroy-twice (saved value, no deref) =====
  ResetLog();
  ResetCounts();
  {
    napi_env env = chakra_nodeapi_create_env(
        FakeRuntime(), FakeContext(), "phase2", nullptr, nullptr);
    Check(env != nullptr, "P1-2: create");
    std::uintptr_t saved = reinterpret_cast<std::uintptr_t>(env);
    chakra_nodeapi_test_set_teardown_log(env, Sink, nullptr);
    chakra_nodeapi_destroy_env(env);
    std::vector<std::string> first = g_log;
    Check(chakra_nodeapi_test_is_live(
              reinterpret_cast<napi_env>(saved)) == false,
          "P1-2: not live after first destroy");
    Check(std::strcmp(chakra_nodeapi_test_state_name(
                          reinterpret_cast<napi_env>(saved)),
                      "dead") == 0,
          "P1-2: state name dead after destroy (no deref)");
    // Second destroy with the same (now-dead) value: must be a clean
    // no-op (assert in debug is enabled here but the probe compiles
    // without NDEBUG -- the assert would abort! So compile with
    // -DNDEBUG for the probe (see build note) and assert release
    // behaviour; in non-NDEBUG the provider asserts by design (R-ENV-9).
    // To keep the probe runnable in both modes, the second destroy is
    // expected to log "ignored" and add NO new events either way when
    // asserts are disabled. If asserts are enabled the process aborts
    // -- that IS the debug contract (P1-2 asserts the abort, run
    // separately). This probe documents that split below.)
#ifdef NDEBUG
    chakra_nodeapi_destroy_env(reinterpret_cast<napi_env>(saved));
    Check(g_log == first, "P1-2: second destroy adds no events (NDEBUG)");
    Check(!chakra_nodeapi_test_is_live(
              reinterpret_cast<napi_env>(saved)),
          "P1-2: still not live after second destroy");
#else
    std::printf("P1-2/NDEBUG note: built without NDEBUG; skipping second-destroy "
                "call (provider asserts by design). Rebuild with -DNDEBUG "
                "to exercise the no-op path.\n");
#endif
  }
  std::printf("phase2 (P1-2): done\n");

  // ===== Phase 3: sync LIFO + verbatim order (full P1-1) =====
  ResetLog();
  ResetCounts();
  {
    napi_env env = chakra_nodeapi_create_env(
        FakeRuntime(), FakeContext(), "phase3", nullptr, nullptr);
    Check(env != nullptr, "P1-1: create");
    g_current_env = env;
    // Registration order S1,S2,S3 must fire S3,S2,S1 (LIFO).
    Check(napi_add_env_cleanup_hook(env, SyncHook,
             (void*)(std::uintptr_t)1) == napi_ok,
          "add S1");
    Check(napi_add_env_cleanup_hook(env, SyncHook,
             (void*)(std::uintptr_t)2) == napi_ok,
          "add S2");
    Check(napi_add_env_cleanup_hook(env, SyncHook,
             (void*)(std::uintptr_t)3) == napi_ok,
          "add S3");
    Check(napi_set_instance_data(env, &g_dataB, FinB,
                                 (void*)(std::uintptr_t)0xB1) == napi_ok,
          "set instance B");
    chakra_nodeapi_test_set_teardown_log(env, Sink, nullptr);
    chakra_nodeapi_destroy_env(env);
    g_current_env = nullptr;
    Check(g_sync_counts[1] == 1 && g_sync_counts[2] == 1 &&
              g_sync_counts[3] == 1,
          "P1-1: each sync hook exactly once");
    Check(g_finB_count == 1, "P1-1: instance finalizer once");
    std::vector<std::string> expected = {
        "closing", "wrap_drain",
        "sync:S3", "sync:S2", "sync:S1",
        "instance:FinB",
        "refs_scopes_release", "dead"};
    LogEquals(expected, "P1-1: sync LIFO + instance order verbatim");
  }
  std::printf("phase3 (P1-1 sync): done\n");

  // ===== Phase 4: async order + self-remove + exactly-once =====
  ResetLog();
  ResetCounts();
  {
    napi_env env = chakra_nodeapi_create_env(
        FakeRuntime(), FakeContext(), "phase4", nullptr, nullptr);
    Check(env != nullptr, "async: create");
    napi_async_cleanup_hook_handle h1 = nullptr, h2 = nullptr;
    Check(napi_add_async_cleanup_hook(env, AsyncHook,
             (void*)(std::uintptr_t)1, &h1) == napi_ok && h1 != nullptr,
          "add A1");
    Check(napi_add_async_cleanup_hook(env, AsyncHook,
             (void*)(std::uintptr_t)2, &h2) == napi_ok && h2 != nullptr,
          "add A2");
    Check(h1 != h2, "handles distinct");
    Check(napi_set_instance_data(env, &g_dataB, FinB,
                                 (void*)(std::uintptr_t)0xB1) == napi_ok,
          "set instance");
    chakra_nodeapi_test_set_teardown_log(env, Sink, nullptr);
    chakra_nodeapi_destroy_env(env);
    Check(g_async_counts[1] == 1 && g_async_counts[2] == 1,
          "async: each hook exactly once");
    Check(g_async_remove_status[1] == (int)napi_ok &&
              g_async_remove_status[2] == (int)napi_ok,
          "async: self-remove ok");
    std::vector<std::string> expected = {
        "closing", "wrap_drain",
        "async:A2", "async:A1",
        "instance:FinB",
        "refs_scopes_release", "dead"};
    LogEquals(expected, "async: reverse order verbatim (no sync hooks)");
  }
  std::printf("phase4 (async): done\n");

  // ===== Phase 5: full order with sync+async+instance (P1-1 complete) =====
  ResetLog();
  ResetCounts();
  {
    napi_env env = chakra_nodeapi_create_env(
        FakeRuntime(), FakeContext(), "phase5", nullptr, nullptr);
    Check(env != nullptr, "full: create");
    napi_add_env_cleanup_hook(env, SyncHook, (void*)(std::uintptr_t)1);
    napi_add_env_cleanup_hook(env, SyncHook, (void*)(std::uintptr_t)2);
    napi_async_cleanup_hook_handle h1 = nullptr, h2 = nullptr;
    napi_add_async_cleanup_hook(env, AsyncHook,
                                (void*)(std::uintptr_t)1, &h1);
    napi_add_async_cleanup_hook(env, AsyncHook,
                                (void*)(std::uintptr_t)2, &h2);
    napi_set_instance_data(env, &g_dataB, FinB,
                           (void*)(std::uintptr_t)0xB1);
    chakra_nodeapi_test_set_teardown_log(env, Sink, nullptr);
    chakra_nodeapi_destroy_env(env);
    std::vector<std::string> expected = {
        "closing", "wrap_drain",
        "sync:S2", "sync:S1",
        "async:A2", "async:A1",
        "instance:FinB",
        "refs_scopes_release", "dead"};
    LogEquals(expected, "P1-1: full verbatim wrap>sync>async>instance>refs");
  }
  std::printf("phase5 (P1-1 full): done\n");

  // ===== Phase 6: remove-existing suppresses; remove-unknown invalid_arg =====
  ResetLog();
  ResetCounts();
  {
    napi_env env = chakra_nodeapi_create_env(
        FakeRuntime(), FakeContext(), "phase6", nullptr, nullptr);
    Check(env != nullptr, "remove: create");
    napi_add_env_cleanup_hook(env, SyncHook, (void*)(std::uintptr_t)1);
    napi_add_env_cleanup_hook(env, SyncHook, (void*)(std::uintptr_t)2);
    napi_add_env_cleanup_hook(env, SyncHook, (void*)(std::uintptr_t)3);
    Check(napi_remove_env_cleanup_hook(env, SyncHook,
                 (void*)(std::uintptr_t)2) == napi_ok,
          "remove S2 -> ok");
    Check(napi_remove_env_cleanup_hook(env, SyncHook,
                 (void*)(std::uintptr_t)2) == napi_invalid_arg,
          "remove S2 again -> invalid_arg (already gone)");
    Check(napi_remove_env_cleanup_hook(env, SyncHook,
                 (void*)(std::uintptr_t)7) == napi_invalid_arg,
          "remove unknown S7 -> invalid_arg");
    Check(napi_remove_env_cleanup_hook(env, nullptr,
                 (void*)(std::uintptr_t)1) == napi_invalid_arg,
          "remove NULL fun -> invalid_arg");

    napi_async_cleanup_hook_handle h1 = nullptr, h2 = nullptr;
    napi_add_async_cleanup_hook(env, AsyncHook,
                                (void*)(std::uintptr_t)1, &h1);
    napi_add_async_cleanup_hook(env, AsyncHook,
                                (void*)(std::uintptr_t)2, &h2);
    Check(napi_remove_async_cleanup_hook(h2) == napi_ok,
          "remove A2 before destroy -> ok");
    Check(napi_remove_async_cleanup_hook(h2) == napi_invalid_arg,
          "remove A2 again -> invalid_arg (double-remove safe)");
    Check(napi_remove_async_cleanup_hook(nullptr) == napi_invalid_arg,
          "remove NULL handle -> invalid_arg");

    std::uintptr_t h2_value =
        reinterpret_cast<std::uintptr_t>(h2);
    chakra_nodeapi_test_set_teardown_log(env, Sink, nullptr);
    chakra_nodeapi_destroy_env(env);
    Check(g_sync_counts[1] == 1 && g_sync_counts[3] == 1 &&
              g_sync_counts[2] == 0,
          "removed sync S2 never fires");
    Check(g_async_counts[1] == 1 && g_async_counts[2] == 0,
          "removed async A2 never fires");
    // Post-destroy handle value (freed): remove must return
    // invalid_arg WITHOUT dereferencing (registry by value).
    Check(napi_remove_async_cleanup_hook(
              reinterpret_cast<napi_async_cleanup_hook_handle>(h2_value)) ==
              napi_invalid_arg,
          "remove post-destroy handle -> invalid_arg (no deref)");
    // h1 self-removed during destroy; its value is also dead now.
    std::uintptr_t h1_value =
        reinterpret_cast<std::uintptr_t>(h1);
    (void)h1_value; /* value tested implicitly above; do not deref */
  }
  std::printf("phase6 (remove): done\n");

  // ===== Phase 7: Closing window (P1-4) =====
  ResetLog();
  ResetCounts();
  {
    napi_env env = chakra_nodeapi_create_env(
        FakeRuntime(), FakeContext(), "phase7", nullptr, nullptr);
    Check(env != nullptr, "closing: create");
    g_current_env = env;
    napi_set_instance_data(env, &g_dataB, FinB,
                           (void*)(std::uintptr_t)0xB1);
    Check(napi_add_env_cleanup_hook(env, ClosingProbeSyncHook,
             nullptr) == napi_ok,
          "add closing probe");
    chakra_nodeapi_test_set_teardown_log(env, Sink, nullptr);
    chakra_nodeapi_destroy_env(env);
    g_current_env = nullptr;
    Check(g_closing_probe_ran == 1, "closing probe ran once");
    Check(g_closing_state_ok, "state was closing inside hook");
    Check(g_closing_get_ok && g_closing_get_value,
          "get ok + current value inside hook (Closing)");
    Check(g_closing_add_generic, "add during Closing -> generic_failure");
    Check(g_closing_set_generic, "set during Closing -> generic_failure");
  }
  std::printf("phase7 (Closing): done\n");

  // ===== Phase 8: NULL validation + dead-env validation =====
  {
    void* got = nullptr;
    Check(napi_set_instance_data(nullptr, &g_dataA, FinA, nullptr) ==
              napi_invalid_arg,
          "set NULL env -> invalid_arg");
    Check(napi_get_instance_data(nullptr, &got) == napi_invalid_arg,
          "get NULL env -> invalid_arg");
    Check(napi_add_env_cleanup_hook(nullptr, SyncHook, nullptr) ==
              napi_invalid_arg,
          "add NULL env -> invalid_arg");
    Check(napi_add_async_cleanup_hook(nullptr, AsyncHook, nullptr,
             nullptr) == napi_invalid_arg,
          "add async NULL env -> invalid_arg");

    napi_env env = chakra_nodeapi_create_env(
        FakeRuntime(), FakeContext(), "phase8", nullptr, nullptr);
    Check(env != nullptr, "nullval: create");
    Check(napi_get_instance_data(env, nullptr) == napi_invalid_arg,
          "get NULL out-param -> invalid_arg");
    Check(napi_add_env_cleanup_hook(env, nullptr, nullptr) ==
              napi_invalid_arg,
          "add NULL fun -> invalid_arg");
    napi_async_cleanup_hook_handle h = nullptr;
    Check(napi_add_async_cleanup_hook(env, nullptr, nullptr, &h) ==
              napi_invalid_arg,
          "add async NULL hook -> invalid_arg");
    Check(napi_add_async_cleanup_hook(env, AsyncHook, nullptr, nullptr) ==
              napi_invalid_arg,
          "add async NULL handle out -> invalid_arg");
    // Fabricated env value (never live): must not crash.
    napi_env fake = reinterpret_cast<napi_env>((std::uintptr_t)0xDEAD);
    Check(napi_get_instance_data(fake, &got) == napi_invalid_arg,
          "get fabricated env -> invalid_arg (no crash)");
    Check(!chakra_nodeapi_test_is_live(fake),
          "fabricated env not live");
    chakra_nodeapi_destroy_env(env);
    // Post-destroy env value for napi_*: also invalid_arg, no deref.
    Check(napi_get_instance_data(env, &got) == napi_invalid_arg,
          "get dead env -> invalid_arg (no deref)");
    Check(napi_set_instance_data(env, &g_dataA, FinA, nullptr) ==
              napi_invalid_arg,
          "set dead env -> invalid_arg (no deref)");
  }
  std::printf("phase8 (nullval): done\n");

  // ===== Phase 9: wrong-thread rejection (R-ENV-11) =====
  {
    napi_env env = chakra_nodeapi_create_env(
        FakeRuntime(), FakeContext(), "phase9", nullptr, nullptr);
    Check(env != nullptr, "thread: create");
    bool worker_done = false;
    bool worker_result_ok = false;
    std::thread t([&]() {
      void* got = nullptr;
      bool ok = (napi_get_instance_data(env, &got) == napi_invalid_arg);
      // Note: ResolveEnv asserts in debug; the probe is built with
      // -DNDEBUG (see build note) so the release path (invalid_arg,
      // no context entry) is what runs here. A debug build asserts
      // instead -- that IS the debug contract (P1-5 splits the same
      // way as P1-2).
      worker_result_ok = ok;
      worker_done = true;
    });
    t.join();
    Check(worker_done && worker_result_ok,
          "wrong-thread get -> invalid_arg");
    // Owner thread still works after the wrong-thread attempt.
    void* got = nullptr;
    Check(napi_get_instance_data(env, &got) == napi_ok,
          "owner thread still alive after wrong-thread attempt");
    chakra_nodeapi_destroy_env(env);
  }
  std::printf("phase9 (thread): done\n");

  std::printf("teardown_probe: %d checks, %d failures\n", g_checks,
              g_failures);
  ShutdownEngine();
  return g_failures == 0 ? 0 : 1;
}
