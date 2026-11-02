# C4 — Feature/result manifest (certification-grade baseline)

Published 2026-11-02 by the `c4-certification` measurement lane. This is
the published feature/result manifest required by
[plan §C4](modernization-plan.md); the gate verdict against the C4 "Done
when" text is [c4-gate.md](c4-gate.md), and the raw run evidence
(commands, log excerpts, identity) is [c4-run-evidence.md](c4-run-evidence.md).
**This manifest records a measurement; it does not claim ES2025
conformance** — 4,080 edition-selected failures remain (§2).

## 1. Engine, build and tool identity

| Field | Value |
|---|---|
| Engine commit | `ba796f74f` (= main checkpoint; lane adds no engine changes) |
| Declared configuration | Mac ARM64, **shared / no-JIT `test` profile** (RelWithDebInfo/Test), `aarch64-macos.26.0`, SDK 26.5, ICU 79.1 (Homebrew), Zig 0.17.0 |
| Host shell | `zig-out/bin/ch`, SHA-256 `bc74751b…ce99f09` (dylib `cc89d40b…57a287f`) |
| Corpus | tc39/test262 **`2e0a56762801e275a9fdf96dc49d90ba0cddcf63`** (2026-10-08), patterns `test/language/**`, `test/built-ins/**`, `test/annexB/**` |
| Runner | test262-harness 10.0.0 / eshost, lockfile SHA-256 `f5520103…` (full identity in `summary.json`), Node v26.5.0, Python 3.12.13 |
| Host flags | `-nonative -Test262 -CollectGarbage -NegativePhaseReport` (runner defaults; **no feature flags enabled**) |
| Jobs / timeout | 8 / 10,000 ms per scenario |
| Controls | **25/25** (incl. 5 marker-phase controls + the forged-marker adversarial control) |

JIT note: the declared supported execution configurations for *this*
manifest are the Mac no-JIT gates (2038/2090, §7). Per-batch test262 runs
under SimpleJIT/FullJIT on the merged Mac ARM64 JIT profile are owed per
C2 receipt, and Linux JIT is B4-blocked; neither is claimed here.

## 2. Result totals and failure inventory

| Measure | Broad diagnostic | Edition-selected (ES2025 policy) |
|---|---:|---:|
| Scenarios | 93,141 | 80,544 |
| Passed | **77,268** | **76,464** |
| Failed | **15,873** | **4,080** |
| Rate | 82.96% | 94.93% |
| Excluded (post-ES2025 tags) | — | 12,597 (804 pass / 11,793 fail) |
| Skipped pre-execution (§4) | 902 | 902 |

Failure buckets — c1-triage §2 rules (first match wins; tag/signal
classification, not per-test root-cause proof). Broad failures 15,873 =
11,793 post-ES2025 + the four edition buckets:

| Bucket | Count | Dominant clusters |
|---|---:|---|
| post-ES2025 (excluded from edition score) | 11,793 | `Temporal` 9,226; `Array.fromAsync` 186; `explicit-resource-management` 709; `source-phase-imports` 187+83; `ShadowRealm` 124 … |
| disabled-flag | **1,734** | `ESSharedArrayBuffer` **1,176** (Atomics 744, SharedArrayBuffer 208, SAB-gated TypedArray/DataView 186, …); `ES6RegExSymbols` **558** (RegExp Symbol protocol 412, String matchAll 96, RegExpStringIterator 34, …) |
| engine-defect-on-existing | **1,700** | dynamic-import (non-host-shaped) 124; compound-assignment 66; detached-buffer TypedArray/DataView ~104; class residue ~84; `let`-ASI over-rejection; early-error `$DONOTEVALUATE` executions ~70; … |
| host | **426** | dynamic-import fixture loader `URIError` 419 (+4 Array, 2 Atomics wait, 1 module-code) |
| required-feature-missing | **220** | `RegExp/prototype` accessor getters (the `-ES6RegExPrototypeProperties` substrate seen through the missing-signal rule: `dotAll`/`flags`/`source`/`hasIndices`/… descriptors absent) 156; `let`-newline ASI SyntaxErrors 22; … |

