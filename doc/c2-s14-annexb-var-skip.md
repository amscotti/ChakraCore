# C2-S14 P3: Annex B block-function var-extension skip

Status: **`<pending>`** — follow-up draft commit on `s14-p3` on top of `afd8aef9e`.
Interpreter/Mac no-JIT verified; JIT evidence is not claimed (see B4).
Integration owns the shared plan/triage update and gate counts.

## Scope and spec rule

Annex B gives an ordinary sloppy block-level function declaration a synthetic
function-scoped `var` binding:
`B.3.2.1 Changes to FunctionDeclarationInstantiation`,
`B.3.3.1`/`B.3.3.2`/`B.3.3.3` (`GlobalDeclarationInstantiation`,
`EvalDeclarationInstantiation`) and the `IfStatement` production in B.3.3.
The extension is created only if replacing that one `FunctionDeclaration` with a
`VariableStatement` having the same name would not produce an early error in the
enclosing function body, script or eval body. Generator, async and
async-generator declarations are not `FunctionDeclaration` and never receive it.

The engine previously created the synthetic `ParseNodeVar` unconditionally for
every sloppy function declaration whose current block was `Regular`. That made
the function-scoped binding observable (initialized to `undefined`, then updated
when the block executed) even when a lexical name in an enclosing block, loop
head, switch case, catch pattern, function body, global scope or eval scope made
the replacement an early error. Eval global code additionally raised
`SyntaxError: Let/Const redeclaration` when the synthetic var met a top-level
eval `let`/`const`.

## Parser design (`lib/Parser/Parse.cpp`, `Parse.h` only)

- `ParseFncDeclInternal` no longer creates the synthetic var node immediately.
  For a sloppy ordinary (not generator/async) function declaration in a
  `Regular` block it records an `AnnexBPendingVar` on the enclosing function
  block's pending list/map. The record holds the name, owner function node,
  declaration block id and source extent.
- Conflict names are tracked parser-only:
  - source blocks keep the existing `lexicalNameMap` (let/const/class,
    function/class declarations). An ordinary sloppy function in the pending
    declaration's *own* source block is the Annex B duplicate exception; the
    same name in an *enclosing* source block stays lexical when the inner
    declaration is replaced with `var`, so descendants conflict;
  - function-body, global and global-eval blocks record let/const/class in
    `annexBConflictMap` (their top-level function declarations are var scoped);
  - a destructured catch parameter's bound names reach the catch parameter
    scope's `annexBConflictMap` through the existing `CreateBlockScopedDeclNode`
    path, so pattern catch parameters conflict while a simple catch parameter
    keeps the B.3.4 `var` compatibility;
  - for-head and switch blocks are already source blocks and participate through
    their lexical maps.
- At registration the parser walks the active block chain up to the enclosing
  function block and marks the pending entry if a conflict already exists.
  Enclosing source-block function declarations conflict unless the map is the
  pending declaration's own block. A lexical declaration parsed later calls
  `MarkAnnexBPendingConflicts`; block ids are assigned depth-first, so
  `pending.declBlockId >= current.blockId` is exactly "the pending declaration
  is inside this construct". A sloppy function declaration in the current
  construct only conflicts with descendants (`declBlockId > current`), not
  same-block duplicates. Sibling lexical names do not conflict.
- When the enclosing function body (or the global/eval var-scope block) closes,
  `FinalizeAnnexBPendingVars` creates the synthetic `ParseNodeVar`s for
  non-conflicting entries, in source order, and marks them
  `isBlockScopeFncDeclVar`. Conflicting entries are absent, so no synthetic
  function-scope symbol is created by the parser. `GetFuncScopeVarSym` still
  falls back to any same-named body/parameter/`arguments` symbol; that residual
  is described below.
- `FinishParseBlock` runs the finalize before symbol binding. The deferred-scope
  path (`ParseType_Deferred` with restored `ScopeInfo`) also runs it before
  `FinishScopeInfo`. Fast-scanned deferred bodies use the same
  `FinishParseBlock` hook. The saved `m_ppnodeVar` tail is advanced only when it
  is a link in the current function's var list, so the subsequent
  `ParseFncDeclInternal` list termination does not drop the appended nodes.
- No AST/deferred-stub payload or bytecode emitter code changed. The only
  bytecode difference is the intended absence/presence of the synthetic var
  binding. Parser state lives in the parse arena and is not retained.

## Evidence

