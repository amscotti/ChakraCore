# ChakraCore Modernization -- Architecture and Readiness Review

> Date: 2026-11-16
> Branch / Commit: `modernize/baseline` at `c24266598` (`8f4e85d62..c24266598`
> integrated + Stage 3B docs & Test262 refresh)
> Scope: Synthesis of the Stage 1 architecture audit (`arch-audit`), Stage 2/3
> P0/P1 architectural and memory-safety fixes, current build/runtime/host
> layout, verified Mac subsystem test-coverage matrix, explicit residuals and
> API boundaries, and the Linux x64 bring-up readiness checklist.

---

## 1. Executive Summary

Following the Stage 1 read-only architectural review (`8f4e85d62`), Stage 2 and
Stage 3 landed nine commits (`9d8d1817a..c24266598`) closing the identified P0/P1
correctness, GC, JIT, concurrency, JsRT ABI, Node-API, build/CI, and StyleCheck
findings, followed by Stage 3B's certified Test262 baseline refresh and
documentation sync.

Key state on `modernize/baseline` (`c24266598` + Stage 3B `docs-cert`):

- **All 4 Mac ARM64 Zig build profiles pass** (`test` **2063/0**, `debug-static`
  **2115/0** + `8/8` static native suites, `release-shared` smoke/native/metadata,
  and `jit` + `-Dwasm=true` `check-jit` + **382/382 P1** WASM).
- **Node-API (`NAPI_VERSION=8`) passes** `tools/nodeapi/run_probe.sh` and
  **18/18** `test/nodeapi/probes/run_*_probe.sh` on both `test` and `jit`
  profiles (`BINARY_PROBE` `308/308` incl. `BigInt64Array`/`BigUint64Array`,
  `run_host_async_probe` `4/4`), `REPEATS=5` stress across both profiles,
  `run_matrix.py` **`46/23/0`**, and `run_node_addon_api_matrix.py --hosts both`
  **`52/61/6`** (`node-direct` `29/0`, `ch-nodeapi` `23/6` with all `6` diffs
  the documented PR-12 `test/object/*` V8 message checks).
- **Certified ES2025 Test262 baseline refreshed (`refresh-2026-11-16` on
  `zig-out/bin/ch`):** edition-selected **78,731 passed / 1,813 failed of
  80,544 (97.75%)**, down **-1,136** failures from `refresh-2026-11-05`
  (`77,595 / 2,949`, `96.34%`) with zero feature regressions; broad corpus
  **79,569 passed / 13,572 failed of 93,141 (85.43%)**. Primary drivers:
  `7412beb56` (`ESSharedArrayBuffer=true` default-on), B4 D1-D4 exact number
  formatting, A1-A12/JITREG, and Stage 2/3 runtime/JIT fixes.
- **StyleChecks vs `origin/master` (`97e68c917`) pass 5/5** using portable Python
  verification (`check_style_portable.py --self-test` + all four shell scripts).
- **Primary platform boundary:** Linux x64 Zig parameterization
  (`tools/build/linux.zig`), metadata checks (`tools/build/check_metadata_linux.py`),
  and `.github/workflows/zig-linux.yml` are locally verified via unit/format
  checks (`test_build_tools.py` `81/81`, `linux-gate-tests.zig` `7/7`), but
  post-Stage-2/3 native Linux x64 execution and hosted GitHub Actions runs remain
  pending (Section 7).

---

## 2. Current Repository, Build, and Host Architecture

### 2.1 Zig 0.17.0 Build Graph (`build.zig`, `tools/build/`)

The repository uses Zig `0.17.0` (`mise.toml`) as the primary build driver for
macOS ARM64 (`build.zig` + `tools/build/tables.zig`) and Linux x86_64
(`tools/build/linux.zig` + `tools/build/tables_linux.zig`):

- **macOS profiles (`build.zig`):**
  - `test` (default, RelWithDebInfo/Test, no-JIT shared `libChakraCore.dylib` +
    `ch` + `libChakraNodeApi.dylib` + `ch-nodeapi` + `GCStress` + `test-shared-basic`).
  - `debug-static` (Debug static `libChakraCoreStatic.a` + `ch` + `GCStress` +
    8 static native suites).
  - `release-shared` (Release shared `libChakraCore.dylib` + `ch` + `ch-nodeapi` +
    `test-shared-basic`).
  - `jit` (`-Dprofile=jit`, optionally `-Dwasm=true` for WABT + WebAssembly ARM64
    JIT support, `out/jit/bin/ch`, `GCStress`, `libChakraNodeApi.dylib`, `ch-nodeapi`).
