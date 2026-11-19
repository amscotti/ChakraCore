# Track P1: Pathological JIT Bailout, Liveness, ReJIT, and ARM64 asm.js Tier-Up Fixes

> **Branch / Scope:** `modernize/phase4` (macOS ARM64, 2026-11-19). `modernize/baseline` remains untouched at `1bf42d1f6`. `lib/Runtime/Library/InJavascript/Intl.js`, `lib/Runtime/InternalPropertyList.h`, and `lib/Runtime/Library/InJavascript/JsBuiltIn*.bc.*.h` remain untouched.
>
> **Commits (`modernize/phase4`):**
> - `a6d3a6fe8` (cherry-picked from `72c3c7847`, `p1-loop-liveness`): *jit: fix compound element key liveness abort and loop-body retry storm*
> - `628cf5391` (cherry-picked from `3ba8b393c`, `p1-bailout-rejit`): *Fix P1-B unbounded BailOutIntOnly, array specialization, and loop/function ReJIT storm*
> - `7723bd804` (cherry-picked from `8b30fadfe`, `p1-asmjs-arm64`): *fix(asmjs): poll CheckAsmJsCodeGen on ARM64 JIT bridges and guard unwind headroom*
> - Integration commit on `modernize/phase4` (this receipt): wires regression tests (`test/Optimizer/rlexe.xml`, `test/AsmJs/rlexe.xml`, `test/Bugs/rlexe.xml`, `test/Bugs/p1_jit_bailout_and_tiering.js`), updates `test/wasm-arm64-manifest.json` & `tools/build/gate_counts.json`, refreshes `tools/bench/results/head-jit-darwin-arm64.json`, and updates docs.

---

## 1. Root Causes & Architectural Invariants

### 1.1 `a6d3a6fe8` (`p1-loop-liveness`) -- Compound Element Key Liveness Abort & Loop-Body Retry Storm

**Root cause:**
1. In `EmitLoad` (`lib/Runtime/ByteCode/ByteCodeEmitter.cpp`), compound assignment on computed property keys (`o[k] += v`, `o[k]++`) emitted `BrOnObject_A` directly adjacent to an unconditional `Br` and wrote `Conv_Prop keyTmp, keyTmp`. During peephole optimization, `Peeps::PeepBranch` attempted to invert `BrOnObject_A` via `BranchInstr::Invert()` (`IR.cpp`, unhandled opcode -> backend abort), while reusing `keyTmp` for `Conv_Prop` across the control-flow join disrupted single-`StackSym` liveness.
2. When any loop slice hit a backend abort (including `rejitCounter >= 25` in `Func::Codegen`), `NativeCodeGenerator::JobProcessed` reset the `LoopEntryPointInfo` state back to `NotScheduled`, allowing `InterpreterStackFrame` and `NativeCodeGenerator::GenerateAllFunctions` to reschedule the uncompilable loop indefinitely on every iteration.

**Fix & invariants:**
- **IR / Bytecode Liveness Invariant (`lib/Runtime/ByteCode/ByteCodeEmitter.cpp`):** `EmitLoad` now acquires a separate `convTmp` register, places `Ld_A keyTmp, keyLocation` (`Br keyDone`) between `BrOnObject_A convertKey, keyLocation` and `MarkLabel(convertKey)`, and lowers the object arm as `Conv_Prop convTmp, keyLocation` followed by `Ld_A_ReuseLoc keyTmp, convTmp` before `ReleaseTmpRegister(convTmp)`. Keeping `BrOnObject_A` non-adjacent to `Br` ensures `Peeps::PeepBranch` never calls `BranchInstr::Invert()` on `BrOnObject_A`, and `Ld_A_ReuseLoc` preserves one `StackSym` for `keyTmp` across the diamond join.
- **Unjittable Loop Termination Invariant (`lib/Runtime/Base/FunctionBody.h`, `lib/Backend/Func.cpp`, `lib/Backend/NativeCodeGenerator.cpp`, `lib/Runtime/Language/InterpreterStackFrame.cpp`):**
  - `Func::Codegen` (`lib/Backend/Func.cpp`) explicitly throws `Js::OperationAbortedException()` if `rejitCounter >= 25` (and flushes trace output).
  - `LoopHeader` (`lib/Runtime/Base/FunctionBody.h`) adds zero-initialized `Field(bool) isUnjittable`.
  - `NativeCodeGenerator::Process` catches `Js::OperationAbortedException` and sets `loopBodyCodeGenWorkItem->loopHeader->isUnjittable = true` strictly when `!this->IsClosed() && !fn->GetScriptContext()->IsClosed() && (!JITManager::GetJITManager()->IsOOPJITEnabled() || JITManager::GetJITManager()->IsConnected())`.
  - `NativeCodeGenerator::JobProcessed` calls `entryPoint->SetJITCapReached()` for unjittable loops so `entryPoint->Reset(false)` permanently stops rescheduling. `NativeCodeGenerator::GenerateAllFunctions`, `NativeCodeGenerator::GenerateLoopBody`, and both `InterpreterStackFrame.cpp` scheduling gates skip loops marked `isUnjittable`.
