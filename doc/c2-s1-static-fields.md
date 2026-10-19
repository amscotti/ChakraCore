# C2-S1: public static class fields (simple names)

Status: **landed in `041a9bb8f`** (feature `c76c75023`; base `d2ce6703f`); current integrated gates **2010/2062**. Evidence: focused static-public subset 75/75, **35 fail→pass / 0 pass→fail**; local es7 75/0. Private static/static blocks/computed static/super.prop/JIT remain residuals. This is the
follow-up to the landed public/private instance-field slices
(`doc/c2-s1-public-fields.md`, `doc/c2-s1-private-fields.md`). Static private
fields, static private methods/accessors, static blocks, computed names and
decorators remain out of scope.

## Scope and observable semantics

- `class C { static x; static y = 1; }` and the same elements in class
  expressions parse and define **own data properties on the constructor**
  with `writable: true`, `enumerable: true`, `configurable: true`.
- Static fields do not appear on the prototype or on constructed instances;
  no per-instance initialization is added.
- Class definition first evaluates and installs **all** methods, accessors
  and computed property names in textual order, including private methods and
  accessors. It then initializes the class-name binding, and only then runs
  static field initializers in their own textual order. From
  `class C { static f = typeof this.m; static m() {} }` the field sees
  `"function"` and may call `this.m()`. Because fields are applied last, a
  field wins over a same-named method/accessor:
  `class C { static x = 1; static x() {} }` has `C.x === 1`. Computed-name
  side effects occur before every static initializer, and a computed-name
  throw suppresses all of them.
- `this` inside an initializer is the constructor being defined (the derived
  constructor for subclasses); arrows and direct `eval` observe the same
  receiver. The class-name binding is in its TDZ while members/computed names
  evaluate (`class C { [C]() {} }` throws `ReferenceError`) and is initialized
  before static fields, so `class C { static self = C; }` and named class
  expressions work. Anonymous class expressions use their inferred name,
  including `export default class { ... }` (the constructor is named
  `"default"` before its static fields run).
- A missing initializer defines `undefined`. Duplicate static names run every
  initializer in order and the last definition wins. `CreateDataPropertyOrThrow`
  semantics are used, so inherited setters/read-only properties are ignored
  and a failed define on a non-extensible/non-configurable constructor (or a
  proxy trap returning false) throws `TypeError`.
- `constructor` is an early error for both instance and static fields
  (including string spellings); `prototype` is additionally an early error
  for static fields. Static blocks (`static {}`), static private fields/methods,
  and computed static field names are rejected with `SyntaxError` in this
  slice.
- Static initializers share field early errors: direct `arguments`, `super()`,
  and `await`/`yield` in the surrounding async/generator shape are
  `SyntaxError`s. Arbitrary JS, proxies and abrupt completion propagate before
  later static initializers run. Direct eval of `this` is supported.

## Parser/AST design

Static disambiguation extends the existing class-field look-ahead:

- `static;`, `static = 1`, `static }` and `static` followed by a line
  terminator remain an **instance field named `static`**.
- `static` followed by `(`, `*`, an identifier, string/numeric literal or
  `async`/`get`/`set` spellings is the static modifier. The normal field
  terminator/ASI detection then recognizes `static x`, `static x = 1`,
  `static x` before `}`, and synthetic forms such as `static static = 1`,
  `static async = 1`, `static get = 1`, and `static set\n*a(){}` (static field
  `set` followed by an instance generator).
- Static field entries are stored in `pnodeClass->pnodeMembers` (not the
  instance-field list) so their own textual order is preserved and their
  names participate in the same class visit. `pnode2` is either null (no
  initializer) or the initializer helper call, while method members keep a
  function node in `pnode2`. Emission separates the member walk into methods
  first and static fields last.
- Initializers reuse `CreateClassFieldInitializerFunction`, the hidden
  block-scoped helper and the existing `arguments`/await/yield/NamedEvaluation
  handling. Instance helpers keep the explicit constructor-`this` override;
  static helpers deliberately carry no AST override and receive the
  constructor at the class-definition emission site.
- `ClassBodyHasFieldSyntax` already recognizes `static` field spellings, so
  classes with only static fields keep their constructors on the existing
  eager path.

## Bytecode/runtime design

No opcode or PropertyId is added. Static fields reuse the existing
interpreter-only `InitClassField` (`CreateDataPropertyOrThrow`) and the
existing helper function objects.

- `EmitClass` walks `pnodeMembers` twice. The first pass evaluates/installs
  every method, accessor, computed name and private member in textual order;
  static field entries are skipped. The inner class-name binding is stored
  only after that pass (its TDZ covers computed names), and a second pass
  runs every static field initializer in textual order. This also guarantees
  private methods/accessors exist before a static initializer can use
  `new C().#m`.
- A static initializer helper is invoked with an explicit receiver register
  loaded from `pnodeClass->location` (the constructor). The result slot is
  acquired before the receiver temp so temp-stack LIFO order is preserved.
  A missing initializer loads `undefined`.
- `InitClassField` is emitted with the constructor as receiver. No instance
  field metadata is attached to the constructor (`hasInstanceFields` remains
  false for static-only classes), and the field is absent from instance and
  prototype property lists.
- Anonymous default-exported classes now pass the `"default"` name hint into
  `ParseClassDecl`, so the constructor name is installed before static field
  initializers run. This is spec-correct and is covered by the static-field
  test262 subset.

### Ownership/lifecycle record

- **Resource**: per-field initializer helper `ParseNodeFnc`/`FunctionBody`
  and class-block scope slot; one ordinary own data property per static field
  on the constructor.
