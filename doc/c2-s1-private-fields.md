# C2-S1: private instance fields and `#x in obj` brand checks

Status: **landed in `da0351e8e`** (feature fixes `047f877c1`, `1f62ac060`; base `1927b9493`); current integrated gates **2004/2056**. Evidence: focused private subset 176/30, **160 fail→pass / 0 pass→fail**; es7 64/0. Private methods/accessors/static/computed/direct-eval/optional-chain remain residuals; JIT/B4 gated.

This slice implements non-static **private instance fields** (`#x;`,
`#x = init;`) and the `#x in obj` private-brand check in class declarations and
class expressions. Private methods/accessors, static and private-static fields,
static blocks, computed names and decorators remain out of scope.

## Scope and observable semantics

- A declared private name is lexically scoped to the class body, including
  method/field-initializer bodies and nested classes. Nested classes may read
  outer private names and may shadow them with their own declarations.
- Uses may precede declarations. Duplicate private names in one class,
  `#constructor` (declaration or use), use of an undeclared private name, and
  private references outside a class are `SyntaxError`s.
- Each class **evaluation** creates a new brand. Two evaluations of the same
  class source therefore produce instances that are not mutually branded.
- Private state is per instance and is not a property: it is absent from
  `Object.keys`, `getOwnPropertyNames`, `getOwnPropertySymbols`, proxies see no
  private key, and inherited private state is not visible (`#x in
  Object.create(instance)` is `false`).
- Get/set and compound/update forms brand-check the receiver and throw
  `TypeError` for a wrong receiver, including primitives and proxies. A
  primitive right-hand side of `#x in` also throws `TypeError`; an unbranded
  object returns `false`.
- A missing initializer defines `undefined`. Private and public fields run in
  a single textual declaration order. A private field add throws `TypeError`
  if the receiver is non-extensible or is already branded by that same class
  evaluation (`PrivateFieldAdd`), while reading/writing an existing private
  field after `preventExtensions` still works.
- Base fields run before the constructor body; derived fields run immediately
  after `super()` binds `this`. Initializers evaluate with `this` bound to the
  instance. Anonymous function/arrow/class initializers receive the private
  source name including `#` (`#foo`, `#bar`, `#Baz`). Public fields/methods are
  unaffected.

## Parser/AST design

`tkPrivateName` is a scanner token whose token range includes `#` while the
interned identifier excludes it. It is accepted as a field name and after `.`;
the `[In]` grammar parameter is threaded to `#x in` so `for (#x in ...)` is
rejected. A parenthesized expression is a valid ShiftExpression right operand,
so `#x in (o in p)`, `#x in ((o in p))` and `#x in (o in p) in o` are accepted
(as is left-associative `#x in o in p`); only a bare PrivateIdentifier right
operand such as `#x in #x in obj` is an early error.

Private-name validation also runs with `buildAST == false`. The parser pushes
the same lexical `PrivateNameScope` during fast scan, records declarations and
deferred references without AST/brand nodes, and resolves them at the outermost
class pop. Consequently a deferred or large function body containing
`this.#x`, duplicate private fields in nested classes, or an undeclared private
name reports `SyntaxError` during the initial parse instead of failing later
when the body is reparsed or called.

Each class push creates a `PrivateNameScope`. In an AST parse, field
declarations add a hidden block-scoped `const` (`*s1privateN*`) plus a
`PrivateNameEntry`; private references are recorded in a deferred list and
resolved when the outermost class scope pops, which permits forward uses and
implements nested-class shadowing. A reference becomes a `ParseNodeName` marked
`isPrivateName` whose `sym` is the hidden const. `BindInstAndMember` must not
treat this name as a property reference. `ParseNodeClass::pnodePrivateNames`
retains the hidden declaration list for emission.

Two parser edge cases are guarded explicitly: the "function assignment"
marker is only applied to real assignments whose left operand is an ordinary
name (not a private reference), so `#x in function(){}` cannot dereference a
missing `PidRefStack`; and `AttachClassFieldsToLambda` clones a private field
name as another private `knopName` retaining its brand symbol instead of
casting it to `ParseNodeStr`. Arrow-`super()` field calls therefore use
`InitPrivateField` and do not create a public property.

