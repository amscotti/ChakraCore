# WASM ARM64 — W0 x86-only inventory (W0 task 4)

Status: W0 slice (tasks 1+2+4). Base: `97ee0be80` (includes prerequisite lane
and amended `wasm-arm64-plan.md`). Host: darwin/arm64, `zig 0.17.0`.

Build option spelling (W0 task 2): `-Dwasm=true` (bool, default off), jit only.
`-Dwasm=true` with any `DISABLE_JIT` profile (`test`, `debug-static`,
`release-shared`) fails loudly at configure time:
`-Dwasm=true requires -Dprofile=jit (DISABLE_JIT profiles cannot enable
ENABLE_WASM/ENABLE_ASMJS on ARM64)`.
The option passes `-DWASM_ARM64_PORT=1` to every TU; option-off command lines
are byte-identical to today.

Gate matrix (D2, `lib/Common/CommonDefines.h`): `ENABLE_ASMJS` and
`ENABLE_WASM` are `defined(ASMJS_PLAT) || (defined(_M_ARM64) &&
defined(WASM_ARM64_PORT))`. On x86/x64 the second disjunct is always false
(D6: macro state byte-identical). `ENABLE_WASM_THREADS` /
`ENABLE_WASM_SIMD` / `ENABLE_WABT` stay `ASMJS_PLAT`-only (off on ARM64 until
W8/W9). `ASMJS_PLAT` itself is unchanged.

Categories: **port-P1** = W3 (+W2) interpreter; **port-P2** = W4/W5 JIT emission;
**gate-to-W8W9** = SIMD/threads; **fail-controlled** = deterministic fail-fast
today (`AssertOrFailFast`), never silent miscompile or segfault.

## Touched sites (re-gated to ENABLE_ASMJS or narrowed to ASMJS_PLAT)

| Site | Change |
|---|---|
| `lib/Common/CommonDefines.h:725-748` | New `ENABLE_ASMJS`; `ENABLE_WASM` widened to ARM64+PORT; SIMD/THREADS/WABT stay `ASMJS_PLAT` |
| `AsmJs.cpp:24`, `AsmJsByteCodeGenerator.cpp:8`, `AsmJsLink.cpp:8`, `AsmJsModule.cpp:8`, `AsmJsTypes.cpp:26`, `AsmJsUtils.cpp:25` + headers `AsmJs.h:25`, `AsmJsByteCodeGenerator.h:7`, `AsmJsLink.h:7`, `AsmJsModule.h:8`, `AsmJsTypes.h:27`, `AsmJsUtils.h:25` | Wholesale `ASMJS_PLAT` → `ENABLE_ASMJS` (arch-neutral parser/validator/codegen/module/link/types/utils) |
| `AsmJsCodeGenerator.{cpp:7,h:7}`, `AsmJsEncoder.{cpp:8,h:8}`, `AsmJsJitTemplate.h:8`, `amd64/AsmJsJitTemplate.cpp:7` | Stay `ASMJS_PLAT` (emission, port-P2) |
| `FunctionBody.h:60,555,1531,2454,3206` | fwd decls, `SetIsAsmJSFunction` (both classes), `ParseAsmJs`, FullJitScheduled + asm.js info API → `ENABLE_ASMJS` |
| `FunctionBody.h:540`, `2473` | TJ methods + `IsHotAsmJsLoop` stay `ASMJS_PLAT` (emission, port-P2) |
| `FunctionBody.cpp:2686,3755,6659,7267,7281,7320,9482` | `ParseAsmJs`, `HasValidEntryPoint` asm branch, allocators, `DoSimpleJit`/auto-profile exclusions, interface-entry asm branch → `ENABLE_ASMJS` |
| `Runtime.h:284` | `AsmJsExternalEntryPoint` decl → `ENABLE_ASMJS` (CodeGenerator/Encoder fwd decls stay `ASMJS_PLAT`) |
| `AsmJsUtils.cpp:421` | New `#else` ARM64 `AsmJsExternalEntryPoint` (`AssertOrFailFast`, fail-controlled; W1/W3 replaces with ABI entry) |
| `OpLayoutsAsmJs.h:8`, `OpCodeUtilAsmJs.{h:7,cpp:7}`, `LayoutTypesAsmJs.h:82`, `OpCodes.cpp:17`, `ByteCodeReader.{h:67,cpp:258}` | Opcode/layout/reader asm infra → `ENABLE_ASMJS` |
| `BackendOpCodeAttrAsmJs.{h:5,cpp:7}`, `AsmJsJITInfo.{h:10,cpp:10}` | Opcode attrs + JIT-info accessors (incl. `GetWasmSignature`) → `ENABLE_ASMJS` (arch-neutral data) |
| `Lower.cpp:3112` | `Copysign_A`/`Trunc_A`/`Nearest_A` narrowed to `ENABLE_WASM && ASMJS_PLAT`; `ThrowRuntimeError` stays `ENABLE_WASM` (x86-only lowering + SSE checks; ARM64 falls to default, port-P2) |
| `tools/build/tables.zig`, `build.zig` | New `wasmreader_group` (11) + `wasm_library_group` (7, incl. `WasmLibrary.cpp`, excl. `WabtInterface.cpp`); compiled only on jit+wasm, numbered after backend (629→647) |

