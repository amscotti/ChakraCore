# C2-S1: private static class fields

Status: **pending**. Implementation and focused evidence are on branch
`s1-static-private` (base `f352f4e5e`); B4 JIT/default gates have not run.
This slice implements `static #x;` and `static #x = expr;` in class
declarations and class expressions. Private static methods/accessors, static
blocks, computed static names and decorators remain out of scope and are
rejected as `SyntaxError`.

## Scope and observable semantics

- A private static field is own private storage of the class constructor. It
  is not a property: it is absent from `Object.keys`,
  `getOwnPropertyNames`, `getOwnPropertySymbols`, property descriptors,
  `for-in` and proxies, and it never appears on the prototype or on
  constructed instances.
- Static fields run after every method, accessor and computed name has been
  installed and after the inner class-name binding is initialized. Public and
  private static fields then run in one textual order, so `this.a` /
  `C.#p` interleaving is preserved across initializers and methods.
- A missing initializer adds `undefined` to the private brand; it does not
  leave the brand absent. Initializer `this` is the constructor being
  defined. Arrows and nested functions observe that `this`; direct eval
  cannot resolve private names (existing residual).
- Duplicate private names are early errors across static fields, instance
  fields, private methods and private accessors. Duplicate public static
  names keep the existing public-field behavior: every initializer runs in
  order and the last definition wins. `#constructor` is an early error in
  both declaration and use.
- Each class evaluation creates a fresh brand, so two evaluations of the same
  class source are mutually unbranded. A subclass constructor is not branded
  by its base: `Base.#x`/inherited `this.#x` calls on the derived constructor
  throw `TypeError`; `#x in Derived` is `false`. Nested classes may capture or
  shadow outer private static names.
- `C.#x`, `this.#x`, reads, writes, compound/update forms, `typeof` and
  `#x in obj` use the existing private opcodes. Wrong receivers, primitives,
  proxies and unbranded objects produce the existing `TypeError` behavior.
- Private state remains readable/writable after `Object.freeze(C)`, because it
  is stored in the brand map rather than in properties. Static private field
  addition also works when the constructor is already frozen/non-extensible,
  matching Node and the spec's private-element model. The legacy instance
  private-field add gate is unchanged (see residual).

## Parser/AST design

- `static` followed by `tkPrivateName` is recognized by the existing static
  modifier/field look-ahead. A field terminator (`=`, `;`, `}` or ASI) marks
  the name as `PrivateNameKind::Field`; otherwise the static form is rejected
  explicitly as an out-of-scope static private method. Static private
  accessors are rejected in the existing private accessor branch.
- Static private fields are appended to `pnodeClass->pnodeMembers`, not the
  instance-field list. Their `pnode2` is null or the existing field
  initializer call node, which preserves textual order with public static
  fields and static methods. The first class-emission pass skips them; the
  second pass runs them.
- A `*` following a private field name after a line terminator starts the next
  class element (ASI), rather than being consumed as a generator modifier on
  the private name: `static #x\n*gen(){}` and
  `static #x\n*[Symbol.iterator](){}` are valid. The same holds for instance
  private fields. Without a line terminator (`#x *gen(){}`) the form remains a
  `SyntaxError`. Public field names already followed this ASI path.
- The existing hidden block-scoped private-name const, deferred reference
  resolution, duplicate checks, `#constructor` check, `arguments`/`super()`
  early errors and NamedEvaluation (`#field`) are reused unchanged.
- Private static methods/accessors, static blocks and computed static field
  names continue to report `SyntaxError`; no new AST/PropertyId kinds are
  introduced.

## Bytecode/runtime design

No PropertyId was added. The implementation reuses the existing private
machinery and appends one interpreter-only opcode:

- `NewPrivateName` — one hidden per-name brand `JavascriptWeakMap` per class
  evaluation, stored in the class-block const.
- `InitStaticPrivateField` (appended after `StPrivateMethod`) — add the static
  field value to that map keyed by the constructor. Like the spec's
  `PrivateFieldAdd`, it rejects a duplicate brand but does not require the
  constructor to be extensible, so frozen constructors accept their own
  private static storage.
- `LdPrivateField`, `StPrivateField`, `HasPrivateField` — unchanged access,
  write and `#x in obj` behavior for constructor receivers.

`IsStaticFieldMemberNode` now accepts either a public `knopStr` name or a
private `knopName`. `EmitStaticClassFieldDefinition` loads the hidden brand
const for private names and emits `InitStaticPrivateField(value, constructor,
brand, value)`; public names still emit `InitClassField`. The initializer
helper is called with the constructor as its explicit receiver, exactly as
for public static fields. `EmitClass` keeps its existing two-pass shape:
methods/accessors/private definitions first, class-name binding, then all
static fields in textual order.

### Ownership/lifecycle record

- **Resource**: one hidden `JavascriptWeakMap` per declared private name per
  class evaluation (existing private-field resource), plus one weak map entry
  per initialized private static field. No new native allocation; one appended
  interpreter opcode.
- **Owner**: the hidden block-scoped const owns the map; the map owns values;
  the constructor is the weak key for a static name. The class block and its
  closure keep the map alive for the class lifetime.
- **Invalidation**: none explicit. Static entries are weakly keyed by the
  constructor; ordinary weak-map and class/object lifetime reclaim them. No
  raw pointer, manual release or cross-context handle is introduced.
- **User-code/reentrancy**: initializers may allocate, call proxies, eval,
  mutate/freeze the constructor or throw. Abrupt completion prevents later
  static fields from running; entries already added remain ordinary weak-map
  entries. `this` and helper calls are normal rooted values.
