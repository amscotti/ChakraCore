# C2-S14 P4: global/eval declaration-instantiation collisions

Status: **`<pending>`** (implementation and focused evidence on `s14-p4`;
Mac interpreter/no-JIT only, JIT gated at B4). Base `ff170d34b`.

## Scope and spec operations

This slice implements the binding checks and descriptor updates of
`GlobalDeclarationInstantiation`, `EvalDeclarationInstantiation`, and the
Annex B global/eval var extension at their runtime boundaries:

- Global lexical names (`lexNames`) reject an existing global var/function
  binding (`HasVarDeclaration`), an existing global lexical binding
  (`HasLexicalDeclaration`), and a non-configurable own property
  (`HasRestrictedGlobalProperty`) with `SyntaxError`.
- Global var/function names reject an existing global lexical binding with
  `SyntaxError`.
- `CanDeclareGlobalFunction` rejects a non-extensible global without the
  property and an existing non-configurable property unless it is a
  writable+enumerable data property, with `TypeError`.
  `CanDeclareGlobalVar`'s non-extensible `TypeError` remains on the existing
  property-initialization path.
- All of those checks run before `DefineUserVars`/`DefineFunctions`, so a
  rejected script does not execute its body or create sibling var bindings.
- Sloppy direct/indirect eval rejects a var/function declaration whose name is
  bound by any lexical environment between the eval and the var environment
  with `SyntaxError`; a parameter-default direct eval does so during
  `FunctionDeclarationInstantiation`, before the body.
- Global eval function bindings use `CreateGlobalFunctionBinding(..., true)`:
  absent/configurable properties receive writable+enumerable+configurable
  defaults; a non-configurable existing property is updated by value only.
- The Annex B global/eval block-function synthetic binding uses
  `CreateGlobalVarBinding`: an existing property is not reinitialized and keeps
  all attributes; the subsequent block-function assignment updates only value.
- Global lexical bindings are independent of global-object extensibility.

## Engine design

`lib/Runtime/Library/RootObjectBase.cpp`:

- `EnsureCanDeclGloFunc` now implements `CanDeclareGlobalFunction`: it also
  rejects an existing global lexical binding (`HasRootProperty` `noRedecl`) with
  `SyntaxError`, treats an absent property as declarable only when the global is
  extensible, and inspects the real own-property descriptor instead of the old
  "not configurable AND not writable AND not enumerable" approximation.
- `EnsureNoProperty` (global lexical preflight) throws `SyntaxError` when the
  existing property is a lexical binding or non-configurable. A configurable
  property, including a var introduced by non-strict eval, is shadowable.
- `EnsureNoRedeclProperty` (global var/function preflight) throws `SyntaxError`
  for a lexical collision instead of `ReferenceError`.

`lib/Runtime/Types/ActivationObject.cpp`:

- `ActivationObject`'s lexical-redeclaration helper now throws `SyntaxError`.
- `BlockActivationObject::EnsureNoRedeclProperty` checks its own lexical
  bindings and throws `SyntaxError`; it still returns false so a scoped store is
  not attempted in the block. This closes direct-eval lower-lexical collisions
  (`{ let x; eval('var x;') }`) without letting eval create vars on block scope.

`lib/Runtime/Library/GlobalObject.cpp`:

- `InitPropertyInEval` and `InitFuncScoped` obtain the existing descriptor. An
  existing writable data property is updated by value with its current
  attributes, which is the Annex B synthetic-var/CreateGlobalVarBinding case.
  An absent or non-writable/configurable property follows
  `CreateGlobalFunctionBinding` (writable+enumerable+configurable defaults, or
  value-only/`TypeError` for a non-configurable descriptor).
- A global lexical eval binding (`noRedecl`) makes `InitFuncScoped` throw
  `SyntaxError`; the eval preflight normally reports it before the body.

`lib/Runtime/Language/JavascriptOperators.cpp`:

- `OP_InitLetProperty`/`OP_InitConstProperty` add
  `PropertyOperation_Force` for `RootObjectBase`, and the root TDZ initializers
  do the same. Global lexical bindings are not global-object properties in the
  spec, so `Object.preventExtensions(global)` must not prevent their creation.

`lib/Runtime/ByteCode/ByteCodeEmitter.cpp`:

- `EnsureCanDeclGloFunc` is emitted after the lexical passes for script and
  eval function declarations. For eval code, `OP_EnsureCanDeclGloFunc` walks the
  eval environment and applies `CanDeclareGlobalFunction` only when its var
  environment is global, so direct and indirect global eval check while a
  caller-function eval does not.
- A separate preflight walks synthetic Annex B var nodes and checks only
  `HasLexicalDeclaration` on the eval's global var environment. It rejects a
  global lexical collision before the eval body; a local lexical environment
  stops the walk and the synthetic store is skipped by `InitFuncScoped`.
