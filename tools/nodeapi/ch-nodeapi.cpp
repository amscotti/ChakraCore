//-------------------------------------------------------------------------------------------------------
// Copyright (c) ChakraCore Project Contributors. All rights reserved.
// Licensed under the MIT license. See LICENSE.txt file in the project root for full license information.
//-------------------------------------------------------------------------------------------------------
//
// Test host for the Node-API provider (plan doc/node-api-plan.md N0.3).
//
// Loads a provider shared library RTLD_GLOBAL, creates a napi_env via the
// private API (lib/NodeApi/NodeApiHostApi.h), dlopens a .node addon,
// negotiates its API version, calls its napi_register_module_v1 inside a
// registration handle scope, binds the result to globalThis.__exports,
// runs a .js driver, pumps promise continuations (PR-8), then destroys the
// env before disposing the runtime.
//
// Test-host-only globals (never part of the provider): `print` (stdout),
// `gc` (N5.1: runs JsCollectGarbage on the host runtime so JS drivers
// and the N5.1 common/gc shim can drive collection deterministically),
// `console`, `require` (minimal CommonJS over shims + __exports),
// `process` (env/cwd/argv only), `global` (alias), and per-driver
// `module`/`exports`/`__filename`/`__dirname`.
//
// Usage:
//   ch-nodeapi [--provider PATH] [--exports NAME] [--pump-limit N]
//              [--pump-timeout-ms N] [--addon PATH.node] [--driver PATH.js]
//              [addon.node] [driver.js]
//   Provider path: --provider, else $CHAKRA_NODEAPI_PROVIDER, else beside
//   the host binary (libChakraNodeApi, then the test-only stub). The
//   --addon/--driver spellings are the test/nodeapi/run_matrix.py
//   contract; positionals are equivalent. Driver omitted: registration
//   only (still pumps, then tears down).
//
// Exit codes: 0 = registered + driver ran + pump quiet; 1 = clean host
// error (bad args, missing symbol, version > 8, dlopen failure, JS
// exception, pump overflow). The host never crashes on addons that
// request >8 or lack registration: it prints one line to stderr and exits 1.
//
// Host ownership record (memory-management.md §3):
//   - JsRuntimeHandle / JsContextRef: host-owned, created on the host thread
//     (JsCreateRuntime / JsCreateContext), disposed after destroy_env
//     (JsSetCurrentContext(null) + JsDisposeRuntime). No provider ownership.
//   - napi_env: provider-owned (NodeEnv in provider memory); destroyed via
//     chakra_nodeapi_destroy_env on the host thread before runtime disposal.
//   - Every JsValueRef the host keeps across a call that can run JS or GC
//     (exports, script, result, promise tasks) is JsAddRef'd at store and
//     JsRelease'd before disposal (R1/R5). Promise tasks are rooted in the
//     host queue until their JsCallFunction returns.
//   - dlopen handles: provider is dlopen'd RTLD_GLOBAL|RTLD_NOW before the
//     addon (plan §1.3: addons import napi_* as undefined symbols resolved
//     from the provider). The addon is dlopen'd RTLD_LOCAL|RTLD_NOW. Both
//     are dlclose'd after env destroy + runtime disposal (addon first,
//     provider second); on macOS RTLD_GLOBAL symbols may survive dlclose,
//     so the host exits immediately after and treats the process as torn
//     down. Symbol resolution is dlsym on the provider handle for the
//     private create/destroy entry points (the host never links the
//     provider), and dlsym on the addon handle for the version + register
//     exports.

#include "NodeApiHostApi.h"
#include "node_api.h"
#include "ChakraCommon.h"
#include "ChakraCore.h"

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <dlfcn.h>
#include <map>
#include <string>
#include <unistd.h>
#include <vector>

extern char** environ;

