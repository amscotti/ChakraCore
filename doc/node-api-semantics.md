# Node-API provider — semantics companion

Companion to `doc/node-api-plan.md` §3. Normative for N1.x–N3.x
implementers. Where this doc and the plan disagree, the plan wins
and this doc is amended in the same change.

Status: **draft for N0.4 review**. Owner: N0.4. Depends on N0.1
inventory and N0.5 asset manifest for vendored-test pinning.

Governing docs:

- [node-api-plan.md](node-api-plan.md) §3.3–§3.10 (cited as
  `[plan §3.x]`), §4 task partition, §6 task cards.
- [memory-management.md](memory-management.md) (cited as
  `[mem §N]`); the ownership records in Appendix A are part of
  this doc per N0.4.
- Vendored Node 22.22.3 headers and `test/js-native-api` / examples
  assets per PR-2/PR-11 (pinned by N0.1/N0.5). Header docs are the
  oracle for per-function pending-exception and `*_expected`
  semantics unless a vendored test contradicts them.

Conventions:

- "MUST/MUST NOT/SHOULD" are normative. "Probe" means a small
  addon + JS driver under `test/nodeapi/probes/` run by
  `run_matrix.py` through `ch-nodeapi` (N0.3/N5.1), asserting
  statuses and JS-visible results including negative cases.
- Every rule cites the plan section it refines: e.g.
  `R-ENV-2 [plan §3.6 step 2]`. A rule without such a citation is
  not normative.
- `AMB-*` flags an ambiguity left open by header docs. Each flag
  names the vendored-test evidence (N0.5 manifest asset) that
  decides it. `PLAN-GAP-*` flags a plan silence this doc resolves
  by a stated choice + probe (no vendored test covers the private
  host API or the provider test hooks).
- Probe IDs are stable: `P1-*` env/lifetime (N1.1), `P2-*` scopes
  (N1.2), `P3-*` refs (N1.3), `P4-*` instance/cleanup (N1.4),
  `P5-*` status guard (N1.5), `P6-*` values/objects/functions
  (N2.x), `P7-*` wrap/finalizer (N3.2/N3.3), `P8-*` callbacks
  (N2.4). Section 7 maps every rule to its card(s) + probe(s).

## 0. Model in one page

One `NodeEnv` per (JsContext × addon registration) `[plan §1.3]`.
Env state: `Alive → Closing → Dead` `[plan §3.3]`; only `Alive`
accepts public calls. Provider-owned containers hold JS values
only rooted per R1 `[plan §3.4]`; `napi_value` **is**
`JsValueRef` under §3.4 conditions (D2) `[plan §3.3, §3.4]`.
User finalizers never run inside a GC callback (D4): GC enqueues,
the env thread drains `[plan §3.7]`. All N-API functions use
`JsCreateEnhancedFunction` (D3) with the §3.8 thunk `[plan §3.8]`.
Every public entry follows the §3.10 discipline:
validate → pending-check → pre-check → JsRT → map `[plan §3.10]`.

---

## 1. Env lifecycle state machine

### 1.1 `NodeEnv` layout (normative sketch)

- **R-ENV-1.** One `NodeEnv` holds `runtime`, `context`,
  `owner_thread`, `state ∈ {Alive, Closing, Dead}`,
  `module_api_version (≤8)`, provider-owned
  `module_filename` copy, `services`/`host_ctx`, `scopes` stack,
  `refs` map, `wraps` side table, `finalizer_queue` (mutex + list),
  `callbacks` stack, `instance_data`, cleanup-hook lists,
  `last_error` `[plan §3.3]`. Env is allocated by the provider
  allocator, never in Recycler memory; every stored `JsValueRef`
  is rooted per R1 `[plan §3.3]` `[mem §2: Recycler/JS value]`.
- **R-ENV-2.** `N1.1` fixes the `NodeEnv` layout; other tasks
  treat it as a reviewed interface and propose changes in
  reports, never edit concurrently `[plan §5.1]`.

### 1.2 Create → Closing → Dead (step-by-step)

- **R-ENV-3 (create).** `chakra_nodeapi_create_env(runtime,
  context, module_filename, services, host_ctx)` allocates the
  env on the calling thread, records `owner_thread = caller`,
  `state = Alive`, negotiates `module_api_version` (absent
  version symbol ⇒ 8; >8 rejected by the host before env
  creation) `[plan §3.2, §1.3]`. G1 evidence: 1000 sequential
  create/destroy cycles without leak growth (provider allocator
  accounting) (N1.1).
- **R-ENV-4 (state guard).** Every public entry validates the
  env, enters its context (`JsSetCurrentContext`), and restores
  the previous context on exit `[plan §3.2]`. During `Closing`,
  new public calls (except teardown-internal helper calls) MUST
  return `napi_generic_failure` and record `last_error`
  `[plan §3.6 step 1]` `[plan §3.10]`. During `Closing`,
  JS-calling APIs needed by finalizers likewise return
  `napi_generic_failure` (finalizers run with a live env but a
  host that refuses further JS) `[plan §3.7]`.
- **R-ENV-5 (steps).** `chakra_nodeapi_destroy_env(env)` runs
  on the env thread, before `JsDisposeRuntime`, with no provider
  call in flight, in this exact order `[plan §3.6]`:
  1. `state → Closing` (see R-ENV-4).
  2. Drain the finalizer queue (§4); wrap finalizers run with a
     still-valid env.
  3. Run env cleanup hooks, LIFO.
  4. Run async cleanup hooks; host pumps until each hook calls
     `napi_remove_async_cleanup_hook`. Host MUST bound this; the
     test host pumps with a timeout and reports a hang as a test
     failure, not a provider deadlock.
  5. Invoke the instance-data finalizer (if any).
  6. Delete all references (strong roots released; weak handles
     released), close all scopes (roots released), free wrap
     records (remaining wrap finalizers run here, exactly once).
  7. `state → Dead`; free `NodeEnv`. Only now may the host
     dispose the Chakra context/runtime.

  The plan's note is part of the spec: where Node's exact
  teardown behavior is uncertain from docs, vendored
  `test_finalizer`, `test_cleanup_hook`,
  `test_async_cleanup_hook` assets (N5.1 scope, N0.5 manifest)
  are the oracle; N1.4 MUST check this order against them and
  amend plan §3.6 if they disagree `[plan §3.6 note, N1.4]`.

- **R-ENV-6 (destroy thread/context).** Destroy MUST be called
  on the env thread. Calling it from another thread is host
  misuse: debug builds assert, release builds return without
  acting (no partial teardown) — a single complete destroy is
  the only defined path. This choice satisfies N1.1's
  "rejected or documented as UB (choose: debug assert + release
  `napi_invalid_arg`)" at the `napi_*` level; the host API is
  `void` so release behaviour is a no-op return, which the host
  MUST treat as fatal. Probe P1-3 asserts the debug assert
  fires and no env state changes on a wrong-thread destroy
  attempt. This is a `PLAN-GAP-1` resolution: plan §3.2 states
  the thread requirement but not the release fallback for the
  `void` host function.

### 1.3 Destroy-with-live-wraps, destroy-twice, calls on Dead env

- **R-ENV-7 (destroy with live wraps).** A wrapped object still
  reachable from JS at destroy time has its wrap finalizer run
  exactly once at step 6 (after refs/scopes are torn down in the
  same step), not at step 2's queue drain unless it was already
  enqueued by GC. Rationale: step 2 drains already-collected
  records; step 6 is the backstop for live records so no
  `WrapRecord` leaks and no user data is lost `[plan §3.6
  steps 2, 6]` `[plan §3.7]`. Sub-order inside step 6 is
  normative: (a) delete refs, (b) close scopes, (c) run
  remaining wrap finalizers, (d) free records. A finalizer at
  (c) MUST observe refs/scopes already gone: any public API it
  calls returns `napi_generic_failure` per R-ENV-4. Probe P1-1
  asserts this ordering with an event log. Evidence: vendored
  `test_finalizer` (N5.1) is the oracle; if it shows a different
  order, amend plan §3.6 (see AMB-ENV-1). N1.4 + N3.3.
- **R-ENV-8 (instance-data wart reproduced).** Replacing the
  instance data does **not** call the previous finalizer.
  Destroy step 5 runs the currently-registered finalizer once
  `[plan §3.6 step 5]`. Probe P4-1 asserts replace-then-destroy
  calls the finalizer once, not twice.
