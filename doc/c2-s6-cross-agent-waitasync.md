# C2-S6e: cross-agent Atomics.waitAsync (flag-on slice)

Status: **landed** — implementation `d972c53fe` on branch
`s6-cross-agent-waitasync` (base `f305a175f`); evidence below.
`ESSharedArrayBuffer` (and `ESBigInt` for the BigInt64 paths) remain **off
by default** (BigInt itself is default-on since S2); this slice changes no
default. It supersedes the cross-agent residual of
[c2-s6-waitasync.md](c2-s6-waitasync.md) (S6b); S6d finite timeouts are
unchanged and now arbitrate with the new wake path.

## Scope

1. A **foreign** `Atomics.notify` (a different `ThreadContext`) now wakes a
   cross-agent `Atomics.waitAsync` waiter: the notify counts the waiter,
   removes its native token from the shared waiter list, and atomically sets
   a wake-request latch on the token. The notifying thread never
   dereferences the waiter record, the owner wrapper or any GC-visible
   field, never mutates the registrant runtime, and never frees the token.
2. The registrant agent resolves the wake itself: `Atomics.waitAsync` on
   **cross-runtime** shared content schedules a recycler-owned monitor task
   through the S6d host timer callback. The monitor polls the latch at a
   bounded 2 ms interval and, on wake-request, claims/settles/unlinks and
   resolves `"ok"` — all on the registrant's thread, outside locks for the
   resolution. Unmonitored content keeps the S6b behavior exactly.
3. The `ch` child-agent runtime now has a **per-child job/timer queue**: the
   promise and timer continuation callbacks are installed on the child
   context, and an agent event loop drains due jobs (promise reactions,
   async-function steps, timeout/monitor tasks) after `receiveBroadcast`
   callbacks and whenever a delayed task comes due, while still serving
   broadcast/shutdown events promptly. Async `receiveBroadcast` functions
   (the shape every remaining test262 agent waitAsync scenario uses) now
   suspend and resume correctly.
4. Arbitration is exactly-once through the existing `settled` flag: timeout
   vs monitor vs same-thread notify — whoever runs first on the registrant
   thread settles, unlinks and resolves; every later task is a no-op that
   never touches the released token. A resolved waiter is unregistered, so
   late notifies count 0.

Out of scope: sync `Atomics.wait` suspension (still a non-Windows no-op),
default enablement of SAB/BigInt flags, TTD modeling of the host timer
queue (cross-agent waits stay pending while recording/replaying), and any
event-based (non-polling) wake mechanism.

## Design

### Foreign notify handoff (`WaiterList::RemoveAndWakeWaiters`)

Under the waiter-list lock, a foreign async entry whose token has
`monitored == true` is removed from the shared list (`linked = false`),
counted toward notify's return value, and its `wakeRequested` latch is set
with an interlocked RMW. A foreign entry without a monitor keeps the S6b
contract: left registered, not counted, promise pending. The branch reads
and writes only native token fields; `record` is never dereferenced from
the notifying thread. FIFO order is preserved because foreign entries are
consumed in waiter-list order like same-thread ones.

### Cross-runtime opt-in (`SharedContents::MarkCrossRuntime`)

The monitor is **not** scheduled for every waitAsync. It requires all of:

- the host installed the S6d timer continuation callback (a scheduling
  mechanism must exist), and
- the shared content is marked cross-runtime — set when a host extracts a
  content handle (`JsGetSharedArrayBufferContent`) or creates a wrapper
  from one (`JsCreateSharedArrayBufferWithSharedContent`, also covering SCA
  deserialization of shared buffers), and
- not TTD record/replay.

Consequence: ordinary single-runtime `Atomics.waitAsync` (the overwhelmingly
common embedding, and every main-thread test262 scenario that does not
broadcast) gains **no polling**; only buffers actually shared with another
runtime do. A host that never shares content sees no behavior change beyond
S6d. The latch is a one-way interlocked counter; the host broadcast
handshake (event/semaphore) already orders the marker against the receiving
runtime's reads.

### Monitor task (`AtomicsWaitAsyncMonitorTaskFunction`)

