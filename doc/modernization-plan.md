# ChakraCore Modernization Plan

> Status: DRAFT. Pinned to HEAD `97e68c917`. Scope: (1) switch the build to Zig
> 0.17 for the existing C/C++ project, (2) introduce new Zig code incrementally,
> (3) close major feature gaps vs V8 / QuickJS-ng / JavaScriptCore with priorities.
>
> Convention: any claim about Zig 0.17 API, behavior, or version-pinned dependency
> that was researched against Zig 0.16 locally plus 0.17 release notes is marked
> **[NEEDS VERIFICATION on Zig 0.17]** and must be re-checked after upgrading the
> local toolchain. Competitor-internals claims without a primary source are marked
> **[UNVERIFIED EXTERNAL]**. See §6.

## 0. Goals and non-goals

Goals:

- Keep ChakraCore a small, embeddable, community-maintained JS engine with a stable
  flat C (JsRT) API, time-travel debugging (TTD), and out-of-box WASM + Intl in one
  binary.
- Replace the CMake + `build.sh` / MSBuild dual-entry build with a single hermetic
  Zig build, without changing engine behavior first.
- Add Zig code only behind C-ABI boundaries, file by file, with no big-bang rewrite.
- Close the highest-cost JS/Intl/WASM gaps so modern test262-era code parses and
  runs, prioritized by breakage cost.

Non-goals:

- No DOM / Node runtime, no JS-to-C compiler parity with `qjsc`, no rewrite of the
  JIT/GC in Zig in this plan.
- No ABI break in `ChakraCore.h` / `ChakraCommon.h` / `ChakraDebug.h` during
  Phases 1–2.
- No commit to exact V8/JSC internal-architecture parity; competitor internals are
  anchors, not targets.

## 1. Current state (where we are)

Detail: [current-state.md](current-state.md), [architecture.md](architecture.md),
[build.md](build.md).

### 1.1 Version, identity, size

- Version **1.13.0 dev** (`RELEASE=0`) from `lib/Common/ChakraCoreVersion.h`; binary
  version composes from the same macros via `bin/CoreCommon.ver`.
- Community project for embedded use. Microsoft ended ChakraCore 1.11 security
  updates in March 2021; Edge no longer uses Chakra (`README.md:9-13`). No
  `ARCHIVED`/`DEPRECATED` file in repo; sustainer-scale activity — commits/year
  (author date) 1×2026 / 23×2025 / 25×2024 / 10×2023 / 36×2022 (recent drivers:
  VS/ARM64/ICU/clang/C++17 fixes).
- Working tree ~351M, 7,124 tracked files, single pack (~179 MB pack + index).
  Language mix dominated
  by C++ engine + JS test corpus: ~652k lines `.cpp`, ~272k lines `.h`, ~1.73M lines
  test JS, ~2.7k lines Python. Largest trees: `test/` 122M, `lib/Runtime` 19M,
  `lib/Backend` 9.4M, `lib/Common` 6.5M, `pal/src` 3.5M.
- No `src/` directory, no `package.json`/`VERSION` file at root. Only 12 tracked `.md`
  files; real docs are wiki links (Architecture, Embedding, JSRT Reference,
  Building, Resources) plus this new `doc/` folder.

### 1.2 Source layout and engine architecture

- There is no `src/`; engine source lives in `lib/` with 11 top-level units:
  `Backend` (JIT), `Common` (infra: Core, Memory/GC, DataStructures, Util, Codex,
  Exceptions, PlatformAgnostic), `JITClient` / `JITIDL` / `JITServer`
  (out-of-process JIT over RPC, Windows or opt-in), `Jsrt` (public C API +
  implementation), `Parser` (scanner/parser/AST + background parse), `Runtime`
  (engine: Base, ByteCode, Language, Library, Types, Math, Debug,
  PlatformAgnostic incl. ICU/Unicode shims), `SCACore` (structured clone),
  `WasmReader` (WASM binary frontend), `wabt` (vendored WABT).
- `pal/` is the Rotor-derived Platform Adaptation Layer for Linux/macOS. `bin/ch`
  is the `ch` developer host shell (minimal host: print-based I/O + WScript compat
  + debugger/TTD hooks — not print-only, but no DOM/Node); `bin/rl` is the test
  runner source (built on Windows; not in the xplat CMake); `bin/NativeTests` has
  C++ embedding tests (`JsRTApiTest`, `JsDiagApiTest`, etc.).
