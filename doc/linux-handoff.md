# ChakraCore Modernization -- Linux Bring-Up and Testing Handoff

> Original Handoff: 2026-11-16 (`modernize/baseline` at `526393e26`, `8f4e85d62..526393e26`)
> Synchronized: **2026-11-17** (`modernize/baseline` at `06d783c64`, including Linux bring-up `4778b3a2b..c639e9f02` and post-review fixes `193c95046..06d783c64`)
> Companion Docs: [architecture-and-readiness-review.md](architecture-and-readiness-review.md),
> [b4-linux-run.md](b4-linux-run.md), [b4-d1-d4.md](b4-d1-d4.md),
> [build-contract-linux.md](build-contract-linux.md), [open-cards.md](open-cards.md),
> [perf-baseline.md](perf-baseline.md)

> [!NOTE]
> **Execution Status & Closure Summary (2026-11-17, `06d783c64`)**
> Phases 1–4 and Phase 6 of this handoff have been **executed on native `x86_64` Linux** (Ubuntu 24.04.5, 4 vCPU, glibc 2.39, ICU 74.2) in `4778b3a2b..c639e9f02` — see [b4-linux-run.md](b4-linux-run.md) §10 for full receipts and SHA-256 provenance — and both hosted GitHub Actions workflows (`zig-linux` [`44054742113`](https://github.com/amscotti/ChakraCore/actions/runs/44054742113) and `zig-macos` [`44054742259`](https://github.com/amscotti/ChakraCore/actions/runs/44054742259)) passed at `c639e9f02`. Post-review fixes (`193c95046`, `8b0b0e6a8`, `06d783c64`) are verified on macOS ARM64 ([b4-linux-run.md](b4-linux-run.md) §11) and await Linux x64 re-verification alongside the open items below. The historical pre-execution handoff checklist (`526393e26`) is preserved verbatim below this summary.

> [!NOTE]
> **G0 single-pass Linux x64 revalidation (2026-11-18, `8b447ac48`).** The §0 twelve-step runbook was executed on Ubuntu 24.04 x86_64 (4 vCPU, glibc 2.39, ICU 74.2). Steps 1–7 and 9–11 are green: default **2868/0** interpreted and **2330/0** dynapogo; no-JIT/release-shared/debug-static PASS; FullJit **615/0**; WASM P1 **382/382**; debug WASM **66/66**; ASan/UBSan **26/26** each; Node-API **150** exports, **18/18** probes, **5x** stress, **46/23/0** matrix, **52/61/6** addon matrix; stress loops **30x/50x/30x**; packaging PASS. Test262 `refresh-2026-11-18` on a clean tree: **79,399 / 1,145 (98.58%)** edition-selected, **80,237 / 12,904 (86.15%)** broad, **0 shape-suspects**. TSan **25/26** fails on the PAL startup race (`cs.cpp:723` vs `cs.cpp:775/907`), triaged in [open-cards.md](open-cards.md) §12. Residual host-speed deltas: two non-P1 `wasm/limits.js` 300 s timeouts, eight interpreter benchmark 60 s timeouts, and 16 pre-fix Test262 pass→fail paths (two Proxy cases fixed by `28d3b585c`; 12 `decodeURI*` host flakes; two revoked-proxy realm cases carded). Full receipt: [b4-linux-run.md](b4-linux-run.md) §12. Hosted Linux CI [`44407984285`](https://github.com/amscotti/ChakraCore/actions/runs/44407984285) is green. macOS run [`44407984282`](https://github.com/amscotti/ChakraCore/actions/runs/44407984282) shows **failure only because GitHub cancelled queue-bound jobs** (`zig test`, `debug-static`, `StyleChecks`, and `record` passed; `release-shared`, `jit+wasm`, `nodeapi`, `B1`, `legacy cmake`, and `incremental proofs` were cancelled). Post-G0 fix `28d3b585c` makes getter-only accessors report `Reflect.set` failure; regression stays **2868/0** and **2330/0**, the two Proxy `set` Test262 scenarios pass, and the two `base-ctor-revoked-proxy-realm.js` scenarios are carded in [open-cards.md](open-cards.md) §12.

### Phase 1–6 Execution Mapping (`4778b3a2b..c639e9f02`)

| Phase | Scope | Linux Status (`c639e9f02`) | Receipt / Notes |
|---|---|---|---|
| **Phase 1** | Pre-flight tooling & portable StyleChecks | **Executed (PASS)** | Hosted CI [`44054742113`](https://github.com/amscotti/ChakraCore/actions/runs/44054742113) `style` + `zig` gate checks green; at `06d783c64` local Mac re-verify `5/5` StyleChecks, `89/89` `test_build_tools.py`, `7/7` `linux-gate-tests.zig`, `4/4` `owned-bytes.zig`. |
| **Phase 2** | Default `test` profile, `RECORDED_BASELINE`, D1-D4 & ELF unwind | **Executed (PASS)** | `81be961a1` / [b4-linux-run.md](b4-linux-run.md) §10: `check-smoke`/`metadata`/`jit`/`native`/`bytecode` PASS; `RECORDED_BASELINE` re-pinned to **2868/0** interpreted and **2330/0** dynapogo (`AsyncSuspendedResume.js` enabled); `linux_exports_{so,ch}.txt` vs legacy CMake build remains uncompared. |
| **Phase 3** | Linux build variants (`-Djit=false`, `release-shared`, `debug-static`) | **Executed (PASS)** | `81be961a1`, `42a38047c`: `-Djit=false` PASS; `release-shared` smoke/metadata/native PASS (`234` exports vs `237` on `test`, omitting 3 `ENABLE_DEBUG_CONFIG_OPTIONS` `JsPrivate*` symbols); `debug-static` smoke/metadata/bytecode/native (`8/8` static suites) + `66/66` debug WASM (zero assertions). |
| **Phase 4** | Node-API stack (`libChakraNodeApi.so` & `ch-nodeapi`) | **Executed (PASS w/ caveat)** | `81be961a1`, `c639e9f02`: `check-nodeapi` (`150` exports), `18/18` probes, `5x` stress, `run_matrix.py` **46/23/0**, `run_node_addon_api_matrix.py` **52/61/6** (only the 6 documented PR-12 V8 error-text diffs). Local run used Node `v26.12.1` instead of pinned `v22.22.3`; hosted `nodeapi` CI job green. |
| **Phase 5** | Sanitizers (`ASan`/`TSan`/`UBSan`), `GCStress` & historic flakes | **Partially executed / OPEN** | `GCStress -js -off:ConcurrentCollect -ForceBlockingConcurrentCollect` ran 300s without crashing (`timeout` rc=124 before completing full heap walk — bounded liveness check only). `ASan`/`TSan`/`UBSan` **not run** (`tools/build/linux.zig` has no sanitizer lane; `zig c++` lacks bundled ASan runtime). `earley-boyer` 30x & `class-private-methods` 50x stress unrun. |
| **Phase 6** | Certified Linux Test262, benchmarks & hosted Actions CI | **Executed (PASS w/ open follow-ups)** | Test262 `refresh-2026-11-17` (`29230a5b7`): **78,491 / 2,053** (**97.45%** edition), **79,329 / 13,812** (**85.17%** broad). Benchmarks (`120 s` timeout): **65/65** JIT (5 iters), **54/55** Interp (`Octane/zlib.js` `593` at `300 s`, JetStream omitted); JSONs not yet committed. Hosted CI green (`44054742113`, `44054742259`). |

### Remaining Open Items on Linux (`06d783c64`)
1. **Linux re-verification of post-review fixes (`193c95046..06d783c64`):** native Linux + hosted CI confirmation of `.github/workflows/zig-linux.yml` `debug-static` `check-native`, `check_native.py` `--whole-archive` check, `check_jit.py` Release default-flags runtime execution on Linux `release-shared`, `refs_probe`/`wrap_probe` `PROBE_NOINLINE_OPTNONE` at `-O3`, `BigInt.prototype.toLocaleString` (`8b0b0e6a8`), DBG `EmitBlock`/`EmitLoop` `MarkAsmJsLabel`, and `_M_AMD64` `CrossSite` multi-value runtime dispatch (`06d783c64`).
2. **Sanitizer lane (`ASan`/`TSan`/`UBSan`):** wire system `clang++` / sanitizer runtime linkage in `tools/build/linux.zig` and execute Phase 5 items 1–3.
3. **Committed Linux benchmark JSON artifacts:** capture and commit `tools/bench/results/linux-x64-{jit,interp}.json` (currently only `*-darwin-arm64.json` exist in `tools/bench/results/`).
4. **Pinned Node `v22.22.3` local matrix confirmation:** re-run local Linux Node-API matrix under pinned `v22.22.3` (local receipt [`c639e9f02`](b4-linux-run.md) used `v26.12.1`).
5. **Historic flake & stress runs:** `Octane/earley-boyer.js` 30x under Linux JIT, `test/es7/class-private-methods.js` 50x, and an unbounded/completed `GCStress` matrix.
6. **`check-wasm` target in `tools/build/linux.zig`:** `build.zig` defines `check-wasm`, whereas `tools/build/linux.zig` currently does not (Linux `66/66` debug WASM ran via `runtests.py`); optional `linux_exports_{so,ch}.txt` diff against legacy CMake also remains open.

> [!IMPORTANT]
> **Wave 3 update (2026-11-17, `modernize/baseline` at `40c2fe5be`).** Tiers 1-5 are complete on macOS ARM64 (see [modernization-plan.md](modernization-plan.md), [open-cards.md](open-cards.md) §13). Certified Mac Test262: **98.89% edition (79,648 / 896 of 80,544)**, **86.41% broad (80,486 / 12,655 of 93,141)**; buckets post-ES2025 `11,759`, engine-defect `846`, required-feature-missing `34`, host `16`; negative phase `8,741` passed / `0` shape-suspects / `1` marker downgrade. Mac gates: `check` 2064/0, `debug-static` 2116/0, `check-wasm` P1 382/382, `check-fulljit` 615/0, `check-sanitize` 26/26 (`address`, `undefined`), `check-package` PASS. Section 0 is the single-pass Linux x64 revalidation runbook and supersedes the "Remaining Open Items" list below.

## 0. Single-Pass Linux x64 Revalidation Runbook (`40c2fe5be`)

Run on Ubuntu 24.04 x86_64 (override flags: section 2). Stop at the first failure and record it in [b4-linux-run.md](b4-linux-run.md). Every step runs from the repo root; prefix `zig` with `mise exec --`.

| # | Step | Commands | Expected |
|---|---|---|---|
| 1 | Pre-flight and StyleChecks | `git fetch origin master:refs/remotes/origin/master`; `STYLECHECK_BASE=8b073a4c3 bash tools/StyleChecks/{check_copyright,check_ascii,check_tabs,check_eol}.sh`; `python3 tools/StyleChecks/check_style_portable.py --self-test` | all exit 0, `NO PROBLEMS DETECTED` |
| 2 | Tool unit suites | `python3 tools/build/test_build_tools.py`; `python3 tools/package/test_package_tools.py`; `python3 tools/test262/test_edition_policy.py`; `python3 tools/test262/refresh_baseline.py --self-test`; `zig test tools/linux-gate-tests.zig`; `zig test doc/examples/owned-bytes.zig` | all OK; `package` 9 tests; `linux-gate-tests` 9/9; `owned-bytes` 4/4; edition policy 7 tests |
| 3 | Default `test` build + core gates | `zig build check` then `zig build check-selftest check-smoke check-jit check-native check-metadata verify-bytecode` | exit 0; `check_regression` interpreted/dynapogo 0 failures; if counts differ from `RECORDED_BASELINE` in `tools/build/check_regression_linux.py` (last pinned 2868/2330), audit against Mac 2064 and re-pin with `test_build_tools.py` |
| 4 | Build variants | `zig build -Djit=false check-smoke check-metadata verify-bytecode check-native`; `zig build -Dprofile=release-shared check-smoke check-metadata check-jit check-native`; `zig build -Dprofile=debug-static -Doptimize=debug check` | all PASS; debug-static 8/8 static native suites |
| 5 | WASM, fulljit, GCStress gates | `zig build check-wasm check-fulljit check-gcstress`; `(cd test && python3 runtests.py --x64 --debug --variants=interpreted -b ../out/debug-static/bin/ch wasm)` | `check-wasm` P1 382/382 (x64 manifest may differ; list deltas), `check-fulljit` 0 failures, GCStress completes (no 300 s timeout), zero debug assertions |
| 6 | ASan | `zig build -Dsanitize=address check-sanitize` | 26/26 legs clean, no LSan reports |
| 7 | UBSan | `zig build -Dsanitize=undefined check-sanitize` | 26/26 legs clean (the 4 Tier 3 UB fixes hold on x64) |
| 8 | TSan (Linux-only, never run) | `zig build -Dsanitize=thread check-sanitize`; then `bash test/nodeapi/probes/run_host_async_probe.sh`, `test/es7/s6-growable-cross-agent.js`, `WScript.Broadcast` multi-agent run | clean, or each report triaged into [open-cards.md](open-cards.md) |
| 9 | Node-API stack | `zig build libChakraNodeApi ch-nodeapi check-nodeapi`; `export SKIP_BUILD=1`; `bash tools/nodeapi/run_probe.sh`; `for s in test/nodeapi/probes/run_*.sh; do bash "$s" \|\| exit 1; done`; `REPEATS=5 sh test/nodeapi/stress/run_stress.sh`; `python3 test/nodeapi/run_matrix.py --suite all --host ch-nodeapi`; `python3 test/nodeapi/run_node_addon_api_matrix.py --hosts both` under pinned Node `v22.22.3` | 150 exports, 18/18 probes, matrix 46/23/0, addon-api 52/61/6 (only the 6 PR-12 V8 error-text diffs) |
| 10 | Stress and historic flakes | `Octane/earley-boyer.js` 30x under Linux JIT; `test/es7/class-private-methods.js` 50x; `AsyncSuspendedResume.js`/`AsyncSuspendedDetach.js` 20-30x interpreted; `(cd test && python3 runtests.py --x64 -t -b ../zig-out/bin/ch TryCatch StackTrace)` | zero crashes or hangs |
| 11 | Packaging | `zig build -Dprofile=release-shared package check-package` | `MANIFEST.json` checksums verify, deterministic archive, packaged `ch`/`ch-nodeapi` run with `$ORIGIN/../lib`; `ALL PACKAGE TESTS PASSED` |
| 12 | Certified Test262, benchmarks, hosted CI | `python3 tools/test262/refresh_baseline.py --binary "$PWD/zig-out/bin/ch" --jobs 8`; `python3 tools/bench/run.py --binary zig-out/bin/ch --tier jit --iterations 5 -o tools/bench/results/linux-x64-jit.json` (and `--tier interp` -> `linux-x64-interp.json`); push and confirm `zig-linux.yml` + `zig-macos.yml` green | Compare to Mac `98.89% (79,648 / 896)`; expect only ICU and futex-timing deltas (`Atomics/{notify,wait,waitAsync}` host bucket 16), `0` feature regressions, `0` shape-suspects; publish with `--publish` only after a clean run; commit benchmark JSONs |

Exit criteria: all 12 steps green (or each deviation recorded as a card), `RECORDED_BASELINE` re-pinned if changed, Linux Test262 and benchmark artifacts committed, hosted CI green. Remaining out of scope: B6 Windows and `aarch64-linux-gnu` (section 4).


---

## Historical Pre-Execution Handoff Record (`526393e26`, 2026-11-16)

This document records the exact closure state reached on macOS ARM64 (`darwin-arm64`)
at `526393e26` and specifies everything that was queued to be built, verified,
re-pinned, stressed, and monitored when executing the Linux x64 bring-up above
(with §4 notes for future `aarch64-linux-gnu`).

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
