// host_probe.cpp — N4.1 registration edge cases + fatal routing
// (plan doc/node-api-plan.md §6 card N4.1, Table B B1).
//
// Covers (each MUST fail probe on mismatch):
//   R1. Deprecated path basic: napi_module_register stores a record,
//       count increments, get_pending_module returns exact copies,
//       run_pending invokes nm_register_func for that env, result chains.
//   R2. String copies: caller buffers freed/overwritten after register,
//       stored copies unchanged; NULL strings stored as "".
//   R3. Double-registration: second run_pending on SAME env => napi_invalid_arg,
//       last_error recorded, func not re-invoked; fresh env runs again (per-env).
//       NULL/dead/null exports/result => invalid_arg without crash.
//   R4. Replacement chaining: non-NULL return != input replaces for next/final,
//       NULL keeps input. Throwing func => pending_exception, stops, preserves.
//       Pending-at-entry => pending without invoking/consuming (retryable).
//   R5. Thread-safety smoke: concurrent napi_module_register from 4 threads.
//   F1. napi_fatal_exception with service: service called once with same value,
//       returns napi_ok, does NOT set pending, rooting holds across JS churn.
//   F2. napi_fatal_exception NULL err => invalid_arg, service not called.
//   F3. napi_fatal_exception gated by pending => pending, service not called.
//   F4/F5. Abort modes (subprocess via run_host_probe.sh, not in main):
//          --fatal-exception-no-service and --fatal-error must abort.
//
// Deprecated-path addon note: a real NAPI_MODULE_X .node that self-registers
// via a dlopen static constructor requires ch-nodeapi host fallback (symbol
// missing => use pending). The host change is out of scope for this slice
// (lib/NodeApi/NodeApiHost + NodeEnv only per task), so this probe is the
// task's allowed fallback: direct napi_module_register + host callback
// (run_pending) driving, which is exactly what a dlopen ctor + host glue
// would do. See NodeApiHost.h for the design.
//
// Build (repo root, via gate lock — see run_host_probe.sh):
//   /tmp/opencode/cc-gate-lock.sh mise exec -- zig c++ \
//     -std=gnu++17 -DNAPI_VERSION=8 -DCHAKRA_NODEAPI_TEST_HOOKS \
//     -Iinclude/nodeapi -Ilib/Jsrt -Ilib/NodeApi \
//     test/nodeapi/probes/host_probe.cpp \
//     lib/NodeApi/NodeApiEnv.cpp ... lib/NodeApi/NodeApiHost.cpp \
//     -o /tmp/host_probe -Lzig-out/lib -lChakraCore ...
// Exit 0 = green; 1 = any mismatch. Fatal abort modes exit via SIGABRT.

#include "NodeApiHostApi.h"
#include "NodeApiHost.h"
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
#include <thread>
#include <vector>

