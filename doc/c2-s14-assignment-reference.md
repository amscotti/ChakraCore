# C2-S14 P5: assignment/PutValue reference base

Status: **pending**. Branch `s14-p5`, base `fc9dceb93`. Interpreter/Mac
no-JIT evidence only; JIT evidence is not claimed (B4).

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
  Destructuring and `for-in`/`for-of` head stores keep their existing paths.

`EmitAssignment` uses the captured base when the register is not `Undecl`:

- Store through the captured object with the existing `StFld`/`StFldStrict`
  opcode and inline cache. This preserves getters/setters, writable checks and
  const/TDZ errors from `SetProperty`.
- In strict mode, if the captured object environment record no longer has the
  property, emit `RuntimeReferenceError` at store time. This matches
  `SetMutableBinding` with `S = true`; non-strict assignment recreates the
  property on the captured object.
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
caller keeps its current behavior.

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
| after (`out/test262-c2-s14-p5-final-exact`) | **50** | **0** | 50 |

Exact flips: **50 fail→pass, 0 pass→fail**.

Broad assignment/compound-assignment directories
(`test/language/expressions/assignment/**` and
`test/language/expressions/compound-assignment/**`):

| Run | Passed | Failed | Scenarios |
|---|---:|---:|---:|
| before (`out/test262-c2-s14-p5-assign-before`) | 1,395 | 241 | 1,636 |
| after (`out/test262-c2-s14-p5-final-assign`) | **1,445** | **191** | 1,636 |

Exact flips: **50 fail→pass, 0 pass→fail**.

Wider affected run (`test/language/expressions/**`,
`test/language/statements/with/**`, `test/language/eval-code/**`; 21,922
scenarios): before `out/test262-c2-s14-p5-regress-before`
16,540/5,382; after `out/test262-c2-s14-p5-regress-final` **16,601/5,321**.
Ignoring the parallel-run dynamic-import flake described below, the observed
fail→pass set is 64: the 50 P5 scenarios, 12 increment/decrement scenarios, and
2 strict `with` deletion scenarios. `test/language/statements/with/**` alone
moved 172/10 → **173/9** (2 fail→pass, 1 pass→fail outside P5, below).

Focused `test/es7/assignment-reference-base.js` (registered in
`test/es7/rlexe.xml` with `-args summary -endargs`) passes. It covers simple
and compound assignment through `with`, nested `with`, eval-introduced vars,
eval-local `with`, getters/setters, postfix/prefix increment/decrement,
reference-before-RHS ordering, strict deleted-binding errors, and eval
const/TDZ errors.

Full legacy regression: `python3 test/runtests.py -b "$PWD/zig-out/bin/ch" -t
--variants disable_jit` → **1,996 passed / 0 failed**. This includes the new
es7 test and the updated `test/es6/unscopablesWithScopeTest.js` expectations;
the legacy test previously encoded the old re-resolution behavior for
`with` + `Symbol.unscopables`.

## Residual

- `test/language/statements/with/set-mutable-binding-binding-deleted-with-typed-array-in-proto-chain.js`
  is a pass→fail. The test deletes a `NaN` property from an object whose
  prototype is a typed array. P5 now correctly targets that captured binding
  object, but Chakra's `SetProperty` creates the canonical-numeric property
  instead of honouring the typed-array `[[Set]]` no-op; fixing that requires a
  separate typed-array exotic-property change. The strict counterpart passes.
- Assignments written *inside* a direct eval string but resolving entirely to
  the caller environment still use the existing scoped store when there is no
  eval-local `with`. This preserves global lexical (`let`/`const`), scoped
  `with` in eval, and strict unresolvable errors, but a nested direct eval in
  the RHS that introduces a caller binding can still be re-resolved. The P5
  A6 scenarios assign from caller code (where the reference is captured), not
  from inside eval code.
- Destructuring assignment targets and `for-in`/`for-of` head assignment retain
  their previous emission paths; they are outside the P5 selection.
- The 40 dynamic-import flips in the 21,922-scenario parallel run are harness
  flakiness: the same files fail with `Unexpected identifier after numeric
  literal` or `Test did not run to completion` on either binary and pass with
  `--jobs 1` (for example
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
