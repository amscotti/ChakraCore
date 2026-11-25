//-------------------------------------------------------------------------------------------------------
// Copyright (c) ChakraCore Project Contributors. All rights reserved.
// Licensed under the MIT license. See LICENSE.txt file in the project root for full license information.
//-------------------------------------------------------------------------------------------------------
//
// Node-API Buffer policy implementation (N4.2). See NodeApiBuffers.h for
// the marker design, the explicit non-conformances (no Buffer global,
// no Buffer.prototype methods, no pooling), and the status discipline.
//
// Implementation notes (Node v22.22.3 src/node_api.cc Buffer::New /
// doc/api/n-api.md, via doc/node-api-status-mapping.md Buffer rows):
//   - All public entries follow validated-first (NULL binding args ->
//     napi_invalid_arg before anything else), then the shared EntryGuard
//     (Closing -> generic_failure, gated creates -> napi_pending_exception
//     without executing, context entered via nodeapi::ContextGuard).
//     Creates are C-column No (gated); is/get are Yes (execute with
//     pending, pending preserved).
//   - Buffers are Uint8Array views (JsCreateTypedArray with
//     JsArrayTypeUint8) over an ArrayBuffer.
//     GC'd: JsCreateArrayBuffer.
//     External: JsCreateExternalArrayBuffer with NULL finalizer (storage
//     aliases caller's data), the user's finalizer is attached to the
//     view via napi_add_finalizer (WrapRecord queue, section 3.7).
//   - Marker: provider-private Symbol per env (global map keyed by
//     generation, see below). Defined via JsObjectDefineProperty with
//     a real descriptor object {value:true, writable:false,
//     enumerable:false, configurable:false} so it is hidden.
//   - is_buffer: own-property + boolean true check (primitive filter).
//   - get_buffer_info: marker check, then napi_get_typedarray_info
//     composition (N3.1), type must be napi_uint8_array, length is the
//     element count (== byte length for Uint8), data is the storage
//     borrow (R4).
//   - Drain: every entry calls nodeapi_wrap_drain(env) after the guard
//     (cheap empty check; see NodeApiWrap.h R-WRAP-8). This runs any
//     queued external-buffer finalizers (via WrapRecord queue) before
//     the new work, matching N3.2 drain-point semantics. The lock is
//     never held across user finalizers (drain moves the queue out).
//
// Ownership records (memory-management.md section 3; plan section 3.4 R1/R4, section 3.7):
//
//   External Buffer records (napi_create_external_buffer with
//   non-NULL finalize_cb):
//     Resource / creation call: WrapRecord heap allocation (new) in
//       NodeApiWrap.cpp CreateRecord path, created by napi_add_finalizer
//       on the Buffer's Uint8Array view. The WrapRecord holds
//       {env, user_data = caller's external data, finalize =
//       caller's finalize_cb, hint}. The view object itself is the
//       side-table key; the before-collect trampoline is the GC signal.
//     Domain and allocator: provider native heap (C++ new/delete via
//       the provider allocator; backing lifetime == process). Never
//       Recycler. The JsCreateExternalArrayBuffer Recycler object
//       itself aliases the caller's bytes (no copy).
//     Physical allocation: one WrapRecord per external Buffer with a
//       finalizer (plus the JsrtExternalArrayBuffer/Recycler objects
//       for the ArrayBuffer and the TypedArray view). When
//       finalize_cb == NULL, no WrapRecord is created (no finalizer to
//       call; the external storage is still aliased).
//     Owner; transfer point: The provider (NodeApiWrap TU) owns the
//       WrapRecord after napi_add_finalizer succeeds, published to the
//       side table at that point. The JsCreateExternalArrayBuffer
//       transfer is the ArrayBuffer's storage alias (with NULL
//       finalizer, the engine holds no finalizer state to free). The
//       Buffer view (TypedArray) is owned by the provider scope stack
//       (RootIntoInnermost) until scope close; its GC edge keeps the
//       ArrayBuffer alive.
//     Borrow invalidation: GC finalization (the view's before-collect
//       trampoline enqueues the WrapRecord; drain deletes it). Detach
//       of the backing ArrayBuffer does NOT drop the user's finalizer
//       (the finalizer is on the view, not on the ArrayBuffer), so a
//       detached Buffer's finalizer still fires when the view is
//       collected (unlike N3.1's external ArrayBuffer detached-leak
//       residual; see below). Runtime disposal invalidates everything;
//       destroy backstop runs remaining live finalizers once.
//     Potential user-code/reentrancy: GC trampoline never calls user
//       code (D4): it only CASes the record LIVE->QUEUED and pushes
//       onto the WrapState queue under mutex. Drain on env thread
//       invokes napi_finalize(env, data, hint) with a live env and the
//       context entered (EntryGuard). The user finalizer may call
//       provider APIs (which re-enter EntryGuard; the queue was popped
//       before invoking, so reentrant drain sees the next item).
//     Thread/context: trampoline on arbitrary GC thread, no context,
//       only native queue mutation under mutex (never across user
//       code). Drain on owner thread with env context entered.
//     Release on success/partial failure/exception/OOM/cancel/shutdown:
//       Exactly once via Wrap drain (delete after invocation) or, if
//       the env is already dead at drain time, delete without
//       invocation (stale generation guard in NodeApiWrap, no UAF).
//       Shutdown: records for Buffers still live at destroy die with
//       the runtime; nodeapi_wrap_teardown backstop runs them once at
//       step 6. If napi_add_finalizer fails after the external
//       ArrayBuffer + view + marker were created, the unpublished view
//       is discarded (scope roots never published); its ArrayBuffer's
//       external data remains user-owned and no user finalizer runs
//       (the caller did not publish the value).
//     Memory accounting/capacity: one WrapRecord per live external
//       Buffer with a finalizer; queue holds at most live uncollected
//       externals (bounded by JS heap). OOM in record alloc => the
//       add_finalizer path (controlled failure), create returns that
//       status, nothing published.
//     Tests: test/nodeapi/probes/buffers_probe.cpp external finalizer
//       fires once on collect (GC via JsCollectGarbage + next napi
//       entry drain; 3 iterations in probe).
//     Residual uncertainty: finalizer after env destroy not invoked
//       (leak of user external_data, not a UAF; probe does not cover
//       post-destroy collect); Linux x64 not claimed.
//
//   R4 storage borrows (get_buffer_info data pointer, create_* data
//   out-params):
//     Resource: raw BYTE* + length from JsGetTypedArrayStorage /
//       JsGetArrayBufferStorage (borrow, not root).
//     Domain: Recycler heap (ArrayBuffer/TypedArray storage) or, for
//       external Buffers, the caller's `data` buffer aliased as the
//       ArrayBuffer storage.
//     Owner: The Buffer's TypedArray view (already rooted by the
//       caller's scope). The provider takes transient ScratchRoots
//       (JsAddRef) for the borrow's window (R1) so GC cannot collect
//       the view during the JsRT calls + any JS-executing property
//       gets. The returned data pointer is NOT rooted; its lifetime is
//       the caller's responsibility (R4: root the Buffer for the
//       borrow's life and revalidate after any JS call that can run).
//       In get_buffer_info the provider re-reads storage after the
//       napi_get_typedarray_info call (which may run JS via
//       detach/detached getters? No JS user code, but the borrow is
//       still revalidated) and returns the revalidated pointer.
//     Invalidation: detach (ArrayBuffer::Detach via
//       napi_detach_arraybuffer or JS transfer), GC, resize
//       (resizable). After a JS call that detaches, the borrow is
//       invalid; the probe verifies a fresh get_buffer_info after
//       such a call observes NULL/0 (via the N3.1 composition, which
//       already handles the detached-view tagged-int case).
//     Release: ScratchRoots are RAII (JsRelease at return); the
//       caller's scope owns the long-term root until scope close.
//     Tests: buffers_probe data-pointer visible from JS (native writes
//       via data pointer -> JS reads; JS writes -> native reads via
//       fresh get_buffer_info).
//     OOM: ScratchRoot JsAddRef never throws; vector OOM (scratch
//       buffers) maps to generic_failure.
//
//   Per-context shared marker Symbol (ContextBufferSymbol) & per-env
//   lease (BufferState):
//     Resource / creation call:
//       (a) ContextBufferSymbol heap allocation (new) in
//           NodeApiBuffers.cpp GetOrCreateState, inserted into
//           g_context_symbols[nenv->context]. Holds the shared Recycler
//           JsValueRef Symbol ("chakra-nodeapi-buffer", rooted once via
//           JsAddRef), its cached JsPropertyIdRef, and `ref_count`.
//       (b) BufferState heap allocation (new) per env generation in
//           GetOrCreateState, inserted into g_env_states[nenv->generation],
//           holding that env's single lease on its context's
//           ContextBufferSymbol (`ref_count` += 1).
//     Domain and allocator: provider native heap (C++ new/delete) for
//       both ContextBufferSymbol and BufferState; Recycler heap for the
//       single shared Symbol JsValueRef (never freed directly; released
//       only via JsRelease).
//     Physical allocation: at most one ContextBufferSymbol + one rooted
//       Symbol per active JsContextRef that uses Buffers; one BufferState
//       lease per env generation that uses Buffers.
//     Owner; transfer/publication point; remaining aliases:
//       The provider (this TU) owns g_context_symbols and g_env_states
//       under g_buf_mutex. ContextBufferSymbol is published to
//       g_context_symbols[ctx] outside any JS call; BufferState is
//       published to g_env_states[gen] and registered with the env's
//       step-3 cleanup hook (BuffersCleanupHook). Sibling envs sharing
//       the same JsContextRef increment `ctx_sym->ref_count` and borrow
//       the same Symbol/PropertyIdRef without creating a second Symbol.
//     Borrow invalidation: env destroy (per-env sync cleanup hook
//       BuffersCleanupHook at step 3, or idempotent explicit
//       nodeapi_buffers_teardown which also unregisters the hook).
//       Generation keying survives env address reuse.
//     Potential user-code/reentrancy: GetOrCreateState creates the JS
//       string/Symbol outside g_buf_mutex with the env context entered;
//       no lock is held across any JS/JsRT call.
//     Thread/context allowed: GetOrCreateState / MarkBuffer /
//       IsBufferMarker run on the owner thread with the env context
//       entered. ReleaseBufferState runs on the owner thread; when
//       ref_count hits 0, ContextGuard enters the last env's context
//       and verifies JsGetCurrentContext() == cs->context before
//       JsRelease(cs->symbol) and removing g_context_symbols[ctx].
//     Release on success/partial failure/exception/OOM/cancel/shutdown:
//       Creation/map/hook-registration failure rolls back g_env_states,
//       decrements ref_count (releasing Symbol + ContextBufferSymbol if
//       0), and deletes the BufferState inline (nothing leaked).
//       On shutdown, each sibling env releases its lease independently;
//       the Symbol root stays alive while >= 1 sibling env uses that
//       context and is JsRelease'd exactly once when the final sibling
//       tears down (while its context is still entered, before
//       JsDisposeRuntime).
//     Memory accounting/capacity: O(1) per live env + O(1) per live
//       context using Buffers.
//     Tests: cross-addon Buffer sharing + multi-env teardown on shared
//       context (v10_multi_addon_probe / buffers_probe).
//
//   Returned napi_values (create_* Buffers):
//     Rooted into innermost scope via NodeApiScopes_RootIntoInnermost
//     (R-SC-1). On rooting failure the out-param is untouched and the
//     unpublished view is discarded (its ArrayBuffer + view will be
//     collected; if an external WrapRecord was created, its
//     before-collect will enqueue and drain will run the user
//     finalizer for the unpublished buffer -- leak of user data is
//     avoided at the cost of calling a finalizer for a value the
//     caller never saw; see the add_finalizer failure note above for
//     the opposite failure mode where no record was created and no
//     finalizer runs).
//
// Potential user-code points (R4 / section 6):
//   - JsObjectDefineProperty with a symbol key and a plain descriptor
//     object runs no user code (the descriptor is a fresh plain
//     object; the TypedArray has no proxy traps). The provider holds
//     ScratchRoots for subjects.
//   - JsGetTypedArrayStorage / JsGetArrayBufferStorage execute no JS.
//   - napi_add_finalizer's before-collect registration and weak-
//     reference creation execute no JS.
//   - External finalizer user code runs only at drain (env thread).
//
// Language: C++17 (GNU++17 via `zig c++`, matching Waves 1-3) behind the
// extern "C" napi_* surface (PR-4 decision record: C++ through provider
// v1). No C++ exception crosses extern "C": every allocation is
// nothrow/try-catch wrapped and maps to generic_failure.

