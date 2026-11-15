# Lane A11 — RegExp lastIndex clamp + post-8.0 case-folding

## A11-1: negative `lastIndex` not ToLength-clamped on exec/test path

Spec: `RegExpBuiltinExec` takes `ToLength(Get(R, "lastIndex"))`;
`ToLength(-1)` is `0`, so matching must start at 0.

Root: `JavascriptRegExp::CacheLastIndex`
(`lib/Runtime/Library/JavascriptRegularExpression.cpp`) used `ToInteger`
and mapped every negative (including `-Infinity`) to the `InvalidValue`
sentinel. `RegexHelper::GetInitialOffset`
(`lib/Runtime/Library/RegexHelper.cpp`) treats that sentinel as beyond-end,
so `exec`/`test` returned null/false and reset `lastIndex` to 0 instead of
matching from 0.

Fix (`JavascriptRegularExpression.cpp`, `CacheLastIndex`): clamp
`NaN || v <= 0.0` (negatives, `-0`, `-Infinity`) to 0; only `+Infinity` and
values above `MaxCharCount` stay `InvalidValue` (beyond-end, spec-identical
outcome: fail + reset to 0). Header state comment in
`JavascriptRegularExpression.h` updated to match. The parallel
`@@match/@@replace/@@split` path via `GetLastIndexProperty` (already
`ToLength`) is untouched, as are fractional/huge/`NaN` behaviors.

Scope note: `CacheLastIndex` is the single integerization point (verified:
no other `ToInteger(lastIndex)` in Runtime/Backend; the JIT inlines only
the cached `lastIndexOrFlag` field, `Lower.cpp:23424`). The legacy
non-global+sticky fast paths (`RegexHelper.cpp` match/replace loops) read
the same cache, so they move from no-match to match-from-0 consistently
with `ToLength`. One fix covers default + forced-native tiers.

## A11-2: u+ignoreCase folding misses post-8.0 pairs

Table `transforms` in `lib/Parser/CaseInsensitive.cpp` cited CaseFolding.txt
8.0.0 and ended at U+118DF (selective 9.0 Warang Citi), missing the three
Unicode 9.0 scripts: Adlam, Osage, Medefaidrin.

Regen search: no generator, script, or documented regeneration process
exists anywhere in-repo for this table (`CaseFolding` appears only in
`lib/Parser/*`, `lib/Runtime/*`, and `doc/c2-s3-v-flag.md`; no tooling or
regen references). Per lane instructions, added the pairs minimally instead
of regenerating, and filed the regen gap as a card below.

Ground truth (fetched UCD, verified contiguous ranges, uniform deltas, all
status C in CaseFolding.txt; cross-checked with Python `unicodedata` 15.0):

| Script      | Upper range       | Lower range       | Delta | CaseFolding.txt endpoints |
|-------------|-------------------|-------------------|-------|---------------------------|
| Osage       | U+104B0–U+104D3   | U+104D8–U+104FB   | 40    | 104B0; C; 104D8 – 104D3; C; 104FB |
| Medefaidrin | U+16E40–U+16E5F   | U+16E60–U+16E7F   | 32    | 16E40; C; 16E60 – 16E5F; C; 16E7F |
| Adlam       | U+1E900–U+1E921   | U+1E922–U+1E943   | 34    | 1E900; C; 1E922 – 1E921; C; 1E943 |

Boundary checks: U+104AF/U+104D4–U+104D7/U+104FC unassigned; U+16E3F
unassigned, U+16E80 digit; U+1E8FF unassigned, U+1E944 mark. Entries marked
`MappingSource::CaseFolding` (recent-addition convention: Deseret, Cherokee,
Old Hungarian, Warang Citi) with exact citations; sorted insertion keeps the
table ascending (required by the linear scan) and satisfies the
compile-time `isTransformEntryValid` static assert. Ranges verified not to
overlap neighbors (Deseret ends 0x1044F, Old Hungarian starts 0x10C80;
Warang Citi ends 0x118DF).

## Verification

Engine: `zig build` (test profile, ARM64 macOS, zig 0.17.0), all builds
and test runs serialized via `/tmp/opencode/cc-gate-lock.sh`.

- Parent repros (`/tmp/a11/repro_lastindex.js`, `/tmp/a11/repro_folds.js`,
  kept out of tree): fixed `ch` output is byte-identical to Node v26.5.0
  in default and forced-native (`-forceNative -off:simpleJit -bgJitDelay:0`)
  tiers. Base binary (stashed fix, rebuilt) reproduces both bugs
  (`exec g -1` null/li 0; Adlam/Osage/Medefaidrin `/iu` false).
