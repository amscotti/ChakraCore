//-------------------------------------------------------------------------------------------------------
// Copyright (c) ChakraCore Project Contributors. All rights reserved.
// Licensed under the MIT license. See LICENSE.txt file in the project root for full license information.
//-------------------------------------------------------------------------------------------------------
//
// Node-API binary values (N3.1). See NodeApiBinary.h for the surface list
// and status discipline, plus ownership records below.
//
// Implementation notes (Node v22.22.3 src/js_native_api_v8.cc + doc/api/n-api.md,
// via doc/node-api-status-mapping.md):
//   - All public entries follow validated-first (NULL binding args ->
//     napi_invalid_arg before anything else), then the shared EntryGuard
//     (Closing -> generic_failure, gated functions -> napi_pending_exception
//     without executing, context entered via nodeapi::ContextGuard).
//     Creates are C-column No (gated); gets/detach/is_detached are Yes
//     (execute with pending, pending preserved).
//   - ArrayBuffer creation via JsCreateArrayBuffer; external via
//     JsCreateExternalArrayBuffer with trampoline §3.7 record.
//   - TypedArray/DataView creation via JsCreateTypedArray / JsCreateDataView.
//   - Info getters compose JsGet*Info / JsGet*Storage per audit §2.4.
//   - Detached detection is via the JS ArrayBuffer.prototype.detached getter
//     (verified present in this build: JavascriptLibrary.cpp registers
//     accessor "detached" with GetterDetached, ArrayBuffer.cpp:546-562).
//     Storage-only detection is insufficient: detached buffers report
//     NULL/0 via JsGetArrayBufferStorage (ArrayBuffer::GetBuffer returns
//     nullptr when bufferContent==nullptr after Detach, ArrayBuffer.cpp:76-84
//     + 268-298), which is indistinguishable from a zero-length attached
//     buffer (also NULL/0). See the detached-evidence comment on
//     IsDetachedViaProperty.
//   - BigInt64/BigUint64 typed arrays have no JsTypedArrayType counterpart
//     (ChakraCommon.h JsTypedArrayType ends at Float16, 10 values; napi
//     has 11 with BigInt64/BigUint64). They return napi_generic_failure
//     with a last-error message (recorded non-conformance per §4.3/PR-9).
//     Float16 similarly has no napi counterpart; get_typedarray_info on a
//     JS-created Float16Array also returns generic_failure (documented
//     extension, not a napi value).
//
// Ownership records (memory-management.md §3; plan §3.7/§3.4 R4):
//
//   External ArrayBuffer records (napi_create_external_arraybuffer):
//     Resource / creation call: ExternalRecord heap allocation (new) in
//       NodeApiBinary.cpp, transferred to JsCreateExternalArrayBuffer as
//       callbackState with the trampoline as finalizeCallback.
//     Domain and allocator: provider native heap (C++ new/delete via the
//       provider allocator; backing lifetime == process). Never Recycler.
//     Physical allocation: one ExternalRecord per external buffer (plus the
//       JsrtExternalArrayBuffer/Recycler object created by JsRT).
//     Owner; transfer point: The record is owned by the JS ArrayBuffer's
//       finalize slot after JsCreateExternalArrayBuffer. The finalize
//       callback (JsrtExternalArrayBuffer::Finalize) owns invocation on
//       GC. Creation failure before transfer (OOM / JsCreate failure)
//       deletes the record inline; after transfer, the trampoline owns
//       queueing, never inline delete.
//     Borrow invalidation: GC finalization (Finalize or detached-state
//       DiscardState). Detach transfers the record's callback/state to the
//       detached-state (JsrtExternalArrayBuffer.cpp:42-67); simple
//       JsDetachArrayBuffer (ArrayBuffer::Detach) drops the callback (skips
//       finalize on detached, § below), which is a known JsRT leak for
//       detached-external finalizers (recorded as residual, not a provider
//       double-free).
//     Potential user-code/reentrancy: GC trampoline never calls user code
//       (D4): it only enqueues the record pointer under a mutex, no JS,
//       no provider-container mutation beyond the queue push. Drain on env
//       thread invokes the user napi_finalize(env, data, hint) with a live
//       env and the context entered (EntryGuard). The user finalizer may
//       call provider APIs (which re-enter EntryGuard; queue was moved
//       before invoking, so reentrant drain sees empty).
//     Thread/context: trampoline on arbitrary GC thread, no context, only
//       native queue mutation under mutex (never across user code). Drain
//       on owner thread with env context entered.
//     Release on success/partial failure/exception/OOM/cancel/shutdown:
//       Exactly once via drain (delete after invocation) or, if the env is
//       already dead at drain time, delete without invocation (stale
//       generation guard, no use-after-free). Shutdown: records for buffers
//       still live at destroy die with the runtime; if the env is destroyed
//       before the buffer is collected, the trampoline will enqueue under a
//       dead generation; the next Drain (or never) deletes without calling
//       the user finalizer (leak of user external_data, not a UAF; probe
//       does not cover post-destroy collect).
//     Memory accounting/capacity: one record per live external buffer;
//       queue vectors hold at most live uncollected external buffers
//       (bounded by JS heap). OOM in record alloc => generic_failure,
//       nothing published.
//     Tests: test/nodeapi/probes/binary_probe.cpp external finalizer fires
//       once on collect (GC via JsCollectGarbage + next napi call drain).
//     Residual uncertainty: detached-external finalizer leak (see above);
//       finalizer after env destroy not invoked (leak, not crash); Linux
//       x64 not claimed.
//
//   R4 storage borrows (get_*_info):
//     Resource: raw BYTE* + length from JsGet*Storage (borrow, not root).
//     Domain: Recycler heap (ArrayBuffer/TypedArray/DataView storage).
//     Owner: The caller-passed JsValueRef (already rooted by the caller's
//       scope). The provider takes a transient ScratchRoot (JsAddRef) for
//       the borrow's window (R1) so GC cannot collect the owner during the
//       JsRT calls + any JS-executing property gets. The returned data
//       pointer is NOT rooted; its lifetime is the caller's responsibility
//       (R4: root the ArrayBuffer for the borrow's life and revalidate
//       after any JS call that can run). In get_typedarray_info /
//       get_dataview_info the provider re-reads storage after property
//       getters (JS-executing) and returns the revalidated pointer.
//     Invalidation: detach (ArrayBuffer::Detach), GC, resize (resizable).
//       After a JS call that detaches (e.g., via napi_detach or JS
//       transfer), the borrow is invalid; the probe verifies a fresh
//       get_*_info after such a call observes NULL/0.
//     Release: ScratchRoots are RAII (JsRelease at return); the caller's
//       scope owns the long-term root until scope close.
//     Tests: binary_probe storage-borrow-invalidated-after-detach case.
//     OOM: ScratchRoot JsAddRef never throws; vector OOM (scratch buffers)
//       maps to generic_failure.
//
//   Returned napi_values (create_*, get_*_info arraybuffer out):
//     Rooted into innermost scope via NodeApiScopes_RootIntoInnermost
//     (R-SC-1). On rooting failure the out-param is untouched and the
//     unpublished value is discarded (its external record, if any, will be
//     freed by GC as above).
//
// Potential user-code points (R4 / §6):
//   - TypedArray/DataView property getters ("buffer", "byteOffset",
//     "byteLength", "detached") invoke engine getters (no user trap on the
//     builtin prototype, but a Proxy or tampered prototype could run user
//     code). The provider holds ScratchRoots for subjects and re-reads
//     storage after each getter.
//   - JsCreateTypedArray / JsCreateDataView can throw (RangeError for
//     alignment/length, TypeError for detached) leaving a pending exception
//     (mapped to napi_pending_exception). The partially created value is
//     never rooted/published.
//   - External finalizer user code runs only at drain (env thread).
//
// Language: C++17 (GNU++17 via `zig c++`, matching Waves 1/2) behind the
// extern "C" napi_* surface (PR-4 decision record: C++ through provider
// v1). No C++ exception crosses extern "C": every allocation is
// nothrow/try-catch wrapped and maps to generic_failure.

