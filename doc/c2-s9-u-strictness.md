# C2-S9: u-mode RegExp strictness

Status: **pending** (review-repair round applied on top; see "Review
repairs" below)

## Scope

Close the pinned `test/language/literals/regexp` u-mode residual (22 scenarios /
11 files) and the adjacent u-mode strictness classes without regressing Annex B
non-u behavior, S3-a (Unicode property escapes) or S3-b (`v` flag):

1. **Extended pattern characters** — `/{/u`, `/a{/u`, `/}/u`, `/a{}/u`,
   `/a{1,2,3}/u` and the bare `]` atom (`/]/u`) reject under u/v; without u
   they stay Annex B literals exactly as before.
2. **Legacy octal / out-of-bounds decimal escapes** — `/\1/u`, `/\8/u`,
   `/\9/u`, `/\123/u`, `/\01/u`, `/\08/u` and the class forms `[\1]`, `[\00]`,
   `[\08]`, `[\9]`, `[\M]` reject under u/v (class identity escapes are
   limited to SyntaxCharacter plus `-` and `/`); valid backreferences
   (including forward references) and `\0`-not-followed-by-a-digit stay
   legal; non-u keeps the legacy octal/identity meanings byte-for-byte.
3. **Quantifiable assertions** — `/.(?=.)?/u`, `/.(?!.)?/u`, `/.(?=.){2,3}/u`,
   `/.(?!.){2,3}/u` (and the lazy `*?` form) reject under u/v; non-u keeps the
   Annex B quantified lookaheads; lookbehinds stay non-quantifiable in every
   mode.
4. **Malformed escape bodies** — `/\u{1,}/u`, `/\u{1F_639}/u`, `/\u{}/u`,
   `/\u{/u`, `/\u{zz}/u`, `/\u12z/u`, `/\uu/u`, `/\xzz/u`, `/\x/u` and the
   class forms reject under u/v; non-u keeps the identity-escape-plus-literals
   Annex B readings.
5. **Surrogate code-point semantics** (semantic fixes, not test-specific
   recognition):
   - inverted astral literal classes (`/^[^❤️]$/u`, `/^[^🧡]/u`,
     `/[^💛]$/u`, `/[^💚]/u`) return `null` for the full excluded astral
     symbol: a NEGATED u-mode class never falls back to the lead unit of a
     complete pair whose code point is excluded (`noHalfPairFallback` on the
     `MatchCodePointSet` instruction);
   - the unanchored search advance moves per code point under u/v, so a
     failed pair position is not retried at its low half (`/[^💚]/u` on
     `"💚x"` resumes at `x`);
   - the u-mode `.`/`\D`/`\S`/`\W` atoms compile to one deterministic
     whole-code-point-set matcher instead of the
     prefix/pair/lone-high-surrogate alternation, removing the ambiguous
     surrogate partitioning that let `/(.+).*\1/u` capture a half of a pair;
   - pass 0's astral `\u{...}` scan consumed only the escape body after the
     helper's cursor rewind, leaving the escape tail to parse as literal
     terms — harmless under the old leniency, fatal under the new `}`/digit
     rules; the scan now consumes the whole escape.

`v` implies the whole u-mode surface, so every deferred rule and runtime
behavior applies under `v` through `unicodeFlagPresent` / the
`UnicodeRegexFlag | UnicodeSetsRegexFlag` program flags.

## Design

Parser (`lib/Parser/RegexParser.{h,cpp}`), all new rejections recorded with the
existing deferred mechanisms in pass 0 so deferred parses (`-force:deferParse`,
`ParseLiteralNoAST`) fail at scan time:

- `TermPass0`: `case '{'` with a non-quantifier body and `case '}'`/`case ']'`
  atoms record `DeferredFailIfUnicode` (the `]` case keeps the existing v-class
  span exemption and adds the same deferred-if-unicode error outside it).
- `TermPass0` tracks `thisTermIsLookahead` for `(?=`/`(?!` terms; a quantifier
  on one records `DeferredFailIfUnicode(JSERR_RegExpBadQuant)` (lookbehinds
  keep the existing hard fail in both modes).
- `TryParseExtendedUnicodeEscape` records `DeferredFailIfUnicode` when
  `\u{`+hex digits is not `}`-terminated; the atom/class `\u` and `\x` cases
  record it when both the extended and the fixed-length hex forms fail.
- Class octal escapes (`ClassEscapePass0`) record `DeferredFailIfUnicode`
  unless the escape is exactly `\0` not followed by a decimal digit; the class
  identity-escape default records `DeferredFailIfUnicodeNoSets` (fires for u
  without v only — the v grammar has its own class-escape policy) unless the
  escaped character is a SyntaxCharacter or `-` or `/`.
- Atom decimal escapes are recorded during pass 0 (value capped at five
  digits, `0`-leading runs and longer runs flagged) and validated once the
  flag and the final group count are known (all three entry points, before
  `ResolveNamedGroupRefs`): under u/v every occurrence must be `\0` or a
  backreference number ≤ the final `numGroups` (forward references legal).
- `AtomEscapePass0`'s surrogate-encountered branch consumes
  `lengthOfSurrogate + 2` (the `\` and `u` the helper rewound over), landing
  the scan after the closing `}` instead of two units short.

Runtime:

- `MatchCodePointSetInst` keeps the engine's pair-then-unit fallback for
  positive sets and property escapes (the pinned property corpus's loop forms
  rely on it; see Residuals) and gains `noHalfPairFallback`, set only for
  NEGATED u-mode character classes (the legacy `CharacterClassPass1`
  complement path and v's pure-code-point negations in `VBuildNode`): at a
  complete pair whose code point is not a member, the atom fails instead of
  matching the lead unit. Lone surrogates at non-pair positions keep
  single-unit membership in every case.
- `Matcher::Match` retry loop: under u/v the next match start advances by the
  length of the code point at the failed start (2 units inside a complete
  pair, else 1); non-u advances per unit exactly as before.
- `GetNodeWithValidCharacterSet` (u-mode `.`/`\D`/`\S`/`\W`): builds the same
  standard-chars set the previous prefix arm used, lifts it to a
  `CharSet<codepoint_t>` plus the astral planes `0x10000..0x10FFFF`, and emits
  `TranslateCodePointSet` (one deterministic `MatchCodePointSetNode` whose set
  contains every astral code point, so complete pairs always resolve to the
  2-unit member interpretation); the non-u branch is byte-identical to before.

### What was tried and rejected

A blanket "complete pair = exactly one interpretation" rewrite of
`MatchCodePointSetInst` (no unit fallback anywhere) is spec-correct but
regressed 69 `property-escapes/generated/*` scenarios: those files' unanchored
`/^\p{...}+$/u` loop forms pass at the base only because the unit fallback
matches both halves of pairs that ICU 79.1 includes in the property but the
newer pinned corpus expects to match — data drift masked by the fallback. The
pinned corpora are the gate, so the strict semantics is scoped to negated
classes (`noHalfPairFallback`), which no corpus scenario depends on, and the
general rewrite is deferred with the Unicode-data decision (S3 residual).
Re-routing all-BMP surrogate-containing sets and lone-surrogate singletons
through the code-point matcher was similarly behavior-neutral-to-risky and
was dropped to keep the diff minimal.

## Ownership / allocation record (doc/memory-management.md)

- The decimal-escape occurrences live in the parser's compile-time arena
  (`ctAllocator`), grow geometrically like `namedGroupRefSpans`, and die with
  the pattern compile; no GC root, tracer, barrier or runtime allocation is
  added or removed.
- The new `CharSet<codepoint_t>` in `GetNodeWithValidCharacterSet` is a
  compile-time-arena set passed to and freed by `TranslateCodePointSet`
  exactly like every class/property set; the lifted standard-chars unit set
  is a stack `CharSet<Char>` freed with `FreeBody` on the single exit path.
- Runtime changes add one bool per `MatchCodePointSet` instruction (fixed
  size, same arena emission) and branch-only logic; no allocation, lifetime
  or ownership change.

## Tests and evidence

- **Focused regression test** `test/es7/regexp-u-strictness.js` (registered
  once in `test/es7/rlexe.xml`, `-args summary -endargs`): 8 groups / ~120
  assertions covering the four named failure classes (`/{/u`, `/\1/u`,
  `/\8/u`, the four quantifiable-assertion shapes, `/\u{1,}/u`,
  `/\u{1F_639}/u`, the four inverted-astral assertions,
  `/(.+).*\1/u`), the adjacent strictness shapes (`/]/u`, `[\M]`, `[\8]`,
  `[\q{a}]/u`, `[\08]`, forward references, lookbehind quantifiers), valid
  controls under u and v, the Annex B non-u preservation battery, and the
  eval/dynamic forms of every rejection. **CH gate runs** (`zig-out/bin/ch`,
  test profile): `pass` on default, `-noDeferParse`, `-CollectGarbage`,
  `-noDeferParse -CollectGarbage` and `-force:deferParse`; the same five
  launches `pass` on `out/debug-static/bin/ch` (asserts active). The file's
  groups also run 8/8 under Node 26.5.0 and Bun 1.4.3 through a shim.
- **Node/Bun oracle battery** (105 eval-source cases, both literal and
  dynamic forms): Node == Bun on 105/105; the final engine == Node on
  **104/105** — the single divergence is the documented pre-existing
  positive-low-surrogate-class lacunarity (below).
- **Pinned test262** (`tools/test262/run.py`, default host flags
  `-nonative -Test262 -CollectGarbage`, 20/20 harness controls in every
  run). Baseline = the pristine base commit built into a self-contained
  directory (`zig-out/bin/s9-base`, launcher + dylib); its totals reproduce
  the recorded S3-a/S3-b gates exactly. Final = the edited build.
  - `test/language/literals/regexp/**` = 480 scenarios:
    **458/22 → 480/0**, +22 fail→pass, 0 pass→fail — the entire pinned
    residual closed.
  - S3-a 1,980-scenario selection (property-escapes, match-indices,
    lookBehind, named-groups, regexp-modifiers, literals/regexp):
    **1,828/152 → 1,862/118**, +34 fail→pass, 0 pass→fail.
  - v universe (296 scenarios: unicodeSets incl. generated/, the
    property-of-strings files and the exec/flags/String v-flag files):
    **286/10 → 286/10**, 0/0.
  - adjacent syntax (1,008 scenarios: `RegExp/*.js` top-level,
    CharacterClassEscapes, dotall): **972/36 → 988/20**, +16 fail→pass
    (exactly `unicode_restricted_{brackets, identity_escape,
    identity_escape_alpha, identity_escape_u, identity_escape_x,
    incomplete_quantifier, octal_escape, quantifiable_assertion}` ×2
    scenarios), 0 pass→fail.
  - annexB RegExp/String/literals (350 scenarios): **284/66 → 286/64**,
    +2 fail→pass (`RegExp/prototype/compile/pattern-string-invalid-u`),
    0 pass→fail.
  - **Total: +74 fail→pass, 0 pass→fail across 4,114 scenarios.**
  - Methodology note: an earlier before/after pair was measured with a
    launcher/dylib mismatch (`ch` loads the sibling dylib, so a copied
    launcher silently picked up the rebuilt library) and produced phantom
    regressions; every number above comes from the self-contained-directory
    comparison.
- **Adjacent gate files**: 17 of 18 legacy/es7 RegExp gate files produce
  byte-identical output on the baseline and final binaries (gate log). Two
  legacy fixtures were repaired in place for patterns that are spec
  SyntaxErrors under u since ES2015 and are now enforced (the S1-wave
  precedent):
  - `test/es6/unicode_regex_surrogate_utf8.js`: four `(\u0061|\142|...)`
    alternations re-spelled `\u0062` (same characters/outputs), and the two
    crash repros' legacy octal escapes (`\2`,`\1`,`\3`) re-spelled as case
    escapes and their `iu`/`gimu` flags reduced to `i`/`gim` — their
    quantified lookaheads (`(?=...)?`, `(?=1){3,}`) are u-mode
    SyntaxErrors; output unchanged.
  - `test/es6/unicode_escape_sequences.js`: the four lenient
    `/a\u{}b/u`- and `/a\u{1.1}b/u`-shaped lines replaced by eval probes
    asserting the SyntaxError; the baseline's four corresponding entries
    flip `false`→`true` (file re-baselined, CRLF format preserved).
- **Gate impact**: `regexp-u-strictness.js` adds one rlexe entry;
  `zig build check-regression` (test profile) reports the expected **+1**
  state — 2025 passed / 0 failed against the 2024 pin — and the
  debug-static profile 2077/0 against the 2076 pin;
  `tools/build/gate_counts.json` intentionally not edited (the integrator's
  single-source bump, per the standing process).
- **Perf spot checks** (test profile, final binary): `.*` over 20k ASCII,
  `.+`/`\D+`/`[^x]*` over 10k astral, `\p{L}+` over 10k astral, an
  unanchored search after 10k astral, and 2,000 `(.+).*\1` backreference
  matches all complete in single-digit-to-tens of ms (recorded in the gate
  log); no scan cliff from the deterministic dot/shorthand atom or the
  code-point advance.
- **Reproducibility**: all builds used `zig build` / `-Dprofile=debug-static`
  behind the shared gate lock (`sh /tmp/opencode/cc-gate-lock.sh`); corpus
  runs are `out/test262-s9-truebase-*` (baseline) and
  `out/test262-s9-final-*` (final), each `summary.json` recording the run
  identity and dylib hash.

## Review repairs (combined review round on `a7b60e94a`)

1. **u-mode rejects v nested-class syntax.** The v-class span exemption for
   `]`/`{`/`}` atoms (the legacy scanner ends nested/`\q` classes early)
   now only applies when the `v` flag is actually present: inside a span
   the atoms record a deferred-if-unicode-WITHOUT-v error, so
   `[[a]]/u`, `[a[b]c]/u`, `[[a]--[b]]/u`, `[^[a]]/u`, `[[ab][cd]]/u`
   reject under plain u while staying legal under v and under non-u
   Annex B (Node 26.5.0 == Bun 1.4.3 on every shape).
2. **v-mode escaped class punctuation.** `VValidateEscape`'s identity
   default is now limited to the ClassSet punctuation set (syntax
   characters, reserved-double-punctuator components, `-`, `^`):
   `[\" ]`, `[\' ]`, `[\ ]`, `[\k]`, `[\3]`, `[\_]` reject under v, while
   `[\&]`, `[\-]`, `[\/]`, `[\!]`, `[\#]`, `[\~]`, `[\{]`, `[\^]`,
   `` [\`] ``, `[a\-c]` and every CharacterEscape stay legal (the full
   printable-ASCII set was probed against Node/Bun; Bun alone accepts
   `[\é]`, Node/spec reject — engine follows Node).
3. **Sticky single-char out-of-bounds read.** The sticky branches of
   `MatchSingleCharCaseSensitive/Insensitive` read `input[offset]` with
   `offset == inputLength` (visible as `/\0/y.exec("")` returning a match
   through the terminating NUL; a sliced string could read its parent's
   next character). Both branches now guard `offset < inputLength`.
4. **Deferred atom identity escapes under u.** `AtomEscapePass0`'s
   identity default records a deferred-if-unicode error for any escaped
   character that is not a SyntaxCharacter, `/`, ZWNJ/ZWJ or one of the
   pseudo-escape letters, so `/\e/u`, `/\-/u`, `/\ /u`, `/\M/u`, `/\q/u`
   (literal, dynamic and `new Function` deferred forms) fail with a clean
   SyntaxError from the no-AST scan instead of surfacing only at the
   reparse; non-u Annex B identity escapes are untouched.
5. **Comment/error nits.** The `MatchCodePointSetInst` comment now states
   the fallback contract exactly: POSITIVE sets AND property escapes of
   both polarities keep the lead-unit fallback (the pinned ICU-drift
   property corpus depends on it); only negated character classes take
   `noHalfPairFallback`. The >`INT16_MAX` capturing-group limit keeps its
   RangeError: raising the cap to the spec's 65535 touches uint16 group
   ids across parser/runtime, and converting only the error type would
   leave the 32768..65535 band failing while Node succeeds — documented
   residual, not changed.
6. **Per-unit advance in single-char fast paths (partial fix).**
   `MatchSingleCharCaseSensitive/Insensitive` scan loops now advance per
   code point under u/v, so trail-surrogate single-char patterns
   (`/\uDE00/u` on `"\uD800\uDE00"`) and plain searches after pairs
   behave per spec. The literal-scanner fast paths (literal prefix skip /
   TextbookBoyer-Moore / octoquad) still advance per UTF-16 unit:
   `/\uDE00X/u` (and alternation/search/split forms built on it, also
   under v) match at the in-pair trail position where Node returns
   null/`["q"]`/no-split. Those loops are the engine's hottest scan path
   and need B4 JIT verification before a pair-aware change — documented
   residual with the exact shapes below.
7. **Lone-surrogate/backref unit-wise lacunarity** remains as previously
   documented; the 69 ICU-drift property scenarios were re-verified green
   (the fallback is untouched).

### Final iteration (`4c2ccf463` repairs, round 2)

1. **v escaped backslash.** `VValidateEscape`'s ClassSet punctuation set
   omitted the backslash itself, so `[\\]/v`, `[a\\]/v`, `[[a]--\\]/v` and
   `[\q{a\\b}]/v` rejected while Node/Bun/spec accept (`\` is a
   ClassSetSyntaxCharacter). Added; the full printable-ASCII escaped-class
   sweep now reads **LEGAL = `!#$%&()*+,-./0:;<=>?@DSW[]^\`bdfnrstvw{|}~\`
   (incl. `\`)** identically on CH, Node 26.5.0 and Bun 1.4.3, and the
   focused test gained an 11-assertion escaped-backslash group (literal,
   dynamic, class, set-op and `\q` forms; the earlier "full printable set
   probed" claim had silently skipped the backslash itself — corrected).
2. **Perf.** See "Perf accounting": the pair-aware advance is now gated on
   trail-surrogate pattern characters only, restoring the original tight
   loops for everything else; final ≤ base within noise on the microbench.
3. **Residual probes recorded** (below): mid-pair manual `lastIndex`
   divergences, negative `lastIndex` TypeError, compile/flags substrate.
   `compile()` itself was re-probed behaviorally (`v→u` property escapes,
   `u→v` nested classes, `compile("{","u")` strictness, plain recompile):
   the final build matches Node on all four (`compile("{","u")` now
   throws like Node; the base accepted it) — the only compile-adjacent
   gap is the default-off `ES6RegExPrototypeProperties` substrate
   (`r.flags`/`r.unicodeSets` are undefined without it), not compile
   logic.

### Canonical v universe (final iteration)


The S3-b receipt's listing **does** reproduce exactly; earlier runs of this
lane mis-transcribed it twice (296 scenarios: missed
`test/built-ins/RegExp/prototype/unicodeSets/**`; 298: also mis-listed the
String `search` v-flag files). The canonical selection is
`test/built-ins/RegExp/unicodeSets/**` (114 files),
`test/built-ins/RegExp/prototype/unicodeSets/**` (38 files),
`test/built-ins/RegExp/property-escapes/generated/strings/**` (28),
`RegExp/prototype/exec/regexp-builtin-exec-v-u-flag.js`,
`RegExp/prototype/flags/this-val-regexp.js`, and the five String
match/matchAll/replace/search(+search v-u) v-flag files — **187 files /
374 scenarios**, exactly the S3-b count. Run on both binaries
(`out/test262-s9-truebase-vcanon` vs `out/test262-s9-final3-vcanon`):
**346 passed / 28 failed on BOTH** — identical totals to the S3-b
receipt's recorded default-flags result, 0 fail→pass / 0 pass→fail. The
28-scenario residue is the deliberately excluded substrate class (18
`unicodeSets` accessor + 10 String v-flag scenarios needing
`-ES6RegExPrototypeProperties -ES6RegExSymbols`, one additionally
`-ESBigInt`), exactly the S3-b decomposition. Combined with the other
corpora (`out/test262-s9-final3-{literals,s3a,adjacent,annexb}` vs
`out/test262-s9-truebase-*`): **total +74 fail→pass, 0 pass→fail over
4,192 scenarios**.

### Artifact and accounting notes

- The `ch` launcher **loads the dylib next to itself**, so a copied
  launcher silently executes whichever dylib currently sits beside it;
  run identity therefore rests on the **dylib** hash recorded in each
  `summary.json` (`binary_sha256` is the launcher and is stable across
  this lane's builds). Every before/after comparison in this receipt uses
  self-contained directories: `zig-out/bin/s9-base/{ch,
  libChakraCore.dylib}` (pristine base) vs `zig-out/bin/{ch,
  libChakraCore.dylib}` (final).
- Result directories per count (all under `out/`):
  - true base: `test262-s9-truebase-{literals,s3a,adjacentsyntax,annexb}`
    and `test262-s9-truebase-vcanon`;
  - final: `test262-s9-final2-{literals,s3a,adjacent,annexb,vuni2}` for
    the repair round and `test262-s9-final3-{vcanon,literals,s3a,adjacent,
    annexb}` for this iteration;
  - the superseded first attempts are `test262-s9-before-*` /
    `test262-s9-after-*` (launcher/dylib mismatch) and
    `test262-s9-final-vuniverse` / `-before2-*` / `-after2-*` (mis-
    transcribed v selections). The v-selection history is: 296 (missing
    prototype/unicodeSets), 298 (same, plus a search-file listing slip),
    374 (canonical).
- The earlier statement that the S3-b listing "does not reproduce" was
  wrong — the omission was this lane's transcription; corrected above.

### Perf accounting

The first pair-aware single-char change added a flag+surrogate branch to
both scan loops (measured on one 20k-ASCII microbench as ~2x relative on
non-u and ~4.3x on u vs the base tight loop — absolute runs were 0-2 ms,
so the ratios reflect loop-body noise, but the branch was real). The
final shape gates the pair-aware advance on the pattern character being a
trail surrogate (`IsSurrogateUpperPart(c)`): a non-surrogate single char
can never match a unit inside a complete pair, and a lead-surrogate
pattern matches at pair starts by the documented lead-lacunarity either
way, so the tight original loop is restored for every other pattern.
Measured (identical microbench, 200x20k scans): base 2/0/10/10 ms and
final 1/1/10/9 ms (non-u / u / non-u-i / u-i), trail-surrogate astral
10k x50: base 4 ms, final 2 ms — final ≤ base within noise on every
line, including `-CollectGarbage`. No perf-regression claim is made
beyond these numbers; B4/JIT re-measurement remains the standing gate.



### Repair-round evidence

- Focused test: `pass` on the test profile and on
  debug-static across default, `-noDeferParse`, `-CollectGarbage`,
  combined and `-force:deferParse` (13 groups at the repair round, 14
  after the final iteration's escaped-backslash group); 14/14 groups
  under Node 26.5.0 and Bun 1.4.3 via the shim.
- Differential batteries (s9-repro2/3/4 + the residual-shape battery,
  normalized): **CH == Node == Bun** on every fixed shape (nested-class
  rejection under u, v punctuation accept/reject over the full printable
  set, sticky-at-end, identity escapes incl. `new Function` deferred
  forms, single-char pair advance); the only CH-vs-Node divergences are
  the documented `\uDE00X`-family literal-scanner lines. Bun alone
  accepts `[\é]` under v (Node/spec reject); engine follows Node.
- Pinned test262 (identical selections before/after; before = the
  pristine-base `s9-base` self-contained binary directory):
  - literals 480 scenarios: **458/22 → 480/0**, +22/0;
  - S3-a selection 1,980: **1,828/152 → 1,862/118**, +34/0;
  - adjacent syntax 1,008: **972/36 → 988/20**, +16/0;
  - annexB 350: **284/66 → 286/64**, +2/0;
  - **total +74 fail→pass, 0 pass→fail over the 3,818 non-v scenarios** —
    identical to the pre-repair state (the repairs cost nothing and the 69
    ICU-drift property scenarios stay green). The v-universe history and
    the final canonical 374-scenario run are recorded in the "canonical v
    universe" note below.
- The 18 legacy/es7 adjacent gate files re-verified: byte-identical on
  base vs final (es6 regex-*/unicode_regex_* and es7 regexp-* incl.
  `regexp-v-flag`); `unicode_escape_sequences` matches its re-baselined
  expectations; lookbehind 15/16 and namedgroups-dup 10/11 are the
  pre-existing base failures (identical on both binaries).
