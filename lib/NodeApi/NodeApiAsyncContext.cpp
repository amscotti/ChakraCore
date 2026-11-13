//-------------------------------------------------------------------------------------------------------
// Copyright (c) ChakraCore Project Contributors. All rights reserved.
// Licensed under the MIT license. See LICENSE.txt file in the project root for full license information.
//-------------------------------------------------------------------------------------------------------
//
// Async context + callback scopes (N4.3). See NodeApiAsyncContext.h for
// the surface, status mapping, Closing policy, and the loud no-op
// warning (NO async_hooks propagation / AsyncLocalStorage semantics).
// This file is the ownership record (plan B3 + §3.6; memory-
// management.md §3).
//
// Implementation notes (Node v22.22.3 via doc/node-api-status-mapping.md):
//   - napi_async_init: C Yes (executes with pending). Binding NULLs =>
//     invalid_arg. resource null/undefined => object_expected
//     (ToObject pre-check, no JS throw, mirroring NodeApiObjects.cpp
//     ToObject); else JsConvertValueToObject. name null/undefined =>
//     string_expected (pre-check); else JsConvertValueToString.
//     Convert throws map via §3.10 (pending/generic). The converted
//     values are discarded (no JS roots retained; see record (a) in
//     the header -- bookkeeping only, no string copy, no root).
//   - napi_async_destroy: C Yes. NULL/unknown/foreign/dead/double =>
//     invalid_arg. Teardown-compatible (Closing succeeds).
//   - napi_open_callback_scope: C Yes. resource_object/result NULL =>
//     invalid_arg. Context NULL (default) or live-in-this-env => ok;
//     otherwise invalid_arg. No JS type check (C column).
//   - napi_close_callback_scope: C Yes. NULL/unknown => invalid_arg.
//     Out-of-order (in-stack, not top) => callback_scope_mismatch +
//     still releases the named scope (retire), stack otherwise
//     intact (N1.2 CloseImpl discipline, distinct stack).
//   - napi_make_callback: C No (gated). See header for the full
//     validate -> guard -> pre-checks -> handle scope -> call ->
//     pump -> move-root -> close -> publish sequence. The pump is
//     NodeApiPromise_Pump (PR-8 provider-queue case); host-queue
//     draining is host-owned (see header split).
//
// No C++ exception crosses the extern "C" surface: every provider
// allocation on a napi_* path is nothrow/try-catch wrapped and maps
// to napi_generic_failure; the whole body of each entry is wrapped
// in try/catch(...).

#include "NodeApiAsyncContext.h"

#include "NodeApiEnv.h"
#include "NodeApiPromise.h"
#include "NodeApiScopes.h"
#include "node_api.h" /* NAPI_EXTERN default-visibility decls for B3 */
#include "ChakraCommon.h"
#include "ChakraCore.h"

#include <cstdint>
#include <mutex>
#include <new>
#include <unordered_map>
#include <vector>

// Opaque handle layouts (provider-defined). The record address IS the
// handle value. Handles are validated by VALUE (pointer-identity scan
// of the owning env's live list) before any dereference, so
// foreign/dead/fabricated values fail cleanly (R-ENV-10 pattern).
struct napi_async_context__ {
  napi_env owner = nullptr;      // pointer VALUE only; never dereferenced
  std::uint64_t generation = 0;  // env generation at creation (ABA guard)
  std::uint64_t id = 0;          // per-env monotonic
  bool destroyed = false;
};

struct napi_callback_scope__ {
  napi_env owner = nullptr;             // pointer VALUE only
  std::uint64_t generation = 0;         // env generation at creation
  napi_async_context async_context = nullptr; // snapshot, never dereferenced
  std::uint64_t id = 0;                 // per-env monotonic
};

