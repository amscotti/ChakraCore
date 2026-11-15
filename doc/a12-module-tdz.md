# Lane A12 — cross-module lexical TDZ (taken over by parent; lane stalled)

Module export slots were pre-filled with `undefined`, so any read of a
binding whose declaration had not been evaluated yet silently produced
`undefined` — including cyclic-import reads, namespace reads, and
`typeof`, all of which must throw `ReferenceError` per ECMA-262.

## Fix

- `SourceTextModuleRecord::InitializeLocalExports` pre-fills export slots
  with the undeclared-block-var (TDZ) sentinel instead of `undefined`.
- `ByteCodeGenerator::EmitModuleExportAccess` emits `ChkUndecl` after
  `LdModuleSlot` (direct import reads; covers `typeof` which routes through
  the same emission in `EmitPropTypeof`).
- `JavascriptOperators::OP_LdModuleSlot` (interpreter helper path) throws on
  the sentinel.
- `ModuleNamespace::GetNSSlot` throws on the sentinel. `HasProperty` stays
  map-based (no TDZ throw on `in`, per spec); `GetOwnPropertyDescriptor`
  routes through `GetPropertyQuery` and throws (per spec).
- `ByteCodeGenerator::DefineUserVars`: uninitialized `export var` bindings
  are hoisted + initialized to `undefined` at instantiation, but nothing ever
  wrote their slots. They are initialized with `LdUndef` + `StModuleSlot` in
  the module-body prologue (before any user code, so hoisting holds across
  cycles). `let`/`const` block vars are excluded to preserve TDZ.

## Corrections to pre-existing tests

- `test/es6module/module-bugfixes.js` #10 (`Issue 5236`) line 191 asserted
  `isUndefined(otherDefault)` where `otherDefault` is test5236b's
  default-exported *class*, read while test5236c evaluates (before
  test5236b's body runs). A class binding in TDZ must throw on read — the
  Node oracle throws `ReferenceError` on the identical shape — so the
  expectation was updated to `assert.throws(..., ReferenceError)`. The old
  expectation captured the pre-fix `undefined` pre-fill.

## Verification (parent)

- New `test/es6module/module-cycletdz.js` (4 subtests: cyclic read, namespace
  read, `typeof` read, post-init order): pre-fix binary prints
  `FAILED: ... did not throw` on the first three (red proof); post-fix
  `pass` x4 in interpreted, FullJIT, and default tiers.
- Node oracle: identical cycle shape throws `ReferenceError` with events `B`
  (matches the test's `expectedEvents`).
- `module-bugfixes.js` #10, `module-namespace.js`, `export_namespace_as.js`:
  failed under the lane's pre-fill-only patch (uninitialized `var` exports);
  pass with the prologue init.
- Suites: `zig build check` green (pin 2059/2111); es6module 32/0, es6 224/0,
  Basics 48/48, es7 111/0, Intl 17/0, UnifiedRegex 23/0 (JIT binary legs).
- Durable pins: `module-cycletdz.js` + baseline (new rlexe entry); corrected
  `module-bugfixes.js` #10.
