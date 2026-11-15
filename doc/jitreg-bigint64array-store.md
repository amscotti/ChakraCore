# JITREG-BI Lane Card — bigint64array typed-store ToBigInt (PGO-only)

Status: **FIXED** (fix landed in worktree, uncommitted) — `lib/Runtime/Language/JavascriptConversion.cpp`
(`JavascriptConversion::ToBigInt`), plus regression subtest in `test/es7/bigint64array.js`.
Owner: bigint64-fix. Evidence: min-repro PGO recipe, probe matrix, full-suite
before/after comparison. No commit per task instructions (`git status` shows the
two modified files only).

## Failing test

`test/es7/bigint64array.js` — 9/10 under the two-step recipe below (now 11/11
with the new regression subtest, see § Regression test). It is the ONLY BigInt
failure in the repo. Single-thread, no workers:
profile then force-native with `-off:simpleJit`:

```sh
cd test/es7
CH=out/jit/bin/ch
rm -rf /tmp/jitreg/p_bi && mkdir -p /tmp/jitreg
$CH -ESBigInt -CollectGarbage \
  -maxInterpretCount:1 -maxSimpleJitRunCount:1 -bgjit- \
  -dynamicprofilecache:/tmp/jitreg/p_bi bigint64array.js -args summary -endargs
$CH -ESBigInt -CollectGarbage \
  -forceNative -off:simpleJit -bgJitDelay:0 \
  -dynamicprofileinput:/tmp/jitreg/p_bi bigint64array.js -args summary -endargs
```

- Profile run: `pass`. FullJIT+PGO run: `FAILED — Summary of tests: total
  executed: 10; passed: 9; failed: 1` (pre-fix).
- Failure is function-local: the suite catches per-test; only the OOB-subtest
  body throws (`TypeError: Type mismatch` at line 173, the OOB object store).
  Everything else in the file passes either way.

Methodology note (PGO-only visibility): a `dynapogo`-only run is VACUOUS for
PGO-gated defects — the `interpreted` leg (or the repo's two-step recipe
above) is what produces the profile the second leg consumes. Always use the
two-step recipe; never infer PGO behavior from a single-leg run.

## Failing assertion (pre-fix)

`*** Running test #3 (2): Out-of-bounds and detached stores convert before
validation` → `Test threw exception: TypeError: Type mismatch` at
`bigint64array.js:173` (`view[10] = { valueOf() { calls++; return 3n; } };`).
The pre-existing in-bounds conversion assertions (same file, earlier subtest)
pass; the OOB `3n`-valued object store is the one that throws.

## Corrected root-cause analysis (supersedes the original suspect site)

The original card pointed at `lib/Backend/Lower.cpp:17782-17815` (inline
`HelperOp_ConvNumber_Full` before the typed-store fast path). JIT IR tracing
plus temporary lowering prints proved that block is **unreachable for
BigInt-element arrays**: `ShouldGenerateArrayFastPath` is false for
non-optimized arrays (`IsLikelyAnyUnOptimizedArray`, which covers
Int64Array..CharArray), so `GenerateFastElemICommon` returns `nullptr` and
every BigInt StElem lowers straight to the generic `HelperOp_SetElementI`
(the conversion-block print never fired in any probe or suite run). The
Lower.cpp reroute idea was therefore dead code and was reverted.

Actual mechanism, verified end-to-end with temporary prints in Lower, the
StElem helper, `ToBigInt`, `ToPrimitive` and the valueOf call site:

1. The BigInt StElemI carries `BailOutOnImplicitCallsPreOp` (bailKind 5) and
   lowers to the generic `OP_SetElementI_JIT` helper (correct lowering).
2. PreOp lowering disables implicit calls around the helper call
   (`SplitBailOnImplicitCall`: reset flags, disable, helper, re-enable,
   post-helper `BailOnNotEqual` check).
3. Inside the helper, `ToBigInt(obj)` → `ToPrimitive` →
   `ExecuteImplicitCall` sees calls disabled, SKIPS `valueOf`, sets the
   `ImplicitCall_ToPrimitive` flag, and returns `undefined` as a placeholder.
4. `ToBigInt` loop iterates on that `undefined` and hits the
   `TypeIds_Undefined` case, which throws `TypeMismatch` (correct for a real
   `undefined`, wrong for the placeholder).
5. The throw happens INSIDE the helper, so the post-helper bailout check —
   which would otherwise have bailed out to the interpreter for a correct
   re-execution — never runs. The user sees `TypeMismatch` with `valueOf`
   never called (`calls === 0`).

