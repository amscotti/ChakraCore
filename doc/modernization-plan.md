# ChakraCore Modernization Plan

> Status: F0–B3 verified — three Mac profiles green, incremental proofs,
> CI recipe validated (CONDITIONAL on the first green post-merge Actions run);
> **B4 is BLOCKED-with-green-build** — the Linux Zig build is green, but zero
> behavior checks have run anywhere ([B4 gate verdict](b4-gate.md),
> [post-B4 review](b4-review.md)). **C1 is recorded (CONDITIONAL)** — edition
> policy, triage and shortlist are in [c1-triage.md](c1-triage.md);
> qualification is conditional on explicit host-reported negative phase
> before any full result is certified. Engine/legacy-build references use baseline
> `97e68c917`; execution evidence and the [B1-B3 review](b1-b3-review.md)
> are recorded through 2026-10-12 local time.
> This is a staged roadmap for a small maintained fork, not a dated delivery promise.

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
| B3 | Mac build profiles, incremental correctness and CI | B2 | Verified* |
| B4 | Native x64 Linux JIT/WASM/WABT proof and Zig build | B2; B3 before default rollout | **BLOCKED-with-green-build** (native baseline pending) |
| B5 | First release package with controlled dependencies | B3; B4 for a two-platform package | Planned |
| B6 | Windows feasibility and rollout; later target extensions | Stable build contracts | Separate expansion |
| Z1 | One useful mixed C++/Zig unit behind a tested C ABI | B3; B4 if the unit ships on Linux | Optional pilot |
| C1 | Qualified ES2025 test policy and triaged failures | Stable Mac build at B3 | Recorded (CONDITIONAL) |
| C2 | Bounded compatibility batches and partial releases | C1; B4 before JIT-wide enablement | Planned |
| C3 | Larger runtime/object-model feature projects | C1 and their feature prerequisites | Separate projects |
| C4 | Verified full ES2025 core-language result | Required C2/C3 work and qualified runner | Long-term objective |

**Default execution order:** F0 → B1 → B2 → B3 → B4. Start C1 after B3;
release/dependency work at B5 and an approved Z1 pilot can then be scheduled without
holding up C++ compatibility fixes. B6 is an expansion decision, not a prerequisite
for completing the first Mac/Linux build rollout. All entries marked planned are
future work; command names below describe the intended build interface.
(*B3's CI clause is CONDITIONAL on the first green post-merge Actions run;
the recipe is validated by lint plus literal local simulation.)

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
  feature/host requirements in Phase 3 need an explicit API/versioning decision.
- DOM/Node hosting, JIT/GC rewrites, V8/JSC internal-design parity and a language
  migration percentage are outside this roadmap's completion gates.
- Intl/ECMA-402 and WASM extensions have their own milestones and coverage claims.
  Their entire future feature sets do not gate ES2025 core-language completion.
- Modern OS/tool floors may be raised deliberately. A supported-platform claim
  requires a declared contract and testing on the relevant platform/floor.

## 1. Current evidence and its limits

| Evidence | What it establishes |
|---|---|
| Legacy ARM64 Mac `ninja check`: 1959/0, bytecode and shared embedding checks | Behavior-parity reference for the shared/no-JIT Test configuration |
| Zig 0.17.0: 538/538 units, three linked binaries, 1959/0 plus bytecode/native checks | C++17/C/assembly toolchain capability for that Mac configuration |
| Explicit `aarch64-macos.26.0`, SDK 26.5, ICU 78.3, 8 MiB stacks and callback retention | Verified local build inputs and artifact metadata; not testing every 26.x host |
| Pinned test262 diagnostic: 61,565 passed / 31,576 failed in 93,141 scenarios | A reproducible place to investigate feature debt; not an ES2025 score |
| Sources inspected at `97e68c917` | Architecture/build map and current conditional feature paths |

The Mac pilot does not compile Backend, WasmReader or WABT. It still obtains
configuration through CMake before replay; B1/B2 must make Zig own that work.
Windows, other CPUs, LTO and a hermetic dependency package remain
unproved; Linux x64 JIT compiles and links in the B4 Zig graph, but its
behavior is unproved — zero Linux behavior checks have run (B4 is
BLOCKED-with-green-build). The number of diagnostic failures is neither the number of bugs nor an
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
system ICU**, followed by **native x64 Linux with JIT**. Work in small build-graph
slices and compare each against the recorded reference configuration.

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
- The first active Zig matrix is Mac ARM64/no-JIT and Linux x64/JIT after their
  gates pass. Windows keeps its legacy MSBuild route until its own rollout is
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
manifest. A claim of byte-identical reproducibility additionally needs two-build
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
recorded pre-fix, so rerunning the affected subsets is owed before any C4
certification ([c1-triage.md](c1-triage.md) §3/§8).

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
  on JIT configurations (B4 supplies that environment);
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
S10 change-array-by-copy + Set methods — [c2-s10.md](c2-s10.md)
(closed: interpreter-verified, unconditional library methods, no
feature flag and no kill switch, no new opcodes). The default carries Mac no-JIT evidence only: **B4 must run both S10
test262 corpora and `test/es7/change-array-by-copy.js` and
`test/es7/setmethods.js` under
SimpleJIT/FullJIT (dynapogo) before any JIT-configuration claim.**
Documented gaps, not defects in the fourteen methods: the typed-array
default comparator treats `-0` and `+0` as equal (shared with
`.sort()`); `ESBigInt` is off; resizable ArrayBuffer is S8.