- Public API is a flat C API: `lib/Jsrt/ChakraCore.h` (ChakraCore-only: `JsParse` /
  `JsRun` / `JsSerialize`, module-record and serializer handles
  `JsVarSerializerHandle` / `JsVarDeserializerHandle`) + `ChakraCommon.h` (shared:
  `JsCreateRuntime` / `JsCreateContext` / `JsSetCurrentContext` / `JsCallFunction`,
  value/property APIs, `JsRuntimeAttributes`)
  + `ChakraDebug.h` (diagnostics events). Implementation in `Jsrt.cpp` /
  `JsrtRuntime.cpp` / `JsrtContext.cpp` plus `lib/Jsrt/Core/`; `JsrtRuntime` wraps
  `ThreadContext`, `JsrtContext` wraps `ScriptContext` + `JavascriptLibrary`.
  (No WASM surface in the JsRT headers — WASM is JS builtins under `ENABLE_WASM`;
  the host-side `JsCreateWeakReference` API lives in `ChakraCore.h`, not
  `ChakraCommon.h`.)
- Threading/hosting: one `ThreadContext` per thread (`GetContextForCurrentThread`),
  one `ScriptContext` per JS global/realm; host may supply
  `JsThreadServiceCallback` for background JIT/GC work or set
  `DisableBackgroundWork`; script interruption, idle processing, and
  no-JIT/no-eval/no-exec modes are runtime attributes.
- Pipeline: source → Scanner/Parser (`ParseNode` AST, upfront or deferred/background
  parse) → `ByteCodeGenerator` (register-based bytecode via `ByteCodeWriter`;
  serializable via `ByteCodeSerializer` / `JsSerialize`) → `InterpreterStackFrame`
  interpreter loop (shared `InterpreterLoop.inl` with debug/profile variants) →
  tiered JIT driven by `FunctionExecutionStateMachine` (owned by `FunctionBody`)
  through AutoProfiling/Profiling interpreter states to SimpleJit then FullJit,
  using `DynamicProfileInfo` + inline caches. `FunctionProxy` →
  `ParseableFunctionInfo` → `FunctionBody` with swappable `EntryPointInfo`
  `jsMethod` thunks (CheckCodeGen, interpreter thunks, native entry points,
  loop-body entry points).
- JIT: `lib/Backend` `NativeCodeGenerator` (background job manager) lowers bytecode
  to arch-specific IR (amd64/arm/arm64/i386); `SimpleLayout` for SimpleJIT; `GlobOpt`
  (value numbering, type specialization, PRE, array opts, bailouts) +
  Lower/LinearScan/Encoder/EmitBuffer for FullJIT. Supports in-proc and out-of-proc
  (`JITServer`) codegen. `DISABLE_JIT` CMake option exists (compile-out; runtime
  disable also exists as the `NoNative` ConfigFlag).
- GC: `Memory::Recycler` mark-sweep collector with concurrent/parallel mark,
  concurrent sweep, partial (generational-style) collection, finalization, weak refs,
  write barriers, page/bucket heap blocks. No sandbox/heap cage, no pointer
  compression.
- Intl: `ENABLE_INTL_OBJECT` + `HAS_ICU`; ICU C API wrapped by
  `lib/Runtime/PlatformAgnostic/ChakraICU.h`, `UnicodeText` abstraction with non-ICU
  fallback; `Intl.*` builtins partly in C++ (`IntlEngineInterfaceExtensionObject`)
  and partly JS (`InJavascript/Intl.js`). Only 4 Intl constructors exist (see §4).
- WASM: `ENABLE_WASM`; `WasmBinaryReader` decodes MVP binary (v1) into
  `WasmFunctionInfo`/parse tree; `WasmToAsmJs` reuses the asm.js typed pipeline for
  codegen; JS API via `WebAssemblyModule`/`Instance`/`Memory`/`Table`/`Environment`
  (`WasmI64` defaults false — i64 values surface per the flag description, not as
  JS BigInts); WABT vendored for tooling interop.
- Serialization/hosting extras: `SCACore` structured-clone Serializer/Deserializer;
  `Runtime.h` umbrella include pulls Parser + RuntimeCommon; TTD time-travel
  debugging.

### 1.3 Platforms, build, tests, CI

- Supported per README: x64 on Linux/macOS/Windows; x86 and ARM Windows-only;
  x86/ARM Linux + ARM macOS stated as future goal. CMake adds AMD64/X86/ARM/ARM64 ×
  Linux/macOS/Android select, macOS floor 10.9, C++17. ARM64-Linux warns unsupported
  and ARM64 JIT warns non-functional off-Windows.
- Unix entry: root `build.sh` (695 lines): usage/arg-parse, CMake invocation,
  make/ninja build, output `out/<Debug|Release|Test>`. Requires clang ≥3.7,
  `--cc`/`--cxx` overrides; LTO via `--lto`/`--lto-thin` downloads/builds LLVM 5.0.0
  + gold plugin via `tools/compile_clang.sh`. There is no `build.cmd` at root.
