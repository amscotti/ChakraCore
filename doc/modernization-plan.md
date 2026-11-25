# ChakraCore Modernization Plan

> **Authoritative live status register:** **[open-cards.md](open-cards.md)** is the single live source of truth for current gate counts, Test262 scores, and open/closed items across `modernize/phase4` (Waves 1–5), paired with [vision-and-roadmap.md](vision-and-roadmap.md) for Phase-4 tracks. Dated milestone summaries and §6 receipts below preserve historical checkpoint records; current capability facts at HEAD are reconciled consistently throughout.

> **Baseline Tier 1–5 snapshot (2026-11-17, `modernize/baseline` `40c2fe5be`; superseded on `modernize/phase4` by `Q1.1`–`Q3` and Waves 1–5 in [open-cards.md](open-cards.md)):** Tiers 1–5 pre-Linux work completed on macOS ARM64 (`2064/0` `test`, `2116/0` `debug-static`, `382/382` P1 `check-wasm`, `615/0` `check-fulljit`, `98.89%` ES2025 `79,648/80,544`). On `modernize/phase4`, macOS ARM64 stands at **`99.57%` certified ES2025 (`80,195/80,544`)** and **`88.97%` broad (`82,866/93,141`)**, with regression pins **`2110/0` `test`**, **`2162/0` `debug-static`**, and **`643/0` `check-fulljit`** (`tools/build/gate_counts.json`).
>
> - **Tier 1 (sanitizers & gates):** `-Dsanitize=address|undefined|thread` lanes (`thread` Linux-only; Mac `address` and `undefined` lanes configured and active — `32/32` legs on `test` and `34/34` on `jit -Dwasm=true` via hardened `check_sanitize.py`), gate targets `check-wasm` / `check-wasm-jit` (P1 `382/382`), `check-gcstress`, `check-fulljit` (`643/0`), `check-perf-smoke` (Wave 4), and `regenByteCode.py` create-on-refuse guard.
> - **Tier 2 (RegExp):** `ES6RegExSymbols` and `ES6RegExPrototypeProperties` are **default-on** (`true` in `lib/Common/ConfigFlagsList.h:689-700`); `Lower.cpp` JIT fast-path guards (`GenerateFastReplace`, `GenerateFastInlineStringSplitMatch`), stack regex boxing, and legacy test flag pinning.
> - **Tier 3 (conformance/engine):** Array/%TypedArray% `toLocaleString(locales, options)` forwarding; integer-index `configurable: true`; `Reflect.set` canonical numeric conversion; `unshift` indexed setter; `join` length capture; `ModuleNamespace` strict `[[Set]]`/`[[Delete]]`; duplicate lexical declaration errors; labelled import/export early errors; anonymous default export names; Tarjan SCC TLA cycle settlement; strict `08`/`09` and template `\08`/`\09`; `NamedEvaluation` for `o[expr] = class{}`; cross-realm derived constructor (`e3-R3`); indirect `eval` (`e3-R4`); and UBSan/ASan engine fixes.
> - **Tier 4 (WASM/JIT):** `check-wasm` (`382/382` P1) and `check-wasm-jit` cover **MVP + sign-extension + nontrapping conversions only** (`test/wasm-arm64-manifest.json`); mixed-type multi-value (`-WasmMultiValue`, off by default, P3 `test/WasmSpec.MultiValue`) and ARM64 WASM NEON SIMD (`-WasmSimd`, experimental, off by default, P3 `test/wasm.simd` `14/14` interp + JIT after Wave 3 `J-1`/`J-2` fixes) are gated outside P1; **bulk-memory (`0xFC 0x0A`/`0x0B`) is not implemented**.
> - **Tier 5 (packaging):** release packaging (`tools/package/*`), `build.zig` / `tools/build/linux.zig` / `tools/build/windows.zig` `package` and `check-package` targets, bundled macOS ICU dylibs & prefix isolation (Wave 2), and deterministic `MANIFEST.json` archive.

> **Current Program Summary (reconciled with Waves 1–5):** **F0–B3 verified**, **B4 native proof completed (`2026-11-09/10`, [b4-linux-run.md](b4-linux-run.md))**, **G0 Linux baseline completed (`1bf42d1f6`, merged into `modernize/phase4`)**, and **macOS ARM64 `modernize/phase4` certified at `99.57%` ES2025 (`80,195/80,544`) / `88.97%` broad (`82,866/93,141`)**. Hosted GitHub Actions Zig workflows (`.github/workflows/zig-{macos,linux,windows}.yml`) are live on `master` and `modernize/**` with Waves 1–4 remediating the `B-15..B-18` and `B-4` matrix gaps (`G0-phase4` Linux and `B6` Windows native CI runs carry Phase-4 cross-platform revalidation). Active Mac sanitizer lanes (`-Dsanitize=address|undefined`) pass `32/32` (`test`) and `34/34` (`jit -Dwasm=true`) under `check_sanitize.py` (Mac `-Dsanitize=thread` remains unconfigured / Linux-only); `ES6RegExSymbols` and `ES6RegExPrototypeProperties` are **default-on** (`true` in `ConfigFlagsList.h:689-700`); CMake/legacy `build.sh` do not link `lib/ZigCodec` or build `libChakraNodeApi`/`ch-nodeapi` and are retained solely as historical reference; and Wave 4 added `tools/bench/check_perf_smoke.py` (`check-perf-smoke`). `JsrtExternalArrayBuffer` finalizer native tests and the Phase-3 JsRT additive API versioning contract (§0.1) are landed and documented. This is a staged roadmap for a small maintained fork, not a dated delivery promise.

## Read this first

The direction is attainable when each milestone produces a usable, tested result:

1. **Own the build:** implement one clean Mac configuration, prove its checks,
   then add Linux with JIT. Roll out the new build per verified platform.
2. **Use Zig selectively:** prove one small C-ABI integration when it provides
   value. JavaScript improvements can proceed in the existing C++ engine.
3. **Improve compatibility incrementally:** qualify the conformance runner,
   triage failures, and deliver bounded feature groups. Full ES2025 remains the
   long-term core-language target, reached across releases.

Terminology: **PAL** is the operating-system compatibility layer; **JsRT** is the
C embedding API; **JIT** compiles JS to native code; **WABT** supplies WASM tooling.
A **qualified suite** means its edition selection, host behavior and pass/fail
handling have been validated before its results are used as a completion gate.

| Milestone | Outcome | Depends on | Status |
|---|---|---|---|
| F0 | Compiler, behavior-parity and diagnostic-conformance foundation | — | Verified |
| B1 | Explicit Mac build/PAL configuration contract | F0 | Verified |
| B2 | Source-only Mac `zig build` and `zig build check` | B1 | Verified |
| B3 | Mac build profiles, incremental correctness and CI | B2 | **Verified** (hosted `.github/workflows/zig-macos.yml` live; Wave 1–4 remediated `B-15..B-18` & `B-4`) |
| B4 | Native x64 Linux JIT/WASM/WABT proof and Zig build | B2; B3 before default rollout | **Completed baseline (`G0` `1bf42d1f6`, merged into `modernize/phase4`)**; Phase-4 `G0-phase4` queued ([linux-handoff.md](linux-handoff.md) §0A) |
| B5 | First release package with controlled dependencies | B3; B4 for a two-platform package | **Verified on macOS** (`check-package`, Wave 2 bundled ICU + prefix isolation, [b5-package.md](b5-package.md)); Linux/Windows runs queued |
| B6 | Windows feasibility and rollout; later target extensions | Stable build contracts | **Wired on `modernize/phase4`** (`windows.zig`/`tables_windows.zig` + `zig-windows.yml`; awaiting native Windows certification) |
| Z1 | One useful mixed C++/Zig unit behind a tested C ABI | B3; B4 if the unit ships on Linux | **Completed (`Q1.3`)** (`lib/ZigCodec/u8a_codec.zig`, [z1-u8a-codec.md](z1-u8a-codec.md)) |
| C1 | Qualified ES2025 test policy and triaged failures | Stable Mac build at B3 | **Verified** (`-NegativePhaseReport` host-reported phase landed; [c1-triage.md](c1-triage.md), [c1-negative-phase.md](c1-negative-phase.md)) |
| C2 | Bounded compatibility batches and partial releases | C1; B4 before JIT-wide enablement | **Completed on macOS ARM64** (`ESBigInt`, `ESSharedArrayBuffer`, `ES6RegExSymbols`, `ES6RegExPrototypeProperties` all default-on `true` in `ConfigFlagsList.h`; `2110/0` `test`, `2162/0` `debug-static`, `643/0` `check-fulljit`, `382/382` P1 `check-wasm`; live status in [open-cards.md](open-cards.md)) |
| C3 | Larger runtime/object-model feature projects | C1 and their feature prerequisites | `C3.1` Explicit Resource Management **completed** (`947/0`); `Temporal` / WASM 2.0 remain separate tracks |
| C4 | Verified full ES2025 core-language result | Required C2/C3 work and qualified runner | **99.57% certified ES2025 (`80,195/80,544`) & 88.97% broad (`82,866/93,141`) on macOS ARM64 (`Q3`)** (`98.89%` / `79,648` on `modernize/baseline`); Linux/Windows re-verify queued |
| G0 | Single-pass Linux x64 revalidation & matched two-platform `b5` release | B4, B5, C4 | **Baseline completed (`1bf42d1f6`)**; `G0-phase4` queued ([linux-handoff.md](linux-handoff.md) §0A) |
| Q1 | Quick wins: Node-API v9/v10 (`Q1.1`), ES2025 sweeps (`Q1.2`/`Q1.4`/`Q1.5`), ES2026 waves (`Q1.3`/`Q2`/`Q3`) + `Z1` Base64/Hex | G0 (or branch) | **Completed on `modernize/phase4`** ([vision-and-roadmap.md](vision-and-roadmap.md) §3, [open-cards.md](open-cards.md)) |
| I1 / I2 | Modern ECMA-402 `Intl` wrappers over linked C `icu4c` (`Locale`, `ListFormat`, `RelativeTimeFormat`, `DisplayNames`, `Segmenter`, `supportedValuesOf`, `DurationFormat`) | B3/B4 ICU contracts | **Completed on `modernize/phase4`** (`I3` legacy `Intl.js` replacement deferred; [vision-and-roadmap.md](vision-and-roadmap.md)) |
| H1 | Standalone Zig + `libuv` + `libChakraNodeApi` lightweight runtime host ("Option B") | `JsRT` + `libChakraNodeApi` + `Q1.1` | Planned separate host ([vision-and-roadmap.md](vision-and-roadmap.md) §3) |

**Default execution order:** F0 → B1 → B2 → B3 → B4. Start C1 after B3;
release/dependency work at B5 and an approved Z1 pilot can then be scheduled without
holding up C++ compatibility fixes. B6 is an expansion decision, not a prerequisite
for completing the first Mac/Linux build rollout. All entries marked planned are
future work; command names below describe the intended build interface.
(Hosted Zig CI workflows — `.github/workflows/zig-{macos,linux,windows}.yml` — are live
on `master` and `modernize/**`, and Waves 1–4 remediated the `B-15..B-18` and `B-4`
matrix gaps; see [open-cards.md](open-cards.md) for current CI status.)

## 0. Goals and boundaries

Goals:

- Keep a small embeddable engine with the flat JsRT C API, TTD debugging and
  JavaScript/Intl/WASM functionality on configurations that actually support it.
- Use Zig 0.17.0 as the build front end and C/C++ toolchain, preserving engine
  behavior while declaring platform floors and dependencies explicitly.
- Replace legacy entry points platform by platform once the replacement has
  evidence. Maintain a working build and usable partial releases throughout.
- Introduce Zig implementation code through narrow C-ABI slices where it improves
  maintainability, tooling or a measured requirement.
- Target full **ES2025 (ECMA-262)** coverage, while measuring and shipping smaller
  compatibility gains along the way.

Boundaries:

- Phases 1–2 preserve public header spellings and the JsRT ABI. New runtime
  feature/host requirements in Phase 3 follow the **Phase-3 JsRT additive API
  versioning contract** below.