- **Backend archive link order (`9d8d1817a`, `build.zig`):**
  Historically `build.zig` appended Backend object files at the tail of
  `static_objs`, diverging from `lib/CMakeLists.txt:21-28` (`${chakra_backend_objects}`
  between `Chakra.Jsrt.Core` and `Chakra.Common.Common`). Stage 2/3 aligned
  `build.zig` so Backend TUs sit between `Jsrt.Core` and `Common` before
  archiving via `/usr/bin/libtool -static`, and `test-shared-basic` + Phase-3
  native tests link directly against `libChakraCore.dylib` (without falling
  back to `libChakraCoreStatic.a`).
- **Linux x64 parameterization (`9d8d1817a`, `tools/build/linux.zig`,
  `tools/build/check_metadata_linux.py`):**
  Removed the hard-coded Ubuntu 24.04 + `test`-only gate assumption.
  `linux.zig` now accepts `-Dglibc` (default `2.39`), `-Dicu-major` / ICU
  include/lib overrides (default `74`), `-Dprofile` (`test`, `release-shared`,
  `debug-static`), `-Djit=true/false`, `-Dwasm=true/false`, and targets
  `libChakraCore.so` / `libChakraCoreStatic.a` / `ch` / `GCStress` /
  `libChakraNodeApi.so` / `ch-nodeapi` + `check-nodeapi` (`150` ELF exports).
- **CI Workflows (`.github/workflows/zig-macos.yml`, `zig-linux.yml`):**
  Expanded macOS workflow and added `zig-linux.yml` (`build` + `zig-variants` +
  `nodeapi` jobs), replacing macOS BSD `grep -P` false-greens with
  `tools/StyleChecks/check_style_portable.py`.
- **Legacy CMake (`CMakeLists.txt`, `lib/CMakeLists.txt`, `bin/CMakeLists.txt`):**
  Still present for legacy reference; does not build `lib/NodeApi` or
  `ch-nodeapi` and leaves `tools/RecyclerChecker/` CMake-only.

### 2.2 Runtime Shell Host (`bin/ch/`)

Stage 2 (`50570a36e`, `98c81ec88`) decomposed the ~3,200-line `bin/ch/WScriptJsrt.cpp`
monolith into cohesive headers while preserving the TU counts pinned in
`tools/build/test_incremental.py` (`CH_TU_EXPECT`):

- `bin/ch/WScriptJsrt.cpp` (~1.6k lines): host bootstrap, `WScript` entrypoints,
  and JsRT wiring.
- `bin/ch/WScriptJsrtState.h`: per-runtime / host synchronization state incl.
  `StderrLock`, `std::atomic` `sourceContext`, `s_moduleGraphFailureWitness`,
  `s_negativePhaseMarkerEmitted`, and thread-local `t_moduleParseDepth`.
- `bin/ch/WScriptModuleHost.h`: ES module registry, dynamic `import()`, import
  attributes resolution, and `StderrLock`-serialized negative-phase emission.
- `bin/ch/WScriptSerializer.h`: StructuredClone (`WScript.Serializer`) callbacks.
- `bin/ch/WScriptTest262Agents.h` & `bin/ch/RuntimeThreadData.{h,cpp}`: Test262
  `$262.agent` worker threads, `WScript.Broadcast` per-child `broadcastQ`, and
  safe teardown.

### 2.3 Node-API Provider & Embedding Host (`lib/NodeApi/`, `tools/nodeapi/`)

- **`lib/NodeApi/` -> `libChakraNodeApi.{dylib,so}`:** Implements Node-API v8
  (`NAPI_VERSION=8`: `115` `js_native_api.h` + `30` `node_api.h` + `5` host
  embedding APIs in `NodeApiHostApi.h` = `150` exported symbols). Built directly
  against public/internal JsRT headers (`lib/Jsrt/`).
