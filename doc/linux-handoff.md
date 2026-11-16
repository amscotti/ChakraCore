# ChakraCore Modernization -- Linux Bring-Up and Testing Handoff

> Date: 2026-11-16
> Branch / Commit: `modernize/baseline` at `526393e26` (`8f4e85d62..526393e26` integrated)
> Companion Docs: [architecture-and-readiness-review.md](architecture-and-readiness-review.md),
> [b4-d1-d4.md](b4-d1-d4.md), [build-contract-linux.md](build-contract-linux.md),
> [open-cards.md](open-cards.md), [perf-baseline.md](perf-baseline.md)

This document records the exact closure state reached on macOS ARM64 (`darwin-arm64`)
and specifies everything that must be built, verified, re-pinned, stressed, and
monitored when continuing the project on Linux (`x86_64-linux-gnu` first, with notes
for future `aarch64-linux-gnu`).

---

## 1. Handoff Baseline: What Landed and Passed on macOS (`8f4e85d62..526393e26`)

All Mac ARM64 build profiles, conformance gates, StyleChecks, Node-API matrices,
certified Test262 baselines, and benchmark suites were verified after the code
changes on `modernize/baseline`:

### 1.1 Mac Verification Gates (`c24266598` / `4b2077687` / `526393e26`)
- **`test` profile (`zig-out/`):** `mise exec -- zig build check libChakraNodeApi ch-nodeapi check-nodeapi`
  -- **PASS** (`check_regression` **2064 passed / 0 failed** in `tools/build/gate_counts.json`,
  `check-native` `test-shared-basic` incl. Phase-3 JsRT tests, `check-smoke`,
  `check-metadata`, `verify-bytecode`, `check-nodeapi` 150 symbols, `check-selftest`).
- **`debug-static` profile (`out/debug-static/`):** `mise exec -- zig build -Dprofile=debug-static check`
  -- **PASS** (`check_regression` **2116 passed / 0 failed**, `check-native` **8/8**
  static suites incl. `test-static-external`, `check-metadata` 526 archive members).
- **`release-shared` profile (`out/release-shared/`):** `mise exec -- zig build -Dprofile=release-shared check ch-nodeapi`
  -- **PASS** (smoke + `hello.js`, `test-shared-basic`, metadata, `check-selftest`, `ch-nodeapi`).
- **`jit` + `wasm` profile (`out/jit/`, `-Dprofile=jit -Dwasm=true`):**
  `mise exec -- zig build -Dprofile=jit -Dwasm=true check check-wasm libChakraNodeApi ch-nodeapi check-nodeapi`
  -- **PASS** (`check-jit` PASS, `check-wasm` **382/382 P1 PASS** and `438` passed / `15`
  non-P1 failed of `483` manifest entries in `test/wasm-arm64-manifest.json`,
  `verify-bytecode`, `check-nodeapi` 150 symbols, bounded 6s `GCStress` smoke on
  both `zig-out/bin/GCStress` and `out/jit/bin/GCStress`).
- **Node-API (`NAPI_VERSION=8`, `CC_GATE_LOCK=""`):**
  - `tools/nodeapi/run_probe.sh` + all **18/18** `test/nodeapi/probes/run_*_probe.sh`
    (`test` + `jit` profiles): **ALL PASS** (`BINARY_PROBE` 308/308, `run_host_async_probe` 4/4).
  - `SKIP_BUILD=1 REPEATS=5 sh test/nodeapi/stress/run_stress.sh`: **ALL PASS** (`test` + `jit`).
  - `python3 test/nodeapi/run_matrix.py --suite all --host ch-nodeapi`: **46 pass / 23 skip / 0 fail**.
  - `python3 test/nodeapi/run_node_addon_api_matrix.py --hosts both`: **52 pass / 61 skip / 6 fail**
    (`node-direct` 29/0; `ch-nodeapi` 23/6 with only the 6 documented PR-12 `test/object/*`
    V8 exact error-message string checks).
- **Tooling & incremental proofs:**
  `test_build_tools.py` **81/81 OK**, `zig test tools/linux-gate-tests.zig` **7/7 PASS**,
  `zig test doc/examples/owned-bytes.zig` **4/4 PASS**, `test_incremental.py --quick` **0 FAIL**.