Top edition-selected failure directories: `RegExp/prototype` 588 ·
`language/expressions/dynamic-import` 543 · `Atomics` 746 ·
`SharedArrayBuffer` 208 · `TypedArray/prototype` 128 ·
`TypedArrayConstructors/internals` 116 · `String/prototype` 116 ·
`RegExp/property-escapes` 112 · `language/expressions/compound-assignment` 66.

## 3. Negative-phase statistics (host-reported phase)

- **8,751 negatives marker-judged** (8,652 parse + 34 resolution + 65
  runtime) — every marker at line 1 of stderr, **0 displaced** markers,
  0 natural forgeries across 23,517+ marker-carrying records.
- 8,673 marker-judged harness-passing negatives, of which **1 downgraded**
  (`Negative phase mismatch`): the post-ES2025-excluded `import-defer`
  scenario — correct: the parser still rejects `import defer` at parse
  where resolution is declared.
- 79 marker-less negatives (crash/timeout shape fallback; all
  harness-failed): 78 `marker+harness-failed`, 1 `shape+harness-failed`.
- 0 shape suspects among marker-judged records (clean by construction).
- Known host limits carried from [c1-negative-phase.md](c1-negative-phase.md):
  deep-child link errors and deferred-parse early errors classify
  `runtime`; the sticky resolution witness and agent-thread emission
  remain latent (0 incidence here).

## 4. Skip / host-applicability inventory (all verified on this run)

| Skip class | Count | Justification (still valid) |
|---|---:|---|
| `duplicate-module-scenario` | 828 | Upstream harness generates a strict-mode scenario variant for module tests; module code is always strict per spec, so the duplicate is skipped and the original source runs once (`host-filter.js`; C1-recorded). Count unchanged from C1. |
| `unsupported-optional-HTMLDDA-host` | 70 | `IsHTMLDDA` tests require the optional HTML `document.all` exotic; this host defines no fake exotic (`harness.js` deletes it). Optional per INTERPRETING.md; count unchanged. |
| `blocking-host` (CanBlockIsFalse) | 4 | Tests requiring a non-blocking main agent; the `ch` host agent can block, so they do not apply (INTERPRETING.md agent/CanBlock). Count unchanged. |
| *(arch / backend tag skips)* | **0 — none exist** | Verified: `host-filter.js` is the only skip source and encodes exactly the three rules above (4+828+70 = 902 = total recorded skips). No architecture- or backend-based exclusion exists in this runner; platform limits (e.g. the Mac asm.js/WASM JIT gap, stack depth) surface as *failures*, never silent skips. |

## 5. ES2025-required features vs the default configuration

**(a) Default-enabled, with probe evidence** (this run's
`feature-probe.js` + extended probe transcripts, §4 of the evidence doc;
all ES2025 additions from the C1 anchor set):

| ES2025 feature | Probe result (default flags) | Receipt |
|---|---|---|
| RegExp modifiers `(?ims-ims:…)` | parses/executes | S9 sub-batch 5 |
| RegExp duplicate named groups | `groups.x` → participating value | S9 sub-batch 3 |
| Set methods (×7) | `typeof` → `function` | S10 |
| Iterator helpers (`Iterator`, `.map`, `Iterator.from`) | `function` | S5 |
| `Promise.try` | `function` | S11 |
| `RegExp.escape` | `function` | S11 |
| `Float16Array` / `Math.f16round` / `DataView.getFloat16` | `function`; `f16round(1.33)` = 1.330078125 | S8 |
| Import attributes + JSON modules | module loads/links/evaluates | S12 |
| Closed earlier-edition required set | class elements, BigInt (default since S2-default), property escapes, `v` flag, lookbehind, `d`/indices, named groups, resizable ArrayBuffer + `transfer`/`transferToFixedLength`, WeakRef/FinalizationRegistry, symbol weak keys, optional chaining, logical assignment, replaceAll, groupBy, well-formed strings, withResolvers, change-array-by-copy — all probe-true | S1–S14 receipts |

