# C2-S1: class static initialization blocks

Status: **implemented on `lane/s1-static-blocks`** (base `0c01875b5`);
gates hold **2012/0 (test) and 2064/0 (debug-static)** with no
scenario-count movement. This closes the shared static-block residual
noted in [c2-s1-static-fields.md](c2-s1-static-fields.md),
[c2-s1-super-static-init.md](c2-s1-super-static-init.md),
[c2-s1-private-static-methods.md](c2-s1-private-static-methods.md) and
[c2-s1-static-private-fields.md](c2-s1-static-private-fields.md).
Computed static names, decorators, direct-eval private names and JIT remain
out of scope.

## Scope and observable semantics

- `static { ... }` (ClassStaticBlock) is supported in class declarations and
  class expressions, base and derived, anonymous and named, any number of
  blocks per class.
- Blocks evaluate in **textual order interleaved with static field
  initializers** (public and private share one order): a block before a field
  observes it as absent; a field before a block observes the block's writes.
- Class definition first evaluates and installs **all** methods, accessors
  and computed names in textual order (including private methods/accessors,
  instance and static), initializes the class-name binding, and only then
  runs static fields and blocks. A block textually before a method still
  observes it (`typeof this.m === "function"`, `new C().#m()`, `this.#sm()`).
- `this` inside a block is the constructor being defined; arrows capture it.
  Blocks can access **every private name declared in the class** (static and
  instance fields, methods, accessors), including installing closures that
  later use instance private names (friend pattern) and `#x in obj` checks.
  Forward private references parse but throw `TypeError` at runtime (brand
  absent until its declaration runs), matching static fields. Cross-class
  private uses are `SyntaxError` (lexical scoping).
- `super.prop` / `super[foo]` reads, calls (including from arrows) and
  assignments work with `[[HomeObject]]` = the constructor, reusing the
  static-super machinery (`isStaticClassFieldInitializer` +
  `LoadSuperObject` from `this`); base classes resolve through
  `Function.prototype` (`undefined`); `extends null` likewise. `super()`
  stays an early `SyntaxError`. Nested plain functions cannot use `super`
  (early error in the full parse; see residuals for scan/eval behavior).
- The block has a **function-like own scope**: `var`/`function`
  declarations are block-local (no leak onto the constructor, scope, or
  outer code); `let`/`const` are block-local; duplicate `var`/`function`
  is allowed, duplicate lexicals and lexical/`var` collisions are early
  errors; each block gets its own scope.
- Early errors: direct `arguments` (including through arrows — the
  `ContainsArguments` machinery), `await`/`yield` expressions in
  async/generator shapes, `return` at block level (even inside a nested
  block/loop/branch; nested functions keep their own returns), bare
  `break`/`continue`, and `break`/`continue` targeting labels outside the
  block (`Undefined label`). Inner loops/labels and nested-function returns
  are valid. `new.target` evaluates to `undefined` (no error).
- Exceptions propagate out of the class definition; later static elements
  do not run; the class binding stays uninitialized.
- `static {}` (empty), `static {};` (semicolon adjacency) and `static`
  followed by a newline and `{` are all valid blocks.

## Parser/AST design

- The `static` + `{` look-ahead in `ParseClassDecl` now parses a static
  block instead of erroring. The body is parsed into a hidden block-scoped
  helper function created by the new `CreateClassStaticBlockFunction`
  (mirrors `CreateClassFieldInitializerFunction`): strict mode, no AST
  `this` override, `superRestrictionState = PropertyAllowed`,
  `isStaticClassFieldInitializer = true` (reuses the static-super stub and
  `LoadSuperObject` paths), `isClassFieldInitializerContext = true`
  (keeps the `arguments`/eval-context machinery), plus a new
  `isClassStaticBlock` marker. The helper range starts at the `static`
  keyword so it strictly contains nested ranges for deferred-reparse keying.
- The body is a `ParseStmtList` in `SM_OnFunctionCode` (full parse) /
  `SM_DeferredParse` (fast scan), not a single expression. `await`/`yield`
  regions are forced off exactly like field initializers, so invalid uses
  are `SyntaxError`s rather than unemittable nodes. `m_pstmtCur` is reset
  before the helper's blocks are pushed (full parse, matching
  `ParseFncDeclHelper`) or replaced by a deferred `knopBlock` sentinel
  (fast scan), so outer labels/loops are invisible from the block while
  `ParseFncDeclCheckScope` and declaration checks still see a block.
- `return` at helper level is rejected by a new `tkRETURN` early error:
  full parses check the helper's `isClassStaticBlock` marker (nested
  functions rebind `m_currentNodeFunc`, so they keep their returns); scans
  check a new `m_inClassStaticBlock` flag, which `ParseFncDeclInternal`
  resets on every nested function entry (save/restore) because scan bodies
  may be statement-parsed when the enclosing function is deferred for a
  large array literal.
