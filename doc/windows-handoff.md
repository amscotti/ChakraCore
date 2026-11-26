# ChakraCore Modernization -- Windows x64 (`B6`) `zig build` Bring-Up & Certification Handoff

> Status: **INTEGRATED ON `modernize/baseline` (`1e30ad2af`) -- AWAITING CLEAN NATIVE WINDOWS RE-CERTIFICATION (`B6`)**
> Target / Host Contract: **`x86_64-windows-msvc`**, Zig **`0.17.0`** (`mise.toml` or `zig 0.17.0` on Windows)
> Companion Docs: [README.md](../README.md), [modernization-plan.md](modernization-plan.md),
> [vision-and-roadmap.md](vision-and-roadmap.md), [linux-handoff.md](linux-handoff.md),
> [node-api-receipt.md](node-api-receipt.md), [b5-package.md](b5-package.md), [build.md](build.md) (legacy MSBuild reference)

> [!IMPORTANT]
> **Scope & Honesty Note:** This handoff covers the native `x86_64-windows-msvc` `zig build` front end (`tools/build/windows.zig`, `tools/build/tables_windows.zig`, `tools/windows-gate-tests.zig`, including §31 bring-up fixes, `VS_VERSION_INFO` resource embedding via `zig rc` for `-Serialized`, and `debug-static` CRT/UBSan linking now promoted to `modernize/baseline` `1e30ad2af`), PE32+ metadata/smoke/Node-API gates (`check_metadata_windows.py`, `check_smoke_windows.py`, `check_nodeapi.py`, `gen_nodeapi_exports.py --def`), Windows packaging (`tools/package/*`), and hosted CI ([.github/workflows/zig-windows.yml](../.github/workflows/zig-windows.yml)). Because `modernize/baseline` development runs on macOS ARM64, the Windows graph and unit tests are cross-checked structurally on macOS/Linux, while **native compilation, runtime gates, Node-API matrices, packaging execution, Test262, and benchmarks require execution on a native Windows x64 host or `windows-latest` runner**.

---

## 1. Windows `zig build` CLI Contract (`x86_64-windows-msvc`)

All active Windows x64 builds use `zig build` (`build.zig` delegates automatically to `tools/build/windows.zig` when `builtin.os.tag == .windows`). Legacy `Build\Chakra.Core.sln` / `test\ci.buildone.cmd` ([build.md](build.md) §5) remain historical reference only.

### 1.1 Build Options

| Option | Values (Default) | Effect / Output Prefix |
|---|---|---|
| `-Dprofile` | `test` *(default)*, `debug-static`, `release-shared` | `test` -> `zig-out/` (RelWithDebInfo shared + JIT); `debug-static` -> `out/debug-static/` (Debug static `ChakraCore.lib`, standalone `ch.exe`, `DBG=1` asserts, no `ChakraCore.dll`); `release-shared` -> `out/release-shared/` (Release shared, no WABT) |
| `-Djit` | `true` *(default)*, `false` | `false` sets `-DDISABLE_JIT=1`, omits `lib/Backend` C++ sources and the 2 backend `.asm` files (`LinearScanMdA.asm`, `Thunks.asm`) |
| `-Dicu` | `icu4c` *(default)*, `windows`, `none` | `icu4c`: links `vcpkg` `icu:x64-windows` or `CHAKRA_ICU_DIR` (`HAS_ICU=1`, `INTL_ICU=1`) for full `I1`/`I2` ECMA-402 parity; `windows`: selects `WINDOWS10_ICU=1` (`<icu.h>`); `none`: omits ICU (falls back to `INTL_WINGLOB=1`) |
| `-Dicu-include=<dir>` | optional path | Overrides auto-detected `CHAKRA_ICU_DIR` / `VCPKG_ROOT\installed\x64-windows\include` header directory |
| `-Dicu-libdir=<dir>` | optional path | Overrides auto-detected `CHAKRA_ICU_DIR` / `VCPKG_ROOT\installed\x64-windows\lib` import-library directory |
| `-Dml64=<path>` | optional path | Overrides auto-discovered `ml64.exe` (PATH / `VCToolsInstallDir` / `vswhere.exe`) |
| `-Dwin-sdk-um=<dir>` | optional path | Overrides auto-discovered Windows SDK `um` directory containing `ksamd64.inc` (`WindowsSdkDir` + `WindowsSDKVersion` / `UniversalCRTSdkDir`) |
| `-Dallow-unpinned-regression` | `false` *(default)*, `true` | `true` passes `--allow-unpinned` to `check_regression_windows.py` during first-run `B6` bootstrap before `RECORDED_BASELINE` is pinned |

