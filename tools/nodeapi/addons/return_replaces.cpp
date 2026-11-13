// Return-replaces-exports probe (N0.3): Init creates a fresh object,
// sets a property on it, and returns THAT object instead of the passed
// exports. The host must bind the returned object (plan §1.3). The
// driver prints "replaced: yes" iff the replacement is visible; if the
// host wrongly kept the original exports, the driver throws (exit 1).
// Built against vendored headers at NAPI_VERSION=8.

#include <node_api.h>

static napi_value Init(napi_env env, napi_value) {
  napi_value fresh;
  napi_create_object(env, &fresh);
  napi_value msg;
  napi_create_string_utf8(env, "yes", NAPI_AUTO_LENGTH, &msg);
  napi_set_named_property(env, fresh, "status", msg);
  return fresh;
}

NAPI_MODULE(return_replaces, Init)
