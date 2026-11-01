# C2-S14 residual: body eval with non-simple parameters

Status: **fixed on this branch, including the review repair** (catch-pattern
bindings in the eval preflight, parameter named `arguments`; diagnosis
`47a357092`; fix/tests/pins `c1b769e1f`; repair + receipt commits follow).
Mac interpreter/no-JIT (test + debug-static profiles); JIT out of scope for
this lane.

Tracked P4 residual from `doc/c2-s14-global-eval-declarations.md`: a sloppy
direct eval in the body of a function with a non-simple parameter list
over-reports `SyntaxError` for `eval('var <param>')` and for eval'd function
declarations whose name collides with a parameter.

## Spec analysis

ES2025 10.2.11 FunctionDeclarationInstantiation: when the parameter list is
non-simple (`hasParameterExpressions`), the body gets a **new declarative
environment** (FDI step 27) which becomes the running context's lexical *and*
variable environment. Parameter bindings live in the *outer* (parameter)
environment — outside the function's var environment.

ES2025 19.2.1.3 EvalDeclarationInstantiation step 3.b (sloppy, non-global
varEnv): the preflight walk starts at `lexEnv` and checks `HasBinding(name)`
for each var name **while `thisEnv` is not `varEnv`** — only intermediate
(lexical) environments are checked; the walk terminates at the var
environment. Parameters are outside the var environment, so a var colliding
with a non-simple parameter is **not** a SyntaxError; steps 14–15 create the
binding in `varEnv` (the body environment): `CreateMutableBinding` +
`InitializeBinding(undefined)` when the body env has no such binding, nothing
when it does (e.g. a body `var` of the same name, which FDI initialized from
the parameter value). A body read of the name then resolves to the body-env
var (shadowing the parameter), while parameter-list closures still see the
parameter binding.

With a **simple** parameter list there is one environment: parameters and
vars share the var environment, the walk is empty (or stops immediately), and
`eval('var a = 7')` assigns the existing parameter binding (no throw).

## Scenario evidence (before → after fix)

Oracle: Node v26.5.0. `ch` = main `zig-out/bin/ch -nonative` at `3a6d245de`
for "before", the worktree binary for "after". `⇒` marks the cell that
changed. Post-eval `return a` values follow the spec: the eval var binding
lives in the body (variable) environment, so body reads resolve to it
(`undefined` without initializer — the parameter binding is untouched and
still observed by parameter-list closures).

| # | Scenario | Node | ch before | ch after |
|---|---|---|---|---|
| f1 | `function f1(a = 1) { eval('var a'); return a; } f1()` | `undefined` | SyntaxError | ⇒ `undefined` |
| f2 | `function f2(a = 1, ...rest) { eval('var a = 2'); return [a, rest.length]; } f2()` | `[2, 0]` | SyntaxError | ⇒ `[2, 0]` |
| f2b | `function (a = 1, ...rest) { eval('var rest = 4'); return [rest, a]; }` | `[4, 1]` | SyntaxError | ⇒ `[4, 1]` |
| f3 | `function f3({a} = {}) { eval('var a'); return a; } f3()` | `undefined` | SyntaxError | ⇒ `undefined` |
| f3b | `function f3b({a} = {}) { eval('var a = 9'); return a; } f3b()` | `9` | SyntaxError | ⇒ `9` |
| f4 | `function f4({a} = {}) { eval('var b = 3'); return [a, b]; } f4()` | `[null, 3]` | `[null, 3]` | `[null, 3]` |
| f5 | `function f5(a = 1) { eval('function a(){}'); return typeof a; } f5()` | `"function"` | SyntaxError | ⇒ `"function"` |
| f5b | `function f5b(a = 1) { eval('function a(){return 42}'); return a(); } f5b()` | `42` | SyntaxError | ⇒ `42` |
| n4 | `function n4(a = 1) { eval('var a'); return [a, (()=>a)()]; } n4()` | `[null, null]` | SyntaxError | ⇒ `[null, null]` |
| n6 | `eval('var a = 2'); eval('var a'); return a` | `2` | SyntaxError | ⇒ `2` |
| n7 | `function n7(cb = () => a, a = 1) { eval('var a'); return [a, cb()]; } n7()` | `[null, 1]` | `[null, 1]` | `[null, 1]` |
| q6h | `eval('var a'); eval('a = 6'); return a` | `6` | SyntaxError | ⇒ `6` |
| g3 | `function (a = 1) { eval('var arguments'); return typeof arguments; }` | `"undefined"` | `"object"` | ⇒ `"undefined"` |
| s1 | `function s1(a) { eval('var a = 7'); return a; } s1()` (simple param) | `7` | `7` | `7` |
| s1b | `function (a) { eval('function a(){}'); return typeof a; }` (simple) | `"function"` | `"function"` | `"function"` |
| h1 | `function (a = 1) { let x; eval('var x'); }` (body lexical collision) | SyntaxError | SyntaxError | SyntaxError |
| h2 | outer `let` + body `eval('var outer')` (walk stops at varEnv) | no-throw, shadowed | no-throw, shadowed | no-throw, shadowed |
| b1 | `function b1(a = 1) { var a = 5; return a; } b1()` (no eval) | `5` | `5` | `5` |
| m1-m3 | formals eval (`a = eval('')`) + body var/fn collisions + eval var | `5`/`5`/`3` | same | same |
| p2-p4 | `eval('')` + body var/fn of param name (no eval decl) | `5`/`5`/`"function"` | same | same |
| p5/p9-class | body var/fn of param name + eval decl of same name, merged shape | `5`/`9` | SyntaxError | SyntaxError (residual) |

