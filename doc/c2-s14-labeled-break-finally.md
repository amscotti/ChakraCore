# C2-S14 P7: labeled break out of nested try-finally

Status: **landed in `05616c535`** (feature `45361dbd2`; base `ff170d34b`).
At landing 1993/2045; current integrated gates **2004/2056**. Interpreter/Mac no-JIT evidence only;
B4 owns JIT/Linux/concurrent-GC claims.

## Crash

The pre-existing crash found in the S14 P1 review was:

```js
1; try { L: { try { break L; } finally {} } } finally {}
```

Run as eval completion (for example `print(eval(...))`) the base/current engine
exited **139** with no stdout; Node v26.5.0 returns `undefined`. The same shape
inside an ordinary function also crashed:

```js
function f(){ try { L: { try { break L; } finally {} } } finally {} return 7; }
```

Node and the fixed engine return `7`. An inner `try { throw } catch` whose
`break L` is in the catch body, and nested `L1`/`L2` labels, crashed the same
way. The ordinary-function form has no completion register save, so this is not
caused by P1's `saveReturnValueReg`; P1 only made the eval cleanup path carry an
additional save/restore.

## Root cause

`EmitJumpCleanup` walked every active entry in `jumpCleanupList` and emitted a
`Leave` for each try/handler entry. It stopped only when a loop entry's
`loopNode` matched the jump target. A `break L` whose target is a labeled
statement nested inside an enclosing try therefore also emitted `Leave` for that
enclosing try, even though the break did not exit it.

Pre-fix bytecode for the eval repro (debug-static dump):

```
0002   TryFinally           x:0019       ; outer try-finally
0005   TryFinally           x:0011       ; inner try-finally
0008   Leave                             ; inner cleanup
0009   Leave                             ; outer cleanup -- emitted by mistake
000a   Br                   x:0015       ; break L target (end of block L)
...
0011   Finally
0014   LeaveNull
0015   Leave                             ; outer try body's normal exit
0016   Br                   x:001d
0019   Finally
001c   LeaveNull
```

The second `Leave` runs the outer finally and resumes after it while still
inside the outer try. Execution then reaches the outer try's normal `Leave` at
`0015` (which is also block L's break label) with no active `ProcessTryFinally`
for that scope. The stale resume offset/control flow ends in SIGSEGV.

After the fix the same dump has only the inner cleanup before the break target:

```
0005   TryFinally           x:0010       ; inner try-finally
0008   Leave                             ; inner cleanup only
0009   Br                   x:0014       ; break L target
...
0014   Leave                             ; outer try body's normal exit
0015   Br                   x:001c
0018   Finally
001b   LeaveNull
```

## Fix

A jump target statement now records the bytecode offset at which its emission
starts (`ParseNodeStmt::byteCodeStartOffset`), and each try cleanup entry records
the offset of the protected body it entered (`JumpCleanupInfo::
byteCodeStartOffset`, captured by `PushJumpCleanupForTry`). `EmitJumpCleanup`
stops at the first try entry whose protected body starts at or before the target
start: the target is inside that region, so the jump does not exit it. Because
the cleanup list is innermost-first, all remaining entries enclose the target as
well. Entries the jump does exit still emit `Leave`, and P1's
`saveReturnValueReg` save/restore remains on those paths.

Offsets are generator-local ordinal byte offsets, not pointers. The new fields
are a 32-bit scalar in the arena-allocated parser node and a 32-bit scalar in the
arena-allocated jump-cleanup list entry. No allocation, ownership, GC tracing,
barrier, memory-accounting or JsRT boundary changes. No new runtime opcode is
emitted; only the set of existing cleanup `Leave` operations changes. Parser and
bytecode generation are single-threaded for a function's emission.

## Evidence

Pre-fix binaries (same P1 source, no P7 changes) and the exact repros:

- test profile `/Users/ascotti/Documents/code/ChakraCore-wt/s14-p1/zig-out/bin/ch`:
  eval repro and ordinary-function repro both exit **139** with no stdout.
- debug-static `/Users/ascotti/Documents/code/ChakraCore-wt/s14-p6/out/debug-static/bin/ch`:
  ordinary-function repro exits **139**.

Post-fix, `zig-out/bin/ch`:

```
eval("1; try { L: { try { break L; } finally {} } } finally {}") -> undefined
function f(){ ... return 7; } f()                                     -> 7
```

Focused `test/es7/labeled-break-finally.js` (registered in
`test/es7/rlexe.xml` with `-args summary -endargs`) passes:

- normal test binary: pass;
- test binary with `-CollectGarbage`: pass;
- existing `statement-completion.js` with `-CollectGarbage`: pass;
- debug-static binary: pass;
- debug-static binary with `-CollectGarbage`: pass.

The test covers the eval and ordinary-function repros, catch/finally inner tries,
nested labels, break from a finally to a label inside the enclosing try,
`break`/`continue` through finally in loops, finally side-effect ordering
(exactly once), Node-matched completion values, and nested return-through-finally.

Broad test profile regression:
`python3 test/runtests.py --test --variants=disable_jit --binary "$PWD/zig-out/bin/ch"`
=> **1993 passed / 0 failed** (1,992 previous scenarios plus the new es7 test),
45.3 s. Integration owns the cumulative gate pin count.

Test262 selection (pinned `2e0a5676…`, default host flags
`-nonative -Test262 -CollectGarbage`), patterns
`statements/labeled/**`, `statements/try/**`, `statements/for/**`:
before and after **1171 passed / 12 failed** (1,183 scenarios, 610 files),
**0 fail→pass / 0 pass→fail**. The 12 failures are unchanged and
pre-existing/unrelated: TCO stack overflow (8), `static-init-await-binding-valid.js`
parse error (2), and `let-block-with-newline.js`/`let-identifier-with-newline.js`
parse errors (2). No unrelated corpus gain is claimed.

A 22-case Node v26.5.0 comparison probe (eval and function forms: nested
labels/finally, catch-inner break, break from finally, loop `continue outer`,
finally side-effect order, and nested return-through-finally) produced identical
output on Node and the fixed test binary.

## Residual and risks

- Evidence is Mac interpreter (`-nonative`) only; JIT codegen and bailout are not
  claimed here (B4 remains gated). The change alters emitted bytecode for jumps
  that stay inside a try; the JIT consumes the same cleanup `Leave` stream and is
  covered by B4, not this note.
- `EmitJumpCleanup` still uses the original `loopNode == target` check for loop
  profiling entries. That is unchanged: loop entries carry no control-flow
  `Leave`, so the crash path is the try/handler entries fixed here.
- `byteCodeStartOffset` is meaningful only during emission of a target statement;
  it is reset to `UINT_MAX` in the node constructor and is not serialized or used
  after bytecode generation.
- No shared plan, triage or gate-pin file was changed.

## B4 note

Mac no-JIT interpreter evidence only. JIT (`SimpleJIT`/`FullJIT`), bailout,
profiling and cross-platform behavior remain for B4; no claim is made that this
slice changes JIT semantics.
