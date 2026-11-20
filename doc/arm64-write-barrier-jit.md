# ARM64 JIT Software Write-Barrier Fix (`modernize/phase4`)

> **Status:** **COMPLETED (`modernize/phase4`)** on macOS ARM64 (`modernize/baseline` remains untouched at `1bf42d1f6`).
> **Scope invariant:** Bytecode-header purity is preserved (`lib/Runtime/Base/JnDirectFields.h`, `lib/Runtime/InternalPropertyList.h`, `lib/Runtime/Library/InJavascript/Intl.js`, and `lib/Runtime/Library/InJavascript/JsBuiltIn*.bc.*.h` are completely unchanged; `verify-bytecode` exits `0`).

---

## 1. Root Causes & Fixes

Five cooperating bugs across ARM64 JIT lowering and post-lowering register-allocation moves broke software write-barrier card-table marking under `ConcurrentCollect`:

1. **`LowererMD::ChangeToAssign` (`lib/Backend/arm64/LowerMD.cpp`) bypassed `ChangeToWriteBarrierAssign`:**
   - Unlike `LowerMDShared.cpp:851-855` and `arm/LowerMD.cpp:2294-2298`, ARM64 `LowererMD::ChangeToAssign(IR::Instr * instr)` did not route through `ChangeToWriteBarrierAssign(instr, instr->m_func)`, leaving all `ChangeToAssign` stores (`InlineArrayPush`, `StElemI_A`, `StElemC`, `StSlot`, `StSlotChkUndecl`, `ConcatStringMulti`, etc.) without barrier evaluation.
   - **Fix:** Delegate `LowererMD::ChangeToAssign(IR::Instr * instr)` directly to `ChangeToWriteBarrierAssign(instr, instr->m_func)`.

2. **`LowererMD::ChangeToWriteBarrierAssign` (`lib/Backend/arm64/LowerMD.cpp`) checked opcode/source metadata too late:**
   - Calling `ChangeToAssignNoBarrierCheck(assignInstr)` invokes `LegalizeMD::LegalizeInstr` and `SinkDst`, lowering `MOV [indir], src` to `OpCode::STR` and stripping `SymOpnd`/`AddrOpnd` metadata before inspecting `assignInstr->m_opcode == Js::OpCode::MOV` and `src->IsWriteBarrierTriggerableValue()`.
   - **Fix:** Compute `isPossibleBarrieredDest` (requiring `TySize[dest->GetType()] == sizeof(void*) && !IRType_IsFloat(dest->GetType()) && !IRType_IsSimd128(dest->GetType())` alongside `IndirOpnd`/`MemRefOpnd` checks) and `needsWriteBarrier = isPossibleBarrieredDest && src->IsWriteBarrierTriggerableValue()` *before* `ChangeToAssignNoBarrierCheck(assignInstr)`. Save `prevInstr` / `nextInstr`, locate the actual lowered `storeInstr` (`assert(storeInstr->m_opcode == Js::OpCode::STR && storeInstr->GetDst()->IsIndirOpnd())`), emit `GenerateWriteBarrier(storeInstr)` when needed, and return `prevInstr ? prevInstr->m_next : (storeInstr == assignInstr ? barrierStart : instr)`.

3. **`GenerateWriteBarrierAddress` (`lib/Backend/arm64/LowerMD.cpp`) missed operand widening, `RegSP` move encoding, and instruction legalization:**
   - Narrow `indexOpnd` (`GetSize() < TySize[TyMachReg]`) must be widened first into a 64-bit temp via `Lowerer::InsertMove(wideIndex, indexOpnd, insertBeforeInstr, false)` before `LSL`/`ADD` so operand widths match in `EncoderMD::EmitOp3*`.
   - When `baseOpnd` is `RegSP`, plain ARM64 `MOV` (encoded via `ORR`) interprets `SP` as `XZR`, so an explicit `ADD Xd, SP, #0` legalization/move path is required.
   - All newly emitted `LDIMM`, `LSL`, and `ADD` instructions are passed through `LegalizeMD::LegalizeInstr(...)`, allowing large positive offsets (e.g. `arr[600]` byte offset `24 + 4800 = 4824 > 4095`) and negative stack offsets (`-MachArgsSlotOffset`) to legalize instead of overflowing ARM64 12-bit immediates.

