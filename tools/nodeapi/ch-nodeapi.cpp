//-------------------------------------------------------------------------------------------------------
// Copyright (c) ChakraCore Project Contributors. All rights reserved.
// Licensed under the MIT license. See LICENSE.txt file in the project root for full license information.
//-------------------------------------------------------------------------------------------------------
//
// Test host for the Node-API provider (plan doc/node-api-plan.md N0.3 / Q1.1).
//
// Loads a provider shared library RTLD_GLOBAL, creates isolated per-addon
// napi_env handles via the private API (lib/NodeApi/NodeApiHostApi.h),
// dlopens one or more .node addons (CLI --addon preloads and/or dynamic
// CommonJS require('./foo.node')), negotiates each addon's API version
// (v1..v10 via chakra_nodeapi_set_module_api_version when exported), runs
// napi_register_module_v1 (or deprecated napi_module_register pending
// entries captured around dlopen) inside a registration handle scope, binds
// the first CLI addon's exports to globalThis.__exports, runs an optional .js
// driver, pumps promise continuations (PR-8), drains finalizers on all
// loaded envs, then destroys envs in reverse LIFO order before disposing the
// runtime.
//
// Test-host-only globals (never part of the provider): `print` (stdout),
// `gc` (N5.1: runs JsCollectGarbage on the host runtime so JS drivers
// and the N5.1 common/gc shim can drive collection deterministically),
// `console`, `require` (CommonJS over shims + dynamic .node loader),
// `process` (env/cwd/argv/platform only), `global` (alias), and per-driver
// `module`/`exports`/`__filename`/`__dirname`.
//
// Usage:
//   ch-nodeapi [--provider PATH] [--exports NAME] [--pump-limit N]
//              [--pump-timeout-ms N] [--addon PATH.node]... [--driver PATH.js]
//              [addon.node] [driver.js]
//   Provider path: --provider, else $CHAKRA_NODEAPI_PROVIDER, else beside
//   the host binary (libChakraNodeApi, then the test-only stub). --addon is
//   repeatable; --driver alone (zero --addon flags) is valid for drivers that
//   dynamically require('./foo.node'). Driver omitted: registration only
//   (still pumps, then tears down).
//
// Exit codes: 0 = registered + driver ran + pump quiet; 1 = clean host
// error (bad args, missing symbol, version > 10 or < 1, dlopen failure, JS
// exception, pump overflow). The host never crashes on addons that
// request >10 or lack registration: it prints one line to stderr (or throws
// a JS Error inside require()) and exits 1.
//
// Host ownership record (doc/memory-management.md section 3):
//   - JsRuntimeHandle / JsContextRef: host-owned, created on the host thread
//     (JsCreateRuntime / JsCreateContext), disposed after all destroy_env
//     calls (JsSetCurrentContext(null) + JsDisposeRuntime).
//   - AddonRecord / AddonRegistry:
//     * Resource / creation call: AddonRecord heap objects owned via
//       std::vector<std::unique_ptr<AddonRecord>> addon_order and indexed by
//       canonical path in std::map<std::string, AddonRecord*> registry_by_path
//       in tools/nodeapi/ch-nodeapi.cpp (load_and_register_addon).
//     * Domain and allocator: host C++ heap (new (std::nothrow) AddonRecord
//       + std::string/vector/map using default host C++ allocator; backing
//       lifetime == host process). Never Recycler memory.
//     * Physical allocation & release authority: one AddonRecord per unique
//       canonical .node path; holds dl_handle (dlopen RTLD_NOW|RTLD_LOCAL),
//       provider-owned napi_env (create_env / destroy_env), and host-rooted
//       JsValueRef fields (exports, pending_reg_error). Sole release authority
//       is shutdown_all() in main().
//     * Logical views / aliases: registry_by_path values and addon_order[0]
//       borrow AddonRecord* from addon_order; std::unique_ptr guarantees
//       pointer stability across vector growth (including reentrant require()).
//       RequireState::registry borrows the stack-allocated AddonRegistry*.
//     * Borrow invalidation: AddonRecord* stays valid from publication until
//       shutdown_all() clears registry_by_path/addon_order after runtime
//       disposal. Reentrant require() inside an addon's Init cannot
//       invalidate in-flight AddonRecord* pointers.
//     * Reentrancy / user code: dlopen static constructors, Init
//       (napi_register_module_v1 / nm_register_func), drain scopes, and JS
//       require() can invoke user JS, trigger GC, or reenter
//       load_and_register_addon. Records are published to addon_order +
//       registry_by_path only after registration completes, and drain loops
//       index addon_order by integer index rather than holding iterators.
//     * Thread/context allowed: main/host thread only with host context
//       current for creation, JS root changes, drains, and destroy_env.
//     * Release on success, partial failure, exception, OOM, shutdown:
//       - Partial failure before publication: local cur_js /
//         pending_reg_error roots released (if acquired), registration scope
//         closed, env destroyed via destroy_env, and dl_handle closed via
//         dlclose unless a static constructor registered a deprecated module
//         via napi_module_register (in which case dl_handle is parked in
//         orphan_dl_handles until shutdown_all so provider pending-module
//         code pointers remain valid across later envs).
//       - Registration JS exception: captured via JsGetAndClearException,
//         JsAddRef'd into rec->pending_reg_error, and re-thrown once on first
//         JS require() (or reported as error in registration-only mode).
//       - Shutdown (shutdown_all): releases req_state.cache, pump.tasks, and
//         every AddonRecord's exports + pending_reg_error roots via JsRelease;
//         destroys every rec->env in reverse addon_order (LIFO); shuts down
//         host_loop; clears promise callback; releases any late pump.tasks
//         queued during teardown; disposes runtime; and when joined == true,
//         deletes host_loop, dlcloses every rec->dl_handle in reverse order,
//         dlcloses orphan_dl_handles, and dlcloses provider. If joined == false
//         (timeout/error with uncancellable worker running), dl_handles and
//         host_loop stay alive until immediate process exit.
//     * Memory accounting & bound: bounded by distinct .node files loaded in
//       the process; OOM during AddonRecord/container push rolls back all
//       roots, env, and dl_handle cleanly without publishing partial state.
//   - dlopen handles: provider is dlopen'd RTLD_GLOBAL|RTLD_NOW before any
//     addon (plan section 1.3: addons import napi_* as undefined symbols
//     resolved from the provider). Addons are dlopen'd RTLD_LOCAL|RTLD_NOW.

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

#include "NodeApiHostApi.h"
#include "node_api.h"
#include "ChakraCommon.h"
#include "ChakraCore.h"

#include <cctype>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <map>
#include <memory>
#include <mutex>
#include <new>
#include <string>
#include <thread>
#include <vector>
#ifndef _WIN32
#include <dlfcn.h>
#include <sys/stat.h>
#include <unistd.h>

extern char** environ;
#endif

