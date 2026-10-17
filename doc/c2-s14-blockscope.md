# C2-S14: block static-semantics early errors

Status: landed in `5366c23f5` plus the statement-position follow-up `<pending>` on top of `68784ee87` (2026-10-17).
Interpreter/Mac no-JIT verified; JIT evidence is not claimed (see B4 below).
No flag was added; this is parser correctness.

## Scope and spec rule

ES `sec-block-static-semantics-early-errors`:

- `Block : { StatementList }` is a Syntax Error if a
  LexicallyDeclaredName of the StatementList also occurs in its
  VarDeclaredNames.
- The block's LexicallyDeclaredNames are `let`, `const`, `class` and
  function declarations (ordinary, generator, async and async-generator) at
  the top level of the block. Duplicate entries are also a Syntax Error.
- VarDeclaredNames recursively includes `var` declarations in nested
  statements, blocks, loops, `catch`, `switch` and `with`, but not names
  declared inside a nested function (including methods). Class static blocks
  are not supported by this engine and are outside this slice.
- Annex B block-level function declarations remain valid: a lone
  `{ function f() {} }` is accepted, and duplicate ordinary function
  declarations are accepted in sloppy blocks but not in strict code.

Function declarations directly as loop bodies (`while`, `do-while`, `for`,
`for-in`, `for-of`, `for-await-of`) are also rejected, matching the
Annex B statement-position rule; the three
`syntax/function-declarations/in-statement-position-*` negatives now pass.
Unlabelled `with` function declarations remain inherited (see Residual).

## Parser design

`lib/Parser/Parse.cpp` / `Parse.h` add parser-only name lists to
`BlockInfoStack`; they are not AST or bytecode state:

- `AddBlockLexicalDecl` records each lexical declaration in the current
  source block and rejects duplicates through a lazy per-block
  `BlockNameMap` hash set, except two ordinary sloppy function declarations
  (the Annex B case). Function declarations are recorded after their body
  is parsed, when `IsGenerator`/`IsAsync` and the enclosing strictness are
  known.
- `AddBlockVarDecl` records at most one `BlockInfoName` per name in the
  nearest function block's `varNameMap`, holding the highest source-block
  id seen for that name; it does not copy the name to every enclosing block
  or grow a per-name list. Only declarations created by
  `ParseVariableDeclaration` pass `isVarDeclaredName`; parameter/computed/
  functions synthetic var nodes (`arguments`, special symbols and the
  Annex B block-function var alias) are excluded.
- `CheckBlockDeclarations` runs at `FinishParseBlock` and looks up each
  lexical name in the function's var map. Block ids are assigned in
  depth-first parse order, so at the close of block B a record with
  `sourceBlockId >= B.blockId` was parsed inside B; report
  `ERRRedeclaration` in that case. This is O(1) per lexical name after the
  dictionary lookup.
- `ParseBlock` marks the `BlockInfoStack` as a syntactic source block.
  Function bodies, parameter/catch scopes, `for`-head scopes, `switch`
  case blocks and class scopes are not treated as `Block : { … }`, so their
  inherited (and separately specified) behavior is unchanged. In
  particular this preserves the existing for-loop head behavior and avoids
  a broader `ForStatement` early-error change in this slice.
- `FncDeclAllowedWithoutContext` now rejects a sloppy function declaration
  whose immediate statement is a loop body (`knopWhile`, `knopDoWhile`,
  `knopFor`, `knopForIn`, `knopForOf`, `knopForAwaitOf`). `if`/label/switch
  forms remain valid; the existing labelled-function-in-`with` diagnostic is
  preserved by leaving `with` to that check.
- The maps live only during parsing. Deferred parsing builds them again;
  no AST/deferred-reparse payload changed, and no bytecode was regenerated.
  The maps are arena-backed; var records are deduplicated per source block,
  so parser memory is O(lexical names + var declarations), not
  O(block depth × vars).

This deliberately leaves function-body lexical-vs-var checks to the
existing shared function-scope symbol path, which already rejected
`function f() { let x; { var x; } }` before this batch.

## Evidence

Block-scope corpus (pinned test262 `2e0a5676…`, `-nonative -Test262
-CollectGarbage`, 287 scenarios, 145 files):

| Run | Passed | Failed | Failures |
|---|---:|---:|---|
| before (`out/test262-c1-blockscope`) | 167 | 120 | 117 redeclaration + 3 statement-position |
| after (`out/test262-c2-s14-blockscope-final4`; debug `…-debug4`) | **287** | **0** | none |

