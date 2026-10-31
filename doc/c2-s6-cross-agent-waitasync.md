# C2-S6e: cross-agent Atomics.waitAsync (flag-on slice)

Status: **landed** — implementation `d972c53fe`, review repair `e547fa345`
(docs `6c319e065`… `05d70b805`), final repair iteration 2
(`e23fc6f6b`), teardown-only iteration 3 (`WScript.Quit` raw exit;
`a78a5dba4`, with the post-landing SIGBUS diagnosis and fresh campaign
evidence below) on
branch `s6-cross-agent-waitasync` (base `f305a175f`); evidence below. A
docs-only receipt cleanup after `a78a5dba4` (no runtime change) copies the
reviewer's final gate logs under `out/gate-receipt/` and the adversarial
campaign under `out/s6e-teardown-review/` (both git-ignored evidence) and
corrects the evidence labels below. A later conformance repair (branch
`s6e-fifo-resolution`, base `3574b6168` = main: diagnosis `533e41d37`,
fix `d99ab67c0`) fixes foreign-notify-pass FIFO **resolution** order —
see "S6e conformance repair" below. A later host teardown repair
(branch `host-exit-join`, base `84bda26de` = main: diagnosis
`d640a56e6`, fix `17c05c557`) fixes the iteration-3 end-of-main join
over >64 accumulated agents — see "S6e host teardown repair" below.
`ESSharedArrayBuffer` (and `ESBigInt` for the BigInt64 paths)
remain **off by default** (BigInt itself is default-on since S2); this slice
changes no default. It supersedes the cross-agent residual of
[c2-s6-waitasync.md](c2-s6-waitasync.md) (S6b); S6d finite timeouts are
unchanged and now arbitrate with the new wake path.

Landing: merged to main in `6900598e4` (merging `866a03c82`). Post-merge
main gates: `zig build check` **583/583** with test regression
**2031/0**, and debug-static regression **2083/0**;
`tools/build/gate_counts.json` pins moved 2030/2082 → 2031/2083 in the
landing commit, adding the single `s6-waitasync-cross-agent` rlexe entry.

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
   (concurrent `JsCreateContext`), firing before any Quit and
   identically on the intermediate-snapshot comparison host — not an
   exit-path crash at all.

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

1. **Same-engine/old-host reproduction.** The comparison host is an
   **unlabeled intermediate snapshot, not the final iteration-2
   launcher**: SHA-256 `29dba05e…8a4` (preserved as
   `out/s6e-bins/ch-test`, byte-identical to the mislabeled
   `out/s6e-bins/ch-debug-static` copy; UUID
   `A84219BA-9D9A-3762-B057-0A3643C02BCD`). It matches neither the
   recorded iteration-1 binary nor the final iteration-2 launcher
   (`c03eea6e…528`), and no commit label was recorded for it, so its
   exact source tree is unknown (the earlier
   "join-then-`ExitProcess` design" label for this binary is
   retracted). The snapshot preserved no dylib, so the engine dylib
   had to be relocated next to it (copied from `zig-out`, SHA
   `cef46d17…73a` — iteration 3 changed only `bin/ch` host code; the
   `ch` launcher loads the dylib from its own directory). Running
   that snapshot host against the identical, engine-unchanged dylib
   reproduces the *same* fault, same frame, same fixed address
   (`0x30005c000`): 1 REALFAULT / 400 vs the new host's 3/400 under
   the same driver. This proves the fault independent of the
   teardown host change; it does not establish the snapshot host's
   own Quit design.
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
scope for the teardown-only repair**. Proven so far: independent of
the teardown host change and of the current dylib; **not** proven
absent from the entire S6e engine branch at current test power (the
pre-S6e base host has been churn-probed only at the weak 8-agent/
60 ms setting — 0/200 — never at the sharp 16-agent setting; see the
residual below). What the raw-exit change did
remove is every *exit-path* crash: the deterministic families of
iteration 2 (`n7` agent-initiated Quit SIGSEGV 18/20, `n8` wedged
agent 8/8, `n6` Quit-with-live-agents 17–21/25) are all clean now —
`n7` exits **exactly 7 in 180/180** runs, `n8` exits 0 immediately in
126/126 (no 10 s join bound left to burn), and every remaining signal
in 1255 campaign runs is this startup race. Recorded as a residual
with the engine backtrace; a future engine slice should own it.