- **`tools/nodeapi/ch-nodeapi.cpp`:** Standalone test/embedding host loading the
  provider (`RTLD_GLOBAL`) and `.node` addon (`RTLD_LOCAL`). Injects
  `chakra_nodeapi_host_services` with `host_manages_promise_queue = true`, a FIFO
  host task queue (`post_task`), a 4-worker thread pool (`post_work`,
  `cancel_work`, `ref_loop`, `unref_loop`), `--pump-timeout-ms`, and
  `--pump-limit`. `uv_loop` stays `NULL` by design (`napi_get_uv_event_loop`
  returns `napi_generic_failure`).

### 2.4 Evidence & StyleCheck Hygiene (`9d8d1817a`, `c24266598`)

- Root `evidence/gc-async-continuation/` (10 files) was relocated to
  `doc/evidence/gc-async-continuation/` (`9d8d1817a`), and all doc/LLDB path
  references (`doc/e2-gc-async-continuation.md`, `doc/evidence/gc-async-continuation/trace1.lldb`)
  were updated.
- `tools/StyleChecks/check_style_portable.py` provides portable Python checks
  called by `check_eol.sh`, `check_copyright.sh`, `check_tabs.sh`,
  `check_ascii.sh`, `check_file_eol.sh`, and `check_file_ascii.sh`, closing
  all 384+ pre-existing EOL/copyright/non-ASCII errors vs `origin/master`
  (`c24266598`).

---

## 3. Phase-3 JsRT Additive API Versioning Contract

As codified in [modernization-plan.md](modernization-plan.md) Section 0.1,
`lib/Common/ChakraCoreVersion.h` stays at pre-release dev `1.13.0`
(`CHAKRA_CORE_MAJOR_VERSION=1`, `MINOR=13`, `PATCH=0`, `RELEASE=0`). All
Phase-3 JsRT extensions are **strictly additive**:

1. **Additive enumerators and callbacks:**
   - `JsTypedArrayType` (`lib/Jsrt/ChakraCommon.h:533-541`): `JsArrayTypeFloat16`
     (`9`), `JsArrayTypeBigInt64` (`10`), and `JsArrayTypeBigUint64` (`11`)
     appended after `JsArrayTypeFloat64` (`8`), with compile-time `C_ASSERT`
     stride checks against `Js::TypeIds_*` (`lib/Jsrt/Jsrt.cpp:2231-2241`).
   - `JsSetTimerContinuationCallback` (`lib/Jsrt/ChakraCommon.h:2517`,
     `lib/Jsrt/JsrtCommonExports.inc:106`, `lib/Jsrt/Jsrt.cpp:3601`): opt-in
     per-context delayed continuation hook driving finite positive
     `Atomics.waitAsync` timeouts.
   - `FetchImportedModuleFromScriptWithAttributesCallBack` (`lib/Jsrt/ChakraCore.h:139,251`,
     `lib/Jsrt/Core/JsrtCore.cpp:188,247`), paired with
     `FetchImportedModuleWithAttributesCallBack` (`0x9`), registered via
     `JsModuleHostInfo_FetchImportedModuleFromScriptWithAttributesCallback` (`0xA`).
2. **Header parity & export gate:** `check-metadata` / `check_metadata_linux.py`
   enforce that the 4 staged `include/` headers are **byte-identical** to `lib/`
   sources and verify exported JsRT symbol sets on every profile build.
3. **Internal TypedArray safety:** `IsPublicTypedArrayTypeId` (`lib/Jsrt/Jsrt.cpp:2243-2247`)
   restricts `JsGetTypedArrayInfo` and `JsGetTypedArrayStorage` to
   `TypedArrayBase::Is(typeId) && typeId <= Js::TypeIds_Uint64Array`, returning
   `JsErrorInvalidArgument` on internal `TypeIds_CharArray` / `TypeIds_BoolArray`
   (never returning out-of-range ordinals or hitting Debug `AssertMsg` /
   `JsErrorFatal`), and returning element strides (`2`/`8`/`8`) for Float16/
   BigInt64/BigUint64.
4. **Backward compatibility & native tests:** Pre-Phase-3 hosts that ignore the
   new callbacks retain unchanged behavior on finite `Atomics.waitAsync` timeouts
   (remain pending) and attribute-free modules; `test/native-tests/jsrt_phase3_tests.h`
   exercises all four surfaces (`TestExternalArrayBufferLifetimes`,
   `TestTypedArrayAbi`, `TestTimerContinuationCallback`,
   `TestFetchImportedModuleFromScriptWithAttributes`) inside `check-native`. Any
   future breaking JsRT ABI change requires bumping `CHAKRA_CORE_MINOR_VERSION` or
   `MAJOR_VERSION`.

