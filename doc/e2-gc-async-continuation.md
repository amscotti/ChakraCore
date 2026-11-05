# gc-async-continuation — host microtask ordering repair (lane receipt)

Lane: `gc-async-continuation`, base `3748cdd45` (main; gates 2037 test / 2089
debug). Filed by the dbg-async-attach adversarial review as a HIGH pre-existing
engine defect ("GC during churn while async frames are suspended silently
drops promise continuations"); independently reproduced 3/3 on debug-static
before this lane. **Verdict: GC exonerated — the defect was `ch` host message
ordering, plus two latent bugs the repair exposed.** Evidence and the
instrumented traces: `../evidence/gc-async-continuation/` (DIAGNOSIS.md, h0.js,
h5.js, t-order.js, verification-*.txt).

## 1. Root cause chain

1. **The filed symptom is real but misattributed.** h0 (async fn awaiting a
   promise; churn 15×12000 objects in a t=80 timer; resolve in a t=300 timer)
   and h5 (after the churn, a NEW async fn awaiting `Promise.resolve(99)`)
   both print no resume lines and exit 0 on debug-static, 3/3.
2. **Instrumented trace (temporary printf probes, all reverted).** Exactly one
   `Recycler::DoCollect` per run. Across it, the pending promise's reaction
   list, `JavascriptPromiseReaction`, handler closure and generator pointers
   are IDENTICAL before/after (e.g. reactions `0x110D83EC0`, reaction
   `0x111BB40A0`, handler `0x110D84CB0` unchanged). The reaction task was
   created, `PromiseContinuationCallback` received it, and
   `EntryReactionTaskFunction` executed it — **after** the t=500 END timer
   printed the output array. Nothing was collected or dropped; the resume ran
   after its observation point. The recycler's conservative body scanning
   plus the host's `JsAddRef` root (`CallbackMessage` ctor/dtor,
   `WScriptJsrt.cpp:2238-2262`) keep the whole chain alive, as designed.
3. **The defect: `ch`'s `MessageQueue` is one priority queue keyed by absolute
   due time, and promise-continuation microtasks were stamped `now`
   (`CallbackMessage(0, task)` → `InsertSorted` → `BeginTimer()`).**
   - *h0/h5 mechanism:* the debug-static churn callback ran 566 ms (tick
     1462→2238), so the t=300 resolve callback actually executed at wall
     2238 — 366 ms past the END timer's absolute deadline (1872). The
     microtask (due 2239) sorted after END (due 1872) and printed nothing.
   - *Deterministic minimization (`t-order.js`, no churn/GC/wall-clock):*
     `setTimeout(fn1,0); setTimeout(fn2,0)` where fn1 awaits
     `Promise.resolve()` — fn2 ran before the await resume on BOTH profiles,
     because the microtask is stamped a few ms after the already-queued
     delay-0 timer. An ES job-semantics violation (microtask checkpoint must
     drain before the next macrotask) with 100% reproduction.
4. **Profile dependence explained (no engine difference).** Debug-static's
   unoptimized allocation path makes the churn callback overrun the t=500
   deadline deterministically; the test profile's churn finishes ~200 ms
   earlier, the resolve callback runs before wall 500, and the microtask
   stamped "now" legitimately precedes END. The GC was only the churn's cost
   center. (The debug-static and test tables differ only in defines/flags,
   `tools/build/tables.zig:116-145` — no GC-behavior delta is involved.)

## 2. Fix (3 pieces)

