# C2 JIT lane: finally-early-exit back edges in GlobOpt

Status: **landed on `jit-finally-globopt`** (commits `dcd37f4f8` trace,
`0cd8f33b1` fix, `RECEIPT` this commit; base `8bc24be62` = main, gates
2038 test / 2090 debug-static, `zig build -Dprofile=jit check` green).

This closes the first of the three documented JIT coverage ports: the
labeled-break-out-of-a-finally CFG shape now **JIT-compiles correctly**
instead of aborting to the interpreter (`2eaa51bf5`'s defensive abort
class). The abort remains as the last-resort backstop for shapes outside
the family characterized here.

## The coverage gap (before)

[arm64-macos-jit.md](arm64-macos-jit.md) coverage limits recorded that
"the labeled-break-from-`finally` CFG shape in GlobOpt" aborts codegen.
A labeled `break L` out of an inner `finally`, with `L` inside an outer
`try` whose body also has a `try/catch`, made a finally-end block a
predecessor of a join block that GlobOpt's forward pass visits **before**
the finally-end block. `BasicBlock::MergePredBlocksValueMaps`
(`lib/Backend/FlowGraph.cpp`) then cloned from an unvisited
predecessor's uninitialized `globOptData` (`globOpt == nullptr`) — the
pre-`2eaa51bf5` SEGV; `2eaa51bf5` turned it into
`OperationAbortedException`, so these functions stayed interpreted
(`test/es7/labeled-break-finally.js` passes interpreted; the dynapogo
segfault was gone).

## Root cause (visitation gap mechanics)

Anchors in `lib/Backend/FlowGraph.cpp` unless noted:

1. **Edge creation.** `CheckIfEarlyExitAndAddEdgeToFinally` (≈:104)
   classifies a `Leave` inside a `RegionTypeFinally` as an early exit;
   the caller in `FlowGraph::Build` (≈:512) inserts an unconditional
   `BailOnEarlyExit` before a `Leave` to the *enclosing* finally's
   excepting label and calls `InsertEdgeFromFinallyToEarlyExit` (≈:688),
   which appends a `BrOnException` to the early-exit target at the end
   of that finally — creating the **finally-end → join** edge. The join
   (code after the labeled block) is inside the enclosing try and is
   laid out **before** the finally body, so this edge is a *back edge in
   block-list order* — while the bytecode branch it models is forward.

2. **Why GlobOpt never visits the pred first.** `GlobOpt::ForwardPass`
   iterates the block list in order (`FOREACH_BLOCK_IN_FUNC_EDITING`,
   `lib/Backend/GlobOpt.cpp:400`) and `OptBlock` merges **all**
   predecessors at first visit (`MergePredBlocksValueMaps`). A later-
   in-list predecessor has no `globOptData` yet. Loops survive this by
   the pre-pass machinery: `MergePredBlocksValueMaps` skips back-edge
   preds during the loop pre-pass and merges them on the head's second
   merge after `OptLoops` walked the body (`GlobOpt.cpp:653`).