Exact flips vs the pre-slice artifact: **120 fail→pass, 0 pass→fail**. Both
declaration orders, nested inner blocks, class/function/async/generator/
async-generator lexical names, function-scope variants and the three
statement-position negatives are included in the flips. The block-scope
corpus is clean.

Focused `test/es7/blockscope-early-errors.js` (registered in
`test/es7/rlexe.xml` with `-args summary -endargs`) passes on both the test
and debug-static binaries. It covers valid Annex B blocks and sibling/
function-boundary cases, both orders for `let`/`const`/class/function/
generator/async/async-generator, nested blocks, loops/`switch`/`try`/
`with`, function-scope variants, duplicate lexical functions, strict
duplicate ordinary functions, loop-body negatives, and a
`WScript.LoadScript` script-level parse block.

Direct `eval` and `WScript.LoadScript` probes (both orders, nested blocks,
function scopes, Annex B valid cases, strict duplicate functions, catch
`var`) produced byte-identical OK/SyntaxError output on ChakraCore and Node
v26.5.0. The script-level probes accepted the Annex B scripts and rejected
the class/var scripts on both engines.

Resource check on the final binary (Mac test profile):

| Input | Final | Pre-fix |
| --- | ---: | ---: |
| 4,000 nested blocks each with `var x` (32 KB) | 26 MB RSS | 524 MB |
| 16,000 nested blocks each with `var x` (128 KB) | 36 MB RSS, 0.94 s | 2.01 s |
| 4,000 nested blocks with 20 vars each (608 KB) | 37 MB RSS, 1.20 s | 6.51 s |
| 2,000 nested blocks with 10,000 unique vars (103 KB) | 32 MB RSS | 673 MB |
| 20,000 distinct `let` declarations (209 KB) | 0.04 s | 1.40 s |

Existing `test/LetConst/` expectations asserted the old non-conforming
behavior and were updated deliberately (not silently):

- `letvar.js` / `letvar.baseline`: `{ let x; var x; }` and
  `{ var x; let x; }` are redeclaration SyntaxErrors.
- `defer3.js` / `defer3.baseline`: the four block-scope
  `var`/`const`/`let` combinations (lines 31–34) are SyntaxErrors.
- `for-loop.js` / `for-loop.baseline`: the `{ for (var a …) … let a; }`
  case is a block redeclaration SyntaxError; the separate for-head
  `let v2`/`var v2` legacy case in `test3` is unchanged.
- `test/es6/blockscope-functionbinding.js`: Test 20 removed its direct
  `while`/`for`/`for-in`/`for-of`/`with` function declarations, which were
  non-conforming; its baseline output was already unchanged.
- `test/strict/21.functionDeclaration.baseline`: the four non-strict loop
  cases now report `SyntaxError`, matching Node and the spec.

Gates:

- `mise exec -- zig build`: pass.
- `mise exec -- zig build check --summary line`: **1976 passed / 0
  failed**, bytecode up to date, 572/572 steps.
- `mise exec -- zig build check-regression -Dprofile=debug-static
  --summary line`: **2028 passed / 0 failed**, 545/545 steps.
- `python3 tools/build/check_regression.py --self-test`: all negative
  controls behave, including the 2028/0 debug pin.

Gate bookkeeping was updated in `tools/build/check_regression.py`
(docstring, defaults, self-test, `SELECT_COUNTS`), `build.zig` (test and
debug-static), all three `.github/workflows/zig-macos.yml` legs (test,
debug-static, legacy cmake test) and `doc/memory-management.md`.

## Residual

The block-scope cluster is complete. The following are separate early-error
rules and remain inherited (all pre-existing; Node rejects):

- `ForStatement` head lexical-vs-var, e.g. `for (let f;;){ var f; }` and
  `for (let f of []){ var f; }` (4 probe cases).
- `SwitchStatement` CaseBlock lexical-vs-var, including duplicate lexical
  names across cases (2 probe cases).
- `CatchParameter` conflicts, including simple `catch(e){ let e; }` and
  pattern/`var` cases (2 probe cases).
- Unlabelled `with ({}) function f() {}`; the labelled form retains its
  specific existing diagnostic.
- `eval`-introduced var conflicts such as `{ let f; eval('var f'); }`
  (base and current both accept).

The block rule still sees vars nested inside those statements when they are
inside a source block, but those statements' own head/case/parameter rules
are not expanded here. Full-corpus and JIT claims remain outside this
receipt.

## B4 note

This is parser-only and emits no new bytecode or runtime state; bytecode
verification is unchanged. Execution evidence is Mac no-JIT only. B4 still
owns any JIT-configuration claim; the parser checks run before either JIT
tier, but no JIT run is claimed in this receipt.
