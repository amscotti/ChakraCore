# Project Vision & Phase-4 Roadmap — The Embeddable JIT Sweet Spot

> **Status (2026-11-17, `modernize/baseline` at `40c2fe5be`; `modernize/phase4` Q1.1 landed):** Tiers 1–5 pre-Linux modernization work is complete on `modernize/baseline` on macOS ARM64 (`98.89%` certified ES2025 edition conformance, multi-tier JIT + WASM NEON SIMD, `libChakraNodeApi` v8, ASan/UBSan sanitizer lanes, and deterministic `b5-package` tarballs). On `modernize/phase4`, Track **`Q1.1`** (Node-API v9/v10 + multi-addon support, `161` exports) is complete. This document defines where the fork is positioned going forward and organizes the remaining Phase-4 expansion tracks alongside [linux-handoff.md](linux-handoff.md) §0 Linux x64 revalidation.
>
> Companion Docs: [modernization-plan.md](modernization-plan.md), [zig-strategy.md](zig-strategy.md), [node-api-plan.md](node-api-plan.md), [node-api-receipt.md](node-api-receipt.md), [memory-management.md](memory-management.md), [b5-package.md](b5-package.md), [linux-handoff.md](linux-handoff.md), [open-cards.md](open-cards.md).

---

## 1. Strategic Positioning — The Embeddable JIT Sweet Spot

JavaScript engines tend to cluster at two extremes:

- **Lightweight interpreter engines** (`QuickJS-NG`, `Boa`, `LibJS`) are compact and fast to compile from source, making them great for small embeds — **but without a full optimizing multi-tier JIT and concurrent collector**, compute-heavy JavaScript and WebAssembly workloads pay an interpreter tax.
- **Heavyweight browser engines** (`V8`, `JavaScriptCore`, `SpiderMonkey`) deliver top peak throughput — **but ship in multi-tens-of-megabyte binaries, take complex multi-step GN/CMake builds, and expose large, rapidly shifting C++ APIs** (historically forcing Node forks like `node-chakracore` to maintain brittle `v8.h` shims).

This fork intentionally occupies the **middle ground ("The Embeddable JIT Sweet Spot")**:

1. **Compact release artifact (~5.3 MB):** `zig build package` produces a self-contained, deterministic archive (`5,285,742` bytes observed on `darwin-arm64`) shipping `libChakraCore`, `libChakraNodeApi`, `ch`, `ch-nodeapi`, public headers, legal notices, and a SHA-256 `MANIFEST.json`.
2. **Sub-minute hermetic source build:** A single `zig build` front end (`Zig 0.17.0` pinned in `mise.toml`) owns both macOS ARM64 and Linux x64 profiles (`test`, `debug-static`, `release-shared`, `jit`), explicit `pal-config` checks (`95/95` on macOS), and host-`clang++` `-Dsanitize=address|undefined|thread` lanes (`26/26` legs clean on macOS ASan/UBSan).
3. **Dual stable flat C ABIs:** Embedders talk to **JsRT** (`237` exports with strict Phase-3 additive versioning in [modernization-plan.md](modernization-plan.md) §0.1), while native ecosystem addons talk to **`libChakraNodeApi`** (`161` exports on `modernize/phase4`: all `155` `NAPI_VERSION=10` symbols plus `6` host helpers, expanded from `150` at `v8` on `modernize/baseline`), loading compiled `.node` and `node-addon-api` packages with zero `v8.h` compatibility shims.
4. **Modern ES2025 + Multi-Tier JIT + WASM SIMD:** `98.89%` certified ES2025 core conformance (`79,648 / 80,544` scenarios), 3-tier execution (`Interpreter` -> `SimpleJit` -> `FullJit` backed by concurrent `Recycler` GC) across **macOS ARM64** and **Linux x64**, plus **WebAssembly 1.0 + Bulk-Memory + Multi-Value + NEON/SSE SIMD JIT** (`382/382` P1).

### Explicit Non-Goals (Protecting the Sweet Spot)

To keep the codebase maintainable by a small team/maintainer without regressing build speed, footprint, or correctness:

- **No `v8.h` / `ChakraShim` layer and no fork of upstream `nodejs/node`:** Native addon compatibility flows exclusively through the C **Node-API (`napi_*`)** ABI (`libChakraNodeApi`), never by emulating V8 C++ classes.
- **No C++->Zig rewrite of core engine internals:** Parser, bytecode emitter, `Interpreter`, `Backend` JIT tiers, and the `Recycler` GC (`Field(T*)` write barriers + 646 C++ `throw` unwind sites) remain C++ ([zig-strategy.md](zig-strategy.md), [memory-management.md](memory-management.md)). Zig is used only for hermetic builds, standalone host executables, and narrow **C-ABI leaf wedges**.
- **No speculative TurboFan-class JIT rewrite or 32-bit pointer-compression overhaul:** JIT improvements target correctness, bailout safety, and targeted fast-paths rather than matching a 50-engineer browser compiler team.
- **Honest external benchmarks & comparisons:** External engine comparisons `[UNVERIFIED EXTERNAL]` from dynamic web trackers remain informational (not project acceptance gates), and same-host A/B benchmark receipts ([perf-baseline.md](perf-baseline.md)) are never compared across different machines or runtimes without matched-host methodology.

---

## 2. Foundation Already in Tree (`40c2fe5be`)

| Capability Layer | Verified State on `modernize/baseline` (macOS ARM64; Linux x64 queued in `G0`) | Anchor |
|---|---|---|
| **Core JS (ECMA-2025)** | **98.89%** ES2025 edition (`79,648/80,544`), **86.41%** broad (`80,486/93,141`, incl. `Temporal` proposal); negative phase `8,741` passed / `0` shape suspects; `check` **2064/0**, `debug-static` **2116/0** | [baseline-register.json](../tools/test262/baseline-register.json), [open-cards.md](open-cards.md) |
| **Multi-Tier JIT & WASM** | ARM64 & x64 3-tier JIT (`check-fulljit` **615/0**); WASM 1.0 + Bulk-Memory + mixed-type Multi-Value + ARM64 NEON SIMD tier-up (`check-wasm` P1 **382/382**) | [arm64-macos-jit.md](arm64-macos-jit.md), [wasm-arm64-plan.md](wasm-arm64-plan.md) |
| **Node-API Provider & Host Contract** | `libChakraNodeApi`: `161` exports (`NAPI_VERSION=10`: `155` `napi_*`/`node_api_*` + `6` host, per-context cross-addon `Buffer` marker sharing), `19/19` probes, `5x` stress, `52/17/0` C matrix, `52/61/6` `node-addon-api` matrix; `NodeApiHostApi.h` `chakra_nodeapi_host_services` (`post_task`, `post_work`, `cancel_work`, `ref_loop`, `unref_loop`, `fatal_exception`, `uv_loop`) + `chakra_nodeapi_set_module_api_version` (`modernize/baseline` stays at `150` exports / `v8`) | [NodeApiHostApi.h](../lib/NodeApi/NodeApiHostApi.h), [node-api-receipt.md](node-api-receipt.md) |
| **Reference Test Host** | `ch-nodeapi` demonstrates `RTLD_GLOBAL` provider loading, multi-addon `--addon` `.node` `dlopen` (`v1..v10` + `napi_module_register`), LIFO cleanup hooks, FIFO async/worker dispatch, and microtask promise pumping | [ch-nodeapi.cpp](../tools/nodeapi/ch-nodeapi.cpp) |
| **Sanitizers & Packaging** | Host-`clang++` ASan & UBSan **26/26** legs clean (`thread` wired for Linux); `check-gcstress` clean; `zig build package` / `check-package` deterministic tarball + `MANIFEST.json` | [build.zig](../build.zig), [linux.zig](../tools/build/linux.zig), [b5-package.md](b5-package.md) |

---

## 3. Phase-4 Expansion Tracks (Ordered by ROI & Effort)

### Track `G0` — Immediate Gate: Linux x64 Final Revalidation & Two-Platform Release (`Days`)

- **Prerequisite to everything below:** Execute the single-pass 12-step runbook in [linux-handoff.md](linux-handoff.md) §0 on Ubuntu 24.04 x86_64:
  - StyleChecks, tool unit suites, `test` profile (`check`, `verify-bytecode`), variants (`-Djit=false`, `release-shared`, `debug-static`), `check-wasm` + `check-fulljit` + unbounded `check-gcstress`.
  - Host-`clang++` `-Dsanitize=address` (`26/26`), `-Dsanitize=undefined` (`26/26`), and Linux-only `-Dsanitize=thread` (TSan on `host_async_probe`, `s6-growable-cross-agent.js`, and `WScript.Broadcast`).
  - Full Node-API matrix on pinned Node `v22.22.3`, `check-package` (`linux-x64` tarball), certified Linux Test262 refresh, and committed Linux benchmark JSONs (`tools/bench/results/linux-x64-{jit,interp}.json`).
  - Verify green hosted GitHub Actions (`zig-macos` + `zig-linux`).

