# C2-S1: direct-eval SuperProperty early errors

**Status: landed** (merge `ea8f253ac`; final review fix `39e9f2a16`;
final gates **2020/0 test**, **2072/0 debug-static**). B4 JIT/Linux and
serialization checks remain open.

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
- `lib/Parser/ptree.{h,cpp}`, `lib/Parser/Parse.cpp`: each function node records
  a lexical `hasHomeObjectContext`. Methods, accessors, constructors and the
  synthetic field/static-block helpers set it; arrows inherit it from the
  lexical non-lambda function (or from the reparsed body / eval caller context
  when no enclosing node is available). Plain functions do not inherit.
- `lib/Runtime/ByteCode/ByteCodeGenerator.cpp`: copies that parser hint to the
  function/arrow body (first parse and deferred reuse) and keeps it across
  deferred bodies. The eval program body takes it from the direct-eval context.
- `lib/Runtime/Library/GlobalObject.cpp`: computes `evalInMethod` from the
  direct eval caller's `FunctionInfo::HasHomeObj()`, class-field-initializer
  context (field and static-block helpers have a [[HomeObject]] without the
  `HomeObj` bit), or the caller body's lexical marker. Indirect eval never sets
  it. There is no active-stack inference: an arrow created in a plain function
  stays SyntaxError even when invoked under a method.
- `lib/Runtime/Base/FunctionBody.{h,cpp}`: `m_hasHomeObjectContext` carries the
  lexical marker through arrows and direct-eval bodies, so nested direct eval
  inherits it. It is refreshed on each eval call (so a cached body follows its
  current caller), copied in `ParseableFunctionInfo::Copy` and the proxy
  constructor, and is not serialized; bodies containing `super` already bypass
  the eval map.

## Evidence

All builds/tests were run under `/tmp/opencode/cc-gate-lock.sh`; no
`--prepare` and no full `zig build check`.

Focused `test/es7/eval-super-early.js` (new; three rlexe entries: default,
`-force:deferparse`, `-noDeferParse`):

- test profile, `--variants=disable_jit`: **passed 3, failed 0**
  (`python3 test/runtests.py -t --variants=disable_jit --binary $PWD/zig-out/bin/ch test/es7/eval-super-early.js`).
- debug-static, `--variants=disable_jit`: **passed 3, failed 0**.
  The file covers the false-accept side effect case (arrow lexically in a plain
  function invoked under a method: `SyntaxError`, key not evaluated), arrows in
  methods through wrappers / after return / created by eval, nested eval arrows,
  constructor arrows running `super()`, static field/block helpers, and
  accessor/async/generator methods, plus repeated eval in all three parse modes.
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
residuals. The merged binary was re-run on the full eval-code corpus on
2026-10-21: **454/0** (`out/test262-final-evalsuper`).

Artifacts (scratch, not committed): `out/test262-results-evalsuper-base*`,
`out/test262-results-evalsuper-fix2-superprop`,
`out/test262-results-evalsuper-fix2-evalcode`,
`out/test262-results-evalsuper-fix2-elements`.

## Residual work and risks

- The lexical marker is not serialized. Ordinary methods/constructors still
  hit the `FunctionInfo` fallback after deserialization, but a deserialized
  arrow/body that later performs direct eval can lose its inherited context.
  `-ForceSerialized` passes the focused file and matrix in the final review,
  but snapshot/TTD nested-eval paths were not exercised and remain a B4 check.
- An eval function body cached under one caller can be reused across callers;
  the body marker is refreshed per call, but arrows already created inside that
  cached body keep the marker they were generated with. Eval sources with a
  literal `super` bypass the eval map; dynamically constructed super names are
  the residual case.
- Pre-existing super-property evaluation order before `super()` remains: a
  computed `super[expr]` in a derived constructor can evaluate `expr` before
  the missing-binding ReferenceError. Node/Bun also differ from each other on
  the write case. This is outside the eval-super diff and is tracked as a
  constructor/super-order residual for B4 review.
- Object-literal function/arrow property values no longer receive a
  [[HomeObject]]; concise methods and accessors still do. `EmitMemberNode`
  records a home-object location only when the parser marked one. This fix
  required regenerating only the no-JIT `JsBuiltIn` bytecode header; the JIT
  header remains a B4 regeneration item.
- JIT/backend was not exercised by these runs (`-nonative` and
  `--variants=disable_jit`). **B4 must run the focused test262 subset plus
  `test/es7/eval-super-early.js` and the class regression files under
  SimpleJIT/FullJIT before any JIT/default claim.**
- The three new rlexe entries raised the regression selection by three. The
  S6 lane added one more, and integration set the single-source pins from
  2016/2068 to **2020/2072**; full `zig build check` and the debug-static
  regression gate are green at those counts.

## Integration

Merged as `ea8f253ac` on top of the S6 merge `e3371d313`; the final
gate/pin and debug-invariant commit is `9fceb6fe4`, and the legacy-expectation
commit is `e8afc8101`. The full gate initially failed six legacy
`es6/classes.js`, `es6/ES6Super.js` and `es6/bug_981217` scenarios because
those fixtures still expected the old runtime `ReferenceError`. They now
expect the spec early `SyntaxError`, and the `super[value = 0]` control asserts
that the computed key is not evaluated. This is a test-expectation correction,
not an engine behavior waiver. The final adversarial review then found that
object-literal function/arrow property values were still treated as methods;
fix `39e9f2a16` removes that false home object, guards `EmitMemberNode`, and
adds the object-literal/arrow cases to the same rlexe entries. The no-JIT
`JsBuiltIn` header was regenerated and the full gates re-ran without a count
change: **2020/0 test**, **2072/0 debug-static**, **454/0 eval-code** after the
fix.

## Files

- `lib/Parser/Parse.cpp`, `lib/Parser/ptree.{h,cpp}`
- `lib/Runtime/ByteCode/ByteCodeGenerator.cpp`,
  `lib/Runtime/ByteCode/ByteCodeEmitter.cpp`
- `lib/Runtime/Library/InJavascript/JsBuiltIn.nojit.bc.{32b,64b}.h`
- `lib/Runtime/Base/FunctionBody.{h,cpp}`, `lib/Runtime/Base/ScriptContext.{h,cpp}`
- `lib/Runtime/Library/GlobalObject.cpp`
- `test/es7/eval-super-early.js` (new), `test/es7/rlexe.xml`
- `doc/c2-s1-eval-super.md` (this file)
