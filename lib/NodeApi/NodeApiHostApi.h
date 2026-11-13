//-------------------------------------------------------------------------------------------------------
// Copyright (c) ChakraCore Project Contributors. All rights reserved.
// Licensed under the MIT license. See LICENSE.txt file in the project root for full license information.
//-------------------------------------------------------------------------------------------------------
//
// Private Node-API host API (plan doc/node-api-plan.md §3.2, PR-3/PR-5/PR-8).
//
// This header is PRIVATE: it is the embedder-facing factory for libChakraNodeApi
// (Node-API has no public napi_create_env, plan §1.3). It is NOT part of the
// public Node-API surface, must NOT be installed with the public JsRT headers
// (lib/Jsrt/Chakra*.h), and must NOT collide with napi_*/node_api_* symbols.
// It is compiled by the host and the provider with:
//
//   -I include/nodeapi -I lib/Jsrt -I lib/NodeApi -DNAPI_VERSION=8
//
// and is required to compile standalone against the vendored Node-API headers
// (include/nodeapi/, Node 22.22.3, PR-2) and the JsRT headers
// (lib/Jsrt/ChakraCommon.h / ChakraCore.h). <uv.h> is never included: the
// libuv loop is an opaque forward-declared pointer, injected by the host.
//
// Ownership / lifetime record (memory-management.md §3):
//
//   Resource / creation call: napi_env from chakra_nodeapi_create_env.
//   Domain and allocator: provider-owned NodeEnv in provider native memory
//     (never Recycler memory; plan §3.3). Freed only by
//     chakra_nodeapi_destroy_env on the env thread (plan §3.6 step 7).
//   Owner; transfer/publication point: provider owns the env; the host owns
//     the JsRuntimeHandle + JsContextRef and hands them to the provider at
//     creation. The provider never takes ownership of runtime/context.
//   Borrow invalidation: env is valid Alive -> Closing -> Dead (plan §3.6).
//     After destroy the pointer is dead (host use-after-free; no provider
//     validation through the freed pointer, semantics R-ENV-10). All JsValueRef
//     roots held by the env are released before JsDisposeRuntime (R5).
//   Thread/context allowed: create, destroy, and every napi_* entry run on
//     the env (= host) thread with that thread's context set (plan §3.2).
//     No napi_env / napi_value / napi_ref use from worker threads (R3).
//   Release on success / failure / shutdown: destroy_env is the sole release
//     authority; it runs the §3.6 sequence (finalizer drain -> sync cleanup
//     hooks LIFO -> async hooks + host pump -> instance finalizer -> refs /
//     scopes / wrap backstop -> free NodeEnv) before the host may dispose the
//     Chakra context/runtime. Double destroy is a host error (assert in debug,
//     no-op in release; the void signature cannot return a status).
//   Tests: N1.1 (1000x create/destroy, wrong-thread / Closing / Dead probes
//     P1-* in doc/node-api-semantics.md §1.5); host loader proof in N0.3.
//   Remaining uncertainty: exact §3.6 step order is checked against vendored
//     test_finalizer / test_cleanup_hook / test_async_cleanup_hook (N1.4).
//
// Promise-pump contract (PR-8, normative):
//
//   - The PROVIDER installs the JsRT promise-continuation callback (or the
//     host may install it first; the provider installs only if the host has
//     not, so it never clobbers a host-installed callback). The provider
//     NEVER spins its own pump loop and NEVER blocks waiting for jobs.
//   - The HOST owns pumping: after running a script (JsRun) and during env
//     teardown (plan §3.6 step 4, async cleanup hooks), the host must drain
//     queued continuations until quiet (e.g. call queued tasks via
//     JsCallFunction and/or JsIdle until no new tasks arrive). The test host
//     (tools/nodeapi/ch-nodeapi) pumps explicitly with a bounded iteration
//     count; a hang is reported as a host test failure, never a provider
//     deadlock. Embedders must do the same: provider promise/continuation
//     progress stops unless the host pumps. This holds independently of any
//     JsIdle use: JsIdle (idle processing) is a GC/memory-cleanup pump the
//     host may also call; the continuation queue is the PR-8 pump.
//
// Host services (PR-7): every field of chakra_nodeapi_host_services is
// optional (NULL = service absent). Absence must surface as
// napi_generic_failure from dependent Table B functions (D7), never fake
// success. napi_get_uv_event_loop returns the injected loop or
// napi_generic_failure. post_task/post_work route async work / TSFN
// scheduling through the host event loop; the provider keeps no internal
// thread pool in v1.

#ifndef CHAKRA_NODEAPI_HOST_API_H_
#define CHAKRA_NODEAPI_HOST_API_H_

#include "js_native_api.h" /* napi_env, napi_value (NAPI_VERSION=8) */
#include "node_api_types.h" /* napi_addon_register_func (register ABI) */
#include "ChakraCommon.h" /* JsRuntimeHandle, JsContextRef */

/* The host's libuv loop, injected opaquely. Never include <uv.h> here. */
struct uv_loop_s;

#ifdef __cplusplus
extern "C" {
#endif

typedef struct chakra_nodeapi_host_services {
  /* Async work / TSFN scheduling. post_task queues fn(arg) on the env
   * thread; post_work runs execute(arg) off-thread then post_task(complete,
   * arg). Return false = cannot accept. NULL = service absent. */
  bool (*post_task)(void* host_ctx, void (*fn)(void*), void* arg);
  bool (*post_work)(void* host_ctx, void (*execute)(void*),
                    void (*complete)(void*), void* arg);
  void (*cancel_work)(void* host_ctx, void* arg); /* best effort */
  void (*ref_loop)(void* host_ctx); /* TSFN keep-alive */
  void (*unref_loop)(void* host_ctx);
  /* NULL = abort policy (napi_fatal_error / uncaught). */
  void (*fatal_exception)(void* host_ctx, napi_value exc);
  struct uv_loop_s* uv_loop; /* NULL = no uv */
} chakra_nodeapi_host_services;

/* Create an env for (runtime, context) owned by the calling thread.
 * `module_filename` may be NULL (the provider keeps its own copy).
 * `services` may be NULL (equivalent to all-NULL fields).
 * The env records the negotiated API version (provider max = 8 in v1):
 * the host resolves the addon's node_api_module_get_api_version_v1 (absent
 * => 8) and rejects requests > 8 before calling this (plan §1.3, D1).
 * Returns NULL on failure (host must treat as fatal, not pass onward).
 * The runtime/context must outlive the env; the env never disposes them. */
napi_env chakra_nodeapi_create_env(JsRuntimeHandle runtime,
                                   JsContextRef context,
                                   const char* module_filename,
                                   const chakra_nodeapi_host_services* services,
                                   void* host_ctx);

/* Destroy per the §3.6 state machine. Must be called on the env thread,
 * before JsDisposeRuntime, with no provider call in flight. Runs cleanup
 * hooks, finalizers, refs/scopes teardown, then frees NodeEnv. */
void chakra_nodeapi_destroy_env(napi_env env);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* CHAKRA_NODEAPI_HOST_API_H_ */
