//-------------------------------------------------------------------------------------------------------
// Copyright (c) ChakraCore Project Contributors. All rights reserved.
// Licensed under the MIT license. See LICENSE.txt file in the project root for full license information.
//-------------------------------------------------------------------------------------------------------
//
// Q1.1 Node-API v9/v10 + multi-addon probe (standalone C++ probe).
//
// Exercises all 10 additive v9/v10 symbols, version negotiation, NestedWrap-
// style optional-ref cancellation, Closing versioned return codes, and
// multi-env cross-addon Buffer marker sharing on one (runtime, context).
//
// Must compile standalone against lib/NodeApi/*.cpp + libChakraCore at
// -DNAPI_VERSION=10 -DCHAKRA_NODEAPI_TEST_HOOKS (also wired into
// tools/build/check_sanitize.py).
//
// Output: one line per check, then "V10_MULTI_ADDON_PROBE: PASS (n/n)" exit 0
// or "V10_MULTI_ADDON_PROBE: FAIL (k/n)" exit 1.

#include "NodeApiHostApi.h"
#include "NodeApiEnv.h"
#include "NodeApiWrap.h"
#include "NodeApiScopes.h"

#include "js_native_api.h"
#include "node_api.h"
#include "ChakraCommon.h"
#include "ChakraCore.h"

#include <climits>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

namespace {

#if defined(__clang__)
#define PROBE_NOINLINE_OPTNONE __attribute__((noinline, optnone))
#elif defined(__GNUC__)
#define PROBE_NOINLINE_OPTNONE __attribute__((noinline, optimize("O0")))
#else
#define PROBE_NOINLINE_OPTNONE
#endif

template <typename T>
inline void ClearDeadRef(T& ref, T invalid_val) {
  volatile T* slot = &ref;
  *slot = invalid_val;
#if defined(__clang__) || defined(__GNUC__)
  __asm__ volatile("" : : "r"(slot) : "memory");
#endif
}

int g_total = 0;
int g_pass = 0;

void Check(bool cond, const char* name) {
  g_total++;
  if (cond) {
    g_pass++;
    std::fprintf(stdout, "PASS %s\n", name);
  } else {
    std::fprintf(stdout, "FAIL %s\n", name);
  }
  std::fflush(stdout);
}

const char* StatusName(napi_status s) {
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
    case napi_arraybuffer_expected: return "arraybuffer_expected";
    case napi_cannot_run_js: return "cannot_run_js";
    default: return "?";
  }
}

void CheckStatus(napi_status got, napi_status want, const char* name) {
  g_total++;
  if (got == want) {
    g_pass++;
    std::fprintf(stdout, "PASS %s (status=%s)\n", name, StatusName(got));
  } else {
    std::fprintf(stdout, "FAIL %s (got=%s want=%s)\n", name,
                 StatusName(got), StatusName(want));
  }
  std::fflush(stdout);
}

bool HasPending(napi_env env) {
  bool p = false;
  return napi_is_exception_pending(env, &p) == napi_ok && p;
}

std::string ReadUtf8(napi_env env, napi_value val) {
  size_t len = 0;
  if (napi_get_value_string_utf8(env, val, nullptr, 0, &len) != napi_ok) {
    return "<non-string>";
  }
  std::string out(len + 1, '\0');
  size_t copied = 0;
  if (napi_get_value_string_utf8(env, val, &out[0], out.size(), &copied) !=
      napi_ok) {
    return "<copy-err>";
  }
  out.resize(copied);
  return out;
}

std::string ReadStrProp(napi_env env, napi_value obj, const char* key) {
  napi_value v = nullptr;
  if (napi_get_named_property(env, obj, key, &v) != napi_ok || !v) {
    return "<missing>";
  }
  return ReadUtf8(env, v);
}

napi_value RunJs(napi_env env, const char* code) {
  napi_value s = nullptr;
  if (napi_create_string_utf8(env, code, NAPI_AUTO_LENGTH, &s) != napi_ok) {
    return nullptr;
  }
  napi_value res = nullptr;
  if (napi_run_script(env, s, &res) != napi_ok) {
    return nullptr;
  }
  return res;
}

bool IsInstanceOfGlobal(napi_env env, napi_value val, const char* ctor_name) {
  napi_value global = nullptr;
  napi_value ctor = nullptr;
  bool is_inst = false;
  if (napi_get_global(env, &global) != napi_ok || !global) return false;
  if (napi_get_named_property(env, global, ctor_name, &ctor) != napi_ok ||
      !ctor) {
    return false;
  }
  if (napi_instanceof(env, val, ctor, &is_inst) != napi_ok) return false;
  return is_inst;
}

void Collect10x(JsRuntimeHandle rt) {
  for (int i = 0; i < 10; i++) {
    if (JsCollectGarbage(rt) != JsNoError) {
      std::fprintf(stderr, "FATAL: JsCollectGarbage failed\n");
      std::exit(2);
    }
  }
}

PROBE_NOINLINE_OPTNONE void ClearStack() {
  constexpr int kN = 512;
  volatile void* dummy[kN];
  volatile unsigned char* bp =
      reinterpret_cast<volatile unsigned char*>(dummy);
  while (bp < reinterpret_cast<volatile unsigned char*>(dummy) +
                  sizeof(dummy)) {
    *bp++ = 0;
  }
#if defined(__clang__) || defined(__GNUC__)
  __asm__ volatile("" : : "r"(dummy) : "memory");
#endif
}

struct ExtStrFinRec {
  int calls = 0;
  napi_env last_env = nullptr;
  void* last_data = nullptr;
  void* last_hint = nullptr;
};

void ExtStrFinalize(node_api_basic_env env, void* data, void* hint) {
  auto* rec = static_cast<ExtStrFinRec*>(hint);
  if (!rec) return;
  rec->calls++;
  rec->last_env = reinterpret_cast<napi_env>(env);
  rec->last_data = data;
  rec->last_hint = hint;
}

struct ClosingHookState {
  napi_env env = nullptr;
  napi_status set_ver_status = napi_ok;
  napi_status get_file_status = napi_generic_failure;
  std::string file_url;
  napi_status get_global_status = napi_generic_failure;
  napi_status get_named_status = napi_ok;
  int ran = 0;
};

void ClosingCheckHook(void* arg) {
  auto* st = static_cast<ClosingHookState*>(arg);
  if (!st || !st->env) return;
  st->ran++;
  st->set_ver_status = chakra_nodeapi_set_module_api_version(st->env, 9);
  const char* url = nullptr;
  st->get_file_status = node_api_get_module_file_name(st->env, &url);
  if (st->get_file_status == napi_ok && url != nullptr) {
    st->file_url = url;
  }
  napi_value global = nullptr;
  st->get_global_status = napi_get_global(st->env, &global);
  napi_value prop = nullptr;
  if (global != nullptr) {
    st->get_named_status =
        napi_get_named_property(st->env, global, "setTimeout", &prop);
  }
}

