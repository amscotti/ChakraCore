# WebAssembly on macOS ARM64 — implementation plan

Status: plan, not started. Owner: unassigned. Last updated: 2026-11-11
(revised same day twice: first after review — feature/test manifest,
asm.js gate matrix, runner capability gating, P1 command, dependency
honesty; then after the Ember research swarm — D7 tokenization and
fast-array/MathEx pins, runner-tag applicability in the P1 set,
`WasmLibrary.cpp` transplant, D2 disjunct definition, quantified zlib
gate, defined exit terms, W0-before-W1 ordering).
Governing docs: [modernization-plan.md](modernization-plan.md) (this port
is listed there as P2, "an ARM64 asm.js/WASM port"),
[memory-management.md](memory-management.md) (read before touching
allocation, GC tracing/barriers, stack walking, or host boundaries),
[build-contract-mac.md](build-contract-mac.md) (Mac build graph).

Prerequisite lane: landed as commit `97ee0be80` (ARM64 JIT software
write-barrier fix in `lib/Backend/Lower.cpp`,
`lib/Backend/arm64/LowerMD.{h,cpp}`, `test/es7/jit-write-barrier.*`;
bench harness/receipt in `tools/bench/`,
[perf-baseline.md](perf-baseline.md); this plan). This plan depends
on both halves (W6.3 reuses the barrier path; success criterion 3
re-records the receipt). Execute only against a tree containing
`97ee0be80` or later.

## Goal

Make the `WebAssembly` JS API and asm.js work on the macOS ARM64 build
(`zig build -Dprofile=jit` and the test profile), to the feature level
the in-tree test suites define, with correctness proven by those suites
running on ARM64 — first interpreter-only, then JIT-compiled.

Interpretation note: this plan ports the engine's existing WebAssembly
support to ARM64. It does not compile ChakraCore itself to wasm (that
would be an Emscripten/PAL project with no JIT; out of scope, see
Non-goals).

## Success criteria (overall definition of done)

1. `WebAssembly.compile/instantiate/validate`, `Module`, `Instance`,
   `Memory`, `Table`, `Global`, and the compile/runtime error types
   behave on darwin/arm64 as they do on Linux x64 for the full
   in-tree feature set (D1): MVP + sign-extension
   (`-WasmSignExtends`, default on) + non-trapping float-to-int
   (`-WasmNontrapping`, default on) + multi-value + SIMD + threads.
   Threads are *built and tested* behind `-WasmThreads` /
   `-ESSharedArrayBuffer` (both default off today); whether they are
   ever default-on is the §9.2 product decision, not part of this
   criterion.
2. The suites in §5 pass on darwin/arm64 in the variants each phase
   claims, with counts recorded in a receipt next to this file.
3. asm.js (`"use asm"`) modules validate and run on darwin/arm64,
   with tier engagement *proven*, not inferred from wall time: the
   P2 gate runs `Octane/zlib.js` with the asm.js test trace
   (`-testtrace:asmjs`, as used by `test/AsmJs` entries) and asserts
   the trace shows the module validated/compiled as asm.js (a
   plain-JS fallback emits no such trace), **and** the bench median
   wall time over 5 JIT-tier runs is ≤ 20 s on the receipt host.
   The darwin/arm64 exclusion in `tools/bench/run.py` is then
   removed with a re-recorded receipt in
   [perf-baseline.md](perf-baseline.md) (both files are the
   prerequisite lane named in the header). Completion under a 60 s
   timeout alone proves nothing: fallback can finish inside it.
4. No regression to existing gates: `zig build check` counts change
   only by deliberate, recorded additions (see `tools/build/gate_counts.json`
   lineage rules); `zig build -Dprofile=jit` and `-Dprofile=debug-static`
   stay green; Linux x64 WASM behavior is unchanged (shared code edits
   are gated or arch-neutral).
5. GC safety evidence, not just test green: asm.js/WASM frames under
   concurrent GC and GCStress are exercised deliberately (workstream
   W6), with ownership records per memory-management.md.

## Non-goals

- Modern WebAssembly beyond the in-tree surface: no GC proposal, no
  exception handling, no memory64, no tail calls, no component model.
  The port target is this engine's WASM, not current WebAssembly.
- Compiling ChakraCore itself to wasm; iOS (no JIT entitlement path);
  Windows ARM64 (separate platform proof).
- Performance parity with V8. Correctness first; perf is measured
  (§8), not gated, until a later receipt says otherwise.
- Replacing the WASM tier with an external interpreter library. That
  contradicts the tree's minimal-dependency posture and would still
  need the same JS-API, GC, and stack-walking integration; rejected
  unless a future decision record overrides this plan.

## 1. Current state (verified 2026-11-11)

All paths below were read in-tree; line numbers are as of base commit
`61b6af3f6` on `modernize/baseline`.

| Layer | State | Evidence |
|---|---|---|
| Feature gate | `ASMJS_PLAT` defined only for `_M_IX86`/`_M_X64` && !`DISABLE_JIT`; `ENABLE_WASM`, `ENABLE_WASM_THREADS`, `ENABLE_WASM_SIMD` hang off it | `lib/Common/CommonDefines.h:721-728` |
| Feature flags | Runtime flags refine the surface per module: `WasmSignExtends` default **true**, `WasmNontrapping` default **true**, `WasmThreads` default false, `ESSharedArrayBuffer` default false. `test/WasmSpec/features/` has `extends`, `nontrapping`, `threads` subdirs | `lib/Common/ConfigFlagsList.h:443,445-446,712,986-989` |
| asm.js sources | All 8 `AsmJs*.cpp` (parser/validator, bytecode gen, module, link, types, utils, codegen, encoder) are wholesale `#ifdef ASMJS_PLAT` — they compile **empty** on ARM64. Enabling `ENABLE_WASM` alone does not bring up asm.js; the gate matrix (D2) must split arch-neutral asm.js logic from x86/x64 JIT emission | `lib/Runtime/Language/AsmJs.cpp:24`, `AsmJsByteCodeGenerator.cpp:8`, same guard in `AsmJs{CodeGenerator,Encoder,Link,Module,Types,Utils}.cpp` |
| Fast array buffer | `ENABLE_FAST_ARRAYBUFFER` is Windows-only (`_WIN32 && TARGET_64 && !_M_ARM64`); token-wise there are 3 fast-array entries in `test/wasm` (1 enabled form `:68`, 2 disabled forms `:62,:360`) plus disabled-form variants in WasmSpec. Only the **enabled** form is inapplicable on macOS/Linux; disabled forms exercise D3's generic path and stay (D7 token map) | `lib/Common/CommonDefines.h:326` (TODO comment: "Disable VirtualTypedArray on ARM64 till we make sure it works correctly"); `test/wasm/rlexe.xml:62,68,360` |
| Test runner | `arch` is force-set to `'x64'` on every non-Windows host, so the arm/arm64 `require_asmjs` exclusion never fires on this Mac, `exclude_arm64` tags never apply, and baseline `<condition><target>` overrides only ever compare against the `amd64` alias (an `arm64` target condition already exists in-tree and is dead code). Suites therefore *run* on Mac today and fail for missing engine support, not runner gating | `test/runtests.py:112-115` (force x64), `:188-189` (dead exclusion), `:159-162,676-694` (alias/override); dormant template `test/typedarray/rlexe.xml:149-160` |
| Mac build | WasmReader library not built; Runtime gets only `WasmByteCodeWriter.cpp` + `WebAssemblySource.cpp` and a `lib/WasmReader` include path | `tools/build/tables.zig` (2 wasm-adjacent files); `doc/build-contract-mac.md:182,919` |
| Linux x64 build | Full inventory exists and is the transplant source: 11 WasmReader units, 6 `WebAssembly*.cpp`, `WebAssemblySource.cpp`, vendored WABT group | `tools/build/tables_linux.zig:545-563,887-892,1051+`; `tools/build/linux.zig:8,162-190` |
| WABT | Vendored at `lib/wabt`; `CAN_BUILD_WABT` → `ENABLE_WABT` under `ASMJS_PLAT` | `CMakeLists.txt:283,323`; `CommonDefines.h:730-732` |
| Frontend | `WasmBinaryReader` (1423 lines), `WasmByteCodeGenerator` (2292), sections/signature/global/data/element readers — arch-neutral by inspection, unproven on ARM64 | `lib/WasmReader/` |
| JS API / runtime | `WebAssembly{,Module,Instance,Memory,Table,Environment}.cpp` exist; compiled only on Linux/Windows tables today | `lib/Runtime/Library/WebAssembly*.cpp` |
| Interpreter | `OpCodeAsmJs` handlers exist (`InterpreterHandlerAsmJs.inl`, 771 lines), but entry is x86 inline asm / x64-only; non-x64 fallback is `AssertOrFailFast(UNREACHED)` with comment "Asm.js only supported on x64 and x86" | `lib/Runtime/Language/InterpreterStackFrame.cpp:1638-1765` (x86 `__asm` thunk at 1639; fallback at 1760-1765) |
| JIT backend | ARM64 lowerer has 17 `Assert(UNREACHED)` stubs for the asm.js/WASM surface (calls, elem ld/st helpers, bounds check, atomic ld/st, ctz/popcnt/trunc-check, float32/int64 converts, sign-extend, reinterpret, asm exit); x64 reference implementations exist | `lib/Backend/arm64/LowerMD.h:104-202`; `lib/Backend/amd64/LowererMDArch.cpp` (3500 lines; `LowerAsmJsCallE/I` at 1083/1094, elem helpers at 1189/1290) |
| ABI/runtime | `AsmJsCallStackLayout` is a bare `{functionObject, Var args[]}`; optimized-buffer path is `_M_X64`-only with `Assert(UNREACHED)` elsewhere (generic false branch exists) | `lib/Runtime/Language/JavascriptStackWalker.h:37-41`; `lib/Runtime/Language/AsmJsModule.cpp:1841-1849` |
| Tests | Suites exist and are tag-gated off ARM64 (see §5) | `test/rlexedirs.xml`, `test/runtests.py:188-189` |

