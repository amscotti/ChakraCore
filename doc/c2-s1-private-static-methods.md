# C2-S1: private static methods and accessors

Status: **implemented on `lane/private-static-methods`** (base `7eeccbb5c`);
current integrated gates **2012/2064** (unchanged). Evidence: in-tree es7
`class-private-methods.js` 25/25 and `class-static-private-fields.js` 13/13
in normal/`-force:deferparse`/`-noDeferParse` modes on release and
debug-static binaries; full `runtests.py --variants disable_jit`
**2012 passed / 0 failed** on both binaries; `verify-bytecode` green. The
`out/test262` corpus is absent from this worktree, so there is no scoped
corpus before/after; behavior was instead verified with `ch` JS probes
(43-case behavior matrix + 24-case early-error matrix, all passing in all
three parse modes) and V8 cross-checks for `#prototype`/async/generator
legality. This slice implements `static #m(){}`,
`static get #p(){}` / `static set #p(v){}` in class declarations and class
expressions. Async/generator private methods (static and instance), static
blocks, computed names and decorators remain out of scope.

## Scope and observable semantics

- A static private method/accessor is own private storage of the class
  constructor. It is not a property: absent from `Object.keys`,
  `getOwnPropertyNames`, descriptors, `for-in` and proxies, and never on the
  prototype or on instances. Reads are `LdPrivateMethod`/`LdPrivateAccessor`
  with the constructor as receiver; writes to methods throw `TypeError`.
- The brand is the defining constructor. Instances, the prototype, other
  constructors and subclass constructors all fail the brand check
  (`TypeError` on access, `false` for `#m in obj`); only the defining
  constructor passes. A subclass that redeclares the name gets a fresh
  brand. Instance code reaches static names lexically (`C.#m`), never
  through instance brand installation.
- Definitions are created once per class evaluation and stored in the brand
  record; brand entries install on the constructor during member evaluation,
  before the inner binding initializes and before any static field
  initializer runs. A field declared before the method textually still
  observes it, and initializers may call static private methods.
- `static #prototype(){}` is legal: a private identifier is never a
  PropName, so the static `prototype` early error does not apply (matching
  instance `#prototype(){}` and V8). `static constructor(){}` stays legal;
  `#constructor` declaration and use stay early errors.
- Duplicate private names are early errors across static/instance and
  field/method/accessor kinds, including mixed static/instance get/set
  pairing. `get #x` + `set #x` pairing still shares one brand.
- `static async #m`, `static *#m`, `static async *#m` keep their explicit
  `SyntaxError`, mirroring the instance async/generator residual: the
  async/star modifiers are only recognized before public names, so those
  spellings fail in the member grammar. Enumerated precisely in Residuals.
- `static #x` ASI behavior is unchanged: `static #x` followed by a line
  terminator is a field even when the next element starts with `*` or
  `static`.

## Parser/AST design

- `PrivateNameEntry` gains `isStatic`, fixed at declaration. The direct
  `#name` path records the element's `isStatic`;
  `DeclareClassPrivateAccessor` takes `isStatic` and rejects
  static/instance mixing on get/set pairing as a duplicate-name error.
- The private-method path no longer rejects `isStatic`: static and instance
  methods share the method grammar (rewind + `ParseFncDecl`), hints
  (`#m`), and duplicate rules. `AddPrivateMethodInit` routes each entry by
  `isStatic` into the instance list or a new `pnodeStaticPrivateMethods`
  list; the shared `methodInitAdded` flag still dedupes get/set pairs
  (each entry is exclusively static or instance).
- The private-accessor path no longer rejects `isStatic` and marks
  `SetIsStaticMember(isStatic)`; async/generator accessors stay errors.
  The static-`prototype` early error is exempted for private identifiers.
- `ParseNodeClass` gains `pnodeClassStaticPrivateMethods`, populated at the
  end of `ParseClassDecl`. Fast-scan/deferred agreement is unchanged: list
  building is `buildAST`-gated and both scans share the error paths.

## Bytecode/runtime design

No new opcode, PropertyId, or runtime handler. The instance machinery is
receiver-agnostic, so it generalizes cleanly:

- `EmitPrivateMemberDefinition` still stores each definition in the brand
  record via `SetPrivateDefinition`; static members now pass the
  constructor (not the prototype) as the home object, so `super.x` in a
  static private method resolves against the base constructor, mirroring
  public static methods.
- A new class-emission step runs the static init list with
  `EmitClassPrivateMethodInits(..., pnodeClass->location)` after member
  evaluation and before the inner binding and static fields, so
  `InitPrivateMethod` brands the constructor exactly once per name.
- `LdPrivateMethod`, `LdPrivateAccessor`, `StPrivateAccessor`,
  `StPrivateMethod`, `HasPrivateField` behave as for instances with the
  constructor as receiver.

