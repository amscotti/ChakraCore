# C4 — Feature/result manifest (certification-grade baseline, refreshed)

Published 2026-11-02 by the `c4-certification` lane; **republished
2026-11-05 by the `c4-refresh` closing lane** after the A–E engine lanes
landed. This is the published feature/result manifest required by
[plan §C4](modernization-plan.md); the gate verdict against the C4 "Done
when" text is [c4-gate.md](c4-gate.md), the raw run evidence of the
original certification is [c4-run-evidence.md](c4-run-evidence.md), and
the refresh's delta reconciliation is [c4-refresh.md](c4-refresh.md)
(machine artifact:
[`tools/test262/c4-refresh-delta.json`](../tools/test262/c4-refresh-delta.json)).
**This manifest records a measurement; it does not claim ES2025
conformance** — 2,949 edition-selected failures remain (§2).

## 1. Engine, build and tool identity (refresh run, register `refresh-2026-11-05`)

| Field | Value |
|---|---|
| Engine commit | `4e9f95da7` (23 engine commits after the certified `ba796f74f`; the refresh lane adds none — `git diff HEAD -- lib pal bin` empty) |
| Declared configuration | Mac ARM64, **shared / no-JIT `test` profile** (RelWithDebInfo/Test), `aarch64-macos.26.0`, SDK 26.5, ICU 79.1 (Homebrew), Zig 0.17.0 |
| Host shell | `zig-out/bin/ch`, SHA-256 `5e210836…fea7028` (dylib identity in the run `summary.json`; tree dirty at run time with tool/doc files only, binary built at HEAD first) |
| Corpus | tc39/test262 **`2e0a56762801e275a9fdf96dc49d90ba0cddcf63`** (2026-10-08, unchanged), patterns `test/language/**`, `test/built-ins/**`, `test/annexB/**` |
| Runner | test262-harness 10.0.0 / eshost, Node v26.5.0, Python 3.12.13; one-command refresh via `tools/test262/refresh_baseline.py` (E12) |
| Host flags | `-nonative -Test262 -CollectGarbage -NegativePhaseReport` (runner defaults; **no feature flags enabled**) |
| Jobs / timeout | 8 / 10,000 ms per scenario |
| Controls | **25/25** (incl. 5 marker-phase controls + the forged-marker adversarial control) |

JIT note (unchanged): the declared supported execution configurations for
*this* manifest are the Mac no-JIT gates (2051/2103, §7). Per-batch
test262 under SimpleJIT/FullJIT on the merged Mac ARM64 JIT profile is
discharged for 17/19 C2 batches with the remainder carded
([c2-jit-test262.md](c2-jit-test262.md), [open-cards.md](open-cards.md) —
the e12 fulljit SIGSEGV family); Linux JIT is B4-blocked; neither broader
claim is made here.

## 2. Result totals and failure inventory

| Measure | Broad diagnostic | Edition-selected (ES2025 policy) |
|---|---:|---:|
| Scenarios | 93,141 | 80,544 |
| Passed | **78,399** | **77,595** |
| Failed | **14,742** | **2,949** |
| Rate | 84.17% | **96.34%** |
| Excluded (post-ES2025 tags) | — | 12,597 (804 pass / 11,793 fail) |
| Skipped pre-execution (§4) | 902 | 902 |

Failure buckets — c1-triage §2 rules (first match wins; tag/signal
classification, not per-test root-cause proof; re-derived independently
from the retained `results.json`, §6 of [c4-refresh.md](c4-refresh.md)).
Edition-selected failures 2,949:

| Bucket | Count (cert → refresh) | Dominant clusters (fresh) |
|---|---:|---|
| disabled-flag | **1,734** (1,734 → 1,734) | `ESSharedArrayBuffer` **1,176** (Atomics incl. `waitAsync` 202 + wait/notify 252, SharedArrayBuffer 912-tagged overlap, SAB-gated TypedArray/DataView/ArrayBuffer ctor+prototype surfaces); `ES6RegExSymbols` **558** (RegExp Symbol protocol: Symbol.replace 162, Symbol.split 98, Symbol.match 142, Symbol.matchAll 112 + String matchAll/replaceAll substrate, Symbol.search 56) |
| engine-defect-on-existing | **1,010** (1,700 → 1,010) | RegExp property escapes 112 (S3 lead-surrogate lacunarity, decision-blocked); dynamic-import semantics 68 (cards B1-1…B1-10); Function/prototype 56 (fn own-key order + anonymous `name`, carded); Date/prototype 52; TypedArray internals 44 + ctors 104; Array/prototype 40 (incl. E11-2 unshift 6); RegExp/prototype 36; DataView offset-argument 32; JSON/parse 28; for-await-of 24; `AsyncFromSyncIteratorPrototype` 26; … |
| required-feature-missing | **202** (220 → 202) | `RegExp/prototype` accessor getters (the `-ES6RegExPrototypeProperties` substrate seen through the missing-signal rule) **156** (unchanged); misc 46 (module-code string-export names 10 = B1-8, misc parse gaps 36) |
| host | **3** (426 → 3) | 1 `decl-pos-import-labeled` URIError (B1-6 parser gap surfacing through the loader) + 2 `Atomics/wait/good-views` deterministic hangs (E11-3 — SAB-owned work that *manifests* as timeout; the bucket rules keep timeouts in `host`, see c4-refresh §3) |

