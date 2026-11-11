# ARM64 asm.js/WASM ABI — design spec (W1, D4)

Status: design, no code yet. Owner: unassigned. Base commit: `97ee0be`.
Plan: [wasm-arm64-plan.md](wasm-arm64-plan.md) W1 + D4.
This document DECIDES the ARM64 asm.js/WASM calling convention. W3/W4 must
implement against it. Deviations require a recorded amendment here, not a
local improvisation in thunk or lowering code.

Marking: every statement below is tagged `[x64 fact]`, `[decision]`, or
`[open]`. Every `[x64 fact]` carries `file:line` anchors into the x64 tree.
`[decision]` entries cite the facts that justify them.

## 0. Terminology

- `[x64 fact]` A verified statement about today's x86/x64 code, with
  `file:line` anchors. Line numbers are as of base commit `97ee0be`.
- `[decision]` A normative choice for ARM64. It is binding on W3/W4 until
  amended in this document.
- `[open]` An undecided question, converted to an explicit P1/P2 decision
  point (see §9).

## 1. What exists on x64 today

### 1.1 Call-stack layouts

- `[x64 fact]` `AsmJsCallStackLayout` is a bare two-field struct:
  `JavascriptFunction* functionObject` followed by a flexible `Var args[]`
  array — `lib/Runtime/Language/JavascriptStackWalker.h:37-41`.
- `[x64 fact]` `JavascriptCallStackLayout` is the external (JS) frame: it
  carries `JavascriptFunction* functionObject`, `CallInfo callInfo`, then
  `Var args[]` — `lib/Runtime/Language/JavascriptStackWalker.h:42-63`
  (fields at `:45-47`, accessor methods at `:48-60`).
- `[x64 fact]` The walker converts between frame pointer and
  `JavascriptCallStackLayout` via `JavascriptFunctionArgIndex_Function - 
  JavascriptFunctionArgIndex_Frame` — `lib/Runtime/Language/JavascriptStackWalker.cpp:78-91`
  (impl at `:78-83` and `:86-91`). Arg-slot constants are Function=0,
  CallInfo=1, This=2 — `lib/Runtime/Language/JavascriptFunctionArgIndex.h:20-23`.
- `[x64 fact]` The x64 `Amd64StackFrame::GetArgv` returns `frame + 2`
  (rbp-relative `[rbp][retaddr]` frame model) —
  `lib/Runtime/Language/amd64/StackFrame.h:133-165` (method at `:144`).
- `[x64 fact]` `CallInfo` packs `Flags` + `Count`; flags include
  `CallFlags_InternalFrame = 0x80` — `lib/Runtime/Base/CallInfo.h:11-19`
  (enum at `:11-19`, `CallFlags_InternalFrame` at `:19`).
- `[x64 fact]` `AsmJsFunctionInfo` stores per-arg types (`mArgType`), sizes
  (`mArgSizes`, `mArgByteSize`), a *single* return type (`mReturnType`), and
  the wasm `WasmSignature` — `lib/Runtime/Language/AsmJsTypes.h:878-1000`
  (fields at `:880-892`, accessors at `:934-999`).

### 1.2 Type universes

- `[x64 fact]` `AsmJsType::Which` is the full asm.js expression-type
  universe: `Int, Int64, Double, Float, MaybeDouble, DoubleLit, MaybeFloat,
  Floatish, …, Fixnum, Signed, Unsigned, Intish, Void`, plus SIMD types —
  `lib/Runtime/Language/AsmJsTypes.h:101-129`.
- `[x64 fact]` `AsmJsRetType::Which` is the single-return subset: `Void,
  Signed, Int64, Double, Float, Fixnum, Unsigned, Floatish` (+ SIMD under
  `ENABLE_WASM_SIMD`) — `lib/Runtime/Language/AsmJsTypes.h:179-203`.
- `[x64 fact]` `AsmJsVarType::Which` is the variable/argument subset:
  `Int, Int64, Double, Float` (+ SIMD) — `lib/Runtime/Language/AsmJsTypes.h:244-262`.
- `[x64 fact]` The wasm value-type universe `WasmTypes::WasmType` is
  `Void=0, I32=1, I64=2, F32=3, F64=4, V128=5`, with `Ptr, Any` as extra
  non-local sentinels — `lib/WasmReader/WasmParseTree.h:36-60`.
- `[x64 fact]` `WasmSignature` stores params (`m_params`), results
  (`m_results`), counts (`m_paramsCount`, `m_resultsCount`), a computed
  `m_paramSize` and a packed `m_shortSig` — `lib/WasmReader/WasmSignature.h:41-49`
  (fields at `:42-48`).

### 1.3 External entry: unbox / call / box (x64)

- `[x64 fact]` The external entrypoint is `AsmJsExternalEntryPoint`
  (`RecyclableObject*, CallInfo, ...`); its x64 body is the native thunk
  `AsmJsExternalEntryPoint` in `amd64_Thunks.S/.asm` — see
  `lib/Runtime/Language/amd64/amd64_Thunks.S:202-281` (entry at `:213-215`).
  It: pushes `rbp`, `sub rsp, 0x40`, saves `r12/r13`, calls
  `GetStackSizeForAsmJsUnboxing`, calls `UnboxAsmJsArguments`, calls the
  target (`call rax`, `:253-254`), then calls `BoxAsmJsReturnValue`
  (`:256-268`) and restores the stack (`:270-280`).
- `[x64 fact]` The x86 inline-asm variant `AsmJsExternalEntryPoint` aligns
  arg size to 8, `_alloca`s the buffer, calls `UnboxAsmJsArguments`, then
  switches on return type with `__asm` `call ecx` / `mov eax, …` /
  `mov edx, …` (i64), `movsd xmm0` (f64), `movss xmm0` (f32),
  `movups xmm0` (SIMD) — `lib/Runtime/Language/AsmJsUtils.cpp:421-559`
  (unbox call at `:437`, return switch at `:440-556`).
- `[x64 fact]` `UnboxAsmJsArguments` walks `AsmJsFunctionInfo` arg types in
  order and converts each `Var` from `origArgs`: int via `ToInt32`, float
  via `(float)ToNumber`, double via `ToNumber` — `lib/Runtime/Language/AsmJsUtils.cpp:181-333`
  (loop at `:194`, int at `:197-221`, float at `:268-292`, double at
  `:293-315`). It skips the `ScriptFunction` slot by `argDst + MachPtr`
  (`:193`). On `TARGET_64` int and float slots are zero-padded to `MachPtr`
  (`:216-220`, `:287-291`).
- `[x64 fact]` i64 args on x64 throw `WASMERR_InvalidTypeConversion` unless
  `WasmI64` debug config is on, in which case a test-only object
  (`{low, high}`) or string path is used — `lib/Runtime/Language/AsmJsUtils.cpp:222-267`
  (throw at `:228`, debug-object path at `:239-252`).
