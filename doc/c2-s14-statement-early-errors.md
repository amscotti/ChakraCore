# C2-S14 sub-batch 2: statement-level static-semantics early errors

Status: `<pending>` — uncommitted on top of `55265de59` (2026-10-17).
Interpreter/Mac no-JIT verified; JIT evidence is not claimed (see B4).
No flag was added; this is parser correctness.

## Scope

This sub-batch closes the next S14 clusters after the block-scope batch:

- **ForStatement / ForIn / ForOf / ForAwaitOf**: a lexical bound name in
  the head may not occur in `VarDeclaredNames` of the body.
  Corpus failures: 4 (`head-let-bound-names-in-stmt`,
  `head-const-bound-names-in-stmt`, default + strict).
- **SwitchStatement CaseBlock**: `LexicallyDeclaredNames` across case
  clauses must be duplicate-free and must not occur in the CaseBlock's
  `VarDeclaredNames`. Corpus failures: 59 redeclaration parse negatives.
- **CatchParameter**: a `CatchParameter` bound name may not occur in the
  `LexicallyDeclaredNames` of the directly nested body Block. Corpus
  failures: 4 (`early-catch-function`, `early-catch-lex`,
  default + strict).
- **Unlabelled `with` function declaration**: a FunctionDeclaration is not
  a valid Statement for a `with` body. Corpus failure: 1
  (`with/decl-fun.js`).

Not changed: catch pattern-vs-`var` (`catch({e}) { var e; }`), other
`TryStatement` completion/TCO failures, and the separate ForStatement
head rules beyond lexical-vs-var.

## Parser design

All state is parser-only (`lib/Parser/Parse.cpp`); no AST/bytecode shape
or runtime state changed.

- **For head**: the regular block created for the head is marked as a
  source block. It remains the current block while the body is parsed, so
  its close runs the existing lexical-vs-var check. A direct body `var`
  records against the head block; a body block's `var` records against that
  block, whose higher source-block id is visible to the head check. Nested
  functions keep their own map and remain a boundary.
- **Switch CaseBlock**: the regular block created for the CaseBlock is
  marked as a source block. The existing per-block lexical map therefore
  covers all case clauses, and the existing lexical-vs-var check covers
  vars in cases, nested blocks and direct statements. This closes both
  duplicate-lexical and lexical-vs-var cases without a separate list.
- **CatchParameter**: after the parameter is parsed, its BoundNames are
  collected from the simple identifier or destructuring pattern. They are
  attached by `PushBlockInfo` to the directly nested body block through a
  one-shot pending flag. At `FinishParseBlock`, each parameter name is
  looked up in the body block's lexical map and `ERRRedeclaration` is
  raised on a match. The names are not added to the body's lexical map, so
  `catch(e) { var e; }` and nested blocks remain legal.
- **With**: after a function declaration is parsed, an unlabelled direct
  `with` body is rejected with `ERRsyntax`. The labelled form continues to
  use the existing specific `ERRStmtOfWithIsLabelledFunc` diagnostic.
- No new parser member or AST field is retained; catch names are
  arena-allocated and consumed at the body's close.

## Evidence

Pinned test262 `2e0a5676…`, `-nonative -Test262 -CollectGarbage`,
test binary unless noted:

| Corpus | Before | After | Fail→pass | Pass→fail | Remaining parse negatives |
|---|---:|---:|---:|---:|---:|
| `statements/for` (758 scenarios) | 736 | **740** | 4 | 0 | 0 |
| `statements/switch` (216) | 129 | **188** | 59 | 0 | 0 |
| `statements/try` (388) | 355 | **359** | 4 | 0 | 0 |
| `statements/with` (182) | 169 | **170** | 1 | 0 | 0 |
| **total** | 1,389 | **1,457** | **68** | **0** | **0** |

The table's "after" columns are `out/test262-c2-s14-{for,switch,try,with}-final`,
all produced by the final test binary. Debug-static reruns match those
counts (`for` 740, `switch` 188, `try` 359, `with` 170); block-scope remains
287/287. No remaining failure in these directories is a parse-phase
negative. Some positive tests still fail with a pre-existing `SyntaxError`
(`let` ASI block/identifier cases in `for`/`with`) and others are
completion/TCO/runtime implementation gaps; none is introduced here.

Focused `test/es7/blockscope-early-errors.js` (already registered) now
covers for-head (including direct non-block, for-await and destructuring
forms), switch, catch (including a destructuring default) and with cases
plus valid siblings. The
legacy `test/LetConst/for-loop.js` `test3` case was made conforming by
moving its same-name `var` out of the for body (baseline unchanged). The
old same-name `let`/`var` collision is now a parse error, so its original
`EnsureScopeSlot` pressure cannot be retained; the remaining `v2` probe is
a function-scope `var` and is not claimed to cover that path.

`test/LetConst/for-loop.js` and the three `test/LetConst` files from the
block-scope batch remain green. `test/Function/LabelFuncAsWithStmt.js`
still reports its specific diagnostic.

## Gates

- `mise exec -- zig build`: pass.
- `mise exec -- zig build check --summary line`: 1976/0, bytecode up to
  date, 572/572 steps (no test file was added, so the count is unchanged).
- `mise exec -- zig build check-regression -Dprofile=debug-static
  --summary line`: 2028/0, 545/545 steps.
- `python3 tools/build/check_regression.py --self-test`: all negative
  controls behave.

## Residual S14 stream

- Catch pattern BoundNames vs `var` (for example
  `catch({e}) { var e; }`, including `for(var e…)`/nested-block forms)
  remain accepted; Node rejects. Pattern-vs-lexical/function cases are
  rejected (partly by existing scope machinery); only pattern-vs-`var` is
  the confirmed residual. The no-AST parse path collects only a simple
  identifier parameter; no reproducer for a missed pattern lexical error
  was found, and it is recorded as latent.
- For-head/switch/catch completion-value and TCO failures in the corpora
  are runtime issues, not parse negatives. Pre-existing positive `let` ASI
  failures are separate parser gaps.
- `eval`-introduced var conflicts (`{ let f; eval('var f'); }`) remain.
- The broader 3,822-case inherited-defect bucket still needs cluster
  triage.

## B4 note

Parser-only; no bytecode or runtime state changed, and bytecode
verification is unchanged. Execution evidence is Mac no-JIT only; B4 owns
JIT-configuration claims.
