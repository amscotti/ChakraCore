//-------------------------------------------------------------------------------------------------------
// Copyright (c) ChakraCore Project Contributors. All rights reserved.
// Licensed under the MIT license. See LICENSE.txt file in the project root for full license information.
//-------------------------------------------------------------------------------------------------------
//
// Debug-build Recycler JsAddRef/JsRelease pairing verification probe.
// Exercises napi_run_script and napi_create_async_work(..., undefined, ...)
// linked against debug-static libChakraCoreStatic.a (assertions enabled)
// to verify zero Debug Assert(false) at Recycler.cpp:649.

#include "NodeApiHostApi.h"
#include "js_native_api.h"
#include "node_api.h"
#include "ChakraCommon.h"
#include "ChakraCore.h"

#include <atomic>
#include <cstdio>
#include <deque>
#include <mutex>

namespace {

int g_checks = 0;
int g_failures = 0;

void Check(bool ok, const char* what) {
  g_checks++;
  if (!ok) {
    g_failures++;
    std::fprintf(stderr, "FAIL: %s\n", what);
  }
}

struct Task {
  void (*fn)(void*) = nullptr;
  void* arg = nullptr;
};

struct HostState {
  std::mutex m;
  std::deque<Task> tasks;
  std::atomic<int> loop_refs{0};
};

bool HostPostTask(void* ctx, void (*fn)(void*), void* arg) {
  auto* h = static_cast<HostState*>(ctx);
  std::lock_guard<std::mutex> lk(h->m);
  h->tasks.push_back(Task{fn, arg});
  return true;
}

bool HostPostWork(void* ctx, void (*execute)(void*), void (*complete)(void*),
                  void* arg) {
  if (execute) execute(arg);
  return HostPostTask(ctx, complete, arg);
}

void HostCancelWork(void* ctx, void* arg) {
  (void)ctx;
  (void)arg;
}

void HostRefLoop(void* ctx) {
  static_cast<HostState*>(ctx)->loop_refs.fetch_add(1, std::memory_order_relaxed);
}

void HostUnrefLoop(void* ctx) {
  static_cast<HostState*>(ctx)->loop_refs.fetch_sub(1, std::memory_order_relaxed);
}

void PumpTasks(HostState* h) {
  for (;;) {
    Task t{};
    {
      std::lock_guard<std::mutex> lk(h->m);
      if (h->tasks.empty()) break;
      t = h->tasks.front();
      h->tasks.pop_front();
    }
    if (t.fn) t.fn(t.arg);
  }
}

struct AsyncWorkCtx {
  std::atomic<int> executed{0};
  std::atomic<int> completed{0};
};

void ExecuteCb(napi_env, void* data) {
  static_cast<AsyncWorkCtx*>(data)->executed.fetch_add(1, std::memory_order_relaxed);
}

void CompleteCb(napi_env env, napi_status status, void* data) {
  (void)env;
  if (status == napi_ok) {
    static_cast<AsyncWorkCtx*>(data)->completed.fetch_add(1, std::memory_order_relaxed);
  }
}

}  // namespace