namespace {

// Per-env state (lazy, global map keyed by env VALUE; NodeEnv layout
// untouched, mirroring Wrap lazy state / Lifetime pump map).
struct AsyncState {
  napi_env owner = nullptr; // pointer VALUE (map key mirror)
  std::uint64_t generation = 0;
  std::uint64_t next_context_id = 1;
  std::uint64_t next_scope_id = 1;
  std::vector<napi_async_context> contexts;        // live (not destroyed)
  std::vector<napi_async_context> retired_contexts; // destroyed, deferred-free
  std::vector<napi_callback_scope> scopes;          // stack (LIFO, back=top)
  std::vector<napi_callback_scope> retired_scopes;  // closed, deferred-free
};

std::mutex g_state_mutex;
std::unordered_map<void*, AsyncState*> g_states;

using Js = JsValueRef;

napi_value FromJs(Js v) { return reinterpret_cast<napi_value>(v); }
Js ToJs(napi_value v) { return reinterpret_cast<Js>(v); }

napi_status MapJs(napi_env env, JsErrorCode ec, const char* what) {
  if (ec == JsNoError) return napi_ok;
  if (ec == JsErrorInExceptionState || ec == JsErrorScriptException) {
    nodeapi::SetLastError(env, napi_pending_exception, what);
    return napi_pending_exception;
  }
  if (ec == JsErrorInvalidArgument || ec == JsErrorNullArgument) {
    nodeapi::SetLastError(env, napi_invalid_arg, what);
    return napi_invalid_arg;
  }
  nodeapi::SetLastError(env, napi_generic_failure, what);
  return napi_generic_failure;
}

bool IsFunctionValue(napi_value v) {
  JsValueType t = JsUndefined;
  if (JsGetValueType(ToJs(v), &t) != JsNoError) return false;
  return t == JsFunction;
}

// ToObject pre-check (mirrors NodeApiObjects.cpp ToObject): JS
// null/undefined => napi_object_expected WITHOUT calling
// JsConvertValueToObject (which would throw and surface as pending
// -- the wrong status). Every other type converts (boxes). The
// converted value is borrowed for the call only (discarded after;
// no root retained -- see header record (a)).
napi_status CheckToObject(napi_env env, napi_value in) {
  JsValueType t = JsUndefined;
  JsErrorCode te = JsGetValueType(ToJs(in), &t);
  if (te != JsNoError) return MapJs(env, te, "ToObject: type query");
  if (t == JsUndefined || t == JsNull) {
    nodeapi::SetLastError(env, napi_object_expected,
                          "ToObject: null/undefined is not an object");
    return napi_object_expected;
  }
  Js out = JS_INVALID_REFERENCE;
  JsErrorCode e = JsConvertValueToObject(ToJs(in), &out);
  if (e != JsNoError) return MapJs(env, e, "ToObject");
  return napi_ok;
}

// ToString pre-check (mirrors the object pattern): JS null/undefined
// => napi_string_expected without calling the convert. Otherwise
// JsConvertValueToString; a throw (e.g. symbol, throwing toString)
// maps via §3.10 (pending). Converted value discarded (no root).
napi_status CheckToString(napi_env env, napi_value in) {
  JsValueType t = JsUndefined;
  JsErrorCode te = JsGetValueType(ToJs(in), &t);
  if (te != JsNoError) return MapJs(env, te, "ToString: type query");
  if (t == JsUndefined || t == JsNull) {
    nodeapi::SetLastError(env, napi_string_expected,
                          "ToString: null/undefined is not a string");
    return napi_string_expected;
  }
  Js out = JS_INVALID_REFERENCE;
  JsErrorCode e = JsConvertValueToString(ToJs(in), &out);
  if (e != JsNoError) return MapJs(env, e, "ToString");
  return napi_ok;
}

void FreeState(AsyncState* st) {
  if (st == nullptr) return;
  for (auto* r : st->contexts) delete r;
  for (auto* r : st->retired_contexts) delete r;
  for (auto* r : st->scopes) delete r;
  for (auto* r : st->retired_scopes) delete r;
  delete st;
}

// Current env generation (env already validated live by the caller;
// single-field read is safe). 0 if the lookup fails (dead/fabricated).
std::uint64_t CurrentGeneration(napi_env env) {
  if (env == nullptr) return 0;
  nodeapi::NodeEnv* nenv = nullptr;
  if (!nodeapi::LookupLiveEnv(static_cast<void*>(env), &nenv)) return 0;
  return nenv->generation;
}

AsyncState* FindStateLocked(void* key) {
  auto it = g_states.find(key);
  return it == g_states.end() ? nullptr : it->second;
}

// Get (and optionally create) the AsyncState for a live env. Caller
// must have validated the env live first (EntryGuard). Generation-
// mismatch stale state (address reuse) is freed and replaced.
// Returns nullptr on OOM (creation) or on absent state when
// create==false.
AsyncState* GetState(napi_env env, bool create) {
  if (env == nullptr) return nullptr;
  std::uint64_t gen = CurrentGeneration(env);
  if (gen == 0) return nullptr; // not live (caller maps to invalid_arg)
  void* key = static_cast<void*>(env);
  std::lock_guard<std::mutex> lk(g_state_mutex);
  AsyncState* st = FindStateLocked(key);
  if (st != nullptr && st->generation != gen) {
    // Stale: address reused by a new env. Free the old state under
    // the lock (deletes are native-only, no user code). Pointers
    // erased below before delete to avoid a dangling map entry if
    // delete throws (it cannot: scalar deletes).
    g_states.erase(key);
    // Release the lock before freeing would be ideal, but FreeState
    // touches only native memory and the map is already erased;
    // holding the lock across it is safe (no user code). Unlock by
    // scope only after the block: free here while holding is fine.
    AsyncState* stale = st;
    st = nullptr;
    // Free after erase but before creation; still holding the lock.
    // (delete never calls user code.)
    // Note: std::lock_guard keeps the lock until function exit if
    // we keep it in scope; to free outside the lock we would need
    // manual unlock. Native-only free under the lock is deliberate.
    FreeState(stale);
  }
  if (st != nullptr) return st;
  if (!create) return nullptr;
  AsyncState* fresh = new (std::nothrow) AsyncState();
  if (fresh == nullptr) return nullptr;
  fresh->owner = env;
  fresh->generation = gen;
  try {
    g_states[key] = fresh;
  } catch (...) {
    delete fresh;
    return nullptr;
  }
  return fresh;
}

// Pointer-identity search in a live list (never dereferences the
// probe handle). -1 if absent.
long long FindContext(const std::vector<napi_async_context>& list,
                      napi_async_context probe) {
  if (probe == nullptr) return -1;
  for (size_t i = 0; i < list.size(); ++i) {
    if (list[i] == probe) return static_cast<long long>(i);
  }
  return -1;
}

long long FindScope(const std::vector<napi_callback_scope>& list,
                    napi_callback_scope probe) {
  if (probe == nullptr) return -1;
  for (size_t i = 0; i < list.size(); ++i) {
    if (list[i] == probe) return static_cast<long long>(i);
  }
  return -1;
}

// Validate a (possibly NULL) async_context parameter against THIS
// env's live list. NULL (default) => ok. Non-null but not live here
// (dead/foreign/fabricated/destroyed) => invalid_arg. Must be called
// after EntryGuard validates the env and with a non-null AsyncState
// (state is required to hold the live list; absent state means no
// contexts were ever created for this env, so any non-null handle is
// unknown).
napi_status ValidateContextInState(napi_env env, AsyncState* st,
                                   napi_async_context ctx,
                                   const char* what) {
  if (ctx == nullptr) return napi_ok; // default context (allowed)
  if (st == nullptr) {
    nodeapi::SetLastError(env, napi_invalid_arg, what);
    return napi_invalid_arg;
  }
  long long idx = FindContext(st->contexts, ctx);
  (void)idx;
  if (idx < 0) {
    nodeapi::SetLastError(env, napi_invalid_arg, what);
    return napi_invalid_arg;
  }
  return napi_ok;
}

} /* namespace */