- Windows entry: `Build/Chakra.Core.sln` (VS15) via `test/ci.buildone.cmd` →
  `ci.build.init.cmd` → `test/ci.build.cmd` (`msbuild /m
  /p:Configuration=<Debug|Test|Release> /p:Platform=<x86|x64|arm>`). Auto toolset
  v110, v120, v140–v143, v145 (no v144); `stdcpp17`.
- Root `CMakeLists.txt`: `cmake_minimum_required 3.10`; options `DISABLE_JIT`,
  `INTL_ICU` (ON), `EMBED_ICU`, `USE_CCACHE`; `_SH`-suffixed vars from `build.sh`
  unset-from-cache after use; arch mapping x86_64→AMD64, armv7l→ARM,
  arm64/aarch64→ARM64; Android NDK abi armeabi-v7a API 21; fatal on unknown OS/arch.
- Deps: Python interpreter (`jstoc.py`/`regenByteCode.py`/`runtests.py`); no libuv.
  ICU: `--system-icu`/`--custom-icu`/`--embed-icu`/`--no-icu`; `EMBED_ICU` downloads
  `icu4c-63_2-src.tgz` with SHA512 and builds static; Linux default links `icuuc`+
  `icui18n`; macOS falls back to `--no-icu` if no path. Windows:
  `deps/Chakra.ICU/*.vcxproj` with `HAS_ICU`/`INTL_ICU`/`U_STATIC_IMPLEMENTATION`/
  `WINDOWS10_ICU` modes (`Build/Chakra.Build.props:42-62`).
- CMake structure: root adds `pal`, `lib`, `bin`, `test`. `lib/` builds
  `ChakraCoreStatic` from 20 unconditional plus up to 4 conditional object libs
  (`Backend` only if `BuildJIT`; `WasmReader` only AMD64; `wabt` only
  `CAN_BUILD_WABT` = non-Release AMD64; LTTng only if `USE_LTTNG`); post-build
  copies public headers to `<build-root>/include`. `bin/` builds `GCStress`
  (skipped on Android/Valgrind), `ch` host, `ChakraCore` shared lib unless
  `STATIC_LIBRARY`.
- Codegen: `bin/ch/jstoc.py` (JS → C const-char-array header, e.g. `DbgController.js`);
  `tools/regenByteCode.py` (embedded bytecode headers from `lib/Runtime/Library/
  InJavascript`, `--verify` in `bytecode-check`); `tools/lttng.py` (ETW `.man` →
  LTTng); `--trace` is static-only (the static-only FATAL gates
  `ENABLE_CC_XPLAT_TRACE`, not LTTng); `Build/CMakeFeatureDetect.cmake`
  (`disable_tail_calls` probe); RecyclerChecker clang plugin
  (`--wb-check`/`--wb-analyze`); `pal` configure probes. Suspected bug: root
  CMakeLists.txt:610-613 expands `${CLANG_SANITIZE_SH}` after unsetting it, so
  `--sanitize` flags may never apply — needs a configured-build probe.
- Tests: `test/` has 85 top entries, ~2,801 `.js` files (Array, AsmJs, es5/es6/es7,
  Intl, WasmSpec, wasm, Debugger, Generated, benchmarks, native-tests, …). Xplat
  `ninja check` = `bytecode-check` + `regression-check` (`python runtests.py
  --debug|--test … --binary=<build>/ch`; Release only smoke `ch
  test/Basics/hello.js`) + `native-check` unless NO_ICU/EMBED_ICU. `runtests.py`
  requires `--debug`/`--test`, default variants `interpreted,dynapogo`, 60s/180s
  timeouts, auto-excludes `exclude_xplat` + `require_simd` off-Windows and
  `exclude_mac` on macOS. Windows `runtests.cmd` drives `rl.exe` with arch+config
  args; `ci.testone.cmd` runs bytecode verify + `runtests.py` + `runnativetests.cmd`
  (failure codes 2/3/4).
- CI: `azure-pipelines.yml` (master/release + weekly cron; Style job
  `check_{copyright,ascii,eol,tabs}.sh`; CMake legs: ubuntu-24.04 ×
  Debug/NoJit/RelWithDebInfo/Release, ubuntu-22.04 × RelWithDebInfo/Release only,
  macOS-latest × DebugNoICU-static/RelWithDebInfo/Release — no macOS NoJit leg —
  via `cmake -GNinja … -DCMAKE_CXX_COMPILER=clang++ … && ninja && ninja check`;
  MSVC legs: windows-2025 x86/x64 × debug/test/release plus x86-only NoJit, and
  windows-2022 x86/x64 release-only build, tests only on debug/test/NoJit).
  `.cirrus.yml`:
  ARM64 macOS noJit Debug/RelWithDebInfo/Release. Tested-NoJit exists only on
  ubuntu-24.04 plus Cirrus ARM64-macOS. `.github/workflows/agreement.yml` is
  CLA-only.