### Ownership/lifecycle record

- **Resource**: the existing one `JavascriptWeakMap` per declared private
  name per class evaluation, plus one weak entry keyed by the constructor
  per static method/accessor name. No new native allocation, no new opcode.
- **Owner**: the hidden block-scoped const owns the map; the map owns the
  definition; the constructor is the weak key. Class-block closure keeps
  the map alive for the class lifetime.
- **Invalidation**: none explicit. Static entries are weakly keyed by the
  constructor and reclaimed with it. No raw pointer or cross-context handle.
- **User-code/reentrancy**: definitions run no user code; brand installs
  run no user code; method/getter/setter bodies run arbitrary JS with a
  rooted receiver, as for instance members.
- **Failure**: wrong/primitive receiver, duplicate install, missing
  accessor half, and method writes throw `TypeError`. No partial native
  resource exists.
- **Thread/context**: current-thread script execution; no async job, TLS,
  callback or cross-thread access.

## Evidence

Build (Mac ARM64), all with local `.zig-cache`:

- `mise x -- zig build --cache-dir "$PWD/.zig-cache"` — green.
- `mise x -- zig build -Dprofile=debug-static --cache-dir "$PWD/.zig-cache"` — green.
- `mise x -- zig build verify-bytecode --cache-dir "$PWD/.zig-cache"` —
  green; checked-in nojit bytecode headers unchanged.

Local suites (`disable_jit`):

- `test/es7/class-private-methods.js` — **25/25** (18 existing + 7 new
  static cases) in normal, `-force:deferparse` and `-noDeferParse` modes
  on release and debug-static binaries.
- `test/es7/class-static-private-fields.js` — **13/13** (12 existing + 1
  interleave case) in the same three modes on both binaries.
- `test/es7/class-static-fields.js`, `test/es7/class-private-fields.js` —
  pass in all three modes (two stale static-rejection assertions updated
  to `doesNotThrow`).
- `python3 test/runtests.py -t -b "$PWD/zig-out/bin/ch" --variants
  disable_jit` — **2012 passed / 0 failed**; debug-static binary —
  **2012 passed / 0 failed**.
- JS probes (in `/tmp`, not committed): 43-assertion behavior matrix
  (dispatch, brand, accessors, `in`, reflection, freeze, per-evaluation
  brands, instance isolation, `super`, `#prototype`, class expressions,
  ASI) and 24-case early-error matrix (duplicates incl. mixed
  static/instance, `#constructor`, async/gen/static-block rejections) —
  all pass in all three parse modes. V8 (`node`) cross-checks confirm
  `static #prototype(){}` parses and async/generator statics are the only
  intentionally divergent forms.

## Residual work and limitations

- Async/generator private methods — `static async #m`, `static *#m`,
  `static async *#m` (and unchanged instance `async #m` / `*#m`) — remain
  `SyntaxError`. Supporting them needs new grammar (private name after the
  async modifier / star) for both static and instance paths; enabling it
  for statics alone would diverge from instances asymmetrically.
- Static blocks, computed class-element names and decorators are not
  implemented and remain `SyntaxError`.
- `super.#m` (private access through `super`) is untouched: `super.prop`
  handling is the known out-of-scope item from the sibling slices, so
  base-static-private access from a derived class goes through the
  defining class only, per the private-name environment.
- Direct eval cannot resolve private names (pre-existing residual,
  identical for instance members); anonymous classes declaring private
  members inside direct eval stay `SyntaxError` as before.
- Brands are `JavascriptWeakMap` entries rather than internal slots.
  `-CollectGarbage` was not exercised here (no test262 corpus in this
  worktree); GC-stress and weak-key lifetime are not separately covered.
- No JIT/backend support is claimed. **B4 must run the es7 private/static
  tests under SimpleJIT/FullJIT (dynapogo) before any JIT or default
  claim.**
- No new config flag or kill-switch: consistent with the sibling private
  slices, which likewise gate nothing.

## Files

- `lib/Parser/Parse.cpp`, `lib/Parser/Parse.h`: `isStatic` on
  `PrivateNameEntry`, accessor staticness check, static init-list routing,
  `static #prototype` exemption, removal of the two static rejections.
- `lib/Parser/ptree.h`, `lib/Parser/ptree.cpp`:
  `ParseNodeClass::pnodeClassStaticPrivateMethods`.
- `lib/Runtime/ByteCode/ByteCodeEmitter.cpp`: static home object for
  private definitions; constructor brand-install step in `EmitClass`.
- `test/es7/class-private-methods.js`: 7 new static cases; stale static
  rejections updated; async/generator statics asserted as errors.
- `test/es7/class-static-private-fields.js`: 1 new interleave case; stale
  method/accessor rejections updated.
- `test/es7/class-static-fields.js`: stale static-method rejection
  updated to `doesNotThrow`.