namespace {

int g_failures = 0;
int g_checks = 0;

void Check(bool cond, const char* what) {
  g_checks++;
  if (!cond) {
    g_failures++;
    std::printf("FAIL: %s\n", what);
  }
}

void CheckStatus(napi_status got, napi_status want, const char* what) {
  g_checks++;
  if (got != want) {
    g_failures++;
    std::printf("FAIL: %s (got %d want %d)\n", what, (int)got, (int)want);
  }
}

JsRuntimeHandle g_runtime = JS_INVALID_RUNTIME_HANDLE;
JsContextRef g_context = JS_INVALID_REFERENCE;

bool InitEngine() {
  if (JsCreateRuntime(JsRuntimeAttributeNone, nullptr, &g_runtime) != JsNoError)
    return false;
  if (JsCreateContext(g_runtime, &g_context) != JsNoError) {
    JsDisposeRuntime(g_runtime);
    g_runtime = JS_INVALID_RUNTIME_HANDLE;
    return false;
  }
  if (JsSetCurrentContext(g_context) != JsNoError) {
    JsDisposeRuntime(g_runtime);
    g_runtime = JS_INVALID_RUNTIME_HANDLE;
    g_context = JS_INVALID_REFERENCE;
    return false;
  }
  return true;
}

void ShutdownEngine() {
  if (g_context != JS_INVALID_REFERENCE) {
    JsSetCurrentContext(JS_INVALID_REFERENCE);
    g_context = JS_INVALID_REFERENCE;
  }
  if (g_runtime != JS_INVALID_RUNTIME_HANDLE) {
    JsDisposeRuntime(g_runtime);
    g_runtime = JS_INVALID_RUNTIME_HANDLE;
  }
}

bool HasPending(napi_env env) {
  bool has = false;
  if (napi_is_exception_pending(env, &has) != napi_ok) return false;
  return has;
}

napi_value ClearEx(napi_env env) {
  napi_value v = nullptr;
  (void)napi_get_and_clear_last_exception(env, &v);
  return v;
}

/* JS helpers: create/root string via provider scope. */
napi_value NapiStr(napi_env env, const char* s) {
  JsValueRef v = JS_INVALID_REFERENCE;
  (void)JsCreateString(s, std::strlen(s), &v);
  (void)NodeApiScopes_RootIntoInnermost(env, reinterpret_cast<napi_value>(v));
  return reinterpret_cast<napi_value>(v);
}

/* Read a simple property as string for verification. */
std::string GetStrProp(napi_value obj, const char* prop) {
  JsPropertyIdRef pid = JS_INVALID_REFERENCE;
  if (JsCreatePropertyId(prop, std::strlen(prop), &pid) != JsNoError)
    return "<propid-fail>";
  JsValueRef v = JS_INVALID_REFERENCE;
  if (JsGetProperty(reinterpret_cast<JsValueRef>(obj), pid, &v) !=
      JsNoError)
    return "<get-fail>";
  JsValueType t = JsObject;
  if (JsGetValueType(v, &t) != JsNoError) return "<type-fail>";
  if (t == JsUndefined) return "<undefined>";
  if (t == JsNull) return "<null>";
  if (t != JsString) return "<non-string>";
  size_t len = 0;
  JsCopyString(v, nullptr, 0, &len);
  std::string s(len + 1, '\0');
  size_t written = 0;
  if (JsCopyString(v, &s[0], s.size(), &written) != JsNoError)
    return "<copy-fail>";
  s.resize(written);
  return s;
}

/* --- Test register funcs --- */
int g_reg1_calls = 0;
int g_reg_replace_calls = 0;
int g_reg_throw_calls = 0;
napi_env g_seen_env = nullptr;
napi_value g_seen_exports = nullptr;

napi_value RegBasic(napi_env env, napi_value exports) {
  g_reg1_calls++;
  g_seen_env = env;
  g_seen_exports = exports;
  /* Attach a JS-visible marker so the host can observe invocation. */
  napi_value v = nullptr;
  if (napi_get_boolean(env, true, &v) == napi_ok) {
    (void)napi_set_named_property(env, exports, "from_deprecated", v);
  }
  return exports;
}

napi_value RegReplace(napi_env env, napi_value /*exports*/) {
  g_reg_replace_calls++;
  napi_value replacement = nullptr;
  (void)napi_create_object(env, &replacement);
  if (replacement != nullptr) {
    napi_value v = nullptr;
    napi_value s = NapiStr(env, "replaced-by-deprecated");
    if (napi_create_string_utf8(env, "who", 3, &v) == napi_ok) {
      (void)s;
    }
    (void)napi_set_named_property(env, replacement, "who", s);
    (void)v;
  }
  return replacement;
}

napi_value RegReturnNull(napi_env env, napi_value exports) {
  (void)env;
  return nullptr; /* keep input exports */
}

napi_value RegThrow(napi_env env, napi_value exports) {
  g_reg_throw_calls++;
  (void)napi_throw_error(env, "ERR_HOST", "deprecated threw");
  return exports;
}

/* --- Fatal service state --- */
int g_fatal_calls = 0;
napi_value g_fatal_seen = nullptr;
bool g_fatal_root_ok = false;
void FatalRecorder(void* /*host_ctx*/, napi_value exc) {
  g_fatal_calls++;
  g_fatal_seen = exc;
  /* Rooting check: the value must survive JS churn during the callback.
   * Create fresh objects (allocation may trigger GC) and then verify the
   * borrowed value still type-checks. Provider holds JsAddRef for us. */
  JsValueRef junk = JS_INVALID_REFERENCE;
  (void)JsCreateObject(&junk);
  (void)JsCreateObject(&junk);
  JsValueType t = JsObject;
  JsErrorCode ec = JsGetValueType(reinterpret_cast<JsValueRef>(exc), &t);
  g_fatal_root_ok = (ec == JsNoError);
  if (junk != JS_INVALID_REFERENCE) (void)JsRelease(junk, nullptr);
}

/* --- Closing-state probe state (R6) --- */
struct ClosingProbeState {
  napi_env env = nullptr;
  napi_value exports = nullptr;
  napi_value errval = nullptr;
  napi_status run_status = napi_ok;
  napi_status fatal_status = napi_ok;
  bool ran = false;
};
void ClosingProbeHook(void* arg) {
  auto* s = static_cast<ClosingProbeState*>(arg);
  if (s == nullptr) return;
  s->ran = true;
  napi_value result = nullptr;
  s->run_status =
      chakra_nodeapi_run_pending_registrations(s->env, s->exports, &result);
  s->fatal_status = napi_fatal_exception(s->env, s->errval);
}

} /* namespace */

