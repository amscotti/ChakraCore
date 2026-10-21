# C2-S1: direct-eval SuperProperty early errors

**Status: pending** (integration/B4 review not yet recorded).

Lane `evalsuper`, base `9a99ccbee`. Scope: the 10 failing direct-eval
`super-prop*` scenarios from the wave-3 triage. This slice does not change the
shared plan/triage or the regression gate pins.

## Problem

`Parser::ParseSuper` allowed `super.property` / `super[expr]` at the top level of
any `fscrEval` parse. In eval code called from global code, a plain function or
an arrow, there is no caller [[HomeObject]], so the reference compiled and threw
a runtime `ReferenceError` (`Missing or invalid 'super' binding`). Test262
requires a `SyntaxError` (and, for `super[evaluated = true]`, requires the
property expression not to run).

## Design

PerformEval decides `inMethod` from the eval caller's environment (it has a
[[HomeObject]]), not from the source text alone:

- `lib/Parser/Parse.cpp`: the eval top-level SuperProperty allowance now also
  requires `m_scriptContext->IsEvalInMethod()`. Everything else falls through to
  the existing `ERRInvalidSuper` early error. The nested-plain-function rule
  (`currentNonLambdaFunc == nullptr`) and `m_disallowSuperCallInEval` are
  unchanged; `super()` in eval keeps its existing constructor-only checks.
- `lib/Runtime/Base/ScriptContext.{h,cpp}`: transient `evalInMethod` flag,
  saved/restored around `EvalHelper` like the existing field-initializer flags.
- `lib/Runtime/Library/GlobalObject.cpp`: computes `evalInMethod` from the
  direct eval caller's `FunctionInfo::HasHomeObj()`, class-field-initializer
  context (field and static-block helpers have a [[HomeObject]] without the
  `HomeObj` bit), or a direct-eval body marker for nested eval. Indirect eval
  never sets it.
- `lib/Runtime/Base/FunctionBody.{h,cpp}`: `m_isEvalInMethod` marks a direct
  eval body's inMethod context so a nested direct eval inherits it. It is
  refreshed on each eval call (so a cached body follows its current caller),
  copied in `ParseableFunctionInfo::Copy` and the proxy constructor, and is not
  serialized; bodies containing `super` already bypass the eval map.
- Arrow callers have no home object of their own. When the arrow is active, a
  stack walk past lambdas/eval bodies finds the lexical non-lambda caller;
  global arrows (`EnclosedByGlobalFunc`) and arrows nested in plain functions
  still fail closed.

## Evidence

All builds/tests were run under `/tmp/opencode/cc-gate-lock.sh`; no
`--prepare` and no full `zig build check`.

Focused `test/es7/eval-super-early.js` (new; three rlexe entries: default,
`-force:deferparse`, `-noDeferParse`):

- test profile, `--variants=disable_jit`: **passed 3, failed 0**
  (`python3 test/runtests.py -t --variants=disable_jit --binary $PWD/zig-out/bin/ch test/es7/eval-super-early.js`).
- debug-static, `--variants=disable_jit`: **passed 3, failed 0**.
- Existing regression files `class-fields-public`, `class-static-fields`,
  `class-static-blocks`, `eval-arguments-declarations` plus the new file:
  **10/0** (test and debug-static); adding `asyncawait-syntax.js` gives
  **12/0** (test), covering the repair-loop nested-plain-function and
  eval/await behavior.

test262 (pinned corpus `2e0a5676`, `tools/test262/run.py`, host flags
`-nonative -Test262 -CollectGarbage`):

| run | base (`ChakraCore/zig-out/bin/ch`) | this lane |
|---|---:|---:|
| `test/language/eval-code/direct/super-prop*.js` | 2 passed / 10 failed (12) | **12 passed / 0 failed (12)** |
| `test/language/eval-code/**/*.js` | 444 / 10 failed (454) | **454 / 0 failed (454)** |
| class elements statements+expressions | 5,888 / 9 failed (5,897) | 5,888 / 9 failed (5,897) |

Scenario diff for the focused and full eval-code runs: exactly the 10
`super-prop*` default/strict failures flipped **fail -> pass**, **0 pass ->
fail**. The class-elements run has **0 scenario changes**; its 9 failures are
the pre-existing private-nonextensible, BigInt-runner and field-accessor
residuals.

Artifacts (scratch, not committed): `out/test262-results-evalsuper-base*`,
`out/test262-results-evalsuper-final`, `out/test262-results-evalsuper-final-elements`.

## Residual work and risks

- An arrow created inside a method but invoked after the method has returned
  can still reject a direct eval `super.property`: the marker is not yet
  propagated to arrow `FunctionBody`s, and the active-frame fallback has no
  method frame to find. Test262 only covers the global arrow.
- The `m_isEvalInMethod` bit is not serialized. Eval bodies with `super` are
  excluded from the eval map, so nested eval through `-ForceSerialized` /
  snapshot paths was not exercised and remains a B4 check.
- JIT/backend was not exercised by these runs (`-nonative` and
  `--variants=disable_jit`). **B4 must run the focused test262 subset plus
  `test/es7/eval-super-early.js` and the class regression files under
  SimpleJIT/FullJIT before any JIT/default claim.**
- The three new rlexe entries raise the regression selection from 2016/2068 to
  an expected 2019/2071. Per the lane instruction the shared
  `tools/build/gate_counts.json` pins were deliberately not edited; integration
  must bump them with the other lane counts.

## Files

- `lib/Parser/Parse.cpp`
- `lib/Runtime/Base/FunctionBody.{h,cpp}`, `lib/Runtime/Base/ScriptContext.{h,cpp}`
- `lib/Runtime/Library/GlobalObject.cpp`
- `test/es7/eval-super-early.js` (new), `test/es7/rlexe.xml`
- `doc/c2-s1-eval-super.md` (this file)