- **Certified ES2025 Test262 (`refresh-2026-11-16`, `zig-out/bin/ch`):**
  - Edition-selected (`80,544` scenarios): **78,731 passed / 1,813 failed (97.75%)**,
    **+1,136 passed** (`+1.41` pp, 0 feature regressions) over `refresh-2026-11-05` (`77,595 / 2,949`).
  - Broad (`93,141` scenarios): **79,569 passed / 13,572 failed (85.43%)**.
  - Recorded in [tools/test262/baseline-register.json](../tools/test262/baseline-register.json).
- **Quiet-machine Benchmarks (`526393e26`, `out/jit/bin/ch`, `--iterations 5`):**
  - `head-jit-darwin-arm64.json`: **64/64** applicable pass (`Octane/zlib.js` excluded,
    `0` unexpected failures, `333.5s` total).
  - `head-interp-darwin-arm64.json`: **63/64** applicable pass (`Octane/zlib.js` excluded,
    `JetStream/gcc-loops.cpp.js` `180s` timeout matching Nov 11, `0` crashes, `2889.3s` total).
  - `JetStream/cdjs.js`: **5/5 PASS** on both tiers (`202.0 +/- 2.0` JIT, `22.5 +/- 0.1` Interp).
  - Suite geomeans (JIT / Interp / Speedup vs Nov 11): SunSpider `6.59 ms / 28.85 ms` (`4.38x`, `+1.2% / +3.0%`),
    Kraken `207.79 ms / 980.93 ms` (`4.72x`, `+2.0% / +3.6%`),
    Octane `22786.59 / 1550.23` (`14.70x`, `+1.6% / +1.4%`),
    JetStream matched `n=9` `304.23 / 8.53` (`35.65x`, `+21.6% / +34.9%`).
- **Portable StyleChecks vs `origin/master` (`97e68c917`):**
  **5/5 PASS** (`check_style_portable.py --self-test`, `check_eol.sh`, `check_copyright.sh`,
  `check_tabs.sh`, `check_ascii.sh`).

### 1.2 Linux-Relevant Changes Landed During Mac Closure
The following commits on `modernize/baseline` modified Linux, shared C++, host, or CI files
that could not be executed natively on macOS and therefore require Linux validation:

1. **Build parameterization & CI (`9d8d1817a`):**
   - [tools/build/linux.zig](../tools/build/linux.zig) parameterized `-Dglibc` (default `2.39`),
     `-Dicu-major` (default `74`), `-Djit=true|false`, `-Dprofile=test|debug-static|release-shared`,
     and `--icu-major` in [tools/build/check_metadata_linux.py](../tools/build/check_metadata_linux.py).
   - Added [.github/workflows/zig-linux.yml](../.github/workflows/zig-linux.yml) covering `style`,
     `zig` (`test` full check), `zig-variants` (`-Djit=false`, `release-shared`, `debug-static`),
     and `nodeapi`.
   - Replaced macOS-broken BSD `grep -P` in `tools/StyleChecks/check_{ascii,tabs}.sh` via
     `tools/StyleChecks/check_style_portable.py`.
2. **ELF vs Mach-O unwinding & memory alignment (`35152953b`, `20ea96941`, `4b2077687`):**
   - [lib/Common/PlatformAgnostic/AssemblyCommon.h](../lib/Common/PlatformAgnostic/AssemblyCommon.h)
     and [lib/Common/Memory/arm/XDataAllocator.cpp](../lib/Common/Memory/arm/XDataAllocator.cpp) /
     [arm64/XDataAllocator.cpp](../lib/Common/Memory/arm64/XDataAllocator.cpp):
     separated Darwin Mach-O (`__APPLE__`) from ELF (`!__APPLE__ && __ELF__`) `CHAKRA_UNWINDER_SINGLE_FDE`
     CIE-to-FDE scanning and gated non-Apple ARM64 `.xdata` registration under `CHAKRA_ARM64_DWARF_EH_FRAME`.
   - [lib/Common/Memory/Recycler.inl](../lib/Common/Memory/Recycler.inl): shifted `(rawInteriorAddress - baseAddress)`
     before narrowing to avoid 16-bit wrap on 128 KiB medium blocks under 16 KiB pages.
   - [lib/Backend/Inline.cpp](../lib/Backend/Inline.cpp) (`:1517`, `:4104`): switched `#ifdef ASMJS_PLAT`
     to `#ifdef ENABLE_ASMJS`.
   - Cross-runtime Growable SAB (`SharedContents::csGrow`, atomic `bufferLength`, `tracksSharedLength`)
     and non-writable `lastIndex` / `Float16Array` JIT guards.
