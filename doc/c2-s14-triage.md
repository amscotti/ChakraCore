# C2-S14 triage: residual inherited-semantics clusters

Status: `<pending>` — analysis-only follow-up plan. No engine source, gate
pin, or shared plan/triage document was changed. Base `10d44c972`; evidence
recorded 2026-10-18.

## Method and evidence

The recorded baseline is
`out/test262-zig/results.json` (93,141 scenarios, 61,565 passed / 31,576
failed; test262 `2e0a56762801e275a9fdf96dc49d90ba0cddcf63`, binary
`f2817d95…a0e29e`, `-nonative -Test262 -CollectGarbage`). The committed
aggregate is `tools/test262/c1-baseline-analysis.json`: 19,648
edition-selected failures, of which 3,822 are
`engine-defect-on-existing` and 1,460 are untagged.

The artifacts below are ignored files under the main checkout's `out/`
(`/Users/ascotti/Documents/code/ChakraCore/out/`); this worktree's `out/`
only links the corpus. Post-S14 evidence exists only for the scoped
corpora in `out/test262-c2-s14-blockscope-final4` (287/287),
`out/test262-c2-s14-for-final` (740/758),
`out/test262-c2-s14-switch-final` (188/216),
`out/test262-c2-s14-try-final` (359/388) and
`out/test262-c2-s14-with-final` (170/182). All five use binary
`3d1bd3db…2145c` and the same pinned revision. The less complete
`out/test262-c2-s14-catch` run (355/388) is superseded by `try-final`.

Counts below were obtained by applying the same edition-selection and
failure-bucket rules as `tools/test262/analyze.py` to the recorded JSON and
grouping by file/glob and failure message. No corpus was re-run. Except
where marked “current”, counts are **baseline fail→pass ceilings**, not
claims about the current binary. All counts are scenarios; a file may carry
two strictness scenarios.

The six primary sets are disjoint by file and total **635 scenarios**
(564 failing files). They are not a “remaining defect bucket”; each is a
separate rule or runtime mechanism with a scoped regression universe.

## Candidate table

| ID | Cluster | test262 selection | Fail / total | Root class | S1 conflict | Negative risk | Batch |
|---|---|---|---|---:|---|---|---|---:|
| P1 | Statement completion values | `test/language/statements/**/cptn-*.js`; `statements/try/completion-values.js`; `statements/for/head-init-*-completion.js` (exclude `statements/class/**`) | 112 / 152 | Bytecode/runtime: empty completion, `UpdateEmpty`, explicit `undefined` | Wait: `ByteCodeEmitter.cpp` | Low (positive tests) | 60 current, then 112 |
| P2 | Direct-eval `arguments` declaration errors | `test/language/eval-code/direct/*arguments*.js` | 157 / 192 | Parser/eval declaration instantiation | Wait: `Parse.cpp` + eval emit | Medium | 157 |
| P3 | Annex B block-function var-extension skip | `test/annexB/language/{eval-code,function-code,global-code}/**/*skip-early-err*.js` | 264 / 280 | Parser + global/eval/function declaration instantiation | Wait: `Parse.cpp` | Medium-high | 264 |
| P4 | Global/eval declaration-instantiation collisions | `test/language/global-code/**/*.js` (19/75); `eval-code/{direct,indirect}/var-env-*.js` (7/59); `**/eval-var-scope-syntax-err.js` (16/16) | 42 / 150 | Runtime global/eval environment | Low-medium; confirm emitter touch | Medium | 19 + 23 |
| P5 | Assignment/PutValue reference base | `expressions/assignment/S11.13.1_A[56]*.js`; `expressions/compound-assignment/S11.13.2_A[56]*.js` | 50 / 50 | Bytecode/runtime reference and scoped store | Wait: `ByteCodeEmitter.cpp` | Low negative, high regression | 50 |
| P6 | `for-in` / own-property enumeration order | `statements/for-in/order-*.js`; `built-ins/Object/{keys,values,entries}/return-order.js`; `built-ins/JSON/stringify/property-order.js` | 10 / 18 | Runtime property enumeration | Parallel-safe (type handlers) | Low (positive tests) | 10 |

