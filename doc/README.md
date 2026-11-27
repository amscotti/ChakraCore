# ChakraCore Documentation

In-repo documentation for the ChakraCore modernization effort. Engine and legacy
build references are pinned to baseline `97e68c917`; execution results and
modernization decisions are updated through **2026-11-26** (`modernize/baseline`,
including `G0` Linux baseline sync, `Q1.1`-`Q1.5`, `I1`/`I2`, `Q2`, `Q3`, `P1`,
`C3.1`, `ARM64 WB`, `Pre-Q1.5`, Windows x64 `B6` `zig build`, and Waves 1-5b +
open-cards Sections 30-33 hardening/remediation waves). See
[vision-and-roadmap.md](vision-and-roadmap.md) for strategic positioning and
Phase-4 expansion tracks (`G0-phase4`, `Q1`, `I1`/`I2`, `P1`, `C3.1`, `Q3`, `H1`),
[current-state.md](current-state.md) Section 7 for historical activity context,
and [architecture-and-readiness-review.md](architecture-and-readiness-review.md) /
[linux-handoff.md](linux-handoff.md) / [windows-handoff.md](windows-handoff.md)
for platform readiness. The complete per-batch C2 receipt ledger (S1-S14, E1,
host slices) lives in [modernization-plan.md](modernization-plan.md) Section 6;
the table below indexes the foundational documents, readiness reviews, and
primary receipts.

