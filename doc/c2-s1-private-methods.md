# C2-S1: private instance methods and private accessors

Status: **pending** (branch `s1-private-methods`, base `7a2199595`). This slice
adds non-static private instance methods (`#m(){}`) and private accessors
(`get #x(){}` / `set #x(v){}`). Private static members, static blocks, computed
class-element names, async/generator private methods, decorators, optional-chain
private tails and direct eval of private names remain out of scope. No shared
plan/triage or gate pins are changed.

## Scope and observable semantics

- Private method/accessor names use the same class-lexical private-name
  environment as private fields: forward references, nested-class shadowing,
  undeclared/out-of-class early errors, duplicate detection with fields and
  methods, and `#constructor` rejection.
- A private method is a single function created once per class evaluation and
  shared by every instance. It is not an own property of the instance, the
  prototype or the constructor, and does not appear in any reflection surface.
- `this.#m` reads the shared function; `this.#m()` dispatches with the receiver
  as `this`. Calling the function value with another receiver is allowed (the
  brand check happens in `this.#m`, not in the call itself). Writing a private
  method (`this.#m = v`) throws `TypeError`.
- Get/set accessors share one private name/brand. Reads invoke the getter with
  the receiver as `this`; writes invoke the setter. A missing getter/setter
  throws `TypeError`. `#m in obj` and `#x in obj` are own-brand checks; a
  primitive receiver throws `TypeError`, an unbranded object returns `false`,
  and `Object.create(instance)` is not branded.
- Private methods/accessors are installed before any field initializer, even
  when declared textually after the field, matching `InitializeInstanceElements`.
  They are also installed for derived constructors immediately after `super()`
  binds `this`, and for super calls inside arrow functions. A second
  installation on the same receiver throws `TypeError` (double initialisation).
- Installing a method/accessor brand on a non-extensible receiver is allowed;
  a following field add fails with `TypeError`. Frozen/non-extensible instances
  may still call methods and access accessors after construction.
- Distinct class evaluations have distinct brands (and distinct method
  functions). Base-class private members are visible from derived instances
  because the base constructor installs its brand on the receiver.

## Parser/AST design

`PrivateNameEntry` now carries a `PrivateNameKind` (field/method/accessor),
get/set presence and a "brand init already queued" flag. Accessor declarations
use a dedicated declaration path so `get #x` + `set #x` reuse one entry, while
duplicate getters, duplicate setters, field/method collisions and method
collisions remain `SyntaxError`s. Private method/accessor names are parsed by
the existing class-member grammar; `ParseFncDeclNoCheckScope` consumes the
private token as the suppressed function name, and the parser rewinds to the
token so the ordinary method/accessor path can run. Function hints use the
source spelling including `#` (so `new C().ref().name === "#m"`).

Resolved private reference `ParseNodeName`s record the entry kind at the
outermost class pop. This lets the emitter select field, method or accessor
dispatch without new static PropertyIds. The class constructor carries
`pnodeClassPrivateMethods`, an ordered list of brand references; the existing
field-initializer list stays separate so methods always run first. The list is
cloned for arrow super lambdas the same way field calls are.

## Runtime/brand design

The per-name `JavascriptWeakMap` brand record from the private-fields slice is
reused. Its self entry (`map[map]`) holds the class-evaluation definition:
the shared method function, or a hidden null-prototype accessor record with
own `get`/`set` data properties. Each instance gets one brand entry whose value
is `undefined`; the definition is never copied per instance.

Appended interpreter-only opcodes (after `HasPrivateField`):

- `SetPrivateDefinition` — store the method function or accessor half in the
  brand record self entry.
- `InitPrivateMethod` — `PrivateMethodOrAccessorAdd`: duplicate check, no
  extensibility requirement, install the per-instance brand entry.
- `LdPrivateMethod` — brand check and return the shared method function.
- `LdPrivateAccessor` — brand check, retrieve getter, `CallGetter` with the
  receiver, `TypeError` if absent.
- `StPrivateAccessor` — brand check, retrieve setter, `CallSetter`, `TypeError`
  if absent.
- `StPrivateMethod` — brand check, then `TypeError` (methods are not writable).

No static private PropertyIds were added.

### Ownership/lifecycle record