namespace {

// ---- CLI ----

struct Config {
  std::string provider_path;
  std::vector<std::string> addon_paths;
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
      const bool ex_rooted = (JsAddRef(ex, nullptr) == JsNoError);
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
      if (ex_rooted) JsRelease(ex, nullptr);
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

// ---- Host console / process / require / multi-addon registry ----
//
// Minimal CommonJS + Node-ish globals so vendored drivers run through
// ch-nodeapi without a Node runtime (offline, no npm). Require resolves
// assert/bindings via NODE_PATH (runner-injected shims dir), relative .js
// via active_module_dir, concrete .node files via load_and_register_addon
// (cached by canonical path), and falls back to addon_order[0] for
// synthetic *.node or */build/* requests when no concrete file matches.
// Filesystem (fs), Buffer, child_process, worker_threads are NOT
// provided: drivers needing them stay skipped needs-runtime-global.

#ifdef _WIN32
bool host_is_sep(char c) { return c == '/' || c == '\\'; }

bool host_has_drive_prefix(const std::string& p) {
  return p.size() >= 2 &&
         std::isalpha(static_cast<unsigned char>(p[0])) != 0 &&
         p[1] == ':';
}

bool host_is_abs(const std::string& p) {
  if (p.empty()) return false;
  if (host_is_sep(p[0])) return true;
  return host_has_drive_prefix(p) && p.size() >= 3 && host_is_sep(p[2]);
}

std::string host_dirname(const std::string& p) {
  size_t s = p.find_last_of("/\\");
  if (s == std::string::npos) return ".";
  if (s == 0) {
    if (p.size() >= 2 && host_is_sep(p[1])) return p.substr(0, 2);
    return p.substr(0, 1);
  }
  if (s == 2 && host_has_drive_prefix(p)) return p.substr(0, 3);
  return p.substr(0, s);
}

std::string host_basename(const std::string& p) {
  size_t s = p.find_last_of("/\\");
  if (s == std::string::npos) return p;
  return p.substr(s + 1);
}

bool host_ends_with(const std::string& s, const char* suf) {
  size_t slen = std::strlen(suf);
  return s.size() >= slen && s.compare(s.size() - slen, slen, suf) == 0;
}

std::string host_normalize(const std::string& p) {
  std::string prefix;
  bool abs = false;
  size_t i = 0;
  if (p.size() >= 2 && host_is_sep(p[0]) && host_is_sep(p[1])) {
    prefix = "//";
    abs = true;
    i = 2;
  } else if (host_has_drive_prefix(p)) {
    if (p.size() >= 3 && host_is_sep(p[2])) {
      prefix = std::string(1, p[0]) + ":/";
      abs = true;
      i = 3;
    } else {
      prefix = p.substr(0, 2);
      i = 2;
    }
  } else if (!p.empty() && host_is_sep(p[0])) {
    prefix = "/";
    abs = true;
    i = 1;
  }
  std::vector<std::string> parts;
  while (i < p.size()) {
    while (i < p.size() && host_is_sep(p[i])) i++;
    size_t j = i;
    while (j < p.size() && !host_is_sep(p[j])) j++;
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
  std::string out = prefix;
  for (size_t k = 0; k < parts.size(); k++) {
    if (k) out += "/";
    out += parts[k];
  }
  if (out.empty()) out = abs ? "/" : ".";
  return out;
}

std::string host_resolve_abs(const std::string& base_dir,
                             const std::string& raw) {
  if (host_is_abs(raw)) return host_normalize(raw);
  return host_normalize(base_dir + "/" + raw);
}

std::string host_canonicalize(const std::string& abs_path) {
  char* fp = ::_fullpath(nullptr, abs_path.c_str(), 0);
  if (fp != nullptr) {
    std::string out(fp);
    std::free(fp);
    return out;
  }
  return host_normalize(abs_path);
}

bool host_file_exists(const std::string& p) {
  DWORD attr = GetFileAttributesA(p.c_str());
  return (attr != INVALID_FILE_ATTRIBUTES) &&
         ((attr & FILE_ATTRIBUTE_DIRECTORY) == 0);
}

std::string host_win32_error(DWORD err) {
  if (err == 0) return "(unknown)";
  char* msg_buf = nullptr;
  DWORD len = FormatMessageA(
      FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM |
          FORMAT_MESSAGE_IGNORE_INSERTS,
      nullptr, err, MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT),
      reinterpret_cast<LPSTR>(&msg_buf), 0, nullptr);
  std::string out;
  if (len > 0 && msg_buf != nullptr) {
    out.assign(msg_buf, len);
    while (!out.empty() && (out.back() == '\n' || out.back() == '\r')) {
      out.pop_back();
    }
  } else {
    char buf[64];
    std::snprintf(buf, sizeof(buf), "Win32 error %lu",
                  static_cast<unsigned long>(err));
    out = buf;
  }
  if (msg_buf != nullptr) LocalFree(msg_buf);
  return out;
}

void* host_dlopen(const std::string& path, bool global, std::string* err_out) {
  (void)global;
  std::string win_path = path;
  for (char& ch : win_path) {
    if (ch == '/') ch = '\\';
  }
  HMODULE h = nullptr;
  if (win_path.find('\\') != std::string::npos) {
    char* full = ::_fullpath(nullptr, win_path.c_str(), 0);
    if (full != nullptr) {
      h = LoadLibraryExA(full, nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
      std::free(full);
    } else {
      h = LoadLibraryExA(win_path.c_str(), nullptr,
                         LOAD_WITH_ALTERED_SEARCH_PATH);
    }
  } else {
    h = LoadLibraryA(win_path.c_str());
  }
  if (!h && err_out) {
    *err_out = host_win32_error(GetLastError());
  }
  return reinterpret_cast<void*>(h);
}

void* host_dlsym(void* handle, const char* sym, std::string* err_out) {
  if (!handle) {
    if (err_out) *err_out = "null module handle";
    return nullptr;
  }
  FARPROC p = GetProcAddress(reinterpret_cast<HMODULE>(handle), sym);
  if (!p && err_out) {
    *err_out = host_win32_error(GetLastError());
  }
  return reinterpret_cast<void*>(p);
}

void host_dlclose(void* handle) {
  if (handle) {
    FreeLibrary(reinterpret_cast<HMODULE>(handle));
  }
}
#else
std::string host_dirname(const std::string& p) {
  size_t s = p.find_last_of('/');
  if (s == std::string::npos) return ".";
  if (s == 0) return "/";
  return p.substr(0, s);
}

std::string host_basename(const std::string& p) {
  size_t s = p.find_last_of('/');
  if (s == std::string::npos) return p;
  return p.substr(s + 1);
}

bool host_ends_with(const std::string& s, const char* suf) {
  size_t slen = std::strlen(suf);
  return s.size() >= slen && s.compare(s.size() - slen, slen, suf) == 0;
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

std::string host_resolve_abs(const std::string& base_dir,
                             const std::string& raw) {
  if (!raw.empty() && raw[0] == '/') return host_normalize(raw);
  return host_normalize(base_dir + "/" + raw);
}

std::string host_canonicalize(const std::string& abs_path) {
  char* rp = ::realpath(abs_path.c_str(), nullptr);
  if (rp != nullptr) {
    std::string out(rp);
    std::free(rp);
    return out;
  }
  return host_normalize(abs_path);
}

bool host_file_exists(const std::string& p) {
  struct stat st;
  if (::stat(p.c_str(), &st) != 0) return false;
  return S_ISREG(st.st_mode);
}

void* host_dlopen(const std::string& path, bool global, std::string* err_out) {
  dlerror();
  void* h = dlopen(path.c_str(), RTLD_NOW | (global ? RTLD_GLOBAL : RTLD_LOCAL));
  if (!h && err_out) {
    const char* e = dlerror();
    *err_out = e ? e : "(unknown)";
  }
  return h;
}

void* host_dlsym(void* handle, const char* sym, std::string* err_out) {
  dlerror();
  void* p = dlsym(handle, sym);
  if (!p && err_out) {
    const char* e = dlerror();
    *err_out = e ? e : "(unknown)";
  }
  return p;
}

void host_dlclose(void* handle) {
  if (handle) {
    dlclose(handle);
  }
}
#endif

struct chakra_nodeapi_pending_module_info_compat {
  int nm_version;
  unsigned int nm_flags;
  const char* nm_filename;
  napi_addon_register_func nm_register_func;
  const char* nm_modname;
  void* nm_priv;
};

typedef napi_env (*CreateEnvFn)(JsRuntimeHandle, JsContextRef, const char*,
                                const chakra_nodeapi_host_services*, void*);
typedef void (*DestroyEnvFn)(napi_env);
typedef napi_status (*SetModuleApiVersionFn)(napi_env, int32_t);
typedef size_t (*PendingModuleCountFn)(void);
typedef napi_status (*GetPendingModuleFn)(
    size_t, chakra_nodeapi_pending_module_info_compat*);
typedef napi_value (NAPI_CDECL *RegisterFn)(napi_env, napi_value);
typedef int32_t (NAPI_CDECL *GetVersionFn)(void);
typedef napi_status (NAPI_CDECL *OpenHandleScopeFn)(napi_env,
                                                    napi_handle_scope*);
typedef napi_status (NAPI_CDECL *CloseHandleScopeFn)(napi_env,
                                                     napi_handle_scope);
typedef napi_status (NAPI_CDECL *CreateObjectFn)(napi_env, napi_value*);

struct HostLoopState;

struct AddonRecord {
  std::string canonical_path;
  void* dl_handle = nullptr;
  napi_env env = nullptr;
  int32_t api_version = 8;
  JsValueRef exports = JS_INVALID_REFERENCE;           /* JsAddRef'd until shutdown */
  JsValueRef pending_reg_error = JS_INVALID_REFERENCE; /* JsAddRef'd if Init threw */
  bool pending_reg_thrown = false;
  bool used_deprecated_register = false;
};

struct AddonRegistry {
  std::string cwd_dir;
  JsRuntimeHandle runtime = JS_INVALID_RUNTIME_HANDLE;
  JsContextRef context = JS_INVALID_REFERENCE;
  chakra_nodeapi_host_services services{};
  HostLoopState* host_loop = nullptr;

  CreateEnvFn create_env = nullptr;
  DestroyEnvFn destroy_env = nullptr;
  SetModuleApiVersionFn set_ver_fn = nullptr;
  PendingModuleCountFn pending_count_fn = nullptr;
  GetPendingModuleFn get_pending_fn = nullptr;
#ifdef _WIN32
  OpenHandleScopeFn open_scope_fn = nullptr;
  CloseHandleScopeFn close_scope_fn = nullptr;
  CreateObjectFn create_object_fn = nullptr;
#endif

  std::vector<std::unique_ptr<AddonRecord>> addon_order;
  std::map<std::string, AddonRecord*> registry_by_path;
  std::vector<void*> orphan_dl_handles;
  bool has_cli_addon = false;
};

struct RequireState {
  std::string driver_dir;        /* absolute dir of the driver file */
  std::string driver_path;       /* absolute driver path */
  std::string cwd_dir;           /* getcwd at startup */
  std::string active_module_dir; /* dir of currently loading JS module */
  std::map<std::string, JsValueRef> cache; /* abs path -> module.exports
                                              (host-rooted, R1) */
  AddonRegistry* registry = nullptr;       /* borrowed from main() */
};

void host_throw_error(const std::string& msg) {
  JsValueRef msg_val = JS_INVALID_REFERENCE;
  JsValueRef err_val = JS_INVALID_REFERENCE;
  if (JsCreateString(msg.c_str(), msg.size(), &msg_val) == JsNoError &&
      JsCreateError(msg_val, &err_val) == JsNoError) {
    (void)JsSetException(err_val);
  }
}

bool load_and_register_addon(AddonRegistry* reg, const std::string& raw_path,
                             AddonRecord** out, std::string* err_out) {
  if (out) *out = nullptr;
  if (!reg || raw_path.empty()) {
    if (err_out) *err_out = "invalid addon load request";
    return false;
  }

  std::string resolved = host_resolve_abs(reg->cwd_dir, raw_path);
  std::string canon = host_canonicalize(resolved);

  auto existing = reg->registry_by_path.find(canon);
  if (existing != reg->registry_by_path.end()) {
    if (out) *out = existing->second;
    return true;
  }

  size_t pending_before = reg->pending_count_fn ? reg->pending_count_fn() : 0;
  std::string dl_err;
  void* handle = host_dlopen(canon, false, &dl_err);
  if (!handle) {
    if (err_out) {
      *err_out = "dlopen addon " + raw_path + ": " + dl_err;
    }
    return false;
  }
  size_t pending_after = reg->pending_count_fn ? reg->pending_count_fn() : 0;
  bool has_deprecated =
      (pending_after > pending_before && reg->get_pending_fn != nullptr);

  auto retire_handle = [&]() {
    // If deprecated napi_module_register ran at dlopen, do not dlclose early
    // if the process may still create new envs; shutdown_all closes all
    // handles after all envs are destroyed.
    if (!has_deprecated) {
      host_dlclose(handle);
    } else {
      try {
        reg->orphan_dl_handles.push_back(handle);
      } catch (...) {
      }
    }
  };

  auto get_ver = reinterpret_cast<GetVersionFn>(
      host_dlsym(handle, "node_api_module_get_api_version_v1", nullptr));
  int32_t version = 8;
  if (get_ver) {
    version = get_ver();
    if (version > 10) {
      if (err_out) {
        char buf[64];
        std::snprintf(buf, sizeof(buf), "%d", static_cast<int>(version));
        *err_out = "addon " + raw_path + " requests N-API v" + buf +
                   " > v10 (unsupported)";
      }
      retire_handle();
      return false;
    }
    if (version < 1) {
      if (err_out) {
        char buf[64];
        std::snprintf(buf, sizeof(buf), "%d", static_cast<int>(version));
        *err_out = "addon " + raw_path + " reports invalid N-API v" + buf;
      }
      retire_handle();
      return false;
    }
  }

  std::string sym_err;
  auto register_fn = reinterpret_cast<RegisterFn>(
      host_dlsym(handle, "napi_register_module_v1", &sym_err));
  if (!register_fn && !has_deprecated) {
    if (err_out) {
      *err_out = "addon " + raw_path + " lacks napi_register_module_v1: " +
                 sym_err;
    }
    retire_handle();
    return false;
  }

  std::unique_ptr<AddonRecord> rec(new (std::nothrow) AddonRecord());
  if (!rec) {
    if (err_out) *err_out = "out of memory allocating AddonRecord";
    retire_handle();
    return false;
  }

  napi_env env = reg->create_env(reg->runtime, reg->context, canon.c_str(),
                                 &reg->services, reg->host_loop);
  if (!env) {
    if (err_out) *err_out = "chakra_nodeapi_create_env failed for " + raw_path;
    retire_handle();
    return false;
  }

  rec->canonical_path = canon;
  rec->dl_handle = handle;
  rec->env = env;
  rec->api_version = version;
  rec->used_deprecated_register = (!register_fn && has_deprecated);

  if (reg->set_ver_fn) {
    napi_status vs = reg->set_ver_fn(env, version);
    if (vs != napi_ok) {
      if (err_out) {
        *err_out = "chakra_nodeapi_set_module_api_version failed for " +
                   raw_path;
      }
      reg->destroy_env(env);
      retire_handle();
      return false;
    }
  }

  auto open_scope = [&](napi_env e, napi_handle_scope* s) -> napi_status {
#ifdef _WIN32
    return reg->open_scope_fn(e, s);
#else
    return napi_open_handle_scope(e, s);
#endif
  };
  auto close_scope = [&](napi_env e, napi_handle_scope s) -> napi_status {
#ifdef _WIN32
    return reg->close_scope_fn(e, s);
#else
    return napi_close_handle_scope(e, s);
#endif
  };
  auto create_obj = [&](napi_env e, napi_value* v) -> napi_status {
#ifdef _WIN32
    return reg->create_object_fn(e, v);
#else
    return napi_create_object(e, v);
#endif
  };

  napi_handle_scope scope = nullptr;
  if (open_scope(env, &scope) != napi_ok) {
    if (err_out) *err_out = "napi_open_handle_scope failed for " + raw_path;
    reg->destroy_env(env);
    retire_handle();
    return false;
  }

  napi_value exports = nullptr;
  if (create_obj(env, &exports) != napi_ok || !exports) {
    close_scope(env, scope);
    if (err_out) *err_out = "napi_create_object failed for " + raw_path;
    reg->destroy_env(env);
    retire_handle();
    return false;
  }

  JsValueRef cur_js = reinterpret_cast<JsValueRef>(exports);
  if (JsAddRef(cur_js, nullptr) != JsNoError) {
    close_scope(env, scope);
    if (err_out) *err_out = "JsAddRef(exports) failed for " + raw_path;
    reg->destroy_env(env);
    retire_handle();
    return false;
  }

  auto capture_pending_reg_error = [&]() -> bool {
    bool has = false;
    if (JsHasException(&has) == JsNoError && has) {
      JsValueRef ex = JS_INVALID_REFERENCE;
      if (JsGetAndClearException(&ex) == JsNoError &&
          ex != JS_INVALID_REFERENCE) {
        if (rec->pending_reg_error == JS_INVALID_REFERENCE) {
          if (JsAddRef(ex, nullptr) == JsNoError) {
            rec->pending_reg_error = ex;
          }
        }
      }
      return true;
    }
    return false;
  };

  if (register_fn) {
    napi_value result = register_fn(env, exports);
    if (result != nullptr && result != exports) {
      JsValueRef new_js = reinterpret_cast<JsValueRef>(result);
      if (JsAddRef(new_js, nullptr) == JsNoError) {
        JsRelease(cur_js, nullptr);
        cur_js = new_js;
      }
    }
    (void)capture_pending_reg_error();
  } else {
    for (size_t i = pending_before; i < pending_after; ++i) {
      chakra_nodeapi_pending_module_info_compat info{};
      if (reg->get_pending_fn(i, &info) != napi_ok ||
          info.nm_register_func == nullptr) {
        continue;
      }
      napi_value cur_napi = reinterpret_cast<napi_value>(cur_js);
      napi_value ret = info.nm_register_func(env, cur_napi);
      if (ret != nullptr && ret != cur_napi) {
        JsValueRef new_js = reinterpret_cast<JsValueRef>(ret);
        if (JsAddRef(new_js, nullptr) == JsNoError) {
          JsRelease(cur_js, nullptr);
          cur_js = new_js;
        }
      }
      if (capture_pending_reg_error()) {
        break;
      }
    }
  }

  rec->exports = cur_js;
  close_scope(env, scope);

  AddonRecord* raw_rec = rec.get();
  bool pushed = false;
  try {
    reg->addon_order.push_back(std::move(rec));
    pushed = true;
    reg->registry_by_path.emplace(raw_rec->canonical_path, raw_rec);
  } catch (...) {
    if (raw_rec->exports != JS_INVALID_REFERENCE) {
      JsRelease(raw_rec->exports, nullptr);
      raw_rec->exports = JS_INVALID_REFERENCE;
    }
    if (raw_rec->pending_reg_error != JS_INVALID_REFERENCE) {
      JsRelease(raw_rec->pending_reg_error, nullptr);
      raw_rec->pending_reg_error = JS_INVALID_REFERENCE;
    }
    reg->destroy_env(raw_rec->env);
    raw_rec->env = nullptr;
    if (pushed) {
      reg->addon_order.pop_back();
    }
    retire_handle();
    if (err_out) *err_out = "out of memory registering addon";
    return false;
  }

  if (out) *out = raw_rec;
  return true;
}

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
// reference them; saves/restores active_module_dir and wrapper globals for
// nesting. Returns JS_INVALID_REFERENCE on failure (exception already printed).
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
  struct ScopedJsRoot {
    JsValueRef ref = JS_INVALID_REFERENCE;
    bool rooted = false;
    explicit ScopedJsRoot(JsValueRef v) : ref(v) {
      if (ref != JS_INVALID_REFERENCE && JsAddRef(ref, nullptr) == JsNoError) {
        rooted = true;
      }
    }
    ~ScopedJsRoot() {
      if (rooted && ref != JS_INVALID_REFERENCE) {
        JsRelease(ref, nullptr);
      }
    }
    ScopedJsRoot(const ScopedJsRoot&) = delete;
    ScopedJsRoot& operator=(const ScopedJsRoot&) = delete;
  };

  JsValueRef module_obj = JS_INVALID_REFERENCE;
  JsValueRef exports_obj = JS_INVALID_REFERENCE;
  if (JsCreateObject(&module_obj) != JsNoError ||
      JsCreateObject(&exports_obj) != JsNoError) {
    std::fprintf(stderr, "ch-nodeapi: require: cannot create module\n");
    return JS_INVALID_REFERENCE;
  }
  // Root module_obj, exports_obj, and the saved host globals across JsRun:
  // executed user code can mutate globalThis.__nodeapi_* / __filename__ /
  // __dirname__ and trigger GC before this frame restores or caches them
  // (doc/memory-management.md sec 3: RAII JsAddRef/JsRelease scope).
  ScopedJsRoot module_root(module_obj);
  ScopedJsRoot exports_root(exports_obj);
  {
    JsPropertyIdRef pid = JS_INVALID_REFERENCE;
    JsCreatePropertyId("exports", 7, &pid);
    JsSetProperty(module_obj, pid, exports_obj, false);
  }
  std::string prev_active_dir = rs->active_module_dir;
  rs->active_module_dir = host_dirname(abs_path);
  JsValueRef prev_mod = host_get_global("__nodeapi_module__");
  JsValueRef prev_exp = host_get_global("__nodeapi_exports__");
  JsValueRef prev_fn = host_get_global("__filename__");
  JsValueRef prev_dn = host_get_global("__dirname__");
  ScopedJsRoot prev_mod_root(prev_mod);
  ScopedJsRoot prev_exp_root(prev_exp);
  ScopedJsRoot prev_fn_root(prev_fn);
  ScopedJsRoot prev_dn_root(prev_dn);
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
  // Pin script_val before creating url_val (which can trigger GC) and pin
  // url_val across JsRun; run_result is unused and never JsAddRef'd.
  JsCreateString(wrapper.c_str(), wrapper.size(), &script_val);
  const bool script_rooted =
      (script_val != JS_INVALID_REFERENCE &&
       JsAddRef(script_val, nullptr) == JsNoError);
  JsCreateString(abs_path.c_str(), abs_path.size(), &url_val);
  const bool url_rooted =
      (url_val != JS_INVALID_REFERENCE &&
       JsAddRef(url_val, nullptr) == JsNoError);
  JsErrorCode err = JsRun(script_val, JS_SOURCE_CONTEXT_NONE, url_val,
                          JsParseScriptAttributeNone, &run_result);
  if (url_rooted) JsRelease(url_val, nullptr);
  if (script_rooted) JsRelease(script_val, nullptr);
  if (prev_mod != JS_INVALID_REFERENCE) host_set_global("__nodeapi_module__", prev_mod);
  if (prev_exp != JS_INVALID_REFERENCE) host_set_global("__nodeapi_exports__", prev_exp);
  if (prev_fn != JS_INVALID_REFERENCE) host_set_global("__filename__", prev_fn);
  if (prev_dn != JS_INVALID_REFERENCE) host_set_global("__dirname__", prev_dn);
  rs->active_module_dir = prev_active_dir;
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

  auto split_node_path = [](const char* np) -> std::vector<std::string> {
    std::vector<std::string> dirs;
    if (!np || !np[0]) return dirs;
#ifdef _WIN32
    const char path_sep = ';';
#else
    const char path_sep = ':';
#endif
    std::string s(np);
    size_t start = 0;
    while (start <= s.size()) {
      size_t sep = s.find(path_sep, start);
      std::string seg = (sep == std::string::npos)
                            ? s.substr(start)
                            : s.substr(start, sep - start);
      if (!seg.empty()) dirs.push_back(seg);
      if (sep == std::string::npos) break;
      start = sep + 1;
    }
    return dirs;
  };

  // 1. Builtin-ish shims via NODE_PATH.
  if (req == "assert" || req == "node:assert" || req == "bindings") {
    std::string shim = (req == "bindings") ? "bindings.js" : "assert.js";
    std::vector<std::string> dirs = split_node_path(npath);
    if (!dirs.empty()) {
      for (const std::string& base : dirs) {
        std::string cand = host_normalize(base + "/" + shim);
        if (!host_file_exists(cand)) continue;
        JsValueRef m = load_commonjs_module(rs, cand);
        if (m != JS_INVALID_REFERENCE) return m;
        return undef;
      }
      std::fprintf(stderr, "ch-nodeapi: require: cannot load shim %s\n",
                   shim.c_str());
      return undef;
    }
    std::fprintf(stderr, "ch-nodeapi: require('%s') needs NODE_PATH (shims dir)\n", req.c_str());
    return undef;
  }

  auto return_addon_record = [&](AddonRecord* rec) -> JsValueRef {
    if (rec->pending_reg_error != JS_INVALID_REFERENCE &&
        !rec->pending_reg_thrown) {
      rec->pending_reg_thrown = true;
      (void)JsSetException(rec->pending_reg_error);
      return undef;
    }
    return (rec->exports != JS_INVALID_REFERENCE) ? rec->exports : undef;
  };

  auto load_concrete_node = [&](const std::string& node_path) -> JsValueRef {
    AddonRecord* rec = nullptr;
    std::string err;
    if (!load_and_register_addon(rs->registry, node_path, &rec, &err)) {
      host_throw_error("ch-nodeapi: " + err);
      return undef;
    }
    return return_addon_record(rec);
  };

  // 2. Check if req resolves to a real .node file on disk.
  bool is_node = host_ends_with(req, ".node");
#ifdef _WIN32
  bool has_build = (req.find("/build/") != std::string::npos ||
                    req.find("\\build\\") != std::string::npos);
  bool is_abs = host_is_abs(req);
  bool is_rel = (req == "." || req == ".." ||
                 req.rfind("./", 0) == 0 || req.rfind("../", 0) == 0 ||
                 req.rfind(".\\", 0) == 0 || req.rfind("..\\", 0) == 0);
#else
  bool has_build = (req.find("/build/") != std::string::npos);
  bool is_abs = (!req.empty() && req[0] == '/');
  bool is_rel = (req == "." || req == ".." ||
                 req.rfind("./", 0) == 0 || req.rfind("../", 0) == 0);
#endif

  std::string cand_node;
  if (is_abs || is_rel || has_build) {
    std::string base = is_abs
                           ? host_normalize(req)
                           : host_normalize(rs->active_module_dir + "/" + req);
    if (is_node) {
      if (host_file_exists(base)) cand_node = base;
    } else if (has_build && !host_ends_with(base, ".js")) {
      if (host_file_exists(base + ".node")) cand_node = base + ".node";
      else if (host_file_exists(base)) cand_node = base;
    }
    if (cand_node.empty() && has_build && bpath && bpath[0]) {
      std::string bname = host_basename(req);
      if (!bname.empty()) {
        std::string bcand = host_normalize(std::string(bpath) + "/" + bname);
        if (is_node) {
          if (host_file_exists(bcand)) cand_node = bcand;
        } else {
          if (host_file_exists(bcand + ".node")) cand_node = bcand + ".node";
          else if (host_file_exists(bcand) && host_ends_with(bcand, ".node")) {
            cand_node = bcand;
          }
        }
      }
    }
  } else if (is_node) {
    for (const std::string& dir : split_node_path(npath)) {
      std::string cand = host_normalize(dir + "/" + req);
      if (host_file_exists(cand)) {
        cand_node = cand;
        break;
      }
    }
  }

  // 3. Concrete .node file on disk: load & register (throw JS Error on fail).
  if (!cand_node.empty()) {
    return load_concrete_node(cand_node);
  }

  // 4. Fallback to addon_order[0] for CLI-preloaded runs when drivers issue
  // synthetic */build/* or bare *.node requests (e.g. node-addon-api drivers
  // requiring ./build/Release/binding.node when --addon points to a
  // differently named binary). Explicit ./ or / missing .node files and
  // --driver-only runs throw a JS Error instead of aliasing another addon.
  if (is_node || has_build) {
    if (rs->registry != nullptr && rs->registry->has_cli_addon &&
        !rs->registry->addon_order.empty() &&
        (has_build || (!is_abs && !is_rel))) {
      return return_addon_record(rs->registry->addon_order[0].get());
    }
    host_throw_error("ch-nodeapi: cannot load addon '" + req + "'");
    return undef;
  }

  // 5. Relative .js, absolute .js, or NODE_PATH file (falling back to .node
  // for extensionless relative/NODE_PATH addon requires).
  std::string abs;
  bool found = false;
  if (is_abs || is_rel) {
    abs = is_abs ? host_normalize(req)
                 : host_normalize(rs->active_module_dir + "/" + req);
    if (host_file_exists(abs + ".js")) { abs += ".js"; found = true; }
    else if (host_file_exists(abs)) { found = true; }
    else if (host_file_exists(abs + "/index.js")) { abs += "/index.js"; found = true; }
    else if (host_file_exists(abs + ".node")) {
      return load_concrete_node(abs + ".node");
    }
  } else {
    std::vector<std::string> dirs = split_node_path(npath);
    if (dirs.empty()) {
      std::fprintf(stderr, "ch-nodeapi: require('%s'): unsupported (no ./ or NODE_PATH)\n", req.c_str());
      return undef;
    }
    for (const std::string& dir : dirs) {
      std::string cand = host_normalize(dir + "/" + req);
      if (host_file_exists(cand + ".js")) { abs = cand + ".js"; found = true; break; }
      if (host_file_exists(cand)) { abs = cand; found = true; break; }
      if (host_file_exists(cand + "/index.js")) { abs = cand + "/index.js"; found = true; break; }
      if (host_file_exists(cand + ".node")) {
        return load_concrete_node(cand + ".node");
      }
    }
    if (!found) abs = host_normalize(dirs[0] + "/" + req);
  }

  if (found) {
    JsValueRef m = load_commonjs_module(rs, abs);
    if (m != JS_INVALID_REFERENCE) return m;
    return undef;
  }
  std::fprintf(stderr, "ch-nodeapi: require('%s'): cannot resolve (tried %s)\n", req.c_str(), abs.c_str());
  return undef;
}

// ---- Promise + host loop queues (PR-7 / PR-8) ----

struct PumpState {
  std::vector<JsValueRef> tasks; /* each JsAddRef'd */
};

struct PendingTask {
  void (*fn)(void*) = nullptr;
  void* arg = nullptr;
};

struct WorkItem {
  void (*execute)(void*) = nullptr;
  void (*complete)(void*) = nullptr;
  void* arg = nullptr;
  bool cancelled = false;
};

// HostLoopState ownership / invalidation record (see doc/memory-management.md):
//   - Domain: host C++ heap (`new (std::nothrow) HostLoopState()` in main()).
//   - Transfer: raw pointer passed to NodeApi host services (`host_ctx`) and
//     spawned `worker_thread_main` threads.
//   - Invalidation: `destroy_env(env)` invalidates all provider async/TSFN
//     handles first so no further `post_work` calls happen; `shutdown_host_loop`
//     sets `shutting_down = true` under `mutex`, drops unstarted `work_queue`
//     entries, and joins workers when safe (`joined == true`).
//   - Release: deleted (alongside `dlclose`) ONLY when `joined == true`. On a
//     timeout/error exit with an active uncancellable `ExecuteWork` still
//     running (`joined == false`), workers are detached and both `host_loop`
//     and the dlopen handles stay alive until process exit so detached worker
//     threads never dereference destroyed sync state or unmapped addon code.
struct HostLoopState {
  std::mutex mutex;
  std::condition_variable cv;
  std::deque<PendingTask> task_queue;
  std::deque<WorkItem> work_queue;
  std::vector<std::thread> workers;
  size_t idle_workers = 0;
  size_t active_works = 0;
  size_t loop_refs = 0;
  bool shutting_down = false;
};

void worker_thread_main(HostLoopState* st) {
  for (;;) {
    WorkItem item{};
    {
      std::unique_lock<std::mutex> lk(st->mutex);
      st->idle_workers++;
      st->cv.wait(lk, [&] {
        return st->shutting_down || !st->work_queue.empty();
      });
      st->idle_workers--;
      if (st->shutting_down) break;
      item = st->work_queue.front();
      st->work_queue.pop_front();
    }
    if (!item.cancelled && item.execute != nullptr) {
      try {
        item.execute(item.arg);
      } catch (...) {
      }
    }
    {
      std::lock_guard<std::mutex> lk(st->mutex);
      if (item.complete != nullptr && !st->shutting_down) {
        try {
          st->task_queue.push_back(PendingTask{item.complete, item.arg});
        } catch (...) {
        }
      }
      if (st->active_works > 0) st->active_works--;
      st->cv.notify_all();
    }
  }
}

bool host_post_task(void* host_ctx, void (*fn)(void*), void* arg) {
  auto* st = static_cast<HostLoopState*>(host_ctx);
  if (st == nullptr || fn == nullptr) return false;
  std::lock_guard<std::mutex> lk(st->mutex);
  if (st->shutting_down) return false;
  try {
    st->task_queue.push_back(PendingTask{fn, arg});
  } catch (...) {
    return false;
  }
  st->cv.notify_all();
  return true;
}

bool host_post_work(void* host_ctx, void (*execute)(void*),
                    void (*complete)(void*), void* arg) {
  auto* st = static_cast<HostLoopState*>(host_ctx);
  if (st == nullptr) return false;
  std::lock_guard<std::mutex> lk(st->mutex);
  if (st->shutting_down) return false;
  constexpr size_t kMaxWorkers = 4;
  if (st->idle_workers == 0 && st->workers.size() < kMaxWorkers) {
    try {
      st->workers.emplace_back(worker_thread_main, st);
    } catch (...) {
      if (st->workers.empty()) return false;
    }
  }
  try {
    st->work_queue.push_back(WorkItem{execute, complete, arg, false});
  } catch (...) {
    return false;
  }
  st->active_works++;
  st->cv.notify_all();
  return true;
}

void host_cancel_work(void* host_ctx, void* arg) {
  auto* st = static_cast<HostLoopState*>(host_ctx);
  if (st == nullptr) return;
  std::lock_guard<std::mutex> lk(st->mutex);
  for (auto it = st->work_queue.begin(); it != st->work_queue.end(); ++it) {
    if (it->arg == arg && !it->cancelled) {
      void (*complete)(void*) = it->complete;
      st->work_queue.erase(it);
      if (complete != nullptr && !st->shutting_down) {
        try {
          st->task_queue.push_back(PendingTask{complete, arg});
        } catch (...) {
        }
      }
      if (st->active_works > 0) st->active_works--;
      st->cv.notify_all();
      return;
    }
  }
}

void host_ref_loop(void* host_ctx) {
  auto* st = static_cast<HostLoopState*>(host_ctx);
  if (st == nullptr) return;
  std::lock_guard<std::mutex> lk(st->mutex);
  st->loop_refs++;
}

void host_unref_loop(void* host_ctx) {
  auto* st = static_cast<HostLoopState*>(host_ctx);
  if (st == nullptr) return;
  std::lock_guard<std::mutex> lk(st->mutex);
  if (st->loop_refs > 0) st->loop_refs--;
  if (st->loop_refs == 0) st->cv.notify_all();
}

// Ownership record (doc/memory-management.md sec 3):
// - Unstarted `WorkItem`s in `work_queue` and pending `PendingTask`s in
//   `task_queue` own libChakraNodeApi heap state (e.g., `AsyncWork*`,
//   `TsfnTaskArg*`) until their `complete`/`fn` trampoline runs once.
// - `shutdown_all` calls `destroy_env` on every addon env before calling
//   `shutdown_host_loop`. When exiting via `--pump-timeout-ms` or error while
//   workers are still busy, flush swapped `task_queue` / `work_queue`
//   trampolines outside `st->mutex` so queued records drop refs cleanly
//   (post-destroy `WorkCompleteTramp` skips user JS and calls `DropWorkRef`).
bool shutdown_host_loop(HostLoopState* st, bool wait_for_active) {
  if (st == nullptr) return true;
  std::vector<std::thread> to_join;
  std::deque<WorkItem> unstarted_work;
  std::deque<PendingTask> remaining_tasks;
  bool has_active = false;
  {
    std::lock_guard<std::mutex> lk(st->mutex);
    st->shutting_down = true;
    unstarted_work.swap(st->work_queue);
    remaining_tasks.swap(st->task_queue);
    const size_t dropped = unstarted_work.size();
    st->active_works =
        (st->active_works > dropped) ? (st->active_works - dropped) : 0;
    has_active = (st->active_works > 0);
    st->cv.notify_all();
    to_join.swap(st->workers);
  }
  if (!has_active || wait_for_active) {
    for (auto& w : to_join) {
      if (w.joinable()) w.join();
    }
    {
      std::lock_guard<std::mutex> lk(st->mutex);
      while (!st->task_queue.empty()) {
        remaining_tasks.push_back(st->task_queue.front());
        st->task_queue.pop_front();
      }
    }
  } else {
    // Timeout/error exit while an uncancellable worker ExecuteWork is still
    // running: detach workers so process exit honors --pump-timeout-ms
    // promptly; caller skips dlclose so addon text stays mapped until exit.
    for (auto& w : to_join) {
      if (w.joinable()) w.detach();
    }
  }
  for (const PendingTask& t : remaining_tasks) {
    if (t.fn != nullptr) {
      try {
        t.fn(t.arg);
      } catch (...) {
      }
    }
  }
  for (const WorkItem& w : unstarted_work) {
    if (w.complete != nullptr) {
      try {
        w.complete(w.arg);
      } catch (...) {
      }
    }
  }
  return !has_active || wait_for_active;
}

void promise_continuation(JsValueRef task, void* state) {
  auto* pump = static_cast<PumpState*>(state);
  if (JsAddRef(task, nullptr) == JsNoError) {
    pump->tasks.push_back(task);
  }
}

// Run queued promise tasks, FIFO host post_task items, and JsIdle until quiet
// (both queues empty AND active_works == 0 AND loop_refs == 0). Waits up to
// `timeout_ms` when waiting on async work / kept-alive TSFNs.
int pump_until_quiet(PumpState* pump, HostLoopState* loop, long limit,
                     long timeout_ms, bool allow_wait = true) {
  const auto deadline = std::chrono::steady_clock::now() +
                        std::chrono::milliseconds(timeout_ms < 0 ? 0 : timeout_ms);
  for (long iter = 0; iter < limit;) {
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

    if (loop != nullptr) {
      for (;;) {
        PendingTask ht{};
        {
          std::lock_guard<std::mutex> lk(loop->mutex);
          if (loop->task_queue.empty()) break;
          ht = loop->task_queue.front();
          loop->task_queue.pop_front();
        }
        any = true;
        if (++iter > limit) {
          std::fprintf(stderr,
                       "ch-nodeapi: pump exceeded limit %ld tasks (hang?)\n",
                       limit);
          return 1;
        }
        if (ht.fn != nullptr) {
          ht.fn(ht.arg);
          bool has = false;
          if (JsHasException(&has) == JsNoError && has) {
            clear_exception("host task threw");
            return 1;
          }
        }
        // Interleave any promise continuations queued by the host task before
        // draining further tasks.
        if (!pump->tasks.empty()) break;
      }
      if (!pump->tasks.empty()) continue;
    }

    unsigned int next_tick = 0;
    (void)JsIdle(&next_tick);

    if (!pump->tasks.empty()) continue;
    if (loop != nullptr) {
      std::unique_lock<std::mutex> lk(loop->mutex);
      if (!loop->task_queue.empty()) continue;
      if (allow_wait && (loop->active_works > 0 || loop->loop_refs > 0)) {
        bool ready = loop->cv.wait_until(lk, deadline, [&] {
          return !loop->task_queue.empty() ||
                 (loop->active_works == 0 && loop->loop_refs == 0) ||
                 loop->shutting_down;
        });
        if (!ready && loop->task_queue.empty() &&
            (loop->active_works > 0 || loop->loop_refs > 0)) {
          std::fprintf(stderr,
                       "ch-nodeapi: pump timed out after %ldms "
                       "(active_works=%zu, loop_refs=%zu)\n",
                       timeout_ms, loop->active_works, loop->loop_refs);
          return 1;
        }
        continue;
      }
    }
    if (!any) break;
  }
  bool remaining = !pump->tasks.empty();
  if (!remaining && loop != nullptr) {
    std::lock_guard<std::mutex> lk(loop->mutex);
    remaining = !loop->task_queue.empty() ||
                (allow_wait && (loop->active_works > 0 || loop->loop_refs > 0));
  }
  if (remaining) {
    std::fprintf(stderr,
                 "ch-nodeapi: pump exceeded limit %ld tasks (hang?)\n", limit);
    return 1;
  }
  return 0;
}

} /* namespace */

int main(int argc, char** argv) {
  Config cfg;
  std::vector<std::string> positional;
  std::vector<std::string> flag_addons;
  std::string flag_driver;
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
    else if (a == "--addon") flag_addons.push_back(need(a.c_str()));
    else if (a == "--driver") flag_driver = need(a.c_str());
    else if (a == "--exports") cfg.exports_name = need(a.c_str());
    else if (a == "--pump-limit") cfg.pump_limit = std::atol(need(a.c_str()).c_str());
    else if (a == "--pump-timeout-ms")
      cfg.pump_timeout_ms = std::atol(need(a.c_str()).c_str());
    else if (a == "-v" || a == "--version") {
      std::printf("ch-nodeapi 1.0.0\n");
      return 0;
    } else if (a == "-h" || a == "--help") {
      std::printf("usage: ch-nodeapi [--provider PATH] [--exports NAME] [--pump-limit N] [--pump-timeout-ms N] [--addon PATH.node]... [--driver PATH.js] [addon.node] [driver.js]\n");
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
#ifdef _WIN32
    std::vector<char> exe_buf(4096, '\0');
    DWORD exe_len = 0;
    for (;;) {
      exe_len = GetModuleFileNameA(
          nullptr, exe_buf.data(), static_cast<DWORD>(exe_buf.size()));
      if (exe_len == 0) break;
      if (exe_len < exe_buf.size() - 1) break;
      if (exe_buf.size() >= 32768) break;
      exe_buf.resize(exe_buf.size() * 2, '\0');
    }
    if (exe_len > 0) {
      std::string exe(exe_buf.data(), exe_len);
      size_t slash = exe.find_last_of("/\\");
      if (slash != std::string::npos) {
        std::string dir = exe.substr(0, slash);
        static const char* cands[] = {
            "libChakraNodeApi.dll", "ChakraNodeApi.dll",
            "libChakraNodeApiStub.dll", "ChakraNodeApiStub.dll", nullptr};
        static const char* subdirs[] = {"\\", "\\..\\lib\\", "\\..\\bin\\", nullptr};
        for (int s = 0; subdirs[s] && cfg.provider_path.empty(); s++) {
          for (int k = 0; cands[k]; k++) {
            std::string p = dir + subdirs[s] + cands[k];
            if (host_file_exists(p)) {
              cfg.provider_path = p;
              break;
            }
          }
        }
      }
    }
#else
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
#endif
  }
  cfg.addon_paths = flag_addons;
  cfg.driver_path = flag_driver;
  if (flag_addons.empty() && flag_driver.empty()) {
    if (positional.size() >= 1) cfg.addon_paths.push_back(positional[0]);
    if (positional.size() >= 2) cfg.driver_path = positional[1];
    if (positional.size() > 2) {
      std::fprintf(stderr, "ch-nodeapi: unexpected positional argument %s\n",
                   positional[2].c_str());
      return 1;
    }
  } else {
    for (const std::string& p : positional) {
      if (cfg.driver_path.empty() && !host_ends_with(p, ".node")) {
        cfg.driver_path = p;
      } else {
        cfg.addon_paths.push_back(p);
      }
    }
  }
  if (cfg.addon_paths.empty() && cfg.driver_path.empty()) {
    std::fprintf(stderr,
                 "usage: ch-nodeapi [--provider PATH] [--addon PATH.node]... [--driver PATH.js] [addon.node] [driver.js]\n"
                 "  provider: --provider or $CHAKRA_NODEAPI_PROVIDER\n");
    return 1;
  }

  std::string startup_cwd;
  {
#ifdef _WIN32
    DWORD needed = GetCurrentDirectoryA(0, nullptr);
    if (needed > 0) {
      std::vector<char> cwdbuf(needed, '\0');
      DWORD got = GetCurrentDirectoryA(needed, cwdbuf.data());
      if (got > 0 && got < needed) startup_cwd.assign(cwdbuf.data(), got);
      else startup_cwd = ".";
    } else {
      startup_cwd = ".";
    }
#else
    char cwdbuf[4096];
    if (getcwd(cwdbuf, sizeof(cwdbuf))) startup_cwd = cwdbuf;
    else startup_cwd = ".";
#endif
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

  // Host promise-pump queue (PR-8). Installed BEFORE chakra_nodeapi_create_env
  // with `services.host_manages_promise_queue = true` so the provider knows
  // the host owns the JsSetPromiseContinuationCallback slot and does not
  // overwrite it with its own continuation trampoline. Host drains `pump`
  // alongside `HostLoopState::task_queue` after each JsRun / between
  // registration and teardown.
  PumpState pump;

  // --- Load provider RTLD_GLOBAL (PR-3) ---
  if (cfg.provider_path.empty()) {
    std::fprintf(stderr,
                 "ch-nodeapi: no provider path (--provider or "
                 "$CHAKRA_NODEAPI_PROVIDER)\n");
    dispose_runtime();
    return 1;
  }
  std::string prov_err;
  void* provider = host_dlopen(cfg.provider_path, true, &prov_err);
  if (!provider) {
    std::fprintf(stderr, "ch-nodeapi: dlopen provider %s: %s\n",
                 cfg.provider_path.c_str(), prov_err.c_str());
    dispose_runtime();
    return 1;
  }
  std::string sym_err;
  auto create_env = reinterpret_cast<CreateEnvFn>(
      host_dlsym(provider, "chakra_nodeapi_create_env", &sym_err));
  auto destroy_env = reinterpret_cast<DestroyEnvFn>(
      host_dlsym(provider, "chakra_nodeapi_destroy_env", &sym_err));
  if (!create_env || !destroy_env) {
    std::fprintf(stderr,
                 "ch-nodeapi: provider %s lacks private host API: %s\n",
                 cfg.provider_path.c_str(),
                 sym_err.empty() ? "(unknown)" : sym_err.c_str());
    host_dlclose(provider);
    dispose_runtime();
    return 1;
  }
  // Optional provider symbols (NULL-tolerated for minimal stubs).
  auto set_ver_fn = reinterpret_cast<SetModuleApiVersionFn>(
      host_dlsym(provider, "chakra_nodeapi_set_module_api_version", nullptr));
  auto pending_count_fn = reinterpret_cast<PendingModuleCountFn>(
      host_dlsym(provider, "chakra_nodeapi_pending_module_count", nullptr));
  auto get_pending_fn = reinterpret_cast<GetPendingModuleFn>(
      host_dlsym(provider, "chakra_nodeapi_get_pending_module", nullptr));
#ifdef _WIN32
  auto open_scope_fn = reinterpret_cast<OpenHandleScopeFn>(
      host_dlsym(provider, "napi_open_handle_scope", &sym_err));
  auto close_scope_fn = reinterpret_cast<CloseHandleScopeFn>(
      host_dlsym(provider, "napi_close_handle_scope", &sym_err));
  auto create_object_fn = reinterpret_cast<CreateObjectFn>(
      host_dlsym(provider, "napi_create_object", &sym_err));
  if (!open_scope_fn || !close_scope_fn || !create_object_fn) {
    std::fprintf(stderr,
                 "ch-nodeapi: provider %s lacks Node-API scope/object API: %s\n",
                 cfg.provider_path.c_str(),
                 sym_err.empty() ? "(unknown)" : sym_err.c_str());
    host_dlclose(provider);
    dispose_runtime();
    return 1;
  }
#endif

  HostLoopState* host_loop = new (std::nothrow) HostLoopState();
  if (!host_loop) {
    std::fprintf(stderr, "ch-nodeapi: out of memory allocating HostLoopState\n");
    host_dlclose(provider);
    dispose_runtime();
    return 1;
  }

  // Install the host continuation callback BEFORE env creation and mark
  // `host_manages_promise_queue = true` so the provider preserves the host
  // callback cleanly without clobbering it. Wire real off-thread post_work,
  // FIFO post_task, cancel_work, and ref_loop/unref_loop host services.
  JsSetPromiseContinuationCallback(promise_continuation, &pump);

  chakra_nodeapi_host_services services = {};
  services.post_task = host_post_task;
  services.post_work = host_post_work;
  services.cancel_work = host_cancel_work;
  services.ref_loop = host_ref_loop;
  services.unref_loop = host_unref_loop;
  services.host_manages_promise_queue = true;

  AddonRegistry registry;
  registry.cwd_dir = startup_cwd;
  registry.runtime = runtime;
  registry.context = context;
  registry.services = services;
  registry.host_loop = host_loop;
  registry.create_env = create_env;
  registry.destroy_env = destroy_env;
  registry.set_ver_fn = set_ver_fn;
  registry.pending_count_fn = pending_count_fn;
  registry.get_pending_fn = get_pending_fn;
#ifdef _WIN32
  registry.open_scope_fn = open_scope_fn;
  registry.close_scope_fn = close_scope_fn;
  registry.create_object_fn = create_object_fn;
#endif
  registry.has_cli_addon = !cfg.addon_paths.empty();

  RequireState req_state;
  req_state.cwd_dir = startup_cwd;
  req_state.registry = &registry;

  // Shared teardown: releases host-held roots (R5), destroys every addon's
  // env in reverse addon_order (LIFO) BEFORE shutting down host workers, and
  // only deletes `host_loop` / closes dlopen handles when joined == true.
  auto shutdown_all = [&](int exit_code) -> int {
    for (auto& kv : req_state.cache) {
      if (kv.second != JS_INVALID_REFERENCE) JsRelease(kv.second, nullptr);
    }
    req_state.cache.clear();
    for (JsValueRef t : pump.tasks) {
      if (t != JS_INVALID_REFERENCE) JsRelease(t, nullptr);
    }
    pump.tasks.clear();
    for (auto& rec_ptr : registry.addon_order) {
      if (rec_ptr->exports != JS_INVALID_REFERENCE) {
        JsRelease(rec_ptr->exports, nullptr);
        rec_ptr->exports = JS_INVALID_REFERENCE;
      }
      if (rec_ptr->pending_reg_error != JS_INVALID_REFERENCE) {
        JsRelease(rec_ptr->pending_reg_error, nullptr);
        rec_ptr->pending_reg_error = JS_INVALID_REFERENCE;
      }
    }

    for (size_t i = registry.addon_order.size(); i > 0; --i) {
      AddonRecord* rec = registry.addon_order[i - 1].get();
      if (rec->env != nullptr) {
        destroy_env(rec->env);
        rec->env = nullptr;
      }
    }

    bool joined = shutdown_host_loop(host_loop, exit_code == 0);
    JsSetPromiseContinuationCallback(nullptr, nullptr);
    for (JsValueRef t : pump.tasks) {
      if (t != JS_INVALID_REFERENCE) JsRelease(t, nullptr);
    }
    pump.tasks.clear();
    dispose_runtime();
    if (joined) {
      delete host_loop;
      host_loop = nullptr;
      for (size_t i = registry.addon_order.size(); i > 0; --i) {
        AddonRecord* rec = registry.addon_order[i - 1].get();
        if (rec->dl_handle != nullptr) {
          host_dlclose(rec->dl_handle);
          rec->dl_handle = nullptr;
        }
      }
      for (size_t i = registry.orphan_dl_handles.size(); i > 0; --i) {
        if (registry.orphan_dl_handles[i - 1] != nullptr) {
          host_dlclose(registry.orphan_dl_handles[i - 1]);
        }
      }
      registry.orphan_dl_handles.clear();
      registry.registry_by_path.clear();
      registry.addon_order.clear();
      host_dlclose(provider);
      provider = nullptr;
    }
    return exit_code;
  };

  // --- Preload CLI addons ---
  for (const std::string& apath : cfg.addon_paths) {
    AddonRecord* rec = nullptr;
    std::string err;
    if (!load_and_register_addon(&registry, apath, &rec, &err)) {
      std::fprintf(stderr, "ch-nodeapi: %s\n", err.c_str());
      return shutdown_all(1);
    }
  }
  if (!registry.addon_order.empty() &&
      registry.addon_order[0]->exports != JS_INVALID_REFERENCE) {
    host_set_global(cfg.exports_name.c_str(), registry.addon_order[0]->exports);
  }

  // --- Run driver (if given) + pump ---
  int run_rc = 0;
  if (!cfg.driver_path.empty()) {
    std::string abs_driver = host_resolve_abs(startup_cwd, cfg.driver_path);
    req_state.driver_dir = host_dirname(abs_driver);
    req_state.driver_path = abs_driver;
    req_state.active_module_dir = req_state.driver_dir;
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
    // process: env (from OS environ) + cwd() + argv + platform. No `on`/`exit`:
    // mustCall verification is done explicitly by the host below.
    {
      JsValueRef proc = JS_INVALID_REFERENCE;
      JsCreateObject(&proc);
      JsValueRef env_obj = JS_INVALID_REFERENCE;
      JsCreateObject(&env_obj);
      auto add_env_entry = [&](const std::string& s) {
        size_t eq = s.find('=');
        if (eq == std::string::npos || eq == 0) return;
        std::string k = s.substr(0, eq), v = s.substr(eq + 1);
        JsValueRef vv = JS_INVALID_REFERENCE;
        JsCreateString(v.c_str(), v.size(), &vv);
        JsPropertyIdRef pid = JS_INVALID_REFERENCE;
        JsCreatePropertyId(k.c_str(), k.size(), &pid);
        JsSetProperty(env_obj, pid, vv, false);
      };
#ifdef _WIN32
      LPCH env_block = GetEnvironmentStringsA();
      if (env_block) {
        for (const char* cur = env_block; *cur != '\0';
             cur += std::strlen(cur) + 1) {
          add_env_entry(std::string(cur));
        }
        FreeEnvironmentStringsA(env_block);
      }
#else
      if (environ) {
        for (char** e = environ; *e; e++) {
          add_env_entry(std::string(*e));
        }
      }
#endif
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
        JsCreateString(abs_driver.c_str(), abs_driver.size(), &a1);
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
#ifdef _WIN32
        const char* pl_s = "win32";
#elif defined(__linux__)
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
    if (!read_file(abs_driver, &script)) {
      std::fprintf(stderr, "ch-nodeapi: cannot read driver %s\n",
                   cfg.driver_path.c_str());
      return shutdown_all(1);
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
      JsCreateString(abs_driver.c_str(), abs_driver.size(), &s);
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
    if (JsCreateString(wrapped.c_str(), wrapped.size(), &script_val) != JsNoError) {
      std::fprintf(stderr, "ch-nodeapi: JsCreateString (driver) failed\n");
      return shutdown_all(1);
    }
    const bool script_rooted = (JsAddRef(script_val, nullptr) == JsNoError);
    if (JsCreateString(abs_driver.c_str(), abs_driver.size(), &url_val) != JsNoError) {
      if (script_rooted) JsRelease(script_val, nullptr);
      std::fprintf(stderr, "ch-nodeapi: JsCreateString (driver) failed\n");
      return shutdown_all(1);
    }
    const bool url_rooted = (JsAddRef(url_val, nullptr) == JsNoError);
    JsErrorCode err =
        JsRun(script_val, JS_SOURCE_CONTEXT_NONE, url_val,
              JsParseScriptAttributeNone, &run_result);
    if (url_rooted) JsRelease(url_val, nullptr);
    if (script_rooted) JsRelease(script_val, nullptr);
    if (err != JsNoError) {
      bool has = false;
      if (JsHasException(&has) == JsNoError && has) {
        clear_exception("driver threw");
      } else {
        std::fprintf(stderr, "ch-nodeapi: JsRun failed (%d)\n", (int)err);
      }
      run_rc = 1;
    }
    // Host pump (PR-7 / PR-8): drain promise continuations + host task queue
    // while waiting on active works / TSFN loop refs up to --pump-timeout-ms.
    if (run_rc == 0) {
      // The pump itself reports and returns 1 on overflow/timeout/exception.
      run_rc = pump_until_quiet(&pump, host_loop, cfg.pump_limit,
                                cfg.pump_timeout_ms, /*allow_wait=*/true);
    } else {
      // Driver failed: drain already-queued items without blocking on infinite refs.
      pump_until_quiet(&pump, host_loop, cfg.pump_limit, 0, /*allow_wait=*/false);
    }
    // mustCall verification: the test/common shim records unfulfilled
    // calls; Node fails at process exit. The host replicates that gate
    // here (no process/exit events on this host).
    if (run_rc == 0) {
      // Drain GC-enqueued finalizers on every loaded addon env before
      // checking mustCall: the driver may have run global.gc() with no
      // later napi call. Indexed loop avoids iterator invalidation if a
      // finalizer triggers JS require().
      for (size_t i = 0; i < registry.addon_order.size(); ++i) {
        napi_env addon_env = registry.addon_order[i]->env;
        if (!addon_env) continue;
        napi_handle_scope drain_scope = nullptr;
        napi_value tmp = nullptr;
#ifdef _WIN32
        if (registry.open_scope_fn(addon_env, &drain_scope) == napi_ok &&
            registry.create_object_fn(addon_env, &tmp) == napi_ok) {
          (void)tmp;
        }
        if (drain_scope) registry.close_scope_fn(addon_env, drain_scope);
#else
        if (napi_open_handle_scope(addon_env, &drain_scope) == napi_ok &&
            napi_create_object(addon_env, &tmp) == napi_ok) {
          (void)tmp;
        }
        if (drain_scope) napi_close_handle_scope(addon_env, drain_scope);
#endif
        bool has = false;
        if (JsHasException(&has) == JsNoError && has) {
          clear_exception("drain threw");
          run_rc = 1;
          break;
        }
      }
      // Finalizers may have queued promise/host tasks; pump again.
      if (run_rc == 0) {
        run_rc = pump_until_quiet(&pump, host_loop, cfg.pump_limit,
                                  cfg.pump_timeout_ms, /*allow_wait=*/true);
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
  } else {
    // Registration-only: still pump once so queued continuations / tasks from
    // the register call run before teardown.
    run_rc = pump_until_quiet(&pump, host_loop, cfg.pump_limit,
                              cfg.pump_timeout_ms, /*allow_wait=*/true);
    for (const auto& rec_ptr : registry.addon_order) {
      if (rec_ptr->pending_reg_error != JS_INVALID_REFERENCE) {
        std::fprintf(stderr, "ch-nodeapi: registration threw with no driver\n");
        run_rc = 1;
        break;
      }
    }
  }

  return shutdown_all(run_rc);
}
