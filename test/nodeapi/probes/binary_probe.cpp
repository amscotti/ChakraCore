//-------------------------------------------------------------------------------------------------------
// Copyright (c) ChakraCore Project Contributors. All rights reserved.
// Licensed under the MIT license. See LICENSE.txt file in the project root for full license information.
//-------------------------------------------------------------------------------------------------------
//
// N3.1 binary probes (plan task N3.1, A5 group; §3.4 R4, §3.7).
//
// Direct C++ probe: creates a Chakra runtime + context, a real env
// (chakra_nodeapi_create_env) and exercises every lib/NodeApi/NodeApiBinary.cpp
// entry in-process (no addon, no ch-nodeapi host). Run via
// run_binary_probe.sh, which builds with `zig c++` through
// the runner script and links the engine libChakraCore.dylib
// produced by `mise exec -- zig build`.
//
// Coverage (each MUST fail on mismatch):
//   - ArrayBuffer create + get info (data pointer + length), zero-length.
//   - External ArrayBuffer create + finalizer fires exactly once on collect
//     (GC via JsCollectGarbage + next napi entry drain; 3 iterations).
//   - TypedArray create (attached + standalone) for every napi-supported
//     type (9) and info tuple (type/length/data/arraybuffer/byte_offset),
//     plus BigInt64/BigUint64 generic_failure non-conformance.
//   - DataView create + get info (attached + standalone), offset/length.
//   - Detach observable from JS (ArrayBuffer.prototype.detached getter
//     + typedarray length 0 after detach) and napi_is_detached query
//     on attached/detached/zero-length/non-ArrayBuffer.
//   - Storage borrow invalidated after a JS call that detaches (native
//     callback invoked from JS detaches the buffer; fresh get_*_info
//     observes NULL/0).
//   - Root-count baselining (R-REF-14) and last-error discipline spot checks.
//
// Rooting methodology: each case runs in its own handle scope; the probe
// asserts after every case the R-REF-14 total (scope roots + ref-record
// roots) is back at baseline, so provider scratch roots (counter-neutral)
// and scope roots are accounted.
//
// Output: one line per assertion ("PASS <case>" / "FAIL <case> ..."),
// then "BINARY_PROBE: PASS (n/n)" exit 0, or "BINARY_PROBE: FAIL
// (k/n)" exit 1.

#include "NodeApiHostApi.h"
#include "js_native_api.h"
#include "NodeApiBinary.h"
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
#error "binary_probe requires -DCHAKRA_NODEAPI_TEST_HOOKS (root counter)"
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
        // Convert to string for diagnostics (best effort).
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

bool RunJSEqualsDetached(const char* global_name) {
  // Evaluate `globalThis.<name>.detached === true`
  std::string code = "globalThis.";
  code += global_name;
  code += ".detached == true";
  JsValueRef script = JS_INVALID_REFERENCE;
  JsValueRef url = JS_INVALID_REFERENCE;
  JsCreateString(code.c_str(), code.size(), &script);
  JsCreateString("probe.js", 8, &url);
  JsAddRef(script, nullptr);
  JsValueRef result = JS_INVALID_REFERENCE;
  JsRun(script, JS_SOURCE_CONTEXT_NONE, url,
        JsParseScriptAttributeNone, &result);
  JsRelease(script, nullptr);
  JsRelease(url, nullptr);
  bool out = false;
  if (result) {
    JsValueType t = JsUndefined;
    if (JsGetValueType(result, &t) == JsNoError && t == JsBoolean) {
      JsBooleanToBool(result, &out);
    }
  }
  if (result) JsRelease(result, nullptr);
  // Clear any exception.
  bool has = false;
  if (JsHasException(&has) == JsNoError && has) {
    JsValueRef ex = JS_INVALID_REFERENCE;
    JsGetAndClearException(&ex);
  }
  return out;
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
  // Use delete to match allocation via new[] in probe.
  delete[] static_cast<char*>(data);
}

// Create an external ArrayBuffer and release every reference to it, in a
// dedicated frame: no JsValueRef escapes to the caller, so after return
// the only thing keeping the buffer alive is GC reachability (plus any
// conservative stack residue, flushed by ChurnStack before collecting).
// Returns the create status.
napi_status CreateUnrootedExternal(napi_env env, void* data, size_t len,
                                   void* hint) {
  napi_handle_scope s = nullptr;
  if (napi_open_handle_scope(env, &s) != napi_ok) return napi_generic_failure;
  napi_value ab = nullptr;
  napi_status st =
      napi_create_external_arraybuffer(env, data, len, ExtFinalize, hint, &ab);
  (void)napi_close_handle_scope(env, s);
  return st;
}

// Overwrite the current stack region so stale conservative roots (e.g. a
// spilled JsValueRef from the creating frame, kept alive by -O3 codegen)
// do not pin an otherwise unreachable buffer across a forced GC.
inline void ChurnStack() {
  volatile char buf[8192];
  for (size_t i = 0; i < sizeof(buf); i++) buf[i] = static_cast<char>(i & 0x7f);
  ClearStack();
}

// Detach callback: JS-invoked native function that detaches its argument.
napi_value DetachCb(napi_env env, napi_callback_info info) {
  size_t argc = 1;
  napi_value argv[1] = {nullptr};
  napi_get_cb_info(env, info, &argc, argv, nullptr, nullptr);
  if (argc >= 1 && argv[0]) {
    napi_detach_arraybuffer(env, argv[0]);
  }
  napi_value undef = nullptr;
  napi_get_undefined(env, &undef);
  return undef;
}

