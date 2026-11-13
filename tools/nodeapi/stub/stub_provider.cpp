//-------------------------------------------------------------------------------------------------------
// Copyright (c) ChakraCore Project Contributors. All rights reserved.
// Licensed under the MIT license. See LICENSE.txt file in the project root for full license information.
//-------------------------------------------------------------------------------------------------------
//
// TEST-ONLY STUB PROVIDER for N0.3 loader mechanics. NOT libChakraNodeApi.
//
// The real provider (lib/NodeApi/, Wave 1+) is not implemented yet. This stub
// exists only so the host loader (tools/nodeapi/ch-nodeapi.cpp) can prove:
// dlopen RTLD_GLOBAL, version negotiation, registration scope, return-replaces-
// exports, global binding, JsRun, promise pumping, and env-before-runtime
// teardown, without waiting for the full N1 provider.
//
// Scope: exports napi_get_version (=8) + a tiny JsRT-backed subset sufficient
// to register the hello-world addon (create_string_utf8 / create_object /
// create_function / get_cb_info / set_named_property / get_named_property /
// create_double/int32/uint32 / get_global/undefined/null/boolean / handle
// scopes + escape / get_new_target). Every OTHER napi_* returns
// napi_generic_failure (D7). It records no per-env state beyond
// (runtime, context), roots nothing in provider scopes (scope handles are
// dummies), and ignores host services. It is NOT the real provider and must
// never be shipped as one.
//
// License/ABI: it implements the vendored Node-API v8 C ABI (NAPI_VERSION=8)
// declarations by including the vendored headers. Do not add <v8.h>/<node.h>/uv.h.

#include "node_api.h"
#include "NodeApiHostApi.h"
#include "ChakraCommon.h"
#include "ChakraCore.h"

#include <cstdint>
#include <cstring>
#include <new>

// ---------------------------------------------------------------------------
// Stub env: provider-allocator struct holding the host-owned handles.
// ---------------------------------------------------------------------------

namespace {

struct StubEnv {
  JsRuntimeHandle runtime;
  JsContextRef context;
};

StubEnv* to_stub(napi_env env) {
  return reinterpret_cast<StubEnv*>(env);
}

// Context enter/exit discipline (plan §3.2): every entry validates env,
// enters its context, restores the previous on exit. The test host keeps
// the context current for the whole run, so this is normally a no-op,
// but the stub still implements it so the contract is exercised.

JsContextRef enter(StubEnv* s) {
  JsContextRef prev = JS_INVALID_REFERENCE;
  JsGetCurrentContext(&prev);
  if (s->context != prev) JsSetCurrentContext(s->context);
  return prev;
}

void leave(JsContextRef prev) {
  JsContextRef cur = JS_INVALID_REFERENCE;
  JsGetCurrentContext(&cur);
  if (cur != prev) JsSetCurrentContext(prev);
}

napi_status map_js(JsErrorCode err) {
  if (err == JsNoError) return napi_ok;
  if (err == JsErrorInExceptionState || err == JsErrorScriptException)
    return napi_pending_exception;
  return napi_generic_failure;
}

// Minimal callback-info record (mirrors the opaque napi_callback_info__).
// The stub owns one per native invocation on the stack; napi_get_cb_info
// reads back from it. Never escape this pointer beyond the call.
struct StubCallbackInfo {
  size_t argc;
  napi_value* argv;
  napi_value this_arg;
  void* data;
  napi_value new_target; /* NULL = non-construct */
};

JsValueRef js_cb(JsValueRef callee, bool is_construct, JsValueRef* args,
                 unsigned short count, void* state);

struct StubCallbackState {
  napi_env env;
  napi_callback cb;
  void* data;
  napi_value* argv; /* owned copy, freed after call */
  size_t argc;
  napi_value this_arg;
};

} /* namespace */

struct napi_callback_info__;

// This translation unit defines the layout behind the opaque handle.
struct napi_callback_info__ {
  size_t argc;
  napi_value* argv;
  napi_value this_arg;
  void* data;
  napi_value new_target;
};