- Perf/GC spot checks unchanged (below).
- Artifact note (general-review nit): the `ch` launcher loads the dylib
  next to it, so every comparison uses self-contained binary directories
  (`zig-out/bin/s9-base` = pristine base launcher+dylib, `zig-out/bin` =
  final); each corpus run's `summary.json` records the exact dylib hash
  it executed.


   A complete surrogate pair whose code point is NOT a member still falls
   back to its lead unit in `MatchCodePointSetInst` (engine-wide since
   S3-a). The spec-critical removal regressed 69 pinned
   `property-escapes/generated/*` scenarios whose loop forms pass only
   because the fallback matches both halves of pairs ICU 79.1 includes in
   the property but the newer pinned corpus expects to match — Unicode
   data drift masked by the fallback. Closing those requires the S3
   Unicode-data decision; only negated classes take the strict rule
   (`noHalfPairFallback`) today.
## Residuals

1. **Pair→unit fallback retained for positive sets and property escapes.**
   A complete surrogate pair whose code point is NOT a member still falls
   back to its lead unit in `MatchCodePointSetInst` (engine-wide since
   S3-a). The spec-critical removal regressed 69 pinned
   `property-escapes/generated/*` scenarios whose loop forms pass only
   because the fallback matches both halves of pairs ICU 79.1 includes in
   the property but the newer pinned corpus expects to match — Unicode
   data drift masked by the fallback. Closing those requires the S3
   Unicode-data decision; only negated classes take the strict rule
   (`noHalfPairFallback`) today.