Normal (non-BigInt) typed arrays survive the same sequence because their
converters coerce the placeholder (`ToInt32(undefined) === 0`) and return
normally, letting the post-check bail and the interpreter redo the store.
Only the throwing BigInt element converters (`ToBigInt64Element` /
`ToBigUint64Element` via `ToBigInt`) break the PreOp placeholder contract.

## Fix (shared, arch-neutral, one site)

`lib/Runtime/Language/JavascriptConversion.cpp`, `ToBigInt`: in the
`TypeIds_Undefined` case, when the undefined is a skipped-conversion
placeholder — `primitiveCoerced && IsDisableImplicitCall() &&
(flags & ImplicitCall_ToPrimitive)` — return a dummy zero BigInt instead of
throwing. The post-helper check then observes the flag, bails out, and the
interpreter performs the real conversion exactly once (`calls === 1`
preserved: skip(0) + redo(1); the dummy is never observed because the bailout
discards the JIT frame). A real `undefined` (no coercion, calls enabled, or
no skip) still throws per spec. The check is triple-guarded so it can only
fire for a genuine skip, and the dummy is only reachable under PreOp, which
always pairs the disable with the post-check.

## Regression test

New subtest in `test/es7/bigint64array.js`: "BigInt-element object stores
convert under JIT" — in-bounds const-index, dynamic-index, out-of-bounds, and
unsigned stores, each asserting `valueOf` runs exactly once and the converted
value is stored. It is shaped so the first store executes JIT-compiled
(later stores would run interpreted after a deopt, masking the bug).

## Verification (all on worktree `out/jit/bin/ch`)

- Min-repro flips: standalone const-index probe threw `TypeMismatch` with
  `calls=0` pre-fix; post-fix stores `3n` with `calls=1`. Number-valued
  object still throws WITH `calls=1` (correct: conversion ran, number
  rejected); `Object(3n)` and primitive `6n` stores unaffected.
- `test/es7/bigint64array.js` two-step recipe: pre-fix 9/10 (line 173),
  post-fix 11/11 (`Summary of tests: total executed: 11; passed: 11`).
  Baseline binary fails the new subtest too (11 tests, 9 pass, 2 fail:
  line 173 + the new test), confirming it reproduces.
- Full `runtests.py -t --variants interpreted dynapogo` before/after on the
  same build lineage: base 2022/301 → fixed 2023/300; es7 108+3 failed →
  109+2 failed. The fix resolves `bigint64array.js` with zero new failures.
  (Full-suite runs include many pre-existing environment failures, e.g. wasm
  0/64; untouched by this change.)
- Remaining es7 failures (`matchall.js`, `s6-growable-nonshared.js`) are
  pre-existing at this worktree's base commit `9d12aebf3` — the branch tip
  `e944f1f66` already contains their fixes (`JITREG-MA`, `JITREG-S6`).
  Verified they fail identically with and without this fix.
- `zig build check`: NOT run — unaffordable here (disk: the default-profile
  check needs a second full build; ~7 GB free after clearing stale
  multi-GB archives from interrupted builds). The JIT profile builds clean
  (`EXIT:0`, `install_profile` copies) and all JIT-relevant suites above
  were run instead.

## Working notes

- JIT IR tracing used: `-trace:TypedArrayTypeSpec,Lowerer`,
  `-trace:bailout`, `-trace:rejit` (baseline binary has no symbols; lldb
  breakpoints were not usable, so temporary `fprintf`/`Output::Print`
  probes in Lower + runtime were used and then fully reverted).
- Disk pressure: the jit profile's `libChakraCoreStatic.a` (~2.5 GB) is
  rewritten by libtool on every build; with a nearly-full disk the archive
  step fails while `ch` still links/installs. Two stale archives were
  removed from `.zig-cache` (superseded build-graph hashes); no source or
  repo files were touched for this.
- Base note: this worktree is pinned at `9d12aebf3` (detached), behind
  `modernize/baseline` tip `e944f1f66`. The fix applies cleanly to the
  shared runtime file either way; the s6/matchall deltas above are purely
  from those 4 newer branch commits.

## Follow-ups (stale — superseded by the fix above)

- ~~Reroute BigInt-element stores to the helper in shared label/branch
  code (Lower.cpp:17782-17815)~~ — REJECTED: block unreachable for BigInt
  bases (fast path never taken); the helper path was already used.
- ~~Add a ValueType predicate for Int64Array/Uint64Array~~ — REJECTED:
  unused once the Lower change was dropped; fix needs no JIT-side type test.
- Open: explicit `BigInt(obj)` conversion under PreOp has the same shape
  (fixed automatically by the shared `ToBigInt` change); DataView/Atomics
  BigInt setters funnel through the same converter and benefit identically.
- Open: consider a JIT-side assertion/debug aid that flags helpers which
  can throw on the PreOp placeholder (BigInt converters were the first).
