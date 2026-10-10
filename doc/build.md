# ChakraCore Build

Pinned to HEAD `97e68c917`. This document describes the build as it exists today:
a dual-entry system — `build.sh` + CMake on Unix, MSBuild solution + `.cmd` drivers
on Windows. Every command, option, and path below is cited to a file and line that
was read at this HEAD.

> Scope caveat: the reference below is source-inspection verified throughout, and
> the macOS ARM64 baseline in §13 was additionally run-verified green on 2026-10-10
> (1959 passed / 0 failed). Everything else remains run-unverified; the CI matrix
> in §11 is the known-good set.

## 1. Overview and entry points

| Platform | Entry | Backend | Output root |
|---|---|---|---|
| Linux / macOS | `./build.sh [options]` (695 lines) | CMake 3.10+ → make or ninja | `out/<Debug\|Release\|Test>/` |
| Linux / macOS (direct) | `cmake … && ninja` (used by CI) | CMake directly, no `build.sh` | caller-chosen `build/` |
| Windows | `Build\Chakra.Core.sln` via `test\ci.buildone.cmd <arch> <type>` | MSBuild | `Build\VcBuild\bin\<arch>_<config>\` |

There is no `build.cmd` at the repo root — the Unix and Windows entries are
entirely separate. `build.sh` is a flag-translating wrapper: it converts its CLI
options into `-D*_SH=...` CMake variables, runs one `cmake` configure
(`build.sh:636-640`), then one make/ninja invocation (`build.sh:644-656`). Root
`CMakeLists.txt` consumes most `*_SH` variables and unsets them from the cache so
re-configures do not go stale (e.g. :29-53, :65, :86-88) — with two exceptions:
`CC_TARGETS_ARM64_SH` is never unset (:86-88 cover only ARM/X86/AMD64), and :483
unsets a misspelled `DENABLE_FULL_LTO_SH`, leaving the real `ENABLE_FULL_LTO_SH`
cached.

## 2. Prerequisites

Unix (`build.sh`, root `CMakeLists.txt`):

- **clang** (required): minimum version 3.7, enforced at `build.sh:514-521`.
  Detection order: `--cc`/`--cxx` if given (must be paired, :472-476), else `c++`
  if it is clang, else `/usr/bin/clang++`, else `$CXX`/`$CC` (:471-512). No maximum
  version is validated — CI just installs distro clang.
- **cmake** ≥ 3.10 (root `CMakeLists.txt:1`), **make** or **ninja** (`-n` selects
  ninja, `build.sh:276-279`).
- **Python** interpreter: probed at `build.sh:131` (`python3` → `python` → …);
  CMake requires it (`find_package(... Interpreter ...)` in `test/CMakeLists.txt:1`
  and `bin/ch/CMakeLists.txt:1`). Scripts are py2/3-compatible
  (`regenByteCode.py:22`).
- **ICU** (unless `--no-icu`): system, custom path, or embedded — see §6.
- For `--lto` on Linux without GNU gold: `tools/compile_clang.sh` builds LLVM 5.0.0
  (:7) into `cc-toolchain/`; `RANLIB` is disabled for LTO builds
  (`build.sh:440-469`).

Windows (`README.md:37`, `Build/`):

- README says VS2015/2017 on Win7 SP1+; the solution is VS15
  (`Build/Chakra.Core.sln:2-5`, minimum VS version 14.0 at :5, so VS2015/v140 is
  also permitted) with v141 for the 15.0 toolset
  (`Build/Common.Build.Default.props:19`); toolsets v110, v120, v140, v141, v142,
  v143, v145 auto-select by VS version — no v144 (`18.0` → v145, :16-22); the
  Windows SDK is auto-detected from the registry
  (`Build/Chakra.Build.Default.props:5-14`) except an ARM fallback pin to
  10.0.10240.0 (:17). C++17 both sides
  (`CMakeLists.txt:398-399`, `Build/Common.Build.props:101`).
- Chakra.ICU projects under `deps/` provide ICU on Windows.

There is no libuv dependency (negative grep) and no Node/npm involvement.

## 3. Quick start

Unix, defaults (Release, shared lib, system-arch target):

```sh
./build.sh -j
```

Unix, debug static ninja build with explicit compilers:

```sh
./build.sh --debug --static -n -j --cc=/usr/bin/clang --cxx=/usr/bin/clang++
```

Unix, direct CMake exactly as CI does it (Release with debug info + tests):

```sh
mkdir -p build && cd build
cmake -GNinja -DCMAKE_BUILD_TYPE=RelWithDebInfo \
  -DCMAKE_CXX_COMPILER=clang++ -DCMAKE_C_COMPILER=clang ..