### 1.2 Supported Steps (16 Named Steps; 17 Targets Including Default `zig build`)

| Step | Scope / Artifacts |
|---|---|
| *(default)* `zig build` | Compiles `606` engine/bin TUs (`582` engine C++ + `1` engine C + `7` MASM `.asm` + `3` dll + `9` ch + `4` gcstress) + `1` Zig (`u8a_codec.zig`) and installs `ChakraCore.dll` / `ChakraCore.lib` (shared profiles; static `ChakraCore.lib` on `debug-static`), `ch.exe`, `GCStress.exe`, and public JsRT headers |
| `libChakraNodeApi` | Compiles `16` `lib/NodeApi` TUs into `libChakraNodeApi.dll` + `libChakraNodeApi.lib` (using `gen_nodeapi_exports.py --def`, `161` exports) and stages `include/nodeapi/*.h` (shared profiles only) |
| `ch-nodeapi` | Builds `ch-nodeapi.exe` (`NAPI_VERSION=10` multi-addon test host), `libChakraNodeApiStub.dll`, and 5 test `.node` addons (shared profiles only) |
| `verify-bytecode` | Runs `verify_bytecode_pure.py` / `regenByteCode.py --verify` against built `ch.exe` (`test` & `debug-static`) |
| `check-regression` | Runs `tools/build/check_regression_windows.py` against `test/runtests.py` (`--x64`, `--test`/`--debug`, `--no-jit`, `--no-icu`, `--allow-unpinned`) and `ch.exe` (`test` & `debug-static`) |
| `check-selftest` | Runs `--self-test` negative controls across all 10 Python check scripts (incl. `check_regression_windows.py`, `check_jit.py`, `check_fulljit.py`, `check_gcstress.py`) + `test_build_tools.py` + Zig unit tests (`windows-gate-tests.zig`, `owned-bytes.zig`, `u8a_codec.zig`) |
| `check-build-tools` | Runs `tools/build/test_build_tools.py` (no full engine build required) |
| `check-smoke` | Runs `tools/build/check_smoke_windows.py <ch.exe\|prefix> [--profile test\|debug-static\|release-shared] [--ch-nodeapi <ch-nodeapi.exe>] [--icu icu4c\|windows\|none]` (`zig build check-smoke` always builds `ch-nodeapi` and passes `--ch-nodeapi=` on shared profiles; standalone invocation auto-detects beside `ch.exe`) |
| `check-metadata` | Runs `tools/build/check_metadata_windows.py <prefix> [--profile test\|debug-static\|release-shared] [--no-jit] [--icu icu4c\|windows\|none]` (pure-Python PE32+ & COFF archive checker: `.profile`, `bin/{ch,GCStress}.exe`, 4 staged headers, `lib/ChakraCore.lib` `!<arch>\n`, shared `bin/ChakraCore.dll` `>=100` exports / `0` `zig_u8a_*` leaks / `--icu` imports, or `debug-static` standalone binaries with no `ChakraCore.dll`) |
| `check-jit` | Runs `tools/build/check_jit.py` (PE32+ AMD64 `ChakraCore.dll`/`ch.exe` validation + `-trace:BackEnd` `BeginBackEnd`/`EndBackEnd` probe + `-mic:1` / `dynapogo` on `test`/`debug-static`, or PE32+ + default-flags runtime on `release-shared`; requires `-Djit=true`) |
| `check-nodeapi` | Verifies `gen_nodeapi_exports.py --check` + pure-Python PE32+ export parser `check_nodeapi.py --dll <libChakraNodeApi.dll>` matches all `161` symbols (`155` `napi_*`/`node_api_*` + `6` host) |
| `check-gcstress` | Runs bounded `GCStress.exe` in-thread and concurrent heap walks |
| `check-wasm` | Runs `tools/build/check_wasm_p1.py` (`383` P1 entries; requires `-Djit=true` and non-release profile) |
| `check-fulljit` | Runs `tools/build/check_fulljit.py` no-profile FullJit suites (`Basics/Array/Function/Regex/es6/es7`, pinned in `tools/build/gate_counts.json` at `650/0` `win32_test` and `669/0` `win32_debug`) |
| `check` | Aggregates `check-selftest`, `verify-bytecode` (non-release), `check-regression` (non-release), `check-smoke`, `check-jit` (when `-Djit=true`), `check-nodeapi` (shared profiles), `check-gcstress`, and `check-metadata` |
| `package` | Builds deterministic `out/package/<profile>/chakracore-<version>-windows-x64.tar.gz` + `MANIFEST.json` (shared profiles only) |
| `check-package` | Builds, SHA-256 verifies (`verify_package.py`), and executes packaged `ch.exe` + `ch-nodeapi.exe` (`test_package.py`) |

