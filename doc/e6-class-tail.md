# E6 — B5 lane: the class-tail engine-defect population

Status: **implemented on mainline `modernize/baseline`** (base `f085b4742`).
Scope: the C4 engine-defect bucket's "class residue ~84" cluster
(c4-manifest §2) — everything class-shaped that is *not* the nine
already-classified S1-tail residuals (4 post-ES2025 `accessor` grammar, 4
primitive-receiver BigInt-flag artifacts — now passing since BigInt is
default — and 1 Stage-1 nonextensible; those stay out of scope).

## 1. Triage (composition of "~84", extracted from the post-B4 wide run at base)

The certified cluster is exactly three tagged groups: `class` 76 +
`class-static-block` 6 + `class`-tagged-in-`expressions/super/` 2 = **84
edition-selected failures**. The same machinery also fails 14 untagged
`expressions/super/` scenarios (counted elsewhere in the engine-defect
bucket); this lane owns both. Oracle: Node v26.5.0 + the ES2025 spec
snapshot (read262 §10.2.2 [[Construct]], §13.3.7.1 SuperProperty,
sec-super-keyword, sec-addrestrictedfunctionproperties,
sec-class-definitions-static-semantics-early-errors).

| # | Family | Scenarios | Shape | Engine (before) | Node | ES2025 verdict |
|---|---|---:|---|---|---|---|
| 1 | legacy `caller`/`arguments` virtual own properties | 48 class dirs (+58 other fn-kind dirs, bonus) | `forbidden-ext/b2/*indirect-access-*` | sloppy fns expose non-configurable own `caller`/`arguments`; defineProperty throws; reads throw or return the strict caller | own props definable/readable; `hasOwnProperty` false | **Defect** — no function has own caller/arguments; poison accessors live on %FunctionPrototype% (sec-addrestrictedfunctionproperties); `[[GetOwnProperty]]` must see a missing property |
| 2 | derived-constructor return completion | 18 `subclass/derived-class-return-override-*` | `return`/`return 0` in try/catch/finally/for-of, incl. arrow variants | checks run *at the return statement*: "Use before declaration" before `finally { super() }` runs; `return 0` TypeError catchable and pre-empting iterator close | checks run at function exit | **Defect** — [[Construct]] steps 9-11 apply after the body completes |
| 3 | super evaluation order | 16 | `super[super()]` (2), `super(args)` with non-ctor proto (2), GetSuperBase-vs-ToPropertyKey ×4, uninitialized-this get/put ×8 | key expression evaluated before the this-binding check; base resolved after ToPropertyKey/RHS; IsConstructor TypeError before ArgumentListEvaluation | spec order | **Defect** — GetThisBinding before Expression; GetSuperBase captured at MakeSuperPropertyReference; ArgumentListEvaluation before the IsConstructor check |
| 4 | `await` in class static blocks | 6 | `static-init-await-binding` fn/gen (4), `static-init-invalid-await` label (2) | `(function await(await){})` rejected as keyword; `await: 0` label accepted | inverse | **Defect** — ContainsAwait stops at function boundaries; LabelIdentifier `await` inside the block is an early error |
| 5 | class-ctor own-property order | 4 | `fn-{length,name}-static-precedence-order` | own keys `prototype,length,name` | `length,name,prototype,method` | **Defect** — FunctionCreate/SetFunctionName run before MakeConstructor |
| 6 | anonymous class `name` | 2 | `expressions/class/name.js` | no own `name` on anonymous class values | own `name === ""` | **Defect** — ClassExpression evaluates ClassDefinitionEvaluation with className "" → SetFunctionName(F, "") |
| 7 | class-declaration completion | 2 | `cptn-decl.js` | `eval("1; class C {}")` → the class | `1`; lone decl → undefined | **Defect** — BindingClassDeclarationEvaluation returns NormalCompletion(empty) |
| 8 | `yield` newline `*` | 2 | `methods-gen-yield-star-after-newline` | parsed as multiplication `yield * 1` | SyntaxError | **Defect** — `[no LineTerminator here]` in YieldExpression; `*` cannot start a statement |
| 9 | default derived ctor spread | 2 | `default-constructor-spread-override` | synthesized `super(...args)` iterates via @@iterator | not iterated | **Defect** — the default ctor is a built-in forwarding the argument list; no iterator protocol |

Out of scope (verified unchanged): decorators (34), SharedArrayBuffer
subclass-builtins (4, disabled-flag bucket), `nonextensible-applies-to-private`
(2, Stage-1 post-ES2025).

## 2. Fixes

