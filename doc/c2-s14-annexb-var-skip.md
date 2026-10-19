# C2-S14 P3: Annex B block-function var-extension skip

Status: **landed in `0a723970e`** (feature `8731bad6d`; base `b50fa4d94`).
Interpreter/Mac no-JIT verified; JIT evidence is not claimed (see B4).
Current integrated gates **2004/2056**. Targeted Annex B selection **280/0**, full three directories **765/17**
(**282 fail→pass / 0 pass→fail**); the residual 17 are P4
declaration-instantiation collisions.

## Scope and spec rule

Annex B gives an ordinary sloppy block-level function declaration a synthetic
function-scoped `var` binding:
`B.3.2.1 Changes to FunctionDeclarationInstantiation`,
`B.3.3.1`/`B.3.3.2`/`B.3.3.3` (`GlobalDeclarationInstantiation`,
`EvalDeclarationInstantiation`) and the `IfStatement` production in B.3.3.
The extension is created only if replacing that one `FunctionDeclaration` with a
`VariableStatement` having the same name would not produce an early error in the
enclosing function body, script or eval body, and F is not in `paramNames`.
Generator, async and async-generator declarations are not `FunctionDeclaration`
and never receive it.

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
- The B.3.3.1 `paramNames` exclusion (`AnnexBVarExcludedByParameterNames`) is
  evaluated at finalize time, after the body is parsed. A formal (simple, rest
  or destructured) with the same name excludes the extension. For `arguments`
  it follows `argumentsObjNeeded`: arrows and a formal named `arguments` do not
  exclude; non-simple/rest parameter lists still need the arguments object; with
  simple parameters a top-level body function (ordinary, generator or async) or
  top-level lexical declaration named `arguments` disables the arguments
  binding, so the extension applies, while a body-level `var arguments` does
  not disable it. Both source orders are covered.
- When the enclosing function body (or the global/eval var-scope block) closes,
  `FinalizeAnnexBPendingVars` creates the synthetic `ParseNodeVar`s for
  applicable entries, in source order, and marks them
  `isBlockScopeFncDeclVar`. It also sets the per-declaration AST flag
  `ParseNodeFnc::isAnnexBVarExtension`; skipped/conflicting/excluded
  declarations leave it false.
- `ByteCodeEmitter::EmitAssignmentToFuncName` now emits the synthetic
  function-scope store only when that AST flag is set. This matters because
  `Symbol::GetFuncScopeVarSym` can find an unrelated same-named body-scope var,
  parameter or `arguments` symbol; a skipped declaration must not update it.
  Ordinary function declarations keep their existing emission path (the flag is
  false and `GetFuncScopeVarSym` is null for non-block declarations).
- The flag lives on the declaration node, not in `ScopeInfo`. Deferred bodies
  reparse their declarations and run the same parser decision before emission;
  a deferred block-function declaration retains its node and flag when its body
  is later undeferred. Fast-scan, `-force:deferparse`, deferred-scope restore
  and the legacy cached-scope/serialized variants were exercised by the runs
  below.
- `FinishParseBlock` runs the finalize before symbol binding. The deferred-scope
  path (`ParseType_Deferred` with restored `ScopeInfo`) also runs it before
  `FinishScopeInfo`. Fast-scanned deferred bodies use the same
  `FinishParseBlock` hook. The saved `m_ppnodeVar` tail is advanced only when it
  is a link in the current function's var list, so the subsequent
  `ParseFncDeclInternal` list termination does not drop the appended nodes.
- The only emitter change is the flag gate above; no other bytecode or runtime
  instantiation code changed. Parser state lives in the parse arena and is not
  retained.

## Evidence