## Remaining `ASMJS_PLAT` sites (all off on ARM64)

| Site | Reachability | Category |
|---|---|---|
| `InterpreterStackFrame.{h:96,361, cpp:1597,1619,1797,1810,2183,2209,2656,3326,3451,3848,5990,9105,9185}` — `AsmJsReturnStruct`, static/interpreter thunks, `AsmJsInterpreter`, `ProcessAsmJsModule`, `ProcessAsmJs` loop, `OP_CallAsmInternal`, `OP_Ld/StArr`, wasm-loop body start | Interpreter entry/execution for asm.js/wasm bytecode | **port-P1** (W3; thunks need W1 ABI) |
| `InterpreterProcessOpCodeAsmJs.h:6`, `InterpreterHandlerAsmJs.inl:6` | asm.js opcode handlers | **port-P1** (W3) |
| `Parser/Parse.cpp:7403` (lambda reject), `:15737` (`CheckAsmjsModeStrPid`) | `"use asm"` detection. Note `:15737` also gates on `SSE2Available()` — x86 assumption to fix | **port-P1** (W3 detection) |
| `ByteCode/ByteCodeEmitter.cpp:4033` (`AsmJSCompiler::Compile` dispatch) | Needs detection above; inert until Parse enables | **port-P1** (W3) |
| `ByteCode/ByteCodeSerializer.cpp:885,1603,1975,2157,2172,2307,3295,3847,4317,4337,4358` — rewrite/add/read asm.js info | Bytecode (de)serialization of asm.js/wasm functions (self-contained: defs + callers both gated) | **port-P1** (W3; arch-neutral, enable with emitter) |
| `ByteCode/AsmJsByteCodeDumper.{h:8,cpp:8}`, `AsmJsModule.cpp:93`, `InterpreterStackFrame.cpp:2608` (dump guards) | Debug dump only | **fail-controlled** (inert, debug aid) |
| `AsmJsByteCodeWriter.{h:15,cpp:10}`, `WAsmjsUtils.{h:8,cpp:8}` (`ASMJS_PLAT \|\| ENABLE_WASM`) | Already on via `ENABLE_WASM` | no work |
| `ScriptContext.cpp:546,719,1372` + `ScriptContext.h:787` (`asmJsInterpreterThunkEmitter` create/close/delete) | Dynamic asm interpreter thunks | **port-P1** (W3) |
| `ScriptContext.cpp:1395` + `ScriptContext.h:508` (`Init/GetAsmJsCodeGenerator`), `:31` includes | Template-JIT codegen | **port-P2** (W5) |
| `ScriptContext.cpp:3716,3726,4092,4229` + `ScriptContext.h:1620` (debugger transition, `asmJsEnvironmentMap`) | Debugger attach/detach for asm env | **gate-to-W6** (debugger deferred per D7) |
| `ScriptContext.cpp:5809,5836` (`Get/ReleaseDynamicAsmJsInterpreterThunk` else branches) | `#else __debugbreak()/Assert(UNREACHED)` if ever called on ARM64 | **fail-controlled** today; impl **port-P1** (W3) |
| `ThreadContext.{h:700,885, cpp:173}` (`DYNAMIC_INTERPRETER_THUNK \|\| ASMJS_PLAT` thunk allocators) | Already true on jit via `DYNAMIC_INTERPRETER_THUNK` | no work (verify in W3) |
| `CrossSite.cpp:438` (`DefaultThunk` asm.js entry) | Cross-realm asm.js calls | **port-P1** (W3) |
| `JavascriptFunction.{h:118, cpp:1196}` (`CallAsmJsFunction`, x86 thunk), `ScriptFunction.cpp:619` (`GetAsmJsArrayBuffer` fallback) | x86 call/mem helpers | **port-P2** (W4/W5) |
| `DynamicProfileInfo.{h:459, cpp:555}` (`RecordAsmJsCallSiteInfo`) | asm.js profiling | **port-P2** |
| `RuntimeLanguagePch.h:14` (emission includes) | `AsmJsEncoder/CodeGenerator/JitTemplate` | **port-P2** |
| `FunctionBody.{h:540,2473, cpp:3923,9194,9330,9361,9598}` (TJ methods, `IsHotAsmJsLoop`, DBG assert, TJ cleanup) | Template-JIT state | **port-P2** (except `:3923` DBG-only assert: **fail-controlled**) |
| `Backend/*` (emission lowering): `Lower.cpp:696,707,9635,9974,11189`, `IRBuilderAsmJs.{h:8,cpp:7}`, `Func.cpp:412`, `Inline.cpp:1517,4104`, `JITTimeFunctionBody.cpp:263`, `LowerMDShared.cpp:894`, `NativeCodeGenerator.{h:55, cpp:225,564,656,1555,1700,1889}`, `InterpreterThunkEmitter.cpp:306,505`, `SwitchIRBuilder.{h:44,cpp:63}`, `BackendApi.{h:80,cpp:120}` | IR/lowering/entrypoints for asm.js JIT | **port-P2** (W4/W5) |
| `CommonDefines.h` (6), `Runtime.h:287` (fwd decls), `BackendApi.h` decl | Gate definitions / emission decls | done / **port-P2** |