- **Resource**: the existing one `JavascriptWeakMap` per declared private name
  per class evaluation, plus one hidden null-prototype accessor record for an
  accessor name (stored as the map's self value), plus weak per-instance keys.
- **Owner**: the hidden class-block const; methods and instance initializers
  reach it through existing closure slots.
- **Invalidation**: no explicit invalidation; instance entries are weak keys and
  disappear with the instance through the existing `JavascriptWeakMap`
  machinery. Method/accessor definitions are held by the map self entry, whose
  key is weakly held but whose value is reachable from the map while the class
  binding is alive.
- **User-code/reentrancy**: getters/setters and method calls run arbitrary JS.
  The receiver is a rooted `this`; reads/writes complete or propagate abrupt
  completion before later initialization. Accessor records are null-prototype
  objects, so `Object.prototype.get`/`set` cannot affect dispatch.
- **Failure**: wrong/primitive receiver, duplicate method installation, missing
  accessor half, and method writes throw `TypeError`. No partial native resource
  exists; completed brand entries remain ordinary weak entries.
- **Thread/context**: current-thread script execution; no async job, TLS,
  callback or cross-thread access.

## Evidence

Build (Mac ARM64 release/no-JIT; all commands under the serialized gate lock):

- `sh /tmp/opencode/cc-gate-lock.sh mise exec -- zig build --cache-dir "$PWD/.zig-cache"` — green.
- `sh /tmp/opencode/cc-gate-lock.sh mise exec -- zig build verify-bytecode --cache-dir "$PWD/.zig-cache"` — green; checked-in bytecode unchanged.
- `sh /tmp/opencode/cc-gate-lock.sh mise exec -- zig build -Dprofile=debug-static --cache-dir "$PWD/.zig-cache"` — green.
- Release `zig-out/bin/ch` sha256 `bb2b06bd95337cc22b66b5510e9770a4030b2f5877261a015c8e7b750ad8eca6`
  (unchanged launcher; shared build), `zig-out/bin/libChakraCore.dylib` sha256
  `18f4e08a957b172fb99b15dcb58aaa24651e4b510e304a5f184dbc7fc4afbc1b`.
  The pre-slice base dylib was `0568fb41ecea5159eaf17cd1fe86aa7d94de7ddc642b9201df828a22d3723175`.
  Debug-static `out/debug-static/bin/ch` sha256
  `b4de223c6366c6cea8ecb583e185d833a78b0abe8fba2e4830bff66ecbf12742` (static).

Local suites (disable_jit, release binary):

- `test/es7/class-private-methods.js` — 17/17 cases pass in normal,
  `-force:deferparse` and `-noDeferParse` modes. Cases cover method dispatch,
  shared function identity/name/length, reflection, wrong receivers, get/set
  accessors and missing halves, derived/base brands, initialization order,
  `#m in obj`, frozen/non-extensible receivers, per-evaluation brands, nested
  shadowing, arrow-`super()` cloning, method write rejection, and early errors.
- Debug-static build passes the same test file in normal, `-force:deferparse`,
  `-noDeferParse`, and `-force:DeferParse -UseParserStateCache
  -ParserStateCache` modes; the debug build also exercises the private-accessor
  `knopGetMember`/`knopSetMember` AST assertion. `class-private-fields.js`
  remains green.
- `test/es7/class-private-fields.js` — still passes in all three release modes.
- `python3 test/runtests.py -t -b "$PWD/zig-out/bin/ch" --variants disable_jit`
  — **2003 passed / 0 failed** across the checked-in suite, including
  `es7: 68/0`, `es6: 233/0`, `es6module: 31/0`.

Focused test262 subset (pinned corpus
`2e0a56762801e275a9fdf96dc49d90ba0cddcf63`; same command and filters as the
private-fields slice, with method/accessor/inner-function/nested/shadowing
patterns; 222 scenarios / 111 files):

| run | passed | failed |
|---|---:|---:|
| base binary before this slice | 4 | 218 |
| S1 private-method/accessor binary | **202** | **20** |

Scenario diff: **198 fail -> pass, 0 pass -> fail**. Filtering out the 10 files
that require direct eval or computed public field names leaves **202/202**
passing. Reported `class-methods-private` feature counts are 198 passed / 20
failed. Residual failures are exactly:

- `private-{method,getter,setter}-visible-to-direct-eval{,-on-initializer}` (6
  files x 2 scenarios): direct eval cannot see the class private environment.
- `private-{method,getter,setter}-is-not-clobbered-by-computed-property` and
  `private-method-is-visible-in-computed-properties` (4 files x 2 scenarios):
  computed public field names remain outside the S1 field slice.

## Residual work and limitations

- Private static fields/methods, static blocks, computed class-element names
  and decorators are excluded by design.
- Async/generator private instance methods are not implemented in this slice
  (valid syntax is reported as `SyntaxError` until follow-up); accessors cannot
  be async/generator.
- Direct eval inside methods/initializers cannot see the class private
  environment, as with private fields.
- Optional-chain private tails (`o?.#m`) remain unsupported.
- Destructuring assignment targets containing private methods/accessors remain
  unsupported (private destructuring targets were already a field residual).
- The `JavascriptWeakMap` self entry relies on WeakMap keys being objects and on
  the existing weak-key storage; clone/marshal across script contexts is
  untested.
- All new opcodes are interpreter-only. **B4 must run this corpus and the es7
  tests under SimpleJIT/FullJIT (dynapogo) before any JIT/default claim.**

## Files

- `lib/Parser/ptree.h`, `lib/Parser/ptree.cpp`: private reference kind and the
  constructor private-method brand list.
- `lib/Parser/Parse.h`, `lib/Parser/Parse.cpp`: accessor/method declaration
  handling, duplicate rules, private-method brand list, arrow-super cloning.
- `lib/Parser/rterrors.h`: method-write and missing-accessor `TypeError`s.
- `lib/Runtime/ByteCode/OpCodes.h`, `ByteCodeEmitter.cpp`,
  `ByteCodeGenerator.h`, `ByteCodeDumper.cpp`: append-only private method/
  accessor opcodes and emission.
- `lib/Runtime/Language/InterpreterHandler.inl`,
  `InterpreterStackFrame.{h,cpp}`: interpreter handlers.
- `lib/Runtime/Language/JavascriptOperators.{h,cpp}`: brand-record definition
  storage, method/accessor dispatch and accessor calling.
- `test/es7/class-private-methods.js`, `test/es7/rlexe.xml`: focused tests and
  three registered modes.