3. **JsRT, `bin/ch`, and Node-API (`50570a36e`, `98c81ec88`, `69df54b1d`, `29362397a`, `83ab4d28e`):**
   - Single-owner external ArrayBuffer detach finalizer transfer (`JsrtExternalArrayBuffer.cpp`),
     `JsArrayTypeBigInt64`/`JsArrayTypeBigUint64` ABI (`Jsrt.cpp`, `ChakraCommon.h`),
     and 4 new native test suites (`test/native-tests/jsrt_phase3_tests.h`).
   - `bin/ch` `StderrLock`, atomic `sourceContext` / negative-phase markers, per-child `broadcastQ`.
   - Node-API `extern "C"` OOM guards, ref-counted `PromiseQueue*` reclamation (`A8-3`),
     `host_manages_promise_queue`, direct `BigInt64`/`BigUint64` wiring (`NodeApiBinary.cpp`),
     and `ch-nodeapi` worker pool (`post_task`/`post_work`, `--pump-timeout-ms`), plus optional `CC_GATE_LOCK`.

---

## 2. Linux Host Prerequisites and Build Overrides

### Default Environment Contract (Ubuntu 24.04 x86_64)
- **Target:** `x86_64-linux-gnu` (`glibc 2.39`, `libicu*.so.74`, Clang/LLD via Zig `0.17.0`).
- **Required packages (Ubuntu/Debian):**
  - `libicu-dev`, `binutils` (`nm`, `readelf`), `clang`, `make`, `python3`, `git`, plus `zig 0.17.0`
    and `node v22.22.3` via `mise` (see `mise.toml` and `tools/docker/Dockerfile.ubuntu-x64`).
  - Verify headers/libs exist at default paths (`/usr/include/unicode/uchar.h`,
    `/usr/lib/x86_64-linux-gnu/libicuuc.so`).

### Parameterization Flags on Non-Ubuntu-24.04 Hosts
If your Linux machine is not stock Ubuntu 24.04 (`glibc 2.39`, `ICU 74`), pass the corresponding
override flags to `mise exec -- zig build`:

- **`-Dglibc=<M.m>`:** Must match the trailing bare version token of `ldd --version`
  (e.g., `-Dglibc=2.35`, `-Dglibc=2.40`). Setting a custom value emits target triple
  `x86_64-linux-gnu.<M.m>`.
- **`-Dicu-major=<N>`:** Must match `U_ICU_VERSION_SHORT` in `<icu-include>/unicode/uvernum.h`
  (e.g., `-Dicu-major=76`). When invoking `tools/build/check_metadata_linux.py` standalone, also
  pass `--icu-major <N>`.
- **`-Dicu-include=<dir>` and `-Dicu-libdir=<dir>`:** Override `/usr/include` and
  `/usr/lib/x86_64-linux-gnu` (for example on Fedora/Arch/multiarch hosts using `/usr/lib64`).
- **`-Dprofile=test|debug-static|release-shared`:**
  - `test` (default): RelWithDebInfo shared (`zig-out/`), JIT + WABT/WASM enabled by default unless `-Djit=false`.
  - `debug-static`: Requires `-Doptimize=debug`; static archive + `TestHooks.cpp` + UBSan disabled (`out/debug-static/`).
  - `release-shared`: Fast Release shared build without WABT (`out/release-shared/`).
- **`-Djit=true|false`:** Pass `-Djit=false` to build the Linux interpreter-only shared variant.

*(Note: `test/nodeapi/run_matrix.py` and `run_node_addon_api_matrix.py` automatically resolve `node` and
`zig` from env vars, `PATH`, or `mise which`, and all probe/stress runners now accept unset `CC_GATE_LOCK`.)*

---

## 3. What Needs to Be Tested on Linux (Ordered Verification Checklist)

