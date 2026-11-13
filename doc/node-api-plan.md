# Node-API (N-API) provider — implementation plan

Status: **plan only; no implementation started.** Owner: unassigned.
Last updated: 2026-11-11. Base commit: `48b7db6d8`
(`modernize/baseline`). This revision replaces the 2026-11-11 first
draft; it folds in three audits (official Node 22 header inventory,
JsRT capability mapping, other-engine precedents). Where this plan and
an earlier draft disagree, this plan wins.

Governing docs:

- [zig-strategy.md](zig-strategy.md) — this is wedge #1 of the
  incremental C-ABI program: new code at a C-ABI boundary, engine core
  untouched.
- [memory-management.md](memory-management.md) — ownership records are
  mandatory for every lane touching GC roots, finalizers, thread
  boundaries, host callbacks, or teardown. Read it before starting any
  task marked "ownership record required".
- [modernization-plan.md](modernization-plan.md) — project gates. This
  plan must not move the regression pins in
  `tools/build/gate_counts.json`; Node-API tests run under their own
  build step (§7).

How to use this plan: a sub-agent takes one task card from §6 plus
§0–§3 and the decisions/pre-requirements it cites. Do not start a task
whose blocking PRs (§2.2) are undecided. Report drift in any cited
anchor; do not silently work from stale anchors.

---

## 0. Scope: three things called "Node-API support"

Confusing these is the main planning risk. They are different
deliverables with different owners.

| # | Thing | What it is | In this plan? |
|---|---|---|---|
| A | **Engine provider** (`js_native_api.h`) | The ECMAScript value/object/function/lifetime C surface, implemented by a JS engine. Engine-neutral by design; Node's docs state these headers exist "to allow implementations of Node-API outside of Node.js". | **Yes — the core deliverable.** |
| B | **Runtime/host functions** (`node_api.h` beyond the JS surface) | Buffer construction, async work, thread-safe functions, libuv loop access, cleanup hooks, module registration support. Meaningful only relative to a host that owns an event loop, a `Buffer` global, and a module loader. | **Partially** — only under the explicit host contract in §3.2; otherwise functions return `napi_generic_failure`, never fake success. |
| C | **A Node.js runtime on ChakraCore** | `require`/CommonJS, `process`, Node built-ins, npm, a `node` executable. | **No.** Separate project; not implied by A or B. Anyone claiming "Node runs on ChakraCore" from this plan is overstating it. |

### 0.1 What we build

A shared library (working name `libChakraNodeApi`) that:

- exports the Node-API v8 C ABI (`napi_*` / `node_api_*` symbols,
  §4) implemented on ChakraCore's JsRT C API;
- exports a small **private** host API (§3.2) with which an embedder
  creates/destroys a `napi_env` bound to a Chakra runtime+context —
  necessary because Node-API has no public env-creation function;
- is loaded by a host (our test host first, real embedders later)
  which dlopens addons, checks their requested API version, and calls
  their `napi_register_module_v1` export.

### 0.2 What we never build here

- `require`, module resolution, `process`, Node built-ins, timers.
- The V8 C++ API or NAN support. Addons including `<v8.h>`/`<node.h>`
  are out of scope permanently.
- A `node` executable or npm flow.

### 0.3 Definition of success

1. Every non-experimental Node-API **v8 engine-surface** function
   (§4, Table A) is implemented and passes the vendored Node
   `test/js-native-api` subset selected in N5.1, with pass/skip/fail
   counts published per platform (skips carry reasons; "0 failures"
   next to unexplained skips is not success).
2. The node-addon-examples ladder (N5.2) runs against the provider
   and, differentially, against real Node with the same addon binary.
3. Host-surface functions (§4, Table B) behave exactly as the §3.2
   host contract specifies: real behavior where a host service exists,
   `napi_generic_failure` where it does not.
4. No regression: existing gates stay at their pins; Linux x64
   behavior is unchanged (the provider is new code; any shared-code
   edit follows the D6 rule in §9).

---

## 1. Validated facts

### 1.1 Official surface (Node 22.22.3 headers, counted)

Source: the installed official headers
`include/node/{js_native_api.h, js_native_api_types.h, node_api.h, node_api_types.h}`
under Node 22.22.3. Counts are declaration counts, re-derived in N0.1.

- `js_native_api.h`: 126 declarations; **115 visible at
  `NAPI_VERSION=8`** (rest: 3 experimental, 3 v9-only, 5 v10-only).
- `node_api.h`: 32 declarations; **30 at v8**.
- **Provider target at v8: 145 symbols.** Two are not
  `napi_status`-returning and need explicit treatment, not stubs:
  `napi_module_register` (void, deprecated registration) and
  `napi_fatal_error` (noreturn).
- Defining `NODE_API_NO_EXTERNAL_BUFFERS_ALLOWED` removes 2 symbols;
  the provider must never define it.

### 1.2 Version facts

- `NAPI_VERSION` defaults to **8** when undefined
  (`js_native_api_types.h`). v8 is the last strictly additive version
  and has the widest Node support matrix (12.22+/14.17+/16+ and all
  later). Baseline target is therefore **v8**.
- v9 adds 4 symbols (`node_api_symbol_for`,
  `node_api_create_syntax_error`, `node_api_throw_syntax_error`,
  `node_api_get_module_file_name`). v10 adds 6 more (external-string
  and property-key creators, `node_api_create_buffer_from_arraybuffer`)
  **and changes semantics** (`napi_create_reference` accepts all value
  types and is never weak; TSFN uncaught-exception routing). Adding
  v9/v10 symbols without the semantics is a conformance bug. Both are
  deferred to a future lane (§10); the provider must **reject addons
  requesting > 8 before registration**.
- Node keys behavior off a **per-env `module_api_version`** recorded
  at registration. Our `NodeEnv` does the same (§3.3).

### 1.3 Registration facts (who exports what)

- The **addon** exports `napi_register_module_v1(napi_env, napi_value)
  -> napi_value` and optionally
  `node_api_module_get_api_version_v1() -> int32_t` (absent ⇒ version
  defaults to 8). The provider must never export these.
- The **provider** exports the 145 `napi_*` symbols. A normally-built
  addon has them as undefined symbols; the host must load the provider
  with `RTLD_GLOBAL` (or link it into the host executable with
  exported symbols) **before** dlopening the addon.
