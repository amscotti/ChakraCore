# C2-S14 P5: assignment/PutValue reference base

Status: **landed in `8b5f28c06`** (feature `fdd5aff0`; base `fc9dceb93`).
Current integrated gates **1996/2048**; P5 selection **50/50**, **50 fail→pass /
0 pass→fail**; broad assignment/compound **1,445/191** with 50/0, and 65/0
non-import wider flips. Interpreter/Mac no-JIT only; B4 owns JIT claims.
Residual: eval/function-expression reference paths and pre-existing proxy
descriptor duplication.

## Scope and spec rule

The target is the P5 cluster in `doc/c2-s14-triage.md`:

- `test/language/expressions/assignment/S11.13.1_A[56]*.js`
- `test/language/expressions/compound-assignment/S11.13.2_A[56]*.js`

An assignment evaluates its left-hand side to a Reference before it evaluates
the right-hand side. `PutValue` then uses that Reference's base and binding.
The engine previously re-resolved the name at store time, so the RHS could
change which environment record it selected:

```js
var x = 0, scope = { get x() { delete this.x; return 2; } };
with (scope) { x *= 3; }          // scope.x = 6, outer x = 0

var x = 3;
(function () { x *= (eval("var x = 2;"), 4); return x; })(); // inner x = 2
```

The first case stored through the deleted `with` binding to the outer `x`; the
second stored to the eval-introduced inner `x` instead of the captured outer
binding. The cluster is positive-only in the selection: 44 compound-assignment
and 6 assignment scenarios.

## Design

`lib/Runtime/ByteCode/ByteCodeEmitter.cpp` now has
`EmitAssignmentReferenceBase`. It runs at reference-evaluation time (before the
RHS) and returns a pre-acquired temporary register containing the binding
object, or the `Undecl` sentinel when the reference has only a static binding:

- Ordinary names walk the same dynamic scopes as `EmitPropLoad`:
  `BrOnHasLocalProperty`, `BrOnHasLocalEnvProperty`, and the with variants.
  On a hit the label materializes the owning object (`LdLocalObj`,
  `LdParamObj`, `LdEnvObj`, and `UnwrapWithObj` for object environments).
  The sentinel is `InitUndecl`, not JS null, because `BrNotNull_A` compares
  against C++ `NULL`.
- Unknown-scope names in direct eval also check eval-local dynamic scopes (a
  `with` statement inside the eval string) before falling back to the existing
  scoped store for the caller chain. This keeps
  `eval("with (o) { p = ... }")` working and avoids breaking global lexical
  (`let`/`const`) bindings.
- The register is threaded through `EmitBinaryReference`, compound assignment,
  logical assignment, prefix/postfix increment/decrement, and `EmitAssignment`.
  For compound/logical/inc-dec, `EmitLoad` branches on the captured register.
  When it is not `Undecl`, GetValue first probes `HasProperty` on the captured
  binding object (Object Environment Record GetBindingValue), then loads with
  a direct `LdFld`; a missing binding is `undefined` in sloppy mode and throws
  `ReferenceError` before the RHS in strict mode. `ChkUndecl` handles a
  captured declarative TDZ binding. When the register is `Undecl`, a new
  `EmitPropLoad(..., skipDynamicScopes)` path writes directly to the static
  owner/root, so a stateful `@@unscopables` or proxy `has` cannot redirect the
  read. Eval fallback deliberately retains the old scoped load because its
  caller-chain/global-lexical semantics are different.
  Plain `with` reads now use the same GetBindingValue `HasProperty` probe in
  `EmitPropLoad`, including the dynamic call-target path, so test262
  `get-binding-value-*` observe `has` before `get`.
  Destructuring and `for-in`/`for-of` head stores keep their existing paths.

`EmitAssignment` uses the captured base when the register is not `Undecl`:

- Store through the captured object with the existing `StFld`/`StFldStrict`
  opcode and inline cache. This preserves getters/setters, writable checks and
  const/TDZ errors from `SetProperty`.
- `SetMutableBinding` probes `HasProperty` whether or not `S` is true. The
  captured store therefore emits `BrOnHasProperty` unconditionally. A missing
  binding only becomes `RuntimeReferenceError` in strict mode; sloppy
  assignment falls through to the set and recreates it on the captured object.
