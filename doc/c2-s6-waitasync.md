# C2-S6b: Atomics.pause and Atomics.waitAsync (flag-on slice)

Status: **landed** — merged as `eef3b93c5`; review fixes `5fa9524e2`;
integration/gate commit `92e419ffd`. Final Mac ARM64 no-JIT gates are
**2021/0 test** and **2073/0 debug-static**. `ESSharedArrayBuffer` remains
**off by default**: default probes still print `SAB=undefined
Atomics=undefined`. This receipt does not propose a default change and does
not claim multi-agent waitAsync suspension.

## Scope

1. `Atomics.pause()` is installed as a 0-length, non-constructor built-in with
   name `pause` and `{writable:true, enumerable:false, configurable:true}`. It
   performs no argument validation/coercion, performs an architecture
   spin-wait hint, returns `undefined`, and never touches a SAB.
2. `Atomics.waitAsync(ta, index, value, timeout)` returns an ordinary result
   object with own data properties `async` then `value`:
   - shared `Int32Array` only (`requireShared`, waitable Int32);
   - `ValidateAtomicAccess` captures `TypedArrayLength` **before** `ToIndex`;
   - `value` is `ToInt32`-coerced, then `timeout` is `ToNumber`-coerced;
   - `NaN` and `+Infinity` mean an infinite wait, `-Infinity` and any
     non-positive finite value mean an immediate timeout, and any other finite
     value is a finite wait;
   - equal witness + zero/negative timeout returns
     `{async:false, value:"timed-out"}`; unequal witness returns
     `{async:false, value:"not-equal"}` even when the timeout is infinite;
   - the async path returns `{async:true, value:%Promise%}` using the engine
     promise capability machinery;
   - missing arguments are legal short calls: missing `index`/`value` default
     to `0`, missing `timeout` to infinity, while a missing/null/undefined
     typed array throws `TypeError` (not the old arity `RangeError`).
3. `Atomics.notify` resolves a registered async waiter to `"ok"` after
   releasing the waiter-list critical section. It wakes same-thread waiters
   across realms; `count` uses `ToIntegerOrInfinity` and clamps above
   `INT_MAX` (including `+Infinity`) to "wake all", while `NaN` is `+0`,
   negative/`-Infinity` are zero, and `2^31`, `2^32+1`, `MAX_VALUE` and
   `+Infinity` wake all registered waiters. Woken promises resolve in FIFO
   waiter-list order.
4. `Atomics.wait` is unchanged: still shared-only, and still a non-suspending
   no-op on non-Windows (`AddAndSuspendWaiter` returns false). S6a non-shared
   RMW/wait/notify behavior is unchanged.

Out of scope: finite positive async timeout scheduling, cross-agent (different
`ThreadContext`) async suspension/resolution, BigInt waitAsync, and any default
enablement.

## Design

Property installation stays inside the existing
`IsESSharedArrayBufferEnabled` block. `pause` and `waitAsync` are installed
with dynamic tracked property names, so no new static `PropertyId` is added.
Their `FunctionInfo`s are appended at the end of
`JavascriptBuiltInFunctionList.h`; no pre-existing `BuiltInEnum` value shifts.
The focused test is registered once in `test/es7/rlexe.xml`; integration
therefore moves the single-source gate pins by one.

`DoWait(async)` ordering is implemented explicitly in `EntryWaitAsync`:
type/shared validation → length capture + index coercion/range check → value
coercion → timeout coercion → waiter-list lookup → lock → witness compare →
fast result or registration. Timeout is coerced before the witness compare,
so a poisoned timeout throws even on a would-be `not-equal` path. Missing
arguments are defaulted before validation, so `waitAsync()` and
`waitAsync(null)` throw `TypeError`, while `waitAsync(ta)`,
`waitAsync(ta,0)` and `waitAsync(ta,0,0)` register an infinite wait on index 0
with expected value 0.

`Atomics.notify` converts `count` with `JavascriptConversion::ToInteger`
(the engine's ToIntegerOrInfinity: NaN → +0, ±∞ preserved, truncation toward
zero) and then clamps to `[0, INT_MAX]` before the int32 waiter-list API. This
avoids the old `ToInt32` wrap where `2^31` became `INT_MIN` and woke nobody.
`NaN` deliberately remains `0` per current test262 (`count-from-nans`,
`notify-nan`), not "wake all".

The result object uses `DefineOwnPropertyHelper` with a data descriptor, not
`[[Set]]`; an inherited `Object.prototype.async` setter cannot run and cannot
replace the own `async`/`value` slots.

## Ownership and threading record