struct NestedWrapState {
  napi_env env = nullptr;
  napi_ref wrapper_ref = nullptr;
  napi_ref rider_ref = nullptr;
  int head_calls = 0;
  int rider_calls = 0;
  int total_calls = 0;
};

void NestedHeadFinalize(node_api_basic_env env_raw, void* data, void* /*hint*/) {
  auto* st = static_cast<NestedWrapState*>(data);
  if (!st) return;
  st->head_calls++;
  st->total_calls++;
  napi_env env = reinterpret_cast<napi_env>(env_raw);
  if (st->wrapper_ref != nullptr) {
    (void)napi_delete_reference(env, st->wrapper_ref);
    st->wrapper_ref = nullptr;
  }
  if (st->rider_ref != nullptr) {
    (void)napi_delete_reference(env, st->rider_ref);
    st->rider_ref = nullptr;
  }
}

void NestedRiderFinalize(node_api_basic_env /*env*/, void* data, void* /*hint*/) {
  auto* st = static_cast<NestedWrapState*>(data);
  if (!st) return;
  st->rider_calls++;
  st->total_calls++;
}

PROBE_NOINLINE_OPTNONE bool SetupNestedWrapAndDrop(
    napi_env env, NestedWrapState* st, napi_ref* witness_out) {
  *witness_out = nullptr;
  JsValueRef obj = JS_INVALID_REFERENCE;
  if (JsCreateObject(&obj) != JsNoError || obj == JS_INVALID_REFERENCE) {
    return false;
  }
  napi_value nobj = reinterpret_cast<napi_value>(obj);
  if (napi_create_reference(env, nobj, 0, witness_out) != napi_ok) {
    ClearDeadRef(obj, JS_INVALID_REFERENCE);
    return false;
  }
  if (napi_wrap(env, nobj, st, NestedHeadFinalize, nullptr,
                &st->wrapper_ref) != napi_ok) {
    (void)napi_delete_reference(env, *witness_out);
    *witness_out = nullptr;
    ClearDeadRef(obj, JS_INVALID_REFERENCE);
    return false;
  }
  if (napi_add_finalizer(env, nobj, st, NestedRiderFinalize, nullptr,
                         &st->rider_ref) != napi_ok) {
    (void)napi_delete_reference(env, st->wrapper_ref);
    st->wrapper_ref = nullptr;
    (void)napi_delete_reference(env, *witness_out);
    *witness_out = nullptr;
    ClearDeadRef(obj, JS_INVALID_REFERENCE);
    return false;
  }
  ClearDeadRef(obj, JS_INVALID_REFERENCE);
  return true;
}

} /* namespace */