---

### Track `Q1` — Quick, High-ROI Engine, Addon & Zig-Wedge Wins (`1–2 Weeks`, Independent Slices)

#### `Q1.1` Node-API `v9` / `v10` Additive Expansion (`lib/NodeApi/`) — **COMPLETED on `modernize/phase4`**
*(Note: `modernize/baseline` remains at the `NAPI_VERSION=8` `150`-export baseline; the `Q1.1` expansion below is landed and verified on `modernize/phase4`.)*
- `NAPI_VERSION=9`: `node_api_symbol_for`, `node_api_create_syntax_error`, `node_api_throw_syntax_error`, `node_api_get_module_file_name`, plus versioned `napi_cannot_run_js` status during `Closing` (`v10` -> `napi_cannot_run_js`, `v9` -> `napi_pending_exception`, `<= v8` -> `napi_generic_failure`).
- `NAPI_VERSION=10`: `node_api_create_external_string_latin1`, `node_api_create_external_string_utf16` (`*copied = true` Recycler-copy fallback), `node_api_create_property_key_latin1`, `node_api_create_property_key_utf16`, `node_api_create_property_key_utf8`, `node_api_create_buffer_from_arraybuffer`, and weak optional `napi_ref` / `napi_delete_reference` finalizer cancellation semantics (`NestedWrap`).
- Multi-addon & host upgrades: indexed per-env addon / module registration and LIFO cleanup in `NodeApiHost.cpp` + `tools/nodeapi/ch-nodeapi.cpp` (repeatable `--addon`), `chakra_nodeapi_set_module_api_version` (`1..10`), and per-context `ContextBufferSymbol` marker sharing so Buffers cross `napi_env` boundaries cleanly.
- Verified gates (`modernize/phase4`, macOS ARM64): `gen_nodeapi_exports.py --check` & `check-nodeapi` (`155` `napi_*`/`node_api_*` + `6` host = `161` exports), `19/19` probes (`test` + `jit`) + `tools/nodeapi/run_probe.sh`, `5x` stress (`ALL PASS`), `run_matrix.py` (`52/17/0`), and `run_node_addon_api_matrix.py` (`52/61/6`). See [node-api-receipt.md](node-api-receipt.md) §14.

#### `Q1.2` Pragmatic ES2025 Conformance Tail (`98.89%` -> `~99.3%+`, ~3–5 Days)
From `out/test262-refresh-20261117/refresh-analysis.txt`, four high-density, low-risk clusters account for over ~220 of the remaining `896` edition-selected failures:
1. **`DataView` undefined `byteOffset` coercion (~34 scenarios):** Handle explicit `undefined` `byteOffset` per ES2016+ spec (`GetViewValue` / `SetViewValue` and constructor default offset) without throwing `TypeError: Required argument offset in DataView method is not specified`.
2. **Built-in function own-property key order & own `.name` (~60–80 scenarios):** Align older built-ins where `name` is missing as an own property or `length` precedes `name` inconsistently vs ES2015+ function property order.
3. **String-literal module export/import names (`arbitrary-module-namespace-names`, 9 scenarios):** Support quoted string `ModuleExportName` (`export { x as "a-b" }`, `import { "a-b" as x }`) in parser and `SourceTextModuleRecord`.
4. **ICU 79.1 Unicode property-escape table delta & surrogate-pair tail (112 scenarios):** Audit Test262 `2e0a5676` vs Unicode 18.0 property deltas and fix bounded surrogate-pair handling in `lib/Parser/RegexRuntime.cpp` (following the Wave 1 `#8e` investigation).

*(Deliberate policy retention: keep ES2015 Proper Tail Calls [`34` scenarios] unimplemented/excluded by policy, matching V8 and SpiderMonkey to preserve clean stack traces and JIT calling conventions.)*