## `UNREACHED` / stub sites reachable on ARM64

| Site | Behavior today | Category |
|---|---|---|
| `AsmJsUtils.cpp` (new ARM64 `AsmJsExternalEntryPoint`) | `AssertOrFailFast(UNREACHED)` on any JS→wasm/asm.js call | **fail-controlled**; ABI entry **port-P1** (W1/W3) |
| `InterpreterStackFrame.cpp:1763` (`AsmJsDelayDynamicInterpreterThunk` non-AMD64 fallback) | `AssertOrFailFast(UNREACHED)` ("Asm.js only supported on x64 and x86") | **fail-controlled**; ARM64 thunk **port-P1** (W3) |
| `AsmJsModule.cpp:1841-1848` (`isOptimizedBuffer` else branch) | `Assert(UNREACHED)` + generic `false` (D3 generic path, minus assert) on array-view creation | **port-P1** (W3 task 2: drop assert, keep generic) |
| `Lower.cpp` default case (Copysign/Trunc/Nearest narrowed out) | Falls to switch default on ARM64 | **fail-controlled**; math lowering **port-P2** (W4) |
| `LowerMD.h:104-202` (17 ARM64 asm.js/wasm lowering stubs) | Pre-existing `Assert(UNREACHED)` stubs (plan §1) | **port-P2** (W4) |
| SIMD/threads opcodes (`ENABLE_WASM_SIMD`/`THREADS` off; e.g. `AsmJsEncoder.cpp:28,179`, `WasmSignature.cpp:150,185`, interpreter SIMD paths `:2233,2303,3098,3198,3271,3913,8480,8533,8592,8615`) | Not compiled/validated on ARM64 | **gate-to-W8W9** |
| `ScriptContext.cpp:5809,5836` else branches | `__debugbreak()` / `Assert(UNREACHED)` if dynamic asm thunk lookup reached | **fail-controlled**; **port-P1** |

## Unresolved (named remainder, not fully classified)

- `TARGET_64` / `_M_X64` / `_M_IX86` branch audit across `lib/WasmReader`,
  `lib/Runtime/Language/AsmJs*`, `WAsmjsUtils.*`, `lib/Runtime/Library/WebAssembly*`,
  `lib/Backend` (shared + arm64): not enumerated line-by-line in this slice.
  The wasm stack compiled and `WebAssembly.validate` ran on ARM64 (below),
  but latent arch branches may exist. **Remainder for W1/W2.**
- `ByteCode/ByteCodeEmitter.cpp:4033` + `Parser/Parse.cpp` need the `SSE2Available`
  arch fix before they can be enabled; left off deliberately (minimal W0).
- `tools/build/test_build_tools.py`: no table-shape assertion covers the new
  groups (`object_groups` unchanged), so no update was needed; suite passes
  (ran: 73 tests OK).

## Evidence (validation commands, exact results)

- `mise exec -- zig build -Dprofile=jit` (option OFF): exit 0; dylib has 0
  `WasmBinaryReader` symbols. `out/jit/bin/ch /tmp/wasm_probe.js` → `wasm:undefined`.
- `mise exec -- zig build -Dprofile=jit -Dwasm=true` (option ON): exit 0
  (compile + link); dylib has 426 `wasm` symbols incl. `WasmBinaryReader`.
  Probe → `wasm:object`; validate probe → `typeof:object`,
  `validate-empty:true`, exit 0 (no crash).
- `mise exec -- zig build -Dprofile=test -Dwasm=true`: exit 1, loud configure error
  (DISABLE_JIT fail-loudly, as designed).
- `mise exec -- zig build check` (test profile): exit 0,
  `Total: passed 2053, failed 0` (matches `gate_counts.json` test pin).
  Debug leg (`mise exec -- zig build -Dprofile=debug-static check`): exit 0,
  `Total: passed 2105, failed 0` (matches `gate_counts.json` debug pin).
- `python3 tools/build/test_build_tools.py`: 73 tests OK.
