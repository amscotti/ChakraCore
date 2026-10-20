# C2-S1: super.property in static field initializers (public and private)

Status: **implemented on `lane/super-static-init`** (base `7eeccbb5c`);
gates hold **2012/0 (test) and 2064/0 (debug-static)** with no scenario-count
movement. This closes the shared `super.prop`-in-static-initializer residual
noted in [c2-s1-static-fields.md](c2-s1-static-fields.md) for public **and**
private statics in one fix. Static blocks, computed names, private static
methods, decorators, instance-field super and eval-super residuals are
unchanged and out of scope.

## Scope and observable semantics

- `static x = super.foo`, `static x = super[foo]`, `static #x = super.foo`,
  `super.m()` calls (including from arrows: `static x = (() => super.foo)()`)
  and `super.y = v` assignments work in static initializers, for base and
  derived classes alike.
- `[[HomeObject]]` is the constructor being defined: `super.foo` reads the
  constructor's `[[Prototype]]` with `this` bound to the constructor. Derived
  `static x = super.foo` reads the parent static (e.g. `7`); base
  `static #x = super.foo` is `undefined` (reads through `Function.prototype`,
  matching Node); `super.m()` observes `this === Derived`.
- Assignment follows ordinary super-assignment semantics: a base setter runs
  with the derived constructor as receiver; a plain `super.foo = v` defines
  an own property on the derived constructor and leaves the base binding
  alone. `extends null` resolves through `Function.prototype` (`undefined`).
- `super()` stays a `SyntaxError` early error in static initializers (base
  and derived). Instance field `super` stays a parse `SyntaxError` in direct
  code (residual unchanged; eval-defined classes still fail at construction
  with `ReferenceError`, as before).

## Parser/AST design

- `CreateClassFieldInitializerFunction` takes `isStatic`: static helpers get
  `superRestrictionState = PropertyAllowed` (instance helpers keep
  `Disallowed`) plus a new `ParseNodeFnc::isStaticClassFieldInitializer`
  marker. `super()` is still rejected by the existing `ParseSuper` call check,
  and nested plain functions keep `Disallowed`.
- A new `Parser::m_inStaticClassFieldInitializer` flag (save/restored with
  `m_inClassFieldInitializer`, nest-safe for classes inside initializers) lets
  stub builds (`buildAST == false`, which parse initializer expressions to
  discover nested functions) accept `super.prop`/`super[foo]` without
  recording `SuperReference` on the enclosing function. Recording it there
  would poison the deferred proxy's stable-mask attributes, which the full
  parse records on the helper instead. `super()` still errors in stubs,
  matching the full parse.

## Bytecode/runtime design

No opcode or PropertyId is added. The helper-call path is unchanged; only the
helper entry prologue changes:

- `LoadSuperObject` binds the helper's super slot with `Ld_A` from `this`
  (the constructor, passed as the helper's explicit receiver) when the
  function is a static field initializer with a `this` symbol, instead of
  `LdHomeObj` (helpers are created at class-block entry, before the
  constructor exists, so they have no home object). `LdHomeObjProto` on the
  constructor yields exactly the specified super base, and the existing
  `LdSuperFld`/`StSuperFld` paths receive the correct base with
  `this == constructor`. Helpers without `super` emit byte-identical code.
- Instance helpers keep the `LdHomeObj` path (unreachable for `super`, which
  stays a parse error for them).

### Ownership/lifecycle record

- No new allocation, handle, or release path: one extra register-to-register
  copy at helper entry using the helper frame's existing `this`/super
  registers. `Ld_A` cannot run user code or fail.
- User-code/reentrancy posture is unchanged: initializers already run
  arbitrary code with the constructor as receiver; the super base is derived
  per evaluation from that same receiver, so re-evaluation creates no shared
  state.
- Failure modes are unchanged: missing super methods throw `TypeError` from
  the existing `LdSuperFld` path; base-class reads resolve to `undefined`
  through `Function.prototype` with no throw.

## Evidence

Builds (Mac ARM64):

- `mise x -- zig build` — green (`zig-out/bin/ch` rebuilt).
- `mise x -- zig build -Dprofile=debug-static` — green
  (`out/debug-static/bin/ch` rebuilt).
- `mise x -- zig build check` — green, including
  `check_regression: PASS: passed 2012 / failed 0`.

Local suites (`disable_jit`):

- `python3 test/runtests.py --test --variants=disable_jit --binary=$PWD/zig-out/bin/ch` —
  **Total: passed 2012, failed 0** (es7 77/0 within it).
- `python3 test/runtests.py --debug --variants=disable_jit --binary=$PWD/out/debug-static/bin/ch` —
  **Total: passed 2064, failed 0**.
- Scoped class files (public/private static + public/private instance +
  private methods): 12/12 test-profile, 4/4 debug-profile.
- New cases: 3 entries in `test/es7/class-static-fields.js` (super reads
  incl. base/`extends null`/arrow/call/`this`, super assignment incl.
  setter/receiver, instance-super `SyntaxError` lock) and 1 entry in
  `test/es7/class-static-private-fields.js` (private super reads, mixed
  public/private ordering). No new files; scenario totals unmoved.

Probes (quoted, `zig-out/bin/ch` unless noted):

- Derived `static x = super.foo` reads parent static: `x=7 y=B w=7 v=7`;
  assignment `(super.zz = 42)` gives `B.zz=42 A.zz=undefined`; base gives
  `u=undefined t=function`; private `base-priv=undefined derived-priv=7`;
  `super()` → `SyntaxError`; direct instance `super` → `SyntaxError`;
  eval-defined instance `super` → `ReferenceError` at construction (all
  pre-existing residual paths unchanged).
- Same probe scripts under `node` produce identical values, including
  setter-receiver doubling (`B.sv=42`), own-property creation
  (`C.foo=99 A.foo=7`), and `extends null` (`undefined`).
- `-force:deferparse` and `-nonative` runs of the probes match; classes
  defined inside deferred functions also resolve correctly.

Corpus: `out/test262` is absent in this worktree (ignored corpus not
materialized), so no scoped test262 run was possible; in-tree suites plus the
Node-parity JS probes above are the regression net, per the lane brief.

## Residual work and limitations

- Instance field `super` (parse `SyntaxError` direct, runtime
  `ReferenceError` for eval-defined) is unchanged; a future lane must give
  instance helpers a real home environment (`HomeObject` = prototype), which
  the `Ld_A`-from-`this` trick used here would get wrong.
- Direct `eval("super...")` inside a static initializer still resolves
  through the caller check and fails cleanly; eval-super inheritance of the
  static home object is not implemented (pre-existing residual class).
- `delete super.x` keeps its existing `RuntimeReferenceError` path;
  `super?.` keeps its existing `SyntaxError`; computed static names, static
  blocks and private static methods keep their existing `SyntaxError`s.
- No JIT/backend claim: no new opcode was added and helper entry uses only
  `Ld_A`, but JIT coverage of class static fields remains B4's to prove
  before any JIT/default claim, as in the prior S1 receipts.

## Files

- `lib/Parser/ptree.h`, `lib/Parser/ptree.cpp`: `isStaticClassFieldInitializer`
  marker on `ParseNodeFnc`, default false.
- `lib/Parser/Parse.h`, `lib/Parser/Parse.cpp`: `m_inStaticClassFieldInitializer`
  parse flag; `isStatic` parameter on `CreateClassFieldInitializerFunction`
  (helper `PropertyAllowed` state + marker); shared save/restore at the
  field-parse site; stub-build early return in `ParseSuper`.
- `lib/Runtime/ByteCode/ByteCodeEmitter.cpp`: `LoadSuperObject` binds the
  super slot from `this` for static field initializer helpers.
- `test/es7/class-static-fields.js`,
  `test/es7/class-static-private-fields.js`: new cases inside the already
  registered files (scenario totals unmoved).