`ClassBodyHasPrivateSyntax` is a conservative look-ahead used to keep nested
functions eager when a class may contain private syntax. It bails out as
"private" on context-sensitive tokens (template pieces, regex, division,
scan errors), trading some deferral for correctness; this is recorded as a
performance residual, not an observable semantic.

No static PropertyIds were added. The hidden const uses the existing dynamic
synthetic-name path from public class fields.

## Runtime/brand design

`NewPrivateName` creates a fresh hidden `JavascriptWeakMap` per private-name
declaration at class-definition evaluation and stores it in the hidden class
const. Each private field is an entry in that map keyed by the instance:
private state is off the object's property list, obeys weak key lifetime, works
on frozen instances after initialization and gives per-evaluation identity.

Bytecode operations are appended after `InitClassField`, so existing opcode
values do not shift:

- `NewPrivateName` — create the hidden brand map.
- `LdPrivateField` — brand-check and load.
- `StPrivateField` — brand-check and store.
- `InitPrivateField` — `PrivateFieldAdd` (extensible and not already present).
- `HasPrivateField` — `#x in obj` own-brand test; non-object throws.

Reads/writes/calls/`typeof`/compound operations on `a.#x` are recognized in the
emitter by the private member node; the class-block brand symbol is loaded
through the normal closure/slot path. Field initializer calls pass the brand
alongside the instance.

### Ownership/lifecycle record

- **Resource**: one `JavascriptWeakMap` per private name per class evaluation,
  plus the recycled map entries for instance keys.
- **Owner**: the hidden block-scoped const in the class block; methods and
  field-initializer helpers reach it through existing closure slots.
- **Invalidation**: no explicit invalidation. Map entries are weakly keyed by
  the instance and disappear with the key through existing `JavascriptWeakMap`
  machinery; no raw/native pointer is retained.
- **User-code/reentrancy**: field initializers, getters and proxies can run
  arbitrary JS during construction/access. The instance is an ordinary rooted
  `this`; map operations allocate only through the recycler. Abrupt completion
  propagates before later fields/stores.
- **Failure**: wrong receiver, primitive receiver, duplicate add or
  non-extensible add throws `TypeError`; no partial native resource exists.
  Private definitions already completed on an abandoned instance remain
  ordinary weak entries.
- **Thread/context**: normal current-thread script execution; no new async
  job, TLS, callback or cross-thread access.

## Evidence

Build (Mac ARM64, `-nonative` interpreter evidence):

- `sh /tmp/opencode/cc-gate-lock.sh mise exec -- zig build --cache-dir "$PWD/.zig-cache"` — green.
- `... zig build -Dprofile=debug-static ...` — green.
- `... zig build verify-bytecode ...` — green; checked-in bytecode unchanged.
- Release `zig-out/bin/ch` sha256 `8be2157112dd5b39c25c46616de2fc1101d78fb69dc2b38983ca4667840b606c`;
  `libChakraCore.dylib` sha256 `4bcccad4be9b9103d041092d8e42760597ffc0b8a72e7eb492849b1d5279a644`;
  Debug `out/debug-static/bin/ch` sha256 `6880da1ce77071815cf1766ea5d41c2dae702c928607a3c162b7ed7fd7112aad`.

Local suites (disable_jit):

- `test/es7/class-private-fields.js` — pass in normal, `-force:deferparse`,
  `-noDeferParse`, and `-force:DeferParse -UseParserStateCache
  -ParserStateCache` modes (Release and Debug). The file includes deferred fast
  scan cases for `this.#x`, duplicate private fields, nested duplicate classes,
  `Function("return this.#x")`, and a 300-statement prefix; parenthesized RHS
  forms and `#foo`/`#bar`/`#Baz` NamedEvaluation; `#x in` with function,
  arrow, generator, async-function, class and object values (no crash, all
  `false`); and arrow-`super()` private initialization (only public `b` is
  enumerable, `d.x` is `undefined`, `getX()` is `42`).