#### `Q1.3` Bite-Sized ES2026 Stage-4 Built-ins + First Production `Z1` Zig Wedge (~1 Week)
*(Note: `RegExp.escape` `40/0` and `Atomics.pause` `10/0` already pass in-tree.)*
1. **`Error.isError(arg)` (24 Test262 scenarios):** Cross-realm internal error brand check via `JsIsError` / internal `JavascriptError` type check (including DOM/native error distinction edge cases).
2. **`Uint8Array` Base64 & Hex (`140` Test262 scenarios) — First Production `Z1` Zig C-ABI Wedge:**
   - Implement `Uint8Array.fromBase64`, `Uint8Array.prototype.toBase64`, `Uint8Array.prototype.setFromBase64`, `Uint8Array.fromHex`, and `Uint8Array.prototype.toHex` by putting the **UTF-8/ASCII <-> raw byte encoding/decoding & validation engine in a Zig static library** behind a strict flat C ABI (`zig_u8a_b64_encode`, `zig_u8a_b64_decode`, `zig_u8a_hex_encode`, `zig_u8a_hex_decode`).
   - **Why it is the ideal `Z1` wedge:** Zero `Recycler` pointers, zero C++ exceptions, bounded caller-owned byte slices (`uint8_t*` + `size_t`, matching the [memory-management.md](memory-management.md) §3 ownership record), trivial differential testing (`zig test` + fuzzing against Node/Python), and built-in SIMD potential without touching JIT/GC internals.
   - C++ `TypedArray` methods handle JS argument coercion (`GetOptionsObject`, `lastChunkHandling`, detached-buffer / GSAB bounds re-validation before/after allocation) and call the C ABI cleanly.
3. **`Array.fromAsync(asyncItems, mapfn, thisArg)` (`186` scenarios):** Bounded async-iterator/sync-iterator array builder following ES2026 Stage 4 promise/abrupt-completion closing order (`AsyncIteratorClose`).
4. **`Math.sumPrecise(iterable)` (`20` scenarios):** Exact compensated summation (Shewchuk-style exact accumulator) over arbitrary JS iterables with fast path for dense numeric arrays.

---

### Track `I1` — Modern `Intl` (ECMA-402) Wrapper Slices (`2–4 Weeks`)

Because both macOS (`icu4c` 79.1 via Homebrew) and Linux (`icu4c` 74.2+) already link full `libicui18n` and `libicuuc` into `libChakraCore`, ChakraCore can gain modern `Intl` classes **without adding any new third-party dependencies**—only thin C++ wrappers over existing ICU C/C++ APIs plus ICU availability probes:
- **`Intl.Locale`:** BCP 47 tag parsing/canonicalization/likely-subtags over `icu::Locale` / `uloc_*`.
- **`Intl.ListFormat`:** Locale-aware conjunction/disjunction/unit formatting over `icu::ListFormatter`.
- **`Intl.RelativeTimeFormat`:** Over `icu::RelativeDateTimeFormatter`.
- **`Intl.DisplayNames`:** Language, region, script, currency, calendar display names over `icu::LocaleDisplayNames` / `ucurr_getName`.
- **`Intl.Segmenter`:** Grapheme, word, and sentence break iteration over `icu::BreakIterator` (`ubrk_open`).
- Add an opt-in `tools/test262` `intl402` campaign lane to verify ECMA-402 parity against Node's pinned ICU behavior.

---

### Track `H1` — Flagship Embedder Proof: Lightweight Zig Runtime Host ("Option B", `4–8 Weeks`)

**Architectural boundary:** `H1` lives in a **separate host directory (e.g., `host/zig/`)** built via an opt-in `zig build runtime` step. It links **only public `JsRT` (`ChakraCore.h` / `ChakraCommon.h`) + `NodeApiHostApi.h` (`libChakraNodeApi`) + `libuv`**, proving external embeddability without adding a single host API or Node coupling into `libChakraCore`.

