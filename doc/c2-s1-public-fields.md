# C2-S1: public instance class fields (simple identifier names)

Status: `<pending>` — implementation and focused Mac no-JIT evidence are in the
working tree; integration has not yet recorded a commit hash or updated the
shared C1/C2 status documents.

This is the first bounded S1 slice: **non-static public instance fields with
simple (non-computed) names and optional initializers**, in class declarations
and class expressions. Private fields/methods/brands, static fields, computed
field names, static blocks and decorators are deliberately excluded.

## Scope and observable semantics

- `class C { x; y = 1; m() {} }` parses and constructs normally.
- Every field defines an **own data property** on the instance with
  `writable: true`, `enumerable: true`, `configurable: true`, in textual
  declaration order. A field without an initializer defines the value
  `undefined`; duplicate public names are allowed and the last definition wins
  while every initializer still executes in order.
- Initializer expressions evaluate per construction with `this` bound to the
  new instance. Closures and arrow functions created by an initializer keep
  that instance as their lexical `this`.
- Base class: fields initialize after the instance object is created and
  **before parameter default initializers and the constructor body**.
- Derived class: fields initialize **immediately after `super()` returns and
  binds `this`**, before the rest of the expression/constructor body resumes.
  A derived class without an explicit constructor inherits the same behavior
  from the generated default constructor.
- The field definition uses `CreateDataProperty` semantics, not assignment:
  an inherited setter is not called and an inherited read-only property does
  not block the own field. (The emitter uses `InitFld`, the existing
  object-literal own-property opcode.)
- `constructor` is rejected as a field name (early error, including the string
  literal spelling), matching the class-fields early error. `arguments` in an
  initializer is also a syntax error.

## Parser/AST design

`ParseNodeFnc` gains `pnodeClassFields`, a textual-order list of `knopMember`
records: `pnode1` is the field name (`knopStr`) and `pnode2` is either a call
to a synthesized initializer helper or null for a field without an initializer.

For each initializer, the parser synthesizes a hidden block-scoped **function
declaration** in the class body (`*s1fieldN*`, not a source-writable name).
The helper has a real parameter/body scope and strict `this`, and its body is
`return <initializer>;`. Parsing the expression inside this helper ensures
nested functions/arrows are parented to the initializer rather than to the
class's enclosing function. The helper symbol is marked as captured so the
class block allocates it a scope slot that the constructor can load through the
closure. No initializer means no helper: the constructor emission defines
`undefined` directly.

The field scanner first looks one token past a simple member name and restores
the scanner when the member is a method/accessor. It recognizes `=`, `;`, `}`
and ASI line breaks; `get`/`set` remain accessor prefixes when followed by a
property name, and `async` remains a field name when terminated or followed by
a line break. Static and computed field definitions are rejected explicitly in
this slice.

Deferred-parse accounting: helper declarations are synthetic nested functions,
so the parser keeps class members and functions inside field initializers eager
and does not defer functions whose body contains `class` syntax. The fast scan
also reserves a generated-default helper slot for each initialized field so a
later deferred reparse cannot see a smaller nested-function array than the
real parse produces. This is a correctness trade-off for S1; it does not
change defer-parse semantics of unrelated functions.

## Bytecode/runtime design

- No new opcodes and no new static `PropertyIds`. Field names use the existing
  pid/`GetPropertyId` and inline-cache path; `InitFld` applies the own-property
  definition.
- The field-call ASTs are registered during the constructor's visit pass
  (`VisitNestedScopes`) and emitted by
  `ByteCodeGenerator::EmitClassFieldInitializerCalls`.
- Base constructors emit the calls from `EmitOneFunction` after the prologue
  has created `this` and before `EmitDefaultArgs`.
- Derived constructors emit them from `EmitSuperCall` after `super()` binds
  `this` and stores it through the constructor's special `this` symbol.
- The helper call uses the existing `pnodeThisOverride` call shape, passing the
  constructor's instance as the explicit receiver. This was already used for
  optional-call member bases; no call ABI was added.

### Ownership/lifecycle record

- **Resource**: per-field helper `ParseNodeFnc` / `FunctionBody` and its class
  block scope slot.
- **Owner**: class body's lexical block scope; created during class definition
  evaluation, held by the constructor closure for the lifetime of the class.