- Registration protocol (mirroring Node's loader): check version
  symbol if present → reject > provider max → create fresh `exports`
  object → call register fn → if it returns non-null and ≠ the passed
  object, the return value replaces `exports`.
- Deprecated path: an addon may self-register via a static constructor
  calling the provider's `napi_module_register(napi_module*)`. The
  provider must implement a pending-module registry for this or such
  addons are silently lost (N4.1).
- Node creates a **new `napi_env` per (context, addon registration)**.
  One env per context shared by all addons makes instance data,
  cleanup hooks, and module filename collide. Our env model follows
  Node: env = (JsContext × addon registration), sharing the context,
  with private per-env state (§3.3).
- There is **no public `napi_create_env`**. Node's factory is internal
  and V8-bound. A private provider API is therefore required (§3.2).

### 1.4 Repo facts (JsRT capability audit, anchors verified)

Full audit detail is summarized in Appendix A. Load-bearing points:

- JsRT exports 237 symbols (`tools/build/linux_exports_so.txt`:
  `DllMain` + 236 `Js*`). No Node-API headers or `napi_*` code exist
  in-tree.
- `napi_value` can be `JsValueRef` directly (both opaque
  pointer-sized handles; `lib/Jsrt/ChakraCommon.h:373,398-407`), with
  the conditions in §3.4.
- Rooting primitive: `JsAddRef`/`JsRelease`
  (`ChakraCommon.h:989-1008`). Caveats (§3.4, R-rules): tagged numbers
  no-op; non-context refs need a thread context on the calling thread;
  arena/invalid objects silently succeed **without rooting**;
  unbalanced release is not validated. Weak references need
  `JsCreateWeakReference`/`JsGetWeakReferenceValue`
  (`ChakraCore.h:987-1004`).
- Callbacks with `this`/`newTarget` require
  `JsCreateEnhancedFunction` (`ChakraCore.h:404-409`;
  `JsNativeFunctionInfo` at `:308-328`), not plain `JsCreateFunction`
  (whose wrapper discards newTarget). In-tree oracle:
  `bin/NativeTests/JsRTApiTest.cpp:734-866` (arguments[0] == thisArg;
  argumentCount includes this).
- `JsCreateExternalObject` (`ChakraCommon.h:1517-1521`) carries
  exactly one `void* data` + one finalize callback (no env, no hint),
  cached per (callback, prototype), invoked from GC finalization with
  no current context. A trampoline + heap record is mandatory (§3.7).
- Genuinely absent in JsRT (need provider design or an engine lane,
  §4.3): BigInt create/read and BigInt `typeof` discrimination (a
  BigInt primitive reports `JsObject` from `JsGetValueType`), Date
  create/read/is-date, detached-ArrayBuffer query, freeze/seal,
  function `length` parameter, wrap/finalizer attachment to arbitrary
  objects with env-thread exactly-once delivery, BigInt64/BigUint64
  typed arrays in `JsTypedArrayInfo`, and the entire async/TSFN/uv
  lane.
- `napi_run_script` maps to a single `JsRun` (`ChakraCore.h:733-739`),
  not parse-then-run.

### 1.5 Precedent facts (other engines, condensed)

- **Hermes** (first-party, `API/napi/` on `static_h`): complete v10
  provider on engine internals. `napi_value` = pinned handle-scope
  slots with a custom GC roots function; weak refs are GC weak slots;
  finalizers are queued by GC and drained on the JS thread, never run
  inside GC; host supplies a services struct (post-work, post-task,
  loop ref/unref, fatal handler, optional uv loop) — without it async
  functions return `napi_generic_failure`. Reports 33 pass / 31 skip /
  0 fail on vendored Node tests.
- **Bun** (JSC): all 156 symbols exported; Node's `js-native-api` suite
  53/53, `node-api` suite 21 pass / 23 todo. Proves symbol coverage is
  the easy half; runtime semantics are the long tail.
- **Babylon JsRuntimeHost** (public-C-API adapters incl. a Chakra/JsRT
  one): proves a JsRT-surface provider is feasible and shows its
  ceiling — historically missing env cleanup hooks, property
  enumeration divergences. This is our closest precedent and its gaps
  are our known risks.
- **node-api-cts** (official conformance repo) splits
  `tests/js-native-api` (engine) from `tests/node-api` (runtime) and
  is explicitly work-in-progress — usable as structure, not as a gate.

Lessons encoded below: prove the GC-handle contract before API
breadth; specify teardown as a state machine; never fake async/Buffer
success; publish pass/skip/todo counts, never bare pass rates.

---

## 2. Decisions and pre-requirements

### 2.1 Decisions already made (do not re-litigate in tasks)

| ID | Decision |
|---|---|
| D1 | Baseline is **Node-API v8, engine surface first** (§4 Table A). Addons requesting > 8 are rejected at load. v9/v10 are future lanes (§10). |
| D2 | `napi_value` **is** `JsValueRef` (direct cast), under conditions §3.4. No wrapper allocation per value. |
| D3 | All provider-created JS functions use `JsCreateEnhancedFunction`; the callback thunk is §3.8. Plain `JsCreateFunction` is not used for N-API callbacks. |
| D4 | User finalizers **never run inside a GC callback**. GC-side code only enqueues; finalizers drain on the env thread at defined points (§3.7). |
| D5 | Implementation language: **Zig 0.17** (per zig-strategy wedge #1), proven by spike N0.2; if the spike fails its criteria, fall back to C++ by decision record amending this plan — a task may not silently switch languages. **Decision record 2026-11-11 (PR-4): C++ through provider v1 if G5 passes, Zig re-evaluation after (see §10).** Wave 1 (`lib/NodeApi/NodeApiEnv.*`, `NodeApiScopes.*`, `NodeApiRefs.*`, `NodeApiLifetime.*`) is C++17 behind the same C ABI (ABI-identical to a Zig port); porting now would churn the landed kernel for no ABI gain before G1. Rationale: no N0.2 Zig spike evidence for the provider surface exists, C++ is table-stakes for the JsRT C-API call sites, and the C surface is language-agnostic. Re-evaluate a Zig port only from G5 evidence with a fresh spike; a port before G5 needs its own decision record. This amends D5 per its own fallback clause and closes PR-4. **Amendment 2026-11-12 (Wave-2 review, exception discipline): C++ exceptions stay ON (-std=gnu++17, no -fno-exceptions).** `-fno-exceptions` was probed and rejected: every provider TU uses try/catch for allocation OOM (`std::vector`/`string`/`new` throwing `bad_alloc`/`length_error`) because no C++ exception may cross the `extern "C"` napi_* boundary, and `-fno-exceptions` makes `try` a compile error (verified 2026-11-12: `cannot use 'try' with exceptions disabled`), so the one-flag option would force a full alloc-site refactor with no ABI gain (N0.2 rationale: C ABI identical either way, so the flag buys no Zig-port argument). Discipline: throws originate only from C++ allocation/library code inside a TU; every `extern "C"` entry that can allocate or invoke user callbacks catches `(...)` and maps to `napi_generic_failure` with a recorded last_error, preserving any pending JS exception. |
| D6 | Shared-code edits (anything outside the new provider/test dirs) must be arch- and behavior-neutral for existing platforms, or gated, and must say which in the change description. |
| D7 | Unsupported or host-less functions return `napi_generic_failure` with a recorded last-error message. Missing symbols, silent no-ops, and fake success are defects. |
| D8 | No string `eval` of provider-generated code for missing engine features. Approved intrinsic-call paths (calling `Object.freeze`, `Symbol.for`, `BigInt(...)` etc. via `JsCallFunction`) are allowed and specified per task; compiling a fixed internal helper snippet at env creation is allowed only where a task says so. |

### 2.2 Pre-requirements (decide before the listed tasks start)

Each PR needs: an owner, a decision recorded in this section (edit the
table with the outcome + date), and the acceptance evidence named.
A task whose blocking PRs are open **must not start**; a sub-agent
hitting an undecided PR stops and escalates (§9).

| ID | Question | Recommendation (default if owner silent) | Blocks | Acceptance evidence |
|---|---|---|---|---|
| PR-1 | What may we publicly claim? ("supports Node-API v8" wording) | Claim only: "implements the Node-API v8 engine surface (`js_native_api.h`) plus the §4 Table B functions under the §3.2 host contract", with the N5 receipt as proof. Never claim Node runtime compatibility. | N5.5, any README | Wording in README matches §0.3 evidence. |
| PR-2 | Header + test-asset provenance | Vendor headers and test assets from **one pinned Node release** (recommend v22.22.3, matching the audited local headers), with version, SHA-256 per file, and Node's MIT license notice preserved. `NAPI_VERSION=8` defined explicitly for provider and test addons. Never vendor `<v8.h>`, `<node.h>`, `<node_buffer.h>`, `uv.h`. | N0.1, N0.5, all impl tasks | Vendored tree + manifest with hashes; license file present. |
| PR-3 | Library + loading strategy | Separate shared library `libChakraNodeApi` linking `libChakraCore`; host loads it `RTLD_GLOBAL` before addons. Exports recorded in a new inventory file checked by N1.6/N5.5. (Alternative — symbols inside `libChakraCore` — only if the spike shows a concrete problem.) | N0.3, N1.1, N1.6 | Loader smoke: addon with undefined `napi_*` symbols resolves and registers. |
| PR-4 | Language spike outcome (D5) | Zig unless N0.2 criteria fail. | N1.1+ | Spike report (§6, N0.2) + decision recorded here. |
| PR-5 | Host contract shape (§3.2) | Private API as sketched in §3.2; host services all optional, absence ⇒ `napi_generic_failure` for dependent functions. | N0.3, N1.1, N4.* | Header sketch reviewed; test host compiles against it. |
| PR-6 | Buffer policy | Provider-created buffers are `Uint8Array` views marked with a provider-private hidden symbol (§3.9); `napi_is_buffer` checks the marker. We do **not** provide a JS `Buffer` global; addons needing `require('buffer')` are runtime scope (§0, C). Document as known non-conformance vs Node. | N4.2 | Policy paragraph + marker test in N4.2. |
| PR-7 | Async posture for v1 | Implement N4.3/N4.4 only against injected host services; with no services, async-work/TSFN/uv functions return `napi_generic_failure`. No provider-internal thread pool in v1. | N4.3, N4.4 | Host-contract test with and without services. |
| PR-8 | Promise pumping | Provider installs the continuation callback only if the host has not; the **host** owns pumping (test host pumps explicitly; embedders must do the same). Provider never spins its own loop. | N3.4, N0.3 | Pump contract in §3.2 header comments + test. |
| PR-9 | BigInt/Date gaps | v1: JS-mediated via intrinsic calls (D8): `BigInt(string)` for creation, string conversion for reads, `Object.prototype.toString` discrimination for `typeof`, global `Date` constructor/`getTime`/`instanceof` for dates. If N2.6/N3.1 evidence shows this is too slow or wrong, file a separate JsRT-extension engine lane; do not block the provider on it. BigInt64 typed arrays stay `napi_generic_failure` until a JsRT extension exists. | N2.6, N3.1 | Gap table §4.3 updated with chosen path per row. |
| PR-10 | Platform/profile order | macOS arm64 first (test + jit profiles), then Linux x64. Provider tests are a separate `zig build check-nodeapi` step and must not move `gate_counts.json` pins. | N1.6, N5.* | Two platform receipts (N5.5). |
| PR-11 | Test assets | Vendor: Node `test/js-native-api` (subset per N5.1), node-addon-examples `napi/` variants (subset per N5.2). Compile addons directly with `zig cc`/`zig c++` against vendored headers — no node-gyp/npm network dependency in CI. node-gyp consumer proof is a packaging-only check (N5.5, may be manual). | N0.5, N5.1, N5.2 | Asset manifest + runner works offline. |
| PR-12 | Error semantics policy | Status mapping table in §3.10 is normative; JS-visible semantics (exception type, pending state) are conformance, Node's exact message text is not. Type-mismatch statuses are the specific `napi_*_expected` codes, produced by provider pre-checks — never blanket `napi_invalid_arg`. | N1.5, all impl tasks | Mapping table reviewed; N1.5 tests cover each row. |

---

## 3. Architecture

### 3.1 Components

```text
addon.node  (exports napi_register_module_v1; imports napi_* undefined)
    ▲ dlopen (RTLD_LOCAL), version check, registration call
    │
host (test host: tools/nodeapi/ch-nodeapi; later: real embedders)
    │ loads provider RTLD_GLOBAL first (PR-3)
    ▼
libChakraNodeApi  (exports the 145 napi_* symbols + private host API §3.2)
    │ JsRT C calls only (ChakraCommon.h / ChakraCore.h)
    ▼
libChakraCore  (JsRuntimeHandle + JsContextRef per env)
```

Rules: the provider calls **only** the public JsRT C API (no engine
internals, no C++ engine headers). The host owns: runtime/context
creation, provider loading, addon loading, promise pumping (PR-8),
and any event-loop services it injects (PR-7).

### 3.2 Private host API (to be finalized in N0.3, shape fixed here)

Not part of Node-API; names must not collide with `napi_*`/`node_api_*`.

```c
// Host services: every field optional (NULL = service absent).
typedef struct chakra_nodeapi_host_services {
    // Async work / TSFN scheduling (PR-7). post_task queues fn(arg) on
    // the env thread; post_work runs execute(arg) off-thread then
    // post_task(complete, arg). Return false = cannot accept.
    bool (*post_task)(void* host_ctx, void (*fn)(void*), void* arg);
    bool (*post_work)(void* host_ctx,
                      void (*execute)(void*), void (*complete)(void*),
                      void* arg);
    void (*cancel_work)(void* host_ctx, void* arg);      // best effort
    void (*ref_loop)(void* host_ctx);                    // TSFN keep-alive
    void (*unref_loop)(void* host_ctx);
    void (*fatal_exception)(void* host_ctx, napi_value exc); // NULL = abort
    struct uv_loop_s* uv_loop;                           // NULL = no uv
} chakra_nodeapi_host_services;

// Create an env for (runtime, context) owned by the calling thread.
// `module_filename` may be NULL. The env records the negotiated API
// version (provider max = 8 in v1).
napi_env chakra_nodeapi_create_env(JsRuntimeHandle runtime,
                                   JsContextRef context,
                                   const char* module_filename,
                                   const chakra_nodeapi_host_services* services,
                                   void* host_ctx);

// Destroy per the §3.6 state machine. Must be called on the env
// thread, before JsDisposeRuntime, with no provider call in flight.
void chakra_nodeapi_destroy_env(napi_env env);
```

The provider also needs the current-context discipline internally:
every public entry validates the env, enters its context
(`JsSetCurrentContext`, `ChakraCommon.h:1072-1074`), and restores the
previous context on exit. Note `JsContextRef` remarks at
`ChakraCommon.h:394-399` are context documentation, not the setter.

### 3.3 `NodeEnv` model (normative sketch; N1.1 finalizes)

One `NodeEnv` per (context × addon registration) (§1.3):

```text
NodeEnv {
  runtime: JsRuntimeHandle, context: JsContextRef,
  owner_thread, state: Alive | Closing | Dead,
  module_api_version: u32,        // negotiated at registration (<= 8)
  module_filename: ?[]u8,         // provider-owned copy
  services: HostServices, host_ctx,

  scopes: stack of Scope,         // Scope = list of rooted JsValueRef
                                  //   + escapable flag + escaped flag
  refs: map RefId -> Ref,         // Ref = strong(JsAddRef count held
                                  //   by provider) | weak(JsWeakRef)
  wraps: map WeakRefHandle -> WrapRecord,   // §3.7
  finalizer_queue: mutex + list of WrapRecord*,  // drained on env thread
  callbacks: stack of CallbackRecord,       // §3.8
  instance_data: ?{ptr, finalize, hint},
  cleanup_hooks: list (LIFO at teardown),
  async_cleanup_hooks: list of {handle, fn, data, completed},
  last_error: {status, message},
  pending_finalizers_at_close: count (teardown diagnostics),
}
```

All provider-owned containers live in provider (native) memory —
never in Recycler memory — and every `JsValueRef` stored in them is
rooted per §3.4. `NodeEnv` itself is allocated by the provider's
allocator and freed only in `chakra_nodeapi_destroy_env` after the
§3.6 sequence completes.

### 3.4 Rooting rules (load-bearing; violations are memory bugs)

From [memory-management.md](memory-management.md) §2 and the JsRT
audit:

- **R1.** Any `JsValueRef` stored in provider-owned memory (scope
  lists, refs, wrap records, deferreds, callback records, TSFN state)
  is `JsAddRef`'d at store time and `JsRelease`'d at the matching
  release point. Do not rely on native stack/container scanning to
  keep JS values alive — Zig locals/containers are not a rooting
  mechanism in this boundary policy.
- **R2.** Provider keeps its own counts and crosses
  `JsAddRef`/`JsRelease` only on 0↔1 transitions. `JsRelease` does not
  validate balance; imbalance corrupts silently (asserts only in
  debug). Tagged numbers no-op in `JsAddRef` — harmless, but do not
  mistake the returned count for a root.
- **R3.** No `JsAddRef`/`JsRelease`, `napi_env`, `napi_value`, or
  `napi_ref` use from worker threads (rooting requires a thread
  context on the calling thread). Worker threads touch only native
  data and the TSFN queue.
- **R4.** A raw buffer pointer from `JsGet*Storage` is a borrow, not a
  root and not stable: root the ArrayBuffer for the borrow's life and
  revalidate (detach/resize) after any call that can run JS.
- **R5.** All roots are released before `JsDisposeRuntime`. Roots do
  not survive runtime disposal and never license use after teardown.
- **R6.** `JsWeakRef` handles stored by the provider are themselves
  `JsRef`s: root the handle (`JsAddRef` on the weak-ref handle) for
  as long as it is stored; release it when the record dies.

### 3.5 Handle scopes

- A scope is a list of rooted values (R1). Every API that returns a
  new `napi_value` roots it into the innermost open scope. API entry
  points that Node documents as callable with a pending exception
  (scope close, ref delete, exception queries — per the vendored
  header docs) must not early-return on pending state.
- `napi_escape_handle`: moves the value's root from the escapable
  scope to its parent; callable once per escapable scope; second call
  ⇒ `napi_escape_called_twice`.
- Scopes close in LIFO order; out-of-order close ⇒
  `napi_handle_scope_mismatch` and the provider still releases the
  requested scope's roots (no leak on misuse).
- The host glue opens a registration scope around
  `napi_register_module_v1`; the callback thunk (§3.8) opens an
  implicit scope per callback invocation. On callback return the
  thunk verifies scope/callback balance — imbalance is a provider
  bug, surfaced as `napi_handle_scope_mismatch` + cleanup, never a
  leak or crash.

### 3.6 Env lifecycle state machine (normative order)

`chakra_nodeapi_destroy_env` performs, in order, on the env thread:

1. State → `Closing`. New public API calls (except teardown-internal
   ones) return `napi_generic_failure`.
2. Drain the finalizer queue (§3.7) — wrap finalizers run with a
   still-valid env.
3. Run env cleanup hooks, LIFO.
4. Run async cleanup hooks; the host pumps until each hook calls
   `napi_remove_async_cleanup_hook` (host contract must bound this;
   the test host pumps with a timeout and reports a hang as a test
   failure, not a provider deadlock).
5. Invoke the instance-data finalizer (if any). Note the documented
   Node wart, which we reproduce: replacing instance data does **not**
   call the previous finalizer.
6. Delete all references (strong roots released; weak handles
   released), close all scopes (roots released), free wrap records
   (remaining wrap finalizers run here, exactly once).
7. State → `Dead`; free `NodeEnv`. Only now may the host dispose the
   Chakra context/runtime.

Where Node's exact teardown behavior is uncertain from docs, the
vendored tests (`test_finalizer`, `test_cleanup_hook`,
`test_async_cleanup_hook` assets in N5.1) are the oracle — N1.4 must
check this order against them and amend this section if they disagree.

### 3.7 Finalizers and wrap records

- Every provider-created external/wrapped value allocates a heap
  `WrapRecord { env, user_data, napi_finalize, hint, type_tag?,
  weak handle, exactly_once flag }` owned by the provider allocator.
- GC-side trampoline (the `JsFinalizeCallback` on provider-created
  external objects, and the before-collect trampoline for wrapped
  arbitrary objects) does **only**: mark exactly-once, enqueue the
  record on the env's mutex-protected finalizer queue, drop native
  references. No JS calls, no user finalizer, no provider-container
  mutation beyond the queue (GC-thread safety, D4).
- Drain points (env thread): on entry to any public API (cheap empty
  check), in the host pump, and in destroy step 2/6. Drained records
  invoke the user `napi_finalize(env, data, hint)` with a live env
  (during `Closing`, JS-calling APIs return `napi_generic_failure`).
- `napi_wrap` on arbitrary objects: per-env side table (§3.3 `wraps`)
  keyed by the object's `JsWeakRef` handle, with pointer-key ABA
  guarded by validating `JsGetWeakReferenceValue(handle) == object`
  before trusting a lookup, and records removed by the before-collect
  trampoline at collection time. Wrapping an already-wrapped object
  ⇒ `napi_invalid_arg`. If N3.3 evidence shows before-collect delivery
  is unreliable in this engine, stop and escalate: the fallback is a
  narrow JsRT weak-cell engine lane, not a heuristic.
- `JsCreateExternalObject`'s single data+callback slot always carries
  the provider trampoline + record pointer, never the user's directly
  (audit §2.3: the callback is cached per (callback, prototype) and
  fires with no current context).

### 3.8 Callback thunk

All N-API JS functions are created with `JsCreateEnhancedFunction`
(D3). The provider allocates a thunk record `{ napi_callback cb,
void* data, napi_env env }` as the JsRT callback state (rooted for
the function's lifetime via the function's external record — the
record dies with the function's finalizer trampoline, §3.7).

On invocation the thunk:

1. Validates env/state; enters the env context.
2. Pushes a `CallbackRecord { thisArg, argv (arguments+1),
   argc (argumentCount-1), newTarget, callee }`. Chakra's enhanced
   callback gives `arguments[0] == info->thisArg` and
   `argumentCount` includes `this`
   (`bin/NativeTests/JsRTApiTest.cpp:734-866` is the oracle).
   `newTarget` converts: non-construct ⇒ C `NULL` for
   `napi_get_new_target` (Chakra passes `undefined`; Node semantics
   want no-value); construct ⇒ `info->newTargetArg` (the explicit new
   target, e.g. from `Reflect.construct` — **not** assumed equal to
   the callee).
3. Opens an implicit handle scope; calls the user callback.
4. On return: roots the returned value into the caller's scope,
   closes the implicit scope, pops the record, restores the context.
   A pending JS exception after the callback propagates as the
   function's exception (status handling per §3.10).

Function names and `length`: the `length` parameter of
`napi_create_function` / `napi_define_class` is the UTF-8 **name**
length in bytes (or `NAPI_AUTO_LENGTH`), not a function arity.
Upstream Node proves this: vendored
`test_function/test_function.c` creates
`napi_create_function(env, "Name_extra", 5, ...)` and
`test_function/test.js` asserts `fn.name === 'Name_'`
(truncation), with no `Function.length` arity assertion
(`src/js_native_api_v8.cc` uses `length` only to build the name
string; V8 `Function::New` defaults `Function.length` to 0).
The provider therefore truncates the name per `length` and
defines `Function.length` to the V8 default 0 via a real
descriptor (N2.4); `Function.length` is never set to the
`napi` `length` value.

### 3.9 Buffers (PR-6 design)

- `napi_create_buffer*` produce a `Uint8Array` view over a (possibly
  external) ArrayBuffer, marked as a Buffer by setting a
  provider-private hidden symbol property (`Symbol` created per env,
  never exposed) to `true`. `napi_is_buffer` = marker check;
  `napi_get_buffer_info` composes `JsGetTypedArrayInfo` +
  `JsGetTypedArrayStorage` (data = storage base + byteOffset;
  element length = byteLength / element size — audit §2.4).
- No JS `Buffer` global, no pooling, no `Buffer.prototype` methods.
  This is a recorded non-conformance (PR-6), acceptable because a
  `Buffer` global is runtime scope (§0 C).

### 3.10 Status and exception model (PR-12, normative)

Per-entry discipline: validate args (`napi_invalid_arg`) → check
pending exception (most APIs return `napi_pending_exception` without
executing; exceptions per the vendored header docs) → pre-check types
for the specific `napi_*_expected` status → call JsRT → map
`JsErrorCode`:

| JsRT outcome | napi_status | Notes |
|---|---|---|
| `JsNoError` | `napi_ok` | |
| `JsErrorInvalidArgument`, `JsErrorNullArgument` | `napi_invalid_arg` | only after provider pre-checks have had the chance to produce a more specific code |
| Type mismatch found by provider pre-check | `napi_object_expected` / `napi_string_expected` / `napi_number_expected` / `napi_boolean_expected` / `napi_array_expected` / `napi_function_expected` / `napi_bigint_expected` / `napi_date_expected` / `napi_arraybuffer_expected` as applicable | produced by the provider, not by mapping JsRT's collapsed invalid-arg |
| `JsErrorInExceptionState`, `JsErrorScriptException` | `napi_pending_exception` | exception stays pending; never cleared implicitly |
| `JsErrorScriptCompile` (from `JsRun`) | `napi_pending_exception` | the compile error is the pending exception |
| `JsErrorOutOfMemory` | `napi_generic_failure` + last-error message | do not fabricate a JS exception |
| anything else | `napi_generic_failure` | message recorded for `napi_get_last_error_info` |

`napi_get_last_error_info` returns the per-env record of the most
recent failed call on that env. Throw helpers (`napi_throw_*`) set the
pending exception via `JsSetException` and return `napi_ok` when no
exception was already pending (if one is pending, return
`napi_pending_exception` and leave it).

---

## 4. API surface partition (implementation contract)

Normative inventory is generated from the vendored headers in N0.1;
if this section and the generated inventory disagree, the generated
inventory wins and this section is amended in the same change.

### 4.1 Table A — engine surface (`js_native_api.h`, v8: 115)

| Group | Task | Functions (v8) |
|---|---|---|
| A1 Values | N2.1, N2.2, N2.6 | `napi_get_undefined`, `napi_get_null`, `napi_get_global`, `napi_get_boolean`, `napi_create_double`, `napi_create_int32`, `napi_create_uint32`, `napi_create_int64`, `napi_get_value_bool`, `napi_get_value_double`, `napi_get_value_int32`, `napi_get_value_int64`, `napi_get_value_uint32`, `napi_create_string_utf8`, `napi_create_string_utf16`, `napi_create_string_latin1`, `napi_get_value_string_utf8`, `napi_get_value_string_utf16`, `napi_get_value_string_latin1`, `napi_create_symbol`, `napi_create_date`, `napi_get_date_value`, `napi_create_bigint_int64`, `napi_create_bigint_uint64`, `napi_create_bigint_words`, `napi_get_value_bigint_int64`, `napi_get_value_bigint_uint64`, `napi_get_value_bigint_words` |
| A2 Objects/functions | N2.3, N2.4 | `napi_create_object`, `napi_create_array`, `napi_create_array_with_length`, `napi_get_array_length`, `napi_get_property_names`, `napi_get_all_property_names`, `napi_get_property`, `napi_set_property`, `napi_has_property`, `napi_delete_property`, `napi_has_own_property`, `napi_get_named_property`, `napi_set_named_property`, `napi_has_named_property`, `napi_get_element`, `napi_set_element`, `napi_has_element`, `napi_delete_element`, `napi_define_properties`, `napi_get_prototype`, `napi_object_freeze`, `napi_object_seal`, `napi_create_function`, `napi_get_cb_info`, `napi_get_new_target`, `napi_new_instance`, `napi_call_function`, `napi_define_class`, `napi_instanceof`, `napi_typeof`, `napi_is_array`, `napi_is_date`, `napi_is_error`, `napi_strict_equals`, `napi_coerce_to_bool`, `napi_coerce_to_number`, `napi_coerce_to_object`, `napi_coerce_to_string` |
| A3 Lifetime | N1.2–N1.4, N3.2, N3.3 | `napi_open_handle_scope`, `napi_close_handle_scope`, `napi_open_escapable_handle_scope`, `napi_close_escapable_handle_scope`, `napi_escape_handle`, `napi_create_reference`, `napi_delete_reference`, `napi_reference_ref`, `napi_reference_unref`, `napi_get_reference_value`, `napi_create_external`, `napi_get_value_external`, `napi_wrap`, `napi_unwrap`, `napi_remove_wrap`, `napi_add_finalizer`, `napi_type_tag_object`, `napi_check_object_type_tag`, `napi_set_instance_data`, `napi_get_instance_data` |
| A4 Errors | N2.5 | `napi_get_last_error_info`, `napi_throw`, `napi_throw_error`, `napi_throw_type_error`, `napi_throw_range_error`, `napi_create_error`, `napi_create_type_error`, `napi_create_range_error`, `napi_is_error` (also A2), `napi_get_and_clear_last_exception`, `napi_is_exception_pending` |
| A5 Binary | N3.1 | `napi_create_arraybuffer`, `napi_create_external_arraybuffer`, `napi_get_arraybuffer_info`, `napi_detach_arraybuffer`, `napi_is_detached_arraybuffer`, `napi_create_typedarray`, `napi_get_typedarray_info`, `napi_create_dataview`, `napi_get_dataview_info`, `napi_is_arraybuffer`, `napi_is_typedarray`, `napi_is_dataview` |
| A6 Script/promise/version | N3.4, N3.5 | `napi_create_promise`, `napi_is_promise`, `napi_resolve_deferred`, `napi_reject_deferred`, `napi_run_script`, `napi_get_version`, `napi_adjust_external_memory` |

Notes: `napi_create_named_function` and `napi_symbol_for` do **not**
exist in the v8 C surface (the former is a node-addon-api C++
convenience; the latter is `node_api_symbol_for`, v9) — do not
implement them. SyntaxError creation/throw is likewise v9
(`node_api_*`), absent at v8.

### 4.2 Table B — host surface (`node_api.h`, v8: 30)

| Group | Task | Functions | Disposition |
|---|---|---|---|
| B1 Registration/fatal | N4.1 | `napi_module_register`, `napi_fatal_error`, `napi_fatal_exception` | Registry implemented (pending-module list); `napi_fatal_error` → host fatal path/abort, never returns; `napi_fatal_exception` → host `fatal_exception` service or abort policy. |
| B2 Buffer | N4.2 | `napi_create_buffer`, `napi_create_external_buffer`, `napi_create_buffer_copy`, `napi_is_buffer`, `napi_get_buffer_info` | Per §3.9/PR-6. |
| B3 Async context | N4.3 | `napi_async_init`, `napi_async_destroy`, `napi_make_callback`, `napi_open_callback_scope`, `napi_close_callback_scope` | Bookkeeping + callback invocation; async_hooks propagation is a documented no-op (no AsyncLocalStorage semantics — runtime scope). |
| B4 Async work + uv | N4.4 | `napi_create_async_work`, `napi_queue_async_work`, `napi_cancel_async_work`, `napi_delete_async_work`, `napi_get_uv_event_loop` | Only with host services (PR-7); `napi_get_uv_event_loop` returns the injected loop or `napi_generic_failure`. |
| B5 Thread-safe functions | N4.4 | `napi_create_threadsafe_function`, `napi_get_threadsafe_function_context`, `napi_call_threadsafe_function`, `napi_acquire_threadsafe_function`, `napi_release_threadsafe_function`, `napi_ref_threadsafe_function`, `napi_unref_threadsafe_function` | Only with host services (PR-7); state machine §3/N4.4. |
| B6 Cleanup hooks | N1.4 (sync), N4.3 (async completion) | `napi_add_env_cleanup_hook`, `napi_remove_env_cleanup_hook`, `napi_add_async_cleanup_hook`, `napi_remove_async_cleanup_hook` | Sync hooks in the lifetime kernel; async completion needs the host pump. |
| B7 Node version | N3.5 | `napi_get_node_version` | Returns the provider's version struct (fields = provider version, release string identifies Chakra provider). It does not claim a Node runtime version; addons branching on it get provider truth, documented here. |

### 4.3 Gaps requiring design or an engine lane (from the JsRT audit)

| Gap | v1 path (task) | If evidence rejects it |
|---|---|---|
| BigInt create/read; `typeof` discrimination | String-mediated intrinsic calls (N2.6): `BigInt(str)` create; `Object.prototype.toString` tag for discrimination; decimal/hex string conversion for words | Narrow JsRT export lane (separate engine task; uses internal `JavascriptBigInt` creators) |
| Date create/read/is-date | Global `Date` constructor + `getTime` + `instanceof` intrinsic calls (N2.6) | JsRT date export lane |
| Detached query | Investigate `JsGetArrayBufferStorage` behavior on detached buffers in N3.1; JS `ArrayBuffer.prototype.detached` getter if present | JsRT `JsIsDetachedArrayBuffer` export lane |
| Freeze/seal | `Object.freeze`/`Object.seal` intrinsic calls (N2.3) | — (intrinsic calls are semantically exact) |
| Function `length` | `napi_create_function` `length` is the UTF-8 **name** length (not arity); `Function.length` stays V8 default 0 via post-create define to 0 (N2.4), per vendored `test_function` (`"Name_extra",5` ⇒ `name === 'Name_'`) | — |
| Wrap/finalizer on arbitrary objects, env-thread exactly-once | Side table + before-collect trampoline + drain queue (N3.3) | JsRT weak-cell export lane — **stop-and-escalate**, do not heuristic around it |
| BigInt64/BigUint64 typed arrays | `napi_generic_failure` + recorded non-conformance (N3.1) | JsRT `JsTypedArrayType` extension lane |
| Weak `napi_ref` (count 0) | `JsCreateWeakReference` (N1.3) | — (API exists; semantics verified by test_reference) |

---

## 5. Waves and gates

Waves are dependency-ordered. A wave's exit gate must be green before
the next wave's tasks are handed out, except where a task card says
it may run in parallel. Within a wave, tasks are sub-agent-sized and
parallelizable subject to the file-ownership matrix (§5.2).

| Wave | Name | Tasks | Exit gate |
|---|---|---|---|
| 0 | Decisions, assets, harness | N0.1–N0.6 | **G0**: PR-1…PR-12 recorded; generated inventory exists; language spike decided (PR-4); test host loads a stub-provider hello addon end to end. |
| 1 | Env + lifetime kernel | N1.1–N1.6 | **G1**: hello-world + pass-arguments addons (real provider) pass on macOS arm64; scope/ref/teardown probes green; finalizer drain proven under `-CollectGarbage`. |
| 2 | Values, objects, functions, errors | N2.1–N2.6 | **G2**: object/function/callback/error probes green; nested-callback and construct-call probes green. |
| 3 | Binary, wrap, promises, script | N3.1–N3.5 | **G3**: typed-array/class-wrap/promise/script probes green; no double-finalize or use-after-scope-close reachable in stress probes. |
| 4 | Host surface | N4.1–N4.4 | **G4**: registration edge cases green; Buffer policy tests green; async/TSFN green **with** test-host services and correctly failing without them. |
| 5 | Conformance + packaging | N5.1–N5.5 | **G5**: vendored-test and example receipts published for macOS arm64 **and** Linux x64; exports inventory gated in CI; README claims match evidence (PR-1). |

Critical path: N0.1 → N0.2 → N0.3/N0.4 → N1.1 → N1.2/N1.5 → N1.3/N1.4
→ N2.* → N3.* → N5.1/N5.2. N4.* may slip past G3 without blocking
Wave 5's engine-surface conformance, but G5's README must then scope
the claim accordingly.

### 5.1 Parallelization rules

- Tasks in the same wave may run in parallel if they touch disjoint
  files (§5.2) and depend only on frozen interfaces from earlier
  waves.
- `NodeEnv` layout (§3.3) is owned by N1.1. Other tasks treat it as a
  reviewed interface: propose changes in your report; do not edit it
  concurrently.
- Any task discovering it needs a new JsRT export stops and files the
  gap (§4.3 pattern) instead of working around it with `eval` (D8).

### 5.2 File ownership (initial; N1.6 may refine)

```text
lib/NodeApi/            provider (language per PR-4)
  NodeApiEnv.*          N1.1 (env core, registry, destroy)
  NodeApiScopes.*       N1.2 (handle/escapable scopes)
  NodeApiRefs.*         N1.3 (references)
  NodeApiLifetime.*     N1.4 (instance data, cleanup hooks)
  NodeApiErrors.*       N1.5 (status mapping, last error, entry guard)
  NodeApiValues.*       N2.1, N2.2, N2.6
  NodeApiObjects.*      N2.3
  NodeApiFunctions.*    N2.4 (thunk, cb_info, call/new, define_class glue)
  NodeApiErrorsImpl.*   N2.5 (create/throw/is_error)
  NodeApiBinary.*       N3.1
  NodeApiWrap.*         N3.2, N3.3 (externals, finalizer queue, wrap, tags)
  NodeApiPromise.*      N3.4
  NodeApiScript.*       N3.5 (run_script, versions, external memory)
  NodeApiHost.*         N4.1–N4.4 (registration, Buffer, async, TSFN)
tools/nodeapi/          N0.3 (test host + addon build harness)
test/nodeapi/           per-task probes; N5.1/N5.2 vendored assets
include/nodeapi/        vendored headers (N0.1) — read-only after landing
tools/build/            N1.6 (build step, exports inventory)
```

---

## 6. Task cards

Card fields: **Owns** (files), **Needs** (tasks/PRs that must be done
first), **Do** (numbered steps), **Done when** (checklist), **Prove**
(validation: exact commands/probes, including negative cases),
**Ownership** (memory-management record required or not).

Conventions for every implementation task (not repeated per card):
every public entry follows §3.10 (validate → pending-check →
pre-check → JsRT → map status) and §3.2 context discipline; every
returned `napi_value` is rooted per R1; unsupported ⇒ D7.

### Wave 0 — decisions, assets, harness

#### N0.1 — Vendor headers + generate the normative inventory
- **Owns:** `include/nodeapi/` (vendored headers + `MANIFEST.md` with
  per-file SHA-256, Node version, license), `tools/nodeapi/gen_inventory.py`,
  generated `doc/node-api-inventory.md`.
- **Needs:** PR-2 decided.
- **Do:** (1) Copy the four headers from the pinned Node release
  (PR-2) preserving license headers. (2) Write the generator: parse
  declarations from the vendored headers, filter to `NAPI_VERSION=8`
  visibility (exclude `NAPI_EXPERIMENTAL` and v9/v10 blocks), emit the
  inventory (name, header, group per §4). (3) Diff the generated
  inventory against §4 Tables A/B; amend §4 in the same change if
  they differ.
- **Done when:** inventory lists exactly the v8-visible symbols
  (expect 145; investigate any other number, do not adjust the
  expectation to fit); manifest hashes verify; a second reader
  spot-checks 10 rows against the headers.
- **Prove:** `python3 tools/nodeapi/gen_inventory.py --check` exits 0
  on a clean tree and fails if a header is edited.
- **Ownership:** not required (no runtime code).

#### N0.2 — Language + build spike (decides PR-4)
- **Owns:** spike code under `tools/nodeapi/spike/` (throwaway),
  report appended to this plan's PR-4 row.
- **Needs:** N0.1 (headers to import).
- **Do:** In Zig 0.17 (D5 default): (1) `@cImport` the vendored
  `js_native_api.h` + Chakra `ChakraCommon.h`/`ChakraCore.h` in one
  translation unit; (2) export two C-ABI functions
  (`napi_get_version`, `napi_create_object` minimal) from a Zig shared
  library linked against the built `libChakraCore`; (3) register a
  callback via the §3.8 enhanced-function path and invoke it from JS
  through the N0.3 host skeleton (or a minimal JsRT driver if N0.3 is
  not ready); (4) exercise one finalizer trampoline enqueue (no drain
  needed). Record: compile friction, header constructs that fail to
  translate, link/run evidence.
- **Done when:** all four steps demonstrably work, or the specific
  failing step is documented with compiler output. PR-4 is recorded
  either way (Zig confirmed, or C++ fallback with reason).
- **Prove:** spike binary runs; output shows version=8 path and a
  callback round-trip. Attach logs to the report.
- **Ownership:** not required (throwaway), but note any rooting the
  spike used.

#### N0.3 — Private host API + test host skeleton
- **Owns:** `tools/nodeapi/ch-nodeapi.*` (host executable),
  `tools/nodeapi/build_addon.*` (compiles a test addon with
  `zig cc`/`zig c++` against vendored headers, `NAPI_VERSION=8`),
  `lib/NodeApi/NodeApiHostApi.h` (the §3.2 private API header).
- **Needs:** PR-3, PR-5 decided; N0.1. May use a stub provider (all
  symbols returning `napi_generic_failure`) to prove loading first.
- **Do:** (1) Finalize the §3.2 header (names, service struct,
  create/destroy). (2) Host: create runtime+context, load provider
  `RTLD_GLOBAL`, create env via private API, dlopen addon, resolve
  version symbol (absent ⇒ 8), reject > 8, create exports, open
  registration scope, call `napi_register_module_v1`, apply the
  return-replaces-exports rule, run a JS file with the exports bound
  as a global/module value, pump promises (PR-8), destroy env before
  disposing runtime. (3) `hello-world` test addon + JS driver.
- **Done when:** hello-world runs end to end against the stub
  provider (registration mechanics proven independent of provider
  completeness); version-rejection and missing-symbol paths produce
  clean host errors, not crashes.
- **Prove:** `tools/nodeapi/run_probe.sh hello-world` (or the zig
  build step that wraps it) prints the expected line; negative probes
  (addon requesting v9, addon with no registration export) fail
  cleanly.
- **Ownership:** required for the host's env/runtime lifecycle
  (creation order, destroy order, thread).

#### N0.4 — Semantics spec: env lifecycle, scopes, refs, finalizers
- **Owns:** `doc/node-api-semantics.md`.
- **Needs:** N0.1 inventory; §3.3–§3.10 of this plan.
- **Do:** Expand §3.3–§3.10 into implementer-grade rules with worked
  examples: env state machine (§3.6) step-by-step; scope open/close/
  escape sequences including misuse; strong vs weak ref lifecycle
  (0↔1 transitions, R2/R6); wrap-record lifecycle from `napi_wrap` to
  finalizer drain, including teardown-with-live-objects; callback
  record push/pop under nesting and exceptions; the §3.10 mapping
  applied to five representative functions end to end. For every rule
  that docs leave ambiguous, name the vendored test (N0.5 assets) that
  will decide it.
- **Done when:** a reviewer can answer, from the doc alone: what
  happens on (a) escape-twice, (b) close-out-of-order, (c) wrap →
  collect → drain, (d) destroy with a live wrapped object, (e) nested
  callback throwing.
- **Prove:** review sign-off recorded in the doc; each answer cites a
  probe that N1/N3 tasks will implement.
- **Ownership:** the doc *is* the ownership record for the kernel;
  include the memory-management.md §3 record for scopes, refs, and
  wrap records.

#### N0.5 — Vendor test assets + runner design
- **Owns:** `test/nodeapi/vendor/` (assets + `MANIFEST.md`),
  `test/nodeapi/run_matrix.py` (design + skeleton).
- **Needs:** PR-11 decided; N0.1.
- **Do:** (1) Vendor the Node `test/js-native-api` subset listed in
  N5.1's scope (native sources + JS runners) and the node-addon-examples
  `napi/` subset for N5.2, from the same pinned Node release as PR-2
  where applicable (examples live in their own repo — pin a commit).
  (2) Write the `common` shim the Node JS runners need (`buildType`,
  `mustCall`, tick/GC helpers mapped to host facilities; a host `gc()`
  injection via `-CollectGarbage`-equivalent provider test hook in
  the test host only). (3) Runner design: build each addon with
  `zig cc`, load via ch-nodeapi, compare stdout/exit against expected;
  record pass/skip/fail with reasons in JSON.
- **Done when:** runner executes one vendored test end to end
  (pass or correctly-reported skip); skip-reason taxonomy exists
  (needs-gc, needs-runtime-global, needs-node-api-version>8,
  provider-gap).
- **Prove:** the one-test run + manifest hash check.
- **Ownership:** not required.

#### N0.6 — Status/exception mapping verification
- **Owns:** additions to `doc/node-api-semantics.md` (per-function
  exception-pending allowlist), test list for N1.5.
- **Needs:** N0.1, N0.4 draft.
- **Do:** From the vendored headers' per-function docs, extract: (1)
  which functions may be called with a pending exception; (2) which
  return `napi_pending_exception` vs execute; (3) each getter's
  specific `*_expected` status. Tabulate; flag conflicts with §3.10.
- **Done when:** the allowlist table exists and §3.10 is amended to
  match it; N1.5's test list enumerates one probe per row.
- **Prove:** second reader checks 10 rows against header docs.
- **Ownership:** not required.

### Wave 1 — env + lifetime kernel

#### N1.1 — NodeEnv core + private create/destroy
- **Owns:** `lib/NodeApi/NodeApiEnv.*` (§3.3 layout, registry,
  create/destroy per §3.6 steps 1 and 7 only — hooks/refs land in
  their own tasks but destroy must call their teardown entry points),
  provider version reporting (`napi_get_version` → 8).
- **Needs:** G0 (PR-3/PR-4/PR-5), N0.3 header, N0.4.
- **Do:** Implement env allocation/registry/state machine; context
  enter/exit guard used by all entries; `napi_get_version`;
  destroy sequence skeleton with pluggable teardown steps.
- **Done when:** host creates/destroys 1000 envs sequentially without
  leak growth (provider allocator accounting reported); API calls on
  a `Closing`/`Dead` env fail cleanly; calls with a wrong-thread env
  are rejected or documented as UB per Node semantics (choose:
  debug assert + release `napi_invalid_arg` — record choice).
- **Prove:** host probe (create/destroy loop) + wrong-state probes.
- **Ownership:** required — env allocation domain, owner (provider),
  invalidation (destroy), thread (owner thread).

#### N1.2 — Handle scopes + escapable scopes
- **Owns:** `lib/NodeApi/NodeApiScopes.*`; A3 scope functions.
- **Needs:** N1.1.
- **Do:** §3.5 exactly: rooted scope lists (R1), LIFO close with
  mismatch handling, escape-once, implicit scope support for the
  thunk/registration, balance verification hook for N2.4.
- **Done when:** probes pass: value created in inner scope dies after
 (verified via weak ref + GC), escaped value survives, escape-twice ⇒
  `napi_escape_called_twice`, out-of-order close ⇒
  `napi_handle_scope_mismatch` with no root leak (root count returns
  to baseline — expose a test-only root counter via the private API
  in debug builds).
- **Prove:** `test/nodeapi/probes/scopes.*` via runner; root-counter
  assertions.
- **Ownership:** required — scope root lists (domain, release on
  close, invalidation events).

#### N1.3 — References (strong and weak)
- **Owns:** `lib/NodeApi/NodeApiRefs.*`; A3 reference functions.
- **Needs:** N1.1 (N1.2 helpful, not blocking).
- **Do:** Provider ref objects with own counts (R2): strong =
  `JsAddRef` held while provider count ≥ 1... with Node semantics:
  `napi_reference_ref` increments, `napi_reference_unref` decrements;
  at 0 the ref becomes weak — implement weak state with
  `JsCreateWeakReference` (R6), strong state with `JsAddRef`;
  transitions create/release the underlying handle exactly once.
  Enforce the v8 type restriction on `napi_create_reference` (per
  the Node-API documentation's v8 semantics; v10's all-types rule
  must not leak in).
  `napi_get_reference_value` on a collected weak ref returns
  `napi_ok` with a null result (verify against vendored
  `test_reference`).
- **Done when:** ref/unref/delete probes pass incl. weak collection
  (value gone after GC + deref), delete-then-use ⇒
  `napi_invalid_arg`, double-delete ⇒ `napi_invalid_arg`, refcount
  transitions don't leak roots.
- **Prove:** probes + vendored `test_reference` (when N0.5 assets
  land; otherwise equivalent local probe first).
- **Ownership:** required — ref handles, weak handles (R6), release
  points.

#### N1.4 — Instance data + cleanup hooks
- **Owns:** `lib/NodeApi/NodeApiLifetime.*`; A3 instance-data
  functions; B6 sync hooks; async-hook registration half of B6.
- **Needs:** N1.1; teardown order per §3.6 (verify against vendored
  tests per §3.6 note).
- **Do:** `napi_set/get_instance_data` (replacement does not call the
  old finalizer — documented wart, test it); sync cleanup hooks
  (add/remove, LIFO at destroy step 3); async cleanup hooks
  (register/remove; completion driven at destroy step 4 via host
  pump); remove-of-unknown-hook ⇒ `napi_invalid_arg`.
- **Done when:** teardown probes assert exact ordering:
  finalizer-queue drain → cleanup hooks LIFO → async hooks complete
  → instance-data finalizer → refs deleted; each hook fires exactly
  once; destroying twice is a host error surfaced cleanly.
- **Prove:** ordering probe with an event log compared verbatim.
- **Ownership:** required — hook lists, instance data ptr ownership
  (user-owned; provider only calls the finalizer).

#### N1.5 — Entry guard, last error, exception plumbing
- **Owns:** `lib/NodeApi/NodeApiErrors.*`; A4 plumbing functions
  (`napi_get_last_error_info`, `napi_is_exception_pending`,
  `napi_get_and_clear_last_exception`); the shared entry guard used
  by every public function.
- **Needs:** N1.1, N0.6 table.
- **Do:** Implement the §3.10 mapping helpers (JsErrorCode →
  status, specific-expected pre-check helpers), per-env last-error
  record, pending-exception check discipline per the N0.6 allowlist,
  context enter/exit + finalizer-drain-on-entry (§3.7) in one guard
  every task reuses.
- **Done when:** unit probes cover each §3.10 row; a failed call
  leaves `napi_get_last_error_info` populated and the next successful
  call does not clear it spuriously (per Node semantics — verify);
  calls during pending exception behave per allowlist.
- **Prove:** mapping probe table (one case per row) via runner.
- **Ownership:** not required beyond noting last-error storage is
  per-env native memory.

#### N1.6 — Build integration + exports gate
- **Owns:** `tools/build/` (provider source group in `tables.zig` +
  `tables_linux.zig`, `check-nodeapi` build step skeleton),
  `tools/build/nodeapi_exports.txt` (expected symbol inventory),
  exports verification script.
- **Needs:** PR-3, PR-10, N1.1 (first real symbols).
- **Do:** Build `libChakraNodeApi` (shared) for the test and jit
  profiles without moving existing gate pins; install vendored
  headers to the include dir; verify built library exports ⊇
  inventory (missing symbol = build failure); keep the inventory
  generated from N0.1's output, not hand-maintained.
- **Done when:** `mise exec -- zig build` and
  `mise exec -- zig build -Dprofile=jit` succeed with the provider;
  exports check passes; existing `zig build check` pins unchanged.
- **Prove:** the three commands + `nm -g` diff against inventory.
- **Ownership:** not required.

**Gate G1** additionally requires: hello-world and pass-arguments
addons (from N0.5 assets or local equivalents) pass through the real
provider on macOS arm64, and N1.2–N1.4 probes are green.

### Wave 2 — values, objects, functions, errors

#### N2.1 — Primitives and number conversions
- **Owns:** `lib/NodeApi/NodeApiValues.*` (scalar half); A1 scalar
  functions.
- **Needs:** G1.
- **Do:** undefined/null/global/boolean getters; create/get for
  double/int32/int64/uint32. Conversion semantics **from the vendored
  header docs**, not from JsRT's `ToInt32` behavior: implement
  int64/uint32 via double with the documented truncation/lossy
  rules; type pre-checks produce `napi_number_expected` etc. (§3.10).
- **Done when:** round-trip probes incl. ±2^53 edges, NaN, ±Infinity,
  negative-to-uint32, and wrong-type ⇒ specific expected status.
- **Prove:** probes + differential vs Node for the same inputs (N5.1
  harness may be reused early).
- **Ownership:** not required (values rooted by scope machinery).

#### N2.2 — Strings
- **Owns:** `NodeApiValues.*` (string half); A1 string functions.
- **Needs:** G1.
- **Do:** UTF-8 create/get via `JsCreateString`/`JsCopyString`
  (`ChakraCore.h:586-669`); UTF-16 via the Utf16 pair; **Latin1 by
  transcoding through UTF-16 in both directions** (JsRT has no Latin1
  pair; `JsCopyStringOneByte` truncates — do not use it as Latin1
  get). Honor `NAPI_AUTO_LENGTH` (provider-side strlen), explicit
  lengths with embedded NULs, and the two-call size-query pattern
  (buffer NULL ⇒ required length; truncation semantics per header
  docs).
- **Done when:** probes cover ASCII, non-ASCII UTF-8, surrogate
  pairs, embedded NUL, Latin1 high bytes round-trip, size-query and
  too-small-buffer cases with exact statuses.
- **Prove:** probes + byte-exact comparison vs Node for a fixed
  corpus.
- **Ownership:** required for any provider-side transcode buffers
  (scratch domain, no retention).

#### N2.3 — Objects, arrays, properties, descriptors
- **Owns:** `lib/NodeApi/NodeApiObjects.*`; A2 object half.
- **Needs:** G1; N2.4's thunk for descriptor getters/setters/methods
  (coordinate: descriptor *values* land here, function creation there
  — land N2.4's `napi_create_function` first or stub-jointly).
- **Do:** create object/array/array-with-length; named + element
  get/set/has/delete (element index wrapped provider-side);
  `napi_define_properties` by building a real descriptor object and
  calling `JsDefineProperty` (audit #7: `JsObjectDefinePropertyFull`
  cannot force false flags — do not use it); `napi_get_property_names`
  and `napi_get_all_property_names` composed from
  `JsGetOwnPropertyNames` + `JsGetOwnPropertySymbols` with
  mode/filter/conversion implemented provider-side via own-descriptor
  checks; prototype get/set; freeze/seal via `Object.freeze/seal`
  intrinsic calls (D8).
- **Done when:** descriptor round-trip probe (every attribute bit,
  accessor pair), property-names probes for each
  collection-mode/filter/conversion combination in the vendored
  tests, freeze/seal observable via JS (`Object.isFrozen` etc.),
  element ops at 2^32-1 boundary.
- **Prove:** probes + vendored property tests (N5.1 assets).
- **Ownership:** required for descriptor scratch + any cached
  intrinsic function refs (rooted per env, released at destroy).

#### N2.4 — Functions and callbacks
- **Owns:** `lib/NodeApi/NodeApiFunctions.*`; A2 function half
  (`napi_create_function`, `napi_get_cb_info`, `napi_get_new_target`,
  `napi_call_function`, `napi_new_instance`, `napi_define_class`).
- **Needs:** G1 (thunk depends on N1.2 scopes + N1.5 guard).
- **Do:** §3.8 thunk on `JsCreateEnhancedFunction`; callback-record
  stack with nesting/exception restore; `napi_get_cb_info` honoring
  in/out argc (return required argc when argv too small, per header
  docs); `napi_call_function` (this = argv convention translated to
  JsRT's arguments[0]=this); `napi_new_instance` via
  `JsConstructObject`; names: `napi_create_function` `length` is the
  UTF-8 name length, `Function.length` stays V8 default 0 (post-create
  define to 0, per vendored `test_function` `"Name_extra",5` ⇒
  `name === 'Name_'`);
  `napi_define_class` = enhanced ctor + property definition split by
  `napi_static` between constructor and prototype.
- **Done when:** probes: pass-arguments (argc/argv/this/data),
  construct call with `new.target` from `Reflect.construct`
  (≠ callee case), nested native→JS→native callback with record
  restore, callback throwing ⇒ pending exception at the JS caller,
  `Function.name` truncated per name-lengths 0..5 and
  `Function.length === 0` (V8 default, not arity).
- **Prove:** probes + `bin/NativeTests/JsRTApiTest.cpp` semantics
  cross-checked (argument layout oracle).
- **Ownership:** required — thunk records (owned by the function's
  external record, §3.7), callback record stack (call-scoped).

#### N2.5 — Errors and throw helpers
- **Owns:** `lib/NodeApi/NodeApiErrorsImpl.*`; A4 create/throw half.
- **Needs:** G1 (plumbing from N1.5).
- **Do:** `napi_create_error/type_error/range_error` (+ `code`
  property set provider-side), `napi_throw*` via `JsSetException`
  with pending-state discipline (§3.10), `napi_is_error` (Error
  discrimination — prototype-chain/intrinsic check; realm caveat
  recorded), `napi_fatal_exception` routed to host service/abort
  policy (B1 interface; full host wiring in N4.1).
- **Done when:** each helper's exception has the right JS class and
  message, is catchable in JS, and leaves exactly one pending
  exception; throw-while-pending returns `napi_pending_exception`
  and preserves the original exception.
- **Prove:** throw/catch probes per helper.
- **Ownership:** not required.

#### N2.6 — Coercion, type checks, symbols, Date, BigInt
- **Owns:** `NodeApiValues.*` (coercion half); A1 symbol/date/bigint
  + A2 coercion/type functions.
- **Needs:** G1; N2.1–N2.3 helpful.
- **Do:** `napi_typeof` (with BigInt discrimination via
  `Object.prototype.toString` tag, PR-9 — JsGetValueType reports
  BigInt as object); coerce_* via JsRT convert calls; `napi_instanceof`
  via `JsInstanceOf`; is_array/is_error/is_date/is_typedarray/
  is_dataview/is_arraybuffer via `JsGetValueType` + gaps (Date via
  `instanceof Date`); `napi_create_symbol` (null description ⇒
  `Symbol()`, **not** `Symbol("")` — JsRT quirk, provider must
  special-case); Date create/get via intrinsic calls (PR-9);
  BigInt create/get/words via string-mediated conversions (PR-9),
  including the `lossless` flag semantics for int64/uint64 gets.
- **Done when:** typeof table complete for every JS type incl.
  BigInt and external (`napi_external`); coercion probes with
  valueOf/toString side effects match Node ordering; BigInt
  round-trips at 2^63/2^64 boundaries and beyond (words form);
  date round-trip at epoch/negative times.
- **Prove:** probes + differential vs Node on the coercion corpus.
- **Ownership:** required for cached intrinsics (rooted per env).

**Gate G2:** N2 probes green; object_factory/function_factory
examples (N0.5 assets) pass.

### Wave 3 — binary, wrap, promises, script

#### N3.1 — ArrayBuffer, TypedArray, DataView
- **Owns:** `lib/NodeApi/NodeApiBinary.*`; A5.
- **Needs:** G1; G2 helpful.
- **Do:** create/get-info for ArrayBuffer (incl. external with
  finalizer record per §3.7), TypedArray (all JsRT-supported types;
  BigInt64/BigUint64 ⇒ `napi_generic_failure` + recorded
  non-conformance, §4.3), DataView; info composition per audit §2.4
  (data = storage + byteOffset; length = byteLength/elemSize);
  detach via `JsDetachArrayBuffer`; **investigate** detached-state
  detection (storage behavior on detached; JS `detached` getter) and
  implement `napi_is_detached_arraybuffer` from evidence — if neither
  works, stop-and-escalate for a JsRT export (§4.3).
  R4 discipline on every storage pointer.
- **Done when:** probes: every typed-array type's info tuple correct
  (type/byteOffset/length/data), external buffer finalizer fires
  once on collect, detach observable from JS, detached query correct
  for attached/detached/zero-length buffers, storage borrow
  invalidated correctly after a JS call that detaches.
- **Prove:** probes + vendored arraybuffer/dataview tests.
- **Ownership:** required — external buffer records (owner, finalize
  transfer point, detach invalidation).

#### N3.2 — Externals + finalizer queue machinery
- **Owns:** `lib/NodeApi/NodeApiWrap.*` (external + queue half); A3
  external functions.
- **Needs:** G1 (N1.2/N1.3); §3.7 design.
- **Do:** `napi_create_external`/`napi_get_value_external` on
  `JsCreateExternalObject` with trampoline+record (§3.7); the
  finalizer queue + drain machinery used by N3.1/N3.3; drain points
  wired into the N1.5 entry guard and destroy.
- **Done when:** external round-trips its pointer; finalizer fires
  exactly once, on the env thread, with live env, after the value is
  collected and the next API/host-pump drain runs; no finalizer runs
  during GC (assert via thread-id + reentrancy probe); 10k
  create/collect cycles leak nothing.
- **Prove:** probes under `-CollectGarbage` churn (≥10 repeats).
- **Ownership:** required — WrapRecords (provider allocator, release
  at drain/destroy, GC invalidation events).

#### N3.3 — Wrap, type tags, define-class support, add_finalizer
- **Owns:** `NodeApiWrap.*` (wrap half); A3 wrap/tag/finalizer
  functions.
- **Needs:** N3.2; N2.4 (define_class already landed there — this
  task covers wrap semantics classes depend on).
- **Do:** §3.7 side table: `napi_wrap/unwrap/remove_wrap` (wrap of
  wrapped ⇒ `napi_invalid_arg`; unwrap of unwrapped ⇒
  `napi_invalid_arg`; remove_wrap returns data and suppresses the
  finalizer); type tags stored in the record (tag of unwrapped ⇒
  check returns false; double-tag ⇒ `napi_invalid_arg`);
  `napi_add_finalizer` = record without wrap data semantics +
  optional reference out-param per header docs.
- **Done when:** class-wrap probes: wrapped pointer round-trip,
  type-tag match/mismatch, finalizer exactly-once after collect,
  remove_wrap suppresses finalizer, teardown with live wrapped
  object runs its finalizer once during destroy (§3.6 step 6).
- **Prove:** probes + vendored wrap/factory tests (N5.1 assets).
- **Ownership:** required — side table (key ABA discipline §3.7,
  record lifetime, teardown release).

#### N3.4 — Promises
- **Owns:** `lib/NodeApi/NodeApiPromise.*`; A6 promise functions.
- **Needs:** G1; PR-8 (host pump exists in test host from N0.3).
- **Do:** `napi_create_promise` → provider deferred record rooting
  the resolve/reject functions from `JsCreatePromise`
  (`ChakraCore.h:963-967`); resolve/reject call them with
  `this=undefined` and release the record's roots after settling;
  `napi_is_promise` via promise-state probe (`JsGetPromiseState`
  invalid-arg on non-promise). Provider installs
  `JsSetPromiseContinuationCallback` only when the host has not
  (detect and document; single-slot conflict is a host-contract
  error, surfaced loudly).
- **Done when:** promise resolved/rejected from native settles in JS
  after host pump; double settle is a JS-level no-op; deferred used
  after env destroy ⇒ clean failure, no use-after-free.
- **Prove:** promise probes + vendored promise test assets.
- **Ownership:** required — deferred records (roots held until
  settle/destroy).

#### N3.5 — Script execution, versions, external memory
- **Owns:** `lib/NodeApi/NodeApiScript.*`; A6 script/version +
  B7.
- **Needs:** G1.
- **Do:** `napi_run_script` via single `JsRun` (provider supplies
  string sourceUrl + its own source-context cookie; script exception
  ⇒ `napi_pending_exception`); `napi_get_version` (8, from N1.1 —
  move here if not landed); `napi_get_node_version` per B7;
  `napi_adjust_external_memory` as provider-side per-runtime
  accounting that returns the accumulated total and **does not**
  touch `JsSetRuntimeMemoryLimit` (audit #6); optionally trigger
  `JsCollectGarbage` on large increases — only if a test justifies
  it, otherwise accounting-only and documented.
- **Done when:** run_script returns values, sees globals (not module
  scope — per Node docs), syntax and runtime errors surface as
  pending exceptions with the right JS class; version outputs match
  the recorded decisions.
- **Prove:** script probes incl. error cases.
- **Ownership:** required for the sourceUrl/cookie handling (trivial)
  and accounting state (per-runtime, native).

**Gate G3:** N3 probes green; async_work_promise example passes
(sync-completion path); stress probes (N3.2) green across ≥10 runs.

### Wave 4 — host surface

#### N4.1 — Registration edge cases + fatal routing
- **Owns:** `lib/NodeApi/NodeApiHost.*` (registration half); B1.
- **Needs:** G1; N0.3 host.
- **Do:** pending-module registry for `napi_module_register`
  (host glue invokes `nm_register_func` per env at create-env time
  for modules registered before/without a register export); version
  rejection happens in the host (N0.3) — this task covers the
  provider-side registry + `napi_fatal_error` (host fatal path or
  abort; never returns) + `napi_fatal_exception` routing to the
  host service.
- **Done when:** deprecated-path addon registers and runs;
  double-registration of the same env is rejected cleanly;
  fatal_exception reaches the host handler with the exception value.
- **Prove:** registration probes (all paths) via host.
- **Ownership:** required — pending registry (process-lifetime list,
  thread safety, removal on unload — document unload limits).

#### N4.2 — Buffer policy implementation
- **Owns:** `NodeApiHost.*` (Buffer half); B2.
- **Needs:** N3.1; PR-6.
- **Do:** §3.9: create/copy/external/from-existing ArrayBuffer as
  marked Uint8Array views; `napi_is_buffer` marker check;
  `napi_get_buffer_info` via N3.1 composition. External buffer
  finalizers go through §3.7 records.
- **Done when:** bytes written through the returned data pointer are
  visible from JS and vice versa; copy variant is independent;
  external finalizer fires once; JS-created plain Uint8Array is
  **not** a Buffer; documented non-conformances (no Buffer global)
  listed in the receipt notes.
- **Prove:** Buffer probes + vendored `test_buffer` JS-visible subset
  triaged (runtime-global assertions are skips with reason
  needs-runtime-global).
- **Ownership:** required — external buffer records (as N3.1).

#### N4.3 — Async context + callback scopes
- **Owns:** `NodeApiHost.*` (async-context half); B3 + async-hook
  completion wiring from N1.4.
- **Needs:** G1; N1.4.
- **Do:** `napi_async_init/destroy` as provider bookkeeping records
  (async_hooks propagation explicitly no-op — documented);
  `napi_open/close_callback_scope` with mismatch ⇒
  `napi_callback_scope_mismatch`; `napi_make_callback` = context
  enter + handle scope + `napi_call_function` equivalent + microtask
  drain via host pump (PR-8). Async cleanup hook completion path at
  destroy (§3.6 step 4).
- **Done when:** make_callback invokes JS with correct this/args and
  runs queued promise reactions before returning (pump contract);
  scope mismatch cases return the right status.
- **Prove:** callback-scope probes.
- **Ownership:** required — async-context records (created/destroyed
  pairing, env teardown release).

#### N4.4 — Async work + thread-safe functions
- **Owns:** `NodeApiHost.*` (async-work/TSFN half); B4 + B5.
- **Needs:** PR-7 (host services in test host), N4.3, G3.
- **Do:** Async work state machine (created → queued → executing →
  completed/cancelled → deleted) on host `post_work`; execute runs
  off-thread touching **no** JS/env state (R3); complete runs on the
  env thread via the §3.8-adjacent entry (context + scope + callback
  record); cancel-before-start delivers `napi_cancelled` to
  complete; delete rules per header docs. TSFN: refcounted state
  with holders = {JS side, producer threads (acquire), queued items,
  pending finalize}; `call_js_cb` invoked on env thread; blocking
  call from the env thread itself must not deadlock (return
  `napi_queue_full` per precedent); ref/unref translate to host
  loop ref/unref (coalesced per env); teardown aborts queue and runs
  `thread_finalize_cb` on the env thread. `napi_get_uv_event_loop`
  returns the injected loop or `napi_generic_failure` (D7).
  Without services: every function in this card returns
  `napi_generic_failure`.
- **Done when:** async-work and TSFN probes pass with services
  (round-trip values, cancel, queue-full nonblocking, multi-producer
  threads, teardown with in-flight items) and fail cleanly without;
  no TSFN callback after env destroy; leak check on state objects.
- **Prove:** probes incl. 4-producer stress (≥10 repeats) via host.
- **Ownership:** required — work/TSFN state (thread ownership,
  refcount holders, cancel/close release paths). This card touches
  thread boundaries: full memory-management record, no shortcuts.

**Gate G4:** N4 probes green with the test host's services;
no-services matrix returns the documented failures.

### Wave 5 — conformance and packaging

#### N5.1 — Vendored `test/js-native-api` adoption
- **Owns:** `test/nodeapi/run_matrix.py` (full runner), triage list
  in `doc/node-api-receipt.md`.
- **Needs:** G2 (G3 for full coverage); N0.5 assets.
- **Do:** Run every vendored js-native-api test through the runner;
  each test ends pass / skip-with-reason (taxonomy from N0.5) / fail.
  Failures are provider bugs until proven otherwise — triage each
  into a fix task or a §4.3 gap entry. Publish counts per platform.
  Differential mode: the same addon binary loaded by real Node runs
  the same JS; JS-visible outputs are compared (status text excluded,
  PR-12).
- **Done when:** zero unexplained failures; skip list reviewed;
  counts recorded for macOS arm64 (Linux x64 in N5.5's receipt pass).
- **Prove:** runner JSON + receipt doc.
- **Ownership:** not required.

#### N5.2 — node-addon-examples ladder
- **Owns:** example runner entries; receipt section.
- **Needs:** G3 (G4 for async examples).
- **Do:** Run the `napi/` examples in dependency order (hello_world →
  function_arguments → callbacks → object/function factories →
  object_wrap/factory_wrap → passing_wrapped → async_work_promise →
  threadsafe_function), each against provider and (differentially)
  Node. Exclude `nan/` variants (out of scope, §0.2).
- **Done when:** every claimed example passes on the provider;
  failures triaged as in N5.1.
- **Prove:** example table in receipt with per-example status.
- **Ownership:** not required.

#### N5.3 — node-addon-api spot adoption (stretch; may defer)
- **Owns:** triage notes.
- **Needs:** N5.1 green.
- **Do:** Compile a selected subset of node-addon-api's C++ tests at
  `NAPI_VERSION=8` against the provider; run per-test (their suite
  harness assumes a Node executable — do not adopt it wholesale).
- **Done when:** subset results recorded with the same
  pass/skip/fail discipline; no silent skips.
- **Prove:** receipt section.
- **Ownership:** not required.

#### N5.4 — Stress and teardown campaign
- **Owns:** `test/nodeapi/stress/` probes; receipt section.
- **Needs:** G3; N4.4 for TSFN stress.
- **Do:** GC churn under `-CollectGarbage` while creating/collecting
  externals, wraps, deferreds, refs (≥10 repeats each — single green
  runs are not evidence for timing-sensitive GC bugs); env
  create/destroy churn with live wraps and pending finalizers;
  root-balance assertions via the N1.2 debug counter; TSFN
  multi-producer teardown races.
- **Done when:** all stress probes green across repeats with no root
  leaks and no finalizer count drift.
- **Prove:** stress logs + counters in receipt.
- **Ownership:** the campaign *validates* prior records; file any new
  retained shape as its own record.

#### N5.5 — Packaging, exports gate, receipts, README
- **Owns:** packaging in `tools/build/`, `doc/node-api-receipt.md`,
  public README section.
- **Needs:** G4 (or G3 with scoped claim), N5.1–N5.4.
- **Do:** exports inventory wired as a CI check (built library ⊇
  generated inventory; experimental/v9/v10 symbols absent unless a
  future lane adds them); header install verified by a from-scratch
  consumer build in a temp dir (vendored headers + built library +
  `zig cc`, no Node executable involved); optional node-gyp consumer
  proof (manual, recorded); run the full matrix on Linux x64 and add
  its receipt; write the README claim exactly per PR-1 with links to
  receipts and the known non-conformance list (Buffer global,
  BigInt64 typed arrays, async without host services, async_hooks
  no-op).
- **Done when:** both platform receipts exist; consumer build passes;
  README claims match evidence word-for-word.
- **Prove:** command logs in receipt.
- **Ownership:** not required.

---

## 7. Verification strategy

Evidence levels, in order of strength for this project:

1. **Generated inventory check** (N0.1/N1.6): symbols present, no
   extras beyond the v8 set.
2. **Task probes**: small addons + JS drivers per task card, run by
   `test/nodeapi/run_matrix.py` through `ch-nodeapi`; every probe
   asserts statuses and JS-visible results, including the negative
   cases named in its card.
3. **Vendored Node tests** (N5.1): the strongest engine-surface
   oracle available; pass/skip/fail published, skips reasoned.
4. **Examples + differential vs Node** (N5.2): same addon binary,
   provider vs Node, JS-visible output compared (error *text*
   excluded, PR-12).
5. **Stress/teardown** (N5.4): repeated GC/teardown campaigns.
6. **Platform receipts** (N5.5): macOS arm64 and Linux x64 separately.
   Neither substitutes for the other.

Commands (build steps created by N1.6/N0.3; until then the runner
scripts are invoked directly):

```sh
mise exec -- zig build -Dprofile=jit        # engine + provider
mise exec -- zig build check-nodeapi        # probe matrix (N1.6+)
python3 test/nodeapi/run_matrix.py --suite probes
python3 test/nodeapi/run_matrix.py --suite vendored --platform-report
mise exec -- zig build check                # existing gates: pins unchanged
```

Rules: generated files never land in the source tree (build in temp
dirs, as `tools/build/check_native.py` does); a failing probe stops
its lane; skip lists are data with reasons, reviewed at each gate.

---

## 8. Risks

| Risk | Where | Mitigation (task) |
|---|---|---|
| Roots released early / stored unrooted in native containers | everywhere | R1–R6 (§3.4); root-counter probes (N1.2); stress (N5.4) |
| Finalizer runs on GC thread or after env death | wrap, externals, buffers | D4 queue+drain (§3.7); thread-id probes (N3.2); teardown order (§3.6) |
| Wrap side-table ABA / stale record after collection | N3.3 | weak-handle keying + validation (§3.7); stop-and-escalate fallback |
| Status vs pending-exception drift | every entry | §3.10 + N0.6 allowlist; per-row probes (N1.5) |
| Buffer treated as real Node Buffer | N4.2, addons' JS deps | PR-6 marker design; documented non-conformance; no `Buffer` global claim |
| Async faked without a host loop | N4.3/N4.4 | PR-7 services-or-failure (D7); no provider thread pool in v1 |
| BigInt/Date JS-mediated paths too slow/wrong | N2.6 | PR-9 evidence gate → JsRT extension lane, not hidden hacks |
| Version creep (v9/v10 symbols without semantics) | loader, inventory | reject >8 at load (§1.2); inventory check forbids extras (N1.6) |
| Zig 0.17 churn in provider code | all provider tasks | pinned toolchain; small provider surface; PR-4 fallback recorded |
| node-addon-api/Node tests assumed reusable wholesale | N5.* | per-test adoption with skip taxonomy (§1.5, N0.5) |

---

## 9. Sub-agent handoff protocol

1. Take one task card. Read §0–§3, the card, and every PR it cites.
   If a blocking PR is undecided, stop and escalate — do not guess.
2. Re-verify cited `file:line` anchors against the current checkout;
   report drift. Anchors in this plan were verified at `48b7db6d8`
   (JsRT audit) and against Node 22.22.3 headers (surface counts).
3. Read [memory-management.md](memory-management.md) before any task
   marked "ownership record required"; include the record (resource,
   domain/allocator, owner/transfer point, invalidation events,
   thread/context, release paths) in your report.
4. Keep the change inside your card's files (§5.2). Cross-cutting
   discoveries go in the report as follow-ups, not into the diff.
5. Follow D6 for any shared-code edit; follow D7/D8 always.
6. Report: (a) files changed; (b) Done-when checklist, pass/fail per
   item; (c) exact commands + tallies; (d) ownership record where
   required; (e) PRs touched and any decision you need; (f) named
   gaps and unverified claims (especially Linux x64 — never claim it
   from a Mac run).

---

## 10. Future lanes (explicitly not in this plan's gates)

- **v9 pack**: `node_api_symbol_for`, syntax-error pair,
  `node_api_get_module_file_name` (+ per-env filename plumbing).
  Cheap once registration plumbing exists.
- **v10 pack**: external-string + property-key creators,
  `node_api_create_buffer_from_arraybuffer`, **plus** the semantic
  changes (reference type rules, TSFN exception routing). Not
  additive-only; needs its own decision record.
- **Experimental**: SharedArrayBuffer pair, `node_api_post_finalizer`,
  basic-env finalizers. Only with a product need.
- **JsRT extensions** (engine lanes, separate plans): BigInt
  create/read + typeof, Date, detached query, BigInt64 typed arrays,
  weak-cell finalizer delivery — each filed from §4.3 evidence.
- **A Node runtime on ChakraCore** (§0 C): separate project; this
  plan's provider is a prerequisite, not a down payment claim.
- **Zig provider port (PR-4 / D5 decision record 2026-11-11)**:
  re-evaluate a Zig port of `libChakraNodeApi` only after G5 passes
  in C++; the C ABI is identical, so this is a mechanical port gated
  on fresh spike evidence (compile friction, header translation,
  link/run), not part of this plan's gates.

## 11. Status tracker

Update only with gate evidence (commands + tallies) in the same change.

| Wave | Status | Evidence |
|---|---|---|
| 0 — decisions/assets/harness | not started | — |
| 1 — env + lifetime kernel | not started | — |
| 2 — values/objects/functions/errors | not started | — |
| 3 — binary/wrap/promises/script | not started | — |
| 4 — host surface | not started | — |
| 5 — conformance + packaging | not started | — |

---

## Appendix A — JsRT mapping quick reference

Condensed from the JsRT audit (anchors: `C` = `lib/Jsrt/ChakraCommon.h`,
`K` = `lib/Jsrt/ChakraCore.h`, `J` = `lib/Jsrt/Jsrt.cpp`).

| Area | Verdict | Key backing |
|---|---|---|
| Runtime/context | direct | `JsCreateRuntime` C:816, `JsCreateContext` C:1047, `JsSetCurrentContext` C:1072, `JsDisposeRuntime` C:845 |
| Rooting | primitive exists | `JsAddRef` C:989, `JsRelease` C:1005; weak: `JsCreateWeakReference` K:987 |
| Primitives | direct/partial | getters C:1259-1408; int64/uint32 = provider conversion |
| Strings | partial | UTF-8/16 pairs K:585-669; Latin1 = transcode |
| Objects/descriptors | direct/partial | C:1499-1801, `JsDefineProperty` C:1742; freeze/seal via intrinsics |
| Functions | direct (enhanced) | `JsCreateEnhancedFunction` K:404; `JsCallFunction` C:2176; oracle `bin/NativeTests/JsRTApiTest.cpp:734` |
| Externals | partial | `JsCreateExternalObject` C:1517 (one data+cb, GC-thread, no context) |
| Wrap/tags/finalizer on arbitrary objects | provider-built | side table + before-collect + drain (§3.7) |
| Binary | direct classic | C:1990-2159; BigInt64 arrays absent from `JsTypedArrayType` C:492 |
| Promises | near-direct | `JsCreatePromise` K:963 (returns resolve/reject fns); continuation C:2485 |
| Symbols | partial | `JsCreateSymbol` C:1228 (null-desc quirk); `Symbol.for` via intrinsic call |
| BigInt / Date | absent | string/intrinsic-mediated (PR-9) or engine lane |
| Errors | direct | create family C:2256-2339; exception state C:2366-2414 (**ChakraCommon.h**) |
| Script | direct | single `JsRun` K:733 |
| Async/TSFN/uv | absent | host-injected services only (PR-7) |

## Appendix B — Sources

- Node-API documentation and version matrix, Node.js v22 docs
  (nodejs.org/api/n-api.html) — surface semantics, registration,
  version support ranges.
- Installed official headers, Node 22.22.3
  (`include/node/js_native_api*.h`, `node_api*.h`) — declaration
  counts in §1.1, per-function docs as the normative semantics for
  implementers.
- nodejs/node v22.x `src/node_api.cc`, `src/node_binding.cc`,
  `test/js-native-api/`, `test/node-api/` — loader behavior and test
  oracles (§1.3, §7).
- facebook/hermes `API/napi/` (README, COMPATIBILITY, hermes_napi.h)
  — provider architecture, host services, finalizer discipline.
- oven-sh/bun issue #158 — symbol coverage vs runtime-suite honesty.
- BabylonJS/JsRuntimeHost `js_native_api_chakra.cc` — JsRT-surface
  provider precedent and its gaps.
- nodejs/node-api-cts — engine/runtime test split (work-in-progress;
  structure only, not a gate).