```text
Resource: AtomicsAsyncWaiter record (pending async wait) + native
        AtomicsAsyncWaiterRegistration token (owned by the wrapper)
Creation: RecyclerNew record in AtomicsObject::EntryWaitAsync; HeapNew token in
        JavascriptSharedArrayBuffer::AddAsyncWaiter (JS thread)
Domain: record = Recycler normal heap (conservatively scanned); token = native
        HeapAllocator, deleted with HeapDelete
Fields: record holds capability, owner wrapper, wrapper-list next, ScriptContext,
        settled; token holds record pointer, WaiterList*, cached ScriptContext*,
        cached opaque ThreadContext*, wrapper-link, notify-link, linked flag
Owner/root: JavascriptSharedArrayBuffer::asyncWaiters roots the record; the
        wrapper also owns the native token list. The SharedContents WaiterList
        stores only the native token, never an owning GC reference.
Transfer: registration adds the record to the wrapper GC list and then (still
        under the WaiterList lock) adds the token to the waiter list; notify
        removes the token from the waiter list and hands it to the caller.
Invalidation: notify on the same ThreadContext resolves then releases the token
        and record-list entry; SAB finalize walks the native token list (never
        the record) and unlinks tokens without running promise code; a notify
        from a different ThreadContext leaves the token/record registered and
        does not count or resolve it.
Thread/context: all mutations of the record and its wrapper lists happen on the
        registrant's ThreadContext. The skip decision on a foreign notifying
        thread compares only the token's cached ThreadContext pointer; it never
        dereferences the record. Same-thread waiters in another realm are
        resolved through the record's own ScriptContext.
Release: notify outside the critical section; successful resolve releases the
        wrapper-list reference and token. Settled records are collectible.
Finalizer safety: an unmarked wrapper is not scanned by the collector, so its
        pending records may already have been swept before its finalizer runs;
        the finalizer therefore dereferences only native token fields, never an
        AtomicsAsyncWaiter pointer.
Reentrancy: promise resolution happens after leaving the critical section and
        only resolves a string; record linking does not run user code.
Known finalizer risk: CancelAsyncWaiters takes the same WaiterList critical
        section a foreign agent may hold. A safe lock-free unlink is not bounded
        in this slice; the finalizer only removes native list nodes and holds no
        other lock, but a stop-the-world GC that suspends a thread while it
        holds csForAccess could stall. Documented, not claimed safe under
        concurrent cross-agent waits.
Tests: s6-waitasync-pause.js (forced defer parse + CollectGarbage) covers
        pause, not-equal, zero/negative/-Infinity/NaN timeouts, short calls,
        2^31/2^32+1/MAX_VALUE/+Infinity/NaN/negative notify counts, same-thread
        cross-realm wakeup, FIFO order, GC-before-notify, non-mutation and
        double-notify; a 200-waiter/200-notify stress probe resolves all
        promises. The wrapper-dies-first path is structurally handled by the
        native token but was not exercised (SAB serialization crashes in this
        build).
```

The finite positive timeout path has **no core timer job** in this embedding.
The record is registered and the promise stays pending until notify; this is
explicitly not claimed as a completed timeout. No background thread resolves
engine promises.

## Evidence

Mac ARM64, test profile (RelWithDebInfo, no JIT), shared gate lock
(`sh /tmp/opencode/cc-gate-lock.sh`), local `.zig-cache`/`out`; no `--prepare`
and no full corpus/check.

- Lane builds: `zig build --summary line -Dprofile=test` **568/568** and
  `-Dprofile=debug-static` **546/546**. After integration the authoritative
  `zig build check` is **583/583**, with **2021/0 test** and **2073/0
  debug-static** regression gates.
- New `test/es7/s6-waitasync-pause.js`, registered once in
  `test/es7/rlexe.xml` with
  `-ESSharedArrayBuffer -Test262 -CollectGarbage -force:deferparse -args summary`:
  base binary **exit 1 / 0 passes** (`Atomics.pause` absent); after
  **pass** on both test and debug-static binaries. Covers pause shape/descriptor/
  non-constructor/no mutation, waitAsync shape/descriptors, not-equal,
  zero/negative/−Infinity/false/null timeouts, fractional and missing timeout
  classification, validation/coercion order, length-before-index (growable SAB),
  detached view, inherited-setter avoidance, legal short calls with index/value
  defaults, 2^31/2^32+1/MAX_VALUE/+Infinity/NaN/negative notify counts,
  same-thread cross-realm wakeup and no double resolution, FIFO `.then` order,
  GC before notify, single-agent notify → `"ok"`, double-notify → 0, and no SAB
  mutation.
- Default-off probe: `./zig-out/bin/ch probe.js` prints
  `SAB=undefined Atomics=undefined`. Flag-on prints `SAB=function
  Atomics=object`, `pause=function`, `waitAsync=function`.
