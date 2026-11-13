//-------------------------------------------------------------------------------------------------------
// Copyright (c) ChakraCore Project Contributors. All rights reserved.
// Licensed under the MIT license. See LICENSE.txt file in the project root for full license information.
//-------------------------------------------------------------------------------------------------------
//
// Node-API script execution, version and external memory
// (plan doc/node-api-plan.md task N3.5, A6 script/version + B7;
// status rows in doc/node-api-status-mapping.md C column).
//
// V8 surface implemented here:
//   napi_run_script / napi_adjust_external_memory
// napi_get_version / napi_get_node_version are NOT reimplemented here:
//   they already landed in NodeApiEnv.cpp (Wave 1, N1.1) and are
//   registered in the exports inventory. Their shapes were verified
//   against N3.5 (see below); duplicating them would cause a
//   duplicate-symbol link break. This TU only records their shapes in
//   the probe (promise_script_probe.cpp asserts version==8 and the
//   provider version struct).
//
// Verified landed shapes (Wave 1, NodeApiEnv.cpp:700-743):
//   - napi_get_version(node_api_basic_env, uint32_t*) -> 8. Matches
//     N3.5 ("8, from N1.1") and plan SS4 A6. Delta: none. It is a
//     basic_env entry that validates (CheckAlive) but issues no Js*
//     call, executes with pending (C-Yes), and clears last_error on
//     success (AMB-STATUS-1). The basic_env spelling (same pointer
//     type as napi_env) is header-correct for GC-safe entries.
//   - napi_get_node_version(node_api_basic_env,
//     const napi_node_version**) -> &{1,0,0,"libChakraNodeApi/1.0.0"}.
//     Matches N3.5 + B7 ("provider truth, not a Node runtime claim;
//     release identifies the Chakra provider"). Delta: none. Same
//     basic_env/CheckAlive/pending-execute discipline as above.
//     The out-param is a borrowed provider-owned static struct (valid
//     for the process lifetime, never freed); addons must not free it.
//   - No shape delta requires a fix. Both are already in
//     tools/build/gen_nodeapi_exports.py WAVE1_LANDED and in
//     tools/build/nodeapi_exports.txt.
//
// napi_adjust_external_memory audit (audit #6): provider-side
// accounting ONLY. This TU NEVER calls `JsSetRuntimeMemoryLimit`
// (`ChakraCommon.h:909`, implemented in Jsrt.cpp:3280 as an
// AllocationPolicyManager limit) and NEVER calls
// `JsCollectGarbage`. A runtime memory limit is a hard OOM gate for
// the whole runtime; wiring a per-addon accounting hint into it would
// turn a memory-accounting call into an allocator denial, which is
// not Node semantics. No test justifies a GC trigger, so the
// "optionally trigger JsCollectGarbage on large increases" clause is
// deliberately not implemented (accounting-only, documented here).
// The returned total is the per-env accumulated sum (change added to
// the previous total, initial 0). The counter is provider native
// memory in ScriptState, never Recycler memory.
//
// Ownership record (memory-management.md SS3; plan N3.5 "required"):
//
//   Resource / creation call / source location:
//     1) ScriptState (opaque struct, one per env), created by
//        `NodeApiScript_CreateState` from `chakra_nodeapi_create_env`
//        (NodeApiEnv.cpp), freed by `NodeApiScript_FreeState` at
//        destroy step 6 after `NodeApiScript_DestroyForEnv` (no roots
//        to release; the struct just holds the int64 total + the
//        fixed source-context cookie).
//     2) Per-run script temporaries: the sourceUrl JsValueRef created
//        per `napi_run_script` call via `JsCreateString` (provider
//        sourceUrl "nodeapi"), used once for `JsRun`, then released.
//        No retention.
//     3) The `napi_run_script` result `napi_value` (rooted into the
//        innermost scope on success, like every other value lane).
//        On script-exception failure nothing is published (the
//        *result out-param is left untouched) and the exception stays
//        pending.
//
//   Domain and allocator instance (including backing allocator lifetime):
//     (1) Provider native heap (backing lifetime == env lifetime).
//        The int64 total is plain native memory, never a JsValueRef.
//     (2) Recycler heap (one Js string per run, released immediately).
//     (3) Recycler heap via scope slot (scope-stack lifetime).
//
//   Physical allocation base, extent, alignment:
//     One heap allocation per ScriptState; per-run one string object
//     (transient); per successful run one scope slot for the result.
//
//   Logical length / borrowed views:
//     (1) `external_total` is the logical accounting total (bytes,
//        signed, initial 0). It is kept separate from any allocation
//        descriptor (the provider owns no external buffer bytes for
//        this lane). (2) sourceUrl borrows nothing (owned string).
//
//   Owner; transfer/publication point; remaining aliases:
//     (1) Owned by the env (NodeEnv::script_state); published to the
//        two napi_* entries via the registry-resolved
//        `NodeApiEnv_ScriptState` accessor (pointer VALUE lookup).
//        The int64 total is mutated only by
//        `napi_adjust_external_memory` on the owner thread.
//     (2) Owned by the call frame (transient root via JsCreateString,
//        released before return on every path).
//     (3) Owned by the caller's scope until scope close / destroy.
//
//   Borrow invalidation: GC / detach / growth / mutation / reset / close /
//     disposal:
//     (1) dies at destroy step 6 (struct freed; total discarded).
//        The total is per-env: a new env starts at 0. (Shared-context
//        accumulation across envs is a documented per-env choice:
//        the plan's "per-runtime" wording is approximated per-env
//        because env==context in the Node registration model
//        (plan SS1.3: one env per context x registration) and the
//        probe uses one env per context; a future multi-env shared
//        context would need a runtime-keyed counter, filed as a
//        follow-up, not probed here.)
//     (2) dies at return (released). (3) scope close / destroy.
//
//   Potential user-code or reentrancy points:
//     `napi_run_script` runs arbitrary JS (`JsRun`): globals,
//        getters, valueOf, etc. The script value itself is validated
//        as a string BEFORE the run (no coercion, so no user code runs
//        during validation). The sourceUrl string creation does not
//        run user code. The result rooting happens synchronously
//        after the run with no intervening JS.
//
//   Thread/context allowed for use, root changes and destruction:
//     Owner thread only, env context entered for `napi_run_script`
//     (plan SS3.2/R3, via EntryGuard needContext=true). The adjust
//     entry is native-only (needContext=false, basic_env level, like
//     the landed versions) but still owner-thread-checked (R-ENV-11)
//     and allows Closing (teardown-compatible: external-buffer
//     finalizers that run during Closing may release accounting).
//
//   Release on success, partial failure, exception, OOM, cancel and shutdown:
//     Run: success -> root result, publish, clear last_error, ok.
//        Script exception (compile or runtime, JsErrorScriptCompile /
//        ScriptException / InExceptionState) -> pending with the
//        exception left pending (never cleared), *result untouched,
//        last_error recorded. Non-string input -> string_expected
//        before any run (no pending fabricated). Pending already
//        present -> gated (napi_pending_exception without running).
//     Adjust: success -> total updated, *adjusted_value written,
//        last_error cleared, ok. NULL adjusted_value -> invalid_arg
//        with no counter mutation. No JsRT call, so no partial
//        state.
//
//   Memory accounting, capacity/retention bound and exhaustion behavior:
//     (1) one int64 per env (O(1)). No bound on the total's magnitude
//        (signed 64-bit, addition wraps by the host's addon use; the
//        provider does not clamp). No root-count contribution (no JS
//        values held). The probe asserts exact accumulation
//        (+100=100, +200=300, -50=250, repeat +100=350).
//
//   Tests / commands / measured results / remaining uncertainty:
//     test/nodeapi/probes/promise_script_probe.cpp (run_script value +
//     globals visibility + SyntaxError/TypeError class + pending
//     persistence + string_expected + adjust accumulation + versions
//     landed-shape assertions) run via
//     test/nodeapi/probes/run_promise_script_probe.sh (test + jit).
//     Residuals: sourceUrl is fixed ("nodeapi") and the
//     source-context cookie is fixed (NAPI-tagged); reusing one
//     SourceContextInfo is intentional (same URL, fresh source text
//     each run). `napi_run_script` returns the completion value of
//     the last statement (JsRun semantics == Node's global-scope
//     evaluation). Linux x64 not claimed (Mac only, N5.5).
//
// Language: C++17 (GNU++17 via `zig c++`, matching Wave 1/2) behind
// the extern "C" napi_* surface (PR-4 decision record: C++ through
// provider v1). No C++ exception crosses the extern "C" surface.