## S6e conformance repair: FIFO resolution order across a foreign notify pass

Status: diagnosis `533e41d37`, fix `d99ab67c0`, receipt (this section)
docs-only commit, branch `s6e-fifo-resolution` (base `3574b6168` = main).

### Root cause

The Design section below claimed "FIFO order is preserved because foreign
entries are consumed in waiter-list order like same-thread ones". That was
true at LATCH time but not at RESOLUTION time: each foreign-latched
waiter's promise is resolved by its OWN registrant-thread monitor task
(`AtomicsWaitAsyncMonitorTaskFunction`, 2 ms self-rescheduling poll), and
two monitors whose deadlines land in the same host tick can fire in either
order on the registrant's queue — waiter 2 can resolve before waiter 1.
ES2025 `NotifyWaiters` fulfills woken waiters in list order, and one
registrant thread observes both resolutions through its single microtask
queue, so the inversion is observable through the agent's `.then`
reactions. Structural constraint (why the waiter list cannot fix it): at
foreign-notify time the token is unlinked from the waiter list as it is
latched (that is what the latch means), so a firing monitor cannot
discover earlier latched siblings there. Ordering must be recorded AT
LATCH TIME — the only moment the pass's tokens are all visible in FIFO
order — while claims and resolutions stay on the registrant's thread (the
ownership invariant: the foreign notifier never frees or resolves tokens).

Reproducer `/tmp/opencode/e1-flake/s6fifo.js` (minimal extraction of
scenario 6), debug-static, 6-way parallel, flags
`-nonative -ESBigInt -ESSharedArrayBuffer -Test262 -CollectGarbage`:
**30/2400 = 1.25% `6:B,A` inversions** at base `3574b6168` (binary
SHA-256 `633049b0…a39`, preserved as `out/s6e-fifo-bins/ch-pre-debug`);
the original E1-session observation on main's debug binary was 19/2400 =
0.79%. **Profile skew**: main's test-profile binary measures **0/1200**
— slower execution widens the window where two monitor deadlines land in
the same tick — so the flap surfaces in the DEBUG gate, and the debug
gate is the sharp detector for this defect (test-profile post-fix runs
prove little at these rates).

### Design

Chosen: **per-notify-pass ordered chain + head-first monitor claims**
(candidate A). Candidate B (ordered batch handed to the registrant's
queue) was rejected: it allocates on the notify hot path and adds a new
JsRT-boundary message type/rooting chain — larger risk against the same
locked invariants.

- `WaiterList::RemoveAndWakeWaiters` chains the foreign tokens a notify
  pass latches into a per-pass doubly-linked list (`passPrev`/`passNext`
  on the native token), stamped at latch time under the waiter-list CS,
  in latch (waiter-list FIFO) order.
- `AtomicsAsyncWaiter::TryClaimWakeInPassOrder` (registrant thread, under
  the same CS): walk from the chain head and claim the first unsettled
  same-`ThreadContext` member. If it is a FIFO predecessor, resolve it
  `"ok"` (outside the CS) and immediately re-enqueue this monitor at
  **delay 0** — due now, queued after the promise reactions the
  resolution just enqueued (the ch queue runs equal-due-time messages in
  insertion order, and promise jobs are themselves delay-0 messages), so
  those reactions run first and the next firing advances the chain; the
  walk claims at most one waiter per firing and the chain strictly
  shortens, so it terminates. If the head-reachable member is the waiter
  itself, behavior is identical to the pre-repair claim (the common
  single-waiter case costs nothing extra). Other-`ThreadContext` members
  are skipped by native-field compare only: cross-agent relative order is
  unobservable (each agent drains its own microtask queue), and their own
  monitors claim them on their own threads.
- `TryClaimForWake` split into a CS-taking wrapper +
  `TryClaimForWakeUnderCS` so the walk can claim predecessors while
  holding the (recursive) CS.

Unchanged (locked semantics): wake counts, exactly-once `settled`
arbitration, timeout-vs-wake arbitration (a timeout that wins for a
latched waiter resolves `"timed-out"` — the spec's concurrent-timeout
ambiguity — and simply splices out of the chain), pre-share
retro-monitoring, and the S6d timer design.

### Ownership deltas (memory-management record)

