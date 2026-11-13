//-------------------------------------------------------------------------------------------------------
// Copyright (c) ChakraCore Project Contributors. All rights reserved.
// Licensed under the MIT license. See LICENSE.txt file in the project root for full license information.
//-------------------------------------------------------------------------------------------------------
//
// TEST-ONLY shim implementation (see scopes_shim.h). Not part of the
// provider, not shipped: implements the reconciled N1.1 Interface
// (nodeapi::CheckAlive / CheckAliveAllowClosing / SetLastError +
// NodeApiEnv_ScopeState / IsAlive) over a probe env struct that embeds
// NodeApiScopeState* + owner_thread + live-set by value. Mirrors
// NodeApiEnv.cpp semantics for the needs of the scopes probe:
//   - fabricated/dead values never dereferenced (live-set by value); a
//     dead value (destroyed + freed) is simply absent from the set.
//   - wrong-thread => debug assert + release napi_invalid_arg, no
//     context entry, no state mutation (HIGH-3).
//   - strict (CheckAlive): Closing (alive==false while live) =>
//     napi_generic_failure; teardown-compatible (CheckAliveAllowClosing):
//     Closing => ok (only Dead fails). The shim has no observable
//     Closing window (Destroy frees immediately), so both guards share
//     the same body except the Closing arm. After destroy the pointer
//     is removed from the live set BEFORE delete, so a saved dead
//     value is detected by the set miss without dereferencing it.

#include "scopes_shim.h"
#include "NodeApiEnv.h" /* nodeapi::CheckAlive/... declarations (shim defines) */
#include "NodeApiScopes.h"

#include <cassert>
#include <cstdio>
#include <mutex>
#include <thread>
#include <unordered_set>

struct ScopesProbeEnv {
  JsRuntimeHandle runtime; /* host-owned; never disposed here */
  JsContextRef context; /* host-owned; never disposed here */
  bool alive; /* env state (Alive while true; destroy sets false before teardown) */
  std::thread::id owner_thread; /* HIGH-3 thread check */
  NodeApiScopeState* scopes; /* provider-owned scope state */
};

namespace {

/* Live-set by pointer VALUE (R-ENV-10): consulted before any
 * dereference so fabricated/dead values never crash. */
std::mutex g_shim_mutex;
std::unordered_set<void*> g_shim_live;

ScopesProbeEnv* AsEnv(napi_env env) {
  return reinterpret_cast<ScopesProbeEnv*>(env);
}

bool ShimIsLive(napi_env env, ScopesProbeEnv** out) {
  if (env == nullptr || out == nullptr) return false;
  std::lock_guard<std::mutex> lk(g_shim_mutex);
  if (g_shim_live.find(static_cast<void*>(env)) == g_shim_live.end()) {
    return false;
  }
  *out = AsEnv(env);
  return true;
}

/* Minimal §3.10 mapping (conservative subset; N1.5/N2.1 own the full one):
 * JsNoError ok; exception-state => pending; else generic failure. */
napi_status MapJs(JsErrorCode err) {
  if (err == JsNoError) return napi_ok;
  if (err == JsErrorInExceptionState || err == JsErrorScriptException) {
    return napi_pending_exception;
  }
  return napi_generic_failure;
}

} /* namespace */

namespace nodeapi {

napi_status CheckAlive(napi_env env_value) {
  if (env_value == nullptr) return napi_invalid_arg;
  ScopesProbeEnv* e = nullptr;
  if (!ShimIsLive(env_value, &e)) return napi_invalid_arg;
  if (std::this_thread::get_id() != e->owner_thread) {
#if !defined(NDEBUG)
    std::fprintf(stderr, "scopes_shim: wrong-thread call env=%p\n",
                 static_cast<void*>(env_value));
    assert(false);
#endif
    return napi_invalid_arg;
  }
  if (!e->alive) return napi_generic_failure; /* Closing/Dead (R-ENV-4) */
  return napi_ok;
}

napi_status CheckAliveAllowClosing(napi_env env_value) {
  if (env_value == nullptr) return napi_invalid_arg;
  ScopesProbeEnv* e = nullptr;
  if (!ShimIsLive(env_value, &e)) return napi_invalid_arg;
  if (std::this_thread::get_id() != e->owner_thread) {
#if !defined(NDEBUG)
    std::fprintf(stderr, "scopes_shim: wrong-thread call (allow-closing) env=%p\n",
                 static_cast<void*>(env_value));
    assert(false);
#endif
    return napi_invalid_arg;
  }
  /* Teardown-compatible: Closing (alive==false) is ok; only dead
   * (set miss above) fails. The shim has no live-but-Closing
   * observable window, so this differs from CheckAlive only in the
   * state arm. */
  return napi_ok;
}

void SetLastError(napi_env /*env*/, napi_status /*status*/,
                  const char* /*message*/) {
  /* No last-error storage in the shim (scopes probe does not assert
   * on it). No-op. */
}

} /* namespace nodeapi */

