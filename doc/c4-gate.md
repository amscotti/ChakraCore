# C4 gate verdict (measurement lane, 2026-11-02)

The `c4-certification` lane's job was the honest measurement: a fresh
certification-grade full-corpus run on this lane's binaries, the published
feature/result manifest, and this clause-by-clause grade vs the plan's C4
"Done when" text. **No engine source was changed** (`git diff HEAD -- lib
pal bin` empty; lane = main `ba796f74f` + docs/artifacts). Evidence:
[c4-run-evidence.md](c4-run-evidence.md) (commands, receipts),
[c4-manifest.md](c4-manifest.md) (the manifest). Style mirrors
[b4-gate.md](b4-gate.md).

> Verdict: **C4 is BLOCKED-with-certified-baseline** — the runner, the
> measurement and every preservation clause are green (25/25 controls,
> 8,751 marker-judged negatives, gates 2038/2090 + bytecode/native/metadata,
> manifest published), but the two substantive clauses are red: the
> edition-selected suite is **not green** (76,464 / **4,080 failed**, 94.93%)
> and **three required feature surfaces are not in the default
> configuration** (SAB/Atomics by recorded product/security decision, the
> two RegExp-substrate flags by their recorded receipts). No ES2025
> conformance claim is made. See §3 for the ranked path to green.

## 1. C4 "Done when", clause by clause

Plan §4.1 C4: *"the qualified, pinned required ES2025 suite is green on
the declared supported engine configurations, required features are
enabled in the claimed default configuration, and all skips/host
applicability are justified. Preserve negative-phase validation,
regression/bytecode/native checks and a published feature/result
manifest."*

| # | Clause | Grade | Evidence |
|---|---|---|---|
| 1 | Qualified, pinned required ES2025 suite **green** on declared supported configurations | **NO** | Pinned `2e0a5676`, qualified runner, controls 25/25 — but edition-selected **4,080 failures** of 80,544 (94.93%): disabled-flag 1,734 · engine-defect 1,700 · host 426 · required-feature-missing 220 (manifest §2) |
| 2 | Required features enabled in the **claimed default** | **NO** | Probe-proven off by default: `SharedArrayBuffer`+`Atomics` (1,176 edition failures; recorded product/security decision, S6) and the RegExp `ES6RegExSymbols`/`ES6RegExPrototypeProperties` substrates (558 tag-mapped + ≥156 signal-classified; S7/S9 recorded flag decisions). All nine ES2025 additions themselves are default-on with probe evidence (manifest §5) |
| 3 | All skips / host applicability justified | **PASS** | 902 skips, exactly the three C1-recorded classes with unchanged counts (828 duplicate strict modules / 70 HTMLDDA / 4 CanBlockIsFalse), each re-verified; no arch/backend skip class exists in this runner (manifest §4) |
| 4 | Preserve **negative-phase validation** | **PASS** | Host marker re-judging active corpus-wide: 8,751 marker-judged negatives, 0 displaced/0 forged, **1 downgrade** (the post-ES2025-excluded `import-defer` case; down from 12 — the other 11 now genuinely pass via s12-module-phase). Controls incl. the spoof control 25/25 (manifest §3) |
| 5 | Preserve **regression/bytecode/native checks** | **PASS** | This lane's build: `zig build` rc=0; `zig build check` rc=0 with **2038/0** regression, bytecode nojit 32b/64b up-to-date + pure-verify, native `test-shared-basic` PASS, smoke + metadata PASS; debug-static `check-regression` **2090/0** (evidence §5) |
| 6 | Published **feature/result manifest** | **PASS** | [c4-manifest.md](c4-manifest.md) (this commit): identity, buckets, feature-default table, skip inventory, negative-phase statistics, deltas, receipts |

Clause 1 also has a second axis: "declared supported engine
configurations". The manifest covers the Mac no-JIT gates; per-batch
test262 under SimpleJIT/FullJIT (Mac ARM64 JIT) and all Linux JIT claims
remain owed per the C2 receipts (B4-blocked / in-flight lanes) — green
here would still not authorize a two-configuration claim.

## 2. Why this is not CONDITIONAL

Like B4, the red clauses are substantive, not pending-confirmation: no
future CI run turns 4,080 failures green, and no evidence elsewhere can
make an off-by-default feature "enabled in the claimed default". What
*is* newly green — the qualified measurement itself (controls, marker
judging, gates, manifest) — is exactly what this lane was scoped to
deliver, hence BLOCKED-with-certified-baseline rather than merely
BLOCKED: C4 now has a phase-proven starting point and an exact owed list.

## 3. Exact path to green (ranked by size, with owners)

1. **disabled-flag 1,734** — *decision-gated, then repair-gated.*
   (a) SAB/Atomics default (**1,176**): the recorded product/security
   decision (S6) must be taken (flag-on conformance is already ≈895/982
   per S6e; remainder = flag-on defect tail). (b) RegExp Symbol protocol
   (**558**) + prototype properties (**≥156** of the 220 rfm bucket):
   recorded S7/S9 decisions + the `ConfigFlagsList.h:1177` JIT-inlined
   helper re-check; the flag flip itself is small, the re-check and any
   default/JIT-configuration claims are the work. Flipping either default
   additionally owes the per-receipt B4/JIT-corpus obligations.
2. **engine-defect-on-existing 1,700** — *lane work.* Largest named
   clusters: dynamic-import semantics 126 + its host-shaped 419 (a host
   loader/engine lane — no owner yet, biggest single unowned cluster);
   compound-assignment/eval-var family 66+ (S14 stream, documented
   residuals); detached-buffer TypedArray/DataView ~104
   (align-detached-buffer decision); class-element residue ~84 (S1 tail);
   rfm `let`-newline ASI over-rejection 14; early-error executions ~70 (S14);
   property-escapes residue 114 (S3 lead-surrogate, decision-blocked);
   cross-realm 185; `Reflect`/`Proxy` tails.
3. **host — DONE for the loader class (B1, 2026-11-03):** dynamic-import
   fixture resolution fixed (script-relative); the certified host
   bucket drops 426 → **7** (1 module-code parse-shape + 4 Array
   integer-limit timeouts + 2 Atomics-wait hangs, all engine-owned
   work). Engine-defect correspondingly **1,700 → 1,646**. Historical
   remainder of this clause (pre-B1): 419 are dynamic-import fixture
   `URIError`s from the ch host loader; fix the loader or record the
   host-applicability limit explicitly (it is currently an unjustified
   failure, not a skip). Review correction: the remaining 7 are 1
   harness-shaped module-resolution failure + 6 deterministic
   engine timeouts (4 Array integer-limit O(2^53) work, 2 Atomics-wait
   hangs) — engine-owned work, not loader issues.
4. **required-feature-missing 220** — mostly collapses with 1(b) (156
   RegExp/prototype getters); remainder ~64: `let`-ASI 14 (review-corrected; rfm-bucketed over-rejections, S14-adjacent) and misc
   parse-gaps, each small enough to fold into S14-adjacent lanes.

Zero-failure clauses 3–6 need no work; re-run this lane's commands after
each landing lane to refresh the manifest (the analysis artifact
regenerates deterministically from `results.json` + the pinned policy).

## 4. Scope audit

Commit (a) `becb482b4`: run evidence + committed analysis
(`tools/test262/c4-cert-analysis.json`, `doc/c4-run-evidence.md`).
Commit (b): this file + `doc/c4-manifest.md` + the plan's C4 checklist
line updated to point here (C4 stays unchecked). Raw run artifacts are
ignored `out/` content per repo convention; the manifest quotes every
gate-relevant number.