#### `H1.1` Hermetic `libuv` + Event Loop & `HostServices` (`~1.5k–2.5k` LOC Zig, ~1–2 Weeks)
- Compile `libuv` directly in `build.zig` / `tools/build/linux.zig` (~45 C sources on Darwin/Linux; zero system package-manager prerequisite beyond OS libc).
- Implement `chakra_nodeapi_host_services` ([NodeApiHostApi.h](../lib/NodeApi/NodeApiHostApi.h#L101-L121)) in Zig:
  - Inject real `uv_loop_t*` so `napi_get_uv_event_loop` succeeds cleanly for real `.node` addons.
  - Wire `post_task` via a mutex-guarded FIFO queue + `uv_async_t`; wire `post_work`/`cancel_work` via `uv_queue_work` / `uv_cancel`; wire `ref_loop`/`unref_loop` to `uv_ref`/`uv_unref`.
  - Run the unified Node tick cycle: `process.nextTick` queue -> Chakra promise microtasks (`JsSetPromiseContinuationCallback`) -> `uv_run` timers (`setTimeout`/`setInterval` + `JsSetTimerContinuationCallback` hook) / I/O / `setImmediate`, draining microtasks after every host callback.
  - Deterministic shutdown: drain async cleanup hooks -> `chakra_nodeapi_destroy_env` -> `JsSetCurrentContext(null)` -> `JsDisposeRuntime`.

#### `H1.2` Node ESM + CommonJS Module & Multi-Addon Loader (`~1.5k–2k` LOC Zig, ~1 Week)
- Implement Node module resolution in Zig: `node:*` built-in specifier handling, relative/file extensions (`.mjs`, `.cjs`, `.js`, `.json`, `.node`), `node_modules` directory walk, and `package.json` (`type`, `main`, `exports`, `imports`, conditional `"node"`/`"import"`/`"default"`).
- Wire native ESM + `import()` + `import ... with { type: "json" }` through `FetchImportedModuleFromScriptWithAttributesCallBack`.
- Wrap `.cjs` execution (`(function(exports, require, module, __filename, __dirname) { ... })`), support `dlopen` of multiple `.node` addons sharing one `napi_env`, and provide synthetic module namespace bridging for `import` of CJS modules.

#### `H1.3` Web Globals & Core `node:*` Standard Library (`~5k–8k` LOC Zig + Embedded JS, ~2–4 Weeks)
Following the pattern proven in modern minimal runtimes: keep **low-level syscalls/crypto/parsing in memory-safe Zig** exported as host callbacks, and put **ergonomic JS surfaces** (`EventEmitter`, `Buffer` slices, `Readable`/`Writable` streams, `path` string helpers) in embedded precompiled JS:
- **Globals:** `console`, `process` (`argv`, `env`, `cwd`, `exit`, `nextTick`, `hrtime`), `Buffer`, `TextEncoder`/`TextDecoder`, `URL`/`URLSearchParams`, `AbortController`, `structuredClone`, `crypto` (`std.crypto` in Zig), and `fetch` (Zig `std.http.Client` / TLS or `libuv` + mbedTLS/OpenSSL).
- **`node:*` built-ins:** `node:fs` & `node:fs/promises` (`uv_fs_*`), `node:path`, `node:os`, `node:util`, `node:events`, `node:assert`, `node:stream`, `node:net`, `node:http` (Zig HTTP parser + `uv_tcp_t` -> `http.createServer` capable of serving Express/Hono), `node:child_process` (`uv_spawn`).

#### `H1.4` Optional Runtime Extensions (`2–3 Weeks` Later)
- **`node:worker_threads`:** Dedicated OS thread per worker owning an independent `JsRuntimeHandle` + `napi_env` (per ChakraCore's thread-affinity rules in [memory-management.md](memory-management.md) §2), exchanging messages via `postMessage` and sharing memory cleanly through `ESSharedArrayBuffer` / `Atomics`.
- **CDP Inspector Bridge:** Small (~1.5k LOC) Zig WebSocket + JSON-RPC adapter translating Chrome DevTools Protocol (`Debugger.*`, `Runtime.*`) calls to ChakraCore's `JsDiag*` C API (`lib/Jsrt/ChakraDebug.h`).

---

### Track `C3` — Deferred Epics & Smart Shortcuts (Opportunistic / Out-of-Timeframe Now)

1. **`Explicit Resource Management` (`using` / `await using`, 842 scenarios):** High complexity across parser, bytecode unwind tables, async generators, and `DisposableStack`; schedule separately.
2. **`Temporal` (`9,226` scenarios):** **Do not write calendar arithmetic from scratch in C++** — evaluate wrapping an MIT/Apache-2.0 C-ABI library such as **`temporal_rs`** (`diplomat` C headers) compiled alongside `build.zig`, mirroring the `Z1` C-ABI strategy.
3. **WASM 2.0 native-compiler parity:** Adding `wasm-tail-call`, `extended-const`, and multi-table `reference-types` once the current `382/382` P1 + SIMD baseline completes Linux x64 proof.
4. **Windows `zig build` (`B6`):** Wire Windows targets in `build.zig`, resolve the documented MSVC ARM64 `InterpreterStackFrame.cpp` guard ([open-cards.md](open-cards.md) §10), and run `NodeApi` + `INTL_WINGLOB` matrices under MSVC/Windows.
5. **Peak JIT profiling & CI performance gate:** Wire `tools/bench/run.py` into regression tracking for `modernize/baseline` without attempting to clone V8's 4-tier architecture.