4. **Shared `EmitWriteBarrierAfterStore`, `GenerateWriteBarrier`, & `GenerateWriteBarrierAssign` (`lib/Backend/arm64/LowerMD.cpp`):**
   - Extracted `EmitWriteBarrierAfterStore(IR::RegOpnd * addrRegOpnd, IR::Instr * storeInstr)` logging `JitWriteBarrierPhase` and legalizing `LSR` (`RecyclerWriteBarrierManager::s_BitArrayCardTableShift + 5 = 14`), `LDIMM cardTable`, `LDIMM #1`, and `STR [cardTable + addrRegOpnd] (TyInt8)`.
   - Both `GenerateWriteBarrierAssign` overloads (`MemRefOpnd` and `IndirOpnd`) pass `false` to `Lowerer::InsertMove` (avoiding double barriers), legalize inserted `MOV`/`LDIMM` instructions, and invoke `EmitWriteBarrierAfterStore(writeBarrierAddrRegOpnd, insertBeforeInstr->m_prev)` *after* emitting the store so `writeBarrierAddrRegOpnd` stays intact for `storeInstr`.

5. **Post-`Lowerer` `LinearScan::InsertMove` (`lib/Backend/LinearScan.cpp:4884`) emitted barriers during register allocation:**
   - Routing `ChangeToAssign` through `ChangeToWriteBarrierAssign` exposed a latent bug when `LinearScanMD::GenerateBailOut` (`lib/Backend/arm64/LinearScanMD.cpp:296`, saving caller-saved registers to `ThreadContext::GetBailOutRegisterSaveSpaceAddr()`) and `GeneratorBailIn` called `LinearScan::InsertMove` (`lib/Backend/LinearScan.cpp:4884`) with default `generateWriteBarrier = true`. Because `LinearScan` runs *after* `Lowerer`, virtual `RegOpnd`s allocated inside `GenerateWriteBarrier` are never register-allocated and remain `RegNOREG` (`0xf` = `x15` in `lib/Backend/arm64/RegList.h:18`), emitting `strb w15, [x15, x15, lsl #0]` and crashing with `EXC_BAD_ACCESS` at `0x2` (seen in `testStElemIConcurrent` and `c31_disposal_jit.js` bailout tests).
   - **Fix:** Pass `false` for `generateWriteBarrier` in `LinearScan::InsertMove` (`Lowerer::InsertMove(dst, src, insertBeforeInstr, false)`), because post-lowering bailout register-save buffers live in non-GC `ThreadContext` storage and spill/bailin frame slots are already covered by stack roots/`OnThreadInit`.

---

## 2. Negative Control & Trace Verification

- **Negative control (unpatched `80ac2a231`, `./out/jit/bin/ch`):**
  - `test/Optimizer/arm64_write_barrier_jit.js` crashed **`5/5`** runs with `rc=139` (`SIGSEGV`) under default `ConcurrentCollect`, while passing (`rc=0`, `PASS`) under `-ForceBlockingConcurrentCollect`.
  - Isolated `testAsyncStackWarm()` in [test/Optimizer/c31_disposal_jit.js](../test/Optimizer/c31_disposal_jit.js) at `n=300` crashed **`4/5`** runs (`rc=-11`), `1/5` `PASS`.