- **R-ENV-9 (destroy twice).** `chakra_nodeapi_destroy_env`
  returns `void` `[plan §3.2]`, so a second destroy cannot
  return a `napi_status`. The provider MUST keep a tombstone
  entry for a destroyed env pointer value (provider allocator
  reuse of the same address MUST NOT resurrect the tombstone:
  tombstones are keyed by `{pointer, generation}` or the
  registry is cleared by generation) and a second call MUST:
  assert in debug builds, return immediately without touching
  freed memory in release builds, and leave the host's
  runtime/context untouched. No public `napi_*` call can be
  issued against a freed env: the pointer is dead. Probe P1-2
  issues destroy twice through the test host and asserts clean
  host-error behaviour (no crash, no double-free, second call
  reports the host error) rather than a `napi_status`. This is
  `PLAN-GAP-2` resolved: plan N1.4 requires "destroying twice
  is a host error surfaced cleanly" but §3.2's `void` signature
  leaves the signal to the host contract. Vendored tests do not
  cover the private host API; the probe + N1.4's ordering probe
  are the evidence.
- **R-ENV-10 (calls on Dead env).** Distinguish two windows,
  per `[plan §3.6 step 1, step 7]` and N1.1:
  - *Closing and the Dead-before-free window* (state flag
    set, memory still mapped): any public call MUST return
    `napi_generic_failure` (R-ENV-4). Probes P1-4 exercise the
    `Closing` window by calling a public API from a cleanup
    hook / finalizer during destroy (guaranteed `Closing`) and
    assert `napi_generic_failure`.
  - *After free* (`NodeEnv` memory released): the env pointer is
    dangling. Passing it to any `napi_*` is host use-after-free
    (UB), not a provider-recoverable error. The provider MUST
    NOT attempt to validate through the freed pointer. Probes
    MUST NOT dereference a freed env; the Dead-before-free
    branch is covered by the `Closing` probes plus a debug-only
    state query (see PLAN-GAP-3).
- **R-ENV-11 (wrong-thread calls).** A `napi_*` call whose env
  belongs to another thread MUST be rejected:
  debug builds assert (with env pointer + owner thread in the
  diagnostic), release builds return `napi_invalid_arg` without
  entering the wrong context `[task N1.1 Do, plan §3.2]`.
  Rooting requires a thread context on the calling thread
  (R3) `[plan §3.4]`, so cross-thread use can never be made
  correct by entering the context from the wrong thread.
  Probe P1-5 spawns a helper thread that calls a trivial
  `napi_*` with the env and asserts the release status (or the
  assert in debug). `PLAN-GAP-3`: the plan leaves the choice to
  N1.1 ("rejected or documented as UB"); this doc fixes the
  choice above and the debug-only introspection probe P1-4b
  (state query via private test API) to make "Dead-before-free"
  observable without UAF.

### 1.4 Reentrancy and pumping during teardown

- **R-ENV-12.** Provider code MUST NOT call arbitrary JS while
  holding the `finalizer_queue` mutex or any non-reentrant
  native lock `[mem §6, §7]`. Finalizer drain invokes user
  code (a `napi_finalize` callback) which may reenter the
  provider; the queue lock is held only to pop a record, never
  across the user call. Probe P7-4 covers reentrant drain.
- **R-ENV-13.** Promise/continuation pumping during destroy is
  host-owned (PR-8) `[plan §3.2, §3.6 step 4]`; the provider
  never spins its own loop. The test host pumps explicitly;
  a hang is a host test failure, not a provider deadlock.

### 1.5 Probe list for N1.1 (plus N1.4 ordering)

| Probe | What it asserts | Card |
|---|---|---|
| P1-1 | Ordering: step 2 drain → step 3 LIFO hooks → step 4 async hooks → step 5 instance finalizer → step 6 refs/scopes/wrap backstop; event log compared verbatim | N1.4 |
| P1-2 | Destroy twice: clean host error, no crash/double-free | N1.1, N1.4 |
| P1-3 | Wrong-thread destroy attempt: assert/no-op, no partial teardown | N1.1 |
| P1-4 | Calls during `Closing` (from hook/finalizer) return `napi_generic_failure` | N1.1 |
| P1-4b | Debug state query observes `Closing`/`Dead` without UAF | N1.1 |
| P1-5 | Wrong-thread `napi_*` call: debug assert + release `invalid_arg`, no context entry on wrong thread | N1.1 |
| P1-6 | 1000× create/destroy: allocator high-water flat (provider allocator accounting) | N1.1 |
| P1-7 | Replace instance data then destroy: old finalizer never called, new finalizer called once | N1.4 |

Negative cases are mandatory: each probe MUST fail the suite if
the ordering or status differs, not merely print.

### 1.6 Ambiguities (env)

- **AMB-ENV-1.** Whether refs/scopes teardown (step 6) precedes
  or follows the last wrap-finalizer run when both are live at
  destroy. Spec choice: refs/scopes first, then wrap backstop
  (R-ENV-7). Decided by vendored `test_finalizer` + ordering
  probe P1-1; if vendored evidence disagrees, amend plan §3.6.
- **AMB-ENV-2.** Exact LIFO vs FIFO order of async cleanup-hook
  completion when hooks remove themselves during the pump.
  Host-pump contract only. Decided by vendored
  `test_async_cleanup_hook` (N5.1); test host timeout bound is
  `PLAN-GAP-4` (plan mandates a timeout but not its value —
  N0.3 fixes it in the host, this doc requires it be a test
  failure, never provider deadlock).

---

## 2. Handle scopes

### 2.1 Open / close / escape rules

- **R-SC-1.** A scope is a list of rooted values (R1) plus
  `escapable_flag` + `escaped_flag` on a per-env stack
  `[plan §3.5; §3.3 `scopes` field]`. Every API returning a new
  `napi_value` roots it into the innermost open scope
  `[plan §3.5; R1 plan §3.4]`. The stored value is `JsAddRef`'d
  at store time and `JsRelease`'d at the matching release point
  (scope close) `[plan §3.4 R1]` `[mem §2: Rooting]`.
- **R-SC-2 (plain scope).** `napi_open_handle_scope` pushes a
  non-escapable scope. `napi_close_handle_scope` closes the
  innermost scope. Closing releases all its roots and pops it.
  A value created in the closed scope is thereafter unrooted:
  uses of its handle are misuse (UB unless the value was also
  escaped or refereed); the value is collectable once GC runs.
  Probe P2-3 proves collection via a weak ref + GC `[task N1.2]`.
- **R-SC-3 (escapable scope).** `napi_open_escapable_handle_scope`
  pushes a scope with `escapable_flag=true`. `napi_escape_handle`
  moves the value's root from the escapable scope to its
  parent. The escaped value survives the escapable scope's
  close; all other roots in that scope die. `[plan §3.5]`.
- **R-SC-4 (escape-once).** `napi_escape_handle` is callable
  once per escapable scope; a second call returns
  `napi_escape_called_twice` and MUST NOT move any root or
  clear the first escape `[plan §3.5]`. Worked example (a) in
  Appendix C. Probe P2-4.
- **R-SC-5 (close escapable).** `napi_close_escapable_handle_scope`
  closes the innermost escapable scope, releasing its remaining
  roots (the escaped value, if any, is already in the parent and
  is not released). Closing an escapable scope without any
  escape is legal; the scope simply dies.
- **R-SC-6 (LIFO / reverse-order).** Scopes close in LIFO order.
  An out-of-order close returns `napi_handle_scope_mismatch` and
  the provider MUST still release the requested scope's roots
  (no leak on misuse) `[plan §3.5]`. The mismatch MUST NOT
  unwind the whole stack: only the named scope is closed;
  subsequent correct-order closes see the remaining stack.
  Worked example (b) in Appendix C. Probe P2-5.
- **R-SC-7 (default scope / no scope open).** After the host
  registration scope and the thunk implicit scope are accounted
  for (§5), the provider MUST guarantee an outermost default
  scope exists for the env's lifetime: every value-returning API
  invoked outside an explicit scope roots into the default
  scope, which is closed only at destroy step 6. Rationale: the
  plan requires every returned value be rooted in the innermost
  open scope `[plan §3.5]` and exposes no public "open default
  scope" call; without a default, the first rooting API called
  outside a scope would have nowhere to root. This is
  `PLAN-GAP-5` resolved by the default-scope invariant. Probe
  P2-1 (create without explicit scope, destroy, root count
  returns to baseline) proves it.
- **R-SC-8 (pending-exception allowlist).** APIs Node documents
  as callable with a pending exception (scope close, ref delete,
  exception queries) MUST NOT early-return on pending state
  `[plan §3.5]`. Membership is per N0.6's header-doc allowlist;
  §6 marks every allowlist row for N0.6 confirmation. Probe
  P5-3.

