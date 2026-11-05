# C4 gate verdict (measurement lane, 2026-11-02; refreshed 2026-11-05)

The `c4-certification` lane delivered the honest measurement: a fresh
certification-grade full-corpus run, the published feature/result
manifest, and the clause-by-clause grade below. The `c4-refresh` closing
lane (2026-11-05) re-measured after the A–E engine lanes landed and
republished the manifest; this file carries the refreshed grades and the
re-ranked path to green. **No engine source changed in either lane.**
Evidence: [c4-run-evidence.md](c4-run-evidence.md) (certification
commands/receipts), [c4-manifest.md](c4-manifest.md) (the manifest),
[c4-refresh.md](c4-refresh.md) (refresh delta reconciliation).

> Verdict (unchanged in kind, refreshed in numbers): **C4 is
> BLOCKED-with-certified-baseline** — the runner, the measurement and
> every preservation clause are green (25/25 controls, 8,751
> marker-judged negatives, gates 2051/2103 + bytecode/native/metadata,
> manifest published with every delta attributed), but the two
> substantive clauses are red: the edition-selected suite is **not
> green** (77,595 / **2,949 failed** of 80,544, 96.34%) and **three
> required feature surfaces are not in the default configuration**
> (SAB/Atomics by recorded product/security decision, the two
> RegExp-substrate flags by their recorded receipts). No ES2025
> conformance claim is made. See §3 for the ranked path to green.

## 1. C4 "Done when", clause by clause

Plan §4.1 C4: *"the qualified, pinned required ES2025 suite is green on
the declared supported engine configurations, required features are
enabled in the claimed default configuration, and all skips/host
applicability are justified. Preserve negative-phase validation,
regression/bytecode/native checks and a published feature/result
manifest."*

| # | Clause | Grade | Evidence (refresh) |
|---|---|---|---|
| 1 | Qualified, pinned required ES2025 suite **green** on declared supported configurations | **NO** | Pinned `2e0a5676`, qualified runner, controls 25/25 — but edition-selected **2,949 failures** of 80,544 (**96.34%**): disabled-flag 1,734 · engine-defect 1,010 · rfm 202 · host 3 (manifest §2). Down from 4,080 (94.93%) at certification: −1,131 flips, 0 regressions, every delta attributed (c4-refresh §3) |
| 2 | Required features enabled in the **claimed default** | **NO** | Probe-proven off by default and **unchanged at refresh** (spot-verified on the certified binary): `SharedArrayBuffer`+`Atomics` (1,176 edition failures; recorded product/security decision, S6) and the RegExp `ES6RegExSymbols`/`ES6RegExPrototypeProperties` substrates (558 tag-mapped + 156 signal-classified; S7/S9 recorded flag decisions). All nine ES2025 additions themselves are default-on with probe evidence (manifest §5) |
| 3 | All skips / host applicability justified | **PASS** | 902 skips, exactly the three C1-recorded classes with unchanged counts (828 / 70 / 4), re-verified on the refresh run; no arch/backend skip class exists in this runner (manifest §4) |
| 4 | Preserve **negative-phase validation** | **PASS** | 8,751 marker-judged negatives, 0 displaced/0 forged, **1 downgrade** (the post-ES2025-excluded `import-defer` case — correct behavior); controls incl. the spoof control 25/25 (manifest §3) |
| 5 | Preserve **regression/bytecode/native checks** | **PASS** | Refresh tree: `zig build` rc=0; `zig build check` rc=0 with **2051/0** regression, bytecode up-to-date + pure-verify, native `test-shared-basic` PASS, smoke + metadata PASS; debug-static `check-regression` **2103/0** (manifest §7) |
| 6 | Published **feature/result manifest** | **PASS** | [c4-manifest.md](c4-manifest.md), republished 2026-11-05 with the refreshed buckets, partials and delta ledger; machine artifacts `tools/test262/e12-refresh-analysis.json` + `c4-refresh-delta.json` |