Same recycler-owned model as the S6d timeout task: the host queue AddRefs
the task, rooting task → record → wrapper → native token → SharedContents,
so the record's cached native links stay valid for the task's whole
lifetime. The task stops immediately if the waiter settled or the
registration is gone (the token may already be released — `settled` /
`registration` are checked before any use). On a set latch it claims
(`TryClaimForWake`: settle + unlink + release the token under the
waiter-list lock) and resolves `"ok"` outside the lock; otherwise it
re-enqueues **itself** through `EnqueueDelayedTask` at
`AtomicsAsyncWaiter::MonitorPollIntervalMs` (2 ms), which is one more
AddRef/Release cycle on the host side and keeps the root chain alive.

Timeout arbitration: the timeout task and the monitor task both run on the
registrant's host queue — serialized, no concurrency. Timeout first: it
settles and releases the token; the monitor's next poll sees `settled` and
stops. Monitor first: it settles and releases the token; the late timeout
task's `TryClaimForTimeout` short-circuits on `settled` without touching
the freed token. Same-thread notify (including cross-realm) keeps using the
S6b handoff and settles before the monitor's next poll.

### Memory ordering and lifetime record

```text
Resource: native AtomicsAsyncWaiterRegistration token (+ new atomic fields)
Creation: HeapNew in JavascriptSharedArrayBuffer::AddAsyncWaiter on the
          registrant's thread, under the waiter-list lock (unchanged S6b)
Domain: native HeapAllocator; released only with HeapDelete from
        JavascriptSharedArrayBuffer::RemoveAsyncWaiter (registrant thread)
        or CancelAsyncWaiters (wrapper finalizer / runtime dispose)
New fields: monitored (immutable after publication under the waiter-list
        lock; written once at registration, read by foreign notifiers under
        the same lock) and wakeRequested (volatile long latch; set by a
        foreign notify with an interlocked RMW under the waiter-list lock
        after the token is unlinked; read by the registrant's monitor with
        an interlocked RMW without the lock). The waiter-list CS orders the
        structural unlink; the RMWs order the flag itself.
Cross-thread rule: the notifying thread touches ONLY {threadContext
        (read), monitored (read), linked (write), wakeRequested (RMW)} and
        list membership — all native fields. It never dereferences record,
        never touches the wrapper/GC lists, and never frees the token. The
        child (registrant) exclusively performs the claim, the wrapper-list
        unlink and the token deletion, so there is no double delete.
Root chain: host queue AddRef(monitor task) → task.waiter (traced Field) →
        record.buffer (traced Field) → wrapper → asyncRegistrations token +
        SharedContents (waiter list). While the task is queued or executing
        the chain is live; wrapper finalization therefore cannot race a
        live monitor (same argument as S6d's timeout task).
settled: mutated only on the registrant's thread (notify resolution,
        timeout claim, monitor claim); the defensive cross-thread Abandon
        tail remains but is unreachable for monitored foreign waiters.
Finalizer: CancelAsyncWaiters unlinks tokens whose `linked` is still true
        (a foreign-notified token has linked=false and is only deleted) and
        HeapDeletes them without touching records — unchanged S6b path; a
        foreign notify cannot race it because both take the waiter-list CS
        for the structural part and neither frees what the other owns.
Failure paths: monitor task creation/enqueue failure (OOM) degrades to the
        unmonitored contract — the waiter stays registered, foreign notifies
        skip and do not count it (S6b fallback); no cross-thread hazard is
        introduced. Child shutdown with a pending monitor drops the task at
        queue teardown ( JsRef released before dispose); the promise stays
        pending by design.
Thread/context: promise resolution, GC-visible writes and token deletion
        happen exclusively on the registrant's thread. The notifying thread
        performs no JsRT/GC operation beyond its own Atomics.notify.
Tests: test/es7/s6-waitasync-cross-agent.js (see Evidence).
```

### Child-host job support (`RuntimeThreadData::ThreadProc`)

