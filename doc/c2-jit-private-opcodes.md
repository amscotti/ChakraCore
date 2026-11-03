# C2-A3: class/private/import opcode JIT lowering (helper-call port #3)

Recorded 2026-11-03, lane `a3-class-private-import-lowering` (mainline
`modernize/baseline`, base `b436f3807`). Objective: remove the
shared-IRBuilder whole-function codegen aborts for the S1
class-fields/private-name and S12 import-attributes opcode family so
containing functions JIT, using the interpreter's own runtime
implementations as helper calls.

## 1. Enumeration (18 opcodes + 3 env-path aborts)

Abort site at baseline: `lib/Backend/IRBuilder.cpp:710-739` (switch before
layout dispatch). Layouts and interpreter handlers from
`lib/Runtime/ByteCode/OpCodes.h:891-946`,
`lib/Runtime/Language/InterpreterStackFrame.cpp:5104-5211`, runtime
implementations `lib/Runtime/Language/JavascriptOperators.cpp:3342-3700`
and `:11460`.

| # | Opcode | Layout | Operands (dst ←) | Helper (`JavascriptOperators::`) | Status |
|---|---|---|---|---|---|
| 1 | InitClassField | ElementCP | – ← Instance, cacheId→propertyId, Value | `OP_InitClassField(inst, propId, value)` | lowered |
| 2 | NewPrivateName | Reg1 | R0 | `OP_NewPrivateName(sc)` | lowered |
| 3 | LdPrivateField | Reg3 | R0 ← R1 inst, R2 brand | `OP_LdPrivateField` | lowered |
| 4 | StPrivateField | Reg4 | R0 ← R1, R2, R3 | `OP_StPrivateField` | lowered |
| 5 | InitPrivateField | Reg4 | R0 ← R1, R2, R3 | `OP_InitPrivateField` | lowered |
| 6 | HasPrivateField | Reg3 | R0 ← R1, R2 | `OP_HasPrivateFieldVar` (new Var form) | lowered |
| 7 | SetPrivateDefinition | Reg2B1 | R0(old=value) ← R1 brand, B2 kind | `OP_SetPrivateDefinition` | lowered |
| 8 | InitPrivateMethod | Reg2 | R0(old=inst) ← R1 brand | `OP_InitPrivateMethod` | lowered |
| 9 | LdPrivateMethod | Reg3 | R0 ← R1, R2 | `OP_LdPrivateMethod` | lowered |
| 10 | LdPrivateAccessor | Reg3 | R0 ← R1, R2 | `OP_LdPrivateAccessor` | lowered |
| 11 | StPrivateAccessor | Reg4U | R0 ← R1, R2, R3 (SlotIndex unused) | `OP_StPrivateAccessor` | lowered |
| 12 | StPrivateMethod | Reg3 | R0 ← R1, R2 | `OP_StPrivateMethod` | lowered |
| 13 | InitStaticPrivateField | Reg4 | R0 ← R1, R2, R3 | `OP_InitStaticPrivateField` | lowered |
| 14 | InitClassFieldComputedName | ElementI | – ← Instance, Element, Value | `OP_InitClassFieldComputedName` | lowered |
| 15 | LdPrivateBrand | ElementP | Value ← cacheId (eval-scope walk) | — (needs `GetEnvForEvalCode` + implicit-call-flag save/restore, not helper-callable) | **carded** |
| 16 | LdPrivateDynamic | Reg3 | R0 ← R1, R2 | `OP_LdPrivateDynamic` | lowered |
| 17 | StPrivateDynamic | Reg4 | R0 ← R1, R2, R3 | `OP_StPrivateDynamic` | lowered |
| 18 | ImportCallWithOptions | Reg3 | R0 ← R1 specifier, R2 options | `OP_ImportCallWithOptions(func, spec, opts, sc)` | lowered |

Associated env-path aborts reached by private-brand loads (A2 finding):

| Site | Opcode family | Condition | Status |
|---|---|---|---|
| IRBuilder `BuildElementSlotI2` (was ~:4109) | LdEnvSlot/StEnvSlot(+Obj/ChkUndecl) | `GetEnvReg()==NoRegister` | **fixed** (env materialized via `LdEnv`) |
| IRBuilder `BuildElementSlotI2` LdEnvObj (was ~:3923) | LdEnvObj | `GetEnvReg()==NoRegister` | carded (not reached by the corpora; same fix pattern applies) |
| IRBuilder `InvalCachedScope` (was ~:2984) | InvalCachedScope | `GetEnvReg()==NoRegister` | carded (direct-eval scope-cache path; out of this slice) |

