// Hello-world Node-API addon for the N0.3 loader proof (plan N0.3).
// Built against the vendored Node 22.22.3 headers at NAPI_VERSION=8
// (include/nodeapi/, PR-2). Exports a hello() function returning "world".
//
// This is the canonical node-addon-examples shape: NAPI_MODULE boilerplate
// emits node_api_module_get_api_version_v1 (=8) and napi_register_module_v1.
// The host (tools/nodeapi/ch-nodeapi) resolves both, negotiates the
// version, and calls the register function with a fresh exports object.

#include <node_api.h>

static napi_value Hello(napi_env env, napi_callback_info) {
  napi_value str;
  napi_create_string_utf8(env, "world", NAPI_AUTO_LENGTH, &str);
  return str;
}

static napi_value Init(napi_env env, napi_value exports) {
  napi_value fn;
  napi_create_function(env, "hello", NAPI_AUTO_LENGTH, Hello, nullptr, &fn);
  napi_set_named_property(env, exports, "hello", fn);
  return exports;
}

NAPI_MODULE(hello_world, Init)
