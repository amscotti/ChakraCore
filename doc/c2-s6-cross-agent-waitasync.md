# C2-S6e: cross-agent Atomics.waitAsync (flag-on slice)

Status: **landed** — implementation `d972c53fe`, review repair `e547fa345`
(docs `6c319e065`… `05d70b805`), final repair iteration 2
(`e23fc6f6b`), teardown-only iteration 3 (`WScript.Quit` raw exit; this
commit, with the post-landing SIGBUS diagnosis and fresh campaign
evidence below) on
branch `s6-cross-agent-waitasync` (base `f305a175f`); evidence below. `ESSharedArrayBuffer` (and `ESBigInt` for the BigInt64 paths)
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

## Review repair, iteration 2 (final)

1. **Child runtime teardown crashes (SIGSEGV/SIGTRAP/SIGBUS).** The
   adversarial campaign reproduced two independent families on the
   iteration-1 binaries:
   - *Dispose family*: child `JsDisposeRuntime` →
     `Recycler::FinalizeConcurrent` → `RecyclerParallelThread::Shutdown` →
     `ThreadService::HasCallback` on a stale pointer. Each agent runtime
     was created with no thread service, so every agent spun its own
     "Chakra Background Recycler" + parallel-mark threads whose
     concurrent/parallel shutdown raced the agent's own dispose.
   - *Exit family*: `WScript.Quit` called raw `ExitProcess` while agent
     threads were mid-JsRT-call; the process-detach path then walked their
     hard-terminated runtimes (queued-callback/context-teardown SIGSEGV in
     `JsGetGlobalObject`, `!currentThreadContext->IsScriptActive()` and
     `recycler->IsReentrantState()` asserts). Minimized reproducers showed
     the trigger is `Quit` with live agents (8 agents × 2 waiters crashed
     17–21/25; the same scripts without `Quit`, or single-agent variants,
     were clean), and that `CollectGarbage` was irrelevant.
   Fixes (no signal catching/hiding): agent runtimes are created with
   `JsRuntimeAttributeDisableBackgroundWork` (in-thread GC via
   `OptimizeForManyInstances`; no per-agent recycler threads), and
   `QuitCallback` then performed an orderly agent shutdown — signal every
   child's shutdown event and join chunked (≤64 handles) under a 10 s
   bound — before `ExitProcess`. That join design was **superseded in
   iteration 3** by the raw-exit semantics below (it still crashed for
   agent-initiated Quit, wedged agents, and residually under parallel
   load); the DisableBackgroundWork half stands unchanged.
2. **Pre-share waiter was deaf.** A `waitAsync` registered before the first
   `JsGetSharedArrayBufferContent` had `monitored == false` forever:
   foreign notifies returned 0 and its promise stayed pending. On the
   FIRST extraction (`MarkCrossRuntime` now reports the transition),
   `JavascriptSharedArrayBuffer::MonitorPendingAsyncWaiters` walks the
   wrapper's pending unmonitored records and retroactively schedules their
   monitor tasks — create + enqueue first, then publish `monitored` (and
   cache the token links, which only finite-timeout registrations had)
   under the waiter-list CS. Exactly-once and OOM behavior match
   `EntryWaitAsync`: task-creation OOM propagates and leaves the waiter
   (and the rest of the walk) on the S6b skip-and-uncount contract. The
   first extraction runs before any child wrapper exists, so the walk is
   on the registrant's own thread and may safely touch the records.
3. **Binary identity.** All iteration-2 corpus/stress/gate evidence below
   comes from one recorded final identity: launcher
   `zig-out/bin/ch` SHA-256 `c03eea6e…528`, `libChakraCore.dylib`
   SHA-256 `cef46d17…73a`, dylib UUID `6832F606-73DE-3A66-97A9-6A53082F3109`
   (the `ch` launcher embeds no engine code; engine changes move only the
   dylib, so launcher UUIDs repeat across engine builds). The iteration-1
   UUID labels above identify the iteration-1 binaries only; iteration-1
   corpus counts are superseded by the final-binary counts below.
4. **Comments/residuals corrected**: `Abandon` documented as an
   unreachable defensive tail that never releases the token (release stays
   with registrant-thread claims/finalization); the finalizer's
   read-`linked`-under-CS / delete-outside contract is stated at both
   sites; monitor self-reschedule OOM and the 5 s stuck-agent consequence
   are recorded as residuals (below).

