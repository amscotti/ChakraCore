# C2-S14 P1: statement completion values (UpdateEmpty)

Status: **landed in `a4eba5a38`** (feature `e1e271c42`; base `b50fa4d94`),
2026-10-18. At landing 1992/2044; current integrated gates **2004/2056**; exact selection on the
integrated binary **152/4**, **112 fail→pass / 0 pass→fail**. Interpreter/Mac
no-JIT only; JIT evidence is not claimed (see B4).

## Scope and spec rule

Direct eval of a Script returns the script's completion value after
`UpdateEmpty`. ES2015+ changed the statement rules from "empty" to an
explicit undefined in several shapes:

- `IfStatement` returns `? UpdateEmpty(stmtCompletion, undefined)`; a
  false condition or an empty selected branch therefore completes with
  undefined, not the value of an earlier statement.
- `WithStatement` likewise returns `? UpdateEmpty(stmtCompletion,
  undefined)`.
- `TryStatement` returns `? UpdateEmpty(C, undefined)` (try or catch
  completion) or `? UpdateEmpty(F, undefined)` for the finally form. A
  normal finally completion discards its own value; an abrupt finally
  completion keeps the value produced in the finally body.
- Iteration statements and `switch` start their accumulator `V` at
  undefined and return `V`; a `break`/`continue` with an empty value is
  replaced by the accumulator, while a value already produced in the
  same statement list is kept.
- Empty statements, blocks with an empty list, `var`/`let`/`const`
  declarations, function/class declarations and `debugger` still
  complete with empty, so a preceding statement value is inherited.
  Expression statements, including the explicit `undefined` literal,
  overwrite the value.

The triage target was `eval('1; if (false) { }') === undefined`; the same
rule covers switch, for/for-in/for-of/while/do-while, try, with, labeled
breaks, and empty statement shapes.

## Design

Only global/eval code has `fscrReturnExpression`, so only that emission
path needs completion state. The existing `ReturnRegister` is used as the
single completion accumulator; expression statements already copy their
result there. `EmitLoadUndefinedToReturnRegister` establishes the
undefined baseline at the statement shapes above:

- `if`: emitted **before** condition dispatch so both the true and false
  labels see it; a branch that produces a value overwrites it.
- `for`, `for-in`/`for-of`, `while`, `do-while`, `switch`: emitted before
  the loop/switch entry, matching `V = undefined`. The emitter's normal
  back edges do not re-run the reset, so `V` persists across iterations.
- `with`: emitted before the body.
- `try-catch`: emitted before the try header and again immediately after
  the runtime `Catch` opcode, so a value stored before a throw cannot leak
  through an empty catch body.
- `try-finally`: the try completion (`B`) is saved in a temp register and
  the return register is cleared before the finally body. A normal finally
  exit restores `B`; an abrupt `break`/`continue` from the finally keeps
  the finally value. The same save/restore is emitted by
  `EmitJumpCleanup` for a `break`/`continue` that leaves the try region,
  because that path reaches the finally through `Leave` without executing
  the normal fall-through save. `JumpCleanupInfo` carries the save
  register; it is `NoRegister` for non-eval code and for other cleanup
  kinds.
- The inverted `for` optimization is skipped when `fReturnValue` is set:
  it deliberately emits both loops without completion propagation.

No parser change. Deferred parse and the AST are untouched; only
bytecode emission and the generator's jump-cleanup metadata changed.
`ReturnRegister` is the frame's existing return slot, so no new
serialized/runtime state or GC edge is introduced. The temp register used
by try-finally is a normal emitter temp released at the end of the
statement.

## Evidence

Pinned test262 `2e0a5676…`, `-nonative -Test262 -CollectGarbage`, test
binary unless noted. Before counts are from a binary built from the
same branch with the three source files reverted (stash + rebuild), not
from the recorded C1 corpus.

Selection: `test/language/statements/**/cptn-*.js`,
`statements/try/completion-values.js`,
`statements/for/head-init-*-completion.js`.

