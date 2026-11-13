//-------------------------------------------------------------------------------------------------------
// Copyright (c) ChakraCore Project Contributors. All rights reserved.
// Licensed under the MIT license. See LICENSE.txt file in the project root for full license information.
//-------------------------------------------------------------------------------------------------------
//
// Host surface: deprecated registration (pending registry) + fatal routing
// (plan §4 Table B1, task N4.1). See NodeApiHost.h for the ownership
// record, string-copy rule, unload limits, host invocation design, and
// fatal routing design.
//
// Notes for maintainers:
//   - C++17 (matching the provider). Every extern "C" entry that can
//     allocate or invoke user callbacks catches (...) and maps to
//     napi_generic_failure with a recorded last_error (plan D5 amendment).
//     napi_module_register is void: OOM there drops the record silently.
//   - The registry mutex is held only to copy strings / push / snapshot.
//     It is never held across nm_register_func or the host fatal service.
//   - deque (not vector) for stability: inspection returns c_str()
//     pointers into the element's std::string; deque element addresses
//     are stable across push_back, so those borrows stay valid for the
//     process lifetime without an extra heap indirection.

#include "NodeApiHost.h"

#include "NodeApiEnv.h"
#include "NodeApiHostApi.h"
#include "NodeApiScopes.h"
#include "node_api.h"
#include "ChakraCommon.h"
#include "ChakraCore.h"

#include <cstdio>
#include <cstdlib>
#include <deque>
#include <mutex>
#include <string>
#include <vector>

namespace {

struct PendingModule {
  int nm_version;
  unsigned int nm_flags;
  std::string nm_filename; /* provider-owned copy ("" if caller NULL) */
  napi_addon_register_func nm_register_func;
  std::string nm_modname; /* provider-owned copy ("" if caller NULL) */
  void* nm_priv;           /* borrowed value, never dereferenced/freed */
};

/* Process-lifetime pending list. Never cleared (v1 has no unload). */
std::mutex g_pending_mutex;
std::deque<PendingModule> g_pending;

} /* namespace */