- DOM/Node hosting, JIT/GC rewrites, V8/JSC internal-design parity and a language
  migration percentage are outside this roadmap's completion gates.
- Intl/ECMA-402 and WASM extensions have their own milestones and coverage claims.
  Their entire future feature sets do not gate ES2025 core-language completion.
- Modern OS/tool floors may be raised deliberately. A supported-platform claim
  requires a declared contract and testing on the relevant platform/floor.

### 0.0 Strategic Positioning — The Embeddable JIT Sweet Spot

This fork intentionally targets the **embeddable JIT sweet spot** between tiny
interpreter-only engines (fast to compile and small, but lacking a full
optimizing JIT + concurrent collector) and heavyweight browser engines
(maximum peak speed, but multi-tens-of-MB binaries, sprawling builds, and
volatile C++ embedding APIs):

- **Small package, fast hermetic build:** `~5.5 MB` deterministic
  `zig build package` archives on `modernize/phase4` (`~5.3 MB` on
  `modernize/baseline`; `darwin-arm64` verified, `linux-x64` `G0` baseline
  verified / `G0-phase4` queued, `windows-x64` `B6` wired) built in under a
  minute from a single `zig build` front end (`Zig 0.17.0`).
- **Dual flat C ABIs:** Embedders use versioned `JsRT` (`237` exports) while
  native addons use `libChakraNodeApi` (**`161` exports / Node-API v10
  (`-DNAPI_VERSION=10`)** on `modernize/phase4`, matching [README.md](../README.md);
  note upstream `include/nodeapi/js_native_api_types.h` defines
  `#define NAPI_VERSION 8` as default when `-DNAPI_VERSION` is not passed;
  `modernize/baseline` stays at `150` exports / v8), with explicit
  **non-goals** against `v8.h` C++ shims, upstream `nodejs/node` forks,
  or C++->Zig core rewrites ([zig-strategy.md](zig-strategy.md)).
- **Modern ES2025 + ES2026 + Multi-Tier JIT + WASM:** `99.57%` certified ES2025
  (`80,195/80,544`) and `88.97%` broad (`82,866/93,141`) on macOS ARM64, 3-tier
  x64/ARM64 JIT with concurrent GC, and `382/382` P1 WASM via `check-wasm` /
  `check-wasm-jit` (covering **MVP + sign-extension + nontrapping conversions
  only** per `test/wasm-arm64-manifest.json`; multi-value is off by default via
  `-WasmMultiValue` in P3, SIMD is experimental off by default via `-WasmSimd`
  with `test/wasm.simd` `14/14` interp + JIT in P3 after Wave 3 `J-1`/`J-2`
  fixes, and **bulk-memory `0xFC 0x0A`/`0x0B` is not implemented**).
- **Phase-4 tracks (`G0` → `Q1`–`Q3` → `I1`/`I2` → `H1`):** `G0` completed on
  `modernize/baseline` and `Q1.1`–`Q3`/`I1`/`I2`/`P1`/`C3.1` completed on
  `modernize/phase4` ([vision-and-roadmap.md](vision-and-roadmap.md));
  `G0-phase4` Linux, `B6` Windows, `I3` Intl, and the standalone Zig + `libuv`
  + `libChakraNodeApi` host (`H1`, strictly external over `JsRT` +
  `NodeApiHostApi.h`) are tracked in [vision-and-roadmap.md](vision-and-roadmap.md)
  and [open-cards.md](open-cards.md).

### 0.1 Phase-3 JsRT Additive API Versioning Contract

`ChakraCoreVersion.h` remains at pre-release dev `1.13.0`
(`CHAKRA_CORE_MAJOR_VERSION=1`, `MINOR=13`, `PATCH=0`, `RELEASE=0`). All
Phase-3 JsRT extensions are **strictly additive** (appended enum values,
appended export entries, and opt-in host callbacks) so existing Phase 1–2
callers preserve binary and source compatibility without breaking existing
ordinals or struct layouts:

1. **Covered Phase-3 additions:**
   - `JsTypedArrayType` enumerators (`lib/Jsrt/ChakraCommon.h:533-541`):
     `JsArrayTypeFloat16` (9), `JsArrayTypeBigInt64` (10), and
     `JsArrayTypeBigUint64` (11), appended immediately after `JsArrayTypeFloat64`
     (8) and pinned with compile-time `C_ASSERT` stride checks against
     `Js::TypeIds_*` (`lib/Jsrt/Jsrt.cpp:2231-2241`).
   - `JsSetTimerContinuationCallback` (`lib/Jsrt/ChakraCommon.h:2517`,
     `lib/Jsrt/JsrtCommonExports.inc:106`, `lib/Jsrt/Jsrt.cpp:3601`): optional
     per-context delayed-task hook used for finite positive `Atomics.waitAsync`
     timeouts.
   - `FetchImportedModuleFromScriptWithAttributesCallBack`
     (`lib/Jsrt/ChakraCore.h:139,251`, `lib/Jsrt/Core/JsrtCore.cpp:188,247`,
     paired with `FetchImportedModuleWithAttributesCallBack` at `0x9`):
     ES2025 import-attributes host callback registered via
     `JsModuleHostInfo_FetchImportedModuleFromScriptWithAttributesCallback` (`0xA`).
2. **Header parity guarantee:** `zig build check-metadata` (`tools/build/check_metadata.py`)
   and `check_metadata_linux.py` verify on every profile that the 4 staged public
   headers (`include/ChakraCore.h`, `ChakraCommon.h`, `ChakraDebug.h`,
   `ChakraCoreVersion.h`) are non-empty and **byte-identical** to their
   `lib/` sources, and verify dynamic/static JsRT symbol exports.
3. **Internal typed-array safety (`JsErrorInvalidArgument`):**
   `IsPublicTypedArrayTypeId` (`lib/Jsrt/Jsrt.cpp:2243-2247`) restricts
   `JsGetTypedArrayInfo` and `JsGetTypedArrayStorage` to
   `TypedArrayBase::Is(typeId) && typeId <= Js::TypeIds_Uint64Array`, returning
   `JsErrorInvalidArgument` on internal-only `TypeIds_CharArray` and
   `TypeIds_BoolArray` rather than emitting out-of-range enum values or
   hitting Debug `AssertMsg(FALSE)` / `JsErrorFatal`. `JsGetTypedArrayStorage`
   returns `sizeof(uint16)`, `sizeof(int64)`, and `sizeof(uint64)` for the
   three new element types.
4. **Runtime compatibility posture for pre-Phase-3 hosts:**
   - Hosts that never call `JsSetTimerContinuationCallback` retain prior
     pending behavior on finite `Atomics.waitAsync` timeouts and unaffected
     promise microtask continuation behavior.
   - Hosts that only register `FetchImportedModuleCallBack` /
     `FetchImportedModuleFromScriptCallBack` continue to load attribute-free
     static and dynamic imports unchanged; only attribute-bearing imports fail
     until an attributes callback is installed.
   - Hosts passing unknown `JsTypedArrayType` values or missing constructors to
     `JsCreateTypedArray` / `JsSetIndexedPropertiesToExternalData` receive
     `JsErrorInvalidArgument`.
   - `test/native-tests/jsrt_phase3_tests.h` (`TestExternalArrayBufferLifetimes`,
     `TestTypedArrayAbi`, `TestTimerContinuationCallback`,
     `TestFetchImportedModuleFromScriptWithAttributes`) exercises these contracts
     inside `check-native`. Any future breaking change to existing JsRT
     signatures or structs requires bumping `CHAKRA_CORE_MINOR_VERSION` or
     `MAJOR_VERSION` with an explicit migration note.

## 1. Current evidence and its limits

| Evidence | What it establishes |
|---|---|
| Legacy ARM64 Mac `ninja check`: 1959/0, bytecode and shared embedding checks | Behavior-parity reference for the shared/no-JIT Test configuration |
| Zig 0.17.0: 538/538 units, three linked binaries, 1959/0 plus bytecode/native checks | C++17/C/assembly toolchain capability for that Mac configuration |
| Explicit `aarch64-macos.26.0`, SDK 26.5, ICU 79.1, 8 MiB stacks and callback retention | Verified local build inputs and artifact metadata; ICU 79.1 date-format literals adapted in the legacy Intl test; not testing every 26.x host |
| Pinned test262 diagnostic: 61,565 passed / 31,576 failed in 93,141 scenarios | A reproducible place to investigate feature debt; not an ES2025 score |
| Sources inspected at `97e68c917` | Architecture/build map and current conditional feature paths |

The original Mac pilot did not compile Backend, WasmReader or WABT. It still
obtained configuration through CMake before replay; B1/B2 made Zig own that
work, and the Mac ARM64 `jit` profile now compiles and runs the Backend
(merged 2026-10-31; [arm64-macos-jit.md](arm64-macos-jit.md)).
Windows, other CPUs, LTO and a hermetic dependency package remain
unproved at this milestone's verification floor (`B6` Windows x64 `zig build`
is now wired on `modernize/phase4`, awaiting native certification); Linux x64
JIT behavior was proved natively on the proof host (`2026-11-09/10`, and `G0`
baseline completed at `1bf42d1f6`), with Phase-4 `G0-phase4` revalidation
queued ([linux-handoff.md](linux-handoff.md) §0A). The number of diagnostic failures is neither the number of bugs nor an
effort estimate: tags overlap, negative tests can pass for unsupported syntax, and
the corpus contains later proposals (including Temporal).

Reference material:

- [Current state](current-state.md): source/feature inventory and platform limits.
- [Architecture](architecture.md): ownership, execution, JIT, GC and optional units.
- [Build reference](build.md): legacy flags, source groups, generators, checks and
  the compiler/linker findings in §13–14.
- [Conformance baseline](conformance-baseline.md): exact corpus, flags, counts,
  host skips and limitations.
- [Pilot harness](../tools/zig-pilot/README.md) and
  [test262 tooling](../tools/test262/README.md): repeatable evidence workflows.
- [Memory-management guidance](memory-management.md): source-backed ownership,
  allocator, GC/FFI and invalidation rules for assistant-authored slices.
- [B4 Linux baseline](b4-linux.md), [Linux build contract](build-contract-linux.md)
  and [Linux PAL contract](pal-config-contract-linux.md): Linux environment,
  sources/flags/links and configuration contracts.
- [B4 gate verdict](b4-gate.md) and [post-B4 review](b4-review.md):
  BLOCKED-with-green-build clause grades, errata and gate-fix record.

## 2. Phase 1 — Own the existing C/C++ build with Zig

The initial delivery scope is **native ARM64 macOS, shared/no-JIT, RelWithDebInfo,
system ICU**, followed by **native x64 Linux with JIT**. (Out-of-band update
2026-10-31: a native ARM64 macOS JIT profile also landed and merged from the
worktree port; it does not change the Linux B4 scope below.) Work in small
build-graph slices and compare each against the recorded reference configuration.

**Initial build-migration completion:** B1–B4 green for the declared Mac/Linux
profiles. B5 is the release-package gate; B6 tracks later platform expansion.
The eventual single build front end remains the goal as those platforms graduate.

### 2.1 Platform, rollout and dependency contract

- Pin **Zig 0.17.0**. Capture `zig version`, `zig env`, target, build flavor,
  feature defines, dependency versions and artifact metadata in build/CI records.
- The current Mac pilot deliberately selects **macOS 26.0**, matching the
  Homebrew ICU 78 dylib minimum, with an explicit SDK. Host 26.6 is not an implicit
  deployment choice. Release testing must cover the declared minimum, not only
  inspect its Mach-O header.
- For B4, choose and record a modern Linux distribution, libc minimum, CPU target
  and ICU configuration before accepting its baseline. For B6, do the same for
  Windows/SDK/tooling. Higher floors are acceptable policy choices.
- The first active Zig matrix was Mac ARM64/no-JIT and Linux x64/JIT after their
  gates pass. As of 2026-10-31 the active Mac matrix also includes the merged
  ARM64 `jit` profile; Linux x64/JIT still enters only through B4. Windows
  keeps its legacy MSBuild route until its own rollout is
  verified. x86/ARM/Android and additional OS/architecture combinations enter the
  supported matrix only through an explicit expansion milestone.