int main() {
  JsRuntimeHandle rt = JS_INVALID_RUNTIME_HANDLE;
  JsContextRef ctx = JS_INVALID_REFERENCE;
  if (JsCreateRuntime(JsRuntimeAttributeNone, nullptr, &rt) != JsNoError) {
    std::fprintf(stderr, "FAIL: JsCreateRuntime\n");
    return 2;
  }
  if (JsCreateContext(rt, &ctx) != JsNoError ||
      JsSetCurrentContext(ctx) != JsNoError) {
    JsDisposeRuntime(rt);
    std::fprintf(stderr, "FAIL: JsCreateContext / JsSetCurrentContext\n");
    return 2;
  }

  HostState host;
  chakra_nodeapi_host_services svc{};
  svc.post_task = &HostPostTask;
  svc.post_work = &HostPostWork;
  svc.cancel_work = &HostCancelWork;
  svc.ref_loop = &HostRefLoop;
  svc.unref_loop = &HostUnrefLoop;

  napi_env env = chakra_nodeapi_create_env(rt, ctx, "debug_jsrelease", &svc, &host);
  Check(env != nullptr, "create env");
  if (env != nullptr) {
    napi_handle_scope scope = nullptr;
    Check(napi_open_handle_scope(env, &scope) == napi_ok && scope != nullptr,
          "open handle scope");

    // Exercise napi_run_script (hits NodeApiScript.cpp sourceUrl pinning).
    for (int i = 0; i < 8; ++i) {
      napi_value code = nullptr;
      napi_value out = nullptr;
      double d = 0.0;
      Check(napi_create_string_utf8(env, "21 + 21", NAPI_AUTO_LENGTH, &code) == napi_ok,
            "create script string");
      Check(napi_run_script(env, code, &out) == napi_ok, "napi_run_script");
      Check(napi_get_value_double(env, out, &d) == napi_ok && d == 42.0,
            "napi_run_script value");
      JsCollectGarbage(rt);
    }

    // Exercise napi_create_async_work with undefined resource / resource_name
    // (hits NodeApiAsync.cpp CheckResourceObject & CheckResourceString).
    napi_value undef = nullptr;
    napi_value str_name = nullptr;
    napi_value obj_res = nullptr;
    Check(napi_get_undefined(env, &undef) == napi_ok, "get undefined");
    Check(napi_create_string_utf8(env, "dbg-work", NAPI_AUTO_LENGTH, &str_name) == napi_ok,
          "create resource name");
    Check(napi_create_object(env, &obj_res) == napi_ok, "create resource obj");

    // 1) resource_object = undefined, resource_name = string -> object_expected
    //    (JsConvertValueToObject produces pending TypeError without unmatched JsRelease).
    napi_async_work w_bad_obj = nullptr;
    Check(napi_create_async_work(env, undef, str_name, ExecuteCb, CompleteCb,
                                 nullptr, &w_bad_obj) == napi_object_expected,
          "create_async_work(..., undefined_obj, str)");
    napi_value pending_ex = nullptr;
    (void)napi_get_and_clear_last_exception(env, &pending_ex);

    // 2) resource_object = obj, resource_name = undefined -> coerces via
    //    JsConvertValueToString("undefined") without unmatched JsRelease.
    AsyncWorkCtx work_ctx;
    napi_async_work w_undef_name = nullptr;
    Check(napi_create_async_work(env, obj_res, undef, ExecuteCb, CompleteCb,
                                 &work_ctx, &w_undef_name) == napi_ok,
          "create_async_work(..., obj, undefined_name)");
    if (w_undef_name != nullptr) {
      Check(napi_queue_async_work(env, w_undef_name) == napi_ok, "queue work");
      PumpTasks(&host);
      Check(work_ctx.executed.load() == 1 && work_ctx.completed.load() == 1,
            "async work executed & completed");
      Check(napi_delete_async_work(env, w_undef_name) == napi_ok, "delete work");
    }

    // 3) resource_object = undefined directly as both args (object_expected).
    Check(napi_create_async_work(env, undef, undef, ExecuteCb, CompleteCb,
                                 nullptr, &w_bad_obj) == napi_object_expected,
          "create_async_work(..., undefined, undefined)");
    (void)napi_get_and_clear_last_exception(env, &pending_ex);

    // 4) resource_object = number (primitives convert to boxed object cleanly
    //    via JsConvertValueToObject without unmatched JsRelease).
    napi_value num_res = nullptr;
    napi_async_work w_num_obj = nullptr;
    Check(napi_create_int32(env, 7, &num_res) == napi_ok, "create int32");
    Check(napi_create_async_work(env, num_res, undef, ExecuteCb, CompleteCb,
                                 &work_ctx, &w_num_obj) == napi_ok,
          "create_async_work(..., int32, undefined)");
    if (w_num_obj != nullptr) {
      Check(napi_delete_async_work(env, w_num_obj) == napi_ok, "delete num work");
    }

    JsCollectGarbage(rt);
    Check(napi_close_handle_scope(env, scope) == napi_ok, "close handle scope");
    chakra_nodeapi_destroy_env(env);
  }

  JsSetCurrentContext(JS_INVALID_REFERENCE);
  JsDisposeRuntime(rt);
  std::printf("DEBUG_JSRELEASE_PROBE: checks=%d failures=%d %s\n",
              g_checks, g_failures, g_failures == 0 ? "PASS" : "FAIL");
  return g_failures == 0 ? 0 : 1;
}
