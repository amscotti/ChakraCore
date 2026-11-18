//-------------------------------------------------------------------------------------------------------
// Copyright (c) ChakraCore Project Contributors. All rights reserved.
// Licensed under the MIT license. See LICENSE.txt file in the project root for full license information.
//-------------------------------------------------------------------------------------------------------
// Version-rejection probe (N0.3 / Q1.1 negative case): this addon requests
// N-API v11 (> provider max 10, plan D1). The host must reject it with a
// clean error (exit 1, one stderr line naming the version) and never
// call its register function. If the host is buggy, the register function
// prints a second line that the proof asserts as absent.
//
// Built --NAPI_VERSION=11 right, but the version symbol is written by hand
// (NAPI_MODULE would stamp 10): this simulates an addon compiled against a
// newer header.

#include <node_api.h>
#include <cstdio>

static napi_value Init(napi_env, napi_value exports) {
  std::fprintf(stdout, "version_reject register called (BUG)\n");
  std::fflush(stdout);
  return exports;
}

extern "C" {
NAPI_MODULE_EXPORT int32_t node_api_module_get_api_version_v1() {
  return 11;
}
NAPI_MODULE_EXPORT napi_value napi_register_module_v1(napi_env env,
                                                      napi_value exports) {
  return Init(env, exports);
}
} /* extern "C" */
