# S1 lanes: computed field names + `super` in instance field initializers

## Overview

This slice closes two related S1 residuals on top of the public-fields
([`doc/c2-s1-public-fields.md`](doc/c2-s1-public-fields.md)), static-fields
([`doc/c2-s1-static-fields.md`](doc/c2-s1-static-fields.md)) and static-super
([`doc/c2-s1-super-static-init.md`](doc/c2-s1-super-static-init.md)) slices:

(a) Computed class field names: `[expr] = v`, `static [expr] = v`, and bare
`[expr]`, for instance and static fields.
(b) `super` references in INSTANCE field initializers: `x = super.foo`,
`x = super[expr]`, `super.foo = v` and compound forms, `super.m()`. This is
the instance analog of
[`doc/c2-s1-super-static-init.md`](doc/c2-s1-super-static-init.md) and reuses
its machinery with the instance home object. The static path is not regressed.

Spec target is ECMAScript 2025. Node (v26.5.0) was used as a differential
oracle for semantics and early errors; no node-vs-spec deviation was needed.

## Base probe (what already worked on 0c01875b5)

Probed by reading the implementation plus node differential runs:

- Computed METHOD/ACCESSOR names are ES6 and already work. Computed FIELD
  names were unconditionally rejected: `ParseClassDecl` raised `ERRsyntax`
  for any `isComputedName` field (`lib/Parser/Parse.cpp`, old field branch).
- Instance field `super` was rejected at parse: instance helpers kept
  `SuperRestrictionState::Disallowed`, so `ParseSuper` raised
  `ERRInvalidSuper` for direct code.
- Static computed fields and instance `super` were asserted as SyntaxError by
  the existing tests (`class-static-fields.js` early-error entry and the
  "instance field super remains a parse-time SyntaxError" entry, plus the
  `class-static-private-fields.js` computed entry). All three entries are
  updated by this slice.

Node semantics recorded for the implementation (all verified against node):

- All computed keys (methods, instance fields, static fields) evaluate once,
  in textual element order, at class definition time, before any static
  initializer runs. Instance initializers run per construction.
  (`key:m1 key:f1 key:sm1 key:sf1 key:m2 init:sf1` for an interleaved class.)
- `ToPropertyKey` runs once per element at definition time (side-effect
  counter stays flat across constructions).
- Key-expression scope is the class scope: outer bindings visible, the class
  name itself is TDZ (`ReferenceError`).
- Computed `"constructor"` defines an ordinary own property (instance and
  static); computed `"prototype"` defines an own property on instances but
  throws `TypeError` for static fields at definition time; `"__proto__"`
  defines an own property (prototype unchanged).
- Private names are never computed (`#["x"]` is a `SyntaxError`).
- Instance `super`: HomeObject is the derived prototype, receiver is the
  instance (gets read through the base prototype, sets create/shadow own
  properties, setters run with the instance as receiver, `super.m()` calls
  with the instance). Arrow functions inside the initializer inherit the
  binding; nested plain functions and `super()` are `SyntaxError`; base
  classes resolve through `Object.prototype`; `extends null` cannot construct
  (`TypeError` before fields run).
- Anonymous function/class values take the computed key as their `.name`.

## Design

### Computed field names

Parsing (`lib/Parser/Parse.cpp`, `Parse.h`, `ptree.h`, `ptree.cpp`):

- Field recognition now scans one token past `]` as well as past simple and
  private names, so `[expr] = v`, `[expr];`, `[expr]}` (and ASI) classify as
  fields while `[expr](` still restores and parses as a method/accessor.
  The old `isComputedName -> Error(ERRsyntax)` rejection is removed; the
  `constructor`/`prototype` early errors stay gated on simple names (computed
  keys resolve at runtime per the node behavior above).