- **Regression test:** `test/Optimizer/p1_loop_liveness_and_abort.js`.

---

### 1.2 `628cf5391` (`p1-bailout-rejit`) -- Unbounded `BailOutIntOnly`, Array Specialization & ReJIT Storms

**Root cause:**
1. Under `CONFIG_FLAG(ESBigInt)` (`lib/Backend/GlobOpt.cpp`), lossy Int32 specialization (`Or_A`, `And_A`, `Xor_A`, `Shl_A`, `Shr_A`, `ShrU_A`, `Not_A`, comparisons, `TypeSpecializeStElem`, `GetDivValueType`) queried `this->DoAggressiveIntTypeSpec()` / `DoLossyIntTypeSpec()` / `DoArrayCheckHoist()` / `DoLdLenIntSpec()` without checking inlinee `instr->m_func` or loop-scoped profile disable flags (`IsAggressiveIntTypeSpecDisabled(true)`, `IsArrayCheckHoistDisabled(true)`, `IsDivIntTypeSpecDisabled(...)`). In addition, `GlobOpt::ToTypeSpecUse` emitted direct `Var -> Int32` conversions with `BailOutIntOnly` even when `DoAggressiveIntTypeSpec` was already disabled, and `TryHoistInvariant` could hoist a lossy `TyInt32` `FromVar` out of a loop into a non-primitive landing pad.
2. In `lib/Backend/BailOut.cpp`, masked array bailout kinds (`BailOutOnArrayAccessHelperCall`, `BailOutOnNotNativeArray`, `BailOutConventionalNativeArrayAccessOnly`, `BailOutConvertedNativeArray`) were missed by exact `==` checks; shared ReJIT flag transitions were duplicated ad hoc and could leave an inlinee's profile flag diverging from its callers/loops; and stale loop entry points or `AfterLoopBodyRejit` paths could spin indefinitely without an upper bound.

**Fix & invariants:**
- **Type-Spec & Two-Step `ESBigInt` Lowering (`lib/Backend/GlobOpt.cpp`):**
  - `AllowOptForInstr` and `CanLossyIntTypeSpecForInstr` couple lossy Int32 optimization to `!CONFIG_FLAG(ESBigInt) || DoAggressiveIntTypeSpec || DoFloatTypeSpec` for both `topFunc` and `instr->m_func`. `DoAggressiveIntTypeSpec(func)` also checks loop-level `IsAggressiveIntTypeSpecDisabled(true)` when `func->GetJITFunctionBody()->HasLoops()`.
  - `GlobOpt::ToTypeSpecUse` lowers un-specialized `Var` values in `lossy && CONFIG_FLAG(ESBigInt)` contexts through a two-step `Var -> Float64 (BailOutNumberOnly) -> Int32` path when `DoAggressiveIntTypeSpec` is disabled and `DoFloatTypeSpec` is active, and `TryHoistInvariant` refuses to hoist lossy `ESBigInt` `FromVar(TyInt32)` into a landing pad where `src1StackSym` is neither primitive nor type-specialized.
  - `TypeSpecializeStElem`, `DoArrayCheckHoist`, `DoLdLenIntSpec`, and `GetDivValueType` check loop-level and inlinee-level profile disable flags consistently.