- Legacy Azure/Cirrus recipes are comparison data, not a requirement to reproduce
  every old image and compatibility floor. Record replacements/retirements and
  test coverage when changing the active matrix.
- First prove a **repeatable system-dependency build**. SDK/ICU/Python acquisition,
  versions and packaging become controlled inputs at B5. Pinning Zig alone is not
  a hermetic build, and functional parity is not byte-identical reproducibility.
- Keep legacy entry points until each replacement passes its platform gate.
  Default-build changes and release claims name the exact supported configurations.

### 2.2 Milestones and completion gates

#### B1 — Mac configuration contract and the first build slice

Deliver:

1. In-repo source groups, include paths, C/C++/assembly flags and defines extracted
   from the inspected build. Use compile-database/pilot evidence for comparison;
   the eventual build must not need a CMake-generated file list.
2. A PAL configuration contract for the chosen Mac/SDK/toolchain. Classify each
   relevant `config.h.in` setting as fixed by the declared target, compile-checked,
   or native-runtime-verified. Document assumptions and fail on unsupported targets.
   Start with one validated profile rather than a generic cross-platform probe engine.
3. A build-owned generated `pal/src/config.h` and compiler-attribute decisions,
   including `disable_tail_calls`. Compile the PAL or another small object group
   through Zig to test the contract before wiring the whole engine.

Done when that slice builds from source plus declared dependencies, generated
configuration is comparable to the reference, and missing SDK/ICU/unsupported
profile choices fail clearly. A copied AppleClang-generated header is comparison
evidence, not B1 completion (`pal/src/configure.cmake:1045`,
`Build/CMakeFeatureDetect.cmake:17-34`).

#### B2 — One complete source-only Mac build and check

Deliver the archive, `libChakraCore.dylib`, dynamic `ch`, GCStress and staged public
headers from the verified shared/no-JIT Test configuration. Preserve per-group
flags/source membership; the Zig graph need not recreate CMake's 546-edge layout.
Model generator/input dependencies explicitly:

- `jstoc.py` generates `DbgController.js.h` before its consumers compile.
- Consume checked-in built-in bytecode to bootstrap the engine, then run
  `regenByteCode.py --noJit --verify` using the built shell as a **check step**.
  A build must not depend on regenerating bytecode with a not-yet-built engine.
- Stage the four public headers unchanged, with platform-appropriate archive,
  symbol/export and library-loading behavior.
- Preserve warning suppression, joined force-load/object linking, the dynamic
  callback and 8 MiB executable stack established by the pilot.

Done when a fresh checkout, with `build-xplat/` and `build-zigpilot/` absent,
can run the planned **`zig build` and `zig build check`** without CMake/Ninja
configuration or stale artifacts. Require:

- **1959 passed / 0 failed**, matching the no-JIT variant and tag selection;
- bytecode verification and the shared embedding test;
- smoke/flag output, not rc alone;
- correct macOS minimum, executable stack and public API/export checks.

Land and demonstrate this vertical slice; public packaging has the B5 gate.
Unsupported optional combinations should produce
an explicit error rather than silently select a different flavor.

#### B3 — Mac profiles, incremental correctness and CI

Add Debug, static and Release as separate configurations with corresponding legacy
baselines. Preserve the distinction between CMake **RelWithDebInfo / Test**
(`-O3 -g`, `ENABLE_DEBUG_CONFIG_OPTIONS`) and Zig optimization modes; a mode name
alone does not reproduce the engine's defines.

Minimum initial Mac profile set: the shared Test build from B2, a Debug/static
build covering static embedding, and a Release/shared build. Additional combinations
are named additions; a static release package also requires its own release-profile
gate at B5.

Done when:

- each declared profile passes its matching checks; static native tests are no
  longer assumed covered by the shared build's eight expected skips;
- source/header/JS-generator/configuration changes rebuild their dependents,
  flavor switches isolate artifacts, and a no-op rebuild behaves correctly;
- clean CI runs record inputs and outputs and keep the equivalent legacy leg green;
- Release artifacts have output-checked, release-compatible smoke/embedding checks.
  The legacy Release `ninja check` is only a smoke gate, not the full regression suite
  (`test/CMakeLists.txt:23-45`).

Default the Mac route to Zig only for profiles meeting these gates. No-ICU,
embedded ICU, tracing, plugins and extra architectures are separate additions.

#### B4 — Native x64 Linux, JIT and previously unbuilt subsystems

Obtain a Linux runner/container/CI environment and first establish its own legacy
baseline. Run a compiler/linker pilot there before attributing failures to new
build logic. Include Backend, WasmReader and debug/Test WABT where selected by
the reference graph (`lib/CMakeLists.txt:86-104`).

Then implement Linux PAL configuration, assembly, archives/version scripts,
system ICU and native loading in the Zig build. Require bytecode and regression
checks in the applicable **interpreted/dynapogo** variants, focused JIT/WASM tests,
native embedding checks, and applicable Debug/Test/Release coverage. Mac's 1959
count and one no-JIT variant are not the Linux acceptance count.

Done when clean native Linux CI proves behavior/ABI parity for the declared
configuration and its floor. Runtime behavior must run on the target; linking a
cross-built binary on a Mac is not the gate. Enable the Linux Zig default per
verified profile. This is a higher-uncertainty milestone because the Mac proof
has not exercised those code paths.

#### B5 — First controlled release package

Specify one release profile at a time: headers, static/shared libraries, host shell
and dependency closure. Choose recorded/pinned ICU and tools; validate loading and
embedding on a clean machine at the supported floor. Keep the old embedded-63.2
recipe as reference data while deciding the maintained package's dependency version.

Done when a second clean environment can build/install/test the package with its
declared inputs, with a documented SDK acquisition contract and versioned artifact
manifest. The first release must also rotate the serialized-bytecode release
GUID (`ByteCodeCacheReleaseFileVersion.h`): this branch changed the serialized
format versus `97e68c917` (appended `ctBigInt`, new function-attribute bits,
and with the 2026-10-31 JIT merge an appended built-in property
(`ENTRY(bigint)`) and extended opcode), and every regen so far — including
for the now-merged Mac JIT variant — was single-variant, while
`tools/regenByteCode.py` mints a new GUID only on a dual-variant (JIT + no-JIT)
regen. Serialized caches written by a baseline build and by this branch
therefore still share a GUID despite the format differences; B5 must perform
the coordinated dual-variant regen/rotation. **Update 2026-11-01: the
rotation is done** — dual-variant regen from variant-correct binaries, new
GUID `55c5da6d-dcde-42f9-8112-f7f698fcc96c`, stale-GUID caches rejected at
load (receipt with per-variant header identity, gates and a runtime
rejection demo in [b5-guid-rotation.md](b5-guid-rotation.md)); the
remaining B5 work is the package/floor scope above. No cross-version
serialized-cache compatibility is claimed beyond that rotation boundary. A claim of byte-identical reproducibility additionally needs two-build
comparison and controlled timestamp/build-ID inputs (including the existing
`__DATE__`/`__TIME__` behavior); it is a separate proof from behavior parity.

#### B6 — Windows and later extensions

Start Windows with a feasibility slice: compiler/ABI and exception behavior,
DLL exports/loading, native tests, resource/version generation and RPC/MIDL for
selected JIT modes (`lib/JITIDL/Chakra.JITIDL.vcxproj:26-43`). Decide which native
SDK tools must remain explicit inputs to a Zig-orchestrated build.

Only after that proof expand Debug/Test/Release and compare against MSBuild.
Do not promise uniform compiler/tooling behavior from the Unix pilot alone.
Additional CPUs, Android, LTO, LTTng/`--trace`, RecyclerChecker and alternative ICU
modes each get a named configuration, baseline and gate. They are not prerequisites
to a usable first Mac/Linux build or feature release.

### 2.3 Version-sensitive build facts