Clause 1's second axis ("declared supported engine configurations")
remains: the manifest covers the Mac no-JIT gates; the Mac JIT corpus
obligations are 17/19 discharged with the fulljit SIGSEGV family carded
([open-cards.md](open-cards.md)), and all Linux claims wait on B4 —
green here would still not authorize a two-configuration claim.

## 2. Why this is not CONDITIONAL

Unchanged from the certification verdict: the red clauses are
substantive, not pending-confirmation — no future CI run turns 2,949
failures green, and no evidence elsewhere can make an off-by-default
feature "enabled in the claimed default". What *is* green — the
qualified measurement itself, now delta-attributed across the whole A–E
program — is exactly what the measurement lanes were scoped to deliver,
hence BLOCKED-with-certified-baseline: C4 has a phase-proven, refreshed
starting point and an exact owed list.

## 3. Exact path to green (re-ranked with the 2026-11-05 numbers)

1. **The three flag decisions — −1,890, the single lever, exactly
   unchanged by A–E.** (a) SAB/Atomics default (**1,176**): the recorded
   product/security decision (S6) must be taken; flag-on conformance is
   ≈895/982 per S6e, so a ~87-scenario flag-on defect tail follows the
   flip. (b) RegExp Symbol protocol (**558**) + prototype properties
   (**156** of the 202 rfm bucket): recorded S7/S9 decisions + the
   `ConfigFlagsList.h:1177` JIT-inlined helper re-check; e8's flag-on
   ceilings (matchAll 50/0, replaceAll 92/0, prototype getters 84/0)
   say these flip nearly clean. Both flips
   additionally owe the per-receipt B4/JIT-corpus obligations. After
   all three: **2,949 → 1,059**.
2. **engine-defect-on-existing 1,010** — a mix of decision cards and
   lane work (fresh decomposition, manifest §2): proper tail calls 34
   (card E11-1 — never implemented, C3-class, V8-parity product
   decision); property escapes 112 (S3 lead-surrogate lacunarity,
   decision-blocked); dynamic-import semantics 68 (cards B1-1…B1-10:
   module early errors, `$DONOTEVALUATE` update-shapes, instn TDZ,
   namespace Set/Delete, eval-gtbndng asserts — module host surface
   owner); Function/prototype 56 (fn own-key order + anonymous `name`,
   carded); Date/prototype 52; TypedArray ctor/internals ~148;
   Array/prototype 40 (incl. E11-2 unshift fast-path 6);
   RegExp/prototype 36; DataView offset-argument 32; JSON/parse 28;
   `AsyncFromSyncIteratorPrototype` 26; for-await-of 24; cross-realm 7
   (e3-R3/R4/R5); the untagged long tail ~405 (assorted semantic gaps).
   The e8-proven substrate families (matchAll 30 / replaceAll 28 /
   match-indices 26) ride item 1, not this one.
3. **host 3** — B1-6 (1: label-before-`import` parser gap) is a bounded
   parser slice; the 2 `Atomics/wait/good-views` hangs are E11-3 —
   SAB-owned (surface with item 1(a)'s flag; the corpus helper's
   blocking `getReport` turns the dead agent into a timeout).
4. **rfm beyond the getter substrate 46** — module string-export names
   10 (B1-8) + misc parse gaps 36, each small enough for S14-adjacent
   slices.

Zero-failure clauses 3–6 need no work; the refresh is now one command
(`refresh_baseline.py`) plus the delta ledger, re-runnable after each
landing decision.

## 4. Scope audit

Certification: commits `becb482b4` (run evidence + analysis) and the
doc commit per [c4-run-evidence.md](c4-run-evidence.md). Refresh:
commit `537e8156a` (delta evidence: `doc/c4-refresh.md`,
`tools/test262/c4-refresh-delta.json`) and this commit (manifest, gate
verdict, plan restart point). Raw run artifacts stay ignored `out/`
content per repo convention; the manifest and refresh receipt quote
every gate-relevant number, and the fresh run's `results.json` is
retained and registered for future per-key joins.