ninja
ninja check
```

Windows (from any prompt at the repo root — `ci.build.init.cmd` locates MSBuild
itself via `Build/scripts/add_msbuild_path.cmd`, using PATH msbuild or probing
Dev17/16/15/14 installs):

```cmd
test\ci.buildone.cmd x64 test
test\ci.testone.cmd x64 test
```

## 4. Unix build in detail (`build.sh`)

Usage text: :32-92. Argument parsing: :162-407. Selected options and what they
become:

| Option | CMake variable(s) | Notes |
|---|---|---|
| `--arch=(arm,x86,amd64[,arm64])` | `CC_TARGETS_*_SH=1` | Help (:39) omits `arm64` though :623-625 accepts it (and `aarch64`); default is system arch (:630-631) |
| `--cc/--cxx=PATH` | `CMAKE_C/CXX_COMPILER` | Must be used together (:472-476) |
| `-d/--debug` | `CMAKE_BUILD_TYPE=Debug` | |
| `-t/--test-build` | `CMAKE_BUILD_TYPE=RelWithDebInfo` | Output dir is still `Test/` (:548-552) |
| `-j/--jobs[=N]` | make/ninja `-j` | `-j` alone uses `nproc` (:210-226) |
| `-n/--ninja` | `-G Ninja`, `MAKE=ninja` | |
| `--no-jit` | `NO_JIT_SH=1` → `BuildJIT` off | Root CMake :518-527 |
| `--static` | `STATIC_LIBRARY_SH=1` | Default is shared (`SHARED_LIBRARY_SH=1`, :109) |
| `--libs-only` | `LIBS_ONLY_BUILD_SH=1` | Skips `ch` and GCStress |
| `--system-icu/--custom-icu/--embed-icu/--no-icu` | `SYSTEM_ICU_SH` / `ICU_INCLUDE_PATH_SH` / `EMBED_ICU_SH` / `NO_ICU_SH` | :228-255, :409-411; default probes system ICU on Linux |
| `--without-intl` | `INTL_ICU_SH=0` | Intl defaults ON (:107) |
| `--lto/--lto-thin` | `ENABLE_FULL_LTO_SH` / `ENABLE_THIN_LTO_SH` | LLVM 5.0.0 + gold path :440-464 |
| `--lttng` | `ENABLE_JS_LTTNG_SH=1` | Runs `tools/lttng.py` pre-configure (:540-545) |
| `--trace` | `ENABLE_CC_XPLAT_TRACE_SH=1` | **Static-only**: FATAL otherwise (root CMake :141-156) |
| `--sanitize=CHECKS` | `CLANG_SANITIZE_SH` | Suspected repo bug — see §11 |
| `--target=android` | `CC_TARGET_OS_ANDROID_SH=1` + toolchain env | :310-332, NDK armeabi-v7a API 21 |
| `--target-path=PATH` | output root (default `out/`) | Rejects `~/` (:532-536) |
| `--xcode` | `-G Xcode -DCC_XCODE_PROJECT=1` | Project only — no build runs (`MAKE=0`, :289-293) |
| `--create-deb=V` | — | amd64-only `.deb`, libc6/uuid-dev/libicu-dev deps (:665-691) |
| `--valgrind` | `ENABLE_VALGRIND_SH=1` | Disables concurrent GC; x86 exempt; skips GCStress |
| `--ccache[=NAME]` | `CCACHE_PROGRAM_NAME_SH` | |
| `--extra-defines=` / `--without=` | `EXTRA_DEFINES_SH` / `WITHOUT_FEATURES_SH` | `--without=X` → `-DCOMPILE_DISABLE_X=1` |
| `--wb-check/--wb-analyze FILE` | `WB_CHECK_SH` / `WB_ANALYZE_SH` | Builds RecyclerChecker plugin; single-file mode needs ninja (:560-610) |
| `--wb-args=ARGS` | `WB_ARGS_SH` | Plugin args (`;`-separated); `-fix` forces `-j 1` (:385-389, :580-585) |
| `-v/--verbose` | — | Echoes resolved options back (:184-186, :427-438) |
| `--icu=PATH` | same as `--custom-icu` | Legacy alias (:236-241) |
| `-h/--help` | — | Prints usage and exits (:179-182) |
| `-y` | auto-yes | |

Notable silent behaviors: `--target=<anything-but-android>` is silently ignored
(:310-332 only act on `android`, with no error otherwise); when `build.sh` runs
under a parent `make -j`, host `MFLAGS` win and `MULTICORE_BUILD` is cleared
(:646-651).

Flow: parse args → optional LTO toolchain setup (:440-464) → clang detection
(:471-521) → target path + `Test` dir mapping (:528-556) → optional LTTng header
generation (:540-545) and write-barrier setup (:560-610) → `cmake` configure
(:636-640) → `$MAKE … | tee build.log` (:644-656) → optional `.deb` (:665-691).

## 5. Windows build in detail

Chain: `test\ci.buildone.cmd <arch> <type> [extra msbuild args]` (arg check :15-20)
→ `test\ci.build.init.cmd` (sets `REPO_ROOT`, msbuild setup, `JENKINS_BUILD_ARGS`)
→ `test\ci.build.cmd` (:29), which parses arch (`x86`/`x64`/`arm`, plus deprecated
`amd64`, :19-34) and type (`debug`/`test`/`release` or legacy `chk`/`fre`, :37-55)
and runs:

```cmd
msbuild /m /p:Configuration=<Debug|Test|Release> /p:Platform=<x86|x64|arm>
  Build\Chakra.Core.sln … /verbosity:minimal /nr:false
