# A2: direct-eval caller JIT fidelity (S14 EvalDeclarationInstantiation)

Status: **in progress** (gap analysis complete, increment landed, verification
below). Lane objective from
[arm64-macos-jit.md](arm64-macos-jit.md)'s coverage limits: make JIT frames
materialize what the baseline S14 eval machinery reads so direct-eval callers
can JIT, or land the largest correct increment with an honest boundary.

Base: `modernize/baseline` `270ed4adc` (gates 2038 test / 2090 debug-static,
`zig build -Dprofile=jit check` green).

## 1. What the guards did

`841c3fa6d` (widening `5e926f62d`'s param-scope abort) added two aborts in
`lib/Backend/IRBuilder.cpp`:

- `IRBuilder::Build()`: `IsEval()` function bodies (eval script functions)
  abort codegen — **eval-generated code stays interpreted**.
- `IRBuilder::BuildCallI_Helper`: any direct-eval call site
  (`CallFlags_Eval`, `CallFlags_ExtraArg`, or the converted `CallIEval`
  opcode) aborts codegen — **functions containing direct eval stay
  interpreted**. `1cc618e87` re-added the `CallsEval` PerfHint at that abort
  site.

Recorded failure repros: `'var arguments'` in a parameter default silently
succeeded from JIT'd code, and indirect eval after global block-scope `let`
declarations falsely reported redeclaration when eval code ran JIT-compiled.

## 2. What the S14 eval machinery actually reads

Inventory (interpreter = oracle):

1. **`GlobalObject::VEval`'s stack walk** (the eval *entry*): the first
   script caller frame supplies `evalInParamScope` /
   `evalInArrowParamScope` from `InterpreterStackFrame::IsParamScopeDone`
   (true once `OP_BeginBodyScope` ran, or at entry for merged param/body
   scopes), plus `evalCallerParameterArguments`, home-object and class-field
   context from the caller's `FunctionInfo`/`FunctionBody` — those are
   frame-independent metadata.
2. **The caller's frame display** (passed as the direct-eval ExtraArg and
   built by `LdFrameDisplay`/`NewStackFrameDisplay` from scope objects):
   walked by `OP_ScopedEnsureNoRedeclProperty` (the `arguments`
   parameter-environment and lexical-block checks read
   `FrameDisplay::GetScopeType`, `ActivationObject` /
   `BlockActivationObject` / `PseudoActivationObject`, type-handler
   `HasProperty(..., &noRedecl)`), by `OP_ScopedEnsureCanDeclGloFunc`, and by
   the scoped init/stores. In a JIT'd caller these scope objects are heap
   objects created by the same runtime ops the interpreter executes.
3. **The eval body itself** when eval code is JIT-compiled: the eval
   script function's own S14 scoped checks (`ScopedEnsureNoRedeclFld` walks,
   global-lexical preflights).

## 3. Gap analysis (experiments on the jit binary, dynapogo flags)

Both aborts were bypassed via a temporary config flag and the recorded
cluster re-run (`LetConst/eval1`, `eval_fncdecl`, `funcDeclConflict`,
`strict/21.functionDeclaration`, `es6/blockscope-functionbinding`,
`es7/global-eval-declarations`, `es7/eval-arguments-declarations`,
`es7/s14-eval-nonsimple-var`): **10 rlexe failures**, splitting cleanly:

- **Eval code JIT'd** (the `IsEval()` abort removed): false
  `SyntaxError: Let/Const redeclaration` in `eval1`/`eval_fncdecl`/
  `funcDeclConflict`/`blockscope-functionbinding`/`global-eval-declarations`,
  a wrong `CanDeclareGlobalFunction` TypeError, and a **SIGSEGV**
  (`strict/21.functionDeclaration.js`, exit −11). A separate, deeper surface.