#ifndef CHAKRA_NODEAPI_SCRIPT_H_
#define CHAKRA_NODEAPI_SCRIPT_H_

#include "js_native_api.h" /* napi_env, napi_value, NAPI_VERSION=8 */

#ifdef __cplusplus
extern "C" {
#endif

/* --------------------------------------------------------------------------
 * Per-env script state (opaque to N1.1; see NodeApiRefs.h pattern).
 * Created by NodeApiScript_CreateState() at env creation, destroyed at
 * destroy step 6 (NodeApiScript_DestroyForEnv + FreeState). Env owner
 * may consult NodeApiScript_ExternalTotal() before destroy.
 * ------------------------------------------------------------------------ */
struct NodeApiScriptState;

/* Creates fresh script state (external total 0). Provider-owned; see
 * NodeApiScript_FreeState. Never returns null on success (aborts via
 * bad_alloc only if the provider cannot allocate env state at all;
 * chakra_nodeapi_create_env treats that as fatal OOM rollback). */
NodeApiScriptState* NodeApiScript_CreateState(void);

/* Destroy step 6 (plan SS3.6): no roots to release (the total is
 * native memory). Frees nothing itself (see NodeApiScript_FreeState);
 * this is the teardown hook for symmetry with Refs/Promise and to
 * keep the destroy order explicit. Must be called on the env thread
 * (no context enter needed; native-only). Idempotent-safe. */
void NodeApiScript_DestroyForEnv(napi_env env);

/* Frees the NodeApiScriptState struct itself, after
 * NodeApiScript_DestroyForEnv. Null-safe. Must be called on the env
 * thread. */
void NodeApiScript_FreeState(NodeApiScriptState* state);

/* ----- N1.1 accessor (implemented by NodeApiEnv.cpp via the live
 * registry). Returns the per-env script state (created once at env
 * creation, destroyed at destroy step 6). NULL for NULL/dead env
 * (checked by pointer VALUE only, never dereferencing). ----- */
NodeApiScriptState* NodeApiEnv_ScriptState(napi_env env);

/* Current accumulated external-memory total for the env (0 for
 * NULL/dead env). Hidden (probe link only; the napi_* entry is the
 * public surface). */
int64_t NodeApiScript_ExternalTotal(napi_env env);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* CHAKRA_NODEAPI_SCRIPT_H_ */