### Phase 1 -- Pre-Flight Tooling and Portable StyleChecks
Run first before compiling engine objects:

```sh
git fetch origin master:refs/remotes/origin/master  # if origin/master ref is missing locally
python3 tools/StyleChecks/check_style_portable.py --self-test
bash tools/StyleChecks/check_ascii.sh
bash tools/StyleChecks/check_tabs.sh
bash tools/StyleChecks/check_eol.sh
bash tools/StyleChecks/check_copyright.sh
python3 tools/build/test_build_tools.py
python3 tools/build/check_metadata_linux.py --self-test
mise exec -- zig test tools/linux-gate-tests.zig
mise exec -- zig test doc/examples/owned-bytes.zig
```

Expected: all StyleChecks exit `0`, `test_build_tools.py` runs `81/81 OK`, `linux-gate-tests.zig`
passes `7/7`, and `owned-bytes.zig` passes `4/4`.

### Phase 2 -- Default Linux `test` Profile, Regression Re-Pin, and B4 D1-D4 Handoff
1. **Build and non-regression gates:**
   ```sh
   mise exec -- zig build
   mise exec -- zig build check-selftest check-smoke check-jit check-native check-metadata verify-bytecode
   ```
   Verify:
   - `check_metadata_linux: PASS` (237 exported `.so` symbols, `669` static members on baseline pin,
     4 byte-identical JsRT headers in `zig-out/include/`, no RUNPATH/RPATH leaks, NX non-exec stack).
   - `check_native`: `test-shared-basic : PASS` including the 4 new C++11 Phase-3 JsRT suites in
     `test/native-tests/jsrt_phase3_tests.h` (`TestExternalArrayBufferLifetimes`, `TestTypedArrayAbi`,
     `TestTimerContinuationCallback`, `TestFetchImportedModuleFromScriptWithAttributes`).
   - *(Optional legacy parity check)*: If validating against CMake on Linux, run a CMake Ninja build once
     and diff `nm -D` exports against `tools/build/linux_exports_so.txt` (237 symbols) and
     `tools/build/linux_exports_ch.txt` (which currently lacks a fresh native diff receipt). Note `CMakeLists.txt`
     does not build `lib/NodeApi` or `tools/nodeapi/ch-nodeapi`.

2. **Re-pin `tools/build/check_regression_linux.py` (`RECORDED_BASELINE`):**
   - `tools/build/check_regression_linux.py:61` still holds the Nov 9 pre-D1-D4 counts:
     `RECORDED_BASELINE = {"interpreted": (2841, 0), "dynapogo": (2305, 0)}`.
   - Since Nov 9, B4 D1-D4 (`2845 / 2307` expected in `doc/b4-d1-d4.md` Section 5), W7-W9 WASM tests,
     A1-A12/JITREG tests, and Stage 2-4 (`+5` tests: `float16-jit-bounds.js`,
     `regex-nonwritable-lastindex-jit.js`, `s6-growable-cross-agent.js`,
     `cdjs-mediumblock-interior-gc.js`, `asmjs-inline-const.js`) were added.
   - Run `mise exec -- zig build check-regression`: verify **`0` failures** in both `interpreted`
     and `dynapogo`, audit the exact `passed` delta against added `rlexe.xml` tests, update
     `RECORDED_BASELINE` + docstring in `tools/build/check_regression_linux.py`, update `test_build_tools.py`
     if any count tests assert `2841`/`2305`, and run full `mise exec -- zig build check` to confirm exit `0`.