3. **Why the loop machinery didn't apply.** `FindLoops` (≈:1256) only
   builds a `Loop` for an edge whose target is already flagged
   `isLoopHeader` — which only bytecode loop tops get
   (`SetBlockTargetAndLoopFlag`, ≈:1620, from `m_isLoopTop` labels).
   The finally-end → join cycle (join → finally entries → finally end →
   join; the join need not dominate the finally end because exception
   edges enter the finally directly) is invisible to it. The pre-existing
   design *does* handle the sibling case: a `continue` through a finally
   targets a **real loop header**, and `GlobOpt::RemoveFlowEdgeToFinallyOnExceptionBlock`
   (`GlobOpt.cpp:16437`) explicitly preserves those edges. The
   labeled-break-to-non-loop-target case was simply never covered
   upstream (same latent bug on x64; the test corpus with these shapes
   is this repo's S14-era addition).

Empirical CFG (temporary `-trace:FGBuild` instrumentation, since
removed) for the baseline shape: join `#17` has preds `{#16 (normal
fall-in), #20 (finally end)}`, `#20` is laid out after `#17`,
`#17 → #18/#19 (finally entries) → #20 → #17` closes the cycle, and no
block is a loop header.

## Fix design

All in `lib/Backend/FlowGraph.cpp` (+ `FlowGraph.h`), using the
existing loop machinery rather than new merge logic:

1. **Synthetic loop tops.** After `FlowGraph::Build`'s early-exit pass
   renumbers blocks (numbers are then final list positions), a scan
   marks every `m_brFinallyToEarlyExit` branch target that precedes its
   source in list order as a loop top via `SetBlockTargetAndLoopFlag` —
   the same treatment a `continue` target already gets and what
   `IRBuilder.cpp:1038` does for backward bytecode branches. `FindLoops`
   then builds a `Loop` for the cycle; `BuildLoop`'s existing
   `tail->number < head->number` check rejects any misclassification
   self-guardingly. The list-order comparison is made after renumbering
   precisely so freshly created finally-end blocks cannot masquerade as
   forward targets.

2. **`Loop::isFinallyEarlyExitLoop`.** The loops built for those heads
   are flagged, and `CanonicalizeLoops` skips `RemoveBreakBlocks` for
   them: the blocks between such a loop's head and the finally end
   belong to the enclosing try/catch — they are not break blocks of a
   source loop and must not be relocated after the loop tail.

3. **`RemoveUnreachableBlocks` worklist.** The fix exposed a latent
   ordering bug: the single forward mark-and-sweep deleted any block
   whose only predecessors appear later in the block list. A join
   reachable *solely* through the finally-end back edge (an
   always-breaking finally — the code after the labeled block is
   unreachable on the normal path) was removed while the finally-end
   `BrOnException` kept branching into the removed block's label,
   producing wrong code after a `BailOutOnException` (observed: SIGSEGV
   in `OP_BrFalse_A` reading a null Var the bailout never restored).
   Reachability is now marked with a worklist to a fixpoint before the
   sweep. For graphs without backward non-loop edges — every graph the
   old code handled — the fixpoint equals the old single pass.

Alternatives rejected: reordering the block list (breaks EH region
layout contiguity); merging the unvisited pred conservatively or
skipping the edge at the join (GlobOpt dataflow needs the pred's actual
live/type state to place conversions and bailout sets; the edge is
statically live for liveness and bailout layout even though the
runtime break path always takes `BailOnEarlyExit`); a GlobOpt-side
deferral/worklist (reimplements the loop pre-pass).

Ownership/allocation note (per
[memory-management.md](memory-management.md)): the scan's
`SList<BasicBlock*>` and the sweep's worklist are arena-allocated on
the `FlowGraph`'s per-function `JitArenaAllocator`, live for the
compile only; no recycler/GC memory, no ownership transfer, no
JsRT boundary change. The `Loop` bit flag is POD.

## Shape family covered (and how it behaved before)

Scratch corpus (kept in this receipt, ~44k assertions per run):
`baseline.js` = the task's baseline; `family.js` v1–v11 (baseline, no-
throw, no-try/catch, catch-finally, deeper nesting with mid finally,
multiple breaks/labels, labeled `continue` through finally in loops,
return-through, label outside the outer try, two sequential labels, and
a loop-state variant); `family2.js` w1–w6 (return between join and
finally end — the only-through-back-edge case; live loop after join;
outer try with its own catch; two labels from one finally; the eval
completion form; nested `continue outer` with state).

| Variant class | Before (base `8bc24be62`, dynapogo flags) | After |
|---|---|---|
| baseline, v1, v2, v4, v11 | abort (stays interpreted; `-trace:bailout` prints the abort) | compiles; `BailOutOnException`/`BailOutOnEarlyExit` records prove JIT execution; values correct |
| v3, v5, v6, v7, v9, v10 | compiled (no abort) | compiled; unchanged behavior |
| w1 (only-back-edge join) | not measured separately (class member of the abort family); latent `RemoveUnreachableBlocks` wrong-code exposed by the fix and closed by the worklist | compiles; 3000×2 iterations correct |
| w2–w6 | compiled or n/a | compiled; correct |

Before/after abort counts for the family under
`-forceNative -off:simpleJit -bgJitDelay:0 -trace:bailout`:
**before 4 aborting functions (v1, v2, v4, v11) + baseline; after 0.**