#### C3 — Larger runtime projects

Class fields/private elements/static blocks, WeakRef/FinalizationRegistry/job
retention, resizable buffers/typed arrays, and advanced RegExp/module work need
their own design and experiments. Split implementation, interpreter correctness,
JIT coverage, GC/ownership invariants and default enablement into reviewable parts.

Do not bundle these into one change or assume they are leaf migrations. Keep
experimental flags until their active-matrix tests pass. If a required host/API
contract affects embedders, specify versioning and compatibility before rollout.

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
| ES2015–ES2019 | modules/generators, async/await/iteration, rest/spread, dotAll, flat/flatMap, fromEntries | named groups, lookbehind, property escapes; RegExp symbols/prototype properties default off; inherited semantics → P0/C1 triage |
| ES2020 | `??`, import(), import.meta, globalThis, allSettled | optional chaining, matchAll, BigInt completeness/default; for-in formalization → P0 |
| ES2021 | numeric separators, Promise.any | replaceAll, logical assignment, WeakRef/FinalizationRegistry → P0/P1 |
| ES2022 | .at, hasOwn, cause, TLA | class fields/private elements/static blocks, RegExp d → P0 |
| ES2023 | findLast, hashbang | copy-by-change arrays/typed arrays, weak-collection symbol keys → P0/P1 |
| ES2024 | no complete coverage established | groupBy, withResolvers, well-formed strings, v, resizable/transfer buffers, waitAsync → P1 |
| ES2025 | no complete coverage established | escape, eval-var semantics, Float16, try, iterator helpers, JSON modules/import attributes, modifiers, Set methods, duplicate groups → P1 |
| ES2026 | stretch after required scope | upsert, JSON.parse source, sequencing, base64, sumPrecise, isError, fromAsync |

**P0 — High-breakage compatibility:** optional chaining/logical assignment;
class syntax; replaceAll/matchAll and the copy-by-change family; inherited
ES2018 RegExp gaps and later `d`/import-attribute syntax. Named groups precede
duplicate named-group behavior. Runtime probes reject named groups, lookbehind,
property escapes, `d`/`v` and import attributes. Audit BigInt, SAB/Atomics and
RegExp-symbol/prototype-property flags through enabled conformance runs.

**P1 — Required remaining core and independent ecosystem work:** ES2024/2025
workstreams above, WeakRef/FinalizationRegistry and weak-collection symbol keys.
Eval-variable redeclaration is a semantic triage item, not a name-search absence
claim. Separately expand Intl (RelativeTimeFormat, ListFormat, DisplayNames,
Locale, Segmenter, DurationFormat) and decide maintained ICU acquisition/versioning;
separately stabilize WASM multi-value, SIMD and threads, then assess further proposals.

**P2 — Longer-range architecture:** ARM64 JIT off Windows, additional platforms,
and researched performance/memory designs. Sandboxing/pointer compression and
competitor tier counts are research, not this migration's completion gates.

**P3 — Release/tooling health:** publish results and feature manifests as batches
land; improve embedding samples and host ergonomics; refresh old links/images/tool
floors alongside their relevant build milestone. A dated supported package can
ship at B5, well before C4.

Decorators remain Stage 2.7 and explicit resource management/Temporal are later
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
  scoped default rollout. (*CONDITIONAL on the first green post-merge
  Actions run; recipe validated by lint + literal local simulation.)
- [ ] B4: Linux native baseline/pilot and JIT/WASM/native/profile CI gates;
  scoped Linux rollout. (**BLOCKED-with-green-build**: the Zig build is
  green, but zero behavior checks have run — [b4-gate.md](b4-gate.md).)
- [ ] B5: Controlled dependency/artifact package and clean-machine floor testing.
- [ ] B6: Windows feasibility, then declared-profile parity; further expansions
  approved individually.
- [ ] Z1: One useful C-ABI integration, **if selected**; further replacements
  decided independently.
- [x] C1: Edition policy, host/negative-phase qualification and root-cause shortlist.
  (*CONDITIONAL on explicit host-reported negative phase before any full
  result is certified; policy, triage and shortlist are recorded in
  [c1-triage.md](c1-triage.md), qualification limits in its §3.)
- [ ] C2/C3: Feature-specific receipts and partial releases; required work remains
  tracked until C4, optional ecosystem scope has separate gates.
- [ ] C4: Published qualified ES2025 result on the claimed default configurations.

**C2 batch status:** S4, S5, S11 `String.prototype.replaceAll`, and
all of S10 (the copy-by-change half and the Set-methods half) are
closed on Mac no-JIT
([c2-s4.md](c2-s4.md), [c2-s5.md](c2-s5.md), [c2-s11.md](c2-s11.md),
[c2-s10.md](c2-s10.md)). JIT claims still wait on B4. The next bounded
C2 batch is drawn from the remaining S11 builtins; `matchAll` stays
behind S7.

**Next platform gate:** B4 native execution. Provision a native x64 Linux
runner via the preferred path to green in [b4-gate.md](b4-gate.md) §4(a),
record the legacy then Zig interpreted/dynapogo counts natively, and judge
behavior/ABI parity only from that native execution — not from emulation
or cross-linking from Mac.

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