**(1) caller/arguments (48 + bonus).** `JavascriptFunction.cpp`: the
virtual own-property layer for legacy (sloppy script) functions is removed
from `HasPropertyQuery`, `GetAccessors`, `GetSetterBuiltIns`,
`IsConfigurable/IsEnumerable/IsWritable`, `GetSpecialPropertyCount` and both
`DeleteProperty` overloads. The legacy stack-walk *read*
(`GetPropertyBuiltIns` → `GetCallerProperty`/`GetArgumentsProperty`) and the
read-only *write* (`SetProperty`) are kept, so `arguments.callee.caller`
(10.6-13-a-2) and strict-receiver throws (13.2-*-s) behave exactly as
before; `%FunctionPrototype%`'s real %ThrowTypeError% accessors are
untouched (prop-desc.js pins them). `JavascriptOperators.cpp`
(`GetOwnPropertyDescriptor`): `caller`/`arguments` on a function with no real
own property now report missing before the receiver-level builtin read, so
gOPD/hasOwnProperty see `undefined`/false while ordinary reads still work.
`JavascriptGeneratorFunction.cpp` already routed around the layer; nothing
changed there.

**(2) Derived return checks at exit (18).** `ByteCodeEmitter.cpp`: the
inline object/undefined/TypeError/this-load sequence at each `return`
statement in derived constructors is replaced by one
`EmitDerivedClassConstructorReturnCheck` at the single exit label
(`knopEndCode`), after every finally region and iterator close handler has
run. The fall-through path loads `undefined` like an ordinary function; base
constructors keep the inline `return this` rewrite (`this` is bound at
entry, so no ordering is observable).

**(3) Super order (16).** New `EmitSuperPropertyReferenceOperands` emits
`super[Expression]` operand order as: this-binding check (scratch temp,
`ChkUndecl`) → super base load → key Expression, and (for the
reference/compound paths) captures GetSuperBase into a dedicated register
carried on the reference's `pnodeThis` node, so `EmitLoad`'s compound
machinery (null/undefined guard + one-time `Conv_Prop`) and
`EmitAssignment`'s store all reuse the same captured base — which also
fixes the compound load previously reading through the home object's
*current* prototype. The short-lived rvalue-load and call-target sites keep
the historical fresh-temp base emission (their base does not need to outlive
the key expression, and the call sequence's register shape is
load-bearing); only the operand *order* (check first) changed there. The
captured base never overwrites the super reference's own register, which
holds the shared home-object symbol value that later super references in
the same function reload through. Wired through `EmitReference`, the rvalue
`knopIndex` case and `EmitCallTarget`. `EmitSuperCall`: the early
`LdFuncObjProto` (IsConstructor-checking) that picks the
`this`-initialization strategy now uses the non-throwing `LdHomeObjProto`;
the call site's own `LdFuncObjProto` — emitted after the argument list —
still raises the TypeError, giving ArgumentListEvaluation-before-IsConstructor.

**(4) Static-block `await` (6).** `Parse.cpp` `ParseFncName`: the function
*expression* name rule now excludes `tkAWAIT` from the strict-mode
short-circuit, and a generator rejects only `yield` while an async function
rejects only `await` as its own name (declarations keep inheriting the
enclosing await region: `async function foo() { function await(){} }` and
module-level declarations stay SyntaxErrors). `ParseStatement`'s label
lookahead: a label named `await` whose enclosing function is a class static
block (node marker `isClassStaticBlock` or the scan flag) is an early
error.

**(5+6) Constructor shape (6).** `JavascriptLibrary.cpp`: the class
constructor type handler descriptor order is now `length, name, prototype`
(both the named and the previously 2-slot anonymous handler — anonymous
class values now always own `name`). `ScriptFunction.cpp`
`OP_NewClassConstructor` writes length→slot 0 and name→slot 1 (empty string
for anonymous/computed), `SetComputedNameVar`'s two class-constructor sites
write slot 1, `InitClassHelper` and the JIT's `LowerNewClassConstructor`
write the prototype into slot 2. NamedEvaluation (assignment/property
inference) still redefines `name` in place.

**(7) Completion value (2).** `ByteCodeGenerator.cpp`
`IsExpressionStatement`: a `knopClassDecl` with a declaration name is not an
expression statement, so `MustProduceValue` no longer selects it as the
last-value statement and eval returns the previous completion (or
undefined).

**(8) yield-newline-star (2).** `ParseExpr`'s yield handling: `yield` +
line terminator + `*` is an immediate SyntaxError (the token cannot start a
statement, so the multiplication-loop parse was the only wrong outcome).

**(9) Default ctor spread (2).** The parser marks the synthesized
`super(...args)` spread `fpnSyntheticNode`; `EmitOneArg` passes the rest
array itself (no `LdCustomSpreadIteratorList`) and `SpreadArgs` copies a
plain array's elements directly (GetSpreadSize already sizes arrays without
iterating). User-written `super(...args)` keeps the observable iteration.
The mechanism reuses the existing `CallIExtended_SpreadArgs` helper, so JIT
lowering is unchanged.