Corrections to the kickstart note this plan was seeded from (verified,
so sub-agents do not chase ghosts):

- There is no `test/wasmthreads` directory. Threads/atomics coverage
  is `-ESSharedArrayBuffer`-flagged entries inside `test/WasmSpec`
  (12) and `test/wasm` (2). `ESSharedArrayBuffer` defaults false
  (`lib/Common/ConfigFlagsList.h:712`).
- The ARM64 JIT gap is 17 stubbed lowerer methods, not four.
- The Mac source table is `tools/build/tables.zig`; there is no
  `tables_macos.zig`. The Linux table is the copy source.

## 2. Key decisions

D1. **Full surface committed; interpreter-first sequencing.**
Decided 2026-11-11: the end-state is the full in-tree surface —
JIT + SIMD + threads (P2 and all of P3 are committed scope, not
optional extras). Sequencing is still interpreter-first: the
interpreter tier unblocks the JS API, the spec suite, and every
later phase's differential testing, at a fraction of the JIT cost,
and serves as the correctness oracle for the JIT. Consequence to
state plainly: interpreted WASM/asm.js is slow — Octane `zlib.js`
will *pass* but stay slow until the JIT phase. Do not remove the
bench exclusion at the interpreter milestone; record a
timeout-scoped note instead. "Phased" below describes gate order,
not optional scope: the project is not done at P1 or P2.

D2. **Gate matrix — decided here, implemented in W0.** The blocker:
all 8 `AsmJs*.cpp` are wholesale `#ifdef ASMJS_PLAT`, so enabling
`ENABLE_WASM` on ARM64 alone leaves asm.js compiling empty while
W2/W3 require it to validate and link. The matrix splits three
concerns `ASMJS_PLAT` currently conflates:

| Gate | Covers | x86/x64 | ARM64 |
|---|---|---|---|
| `ENABLE_WASM` | WasmReader frontend, `WebAssembly` JS API, wasm bytecode generation | on (via `ASMJS_PLAT` today) | **on from P0** (behind the W0 build option) |
| `ENABLE_ASMJS` (**new**) | asm.js parser/validator, `AsmJsByteCodeGenerator`, module/link/types/utils, interpreter execution path — everything arch-neutral in the 8 `AsmJs*.cpp` | on (defined wherever `ASMJS_PLAT` is) | **on from P0** with `ENABLE_WASM` |
| `ASMJS_PLAT` (narrowed meaning) | x86/x64 asm.js JIT emission: `AsmJsCodeGenerator`/`AsmJsEncoder` native emission, x64 thunks, backend lowering entry points | on, unchanged | off until W4/W5 replace each use; ARM64 JIT emission gates on the normal `ENABLE_NATIVE_CODEGEN` + workstream completion, not on `ASMJS_PLAT` |