| Change | File | Content |
|---|---|---|
| Microtask ordering | `bin/ch/MessageQueue.h` | `InsertSorted(msg, immediate=false)`: immediate messages get due time `0` — strictly before every timed entry (timed due = `GetTickCount64()+delay`, a large positive ms-since-boot value), FIFO among themselves (SortedList inserts equal keys after existing). `PopAndWait`/`HasDueMessage`/`PopDue`/`NextDueTime` all treat due-0 as "due now": no sleep, drains first — including the S6e agent-thread queues. |
| Microtask classification | `bin/ch/WScriptJsrt.cpp` | `PromiseContinuationCallback` passes `immediate=true` (comment documents the ES contract + the JsAddRef rooting). Timers, timer-continuations, debugger ops and module-load messages keep the previous timed behavior. |
| TLA guard (engine) | `lib/Runtime/Language/SourceTextModuleRecord.cpp` | `DecrementAndEvaluateIfNothingAwaited` now returns early when `!IsEvaluating()`, mirroring the existing guard at its sibling `DecrementAwaited` site (`FinishModuleEvaluation`, line ~1213). Awaited-count bookkeeping stays balanced (the decrement always happens; the module's own `ModuleEvaluation` later sees `awaitedModules == 0` and evaluates normally). |
| Hang repair (host) | `bin/ch/MessageQueue.h` | `SortedList::Remove(PredicateFn)` advances `node = node->next` — it previously spun forever when the predicate missed the head, i.e. any `ClearTimeout` for a timer behind earlier-queued messages. |

Why the two latent bugs fired after the ordering fix: the UnitTestFramework's
final `ClearTimeout(watchdog)` used to find the watchdog at the queue head
(the summary microtask ran last); with microtasks first, delay-0 module-load
messages precede it → non-head removal → spin. The TLA SEGV
(`top-level-await.js`, CyclicDouble6: `evaluating=0, wasEval=0, generator=null`)
fired because a child's async completion microtask now runs before the cyclic
importer's queued module-load message; the missing guard let
`FinishModuleEvaluation` dereference the null generator (SIGSEGV test profile,
`Assert(evaluating)` debug). Both are pre-existing defects, unreachable only
through the old ordering.

## 3. Ownership record (per doc/memory-management.md)

Resource / creation call / source location: queued promise-continuation task
(`JsValueRef task`) — engine `JavascriptLibrary::EnqueueTask` → host
`WScriptJsrt::PromiseContinuationCallback` (`WScriptJsrt.cpp:2918`);
`WScriptJsrt::CallbackMessage(0, task)` (`WScriptJsrt.cpp:2238`).
- Domain and allocator: Recycler/JS value owned by the engine runtime; the
  host holds only the `JsAddRef` reference-counted root (ctor AddRef → fatal
  on failure; dtor JsRelease after the call or at queue teardown
  `RemoveAll`). Unchanged by this lane — verified the rooting was already
  correct; the filed "GC drops continuations" hypothesis is disproven by the
  pointer-stable trace.
- Before: ordering contract (the actual defect) — the message competed in the
  absolute-due-time queue with a stamp of "now", so any timer with an equal
  or earlier absolute due time (same-millisecond stamps, or a deadline passed
  during a slow callback) ran first; the continuation then ran after later
  timers — observed as a silent drop.
- After: immediate messages carry due time 0 (a sentinel strictly earlier
  than every timed due); classification happens at insert; the descriptor
  (`MessageBase::m_time` stays 0) and all pop/due/next-due readers treat it
  as always-due. Timers keep `now+delay` semantics unchanged among
  themselves.
- Borrow invalidation: none new — queue mutation (`RemoveById`) already
  removes by id; the repaired predicate loop advances correctly on misses.
- User-code/reentrancy: `CallbackMessage::CallFunction` runs JS which may
  enqueue more messages; immediates stay FIFO, no starvation (drain order is
  total). `PopAndWait` never sleeps for immediates.
- Release on all paths: unchanged (dtor release; teardown `RemoveAll`;
  failed-call path still releases — `ProcessAll` deletes the message after
  `Call`).
- TLA record (engine): `awaitedModules` count on
  `SourceTextModuleRecord` — the decrement at child completion now happens
  for non-evaluating parents without attempting to finish them; no new
  allocation, root or trace obligation (all recycler state unchanged).
- Tests: `test/es7/async-microtask-order.js` (deterministic ordering + both
  churn repros) — the observed evidence for both contracts.

## 4. Verification

Repros (asserting the resume prints), post-fix:

| Case | debug-static | test profile |
|---|---|---|
| h0 (churn + later resolve) | 20/20 | 20/20 |
| h5 (new fn + already-resolved after churn) | 20/20 | 20/20 |
| t-order (deterministic delay-0) | in test, 20/20 via gate+runs | 20/20 |
| Pre-fix | h0 3/3 drop, h5 3/3 drop | resumed (churn under deadline) |

Gates (both with the final tree, `sh /tmp/opencode/cc-gate-lock.sh mise exec
-- zig build [check]`):

| Gate | Result |
|---|---|
| `zig build check` (test profile) | rc=0, regression **2038/0** (pin: 2038/0), bytecode/native/smoke/metadata checks green |
| `zig build -Dprofile=debug-static check` | rc=0, regression **2090/0** (pin: 2090/0) |

Focused scope:

| Suite | Result |
|---|---|
| es7 (incl. S6e waitAsync trio, FinalizationRegistry, DebuggerCommon AsyncSuspendedResume) | green in gate; DebuggerCommon 210/0 |
| es6 promises (ES6PromiseAsync etc.) | green after baseline regen (see §5) |
| test262 `test/built-ins/Promise/**` + `test/language/expressions/await/**` + `test/language/statements/async-function/**`, base-vs-fixed A/B (pinned corpus 2e0a5676) | 1635 scenarios: **0 pass→fail, 0 fail→pass, 0 result changes** |
| JIT profile (out/jit), es7+es6module+DebuggerCommon, disable_jit | fixed 341/0 vs base 340/0 (+1 = new test) |
| JIT profile, dynapogo | fixed 276/4 vs base 275/4 — the 4 (matchall, s6-growable-nonshared, bigint64array, resizable-typedarray) are byte-identical pre-existing Mac JIT gaps (documented in arm64-macos-jit.md class); zero new failures |
| The hang/SEGV exposures | dynamic-module-functionality.js rc=0 ×3 (was 60 s timeout), top-level-await.js pass ×5 test-profile + ×8 debug deterministic |

Deliberate expected-output changes (reviewed individually, each with an
in-source comment):

1. `test/es6/ES6PromiseAsync.baseline` regenerated (72 completion-order lines
   move): timer-driven outputs (#10's timer-triggered chain, #27/#28) ran
   between microtask waves; now microtask waves drain first — ES-correct
   (each test's own outputs unchanged; no content differences).
2. `test/es7/finalization-registry.js` timer-unregister block: cleanup jobs
   are engine host tasks (EnqueueTask → microtask) and now drain before the
   0 ms timer; the block asserts the corrected contract (unregister returns
   false; `timerSeen === 1`).
3. `test/es6module/top-level-await.js` LoadOrder/CyclicDouble expected
   strings: each module's await-resume chain completes in dependency order
   before the later dynamically imported graph evaluates — consistent with
   the single-cycle CyclicOrder expectation (which passes unchanged);
   dependency completion order preserved in both.

## 5. Pin decision and lineage

New deterministic entry `test/es7/async-microtask-order.js` (no flags; runs in
both selections; debug-gate runtime measured 0.93 s user / 1.05 s wall).
`tools/build/gate_counts.json`: **2037→2038 (test), 2089→2090 (debug)**,
lineage updated; `check_regression.py`/build.zig consumers follow
automatically (re-verified by both green gates above).

## 6. Residuals / limits

- The corrected host order is FIFO-within-microtasks, before all timed
  messages; `WScript.SetTimeout(fn, 0)` remains a timed macrotask (runs after
  microtasks queued before it — ES-correct; timer-vs-timer order unchanged).
- The engine's TLA async-module bookkeeping is not a spec-grade
  implementation of ES2025 module-graph evaluation; this lane only removed
  the reachable null-generator invariant break. Cyclic TLA interleaving
  remains host-scheduling-dependent by design (the updated expectations pin
  this host's deterministic FIFO order).
- test262 evidence covers Promise/await/async-function selections on the
  test profile only (the no-JIT default); the full-corpus run with the fixed
  binary (77268/15873) is recorded as a diagnostic snapshot only, not
  compared per-scenario against base beyond the focused selection.
- The recycler-side exoneration is specific to this repro shape (one
  collection, conservative marking plus host JsAddRef); it does not certify
  promise machinery under GCStress beyond the existing gate coverage.

### Review outcome (dual ACCEPT, nits folded)

Both reviewers ACCEPTed with zero `[bug]`/`[risk]`. The general review
reran the t-order A/B on base vs fixed vs Node oracle, verified every
queue reader's due-0 handling, the TLA guard's count balance, and that
the 72-line baseline change is a pure reordering to the Node/spec order
(sorted old == sorted new byte-identical; base binary reproduces the
old baseline). The adversarial review audited all three insertion
sites (single immediate producer), established the drain policy from
source (one message per `ProcessAll` iteration; immediates drain
before the next timed message — the ES job contract), ran a 32-shape
mixed matrix with programmatic invariant checking (zero violations),
verified starvation behavior equals Node (10k chain drains then timers;
infinite chain hangs identically), the ClearTimeout matrix incl.
reentrancy, and the S6e trio at 20/20 + 5/5×4.

Folded nits: (1) among ALREADY-OVERDUE timers ch deadline-orders
(delay-0 batch before delay-1) where Node/libuv coalesces 0/1ms into a
FIFO bucket — documented-granularity class (Chrome-like), not
ES-observable, but ch can swap that interleave run-to-run when a slow
callback overruns ms boundaries (inherent to the absolute-deadline
queue; deterministic gate baselines never mix the shape); (2) NEW
pre-existing follow-up card: dynamic `import()` during TLA evaluation
of a module the imported graph statically imports back never settles
(identical on base; re-entering an evaluating module shouldn't block;
no crash) — filed for a TLA follow-up lane
→ (2026-11-05 C sweep) corpus witness identified:
`top-level-await/pending-async-dep-from-cycle.js` and
`fulfillment-order.js` hang ("Test did not run to completion") on the
current binary — still open; see doc/open-cards.md;; (3) FinalizationRegistry
cleanup jobs now always precede any 0ms timer (host-legitimate,
documented in-test; a future host wanting V8-style separate-task
cleanup should note it).