namespace {

// ---- CLI ----

struct Config {
  std::string provider_path;
  std::string addon_path;
  std::string driver_path;
  std::string exports_name = "__exports";
  long pump_limit = 100000;
  long pump_timeout_ms = 5000;
};

bool read_file(const std::string& path, std::string* out) {
  std::FILE* f = std::fopen(path.c_str(), "rb");
  if (!f) return false;
  std::vector<char> buf(4096);
  size_t n = 0;
  while ((n = std::fread(buf.data(), 1, buf.size(), f)) > 0) out->append(buf.data(), n);
  bool ok = std::ferror(f) == 0;
  std::fclose(f);
  return ok;
}

std::string js_to_utf8(JsValueRef v) {
  JsValueRef s = JS_INVALID_REFERENCE;
  if (JsConvertValueToString(v, &s) != JsNoError) return "<string-convert failed>";
  size_t len = 0;
  JsCopyString(s, nullptr, 0, &len);
  if (len == 0) return "";
  std::string str(len + 1, '\0');
  size_t written = 0;
  if (JsCopyString(s, &str[0], str.size(), &written) != JsNoError)
    return "<string-copy failed>";
  str.resize(written);
  return str;
}

void clear_exception(const char* what) {
  bool has = false;
  if (JsHasException(&has) == JsNoError && has) {
    JsValueRef ex = JS_INVALID_REFERENCE;
    if (JsGetAndClearException(&ex) == JsNoError) {
      JsAddRef(ex, nullptr);
      std::string msg = js_to_utf8(ex);
      std::fprintf(stderr, "ch-nodeapi: %s: %s\n", what, msg.c_str());
      // Best-effort stack for triage (Node prints stacks; Chakra may
      // not populate stack strings for host-thrown errors).
      JsPropertyIdRef sid = JS_INVALID_REFERENCE;
      if (JsCreatePropertyId("stack", 5, &sid) == JsNoError) {
        JsValueRef sv = JS_INVALID_REFERENCE;
        if (JsGetProperty(ex, sid, &sv) == JsNoError &&
            sv != JS_INVALID_REFERENCE && sv != ex) {
          JsValueType t = JsUndefined;
          if (JsGetValueType(sv, &t) == JsNoError && t == JsString) {
            std::fprintf(stderr, "%s\n", js_to_utf8(sv).c_str());
          }
        }
      }
      JsRelease(ex, nullptr);
    } else {
      std::fprintf(stderr, "ch-nodeapi: %s (exception; get failed)\n", what);
    }
  } else {
    std::fprintf(stderr, "ch-nodeapi: %s\n", what);
  }
}

// ---- Host print global ----

JsValueRef host_print_cb(JsValueRef, bool, JsValueRef* args,
                         unsigned short count, void*) {
  if (count >= 2 && args) {
    std::fprintf(stdout, "%s\n", js_to_utf8(args[1]).c_str());
  } else {
    std::fprintf(stdout, "\n");
  }
  std::fflush(stdout);
  JsValueRef u = JS_INVALID_REFERENCE;
  JsGetUndefinedValue(&u);
  return u;
}

// ---- Host gc global (N5.4, test-host only) ----
//
// `globalThis.gc` runs a synchronous full GC (JsCollectGarbage).
// TEST-HOST ONLY: this hook exists so JS drivers and the N5.1 `common/gc`
// shim can drive collection deterministically. It is never part of the
// provider (libChakraNodeApi) and never part of any embedder contract.
// callbackState carries the host-owned JsRuntimeHandle (borrowed for the
// process lifetime; the host exits immediately after disposal, so no
// dangling-state window exists). Arguments are ignored (gcOptions in the
// shim is accepted but meaningless here; Node's gc() options do not map
// to JsRT). Pending exceptions are not fabricated; GC never runs user
// finalizers (D4: GC-side code only enqueues).

JsValueRef host_gc_cb(JsValueRef, bool, JsValueRef*, unsigned short,
                      void* callbackState) {
  auto runtime = static_cast<JsRuntimeHandle>(callbackState);
  if (runtime != JS_INVALID_RUNTIME_HANDLE) {
    (void)JsCollectGarbage(runtime);
  }
  JsValueRef u = JS_INVALID_REFERENCE;
  JsGetUndefinedValue(&u);
  return u;
}

// ---- Host console / process / require (test-host only, N5.1) ----
//
// Minimal CommonJS + Node-ish globals so vendored drivers run through
// ch-nodeapi without a Node runtime (offline, no npm). This is a test
// harness shim, not Node compatibility: require resolves assert/bindings
// via NODE_PATH (runner-injected shims dir), relative .js via the driver
// dir, and any *.node or */build/* request to the already-registered
// addon exports (the host dlopens one addon; Node would dlopen a fresh
// copy per require path -- same registration, same behavior here).
// Filesystem (fs), Buffer, child_process, worker_threads are NOT
// provided: drivers needing them stay skipped needs-runtime-global.

std::string host_dirname(const std::string& p) {
  size_t s = p.find_last_of('/');
  if (s == std::string::npos) return ".";
  if (s == 0) return "/";
  return p.substr(0, s);
}

std::string host_normalize(const std::string& p) {
  bool abs = (!p.empty() && p[0] == '/');
  std::vector<std::string> parts;
  size_t i = 0;
  while (i < p.size()) {
    while (i < p.size() && p[i] == '/') i++;
    size_t j = i;
    while (j < p.size() && p[j] != '/') j++;
    std::string seg = p.substr(i, j - i);
    i = j;
    if (seg.empty() || seg == ".") continue;
    if (seg == "..") {
      if (!parts.empty() && parts.back() != "..") parts.pop_back();
      else if (!abs) parts.push_back("..");
      continue;
    }
    parts.push_back(seg);
  }
  std::string out = abs ? "/" : "";
  for (size_t k = 0; k < parts.size(); k++) {
    if (k) out += "/";
    out += parts[k];
  }
  if (out.empty()) out = abs ? "/" : ".";
  return out;
}

bool host_file_exists(const std::string& p) {
  std::FILE* f = std::fopen(p.c_str(), "rb");
  if (f) { std::fclose(f); return true; }
  return false;
}

struct RequireState {
  std::string driver_dir;   /* absolute dir of the driver file */
  std::string driver_path;  /* absolute driver path */
  std::string cwd_dir;      /* getcwd at startup (== driver dir under runner) */
  JsValueRef exports = JS_INVALID_REFERENCE; /* addon exports, borrowed
                                                (rooted by global) */
  std::map<std::string, JsValueRef> cache;   /* abs path -> module.exports
                                                (host-rooted, R1) */
  JsValueRef pending_reg_error = JS_INVALID_REFERENCE; /* registration
    exception (host-rooted), thrown one-shot by the first *.node require */
  bool pending_reg_thrown = false;
};

JsValueRef console_log_cb(JsValueRef, bool, JsValueRef* args,
                          unsigned short count, void* state) {
  bool to_stderr = (state != nullptr);
  std::string line;
  if (args && count >= 2) {
    for (unsigned short k = 1; k < count; k++) {
      if (k > 1) line += " ";
      line += js_to_utf8(args[k]);
    }
  }
  std::fprintf(to_stderr ? stderr : stdout, "%s\n", line.c_str());
  std::fflush(to_stderr ? stderr : stdout);
  JsValueRef u = JS_INVALID_REFERENCE;
  JsGetUndefinedValue(&u);
  return u;
}

JsValueRef process_cwd_cb(JsValueRef, bool, JsValueRef*, unsigned short,
                          void* state) {
  const char* dir = static_cast<const char*>(state);
  JsValueRef out = JS_INVALID_REFERENCE;
  JsCreateString(dir ? dir : ".", dir ? std::strlen(dir) : 1, &out);
  return out;
}

void host_set_global(const char* name, JsValueRef val) {
  JsValueRef global = JS_INVALID_REFERENCE;
  JsGetGlobalObject(&global);
  JsPropertyIdRef pid = JS_INVALID_REFERENCE;
  if (JsCreatePropertyId(name, std::strlen(name), &pid) != JsNoError) return;
  JsSetProperty(global, pid, val, false);
}

JsValueRef host_get_global(const char* name) {
  JsValueRef global = JS_INVALID_REFERENCE;
  JsGetGlobalObject(&global);
  JsPropertyIdRef pid = JS_INVALID_REFERENCE;
  if (JsCreatePropertyId(name, std::strlen(name), &pid) != JsNoError)
    return JS_INVALID_REFERENCE;
  JsValueRef out = JS_INVALID_REFERENCE;
  JsGetProperty(global, pid, &out);
  return out;
}

// Execute one .js file as a CommonJS module and return module.exports.
// Sets __nodeapi_module__/__nodeapi_exports__ globals so the wrapper can
// reference them; saves/restores for nesting. Returns JS_INVALID_REFERENCE
// on failure (exception already printed).
JsValueRef load_commonjs_module(RequireState* rs,
                                const std::string& abs_path) {
  auto it = rs->cache.find(abs_path);
  if (it != rs->cache.end()) return it->second;
  std::string source;
  if (!read_file(abs_path, &source)) {
    std::fprintf(stderr, "ch-nodeapi: require: cannot read %s\n",
                 abs_path.c_str());
    return JS_INVALID_REFERENCE;
  }
  JsValueRef module_obj = JS_INVALID_REFERENCE;
  JsValueRef exports_obj = JS_INVALID_REFERENCE;
  if (JsCreateObject(&module_obj) != JsNoError ||
      JsCreateObject(&exports_obj) != JsNoError) {
    std::fprintf(stderr, "ch-nodeapi: require: cannot create module\n");
    return JS_INVALID_REFERENCE;
  }
  {
    JsPropertyIdRef pid = JS_INVALID_REFERENCE;
    JsCreatePropertyId("exports", 7, &pid);
    JsSetProperty(module_obj, pid, exports_obj, false);
  }
  JsValueRef prev_mod = host_get_global("__nodeapi_module__");
  JsValueRef prev_exp = host_get_global("__nodeapi_exports__");
  JsValueRef prev_fn = host_get_global("__filename__");
  JsValueRef prev_dn = host_get_global("__dirname__");
  host_set_global("__nodeapi_module__", module_obj);
  host_set_global("__nodeapi_exports__", exports_obj);
  {
    JsValueRef s = JS_INVALID_REFERENCE;
    JsCreateString(abs_path.c_str(), abs_path.size(), &s);
    host_set_global("__filename__", s);
    JsValueRef d = JS_INVALID_REFERENCE;
    std::string dn = host_dirname(abs_path);
    JsCreateString(dn.c_str(), dn.size(), &d);
    host_set_global("__dirname__", d);
  }
  std::string wrapper = ";(function(module, exports, require, __filename, __dirname){";
  wrapper += source;
  wrapper += "\n})(globalThis.__nodeapi_module__, globalThis.__nodeapi_exports__, globalThis.require, globalThis.__filename__, globalThis.__dirname__);\n";
  JsValueRef script_val = JS_INVALID_REFERENCE;
  JsValueRef url_val = JS_INVALID_REFERENCE;
  JsValueRef run_result = JS_INVALID_REFERENCE;
  JsCreateString(wrapper.c_str(), wrapper.size(), &script_val);
  JsCreateString(abs_path.c_str(), abs_path.size(), &url_val);
  JsErrorCode err = JsRun(script_val, JS_SOURCE_CONTEXT_NONE, url_val,
                          JsParseScriptAttributeNone, &run_result);
  if (script_val != JS_INVALID_REFERENCE) JsRelease(script_val, nullptr);
  if (url_val != JS_INVALID_REFERENCE) JsRelease(url_val, nullptr);
  if (run_result != JS_INVALID_REFERENCE) JsRelease(run_result, nullptr);
  if (prev_mod != JS_INVALID_REFERENCE) host_set_global("__nodeapi_module__", prev_mod);
  if (prev_exp != JS_INVALID_REFERENCE) host_set_global("__nodeapi_exports__", prev_exp);
  if (prev_fn != JS_INVALID_REFERENCE) host_set_global("__filename__", prev_fn);
  if (prev_dn != JS_INVALID_REFERENCE) host_set_global("__dirname__", prev_dn);
  if (err != JsNoError) {
    clear_exception(("require: " + abs_path + " threw").c_str());
    return JS_INVALID_REFERENCE;
  }
  JsValueRef result = JS_INVALID_REFERENCE;
  {
    JsPropertyIdRef pid = JS_INVALID_REFERENCE;
    JsCreatePropertyId("exports", 7, &pid);
    JsGetProperty(module_obj, pid, &result);
  }
  if (result == JS_INVALID_REFERENCE) result = exports_obj;
  if (JsAddRef(result, nullptr) == JsNoError) {
    rs->cache[abs_path] = result;
  }
  return result;
}

JsValueRef host_require_cb(JsValueRef, bool, JsValueRef* args,
                           unsigned short count, void* state) {
  auto* rs = static_cast<RequireState*>(state);
  JsValueRef undef = JS_INVALID_REFERENCE;
  JsGetUndefinedValue(&undef);
  if (count < 2 || !args) return undef;
  std::string req = js_to_utf8(args[1]);
  if (req.empty()) return undef;
  const char* bpath = getenv("NODEAPI_BUILD_DIR");
  const char* npath = getenv("NODE_PATH");
  // Builtin-ish shims via NODE_PATH.
  if (req == "assert" || req == "node:assert" || req == "bindings") {
    std::string shim = (req == "bindings") ? "bindings.js" : "assert.js";
    if (npath && npath[0]) {
      std::string base = npath;
      size_t semi = base.find(':');
      if (semi != std::string::npos) base = base.substr(0, semi);
      std::string cand = host_normalize(base + "/" + shim);
      JsValueRef m = load_commonjs_module(rs, cand);
      if (m != JS_INVALID_REFERENCE) return m;
      std::fprintf(stderr, "ch-nodeapi: require: cannot load shim %s\n", cand.c_str());
      return undef;
    }
    std::fprintf(stderr, "ch-nodeapi: require('%s') needs NODE_PATH (shims dir)\n", req.c_str());
    return undef;
  }
  // Any .node request or */build/* path resolves to the live addon
  // exports (host dlopened exactly one addon; Node would dlopen fresh).
  // If registration threw (vendored test_exception), the first such
  // require re-throws the stored exception (Node loader semantics:
  // the exception propagates through require() and the driver recovers
  // exports via error.binding); later requires return the exports.
  size_t dotnode = req.rfind(".node");
  bool is_node = (dotnode != std::string::npos &&
                  dotnode + 5 == req.size());
  if (is_node || req.find("/build/") != std::string::npos) {
    if (rs->pending_reg_error != JS_INVALID_REFERENCE &&
        !rs->pending_reg_thrown) {
      rs->pending_reg_thrown = true;
      (void)JsSetException(rs->pending_reg_error);
      return undef;
    }
    if (rs->exports != JS_INVALID_REFERENCE) return rs->exports;
    std::fprintf(stderr, "ch-nodeapi: require('%s'): no addon exports\n", req.c_str());
    return undef;
  }
  // Relative .js (./ ../../) or NODE_PATH file.
  std::string abs;
  if (!req.empty() && req[0] == '/') {
    abs = host_normalize(req);
  } else if (req.size() >= 2 && req[0] == '.' && req[1] == '/') {
    abs = host_normalize(rs->driver_dir + "/" + req);
  } else if (req.rfind("../", 0) == 0 || req == "..") {
    abs = host_normalize(rs->driver_dir + "/" + req);
  } else if (npath && npath[0]) {
    std::string base = npath;
    size_t semi = base.find(':');
    if (semi != std::string::npos) base = base.substr(0, semi);
    abs = host_normalize(base + "/" + req);
  } else {
    std::fprintf(stderr, "ch-nodeapi: require('%s'): unsupported (no ./ or NODE_PATH)\n", req.c_str());
    return undef;
  }
  if (host_file_exists(abs + ".js")) abs += ".js";
  else if (host_file_exists(abs)) { /* as-is */ }
  else if (host_file_exists(abs + "/index.js")) abs += "/index.js";
  else if (host_file_exists(abs + ".node")) {
    if (rs->exports != JS_INVALID_REFERENCE) return rs->exports;
    return undef;
  }
  JsValueRef m = load_commonjs_module(rs, abs);
  if (m != JS_INVALID_REFERENCE) return m;
  std::fprintf(stderr, "ch-nodeapi: require('%s'): cannot resolve (tried %s)\n", req.c_str(), abs.c_str());
  return undef;
}

// ---- Promise queue (PR-8) ----

struct PumpState {
  std::vector<JsValueRef> tasks; /* each JsAddRef'd */
};

void promise_continuation(JsValueRef task, void* state) {
  auto* pump = static_cast<PumpState*>(state);
  if (JsAddRef(task, nullptr) == JsNoError) {
    pump->tasks.push_back(task);
  }
}

// Run queued promise tasks + JsIdle until quiet. Returns 0 on quiet,
// 1 on exception / Js error / bound exceeded (message already printed).
int pump_until_quiet(PumpState* pump, long limit) {
  for (long iter = 0; iter < limit; iter++) {
    bool any = false;
    while (!pump->tasks.empty()) {
      any = true;
      if (++iter > limit) {
        std::fprintf(stderr,
                     "ch-nodeapi: pump exceeded limit %ld tasks (hang?)\n",
                     limit);
        return 1;
      }
      JsValueRef task = pump->tasks.front();
      pump->tasks.erase(pump->tasks.begin());
      JsValueRef undef = JS_INVALID_REFERENCE;
      JsGetUndefinedValue(&undef);
      JsValueRef arr[1] = {undef};
      JsValueRef result = JS_INVALID_REFERENCE;
      JsErrorCode err = JsCallFunction(task, arr, 1, &result);
      JsRelease(task, nullptr);
      if (err != JsNoError) {
        clear_exception("promise task threw");
        return 1;
      }
    }
    unsigned int next_tick = 0;
    JsErrorCode idle_err = JsIdle(&next_tick);
    // JsIdle needs EnableIdleProcessing; without it this is a no-op
    // returning max ticks. Treat any error other than NoError as quiet
    // (no idle work to do) rather than fatal, but surface real errors.
    if (idle_err == JsNoError && next_tick != static_cast<unsigned int>(-1)) {
      // Idle work is pending but promise queue is empty: one idle
      // step can queue continuations, so loop once more. Bound by limit.
      if (!any) {
        // Avoid spinning on idle alone: assume idle does not produce
        // promise work here (runtime created without EnableIdleProcessing).
        // Break after one extra cycle if no tasks appeared.
        if (pump->tasks.empty()) {
          // Single idle tick with no tasks: quiet for our purposes.
          // The loop will re-check tasks; break to avoid busy spin.
          break;
        }
      }
    } else {
      if (!any) break;
    }
    if (pump->tasks.empty() && idle_err != JsNoError) break;
  }
  // Loop exits quiet only (bound overflow returned 1 above).
  return 0;
}

// ---- Symbol types ----

typedef napi_env (*CreateEnvFn)(JsRuntimeHandle, JsContextRef, const char*,
                                const chakra_nodeapi_host_services*, void*);
typedef void (*DestroyEnvFn)(napi_env);
typedef napi_value (*RegisterFn)(napi_env, napi_value);
typedef int32_t (*GetVersionFn)(void);

} /* namespace */