- **Unified Profile Disablement & Bounded ReJIT Convergence (`lib/Backend/BailOut.cpp`):**
  - Array bailout checks mask bits cleanly (`(bailOutKind & IR::BailOutOnArrayAccessHelperCall) != 0`, `(bailOutKind & ~IR::BailOutKindBits) == IR::BailOutOnNotNativeArray`, etc.).
  - `TryHandleBailOutProfileFlag` centralizes profile flag inspection and disablement across function-body (`isLoopBody=false`) and loop-body (`isLoopBody=true`) bailouts.
  - When an inlinee triggers a profile flag disablement while its default entry point has `IsCodeGenDone()`, `inlineeBody->SetDontRethunkAfterBailout()` is set; stale callers/loops detecting an already-disabled inlinee flag (or sibling loops sharing a loop-level flag with `bailOutCount <= 6 && loopHeader->GetRejitCount() < 4`) force immediate ReJIT convergence (`callsCount = 0` / `totalJittedLoopIterations = 0`).
  - Hard upper bounds cap `profileInfo->GetRejitCount() >= 100` (including the `AfterLoopBodyRejit` path) and `loopHeader->GetRejitCount() >= 50` (including inner bailouts and final guard).
- **Regression test & bailout verification:** `test/Optimizer/p1_bailout_rejit_storm.js` and `-trace:BailOut,ReJIT` on `Kraken/stanford-crypto-aes.js` (`59` `BailOut` lines + `17` `Rejit` lines, `90 ms`).
- **Observed trade-off (`Kraken/stanford-crypto-ccm.js`):** When a function/loop disables *both* `AggressiveIntTypeSpec` (`BailOutIntOnly`) *and* `FloatTypeSpec` (`BailOutNumberOnly`), `CanLossyIntTypeSpecForInstr` intentionally disables `LossyIntTypeSpec` under `ESBigInt`, leaving `stanford-crypto-ccm.js` at `205-210 ms` (`61 ms` with `-ESBigInt-`). Adding a dedicated `BigInt`-aware non-primitive bailout kind would allow lossless int specialization to persist safely without disabling bitwise specialization after float bailouts.

---

### 1.3 `7723bd804` (`p1-asmjs-arm64`) -- ARM64 asm.js `CheckAsmJsCodeGen` Polling & Stack-Reserve Headroom

**Root cause:**
1. On ARM64, `AsmJsExternalEntryPoint` (`lib/Runtime/Language/AsmJsUtils.cpp`) and `TryCallAsmJsJit` (`lib/Runtime/Library/JavascriptFunction.cpp`) bailed back to the interpreter whenever `entryPointInfo->jsMethod == GetCheckAsmJsCodeGenThunk()`, never calling `NativeCodeGenerator::CheckAsmJsCodeGen(function)`. Consequently, background JIT jobs that finished compiling asm.js functions stayed unreachable on warm calls (and internal `TryCallAsmJsJit` duplicated slot-image `_alloca` / floor-guard setup).
2. `AsmJsJitCall` saves its C++ callee-saved bridge frame immediately below `raw` on the thread stack, so setting `wasmStackReserveFloor` directly to `(uintptr_t)raw` risked a stack-overflow C++ exception / `libunwind` walk descending into and overwriting `AsmJsJitCall`'s saved frame.