Top edition-selected failure directories (fresh): `RegExp/prototype` 588
· `Atomics/waitAsync` 202 · `SharedArrayBuffer/prototype` 156 ·
`Atomics/wait` 150 · `String/prototype` 114 · `RegExp/property-escapes`
112 · `dynamic-import` 68 · `DataView/prototype` 64 ·
`Function/prototype` 56 · `TypedArrayConstructors/ctors` 54+50 ·
`Date/prototype` 52.

## 3. Negative-phase statistics (host-reported phase)

- **8,751 negatives marker-judged** (8,652 parse + 34 resolution + 65
  runtime) — every marker at line 1 of stderr, **0 displaced** markers,
  0 natural forgeries; **0 shape suspects**.
- **8,724 passed negatives** (8,633 parse + 33 resolution + 58 runtime;
  8,672 at certification — the +52 are e7's 40 early-error executions
  plus e5/e6 parse fixes, matching the §6 ledger); **1 downgrade**
  (`Negative phase mismatch`): the post-ES2025-excluded `import-defer`
  scenario — correct: the parser still rejects `import defer` at parse
  where resolution is declared.
- 28 failing negatives (80 at certification; marker judging does not
  apply on the fail side — these are harness-failed or marker-less
  shapes, the early-error tail the lanes have been consuming).
- Known host limits carried from [c1-negative-phase.md](c1-negative-phase.md)
  unchanged.

## 4. Skip / host-applicability inventory (re-verified on this run)

| Skip class | Count | Justification (unchanged) |
|---|---:|---|
| `duplicate-module-scenario` | 828 | Upstream harness generates a strict-mode scenario variant for module tests; module code is always strict, so the duplicate is skipped and the original runs once (`host-filter.js`; C1-recorded). |
| `unsupported-optional-HTMLDDA-host` | 70 | `IsHTMLDDA` tests require the optional HTML `document.all` exotic; this host defines none (`harness.js` deletes it). Optional per INTERPRETING.md. |
| `blocking-host` (CanBlockIsFalse) | 4 | Tests requiring a non-blocking main agent; the `ch` host agent can block (INTERPRETING.md agent/CanBlock). |
| *(arch / backend tag skips)* | **0 — none exist** | `host-filter.js` is the only skip source and encodes exactly the three rules above (902 = total). Platform limits surface as *failures*, never silent skips. |

## 5. ES2025-required features vs the default configuration

**(a) Default-enabled, with probe evidence.** The certification's
feature-default audit carries over **unchanged** — spot-verified on the
certified binary with `feature-probe.js` at refresh time (all ES2025
additions default-on: modifiers, duplicate named groups, Set methods,
iterator helpers, `Promise.try`, `RegExp.escape`, Float16, import
attributes + JSON modules, plus the earlier-edition set per the S1–S14
receipts). No drift was found.

**(b) Flag-only in the default configuration** (the policy's
`disabled_by_default_flags`; each a recorded decision; **counts unchanged
at the refresh**):

