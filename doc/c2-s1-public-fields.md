# C2-S1: public instance class fields (simple identifier names)

Status: `<pending>` — implementation and focused Mac no-JIT evidence are in the
working tree; integration has not yet recorded a release/commit decision or
updated the shared C1/C2 status documents.

This is the first bounded S1 slice: **non-static public instance fields with
simple (non-computed) names and optional initializers**, in class declarations
and class expressions. Private fields/methods/brands, static fields, computed
field names, static blocks and decorators are deliberately excluded.

## Scope and observable semantics

- `class C { x; y = 1; m() {} }` parses and constructs normally.
- Every field defines an **own data property** on the instance with
  `writable: true`, `enumerable: true`, `configurable: true`, in textual
  declaration order. Fields without an initializer define `undefined`;
  duplicate public names are allowed and the last definition wins while every
  initializer still executes in order.
- Initializer expressions evaluate per construction with `this` bound to the
  new instance. Closures and arrow functions created by an initializer keep
  that instance as their lexical `this`.
- Base class: fields initialize after the instance object is created and
  **before parameter default initializers and the constructor body**. For
  simple parameter lists the call is emitted after `DefineUserVars` so captured
  or eval-visible `this` (and hoisted `var` bindings) are already published.
  For non-simple parameter lists the special-symbol storage precedes defaults
  and fields are emitted immediately before `EmitDefaultArgs`.
- Derived class: fields initialize **immediately after `super()` returns and
  binds `this`**, before the rest of the expression/constructor body resumes.
  A derived class without an explicit constructor inherits the same behavior
  from the generated default constructor.
- Field definition uses `CreateDataPropertyOrThrow` semantics through the new
  `InitClassField` bytecode op: inherited setters are ignored, inherited
  read-only properties do not block the own field, and failed defines on
  frozen/non-extensible/non-configurable objects or a proxy `defineProperty`
  trap returning false throw `TypeError`.
- `constructor` is rejected as a field name (including its string spelling).
  `arguments` is rejected by the class-field early error; shorthand and
  destructuring forms are covered, and direct eval code containing an
  `arguments` identifier reference throws `SyntaxError` at eval execution time.
- Anonymous initializer functions/classes receive the field name through
  NamedEvaluation (`x = function(){}`, `y = () => {}`, `z = class {}`).

## Parser/AST design

`ParseNodeFnc` gains `pnodeClassFields`, a textual-order list of `knopMember`
records: `pnode1` is the field name (`knopStr`) and `pnode2` is either a call
to a synthesized initializer helper or null for a field without an initializer.

For each initializer the parser synthesizes a hidden block-scoped **function
declaration** in the class body (`*s1fieldN*`, not source-writable). The helper
has a real parameter/body scope and strict `this`; its body is
`return <initializer>;`. Parsing the expression inside the helper parents
nested functions/arrows to the initializer rather than to the class's enclosing
function. The helper symbol is marked captured so the class block allocates it
a slot the constructor can load through the closure. A helper also gets the
normal built-in `arguments` binding for direct eval; the ContainsArguments
early error still rejects direct `arguments` in the initializer. No
initializer means no helper; the constructor emission defines `undefined`
directly.

The field scanner looks one token past a simple member name and restores the
scanner when the member is a method/accessor. It recognizes `=`, `;`, `}` and
ASI line breaks. `get`/`set` remain accessor prefixes when followed by a
property name, but `get\n*gen` is a field named `get` followed by a generator
method. `async x = 1`, `async x;`, `async get;` and `async static=1` are syntax
errors; `async = 1` and `async\nam(){}` are valid field spellings. Static and
computed definitions are rejected explicitly in this slice. Literal string and
numeric field names are accepted (and tested) because the property-id path is
the same; computed expressions are not.

Deferred accounting: helper declarations are synthetic nested functions, so
class members and functions inside field initializers are kept eager, and
functions whose body contains `class` are not deferred. A fast-scan stub
reserves each initialized field helper's nested slot. Constructors are eager.
Arrows in class constructors are also eager so `super()` validation sees the
complete constructor AST. These are correctness trade-offs for S1, not a broad
defer-parse policy change.