- Fast scans reserve one `IsGeneratedDefault` stub slot per block (reusing
  `CreateClassFieldInitializerFastScanStub`) and parent discovered children
  to it, so nested counts match the real parse; `arguments`/`super`/`return`
  early errors fire eagerly there just like for fields.
- Member entries are `knopMember` with `pnode1 == nullptr` and the helper
  call in `pnode2`, appended to `pnodeClass->pnodeMembers` in textual order.
  `ClassBodyHasFieldSyntax` additionally recognizes `static {` so
  block-only classes keep eager constructors.
- Three `nullptr`-name hazards from the new member shape are guarded:
  `Bind`'s `knopMember` case, the generic `Visit` bin path via a null-safe
  `ByteCodeGenerator::IsSuper`, and `IsStaticFieldMemberNode` (new
  `IsStaticBlockMemberNode` matches the nameless shape).

## Bytecode/runtime design

No opcode or PropertyId is added. Blocks reuse the static-initializer
helper-call path:

- `EmitClass` first pass skips field **and** block entries; the second pass
  runs `EmitStaticClassFieldDefinition` / the new
  `EmitStaticClassBlockDefinition` in shared textual order after the inner
  class-name binding is stored.
- `EmitStaticClassBlockDefinition` calls the helper with the constructor as
  explicit receiver (acquire-before-receiver-temp LIFO order, as for
  fields) and discards the result. Abrupt completion propagates before
  later static elements. `super` binds via the existing
  `isStaticClassFieldInitializer` prologue (`Ld_A` from `this`); private
  brand loads resolve through the class-block closure like field
  initializers.
- Helpers are ordinary eager nested functions in the class-block scope
  list, so binding/visiting/emission of the helper itself needs no new
  path; `Visit`'s class loop already binds `pnode2` calls regardless of
  name.

### Ownership/lifecycle record

- **Resource**: one hidden helper `ParseNodeFnc`/`FunctionBody` per block
  plus ordinary locals; no new native allocation, no new opcode.
- **Owner**: the class block owns helper functions and the class-name
  binding; the constructor owns properties the block defines. Helpers are
  published with the class block and remain reachable through its closure
  for the class lifetime.
- **Invalidation**: none after class evaluation. Ordinary object/recycler
  lifetime; no raw pointer, weak reference or manual release.
- **User-code/reentrancy**: block bodies run arbitrary JS with the
  constructor as receiver (calls, proxies, eval, mutation, throw). Abrupt
  completion prevents later static elements; already-defined state remains
  ordinary reachable objects.
- **Failure**: uncaught throw propagates out of class evaluation and leaves
  the class binding uninitialized. No partial native resource exists.
- **Thread/context**: current-thread script execution on the active
  `ThreadContext`; no new async job, callback, TLS or cross-thread access.

## Evidence

Builds (Mac ARM64, local `.zig-cache`):

- `mise x -- zig build --cache-dir "$PWD/.zig-cache"` — green (568/568
  steps), `s1block` UTF-16 marker present in `libChakraCore.dylib` and
  `libChakraCoreStatic.a`.
- `mise x -- zig build check --cache-dir "$PWD/.zig-cache"` — rc=0:
  `check_regression: PASS: passed 2012 / failed 0` (scenario totals
  unmoved — cases added to already-registered files);
  `verify-bytecode`: "Bytecode is up to date" (no new opcodes, checked-in
  headers unchanged); `check_native`, `check_smoke`, `check_metadata`
  all PASS.
- `mise x -- zig build check-regression -Dprofile=debug-static
  --cache-dir "$PWD/.zig-cache"` — **PASS: passed 2064 / failed 0**
  (`out/debug-static/bin/ch`). This caught one real bug the release
  binary hides: `ParseNodeBin`'s constructor asserted that every
  `knopMember` carries a name, so nameless block entries crashed debug
  builds at parse time. The assert now documents and permits the
  nameless-block shape (release builds never evaluated the shape
  predicate and were unaffected).

Local suites (`disable_jit`, release `zig-out/bin/ch` unless noted):

- `test/es7/class-static-fields.js` — **pass** (26 existing + 6 new block
  entries: ordering, `this`/methods-first, super, scope, early errors,
  base/derived/exceptions) in normal, `-force:deferparse` and
  `-nodeferparse` modes.
- `test/es7/class-static-private-fields.js` — **pass** (15 existing + 2
  new block entries: private access incl. friend pattern, lexical
  scope/brand timing) in all three modes.
- `test/es7/class-private-methods.js`, `class-private-fields.js`,
  `class-fields-public.js`, `deferparseclass.js` — pass in all three
  modes (only header-comment updates; one stale `static {}` rejection
  updated to `doesNotThrow` in each static file).