#include "NodeApiBuffers.h"

#include "NodeApiEnv.h"
#include "NodeApiScopes.h"
#include "NodeApiWrap.h"
#include "ChakraCommon.h"
#include "ChakraCore.h"

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <limits>
#include <mutex>
#include <new>
#include <unordered_map>

namespace {

JsValueRef ToJs(napi_value v) { return reinterpret_cast<JsValueRef>(v); }
napi_value FromJs(JsValueRef v) { return reinterpret_cast<napi_value>(v); }

napi_status MapJsError(napi_env env, JsErrorCode err, const char* what) {
  switch (err) {
    case JsNoError:
      return napi_ok;
    case JsErrorInExceptionState:
    case JsErrorScriptException:
      nodeapi::SetLastError(env, napi_pending_exception, what);
      return napi_pending_exception;
    case JsErrorInvalidArgument:
    case JsErrorNullArgument:
      nodeapi::SetLastError(env, napi_invalid_arg, what);
      return napi_invalid_arg;
    case JsErrorOutOfMemory:
      nodeapi::SetLastError(env, napi_generic_failure, what);
      return napi_generic_failure;
    default:
      nodeapi::SetLastError(env, napi_generic_failure, what);
      return napi_generic_failure;
  }
}

class ScratchRoot {
 public:
  explicit ScratchRoot(JsValueRef v) : v_(v), owned_(false) {
    if (v_ != JS_INVALID_REFERENCE) {
      if (JsAddRef(v_, nullptr) == JsNoError) owned_ = true;
    }
  }
  ~ScratchRoot() { reset(); }
  ScratchRoot(const ScratchRoot&) = delete;
  ScratchRoot& operator=(const ScratchRoot&) = delete;
  ScratchRoot(ScratchRoot&& other) noexcept : v_(other.v_), owned_(other.owned_) {
    other.v_ = JS_INVALID_REFERENCE;
    other.owned_ = false;
  }
  ScratchRoot& operator=(ScratchRoot&& other) noexcept {
    if (this != &other) {
      reset();
      v_ = other.v_;
      owned_ = other.owned_;
      other.v_ = JS_INVALID_REFERENCE;
      other.owned_ = false;
    }
    return *this;
  }
  JsValueRef get() const { return v_; }
  void reset() {
    if (owned_) {
      (void)JsRelease(v_, nullptr);
      owned_ = false;
    }
    v_ = JS_INVALID_REFERENCE;
  }