### 2.2 Misuse statuses

| Misuse | Status | What provider does | Cite |
|---|---|---|---|
| `escape` twice on one escapable scope | `napi_escape_called_twice` | Second call moves nothing; first escape stands | `[plan §3.5]` |
| Close out of LIFO order | `napi_handle_scope_mismatch` | Still releases the named scope's roots; stack otherwise intact | `[plan §3.5]` |
| Callback-return scope imbalance (thunk) | `napi_handle_scope_mismatch` + cleanup, never leak/crash | See §5.4 | `[plan §3.5, §3.8]` |
| `escape` on non-escapable scope / no escape attempted scope | see AMB-SC-1 | — | — |
| Close with bad/NULL scope handle | `napi_invalid_arg` | Validate first (`[plan §3.10]`) | `[plan §3.10]` |

- **AMB-SC-1.** Plan §3.5 does not name the status when
  `napi_escape_handle` is called with a non-escapable innermost
  scope or a NULL scope. Spec choice (to be confirmed by N0.6
  from header docs): `napi_invalid_arg`. N0.6 MUST confirm and
  correct this line if header docs say otherwise; probe P2-4b
  is added the same task. Vendored evidence: vendored
  handle-scope tests (N5.1 subset; exact asset per N0.5
  manifest).
- **AMB-SC-2.** Whether `napi_close_escapable_handle_scope`
  on a plain (non-escapable) scope returns
  `napi_handle_scope_mismatch` or `napi_invalid_arg`. Spec
  choice: `napi_handle_scope_mismatch` (it is an ordering/
  kind mismatch against the stack top). N0.6 MUST confirm from
  header docs; vendored handle-scope tests decide. Probe P2-5b.

### 2.3 Values returned from native methods

- **R-SC-9.** A value returned from a N-API callback (the
  `napi_callback` return) lives in the caller's scope after the
  thunk finishes (§5): the thunk roots it into the caller's
  scope before closing the implicit scope `[plan §3.8 step 4]`.
  If the JS caller immediately drops all other references, the
  value is held by the caller's scope until that scope closes
  (or by an explicit ref). Implementers MUST NOT root the
  callback return in the implicit (closing) scope — that would
  let GC collect the return value the JS caller still holds.
  Probe P8-6 (return value observed from JS + weak-ref check
  after caller scope close).

### 2.4 Balance verification

- **R-SC-10.** The host glue opens a registration scope around
  `napi_register_module_v1`; the thunk opens an implicit scope
  per callback invocation `[plan §3.5]`. On callback return the
  thunk MUST verify scope/callback balance (stack depths equal
  to entry depths except for the implicit scope it will close);
  imbalance is a provider bug, surfaced as
  `napi_handle_scope_mismatch` + cleanup (close the implicit
  scope, release its roots, pop the callback record), never a
  leak or crash `[plan §3.5]`. Full callback discipline is §5.
  Probe P2-6 (balance hook) + P8-5 (nested callback balance).

### 2.5 Exact probe list for N1.2 (implementers write these)

| Probe | Steps | Expected | Root-count assertion |
|---|---|---|---|
| P2-1 | Create value with no explicit scope; destroy env | `napi_ok`; value in default scope | returns to baseline |
| P2-2 | Open inner+outer scopes; escape value from inner to outer; close inner | escaped value alive in outer; non-escaped sibling dead | inner roots released, escaped root now outer's |
| P2-3 | Create in inner scope; close; force GC (`-CollectGarbage` equiv via host) | value collectable (weak-ref deref empty) | baseline |
| P2-4 | Escape twice on one escapable scope | 1st `napi_ok`, 2nd `napi_escape_called_twice`; first escape stands | no double-move |
| P2-4b | Escape on plain scope / bad scope (per AMB-SC-1) | `napi_invalid_arg` (or amended per N0.6) | no leak |
| P2-5 | Close scope B while scope C (opened above B) still open | `napi_handle_scope_mismatch`; B's roots released | B released, C intact |
| P2-5b | Close escapable API on plain scope (per AMB-SC-2) | amended status per N0.6; no leak | baseline after correct closes |
| P2-6 | Open 10× nested scopes, close in order; balance hook | all `napi_ok` | baseline |
| P2-7 | Close/escape during pending exception (if on allowlist) | executes (no early `napi_pending_exception`) per N0.6 | baseline |

### 2.6 Ambiguities (scopes)

- **AMB-SC-1, AMB-SC-2** above; both decided by N0.6 header-doc
  extraction + vendored handle-scope tests (N5.1/N0.5 manifest).
- **AMB-SC-3.** Whether a closed scope's released value that is
  passed to a later API is `napi_invalid_arg` or silently UB.
  Spec: UB — the plan gives no validation path for dead
  handles, and rooting rules treat close as the invalidation
  point `[plan §3.4 R1, §3.5]`. Decided by vendored tests
  that reuse handles after close (N5.1 scope tests); do not
  add provider validation unless such a test demands it.

---

## 3. References (strong and weak)

### 3.1 Model

- **R-REF-1.** The provider keeps its own count and crosses
  `JsAddRef`/`JsRelease` only on 0↔1 transitions `[plan §3.4
  R2]`. Provider `Ref` state `[plan §3.3]`:
  `strong(provider_count ≥ 1, JsAddRef held)` or
  `weak(count = 0, JsWeakRef handle held)`.
  `JsRelease` does not validate balance; imbalance corrupts
  silently (asserts only in debug) `[plan §3.4 R2; §1.4]`.
  Implementers MUST serialize every transition through one
  helper (N1.3) — never call `JsAddRef`/`JsRelease` directly
  at call sites.
- **R-REF-2.** `JsWeakRef` handles are themselves `JsRef`s:
  root the handle (`JsAddRef` on the weak-ref handle) for as
  long as it is stored; release it when the record dies
  `[plan §3.4 R6]`. Probes assert the handle's root is counted
  (see §3.7).

### 3.2 Create semantics (v8 restriction)

- **R-REF-3.** `napi_create_reference(env, value, initial_refcount,
  &ref)`: `initial_refcount ≥ 1` ⇒ strong from birth (one
  `JsAddRef`); `== 0` ⇒ weak from birth (`JsCreateWeakReference`,
  no `JsAddRef` on the referent). The v8 type restriction
  applies (per Node-API v8 semantics; v10's all-types rule MUST
  NOT leak in) `[task N1.3; §4.3 row Weak napi_ref]`. N0.6 MUST
  confirm the exact v8 type allowlist from header docs and add
  one probe per rejected type (see §6 table row 4).
- **R-REF-4 (weak at count 0).** Weak refs are implemented with
  `JsCreateWeakReference` / `JsGetWeakReferenceValue`
  `[plan §4.3; lib/Jsrt/ChakraCore.h:987-1004]`. Semantics
  verified by vendored `test_reference` `[plan §4.3, task N1.3]`.

### 3.3 Strong/weak transitions

- **R-REF-5.** `napi_reference_ref` increments the provider
  count; on 0→1 transition it creates the strong root
  (`JsAddRef`) exactly once. `napi_reference_unref` decrements;
  on 1→0 it releases the strong root (`JsRelease`) exactly once
  and creates the weak handle (`JsCreateWeakReference` + R6
  root) exactly once `[task N1.3]`. The underlying handle is
  never double-created or leaked across a transition, even
  through repeated ref/unref cycles. Probe P3-1 cycles 1000×
  and asserts handle creation counts.
- **R-REF-6.** Unref at count 0 (already weak) leaves the ref
  weak. Whether it returns `napi_ok` or `napi_invalid_arg` is
  **AMB-REF-1** (see §3.8); N0.6 MUST confirm from header docs
  and vendored `test_reference`.

### 3.4 Get-value semantics

- **R-REF-7.** `napi_get_reference_value` on a live strong ref
  roots the value into the innermost scope (R-SC-1) and returns
  it. On a weak ref whose referent is still alive, same.
  On a weak ref whose referent has been collected, returns
  `napi_ok` with a null result (out-param set to NULL) — verify
  against vendored `test_reference` `[task N1.3]`. The returned
  value (when non-null) follows R-SC-1 rooting.

### 3.5 Delete semantics

- **R-REF-8.** `napi_delete_reference` deletes regardless of
  provider count: it releases the strong root if strong, or
  the weak handle (+ its R6 root) if weak, exactly once, and
  frees the provider `Ref` record `[plan §3.3; task N1.3]`.
