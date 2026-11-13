//-------------------------------------------------------------------------------------------------------
// Copyright (c) ChakraCore Project Contributors. All rights reserved.
// Licensed under the MIT license. See LICENSE.txt file in the project root for full license information.
//-------------------------------------------------------------------------------------------------------
//
// N5.4 gc-hook proof addon: wraps a native counter in an external on
// each `create()` call, exposed to JS as `create()` / `count()` so the
// JS driver can prove the host `globalThis.gc()` hook runs a real GC
// (finalizers drain after collection, exactly once each).

#include <node_api.h>

static int g_outer = 0;

static void Fin(napi_env, void* data, void* hint) {
  (void)hint;
  int* c = static_cast<int*>(data);
  if (c) (*c)++;
}

static napi_value Create(napi_env env, napi_callback_info) {
  napi_value ext = nullptr;
  if (napi_create_external(env, &g_outer, Fin, nullptr, &ext) != napi_ok)
    return nullptr;
  return ext;
}

static napi_value Count(napi_env env, napi_callback_info) {
  /* Self-drain first: wrap-TU entries drain the GC finalizer queue on
   * entry (plan §3.7), so this tag check runs any finalizers enqueued
   * by a preceding gc() before the counter is read. Without it the
   * read observes the pre-drain count (one entry behind). */
  napi_value probe = nullptr;
  napi_create_object(env, &probe);
  napi_type_tag tag{0, 0};
  bool tagged = false;
  (void)napi_check_object_type_tag(env, probe, &tag, &tagged);
  probe = nullptr;
  napi_value v = nullptr;
  napi_create_int32(env, g_outer, &v);
  return v;
}

static napi_value Init(napi_env env, napi_value exports) {
  napi_value fn = nullptr;
  napi_create_function(env, "create", NAPI_AUTO_LENGTH, Create, nullptr,
                       &fn);
  napi_set_named_property(env, exports, "create", fn);
  napi_value fn2 = nullptr;
  napi_create_function(env, "count", NAPI_AUTO_LENGTH, Count, nullptr, &fn2);
  napi_set_named_property(env, exports, "count", fn2);
  return exports;
}

NAPI_MODULE(gc_hook_addon, Init)