## Review repair, iteration 3 (teardown-only: raw exit for `WScript.Quit`)

The iteration-2 join-then-`ExitProcess` design still crashed three ways,
all rooted in the same fact: **`ExitProcess`/`exit` run the PAL
process-detach path** (`JsrtRuntime::Uninitialize`-driven runtime
finalization, DLL detach, C++/TLS destructors) over runtimes whose threads
were still inside JsRT calls.

1. **Agent-initiated Quit** (`n7_agent_quit`: one agent of 8 calls
   `WScript.Quit(7)` mid-flight). The join only ever covered the *calling*
   thread's own sub-agents — never the main thread or sibling agents,
   which `ExitProcess` then hard-terminated mid-JsRT-call before the
   detach path walked their runtimes: **SIGSEGV 18/20** on the final
   iteration-2 binary (test profile, parallel 6).
2. **Wedged agent** (`n8_wedged_agent_quit`: one agent of 6 inside
   `while(true){}`, main calls `Quit(0)`). The bounded join expired after
   ~10 s and the same detach path ran over the wedged runtime:
   **SIGSEGV 8/8**.
3. **Residual SIGBUS under parallel load** in the flagship teardown
   families (`n6_quit_live_agents` 1/25, `n10_timed_leaving` 1/25,
   `n12_interleave` 1/25 test + 1/5 debug) — iteration 2 attributed these
   to "the same detach path racing agents that were mid-JsRT while the
   join window closed". **That attribution was wrong** (see the
   post-landing diagnosis below): a signal-handler capture proved the
   residual SIGBUS is a pre-existing agent-*startup* race in the engine
   (concurrent `JsCreateContext`), firing before any Quit and identically
   on the iteration-2 host — not an exit-path crash at all.

Design (`bin/ch/WScriptJsrt.cpp`, `RawExitProcess`): `WScript.Quit`
attempts **no runtime teardown at all** anymore. It flushes `stdout` and
`stderr` (`PAL_fflush` writes the underlying `FILE` buffers
synchronously; `Echo` already flushes per line) and then terminates
through the raw syscall path — POSIX: `::_exit(exitCode & 0xff)`, which
runs no PAL cleanup, no atexit handlers, no C++/TLS destructors, no DLL
or process detach notifications and no runtime finalizers
(`pal.h` remaps `exit` but not `_exit`; the explicit `& 0xff` documents
the `wait()`-observable mask). Sibling and wedged agent threads simply
stop existing; joining is unnecessary by construction (and unjoinable in
the agent-initiated case, where the quitter never owned the main thread
or its siblings), so no signal phase is retained either — a best-effort
root-tree walk would only add a new unsynchronized read of other
threads' `children` lists microseconds before the process dies. Windows
preserves the host's historical `ExitProcess` semantics (the adversarial
campaign is POSIX/macOS only). Exit-code exactness is unchanged: probe
`WScript.Quit(0|1|3|7|42|255|-1)` → `0|1|3|7|42|255|255` (negative codes
mask, as before), and the rlexe gates' `WScript.Quit(0/1)` tests keep
their exact codes.

## Iteration 3 post-landing diagnosis: the residual SIGBUS is a
pre-existing agent-startup race, not an exit-path crash

After landing the raw-exit design, a fresh teardown campaign (below)
still observed a rare SIGBUS (~0.3%) across the teardown families —
but with **no** `.ips` crash reports and **no** `ReportCrash`
activity. A `DYLD_INSERT_LIBRARIES` interposer installing a
`SA_SIGINFO` SIGBUS/SIGSEGV/SIGTRAP handler (the PAL chains unhandled
faults to the previous handler) captured it on demand:

```text
SIGBUS si_code 1 (KERN_INVALID_ADDRESS), fault addr 0x30005c000
Js::JavascriptStringObject::JavascriptStringObject(...)  JavascriptStringObject.cpp:36
Js::JavascriptLibrary::InitializePrototypes()            JavascriptLibrary.cpp:289
Js::JavascriptLibrary::Initialize -> GlobalObject::Initialize
ScriptContext::Initialize -> JsrtContextCore::EnsureScriptContext
JsCreateContext
RuntimeThreadData::ThreadProc +140      (agent thread)
```

The faulting thread is an **agent still inside `JsCreateContext`** —
before it can register, before the parent's registration barrier can
pass, therefore strictly **before any `WScript.Quit` can run**. Three
independent confirmations:

1. **Same-engine/old-host reproduction.** The iteration-era host
   binary (`out/s6e-bins/ch-test`, join-then-`ExitProcess` design)
   running the identical, engine-unchanged `libChakraCore.dylib`
   (SHA `cef46d17…73a` — iteration 3 changed only `bin/ch` host code)
   reproduces the *same* fault, same frame, same fixed address
   (`0x30005c000`): 1 REALFAULT / 400 vs the new host's 3/400 under
   the same driver. The teardown design is irrelevant to this fault.
2. **No-Quit variant.** `n6noquit` (8 agents, identical startup, main
   spins forever — the driver SIGKILLs, so **no exit path ever runs**):
   with a churn-matched 60 ms kill the same captured fault fires
   **3/600** with the identical backtrace (fault addresses
   `0x30005c000` and `0xa8c05c000` — same `+0x5c000` page offset in
   different segments); with a 1.5 s kill it fires 0/400, i.e. the race
   scales with agent-startup churn, not with any exit behavior.
3. **Timing displacement.** 300 `lldb --batch` runs of the worst
   families caught nothing (ptrace changes thread startup timing),
   matching a scheduling-sensitive allocation race at context init.

Root cause is engine-side (concurrent `ScriptContext::Initialize` /
prototype allocation racing across agent threads — a fixed
page-aligned fault address suggests the recycler/page-allocator
segment path); it predates this slice's host change and is **out of
scope for the teardown-only repair**. What the raw-exit change did
remove is every *exit-path* crash: the deterministic families of
iteration 2 (`n7` agent-initiated Quit SIGSEGV 18/20, `n8` wedged
agent 8/8, `n6` Quit-with-live-agents 17–21/25) are all clean now —
`n7` exits **exactly 7 in 180/180** runs, `n8` exits 0 immediately in
126/126 (no 10 s join bound left to burn), and every remaining signal
in 1255 campaign runs is this startup race. Recorded as a residual
with the engine backtrace; a future engine slice should own it.

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

## Evidence (final binary — iteration 2)

Mac ARM64 (aarch64-apple-macos.26.0), test profile (RelWithDebInfo, no
JIT) and debug-static, shared gate lock, pinned test262
`2e0a56762801e275a9fdf96dc49d90ba0cddcf63`, 20 controls passed per run,
Node **v26.5.0**, jobs 6, timeout 10 s, host flags
`-ESBigInt -ESSharedArrayBuffer -nonative -Test262 -CollectGarbage`.
Final identity (all counts below): launcher SHA-256 `c03eea6e…528`,
dylib SHA-256 `cef46d17…73a`, dylib UUID `6832F606-73DE-3A66-97A9-6A53082F3109`
(debug-static `out/debug-static/bin/ch` SHA-256 `99a20675…5c`). The `ch`
launcher embeds no engine code; engine changes move only the dylib, so
launcher UUIDs repeat across engine builds.

- Builds: test **568/568**, debug-static **546/546**.
- **Crash repro loops (review scripts, zero signals)**: before the fixes,
  `gc_pressure` crashed **17/20** on the then-current binary (reproduced on
  demand; minimized to "Quit with live agents" — single-agent and no-Quit
  variants were clean, `CollectGarbage` irrelevant). After both fixes:
  12 reviewer scripts (`gc_pressure`, `gc_pressure_dbg`, `throw_exit`,
  `broadcast_chaos`, `teardown_storm`, `stall_G5`, `shutdown_mainwaiter`,
  `race_orig`, `agents12_notifyall`, `agents12_dbg`, `child_to_child`,
  `uaf_timeout_race`) × 15 = **180/180 clean** on the final test binary,
  plus the five worst × 30 = **150/150 clean** on the Quit-fix build, plus
  `gc_pressure_dbg` × 12 = **12/12 clean** on the final debug-static
  binary — only exits 0/1, no signals, no new `.ips` reports.
- Focused `test/es7/s6-waitasync-cross-agent.js` — **11 scenario groups**
  (iteration 1's 10 plus: pre-share Int32 waiter registered before the
  first broadcast — agent notify returns 1 and resolves `"ok"`; pre-share
  BigInt64 twin; pre-share 300 ms finite waiter — the timeout still wins
  after the share; post-share control waiter) — **pass** on the test
  binary (registered ×3, `-nodeferparse`, `-force:deferparse`, no-GC) and
  on `out/debug-static/bin/ch` (registered, `-nodeferparse`).
