// Promise-pump probe (N0.3 / PR-8): exercises the host's continuation
// pump (install callback + drain until quiet). The addon registers a
// plain exports object; the JS driver creates a promise chain that only
// completes if the host pumps. Expected output: "promise: done".
//
// Kept as a separate addon/driver pair so the pump can be proved without
// provider promise APIs (N3.4): the driver uses engine promises only.

#include <node_api.h>

static napi_value Init(napi_env, napi_value exports) { return exports; }

NAPI_MODULE(promise_probe, Init)