**(b) Flag-only in the default configuration** (the policy's
`disabled_by_default_flags`, each a recorded decision):

| Flag | Required features held off default | Edition failures owed | Decision owner |
|---|---|---:|---|
| `-ESSharedArrayBuffer` | `SharedArrayBuffer` + `Atomics` incl. `waitAsync`, growable SAB, BigInt64 Atomics; SAB-gated TypedArray/DataView constructors | **1,176** | Recorded product/security decision (plan §4.1 S6, §6 "S6 default"); probes: default `typeof SharedArrayBuffer/Atomics` = `undefined`, flag-on = `function`/`object` |
| `-ES6RegExSymbols` | `RegExp.prototype[Symbol.match/matchAll/replace/search/split]`, `String.prototype.matchAll`, RegExp String Iterator | **558** | S7/S11 receipts: flag stays off (JIT-inlined String/RegExp helper re-check, `ConfigFlagsList.h:1177`); probes: default `undefined`, flag-on `function` |
| `-ES6RegExPrototypeProperties` | prototype getters `source`/`flags`/`dotAll`/`hasIndices`/… (+ legacy instance built-in removal); no dedicated corpus tag | **≥156** (subset of the 220 rfm bucket) | S9 sub-batch 2 (corpus-pinned); probe: descriptors `absent` default, `getter` flag-on |

**(c) Partial (default-enabled but with edition failures left)**:
BigInt 378 · property escapes 114 (S3 residue — lead-surrogate lacunarity,
decision-blocked) · `v` flag 28 · dynamic-import 545 (419 host + 126
edition buckets) · import attributes 19 · json-modules 2 ·
promise-with-resolvers 2 · match-indices 26 · named groups 20 · duplicate
named groups 2 · resizable-arraybuffer 130 · replaceAll 28 ·
tail-call-optimization 34 (ES2015, never implemented) · TypedArray
surfaces ~1,010 (detach-semantics/SAB-gated overlap) · cross-realm 185 ·
`Proxy`/`Reflect` residue. Full per-feature table:
[`c4-cert-analysis.json`](../tools/test262/c4-cert-analysis.json).

## 6. Gate-relevant deltas vs the previous recorded baseline

Baseline: c1-negative-phase full run (2026-11-01, base `b490dc6de`+lane):
broad **77,257/15,884**, edition **76,453/4,091**, 12 downgrades. This
run: broad **77,268/15,873**, edition **76,464/4,080**, 1 downgrade.
Key-join: **+11 fail→pass, 0 pass→fail**, every one of the 11 owned by
the s12-module-phase lane (merge `8f17ba38d`; 9 import-attribute shape
tests + `instn-star-err-not-found` + `json-invalid` — also the 12→1
downgrade drop). gc-async-continuation (microtask-before-timer ordering),
dbg-async-attach and the E1/S6e repair lanes flipped **zero** corpus
scenarios (the ordering fix's witness is the regression gate's
`es7/async-microtask-order.js`). Node version, jobs, flags, corpus
revision and policy hash identical → numbers are directly comparable.

## 7. Regression / bytecode / native checks (this lane's build)

`zig build` rc=0 · `zig build check` **rc=0** (regression **2038/0**,
bytecode nojit 32b/64b "up to date" + pure-verify rc 0, native
`test-shared-basic` PASS, smoke PASS, metadata PASS) ·
`-Dprofile=debug-static check-regression` **2090/0**. Full excerpts in
[c4-run-evidence.md](c4-run-evidence.md) §5.

## 8. What this manifest does and does not claim

It certifies the *measurement*: a pinned, control-qualified,
negative-phase-proven full-corpus result on the declared Mac no-JIT
configuration, with every skip justified and every delta attributed. It
does **not** claim the C4 gate: the edition-selected suite is not green
(4,080 owed), and three required flag surfaces are not in the default
configuration (one by recorded product decision). The clause-by-clause
grading and the ranked path to green are in [c4-gate.md](c4-gate.md).