## P1 — statement completion values (112)

Current evidence: `try-final` 24, `for-final` 12, `switch-final` 22 and
`with-final` 2 completion failures, so the cluster survives S14 and is not
a parser issue. The baseline adds `for-in` 16, `for-of` 12, `if` 14,
`while` 6 and `do-while` 4; two `statements/class/cptn-decl.js`
scenarios are excluded as S1-owned.

Representative failure: `eval('1; if (false) { }')` must evaluate to
`undefined` but returns `1`; likewise `eval('1; switch ("a") { case "a":
break; }')` and `eval('1; for (var a in { x: 0 }) { }')`. A second form is
`eval('for (var count=0;;) { if (count===4) break; count++; }')`, which
must be `undefined` but returns the last statement value.

The tests are positive assertions over the eval completion value. The
likely edit surface is statement emission in
`lib/Runtime/ByteCode/ByteCodeEmitter.cpp` and the result register consumed
by `lib/Runtime/Language/InterpreterStackFrame.cpp`. It is disjoint from
private-name/brand work, but shares the emitter with S1: schedule after
S1, then rebase. First gate the 60 current-confirmed scenarios (try 24,
for 12, switch 22, with 2) and require 60/0; then run the full
152-scenario selection and require 112 fail→pass, 0 pass→fail.

## P2 — direct-eval `arguments` declaration errors (157)

Evidence: baseline `out/test262-zig/results.json` only; no post-S14
`eval-code/direct` rerun exists.

All 157 failures are in one directory and one binding rule:

```js
function f(p = eval("var arguments")) { let arguments; }
assert.throws(SyntaxError, f);
```

The generated corpus covers function declarations/expressions, arrows,
methods, generators, async functions and async generators, with
`arguments` as a preceding/following parameter, a `var`/`let`/function
body binding, or no pre-existing binding. Engine outcomes are mostly “no
exception” (88) or `ReferenceError` instead of `SyntaxError` (48), plus 16
wrong-value results and 5 `'arguments' is not defined`.

Root is the direct-eval declaration path: the parser/eval instantiation
must reject a sloppy eval `var arguments` when it would collide with an
existing parameter/lexical/var binding, without rejecting legal eval uses.
Likely files: `lib/Parser/Parse.cpp` (parameter/eval binding checks) and
the eval declaration/emission path in
`lib/Runtime/ByteCode/ByteCodeGenerator.cpp`. Do not broaden this into a
general `arguments` check: 35 scenarios in the same selection pass and are
the regression controls. Wait for S1 because the parser file is shared.

## P3 — Annex B block-function var-extension skip (264)

Evidence: baseline `out/test262-zig/results.json` only. S14 explicitly
left the Annex B synthetic `var` alias out of its block rule, so these
counts are expected to be unchanged, but no post-S14 Annex B rerun exists.

This is the “TDZ” cluster, but it is not a general TDZ defect: all 240
scenarios with the message “An initialized binding is not created …” are
Annex B `*-skip-early-err-*` tests. The remaining 24 are the same rule's
parse-side symptom: the engine raises `SyntaxError: Let/Const
redeclaration` where the Annex B extension should simply not be created.

Breakdown: 112 direct-eval, 56 indirect-eval, 48 function-code and 48
global-code scenarios. Representative:

```js
(function() {
  assert.throws(ReferenceError, function() { f; });
  { let f = 123; { function f() {} } }
  assert.throws(ReferenceError, function() { f; });
}());
```

Also `let f = 123; { function f() {} }` inside direct/indirect eval must
not throw at parse time. The engine currently creates the synthetic
function-scoped `var f` unconditionally for sloppy block functions
(`Parser::ParseFncDeclInternal`, near the `isBlockScopeFncDeclVar`
creation), and the bytecode/global/eval instantiation paths then expose or
reject it. The correct condition is the spec's “replacing the function
declaration with a `var` statement would not produce early errors” test;
the enclosing block's lexical names and the eval/global var environment
both participate.