| Document | Contents |
|---|---|
| [vision-and-roadmap.md](vision-and-roadmap.md) | **Embeddable JIT Sweet Spot** vision & Phase-4 expansion tracker (`G0`, `G0-phase4`, `Q1.1`-`Q1.5`, `I1`/`I2`, `Q2`, `Q3`, `P1`, `C3.1`, `ARM64 WB`, `Pre-Q1.5`, `H1`) |
| [b5-package.md](b5-package.md) | Release packaging (`zig build package` / `check-package`), deterministic tarballs, and SHA-256 `MANIFEST.json` verification |
| [zig-strategy.md](zig-strategy.md) | Incremental C-ABI Zig wedge strategy (`Z1` boundaries vs keeping C++ core intact) |
| [architecture-and-readiness-review.md](architecture-and-readiness-review.md) | System architecture, layout, P0/P1 Mac fixes, 4-profile test matrix, Test262 `refresh-2026-11-16`, residuals, and Linux readiness |
| [linux-handoff.md](linux-handoff.md) | Linux x64 bring-up and revalidation handoff: Phase-4 (`G0-phase4`) 10-step runbook (§0A), `G0` baseline runbook (§0), build overrides, sanitizers, Node-API, and CI checklist |
| [windows-handoff.md](windows-handoff.md) | **B6 Windows x64 `zig build` bring-up and certification handoff**: VS 2022 / WinSDK / `vcpkg` ICU4C prerequisites, 10-step runbook, troubleshooting, and `b6-windows-run.md` receipt template |
| [b4-linux-run.md](b4-linux-run.md) | Native Linux x64 behavior proofs and `G0` single-pass revalidation receipt (`8b447ac48..1bf42d1f6`) |
| [perf-baseline.md](perf-baseline.md) | Darwin ARM64 quiet-machine benchmark receipts (`baseline` vs `HEAD` JIT/Interpreter across SunSpider, Kraken, Octane, JetStream) |
| [node-api-receipt.md](node-api-receipt.md) / [node-api-kickoff.md](node-api-kickoff.md) / [node-api-status-mapping.md](node-api-status-mapping.md) | Node-API provider/host verification (`NAPI_VERSION=10` `161` exports in §14; `v8` `150` exports historical baseline in §§1-13), kickoff brief, and N0.6 status mapping |
| [z1-u8a-codec.md](z1-u8a-codec.md) | `Z1` Zig 0.17.0 SIMD `Uint8Array` Base64/Hex C-ABI codec boundary contract and ownership record (`Q1.3`) |
| [i1-modern-intl.md](i1-modern-intl.md) | Track `I1` modern ECMA-402 C++ wrappers over C ICU4C (`Locale`, `ListFormat`, `RelativeTimeFormat`, `DisplayNames`, `Segmenter`, `getCanonicalLocales`) |
| [i2-duration-enumeration.md](i2-duration-enumeration.md) | Track `I2` `Intl.supportedValuesOf` and `Intl.DurationFormat` C++ implementation over C ICU4C |
| [q14-es2025-sweep.md](q14-es2025-sweep.md) | Track `Q1.4` ES2025 Conformance Sweep II (`Date.prototype.set*`, `Function.prototype.toString`, `JSON.parse` `Walk`, `AsyncFromSyncIterator`) |
| [q2-es2026-wave2.md](q2-es2026-wave2.md) | Track `Q2` ES2026 Wave 2 (`Iterator.concat`, `Map`/`WeakMap` `getOrInsert*`, `JSON.rawJSON`/`isRawJSON`/`context.source`) |
| [p1-jit-bailouts.md](p1-jit-bailouts.md) | Track `P1` pathological JIT bailout, loop-liveness, ReJIT storm, and ARM64 asm.js BGJIT tier-up fixes (`1.453x` 64-test geomean) |
| [c31-explicit-resource-management.md](c31-explicit-resource-management.md) | Track `C3.1` Explicit Resource Management (`Symbol.{dispose,asyncDispose}`, `SuppressedError`, `DisposableStack`, `AsyncDisposableStack`, `using`/`await using` + JIT lowering, `947/0` Test262) |
| [arm64-write-barrier-jit.md](arm64-write-barrier-jit.md) | ARM64 JIT software write-barrier fix (`LowerMD.cpp` + `LinearScan::InsertMove` under `ConcurrentCollect`) |
| [pre-q15-cleanup.md](pre-q15-cleanup.md) | Pre-Q1.5 Hardening & Cleanup (`29/29` JIT+WASM ASan & UBSan under `halt_on_error=1`, `-ParserStateCache` `ImmutableList` UB, ARM64 `Math.{ceil,floor}` `-0` `44/0`, `98` build-tool tests) |
| [q15-es2025-sweep.md](q15-es2025-sweep.md) | Track `Q1.5` ES2025 Conformance Sweep III (`99.56%` ES2025 edition / `88.34%` broad, `+247` non-`waitAsync` fixes across `133` files, `0` JIT diffs) |
| [q3-es2026-wave3.md](q3-es2026-wave3.md) | Track `Q3` ES2026 Wave 3 (`Iterator.{zip,zipKeyed}`, `Iterator.prototype.{chunks,windows,includes,join}`, `Promise.{allKeyed,allSettledKeyed}`, `88.97%` broad / `99.57%` edition, `622/0` JIT parity) |
| [open-cards.md](open-cards.md) | Residual tracker: open engine/JIT/conformance items, closed cards ledger, and Phase-4 receipts (`Q1.1`-`Q3`, `P1`, `C3.1`, `ARM64 WB`, `Pre-Q1.5`) |
| [current-state.md](current-state.md) | Baseline repository inventory, architecture/build/test summaries, platforms, activity, constraints, gaps |
| [arm64-macos-jit.md](arm64-macos-jit.md) | ARM64 macOS JIT (`9004b3ed7`; M1 tag `jit-arm64-macos-m1`) bring-up record |
| [architecture.md](architecture.md) | Engine architecture: component map, execution pipeline, JIT tiers, JsRT API, threading, GC, Intl/WASM, with a Mermaid diagram |
| [build.md](build.md) | Build process: entry points, prerequisites, Unix + Windows builds, CMake structure, ICU matrix, codegen, outputs, testing, CI, staleness |
| [modernization-plan.md](modernization-plan.md) | Executable roadmap: staged build/platform rollout (F0-B4, B5-B6), Phase-3 JsRT API versioning contract, C2 ES2025 batches, Zig strategy |
| [c1-triage.md](c1-triage.md) | C1 record: edition policy, qualified runner, root-cause triage and the measured S-row shortlist |
| [c2-s4.md](c2-s4.md) / [c2-s5.md](c2-s5.md) / [c2-s10.md](c2-s10.md) / [c2-s11.md](c2-s11.md) / [c2-s7-matchall.md](c2-s7-matchall.md) / [c2-s9-namedgroups.md](c2-s9-namedgroups.md) | C2 batch receipts: optional chaining + logical assignment; iterator helpers; Set methods + copy-by-change; small builtins + replaceAll; matchAll + RegExp symbol-protocol fixes; RegExp named groups + `d` flag/indices + duplicate named groups + lookbehind |
| [c2-flag-audits.md](c2-flag-audits.md) | Measured enabled-flag audits for BigInt, SharedArrayBuffer/Atomics and the RegExp symbol family |
| [b1-b3-review.md](b1-b3-review.md) / [b4-gate.md](b4-gate.md) / [b4-review.md](b4-review.md) | External reviews and gate verdicts for B1-B3 and B4 |
| [conformance-baseline.md](conformance-baseline.md) | Pinned upstream diagnostic baseline: configuration, executed/skip counts, feature debt and edition-gate limits |
| [e12-testing-quality.md](e12-testing-quality.md) | Testing-quality lane: one-command certified-baseline refresh (`tools/test262/refresh_baseline.py`) with its validation run, the bounded GCStress campaign over the rlexe core, and flake-family hardening |
| [memory-management.md](memory-management.md) | Coding-assistant ownership guide: TigerStyle/Ghostty/Bun lessons applied to allocators, GC/FFI, callbacks, async lifetime and focused tests |
| [swarm-review-final-report.md](swarm-review-final-report.md) / [a2-serialization-homeobject.md](a2-serialization-homeobject.md) / [a6-smint-pair-test.md](a6-smint-pair-test.md) / [a7-link-validation.md](a7-link-validation.md) | A1-A12 + JITREG adversarial review report and lane receipts (`[[HomeObject]]` bytecode serialization, ARM64 `GenerateSmIntPairTest`, asm.js/SIMD link validation) |
| [wasm-arm64-p2-jit.md](wasm-arm64-p2-jit.md) / [wasm-arm64-w4-differential.md](wasm-arm64-w4-differential.md) | ARM64 WebAssembly JIT receipts (P2 value-path fixes and W4 dynapogo vs interpreted differential) |