## 2. Mechanism

**Helper-call lowering**, the `LdImportMeta`/`ImportCall` pattern: the
bytecode opcode flows through IRBuilder as a normal instruction (real
operands, real bytecode offset), and `Lower.cpp` converts it to one call of
the existing runtime helper. Semantics are the interpreter's by
construction; all new risk is argument marshalling.

- **≤2 src operands** (Reg1/Reg3 families, ImportCallWithOptions): the
  generic per-layout builders already emit `dst, src1, src2`; the Lowerer
  unlinks them, queues `LoadHelperArgument` in reverse C-parameter order,
  and `ChangeToHelperCall`s. `ImportCallWithOptions` additionally
  materializes the function object (`LoadFunctionObjectOpnd`) exactly like
  `ImportCall`.
- **3+ helper parameters** (Reg4 family, Reg4U, Reg2B1, ElementCP,
  ElementI): extra parameters ride an `ExtendArg_A` chain linked from src2
  (`IRBuilder::BuildPrivateElementHelper`, the `BuildInitClass`/
  `NewClassConstructor` shape). `Lowerer::LowerPrivateElementHelperCall`
  walks the chain from src2 (last parameter outermost), queues arguments
  reversed, and `ChangeToHelperCall`s; orphaned chain instructions are
  removed by the generic `ExtendArg_A` case in `LowerInstruction`. GlobOpt
  deliberately does not copy-prop through ExtendArg links (fresh value per
  link), so the walk stays valid.
- **dst==src overlap** (SetPrivateDefinition: R0 is value-old and dst;
  InitPrivateMethod: R0 is instance-old and dst): source operands are built
  before the dst so the old value is read.
- **HasPrivateField** returns `BOOL`; the JIT moves the helper's return
  register straight into a Var register, so the new
  `OP_HasPrivateFieldVar` wraps the BOOL form with the exact
  true/false-materialization the interpreter handler had (one semantic
  implementation, shared).
- **Env-slot brand loads** (`LdEnvSlot` family with no env register): class
  methods read the hidden class-block brand const without their binding
  ever allocating an env register — the interpreter reads the display from
  the closure. The IRBuilder now emits an `LdEnv` instruction (Lowerer
  reads `ScriptFunction::environment`, constant for a function that never
  re-scopes) and builds the slot-array field operand pointer-based. Two
  marshalling pitfalls found and fixed en route: `BuildFieldOpnd` only
  accepts bytecode-register ids (a fresh temp's id corrupts the loop-body
  temp map / `EnsureLoopBodyLoadSlot` bounds), and by-ID `PropertySym`
  lookups break under the inlinee sym-table ID adjustment — hence
  `PropertySym::New(StackSym*, ...)`.

## 3. Calling-convention and GC-safety analysis (doc/memory-management.md)

- **Argument order**: `LoadHelperArgument` queues into `helperCallArgs[]`;
  `LowererMD::LowerCall` (ARM64) assigns the last-queued operand to x0. All
  emissions therefore queue in reverse C-parameter order, script context
  first when it is the last parameter — cross-checked against
  `OP_LdImportMeta(moduleIndex, sc)`, `OP_ImportCall(func, spec, sc)` and
  `OP_NewClassConstructor(env, infoRef, homeObj, ctorParent)` load sites.
- **Helper attributes** (`JnHelperMethodList.h`): the DBG
  `ReconcileWithLowererStateOnHelperCall` check requires a reentrant-capable
  helper's *bytecode opcode* to carry implicit-call attributes.
  `NewPrivateName`, `InitPrivateMethod`, `StPrivateMethod` have no
  `Op*ImplicitCall` attributes, so their helpers are
  `AttrCanNotBeReentrant` (their implementations never call user script).
  Brand-checking/throwing helpers carry `AttrCanThrow`; accessors may call
  user getters (reentrant, like `ToNumber`/`Add`).
- **Throw-through**: helpers that throw (brand checks, module resolution)
  propagate through the native frame; the call instruction keeps the
  bytecode offset, so EH maps, bails out and the interpreter re-executes
  the opcode — the standard `ToNumber`-style path, nothing new.
- **GC at the call site**: operands are Var values in JIT-registers/spill
  slots tracked by the normal register-allocator/bailout machinery; the
  helpers themselves allocate only in the Recycler. No new ownership,
  roots or native resources are introduced (no ownership record needed —
  no retained allocation crosses the boundary).

