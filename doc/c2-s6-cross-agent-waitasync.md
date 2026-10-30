# C2-S6e: cross-agent Atomics.waitAsync (flag-on slice)

Status: **landed** — implementation `d972c53fe`, review repair `e547fa345`
(docs receipt below) on branch `s6-cross-agent-waitasync` (base `f305a175f`);
evidence below. `ESSharedArrayBuffer` (and `ESBigInt` for the BigInt64 paths)
remain **off by default** (BigInt itself is default-on since S2); this slice
changes no default. It supersedes the cross-agent residual of
[c2-s6-waitasync.md](c2-s6-waitasync.md) (S6b); S6d finite timeouts are
unchanged and now arbitrate with the new wake path.

## Review repair (e547fa345)

1. **Token-latch ordering (UAF window).** The foreign notify now performs,
   entirely under the waiter-list CS: latch → list removal →
   `linked = false` as its **final** token write. While `linked == true`,
   every path that may delete the token (wrapper finalization through
   `WaiterList::RemoveAsyncWaiter`, the registrant's claim through
   `JavascriptSharedArrayBuffer::RemoveAsyncWaiter`) must first take that
   same CS, so nothing can free the token mid-handoff; once `linked` reads
   false under the CS the notifier is provably done with the token. No
   monitor can claim between latch and unlink: `TryClaimForWake` runs on the
   registrant thread and needs the same CS — observing the latch early only
   parks it there until the unlink completes. The same-thread handoff branch
   writes `nextAwoken` before `linked = false` for the same reason, and
   `CancelAsyncWaiters` now reads `linked` under the CS (HeapDelete stays
   outside the lock).
2. **Crash root cause (SIGTRAP/SIGBUS, reproduced).** macOS `.ips` backtrace
   of a crashing multi-agent run: *child* `RuntimeThreadData::ThreadProc` →
   `WScriptJsrt::Uninitialize` → `scriptDirMap.clear()` (process-global
   `std::map<DWORD_PTR, std::string>`) → `~basic_string` → malloc abort.
   Since S6e, agents routinely exit through `leaving()`, and every child
   exit called `Uninitialize`, which cleared the four process-global
   module/script maps from agent threads — racing the main thread and each
   other (libcpp `std::map` writes) and corrupting the heap. With ≥3 agents
   leaving concurrently this is the reviewer's reproducible crash; it also
   explains the first session's one-off corpus SIGSEGV. Fix: the global-map
   clears are main-thread-only (no `threadData`, or the root
   `RuntimeThreadData` with `parent == nullptr`); agent threads only join
   their own sub-agents. TSan is not wired for this build (the legacy
   sanitizer wiring is documented as suspect), so the evidence is the
   captured backtrace plus 255+ zero-signal stress runs after the fix.
3. **Broadcast handshake.** `BroadcastCallback` used to create a semaphore
   sized for all children but wait for **one** release, then close it —
   leaving every other child to `ReleaseSemaphore` a closed (and possibly
   recycled) handle — and a following broadcast could overwrite
   `RuntimeThreadData::sharedContent` before slow consumers read it.
   Replaced with a per-child auto-reset **consumed event** owned by the
   child's `RuntimeThreadData` for the child's whole lifetime (the parent
   never waits on a closable-from-under-it handle): the child sets it after
   creating its SAB wrapper (consuming the content) and **before** running
   any script; a departing (`leaving`/shutdown) child acknowledges without
   consuming and never touches `parent->sharedContent` again; the parent
   waits for each live child (zero-timeout probe, then 50 ms slices, ≤5 s
   bounded, skipping threads that already exited) before releasing the
   content handle or returning, so `sharedContent` cannot be overwritten
   until every live child consumed it. Dead-agent broadcasts are skipped
   via the thread-liveness check (not a warning); only a live non-acking
   agent warns on stderr.
4. **`monitored` publication (OOM edge).** `monitored` now starts false and
   is published (under the waiter-list CS) only after the monitor task was
   successfully created and enqueued. On task-creation OOM the registration
   therefore keeps the exact S6b contract — foreign notifies skip and do
   not count the waiter — instead of the previously possible
   counted-but-unresolvable wake. The publishing thread is the only
   drainer of its host queue, so the task cannot run before publication
   completes.