- **R-REF-9 (delete-then-use; double-delete).** Any use of a
  deleted ref (`get_reference_value`, `ref`, `unref`) returns
  `napi_invalid_arg`. A second `delete_reference` on the same
  handle also returns `napi_invalid_arg` `[task N1.3 Done when]`.
  The handle value MUST be invalidated at delete (set to NULL /
  poisoned) so a use is a clean status, not a UAF. Probes P3-4/5.

### 3.6 `JsAddRef`/`JsRelease` caveats (normative warnings)

- **R-REF-10 (tagged numbers no-op).** `JsAddRef` on a tagged
  number is a no-op (and `JsRelease` likewise). Returning count
  must not be mistaken for a root `[plan §3.4 R2; §1.4]`.
  Consequence: for number/boolean/null/undefined singletons the
  scope/ref bookkeeping still runs (so counts balance), but the
  GC-safety argument is vacuous — these values are not
  collected. Probes MUST NOT treat a non-zero `JsAddRef` return
  on a number as proof of rooting; use the root-counter hook
  (§3.7) + GC-churn instead.
- **R-REF-11 (thread-context requirement, R3).** No
  `JsAddRef`/`JsRelease`, `napi_env`, `napi_value`, or
  `napi_ref` use from worker threads; rooting requires a thread
  context on the calling thread `[plan §3.4 R3]`. Worker threads
  touch only native data and the TSFN queue. Any transition
  attempted off-thread returns / asserts per R-ENV-11.
- **R-REF-12 (arena/silent-unrooted).** Arena/invalid objects
  silently succeed in `JsAddRef` **without rooting**
  `[plan §1.4]` `[mem §2: Rooting]`. JsRT offers no
  "is-rootable" query. The provider therefore MUST only ever
  `JsAddRef` values obtained from `JsCreate*` calls or JS
  execution (which are Recycler-heap values); it MUST NEVER
  `JsAddRef` a native pointer cast to `JsValueRef` (e.g. an
  external-data pointer) and count it as a root. Any test that
  stores a borrowed pointer through the ref path is a test bug.
  Residual risk (a Recycler value the engine routes through an
  arena) is unprovable by the API; mitigation is GC-churn
  stress (P3-2) + N5.4 campaigns asserting no collection while
  strongly referenced. **AMB-REF-2** records this gap: there is
  no status code for "root failed silently", so D7 cannot apply;
  detection is by GC observation only.
- **R-REF-13 (unbalanced release).** Because `JsRelease` does not
  validate balance `[plan §3.4 R2]`, every provider transition
  (scope close, ref 1→0, delete, destroy step 6) MUST go through
  the counted helper and the root counter (§3.7). Extra
  releases are invisible to JsRT; only the counter +
  use-after-free would reveal them. Root-counter probes cover
  the success path; failure-path counter assertions are in
  P3-6.

### 3.7 Root-counter test hook (design required by N1.2)

- **R-REF-14.** N1.2 requires a test-only root counter via the
  private API in debug builds `[task N1.2 Done when]`. Design
  (provider private API, never a `napi_*` symbol; unavailable or
  stubbed in release builds):
  - `size_t chakra_nodeapi_test_root_count(napi_env env)` —
    total outstanding rooted slots: scope entries + strong-ref
    roots + weak handles (R6) + wrap-record weak handles +
    deferred resolve/reject roots + callback-record argv roots.
  - `size_t chakra_nodeapi_test_root_baseline(napi_env env)` —
    the count recorded immediately after env creation (or 0
    before any scope opens); assertions compare equality, not
    absolute values, so the hook survives new root kinds.
  - Debug-only; the symbol MUST NOT be exported from release
    `libChakraNodeApi` (checked by the N1.6 exports inventory
    which forbids extras). Probes call it only under a
    `CHAKRA_NODEAPI_TEST_HOOKS` build flag, mirroring the N0.5
    host `gc()` injection pattern.
  - Each counted slot pairs with a named release point (scope
    close / ref 1→0-or-delete / wrap drain-or-destroy /
    deferred settle / callback pop). Probes assert the counter
    returns to baseline after every probe, including misuse
    probes (R-SC-6 mismatch path MUST still release).
  - `PLAN-GAP-6`: the plan mandates the hook but not its
    signature; this design is the contract N1.2 implements.
    Vendored tests do not cover it (private API); the probes
    are the evidence.

### 3.8 Probe list for N1.3

| Probe | Asserts |
|---|---|
| P3-1 | ref/unref cycles: 1000× 0↔1 transitions; exactly one `JsAddRef`/`JsRelease` + weak-handle create/release per cycle; counter returns to baseline |
| P3-2 | Weak from birth (count 0): value collectable after GC; `get_reference_value` ⇒ `napi_ok` + NULL; vendored `test_reference` agrees |
| P3-3 | Strong ref survives GC churn (`-CollectGarbage` equiv, ≥10 repeats per N5.4); weak child with same referent collects while strong holds it; after unref-to-0 + GC, weak deref ⇒ NULL |
| P3-4 | Delete-then-use (`get`/`ref`/`unref`) ⇒ `napi_invalid_arg` each |
| P3-5 | Double-delete ⇒ `napi_invalid_arg`; counter at baseline |
| P3-6 | Tagged-number ref: create strong ref on a number; counter consistent; release balances (no reliance on `JsAddRef` return count) |
| P3-7 | v8 type restriction: each disallowed type at create ⇒ expected status per N0.6 (see §6 row 4); counter unchanged (no partial root) |

### 3.9 Ambiguities (refs)

- **AMB-REF-1.** Unref-when-0 status (`napi_ok` vs
  `napi_invalid_arg`). Decided by N0.6 header docs + vendored
  `test_reference` (N5.1). Spec choice until then: `napi_ok`
  with no-op (weak stays weak) — do not crash, do not create a
  new handle.
- **AMB-REF-2.** Silent arena non-root (R-REF-12). No vendored
  test can observe it by status; detected only by GC
  collection under a strong ref in stress (P3-3/N5.4).
- **AMB-REF-3.** Whether `get_reference_value` on a collected
  weak ref sets the out-param to NULL vs leaves it untouched
  (N1.3 says "null result"; N0.6 MUST confirm the out-param
  contract from header docs). Probe P3-2 initialises the
  out-param to a sentinel and asserts NULL was written.

---

## 4. Wrap / finalizer lifecycle

### 4.1 Record shape

- **R-WRAP-1.** Every provider-created external/wrapped value
  allocates a heap `WrapRecord { env, user_data, napi_finalize,
  hint, type_tag?, weak_handle, exactly_once_flag }` owned by
  the provider allocator `[plan §3.7]`. The env-thread drain
  invokes `napi_finalize(env, data, hint)`. `JsCreateExternalObject`'s
  single data+callback slot always carries the provider
  trampoline + record pointer, never the user's directly
  (the callback is cached per (callback, prototype) and fires
  with no current context) `[plan §3.7; §1.4 audit]`.

### 4.2 wrap → inspect → collect → drain (timeline)

- **R-WRAP-2 (wrap).** `napi_wrap` (arbitrary objects) uses the
  per-env side table (§3.3 `wraps`) keyed by the object's
  `JsWeakRef` handle; `napi_create_external` uses
  `JsCreateExternalObject` with trampoline+record `[plan §3.7]`.
  Wrapping an already-wrapped object ⇒ `napi_invalid_arg`
  `[plan §3.7; task N3.3]`.
- **R-WRAP-3 (inspect).** `napi_unwrap` / `napi_get_value_external`
  look up the record by the ABA-guarded key (R-WRAP-9) and
  return the user pointer; unwrapped ⇒ `napi_invalid_arg`
  (N3.3). Inspection does not alter the exactly-once flag.
- **R-WRAP-4 (collect).** The GC-side trampoline does **only**:
  mark exactly-once, enqueue the record on the env's
  mutex-protected finalizer queue, drop native references. No JS
  calls, no user finalizer, no provider-container mutation
  beyond the queue (GC-thread safety, D4) `[plan §3.7]`. For
  wrapped arbitrary objects, records are removed from the side
  table by the before-collect trampoline at collection time
  `[plan §3.7]`.
- **R-WRAP-5 (drain).** Drained records invoke the user
  `napi_finalize(env, data, hint)` with a live env `[plan §3.7]`.
  During `Closing`, JS-calling APIs called from the finalizer
  return `napi_generic_failure` (R-ENV-4).

### 4.3 Exactly-once guarantees

