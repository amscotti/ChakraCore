# Current State of ChakraCore

Engine-source baseline `97e68c917` (19 Feb 2026; see §7), with modernization
execution/decisions updated through 2026-10-27. Version **1.13.0 dev**
(`lib/Common/ChakraCoreVersion.h:19-21`, `CHAKRA_CORE_VERSION_RELEASE 0` at :58).
MIT license (`LICENSE.txt`). Every claim below cites an in-repo path; line numbers
were verified against this HEAD.

## 1. What ChakraCore is

Microsoft's former Edge JavaScript engine: standalone, embeddable, open-source,
with a flat C API (JsRT), a `ch` developer-host shell, and JS + WASM + Intl in one
binary (`README.md`, 82 lines). Edge no longer uses Chakra; Microsoft security
updates for 1.11 ended 9 Mar 2021; since then it is a community project aimed at
embedded use (`README.md:9-19`, plan links + issue #6384). Security reporting is
split 1.11-vs-master (:21-25). There is no `ARCHIVED`/`DEPRECATED` file in the repo
(the GitHub archival flag itself was not checked). There is no DOM/Node runtime —
`ch` (`bin/ch/ch.cpp`) is a minimal host with print-based I/O plus harness helpers
(WScript compat, debugger/TTD hooks), not a runtime.

## 2. Repo inventory

Size: ~351 MB tree; 122,322 objects in one pack (~179 MB pack + index on disk);
7,124 tracked files.
Lines: test JS ~1.73M; `.cpp` ~652.5K; `.h` ~271.7K; `.c` ~2.4K. Largest dirs:
`lib/Runtime` 19M, `lib/Backend` 9.4M, `lib/Common` 6.5M, `test/` 122M, `pal/src`
3.5M. Extensions: 2817 js, 1882 baseline, 974 h, 705 cpp, 125 wast, 77 xml, 65 cc,
50 inl, 43 txt, 42 wasm, 38 vcxproj, 32 c, 30 S, 28 hpp, 24 asm, 20 cmd, 12 md,
10 py (plus smaller counts).

There is no `src/` dir. Root layout: `lib/` engine; `pal/` Rotor-derived Platform
Adaptation Layer for non-Windows (`pal/inc/pal.h:14-18`); `bin/` = `ch` host, `rl`
runner source, `NativeTests` (`JsRTApiTest`/`JsDiagApiTest`), `GCStress`, `External/`
(vendored catch.hpp), ChakraCore shared-lib packaging; `test/` 85 entries, 2801 `.js` (262, Array, AsmJs, es5/6/7,
Intl, WasmSpec, wasm, benchmarks, native-tests, UnitTestFramework, Debugger,
Generated); `deps/` only Chakra.ICU projects; `tools/` has `regenByteCode.py`,
`lttng.py`, `compile_clang.sh`, RecyclerChecker, StyleChecks, XPlatInstall, icu and
android/packaging scripts. Root files: README, LICENSE.txt, CONTRIBUTING.md
(78 lines), ContributionAgreement.md (13 signers :36-48), CMakeLists.txt, build.sh
(695 lines; no `build.cmd` at root), azure-pipelines.yml, .cirrus.yml,
THIRD-PARTY-NOTICES.txt (34 items :20-53), plus `.github/`, `.editorconfig`,
`manifests/`, `Build/` (selective list). No package.json/nuspec/VERSION at root
(`test/WasmSpec/convert-test-suite/package.json` and five
`Build/NuGet/**/*.nuspec` files exist below root).

`lib/` has 11 units: Backend (JIT), Common (Core, Memory/GC, DataStructures, Util,
Codex, Exceptions, PlatformAgnostic), JITClient/JITIDL/JITServer (out-of-process JIT
over RPC), Jsrt (C API), Parser, Runtime (Base, ByteCode, Language, Library, Types,
Math, Debug, PlatformAgnostic), SCACore (structured clone), WasmReader, wabt
(vendored + `chakra/wabtapi` adapter).

Docs: no `docs/` dir; 12 tracked `.md` files; the real docs live on the wiki
(`README.md:27-33` Architecture/Embedding/JsRT-Reference/Resources, :46 Building,
:48 binaries; `CONTRIBUTING.md:36`, `.editorconfig:3` coding-conventions wiki).
This `doc/` folder is the start of durable in-repo documentation. The
feature inventory and line anchors below are the 2026-10-10 baseline
snapshot; the modernization branch has since added the C2 surfaces listed
in [modernization-plan.md](modernization-plan.md) §6 and its receipts.
Latest merged checkpoint (2026-10-27): **2026/0 test**, **2078/0
debug-static**, including direct-eval `super` early errors, the flag-on
S6a growable-SAB/non-shared-Atomics slice, S6b `Atomics.pause` /
single-thread `Atomics.waitAsync`, S6c BigInt Atomics, the S14
hidden-outer eval-`arguments` fix, the bounded S8 immutable-ArrayBuffer
surface, both S3 RegExp slices (`u` property escapes and `v` Unicode Sets
mode), and S9 u-mode RegExp strictness.

## 3. Architecture (summary)

Full detail with a diagram is in [architecture.md](architecture.md).

API: flat C API, `lib/Jsrt/ChakraCore.h:16` including ChakraCommon.h + ChakraDebug.h
(:29-30). Notable entries: `JsCreateRuntime` (ChakraCommon.h:781-785),
`JsCreateContext` (:1012-1015), `JsSetCurrentContext` (:1037-1039), `JsCallFunction`
(:2142), `JsThreadServiceCallback` (:714-725), `JsRuntimeAttributes` (:434-486:
DisableBackgroundWork, AllowScriptInterrupt, DisableNativeCodeGeneration,
DisableEval, …). ChakraCore.h: `JsParse` (:629-635), `JsRun` (:664-670),
`JsSerialize` (:779-783), `JsModuleRecord` (:41, :90-234), `JsVarSerializerHandle` /
`JsVarDeserializerHandle` (:52-66). Implemented in Jsrt.cpp/JsrtRuntime.cpp/
JsrtContext.cpp plus `lib/Jsrt/Core/`; `JsrtRuntime` wraps a `ThreadContext` (`JsrtRuntime.h:20-31`);
`JsrtContext` is a finalizable object holding the `JavascriptLibrary` plus a
back-pointer to its runtime (`JsrtContext.h:9-44`).

Threading: each JsRT runtime allocates and owns a `ThreadContext`
(`lib/Jsrt/Jsrt.cpp:315,374-375`), which owns its Recycler
(`lib/Runtime/Base/ThreadContext.h:1111`); one `ScriptContext` per realm.
`GetContextForCurrentThread` (:1109) is a current TLS lookup. Rental-threading
allows serial rebinding of inactive, non-thread-bound runtimes, not concurrent
script execution on the same runtime
(`lib/Runtime/Base/ThreadContextTlsEntry.cpp:70-125,140-162`). The host supplies a
`JsThreadServiceCallback` or sets DisableBackgroundWork.

Pipeline: `Parser.h` aggregates Scan.h/Parse.h/ptree.h/BackgroundParser.h/
BGParseManager.h (:41-56); Parse.h holds precedence + Upfront/Deferred parse types
(:16-43); `BackgroundParseItem` is a `JsUtil::Job` (:10-21). Bytecode:
`OpCodeList.h` `DEF_OP` multi-include list (:6-17); `ByteCodeGenerator` walks
ParseNode with Scope/FuncInfo via `ByteCodeWriter` (:47-62); `ByteCodeSerializer`
packing (:28-49). Interpreter: `InterpreterStackFrame` (:40-46, stack-allocated
except generator frames which are Recycler-allocated); `InterpreterLoop.inl` single
loop definition plus debug/profile copies (:5-38). Tiers: `ExecutionModes.h`
Interpreter/AutoProfilingInterpreter/ProfilingInterpreter/SimpleJit/FullJit (:9-27);
`FunctionExecutionStateMachine` linearizes 7 states with per-state limits (:58-80),
fed by `DynamicProfileInfo` + inline caches. Function model:
FunctionProxy → ParseableFunctionInfo → FunctionBody (:867-871, :1471, :1907);
`EntryPointInfo` carries jsMethod/native-data/work-item/state (:200-204, :312-318)
with Get/SetEntryPoint, SimpleJit/default and loop-body entries (:2271-2276,
:2464-2468, :2707-2708).

JIT: `NativeCodeGenerator` extends `WaitableJobManager`, with CheckCodeGen thunks
and the SimpleJit → FullJit transition (:18-67); `CodeGenWorkItem` is the Job
carrying jitData/jitMode (:9-20, :44-54); `SimpleLayout` (:7-12); `GlobOpt::Optimize`
with value-numbering/type-spec/PRE/bailouts (:417-424, :520-539) then
Lower/LinearScan/Encoder/EmitBuffer; backends amd64/i386/arm/arm64. JIT is compiled
out with the `DISABLE_JIT` CMake option (root `CMakeLists.txt:12` → `BuildJIT`
:518-527) — only the compile-out is CMake-exclusive; runtime disable exists both
as the `NoNative` ConfigFlag (`lib/Common/ConfigFlagsList.h:1415`) and via
`JsRuntimeAttributes`. Out-of-process JIT goes over RPC (`JITClient/JITManager.h`,
Windows or `ENABLE_OOP_NATIVE_CODEGEN` only) plus JITServer/JITIDL.

GC: `Memory::Recycler` mark-sweep collector (:645) with concurrent/parallel mark and
concurrent sweep (`ENABLE_CONCURRENT_GC` default 1, `lib/Common/CommonDefines.h:174`;
`RecyclerParallelThread` :586-628; `ProcessMark`/`ProcessParallelMark` :1658-1661;
`Sweep` :1697-1705), partial collection, weak refs, write barriers
(`WriteBarrierMacros.h`); blocks/buckets under `Common/Memory`. No heap
cage/sandbox, no pointer compression (zero hits across lib/).

JS level: substantial ES2015–ES2019 coverage with inherited gaps, plus
cherry-picked ES2020–ES2023 features; this is not full edition conformance. ES6 master
flag ON with sub-flags (`lib/Common/ConfigFlagsList.h:625`, :631-667, :1118; ES6Module
:640, :1144). ES2016/17: async/await, exponent operator, trailing commas,
values/entries, padding (:698-701; `test/es7/rlexe.xml:36-52` covers the
stringpad/asyncawait-apis/valuesAndEntries tests). ES2018: async
iteration/for-await, rest-spread, dotAll (:650, :668, :706, :1153, :1163, :1165;
rlexe :109-126 covers the async-iteration tests). ES2019: `fromEntries`
(`InJavascript/Object_constructor.js:16-41`), `Symbol.description` (:672),
trimStart/End, JSON superset (JavascriptLibrary.cpp:4487-4489; rlexe :127-138, incl.
`wellformedJSON.js`/:129 and `json_superset.js`/:135; note `symboldescription.js`
sits at `test/es7/rlexe.xml:99`). ES2020: `??` ON (knopCoalesce ptlist.h:91; :675, :1200;
nullish.js; rlexe :145-150), globalThis/dynamic-import/import.meta/TLA (:676-677,
:703-707; JavascriptLibrary.cpp:1456-1458; ImportMeta.js). ES2021: Promise.any +
AggregateError behind ESPromiseAny ON, allSettled unconditional (:674, :1211;
JavascriptLibrary.cpp:1801-1806, :2691-2699). ES2022/23: `.at()`, `hasOwn`,
`findLast`, hashbang, numeric separators (:670-673, :1203-1208;
JavascriptLibrary.cpp:2048, :2096-2097, :2352, :2408-2409, :4210, :4446;
ESNumericSeparator FLAG at :1197) — note `.at()`/`hasOwn`/String-padding are
unconditional (no ConfigFlags gate) even though nearby flags exist. Also present:
`flat`/`flatMap`/`Array.includes` (`InJavascript/Array_prototype.js:310, :327,
:453`), String includes (C++ builtins), and error `cause`
(`JavascriptError.cpp:68-71`). OFF by default: SharedArrayBuffer/Atomics (:709,
:1189; rlexe :80-88), BigInt (:669, :1194; `test/BigInt/rlexe.xml`), ES6 RegExp
symbols and prototype properties (DEFAULTs :686-696; FLAGs :1171, :1180). At
the 2026-10-10 baseline, the engine also had no optional chaining/logical
assignment, class fields/`#private`/static blocks, `replaceAll`, `matchAll`,
`WeakRef`, `FinalizationRegistry`, named capture groups/lookbehind, import
attributes, or RegExp `d`/`v` flags. The modernization branch has since landed
those C2 surfaces, including `u` Unicode property escapes and `v` Unicode Sets
mode on ICU 79.1; decorators and `queueMicrotask` remain unimplemented. BigInt,
RegExp symbols
and SAB/Atomics remain default-off; the SAB/Atomics flag-on surface now includes
growable buffers and non-shared Atomics but awaits the product/security
decision. The repeatable probe is `tools/test262/feature-probe.js`; see
[modernization-plan.md](modernization-plan.md) §4 for the current gap list and
per-batch receipts.

Intl: only Collator/DateTimeFormat/NumberFormat/PluralRules (`InJavascript/Intl.js`,
IntlEngineInterfaceExtensionObject.cpp; `ENABLE_INTL_OBJECT` :6-23). The ICU C API is
wrapped by `lib/Runtime/PlatformAgnostic/ChakraICU.h` (:36-46, 11 ICU headers,
HAS_ICU, WINDOWS10_ICU variant); `UnicodeText.h` builds on it with non-ICU fallbacks
(:8-13, :40-58).
Embedded ICU is pinned to 63.2 (root CMakeLists.txt:196-222); system/custom ICU
is unpinned; the current Mac pilot is run-verified on ICU **79.1** (the
prior 78.3 formatting baseline was updated for the new apostrophe/literal
placement in `Intl/DateTimeFormat.js`). macOS `--no-icu` fallback
(:268-275). RelativeTimeFormat/ListFormat/DisplayNames/Segmenter/DurationFormat:
0 hits.

WASM (`ENABLE_WASM`): `WasmBinaryReader` (:39-56); `WasmToAsmJs` reuses the asm.js
pipeline (:66-70); Module/Instance/Memory/Table in Runtime/Library
(`WebAssemblyModule.h:30-58`). MVP ON + sign-ext/nontrapping ON; threads OFF,
multi-value OFF, SIMD flag-only (`lib/Common/ConfigFlagsList.h:428-447`, :996;
`wasm.simd/rlexe.xml`); `WasmI64` defaults false (:429). GC/tail-calls/EH/stringref/
extended-const: 0 opcode hits (`WasmBinaryOpCodes.h`); `WasmSpec/features` tracks
extends/nontrapping/threads only, with a separate `test/WasmSpec.MultiValue/` suite.

Extras: SCACore Serializer/Deserializer over StreamWriter/Reader (SCACore.h:12-52);
`Runtime.h` umbrella (Parser + RuntimeCommon :13-43); TTD time-travel debugging
(`ENABLE_TTD`, `ch` record/replay).

## 4. Build (summary)

Full detail is in [build.md](build.md). All commands below are source-cited; the
macOS ARM64 leg is additionally run-verified ([build.md](build.md) §13).

Unix: `build.sh` usage :31-92, args :162-407, cmake invocation :636-640, make/ninja
:644-656, `out/<Debug|Release|Test>` :548-556. `build.sh` passes `*_SH` vars that
`CMakeLists.txt` consumes and unsets from cache (:29-53, :65, :83-88, …).
Windows: `Build/Chakra.Core.sln` (VS15) via `test/ci.buildone.cmd` (arg check :15-20)
→ `ci.build.init.cmd` → `test/ci.build.cmd` msbuild `/m
/p:Configuration=<Debug|Test|Release> /p:Platform=<x86|x64|arm>` (:91-115), with
chk → Debug, fre → Release, test → Test (:92-97).

Root CMakeLists.txt: minimum 3.10 (:1); options DISABLE_JIT/INTL_ICU(ON)/EMBED_ICU/
USE_CCACHE (:12-18); `add_subdirectory` pal :615, lib :637, bin :639, test :641.
`lib/`: `ChakraCoreStatic` from 20 unconditional plus up to 4 conditional object
libs; Backend iff BuildJIT (:86-88); WasmReader iff AMD64 (:95-96); wabt iff
CAN_BUILD_WABT = non-Release AMD64 (root :282-284, lib :102-104); LTTng iff
`USE_LTTNG`; headers copied to `<build-root>/include` (:106-122). `bin/`:
GCStress (skipped on Android/Valgrind), `ch`, ChakraCore shared unless
STATIC_LIBRARY. `ch` static links ChakraCoreStatic + ICU + pthread/dl (:51-90),
shared links PAL objects (:99-107), post-build copy of `ch` to build root
(:131-135). ChakraCore shared uses `--whole-archive` + version script (:28-60).
C++17 on both sides (root CMake :398-399; `Build/Common.Build.props:101`).

Toolchains: clang required, minimum 3.7 (`build.sh`:514-521); `--cc`/`--cxx`
(:169-177, must be paired :472-476); CI installs ninja-build clang libicu-dev
(azure :46, :51, :56, :61, …); LTO downloads LLVM 5.0.0 + gold via
`tools/compile_clang.sh` (:7; build.sh :440-464, RANLIB disabled :466-469). MSVC:
README says VS2015/2017 (:37); solution is VS15 (`Build/Chakra.Core.sln:2-5`, minimum
VS 14.0) with v141 for the 15.0 toolset (`Build/Common.Build.Default.props:19`);
toolsets v110, v120, v140–v143, v145 auto-select — no v144 (:16-22); SDK
auto-detected from registry except an ARM fallback pin to 10.0.10240.0
(`Build/Chakra.Build.Default.props:5-14`, `:17`).
Python: `build.sh`:131 probe; `find_package(... Interpreter ...)`
(`test/CMakeLists.txt:1`, `bin/ch/CMakeLists.txt:1`); scripts are py2/3-compatible
(`regenByteCode.py:22`). No libuv (negative grep). ICU xplat: --system/--custom/
--embed/--no-icu (build.sh :228-255); EMBED_ICU builds icu4c-63_2-src.tgz from a
SHA512-pinned URL statically (:192-222); custom path (:223-253); Linux links
icuuc+icui18n (:255-267); macOS `--no-icu` fallback (:268-275); CI apt/brew +
`-DICU_INCLUDE_PATH` (azure :76-88). ICU on Windows: `deps/Chakra.ICU` MSBuild glue (6 vcxproj + props, no vendored
sources); default `ChakraICU=windows` links system ICU
(`Build/Chakra.Build.Default.props:32`); defines in `Build/Chakra.Build.props:42-62`
(HAS_ICU, INTL_ICU, U_STATIC_IMPLEMENTATION, WINDOWS10_ICU).
STALE: ICU 63.2 (2018) at CMake :216 vs build.sh :53 help text still saying 57.1.

Codegen: `jstoc.py` JS → C header (:15, :18-61), runs on DbgController.js (:2-6).
`regenByteCode.py` from InJavascript (:8-22); `--verify` bytecode-check
(`test/CMakeLists.txt:33-38`; Windows `ci.testone.cmd:102-116`); Release skips it.
`lttng.py` from the Scripting-Chakra `.man` (`build.sh:540-545`; CMake :623-635, no
static-only requirement — the static-only FATAL at CMake :141-156 gates `--trace`
/ `ENABLE_CC_XPLAT_TRACE`, not LTTng). `CMakeFeatureDetect` `disable_tail_calls`
probe (:17-34); RecyclerChecker `--wb-check`/`--wb-analyze` (build.sh :560-610,
CMake :557-586); `pal/src/configure.cmake` feature detection (root :606).
Suspected repo bug (inspection only, needs a configured-build probe): root
CMakeLists.txt:610-613 tests `CLANG_SANITIZE` but expands `${CLANG_SANITIZE_SH}`
after it was unset at :48-51, so `--sanitize` flags may never apply.

Types/outputs: xplat Debug/Release/RelWithDebInfo (`-t` → Test dir :548-552);
Windows Debug(chk)/Test/Release(fre) (`ci.build.cmd:91-100`). Outputs:
`out/<Type>/ch` + `libChakraCore.so` (`bin/ChakraCore/CMakeLists.txt:1`, post-build
copy; README :56 shows the `ch` path only); Windows
`Build/VcBuild/bin/<arch>_<config>/ch.exe` (README :55). Canonical commands:
`./build.sh -j`; `./build.sh --debug --static -n -j`; direct CMake
`cmake -GNinja -DCMAKE_BUILD_TYPE=RelWithDebInfo -DCMAKE_CXX_COMPILER=clang++
-DCMAKE_C_COMPILER=clang .. && ninja && ninja check`.

## 5. Platforms

README :7: x64 Linux/macOS/Windows; x86 + ARM Windows-only; x86/ARM Linux + ARM macOS
as a future goal. CMake: `CC_TARGETS_*` (:68-84); system-arch map
x86_64 → AMD64 / armv7l → ARM / arm64|aarch64 → ARM64, fatal on unknown (:55-89);
`build.sh --arch` (:617-632; note the :39 help text omits arm64 though :623-625
accepts it). OS: ANDROID (NDK armeabi-v7a API 21) / LINUX / OSX (:125-139), fatal
otherwise (:358); `--target=android` (:310-332); `--xcode` (:289-293, project only,
MAKE=0); macOS minimum 10.9 (:344-351). ARM64-Linux unsupported + ARM64 JIT
non-functional off-Windows (:547-555). Windows: Debug|Test|Release ×
ARM/ARM64/CHPE/x64/x86; Win10 SDK default, Win7 NtTarget, ARM/64 Win8 minimum
(`Chakra.Build.Default.props:5-18, :23, :26`). Stale floors: cmake 3.10, macOS 10.9,
VS2015/17 + Win7 text (README :37) vs CI windows-2025/2022, cirrus
macos-ventura-xcode, checkout@v2 (`.github/workflows/agreement.yml:11`, CLA only).

## 6. Tests

Layout: 85 entries, 2801 `.js`; benchmarks ARES-6/jetstream/Kraken/Octane/
Octane_Closure/SunSpider + perf.pl/perftest.pl; native-tests (11 entries) including
`test_native.sh` (:1-20).

Runners: `runtests.py` argparse (:22-37), timeouts 60/180 (:39-40),
`--debug`/`--test`/`--variants` (:51-56); requires `-d`/`-t` (:119-123); default
variants `interpreted,dynapogo` (:131-132); tag/not-tag/slow/variants/timeout
(:39-91); auto-excludes `exclude_xplat` + `require_simd` off-Windows (:176-178),
`exclude_mac` on Darwin (:190-191); xplat hard-codes arch x64 (:114-115); default
binary `out/<Flavor>/ch` (:146). Windows `runtests.cmd` → rl.exe, same defaults
(:330), requires arch + config (:37-56), `-codecoverage`. `rl` sources live in
`bin/rl` (built on Windows; not in the xplat `bin/CMakeLists.txt`); embedding tests
in `bin/NativeTests`.

CI gates: xplat `check` = bytecode-check + regression-check (`python runtests.py
--debug|--test [--not-tag exclude_noicu] [--variants=disable_jit]
--binary=<build>/ch`, `test/CMakeLists.txt:39-45`) on Debug/RelWithDebInfo; Release
only smokes `ch test/Basics/hello.js` (:24-31); native-check runs `test_native.sh`
unless NO/EMBED_ICU (:48-64); when shared, `check` also depends on ChakraCore
(:66-68). Exact: `ninja check` / `make check`. Windows `ci.testone.cmd`: bytecode
verify + runtests.py + runnativetests.cmd, failure codes 2/3/4 (:62-95, :109, :127,
:144). azure: trigger master/release/* + weekly (:6-17); Style
check_{copyright,ascii,eol,tabs}.sh (:26-38); CMake legs ubuntu-24.04 ×
Debug/NoJit/RelWithDebInfo/Release, ubuntu-22.04 × RelWithDebInfo/Release only,
macOS × DebugNoICU-static/RelWithDebInfo/Release (no macOS NoJit leg) →
cmake/ninja/ninja check (:93-120; matrix :44-88); MSVC windows-2025 x86/x64 ×
debug/test/release plus x86-only NoJit, and windows-2022 x86/x64 release-only
build, via ci.buildone + ci.testone with tests only on debug/test/NoJit (:202-219;
matrix :127-198). cirrus: 3× ARM64 macOS noJit Debug/RelWithDebInfo/Release (brew ninja
icu4c, STATIC_LIBRARY=ON except the RelWithDebInfo task, DISABLE_JIT=ON, ninja
check :6-31). The legacy `test/262` directory holds one `262test.js` fixture and
its `rlexe.xml`, not a full test262 checkout. The regression count is not a
test262 conformance rate. Pinned upstream corpus/runner tooling is now under
`tools/test262/`, with positive and deliberate-failure controls, raw scenario
results, and per-feature summaries. Its diagnostic corpus includes newer
proposals; full ES2025 gating requires a validated edition/host policy.
Run-verified 2026-10-10 on macOS ARM64 (Cirrus RelWithDebInfo recipe, DISABLE_JIT,
Homebrew ICU 78): `ninja check` EXIT=0, regression Total passed 1959 / failed 0 —
see [build.md](build.md) §13. Other platforms/configs remain run-unverified.

## 7. Activity / health

Baseline HEAD `97e68c917` (2026) "ci: use windows 2025 runners (#7048)" by Lukas Kurz; prior
work Dec 2025 by Ivan Krasilnikov (clang/ICU/ARM64 + C++17 fixes). Commits per year
(author date): 1×2026 / 23×2025 / 25×2024 / 10×2023 / 36×2022 (committer-date counts
differ: 9/15/26/9/36 — the gap is rebased/merged history; either way this is
sustainer-scale CI/toolchain work, no major feature work — recent history holds only
small functional fixes (e.g. TryFinally/JIT loop tracking, JIT Generator loops,
Module API bugs)). Recent-30 authors: Kurz 15, Krasilnikov 7, plus 8 by 4 others
(Richard 3, Petr Penzin 3, Stephan T. Lavavej 1, rhuanjl 1). Fork:
`origin git@github.com:amscotti/ChakraCore.git`, master tracking origin/master,
the modernization branch starts at this baseline and has committed `doc/`,
the toolchain pin, and pilot harness; `git tag` was EMPTY at baseline (upstream tags
unconfirmed). Link risks
(unchecked): technet.microsoft.com URLs (README :23, CONTRIBUTING :11, retired
domain); Microsoft/ChakraCore vs chakra-core/ChakraCore + aka.ms mix; wabt
travis/appveyor badges.

## 8. Modernization constraints

(a) The 2026-10-10 baseline was a partial ES2015–ES2019 engine: ES2018
named groups/lookbehind/Unicode property escapes and `?.` / logical-assign /
class-fields / `#private` / static-blocks / `WeakRef` / `FinalizationRegistry` /
`replaceAll` / `matchAll` were absent. The modernization branch has since
landed the C2 parser/builtins batches recorded in
[modernization-plan.md](modernization-plan.md) §6, including class elements,
S2 BigInt slices, S8 resizable buffers/Float16, S9 regex, S12 import attributes,
S13 WeakRef/FinalizationRegistry, and S14 eval/statement fixes. Remaining
feature work is tracked in that plan; decorators and `queueMicrotask` are not
claimed. (Error `cause`, `flat`/`flatMap`/`includes` were already present in
the baseline.) (b) Intl 4-constructor subset; embedded ICU 63.2
(2018), current system-ICU pilot 79.1. ICU upgrade and `--no-icu` behavior need
explicit dependency/test decisions. (c) WASM MVP + sign-ext /
nontrapping; threads/multi-value/SIMD gated off; GC/tail-calls/EH/stringref/
extended-const absent. (d) BigInt / RegExp-symbols remain OFF by default. The
SAB+Atomics flag-on surface now includes growable SharedArrayBuffer and
non-shared Atomics, but the feature remains OFF by default pending the
product/security decision; keep all three flagged until deliberately enabled. (e) C++17 + Rotor PAL; x86/ARM win-only;
ARM64 JIT broken off-Windows — PAL/backend first. (f) No sandbox/heap cage, no
pointer compression; mark-sweep GC with concurrent/parallel mark + concurrent sweep
— hostile-input handling is a threat-model decision. (g) Legacy floors describe
the old build, not a compatibility commitment for this revival. Modern floors may
be raised deliberately with explicit target/SDK/dependency records and CI coverage:
the current ICU 78 Mac pilot chooses macOS 26.0, rather than inheriting host 26.6.
Legacy CMake 3.10, VS2015/17, LLVM5 LTO, ICU 63.2 and the suspected `--sanitize`
flag-drop bug remain reference-build issues. The local ARM64 Mac build is
run-verified; the broader CI matrix is source-inspected. (h) Release
tests ~nothing (hello.js smoke); Debug/RelWithDebInfo `check` is the real gate;
default variants interpreted,dynapogo. (i) Docs live on the wiki — put durable
decisions in-repo (`doc/`). (j) Sustainer community (~1-36 commits/yr, ~2 authors) —
plan to carry the fork. (k) Preserve advantages: tiny C API, TTD debugging,
single-binary WASM + Intl vs QuickJS-ng (small, current-spec, frequent releases),
V8 (multi-tier JIT, pointer compression, sandbox), JSC (multi-tier JIT) — competitor
internals are secondary-sourced anchors, not verified claims. Feature work targets
full ES2025 conformance — see [modernization-plan.md](modernization-plan.md) §4.
(l) Zig 0.17.0 is installed/pinned and reports Clang 22.1.8. Build rework and
`@cImport` removal / external `translate-c` APIs were checked against its release
notes and installed sources. The Mac no-JIT C/C++/assembly-at-scale pilot has run;
JIT/WASM/WABT and other platforms are subsequent milestones. Native callback
retention uses `forceUndefinedSymbol`, not WASM-only `export_symbol_names`.
The original regex delta was a 16 MiB linker stack default: an explicit 8 MiB
stack restores 1959/0, rather than requiring a test waiver. ZLS has no 0.17 release
as of this review and does not gate Phase 1. SDK/ICU/tool versions are separate
inputs to a future hermetic build; pinning Zig alone does not pin them.
See [modernization-plan.md](modernization-plan.md) and [build.md](build.md) §14.

## 9. Gaps and open items

Resolved during review: error `cause` PRESENT, `flat`/`flatMap`/includes PRESENT,
velocity counts pinned, GC concurrency confirmed, LTTng-vs-`--trace` attribution
fixed, `rl` xplat absence explained, `flat` conflict settled by opening
`Array_prototype.js`. Resolved 2026-10-10 by execution: baseline build/test green
(1959/0) on macOS ARM64; the stack-aligned Zig variant also passed 1959/0 with
bytecode and shared embedding checks. Runtime `d`/`v`/import-attribute probes
confirmed absence; ES2018 regex gaps and the runtime/TLS distinction were corrected.

Reviewed: PAL configure/source-list bodies (including target-side run probes and
`config.h.in` generation), `ci.build.init.cmd` / `ci.parsetestargs.cmd` (MSBuild
setup and x86/x64 test routing). Still open: android_toolchain.sh, create_package.sh,
XPlatInstall/install_ch.sh, icu/configure.py bodies unopened. Unpinned: clang
maximum, Windows SDK. Unverified: link liveness, GitHub archival flag, upstream
tags, V8/JSC specifics, QuickJS-ng ES2024/25 delta, full-scale C++ build.zig
gotchas, macOS/Windows incremental behavior, and full edition-conformance results.
The pinned test262 diagnostic baseline is a foundation for measuring older gaps,
not evidence that any complete edition already passes.

The first [conformance baseline](conformance-baseline.md) ran 93,141 scenarios
from 48,598 files: 61,565 passed / 31,576 failed, with 902 explicit skips. Its
corpus includes post-ES2025 proposals and uses default language flags.

**Modernization checkpoint (2026-10-21):** the merged branch is green at
**2026/0 test** and **2078/0 debug-static** on Mac ARM64 no-JIT. Direct-eval
`super` early errors are closed (eval-code 454/0; object-literal function/arrow
false accepts fixed in `39e9f2a16`), the flag-on S6a slice has growable
SharedArrayBuffer plus ES2020 non-shared Atomics (focused S6 574/982;
112 fail→pass / 0 pass→fail; debug index-detach abort fixed), and S6b adds
`Atomics.pause` plus single-thread `waitAsync` (80/124; 0 pass→fail), and
S6c adds BigInt64Array/BigUint64Array Atomics while BigInt and SAB/Atomics
remain off by default. The catch-hidden eval-`arguments`
under-rejection is closed, the bounded immutable-ArrayBuffer surface is
landed with explicit JIT/TTD residuals, and both S3 RegExp surfaces are
landed on ICU 79.1: `u` property escapes with a deterministic
code-point-set matcher, `v` Unicode Sets mode with set operations,
`\q{...}`, properties-of-strings and `RegExp.prototype.unicodeSets`, and
u-mode strict syntax/astral semantics with literals corpus 480/0.
The 30-directory fixture-cache subset has also been rerun with the fixed
harness: **1,367/998 of 2,365**, 0 pass→fail and 102 fail→pass versus the old
precombined result ([c4-fixture-rerun.md](c4-fixture-rerun.md)). S3
Unicode/ICU, S6 default enablement, B4 Linux/JIT/WASM and C4 host-negative
qualification remain decision- or hardware-blocked; no JIT, Linux, native-x64,
TTD or multi-agent claim is made.
