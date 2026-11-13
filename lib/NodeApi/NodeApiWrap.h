//-------------------------------------------------------------------------------------------------------
// Copyright (c) ChakraCore Project Contributors. All rights reserved.
// Licensed under the MIT license. See LICENSE.txt file in the project root for full license information.
//-------------------------------------------------------------------------------------------------------
//
// Externals + finalizer queue + wrap/type-tags/add-finalizer (plan
// doc/node-api-plan.md §3.7, tasks N3.2/N3.3; semantics
// doc/node-api-semantics.md §4 R-WRAP-1..12, probes P7-*).
//
// Implements the v8 A3 wrap surface:
//   napi_create_external / napi_get_value_external,
//   napi_wrap / napi_unwrap / napi_remove_wrap / napi_add_finalizer,
//   napi_type_tag_object / napi_check_object_type_tag.
//
// Design summary (see NodeApiWrap.cpp for the full ownership record):
//   - Every external/wrapped value allocates a heap WrapRecord
//     {env, user_data, finalize, hint, tag?, weak handle,
//      exactly-once state} on provider native memory (C++ new/delete).
//     JsCreateExternalObject's single data+callback slot always carries
//     the provider trampoline + record pointer, never the user's
//     directly (plan §1.4 audit: the callback is cached per
//     (callback, prototype) and fires with no current context).
//   - GC-side trampolines (JsFinalizeCallback for externals,
//     JsObjectBeforeCollectCallback for wrapped arbitrary objects) do
//     ONLY: CAS the record exactly-once state LIVE->QUEUED, then push
//     the record onto the env's mutex-protected MPSC queue
//     (std::mutex + std::deque, single env-thread consumer; chosen
//     because the provider is C++ -- std::mutex -- per the brief).
//     No JS calls, no user finalizer, no side-table mutation. The
//     queue lock is never held across user code (R-ENV-12).
//   - DrainFinalizerQueue runs on the env thread: drain-until-empty
//     (R-WRAP-8). Drain points: entry to every wrap napi_* entry in
//     this TU (cheap atomic empty check first), destroy step 2
//     (NodeApiEnv.cpp) and destroy step 6 backstop (TeardownWraps),
//     plus the test hook chakra_nodeapi_test_drain_finalizers.
//     Drained records remove their side-table/external entry, then
//     invoke napi_finalize(env, data, hint) with a live env (during
//     Closing the env is still valid; JS-calling APIs from the
//     finalizer return napi_generic_failure per R-ENV-4).
//   - Wrap side table: per-env map keyed by the JS object pointer
//     (JsValueRef bits), ABA-guarded by validating
//     JsGetWeakReferenceValue(record.weak_handle) == object before
//     trusting a lookup (R-WRAP-11), with records removed at drain
//     (not in the GC trampoline) so the map is never mutated off the
//     env thread. Type tags are stored by value in the record.
//   - Env state is held in a provider-global map keyed by the env
//     pointer value (lazy-created on first wrap call, erased at
//     teardown), so NodeEnv's layout (owned by N1.1) is untouched.
//     This mirrors the NodeApiLifetime async-pump map pattern.
//
// Before-collect reliability evidence (AMB-WRAP-2, plan §3.7/§4.3):
//   JsSetObjectBeforeCollectCallback (lib/Jsrt/ChakraCommon.h,
//   implemented in lib/Jsrt/Jsrt.cpp + lib/Common/Memory/Recycler.cpp)
//   registers a per-object callback that Recycler invokes during the
//   mark phase iff the object is unmarked (IsObjectMarked == false),
//   staying registered while reachable. That is exactly the delivery
//   this side table needs for arbitrary objects, and the engine probe
//   (test/nodeapi/probes/wrap_probe.cpp P7-8 churn + P7-2/P7-10
//   exactly-once after JsCollectGarbage) exercises it on each
//   profile. No JsRT weak-cell lane is filed. Residual limit: the
//   callback fires during mark on the GC thread (the thread running
//   collection) with IsInObjectBeforeCollectCallback set, so most JsRT
//   APIs fail with JsErrorInObjectBeforeCollectCallback there --
//   which is why the trampoline touches only the atomic state and
//   the native queue. Rental threading (memory-management.md §2)
//   forbids concurrent runtime use, so teardown while another thread
//   is inside GC is host misuse; if before-collect delivery were
//   unreliable the probe would fail rather than be faked.
//
// Pending-exception discipline (doc/node-api-status-mapping.md C
// column, like NodeApiRefs/Values):
//   Executes: napi_add_finalizer, napi_get_value_external.
//   Gated: napi_wrap, napi_unwrap, napi_remove_wrap,
//   napi_create_external, napi_type_tag_object,
//   napi_check_object_type_tag.
//   allowClosing: creators/writers (create_external, wrap,
//   add_finalizer, type_tag_object) are strict Alive-only; getters/
//   removers (get_value_external, unwrap, remove_wrap,
//   check_object_type_tag) allow Closing (finalizers/hooks may read
//   during teardown, parity with refs get/delete).
//
// Visibility: napi_* declarations carry NAPI_EXTERN default
// visibility from the vendored headers; helpers below are C-linkage
// but NOT exported from release libChakraNodeApi (the build uses
// -fvisibility=hidden and these helpers carry no default attribute,
// so they stay hidden; the N1.6 exports check forbids extras).

