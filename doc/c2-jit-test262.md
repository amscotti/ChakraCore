# C2 JIT test262 campaign — per-batch corpora under the merged Mac ARM64 JIT

Recorded 2026-11-01, lane `c2-jit-test262` (worktree
`ChakraCore-wt/c2-jit-test262`, base `e4b2ea499` = `modernize/baseline` tip).
This is a **measurement + triage campaign**, not a fix lane. It discharges (or
re-scopes) the per-batch C2 obligation recorded in
[modernization-plan.md](modernization-plan.md) §C2:

> interpreter and applicable SimpleJIT/FullJIT paths pass before enabling
> globally on JIT configurations (since 2026-10-31 the merged Mac ARM64 `jit`
> profile supplies that environment for regression suites; each batch's
> test262 corpus under SimpleJIT/FullJIT and any Linux JIT claim remain owed —
> Linux via B4)

and the 2026-10-31 update's residual wording: *"What remains owed per batch
before any broader JIT-configuration claim: its test262 corpus under
SimpleJIT/FullJIT, and Linux JIT evidence via B4."*

**Nothing in this lane changed engine source.** The only in-tree additions are
this receipt and the campaign tooling under `tools/jit-test262/` (the batch
definitions, the driver/differ, and the S2 selection list). All gates below
ran on the unchanged base.

## Scope

- **Binary**: `out/jit/bin/ch` + `libChakraCore.dylib` built in this worktree
  by `zig build -Dprofile=jit` through the shared gate lock
  (`BUILD_EXIT:0`; dylib 27,529,560 bytes, 2026-11-01 11:41;
  `ch version 1.13.0.0-beta`). `zig build -Dprofile=jit check` on the same
  install: `check_jit: PASS` (NativeCodeGenerator symbol present; hot
  call/rest/apply script green under both `-mic:1 -bgjit-` and
  `-forceNative -off:simpleJit -bgJitDelay:0`), bytecode verify green.
- **Corpus**: the pinned test262 checkout
  `/Users/ascotti/Documents/code/ChakraCore/out/test262`
  (`2e0a56762801e275a9fdf96dc49d90ba0cddcf63`, read-only; verified clean and
  at the lock revision by `run.py` before and after every run).
- **Harness**: `tools/test262/run.py` (worktree copy at the base commit),
  `--jobs 8`, per-scenario timeout 10 s, controls (the 18 negative-phase /
  sync / async / module / realm / detachment control scenarios) passed in
  **every** invocation of every batch and mode — that is also the
  "negative-phase-relevant trees" coverage: the negative-phase judging path
  (`-NegativePhaseReport` marker + fallback) executed on all 57 runs.
