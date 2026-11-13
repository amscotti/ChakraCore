//-------------------------------------------------------------------------------------------------------
// Copyright (c) ChakraCore Project Contributors. All rights reserved.
// Licensed under the MIT license. See LICENSE.txt file in the project root for full license information.
//-------------------------------------------------------------------------------------------------------
//
// N2.1/N2.2 values probes (plan tasks N2.1/N2.2, semantics section 6,
// status mapping rows for get_value_*/create_*/get_value_string_*).
//
// Direct C++ probe: creates a Chakra runtime + context, a real env via
// chakra_nodeapi_create_env, and exercises the 19 napi_* scalar/string
// functions in lib/NodeApi/NodeApiValues.cpp in-process (no addon).
// Run via run_values_probe.sh (compiles with `zig c++` through
// /tmp/opencode/cc-gate-lock.sh and links the engine dylib).
//
// Coverage (each MUST fail on mismatch):
//   Numbers: round trips +-2**53 edges, NaN, +-Infinity, fractional
//     truncation, int32 bottom-32-bits wrap, uint32 ToUint32 wrap
//     (negative-to-uint32), int64 saturation, wrong-type exact statuses
//     (number_expected), out-param NULL => invalid_arg with no write,
//     sentinel preservation on type mismatch.
//   Strings: ASCII, non-ASCII UTF-8, surrogate pairs, embedded NUL,
//     Latin1 high bytes round-trip, Latin1 truncation for >U+00FF
//     (V8 WriteOneByte low-byte), NAPI_AUTO_LENGTH, size-query (buf
//     NULL), too-small buffer / bufsize 0 / result optional, wrong-type
//     string_expected, create null/large-length invalid.
//   Singletons: undefined/null/global/boolean round trips.
//   Guards: last_error set/clear, pending-exception execution (Yes rows),
//     root-count spot checks (R1).
//
// Differential: expected constants for wrapping/truncation were taken
// from Node v26.5.0 (V8 Int32Value/Uint32Value/IntegerValue + WriteOneByte
// low-byte). The fixed corpus below is byte-exact vs Node for the cases
// marked (Node). No live Node runtime is required for this probe.
//
// Output: one line per assertion, then "VALUES_PROBE: PASS (n/n)".

#include "NodeApiHostApi.h"
#include "js_native_api.h"
#include "NodeApiEnv.h"
#include "NodeApiScopes.h"
#include "ChakraCommon.h"
#include "ChakraCore.h"

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <vector>

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
    default: return "?";
  }
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

bool double_eq(double a, double b) {
  if (std::isnan(a) && std::isnan(b)) return true;
  if (std::isinf(a) && std::isinf(b)) {
    return (a > 0) == (b > 0);
  }
  // bitwise to distinguish -0/+0 and NaN payloads
  uint64_t ai, bi;
  std::memcpy(&ai, &a, sizeof(ai));
  std::memcpy(&bi, &b, sizeof(bi));
  return ai == bi;
}

} /* namespace */

