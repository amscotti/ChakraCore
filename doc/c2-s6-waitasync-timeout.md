# C2-S6d: finite Atomics.waitAsync timeouts (flag-on slice)

Status: **implemented** — branch `s6-waitasync-timeout`, base `532eab420`,
commit `c37a1c255`. Focused gates green: `zig build check` (test) **583/583**
with regression **2026/0**, `zig build check -Dprofile=debug-static`
**560/560** with regression **2078/0**, `check-native` PASS.
`ESSharedArrayBuffer` (and `ESBigInt` for the BigInt64 paths) remain
**off by default**; this slice changes no default.
Supersedes the finite-timeout residual of
[c2-s6-waitasync.md](c2-s6-waitasync.md) (S6b); everything else in that
receipt is unchanged.

## Scope

1. A finite **positive** `Atomics.waitAsync` timeout now resolves its promise
   to `"timed-out"` when the host has installed the new optional delayed-task
   (timer) continuation callback. The delayed task is a recycler-owned
   `RuntimeFunction` executed by the host on the script thread — the same
   loop as promise jobs. No background thread calls into the runtime.
2. New additive JsRT API `JsSetTimerContinuationCallback` (optional; default
   unset): callback shape `(JsValueRef task, unsigned int delayMilliseconds,
   void* callbackState)`. Existing promise-continuation behavior is
   unchanged. Without the callback, finite positive timeouts keep the S6b
   documented behavior (registration stands, promise stays pending until
   notify) — embedders that install nothing observe no change.
3. Timeout classification is unchanged: `0`, `-0`, negative finite,
   `-Infinity` (and `false`/`null` coercions) are immediate synchronous
   `{async:false, value:"timed-out"}`; `NaN`, `+Infinity` and an omitted
   timeout are infinite (pending until notify); finite positive values are
   scheduled after `ceil` to whole milliseconds — sub-millisecond values
   round **up** to 1 ms (never degrade to the synchronous path), values above
   `UINT32_MAX` ms (~49.7 days) clamp to `UINT32_MAX`. Argument validation
   and coercion order are unchanged (S6b ordering tests still pass).
4. Win/lose arbitration: notify-before-timeout resolves `"ok"` exactly once;
   the later timeout task no-ops without touching the released registration
   token. Timeout-before-notify unregisters the waiter under the waiter-list
   lock, so a later notify counts 0 and cannot re-resolve. GC while a task is
   pending is safe: the host-held task roots the whole chain.

Out of scope: cross-agent (different `ThreadContext`) async suspension and
resolution, `Atomics.wait` changes, default enablement of SAB/BigInt flags,
and any host-independent timer service.

## API

`JsTimerContinuationCallback` / `JsSetTimerContinuationCallback` are declared
in `lib/Jsrt/ChakraCommon.h` beside `JsPromiseContinuationCallback` /
`JsSetPromiseContinuationCallback` and follow the same contract: per
JavaScript context (settable in an object-before-collect callback), host must
not throw across it, and the host owns the `task` reference (`JsAddRef`/
`JsRelease`) from the callback until it has called or dropped the task.
Differences from the promise callback: it receives a minimum delay in
milliseconds, it must run the task no earlier than that delay, and it is
**optional** — the engine never requires it (features degrade to pending
behavior, they never throw for its absence). Timed-debug (TTD) does not model
the host timer queue: while recording or replaying, the engine does not
schedule through this callback and finite timeouts stay pending (guarded in
`AtomicsObject::EntryWaitAsync`).

Exported through `lib/Jsrt/JsrtCommonExports.inc` (used by the legacy
`bin/ChakraCore/ChakraCore.def`) and implemented in `lib/Jsrt/Jsrt.cpp` with
the same `ContextAPINoScriptWrapper_NoRecord` shape as
`JsSetPromiseContinuationCallback`. The `ch` host installs it in both
callback-registration sites (`bin/ch/ch.cpp` run path and the per-file
context in `bin/ch/WScriptJsrt.cpp`), routing delayed tasks into the existing
per-script `MessageQueue` as a `CallbackMessage` with the requested delay —
exactly the `WScript.SetTimeout` mechanism (AddRef on enqueue, `JsCallFunction`
on drain, release after call/queue teardown).