extern "C" {

/* napi_module_register (deprecated registration, node_api.h:87-90).
 * C column: Yes (executes with pending, void). No env, no pending check,
 * no status to return: validation failures are silent drops.
 *   - mod == NULL or mod->nm_register_func == NULL => ignore (count
 *     unchanged). This matches the Node contract that adding a module
 *     with no register func has no observable effect; hosts never see a
 *     status, so a silent drop is the only option.
 *   - Strings are copied (see header string-copy rule). Allocation
 *     failure drops the record silently (no abort on a dlopen-thread
 *     static constructor).
 * Thread-safe; callable from any thread including static constructors. */
NAPI_EXTERN void NAPI_CDECL napi_module_register(napi_module* mod) {
  if (mod == nullptr || mod->nm_register_func == nullptr) {
    return;
  }
  try {
    PendingModule rec;
    rec.nm_version = mod->nm_version;
    rec.nm_flags = mod->nm_flags;
    rec.nm_filename = (mod->nm_filename != nullptr) ? mod->nm_filename : "";
    rec.nm_register_func = mod->nm_register_func;
    rec.nm_modname = (mod->nm_modname != nullptr) ? mod->nm_modname : "";
    rec.nm_priv = mod->nm_priv;
    std::lock_guard<std::mutex> lk(g_pending_mutex);
    g_pending.push_back(std::move(rec));
  } catch (...) {
    /* OOM (or allocator failure) while copying strings / pushing.
     * Void signature cannot report; drop silently rather than aborting
     * a global constructor. The addon will be missing, not crashed. */
  }
}

#if defined(__GNUC__)
__attribute__((visibility("default")))
#endif
size_t chakra_nodeapi_pending_module_count(void) {
  std::lock_guard<std::mutex> lk(g_pending_mutex);
  return g_pending.size();
}

#if defined(__GNUC__)
__attribute__((visibility("default")))
#endif
napi_status chakra_nodeapi_get_pending_module(
    size_t index, chakra_nodeapi_pending_module_info* out) {
  if (out == nullptr) {
    return napi_invalid_arg;
  }
  std::lock_guard<std::mutex> lk(g_pending_mutex);
  if (index >= g_pending.size()) {
    return napi_invalid_arg;
  }
  const PendingModule& rec = g_pending[index];
  out->nm_version = rec.nm_version;
  out->nm_flags = rec.nm_flags;
  out->nm_filename = rec.nm_filename.c_str();
  out->nm_register_func = rec.nm_register_func;
  out->nm_modname = rec.nm_modname.c_str();
  out->nm_priv = rec.nm_priv;
  return napi_ok;
}

#if defined(__GNUC__)
__attribute__((visibility("default")))
#endif
napi_status chakra_nodeapi_run_pending_registrations(napi_env env,
                                                     napi_value exports,
                                                     napi_value* result) {
  /* Validate-first (§3.10): NULL exports/result are invalid_arg even if
   * the env is Closing. Record last_error when the env is live. */
  if (exports == nullptr || result == nullptr) {
    if (env != nullptr) {
      nodeapi::SetLastError(env, napi_invalid_arg,
                            "chakra_nodeapi_run_pending_registrations: "
                            "exports/result is NULL");
    }
    return napi_invalid_arg;
  }
  /* Strict Alive-only, gated on pending, with context (invocation runs
   * user JS). Closing => generic_failure; pending => pending_exception
   * without invoking and without marking consumed; dead/wrong-thread =>
   * invalid_arg without entering a context. */
  nodeapi::EntryGuard guard(env, /*allowClosing=*/false,
                            /*allowPending=*/false, /*needContext=*/true);
  if (!guard.ok()) {
    return guard.status();
  }
  nodeapi::NodeEnv* e = nullptr;
  if (!nodeapi::LookupLiveEnv(static_cast<void*>(env), &e) || e == nullptr) {
    nodeapi::SetLastError(env, napi_invalid_arg,
                          "run_pending: unknown env");
    return napi_invalid_arg;
  }
  /* Double-registration rejection: invalid_arg (documented choice). */
  if (e->pending_registrations_consumed) {
    nodeapi::SetLastError(env, napi_invalid_arg,
                          "run_pending: env already consumed pending "
                          "registrations (double registration)");
    return napi_invalid_arg;
  }
  /* Snapshot under lock (copies records). No user code under lock. */
  std::vector<PendingModule> snapshot;
  try {
    std::lock_guard<std::mutex> lk(g_pending_mutex);
    snapshot.reserve(g_pending.size());
    for (const PendingModule& rec : g_pending) {
      snapshot.push_back(rec);
    }
  } catch (...) {
    nodeapi::SetLastError(env, napi_generic_failure,
                          "run_pending: snapshot allocation failed");
    return napi_generic_failure;
  }
  /* Mark consumed BEFORE any invocation: reentrant calls from inside a
   * register func (same env, same thread) see invalid_arg, and nested
   * snapshot growth during invocation is not run for this env. */
  e->pending_registrations_consumed = true;

  napi_value current = exports;
  for (size_t i = 0; i < snapshot.size(); ++i) {
    napi_addon_register_func fn = snapshot[i].nm_register_func;
    if (fn == nullptr) {
      continue;
    }
    napi_value returned = nullptr;
    try {
      returned = fn(env, current);
    } catch (...) {
      /* C++ exception from addon code must not cross extern "C". */
      nodeapi::SetLastError(env, napi_generic_failure,
                            "run_pending: register function threw C++ "
                            "exception");
      *result = current;
      return napi_generic_failure;
    }
    if (returned != nullptr && returned != current) {
      /* Replacement: root into the innermost scope (R1) before the host
       * binds it. The host's registration scope is expected to be the
       * innermost scope; a root failure is surfaced without publishing
       * an unrooted replacement. */
      napi_status rs =
          NodeApiScopes_RootIntoInnermost(env, returned);
      if (rs != napi_ok) {
        nodeapi::SetLastError(env, napi_generic_failure,
                              "run_pending: root replacement failed");
        *result = current;
        return napi_generic_failure;
      }
      current = returned;
    } else if (returned == nullptr) {
      /* NULL return: keep current exports (Node convention: some addons
       * return null; the passed object remains exports). */
    }
    /* Pending check after each invocation: if this module threw (left a
     * JS exception pending), stop and surface it. The exception stays
     * pending (never cleared implicitly); the host's post-registration
     * pending check treats this like a throwing napi_register_module_v1.
     * *result is still the chained value so the host can observe it. */
    bool has = false;
    JsErrorCode ec = JsHasException(&has);
    if (ec == JsNoError && has) {
      *result = current;
      nodeapi::SetLastError(env, napi_pending_exception,
                            "run_pending: register function left a pending "
                            "JS exception");
      return napi_pending_exception;
    }
    /* JsHasException failure without a context is not a pending state
     * (guard already entered the context; recovery is best-effort). */
  }
  *result = current;
  nodeapi::ClearLastError(env);
  return napi_ok;
}

/* napi_fatal_exception (Table B B1) -- full host wiring moved here from
 * NodeApiErrorsImpl.cpp (N2.5 interface). See header fatal routing
 * design. C column: No (gated). */
napi_status NAPI_CDECL napi_fatal_exception(napi_env env, napi_value err) {
  if (err == nullptr) {
    if (env != nullptr) {
      nodeapi::SetLastError(env, napi_invalid_arg,
                            "napi_fatal_exception: err is NULL");
    }
    return napi_invalid_arg;
  }
  /* C No: gated on pending. If pending, return pending without firing
   * (status-mapping fatal_exception row). Strict Alive-only (Closing
   * is generic_failure via the guard). */
  nodeapi::EntryGuard guard(env, /*allowClosing=*/false,
                            /*allowPending=*/false, /*needContext=*/true);
  if (!guard.ok()) return guard.status();
  nodeapi::NodeEnv* e = nullptr;
  if (!nodeapi::LookupLiveEnv(static_cast<void*>(env), &e) || e == nullptr) {
    nodeapi::SetLastError(env, napi_invalid_arg,
                          "napi_fatal_exception: unknown env");
    return napi_invalid_arg;
  }
  if (e->services.fatal_exception != nullptr) {
    /* Root for the duration of the host callback (R1). The host may
     * run JS / GC before reading the value; the provider scope root
     * alone is not a guarantee because the callback itself is user
     * code. JsAddRef on a tagged value is a no-op (harmless). */
    JsValueRef errJs = reinterpret_cast<JsValueRef>(err);
    (void)JsAddRef(errJs, nullptr);
    e->services.fatal_exception(e->host_ctx, err);
    (void)JsRelease(errJs, nullptr);
    nodeapi::ClearLastError(env);
    return napi_ok;
  }
  std::fprintf(stderr,
               "napi_fatal_exception: uncaught exception (no host "
               "fatal_exception service, aborting)\n");
  std::fflush(stderr);
  std::abort();
  /* Never returns (noreturn by policy). */
}

NAPI_EXTERN NAPI_NO_RETURN void NAPI_CDECL napi_fatal_error(
    const char* location, size_t location_len, const char* message,
    size_t message_len) {
  /* Noreturn, no env, no pending check (status-mapping fatal_error row:
   * Yes noreturn, impl has no pending check). No per-env service is
   * consulted because the signature carries no env (see header).
   * Prints bounded "%.*s: %.*s" and aborts; never returns. */
  const char* loc = (location != nullptr) ? location : "";
  const char* msg = (message != nullptr) ? message : "";
  size_t llen = (location != nullptr) ? location_len : 0;
  size_t mlen = (message != nullptr) ? message_len : 0;
  /* Cap the %.*s int precision so a huge length cannot overflow. */
  const int kCap = 1 << 20;
  int llen_i = (llen > (size_t)kCap) ? kCap : (int)llen;
  int mlen_i = (mlen > (size_t)kCap) ? kCap : (int)mlen;
  std::fprintf(stderr, "%.*s: %.*s\n", llen_i, loc, mlen_i, msg);
  std::fflush(stderr);
  std::abort();
}

} /* extern "C" */