- A captured base is a plain object. `JavascriptOperators::SetAccessorOrNonWritableProperty`
  now treats a canonical numeric non-index property on a typed-array prototype
  as the typed-array `[[Set]]` no-op instead of creating an own property on the
  receiver. This is needed for the captured `with` binding shape; valid numeric
  indices and ordinary prototype properties keep the normal path.
- If no dynamic base matched, the static fallback calls `EmitPropStore` with
  the new `skipDynamicScopes` flag so it writes directly to the symbol's owner
  or the root instead of re-running the dynamic checks. For eval code with no
  eval-local dynamic base, the existing scoped store is retained so global
  lexical bindings and caller-chain semantics are unchanged.

### Ownership and invalidation

The captured base is a normal emitter temporary register
(`AcquireTmpRegister`/`ReleaseTmpRegister`). It holds a GC-visible environment
object only for the duration of the assignment expression, exactly like the
existing dynamic load/store temporaries. No runtime object, parse node, or
serialized bytecode state is added; no allocation or GC barrier is introduced.
`EmitPropStore`'s new parameter defaults to `false`, so every pre-existing
caller keeps its current behavior. Every captured-assignment path releases the
base temp only after `ReleaseLoc(rhs)`, `ReleaseReference(lhs)`,
`ReleaseLoad(pnode)`, and any result/RHS temp acquired after it; the debug
build asserts this LIFO order (`FuncInfo::ReleaseTmpRegister`).

### Error order

Reference evaluation and its `HasBinding` checks run before the RHS. The RHS
runs completely before `PutValue`. A strict deleted object-environment binding
throws `ReferenceError` at store time, not before the RHS. Non-writable and
const properties still throw from `SetProperty`; TDZ (`UndeclBlockVar`) still
throws `ReferenceError` from the type handler. Getter/setter order is unchanged:
the getter is read for compound/inc/dec before the RHS, and the setter is
invoked by the final store.

## Evidence

Pinned test262 `2e0a5676…`, `-nonative -Test262 -CollectGarbage`; test-profile
binary built with `mise exec -- zig build --cache-dir "$PWD/.zig-cache"`.
Before/after runs use binaries built from the same branch with and without the
source changes (stash/rebuild), not the recorded C1 baseline.

Exact P5 selection:

| Run | Passed | Failed | Scenarios |
|---|---:|---:|---:|
| before (`out/test262-c2-s14-p5-before`) | 0 | 50 | 50 |
| after (`out/test262-c2-s14-p5-r7-exact`) | **50** | **0** | 50 |

Exact flips: **50 fail→pass, 0 pass→fail**.

Broad assignment/compound-assignment directories
(`test/language/expressions/assignment/**` and
`test/language/expressions/compound-assignment/**`):

| Run | Passed | Failed | Scenarios |
|---|---:|---:|---:|
| before (`out/test262-c2-s14-p5-assign-before`) | 1,395 | 241 | 1,636 |
| after (`out/test262-c2-s14-p5-r7-assign`) | **1,445** | **191** | 1,636 |

Exact flips: **50 fail→pass, 0 pass→fail**.

Wider affected run (`test/language/expressions/**`,
`test/language/statements/with/**`, `test/language/eval-code/**`; 21,922
scenarios): before `out/test262-c2-s14-p5-regress-before`
16,540/5,382; after `out/test262-c2-s14-p5-regress-r6` **16,623/5,299**.
Excluding the parallel-run dynamic-import flake described below, the observed
flips are **68 fail→pass, 0 pass→fail**: the 50 P5 scenarios, 12
increment/decrement scenarios, 2 strict `with` deletion scenarios,
`unscopables-inc-dec.js` (fixed by reading through the captured base), and 3
GetBindingValue proxy/with scenarios. The reviewed `d95e63597` run had
**64 fail→pass / 1 pass→fail** here; the one pass→fail was the typed-array
shape now fixed. `test/language/statements/with/**` alone moved 172/10 →
**178/4** (6 fail→pass, 0 pass→fail).

The dynamic-import slice differs between the two parallel runs: the reviewed
`d95e63597` run had an import diff of 78 scenarios (38 fail→pass, 40
pass→fail); the final run had 63 (39+24). Those scenarios flip
nondeterministically under the parallel harness on both binaries and are not
attributed to this change (single-file `--jobs 1` runs pass 2/2).