---

## 2. Prerequisites & Environment Setup

Install on **Windows 10 / 11 x64** (`x86_64-windows-msvc`):

1. **Visual Studio 2022 Build Tools (or VS 2022 Community+):**
   - Select the **Desktop development with C++** workload and a **Windows 10 or Windows 11 SDK** (provides `ml64.exe` MASM assembler, `ksamd64.inc`, UCRT headers/libs, and Win32 import libraries `kernel32.lib`, `user32.lib`, `advapi32.lib`, `ole32.lib`, `rpcrt4.lib`, `bcrypt.lib`, `oleaut32.lib`, plus Windows inbox `icu.lib` — or `icuuc.lib` + `icuin.lib` if `icu.lib` is absent from SDK `Lib\<ver>\um\x64` — when using `-Dicu=windows`). Because `tools/build/windows.zig` passes `-DENABLE_OOP_NATIVE_CODEGEN=0` (no `midl.exe`) and `-DDISABLE_JS_ETW` (no `mc.exe`), **`ml64.exe` is the sole external MSVC binary required by `zig build`**.
   - Open an **x64 Native Tools Command Prompt for VS 2022** (or initialize MSVC env vars in PowerShell / use `ilammy/msvc-dev-cmd@v1` in CI) so `VCToolsInstallDir`, `WindowsSdkDir`, `WindowsSDKVersion`, and `PATH` (`ml64.exe`) are set.
2. **Zig `0.17.0`:**
   - Install `0.17.0` (matching [mise.toml](../mise.toml)) via `mise` (`mise install`) or the official Windows x64 Zig `0.17.0` archive and place `zig.exe` on `PATH`.
   - Confirm: `zig version` prints `0.17.0`.