- **R-WRAP-6.** Each `WrapRecord` invokes its user finalizer
  exactly once across all paths: GC-enqueued drain, destroy
  step 6 backstop, and external-buffer finalizers (N3.1). The
  `exactly_once_flag` is set atomically by the GC trampoline
  before enqueue and checked again at drain/destroy; the
  destroy backstop MUST skip already-enqueued records
  `[plan §3.7; §3.6 steps 2, 6]`. Probe P7-3 asserts count==1
  under GC churn + immediate destroy (the double-fire window).
- **R-WRAP-7 (thread constraint, D4).** User finalizers NEVER
  run inside a GC callback `[plan D4, §3.7]`. The trampoline
  runs on an arbitrary GC thread with no current context; it
  MUST NOT call JS, the user finalizer, or mutate containers
  beyond the queue `[plan §3.7]`. Probe P7-2 asserts via
  thread-id capture + reentrancy trap that the user finalizer
  runs on the env thread and never during GC.
- **R-WRAP-8 (drain points).** Env-thread drain points: on
  entry to any public API (cheap empty check), in the host pump,
  and in destroy steps 2/6 `[plan §3.7]`. N3.2 wires the
  entry-guard drain into N1.5's shared guard (`[task N3.2]`);
  all provider entries reuse it. A finalizer enqueued during a
  drain MUST be picked up by the next drain (or the same drain
  loop until empty) — spec requires drain-until-empty so a
  self-chaining finalizer cannot starve the host pump.
  `PLAN-GAP-7`: drain-until-empty vs one-per-entry is a spec
  choice; probe P7-5 asserts chained finalizers all complete
  before the host pump returns.

### 4.4 remove_wrap suppression

- **R-WRAP-9a (remove).** `napi_remove_wrap` returns the data and
  suppresses the finalizer: it clears the record's
  finalizer/flag so neither the GC path nor the destroy
  backstop will invoke it, and removes the side-table entry
  `[task N3.3; plan §3.7 record model]`. The record's allocation
  is still freed exactly once (by the remove itself, or
  deferred to drain if GC already enqueued — in which case the
  enqueued entry is neutralised and freed at drain without
  calling the user finalizer). Probe P7-6 asserts zero
  finalizer calls after remove + GC + destroy.

### 4.5 Type-tag rules

- **R-WRAP-10.** `napi_type_tag_object` stores the 16-byte tag
  in the record. Tag on an unwrapped object ⇒ check via
  `napi_check_object_type_tag` returns false (not an error).
  Double-tag (second `type_tag_object` on the same record) ⇒
  `napi_invalid_arg` `[task N3.3]`. Tags die with the record
  (release at drain/destroy/remove); a tag MUST NOT outlive its
  wrap. Probe P7-7.

### 4.6 ABA guard

- **R-WRAP-11.** The side table is keyed by the object's
  `JsWeakRef` handle, with pointer-key ABA guarded by validating
  `JsGetWeakReferenceValue(handle) == object` before trusting a
  lookup, and records removed by the before-collect trampoline
  at collection time `[plan §3.7]`. A new object reallocated at
  a collected object's address MUST NOT resolve to the dead
  record. If N3.3 evidence shows before-collect delivery is
  unreliable in this engine, stop and escalate: the fallback is
  a narrow JsRT weak-cell engine lane, not a heuristic
  `[plan §3.7]` `[§4.3 wrap gap row]`. Probe P7-8 forces
  address reuse via collect+recreate churn and asserts the new
  object unwraps to `napi_invalid_arg` (unwrapped), not the old
  pointer.

### 4.7 `napi_add_finalizer`

- **R-WRAP-12.** `napi_add_finalizer` = a record without wrap
  data semantics (no user pointer to unwrap), with an optional
  reference out-param per header docs `[task N3.3]`. Its
  exactly-once/thread/drain rules are identical (R-WRAP-6…8).
  N0.6 MUST confirm the reference out-param contract; until
  then the out-param is NULL-out if present. Probe P7-9.

### 4.8 Probe list for N3.2/N3.3

| Probe | Asserts | Card |
|---|---|---|
| P7-1 | External round-trip: `create_external` → `get_value_external` returns pointer | N3.2 |
| P7-2 | Finalizer runs exactly once, on env thread, with live env, after collection + next drain; never during GC (thread-id + reentrancy probe) | N3.2 |
| P7-3 | 10× create/collect + immediate destroy: still exactly once (backstop vs queue race) | N3.2, N3.3 |
| P7-4 | Reentrant drain: finalizer calls a public API; queue lock not held across user call; no deadlock | N3.2 |
| P7-5 | Chained finalizers (finalizer creates/collects another) all drained before host-pump return | N3.2 |
| P7-6 | `remove_wrap` suppresses finalizer across GC + destroy | N3.3 |
| P7-7 | Type-tag match / mismatch / double-tag (`invalid_arg`); tag on unwrapped ⇒ check false | N3.3 |
| P7-8 | ABA: collect wrapped object, recreate at same address, `unwrap` ⇒ `invalid_arg`, not stale pointer | N3.3 |
| P7-9 | `napi_add_finalizer` exactly-once + reference out-param | N3.3 |
| P7-10 | Teardown with live wrapped object: finalizer runs once at destroy step 6 (R-ENV-7) | N3.3 |
| P7-11 | 10k create/collect cycles leak nothing (allocator accounting) | N3.2 |

GC churn MUST use the host `-CollectGarbage`-equivalent hook
(N0.5) with ≥10 repeats for timing-sensitive cases `[task N3.2]`.

### 4.9 Ambiguities (wrap)

- **AMB-WRAP-1.** Whether `remove_wrap` on an already-collected
  (enqueued but not yet drained) record returns
  `napi_invalid_arg` or `napi_ok` (suppression of an in-flight
  finalizer). Spec choice: `napi_invalid_arg` (the wrap is
  already gone from the side table; only the queue entry
  remains). Decided by vendored wrap/factory tests (N5.1) +
  probe P7-6b. N0.6 MUST confirm.
- **AMB-WRAP-2.** Whether the before-collect trampoline is
  delivered reliably for arbitrary objects in this engine. This
  is the plan's flagged engine risk `[plan §3.7, §4.3]`; N3.3
  evidence (P7-8 churn) decides whether the side-table design
  stands or the stop-and-escalate lane is filed. No header-doc
  or vendored test alone decides it — the engine probe does.
- **AMB-WRAP-3.** External-buffer finalizer interplay: a
  detached external ArrayBuffer's finalizer is skipped by
  `JsrtExternalArrayBuffer::Finalize` (`[mem §5]`) while the
  provider's §3.7 record still owns user data. N3.1 MUST trace
  this transfer and probe detached-external finalization;
  vendored arraybuffer tests (N5.1) + probe cover it.

---

## 5. Callback records

### 5.1 Push / pop under nesting and exceptions

- **R-CB-1.** All N-API JS functions are created with
  `JsCreateEnhancedFunction` (D3); plain `JsCreateFunction` is
  never used for N-API callbacks `[plan D3, §3.8]`. The
  provider allocates a thunk record `{ napi_callback cb,
  void* data, napi_env env }` as the JsRT callback state,
  rooted for the function's lifetime via the function's
  external record — the record dies with the function's
  finalizer trampoline `[plan §3.8]` `[plan §3.7]`.
- **R-CB-2 (entry).** On invocation the thunk MUST:
  1. validate env/state, enter the env context;
  2. push `CallbackRecord { thisArg, argv (arguments+1),
     argc (argumentCount-1), newTarget, callee }` — Chakra's
     enhanced callback gives `arguments[0] == info->thisArg`
     and `argumentCount` includes `this` (oracle
     `bin/NativeTests/JsRTApiTest.cpp:734-866`) `[plan §3.8]`;
  3. open an implicit handle scope; call the user callback
     `[plan §3.8 step 3]`;
  4. on return: root the returned value into the caller's scope,
     close the implicit scope, pop the record, restore context
     `[plan §3.8 step 4]`. A pending JS exception after the
     callback propagates as the function's exception (status
     handling per §6) `[plan §3.8 step 4]`.
- **R-CB-3 (nesting).** The `callbacks` stack supports nesting:
  native→JS→native callbacks each push/pop their own record;
  inner callbacks MUST observe their own `thisArg`/`argv` and
  MUST NOT disturb the outer record `[plan §3.3 `callbacks`
  stack; task N2.4]`. Pop order is LIFO. Probe P8-2.