- `[x64 fact]` SIMD args throw `WASMERR_InvalidTypeConversion`
  ("Todo:: support test input for wasm.simd") — `lib/Runtime/Language/AsmJsUtils.cpp:316-320`.
  The fallback arm asserts `UNREACHED` then throws the same — `:321-325`.
- `[x64 fact]` `BoxAsmJsReturnValue` switches on
  `info->GetReturnType().which()`: `Void`→`OP_LdUndef`, `Signed`→
  `ToVar((int)intRetVal)`, `Double`→`NewWithCheck(doubleRetVal)`,
  `Float`→`NewWithCheck(floatRetVal)`, i64→`ThrowTypeError
  (WASMERR_InvalidTypeConversion)` (or debug `{low,high}` object under
  `WasmI64`), SIMD→same throw, default `Assume(UNREACHED)` + throw —
  `lib/Runtime/Language/AsmJsUtils.cpp:359-419` (switch at `:366`, i64 at
  `:376-385`, SIMD at `:397-412`, default at `:413-415`).
- `[x64 fact]` `GetStackSizeForAsmJsUnboxing` computes
  `argSize = GetArgByteSize() + MachPtr`, aligns to 16, floors at 32
  ("convention is to always allocate spill space for rcx,rdx,r8,r9"), then
  `PROBE_STACK_CALL(..., argSize + MinStackDefault)` —
  `lib/Runtime/Language/AsmJsUtils.cpp:344-357`.
- `[x64 fact]` `WASMERR_InvalidTypeConversion` is
  `"Invalid WebAssembly type conversion %s to %s"` / TypeError —
  `lib/Parser/rterrors.h:400` (single line at `:400`).
- `[x64 fact]` Per-arg stack sizes in `AsmJsFunctionInfo::SetArgType`:
  double→`sizeof(double)`, SIMD→`sizeof(AsmJsSIMDValue)`, int64→
  `sizeof(int64)`, int/float→`MachPtr` — `lib/Runtime/Language/AsmJsTypes.cpp:866-902`
  (switch at `:882-898`).
- `[x64 fact]` `AsmJsFunctionInfo::Init` always allocates `mArgSizes` for
  `max(mArgCount, 3)` with the comment "on x64, AsmJsExternalEntryPoint
  reads first 3 elements to figure out how to shadow args on stack" —
  `lib/Runtime/Language/AsmJsTypes.cpp:816-842` (comment+alloc at `:827-830`).