---

## 4. Stage 2/3 Architectural & Safety Fixes Ledger

Every P0 and P1 finding raised by `arch-audit` (`8f4e85d62`) was fixed and
verified on `modernize/baseline` (`c24266598`):

| Area / Finding (`arch-audit`) | Landing Commit(s) & Key Files | Fix & Ownership / Safety Invariant |
|---|---|---|
| `cdjs.js` `-nonative` concurrent-GC `SIGSEGV` (`MediumHeapBlock` 16-bit interior offset wrap) | `20ea96941` (`lib/Common/Memory/Recycler.inl`, `test/Bugs/cdjs-mediumblock-interior-gc.js`, `83ab4d28e` `tools/bench/run.py`) | On 16 KiB-page hosts (Apple Silicon), `MediumHeapBlock` spans 128 KiB (`> USHRT_MAX`). Shifted `(rawInteriorAddress - baseAddress)` by `HeapConstants::ObjectAllocationShift` in `uint` before narrowing and widened index*size math to `size_t`. Added `cdjs-mediumblock-interior-gc.js`; cleared `cdjs.js` `KNOWN_ISSUES` (`30/30` and `5x` green). |
| `Float16Array` desynced `Lower.cpp` `ObjectType` tables + `ProfilingHelpers` ceiling | `35152953b` (`lib/Backend/Lower.{h,cpp}`, `lib/Backend/JnHelperMethod.cpp`, `lib/Runtime/Language/ProfilingHelpers.cpp`, `test/es7/float16-jit-bounds.js`) | Inserted `Float16Array` in its exact ordinal slot across all 5 `ObjectType::Count` tables (`VtableAddresses`, `OffsetsOfHeadSegment`, `OffsetsOfLength`, `IndirTypes`, `IndirScales`), added `CompileAssert(_countof(...) == ObjectType::Count)`, added `VtableFloat16Array`, and replaced `<= TypeIds_Float64Array` with `TypedArrayBase::Is(instanceType)`. |
| Cross-runtime Growable SAB `bufferLength` race & stale cached `TypedArray` lengths | `35152953b` (`SharedArrayBuffer.{h,cpp}`, `TypedArray.{h,cpp}`, `AtomicsObject.cpp`, `WebAssemblyMemory.cpp`, `GlobOptArrays.cpp`, `Lower.cpp`, `test/es7/s6-growable-cross-agent.js`) | Serialized backing `ZeroMemory`/commit under `SharedContents::csGrow` with `std::atomic<uint32> bufferLength` (`memory_order_release` store on grow, `memory_order_acquire` / ARM64 `LDAR` on read). Added `TypedArrayBase::tracksSharedLength` to prevent `BailOnNotArray` length hoisting; switched `AtomicsObject::EntryWait`/`EntryWaitAsync` witness loads to `AtomicsOperations::Load`. |
| Non-writable `lastIndex` bypass in JIT/slow paths & `ES6RegExSymbols` empirical audit | `35152953b` (`Lower.cpp`, `RegexHelper.cpp`, `JavascriptRegularExpression.{h,cpp}`, `test/es7/regex-nonwritable-lastindex-jit.js`) | Guarded `GenerateFastInlineRegExpExec` (`lastIndexNonWritable != 0 -> labelFastHelper`), `RegexEs5ReplaceImpl`, `PropagateLastMatchToRegex`, `GetInitialOffset`, and `EntryCompile`; added `JavascriptRegExp::SetPropertyWithAttributes` enforcement. Documented empirical blockers keeping `ES6RegExSymbols` & `ES6RegExPrototypeProperties` `(false)` (Section 6). |
| `WScriptJsrt.cpp` monolith, unsynchronized/sticky module witness, & T3 broadcast overrun | `50570a36e`, `98c81ec88` (`bin/ch/WScriptJsrt*`, `bin/ch/RuntimeThreadData.{h,cpp}`) | Split `WScriptJsrt.cpp` cleanly; added `StderrLock` and atomic `sourceContext`, `s_moduleGraphFailureWitness`, `s_negativePhaseMarkerEmitted` + `t_moduleParseDepth` (preventing caught `import()` bleed); replaced broadcast timeout overwrite with per-child `broadcastQ` holding AddRef'd `JsSharedArrayBufferContentHandle`. |
| JsRT `Int64`/`Uint64`/`Float16` ABI crash & detached external AB finalizer leak | `50570a36e` (`lib/Jsrt/Jsrt.cpp`, `lib/Jsrt/JsrtExternalArrayBuffer.cpp`, `lib/Jsrt/ChakraCommon.h`, `test/native-tests/jsrt_phase3_tests.h`) | Explicit enum values + `C_ASSERT` checks + `IsPublicTypedArrayTypeId` (`JsErrorInvalidArgument` on `CharArray`/`BoolArray`); single-owner transfer across `Finalize`/`CreateDetachedState`/`DiscardState` firing once on GC or detach; added 4 native Phase-3 test suites. |
| Node-API OOM rollback, `A8-3` `PromiseQueue*`, `BigInt64` wiring, & `ch-nodeapi` async loop | `69df54b1d`, `29362397a`, `83ab4d28e` (`lib/NodeApi/*`, `tools/nodeapi/ch-nodeapi.cpp`, `test/nodeapi/*`) | Moved `g_live_envs` and state creation inside `try/catch (...)` with `new (std::nothrow)` and full rollback; ref-counted per-context `PromiseQueue*` reclamation + `host_manages_promise_queue`; wired `BigInt64Array`/`BigUint64Array` directly through JsRT; implemented `post_task`/`post_work` worker loop + `--pump-timeout-ms` + clean shutdown in `ch-nodeapi`; scrubbed hardcoded `/Users/ascotti` & `/tmp/opencode` paths. |
| Unwinder `_LIBCPP_VERSION` proxy & non-Apple ARM/ARM64 FDE registration | `35152953b` (`lib/Common/PlatformAgnostic/AssemblyCommon.h`, `lib/Common/Memory/arm/XDataAllocator.cpp`, `lib/Common/Memory/arm64/XDataAllocator.{h,cpp}`) | Separated `__APPLE__` Mach-O (`mac_fde_wrapper`) from `!__APPLE__ && __ELF__` `CHAKRA_UNWINDER_SINGLE_FDE` CIE-to-FDE walk; gated non-Apple ARM64 `.xdata` registration under `CHAKRA_ARM64_DWARF_EH_FRAME`. |
| Build, Backend link order, Linux parameterization, WASM manifest, & StyleChecks | `9d8d1817a`, `83ab4d28e`, `c24266598` (`build.zig`, `tools/build/*`, `.github/workflows/*`, `tools/StyleChecks/*`, `doc/evidence/gc-async-continuation/`) | Aligned Backend link order between `Jsrt.Core` and `Common`; parameterized `tools/build/linux.zig` + updated `zig-linux.yml`/`zig-macos.yml`; resynced `wasm-arm64-manifest.json` (`EXPECTED_P1_TOTAL=382`); relocated `evidence/gc-async-continuation/`; replaced BSD `grep -P` false-greens and closed all EOL/copyright/ASCII diffs (`5/5` green). |

