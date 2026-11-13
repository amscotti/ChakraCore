// gate_probe.cpp — MEDIUM-2/3, N0.6/C, missing-symbols probes.
//
// Covers (each MUST fail probe on mismatch):
//   Async pump (MEDIUM-2, plan §3.6 step 4):
//     A. Self-removing hook (synchronous remove inside fn) passes
//        with 0 pump iterations, no violation, reverse order.
//     B. Non-self-removing hook (never removes) hits the host pump
//        bound and is reported as host contract violation
//        (Lifetime_AsyncViolation true, teardown "async_pump_timeout"
//        emitted, destroy still completes without hang, fails fast).
//     C. Deferred removal via pump (hook schedules removal after N
//        pump turns) passes with bounded iterations, no violation.
//     D. Reverse-registration order preserved with pump.
//   Entry guard / Closing parity (MEDIUM-3):
//     E. Closing parity: delete_reference and get_reference_value
//        succeed inside a sync hook (Closing), matching lifetime
//        get/remove. create_reference still fails in Closing.
//   Last-error clearing (N0.6/C AMB-STATUS-1):
//     F. fail -> get_last_error_info == failure, then success ->
//        get_last_error_info == napi_ok (success clears). Reading
//        via get_last_error_info does NOT clear itself.
//   Missing symbols (napi_is_exception_pending / get_and_clear):
//     G. is_pending false initially, true after JsSetException,
//        get_and_clear returns the exception and clears, second
//        check false, get_and_clear with no pending returns ok+null.
// Build (repo root, via gate lock — see run_gate_probe.sh):
//   /tmp/opencode/cc-gate-lock.sh mise exec -- zig c++ \
//     -std=gnu++17 -DNAPI_VERSION=8 -DCHAKRA_NODEAPI_TEST_HOOKS \
//     -Iinclude/nodeapi -Ilib/Jsrt -Ilib/NodeApi \
//     test/nodeapi/probes/gate_probe.cpp \
//     lib/NodeApi/NodeApiEnv.cpp lib/NodeApi/NodeApiLifetime.cpp \
//     lib/NodeApi/NodeApiScopes.cpp lib/NodeApi/NodeApiRefs.cpp \
//     -o /tmp/gate_probe -Lzig-out/lib -lChakraCore \
//     -Wl,-rpath,... (see runner)
// Links real engine for ContextGuard / JsHasException / scope rooting.
// Exit 0 = green; 1 = any mismatch.