#include "NodeApiBinary.h"

#include "NodeApiEnv.h"
#include "NodeApiScopes.h"
#include "ChakraCommon.h"
#include "ChakraCore.h"

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <limits>
#include <mutex>
#include <new>
#include <unordered_map>
#include <vector>

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

napi_status MakeId(const char* name, JsPropertyIdRef* out) {
  JsErrorCode e = JsCreatePropertyId(name, std::strlen(name), out);
  return (e == JsNoError) ? napi_ok : napi_generic_failure;
}

napi_status MakeIdMap(napi_env env, const char* name, JsPropertyIdRef* out,
                      const char* what) {
  JsErrorCode e = JsCreatePropertyId(name, std::strlen(name), out);
  if (e != JsNoError) return MapJsError(env, e, what);
  return napi_ok;
}

// --- External ArrayBuffer finalizer queue (§3.7-style, N3.2-compatible) ---
//
// The queue is keyed by per-env generation (not by env pointer value) to
// survive address reuse: a trampoline for a dead env enqueues under its
// dead generation; the next Drain for a new env at the same address sees a
// different generation and never dispatches stale records. Stale records
// are deleted without invoking user code (no UAF; leak of user data is
// preferable to calling a finalizer with a dead env).

struct ExternalRecord {
  napi_env env = nullptr;
  uint64_t generation = 0;
  node_api_basic_finalize finalize_cb = nullptr;
  void* finalize_data = nullptr;
  void* finalize_hint = nullptr;
};

std::mutex g_ext_mutex;
std::unordered_map<uint64_t, std::vector<ExternalRecord*>> g_ext_queues;

void EnqueueRecord(ExternalRecord* rec) {
  if (!rec) return;
  std::lock_guard<std::mutex> lk(g_ext_mutex);
  try {
    g_ext_queues[rec->generation].push_back(rec);
  } catch (...) {
    delete rec;
  }
}

// GC-side trampoline (JsFinalizeCallback): runs on arbitrary GC thread
// with no current context. Only enqueues the record pointer (D4). No JS,
// no user finalizer, no provider-container mutation beyond the queue push
// under mutex.
void ExternalArrayBufferTrampoline(void* state) {
  auto* rec = static_cast<ExternalRecord*>(state);
  EnqueueRecord(rec);
}

// Drain on env thread with a live env and the context entered.
// Moves the queue out before invoking (lock never held across user code).
// If the env is dead or generation-mismatched, records are deleted without
// invocation.
void DrainExternalFinalizers(napi_env env) {
  if (!env) return;
  nodeapi::NodeEnv* nenv = nullptr;
  if (!nodeapi::LookupLiveEnv(static_cast<void*>(env), &nenv)) return;
  uint64_t gen = nenv->generation;
  std::vector<ExternalRecord*> to_run;
  {
    std::lock_guard<std::mutex> lk(g_ext_mutex);
    auto it = g_ext_queues.find(gen);
    if (it != g_ext_queues.end()) {
      to_run = std::move(it->second);
      g_ext_queues.erase(it);
    }
  }
  for (auto* rec : to_run) {
    if (rec && rec->finalize_cb) {
      // Env still live (generation matched above); context is entered by
      // the caller's EntryGuard. Invoke user finalizer.
      try {
        rec->finalize_cb(rec->env, rec->finalize_data, rec->finalize_hint);
      } catch (...) {
        // No C++ exception may escape extern "C".
      }
    }
    delete rec;
  }
}