 private:
  JsValueRef v_;
  bool owned_;
};

napi_status RootAndPublish(napi_env env, JsValueRef value, napi_value* result) {
  if (value == JS_INVALID_REFERENCE) {
    nodeapi::SetLastError(env, napi_generic_failure, "null value to publish");
    return napi_generic_failure;
  }
  napi_status rs = NodeApiScopes_RootIntoInnermost(env, FromJs(value));
  if (rs != napi_ok) {
    nodeapi::SetLastError(env, rs, "rooting returned value failed");
    return rs;
  }
  *result = FromJs(value);
  return napi_ok;
}

napi_status ValidateSizeToUInt(napi_env env, size_t v, unsigned int* out,
                               const char* what) {
  if (v > std::numeric_limits<unsigned int>::max()) {
    nodeapi::SetLastError(env, napi_invalid_arg, what);
    return napi_invalid_arg;
  }
  *out = static_cast<unsigned int>(v);
  return napi_ok;
}

// --- Per-context shared marker Symbol + per-env lease (section 3.9) ---
//
// The hidden marker Symbol is created once per JsContextRef (via
// JsCreateSymbol, never Symbol.for, never exposed) and refcount-shared
// across sibling napi_env instances bound to that JsContextRef, so a
// Buffer created by one addon is recognized by sibling addons. Each env
// holds one BufferState lease keyed by env generation in g_env_states;
// BuffersCleanupHook (step 3) and nodeapi_buffers_teardown release that
// lease idempotently, and the last lease on a context JsRelease's the
// Symbol while that env's context is current.

struct ContextBufferSymbol {
  JsContextRef context = JS_INVALID_REFERENCE;
  JsValueRef symbol = JS_INVALID_REFERENCE;
  JsPropertyIdRef symPid = JS_INVALID_REFERENCE;
  size_t ref_count = 0;
};

struct BufferState {
  napi_env env = nullptr;
  uint64_t generation = 0;
  ContextBufferSymbol* ctx_sym = nullptr;
};

std::mutex g_buf_mutex;
std::unordered_map<JsContextRef, ContextBufferSymbol*> g_context_symbols;
std::unordered_map<uint64_t, BufferState*> g_env_states;

void ReleaseBufferState(BufferState* st) {
  if (st == nullptr) return;
  ContextBufferSymbol* cs_to_release = nullptr;
  {
    std::lock_guard<std::mutex> lk(g_buf_mutex);
    auto it = g_env_states.find(st->generation);
    if (it != g_env_states.end() && it->second == st) {
      g_env_states.erase(it);
    }
    ContextBufferSymbol* cs = st->ctx_sym;
    st->ctx_sym = nullptr;
    if (cs != nullptr) {
      if (cs->ref_count > 0) cs->ref_count--;
      if (cs->ref_count == 0) {
        auto cit = g_context_symbols.find(cs->context);
        if (cit != g_context_symbols.end() && cit->second == cs) {
          g_context_symbols.erase(cit);
        }
        cs_to_release = cs;
      }
    }
  }
  if (cs_to_release != nullptr) {
    nodeapi::ContextGuard guard(st->env);
    (void)guard.entered();
    JsContextRef cur = JS_INVALID_REFERENCE;
    if (cs_to_release->symbol != JS_INVALID_REFERENCE &&
        JsGetCurrentContext(&cur) == JsNoError &&
        cur == cs_to_release->context) {
      (void)JsRelease(cs_to_release->symbol, nullptr);
    }
    delete cs_to_release;
  }
  delete st;
}

// Destroy step 3 cleanup hook (self-contained teardown, no
// NodeApiEnv.cpp edit needed): installed on first Buffer use per env
// via napi_add_env_cleanup_hook. Runs with the env context entered
// (destroy's ContextGuard) in Closing state, releasing the env's lease
// (and JsRelease'ing the Symbol when the last sibling env on that
// context tears down).
void BuffersCleanupHook(void* arg) {
  auto* st = static_cast<BufferState*>(arg);
  if (st == nullptr) return;
  ReleaseBufferState(st);
}

napi_status GetOrCreateState(napi_env env, BufferState** out) {
  *out = nullptr;
  nodeapi::NodeEnv* nenv = nullptr;
  if (!nodeapi::LookupLiveEnvForOwner(static_cast<void*>(env), &nenv)) {
    return napi_invalid_arg;
  }
  uint64_t gen = nenv->generation;
  JsContextRef ctx = nenv->context;
  bool has_context_symbol = false;
  BufferState* reused_st = nullptr;
  {
    std::lock_guard<std::mutex> lk(g_buf_mutex);
    auto it = g_env_states.find(gen);
    if (it != g_env_states.end()) {
      *out = it->second;
      return napi_ok;
    }
    auto cit = g_context_symbols.find(ctx);
    if (cit != g_context_symbols.end() && cit->second != nullptr) {
      has_context_symbol = true;
      reused_st = new (std::nothrow) BufferState();
      if (reused_st == nullptr) {
        nodeapi::SetLastError(env, napi_generic_failure, "buffers: state OOM");
        return napi_generic_failure;
      }
      reused_st->env = env;
      reused_st->generation = gen;
      reused_st->ctx_sym = cit->second;
      try {
        g_env_states[gen] = reused_st;
      } catch (...) {
        delete reused_st;
        nodeapi::SetLastError(env, napi_generic_failure,
                              "buffers: state map OOM");
        return napi_generic_failure;
      }
      cit->second->ref_count++;
    }
  }
  if (has_context_symbol) {
    napi_status hs = napi_add_env_cleanup_hook(
        reinterpret_cast<node_api_basic_env>(env), BuffersCleanupHook,
        reused_st);
    if (hs != napi_ok) {
      ReleaseBufferState(reused_st);
      return hs;
    }
    *out = reused_st;
    return napi_ok;
  }

  // Slow path: first Buffer use on this JsContextRef. Create the Symbol
  // outside g_buf_mutex (JS calls must not hold the lock).
  BufferState* st = new (std::nothrow) BufferState();
  if (st == nullptr) {
    nodeapi::SetLastError(env, napi_generic_failure, "buffers: state OOM");
    return napi_generic_failure;
  }
  st->env = env;
  st->generation = gen;
  const char* descText = "chakra-nodeapi-buffer";
  JsValueRef descStr = JS_INVALID_REFERENCE;
  JsErrorCode ec = JsCreateString(descText, std::strlen(descText), &descStr);
  if (ec != JsNoError) {
    delete st;
    return MapJsError(env, ec, "buffers: create description");
  }
  ScratchRoot descRoot(descStr);
  JsValueRef sym = JS_INVALID_REFERENCE;
  ec = JsCreateSymbol(descStr, &sym);
  if (ec != JsNoError) {
    delete st;
    return MapJsError(env, ec, "buffers: create symbol");
  }
  ScratchRoot symRoot(sym);
  ec = JsAddRef(sym, nullptr);
  if (ec != JsNoError) {
    delete st;
    return MapJsError(env, ec, "buffers: root symbol");
  }
  JsPropertyIdRef pid = JS_INVALID_REFERENCE;
  ec = JsGetPropertyIdFromSymbol(sym, &pid);
  if (ec != JsNoError) {
    (void)JsRelease(sym, nullptr);
    delete st;
    return MapJsError(env, ec, "buffers: symbol property id");
  }
  ContextBufferSymbol* new_cs = new (std::nothrow) ContextBufferSymbol();
  if (new_cs == nullptr) {
    (void)JsRelease(sym, nullptr);
    delete st;
    nodeapi::SetLastError(env, napi_generic_failure,
                          "buffers: context symbol OOM");
    return napi_generic_failure;
  }
  new_cs->context = ctx;
  new_cs->symbol = sym;
  new_cs->symPid = pid;
  new_cs->ref_count = 0;

  bool drop_new_cs = false;
  bool map_oom = false;
  {
    std::lock_guard<std::mutex> lk(g_buf_mutex);
    auto it = g_env_states.find(gen);
    if (it != g_env_states.end()) {
      delete st;
      *out = it->second;
      drop_new_cs = true;
    } else {
      auto cit = g_context_symbols.find(ctx);
      if (cit != g_context_symbols.end() && cit->second != nullptr) {
        drop_new_cs = true;
        st->ctx_sym = cit->second;
        try {
          g_env_states[gen] = st;
        } catch (...) {
          map_oom = true;
        }
        if (!map_oom) cit->second->ref_count++;
      } else {
        try {
          g_context_symbols[ctx] = new_cs;
          g_env_states[gen] = st;
        } catch (...) {
          auto cit2 = g_context_symbols.find(ctx);
          if (cit2 != g_context_symbols.end() && cit2->second == new_cs) {
            g_context_symbols.erase(cit2);
          }
          map_oom = true;
          drop_new_cs = true;
        }
        if (!map_oom) {
          new_cs->ref_count = 1;
          st->ctx_sym = new_cs;
        }
      }
    }
  }
  if (drop_new_cs) {
    (void)JsRelease(new_cs->symbol, nullptr);
    delete new_cs;
  }
  if (map_oom) {
    delete st;
    nodeapi::SetLastError(env, napi_generic_failure, "buffers: state map OOM");
    return napi_generic_failure;
  }
  if (*out != nullptr) {
    return napi_ok;
  }
  napi_status hs = napi_add_env_cleanup_hook(
      reinterpret_cast<node_api_basic_env>(env), BuffersCleanupHook, st);
  if (hs != napi_ok) {
    ReleaseBufferState(st);
    return hs;
  }
  *out = st;
  return napi_ok;
}

napi_status IsArrayBufferDetached(napi_env env, JsValueRef ab, bool* out) {
  *out = false;
  JsPropertyIdRef pid = JS_INVALID_REFERENCE;
  JsErrorCode ec = JsCreatePropertyId("detached", 8, &pid);
  if (ec != JsNoError) return MapJsError(env, ec, "is_detached: id");
  JsValueRef val = JS_INVALID_REFERENCE;
  ec = JsGetProperty(ab, pid, &val);
  if (ec != JsNoError) return MapJsError(env, ec, "is_detached: get");
  ScratchRoot valRoot(val);
  JsValueType t = JsUndefined;
  ec = JsGetValueType(val, &t);
  if (ec != JsNoError) return MapJsError(env, ec, "is_detached: type");
  if (t != JsBoolean) {
    nodeapi::SetLastError(env, napi_generic_failure,
                          "is_detached: detached not boolean");
    return napi_generic_failure;
  }
  bool b = false;
  ec = JsBooleanToBool(val, &b);
  if (ec != JsNoError) return MapJsError(env, ec, "is_detached: bool");
  *out = b;
  return napi_ok;
}

// Define the hidden marker property on a fresh Uint8Array view.
// Descriptor: {value:true, writable:false, enumerable:false,
// configurable:false} via a real descriptor object + JsObjectDefineProperty.
napi_status MarkBuffer(napi_env env, BufferState* st, JsValueRef ta) {
  JsValueRef desc = JS_INVALID_REFERENCE;
  JsErrorCode ec = JsCreateObject(&desc);
  if (ec != JsNoError) return MapJsError(env, ec, "mark: create descriptor");
  ScratchRoot descRoot(desc);
  JsValueRef trueVal = JS_INVALID_REFERENCE;
  ec = JsGetTrueValue(&trueVal);
  if (ec != JsNoError) return MapJsError(env, ec, "mark: true");
  JsValueRef falseVal = JS_INVALID_REFERENCE;
  ec = JsGetFalseValue(&falseVal);
  if (ec != JsNoError) return MapJsError(env, ec, "mark: false");
  JsPropertyIdRef id_value = JS_INVALID_REFERENCE;
  ec = JsCreatePropertyId("value", 5, &id_value);
  if (ec != JsNoError) return MapJsError(env, ec, "mark: value id");
  JsPropertyIdRef id_writable = JS_INVALID_REFERENCE;
  ec = JsCreatePropertyId("writable", 8, &id_writable);
  if (ec != JsNoError) return MapJsError(env, ec, "mark: writable id");
  JsPropertyIdRef id_enum = JS_INVALID_REFERENCE;
  ec = JsCreatePropertyId("enumerable", 10, &id_enum);
  if (ec != JsNoError) return MapJsError(env, ec, "mark: enumerable id");
  JsPropertyIdRef id_conf = JS_INVALID_REFERENCE;
  ec = JsCreatePropertyId("configurable", 12, &id_conf);
  if (ec != JsNoError) return MapJsError(env, ec, "mark: configurable id");
  ec = JsSetProperty(desc, id_value, trueVal, false);
  if (ec != JsNoError) return MapJsError(env, ec, "mark: set value");
  ec = JsSetProperty(desc, id_writable, falseVal, false);
  if (ec != JsNoError) return MapJsError(env, ec, "mark: set writable");
  ec = JsSetProperty(desc, id_enum, falseVal, false);
  if (ec != JsNoError) return MapJsError(env, ec, "mark: set enumerable");
  ec = JsSetProperty(desc, id_conf, falseVal, false);
  if (ec != JsNoError) return MapJsError(env, ec, "mark: set configurable");
  bool defined = false;
  ec = JsObjectDefineProperty(ta, st->ctx_sym->symbol, desc, &defined);
  if (ec != JsNoError) return MapJsError(env, ec, "mark: define");
  if (!defined) {
    nodeapi::SetLastError(env, napi_generic_failure, "mark: define failed");
    return napi_generic_failure;
  }
  return napi_ok;
}

// Marker check (section 3.9): own-property + boolean true. Primitives are
// never Buffers; objects without the marker are false. Requires the
// env context entered. Uses JsGetProperty only (JsHasOwnProperty uses
// ContextAPIWrapper<true> which verifies the pending exception and
// returns JsErrorInExceptionState when pending, breaking the C-column
// Yes contract for is_buffer/get_buffer_info). JsGetProperty uses
// JSRT_MAYBE_TRUE (false in this build) so it tolerates a pending
// exception. Missing properties return undefined (not true), so the
// single get is sufficient; inherited markers are irrelevant because
// the marker Symbol is private and only our Buffers carry it.
napi_status IsBufferMarker(napi_env env, BufferState* st, JsValueRef value,
                           bool* out) {
  *out = false;
  JsValueType t = JsUndefined;
  JsErrorCode ec = JsGetValueType(value, &t);
  if (ec != JsNoError) return MapJsError(env, ec, "is_buffer: type");
  bool object_like = (t == JsObject || t == JsFunction || t == JsArray ||
                     t == JsArrayBuffer || t == JsTypedArray ||
                     t == JsDataView || t == JsError);
  if (!object_like) return napi_ok;  // primitives: never Buffers
  JsValueRef val = JS_INVALID_REFERENCE;
  ec = JsGetProperty(value, st->ctx_sym->symPid, &val);
  if (ec != JsNoError) return MapJsError(env, ec, "is_buffer: get");
  if (val == JS_INVALID_REFERENCE) return napi_ok;
  JsValueType vt = JsUndefined;
  ec = JsGetValueType(val, &vt);
  if (ec != JsNoError) return MapJsError(env, ec, "is_buffer: val type");
  if (vt != JsBoolean) return napi_ok;  // forged non-boolean: false
  bool b = false;
  ec = JsBooleanToBool(val, &b);
  if (ec != JsNoError) return MapJsError(env, ec, "is_buffer: bool");
  *out = b;
  return napi_ok;
}

}  // namespace

