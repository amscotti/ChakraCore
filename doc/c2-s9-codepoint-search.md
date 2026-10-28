# C2-S9-b: code-point-aware literal search under `u`/`v`

Status: **pending** (implemented and fully evidenced on Mac no-JIT; awaiting
review/integration; includes the review-repair round: the `v`
full-Unicode matchAll/split/match/replace fix and receipt corrections).
Gate pins unchanged and re-verified on the committed tree: `zig build check`
green on both profiles — `check-regression` **2026/0** (test) and **2078/0**
(debug-static), plus bytecode/native/smoke/metadata legs — all through the
shared gate lock (`sh /tmp/opencode/cc-gate-lock.sh`); logs preserved under
`out/gate-receipt/`.

Closes the S9-b literal-scanner surrogate residual: under `u`/`v` a search
fast path could start (or a walk could land) on the second half of a
complete surrogate pair, and a manual mid-pair `lastIndex` behaved
per-unit instead of resolving to the containing code point.

## Scope trigger (shapes fixed)

- `/\uDE00X/u.exec("\uD83D\uDE00X")` matched at index 1; Node/Bun: `null`.
- `/[\uDE00x]a/u`, `/a|\uDE00/u`, `"\u{1F600}".search(/\uDE00/u)`, global/
  sticky walks and `split` could start on the low half of a valid pair
  through the literal/prefix/set-scan fast paths.
- `/\uDC00/gu` with `lastIndex = 1` matched at 1 (Node `null`);
  `/\u{10400}/gu` with `lastIndex = 1` returned `null` where Node/Bun match
  the pair at index 0 (spec pair-splitting `lastIndex`).

## Review repair: fullUnicode covers `v` (blocking hang)

The S9-b pair-start snap exposed a latent `v` bug in the flag-string walks:
the ES6 symbol drivers computed fullUnicode from the `flags`/`unicode`
properties alone, which are **false/absent for a `v`-only regex**
(`(/a/v).unicode === false`; ES2026 22.2.6.8/22.2.6.11/22.2.7.2 require
"u" **or** "v"). An empty-capable `v` pattern then ping-ponged forever: the
walk's empty-match advance stepped +1 into a pair and the snap moved the
next `exec` back to the pair start.

**Iteration 1** treated `'v'` in the matchAll flag scan and read
`unicode`/`unicodeSets` properties elsewhere — still wrong: without
`-ES6RegExPrototypeProperties` those accessors are not installed at all, so
a *subclassed* `v` regex (exactly the case the observable ES6 drivers exist
for) still hung in `String.match`/`replace`/`replaceAll`, and `@@split`
stringified `undefined` and threw a SyntaxError building its splitter.

**Iteration 2 (final shape):** `@@match`, `@@replace`, `@@split` and
`@@matchAll` all derive their loop controls the spec way — ONE observable
`Get(R, "flags")` + `ToString` (`JavascriptRegExp::
GetFlagsStringForBuiltin`), parsed by `ParseBuiltinFlags`
(`global` = 'g'; fullUnicode = 'u' **or 'v'**). When the value is
`undefined` because the `ES6RegExPrototypeProperties` substrate did not
install the accessor and no user accessor/own value provides it, a genuine
RegExp instance falls back to its compiled internal flags — the same
deviation `@@matchAll` and `String.prototype.replaceAll` already made. A
user-provided observable value (lying, object-with-ToString, or throwing)
is never ignored, and the read sequence is spec-shaped: exactly one
`flags` read per builtin, never per iteration; `global`/`unicode`/
`unicodeSets` are not consulted (probe-verified under
`-ES6RegExSymbols -ES6RegExPrototypeProperties` with recording accessors,
CH == Node). The now-dead property readers
(`GetGlobalProperty`/`GetUnicodeProperty`/`GetFullUnicodeProperty`) were
removed. Plain regexes still take the default ES5 fast paths (nothing
changed for them); non-`v` behavior is unchanged.

## Spec basis