2. **Positive low-surrogate classes** (`[\udc00-\udfff]`) still match the
   low half at a complete pair (the per-unit scan-start lacunarity; 1
   oracle-battery divergence line vs Node, pre-existing and unchanged).
   The single-character form `/\uD800/u` over a complete pair matches the
   lead half on the engine and returns null on Node (lead-lacunarity,
   pre-existing, kept by design this round).
3. **Literal-scanner fast paths advance per UTF-16 unit under u/v** (the
   prefix-skip / TextbookBoyer-Moore / octoquad programs). Exact divergent
   shapes (engine vs Node): `/\uDE00X/u.exec("\uD800\uDE00X")` →
   `["\uDE00X"]` at index 1 vs `null`; `/\uDE00X|q/u` on the same input →
   `["\uDE00X"]` vs `["q"]`; `"\uD800\uDE00Xz".split(/\uDE00X/u)` → 2
   parts vs 1; the same first shape under `v`. A trail surrogate as the
   FIRST literal unit is the trigger; single-char (since this lane),
   class-atom and main-program paths are pair-aware. B4 follow-up: the
   scan loops are the hottest interpreter path and need JIT verification
   before a pair-aware advance.
4. **Mid-pair manual `lastIndex` (global u)** — pre-existing on the base
   binary, unchanged: `/\uDC00/gu` with `lastIndex = 1` on
   `"\uD800\uDC00"` matches at index 1 (`["\uDC00"]`) where Node returns
   null (over-match at an in-pair start); `/\u{10400}/gu` with
   `lastIndex = 1` on two pairs matches at index 2 (advances to the next
   pair start) where Node reports index 0 (snap-back restart).
