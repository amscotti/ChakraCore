# C2-S14 residual: body eval with non-simple parameters

Status: **in progress** (diagnosis recorded; fix pending).

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

## Scenario evidence (before fix)

Oracle: Node v26.5.0. `ch` = main `zig-out/bin/ch -nonative` at `3a6d245de`
(identical probes on the worktree binary pre-fix). `⇒` marks the divergent
cell.

| # | Scenario | Node | ch before |
|---|---|---|---|
| f1 | `function f1(a = 1) { eval('var a'); return a; } f1()` | `undefined` | ⇒ SyntaxError |
| f2 | `function f2(a = 1, ...rest) { eval('var a = 2'); return [a, rest.length]; } f2()` | `[2, 0]` | ⇒ SyntaxError |
| f3 | `function f3({a} = {}) { eval('var a'); return a; } f3()` | `undefined` | ⇒ SyntaxError |
| f3b | `function f3b({a} = {}) { eval('var a = 9'); return a; } f3b()` | `9` | ⇒ SyntaxError |
| f4 | `function f4({a} = {}) { eval('var b = 3'); return [a, b]; } f4()` | `[null, 3]` | `[null, 3]` |
| f5 | `function f5(a = 1) { eval('function a(){}'); return typeof a; } f5()` | `"function"` | ⇒ SyntaxError |
| n4 | `function n4(a = 1) { eval('var a'); return [a, (()=>a)()]; } n4()` | `[null, null]` | ⇒ SyntaxError |
| n6 | two evals: `eval('var a = 2'); eval('var a'); return a` | `2` | ⇒ SyntaxError |
| n7 | `function n7(cb = () => a, a = 1) { eval('var a'); return [a, cb()]; } n7()` (split today) | `[null, 1]` | `[null, 1]` |
| q3 | `function q3(cb = () => a, a = 1) { eval('var a = 2'); return [a, cb()]; } q3()` (split today) | `[2, 1]` | `[2, 1]` |
| s1 | `function s1(a) { eval('var a = 7'); return a; } s1()` (simple param) | `7` | `7` |
| h1 | `function h1(a = 1) { let x; eval('var x'); }` (body lexical collision) | SyntaxError | SyntaxError |
| b1 | `function b1(a = 1) { var a = 5; return a; } b1()` (no eval) | `5` | `5` |

Already-split controls (n7, q3, a4/a5/a7–a10 in the probe set) match Node
exactly today, including `undefined` for a no-initializer eval var and the
parameter binding remaining untouched for parameter closures.

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
eval — the same treatment `Parse.cpp:8455` already applies to eval seen
during formals. This is a parser flag decision only; it changes no
allocation, ownership, retained pointer, callback, GC barrier or memory
accounting (per repo `AGENTS.md`, `doc/memory-management.md` was not
re-consulted beyond confirming the change is outside its scope: one boolean
on the parse node, consumed by existing scope/emission machinery).

Residual (not claimed): in split functions the placement/visibility of
eval'd *non-colliding* declarations for parameter-list closures — probe a2 on
main: `function a2(cb = () => q, a = 1) { eval('var q = 3'); ... }` gives
`[3, 3]` where Node gives `[3, ReferenceError]` — a pre-existing property of
the already-split path, unchanged by this slice. JIT lowering of the eval
declaration ops remains the separately tracked B4/eval-declaration follow-up.