## 4. Coverage evidence (before/after, same script, same flags)

Witness `class A { #x = 1; #m(v){...} run(n,inc){ loop with this.#m/this.#x } }`
warmed 3000 iterations (`-trace:backend`, `-forceNative -off:simpleJit
-bgJitDelay:0`):

- **Before** (base `b436f3807` rebuilt for the A/B): `A.prototype.run`,
  `#m` show `BeginBackEnd` and never `EndBackEnd` — codegen aborted in the
  IRBuilder (brand `LdEnvSlot` env-reg abort, then the private-opcode
  abort), functions stayed interpreted. Only the constructor `A` and the
  public-field initializer completed.
- **After**: every class-body function completes FullJit codegen
  (`BeginBackEnd`/`EndBackEnd` pairs for `Global code`, `A`,
  `*s1field0*` (the `InitPrivateField` initializer), `#m`,
  `A.prototype.run`). Smoke corpus output is byte-identical across
  `-nonative`, `-mic:1 -msjrc:1 -bgjit-`, `-mic:1 -bgjit-` and
  `-forceNative -off:simpleJit -bgJitDelay:0`.

Two crash repros found *during* development of the env-slot fix (fresh-temp
sym id through `BuildFieldOpnd`; by-ID `PropertySym` lookup under the
inlinee ID adjustment — a loop-body compile inlining `#m`) are fixed and
covered by the b4-style loop probes in `test/es7/class-private-jit-probes.js`.

## 5. Verification

test262 (pinned `out/test262` `2e0a5676`, harness `tools/test262/run.py`
`--jobs 8`, controls green every run, `tools/jit-test262/campaign.py`,
evidence under `out/c2jit/<batch>/<mode>/`):

| Batch | nojit | interp | fulljit | diffs vs nojit | failure shapes |
|---|---|---|---|---|---|
| s1-class (16,689 scenarios) | 16565/124 | 16565/124 | 16565/124 | 0 / 0 | all `wrong-result` (recorded baseline features); 0 crash, 0 timeout |
| s12-modules (737 scenarios) | 565/172 | 565/172 | 565/172 | 0 / 0 | all `wrong-result`; 0 crash, 0 timeout |

The tripwire was newly-JITting functions staying correct: both JIT modes
match the interpreter outcome for every scenario (pass↔fail flips: zero).

Focused probes — `test/es7/class-private-jit-probes.js` (8 tests: warm-loop
private get/set/has with type churn; brand-check TypeError thrown *from*
JIT-compiled bodies; shared methods/accessors across instances; exotic
receivers — proxy throws (private access bypasses proxies), null-proto,
frozen, non-extensible; public/computed/static field definitions from warm
constructors; private statics; duplicate-name SyntaxError; per-evaluation
brand distinctness): **8/8 pass** under `-nonative`,
`-maxInterpretCount:1 -maxSimpleJitRunCount:1 -bgjit-`,
`-maxInterpretCount:1 -bgjit-`, `-forceNative -off:simpleJit -bgJitDelay:0`.

## 6. Commands and gates (all through the shared gate lock)

| Gate | Result |
|---|---|
| `zig build -Dprofile=jit` | BUILD_EXIT:0 |
| Full dynapogo `test/runtests.py --test --variants=dynapogo -j 4` | **2007 passed / 286 failed** (baseline 2006/286 + the new probes test; 286 exactly the tagged asm.js/WASM set: AsmJs 92, AsmJSFloat 14, ASMJSParser 1, wasm 54, WasmSpec 123, WasmSpec.MultiValue 2); **zero signals** |
| `--variants=interpreted -j 4` | **2433 / 397** (baseline 2432/397 + probes; 397 exactly the tagged set incl. `Bugs/fabs1.js`); zero signals |
| `--variants=disable_jit` on the JIT binary | **Success (0 failed)** — 2039/0 shape |
| `zig build check` (test profile) | **PASS, 2039 / 0** (pin bumped 2038→2039 with lineage in `tools/build/gate_counts.json`) |
| `zig build check -Dprofile=debug-static` | **PASS, 2091 / 0** (pin bumped 2090→2091) |
| `zig build -Dprofile=jit check` | **PASS** (NativeCodeGenerator symbol; call/rest/apply smoke green under both `-mic:1 -bgjit-` and dynapogo flags; bytecode verify green) |