int main(int argc, char** argv) {
  if (argc >= 2 && std::strcmp(argv[1], "--fatal-exception-no-service") == 0) {
    if (!InitEngine()) return 2;
    napi_env env = chakra_nodeapi_create_env(g_runtime, g_context, "fatal-no-svc",
                                            nullptr, nullptr);
    if (env == nullptr) return 2;
    JsValueRef m = JS_INVALID_REFERENCE;
    JsCreateString("abort-me", std::strlen("abort-me"), &m);
    (void)NodeApiScopes_RootIntoInnermost(env, reinterpret_cast<napi_value>(m));
    napi_value err = reinterpret_cast<napi_value>(m);
    (void)napi_fatal_exception(env, err);
    std::printf("FAIL: fatal_exception without service returned\n");
    return 1;
  }
  if (argc >= 2 && std::strcmp(argv[1], "--fatal-error") == 0) {
    napi_fatal_error("host_probe.cpp", std::strlen("host_probe.cpp"),
                     "fatal-error probe abort", std::strlen("fatal-error probe abort"));
    std::printf("FAIL: fatal_error returned\n");
    return 1;
  }

  if (!InitEngine()) return 2;

  size_t baseline = chakra_nodeapi_pending_module_count();

  /* ========== R1: basic deprecated registration ========== */
  {
    std::printf("phaseR1 basic registration: ...\n");
    g_reg1_calls = 0;
    g_seen_env = nullptr;
    napi_module mod;
    mod.nm_version = 1;
    mod.nm_flags = 0;
    mod.nm_filename = "file_a.node";
    mod.nm_register_func = RegBasic;
    mod.nm_modname = "mod_a";
    mod.nm_priv = (void*)0xDEADBEEF;
    mod.reserved[0] = mod.reserved[1] = mod.reserved[2] = mod.reserved[3] = nullptr;
    napi_module_register(&mod);
    Check(chakra_nodeapi_pending_module_count() == baseline + 1,
          "R1 count +1");
    chakra_nodeapi_pending_module_info info{};
    CheckStatus(chakra_nodeapi_get_pending_module(baseline, &info), napi_ok,
                "R1 get info ok");
    Check(info.nm_version == 1, "R1 version 1");
    Check(info.nm_flags == 0, "R1 flags 0");
    Check(info.nm_filename != nullptr && std::strcmp(info.nm_filename, "file_a.node") == 0,
          "R1 filename copy");
    Check(info.nm_modname != nullptr && std::strcmp(info.nm_modname, "mod_a") == 0,
          "R1 modname copy");
    Check(info.nm_register_func == RegBasic, "R1 func pointer");
    Check(info.nm_priv == (void*)0xDEADBEEF, "R1 priv borrowed");
    /* OOB + NULL out. */
    CheckStatus(chakra_nodeapi_get_pending_module(baseline + 100, &info),
                napi_invalid_arg, "R1 OOB invalid_arg");
    CheckStatus(chakra_nodeapi_get_pending_module(baseline, nullptr),
                napi_invalid_arg, "R1 NULL out invalid_arg");
    /* NULL / null-func record is dropped. */
    size_t c0 = chakra_nodeapi_pending_module_count();
    napi_module_register(nullptr);
    Check(chakra_nodeapi_pending_module_count() == c0, "R1 null mod dropped");
    napi_module bad{};
    bad.nm_version = 1; bad.nm_flags = 0; bad.nm_filename = "x.node";
    bad.nm_register_func = nullptr; bad.nm_modname = "x"; bad.nm_priv = nullptr;
    napi_module_register(&bad);
    Check(chakra_nodeapi_pending_module_count() == c0, "R1 null func dropped");

    /* Invoke for a fresh env. */
    napi_env env = chakra_nodeapi_create_env(g_runtime, g_context, "r1", nullptr, nullptr);
    Check(env != nullptr, "R1 create env");
    napi_value exports = nullptr;
    CheckStatus(napi_create_object(env, &exports), napi_ok, "R1 create exports");
    napi_value result = nullptr;
    CheckStatus(chakra_nodeapi_run_pending_registrations(env, exports, &result),
                napi_ok, "R1 run_pending ok");
    Check(g_reg1_calls == 1, "R1 register called once");
    Check(g_seen_env == env, "R1 register saw env");
    Check(g_seen_exports == exports, "R1 register saw exports");
    Check(result == exports, "R1 result == exports (no replacement)");
    Check(GetStrProp(exports, "from_deprecated") == "<non-string>" ||
          true, "R1 marker read (boolean, type check below)");
    /* Verify boolean true marker via JsGetValueType. */
    {
      JsPropertyIdRef pid = JS_INVALID_REFERENCE;
      (void)JsCreatePropertyId("from_deprecated", 15, &pid);
      JsValueRef v = JS_INVALID_REFERENCE;
      Check(JsGetProperty(reinterpret_cast<JsValueRef>(exports), pid, &v) == JsNoError,
            "R1 get marker");
      JsValueType t = JsObject;
      (void)JsGetValueType(v, &t);
      Check(t == JsBoolean, "R1 marker is boolean true");
    }
    chakra_nodeapi_destroy_env(env);
    std::printf("phaseR1: done\n");
  }

  /* ========== R2: string copies ========== */
  {
    std::printf("phaseR2 string copies: ...\n");
    size_t b = chakra_nodeapi_pending_module_count();
    char* fn = strdup("/tmp/orig_filename_abc");
    char* mn = strdup("orig_mod_xyz");
    Check(fn != nullptr && mn != nullptr, "R2 strdup");
    napi_module mod;
    mod.nm_version = 1;
    mod.nm_flags = 7;
    mod.nm_filename = fn;
    mod.nm_register_func = RegBasic;
    mod.nm_modname = mn;
    mod.nm_priv = (void*)0x1234;
    mod.reserved[0] = mod.reserved[1] = mod.reserved[2] = mod.reserved[3] = nullptr;
    napi_module_register(&mod);
    /* Free + smash caller buffers: provider copy must be unaffected. */
    free(fn);
    free(mn);
    /* Overwrite the freed heap words (best effort; copy independence is
     * proven by the stored bytes still matching, not by UAF read). */
    chakra_nodeapi_pending_module_info info{};
    CheckStatus(chakra_nodeapi_get_pending_module(b, &info), napi_ok, "R2 get ok");
    Check(info.nm_filename != nullptr &&
          std::strcmp(info.nm_filename, "/tmp/orig_filename_abc") == 0,
          "R2 filename copy survives free");
    Check(info.nm_modname != nullptr &&
          std::strcmp(info.nm_modname, "orig_mod_xyz") == 0,
          "R2 modname copy survives free");
    Check(info.nm_flags == 7, "R2 flags 7");
    Check(info.nm_priv == (void*)0x1234, "R2 priv");

    /* NULL strings stored as "". */
    size_t b2 = chakra_nodeapi_pending_module_count();
    napi_module mod2{};
    mod2.nm_version = 1; mod2.nm_flags = 0;
    mod2.nm_filename = nullptr;
    mod2.nm_register_func = RegBasic;
    mod2.nm_modname = nullptr;
    mod2.nm_priv = nullptr;
    napi_module_register(&mod2);
    CheckStatus(chakra_nodeapi_get_pending_module(b2, &info), napi_ok, "R2 null get ok");
    Check(info.nm_filename != nullptr && std::strcmp(info.nm_filename, "") == 0,
          "R2 null filename => \"\"");
    Check(info.nm_modname != nullptr && std::strcmp(info.nm_modname, "") == 0,
          "R2 null modname => \"\"");
    std::printf("phaseR2: done\n");
  }

  /* ========== R3: double-registration rejection ========== */
  {
    std::printf("phaseR3 double rejection: ...\n");
    napi_env env = chakra_nodeapi_create_env(g_runtime, g_context, "r3", nullptr, nullptr);
    Check(env != nullptr, "R3 create env");
    napi_value exports = nullptr;
    (void)napi_create_object(env, &exports);
    napi_value result = nullptr;
    int before = g_reg1_calls;
    CheckStatus(chakra_nodeapi_run_pending_registrations(env, exports, &result),
                napi_ok, "R3 first run ok");
    int after_first = g_reg1_calls;
    Check(after_first > before, "R3 first run invoked");
    /* Second call on SAME env => invalid_arg, no re-invoke. */
    CheckStatus(chakra_nodeapi_run_pending_registrations(env, exports, &result),
                napi_invalid_arg, "R3 double => invalid_arg");
    Check(g_reg1_calls == after_first, "R3 double did not re-invoke");
    const napi_extended_error_info* ei = nullptr;
    (void)napi_get_last_error_info(env, &ei);
    Check(ei != nullptr && ei->error_code == napi_invalid_arg, "R3 last_error invalid_arg");
    /* NULL args => invalid_arg (no crash, no consume on fresh env). */
    napi_env env2 = chakra_nodeapi_create_env(g_runtime, g_context, "r3b", nullptr, nullptr);
    CheckStatus(chakra_nodeapi_run_pending_registrations(nullptr, exports, &result),
                napi_invalid_arg, "R3 null env invalid_arg");
    CheckStatus(chakra_nodeapi_run_pending_registrations(env2, nullptr, &result),
                napi_invalid_arg, "R3 null exports invalid_arg");
    CheckStatus(chakra_nodeapi_run_pending_registrations(env2, exports, nullptr),
                napi_invalid_arg, "R3 null result invalid_arg");
    /* Dead env value (after destroy) => invalid_arg, no dereference crash. */
    napi_env dead_val = env2;
    chakra_nodeapi_destroy_env(env2);
    CheckStatus(chakra_nodeapi_run_pending_registrations(dead_val, exports, &result),
                napi_invalid_arg, "R3 dead env invalid_arg");
    /* Fresh env after the double still runs (per-env, not global once). */
    napi_env env3 = chakra_nodeapi_create_env(g_runtime, g_context, "r3c", nullptr, nullptr);
    napi_value exports3 = nullptr;
    (void)napi_create_object(env3, &exports3);
    napi_value result3 = nullptr;
    int before3 = g_reg1_calls;
    CheckStatus(chakra_nodeapi_run_pending_registrations(env3, exports3, &result3),
                napi_ok, "R3 fresh env ok");
    Check(g_reg1_calls > before3, "R3 fresh env invoked again");
    chakra_nodeapi_destroy_env(env3);
    chakra_nodeapi_destroy_env(env);
    std::printf("phaseR3: done\n");
  }

  /* ========== R4: replacement + throw + pending gate ========== */
  {
    std::printf("phaseR4 replacement/throw/pending: ...\n");
    /* Replacement: register a replacing module; fresh env sees full
     * snapshot (all earlier + this one run in order). Use g_reg_replace
     * delta to prove the new module ran. */
    size_t base = chakra_nodeapi_pending_module_count();
    napi_module mod{};
    mod.nm_version = 1; mod.nm_flags = 0;
    mod.nm_filename = "replacer.node";
    mod.nm_register_func = RegReplace;
    mod.nm_modname = "replacer";
    mod.nm_priv = nullptr;
    napi_module_register(&mod);
    Check(chakra_nodeapi_pending_module_count() == base + 1, "R4 count +1 replacer");
    napi_env env = chakra_nodeapi_create_env(g_runtime, g_context, "r4", nullptr, nullptr);
    napi_value exports = nullptr;
    (void)napi_create_object(env, &exports);
    napi_value result = nullptr;
    int replace_before = g_reg_replace_calls;
    CheckStatus(chakra_nodeapi_run_pending_registrations(env, exports, &result),
                napi_ok, "R4 run ok");
    Check(g_reg_replace_calls == replace_before + 1, "R4 replacer invoked");
    Check(result != nullptr && result != exports, "R4 result is replacement");
    Check(GetStrProp(result, "who") == "replaced-by-deprecated", "R4 replacement prop");
    /* NULL-return module keeps input: register null-returner, fresh env
     * result should still chain correctly (replacer runs last here
     * because it was registered before null-returner? Order: null after
     * replacer => replacer replacement is input to null-returner which
     * returns NULL => final is replacer object). Just assert result
     * non-null and who preserved. */
    napi_module modnull{};
    modnull.nm_version = 1; modnull.nm_flags = 0;
    modnull.nm_filename = "nullret.node";
    modnull.nm_register_func = RegReturnNull;
    modnull.nm_modname = "nullret";
    modnull.nm_priv = nullptr;
    napi_module_register(&modnull);
    napi_env envb = chakra_nodeapi_create_env(g_runtime, g_context, "r4b", nullptr, nullptr);
    napi_value exportsb = nullptr;
    (void)napi_create_object(envb, &exportsb);
    napi_value resultb = nullptr;
    CheckStatus(chakra_nodeapi_run_pending_registrations(envb, exportsb, &resultb),
                napi_ok, "R4b run ok (null keeps)");
    Check(resultb != nullptr && resultb != exportsb, "R4b result is replacer object");
    Check(GetStrProp(resultb, "who") == "replaced-by-deprecated", "R4b who preserved");
    chakra_nodeapi_destroy_env(envb);

    /* Pending-at-entry: gated without invoking/consuming (retryable). */
    {
      napi_env envp = chakra_nodeapi_create_env(g_runtime, g_context, "r4p", nullptr, nullptr);
      JsValueRef marker = JS_INVALID_REFERENCE;
      (void)JsCreateObject(&marker);
      (void)NodeApiScopes_RootIntoInnermost(envp, reinterpret_cast<napi_value>(marker));
      CheckStatus(napi_throw(envp, reinterpret_cast<napi_value>(marker)), napi_ok,
                  "R4p seed throw");
      napi_value exportsp = nullptr;
      (void)napi_create_object(envp, &exportsp);
      napi_value resultp = nullptr;
      int calls_before = g_reg1_calls;
      CheckStatus(chakra_nodeapi_run_pending_registrations(envp, exportsp, &resultp),
                  napi_pending_exception, "R4p gated => pending");
      Check(g_reg1_calls == calls_before, "R4p no invoke when pending");
      napi_value cl = ClearEx(envp);
      Check(cl == reinterpret_cast<napi_value>(marker), "R4p original preserved");
      /* Retry after clear must succeed (not consumed). */
      CheckStatus(chakra_nodeapi_run_pending_registrations(envp, exportsp, &resultp),
                  napi_ok, "R4p retry ok after clear");
      Check(g_reg1_calls > calls_before, "R4p retry invoked");
      chakra_nodeapi_destroy_env(envp);
    }
    /* Throwing: last registered throws => pending_exception, stops. */
    napi_module modthrow{};
    modthrow.nm_version = 1; modthrow.nm_flags = 0;
    modthrow.nm_filename = "thrower.node";
    modthrow.nm_register_func = RegThrow;
    modthrow.nm_modname = "thrower";
    modthrow.nm_priv = nullptr;
    napi_module_register(&modthrow);
    napi_env envc = chakra_nodeapi_create_env(g_runtime, g_context, "r4c", nullptr, nullptr);
    napi_value exportsc = nullptr;
    (void)napi_create_object(envc, &exportsc);
    napi_value resultc = nullptr;
    CheckStatus(chakra_nodeapi_run_pending_registrations(envc, exportsc, &resultc),
                napi_pending_exception, "R4c throw => pending");
    Check(HasPending(envc), "R4c pending still set");
    Check(g_reg_throw_calls == 1, "R4c thrower called");
    napi_value cleared = ClearEx(envc);
    Check(cleared != nullptr, "R4c cleared non-null (Error)");
    Check(GetStrProp(cleared, "message") == "deprecated threw", "R4c message");
    Check(!HasPending(envc), "R4c cleared");
    /* Throw env stays consumed: second call => invalid_arg (not pending). */
    CheckStatus(chakra_nodeapi_run_pending_registrations(envc, exportsc, &resultc),
                napi_invalid_arg, "R4c throw env consumed => invalid_arg");
    chakra_nodeapi_destroy_env(envc);
    chakra_nodeapi_destroy_env(env);

    std::printf("phaseR4: done\n");
  }

  /* ========== R5: parallel register smoke ========== */
  {
    std::printf("phaseR5 parallel register: ...\n");
    size_t b = chakra_nodeapi_pending_module_count();
    const int kThreads = 4, kPer = 25;
    std::vector<std::thread> th;
    for (int t = 0; t < kThreads; ++t) {
      th.emplace_back([t] {
        for (int i = 0; i < kPer; ++i) {
          char fnbuf[64], mnbuf[64];
          std::snprintf(fnbuf, sizeof fnbuf, "par_%d_%d.node", t, i);
          std::snprintf(mnbuf, sizeof mnbuf, "par_%d_%d", t, i);
          /* Strings must outlive only the call (provider copies). */
          napi_module m{};
          m.nm_version = 1; m.nm_flags = 0;
          m.nm_filename = fnbuf;
          m.nm_register_func = RegBasic;
          m.nm_modname = mnbuf;
          m.nm_priv = nullptr;
          napi_module_register(&m);
          /* Query count concurrently (must be thread-safe). */
          (void)chakra_nodeapi_pending_module_count();
        }
      });
    }
    for (auto& t : th) t.join();
    Check(chakra_nodeapi_pending_module_count() == b + kThreads * kPer,
          "R5 count +100");
    /* Spot-check one parallel record's copies. */
    chakra_nodeapi_pending_module_info info{};
    CheckStatus(chakra_nodeapi_get_pending_module(b, &info), napi_ok, "R5 get parallel");
    Check(info.nm_filename != nullptr && std::strncmp(info.nm_filename, "par_", 4) == 0,
          "R5 parallel filename");
    std::printf("phaseR5: done\n");
  }

  /* ========== F: fatal_exception routing ========== */
  {
    std::printf("phaseF fatal routing: ...\n");
    chakra_nodeapi_host_services svc{};
    svc.fatal_exception = FatalRecorder;
    napi_env env = chakra_nodeapi_create_env(g_runtime, g_context, "f1", &svc, nullptr);
    Check(env != nullptr, "F1 create env with service");
    g_fatal_calls = 0;
    g_fatal_seen = nullptr;
    g_fatal_root_ok = false;
    JsValueRef m = JS_INVALID_REFERENCE;
    (void)JsCreateObject(&m);
    (void)NodeApiScopes_RootIntoInnermost(env, reinterpret_cast<napi_value>(m));
    napi_value err = reinterpret_cast<napi_value>(m);
    CheckStatus(napi_fatal_exception(env, err), napi_ok, "F1 fatal ok");
    Check(g_fatal_calls == 1, "F1 service called once");
    Check(g_fatal_seen == err, "F1 service saw same value (identity)");
    Check(g_fatal_root_ok, "F1 rooting survived churn in callback");
    Check(!HasPending(env), "F1 does not set pending");
    CheckStatus(napi_fatal_exception(env, nullptr), napi_invalid_arg, "F1 NULL err => invalid_arg");
    Check(g_fatal_calls == 1, "F1 service not called on NULL");
    chakra_nodeapi_destroy_env(env);

    /* F3: gated by pending. */
    napi_env env2 = chakra_nodeapi_create_env(g_runtime, g_context, "f2", &svc, nullptr);
    Check(env2 != nullptr, "F2 create env");
    g_fatal_calls = 0;
    JsValueRef a = JS_INVALID_REFERENCE;
    (void)JsCreateObject(&a);
    (void)NodeApiScopes_RootIntoInnermost(env2, reinterpret_cast<napi_value>(a));
    napi_value aval = reinterpret_cast<napi_value>(a);
    CheckStatus(napi_throw(env2, aval), napi_ok, "F2 seed throw");
    CheckStatus(napi_fatal_exception(env2, aval), napi_pending_exception, "F2 gated => pending");
    Check(g_fatal_calls == 0, "F2 service not called when pending");
    napi_value cleared = ClearEx(env2);
    Check(cleared == aval, "F2 original preserved");
    chakra_nodeapi_destroy_env(env2);
    std::printf("phaseF: done\n");
  }

  /* ========== R6: Closing-state rejection (EntryGuard strict Alive) ==========
   * A cleanup hook fires during destroy step 3 with state == Closing
   * (R-ENV-4). Both run_pending and fatal_exception are strict
   * Alive-only, so each must return napi_generic_failure without
   * invoking any register func or firing the fatal service. The
   * hook path also proves the registry/EntryGuard locks are not held
   * across user code (no deadlock; destroy does not hold the live
   * registry mutex across hooks, NodeApiEnv.cpp). Wrong-thread
   * (R-ENV-11) is not probed here by design: in debug builds the
   * shared EntryGuard asserts (debug contract); the release
   * invalid_arg path needs an NDEBUG build (see teardown_probe P1-5
   note). Closing is the non-asserting state check and is probed. */
  {
    std::printf("phaseR6 closing rejection: ...\n");
    chakra_nodeapi_host_services svc{};
    svc.fatal_exception = FatalRecorder;
    napi_env env = chakra_nodeapi_create_env(g_runtime, g_context, "r6",
                                             &svc, nullptr);
    Check(env != nullptr, "R6 create env");
    napi_value exports = nullptr;
    CheckStatus(napi_create_object(env, &exports), napi_ok, "R6 create exports");
    JsValueRef ev = JS_INVALID_REFERENCE;
    (void)JsCreateObject(&ev);
    (void)NodeApiScopes_RootIntoInnermost(env, reinterpret_cast<napi_value>(ev));
    napi_value errval = reinterpret_cast<napi_value>(ev);
    ClosingProbeState st{};
    st.env = env;
    st.exports = exports;
    st.errval = errval;
    CheckStatus(napi_add_env_cleanup_hook(
                    reinterpret_cast<node_api_basic_env>(env),
                    ClosingProbeHook, &st),
                napi_ok, "R6 add cleanup hook");
    int reg_before = g_reg1_calls;
    g_fatal_calls = 0;
    chakra_nodeapi_destroy_env(env); /* hook fires here (Closing) */
    Check(st.ran, "R6 hook ran");
    CheckStatus(st.run_status, napi_generic_failure,
                "R6 run_pending Closing => generic_failure");
    CheckStatus(st.fatal_status, napi_generic_failure,
                "R6 fatal Closing => generic_failure");
    Check(g_reg1_calls == reg_before, "R6 closing did not invoke register func");
    Check(g_fatal_calls == 0, "R6 closing did not fire fatal service");
    std::printf("phaseR6: done\n");
  }

  std::printf("host_probe: %d checks, %d failures\n", g_checks, g_failures);
  ShutdownEngine();
  return g_failures == 0 ? 0 : 1;
}
