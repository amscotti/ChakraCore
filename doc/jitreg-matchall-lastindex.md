# JIT-REG card: matchall.js #15 — sloppy store to non-writable lastIndex goes through under FullJIT+PGO

Status: NOT a post-e14 regression. Same bug as the e14-carded "matchall #15
`lastIndex` sloppy-[[Set]] tiering flake" (`doc/open-cards.md`, commit d6663aa88);
reproduces identically at e14-green tree 69281b6e4 and pre-b5 tree 6c1470f9d,
now deterministic under the profile-input recipe.

Status 2026-11-15: FIXED in worktree (uncommitted) — guard added in
`GenerateFastStFldForCustomProperty`, red→green evidence below. Card retained
as the fix record.

Note on the parent report: "23/25 fail with `TypeError: Object doesn't support
property or method 'matchAll'`" is the shape of running matchall.js WITHOUT the
rlexe compile flag `-ES6RegExSymbols` (verified: flag-less run fails 23/25 in
every tier, including interpreted). Under canonical runtests (flags applied) the
only failure is #15 below.

## Failing test (1 subtest)

`test/es7/matchall.js`, compile flags `-ES6RegExSymbols`, test #15
"lastIndex [[Set]]: non-writable lastIndex throws on failed match reset
(defect fix 2)": line 309, `r2.lastIndex = 5` (sloppy, silently ignored per
spec) then `assert.areEqual(0, r2.lastIndex)` — actual `5`. 24/25 pass.

## Repro (deterministic given a profile)

```
CH=out/jit/bin/ch
$CH -ES6RegExSymbols -maxInterpretCount:1 -maxSimpleJitRunCount:1 -bgjit- \
  -dynamicprofilecache:/tmp/jitreg/p_ma matchall.js -args summary -endargs   # pass
$CH -ES6RegExSymbols -forceNative -off:simpleJit -bgJitDelay:0 \
  -dynamicprofileinput:/tmp/jitreg/p_ma matchall.js -args summary -endargs   # 24/25 fail
```

Minimal repro:

```js
var r2 = /a/g;
Object.defineProperty(r2, "lastIndex", { writable: false });
r2.lastIndex = 5;
if (r2.lastIndex !== 0) { print("FAIL: expected 0 got " + r2.lastIndex); throw new Error("lastIndex"); }
print("pass");
```

- With profile: FAIL (5/5 fresh profiles, 6/6 same-profile repeats).
- Without profile / interpreted: pass.

Store-through vs load-fold probe:

```js
var r2 = /a/g;
Object.defineProperty(r2, "lastIndex", { writable: false });
r2.lastIndex = 5;
print("load=" + r2.lastIndex + " desc=" + Object.getOwnPropertyDescriptor(r2, "lastIndex").value);
```

- PGO: `load=5 desc=5` — the STORE went through to memory despite the
  non-writable attribute (not a load-folding artifact).
- noPGO: `load=0 desc=0`.

## Evidence it predates e14

- Fails identically (line 309) on binaries built at 69281b6e4 and 6c1470f9d.
- Already carded at e14 time as a tiering flake ("IC/[[Set]]-bypass-after-
  transition hazard", 40/40 standalone-clean incl. profile-input recipe). The
  current determinism under the same recipe is consistent with a
  timing-sensitive guard gap whose distribution shifted, not with a new bug:
  the defect (store bypasses non-writable [[Set]]) is identical.
- Same e14-suite methodology caveat as the sibling cards (dynapogo-only run
  never feeds profiles, so PGO-only paths were uncovered).

## Narrowed suspect

JIT StFld fast path / inline cache performs the store on a type match without
revalidating the property's writability after the `defineProperty` attribute
transition (type changed or attributes mutated post-profile; the IC still hits
and stores). Suspect area: StFld inline-cache guard emission (type/attribute
guards) in shared Lower/GlobOpt StFld path + ARM64 `GenerateStFld*
FromLocalInlineCache` (`lib/Backend/arm64/LowerMD.cpp:7003-7045`); the e14 card
already names this hazard.

## Root cause (confirmed 2026-11-15)

Not the IC type-guard path: it is `Lowerer::GenerateFastStFldForCustomProperty`
(`lib/Backend/Lower.cpp`), which inlines `StFld lastIndex` on a LikelyRegExp
owner as a direct `[object + offset(lastIndexVar)]` store guarded only by the
object test + `JavascriptRegExp` vtable check. `lastIndex` is a virtual own
property backed by `lastIndexVar` + the `lastIndexNonWritable` flag
(`lib/Runtime/Library/JavascriptRegularExpression.{h,cpp}`):
`SetWritable(lastIndex, false)` flips the flag with no type change and no
inline-cache invalidation, so the vtable guard still passes post-`defineProperty`
and the inlined store runs. The slow helper (`SetPropertyBuiltIns` via
`PatchPutValue`) honors the flag (sloppy ignore / strict throw), which is why
interpreted and helper-fallback legs were always correct.

The defect is not ARM64-only: the fast path lives in arch-shared `Lower.cpp`
(no `LowerMD` involvement, no `#ifdef`), so x64/ARM/ARM64 emit the same gap.
No ARM64-only fork was made.