Conventions:

- Lane receipts archive their final-state gate logs lane-named under
  `out/` and cite them (started by the B4 lane -- `out/b4-gate-logs/`;
  e.g. `out/e7-gate-test.log` / `out/e7-gate-debug.log`, re-run on the
  exact committed tree after the review legs), per the evidence-hygiene
  lessons in [b4-gate.md](b4-gate.md) and [b4-linux.md](b4-linux.md).
- Engine/build claims cite in-repo `path:line` references at the pinned baseline;
  external toolchain and language-edition claims cite primary sources.
- `build.md` distinguishes source-inspected recipes from run-verified results:
  Section 13 records the legacy Mac baseline, Section 14 the Zig pilot and its corrections.
- Zig 0.17.0 APIs were checked against its installed sources and release notes.
  `[NEEDS VERIFICATION on Zig 0.17]` identifies a specific remaining experiment,
  not an unchecked API spelling. ZLS and incremental-link research do not gate
  Phase 1.
- The roadmap's milestone gates define the active implementation scope. Detailed
  inventories describe reference behavior and future work, not a requirement to
  ship every legacy option or ecosystem feature in the first migration.
- `[UNVERIFIED EXTERNAL]` marks competitor-ecosystem claims without a primary
  source -- keep generic until sourced.

How these were produced: parallel research agents mapped the architecture, build,
current state, and feature gaps from repo sources; independent skeptic agents
reviewed each draft against file bodies; the maintainer then resolved review
conflicts (notably `flat`/`flatMap` presence and commit-date counting) by opening
the files, and a final review pass re-checked the saved documents.

The subsequent execution review corrected the JsRT runtime/TLS ownership model,
inherited ES2018 RegExp gaps, callback-retention API, executable stack setting,
and deployment-target assumptions. Modern platform floors are intentional policy
choices ([modernization-plan.md](modernization-plan.md) Section 2.1), rather than inherited
legacy requirements or the build host's defaults. The durable pilot and pinned
conformance tooling live in [tools/zig-pilot](../tools/zig-pilot/README.md) and
[tools/test262](../tools/test262/README.md).
