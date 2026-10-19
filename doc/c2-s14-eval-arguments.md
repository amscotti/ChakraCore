# C2-S14 P2: direct-eval `arguments` declaration errors

Status: **`<pending>`** (branch `s14-p2`, base `a2e921875`), 2026-10-19.
Target selection **192/192** with **157 fail→pass / 0 pass→fail** and the 35
passing controls preserved. Interpreter/Mac no-JIT only; JIT evidence is not
claimed (see B4).

## Scope and spec rule

`test/language/eval-code/direct/*arguments*.js` exercises
`EvalDeclarationInstantiation` (ES2019 18.2.1.3, step 5.d), not a general
`arguments` restriction. For a sloppy direct eval, walk the lexical
environment chain from the eval's fresh lexical environment up to (but not
including) its variable environment. Each intermediate declarative
environment participates: if it has a binding for a var-declared name, the
eval declaration throws `SyntaxError` instead of hoisting over it.

During parameter initialization, a function's parameter environment is such
an intermediate environment. A normal function (including methods,
generators, async functions and async generators) always has an `arguments`
binding there when it has a non-simple parameter list: the implicit arguments
object, or a parameter named `arguments`. Therefore
`function f(p = eval("var arguments")) {}` must throw `SyntaxError` before the
body runs, regardless of a body `var`/`let`/function `arguments` declaration.
Arrow functions have no implicit arguments binding, so the same eval is legal
there unless a parameter is named `arguments`; the eval-created binding must
be visible to the body and to closures created in the parameter list.
A direct eval in an ordinary function *body* is also legal because the body's
variable environment, which holds the arguments binding, is where the loop
stops.

The implementation deliberately does not generalize the error to other var
names: non-`arguments` eval/parameter collisions remain P4-owned.

## Design

- `GlobalObject::VEval` determines whether a direct eval call is running in
  the caller's parameter scope, whether that caller is an arrow, whether the
  caller has a parameter named `arguments`, and whether the caller's top-level
  body has a lexical `arguments` declaration. It already walks the stack for
  the class-field context; the first script caller's
  `InterpreterStackFrame::IsParamScopeDone`, `FunctionBody::IsLambda`, and two
  stable `FunctionInfo` attributes (set from the parser's
  `fpnArguments_overriddenInParam`/`fpnArguments_overriddenByDecl`) are the
  discriminators. The four booleans are passed to
  `ExecuteEvalParsedFunction`, which publishes them on `ScriptContext` with an
  RAII save/restore across the eval call (including abrupt completion).
- In `OP_ScopedEnsureNoRedeclProperty` (`ByteCodeEmitter`'s eval-only
  `ScopedEnsureNoRedeclFld`), `arguments` is checked directly:
  - in a parameter-scope eval, the first frame-display environment is the
    parameter scope. An implicit arguments object or a formal named
    `arguments` (no-redeclaration property) throws `SyntaxError`. An
    eval-created property has no redeclaration flag, so a second eval
    declaration is legal; the first legal declaration creates a writable,
    enumerable, non-configurable var binding with `InitProperty`.
  - in a body eval, the first activation is either the body lexical
    environment or its variable environment. A no-redeclaration `arguments`
    there is a lexical collision unless the caller's parameter itself is named
    `arguments` and there is no top-level body lexical `arguments`; that
    parameter lives in the merged variable environment and remains legal.
    This covers simple/split/generator bodies, catch/with blocks and nested
    blocks, and lets a body eval declare its own binding without inspecting an
    outer split-scope parameter binding.
  - elsewhere the normal eval redeclaration walk runs. For `arguments` it uses
    `EnsureNoRedeclPropertyForEval`, a SyntaxError-reporting variant of the
    existing lexical-redeclaration check; other names keep the existing
    `EnsureNoRedeclProperty` behavior.
- An arrow's parameter scope is marked dynamic when the direct eval is in its
  parameter list (`ByteCodeGenerator::StartBindFunction`), so a parameter
  closure (`q = () => arguments`) resolves the eval-created binding. Body
  loads, `typeof`, call-target resolution, stores, compound assignments,
  increments and deletes of the outer parameter scope emit `LdParamObj` plus a
  property check instead of reading the body closure register
  (`DynamicLoadKind::ParamObj`). Stores use `StFld`, deletes
  `DeleteFld_ReuseLoc`; this keeps body reads and writes on the same parameter
  binding, never a global property, and lets delete fail on the
  non-configurable binding. Unrelated body var/function bindings keep their
  own slots, and body eval/var declarations shadow the parameter binding
  normally.
- `ActivationObject`/`RootObjectBase` gain the
  `EnsureNoRedeclPropertyForEval` override; `BlockActivationObject` and
  `PseudoActivationObject` keep returning false. `RecyclableObject` provides
  the default false. The helper is only reachable from the eval scoped check.

No parser AST/early-error change is required. The eval source is parsed as
before; the check runs at eval declaration instantiation, so error timing is
parameter initialization rather than eval parse or body execution.

Ownership: the eval frame display and its scope objects remain owned by the
caller frame; the new booleans are non-owning state. No value is copied into a
second scope. The parameter-scope loads read the existing parameter closure via
`LdParamObj`; the body and parameter closures remain owned by the split-scope
frame. Invalidation/threading: parser/bytecode AST unchanged; the runtime state
is thread-local (`ScriptContext`), and eval executes synchronously on the
caller thread. No GC tracing or async job state is added.