- The Annex B synthetic global var store uses `InitFuncScoped` so local lexical
  bindings are never written; existing global properties keep their attributes.

### Error order

For global scripts, `EnsureNoRedeclarations` is emitted before
`DefineUserVars` and `DefineFunctions`. Its lexical, function and var
preflights therefore throw before the script can create any binding or execute
its body. For eval, the same op sequence runs when the eval function body is
entered, before `DefineUserVars`/`DefineFunctions` and before the evaluated
statements; the synthetic Annex B lexical preflight is part of that sequence.
All var-name lexical checks precede the `CanDeclareGlobalFunction` pass, so a
later lexical collision is not masked by an earlier non-definable function.
For a direct eval in a parameter initializer, the parameter-scope activation
object is in the eval's environment chain, so the rewrite fails before the
function body runs. A failed descriptor define throws from the
existing `DefineOwnPropertyDescriptor`/property-operation path, whose caller
unwinds the eval/script without retaining partial C++ state.

### Test-host correction

`tools/test262/harness.js` now overrides the eshost Chakra host's
`$262.evalScript` to propagate `WScript.LoadScript` failures instead of
returning a completion object. The declaration-instantiation tests assert those
failures with `assert.throws`. Only 31 corpus files call `$262.evalScript`; all
are global-code/eval-code/annexB declaration tests. Four recorded baseline
failures (`script-decl-func-err-non-extensible.js` and `script-decl-var-err.js`,
2 scenarios each) were already engine-correct and pass once the host propagates
the errors.

## Evidence

Pinned corpus `2e0a56762801e275a9fdf96dc49d90ba0cddcf63`, flags
`-nonative -Test262 -CollectGarbage`, checkout `out/test262`, all
`zig build`/test262 commands under `/tmp/opencode/cc-gate-lock.sh` with local
`out/` caches. Final binary `zig-out/bin/ch` (sha256
`3a57922e9ae7cfbf10477e17216507034fdca33e16312533bf95db1d6371c35e`) and
`libChakraCore.dylib` (sha256
`90d481b60d36ad095718998e8f2f63657f67f134af3cbac70e0e00c4afeea647`). The
"before" dylib is
`d8e0eb0fb6ab0773c83d800f9cf39262f52951f1f656f8b94ae6d90470a266e9`.

P4 selection is the 150 scenarios named by the triage: 75 in
`test/language/global-code/**/*.js`, 59 in
`test/language/eval-code/{direct,indirect}/var-env-*.js`, and 16
`**/eval-var-scope-syntax-err.js` (including the seven
`expressions/object/method-definition` files not matched by a single `**`).

| Selection | Recorded baseline | Corrected-host baseline | After | Fail→pass | Pass→fail |
|---|---:|---:|---:|---:|---:|
| global-code | 56/19 | 60/15 | **75/0** | 19 | 0 |
| eval var-env | 52/7 | 52/7 | **59/0** | 7 | 0 |
| eval-var-scope | 0/16 | 0/16 | **16/0** | 16 | 0 |
| **total** | **108/42** | **112/38** | **150/0** | **42** | **0** |

The recorded-baseline comparison is **42 fail→pass / 0 pass→fail**. Under the
corrected host, **38 fail→pass / 0 pass→fail** and all **112** previously
passing controls remain green; the other four recorded failures are the
host-wrapper cases above. Artifacts:
`out/test262-p4-global-throw-before`, `out/test262-p4-var-env-before`,
`out/test262-p4-scope-throw-before`, `out/test262-p4-r17-{global,var-env,scope}`.
The split also satisfies the triage's 19+23 batch shape: global-code 19 flips
(15 engine + 4 host), eval-code 23 flips (7 var-env + 16 parameter-default).

Controls and related selections:

| Corpus | Before | After |
|---|---:|---:|
| P3 skip selection (280) | 280/0 | **280/0** |
| full Annex B three directories (782) | P3 doc: 765/17 | **782/0** |
| direct/indirect non-definable eval selection (15) | not part of P4 baseline | **15/0** (`out/test262-p4-r17-nondef`) |
| `test/language/block-scope/**/*.js` (287) | 287/0 | **287/0** |
| P1 completion selection (156) | 152/4 known class/using | **152/4** |
| `test/es7` legacy suite | 57/0 existing (57/1 with new test) | **58/0** |
| full legacy `runtests.py -t --variants=disable_jit` | 1992/1 (new P4 test only) | **1993/0** |

P3's 17 documented residual `existing-global-*` collisions now pass; the full
Annex B directories are 782/0. P1's four failures remain the two
`statements/class/cptn-decl.js` and two `statements/using/cptn-value.js`
scenarios excluded by that batch.