- Stale floors / health risks (source-inspection only, no build was run): embedded
  ICU pinned to 63.2 (2018) while `build.sh` help still says 57.1; CMake 3.10 floor;
  `actions/checkout@v2`; README VS2015/2017 + Win7-era text vs windows-2025/2022 CI;
  macOS 10.9 floor; ventura image; travis/appveyor badges in vendored wabt README;
  retired `technet.microsoft.com` URLs; `Microsoft/ChakraCore` vs
  `chakra-core/ChakraCore` org-URL split.

## 2. Phase 1 — Switch the build to Zig 0.17 for the existing C/C++ project

Pattern: **Zig as a hermetic C/C++ toolchain first, language migration later**
(Uber pattern: `zig cc` compiles C/C++ behind a build flag; language adoption is a
separate step — **[UNVERIFIED EXTERNAL]** detail, re-source before citing). Phase 1
must reproduce the existing outputs with zero engine changes.

### 2.1 Pin and parallel-run

1. Pin Zig **0.17.0** in CI + docs (verified released 2026; release notes: 5 months,
   206 contributors, 925 commits). Keep CMake + `build.sh` / MSBuild working in
   parallel until `zig build` reaches full parity, then flip the default.
2. Add `zig --version` / `zig env` capture to every CI log. The §2.2 facts below
   were verified against Zig 0.17.0 itself (release notes + installed
   `lib/std/Build*.zig` sources, 2026-10-10); only the items still carrying
   **[NEEDS VERIFICATION on Zig 0.17]** remain open.
3. Pick one pilot target first (recommend: `ch` + `ChakraCoreStatic` for x64 Linux,
   `RelWithDebInfo`, system ICU), then expand the matrix.

### 2.2 Zig 0.17 facts that shape the build (all version-sensitive)

- Build system rework, verified in the 0.17.0 release notes: configurer/maker
  split, Build Server Protocol, cache switched to a binary format (~25% smaller),
  new ELF linker with incremental compilation for x86_64-linux
  **[NEEDS VERIFICATION on Zig 0.17 for incremental on macOS/Windows]**.
- **`@cImport` is REMOVED in 0.17** (deprecated in 0.16, both per the 0.17.0 release
  notes). C headers now go through the external ZSF `translate-c` package:
  `zig fetch --save git+https://codeberg.org/ziglang/translate-c`, then
  `const Translator = @import("translate_c").Translator;` +
  `Translator.init(dep, .{ .c_source_file = ..., .target, .optimize, ... })`
  (spelling verified against the release-notes upgrade guide). In-repo
  `Build.addTranslateC` still exists in 0.17 std (`Build.zig:949`) but
  `Step.TranslateC` is deprecated.
- Toolchain pins for 0.17, verified in the release notes: LLVM 22.1.8 (covers Clang,
  libc++, libc++abi, libunwind, libtsan), glibc 2.44, musl 1.2.5. Locally,
  `zig cc --version` reports clang 22.1.8.
- **ZLS has no 0.17 release** (latest tag is 0.16.0 as of 2026-10-10; verified on
  the ZLS releases page). Do not gate Phase 1 on IDE integration; re-check for a
  0.17-compatible ZLS before promising editor support.
- C/C++ pattern, spellings verified against installed 0.17.0 `lib/std` sources: one
  `build.zig` with `b.addExecutable` (`Build.zig:579`) / `b.addLibrary` (`:637`) +
  `b.createModule(options: Module.CreateOptions)` (`:722`),
  `Module.addCSourceFile(s)` (`Module.zig:391,415`) with
  `CSourceFile{ .file, .flags = &.{}, .language = null }` (`:118-122`, language
  auto-detected per extension so `.c`/`.cpp` sit side by side),
  `linkSystemLibrary` / `linkLibrary` (`:344,459`), `zig build-exe x.c -lc` for
  direct C builds, and `zig cc` / `zig c++` as drop-in cross-compilers.

### 2.3 build.zig work items (parity checklist)

- [ ] Top-level `build.zig` with options mirroring CMake: `DISABLE_JIT`, `INTL_ICU`,
  `EMBED_ICU`, `STATIC_LIBRARY`, `BUILD_TYPE` (Debug/Release/RelWithDebInfo/Test
  mapping), arch select (AMD64/X86/ARM/ARM64 + Android ARM), `--trace` static-only
  guard, `--lttng` support (no static requirement).
- [ ] `zig cc` pilot: compile the existing CMake file list with `zig cc`/`zig c++`
  before writing `build.zig` logic, to isolate toolchain vs build-logic failures.
  Micro-pilot already passed (C++17 TU with exceptions, RTTI, and virtual dispatch
  compiles and runs under 0.17.0 `zig c++`). Still needed: the **C++-at-scale
  pilot** for ChakraCore's full codebase (per-file `.language` override usage, lld
  vs self-hosted linker choice for big C++) before committing.