3. **ICU4C via `vcpkg` (Recommended Default `-Dicu=icu4c`):**
   - Install `vcpkg` (if not already present) and install x64 Windows ICU:
     ```powershell
     vcpkg install icu:x64-windows
     ```
   - Export `VCPKG_ROOT` (auto-probed at `%VCPKG_ROOT%\installed\x64-windows`) or set `CHAKRA_ICU_DIR` directly:
     ```powershell
     $env:VCPKG_ROOT = "C:\vcpkg"
     $env:CHAKRA_ICU_DIR = "$env:VCPKG_ROOT\installed\x64-windows"
     $env:PATH = "$env:CHAKRA_ICU_DIR\bin;$env:PATH"
     ```
   - **Critical `PATH` note:** `vcpkg` `icu:x64-windows` provides dynamic DLLs (`icuuc*.dll`, `icuin*.dll`, `icudt*.dll`) under `installed\x64-windows\bin`. Keep that `bin` directory on `PATH` (or copy the ICU DLLs next to `zig-out\bin\`) before running `ch.exe`, `GCStress.exe`, `ch-nodeapi.exe`, or any `check-*` step.
4. **Python `3.10+`, Node.js `v22.x`, & Git for Windows:**
   - Python `3.10+` (`python` / `python3` on `PATH`) for `tools/build/*`, `tools/package/*`, `test/runtests.py`, `tools/test262/*`, and `tools/bench/*`.
   - Node.js `v22.x` (`v22.22.3` pinned for Test262 harness and Node-API differential runs).
   - Git for Windows (provides `git` and Git Bash for running `tools/StyleChecks/check_*.sh`).

---

## 3. Turnkey 10-Step Windows x64 `zig build` Runbook

Run every command from the repository root inside an **x64 Native Tools** shell with `VCPKG_ROOT` / `CHAKRA_ICU_DIR` and `$CHAKRA_ICU_DIR\bin` on `PATH`. Stop at the first unexpected failure and capture all logs for `doc/b6-windows-run.md`.

### Step 1 -- Pre-Flight Gate Tests, Build Tools, Packaging Tests, & StyleChecks

```powershell
zig test tools/windows-gate-tests.zig
zig test doc/examples/owned-bytes.zig
zig test lib/ZigCodec/u8a_codec.zig
zig build check-build-tools
python tools/package/test_package_tools.py
python tools/test262/test_edition_policy.py
python tools/test262/refresh_baseline.py --self-test
python tools/build/check_metadata_windows.py --self-test
python tools/build/check_smoke_windows.py --self-test
python tools/build/check_regression_windows.py --self-test
python tools/build/check_jit.py --self-test
python tools/build/check_fulljit.py --self-test
python tools/build/check_gcstress.py --self-test
python tools/build/check_nodeapi.py --self-test
python tools/build/gen_nodeapi_exports.py --check
```

In Git Bash (confirming style & bytecode-header purity):

```sh
python3 tools/StyleChecks/check_style_portable.py --self-test
STYLECHECK_BASE=origin/master bash tools/StyleChecks/check_eol.sh
STYLECHECK_BASE=origin/master bash tools/StyleChecks/check_tabs.sh
STYLECHECK_BASE=origin/master bash tools/StyleChecks/check_ascii.sh
STYLECHECK_BASE=origin/master bash tools/StyleChecks/check_copyright.sh
git diff --exit-code 1bf42d1f6 -- \
  lib/Runtime/Base/JnDirectFields.h \
  lib/Runtime/InternalPropertyList.h \
  lib/Runtime/Library/InJavascript/Intl.js \
  'lib/Runtime/Library/InJavascript/JsBuiltIn*.bc.*.h'
```

Expected: all Zig/Python unit suites and self-tests PASS; `4/4` StyleChecks `NO PROBLEMS DETECTED`; empty bytecode-header diff (vs pre-Phase-4 `1bf42d1f6`).

### Step 2 -- Default `test` Profile Build (`x86_64-windows-msvc`)

```powershell
zig build
```

*(Pass `-Dicu-include=<dir> -Dicu-libdir=<dir>` or `-Dml64=<path> -Dwin-sdk-um=<dir>` if auto-detection misses a custom layout.)*

Expected: produces `zig-out/bin/ChakraCore.dll`, `zig-out/lib/ChakraCore.lib`, `zig-out/bin/ch.exe`, `zig-out/bin/GCStress.exe`, and `zig-out/include/{ChakraCore,ChakraCommon,ChakraDebug,ChakraCoreVersion}.h`.

### Step 3 -- Smoke, PE32+ Metadata, Bytecode Purity, & JIT Checks

```powershell
zig build check-smoke check-metadata verify-bytecode check-jit check-selftest
```

Standalone CLI equivalents (matching `tools/build/windows.zig`):

```powershell
python tools/build/check_metadata_windows.py zig-out --profile test --icu icu4c
python tools/build/check_smoke_windows.py zig-out --profile test --icu icu4c
# Or once ch-nodeapi.exe is built in Step 7:
# python tools/build/check_smoke_windows.py zig-out --profile test --ch-nodeapi zig-out/bin/ch-nodeapi.exe --icu icu4c
```

Expected:
- `check-smoke`: `ch.exe -v`, profile invalid-flag behavior, JS snippet, ES5/ES6/`Iterator.zip`/`Promise.allKeyed`/`Uint8Array.fromBase64`/`Intl.Locale` (skipped on `--icu none`)/`using`, `hello.js` (`release-shared`), and `ch-nodeapi.exe --help` + `--version` (`zig build check-smoke` always builds `ch-nodeapi` and passes `--ch-nodeapi=` on shared profiles; standalone `check_smoke_windows.py` auto-detects `ch-nodeapi.exe` beside `ch.exe` when omitted) all PASS.
- `check-metadata`: pure-Python PE32+ & COFF archive validation confirms `.profile = test`, `bin/ch.exe`, `bin/GCStress.exe`, all 4 staged headers (`include/Chakra{Core,Common,Debug}.h`, `include/ChakraCoreVersion.h`), `lib/ChakraCore.lib` (`!<arch>\n`), and shared `bin/ChakraCore.dll` (`>=100` exports, required JsRT exports, zero leaked `zig_u8a_*` exports, `--icu` import check).
- `verify-bytecode`: exits `0`.
- `check-jit`: PE32+ AMD64 validation + `-trace:BackEnd` (`BeginBackEnd`/`EndBackEnd`) + `-mic:1` + `dynapogo` smoke PASS (or PE32+ validation + default-flags runtime on `release-shared`).

### Step 4 -- Full Default `check` Gate, Regression Pinning, & `I1`/`I2` ECMA-402 Spot-Check

On the first native `B6` run before `RECORDED_BASELINE` in `tools/build/check_regression_windows.py` is pinned, pass `-Dallow-unpinned-regression=true` (or run `python tools/build/check_regression_windows.py test/runtests.py zig-out/bin/ch.exe --allow-unpinned`):

```powershell
zig build -Dallow-unpinned-regression=true check
foreach ($t in @(
  "test/Intl/IntlLocaleTest.js",
  "test/Intl/IntlListRelativeTimeTest.js",
  "test/Intl/IntlDisplayNamesTest.js",
  "test/Intl/IntlSegmenterTest.js",
  "test/Intl/IntlSupportedValuesOfTest.js",
  "test/Intl/IntlDurationFormatTest.js"
)) {
  ./zig-out/bin/ch.exe -args summary -endargs $t
  if ($LASTEXITCODE -ne 0) { throw "Intl test failed: $t" }
}
```

Expected: `check` runs `check-selftest`, `verify-bytecode`, `check-regression`, `check-smoke`, `check-jit`, `check-nodeapi`, `check-gcstress`, and `check-metadata`. Audit every `passed` count against `exclude_windows` tags (§4.6) and confirm `0` failures (`rc == 0`), then pin the printed `RECORDED_BASELINE[(select, variant)] = (passed, 0)` tuples in `tools/build/check_regression_windows.py` and `doc/b6-windows-run.md` so subsequent `zig build check` / `zig build check-regression` invocations enforce strict pins without `-Dallow-unpinned-regression=true`.

### Step 5 -- Build Variants (`-Djit=false`, `debug-static`, `release-shared`, Optional `-Dicu`)

```powershell
zig build -Djit=false check-smoke check-metadata verify-bytecode check-gcstress
zig build -Dprofile=debug-static check-smoke check-metadata verify-bytecode check-gcstress
zig build -Dprofile=debug-static -Dallow-unpinned-regression=true check-regression
zig build -Dprofile=release-shared check-smoke check-metadata check-jit
```

Optional non-default ICU mode spot checks:

```powershell
zig build -Dicu=windows check-smoke
zig build -Dicu=none check-smoke
```

*(Re-run `zig build` afterwards to restore default `zig-out/` with `-Dicu=icu4c` + `-Djit=true`.)*

Expected: `-Djit=false` compiles without `lib/Backend` or backend `.asm` files; `out/debug-static/` produces standalone `ch.exe` and `GCStress.exe` without `ChakraCore.dll` imports, static `lib/ChakraCore.lib` (`!<arch>\n`), no `ChakraCore.dll` in `bin/`/`lib/`, and `DBG=1` assertions clean; `out/release-shared/` passes Release smoke (`hello.js`), metadata, and JIT gates.

### Step 6 -- WASM, FullJit, & Bounded `GCStress` Gates

```powershell
zig build check-wasm check-fulljit check-gcstress
Push-Location test
python runtests.py --x64 --debug --variants=interpreted -b ../out/debug-static/bin/ch.exe wasm
Pop-Location
```

Expected: `check-wasm` (`383/383` P1 or record any x64/Windows manifest deltas), `check-fulljit` `0` failures, `check-gcstress` completes in-thread and concurrent heap walks cleanly, and debug `wasm` triggers zero debug assertions.

### Step 7 -- Node-API v10 Stack (`libChakraNodeApi.dll` + `ch-nodeapi.exe` + `check-nodeapi`)

```powershell
zig build libChakraNodeApi ch-nodeapi check-nodeapi
python test/nodeapi/run_matrix.py --verify-manifest
python test/nodeapi/run_matrix.py --suite all --host ch-nodeapi
```

Also verify multi-addon loading directly:

```powershell
./zig-out/bin/ch-nodeapi.exe --addon ./zig-out/lib/nodeapi-addons/hello_world.node --driver ./zig-out/lib/nodeapi-addons/hello_world.js
```

Expected: `check-nodeapi` parses `libChakraNodeApi.dll`'s PE32+ Export Directory and verifies exact `161` exports (`155` `napi_*`/`node_api_*` + `6` host helpers); `hello_world` and C matrix pass cleanly (record any Windows-specific addon compiler/skip deltas).

### Step 8 -- Deterministic Release Packaging (`package` & `check-package`)

```powershell
zig build -Dprofile=release-shared libChakraNodeApi ch-nodeapi check-nodeapi package check-package
```

*(Packaging invokes `python tools/package/build_package.py --prefix out/release-shared --out-dir out/package/release-shared --target windows-x64 --profile release-shared --icu-mode auto`.)*

Expected: produces `out/package/release-shared/chakracore-<version>-windows-x64.tar.gz` and `MANIFEST.json` staging `bin/{ch,ch-nodeapi}.exe`, copied `bin/{ChakraCore,libChakraNodeApi}.dll` (no symlinks), `lib/{ChakraCore,libChakraNodeApi}.lib`, ICU runtime DLLs when present, `include/{ChakraCore,ChakraCommon}.h`, `include/nodeapi/*.h`, and notices, with `VERIFY PASS` (`0` errors) and `ALL PACKAGE TESTS PASSED SUCCESSFULLY!` (tested cleanly without vcpkg `PATH` injection).

### Step 9 -- Certified Windows x64 Test262 & Benchmarks

*(First-time setup on a fresh clone: install the pinned harness with `npm --prefix tools/test262 ci --ignore-scripts` and prepare the pinned Test262 checkout with `python tools/test262/run.py --binary "$PWD\zig-out\bin\ch.exe" --prepare --controls-only`.)*

```powershell
python tools/test262/refresh_baseline.py --binary "$PWD\zig-out\bin\ch.exe" --jobs 8 --output out/test262-refresh-phase4-win64
python tools/test262/run.py --checkout "$PWD\out\test262" --binary "$PWD\zig-out\bin\ch.exe" --intl402 --jobs 8 --output out/test262-intl402-phase4-win64 -- "test/intl402/**/*.js"
python tools/bench/run.py --binary zig-out/bin/ch.exe --tier jit --iterations 5 -o tools/bench/results/windows-x64-jit.json
python tools/bench/run.py --binary zig-out/bin/ch.exe --tier interp --iterations 5 -o tools/bench/results/windows-x64-interp.json
```

Expected: compare edition/broad/buckets against macOS ARM64 (`99.57%` edition = `80,195 / 349`, `88.97%` broad = `82,866 / 10,275`, `--intl402` `1,886 / 6,764`) and Linux x64; verify `0` shape suspects and triage any Windows-specific ICU, line-ending, or thread-wait deltas before publishing (`--publish` on a clean tree). *(Note: `tools/test262/run.py`'s `shared_library_paths` globs `libChakraCore.{dylib,so}`, so record `zig-out/bin/ChakraCore.dll` SHA-256 explicitly in `doc/b6-windows-run.md` alongside `summary.json`'s `binary_sha256`.)*

### Step 10 -- Hosted CI Confirmation & Capture Receipt in `doc/b6-windows-run.md`

Confirm [.github/workflows/zig-windows.yml](../.github/workflows/zig-windows.yml) passes on `windows-latest`, then create `doc/b6-windows-run.md` using the receipt template below.

```markdown
# B6: Windows x64 `zig build` Certification Receipt

- Date: YYYY-MM-DD
- Commit: `<full 40-char git rev-parse HEAD>` (`modernize/baseline`)
- Host OS: Windows <10|11> x64 (`<build number>`)
- Toolchain: Zig `0.17.0`, VS 2022 `<version>`, Windows SDK `<version>`, `ml64.exe <version>`
- ICU: `vcpkg icu:x64-windows` (`<icu version>`)
- Node / Python: Node `<v22.x>`, Python `<3.x>`
- Artifacts SHA-256:
  - `zig-out/bin/ChakraCore.dll`: `<sha256>`
  - `zig-out/bin/ch.exe`: `<sha256>`
  - `zig-out/bin/libChakraNodeApi.dll`: `<sha256>`
  - `out/package/release-shared/chakracore-<ver>-windows-x64.tar.gz`: `<sha256>` (`<bytes>` bytes)

## Gate Matrix

| Step | Scope / Command | Result | Notes |
|---|---|---|---|
| 1 | `windows-gate-tests.zig`, `check-build-tools`, `test_package_tools.py`, StyleChecks | PASS/FAIL | |
| 2-4 | `zig build`, `check-smoke/metadata/jit`, `verify-bytecode`, `zig build check`, `I1`/`I2` Intl | PASS/FAIL | |
| 5 | `-Djit=false`, `debug-static`, `release-shared`, `-Dicu=windows|none` | PASS/FAIL | |
| 6 | `check-wasm`, `check-fulljit`, `check-gcstress`, debug `wasm` | PASS/FAIL | |
| 7 | `libChakraNodeApi`, `ch-nodeapi`, `check-nodeapi` (`161`), Node-API matrix | PASS/FAIL | |
| 8 | `package`, `check-package` (`VERIFY PASS`) | PASS/FAIL | |
| 9 | Test262 (`refresh_baseline.py` + `--intl402`) & benchmarks (`windows-x64-{jit,interp}.json`) | PASS/FAIL | |
| 10 | Hosted `.github/workflows/zig-windows.yml` run URL | PASS/FAIL | |
```

---

## 4. Troubleshooting & First-Run Watchpoints

### 4.1 `ml64.exe` and `ksamd64.inc` Overrides (`-Dml64`, `-Dwin-sdk-um`)
The Windows x64 build assembles 5 core MASM files (`amd64_SAVE_REGISTERS.asm`, `JavascriptConversionA.asm`, `JavascriptOperatorsA.asm`, `amd64_Thunks.asm`, `JavascriptFunctionA.asm`) plus 2 JIT backend files (`LinearScanMdA.asm`, `Thunks.asm`) when `-Djit=true`. All 7 contain `include ksamd64.inc`.
- Normally `VCToolsInstallDir` / `vswhere.exe` resolves `Hostx64\x64\ml64.exe` and `WindowsSdkDir` + `WindowsSDKVersion` resolves `...\Include\<ver>\um\ksamd64.inc`.
- If running outside an x64 Native Tools prompt or when multiple SDK versions/host architectures exist, pass explicit paths:
  ```powershell
  zig build `
    -Dml64="C:\Program Files\Microsoft Visual Studio\2022\BuildTools\VC\Tools\MSVC\<ver>\bin\Hostx64\x64\ml64.exe" `
    -Dwin-sdk-um="C:\Program Files (x86)\Windows Kits\10\Include\<ver>\um"
  ```

### 4.2 ICU4C DLLs on `PATH` (`0xc0000135` / `STATUS_DLL_NOT_FOUND`)
`vcpkg install icu:x64-windows` builds dynamic `icuuc*.dll`, `icuin*.dll`, and `icudt*.dll` in `%VCPKG_ROOT%\installed\x64-windows\bin`. Because Windows does not support ELF `$ORIGIN` RPATHs, running `zig-out/bin/ch.exe` or `zig build check-*` fails with `STATUS_DLL_NOT_FOUND` (`0xc0000135`) unless `%CHAKRA_ICU_DIR%\bin` (`%VCPKG_ROOT%\installed\x64-windows\bin`) is on `PATH` or the ICU DLLs are copied next to the executable.

### 4.3 `-DENABLE_OOP_NATIVE_CODEGEN=0` & `-DDISABLE_JS_ETW` (`ml64.exe`-Only Build)
Legacy MSBuild (`Build\Chakra.Core.sln`) defaults `ENABLE_OOP_NATIVE_CODEGEN` to `1` (invoking `midl.exe` over `JITIDL` and linking `JITClient`/`JITServer` COM/RPC projects) and defines `ENABLE_JS_ETW` (requiring `mc.exe` / `CoreManifests`). On `modernize/baseline`:
- `lib/Common/CommonDefines.h` wraps `#define ENABLE_OOP_NATIVE_CODEGEN 1` in `#ifndef ENABLE_OOP_NATIVE_CODEGEN` and guards `ENABLE_JS_ETW` with `#if !defined(CHAKRACORE_LITE) && !defined(DISABLE_JS_ETW)`.
- `tools/build/windows.zig` passes both `-DENABLE_OOP_NATIVE_CODEGEN=0` (clean in-process JIT without `midl.exe` or `JITIDL`/`JITClient`/`JITServer`) and `-DDISABLE_JS_ETW` (no `mc.exe` / `CoreManifests`), leaving **`ml64.exe`** as the sole external MSVC binary required by `zig build`, while keeping legacy MSBuild's `1` / ETW defaults `100%` intact.
- `bin/ChakraCore/ChakraCore.def` guards `JsInitializeJITServer` and `lib/Jsrt/JsrtCommonExports.inc` guards `JsConnectJITProcess` and `JsEnableOOPJIT` with `#if !defined(ENABLE_OOP_NATIVE_CODEGEN) || ENABLE_OOP_NATIVE_CODEGEN` so `.def` preprocessing omits them from `ChakraCore.dll` exports when `ENABLE_OOP_NATIVE_CODEGEN == 0`; `bin/ch/ChakraRtInterface.cpp` (`GetChakraCoreSymbol`) and `bin/ch/ch.cpp` resolve `JsConnectJITProcess` / `JsInitializeJITServer` dynamically via `GetProcAddress` (never statically imported from `ChakraCore.dll`), while `lib/Jsrt/Core/JsrtCore.cpp` defines `JsEnableOOPJIT` / `JsConnectJITProcess` on `_WIN32` returning `JsErrorNotImplemented` when `ENABLE_OOP_NATIVE_CODEGEN == 0`.

### 4.4 `-Dicu=icu4c` vs `-Dicu=windows` vs `-Dicu=none` Trade-Offs
- **`-Dicu=icu4c` (Default):** Defines `HAS_ICU=1` and `INTL_ICU=1` against `vcpkg` `icu:x64-windows` (`unicode/*.h`). Gives full `I1`/`I2` ECMA-402 C-API parity (`Intl.Locale`, `ListFormat`, `RelativeTimeFormat`, `DisplayNames`, `Segmenter`, `supportedValuesOf`, `DurationFormat`) and modern ICU Unicode property escapes, at the cost of deploying `vcpkg` ICU DLLs.
- **`-Dicu=windows`:** Defines `HAS_ICU=1`, `INTL_ICU=1`, `WINDOWS10_ICU=1` (`lib/Runtime/PlatformAgnostic/ChakraICU.h` includes `<icu.h>` with `NTDDI_WIN10_RS5` and `tools/build/windows.zig` links `-licu` when `icu.lib` is present in SDK `Lib\<ver>\um\x64`, or `-licuuc -licuin` if `icu.lib` is absent). Avoids third-party ICU DLLs, **but** Windows inbox `<icu.h>` targets an older Redstone C API subset and OS-bundled CLDR/Unicode data that may lack newer ICU4C 74+ `I1`/`I2` APIs or match older Unicode tables depending on Windows OS version.
- **`-Dicu=none`:** Omits `HAS_ICU` and `INTL_ICU`, so `lib/Common/CommonDefines.h` defines `INTL_WINGLOB=1` (`Windows.Globalization`). Legacy WinGlob `Intl` lacks `I1`/`I2` modern `INTL_ICU` constructors and ICU-backed Unicode `RegExp` property escapes.

### 4.5 `ChakraCore.def`, `libChakraNodeApi.dll`, & Windows `.node` Addons
- `ChakraCore.dll` expands `bin/ChakraCore/ChakraCore.def` (which `#include`s `lib/Jsrt/JsrtCommonExports.inc`) via the C preprocessor with `-Ilib/Jsrt` and `-DENABLE_OOP_NATIVE_CODEGEN=0` before passing `/DEF:...` to `lld-link`.
- `libChakraNodeApi.dll` exports all `161` symbols (`155` `napi_*`/`node_api_*` + `6` `chakra_nodeapi_*` host functions) via `CHAKRA_NODEAPI_EXPORT` (`__declspec(dllexport)`) and a generated `.def` file from `python tools/build/gen_nodeapi_exports.py --def <path>` (`LIBRARY libChakraNodeApi\nEXPORTS\n...`), while `libChakraNodeApiStub.dll` links `ChakraCore.lib` without `libChakraNodeApi.def` and exports only its minimal stub subset + `STUB_HOST_EXPORT` host helpers. `python tools/build/check_nodeapi.py --dll <libChakraNodeApi.dll>` inspects the PE32+ Export Directory directly in Python without needing external `dumpbin` or `nm`.
- On `_WIN32`, `ch-nodeapi.exe` (`tools/nodeapi/ch-nodeapi.cpp`) links only `ChakraCore.lib` and resolves `libChakraNodeApi.dll` / `libChakraNodeApiStub.dll` (`napi_open_handle_scope`/`napi_close_handle_scope`/`napi_create_object` via `LoadLibrary`/`GetProcAddress`, probing beside `ch-nodeapi.exe`, `..\lib\`, and `..\bin\`) so `--provider` stub overrides work cleanly, while `.node` addons compile with `-DBUILDING_NODE_EXTENSION` and link `lib/libChakraNodeApi.lib`.

### 4.6 `test/runtests.py` Watchpoints on Windows
- **Always pass `--x64`:** In `test/runtests.py`, on `sys.platform == 'win32'`, omitting `--x64` falls back to `os.environ.get('_BuildArch', 'x86')`, selecting `x86` exclusion tags instead of `x64`/`amd64`! (`tools/build/check_regression_windows.py` always passes `--x64`, `--test`/`--debug`, and `--binary=<abs_ch>` automatically.) Always run `python runtests.py --x64 --test -b ...` (or `--debug`) when invoking manually.
- **Always pass `-b` / `--binary`:** Without `-b`, `runtests.py` defaults to legacy `Build\VcBuild\bin\x64_Test\ch.exe` rather than `zig-out\bin\ch.exe` or `out\debug-static\bin\ch.exe`.
- **`exclude_windows` tags in `test/Intl/rlexe.xml`:** Legacy `IntlPlatform.js`, `PluralRules.js`, and the 6 `I1`/`I2` `Intl*.js` suites currently carry `exclude_windows` (written for legacy `INTL_WINGLOB`), so `runtests.py` skips them on Windows. Run the 6 `I1`/`I2` suites explicitly under `-Dicu=icu4c` as shown in Step 4.
- **Line endings & stdout:** `test/runtests.py` and `tools/build/check_fulljit.py` normalize CRLF when comparing `.baseline` files, and require `ch.exe` on an absolute `-b` path because `runtests.py` changes `cwd` into each suite subdirectory.