## 3. Ownership (memory-management.md read first)

No new heap resource, allocation domain, retained pointer, GC barrier or
memory-accounting change. (1) removes a virtual-property reporting layer and
adds one `HasProperty` probe on two property ids in gOPD; the legacy
stack-walk reads still box through the recycler exactly as before. (3) is
emitter register discipline: the captured super base lives in the super
reference node's own location (node lifetime, not a temp), and the scratch
this-check temp is acquired/released immediately (LIFO, debug-gate
asserted). (5) reorders a shared static type handler's descriptors and
updates the three slot-writing sites plus the JIT inlined store — no object
layout grows. (9) adds a parse-node flag and a plain-array element copy into
the pre-sized spread destination buffer. No parse-node mutation beyond the
one flag; no serialized-state change; reentrancy surface is unchanged
(GetSuperBase/stack-walk/SpreadArgs may run user getters exactly as the old
paths did, now in the specified order).

## 4. Before/after (pinned corpus `2e0a5676`, default host flags)

| Selection | Before | After | Flips |
|---|---|---|---|
| class dirs (statements/class + expressions/class, 16,689 scenarios) | 16,565 / **124** | **16,645 / 44** | 80 fail→pass, 0 pass→fail |
| class elements corpus (subset) | 5,892 / 5 | 5,892 / 5 | 0 / 0 (not regressed; residue = 4 accessor-grammar + 1 nonextensible, all post-ES2025-excluded) |
| expressions/super + statements/class/super (200) | 184 / 16 | **200 / 0** | 16 fail→pass |
| static-init await selection (71) | 65 / 6 | **71 / 0** | 6 fail→pass |
| caller-sensitive legacy selection (890) | 855 / 35 | 856 / 34 | 1 fail→pass, 0 pass→fail (residue pre-existing, other families) |
| Wide: expressions+statements+eval-code (39,757) | 38,750 / **1,007** | **38,906 / 851** | **156 fail→pass, 0 pass→fail** |
| built-ins sensitive surface (Function/Object/GeneratorFunction/ThrowTypeError/Reflect/Proxy, 8,682) | — | 2 fewer failures | 2 fail→pass, 0 pass→fail (both the `consistent-value-function-{caller,arguments}` invariant tests, bonus) |

The 156 wide flips: 80 class dirs + 16 super dir + 6 static-init await +
54 same-defect `forbidden-ext/b2` scenarios in non-class function-kind
directories (method-definition/async-function/async-generator/generators/
function/arrow-function/async-arrow-function — the same caller/arguments
root, counted elsewhere in the engine-defect bucket). Parse modes: the
class+super corpora re-run under `-force:deferparse` and `-noDeferParse`
(16,829 passed / 44 failed each, identical to default: 38 decorators + 4 SAB
+ 2 nonextensible); the focused rlexe file passes in default,
`-force:deferparse` and `-noDeferParse` on the test and debug-static
binaries.

JIT consistency spot-probes (interpreter vs `-mic:1 -bgjit-` vs
`-forceNative -off:simpleJit -bgJitDelay:0`, 100k-iteration warmup):
derived-exit return checks, captured-super-base compound assignment,
constructor own-key order, anonymous class name, default-ctor spread with
poisoned @@iterator, and eval class-declaration completion all hold in both
JIT tiers — the semantics live in the bytecode stream and shared runtime
helpers (the new spread path reuses the JIT-lowered `CallIExtended`
helper).

## 5. Gates

| Gate | Result |
|---|---|
| `zig build` (test profile) | green |
| `zig build check-regression` | **2045 / 0** (pin bumped 2042→2045; +3 rlexe entries for class-tail-semantics.js) |
| `zig build check` | green: regression 2045/0, `verify_bytecode_pure: source headers unchanged`, native `test-shared-basic` PASS, smoke PASS, metadata PASS |
| `zig build check-regression -Dprofile=debug-static` | **2097 / 0** (pin bumped 2094→2097; exercised the temp-LIFO asserts that caught two intermediate register-discipline defects, fixed before commit) |
| `zig build -Dprofile=jit check` | **PASS** (check_jit PASS, JIT bytecode up to date — required: `Lower.cpp` changed with the constructor slot reorder) |
| `python3 tools/build/check_regression.py --self-test` | all negative controls behave |

JIT consistency spot-probes (interpreter vs `-mic:1 -bgjit-` vs
`-forceNative -off:simpleJit -bgJitDelay:0`, 100k-iteration warmup):
derived-exit return checks, captured-super-base compound assignment,
constructor own-key order, anonymous class name, default-ctor spread with
poisoned @@iterator and eval class-declaration completion all hold in both
JIT tiers.