- [ ] `ChakraCoreStatic` object-lib equivalents (20 unconditional + up to 4
  conditional: `Backend` only if `BuildJIT`; `WasmReader` only AMD64; `wabt` only
  non-Release AMD64; LTTng only if `USE_LTTNG`), C++17 flags, arch/OS defines,
  `PAL` objects, version-header generation (`ChakraCoreVersion.h` →
  `<build-root>/include` + `ChakraCore.h`/`ChakraCommon.h`/`ChakraDebug.h` copy
  step).
- [ ] ICU modes: system / custom path / embed-63.2-static / no-ICU, with Linux
  `icuuc`+`icui18n` link, macOS `--no-icu` fallback, and Windows `Chakra.ICU` modes
  preserved for the MSVC path until Zig covers Windows.
- [ ] Codegen steps as Zig build steps: `jstoc.py` (`DbgController.js`),
  `regenByteCode.py` (+ `--verify` gate), `lttng.py` manifests, `disable_tail_calls`
  probe, RecyclerChecker plugin wiring, `pal` configure probes.
- [ ] Outputs: `ch` host, shared `ChakraCore` (`--whole-archive` + version script
  parity), `GCStress`, Windows-side `rl` runner; post-build `ch` copy to build root.
- [ ] Tests: `zig build check` must equal `ninja check` (bytecode-check +
  `runtests.py --debug|--test … --binary=<build>/ch` + native-check; Release smoke
  `ch test/Basics/hello.js`). Keep default variants `interpreted,dynapogo` and tag
  excludes.
- [ ] CI: mirror the Azure legs (ubuntu-24.04 × Debug/NoJit/RelWithDebInfo/Release;
  ubuntu-22.04 × RelWithDebInfo/Release; macOS-latest × DebugNoICU-static/
  RelWithDebInfo/Release; windows-2025 x86/x64 × debug/test/release plus x86-only
  NoJit; windows-2022 x86/x64 release-only) plus Cirrus ARM64-macOS noJit, with
  `zig build` and `zig build check` alongside the legacy commands during transition.

Acceptance for Phase 1: clean `zig build` + `zig build check` on the pilot triple,
then full-matrix parity with CMake/MSBuild, with no engine source changes except
build-required flag/define moves. Rollback is keeping the legacy entry points until
the flip.

## 3. Phase 2 — Introduce new Zig code incrementally

Pattern: **file-by-file behind C-ABI boundaries, no rewrite**. Start from
`translate-c` output for bindings, then hand-maintain the boundary. Link a Zig
static lib into the C/C++ exe or vice versa; expose Zig to C with `export fn`
(C ABI).

### 3.1 Boundary rules

- One C-ABI boundary per migrated unit; no C++ exceptions, no RTTI, no STL types
  across the boundary. Memory ownership and thread affinity (`ThreadContext` per
  thread, `ScriptContext` per realm) stay on the C++ side until a unit fully moves.
- Keep the flat JsRT ABI (`JsCreateRuntime`/`JsCreateContext`/`JsSetCurrentContext`/
  `JsCallFunction`/`JsParse`/`JsRun`/`JsSerialize`, module-record and serializer
  handles) byte-stable. New Zig code must not change header spellings.
- `translate-c` is scaffolding, not the final API: generate once via the ZSF package
  **[NEEDS VERIFICATION on Zig 0.17]**, then narrow to the minimal `extern` surface
  the Zig unit needs.
- Each migrated file keeps its existing tests green (`runtests.py` variants + native
  tests) before the next file starts.

### 3.2 Recommended migration order (leaf-first)

1. **New code first**: build helpers, test helpers, new tooling. Proves `build.zig` +
   `export fn` + static-lib linking with zero legacy risk.
2. **Leaf `Common/` utilities**: `Util`, `DataStructures`, `PlatformAgnostic`
   helpers with no JIT/GC coupling.
3. **`SCACore`** (structured clone Serializer/Deserializer over
   StreamWriter/StreamReader): small, well-bounded, handle-based API already
   (`JsVarSerializerHandle`/`JsVarDeserializerHandle`).
4. **`WasmReader` frontend** (`WasmBinaryReader` decode): parser-like, testable
   against `WasmSpec` + `wasm` suites without touching codegen.
5. **`Parser` helpers** (scanner tables, background-parse jobs): only after 1–4 are
   stable; keep `ParseNode` AST ownership in C++.
6. **Defer**: `Runtime` (ByteCode/Library/Types), `Backend`
   (NativeCodeGenerator/GlobOpt/Encoder), `Common/Memory` (Recycler), `Jsrt`
   runtime/context wrappers. These own threading, GC safety, and JIT entry-point
   thunks and move last, if ever.

### 3.3 Per-unit steps

