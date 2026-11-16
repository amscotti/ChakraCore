// JS-driven async_work + threadsafe_function + pump-timeout probe addon.
// Built as a standalone .node module and executed via ch-nodeapi.

#include <node_api.h>

#include <atomic>
#include <chrono>
#include <cstring>
#include <thread>
#include <vector>

namespace {

struct AsyncDoubleData {
  napi_async_work work = nullptr;
  napi_deferred deferred = nullptr;
  int32_t input = 0;
  int32_t output = 0;
  std::atomic<bool> executed{false};
};

void ExecuteDouble(napi_env, void* raw) {
  auto* d = static_cast<AsyncDoubleData*>(raw);
  std::this_thread::sleep_for(std::chrono::milliseconds(10));
  d->output = d->input * 2;
  d->executed.store(true, std::memory_order_release);
}

void CompleteDouble(napi_env env, napi_status status, void* raw) {
  auto* d = static_cast<AsyncDoubleData*>(raw);
  if (status == napi_cancelled) {
    napi_value msg = nullptr;
    napi_create_string_utf8(env, "work cancelled", NAPI_AUTO_LENGTH, &msg);
    napi_value err = nullptr;
    napi_create_error(env, nullptr, msg, &err);
    napi_reject_deferred(env, d->deferred, err);
  } else if (status != napi_ok) {
    napi_value msg = nullptr;
    napi_create_string_utf8(env, "work failed", NAPI_AUTO_LENGTH, &msg);
    napi_value err = nullptr;
    napi_create_error(env, nullptr, msg, &err);
    napi_reject_deferred(env, d->deferred, err);
  } else {
    napi_value res = nullptr;
    napi_create_int32(env, d->output, &res);
    napi_resolve_deferred(env, d->deferred, res);
  }
  napi_delete_async_work(env, d->work);
  delete d;
}

napi_value AsyncDouble(napi_env env, napi_callback_info info) {
  size_t argc = 1;
  napi_value argv[1] = {nullptr};
  napi_get_cb_info(env, info, &argc, argv, nullptr, nullptr);
  int32_t val = 0;
  if (argc >= 1) napi_get_value_int32(env, argv[0], &val);

  auto* d = new AsyncDoubleData();
  d->input = val;
  napi_value promise = nullptr;
  if (napi_create_promise(env, &d->deferred, &promise) != napi_ok) {
    delete d;
    return nullptr;
  }
  napi_value name = nullptr;
  napi_create_string_utf8(env, "AsyncDouble", NAPI_AUTO_LENGTH, &name);
  if (napi_create_async_work(env, nullptr, name, ExecuteDouble, CompleteDouble,
                             d, &d->work) != napi_ok ||
      napi_queue_async_work(env, d->work) != napi_ok) {
    if (d->work) napi_delete_async_work(env, d->work);
    delete d;
    return nullptr;
  }
  return promise;
}

struct TsfnBatchContext {
  napi_threadsafe_function tsfn = nullptr;
  napi_deferred done_deferred = nullptr;
  int32_t count = 0;
  std::thread worker;
};

void CallJsInt(napi_env env, napi_value js_cb, void*, void* raw_val) {
  intptr_t v = reinterpret_cast<intptr_t>(raw_val);
  if (env == nullptr || js_cb == nullptr) return;
  napi_value undef = nullptr;
  napi_get_undefined(env, &undef);
  napi_value arg = nullptr;
  napi_create_int32(env, static_cast<int32_t>(v), &arg);
  napi_call_function(env, undef, js_cb, 1, &arg, nullptr);
}

void FinalizeTsfn(napi_env env, void* raw_fin, void*) {
  auto* ctx = static_cast<TsfnBatchContext*>(raw_fin);
  if (ctx->worker.joinable()) ctx->worker.join();
  if (env != nullptr && ctx->done_deferred != nullptr) {
    napi_value done = nullptr;
    napi_get_boolean(env, true, &done);
    napi_resolve_deferred(env, ctx->done_deferred, done);
  }
  delete ctx;
}

napi_value RunTsfnStream(napi_env env, napi_callback_info info) {
  size_t argc = 2;
  napi_value argv[2] = {nullptr, nullptr};
  napi_get_cb_info(env, info, &argc, argv, nullptr, nullptr);
  if (argc < 2) {
    napi_throw_type_error(env, nullptr, "expected (cb, count)");
    return nullptr;
  }
  int32_t count = 0;
  napi_get_value_int32(env, argv[1], &count);

  auto* ctx = new TsfnBatchContext();
  ctx->count = count;
  napi_value promise = nullptr;
  if (napi_create_promise(env, &ctx->done_deferred, &promise) != napi_ok) {
    delete ctx;
    return nullptr;
  }
  napi_value name = nullptr;
  napi_create_string_utf8(env, "TsfnStream", NAPI_AUTO_LENGTH, &name);
  if (napi_create_threadsafe_function(env, argv[0], nullptr, name, 0, 1, ctx,
                                      FinalizeTsfn, nullptr, CallJsInt,
                                      &ctx->tsfn) != napi_ok) {
    delete ctx;
    return nullptr;
  }
  ctx->worker = std::thread([ctx]() {
    for (int32_t i = 1; i <= ctx->count; ++i) {
      napi_call_threadsafe_function(
          ctx->tsfn, reinterpret_cast<void*>(static_cast<intptr_t>(i)),
          napi_tsfn_blocking);
    }
    napi_release_threadsafe_function(ctx->tsfn, napi_tsfn_release);
  });
  return promise;
}

// Intentionally leaks a kept-alive (referenced) TSFN so --pump-timeout-ms can
// be tested against an active host wait without hanging forever.
napi_value HoldLoopRefForever(napi_env env, napi_callback_info) {
  napi_value name = nullptr;
  napi_create_string_utf8(env, "HoldRef", NAPI_AUTO_LENGTH, &name);
  napi_value fn = nullptr;
  napi_create_function(env, "noop", NAPI_AUTO_LENGTH,
                       [](napi_env e, napi_callback_info) -> napi_value {
                         napi_value u = nullptr;
                         napi_get_undefined(e, &u);
                         return u;
                       },
                       nullptr, &fn);
  napi_threadsafe_function tsfn = nullptr;
  napi_create_threadsafe_function(env, fn, nullptr, name, 0, 1, nullptr,
                                  nullptr, nullptr, nullptr, &tsfn);
  napi_value undef = nullptr;
  napi_get_undefined(env, &undef);
  return undef;
}

napi_value Init(napi_env env, napi_value exports) {
  napi_property_descriptor descs[] = {
      {"asyncDouble", nullptr, AsyncDouble, nullptr, nullptr, nullptr,
       napi_default, nullptr},
      {"runTsfnStream", nullptr, RunTsfnStream, nullptr, nullptr, nullptr,
       napi_default, nullptr},
      {"holdLoopRefForever", nullptr, HoldLoopRefForever, nullptr, nullptr,
       nullptr, napi_default, nullptr},
  };
  napi_define_properties(env, exports, sizeof(descs) / sizeof(descs[0]), descs);
  return exports;
}

}  // namespace

NAPI_MODULE(host_async_addon, Init)