```

(`ci.build.cmd:113-115`, file logging params :87-88). Type mapping: `chk` → Debug,
`test` → Test, `fre` → Release (:92-97). There is also a `swb` switch
(`ForceSWB=true`, :62-66).

Output: `Build\VcBuild\bin\<arch>_<config>\ch.exe` (e.g. `x64_debug`, README :55).
Windows-ChakraICU defaults (`ChakraICU=windows`, `EnableIntl=true`) live in
`Build/Chakra.Build.Default.props:29-34`.

## 6. Dependencies: ICU matrix

| Mode | Unix mechanism | Windows mechanism |
|---|---|---|
| System ICU | default on Linux: links `icuuc` + `icui18n` (root CMake :255-267) | n/a |
| Custom path | `--custom-icu=PATH`, `PATH/../lib` must hold libs (build.sh :146-160, root CMake :223-253) | n/a |
| Embedded | `--embed-icu`: downloads `icu4c-63_2-src.tgz` (SHA512-pinned URL, root CMake :212-222), static build | `deps/Chakra.ICU` vcxproj set |
| None | `--no-icu` (disables Unicode + Intl); macOS falls back here automatically when no ICU path is given (:268-275) | `ChakraICU=false` (`EnableIntl=false` drops Intl but keeps ICU/Unicode, :42-50) |

Windows ICU defines: `Build/Chakra.Build.props:42-62` (`HAS_ICU`, `INTL_ICU=1`,
`U_STATIC_IMPLEMENTATION`, `WINDOWS10_ICU`). Known staleness: embedded ICU is 63.2
(2018) while `build.sh:53` help text still says 57.1.

## 7. CMake structure and options

Root `CMakeLists.txt` (641 lines):

- Direct-CMake options (no `build.sh`): `DISABLE_JIT` (OFF), `INTL_ICU` (ON),
  `EMBED_ICU` (OFF), `USE_CCACHE` (OFF), `ICU_INCLUDE_PATH`, Debug default
  (:11-21). `RelWithDebInfo` gets `-O3 -g` (:4).
- Arch: `CC_TARGETS_*` from `*_SH` vars or system processor
  (x86_64 → AMD64, armv7l → ARM, arm64/aarch64 → ARM64), fatal on unknown
  (:55-89); per-arch defines/flags (:277-321, fatal :320). Android NDK ABI
  armeabi-v7a API 21 (:125-134); OS fatal otherwise (:358).
- JIT: `NO_JIT_SH`/`DISABLE_JIT` → `add_definitions(-DDISABLE_JIT=1)`,
  `BuildJIT` off (`NO_JIT_SH` *unsets* `BuildJIT` while `DISABLE_JIT` sets it 0 —
  both falsy, :518-527). `CAN_BUILD_WABT` = non-Release AMD64 (:282-284).
- LTO (:476-496), Valgrind (:107-114, x86 exempt), `--trace` static-only FATAL
  (:141-156), `-Werror` Linux-only (:327-337), `-fno-rtti` only for static Release
  (:466-473), C++17 required (:398-399), `disable_tail_calls` probe via
  `Build/CMakeFeatureDetect.cmake` (:451, probe :17-34), RecyclerChecker wiring
  (:557-586), `pal/src/configure.cmake` feature detection (:606).
- Suspected bug (inspection only): :610-613 tests `CLANG_SANITIZE` but expands
  `${CLANG_SANITIZE_SH}` after that variable was unset at :48-51, so `--sanitize`
  flags likely never reach the compile/link lines. Needs a configured-build probe.
- Subdirectories: `pal` :615, `lib` :637, `bin` :639, `test` :641.

`lib/CMakeLists.txt`: `ChakraCoreStatic` from 20 unconditional plus up to 4
conditional `$<TARGET_OBJECTS:…>` entries (Backend, WasmReader, wabt, LTTng);
Backend only if `BuildJIT` (:86-88); WasmReader AMD64-only (:95-96, with
`BUILD_TESTS/RUN_BISON/RUN_RE2C` off at :97-99); wabt only if `CAN_BUILD_WABT` (:102-104);
LTTng only if `USE_LTTNG` (:17-19); post-build copies `ChakraCore.h`/
`ChakraCommon.h`/`ChakraDebug.h`/`ChakraCoreVersion.h` to
`${CMAKE_BINARY_DIR}/include` (e.g. `out/Release/include`, :106-122).

`bin/CMakeLists.txt`: GCStress unless Android/Valgrind; `ch` unless libs-only;
`ChakraCore` shared unless `STATIC_LIBRARY`. Note: `rl` is **not** built by xplat
CMake (Windows-side runner; `bin/rl/CMakeLists.txt` defines an `rl` library, but
`bin/CMakeLists.txt` never adds the subdirectory).

`bin/ch/CMakeLists.txt`: generates `DbgController` header via `jstoc.py` (:2-6);
`ch` sources :8-17; static links `ChakraCoreStatic` + ICU + pthread/dl (:51-90,
Linux `-pie --start-group`, macOS `-force_load`); shared links PAL/Codex/
PlatformAgnostic singular objects + ICU (:92-107); version script on Linux/Android
(:109-113); post-build copy of `ch` to build root (:127-135).

`bin/ChakraCore/CMakeLists.txt`: `add_library(ChakraCore SHARED …)` (:1);
Linux/Android `--whole-archive` + `-static-libstdc++` (:28-39);
`-Wl,--version-script=libChakraCoreLib.version` (:55-59); post-build copy of
`libChakraCore.<so|dylib>` to build root (:73-80).

## 8. Codegen steps

- `bin/ch/jstoc.py`: JS → C const-char-array header (:15, :18-61); runs on
  `DbgController.js` at `ch` build time (`bin/ch/CMakeLists.txt:2-6`).
- `tools/regenByteCode.py`: regenerates embedded bytecode headers
  (`JsBuiltIn*.bc.*.h` under `lib/Runtime/Library/InJavascript`) from the JS
  sources (:8-22); `--verify` mode is the `bytecode-check` gate
  (`test/CMakeLists.txt:33-38`; Windows `test/ci.testone.cmd:102-116`).
- `tools/lttng.py`: ETW `.man` → LTTng headers, run pre-configure when `--lttng`
  (`build.sh:540-545`); CMake side at root :623-635 (no static-only requirement).
- `Build/CMakeFeatureDetect.cmake`: `disable_tail_calls` compiler probe (:17-34).
- RecyclerChecker (`tools/RecyclerChecker`): clang plugin for write-barrier
  `--wb-check`/`--wb-analyze` (build.sh :560-610; CMake :557-586).

## 9. Build outputs and layouts

Unix (`out/<Type>/`, where `-t` maps RelWithDebInfo → `Test/`):

- `ch` (copied post-build from `bin/ch/ch`), `libChakraCore.so` (or `.dylib`;
  copied post-build from `bin/ChakraCore/`), `GCStress` (unless skipped),
  `build.log` at the build root, `include/` with the four public headers.

Windows: `Build\VcBuild\bin\<arch>_<config>\` with `ch.exe` (+ `rl.exe`, native
test binaries).

## 10. Testing

`test/`: 85 top-level entries, 2801 `.js` files. Suites include test262 (`test/262`
via `rlexe.xml` gating), Array, AsmJs, es5/es6/es7, Intl, WasmSpec (+
`WasmSpec.MultiValue`, `wasm`, `wasm.simd`), Debugger, Generated, benchmarks
(ARES-6, jetstream, Kraken, Octane, Octane_Closure, SunSpider), native-tests, and
the UnitTestFramework.

`test/runtests.py` (xplat driver, also used on Windows by `ci.testone.cmd`):

- Requires `-d/--debug` or `-t/--test` (:119-123); default variants
  `interpreted,dynapogo` (:131-132); timeouts 60s default / 180s slow (:39-40);
  `--tag/--not-tag/--include-slow/--variants/--timeout/--flags/--extra-flags`
  (:39-91); `--swb` uses `VcBuild.SWB` (:68-69, path select :143).
- Auto-excludes `exclude_xplat` + `require_simd` off-Windows (:176-178),
  `exclude_<arch>`/`exclude_<flavor>` (:154), `exclude_mac` on Darwin (:190-191),
  `require_asmjs` on ARM/ARM64 (:183-184); xplat hard-codes arch to x64 (:114-115);
  default binary `out/<Flavor>/ch` (:146).

`test/CMakeLists.txt` (`ninja check` / `make check`):

- Release: `smoke-check` only — `ch test/Basics/hello.js` (:23-31).
- Debug/RelWithDebInfo: `bytecode-check` (`regenByteCode.py --verify …`, :33-38)
  + `regression-check` (`runtests.py --debug|--test [--not-tag exclude_noicu]
  [--variants=disable_jit] --binary=<build>/ch`, :39-45).
- `native-check` runs `test_native.sh` in `test/native-tests` unless NO_ICU or
  EMBED_ICU (:48-64). When shared, `check` also depends on `ChakraCore` (:66-68).

Windows: `test\runtests.cmd` drives `rl.exe` (usage :37-56, release unsupported
:56, same default variants :330, `-codecoverage`); `test\ci.testone.cmd <arch>
<type>` runs bytecode-verify (:109) → `runtests.py` (:127) → `runnativetests.cmd`
(:144) with failure codes 2/3/4 (:113, :131, :150). `ci.parsetestargs.cmd` notes
ARM tests unsupported.

## 11. CI

`azure-pipelines.yml`: trigger `master` + `release/*` + weekly Monday cron (:6-17).
Jobs: **Style** (`check_{copyright,ascii,eol,tabs}.sh`, :26-38); **CMake** matrix
ubuntu-24.04 × (Debug, NoJit, RelWithDebInfo, Release), ubuntu-22.04 ×
(RelWithDebInfo, Release), macOS-latest × (DebugNoICU static, RelWithDebInfo,
Release with `-DICU_INCLUDE_PATH=…`) — deps install, `cmake -GNinja
-DCMAKE_BUILD_TYPE=… -DCMAKE_CXX_COMPILER=clang++ -DCMAKE_C_COMPILER=clang ..`,
`ninja`, publish `build/bin`, `ninja check` (:44-120); **MSVC** matrix windows-2025
x86/x64 × (debug, test + `--include-slow`, release build-only) plus x86-only
debug-NoJit with `/p:BuildJIT=false` + `-disablejit`, plus windows-2022 x86/x64
release build-only,
via `test\ci.buildone.cmd` + `test\ci.testone.cmd`, tests only when `do_test`
(:127-219); special-build output suffixes (`.NoJIT`/`.SWB`/`.Lite`/`.NoScriptDebug`)
come from `Build/Common.Build.Default.props:71-77`.

`.cirrus.yml`: 3× ARM64 macOS noJit (Debug / RelWithDebInfo / Release): brew ninja
icu4c, `DISABLE_JIT=ON`, `STATIC_LIBRARY=ON` (Debug and Release tasks; the
RelWithDebInfo task omits it), `ninja check` (:6-31).

`.github/workflows/agreement.yml`: CLA-only (`checkout@v2` at :11).

## 12. Troubleshooting and known staleness

- `--sanitize` may silently do nothing (suspected `${CLANG_SANITIZE_SH}`-after-unset
  bug, root CMake :48-51 vs :610-613). Verify `compile_commands.json`
  (`CMAKE_EXPORT_COMPILE_COMMANDS` is ON by default, `build.sh:127`) or `VERBOSE=1`
  output before trusting a sanitize build.
- `--trace` requires `--static` (FATAL at configure time otherwise, root CMake
  :141-156). LTTng (:623-635) has no static-only requirement.
- `--wb-check ONE_FILE` requires `--ninja`.
- `build.sh --arch` help omits `arm64` (accepted anyway); macOS without an ICU path
  silently becomes `--no-icu`; `--target-path` rejects `~/`.
- Stale floors (all load-bearing until proven otherwise): cmake 3.10, clang floor
  3.7 with no ceiling, VS2015/17-era README vs windows-2025/2022 CI, macOS 10.9
  floor, ventura Cirrus image, LLVM 5.0.0 LTO toolchain, ICU 63.2 (2018) + stale
  57.1 help text, `checkout@v2`, `THIRD-PARTY-NOTICES.txt` (34 items) freshness.
- ARM64-Linux is warned unsupported and ARM64 JIT warned non-functional
  off-Windows (root CMake :547-555) — use `--no-jit` there.
- On macOS (case-insensitive FS), do NOT configure in a `build/` directory: it
  collides with the tracked `Build/` folder and pollutes it with CMake artifacts.
  Use `build-xplat/`, `out/` (via `build.sh`), or another distinctly named dir.
- Open/unread: `pal/src/configure.cmake` + `pal/src/CMakeLists.txt` bodies,
  `test/ci.build.init.cmd` + `test/ci.parsetestargs.cmd` bodies,
  `tools/android_toolchain.sh`, `tools/create_package.sh`,
  `tools/XPlatInstall/install_ch.sh`, `tools/icu/configure.py`; Windows SDK pin;
  external link liveness; GitHub archival flag; upstream tags (local `git tag`
  empty).

## 13. Run-verified baseline (2026-10-10)

Machine: macOS ARM64, AppleClang (`/usr/bin/clang`), Homebrew ninja/cmake,
Homebrew ICU 78 (`/opt/homebrew/opt/icu4c`), Python 3.14. Branch
`modernize/baseline` at `97e68c917`. Cirrus RelWithDebInfo recipe in `build-xplat/`:

```sh
cmake -GNinja -DCMAKE_BUILD_TYPE=RelWithDebInfo \
  -DICU_INCLUDE_PATH=/opt/homebrew/opt/icu4c/include -DDISABLE_JIT=ON \
  -DCMAKE_CXX_COMPILER=clang++ -DCMAKE_C_COMPILER=clang ..
ninja            # 546/546 targets, ~4 min; ch + libChakraCore.dylib produced
ninja check      # EXIT=0
```

Results: configure clean (~25s; a few expected PAL feature-test `Failed` lines, all
non-fatal); build clean except benign link warnings (deprecated `-undefined error`
flag; macOS-11.0 target vs ICU dylibs built for 26.0); `ch
test/Basics/hello.js` → `hello world / PASS`; `ninja check` green —
**regression Total: passed 1959, failed 0** (34.57s), bytecode-check passed,
native-check `test-shared-basic : PASS` with 8 static-only tests ignored (expected
for a shared build). Note the tree links Homebrew ICU 78, far newer than the pinned
embed-63.2 — system-ICU forward compatibility held at least this far. This is the
parity target for the Phase 1 `zig build`: same triple, same `ninja check` result.