**Fix & invariants:**
- **Backend API Bridge (`lib/Common/BackendApi.h`, `lib/Backend/BackendApi.cpp`):** Exposes `Js::CheckAsmJsCodeGen(Js::ScriptFunction * function)` forwarding to `NativeCodeGenerator::CheckAsmJsCodeGen(function)` under `#ifdef ENABLE_ASMJS`.
- **Unified Entrypoint Resolution & Slot-Image Dispatch (`lib/Runtime/Language/AsmJsUtils.cpp`, `lib/Runtime/Library/JavascriptFunction.cpp`):**
  - `ResolveAsmJsJitEntryPoint(ScriptFunction* func)` filters out null functions/info, multi-value WASM signatures (`wasmSig->IsMultiValue()`), and SIMD signature functions (`WAsmJs::FunctionUsesSimd(body)` under `ENABLE_WASM_SIMD`).
  - Only when `entryPointInfo->jsMethod == GetCheckAsmJsCodeGenThunk()` (never on `AsmJsDelayDynamicInterpreterThunk` or `DelayDynamicInterpreterThunk`), it polls `jsMethod = CheckAsmJsCodeGen(func)` and re-reads `entryPointInfo`.
  - Defense in depth: only allows bridging when `entryPointInfo != nullptr && entryPointInfo->IsCodeGenDone() && jsMethod != nullptr && jsMethod == entryPointInfo->GetNativeEntrypoint()` and `jsMethod` is none of `AsmJsDelayDynamicInterpreterThunk`, `DelayDynamicInterpreterThunk`, `GetCheckAsmJsCodeGenThunk()`, `GetCheckCodeGenThunk()`, `AsmJsExternalEntryPoint`, `WasmLazyTrapCallback`, `IsIntermediateCodeGenThunk(jsMethod)`, or `IsAsmJsCodeGenThunk(jsMethod)`.
  - Both `AsmJsExternalEntryPoint` (`unboxExternalArgs=true`) and `TryCallAsmJsJit` (`unboxExternalArgs=false`) call `InvokeAsmJsJitSlotImage(...)`.

#### Section 3 Ownership & Lifetime Record (`doc/memory-management.md`)