extern "C" {

void NodeApiAsyncContext_DestroyForEnv(napi_env env) {
  if (env == nullptr) return;
  void* key = static_cast<void*>(env);
  AsyncState* st = nullptr;
  {
    std::lock_guard<std::mutex> lk(g_state_mutex);
    AsyncState* found = FindStateLocked(key);
    if (found == nullptr) return;
    st = found;
    g_states.erase(key);
  }
  // Free outside the lock: native-only deletes, no user code, no
  // Js* calls. Safe to call during Closing with the env context
  // entered (NodeApiEnv destroy step 6); also safe after the env is
  // dead (stale-state cleanup by value, R-ENV-10: no dereference of
  // the env beyond its pointer value).
  FreeState(st);
}

size_t NodeApiAsyncContext_ContextCount(napi_env env) {
  if (env == nullptr) return 0;
  // Never dereference a dead/fabricated env value: consult the live
  // registry by value first (R-ENV-10).
  nodeapi::NodeEnv* nenv = nullptr;
  if (!nodeapi::LookupLiveEnv(static_cast<void*>(env), &nenv)) return 0;
  void* key = static_cast<void*>(env);
  std::lock_guard<std::mutex> lk(g_state_mutex);
  AsyncState* st = FindStateLocked(key);
  if (st == nullptr) return 0;
  if (st->generation != nenv->generation) return 0; // stale
  return st->contexts.size();
}

size_t NodeApiAsyncContext_ScopeDepth(napi_env env) {
  if (env == nullptr) return 0;
  nodeapi::NodeEnv* nenv = nullptr;
  if (!nodeapi::LookupLiveEnv(static_cast<void*>(env), &nenv)) return 0;
  void* key = static_cast<void*>(env);
  std::lock_guard<std::mutex> lk(g_state_mutex);
  AsyncState* st = FindStateLocked(key);
  if (st == nullptr) return 0;
  if (st->generation != nenv->generation) return 0; // stale
  return st->scopes.size();
}

size_t NodeApiAsyncContext_TotalStates(void) {
  std::lock_guard<std::mutex> lk(g_state_mutex);
  return g_states.size();
}

/* ------------------------------------------------------------------ */
/* napi_async_init / napi_async_destroy                                */
/* ------------------------------------------------------------------ */

napi_status NAPI_CDECL napi_async_init(napi_env env,
                                       napi_value async_resource,
                                       napi_value async_resource_name,
                                       napi_async_context* result) {
  try {
    if (result == nullptr) {
      if (env != nullptr) {
        nodeapi::SetLastError(env, napi_invalid_arg,
                              "napi_async_init: result is NULL");
      }
      return napi_invalid_arg;
    }
    if (async_resource == nullptr || async_resource_name == nullptr) {
      if (env != nullptr) {
        nodeapi::SetLastError(env, napi_invalid_arg,
                              "napi_async_init: NULL arg");
      }
      return napi_invalid_arg;
    }
    // C Yes: executes with pending. Strict Alive-only (creates).
    nodeapi::EntryGuard guard(env, /*allowClosing=*/false,
                              /*allowPending=*/true, /*needContext=*/true);
    if (!guard.ok()) return guard.status();
    // Bookkeeping-only validation (loud no-op: coerced values are
    // discarded; no async_hooks propagation, no JS roots retained).
    napi_status s = CheckToObject(env, async_resource);
    if (s != napi_ok) return s;
    s = CheckToString(env, async_resource_name);
    if (s != napi_ok) return s;
    AsyncState* st = GetState(env, /*create=*/true);
    if (st == nullptr) {
      nodeapi::SetLastError(env, napi_generic_failure,
                            "napi_async_init: state OOM");
      return napi_generic_failure;
    }
    auto* rec = new (std::nothrow) napi_async_context__();
    if (rec == nullptr) {
      nodeapi::SetLastError(env, napi_generic_failure,
                            "napi_async_init: record OOM");
      return napi_generic_failure;
    }
    rec->owner = env;
    rec->generation = st->generation;
    rec->id = st->next_context_id++;
    rec->destroyed = false;
    try {
      st->contexts.push_back(rec);
    } catch (...) {
      delete rec;
      nodeapi::SetLastError(env, napi_generic_failure,
                            "napi_async_init: list OOM");
      return napi_generic_failure;
    }
    *result = rec;
    nodeapi::ClearLastError(env);
    return napi_ok;
  } catch (...) {
    if (env != nullptr) {
      nodeapi::SetLastError(env, napi_generic_failure,
                            "napi_async_init: C++ exception");
    }
    return napi_generic_failure;
  }
}

napi_status NAPI_CDECL napi_async_destroy(napi_env env,
                                          napi_async_context async_context) {
  try {
    if (async_context == nullptr) {
      if (env != nullptr) {
        nodeapi::SetLastError(env, napi_invalid_arg,
                              "napi_async_destroy: context is NULL");
      }
      return napi_invalid_arg;
    }
    // C Yes: executes with pending. Teardown-compatible (Closing ok).
    nodeapi::EntryGuard guard(env, /*allowClosing=*/true,
                              /*allowPending=*/true, /*needContext=*/false);
    if (!guard.ok()) return guard.status();
    AsyncState* st = GetState(env, /*create=*/false);
    if (st == nullptr) {
      nodeapi::SetLastError(env, napi_invalid_arg,
                            "napi_async_destroy: unknown context");
      return napi_invalid_arg;
    }
    long long idx = FindContext(st->contexts, async_context);
    if (idx < 0) {
      // Foreign / dead / already-destroyed / fabricated: by-value
      // search never dereferenced the handle.
      nodeapi::SetLastError(env, napi_invalid_arg,
                            "napi_async_destroy: unknown context");
      return napi_invalid_arg;
    }
    napi_async_context rec = st->contexts[static_cast<size_t>(idx)];
    st->contexts.erase(st->contexts.begin() +
                       static_cast<std::ptrdiff_t>(idx));
    if (rec != nullptr) {
      rec->destroyed = true;
      try {
        st->retired_contexts.push_back(rec);
      } catch (...) {
        // Retire-cache OOM: free inline (double-destroy still maps
        // to invalid_arg via the absent live entry; address-reuse
        // aliasing in this OOM-only path is accepted and documented
        // in the header record (c)).
        delete rec;
      }
    }
    nodeapi::ClearLastError(env);
    return napi_ok;
  } catch (...) {
    if (env != nullptr) {
      nodeapi::SetLastError(env, napi_generic_failure,
                            "napi_async_destroy: C++ exception");
    }
    return napi_generic_failure;
  }
}

/* ------------------------------------------------------------------ */
/* napi_open_callback_scope / napi_close_callback_scope                */
/* ------------------------------------------------------------------ */

napi_status NAPI_CDECL napi_open_callback_scope(
    napi_env env, napi_value resource_object,
    napi_async_context context, napi_callback_scope* result) {
  try {
    if (result == nullptr) {
      if (env != nullptr) {
        nodeapi::SetLastError(env, napi_invalid_arg,
                              "napi_open_callback_scope: result is NULL");
      }
      return napi_invalid_arg;
    }
    if (resource_object == nullptr) {
      if (env != nullptr) {
        nodeapi::SetLastError(env, napi_invalid_arg,
                              "napi_open_callback_scope: resource is NULL");
      }
      return napi_invalid_arg;
    }
    // C Yes: executes with pending. Strict Alive-only (creates).
    // No JS type check (C column); no context enter needed (native
    // record only).
    nodeapi::EntryGuard guard(env, /*allowClosing=*/false,
                              /*allowPending=*/true, /*needContext=*/false);
    if (!guard.ok()) return guard.status();
    // Validate the context snapshot (NULL = default). No ALS enter:
    // the snapshot is stored by value and never dereferenced.
    AsyncState* st = GetState(env, /*create=*/true);
    if (st == nullptr) {
      nodeapi::SetLastError(env, napi_generic_failure,
                            "napi_open_callback_scope: state OOM");
      return napi_generic_failure;
    }
    napi_status vs =
        ValidateContextInState(env, st, context,
                               "napi_open_callback_scope: unknown context");
    if (vs != napi_ok) return vs;
    auto* rec = new (std::nothrow) napi_callback_scope__();
    if (rec == nullptr) {
      nodeapi::SetLastError(env, napi_generic_failure,
                            "napi_open_callback_scope: record OOM");
      return napi_generic_failure;
    }
    rec->owner = env;
    rec->generation = st->generation;
    rec->async_context = context; // snapshot value only
    rec->id = st->next_scope_id++;
    try {
      st->scopes.push_back(rec);
    } catch (...) {
      delete rec;
      nodeapi::SetLastError(env, napi_generic_failure,
                            "napi_open_callback_scope: stack OOM");
      return napi_generic_failure;
    }
    *result = rec;
    nodeapi::ClearLastError(env);
    return napi_ok;
  } catch (...) {
    if (env != nullptr) {
      nodeapi::SetLastError(env, napi_generic_failure,
                            "napi_open_callback_scope: C++ exception");
    }
    return napi_generic_failure;
  }
}

napi_status NAPI_CDECL napi_close_callback_scope(napi_env env,
                                                 napi_callback_scope scope) {
  try {
    if (scope == nullptr) {
      if (env != nullptr) {
        nodeapi::SetLastError(env, napi_invalid_arg,
                              "napi_close_callback_scope: scope is NULL");
      }
      return napi_invalid_arg;
    }
    // C Yes: executes with pending. Teardown-compatible (Closing ok).
    nodeapi::EntryGuard guard(env, /*allowClosing=*/true,
                              /*allowPending=*/true, /*needContext=*/false);
    if (!guard.ok()) return guard.status();
    AsyncState* st = GetState(env, /*create=*/false);
    if (st == nullptr) {
      nodeapi::SetLastError(env, napi_invalid_arg,
                            "napi_close_callback_scope: unknown scope");
      return napi_invalid_arg;
    }
    long long idx = FindScope(st->scopes, scope);
    if (idx < 0) {
      // Foreign / already-closed / fabricated: invalid_arg (N1.2
      // CloseImpl pattern: unknown handles are invalid_arg without
      // dereferencing).
      nodeapi::SetLastError(env, napi_invalid_arg,
                            "napi_close_callback_scope: unknown scope");
      return napi_invalid_arg;
    }
    const bool is_top =
        (static_cast<size_t>(idx) == st->scopes.size() - 1);
    napi_callback_scope rec = st->scopes[static_cast<size_t>(idx)];
    // N1.2 discipline (R-SC-6 by analogy): remove ONLY the named
    // scope; the rest of the stack is intact. Release happens on
    // every path (retire = deferred-free until teardown).
    st->scopes.erase(st->scopes.begin() +
                     static_cast<std::ptrdiff_t>(idx));
    try {
      st->retired_scopes.push_back(rec);
    } catch (...) {
      delete rec; // retire-cache OOM (see destroy path)
    }
    if (!is_top) {
      nodeapi::SetLastError(
          env, napi_callback_scope_mismatch,
          "napi_close_callback_scope: not innermost");
      return napi_callback_scope_mismatch;
    }
    nodeapi::ClearLastError(env);
    return napi_ok;
  } catch (...) {
    if (env != nullptr) {
      nodeapi::SetLastError(env, napi_generic_failure,
                            "napi_close_callback_scope: C++ exception");
    }
    return napi_generic_failure;
  }
}

/* ------------------------------------------------------------------ */
/* napi_make_callback                                                  */
/* ------------------------------------------------------------------ */

napi_status NAPI_CDECL napi_make_callback(napi_env env,
                                          napi_async_context async_context,
                                          napi_value recv, napi_value func,
                                          size_t argc,
                                          const napi_value* argv,
                                          napi_value* result) {
  try {
    if (recv == nullptr) {
      if (env != nullptr) {
        nodeapi::SetLastError(env, napi_invalid_arg,
                              "napi_make_callback: recv is NULL");
      }
      return napi_invalid_arg;
    }
    if (func == nullptr) {
      if (env != nullptr) {
        nodeapi::SetLastError(env, napi_invalid_arg,
                              "napi_make_callback: func is NULL");
      }
      return napi_invalid_arg;
    }
    if (argc > 0 && argv == nullptr) {
      if (env != nullptr) {
        nodeapi::SetLastError(env, napi_invalid_arg,
                              "napi_make_callback: argv is NULL");
      }
      return napi_invalid_arg;
    }
    // C No: gated (pending => pending_exception without executing).
    // Strict Alive-only. needContext enters the ENV context
    // (plan §3.2); the async_context itself is validated but NOT
    // entered for ALS (loud no-op).
    nodeapi::EntryGuard guard(env, /*allowClosing=*/false,
                              /*allowPending=*/false, /*needContext=*/true);
    if (!guard.ok()) return guard.status();
    // Validate the async context (NULL = default). Does not create
    // state: a never-initialized env has no live contexts, so a
    // non-null handle must fail without creating an empty state.
    AsyncState* st = GetState(env, /*create=*/false);
    napi_status vs = ValidateContextInState(
        env, st, async_context, "napi_make_callback: unknown context");
    if (vs != napi_ok) return vs;
    // Pre-checks (C): recv ToObject (object_expected), func
    // CHECK_TO_FUNCTION (invalid_arg, NOT function_expected).
    napi_status ps = CheckToObject(env, recv);
    if (ps != napi_ok) return ps;
    if (!IsFunctionValue(func)) {
      nodeapi::SetLastError(env, napi_invalid_arg,
                            "napi_make_callback: not a function");
      return napi_invalid_arg;
    }
    // HIGH-1 wrap guard (as in napi_call_function): argc+1 must be
    // exact in unsigned short.
    if (argc > 65534) {
      nodeapi::SetLastError(env, napi_generic_failure,
                            "napi_make_callback: too many args");
      return napi_generic_failure;
    }
    // Context enter (env) + handle scope (card sequence): open the
    // hidden handle scope for the call duration.
    napi_handle_scope hidden = nullptr;
    napi_status os = NodeApiScopes_OpenImplicit(env, &hidden);
    if (os != napi_ok || hidden == nullptr) {
      nodeapi::SetLastError(env, os != napi_ok ? os : napi_generic_failure,
                            "napi_make_callback: handle scope");
      return os != napi_ok ? os : napi_generic_failure;
    }
    // Call equivalent to napi_call_function (JsCallFunction with
    // arguments[0]==this layout, oracle JsRTApiTest.cpp:734-866).
    std::vector<Js> args;
    try {
      args.reserve(argc + 1);
      args.push_back(ToJs(recv));
      for (size_t i = 0; i < argc; ++i) args.push_back(ToJs(argv[i]));
    } catch (...) {
      (void)napi_close_handle_scope(env, hidden);
      nodeapi::SetLastError(env, napi_generic_failure,
                            "napi_make_callback: args OOM");
      return napi_generic_failure;
    }
    Js jsResult = JS_INVALID_REFERENCE;
    Js* resultPtr = nullptr;
    if (result != nullptr) resultPtr = &jsResult;
    // JsCallFunction with a null result pointer still runs the
    // callee (discard mode): use a scratch slot so the engine can
    // report errors the same way.
    Js scratch = JS_INVALID_REFERENCE;
    if (resultPtr == nullptr) resultPtr = &scratch;
    JsErrorCode e = JsCallFunction(ToJs(func), args.data(),
                                   static_cast<unsigned short>(argc + 1),
                                   resultPtr);
    if (e != JsNoError) {
      (void)napi_close_handle_scope(env, hidden);
      return MapJs(env, e, "napi_make_callback: call");
    }
    // Thunk imbalance/throw can leave a pending exception with a
    // successful JsCallFunction return (SetMismatchPending sets the
    // exception and returns undefined). Treat any pending as
    // pending_exception (result untouched), like the thunk's discard
    // path (R-CB-4/AMB-CB-2).
    {
      bool has = false;
      JsErrorCode hq = JsHasException(&has);
      if (hq == JsNoError && has) {
        (void)napi_close_handle_scope(env, hidden);
        nodeapi::SetLastError(env, napi_pending_exception,
                              "napi_make_callback: pending after call");
        return napi_pending_exception;
      }
    }
    if (resultPtr == &scratch) {
      // Discard mode: callee ran, value dropped. Still drain
      // microtasks before returning (pump contract applies even
      // when the result is discarded).
      size_t pumped = 0;
      napi_status pumpSt =
          NodeApiPromise_Pump(env, /*max_tasks=*/0, &pumped);
      (void)napi_close_handle_scope(env, hidden);
      if (pumpSt != napi_ok) return pumpSt;
      nodeapi::ClearLastError(env);
      return napi_ok;
    }
    if (jsResult == JS_INVALID_REFERENCE) {
      (void)napi_close_handle_scope(env, hidden);
      nodeapi::SetLastError(env, napi_generic_failure,
                            "napi_make_callback: null result value");
      return napi_generic_failure;
    }
    // Publish-path only: root into the hidden scope first (R1).
    // A native-thunk callee already rooted its return into this
    // scope (extra root is harmless: both are released at hidden
    // close; a plain-JS callee needs this root).
    napi_status rs =
        NodeApiScopes_RootIntoInnermost(env, FromJs(jsResult));
    if (rs != napi_ok) {
      (void)napi_close_handle_scope(env, hidden);
      return rs;
    }
    // Microtask drain via the host pump (PR-8 provider-queue case).
    // Unbounded until quiet (max_tasks==0 per NodeApiPromise_Pump);
    // a task throw maps to pending (result not yet published, so
    // the out-param stays untouched and no parent root is taken).
    {
      size_t pumped = 0;
      napi_status pumpSt =
          NodeApiPromise_Pump(env, /*max_tasks=*/0, &pumped);
      if (pumpSt != napi_ok) {
        (void)napi_close_handle_scope(env, hidden);
        return pumpSt;
      }
    }
    // Move the return into the caller's scope BEFORE closing the
    // hidden scope (R-SC-9 / §3.8 step 4).
    napi_status ms = NodeApiScopes_RootIntoCallerScope(env, FromJs(jsResult));
    if (ms != napi_ok) {
      (void)napi_close_handle_scope(env, hidden);
      return ms;
    }
    (void)napi_close_handle_scope(env, hidden);
    *result = FromJs(jsResult);
    nodeapi::ClearLastError(env);
    return napi_ok;
  } catch (...) {
    if (env != nullptr) {
      nodeapi::SetLastError(env, napi_generic_failure,
                            "napi_make_callback: C++ exception");
    }
    return napi_generic_failure;
  }
}

} /* extern "C" */