- Pinned test262 `2e0a56762801e275a9fdf96dc49d90ba0cddcf63`, 20 controls
  passed, host flags `-ESSharedArrayBuffer -nonative -Test262 -CollectGarbage`,
  Node 26.5.0 runner:
  - `test/built-ins/Atomics/waitAsync/*.js` + `Atomics/pause/**/*.js`
    (57 + 5 files, **124 scenarios**): base **0/124 → 80/124 after**,
    **80 fail→pass / 0 pass→fail**. Breakdown: pause **0/10 → 10/10**;
    non-BigInt waitAsync **0/114 → 70/114**.
  - Exact `Atomics/waitAsync/**/*.js` (101 files, **202 scenarios**, includes
    BigInt): **0/202 → 72/202**, **72 fail→pass / 0 pass→fail**; non-BigInt
    subset **70/114**, BigInt subset **2/88**. Re-run on the merged binary:
    **80/124** (`out/test262-final-s6b`) and **72/202**
    (`out/test262-final-waitasync`), 0 pass→fail.
  - Adjacent wait/notify selection `Atomics/wait/*.js` +
    `Atomics/notify/*.js` (95 files, 190 scenarios): **138/190 before →
    138/190 after, 0 fail→pass / 0 pass→fail**.
  - S6a adjacent: `s6-growable-nonshared.js` pass (plain and
    `-CollectGarbage`), `atomics_test.js` pass, `sharedarraybuffer.js` pass.
- Differential `/tmp/opencode/s6b-diff.js` (Node **v26.5.0**, Bun **1.4.3**,
  this binary) reports identical
  `pause=function 0 pause undefined`, `not-equal=false not-equal`,
  `zero=false timed-out`, `negative=timed-out`, `neg-inf=timed-out`,
  `inf=true true true`, `notify=1 0`, `resolved=ok`.

### Residual classification of the 44 non-BigInt waitAsync failures

- **4 finite-timeout scenarios**: `returns-result-object-value-is-promise-
  resolves-to-timed-out.js` and `true-for-timeout.js` (2 each) expect a
  positive 1 ms async timeout to resolve `"timed-out"`. This core has no
  timer-job facility; Node 26.5.0 and Bun 1.4.3 resolve, this binary leaves the
  promise pending. Differential probe: `finite-1ms=timed-out` (Node/Bun) vs
  `finite-1ms=pending` (Chakra).
- **40 multi-agent scenarios** (20 files × default/strict): agent tests await
  `Atomics.waitAsync` in `$262.agent` crossthread workers. This slice does not
  register/resolve waiters from another `ThreadContext`, and the crossthread
  host does not drain async-function promise jobs even for `await 1` (verified
  directly); the harness therefore kills the hung host with SIGKILL. No
  multi-agent suspension is claimed.
- The former 86-scenario BigInt `waitAsync` residue is superseded by S6c
  (merge `981fe000d`, [c2-s6-bigint-atomics.md](c2-s6-bigint-atomics.md)).
  The remaining BigInt failures are the same finite-timeout/cross-agent
  classes above rather than missing BigInt Atomics support.

## Residuals and risks

- Finite positive async timeouts are implemented by S6d when the host installs
  the optional timer continuation callback
  ([c2-s6-waitasync-timeout.md](c2-s6-waitasync-timeout.md)). Embedders
  without that callback retain the pending-until-notify fallback; see that
  receipt for host granularity and stale-task shutdown limitations.
- **Cross-agent waitAsync is unimplemented.** `Atomics.notify` ignores and does
  not count async waiters owned by another `ThreadContext`; those waiters stay
  registered until SAB finalization. The skip check uses only the token's cached
  thread pointer, never a foreign GC record. Same-thread cross-realm waiters do
  wake. This avoids cross-thread GC writes and promise resolution but is not
  multi-agent conformance.
- **Finalizer lock risk remains documented.** `CancelAsyncWaiters` takes the
  WaiterList critical section while unlinking native tokens. It allocates
  nothing and holds no other lock, but a stop-the-world GC suspending a foreign
  thread inside `csForAccess` could stall finalization; a lock-free unlink is
  not bounded in this slice.
- **`Atomics.pause` timing is not measured.** It emits ARM64 `yield` (MSVC
  ARM64 `__yield`, other architectures `YieldProcessor`); no upper-bound tuning
  or benchmark is claimed.
- **Non-Windows `Atomics.wait` remains a no-op.** Preserved deliberately; no
  synchronous suspension behavior changed.
- **JIT/TTD.** No bytecode/JIT helper was changed. The test/debug evidence is
  no-JIT; JIT and TTD were not exercised.
- **B4.** This is Mac ARM64 no-JIT evidence only. Before any B4 claim the
  focused es7 test and the two test262 selections must run on the native Linux
  gate and JIT profiles. The flag-on S6 surface remains a product/security
  decision; no default is changed here.