5. **Counting nuance (documented).** A foreign notify can return 1 while
   the registrant's already-due timeout task wins the resolution inside
   the ≤2 ms poll window: `settled` arbitrates exactly-once, and the
   promise resolves `"timed-out"` after a notify that counted the waiter —
   the spec's own concurrent-timeout ambiguity, same shape as the sync
   wait.

Artifacts (this repair round): test-profile binary `zig-out/bin/ch`
(UUID `EC33225D-8002-3850-9F56-E6DC31CDDBBA`), genuine debug-static binary
`out/debug-static/bin/ch` (UUID `93518278-2940-3E19-840F-8C578873F0FA`).
The first session's `out/s6e-bins/ch-debug-static` copy was in fact a
test-profile build (identical UUID to `ch-test`) — mislabeled, corrected
here; debug-profile evidence below was re-run on the genuine path.

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
`monitored == true` is processed in the order: set the atomic wake-request
latch → remove from the shared list → `linked = false` (the notifier's
final token touch), and it counts toward notify's return value. The latch
leads so that, throughout the handoff, the token is either still linked
(deletion paths must take this same CS) or the notifier has no further
writes; see the repair section for the full ordering argument. A foreign
entry without a monitor keeps the S6b contract: left registered, not
counted, promise pending. The branch reads and writes only native token
fields; `record` is never dereferenced from the notifying thread. FIFO
order is preserved because foreign entries are consumed in waiter-list
order like same-thread ones.

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
New fields: monitored (false at registration, published under the
        waiter-list lock only after the monitor task is created and
        enqueued — task-creation OOM keeps the S6b skip-and-uncount
        contract; read by foreign notifiers under the same lock) and
        wakeRequested (volatile long latch; set by a foreign notify with an
        interlocked RMW under the waiter-list lock BEFORE the token is
        unlinked and with linked=false as the notifier's final token
        write; read by the registrant's monitor with an interlocked RMW
        without the lock). The waiter-list CS orders the structural
        unlink; the RMWs order the flag itself.
Cross-thread rule: the notifying thread touches ONLY {threadContext
        (read), monitored (read), linked (final write), wakeRequested
        (RMW)} and list membership — all native fields. It never
        dereferences record, never touches the wrapper/GC lists, and never
        frees the token. The child (registrant) exclusively performs the
        claim, the wrapper-list unlink and the token deletion, so there is
        no double delete; the finalizer reads linked under the same CS
        before its fast-path HeapDelete.
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
Failure paths: monitor task creation/enqueue failure (OOM) never publishes
        `monitored`, so the waiter keeps the S6b contract — foreign
        notifies skip and do not count it, same-thread notify still
        resolves; no cross-thread hazard is introduced. Child shutdown with
        a pending monitor drops the task at queue teardown (JsRef released
        before dispose); the promise stays pending by design.
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
that must be released while the runtime is valid. The child's drain does
not call `JsTTDNotifyYield` (TTD models no host timer queue, and agents
schedule no delayed tasks while recording/replaying). The broadcast
handshake is the per-child consumed-event protocol described in the repair
section; on exit the child acknowledges any pending broadcast without
consuming it.

## Evidence

Two rounds: implementation `d972c53fe` (first session, summarized) and the
review repair `e547fa345` (this round, authoritative). Mac ARM64
(aarch64-apple-macos.26.0), test profile (RelWithDebInfo, no JIT) and
debug-static, shared gate lock (the wrapper leaks its lock on completion —
the documented S6c/S6d operational note; reclaimed by mtime aging only when
no zig/runtests/run.py gate process was active). Pinned test262
`2e0a56762801e275a9fdf96dc49d90ba0cddcf63`, 20 controls passed per run,
Node **v26.5.0**, jobs 6, timeout 10 s, host flags
`-ESBigInt -ESSharedArrayBuffer -nonative -Test262 -CollectGarbage`.

Repair-round binaries: `zig-out/bin/ch` (test, UUID
`EC33225D-8002-3850-9F56-E6DC31CDDBBA`) and `out/debug-static/bin/ch`
(UUID `93518278-2940-3E19-840F-8C578873F0FA`).

- Builds: test **568/568**, debug-static **546/546** (only pre-existing
  warnings).