- **Direct-eval caller JIT'd only** (eval code kept interpreted): every file
  above passes except `es7/eval-arguments-declarations.js`, whose 7 failing
  subtests all reduce to the **param-scope determination** (read #1 above):

  - **Native frame** (no bailout): the walker's
    `GetCurrentInterpreterFrame()` is `nullptr`, so `evalInParamScope`
    silently becomes false. `var arguments` in a parameter default then
    targets the body var environment and the required `SyntaxError` is
    skipped (probe A: `NO-THROW`); the binding the eval creates lands
    nowhere the body closure can see (probe G: `ReferenceError` on
    `delete arguments` in an arrow body).
  - **Bailout frame**: on this port a JIT'd `try/catch` caller bails to the
    interpreter EH; bailout frame construction does not run
    `InitializeClosures`, so a frame that bailed out in the body region
    keeps the reset `isParamScopeDone=false`. The walker adopts that frame
    and `evalInParamScope` becomes wrongly true: the eval's param-scope
    branch then inspects and pollutes the **catch scope** instead of
    walking to the body scope (probe C: missing SyntaxError; probe F:
    `arguments` created on the catch scope, parameter binding unchanged).

  The frame displays themselves were faithful in every probed shape
  (`ScopeType`, block/catch/plain activation ordering, noRedecl flags all
  matched the interpreter): probes D/E (blocks, arrow param-scope writes)
  already returned interpreter-identical values from a JIT'd caller.

**Minimal fidelity gap set for direct-eval callers = exactly the
`IsParamScopeDone` stack-walk read.** The 5e926f62d-era belief that the
JIT display "does not carry the parameter scope" was mis-attributed: the
param scope object is in the display; what was missing was the
param-scope *verdict* for frames the walker cannot interrogate.

## 4. Probe family (failing-when-JIT before the increment)

Scratch scripts (kept inline here; interpreter oracle in parentheses):

| Probe | Shape | Before (JIT caller) | After |
|---|---|---|---|
| A | `function f(p = eval("var arguments")) {}` | NO-THROW (SyntaxError) | SyntaxError |
| B | `{ let x; } (0,eval)("var x")` | NO-THROW, eval code JIT'd too — the original false-redeclaration repro needs the fuller test shapes above | NO-THROW |
| C | `function f() { let arguments = 1; try { throw 1; } catch (e) { eval("var arguments"); } }` | NO-THROW (SyntaxError) | SyntaxError |
| D | `function f2(arguments) { { { eval("var arguments = 'b'"); } } return arguments; }` | `b` (passed even before) | `b` |
| E | `(p = eval("var arguments = 'p'")) => { arguments = "q"; return arguments; }` | `q` (passed even before) | `q` |
| F | `function f1(arguments) { try { throw 1; } catch (e) { eval("var arguments = 'b'"); } return arguments; }` | `a` (`b`) | `b` |
| G | `var f4 = (p = eval("var arguments = 'p'")) => { return [delete arguments, arguments]; }` | ReferenceError (`false,p`) | `false,p` |

Probes were verified under default, `-forceNative -off:simpleJit
-bgJitDelay:0` (dynapogo), `-maxInterpretCount:1 -maxSimpleJitRunCount:1
-bgjit-` (interpreted variant) and `-mic:1 -bgjit-` on the jit binary; the
same flags with no JIT flags are the oracle.

## 5. The increment and its boundary

Two coordinated changes (commit (b)):

- `IRBuilder::BuildCallI_Helper`: the direct-eval abort now fires only for
  a call site with `!IsParamScopeDone()` (a split-scope function's
  parameter region — `m_paramScopeDone` is set at `Build()` start for
  merged scopes and loop bodies, and at `BeginBodyScope` otherwise) or for
  a loop-body work item (`IsLoopBody()`; the stack walker skips loop-body
  frames, so `VEval` would attribute the eval to the wrong caller).
  **Body-region direct-eval callers now JIT.**
- `GlobalObject::VEval`: `IsParamScopeDone` is trusted only from a live,
  non-bailout interpreter frame. A native frame has no interpreter frame
  and a bailout frame's flag is only correct when the bailout resumed in
  the parameter region, so both are treated as *param scope done*. This is
  sound by the abort policy: a param-region direct-eval site can never
  appear in JIT'd code (full-function builds, loop bodies, and per-inlinee
  `IRBuilder` builds all abort), so any native or bailout frame reaching a
  direct-eval call is in the body region. A bailout frame that resumes in
  the parameter region of a (body-only-eval) function executes
  `OP_BeginBodyScope` before any body eval, keeping the live-frame path
  correct.

**Boundary (unchanged on purpose):** eval script functions (eval-generated
code) stay interpreted — the `IsEval()` abort remains; JIT'ing eval bodies
is a separate surface (false redeclarations plus the crash above, root-caused
to eval-code-side walks, not caller frames). Direct-eval call sites in a
split parameter region and in loop bodies stay interpreted.

## 6. Ownership and lifetime record

No new allocation, retained pointer, root, or transfer point. The `VEval`
edit reads an existing flag (`IsParamScopeDone`) through the existing
stack-walker borrow of the caller frame — the same borrow, scope and
lifetime the surrounding code already uses (`IsParamScopeDone` was already
read there). The frame display, scope objects, and eval environment remain
owned by the caller frame and the recycler; nothing is copied or retained
past the eval call. The IRBuilder edit only narrows an existing
`OperationAbortedException`. Invalidation events (GC, reentrancy through
the eval itself, exception unwind) see identical lifetimes to before. The
five questions of [memory-management.md](memory-management.md) were applied;
there is no new resource to record.

(Verification tables, coverage evidence and gates: appended in commit (c).)
