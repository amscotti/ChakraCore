# Project Vision & Phase-4 Roadmap — The Embeddable JIT Sweet Spot

> **Status (2026-11-18, `modernize/baseline` at `40c2fe5be`; `modernize/phase4` `Q1.1` + `Q1.2` landed):** Tiers 1–5 pre-Linux modernization work is complete on `modernize/baseline` on macOS ARM64 (`98.89%` certified ES2025 edition conformance, multi-tier JIT + WASM NEON SIMD, `libChakraNodeApi` v8, ASan/UBSan sanitizer lanes, and deterministic `b5-package` tarballs). On `modernize/phase4`, Tracks **`Q1.1`** (Node-API v9/v10 + multi-addon support, `161` exports) and **`Q1.2`** (`99.05%` certified ES2025 edition conformance, `79,781 / 80,544`) are complete. This document defines where the fork is positioned going forward and organizes the remaining Phase-4 expansion tracks alongside [linux-handoff.md](linux-handoff.md) §0 Linux x64 revalidation.
>
> Companion Docs: [modernization-plan.md](modernization-plan.md), [zig-strategy.md](zig-strategy.md), [node-api-plan.md](node-api-plan.md), [node-api-receipt.md](node-api-receipt.md), [memory-management.md](memory-management.md), [b5-package.md](b5-package.md), [linux-handoff.md](linux-handoff.md), [open-cards.md](open-cards.md).

---

## 1. Strategic Positioning — The Embeddable JIT Sweet Spot

JavaScript engines tend to cluster at two extremes:

- **Lightweight interpreter engines** (`QuickJS-NG`, `Boa`, `LibJS`) are compact and fast to compile from source, making them great for small embeds — **but without a full optimizing multi-tier JIT and concurrent collector**, compute-heavy JavaScript and WebAssembly workloads pay an interpreter tax.
- **Heavyweight browser engines** (`V8`, `JavaScriptCore`, `SpiderMonkey`) deliver top peak throughput — **but ship in multi-tens-of-megabyte binaries, take complex multi-step GN/CMake builds, and expose large, rapidly shifting C++ APIs** (historically forcing Node forks like `node-chakracore` to maintain brittle `v8.h` shims).

This fork intentionally occupies the **middle ground ("The Embeddable JIT Sweet Spot")**:

1. **Compact release artifact (~5.3 MB):** `zig build package` produces a self-contained, deterministic archive (`5,293,501` bytes verified at `Q1.1` on `modernize/phase4` `darwin-arm64`; `5,285,742` bytes on `modernize/baseline` `40c2fe5be`) shipping `libChakraCore`, `libChakraNodeApi`, `ch`, `ch-nodeapi`, public headers, legal notices, and a SHA-256 `MANIFEST.json`.
2. **Sub-minute hermetic source build:** A single `zig build` front end (`Zig 0.17.0` pinned in `mise.toml`) owns both macOS ARM64 and Linux x64 profiles (`test`, `debug-static`, `release-shared`, `jit`), explicit `pal-config` checks (`95/95` on macOS), and host-`clang++` `-Dsanitize=address|undefined|thread` lanes (`27/27` legs clean on `modernize/phase4` macOS ASan/UBSan, adding `nodeapi:v10_multi_addon_probe`; `26/26` on `modernize/baseline`).
3. **Dual stable flat C ABIs:** Embedders talk to **JsRT** (`237` exports with strict Phase-3 additive versioning in [modernization-plan.md](modernization-plan.md) §0.1), while native ecosystem addons talk to **`libChakraNodeApi`** (`161` exports on `modernize/phase4`: all `155` `NAPI_VERSION=10` symbols plus `6` host helpers, expanded from `150` at `v8` on `modernize/baseline`), loading compiled `.node` and `node-addon-api` packages with zero `v8.h` compatibility shims.
4. **Modern ES2025 + Multi-Tier JIT + WASM SIMD:** `99.05%` certified ES2025 core conformance (`79,781 / 80,544` scenarios on `modernize/phase4`; `98.89%` / `79,648 / 80,544` on `modernize/baseline`), 3-tier execution (`Interpreter` -> `SimpleJit` -> `FullJit` backed by concurrent `Recycler` GC) across **macOS ARM64** and **Linux x64**, plus **WebAssembly 1.0 + Bulk-Memory + Multi-Value + NEON/SSE SIMD JIT** (`382/382` P1).

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
| **Node-API Provider & Host Contract** | `libChakraNodeApi`: `150` exports (`NAPI_VERSION=8`), `18/18` probes, `5x` stress, `46/23/0` C matrix, `52/61/6` `node-addon-api` matrix; `NodeApiHostApi.h` `chakra_nodeapi_host_services` (`post_task`, `post_work`, `cancel_work`, `ref_loop`, `unref_loop`, `fatal_exception`, `uv_loop`) | [NodeApiHostApi.h](../lib/NodeApi/NodeApiHostApi.h), [node-api-receipt.md](node-api-receipt.md) |
| **Reference Test Host** | `ch-nodeapi` demonstrates `RTLD_GLOBAL` provider loading, `.node` `dlopen`, FIFO async/worker dispatch, and microtask promise pumping | [ch-nodeapi.cpp](../tools/nodeapi/ch-nodeapi.cpp) |
| **Sanitizers & Packaging** | Host-`clang++` ASan & UBSan **26/26** legs clean (`thread` wired for Linux); `check-gcstress` clean; `zig build package` / `check-package` deterministic tarball (`5,285,742` bytes) + `MANIFEST.json` | [build.zig](../build.zig), [linux.zig](../tools/build/linux.zig), [b5-package.md](b5-package.md) |