Typed-array focused checks: the two
`set-mutable-binding-binding-deleted-with-typed-array-in-proto-chain*.js`
scenarios are **2/2** on the test and debug-static binaries, and the
`TypedArray`/`TypedArrayConstructors` built-in selection is unchanged at
2,175 pass / 2,161 fail with **0 flips**.

Focused `test/es7/assignment-reference-base.js` (registered in
`test/es7/rlexe.xml` with `-args summary -endargs`) passes **21/21**. It covers
simple, compound and logical assignment through `with`, stateful/reverse
`@@unscopables`, proxy `has` side effects and GetBindingValue probes, variable
RHS/result temporaries,
nested `with`, eval-introduced vars, eval-local `with`, getters/setters,
postfix/prefix increment/decrement, reference-before-RHS ordering, strict
deleted-binding errors, eval const/TDZ errors, the function-expression-name
shadowing path, and the typed-array canonical numeric non-index no-op.

Full legacy regression: `python3 test/runtests.py -b "$PWD/zig-out/bin/ch" -t
--variants disable_jit` → **1,996 passed / 0 failed**. Debug-static
(`runtests.py -b "$PWD/out/debug-static/bin/ch" --debug --variants disable_jit`)
→ **2,048 passed / 0 failed**, which exercises the `ReleaseTmpRegister` LIFO
assertions. This includes the new es7 test and the updated
`test/es6/unscopablesWithScopeTest.js` expectations; the legacy test
previously encoded the old re-resolution behavior for `with` +
`Symbol.unscopables`.

## Residual

- Eval fallback still broadens the residual. When an assignment is written
  *inside* a direct eval string and its reference resolves outside all
  eval-local dynamic bases, the existing scoped store is used at store time.
  Consequently an eval-local `with` that has no binding at reference time can
  be selected after the RHS creates it: `var s = {}; var x = 0;
  eval("with (s) { x = (s.x = 2, 1); }")` yields `x === 0, s.x === 1`
  instead of the captured caller base. The same fallback applies to caller
  environment references in eval code, so a nested eval in the RHS can still
  re-resolve. Fixing this safely needs a captured store that also preserves
  global lexical (`let`/`const`) and root semantics; left as residual.
- `EmitAssignmentReferenceBase` deliberately returns no capture for
  `sym->GetIsFuncExpr()`. A `with` binding present at reference time shadows a
  function-expression name correctly through the existing dynamic store
  (covered by the focused test), but if the `with` binding is absent at
  reference time and the RHS creates it, the store can still select that new
  property. This matches current Node behaviour and has no test262 cover.
- The two `set-mutable-binding-idref-*-with-proxy-env.js` scenarios still fail
  on a pre-existing extra proxy `getOwnPropertyDescriptor` call in the proxy
  set path. Their `has`/`get`/`set` prefix now matches the expected sequence
  (`has, get:unscopables, has, get, has, set` for the compound case), but the
  extra descriptor call is independent of P5 and was present before this batch.
  The corresponding `get-binding-value-*` scenarios are 2/2.
- Typed-array invalid integer indices remain on the pre-existing path: the P5
  change only recognizes canonical numeric non-index properties on a typed-array
  prototype. `TypedArray`/`TypedArrayConstructors` has 0 flips against before.
- Destructuring assignment targets and `for-in`/`for-of` head assignment retain
  their previous emission paths; they are outside the P5 selection.
- The dynamic-import diff in the 21,922-scenario parallel run (78 in the
  reviewed run, 59 in the final run) is harness flakiness: the same files fail
  with `Unexpected identifier after numeric literal` or `Test did not run to
  completion` on either binary and pass with `--jobs 1` (for example
  `dynamic-import/catch/nested-async-function-file-does-not-exist.js` is 2/2
  on both before and after with `--jobs 1`). They are not attributed to P5.
- The local `unscopablesWithScopeTest` expectation change is deliberate: the
  spec/P5 result is `o.a = 3`, outer `a = 2` for the compound case, and
  `obj.a = true`, outer `a = false` for the precedence case.

## B4 note

Mac interpreter (`-nonative`, no JIT) evidence only. No new opcodes are added;
the change composes existing bytecodes (`LdLocalObj`, `LdParamObj`, `LdEnvObj`,
`UnwrapWithObj`, `InitUndecl`, `BrNotUndecl_A`, `BrOnHasProperty`, `StFld`).
JIT lowering, bailout, profiling, GC stress and memory accounting are not
claimed here; B4 remains gated.
