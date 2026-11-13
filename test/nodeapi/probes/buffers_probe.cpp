//-------------------------------------------------------------------------------------------------------
// Copyright (c) ChakraCore Project Contributors. All rights reserved.
// Licensed under the MIT license. See LICENSE.txt file in the project root for full license information.
//-------------------------------------------------------------------------------------------------------
//
// N4.2 Buffer probes (plan task N4.2, B2 group; §3.9 PR-6).
//
// Direct C++ probe: creates a Chakra runtime + context, a real env
// (chakra_nodeapi_create_env) and exercises lib/NodeApi/NodeApiBuffers.cpp
// in-process (no addon, no ch-nodeapi host). Run via
// run_buffers_probe.sh, which builds with `zig c++` through
// /tmp/opencode/cc-gate-lock.sh and links the engine libChakraCore.dylib
// produced by `mise exec -- zig build`.
//
// Coverage (each MUST fail on mismatch):
//   - napi_create_buffer: bytes written via returned data pointer are
//     visible from JS and vice versa (JS writes visible natively).
//   - napi_create_buffer_copy: copy is independent after mutation
//     (source mutation after copy does not affect copy; copy mutation
//     does not affect source).
//   - napi_create_external_buffer: finalizer fires exactly once on
//     collect (GC via JsCollectGarbage + next napi entry drain;
//     deferred until drain, exactly-once across repeated GC).
//   - napi_is_buffer: Buffer is true, plain Uint8Array is NOT a Buffer,
//     ArrayBuffer / object / number are not Buffers.
//   - napi_get_buffer_info: data ptr + length tuple correct for
//     create/copy/external (length == requested, data aliases storage).
//   - Marker hidden: the provider-private Symbol property is
//     non-enumerable/hidden (descriptor enumerable==false,
//     configurable==false, writable==false) and Object.keys does not
//     show it; plain Uint8Array has zero own Symbols.
//   - Documented non-conformances (PR-6): no JS Buffer global
//     (typeof Buffer === 'undefined'; `instanceof Buffer` throws
//     ReferenceError, never true).
//   - External finalizer never runs during GC (count is 0 right after
//     JsCollectGarbage, before any drain entry); it fires exactly once
//     at the next napi-entry drain.
//   - Root-count baselining (R-REF-14) and pending/Closing discipline
//     spot checks (creates gated, is/get execute).
//
// Rooting methodology: each case runs in its own handle scope; the probe
// asserts after every case the R-REF-14 total is back at baseline.
//
// Output: one line per assertion ("PASS <case>" / "FAIL <case> ..."),
// then "BUFFERS_PROBE: PASS (n/n)" exit 0, or "BUFFERS_PROBE: FAIL
// (k/n)" exit 1.

#include "NodeApiHostApi.h"
#include "js_native_api.h"
#include "node_api.h"
#include "NodeApiEnv.h"
#include "NodeApiScopes.h"
#include "ChakraCommon.h"
#include "ChakraCore.h"

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#ifndef CHAKRA_NODEAPI_TEST_HOOKS
#error "buffers_probe requires -DCHAKRA_NODEAPI_TEST_HOOKS (root counter)"
#endif

