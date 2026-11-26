# ChakraCore

[![Discord Chat](https://img.shields.io/discord/695166668967510077?label=Discord&logo=Discord)](https://discord.gg/dgRawPdNuC)
[![Licensed under the MIT License](https://img.shields.io/badge/License-MIT-blue.svg)](https://github.com/Microsoft/ChakraCore/blob/master/LICENSE.txt)
[![PR's Welcome](https://img.shields.io/badge/PRs%20-welcome-brightgreen.svg)](#contribute)

ChakraCore is a compact, embeddable JavaScript and WebAssembly engine with a flat C API (`JsRT`) and a separate Node-API v10 addon provider (`libChakraNodeApi` on `modernize/baseline` with `161` exports; previously v8 / `150` exports pre-Phase-4). With Phase-4 (`Q1.1`–`Q3`, `I1`/`I2`, `P1`, `C3.1`), Waves 1–5b + §§32–33 hardening/Wave 2 remediation fixes, and audited Linux/Windows bring-up fixes promoted into `modernize/baseline`, it builds with a single Zig `0.17.0` front end across **macOS ARM64** (certified), **Linux x64** (`G0` baseline verified; §§30, 32, 33 follow-ups landed, `G0-phase4` native re-certification queued), and **Windows x64** (`B6` native `x86_64-windows-msvc` `zig build`, `VS_VERSION_INFO`, `debug-static` CRT/UBSan, `SharedArrayBuffer.cpp` PCH, and packaging fixes landed, awaiting native certification), delivering a `~5.5 MB` release package (`~5.3 MB` pre-Phase-4), **`99.57%`** certified ES2025 edition conformance (**`80,195 / 80,544`**) and **`88.97%`** broad conformance (**`82,866 / 93,141`**) on macOS ARM64, 3-tier JIT + concurrent `Recycler` GC, and WebAssembly JIT (MVP + sign-extension + nontrapping conversions: `383/383` P1 via `check-wasm` / `check-wasm-jit` + `14/14` `wasm.simd` on non-debug `jit`; multi-value off by default via `-WasmMultiValue`, experimental SIMD off by default via `-WasmSimd`, bulk-memory unsupported).

## Project Direction: The Embeddable JIT Sweet Spot

This fork targets the middle ground between interpreter-only embeds and heavyweight browser engines: sub-minute Zig builds, compact binaries, flat C ABIs (`JsRT` `237` exports + `libChakraNodeApi` `161` exports) with zero `v8.h` shims, multi-tier x64/ARM64 JIT + WebAssembly (MVP/sign-extension/nontrapping default-on; opt-in `-WasmMultiValue` and experimental `-WasmSimd`), and narrow Zig C-ABI leaf wedges outside the C++ engine core. Highlights promoted into `modernize/baseline`:

- **`Q1.1` Node-API v9/v10 + Multi-Addon (`161` exports):** `155` `napi_*`/`node_api_*` + `6` host helpers, `--addon` multi-addon registry, per-context shared `Buffer` marker, `19/19` probes, `52/17/0` C matrix.
- **`Q1.2`–`Q1.5` ES2025 Conformance Sweeps I–III:** Lifted certified ES2025 edition conformance from `98.89%` (historical pre-Phase-4 baseline) through `99.06%` (`Q1.2`), `99.25%` (`Q1.4`), `99.26%` (`Q2`/`C3.1`), and `99.56%` (`Q1.5`, `+247` non-`waitAsync` fixes across `133` files with `0` JIT diffs) to **`99.57%`** (`Q3`).
- **`Q1.3` / `Q2` / `Q3` ES2026 Waves 1–3 & `Z1` Zig Codec:** `Error.isError`, `Math.sumPrecise`, `Array.fromAsync`, `Uint8Array` Base64/Hex backed by `lib/ZigCodec/u8a_codec.zig` (`Z1`), `Iterator.concat`, `Map`/`WeakMap` `getOrInsert*`, `JSON.rawJSON`/`isRawJSON`/`context.source`, `Iterator.{zip,zipKeyed}`, `Iterator.prototype.{chunks,windows,includes,join}`, and `Promise.{allKeyed,allSettledKeyed}` (`+578` broad Test262 in `Q3` with `622/0` JIT parity).
- **`I1` / `I2` Modern ECMA-402 over C ICU4C:** `Intl.{Locale,ListFormat,RelativeTimeFormat,DisplayNames,Segmenter,getCanonicalLocales,supportedValuesOf,DurationFormat}` implemented strictly in C++ over C ICU4C APIs (`Intl.js` and `JsBuiltIn*.bc.*.h` untouched).
- **`P1`, `C3.1`, `ARM64 WB`, & Waves 1–5b + §§32–33 Hardening & Wave 2:** Pathological JIT bailout/ReJIT and ARM64 asm.js tier-up fixes (`1.453x` 64-test geomean vs pre-Phase-4 JIT; `Kraken/audio-fft.js` `65.94x`), Explicit Resource Management (`Symbol.{dispose,asyncDispose}`, `SuppressedError`, `DisposableStack`, `AsyncDisposableStack`, `using`/`await using` + JIT lowering, `947/0` Test262), ARM64 JIT software write-barrier fix under `ConcurrentCollect`, and Waves 1–5b + §§32–33 runtime/conformance/security/UB/CI/packaging fixes (`2132/0` `test`, `2184/0` `debug-static`, `651/0` `check-fulljit` / `961/961` `--all-jit-suites`, `32/32` & `34/34` Mac sanitize, `126` build-tool tests).

See [doc/modernization-plan.md](doc/modernization-plan.md#active-next-up-execution-queue-modernizebaseline) for the active Now/Next/Later queue, [doc/vision-and-roadmap.md](doc/vision-and-roadmap.md) for Phase-4 tracks (`G0-phase4`, `B6`, `I3`, `H1`), and [doc/open-cards.md](doc/open-cards.md) for verified gates and open items.

## Upstream History

As you may have heard Microsoft Edge no longer uses Chakra. Microsoft stopped actively maintaining upstream ChakraCore 1.11 after March 2021.

ChakraCore is planned to continue as a community project targeted primarily at embedded use cases. We hope to produce future releases with new features and enhancements to support such use cases. We also would like to invite any interested parties to be involved in this project. For further details please see the following draft planning documents:
[Overall plan](https://github.com/chakra-core/org/blob/master/ChakraCore%20Future%20Plan.md)
[Version 1.12 plan](https://github.com/chakra-core/org/blob/master/Release%201.12%20plan.md)

Also see discussion in issue [#6384](https://github.com/microsoft/ChakraCore/issues/6384)

If you'd like to contact the community team please either open an issue or join the Discord chat linked above.

## Security

If you believe you have found a security issue in ChakraCore 1.11, please share it with Microsoft privately following the guidance at the Microsoft [Security TechCenter](https://technet.microsoft.com/en-us/security/ff852094). Reporting it via this channel helps minimize risk to projects built with ChakraCore.

If you find a security issue in the Master branch of Chakracore but not in 1.11 please join our Discord server and private message one of the Core team members.

## Documentation

* [ChakraCore Architecture](https://github.com/chakra-core/ChakraCore/wiki/Architecture-Overview)
* [Quickstart Embedding ChakraCore](https://github.com/chakra-core/ChakraCore/wiki/Embedding-ChakraCore)
* [API Reference](https://github.com/chakra-core/ChakraCore/wiki/JavaScript-Runtime-%28JSRT%29-Reference)
* [Contribution guidelines](CONTRIBUTING.md)
* [Blogs, talks and other resources](https://github.com/chakra-core/ChakraCore/wiki/Resources)
* [Modernization documentation and verified baselines](doc/README.md)

## Zig `0.17.0` Build & Test Quickstarts (`modernize/baseline`)

All active platform builds use the Zig `0.17.0` front end pinned in [mise.toml](mise.toml) (`zig` on `PATH` or `mise exec -- zig ...`).

| Platform | Target / Toolchain | Default Install | Verification & Handoff |
|---|---|---|---|
| **macOS ARM64** | `aarch64-macos.26.0`, Xcode 26.5 SDK, Homebrew `icu4c` | `zig-out/` (`out/<profile>/`) | Certified on `modernize/baseline`; CI [.github/workflows/zig-macos.yml](.github/workflows/zig-macos.yml) |
| **Linux x64** | `x86_64-linux-gnu` (Ubuntu 24.04: `glibc 2.39`, `ICU 74`, `clang` for sanitizers) | `zig-out/` (`out/<profile>/`) | `G0` verified; §30 fixes landed, `G0-phase4` re-certification in [doc/linux-handoff.md](doc/linux-handoff.md) §0A; CI [.github/workflows/zig-linux.yml](.github/workflows/zig-linux.yml) |
| **Windows x64** | `x86_64-windows-msvc`, VS 2022 Build Tools + Win10/11 SDK (`ml64.exe` only; no `midl.exe`/`mc.exe`), `vcpkg` `icu:x64-windows` | `zig-out\` (`out\<profile>\`) | `B6` bring-up + `VS_VERSION_INFO`/`debug-static` fixes landed, awaiting native certification; runbook [doc/windows-handoff.md](doc/windows-handoff.md); CI [.github/workflows/zig-windows.yml](.github/workflows/zig-windows.yml) |

### macOS ARM64

```sh
mise exec -- zig build                              # default (test) profile -> zig-out/
mise exec -- zig build check                        # regression (2132/0) + bytecode + native + smoke + metadata + selftest
mise exec -- zig build -Dprofile=debug-static check # static Debug archive + 8/8 native suites (2184/0)
mise exec -- zig build -Dprofile=release-shared check-package
mise exec -- zig build -Dprofile=jit -Dwasm=true check-wasm check-wasm-jit check-fulljit # P1 WASM (383/383) + wasm.simd (14/14) & FullJit (--all-jit-suites 961/961; default suites 651/0); multi-value (-WasmMultiValue) & SIMD (-WasmSimd) remain P3
mise exec -- zig build -Dsanitize=address check-sanitize
mise exec -- zig build -Dsanitize=undefined check-sanitize
```

### Linux x64

See [doc/linux-handoff.md](doc/linux-handoff.md) §0A for the Phase-4 (`G0-phase4`) 10-step revalidation runbook and non-Ubuntu-24.04 override flags (`-Dglibc=<M.m>`, `-Dicu-major=<N>`, `-Dicu-include=<dir>`, `-Dicu-libdir=<dir>`):

```sh
mise exec -- zig build                              # default (test) profile w/ x64 JIT + WASM
mise exec -- zig build check                        # regression + bytecode + native + smoke + jit + metadata + selftest
mise exec -- zig build -Djit=false check-smoke check-metadata verify-bytecode check-native
mise exec -- zig build -Dprofile=debug-static check
mise exec -- zig build -Dprofile=release-shared package check-package
mise exec -- zig build check-fulljit check-wasm check-gcstress
mise exec -- zig build -Dsanitize=address check-sanitize   # also undefined, thread
mise exec -- zig build libChakraNodeApi ch-nodeapi check-nodeapi
```

### Windows x64 (`x86_64-windows-msvc`)

Run from an **x64 Native Tools Command Prompt for VS 2022** with Zig `0.17.0` and `vcpkg` `icu:x64-windows` (`VCPKG_ROOT` or `CHAKRA_ICU_DIR`, with its `bin` directory on `PATH`). The Windows graph passes `-DENABLE_OOP_NATIVE_CODEGEN=0` (no `midl.exe`) and `-DDISABLE_JS_ETW` (no `mc.exe`), leaving `ml64.exe` as the sole external MSVC binary required by `zig build`. See [doc/windows-handoff.md](doc/windows-handoff.md) for prerequisites, the 10-step bring-up runbook, and troubleshooting:

```powershell
zig test tools/windows-gate-tests.zig
zig build                                           # ChakraCore.dll/.lib + ch.exe + GCStress.exe -> zig-out\
zig build check-smoke check-metadata verify-bytecode check-jit check-selftest
zig build -Dallow-unpinned-regression=true check    # first-run B6 bootstrap before pinning RECORDED_BASELINE
zig build -Djit=false check-smoke check-metadata verify-bytecode check-gcstress
zig build -Dprofile=debug-static check-smoke check-metadata verify-bytecode check-gcstress
zig build -Dprofile=release-shared check-metadata check-jit libChakraNodeApi ch-nodeapi check-nodeapi check-smoke check-package
zig build check-wasm check-fulljit check-gcstress
```

Windows x64 options: `-Dprofile=test|debug-static|release-shared` (`test` -> `zig-out/`, `debug-static` -> `out/debug-static/`, `release-shared` -> `out/release-shared/`), `-Djit=true|false` (`false` sets `-DDISABLE_JIT=1`), `-Dicu=icu4c|windows|none` (default `icu4c` for full `I1`/`I2` ECMA-402 parity; `windows` selects `WINDOWS10_ICU`; `none` omits ICU), `-Dicu-include=<dir>`, `-Dicu-libdir=<dir>`, `-Dml64=<path>`, `-Dwin-sdk-um=<dir>`, and `-Dallow-unpinned-regression=true|false` (first-run `B6` bootstrap for `check-regression`/`check` before pinning `RECORDED_BASELINE` in `tools/build/check_regression_windows.py`).

> **Legacy MSBuild & CMake (Historical Reference):** Legacy `Build\Chakra.Core.sln` (`test\ci.buildone.cmd`) on Windows and `build.sh` / `cmake` on Unix remain in the tree solely as historical references for upstream comparisons (see [doc/build.md](doc/build.md); legacy scripts do not build `libChakraNodeApi` or `ch-nodeapi` and do not include all Phase-4 Zig tooling).

## Using ChakraCore

Once built, you have a few options for how you can use ChakraCore:

* Run JavaScript directly with the `ch` (`ch.exe` on Windows) host binary in `zig-out/bin/ch` (or `out/<profile>/bin/ch`), for example `zig-out/bin/ch hello.js` (with `print()` available for console output).
* Embed ChakraCore via the flat `JsRT` C API (`zig-out/include/ChakraCore.h` and `ChakraCommon.h`) — see [Embedding ChakraCore](https://github.com/chakra-core/ChakraCore/wiki/Embedding-ChakraCore) and [API Reference](https://github.com/chakra-core/ChakraCore/wiki/JavaScript-Runtime-%28JSRT%29-Reference).
* Note: ChakraCore is a JavaScript and WebAssembly engine, not a browser or Node.js runtime; DOM APIs (`document.write()`) and Node built-ins are not provided by the engine.

## Node-API provider (`libChakraNodeApi`)

ChakraCore on this branch can host multiple Node-API addons in one
process through a separate shared library, `libChakraNodeApi`,
implemented on ChakraCore's JsRT C API. The provider **implements the
Node-API v10 engine surface (`js_native_api.h`, 123 symbols) plus the
plan §4 Table B functions (`node_api.h`, 32 symbols) under the §3.2 host
contract**. This does not provide a Node.js runtime: there is no
`require`/CommonJS resolution, `process`, Node built-ins, timers, a
`node` executable, or npm, and the provider must never be described as
Node runtime compatibility. Addons requesting `NAPI_VERSION` >
10 (including `NAPI_VERSION_EXPERIMENTAL`) are rejected at load.
(`Q1.1` additive `NAPI_VERSION=9/10` exports and multi-addon registry
support promoted into `modernize/baseline`; a separate Zig + `libuv` host
runtime [`H1`] is documented in
[doc/vision-and-roadmap.md](doc/vision-and-roadmap.md).)

Known non-conformances (see [doc/node-api-receipt.md](doc/node-api-receipt.md) for the
probe-backed record):

- No JS `Buffer` global. Provider Buffers are `Uint8Array` views
  marked by a per-context provider-private symbol (shared across
  coexisting `napi_env` instances on the same `(runtime, context)` so
  cross-addon `napi_is_buffer` checks pass); `typeof Buffer ===
  'undefined'`.
- `Float16Array` (JS-created) returns `napi_generic_failure` in
  `napi_get_typedarray_info` because `NAPI_VERSION=10` defines no float16
  typed-array enum value. (`BigInt64Array` / `BigUint64Array` are wired
  directly through Phase-3 JsRT.)
- External Latin-1 / UTF-16 string creation
  (`node_api_create_external_string_{latin1,utf16}`) always copies into
  ChakraCore's Recycler string heap (`*copied = true`) and invokes the
  finalizer synchronously.
- Async work / thread-safe functions / `napi_get_uv_event_loop`
  return `napi_generic_failure` unless the host injects the §3.2
  scheduling/uv services (the tree's `tools/nodeapi/ch-nodeapi` test host
  injects `post_task`/`post_work`/`cancel_work`/`ref_loop`/`unref_loop`,
  with `uv_loop = NULL` so `napi_get_uv_event_loop` still fails cleanly).
- `napi_async_init`/`napi_async_destroy` bookkeeping only:
  async_hooks propagation is a documented no-op.

Build and gate the provider (`libChakraNodeApi` + `ch-nodeapi` + `check-nodeapi` verifying exactly `155` `napi_*`/`node_api_*` + `6` host = `161` exports on shared profiles `test`, `jit`, and `release-shared`):

```sh
mise exec -- zig build libChakraNodeApi ch-nodeapi check-nodeapi
# installs lib/libChakraNodeApi.{dylib,so} (or bin/libChakraNodeApi.dll + lib/libChakraNodeApi.lib on Windows)
# plus include/nodeapi/{js_native_api,js_native_api_types,node_api,node_api_types}.h
```

The exports gate (`check-nodeapi`) stays separate from `zig build check` so engine regression pins in `tools/build/gate_counts.json` remain decoupled. A from-scratch consumer (`zig cc -DNAPI_VERSION=10 -I<installed>/include/nodeapi`, linking the built provider, run through `tools/nodeapi/ch-nodeapi` -- no Node executable; note upstream `include/nodeapi/js_native_api_types.h` defaults `NAPI_VERSION` to `8` when `-DNAPI_VERSION=...` is omitted) is the packaging proof. macOS ARM64 v10 verification (`19/19` probes, stress, `52/17/0` C matrix, `52/61/6` `node-addon-api` matrix) is recorded in [doc/node-api-receipt.md](doc/node-api-receipt.md) §14; Linux x64 (`G0-phase4`) and Windows x64 (`B6`) validation queues live in [doc/linux-handoff.md](doc/linux-handoff.md) §0A and [doc/windows-handoff.md](doc/windows-handoff.md).

## Contribute

Contributions to ChakraCore are welcome.  Here is how you can contribute to ChakraCore:

* [Submit bugs](https://github.com/chakra-core/ChakraCore/issues) and help us verify fixes.
* [Submit pull requests](https://github.com/chakra-core/ChakraCore/pulls) for bug fixes and features and discuss existing proposals

Please refer to [Contribution Guidelines](CONTRIBUTING.md) for more details.

## License

Code licensed under the [MIT License](https://github.com/chakra-core/ChakraCore/blob/master/LICENSE.txt).

## Contact Us

If you have questions about ChakraCore, or you would like to reach out to us about an issue you're having or for development advice as you work on a ChakraCore issue, you can reach us as follows:

* Open an [issue](https://github.com/chakra-core/ChakraCore/issues/new) and prefix the issue title with [Question]. See [Question](https://github.com/chakra-core/ChakraCore/issues?q=label%3AQuestion) tag for already-opened questions.
* Discuss ChakraCore with the team and the community via the Discord link above