Node-differential probes (`/tmp/sb-probes.js`, 27 probes, release `ch`
vs `node v26.5.0`, error classes compared, values compared verbatim):
**26/27 identical**. The single diff is `await-ident`
(`class X { static { var a = await; } }` in a script): `ch` parses it
(await is an identifier when Await=false) and throws `ReferenceError` at
runtime; `node` throws `SyntaxError`. Per spec the program is valid
(ClassStaticBlockBody inherits Await=false in scripts) and the
`ReferenceError` is correct evaluation behavior — spec wins. Static
field initializers behave identically (`static x = await` is also
`ReferenceError`), so blocks match the engine's field handling.

Focused test262 subset: not run — the pinned corpus (`out/test262`) is
absent from this worktree (only `tools/test262/run.py` is present), so no
before/after corpus comparison was possible; in-tree suites plus the
node-differential probes above are the regression net, per the lane brief.

## Fix round (review loop 2): defer-scan scope attribution

Two adversarial-review defects, both in the fast-scan (`buildAST == false`)
branch of the `static {` member loop in `ParseClassDecl`. Reproduced before
changing code with the lane binaries (`zig-out/bin/ch`,
`out/debug-static/bin/ch`) using `-forcedeferparse` vs `-nodeferparse`
with `node v26.5.0` as oracle:

| defect | before | after |
|---|---|---|
| D1 `function f(){class C{static{let a=1;var a=2;}}return 1;} f();` (also reversed order, `const`/`var`) | full parse: `SyntaxError`; defer scan: missed, release `ch` throws `Error: Internal error` at the `f()` call, debug-static asserts at `FunctionBody.cpp:2519` (`Syntax errors should never be detected on deferred re-parse`), SIGTRAP rc=133 | `SyntaxError` (Let/Const redeclaration) in normal, `-forcedeferparse` and `-nodeferparse` modes on both binaries; debug build reports it at the same point the full parse does, no assert |
| D2a `function f(){class C{static{let C=5;}} return 1;} f();` | accepted by node and full-parse `ch`; `SyntaxError` under `-forcedeferparse` | accepted in all modes |
| D2b `function f(){let z=1; class C{static{var z=2; this.v=z;}} return C.v;} f();` | `node` and full-parse `ch` print `2`; `SyntaxError` under `-forcedeferparse` | prints `2` in all modes |

Root cause: the scan branch parsed the block statement list with no
`BlockInfoStack` pushed — only a `StmtNest` sentinel plus the flat
stub-child list. Every declaration-attribution path keys off
`GetCurrentBlockInfo()` / `GetCurrentFunctionBlockInfo()` and pid-ref scope
ids, so the block's `let`/`const` landed in the enclosing function-body
block and its `var`/`function` in the enclosing function scope. Same-scope
`let`+`var` therefore never shared a scope (D1 missed, then re-detected
when the deferred reparse built the real helper with full validation —
hence the Internal-error/assert), while block locals falsely collided with
the class binding (D2a) and outer `let`s (D2b). The full parse is correct
because `CreateClassStaticBlockFunction` builds Parameter + FunctionBody
blocks for the helper; plain `{ }` blocks and method bodies are correct in
scans for the same reason (real `BlockInfoStack`s via `ParseBlock<false>`
and `ParseFncDeclInternal<false>`).

Fix (`lib/Parser/Parse.cpp`, scan branch only): bottom out the statement
chain as before (outer labels/loops stay invisible), then wrap the
statement list in a real `StartParseBlock<false>(PnodeBlockType::Function,
ScopeType_Block)` / `FinishParseBlock` pair, keeping the existing
stub-child redirection. `Function` block type makes the block its own
`pBlockInfoFunction`, so `var`/hoisted-function declarations stay
block-local and collide with block-level `let`/`const` exactly like the
helper; nested regular blocks inherit it as their function block, and
Annex B maps, `CheckBlockDeclarations` and `BindPidRefs` run at close like
any scanned block. `ScopeType_Block` (not `FunctionBody`) keeps the
split-param-scope check aimed at the enclosing function away from block
locals, matching the parameterless helper — verified with
`([p]) => { class SP { static { let p = 4; ... } } }`. The scope object is
attached up front because non-regular blocks never lazily create one
(`AddDeclForPid` asserts `Regular` on the lazy path; debug caught this on
the first iteration). No special cases per repro: every shape in both
defect families falls out of the one scope.

Tests: new `static block declaration scope attribution agrees under
deferred parsing` entry in `test/es7/class-static-fields.js` (no new files,
no `rlexe.xml` change, gate counts unmoved). Must-error shapes are pinned
via `eval`/`new Function` (eager full parse); must-pass shapes also run as
direct code, so this file's existing `-force:deferparse` `rlexe.xml`
variant scans them with the fixed scope in CI — pre-fix that variant
rejects them at parse time. Must-error shapes cannot appear as direct
committed code (a file that fails to parse fails the suite), so the D1
scan direction is pinned by the manual runs below, per repo convention
(same as `blockscope-early-errors.js`).