- Each computed field member keeps `knopComputedName` as its name but wrapped
  in a self-contained `knopMember(computedName, keyDecl)` node: `keyDecl` is a
  hidden block-scoped `let` (`*s1keyN*`, globally numbered) in the class block
  holding the once-evaluated key. A `knopTrue` marker on the decl's `pnodeInit`
  records that an anonymous function/class value needs runtime naming.
  (`ParseNodeBin`'s member-name assert in `ptree.cpp` accepts the wrapper.)
- Key expressions are parsed before any helper exists, so `super` in a key
  stays `Disallowed` (a `SyntaxError`, matching node), and `arguments` in a
  key is a plain reference (only initializers carry the field-`arguments`
  early error).
- Anonymous function/class values with computed keys get
  `SetHasComputedName()` plus a fully qualified display hint
  (`C.prototype[k]` / `C.[k]`, via `ConstructFinalHintNode`) mirroring
  computed methods. Without the hint the value stays "anonymous" and the
  runtime skips installing `.name`; with it the existing
  `SetComputedNameVar` path installs the key as the name.

Emission (`lib/Runtime/ByteCode/ByteCodeEmitter.cpp`,
`lib/Runtime/ByteCode/ByteCodeGenerator.cpp`):

- Class evaluation stashes each key once: the member walk drains instance
  fields by source position around each member (both lists are textual, so
  the merge is exact) and evaluates static computed keys inline in member
  position. `EmitComputedFieldKey` emits the key expression under the class
  computed-name strict flag, converts once with the existing `Conv_Prop`
  (`ToPropertyKey`), and stores the result in the hidden slot.
- Field definitions load the stashed key and define with a new opcode (see
  below). Anonymous values are named with `SetComputedNameVar` first when the
  parser marked the slot.
- Visiting: instance key expressions are visited under the class-evaluation
  frame (constants/property-ids bind to the correct `FunctionBody`; visiting
  them under the constructor mis-bound string constants to the wrong table).
  Static keys were already covered by the generic member walk. The hidden
  slot declarations need no visit (same as private brand consts).

New opcode `InitClassFieldComputedName` (ElementI: value, instance, element):

- `lib/Runtime/ByteCode/OpCodes.h` (appended after `InitStaticPrivateField`,
  so no established value shifts), `InterpreterHandler.inl`,
  `InterpreterStackFrame.h/.cpp`, `JavascriptOperators.h/.cpp`.
- Semantics: dynamic-key `CreateDataPropertyOrThrow` (own W/E/C data
  property; inherited setters ignored; failed defines throw), plus the
  `prototype`-on-constructor `TypeError` shared with computed static methods.
  No existing opcode combines a dynamic key with field-define semantics
  (`InitComputedProperty` ignores define failures; `InitClassMember*` uses
  method attributes and setter semantics), hence the addition. Interpreter
  only, like `InitClassField`; no JIT/back-end change. Source bytecode headers
  are unchanged (append-only); the `verify-bytecode` path is covered by the
  gates below.

### Instance `super`

- `CreateClassFieldInitializerFunction` now grants `PropertyAllowed` to
  instance helpers too (static helpers unchanged). `super()` still throws via
  the existing `ParseSuper` call check; nested plain functions keep
  `Disallowed`; arrows inherit through the non-lambda helper. The stub-build
  (`buildAST == false`) early-accept in `ParseSuper` now keys on
  `m_inClassFieldInitializer` (both static and instance) instead of
  `m_inStaticClassFieldInitializer`, so nothing is recorded on the enclosing
  deferred proxy; validation happens on the full parse.
- One hidden `let` per class (`*s1home*`, created with the first initialized
  instance field, save/restored across nested classes) holds the class
  prototype. `EmitClass` publishes `protoLoc` to it right after class setup.
  The instance helper prologue (`LoadSuperObject`) binds its super slot from
  that slot via closure load when the helper is non-static and carries
  `pnodeFieldHome` (new `ParseNodeFnc` field, null otherwise). `this` remains
  the instance, so `LdHomeObjProto` yields the base prototype and all
  `Ld/StSuperFld` paths behave per spec. This is exact even if an earlier
  field mutates the instance's prototype (unlike deriving home from `this`).

## Files changed

- `lib/Parser/Parse.cpp`, `lib/Parser/Parse.h`, `lib/Parser/ptree.h`,
  `lib/Parser/ptree.cpp`: field recognition, hidden key/home declarations,
  wrapper names, runtime-name markers + display hints, stub accept,
  `PropertyAllowed` for instance helpers, `AttachClassFieldsToLambda`
  wrapper clones (shared key slot, helper-pid target).
- `lib/Runtime/ByteCode/ByteCodeEmitter.cpp`: `IsComputedFieldNameNode`
  family, `EmitComputedFieldKey[/Load]`, merged key walk + home publish in
  `EmitClass`, computed branches in both field-definition emitters,
  instance branch in `LoadSuperObject`.
- `lib/Runtime/ByteCode/ByteCodeGenerator.cpp`: instance key visits under
  the class frame.
- Opcode: `OpCodes.h`, `InterpreterHandler.inl`, `InterpreterStackFrame.*`,
  `JavascriptOperators.*`.
- Tests: `test/es7/class-fields-public.js` (+5 entries),
  `test/es7/class-static-fields.js` (+3 entries, 2 stale entries rewritten),
  `test/es7/class-static-private-fields.js` (1 stale assertion rewritten).
  No new test files, so `tools/build/gate_counts.json` is untouched.

## Tests and gates observed

- Focused suites, all green in every parse mode:
  `class-fields-public.js` 32/32, `class-static-fields.js` 28/28,
  `class-static-private-fields.js` 15/15, each under default,
  `-forcedeferparse` and `-nodeferparse`.
- Node differential probes covering the whole checklist (ordering,
  once-only conversion, scope/TDZ, special names, bare fields, value naming,
  super gets/sets/calls/compound/arrow/base/extends-null, every early-error
  shape): identical output except one eval-only divergence noted below.
- `mise x -- zig build check`: PASS, `passed 2012 / failed 0` (unchanged).
- `mise x -- zig build check-regression -Dprofile=debug-static`: PASS,
  `passed 2064 / failed 0` (unchanged).
- `git diff --check`: clean.
- test262 class-elements corpus
  (`test/language/statements/class/elements/*`, pinned corpus
  `2e0a567...`, `tools/test262/run.py`): lane build 1833 passed / 255
  failed vs base 1699 passed / 389 failed on the same 2088 scenarios.
  Fail-to-pass: 67 files (computed field names, interleavings, static field
  declaration, anonymous-function naming, instance super incl.
  `super-access-from-arrow-func-on-field.js` and the direct-eval
  superproperty tests). Pass-to-fail: 0. Remaining failures are outside this
  lane (async/generator methods, private+direct-eval, private brand checks).

## Residuals and limitations

- Direct-`eval` code: `super` in a nested plain function inside a field
  initializer is a runtime `ReferenceError` instead of the spec `SyntaxError`
  (node gives `SyntaxError`). This matches Chakra's pre-existing behavior for
  methods and static initializers in eval code (verified identical); direct
  code is a `SyntaxError` in all three. Eval-super residuals are out of scope.
- Nested-class key scope (`x = class { [super.f] = 1 }`) is accepted instead
  of rejected; the key inherits the outer helper's `PropertyAllowed`. The
  static lane has the identical hole (e.g. `static x = class { [super.f]() {}
  }`); parity kept, fix out of scope.
- Deferred-parse agreement relies on the standard gates above (whole-file
  `-forcedeferparse` variants) plus a dedicated test entry exercising
  classes created inside deferred functions, `eval` and `new Function`.
- No new opcodes beyond `InitClassFieldComputedName`; no JIT/back-end
  changes; fast-scan/deferred/full-parse agreement holds by construction
  (keys and initializers parse in both modes; validation defers to the full
  parse).