#ifndef CHAKRA_NODEAPI_WRAP_H_
#define CHAKRA_NODEAPI_WRAP_H_

#include "js_native_api.h" /* napi_env, napi_value, napi_ref, NAPI_VERSION=8 */
#include "ChakraCommon.h"  /* JsRuntimeHandle, JsContextRef */

#ifdef __cplusplus
extern "C" {
#endif

/* Drain the finalizer queue until empty on the env thread (R-WRAP-8).
 * Pops one record under the queue lock, removes its container entry,
 * then invokes the user napi_finalize WITHOUT the lock held (R-ENV-12).
 * Finalizers enqueued by reentrant GC inside a user finalizer are
 * picked up by the same loop (drain-until-empty). Null/dead env is a
 * no-op (registry lookup by value). The caller must hold a live env
 * on the owner thread with the env context entered (wrap entries do
 * this via EntryGuard/ContextGuard; destroy does via its guard). */
void nodeapi_wrap_drain(napi_env env);

/* Drain + backstop for destroy steps 2/6 (called by NodeApiEnv.cpp):
 * first drain until empty (step 2), then run every remaining live
 * record's finalizer exactly once and free their container entries
 * (step 6, R-ENV-7). Null/dead env is a no-op. After return the env's
 * wrap state is freed and unregistered. */
void nodeapi_wrap_teardown(napi_env env);

/* Test-hook helpers (semantics P7-*). Declared only when
 * CHAKRA_NODEAPI_TEST_HOOKS is defined (probe runners define it),
 * mirroring NodeApiScopes/Refs so release libChakraNodeApi never
 * exports them. All consult the wrap state by env pointer value and
 * never dereference dead/fabricated values (null/dead => 0). */
#ifdef CHAKRA_NODEAPI_TEST_HOOKS
/* Drain on demand (explicit host-pump drain for P7-2/P7-5/P7-11). */
void chakra_nodeapi_test_drain_finalizers(napi_env env);
/* Live wrap/drop records still owned (side-table + external records;
 * queued-but-undrained records still count as live). 0 for null/dead. */
size_t chakra_nodeapi_test_wrap_live_count(napi_env env);
/* Records currently queued for drain. 0 for null/dead. */
size_t chakra_nodeapi_test_wrap_queue_count(napi_env env);
#endif /* CHAKRA_NODEAPI_TEST_HOOKS */

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* CHAKRA_NODEAPI_WRAP_H_ */