- [ ] Define the C-ABI slice (functions, structs, error codes, ownership).
- [ ] Generate bindings with ZSF `translate-c`, trim to the slice, check in the
  trimmed `.zig`.
- [ ] Implement the unit in Zig behind `export fn`, link as a static lib into
  `ChakraCoreStatic` (or link C++ into a Zig exe for tools)
  **[NEEDS VERIFICATION on Zig 0.17 for link API spelling]**.
- [ ] Run `zig build check` + the focused `runtests.py` tags/variants + related
  native tests; compare `ch` behavior on `test/Basics/hello.js` smoke and the unit's
  suite.
- [ ] Delete the replaced `.cpp` only after CI-matrix green; keep the C header as
  the contract.

## 4. Phase 3 — Close major feature gaps vs V8 / QuickJS-ng / JavaScriptCore

Baseline: ChakraCore is effectively an **ES2015–ES2019 engine with cherry-picked
ES2020–ES2023 features**, not a current-spec engine.

**Target: full ES2025 (ES16) conformance as the Phase 3 exit**, verified by
test262; ES2026 features are stretch, tracked opportunistically after ES2025 is
green. (Edition contents below verified against the TC39 finished-proposals list,
2026-10-10. Current published edition is ES2026; ES2025 is chosen as the gate
because it is fully shipped by all majors and covers everything users mean by
"modern JavaScript", while the ES2026 delta is small and low-impact.)

| Edition | Have today | Missing → plan level |
|---|---|---|
| ES2020 | `??`, import(), import.meta, globalThis, allSettled, for-in order* | `?.`, matchAll, BigInt on → P0 |
| ES2021 | numeric seps, Promise.any | replaceAll, logical assign, WeakRef → P0/P1 |
| ES2022 | .at, hasOwn, cause, TLA | class fields, #private, static blocks, d flag → P0 |
| ES2023 | findLast, hashbang | toSorted family, WeakMap symbols → P0/P1 |
| ES2024 | none of 7 | groupBy, withResolvers, well-formed strings, v flag, resizable/transfer AB, waitAsync → P1 |
| ES2025 | none of 10 | escape, eval vars, Float16, try, iterators, JSON modules, import attrs, modifiers, Set methods, dup groups → P1 |
| ES2026 | none of 7 | Upsert, JSON.parse source, sequencing, base64, sumPrecise, isError, fromAsync → stretch |

\* `for-in` order formalization (ES2020 `for-in` mechanics): ChakraCore predates the
spec text but de-facto conformance is near-certain — confirm via test262 rather
than assuming.

Solid today: ES6 incl. modules/generators/proxy, async/await, async iteration,
dynamic import, `import.meta`, top-level await, nullish `??`, `flat`/`flatMap`/
`includes`, `fromEntries`, error `cause`, `globalThis`, `Promise.any`/`allSettled`/
`AggregateError`, `.at()`, `hasOwn`, `findLast`, numeric separators, hashbang, WASM
MVP + sign-ext/nontrapping, 4-constructor Intl (`Collator`/`DateTimeFormat`/
`NumberFormat`/`PluralRules`), JsRT C embedding API, 3-tier JIT
(interp/SimpleJit/FullJit + background JIT) on amd64/i386/arm/arm64.

Flag-gated OFF by default: `BigInt`, `SharedArrayBuffer`/`Atomics`, ES6 RegExp
symbols. WASM threads + multi-value OFF, SIMD experimental-flag-only.

Advantages to preserve: small embeddable C API, TTD time-travel debugging, out-of-box
WASM + Intl in one binary.

### P0 — Syntax/builtin blockers (modern code fails to parse/run)

- Optional chaining `?.`, logical assignment (`??=`/`&&=`/`||=`), class fields,
  `#private` methods (including ergonomic brand checks, `#x in obj`), static
  blocks. (No engine implementation or feature tests; remaining literal hits are
  benchmark-internal identifiers and C++-level name collisions.)
- `String.replaceAll`, `String.matchAll`, `queueMicrotask`, and the ES2023
  change-array-by-copy family (`toSorted`/`toReversed`/`toSpliced`/`with`, all
  0 hits). (Absent from `lib/Runtime`; for weak refs only the internal
  `RecyclerWeakReference` plus the host-side JsRT
  `JsCreateWeakReference`/`JsGetWeakReferenceValue` exist — no JS language
  builtins.)
- Turn `BigInt` ON by default (today all BigInt tests need `-ESBigInt`); decide
  `SharedArrayBuffer`/`Atomics` default (today needs `-ESSharedArrayBuffer`); turn
  ES6 RegExp symbols ON.
- Verify and close, then implement: RegExp `d` flag (ES2022; flag switch handles
  only i/g/m/s/u/y) and import attributes (ES2025, pulled forward into P0 as
  syntax-adjacent; 0 repo hits) — confirm each with a runtime probe via `ch`, then
  build. (RegExp `v` flag + modifiers + duplicate groups are ES2024/ES2025 scope
  in P1.)