Verification observed (release `zig-out/bin/ch` unless noted):

- Focused files pass in all three modes on both binaries:
  `class-static-fields.js` (with the new entry), plus
  `class-static-private-fields.js`, `class-private-methods.js`,
  `class-private-fields.js`, `class-fields-public.js`,
  `deferparseclass.js` — `pass` in normal / `-forcedeferparse` /
  `-nodeferparse`.
- Node-differential sweeps, 69 probes total, `ch` normal/defer/nodefer vs
  `node`: **69/69 identical error class, identical values on every
  value case**. Covers let/const/var/function collisions in both orders,
  nested blocks/classes/functions, blocks in nested deferred functions
  (incl. 3-deep), class-name shadowing (let/const/var/function),
  outer let/const/var coexistence, split-param enclosing functions,
  return/break/continue/label inward+outward, and the lane's prior probes
  (ordering, private access incl. friend pattern, super, scope,
  exceptions, arguments error, this/arrow, eval, empty blocks).
- Gates: `mise x -- zig build check` PASS (incl. `check_regression:
  PASS: passed 2012 / failed 0`); `mise x -- zig build
  check-regression -Dprofile=debug-static` **PASS: passed 2064 /
  failed 0**. `tools/build/gate_counts.json` untouched.

Observation (not changed, for the coordinator): the S1 `sawClass`
eager-guard in `ParseFncDeclInternal` does not keep small class-bearing
functions eager under `-forcedeferparse` (`-trace:deferparse` still shows
`Deferred : f` for `function f(){class C{m(){}} return 1;} f();`), so the
scan path fixed here is live for ordinary class code and must stay
correct; the guard's effectiveness was left as-is.

## Residual work and limitations

- `super.prop` inside a **nested plain function** in a block is an early
  error in the full parse, but shares the field-initializer residuals
  elsewhere: direct eval defers to runtime (fires only if called), and
  `-force:deferparse` skips nested bodies (upfront scan) while the deferred
  reparse skips super validation (`ParseType_Deferred`), so an uncalled
  nested function never reports. Verified identical for field
  initializers; committed tests assert only shapes that throw in all
  modes, probes cover the rest.
- Bare `await`/`yield` identifiers follow the field-initializer
  region-off treatment: `await 1` / `yield 1` are `SyntaxError` everywhere,
  but a bare `await` identifier parses (spec-valid in scripts) and fails
  at runtime instead of V8's early error. Identical for fields.
- Direct eval cannot resolve private names inside blocks (pre-existing
  residual shared with fields/methods).
- Computed static names and decorators keep their existing `SyntaxError`s.
- No JIT/backend support is claimed. Block helpers emit only pre-existing
  opcodes and helper calls, but **B4 must run the es7 static/private
  tests under SimpleJIT/FullJIT (dynapogo) before any JIT or default
  claim**, as in the prior S1 receipts.

## Files

- `lib/Parser/ptree.h`, `lib/Parser/ptree.cpp`: `isClassStaticBlock`
  marker on `ParseNodeFnc`, default false; `ParseNodeBin` constructor
  assert permits nameless `knopMember` block entries.
- `lib/Parser/Parse.h`, `lib/Parser/Parse.cpp`: `m_inClassStaticBlock`
  scan flag; `CreateClassStaticBlockFunction`; `static {` member-loop
  branch (AST + stub paths, flag save/restore, nameless member entries; fix round: scan branch wraps the statement list
  in a synthetic Function-typed/Block-scoped scan block so declarations attribute like the helper);
  `tkRETURN` early error (marker + flag); `m_inClassStaticBlock`
  save/reset/restore in `ParseFncDeclInternal`; `static {` in
  `ClassBodyHasFieldSyntax`.
- `lib/Runtime/ByteCode/ByteCodeEmitter.cpp`: `IsStaticBlockMemberNode`,
  `nullptr` guard in `IsStaticFieldMemberNode`, first-pass skip,
  interleaved second pass, `EmitStaticClassBlockDefinition`.
- `lib/Runtime/ByteCode/ByteCodeGenerator.h`: `EmitStaticClassBlockDefinition`
  declaration.
- `lib/Runtime/ByteCode/ByteCodeGenerator.cpp`: `nullptr` guards in `Bind`'s
  `knopMember` case and `IsSuper`.
- `test/es7/class-static-fields.js`: stale `static {}` rejection updated;
  6 new block entries plus the fix-round scope-attribution entry. `test/es7/class-static-private-fields.js`: stale
  rejection updated; 2 new block entries. `test/es7/class-private-methods.js`:
  header comment only. No new test files; `rlexe.xml` and
  `tools/build/gate_counts.json` unmoved (2012/2064).