3. **Execute B4 D1-D4 & ELF Unwind Handoff (`doc/b4-d1-d4.md` Section 5):**
   - **ELF `.eh_frame` / `ChakraEhFrameFdeStart` (`AssemblyCommon.h`):** Confirm our `!__APPLE__ && __ELF__`
     guard splits cleanly from Mach-O on native x64 Linux:
     ```sh
     (cd test && python3 runtests.py --x64 -t -b ../zig-out/bin/ch TryCatch StackTrace)
     ./zig-out/bin/ch -mic:1 -bgjit- test/StackTrace/LongCallStackThrow.js
     ./zig-out/bin/ch -forceNative -off:simpleJit -bgjit- test/TryCatch/TryCatchStackOverflow.js
     ```
   - **D1 debugger attach/detach loop (no Linux hang):** Run `AsyncSuspendedResume.js` and
     `AsyncSuspendedDetach.js` (`test/DebuggerCommon/rlexe.xml:1376-1391`, where `exclude_linux` was
     removed in D1) **20-30x each** in the interpreted variant to prove the `WScript.Attach`/`Detach`
     loop exits cleanly without hanging on Linux.
   - **D2-D4 exact Number formatting, SysV A3 arg order, and WASM deferred validation:** Verify
     `test/es7/` exact formatting tests, `test/wasm/deferred-invalid-body.js`, `WasmSpec/jsapi`, and an
     imported-table/`call_indirect` module, plus `test262` `built-ins/Number` (`276/276` expected).

### Phase 3 -- Untested Linux Build Variants (`debug-static`, `release-shared`, `-Djit=false`)
These variants were wired in `tools/build/linux.zig` on macOS and have **never been compiled or executed on Linux**:

```sh
# 1. No-JIT shared build
mise exec -- zig build -Djit=false check-smoke check-metadata verify-bytecode check-native

# 2. Release shared build
mise exec -- zig build -Dprofile=release-shared check-smoke check-metadata check-jit check-native

# 3. Debug static build (exercises DBG asserts + all 8 static native suites on ELF)
mise exec -- zig build -Dprofile=debug-static -Doptimize=debug check-smoke check-metadata verify-bytecode check-native

# 4. Debug WASM deferred-body asserts (B4 D4 handoff)
(cd test && python3 runtests.py --x64 --debug --variants=interpreted -b ../out/debug-static/bin/ch wasm)
```

Verify:
- Every variant compiles, passes `install_profile.py` staging without colliding with `zig-out/`,
  and passes `check_metadata_linux.py` (`--no-jit` for `-Djit=false`).
- `debug-static` passes all **8 static native suites** (`test-static-external` incl. detached external
  ArrayBuffer finalizers + `JsArrayTypeBigInt64`/`BigUint64`/`Float16` + timers + module attributes).
- Debug `wasm` suite triggers zero `SetFirstTmpReg` / `SetExitLabel` debug assertions.

### Phase 4 -- Node-API Stack on Linux (`libChakraNodeApi.so` & `ch-nodeapi`)
Node-API has **zero prior Linux runs** (`libChakraNodeApi.so` was only wired blindly before this branch
cleanup, and Stage 2/3 added `ch-nodeapi` async host services + direct `BigInt64` wiring):

```sh
mise exec -- zig build libChakraNodeApi ch-nodeapi check-nodeapi
```
Then run all probes, stress, and JS matrices:
```sh
export SKIP_BUILD=1
bash tools/nodeapi/run_probe.sh
for s in test/nodeapi/probes/run_*.sh; do
  echo "=== $s ==="
  bash "$s" || exit 1
done
REPEATS=5 sh test/nodeapi/stress/run_stress.sh
python3 test/nodeapi/run_matrix.py --verify-manifest
python3 test/nodeapi/run_matrix.py --suite all --host ch-nodeapi
python3 test/nodeapi/run_node_addon_api_matrix.py --hosts both
```

What to watch on Linux:
- **`check-nodeapi` (`tools/build/check_nodeapi.py:103-120`):** On Linux ELF, `exported_t_symbols` runs
  `nm -g --defined-only`. Verify all 150 provider+host symbols export cleanly and that any bundled
  static-libc++ replaceable `operator new/delete` symbols still match `RUNTIME_T_RE` without unexpected
  C++ internal leakage or missing visibility.
- **`ch-nodeapi` worker pool & `dlclose` (`run_host_async_probe.sh` Case 3 & 4):** Verify detached worker
  threads on `--pump-timeout-ms` expiration and `tsfn_teardown` behave cleanly under `glibc`/`libpthread`
  process teardown.
- **Matrix targets:** Expect `46 pass / 23 skip / 0 fail` on `run_matrix.py` and `0` failures outside
  the 6 known PR-12 `test/object/*` V8 error-text diffs on `run_node_addon_api_matrix.py`.

