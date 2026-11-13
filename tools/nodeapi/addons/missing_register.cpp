// Missing-symbol probe (N0.3 negative case): this addon exports neither
// napi_register_module_v1 nor node_api_module_get_api_version_v1. The
// host must report a clean error (exit 1, stderr names the missing
// register symbol) and never create an env. It exercises the dlsym
// absence path, not a crash.

extern "C" int some_unrelated_export() { return 42; }