---

## 5. Subsystem Test-Coverage Matrix

All checks below were observed passing after the code changes on committed
`HEAD` (`c24266598`), with Test262 refreshed in Stage 3B against the `c24266598`
`zig-out/bin/ch` binary and StyleChecks re-verified after all Stage 3B doc edits:

| Subsystem / Profile | Command / Gate | Result (`c24266598`) | Scope & Notes |
|---|---|---|---|
| **Mac `test` profile** (shared, no-JIT) | `zig build check libChakraNodeApi ch-nodeapi check-nodeapi` | **PASS** | `check_regression` **2063/0**; `check-native` (`test-shared-basic` incl. Phase-3 JsRT tests; 8 static ignored); `check-smoke`; `check-metadata` (minos 26.0, 8 MiB stacks, 4 headers identical, 8 JsRT exports); `verify-bytecode`; `check-nodeapi` (`150` symbols); `check-selftest` |
| **Mac `debug-static` profile** | `zig build -Dprofile=debug-static check` | **PASS** | `check_regression` **2115/0**; `check-native` **8/8** static suites PASS; `check-smoke`; `check-metadata` (minos 26.0, stacksize 0, `526` archive members); `verify-bytecode`; `check-selftest` |
| **Mac `release-shared` profile** | `zig build -Dprofile=release-shared check ch-nodeapi` | **PASS** | `check-smoke`; `check-native` (`test-shared-basic`); `check-metadata`; `check-selftest`; builds `ch-nodeapi` |
| **Mac `jit` + `wasm` profile** | `zig build -Dprofile=jit -Dwasm=true check check-wasm libChakraNodeApi ch-nodeapi check-nodeapi` | **PASS** | `check-jit` PASS; `check-wasm` **382/382 P1 PASS** (`runtests` `438` passed / `15` non-P1 failed of `483` manifest total); `verify-bytecode`; `check-smoke`; `check-nodeapi` (`150`) |
| **GCStress bounded smoke** | 6s runs on `./zig-out/bin/GCStress` & `./out/jit/bin/GCStress` (default + `-js -ForceBlockingConcurrentCollect`) | **PASS** | Reached gen `73`, `0` failures (bounded smoke, not an unbounded overnight soak) |
| **Node-API probes (`test` + `jit`)** | `SKIP_BUILD=1 tools/nodeapi/run_probe.sh` & all `18` `test/nodeapi/probes/run_*_probe.sh` | **ALL PASS (18/18)** | `BINARY_PROBE` `308/308` (incl. `BigInt64`/`BigUint64` + `Float16Array` error check); `run_host_async_probe` `4/4` |
| **Node-API stress (`test` + `jit`)** | `SKIP_BUILD=1 REPEATS=5 sh test/nodeapi/stress/run_stress.sh` | **ALL PASS** | 5 repeats each on both profiles: `gc_churn` (`78/78`), `env_churn` (`1208/1208`), `tsfn_teardown` (`252/252`), `gc_hook` |
| **Node-API JS conformance matrix** | `python3 test/nodeapi/run_matrix.py --verify-manifest && ... --suite all --host ch-nodeapi` | **PASS (`46/23/0`)** | `46` pass, `23` documented skip, `0` fail (`async_work_promise`, `async_work_thread_safe_function`, `test_typedarray` now pass) |
| **node-addon-api differential matrix** | `python3 test/nodeapi/run_node_addon_api_matrix.py --hosts both` | **52/61/6 (expected)** | `node-direct` `29/0`; `ch-nodeapi` `23/6` (`typedarray*.js` pass; remaining `6` are PR-12 `test/object/*` V8 error text diffs) |
| **`cdjs` benchmark (Mac ARM64 JIT)** | `tools/bench/run.py --binary out/jit/bin/ch --test cdjs --iterations 5` | **PASS (0 fail)** | `interp` `22.1` (MAD `0.0`), `simplejit` `12.4` (MAD `0.0`), `jit` `202.0` (MAD `2.0`); `KNOWN_ISSUES = []` |
| **Build tooling, incremental, & Zig tests** | `test_build_tools.py`, `zig test tools/linux-gate-tests.zig`, `zig test doc/examples/owned-bytes.zig`, `test_incremental.py --quick` | **PASS** | `81/81` `test_build_tools.py`; `7/7` `linux-gate-tests.zig`; `4/4` `owned-bytes.zig`; `0 FAIL` incremental |
| **Certified Test262 (`refresh-2026-11-16`)** | `python3 tools/test262/refresh_baseline.py --binary "$PWD/zig-out/bin/ch" --publish` | **97.75% edition** | Broad `79,569 / 13,572` of `93,141` (`85.43%`, `-1,170` vs `2026-11-05`); Edition `78,731 / 1,813` of `80,544` (`97.75%`, `-1,136` vs `77,595 / 2,949`, `0` regressions); `25/25` controls; `902` skips; `8,751` negatives |
| **StyleChecks vs `origin/master`** | `check_style_portable.py --self-test` + `check_{eol,copyright,tabs,ascii}.sh` | **PASS (5/5 exit 0)** | Portable across macOS and Linux against `97e68c917` |