- **`-Trace:BackEnd -Trace:JitWriteBarrier -ForceNative -bgjit-` (post-fix `./out/jit/bin/ch`):**
  - `InlineArrayPush` (`pushYoung`): FullJit `1`
  - `StElemI_A` (`storeInBounds`: FullJit `1`; `storeGrow`: FullJit `1`)
  - `StElemC` (`storeSmallConst`: FullJit `2`; `storeLargeConst` `arr[600]` offset `4824 > 4095`: FullJit `2`)
  - `StSlot` / `StSlotChkUndecl` (`update`: SimpleJit `6`, FullJit loop `8`, FullJit `8`; `testTdzInLoop`: SimpleJit `9`, FullJit `9`)
  - `ConcatStringMulti` (`buildConcat` with `--concat-delim--` `> ShouldAppendChars`: SimpleJit `11`, FullJit `11`)
  - `StFld` / `MemRefOpnd` / `SinkDst` (`SymOpnd`/`AddrOpnd`) / `arguments` (`-MachArgsSlotOffset`): `storeGlobalMemRef` FullJit `1`, `storeInlineAndAux` FullJit `4`, `storeWithSpillAndConst` SimpleJit `40` / FullJit `174`, `useArgumentsSlot` SimpleJit `1` / FullJit `4`
  - Non-triggerable exclusion (`storeNonTriggerable` tagged-int, float64, `Uint32Array`/`Float64Array`): SimpleJit `0`, FullJit `0`
  - Standalone WASM linear-memory stores (`st32[0]` `i32.store`, `stf64[2]` `f64.store`, `runWasm`): `0` write barriers (`WASM_OK`)
- **Test enhancements & C3.1 restoration:**
  - Added [test/Optimizer/arm64_write_barrier_jit.js](../test/Optimizer/arm64_write_barrier_jit.js) (registered in [test/Optimizer/rlexe.xml](../test/Optimizer/rlexe.xml)).
  - Restored `testAsyncStackWarm()` in [test/Optimizer/c31_disposal_jit.js](../test/Optimizer/c31_disposal_jit.js) from `25` to `200`.

---

## 3. Memory Management & GC Ownership Record (`doc/memory-management.md` §3)

| Type / Surface | Allocation Domain | Owner & Lifetime Transfer | Barrier / Pinning Invariant | Invalidation & Thread Context |
|---|---|---|---|---|
| ARM64 JIT heap stores (`LowererMD::ChangeToWriteBarrierAssign`, `GenerateWriteBarrierAssign`, `GenerateWriteBarrier`, `EmitWriteBarrierAfterStore`) | Script `Recycler` heap blocks (and thread stack pages committed into card table via `RecyclerWriteBarrierManager::OnThreadInit`) | Target container object owns stored `Var`/object/string edge while reachable | Pointer-sized non-float/non-SIMD128 stores of `IsWriteBarrierTriggerableValue()` sources mark the byte card (`addr >> 14`, `#1`) in `cardTable` *after* the store; narrow indices are widened to 64-bit, `RegSP` moves preserve `SP` via `ADD #0`, and all address/card instructions are legalized | `Recycler` concurrent mark thread rescans dirty cards; non-triggerable tagged-int/float/`TypedArray`/WASM linear-memory stores skip barrier emission |
| Post-`Lowerer` register-save & spill moves (`LinearScan::InsertMove` in `lib/Backend/LinearScan.cpp`, `LinearScanMD::GenerateBailOut`) | Non-GC `ThreadContext::GetBailOutRegisterSaveSpaceAddr()` buffer or already-rooted stack frame slots | Ephemeral across JIT-to-interpreter bailout or local frame lifetime | Explicitly passes `generateWriteBarrier = false` to `Lowerer::InsertMove`: avoids allocating post-regalloc `RegNOREG` (`x15`) virtual temps and avoids touching `cardTable` on non-heap addresses | Bailout thunk consumes saved registers immediately before resuming interpreter; thread-local script execution only |

---

## 4. Stress, Perf, and Full macOS ARM64 Gate Matrix

### 4.1 Multi-Config `20x` Stress Matrix (`./out/jit/bin/ch`)