ES2026 22.2.7.2 (`RegExpBuiltinExec`) step 11 makes the u/v matcher input a
code-point sequence (`StringToCodePoints`); step 13.b resolves the attempt
start ("the index into input of the character that was obtained from
element `lastIndex`") to the **containing** code point, so an offset that
splits a complete surrogate pair starts at the pair start; failures advance
by `AdvanceStringIndex` (22.2.7.3: +code-unit-count, i.e. +2 over a pair).
22.2.7.4 `GetStringIndex` maps match ends back to code units. Verified
behaviorally against Node 26.5.0 and Bun 1.4.3 (they agree on every probe
below): the snap-back applies to global **and** sticky (`/\u{10400}/yu`
with `lastIndex = 1` matches at 0 and leaves `lastIndex` 2), and to empty
patterns (`/()/gu` mid-pair `lastIndex` yields an empty match at the pair
start with `lastIndex` 0).

## Design

One invariant everywhere: **a u/v match attempt never starts at a position
whose previous unit is a lead surrogate and whose unit is a trail surrogate**
("pair-splitting position"); non-u keeps the Annex B per-unit behavior
byte-for-byte. All sites are in `lib/Parser/RegexRuntime.cpp` (engine) and
`lib/Runtime/Library/RegexHelper.cpp` (walk drivers):

1. **Attempt start — `Matcher::Match`**: if `u|v` and the incoming `offset`
   splits a pair, `offset--` (the spec's containing-code-point resolution).
   This is the single choke point for every top-level attempt (exec, test,
   search, and every internal walk), and it is what makes both the
   `/\uDC00/gu` (fails after snapping; Node `null`) and `/\u{10400}/gu`
   (matches the pair at 0) manual-`lastIndex` shapes Node-exact.
   `isUnicodeMode` is cached on the `Matcher` at construction so hot paths
   read one field, not `program->flags`.
2. **Sync scan instructions** (`SyncToChar/Char2Set/Set...AndContinue/
   Consume/Backup`): the original tight per-unit loops are unchanged. After
   a stop, under u/v an out-of-line stop check (`_NOINLINE
   ScanStopCheck{Char,Char2Set,Set}`) tests whether the stop position splits
   a pair (pattern-gated first: only a trail-surrogate stop unit can split,
   so char/char2-set checks test the pattern character first and cost one
   compare) and resumes the same tight scan past the trail half
   (`ScanTo*PairAwareResume`). Correct for arbitrary (including negated)
   stop-sets: the resume never skips a boundary stop, and pair-splitting
   candidates are ≥2 units apart, so rescans stay linear. Set scans have no
   pattern-side gate; every u-mode stop pays two loads.
3. **Literal scanners** (`ScannerMixinT`, `EquivScannerMixinT`,
   `Char2LiteralScannerMixin`, and the multi-literal
   `SyncToLiteralsAndBackup` via `MatchLiteralSkipPairSplits`): after a
   Boyer-Moore/char2 hit, gate on the first pattern unit (the 4-unit
   equivalence class for case-insensitive scans) being a trail surrogate and,
   only then, on `isUnicodeMode`; a splitting candidate is skipped and the
   scanner re-run from the next unit (`RescanLiteralSkipPairSplits`, out of
   line). Non-trailing literals pay one compare per scan invocation.
4. **Backup repositioning**: the `SyncTo*AndBackup` instructions reposition
   `matchStart` to `found − backup.upper`, which can be pair-splitting; the
   repositioned start snaps to the pair start (the main retry advances per
   code point from there). This is what fixes `/[\uDE00x]a/u` (the literal
   `'a'` backup previously re-anchored the set scan inside the pair).
5. **Walk drivers** (`RegexHelper.cpp`): after a successful match the next
   start is `RegexNextMatchStart`: empty matches advance by
   `AdvanceStringIndex` (pair-aware); an end that splits a pair with
   `end − 1 == start` (the engine's documented one-lead-unit lacunarity
   match) steps past the trail half so the walk cannot ping-pong through the
   pair-start snap. ES5 `match`, both `replace` drivers (string and
   function), and `split`'s empty-match advance use it; the empty-match
   replace path appends the whole stepped code point. `RegexHelper::
   AdvanceStringIndex` now implements the real 22.2.7.3 (the previous +1
   TODO stub — this is the S7/S3-b-noted empty-match-iteration residual for
   `@@match`/`@@replace`/matchAll under `-ES6RegExSymbols`).
6. **exec/test `lastIndex` propagation**: `RegexAdjustedLastMatchForPropagation`
   steps the regex's `lastIndex` write past the trail half for that same
   one-lead-unit lacunarity match, so JS-visible exec loops (the ES6
   `@@match`/`@@replace`/matchAll drivers) always strictly advance. The
   result array and the RegExp constructor keep the unadjusted match; only
   the regex's `lastIndex` differs, and only in the already-Node-divergent
   lacunarity case.

## Ownership / memory notes (doc/memory-management.md)

No allocation, ownership, GC-boundary or completion-path change: the edits
are offset arithmetic inside existing scan loops and driver walks. The one
new Matcher field (`isUnicodeMode`, bool) is set at Matcher construction
from the immutable program flags. The `_NOINLINE` helpers add no frames on
hot paths (branch-not-taken). The `Char2LiteralScannerMixin::Scan` split is
a private static member so it keeps the mixin's `Matcher` friendship for
profiling stats. No instruction encoding, opcode set or bytecode layout
change (the regex program is untouched; B4 scope is unchanged from the S9
lane plus ordinary re-verification).

## What is deliberately preserved

- **Lone surrogates**: a lone trail/lead at a code-point boundary still
  matches (`/\uDE00/u` on `"\uD800\uDC00\uDE00"` → index 2; `[\uDC00-\uDFFF]`
  on `"a\uDC00"` → index 1). Skipping only ever removes starts strictly
  inside a *complete* pair.
- **Lead-half lacunarity** (S9 residual 2's second half, kept by design):
  positive sets/classes/single chars containing lead units still match the
  lead half of a complete pair at the pair start (pinned S3-a
  property-escapes corpus depends on the sibling fallback;
  `doc/c2-s9-u-strictness.md` residuals 1–2). `/\uD800/u.exec("\uD800\uDC00")`
  still returns `"\uD800"` where Node returns `null`. The snap in
  `Matcher::Match` can now surface this lacunarity at a pair start that a
  mid-pair `lastIndex` previously skipped (4 of 4,000 differential fuzz
  scenarios, e.g. `/\u{10400}|\u{1F600}|\uD800/giu` with `lastIndex = 1`
  matches the lead half at 0 where Node returns `null`); this is the
  documented-divergence family, not a new kind of divergence.
- **Trail-half set-scan lacunarity closed as a side effect**:
  `/[\uDC00-\uDFFF]/u.exec("\uD800\uDC00")` now returns `null` (Node-exact;
  was the S9 receipt's residual 2 first shape).
- **Non-u Annex B**: every gate is `isUnicodeMode`-guarded; all non-u
  loops, starts, walks and split/replace advances are the original code.

## Evidence

All binaries are self-contained launcher+dylib directories
(`/tmp/opencode/s9-base-bin` = pristine `ec01ad6ac`, `zig-out/bin` = final;
`ch` loads the sibling dylib, so run identity rests on the dylib — base
`f206c595…`, final per build). Builds: `zig build -Dprofile=test` and
`-Dprofile=debug-static` (Zig 0.17.0, macOS ARM64 no-JIT, asserts active in
debug-static).

- **Focused test** `test/es7/regexp-u-strictness.js` (same rlexe entry, no
  new file; gate counts unchanged): extended 14 → **21** groups — six S9-b
  groups (trail-literal scans incl. literal/char2/TBM/dynamic/set-scan/
  alternation first-set with `i`/`v` mirrors and non-u Annex B pins, pair
  literals, manual mid-pair `lastIndex` with boundary controls, global/sticky
  walks and search/split/replace/exec-walk, anchors and alternation, lone
  surrogates) plus the repair round's `v`-mode empty-capable walk group
  (split/match/replace/replaceAll + the exec-level snap contract; the
  `-ES6RegExSymbols` matchAll forms are pinned in `test/es7/matchall.js`,
  extended 22 → **24 groups** under its existing flag-enabled rlexe entry:
  the matchAll hang group, and the iteration-2 subclass group — subclass v
  match/replace/replaceAll/split empty-capable termination + exact results
  with u/no-flag controls, one-flags-read-per-builtin recording accessors,
  own/lying/ToString-object/throwing flags values).
  **21/21 and 24/24 pass** on test and debug-static across default,
  `-noDeferParse`, `-CollectGarbage`, combined and `-force:deferParse`
  (matchall.js always with `-ES6RegExSymbols`); 21/21 and 24/24 under Node
  26.5.0 and Bun 1.4.3 via a WScript shim (`out/s9-probe/shim.js`).
- **Repair-round differential batteries** (`out/s9-probe/vfix.js` 25 shapes;
  `out/s9-probe/vfix2.js` 21 shapes; `out/s9-probe/reads.js` read-sequence
  probes; mirrored in Node first): v/u/no-flag empty-capable matchAll,
  match, replace, replaceAll and split walks (including every hang shape),
  subclass instances, done/count, `d`-flag indices, non-global throws,
  manual mid-pair `lastIndex`, direct `Symbol.matchAll`, and the flags
  observability matrix (own value overrides, object ToString, throwing
  accessor propagates, exactly one `flags` read per builtin with
  `global`/`unicode`/`unicodeSets` never consulted): **CH == Node on every
  shape** under both `-ES6RegExSymbols` and
  `-ES6RegExSymbols -ES6RegExPrototypeProperties`; the default-mode
  split/match/replace subset is also identical. A lying observable
  `"g"`-only flags value on an empty-capable `v` regex loops in the spec
  algorithm itself — Node OOMs on it; that spec-inherent shape is not
  pinned.
- **Differential batteries** (60-assertion group battery
  `out/s9-probe/groups.js` + probe scripts): CH == Node == Bun on every
  fixed shape; the only remaining CH-vs-Node divergences are the kept
  lead-lacunarity family and default-off substrate gaps (matchAll needs
  `-ES6RegExSymbols`).
- **Deterministic fuzz differential** (4,000 scenarios: random 1–3-atom
  u/v/i/g/y patterns × surrogate-heavy/ASCII inputs × exec/split/replace/
  search/test/walk, seeded): base vs final — 385 scenarios changed, **249
  now exactly Node-equal**, 4 flip to the kept lead-lacunarity divergence
  (snap exposes a pair start, above), 132 diverge from Node on both
  binaries (all in the kept lead/trail-lacunarity family; zero outside it).
  No non-u parse/semantics changes (the one Node SyntaxError delta,
  `new RegExp("\\u{10400}*|…")` non-u, is byte-identical on base).
- **Pinned test262** (`tools/test262/run.py`, revision
  `2e0a56762801e275a9fdf96dc49d90ba0cddcf63`, default host flags), base
  (pristine `ec01ad6ac`) vs final over identical named selections
  (`out/t262-s9b-{base,final}-*`), **0 pass→fail in every selection**:
  - literals (`test/language/literals/regexp/**`), 480 scenarios:
    **480/0 → 480/0**;
  - S3-a selection (`test/built-ins/RegExp/{property-escapes,match-indices,
    lookBehind,named-groups,regexp-modifiers}/**` + the literals glob),
    1,980: **1,862/118 → 1,862/118** (identical to the S9 receipt pin, so
    the 69 ICU-drift property scenarios and the pair→unit fallback are
    untouched);
  - v canonical (`test/built-ins/RegExp/{unicodeSets,prototype/unicodeSets,
    property-escapes/generated/strings}/**`, `prototype/exec/regexp-builtin-
    exec-v-u-flag.js`, `prototype/flags/this-val-regexp.js`, and the five
    String v-flag files `String/prototype/{match,matchAll,replace}/regexp-
    prototype-*-v-u-flag.js` + `search/regexp-prototype-search-v{,-u}-flag.js`),
    374: **346/28 → 346/28** (the 28-scenario substrate class unchanged);
  - adjacent syntax (`test/built-ins/RegExp/*.js`,
    `CharacterClassEscapes/**`, `dotall/**`), 1,008: **988/20 → 988/20**;
  - annexB (`test/annexB/built-ins/RegExp/**`, annexB String split/replace,
    annexB literals/regexp), 140: **76/64 → 76/64**;
  - v superset (the v-canonical globs with the **entire**
    `String/prototype/{match,matchAll,replace,search}` directories), 710:
    **614/96 → 614/96**.
  These selections deliberately overlap (literals ⊂ S3-a; v canonical ⊂ v
  superset; `property-escapes/generated/strings` ⊂ S3-a's property-escapes),
  so no de-duplicated grand total is claimed — the per-selection totals
  above are the exact counts.
- **Adjacent legacy gate files**: every `test/es6/*regex*`,
  `test/es7/regexp-*`/`matchall` output is byte-identical base vs final
  except the deliberately extended `regexp-u-strictness.js` (and
  `matchall.js` in this repair round, whose new groups fail only their new
  assertions on the base binary). `test/es6/unicode_regex_surrogate_utf8.js`
  — which the S9 receipt had re-spelled for u-mode SyntaxErrors after two
  legacy crash repros — runs clean on the committed binary: exit 0, output
  content-identical to its re-baselined `.baseline` (CRLF-only byte
  difference, as the harness normalizes); no crash artifact exists on this
  tree.
- **Hang/termination probes** (`out/s9-probe/probe3.js` under
  `-ES6RegExSymbols`): lead-class global match/replace/split/matchAll and
  empty-match walks over pairs all terminate with Node-order results where
  the model agrees (the pre-fix binary either matched mid-pair or, with a
  naive snap, would have ping-ponged; the walk guards in items 5–6 are the
  termination argument, exercised by these probes).

### Perf accounting

Microbenches (`out/s9-probe/perf*.js`, best-of-N wall time, 90 KB
surrogate-heavy / 620 KB ASCII inputs; base vs final vs Node for context):

- Single searches are unchanged: `/quick brown fox/u` 71→71 ms,
  non-u TBM 71→71, `\d`-style scans 0→0 ms.
- Match-count-heavy walks pay the per-match boundary bookkeeping
  (snap + stop-check + `RegexNextMatchStart`): `/ab/g` 6→9 ms, `/ab/gu`
  6→12 ms, `/[cd]/g` 17→21 ms, `/[cd]/gu` 17→28 ms (100 × 10k-match walks;
  Node pays 10/10/17/21 ms for the same four, so the final binary is at or
  under Node on the u walks and within 1 ms on non-u `/ab/g`; `/[cd]/g`
  21 vs Node 17). ASCII `replace(/abc/g…)` 14→16/14→18 ms. Empty-match u
  walks (`/x?/gu` over 90 KB of pairs) 13→13 ms. The first implementation
  (inline pair-aware loops in the instruction bodies) measured ~2× on all
  walks; moving every u/v branch out of line and gating pattern-side first
  recovered the scan paths entirely and left only the per-match costs
  above. No claim beyond these numbers.
- **Linear alternation-first-set shape** (`out/s9-probe/perf5.js`): a full
  miss of `/q|\uDE00/u` over 90 KB of surrogate-heavy text costs 13 ms on
  the final binary vs 0 ms on base — but base's 0 ms is the **wrong-answer
  fast path** (it matched the trail half mid-pair immediately); the final
  scan is linear and at Node parity (Node: 11 ms for the same miss, and
  Node also pays ~13 ms for the ASCII miss where CH's non-trail control
  stays 0 ms). The no-trail control `/q|z/u` is 0→0 ms on CH (identical
  base vs final), and the ASCII miss `/q|\uDE00/u` is 14→14 ms (surrogate
  checks never fire on ASCII input).

## Residuals

1. **Lead-half lacunarity kept** (S9 residuals 1–2, S3-a corpus
   dependency): positive sets/property escapes fall back to the lead unit
   at a pair start, and single lead chars match a pair's first half;
   Node/Bun return `null`. The S9-b snap can surface this at pair starts a
   mid-pair `lastIndex` used to skip (4/4,000 fuzz scenarios, recorded
   above). Closing it needs the S3 Unicode-data decision.
2. **u-mode set-scan stop check** costs two loads per stop with no
   pattern-side gate (sets cannot be cheaply probed for trail membership);
   measured above (`/[cd]/gu`). A compile-time stop-set trail-membership
   bit inside `SetMixin` would remove it at the cost of an instruction
   layout change — deferred with B4.
3. **Perf deltas on match-count-heavy walks** (+3–11 ms per 100 × 10k-match
   microbench walks, scans unchanged) and the linear alternation-first-set
   miss shape (above) — final ≤ Node on the u forms, within ~1 ms of Node
   on non-u `/ab/g`; absolute deltas vs base are documented above.
4. **Failed non-global/non-sticky exec/test resets `lastIndex` to 0**
   (`/a/u` with `lastIndex = 5` on a failing input leaves 0; Node keeps 5).
   Cause: the flag-off `PropagateLastMatchToRegex` resets unconditionally
   on a failed match instead of only for global/sticky (ES2026 22.2.7.2
   step 13.a). Not regressed by S9-b (identical on base), left documented.
5. **`test()` cache makes the reset profile-dependent**: a *repeated*
   failing `test()` on a non-global/non-sticky regex hits the
   `RegExpTestCache` — the release/test profile returns the cached result
   early and **keeps** `lastIndex` (accidentally spec-shaped), while
   debug-static re-runs the match for validation and resets it to 0
   (`re.lastIndex = 7; re.test(s)` → 7 on test, 0 on debug-static; Node
   keeps 7 in both). Debug-only revalidation in `RegexEs5TestImpl`; left
   documented.
6. **Negative/−Infinity `lastIndex`**: ToLength should clamp to 0 and match
   from there — `/a/g` with `lastIndex = -1` or `-Infinity` on `"ba"`
   returns `null` with `lastIndex` reset to 0 on CH, while Node matches at
   1 and leaves `lastIndex` 2 (the S9 receipt's older "throws TypeError on
   -5" shape no longer reproduces after the S7 ToLength work; current
   behavior is the null+reset shape above, identical on base). GetInitial
   offset handling of negative values; left documented.
7. **`split` limit does not count capture groups**:
   `"aXbXc".split(/(X)/u, 2)` returns `["a","X","b"]` (3 elements — the
   limit check runs before the captures are appended, and captures are
   appended beyond it) where Node returns exactly `["a","X"]`;
   `limit = 3` coincides. Same for `v`. ES5 split driver limit checks;
   pre-existing on base, left documented.
8. **matchAll/`r.flags`/`unicodeSets` substrate** gaps are unchanged
   (default-off `ES6RegExSymbols`/`ES6RegExPrototypeProperties`); the
   `AdvanceStringIndex` +1 TODO from the S7/S3-b receipts is now fixed for
   u **and v** regexes (empty-match iteration advances per code point),
   observable only under those flags.
9. The `INT16_MAX` group cap remains as in the S9 receipt (untouched).
10. **Legacy instance flag properties without the substrate**: with
    `-ES6RegExSymbols` but not `-ES6RegExPrototypeProperties`, RegExp
    instances carry non-configurable own `global`/`unicode`-style
    properties, so `Object.defineProperty(r, "global", ...)` throws a
    TypeError where Node (accessor prototype) allows it - pre-existing on
    the base binary and unrelated to the flags derivation (the iteration-2
    read-sequence probes run under
    `-ES6RegExSymbols -ES6RegExPrototypeProperties`, where the accessors
    exist, and CH == Node there). Left documented.
11. **Lying observable flags on empty-capable v regexes**: an observable
    `flags` value of `"g"` on an empty-capable `v` regex makes the spec
    algorithm itself loop (the +1 advance fights the builtin exec's
    code-point snap) - Node OOMs on the shape; the engine follows the same
    spec-shaped derivation and does not special-case it. Not pinned.

## Gate receipts (committed tree)

Both profiles rebuilt from the committed tree and run through the shared
gate lock; raw logs preserved under `out/gate-receipt/`:

- `zig build check -Dprofile=test`: **exit 0**, `check-regression`
  **2026 passed / 0 failed** (pin 2026), bytecode/native/smoke/metadata
  legs green.
- `zig build check -Dprofile=debug-static`: **exit 0**, `check-regression`
  **2078 passed / 0 failed** (pin 2078), static native 8/8 suites PASS,
  bytecode/smoke/metadata legs green.

## JIT (B4)

All evidence is Mac no-JIT interpreter. No opcode, instruction-encoding or
bytecode change (one bool field on the C++ `Matcher`, one private static on
`Char2LiteralScannerMixin`, one `JavascriptRegExp` static); B4 must still
run the focused tests (21 + 24 groups) and the RegExp corpora above under
SimpleJIT/FullJIT before any JIT-configuration claim, per the standing C2
policy, and re-measure the walk microbenches (the per-match bookkeeping is
in interpreter C++ only, so JIT regex callers should be unaffected, but
that is unverified).