namespace {

int g_total = 0;
int g_pass = 0;

const char* status_name(napi_status s) {
  switch (s) {
    case napi_ok: return "ok";
    case napi_invalid_arg: return "invalid_arg";
    case napi_object_expected: return "object_expected";
    case napi_string_expected: return "string_expected";
    case napi_name_expected: return "name_expected";
    case napi_function_expected: return "function_expected";
    case napi_number_expected: return "number_expected";
    case napi_boolean_expected: return "boolean_expected";
    case napi_array_expected: return "array_expected";
    case napi_generic_failure: return "generic_failure";
    case napi_pending_exception: return "pending_exception";
    case napi_cancelled: return "cancelled";
    case napi_escape_called_twice: return "escape_called_twice";
    case napi_handle_scope_mismatch: return "handle_scope_mismatch";
    case napi_callback_scope_mismatch: return "callback_scope_mismatch";
    case napi_queue_full: return "queue_full";
    case napi_closing: return "closing";
    case napi_bigint_expected: return "bigint_expected";
    case napi_date_expected: return "date_expected";
    case napi_arraybuffer_expected: return "arraybuffer_expected";
    case napi_detachable_arraybuffer_expected: return "detachable_arraybuffer_expected";
    case napi_would_deadlock: return "would_deadlock";
    case napi_no_external_buffers_allowed: return "no_external_buffers_allowed";
    case napi_cannot_run_js: return "cannot_run_js";
  }
  return "?";
}

void expect(bool cond, const char* name) {
  g_total++;
  if (cond) {
    g_pass++;
    std::fprintf(stdout, "PASS %s\n", name);
  } else {
    std::fprintf(stdout, "FAIL %s\n", name);
  }
  std::fflush(stdout);
}

void expect_status(napi_status got, napi_status want, const char* name) {
  g_total++;
  if (got == want) {
    g_pass++;
    std::fprintf(stdout, "PASS %s (status=%s)\n", name, status_name(got));
  } else {
    std::fprintf(stdout, "FAIL %s (got=%s want=%s)\n", name, status_name(got),
                 status_name(want));
  }
  std::fflush(stdout);
}

JsRuntimeHandle g_rt = JS_INVALID_REFERENCE;
napi_env g_env = nullptr;

inline void ClearStack() {
  volatile char buf[1024];
  for (size_t i = 0; i < sizeof(buf); i++) buf[i] = 0;
}

void force_gc(JsRuntimeHandle rt) {
  for (int i = 0; i < 10; i++) {
    ClearStack();
    if (JsCollectGarbage(rt) != JsNoError) break;
  }
}

bool RunJSBool(const char* code) {
  JsValueRef script = JS_INVALID_REFERENCE;
  JsValueRef url = JS_INVALID_REFERENCE;
  if (JsCreateString(code, std::strlen(code), &script) != JsNoError)
    return false;
  JsCreateString("probe.js", 8, &url);
  JsAddRef(script, nullptr);
  JsValueRef result = JS_INVALID_REFERENCE;
  JsErrorCode e = JsRun(script, JS_SOURCE_CONTEXT_NONE, url,
                        JsParseScriptAttributeNone, &result);
  JsRelease(script, nullptr);
  JsRelease(url, nullptr);
  if (e != JsNoError) {
    bool has = false;
    if (JsHasException(&has) == JsNoError && has) {
      JsValueRef ex = JS_INVALID_REFERENCE;
      if (JsGetAndClearException(&ex) == JsNoError) {
        JsValueRef s = JS_INVALID_REFERENCE;
        if (JsConvertValueToString(ex, &s) == JsNoError && s) {
          size_t len = 0;
          JsCopyString(s, nullptr, 0, &len);
          std::vector<char> b(len + 1, '\0');
          size_t w = 0;
          JsCopyString(s, b.data(), b.size(), &w);
          std::fprintf(stderr, "JS threw: %.*s\n", (int)w, b.data());
        }
      }
    }
    return false;
  }
  JsAddRef(result, nullptr);
  JsValueType t = JsUndefined;
  if (JsGetValueType(result, &t) != JsNoError || t != JsBoolean) {
    JsRelease(result, nullptr);
    return false;
  }
  bool b = false;
  if (JsBooleanToBool(result, &b) != JsNoError) {
    JsRelease(result, nullptr);
    return false;
  }
  JsRelease(result, nullptr);
  return b;
}

void StoreGlobal(const char* name, napi_value v) {
  JsValueRef global = JS_INVALID_REFERENCE;
  JsGetGlobalObject(&global);
  JsPropertyIdRef pid = JS_INVALID_REFERENCE;
  JsCreatePropertyId(name, std::strlen(name), &pid);
  JsSetProperty(global, pid, reinterpret_cast<JsValueRef>(v), false);
}

void DeleteGlobal(const char* name) {
  JsValueRef global = JS_INVALID_REFERENCE;
  JsGetGlobalObject(&global);
  JsPropertyIdRef pid = JS_INVALID_REFERENCE;
  JsCreatePropertyId(name, std::strlen(name), &pid);
  JsValueRef dr = JS_INVALID_REFERENCE;
  JsDeleteProperty(global, pid, false, &dr);
}

napi_handle_scope OpenScope() {
  napi_handle_scope s = nullptr;
  napi_open_handle_scope(g_env, &s);
  return s;
}
void CloseScope(napi_handle_scope s) { napi_close_handle_scope(g_env, s); }

size_t RootCount() { return chakra_nodeapi_test_root_count(g_env); }
size_t Baseline() { return chakra_nodeapi_test_root_baseline(g_env); }

void AtBaseline(const char* where) {
  expect(RootCount() == Baseline(), where);
}

// External finalizer probe state
int g_ext_finalizer_calls = 0;
void* g_ext_finalizer_last_data = nullptr;
void* g_ext_finalizer_last_hint = nullptr;
void ExtFinalize(napi_env, void* data, void* hint) {
  g_ext_finalizer_calls++;
  g_ext_finalizer_last_data = data;
  g_ext_finalizer_last_hint = hint;
  // Free the external buffer (provider does not own it; finalizer owns).
  delete[] static_cast<char*>(data);
}

}  // namespace