Mechanics: W0 audits every `ASMJS_PLAT` use in the 8 files (plus
headers and call sites) and re-gates each as `ENABLE_ASMJS`
(arch-neutral) or keeps `ASMJS_PLAT` (JIT emission).
`ENABLE_ASMJS` is defined as the disjunction
`defined(ASMJS_PLAT) || (defined(_M_ARM64) && defined(<wasm build-option define>))`
(W0 fixes the option define's spelling). On x86/x64 the second
disjunct is always false, so their macro state is byte-for-byte
today's (D6); on ARM64 the disjunct is precisely what turns the
arch-neutral asm.js logic on while `ASMJS_PLAT` stays off. Any use
that is neither cleanly neutral nor emission (e.g. layout constants
in `AsmJsTypes`) is classified in the W0 inventory and resolved
against the W1 ABI before P1. Ordering inside P0 is therefore:
W0 inventory/classification **first**, W1 ABI spec finalized
against it (W1 reading may start in parallel; its spec may not be
declared final until the inventory lands).
`ENABLE_WASM_SIMD`/`ENABLE_WASM_THREADS` stay undefined on ARM64
until W8/W9. While JIT lowering is stubbed, stubs fail fast in a
controlled way (W2 guardrails), never silently miscompile.

D3. **Generic typed-array buffer path first.** Port with
`isOptimizedBuffer = false` on ARM64 (the existing `#else` shape in
`AsmJsModule.cpp:1846-1848`, minus the assert). The x64
optimized/virtual-buffer path is a perf optimization, not a
correctness requirement; revisit only with bench evidence (§8).

D4. **One ABI, designed before code.** The ARM64 asm.js/WASM calling
convention (argument registers/stack layout for unboxed
int32/int64/float32/float64/SIMD, return boxing to `Var`, callee-save
set, stack alignment, the interpreter↔JIT shared `AsmJsCallStackLayout`
contract) is designed in W1 as a short written spec before W3/W4
implement against it. AAPCS64 is the starting point; deviations must
be justified in that spec. This is the decision everything else
compounds on — no sub-agent improvises it locally.

D5. **Linux x64 is the behavioral oracle.** Every ambiguous semantics
question (NaN bits, trap messages, error text in baselines) is
answered by running the Linux x64 build or reading its baselines,
not by re-deriving from the spec. Where Linux x64 cannot run in this
environment, baselines in-tree are the oracle and the gap is named.

D6. **Shared-code edits must be arch-neutral or gated.** Linux x64
WASM already works (B4 proof). Any edit under `lib/WasmReader`,
`lib/Runtime/Library/WebAssembly*`, or shared Backend files must
either be behavior-identical for x64 or sit behind the D2 gates, and
say which in the change description.

D7. **Feature/test selection by checked-in manifest, never by
hand-editing generated files.** `test/WasmSpec/rlexe.xml` is auto-
generated by `test/WasmSpec/convert-test-suite/index.js`
(`updateTests.cmd`); its 231 entries mix MVP, sign-extension,
non-trapping, and threads (the very first entry is a threads test)
with no MVP tag. "WasmSpec MVP" therefore needs an explicit
selection mechanism:

- A small checked-in generator script (W0) classifies every entry
  in `WasmSpec`, `WasmSpec.MultiValue`, `wasm`, `wasm.simd`,
  `AsmJs`, `AsmJsFloat`, `ASMJSParser` (suite paths resolved via
  `git ls-files` — git tracks `ASMJSParser`/`AsmJSFloat` while
  `test/rlexedirs.xml` spells `AsmJsParser`/`AsmJsFloat`; never
  trust the rlexedirs spelling on a case-sensitive filesystem).
  Classification rules, all mechanical:
  - Split `compile-flags` on whitespace into **tokens** (never
    substring match); compare tokens **case-insensitively** (the
    tree uses `-wasmthreads`, `-wasmfastarray-`, `-wasmsimd`
    alongside canonical case); a trailing `-` on a flag token is
    its **disabled** form (`-WasmFastArray-` ≠ `-WasmFastArray`).
  - Token → feature map: `wasmthreads`/`essharedarraybuffer` →
    threads; `wasmsignextends` → sign-ext; `wasmnontrapping` →
    nontrapping; `wasmmultivalue` → multi-value; `wasmsimd` →
    simd; `wasmfastarray` **enabled form only** → fast-array;
    `--no-wabt` present → wabt-free, its sibling default regress
    entry → wabt. Featureless (recorded, no feature):
    `-wasmfastarray-` (disabled = generic-path control),
    `-wasmsharedarrayvirtualbuffer-` (disabled),
    `-wasmmathexfilter` (Windows/x86-only code sites, inert here —
    pinned featureless; do not add to any exclusion list),
    `-wasmi64` (legacy i64 enable; i64 is MVP), `-nonative`
    (variant dimension, not a feature).
  - Suite/path rules: `wasm.simd` → simd; `WasmSpec.MultiValue` →
    multi-value; `features/{extends,nontrapping,threads}` and
    `chakra_atomic_*` → sign-ext/nontrapping/threads respectively.
  - Per-file overrides live in the generator with a reason string,
    initially exactly two: `test/wasm/signextend.js` → sign-ext
    (flagless in rlexe but tests sign-extension ops), and the 8
    `test/wasm` entries with no `-wasm` token (`badfuncformat`,
    `inlining`, `polyinline`, `loop*`) → non-wasm (JS/JIT tests
    housed in this suite; not wasm-feature evidence).
  Output `test/wasm-arm64-manifest.json`:
  `{test-id, suite, features[], runnerTags[], phase}` — `phase` is
  a separate axis from features (e.g. `AsmJsFloat` is featureless
  but phase P2). Counts in this plan are rlexe **entries**
  (including `-nonative` variant entries), never files. The
  manifest is regenerable; regeneration diffs are reviewed like
  code. Generated `rlexe.xml` files are never hand-edited.
- Phase sets are defined over the manifest. **P1 set** = entries
  whose features ⊆ {mvp, sign-ext, nontrapping}, excluding anything
  tagged threads/simd/multi-value/wabt/fast-array, **and** subject
  to runner applicability, decided here so D8's arch flip cannot
  silently import entries: `require_debugger` entries (4 in
  `test/wasm`, one also `exclude_xplat`) are deferred out of P1/P2
  until the debugger harness is validated on this host as its own
  slice; `exclude_x64` entries (5 OOM entries in `test/wasm`) are
  excluded from P1 and individually assessed at P2 (OOM behavior is
  arch-dependent; each gets an include/exclude decision with reason
  in the manifest). W0 records the exact resulting P1 entry count
  in this plan — recorded 2026-11-11 from the generated manifest:
  **388 P1 entries of 473** (WasmSpec 219, wasm 46, AsmJs 121,
  ASMJSParser 2; remainder: P3 29, P2-pending 28, P2 14, excluded 9,
  deferred 4). Candidate arithmetic from the 2026-11-11 swarm
  (WasmSpec 214 incl. mathEx-featureless pin, before fast-array-
  disabled inclusion) is superseded by the generator's output.
  Later phases widen the set by feature, and each phase gate names
  its superset.
- `test/wasm` specifics: select the `--no-wabt` regress variant
  (`test/wasm/rlexe.xml:40-50`) and exclude the default WABT variant
  until/unless Mac builds WABT (§9.4); fast-array policy per the
  token map above — only the enabled-form entry
  (`test/wasm/rlexe.xml:68`, plus any enabled-form elsewhere) is
  excluded permanently on this platform
  (`ENABLE_FAST_ARRAYBUFFER` is Windows-only — applicability, not a
  porting gap); disabled-form entries **stay** as generic-path
  controls.
  (Revised: the Mac `-Dwasm` build now compiles the vendored WABT
  (§9.4), so no `--no-wabt` selection is needed for the harness to
  run; the manifest still records the sibling classification for
  phase bookkeeping.)

D8. **Capability-based runner gating (W0 scope).** `runtests.py`
forces `arch='x64'` off-Windows (`:112-115`), which makes the
arm64 `require_asmjs` exclusion (`:188-189`) dead on Mac and leaves
the in-tree `arm64` baseline-override condition
(`test/typedarray/rlexe.xml:154-160`) unreachable. W0 fixes the
runner, narrowly:
1. Honor `--arm64` (or probe the binary) *before* the xplat
   force-to-x64, so `arch='arm64'` is real on this host; Windows
   and Linux-x64 behavior unchanged.
2. Pass `arch` itself (not only the `amd64` alias) to
   `test_override` (`:676-694`) so `<target>arm64</target>` fires.
   Baseline naming follows the existing shape: a new baseline file
   (e.g. `<name>.arm64.baseline`) selected by an arm64 condition
   override, exactly as `nan.x64.baseline` is selected by the amd64
   one. This is the *only* per-arch baseline mechanism; do not
   invent suffix magic in the runner.
3. Add a capability probe: before running `require_wasm` /
   `require_asmjs` suites, run the binary on a one-line probe
   script (`typeof WebAssembly`, plus a minimal `"use asm"` link
   check) and exclude with a printed reason if unsupported. The
   probe replaces arch-name guessing as phases land; tags stay as
   the coarse filter.
Gate-count discipline (§5) applies to every runner change: the
`disable_jit` selection count must not move unless a change
deliberately re-pins it everywhere.

## 3. Phases and gates

| Phase | Content | Exit gate (all must hold) |
|---|---|---|
| P0 Audit + build | W0, W1 | D2 gate matrix implemented (Mac build compiles/links WasmReader + WebAssembly API + arch-neutral asm.js behind a build option defaulting **off**); D8 runner fixes landed (real `arm64` arch, arm64 baseline overrides fire, capability probe); D7 manifest generated with the exact P1 entry count recorded in this plan; ABI spec written; prerequisite-lane commit hash recorded in the header |
| P1 Interpreter MVP | W2, W3, W6 (interpreter scope) | `WebAssembly.*` and asm.js work with JIT execution disabled (`-nonative`, §6 command); the **D7 P1 manifest set** (features ⊆ {mvp, sign-ext, nontrapping}) passes; asm.js modules validate and run interpreted |
| P2 JIT MVP | W4, W5, W6 (JIT scope) | Same suites pass with full JIT on the `jit` profile; zlib gate per success criterion 3 (asm.js trace asserted + median wall ≤ 20 s over 5 runs); bench exclusion removed + receipt re-recorded; GC-stress evidence for JIT frames |
| P3 Completion (committed scope, each gated separately) | W7 multi-value hardening, W8 SIMD/NEON, W9 threads/atomics | Per-workstream gates below; W9 additionally requires the SAB enablement decision (§9.2 — flag-gated vs default-on, not whether to build it) |

Phases are sequential gates, not calendar commitments. Rough sizing
for one experienced engine engineer, to be revised after P0's
inventory: P0 weeks; P1 2–4 months; P2 3–6 months; P3 2–4 months.
Sub-agent execution does not change the gate order; parallelize only
within a phase where dependencies allow (§4).

## 4. Workstreams

Each workstream is sized for one sub-agent (or one engineer) per
numbered task group. "DoD" = definition of done for that workstream.
Every workstream that touches allocation, GC, stack walking, or host
callbacks must first re-read `doc/memory-management.md` and include
an ownership record (allocation domain, owner/transfer point,
invalidation events, permitted thread/context) in its report, per
AGENTS.md. Every workstream ends with: focused tests added or
enabled, gates in §6 run, and a report in the §10 format.

### W0 — Build plumbing and x64-only inventory (P0)

Objective: a Mac build that *can* compile the WASM stack for ARM64,
plus the definitive list of what is missing.

Tasks:
1. Transplant the Linux inventory into the Mac table: WasmReader
   group (11 units + include dirs,
   `tools/build/tables_linux.zig:545-563`) and **seven** Library
   units (`tables_linux.zig:886-892`): `WasmLibrary.cpp` plus the
   six `WebAssembly*.cpp`. `WasmLibrary.cpp` is load-bearing, not
   optional: enabling `ENABLE_WASM` compiles in callers of
   `WasmLibrary::ResetFunctionBodyDefaultEntryPoint`
   (`lib/Runtime/Base/ScriptContext.cpp:4107`,
   `lib/WasmReader/WasmByteCodeGenerator.cpp:434`); omitting it
   fails this workstream's link DoD. `WabtInterface.cpp` (:893)
   stays out with WABT. Do **not** transplant the WABT group
   (§9.4: skip; `--no-wabt` test variants are selected instead).
   Note the Mac table already compiles the 8 `AsmJs*.cpp` (empty
   under today's guards) — do not re-add them. Generator edges in
   `tools/build/linux.zig:162-190,266` have Mac analogues to check
   in `build.zig`; `tables_linux.zig` is itself generated
   (`gen_tables_linux.py`) — transplant from the CMake inventory it
   transcribes, not by hand-editing generated output expectations.
2. Implement the D2 gate matrix: build option (e.g. `-Dwasm=true`,
   default off on Mac until the P1 gate) driving `ENABLE_WASM` +
   the new `ENABLE_ASMJS` in CommonDefines; audit and re-gate every
   `ASMJS_PLAT` use in the 8 `AsmJs*.cpp` + headers/call sites per
   D2's mechanics. `ENABLE_WASM_SIMD`/`ENABLE_WASM_THREADS` remain
   undefined on ARM64. Add the new source groups to the Mac table
   **only when the option is on**, so the off build's object graph
   (including numbered-object link order) is untouched.
3. Implement the D8 runner fixes (real arm64 arch, arm64 baseline
   overrides, capability probe) and the D7 manifest generator +
   first generated `test/wasm-arm64-manifest.json`; record the
   exact P1 set count in §3/§5 of this plan.
4. Force-compile with the gates on and enumerate every failure and
   every stub: for each `Assert(UNREACHED)`, record which
   opcode/feature reaches it **at runtime** and classify per entry
   (port-in-P1 / port-in-P2 / gate-off-until-W8-or-W9 /
   must-fail-controlled). Same for `TARGET_64`/`_M_X64`/`_M_IX86`
   branches in `lib/WasmReader`, `lib/Runtime/Language/AsmJs*`,
   `WAsmjsUtils.*`, `lib/Runtime/Library/WebAssembly*`,
   `lib/Backend` (shared + arm64). Checklist with file:line.

DoD: **precondition** — the prerequisite lane (header) is committed
and its hash is recorded in this plan's header; W0 does not start
against an uncommitted base. Then:
`mise exec -- zig build -Dprofile=jit` with the WASM option on
compiles and links (runtime use of `WebAssembly` may still fail-fast);
option off = behavior- and gate-count-identical to today
(`zig build check` green at unchanged counts; off object graph
unchanged by construction); D7 manifest + D8 runner changes landed
with their own focused checks (probe excludes cleanly on today's
binary); inventory checklist committed;
`tools/build/test_build_tools.py` updated for any table-shape change
and passing.

Depends on: nothing. Blocks: everything.

### W1 — ARM64 asm.js/WASM ABI spec (P0, design only)

Objective: the written contract W3/W4 implement. No code.

Tasks:
1. Read the x64 contract end-to-end: `AsmJsCallStackLayout`
   (`JavascriptStackWalker.h:37-41`), `AsmJsTypes.*`, `AsmJsUtils.*`,
   x64 thunk/entry code paths (`InterpreterStackFrame.cpp` x64
   branches, `lib/Backend/amd64/Thunks.S`), external (JS→wasm) and
   internal (wasm→wasm, wasm→JS) call shapes, return-value boxing
   per WasmType (incl. i64 → BigInt-or-TypeError boundary behavior as
   the tree implements it today — record what x64 does, do not
   redesign it here).
2. Write `doc/wasm-arm64-abi.md`: register assignment (AAPCS64
   base), stack layout diagram for `AsmJsCallStackLayout` on ARM64,
   where unboxed args live at each boundary
   (JS→interpreter, JS→JIT, JIT→JIT, JIT→interpreter, either→runtime
   helper), float32 handling, multi-value returns (stack area
   ownership), trap signaling (how a bounds/div trap becomes a JS
   `RuntimeError` — exception vs. check-and-branch), and stack-walk
   frame recognition markers. Boundary rule to state explicitly:
   AAPCS64-derived conventions apply to *internal* boundaries
   (wasm→wasm, wasm↔interpreter thunks); the *external* JS→wasm
   entry speaks the engine's existing `Var`/`CallInfo` ABI
   (`JavascriptCallStackLayout`), with unboxing/boxing inside the
   thunk — do not force AAPCS64 at the JS boundary.
3. List every place the spec forces a change outside ARM64 files;
   each must satisfy D6.

DoD: ABI doc reviewed against the x64 code by a second reader (or a
second sub-agent with the x64 sources); open questions in the doc are
either resolved or converted to explicit P1/P2 decision points.
W3/W4 may not start coding against an unreviewed draft.

Depends on: W0 inventory (for the boundary list). Blocks: W3, W4.

### W2 — Frontend and JS-API bring-up, JIT off (P1)

Objective: module parsing, validation, instantiation, and the
`WebAssembly` JS surface work on ARM64 with all code execution still
failing fast in a controlled way.

Tasks:
1. Compile-fix `lib/WasmReader` on ARM64 (expected small: asserts,
   `TARGET_64` assumptions). No behavior change for x64 (D6).
2. Wire `WebAssemblyModule/Instance/Memory/Table/Environment`
   construction, imports/exports linking, memory creation/grow
   (respecting 16 KB host pages vs 64 KB wasm pages — verify, do not
   assume), table set/get/grow, globals.
3. Guardrail: any attempt to *call* an exported function before W3
   lands throws a controlled `RuntimeError`/TypeError path — never
   reaches an `UNREACHED` stub or null entry point. Find the entry-
   point assignment sites and gate them.

DoD: with `-nonative`, a JS probe can `validate`, `compile`,
`instantiate` a module with memory/table/globals/exports and read
back memory bytes, tables, and globals; calling an export fails
controlled (probe asserts the error). Probe checked in as a test
(enabled per §5 mechanics).

Depends on: W0. Blocks: W3 testing.

### W3 — ARM64 interpreter execution (P1) — highest-risk workstream

Objective: wasm/asm.js bytecode actually executes on ARM64 through
the existing `OpCodeAsmJs` interpreter.

Tasks:
1. Implement the ARM64 entry/exit thunks replacing the
   `!defined(_M_AMD64)` fail-fast (`InterpreterStackFrame.cpp:
   1760-1765`) and the argument unboxing / return boxing around
   `AsmJsInterpreter`, per the W1 spec. Cover int32, int64, float32,
   float64 args and returns; SIMD-typed signatures fail controlled
   until W8.
2. Implement the asm.js module link path on ARM64: `AsmJsModule`,
   `AsmJsLink`, array-view creation with D3 generic buffers
   (`AsmJsModule.cpp:1841-1849` — remove the ARM64 `UNREACHED`,
   keep x64/x86 branches untouched), foreign/FFI and stdlib binding.
3. Traps: out-of-bounds, divide-by-zero, invalid conversion, and
   unreachable must surface as the same JS errors x64 produces
   (baseline text is the oracle, D5).
4. Stack checks / recursion limits behave as in normal interpreter
   frames (no unbounded native recursion through wasm→JS→wasm).

DoD: P1 exit gate — the D7 P1 manifest set across `WasmSpec`,
`test/wasm` (incl. the `--no-wabt` regress variant), `test/AsmJs`,
and `test/ASMJSParser` passes under the §6 no-JIT command on
darwin/arm64; the W2 probe's export call now returns real values;
`Octane/zlib.js` completes interpreted in a single run under an
explicit 1800 s timeout (`timeout 1800 out/jit/bin/ch -nonative
Octane/zlib.js` from `test/benchmarks/`), printing `### SCORE:`
(record the wall time; it will be slow — that is expected, not a
failure; this probe is standalone evidence, not part of the
60 s-timeout suite run).
Sign-extension and non-trapping entries are in the P1 set by
construction (both flags default on); if either feature cannot pass
at P1, that is a P1 blocker to surface, not a silent manifest edit.

Depends on: W1, W2. Blocks: P2 differential testing.

Risk notes: this workstream plus W6 is where GC/stack-walk defects
hide. If a frame-shape question is not answered by the W1 spec, stop
and amend the spec — do not improvise in thunk code.

### W4 — ARM64 JIT lowering for OpCodeAsmJs (P2) — largest workstream

Objective: replace all 17 ARM64 `UNREACHED` stubs with correct
lowering, modeled on the x64 implementations and the W1 ABI.

Reference map (x64 → ARM64 home):
`lib/Backend/amd64/LowererMDArch.cpp` (`LowerAsmJsCallE` 1083,
`LowerAsmJsCallI` 1094, `LowerAsmJsLdElemHelper` 1189,
`LowerAsmJsStElemHelper` 1290, plus bounds-check/atomic/convert
helpers) → `lib/Backend/arm64/LowerMD.cpp` + declarations in
`lib/Backend/arm64/LowerMD.h:104-202`.

Tasks, in dependency order (each is a sub-agent-sized slice; land and
test incrementally behind the W0 build option):
1. Calls/returns: `LowerAsmJsCallI`, `LowerAsmJsCallE`,
   `LowerExitInstrAsmJs`, `LoadFloatHelperArgument`,
   `HelperCallForAsmMathBuiltin` — internal, external (imported JS),
   and helper calls with correct unboxed arg/return movement.
2. Memory ops: `LowerAsmJsLdElemHelper`, `LowerAsmJsStElemHelper`,
   `LowerWasmArrayBoundsCheck` — explicit bounds checks against the
   generic buffer (D3); trap path per W1 (no signal-handler tricks
   in this phase).
3. Scalar completes: `GenerateCtz` (ARM64 has no ctz — `rbit`+`clz`),
   `GeneratePopCnt` (NEON `cnt` or sequence — pick per available
   encoder support, record choice), `GenerateTruncWithCheck`,
   `EmitFloat32ToFloat64`, `EmitInt64toFloat`, `EmitSignExtend`,
   `EmitReinterpretPrimitive` — exact NaN/canonicalization and
   saturation semantics per spec tests, i64 as single 64-bit regs
   (no split-int64 on ARM64; `LOWER_SPLIT_INT64` is 0 here).
4. Encoder/legalizer gaps discovered along the way (float32 regs,
   64-bit immediates) — fix in `arm64/EncoderMD.*`/`LegalizeMD.*`
   with the same care as the write-barrier work: TyInt8/TyMachReg
   temp aliasing bit this backend before (see
   `LowerMD.cpp` GenerateWriteBarrier comment); prefer full-width
   temps for synthesized stores.

DoD: zero asm.js/WASM `UNREACHED` stubs remain in
`lib/Backend/arm64/LowerMD.h` for the P1+P2 feature surface
(atomics may stay stubbed until W9 *only if* unreachable with the
threads gate off — proven by a checked-in negative test: a module
using wasm atomics is rejected at compile/validate time with
`-WasmThreads-`, in interpreter and JIT tiers; W9 flips that test's
expectation); the D7 P1 set plus `test/AsmJsFloat` pass on the
`jit` profile in default and `dynapogo` variants; differential
interpreter-vs-JIT run over the manifest set shows no divergence.

Depends on: W1, W3 (oracle), W5 for full call coverage.

### W5 — JIT runtime/ABI integration (P2)

Objective: JIT-compiled wasm/asm.js bodies are entered, tiered, and
exited correctly from JS and from each other.

Tasks:
1. External entry point: JS→JIT-wasm thunking (arg unboxing from
   `Var[]`, `this`/function-object handling, return boxing) and
   JIT→JS import calls, incl. reentrancy (import calls back into
   wasm; GC/allocation inside imports while unboxed values are live
   — rooting per W6).
2. Tier behavior: wasm bodies' place in the
   interpreter→simpleJIT→fullJIT pipeline (or direct-to-full, as x64
   does — mirror x64, record any deviation), OSR/bailout behavior if
   the x64 tier has it, and code-memory lifecycle (W^X/MAP_JIT are
   already solved for the JS JIT; reuse, verify page-size handling).
3. `AsmJsCodeGenerator`/`AsmJsEncoder` ARM64 paths used by asm.js
   (distinct from binary-wasm bytecode gen) compile and run.

DoD: `test/AsmJs` (144 entries) and `test/AsmJsFloat` pass on the
`jit` profile; a wasm→JS→wasm reentrancy probe under
`-CollectGarbage` (or W6's stress harness) survives; the zlib gate
of success criterion 3 passes (asm.js trace asserted + median wall
≤ 20 s over 5 bench runs).

Entry-convention invariant (tranche 7 finding, must hold for every
slice): the Var-convention `AsmJsExternalEntryPoint` serves JS
dispatch only (DynamicType entry installed at link). Every address
reachable by slot-image callers (`entryPointInfo->jsMethod`,
original entry, `AsmJsGetEntryPoint`, `CheckAsmJsCodeGen` idle
return, Expire/debugger fallbacks) must be slot-image-compatible
(JIT code, codegen thunk, or the slot-image delay thunk). Installing
the Var entry there segfaults deterministically on the first
JIT→uncompiled-callee call (tranche-7 merge validation).

Depends on: W4 slices 1–2 minimum.

### W6 — GC, stack walking, and memory safety (P1 interpreter scope; P2 JIT scope)

Objective: the collector and stack walker treat asm.js/WASM frames
exactly right — unboxed slots never traced as pointers, real roots
never missed. Mandatory reading: `doc/memory-management.md` in
full before starting; this workstream's report carries the
ownership record for every new frame/root shape.

Tasks:
1. Frame recognition: teach `JavascriptStackWalker` and the GC's
   stack scan to identify ARM64 interpreter asm frames (P1) and JIT
   asm frames (P2) per the W1 markers; verify against how x64 frames
   are recognized today (`GetAsmJsFunctionInfo`,
   `FunctionBody::GetIsAsmJsFunction`).
2. Rooting audit: every boundary where unboxed values are live
   across an allocation or callback (imports, FFI, memory grow,
   `WebAssembly` API calls during instantiation) — enumerate, root
   or prove no-GC, record in the ownership record.
3. Write barriers: JIT wasm stores to JS-visible objects (globals of
   ref type if any, table sets, asm.js view stores into JS arrays
   where applicable) must go through the ARM64 JIT write-barrier
   path from the prerequisite lane (header) — verify its commit is
   in HEAD before relying on it; reuse `GenerateWriteBarrier*`, do
   not hand-roll card stores.
4. Stress evidence (primary: flag-driven, in-tree): run manifest
   subsets under `-CollectGarbage` and the tree's GC-stress flags,
   plus a targeted churn probe (instantiate/call/grow in a loop
   under concurrent GC) in the style of
   `test/es7/jit-write-barrier.js`. `bin/GCStress` soaks are
   supporting evidence only; they are usable at the base commit
   (startup/concurrent fixes are in `61b6af3f6`) but loop forever by
   design, so bound them externally and never gate on them alone.

DoD: stress probes pass repeatedly (≥10 runs, timing-sensitive bugs
hide in single passes); no new `Recycler`-visible raw-pointer shape
without an ownership record; a deliberate "unboxed int that looks
like a heap pointer" probe is checked in and asserts two concrete
outcomes across a forced full collection: (a) the process exits 0
(no crash from tracing the fake pointer), and (b) a `WeakRef` to an
object whose only "reference" is that unboxed int value reads
`undefined` after collection (the lookalike did not retain it).

Depends on: W3 (P1 scope), W4/W5 (P2 scope). Gate for both phases —
P1/P2 do not close on test-green alone without this evidence.

### W7 — Multi-value hardening (P3, small)

Objective: `test/WasmSpec.MultiValue` (2 rlexe entries) and
multi-return signatures work in both tiers.

Tasks: implement/verify the W1-specified multi-value return area in
thunks (W3) and JIT calls (W4 slice 1 follow-up); block-type
signatures in validation if the reader gates them.

DoD: WasmSpec.MultiValue passes interpreted and JIT; a multi-return
export called from JS returns correct values (probe).

Depends on: P2.

### W8 — SIMD → NEON (P3, large, separable)

Objective: `ENABLE_WASM_SIMD` on ARM64; `test/wasm.simd` (12
entries) passes. Note the suite is also tagged `exclude_xplat` and
`require_simd` handling in `runtests.py` excludes it off-Windows
today — enabling on darwin is part of this workstream.

Tasks:
1. Inventory SSE-shaped assumptions: `SIMDValue` layout,
   `WasmBinaryOpcodesSimd.h`, x64 encoder/lowering for each SIMD
   opcode the reader accepts; classify NEON-direct vs sequence vs
   helper.
2. Interpreter handlers first (correctness oracle), then JIT
   lowering + encoder support for the NEON forms needed.
3. Lane/NaN semantics differential-tested against interpreter.

DoD: `test/wasm.simd` passes in both tiers on darwin/arm64;
`exclude_xplat`/`require_simd` gating updated narrowly (darwin/arm64
only); zero SIMD `UNREACHED` reachable with the gate on.

Depends on: P2. Independent of W7/W9.

### W9 — Threads/atomics (P3, gated on product decision)

Objective: `ENABLE_WASM_THREADS` on ARM64: shared memories, wasm
atomics, `wait`/`notify`.

Precondition: none for building it (committed scope, D1); the
enablement posture (§9.2 — flag-gated vs default-on) must be decided
before W9 *ships*, and W9 work proceeds behind the existing flags
until then. `ESSharedArrayBuffer` currently defaults false
(`ConfigFlagsList.h:712`); the 14 flagged test entries (WasmSpec 12
+ wasm 2) are the acceptance set.

Tasks:
1. `LowerAtomicLoad`/`LowerAtomicStore` + atomic RMW lowering with
   ARM64 memory ordering (LDAR/STLR / LSE atomics — choose per
   target CPU floor the build already assumes; record it).
2. Shared `WebAssembly.Memory` growth + SAB interop; wait/notify
   through the existing Atomics infrastructure
   (`lib/Runtime/Library/AtomicsObject.cpp` — recent swarm-review
   fixes touched this file; re-read before editing).
3. Thread/context ownership records for cross-agent memory handles
   (memory-management.md §host boundaries).

DoD: the 14 `-ESSharedArrayBuffer` entries pass in both tiers;
concurrent-GC + shared-memory stress probe survives (W6 extension);
no behavior change when the flag is off.

Status 2026-11-13 (landed, flags default off; §9.2 default-on decision
still open): gate widened (`CommonDefines.h`: `ENABLE_WASM_THREADS`
on `ASMJS_PLAT || (_M_ARM64 && WASM_ARM64_PORT)`; all newly compiled
regions verified arch-neutral, no `#else` flips). ARM64 JIT lowering
is LDAR/STLR (`LowerMD.{h,cpp}`, new `MdOpCodes` LDAR/STLR +
`EncoderMD` wiring; encodings already existed in `ARM64Encoder.h`).
Load/store-only matches this vintage (`OpCodesAsmJs.h`: no atomic
RMW opcodes; `WasmBinaryOpCodes.h` 0x10-0x1d loads/stores, RMW
hard-disabled; no wasm wait/notify — JS `Atomics.wait/notify`
unchanged). Narrow-signed-load SBFX fixup is included but unreachable
(no signed-narrow atomic load opcodes exist); it mirrors
`LegalizeLoad`'s LDRS split. Evidence: 12 WasmSpec threads +
`atomics_test.js` + `reload.js` pass interp + FullJIT (incl.
`-MaxAsmJsInterpreterRunCount:0` forced-JIT); `atomics_gc_stress.js`
(new, 200 rounds shared-memory + forced GC + graph checks) passes
both tiers; `atomics_threads_off.js` negatives pass both tiers; P1
381/381 interp; P1 --jit 250/381 (4 pre-existing stack-exhaustion
failures, unchanged); `wasm.simd` 12/12 x2 + 8/8 oracles; `zig
build check` rc=0. Ownership: no new retained state — IR arena
discipline unchanged, shared-memory lifecycle unchanged (existing
owners), no new roots/handles.

Depends on: P2, §9 decision. Highest concurrency risk; do not
parallelize with W6 changes to the same files.

## 5. Test suites and enabling mechanics

Suite inventory (rlexe `<test>` entries, verified):

| Suite | Entries | Dir tags (`test/rlexedirs.xml`) | Phase |
|---|---|---|---|
| `test/WasmSpec` | 231 | `require_backend, require_wasm` | P1 per D7 manifest set (mvp+sign-ext+nontrapping; threads entries wait for W9) → P2 full minus SIMD/threads → W9 completes |
| `test/WasmSpec.MultiValue` | 2 | `require_backend, require_wasm` | P3/W7 (may pass earlier; claim only at W7) |
| `test/wasm` | 67 | `require_backend, require_wasm` | P1 → P2 |
| `test/wasm.simd` | 12 | `require_wasm, require_backend, exclude_xplat` | P3/W8 |
| `test/AsmJs` | 144 | `require_asmjs` (per-suite) | P1 interp → P2 JIT |
| `test/AsmJsFloat` (`AsmJSFloat` dir casing varies) | 14 | `require_asmjs` | P2 |
| `test/ASMJSParser` | 2 | `require_backend, require_asmjs` | P1 |

Enabling mechanics a sub-agent must handle explicitly:

- **Selection is D7's manifest, full stop.** `test/WasmSpec/
  rlexe.xml` is auto-generated (`convert-test-suite/index.js`);
  hand-edits and hand-added tags there will be overwritten. All
  phase scoping, exclusions, and applicability rules (threads,
  SIMD, multi-value, WABT, fast-array) live in
  `test/wasm-arm64-manifest.json` + its generator. Non-generated
  suites (`test/wasm`, `test/AsmJs`, …) may use rlexe tags, but the
  manifest remains the single phase-set definition so counts are
  comparable across suites.
- **Runner arch logic (D8) lands in W0, before any suite is
  enabled.** Today `runtests.py:112-115` forces `arch='x64'`
  off-Windows: the `:188-189` arm64 `require_asmjs` exclusion is
  dead, `exclude_arm64` never applies, and suites run-and-fail on
  Mac for missing engine support. After D8, `--arm64` runs get real
  arm64 tagging plus the capability probe; keep 32-bit `arm`
  excluded.
- **`disable_jit` cannot be P1's vehicle.** `require_backend`
  excludes these suites from `disable_jit` (`runtests.py:744-748`),
  and `--variants=interpreted` does **not** disable JIT — it passes
  `-maxInterpretCount:1 -maxSimpleJitRunCount:1 -bgjit-`
  (`:732-736`), which still JITs after one iteration. P1 evidence
  uses `interpreted` **plus** `--extra-flags=-nonative` (§6). Do not
  "fix" this by removing `require_backend` tags without handling
  the gate-count consequences below in the same change.
- Gate counts: `zig build check` runs the `disable_jit` selection
  pinned in `tools/build/gate_counts.json` (test 2053/0, debug
  2105/0 at plan time) with consumers in `check_regression.py`,
  `build.zig`, CI, and `test_build_tools.py`. Tests tagged
  `require_backend` do not move these counts. Any newly counted test
  (or D8 runner change that moves a count) requires bumping the
  JSON lineage *and* every consumer, in the same change, per that
  file's own instructions.
- Baselines: most wasm/asm.js baselines are x64-generated text.
  The per-arch mechanism is D8.2 and only that: an arm64
  `<condition><target>arm64</target><override><baseline>` entry
  (template: `test/typedarray/rlexe.xml:149-160`) pointing at a new
  `<name>.arm64.baseline` file. For generated WasmSpec entries,
  baseline divergence is handled in the manifest (exclude +
  follow-up) or by teaching the generator — never by editing the
  generated XML or regenerating an x64 baseline from an ARM64 run.
- WABT: `test/wasm/rlexe.xml:40-50` has a default (WABT) regress
  entry and a `--no-wabt` variant. The manifest still selects
  `--no-wabt` and phase-excludes the WABT entry (it remains P3 until
  a manifest regeneration promotes it), but the Mac `-Dwasm` build
  now compiles the vendored WABT (revised §9.4), so both variants
  run — the exclusion is bookkeeping, not a build gap. The whole
  WasmSpec harness depends on this converter (`WebAssembly.wabt.
  convertWast2Wasm` in `test/WasmSpec/spec.js`), which is why the
  P1 gate cannot be met without it (§9.4).
- `-WasmFastArray` (2 entries in `test/wasm`): permanently
  inapplicable here (`ENABLE_FAST_ARRAYBUFFER` is Windows-only,
  §1). Manifest excludes with that reason; this is an applicability
  rule, not a TODO.

## 6. Validation plan (commands)

Replace `<wasm-opt>` with W0's actual option. Two invocation rules
that bite if ignored: runtests.py is run **from `test/`** and
`-binary` must be an **absolute** path (it changes cwd per test;
same requirement as documented in `tools/build/check_regression.py`).

Builds (every workstream, before claiming done), from repo root:

```
mise exec -- zig build -Dprofile=jit <wasm-opt>
mise exec -- zig build             # test profile, option as decided
mise exec -- zig build -Dprofile=debug-static
mise exec -- zig build check       # gate counts must match §5 handling
```

Suites (record exact variant + counts in report). All runs are
filtered to the phase's D7 manifest set; unfiltered suite runs are
expected to fail on not-yet-implemented features and prove nothing:

```
cd test
# P1 evidence (JIT execution truly off: interpreted variant alone
# still JITs after 1 iteration — -nonative is what disables it):
python3 runtests.py -binary "$PWD/../out/jit/bin/ch" --test --arm64 \
    --variants=interpreted --extra-flags=-nonative \
    WasmSpec wasm AsmJs ASMJSParser
# P2 evidence (JIT):
python3 runtests.py -binary "$PWD/../out/jit/bin/ch" --test --arm64 \
    WasmSpec wasm AsmJs AsmJsFloat
python3 runtests.py -binary "$PWD/../out/jit/bin/ch" --test --arm64 \
    --variants=dynapogo WasmSpec wasm
# Differential: same manifest set, -nonative vs default; diff the
# tallies and any per-test output the harness exposes.
```

The checked-in P1 gate counter is `tools/build/check_wasm_p1.py`.
It runs the P1 command above (plus `-v --include-slow` so every P1 entry emits a joinable command-line block), joins each block to `test/wasm-arm64-manifest.json` by test-id via the `-dynamicprofilecache:profile.dpl.<id>` token with exact flags-suffix verification, and prints `P1 pass N/385` with failing P1 test-ids grouped by suite; it exits nonzero if any P1 entry failed or the join is incomplete (not yet wired into `zig build check`).

```
python3 tools/build/check_wasm_p1.py test/runtests.py out/jit/bin/ch
python3 tools/build/check_wasm_p1.py --selftest
```

Smoke probes (fast, run constantly during development), repo root:

```
out/jit/bin/ch -nonative <probe.js>   # interpreter path
out/jit/bin/ch <probe.js>             # JIT path (P2+)
cd test/benchmarks && ../../out/jit/bin/ch Octane/zlib.js
```

GC stress (W6, phase gates): `-CollectGarbage` and the tree's
GC-stress flags are the primary evidence, plus ≥10 repeats of the
churn probes; `bin/GCStress` soaks are supporting only (W6.4).
Single green runs are not evidence for timing-sensitive GC bugs:
the splay write-barrier defect crashed full JIT reproducibly at
first smoke ([perf-baseline.md](perf-baseline.md)), but after a
partial fix it passed individual runs for tens of iterations while
still broken intermittently — repeats are the evidence, in both
directions.

Non-regression: full `zig build check`; spot-run `es7` +
`es6module` under `disable_jit` as prior lanes did; Linux x64
behavior is argued via D6 classification in each change (native
Linux re-run is owed at phase gates if a host is available; name it
unverified if not — do not claim it).

## 7. Risks

| Risk | Where it bites | Mitigation in this plan |
|---|---|---|
| GC traces unboxed slots as pointers (crash/corruption, intermittent) | W3, W5, W6 | W1 frame markers; W6 as a phase *gate*, pointer-lookalike probe, ≥10× stress repeats |
| ABI designed implicitly, diverging between interpreter and JIT | W3 vs W4 | W1 written spec + second-reader review before either codes |
| Register/legalizer bugs in new ARM64 lowering (precedent: TyInt8 temp aliased a live base register in the write-barrier bring-up) | W4 | Incremental slices, differential testing vs interpreter per slice, full-width temps for synthesized ops |
| Trap semantics drift (wrong error type/text, missing canonical NaN) | W3, W4 | Linux x64 baselines as oracle (D5); spec suite is the gate, not samples |
| Shared-code edit breaks Linux x64 WASM | all | D6 classification per change; gate-count discipline |
| 16 KB host pages vs wasm/memory assumptions (precedent: card-table shift bug in the JS JIT barrier) | W2, W5 | Explicit verify-don't-assume tasks; grep for hardcoded 4K/64K constants in memory paths during W0 inventory |
| Threads memory-model bugs | W9 | Gated last, product decision first, no parallel W6 edits |
| Scope creep into modern WASM proposals | all | Non-goals are explicit; new proposals need their own plan |

Highest-risk validation step: W6's stress evidence at the P2 gate.
Test-suite green without it has already proven insufficient once in
this codebase (splay).

## 8. Performance expectations (measured, not gated)

- P1 (interpreter): expect 10–50× slower than JIT on compute-bound
  wasm/asm.js. `zlib.js` interpreted time is recorded for the
  record; the bench exclusion stays.
- P2 (JIT): remove the `darwin/arm64` zlib exclusion in
  `tools/bench/run.py`, re-record both baseline JSONs, update
  `doc/perf-baseline.md`. Compare same-host medians only (§"How to
  use this receipt" there). No absolute score claims.
- D3's generic-buffer choice costs bounds-check overhead vs x64's
  optimized buffer; quantify with the bench harness before anyone
  ports the optimized path.

## 9. Open decisions (need a human decision before the named gate)

1. **End-state ambition** — RESOLVED 2026-11-11: full surface
   (JIT + SIMD + threads) is the committed goal. See D1.
2. **SAB/threads enablement posture** (before W9 ships): threads
   will be built (decision 1); the open half is whether
   `SharedArrayBuffer`/wasm threads ship behind the existing
   `-ESSharedArrayBuffer` default-off flag or enabled by default.
   Security posture decision, not an engineering one. W9
   implementation may proceed flag-gated while this is open;
   flipping any default may not.
3. **Default-on timing** (at each phase gate): when does the W0
   build option flip to default-on for Mac profiles — recommended:
   P1 gate for interpreter availability in test profile, P2 gate for
   the `jit` profile — with `modernization-plan.md` updated in the
   same change (it currently lists this port as P2 future work).
4. **WABT on Mac** — REVISED 2026-11-11: build it. The original
   "skip, select `--no-wabt`" decision cannot meet the P1 gate: the
   entire WasmSpec harness (`test/WasmSpec/spec.js`) reaches the
   converter unconditionally (`WebAssembly.wabt.convertWast2Wasm`),
   and `--no-wabt` does not cover that use — 183 of 388 P1 entries
   fail at the harness with `convertWast2Wasm` undefined. The Mac
   `-Dwasm=true` build now compiles the vendored WABT
   (`lib/wabt`, `tools/build/tables.zig` `wabt_group` +
   `WabtInterface.cpp`, `config.h` from the `lib/wabt/src/config.h.in`
   substitution mirroring `lib/wabt/CMakeLists.txt:41-44`, and
   `ENABLE_WABT` via the D2 disjunct with `CAN_BUILD_WABT`). The
   vendored sources compile unmodified. The manifest is untouched:
   its `wabt`/`wabt-free` sibling classification remains for phase
   bookkeeping (the one `wabt`-feature entry stays P3 until a
   regeneration promotes it); W0's skip text above is historical.

## 10. Sub-agent handoff protocol

To execute a workstream, a sub-agent (or engineer) needs only this
file plus its workstream section. Rules:

1. Start by re-verifying your workstream's cited file:line anchors
   (they drift). If an anchor is wrong, note the correction in your
   report; do not silently work from stale anchors.
2. Read `doc/memory-management.md` before touching allocation, GC,
   stack walking, barriers, or callbacks (AGENTS.md requirement).
   Establish allocation domain/owner/invalidation/thread from source
   and callers, not from this plan's summaries.
3. Keep the change to your workstream's slice. Cross-workstream
   discoveries go in the report as proposed follow-ups, not into the
   diff.
4. Preserve existing ownership/release mechanisms until your slice's
   contract is proved (AGENTS.md). No drive-by refactors of x64
   paths (D6).
5. Add or enable the smallest focused test that fails without your
   change (probe-first where the workstream says "probe").
6. Report format: (a) what changed, file list; (b) DoD checklist
   with pass/fail per item; (c) exact commands run and their
   tallies; (d) ownership record if applicable; (e) D6
   classification of every shared-code edit; (f) named gaps and
   unverified claims (especially Linux x64); (g) proposed updates
   to this plan (anchor fixes, inventory additions).
7. Do not update gate counts, baselines for another arch, or this
   plan's phase status without the §5/§6 evidence in the same
   change.

Phase status tracker (update only with gate evidence):

| Phase | Status | Evidence |
|---|---|---|
| P0 | complete (2026-11-11) | D2 gates + `-Dwasm` option compile/link ON and OFF; `zig build check` 2053/0 unchanged; D7 manifest 388 P1/472; D8 runner probe validated (109/0 count discipline); ABI spec [wasm-arm64-abi.md](wasm-arm64-abi.md); inventory [wasm-arm64-inventory.md](wasm-arm64-inventory.md). Residual: TARGET_64/_M_* branch line-audit deferred to W2 (named in inventory) |
| P1 | complete (2026-11-11) | `tools/build/check_wasm_p1.py` PASS 378/378 (complete join; deferrals with evidence: 6 jsapi + 2 select pre-existing x64-parity, 4 require_debugger; limits segments P2-pending on wall-time with solo passes recorded). W3 interpreter + WABT harness + W6 probes 10/10 under `-CollectGarbage`; zlib interpreted 98s score 1565; `zig build check` 2053/0 |
| P2 | not started | — |
| P3/W7 | not started | — |
| P3/W8 | not started | — |
| P3/W9 | not started (enablement posture §9.2 open; build is committed) | — |