---

## 6. Known Boundaries, Non-Conformances, and Explicit Residuals

The project explicitly avoids hiding limitations behind silent skips or
unverified claims. Known boundaries at `c24266598`:

1. **`node-addon-api` 6 V8 Error-Text Diffs (`test/object/*`):**
   `delete_property.js`, `get_property.js`, `has_own_property.js`,
   `has_property.js`, `object_freeze_seal.js`, and `set_property.js` fail on
   `ch-nodeapi` solely due to V8 vs Chakra `TypeError` message text differences
   (`An object was expected` vs `Cannot convert undefined or null to object`,
   and strict-mode read-only assignment message; PR-12 divergence documented in
   [node-api-receipt.md](node-api-receipt.md) Section 11.7). `node-direct` passes
   all `29/29`.
2. **15 Non-P1 WASM / AsmJs Tests (`runtests` `438/15` of `483`):**
   All `382/382` P1 tests in `test/wasm-arm64-manifest.json` pass on Mac ARM64
   `jit` + `-Dwasm=true`. The remaining `15` failing tests belong to deferred /
   P3 groups (e.g., `WasmSpec.MultiValue` `0/2`, `wasm.simd` `0/12`, plus partial
   non-P1 `AsmJs`/`wasm`/`WasmSpec` slices).