### Phase 5 -- Linux Sanitizers (ASan / UBSan / TSan) & Stress Campaigns
No sanitizer lane has run on macOS (`debug-static` currently passes `-fno-sanitize=undefined`).
Once basic Linux builds are green, prioritize Linux sanitizers in this order:

1. **ASan + LSan (`libChakraNodeApi.so`, `ch-nodeapi`, `check-native`):**
   - Run all 18 Node-API probes (especially `binary_probe`, `buffers_probe`, `teardown_probe`,
     `promise_script_probe`, `env_addr_reuse_probe`, `host_async_probe`) and the 8 static native suites
     to confirm zero leaks or use-after-free across provider/JsRT/host teardown.
2. **TSan (`ch-nodeapi` async loop, S6e `Atomics.waitAsync`, `WScript.Broadcast`):**
   - Run `run_host_async_probe.sh`, `run_tsfn_teardown`, `test/es7/s6-growable-cross-agent.js`,
     and multi-agent `-NegativePhaseReport` + broadcast overrun stress under ThreadSanitizer.
3. **UBSan + `GCStress` + Historic Flake Checks:**
   - Remove `-fno-sanitize=undefined` on a Linux test build and address any surfaced UB
     (known audit candidates in `open-cards.md`: signed shifts/casts in `ToInt32Core`/`TryToInt64`).
   - Run extended `./zig-out/bin/GCStress` + `-js -ForceBlockingConcurrentCollect`.
   - Stress `test/es7/class-private-methods.js` (50x, historically flagged in `open-cards.md` for
     rare Linux simple-JIT SIGSEGV) and `Octane/earley-boyer.js` (30x under Linux JIT to confirm
     whether the 1-off pre-`Inline.cpp`-fix Mac flake occurs on x64).

### Phase 6 -- Certified Linux Test262, Linux Benchmarks & Hosted GitHub Actions CI
1. **Certified Linux Test262 run (supersedes Nov 9 `77,354 / 3,190` in `doc/b4-linux-run.md`):**
   ```sh
   python3 tools/test262/refresh_baseline.py --binary "$PWD/zig-out/bin/ch" --jobs 8
   ```
   (Use `--publish` once verified cleanly on Linux.) Compare against Mac `refresh-2026-11-16`
   (`78,731 / 1,813` = `97.75%`), paying close attention to whether the `16` Mac host
   `Atomics/{notify,wait,waitAsync}` timeout/`SIGKILL` entries behave differently under Linux futex scheduling.
2. **Linux x64 Benchmark Baseline:**
   ```sh
   python3 tools/bench/run.py --binary zig-out/bin/ch --tier jit --iterations 5 -o tools/bench/results/linux-x64-jit.json
   python3 tools/bench/run.py --binary zig-out/bin/ch --tier interp --iterations 5 -o tools/bench/results/linux-x64-interp.json
   ```
3. **Hosted GitHub Actions Confirmation:**
   - Once pushed on a branch/PR, verify first green hosted runs for both
     [zig-linux.yml](../.github/workflows/zig-linux.yml) (`style`, `zig`, `zig-variants`, `nodeapi`, `record`)
     and [zig-macos.yml](../.github/workflows/zig-macos.yml) (all jobs green).

---

## 4. Future Notes for `aarch64-linux-gnu` (Linux ARM64)
Not needed for initial `x86_64` Linux bring-up, but when extending to Linux ARM64:
- Already in place: ARM64 FullJIT, WASM ARM64 (`382/382` P1 on macOS), `Inline.cpp` `ENABLE_ASMJS`,
  cross-runtime GSAB `LDAR`, and `Recycler.inl` 16 KiB-page medium-block fix (`20ea96941`, which also
  protects 16 KiB/64 KiB page ARM64 Linux kernels).
- Remaining work to enable `aarch64-linux-gnu`:
  1. Generalize `gateTarget` and target triple logic in `tools/build/linux.zig:50,147-170` beyond `x86_64`.
  2. Add ARM64 Backend/runtime/PAL object groups in `tools/build/tables_linux.zig`.
  3. Complete and test `CHAKRA_ARM64_DWARF_EH_FRAME` DWARF `.eh_frame` unwind registration in
     `lib/Common/Memory/arm64/XDataAllocator.cpp` and `lib/Common/PlatformAgnostic/AssemblyCommon.h`.