- S6b/S6D `s6-waitasync-pause.js`, S6c `s6-bigint-atomics.js`, adjacent
  es7 (`s6-growable-nonshared` 13/13, `atomics_test` 16/16,
  `sharedarraybuffer` 7/7, `bigint64array` 10/10): **pass** on both
  profiles. Default-off probes unchanged (`SAB=undefined
  Atomics=undefined` with no flags / `-ESBigInt` alone).
- **Exact `Atomics/waitAsync/**` (202 scenarios), two runs**: **201/1**
  (79 fail→pass / 0 pass→fail) and **199/3** (77 fail→pass / 0 pass→fail)
  vs base 122/80; every residual in both runs is the
  `no-spurious-wakeup-*` witness race.
- **Atomics + SAB 982**: Atomics `**` **691/83** (76 fail→pass /
  0 pass→fail vs base 615/159), `SharedArrayBuffer/**` **204/4** (the
  same 4 documented constructor-order scenarios). Merged **895/87**.
- Gates (expected-pin overrides; pins deliberately not edited):
  `check_regression.py --select test --expected-passed 2031` →
  **PASS 2031/0**; `--select debug` (on `out/debug-static/bin/ch`)
  `--expected-passed 2083` → **PASS 2083/0**. Pin move is exactly the one
  new rlexe entry (test 2030→2031, debug 2082→2083).

## Evidence (final binaries — iteration 3, teardown-only repair)

Iteration 3 changes only host code (`bin/ch/WScriptJsrt.cpp`), so the
engine dylib identity is unchanged (SHA `cef46d17…73a`, UUID
`6832F606-73DE-3A66-97A9-6A53082F3109`); the test launcher moved to
SHA-256 `f13a52f1…f35f` (`zig-out/bin/ch`) and debug-static to
`3c4b6d56…e9c0` (`out/debug-static/bin/ch`); both rebuilt clean
(`zig build` / `zig build -Dprofile=debug-static`, up-to-date
no-ops; prior full-build gates 568/568 and 546/546 stand).

- **Exit-code exactness, both binaries** (`-ESBigInt
  -ESSharedArrayBuffer`): `WScript.Quit(0|1|3|7|42|255|-1)` →
  `0|1|3|7|42|255|255`; `print("x");Quit(5)` prints `x` then exits 5
  (flush-before-exit verified).
- **Teardown campaign** (`drive4.mjs`, seq PAR=1 ×7 rounds and par
  PAR=6 ×4 rounds; families n6/n7/n8/n10/n12 + focused + pause +
  bigint-atomics; alternating `-CollectGarbage`): **1251 pass, 0 fail,
  0 hang**, every exit code exact (`n7` 180/180 exit **7**, `n8`
  126/126 immediate exit 0 — no join bound), 4 SIGBUS (~0.3%:
  n6-test, n7-test, n8-debug, n12-test) — **all four identified as
  the pre-existing agent-startup race** above (interposer capture,
  old-host reproduction, no-Quit variant); wall ≈37 s/seq round,
  ≈15 s/par round.
- **Focused suites, both profiles** (`runtests.py --test|--debug
  --variants=disable_jit`, per-test rlexe flags):
  `s6-waitasync-cross-agent`, `s6-waitasync-pause` (S6b pause +
  waitAsync, S6d finite timeouts), `s6-bigint-atomics` (S6c),
  `s6-growable-nonshared`, `atomics_test`, `sharedarraybuffer`,
  `bigint64array` → **7/7 test, 7/7 debug**. Cross-agent variant
  matrix additionally all `pass`/exit 0: test (registered ×3,
  `-nodeferparse`, `-force:deferparse`, no-GC) and debug
  (registered, `-nodeferparse`).
- **Interposer diagnosis campaign** (node-spawn,
  `DYLD_INSERT_LIBRARIES` handler): new host `n6` 3 REALFAULT/400
  (backtrace above); iteration-era host + same dylib 1/400 identical
  backtrace/address; `lldb --batch` 300 runs of n6/n8/n7 clean
  (timing displacement). No-Quit `n6noquit` (churn-matched 60 ms
  driver kill): 3 REALFAULT/600, identical backtrace — see the
  residual below.

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
- **Monitor self-reschedule OOM (host-queue failure)**: the monitor's
  self re-enqueue hands the existing task to the host timer callback
  again; that is a host-side allocation (`new CallbackMessage` in ch,
  which fatally exits on failure). A hypothetical non-fatal host that
  drops the reschedule would leave a `monitored` waiter whose monitor is
  dead: later foreign notifies would count and latch it with nobody to
  resolve — bounded by the host contract that the timer callback must
  accept the task.