int main(int argc, char** argv) {
  Config cfg;
  std::vector<std::string> positional;
  std::string flag_addon, flag_driver;
  for (int i = 1; i < argc; i++) {
    std::string a = argv[i];
    auto need = [&](const char* opt) -> std::string {
      if (i + 1 >= argc) {
        std::fprintf(stderr, "ch-nodeapi: %s requires a value\n", opt);
        std::exit(1);
      }
      return argv[++i];
    };
    if (a == "--provider") cfg.provider_path = need(a.c_str());
    else if (a == "--addon") flag_addon = need(a.c_str());
    else if (a == "--driver") flag_driver = need(a.c_str());
    else if (a == "--exports") cfg.exports_name = need(a.c_str());
    else if (a == "--pump-limit") cfg.pump_limit = std::atol(need(a.c_str()).c_str());
    else if (a == "--pump-timeout-ms")
      cfg.pump_timeout_ms = std::atol(need(a.c_str()).c_str());
    else if (a == "-h" || a == "--help") {
      std::printf("usage: ch-nodeapi [--provider PATH] [--exports NAME] [--pump-limit N] [--addon PATH.node] [--driver PATH.js] [addon.node] [driver.js]\n");
      std::printf("  provider: --provider, else $CHAKRA_NODEAPI_PROVIDER, else beside the host\n");
      return 0;
    } else if (!a.empty() && a[0] == '-') {
      std::fprintf(stderr, "ch-nodeapi: unknown option %s\n", a.c_str());
      return 1;
    } else {
      positional.push_back(a);
    }
  }
  if (cfg.provider_path.empty()) {
    const char* env = std::getenv("CHAKRA_NODEAPI_PROVIDER");
    if (env && env[0]) cfg.provider_path = env;
  }
  // Default provider: beside the host binary (same dir, then ../lib).
  // Prefers the real provider over the test-only stub.
  if (cfg.provider_path.empty()) {
    Dl_info info;
    if (dladdr(reinterpret_cast<void*>(main), &info) && info.dli_fname) {
      std::string exe = info.dli_fname;
      size_t slash = exe.find_last_of('/');
      if (slash != std::string::npos) {
        std::string dir = exe.substr(0, slash);
        static const char* cands[] = {
            "libChakraNodeApi.dylib", "libChakraNodeApi.so",
            "libChakraNodeApiStub.dylib", "libChakraNodeApiStub.so", nullptr};
        for (int k = 0; cands[k]; k++) {
          std::string p = dir + "/" + cands[k];
          std::FILE* f = std::fopen(p.c_str(), "rb");
          if (f) { std::fclose(f); cfg.provider_path = p; break; }
        }
        if (cfg.provider_path.empty()) {
          for (int k = 0; cands[k]; k++) {
            std::string p = dir + "/../lib/" + cands[k];
            std::FILE* f = std::fopen(p.c_str(), "rb");
            if (f) { std::fclose(f); cfg.provider_path = p; break; }
          }
        }
      }
    }
  }
  cfg.addon_path = flag_addon;
  cfg.driver_path = flag_driver;
  if (positional.size() >= 1 && cfg.addon_path.empty()) cfg.addon_path = positional[0];
  if (positional.size() >= 2 && cfg.driver_path.empty()) cfg.driver_path = positional[1];
  if (cfg.addon_path.empty()) {
    std::fprintf(stderr,
                 "usage: ch-nodeapi [--provider PATH] [--addon PATH.node] [--driver PATH.js] [addon.node] [driver.js]\n"
                 "  provider: --provider or $CHAKRA_NODEAPI_PROVIDER\n");
    return 1;
  }

  // --- Runtime + context (host-owned) ---
  JsRuntimeHandle runtime = JS_INVALID_RUNTIME_HANDLE;
  if (JsCreateRuntime(JsRuntimeAttributeNone, nullptr, &runtime) != JsNoError) {
    std::fprintf(stderr, "ch-nodeapi: JsCreateRuntime failed\n");
    return 1;
  }
  auto dispose_runtime = [&]() {
    JsSetCurrentContext(JS_INVALID_REFERENCE);
    JsDisposeRuntime(runtime);
  };

  JsContextRef context = JS_INVALID_REFERENCE;
  if (JsCreateContext(runtime, &context) != JsNoError) {
    std::fprintf(stderr, "ch-nodeapi: JsCreateContext failed\n");
    dispose_runtime();
    return 1;
  }
  if (JsSetCurrentContext(context) != JsNoError) {
    std::fprintf(stderr, "ch-nodeapi: JsSetCurrentContext failed\n");
    dispose_runtime();
    return 1;
  }

  // Install the host print + gc globals before anything runs JS.
  {
    JsValueRef global = JS_INVALID_REFERENCE;
    JsGetGlobalObject(&global);
    JsValueRef fn = JS_INVALID_REFERENCE;
    if (JsCreateFunction(host_print_cb, nullptr, &fn) == JsNoError) {
      JsPropertyIdRef pid = JS_INVALID_REFERENCE;
      if (JsCreatePropertyId("print", 5, &pid) == JsNoError) {
        JsSetProperty(global, pid, fn, false);
      }
    }
    // No host root held long-term: the global property roots it.
    // gc hook (N5.4, test-host only): callbackState = runtime (borrowed).
    JsValueRef gc_fn = JS_INVALID_REFERENCE;
    if (JsCreateFunction(host_gc_cb, runtime, &gc_fn) == JsNoError) {
      JsPropertyIdRef pid = JS_INVALID_REFERENCE;
      if (JsCreatePropertyId("gc", 2, &pid) == JsNoError) {
        JsSetProperty(global, pid, gc_fn, false);
      }
    }
  }

  // Host promise-pump queue (PR-8). The install site is AFTER
  // chakra_nodeapi_create_env (see below): the provider installs its
  // own continuation trampoline at env create iff it has not already
  // (N3.4 provider-side flag; host installs are undetectable through
  // the public JsRT C API, so a pre-create host install would be
  // clobbered by the provider). Host owns pumping either way
  // (plan SS3.2): after any JsRun / between registration and teardown
  // the host drains this queue until quiet.
  PumpState pump;

  // --- Load provider RTLD_GLOBAL (PR-3) ---
  if (cfg.provider_path.empty()) {
    std::fprintf(stderr,
                 "ch-nodeapi: no provider path (--provider or "
                 "$CHAKRA_NODEAPI_PROVIDER)\n");
    dispose_runtime();
    return 1;
  }
  dlerror();
  void* provider = dlopen(cfg.provider_path.c_str(),
                          RTLD_NOW | RTLD_GLOBAL);
  if (!provider) {
    std::fprintf(stderr, "ch-nodeapi: dlopen provider %s: %s\n",
                 cfg.provider_path.c_str(), dlerror());
    dispose_runtime();
    return 1;
  }
  dlerror();
  auto create_env = reinterpret_cast<CreateEnvFn>(
      dlsym(provider, "chakra_nodeapi_create_env"));
  auto destroy_env = reinterpret_cast<DestroyEnvFn>(
      dlsym(provider, "chakra_nodeapi_destroy_env"));
  if (!create_env || !destroy_env) {
    const char* e = dlerror();
    std::fprintf(stderr,
                 "ch-nodeapi: provider %s lacks private host API: %s\n",
                 cfg.provider_path.c_str(), e ? e : "(unknown)");
    dlclose(provider);
    dispose_runtime();
    return 1;
  }

  // --- Load addon RTLD_LOCAL, resolve version + register ---
  dlerror();
  void* addon = dlopen(cfg.addon_path.c_str(), RTLD_NOW | RTLD_LOCAL);
  if (!addon) {
    std::fprintf(stderr, "ch-nodeapi: dlopen addon %s: %s\n",
                 cfg.addon_path.c_str(), dlerror());
    dlclose(provider);
    dispose_runtime();
    return 1;
  }
  dlerror();
  auto get_ver = reinterpret_cast<GetVersionFn>(
      dlsym(addon, "node_api_module_get_api_version_v1"));
  int version = 8;
  if (get_ver) {
    version = get_ver();
    if (version > 8) {
      std::fprintf(stderr,
                   "ch-nodeapi: addon %s requests N-API v%d > v8 (unsupported)\n",
                   cfg.addon_path.c_str(), version);
      dlclose(addon);
      dlclose(provider);
      dispose_runtime();
      return 1;
    }
    if (version < 1) {
      std::fprintf(stderr,
                   "ch-nodeapi: addon %s reports invalid N-API v%d\n",
                   cfg.addon_path.c_str(), version);
      dlclose(addon);
      dlclose(provider);
      dispose_runtime();
      return 1;
    }
  }
  dlerror();
  auto register_fn = reinterpret_cast<RegisterFn>(
      dlsym(addon, "napi_register_module_v1"));
  if (!register_fn) {
    const char* e = dlerror();
    std::fprintf(stderr,
                 "ch-nodeapi: addon %s lacks napi_register_module_v1: %s\n",
                 cfg.addon_path.c_str(), e ? e : "(unknown)");
    dlclose(addon);
    dlclose(provider);
    dispose_runtime();
    return 1;
  }

  // --- Create env (provider-owned) ---
  chakra_nodeapi_host_services services = {};
  napi_env env = create_env(runtime, context, cfg.addon_path.c_str(),
                            &services, nullptr);
  if (!env) {
    std::fprintf(stderr, "ch-nodeapi: chakra_nodeapi_create_env failed\n");
    dlclose(addon);
    dlclose(provider);
    dispose_runtime();
    return 1;
  }

  // Install the host continuation callback AFTER env creation so the
  // host (last writer) owns the queue (N3.4/PR-8 host-clobber
  // contract: the provider installs at create; a pre-create host
  // install would be overwritten and starved). All JS runs after
  // this point, so no task is lost.
  JsSetPromiseContinuationCallback(promise_continuation, &pump);

  // --- Registration scope + exports + register call (plan §1.3/§3.5) ---
  // The scope stays open until the result is bound to the global, so the
  // returned value stays rooted across JsRun (provider R1).
  RequireState req_state;
  napi_handle_scope scope = nullptr;
  if (napi_open_handle_scope(env, &scope) != napi_ok) {
    std::fprintf(stderr, "ch-nodeapi: napi_open_handle_scope failed\n");
    destroy_env(env);
    dlclose(addon);
    dlclose(provider);
    dispose_runtime();
    return 1;
  }
  napi_value exports = nullptr;
  napi_value result = nullptr;
  bool exported_ok = true;
  if (napi_create_object(env, &exports) != napi_ok || !exports) {
    std::fprintf(stderr, "ch-nodeapi: napi_create_object failed\n");
    exported_ok = false;
  }
  if (exported_ok) {
    // Host root on the exports JS value across the register call.
    JsAddRef(reinterpret_cast<JsValueRef>(exports), nullptr);
    result = register_fn(env, exports);
    // Return-replaces-exports: non-null and != passed object replaces.
    bool replaced = (result != nullptr && result != exports);
    if (result == nullptr) {
      // NULL return: keep original exports (some addons return null).
      bool has = false;
      if (JsHasException(&has) == JsNoError && has) {
        // Registration threw with no return (vendored test_exception
        // shape: Init throws; the error carries error.binding). Store
        // the exception for the driver's require() to re-throw (see
        // host_require_cb); keep the original exports as the binding.
        JsValueRef ex = JS_INVALID_REFERENCE;
        if (JsGetAndClearException(&ex) == JsNoError &&
            ex != JS_INVALID_REFERENCE) {
          JsAddRef(ex, nullptr);
          req_state.pending_reg_error = ex;
        }
        result = exports;
        replaced = false;
      } else {
        result = exports;
        replaced = false;
      }
    }
    if (replaced) {
      // Keep the replacement rooted by the host across binding.
      JsAddRef(reinterpret_cast<JsValueRef>(result), nullptr);
    }
    napi_value final_exports = result;
    // Bind to globalThis.<exports_name>.
    JsValueRef global = JS_INVALID_REFERENCE;
    JsGetGlobalObject(&global);
    JsPropertyIdRef pid = JS_INVALID_REFERENCE;
    JsCreatePropertyId(cfg.exports_name.c_str(), cfg.exports_name.size(), &pid);
    JsSetProperty(global, pid,
                  reinterpret_cast<JsValueRef>(final_exports), false);
    if (replaced) {
      JsRelease(reinterpret_cast<JsValueRef>(exports), nullptr);
      JsRelease(reinterpret_cast<JsValueRef>(final_exports), nullptr);
    } else {
      JsRelease(reinterpret_cast<JsValueRef>(exports), nullptr);
    }
    // Abort the scope rooting the original exports if replaced; the bound
    // replacement is held by the global.
    napi_close_handle_scope(env, scope);
  } else {
    napi_close_handle_scope(env, scope);
  }
  if (!exported_ok) {
    destroy_env(env);
    dlclose(addon);
    dlclose(provider);
    dispose_runtime();
    return 1;
  }
  // Registration scope is closed; the global now roots the exports.
  // A pending exception from registration (vendored test_exception:
  // Init throws "Error during Init" and attaches error.binding) is
  // stored for the driver's require() to re-throw, rather than exiting
  // here (JsRun would fail with InExceptionState anyway).
  {
    bool has = false;
    if (JsHasException(&has) == JsNoError && has) {
      JsValueRef ex = JS_INVALID_REFERENCE;
      if (JsGetAndClearException(&ex) == JsNoError &&
          ex != JS_INVALID_REFERENCE &&
          req_state.pending_reg_error == JS_INVALID_REFERENCE) {
        JsAddRef(ex, nullptr);
        req_state.pending_reg_error = ex;
      } else {
        clear_exception("pending exception after registration");
        if (ex != JS_INVALID_REFERENCE) JsRelease(ex, nullptr);
      }
    }
  }

  // --- Run driver (if given) + pump ---
  int run_rc = 0;
  if (!cfg.driver_path.empty()) {
    // Install test-host globals used by vendored drivers (console,
    // process, require, global alias). require returns the live addon
    // exports for *.node / */build/* paths (see host_require_cb).
    req_state.driver_dir = host_dirname(cfg.driver_path);
    req_state.driver_path = cfg.driver_path;
    {
      char cwdbuf[4096];
      if (getcwd(cwdbuf, sizeof(cwdbuf))) req_state.cwd_dir = cwdbuf;
      else req_state.cwd_dir = req_state.driver_dir;
    }
    req_state.exports = host_get_global(cfg.exports_name.c_str());
    // console object: log/info/warn/debug -> stdout, error -> stderr.
    {
      JsValueRef console_obj = JS_INVALID_REFERENCE;
      JsCreateObject(&console_obj);
      const char* names[5] = {"log", "info", "warn", "debug", "error"};
      for (int k = 0; k < 5; k++) {
        JsValueRef fn = JS_INVALID_REFERENCE;
        JsCreateFunction(console_log_cb, (k == 4) ? (void*)1 : nullptr, &fn);
        JsPropertyIdRef pid = JS_INVALID_REFERENCE;
        JsCreatePropertyId(names[k], std::strlen(names[k]), &pid);
        JsSetProperty(console_obj, pid, fn, false);
      }
      host_set_global("console", console_obj);
    }
    // global alias (Node: global === globalThis).
    {
      JsValueRef global = JS_INVALID_REFERENCE;
      JsGetGlobalObject(&global);
      host_set_global("global", global);
    }
    // process: env (from OS environ) + cwd() + argv. No `on`/`exit`:
    // mustCall verification is done explicitly by the host below.
    {
      JsValueRef proc = JS_INVALID_REFERENCE;
      JsCreateObject(&proc);
      JsValueRef env_obj = JS_INVALID_REFERENCE;
      JsCreateObject(&env_obj);
      if (environ) {
        for (char** e = environ; *e; e++) {
          std::string s(*e);
          size_t eq = s.find('=');
          if (eq == std::string::npos) continue;
          std::string k = s.substr(0, eq), v = s.substr(eq + 1);
          JsValueRef vv = JS_INVALID_REFERENCE;
          JsCreateString(v.c_str(), v.size(), &vv);
          JsPropertyIdRef pid = JS_INVALID_REFERENCE;
          JsCreatePropertyId(k.c_str(), k.size(), &pid);
          JsSetProperty(env_obj, pid, vv, false);
        }
      }
      {
        JsPropertyIdRef pid = JS_INVALID_REFERENCE;
        JsCreatePropertyId("env", 3, &pid);
        JsSetProperty(proc, pid, env_obj, false);
      }
      {
        JsValueRef fn = JS_INVALID_REFERENCE;
        JsCreateFunction(process_cwd_cb, const_cast<char*>(req_state.cwd_dir.c_str()), &fn);
        JsPropertyIdRef pid = JS_INVALID_REFERENCE;
        JsCreatePropertyId("cwd", 3, &pid);
        JsSetProperty(proc, pid, fn, false);
      }
      {
        JsValueRef arr = JS_INVALID_REFERENCE;
        JsCreateArray(2, &arr);
        JsValueRef a0 = JS_INVALID_REFERENCE, a1 = JS_INVALID_REFERENCE;
        const char* argv0 = "ch-nodeapi";
        JsCreateString(argv0, std::strlen(argv0), &a0);
        JsCreateString(cfg.driver_path.c_str(), cfg.driver_path.size(), &a1);
        JsPropertyIdRef pid0 = JS_INVALID_REFERENCE;
        JsPropertyIdRef pid1 = JS_INVALID_REFERENCE;
        JsCreatePropertyId("0", 1, &pid0);
        JsCreatePropertyId("1", 1, &pid1);
        JsSetProperty(arr, pid0, a0, false);
        JsSetProperty(arr, pid1, a1, false);
        JsPropertyIdRef pid = JS_INVALID_REFERENCE;
        JsCreatePropertyId("argv", 4, &pid);
        JsSetProperty(proc, pid, arr, false);
      }
      {
        JsPropertyIdRef pid = JS_INVALID_REFERENCE;
        JsCreatePropertyId("platform", 8, &pid);
        JsValueRef pl = JS_INVALID_REFERENCE;
#ifdef __linux__
        const char* pl_s = "linux";
#else
        const char* pl_s = "darwin";
#endif
        JsCreateString(pl_s, std::strlen(pl_s), &pl);
        JsSetProperty(proc, pid, pl, false);
      }
      host_set_global("process", proc);
    }
    // require: minimal CommonJS (see host_require_cb). callbackState is
    // the stack RequireState (borrowed; host exits after disposal).
    {
      JsValueRef fn = JS_INVALID_REFERENCE;
      JsCreateFunction(host_require_cb, &req_state, &fn);
      host_set_global("require", fn);
    }
    std::string script;
    if (!read_file(cfg.driver_path, &script)) {
      std::fprintf(stderr, "ch-nodeapi: cannot read driver %s\n",
                   cfg.driver_path.c_str());
      destroy_env(env);
      dlclose(addon);
      dlclose(provider);
      dispose_runtime();
      return 1;
    }
    // Run as a CommonJS module (module/exports/__filename/__dirname in
    // scope) so top-level `return` and module patterns work.
    JsValueRef driver_module = JS_INVALID_REFERENCE;
    JsValueRef driver_exports = JS_INVALID_REFERENCE;
    JsCreateObject(&driver_module);
    JsCreateObject(&driver_exports);
    {
      JsPropertyIdRef pid = JS_INVALID_REFERENCE;
      JsCreatePropertyId("exports", 7, &pid);
      JsSetProperty(driver_module, pid, driver_exports, false);
    }
    host_set_global("module", driver_module);
    host_set_global("exports", driver_exports);
    {
      JsValueRef s = JS_INVALID_REFERENCE;
      JsCreateString(cfg.driver_path.c_str(), cfg.driver_path.size(), &s);
      host_set_global("__filename__", s);
      JsValueRef d = JS_INVALID_REFERENCE;
      JsCreateString(req_state.driver_dir.c_str(), req_state.driver_dir.size(), &d);
      host_set_global("__dirname__", d);
    }
    std::string wrapped = ";(function(module, exports, require, __filename, __dirname){";
    wrapped += script;
    wrapped += "\n})(globalThis.module, globalThis.exports, globalThis.require, globalThis.__filename__, globalThis.__dirname__);\n";
    JsValueRef script_val = JS_INVALID_REFERENCE;
    JsValueRef url_val = JS_INVALID_REFERENCE;
    JsValueRef run_result = JS_INVALID_REFERENCE;
    if (JsCreateString(wrapped.c_str(), wrapped.size(), &script_val) != JsNoError ||
        JsCreateString(cfg.driver_path.c_str(), cfg.driver_path.size(), &url_val) != JsNoError) {
      std::fprintf(stderr, "ch-nodeapi: JsCreateString (driver) failed\n");
      destroy_env(env);
      dlclose(addon);
      dlclose(provider);
      dispose_runtime();
      return 1;
    }
    JsErrorCode err =
        JsRun(script_val, JS_SOURCE_CONTEXT_NONE, url_val,
              JsParseScriptAttributeNone, &run_result);
    if (err != JsNoError) {
      bool has = false;
      if (JsHasException(&has) == JsNoError && has) {
        clear_exception("driver threw");
      } else {
        std::fprintf(stderr, "ch-nodeapi: JsRun failed (%d)\n", (int)err);
      }
      run_rc = 1;
    }
    // Host pump (PR-8): drain continuations until quiet, even after a
    // top-level JsRun error, so teardown starts with a quiet queue.
    if (run_rc == 0) {
      // The pump itself reports and returns 1 on overflow/exception.
      run_rc = pump_until_quiet(&pump, cfg.pump_limit);
    } else {
      // Still drain to release roots before disposal.
      pump_until_quiet(&pump, cfg.pump_limit);
    }
    // mustCall verification: the test/common shim records unfulfilled
    // calls; Node fails at process exit. The host replicates that gate
    // here (no process/exit events on this host).
    if (run_rc == 0) {
      // Drain GC-enqueued finalizers before checking mustCall: the
      // driver may have just run global.gc() with no later napi call.
      // One JS-calling provider entry triggers the EntryGuard drain.
      {
        napi_handle_scope drain_scope = nullptr;
        napi_value tmp = nullptr;
        if (napi_open_handle_scope(env, &drain_scope) == napi_ok &&
            napi_create_object(env, &tmp) == napi_ok) {
          (void)tmp;
        }
        if (drain_scope) napi_close_handle_scope(env, drain_scope);
        bool has = false;
        if (JsHasException(&has) == JsNoError && has) {
          clear_exception("drain threw");
          run_rc = 1;
        }
      }
      // Finalizers may have queued promise continuations; pump again.
      if (run_rc == 0) {
        run_rc = pump_until_quiet(&pump, cfg.pump_limit);
      }
      if (run_rc == 0) {
        for (auto& kv : req_state.cache) {
        JsValueRef c = JS_INVALID_REFERENCE;
        {
          JsPropertyIdRef pid = JS_INVALID_REFERENCE;
          JsCreatePropertyId("_checkMustCall", 14, &pid);
          JsGetProperty(kv.second, pid, &c);
        }
        JsValueType ty = JsUndefined;
        if (c == JS_INVALID_REFERENCE ||
            JsGetValueType(c, &ty) != JsNoError || ty != JsFunction) {
          continue;
        }
        JsValueRef undef = JS_INVALID_REFERENCE;
        JsGetUndefinedValue(&undef);
        JsValueRef arr[1] = {undef};
        JsValueRef out = JS_INVALID_REFERENCE;
        JsErrorCode ce = JsCallFunction(c, arr, 1, &out);
        if (ce != JsNoError) {
          clear_exception("mustCall check failed");
          run_rc = 1;
        }
        break;
        }
      }
    }
    // Release host roots on temporaries before teardown (R5).
    if (script_val != JS_INVALID_REFERENCE) JsRelease(script_val, nullptr);
    if (url_val != JS_INVALID_REFERENCE) JsRelease(url_val, nullptr);
    if (run_result != JS_INVALID_REFERENCE) JsRelease(run_result, nullptr);
    for (auto& kv : req_state.cache) {
      if (kv.second != JS_INVALID_REFERENCE) JsRelease(kv.second, nullptr);
    }
    req_state.cache.clear();
    if (req_state.pending_reg_error != JS_INVALID_REFERENCE) {
      JsRelease(req_state.pending_reg_error, nullptr);
      req_state.pending_reg_error = JS_INVALID_REFERENCE;
    }
  } else {
    // Registration-only: still pump once so queued continuations from the
    // register call run before teardown.
    run_rc = pump_until_quiet(&pump, cfg.pump_limit);
    if (req_state.pending_reg_error != JS_INVALID_REFERENCE) {
      std::fprintf(stderr, "ch-nodeapi: registration threw with no driver\n");
      JsRelease(req_state.pending_reg_error, nullptr);
      run_rc = 1;
    }
  }

  // --- Teardown: destroy env BEFORE disposing runtime (§3.6) ---
  // The host's promise queue must be empty here (pumped to quiet); any
  // leftovers are released so no root survives disposal (R5).
  for (JsValueRef t : pump.tasks) JsRelease(t, nullptr);
  pump.tasks.clear();

  destroy_env(env);
  // dlclose order: addon first (depends on provider), provider second,
  // after the runtime is gone so no JS value references unloaded code.
  // The host exits immediately after, so macOS RTLD_GLOBAL unload caveats
  // (symbols may persist) are not observable.
  dispose_runtime();
  dlclose(addon);
  dlclose(provider);
  return run_rc;
}