## Fix (in worktree, not committed)

- `lib/Runtime/Library/JavascriptRegularExpression.h`: added
  `GetOffsetOfLastIndexNonWritable()` (mirrors the existing offset getters).
- `lib/Backend/Lower.cpp` (`GenerateFastStFldForCustomProperty`): after the
  vtable guard, emit `cmp byte [object+off], 0; jne $helper` on
  `lastIndexNonWritable`. When set, control joins the normal StFld helper path,
  which performs the observable `[[Set]]` (sloppy ignore / strict throw).
  Flag-off (the common and all pre-ES6RegExSymbols cases): one extra
  compare, no behavior change.

Related site noted but out of scope: `GenerateFastInlineRegExpExec`
(`Lower.cpp`) unconditionally zeroes `lastIndexVar`/`lastIndexOrFlag` on the
BOI-literal no-match path with no writability check. The failing test's
throw-asserts pass because `@@match`/`@@replace` go through `SetLastIndexProperty`
(throw flags); a direct `.exec()` BOI-inline reset on a non-writable lastIndex
may still silently zero. Needs a regex/JIT owner to disposition.

## Evidence

Build: `zig build -Dprofile=jit` → `out/jit/bin/ch` (+ sibling dylib).
Min-repro (`var r2 = /a/g; defineProperty(writable:false); r2.lastIndex = 5`):

- Baseline binary, card recipe (collect with
  `-maxInterpretCount:1 -maxSimpleJitRunCount:1 -bgjit-
  -dynamicprofilecache:<dir>`, consume with `-forceNative -off:simpleJit
  -bgJitDelay:0 -dynamicprofileinput:<dir>`): FAIL `expected 0 got 5`;
  probe `load=5 desc=5` (store went through, not a load fold).
- Fixed binary, same recipe: `pass`; probe `load=0 desc=0`.
- `test/es7/matchall.js` (`-ES6RegExSymbols -args summary -endargs`), same
  two-step recipe: baseline 24/25 with test #15 failing at `:309`
  (`expected: 0, actual: 5`); fixed 25/25 (`pass` both legs).
- Strict-mode + writable sanity under the same PGO recipe: strict store throws
  `TypeError`, writable `r3.lastIndex = 5` still stores and reads back. Both pass.
- Canonical `python3 test/runtests.py -t -b out/jit/bin/ch --variants
  interpreted dynapogo` (full suite; the file positional arg is not honored as
  a file filter): interpreted es7 111/0, dynapogo es7 109/2 — `matchall.js`
  green in both legs. The 2 dynapogo es7 failures are the separately-carded
  pre-existing PGO issues `s6-growable-nonshared.js` (SAB grow staleness,
  `doc/jitreg-s6-growable-sab.md`) and `bigint64array.js` (typed-store
  ToNumber, `doc/jitreg-bigint64array-store.md`); all other failures are the
  known asm.js/WASM feature gap (`Bugs/fabs1.js` fails in the interpreted leg
  under `-asmjs` flags).

## Follow-up guidance

Fixed-here. `test/es7/matchall.js` test #15 is the maintained pin for this exact
behavior (sloppy-ignore store + strict-throw resets); no new test file was
added — the smallest durable collateral is the existing test, which flips
red→green under the PGO recipe above. Suggested: add a `-j10` load leg for the
old flake shape per the e14 card, and disposition the
`GenerateFastInlineRegExpExec` zeroing site noted above.