- **Owner**: the class block owns helper functions and the class-name
  binding; the constructor owns the installed static properties. Helpers are
  published with the class block and remain reachable through its closure for
  the class lifetime.
- **Invalidation**: none after class evaluation. The constructor and its
  properties use the ordinary object/recycler lifetime; no native pointer,
  weak reference or manual release is introduced.
- **User-code/reentrancy**: initializers can allocate, throw, call proxies,
  eval, mutate the constructor and define/delete other static properties.
  `this` and the helper call are ordinary rooted values. Abrupt completion
  propagates before later members are evaluated.
- **Failure**: a failed `CreateDataPropertyOrThrow` throws `TypeError`;
  earlier static properties/helpers remain ordinary reachable objects. No
  partial native resource exists.
- **Thread/context**: normal current-thread script execution on the active
  `ThreadContext`; no new async job, callback, TLS or cross-thread access.

## Evidence

Build (Mac ARM64, interpreter `-nonative` evidence):

- `sh /tmp/opencode/cc-gate-lock.sh mise exec -- zig build --cache-dir "$PWD/.zig-cache"` — green.
- `... zig build -Dprofile=debug-static --cache-dir "$PWD/.zig-cache"` — green.
- `... zig build verify-bytecode --cache-dir "$PWD/.zig-cache"` — green; checked-in
  nojit bytecode headers unchanged (`verify_bytecode_pure`).
- Release `zig-out/bin/ch` sha256
  `70237022d8e138af512cf4266d539f29376a068ac2df73d9756142978bb0f35a`;
  `libChakraCore.dylib` sha256
  `fbbcac128a93ab685d5f4327bd6c0447627ac19481600b39435013a3c8498ff1`;
  Debug `out/debug-static/bin/ch` sha256
  `66dc4fb207ee6a5d59c63b809704bde83072be15dcea0325b221fd1f933b0926`
  (the launcher hash `70237022…` is stable).

Local suites (`disable_jit`, release unless noted):

- `test/es7` — **75 passed / 0 failed** (includes normal and
  `-force:deferparse` entries for `class-static-fields.js`). The test covers
  field-before-method calls, fields winning after methods, computed-name
  ordering/failure, class-name TDZ, and late private method/accessor use from
  static initializers.
- `test/es6` — 233/0; `test/es6module` — 31/0; `test/es5` — 56/0;
  `test/Function` — 70/0.
- Debug `class-static-fields.js` — pass in normal, `-force:deferparse` and
  `-noDeferParse`; Debug `class-fields-public.js`,
  `class-private-fields.js` and `class-private-methods.js` also pass. No
  SIGABRT/SIGSEGV observed (including the late-private-method regression).
- Debug `test/es7` reports 74/1 because
  `finalization-registry-errors.js` (unrelated FinalizationRegistry GC test)
  fails; the base Debug binary reproduces that failure with the same command.

Focused test262 subset (pinned corpus revision from `out/test262`,
`-nonative -Test262 -CollectGarbage`, controls 18/18): sources tagged
`class-static-fields-public` excluding private/static-block/computed/decorator
interactions (38 files, 75 strict/default/module scenarios):

| run | passed | failed |
|---|---:|---:|
| base `d2ce6703f` binary `2e9d99e3…` | 40 | 35 |
| S1 static-fields binary `70237022…` | **75** | **0** |

Scenario diff: **35 fail→pass, 0 pass→fail**. Additional diagnostic over all
421 scenarios in the 213 files carrying the `class-static-fields-public` tag:
56 → 91 passed, **35 fail→pass, 0 pass→fail**. The remaining 330 failures are
excluded interactions: 266 computed-name, 56 private-static/private-method,
4 static-block, and 4 `super.prop`-from-arrow files. None is a newly
introduced pass→fail.

## Residual work and limitations

- Static private fields/methods/accessors, private brands on constructors,
  static blocks and decorators are not implemented. They are rejected as
  `SyntaxError`; static-block tests are covered as explicit errors in the
  focused test.
- Computed static field names are rejected as `SyntaxError` rather than
  evaluated. In particular `static ["prototype"]` would be a runtime
  `TypeError` under the spec but is rejected at parse time. Computed static
  methods continue to use the existing method path.
- `super.prop` inside static field initializers is a `SyntaxError` (the
  initializer helper keeps `SuperRestrictionState::Disallowed`, matching the
  existing instance-field residual). `super()` is an early error as required.
- Direct eval inside a static initializer can observe `this` and the class
  name, but the existing eval-`super()` restrictions from the instance-field
  slice still apply.
- No JIT/backend support is claimed. `InitClassField` has no JIT helper and
  remains interpreter-only; **B4 must run this focused test262 subset and
  `test/es7/class-static-fields.js` under SimpleJIT/FullJIT (dynapogo) before
  any JIT/default claim**.
- The observed `-force:DeferParse -UseParserStateCache -ParserStateCache`
  failure in the new test is shared with the existing
  `class-fields-public.js` test in this environment and is not specific to
  static fields; normal and forced-defer modes are green.

## Files

- `lib/Parser/Parse.cpp`: static modifier/field disambiguation, member-list
  placement, no-override static helpers, static block/name early errors, and
  anonymous default-export class naming.
- `lib/Runtime/ByteCode/ByteCodeGenerator.{h,cpp}`: binds static field helper
  calls in the class visit and keeps member ordering metadata.
- `lib/Runtime/ByteCode/ByteCodeEmitter.cpp`: static member detection,
  class-name binding order, and `EmitStaticClassFieldDefinition` using
  `InitClassField`.
- `test/es7/class-static-fields.js`, `test/es7/rlexe.xml`: focused regression
  test (normal and forced defer parse).