Focused test: `test/es7/global-eval-declarations.js`, registered in
`test/es7/rlexe.xml` with `-args summary -endargs`. It covers global
lexical/var/function collisions, restricted and configurable properties,
non-extensible global let/const/class and function/var failures with body
non-execution, descriptor-preserving global/eval function updates, direct and
indirect lower/global lexical eval collisions, Annex B global/local let/const
side-effect ordering with own-property checks, direct-eval
`CanDeclareGlobalFunction` and lexical-before-nondefinable ordering,
parameter-default direct eval (function, arrow and method), and non-colliding
controls. `test/es7` is **58/0**, including the existing
`blockscope-early-errors.js` and `annexb-block-function-var.js`.

Legacy expectation updates are limited to formerly non-conforming behavior:
`ReferenceError`→`SyntaxError` for eval var/lexical redeclarations
(`test/LetConst/eval1.baseline`, `test/es6/letconst_eval_redecl.baseline`,
`test/es6/letconst_global_shadow_builtins_nonconfigurable.baseline`,
`test/es6/default.js`, `test/es6/default-splitscope.js`, `test/es6/rest.js`),
the known issue-5070 case in `test/LetConst/funcDeclConflict.js`, the
`test/es6/blockscope-functionbinding.js` section titles and local collision
shape, `test/LetConst/eval_fncdecl.js` restored to its original f/g/i/j names
with explicit SyntaxError assertions and updated baselines, and
`test/GlobalFunctions/CanDeclareGlobalFunction.js` using separately loaded
global scripts rather than local eval for the global `CanDeclareGlobalFunction`
cases. `test/Bugs/vso_os_1091425*.js` now accepts the engine's current
`TypeError` message for a non-configurable accessor. No unrelated legacy
baseline changed.

## Ownership record and review receipt

```text
Resource / creation: no new heap, recycler, arena, GC or native allocation.
  PropertyDescriptor/PropertyAttributes values are stack locals in
  RootObjectBase.cpp, GlobalObject.cpp and JavascriptOperators.cpp.
Domain/allocator: unchanged ScriptContext/Recycler and global-object type
  handlers; no new owner, root, slot, scope or retained pointer.
Physical base/extent/alignment/release: not applicable; no new storage.
Logical length/views: not applicable.
Owner/transfer: existing global object/type handler; descriptor values are
  consumed synchronously by DefineOwnPropertyDescriptor/SetPropertyWithAttributes.
Borrow invalidation: no descriptor pointer is retained across a call. Property
  define/transition may replace the type handler; callers reacquire through the
  existing APIs and do not hold raw descriptor pointers.
Reentrancy/user code: global object is not a Proxy. The checks inspect own
  descriptors and attributes and do not invoke getters/setters. The annotation
  operation can raise existing engine errors; those unwind through the current
  script/eval frame.
Thread/context: JS thread and ScriptContext of the executing script/eval; no
  cross-thread or background state added.
Release on success/failure/OOM: existing property/type-handler ownership and
  exception paths. The slice adds no partial native allocation to release.
Accounting/bound: O(1) per declared name; no new persistent memory.
Tests: P4 150/0 (42 recorded-baseline flips, 38 under corrected host, 0
  pass->fail); P3 280/0 and Annex B 782/0; block-scope 287/0; P1 152/4;
  es7 58/0; legacy 1993/0.
Remaining uncertainty: no JIT, GCStress, sanitizer, OOM-injection or
  concurrency run.
```

Review checked the global lexical/var/function check order, restricted vs
configurable eval-introduced properties, non-extensible function/var and
lexical declarations, direct/indirect eval lower/global lexical walks,
parameter-default eval, non-configurable data/accessor function descriptors, and
the Annex B synthetic binding path. The test-host correction was reviewed
against its 31-file blast radius.

## Residual and B4

- Mac test-profile/interpreter (`-nonative`, no JIT) evidence only. JIT codegen,
  bailout, GCStress, Linux and memory-accounting claims remain B4-owned.
- The `$262.evalScript` correction is test tooling, not engine behavior. It
  changes only the 31 corpus files that call the host helper; no production
  harness or gate pin is moved here.
- Global var non-extensible rejection still occurs during the existing
  `LdElemUndef` pre-initialization path rather than a separate
  `CanDeclareGlobalVar` opcode. The selected tests pass and the first new var
  throws before body execution; a multi-var script with an earlier already
  existing binding was checked to create no new property before throwing.
- `InitFuncScoped`/`InitPropertyInEval` preserve attributes for an existing
  writable data property, which Annex B requires. An ordinary eval function
  binding over a writable+configurable non-enumerable property therefore keeps
  its enumerable flag instead of resetting to the function-binding default; no
  test262 scenario in this selection exercises that combination.
- The interpreter routes eval declaration preflights through the eval
  environment; the JIT lowering still calls the root-only helper. JIT behavior
  and configuration are B4-owned and are not claimed here.
- No full `zig build check` or global gate-pin update was run; integration owns
  cumulative counts.