## Core timeout task and ownership record

```text
Resource: AtomicsWaitAsyncTimeoutTaskFunction (RuntimeFunction, recycler) +
          cached native links on the AtomicsAsyncWaiter record
Creation: JavascriptLibrary::CreateAtomicsWaitAsyncTimeoutTaskFunction in
          AtomicsObject::EntryWaitAsync (script thread, after the waiter-list
          lock is released; mirrors FinalizationRegistry cleanup tasks)
Domain: task = Recycler (EnumFunctionClass); record = Recycler normal heap
        (conservatively scanned); registration token = native HeapAllocator,
        HeapDelete'd by the wrapper (unchanged S6b ownership)
Fields: task.waiter (Field, traced) → record; record.buffer (Field, traced) →
        JavascriptSharedArrayBuffer; record also caches plain (non-traced)
        registration + waiterList pointers, set under the waiter-list lock at
        registration (SetRegistration)
Owner/root chain: host queue AddRefs the task (a recycler root) until it is
        called or dropped → task keeps the record alive → record keeps the
        wrapper alive → wrapper owns the native token and roots asyncWaiters;
        wrapper keeps SharedContents (hence the WaiterList) alive. The chain
        makes every cached pointer valid while the record is live.
Transfer/publication: registration under the waiter-list lock (unchanged),
        then SetRegistration caches the native links, then the task is
        created and handed to the host callback inside
        JavascriptLibrary::EnqueueDelayedTask (BEGIN_LEAVE_SCRIPT; host
        exceptions across the boundary are a fatal host bug, same contract as
        EnqueueTask)
Borrow invalidation: settled=true invalidates the cached registration pointer
        (the settle path deletes the token) — TryClaimForTimeout checks
        settled before any dereference and re-checks under the lock; wrapper
        finalization cannot race a live task (the task roots the wrapper);
        runtime disposal invalidates everything (calling the task after
        dispose is the standard JsRT host error)
Arbitration/exactly-once: notify removes the token under the waiter-list lock
        and resolves outside it (S6b flow); the timeout task claims under the
        same lock — settled→no-op, else settled=true + unlink (the critical
        section is recursive, so WaiterList::RemoveAsyncWaiter re-entry is
        safe). All claim/settle mutations run on the registrant's script
        thread; the only off-thread settled write is the pre-existing
        Abandon() cross-agent marker (false→true only, makes the timeout a
        documented no-op)
Lock ordering: csForAccess (waiter list) only; no engine lock is held while
        resolving (allocation/reactions enqueue jobs) — matches notify
Why script-thread-only: the only caller of the task is the host that received
        it inside EnqueueDelayedTask; the ch host drains its MessageQueue on
        the same thread that ran the script (ch.cpp ProcessAll). The engine
        never invokes the task itself and spawns no thread
Failure paths: task creation/enqueue OOM leaves the registration intact
        (promise pending, notify still works — the no-callback fallback);
        host dropping the task without calling it simply never times out
        (waiter stays registered until notify/finalization); late/dropped
        tasks release their root on host release
TTD: not modeled — no scheduling while ShouldPerformRecordOrReplayAction;
        EnqueueDelayedTask TTDAsserts the same precondition
Tests: test/es7/s6-waitasync-pause.js (1ms/multi/sub-ms/GC-survival
        timeouts, notify-wins + late no-op, timeout-wins + late notify 0,
        NaN/+Infinity pending controls, unregistration checks),
        test/es7/s6-bigint-atomics.js (BigInt64 finite timeouts + notify
        wins), test/native-tests/test-shared-basic/sample.cpp (API
        install/uninstall round-trip)
```