- Why P0: syntax gaps break parsing of any modern bundle; builtin gaps break startup
  of common libraries.

### P1 — Ecosystem parity (Intl, modules-adjacent, WASM)

- `WeakRef` / `FinalizationRegistry` (needs Recycler finalization design; distinct
  from internal weak refs and the host JsRT weak-reference API); symbols as
  WeakMap keys (ES2023).
- ES2024 (all 7, all absent): `Object.groupBy`/`Map.groupBy`,
  `Promise.withResolvers`, `isWellFormed`/`toWellFormed`, RegExp `v` flag,
  resizable + transferable ArrayBuffers, `Atomics.waitAsync`.
- ES2025 (all 10, all absent): `RegExp.escape`, redeclarable global `eval` vars,
  Float16 (TypedArrays/DataView/`Math.f16round`), `Promise.try`, sync Iterator
  helpers, JSON modules, import attributes (see P0), RegExp modifiers, new Set
  methods, duplicate named capture groups.
- Explicit resource management is ES2027-stage (finished, publishes 2027) — track,
  don't commit. Decorators are still Stage 2.7 (in no published edition) — track,
  don't commit. Temporal and the ES2027 iterator extras likewise.
- Intl expansion, in order: `RelativeTimeFormat`, `ListFormat`, `DisplayNames`,
  `Locale`, `Segmenter`, `DurationFormat` (all 0 hits today). Upgrade embedded ICU
  from 63.2 (2018); fix macOS `--no-icu` fallback so Intl tests run on macOS.
- WASM stabilization: threads, multi-value (a `test/WasmSpec.MultiValue/` suite
  already exists, needs `-WasmMultiValue`), SIMD (all flagged off today; SIMD tests
  all need `-wasmsimd`). Then track GC, tail calls, exceptions, stringref,
  extended-const (0 opcode hits today; `WasmSpec/features` tracks only
  extends/nontrapping/threads). QuickJS-ng WASM story unverified — do not claim
  parity without sourcing.
- Why P1: unblocks i18n-heavy apps and WASM workloads; ICU upgrade is also a
  security/staleness fix.

### P2 — Performance, safety, architecture breadth

- JIT tiers: ChakraCore 3-tier (interp/SimpleJit/FullJit + background JIT) vs V8
  multi-tier pipeline (exact current V8 tier list **[UNVERIFIED EXTERNAL]**). No
  tier-addition proposed here; focus on ARM64 JIT off-Windows (warned
  non-functional) and keeping `DISABLE_JIT` viable for embedded.
- Memory safety: no sandbox/heap cage (cf. V8 sandbox **[UNVERIFIED EXTERNAL]**),
  no pointer compression; `Recycler` mark-sweep does have concurrent/parallel mark +
  concurrent sweep + partial GC — the verified V8 delta is sandboxing/compression,
  not concurrent collection. Treat sandboxing/compression as research items, not
  Phase 3 commitments.
- Platform breadth: x86/ARM Linux + ARM macOS (README future goal); ARM64-Linux
  unsupported warning; Windows-centric PAL heritage; x86/ARM Windows-only. Expand
  only after the Zig build covers cross-compilation.
- JSC specifics (tiers, interpreter, spec coverage) unverified — keep JSC gap claims
  generic.

### P3 — Tooling and release health (sustainability)

- `ch` is a minimal host with no DOM/Node APIs — intentional; improve host ergonomics
  and embedding samples instead of building a runtime.
- Ship a conformance dashboard: publish `runtests.py` + test262 pass rate per build
  (pass rate unverified today), plus `rlexe.xml`-gated feature matrix, so P0/P1
  progress is visible. No shipped dashboard exists today.
- Release cadence: community-maintenance only since 2021 vs QuickJS-ng
  **[UNVERIFIED EXTERNAL]** (small current-spec engine, frequent releases, `qjs`
  CLI + `qjsc` JS-to-C compiler — whole characterization unsourced until primary
  sources are fetched). Adopt a dated,
  hermetic `zig build` release artifact (headers + static/shared lib + `ch`) before
  promising cadence.
- Refresh stale floors separately from features: CMake 3.10, `checkout@v2`,
  VS2015/2017 + Win7-era README text, macOS 10.9, ventura image, ICU help-text
  mismatch (57.1 vs 63.2), retired technet URLs, org-URL split, travis/appveyor
  badges.

## 5. Risks and mitigations