`super()` inside an arrow lexically in a constructor is **rejected with a
SyntaxError** when the class has instance fields. This is deliberately stricter
than the spec until the arrow-super field path can be initialized in the
arrow's frame; it prevents the previous silent skipped initialization.

## Bytecode/runtime design

- Property ids are explicitly assigned in the constructor visit pass (and again
  defensively before emission); field-name nodes are visited so their string
  constants exist for NamedEvaluation. This fixes the former empty-name bug
  where `class C { uniqueName = 42 }` defined `""` unless another reference
  interned the name.
- `InitClassField` is an appended extended `ElementCP` opcode. Its runtime
  helper builds a `{ value, writable, enumerable, configurable }` descriptor and
  calls `JavascriptObject::DefineOwnPropertyHelper(..., throwOnError=true)`,
  i.e. `CreateDataPropertyOrThrow`. No static `PropertyIds` were added.
- Field-call ASTs are registered during the constructor's visit pass
  (`VisitNestedScopes`) and emitted by
  `ByteCodeGenerator::EmitClassFieldInitializerCalls`.
- Base constructors emit from `EmitOneFunction` as described above; derived
  constructors emit from `EmitSuperCall` after `super()` binds and stores
  `this`.
- The helper call uses the existing `pnodeThisOverride` call shape, passing the
  instance as the explicit receiver.

### Ownership/lifecycle record

- **Resource**: per-field helper `ParseNodeFnc` / `FunctionBody` and its class
  block scope slot.
- **Owner**: class body's lexical block scope; created during class definition
  evaluation, held by the constructor closure for the class lifetime.
- **Invalidation**: none after publication; GC traces the scope slot and
  function object through existing closure machinery. Helpers are normal
  engine functions, not native allocations.
- **User-code/reentrancy**: initializers can run arbitrary JS, allocate, throw,
  call proxies/setters/eval and reenter construction. The instance is an
  ordinary rooted `this`; no retained native pointer is introduced. Abrupt
  completion propagates before later fields are defined.
- **Failure**: no partial native resources; earlier field definitions remain on
  the abandoned instance and a failed define throws.
- **Thread/context**: normal script execution on the current `ThreadContext`; no
  new async job, callback, TLS or cross-thread access.

## Evidence

Build (Mac ARM64, Test and Debug/static profiles, `-nonative`):

- `sh /tmp/opencode/cc-gate-lock.sh mise exec -- zig build --cache-dir "$PWD/.zig-cache"` — green (95/95 PAL-config checks).
- `... zig build -Dprofile=debug-static ...` — green (crash/assert runs below used this binary).
- `... zig build verify-bytecode ...` — green; source bytecode headers unchanged.
- `python3 test/runtests.py -b "$PWD/zig-out/bin/ch" -t --variants disable_jit -- test/es7` — **49 passed / 0 failed** (includes the two `class-fields-public.js` entries and updated `asyncawait-syntax.js`).
- `... -- test/es6` — **233 / 0**.
- `... -- test/es6module` — **31 / 0**.
- `... -- test/es5` — **56 / 0**.
- Debug binary: `class-fields-public.js` normal and `-force:deferparse` both **21/21**, including eval/new Function, capture/eval, define-failure, arguments and crash regressions.
- `es7/deferparseclass.js` under `-force:deferparse` — **5 / 0**.

Focused test262 subset (same pinned corpus, `-nonative`, scenarios):
`test/language/statements/class/elements/**/*.js` +
`test/language/expressions/class/elements/**/*.js`
(2,962 files / 5,897 scenarios; controls 18/18).

| run | passed | failed |
|---|---:|---:|
| base binary `10d44c972` before S1 | 1,450 | 4,447 |
| S1 fixed binary | 1,987 | 3,910 |