- `[x64 fact]` `WasmSignature::GetParamSize` returns `sizeof(void*)` (8) for
  F32/I32 on `_M_X64` ("on x64, we always alloc (at least) 8 bytes per
  arguments"), `sizeof(int64)` for F64/I64, 16 for V128, and asserts
  `UNREACHED` on other arches — `lib/WasmReader/WasmSignature.cpp:129-161`
  (x64 arm at `:136-138`, assert arm at `:141-143`).

### 1.4 Interpreter entries

- `[x64 fact]` The dynamic-interpreter dispatch symbol
  `AsmJsDefaultEntryThunk` is defined as
  `InterpreterStackFrame::AsmJsDelayDynamicInterpreterThunk` —
  `lib/Common/BackendApi.h:11` (single line at `:11`).
- `[x64 fact]` The non-x64 fallback for that thunk is
  `AssertOrFailFast(UNREACHED)` with the comment "Asm.js only supported on
  x64 and x86" — `lib/Runtime/Language/InterpreterStackFrame.cpp:1759-1766`
  (fallback at `:1760-1765`).
- `[x64 fact]` `WasmLibrary::ResetFunctionBodyDefaultEntryPoint` sets
  `jsMethod = AsmJsDefaultEntryThunk` and
  `SetIsAsmJSFunction(true)` — `lib/Runtime/Library/WasmLibrary.cpp:24-32`
  (body at `:24-31`, entry at `:26-28`).
- `[x64 fact]` `WasmLibrary::EnsureWasmEntrypoint` generates bytecode for
  unparsed wasm bodies, then installs `AsmJsDefaultEntryThunk` (or the lazy
  trap callback when an error was recorded) — `lib/Runtime/Library/WasmLibrary.cpp:37-93`
  (body at `:37-90`, install at `:58-59` and `:83`).
- `[x64 fact]` On x86, `AsmJsInterpreter(AsmJsCallStackLayout* stack,
  byte* retDst)` builds `nbArgs = ArgCount + 1` with `CallInfo(Value,
  nbArgs)`, calls `InterpreterHelper`, then writes the result to `retDst`
  by `GetRetType` — `lib/Runtime/Language/InterpreterStackFrame.cpp:2211-2268`
  (entry at `:2212`, call at `:2214-2226`, writeback at `:2228-2267`).
  The x86 `InterpreterAsmThunk` naked-asm wrapper copies the 16-byte
  return slot from `xmm0`/`eax`/`edx` — `lib/Runtime/Language/InterpreterStackFrame.cpp:1638-1701`
  (entry at `:1638-1639`, tail-copy at `:1670-1699`).
- `[x64 fact]` On x64, `AsmJsInterpreter<T>(AsmJsCallStackLayout*)` is a
  host template over the return type that wraps `InterpreterHelper` and
  returns `asmJsReturn.GetRetVal<T>()` —
  `lib/Runtime/Language/InterpreterStackFrame.cpp:2329-2345` (entry at
  `:2329-2330`, helper at `:2332-2344`).
- `[x64 fact]` `GetAsmJsInterpreterEntryPoint` dispatches on return type:
  Double→`AsmJsInterpreter<double>`, Float→`<float>`,
  Signed/Void→`<int>`, Int64→`<int64>`, SIMD→`AsmJsInterpreterSimdJs`
  (`__m128` wrapper, `:2347-2350`) — `lib/Runtime/Language/InterpreterStackFrame.cpp:2276-2324`
  (dispatch at `:2276-2322`, SIMD arm at `:2303-2318`).
- `[x64 fact]` `AsmJsReturnStruct` is the single-return carrier:
  `int32 i; int64 l; float f; double d; AsmJsSIMDValue simd` with
  `GetRetVal<T>()` specializations — `lib/Runtime/Language/InterpreterStackFrame.h:94-110`
  (struct at `:94-109`).
- `[x64 fact]` The x64 native thunk `InterpreterAsmThunk` (in
  `amd64_Thunks.S`) pushes `rbp`, saves `rdi/rsi` (the AAPCS64/sysv arg
  registers), calls `GetAsmJsInterpreterEntryPoint`, then `call rax` and
  returns — `lib/Runtime/Language/amd64/amd64_Thunks.S:171-200` (entry at
  `:179`, dispatch at `:191`, indirect call at `:196`).
- `[x64 fact]` The asm.js external interpreter entry is installed by
  `scriptFuncObj->GetDynamicType()->SetEntryPoint(AsmJsExternalEntryPoint)`
  at link time, with `entrypointInfo->jsMethod = AsmJsDefaultEntryThunk`
  when the JIT-template phase is off —
  `lib/Runtime/Language/InterpreterStackFrame.cpp:2808-2839` (install at
  `:2823`, `:2832-2837`). JIT readiness is gated on
  `GetEntryPoint() == AsmJsExternalEntryPoint` before calling
  `WAsmJs::JitFunctionIfReady` — `:2869-2878`.
- `[x64 fact]` `InterpreterStackFrame.h` declares the `_M_IX86` vs
  `_M_X64` split for these entry points
  (`AsmJsInterpreter(..., byte*)` on x86; templated `<T>` + entry-point
  selector + `AsmJsInterpreterSimdJs` on x64) —
  `lib/Runtime/Language/InterpreterStackFrame.h:349-364` (split at
  `:349-359`, thunks at `:361-364`).

### 1.5 JIT lowering call paths (x64 reference)

- `[x64 fact]` `LowerAsmJsCallE` lowers args with `extraParams=1` (function
  object) and `LowerAsmJsCallI` with `extraParams=0`, both then calling the
  common `LowerCall` — `lib/Backend/amd64/LowererMDArch.cpp:1082-1101`
  (E at `:1083-1091`, I at `:1094-1101`).
- `[x64 fact]` `LowerCallArgs` moves each IR arg to its arg-slot operand,
  asserts `StartCall` arity (with an `IsAsmJsMode()` exemption for the
  assert) — `lib/Backend/amd64/LowererMDArch.cpp:456-564` (loop at
  `:469-512`, assert at `:531-536`).
- `[x64 fact]` `LowerCallIDynamic` homes 4 args to registers on a normal
  (non-inlinee) path and manually calls `GeneratePreCall` for the function
  object — `lib/Backend/amd64/LowererMDArch.cpp:599-630` (body at
  `:600-629`).
- `[x64 fact]` `LowerWasmArrayBoundsCheck` returns the instruction
  unchanged for the fast-virtual-buffer case, otherwise emits an explicit
  compare-branch to `GenerateThrow(WASMERR_ArrayIndexOutOfRange)` —
  `lib/Backend/amd64/LowererMDArch.cpp:1103-1146` (fast-path return at
  `:1117-1120`, throw at `:1142-1143`).
- `[x64 fact]` `LowerAsmJsLdElemHelper` bounds-checks on xplat
  unconditionally ("Always do bound check. We don't support out-of-bound
  access violation recovery", `:1209-1210`) and falls back to `NaN`/`0` or
  `RuntimeRangeError` on failure (`:1262-1276`) — `lib/Backend/amd64/LowererMDArch.cpp:1188-1287`
  (range at `:1188-1287`).
- `[x64 fact]` `LoadFloatHelperArgument` and `LoadDoubleHelperArgument`
  both forward to `LoadHelperArgument` with a float-32 / float-64 type
  assert — `lib/Backend/amd64/LowererMDArch.cpp:1465-1490` (double arm at
  `:1465-1483`, float arm at `:1485-1490`).
- `[x64 fact]` `LowerExitInstrAsmJs` just forwards to `LowerExitInstr`
  ("epilogue is almost identical on x64, except for return register") —
  `lib/Backend/amd64/LowererMDArch.cpp:2206-2211` (body at `:2207-2210`).
- `[decision]` Baseline for W4 (existing ARM64 code, not x64): the ARM64
  backend stubs all of the above — `GenerateCtz`/`GeneratePopCnt`/
  `GenerateTruncWithCheck` at `lib/Backend/arm64/LowerMD.h:104-107`,
  `HelperCallForAsmMathBuiltin` at `:122`, `LowerAsmJsCallI/E`, bounds
  check, atomics, ld/st-elem helpers at `:166-172`,
  `LoadFloatHelperArgument` at `:178`, `EmitFloat32ToFloat64`/
  `EmitInt64toFloat`/`EmitSignExtend`/`EmitReinterpretPrimitive` at
  `:187-194`, `LowerExitInstrAsmJs` at `:202`. §3–§8 below must replace
  each stub.
- `[x64 fact]` `LOWER_SPLIT_INT64` is `1` only on `_M_IX86` and `0`
  otherwise (so x64 and ARM64 both use native 64-bit regs) —
  `lib/Common/CommonDefines.h:715-719` (define at `:715-719`).
- `[x64 fact]` `GenerateStackAllocation` subtracts directly for
  `size <= PAGESIZE` and otherwise brackets a `_chkstk` call —
  `lib/Backend/amd64/LowererMDArch.cpp:1492-1550` (pagesize arm at
  `:1507-1515`, chkstk arm at `:1516-1549`).

### 1.6 Traps and errors (baseline text is the oracle)

- `[x64 fact]` Trap error IDs and baseline text: `WASMERR_Unreachable` =
  "Unreachable Code" (`kjstWebAssemblyRuntimeError`), `WASMERR_DivideByZero`
  = "Division by zero", `WASMERR_ArrayIndexOutOfRange` = "Memory index is
  out of range", `WASMERR_InvalidTypeConversion` =
  "Invalid WebAssembly type conversion %s to %s" (`kjstTypeError`) —
  `lib/Parser/rterrors.h:391-424` (Unreachable at `:392`, conversion at
  `:400`, divide at `:401`, OOB at `:408`).
- `[x64 fact]` Interpreter divide traps are explicit checks, not signals:
  `OP_DivOverflow`/`OP_RemOverflow` test `aRight == 0` and throw
  `WASMERR_DivideByZero`, plus `MinValue/-1` handling — `lib/Runtime/Language/InterpreterStackFrame.cpp:8433-8461`
  (div at `:8433-8446`, rem at `:8448-8461`). Unsigned variant at
  `:8423-8431`.
- `[x64 fact]` `OP_Unreachable` calls `ThrowUnreachable`, which wraps
  `WASMERR_Unreachable` as a `WebAssemblyRuntimeError` —
  `lib/Runtime/Language/InterpreterStackFrame.cpp:8463-8466` and
  `lib/Runtime/Library/JavascriptError.cpp:324` (single line at `:324`).
- `[x64 fact]` Interpreter memory traps: `WasmArrayBoundsCheck` compares
  `index + sizeof(MemType) > byteLength` and throws
  `WASMERR_ArrayIndexOutOfRange`; atomic variant first checks alignment
  and throws `WASMERR_UnalignedAtomicAccess` —
  `lib/Runtime/Language/InterpreterStackFrame.cpp:9233-9253` (bounds at
  `:9233-9240`, atomic at `:9242-9253`).
- `[x64 fact]` Interpreter i64 JS-conversion failures go through
  `OP_InvalidWasmTypeConversion`, which throws
  `WASMERR_InvalidTypeConversion` with `I64 ↔ Javascript Variable` names —
  `lib/Runtime/Language/InterpreterStackFrame.cpp:2432-2444` (body at
  `:2432-2444`). The interpreter handler table routes `Conv_VTL` and
  `ArgOut_Long` here — `lib/Runtime/Language/InterpreterHandlerAsmJs.inl:39-40`
  (lines at `:39-40`).
- `[x64 fact]` JIT bounds traps use `GenerateThrow` with an explicit
  `WASMERR_ArrayIndexOutOfRange` operand (not a guard-page fault):
  `LowerWasmArrayBoundsCheck` at `lib/Backend/amd64/LowererMDArch.cpp:1142-1143`.
  The generic `Lower.cpp` path uses `GenerateRuntimeError(...
  WASMERR_ArrayIndexOutOfRange, HelperOp_WebAssemblyRuntimeError)` —
  `lib/Backend/Lower.cpp:9722` (single line at `:9722`). Divide-by-zero
  uses `GenerateThrow(WASMERR_DivideByZero)` — `lib/Backend/Lower.cpp:27454`
  and `:27462`.
- `[x64 fact]` Non-trapping float→int trunc: the saturating variant clamps
  (NaN→0, too-small→MIN, overflow→MAX) while the trapping variant throws
  `VBSERR_Overflow` on out-of-range — `lib/Backend/LowerMDShared.cpp:4955-5025`
  (checks at `:4955-5024`, non-saturate throw at `:5013-5016`) and
  `lib/Backend/LowerMDShared.cpp:5027-5063` (entry at `:5027`).
- `[x64 fact]` The only `WASMERR_InvalidTypeConversion` throw sites in
  `Unbox/Box` preserve today's JS-boundary behavior — see §1.3 citations
  above (`AsmJsUtils.cpp:228, 319, 324, 385, 411, 415`); plus the typed
  external-entry variants in
  `lib/Runtime/Library/WebAssemblyInstance.cpp:246, 255, 388, 390, 430`.
- `[x64 fact]` Compile-time invalid conversion in `WasmSignature` /
  `WebAssemblyModule` paths (bad param/result types, signature mismatch)
  throws `WasmCompilationException` / `WASMERR_SignatureMismatch` /
  `WASMERR_LinkSignatureMismatch` — `lib/Parser/rterrors.h:405, 414` and
  `lib/WasmReader/WasmSignature.cpp:157-160` (default arm at `:157-160`).

### 1.7 Stack-walk frame recognition

- `[x64 fact]` `CheckJavascriptFrame` first matches an interpreter frame
  by `codeAddr == tempInterpreterFrame->GetReturnAddress()`, then checks
  library frames, then native frames via
  `JavascriptFunction::IsNativeAddress` —
  `lib/Runtime/Language/JavascriptStackWalker.cpp:937-1045` (interpreter
  arm at `:941-987`, native arm at `:1004-1044`).
- `[x64 fact]` Native asm.js frames are explicitly excluded from the
  walker: after `IsNativeAddress` the walker returns `false` when
  `funcObj->GetFunctionBody()->GetIsAsmjsMode()` is true, because "call
  info is not valid for asm.js" —
  `lib/Runtime/Language/JavascriptStackWalker.cpp:1016-1020` (check at
  `:1017-1020`, comment at `:1022`).
- `[x64 fact]` `GetIsAsmjsMode()` reads `m_isAsmjsMode` on `FunctionBody`
  — `lib/Runtime/Base/FunctionBody.h:1587-1590`
  (accessor at `:1587`). The per-function asm.js flag is
  `GetIsAsmJsFunction()` / `m_isAsmJsFunction` —
  `lib/Runtime/Base/FunctionBody.h:2461-2469` and `:1826` (field at
  `:1826`, accessor at `:2467-2469`).
- `[x64 fact]` `InterpreterFrame` vs native distinction for
  `GetCurrentCodeAddr` / `GetArgv`: native frames read argv directly from
  the x64 stack frame (`frame+2`), interpreter frames via the
  `InterpreterStackFrame` chain —
  `lib/Runtime/Language/JavascriptStackWalker.cpp:923-935` (argv at
  `:930-934`) and `:1047-1052`.

### 1.8 Multi-value (today: single-return machinery)

- `[x64 fact]` `AsmJsRetType` is single-valued (`mReturnType` in
  `AsmJsFunctionInfo` is one `AsmJsRetType`, not an array) —
  `lib/Runtime/Language/AsmJsTypes.h:176-218` (class at `:176-218`) and
  `:886, 936-937` (field at `:886`, accessor at `:936-937`).
- `[x64 fact]` `AsmJsReturnStruct` is single-valued (see §1.4 citation).
  The x64 interpreter entry-point selector has exactly one template per
  return type and no multi-return arm — `lib/Runtime/Language/InterpreterStackFrame.cpp:2276-2324`.
- `[x64 fact]` `WasmSignature` *does* store multiple results:
  `AllocateResults(count)` + `SetResult`/`GetResult` + `GetResultCount` —
  `lib/WasmReader/WasmSignature.cpp:45-74` (alloc at `:45-56`, get at
  `:67-74`). `FinalizeSignature` explicitly bails on `GetResultCount() >
  1` ("Do not support short signature with multiple returns at this
  time") — `:163-169`.
- `[x64 fact]` `WasmSignature.h` exposes `GetResultCount()` /
  `GetResult(index)` — `lib/WasmReader/WasmSignature.h:24-27` (lines at
  `:24-27`). `WriteSignatureToString` prints multi-results as
  `)->(T0, T1, …)` — `lib/WasmReader/WasmSignature.cpp:288-300`.

### 1.9 Registers and buffer paths (for the callee-save decision)

- `[x64 fact]` On Windows x64, callee-saved int regs are
  RBX, RSI, RDI, R12–R15 and callee-saved XMM regs are XMM6–XMM15 —
  `lib/Backend/amd64/RegList.h:22-57` (int at `:27-39`, XMM at `:47-56`).
  On SysV x64, XMM6–XMM15 are *not* callee-saved (`:83-92`).
- `[decision]` Target-side register evidence (existing ARM64 code, not
  x64) for §9: on ARM64, callee-saved int regs are R19–R28 (x19–x28) and
  callee-saved FP regs are D8–D15; R16/R17 are linker-reserved scratch and
  R18 (x18) is the platform/TEB register and must not be touched —
  `lib/Backend/arm64/RegList.h:36-51` (scratch/TEB at `:36-38`, callee-save
  at `:39-48`) and `:68-75` (D8–D15). `INT_ARG_REG_COUNT` is 8 —
  `:55-57`.
- `[x64 fact]` The generic typed-array buffer path (`isOptimizedBuffer =
  false`) is the existing `#else` shape; the optimized/virtual-buffer path
  is `_M_X64`-only with `Assert(UNREACHED)` elsewhere —
  `lib/Runtime/Language/AsmJsModule.cpp:1841-1849` (x64 arm at `:1841-1842`,
  else at `:1845-1848`).

## 2. Boundary rule (which convention at which boundary)

- `[decision]` **External boundary (JS → asm.js/wasm): `Var`/`CallInfo`,
  NOT AAPCS64.** The JS caller presents a `JavascriptCallStackLayout`
  (`functionObject`, `CallInfo`, `Var args[]`). The external thunk does
  unbox → call → box in C++/thunk code and never lets a `Var` cross into
  wasm code. Justification: x64 `AsmJsExternalEntryPoint` does exactly this
  (`UnboxAsmJsArguments` in, `BoxAsmJsReturnValue` out, target reached by
  indirect call), and the walker contract for external frames is
  `functionObject`/`CallInfo`/`args` (§1.1). Forcing AAPCS64 at this
  boundary would break `CallInfo`, `this`, and tagged-int handling.
- `[decision]` **Internal boundaries (wasm → wasm, wasm ↔ interpreter
  thunks, wasm → runtime helpers): AAPCS64-derived internal convention.**
  Unboxed `i32/i64/f32/f64` move in the AAPCS64 argument registers
  (W0–W7 for ints, D0–D7 for floats; §3). Justification: this is
  where the x64 "first-3 shadow" convention and Win64/SysV register split
  live; ARM64 has one target ABI (AAPCS64), so we adopt it directly.
- `[decision]` **JS-return boundary (wasm → JS): boxing lives in the
  external/interpreter thunk, not in wasm code.** The wasm callee returns
  unboxed values in AAPCS64 return registers (W0 / S0 / D0); the thunk
  boxes per `AsmJsRetType` / `WasmSignature` into a `Var`. Exception:
  multi-value returns (§7) box N values, not one. Justification: x64
  `BoxAsmJsReturnValue` already centralizes boxing per return type (§1.3);
  keeping it out of wasm code preserves baseline error text (§8).
- `[decision]` **Unresolved pointers:** none for §2. The remaining
  `Var`/`CallInfo` question is *where* the layout struct is defined per
  arch (§9, Q1).

## 3. Internal register assignment (AAPCS64-derived)

- `[decision]` **Adopt AAPCS64 argument/return registers for all internal
  asm.js/wasm calls.** Integer args take `x0–x7` (W0–W7), float args take
  `d0–d7` (S0–S7 viewed as single precision; §4), return in `x0`/`s0`/`d0`
  by type. Justification: AAPCS64 is the host ABI on darwin/arm64; the x64
  internal convention is already ABI-shaped (first-4 shadow + return-reg
  discipline, §1.3/§1.5) but is Win64-specific, so ARM64 takes the local
  ABI instead.
- `[decision]` **The x64 "first-3 shadow" optimization is REPLACED, not
  ported.** x64 `mArgSizes` is sized `max(mArgCount, 3)` so the external
  entrypoint can read 3 shadow slots without a walk (§1.3,
  `AsmJsTypes.cpp:827-830`). On ARM64 the shadow is the 8-register
  AAPCS64 window: the first 8 int args (and first 8 float args) are
  register-passed, the rest are stack-passed per AAPCS64. The external
  thunk does not peek at "first 3" sizes; it uses `GetArgByteSize()` +
  the §6 alignment rule. Justification: ARM64 needs no 3-slot shadow
  (wider register window, 8 arg regs), and the invariant "unboxed args are
  in AAPCS64 locations" is simpler to verify than a shadow-size table.
- `[decision]` **Register assignment by value class (internal):**
  - `i32`: `x0–x7` in order (zero-extended W-register view).
  - `i64`: `x0–x7` in order (full 64-bit, §5).
  - `f32`: `s0–s7` (low half of `d0–d7`); see §4 for the low-bit rule.
  - `f64`: `d0–d7`.
  - `v128` (W8 only): `q0–q7` when the NEON backend lands; until then
    SIMD-typed signatures fail controlled (§8). Justification: mirrors
    AAPCS64 classes (int → X, float → S/D, vector → Q).
- `[decision]` **Stack-passed overflow (9th+ int arg, 9th+ float arg):
  AAPCS64 stack layout, 8-byte slots, caller-pushes.** The caller writes
  each overflow arg at `SP + 8*k` for `k = 0,1,…` in order. The callee
  reads its locals from `FP + 16 + 8*k` (after `FP = SP` prolog save).
  Justification: standard AAPCS64 overflow; W3/W4 tests can verify both
  sides with one diagram (§6).
- `[decision]` **Do NOT reuse the x64 `extraParams` trick for internal
  calls.** x64 `LowerAsmJsCallE` passes `extraParams=1` (hidden function
  object) vs `AsmJsCallI` `extraParams=0` (§1.5). On ARM64 internal calls
  pass exactly the wasm args in AAPCS64 registers — no hidden function
  object slot. The function object is needed only by external/interpreter
  thunks (§2). Justification: AAPCS64 has no x64-style "shadow" slot to
  repurpose, and hiding it in `x0` would collide with the first wasm arg.
- `[open]` Whether the JIT backend's existing `LowerCallArgs` /
  `HomeJsCallArgs` machinery (which homes args for `Var` calls) can be
  reused for the internal convention or needs a new `LowerAsmJsCallArgs`
  entry. This is a W4 implementation question; the ABI above fixes the
  *result*, not the lowering path.

## 4. float32 transport at each boundary

- `[x64 fact]` On x64, f32 is transported in a full `MachPtr` slot on the
  stack (unbox zero-pads to `MachPtr`; `GetParamSize` = 8 bytes for F32) —
  `lib/Runtime/Language/AsmJsUtils.cpp:287-291`,
  `lib/WasmReader/WasmSignature.cpp:136-138`. Returns arrive in `xmm0`
  (`movss fval, xmm0`, `AsmJsUtils.cpp:509-522`) and are boxed via
  `NewWithCheck((double)fval)` (`:521`).
- `[x64 fact]` JIT helper-arg lowering treats f32 as a real float32
  operand (`LoadFloatHelperArgument` asserts `IsFloat32()` and forwards
  to `LoadHelperArgument`) — `lib/Backend/amd64/LowererMDArch.cpp:1485-1490`.
- `[decision]` **Internal wasm → wasm / wasm → helper: f32 in `s0–s7`
  (low 32 bits of `d0–d7`), with the high 32 bits ZEROED on write.**
  Every f32-producing instruction must clear the upper half (ARM64
  `fmov s` already does; any `s`-from-`d` move must explicitly zero the
  upper bits if the encoder path does not). Justification: AAPCS64 single
  precision passes in S registers; zeroing the upper half makes the value
  well-defined under `d` reads (e.g. a `d` load of the same slot) and
  matches the "low bits meaningful, high bits defined-zero" rule the
  decoder needs for canonicalization tests.
- `[decision]` **JS → wasm (external thunk): f32 converted to a
  single-precision `float`, then moved into `s0–s7` / stack slot as a
  4-byte value zero-extended to 8 bytes in memory (high 4 bytes zero).**
  The unboxer keeps x64's shape (`(float)ToNumber`, missing arg → NaN) but
  stores 4+4 bytes instead of the x64 `MachPtr` zero-pad. Justification:
  preserves x64 conversion semantics (§1.3) while matching AAPCS64 stack
  slot sizes (f32 is a 4-byte stack item, not 8).
- `[decision]` **wasm → JS (box): f32 in `s0` widened to `double` then
  `NewWithCheck(double)`.** The upper-half-zero rule (§4, first bullet)
  guarantees the widening is exact. Justification: identical to x64
  `BoxAsmJsReturnValue` Float arm (`NewWithCheck(floatRetVal)`, §1.3);
  no new JS-number behavior.
- `[decision]` **Stack-passed f32 overflow slots are 4 bytes on the stack
  (packed 8-byte slots, high 4 bytes zero), NOT 8 bytes.** Consecutive
  overflow args each take one 8-byte AAPCS64 slot with the f32 in the low
  4 bytes. Justification: AAPCS64 stack args are 8-byte-aligned slots
  regardless of value size; this keeps the overflow layout uniform
  (same as i32).
- `[open]` Encoder support for `FMOV S ↔ W` (bitcast) vs `FCVT D→S`:
  W4 must pick the NEON/SIMD encoding that preserves NaN payload bits
  per the spec suite and record the choice here. The ABI rule above
  (low bits = f32 value, high zero) is independent of that choice.

## 5. i64: single-register

- `[x64 fact]` `LOWER_SPLIT_INT64` is 0 on x64 (native 64-bit regs, no
  split-pair) — `lib/Common/CommonDefines.h:715-719`. The only
  split-i64 shape in the tree is the x86 inline-asm `eax;edx` pair in
  `AsmJsExternalEntryPoint` — `lib/Runtime/Language/AsmJsUtils.cpp:469-482`.
- `[decision]` **i64 occupies exactly one X register (`x0–x7`) on ARM64.**
  No low/high pair splitting anywhere (external, internal, interpreter
  thunks). The `Conv_VTL`/`ArgOut_Long` interpreter handlers operate on
  the single 64-bit slot value. Justification: ARM64 has native 64-bit
  integer ops and `LOWER_SPLIT_INT64 = 0`; reintroducing a pair would
  double every i64 boundary for no benefit.
- `[decision]` **i64 ↔ JS boundary behavior is PRESERVED, not
  redesigned.** JS → i64 arg: throw `WASMERR_InvalidTypeConversion`
  (TypeError) unless the `WasmI64` debug path is on — identical to x64
  `UnboxAsmJsArguments` (§1.3). wasm → JS return: same throw (or debug
  `{low, high}` object under `WasmI64`) — identical to x64
  `BoxAsmJsReturnValue` (§1.3). No BigInt path is introduced in this
  tranche. Justification: D5 (Linux x64 is the behavioral oracle); the
  spec fixtures encode the TypeError today and changing it here would
  invalidate the WasmSpec baselines.
- `[decision]` **i64 stack slot is 8 bytes** (both as an overflow arg and
  in the unbox buffer). There is no "pair" slot. Justification: follows
  directly from the single-register rule.

## 6. Stack layout, alignment, stack-probe policy

- `[x64 fact]` x64 external unbox buffer: `GetArgByteSize() + MachPtr`,
  `Align(…, 16)`, floor 32 for the rcx/rdx/r8/r9 spill slots, then
  `PROBE_STACK_CALL` — `lib/Runtime/Language/AsmJsUtils.cpp:344-357`.
  x86 equivalent: `Align(…, 8)` + `_alloca` — `:432-436`.
- `[x64 fact]` x64 JIT stack allocation: direct `sub rsp` up to
  `PAGESIZE`, else a `_chkstk` call — `lib/Backend/amd64/LowererMDArch.cpp:1492-1550`.
- `[decision]` **External unbox buffer (JS → wasm thunk), ARM64:**
  `argBufferSize = Align(GetArgByteSize() + 8, 16)` with a floor of **32**
  bytes (retained for shape compatibility with the x64 floor; on ARM64
  the 32 bytes cover the AAPCS64 link register + callee-save pair spill,
  §9). The buffer is 16-byte aligned. Justification: `GetArgByteSize()`
  already counts each unboxed arg at its x64 slot size (§1.3) which equals
  the ARM64 stack slot size for every type except f32/i32, where 8 where
  x64 uses 8 is already correct (§4–§5); `+8` accounts for the function
  object slot that `UnboxAsmJsArguments` skips (`MachPtr` on x64 = 8).
- `[decision]` **Frame layout (internal JIT wasm frame), ARM64:**
  ```
  [higher addr]  caller stack args (overflow args, 8-byte slots) …
  [FP+16+8k]     overflow arg k
  [FP+8]         saved LR (x30)
  [FP+0]         saved FP (x29)
  [FP-8 …]       callee-saved regs spilled (x19–x28, d8–d15) per §9
  [SP …]         locals / spill / multi-value return area (§7)
  ```
  `SP` is 16-byte aligned on entry (AAPCS64 requirement); every `sub SP`
  size is rounded up to a multiple of 16. Justification: AAPCS64 mandates
  16-byte `SP` alignment at function entry; the x64 analogue is the
  `Align(…, 16)` in `GetStackSizeForAsmJsUnboxing` (§1.3).
- `[decision]` **Stack-probe policy (external thunk): KEEP
  `PROBE_STACK_CALL(scriptContext, func, bufferSize + MinStackDefault)`.**
  The probe is executed before the unbox buffer is touched. Justification:
  the x64 thunk probes in `GetStackSizeForAsmJsUnboxing` (§1.3) and the
  interpreter recursion limits depend on it; dropping it would change
  overflow behavior (and therefore `RangeError` timing) vs the oracle.
- `[decision]` **Stack-probe policy (JIT wasm bodies): REUSE the existing
  ARM64 `GenerateStackAllocation`** — direct `sub SP` for small stacks
  (`IsSmallStack(probeSize)`), `_chkstk` helper call
  (`HelperCRT_chkstk`, size in `SP_ALLOC_SCRATCH_REG`) for
  `size > PAGESIZE - MachStackAlignment`, exactly as the JS JIT does on
  ARM64. Justification: the ARM64 backend already implements this
  (`lib/Backend/arm64/LowerMD.cpp:1064-1114`, small arm at `:1073-1082`,
  chkstk arm at `:1086-1113`, helper at `:1098`); duplicating it for wasm
  would add a second probe path to audit. Wasm bodies must go through the
  same entry, not emit their own probe sequence.
- `[decision]` Supporting evidence for the reuse call above: the ARM64
  backend already implements this policy — `GenerateStackAllocation`
  emits `sub SP` for small stacks and a `HelperCRT_chkstk` call
  (size/16 in the scratch reg, `BLR`, `sub SP, … lsl #4`) for large
  ones, `lib/Backend/arm64/LowerMD.cpp:1064-1114` (small arm at
  `:1073-1082`, chkstk arm at `:1086-1113`).

## 7. Multi-value returns (new area, no x64 precedent)

- `[x64 fact]` Today the machinery is single-return: `AsmJsRetType` +
  `AsmJsReturnStruct` + one interpreter entry template per type (§1.8).
  `WasmSignature` already carries `m_results[]` / `m_resultsCount` /
  `GetResult*` (§1.8), but `FinalizeSignature` skips the packed short-sig
  for `>1` results — `lib/WasmReader/WasmSignature.cpp:163-169`.
- `[decision]` **Callee writes multi-value results to a caller-owned
  return area.** For `GetResultCount() <= 1` the callee returns in
  `W0`/`S0`/`D0` as today (§3). For `GetResultCount() >= 2` the caller
  passes a hidden `x8` (result-area pointer) as the *first* internal
  register, followed by the regular AAPCS64 arg registers shifted by one
  slot (`x0` … `x6`, `d0` … `d6`). The callee stores result `k` at
  `x8 + 8*k` in declared signature order (0 first). Justification:
  `x8` is the AAPCS64 indirect-result register (AAPCS64 §5.4.2); using
  the host convention beats inventing a new slot, and the area is exactly
  what the caller's `WasmSignature` describes (`m_results[k]`).
- `[decision]` **Ownership:** the caller allocates the result area on its
  own stack frame (in the §6 locals region, `Align(count*8, 16)` bytes,
  zero-initialized before the call) and keeps it live until the results
  are consumed. The callee never escapes the pointer (no storing it into
  globals, tables, or the heap). Justification: caller-owned / callee-
  written is the standard AAPCS64 indirect-result rule; it also matches
  the x86 `retDst` shape (§1.4: `AsmJsInterpreter(..., byte* retDst)`),
  which is caller-owned.
- `[decision]` **Alignment/ordering:** results are laid out in declared
  order at 8-byte stride from a 16-byte-aligned base. `i32`/`f32` occupy
  the low 4 bytes with high 4 zeroed (§4–§5 rules); `i64`/`f64` use the
  full 8 bytes. Justification: uniform 8-byte stride keeps the consumer
  loop (`for k: load x8 + 8*k`) trivial and matches the AAPCS64
  "each stack slot 8 bytes" rule from §3.
- `[decision]` **JS boxing (multi-value JS call):** the external thunk
  iterates `k = 0 … N-1` over the caller's area and boxes each slot per
  §2/§5 (i32→`ToVar`, f32/f64→`NewWithCheck`, i64→TypeError per §5,
  void only when `N == 0` → `OP_LdUndef`), then constructs the JS result
  tuple (`Array` of `N` `Var`s). Single-value JS calls keep the §2
  single-register box. Justification: x64 has no multi-value box
  (no code path exists, §1.8); boxing per-slot reuses the proven
  single-value boxers so baseline error text is preserved per slot.
- `[decision]` **Wasm → wasm multi-value:** the callee writes the area,
  the caller reads it back; no boxing. The `WasmSignature` on both sides
  must already agree (link-time check, §1.6:
  `WASMERR_SignatureMismatch`). Justification: internal calls stay in the
  AAPCS64 world (§2); boxing is an external-boundary concern only.
- `[decision]` **JIT → interpreter and interpreter → JIT multi-value:**
  the interpreter thunk (`AsmJsInterpreter` ARM64 form) must accept the
  same `x8` area pointer and fill it from `AsmJsReturnStruct`
  generalizations (one slot per result, same 8-byte stride). Justification:
  keeps interpreter and JIT tier behavior identical for W7.
- `[open]` Whether `AsmJsRetType` grows a multi-result arm or multi-value
  is carried *only* via `WasmSignature` (with `AsmJsRetType` staying
  single-valued and the area path keyed off `GetResultCount() > 1`).
  W3/W7 must decide before coding the thunk; this doc recommends the
  WasmSignature-keyed path (smaller shared-code churn, D6).

## 8. Trap style per op class (preserve baseline error text)

All trap styles below preserve the exact x64 error IDs and baseline text
(§1.6, D5 oracle). "Check-and-branch" means an explicit compare + branch
to a throw helper; "signal" would mean faulting into a handler. **No
signal-handler tricks in this tranche** (matches x64: all JIT traps are
explicit `GenerateThrow` / `GenerateRuntimeError`).

- `[decision]` **Memory bounds (loads/stores, all tiers): explicit
  check-and-branch → `GenerateThrow(WASMERR_ArrayIndexOutOfRange)` (JIT) /
  `ThrowWebAssemblyRuntimeError(WASMERR_ArrayIndexOutOfRange)`
  (interpreter).** Mirror `LowerWasmArrayBoundsCheck` /
  `WasmArrayBoundsCheck` (§1.6). Justification: x64 never faults for wasm
  OOB (guard-page fast path was removed on xplat; §1.5); explicit checks
  also work for the D3 generic buffer.
- `[decision]` **Divide/remainder by zero (scalar int): explicit zero
  check → `WASMERR_DivideByZero`.** Mirror `OP_DivOverflow` /
  `OP_RemOverflow` / `GenerateThrow(WASMERR_DivideByZero)` (§1.6).
  `INT_MIN / -1` (signed div) → `VBSERR_Overflow`; `INT_MIN % -1` → 0.
  Justification: preserves the two distinct x64 outcomes.
- `[decision]` **Unreachable: explicit unconditional throw →
  `WASMERR_Unreachable`.** Mirror `OP_Unreachable` → `ThrowUnreachable`
  (§1.6). Justification: wasm `unreachable` is a defined trap, not UB.
- `[decision]` **Float→int trunc (non-trapping / saturating): clamp, no
  throw.** NaN→0, below-min→MIN, above-max→MAX, exactly as
  `GenerateTruncChecks<Saturate=true>` (§1.6). **Trapping variant:
  explicit range check → `VBSERR_Overflow` throw.** Mirror
  `GenerateTruncChecks<Saturate=false>` (§1.6). Justification: the two
  modes have disjoint x64 outcomes; conflating them would fail the
  nontrapping suite.
- `[decision]` **Atomic unaligned access: explicit alignment check →
  `WASMERR_UnalignedAtomicAccess`.** Mirror
  `WasmAtomicsArrayBoundsCheck` (§1.6). Unaligned non-atomics never
  trap (wasm semantics).
- `[decision]` **i64 ↔ JS conversion failure, SIMD JS conversion
  failure, signature mismatch: `WASMERR_InvalidTypeConversion` /
  `WASMERR_SignatureMismatch` / `WASMERR_LinkSignatureMismatch`
  TypeErrors, exactly as x64 (§1.3, §1.6).** Never a signal, never an
  `Assert` in release. Justification: these are JS-visible errors whose
  baseline text the suites pin (§1.6).
- `[decision]` **SIMD-typed signatures before W8: controlled
  `WASMERR_InvalidTypeConversion` TypeError at the boundary** (same throw
  x64 uses for SIMD, §1.3). Never `Assert(UNREACHED)` in a reachable
  header. Justification: the W0 guardrail rule (stubs fail fast in a
  controlled way, plan W0.4).

## 9. Callee-save set and frame markers (walker / GC)

- `[x64 fact]` Windows-x64 callee-saved: RBX, RSI, RDI, R12–R15 (int) and
  XMM6–XMM15 (float) — `lib/Backend/amd64/RegList.h:27-56`. SysV x64 drops
  the XMM callee-save marking (`:83-92`).
- `[decision]` **ARM64 callee-saved set for wasm frames: `x19–x28`
  (`RegR19`–`RegR28`) and `d8–d15` (`RegD8`–`RegD15`), exactly as the
  ARM64 backend already marks them (RA_CALLEESAVE in
  `lib/Backend/arm64/RegList.h:39-48, 68-75`).** Wasm bodies that use
  these registers must save/restore them in the prolog/epilog. Never use
  `x16`/`x17` (linker scratch) or `x18` (TEB/platform) as general
  registers. Justification: matches the host ABI and the backend's
  existing allocation, so wasm and JS-JIT bodies compose without extra
  save/restore negotiation.
- `[decision]` **Frame markers for the walker (ARM64 wasm frames):**
  - JIT wasm frames establish a real frame pointer (`FP = SP` after
    prolog, `LR` saved at `[FP+8]`, §6) so the ARM64 walker can follow
    the `FP` chain. Each wasm frame must record (in a per-module side
    table, keyed by any return address in the body) its extent and arg
    layout so the walker can classify the frame (§9, second bullet).
  - Interpreter wasm frames are recognized by the existing
    interpreter-frame mechanism (return-address match against the
    `InterpreterStackFrame` chain, §1.7), **plus** the wasm-ness flag
    below.
  Justification: the ARM64 `Amd64StackFrame` analogue walks by `FP`
  (`frame+2` argv model, §1.1); without real `FP`s the walker cannot
  cross a wasm frame at all.
- `[decision]` **Walker exclusion rule (wasm = asm.js).** The walker
  must treat a native wasm/asm.js frame as a non-JS frame: when a native
  frame's function body has `GetIsAsmjsMode()` true, `CheckJavascriptFrame`
  returns false (no `CallInfo` decode), and the walker skips to the
  caller — exactly as x64 does (§1.7, `JavascriptStackWalker.cpp:1017-1020`
  + "call info is not valid for asm.js" at `:1022`). The same exclusion
  applies to interpreter wasm frames once the frame is identified as
  asm.js (the `InterpreterStackFrame` argv layout is the asm.js
  `AsmJsCallStackLayout`, not `JavascriptCallStackLayout`).
  Justification: decoding a wasm frame as a JS frame would read
  `CallInfo` garbage and corrupt `this`/`CallFlags` recovery.
- `[decision]` **GC stack-scan rooting rule.** Unboxed wasm values
  (`i32/i64/f32/f64` in registers or §6 locals / §7 result area) are
  NEVER traced as pointers by the collector. Only `Var` slots in
  `JavascriptCallStackLayout` / `InterpreterStackFrame` frames are roots.
  Wasm frames hold no `Var` roots (all JS references must be reachable
  from the external thunk's frame or explicitly rooted in the W6
  ownership record). Justification: D3 generic-buffer choice (no fast
  virtual buffer to trace) + the x64 layout split (`AsmJsCallStackLayout`
  vs `JavascriptCallStackLayout`, §1.1 — the wasm side carries no
  `Var` header). This is the plan's flagged GC risk (risk table, §7):
  a pointer-lookalike unboxed int must not retain an object.
- `[decision]` **Boundaries using `Var`/`CallInfo` (external) vs
  internal convention — summary:**
  - `Var`/`CallInfo` (`JavascriptCallStackLayout`): JS → wasm external
    entry, wasm → JS import calls (imports execute as JS), and every
    runtime-helper call that takes/returns `Var`.
  - Internal convention (AAPCS64, §3): wasm → wasm, wasm ↔ interpreter
    thunks, wasm → memory/table/global helpers that take unboxed args.
  Justification: restates §2 at the call-site level for W4's lowering
  work.
- `[open]` The side-table format and lookup for wasm frame extent /
  arg-layout (needed by the §9 walker marker decision). W3/W6 must
  define it; this doc fixes the requirement (skip wasm frames, never
  decode them as JS), not the encoding.

## 10. D6 classification (changes outside ARM64 files)

- `[decision]` **Arch-neutral / gated share.** The following shared
  files will need ARM64-or-gated edits when W3/W4 implement, and each
  must satisfy D6 (behavior-identical for x64 or behind a D2 gate):
  `lib/WasmReader/WasmSignature.cpp` (`GetParamSize` ARM64 arm,
  §1.3), `lib/Runtime/Language/AsmJsUtils.cpp` (ARM64 unbox/box),
  `lib/Runtime/Language/AsmJsTypes.cpp` (`mArgSizes` floor, §3),
  `lib/Runtime/Language/InterpreterStackFrame.cpp` (ARM64 entries),
  `lib/Runtime/Language/JavascriptStackWalker.cpp` (wasm skip, §9),
  `lib/Runtime/Language/AsmJsModule.cpp` (generic buffer, §1.9),
  `lib/Parser/rterrors.h` (no new strings expected; reuse existing IDs).
- `[decision]` **Untouched.** `WasmSignature` result storage
  (`m_results`), `WasmTypes::WasmType` enum, `JavascriptCallStackLayout`
  fields, and `CallInfo` layout are consumed as-is. The multi-value area
  (§7) is new logic keyed off existing `GetResultCount()`.
- `[open]` Whether the W0 `ENABLE_ASMJS` gate disjunct needs the WasmI64
  debug path re-gated (it is currently inside `#if
  ENABLE_DEBUG_CONFIG_OPTIONS` in `AsmJsUtils.cpp`, §1.3). W1 does not
  decide gate mechanics (that is W0/D2).

## 11. Open questions

1. `[open]` **Q1 (W0/W1 boundary): Where is the ARM64
   `AsmJsCallStackLayout` defined?** x64 uses the shared header struct
   (§1.1). ARM64 needs the same `{functionObject, args[]}` shape for the
   interpreter thunk, but the internal JIT convention (§3) does not carry
   it. Decide whether ARM64 reuses the shared struct or gets a B4-gated
   arch-specific definition. (P1 decision point.)
2. `[open]` **Q2 (W4): Internal-call lowering entry point.** Does
   `LowerCallArgs`/`HomeJsCallArgs` get reused, or is a new
   `LowerAsmJsCallArgs` needed (§3)? Affects `arm64/LowerMD.cpp` only,
   not this ABI.
3. `[open]` **Q3 (W4): `FMOV S ↔ W` vs `FCVT` encoding choice for f32
   bitcasts** (§4). Record the NaN-payload behavior once chosen.
4. `[open]` **Q4 (W3/W7): `AsmJsRetType` multi-value arm vs
   `WasmSignature`-keyed area path** (§7). This doc recommends the
   signature-keyed path; W3/W7 confirm before coding.
5. `[open]` **Q5 (W3/W6): Wasm frame side-table encoding** for the
   walker (§9). Requirement is fixed; encoding is open.
6. `[open]` **Q6 (W0): `WasmI64` debug-path re-gating** (§10). Gate
   mechanics are W0/D2, not W1.
7. `[open]` **Q7 (W4/W5): OSR / bailout for wasm tier.** Does the x64
   tier have wasm bailouts, and should ARM64 mirror them? Out of scope for
   the ABI; recorded so W5 does not assume an answer.