| Flag | Required features held off default | Edition failures owed | Decision owner |
|---|---|---:|---|
| `-ESSharedArrayBuffer` | `SharedArrayBuffer` + `Atomics` incl. `waitAsync`, growable SAB, BigInt64 Atomics; SAB-gated TypedArray/DataView/ArrayBuffer surfaces | **1,176** | Recorded product/security decision (plan §4.1 S6, §6 "S6 default"); flag-on ceilings recorded (S6e ≈895/982; e10 10/10) |
| `-ES6RegExSymbols` | `RegExp.prototype[Symbol.match/matchAll/replace/search/split]`, `String.prototype.matchAll`, `@@replace` dispatch | **558** | S7/S11 receipts: flag stays off (JIT-inlined String/RegExp helper re-check, `ConfigFlagsList.h:1177`); e8 flag-on ceilings: matchAll 50/0, replaceAll 92/0 |
| `-ES6RegExPrototypeProperties` | prototype getters `source`/`flags`/`dotAll`/`hasIndices`/…; no dedicated corpus tag | **156** (signal-classified rfm; exactly re-measured) | S9 sub-batch 2 (corpus-pinned); e8 flag-on ceiling 84/0 |

**The three decisions together are worth exactly −1,890** edition
failures — the single largest lever, unchanged by the A–E lanes.

**(c) Remaining partials** (edition-selected failures per feature,
fresh per-feature table; tag counts overlap): the flag-gated families
dominate — TypedArray 812 (756 SAB-gated + 56 defect), ArrayBuffer 206
(all SAB-gated), BigInt 284 (264 SAB-gated + 20 defect), resizable-AB 106
(104 SAB-gated + 2 `Reflect.construct` card), DataView 112 (all
SAB-gated), Atomics/Atomics.waitAsync 746+202 (all SAB-gated), the
RegExp Symbol protocol (`Symbol.replace` 162 · `Symbol.match` 142 ·
`Symbol.split` 98 · `Symbol.matchAll` 112 · `Symbol.search` 56, all
`ES6RegExSymbols`-gated). Defect-side
partials: property escapes 112 (S3 decision-blocked) · dynamic-import 68
(B1 cards) · cross-realm 35 (28 flag-substrate per e3-R1 + 7 carded) ·
RegExp/prototype 36 · matchAll 30 / replaceAll 28 / match-indices 26 —
**e8-proven substrate-gated, zero engine defect** (flag-on 84/0 family
ceilings; `replaceAll` is now 100% disabled-flag-bucketed) ·
tail-call-optimization 34 (card E11-1: never implemented, V8-parity
posture, C3-class) · Function `name`/key-order 44+ · v-flag 28 ·
named groups 20 · JSON/parse 28 · for-await-of 24 · untagged 405 (the
largest remainder: assorted untagged semantic gaps across
built-ins). **Partials that closed to zero with the A–E lanes:**
import-attributes (was 19), json-modules (2), export-star (8),
class-static-block (6), logical-assignment (6), object-rest (6),
AggregateError (2), class + class-element families (76→24 union),
compound-assignment/let-ASI (112), dynamic-import host loader (473),
cross-realm defect core (130), detached-buffer families (124),
early-error executions (40), BigInt toLocaleString (42),
resizable-AB families (22), Array integer-limit timeouts (8), TA
OOB-delete (4). Full table:
[`e12-refresh-analysis.json`](../tools/test262/e12-refresh-analysis.json).

## 6. Gate-relevant deltas vs the certified baseline

Broad and edition both move **−1,131 failures, zero regressions**;
buckets: host −423, engine-defect −690, rfm −18, disabled-flag 0. Every
significant delta is attributed per lane in
[c4-refresh.md](c4-refresh.md) §3 (two-stage ledger with per-key joins
against the retained `da0a72dbb` midpoint and the retained fresh run;
residue −16 improvement in stage A = corpus ripple outside the lane
selections, −2 over-booking in stage B = the C-sweep's delete-half
estimate; the e11 host→disabled-flag reclassification provably cannot
materialize under the bucket rules and is recorded as such).

## 7. Regression / bytecode / native checks (this lane's tree)

`zig build` rc=0 · `zig build check` **rc=0** (regression **2051/0**,
bytecode nojit up-to-date + pure-verify, native `test-shared-basic`
PASS, smoke PASS, metadata PASS) · `-Dprofile=debug-static
check-regression` **2103/0**. Logs `out/c4r-gate-{test,debug}.log`.

## 8. What this manifest does and does not claim

It certifies the *measurement*: a pinned, control-qualified,
negative-phase-proven full-corpus result on the declared Mac no-JIT
configuration, with every skip justified and every delta attributed —
now including the complete A–E program's effect (94.93% → **96.34%**,
4,080 → 2,949). It does **not** claim the C4 gate: the edition-selected
suite is not green (2,949 owed), and three required flag surfaces are
not in the default configuration (one by recorded product decision).
The clause-by-clause grading and the ranked path to green are in
[c4-gate.md](c4-gate.md).