- Focused `test/es7/s6-waitasync-cross-agent.js` — now **10 scenarios**
  (the original 8 plus: 3 agents × 2 waiters + main waiter with
  `notify(4)`→4 / rest→3 / all-unregistered, and 3 repeated broadcasts to
  the same three agents with per-round partial notify counts) — **pass** on
  the test binary (registered ×3, `-nodeferparse`, `-force:deferparse`,
  no-GC) and on `out/debug-static/bin/ch` (registered, `-nodeferparse`).
  S6b/S6D `s6-waitasync-pause.js` and S6c `s6-bigint-atomics.js` **pass**
  in registered/variant modes on both profiles; adjacent es7
  (`s6-growable-nonshared` 13/13, `atomics_test` 16/16, `sharedarraybuffer`
  7/7, `bigint64array` 10/10) green. Default-off probes unchanged
  (`SAB=undefined Atomics=undefined` with no flags / `-ESBigInt` alone;
  both flags present everything). The first session's base-binary
  discrimination (19 failing checks, exit 1) carries over — the repair
  changes no scenario semantics, only concurrency.
- **Stress (review recipe, out-of-gate)**: the exact adversarial recipe
  (3 agents × 2 waiters + main waiter, `notify(4)`, notify rest, agents
  `leaving()`) × **105** on the test binary and × **30** on
  `out/debug-static/bin/ch` — **zero signals**; plus the full focused test
  file ×70 and a repeated-broadcast variant ×80 under 2-way parallel load —
  **zero signals** (255+ runs total post-fix; no new `.ips` reports). The
  pre-fix crash is captured in
  `~/Library/Logs/DiagnosticReports/ch-2026-10-29-211944.ips` (child
  ThreadProc → Uninitialize → scriptDirMap clear → malloc abort, SIGTRAP;
  the same corruption family explains the SIGBUS variant and the first
  session's corpus SIGSEGV flake). TSan is not wired for this build.
- **Exact `Atomics/waitAsync/**` corpus (202 scenarios), two runs**:
  **198/4** and **197/5** (vs base 122/80, first-session after 193/9 — the
  handshake repair removed the SIGKILL/timeout flake class). **0
  pass→fail** vs base in both runs; the shifting 4–5 residuals are all
  `no-spurious-wakeup-*` (the witness race below — the first session
  demonstrated `unpark=not-equal` at 0.06 ms vs on-time 200 ms timeouts).
- **Atomics + SAB selection (982)**: Atomics `**` **693/81** (base 615/159
  in the first session; residuals = pre-existing sync-`Atomics.wait` agent
  scenarios + the flaky no-spurious family), `SharedArrayBuffer/**`
  **204/4** (the same 4 documented constructor-order scenarios). Merged
  **897/85**.
- Gates (expected-pin overrides; pins deliberately not edited):
  `check_regression.py --select test --expected-passed 2031` →
  **PASS 2031/0**; `--select debug` (on `out/debug-static/bin/ch`)
  `--expected-passed 2083` → **PASS 2083/0**. Pin move is exactly the one
  new rlexe entry (test 2030→2031, debug 2082→2083). `zig build check`
  fails only its embedded count step against the un-edited 2030 pin
  (2031/0 actual), matching the S6c policy.

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
- **`no-spurious-wakeup-*` witness race under parallel load**: 4–5 of 202
  waitAsync scenarios fail non-deterministically at `jobs 6` (subset shifts
  between runs; 197–198 passing) because the parent's RMW after
  `tryYield`(100 ms) can beat a starved agent's witness read — the correct
  `not-equal` fast path then resolves immediately. This is host thread
  scheduling granularity, not the wake/timeout machinery (probe evidence
  in Evidence); a longer yield, an agent-side "parked" handshake, or a
  scheduler-aware host would make it deterministic.
- **Notify-count vs concurrent timeout**: a foreign notify may return 1
  while the registrant's already-due timeout task wins the resolution in
  the ≤2 ms poll window; `settled` arbitrates exactly-once and the promise
  resolves `"timed-out"` — the spec's own concurrent-timeout ambiguity
  (documented in the repair section).
- **Remaining shared-host global state**: `WScriptJsrt::GetNextSourceContext`
  (a plain static counter) is incremented from agent threads — a benign
  duplicate-source-context race, unchanged from the pre-S6e host; module
  loads inside an agent still push to the main queue (`PushMessage`). Both
  pre-existing, unexercised by test262 agents.
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
- **JIT/B4**: no bytecode/JIT surface changed; evidence is Mac ARM64 no-JIT.
  B4 must repeat the focused tests and the two test262 selections on native
  Linux and JIT profiles.