## Evidence

Pinned test262 `2e0a5676…`, `-nonative -Test262 -CollectGarbage`, test
binary unless noted. “Before” is the base `a2e921875` binary; “after” is this
branch.

Selection `test/language/eval-code/direct/*arguments*.js`:

| Run | Passed | Failed | Scenarios |
|---|---:|---:|---:|
| before (`out/test262-c2-s14-p2-before`) | 35 | 157 | 192 |
| after (`out/test262-c2-s14-p2-final-direct`) | **192** | **0** | 192 |
| after, `-force:deferparse` (`out/test262-c2-s14-p2-final-direct-defer`) | **192** | **0** | 192 |

Exact flips: **157 fail→pass, 0 pass→fail**. All 35 pre-existing passes
(including the async cases that pass vacuously on the fulfilled path) remain
green; the same tests now also take the SyntaxError rejection/throw path where
the corpus expects one. The 60 parameter-binding failures, 85 body/no-binding
failures and 12 arrow failures all flip under the one rule above.

Full direct+indirect eval-code directory:

| Run | Passed | Failed | Scenarios |
|---|---:|---:|---:|
| before (`out/test262-c2-s14-p2-evalcode-before`) | 280 | 174 | 454 |
| after (`out/test262-c2-s14-p2-final-evalcode`) | **437** | **17** | 454 |

Exact diff: **157 fail→pass, 0 pass→fail**. The residual 17 are pre-existing
P4/class-field `super` cases (`super-prop*`, `var-env-*`, both direct and
indirect), unchanged by this slice.

Broader affected-shape run
(`arguments-object`, `expressions/arrow-function`, `expressions/function`,
`function-code`; 1,868 scenarios):

| Run | Passed | Failed |
|---|---:|---:|
| before (`out/test262-c2-s14-p2-regress-before`) | 1,711 | 157 |
| after (`out/test262-c2-s14-p2-final-regress`) | **1,715** | **153** |

Exact diff: **4 fail→pass, 0 pass→fail** (the four
`arrow-function/scope-param-{elem,rest}-var-{open,close}.js` cases, which
observe an eval var from an arrow parameter closure).

Focused `test/es7/eval-arguments-declarations.js` is registered in
`test/es7/rlexe.xml` and passes. It covers the implicit binding with a function
declaration/expression/method, preceding/following `arguments` parameters,
body `var`/`let`/function colliders, generator and async/async-generator
timing, arrow legal declarations/body shadowing/parameter closures, direct
body eval, indirect eval, and the “default skipped” timing control. It also
pins the regression shapes found in review: body var/function/parameter
bindings after an arrow parameter eval (no aliasing), duplicate var and
function declarations in one or two parameter evals (last value/function
wins), body eval after a parameter eval (undefined body binding), and body eval
with a parameter named `arguments` in function and arrow default,
direct-eval-default, rest and destructured parameter forms. Body writes,
compound assignments, increments and deletes of the eval-created binding are
covered, including the non-configurable delete result and global-object
non-leakage, plus top-level body `let`/`const` collisions in simple, split,
generator, catch, with and nested-block shapes.

The legacy suite (`runtests.py -t --variants disable_jit test`) is
**1994/0**, including `test/es6/default-splitscope.js`. That test caught an
initial over-broad version: marking every function's parameter scope dynamic
made parameter-eval vars leak into ordinary function bodies. The final change
marks parameter scopes dynamic only for arrows and performs no value copy into
the body scope.

## Residual

- The eval-scoped SyntaxError variant is applied to `arguments` only.
  Other eval var collisions (parameter `p`, body/global lexical names) keep
  the prior behavior and remain P4’s declaration-instantiation batch. In
  particular, `function g(p = 1) { eval("var p = 9"); }` still throws the
  pre-existing `ReferenceError`; this slice does not claim it.
- Indirect eval, global code and module code are outside the trigger: no
  caller parameter scope is involved.
- Nested-block and body-eval lexical collisions are only partially modelled.
  The body rule inspects the first activation environment; a `let`/`const
  arguments` in a nested block is detected for ordinary callers, but is
  under-rejected when the same function also has a parameter named `arguments`
  (the parameter flag allows the no-redeclaration first environment). A lexical
  environment that the frame display does not expose as a separate first
  activation has the same limitation. This predates the slice and is not
  claimed.
- Async functions surface the SyntaxError as a rejected promise; async
  generators and generators evaluate parameter defaults with the engine’s
  existing eager scheduling, so the corpus’s synchronous `assert.throws`
  expectation passes there.
- No sanitizer, JIT, concurrency or OOM-injection run.

## B4 note

Runtime/parameter-environment checks plus one eval-only redeclaration helper;
no GC, JIT, parser early-error or serialized-bytecode layout change is claimed.
Execution evidence is Mac no-JIT (test profile and the legacy disable-JIT
suite). The parameter-scope discriminator reads the caller’s
`InterpreterStackFrame`, so a JIT configuration is not claimed here; B4 owns
JIT configuration and any JIT evidence.