int main() {
  JsRuntimeHandle rt = JS_INVALID_RUNTIME_HANDLE;
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

  /* ---- 1. napi_get_version == 10 + set_module_api_version ---- */
  {
    std::fprintf(stdout, "-- case 1: version + set_module_api_version --\n");
    napi_env env = chakra_nodeapi_create_env(rt, ctx, "/tmp/ver.node", nullptr, nullptr);
    Check(env != nullptr, "1: create env");
    uint32_t ver = 0;
    CheckStatus(napi_get_version(env, &ver), napi_ok, "1: get_version ok");
    Check(ver == 10, "1: napi_get_version == 10");
    CheckStatus(napi_get_version(env, nullptr), napi_invalid_arg,
                "1: get_version NULL result invalid_arg");

    for (int32_t v = 1; v <= 10; ++v) {
      napi_status s = chakra_nodeapi_set_module_api_version(env, v);
      char msg[64];
      std::snprintf(msg, sizeof(msg), "1: set_module_api_version(%d) ok", v);
      CheckStatus(s, napi_ok, msg);
    }
    CheckStatus(chakra_nodeapi_set_module_api_version(env, 0), napi_invalid_arg,
                "1: set_module_api_version(0) invalid_arg");
    CheckStatus(chakra_nodeapi_set_module_api_version(env, 11), napi_invalid_arg,
                "1: set_module_api_version(11) invalid_arg");
    CheckStatus(chakra_nodeapi_set_module_api_version(env, -1), napi_invalid_arg,
                "1: set_module_api_version(-1) invalid_arg");
    CheckStatus(chakra_nodeapi_set_module_api_version(nullptr, 10),
                napi_invalid_arg, "1: set_module_api_version(NULL) invalid_arg");

    ClosingHookState chs{};
    chs.env = env;
    CheckStatus(napi_add_env_cleanup_hook(env, ClosingCheckHook, &chs), napi_ok,
                "1: add Closing hook");
    chakra_nodeapi_destroy_env(env);
    Check(chs.ran == 1, "1: Closing hook ran once");
    CheckStatus(chs.set_ver_status, napi_invalid_arg,
                "1: set_module_api_version in Closing invalid_arg");
    CheckStatus(chakra_nodeapi_set_module_api_version(env, 8), napi_invalid_arg,
                "1: set_module_api_version on dead env invalid_arg");
  }

  /* ---- 2. node_api_symbol_for ---- */
  {
    std::fprintf(stdout, "-- case 2: node_api_symbol_for --\n");
    napi_env env = chakra_nodeapi_create_env(rt, ctx, "/tmp/sym.node", nullptr, nullptr);
    Check(env != nullptr, "2: create env");
    napi_handle_scope sc = nullptr;
    CheckStatus(napi_open_handle_scope(env, &sc), napi_ok, "2: open scope");

    napi_value s1 = nullptr;
    napi_value s2 = nullptr;
    CheckStatus(node_api_symbol_for(env, "chakra.q1.sym", NAPI_AUTO_LENGTH, &s1),
                napi_ok, "2: symbol_for auto len");
    CheckStatus(node_api_symbol_for(env, "chakra.q1.sym", 13, &s2),
                napi_ok, "2: symbol_for explicit len");
    napi_value js_sym = RunJs(env, "Symbol.for('chakra.q1.sym')");
    Check(js_sym != nullptr, "2: JS Symbol.for");
    bool eq = false;
    CheckStatus(napi_strict_equals(env, s1, s2, &eq), napi_ok, "2: eq s1/s2 ok");
    Check(eq, "2: s1 === s2");
    eq = false;
    CheckStatus(napi_strict_equals(env, s1, js_sym, &eq), napi_ok, "2: eq js ok");
    Check(eq, "2: s1 === Symbol.for('chakra.q1.sym')");

    /* Non-global napi_create_symbol is distinct from Symbol.for. */
    napi_value desc = nullptr;
    napi_value priv_sym = nullptr;
    CheckStatus(napi_create_string_utf8(env, "chakra.q1.sym", NAPI_AUTO_LENGTH, &desc),
                napi_ok, "2: desc str");
    CheckStatus(napi_create_symbol(env, desc, &priv_sym), napi_ok, "2: create_symbol");
    eq = true;
    CheckStatus(napi_strict_equals(env, s1, priv_sym, &eq), napi_ok, "2: eq priv ok");
    Check(!eq, "2: Symbol.for !== local Symbol()");

    /* (NULL, 0) -> Symbol.for("") */
    napi_value s_empty = nullptr;
    CheckStatus(node_api_symbol_for(env, nullptr, 0, &s_empty), napi_ok,
                "2: (NULL, 0) ok");
    napi_value js_empty = RunJs(env, "Symbol.for('')");
    eq = false;
    CheckStatus(napi_strict_equals(env, s_empty, js_empty, &eq), napi_ok,
                "2: empty eq ok");
    Check(eq, "2: (NULL, 0) === Symbol.for('')");

    /* Invalid args */
    napi_value bad = nullptr;
    CheckStatus(node_api_symbol_for(env, nullptr, 5, &bad), napi_invalid_arg,
                "2: (NULL, 5) invalid_arg");
    CheckStatus(node_api_symbol_for(env, nullptr, NAPI_AUTO_LENGTH, &bad),
                napi_invalid_arg, "2: (NULL, NAPI_AUTO_LENGTH) invalid_arg");
    CheckStatus(node_api_symbol_for(env, "abc", static_cast<size_t>(INT_MAX) + 1, &bad),
                napi_invalid_arg, "2: len > INT_MAX invalid_arg");
    CheckStatus(node_api_symbol_for(env, "abc", 3, nullptr), napi_invalid_arg,
                "2: NULL result invalid_arg");
    CheckStatus(node_api_symbol_for(nullptr, "abc", 3, &bad), napi_invalid_arg,
                "2: NULL env invalid_arg");

    /* Pending-exception preservation (allowPending=true) */
    CheckStatus(napi_throw_error(env, "ERR_PEND", "preserve-sym"), napi_ok,
                "2: seed pending error");
    Check(HasPending(env), "2: pending before symbol_for");
    napi_value s_pend = nullptr;
    CheckStatus(node_api_symbol_for(env, "pend.key", NAPI_AUTO_LENGTH, &s_pend),
                napi_ok, "2: symbol_for executes with pending");
    Check(HasPending(env), "2: pending preserved after symbol_for");
    napi_value cleared = nullptr;
    CheckStatus(napi_get_and_clear_last_exception(env, &cleared), napi_ok,
                "2: clear preserved exception");
    Check(ReadStrProp(env, cleared, "message") == "preserve-sym",
          "2: preserved exception message intact");
    napi_value js_pend = RunJs(env, "Symbol.for('pend.key')");
    eq = false;
    CheckStatus(napi_strict_equals(env, s_pend, js_pend, &eq), napi_ok,
                "2: pending symbol_for matches Symbol.for");
    Check(eq, "2: s_pend === Symbol.for('pend.key')");

    CheckStatus(napi_close_handle_scope(env, sc), napi_ok, "2: close scope");
    chakra_nodeapi_destroy_env(env);
  }

  /* ---- 3. node_api_create_syntax_error & node_api_throw_syntax_error ---- */
  {
    std::fprintf(stdout, "-- case 3: syntax error create & throw --\n");
    napi_env env = chakra_nodeapi_create_env(rt, ctx, "/tmp/syn.node", nullptr, nullptr);
    Check(env != nullptr, "3: create env");
    napi_handle_scope sc = nullptr;
    CheckStatus(napi_open_handle_scope(env, &sc), napi_ok, "3: open scope");

    napi_value code = nullptr;
    napi_value msg = nullptr;
    napi_value num = nullptr;
    CheckStatus(napi_create_string_utf8(env, "ERR_SYN_CODE", NAPI_AUTO_LENGTH, &code),
                napi_ok, "3: code str");
    CheckStatus(napi_create_string_utf8(env, "bad syntax here", NAPI_AUTO_LENGTH, &msg),
                napi_ok, "3: msg str");
    CheckStatus(napi_create_double(env, 7.0, &num), napi_ok, "3: num");

    napi_value err = nullptr;
    CheckStatus(node_api_create_syntax_error(env, code, msg, &err), napi_ok,
                "3: create_syntax_error ok");
    Check(IsInstanceOfGlobal(env, err, "SyntaxError"),
          "3: err instanceof SyntaxError");
    Check(ReadStrProp(env, err, "code") == "ERR_SYN_CODE", "3: err.code");
    Check(ReadStrProp(env, err, "message") == "bad syntax here", "3: err.message");

    napi_value err_nocode = nullptr;
    CheckStatus(node_api_create_syntax_error(env, nullptr, msg, &err_nocode),
                napi_ok, "3: create_syntax_error NULL code ok");
    Check(IsInstanceOfGlobal(env, err_nocode, "SyntaxError"),
          "3: err_nocode instanceof SyntaxError");
    Check(ReadStrProp(env, err_nocode, "message") == "bad syntax here",
          "3: err_nocode.message");

    /* Type/arg validation (NULL msg => empty message, matching create_error) */
    napi_value dummy_err = nullptr;
    CheckStatus(node_api_create_syntax_error(env, code, num, &dummy_err),
                napi_string_expected, "3: non-string msg => string_expected");
    CheckStatus(node_api_create_syntax_error(env, num, msg, &dummy_err),
                napi_string_expected, "3: non-string code => string_expected");
    CheckStatus(node_api_create_syntax_error(env, code, msg, nullptr),
                napi_invalid_arg, "3: NULL result => invalid_arg");
    CheckStatus(node_api_create_syntax_error(nullptr, code, msg, &dummy_err),
                napi_invalid_arg, "3: NULL env => invalid_arg");
    napi_value empty_msg_err = nullptr;
    CheckStatus(node_api_create_syntax_error(env, nullptr, nullptr, &empty_msg_err),
                napi_ok, "3: NULL code/msg => ok");
    Check(IsInstanceOfGlobal(env, empty_msg_err, "SyntaxError") &&
              ReadStrProp(env, empty_msg_err, "message") == "",
          "3: NULL msg creates SyntaxError with empty message");

    /* allowPending=true for create; pending gate for throw */
    CheckStatus(napi_throw_error(env, "ERR_FIRST", "first err"), napi_ok,
                "3: seed pending");
    napi_value err_pend = nullptr;
    CheckStatus(node_api_create_syntax_error(env, code, msg, &err_pend),
                napi_ok, "3: create_syntax_error executes with pending");
    Check(HasPending(env), "3: pending preserved across create_syntax_error");
    CheckStatus(node_api_throw_syntax_error(env, "ERR_SECOND", "second syn"),
                napi_pending_exception, "3: throw_syntax_error gated by pending");
    napi_value cleared = nullptr;
    CheckStatus(napi_get_and_clear_last_exception(env, &cleared), napi_ok,
                "3: clear first pending");
    Check(ReadStrProp(env, cleared, "message") == "first err",
          "3: original exception preserved by throw_syntax_error gate");

    /* Clean throw_syntax_error */
    CheckStatus(node_api_throw_syntax_error(env, "ERR_THROW_SYN", "syntax thrown"),
                napi_ok, "3: throw_syntax_error ok");
    Check(HasPending(env), "3: pending true after throw_syntax_error");
    napi_value thrown = nullptr;
    CheckStatus(napi_get_and_clear_last_exception(env, &thrown), napi_ok,
                "3: clear thrown syntax error");
    Check(IsInstanceOfGlobal(env, thrown, "SyntaxError"),
          "3: thrown instanceof SyntaxError");
    Check(ReadStrProp(env, thrown, "code") == "ERR_THROW_SYN", "3: thrown.code");
    Check(ReadStrProp(env, thrown, "message") == "syntax thrown",
          "3: thrown.message");
    CheckStatus(node_api_throw_syntax_error(env, nullptr, nullptr),
                napi_ok, "3: throw_syntax_error NULL code/msg ok");
    napi_value thrown_empty = nullptr;
    CheckStatus(napi_get_and_clear_last_exception(env, &thrown_empty), napi_ok,
                "3: clear NULL msg thrown syntax error");
    Check(IsInstanceOfGlobal(env, thrown_empty, "SyntaxError") &&
              ReadStrProp(env, thrown_empty, "message") == "",
          "3: NULL msg throws SyntaxError with empty message");
    CheckStatus(node_api_throw_syntax_error(nullptr, "E", "m"),
                napi_invalid_arg, "3: throw_syntax_error NULL env invalid_arg");

    CheckStatus(napi_close_handle_scope(env, sc), napi_ok, "3: close scope");
    chakra_nodeapi_destroy_env(env);
  }

  /* ---- 4. node_api_get_module_file_name ---- */
  {
    std::fprintf(stdout, "-- case 4: node_api_get_module_file_name --\n");
    napi_env envA = chakra_nodeapi_create_env(
        rt, ctx, "/tmp/dir with spaces/addon#1.node", nullptr, nullptr);
    napi_env envB = chakra_nodeapi_create_env(
        rt, ctx, "/tmp/other/mod.node", nullptr, nullptr);
    napi_env envNull = chakra_nodeapi_create_env(rt, ctx, nullptr, nullptr, nullptr);
    Check(envA && envB && envNull, "4: create three envs");

    const char* uA = nullptr;
    const char* uB = nullptr;
    const char* uNull = nullptr;
    CheckStatus(node_api_get_module_file_name(envA, &uA), napi_ok, "4: get A ok");
    CheckStatus(node_api_get_module_file_name(envB, &uB), napi_ok, "4: get B ok");
    CheckStatus(node_api_get_module_file_name(envNull, &uNull), napi_ok,
                "4: get NULL filename ok");
    Check(uA != nullptr &&
              std::strcmp(uA, "file:///tmp/dir%20with%20spaces/addon%231.node") == 0,
          "4: space/%20 and #/%23 file:// formatting");
    Check(uB != nullptr && std::strcmp(uB, "file:///tmp/other/mod.node") == 0,
          "4: B distinct file:// URL");
    Check(uNull != nullptr && std::strcmp(uNull, "") == 0,
          "4: NULL filename => empty string");
    Check(std::strcmp(uA, uB) != 0, "4: envA and envB module URLs distinct");
    CheckStatus(node_api_get_module_file_name(envA, nullptr), napi_invalid_arg,
                "4: NULL result => invalid_arg");
    CheckStatus(node_api_get_module_file_name(nullptr, &uA), napi_invalid_arg,
                "4: NULL env => invalid_arg");

    ClosingHookState chs{};
    chs.env = envA;
    CheckStatus(napi_add_env_cleanup_hook(envA, ClosingCheckHook, &chs), napi_ok,
                "4: add Closing hook on envA");

    chakra_nodeapi_destroy_env(envNull);
    chakra_nodeapi_destroy_env(envB);
    chakra_nodeapi_destroy_env(envA);
    CheckStatus(chs.get_file_status, napi_ok,
                "4: get_module_file_name succeeds in Closing");
    Check(chs.file_url == "file:///tmp/dir%20with%20spaces/addon%231.node",
          "4: Closing get_module_file_name URL intact");
    CheckStatus(node_api_get_module_file_name(envA, &uA), napi_invalid_arg,
                "4: dead env => invalid_arg");
  }

  /* ---- 5. node_api_create_external_string_{latin1,utf16} ---- */
  {
    std::fprintf(stdout, "-- case 5: external strings latin1 & utf16 --\n");
    napi_env env = chakra_nodeapi_create_env(rt, ctx, "/tmp/extstr.node", nullptr, nullptr);
    Check(env != nullptr, "5: create env");
    napi_handle_scope sc = nullptr;
    CheckStatus(napi_open_handle_scope(env, &sc), napi_ok, "5: open scope");

    /* Latin1: ASCII + high bytes 0xA1, 0xE9, 0xFF */
    char lat_buf[] = {'h', 'i', ' ', static_cast<char>(0xA1),
                      static_cast<char>(0xE9), static_cast<char>(0xFF), '\0'};
    ExtStrFinRec fin_lat;
    napi_value s_lat = nullptr;
    bool copied = false;
    CheckStatus(node_api_create_external_string_latin1(
                    env, lat_buf, NAPI_AUTO_LENGTH, ExtStrFinalize, &fin_lat,
                    &s_lat, &copied),
                napi_ok, "5: external latin1 auto ok");
    Check(copied == true, "5: latin1 *copied == true");
    Check(fin_lat.calls == 1 && fin_lat.last_env == env &&
              fin_lat.last_data == lat_buf && fin_lat.last_hint == &fin_lat,
          "5: latin1 finalize ran synchronously once with args");
    Check(ReadUtf8(env, s_lat) == "hi \xC2\xA1\xC3\xA9\xC3\xBF",
          "5: high latin1 round-trips as UTF-8");

    /* UTF-16: 'A' + U+00E9 + astral U+1F680 (surrogate pair D83D DE80) */
    char16_t u16_buf[] = {0x0041, 0x00E9, 0xD83D, 0xDE80, 0x0000};
    ExtStrFinRec fin_u16;
    napi_value s_u16 = nullptr;
    copied = false;
    CheckStatus(node_api_create_external_string_utf16(
                    env, u16_buf, NAPI_AUTO_LENGTH, ExtStrFinalize, &fin_u16,
                    &s_u16, &copied),
                napi_ok, "5: external utf16 auto ok");
    Check(copied == true, "5: utf16 *copied == true");
    Check(fin_u16.calls == 1 && fin_u16.last_data == u16_buf,
          "5: utf16 finalize ran synchronously once");
    char16_t got_u16[8] = {};
    size_t u16_len = 0;
    CheckStatus(napi_get_value_string_utf16(env, s_u16, got_u16, 8, &u16_len),
                napi_ok, "5: get utf16 ok");
    Check(u16_len == 4 && got_u16[0] == 0x0041 && got_u16[1] == 0x00E9 &&
              got_u16[2] == 0xD83D && got_u16[3] == 0xDE80,
          "5: astral surrogate pair round-trips exactly");

    /* (NULL, 0) -> "" with finalizer */
    ExtStrFinRec fin_empty;
    napi_value s_empty = nullptr;
    copied = false;
    CheckStatus(node_api_create_external_string_latin1(
                    env, nullptr, 0, ExtStrFinalize, &fin_empty, &s_empty,
                    &copied),
                napi_ok, "5: latin1 (NULL, 0) ok");
    Check(copied == true && fin_empty.calls == 1 && fin_empty.last_data == nullptr,
          "5: latin1 (NULL, 0) copied + finalized");
    Check(ReadUtf8(env, s_empty) == "", "5: latin1 (NULL, 0) => empty");

    copied = false;
    ExtStrFinRec fin_empty16;
    napi_value s_empty16 = nullptr;
    CheckStatus(node_api_create_external_string_utf16(
                    env, nullptr, 0, ExtStrFinalize, &fin_empty16, &s_empty16,
                    &copied),
                napi_ok, "5: utf16 (NULL, 0) ok");
    Check(copied == true && fin_empty16.calls == 1 && ReadUtf8(env, s_empty16) == "",
          "5: utf16 (NULL, 0) => empty + finalized");

    /* copied == NULL and finalize_callback == NULL both allowed */
    napi_value s_opt = nullptr;
    CheckStatus(node_api_create_external_string_latin1(
                    env, lat_buf, 2, nullptr, nullptr, &s_opt, nullptr),
                napi_ok, "5: latin1 NULL copied & NULL fin ok");
    Check(ReadUtf8(env, s_opt) == "hi", "5: latin1 explicit length 2 => hi");
    CheckStatus(node_api_create_external_string_utf16(
                    env, u16_buf, 2, nullptr, nullptr, &s_opt, nullptr),
                napi_ok, "5: utf16 NULL copied & NULL fin ok");

    /* Invalid args: finalize_callback MUST run 0 times! */
    ExtStrFinRec fin_err;
    napi_value err_res = nullptr;
    bool err_copied = false;
    CheckStatus(node_api_create_external_string_latin1(
                    env, nullptr, 5, ExtStrFinalize, &fin_err, &err_res,
                    &err_copied),
                napi_invalid_arg, "5: latin1 (NULL, 5) invalid_arg");
    CheckStatus(node_api_create_external_string_latin1(
                    env, lat_buf, 2, ExtStrFinalize, &fin_err, nullptr,
                    &err_copied),
                napi_invalid_arg, "5: latin1 NULL result invalid_arg");
    CheckStatus(node_api_create_external_string_latin1(
                    env, lat_buf, static_cast<size_t>(INT_MAX) + 1,
                    ExtStrFinalize, &fin_err, &err_res, &err_copied),
                napi_invalid_arg, "5: latin1 > INT_MAX invalid_arg");
    CheckStatus(node_api_create_external_string_utf16(
                    env, nullptr, 5, ExtStrFinalize, &fin_err, &err_res,
                    &err_copied),
                napi_invalid_arg, "5: utf16 (NULL, 5) invalid_arg");
    CheckStatus(node_api_create_external_string_utf16(
                    env, u16_buf, 2, ExtStrFinalize, &fin_err, nullptr,
                    &err_copied),
                napi_invalid_arg, "5: utf16 NULL result invalid_arg");
    CheckStatus(node_api_create_external_string_utf16(
                    env, u16_buf, static_cast<size_t>(INT_MAX) + 1,
                    ExtStrFinalize, &fin_err, &err_res, &err_copied),
                napi_invalid_arg, "5: utf16 > INT_MAX invalid_arg");
    Check(fin_err.calls == 0 && err_copied == false,
          "5: invalid args invoke finalize_callback 0 times");

    CheckStatus(napi_close_handle_scope(env, sc), napi_ok, "5: close scope");
    chakra_nodeapi_destroy_env(env);
  }

  /* ---- 6. node_api_create_property_key_{latin1,utf8,utf16} ---- */
  {
    std::fprintf(stdout, "-- case 6: property keys latin1/utf8/utf16 --\n");
    napi_env env = chakra_nodeapi_create_env(rt, ctx, "/tmp/pkey.node", nullptr, nullptr);
    Check(env != nullptr, "6: create env");
    napi_handle_scope sc = nullptr;
    CheckStatus(napi_open_handle_scope(env, &sc), napi_ok, "6: open scope");

    napi_value obj = nullptr;
    CheckStatus(napi_create_object(env, &obj), napi_ok, "6: create object");

    const char pk_lat[] = {'k', '_', static_cast<char>(0xE9), '\0'};
    const char pk_u8[] = "u8_\xC3\xA9_\xF0\x9F\x9A\x80";
    const char16_t pk_u16[] = {0x0075, 0x0031, 0x0036, 0x005F, 0xD83D, 0xDE80, 0x0000};

    napi_value k_lat = nullptr;
    napi_value k_u8 = nullptr;
    napi_value k_u16 = nullptr;
    napi_value k_empty = nullptr;
    CheckStatus(node_api_create_property_key_latin1(env, pk_lat, NAPI_AUTO_LENGTH, &k_lat),
                napi_ok, "6: property_key_latin1 ok");
    CheckStatus(node_api_create_property_key_utf8(env, pk_u8, NAPI_AUTO_LENGTH, &k_u8),
                napi_ok, "6: property_key_utf8 ok");
    CheckStatus(node_api_create_property_key_utf16(env, pk_u16, NAPI_AUTO_LENGTH, &k_u16),
                napi_ok, "6: property_key_utf16 ok");
    CheckStatus(node_api_create_property_key_utf8(env, nullptr, 0, &k_empty),
                napi_ok, "6: property_key_utf8 (NULL, 0) ok");

    napi_value v1 = nullptr, v2 = nullptr, v3 = nullptr, v4 = nullptr;
    CheckStatus(napi_create_int32(env, 101, &v1), napi_ok, "6: val 101");
    CheckStatus(napi_create_int32(env, 202, &v2), napi_ok, "6: val 202");
    CheckStatus(napi_create_int32(env, 303, &v3), napi_ok, "6: val 303");
    CheckStatus(napi_create_int32(env, 404, &v4), napi_ok, "6: val 404");

    CheckStatus(napi_set_property(env, obj, k_lat, v1), napi_ok, "6: set lat");
    CheckStatus(napi_set_property(env, obj, k_u8, v2), napi_ok, "6: set u8");
    CheckStatus(napi_set_property(env, obj, k_u16, v3), napi_ok, "6: set u16");
    CheckStatus(napi_set_property(env, obj, k_empty, v4), napi_ok, "6: set empty");

    napi_value g = nullptr;
    int32_t iv = 0;
    CheckStatus(napi_get_property(env, obj, k_lat, &g), napi_ok, "6: get lat");
    Check(napi_get_value_int32(env, g, &iv) == napi_ok && iv == 101, "6: roundtrip lat 101");
    CheckStatus(napi_get_property(env, obj, k_u8, &g), napi_ok, "6: get u8");
    Check(napi_get_value_int32(env, g, &iv) == napi_ok && iv == 202, "6: roundtrip u8 202");
    CheckStatus(napi_get_property(env, obj, k_u16, &g), napi_ok, "6: get u16");
    Check(napi_get_value_int32(env, g, &iv) == napi_ok && iv == 303, "6: roundtrip u16 303");
    CheckStatus(napi_get_named_property(env, obj, "", &g), napi_ok, "6: get named ''");
    Check(napi_get_value_int32(env, g, &iv) == napi_ok && iv == 404, "6: roundtrip empty 404");

    /* Invalid args */
    napi_value bad = nullptr;
    CheckStatus(node_api_create_property_key_latin1(env, nullptr, 3, &bad),
                napi_invalid_arg, "6: lat (NULL, 3) invalid_arg");
    CheckStatus(node_api_create_property_key_utf8(env, nullptr, NAPI_AUTO_LENGTH, &bad),
                napi_invalid_arg, "6: utf8 (NULL, auto) invalid_arg");
    CheckStatus(node_api_create_property_key_utf16(env, nullptr, 1, &bad),
                napi_invalid_arg, "6: utf16 (NULL, 1) invalid_arg");
    CheckStatus(node_api_create_property_key_utf8(env, "x", 1, nullptr),
                napi_invalid_arg, "6: utf8 NULL result invalid_arg");
    CheckStatus(node_api_create_property_key_utf16(
                    env, pk_u16, static_cast<size_t>(INT_MAX) + 1, &bad),
                napi_invalid_arg, "6: utf16 > INT_MAX invalid_arg");

    CheckStatus(napi_close_handle_scope(env, sc), napi_ok, "6: close scope");
    chakra_nodeapi_destroy_env(env);
  }

  /* ---- 7. node_api_create_buffer_from_arraybuffer ---- */
  {
    std::fprintf(stdout, "-- case 7: buffer_from_arraybuffer --\n");
    napi_env env = chakra_nodeapi_create_env(rt, ctx, "/tmp/abbuf.node", nullptr, nullptr);
    Check(env != nullptr, "7: create env");
    napi_handle_scope sc = nullptr;
    CheckStatus(napi_open_handle_scope(env, &sc), napi_ok, "7: open scope");

    napi_value ab = nullptr;
    void* ab_raw = nullptr;
    CheckStatus(napi_create_arraybuffer(env, 16, &ab_raw, &ab), napi_ok,
                "7: create 16B ArrayBuffer");
    auto* ab_bytes = static_cast<uint8_t*>(ab_raw);
    if (ab_bytes != nullptr) {
      for (uint8_t i = 0; i < 16; ++i) ab_bytes[i] = i;
    }

    napi_value slice = nullptr;
    CheckStatus(node_api_create_buffer_from_arraybuffer(env, ab, 4, 6, &slice),
                napi_ok, "7: slice(4, 6) ok");
    bool is_buf = false;
    CheckStatus(napi_is_buffer(env, slice, &is_buf), napi_ok, "7: is_buffer ok");
    Check(is_buf == true, "7: slice is Buffer");
    void* slice_raw = nullptr;
    size_t slice_len = 0;
    CheckStatus(napi_get_buffer_info(env, slice, &slice_raw, &slice_len),
                napi_ok, "7: get_buffer_info ok");
    auto* slice_bytes = static_cast<uint8_t*>(slice_raw);
    Check(slice_len == 6 && slice_bytes != nullptr && slice_bytes[0] == 4 &&
              slice_bytes[5] == 9,
          "7: slice len & initial bytes");

    /* Bidirectional aliasing */
    if (ab_bytes != nullptr && slice_bytes != nullptr) {
      ab_bytes[5] = 0xAA;
      Check(slice_bytes[1] == 0xAA, "7: AB mutation visible via slice");
      slice_bytes[2] = 0xBB;
      Check(ab_bytes[6] == 0xBB, "7: slice mutation visible via AB");
    } else {
      Check(false, "7: AB/slice aliasing pointers non-null");
    }

    /* Zero-length slice at end of buffer */
    napi_value tail_zero = nullptr;
    CheckStatus(node_api_create_buffer_from_arraybuffer(env, ab, 16, 0, &tail_zero),
                napi_ok, "7: zero-length slice at end ok");
    is_buf = false;
    CheckStatus(napi_is_buffer(env, tail_zero, &is_buf), napi_ok,
                "7: zero-length slice is_buffer ok");
    Check(is_buf == true, "7: zero-length slice is Buffer");
    void* z_raw = nullptr;
    size_t z_len = 99;
    CheckStatus(napi_get_buffer_info(env, tail_zero, &z_raw, &z_len), napi_ok,
                "7: zero-length get_buffer_info ok");
    Check(z_len == 0, "7: zero-length slice len == 0");

    /* Non-ArrayBuffer -> napi_arraybuffer_expected */
    napi_value plain_obj = nullptr;
    CheckStatus(napi_create_object(env, &plain_obj), napi_ok, "7: create plain obj");
    napi_value bad = nullptr;
    CheckStatus(node_api_create_buffer_from_arraybuffer(env, plain_obj, 0, 0, &bad),
                napi_arraybuffer_expected, "7: plain object => arraybuffer_expected");
    CheckStatus(node_api_create_buffer_from_arraybuffer(env, slice, 0, 2, &bad),
                napi_arraybuffer_expected, "7: Uint8Array/Buffer => arraybuffer_expected");
    CheckStatus(node_api_create_buffer_from_arraybuffer(env, nullptr, 0, 0, &bad),
                napi_invalid_arg, "7: NULL ab => invalid_arg");
    CheckStatus(node_api_create_buffer_from_arraybuffer(env, ab, 0, 1, nullptr),
                napi_invalid_arg, "7: NULL result => invalid_arg");

    /* OOB offset / length -> pending RangeError */
    CheckStatus(node_api_create_buffer_from_arraybuffer(env, ab, 17, 0, &bad),
                napi_pending_exception, "7: OOB offset => pending_exception");
    Check(HasPending(env), "7: OOB offset leaves pending");
    napi_value oob_exc = nullptr;
    CheckStatus(napi_get_and_clear_last_exception(env, &oob_exc), napi_ok,
                "7: clear OOB offset exc");
    Check(IsInstanceOfGlobal(env, oob_exc, "RangeError"),
          "7: OOB offset throws RangeError");

    CheckStatus(node_api_create_buffer_from_arraybuffer(env, ab, 12, 5, &bad),
                napi_pending_exception, "7: OOB length => pending_exception");
    oob_exc = nullptr;
    CheckStatus(napi_get_and_clear_last_exception(env, &oob_exc), napi_ok,
                "7: clear OOB length exc");
    Check(IsInstanceOfGlobal(env, oob_exc, "RangeError"),
          "7: OOB length throws RangeError");

    /* Pending gate */
    CheckStatus(napi_throw_error(env, "ERR_GATE", "ab gate"), napi_ok,
                "7: seed pending gate");
    CheckStatus(node_api_create_buffer_from_arraybuffer(env, ab, 0, 2, &bad),
                napi_pending_exception, "7: gated by pending exception");
    napi_value gate_exc = nullptr;
    CheckStatus(napi_get_and_clear_last_exception(env, &gate_exc), napi_ok,
                "7: clear gated exc");
    Check(ReadStrProp(env, gate_exc, "message") == "ab gate",
          "7: gated call preserves original exception");

    /* Detached ArrayBuffer -> RangeError */
    CheckStatus(napi_detach_arraybuffer(env, ab), napi_ok, "7: detach AB ok");
    CheckStatus(node_api_create_buffer_from_arraybuffer(env, ab, 0, 4, &bad),
                napi_pending_exception, "7: detached AB => pending_exception");
    napi_value det_exc = nullptr;
    CheckStatus(napi_get_and_clear_last_exception(env, &det_exc), napi_ok,
                "7: clear detached exc");
    Check(IsInstanceOfGlobal(env, det_exc, "RangeError"),
          "7: detached AB throws RangeError");

    CheckStatus(napi_close_handle_scope(env, sc), napi_ok, "7: close scope");
    chakra_nodeapi_destroy_env(env);
  }

  /* ---- 8. NestedWrap ref cancellation + Closing versioned return codes ---- */
  {
    std::fprintf(stdout, "-- case 8: NestedWrap + Closing version codes --\n");
    napi_env env = chakra_nodeapi_create_env(rt, ctx, "/tmp/nested.node", nullptr, nullptr);
    Check(env != nullptr, "8: create env");
    NestedWrapState nst;
    nst.env = env;
    napi_ref witness = nullptr;
    Check(SetupNestedWrapAndDrop(env, &nst, &witness) && witness != nullptr &&
              nst.wrapper_ref != nullptr && nst.rider_ref != nullptr,
          "8: setup NestedWrap head + rider weak refs");

    ClearStack();
    Collect10x(rt);
    {
      napi_handle_scope vsc = nullptr;
      CheckStatus(napi_open_handle_scope(env, &vsc), napi_ok, "8: open witness scope");
      napi_value got = reinterpret_cast<napi_value>(static_cast<std::uintptr_t>(0x1));
      CheckStatus(napi_get_reference_value(env, witness, &got), napi_ok,
                  "8: witness get after GC");
      Check(got == nullptr, "8: NestedWrap object collected (weak optional refs)");
      CheckStatus(napi_close_handle_scope(env, vsc), napi_ok, "8: close witness scope");
      CheckStatus(napi_delete_reference(env, witness), napi_ok, "8: delete witness");
    }
    Check(nst.total_calls == 0, "8: zero finalizers during GC");
    chakra_nodeapi_test_drain_finalizers(env);
    Check(nst.head_calls == 1, "8: head finalizer ran once");
    Check(nst.rider_calls == 0, "8: rider suppressed by delete_reference in head");
    Check(nst.total_calls == 1, "8: total finalizer calls == 1 (not 2)");
    Check(chakra_nodeapi_test_wrap_live_count(env) == 0, "8: wrap live 0 after drain");
    chakra_nodeapi_destroy_env(env);

    /* Closing versioned status: v10 -> cannot_run_js, v9 -> pending_exception, v8 -> generic_failure */
    const struct {
      int32_t ver;
      napi_status want;
      const char* label;
    } kCases[] = {
        {10, napi_cannot_run_js, "8: Closing v10 => napi_cannot_run_js"},
        {9, napi_pending_exception, "8: Closing v9 => napi_pending_exception"},
        {8, napi_generic_failure, "8: Closing v8 => napi_generic_failure"},
    };
    for (const auto& tc : kCases) {
      napi_env ce = chakra_nodeapi_create_env(rt, ctx, "/tmp/close.node", nullptr, nullptr);
      Check(ce != nullptr, "8: create closing-test env");
      CheckStatus(chakra_nodeapi_set_module_api_version(ce, tc.ver), napi_ok,
                  "8: set_module_api_version for closing test");
      ClosingHookState chs{};
      chs.env = ce;
      CheckStatus(napi_add_env_cleanup_hook(ce, ClosingCheckHook, &chs), napi_ok,
                  "8: add closing hook");
      chakra_nodeapi_destroy_env(ce);
      Check(chs.ran == 1, "8: closing hook ran once");
      CheckStatus(chs.get_global_status, napi_ok,
                  "8: napi_get_global ok in Closing");
      CheckStatus(chs.get_named_status, tc.want, tc.label);
    }
  }

  /* ---- 9. Multi-env cross-addon Buffer sharing ---- */
  {
    std::fprintf(stdout, "-- case 9: multi-env cross-addon Buffer sharing --\n");
    napi_env envA = chakra_nodeapi_create_env(rt, ctx, "/tmp/addonA.node", nullptr, nullptr);
    napi_env envB = chakra_nodeapi_create_env(rt, ctx, "/tmp/addonB.node", nullptr, nullptr);
    Check(envA != nullptr && envB != nullptr, "9: create envA & envB");
    CheckStatus(chakra_nodeapi_set_module_api_version(envA, 9), napi_ok, "9: envA v9");
    CheckStatus(chakra_nodeapi_set_module_api_version(envB, 10), napi_ok, "9: envB v10");

    napi_handle_scope scB = nullptr;
    CheckStatus(napi_open_handle_scope(envB, &scB), napi_ok, "9: open scB");
    napi_handle_scope scA = nullptr;
    CheckStatus(napi_open_handle_scope(envA, &scA), napi_ok, "9: open scA");

    /* Buffer created in envA */
    const uint8_t init_bytes[4] = {11, 22, 33, 44};
    void* bufA_data = nullptr;
    napi_value bufA = nullptr;
    CheckStatus(napi_create_buffer_copy(envA, 4, init_bytes, &bufA_data, &bufA),
                napi_ok, "9: envA create_buffer_copy");

    /* Root bufA + sliceB via envB refs so they outlive envA destruction cleanly */
    napi_ref bufA_refB = nullptr;
    CheckStatus(napi_create_reference(envB, bufA, 1, &bufA_refB), napi_ok,
                "9: envB refs bufA");

    bool is_b = false;
    CheckStatus(napi_is_buffer(envB, bufA, &is_b), napi_ok, "9: is_buffer(envB, bufA) ok");
    Check(is_b == true, "9: bufA recognized as Buffer in envB");
    void* got_ptrB = nullptr;
    size_t got_lenB = 0;
    CheckStatus(napi_get_buffer_info(envB, bufA, &got_ptrB, &got_lenB), napi_ok,
                "9: get_buffer_info(envB, bufA) ok");
    Check(got_lenB == 4 && std::memcmp(got_ptrB, init_bytes, 4) == 0,
          "9: envB reads exact bytes from envA Buffer");

    /* Slice created in envB via node_api_create_buffer_from_arraybuffer */
    napi_value ab = nullptr;
    void* ab_raw = nullptr;
    CheckStatus(napi_create_arraybuffer(envB, 8, &ab_raw, &ab), napi_ok,
                "9: envB create AB");
    auto* ab_u8 = static_cast<uint8_t*>(ab_raw);
    if (ab_u8 != nullptr) {
      for (uint8_t i = 0; i < 8; ++i) ab_u8[i] = static_cast<uint8_t>(100 + i);
    }
    napi_value sliceB = nullptr;
    CheckStatus(node_api_create_buffer_from_arraybuffer(envB, ab, 2, 4, &sliceB),
                napi_ok, "9: envB create slice from AB");
    napi_ref sliceB_refB = nullptr;
    CheckStatus(napi_create_reference(envB, sliceB, 1, &sliceB_refB), napi_ok,
                "9: envB refs sliceB");

    bool is_a = false;
    CheckStatus(napi_is_buffer(envA, sliceB, &is_a), napi_ok,
                "9: is_buffer(envA, sliceB) ok");
    Check(is_a == true, "9: sliceB recognized as Buffer in envA");
    void* got_ptrA = nullptr;
    size_t got_lenA = 0;
    CheckStatus(napi_get_buffer_info(envA, sliceB, &got_ptrA, &got_lenA), napi_ok,
                "9: get_buffer_info(envA, sliceB) ok");
    Check(got_lenA == 4 && static_cast<uint8_t*>(got_ptrA)[0] == 102 &&
              static_cast<uint8_t*>(got_ptrA)[3] == 105,
          "9: envA reads exact sliceB bytes");

    /* Plain Uint8Array is NOT a Buffer in either env */
    napi_value plain_ta = nullptr;
    CheckStatus(napi_create_typedarray(envA, napi_uint8_array, 4, ab, 0, &plain_ta),
                napi_ok, "9: create plain Uint8Array");
    bool plain_is_a = true;
    bool plain_is_b = true;
    CheckStatus(napi_is_buffer(envA, plain_ta, &plain_is_a), napi_ok, "9: is_buf plain A");
    CheckStatus(napi_is_buffer(envB, plain_ta, &plain_is_b), napi_ok, "9: is_buf plain B");
    Check(!plain_is_a && !plain_is_b, "9: plain Uint8Array not a Buffer in either env");

    /* Destroy envA while envB stays alive! Shared marker Symbol must stay alive. */
    CheckStatus(napi_close_handle_scope(envA, scA), napi_ok, "9: close scA");
    chakra_nodeapi_destroy_env(envA);

    napi_value bufA_from_B = nullptr;
    napi_value sliceB_from_B = nullptr;
    CheckStatus(napi_get_reference_value(envB, bufA_refB, &bufA_from_B), napi_ok,
                "9: envB deref bufA post-envA-destroy");
    CheckStatus(napi_get_reference_value(envB, sliceB_refB, &sliceB_from_B), napi_ok,
                "9: envB deref sliceB post-envA-destroy");
    is_b = false;
    CheckStatus(napi_is_buffer(envB, bufA_from_B, &is_b), napi_ok,
                "9: is_buffer(envB, bufA) after envA destroy");
    Check(is_b == true, "9: shared Buffer marker stays alive after envA destroy");
    got_ptrB = nullptr;
    got_lenB = 0;
    CheckStatus(napi_get_buffer_info(envB, bufA_from_B, &got_ptrB, &got_lenB),
                napi_ok, "9: get_buffer_info(envB, bufA) post-envA-destroy");
    Check(got_lenB == 4 && std::memcmp(got_ptrB, init_bytes, 4) == 0,
          "9: bufA bytes intact in envB after envA destroy");
    is_b = false;
    CheckStatus(napi_is_buffer(envB, sliceB_from_B, &is_b), napi_ok,
                "9: is_buffer(envB, sliceB) post-envA-destroy");
    Check(is_b == true, "9: sliceB still Buffer in envB after envA destroy");

    /* Creating a fresh Buffer in envB after envA destroy works */
    napi_value bufB2 = nullptr;
    void* bufB2_ptr = nullptr;
    CheckStatus(napi_create_buffer(envB, 3, &bufB2_ptr, &bufB2), napi_ok,
                "9: envB create_buffer post-envA-destroy");
    is_b = false;
    CheckStatus(napi_is_buffer(envB, bufB2, &is_b), napi_ok,
                "9: envB new Buffer is_buffer");
    Check(is_b == true, "9: envB new Buffer recognized");

    CheckStatus(napi_delete_reference(envB, bufA_refB), napi_ok, "9: delete bufA_refB");
    CheckStatus(napi_delete_reference(envB, sliceB_refB), napi_ok, "9: delete sliceB_refB");
    CheckStatus(napi_close_handle_scope(envB, scB), napi_ok, "9: close scB");
    chakra_nodeapi_destroy_env(envB);

    /* Create envC on same (rt, ctx) after all prior Buffer leases closed */
    napi_env envC = chakra_nodeapi_create_env(rt, ctx, "/tmp/addonC.node", nullptr, nullptr);
    Check(envC != nullptr, "9: create envC after envA+envB destroyed");
    napi_handle_scope scC = nullptr;
    CheckStatus(napi_open_handle_scope(envC, &scC), napi_ok, "9: open scC");
    napi_value bufC = nullptr;
    void* ptrC = nullptr;
    CheckStatus(napi_create_buffer(envC, 2, &ptrC, &bufC), napi_ok,
                "9: envC create_buffer");
    bool is_c = false;
    CheckStatus(napi_is_buffer(envC, bufC, &is_c), napi_ok, "9: envC is_buffer");
    Check(is_c == true, "9: envC fresh Buffer symbol works after full release");
    CheckStatus(napi_close_handle_scope(envC, scC), napi_ok, "9: close scC");
    chakra_nodeapi_destroy_env(envC);
  }

  std::fprintf(stdout, "V10_MULTI_ADDON_PROBE: %s (%d/%d)\n",
               (g_pass == g_total) ? "PASS" : "FAIL", g_pass, g_total);
  std::fflush(stdout);

  JsSetCurrentContext(JS_INVALID_REFERENCE);
  JsDisposeRuntime(rt);
  return (g_pass == g_total) ? 0 : 1;
}
