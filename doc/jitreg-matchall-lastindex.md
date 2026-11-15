# JIT-REG card: matchall.js #15 — sloppy store to non-writable lastIndex goes through under FullJIT+PGO

Status: NOT a post-e14 regression. Same bug as the e14-carded "matchall #15
`lastIndex` sloppy-[[Set]] tiering flake" (`doc/open-cards.md`, commit d6663aa88);
reproduces identically at e14-green tree 69281b6e4 and pre-b5 tree 6c1470f9d,
now deterministic under the profile-input recipe. Carded for follow-up
(regex/JIT lane). Do NOT fix here.

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

## Follow-up guidance

Needs a regex/JIT owner: add/strengthen the writability guard on the StFld fast
path (or force the helper on attribute transitions), with this test's PGO recipe
as the regression (24/25 -> 25/25) plus a `-j10` load leg for the flake shape.