Pinned corpus `2e0a56762801e275a9fdf96dc49d90ba0cddcf63`, flags
`-nonative -Test262 -CollectGarbage` unless noted. "Before" is the base
`b50fa4d94` binary copied from the main checkout
(`ch` sha256 `567d4ee004e945c9b40a39f690b0cc2c28787dd2e287ae5be4cdcedff6e10209`,
dylib `27e9fe54c06ec2f23d898771aba89d9c83894084a4d0cccb73fb2ca4c73588f6`);
"after" is the local test-profile binary (`ch`
`d8cfc255702ed0683efb2a96f371f1be7923d5c2c75181396123a5abf866981d`, dylib
`a025b30a0f1f0930540fd74b6b1ec076a5b45d8b75d482941094d2f554c6a9ec`). Every command was run under
`/tmp/opencode/cc-gate-lock.sh` with `--checkout "$PWD/out/test262"` and a local
`out/` directory; no `--prepare` and no full `zig build check`.

P3 selection
(`test/annexB/language/{eval-code,function-code,global-code}/**/*skip-early-err*.js`):

| Run | Passed | Failed | Fail→pass | Pass→fail |
|---|---:|---:|---:|---:|
| before, `out/test262-p3-before` | 16 | 264 | — | — |
| after, `out/test262-final-selection` | **280** | **0** | **264** | **0** |
| after with `-force:deferparse`, `out/test262-final-selection-defer` | **280** | **0** | — | — |

Full three Annex B directories (782 scenarios, 781 files):

| Run | Passed | Failed | Fail→pass | Pass→fail |
|---|---:|---:|---:|---:|
| before, `out/test262-p3-dirs-before` | 483 | 299 | — | — |
| after, `out/test262-final-dirs` | **765** | **17** | **282** | **0** |
| after with `-force:deferparse`, `out/test262-final-dirs-defer` | **765** | **17** | — | — |

The 17 remaining failures are all P4 declaration-instantiation collisions:
16 eval `existing-global-init`/`existing-non-enumerable-global-init` cases and
`global-code/script-decl-lex-collision.js`. The 17 param/dft-arg/arguments
scenarios and `block-decl-nested-blocks-with-fun-decl.js` now pass. None of the
remaining failures was passing before (pass→fail is zero in both the selection
and the full directories).

Controls:

| Corpus | Before | After |
|---|---:|---:|
| `test/language/block-scope/**/*.js` (287 scenarios, 145 files) | 287/0 | **287/0** (`out/test262-final-blockscope`) |
| same, `-force:deferparse` (`out/test262-final-blockscope-defer`, final binary) | — | **287/0** |
| `scope-lex-{generator,async-function,async-generator}.js` | 3/3 (3 failures) | **6/0** (3 fail→pass; `out/test262-final-switch`) |

The three switch tests are the extra controls requested by the triage:
generator/async/async-generator block functions no longer receive an ordinary
function extension.

Focused tests:

- New `test/es7/annexb-block-function-var.js`, registered in
  `test/es7/rlexe.xml` with `-args summary -endargs`: **pass**, also with
  `-force:deferparse`. It covers function/global code and direct/indirect eval,
  block/for/for-in/for-of/switch/catch-pattern skips, later lexical names
  (same-block and class cases), nested/sibling hoisting controls, A/B nested
  conflict in both call orders (test262's 1/1), same-named `var`/function/
  parameter/rest/default/`arguments` gating (including body-level
  ordinary/generator/async function `arguments` in both orders), generator/
  async/async-generator controls, strict mode, and valid ordinary hoisting
  (duplicate functions, `var`, top-level function, eval/global).
- `python3 test/runtests.py -b "$PWD/zig-out/bin/ch" -t
  --variants=disable_jit test/es7`: **54 passed / 0 failed**, including
  `test/es7/blockscope-early-errors.js`.
- Legacy regression suite run directly, not through `zig build check`:
  `python3 test/runtests.py -b "$PWD/zig-out/bin/ch" -t
  --variants=disable_jit`: **1989 passed / 0 failed**. The extra test is this
  batch's new file; integration owns gate pin counts.

Deliberate legacy expectation updates (non-conforming old Annex B behavior):
`test/es6/blockscope-functionbinding.js` Tests 2 and 3 referenced the block
function name outside its lexical block; those references now throw
`ReferenceError` and the test catches them, with four `ReferenceError` lines in
the baseline. Test 5's inner extension is now skipped, so its final print is the
outer block function instead of the inner one; the section name/comment and
baseline line were updated. `test/es6/default.js` f14 and
`test/es6/default-splitscope.js` f25 expected a block function named
`arguments` to replace the built-in arguments binding. Per B.3.3.1 that binding
is in `paramNames`, so the extension is skipped; those assertions now check that
`arguments` remains the argument object and is not callable. All other legacy
cases remain unchanged.

## Memory ownership record and review receipt

```text
Resource / creation: AnnexBPendingVar and BlockInfoName entries, Anew() on
  Parser::m_nodeAllocator (lib/Parser/Parse.cpp).
Domain/allocator: Parser arena for one parse job (including deferred/background
  parse); backing page allocator owned by the script context/thread context.
Physical base/extent/alignment/release: arena blocks; released by existing
  parser arena teardown, not by this code.
Logical length/views: linked pending list plus per-name map; borrowed only
  during parsing, never stored in bytecode/ScopeInfo. The surviving decision is
  one bool on the declaration ParseNodeFnc (arena lifetime).
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
  483→765 with 282 fail→pass and 0 pass→fail (17 P4 residual); block-scope
  287/287; switch controls 6/0; es7 54/0; legacy 1989/0.
Remaining uncertainty: no sanitizer, JIT, concurrency or OOM-injection run; the
  AST flag is not a serialized field (it is recomputed on reparse and retained
  on an undeferred declaration node).
```

Review checked the before/after-declaration cases (lexical before and after the
block function), duplicate ordinary functions in the same vs an enclosing
source block (both A/B source orders), sibling-block isolation, formal/rest/
default/`arguments` exclusion, generator/async exclusion, simple-vs-pattern
catch, eval/global top-level lexicals, deferred fast scan vs deferred reparse,
the `m_ppnodeVar` tail advance, and the emitter flag gate for same-named
`var`/function/parameter/`arguments` bindings. A first draft that deferred the
nodes without advancing that tail lost the synthetic binding and crashed valid
hoisting (`test/es6/blockscope-functionbinding.js`); the final code advances it
only when the saved tail belongs to the current function, and the legacy suite
and the full Annex B directories pass.

## Residual and risks

- No P3-selection residual remains. Full three-directory residual is 17 P4
  scenarios: 16 eval `existing-global-init`/
  `existing-non-enumerable-global-init` cases and
  `global-code/script-decl-lex-collision.js`. They are declaration-
  instantiation collisions outside this slice and unchanged.
- `EvalDeclarationInstantiation` runtime `bindingExists` checks against a
  caller lexical environment are runtime instantiation, not the parse-time
  early-error rule; not changed here.
- The AST flag is recomputed during deferred/scope-restored body parsing and
  retained on a declaration node whose body is later undeferred. It is not a
  field in `DeferredFunctionStub` or `ScopeInfo`; the force:deferparse corpus,
  legacy `-force:CachedScope` and serialized variants passed, but a dedicated
  bytecode-cache test for this flag was not added.
- Deliberate spec/test262 behavior over V8 in the arguments corner: a body-level
  `var arguments` and non-simple/rest parameter lists keep the arguments object
  in `parameterNames`, so a same-named block function does not replace it. A
  body-level ordinary/generator/async function `arguments` disables
  `argumentsObjNeeded`, so the extension applies. Focused tests pin both.
- Legacy expectations updated deliberately as listed above; no other source
  changes were made to those files.

## B4 note

Parser/declaration-instantiation plus one bytecode-emitter gate; no GC, JIT or
runtime-instantiation code changed. Execution evidence is Mac no-JIT (test
profile and `-force:deferparse`), plus the legacy disable-JIT suite. B4 still
owns any JIT-configuration claim.