- Differential battery (`/tmp/a11/battery.js`: d/s/m flags, lookbehind,
  named groups, `\p{}`/`u`, `v`-mode class, lastIndex exec/sticky/matchAll/
  replace/split/match edges): clean in both tiers except
  `String.prototype.matchAll`, which this engine does not implement
  (pre-existing gap, identical on base, out of lane).
- test262 `test/built-ins/RegExp/**/*.js` (pinned corpus `2e0a5676`,
  `tools/test262/run.py`, default host flags): mine **3036 passed /
  720 failed**, base (same selection, same runner) **3032 / 724**.
  Pass-set diff: **0 regressions**; 4 flips base-fail -> mine-pass, all
  on-point: `RegExp/prototype/exec/S15.10.6.2_A5_T3` and
  `RegExp/prototype/test/S15.10.6.3_A1_T22` (default + strict), which
  assert exactly `ToLength(lastIndex)` start with `lastIndex = -1/-100`.
  The 720 remaining failures are byte-identical on base (missing APIs
  such as matchAll, `Symbol.match`/`matchAll` species paths, property
  descriptor gaps, `Changes_When_Casefolded` data vintage, full-fold Greek
  pairs the 4-slot simple-delta table never supported). Note: the
  reviewer's "2412/2412" matches no count in this corpus revision
  (3756 scenarios / 1879 files here); the operative claim is verified
  instead by the base-vs-fixed pass-set diff above.
- `zig build check`: green (`CHECK_EXIT=0`), full legacy regression
  **Total: passed 2058, failed 0** (`UnifiedRegex: 23/0`), plus bytecode,
  native, smoke and metadata steps. The gate's exact-count pin initially
  failed at 2058 vs pinned 2057/0 — that is the +1 from the new
  `casefold-pairs.js` test (all tests passed). Bumped
  `tools/build/gate_counts.json` to **2058/2110** with a lineage entry,
  per the file's single-source/bump convention (prior single-file lanes
  bump symmetrically; the debug selection was not re-run in this
  worktree, so the 2110 debug pin is conventional, not observed).

## Maintained regression tests

- `test/UnifiedRegex/lastIndex.js` (+14 lines) / `lastIndex.baseline`
  (regenerated): existing `-4`/`-Infinity` exec lines flip from the buggy
  `null`/`r.lastIndex=0` to match-at-1/`lastIndex=2` (ToLength clamp);
  `+Infinity`/huge/`NaN`/fractional lines unchanged. New tail section
  covers `.test()` with negatives (`-1`, `-Infinity`, `-0.5`) on global
  and sticky regexes plus sticky `exec` with `-1`/`-Infinity`.
- `test/UnifiedRegex/casefold-pairs.js` (new) + `.baseline` (new),
  registered in `test/UnifiedRegex/rlexe.xml`: the three pairs, both
  directions, `/iu` literal + class, `/i` (locks in V8-identical `false`
  for astral without `u`), and `[\p{Lu}\p{Ll}]` + doubled-quantifier
  closures. Passes under `runtests.py --test --variants=disable_jit`.

## Card: table-regeneration gap (A11-regen)

No regeneration process exists for the `transforms` table in
`lib/Parser/CaseInsensitive.cpp` — no script, doc, or comment describes
how to rebuild it from UCD, and the file's only provenance note is the
8.0.0 CaseFolding.txt citation. Every post-8.0 script (the 9.0 Warang
Citi one-off, now Adlam/Osage/Medefaidrin) required hand-editing against
fetched UCD with manual delta/range/sort-order reasoning and reliance on
the compile-time `isTransformEntryValid` assert. Recommend: a checked-in
generator (UCD CaseFolding.txt + UnicodeData.txt -> table entries, with
the MappingSource convention documented) plus a CI/vintage probe that
fails when the running Unicode version adds C/S folds outside the table.
Until then, further post-8.0 additions need the same manual treatment
used here.

## Unresolved / out of lane

- `String.prototype.matchAll` missing (pre-existing; identical on base).
- `Changes_When_Casefolded` / `Changes_When_NFKC_Casefolded` property
  escapes and full (multi-char) folds (e.g. `\u0390`/`\u1FD3`) fail on
  base and fixed alike — table vintage/design, separate lane.
- Debug-profile pin 2110 is conventional (symmetric +1); only the test
  profile selection was executed here.
- Disk filled to 100% transiently mid-task (another agent's load;
  recovered to 64G); one test262 `--prepare` validation run died with
  ENOSPC and its residue (`out/test262-results`, mine) was removed.
  Watch disk before large corpus runs.
- Nothing committed, per lane instructions.