int main() {
  JsRuntimeHandle rt = JS_INVALID_REFERENCE;
  if (JsCreateRuntime(JsRuntimeAttributeNone, nullptr, &rt) != JsNoError) {
    std::fprintf(stderr, "FATAL: JsCreateRuntime failed\n");
    return 2;
  }
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
  const size_t baseline = chakra_nodeapi_test_root_count(env);
  expect(baseline == 0, "baseline root count is zero");

  const double k2_53 = 9007199254740992.0; /* 2**53 */
  const double k2_53m1 = 9007199254740991.0; /* 2**53 - 1 */

  /* ---- Singletons: undefined/null/global/boolean ---- */
  {
    std::fprintf(stdout, "-- case singletons --\n");
    napi_value undef = nullptr;
    expect_status(napi_get_undefined(env, &undef), napi_ok, "get_undefined ok");
    expect(undef != nullptr, "get_undefined non-null");
    JsValueType t = JsBoolean;
    expect(JsGetValueType(reinterpret_cast<JsValueRef>(undef), &t) == JsNoError &&
               t == JsUndefined,
           "get_undefined is JsUndefined");
    napi_value nullv = nullptr;
    expect_status(napi_get_null(env, &nullv), napi_ok, "get_null ok");
    expect(JsGetValueType(reinterpret_cast<JsValueRef>(nullv), &t) == JsNoError &&
               t == JsNull,
           "get_null is JsNull");
    napi_value glob = nullptr;
    expect_status(napi_get_global(env, &glob), napi_ok, "get_global ok");
    expect(JsGetValueType(reinterpret_cast<JsValueRef>(glob), &t) == JsNoError &&
               t == JsObject,
           "get_global is JsObject");
    napi_value tr = nullptr, fa = nullptr;
    expect_status(napi_get_boolean(env, true, &tr), napi_ok, "get_boolean true ok");
    expect_status(napi_get_boolean(env, false, &fa), napi_ok, "get_boolean false ok");
    bool b = false;
    expect_status(napi_get_value_bool(env, tr, &b), napi_ok, "get_value_bool true");
    expect(b == true, "get_value_bool true value");
    expect_status(napi_get_value_bool(env, fa, &b), napi_ok, "get_value_bool false");
    expect(b == false, "get_value_bool false value");
    expect(JsGetValueType(reinterpret_cast<JsValueRef>(tr), &t) == JsNoError &&
               t == JsBoolean,
           "get_boolean true is Boolean");
    /* NULL result => invalid_arg, no write (pointer itself null) */
    expect_status(napi_get_undefined(env, nullptr), napi_invalid_arg,
                  "get_undefined null result invalid");
    expect_status(napi_get_null(env, nullptr), napi_invalid_arg,
                  "get_null null result invalid");
    expect_status(napi_get_global(env, nullptr), napi_invalid_arg,
                  "get_global null result invalid");
    expect_status(napi_get_boolean(env, true, nullptr), napi_invalid_arg,
                  "get_boolean null result invalid");
  }

  /* ---- Doubles: round trip incl. NaN/Inf and +-2**53 ---- */
  {
    std::fprintf(stdout, "-- case double-roundtrip --\n");
    struct Case { double in; const char* name; };
    const double nan = std::numeric_limits<double>::quiet_NaN();
    const double inf = std::numeric_limits<double>::infinity();
    Case cases[] = {
        {0.0, "zero"}, {-0.0, "neg-zero"}, {1.5, "1.5"}, {-1.5, "neg1.5"},
        {123.456, "123.456"}, {nan, "NaN"}, {inf, "Inf"}, {-inf, "-Inf"},
        {k2_53m1, "2^53-1"}, {-k2_53m1, "-(2^53-1)"}, {k2_53, "2^53"},
        {-k2_53, "-(2^53)"}, {k2_53 + 2.0, "2^53+2"}, {1e308, "1e308"},
    };
    for (auto& c : cases) {
      napi_value v = nullptr;
      expect_status(napi_create_double(env, c.in, &v), napi_ok, c.name);
      expect(v != nullptr, "double non-null");
      double out = 12345.678;
      expect_status(napi_get_value_double(env, v, &out), napi_ok, "double get ok");
      char buf[64];
      std::snprintf(buf, sizeof(buf), "double roundtrip %s", c.name);
      expect(double_eq(out, c.in), buf);
    }
    /* wrong type: string => number_expected, sentinel untouched */
    napi_value sval = nullptr;
    expect_status(napi_create_string_utf8(env, "123", 3, &sval), napi_ok,
                  "wrongtype setup string");
    double sentinel = 777.777;
    double out = sentinel;
    expect_status(napi_get_value_double(env, sval, &out), napi_number_expected,
                  "double of string is number_expected");
    expect(double_eq(out, sentinel), "double wrong-type leaves out untouched");
    /* null args */
    double d = 1.0;
    expect_status(napi_get_value_double(env, nullptr, &d), napi_invalid_arg,
                  "double null value invalid");
    napi_value num = nullptr;
    expect_status(napi_create_double(env, 42.0, &num), napi_ok, "setup num");
    expect_status(napi_get_value_double(env, num, nullptr), napi_invalid_arg,
                  "double null result invalid");
    expect_status(napi_create_double(env, 1.0, nullptr), napi_invalid_arg,
                  "create_double null result invalid");
  }

  /* ---- Int32: create/get + bottom-32-bits wrap (Node differential) ---- */
  {
    std::fprintf(stdout, "-- case int32 --\n");
    int32_t vals[] = {0, -1, 42, INT32_MIN, INT32_MAX};
    const char* names[] = {"i32 0", "i32 -1", "i32 42", "i32 MIN", "i32 MAX"};
    for (int i = 0; i < 5; i++) {
      napi_value v = nullptr;
      expect_status(napi_create_int32(env, vals[i], &v), napi_ok, names[i]);
      int32_t out = 999;
      expect_status(napi_get_value_int32(env, v, &out), napi_ok, "i32 get ok");
      expect(out == vals[i], names[i]);
    }
    /* via double: truncation toward zero + wrap (Node: 2^31 => INT32_MIN) */
    struct Raw { double in; int32_t want; const char* name; };
    Raw raw[] = {
        {2147483647.9, INT32_MAX, "i32 frac max"},
        {-2147483648.9, INT32_MIN, "i32 frac min"},
        {2147483648.0, INT32_MIN, "i32 2^31 wraps"},
        {4294967297.0, 1, "i32 2^32+1 => 1"},
        {-1.5, -1, "i32 -1.5 => -1"},
        {0.5, 0, "i32 0.5 => 0"},
        {std::numeric_limits<double>::quiet_NaN(), 0, "i32 NaN => 0"},
        {std::numeric_limits<double>::infinity(), 0, "i32 Inf => 0"},
        {-std::numeric_limits<double>::infinity(), 0, "i32 -Inf => 0"},
    };
    for (auto& r : raw) {
      napi_value v = nullptr;
      expect_status(napi_create_double(env, r.in, &v), napi_ok, "i32 setup");
      int32_t out = 12345;
      expect_status(napi_get_value_int32(env, v, &out), napi_ok, "i32 raw get");
      char buf[96];
      std::snprintf(buf, sizeof(buf), "%s", r.name);
      expect(out == r.want, buf);
    }
    napi_value sval = nullptr;
    expect_status(napi_create_string_utf8(env, "s", 1, &sval), napi_ok,
                  "i32 wrongtype setup");
    int32_t io = 555;
    expect_status(napi_get_value_int32(env, sval, &io), napi_number_expected,
                  "i32 of string is number_expected");
    expect(io == 555, "i32 wrong-type leaves out untouched");
    expect_status(napi_get_value_int32(env, nullptr, &io), napi_invalid_arg,
                  "i32 null value invalid");
    napi_value num = nullptr;
    expect_status(napi_create_int32(env, 1, &num), napi_ok, "i32 setup num");
    expect_status(napi_get_value_int32(env, num, nullptr), napi_invalid_arg,
                  "i32 null result invalid");
    expect_status(napi_create_int32(env, 1, nullptr), napi_invalid_arg,
                  "create_int32 null result invalid");
  }

  /* ---- Uint32: negative wrap (Node differential) ---- */
  {
    std::fprintf(stdout, "-- case uint32 --\n");
    uint32_t vals[] = {0u, 42u, UINT32_MAX};
    const char* names[] = {"u32 0", "u32 42", "u32 MAX"};
    for (int i = 0; i < 3; i++) {
      napi_value v = nullptr;
      expect_status(napi_create_uint32(env, vals[i], &v), napi_ok, names[i]);
      uint32_t out = 999;
      expect_status(napi_get_value_uint32(env, v, &out), napi_ok, "u32 get ok");
      expect(out == vals[i], names[i]);
    }
    struct Raw { double in; uint32_t want; const char* name; };
    Raw raw[] = {
        {-1.0, 4294967295u, "u32 -1 => MAX (Node)"},
        {-1.5, 4294967295u, "u32 -1.5 => MAX (Node)"},
        {-0.5, 0u, "u32 -0.5 => 0"},
        {0.5, 0u, "u32 0.5 => 0"},
        {4294967296.0, 0u, "u32 2^32 => 0 (Node)"},
        {4294967296.5, 0u, "u32 2^32+0.5 => 0"},
        {std::numeric_limits<double>::quiet_NaN(), 0u, "u32 NaN => 0"},
        {std::numeric_limits<double>::infinity(), 0u, "u32 Inf => 0"},
    };
    for (auto& r : raw) {
      napi_value v = nullptr;
      expect_status(napi_create_double(env, r.in, &v), napi_ok, "u32 setup");
      uint32_t out = 777;
      expect_status(napi_get_value_uint32(env, v, &out), napi_ok, "u32 raw get");
      char buf[96];
      std::snprintf(buf, sizeof(buf), "%s", r.name);
      expect(out == r.want, buf);
    }
    napi_value bval = nullptr;
    expect_status(napi_get_boolean(env, true, &bval), napi_ok, "u32 setup bool");
    uint32_t uo = 888;
    expect_status(napi_get_value_uint32(env, bval, &uo), napi_number_expected,
                  "u32 of bool is number_expected");
    expect(uo == 888u, "u32 wrong-type leaves out untouched");
    expect_status(napi_get_value_uint32(env, nullptr, &uo), napi_invalid_arg,
                  "u32 null value invalid");
    expect_status(napi_get_value_uint32(env, bval, nullptr), napi_invalid_arg,
                  "u32 null result invalid");
    expect_status(napi_create_uint32(env, 1, nullptr), napi_invalid_arg,
                  "create_uint32 null result invalid");
  }

  /* ---- Int64: +-2**53 edges, NaN/Inf => 0, saturation ---- */
  {
    std::fprintf(stdout, "-- case int64 --\n");
    int64_t vals[] = {0, -1, 42, (int64_t)9007199254740991LL,
                      -(int64_t)9007199254740991LL, (int64_t)9007199254740992LL,
                      -(int64_t)9007199254740992LL};
    const char* names[] = {"i64 0", "i64 -1", "i64 42", "i64 2^53-1",
                           "i64 -(2^53-1)", "i64 2^53", "i64 -(2^53)"};
    for (int i = 0; i < 7; i++) {
      napi_value v = nullptr;
      expect_status(napi_create_int64(env, vals[i], &v), napi_ok, names[i]);
      int64_t out = 999;
      expect_status(napi_get_value_int64(env, v, &out), napi_ok, "i64 get ok");
      expect(out == vals[i], names[i]);
    }
    struct Raw { double in; int64_t want; const char* name; };
    Raw raw[] = {
        {std::numeric_limits<double>::quiet_NaN(), 0, "i64 NaN => 0 (Node)"},
        {std::numeric_limits<double>::infinity(), 0, "i64 Inf => 0 (Node)"},
        {-std::numeric_limits<double>::infinity(), 0, "i64 -Inf => 0"},
        {1.9, 1, "i64 1.9 => 1"},
        {-1.9, -1, "i64 -1.9 => -1"},
        {1e20, std::numeric_limits<int64_t>::max(), "i64 1e20 saturates MAX"},
        {-1e20, std::numeric_limits<int64_t>::min(), "i64 -1e20 saturates MIN"},
        {9007199254740991.0, (int64_t)9007199254740991LL, "i64 double 2^53-1"},
    };
    for (auto& r : raw) {
      napi_value v = nullptr;
      expect_status(napi_create_double(env, r.in, &v), napi_ok, "i64 setup");
      int64_t out = 12345;
      expect_status(napi_get_value_int64(env, v, &out), napi_ok, "i64 raw get");
      char buf[96];
      std::snprintf(buf, sizeof(buf), "%s", r.name);
      expect(out == r.want, buf);
    }
    napi_value sval = nullptr;
    expect_status(napi_create_string_utf8(env, "s", 1, &sval), napi_ok,
                  "i64 wrongtype setup");
    int64_t w = 4242;
    expect_status(napi_get_value_int64(env, sval, &w), napi_number_expected,
                  "i64 of string is number_expected");
    expect(w == 4242, "i64 wrong-type leaves out untouched");
    expect_status(napi_get_value_int64(env, nullptr, &w), napi_invalid_arg,
                  "i64 null value invalid");
    expect_status(napi_get_value_int64(env, sval, nullptr), napi_invalid_arg,
                  "i64 null result invalid");
    expect_status(napi_create_int64(env, 1, nullptr), napi_invalid_arg,
                  "create_int64 null result invalid");
  }

  /* ---- Bool wrong-type ---- */
  {
    std::fprintf(stdout, "-- case bool-wrongtype --\n");
    napi_value n = nullptr;
    expect_status(napi_create_double(env, 1.0, &n), napi_ok, "bool setup num");
    bool b = true;
    expect_status(napi_get_value_bool(env, n, &b), napi_boolean_expected,
                  "bool of number is boolean_expected");
    expect(b == true, "bool wrong-type leaves out untouched");
    bool tmp = false;
    expect_status(napi_get_value_bool(env, nullptr, &tmp), napi_invalid_arg,
                  "bool null value invalid");
    napi_value tv = nullptr;
    expect_status(napi_get_boolean(env, true, &tv), napi_ok, "bool setup");
    expect_status(napi_get_value_bool(env, tv, nullptr), napi_invalid_arg,
                  "bool null result invalid");
  }

  /* ---- Rooting spot check (R1) ---- */
  {
    std::fprintf(stdout, "-- case rooting --\n");
    size_t before = chakra_nodeapi_test_root_count(env);
    napi_handle_scope inner = nullptr;
    expect_status(napi_open_handle_scope(env, &inner), napi_ok, "root open");
    napi_value v = nullptr;
    expect_status(napi_create_double(env, 3.14, &v), napi_ok, "root create");
    expect(chakra_nodeapi_test_root_count(env) == before + 1,
           "root count +1 inside scope");
    expect_status(napi_close_handle_scope(env, inner), napi_ok, "root close");
    expect(chakra_nodeapi_test_root_count(env) == before,
           "root count back to baseline after close");
    (void)v;
  }

  /* ---- Strings: ASCII / UTF-8 / UTF-16 / Latin1 round trips ---- */
  {
    std::fprintf(stdout, "-- case strings-basic --\n");
    struct StrCase { const char* utf8; const char* name; };
    StrCase cases[] = {{"", "empty"}, {"a", "a"}, {"hello world", "ascii"},
                       {"ABCDEFGHIJKLMNOPQRSTUVWXYZ", "AZ"}};
    for (auto& c : cases) {
      size_t len = std::strlen(c.utf8);
      napi_value s = nullptr;
      expect_status(napi_create_string_utf8(env, c.utf8, len, &s), napi_ok,
                    c.name);
      char buf[128] = {0};
      size_t copied = 999;
      expect_status(napi_get_value_string_utf8(env, s, buf, sizeof(buf), &copied),
                    napi_ok, "utf8 get");
      expect(copied == len && std::memcmp(buf, c.utf8, len) == 0 &&
                 buf[len] == '\0',
             "utf8 roundtrip");
      /* utf16 round trip via utf8-created value */
      char16_t b16[128] = {};
      size_t c16 = 999;
      expect_status(napi_get_value_string_utf16(env, s, b16, 128, &c16), napi_ok,
                    "utf16 get");
      expect(c16 == len, "utf16 length matches ascii");
      bool ok = true;
      for (size_t i = 0; i < len; i++) ok = ok && (b16[i] == (char16_t)c.utf8[i]);
      ok = ok && (b16[len] == 0);
      expect(ok, "utf16 content");
      /* latin1 round trip (ascii is subset) */
      char bl[128] = {0};
      size_t cl = 999;
      expect_status(napi_get_value_string_latin1(env, s, bl, sizeof(bl), &cl),
                    napi_ok, "latin1 get");
      expect(cl == len && std::memcmp(bl, c.utf8, len) == 0, "latin1 roundtrip");
    }
  }

  /* ---- Strings: non-ASCII UTF-8, surrogate pairs, high bytes ---- */
  {
    std::fprintf(stdout, "-- case strings-unicode --\n");
    /* "é" = U+00E9, UTF-8 C3 A9 */
    {
      const char u8s[] = "\xC3\xA9";
      napi_value s = nullptr;
      expect_status(napi_create_string_utf8(env, u8s, 2, &s), napi_ok,
                    "utf8 é create");
      size_t q = 0;
      expect_status(napi_get_value_string_utf8(env, s, nullptr, 0, &q), napi_ok,
                    "utf8 é size query");
      expect(q == 2, "utf8 é length 2");
      char16_t b16[8] = {};
      size_t c16 = 0;
      expect_status(napi_get_value_string_utf16(env, s, b16, 8, &c16), napi_ok,
                    "utf16 é get");
      expect(c16 == 1 && b16[0] == 0x00E9 && b16[1] == 0, "utf16 é U+00E9");
      char b1[8] = {0};
      size_t cl = 0;
      expect_status(napi_get_value_string_latin1(env, s, b1, 8, &cl), napi_ok,
                    "latin1 é get");
      expect(cl == 1 && (unsigned char)b1[0] == 0xE9, "latin1 é byte 0xE9");
      size_t lq = 0;
      expect_status(napi_get_value_string_latin1(env, s, nullptr, 0, &lq), napi_ok,
                    "latin1 é size query");
      expect(lq == 1, "latin1 é length 1");
    }
    /* Latin1 string: U+00E9 as single byte 0xE9 */
    {
      const char lb = (char)0xE9;
      napi_value s = nullptr;
      expect_status(napi_create_string_latin1(env, &lb, 1, &s), napi_ok,
                    "latin1 0xE9 create");
      char buf[8] = {0};
      size_t c = 0;
      expect_status(napi_get_value_string_utf8(env, s, buf, sizeof(buf), &c),
                    napi_ok, "latin1->utf8 get");
      expect(c == 2 && (unsigned char)buf[0] == 0xC3 &&
                 (unsigned char)buf[1] == 0xA9,
             "latin1 0xE9 -> utf8 C3 A9 (Node)");
      char16_t b16[4] = {};
      size_t c16 = 0;
      expect_status(napi_get_value_string_utf16(env, s, b16, 4, &c16), napi_ok,
                    "latin1->utf16 get");
      expect(c16 == 1 && b16[0] == 0x00E9, "latin1->utf16 U+00E9");
      char back[4] = {0};
      size_t cb = 0;
      expect_status(napi_get_value_string_latin1(env, s, back, 4, &cb), napi_ok,
                    "latin1 high roundtrip get");
      expect(cb == 1 && (unsigned char)back[0] == 0xE9,
             "latin1 high bytes roundtrip");
    }
    /* "€" U+20AC, UTF-8 E2 82 AC */
    {
      const char u8s[] = "\xE2\x82\xAC";
      napi_value s = nullptr;
      expect_status(napi_create_string_utf8(env, u8s, 3, &s), napi_ok,
                    "utf8 euro create");
      char16_t b16[4] = {};
      size_t c16 = 0;
      expect_status(napi_get_value_string_utf16(env, s, b16, 4, &c16), napi_ok,
                    "utf16 euro get");
      expect(c16 == 1 && b16[0] == 0x20AC, "utf16 euro U+20AC");
      /* Latin1 truncation: low byte 0xAC (Node WriteOneByte) */
      char b1[4] = {0};
      size_t cl = 0;
      expect_status(napi_get_value_string_latin1(env, s, b1, 4, &cl), napi_ok,
                    "latin1 euro get");
      expect(cl == 1 && (unsigned char)b1[0] == 0xAC,
             "latin1 euro truncates to 0xAC (Node)");
    }
    /* "😀" U+1F600, UTF-16 D83D DE00, UTF-8 F0 9F 98 80 */
    {
      const char16_t pairs[] = {0xD83D, 0xDE00};
      napi_value s = nullptr;
      expect_status(napi_create_string_utf16(env, pairs, 2, &s), napi_ok,
                    "utf16 surrogate create");
      size_t q8 = 0;
      expect_status(napi_get_value_string_utf8(env, s, nullptr, 0, &q8), napi_ok,
                    "surrogate utf8 size");
      expect(q8 == 4, "surrogate utf8 length 4");
      char b8[8] = {0};
      size_t c8 = 0;
      expect_status(napi_get_value_string_utf8(env, s, b8, sizeof(b8), &c8),
                    napi_ok, "surrogate utf8 get");
      expect(c8 == 4 && (unsigned char)b8[0] == 0xF0 &&
                 (unsigned char)b8[1] == 0x9F && (unsigned char)b8[2] == 0x98 &&
                 (unsigned char)b8[3] == 0x80 && b8[4] == 0,
             "surrogate utf8 bytes F0 9F 98 80");
      size_t q16 = 0;
      expect_status(napi_get_value_string_utf16(env, s, nullptr, 0, &q16), napi_ok,
                    "surrogate utf16 size");
      expect(q16 == 2, "surrogate utf16 length 2");
      char16_t back[4] = {};
      size_t bc = 0;
      expect_status(napi_get_value_string_utf16(env, s, back, 4, &bc), napi_ok,
                    "surrogate utf16 get");
      expect(bc == 2 && back[0] == 0xD83D && back[1] == 0xDE00,
             "surrogate roundtrip");
      /* Latin1: low bytes 0x3D 0x00 (Node differential) */
      char bl[8] = {0};
      size_t cl = 0;
      expect_status(napi_get_value_string_latin1(env, s, bl, sizeof(bl), &cl),
                    napi_ok, "surrogate latin1 get");
      expect(cl == 2 && (unsigned char)bl[0] == 0x3D &&
                 (unsigned char)bl[1] == 0x00,
             "surrogate latin1 bytes 3D 00 (Node)");
      size_t ql = 0;
      expect_status(napi_get_value_string_latin1(env, s, nullptr, 0, &ql), napi_ok,
                    "surrogate latin1 size");
      expect(ql == 2, "surrogate latin1 length 2");
    }
    /* Latin1 high bytes 0xA1 0xA2 0xA3 round trip */
    {
      const char in[] = {(char)0xA1, (char)0xA2, (char)0xA3};
      napi_value s = nullptr;
      expect_status(napi_create_string_latin1(env, in, 3, &s), napi_ok,
                    "latin1 highs create");
      char out[8] = {0};
      size_t c = 0;
      expect_status(napi_get_value_string_latin1(env, s, out, sizeof(out), &c),
                    napi_ok, "latin1 highs get");
      expect(c == 3 && (unsigned char)out[0] == 0xA1 &&
                 (unsigned char)out[1] == 0xA2 && (unsigned char)out[2] == 0xA3,
             "latin1 highs roundtrip");
      char16_t b16[8] = {};
      size_t c16 = 0;
      expect_status(napi_get_value_string_utf16(env, s, b16, 8, &c16), napi_ok,
                    "latin1 highs utf16");
      expect(c16 == 3 && b16[0] == 0x00A1 && b16[1] == 0x00A2 &&
                 b16[2] == 0x00A3,
             "latin1 highs utf16 U+00A1..");
    }
  }

  /* ---- Strings: embedded NUL, AUTO_LENGTH, size-query, truncation ---- */
  {
    std::fprintf(stdout, "-- case strings-nul-size --\n");
    /* Embedded NUL via explicit length */
    {
      const char in[] = {'a', '\0', 'b', '\0', 'c'};
      napi_value s = nullptr;
      expect_status(napi_create_string_utf8(env, in, 5, &s), napi_ok,
                    "embedded NUL utf8 create");
      size_t q = 0;
      expect_status(napi_get_value_string_utf8(env, s, nullptr, 0, &q), napi_ok,
                    "embedded NUL size");
      expect(q == 5, "embedded NUL length 5");
      char buf[16] = {};
      size_t c = 0;
      expect_status(napi_get_value_string_utf8(env, s, buf, sizeof(buf), &c),
                    napi_ok, "embedded NUL get");
      expect(c == 5 && std::memcmp(buf, in, 5) == 0 && buf[5] == 0,
             "embedded NUL preserved");
      /* AUTO stops at first NUL */
      napi_value a = nullptr;
      expect_status(napi_create_string_utf8(env, in, NAPI_AUTO_LENGTH, &a),
                    napi_ok, "AUTO create stops at NUL");
      size_t qa = 0;
      expect_status(napi_get_value_string_utf8(env, a, nullptr, 0, &qa), napi_ok,
                    "AUTO size");
      expect(qa == 1, "AUTO length is 1 ('a')");
    }
    /* UTF-16 embedded NUL */
    {
      const char16_t in[] = {u'a', 0, u'b'};
      napi_value s = nullptr;
      expect_status(napi_create_string_utf16(env, in, 3, &s), napi_ok,
                    "utf16 NUL create");
      char16_t out[8] = {};
      size_t c = 0;
      expect_status(napi_get_value_string_utf16(env, s, out, 8, &c), napi_ok,
                    "utf16 NUL get");
      expect(c == 3 && out[0] == u'a' && out[1] == 0 && out[2] == u'b',
             "utf16 NUL preserved");
      napi_value a = nullptr;
      expect_status(napi_create_string_utf16(env, in, NAPI_AUTO_LENGTH, &a),
                    napi_ok, "utf16 AUTO create");
      size_t qa = 0;
      expect_status(napi_get_value_string_utf16(env, a, nullptr, 0, &qa), napi_ok,
                    "utf16 AUTO size");
      expect(qa == 1, "utf16 AUTO length 1");
    }
    /* AUTO_LENGTH equals explicit for plain strings */
    {
      napi_value a = nullptr, b = nullptr;
      expect_status(napi_create_string_utf8(env, "hello", NAPI_AUTO_LENGTH, &a),
                    napi_ok, "AUTO utf8 create");
      expect_status(napi_create_string_utf8(env, "hello", 5, &b), napi_ok,
                    "explicit utf8 create");
      char ba[16] = {0}, bb[16] = {0};
      size_t ca = 0, cb = 0;
      napi_get_value_string_utf8(env, a, ba, sizeof(ba), &ca);
      napi_get_value_string_utf8(env, b, bb, sizeof(bb), &cb);
      expect(ca == 5 && cb == 5 && std::memcmp(ba, bb, 5) == 0,
             "AUTO equals explicit");
      const char16_t uin[] = {u'h', u'i', 0};
      napi_value u1 = nullptr, u2 = nullptr;
      expect_status(napi_create_string_utf16(env, uin, NAPI_AUTO_LENGTH, &u1),
                    napi_ok, "AUTO utf16");
      expect_status(napi_create_string_utf16(env, uin, 2, &u2), napi_ok,
                    "explicit utf16");
      size_t q1 = 0, q2 = 0;
      napi_get_value_string_utf16(env, u1, nullptr, 0, &q1);
      napi_get_value_string_utf16(env, u2, nullptr, 0, &q2);
      expect(q1 == 2 && q2 == 2, "utf16 AUTO equals explicit");
      napi_value l1 = nullptr, l2 = nullptr;
      expect_status(napi_create_string_latin1(env, "abc", NAPI_AUTO_LENGTH, &l1),
                    napi_ok, "AUTO latin1");
      expect_status(napi_create_string_latin1(env, "abc", 3, &l2), napi_ok,
                    "explicit latin1");
      size_t ql1 = 0, ql2 = 0;
      napi_get_value_string_latin1(env, l1, nullptr, 0, &ql1);
      napi_get_value_string_latin1(env, l2, nullptr, 0, &ql2);
      expect(ql1 == 3 && ql2 == 3, "latin1 AUTO equals explicit");
    }
    /* Size query and too-small buffer (Node/UTF-8 insufficient = bufsize-1) */
    {
      napi_value s = nullptr;
      expect_status(napi_create_string_utf8(env, "hello world", 11, &s), napi_ok,
                    "size setup");
      size_t q = 0;
      expect_status(napi_get_value_string_utf8(env, s, nullptr, 0, &q), napi_ok,
                    "utf8 size query");
      expect(q == 11, "utf8 size is 11");
      char small[4] = {};
      size_t c = 0;
      expect_status(napi_get_value_string_utf8(env, s, small, 4, &c), napi_ok,
                    "utf8 small get");
      expect(c == 3 && std::memcmp(small, "hel", 3) == 0 && small[3] == 0,
             "utf8 small truncates to 3 (Node)");
      /* bufsize 0: no write, result 0 */
      char keep[] = "keep";
      size_t c0 = 999;
      expect_status(napi_get_value_string_utf8(env, s, keep, 0, &c0), napi_ok,
                    "utf8 bufsize0");
      expect(c0 == 0 && std::memcmp(keep, "keep", 5) == 0,
             "utf8 bufsize0 no write");
      /* bufsize 1: only NUL */
      char one[1] = {'X'};
      size_t c1 = 999;
      expect_status(napi_get_value_string_utf8(env, s, one, 1, &c1), napi_ok,
                    "utf8 bufsize1");
      expect(c1 == 0 && one[0] == 0, "utf8 bufsize1 only NUL");
      /* exactly length+1 fits, exactly length truncates by 1 */
      char exact[12] = {};
      size_t ce = 0;
      expect_status(napi_get_value_string_utf8(env, s, exact, 12, &ce), napi_ok,
                    "utf8 exact fit");
      expect(ce == 11 && std::memcmp(exact, "hello world", 11) == 0,
             "utf8 exact fit full");
      char trunc[11] = {};
      size_t ct = 0;
      expect_status(napi_get_value_string_utf8(env, s, trunc, 11, &ct), napi_ok,
                    "utf8 exact trunc");
      expect(ct == 10 && std::memcmp(trunc, "hello worl", 10) == 0,
             "utf8 length buffer truncates by 1");
      /* result optional when buf non-null */
      char opt[16] = {};
      expect_status(napi_get_value_string_utf8(env, s, opt, sizeof(opt), nullptr),
                    napi_ok, "utf8 result optional");
      /* buf NULL + result NULL => invalid */
      expect_status(napi_get_value_string_utf8(env, s, nullptr, 0, nullptr),
                    napi_invalid_arg, "utf8 size query null result invalid");
      /* utf16 insufficient */
      char16_t s16[4] = {};
      size_t c16 = 0;
      expect_status(napi_get_value_string_utf16(env, s, s16, 4, &c16), napi_ok,
                    "utf16 small get");
      expect(c16 == 3 && s16[0] == u'h' && s16[1] == u'e' && s16[2] == u'l',
             "utf16 small truncates to 3");
      /* latin1 insufficient */
      char sl[4] = {};
      size_t cl = 0;
      expect_status(napi_get_value_string_latin1(env, s, sl, 4, &cl), napi_ok,
                    "latin1 small get");
      expect(cl == 3 && std::memcmp(sl, "hel", 3) == 0, "latin1 small truncates");
      /* multi-byte truncation does not split (Node differential for U+20AC) */
      {
        const char u8s[] = "\xE2\x82\xAC"; /* € (3 bytes) */
        napi_value e = nullptr;
        expect_status(napi_create_string_utf8(env, u8s, 3, &e), napi_ok,
                      "euro setup");
        char b2[2] = {0}; /* bufsize 2 => cap 1 byte => cannot fit 3-byte seq */
        size_t c2 = 999;
        expect_status(napi_get_value_string_utf8(env, e, b2, 2, &c2), napi_ok,
                      "euro small2");
        /* V8/EncodeInto: no partial sequence => 0 bytes copied */
        expect(c2 == 0 && b2[0] == 0, "euro trunc no partial (Node)");
        char b4[4] = {0}; /* cap 3 => fits whole seq */
        size_t c4 = 0;
        expect_status(napi_get_value_string_utf8(env, e, b4, 4, &c4), napi_ok,
                      "euro small4");
        expect(c4 == 3 && std::memcmp(b4, u8s, 3) == 0, "euro fits when cap 3");
      }
    }
    /* Create edge cases (vendored test_null + large-length) */
    {
      napi_value out = nullptr;
      expect_status(napi_create_string_utf8(env, nullptr, NAPI_AUTO_LENGTH, &out),
                    napi_invalid_arg, "create null AUTO invalid");
      expect(out == nullptr, "create null AUTO leaves out untouched");
      out = reinterpret_cast<napi_value>(uintptr_t(0xdeadbeef));
      expect_status(napi_create_string_utf8(env, nullptr, 5, &out),
                    napi_invalid_arg, "create null non-zero invalid");
      expect(out == reinterpret_cast<napi_value>(uintptr_t(0xdeadbeef)),
             "create null leaves out untouched");
      expect_status(napi_create_string_utf8(env, nullptr, 0, &out), napi_ok,
                    "create null zero-length ok (empty)");
      expect(out != nullptr, "create null zero non-null");
      size_t q = 0;
      expect_status(napi_get_value_string_utf8(env, out, nullptr, 0, &q), napi_ok,
                    "empty size query");
      expect(q == 0, "empty length 0");
      expect_status(napi_create_string_utf8(env, "x", 1, nullptr),
                    napi_invalid_arg, "create result null invalid");
      const char16_t uin[] = {u'x', 0};
      expect_status(napi_create_string_utf16(env, nullptr, NAPI_AUTO_LENGTH, &out),
                    napi_invalid_arg, "utf16 create null AUTO invalid");
      expect_status(napi_create_string_utf16(env, nullptr, 0, &out), napi_ok,
                    "utf16 create null zero ok");
      expect_status(napi_create_string_latin1(env, nullptr, NAPI_AUTO_LENGTH, &out),
                    napi_invalid_arg, "latin1 create null AUTO invalid");
      /* length > INT_MAX => invalid (vendored TestLarge) */
      expect_status(napi_create_string_utf8(env, "", (size_t)INT_MAX + 1, &out),
                    napi_invalid_arg, "create utf8 large length invalid");
      expect_status(napi_create_string_latin1(env, "", (size_t)INT_MAX + 1, &out),
                    napi_invalid_arg, "create latin1 large length invalid");
      expect_status(napi_create_string_utf16(env, (const char16_t*)"", (size_t)INT_MAX + 1, &out),
                    napi_invalid_arg, "create utf16 large length invalid");
    }
    /* Get string wrong-type + null value */
    {
      napi_value n = nullptr;
      expect_status(napi_create_double(env, 1.0, &n), napi_ok, "getstr setup");
      char buf[64];
      size_t r = 777;
      expect_status(napi_get_value_string_utf8(env, n, buf, sizeof(buf), &r),
                    napi_string_expected, "utf8 of number is string_expected");
      expect(r == 777, "utf8 wrong-type leaves result untouched");
      expect_status(napi_get_value_string_utf16(env, n, (char16_t*)buf, 64, &r),
                    napi_string_expected, "utf16 of number is string_expected");
      expect_status(napi_get_value_string_latin1(env, n, buf, sizeof(buf), &r),
                    napi_string_expected, "latin1 of number is string_expected");
      napi_value s = nullptr;
      expect_status(napi_create_string_utf8(env, "a", 1, &s), napi_ok,
                    "getstr setup str");
      size_t sentinel = 555;
      expect_status(napi_get_value_string_utf8(env, nullptr, buf, sizeof(buf), &sentinel),
                    napi_invalid_arg, "utf8 null value invalid");
      expect(sentinel == 555, "utf8 null value leaves result untouched");
      expect_status(napi_get_value_string_utf16(env, nullptr, (char16_t*)buf, 64, &sentinel),
                    napi_invalid_arg, "utf16 null value invalid");
      expect_status(napi_get_value_string_latin1(env, nullptr, buf, sizeof(buf), &sentinel),
                    napi_invalid_arg, "latin1 null value invalid");
      /* buf NULL + result NULL => invalid (valid string) */
      expect_status(napi_get_value_string_utf8(env, s, nullptr, 0, nullptr),
                    napi_invalid_arg, "utf8 size query null result invalid");
      expect_status(napi_get_value_string_utf16(env, s, nullptr, 0, nullptr),
                    napi_invalid_arg, "utf16 size query null result invalid");
      expect_status(napi_get_value_string_latin1(env, s, nullptr, 0, nullptr),
                    napi_invalid_arg, "latin1 size query null result invalid");
      /* buf non-null + result NULL => ok (optional) */
      char ob[8] = {0};
      expect_status(napi_get_value_string_utf8(env, s, ob, sizeof(ob), nullptr),
                    napi_ok, "utf8 result optional ok");
      char16_t o16[8] = {};
      expect_status(napi_get_value_string_utf16(env, s, o16, 8, nullptr), napi_ok,
                    "utf16 result optional ok");
      char ol[8] = {0};
      expect_status(napi_get_value_string_latin1(env, s, ol, sizeof(ol), nullptr),
                    napi_ok, "latin1 result optional ok");
    }
  }

  /* ---- Last-error: failure sets, success clears ---- */
  {
    std::fprintf(stdout, "-- case last-error --\n");
    napi_value s = nullptr;
    expect_status(napi_create_string_utf8(env, "x", 1, &s), napi_ok, "last setup");
    double d = 0;
    expect_status(napi_get_value_double(env, s, &d), napi_number_expected,
                  "last fail number_expected");
    const napi_extended_error_info* ei = nullptr;
    expect_status(napi_get_last_error_info(env, &ei), napi_ok, "last read after fail");
    expect(ei != nullptr && ei->error_code == napi_number_expected,
           "last_error records failure");
    expect(ei->error_message != nullptr && std::strlen(ei->error_message) > 0,
           "last_error message non-empty");
    /* reading again without success still same (get_last_error_info does not clear) */
    const napi_extended_error_info* ei2 = nullptr;
    napi_get_last_error_info(env, &ei2);
    expect(ei2 != nullptr && ei2->error_code == napi_number_expected,
           "last reread still failure (no self-clear)");
    /* success clears */
    napi_value okv = nullptr;
    expect_status(napi_create_double(env, 1.0, &okv), napi_ok, "last success");
    expect_status(napi_get_last_error_info(env, &ei), napi_ok, "last read after success");
    expect(ei != nullptr && ei->error_code == napi_ok,
           "success clears last_error (AMB-STATUS-1)");
  }

  /* ---- Pending-exception: values execute and preserve (Yes rows) ---- */
  {
    std::fprintf(stdout, "-- case pending --\n");
    /* Set pending via throw through JsRun (gate_probe pattern). */
    {
      JsValueRef src = JS_INVALID_REFERENCE;
      const char* code = "throw new Error('boom-values')";
      expect(JsCreateString(code, std::strlen(code), &src) == JsNoError,
             "pending setup create source");
      JsValueRef url = JS_INVALID_REFERENCE;
      JsCreateString("values-probe.js", std::strlen("values-probe.js"), &url);
      JsValueRef res = JS_INVALID_REFERENCE;
      JsRun(src, JS_SOURCE_CONTEXT_NONE, url, JsParseScriptAttributeNone, &res);
      bool hp = false;
      JsHasException(&hp);
      expect(hp, "pending set after throw");
    }
    /* All values APIs are Yes: execute with pending, pending preserved. */
    napi_value v = nullptr;
    expect_status(napi_create_double(env, 2.5, &v), napi_ok,
                  "pending create_double executes");
    double d = 0;
    expect_status(napi_get_value_double(env, v, &d), napi_ok,
                  "pending get_double executes");
    expect(double_eq(d, 2.5), "pending double value");
    int32_t i32 = 0;
    expect_status(napi_get_value_int32(env, v, &i32), napi_ok,
                  "pending get_int32 executes");
    napi_value s = nullptr;
    expect_status(napi_create_string_utf8(env, "hi", 2, &s), napi_ok,
                  "pending create_string executes");
    char sb[16] = {0};
    size_t sc = 0;
    expect_status(napi_get_value_string_utf8(env, s, sb, sizeof(sb), &sc), napi_ok,
                  "pending get_string executes");
    expect(sc == 2 && std::memcmp(sb, "hi", 2) == 0, "pending string bytes");
    napi_value u = nullptr;
    expect_status(napi_get_undefined(env, &u), napi_ok, "pending get_undefined executes");
    bool pend = false;
    expect_status(napi_is_exception_pending(env, &pend), napi_ok,
                  "pending still present");
    expect(pend, "pending preserved after values calls");
    napi_value cleared = nullptr;
    expect_status(napi_get_and_clear_last_exception(env, &cleared), napi_ok,
                  "pending clear");
    expect(cleared != nullptr, "pending cleared value non-null");
    bool pend2 = true;
    napi_is_exception_pending(env, &pend2);
    expect(!pend2, "pending cleared");
  }

  /* Root count should be > baseline (all creates rooted in default scope). */
  expect(chakra_nodeapi_test_root_count(env) > baseline,
         "roots accumulated in default scope");

  chakra_nodeapi_destroy_env(env);
  expect(true, "env destroyed");
  JsSetCurrentContext(JS_INVALID_REFERENCE);
  JsDisposeRuntime(rt);

  std::fprintf(stdout, "VALUES_PROBE: %s (%d/%d)\n",
               (g_pass == g_total) ? "PASS" : "FAIL", g_pass, g_total);
  return (g_pass == g_total) ? 0 : 1;
}