```text
Resource: passPrev/passNext chain links on the native
          AtomicsAsyncWaiterRegistration token (+ SpliceFromPassChain)
Creation/first write: stamped by the foreign notify pass in
          WaiterList::RemoveAndWakeWaiters at latch time, under the
          waiter-list CS; the token itself is still HeapNew'd in
          JavascriptSharedArrayBuffer::AddAsyncWaiter on the registrant
          thread (unchanged S6b/S6e ownership: wrapper-owned, released
          by registrant-thread claims or wrapper finalization only)
Domain: native pointer fields on the HeapAllocator-owned token — no new
          allocation, no GC-visible field, no new root, no new lock
Owner/transfer: unchanged; the foreign notifier still never frees the
          token and never dereferences record
Invariants: latched && !settled => token is in its pass chain; all link
          reads/writes under the waiter-list CS of the list the pass ran
          on; every release path (TryClaimForWakeUnderCS,
          TryClaimForTimeout, CancelAsyncWaiters) splices the token out
          under that same CS before HeapDelete, so walkers only see live
          tokens. The one write to an already-latched token (the next
          sibling's passNext stamp) happens inside the notifier's single
          CS hold — every observer of linked == false or deleter of the
          token must first acquire that CS — extending (not breaking)
          the S6e iteration-1 token-latch ordering invariant
Thread/context: stamping on the notifying thread under CS; walk/claim/
          splice on the registrant thread (record fields touched only
          after a native threadContext compare matches); finalizer splice
          on the finalizing runtime thread under the same CS
Borrow invalidation: splice at settle/release (passPrev/passNext =
          nullptr); token HeapDelete follows immediately after
Failure paths: no new fallible operation — no allocation and no host
          call under the CS; the delay-0 re-enqueue is outside the CS
          and inherits the documented monitor self-reschedule OOM
          residual
Tests: test/es7/s6-waitasync-cross-agent.js scenarios 12-13; reproducer
          A/B campaign below
```

### Evidence (S6e FIFO repair)

Mac ARM64 (aarch64-apple-macos.26.0), debug-static + test profiles,
shared gate lock. Reproducer `/tmp/opencode/e1-flake/s6fifo.js`, 6-way
parallel unless noted, flags as above. Binaries (SHA-256): pre-fix
debug `633049b0…a39` (`out/s6e-fifo-bins/ch-pre-debug`), post-fix debug
`b4f18d8e…468` (`out/s6e-fifo-bins/ch-post-debug`), post-fix test
`0d917b2f…c61` (`zig-out/bin/ch`).

| Leg | Binary | Runs | Par | `6:B,A` inversions | Crashes |
|---|---|---|---|---|---|
| Pre-fix (A) | base `3574b6168` debug | 2400 | 6 | **30 (1.25%)** | 0 |
| Post-fix (B) | fix `d99ab67c0` debug | 2400 | 6 | **0** | 0 |
| Post-fix quiet-rate | fix `d99ab67c0` debug | 600 | 1 | **0** | 0 |

- Deterministic scenarios in the gate file: 12 (four waiters, one agent,
  one `notify(4)` pass → `12:A,B,C,D`) and 13 (two agents × two waiters,
  one mixed pass → per-agent `13:1,2`; exercises foreign-token skipping)
  — full-file direct runs (scenarios 6, 12 and 13 all assert FIFO order):
  **30/30 clean on post-fix debug, 30/30 on post-fix test**.
- Focused suites (`runtests.py --test|--debug --variants=disable_jit`):
  s6-waitasync-cross-agent, s6-waitasync-pause (S6b/S6d),
  s6-bigint-atomics (S6c), s6-growable-nonshared, atomics_test,
  sharedarraybuffer, bigint64array → **7/7 test, 7/7 debug**.
- Cross-agent variant matrix: test (registered ×3, `-nodeferparse`,
  `-force:deferparse`, no-GC) and debug (registered, `-nodeferparse`,
  no-GC) — all `pass`/exit 0.
- Builds: debug-static **546/546**, test **568/568**;
  `zig build check` **583/583**, `zig build check -Dprofile=debug-static`
  **560/560**.