extern "C" {

/* ----- Reconciled N1.1 interface (shim stand-ins) ----- */

NodeApiScopeState* NodeApiEnv_ScopeState(napi_env env) {
  ScopesProbeEnv* e = nullptr;
  if (!ShimIsLive(env, &e)) return nullptr;
  return e->scopes;
}

int /*bool*/ NodeApiEnv_IsAlive(napi_env env) {
  ScopesProbeEnv* e = nullptr;
  if (!ShimIsLive(env, &e)) return 0;
  return e->alive ? 1 : 0;
}

#ifdef CHAKRA_NODEAPI_TEST_HOOKS
/* R-REF-14 total's refs part: the shim env has no refs state. */
size_t NodeApiRefs_RefRootCount(napi_env env) {
  (void)env;
  return 0;
}
#endif

/* ----- Host-API entry points (test versions) ----- */

} /* extern "C" */

napi_env ScopesProbe_CreateEnv(JsRuntimeHandle runtime, JsContextRef context) {
  if (runtime == JS_INVALID_RUNTIME_HANDLE || context == JS_INVALID_REFERENCE) {
    return nullptr;
  }
  auto* e = new ScopesProbeEnv();
  e->runtime = runtime;
  e->context = context;
  e->alive = true;
  e->owner_thread = std::this_thread::get_id();
  e->scopes = NodeApiScopes_CreateState(); /* pushes the default scope */
  /* Context discipline (plan §3.2): the env's context must be current for
   * rooting. The test host leaves it current across runs; the shim
   * defensively enters it so driver calls are self-contained. */
  JsSetCurrentContext(context);
  napi_env env = reinterpret_cast<napi_env>(e);
  {
    std::lock_guard<std::mutex> lk(g_shim_mutex);
    g_shim_live.insert(static_cast<void*>(env));
  }
  return env;
}

void ScopesProbe_DestroyEnv(napi_env env) {
  ScopesProbeEnv* e = nullptr;
  if (!ShimIsLive(env, &e)) {
    /* Null/dead/fabricated value: do not dereference (R-ENV-10). */
    return;
  }
  /* Owner-thread destroy only (R-ENV-6): wrong-thread destroys are
   * ignored with no partial teardown (assert in debug). */
  if (std::this_thread::get_id() != e->owner_thread) {
#if !defined(NDEBUG)
    std::fprintf(stderr, "scopes_shim: destroy from wrong thread env=%p (ignored)\n",
                 static_cast<void*>(env));
    assert(false);
#endif
    return;
  }
  e->alive = false; /* Closing; public entries now fail (R-ENV-4) */
  NodeApiScopes_DestroyForEnv(env); /* step 6: all scopes incl. default */
  /* Unpublish BEFORE delete so a saved dead value is a set miss (no
   * dereference) rather than a dangling-pointer read. */
  {
    std::lock_guard<std::mutex> lk(g_shim_mutex);
    g_shim_live.erase(static_cast<void*>(env));
  }
  NodeApiScopes_FreeState(e->scopes);
  delete e;
}

extern "C" {

napi_status NAPI_CDECL napi_create_object(napi_env env, napi_value* result) {
  if (env == nullptr || result == nullptr) return napi_invalid_arg;
  JsValueRef obj = JS_INVALID_REFERENCE;
  napi_status s = MapJs(JsCreateObject(&obj));
  if (s != napi_ok) return s;
  s = NodeApiScopes_RootIntoInnermost(env, reinterpret_cast<napi_value>(obj));
  if (s != napi_ok) return s;
  *result = reinterpret_cast<napi_value>(obj);
  return napi_ok;
}

napi_status NAPI_CDECL napi_create_double(napi_env env, double value,
                                          napi_value* result) {
  if (env == nullptr || result == nullptr) return napi_invalid_arg;
  JsValueRef v = JS_INVALID_REFERENCE;
  napi_status s = MapJs(JsDoubleToNumber(value, &v));
  if (s != napi_ok) return s;
  s = NodeApiScopes_RootIntoInnermost(env, reinterpret_cast<napi_value>(v));
  if (s != napi_ok) return s;
  *result = reinterpret_cast<napi_value>(v);
  return napi_ok;
}

} /* extern "C" */