Scenario diff: **537 fail→pass, 0 pass→fail**. The prior draft commit's 298
fail→pass was incomplete: 204 simple public-field tests still failed on
uninterned/empty field names, and constructor/eval/deferred cases silently
skipped initialization.

Remaining 40 public-only, non-computed, non-static scenarios (outside the
implemented slice despite their tags):

- **24** `*superproperty*` eval cases (direct eval referencing `super` in a
  field initializer).
- **12** files that combine public fields with computed names
  (`field-declaration`, `fields-computed-name-propname-constructor`,
  `fields-hash-constructor-is-a-valid-name`, `redeclaration-symbol`).
- **2** static-field ASI grammar (`static set\n*a`) — static fields excluded.
- **2** double-`super()` through a constructor arrow — this slice rejects the
  arrow-super shape rather than running fields twice.

All other remaining failures are private fields/methods/brands, static
fields/blocks, computed names, decorators, or tests requiring those semantics.
This receipt does **not** claim the whole class corpus.

## Crash evidence

Fixed and regression-tested with the Debug/static binary (no SIGABRT/SIGSEGV):

- `function g(){ class C { x = eval("this"); } return new C(); }` formerly
  aborted in `InitScopeSlotArray`; the helper now receives the normal arguments
  binding/scope state and `this` is published before field calls.
- fields plus an eval-containing arrow in a constructor formerly segfaulted in
  `ByteCodeWriter::CheckReg`; register release order was corrected and `this`
  is loaded from the constructor symbol instead of re-emitting a released
  override.
- invalid `await`/`yield` in the non-async/non-generator helper produces
  `SyntaxError`, not an abort.
- the new opcode is registered in the patchable-field writer path; Debug runs
  no longer hit the patchable-opcode assertion.

## Residual work and limitations

- Private fields/methods/brands, `#x in obj`, private-name environments.
- Static public/private fields and static blocks; computed field names and
  decorators.
- `super.prop`/`super()` inside field initializers; direct eval of `super` from
  an initializer.
- `super()` inside a constructor arrow is rejected with SyntaxError in this
  slice (never silently skipped).
- The eval `arguments` check is a source scan limited to direct eval calls and
  intentionally conservative around unusual property/method names.
- Deferred parsing of class members/class-containing functions is disabled as
  described above.
- JIT/Backend behavior is unverified: all evidence is Mac ARM64 `-nonative`.
  `InitClassField` has no JIT helper yet, and B4/JIT remains gated.

## Files

- `lib/Parser/ptree.h`, `lib/Parser/ptree.cpp`: constructor field-list pointer,
  field-initializer/super-in-arrow state.
- `lib/Parser/Parse.h`, `lib/Parser/Parse.cpp`: field grammar, helper
  synthesis, PID/name handling, arguments/eval statics, defer/eager policy.
- `lib/Runtime/ByteCode/ByteCodeGenerator.{h,cpp}`: constructor visit
  registration and class-field function attribute.
- `lib/Runtime/ByteCode/ByteCodeEmitter.cpp`: base/derived emission,
  NamedEvaluation hints, `InitClassField`.
- `lib/Runtime/ByteCode/OpCodes.h`,
  `lib/Runtime/Language/InterpreterHandler.inl`,
  `lib/Runtime/Language/InterpreterStackFrame.{h,cpp}`,
  `lib/Runtime/Language/JavascriptOperators.{h,cpp}`,
  `lib/Runtime/ByteCode/ByteCodeWriter.cpp`: `InitClassField` opcode and
  CreateDataPropertyOrThrow path.
- `lib/Runtime/Library/GlobalObject.cpp`: direct-eval `arguments` early error
  for field initializer contexts.
- `lib/Runtime/Base/FunctionInfo.h` and `ByteCodeSerializer.cpp`: initializer
  context attribute for arrows/helpers.
- `test/es7/class-fields-public.js`, `test/es7/rlexe.xml` (normal and forced
  defer parse), `test/es7/asyncawait-syntax.js`.
