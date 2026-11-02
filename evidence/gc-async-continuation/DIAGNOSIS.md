# gc-async-continuation — diagnosis evidence

Defect as filed (dbg-async-attach adversarial review, HIGH): "GC during churn
while async frames are suspended silently drops promise continuations — exit 0,
no error." Repros: `h0.js` (awaiting fn never resumes after churn+resolve),
`h5.js` (after churn, even a NEW async fn awaiting an ALREADY-RESOLVED promise
never resumes). Debug-static dropped 3/3; test profile resumed 3/3.

## Files

- `h0.js`, `h5.js` — the filed reproducers (copied from /tmp/opencode).
- `t-order.js` — deterministic, timing-free minimization discovered during
  diagnosis: `setTimeout(fn1,0); setTimeout(fn2,0)` where fn1 awaits
  `Promise.resolve()` — fn2 runs before the await resumes on BOTH profiles
  (no churn, no GC, no wall-clock race).
- `trace1.lldb` — first (abandoned) lldb plan; superseded by printf probes.
- `verification-*.txt` — post-fix outputs (20/20 runs each recorded in the
  receipt).

## Root cause (source + instrumented-trace level; NOT a GC bug)

1. **GC exonerated by pointer-stable trace.** Temporary printf
   instrumentation in `Recycler::DoCollect`, `JavascriptPromise::PerformPromiseThen`,
   `ResolveHelper`, `EnqueuePromiseReactionTask`, `EntryReactionTaskFunction`
   and the host `PromiseContinuationCallback`/`MessageQueue` showed, for h5 on
   debug-static: exactly ONE collection; the pending promise's reaction list,
   reaction record, handler closure and generator pointers IDENTICAL
   before/after the GC; the reaction task created and handed to the host; and
   `EntryReactionTaskFunction` executing BOTH queued tasks — but AFTER the
   t=500 END timer printed `out`. The continuations were never dropped — they
   ran after the observation point.

2. **Host queue ordering bug (the defect).** `ch`'s `MessageQueue` is one
   priority queue keyed by absolute due time. Promise-continuation microtasks
   were stamped `now` (`CallbackMessage(0, task)` → `BeginTimer()`), competing
   with timers:
   - h0/h5 mechanism: the debug-profile churn callback ran 566 ms (tick
     1462→2238), so the t=300 resolve callback actually executed at wall time
     2238 — 366 ms past the END timer's absolute deadline (1872). The
     microtask (due 2239) sorted AFTER END (due 1872) and ran after it.
   - t-order mechanism (deterministic, both profiles): a microtask enqueued
     during fn1's callback is stamped a few ms later than the already-queued
     delay-0 timer2, so timer2 jumps ahead of the await resume — an ES job-
     semantics violation with no timing dependence.

   Profile dependence explained: nothing profile-specific in the engine — the
   debug-static build's slower allocation path made the churn callback overrun
     the t=500 deadline deterministically (3/3), while the test profile's churn
   finished ~200 ms earlier, keeping the resolve callback before wall time 500
   so the microtask stamped "now" legitimately preceded END. The GC was only
   the churn's cost center, never the dropper.

3. **Two latent host/engine bugs exposed once microtasks drain first** (found
   while repairing the gate):
   - `bin/ch/MessageQueue.h` `SortedList::Remove(PredicateFn)` never advanced
     `node` — `ClearTimeout` for a timer not at the queue head spun forever
     (60 s gate timeout on `es6module/dynamic-module-functionality.js`;
     sample showed the spin inside `RemoveById`'s predicate loop). Previously
     unreachable in practice because the cleared watchdog was at the head.
   - `SourceTextModuleRecord::DecrementAndEvaluateIfNothingAwaited` called
     `FinishModuleEvaluation` without the `IsEvaluating()` guard its sibling
     call site has: a cyclic importer dynamically imported later
     (CyclicDouble6) received the decrement before its own evaluation started
     (generator == null, declInit == 0) → SIGSEGV in `gen->IsAsyncModule()`
     under the test profile, `Assert(evaluating)` under debug.

Fix: `MessageQueue::InsertSorted(msg, immediate)` gives promise-continuation
messages due time 0 (strictly before every timed entry, FIFO among
themselves); `PromiseContinuationCallback` passes immediate=true; the
`SortedList::Remove` loop advances; the TLA decrement site mirrors the
`IsEvaluating()` guard. Details, ownership record and verification tables:
`doc/e2-gc-async-continuation.md`.