> **Phase-4 Delta (`modernize/phase4` `Q1.1` + `Q1.2` landed; `modernize/baseline` stays at `40c2fe5be`):**
> - **`Q1.1`:** `libChakraNodeApi` expanded to `NAPI_VERSION=10` (**`161`** exports = `155` v10 + `6` host, adding `chakra_nodeapi_set_module_api_version`, multi-addon `--addon` `ch-nodeapi`, and per-context cross-addon `Buffer` marker sharing); **`19/19`** probes (`test` + `jit`), **`5x`** stress (`ALL PASS`), **`52/17/0`** C matrix, **`52/61/6`** `node-addon-api` matrix, **`27/27`** ASan & UBSan legs clean (including `nodeapi:v10_multi_addon_probe`), and `zig build check-package` (`5,293,501` bytes verified at `Q1.1`).
> - **`Q1.2`:** Certified ES2025 edition conformance rose from **`98.89%` (`79,648 / 896` of `80,544`)** to **`99.05%` (`79,781 / 763` of `80,544`)** (`-133` edition failures; broad **`86.41%` -> `86.56%`**, `80,486/12,655` -> **`80,619/12,522`** of `93,141`; `engine-defect-on-existing` `846 -> 722`, `required-feature-missing` `34 -> 25`, `0` negative-phase shape suspects, `0` engine regressions). Mac multi-profile gates: `verify-bytecode` exit 0, `check` **`2068/0`**, `debug-static check` **`2120/0`** (`8/8` static native), `release-shared` + `check` PASS, `jit` `check-fulljit` **`617/0`**, `check-wasm` **`382/382`**, `jit check` PASS.

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
- Verified gates (`modernize/phase4`, macOS ARM64): `gen_nodeapi_exports.py --check` & `check-nodeapi` (`155` `napi_*`/`node_api_*` + `6` host = `161` exports), `19/19` probes (`test` + `jit`) + `tools/nodeapi/run_probe.sh`, `5x` stress (`ALL PASS`), `run_matrix.py` (`52/17/0`), `run_node_addon_api_matrix.py` (`52/61/6`), `27/27` ASan & UBSan legs clean (with `nodeapi:v10_multi_addon_probe`), and `check-package` (`5,293,501` bytes). See [node-api-receipt.md](node-api-receipt.md) §14.