Each agent thread creates a `MessageQueue`, installs
`WScriptJsrt::PromiseContinuationCallback` and `TimerContinuationCallback`
on its own context with that queue (the same callbacks the main loop uses;
`WScript.SetTimeout`/`ClearTimeout` and samethread loads are routed to the
calling thread's queue via `GetThreadMessageQueue`), and runs an event
loop: block on `{receivedBroadcast, shutdown}` with a timeout computed from
the queue's earliest due time (`NextDueTime`/`HasDueMessage`/`PopDue` — new
`MessageQueue` accessors that never sleep), then drain due messages at that
safe point. A pending self-rescheduling monitor therefore bounds the block
by its poll interval instead of parking the thread forever, and shutdown /
`leaving` are observed between messages (checked in the drain loop and
after it), so teardown stays prompt. The queue is destroyed
(`RemoveAll` + delete, releasing each task's JsRef) **before**
`JsDisposeRuntime` in both the normal and error exits — messages own refs
that must be released while the runtime is valid.

## Evidence

Mac ARM64 (aarch64-apple-macos.26.0), test profile (RelWithDebInfo, no JIT)
and debug-static, shared gate lock (`sh /tmp/opencode/cc-gate-lock.sh`),
local `.zig-cache`/`out`; no `--prepare`, no full corpus/check. Every
authoritative build/test262 command ran through the gate lock (the wrapper
leaks its lock on completion — the documented S6c/S6d operational note; it
was reclaimed by aging per the script's own policy only when no
zig/runtests/run.py gate process was active). Pinned test262
`2e0a56762801e275a9fdf96dc49d90ba0cddcf63`, 20 controls passed per run,
Node **v26.5.0**, jobs 6, timeout 10 s, host flags
`-ESBigInt -ESSharedArrayBuffer -nonative -Test262 -CollectGarbage`. Base
binary: `/Users/ascotti/Documents/code/ChakraCore/zig-out/bin/ch` (the main
worktree clean at `f305a175f`, SHA-256 `3cbb54a7…`).

- Builds: `zig build -Dprofile=test --summary line` **568/568**;
  `-Dprofile=debug-static` **546/546** (only pre-existing warnings).
  `zig build check` **581/583**, the one failed step being the embedded
  `check-regression` count (actual **2031/0** vs the deliberately un-edited
  pin 2030 — same policy as S6c). Authoritative gates with explicit
  overrides: `tools/build/check_regression.py ... --select test
  --expected-passed 2031` → **PASS: passed 2031 / failed 0**;
  `--select debug --expected-passed 2083` (binary
  `out/debug-static/bin/ch`) → **PASS: passed 2083 / failed 0**. Pin move
  is exactly the new rlexe entry: test 2030 → 2031, debug 2082 → 2083.
- New `test/es7/s6-waitasync-cross-agent.js`, registered once in
  `test/es7/rlexe.xml` with
  `-ESBigInt -ESSharedArrayBuffer -Test262 -CollectGarbage -args summary`:
  **pass** on the test binary (registered, `-nodeferparse`) and on the
  debug-static binary (registered, `-nodeferparse`). 8 scenarios: Int32
  foreign notify → `"ok"` + count 1 + unregistered-after; BigInt64 same
  (timeout-before-notify → `"timed-out"` + late notify 0; notify-before-500
  ms-timeout → `"ok"` + late notify/task no-op; two agents on separate
  indexes; FIFO order A,B within one agent across a `CollectGarbage`; async
  `receiveBroadcast` draining across awaits; reverse direction — main-thread
  waiter + monitor in the main queue woken by an agent's notify, count 1,
  then unregistered). The **base binary fails exactly the 19 new checks**
  (exit 1, deterministic) — precise before/after discrimination.
- S6b/S6d focused `s6-waitasync-pause.js`: **pass** ×3 registered flags,
  `-nodeferparse`, without `-CollectGarbage`, and with `-ESBigInt` added;
  debug-static registered **pass**. S6c focused `s6-bigint-atomics.js`:
  **pass** registered, `-nodeferparse`, `-force:deferparse`; debug-static
  registered **pass**. Adjacent es7: `s6-growable-nonshared.js` 13/13,
  `atomics_test.js` 16/16, `sharedarraybuffer.js` 7/7, `bigint64array.js`
  10/10 (both flags + GC).
- Default-off probes (identical shape to the S2 baseline): no flags
  `SAB=undefined Atomics=undefined B64=function BigInt=function`;
  `-ESBigInt` alone unchanged; `-ESSharedArrayBuffer` (alone or with
  `-ESBigInt`) `SAB=function Atomics=object`.
- **Exact `Atomics/waitAsync/**` corpus (101 files, 202 scenarios)**:
  base **122/80 → after 193/9**, **71 fail→pass / 0 pass→fail**
  (scenario-for-scenario diff over `results.json`; artifacts
  `out/test262-s6e-waitAsync-{base,after}`). A full re-run on the same
  binary gives **195/7** with a *different* residual subset — the
  remaining 7–9 failures are all `no-spurious-wakeup-*` scenarios whose
  `lapse >= TIMEOUT` assertion loses the upstream witness race when the
  agent thread is starved under 6-way parallel load: the parent's RMW
  (after `tryYield`'s 100 ms) lands before the descheduled agent's
  `waitAsync` reads the witness, so the *correct* `not-equal` fast path
  resolves in ~0.06 ms (demonstrated directly: probe reports
  `unpark=not-equal duration=0.06`; when registration wins, the same probe
  measures `duration=200.5/201.8` — the timeout fires on time). One
  `no-spurious-wakeup-on-store` SIGSEGV under 6-way load was not
  reproduced in isolation or in the re-run — same class as the S6c
  documented multi-agent load flake. Expected single-source pin move:
  test 2026 → 2027, debug-static 2078 → 2079 (pins not edited here).
- **Atomics + SAB selection (982 scenarios)**: Atomics `**` subset
  (774 scenarios): base **615/159 → after 694/80**, **79 fail→pass /
  0 pass→fail** (artifacts `out/test262-s6e-982-{base,after}`); the 80
  residuals are the documented pre-existing sync-`Atomics.wait` agent
  scenarios (non-Windows no-op suspension) plus the flaky no-spurious
  family. `SharedArrayBuffer/**` subset (208 scenarios): **204/4** — the
  same 4 documented SAB constructor-order scenarios as S6c/S6d
  (`proto-from-ctor-realm`, `options-maxbytelength-compared-before-
  object-creation`, default+strict). Merged 982: ≈**819/163 → 898/84**
  (base SAB half assumed at the recorded 204/4; consistent with S6d's
  820/162 on its binary).

## Residuals and risks

- **Unnotified cross-agent infinite waits poll forever** (by design): the
  monitor re-enqueues at 2 ms until settle, shutdown or leaving; the waiter
  root chain stays alive for the whole wait. On the main thread this also
  means `ch`'s `ProcessAll`-until-empty loop cannot finish while a
  main-queue monitor is pending — an unnotified main-thread waitAsync on
  broadcast content keeps the process alive polling (a notified one
  resolves and drains normally). Node keeps such processes alive too
  (pending promise + live worker); documented rather than capped, because
  every cap silently breaks late notifies (counted-but-unresolved waiters
  or unregistered-but-pending promises are worse).
- **`no-spurious-wakeup-*` witness race under parallel load**: 7–9 of 202
  waitAsync scenarios fail non-deterministically at `jobs 6` (subset shifts
  between runs; 193–195 passing) because the parent's RMW after
  `tryYield`(100 ms) can beat a starved agent's witness read — the correct
  `not-equal` fast path then resolves immediately. This is host thread
  scheduling granularity, not the wake/timeout machinery (probe evidence
  in Evidence); a longer yield, an agent-side "parked" handshake, or a
  scheduler-aware host would make it deterministic.
- **Polling latency**: a foreign wake is observed within one poll interval
  (2 ms) plus host sleep granularity — the host timer contract's few-ms
  early/late fire (S6d) applies. No fairness/starvation claim beyond FIFO
  list order.
- **Main-thread waitAsync registered before the buffer is broadcast** gets
  no monitor (the mark happens at extraction/from-content creation), so a
  foreign notify for it is skipped and uncounted. test262 does not exercise
  this ordering; a future slice could re-check the mark at notify time and
  fall back to leaving the waiter registered.
- **TTD**: no delayed tasks (hence no monitors) while recording/replaying;
  cross-agent waitAsync stays pending — the documented S6d stance.
- **Finalizer lock risk** (S6b, unchanged): `CancelAsyncWaiters` takes the
  waiter-list CS; a stop-the-world GC suspending a foreign thread inside
  `csForAccess` could stall finalization. Not worsened by this slice (the
  foreign notify holds the CS no longer than before).
- **Module loads inside an agent thread** still push to
  `WScriptJsrt::PushMessage` (main queue); pre-existing, unexercised by
  test262 agents.
- **JIT/B4**: no bytecode/JIT surface changed; evidence is Mac ARM64 no-JIT.
  B4 must repeat the focused tests and the two test262 selections on native
  Linux and JIT profiles.