- **Modes** (flags appended to each batch's `-Test262 -CollectGarbage
  -NegativePhaseReport` + batch flags; spellings are the rlexe variants from
  [arm64-macos-jit.md](arm64-macos-jit.md) §6, which `check_jit` proves
  compile native code on this binary):

| Mode | Host flags | Meaning |
|---|---|---|
| `nojit` | `-nonative` | interpreter baseline on the same JIT-configured binary |
| `interp` | `-maxInterpretCount:1 -maxSimpleJitRunCount:1 -bgjit-` | interpret once, then full JIT (the `interpreted` variant) |
| `fulljit` | `-forceNative -off:simpleJit -bgJitDelay:0` | compile everything up front incl. builtins (the `dynapogo` variant) |

- **Batches and corpora**: the globs are each receipt's own selection,
  recorded in `tools/jit-test262/batches.json` with the receipt each came
  from. The S2 selection is the receipt's own rule (every `.js` under
  `test/language`, `test/built-ins`, `test/annexB` whose path or content
  matches /bigint/i — regenerated at 1,730 files, matching the receipt's
  count).
- **Exclusions**: no asm.js/WASM trees are in any batch corpus, so the
  documented Mac asm.js/WASM feature gap (`require_asmjs`/`require_wasm`)
  does not apply to these runs; it stays excluded deliberately. No other
  corpus filtering was applied.

## Results

Per batch: `nojit / interp / fulljit` = passed/failed of scenarios, and
per-scenario diffs vs `nojit` (joined on `(relative file, scenario)`; a diff
is any pass↔fail flip). Machine-readable table: committed snapshot
`tools/jit-test262/campaign-table.json` (live copy
`out/c2jit/campaign-table.json`); per-batch evidence and wall times in
`out/c2jit/<batch>/<mode>/{results,summary}.json` and `<batch>/*.run.log`.

<!-- RESULTS-TABLE -->

| Batch (receipt) | nojit P/F of N | interp P/F of N | fulljit P/F of N | diffs vs nojit |
|---|---|---|---|---|
| `s4-optchain` | 202/6 of 208 | 202/6 of 208 | 202/6 of 208 | interp: 0; fulljit: 0 |
| `s5-iterator` | 834/474 of 1308 | 834/474 of 1308 | 834/474 of 1308 | interp: 0; fulljit: 0 |
| `s10-copy` | 274/0 of 274 | 274/0 of 274 | 274/0 of 274 | interp: 0; fulljit: 0 |
| `s10-setmethods` | 372/0 of 372 | 372/0 of 372 | 218/154 of 372 | interp: 0; fulljit: 154 |
| `s11-replaceall` | 62/28 of 90 | 62/28 of 90 | 62/28 of 90 | interp: 0; fulljit: 0 |
| `s11-remainder` | 170/0 of 170 | 170/0 of 170 | 152/18 of 170 | interp: 0; fulljit: 18 |
| `s1-class` | 16565/124 of 16689 | 16565/124 of 16689 | 16565/124 of 16689 | interp: 0; fulljit: 0 |
| `s2-bigint` | 2531/904 of 3435 | 2531/904 of 3435 | 2531/904 of 3435 | interp: 0; fulljit: 0 |
| `s3a-property-escapes` | 1862/118 of 1980 | 1862/118 of 1980 | 1862/118 of 1980 | interp: 0; fulljit: 0 |
| `s3b-v-universe` | 346/28 of 374 | 346/28 of 374 | 346/28 of 374 | interp: 0; fulljit: 0 |
| `s6-sab-atomics` | 891/91 of 982 | 900/82 of 982 | 896/86 of 982 | interp: 9; fulljit: 11 |
| `s8-resizable` | 804/30 of 834 | 804/30 of 834 | 804/30 of 834 | interp: 0; fulljit: 0 |
| `s8-float16` | 4966/492 of 5458 | 4966/492 of 5458 | 4966/492 of 5458 | interp: 0; fulljit: 0 |
| `s9-regexp` | 1544/84 of 1628 | 1544/84 of 1628 | 1544/84 of 1628 | interp: 0; fulljit: 0 |
| `s12-modules` | 563/174 of 737 | 563/174 of 737 | 563/174 of 737 | interp: 0; fulljit: 0 |
| `s13-weakref-fr` | 152/0 of 152 | 152/0 of 152 | 152/0 of 152 | interp: 0; fulljit: 0 |
| `s14-blockscope` | 287/0 of 287 | 287/0 of 287 | 287/0 of 287 | interp: 0; fulljit: 0 |
| `s14-eval` | 454/0 of 454 | 454/0 of 454 | 454/0 of 454 | interp: 0; fulljit: 0 |
| `s14-labeled-finally` | 1171/12 of 1183 | 1171/12 of 1183 | 1171/12 of 1183 | interp: 0; fulljit: 0 |

Total mode-vs-nojit diffs: **192**.


## Diff triage

Diffs are classified: (a) asm.js/WASM gap (excluded by construction — no batch
corpus touches `require_asmjs`/`require_wasm` trees), (b) abort-to-interpreter
families — tests that pass without being JIT-compiled, expected by design,
(c) genuine JIT-vs-interpreter behavioral divergence, (d) harness/flag/timing
artifact.

**Class (c) — genuine divergences, one root cause, two surfaces.**

- **C2-JIT-1 — all seven `Set.prototype` set methods SIGABRT under a
  JIT-compiled caller.** 154/154 s10-setmethods fulljit diffs (372 scenarios),
  all `crash:SIGABRT` with empty stdout/stderr. Minimal repro
  `tools/jit-test262/repros/set-methods-fulljit-reentrancy.js`:
  `print(new Set([1]).union(new Set([2])).size)` — exit 0 under `-nonative`
  and the interp flags; **exit 134 (SIGABRT)** under
  `-forceNative -off:simpleJit -bgJitDelay:0`. macOS crash-report backtrace:
  `Js::JavascriptSet::Entry*` → `Js::SetMethodCallHas` (or the keys-iterator
  helpers) → `ThreadContext::CheckAndResetReentrancySafeOrHandled` →
  `Js::Throw::FatalInternalError` → `abort`.
- **C2-JIT-2 — `Object.groupBy` / `Map.groupBy` SIGABRT the same way.**
  18/18 s11-remainder fulljit diffs. Minimal repro
  `tools/jit-test262/repros/groupby-fulljit-reentrancy.js`.

Suspected area (from the repro's shape, not fixed here): the Set-method
helpers (`SetMethodCallHas`, `GetSetLikeKeysIterator`, `SetLikeKeysStep` in
`lib/Runtime/Library/JavascriptSet.cpp`) and `MapGroupByCallCallback`
(`lib/Runtime/Library/JavascriptMap.cpp`, shared by both groupBy entries)
re-enter JS through `CALL_FUNCTION` inside `JS_REENTRANCY_LOCK`/`JS_REENTRANT`
**without** `BEGIN_SAFE_REENTRANT_CALL`. `CALL_FUNCTION` first executes
`ThreadContext::CheckAndResetReentrancySafeOrHandled()`, which
`AssertOrFailFast`s unless `reentrancySafeOrHandled ||
isInReentrancySafeRegion` (`lib/Runtime/Language/Arguments.h`,
`lib/Runtime/Base/ThreadContext.h`). The interpreter's `OP_Call` handlers wrap
every call in `BEGIN_SAFE_REENTRANT_REGION`
(`lib/Runtime/Language/InterpreterStackFrame.cpp` ~3957/~4000), so an
interpreter caller satisfies the assert; a JIT-compiled caller enters the C++
builtin with neither flag set and the first C++→JS call fails fast.
`JavascriptMap::EntryForEach` (`JavascriptMap.cpp:179`) shows the
`BEGIN_SAFE_REENTRANT_CALL` arming these helpers lack, which is why the older
callback builtins (sort comparators, `forEach`) are clean.

Caller-mode matrix for C2-JIT-1 (warmed caller,
`out/jit/bin/ch`; corrected per review): `-nonative` pass; interp flags
(`-maxInterpretCount:1 -maxSimpleJitRunCount:1 -bgjit-`) **abort once
the loop crosses the tier-up threshold** (review binary-searched the
boundary to 300 < n ≤ 400 iterations; 200/300-iteration warmers pass
10/10); `-forceNative -off:simpleJit -bgJitDelay:0` **abort**;
`-forceNative -off:fulljit` (forced SimpleJit) **abort**; `-mic:1
-bgjit- -off:fulljit` **abort**; **and default flags (no flags at all)
ABORT too — racy at a 200-iteration warmer (0 134 0 134 0 over 5 runs,
background-JIT race) and deterministic at ≥1000 iterations for both
union and groupBy**. So the abort follows **any JIT-compiled caller**
(SimpleJit or full JIT), and C2-JIT-1/2 are reachable from **ordinary
default-configuration user code** — a hot loop (≥~400 iterations under
interp flags, ~1000 under default background JIT) that calls a Set
method or groupBy SIGABRTs the process. **Severity per review: not an
exotic-configuration defect; the fix lane is prioritized accordingly.**
The interp-mode corpus runs escape it only because test262 loops stay
below the tier-up threshold (tier-up is loop-threshold-based, not
invocation-count-based — a corpus with a ≥400-iteration loop would
silently become a warmed-JIT corpus). The in-tree rlexe gate missed it
for the same reason plus shape: `test/es7/setmethods.js` passes under
the fulljit flags (verified, exit 0) because its Set-like arguments are
recorder objects whose JS getters run before the first un-armed
`CALL_FUNCTION`.

**Review outcome (dual ACCEPT with corrections folded):** the general
review verified methodology (flags verbatim from arm64-macos-jit §6,
corpora verbatim from each receipt incl. the 1,730-file S2 selection,
corpus-clean checks, controls **25** per run — the draft's "18" was a
stale count), re-derived the campaign table end-to-end, confirmed the
17/19 discharge statements by independent rerun, verified the flake
class (mode-invariant), and produced the fix-lane sweep list:
**unarmed sites** = `JavascriptSet.cpp` {374, 385, 404, 431},
`JavascriptMap.cpp` {356}, and `JavascriptObject.cpp` {1232, 1222} —
`Object.groupBy` has its **own** `GroupByCallCallback` copy (the draft's
"shared by both groupBy entries" was wrong; the fix lane must patch
both). The adversarial review confirmed the three modes are genuinely
distinct regimes, ran a 42-probe callback-builtin sweep under fulljit
(**no builtin beyond Set-methods/groupBy aborts — the two-surface
enumeration is complete for this class; the suggested sweep is done),
verified zero-diff batches byte-identical under `--jobs 1`, and
confirmed the flake class shows zero mode correlation under load. Its
severity upgrade (default-flags reachability) and the interp-boundary
correction are folded above. **s12 drift — RESOLVED (forensics, 2026-11-01):** the suspected
`s12-module-phase` regression is exonerated. Per-scenario A/B of the
exact S12 globs (737 scenarios) on the pre-module-phase binary
(`c1-negative-phase` `zig-out`, ≡ base state) vs current main:
**552/185 → 563/174 — exactly 11 fail→pass, 0 pass→fail**, the precise
predicted set (`json-invalid`, 9 attribute-shape tests,
`instn-star-err-not-found`). The −13 gap vs the S12 receipt's recorded
565/172 predates module-phase (the pre-leg already reads 552) and
matches that receipt's own documented `--jobs`/run-order flakiness in
the dynamic-import trees; the reviewer's "identical between main and
campaign" observation held because both measured current main. No
engine bug; current authoritative s12 nojit state is 563/174. Nit folded: single-file `--jobs 1` shapes of
the waitAsync flake family fail deterministically — the run-to-run
flipping is corpus-shape-dependent; classification unchanged.

**Class (d) — timing flakes (not JIT divergences).** s6-sab-atomics showed
9 interp / 11 fulljit diffs, every one an `Atomics.waitAsync`
`no-spurious-wakeup-*` multi-agent `lapse >= TIMEOUT` assertion. A 4-run
repeat matrix of the 11 files (`out/c2jit/s6-flake-{1,2}` nojit,
`out/c2jit/s6-flake-jit-{1,2}` fulljit) shows the same scenarios flipping
**within a single mode across runs** (e.g. `on-add/default` fail in nojit run
2 only; `bigint/on-xor` both scenarios fail only in fulljit run 2;
`on-or/strict` fails only in fulljit runs). These are the wall-clock/
cross-agent flakes the S6 receipts already documented for this family;
classified (d), no repro filed.

**Class (b) — abort-to-interpreter, observed as pass-with-0-diff.** The
documented by-design abort families (direct-eval callers/eval code, the 18
class/private/import-attribute opcodes, the labeled-break-from-`finally`
GlobOpt shape) show up as **0 diffs**: their corpora (s1-class, s14-eval,
s14-labeled-finally, s12) pass identically in all modes because the affected
functions bail to the interpreter and the tests still pass — i.e. those
tests are *not* JIT-compiled, per the merge-closure coverage limits.

**No class (a)** diffs: no asm.js/WASM trees are in any batch corpus.

**Base drift vs receipt totals (not JIT-related, recorded for honesty).**
Several nojit baselines differ from the receipts' final numbers because the
receipts predate later batches: s4-optchain 202/6 vs 154/54 (S14
assignment-reference work fixed the lhs/rhs order family; the 6 residuals
are `lgcl-*-assignment-operator-lhs-before-rhs.js`), s5-iterator 834/474 vs
812/496, s8-resizable 804/30 vs 424/834 (S8 immutable/Float16 follow-ups),
s6 891/91 vs 574/408 (S6 waitAsync follow-ups), s10-copy 274/0 and
s10-setmethods 372/0 and s11-remainder 170/0 vs receipts' BigInt-literal
parse failures (BigInt is default-on at this base), s13 152/0 (WeakRef
58/0 + FR 94/0 receipt totals reproduced exactly), s3a 1862/118, s3b 346/28,
s14-blockscope 287/0, s14-eval 454/0, s14-labeled-finally 1171/12 all
reproduce receipts exactly. s12-modules is 563/174 vs the receipt's 565/172
with a much larger failure list than the receipt's residue paragraph
(module TDZ-binding initialization order, `import defer` syntax, import-bytes
families) — the failure set is byte-identical across all three modes (0
diffs), so it is base-state/judging drift since the receipt, not a JIT
effect; flagging it for the S12 owner as a follow-up question, out of this
lane's scope.


## Gates

Run on this worktree at the unchanged base `e4b2ea499` (no engine source
touched; only `doc/c2-jit-test262.md` and `tools/jit-test262/` added):

- `zig build -Dprofile=jit` through the shared gate lock: `BUILD_EXIT:0`
  (`out/jit/bin` installed; dylib 27,529,560 bytes, 2026-11-01 11:41).
- `zig build -Dprofile=jit check`: `check_jit: PASS` (native-codegen symbol
  plus hot call/rest/apply under both `-mic:1 -bgjit-` and
  `-forceNative -off:simpleJit -bgJitDelay:0`), `check_smoke` and bytecode
  verify green — the sanctioned proof that both campaign modes compile
  native code on the measured binary.
- `zig build check-regression` (no-JIT test profile): **`Total: passed
  2033, failed 0`** (`REGRESSION_EXIT:0`), matching the base expectation —
  the sanity anchor that this lane changed no engine behavior.

## What this discharges

19 batch corpora × 3 modes = 57 corpus runs (36,615 scenarios per mode with
deliberate overlap between selections; ~60.5 min total wall, `--jobs 8`),
plus the 18-scenario control set (incl. negative-phase judging) green in
every run. Every result set is per-scenario identical to the same binary's
`-nonative` baseline except the 172 class-(c) crashes and 20 class-(d)
flakes below.

Per the plan §C2 wording ("its test262 corpus under SimpleJIT/FullJIT, and
Linux JIT evidence via B4"):

- **DISCHARGED (Mac ARM64): test262 corpus under SimpleJIT/FullJIT recorded,
  zero JIT-vs-interpreter diffs** — s4-optchain, s5-iterator, s10-copy,
  s11-replaceall, s11-remainder *except its groupBy half*, s1-class
  (16,689 scenarios incl. the full class-elements corpus), s2-bigint
  (3,435, totals exactly the receipt's flag-on 2,531/904), s3-a
  property-escapes (1,862/118 — receipt-faithful), s3-b v-universe
  (346/28 — receipt-faithful), s6 SAB/Atomics flag-on, s8-resizable,
  s8-float16 sweeps, s9-regexp selections, s12 import-attributes + module
  code (module machinery works identically under both JIT modes),
  s13 WeakRef/FinalizationRegistry, s14 blockscope/eval/labeled-finally.
  For the by-design abort-to-interpreter families inside these corpora
  (direct-eval, the 18 class/private/import opcodes, labeled-break-from-
  `finally`), the tests pass interpreted inside the JIT-configured binary —
  the obligation's wording is met with the merge-closure caveat that those
  functions are not JIT-compiled.
- **RE-SCOPED, not discharged: s10 Set methods and the s11 groupBy half.**
  Their corpora are red under fulljit: 154/372 and 18/170 scenarios SIGABRT
  (C2-JIT-1/C2-JIT-2 above, one root cause, minimal repros filed under
  `tools/jit-test262/repros/`). The interp-mode corpus passes only because
  one-shot test262 scripts never tier up; a warmed caller aborts in interp
  mode too. Discharging these two needs the fix (its own lane), then a rerun
  of exactly this campaign for the two batches (`campaign.py run
  s10-setmethods s11-remainder`).
- **Linux JIT evidence remains owed via B4** for every batch, unchanged.


## Residuals

- **C2-JIT-1/C2-JIT-2 (open, class (c))**: Set-methods/groupBy reentrancy
  fail-fast under JIT-compiled callers. Not fixed here per lane scope;
  suspected area recorded above (missing `BEGIN_SAFE_REENTRANT_CALL` in the
  Set-method helpers and `MapGroupByCallCallback`). A fix lane should also
  sweep other `JS_REENTRANCY_LOCK`-without-`BEGIN_SAFE_REENTRANT_CALL` sites
  reachable from JIT callers, and add a warmed-caller regression test (the
  one-shot rlexe/test262 shapes do not catch this class).
- **Timing flakes (class (d))**: the `Atomics.waitAsync no-spurious-wakeup-*`
  family flips run-to-run in every mode; a future harness change could pin
  them (`--jobs 1`, or a longer `--timeout`) instead of counting them as
  diffs.
- **s12 base drift**: my nojit baseline (563/174) fails ~165 module scenarios
  the s12 receipt's residue paragraph does not list (module TDZ-binding init
  order, `import defer`, import-bytes). Mode-invariant, so not a JIT issue;
  flagged to the S12 owner.
- **Pre-existing engine gaps unchanged by JIT** (identical failure sets in
  all modes): s4 lhs-before-rhs evaluation order (6), s5 excluded-tag
  iterator-helper proposals (474), s3-a ICU data drift + u-literal leniency
  (118), s3-b accessor/substrate class (28), s2 post-ES2025/SAB-off classes
  (904), s14-labeled TCO stack overflow (8) + 4 parse errors, s11-replaceall
  `@@replace` substrate (28 without `-ES6RegExSymbols`).
- **Flag matrices**: corpora were run with each receipt's own flags only
  (plus `-ESSharedArrayBuffer` for S6). The S11 `-ES6RegExSymbols` and S3-b
  `-ES6RegExPrototypeProperties -ES6RegExSymbols -ESBigInt` flag-on variants
  were not rerun under JIT; they exercise the same library code paths as the
  default-flag runs that showed 0 diffs, but the flag-on corpora themselves
  stay owed if a JIT-configuration claim ever needs them.
- **No engine source was changed in this lane**; `zig build check-regression`
  and `zig build -Dprofile=jit check` were both run on the unchanged base
  (see Gates).