- **5-second stuck-agent handshake consequence**: if a *live* agent never
  acknowledges a broadcast within 5 s (wedged in native code before its
  consumed-event SetEvent, or starved past the bound), the parent
  proceeds with a stderr warning; if that agent later wakes during a
  *subsequent* broadcast it may consume the newer round's content (wrong
  buffer for its `receiveBroadcast`). Bounded to the >5 s-starvation case;
  the pre-fix code had the equivalent window on every multi-child
  broadcast.
- **Pre-share waiters on sibling wrappers**: retro-monitoring (iteration 2)
  covers the wrapper being extracted at the first share — including a
  pre-share **main-thread** `waitAsync`, whose waiters are retro-monitored
  when the first `JsGetSharedArrayBufferContent` marks the contents
  cross-runtime (the earlier claim that pre-share main-thread waiters stay
  unmonitored was stale and is removed). The only residual is a pending
  waiter on a *different* wrapper over the same contents inside the
  extracting runtime (only constructible via
  `JsCreateSharedArrayBufferWithSharedContent` into the same runtime):
  those are not retro-monitored and keep the S6b skip-and-uncount
  contract.
- **Remaining shared-host global state**: `WScriptJsrt::GetNextSourceContext`
  (a plain static counter) is incremented from agent threads — a benign
  duplicate-source-context race, unchanged from the pre-S6e host; module
  loads inside an agent still push to the main queue (`PushMessage`). Both
  pre-existing, unexercised by test262 agents.
- **Polling latency**: a foreign wake is observed within one poll interval
  (2 ms) plus host sleep granularity — the host timer contract's few-ms
  early/late fire (S6d) applies. No fairness/starvation claim beyond FIFO
  list order.
- **Rare SIGBUS at agent startup (pre-existing engine race, out of
  scope)**: concurrent `JsCreateContext` → `ScriptContext::Initialize` →
  `JavascriptLibrary::InitializePrototypes` → `JavascriptStringObject`
  ctor (JavascriptStringObject.cpp:36) faults on a fixed page offset
  (`0x30005c000`, also `0xa8c05c000` — `+0x5c000` in different
  segments) with si_code 1, ~0.3% under the teardown campaign and
  ~0.5–0.75% under the interposer driver. Provably independent of
  `WScript.Quit` (fires before the registration barrier; reproduces on
  the iteration-2 host with the same dylib; reproduces 3/600 with no
  Quit at all — `n6noquit`, main spins forever, driver kill only). Produces no `.ips` (ReportCrash
  silent) but is caught by a chained SIGBUS handler. Needs an
  engine-side owner (page-allocator/recycler segment race at context
  init); not worsened by the raw-exit change.
- **`WScript.Quit` performs no runtime teardown** (iteration 3, by
  design): raw `_exit` skips `JsDisposeRuntime`, agent queue teardown,
  finalizers and PAL cleanup for every live runtime — the deliberate
  trade that removed the teardown crash families. Two consequences:
  (a) output written by an agent thread concurrently with a Quit may be
  truncated mid-line if Quit's flush wins the race (the quitting thread
  flushes synchronously; `Echo` flushes per line, so complete lines are
  safe); (b) recycler-owned resources are reclaimed by process death,
  not by GC — fine for the `ch` test host, and the reason real embedders
  should dispose runtimes before exit rather than call `Quit` with live
  agents.
- **TTD**: no delayed tasks (hence no monitors) while recording/replaying;
  cross-agent waitAsync stays pending — the documented S6d stance.
- **Finalizer lock risk** (S6b, unchanged): `CancelAsyncWaiters` takes the
  waiter-list CS; a stop-the-world GC suspending a foreign thread inside
  `csForAccess` could stall finalization. Not worsened by this slice (the
  foreign notify holds the CS no longer than before).
- **JIT/B4**: no bytecode/JIT surface changed; evidence is Mac ARM64 no-JIT.
  B4 must repeat the focused tests and the two test262 selections on native
  Linux and JIT profiles.