3. **Node-API `NAPI_VERSION=8` Scope Limits:**
   - Exported surface is pinned at `NAPI_VERSION=8` (`NodeApiEnv.cpp:552`, `150`
     symbols); v9/v10 APIs are intentionally omitted.
   - No global `Buffer` constructor (`typeof Buffer === 'undefined'`, PR-6).
   - `ch-nodeapi` sets `uv_loop = NULL`, so `napi_get_uv_event_loop` returns
     `napi_generic_failure`.
   - `JsArrayTypeFloat16` (`Float16Array`) returns `napi_generic_failure` from
     `napi_get_typedarray_info` because Node-API v8 defines no float16 enum
     value (`BigInt64Array`/`BigUint64Array` ARE directly wired).
   - `NodeApiValues.cpp` caps `kMaxBigIntWords = 16384`; global/prototype
     lookups in `NodeApiValues`/`NodeApiObjects`/`NodeApiBinary` rely on realm
     intrinsics rather than cached un-tamperable primordials.
4. **Two RegExp Flags Kept `(false)` Per Stage 2 Empirical Audit (`#40`):**
   While `ESSharedArrayBuffer` is now **default-on** (`7412beb56`), both
   `ES6RegExSymbols` (`ConfigFlagsList.h:697-699,1183`; `558` `disabled-flag`
   edition failures) and `ES6RegExPrototypeProperties`
   (`ConfigFlagsList.h:691-693,1174`; `154` `built-ins/RegExp` `rfm` failures)
   remain **`false`**. Empirical testing under Stage 2 proved two concrete
   blockers:
   - `Lower.cpp:20779` (`GenerateFastReplace`) and `Lower.cpp:20859`
     (`GenerateFastInlineStringSplitMatch`) fast-path `VtableJavascriptRegExp`
     without checking `DelegateToRegExSymbolFunction` (`re[Symbol.*]`).
   - Enabling `-ES6RegExSymbols -ES6RegExPrototypeProperties` fails `5`
     legacy interpreted and `5` dynapogo tests (`Regex/regexSplitOptimization.js`,
     `Regex/crossContext.js` x2, `es6/proto_basic.js`,
     `es6/RegExpApisTestWithStickyFlag.js` #16).
5. **`ESSharedArrayBuffer=true` Flag-On Test262 Tail (`94` edition failures):**
   Flipping SAB/threads ON cleared `1,098` `disabled-flag` failures vs
   `2026-11-05` (`1,813` total edition failures remain: `636` `disabled-flag`,
   `960` `engine-defect-on-existing`, `200` `required-feature-missing`, `17`
   `host`), leaving `94` active `SharedArrayBuffer`/`Atomics` failures (`78`
   classified in `disabled-flag` under frozen `edition-policy.json` plus `16`
   `host` `Atomics/{notify,wait,waitAsync}` as `10 SIGKILL` + `6 Test timed out`).
6. **Sanitizers, Full GCStress, Perf Gate, and Build/Tooling Leftovers:**
   - Mac Zig profiles do not yet offer ASan/TSan/UBSan or `FAULT_INJECTION`
     configs (`build.zig` omits sanitizer profiles and disables UBSan on
     `debug-static`).
   - Concurrent/full GC verification beyond the 6s bounded `GCStress` smoke,
     targeted `WeakRef`/`FinalizationRegistry` stress, and `cdjs` runs remains
     outside current automated CI.
   - One pre-existing load-only fulljit crasher (`open-cards.md`) stays open
     (the 109-test `LowerNewClassConstructor` SIGSEGV family was closed by e14).
   - No formal multi-benchmark perf regression gate beyond `cdjs` 5x checks.
   - `CMakeLists.txt` does not build `lib/NodeApi` or `tools/nodeapi/ch-nodeapi`;
     `tools/RecyclerChecker/` (10 files, CMake-only) stays orphaned from Zig/CI;
     `test/DebuggerCommon/AsyncSuspendedResume.js` stays `exclude_linux` pending
     Linux dynamic debugger attach.

---

## 7. Step-by-Step Linux x64 Bring-Up Readiness Checklist

Linux build parameterization and CI definitions (`tools/build/linux.zig`,
`tools/build/check_metadata_linux.py`, `.github/workflows/zig-linux.yml`) are
committed and locally validated (`81/81` `test_build_tools.py`, `7/7`
`tools/linux-gate-tests.zig`), awaiting native `x86_64-linux-gnu` execution
and hosted CI confirmation:

1. **Pre-flight on native `x86_64-linux-gnu`:**
   - Provision `zig 0.17.0` (`mise.toml`), `libicu-dev`, `binutils`, `clang`,
     `make`, `python3` (defaults: `-Dglibc=2.39 -Dicu-major=74`; override
     `-Dglibc`/`-Dicu-major` or explicit ICU paths if off Ubuntu 24.04).
   - Run StyleChecks (`python3 tools/StyleChecks/check_style_portable.py --self-test`
     + `check_{eol,copyright,tabs,ascii}.sh` vs `origin/master`),
     `python3 tools/build/check_metadata_linux.py --self-test`,
     `zig test tools/linux-gate-tests.zig` (`7/7`), and
     `python3 tools/build/test_build_tools.py` (`81/81`).
2. **Default `test` profile (`mise exec -- zig build && zig build check`):**
   - Confirm `95/95` PAL tests, `check-selftest`, `check-smoke`, `check-jit`,
     `check-native` (shared + Phase-3 JsRT tests), `check-metadata` (`237` `.so`
     exports / `669` archive members on baseline pin), and `verify-bytecode`.
   - Re-verify post-D1-D4 Linux runtime stability (`AsyncSuspendedResume.js` /
     `AsyncSuspendedDetach.js` `20-30x`, `test/wasm/deferred-invalid-body.js`,
     `WasmSpec/jsapi`, and A3 helper arg-order on x64).
   - Update `RECORDED_BASELINE` in `tools/build/check_regression_linux.py`
     from `2841/0` (interpreted) & `2305/0` (dynapogo) only after green native
     runs; diff `linux_exports_{so,ch}.txt` if comparing against CMake artifacts.
3. **Variant matrix (`.github/workflows/zig-linux.yml` `zig-variants`):**
   - `-Djit=false`: `check-smoke`, `check-metadata`, `verify-bytecode`, `check-native`.
   - `-Dprofile=release-shared`: `check-smoke`, `check-metadata`, `check-jit`, `check-native`.
   - `-Dprofile=debug-static -Doptimize=debug`: `check-smoke`, `check-metadata`,
     `verify-bytecode`, static `check-native`, and `runtests.py --debug --variants=interpreted test/wasm`.
4. **Node-API Linux stack (`zig build libChakraNodeApi ch-nodeapi check-nodeapi`):**
   - Verify `150` ELF dynamic symbols (`check-nodeapi`).
   - Run `SKIP_BUILD=1 tools/nodeapi/run_probe.sh` and all `18/18`
     `SKIP_BUILD=1 test/nodeapi/probes/run_*.sh`.
   - Run `SKIP_BUILD=1 REPEATS=5 sh test/nodeapi/stress/run_stress.sh`.
   - Run `python3 test/nodeapi/run_matrix.py --suite all --host ch-nodeapi` and
     `python3 test/nodeapi/run_node_addon_api_matrix.py --hosts ch-nodeapi`
     (confirming `0` unexpected failures outside the `6` `test/object/*` diffs).
5. **Certified Linux Test262 & Hosted GitHub Actions Confirmation:**
   - Run `python3 tools/test262/refresh_baseline.py --binary "$PWD/zig-out/bin/ch" --jobs 8`
     on the Linux proof host to supersede the pre-D1-D4 `77,354/3,190` Linux
     snapshot ([b4-linux-run.md](b4-linux-run.md)).
   - Trigger and observe first green hosted GitHub Actions runs on both
     `.github/workflows/zig-linux.yml` and `.github/workflows/zig-macos.yml`.