| Run | Passed | Failed | Scenarios |
|---|---:|---:|---:|
| before (`out/test262-c2-s14-p1-before-final`) | 40 | 116 | 156 |
| after (`out/test262-c2-s14-p1-after2`) | **152** | **4** | 156 |

Exact flips: **112 fail→pass, 0 pass→fail**. The after failures are all
pre-existing before failures and excluded from the triage selection:
`statements/class/cptn-decl.js` (2, S1-owned per the triage) and
`statements/using/cptn-value.js` (2, `using` syntax is not supported;
SyntaxError before and after). Excluding those four scenarios the
selection is **152/152**.

The 60 current-confirmed scenarios (triage: try 24, for 12, switch 22,
with 2) all flip, and the remaining 52 baseline flips land in the
expected clusters:

| Cluster | Scenarios | Before | After | Fail→pass |
|---|---:|---:|---:|---:|
| try (`cptn-*` + `completion-values.js`) | 24 | 0 | 24 | 24 |
| for (`cptn-*` + `head-init-*`) | 12 | 0 | 12 | 12 |
| switch `cptn-*` | 42 | 20 | 42 | 22 |
| with `cptn-*` | 2 | 0 | 2 | 2 |
| if `cptn-*` | 16 | 2 | 16 | 14 |
| for-in `cptn-*` | 16 | 0 | 16 | 16 |
| for-of `cptn-*` | 12 | 0 | 12 | 12 |
| while `cptn-*` | 6 | 0 | 6 | 6 |
| do-while `cptn-*` | 4 | 0 | 4 | 4 |
| **total** | **134** | **22** | **134** | **112** |

The remaining 22 scenarios in the 156-scenario run are 18 scenarios in
`async-function`/`const`/`empty`/`function`/`generators`/`labeled`/`let`/
`variable` that already passed before, plus the 2 class and 2 `using`
scenarios that still fail.

Focused `test/es7/statement-completion.js` (registered in
`test/es7/rlexe.xml` with `-args summary -endargs`) passes on the final
test binary. It covers every statement shape above, explicit undefined,
empty statements/blocks, declarations, labels, break/continue,
try/catch/finally including abrupt finally values, with, and ordinary
function returns.

Focused build: `mise exec -- zig build --cache-dir "$PWD/.zig-cache"
--summary line` succeeds, 567/567 steps.

Manual probe comparison against Node v26.5.0 covered 29 eval cases
including break/continue out of try-finally with values, nested
try-finally, labeled break through an empty `if`, `with` break/continue,
and switch fallthrough; output values/errors were identical.

Broader affected-statement regression run (`if`, `while`, `do-while`,
`for`, `for-in`, `for-of`, `switch`, `try`, `with`, `empty`, `labeled`;
3,500 scenarios, 1,917 files):

| Run | Passed | Failed |
|---|---:|---:|
| before (`out/test262-c2-s14-p1-regress-before`) | 3,305 | 195 |
| after (`out/test262-c2-s14-p1-regress-after`) | **3,417** | **83** |

Exact diff: **112 fail→pass, 0 pass→fail**; the 112 are the same
completion flips above. The 83 remaining failures are all pre-existing
(for-of 38, with 10, if 7, for 6, for-in 6, switch 6, try 5, while 3,
do-while 1, labeled 1) and unrelated to this slice.

## Residual

- `statements/class/cptn-decl.js` (2) still returns the class value from
  eval; the triage assigns class-declaration completion to S1.
- `statements/using/cptn-value.js` (2) still fails at parse time because
  `using` declarations are unsupported; outside P1.
- No parser, JIT, GC or memory-ownership change is claimed. The bytecode
  completion resets execute only in global/eval code (`fReturnValue`);
  ordinary function bodies, including return-through-finally, are
  unchanged.
- The broader affected-statement run above contains no pass→fail
  regressions; its 83 remaining failures are pre-existing and outside the
  completion cluster.

## B4 note

Mac interpreter (`-nonative`, no JIT) evidence only. The changes add
bytecode instructions in eval/global code; JIT codegen, bailout and
profiling behavior are not claimed here. B4 remains gated.