Already-split controls (n7, q3, a4/a5/a7-a10, q6d-q6i in the probe set)
match Node exactly before and after, including `undefined` for a
no-initializer eval var and the parameter binding remaining untouched for
parameter-list closures.

## Root cause

The eval declaration preflight (`OP_ScopedEnsureNoRedeclProperty`,
`lib/Runtime/Language/JavascriptOperators.cpp:8581`) walks the eval's frame
display and stops at the first plain `ActivationObject`
(`ActivationObject::EnsureNoRedeclPropertyForEval` returns TRUE — "the
variable environment for the checked declaration"). That contract is correct
*provided* the body of a non-simple-parameter function keeps a scope object
of its own. The engine usually creates one: `StartBindFunction`
(`lib/Runtime/ByteCode/ByteCodeGenerator.cpp:1657-1678`) marks both the body
and param scopes as objects when the function calls eval, and the parser even
forces the param/body scope split when eval is detected while parsing the
*formals* (`lib/Parser/Parse.cpp:8455-8459`).

The gap is the split decision itself (`lib/Parser/Parse.cpp:7212-7231`):
param/body scopes are split only when a parameter has a non-local reference
(a parameter-list closure) or eval was seen during formals parsing. A body
`eval(...)` is parsed **after** that decision, so a function like
`function f1(a = 1) { eval('var a'); return a; }` keeps its param and body
scopes **merged**: the single function activation object holds the parameter
binding, becomes both the parameter environment and the (only candidate) var
environment, and the preflight observes the parameter binding — reporting
`SyntaxError: Let/Const redeclaration`. Eval'd non-colliding vars in the same
shape land in that merged (parameter) environment (probe q1: a parameter
closure can see `eval('var zzz = 3')`, Node cannot).

With the split (as in n7/q3 today), the body activation object fronts the
frame display: the preflight walk stops at it (no parameter collision), the
var/function creation ops (`OP_LoadUndefinedToElementScoped`,
`OP_InitFuncScoped`) create the binding in the body environment, dynamic body
reads see it, and parameter closures keep reading the parameter scope.

## Fix design

Force the param/body scope split at parse time when the function has a
non-simple parameter list and its body (as parsed or deferred-scanned) calls
eval — the same treatment `Parse.cpp` (`ParseFncFormals`) already applies to
eval seen during formals parsing. The flip runs in `ParseFncDeclHelper` after
the body is parsed and before the body block's `FinishParseBlock`, guarded by
`Parser::BodyRedeclaresParameter`: the split may only be forced when no
body-level var/function declaration reuses a parameter symbol. Under the
merged sharing such a declaration keeps the parameter symbol as its storage,
and that storage is what a later eval declaration would have to reuse
(19.2.1.3 step 15.b: an existing varEnv binding is not re-initialized); a
post-hoc split would strand the body declaration's value on the parameter
environment while the eval creates a fresh shadow. Compound redeclarations
(body var/function of a parameter name plus an eval declaration of the same
name in a function whose scopes were parse-time merged) therefore keep the
merged behavior; the remaining over-report there is the residual below. When
the scopes split — for parameter closures and formals-scope eval today, for
body eval after this change — the pre-existing emission is unmodified and
already conforming.

This is a parser flag decision plus a read-only symbol-list scan; it changes
no allocation, ownership, retained pointer, callback, GC barrier or memory
accounting (per repo `AGENTS.md`, `doc/memory-management.md` was not
re-consulted beyond confirming the change is outside its scope: one boolean
on the parse node, consumed by existing scope/emission machinery).

## Repair iteration (post-review)

### Catch bindings in the eval preflight

Root cause (review finding, blocking): `PseudoActivationObject::
EnsureNoRedeclProperty` (`lib/Runtime/Types/ActivationObject.cpp`) returned
`false` unconditionally, so the generic preflight walk
(`JavascriptOperators.cpp`, `OP_ScopedEnsureNoRedeclProperty`) skipped
catch-scope bindings for every name; only the `arguments`-specialized branch
implemented the correct simple-vs-pattern distinction. This was pre-existing
on main, and main still reported `SyntaxError` for the lane-exposed shape
only accidentally, through the merged scope: with the forced split the
destructured catch binding vanished from the walk entirely.

Fix: `PseudoActivationObject::EnsureNoRedeclProperty` now performs the same
`noRedecl`-aware check the arguments branch uses — a let-like (pattern)
catch binding conflicts (`SyntaxError`), a simple catch parameter does not
(Annex B.3.5), and the walk continues either way. No call-site change: the
generic walk is the only effective caller.

### Parameter named `arguments`

`(function(arguments = 1){ eval('var arguments'); return typeof arguments })()`
now returns `"undefined"` (Node) instead of `"number"`. The guard's first
repair attempt exposed a classification problem: the built-in arguments
node, a formal named `arguments`, and a body `var arguments` can share one
symbol, and which node owns the symbol's declaration distinguishes them.
`BodyRedeclaresParameter` now classifies via the formals chain
(`MapFormals`, parse-time safe — pattern sub-walks are NOT safe: a deferred
formal's pattern has no sub-AST yet, which crashed the first attempt) plus
the `fpnArguments` builtin flag and the well-known arguments pid
(`Symbol::IsArguments()` reads the declaration node's flag, which a winning
body var no longer carries). Body `var arguments` keeps the merged scope
(probe g2: `7` = Node); the parameter-only shape flips and shadows.

### Residual corrections

- The earlier "eval'd non-colliding declarations visible to parameter
  closures" bullet was stale: the lane's forced split FIXED probe a2
  (`function a2(cb = () => q, a = 1) { eval('var q = 3'); ... }` now gives
  `[3, ReferenceError]` = Node; main gives `[3, 3]`). Removed.
- Compound redeclaration residual, reworded: the guard is per-function, so
  the over-reported `SyntaxError` fires for **any** eval'd
  parameter-name/`arguments` declaration in a function whose body
  redeclares **any** parameter symbol — including when the eval'd name
  differs from the body var's parameter (`function (p=1,q=2){ var q;
  eval('var p') }` → throw; Node `[undefined,2]`) and for body
  `var arguments` + eval'd `var arguments` (main-identical).

### Tracked residual card (not fixed, pre-existing)

`(function(a = 1){ return typeof arguments.callee })()` — Node throws
`TypeError` (callee poisoning for non-simple parameter lists), we return
`"function"`. Eval-independent; recorded for a future slice.

### Repair evidence

- Catch battery (12 shapes incl. the lane-exposed c1, pre-existing c2-c4/c7
  `catch({a})`/`catch([a])`/`catch({b})` now throwing, simple-catch-param
  no-throw controls c5/c6/c11, pattern read c8, nested c12) — identical to
  Node v26.5.0.
- test262 `language/eval-code/**`: **454/0** (no flip; the catch shapes are
  not covered there). A `language/statements/try` + annexB spot check is
  391/7 with all 7 failures main-identical pre-existing (3× `tco-*`
  out-of-stack, 4× annexB `catch-redeclared-*-var` loop shapes); verified by
  running the same 8 files against the main binary (6/7, same set).
- Gates unchanged at the existing pins: test **2033/0**, debug-static
  **2085/0**.

## Coverage and evidence

Focused test: `test/es7/s14-eval-nonsimple-var.js`, registered in
`test/es7/rlexe.xml` with `-args summary -endargs`, asserting Node-verified
values for: default/rest/destructured parameter collisions with and without
eval initializers, the rest parameter's own name, eval'd function
declarations of parameter names, non-colliding declarations, double evals
and post-eval assignments, post-eval closures, method/generator/arrow
forms, eval'd `var arguments`, indirect eval, simple-parameter positive
controls (no throw, binding reuse), body-lexical collisions that must still
throw, the varEnv-terminating walk over an outer `let`, the pre-split
parameter-closure control, the formals-eval split path with body
var/function collisions, destructured catch bindings that must throw
(object/array patterns, param and non-param names) with simple catch
parameter no-throw controls, a parameter named `arguments` shadowed by an
eval var, and body `var arguments` keeping the body declaration value.

Legacy expectation updates, limited to formerly non-conforming throws:
`test/es6/default.js` (2 assertions) and `test/es6/rest.js` (6 assertions)
changed from `assert.throws(SyntaxError)` to value assertions of the
separate body-environment binding (`undefined`), verified against Node
v26.5.0 shape by shape. No other expectation in those files changed.

Gates (all through the shared lock; final binary rebuilt after the guard
refinement):

| Selection | Result |
|---|---|
| `runtests.py -t --variants=disable_jit` | **2033/0** (was 2032/0; +1 new entry) |
| `runtests.py -d --variants=disable_jit` (debug-static build) | **2085/0** (was 2084/0; +1 new entry) |
| test262 `test/language/eval-code/**` (pinned 2e0a5676 corpus, controls 20/20) | **454/0** |
| `test/es7` focused (incl. all S14 rlexe files) | 97/0 |
| probe battery (9 files, ~80 scenarios) vs Node v26.5.0 | identical except the residuals below |

Pin decision: the rlexe entry is new, so `tools/build/gate_counts.json`
pins moved 2032→2033 / 2084→2085 with a `_lineage` note appending
"C2-S14 eval-nonsimple-var added s14-eval-nonsimple-var.js (2032/2084 ->
2033/2085)" and this receipt; `tools/build/test_build_tools.py` (62 tests,
including the pin-consistency tests) passes.

Residuals (not claimed; see also the repair-iteration corrections above):
- Compound redeclaration shape — any eval'd parameter-name or `arguments`
  var/function declaration in a function whose body statically redeclares
  any parameter symbol (no parameter closure): keeps the over-reported
  `SyntaxError`, identical to main (probes p5/p6/n10/p9/p10/q6f, and
  differing-name shapes like `function (p=1,q=2){ var q; eval('var p') }`).
  A full fix needs the split decided before the body is parsed (reparse or
  post-hoc symbol re-binding) so the body declaration gets its own
  body-scope symbol.
- `arguments.callee` poisoning for non-simple parameter lists
  (`(function(a = 1){ return typeof arguments.callee })()` returns
  `"function"`; Node throws `TypeError`) — pre-existing, eval-independent,
  tracked card.
- JIT lowering of the eval declaration ops remains the separately tracked
  B4/eval-declaration follow-up; Mac interpreter/no-JIT evidence only, per
  the lane scope.