namespace {

JsValueRef js_cb(JsValueRef callee, bool is_construct, JsValueRef* args,
                 unsigned short count, void* state) {
  auto* s = static_cast<StubCallbackState*>(state);
  // JsCreateFunction gives arguments[0] == thisArg, argumentCount includes it.
  napi_value this_arg = nullptr;
  size_t argc = 0;
  if (count > 0 && args) {
    this_arg = reinterpret_cast<napi_value>(args[0]);
    argc = static_cast<size_t>(count) - 1;
  }
  napi_callback_info__ info;
  info.argc = argc;
  info.argv = reinterpret_cast<napi_value*>(args ? args + 1 : nullptr);
  info.this_arg = this_arg;
  info.data = s->data;
  info.new_target = is_construct ? reinterpret_cast<napi_value>(callee) : nullptr;
  napi_value ret = s->cb(s->env, reinterpret_cast<napi_callback_info>(&info));
  return reinterpret_cast<JsValueRef>(ret);
}

} /* namespace */

extern "C" {

// ---- Private host API (this stub's create/destroy) ----

napi_env chakra_nodeapi_create_env(JsRuntimeHandle runtime,
                                   JsContextRef context,
                                   const char*,
                                   const chakra_nodeapi_host_services*,
                                   void*) {
  if (!runtime || !context) return nullptr;
  StubEnv* e = new (std::nothrow) StubEnv{runtime, context};
  return reinterpret_cast<napi_env>(e);
}

void chakra_nodeapi_destroy_env(napi_env env) {
  auto* e = to_stub(env);
  delete e;
}

// ---- Stubbed napi_* subset ----

napi_status NAPI_CDECL napi_get_version(node_api_basic_env env,
                                        uint32_t* result) {
  if (!env || !result) return napi_invalid_arg;
  *result = 8;
  return napi_ok;
}

napi_status NAPI_CDECL napi_open_handle_scope(napi_env env,
                                              napi_handle_scope* result) {
  if (!env || !result) return napi_invalid_arg;
  StubEnv* s = to_stub(env);
  JsContextRef prev = enter(s);
  (void)prev;
  // Dummy scope: the host passes it back to close. Roots nothing.
  *result = reinterpret_cast<napi_handle_scope>(s);
  leave(prev);
  return napi_ok;
}

napi_status NAPI_CDECL napi_close_handle_scope(napi_env env,
                                               napi_handle_scope scope) {
  if (!env || !scope) return napi_invalid_arg;
  return napi_ok;
}

napi_status NAPI_CDECL napi_open_escapable_handle_scope(
    napi_env env, napi_escapable_handle_scope* result) {
  if (!env || !result) return napi_invalid_arg;
  StubEnv* s = to_stub(env);
  JsContextRef prev = enter(s);
  (void)prev;
  *result = reinterpret_cast<napi_escapable_handle_scope>(s);
  leave(prev);
  return napi_ok;
}

napi_status NAPI_CDECL napi_close_escapable_handle_scope(
    napi_env env, napi_escapable_handle_scope scope) {
  if (!env || !scope) return napi_invalid_arg;
  return napi_ok;
}

napi_status NAPI_CDECL napi_escape_handle(napi_env env,
                                          napi_escapable_handle_scope scope,
                                          napi_value escapee,
                                          napi_value* result) {
  if (!env || !scope || !escapee || !result) return napi_invalid_arg;
  *result = escapee;
  return napi_ok;
}

napi_status NAPI_CDECL napi_get_undefined(napi_env env, napi_value* result) {
  if (!env || !result) return napi_invalid_arg;
  JsContextRef prev = enter(to_stub(env));
  JsValueRef v = JS_INVALID_REFERENCE;
  JsErrorCode e = JsGetUndefinedValue(&v);
  leave(prev);
  *result = reinterpret_cast<napi_value>(v);
  return map_js(e);
}

napi_status NAPI_CDECL napi_get_null(napi_env env, napi_value* result) {
  if (!env || !result) return napi_invalid_arg;
  JsContextRef prev = enter(to_stub(env));
  JsValueRef v = JS_INVALID_REFERENCE;
  JsErrorCode e = JsGetNullValue(&v);
  leave(prev);
  *result = reinterpret_cast<napi_value>(v);
  return map_js(e);
}

napi_status NAPI_CDECL napi_get_global(napi_env env, napi_value* result) {
  if (!env || !result) return napi_invalid_arg;
  JsContextRef prev = enter(to_stub(env));
  JsValueRef v = JS_INVALID_REFERENCE;
  JsErrorCode e = JsGetGlobalObject(&v);
  leave(prev);
  *result = reinterpret_cast<napi_value>(v);
  return map_js(e);
}

napi_status NAPI_CDECL napi_get_boolean(napi_env env, bool value,
                                        napi_value* result) {
  if (!env || !result) return napi_invalid_arg;
  JsContextRef prev = enter(to_stub(env));
  JsValueRef v = JS_INVALID_REFERENCE;
  JsErrorCode e = JsBoolToBoolean(value, &v);
  leave(prev);
  *result = reinterpret_cast<napi_value>(v);
  return map_js(e);
}

napi_status NAPI_CDECL napi_create_object(napi_env env, napi_value* result) {
  if (!env || !result) return napi_invalid_arg;
  JsContextRef prev = enter(to_stub(env));
  JsValueRef v = JS_INVALID_REFERENCE;
  JsErrorCode e = JsCreateObject(&v);
  leave(prev);
  if (e == JsNoError) *result = reinterpret_cast<napi_value>(v);
  return map_js(e);
}

napi_status NAPI_CDECL napi_create_string_utf8(napi_env env, const char* str,
                                               size_t length,
                                               napi_value* result) {
  if (!env || !str || !result) return napi_invalid_arg;
  if (length == NAPI_AUTO_LENGTH) length = std::strlen(str);
  if (length > static_cast<size_t>(-1) / 2) return napi_invalid_arg;
  JsContextRef prev = enter(to_stub(env));
  JsValueRef v = JS_INVALID_REFERENCE;
  JsErrorCode e = JsCreateString(str, length, &v);
  leave(prev);
  if (e == JsNoError) *result = reinterpret_cast<napi_value>(v);
  return map_js(e);
}

napi_status NAPI_CDECL napi_create_string_utf16(napi_env env,
                                                const char16_t* str,
                                                size_t length,
                                                napi_value* result) {
  if (!env || !str || !result) return napi_invalid_arg;
  if (length == NAPI_AUTO_LENGTH) {
    length = 0;
    while (str[length] != 0) length++;
  }
  JsContextRef prev = enter(to_stub(env));
  JsValueRef v = JS_INVALID_REFERENCE;
  JsErrorCode e = JsCreateStringUtf16(
      reinterpret_cast<const uint16_t*>(str), length, &v);
  leave(prev);
  if (e == JsNoError) *result = reinterpret_cast<napi_value>(v);
  return map_js(e);
}

napi_status NAPI_CDECL napi_create_string_latin1(napi_env env, const char* str,
                                                 size_t length,
                                                 napi_value* result) {
  if (!env || !str || !result) return napi_invalid_arg;
  if (length == NAPI_AUTO_LENGTH) length = std::strlen(str);
  JsContextRef prev = enter(to_stub(env));
  JsValueRef v = JS_INVALID_REFERENCE;
  JsErrorCode e = JsCreateString(str, length, &v);
  leave(prev);
  if (e == JsNoError) *result = reinterpret_cast<napi_value>(v);
  return map_js(e);
}

napi_status NAPI_CDECL napi_create_double(napi_env env, double value,
                                          napi_value* result) {
  if (!env || !result) return napi_invalid_arg;
  JsContextRef prev = enter(to_stub(env));
  JsValueRef v = JS_INVALID_REFERENCE;
  JsErrorCode e = JsDoubleToNumber(value, &v);
  leave(prev);
  if (e == JsNoError) *result = reinterpret_cast<napi_value>(v);
  return map_js(e);
}

napi_status NAPI_CDECL napi_create_int32(napi_env env, int32_t value,
                                         napi_value* result) {
  if (!env || !result) return napi_invalid_arg;
  JsContextRef prev = enter(to_stub(env));
  JsValueRef v = JS_INVALID_REFERENCE;
  JsErrorCode e = JsIntToNumber(value, &v);
  leave(prev);
  if (e == JsNoError) *result = reinterpret_cast<napi_value>(v);
  return map_js(e);
}

napi_status NAPI_CDECL napi_create_uint32(napi_env env, uint32_t value,
                                          napi_value* result) {
  if (!env || !result) return napi_invalid_arg;
  JsContextRef prev = enter(to_stub(env));
  JsValueRef v = JS_INVALID_REFERENCE;
  JsErrorCode e = JsDoubleToNumber(static_cast<double>(value), &v);
  leave(prev);
  if (e == JsNoError) *result = reinterpret_cast<napi_value>(v);
  return map_js(e);
}

napi_status NAPI_CDECL napi_create_function(napi_env env, const char* utf8name,
                                            size_t length, napi_callback cb,
                                            void* data, napi_value* result) {
  if (!env || !utf8name || !cb || !result) return napi_invalid_arg;
  if (length == NAPI_AUTO_LENGTH) length = std::strlen(utf8name);
  (void)length;
  StubEnv* s = to_stub(env);
  JsContextRef prev = enter(s);
  // The native callback state outlives the function only via the
  // provider's function-ownership contract; the stub leaks it by design
  // (test-only, process exits after one run). The real provider owns
  // this record via the §3.7 trampoline instead.
  auto* state = new (std::nothrow) StubCallbackState{env, cb, data, nullptr, 0,
                                                     nullptr};
  if (!state) {
    leave(prev);
    return napi_generic_failure;
  }
  JsValueRef v = JS_INVALID_REFERENCE;
  JsErrorCode e = JsCreateFunction(js_cb, state, &v);
  leave(prev);
  if (e == JsNoError) *result = reinterpret_cast<napi_value>(v);
  else delete state;
  return map_js(e);
}

napi_status NAPI_CDECL napi_get_cb_info(napi_env env,
                                        napi_callback_info cbinfo,
                                        size_t* argc, napi_value* argv,
                                        napi_value* this_arg, void** data) {
  if (!env || !cbinfo || !argc) return napi_invalid_arg;
  auto* info = reinterpret_cast<napi_callback_info__*>(cbinfo);
  size_t user_cap = argc ? *argc : 0;
  size_t n = info->argc < user_cap ? info->argc : user_cap;
  if (argv && n > 0) std::memcpy(argv, info->argv, n * sizeof(napi_value));
  *argc = info->argc;
  if (this_arg) *this_arg = info->this_arg;
  if (data) *data = info->data;
  return napi_ok;
}

napi_status NAPI_CDECL napi_get_new_target(napi_env env,
                                           napi_callback_info cbinfo,
                                           napi_value* result) {
  if (!env || !cbinfo || !result) return napi_invalid_arg;
  auto* info = reinterpret_cast<napi_callback_info__*>(cbinfo);
  *result = info->new_target;
  return napi_ok;
}

napi_status NAPI_CDECL napi_set_named_property(napi_env env, napi_value object,
                                               const char* utf8name,
                                               napi_value value) {
  if (!env || !object || !utf8name || !value) return napi_invalid_arg;
  JsContextRef prev = enter(to_stub(env));
  JsPropertyIdRef pid = JS_INVALID_REFERENCE;
  JsValueRef obj = reinterpret_cast<JsValueRef>(object);
  JsValueRef val = reinterpret_cast<JsValueRef>(value);
  JsErrorCode e = JsCreatePropertyId(utf8name, std::strlen(utf8name), &pid);
  if (e != JsNoError) { leave(prev); return map_js(e); }
  e = JsSetProperty(obj, pid, val, false);
  leave(prev);
  return map_js(e);
}

napi_status NAPI_CDECL napi_get_named_property(napi_env env, napi_value object,
                                               const char* utf8name,
                                               napi_value* result) {
  if (!env || !object || !utf8name || !result) return napi_invalid_arg;
  JsContextRef prev = enter(to_stub(env));
  JsPropertyIdRef pid = JS_INVALID_REFERENCE;
  JsValueRef obj = reinterpret_cast<JsValueRef>(object);
  JsValueRef v = JS_INVALID_REFERENCE;
  JsErrorCode e = JsCreatePropertyId(utf8name, std::strlen(utf8name), &pid);
  if (e != JsNoError) { leave(prev); return map_js(e); }
  e = JsGetProperty(obj, pid, &v);
  leave(prev);
  if (e == JsNoError) *result = reinterpret_cast<napi_value>(v);
  return map_js(e);
}

// Everything else is genuinely absent until the Wave 1+ provider lands:
// addons referencing it fail at load (RTLD_NOW) or return a stub
// generic_failure where stubbed.

} /* extern "C" */

// The two non-napi_status symbols (plan §1.1): implement explicitly.
extern "C" {

NAPI_EXTERN void NAPI_CDECL napi_module_register(napi_module*) {
  // Deprecated registration path (N4.1). The stub has no pending-module
  // registry, so this is a no-op by design; addons using NAPI_MODULE
  // (symbol registration) are unaffected.
}

NAPI_EXTERN NAPI_NO_RETURN void NAPI_CDECL
napi_fatal_error(const char* location, size_t location_len,
                 const char* message, size_t message_len) {
  (void)location; (void)location_len; (void)message; (void)message_len;
  // No host fatal_exception service in the stub: abort is the documented
  // fallback (plan §3.2, fatal_exception NULL = abort policy).
  __builtin_abort();
}

} /* extern "C" */