- `test/es7` — **64 passed / 0 failed** (three `class-private-fields.js`
  entries plus the existing suite).
- `test/es7/class-fields-public.js` — 27/27 (public-field regression control).
- `test/es6` — 233/0; `test/es6module` — 31/0; `test/Function` — 70/0.

Focused test262 subset (pinned corpus, exact command from
`out/test262-s1-private-base/summary.json`; patterns cover
`privatefield*`, `private-field-*`, private-field-usage, private-in, and the two
nonextensible files; 206 scenarios / 104 files):

| run | passed | failed |
|---|---:|---:|
| base binary before S1 private work | 16 | 190 |
| S1 private fixed binary | **176** | **30** |

Scenario diff: **160 fail -> pass, 0 pass -> fail**. Filtering to scenarios
tagged `class-fields-private`/`class-fields-private-in` without excluded
method/static/computed/decorator features gives 198 scenarios, 16 -> 176
passed, 160 fail -> pass, 0 pass -> fail.

## Residual work and limitations

- Private methods/accessors, static/private-static fields, static blocks,
  computed names and decorators are excluded by design. The two
  nonextensible-applies-to-private files still fail overall because they also
  contain private methods/accessors/static fields; their field-only assertions
  pass.
- Direct eval inside methods/initializers cannot see the class private
  environment; private identifiers in eval code remain `SyntaxError`
  (`private-field-visible-to-direct-eval*`).
- Optional-chain private tails (`o?.c.#f`) are not implemented
  (`private-field-after-optional-chain`).
- Private members as destructuring assignment targets are not implemented
  (`privatefieldset-evaluation-order-1` expects a ReferenceError before a
  getter).
- The primitive-receiver get/put tests are blocked by the existing missing
  BigInt literal support (`15n`), not by private-field semantics.
- Computed public names are out of scope, so the computed-property private
  interaction tests remain failing. Public/private mixed field initialization
  is declaration-ordered, but computed-name TDZ interactions are not exercised
  and remain a residual risk.
- Parser look-ahead conservatively treats classes with context-sensitive tokens
  (regex/division/templates) as private-syntax classes, keeping more nested
  functions eager than strictly necessary.
- Private values depend on the existing `JavascriptWeakMap` weak-key storage
  (`WeakMapKeyMap` on the instance). Focused test262 runs with `-CollectGarbage`
  and Debug runs are green; clone/marshal across script contexts is untested.
- JIT/backend support is unverified. All five opcodes are interpreter-only and
  have no JIT helpers; **B4 must run the S1 private-field corpus and the es7
  tests under SimpleJIT/FullJIT (dynapogo) before any JIT/default claim**.

## Files

- `lib/Parser/tokens.h`, `lib/Parser/Scan.h`, `lib/Parser/Scan.cpp`:
  PrivateIdentifier token.
- `lib/Parser/ptree.h`, `lib/Parser/ptree.cpp`: private-name flag and class
  private-name list.
- `lib/Parser/Parse.h`, `lib/Parser/Parse.cpp`: lexical private scopes,
  deferred resolution, grammar/early errors, hidden brand declarations,
  `[In]` handling.
- `lib/Runtime/ByteCode/OpCodes.h`: appended private-field operations.
- `lib/Runtime/ByteCode/ByteCodeGenerator.cpp`: binding/visit handling.
- `lib/Runtime/ByteCode/ByteCodeEmitter.cpp`: brand creation, field
  initialization, member load/store/call/`typeof`/`in` emission.
- `lib/Runtime/Language/InterpreterHandler.inl`,
  `InterpreterStackFrame.{h,cpp}`: interpreter handlers.
- `lib/Runtime/Language/JavascriptOperators.{h,cpp}`: WeakMap-backed brand/
  value operations and `PrivateFieldAdd` checks.
- `lib/Parser/rterrors.h`: private-brand `TypeError`.
- `test/es7/class-private-fields.js`, `test/es7/rlexe.xml`: focused regression
  test and one registered entry.