The earlier dynapogo runs in this lane (before the probes test was
registered in `test/es7/rlexe.xml`) reproduced the recorded 2006/286
exactly, confirming no engine-level regression; the final +1 is the new
test. `dyn_fails.txt` (stale untracked scratch from a prior dynapogo
triage, content subsumed by the recorded gate receipts) was removed.

## 7. Residual cards

- **LdPrivateBrand** (ElementP): interpreter handler walks
  `GetEnvForEvalCode()` (strict-global vs closure env) with
  implicit-call-flag save/restore and SyntaxError/ReferenceError paths; not
  expressible as `(args, ScriptContext*)`. A lowering needs a runtime
  extraction (e.g. `OP_LdPrivateBrandEnv(FrameDisplay*, PropertyId,
  ScriptContext*)`) plus a JIT-side env decision mirroring
  `GetEnvForEvalCode`'s strict-global special case. Functions containing it
  (direct eval referencing `#name`) still abort whole-function codegen and
  stay interpreted — verified by the s1-class corpus (whose
  eval-visible-private scenarios are interpreted-baseline failures
  unchanged in all modes).
- **LdEnvObj / InvalCachedScope with NoRegister env**: same shape as the
  fixed LdEnvSlot abort; not reached by the S1/S12 corpora. Apply the same
  `LdEnv`-materialization pattern when a corpus needs them.
- **Private-method call targets** (`this.#m(...)`) do not inline: the
  target is a helper-call result (no inline-cache/constant identity), so
  call-result typing and inlining are weaker than for public methods. A
  later slice could lower `LdPrivateMethod` with an inline cache on the
  brand record. Not a correctness issue.
- **Inlined private-op helpers and int specialization**: private-op results
  are untyped Vars, so surrounding arithmetic on them does not
  int-specialize (no overflow bailouts in private-op loops). Expected for
  helper-only lowerings; a typed fast path would be a separate slice.

## 8. Commits

- `03267de1b` — a3 prep: runtime helper registrations, `OP_HasPrivateFieldVar`,
  `OP_ImportCallWithOptions` DBG reentrancy macros.
- `1bbbef786` — a3 jit: IRBuilder + Lowerer lowerings (17 opcodes) and the
  `LdEnvSlot` no-env-reg fix (amended with a comment fold; original
  pre-amend hash `bd0593e97`).
- (this commit) — a3 receipt + probes: `test/es7/class-private-jit-probes.js`
  + `rlexe.xml` entry, gate-count pin bumps, this receipt.

### Review outcome (dual ACCEPT, nits folded)

Both reviewers ACCEPTed with zero `[bug]`. The general review audited
all 17 lowered opcodes' argument orders one-by-one against the
interpreter handlers and the ARM64 reversed-queue convention (zero
skew), the ExtendArg chain lifecycle (leak-free), dst==src ordering,
and the reentrancy attributes (truthful in both directions). The
adversarial review dynamically attacked skew with distinct-operand
identities (29 checks incl. dual-brand swaps, SetPrivateDefinition
kinds, ImportCallWithOptions arg discriminators), inlinee depth-3
compositions re-attacking both fixed marshalling traps, throws/EH
from helpers inside finally synthetic loops and resume-after-throw
(exact counts, no double-handle), eval-in-eval LdPrivateDynamic, and
cross-realm brands — byte-identical across all four modes everywhere;
corpus key-joins 0-flip (s1-class, s12); dynapogo `-j4` **2007/286
exact-tagged**; debug-static asserts clean. The serialized-format
question dissolved: `OP_HasPrivateFieldVar` is a C++ helper function,
not a new opcode — no format change, no GUID rotation owed
(`55c5da6d…` stands).

**Folded nits:** (1) the pointer-based-PropertySym citation should
reference the inlinee-ID-adjustment rationale (the receipt's
`BuildLoopBodySlotOpnd` cite is loose — that helper uses FindOrCreate);
(2) un-interned slot-array PropertySyms cost CSE/PRE only
(correctness-neutral); (3) `InitClassField`'s top-level propertyId
resolution matches every pre-existing ElementCP site (family-wide
pre-existing pattern); (4) `HelperSetPrivateDefinition`/
`HelperImportCallWithOptions` carry attrs `0` — conservative direction
(provably non-reentrant/no-sync-throw per the ImportCall precedent);
(5) the [risk-LOW] stale-env observation is exactly the already-carded
`LdEnvObj`/`InvalCachedScope` set, not a new hole.