// Detached detection via ArrayBuffer.prototype.detached getter.
// Evidence (verified at build time): the accessor "detached" is registered
// on ArrayBuffer.prototype (JavascriptLibrary.cpp:2392) with
// GetterDetached (ArrayBuffer.cpp:546-562) returning IsDetached() as a
// Boolean. Reading the property on an ArrayBuffer instance invokes the
// getter and observes the internal flag, including for zero-length
// buffers where JsGetArrayBufferStorage alone (NULL/0) is ambiguous.
// The storage path (JsGetArrayBufferStorage) returns NULL/0 both for a
// detached buffer (ArrayBuffer::Detach nulls bufferContent/length) and
// for a zero-length attached buffer (bufferContent==nullptr by
// construction), so it cannot distinguish them. The getter is the only
// reliable JsRT-observable signal without a dedicated JsIsDetached export.
// Returns napi_ok + *out on success; maps JsRT errors otherwise.
napi_status IsDetachedViaProperty(napi_env env, JsValueRef ab, bool* out) {
  *out = false;
  JsPropertyIdRef pid = JS_INVALID_REFERENCE;
  napi_status s = MakeIdMap(env, "detached", &pid, "is_detached: id");
  if (s != napi_ok) return s;
  JsValueRef val = JS_INVALID_REFERENCE;
  JsErrorCode ec = JsGetProperty(ab, pid, &val);
  if (ec != JsNoError) return MapJsError(env, ec, "is_detached: get");
  ScratchRoot valRoot(val);
  // The getter returns a Boolean; convert generically so a tampered
  // non-boolean (should not happen) is treated as generic_failure rather
  // than crashing.
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

// NAPI type <-> Js type maps.
bool NapiTypeToJs(napi_typedarray_type n, JsTypedArrayType* out) {
  switch (n) {
    case napi_int8_array: *out = JsArrayTypeInt8; return true;
    case napi_uint8_array: *out = JsArrayTypeUint8; return true;
    case napi_uint8_clamped_array: *out = JsArrayTypeUint8Clamped; return true;
    case napi_int16_array: *out = JsArrayTypeInt16; return true;
    case napi_uint16_array: *out = JsArrayTypeUint16; return true;
    case napi_int32_array: *out = JsArrayTypeInt32; return true;
    case napi_uint32_array: *out = JsArrayTypeUint32; return true;
    case napi_float32_array: *out = JsArrayTypeFloat32; return true;
    case napi_float64_array: *out = JsArrayTypeFloat64; return true;
    case napi_bigint64_array:
    case napi_biguint64_array:
      return false;  // No Js counterpart; caller maps to generic_failure.
    default:
      return false;
  }
}

size_t ElemSizeForJs(JsTypedArrayType t) {
  switch (t) {
    case JsArrayTypeInt8: return 1;
    case JsArrayTypeUint8: return 1;
    case JsArrayTypeUint8Clamped: return 1;
    case JsArrayTypeInt16: return 2;
    case JsArrayTypeUint16: return 2;
    case JsArrayTypeInt32: return 4;
    case JsArrayTypeUint32: return 4;
    case JsArrayTypeFloat32: return 4;
    case JsArrayTypeFloat64: return 8;
    case JsArrayTypeFloat16: return 2;
    default: return 1;
  }
}

bool JsTypeToNapi(JsTypedArrayType j, napi_typedarray_type* out) {
  switch (j) {
    case JsArrayTypeInt8: *out = napi_int8_array; return true;
    case JsArrayTypeUint8: *out = napi_uint8_array; return true;
    case JsArrayTypeUint8Clamped: *out = napi_uint8_clamped_array; return true;
    case JsArrayTypeInt16: *out = napi_int16_array; return true;
    case JsArrayTypeUint16: *out = napi_uint16_array; return true;
    case JsArrayTypeInt32: *out = napi_int32_array; return true;
    case JsArrayTypeUint32: *out = napi_uint32_array; return true;
    case JsArrayTypeFloat32: *out = napi_float32_array; return true;
    case JsArrayTypeFloat64: *out = napi_float64_array; return true;
    case JsArrayTypeFloat16: return false;  // No napi counterpart.
    default: return false;
  }
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

}  // namespace

extern "C" {

// ---------------------------------------------------------------------
// napi_create_arraybuffer (C No: gated)
// ---------------------------------------------------------------------

napi_status NAPI_CDECL napi_create_arraybuffer(napi_env env, size_t byte_length,
                                               void** data, napi_value* result) {
  if (result == nullptr) {
    if (env) nodeapi::SetLastError(env, napi_invalid_arg,
                                    "create_arraybuffer: result is NULL");
    return napi_invalid_arg;
  }
  nodeapi::EntryGuard guard(env, /*allowClosing=*/false,
                            /*allowPending=*/false, /*needContext=*/true);
  if (!guard.ok()) return guard.status();
  DrainExternalFinalizers(env);
  unsigned int len32 = 0;
  napi_status vs = ValidateSizeToUInt(env, byte_length, &len32,
                                      "create_arraybuffer: length too large");
  if (vs != napi_ok) return vs;
  JsValueRef buf = JS_INVALID_REFERENCE;
  JsErrorCode ec = JsCreateArrayBuffer(len32, &buf);
  if (ec != JsNoError) return MapJsError(env, ec, "JsCreateArrayBuffer");
  ScratchRoot bufRoot(buf);
  // Data borrow (R4): root the buffer for the storage read.
  if (data != nullptr) {
    ChakraBytePtr* outPtr = reinterpret_cast<ChakraBytePtr*>(data);
    unsigned int outLen = 0;
    ec = JsGetArrayBufferStorage(buf, reinterpret_cast<BYTE**>(outPtr), &outLen);
    if (ec != JsNoError) return MapJsError(env, ec, "JsGetArrayBufferStorage");
    // outLen must equal len32; use it as authoritative.
    if (outLen != len32) {
      // Engine should not disagree; treat as generic_failure (no throw).
      nodeapi::SetLastError(env, napi_generic_failure,
                            "create_arraybuffer: length mismatch");
      return napi_generic_failure;
    }
  }
  // Publish rooted.
  napi_status rs = RootAndPublish(env, buf, result);
  if (rs != napi_ok) return rs;
  nodeapi::ClearLastError(env);
  return napi_ok;
}

// ---------------------------------------------------------------------
// napi_create_external_arraybuffer (C No: gated)
// ---------------------------------------------------------------------

napi_status NAPI_CDECL napi_create_external_arraybuffer(
    napi_env env, void* external_data, size_t byte_length,
    node_api_basic_finalize finalize_cb, void* finalize_hint,
    napi_value* result) {
  if (result == nullptr) {
    if (env) nodeapi::SetLastError(env, napi_invalid_arg,
                                    "create_external_arraybuffer: result is NULL");
    return napi_invalid_arg;
  }
  nodeapi::EntryGuard guard(env, /*allowClosing=*/false,
                            /*allowPending=*/false, /*needContext=*/true);
  if (!guard.ok()) return guard.status();
  DrainExternalFinalizers(env);
  // Node semantics: external_data may be NULL only when byte_length==0
  // (mirrors JsCreateExternalArrayBuffer's own check; validated-first).
  if (external_data == nullptr && byte_length > 0) {
    nodeapi::SetLastError(env, napi_invalid_arg,
                          "create_external_arraybuffer: data NULL with length");
    return napi_invalid_arg;
  }
  unsigned int len32 = 0;
  napi_status vs = ValidateSizeToUInt(env, byte_length, &len32,
                                      "create_external_arraybuffer: length too large");
  if (vs != napi_ok) return vs;

  ExternalRecord* rec = nullptr;
  JsFinalizeCallback tramp = nullptr;
  void* tramp_state = nullptr;
  if (finalize_cb != nullptr) {
    try {
      rec = new ExternalRecord();
    } catch (...) {
      nodeapi::SetLastError(env, napi_generic_failure,
                            "create_external_arraybuffer: record OOM");
      return napi_generic_failure;
    }
    if (rec == nullptr) {
      nodeapi::SetLastError(env, napi_generic_failure,
                            "create_external_arraybuffer: record OOM");
      return napi_generic_failure;
    }
    nodeapi::NodeEnv* nenv = nullptr;
    if (!nodeapi::LookupLiveEnv(static_cast<void*>(env), &nenv)) {
      delete rec;
      return napi_invalid_arg;
    }
    rec->env = env;
    rec->generation = nenv->generation;
    rec->finalize_cb = finalize_cb;
    rec->finalize_data = external_data;
    rec->finalize_hint = finalize_hint;
    tramp = ExternalArrayBufferTrampoline;
    tramp_state = rec;
  }

  JsValueRef buf = JS_INVALID_REFERENCE;
  JsErrorCode ec = JsCreateExternalArrayBuffer(external_data, len32, tramp,
                                               tramp_state, &buf);
  if (ec != JsNoError) {
    delete rec;  // Transfer never happened; delete inline.
    return MapJsError(env, ec, "JsCreateExternalArrayBuffer");
  }
  ScratchRoot bufRoot(buf);
  // Transfer point for rec ownership is inside JsCreateExternalArrayBuffer
  // (callbackState). After this point, rec is owned by the JS object;
  // never delete inline here.
  napi_status rs = RootAndPublish(env, buf, result);
  if (rs != napi_ok) {
    // Publish failed (root OOM): the buffer stays unrooted and will be
    // collected; its finalizer trampoline will enqueue and drain will
    // delete. Do not delete inline.
    return rs;
  }
  nodeapi::ClearLastError(env);
  return napi_ok;
}

// ---------------------------------------------------------------------
// napi_get_arraybuffer_info (C Yes: executes)
// ---------------------------------------------------------------------

napi_status NAPI_CDECL napi_get_arraybuffer_info(napi_env env,
                                                 napi_value arraybuffer,
                                                 void** data,
                                                 size_t* byte_length) {
  if (arraybuffer == nullptr) {
    if (env) nodeapi::SetLastError(env, napi_invalid_arg,
                                    "get_arraybuffer_info: arraybuffer is NULL");
    return napi_invalid_arg;
  }
  nodeapi::EntryGuard guard(env, /*allowClosing=*/false,
                            /*allowPending=*/true, /*needContext=*/true);
  if (!guard.ok()) return guard.status();
  DrainExternalFinalizers(env);
  JsValueRef ab = ToJs(arraybuffer);
  JsValueType t = JsUndefined;
  JsErrorCode ec = JsGetValueType(ab, &t);
  if (ec != JsNoError) return MapJsError(env, ec, "JsGetValueType");
  if (t != JsArrayBuffer) {
    // Per C column: invalid_arg (not arraybuffer_expected) for this API.
    nodeapi::SetLastError(env, napi_invalid_arg,
                          "get_arraybuffer_info: not an ArrayBuffer");
    return napi_invalid_arg;
  }
  ScratchRoot abRoot(ab);
  ChakraBytePtr* outPtrRaw = reinterpret_cast<ChakraBytePtr*>(data);
  BYTE* tmpBuf = nullptr;
  unsigned int tmpLen = 0;
  // Always need both to fill optional out-params; pass locals then publish.
  ec = JsGetArrayBufferStorage(ab, &tmpBuf, &tmpLen);
  if (ec != JsNoError) return MapJsError(env, ec, "JsGetArrayBufferStorage");
  // R4: data borrow is valid only while abRoot holds (caller responsibility
  // after return). Detached and zero-length attached both report NULL/0.
  if (data != nullptr) {
    *data = reinterpret_cast<void*>(tmpBuf);
  }
  if (byte_length != nullptr) {
    *byte_length = static_cast<size_t>(tmpLen);
  }
  nodeapi::ClearLastError(env);
  return napi_ok;
}

// ---------------------------------------------------------------------
// napi_detach_arraybuffer (C Yes, with detachable/arraybuffer_expected)
// ---------------------------------------------------------------------

napi_status NAPI_CDECL napi_detach_arraybuffer(napi_env env,
                                               napi_value arraybuffer) {
  if (arraybuffer == nullptr) {
    if (env) nodeapi::SetLastError(env, napi_invalid_arg,
                                    "detach_arraybuffer: arraybuffer is NULL");
    return napi_invalid_arg;
  }
  nodeapi::EntryGuard guard(env, /*allowClosing=*/false,
                            /*allowPending=*/true, /*needContext=*/true);
  if (!guard.ok()) return guard.status();
  DrainExternalFinalizers(env);
  JsValueRef ab = ToJs(arraybuffer);
  JsValueType t = JsUndefined;
  JsErrorCode ec = JsGetValueType(ab, &t);
  if (ec != JsNoError) return MapJsError(env, ec, "JsGetValueType");
  if (t != JsArrayBuffer) {
    nodeapi::SetLastError(env, napi_arraybuffer_expected,
                          "detach_arraybuffer: not an ArrayBuffer");
    return napi_arraybuffer_expected;
  }
  ScratchRoot abRoot(ab);
  bool detached = false;
  napi_status ds = IsDetachedViaProperty(env, ab, &detached);
  if (ds != napi_ok) return ds;
  if (detached) {
    nodeapi::SetLastError(env, napi_detachable_arraybuffer_expected,
                          "detach_arraybuffer: already detached");
    return napi_detachable_arraybuffer_expected;
  }
  ec = JsDetachArrayBuffer(ab);
  if (ec != JsNoError) {
    // Immutable is the only remaining InvalidArgument after the checks
    // above (JsDetachArrayBuffer checks IsImmutable). Map to
    // detachable_expected per docs+impl (status-mapping table).
    if (ec == JsErrorInvalidArgument || ec == JsErrorNullArgument) {
      nodeapi::SetLastError(env, napi_detachable_arraybuffer_expected,
                            "detach_arraybuffer: not detachable");
      return napi_detachable_arraybuffer_expected;
    }
    return MapJsError(env, ec, "JsDetachArrayBuffer");
  }
  // R4 revalidation: after the detach (which can run no user code, but
  // conceptually invalidates the borrow), no storage is returned, so
  // nothing further to revalidate. A fresh get_*_info after this sees
  // NULL/0 (probe-verified).
  nodeapi::ClearLastError(env);
  return napi_ok;
}

// ---------------------------------------------------------------------
// napi_is_detached_arraybuffer (C Yes, ok+false on wrong type)
// ---------------------------------------------------------------------

napi_status NAPI_CDECL napi_is_detached_arraybuffer(napi_env env,
                                                    napi_value value,
                                                    bool* result) {
  if (value == nullptr || result == nullptr) {
    if (env) nodeapi::SetLastError(env, napi_invalid_arg,
                                    "is_detached_arraybuffer: null arg");
    return napi_invalid_arg;
  }
  nodeapi::EntryGuard guard(env, /*allowClosing=*/false,
                            /*allowPending=*/true, /*needContext=*/true);
  if (!guard.ok()) return guard.status();
  DrainExternalFinalizers(env);
  JsValueRef v = ToJs(value);
  JsValueType t = JsUndefined;
  JsErrorCode ec = JsGetValueType(v, &t);
  if (ec != JsNoError) return MapJsError(env, ec, "JsGetValueType");
  if (t != JsArrayBuffer) {
    *result = false;
    nodeapi::ClearLastError(env);
    return napi_ok;
  }
  ScratchRoot vRoot(v);
  bool detached = false;
  napi_status ds = IsDetachedViaProperty(env, v, &detached);
  if (ds != napi_ok) return ds;
  *result = detached;
  nodeapi::ClearLastError(env);
  return napi_ok;
}

// ---------------------------------------------------------------------
// napi_create_typedarray (C No: gated)
// ---------------------------------------------------------------------

napi_status NAPI_CDECL napi_create_typedarray(napi_env env,
                                              napi_typedarray_type type,
                                              size_t length,
                                              napi_value arraybuffer,
                                              size_t byte_offset,
                                              napi_value* result) {
  if (result == nullptr) {
    if (env) nodeapi::SetLastError(env, napi_invalid_arg,
                                    "create_typedarray: result is NULL");
    return napi_invalid_arg;
  }
  nodeapi::EntryGuard guard(env, /*allowClosing=*/false,
                            /*allowPending=*/false, /*needContext=*/true);
  if (!guard.ok()) return guard.status();
  DrainExternalFinalizers(env);
  // Type pre-check: BigInt64/BigUint64 -> generic_failure (non-conformance).
  if (type == napi_bigint64_array || type == napi_biguint64_array) {
    nodeapi::SetLastError(env, napi_generic_failure,
                          "create_typedarray: BigInt typed arrays unsupported");
    return napi_generic_failure;
  }
  JsTypedArrayType jsType;
  if (!NapiTypeToJs(type, &jsType)) {
    nodeapi::SetLastError(env, napi_invalid_arg,
                          "create_typedarray: bad type");
    return napi_invalid_arg;
  }
  size_t elemSize = ElemSizeForJs(jsType);
  unsigned int len32 = 0;
  napi_status vs = ValidateSizeToUInt(env, length, &len32,
                                      "create_typedarray: length too large");
  if (vs != napi_ok) return vs;
  unsigned int off32 = 0;
  vs = ValidateSizeToUInt(env, byte_offset, &off32,
                          "create_typedarray: offset too large");
  if (vs != napi_ok) return vs;
  // Alignment is checked by the engine (RangeError -> pending); validated
  // first is the ArrayBuffer check below (invalid_arg if not ArrayBuffer).

  JsValueRef base = JS_INVALID_REFERENCE;
  if (arraybuffer != nullptr) {
    JsValueRef ab = ToJs(arraybuffer);
    JsValueType t = JsUndefined;
    JsErrorCode te = JsGetValueType(ab, &t);
    if (te != JsNoError) return MapJsError(env, te, "JsGetValueType");
    if (t != JsArrayBuffer) {
      nodeapi::SetLastError(env, napi_invalid_arg,
                            "create_typedarray: not an ArrayBuffer");
      return napi_invalid_arg;
    }
    base = ab;
    // R4: root the ArrayBuffer for the create call.
    // (base is borrowed from the caller's scope; hold a ScratchRoot for
    // the JsCreateTypedArray window.)
  } else {
    // Standalone: byte_offset must be 0 (JsCreateTypedArray contract).
    if (byte_offset != 0) {
      nodeapi::SetLastError(env, napi_invalid_arg,
                            "create_typedarray: offset with no buffer");
      return napi_invalid_arg;
    }
  }
  ScratchRoot baseRoot(base != JS_INVALID_REFERENCE ? base : JS_INVALID_REFERENCE);
  JsValueRef ta = JS_INVALID_REFERENCE;
  JsErrorCode ec = JsCreateTypedArray(jsType, base, off32, len32, &ta);
  if (ec != JsNoError) return MapJsError(env, ec, "JsCreateTypedArray");
  ScratchRoot taRoot(ta);
  napi_status rs = RootAndPublish(env, ta, result);
  if (rs != napi_ok) return rs;
  nodeapi::ClearLastError(env);
  return napi_ok;
}

// ---------------------------------------------------------------------
// napi_get_typedarray_info (C Yes: executes, composition §2.4)
// ---------------------------------------------------------------------

napi_status NAPI_CDECL napi_get_typedarray_info(napi_env env,
                                                napi_value typedarray,
                                                napi_typedarray_type* type,
                                                size_t* length,
                                                void** data,
                                                napi_value* arraybuffer,
                                                size_t* byte_offset) {
  if (typedarray == nullptr) {
    if (env) nodeapi::SetLastError(env, napi_invalid_arg,
                                    "get_typedarray_info: typedarray is NULL");
    return napi_invalid_arg;
  }
  nodeapi::EntryGuard guard(env, /*allowClosing=*/false,
                            /*allowPending=*/true, /*needContext=*/true);
  if (!guard.ok()) return guard.status();
  DrainExternalFinalizers(env);
  JsValueRef ta = ToJs(typedarray);
  JsValueType t = JsUndefined;
  JsErrorCode ec = JsGetValueType(ta, &t);
  if (ec != JsNoError) return MapJsError(env, ec, "JsGetValueType");
  if (t != JsTypedArray) {
    nodeapi::SetLastError(env, napi_invalid_arg,
                          "get_typedarray_info: not a TypedArray");
    return napi_invalid_arg;
  }
  ScratchRoot taRoot(ta);

  JsTypedArrayType jsType = JsArrayTypeUint8;
  JsValueRef jsAB = JS_INVALID_REFERENCE;
  unsigned int jsOff = 0;
  unsigned int jsByteLen = 0;
  ec = JsGetTypedArrayInfo(ta, &jsType, &jsAB, &jsOff, &jsByteLen);
  if (ec != JsNoError) return MapJsError(env, ec, "JsGetTypedArrayInfo");
  ScratchRoot abRoot(jsAB);

  napi_typedarray_type napiType;
  if (!JsTypeToNapi(jsType, &napiType)) {
    nodeapi::SetLastError(env, napi_generic_failure,
                          "get_typedarray_info: unsupported typed array type");
    return napi_generic_failure;
  }

  BYTE* storBuf = nullptr;
  unsigned int storLen = 0;
  JsTypedArrayType storType = JsArrayTypeUint8;
  int elemSize = 0;
  ec = JsGetTypedArrayStorage(ta, &storBuf, &storLen, &storType, &elemSize);
  if (ec != JsNoError) return MapJsError(env, ec, "JsGetTypedArrayStorage");
  // R4 revalidation: storBuf is already offset (GetByteBuffer includes
  // byteOffset); storLen is the view byteLength. The two calls above
  // execute no JS, but the borrow is still rooted (taRoot/abRoot).
  // Detached-view correction: JsGetTypedArrayStorage returns a tagged
  // int encoding (0x100...) for the cleared buffer after detach
  // (ClearLengthAndBufferOnDetach sets buffer to TaggedInt(0)), not a
  // null pointer. Node semantics require data==nullptr, length==0 for a
  // detached view. Detect detached via the ArrayBuffer's getter and
  // override the borrow.
  {
    bool is_det = false;
    napi_status ds = IsDetachedViaProperty(env, jsAB, &is_det);
    if (ds != napi_ok) return ds;
    if (is_det) {
      storBuf = nullptr;
      storLen = 0;
      elemSize = static_cast<int>(ElemSizeForJs(jsType));
    }
  }
  if (elemSize <= 0) {
    nodeapi::SetLastError(env, napi_generic_failure,
                          "get_typedarray_info: bad element size");
    return napi_generic_failure;
  }
  size_t elemCount = static_cast<size_t>(storLen) / static_cast<size_t>(elemSize);

  if (type != nullptr) *type = napiType;
  if (length != nullptr) *length = elemCount;
  if (data != nullptr) *data = reinterpret_cast<void*>(storBuf);
  if (arraybuffer != nullptr) {
    // Root the buffer into the innermost scope as the out-param value.
    napi_status rs = NodeApiScopes_RootIntoInnermost(env, FromJs(jsAB));
    if (rs != napi_ok) {
      nodeapi::SetLastError(env, rs, "get_typedarray_info: buffer root failed");
      return rs;
    }
    *arraybuffer = FromJs(jsAB);
  }
  if (byte_offset != nullptr) *byte_offset = static_cast<size_t>(jsOff);
  nodeapi::ClearLastError(env);
  return napi_ok;
}

// ---------------------------------------------------------------------
// napi_create_dataview (C No: gated)
// ---------------------------------------------------------------------

napi_status NAPI_CDECL napi_create_dataview(napi_env env, size_t length,
                                            napi_value arraybuffer,
                                            size_t byte_offset,
                                            napi_value* result) {
  if (result == nullptr) {
    if (env) nodeapi::SetLastError(env, napi_invalid_arg,
                                    "create_dataview: result is NULL");
    return napi_invalid_arg;
  }
  nodeapi::EntryGuard guard(env, /*allowClosing=*/false,
                            /*allowPending=*/false, /*needContext=*/true);
  if (!guard.ok()) return guard.status();
  DrainExternalFinalizers(env);
  unsigned int len32 = 0;
  napi_status vs = ValidateSizeToUInt(env, length, &len32,
                                      "create_dataview: length too large");
  if (vs != napi_ok) return vs;
  unsigned int off32 = 0;
  vs = ValidateSizeToUInt(env, byte_offset, &off32,
                          "create_dataview: offset too large");
  if (vs != napi_ok) return vs;

  JsValueRef ab = JS_INVALID_REFERENCE;
  bool needTempAB = false;
  if (arraybuffer != nullptr) {
    JsValueRef candidate = ToJs(arraybuffer);
    JsValueType t = JsUndefined;
    JsErrorCode te = JsGetValueType(candidate, &t);
    if (te != JsNoError) return MapJsError(env, te, "JsGetValueType");
    if (t != JsArrayBuffer) {
      nodeapi::SetLastError(env, napi_invalid_arg,
                            "create_dataview: not an ArrayBuffer");
      return napi_invalid_arg;
    }
    ab = candidate;
  } else {
    // Standalone DataView (no backing supplied): allocate offset+length.
    uint64_t total = static_cast<uint64_t>(byte_offset) + static_cast<uint64_t>(length);
    if (total > std::numeric_limits<unsigned int>::max()) {
      nodeapi::SetLastError(env, napi_invalid_arg,
                            "create_dataview: total too large");
      return napi_invalid_arg;
    }
    JsErrorCode ce = JsCreateArrayBuffer(static_cast<unsigned int>(total), &ab);
    if (ce != JsNoError) return MapJsError(env, ce, "JsCreateArrayBuffer");
    needTempAB = true;
  }
  ScratchRoot abRoot(ab);
  if (needTempAB) {
    // The temp buffer will be borrowed by the DataView but not published;
    // the DataView's own edge keeps it alive (GC-tracked). No extra root
    // beyond abRoot for the create call is needed.
  } else {
    // V8 throws RangeError when offset+length exceeds the buffer; JsRT
    // silently clamps/accepts, so validate here (vendored test_dataview).
    BYTE* storage = nullptr;
    unsigned int storage_len = 0;
    JsErrorCode se = JsGetArrayBufferStorage(ab, &storage, &storage_len);
    if (se != JsNoError) return MapJsError(env, se, "JsGetArrayBufferStorage");
    uint64_t end = static_cast<uint64_t>(off32) + static_cast<uint64_t>(len32);
    if (end > static_cast<uint64_t>(storage_len)) {
      // Create a RangeError exception and return pending (Node behavior).
      // napi_create_range_error + napi_throw are the approved (D8) path.
      napi_value err = nullptr;
      napi_value msg = nullptr;
      napi_status cs = napi_create_string_utf8(
          env, "Invalid DataView length", NAPI_AUTO_LENGTH, &msg);
      if (cs != napi_ok) return cs;
      napi_status rs2 = napi_create_range_error(env, nullptr, msg, &err);
      if (rs2 != napi_ok) return rs2;
      napi_status ts = napi_throw(env, err);
      if (ts != napi_ok) return ts;
      nodeapi::SetLastError(env, napi_pending_exception,
                            "create_dataview: range error");
      return napi_pending_exception;
    }
  }
  JsValueRef dv = JS_INVALID_REFERENCE;
  JsErrorCode ec = JsCreateDataView(ab, off32, len32, &dv);
  if (ec != JsNoError) return MapJsError(env, ec, "JsCreateDataView");
  ScratchRoot dvRoot(dv);
  napi_status rs = RootAndPublish(env, dv, result);
  if (rs != napi_ok) return rs;
  nodeapi::ClearLastError(env);
  return napi_ok;
}

// ---------------------------------------------------------------------
// napi_get_dataview_info (C Yes: executes)
// ---------------------------------------------------------------------

napi_status NAPI_CDECL napi_get_dataview_info(napi_env env,
                                              napi_value dataview,
                                              size_t* bytelength,
                                              void** data,
                                              napi_value* arraybuffer,
                                              size_t* byte_offset) {
  if (dataview == nullptr) {
    if (env) nodeapi::SetLastError(env, napi_invalid_arg,
                                    "get_dataview_info: dataview is NULL");
    return napi_invalid_arg;
  }
  nodeapi::EntryGuard guard(env, /*allowClosing=*/false,
                            /*allowPending=*/true, /*needContext=*/true);
  if (!guard.ok()) return guard.status();
  DrainExternalFinalizers(env);
  JsValueRef dv = ToJs(dataview);
  JsValueType t = JsUndefined;
  JsErrorCode ec = JsGetValueType(dv, &t);
  if (ec != JsNoError) return MapJsError(env, ec, "JsGetValueType");
  if (t != JsDataView) {
    nodeapi::SetLastError(env, napi_invalid_arg,
                          "get_dataview_info: not a DataView");
    return napi_invalid_arg;
  }
  ScratchRoot dvRoot(dv);

  BYTE* storBuf0 = nullptr;
  unsigned int storLen0 = 0;
  ec = JsGetDataViewStorage(dv, &storBuf0, &storLen0);
  if (ec != JsNoError) return MapJsError(env, ec, "JsGetDataViewStorage");

  // ArrayBuffer + byteOffset have no JsGetDataViewInfo equivalent; read via
  // the builtin accessors (JS-executing). R4: revalidate storage after.
  JsPropertyIdRef pidBuf = JS_INVALID_REFERENCE;
  napi_status s = MakeIdMap(env, "buffer", &pidBuf, "get_dataview_info: buffer id");
  if (s != napi_ok) return s;
  JsValueRef abVal = JS_INVALID_REFERENCE;
  ec = JsGetProperty(dv, pidBuf, &abVal);
  if (ec != JsNoError) return MapJsError(env, ec, "get_dataview_info: buffer get");
  ScratchRoot abRoot(abVal);
  {
    JsValueType bt = JsUndefined;
    JsErrorCode be = JsGetValueType(abVal, &bt);
    if (be != JsNoError) return MapJsError(env, be, "get_dataview_info: buffer type");
    if (bt != JsArrayBuffer) {
      nodeapi::SetLastError(env, napi_invalid_arg,
                            "get_dataview_info: buffer not ArrayBuffer");
      return napi_invalid_arg;
    }
  }

  JsPropertyIdRef pidOff = JS_INVALID_REFERENCE;
  s = MakeIdMap(env, "byteOffset", &pidOff, "get_dataview_info: offset id");
  if (s != napi_ok) return s;
  JsValueRef offVal = JS_INVALID_REFERENCE;
  ec = JsGetProperty(dv, pidOff, &offVal);
  if (ec != JsNoError) return MapJsError(env, ec, "get_dataview_info: offset get");
  ScratchRoot offRoot(offVal);
  double offD = 0.0;
  ec = JsNumberToDouble(offVal, &offD);
  if (ec != JsNoError) return MapJsError(env, ec, "get_dataview_info: offset num");
  if (offD < 0 || offD > static_cast<double>(std::numeric_limits<unsigned int>::max())) {
    nodeapi::SetLastError(env, napi_generic_failure,
                          "get_dataview_info: offset out of range");
    return napi_generic_failure;
  }
  unsigned int off = static_cast<unsigned int>(offD);

  // Revalidate storage after the JS-executing gets (R4).
  BYTE* storBuf = nullptr;
  unsigned int storLen = 0;
  ec = JsGetDataViewStorage(dv, &storBuf, &storLen);
  if (ec != JsNoError) return MapJsError(env, ec, "JsGetDataViewStorage");
  // Optional byteLength property is not needed; storage is authoritative.

  if (bytelength != nullptr) *bytelength = static_cast<size_t>(storLen);
  if (data != nullptr) *data = reinterpret_cast<void*>(storBuf);
  if (arraybuffer != nullptr) {
    napi_status rs = NodeApiScopes_RootIntoInnermost(env, FromJs(abVal));
    if (rs != napi_ok) {
      nodeapi::SetLastError(env, rs, "get_dataview_info: buffer root failed");
      return rs;
    }
    *arraybuffer = FromJs(abVal);
  }
  if (byte_offset != nullptr) *byte_offset = static_cast<size_t>(off);
  nodeapi::ClearLastError(env);
  return napi_ok;
}

}  // extern "C"