| Record Field | Value |
|---|---|
| **Component / Files** | `ResolveAsmJsJitEntryPoint`, `InvokeAsmJsJitSlotImage` (`lib/Runtime/Language/AsmJsUtils.cpp`), `TryCallAsmJsJit` (`lib/Runtime/Library/JavascriptFunction.cpp`), `CheckAsmJsCodeGen` (`lib/Backend/BackendApi.cpp`) |
| **Allocation Domain** | Native stack via per-call `_alloca(slotImageBytes + 16 + reserve)` on the executing script thread, where `slotImageBytes = max(info->GetArgByteSize() + 3 * MachPtr, 8 * MachPtr)` and `reserve = jitStackBudget (65536) + unwindHeadroom (32768) = 98304` bytes. Zero Recycler / heap allocations. |
| **Owner & Lifetime** | Owned strictly by the active `InvokeAsmJsJitSlotImage` C++ stack frame. `base = align16(raw) + reserve` holds `[func][0][0][unboxed args...]` purely for the synchronous call into `AsmJsJitCall((void*)jsMethod, (void*)base, outRet)` and becomes invalid on frame return. |
| **Retained Pointers / Pinning** | `*(ScriptFunction**)base = func` is passed on the native stack across a single synchronous call from an active C++ frame (`AsmJsExternalEntryPoint` or `TryCallAsmJsJit`) where `func` and `origArgs` are already rooted/live in the caller frame; no `Recycler` reference is stored off-stack or retained asynchronously. |
| **Permitted Thread / Context** | Executing script thread (`ScriptContext` / `ThreadContext`) only. `ThreadContext::WasmStackReserveFloorGuard` sets `wasmStackReserveFloor = (uintptr_t)raw + unwindHeadroom` (`+32 KB` above `AsmJsJitCall`'s saved bridge frame below `raw`), leaving the full `64 KB` JIT recursion budget between `base` and `floor` while preventing `Throw::StackOverflow` / `__cxa_throw` / `libunwind` from overwriting `AsmJsJitCall`'s bridge registers (`fp`/`lr`). |
| **Reentrancy / Unwind** | Nested external/internal bridge calls push/restore previous `wasmStackReserveFloor` monotonically via `WasmStackReserveFloorGuard` RAII destructor on both normal return and C++ exception unwind (`StackOverflow`, WASM traps). Pre-probed with `PROBE_STACK_CALL(scriptContext, func, slotImageBytes + 16 + reserve + Js::Constants::MinStackDefault)`. |
- **Regression test:** `test/AsmJs/p1_asmjs_bgjit_tierup.js`.

---

## 2. Test Wiring, Manifest Sync & Gate Counts

- **`test/Optimizer/rlexe.xml`:** Added `p1_loop_liveness_and_abort.js` and `p1_bailout_rejit_storm.js` (`<compile-flags>-CollectGarbage</compile-flags>`).
- **`test/AsmJs/rlexe.xml`:** Added `p1_asmjs_bgjit_tierup.js` (`<compile-flags>-CollectGarbage</compile-flags>`, `<tags>exclude_interpreted</tags>`).
- **`test/Bugs/rlexe.xml` + `test/Bugs/p1_jit_bailout_and_tiering.js`:** Added cross-tier P1 integration smoke covering (1) compound-key loop liveness, `toString` conversion count (`300`), and bounded aborted-`eval` loop; (2) `BailOutIntOnly` mixed int/double/uint32/object/`BigInt` storm, `-0` preservation, and inlinee ReJIT convergence; (3) asm.js external + internal BGJIT tier-up under `-CollectGarbage`.
- **`test/wasm-arm64-manifest.json`:** Regenerated via `python3 tools/build/gen_wasm_manifest.py` (`484` total entries; `382` P1 entries preserved; `AsmJs[146]` classified `phase: "P2"` due to `exclude_interpreted`; verified fresh with `--check`).
- **`tools/build/gate_counts.json`:** Updated `test: 2086/0 -> 2087/0` (`test/Bugs` `104 -> 105`) and `debug: 2138/0 -> 2139/0` (verified dynamic consumption in `tools/build/check_regression.py` and `tools/build/test_build_tools.py`).

---

## 3. Same-Host Benchmark Receipt (`tools/bench/run.py`, macOS ARM64)

> Same Apple M1 Max (`darwin/arm64`) host; `out/jit/bin/ch` built with `mise exec -- zig build -Dprofile=jit -Dwasm=true` (`sha256:e43f7e87126b`). Pre-P1 JIT/interp baselines are the committed 2026-11-16 `modernize/baseline` (`4b2077687`, `sha256:7afbc2509d65`) 5-iteration results.

### 3.1 Focused P1 Targets (5 iterations, `--timeout 120`)

| Benchmark | Metric | Pre-P1 JIT (`4b2077687`) | Post-P1 JIT (`HEAD`) | Post-P1 Interp (`-nonative`) | JIT Speedup vs Pre-P1 | JIT Speedup vs Interp | Post-P1 JIT `-bgjit-` (5x med) |
|---|---|---|---|---|---|---|---|
| `Kraken/audio-fft.js` | ms (lower better) | `5539.0` | **`84.0`** (`mad 0.0`) | `2287.0` (`mad 4.0`) | **`65.94x`** | **`27.23x`** | **`89.0`** (`[89,89,88,89,88]`) |
| `Kraken/audio-beat-detection.js` | ms (lower better) | `5594.0` | **`110.0`** (`mad 0.0`) | `2391.0` (`mad 14.0`) | **`50.85x`** | **`21.74x`** | -- |
| `Kraken/audio-oscillator.js` | ms (lower better) | `1672.0` | **`202.0`** (`mad 4.0`) | `1836.0` (`mad 1.0`) | **`8.28x`** | **`9.09x`** | -- |
| `Kraken/stanford-crypto-aes.js` | ms (lower better) | `625.0` | **`89.0`** (`mad 1.0`) | `627.0` (`mad 3.0`) | **`7.02x`** | **`7.04x`** | -- |
| `SunSpider/bitops-nsieve-bits.js` | ms (lower better) | `96.0` | **`6.0`** (`mad 0.0`) | `55.0` (`mad 0.0`) | **`16.00x`** | **`9.17x`** | **`6.0`** (`[6,5,6,6,6]`) |
| `JetStream/container.cpp.js` | score (higher better) | `120.2` | **`364.8`** (`mad 2.1`) | `12.2` (`mad 0.0`) | **`3.03x`** | **`29.92x`** | -- |

- **`-trace:BailOut,ReJIT` on `Kraken/stanford-crypto-aes.js`:** `59` `BailOut` lines + `17` `Rejit` lines total, finishing in `90 ms` (all ReJITs converge cleanly; zero storm).

### 3.2 Full 4-Suite Comparison (`tools/bench/results/head-jit-darwin-arm64.json`, 3 iterations, `64/64` applicable PASS)

| Suite | Metric (`n`) | Pre-P1 JIT (`4b2077687`) | Post-P1 JIT (`HEAD`) | Interp (`head-interp-darwin-arm64.json`) | Speedup vs Pre-P1 | Speedup vs Interp |
|---|---|---|---|---|---|---|
| **SunSpider** | sum ms (`26`) | `286.0` ms (`gm 6.59`) | **`172.0` ms** (`gm 5.30`) | `844.0` ms (`gm 28.85`) | **`1.66x`** sum (`1.244x` ratio gm) | **`4.91x`** sum (`5.45x` ratio gm) |
| **Kraken** | sum ms (`14`) | `15054.0` ms (`gm 207.79`) | **`2081.0` ms** (`gm 86.58`) | `36509.0` ms (`gm 980.93`) | **`7.23x`** sum (`2.400x` ratio gm) | **`17.54x`** sum (`11.33x` ratio gm) |
| **Octane** | geomean score (`14`) | `22786.6` | **`32382.1`** | `1550.2` | **`1.421x`** | **`20.89x`** |
| **JetStream** | geomean score (`10`/`9` interp) | `311.5` | **`346.5`** | `8.5` (`9` pass, `gcc-loops` timeout) | **`1.112x`** | **`40.12x`** |
| **Overall** | geomean across `64`/`63` tests | -- | -- | -- | **`1.453x`** (`64` tests) | **`11.49x`** (`63` tests) |

Key additional Octane / SunSpider wins inside the 3-iteration full run:
- `Octane/navier-stokes.js`: `3262.0 -> 31990.0` (**`9.81x`**)
- `Octane/crypto.js`: `2654.0 -> 24830.0` (**`9.36x`**)
- `SunSpider/crypto-aes.js`: `23.0 ms -> 8.0 ms` (**`2.88x`**)
- `Octane/splay.js`: `22090.0 -> 31295.0` (**`1.42x`**); `Octane/pdfjs.js`: `29867.0 -> 35402.0` (**`1.19x`**); `Kraken/stanford-crypto-pbkdf2.js`: `217.0 ms -> 147.0 ms` (**`1.48x`**).
- Only notable delta lower than `4b2077687`: `Octane/regexp.js` (`6903 -> 5513` 3-iter / `5551` 5-iter; only 28 bailout/rejit lines total, `3.78x` vs interp) and `Kraken/stanford-crypto-ccm.js` (`169 -> 205` ms 3-iter / `210` ms 5-iter, `1.98x` vs interp; `61.0 ms` with `-ESBigInt-` as explained in Section 1.2). Note `4b2077687` also predated `Q1.4`, `I1`, `I2`, and `Q2`.

---

## 4. Verification Matrix (`modernize/phase4`, macOS ARM64)

- **Self-tests, bytecode, build tools & manifest:**
  - `mise exec -- zig build verify-bytecode check-selftest check-build-tools`: exit `0` (`Bytecode is up to date`, `pal-config: 95/95`, `check-build-tools`: `97` tests OK).
  - `mise exec -- zig test tools/linux-gate-tests.zig`: `All 9 tests passed.`
  - `python3 tools/build/gen_wasm_manifest.py --check`: `fresh` (`484` total, `382` P1).
- **Multi-profile runtime & regression gates:**
  - `mise exec -- zig build check`: exit `0` (`2087/0`, `Bugs: 105/0`, `check_native: PASS`, `18495 T` symbols).
  - `mise exec -- zig build -Dprofile=debug-static check`: exit `0` (`2139/0`, `535` archive members, `61655 T` symbols, `8/8` static native PASS).
  - `mise exec -- zig build -Dprofile=release-shared check`: exit `0` (`check_smoke: PASS`, `17091 T` symbols, `check_native: PASS`).
  - `mise exec -- zig build -Dprofile=jit -Dwasm=true check-wasm check-fulljit check`: exit `0` (`check_wasm_p1`: **`382/382`** P1, `ASMJSParser 2/0`, `AsmJs 145/0`, `WasmSpec 231/0`, `wasm 70/5`; `check_fulljit`: **`622/0`**; `check_jit: PASS`; `python3 tools/build/check_regression.py test/runtests.py out/jit/bin/ch`: **`2087/0`**).
  - `python3 tools/build/check_wasm_p1.py --jit test/runtests.py out/jit/bin/ch`: **`255/382` P1 JIT PASS (`AsmJs 146/0`, `WasmSpec 123/0`, `ASMJSParser 2/0`, `failed 0, missing 0, unmatched 0, duplicates 0`)**.
  - `python3 test/runtests.py --test --arm64 --binary=out/jit/bin/ch test/Optimizer test/AsmJs test/Bugs --variants interpreted dynapogo`: `interpreted 492/0` (`AsmJs 133/0`, `Bugs 107/0`, `Optimizer 252/0`), `dynapogo 435/0` (`AsmJs 134/0`, `Bugs 102/0`, `Optimizer 199/0`).
  - `python3 test/runtests.py --test --arm64 --binary=out/jit/bin/ch --variants=disable_jit test/Bugs`: `106/0`.
  - `python3 tools/build/check_fulljit.py test/runtests.py out/jit/bin/ch --suites Optimizer Bugs`: `Bugs 102/0`, `Optimizer 198/1` (lone mismatch `Optimizer/mul.js` intentionally produces empty output for `<baseline />`, returning `rc=0` under `-forceNative`, `-nonative`, and default).
- **GC stress & Node-API:**
  - `mise exec -- zig build check-gcstress`: exit `0` (`in-thread 1.00s walks=2`, `concurrent 0.76s walks=2`).
  - `mise exec -- zig build check-nodeapi`: exit `0` (`161` exports = `155 napi + 6 host`, `PASS`).
- **Sanitizers (`ASan` & `UBSan`):**
  - `mise exec -- zig build -Dsanitize=address check-sanitize`: **`27/27`** clean; all 4 new P1 tests (`p1_loop_liveness_and_abort.js`, `p1_bailout_rejit_storm.js`, `p1_asmjs_bgjit_tierup.js`, `p1_jit_bailout_and_tiering.js`) ran under `ASAN_OPTIONS=detect_leaks=0 ./out/sanitize-address/bin/ch -ExtendedErrorStackForTestHost -CollectGarbage` -> exit `0`.
  - `mise exec -- zig build -Dsanitize=undefined check-sanitize`: **`27/27`** clean; all 4 new P1 tests ran under `UBSAN_OPTIONS=print_stacktrace=1:halt_on_error=1 ./out/sanitize-undefined/bin/ch -ExtendedErrorStackForTestHost -CollectGarbage` -> exit `0`.
  - Verified `out/jit/bin/ch` rebuilt intact afterward via `mise exec -- zig build -Dprofile=jit -Dwasm=true`.
- **Package & Style checks:**
  - `mise exec -- zig build check-package`: deterministic tarball (`5418699` bytes), `MANIFEST.json` verification, `ch` + `ch-nodeapi` smoke tests PASS.
  - `tools/StyleChecks/check_{eol,tabs,ascii,copyright}.sh` over `origin/master..HEAD` & `tools/StyleChecks/check_style_portable.py --self-test` verified after integration commit.
- **Targeted Test262 Forced-JIT Stress (`out/jit/bin/ch`, `10,123` scenarios across `5,096` files in `TypedArray`, `Array`, `bitwise-*`, `*-shift`, `Math`; `25/25` controls):**
  - Forced JIT (`-ForceJITLoopBody -maxinterpretcount:1 -Test262 -CollectGarbage -NegativePhaseReport`): `10,057 passed / 66 failed`.
  - Interpreter (`-nonative -Test262 -CollectGarbage -NegativePhaseReport`): `10,061 passed / 62 failed` (`0` regressions vs `out/test262-refresh-phase4-q2/results.json`).
  - JIT vs. Interpreter diff: **`0` diffs** across `TypedArray`, `Array`, `bitwise-*`, and `*-shift`; `4` diffs strictly in `Math/{ceil/S15.8.2.6_A7.js, floor/S15.8.2.9_A7.js}` (`default` & `strict mode`: `SameValue(0, -0)` on `-0.9`/`0.1`, originating in pre-existing `LowererMD::GenerateFastInlineBuiltInMathFloorCeil` in `lib/Backend/arm64/LowerMD.cpp:9146-9222`, which checks `floatOpnd == -0.0` rather than `(-1.0 < floatOpnd && floatOpnd <= -0.0)` for `InlineMathCeil`; untouched by P1).