void nodeapi_buffers_teardown(napi_env env) {
  if (env == nullptr) return;
  uint64_t gen = 0;
  if (!nodeapi::LookupLiveEnvGeneration(static_cast<void*>(env), &gen)) return;
  BufferState* st = nullptr;
  {
    std::lock_guard<std::mutex> lk(g_buf_mutex);
    auto it = g_env_states.find(gen);
    if (it != g_env_states.end()) {
      st = it->second;
    }
  }
  if (st == nullptr) return;
  (void)napi_remove_env_cleanup_hook(
      reinterpret_cast<node_api_basic_env>(env), BuffersCleanupHook, st);
  ReleaseBufferState(st);
}

extern "C" {

// ---------------------------------------------------------------------
// napi_create_buffer (C No: gated)
// ---------------------------------------------------------------------

napi_status NAPI_CDECL napi_create_buffer(napi_env env, size_t length,
                                          void** data, napi_value* result) {
  if (result == nullptr) {
    if (env) nodeapi::SetLastError(env, napi_invalid_arg,
                                    "create_buffer: result is NULL");
    return napi_invalid_arg;
  }
  nodeapi::EntryGuard guard(env, /*allowClosing=*/false,
                            /*allowPending=*/false, /*needContext=*/true);
  if (!guard.ok()) return guard.status();
  ::nodeapi_wrap_drain(env);
  unsigned int len32 = 0;
  napi_status vs = ValidateSizeToUInt(env, length, &len32,
                                      "create_buffer: length too large");
  if (vs != napi_ok) return vs;
  JsValueRef ab = JS_INVALID_REFERENCE;
  JsErrorCode ec = JsCreateArrayBuffer(len32, &ab);
  if (ec != JsNoError) return MapJsError(env, ec, "JsCreateArrayBuffer");
  ScratchRoot abRoot(ab);
  JsValueRef ta = JS_INVALID_REFERENCE;
  ec = JsCreateTypedArray(JsArrayTypeUint8, ab, 0, len32, &ta);
  if (ec != JsNoError) return MapJsError(env, ec, "JsCreateTypedArray");
  ScratchRoot taRoot(ta);
  BufferState* st = nullptr;
  napi_status ss = GetOrCreateState(env, &st);
  if (ss != napi_ok) return ss;
  napi_status ms = MarkBuffer(env, st, ta);
  if (ms != napi_ok) return ms;
  if (data != nullptr) {
    BYTE* storBuf = nullptr;
    unsigned int storLen = 0;
    JsTypedArrayType storType = JsArrayTypeUint8;
    int elemSize = 0;
    ec = JsGetTypedArrayStorage(ta, &storBuf, &storLen, &storType, &elemSize);
    if (ec != JsNoError) return MapJsError(env, ec, "JsGetTypedArrayStorage");
    if (storLen != len32) {
      nodeapi::SetLastError(env, napi_generic_failure,
                            "create_buffer: length mismatch");
      return napi_generic_failure;
    }
    *data = reinterpret_cast<void*>(storBuf);
  }
  napi_status rs = RootAndPublish(env, ta, result);
  if (rs != napi_ok) return rs;
  nodeapi::ClearLastError(env);
  return napi_ok;
}

// ---------------------------------------------------------------------
// napi_create_buffer_copy (C No: gated)
// ---------------------------------------------------------------------

napi_status NAPI_CDECL napi_create_buffer_copy(napi_env env, size_t length,
                                               const void* data,
                                               void** result_data,
                                               napi_value* result) {
  if (result == nullptr) {
    if (env) nodeapi::SetLastError(env, napi_invalid_arg,
                                    "create_buffer_copy: result is NULL");
    return napi_invalid_arg;
  }
  nodeapi::EntryGuard guard(env, /*allowClosing=*/false,
                            /*allowPending=*/false, /*needContext=*/true);
  if (!guard.ok()) return guard.status();
  ::nodeapi_wrap_drain(env);
  unsigned int len32 = 0;
  napi_status vs = ValidateSizeToUInt(env, length, &len32,
                                      "create_buffer_copy: length too large");
  if (vs != napi_ok) return vs;
  if (data == nullptr && length > 0) {
    nodeapi::SetLastError(env, napi_invalid_arg,
                          "create_buffer_copy: data NULL with length");
    return napi_invalid_arg;
  }
  JsValueRef ab = JS_INVALID_REFERENCE;
  JsErrorCode ec = JsCreateArrayBuffer(len32, &ab);
  if (ec != JsNoError) return MapJsError(env, ec, "JsCreateArrayBuffer");
  ScratchRoot abRoot(ab);
  // Copy the source bytes into the fresh GC'd storage.
  if (length > 0) {
    BYTE* bufPtr = nullptr;
    unsigned int bufLen = 0;
    ec = JsGetArrayBufferStorage(ab, &bufPtr, &bufLen);
    if (ec != JsNoError) return MapJsError(env, ec, "JsGetArrayBufferStorage");
    if (bufLen != len32 || bufPtr == nullptr) {
      nodeapi::SetLastError(env, napi_generic_failure,
                            "create_buffer_copy: storage mismatch");
      return napi_generic_failure;
    }
    std::memcpy(bufPtr, data, length);
  }
  JsValueRef ta = JS_INVALID_REFERENCE;
  ec = JsCreateTypedArray(JsArrayTypeUint8, ab, 0, len32, &ta);
  if (ec != JsNoError) return MapJsError(env, ec, "JsCreateTypedArray");
  ScratchRoot taRoot(ta);
  if (result_data != nullptr) {
    BYTE* storBuf = nullptr;
    unsigned int storLen = 0;
    JsTypedArrayType storType = JsArrayTypeUint8;
    int elemSize = 0;
    ec = JsGetTypedArrayStorage(ta, &storBuf, &storLen, &storType, &elemSize);
    if (ec != JsNoError) return MapJsError(env, ec, "JsGetTypedArrayStorage");
    if (storLen != len32) {
      nodeapi::SetLastError(env, napi_generic_failure,
                            "create_buffer_copy: view length mismatch");
      return napi_generic_failure;
    }
    *result_data = reinterpret_cast<void*>(storBuf);
  }
  BufferState* st = nullptr;
  napi_status ss = GetOrCreateState(env, &st);
  if (ss != napi_ok) return ss;
  napi_status ms = MarkBuffer(env, st, ta);
  if (ms != napi_ok) return ms;
  napi_status rs = RootAndPublish(env, ta, result);
  if (rs != napi_ok) return rs;
  nodeapi::ClearLastError(env);
  return napi_ok;
}

// ---------------------------------------------------------------------
// napi_create_external_buffer (C No: gated, section 3.7 WrapRecord path)
// ---------------------------------------------------------------------

napi_status NAPI_CDECL napi_create_external_buffer(
    napi_env env, size_t length, void* data,
    node_api_basic_finalize finalize_cb, void* finalize_hint,
    napi_value* result) {
  if (result == nullptr) {
    if (env) nodeapi::SetLastError(env, napi_invalid_arg,
                                    "create_external_buffer: result is NULL");
    return napi_invalid_arg;
  }
  nodeapi::EntryGuard guard(env, /*allowClosing=*/false,
                            /*allowPending=*/false, /*needContext=*/true);
  if (!guard.ok()) return guard.status();
  ::nodeapi_wrap_drain(env);
  unsigned int len32 = 0;
  napi_status vs = ValidateSizeToUInt(env, length, &len32,
                                      "create_external_buffer: length too large");
  if (vs != napi_ok) return vs;
  if (data == nullptr && length > 0) {
    nodeapi::SetLastError(env, napi_invalid_arg,
                          "create_external_buffer: data NULL with length");
    return napi_invalid_arg;
  }
  // External ArrayBuffer with NULL finalizer: the ArrayBuffer aliases
  // `data` without owning a finalizer; the user's finalizer is carried
  // by the WrapRecord on the view (see header).
  JsValueRef ab = JS_INVALID_REFERENCE;
  JsErrorCode ec = JsCreateExternalArrayBuffer(data, len32, nullptr, nullptr, &ab);
  if (ec != JsNoError) return MapJsError(env, ec, "JsCreateExternalArrayBuffer");
  ScratchRoot abRoot(ab);
  JsValueRef ta = JS_INVALID_REFERENCE;
  ec = JsCreateTypedArray(JsArrayTypeUint8, ab, 0, len32, &ta);
  if (ec != JsNoError) return MapJsError(env, ec, "JsCreateTypedArray");
  ScratchRoot taRoot(ta);
  // Verify the view aliases the caller's data (defensive: the engine
  // contract is alias; a mismatch is an engine bug, not a napi error).
  if (length > 0) {
    BYTE* storBuf = nullptr;
    unsigned int storLen = 0;
    JsTypedArrayType storType = JsArrayTypeUint8;
    int elemSize = 0;
    ec = JsGetTypedArrayStorage(ta, &storBuf, &storLen, &storType, &elemSize);
    if (ec != JsNoError) return MapJsError(env, ec, "JsGetTypedArrayStorage");
    if (storLen != len32 || reinterpret_cast<void*>(storBuf) != data) {
      nodeapi::SetLastError(env, napi_generic_failure,
                            "create_external_buffer: storage does not alias");
      return napi_generic_failure;
    }
  }
  BufferState* st = nullptr;
  napi_status ss = GetOrCreateState(env, &st);
  if (ss != napi_ok) return ss;
  napi_status ms = MarkBuffer(env, st, ta);
  if (ms != napi_ok) return ms;
  // Attach the user's finalizer to the view via the section 3.7 WrapRecord
  // path (N3.2). Only when a finalizer was supplied (NULL => no
  // record; the external storage is still aliased).
  if (finalize_cb != nullptr) {
    napi_status fs = napi_add_finalizer(env, FromJs(ta), data, finalize_cb,
                                        finalize_hint, nullptr);
    if (fs != napi_ok) return fs;
  }
  napi_status rs = RootAndPublish(env, ta, result);
  if (rs != napi_ok) return rs;
  nodeapi::ClearLastError(env);
  return napi_ok;
}

// ---------------------------------------------------------------------
// node_api_create_buffer_from_arraybuffer (v10, C No: gated)
// ---------------------------------------------------------------------

NAPI_EXTERN napi_status NAPI_CDECL node_api_create_buffer_from_arraybuffer(
    napi_env env, napi_value arraybuffer, size_t byte_offset,
    size_t byte_length, napi_value* result) {
  if (arraybuffer == nullptr || result == nullptr) {
    if (env) {
      nodeapi::SetLastError(env, napi_invalid_arg,
                            "create_buffer_from_arraybuffer: null arg");
    }
    return napi_invalid_arg;
  }
  nodeapi::EntryGuard guard(env, /*allowClosing=*/false,
                            /*allowPending=*/false, /*needContext=*/true);
  if (!guard.ok()) return guard.status();
  ::nodeapi_wrap_drain(env);

  JsValueRef ab = ToJs(arraybuffer);
  JsValueType t = JsUndefined;
  JsErrorCode ec = JsGetValueType(ab, &t);
  if (ec != JsNoError) return MapJsError(env, ec, "JsGetValueType");
  if (t != JsArrayBuffer) {
    nodeapi::SetLastError(
        env, napi_arraybuffer_expected,
        "create_buffer_from_arraybuffer: not an ArrayBuffer");
    return napi_arraybuffer_expected;
  }
  ScratchRoot abRoot(ab);

  bool detached = false;
  napi_status ds = IsArrayBufferDetached(env, ab, &detached);
  if (ds != napi_ok) return ds;

  BYTE* abStor = nullptr;
  unsigned int abLen32 = 0;
  ec = JsGetArrayBufferStorage(ab, &abStor, &abLen32);
  if (ec != JsNoError) return MapJsError(env, ec, "JsGetArrayBufferStorage");
  size_t ab_byte_len = detached ? 0 : static_cast<size_t>(abLen32);

  if ((detached && (byte_offset > 0 || byte_length > 0)) ||
      byte_offset > ab_byte_len ||
      byte_length > ab_byte_len - byte_offset) {
    napi_status ts = napi_throw_range_error(
        env, "ERR_BUFFER_OUT_OF_BOUNDS",
        "The byte offset or length is outside buffer bounds");
    if (ts != napi_ok) return ts;
    nodeapi::SetLastError(env, napi_pending_exception,
                          "create_buffer_from_arraybuffer: out of bounds");
    return napi_pending_exception;
  }

  unsigned int off32 = 0;
  napi_status vs = ValidateSizeToUInt(
      env, byte_offset, &off32,
      "create_buffer_from_arraybuffer: offset too large");
  if (vs != napi_ok) return vs;
  unsigned int len32 = 0;
  vs = ValidateSizeToUInt(
      env, byte_length, &len32,
      "create_buffer_from_arraybuffer: length too large");
  if (vs != napi_ok) return vs;

  JsValueRef ta = JS_INVALID_REFERENCE;
  ec = JsCreateTypedArray(JsArrayTypeUint8, ab, off32, len32, &ta);
  if (ec != JsNoError) return MapJsError(env, ec, "JsCreateTypedArray");
  ScratchRoot taRoot(ta);

  BufferState* st = nullptr;
  napi_status ss = GetOrCreateState(env, &st);
  if (ss != napi_ok) return ss;
  napi_status ms = MarkBuffer(env, st, ta);
  if (ms != napi_ok) return ms;
  napi_status rs = RootAndPublish(env, ta, result);
  if (rs != napi_ok) return rs;
  nodeapi::ClearLastError(env);
  return napi_ok;
}

// ---------------------------------------------------------------------
// napi_is_buffer (C Yes: executes, ok+false on wrong type)
// ---------------------------------------------------------------------

napi_status NAPI_CDECL napi_is_buffer(napi_env env, napi_value value,
                                      bool* result) {
  if (value == nullptr || result == nullptr) {
    if (env) nodeapi::SetLastError(env, napi_invalid_arg,
                                    "is_buffer: null arg");
    return napi_invalid_arg;
  }
  nodeapi::EntryGuard guard(env, /*allowClosing=*/false,
                            /*allowPending=*/true, /*needContext=*/true);
  if (!guard.ok()) return guard.status();
  ::nodeapi_wrap_drain(env);
  BufferState* st = nullptr;
  napi_status ss = GetOrCreateState(env, &st);
  if (ss != napi_ok) return ss;
  bool isBuf = false;
  napi_status s = IsBufferMarker(env, st, ToJs(value), &isBuf);
  if (s != napi_ok) return s;
  *result = isBuf;
  nodeapi::ClearLastError(env);
  return napi_ok;
}

// ---------------------------------------------------------------------
// napi_get_buffer_info (C Yes: executes, invalid_arg if not a Buffer)
// ---------------------------------------------------------------------

napi_status NAPI_CDECL napi_get_buffer_info(napi_env env, napi_value value,
                                            void** data, size_t* length) {
  if (value == nullptr) {
    if (env) nodeapi::SetLastError(env, napi_invalid_arg,
                                    "get_buffer_info: value is NULL");
    return napi_invalid_arg;
  }
  nodeapi::EntryGuard guard(env, /*allowClosing=*/false,
                            /*allowPending=*/true, /*needContext=*/true);
  if (!guard.ok()) return guard.status();
  ::nodeapi_wrap_drain(env);
  BufferState* st = nullptr;
  napi_status ss = GetOrCreateState(env, &st);
  if (ss != napi_ok) return ss;
  bool isBuf = false;
  napi_status ms = IsBufferMarker(env, st, ToJs(value), &isBuf);
  if (ms != napi_ok) return ms;
  if (!isBuf) {
    nodeapi::SetLastError(env, napi_invalid_arg,
                          "get_buffer_info: not a Buffer");
    return napi_invalid_arg;
  }
  // Compose the N3.1 typed-array info (length == byte length for Uint8).
  napi_typedarray_type tmpType = napi_int8_array;
  napi_status gs = napi_get_typedarray_info(env, value, &tmpType, length,
                                            data, nullptr, nullptr);
  if (gs != napi_ok) return gs;
  if (tmpType != napi_uint8_array) {
    nodeapi::SetLastError(env, napi_invalid_arg,
                          "get_buffer_info: not a Uint8Array view");
    return napi_invalid_arg;
  }
  nodeapi::ClearLastError(env);
  return napi_ok;
}

}  // extern "C"