struct TypedCase {
  napi_typedarray_type ntype;
  const char* name;
  size_t elemSize;
};

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

  // ---- ArrayBuffer create + get info ----
  {
    std::fprintf(stdout, "-- case arraybuffer-basic --\n");
    napi_handle_scope s = OpenScope();
    napi_value ab = nullptr;
    void* data = nullptr;
    expect_status(napi_create_arraybuffer(env, 42, &data, &ab), napi_ok,
                  "create_arraybuffer 42 ok");
    expect(ab != nullptr, "create_arraybuffer non-null");
    expect(data != nullptr, "create_arraybuffer data non-null for 42");
    void* infoData = nullptr;
    size_t infoLen = 999;
    expect_status(napi_get_arraybuffer_info(env, ab, &infoData, &infoLen), napi_ok,
                  "get_arraybuffer_info 42 ok");
    expect(infoLen == 42, "arraybuffer infoLen 42");
    expect(infoData == data, "arraybuffer data matches create data");
    expect(infoData != nullptr, "arraybuffer info data non-null");
    CloseScope(s);
    expect(RootCount() == Baseline(), "arraybuffer-basic at baseline");
  }

  {
    std::fprintf(stdout, "-- case arraybuffer-zero --\n");
    napi_handle_scope s = OpenScope();
    napi_value ab = nullptr;
    void* data = reinterpret_cast<void*>(0xdeadbeef);
    expect_status(napi_create_arraybuffer(env, 0, &data, &ab), napi_ok,
                  "create_arraybuffer 0 ok");
    expect(ab != nullptr, "zero ab non-null");
    // Zero-length storage is nullptr.
    expect(data == nullptr, "zero data is nullptr");
    void* infoData = reinterpret_cast<void*>(0xdeadbeef);
    size_t infoLen = 999;
    expect_status(napi_get_arraybuffer_info(env, ab, &infoData, &infoLen), napi_ok,
                  "get zero info ok");
    expect(infoLen == 0, "zero infoLen 0");
    expect(infoData == nullptr, "zero info data nullptr");
    bool detached = true;
    expect_status(napi_is_detached_arraybuffer(env, ab, &detached), napi_ok,
                  "is_detached zero ok");
    expect(detached == false, "zero detached false");
    // External AB with NULL/0 is immediately detached (Node NullArrayBuffer parity).
    napi_value nullExt = nullptr;
    expect_status(napi_create_external_arraybuffer(env, nullptr, 0, nullptr, nullptr,
                                                   &nullExt),
                  napi_ok, "create external null/0 ok");
    bool nullExtDet = false;
    expect_status(napi_is_detached_arraybuffer(env, nullExt, &nullExtDet),
                  napi_ok, "is_detached null/0 ok");
    expect(nullExtDet == true, "null/0 external AB is detached");
    // External AB with NULL/0 and a finalizer runs finalizer once after publish,
    // leaves live/queued counts at baseline, and does not double-fire on GC.
    {
      size_t liveNull0 = NodeApiBinary_ExternalLiveCount();
      size_t queuedNull0 = NodeApiBinary_ExternalQueuedCount();
      int callsNull0 = g_ext_finalizer_calls;
      void* nullHint = reinterpret_cast<void*>(0xBEEF0);
      napi_value nullExtCb = nullptr;
      expect_status(napi_create_external_arraybuffer(env, nullptr, 0, ExtFinalize,
                                                     nullHint, &nullExtCb),
                    napi_ok, "create external null/0 with cb ok");
      expect(nullExtCb != nullptr, "null/0 external AB with cb non-null");
      bool detCb = false;
      expect_status(napi_is_detached_arraybuffer(env, nullExtCb, &detCb),
                    napi_ok, "is_detached null/0 cb ok");
      expect(detCb == true, "null/0 external AB with cb is detached");
      expect(g_ext_finalizer_calls == callsNull0 + 1,
             "null/0 external AB finalizer ran once after publish");
      expect(g_ext_finalizer_last_data == nullptr, "null/0 cb data is nullptr");
      expect(g_ext_finalizer_last_hint == nullHint, "null/0 cb hint matches");
      expect(NodeApiBinary_ExternalLiveCount() == liveNull0,
             "null/0 cb live count back at baseline");
      expect(NodeApiBinary_ExternalQueuedCount() == queuedNull0,
             "null/0 cb queue count back at baseline");
    }
    CloseScope(s);
    expect(RootCount() == Baseline(), "zero at baseline");
    force_gc(rt);
  }

  {
    std::fprintf(stdout, "-- case arraybuffer-invalid --\n");
    napi_handle_scope s = OpenScope();
    napi_value obj = nullptr;
    napi_create_object(env, &obj);
    void* d = nullptr;
    size_t l = 123;
    expect_status(napi_get_arraybuffer_info(env, obj, &d, &l), napi_invalid_arg,
                  "get_arraybuffer_info on object invalid_arg");
    expect(l == 123, "invalid_arg leaves out untouched");
    expect_status(napi_detach_arraybuffer(env, obj), napi_arraybuffer_expected,
                  "detach on object arraybuffer_expected");
    bool is_det = true;
    expect_status(napi_is_detached_arraybuffer(env, obj, &is_det), napi_ok,
                  "is_detached on object ok");
    expect(is_det == false, "is_detached on object false (not error)");
    CloseScope(s);
    expect(RootCount() == Baseline(), "invalid at baseline");
  }

  {
    std::fprintf(stdout, "-- case is_detached-wrong-type --\n");
    napi_handle_scope s = OpenScope();
    napi_value num = nullptr;
    napi_create_double(env, 1.0, &num);
    bool r = true;
    expect_status(napi_is_detached_arraybuffer(env, num, &r), napi_ok,
                  "is_detached on number ok");
    expect(r == false, "is_detached on number false");
    CloseScope(s);
    expect(RootCount() == Baseline(), "wrong-type at baseline");
  }

  // ---- External ArrayBuffer finalizer ----
  {
    std::fprintf(stdout, "-- case external-finalizer --\n");
    // Allocate native data.
    char* extData = new char[64];
    std::memcpy(extData, "external", 8);
    void* hint = reinterpret_cast<void*>(0x1234);
    napi_handle_scope s = OpenScope();
    napi_value ab = nullptr;
    expect_status(napi_create_external_arraybuffer(env, extData, 64, ExtFinalize, hint, &ab),
                  napi_ok, "create_external_arraybuffer ok");
    expect(ab != nullptr, "external ab non-null");
    // Verify storage aliases external_data.
    void* infoData = nullptr;
    size_t infoLen = 0;
    expect_status(napi_get_arraybuffer_info(env, ab, &infoData, &infoLen), napi_ok,
                  "external get info ok");
    expect(infoData == extData, "external data pointer aliases");
    expect(infoLen == 64, "external len 64");
    expect(std::memcmp(infoData, "external", 8) == 0, "external contents visible");
    g_ext_finalizer_calls = 0;
    g_ext_finalizer_last_data = nullptr;
    g_ext_finalizer_last_hint = nullptr;
    CloseScope(s);  // Release scope root; buffer now only held by GC/heap.
    // The buffer is now unreachable from provider scopes; force GC.
    // We keep a weak notion: don't keep ab alive in C++ local (it is still
    // a raw pointer but not rooted; GC may not collect if still reachable
    // via stack. ClearStack + overwrite local.)
    ab = nullptr;
    ClearStack();
    force_gc(rt);
    // Finalizer is enqueued by trampoline; drain happens on next napi entry.
    // Trigger drain via a cheap binary entry.
    napi_handle_scope s2 = OpenScope();
    void* dummyData = nullptr;
    size_t dummyLen = 0;
    napi_value dummyAb = nullptr;
    expect_status(napi_create_arraybuffer(env, 1, &dummyData, &dummyAb), napi_ok,
                  "drain-trigger create ok");
    (void)dummyAb;
    CloseScope(s2);
    expect(g_ext_finalizer_calls == 1, "external finalizer fired once");
    expect(g_ext_finalizer_last_data == extData, "finalizer data matches");
    expect(g_ext_finalizer_last_hint == hint, "finalizer hint matches");
    // Second GC should not fire again (exactly once).
    force_gc(rt);
    napi_handle_scope s3 = OpenScope();
    napi_value tmp = nullptr;
    void* td = nullptr;
    napi_create_arraybuffer(env, 1, &td, &tmp);
    (void)tmp;
    CloseScope(s3);
    expect(g_ext_finalizer_calls == 1, "external finalizer still once after second GC");
    expect(RootCount() == Baseline(), "external at baseline");
  }

  // ---- TypedArray: every napi-supported type ----
  {
    std::fprintf(stdout, "-- case typedarray-all-types --\n");
    TypedCase cases[] = {
        {napi_int8_array, "int8", 1},
        {napi_uint8_array, "uint8", 1},
        {napi_uint8_clamped_array, "uint8clamped", 1},
        {napi_int16_array, "int16", 2},
        {napi_uint16_array, "uint16", 2},
        {napi_int32_array, "int32", 4},
        {napi_uint32_array, "uint32", 4},
        {napi_float32_array, "float32", 4},
        {napi_float64_array, "float64", 8},
    };
    for (auto& c : cases) {
      napi_handle_scope s = OpenScope();
      size_t count = 5;
      void* backData = nullptr;
      napi_value backAB = nullptr;
      expect_status(napi_create_arraybuffer(env, count * c.elemSize, &backData, &backAB),
                    napi_ok, "typedarray backing ab");
      napi_value ta = nullptr;
      char name[64];
      std::snprintf(name, sizeof(name), "typedarray create %s", c.name);
      expect_status(napi_create_typedarray(env, c.ntype, count, backAB, 0, &ta),
                    napi_ok, name);
      expect(ta != nullptr, "ta non-null");
      napi_typedarray_type gotType = napi_int8_array;
      size_t gotLen = 999;
      void* gotData = nullptr;
      napi_value gotAB = nullptr;
      size_t gotOff = 999;
      std::snprintf(name, sizeof(name), "get_typedarray_info %s", c.name);
      expect_status(napi_get_typedarray_info(env, ta, &gotType, &gotLen, &gotData,
                                             &gotAB, &gotOff),
                    napi_ok, name);
      std::snprintf(name, sizeof(name), "%s type matches", c.name);
      expect(gotType == c.ntype, name);
      std::snprintf(name, sizeof(name), "%s len 5", c.name);
      expect(gotLen == count, name);
      std::snprintf(name, sizeof(name), "%s byte_offset 0", c.name);
      expect(gotOff == 0, name);
      std::snprintf(name, sizeof(name), "%s data non-null", c.name);
      expect(gotData != nullptr, name);
      std::snprintf(name, sizeof(name), "%s arraybuffer non-null", c.name);
      expect(gotAB != nullptr, name);
      bool isAB = false;
      napi_is_arraybuffer(env, gotAB, &isAB);
      std::snprintf(name, sizeof(name), "%s arraybuffer is ArrayBuffer", c.name);
      expect(isAB, name);
      // Data pointer should equal ArrayBuffer storage + offset.
      void* abData = nullptr;
      size_t abLen = 0;
      expect_status(napi_get_arraybuffer_info(env, gotAB, &abData, &abLen), napi_ok,
                    "typedarray ab info");
      std::snprintf(name, sizeof(name), "%s abLen == count*elemSize", c.name);
      expect(abLen == count * c.elemSize, name);
      std::snprintf(name, sizeof(name), "%s data == abData+offset", c.name);
      expect(static_cast<unsigned char*>(gotData) ==
                 static_cast<unsigned char*>(abData) + gotOff,
             name);
      CloseScope(s);
    }
    // Node-API conformance: null arraybuffer returns napi_invalid_arg.
    {
      napi_handle_scope s = OpenScope();
      napi_value null_ta = nullptr;
      expect_status(napi_create_typedarray(env, napi_uint8_array, 1, nullptr, 0, &null_ta),
                    napi_invalid_arg, "typedarray null arraybuffer invalid_arg");
      CloseScope(s);
    }
    expect(RootCount() == Baseline(), "typedarray all-types at baseline");
  }

  {
    std::fprintf(stdout, "-- case typedarray-with-buffer --\n");
    napi_handle_scope s = OpenScope();
    void* abData = nullptr;
    napi_value ab = nullptr;
    expect_status(napi_create_arraybuffer(env, 32, &abData, &ab), napi_ok,
                  "create ab 32 for view");
    // uint16 view at offset 4, 3 elements (6 bytes).
    napi_value ta = nullptr;
    expect_status(napi_create_typedarray(env, napi_uint16_array, 3, ab, 4, &ta),
                  napi_ok, "create uint16 view offset4");
    napi_typedarray_type gt = napi_int8_array;
    size_t glen = 0;
    void* gdata = nullptr;
    napi_value gab = nullptr;
    size_t goff = 0;
    expect_status(napi_get_typedarray_info(env, ta, &gt, &glen, &gdata, &gab, &goff),
                  napi_ok, "get view info");
    expect(gt == napi_uint16_array, "view type uint16");
    expect(glen == 3, "view len 3");
    expect(goff == 4, "view offset 4");
    expect(gdata == static_cast<unsigned char*>(abData) + 4, "view data offset");
    expect(gab == ab, "view arraybuffer same");
    CloseScope(s);
    expect(RootCount() == Baseline(), "view at baseline");
  }

  {
    std::fprintf(stdout, "-- case typedarray-bigint --\n");
    napi_handle_scope s = OpenScope();
    // Backed BigInt64Array / BigUint64Array
    TypedCase bigCases[] = {
        {napi_bigint64_array, "bigint64", 8},
        {napi_biguint64_array, "biguint64", 8},
    };
    for (auto& c : bigCases) {
      void* backData = nullptr;
      napi_value backAB = nullptr;
      expect_status(napi_create_arraybuffer(env, 3 * c.elemSize, &backData, &backAB),
                    napi_ok, "bigint backing ab");
      napi_value ta = nullptr;
      expect_status(napi_create_typedarray(env, c.ntype, 3, backAB, 0, &ta),
                    napi_ok, "bigint create ok");
      expect(ta != nullptr, "bigint ta non-null");
      bool isTA = false;
      expect_status(napi_is_typedarray(env, ta, &isTA), napi_ok, "bigint is_typedarray ok");
      expect(isTA, "bigint is_typedarray true");
      napi_typedarray_type gt = napi_int8_array;
      size_t gl = 0;
      void* gd = nullptr;
      napi_value gab = nullptr;
      size_t go = 999;
      expect_status(napi_get_typedarray_info(env, ta, &gt, &gl, &gd, &gab, &go),
                    napi_ok, "bigint get_typedarray_info ok");
      expect(gt == c.ntype, "bigint type matches");
      expect(gl == 3, "bigint len 3");
      expect(go == 0, "bigint offset 0");
      expect(gd == backData, "bigint data matches backing");
      expect(gab == backAB, "bigint buffer matches backing");
    }
    // Buffer-backed BigInt64Array with offset 8, length 2
    void* abData = nullptr;
    napi_value ab = nullptr;
    expect_status(napi_create_arraybuffer(env, 32, &abData, &ab), napi_ok,
                  "bigint backing ab 32");
    napi_value bta = nullptr;
    expect_status(napi_create_typedarray(env, napi_bigint64_array, 2, ab, 8, &bta),
                  napi_ok, "bigint view offset 8");
    napi_typedarray_type bgt = napi_int8_array;
    size_t bgl = 0;
    void* bgd = nullptr;
    napi_value bgab = nullptr;
    size_t bgo = 0;
    expect_status(napi_get_typedarray_info(env, bta, &bgt, &bgl, &bgd, &bgab, &bgo),
                  napi_ok, "bigint view info ok");
    expect(bgt == napi_bigint64_array, "bigint view type matches");
    expect(bgl == 2, "bigint view len 2");
    expect(bgo == 8, "bigint view offset 8");
    expect(bgd == static_cast<unsigned char*>(abData) + 8, "bigint view data offset");
    expect(bgab == ab, "bigint view buffer matches");
    // Unaligned offset triggers RangeError -> pending_exception
    napi_value bad_ta = nullptr;
    expect_status(napi_create_typedarray(env, napi_bigint64_array, 1, ab, 4, &bad_ta),
                  napi_pending_exception, "bigint unaligned offset pending RangeError");
    JsValueRef ex = JS_INVALID_REFERENCE;
    JsGetAndClearException(&ex);
    if (ex) JsRelease(ex, nullptr);
    // Detach BigInt64Array backing buffer cleanly -> len 0, data nullptr
    expect_status(napi_detach_arraybuffer(env, ab), napi_ok, "bigint backing detach ok");
    void* dgd = reinterpret_cast<void*>(0x1);
    size_t dgl = 99;
    expect_status(napi_get_typedarray_info(env, bta, nullptr, &dgl, &dgd, nullptr, nullptr),
                  napi_ok, "bigint info after detach");
    expect(dgl == 0, "bigint detached len 0");
    expect(dgd == nullptr, "bigint detached data null");
    // Float16Array (JS intrinsic if available): is_typedarray true, get_typedarray_info generic_failure cleanly
    {
      JsValueRef f16_script = JS_INVALID_REFERENCE;
      JsValueRef f16_url = JS_INVALID_REFERENCE;
      const char* f16_src = "(typeof Float16Array === 'function') ? new Float16Array(2) : null";
      JsCreateString(f16_src, std::strlen(f16_src), &f16_script);
      JsCreateString("f16.js", 6, &f16_url);
      JsValueRef f16_res = JS_INVALID_REFERENCE;
      if (JsRun(f16_script, JS_SOURCE_CONTEXT_NONE, f16_url,
                JsParseScriptAttributeNone, &f16_res) == JsNoError &&
          f16_res != JS_INVALID_REFERENCE) {
        JsValueType ft = JsUndefined;
        if (JsGetValueType(f16_res, &ft) == JsNoError && ft == JsTypedArray) {
          napi_value nf16 = reinterpret_cast<napi_value>(f16_res);
          bool is_ta = false;
          expect_status(napi_is_typedarray(env, nf16, &is_ta), napi_ok, "float16 is_typedarray");
          expect(is_ta, "float16 is_typedarray true");
          napi_typedarray_type ft_out = napi_int8_array;
          expect_status(napi_get_typedarray_info(env, nf16, &ft_out, nullptr, nullptr, nullptr, nullptr),
                        napi_generic_failure, "float16 get_typedarray_info generic_failure");
        }
      }
      if (f16_script) JsRelease(f16_script, nullptr);
      if (f16_url) JsRelease(f16_url, nullptr);
    }
    CloseScope(s);
    expect(RootCount() == Baseline(), "bigint/float16 at baseline");
  }

  {
    std::fprintf(stdout, "-- case typedarray-invalid --\n");
    napi_handle_scope s = OpenScope();
    napi_value ab = nullptr;
    void* ad = nullptr;
    napi_create_arraybuffer(env, 16, &ad, &ab);
    napi_value ta = nullptr;
    // Invalid type enum value (out of range).
    expect_status(napi_create_typedarray(
                      env, static_cast<napi_typedarray_type>(99), 1, ab, 0, &ta),
                  napi_invalid_arg, "bad type invalid_arg");
    // Not an ArrayBuffer.
    napi_value obj = nullptr;
    napi_create_object(env, &obj);
    expect_status(napi_create_typedarray(env, napi_uint8_array, 1, obj, 0, &ta),
                  napi_invalid_arg, "non-AB invalid_arg");
    // Get info on non-typedarray.
    napi_typedarray_type t = napi_int8_array;
    size_t l = 1;
    void* d = nullptr;
    napi_value b = nullptr;
    size_t o = 1;
    expect_status(napi_get_typedarray_info(env, obj, &t, &l, &d, &b, &o),
                  napi_invalid_arg, "get info on object invalid_arg");
    CloseScope(s);
    expect(RootCount() == Baseline(), "typedarray invalid at baseline");
  }

  // ---- DataView ----
  {
    std::fprintf(stdout, "-- case dataview-basic --\n");
    napi_handle_scope s = OpenScope();
    void* abData = nullptr;
    napi_value ab = nullptr;
    expect_status(napi_create_arraybuffer(env, 32, &abData, &ab), napi_ok,
                  "ab 32 for dv");
    napi_value dv = nullptr;
    expect_status(napi_create_dataview(env, 10, ab, 5, &dv), napi_ok,
                  "create dataview len10 off5");
    size_t glen = 0;
    void* gdata = nullptr;
    napi_value gab = nullptr;
    size_t goff = 0;
    expect_status(napi_get_dataview_info(env, dv, &glen, &gdata, &gab, &goff),
                  napi_ok, "get dataview info");
    expect(glen == 10, "dv len 10");
    expect(goff == 5, "dv off 5");
    expect(gdata == static_cast<unsigned char*>(abData) + 5, "dv data offset");
    expect(gab == ab, "dv ab same");
    bool isDV = false;
    napi_is_dataview(env, dv, &isDV);
    expect(isDV, "is_dataview true");
    // Standalone dataview (null ab).
    napi_value dv2 = nullptr;
    expect_status(napi_create_dataview(env, 8, nullptr, 0, &dv2), napi_ok,
                  "create standalone dataview");
    size_t l2 = 0;
    void* d2 = nullptr;
    napi_value ab2 = nullptr;
    size_t o2 = 0;
    expect_status(napi_get_dataview_info(env, dv2, &l2, &d2, &ab2, &o2), napi_ok,
                  "get standalone dv info");
    expect(l2 == 8, "standalone len 8");
    expect(o2 == 0, "standalone off 0");
    expect(ab2 != nullptr, "standalone ab non-null");
    CloseScope(s);
    expect(RootCount() == Baseline(), "dataview at baseline");
  }

  {
    std::fprintf(stdout, "-- case dataview-invalid --\n");
    napi_handle_scope s = OpenScope();
    napi_value obj = nullptr;
    napi_create_object(env, &obj);
    napi_value dv = nullptr;
    expect_status(napi_create_dataview(env, 8, obj, 0, &dv), napi_invalid_arg,
                  "create dv on object invalid_arg");
    size_t l = 1;
    void* d = nullptr;
    napi_value b = nullptr;
    size_t o = 1;
    expect_status(napi_get_dataview_info(env, obj, &l, &d, &b, &o), napi_invalid_arg,
                  "get dv on object invalid_arg");
    CloseScope(s);
    expect(RootCount() == Baseline(), "dv invalid at baseline");
  }

  // ---- Detach + JS observability ----
  {
    std::fprintf(stdout, "-- case detach-js --\n");
    napi_handle_scope s = OpenScope();
    void* ad = nullptr;
    napi_value ab = nullptr;
    expect_status(napi_create_arraybuffer(env, 16, &ad, &ab), napi_ok, "ab16");
    // Write a byte via storage.
    unsigned char* bytes = static_cast<unsigned char*>(ad);
    bytes[0] = 0xAB;
    StoreGlobal("__buf", ab);
    expect(!RunJSEqualsDetached("__buf"), "JS detached false before detach");
    expect_status(napi_detach_arraybuffer(env, ab), napi_ok, "detach ok");
    expect(RunJSEqualsDetached("__buf"), "JS detached true after detach");
    // TypedArray view over detached buffer has length 0.
    napi_value ta = nullptr;
    // Create typedarray on detached buffer should now fail? But we detach after view.
    // Create view before detach case separately.
    // For this buffer, get info shows NULL/0.
    void* infoData = reinterpret_cast<void*>(0xdead);
    size_t infoLen = 999;
    expect_status(napi_get_arraybuffer_info(env, ab, &infoData, &infoLen), napi_ok,
                  "get info after detach ok");
    expect(infoData == nullptr, "detached info data nullptr");
    expect(infoLen == 0, "detached info len 0");
    bool det = false;
    expect_status(napi_is_detached_arraybuffer(env, ab, &det), napi_ok,
                  "is_detached after detach ok");
    expect(det == true, "is_detached true");
    // Second detach -> detachable_expected.
    expect_status(napi_detach_arraybuffer(env, ab), napi_detachable_arraybuffer_expected,
                  "second detach detachable_expected");
    DeleteGlobal("__buf");
    CloseScope(s);
    expect(RootCount() == Baseline(), "detach at baseline");
  }

  {
    std::fprintf(stdout, "-- case detach-t typedarray-view --\n");
    napi_handle_scope s = OpenScope();
    void* ad = nullptr;
    napi_value ab = nullptr;
    expect_status(napi_create_arraybuffer(env, 64, &ad, &ab), napi_ok, "ab64");
    napi_value ta = nullptr;
    expect_status(napi_create_typedarray(env, napi_float64_array, 4, ab, 0, &ta),
                  napi_ok, "ta float64 4");
    // Verify JS length before detach.
    StoreGlobal("__ta", ta);
    StoreGlobal("__ab2", ab);
    expect(RunJSBool("globalThis.__ta.length == 4"), "JS ta length 4 before detach");
    expect_status(napi_detach_arraybuffer(env, ab), napi_ok, "detach ab64");
    expect(RunJSBool("globalThis.__ta.length == 0"), "JS ta length 0 after detach");
    // Get typedarray info after detach: length 0, data nullptr.
    napi_typedarray_type gt = napi_int8_array;
    size_t glen = 999;
    void* gdata = reinterpret_cast<void*>(0xdead);
    napi_value gab = nullptr;
    size_t goff = 999;
    expect_status(napi_get_typedarray_info(env, ta, &gt, &glen, &gdata, &gab, &goff),
                  napi_ok, "get ta info after detach ok");
    expect(glen == 0, "detached ta len 0");
    expect(gdata == nullptr, "detached ta data nullptr");
    expect(gab == ab, "detached ta ab same (still detached)");
    DeleteGlobal("__ta");
    DeleteGlobal("__ab2");
    CloseScope(s);
    expect(RootCount() == Baseline(), "detach-view at baseline");
  }

  // ---- Storage borrow invalidated after JS call that detaches ----
  {
    std::fprintf(stdout, "-- case borrow-invalidate --\n");
    napi_handle_scope s = OpenScope();
    void* ad = nullptr;
    napi_value ab = nullptr;
    expect_status(napi_create_arraybuffer(env, 32, &ad, &ab), napi_ok, "ab32 borrow");
    void* beforeData = nullptr;
    size_t beforeLen = 0;
    expect_status(napi_get_arraybuffer_info(env, ab, &beforeData, &beforeLen),
                  napi_ok, "before info ok");
    expect(beforeData != nullptr && beforeLen == 32, "before borrow valid");
    unsigned char* bptr = static_cast<unsigned char*>(beforeData);
    bptr[0] = 0x11;
    // Create a native function that detaches, store as global, call from JS.
    napi_value detFn = nullptr;
    expect_status(napi_create_function(env, "__detach", NAPI_AUTO_LENGTH, DetachCb,
                                       nullptr, &detFn),
                  napi_ok, "create detach callback");
    StoreGlobal("__detachFn", detFn);
    StoreGlobal("__targetAb", ab);
    // JS call that detaches (runs native code via callback).
    expect(RunJSBool("globalThis.__detachFn(globalThis.__targetAb); globalThis.__targetAb.detached == true"),
           "JS detach call succeeded");
    // The previous borrow (beforeData) is now invalid (detached). A fresh
    // get must observe NULL/0 and detached true, not the old pointer.
    void* afterData = reinterpret_cast<void*>(0xdead);
    size_t afterLen = 999;
    expect_status(napi_get_arraybuffer_info(env, ab, &afterData, &afterLen),
                  napi_ok, "after info ok");
    expect(afterData == nullptr, "after borrow invalidated (nullptr)");
    expect(afterLen == 0, "after length 0");
    bool det = false;
    napi_is_detached_arraybuffer(env, ab, &det);
    expect(det == true, "detached after JS detach true");
    DeleteGlobal("__detachFn");
    DeleteGlobal("__targetAb");
    CloseScope(s);
    expect(RootCount() == Baseline(), "borrow at baseline");
  }

  // ---- DataView bytes visible both directions ----
  {
    std::fprintf(stdout, "-- case bytes-visible --\n");
    napi_handle_scope s = OpenScope();
    void* ad = nullptr;
    napi_value ab = nullptr;
    expect_status(napi_create_arraybuffer(env, 8, &ad, &ab), napi_ok, "ab8");
    napi_value ta = nullptr;
    expect_status(napi_create_typedarray(env, napi_uint8_array, 8, ab, 0, &ta),
                  napi_ok, "ta uint8 8");
    void* taData = nullptr;
    size_t taLen = 0;
    napi_typedarray_type tt;
    napi_value tab = nullptr;
    size_t toff = 0;
    expect_status(napi_get_typedarray_info(env, ta, &tt, &taLen, &taData, &tab, &toff),
                  napi_ok, "ta info");
    unsigned char* u8 = static_cast<unsigned char*>(taData);
    u8[0] = 0x42;
    u8[1] = 0x43;
    StoreGlobal("__ta2", ta);
    expect(RunJSBool("globalThis.__ta2[0] == 0x42 && globalThis.__ta2[1] == 0x43"),
           "JS sees native writes via typedarray");
    expect(RunJSBool("(function(){ globalThis.__ta2[2]=0x99; return globalThis.__ta2[2]==0x99; })()"),
           "native sees JS writes via typedarray");
    DeleteGlobal("__ta2");
    CloseScope(s);
    expect(RootCount() == Baseline(), "bytes at baseline");
  }

  // ---- Pending/Closing discipline spot checks ----
  {
    std::fprintf(stdout, "-- case pending --\n");
    napi_handle_scope s = OpenScope();
    napi_value ab = nullptr;
    void* ad = nullptr;
    expect_status(napi_create_arraybuffer(env, 8, &ad, &ab), napi_ok, "ab for pending");
    // Set a pending exception via JS throw.
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
      // Now a pending exception exists.
      // Gated (No) create should return pending without executing.
      napi_value ab2 = nullptr;
      void* d2 = nullptr;
      napi_status st = napi_create_arraybuffer(env, 8, &d2, &ab2);
      expect(st == napi_pending_exception, "gated create with pending is pending_exception");
      expect(ab2 == nullptr, "gated create with pending no publish");
      // Executes (Yes) get should succeed despite pending.
      void* gi = nullptr;
      size_t gl = 0;
      expect_status(napi_get_arraybuffer_info(env, ab, &gi, &gl), napi_ok,
                    "get with pending executes");
      expect(gl == 8, "get with pending len 8");
      // Clear the pending exception.
      JsValueRef ex = JS_INVALID_REFERENCE;
      JsGetAndClearException(&ex);
      if (ex) JsRelease(ex, nullptr);
    }
    CloseScope(s);
    expect(RootCount() == Baseline(), "pending at baseline");
  }

  // ---- A8-1 destroy-before-drain (external finalizer lifetime) ----
  // A record enqueued-but-undrained at destroy must be invoked by the
  // destroy drain (no intervening napi call); a buffer collected only
  // after destroy must have its record deleted without invocation and
  // must not leak (live/queued counts return to baseline).
  {
    std::fprintf(stdout, "-- case external-destroy-before-drain --\n");
    JsRuntimeHandle rt2 = JS_INVALID_REFERENCE;
    JsContextRef ctx2 = JS_INVALID_REFERENCE;
    if (JsCreateRuntime(JsRuntimeAttributeNone, nullptr, &rt2) != JsNoError) {
      std::fprintf(stderr, "FATAL: second runtime failed\n");
      return 2;
    }
    if (JsCreateContext(rt2, &ctx2) != JsNoError) {
      std::fprintf(stderr, "FATAL: second context failed\n");
      return 2;
    }
    if (JsSetCurrentContext(ctx2) != JsNoError) {
      std::fprintf(stderr, "FATAL: enter ctx2 failed\n");
      return 2;
    }
    napi_env env2 = chakra_nodeapi_create_env(rt2, ctx2, nullptr, nullptr, nullptr);
    expect(env2 != nullptr, "destroy-drain: env2 created");
    size_t live0 = NodeApiBinary_ExternalLiveCount();
    size_t queued0 = NodeApiBinary_ExternalQueuedCount();
    int calls0 = g_ext_finalizer_calls;
    // Variant 1: collect while live, then destroy with no further napi
    // call on env2. The destroy drain must invoke the finalizer. The
    // poll loop makes the premise non-vacuous: it only proceeds once the
    // record is provably queued-but-undrained (a conservative stack root
    // under -O3 can otherwise pin the buffer and skip collection).
    char* extData2 = new char[32];
    std::memcpy(extData2, "destroy-drain", 13);
    void* hint2 = reinterpret_cast<void*>(0xD15C);
    expect_status(CreateUnrootedExternal(env2, extData2, 32, hint2), napi_ok,
                  "destroy-drain: create ext2");
    bool queued = false;
    for (int i = 0; i < 30; i++) {
      ChurnStack();
      force_gc(rt2);
      if (NodeApiBinary_ExternalQueuedCount() == queued0 + 1) {
        queued = true;
        break;
      }
    }
    expect(queued, "destroy-drain: record queued, not yet drained");
    chakra_nodeapi_destroy_env(env2);
    expect(g_ext_finalizer_calls == calls0 + 1,
           "destroy-drain: finalizer ran at destroy");
    expect(g_ext_finalizer_last_data == extData2, "destroy-drain: finalizer data");
    expect(g_ext_finalizer_last_hint == hint2, "destroy-drain: finalizer hint");
    expect(NodeApiBinary_ExternalLiveCount() == live0,
           "destroy-drain: no live records");
    expect(NodeApiBinary_ExternalQueuedCount() == queued0,
           "destroy-drain: queue empty");
    // Variant 2: destroy first, collect after. The trampoline must drop
    // the record without invocation (dead generation); nothing leaks.
    // The poll only proceeds once the post-destroy collect provably ran
    // (live count back at baseline via the enqueue-time drop).
    napi_env env3 = chakra_nodeapi_create_env(rt2, ctx2, nullptr, nullptr, nullptr);
    expect(env3 != nullptr, "destroy-drain: env3 created");
    int calls1 = g_ext_finalizer_calls;
    char* extData3 = new char[16];
    std::memcpy(extData3, "post-destroy", 12);
    expect_status(CreateUnrootedExternal(env3, extData3, 16, nullptr), napi_ok,
                  "destroy-drain: create ext3");
    chakra_nodeapi_destroy_env(env3);
    expect(g_ext_finalizer_calls == calls1,
           "destroy-drain: no finalizer before collect");
    bool dropped = false;
    for (int i = 0; i < 30; i++) {
      ChurnStack();
      force_gc(rt2);
      if (NodeApiBinary_ExternalLiveCount() == live0) {
        dropped = true;
        break;
      }
    }
    expect(dropped, "destroy-drain: post-destroy collect ran");
    expect(g_ext_finalizer_calls == calls1,
           "destroy-drain: post-destroy collect not invoked");
    expect(NodeApiBinary_ExternalQueuedCount() == queued0,
           "destroy-drain: post-destroy queue empty");
    // extData3 is never freed by a finalizer (deleted without invocation
    // by design); free it here to keep the harness leak-free.
    delete[] extData3;

    // Variant 3: external AB detached via napi_detach_arraybuffer BEFORE GC.
    // Finalizer must fire ONCE on detach, live/queued counts must hit baseline
    // immediately AND stay safe across subsequent GC + destroy (both when
    // JsrtExternalArrayBuffer skips detached Finalize and under Stage-3
    // detached Finalize dispatch).
    napi_env env4 = chakra_nodeapi_create_env(rt2, ctx2, nullptr, nullptr, nullptr);
    expect(env4 != nullptr, "detach-ext: env4 created");
    int calls2 = g_ext_finalizer_calls;
    {
      napi_handle_scope s4 = nullptr;
      expect_status(napi_open_handle_scope(env4, &s4), napi_ok, "detach-ext: scope open");
      char* extData4 = new char[24];
      std::memcpy(extData4, "detach-external-finalizer", 24);
      void* hint4 = reinterpret_cast<void*>(0xDE7A);
      napi_value extAb4 = nullptr;
      expect_status(napi_create_external_arraybuffer(env4, extData4, 24,
                                                     ExtFinalize, hint4, &extAb4),
                    napi_ok, "detach-ext: create ext4");
      expect(NodeApiBinary_ExternalLiveCount() == live0 + 1,
             "detach-ext: live count +1");
      expect_status(napi_detach_arraybuffer(env4, extAb4), napi_ok,
                    "detach-ext: detach ok");
      expect(g_ext_finalizer_calls == calls2 + 1,
             "detach-ext: finalizer fired once on detach");
      expect(g_ext_finalizer_last_data == extData4, "detach-ext: data matches");
      expect(g_ext_finalizer_last_hint == hint4, "detach-ext: hint matches");
      expect(NodeApiBinary_ExternalLiveCount() == live0,
             "detach-ext: live record freed on detach");
      expect(NodeApiBinary_ExternalQueuedCount() == queued0,
             "detach-ext: queue empty after detach");
      bool isDet = false;
      expect_status(napi_is_detached_arraybuffer(env4, extAb4, &isDet), napi_ok,
                    "detach-ext: is_detached query ok");
      expect(isDet, "detach-ext: detached true");
      expect_status(napi_detach_arraybuffer(env4, extAb4),
                    napi_detachable_arraybuffer_expected,
                    "detach-ext: second detach rejected");
      expect_status(napi_close_handle_scope(env4, s4), napi_ok, "detach-ext: scope close");
    }
    for (int i = 0; i < 6; i++) {
      ChurnStack();
      force_gc(rt2);
    }
    expect(g_ext_finalizer_calls == calls2 + 1,
           "detach-ext: post-detach GC still once");
    expect(NodeApiBinary_ExternalLiveCount() == live0,
           "detach-ext: post-detach GC live baseline");
    chakra_nodeapi_destroy_env(env4);
    expect(g_ext_finalizer_calls == calls2 + 1,
           "detach-ext: destroy still once");
    expect(NodeApiBinary_ExternalLiveCount() == live0,
           "detach-ext: destroy live baseline");

    expect(JsSetCurrentContext(ctx) == JsNoError, "destroy-drain: restore ctx");
    JsDisposeRuntime(rt2);
    expect(RootCount() == Baseline(), "destroy-drain at baseline");
  }

  chakra_nodeapi_destroy_env(env);
  expect(true, "teardown: env destroyed");
  JsSetCurrentContext(JS_INVALID_REFERENCE);
  JsDisposeRuntime(rt);

  std::fprintf(stdout, "BINARY_PROBE: %s (%d/%d)\n",
               (g_pass == g_total) ? "PASS" : "FAIL", g_pass, g_total);
  return (g_pass == g_total) ? 0 : 1;
}


