# C2-S14 residual: hidden outer eval `arguments` binding

Status: **landed** — commit `d9a1998d1`, merge `13e925a75`; final gates
**2021/0 test** and **2073/0 debug-static**. Mac ARM64 no-JIT evidence only;
no gate-pin change. Base `183e7fab0`.

## Problem

`EvalDeclarationInstantiation` must reject a sloppy direct-eval `var
arguments` when an intervening lexical environment already binds `arguments`.
The body-eval walk in `OP_ScopedEnsureNoRedeclProperty` stopped at the first
runtime `ActivationObject`. A catch scope is such an object, so it hid a
top-level body `let`/`const arguments`:

```js
function f() {
    let arguments = 1;
    try { throw 1; } catch (e) { eval("var arguments"); }
}
```

The old binary accepted this; Node 26 and the spec throw `SyntaxError`.
The catch form was pinned as a known residual in
`test/es7/eval-arguments-declarations.js`.

## Design

`lib/Runtime/Language/JavascriptOperators.cpp` now walks the caller environment
chain for an `arguments` declaration instead of stopping at the first
activation object:

- `ScopeType_ActivationObject` entries are inspected in order.
- `BlockActivationObject` entries are lexical environments; an own
  `arguments` binding is a redeclaration conflict, otherwise the walk
  continues.
- `PseudoActivationObject` entries (catch/function-expression scopes) are
  skipped when they have no lexical binding. A simple catch parameter named
  `arguments` remains legal under Annex B.3.5; a let-like/pattern binding
  conflicts.
- The first plain activation object remains the eval variable environment:
  a lexical binding there conflicts unless it is the allowed parameter
  `arguments` shape; a plain var/parameter binding stops the walk.
- `with` objects and slot arrays are not declarative environments and are
  skipped.
- A nested arrow's own variable environment still terminates the walk, so an
  outer lexical binding is not falsely reported.

No allocation, GC, root, retained pointer, parser layout or serialized-bytecode
change is introduced. The walk reads existing `FrameDisplay` environments
synchronously.

## Evidence

Focused `test/es7/eval-arguments-declarations.js` (existing one-entry rlexe
test, so gate pins do not change): **pass** on test and debug-static,
including the former known-residual catch case and new catch/block/if/with,
nested-block, arrow, split-parameter and generator coverage. The merged
full gates are **2021/0 test** and **2073/0 debug-static**, `zig build check`
**583/583**. Adjacent
`eval-arguments-declarations`, `default-splitscope`,
`global-eval-declarations` and `annexb-block-function-var` files: **5/0**.

Main-vs-lane probe:

```text
main: hidden:no-throw  param:b  simple:no-throw
lane: hidden:SyntaxError param:b simple:no-throw
```

Node 26 gives the same lane result for the hidden and parameter cases.

Pinned test262 (20 controls, `-nonative -Test262 -CollectGarbage`):
- full `test/language/eval-code/**/*.js`: **454 passed / 0 failed**;
- catch-parameter `arguments` focused files: **2/0**.
No pass→fail was observed in these runs.

## Residual and B4

- The catch/block walk is a runtime environment-chain heuristic. JIT-generated
  frame displays, TTD/serialization and debugger scopes were not exercised.
- The run is Mac ARM64 no-JIT only. B4 owns Linux/JIT evidence.
- No new rlexe entry or gate-pin change is required.
