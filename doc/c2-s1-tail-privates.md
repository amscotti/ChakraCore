# C2-S1 tail: direct-eval/computed-key private visibility + private field semantics

Status: **implemented on `lane/s1-tail-privates`** (base `aab65ecfb`);
gates **2012/2064** (`check` 2012/0, `check-regression -Dprofile=debug-static`
2064/0). Evidence: integrated class-elements corpus **5,888 passed / 9 failed**
(5,897 scenarios, 2,962 files), **34 fail -> pass / 0 pass -> fail** vs wave2
baseline (`out/test262-c2-s1-wave2`, 5,854/5,897); es7 private files 17/17 and
34/34 in Release and debug-static under `-forcedeferparse`/`-nodeferparse`/
`-noDeferParse`.

This slice closes the 39 remaining ES2025 scenarios of the integrated
class-elements corpus: 24 direct-eval private visibility, 8 computed-key
private visibility, and 7 private-field semantics (2 evaluation-order, 4
primitive-receiver, 1 nonextensible-strict). The 4 post-ES2025
accessor-grammar scenarios stay out of scope.

## Scope and observable semantics

- A private name used inside a direct `eval` that runs in a class element
  context (method, accessor, field initializer, static block) resolves to the
  enclosing class's brand. `eval("this.#x")` reads/writes/calls through the
  same brand map as static code; `eval` in a nested function of a method sees
  the method's class brand; an unbranded receiver or unknown `#name` throws
  the same `TypeError`/`SyntaxError`/`ReferenceError` static code produces.
- A computed property key may read a private name of the class being defined
  (e.g. a field named `[this.#f]` or `[C.#brand]`): same-function brand loads
  no longer raise a static use-before-declaration error.
- Object-destructuring assignment follows
  `PropertyDestructuringAssignmentEvaluation`: key, then target reference, then
  value Get. A throwing key (including a brand check) fires before the target
  base is evaluated; the value Get still precedes the store.
- Private-field primitive-receiver get/put scenarios pass (verified under
  `-ESBigInt`; the default runner lacks BigInt literals, a pre-existing gap).
- Residual fails: 1 nonextensible-strict scenario from the Stage-1
  `nonextensible-applies-to-private` proposal (Node also fails it), and 4
  post-ES2025 accessor-grammar scenarios (out of scope by lane contract).

## Parser/AST design

- The hidden brand const pid is now spelling-derived
  (`PidForHiddenPrivateName`: `*s1private#name*`) instead of counter-derived
  (`*s1privateN*`). Direct eval is parsed separately from its caller, so a
  counter value is unknowable at the eval parse; the spelling-derived pid lets
  the emitter and the eval-entry check agree on the same slot name.
- `ParseNodeName::isDynamicPrivateName` marks `#x` references that occur in a
  direct-eval program (`pnodeEvalPrivateBrands` on `ParseNodeProg`,
  `CreateDynamicPrivateName`/`RecordEvalPrivateBrand`). Static class-body
  references keep the existing hidden-const binding; only eval-parsed
  references take the dynamic path, so sibling lanes are unaffected.
- `CreatePrivateNameReference`/`ResolvePendingPrivateRefs` route dynamic refs
  through the new path; static resolution is unchanged.
- `VEval` (`GlobalObject.cpp`) skips `fscrCanDeferFncParse` when the source
  contains `#` (`mayContainPrivateRef`): an eval string that may reference a
  private name is never deferred, so brand checks are always emitted for it.

## Emitter/runtime design

- `EmitPrivateBrand` defaults `skipUseBeforeDeclarationCheck=true`: the brand
  store provably dominates same-function brand loads, and the old static TDZ
  error was a false positive on computed keys. Cross-function loads still go
  through the normal declaration check.