5. **Negative `lastIndex`** — pre-existing on the base binary: with
   `lastIndex = -1`, `/a/g.exec("ba")` ends with `lastIndex` 0 on the
   engine vs 2 on Node (spec: ToLength clamps to 0, then the match at 1
   advances lastIndex to 2); `lastIndex = -5` throws a TypeError on the
   engine vs a normal match on Node.
6. **Substrate gaps (not selection omissions)**: `RegExp.prototype.flags`
   / `unicodeSets` reads (also after `compile()`) require the
   default-off `ES6RegExPrototypeProperties` substrate; `matchAll` and
   the String v-flag scenarios in the canonical v universe require
   `-ES6RegExSymbols` (plus the two pre-existing matchAll empty-match
   scenarios); `hasIndices` surface parity is corpus-pinned under the
   same substrate. `compile()` semantics themselves were re-probed and
   match Node.
7. **Capturing-group cap**: the engine rejects more than `INT16_MAX`
   (32767) capturing groups with a RangeError; Node accepts up to 65535
   and then throws SyntaxError. Raising the cap touches uint16 group ids
   across parser/runtime — left documented.
8. **matchAll empty-match advance** (S3-b residual), the `new
   RegExp("\u{...}").source` decoded-pair normalization quirk, and the
   pre-existing literal-scanner rejection of file-level `/[a[b]/`-shaped
   literals (dynamic forms parse) are untouched.
9. JIT/B4: all evidence is Mac no-JIT interpreter (below).

## JIT (B4)

All evidence is Mac no-JIT interpreter. No JS bytecode or regex instruction
layout change beyond one appended bool inside `MatchCodePointSet`; B4 must
still run the focused test and the RegExp corpora under SimpleJIT/FullJIT
before any JIT-configuration claim, per the standing C2 policy.