#include "NodeApiHostApi.h"
#include "js_native_api.h"
#include "node_api.h"
#include "NodeApiEnv.h"
#include "NodeApiLifetime.h"
#include "NodeApiRefs.h"
#include "ChakraCommon.h"
#include "ChakraCore.h"

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace {

int g_failures = 0;
int g_checks = 0;
std::vector<std::string> g_log;

void Log(const char* s) { g_log.emplace_back(s ? s : ""); }
void Sink(void* ctx, const char* event) { (void)ctx; Log(event); }

void Check(bool cond, const char* what) {
  g_checks++;
  if (!cond) {
    g_failures++;
    std::printf("FAIL: %s\n", what);
  }
}

bool LogContains(const char* needle) {
  for (auto& e : g_log) if (e == needle) return true;
  return false;
}

void ResetLog() { g_log.clear(); }

JsRuntimeHandle g_runtime = JS_INVALID_RUNTIME_HANDLE;
JsContextRef g_context = JS_INVALID_REFERENCE;

bool InitEngine() {
  if (JsCreateRuntime(JsRuntimeAttributeNone, nullptr, &g_runtime) != JsNoError) return false;
  if (JsCreateContext(g_runtime, &g_context) != JsNoError) { JsDisposeRuntime(g_runtime); g_runtime = JS_INVALID_RUNTIME_HANDLE; return false; }
  if (JsSetCurrentContext(g_context) != JsNoError) { JsDisposeRuntime(g_runtime); g_runtime = JS_INVALID_RUNTIME_HANDLE; g_context = JS_INVALID_REFERENCE; return false; }
  return true;
}
void ShutdownEngine() {
  if (g_context != JS_INVALID_REFERENCE) { JsSetCurrentContext(JS_INVALID_REFERENCE); g_context = JS_INVALID_REFERENCE; }
  if (g_runtime != JS_INVALID_RUNTIME_HANDLE) { JsDisposeRuntime(g_runtime); g_runtime = JS_INVALID_RUNTIME_HANDLE; }
}

// ---- Async hook helpers ----
int g_async_fired[8] = {0};
void AsyncSelfRemove(napi_async_cleanup_hook_handle h, void* data) {
  int idx = (int)(uintptr_t)data;
  if (idx >=0 && idx<8) g_async_fired[idx]++;
  char buf[32]; snprintf(buf,sizeof(buf),"async:A%d",idx); Log(buf);
  napi_status st = napi_remove_async_cleanup_hook(h);
  Check(st==napi_ok, "self-remove ok");
}
void AsyncNeverRemove(napi_async_cleanup_hook_handle, void* data) {
  int idx=(int)(uintptr_t)data;
  if (idx>=0 && idx<8) g_async_fired[idx]++;
  char buf[32]; snprintf(buf,sizeof(buf),"async:A%d",idx); Log(buf);
  // deliberately never removes
}

// Deferred removal state
struct DeferredCtx {
  napi_async_cleanup_hook_handle h = nullptr;
  int countdown = 0;
  bool scheduled = false;
};
DeferredCtx g_deferred[8];
void AsyncDeferred(napi_async_cleanup_hook_handle h, void* data) {
  int idx=(int)(uintptr_t)data;
  if (idx>=0 && idx<8) g_async_fired[idx]++;
  char buf[32]; snprintf(buf,sizeof(buf),"async:A%d",idx); Log(buf);
  if (idx>=0 && idx<8) {
    g_deferred[idx].h = h;
    g_deferred[idx].countdown = 2; // remove after 2 pump turns
    g_deferred[idx].scheduled = true;
  }
}
int g_pump_calls = 0;
void CountingPump(void* ctx) {
  (void)ctx;
  g_pump_calls++;
  for (int i=0;i<8;i++) {
    if (g_deferred[i].scheduled && g_deferred[i].countdown>0) {
      if (--g_deferred[i].countdown==0) {
        napi_remove_async_cleanup_hook(g_deferred[i].h);
        g_deferred[i].scheduled=false;
      }
    }
  }
}

// Sync hook for Closing parity (delete/get in Closing)
napi_env g_current_env = nullptr;
napi_ref g_parity_ref = nullptr;
bool g_parity_delete_ok = false;
bool g_parity_get_ok = false;
bool g_parity_create_fails = false;
void ClosingParityHook(void*) {
  Log("sync:parity");
  if (!g_current_env || !g_parity_ref) return;
  napi_value v=nullptr;
  napi_status stg = napi_get_reference_value(g_current_env, g_parity_ref, &v);
  g_parity_get_ok = (stg==napi_ok);
  napi_status stdel = napi_delete_reference(g_current_env, g_parity_ref);
  g_parity_delete_ok = (stdel==napi_ok);
  // create should still fail in Closing (strict Alive-only)
  napi_value obj=nullptr;
  JsValueRef jobj=JS_INVALID_REFERENCE; JsCreateObject(&jobj); obj=(napi_value)jobj;
  napi_ref tmp=nullptr;
  napi_status stc = napi_create_reference(g_current_env, obj, 1, &tmp);
  g_parity_create_fails = (stc==napi_generic_failure);
  if (tmp) {
    // create failed, tmp should stay null; if it leaked a ref, delete it if possible (but create failed, so tmp null)
  }
}

int g_finB_count=0;
void FinB(napi_env, void*, void*) { g_finB_count++; Log("instance:FinB"); }

} // namespace