Legacy expectation corrections (engine now spec/Node-conformant; baselines
regenerated or assertions inverted to the ES2025 semantics this lane
implements): 31 debugger `.dbg.baseline` dumps (caller/arguments entries
gone from enumerations; ConsoleScope records the class-declaration empty
completion), `strict/19.function` + `strict/22.callerCalleeArguments`
baselines, `es6/ES6RestrictedProperties` (no virtual own restricted
properties; defineProperty allowed; deletes true), `es6/ES6Super`
(arguments evaluate before the IsConstructor TypeError), `es6/function.name`
(anonymous class owns name ""), `es6/generators-syntax` (yield-newline-star
is a SyntaxError), and the two S1 sites reading class-declaration values out
of eval.

## 6. Residual cards

- **CARD B5-1 — strict-caller legacy read is parse-mode-dependent.**
  Reading `sloppyFn.caller` when the caller is a *strict* function returns
  null in default-parse runs and throws the restricted-property TypeError
  under `-force:deferparse`/`-noDeferParse` (stack-walk finds a different
  frame shape). ES2025 removed the legacy accessor outright; V8 returns
  null. Not pinned by the corpus (both the b2 family and 13.2-* only pin
  receiver-strict throws); pre-existing; fix with any future stack-walk
  work.
- **CARD B5-2 — super-property getter/setter receivers (extends B4-4).**
  The index forms (`LdElemI_A`/`StElemI_A`) carry no this-override, so
  `super[k]` getters/setters and `super[k] = v` stores still run with the
  captured base as receiver (the B4-4 super-receiver card covers the dot
  forms and compound stores). Untested by the pinned corpus; unchanged by
  this lane.
- **CARD B5-3 — NewScObjectNoCtorFull before args for invalid super
  ctors.** With a non-constructor super constructor,
  `thisForSuperCall` is still allocated (reading `newTarget.prototype`)
  before the argument list; a Proxy/getter `newTarget.prototype` would be
  observable ahead of the arguments. Only reachable via Reflect.construct
  with an exotic newTarget plus an invalid super constructor; untested by
  the corpus.
- Pre-existing (inherited, unchanged): plain-function own-key order is
  `prototype,length,name` (spec: `length,name,prototype`) and anonymous
  function *expression* values still lack own `name` — the
  `expressions/function/name.js` / `arrow-function/name.js` /
  `ThrowTypeError/*` families outside the class lane.

## 7. Expected C4 delta

Edition-selected failures 4,080 − 112 (B4's recorded −112 expectation,
already landed) − **100 this lane** (84 class/class-static-block/call-proto
tagged + 12 untagged super-dir + 4 static-init-await in fn/gen dirs) = expected ≈ **3,968** once B4's and this
lane's deltas are both re-measured. The 58 bonus non-class forbidden-ext
flips sit in the same engine-defect bucket and reduce it further; the next
C4 measurement should confirm the totals with zero regressions.

## 8. Files

- `lib/Runtime/Library/JavascriptFunction.cpp` — virtual caller/arguments
  layer removal; SpreadArgs direct-array branch.
- `lib/Runtime/Language/JavascriptOperators.cpp` — gOPD missing-property
  guard for function caller/arguments; the SetProperty_Internal legacy
  read-only early-out (sloppy `f.caller = v` stays a silent no-op instead of
  reaching %FunctionPrototype%'s throwing setter).
- `lib/Runtime/ByteCode/FuncInfo.cpp` — ReleaseReference releases the
  super-element captured-base carriage in temp-LIFO order.
- `lib/Runtime/ByteCode/ByteCodeEmitter.cpp` — derived-ctor exit check,
  EmitSuperPropertyReferenceOperands + consumer rewiring, EmitSuperCall
  non-throwing base load, synthetic spread arg.
- `lib/Runtime/ByteCode/ByteCodeGenerator.{h,cpp}` —
  EmitDerivedClassConstructorReturnCheck; class-decl completion.
- `lib/Runtime/Library/{JavascriptLibrary,ScriptFunction}.cpp`,
  `lib/Runtime/Language/InterpreterStackFrame.cpp`,
  `lib/Backend/Lower.cpp` — constructor shape reorder + anonymous name.
- `lib/Parser/Parse.cpp` — ParseFncName await-expression rule, static-block
  await labels, yield-newline-star, default-ctor spread flag.
- `test/es7/class-tail-semantics.js` (new; 3 rlexe entries),
  `test/es7/rlexe.xml`, `tools/build/gate_counts.json`
  (2042/2094 → 2045/2097).
- Test-expectation corrections (engine now spec/Node-conformant):
  `test/es7/class-private-methods.js` and
  `test/es7/class-static-private-fields.js` no longer read the value of a
  class *declaration* out of `eval` (class expressions instead).