- **Failure**: wrong receiver/primitive or duplicate static add throws
  `TypeError`; static adds on a frozen/non-extensible constructor succeed.
  No partial native resource exists.
- **Thread/context**: current-thread script execution on the active
  `ThreadContext`; no new async job, callback, TLS or cross-thread access.

## Evidence

Build (Mac ARM64, `test` profile, interpreter `-nonative` evidence), all under
`sh /tmp/opencode/cc-gate-lock.sh ...` with local `.zig-cache`:

- `mise exec -- zig build --cache-dir "$PWD/.zig-cache"` — green.
- `... zig build -Dprofile=debug-static --cache-dir "$PWD/.zig-cache"` — green.
- `... zig build verify-bytecode --cache-dir "$PWD/.zig-cache"` — green;
  checked-in nojit bytecode headers unchanged (`verify_bytecode_pure`).
- Release launcher `zig-out/bin/ch` sha256
  `dcad64e59ab207cbf40b75161110dd1ed473f6db7b69af86ab6175dcdf2c2fe3`
  (stable launcher); `libChakraCore.dylib` sha256
  `b02ed4b6f715e6b58ad656494e57c239319e4b13aaf7fefe7ddadc1a69d5606c`.
  Debug `out/debug-static/bin/ch` sha256
  `cba5eb427b5f16d0e471fcb17e7f57d7150092f0f83d6eccd540f9b76ae72007`.

Local suites (`disable_jit`, Release):

- `test/es7/class-static-private-fields.js` — **12/12** in normal,
  `-force:deferparse` and `-noDeferParse` modes. The Debug/static binary
  passes the same three modes.
- `python3 test/runtests.py -t -b "$PWD/zig-out/bin/ch" --variants disable_jit`
  — **2012 passed / 0 failed**; `es7: 77/0` (the two new rlexe entries),
  `es6: 233/0`, `es6module: 31/0`.
- Existing `class-static-fields.js` was updated to expect static private
  fields to parse instead of throwing; it passes in normal and forced-defer
  modes. `class-private-fields.js`, `class-private-methods.js` and
  `class-fields-public.js` remain green in Release and Debug.

Focused test262 subset (pinned corpus
`2e0a56762801e275a9fdf96dc49d90ba0cddcf63`, controls 18/18,
`-nonative -Test262 -CollectGarbage`). Selection: sources tagged
`class-static-fields-private` (345 files) excluding files that also carry
`class-static-methods-private` or `class-methods-private`; no tagged file
contains a static block, computed class-element name or decorator. This leaves
**303 files / 602 scenarios**. The same explicit file list and runner command
were used before and after:

| run | passed | failed |
|---|---:|---:|
| base `f352f4e5e` binary/dylib (`d437ee0e…`) | 29 | 573 |
| S1 static-private binary/dylib (`b02ed4b6…`) | **600** | **2** |

Scenario diff: **571 fail→pass, 0 pass→fail**. Feature counts reported for
`class-static-fields-private`: 29/573 → 600/2.

Residual in this subset: the two scenarios (default and strict) of
`private-static-field-visible-to-direct-eval.js`. Direct eval inside a static
method cannot see the class private environment, the same pre-existing
direct-eval limitation as instance private fields. No other residual was
observed; other files in the tag set are excluded only for private
method/accessor interactions outside this slice.

## Residual work and limitations

- Private static methods/accessors, static blocks, computed static field names
  and decorators are not implemented. They are rejected as `SyntaxError`
  rather than evaluated.
- Direct eval cannot resolve private static (or instance) names; `eval`
  inside a static method/initializer that references `this.#x` remains a
  `SyntaxError`.
- Static private adds accept frozen/non-extensible constructors. The legacy
  instance private-field add still rejects non-extensible receivers. That
  instance-only gate follows the earlier private-field slice and is a tracked
  divergence from current ECMAScript private elements (which are internal
  slots and are not extensibility-gated).
- `super.prop` in field initializers is a runtime `ReferenceError`, not an
  early `SyntaxError`; `super()` is correctly an early error. The field-helper
  path in this checkout still rejects direct field-initializer `super.prop`
  forms at parse time, so those forms are not claimed as conforming. Direct
  eval of `super`/private names is not extended.
- Brands are `JavascriptWeakMap` entries rather than an internal-slot
  implementation. `-CollectGarbage` is used in test262 runs, but heavy
  GC-stress and weak-key lifetime are not separately exercised.
- No JIT/backend support is claimed. `InitClassField` and the private opcodes
  are interpreter-only. **B4 must run this focused test262 subset plus
  `test/es7/class-static-private-fields.js` under SimpleJIT/FullJIT
  (dynapogo) before any JIT or default claim.**

## Files

- `lib/Parser/Parse.cpp`: accepts static private field declarations, keeps
  them in the class member list, rejects static private methods/accessors,
  and reuses the private name/brand const machinery.
- `lib/Runtime/ByteCode/ByteCodeEmitter.cpp`: recognizes private static field
  members and installs them with `InitStaticPrivateField` on the constructor
  in the second class-emission pass.
- `lib/Runtime/ByteCode/OpCodes.h`,
  `lib/Runtime/Language/InterpreterHandler.inl`,
  `lib/Runtime/Language/InterpreterStackFrame.{h,cpp}`,
  `lib/Runtime/Language/JavascriptOperators.{h,cpp}`: appended
  interpreter-only `InitStaticPrivateField` opcode and its non-extensibility-
  gated `PrivateFieldAdd` implementation.
- `test/es7/class-static-private-fields.js`, `test/es7/rlexe.xml`: focused
  regression test and normal/forced-defer entries.
- `test/es7/class-static-fields.js`: updates the earlier public-static
  expectation for the now-supported static private field form.