- Gates: check_regression `--select test --expected-passed 2031` →
  **PASS 2031/0**; `--select debug --expected-passed 2083` →
  **PASS 2083/0** (pins in `tools/build/gate_counts.json` untouched at
  2031/2083; scenarios 12-13 extend the existing rlexe entry, count
  unchanged).

### Residuals (S6e FIFO repair)

- **Chained-pass resolution latency**: a pass latching k same-registrant
  waiters resolves them across k monitor firings (one claim each, delay-0
  re-enqueue) — microseconds per link in-process, unobservable next to
  the 2 ms poll interval; the first waiter still resolves within one
  poll interval of the notify.
- **Delay-0 re-enqueue OOM**: shares the monitor self-reschedule host
  contract and its documented residual (a host that drops it leaves the
  chain's later waiters latched but unresolved — same class as the
  pre-repair dead-monitor residual).
- **Cross-agent relative order remains unspecified by design** (the walk
  skips other-registrant tokens); same-thread vs foreign waiters within
  one pass also keep their pre-repair relative timing (same-thread
  handoffs resolve synchronously inside the notify call, foreign ones
  within one poll interval) — unobservable in a single runtime's job
  queue.
- **Finalizer CS hold**: CancelAsyncWaiters now splices under the CS it
  already took (no longer hold); walkers hold the CS for O(pass size)
  native pointer hops — the same order the notify pass itself holds it.
- Profile skew above: post-fix test-profile reproducer legs are
  uninformative (pre-fix baseline is 0/1200 there); the debug gate
  remains the detector.
- **Pre-existing exit assert under very-high agent churn (adversarial
  review; separate follow-up card)**: `ASSERTION (WScriptJsrt.cpp:1345)
  waitRet == 0 waitRet == WAIT_OBJECT_0` — the end-of-main
  `WaitForMultipleObjects` over the accumulated `threadData->children`
  handles fails after ~100+ leave-completed agents in one process.
  Reproduced 3/3 identically on the pre-fix AND post-fix binaries
  (introduced by the iteration-3 host teardown, not by this repair);
  the committed test (~15 agents) and both gates never hit it; all
  in-process oracles complete before the assert, so data is unaffected.
  **Fixed** by the host-exit-join repair below (branch `host-exit-join`,
  base `84bda26de`): pre-fix it was worse than an assert — on test
  profiles the same 100-agent repro SIGSEGV'd 33/50 (delete loop freeing
  `RuntimeThreadData` whose threads were never observed exiting).

### Review outcome (FIFO repair)

Both reviewers **ACCEPTED** with zero `[bug]` findings.

- **General review**: chain stamping inside the notify's single CS hold,
  splice-before-delete verified on all three release paths, JsRT boundary
  respected (other-registrant tokens touched by native fields only),
  delay-0 ordering proven against `MessageQueue.h`'s insert-after-equals
  semantics, termination proven (monotonically shortening chain),
  ownership record compliant with `doc/memory-management.md`.
  Independently re-measured: pre 7/600 inversions vs post 0/600;
  debug gate **PASS 2083/0**.
- **Adversarial review**: **7,800 pass-observations / 41,600
  waiter-observations with 0 FIFO violations and 0 hangs** (4/8/16
  waiters, full/split notify modes, racing dual notifiers with exact
  totals); timeout×notify poisoning (1,600 waiters: 0 double-resolves,
  0 missed, survivors FIFO); GC churn and leave/teardown races clean;
  pre-share retro path ordered; 32-waiter chains terminate ≤27 ms under
  load; no fix-attributable leak (RSS slopes equal pre/post); both gates
  **PASS 2031/0 and 2083/0** with pins unedited. Ordering is structural
  (claims serialize on the registrant thread and require no unsettled
  same-registrant predecessor), not scheduler luck. Its one `[risk]` is
  the pre-existing exit assert recorded above.

## S6e host teardown repair: bounded end-of-main join (host-exit-join)

Status: diagnosis + repro tooling `d640a56e6`, fix `17c05c557`, receipt
(this section), branch `host-exit-join` (base `84bda26de` = main, gates
2031/2083). Retires the "pre-existing exit assert under very-high agent
churn" residual recorded in the FIFO-repair residuals above.

### Root cause