- Direct eval has three cooperating pieces:
  - `OP_LdPrivateBrand` (ElementP scope walk): resolves the spelling-derived
    brand const from the caller scope chain at eval-entry; absent means
    `SyntaxError`, undeclared means `ReferenceError`.
  - `EmitEvalPrivateBrandChecks` at the top of `EmitOneFunction` for eval
    programs carrying private brands: entry checks give early-error semantics
    (`executed=false`, nested-function cases).
  - `OP_LdPrivateDynamic` (Reg3) / `OP_StPrivateDynamic` (Reg4): runtime
    kind dispatch on the brand-record shape (field vs method vs
    accessor-only), reusing the existing
    `JSERR_PrivateMethodNotWritable`/`JSERR_PrivateAccessorMissingGetter/Setter`
    errors.
- Destructuring: `EmitNameInvoke` is split into `EmitDestructuredObjectKey`
  (evaluate key) + `EmitDestructuredObjectGet` (Get + rest bookkeeping).
  Only dot/index targets hoist the reference (`NeedsDestructuredReferenceHoist`);
  identifier/binding targets keep the old path. The computed key location is
  released after the hoisted reference (`releaseKey=false` + deferred
  `ReleaseLoc`), because locals/temps share one strict-LIFO stack:
  key-then-base acquisition must release base-then-key.
- Three opcodes are appended last in `OpCodes.h` (`LdPrivateBrand` ElementP,
  `LdPrivateDynamic` Reg3, `StPrivateDynamic` Reg4) so no existing opcode value
  shifts. Justification: eval and caller are separately compiled, so the brand
  const and the brand kind live in different functions; a static operand cannot
  name them. All three are interpreter-only (no JIT helpers touched, per
  no-JIT lane rule) and `verify-bytecode` is green with unchanged checked-in
  bytecode. `ByteCodeWriter::ElementP` accepts `LdPrivateBrand` as a
  base-less patchable scope-slot load with no profiling conversion (brand
  identity must stay exact).

### Ownership/lifecycle record

- **Resource**: no new heap resource. Brand maps remain the existing
  `JavascriptWeakMap` per private name per class evaluation, owned by the
  hidden class-block const.
- **Owner**: the caller class-block const; eval-entry brand checks read it
  through the existing scope chain and keep no reference beyond the check.
- **Invalidation**: none needed. Compiled eval bodies are caller-agnostic
  (brand resolved per execution from the scope chain), so eval-map reuse is
  safe.
- **User-code/reentrancy**: brand-check and dynamic dispatch paths run before
  user getters/initializers and allocate only through the recycler; abrupt
  completion propagates in evaluation order (key, target, Get, store).
- **Failure**: wrong receiver/primitive receiver throws `TypeError`; unknown
  `#name` in eval is `SyntaxError`/`ReferenceError`; no partial native state.
- **Thread/context**: current-thread script execution only.

## Evidence

Build (Mac ARM64, `-nonative` interpreter evidence):

- `sh /tmp/opencode/cc-gate-lock.sh mise exec -- zig build --cache-dir "$PWD/.zig-cache"` — green.
- `... zig build -Dprofile=debug-static ...` — green.
- `... zig build verify-bytecode ...` — green; checked-in bytecode unchanged.
- `git diff --check` clean; `check_copyright.py` clean on all touched files;
  `check_tabs.sh`/`check_ascii.sh` clean.

Local suites (disable_jit):

- `zig build check` — **passed 2012 / failed 0** (matches
  `tools/build/gate_counts.json` test count 2012).
- `zig build check-regression -Dprofile=debug-static` — **passed 2064 /
  failed 0** (matches debug-static count 2064).
- `test/es7/class-private-fields.js` — 17/17; `test/es7/class-private-methods.js`
  — 34/34; `test/es7/class-static-private-fields.js` — 17/17; each in normal,
  `-forcedeferparse`, `-nodeferparse`, and `-noDeferParse` modes, Release and
  debug-static. New coverage: computed-key member-target destructuring order
  (`target` logged before `get`), direct-eval brand visibility from methods.
- Debug-static previously aborted on two new-code asserts, both fixed and
  re-verified: missing `LdPrivateBrand` case in `ByteCodeWriter::ElementP`,
  and a temp-stack LIFO violation in the hoisted destructuring path.

Integrated test262 class-elements corpus (main-checkout runner,
`--checkout /Users/ascotti/Documents/code/ChakraCore/out/test262`,
patterns `test/language/statements/class/elements/**/*.js` and
`test/language/expressions/class/elements/**/*.js`):