int main() {
  if (!InitEngine()) return 1;
  napi_env env=nullptr;

  // --- Phase A: self-removing hook passes ---
  {
    ResetLog(); for(int i=0;i<8;i++) g_async_fired[i]=0;
    env = chakra_nodeapi_create_env(g_runtime,g_context,"gateA",nullptr,nullptr);
    Check(env!=nullptr,"A create");
    napi_async_cleanup_hook_handle h1=nullptr,h2=nullptr;
    Check(napi_add_async_cleanup_hook(env,AsyncSelfRemove,(void*)(uintptr_t)1,&h1)==napi_ok,"A add1");
    Check(napi_add_async_cleanup_hook(env,AsyncSelfRemove,(void*)(uintptr_t)2,&h2)==napi_ok,"A add2");
    // host injects pump (max 5) even though self-removing needs 0
    g_pump_calls=0;
    nodeapi::Lifetime_SetAsyncPump(reinterpret_cast<nodeapi::NodeEnv*>(env), CountingPump, nullptr, 5);
    chakra_nodeapi_test_set_teardown_log(env,Sink,nullptr);
    uintptr_t saved=(uintptr_t)env;
    chakra_nodeapi_destroy_env(env);
    Check(g_async_fired[1]==1 && g_async_fired[2]==1,"A fired once each");
    Check(!nodeapi::Lifetime_AsyncViolation((void*)saved),"A no violation");
    Check(g_pump_calls==0,"A pump not needed for sync self-remove");
    std::vector<std::string> exp={"closing","wrap_drain","async:A2","async:A1","refs_scopes_release","dead"};
    Check(g_log==exp,"A reverse order verbatim");
    nodeapi::Lifetime_ClearAsyncPump((void*)saved);
    std::printf("phaseA (async self-remove): done\n");
  }
  // --- Phase B: non-self-removing violates bounded ---
  {
    ResetLog(); for(int i=0;i<8;i++) g_async_fired[i]=0;
    env = chakra_nodeapi_create_env(g_runtime,g_context,"gateB",nullptr,nullptr);
    Check(env!=nullptr,"B create");
    napi_async_cleanup_hook_handle h1=nullptr;
    Check(napi_add_async_cleanup_hook(env,AsyncNeverRemove,(void*)(uintptr_t)1,&h1)==napi_ok,"B add");
    g_pump_calls=0;
    nodeapi::Lifetime_SetAsyncPump(reinterpret_cast<nodeapi::NodeEnv*>(env), CountingPump, nullptr, 5);
    chakra_nodeapi_test_set_teardown_log(env,Sink,nullptr);
    uintptr_t saved=(uintptr_t)env;
    // Use a wall-clock guard: if pump hangs, this would hang; bound 5 ensures fail fast (<1s)
    chakra_nodeapi_destroy_env(env);
    Check(g_async_fired[1]==1,"B fired once");
    Check(nodeapi::Lifetime_AsyncViolation((void*)saved),"B violation reported");
    Check(LogContains("async_pump_timeout"),"B timeout event emitted");
    Check(g_pump_calls==5,"B pump hit bound 5");
    // After bound, handle was force-retired so destroy completed; second remove must be invalid_arg
    // h1 value is now dead (freed at discard); by-value check must be invalid_arg
    Check(napi_remove_async_cleanup_hook(h1)==napi_invalid_arg,"B post-destroy remove invalid_arg (no deref)");
    nodeapi::Lifetime_ClearAsyncPump((void*)saved);
    std::printf("phaseB (async non-self-remove bounded): done\n");
  }
  // --- Phase C: deferred via pump passes ---
  {
    ResetLog(); for(int i=0;i<8;i++) g_async_fired[i]=0;
    for(int i=0;i<8;i++) { g_deferred[i].h=nullptr; g_deferred[i].countdown=0; g_deferred[i].scheduled=false; }
    env = chakra_nodeapi_create_env(g_runtime,g_context,"gateC",nullptr,nullptr);
    Check(env!=nullptr,"C create");
    napi_async_cleanup_hook_handle h1=nullptr,h2=nullptr;
    Check(napi_add_async_cleanup_hook(env,AsyncDeferred,(void*)(uintptr_t)1,&h1)==napi_ok,"C add1");
    Check(napi_add_async_cleanup_hook(env,AsyncDeferred,(void*)(uintptr_t)2,&h2)==napi_ok,"C add2");
    g_pump_calls=0;
    nodeapi::Lifetime_SetAsyncPump(reinterpret_cast<nodeapi::NodeEnv*>(env), CountingPump, nullptr, 10);
    chakra_nodeapi_test_set_teardown_log(env,Sink,nullptr);
    uintptr_t saved=(uintptr_t)env;
    chakra_nodeapi_destroy_env(env);
    Check(g_async_fired[1]==1 && g_async_fired[2]==1,"C fired once each");
    Check(!nodeapi::Lifetime_AsyncViolation((void*)saved),"C no violation (pump allowed removal)");
    Check(g_pump_calls>0,"C pump was used for deferred removal");
    // reverse order still: A2 fires first, pumps to remove, then A1
    std::vector<std::string> exp={"closing","wrap_drain","async:A2","async:A1","refs_scopes_release","dead"};
    Check(g_log==exp,"C reverse order with deferred pump");
    nodeapi::Lifetime_ClearAsyncPump((void*)saved);
    std::printf("phaseC (async deferred via pump): done\n");
  }
  // --- Phase D: Closing parity ---
  {
    ResetLog();
    env = chakra_nodeapi_create_env(g_runtime,g_context,"gateD",nullptr,nullptr);
    Check(env!=nullptr,"D create");
    g_current_env=env;
    JsValueRef jobj=JS_INVALID_REFERENCE; JsCreateObject(&jobj);
    napi_value obj=(napi_value)jobj;
    napi_ref ref=nullptr;
    Check(napi_create_reference(env,obj,1,&ref)==napi_ok && ref!=nullptr,"D create ref");
    g_parity_ref=ref;
    g_parity_delete_ok=g_parity_get_ok=false;
    Check(napi_add_env_cleanup_hook(env,ClosingParityHook,nullptr)==napi_ok,"D add hook");
    chakra_nodeapi_test_set_teardown_log(env,Sink,nullptr);
    chakra_nodeapi_destroy_env(env);
    g_current_env=nullptr;
    Check(g_parity_get_ok,"D get_reference_value ok in Closing");
    Check(g_parity_delete_ok,"D delete_reference ok in Closing (parity)");
    Check(g_parity_create_fails,"D create_reference generic_failure in Closing (strict)");
    std::printf("phaseD (Closing parity): done\n");
  }
  // --- Phase E: Last-error clearing ---
  {
    ResetLog();
    env = chakra_nodeapi_create_env(g_runtime,g_context,"gateE",nullptr,nullptr);
    Check(env!=nullptr,"E create");
    // Fail: get_instance_data with NULL out-param -> invalid_arg, last_error=invalid_arg
    void* got=nullptr;
    Check(napi_get_instance_data(env,nullptr)==napi_invalid_arg,"E fail invalid_arg");
    const napi_extended_error_info* ei=nullptr;
    Check(napi_get_last_error_info(env,&ei)==napi_ok && ei && ei->error_code==napi_invalid_arg,"E last_error records failure");
    Check(ei->error_message && strlen(ei->error_message)>0,"E message non-empty");
    // Reading again (without intervening success) still same error (get_last_error_info does not clear itself)
    const napi_extended_error_info* ei2=nullptr;
    Check(napi_get_last_error_info(env,&ei2)==napi_ok && ei2->error_code==napi_invalid_arg,"E reread still failure (no self-clear)");
    // Success: set_instance_data
    int data=42;
    Check(napi_set_instance_data(env,&data,FinB,nullptr)==napi_ok,"E set success");
    Check(napi_get_last_error_info(env,&ei)==napi_ok && ei->error_code==napi_ok,"E success clears last_error (AMB-STATUS-1)");
    // Another failure overwrites
    Check(napi_get_instance_data(env,nullptr)==napi_invalid_arg,"E second fail");
    Check(napi_get_last_error_info(env,&ei)==napi_ok && ei->error_code==napi_invalid_arg,"E second failure recorded");
    chakra_nodeapi_destroy_env(env);
    std::printf("phaseE (last-error clearing): done\n");
  }
  // --- Phase F: Missing symbols — is_exception_pending / get_and_clear ---
  {
    ResetLog();
    env = chakra_nodeapi_create_env(g_runtime,g_context,"gateF",nullptr,nullptr);
    Check(env!=nullptr,"F create");
    bool is_pending=true;
    Check(napi_is_exception_pending(env,&is_pending)==napi_ok && !is_pending,"F initially not pending");
    // Set pending via JsRun throw (avoids manual Error creation churn
    // which destabilized the probe's Type cache in earlier O3 runs).
    {
      JsValueRef src=JS_INVALID_REFERENCE;
      // JsCreateString with explicit length needs the JsValueRef form.
      const char* code = "throw new Error('boom')";
      Check(JsCreateString(code, strlen(code), &src)==JsNoError,"F create source");
      JsValueRef url=JS_INVALID_REFERENCE;
      JsCreateString("probe.js", strlen("probe.js"), &url);
      JsValueRef res=JS_INVALID_REFERENCE;
      JsErrorCode rc = JsRun(src, JS_SOURCE_CONTEXT_NONE, url, JsParseScriptAttributeNone, &res);
      bool hp=false; JsHasException(&hp);
      Check(hp || rc==JsErrorScriptException || rc==JsErrorInExceptionState,"F run throws (pending set)");
      // Source value is not rooted; callers keep no borrow across the call.
    }
    Check(napi_is_exception_pending(env,&is_pending)==napi_ok && is_pending,"F pending true after throw");
    // get_and_clear returns the exception (rooted) and clears
    napi_value cleared=nullptr;
    Check(napi_get_and_clear_last_exception(env,&cleared)==napi_ok && cleared!=nullptr,"F get_and_clear returns value");
    Check(napi_is_exception_pending(env,&is_pending)==napi_ok && !is_pending,"F cleared (no longer pending)");
    // Second get_and_clear with no pending -> ok + null
    napi_value second=(napi_value)(uintptr_t)0xDEAD;
    Check(napi_get_and_clear_last_exception(env,&second)==napi_ok && second==nullptr,"F second get null ok");
    // Pending discipline: lifetime gates are allowlisted (C Yes),
    // so they execute with pending.
    {
      JsValueRef src2=JS_INVALID_REFERENCE;
      const char* code2 = "throw new Error('again')";
      JsCreateString(code2, strlen(code2), &src2);
      JsValueRef url2=JS_INVALID_REFERENCE; JsCreateString("probe2.js", strlen("probe2.js"), &url2);
      JsValueRef res2=JS_INVALID_REFERENCE;
      JsRun(src2, JS_SOURCE_CONTEXT_NONE, url2, JsParseScriptAttributeNone, &res2);
      bool hp=false; JsHasException(&hp);
      Check(hp,"F second throw pending");
    }
    void* data=nullptr;
    // get_instance_data is Yes (execute with pending) -> should succeed even while pending, pending preserved
    Check(napi_get_instance_data(env,&data)==napi_ok,"F lifetime get executes with pending (C Yes)");
    Check(napi_is_exception_pending(env,&is_pending)==napi_ok && is_pending,"F pending preserved after get (allowlisted)");
    // Cleanup pending before destroy (else destroy's ContextGuard + JsHasException in entry guard might observe? Destroy does not check pending)
    Check(napi_get_and_clear_last_exception(env,&cleared)==napi_ok,"F cleanup pending");
    Check(napi_is_exception_pending(env,&is_pending)==napi_ok && !is_pending,"F clean before destroy");
    chakra_nodeapi_destroy_env(env);
    std::printf("phaseF (is_exception_pending/get_and_clear): done\n");
  }

  std::printf("gate_probe: %d checks, %d failures\n",g_checks,g_failures);
  ShutdownEngine();
  return g_failures==0?0:1;
}