#### `Q1.2` Pragmatic ES2025 Conformance Tail (`98.89%` -> `99.05%` on `modernize/phase4`) — **COMPLETED on `modernize/phase4`**
*(Note: `modernize/baseline` remains at `40c2fe5be` `98.89%` [`79,648/896`]; the `Q1.2` conformance closures and gate results below are landed and verified on `modernize/phase4`.)*
- **Certified Test262 refresh (`out/test262-refresh-phase4-q12` vs `out/test262-refresh-20261117`, frozen `edition-policy.json`, corpus `2e0a5676`):**
  - **ES2025 edition:** `79,648 passed / 896 failed` (`98.89%`) -> **`79,781 passed / 763 failed` (`99.05%`)** across `80,544` scenarios (`+133` net passes; `-133` net failures).
  - **Broad:** `80,486 passed / 12,655 failed` (`86.41%`) -> **`80,619 passed / 12,522 failed` (`86.56%`)** across `93,141` scenarios (`48,598` files).
  - **Failure buckets:** `engine-defect-on-existing` `846 -> 722` (`-124`), `required-feature-missing` `34 -> 25` (`-9`), `post-ES2025` `11,759`, `host` `16`. Negative-phase audit: `8,741` passed negatives (`parse` `8,650`, `resolution` `33`, `runtime` `58`), **`0` shape suspects**, `1` marker downgrade.
  - **Per-key join:** `138` fail->pass (`135` Q1.2 fixes + `3` `Atomics/waitAsync` scheduler flakes) and `5` pass->fail — all `5` are the known `Atomics/waitAsync/*/no-spurious-wakeup-on-*.js` `lapse >= TIMEOUT` parallel-scheduler timing flakes ([open-cards.md](open-cards.md) §1; re-checked at `--jobs 1` with `7/8` passing and the remaining 1 also `lapse >= TIMEOUT`), confirming **`0` engine pass->fail regressions**.
- **Per-cluster closures (`modernize/phase4`):**
  1. **`DataView` `undefined` `byteOffset` & ES2025 `GetViewValue`/`SetViewValue` (`+36` scenarios; `DataView` directory `1,086/1,122 -> 1,122/1,122`):** Removed legacy missing-argument throws across all 8 legacy getters/setters (`{Int8,Uint8,Int16,Uint16,Int32,Uint32,Float32,Float64}`), defaulted omitted `offset`/`value` to `undefined`, unified `EntryGetValue`/`EntrySetValue` (plus `EntryGetFloat16` and `#ifdef _M_ARM` specializations) so all user-observable coercions (`ToIndex`, value conversion, `ToBoolean(isLittleEndian)`) execute before `CheckViewRange` post-coercion buffer detachment/bounds validation.
  2. **`Reflect.construct`, `JavascriptProxy` `[[Construct]]`, & `BoundFunction` ordering (`+18` scenarios: `Reflect.construct` `994/12 -> 1,006/0`, `Proxy/construct` `58/58`, `Reflect/construct` `20/20`, `resizable-arraybuffer` `926/4 -> 930/0` edition / `936/0` broad):** Updated `EntryConstruct` + `CalloutHelper<true>` + `JavascriptProxy::FunctionCallTrap` to extract `argArray` elements before `CreateFromConstructor`, walk `BoundFunction::GetDirectTargetFunction()` chains with `SameValue(boundFn, newTarget) -> targetFn`, and skip pre-allocating `CreateFromConstructor` for `SkipDefaultNewObject` built-ins, class constructors, and `JavascriptProxy` targets — closing `e10-§4.2` (`ArrayBuffer` & `SharedArrayBuffer` `options-maxbytelength-compared-before-object-creation.js`, `4`), `DataView/byteOffset-validated-against-initial-buffer-length` (`2`), `Promise/get-prototype-abrupt-executor-not-callable.js` (`2`), `Promise/prototype/finally/{rejected,resolved}-observable-then-calls-argument.js` (`4`), `Function/prototype/bind/instance-construct-newtarget-{boundtarget-bound,self-reflect}.js` (`4`), and `TypedArrayConstructors/ctors/typedarray-arg/throw-type-error-before-custom-proto-access.js` (`2`).
  3. **Function own-property key order, `.name`, `%ThrowTypeError%`, `Proxy.revocable`, & Promise helpers (`+64` scenarios):** Reordered `Object` and `Promise` constructors (`length`, `name`, `prototype`, `[Symbol.species]`); froze `%ThrowTypeError%` (`length: 0`, `name: ""`, `Object.isFrozen === true`; `ThrowTypeError/*` `10/10`); switched `Proxy.revocable` `revoke` (`6/6`), Promise internal executor/resolve/reject/all/allSettled/any/finally/async-iterator helpers (`+34` scenarios), and anonymous JS functions/arrows/generators/async functions (`+12` `language/expressions/*/name.js`) to `length` -> `name` (`""` or computed name, updating repeated computed-name targets in `SetComputedNameVar`); aligned `test/Function/typeErrorAccessor.baseline`, `test/es6/{function.name.js,ES6Promise.js,proxytest9.baseline}`, and `test/Debugger{,Common}/{dumpFunctionProperties,indexprop}.js.dbg.baseline`.
  4. **String-literal `ModuleExportName` (`arbitrary-module-namespace-names`, `7/16 -> 16/16`):** Added `IsStringWellFormedUnicode` & `ParseModuleExportName` (rejecting strict octal escapes and unpaired UTF-16 surrogates while preserving embedded NULs), replaced `PropertyIds::star_` with `ModuleRecordBase::NamespaceBindingPropertyId` (`Constants::NoProperty`) + `isNamespaceStar`, and added integer-index + aliased re-export lookup in `ModuleNamespace`.
  5. **`RegExp` & Unicode conformance (`+10` scenarios: `Symbol.replace` `186/4 -> 190/0`, `regexp-named-groups` `198/2 -> 200/0`, `exec/{failure,success}-lastindex-access` `4`, `unicode_full_case_folding` `2`):** Updated `MappingSource::CaseFolding` to ICU 79.1 `C`+`S` equivalence classes including cross-plane `{0x00DF, 0x1E9E, 0x1DF95}`, routed `/u`+`/v` all-BMP negated surrogate-pair classes through `MatchCodePointSetNode` (`noHalfPairFallback = true`), fixed `AdvanceStringIndexLength`/`AdvanceLastIndexLength` (`2^53-1`), moved `namedCaptures` `ToObject` into the string-substitution path, and unconditionally invoked `ToLength(lastIndex)` under `ES6RegExSymbols` on `exec`/`test`.