## Design notes

- `TryClaimForTimeout` is deliberately split from
  `ResolveTimedOutAfterClaim`: the claim+unlink happens under
  `csForAccess`, the promise resolution strictly outside it (resolution
  allocates and enqueues reactions; the notify path keeps the same
  discipline).
- The shared settle tail `ResolveWith` factors the S6b `ResolveOk` body;
  `ResolveOk` semantics are unchanged (settled check, then settle, release,
  resolve `"ok"`).
- The delay is `ceil(timeout)` clamped to `[1, UINT32_MAX]`; because the
  synchronous timed-out path only triggers for `timeout <= 0` (already
  classified before registration), a ceiled delay can never reproduce the
  fast path, and a sub-millisecond wait cannot expire "before" a notify that
  runs synchronously later on the same thread.

## Evidence

Mac ARM64 (aarch64-apple-macos.26.0), test profile (RelWithDebInfo, no JIT)
and debug-static, shared gate lock (`sh /tmp/opencode/cc-gate-lock.sh`),
local `.zig-cache`/`out`; no `--prepare`, no full corpus/check. Every
authoritative build/test262 command ran through the gate lock. Node runner
**v22.22.3** (the machine's current default; S6b/S6c recorded v26.5.0).
Binaries (SHA-256, launcher + dylib): after `06bf1875…`/`9e2a7133…`
(`out/s6d-after-bin`), base `cd29fd50…`/`0422fea8…` (`out/s6d-base-bin`,
built from the stashed tree at `532eab420`).

- Builds: `zig build -Dprofile=test --summary line` **568/568** (only the
  pre-existing S6b `__yield` arm_acle warning). `zig build check`
  **583/583** (regression **2026/0 PASS**, bytecode, native embedding,
  smoke, metadata incl. exports, selftest); `zig build check
  -Dprofile=debug-static` **560/560** (metadata PASS, static native
  suites); explicit `zig build check-regression -Dprofile=debug-static`
  **2078/0 PASS**; `zig build check-native -Dprofile=test` **PASS**
  (`test-shared-basic` compiles/runs the extended sample.cpp).
- Export/metadata: `nm -gU` after dylib contains
  `_JsSetTimerContinuationCallback` beside `_JsSetPromiseContinuationCallback`
  (base dylib: absent); `check_metadata` export gate PASS both profiles
  (additive export; the static archive's 8 key JsRT symbols unchanged).
- Extended `test/es7/s6-waitasync-pause.js` (1ms, 0.5ms, 1e-7ms →
  `ceil`→1ms, three simultaneous finite waiters, GC while a 30ms task is
  pending, notify-before-60ms-timeout wins, 1ms-timeout-before-notify +
  late notify counts 0, NaN/+Infinity/omitted pending at 50ms then
  notify-resolved, timed-out waiters unregistered): **pass** on the test
  binary in the registered flags
  (`-ESSharedArrayBuffer -Test262 -CollectGarbage -force:deferparse`),
  ×3 repeats, `-nodeferparse`, and without `-CollectGarbage`; **pass** on
  the debug-static binary in the registered, `-nodeferparse` and no-GC
  modes. The base binary **fails exactly the 8 new finite-timeout checks**
  (exit 1) and still passes every S6b check — precise before/after
  discrimination.
- Extended `test/es7/s6-bigint-atomics.js` (BigInt64 finite timeout →
  `"timed-out"`, notify-before-timeout wins, late notify counts 0):
  **pass** on the test binary (registered, `-nodeferparse`,
  `-force:deferparse`, no-GC) and on the debug-static binary (registered,
  `-nodeferparse`, `-force:deferparse`, no-GC).
- Runtime probes (`out` scripts): default-off `SAB=undefined
  Atomics=undefined` (unchanged). Flag-on differential, same script both
  binaries — base: `waitAsync=function notify-wins=1
  timeout-wins-late-notify=1 outcomes=ok,pending,ok`; after:
  `waitAsync=function notify-wins=1 timeout-wins-late-notify=0
  outcomes=timed-out,timed-out,ok`.
- Adjacent es7 (after binary): `s6-growable-nonshared.js` 13/13 with
  `-ESSharedArrayBuffer` and with both flags + GC; `atomics_test.js`
  16/16 with both flag sets; `sharedarraybuffer.js` 7/7;
  `bigint64array.js` 10/10 (both flags + GC).
- Pinned test262 `2e0a56762801e275a9fdf96dc49d90ba0cddcf63`, 20 controls
  passed per run, host flags
  `-ESBigInt -ESSharedArrayBuffer -nonative -Test262 -CollectGarbage`,
  jobs 6, timeout 10s (`tools/test262/run.py`, artifacts
  `out/test262-s6d2-*`):
  - Exact `Atomics/waitAsync/**/*.js` (101 files, **202 scenarios**):
    base **116/86 → after 122/80**, **6 fail→pass / 0 pass→fail** —
    `true-for-timeout.js`, `returns-result-object-value-is-promise-
    resolves-to-timed-out.js` and their `bigint/` twins, each
    default+strict. (The task framing said "+4": that is the BigInt-less
    S6b selection; this selection carries `-ESBigInt`, so the two BigInt64
    scenarios flip as well.) All **80 residual failures** are the 40
    `$262.agent` crossthread files × 2 scenarios — the documented
    cross-agent residual; **zero** non-agent waitAsync failures remain.
  - Focused `Atomics/**/*.js` + `SharedArrayBuffer/**/*.js`
    (**982 scenarios**): base **814/168 → after 820/162**, the **same
    6 fail→pass / 0 pass→fail** (S6c recorded 813–814 on its binary;
    base here reproduces 814).
  - Adjacent `Atomics/wait/*.js` + `Atomics/notify/*.js`
    (**190 scenarios**): **140/50 base → 140/50 after, 0/0** — matches the
    S6c-recorded 140/190 exactly.
- Native API round-trip: `test/native-tests/test-shared-basic/sample.cpp`
  sets `JsSetTimerContinuationCallback`, runs the sample script, unsets it;
  exercised by `check-native` above. The callback itself cannot deliver a
  task in that sample (the SAB flags are off there), so only the
  set/unset contract is proven natively; task delivery is proven through
  the ch host probes/tests.

## Residuals

- Cross-agent waitAsync remains unimplemented (S6b residual, unchanged):
  the 80 remaining `waitAsync/**` failures are all `$262.agent` crossthread
  scenarios; a cross-`ThreadContext` notify still leaves the waiter
  registered (it `Abandon`s the record, which deliberately makes the
  timeout task a no-op).
- TTD record/replay does not schedule or replay finite timeouts (guarded in
  `EntryWaitAsync` and `TTDAssert`ed in `EnqueueDelayedTask`); **not
  exercised** in this slice's evidence — consistent with S6b/S6c, which
  also ran no TTD.
- The engine still has no host-independent timer: embedders without the new
  callback keep pending promises for finite timeouts (documented fallback,
  unchanged from S6b; the API round-trip is proven, uninstalled-callback
  behavior is only covered by the base-binary runs, which have no host
  timer at all).
- The `ch` `MessageQueue` timer uses `GetTickCount` + `Sleep` granularity:
  delays are minimums (never early), and queue ordering is by absolute due
  time, so ordering robustness holds, but no sub-millisecond precision or
  monotonic-clock claim is made.
- JIT configurations are gated on B4 (Mac no-JIT evidence only).
- Operational note (harness, unchanged from S6c): the gate-lock wrapper's
  `exec` discards its EXIT trap, so killed runs leave a stale lock. This
  session reclaimed a demonstrably ownerless lock twice by aging its mtime
  per the script's own 6-hour policy (no active zig/runtests/run.py gate
  process held it) after tool-timeout kills.