| run | passed | failed |
|---|---:|---:|
| wave2 baseline (`zig-out/bin/ch` ref) | 5,854 | 43 |
| S1-tail lane binary | **5,888** | **9** |

Scenario diff: **34 fail -> pass, 0 pass -> fail**. Flips: 8 computed-key
visibility, 2 field evaluation-order, 24 direct-eval visibility scenarios.

BigInt supplemental (`--host-flags='-nonative -Test262 -CollectGarbage
-ESBigInt'` on `privatefieldget/put-primitive-receiver.js`): **4/4 pass** —
the 4 primitive-receiver residuals are a BigInt-literal runner gap, not a
brand-semantics gap.

## Residual work and limitations

- `private-class-field-on-nonextensible-objects.js | strict mode` still fails:
  it asserts Stage-1 `nonextensible-applies-to-private` proposal semantics
  (private add on a nonextensible object throws). Node fails it too; ES2025
  `PrivateFieldAdd` intentionally adds to nonextensible objects.
- `privatefieldget/put-primitive-receiver.js` (4 scenarios) fail under the
  default flags only because the runner lacks `-ESBigInt` (`15n` literals);
  4/4 pass with `-ESBigInt`.
- `grammar-field-accessor.js` (4 scenarios, statements + expressions x
  default + strict) fail: post-ES2025 field-accessor grammar, out of scope.
- JIT/backend support is unverified. The three new opcodes are
  interpreter-only with no JIT helpers; **B4 must run the S1 private corpus
  and the es7 tests under SimpleJIT/FullJIT (dynapogo) before any
  JIT/default claim**.
- `RecordEvalPrivateBrand` runs under the parser's `CreateBinNode` assert
  regime; all defer modes (`-forcedeferparse`, `-nodeferparse`,
  `-noDeferParse`) are green in Release and debug-static, but parallel lanes
  touching `EmitPrivateBrand` defaults, `EmitDestructuredObjectMember` order,
  or the `VEval` defer heuristic should re-run this corpus.

## Files

- `lib/Parser/Parse.h`, `lib/Parser/Parse.cpp`: drop class-private-name
  counter; spelling-derived `PidForHiddenPrivateName`; `RecordEvalPrivateBrand`,
  `CreateDynamicPrivateName`, eval branches in `CreatePrivateNameReference` /
  `ResolvePendingPrivateRefs`.
- `lib/Parser/ptree.h`, `lib/Parser/ptree.cpp`: `ParseNodeProg::
  pnodeEvalPrivateBrands`; `ParseNodeName::isDynamicPrivateName` flag.
- `lib/Runtime/ByteCode/OpCodes.h`: appended `LdPrivateBrand` (ElementP),
  `LdPrivateDynamic` (Reg3), `StPrivateDynamic` (Reg4).
- `lib/Runtime/ByteCode/ByteCodeEmitter.cpp`: `PropertyIdForDynamicPrivateBrand`,
  `EmitPrivateBrand` dynamic branch + default skip, `EmitEvalPrivateBrandChecks`
  hook in `EmitOneFunction`, `Ld/StPrivateDynamic` dispatch in
  `EmitPrivateLoad`/`EmitPrivateStore`, destructuring split
  (`EmitDestructuredObjectKey`/`EmitDestructuredObjectGet`,
  `NeedsDestructuredReferenceHoist`, deferred key release).
- `lib/Runtime/ByteCode/ByteCodeWriter.cpp`: `LdPrivateBrand` case in
  `ElementP` (no profiling conversion).
- `lib/Runtime/Language/InterpreterHandler.inl`,
  `InterpreterStackFrame.{h,cpp}`, `JavascriptOperators.{h,cpp}`: the three
  new interpreter ops and brand-record kind dispatch.
- `lib/Runtime/Library/GlobalObject.cpp`: `mayContainPrivateRef` VEval
  no-defer heuristic.
- `test/es7/class-private-fields.js` (+3), `test/es7/class-private-methods.js`
  (+1): focused regression tests in existing registered files (no
  `gate_counts.json` change).