Pinned corpus `2e0a56762801e275a9fdf96dc49d90ba0cddcf63`, flags
`-nonative -Test262 -CollectGarbage` unless noted. "Before" is the base
`b50fa4d94` binary copied from the main checkout
(`ch` sha256 `567d4ee004e945c9b40a39f690b0cc2c28787dd2e287ae5be4cdcedff6e10209`,
dylib `27e9fe54c06ec2f23d898771aba89d9c83894084a4d0cccb73fb2ca4c73588f6`);
"after" is the local test-profile binary (`ch`
`d8cfc255702ed0683efb2a96f371f1be7923d5c2c75181396123a5abf866981d`, dylib
`5db456964c516289e02f1eecd78547a0461a51cf421ab66c2213014abc75b574`). Every command was run under
`/tmp/opencode/cc-gate-lock.sh` with `--checkout "$PWD/out/test262"` and a local
`out/` directory; no `--prepare` and no full `zig build check`.

P3 selection
(`test/annexB/language/{eval-code,function-code,global-code}/**/*skip-early-err*.js`):

| Run | Passed | Failed | Fail→pass | Pass→fail |
|---|---:|---:|---:|---:|
| before, `out/test262-p3-before` | 16 | 264 | — | — |
| after, `out/test262-p3b-after` | **280** | **0** | **264** | **0** |
| after with `-force:deferparse`, `out/test262-p3b-after-defer` | **280** | **0** | — | — |

Full three Annex B directories (782 scenarios, 781 files):

| Run | Passed | Failed | Fail→pass | Pass→fail |
|---|---:|---:|---:|---:|
| before, `out/test262-p3-dirs-before` | 483 | 299 | — | — |
| after, `out/test262-p3b-dirs-after` | **747** | **35** | **264** | **0** |
| after with `-force:deferparse`, `out/test262-p3b-dirs-after-defer` | **747** | **35** | — | — |

The 35 remaining failures are: 16 eval
`existing-global-init`/`existing-non-enumerable-global-init` cases, 17
function-code `skip-param`/`skip-dft-param`/`skip-arguments` cases, one
`nested-blocks-with-fun-decl`, and
`global-code/script-decl-lex-collision.js`. None was passing before
(pass→fail is zero in both the selection and the full directories). The nested
case is now skipped by the parser too, but its runtime assertion still sees the
outer Annex B var through the emitter fallback (next section), so the
fail→pass count stays 264.

Controls:

| Corpus | Before | After |
|---|---:|---:|
| `test/language/block-scope/**/*.js` (287 scenarios, 145 files) | 287/0 | **287/0** (`out/test262-p3b-blockscope`) |
| same, `-force:deferparse` | — | **287/0** |
| `scope-lex-{generator,async-function,async-generator}.js` | 3/3 (3 failures) | **6/0** (3 fail→pass; `out/test262-p3b-switch`) |

The three switch tests are the extra controls requested by the triage:
generator/async/async-generator block functions no longer receive an ordinary
function extension.

Focused tests:

- New `test/es7/annexb-block-function-var.js`, registered in
  `test/es7/rlexe.xml` with `-args summary -endargs`: **pass**, also with
  `-force:deferparse`. It covers function/global code and direct/indirect eval,
  block/for/for-in/for-of/switch/catch-pattern skips, later lexical names
  (same-block and class cases), nested/sibling *valid* hoisting controls, valid
  ordinary hoisting (duplicate functions, `var`, top-level function,
  eval/global), strict mode, and generator/async/async-generator controls. It
  also pins the A/B nested-conflict residual: both call orders currently
  observe 2 (test262 expects 1) because of the emitter fallback below.
- `python3 test/runtests.py -b "$PWD/zig-out/bin/ch" -t
  --variants=disable_jit test/es7`: **54 passed / 0 failed**, including
  `test/es7/blockscope-early-errors.js`.
- Legacy regression suite run directly, not through `zig build check`:
  `python3 test/runtests.py -b "$PWD/zig-out/bin/ch" -t
  --variants=disable_jit`: **1989 passed / 0 failed**. The extra test is this
  batch's new file; integration owns gate pin counts.

Deliberate legacy expectation update: `test/es6/blockscope-functionbinding.js`
Tests 2 and 3 referenced the block function name outside its lexical block.
Those references now throw `ReferenceError` because the extension is correctly
skipped; the test catches the error and
`blockscope-functionbinding.baseline` records `ReferenceError` on those four
lines. Test 2/3/5 names and comments no longer claim a function-scope leak; Test
5 output is unchanged because its inner store still reaches the outer Annex B
var through the emitter fallback. `blockscope-functionbinding.js` and the
baseline header text were changed only for those names/comments and the four
`ReferenceError` expectations.