- **Invalidation**: none after publication; GC traces the scope slot and
  function object through the existing closure machinery. The helper is a
  normal engine function, not a native allocation, so no Zig/native release
  path is added.
- **User-code/reentrancy**: initializer calls can run arbitrary JS, allocate,
  throw and reenter construction. The instance is an ordinary rooted `this`
  value; the helper has no retained native pointers. Abrupt completion
  propagates before later fields are defined, matching textual evaluation.
- **Failure**: no partial native resources; a throwing initializer simply
  leaves already-defined earlier fields on the abandoned instance.
- **Thread/context**: normal script execution on the current `ThreadContext`;
  no new async job, callback, TLS or cross-thread access.

## Evidence

Build and focused suites (Mac ARM64, `-nonative`, Test profile):

- `sh /tmp/opencode/cc-gate-lock.sh mise exec -- zig build --cache-dir "$PWD/.zig-cache"` — green (including 95/95 PAL-config checks).
- `python3 test/runtests.py -b "$PWD/zig-out/bin/ch" -t --variants disable_jit -- test/es7` — **48 passed / 0 failed**. This includes the new `es7/class-fields-public.js` and the updated `asyncawait-syntax.js` expectation (`async` followed by a line terminator is a field, then an ordinary method).
- `... -- test/es6` — **233 / 0**.
- `... -- test/es6module` — **31 / 0**.
- `es7/deferparseclass.js` under `-force:deferparse` — **5 / 0**, unchanged.

Focused test262 subset (same pinned corpus, `-nonative`, scenarios):
`test/language/statements/class/elements/**/*.js` +
`test/language/expressions/class/elements/**/*.js`
(2,962 files / 5,897 scenarios; controls 18/18).

| run | passed | failed |
|---|---:|---:|
| base binary `10d44c972` before S1 | 1,450 | 4,447 |
| S1 binary after | 1,748 | 4,149 |

Scenario diff: **298 fail→pass, 0 pass→fail**. Of the 298 fail→pass:

- 296 are `class-fields-public`-only, non-computed, non-static public-field
  scenarios.
- 2 are `privatename-not-valid-eval-earlyerr-3.js` (tagged
  `class-fields-private` + `class-fields-public`). They pass because the public
  `y = eval(...)` field now parses and the eval private-name error occurs at
  runtime; this is public-field spillover, **not** private-field support.

The 4,149 remaining failures are intentionally outside this slice: private
fields/methods/brands, static fields/blocks, computed names, `super` in field
initializers, decorators, and tests that require those semantics. This receipt
does **not** claim the whole class corpus.

## Residual work and limitations

- Private fields/methods/brands, `#x in obj`, private-name environments.
- Static public/private fields and static blocks.
- Computed field names (the parser rejects them in this slice).
- `super.prop`/`super()` inside field initializers (rejected by the helper's
  `SuperRestrictionState::Disallowed`), and `new.target`/`arguments` detail
  beyond the implemented `arguments` early error.
- A derived constructor that calls `super()` from a nested arrow does not
  initialize fields in this slice; the field-call operands are registered in
  the constructor frame, so the emitter intentionally leaves this shape
  unsupported rather than corrupting the arrow's frame.
- Deferred parsing of class members / class-containing functions is disabled
  as described above for layout safety.
- JIT/Backend behavior is unverified: all evidence is Mac ARM64 `-nonative`.
  The implementation adds no new opcodes, but B4/JIT remains gated and no JIT
  claims are made.

## Files

- `lib/Parser/ptree.h`, `lib/Parser/ptree.cpp`: constructor field-list pointer.
- `lib/Parser/Parse.h`, `lib/Parser/Parse.cpp`: field grammar, helper
  synthesis, helper/fast-scan accounting, class-block binding.
- `lib/Runtime/ByteCode/ByteCodeGenerator.h`,
  `lib/Runtime/ByteCode/ByteCodeGenerator.cpp`: constructor visit registration.
- `lib/Runtime/ByteCode/ByteCodeEmitter.cpp`: base/derived emission and
  `InitFld` definitions.
- `test/es7/class-fields-public.js`, `test/es7/rlexe.xml`: focused regression.
- `test/es7/asyncawait-syntax.js`: updated grammar expectation made necessary
  by public fields.