`WScriptJsrt::Uninitialize`'s end-of-main join made **one**
`WaitForMultipleObjects(count, ...)` call over every handle in
`threadData->children`, then `Assert(waitRet == WAIT_OBJECT_0)` and
unconditionally deleted every child. The PAL rejects `nCount >
MAXIMUM_WAIT_OBJECTS` (64) (`pal/src/synchmgr/wait.cpp`), and the
children list only ever grows before that point — established lifecycle
facts: children are `push_back`'d on the owning parent thread in
`LoadScriptCallback` *before* `CreateThread`; `leaving()` never unlinks
a child (it only sets the child's own flag, and a leaving child still
parks in its wait loop until shutdown/broadcast); the sole removal is
the `clear()` in the join block itself. So a process that starts more
than 64 agents joins 65+ handles: debug profiles assert (the residual
above), and release/test profiles fall through to the delete loop with
**no thread having been observed exiting** — freeing `RuntimeThreadData`
objects (events, `hThread`, `messageQueue`, `csReportQ`) their threads
still touch. A use-after-free, not just a bad exit code: the 100-agent
repro SIGSEGV'd 33/50 pre-fix on the test profile.

### Design

Three passes over `threadData->children`, all on the owning parent
thread: `SetEvent(child->hevntShutdown)` for every child **before any
waiting** (unchanged); `WaitForSingleObject(child->hThread, INFINITE)`
per child (each handle individually waited — no `nCount` limit, so the
64-handle PAL cap is irrelevant); only after every child was
individually observed exited, `delete` each and `clear()`. The list is
iterated unlocked because it is mutated only on this same thread (facts
above); children never mutate the parent's list. Semantics for ≤64
children are identical to the old code (signal-all, join-all,
delete-all); no engine change; `WScript.Quit` raw-exit and agent
semantics untouched. Per-child waits also pinpoint a failing handle in
the per-child `Assert` instead of one aggregate.

### Ownership record (memory-management)

```text
Resource: RuntimeThreadData objects in threadData->children (host-side
          new/delete; events, hThread, messageQueue, csReportQ inside)
Owner: the owning parent thread's children list — unchanged; sole
          release authority remains this Uninitialize block (delete +
          clear), now strictly after each child's thread was observed
          exited via its waited handle
Transfer/invalidation: child->parent valid from push_back (parent
          thread, before CreateThread) until the parent's delete;
          no child ever touches the parent's list or another child
Thread rule: signal/wait/delete all on the parent thread; the child's
          last touch of its own RuntimeThreadData precedes its thread
          exit, which the parent observes through the handle wait
Failure paths: a WAIT_FAILED (invalid handle) trips the per-child
          Assert on debug profiles — same contract as before; the
          ASSERTION output identifies the source line inside the loop
          (pinpointing the exact child requires a debugger stop with the
          iterator in scope); release profiles keep the pre-existing
          unconditional-delete shape
```

### Evidence

Mac ARM64, test profile (`zig-out/bin/ch`) and debug-static
(`out/debug-static/bin/ch`), repro `tools/host-exit-join/agents100.js`
(100 agents: 50 report+`leaving()`, 50 just complete their initial
script — both classes park and remain in `children`; main then ends),
driver `tools/host-exit-join/drive.sh`. Binaries (SHA-256): pre-fix
test `ea6e3f7c…9be` (preserved `out/host-exit-join-bins/ch-pre-test`),
pre-fix debug `291ba391…d66` (`ch-pre-debug`, static), post-fix debug
`df5d20f2…045` (`ch-post-debug`), post-fix test `896b8b8f…840`
(zig-out; dylib `6c63d966…5b`, unchanged by this host-only fix).

| Leg | Binary | Runs | Result |
|---|---|---|---|
| Pre-fix test | base `84bda26de` | 50 | 17 clean / **33 SIGSEGV** (one SIGABRT) |
| Pre-fix debug | base `84bda26de` | 10 | **10/10 `ASSERTION (WScriptJsrt.cpp:1345)`** (exit 133) |
| Post-fix debug | fix `17c05c557` | 50 | **50/50 clean exit 0** |
| Post-fix test | fix `17c05c557` | 50 | **50/50 clean exit 0** |

- Focused S6e suite (`runtests.py -t|-d --variants=disable_jit`):
  s6-waitasync-cross-agent, s6-waitasync-pause, s6-bigint-atomics,
  s6-growable-nonshared, atomics_test, sharedarraybuffer, bigint64array
  → **7/7 test, 7/7 debug**.