The rlexe test gate: `test/es7/labeled-break-finally.js` runs once per
shape (no tiering heat), so its dynapogo pass exercises the abort-free
path only indirectly; the JIT-shape proof is the scratch corpus above
with warm loops (3000 iterations each) plus `-trace:bailout` records.
No new rlexe entry was added: a deterministic gate-worthy variant would
need the dynapogo variant to tier the warm loops, which the runner does
not flag per-test beyond the standard variants (campaign-style evidence
recorded here instead, per the lane instructions).

## Verification

Binary: `out/jit/bin/ch` + `libChakraCore.dylib` rebuilt from the fix
commit (`zig build -Dprofile=jit`, green; stale `.zig-cache` archive
dirs from the investigation were pruned on this disk-constrained host —
objects reused, archive rebuilt).

- Focused: `es7/labeled-break-finally.js` 20× each under dynapogo
  flags, interpreted flags, and default — 60/60 pass. Families
  (`family.js`, `family2.js`, `baseline.js`) 20× dynapogo + 20×
  `-nonative` — 80/80 clean, zero aborts, values verified against the
  interpreter (`-nonative` reference run first).
- Full dynapogo suite (`test/runtests.py --variants=dynapogo`,
  497 s): **2006 passed / 286 failed**, zero signals. The 286 are
  exactly the tagged asm.js/WASM set (AsmJs 92, AsmJSFloat 14,
  ASMJSParser 1, wasm 54, WasmSpec 123, WasmSpec.MultiValue 2) — zero
  new failures vs the recorded 2004/286 (the +2 passed are the
  branch-state's added tests, not test changes; failure set identical).
- Interpreted variant (`-maxInterpretCount:1 -maxSimpleJitRunCount:1
  -bgjit-`): **2432 / 397** — exactly the recorded shape (397 = the
  tagged set incl. `Bugs/fabs1.js`); zero signals.
- `disable_jit` on the JIT binary: **2038 / 0**.
- No-JIT gates on this branch: `zig build check` (test profile)
  **2038 / 0**; `zig build check -Dprofile=debug-static` **2090 / 0**.
- `zig build -Dprofile=jit check`: green (`check_jit: PASS` incl.
  `-forceNative -off:simpleJit -bgJitDelay:0` smoke, `check_smoke:
  PASS`, bytecode verify green).
- test262 `s14-labeled-finally` corpus
  (`tools/jit-test262/batches.json`: `statements/labeled/**`,
  `try/**`, `for/**`, 1183 scenarios, pinned `2e0a5676`): **1171/12 in
  each of nojit, interp, fulljit — 0 diffs vs nojit**. The 12 failures
  are exactly the S14 receipt's pre-existing set (8 TCO stack overflow,
  2 `static-init-invalid-await`, 2 `let-*-with-newline` parse). This
  corpus is the fix's direct target class (labeled/try/for shapes)
  executing identically in all three tiers.

## Residual

- The `MergePredBlocksValueMaps` abort stays as the backstop; any
  finally-derived CFG shape producing an unvisited predecessor outside
  the `m_brFinallyToEarlyExit`-backward-edge class still aborts to the
  interpreter. None known: the family scan found no such shape.
- The synthetic loops are optimization-free by construction (no profile
  → `ImplicitCall_All`); the landing pad inserted for them is empty.
  That is coverage, not performance: these functions now JIT but their
  finally/early-exit paths still bail to the interpreter at runtime
  (`BailOnEarlyExit` on the break path, `BailOutOnException` on throws
  in JIT'd try regions), by design of the existing try/finally tiering.
- Upstream (x64) shares the latent `RemoveUnreachableBlocks` ordering
  bug and the unvisited-predecessor crash for this shape class; this
  fix is written backend-neutral (no ARM64-specific code).
- Scratch corpus scripts are not checked in (scratch dir under
  `$TMPDIR/opencode/lbf`; campaign results under
  `out/c2jit/s14-labeled-finally/`); the shapes are fully described by
  this receipt and covered by the existing
  `test/es7/labeled-break-finally.js` family plus the test262 corpus.
- `tools/test262` needed `npm ci --ignore-scripts` in this worktree for
  the campaign runner (dependency install only; no lockfile change).