These are checked against installed 0.17.0 sources and its
[release notes](https://ziglang.org/download/0.17.0/release-notes.html):

- `b.addExecutable`, `b.addLibrary`, `b.createModule`, module C-source and link APIs
  are the build primitives. C/C++ and preprocessed assembly remain explicit inputs.
- Native Mach-O callback retention is
  `ch.forceUndefinedSymbol("_OnChakraCoreLoadedEntry")`; set
  `ch.stack_size = 8 * 1024 * 1024`. `Module.export_symbol_names` is WASM-only.
- `@cImport` is removed; real Zig/C binding work uses a pinned external ZSF
  `translate-c` package. Its integration belongs to Z1, not the C/C++ build gate.
- LLVM/Clang 22.1.8 is bundled; the compiler's supplied libc versions are not an
  automatic policy for our runtime/deployment minimum.
- ZLS has no 0.17 release at verification time. IDE integration and incremental
  linker research on macOS/Windows do not gate these build milestones.

## 3. Phase 2 — A bounded, useful Zig integration

Phase 2 proves a maintenance technique, rather than mandating a subsystem rewrite.
**Z1 is complete after one useful small unit is integrated and tested.** More
language migration is a separate choice for each unit. C++ compatibility work can
proceed after its own build/conformance prerequisites without waiting for Z1.

### Z1 — One C-ABI slice

Choose new tooling or a demonstrated leaf helper with no GC/JIT/AST ownership.
Before selecting it, identify a concrete benefit and a rollback-sized patch.
Use the [ownership record and verification workflow](memory-management.md#3-write-the-ownership-record)
for the selected allocation/borrow/root contract.

The standing conversion policy, wedge ladder and preconditions are
recorded in [zig-strategy.md](zig-strategy.md) (2026-11-09): no
engine-core port; incremental Zig for C-ABI boundary components
(N-API → host slices → PAL subsystems), each parity-gated and
delete-the-old. Read it before proposing any Zig replacement.

Deliver:

1. A narrow C header contract: functions, opaque handles or plain structs, error
   codes, allocation/free responsibility and threading rules.
2. A pinned binding path. `translate-c` can scaffold C declarations; translate
   the real selected slice and trim it. C++ templates/STL/classes are not C bindings.
3. A Zig implementation linked into the active C++ build, with equivalent focused
   behavior tests and any relevant allocation/performance measurements.
4. Full active-matrix checks and byte-identical public JsRT headers. Replace the
   old implementation only once that evidence is green; retain the C contract.

No exceptions, RTTI or STL types cross the boundary. Allocation ownership and
runtime-owned `ThreadContext`/Recycler lifetime, current TLS/rental-threading rules
and `ScriptContext` realm ownership remain explicit C++ responsibilities.

Candidate catalog after Z1:

- selected `Common/` utilities with genuinely isolated dependencies;
- small scanner/table or decoding helpers with C++ ownership retained;
- a structured-clone or WASM frontend slice after its dependency/GC/AST contract
  is demonstrated. SCACore accepts `ScriptContext` and `Var` and owns stream
  readers/writers (`SCACore.h:15-49`); WasmReader feeds existing codegen. Neither
  is automatically a leaf because its public description sounds bounded.

Runtime/Backend/Recycler/JsRT wrapper rewrites remain outside the required migration.
Retain a C++ implementation where introducing a boundary adds coupling or cost.

## 4. Phase 3 — Compatibility gains toward full ES2025

**Full ES2025 core-language coverage remains the target.** It is a multi-release
engine-development objective with uncertain total effort, not a near-term promise
or a reason to postpone useful partial releases. The current diagnostic cannot
yet certify an edition result, and 31,576 failures cannot be converted into a
schedule. Each compatibility batch must have a bounded contract and evidence.

### 4.1 Milestones

#### C1 — Qualify the runner and triage the debt

Deliver an audited manifest/policy of required ES2025 tests, excluding documented
future/optional-host cases without excluding unsupported required features. Feature
tags alone do not define the edition: later semantic changes can test an existing
builtin. Validate scripts/modules, strict/default scenarios, async/jobs/realms,
and negative parse/resolution/runtime phases with positive and deliberate-failure
controls. Finish the existing negative-phase gap before certifying a full result.

Group failures by root cause and produce a shortlist with reproduction, subsystem,
prerequisite, expected test improvement and uncertainty. Separate engine defects,
disabled-feature incompleteness, host limitations and future proposals. Save both
the edition-selected baseline and the broad diagnostic so snapshots remain comparable.

Done when test selection, expected skips and host mechanisms are documented and
the shortlist is backed by reproducible cases. Reforecast feature effort here.

**Record (2026-10-13, HEAD `3c597e4cc`):** done, with a CONDITIONAL
qualification. The edition policy (`tools/test262/edition-policy.json`,
151 required / 27 post-ES2025 tags, conservative required-on-doubt rule),
the analysis tool (`tools/test262/analyze.py`) and the triage/shortlist
([c1-triage.md](c1-triage.md)) are recorded over the pinned baseline
without re-running the corpus: edition-selected **60,896 passed / 19,648
failed of 80,544 scenarios** against the broad 61,565 / 31,576 of 93,141,
with failure buckets and a 14-item shortlist. Controls grew 10 → 18 and
now catch deliberate wrong-phase negatives through observable phase
evidence, and the recorded negatives audit finds 0 parse / 12 resolution
/ 0 runtime suspects — but the evidence is host-shaped and cannot see
every wrong phase, so **the negative-phase gap is narrowed, not finished**:
explicit host-reported phase remains required before certifying any full
(C4) result, exactly as scoped above. Enabled-flag measurement runs for
BigInt/SAB/RegExp-symbol surfaces and B4 native x64 JIT verification stay
open as shortlist prerequisites, not C1 blockers. A post-record review
also found and fixed an eshost fixture-cache collision in the harness
wrapper (2 measured scenario flips at `--jobs=1` over the affected
directories; 888 recorded scenarios exposed); the baseline above was
recorded pre-fix. The exact 30-directory subset has now been rerun with the
fixed harness: **1,367 passed / 998 failed of 2,365**, with **0 pass→fail**
and **102 fail→pass** against the old precombined result. This is pre-negative-
phase evidence, not a C4 certification
([c4-fixture-rerun.md](c4-fixture-rerun.md);
[c1-triage.md](c1-triage.md) §3/§9).

#### C2 — Deliver bounded compatibility batches

Prefer small, demonstrable gains; use the priority inventory below to select them,
not as a requirement to finish every P0 item before a small P1 improvement.
Suitable streams are existing-builtin semantic fixes, smaller missing builtins,
optional chaining/logical assignment, and older RegExp gaps. Take one feature or
coherent dependency group per batch, with a named owner/work item and supported
configuration list.

Done per batch when:

- the promised feature subset and focused qualified tests pass, including valid
  syntax, negative/early-error cases, side effects and relevant cross-realm behavior;
- inherited regression checks remain green; parser/bytecode changes have regenerated
  and verified built-in bytecode where required;
- interpreter and applicable SimpleJIT/FullJIT paths pass before enabling globally
  on JIT configurations (since 2026-10-31 the merged Mac ARM64 `jit` profile
  supplies that environment for regression suites; each batch's test262 corpus
  under SimpleJIT/FullJIT and any Linux JIT claim remain owed — Linux via B4);
- the pinned full comparison reports no unexplained new failures; deliberate
  expected-output changes are reviewed individually;
- any default-flag change has its own coverage and release note. BigInt/SAB/RegExp
  support is tested with flags enabled before deciding the default; flipping a
  switch does not prove conformance.

Publish supported features and remaining gaps after each batch. Partial releases
must describe their coverage; raw aggregate percentage is not a feature guarantee.

Batch receipts: S4 optional chaining + logical assignment —
[c2-s4.md](c2-s4.md) (interpreter-verified, default-enabled; JIT matrix still
gated on B4). Recorded decision for S4's global defaults: the feature flags
are not JIT-scoped per function (no `Jit*` companions), because S4 emits only
pre-existing opcodes and the `JitES6Generators` precedent gates an intrinsic
function kind, not a syntax form — see the receipt's "Round 3" section. The
defaults therefore carry Mac no-JIT evidence only: **B4 must run the S4
test262 corpus and the es7 tests under SimpleJIT/FullJIT (dynapogo) before
any JIT-configuration default or release claim for these features.** Kill
switches if a JIT issue appears: `-ESOptionalChaining-`,
`-ESLogicalAssignment-`. S5 Iterator helpers — [c2-s5.md](c2-s5.md)
(closed: interpreter-verified and default-enabled, including the
review follow-up; pure C++ runtime-library helpers with traced
helper objects, no new opcodes and no bytecode change). As with S4,
the default carries Mac no-JIT evidence only: B4 must run the S5
test262 corpus and the es7 test under SimpleJIT/FullJIT before any
JIT-configuration claim. Kill switch: `-ESIteratorHelpers-`.
S11 `String.prototype.replaceAll` — [c2-s11.md](c2-s11.md)
(interpreter-verified, unconditional library method, no feature flag
and no kill switch, no new opcodes). The default carries Mac no-JIT
evidence only: **B4 must run the S11 test262 corpus and both es7
tests under SimpleJIT/FullJIT (dynapogo) before any JIT-configuration
claim.** Documented gaps, not S11 defects: the default profile has no
`RegExp.prototype.flags`, so a deleted `flags` property does not
throw; named capture groups are unsupported. `@@replace` dispatch
requires `-ES6RegExSymbols` and applies only to Object search values.
S11 remainder (`Object.groupBy`, `Map.groupBy`, `RegExp.escape`,
`Promise.try`, `Promise.withResolvers`, `isWellFormed`, `toWellFormed`)
— same receipt: interpreter-verified, unconditional library methods,
no feature flag, no kill switch, no new opcodes; B4 must run its
test262 corpus and `test/es7/s11-builtins.js` under SimpleJIT/FullJIT
before any JIT-configuration claim. S7 later delivered `matchAll`
itself (see below).
S10 change-array-by-copy + Set methods — [c2-s10.md](c2-s10.md)
(closed: interpreter-verified, unconditional library methods, no
feature flag and no kill switch, no new opcodes). The default carries Mac no-JIT evidence only: **B4 must run both S10
test262 corpora and `test/es7/change-array-by-copy.js` and
`test/es7/setmethods.js` under
SimpleJIT/FullJIT (dynapogo) before any JIT-configuration claim.**
Documented gaps, not defects in the fourteen methods: the typed-array
default comparator treats `-0` and `+0` as equal (shared with
`.sort()`); `ESBigInt` is off; resizable ArrayBuffer is S8.

**Mac ARM64 JIT evidence update (2026-10-31).** With the JIT merge
(`9004b3ed7`), the es7/regression half of the per-batch JIT obligations
above (S4/S5/S10/S11 and the later S1/S2/S3/S6/S7/S8/S9/S12/S13/S14
surfaces) now has Mac evidence: dynapogo 2001/286 and interpreted
2426/397 with every failure the asm.js/WASM gap and zero signals,
including es7 96/0 under dynapogo. Shapes the old backend cannot
faithfully compile — direct-eval callers and eval code, the 18
class/private/import-attribute opcodes, one labeled-break-from-`finally`
CFG shape — run interpreted by design, so those functions are not
JIT-compiled even though their tests pass. What remained owed per batch
before any broader JIT-configuration claim — its test262 corpus under
SimpleJIT/FullJIT, and Linux JIT evidence via B4 — was discharged on the
Mac side by the 2026-11-01 per-batch campaign (17/19 batches, with the
two Set/groupBy reentrancy defects fixed afterwards:
[c2-jit-test262.md](c2-jit-test262.md),
[c2-jit-reentrancy-fix.md](c2-jit-reentrancy-fix.md)); Linux JIT
evidence still waits on B4. The kill switches
recorded above are unchanged.

#### C3 — Larger runtime projects

Class fields/private elements/static blocks, WeakRef/FinalizationRegistry/job
retention, resizable buffers/typed arrays, and advanced RegExp/module work need
their own design and experiments. Split implementation, interpreter correctness,
JIT coverage, GC/ownership invariants and default enablement into reviewable parts.

Do not bundle these into one change or assume they are leaf migrations. Keep
experimental flags until their active-matrix tests pass. Any additive Phase-3
host/API contract must satisfy the versioning, header-parity, and fallback
compatibility requirements in §0.1 (`test/native-tests/jsrt_phase3_tests.h`).

#### C4 — Full ES2025 core-language acceptance

Done when the qualified, pinned **required ES2025 suite is green** on the declared
supported engine configurations, required features are enabled in the claimed
default configuration, and all skips/host applicability are justified. Preserve
negative-phase validation, regression/bytecode/native checks and a published
feature/result manifest. Test262 is the executable gate, not proof of every possible
spec behavior; investigate discovered spec mismatches too.

ECMA-402 constructor expansion and WASM stabilization have independent scope,
baselines and milestones. They improve the product but do not silently enlarge C4.

### 4.2 Edition/feature inventory and priorities

Substantial ES2015–ES2019 coverage exists with inherited debt, plus cherry-picked
newer features. Presence is not blanket conformance. Edition assignments below
were checked against the [TC39 finished-proposals list](https://github.com/tc39/proposals/blob/main/finished-proposals.md).

| Edition | Implemented examples | Debt / priority |
|---|---|---|
*(Debt column is the C1 triage-time snapshot; landed items stay listed
there and are tracked as closed in the C2 batch receipts below.)*
| ES2015–ES2019 | modules/generators, async/await/iteration, rest/spread, dotAll, flat/flatMap, fromEntries | named groups and lookbehind have landed; property escapes; RegExp symbols/prototype properties default off; inherited semantics → P0/C1 triage |
| ES2020 | `??`, import(), import.meta, globalThis, allSettled | optional chaining, matchAll, BigInt completeness/default; for-in formalization → P0 |
| ES2021 | numeric separators, Promise.any | replaceAll, logical assignment, WeakRef/FinalizationRegistry → P0/P1 |
| ES2022 | .at, hasOwn, cause, TLA | class fields/private elements/static blocks, RegExp d → P0 |
| ES2023 | findLast, hashbang | copy-by-change arrays/typed arrays, weak-collection symbol keys → P0/P1 |
| ES2024 | no complete coverage established | groupBy, withResolvers, well-formed strings, v, resizable/transfer buffers, waitAsync → P1 |
| ES2025 | no complete coverage established | escape, eval-var semantics, Float16, try, iterator helpers, JSON modules/import attributes, modifiers, Set methods, duplicate groups → P1 |
| ES2026 | stretch after required scope | upsert, JSON.parse source, sequencing, base64, sumPrecise, isError, fromAsync |

**P0 — High-breakage compatibility** (landed items are tracked in the C2
batch receipts below): optional chaining/logical assignment;
class syntax; replaceAll/matchAll and the copy-by-change family; inherited
ES2018 RegExp gaps and import-attribute syntax. Named groups, the `d`
flag, duplicate named groups, and lookbehind have landed (see the
C2-S9 receipt). Property escapes (S3-a), the `v` flag (S3-b) and import
attributes (S12) have since landed as well (see the §6 receipts). Audit BigInt, SAB/Atomics and
RegExp-symbol/prototype-property flags through enabled conformance runs.

**P1 — Required remaining core and independent ecosystem work:** ES2024/2025
workstreams above, WeakRef/FinalizationRegistry and weak-collection symbol keys.
Eval-variable redeclaration is a semantic triage item, not a name-search absence
claim. Separately expand Intl (RelativeTimeFormat, ListFormat, DisplayNames,
Locale, Segmenter, DurationFormat) and decide maintained ICU acquisition/versioning;
separately stabilize WASM multi-value (`-WasmMultiValue`, P3), SIMD (`-WasmSimd`, experimental P3) and threads (P3), implement bulk-memory (`0xFC 0x0A`/`0x0B`), then assess further proposals.

**P2 — Longer-range architecture:** ARM64 JIT on further platforms (macOS
landed 2026-10-31; Linux is B4-adjacent), JIT coverage ports for the
interpreted-only shapes recorded in [arm64-macos-jit.md](arm64-macos-jit.md)
(eval-from-JIT-frames fidelity, the 18 class/private/import opcodes), an
ARM64 asm.js/WASM port, additional platforms, and researched
performance/memory designs. Sandboxing/pointer compression and
competitor tier counts are research, not this migration's completion gates.

**P3 — Release/tooling health:** publish results and feature manifests as batches
land; improve embedding samples and host ergonomics; refresh old links/images/tool
floors alongside their relevant build milestone. A dated supported package can
ship at B5, well before C4.

Decorators remain Stage 2.7 and Explicit Resource Management (since completed in `C3.1`, `947/0`) / Temporal are later
edition work at verification time. They are tracked stretch items. `queueMicrotask`
is a host-job-queue decision tied to the embedder's promise/FIFO contract
(`ChakraCommon.h:757-768`), separate from ECMA-262 edition coverage.

## 5. Attainability, dependencies and stop points

| Work | Assessment | How to keep it bounded |
|---|---|---|
| B1/B2 | Concrete near-term goal; compiler evidence is strong, build/configuration ownership is still work | One Mac profile, one build slice at a time, clean-checkout gate |
| B3/B4 | Achievable staged expansion; Linux JIT is materially less certain | Native baselines, separate profiles and compiler pilots, target-side execution |
| B5/B6 | Release/Windows work needs its own proof | Controlled dependency package; Windows feasibility before broad toolchain promises |
| Z1 | Attainable optional experiment | One useful unit; fixed C-ABI ownership; no migration quota |
| C1/C2 | Measurable compatibility program | Qualified tests, grouped failures, one feature batch and receipts |
| C3/C4 | High-uncertainty, multi-release engine development | Design spikes, explicit prerequisites, staged flags and reassessed effort |

These assessments are relative work classes, not calendar estimates. Assume a
small maintenance effort and keep one principal risky engine/build change active
at a time; independent documentation/CI work can accompany it. Before starting a
milestone, record its exact configuration, deliverable, success checks and open
assumptions. After finishing, attach logs/results and choose the next bounded item.

Decision/rollback points:

- If a compiler pilot fails, distinguish toolchain/ABI issues from a pre-existing
  engine defect before changing the new graph. Keep a working legacy route.
- If B1 would require a large generic probe framework, finish the one validated
  target contract first and broaden when another target needs it.
- If a Zig boundary adds coupling, keep the unit in C++ and continue compatibility
  work; a useful build is already a successful Zig adoption outcome.
- If C1 finds the apparent pass-rate gain is a harness/selection change, qualify
  that change and rebaseline before attributing it to the engine.
- If a runtime project exceeds a bounded feature change, split it into design,
  flagged implementation and rollout work rather than holding the whole release.

## 6. Completion checklist and immediate next work

- [x] F0: Mac compiler/linker pilot, behavior parity, artifact metadata and
  diagnostic-conformance foundation recorded.
- [x] B1: Source/flag/PAL configuration contract and first Zig build slice.
- [x] B2: Clean source-only Mac `zig build` / `zig build check`, 1959/0,
  bytecode/shared native/metadata/API gates.
- [x] B3: Each declared Mac profile, dependency rebuild behavior and clean CI;
  scoped default rollout. (Hosted Zig CI `.github/workflows/zig-macos.yml` is
  live on `master` and `modernize/**`; Waves 1–4 remediated `B-15..B-18` and
  `B-4` matrix gaps.)
- [x] ARM64 macOS JIT profile (out-of-band worktree port, not a lettered
  milestone): rebased onto `aa42bb4f1`, merged 2026-10-31 (`9004b3ed7`);
  `zig build -Dprofile=jit check` green (`check-fulljit` `643/0`, `check-wasm`
  P1 `382/382` after Wave 3). Coverage notes in
  [arm64-macos-jit.md](arm64-macos-jit.md) and [open-cards.md](open-cards.md).
- [x] B4: Linux native baseline/pilot (`2026-11-09/10`, [b4-linux-run.md](b4-linux-run.md))
  and `G0` baseline Linux x64 revalidation completed (`1bf42d1f6`, merged into
  `modernize/phase4`); Phase-4 `G0-phase4` revalidation on updated
  `modernize/phase4` is queued in [linux-handoff.md](linux-handoff.md) §0A.
- [x] B5: Controlled dependency/artifact package on macOS ARM64
  (`check-package`, Wave 2 bundled ICU + prefix isolation `8c0528d1c`,
  serialized-bytecode GUID rotation [b5-guid-rotation.md](b5-guid-rotation.md));
  Linux/Windows clean-machine packaging runs queued.
- [ ] B6: Windows x64 `zig build` wired (`tools/build/{windows,tables_windows}.zig`,
  Node-API v10 `161` exports, PE32+ gates, `.github/workflows/zig-windows.yml`);
  awaiting native Windows certification ([windows-handoff.md](windows-handoff.md)).
- [x] Z1: Production Zig `0.17.0` C-ABI SIMD codec integrated and tested in
  `Q1.3` (`lib/ZigCodec/u8a_codec.zig`, [z1-u8a-codec.md](z1-u8a-codec.md)).
- [x] C1: Edition policy, host/negative-phase qualification (`-NegativePhaseReport`
  host-reported phase landed) and root-cause shortlist
  ([c1-triage.md](c1-triage.md), [c1-negative-phase.md](c1-negative-phase.md)).
- [ ] C2/C3: C2 batches (`ESBigInt`, `ESSharedArrayBuffer`, `ES6RegExSymbols`,
  `ES6RegExPrototypeProperties` all default-on) and `C3.1` Explicit Resource
  Management (`947/0`) completed on macOS ARM64; `Temporal` and WASM 2.0
  remain separate tracks. Consolidated live register: [open-cards.md](open-cards.md).
- [ ] C4: Latest certified macOS ARM64 ES2025 result (`Q3`): **`99.57%`**
  edition-selected (**`80,195/80,544`**) and **`88.97%`** broad
  (**`82,866/93,141`**), with `ES6RegExSymbols` and
  `ES6RegExPrototypeProperties` default-on (`true` in
  `lib/Common/ConfigFlagsList.h:689-700`); Linux/Windows Phase-4 re-verify
  queued ([open-cards.md](open-cards.md)). *(Historical 2026-11-16 snapshot
  `refresh-2026-11-16` at `c24266598` stood at `97.75%` / `78,731` of `80,544`
  before Tier 2 enabled both RegExp flags and `Q1.2`–`Q3` landed.)*

**Stage 2/3 hardening + architectural review (2026-11-16, `c24266598`).**
Following the prior 2026-11-08 swarm review, Stage 2/3 closed all high-priority
architectural/safety findings and expanded test verification: `Lower.cpp`
`Float16Array` table shift/asserts + `MediumHeapBlock` 16-bit offset truncation
(`cdjs.js`), JsRT `TypedArray` ABI/internal-type guards (`JsErrorInvalidArgument`)
& `JsrtExternalArrayBuffer` detached finalizer ownership, cross-runtime GSAB
release/acquire length atomics, non-writable `lastIndex` RegExp JIT/slow
guards, `WScriptJsrt` header split + `StderrLock` + atomic `sourceContext` +
per-child `broadcastQ`, Node-API A8-3 `PromiseQueue*` reclamation +
`ch-nodeapi` `post_task`/`post_work` + `BigInt64Array`/`BigUint64Array` wiring
+ OOM guards, `AssemblyCommon.h`/`XDataAllocator` unwind portability, Zig
Backend archive link ordering (`test-shared-basic` + Phase-3 native tests
link cleanly against shared libs without `libChakraCoreStatic.a`),
`tools/build/linux.zig` parameterization, portable StyleChecks, and
relocation to `doc/evidence/gc-async-continuation/`. Full synthesis in
[architecture-and-readiness-review.md](architecture-and-readiness-review.md).

**Historical Restart point (2026-11-16, `c24266598` — superseded by Tier 2 default-on RegExp flags, `G0` `1bf42d1f6`, `Q1.2`–`Q3` `99.57%`, and Waves 1–5 in [open-cards.md](open-cards.md)).** At that `c24266598` checkpoint: **2063/0 test**, **2115/0
debug-static** (`8/8` static native suites), `release-shared` green,
`jit` + `wasm` (`check-jit` + **382/382 P1** `check-wasm`), Node-API (`18`
probes + `run_probe.sh` + `REPEATS=5` stress + `46/23/0` & `52/61/6`), and
**C4 baseline refresh (`refresh-2026-11-16`)**: edition-selected
**78,731/1,813 of 80,544 (97.75%)** — broad `79,569/13,572` (`85.43%`) —
(`disabled-flag` `636` down `-1,098`, `engine-defect-on-existing` `960` down
`-50`, `required-feature-missing` `200` down `-2`, `host` `17` up `+14`;
[c4-manifest.md](c4-manifest.md), [c4-gate.md](c4-gate.md)). At that
2026-11-16 snapshot, two RegExp substrate flags remained off; Tier 2
subsequently enabled both `ES6RegExSymbols` and
`ES6RegExPrototypeProperties` by default (`ConfigFlagsList.h:689-700`) and
`Q1.2`–`Q3` + Waves 1–5 advanced the certified macOS ARM64 baseline to
`99.57%` (`80,195/80,544`), documented in [open-cards.md](open-cards.md).
[c2-jit-finally-globopt.md](c2-jit-finally-globopt.md),
[c2-jit-eval-fidelity.md](c2-jit-eval-fidelity.md),
[c2-jit-private-opcodes.md](c2-jit-private-opcodes.md),
[c2-jit-test262.md](c2-jit-test262.md),
[c2-jit-reentrancy-fix.md](c2-jit-reentrancy-fix.md),
[c2-jit-immutable-store.md](c2-jit-immutable-store.md); B — the
path-to-green engine lanes,
[c2-s12-dynamic-import-host.md](c2-s12-dynamic-import-host.md),
[e3-cross-realm.md](e3-cross-realm.md),
[e4-detach-buffer.md](e4-detach-buffer.md),
[e5-compound-assign-let-asi.md](e5-compound-assign-let-asi.md),
[e6-class-tail.md](e6-class-tail.md),
[e7-early-error-executions.md](e7-early-error-executions.md),
[e8-string-method-partials.md](e8-string-method-partials.md),
[e9-bigint-partial.md](e9-bigint-partial.md),
[e10-resizable-ab-partial.md](e10-resizable-ab-partial.md),
[e11-tco-timeouts.md](e11-tco-timeouts.md); C — the card register
[open-cards.md](open-cards.md); D — the review/cleanup sweep; E —
testing quality [e12-testing-quality.md](e12-testing-quality.md) and
the refresh [c4-refresh.md](c4-refresh.md). Pin lineage stays
single-sourced in `tools/build/gate_counts.json`, 2038/2090 →
2051/2103.)

**B4 Linux x64 + D1–D4 update (2026-11-10).** B4 completed natively on
the proof host ([b4-linux-run.md](b4-linux-run.md)): Linux gates
**2841/0 interpreted / 2305/0 dynapogo**, first certified Linux
test262 baseline **77,354 / 3,190** edition-selected. The B4 swarm
batch and the D1–D4 batch then landed on Mac: gates are now
**2053/0 test / 2105/0 debug-static**, `zig build -Dprofile=jit check`
646/646, targeted Number test262 **276/276**. D1 (portable suspended-
coroutine guard), D2 (exact 0..100-digit Number formatting), D3 (exact
integer radix) and D4 (deferred WASM validation) are implemented and
reviewed; the Linux native run, the Linux pin re-record, and the full
certified test262 refresh remain ([b4-d1-d4.md](b4-d1-d4.md) §5).

**Reconciliation of former 2026-11-16 blockers (current status in [open-cards.md](open-cards.md)):**

- **RegExp substrate flags (`ES6RegExSymbols` and `ES6RegExPrototypeProperties`):**
  Subsequently flipped **default-on** (`true` in
  `lib/Common/ConfigFlagsList.h:689-700`) with `Lower.cpp` fast-path guards
  in Tier 2, eliminating the `712`-scenario `disabled-flag` bucket (`99.57%`
  ES2025 after `Q3`: `80,195` passed / `349` failed of `80,544`).
- **Hosted GitHub Actions CI:** `.github/workflows/zig-{macos,linux,windows}.yml`
  are live on `master` and `modernize/**`, and Waves 1–4 remediated
  `B-15..B-18` and `B-4` matrix gaps (`check-fulljit`, `check-gcstress`,
  `check-sanitize` wired).
- **Linux (`G0-phase4`) and Windows (`B6`) execution:** Baseline Linux `G0`
  completed at `1bf42d1f6` ([b4-linux-run.md](b4-linux-run.md) §12); current
  Phase-4 `G0-phase4` Linux and `B6` Windows runbooks live in
  [linux-handoff.md](linux-handoff.md) §0A and
  [windows-handoff.md](windows-handoff.md), and the live open/deferred register
  lives in [open-cards.md](open-cards.md).

### Historical chronological log (Oct–Nov 2026 checkpoints; superseded at HEAD by [open-cards.md](open-cards.md))

**Latest integration checkpoint (2026-11-01):** main is clean at the
**2038/0 test** and **2090/0 debug-static** gates (re-verified after every
merge and repair below; counts are single-sourced in
`tools/build/gate_counts.json` — rungs after this checkpoint belong to the
mainline-only era above and are listed in that file's lineage string,
2038/2090 → 2051/2103 as of 2026-11-05 — Float16Array's new test file moved
2012/2064 → 2013/2065, repair-loop `-noDeferParse` rlexe variants
then moved 2013/2065 → 2016/2068, the flag-on S6a slice added one entry
(2016/2068 → 2017/2069), the direct-eval super lane added three
parse-mode entries (2017/2069 → 2020/2072), S6b added the waitAsync/pause
entry (2020/2072 → 2021/2073), S8 immutable ArrayBuffer added one entry
(2021/2073 → 2022/2074), S3-a Unicode property escapes added one entry
(2022/2074 → 2023/2075), S3-b RegExp `v` mode added one entry
(2023/2075 → 2024/2076), S9 u-mode RegExp strictness added one entry
(2024/2076 → 2025/2077), S6c BigInt Atomics added one entry
(2025/2077 → 2026/2078), S2 BigInt default enablement added four
entries (2026/2078 → 2030/2082), S6e cross-agent waitAsync added
one entry (2030/2082 → 2031/2083), and the host-broadcast-type repair
added one entry (2031/2083 → 2032/2084), and the s14-eval-nonsimple-var
lane added one entry (2032/2084 → 2033/2085), the jit-reentrancy-fix lane added
one entry (2033/2085 → 2034/2086), the jit-closure-1 lane added one
entry (2034/2086 → 2035/2087), and the jit-immutable-store lane added one
entry (2035/2087 → 2036/2088)). The dbg-async-attach lane then closed the
suspended plain-async pending-await resume abort after dynamic debugger
attach (separate debug FunctionBody for the suspended case; deterministic
`DebuggerCommon/AsyncSuspendedResume.js`) and moved the gates
2036/2088 → 2037/2089 ([dbg-async-attach.md](dbg-async-attach.md)). The
gc-async-continuation lane then closed that review's filed "GC drops promise
continuations" defect — root-caused to `ch` host message ordering (promise
microtasks stamped "now" among absolute-due timers; the GC exonerated by
pointer-stable traces), fixed with microtask-before-timer draining plus the
two latent bugs the corrected order exposed (TLA
`DecrementAndEvaluateIfNothingAwaited` null-generator crash on cyclic
importers; `SortedList::Remove`'s non-advancing ClearTimeout hang), with
deliberate expected-output corrections to ES6PromiseAsync (baseline regen),
finalization-registry.js and top-level-await.js, adding
`es7/async-microtask-order.js` and moving the gates 2037/2089 → 2038/2090
([e2-gc-async-continuation.md](e2-gc-async-continuation.md)). A subsequent whole-branch
review repair (2026-10-31) fixed the CI selftest tally that would have
kept the first Actions run red (B3 stays CONDITIONAL until that run is
green), the agent-init-failure spawn hang, host teardown ordering, SCA
cross-runtime marking, and stale status text; gates re-verified
unchanged at 2032/2084. The ARM64 macOS JIT worktree port then rebased
onto that checkpoint and merged (2026-10-31, merge `9004b3ed7`, docs
`56c767e07`): `zig build -Dprofile=jit check` green, interpreted
2426/397 and dynapogo 2001/286 with every failure the asm.js/WASM Mac
gap and zero signals, disable_jit 2032/0 on the JIT binary, and the
2032/2084 no-JIT gates re-verified unchanged after the merge. That
closure also caught and fixed a wrong-format no-JIT bytecode regen
(JIT binary had written the nojit headers) before it could ship.
Merged lanes: S1 public/private instance+static fields and private methods/accessors, S2 arithmetic + BigInt64Array/Uint64Array + collections/
DataView, S8 resizable ArrayBuffer/transfer + TypedArray tracking, S13
WeakRef + FinalizationRegistry, and S14 P1/P2/P3/P4/P5/P6/P7
([c2-s1-public-fields.md](c2-s1-public-fields.md),
[c2-s1-private-fields.md](c2-s1-private-fields.md),
[c2-s1-private-methods.md](c2-s1-private-methods.md),
[c2-s1-static-fields.md](c2-s1-static-fields.md),
[c2-s1-static-private-fields.md](c2-s1-static-private-fields.md),
[c2-s2-bigint-arithmetic.md](c2-s2-bigint-arithmetic.md),
[c2-s2-bigint64array.md](c2-s2-bigint64array.md),
[c2-s2-bigint-collections.md](c2-s2-bigint-collections.md),
[c2-s8-resizable.md](c2-s8-resizable.md),
[c2-s8-typedarray-tracking.md](c2-s8-typedarray-tracking.md),
[c2-s13-weakref.md](c2-s13-weakref.md),
[c2-s13-finalization-registry.md](c2-s13-finalization-registry.md),
[c2-s14-statement-completion.md](c2-s14-statement-completion.md),
[c2-s14-eval-arguments.md](c2-s14-eval-arguments.md),
[c2-s14-annexb-var-skip.md](c2-s14-annexb-var-skip.md),
[c2-s14-global-eval-declarations.md](c2-s14-global-eval-declarations.md),
[c2-s14-assignment-reference.md](c2-s14-assignment-reference.md),
[c2-s14-enumeration-order.md](c2-s14-enumeration-order.md),
[c2-s14-labeled-break-finally.md](c2-s14-labeled-break-finally.md)).
Two further S1 waves then completed the class-elements surface on Mac
no-JIT: private static methods/accessors and `super` in static field
initializers, then computed field names + `super` in instance field
initializers, async/generator private methods, and static
initialization blocks
([c2-s1-private-static-methods.md](c2-s1-private-static-methods.md),
[c2-s1-super-static-init.md](c2-s1-super-static-init.md),
[c2-s1-computed-instance-super.md](c2-s1-computed-instance-super.md),
[c2-s1-private-async-gen.md](c2-s1-private-async-gen.md),
[c2-s1-static-blocks.md](c2-s1-static-blocks.md); static blocks took a
review loop-2 fix for defer-scan scope attribution, recorded in its
receipt). Integrated class-elements corpus (statements + expressions
`class/elements`, 5,897 scenarios, re-run 2026-10-20 on the merged
binary, `out/test262-c2-s1-wave2`): **5,854/5,897 passed (43
failed)**, **3,865 fail→pass / 0 pass→fail** vs the 2026-10-19
integrated run (both S1 waves). The 43 residuals are 24 direct-eval
private-visibility scenarios, 8 private-names-in-computed-keys, 4
primitive-receiver private field get/put, 2 PrivateFieldSet
evaluation-order, 1 non-extensible-object (strict), and 4
post-ES2025 `accessor`-grammar scenarios. A third wave then closed
that tail and two further clusters on Mac no-JIT: direct-eval and
computed-key private visibility plus private-field operation
semantics ([c2-s1-tail-privates.md](c2-s1-tail-privates.md); elements
corpus re-run `out/test262-c2-wave3`: **5,888/5,897**, +34/0 vs
wave 2 — the 9 remaining are the 4 post-ES2025 `accessor` scenarios,
4 primitive-receiver scenarios that need `-ESBigInt` to parse in the
default runner (pass with the flag), and 1 Stage-1 non-extensible
scenario Node also fails), `Float16Array`
([c2-s8-float16array.md](c2-s8-float16array.md); one loop-1 debugger
defect fixed in loop 2), and import attributes + JSON modules
([c2-s12-import-attributes.md](c2-s12-import-attributes.md); additive
JsRT fetch-with-attributes callback, legacy hosts unaffected; two
loop-2 defects fixed in a user-authorized loop 3; `OP_ImportCallWithOptions`
JIT lowering still owed — on the merged Mac ARM64 JIT the opcode aborts to
the interpreter, and native lowering remains follow-up work). The final wave-3 repair cycle then closed the
10 direct-eval `super` early-error scenarios (merge `ea8f253ac`; lexical
HomeObject propagation through arrows/eval bodies, full eval-code
454/0, class-elements 5,888/9 unchanged; legacy fixtures updated to the
spec `SyntaxError`) and landed the flag-on S6a slice (merge
`e3371d313`; growable SharedArrayBuffer plus ES2020 non-shared Atomics;
focused S6 462/982 → 574/982, 112 fail→pass / 0 pass→fail; SAB remains
off by default) ([c2-s1-eval-super.md](c2-s1-eval-super.md),
[c2-s6-growable-nonshared.md](c2-s6-growable-nonshared.md)). A final
adversarial review then closed the object-literal function/arrow eval-super
false accepts and the debug index-detach Atomics abort in `39e9f2a16`
(gates unchanged). S6b then added `Atomics.pause` and single-thread
`Atomics.waitAsync` (merge `eef3b93c5`; focused 80/124, 0 pass→fail;
finite timeout and cross-agent suspension remain), moving the gates to
2021/2073. S14 then closed the catch-hidden eval-`arguments` under-rejection
(merge `13e925a75`, receipt [c2-s14-eval-hidden-outer.md](c2-s14-eval-hidden-outer.md);
focused eval-code 454/0, gates unchanged). S8 then landed the bounded
immutable-ArrayBuffer surface (merge `4ac863379`, receipt
[c2-s8-immutable.md](c2-s8-immutable.md): focused 84/38 default and
118/122 flag-on, adjacent +110 fail→pass / 0 pass→fail; JIT/TTD residuals —
post-JIT-merge probe (2026-10-31) shows sloppy indexed stores no-op and
DataView stores throw identically interpreted and under full JIT, but the
inlined fast-path audit is still not done, so JIT immutable write safety
remains unclaimed in general),
moving the gates to 2022/2074. S3-a then landed Unicode property escapes
under the `u` flag on ICU 79.1 (merge `1d92d7b37`, receipt
[c2-s3-property-escapes.md](c2-s3-property-escapes.md): bounded RegExp
corpus 1,020/960 → **1,814/166**, **794 fail→pass / 0 pass→fail**, with
the deterministic `MatchCodePointSet` repair removing the former 96
property-set timeouts), moving the gates to 2023/2075. S3-b then completed
the `v`/Unicode Sets surface (merge `dd60bdadd`, receipt
[c2-s3-v-flag.md](c2-s3-v-flag.md): v universe 346/28 default and 372/2
with RegExp-substrate flags; S3-a regression selection +14 fail→pass /
0 pass→fail; three bounded review-repair iterations closed shorthand
hangs, canonicalization, astral operands, deferred-parse errors and
deep-nesting crashes), moving the gates to 2024/2076. S9 then closed the
u-mode strict-syntax and astral semantics residual (merge `b8080f59f`,
receipt [c2-s9-u-strictness.md](c2-s9-u-strictness.md): +74 fail→pass /
0 pass→fail over 4,192 scenarios, including literals 480/0; review repairs
also closed a sticky OOB read and deferred identity-escape abort), moving
the gates to 2025/2077. S6c then added BigInt64Array/BigUint64Array
Atomics (merge `981fe000d`, receipt
[c2-s6-bigint-atomics.md](c2-s6-bigint-atomics.md): exact BigInt dirs
22/206 → 136/206; full Atomics+SAB selection 698/284 → 814/168 with
116 fail→pass / 0 pass→fail; BigInt/Atomics defaults remain off), moving
the gates to 2026/2078. S9-b then closed trail-surrogate literal scanning,
mid-pair manual `lastIndex`, and `v` fullUnicode handling across RegExp
builtins (merge `33b1192e7`, receipt
[c2-s9-codepoint-search.md](c2-s9-codepoint-search.md); focused matchAll
25/25 and u-strictness 21/21, all listed RegExp selections unchanged with
0 pass→fail; three bounded review repairs fixed a `v` matchAll/ES6-builtin
hang class, substring flag over-read, and proxy non-progress loops).
S6d then closed finite positive `waitAsync` timeouts (merge `dd6434127`,
receipt [c2-s6-waitasync-timeout.md](c2-s6-waitasync-timeout.md): additive
`JsSetTimerContinuationCallback`, recycler-owned timeout tasks, `waitAsync`
corpus 116/202 → 122/202 with **+6 fail→pass / 0 pass→fail**; cross-agent
remains). Gates remain **2026/2078** because existing focused entries were
extended. S2 then enabled `ESBigInt` by default (merge `f73012a01`, receipt
[c2-s2-bigint-default.md](c2-s2-bigint-default.md)): the 3,435-scenario
affected audit moved old-default 198/3,237 → default-on 2,531/904 with
**2,333 fail→pass / 0 pass→fail**; `-ESBigInt-` restored the old profile
0/0; review repairs added BigInt bytecode serialization, primitive debugger
display, and default/serialized/disabled/display pins, moving the gates to
**2030/2082**. A default-BigInt rerun of the class-elements corpus
(`out/test262-c2-s1-default-bigint`) then moved it **5,888/5,897 →
5,892/5,897**: the four former `-ESBigInt` parse-barrier scenarios now
pass; only the four post-ES2025 `accessor`-grammar scenarios and the one
Stage-1 non-extensible scenario remain. S6e then landed flag-on
cross-agent `Atomics.waitAsync` (merge `6900598e4`, receipt
[c2-s6-cross-agent-waitasync.md](c2-s6-cross-agent-waitasync.md)):
foreign notifies hand off through a native wake latch with
registrant-thread monitor tasks and per-child message queues;
POSIX `WScript.Quit` now flushes and raw-exits with no PAL/runtime
teardown, eliminating the deterministic Quit crash families; the
waitAsync test262 selection moved 122/80 → 201/1 (repeat 199/3) with
**0 pass→fail**, the merged Atomics+SAB selection is ≈895/982, and the
~0.3% pre-Quit agent-startup SIGBUS was diagnosed (three independent
confirmations) as a separate pre-existing engine race in concurrent
child `ScriptContext::Initialize` — routed to its own engine lane, not
hidden. The single new `s6-waitasync-cross-agent` entry moved the gates
to **2031/2083** (post-merge main `zig build check` 583/583 with
2031/0 test; debug-static 2083/0). The separate E1 engine lane then
diagnosed and fixed that startup SIGBUS (merge `3574b6168`, receipt
[e1-agent-startup-sigbus.md](e1-agent-startup-sigbus.md)): the x64
write-barrier card table's `OnThreadInit` rounded the thread-stack page
count down, so macOS ARM64's unaligned pthread stack tops left the top
partial page outside card-table section commit, and stack write barriers
on `WriteBarrierPtr` temporaries read uncommitted PROT_NONE card pages
during concurrent child `ScriptContext::Initialize` — deterministic
arithmetic, not a race (55/11200 interposed pre-fix → 0/2450 post-fix;
dual ACCEPT; gates unchanged 2031/2083). A conformance repair to S6e then
restored guaranteed FIFO **resolution** order across a foreign notify
pass (merge `02c45f214`, receipt section in
[c2-s6-cross-agent-waitasync.md](c2-s6-cross-agent-waitasync.md)):
tokens were latched FIFO but each resolved by its own 2 ms monitor timer,
so near-equal fires could invert order (30/2400 on debug, surfaced by the
post-E1 debug gate); the repair chains latched tokens at latch time under
the waiter-list CS and claims them head-first on the registrant thread
(0/2400 post-fix; 7,800 adversarial pass-observations clean; dual ACCEPT;
gates unchanged). The end-of-main join card was then closed by the
host-exit-join lane (merge `5f4e722d1`, receipt section in the S6e doc):
the PAL caps a single wait at 64 handles, so >64 accumulated agent
children failed the old aggregate join (debug assert; release-profile
delete-without-join UAF, 33/50 SIGSEGV at 100 agents) — the fix signals
all children then joins each individually and deletes only after
observing exit (50/50 clean, cap-edge 63–66 and 400-agent adversarial
scale clean, ≤64 semantics byte-identical; dual ACCEPT; gates unchanged).
The unchecked-`CreateThread` card was then closed by the host-spawn-check
lane (merge `a77b1a49d`, receipt section in the S6e doc): a failed agent
spawn hung the parent forever on the initial-script event (12/12 hangs
per profile, reproduced via a libc-boundary `EAGAIN` injection driving
the genuine PAL failure path); the fix rolls the failed child back on
the spawning thread and surfaces a catchable `JsErrorOutOfMemory`
(12/12 caught on both profiles, post-failure state consistent, nested
sub-agent spawns covered, dual ACCEPT; gates unchanged 2031/2083). Its
adversarial review filed one new pre-existing defect that was then
closed by the host-broadcast-type lane (merge `da9911395`, receipt
section in the S6e doc): `WScript.Broadcast` with a non-SharedArrayBuffer
argument SIGSEGVed 100% with live agents (unchecked
`JsGetSharedArrayBufferContent` left stale content that signaled
children dereferenced) — the fix validates before signaling, throws a
catchable sibling-shaped Error (15/15 SIGSEGV → 20/20 clean throws,
both profiles; validation matrix incl. proxies/growable/cross-runtime
clean), and after a review-found agentless handle leak adds a balanced
release (RSS A/B and the debug allocation-accounting assert both
confirm); the deterministic `s6-broadcast-type.js` rlexe entry moved
the gates to **2032/2084** (dual ACCEPT after repair). All active
feature/triage
worktrees are merged — including, as of 2026-10-31, the ARM64 macOS JIT
worktree itself. The S14 eval-residuals lane then closed the P4
body-eval item (merge `e0751e70d`, receipt
[c2-s14-eval-nonsimple-var.md](c2-s14-eval-nonsimple-var.md)): body eval
over non-simple parameters no longer over-reports `SyntaxError` (the
param/body scope split is now forced post-body-parse when the body calls
eval, with a compound-redeclaration guard), the eval preflight now
checks destructured catch bindings for all names (a review-found
pre-existing gap), and a parameter named `arguments` shadows correctly —
all Node-verified across 1,000+ review shapes; legacy non-conforming
fixtures updated; gates **2032/2084 → 2033/2085** via the new
`s14-eval-nonsimple-var.js` entry; dual ACCEPT after one repair
iteration. Next work items are
the remaining eval compound-family residuals (documented in the
receipt), lead-surrogate lacunarity (S3-decision-blocked), the
C4 qualification work (full baseline rerun, host-reported negative
phase), the c1-negative-phase lane delivered exactly that: `ch
-NegativePhaseReport` origin-classified phase markers with corpus-wide
re-judging (spoof-hardened after adversarial review), 25 controls, and a
fresh full pinned baseline (93,141 scenarios: **77,257/15,884**,
controls 25/25, 8,751 marker-judged negatives, 12 downgrades —
failures halved vs the C1-era 61,565/31,576 record; merge `c381c2e37`,
receipt [c1-negative-phase.md](c1-negative-phase.md); dual ACCEPT after
one repair). The marker also exposed two phase-fidelity engine gaps that
were then closed by the s12-module-phase lane (merge `8f17ba38d`,
receipt [c2-s12-module-phase.md](c2-s12-module-phase.md)): child module
link errors now propagate to root declaration-instantiation and JSON
modules parse at load — module-code 534/602 with all 10 wrong-phase
downgrades eliminated, `json-invalid` genuine, zero pass→fail, gates
unchanged 2033/2085, dual ACCEPT. Remaining C4 work: the
certification-grade full rerun. Next up: per-batch test262 runs under
SimpleJIT/FullJIT on the merged
Mac profile, and the JIT follow-ups recorded in
[arm64-macos-jit.md](arm64-macos-jit.md): eval declaration fidelity from
JIT frames (so direct-eval callers/eval code can JIT), native lowering
for the 18 aborting class/private/import opcodes, the
labeled-break-from-`finally` GlobOpt shape, the S8 immutable
inlined-store audit, and an ARM64 asm.js/WASM port or an explicit
reviewed platform exclusion. (The B5 bytecode-GUID rotation listed
here previously is done:
[b5-guid-rotation.md](b5-guid-rotation.md).) Tracked eval
residuals: P4 body eval with non-simple
parameters; P2 hidden outer lexical and JSC/Node delete difference.
S3 (Unicode data decision), S6 default (product/security decision),
B4 and C4 remain decision- or gate-blocked. A repair loop then closed
the accumulated alignment debt on Mac no-JIT: nested-plain-function
`super` inside eval and bare `await` in static initializers/blocks are
now early errors matching Node/spec in all parse modes, the
debug-static archive-member pin matches the S13 translation units
(526; full `check -Dprofile=debug-static` green again), every S1 test
file runs in all three parse modes under the gates, and the dead
`m_inStaticClassFieldInitializer` parser state was removed and later
revived with its await-region consumer. Kill-switch policy (recorded
decision): completed ES2025-required surfaces ship without feature
flags, matching the flag-cleanup direction — backout is by revert,
not by flag; surviving kill switches are the pre-existing per-feature
ones documented in their receipts (S4/S5).

**C2 batch status:** S4, S5, all of S10 (the copy-by-change half and
the Set-methods half), all of S11, S7, the five S9 sub-batches
(named-groups core, `d` flag/indices, duplicate named groups,
lookbehind, ES2025 modifiers), the bounded S8 Float16 slice
(`DataView.getFloat16`/`setFloat16` + `Math.f16round`) and the S14
block-scope + statement-level early-error
clusters are closed on Mac no-JIT
([c2-s4.md](c2-s4.md), [c2-s5.md](c2-s5.md), [c2-s11.md](c2-s11.md),
[c2-s10.md](c2-s10.md), [c2-s7-matchall.md](c2-s7-matchall.md),
[c2-s9-namedgroups.md](c2-s9-namedgroups.md),
[c2-s9-modifiers.md](c2-s9-modifiers.md),
[c2-s8-float16.md](c2-s8-float16.md),
[c2-s14-blockscope.md](c2-s14-blockscope.md),
[c2-s14-statement-early-errors.md](c2-s14-statement-early-errors.md)). JIT-wide
and Linux claims still wait on B4; Mac ARM64 JIT regression evidence for
these surfaces landed with the 2026-10-31 JIT merge (see the JIT evidence
update in §4.1); the per-batch test262-under-JIT obligation was later
discharged by the campaign recorded in
[c2-jit-test262.md](c2-jit-test262.md) (17/19 batches; Linux remains
B4's). S14 fixed the `Block : { StatementList }`
lexical/var early errors (block-scope corpus 167/287 → 287/287, 120/0
fail→pass) and moved the gates to 1976/2028. S14 sub-batch 2 then closed
the measured for-head, switch CaseBlock, catch-parameter and unlabelled
`with` early errors (68/0 fail→pass; receipt
[c2-s14-statement-early-errors.md](c2-s14-statement-early-errors.md));
catch pattern/var, eval-var and the broader defect bucket remain the S14
stream. S13 implemented symbols as `WeakMap`/`WeakSet` keys on Mac
no-JIT ([c2-s13-symbol-keys.md](c2-s13-symbol-keys.md)): unregistered symbols use a lazy, traced per-collection recycler dictionary
(object keys keep the weak path; WeakSet uses the non-leaf allocator),
registered `Symbol.for` symbols are rejected per ES2023
`CanBeHeldWeakly`, and diagnostics/TTD count and enumerate symbol
entries. The scoped corpora moved WeakMap 214/281 → 232/49 with 18/0
fail→pass and WeakSet 154/170 → 168/2 with 14/0; the WeakMap residue is
47 ES2026 `getOrInsert*` scenarios plus 2 pre-existing
`proto-from-ctor-realm` failures, and WeakSet has the same 2 cross-realm
failures. Gates moved 1976/2028 → 1977/2029; `WeakRef`/
`FinalizationRegistry` remain C3. The S2/S6/S7 enabled-flag measurement audits
([c2-flag-audits.md](c2-flag-audits.md)) drove the S7 implementation
batch: `Symbol.matchAll`, `RegExp.prototype[@@matchAll]`, the RegExp
String Iterator and `String.prototype.matchAll` all landed behind
`-ES6RegExSymbols` together with the two flag-on defect-class fixes
(primitive `Symbol.*` reads and the `lastIndex` `[[Set]]` bypass,
including honoring a non-writable `lastIndex` redefine under the flag)
— the audit's 42 flag-on regressions are 42/42 fixed with 0 new
pass→fail (136 same-binary fail→pass on the 6,199-scenario universe,
matchAll corpus 132/140; the 8 residue is BigInt/v-flag parse debt
plus 4 `species-regexp-get-*` scenarios that need the
`-ES6RegExPrototypeProperties` substrate). The flag stays off by
default; the batch also regenerated the checked-in nojit bytecode
headers (three PropertyIds appended; the jit variants were regenerated
as part of the 2026-10-31 JIT merge). S9 sub-batch 1 then landed the named-groups core
unconditionally (`(?<name>...)`, `\k<name>`, the `groups` object on
exec/match results, `$<name>` substitution on both replace paths;
receipt [c2-s9-namedgroups.md](c2-s9-namedgroups.md)): corpus 158/184
default flags and 162/184 under `-ES6RegExSymbols` with the residue
being exactly the deferred S9 sub-batches (duplicate names, lookbehind,
flag-gated subclass scenarios), and resym-universe joins of 168/0 and
194/0 fail→pass/pass→fail. S9 sub-batch 2 landed the `d` flag / match
`indices` unconditionally (the `indices` array on exec/non-global-match
results with `indices.groups` mirroring, `d` parsing and canonical
`dgimsuy` flag-string order in all four builders; `hasIndices` + the
`flags` getter's `d` under `ES6RegExPrototypeProperties`, corpus-pinned;
same receipt, "Sub-batch 2"): `RegExp/match-indices` 28/28 on both flag
matrices, hasIndices/flags prototype dirs 42/48 under the proto flag
(residue 4 BigInt `4n` + 2 `v` flag), resym joins 28/0 and 28/0, gates
moved to 1972/2024. S9 sub-batch 3 landed ES2025 duplicate named
groups unconditionally (duplicate names parse iff a disjunction
separates them; `groups`/`indices.groups`/`$<name>` resolve to the
last participating group; `\k<name>` with duplicates compiles to a
regex-internal `MatchNamedGroup` instruction with the
participation-guard semantics — no JS-bytecode or PropertyId change;
same receipt, "Sub-batch 3"): the named-groups corpus set is 182/8
default and 188/2 under symbols (residue 2 lookbehind + 6
symbols-gated), match-indices stays 28/28, resym joins 28/0 and 30/0
incremental (224/0 and 252/0 vs C1), gates moved to 1973/2025. S9
sub-batch 4 landed ES2018 lookbehind assertions unconditionally
(`(?<=`/`(?<!` parse, never quantifiable; the body is mirrored at parse
time and run right-to-left as a sub-match over a reversed window, with
frame-aware `^`/`$`/`\b` and real-coordinate capture spans; lookaheads
inside lookbehinds run over the real suffix, nested lookbehinds become
ordinary lookaheads — reverse-order alternation/greed/capture and
mirrored-backreference semantics verified against Node; no JS-bytecode
or PropertyId change; same receipt, "Sub-batch 4"): the whole
`RegExp/lookBehind/` directory passes, the 190-scenario set is 118/6
default / 124/124 under symbols (residue only the 6 long-classified
symbols-gated scenarios), match-indices+duplicate-names 30/30, joins vs
C1 96/0, 102/0, 28/0, resym universe 4,113/2,086 with 260/0 vs C1
(36/0 incremental), gates moved to 1974/2026. S9 sub-batch 5 landed
ES2025 scoped modifiers unconditionally (`(?ims-ims:Disjunction)`,
non-capturing, nested add/remove with the top-level flags as the base;
per-node effective i/s/m state drives parsing and the instruction
compiler, including frame-aware anchors and word boundaries inside
lookbehinds; whole-program special cases are disabled for modified
patterns; no JS-bytecode or PropertyId change; receipt
[c2-s9-modifiers.md](c2-s9-modifiers.md)): the scoped corpus moved
166/140 to 298/8 with 132 fail→pass and 0 pass→fail, the 8 residue
being property escapes deferred to S3, and the focused test/gates
moved to 1978/2030. The remaining S9 work (`v` flag after S3) stays
C2-per-batch.
S2 sub-batch 1 then landed the BigInt conversion/stringification/crash
slice on Mac no-JIT (`BigInt(...)`/ToBigInt, multi-digit NumberToBigInt,
StringToBigInt, `typeof "bigint"`, `toString` radix 2–36, `valueOf`,
`@@toStringTag`, wrapper `ToObject`, correctly rounded `Number(BigInt)`
(including `Number(Object(1n))`), `ToBoolean(0n) === false`, JSON BigInt
`toJSON`/replacer handling and
cross-realm wrapper `valueOf` marshal, and controlled `TypeError`s for
unsupported operator paths; receipt
[c2-s2-bigint.md](c2-s2-bigint.md)). The scoped corpus is 94/154 (32/154
pre-slice, 68/154 at the interrupted-slice point) with 62/0 fail→pass vs
pre-slice and 26/0 vs the interrupted slice; the 40-SIGABRT crash class
is gone and the 6 JSON BigInt files pass 12/12 on both binaries. The 60
residual failures are exactly missing `asIntN`/`asUintN` (56;
`arithmetic.js` also needs non-decimal BigInt literal parsing), wrapper
arithmetic/equality (2), and the fail-closed mixed relational comparison
in `toString/a-z.js` (2). Gates moved to 1975/2027. BigInt arithmetic,
BigInt64Array, Map/Set key identity, mixed comparison and the literal
grammar stay C3-large.
S2 sub-batch 2 then implemented `BigInt.asIntN`/`asUintN` on Mac no-JIT (working tree)
(ToIndex coercion/order, descriptors, non-constructibility, exact
limb-bounded two's-complement truncation, and the spec
`ToBigInt(Number)` `TypeError` correction; receipt
[c2-s2-bigint.md](c2-s2-bigint.md)). The scoped corpus moved 94/154 to
146/154 on both binaries with 52/0 fail→pass; the 8 residue are the four
`asIntN`/`asUintN` `arithmetic.js` scenarios blocked by non-decimal
BigInt literals, wrapper arithmetic/equality (2), and the mixed
relational loop in `toString/a-z.js` (2). The 150/154 target needed the
then-excluded literal parser work. Gates moved 1978/2030 → 1979/2031.
S2 sub-batch 3 then added `0x`/`0o`/`0b` (both prefix cases) BigInt
literal parsing with numeric-separator placement, rejection of leading-zero
decimal forms, the decimal `1_2n` separator fix, and Node-matching syntax
errors on Mac no-JIT (receipt [c2-s2-bigint.md](c2-s2-bigint.md)).
The scoped corpus moved 146/154 to 150/154 on both binaries, exactly the
four `asIntN`/`asUintN` `arithmetic.js` scenarios (4/0 fail→pass); the 4
residue are wrapper arithmetic/equality (2) and the mixed relational
loop in `toString/a-z.js` (2). Gates moved 1979/2031 → 1980/2032.
S2 sub-batch 4 then implemented mixed BigInt/Number abstract equality
and relational comparison plus wrapper multiplication on Mac no-JIT
(exact large-number conversion, NaN/Infinity/fractional handling,
left-first ToPrimitive, exact StringToBigInt equality and relational
comparison, and wrapper/object `*`; receipt
[c2-s2-bigint.md](c2-s2-bigint.md)). The scoped corpus moved 150/154 to
**154/154** on both binaries, exactly the four `toString/a-z.js` and
`wrapper-object-ordinary-toprimitive.js` scenarios (4/0 fail→pass).
Gates moved 1980/2032 → 1981/2033. Full BigInt arithmetic/bitwise/shift,
BigInt64Array, Map/Set key identity, literal property names, giant
`ToString` performance and JIT/B4 remain C3-large.
The bounded S8 Float16 slice then landed ES2025 `DataView.prototype.
getFloat16`/`setFloat16` and `Math.f16round` on Mac no-JIT with a
portable integer binary64↔binary16 converter (round-to-nearest,
ties-to-even, subnormals, overflow/underflow, NaN canonicalization;
no `__fp16`; dynamic property names, so no PropertyId or bytecode
change) and `test/es7/float16.js` (receipt
[c2-s8-float16.md](c2-s8-float16.md)). The pinned 100-scenario corpus
moved 22/100 to 94/100 with 72/0 fail→pass; the 6 residue are the 4
`resizable-buffer` scenarios (S8 proper) plus 2 post-ES2025
`immutable-buffer` scenarios. `Float16Array` and typed-array element
support stay in the remaining S8/C3 work. Gates moved
1981/2033 → 1982/2034.
S6 (`ESSharedArrayBuffer=true` default-on as of `7412beb56`, verified on
`refresh-2026-11-16` with `1,098` `disabled-flag` failures cleared and a
`94`-test flag-on tail across `disabled-flag` and `host`), plus
waitAsync/growable/non-shared-Atomics work; S2's flagged path no longer
has a host-crash class but its arithmetic/typed-array completion still
stays C3-large. With the S1 class-elements surface complete (2026-10-20
checkpoint above), the next batch candidates are the S12
import-attributes cluster (pending its host-contract decision),
Float16Array (S8 remainder), or the S1 tail residuals.

**Historical B4 milestone note (completed):** B4 native execution completed
on the Linux proof host on 2026-11-09/10 ([b4-linux-run.md](b4-linux-run.md))
and `G0` baseline Linux revalidation completed at `1bf42d1f6` (merged into
`modernize/phase4`). Current Phase-4 Linux (`G0-phase4`) and Windows (`B6`)
validation tracking lives in [linux-handoff.md](linux-handoff.md) §0A,
[windows-handoff.md](windows-handoff.md), and [open-cards.md](open-cards.md).

## Appendix — Evidence and deferred research

Important source anchors: `CMakeLists.txt`, `lib/CMakeLists.txt`,
`bin/ch/CMakeLists.txt`, `test/CMakeLists.txt`, `pal/src/configure.cmake`,
`pal/src/config.h.in`, `Build/CMakeFeatureDetect.cmake`, `tools/regenByteCode.py`,
`lib/Jsrt/ChakraCore.h` / `ChakraCommon.h` / `ChakraDebug.h`,
`lib/Runtime/Base/ThreadContextTlsEntry.cpp`, `lib/SCACore/SCACore.h`,
`lib/Common/ConfigFlagsList.h`, `azure-pipelines.yml` and `.cirrus.yml`.
Detailed baseline line references and inventories live in the linked reference docs.

Still to qualify in their milestones: platform SDK/libc floors and native CI,
Windows tool/exception/RPC behavior, LTO/plugins/trace, release/Android packaging,
real translate-c integration for a chosen Zig unit, and required-edition/host
conformance. ZLS, incremental-link behavior and exact V8/JSC/QuickJS-ng internals
are non-gating research. Verify external comparison claims from primary sources
before using them; competitor architecture is not a product acceptance criterion.