- Gates: `check_regression.py --select test --expected-passed 2031` →
  **PASS 2031/0**; `--select debug --expected-passed 2083` →
  **PASS 2083/0**. Pins in `tools/build/gate_counts.json` untouched
  (no new rlexe entry: 100 agent-runtime creations exceed what the
  debug regression gate can afford reliably; the repro is committed as
  evidence tooling instead).

### Residual (host-exit-join)

- Sequential per-child joins cost one syscall per agent (N ≤ a few
  hundred in any realistic host); all children exit concurrently after
  the signal pass, so wall time stays ≈ the slowest child's exit.
- **Wedged-agent failure mode (adversarial review)**: with >64 agents, a
  wedged (`while(true)`) agent now deterministically wedges process exit
  (per-child INFINITE waits) where the pre-fix code crashed with a UAF;
  with ≤64 agents both old and new code hang. Strictly a better failure
  mode, pre-existing INFINITE semantics — but a host needing bounded
  exit has no watchdog.
- The child-side `Error` path of `RuntimeThreadData::ThreadProc` still
  skips joining its own sub-agents, but it is unreachable after any
  script ran (all `Error` exits precede `JsRun`; sub-agents are created
  only by script) — pre-existing, unchanged.
- `CreateThread`'s return value at the spawn site is unchecked
  (pre-existing): a failed spawn leaves `hThread == NULL`, and the join's
  `WaitForSingleObject(NULL)` would trip the per-child Assert (debug) or
  delete a never-started child (release) — same shape pre- and post-fix.
  Follow-up card; no change in this repair.