int main() {
  JsRuntimeHandle rt = JS_INVALID_REFERENCE;
  if (JsCreateRuntime(JsRuntimeAttributeNone, nullptr, &rt) != JsNoError) {
    std::fprintf(stderr, "FATAL: JsCreateRuntime failed\n");
    return 2;
  }
  g_rt = rt;
  JsContextRef ctx = JS_INVALID_REFERENCE;
  if (JsCreateContext(rt, &ctx) != JsNoError) {
    std::fprintf(stderr, "FATAL: JsCreateContext failed\n");
    JsDisposeRuntime(rt);
    return 2;
  }
  if (JsSetCurrentContext(ctx) != JsNoError) {
    std::fprintf(stderr, "FATAL: JsSetCurrentContext failed\n");
    JsDisposeRuntime(rt);
    return 2;
  }
  napi_env env = chakra_nodeapi_create_env(rt, ctx, nullptr, nullptr, nullptr);
  if (!env) {
    std::fprintf(stderr, "FATAL: chakra_nodeapi_create_env failed\n");
    JsSetCurrentContext(JS_INVALID_REFERENCE);
    JsDisposeRuntime(rt);
    return 2;
  }
  g_env = env;
  size_t baseline = chakra_nodeapi_test_root_count(env);
  expect(baseline == 0, "baseline root count is zero");

  // ---- napi_create_buffer: visible both directions ----
  {
    std::fprintf(stdout, "-- case create-buffer-visible --\n");
    napi_handle_scope s = OpenScope();
    napi_value buf = nullptr;
    void* data = nullptr;
    expect_status(napi_create_buffer(env, 16, &data, &buf), napi_ok,
                  "create_buffer 16 ok");
    expect(buf != nullptr, "create_buffer non-null");
    expect(data != nullptr, "create_buffer data non-null for 16");
    bool isBuf = false;
    expect_status(napi_is_buffer(env, buf, &isBuf), napi_ok,
                  "is_buffer on Buffer ok");
    expect(isBuf == true, "is_buffer true for Buffer");
    // Native writes via data pointer -> JS visible.
    unsigned char* u8 = static_cast<unsigned char*>(data);
    u8[0] = 'H';
    u8[1] = 'i';
    u8[2] = 0xFF;
    StoreGlobal("__buf", buf);
    expect(RunJSBool("globalThis.__buf[0] == 72 && globalThis.__buf[1] == 105 && globalThis.__buf[2] == 255"),
           "JS sees native writes via Buffer data pointer");
    // JS writes -> native visible (fresh get_buffer_info).
    expect(RunJSBool("(function(){ globalThis.__buf[3]=0xAB; return globalThis.__buf[3]==171; })()"),
           "JS writes to Buffer element");
    void* infoData = nullptr;
    size_t infoLen = 0;
    expect_status(napi_get_buffer_info(env, buf, &infoData, &infoLen), napi_ok,
                  "get_buffer_info ok");
    expect(infoLen == 16, "get_buffer_info len 16");
    expect(infoData == data, "get_buffer_info data aliases create data");
    unsigned char* nu8 = static_cast<unsigned char*>(infoData);
    expect(nu8[0] == 'H' && nu8[1] == 'i' && nu8[2] == 0xFF,
           "native sees native writes via fresh info");
    expect(nu8[3] == 0xAB, "native sees JS writes via fresh info");
    // Typed-array info: Buffer is a Uint8Array.
    napi_typedarray_type taType = napi_int8_array;
    size_t taLen = 0;
    void* taData = nullptr;
    napi_value taAB = nullptr;
    size_t taOff = 999;
    expect_status(napi_get_typedarray_info(env, buf, &taType, &taLen, &taData, &taAB, &taOff),
                  napi_ok, "get_typedarray_info on Buffer ok");
    expect(taType == napi_uint8_array, "Buffer typedarray type uint8");
    expect(taLen == 16, "Buffer typedarray len 16");
    expect(taOff == 0, "Buffer byte_offset 0");
    expect(taData == data, "Buffer typedarray data aliases");
    bool isTA = false;
    napi_is_typedarray(env, buf, &isTA);
    expect(isTA, "is_typedarray true for Buffer");
    // Marker hidden: one own Symbol, enumerable false, not in Object.keys.
    expect(RunJSBool("(function(){ var syms = Object.getOwnPropertySymbols(globalThis.__buf); return syms.length===1; })()"),
           "Buffer has exactly one own Symbol (marker)");
    expect(RunJSBool("(function(){ var s = Object.getOwnPropertySymbols(globalThis.__buf)[0]; var d = Object.getOwnPropertyDescriptor(globalThis.__buf, s); return d.enumerable===false; })()"),
           "Marker enumerable false (hidden)");
    expect(RunJSBool("(function(){ var s = Object.getOwnPropertySymbols(globalThis.__buf)[0]; var d = Object.getOwnPropertyDescriptor(globalThis.__buf, s); return d.configurable===false; })()"),
           "Marker configurable false");
    expect(RunJSBool("(function(){ var s = Object.getOwnPropertySymbols(globalThis.__buf)[0]; var d = Object.getOwnPropertyDescriptor(globalThis.__buf, s); return d.writable===false; })()"),
           "Marker writable false");
    expect(RunJSBool("(function(){ var s = Object.getOwnPropertySymbols(globalThis.__buf)[0]; return globalThis.__buf[s]===true; })()"),
           "Marker value true");
    expect(RunJSBool("(function(){ return Object.keys(globalThis.__buf).every(function(k){ return /^[0-9]+$/.test(k); }); })()"),
           "Object.keys are numeric indices only (marker is a Symbol, hidden)");
    // Documented non-conformances (PR-6): no JS Buffer global exists, so
    // `instanceof Buffer` can never be true (it throws ReferenceError).
    expect(RunJSBool("typeof Buffer === 'undefined'"),
           "no Buffer global (PR-6 non-conformance)");
    expect(RunJSBool("(function(){ try { globalThis.__buf instanceof Buffer; return false; } catch (e) { return e instanceof ReferenceError; } })()"),
           "instanceof Buffer throws ReferenceError (no Buffer global)");
    DeleteGlobal("__buf");
    CloseScope(s);
    expect(RootCount() == Baseline(), "create-buffer at baseline");
  }

  {
    std::fprintf(stdout, "-- case create-buffer-zero --\n");
    napi_handle_scope s = OpenScope();
    napi_value buf = nullptr;
    void* data = reinterpret_cast<void*>(0xdeadbeef);
    expect_status(napi_create_buffer(env, 0, &data, &buf), napi_ok,
                  "create_buffer 0 ok");
    expect(buf != nullptr, "zero buf non-null");
    // Zero-length storage is nullptr.
    expect(data == nullptr, "zero data is nullptr");
    bool isBuf = false;
    expect_status(napi_is_buffer(env, buf, &isBuf), napi_ok, "zero is_buffer ok");
    expect(isBuf, "zero is_buffer true");
    void* infoData = reinterpret_cast<void*>(0xdeadbeef);
    size_t infoLen = 999;
    expect_status(napi_get_buffer_info(env, buf, &infoData, &infoLen), napi_ok,
                  "zero get_info ok");
    expect(infoLen == 0, "zero infoLen 0");
    expect(infoData == nullptr, "zero get_info data nullptr");
    CloseScope(s);
    expect(RootCount() == Baseline(), "zero at baseline");
  }

  // ---- napi_create_buffer_copy: independent after mutation ----
  {
    std::fprintf(stdout, "-- case create-buffer-copy --\n");
    napi_handle_scope s = OpenScope();
    const unsigned char srcBytes[] = {'H', 'e', 'l', 'l', 'o', '!', 0x00, 0xFF};
    napi_value buf = nullptr;
    void* resultData = nullptr;
    expect_status(napi_create_buffer_copy(env, sizeof(srcBytes), srcBytes,
                                          &resultData, &buf),
                  napi_ok, "create_buffer_copy ok");
    expect(buf != nullptr, "copy buf non-null");
    expect(resultData != nullptr, "copy result_data non-null");
    bool isBuf = false;
    napi_is_buffer(env, buf, &isBuf);
    expect(isBuf, "copy is_buffer true");
    void* infoData = nullptr;
    size_t infoLen = 0;
    expect_status(napi_get_buffer_info(env, buf, &infoData, &infoLen), napi_ok,
                  "copy get_info ok");
    expect(infoLen == sizeof(srcBytes), "copy infoLen matches");
    expect(infoData == resultData, "copy get_info data aliases result_data");
    // Contents match source.
    expect(std::memcmp(infoData, srcBytes, sizeof(srcBytes)) == 0,
           "copy contents match source");
    // Mutate source array (native) -> copy must be unchanged.
    char* mutableSrc = new char[sizeof(srcBytes)];
    std::memcpy(mutableSrc, srcBytes, sizeof(srcBytes));
    napi_value buf2 = nullptr;
    void* rd2 = nullptr;
    expect_status(napi_create_buffer_copy(env, sizeof(srcBytes), mutableSrc, &rd2, &buf2),
                  napi_ok, "copy2 ok");
    mutableSrc[0] = 'X';
    mutableSrc[1] = 'Y';
    void* info2 = nullptr;
    size_t len2 = 0;
    napi_get_buffer_info(env, buf2, &info2, &len2);
    expect(std::memcmp(info2, srcBytes, sizeof(srcBytes)) == 0,
           "copy independent after source mutation");
    delete[] mutableSrc;
    // Mutate copy via JS -> source array unchanged.
    StoreGlobal("__copy", buf2);
    expect(RunJSBool("(function(){ globalThis.__copy[0]=0x5A; return globalThis.__copy[0]==90; })()"),
           "JS can mutate copy");
    // Native view of copy reflects JS mutation.
    void* info3 = nullptr;
    size_t len3 = 0;
    napi_get_buffer_info(env, buf2, &info3, &len3);
    expect(static_cast<unsigned char*>(info3)[0] == 0x5A,
           "native sees JS mutation on copy");
    // First copy's bytes still original (independence between copies).
    expect(std::memcmp(infoData, srcBytes, sizeof(srcBytes)) == 0,
           "first copy unaffected by second copy JS mutation");
    DeleteGlobal("__copy");
    CloseScope(s);
    expect(RootCount() == Baseline(), "copy at baseline");
  }

  {
    std::fprintf(stdout, "-- case create-buffer-copy-zero --\n");
    napi_handle_scope s = OpenScope();
    napi_value buf = nullptr;
    void* rd = reinterpret_cast<void*>(0xdeadbeef);
    expect_status(napi_create_buffer_copy(env, 0, nullptr, &rd, &buf), napi_ok,
                  "copy 0 with null data ok");
    expect(buf != nullptr, "copy0 buf non-null");
    expect(rd == nullptr, "copy0 result_data null for 0");
    bool isBuf = false;
    napi_is_buffer(env, buf, &isBuf);
    expect(isBuf, "copy0 is_buffer true");
    CloseScope(s);
    expect(RootCount() == Baseline(), "copy0 at baseline");
  }

  {
    std::fprintf(stdout, "-- case create-buffer-copy-invalid --\n");
    napi_handle_scope s = OpenScope();
    napi_value buf = nullptr;
    void* rd = nullptr;
    expect_status(napi_create_buffer_copy(env, 4, nullptr, &rd, &buf),
                  napi_invalid_arg, "copy null data with len invalid_arg");
    expect(buf == nullptr, "copy invalid no publish");
    CloseScope(s);
    expect(RootCount() == Baseline(), "copy invalid at baseline");
  }

  // ---- napi_create_external_buffer: finalizer exactly once ----
  {
    std::fprintf(stdout, "-- case external-finalizer --\n");
    char* extData = new char[64];
    std::memcpy(extData, "extbuf!!", 8);
    void* hint = reinterpret_cast<void*>(0x1234);
    napi_handle_scope s = OpenScope();
    napi_value buf = nullptr;
    expect_status(napi_create_external_buffer(env, 64, extData, ExtFinalize, hint, &buf),
                  napi_ok, "create_external_buffer ok");
    expect(buf != nullptr, "external buf non-null");
    bool isBuf = false;
    napi_is_buffer(env, buf, &isBuf);
    expect(isBuf, "external is_buffer true");
    // Data aliases external pointer; JS sees native writes.
    void* infoData = nullptr;
    size_t infoLen = 0;
    expect_status(napi_get_buffer_info(env, buf, &infoData, &infoLen), napi_ok,
                  "external get_info ok");
    expect(infoData == extData, "external data pointer aliases");
    expect(infoLen == 64, "external len 64");
    expect(std::memcmp(infoData, "extbuf!!", 8) == 0, "external contents visible");
    StoreGlobal("__ext", buf);
    expect(RunJSBool("globalThis.__ext[0]==101 && globalThis.__ext[1]==120"),
           "JS sees external native writes");
    expect(RunJSBool("(function(){ globalThis.__ext[8]=0x7A; return globalThis.__ext[8]==122; })()"),
           "JS writes to external Buffer");
    expect(extData[8] == 0x7A, "native external storage sees JS writes");
    DeleteGlobal("__ext");
    g_ext_finalizer_calls = 0;
    g_ext_finalizer_last_data = nullptr;
    g_ext_finalizer_last_hint = nullptr;
    CloseScope(s);  // Release scope root; Buffer now only held by GC/heap.
    buf = nullptr;
    ClearStack();
    force_gc(rt);
    // D4: the GC trampoline never runs the user finalizer inside GC --
    // it only enqueues. No napi entry has run since the GC, so the count
    // must still be zero here.
    expect(g_ext_finalizer_calls == 0, "finalizer not run during GC (enqueue only)");
    // Finalizer is enqueued by Wrap before-collect trampoline; drain
    // happens on next napi entry (N3.2 drain point).
    napi_handle_scope s2 = OpenScope();
    napi_value dummy = nullptr;
    void* dummyData = nullptr;
    expect_status(napi_create_buffer(env, 1, &dummyData, &dummy), napi_ok,
                  "drain-trigger create ok");
    (void)dummy;
    CloseScope(s2);
    expect(g_ext_finalizer_calls == 1, "external finalizer fired once");
    expect(g_ext_finalizer_last_data == extData, "finalizer data matches");
    expect(g_ext_finalizer_last_hint == hint, "finalizer hint matches");
    // Second GC should not fire again (exactly once).
    force_gc(rt);
    napi_handle_scope s3 = OpenScope();
    napi_value tmp = nullptr;
    void* td = nullptr;
    napi_create_buffer(env, 1, &td, &tmp);
    (void)tmp;
    CloseScope(s3);
    expect(g_ext_finalizer_calls == 1, "external finalizer still once after second GC");
    expect(RootCount() == Baseline(), "external at baseline");
  }

  // ---- External with NULL finalizer: no record, no crash ----
  {
    std::fprintf(stdout, "-- case external-null-finalizer --\n");
    char* extData = new char[16];
    std::memset(extData, 0x42, 16);
    napi_handle_scope s = OpenScope();
    napi_value buf = nullptr;
    expect_status(napi_create_external_buffer(env, 16, extData, nullptr, nullptr, &buf),
                  napi_ok, "create_external null finalizer ok");
    bool isBuf = false;
    napi_is_buffer(env, buf, &isBuf);
    expect(isBuf, "null-finalizer is_buffer true");
    CloseScope(s);
    buf = nullptr;
    ClearStack();
    force_gc(rt);
    napi_handle_scope s2 = OpenScope();
    napi_value dummy = nullptr;
    void* dd = nullptr;
    napi_create_buffer(env, 1, &dd, &dummy);
    (void)dummy;
    CloseScope(s2);
    // No user finalizer to run; external data is caller-owned (leak in
    // probe is expected: the provider never frees it).
    // Just free manually to avoid leak report.
    delete[] extData;
    expect(RootCount() == Baseline(), "null-finalizer at baseline");
  }

  // ---- Plain Uint8Array is NOT a Buffer ----
  {
    std::fprintf(stdout, "-- case plain-uint8-not-buffer --\n");
    napi_handle_scope s = OpenScope();
    napi_value ta = nullptr;
    expect_status(napi_create_typedarray(env, napi_uint8_array, 8, nullptr, 0, &ta),
                  napi_ok, "create plain Uint8Array ok");
    bool isBuf = true;
    expect_status(napi_is_buffer(env, ta, &isBuf), napi_ok, "is_buffer on plain ok");
    expect(isBuf == false, "plain Uint8Array is NOT a Buffer");
    void* d = reinterpret_cast<void*>(0xdeadbeef);
    size_t l = 12345;
    expect_status(napi_get_buffer_info(env, ta, &d, &l), napi_invalid_arg,
                  "get_buffer_info on plain invalid_arg");
    expect(l == 12345, "invalid_arg leaves out untouched (length)");
    expect(d == reinterpret_cast<void*>(0xdeadbeef), "invalid_arg leaves out untouched (data)");
    // Plain ArrayBuffer is not a Buffer.
    napi_value ab = nullptr;
    void* abData = nullptr;
    napi_create_arraybuffer(env, 8, &abData, &ab);
    isBuf = true;
    napi_is_buffer(env, ab, &isBuf);
    expect(!isBuf, "ArrayBuffer is NOT a Buffer");
    // Plain object is not a Buffer.
    napi_value obj = nullptr;
    napi_create_object(env, &obj);
    isBuf = true;
    napi_is_buffer(env, obj, &isBuf);
    expect(!isBuf, "plain object is NOT a Buffer");
    // Number is not a Buffer (ok+false, not error).
    napi_value num = nullptr;
    napi_create_double(env, 3.0, &num);
    isBuf = true;
    expect_status(napi_is_buffer(env, num, &isBuf), napi_ok, "is_buffer on number ok");
    expect(!isBuf, "number is NOT a Buffer");
    CloseScope(s);
    expect(RootCount() == Baseline(), "plain at baseline");
  }

  // ---- get_buffer_info invalid cases ----
  {
    std::fprintf(stdout, "-- case get-info-invalid --\n");
    napi_handle_scope s = OpenScope();
    napi_value obj = nullptr;
    napi_create_object(env, &obj);
    void* d = nullptr;
    size_t l = 0;
    expect_status(napi_get_buffer_info(env, obj, &d, &l), napi_invalid_arg,
                  "get_info on object invalid_arg");
    expect_status(napi_get_buffer_info(env, nullptr, &d, &l), napi_invalid_arg,
                  "get_info null value invalid_arg");
    CloseScope(s);
    expect(RootCount() == Baseline(), "get-info invalid at baseline");
  }

  // ---- Pending/Closing discipline spot checks (C column) ----
  {
    std::fprintf(stdout, "-- case pending --\n");
    napi_handle_scope s = OpenScope();
    napi_value buf = nullptr;
    void* data = nullptr;
    expect_status(napi_create_buffer(env, 8, &data, &buf), napi_ok, "buf for pending");
    // Set pending exception via JS throw.
    {
      JsValueRef script = JS_INVALID_REFERENCE;
      JsValueRef url = JS_INVALID_REFERENCE;
      const char* throwCode = "throw new Error('pending')";
      JsCreateString(throwCode, std::strlen(throwCode), &script);
      JsCreateString("probe.js", 8, &url);
      JsAddRef(script, nullptr);
      JsValueRef res = JS_INVALID_REFERENCE;
      JsRun(script, JS_SOURCE_CONTEXT_NONE, url,
            JsParseScriptAttributeNone, &res);
      JsRelease(script, nullptr);
      JsRelease(url, nullptr);
      // Gated (No) create should return pending without executing.
      napi_value buf2 = nullptr;
      void* d2 = nullptr;
      napi_status st = napi_create_buffer(env, 8, &d2, &buf2);
      expect(st == napi_pending_exception, "gated create with pending is pending_exception");
      expect(buf2 == nullptr, "gated create with pending no publish");
      // Executes (Yes) is_buffer / get_buffer_info should succeed.
      bool isBuf = false;
      expect_status(napi_is_buffer(env, buf, &isBuf), napi_ok,
                    "is_buffer with pending executes");
      expect(isBuf, "is_buffer with pending true");
      void* gi = nullptr;
      size_t gl = 0;
      expect_status(napi_get_buffer_info(env, buf, &gi, &gl), napi_ok,
                    "get_info with pending executes");
      expect(gl == 8, "get_info with pending len 8");
      // Clear the pending exception.
      JsValueRef ex = JS_INVALID_REFERENCE;
      JsGetAndClearException(&ex);
      if (ex) JsRelease(ex, nullptr);
    }
    CloseScope(s);
    expect(RootCount() == Baseline(), "pending at baseline");
  }

  chakra_nodeapi_destroy_env(env);
  expect(true, "teardown: env destroyed");
  JsSetCurrentContext(JS_INVALID_REFERENCE);
  JsDisposeRuntime(rt);

  std::fprintf(stdout, "BUFFERS_PROBE: %s (%d/%d)\n",
               (g_pass == g_total) ? "PASS" : "FAIL", g_pass, g_total);
  return (g_pass == g_total) ? 0 : 1;
}