Keep the existing valid Annex B controls green: 16 passing scenarios in
the 280-selection, the 287/287 block-scope corpus, and
`test/es7/blockscope-early-errors.js`. Negative-test risk is medium-high
because an over-broad skip changes legal block-function hoisting.
`syntax/switch/scope-lex-{generator,async-function,async-generator}.js`
(3 current failures) is the same synthetic-var site: generator/async block
functions must not receive the ordinary-function extension. Include them
as extra controls, not as a separate batch.

## P4 — global/eval declaration-instantiation collisions (42)

Evidence: baseline `out/test262-zig/results.json` only; no post-S14
`global-code` or eval-code rerun exists.

Three entry points share the same spec operation but should be measured
separately:

- `test/language/global-code/**/*.js`, 19/75: missing `SyntaxError` for
  `let`/`var`/function collisions, wrong `ReferenceError` for restricted
  globals, and missing `TypeError` for non-configurable/non-extensible
  global function bindings. Representative:
  `eval('var test262Var;'); $262.evalScript('let test262Var = 1;');`
  must leave `test262Var === 1`, but it is `undefined`; and
  `let x; eval('var x;')` must throw `SyntaxError`.
- `eval-code/{direct,indirect}/var-env-*.js`, 7/59: lower-lexical and
  global-lexical collisions, plus the non-configurable global function
  descriptor. `var-env-lower-lex-non-strict.js` is the smallest
  `{ let x; eval('var x;'); }` reproducer.
- `**/eval-var-scope-syntax-err.js`, 16/16: parameter-default direct eval
  (`function f(a = eval("var a = 42")) {}`) must throw `SyntaxError`
  before the body runs.

This is runtime/global scope, not parser syntax, except for the eval
parse/instantiation boundary. Likely files are
`lib/Runtime/Library/GlobalObject.cpp`, the global-binding code in
`lib/Runtime/Types/DictionaryTypeHandler.cpp`, and the eval declaration
path in `ByteCodeGenerator.cpp`/`ByteCodeEmitter.cpp`. The 108 passing
scenarios are the immediate controls. Because the emitter may be touched,
confirm S1 ownership before starting; otherwise this can run in parallel
with parser-only class-field work. Split the batch 19 + 23 if a single
change spans both entry points.

## P5 — assignment/PutValue reference base (50)

Evidence: baseline `out/test262-zig/results.json` only; no post-S14
assignment/compound-assignment rerun exists.

The engine re-resolves an already-evaluated identifier reference at store
time. Two representative families:

```js
var x = 0, scope = { get x() { delete this.x; return 2; } };
with (scope) { x *= 3; }          // scope.x must be 6, outer x must stay 0
```

```js
var x = 3;
(function() { x *= (eval("var x = 2;"), 4); return x; })(); // must be 2
```

Actual results are `scope.x === undefined` and `innerX === 12`,
respectively. The 44 compound-assignment and 6 assignment scenarios are
all failing, with no negative tests. Root is `EmitAssignment` using a
fresh `EmitPropStore` for `knopName` instead of the environment/reference
captured by `EmitReference` (`lib/Runtime/ByteCode/ByteCodeEmitter.cpp`);
object-environment cases also exercise
`lib/Runtime/Language/JavascriptOperators.cpp` and
`InterpreterStackFrame.cpp`. This is adjacent to, but disjoint from, the
eval-var collision tests in P4. Wait for S1 on the emitter; regression
risk is high because every identifier store uses this path.

## P6 — property enumeration order (10)

Evidence: baseline `out/test262-zig/results.json` only; no post-S14
`for-in`/`Object`/`JSON` rerun exists.

The `for-in-order` tag has 18 scenarios, 10 failing in five files:
`statements/for-in/order-simple-object.js`,
`built-ins/Object/{keys,values,entries}/return-order.js` and
`built-ins/JSON/stringify/property-order.js`. Reproducer: create
`p1,p2,p3`, add `p4`, add numeric keys, delete `p1`/`p3`, then re-add
`p1`; expected order is `0,1,2,p2,p4,p1`, actual is `0,1,2,p1,p2,p4`.
The deleted/re-added property retains its old dictionary position.