- **R-CB-4 (exception restore).** If the user callback throws
  (pending JS exception on return), the thunk MUST still pop
  its record, close the implicit scope (releasing its roots
  except the caller's-scope-rooted return), and restore the
  context, leaving the exception pending for the JS caller
  `[plan §3.8 step 4; §3.10]`. The callback stack depth after
  the throw MUST equal the depth before the call. Worked
  example (e) in Appendix C. Probe P8-3.

### 5.2 newTarget conversion

- **R-CB-5.** Non-construct invocation ⇒ C `NULL` for
  `napi_get_new_target` (Chakra passes `undefined` on
  `newTargetArg`; Node semantics want no-value). Construct
  invocation ⇒ `info->newTargetArg` (the explicit new target,
  e.g. from `Reflect.construct` — **not** assumed equal to the
  callee) `[plan §3.8 step 2]`. The provider MUST translate
  `undefined`→`NULL` at `napi_get_new_target` time, never
  store the `undefined` value as a target. Probe P8-4 covers
  both arms plus the `Reflect.construct` ≠ callee case.

### 5.3 Scope-balance check on return

- **R-CB-6.** On every return (success or throw) the thunk
  MUST verify scope/callback balance: the scope stack depth
  must equal entry depth + 1 (the implicit scope) and the
  callback stack depth must equal entry depth + 1, else
  imbalance is a provider bug: surface as
  `napi_handle_scope_mismatch` + cleanup (close the implicit
  scope, release its roots, pop the record), never a leak or
  crash `[plan §3.5, §3.8]`. Because the thunk is invoked
  through JsRT (not a `napi_*` entry), the mismatch signal is
  the pending-exception / abort path the thunk implements —
  N2.4 MUST record the exact surfacing (helper assert vs
  JS-visible error) in its report. Probe P8-5 (balance hook +
  fault-injection probe with a deliberately unbalanced
  callback via test hook).

### 5.4 Probe list for N2.4

| Probe | Asserts | Card |
|---|---|---|
| P8-1 | Pass-arguments: `argc`/`argv`/`this`/`data` correct (arguments[0]==this layout) | N2.4 |
| P8-2 | Nested native→JS→native: records push/pop LIFO, inner `thisArg` independent, outer restored | N2.4 |
| P8-3 | Callback throws ⇒ pending exception at JS caller; stack/scope depths restored; caller can catch and continue | N2.4 |
| P8-4 | `new.target`: non-construct ⇒ `NULL` from `napi_get_new_target`; `Reflect.construct` with explicit target ≠ callee returns that target | N2.4 |
| P8-5 | Balance verification: balanced callbacks pass; injected imbalance ⇒ `handle_scope_mismatch` + cleanup, no leak (root counter) | N2.4 |
| P8-6 | Returned value rooted in caller's scope (R-SC-9): visible from JS after thunk returns, collectable after caller scope closes | N2.4 |
| P8-7 | `napi_create_function` `length` is the UTF-8 **name** length (not arity); `Function.length === 0` (V8 default) and `Function.name` truncated per length 0..5 (vendored `test_function`: `"Name_extra",5` ⇒ `name === 'Name_'`; post-create define to 0) | N2.4 |

### 5.5 Ambiguities (callbacks)

- **AMB-CB-1.** Exact `thisArg`/`newTarget` identity when the
  callback is invoked via `napi_call_function` / `JsCallFunction`
  vs directly from JS. Plan §3.8 oracle is the JsRT test at
  `JsRTApiTest.cpp:734-866`; N2.4 MUST extend P8-1/P8-4 to
  both call paths and verify against vendored function tests
  (N5.1). Any divergence between the two paths is a provider
  bug, not a spec freedom.
- **AMB-CB-2.** Whether a throwing callback's return value
  (if the C callback returns a value *and* leaves an exception
  pending) is rooted in the caller's scope or discarded.
  Spec choice: discarded (exception wins; §3.8 step 4 roots the
  return only on the non-exception path). N0.6 MUST confirm
  from header docs; probe P8-3b asserts the discarded value is
  not observable from JS.

---

## 6. Status mapping — five functions end to end

Per-entry discipline: validate args (`napi_invalid_arg`) →
check pending exception (most APIs return
`napi_pending_exception` without executing; exceptions per the
vendored header docs) → pre-check types for the specific
`*_expected` status → call JsRT → map `JsErrorCode` `[plan
§3.10]`. Throw helpers set the pending exception via
`JsSetException` and return `napi_ok` when no exception was
already pending (if pending, return `napi_pending_exception`
and leave it) `[plan §3.10]`. `napi_get_last_error_info`
returns the per-env record of the most recent failed call and
is not cleared spuriously by success (verify per N1.5/N0.6).

The table `[plan §3.10]` is normative:

| JsRT outcome | napi_status | Notes |
|---|---|---|
| `JsNoError` | `napi_ok` | |
| `JsErrorInvalidArgument`, `JsErrorNullArgument` | `napi_invalid_arg` | only after provider pre-checks produce a more specific code |
| Type mismatch by pre-check | `napi_object_expected` / `string_expected` / `number_expected` / `boolean_expected` / `array_expected` / `function_expected` / `bigint_expected` / `date_expected` / `arraybuffer_expected` as applicable | provider-produced, never by mapping JsRT's collapsed invalid-arg |
| `JsErrorInExceptionState`, `JsErrorScriptException` | `napi_pending_exception` | exception stays pending; never cleared implicitly |
| `JsErrorScriptCompile` (from `JsRun`) | `napi_pending_exception` | compile error is the pending exception |
| `JsErrorOutOfMemory` | `napi_generic_failure` + last-error message | do not fabricate a JS exception |
| anything else | `napi_generic_failure` | message recorded for `napi_get_last_error_info` |

Five representative functions, each traced success + failure.
Rows marked `†N0.6` MUST be confirmed by N0.6 from vendored
header docs; until confirmed, the probe asserts the stated
choice and the confirm task amends the line if docs disagree.

### 6.1 `napi_create_double(env, value, &result)` — values lane (N2.1)

- *Success.* Validate (`env`, `result` non-NULL) → no pending
  check blocks creation (N0.6 confirms: creation functions
  execute even with a pending exception or return
  `pending_exception`? `†N0.6-B1`) → no type pre-check
  (input is a C double) → `JsCreateDouble` → `JsNoError` ⇒
  `napi_ok`; result rooted into innermost scope (R-SC-1).
- *Failure (NULL out-param).* Validate fails (`result==NULL`)
  ⇒ `napi_invalid_arg` immediately, no JsRT call, `last_error`
  recorded `[plan §3.10]`.
- *Failure (OOM).* `JsCreateDouble` ⇒ `JsErrorOutOfMemory` ⇒
  `napi_generic_failure` + last-error message; no JS exception
  fabricated `[plan §3.10]`.
- *Pending-exception interaction.* `†N0.6-B1`: exact gate.
  Spec choice: creation functions are allowlist-gated like most
  APIs (return `napi_pending_exception` without executing)
  unless header docs name them callable-with-pending. N1.5
  probe P5-2b asserts whichever is confirmed.
- *Probes:* P6-1a (success + scope-rooted), P6-1b (NULL
  ⇒ `invalid_arg`), P6-1c (OOM-simulated ⇒ `generic_failure`,
  no pending exception).

### 6.2 `napi_get_value_int32(env, value, &result)` — pre-check lane (N2.1)

- *Success.* Validate → pending check (`†N0.6-B2`) → provider
  pre-check: if `value` is not a Number, return
  `napi_number_expected` (never blanket `napi_invalid_arg`)
  `[plan §3.10, PR-12]` → JsRT double-read + documented
  truncation/lossy rule (from header docs, not JsRT `ToInt32`)
  `[task N2.1]` ⇒ `napi_ok`.
- *Failure (non-number, e.g. a string).* Pre-check returns
  `napi_number_expected`; JsRT is never called, so JsRT's
  collapsed `JsErrorInvalidArgument` never has a chance to
  produce the wrong code `[plan §3.10]`. `†N0.6-B2` confirms
  the exact expected-code name from header docs.
- *Failure (OOM / other JsRT error).* Map per table
  (`generic_failure` + message; `InExceptionState`/`ScriptException`
  ⇒ `pending_exception`).
- *Probes:* P6-2a (±2^53 edges, NaN, ±Infinity per N2.1),
  P6-2b (string ⇒ `number_expected`), P6-2c (boolean ⇒
  `number_expected`, proving no JsRT collapse).

### 6.3 `napi_wrap(env, object, data, finalize, hint, &result)` — wrap lane (N3.3)

- *Success.* Validate (`object`/`result` non-NULL) → pending
  check (`†N0.6-B3`) → pre-checks (object is an object;
  already-wrapped ⇒ `napi_invalid_arg`) `[plan §3.7; task N3.3]`
  → allocate `WrapRecord`, side-table insert via weak handle,
  create external/thunk per §4 ⇒ `napi_ok`; result rooted
  (R-SC-1).
- *Failure (double wrap).* Pre-check finds a live record via
  the ABA-guarded lookup ⇒ `napi_invalid_arg`; no new record
  allocated; root counter unchanged. `[plan §3.7]`
- *Failure (non-object).* ⇒ `napi_object_expected` (pre-check)
  `[plan §3.10]`. `†N0.6-B3` confirms the code name.
- *Pending-exception interaction.* `†N0.6-B3`: whether `wrap`
  is gated like most APIs. Spec choice: gated. Probe P5-2
  asserts gate behaviour with a pending exception (wrap MUST
  NOT run, MUST return `pending_exception`, MUST leave the
  exception untouched and allocate nothing).
- *Probes:* P7-1/P7-8 family + P5-2 (gate row).

### 6.4 `napi_delete_reference(env, ref)` — allowlisted lane (N1.3)

- *Success.* Validate (`ref` non-NULL) → **pending check
  skipped**: ref delete is an allowlisted callable-with-pending
  entry; it executes even with an exception pending `[plan
  §3.5; §3.10]`. `†N0.6-B4` MUST confirm allowlist membership
  from header docs (this row is the canonical allowlist probe).
  → release strong root or weak handle exactly once (R-REF-8)
  ⇒ `napi_ok`.
- *Failure (NULL ref).* Validate ⇒ `napi_invalid_arg`.
- *Failure (already deleted).* Poisoned handle ⇒
  `napi_invalid_arg` (R-REF-9); no double-release.
- *JsRT failure.* `JsRelease` on a valid handle should not
  fail; if it reports `InExceptionState`, the allowlist wins:
  the delete already released, and the entry returns
  `napi_ok` (the delete happened) while leaving the exception
  pending — `†N0.6-B4` MUST confirm this precedence. Until
  then the probe asserts `napi_ok` + exception still pending.
- *Probes:* P3-4/P3-5 + P5-3 (delete with pending exception
  executes).

### 6.5 `napi_call_function(env, recv, func, argc, argv, &result)` — callback/pending lane (N2.4)

- *Success.* Validate → pending check (gated: with an exception
  already pending, return `napi_pending_exception` without
  calling) — `†N0.6-B5` → pre-checks (function-typed `func` ⇒
  else `napi_function_expected`; object-typed `recv`) `[plan
  §3.10]` → `JsCallFunction`/`JsCallFunctionWithCallback`
  translation (argv convention: `this = argv`, arguments
  shifted per §5 oracle) `[task N2.4]` ⇒ `JsNoError` →
  `napi_ok`; result rooted into caller's scope (R-SC-9).
- *Failure (callee throws).* JsRT reports
  `JsErrorScriptException` (or `InExceptionState` on reentry)
  ⇒ `napi_pending_exception`; the thrown value stays pending
  and is **never cleared implicitly** `[plan §3.10]`. This is
  the thunk's step-4 propagation (§5).
- *Failure (non-function).* Pre-check ⇒
  `napi_function_expected`, JsRT never called `[plan §3.10]`.
- *Failure (pending at entry).* Gated ⇒
  `napi_pending_exception` without calling; the pending
  exception is preserved. `†N0.6-B5` confirms gating vs
  allowlist.
- *Probes:* P8-1/P8-3 (throw propagation) + P6-3 (non-function
  ⇒ `function_expected`) + P5-2 (gate row).

### 6.6 N0.6 confirmation checklist (binds §6 to the allowlist)

| Row | Must confirm from header docs | If docs disagree |
|---|---|---|
| `†N0.6-B1` | `napi_create_double` pending gate | Amend §6.1 + probe P5-2b |
| `†N0.6-B2` | `napi_get_value_int32` pending gate + exact expected-code name | Amend §6.2 + probes P6-2b/c |
| `†N0.6-B3` | `napi_wrap` pending gate + double-wrap / non-object codes | Amend §6.3 + probe P5-2 |
| `†N0.6-B4` | `napi_delete_reference` allowlist membership + precedence over `InExceptionState` | Amend §6.4 + probe P5-3 |
| `†N0.6-B5` | `napi_call_function` pending gate + expected-code names | Amend §6.5 + probes P6-3/P5-2 |

**AMB-STATUS-1.** Whether successful calls clear `last_error`
or leave it until the next failure. Plan §3.10 says
`napi_get_last_error_info` returns the most recent *failed*
call; N1.5 Done-when requires verifying "next successful call
does not clear it spuriously" against Node semantics. Spec
choice: success leaves `last_error` untouched. Decided by N0.6
header docs + probe P5-4.

---

## 7. Cross-reference index

Each rule group → implementing task card(s) + proving probe(s).
A task MUST NOT be marked done until its probes are green.

| Rules | Task card(s) | Probe(s) | Gate |
|---|---|---|---|
| R-ENV-1…6, 12, 13 | N1.1 | P1-3…P1-6 | G1 |
| R-ENV-7, 8, 10 | N1.1 + N1.4 | P1-1, P1-4, P1-4b, P1-7 | G1 |
| R-ENV-9 | N1.1 + N1.4 | P1-2 | G1 |
| R-SC-1…8, 10 | N1.2 (+ N2.4 for thunk half) | P2-1…P2-7, P8-5 | G1 / G2 |
| R-SC-9 | N2.4 | P8-6 | G2 |
| R-REF-1…9, 13, 14 | N1.3 (+ N1.2 for hook) | P3-1…P3-7, P2-* counter assertions | G1 |
| R-REF-10…12 | N1.3 | P3-2, P3-6, P3-3 stress | G1 / G3 |
| R-WRAP-1…5, 8 | N3.2 (+ N1.5 guard wiring) | P7-1…P7-5, P7-11 | G3 |
| R-WRAP-6, 7, 9a…12 | N3.3 | P7-3, P7-6…P7-10 | G3 |
| R-CB-1…4, 6 | N2.4 | P8-1…P8-3, P8-5…P8-7 | G2 |
| R-CB-5 | N2.4 | P8-4 | G2 |
| §6.1 `napi_create_double` | N2.1 (+ N1.5 guard) | P6-1a/b/c, P5-2b | G1/G2 |
| §6.2 `napi_get_value_int32` | N2.1 | P6-2a/b/c | G2 |
| §6.3 `napi_wrap` | N3.3 | P7-1/P7-8, P5-2 | G3 |
| §6.4 `napi_delete_reference` | N1.3 | P3-4/P3-5, P5-3 | G1 |
| §6.5 `napi_call_function` | N2.4 | P8-1/P8-3, P6-3, P5-2 | G2 |
| `†N0.6-B1…B5`, AMB-STATUS-1 | N0.6 (then N1.5) | P5-2, P5-2b, P5-3, P5-4 + addendum probes | G0/G1 |
| R1–R6 enforcement (all) | N1.5 guard + N5.4 stress | root-counter assertions in every probe; N5.4 campaigns | G1…G5 |

File ownership per `[plan §5.2]`:

| Area | Files |
|---|---|
| Env core | `lib/NodeApi/NodeApiEnv.*` (N1.1) |
| Scopes | `lib/NodeApi/NodeApiScopes.*` (N1.2) |
| Refs | `lib/NodeApi/NodeApiRefs.*` (N1.3) |
| Instance/cleanup | `lib/NodeApi/NodeApiLifetime.*` (N1.4) |
| Guard/status | `lib/NodeApi/NodeApiErrors.*` (N1.5) |
| Values/objects/functions | `NodeApiValues.*`, `NodeApiObjects.*`, `NodeApiFunctions.*`, `NodeApiErrorsImpl.*` (N2.x) |
| Externals/wrap queue + wrap | `lib/NodeApi/NodeApiWrap.*` (N3.2/N3.3) |
| Probes | `test/nodeapi/probes/scopes.*`, `refs.*`, `finalizer.*`, `teardown.*`, `callbacks.*`, `status.*` (via `run_matrix.py`) |

---

## Appendix A. Ownership records (memory-management §3)

Per N0.4, this doc *is* the ownership record for the kernel.
Records follow `[mem §3]`; domains from `[mem §2]`.

### A.1 Scope root lists

```text
Resource: Scope { roots: list<JsValueRef>, escapable, escaped } (NodeEnv.scopes stack).
Domain/allocator: provider native heap (Zig allocator instance owned by NodeEnv; backing lifetime == env lifetime).
Owner: NodeEnv stack entry; transfer: value ownership moves from "API return" to innermost scope at creation; escape moves inner→parent.
Release: scope close (LIFO or mismatch-with-release R-SC-6) → JsRelease each root; destroy step 6 closes all.
Invalidation: scope close, env Closing (R-ENV-4), runtime disposal (R5).
Thread/context: env thread + entered context only (R3/R-ENV-11).
Reentrancy: closing while a callback holds a value from the scope is prevented by the thunk's implicit-scope discipline (§5).
Accounting/capacity: counted in root counter (R-REF-14); unbounded list depth bounded by JS call depth.
Uncertainty: none beyond AMB-SC-1/2 (status names only).
Test: P2-* counter assertions + P2-3 GC collection.
```

### A.2 Refs (strong roots + weak handles)

```text
Resource: Ref { provider_count, mode: strong|weak, strong_root?: JsValueRef, weak_handle?: JsWeakRef }.
Domain/allocator: provider native heap.
Owner: NodeEnv.refs map; release authority: R-REF-1 helper (0↔1 transitions only) + R-REF-8 delete + destroy step 6.
Transfer: napi_create_reference creates; ref/unref transition; delete frees.
Invalidation: referent GC (weak → empty), delete, destroy step 6, runtime disposal (R5).
Thread/context: env thread + entered context only.
Cycle risk: a Ref holding a value that holds a Ref is legal; delete/destroy breaks it explicitly — no deferred sweep.
Accounting: strong root + weak handle (R6) both counted (R-REF-14).
Test: P3-* (cycles, collection, delete-then-use/double-delete).
```

### A.3 Wrap records (incl. externals)

```text
Resource: WrapRecord { env, user_data, finalize, hint, type_tag?, weak_handle, exactly_once }.
Domain/allocator: provider native heap; data pointer itself is user-owned (provider only invokes finalize).
Owner: side table (N3.3) or external-object slot (N3.2) until GC trampoline enqueues; finalizer queue until drain; destroy step 6 backstop for live records.
Transfer: wrap/create → side table; GC before-collect → queue (mutex only); drain → user finalize → free.
Invalidation: collection, remove_wrap (suppress + free), destroy step 6, detach path for external buffers (AMB-WRAP-3).
Thread/context: GC side — queue enqueue only, no JS/finalizer/container mutation (D4/R-WRAP-7); env thread — drain/destroy destroy-side free.
Reentrancy: drain never holds the queue lock across user code (R-ENV-12).
Accounting: record allocation + weak handle counted (R-REF-14); 10k-cycle leak probe P7-11.
Test: P7-* incl. thread-id/reentrancy (P7-2/4), ABA (P7-8), exactly-once under churn (P7-3).
```

### A.4 Env itself

```text
Resource: NodeEnv.
Domain/allocator: provider native heap; allocation == chakra_nodeapi_create_env, release == destroy step 7.
Owner: host (create/destroy calls) / provider (contents). Context/runtime remain host-owned; provider holds borrows.
Invalidation: destroy (steps 1–7); runtime disposal only after step 7.
Thread: owner thread (R-ENV-6/11).
Test: P1-6 (1000× churn), P1-1 ordering.
```

---

## Appendix B. Ambiguity register (summary)

| Flag | Section | Question | Deciding evidence |
|---|---|---|---|
| `AMB-ENV-1` | 1 | Step 6 internal order (refs/scopes vs wrap backstop) | Vendored `test_finalizer` (N5.1) + P1-1 |
| `AMB-ENV-2` | 1 | Async-hook completion order | Vendored `test_async_cleanup_hook` (N5.1) |
| `PLAN-GAP-1..4` | 1 | Void-host-fn fallbacks, timeout values | Local probes P1-2/3/4b/5 (private API; no vendored cover) |
| `AMB-SC-1` | 2 | `escape` on non-escapable scope status | N0.6 header docs + vendored handle-scope tests (N5.1) |
| `AMB-SC-2` | 2 | Close-escapable-on-plain status | N0.6 header docs + vendored handle-scope tests (N5.1) |
| `AMB-SC-3` | 2 | Use-after-close status (UB vs `invalid_arg`) | Vendored scope tests (N5.1) demanding validation |
| `AMB-REF-1` | 3 | Unref-when-0 status | N0.6 header docs + vendored `test_reference` (N5.1) |
| `AMB-REF-2` | 3 | Silent arena non-root | GC observation under strong ref (P3-3 / N5.4 stress) |
| `AMB-REF-3` | 3 | Collected-weak out-param contract | N0.6 header docs + P3-2 sentinel assertion |
| `AMB-WRAP-1` | 4 | `remove_wrap` on enqueued-but-undrained | Vendored wrap/factory tests (N5.1) + P7-6b; N0.6 |
| `AMB-WRAP-2` | 4 | Before-collect delivery reliability | Engine probe P7-8 churn; fallback = JsRT weak-cell lane |
| `AMB-WRAP-3` | 4 | Detached external-buffer finalizer skip | Vendored arraybuffer tests (N5.1) + N3.1 probe |
| `AMB-CB-1` | 5 | `this`/`newTarget` consistency across call paths | `JsRTApiTest.cpp:734-866` oracle + vendored function tests (N5.1) |
| `AMB-CB-2` | 5 | Throw-with-return-value handling | N0.6 header docs + P8-3b |
| `†N0.6-B1…B5` | 6 | Five functions' pending gates + expected codes | N0.6 header-doc extraction (binds §6) |
| `AMB-STATUS-1` | 6 | Success clearing `last_error` | N0.6 header docs + P5-4 |

---

## Appendix C. N0.4 reviewer answers (a)–(e)

Each answer cites the implementing task and the probe.

**(a) Escape-twice.** First `napi_escape_handle` on the innermost
escapable scope returns `napi_ok` and moves the root to the
parent (R-SC-3/4). A second call on the same scope returns
`napi_escape_called_twice` and moves nothing; the first escape
stands `[plan §3.5]` (R-SC-4). Implemented by N1.2; proved by
P2-4 (status + root-counter no-double-move assertion).

**(b) Close-out-of-order.** `napi_close_handle_scope` on a
scope that is not the innermost returns
`napi_handle_scope_mismatch` and STILL releases the named
scope's roots; the remaining stack is intact for subsequent
correct-order closes `[plan §3.5]` (R-SC-6). Implemented by
N1.2; proved by P2-5 (mismatch status + partial-release
counter assertion + subsequent orderly closes).

**(c) Wrap → collect → drain.** `napi_wrap` allocates a
`WrapRecord` and registers it in the side table (or, for
externals, in the `JsCreateExternalObject` slot with
trampoline+record) (R-WRAP-1/2). The GC trampoline only marks
exactly-once and enqueues — no JS, no user finalizer (D4)
(R-WRAP-4). The env thread drains at entry/pump/destroy and
invokes `napi_finalize(env, data, hint)` exactly once with a
live env (R-WRAP-6/8). Implemented by N3.2/N3.3; proved by
P7-2 (thread-id proof) + P7-3 (exactly-once under churn +
immediate destroy).

**(d) Destroy with a live wrapped object.** The record's
finalizer runs exactly once at destroy step 6 after refs and
scopes are torn down, even though the object is still
reachable from JS; the object handle is host-dead afterwards
and no further API use is legal (R-ENV-7/10). Implemented by
N1.4 teardown wiring + N3.3 backstop; proved by P7-10 +
P1-1 ordering log.

**(e) Nested callback throwing.** Each callback pushes its own
`CallbackRecord` + implicit scope (R-CB-2/3). An inner throw
leaves its record popped, its scope closed, its context
restored, and the exception pending at the JS caller
(R-CB-4); the outer record/scope remain intact on the stack.
Implemented by N2.4; proved by P8-2 (stack restoration) +
P8-3 (pending exception catchable in JS, depths restored).

---

## Appendix D. Review sign-off

- [ ] N0.4 reviewer: answers (a)–(e) above verified against
      plan §3.3–§3.10 and task cards. Date: ______.
- [ ] N0.6: `†N0.6-B1…B5` + AMB-SC-1/2, AMB-REF-1/3,
      AMB-WRAP-1, AMB-CB-2, AMB-STATUS-1 confirmed from header
      docs; this doc amended where they disagreed. Date:
      ______.
- [ ] N0.5: N0.5 manifest pins the exact vendored asset paths
      cited as `test_finalizer`, `test_cleanup_hook`,
      `test_async_cleanup_hook`, `test_reference`,
      `test_buffer`, and the handle-scope / function /
      wrap-factory / arraybuffer / promise assets. Date:
      ______.

Until the boxes are ticked, implementers MUST treat the
flagged lines as provisional and MUST NOT silence them by
choosing a different status without amending this doc.
