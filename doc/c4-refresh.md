# C4 certified refresh (closing measurement, 2026-11-05)

The `c4-refresh` closing lane of the Mac-side program. It re-measures the
certified baseline after the A–E lanes landed and re-derives the failure
inventory: the run itself is E's validated `refresh_baseline.py` run
([e12-testing-quality.md](e12-testing-quality.md) §1 — same corpus, flags,
procedure and controls as the certification); this lane adds the
**re-derivation, the delta reconciliation and the republished manifest/gate
docs**. No engine source is changed (`git diff HEAD -- lib pal bin` empty
throughout; the measured binary is the e12 rebuild of `4e9f95da7`).

## 1. Run identity (register `refresh-2026-11-05`)

Engine `4e9f95da7` (tree dirty with tool/doc files only; binary built at
HEAD first), `ch` SHA-256 `5e210836…fea7028` (**re-verified byte-identical
to `zig-out/bin/ch` at reconciliation time**), corpus `2e0a56762…`
(unchanged), flags `-nonative -Test262 -CollectGarbage
-NegativePhaseReport`, jobs 8 / timeout 10,000 ms, Node v26.5.0 — every
identity field identical to c4-cert except the engine commit. Controls
**25/25**; skips **902** (828 duplicate-module + 70 HTMLDDA + 4
CanBlockIsFalse — unchanged); negatives **8,751 marker-judged, 0 displaced,
0 forged, 1 downgrade** (the known post-ES2025-excluded `import-defer`
case), 0 shape suspects. Results **retained**
(`out/test262-e12-refresh/results.json`) and registered, so this refresh's
per-key joins are reproducible; c4-cert's per-key results are not (the
113 MB artifact was rotated out — see §3 for how the head stage is
attributed anyway).

## 2. Re-derived totals and buckets

Fresh `analyze.py` + pinned-policy rerun on the retained `results.json`
reproduces the committed
[`e12-refresh-analysis.json`](../tools/test262/e12-refresh-analysis.json)
bucket counts exactly:

| Measure | c4-cert (2026-11-02) | **refresh (2026-11-05)** | delta |
|---|---:|---:|---:|
| broad | 77,268 / 15,873 (82.96%) | **78,399 / 14,742 (84.17%)** | **−1,131** |
| edition-selected | 76,464 / 4,080 (94.93%) | **77,595 / 2,949 (96.34%)** | **−1,131** |
| host | 426 | **3** | −423 |
| engine-defect-on-existing | 1,700 | **1,010** | −690 |
| required-feature-missing | 220 | **202** | −18 |
| disabled-flag | 1,734 | **1,734** | 0 |
| post-ES2025 (excluded) | 11,793 | 11,793 | 0 |

Every delta is an edition-selected flip (broad and edition move by the
same −1,131); the bucket sum 1,734 + 1,010 + 202 + 3 = 2,949 closes.
Bucket composition detail: the rfm 202 = **156** RegExp/prototype getters
(unchanged; the disabled `-ES6RegExPrototypeProperties` substrate) + 46
misc; the host 3 = 1 `decl-pos-import-labeled` URIError (card B1-6) + 2
`Atomics/wait/good-views` SIGKILLs (card E11-3); the disabled-flag 1,734 =
**1,176 SAB-gated + 558 `ES6RegExSymbols`-gated** (both flag splits
recomputed from the fresh records, unchanged).

## 3. Delta reconciliation (machine artifact:
[`c4-refresh-delta.json`](../tools/test262/c4-refresh-delta.json))

Zero regressions everywhere measurable: every lane pair recorded 0
pass→fail, both per-key joins below are 0 pass→fail, and all 35
per-feature tag movements are monotonically down. The ledger splits at the
retained full-corpus midpoint `da0a72dbb` (e7's validation "clean" leg,
`out/test262-e7-v-clean`).

**Stage B — `da0a72dbb` → `4e9f95da7` (per-key over all 93,141 scenarios:
116 fail→pass / 0 pass→fail).** Decomposed per key and family:

| lane (commits) | booked | measured | note |
|---|---:|---:|---|
| e7 early-errors (`b59bcd2b2`,`ed9b4bf78`) | 40 | **40** | own full-corpus join (e7 clean→fixed) |
| b8/e9 BigInt toLocaleString (`6f8f9fe02`) | 42 | **42** | 18 BigInt-tagged + 24 analogs, exact |
| b9/e10 resizable-AB (`d28ba6d57`,`f25e8cb9c`) | 22 | **22** | 20 in-tag + 2 `methods-called-as-functions`, exact |
| b10/e11 integer-limit TypeErrors (`b900d80a1`) | 8 | **8** | 4 ex-timeouts + 2 fast-fail + 2 `S15.4.4.7_A2_T2`, exact |
| C-sweep TA-OOB delete (`3e14b6a16`) | 6 | **4** | sweep estimate measured 4 in-corpus; B8-7 BigInt locale = 0 corpus flips (probe-pinned) |

Buckets: host 7→3 (−4, e11) and engine-defect 1,122→1,010
(**−112 = 40+42+22+4+4, exact**); rfm/disabled-flag unmoved.

**Stage A — `ba796f74f` → `da0a72dbb` (edition −1,015; c4-cert per-key
data not retained, so attribution = lane receipts + their retained
selection pairs + the per-tag join).** B1 loader **473** (its own full
key-join; tag-exact: `dynamic-import` −473; includes the
`import-attributes`-tagged 19→0 inside the dynamic-import selection —
verified on the retained `out/b1-diag` before/after pair — and the
`export-star`-tagged 8→0); e3 cross-realm **132** (its affected-surfaces
"+98 bonus" pair is 96/98 cross-realm-tagged — verified per-key on the
retained `out/test262-b2-affected*` pair — so the set-union is exactly
130 + 2); e4 detach **124** (includes e3-R2's 20 `detached-buffer-realm`;
tag view `cross-realm` −150 = 130 + 20, exact); e5 compound/let-ASI
**112** (14 rfm + 98 engine-defect); e6 class-tail **158** (wide 156/0 +
2 built-ins); the five JIT commits inert (no-JIT run). Booked sum
**999** vs measured **−1,015** → residue **−16**
(−12 engine-defect, −4 rfm): corpus-wide ripple of the landed
semantics/parse fixes outside each lane's measured selection — lanes
measured statements+expressions or built-ins-sensitive selections, not
the whole corpus; the rfm misc fell 64→46 beyond the booked let-ASI 14
while the RegExp-getter substrate stayed exactly 156.

**Reclassification honesty note.** e11 booked a host→disabled-flag
reclassification (+2) for the `Atomics/wait/good-views` hangs. It cannot
materialize in these buckets: `analyze.py`'s first-match rules put
timeout/abnormal-exit (rule 2) ahead of the tag rule (rule 3), so the two
hangs remain in `host` (hence host 3, not e11's predicted 1; disabled-flag
1,734, not 1,736). The work is SAB-owned either way (card E11-3 records
the mechanism); the manifest books them as such.

## 4. Feature-default spot-check (no drift)

`feature-probe.js` on the certified binary: all ES2025 additions still
default-on (BigInt `function`, named groups, lookbehind, property escapes,
`import-attributes` module load, `replaceAll`, `toSorted`, …); the three
flag surfaces still off — `SharedArrayBuffer`/`Atomics` `undefined`,
`RegExp.prototype[Symbol.match]` `undefined`, `RegExp.prototype` getters
`undefined` (which also masks `d`/`v` `.flags` observation). The §5
feature-default audit therefore carries over unchanged; the §5(c)
per-feature partial list is refreshed in the manifest from the fresh
per-feature table (many partials now zero).

## 5. Gates

Final tree (this lane adds docs/artifacts only — no engine, no test-tree
count change; pins stay **2051/2103**):

| gate | result | log |
|---|---|---|
| `zig build check` (test profile) | rc=0 — regression **2051/0**, bytecode up-to-date, native/smoke/metadata PASS | `out/c4r-gate-test.log` |
| `zig build -Dprofile=debug-static check-regression` | rc=0 — **2103/0** | `out/c4r-gate-debug.log` |

## 6. What this closes

The Mac-side program is complete (A–E + this refresh): the certified
edition-selected result is **77,595 / 2,949 of 80,544 (96.34%)** with
every skip justified, every negative marker-judged, every delta attributed
and zero regressions. The C4 verdict remains
**BLOCKED-with-certified-baseline** ([c4-gate.md](c4-gate.md), refreshed):
the largest single lever is unchanged — the three flag decisions are
worth exactly **−1,890** (1,176 + 558 + 156), leaving 1,059 before any
flag-on defect tail. The Linux switch (B4) re-proves this baseline on the
other declared configuration.