## Memory ownership record and review receipt

```text
Resource / creation: AnnexBPendingVar and BlockInfoName entries, Anew() on
  Parser::m_nodeAllocator (lib/Parser/Parse.cpp).
Domain/allocator: Parser arena for one parse job (including deferred/background
  parse); backing page allocator owned by the script context/thread context.
Physical base/extent/alignment/release: arena blocks; released by existing
  parser arena teardown, not by this code.
Logical length/views: linked pending list plus per-name map; borrowed only
  during parsing, never stored in AST/bytecode/ScopeInfo.
Owner/transfer: enclosing function's BlockInfoStack; transferred to no one.
  Finalize clears the owner list/map and converts survivors to ParseNodeVar.
Invalidation: arena teardown; finalize; block pops. No GC/detach/mutation
  callback can reach these records because no user code runs in this path.
Reentrancy/thread: parser thread only; deferred fast scan follows the existing
  single-parser background rules. No new async state.
Release on success/failure/OOM: arena teardown for all entries; each allocation
  uses the parser's existing OOM path. No partial native resource.
Accounting/bound: one pending record per sloppy ordinary block function, one
  per-name map entry per distinct name per function, one conflict-map entry per
  lexical name in a non-source lexical block. Not per enclosing block depth.
Tests: P3 selection 280/280 eager and force:deferparse; full three dirs
  483→747 with 264 fail→pass and 0 pass→fail (35 residual); block-scope
  287/287; switch controls 6/0; es7 54/0; legacy 1989/0.
Remaining uncertainty: no sanitizer, JIT, concurrency or OOM-injection run.
```

Review checked the before/after-declaration cases (lexical before and after the
block function), duplicate ordinary functions in the same vs an enclosing
source block (both A/B source orders), sibling-block isolation,
generator/async exclusion, simple-vs-pattern catch, eval/global top-level
lexicals, deferred fast scan vs deferred reparse, and the `m_ppnodeVar` tail
advance that keeps the appended nodes linked. A first draft that deferred the
nodes without advancing that tail lost the synthetic binding and crashed valid
hoisting (`test/es6/blockscope-functionbinding.js`); the final code advances it
only when the saved tail belongs to the current function, and the legacy suite
and the full Annex B directories pass.

## Residual and risks

- **Emitter fallback (in-scope but not changed here).** Annex B emission is
  still not gated by the parser's skip decision. `Symbol::GetFuncScopeVarSym`
  searches the enclosing function body scope and then its parameter scope for
  any same-named symbol, and `EmitAssignmentToFuncName` emits the store whenever
  that search succeeds. A same-named user `var`, an outer Annex B var, a formal
  parameter or `arguments` therefore keeps the emitted store observable even
  when the parser created no synthetic var node.
  - This is why the 17 function-code `skip-param`, `skip-dft-param` and
    `skip-arguments` scenarios still fail: the name is a formal or `arguments`
    and the parser-only skip cannot suppress the store.
  - It is also why `block-decl-nested-blocks-with-fun-decl.js` still observes
    `f() === 2` (test262 expects 1) and why the focused A/B probes both observe
    2: the parser now marks the inner declaration as conflicting (an ordinary
    sloppy function in an enclosing Block is lexical), but the outer block
    function's Annex B var is found by the fallback.
  - Fixing these needs a per-declaration AST flag set by the parser and consumed
    by the emitter (an emitter/AST gate), or an equivalent check in
    `GetFuncScopeVarSym`/`EmitAssignmentToFuncName`. This batch deliberately
    does not touch emitter/runtime code; the parser conflict decision is the
    prerequisite.
- `EvalDeclarationInstantiation` runtime `bindingExists` checks against a
  caller lexical environment are runtime instantiation, not the parse-time
  early-error rule; not changed here.
- The 16 eval `existing-global-init` failures and
  `global-code/script-decl-lex-collision.js` belong to the P4
  declaration-instantiation collision cluster.
- The source text of `test/es6/blockscope-functionbinding.js` was kept
  otherwise intact; Test 2/3 now catch the invalid outer references, the four
  baseline output lines changed deliberately, and Test 5's name/comment records
  the emitter-fallback residual (its output is unchanged).

## B4 note

Parser/declaration-instantiation only; no emitter, GC, JIT or runtime
instantiation code changed. Execution evidence is Mac no-JIT (test profile and
`-force:deferparse`), plus the legacy disable-JIT suite. B4 still owns any
JIT-configuration claim.