| Risk | Mitigation |
|---|---|
| Zig 0.17 API drift (build-system + translate-c + Module APIs) | Pin 0.17.0; re-verify every **[NEEDS VERIFICATION on Zig 0.17]** item in §6 before flipping CI default; keep legacy build until parity. |
| C++17-at-scale link/codegen surprises (exceptions, RTTI, LTO/gold, big-link) | `zig cc` pilot + one-target `build.zig` pilot first; choose linker explicitly per triple. |
| ZLS/IDE friction slows adoption | Do not gate on IDE; revisit after 0.17 tooling status is verified. |
| JsRT ABI drift during Zig migration | C-ABI boundary rule + header-stability gate; native-API tests per unit. |
| WASM/Intl scope creep | P0 before P1 before P2; ICU upgrade and WASM stabilization are separate milestones. |
| Unverified competitor claims | Keep V8-exact, JSC-specific, and QuickJS-delta claims generic until primary sources are fetched. |

## 6. Verification checklist (must be green before calling each phase done)

- [ ] `zig --version` = 0.17.0 in CI logs; remaining **[NEEDS VERIFICATION on
  Zig 0.17]** items closed (incremental compilation behavior on macOS/Windows,
  full C++-at-scale pilot incl. linker choice, ZLS 0.17-compatible release).
- [ ] Phase 1: `zig build` + `zig build check` parity with `ninja check` /
  `ci.testone.cmd` on every current CI leg (ubuntu-24.04 ×
  Debug/NoJit/RelWithDebInfo/Release; ubuntu-22.04 × RelWithDebInfo/Release;
  macOS-latest × DebugNoICU-static/RelWithDebInfo/Release; Cirrus ARM64-macOS
  noJit × 3; windows-2025 x86/x64 × debug/test/release plus x86-only NoJit;
  windows-2022 x86/x64 release-only).
- [ ] Phase 2: per-unit `runtests.py` focused tags + native tests + `ch` smoke green;
  public headers byte-identical.
- [ ] Phase 3: full ES2025 green on test262 (Phase 3 exit gate); P0/P1 lists each
  individually test-verified (not grep-only); test262 pass rate published; ICU
  upgrade + `--no-icu` macOS behavior demonstrated; WASM flag matrix
  (threads/multi-value/SIMD) demonstrated.
- [ ] Still open from research (not gating Phase 1, tracked here): pal configure
  bodies, `ci.build.init.cmd` / `ci.parsetestargs.cmd` mapping, android/packaging
  script bodies, clang upper-bound + Windows SDK pin, external link liveness, GitHub
  archival flag, upstream tag list, actual build/test run on the legacy system,
  RegExp `d`/`v` + import-attributes runtime probes.

## Appendix A — In-repo evidence map

Key paths behind this plan (all verified at HEAD `97e68c917`): version
`lib/Common/ChakraCoreVersion.h`; feature flags `lib/Common/ConfigFlagsList.h`;
public API `lib/Jsrt/ChakraCore.h`, `ChakraCommon.h`, `ChakraDebug.h`; pipeline
`lib/Parser/`, `lib/Runtime/ByteCode/`, `lib/Runtime/Base/FunctionBody.h`,
`lib/Backend/`; GC `lib/Common/Memory/Recycler.h`; Intl
`lib/Runtime/PlatformAgnostic/ChakraICU.h`, `lib/Runtime/Library/InJavascript/
Intl.js`; WASM `lib/WasmReader/`, `lib/wabt/`; JS builtins
`lib/Runtime/Library/InJavascript/Array_prototype.js` (flat :310, flatMap :327,
includes :453), `InJavascript/Object_constructor.js` (fromEntries); error `cause`
`lib/Runtime/Library/JavascriptError.cpp:68-71`; Unix build `build.sh`, root
`CMakeLists.txt`, `lib/CMakeLists.txt`, `bin/*/CMakeLists.txt`; Windows build
`Build/Chakra.Core.sln`, `test/ci.buildone.cmd`, `test/ci.build.init.cmd`,
`test/ci.build.cmd`; tests `test/runtests.py`, `test/CMakeLists.txt`,
`test/ci.testone.cmd`; CI `azure-pipelines.yml`, `.cirrus.yml`.

## Appendix B — Open verification items (carried from research)

V8 exact feature set; JSC specifics; QuickJS-ng ES2024/ES2025 delta + WASM story;
full-scale C++ gotchas (linker choice at ChakraCore scale); macOS/Windows
incremental-compilation behavior; test262 pass rate; RegExp flags; import attributes;
pal/CI-init/packaging bodies; link liveness; archival flag; tag ambiguity. (Resolved
since the first draft: `flat`/`flatMap`/`includes` and error `cause` are present; GC
concurrency confirmed; `--trace` vs LTTng attribution fixed; `rl` is Windows-side;
20+4-conditional object libs; `ch` is a minimal host, not print-only. Resolved
2026-10-10: legacy build/test executed green — 1959 passed / 0 failed; all Zig 0.17
API spellings verified against 0.17.0 sources + release notes; ZLS confirmed to have
no 0.17 release yet.)