- **Unicode 18.0 (`ICU 79.1`) vs Test262 `2e0a5676` (`Unicode 17.0.0`) audit:** Verified on `RegExp/property-escapes/**` (`1,114/1,226`, `56` failing files / `112` scenarios): under `node` (`v26.5.0`, ICU `78.3` = Unicode 17.0) **56/0** pass; under `ch` (ICU `79.1` = Unicode 18.0) 100% of the `56` failing files fail strictly on Unicode 18.0 data deltas (`49` `age=18.0` code points `U+0558`, `U+05C8`, `U+0B53`, `U+0B54`, `U+1ADE`, `U+208F`, `U+209D`, `U+20C2`, `U+2E60`, `U+2E62`, `U+2E63`, `U+A7DD`, and astral `U+1F1AE` in `Extended_Pictographic.js`; `3` `Changes_When_{Casemapped,Titlecased,Uppercased}` on `U+0277`; `3` `Script_Extensions_-_{Kannada,Malayalam,Telugu}` on `U+0B83`; `1` `Script_Extensions_-_Devanagari` on `U+1CF5..U+1CF6`), plus `34` additional `property-escapes/generated/*.js` files (`68` scenarios) with Unicode 18.0 astral-only additions in `nonMatchSymbols` currently masked by Option A's `\P{}` half-pair fallback (`90` total files differing from Unicode 17.0).
- **Verified gates (`modernize/phase4`, macOS ARM64):** `verify-bytecode` exit 0 (source headers unchanged); `zig build check` **`2068/0`** (+ `test-shared-basic` PASS); `zig build -Dprofile=debug-static check` **`2120/0`** (+ `8/8` static native); `zig build -Dprofile=release-shared` + `check` PASS; `zig build -Dprofile=jit check-fulljit` **`617/0`**; `zig build -Dprofile=jit -Dwasm=true check-wasm` **`382/382`** P1; `zig build -Dprofile=jit -Dwasm=true check` PASS (`check_jit` + JIT bytecode verify).

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