Root is the dictionary/type-handler enumeration order, documented as
unordered after delete-and-re-add in
`lib/Runtime/Types/SimpleDictionaryTypeHandler.h` and implemented in
`SimpleDictionaryTypeHandler.cpp`/`DictionaryTypeHandler.cpp`. This is
runtime-only and the only primary candidate expected to be safe alongside
S1. It is low impact, so use it as a parallel filler after P1 starts.

## Small residuals and exclusions

| Residual | Evidence | Disposition |
|---|---|---|
| `with` object-environment proxy traps | 8/13 in `with/{get,set}-mutable-binding-*proxy-env.js`, `*-deleted-in-get-unscopables*`, `unscopables-inc-dec.js`; current artifact | Runtime-only optional batch. Extra `@@unscopables` get and missing `HasProperty` in `SetMutableBinding`; same area as P5. |
| Catch simple-identifier var in `for-in`/`for-of` | 4/10 in `annexB/language/statements/try/catch-redeclared-for-{in,of}-var.js` | Pair with P5/P4. Classic `for (var err…)` passes; only for-in/of binding linkage fails. |
| Catch pattern vs `var` early error | **No test262 scenario found**; probe-only `try {} catch ({e}) { var e; }` | Do not claim corpus gain. Add a focused local test if implemented; maintain the simple-identifier Annex B exception. |
| `switch` generator/async block-function leak | 3/20 in `statements/switch/scope-lex-*.js` (current) | Same parser/bytecode site as P3; include as controls in P3. |
| Module TDZ | 22 scenarios in `module-code/instn-*`, namespace internals, TLA | Exclude as S12/module-work-adjacent; not an S14 batch. |
| Class completion/static-block await | 2 `statements/class/cptn-decl.js` + 2 `try/static-init-await-binding-valid.js` | Exclude: S1 class-field/static-block ownership. |
| Remaining untagged parse negatives | Baseline 159 untagged `$DONOTEVALUATE`; post-S14 scoped artifacts show 0 remaining parse-phase negatives in block-scope, for, switch, try and with | No reliable remaining list from the existing artifacts; do not batch. A full post-S14 rerun would be needed before claiming these. |

### Residual-specific findings

- **Catch pattern-vs-var.** The S14 statement batch only checks catch
  BoundNames against the body's `LexicallyDeclaredNames`; the
  pattern-vs-`VarDeclaredNames` rule is absent. There is no test262
  coverage of the pattern form. The nearest corpus defect is the
  simple-identifier `for-in`/`for-of` linkage above, which is runtime
  catch-scope binding, not the parser early-error rule.
- **Eval-introduced `var`.** P4 covers 42 declaration-instantiation
  failures. P5's `S11.13.*_A6` subset (14 scenarios) covers the related
  dynamic `eval('var x')` shadowing of an already-evaluated assignment
  target. They are disjoint files but should be sequenced so the same
  eval-scope debugging is reused.
- **`for-in-order`.** P6 is genuinely small and independent; it is not a
  proxy for the larger untagged bucket.
- **TDZ/global scope.** P3's 240 “initialized binding” messages are not a
  general TDZ defect; they are all Annex B block-function-instantiation
  tests. The remaining real TDZ cases are the 22 module scenarios
  (excluded) and the small `switch`/`with` residuals above.
- **Larger untagged bucket.** The 1,460 untagged edition failures and
  1,314 untagged defect-bucket failures bisect into the named sets:
  P3 264, P1 ~110, P5 50, P2 93 of 157, P4 22 of 42 — roughly 540
  scenarios across about 475 files. The remainder is not one mechanism;
  it includes baseline-only parse negatives that S14 may already have
  fixed, feature-specific gaps already assigned to other C2 items, and
  host/edition-policy edge cases. Do not create a “fix the untagged
  bucket” batch.

## Recommended Wave-2 order

Wave 2 here means after S1 lands and the S1-owned files are available for
rebase. The order is chosen for risk-adjusted impact, not raw count:

1. **P1 — statement completion values, 60 then 112.** Exact next batch.
   Current post-S14 artifacts already isolate 60 of them; all tests are
   positive; one completion-value rule; no parser change. Gate the 60
   current scenarios 60/0 first, then extend to the 152-scenario
   selection and require 112/0 flips.
2. **P2 — direct-eval `arguments`, 157.** Second-largest clean parser/eval
   cluster; 157 fail→pass, 35 passing controls. Do not broaden.
3. **P3 — Annex B skip, 264.** Largest ceiling, but highest negative-test
   risk and parser-owned. Do it after P2 so the eval/declaration debugging
   is available; include the 3 `switch` generator/async controls.
4. **P4 — global/eval declaration instantiation, 42.** Start with the
   19-scenario global-code half, then the 23-scenario eval half. This area
   can reuse P2/P3 diagnostics; do not run all 150 as one change unless
   the first half is clean.
5. **P5 — reference base, 50.** Independent bytecode fix, but the highest
   regression risk in the table; wait until P1 has settled the emitter.
6. **P6 — enumeration order, 10.** Parallel filler; can start immediately
   if S1 does not touch the type handlers.

Optional parallel lane: the 8 `with` object-environment scenarios. Keep
the catch simple-var (4) and `switch` leak (3) as folded controls rather
than standalone batches.

For every batch: record the pre/post scoped artifact, list exact flips,
require 0 pass→fail on the selection plus the existing block-scope and
focused es7 suites, and do not move gate pins in the trial commit. JIT
claims remain B4-owned.

## Overlap and blocked notes

- `s1-fields` was observed at `10d44c972` (no class-field commit in the
  branch at analysis time). The conflict assessment is therefore by
  predicted file ownership, not an observed diff. S1 is expected to touch
  `lib/Parser/Parse.cpp`, `lib/Parser/ptree.h`,
  `lib/Runtime/ByteCode/ByteCodeEmitter.cpp`,
  `lib/Runtime/ByteCode/ByteCodeGenerator.cpp` and the private-field/type
  layer. P1, P2, P3 and P5 clearly touch one of those; P4 may touch the
  eval emitter but its primary edit can stay in the runtime global
  environment. Sequence P1–P3/P5 after S1; P4 can start in parallel only
  if its first change stays out of the S1 files. P6 and the `with` lane
  are the runtime-only candidates expected to be safe alongside S1.
- Do not count the class `cptn` (2) or static-block await (2) scenarios;
  they belong to S1.
- Do not count module TDZ (22), dynamic import (171), RegExp
  prototype/literals (50+), DataView (110), Array copy-by-change (16),
  Set methods (232) or BigInt/TypedArray clusters; those are S3/S4/S8/
  S9/S10/S11/S12 or feature-completion work, not S14.
- P3's 24 “Expected no error, got SyntaxError” cases and 240 missing-TDZ
  cases are one rule. Splitting them into a parser batch and a runtime
  batch would duplicate the same condition and is not recommended.
- P2's 157 tests are one binding rule but eight syntactic forms. A batch
  may land in one parser/eval change; if the diff grows beyond
  `Parse.cpp` plus one eval-instantiation helper, stop and split by form.

## Not verifiable from the recorded artifacts

- Current post-S14 status for P2, P3, P4, P5 and P6: only the baseline
  JSON exists; no scoped rerun of eval-code, global-code, Annex B,
  assignment or enumeration corpora. Their counts are ceilings.
- P1's 52 baseline-only scenarios outside try/for/switch/with (for-in,
  for-of, if, while, do-while) were not rerun after S14; the 60 current
  scenarios are confirmed.
- No test262 case exercises catch pattern-vs-`var`; that rule can only be
  probed or covered by a new local test.
- The baseline is pre-S1/pre-remainder; it cannot prove which 159 untagged
  parse-negative executions remain after the S14 block/statement batches.
- Negative-phase evidence is the existing heuristic; no new audit was run
  in this triage.
- JIT/interpreter parity is not assessed; B4 owns JIT configuration and
  claim.