- Review note: `-CollectGarbage` is not a real `ch` flag (silently
  accepted; the `CollectGarbage()` global is undefined in these
  profiles, so committed tests' `typeof` guards never fire) — GC legs
  were approximated with allocation-pressure churn (100–400 runtime
  creations) instead.

### Review outcome (host-exit-join)

Both reviewers **ACCEPTED** with zero `[bug]` and zero `[risk]`
findings. The general review re-verified the three-pass structure,
children-lifecycle facts, ownership record, ≤64 semantics equivalence,
and hang-predicate equivalence from source, and re-ran the A/B plus
both gates (2031/0, 2083/0). The adversarial review executed ~250
process-exits: scale to **400 agents** (exit 0, no crash/hang,
≈linear wall time), cap-edge counts **63/64/65/66** (pre-fix UAF
8/12 at 65/66 — matching the receipt's 33/50 at 100 — post-fix zero
across 72 edge runs), teardown races (mid-leaving, pending monitors,
timers, broadcast cutoff, nested sub-agents, raw-exit Quit with 70
live children, 3–4-way concurrent load) all clean, and ≤64 semantics
byte-identical pre vs post on both profiles. Its nits are folded
above (wedged-agent failure mode, GC-flag note); the general review's
assert-wording and unchecked-`CreateThread` nits are folded above and
filed as a follow-up card.

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
order is preserved through RESOLUTION, not just latch: the pass chains
the latched tokens (`passPrev`/`passNext`, stamped at latch time) and the
registrant's monitors claim them head-first — see the S6e conformance
repair section and `AtomicsAsyncWaiter::TryClaimWakeInPassOrder`.

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
  ≈15 s/par round. **Provenance:** this 1,255-run aggregate is
  doc-embedded only and not reconstructable — `drive4.mjs` overwrote
  its per-round result files in place (last-round remnant and driver
  preserved under `out/s6e-teardown-review/drive4/`). The
  independent reviewer campaign under `out/s6e-teardown-review/`
  corroborates it: **1,134 valid of 1,154** Phase A runs (the 20
  discounted runs are the first round's 10 `a3` one-byte stdout
  mismatches and 10 `a4` driver-timeout kills; both families clean in
  all five later rounds) plus Phase B's 970 no-Quit churn runs.
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
  (backtrace above); intermediate-snapshot host + same dylib 1/400
  identical backtrace/address; `lldb --batch` 300 runs of n6/n8/n7
  clean (timing displacement). No-Quit `n6noquit` (churn-matched 60 ms
  driver kill): 3 REALFAULT/600, identical backtrace — see the
  residual below. Reviewer Phase B (no-Quit churn classification,
  `rvb-*.out` under `out/s6e-teardown-review/`, 970 runs): current
  host 0/200 (8 agents/60 ms), 0/120 (2/60 ms), 1/80 (8/1.5 s),
  **2/120 (16 agents/60 ms — the sharp reproducer)**, 0/100 with the
  interposer; base host 0/200 (8/60 ms); intermediate-snapshot host
  0/150 (8/60 ms).
- **Final gate receipt (reviewer, at `a78a5dba4`)**: both rlexe gates
  re-run by the reviewer on the iteration-3 binaries above — test
  **2031/0**, debug-static **2083/0** (same expected-pin overrides as
  the iteration-2 gate entry). Gate evidence provenance now points to
  the copied reviewer logs `out/gate-receipt/rev-s6e3-gate-test.log`
  and `rev-s6e3-gate-debug.log` (with `out/gate-receipt/PROVENANCE.md`
  recording commit, binary SHA-256s and reviewer run times), not to
  the reviewer's scratch directory.

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
- **Rare SIGBUS at agent startup (engine race, out of scope)**:
  concurrent `JsCreateContext` → `ScriptContext::Initialize` →
  `JavascriptLibrary::InitializePrototypes` → `JavascriptStringObject`
  ctor (JavascriptStringObject.cpp:36) faults on a fixed page offset
  (`0x30005c000`, also `0xa8c05c000` — `+0x5c000` in different
  segments) with si_code 1, ~0.3% under the teardown campaign and
  ~0.5–0.75% under the interposer driver. Independent of
  `WScript.Quit` (fires before the registration barrier; reproduces
  on the intermediate-snapshot host with the same dylib; reproduces
  3/600 with no Quit at all — `n6noquit`, main spins forever, driver
  kill only). **Scope of that proof:** independent of the teardown
  host change and of the current dylib, but *not* proven absent from
  the entire S6e engine branch at current test power — the pre-S6e
  base host was churn-probed only at 8 agents/60 ms (0/200), never at
  the sharp setting. Sharp reproducer: **16-agent no-Quit churn,
  ~2/120** (`rvb-cur-60-16.out`); 8-agent/60 ms shows 0/200, so weak
  churn proves nothing. Produces no `.ips` (ReportCrash
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
- **Quit can hang on a blocked full stdout pipe (pre-existing)**: if
  stdout is a pipe nobody reads and agents fill it, an agent blocked
  in `write` holds the stdio `FILE` lock; `Quit`'s synchronous stdout
  flush then blocks and the process hangs (probe
  `out/s6e-teardown-review/a6_blockout.js`, run only with an unread
  stdout pipe). Base/iteration-2 parity verified — the join/PAL-exit
  designs flushed the same way and hung the same way; not introduced
  or worsened by the raw-exit change.
- **`-TTRecord` + `Quit` truncates the time-travel log (pre-existing)**:
  quitting mid-record leaves a truncated TT stream — no Quit path
  (raw exit or PAL exit) finalizes the recorder, so replay cannot
  proceed past the Quit. Base/iteration-2 parity verified. The
  documented S6d/S6e TTD stance (no host timer queue while
  recording/replaying) is unchanged.
- **End-of-main still walks PAL cleanup with live agents
  (pre-existing, out of scope)**: when the main script runs to
  completion while agents are still alive, `bin/ch/ch.cpp`'s
  end-of-main termination (`ExitProcess(0)` semantics —
  `PAL_Shutdown`/CRT exit) still runs the PAL cleanup path over live
  agent runtimes — the natural-exit analogue of the detach-walk
  hazard iteration 3 removed only for `WScript.Quit`. Unchanged since
  base and out of scope for the teardown-only slice.
- **TTD**: no delayed tasks (hence no monitors) while recording/replaying;
  cross-agent waitAsync stays pending — the documented S6d stance.
- **Finalizer lock risk** (S6b, unchanged): `CancelAsyncWaiters` takes the
  waiter-list CS; a stop-the-world GC suspending a foreign thread inside
  `csForAccess` could stall finalization. Not worsened by this slice (the
  foreign notify holds the CS no longer than before).
- **JIT/B4**: no bytecode/JIT surface changed; evidence is Mac ARM64 no-JIT.
  B4 must repeat the focused tests and the two test262 selections on native
  Linux and JIT profiles.