- Ran `20x` per configuration on `test/Optimizer/arm64_write_barrier_jit.js` and `test/Optimizer/c31_disposal_jit.js` (`n=200`):
  - Default (`ConcurrentCollect`): **`20/20` PASS** (`1.46s`), **`20/20` PASS** (`3.14s`)
  - `-ForceJITLoopBody -maxinterpretcount:1`: **`20/20` PASS** (`2.02s`), **`20/20` PASS** (`4.56s`)
  - `-ForceNative -bgjit-`: **`20/20` PASS** (`2.97s`), **`20/20` PASS** (`3.68s`)
  - `-ForceNative -bgjit- -off:fullJit`: **`20/20` PASS** (`2.08s`), **`20/20` PASS** (`2.71s`)
  - `-ForceNative -bgjit- -off:simpleJit`: **`20/20` PASS** (`1.89s`), **`20/20` PASS** (`2.89s`)
  - `-CollectGarbage`: **`20/20` PASS** (`1.58s`), **`20/20` PASS** (`3.22s`)
- Note: `-StrictWriteBarrierCheck` aborts (`exit 134`, `SIGABRT`) pre-existing on macOS even with `-nonative test/Basics/hello.js` (`RecyclerWriteBarrierManager::StrictWriteBarrierCheck`). Also note `test/Optimizer/arm64_write_barrier_jit.js` received `testTdzInLoop` / `buildConcat` expression updates afterward and was subsequently re-verified in `dynapogo` (`Optimizer 201/0`), `test`, `debug-static`, `release-shared`, ASan `-CollectGarbage`, and UBSan `-CollectGarbage`.

### 4.2 Benchmark Smoke Spot-Check (`tools/bench/run.py`, `--tier jit --iterations 3`)

- `Octane/splay.js`: **`PASS`**, median **`31879.0`** (MAD `165.0`, score)
- `Octane/earley-boyer.js`: **`PASS`**, median **`11209.0`** (MAD `47.0`, score)
- `Kraken/audio-fft.js`: **`PASS`**, median **`81.0 ms`** (MAD `1.0`, time; no regression vs `84 ms` P1 baseline)

### 4.3 Gate Summary (macOS ARM64)

| Gate | Command / Scope | Result |
|---|---|---|
| Bytecode purity | `mise exec -- zig build verify-bytecode` & `-Dprofile=jit -Dwasm=true verify-bytecode` | **PASS** (`exit 0`, headers unchanged) |
| Style checks | `check_{eol,tabs,ascii,copyright}.sh` (`modernize/baseline..HEAD` & `origin/master..HEAD`) | **PASS** (`0` issues) |
| Build & gate unit tests | `zig test tools/linux-gate-tests.zig`, `check-selftest`, `check-build-tools` | **PASS** (`9/9`, selftest clean, `97` tests OK) |
| `test` profile | `mise exec -- zig build check` | **PASS** (`2089 passed, 0 failed`, smoke + shared native + metadata) |
| `debug-static` profile | `mise exec -- zig build -Dprofile=debug-static check` | **PASS** (`2141 passed, 0 failed`, `536` archive members, `62409 T` / `8/8` JsRT symbols, smoke + metadata) |
| `release-shared` profile | `mise exec -- zig build -Dprofile=release-shared check` | **PASS** (`test-shared-basic`, smoke, metadata) |
| `jit -Dwasm=true` gates | `check-wasm`, `check-fulljit`, `check` | **PASS** (`382/382` P1 WASM, `624/0` FullJit, `check-jit` clean) |
| `dynapogo` JIT suites | `runtests.py -t --variants=dynapogo` (`Optimizer Bugs Array es6 es7`) | **PASS** (**`770 passed, 0 failed`**: `Array 123/0`, `Bugs 101/0`, `Optimizer 201/0`, `es6 224/0`, `es7 121/0`) |
| `GCStress` | `mise exec -- zig build check-gcstress` | **PASS** (in-thread `0.75s` & concurrent `0.81s`, 2 walks each) |
| Node-API & package | `mise exec -- zig build check-nodeapi check-package` | **PASS** (`161` allowlisted exports; `VERIFY PASS`, `hello: world`) |
| ASan & UBSan | `-Dsanitize={address,undefined} check-sanitize` + both JIT tests `-CollectGarbage` | **PASS** (`27/27` legs + `2/2` tests clean under both ASan and UBSan) |
| Direct multi-profile smoke | `test/Optimizer/arm64_write_barrier_jit.js` on `zig-out`, `debug-static`, `release-shared` | **PASS** (`3/3` `PASS`) |
