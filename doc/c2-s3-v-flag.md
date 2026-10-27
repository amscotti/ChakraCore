# C2-S3-b: RegExp `v` flag (Unicode sets)

Status: **pending review** — implementation complete on branch `s3-v-flag`
(worktree `ChakraCore-wt/s3-v-flag`, base `0a5006399`); focused test green on
both profiles and all launch variants; pinned corpus and adjacent controls
recorded below.

## Scope

The ES2024 Unicode Sets mode (`v` flag) on top of S3-a, using Node 26.5.0
as the behavior oracle (Bun 1.4.3 agreed on every probed outcome except the
two divergences recorded under Residuals):

- Flag surface: `v` parses (gated by `IsES6UnicodeExtensionsEnabled` like
  `u`), `u`+`v` in either order is a `SyntaxError`, duplicates rejected,
  `v` implies the whole u-mode surface (surrogate pairs, escapes,
  restricted identity escapes, no Annex B relaxations). `v` is reflected in
  `flags` (canonical `dgimsuvy` order), `source`, `toString`,
  `RegExp(regexp)` cloning and the four flag-string builders.
  `RegExp.prototype.unicodeSets` accessor with the standard descriptor
  (`get unicodeSets`, length 0, non-enumerable, configurable, undefined
  setter), installed under the `ES6RegExPrototypeProperties` substrate and
  the unicode config gate, with a dynamic property record like `hasIndices`
  (no PropertyId or built-in-bytecode change).
- v character-class grammar (validated in pass 0 with deferred-if-v
  errors, compiled in pass 1): nested classes, union (juxtaposition),
  intersection `&&`, difference `--` (single-operand chains of one operator
  kind, left-associative; ranges and multi-item unions are not chain
  operands), `ClassSetSyntaxCharacter` escaping (`( ) [ ] { } - / |`), the
  reserved-double-punctuator rule (two adjacent *unescaped* occurrences of
  `& ! # $ % * + , . : ; < = > ? @ \` ~ ^`, including inside `\q` bodies
  and nested classes, except the `&&`/`--` operators), class ranges
  (singleton endpoints only), negation, and the negation early error using
  the spec's conservative *MayContainStrings* (difference keeps the left
  side's flag; intersection is the conjunction).
- `\q{...}` string alternatives: `|`-separated literal strings of escaped
  or raw characters (`( ) [ ] { } / | -` must be escaped; `\d`-style,
  octal and backreference escapes rejected), empty alternatives allowed
  (zero-width members), single-code-point strings folded into the code
  point set, escaped `\uD800\uDC00` pairs combining to one code point.
- The seven ES2024 properties of strings (`Basic_Emoji`,
  `Emoji_Keycap_Sequence`, `RGI_Emoji_Modifier_Sequence`,
  `RGI_Emoji_Flag_Sequence`, `RGI_Emoji_Tag_Sequence`,
  `RGI_Emoji_ZWJ_Sequence`, `RGI_Emoji`) resolved through ICU 79.1's
  `uset_applyPropertyAlias` with an empty value (probed: 79.1 = Unicode
  18.0 carries all seven with up to 2770 strings). **No Unicode data is
  copied into the engine.** Under plain `u` the seven names are
  `SyntaxError` (deferred-if-unicode-without-v); `\P{...}` of one is a
  `SyntaxError` under every unicode mode.
- Set operations over the (code points, strings) pair: code-point parts
  through range arithmetic over `CharSet<codepoint_t>`, string parts
  through exact comparison; under `i`, strings are canonicalized per code
  point at insertion (the same equivalence-class machinery the runtime
  uses for u-mode literals/backreferences) so operations compare canonical
  forms; code-point parts keep the existing closure-then-complement order
  for negation.
- Matching semantics: a v class compiles to an alternation of its string
  members sorted by descending code-point length (literal arms), then the
  code-point set (existing `TranslateCodePointSet` translation:
  `MatchCharNode`/`MatchSetNode`/`MatchCodePointSetNode`), then a
  zero-width success arm when the empty string is a member. This is V8's
  architecture (alternation of literal arms), reuses every existing
  matcher node, and adds **no new AST node or runtime instruction**.

## Not implemented / out of scope

- v-mode search still advances per UTF-16 unit like the engine's u mode
  (the S3-a "per-unit lacunarity": a class containing lone-surrogate
  members can match a surrogate half inside a pair; V8 advances per code
  point). Pre-existing engine-wide behavior, inherited by v unchanged.
- The v-class range-order and a few malformed-escape checks are pass-1
  failures only (see Residuals).

## Implementation

- `lib/Parser/RegexFlags.h`: `UnicodeSetsRegexFlag = 1 << 7`.
- `lib/Parser/RegexUnicodeProperty.h`: `StringProperty` kind with the seven
  names and `BuildStringPropertySet` (ranges into the `CharSet`, strings
  through a `StringCollector` callback; string items longer than 24 units
  or any ICU failure reject the pattern rather than truncate).
- `lib/Parser/RegexParser.{h,cpp}`:
  - `Options()` gains the `v` case and the mutual-exclusion checks; the
    three entry points set `unicodeSetsFlagPresent` and treat
    `unicodeFlagPresent` as `u || v`, and fire two new deferred errors:
    deferred-if-v (v-class grammar violations) and
    deferred-if-unicode-without-v (properties of strings under plain `u`).
  - **Pass 0**: `TermPass0`'s `'['` case runs `VClassPass0`, a pure
    validation scan of the v grammar that records deferred-if-v errors and
    restores the cursor, before the untouched legacy `CharacterClassPass0`.
    The legacy scan (and therefore surrogate tracking, group counting and
    the literal extent) is byte-identical; any pattern where the v span
    would swallow a `(` is itself a v-mode syntax error, so the deferred
    error fires before pass 1 can mis-account groups. One legacy
    softening: when a class item `<singleton>` is followed by `--` *and
    the legacy range `<singleton>..'-' would be invalid* (start above
    `-`), pass 0 consumes the first dash as a literal and defers the
    range error for non-unicode and plain-u modes; valid ranges like
    `[+--]`/`u` keep their legacy meaning (Node-verified u/v
    disagreement), and v's pass 1 re-parses the text with the operator
    grammar either way.
  - **Pass 1**: `TermPass1`'s `'['` case dispatches to `VClassPass1` under
    `v`, which parses the class (operands, ranges, nested classes, `\q`,
    property escapes incl. properties of strings, `\d`&co) into a
    `VCSet {code points, strings, MayContainStrings}`, applies
    `--`/`&&`/union/negation, and builds the alternation node; it leaves
    the top-level `]` for `TermPass1` exactly like the legacy
    `CharacterClassPass1` contract (nested classes consume their own).
    Raw surrogate pairs in class text are consumed directly as code points,
    and stale pass-0 tracker entries inside the class span are skipped so
    later atoms still pair correctly. Atom-position property-of-strings
    escapes go through the same builder in `ParsePropertyEscapePass1`.
    Set bodies use the existing `Clear`-then-`CloneFrom` discipline (a
    `FreeBody`-then-`CloneFrom` sequence would re-free dangling plane
    pointers; the debug build's asserts and a dedicated crash repro
    verified the repaired pattern).
  - `v`-mode atom identity escapes deliberately reuse the existing u-mode
    policy (syntax characters and `/`), which matches V8's v behavior
    exactly on every probed character; class-position identity escapes
    reject word characters like u, plus the v-only octal rejection.
- Runtime plumbing: `RegexPattern::IsUnicodeSets`, `GetCaseMappingSource`,
  code-point-wise backreference matching and lookbehind frame reversal all
  treat `u` and `v` alike; `RegexHelper`'s dynamic flags parser and opts
  builder, `EntryGetterFlags`, `StringFromFlags` and `GetOptions` all
  append `v` in canonical position; `unicodeSets` accessor registered via
  `JavascriptLibrary`'s dynamic property records and the built-in function
  list.

### Ownership / allocation record (doc/memory-management.md)

- All v-class parse state (`VCSet` bodies, string buffers, scratch arrays,
  `\q` alternative decoding, the ICU-collected strings) lives in the
  parser's compile-time arena (`ctAllocator`) exactly like the existing
  class sets and property sets; string members are copied into the shared
  literal buffer and become ordinary `MatchLiteralNode` literals whose
  run-time copies the unchanged transfer passes create in the program's
  literal buffer. No new GC root, tracer, barrier or recycler allocation;
  nothing outlives the pattern compile.
- ICU: `BuildStringPropertySet` opens a `USet`, applies the property
  alias, copies ranges/strings out, and `uset_close`s it on one
  straight-line path (the same no-leak shape S3-a uses); measured worst
  case `RGI_Emoji` (79 ranges + 2770 strings) ≈ 3.4 ms on this machine,
  once per escape occurrence per compile, on the calling script thread.
  All ICU calls are const table lookups or local objects; rental-threading
  rules untouched.

## Tests and evidence

- **Focused regression test** `test/es7/regexp-v-flag.js` (registered once
  in `test/es7/rlexe.xml`, default launch, `-args summary -endargs`):
  17 test groups / 364 assertions covering the flag surface (parse, u+v
  rejection, duplicates, canonical `dgimsvy` order, source/toString/
  constructor round-trips; the `unicodeSets` descriptor group runs whenever
  the prototype-properties substrate installs the accessor), the full
  accept/reject battery (reserved double punctuators incl. escaped-pair
  leniency and `\q` bodies, syntax-character escaping, set-operation
  operand rules incl. ranges/unions, negation early error with the exact
  MayContainStrings truth table, properties of strings under `u`/`v`/no
  flags, the `X--` u/v disagreement battery), and semantics (union,
  nested classes, ranges, difference/intersection over code points and
  strings, left-associativity, `\q` ordering/backtracking/zero-width
  members, ICU property-of-strings data, negation, case-insensitive
  canonicalization incl. set operations, astral members, lone surrogates,
  captures/backreferences/lookbehind/split/replace/sticky/hasIndices/
  modifiers composition, dynamic construction and `compile()`), plus the
  u/non-u preservation group (identity escapes, octals, Annex B identity +
  literals, `[a&&b]/u` literal union).
  - **CH gate runs** (`zig-out/bin/ch`, test profile): `pass` on default,
    `-noDeferParse`, `-CollectGarbage`, `-noDeferParse -CollectGarbage`
    and `-force:deferParse` launches — re-verified on the final binary
    after the last parser repair.
  - **Debug-static** (`out/debug-static/bin/ch`, asserts active): `pass`
    on the same five launches (final binary).
  - **Node/Bun oracle battery** (not the gate harness): the same file's
    assertions executed under Node 26.5.0 and Bun 1.4.3 through a
    compatibility shim — **364/364 each except** three assertions where
    Bun (JSC) itself diverges from Node/V8 (below); i.e. the file is
    Node-exact and the engine matches Node.
- **Pinned test262** (`tools/test262/run.py`, default host flags
  `-nonative -Test262 -CollectGarbage`; 20/20 harness controls in each
  run): the **exact 187-file v-flag universe** (`test/built-ins/RegExp/
  unicodeSets/**` incl. `generated/` (114 files) and the prototype
  directory, `property-escapes/generated/strings/**` (28),
  `RegExp/prototype/exec/regexp-builtin-exec-v-u-flag.js`,
  `RegExp/prototype/flags/this-val-regexp.js`, and the five
  `String/prototype/{match,matchAll,replace,search}` v-flag files) =
  **374 scenarios (187 files × 2 modes)**:
  - default flags: **346 passed / 28 failed** — the 28 are exactly the
    accessor/substrate class (18 `unicodeSets` prototype scenarios +
    10 String.prototype v-flag scenarios needing the symbols substrate,
    with `this-val-non-obj` additionally needing `-ESBigInt` for its `4n`
    literal);
  - `-ES6RegExPrototypeProperties -ES6RegExSymbols -ESBigInt`: **372/2**;
    the 2 are `matchAll/regexp-prototype-matchAll-v-u-flag.js` "Empty
    matches with u flag" — the engine's pre-existing per-unit
    empty-match advance under matchAll (base-identical on the same u-mode
    assertions; the file only became reachable because `/v/` now parses);
  - **Node oracle**: the same universe passes under Node 26.5.0 with a
    shim harness (114/114 + 28/28 + the behavior files), i.e. the pinned
    corpus's Unicode-17-generated expectations agree with ICU 79.1
    (Unicode 18.0) and with Node on every v-flag scenario.
- **S3-a bounded regression selection** (the S3-a receipt's exact
  1,980-scenario universe: `property-escapes/**`, `match-indices/**`,
  `lookBehind/**`, `named-groups/**`, `regexp-modifiers/**`,
  `language/literals/regexp/**`): **1,828 passed / 152 failed** vs the
  S3-a final 1,814/166 = **+14 fail→pass, 0 pass→fail**; the 14 are
  exactly the `generated/strings/` scenarios S3-a deferred to this slice,
  and the remaining 152 decompose identically to the S3-a residue (112
  generated data drift, 12 double-escaped literals, 6 named-groups
  symbols substrate, 22 u-literal leniency).
- **Adjacent legacy/es7 controls**: 13 gate files — es6 `regex-set`,
  `regex-annex-b`, `regex-case-folding`, `regex-quantifiers`,
  `regex-code-point`; es7 `regexp-modifiers`, `regexp-lookbehind`,
  `namedgroups`, `namedgroups-symbols`, `namedgroups-dup`,
  `regexp-indices`, `regexp-indices-symbols`, `regexp-property-escapes` —
  produce **byte-identical output on this binary and the S3-a base
  binary** (verified on the final binary; `regex-set` caught and then
  verified the `[+--]` u-range preservation repair).
- **Gate impact**: `regexp-v-flag.js` adds one rlexe entry;
  `zig build check-regression` (test profile) reports **Total: passed
  2024, failed 0** and the debug-static profile reports **Total: passed
  2076, failed 0** — in both cases the only failure is the pin mismatch
  (`require passed 2023/2075`), exactly the expected +1 per profile
  (2023 → 2024, 2075 → 2076), with zero test failures.
  `tools/build/gate_counts.json` intentionally not edited (the
  integrator's single-source bump, per the standing process).
- **Reproducibility**: all builds used `zig build` /
  `-Dprofile=debug-static` behind the shared gate lock; corpus runs are
  in `out/test262-s3b-v-default2`, `-v-proto`, `-v-flagson`,
  `-s3a-regr` (each `summary.json` records the run identity).

## Residuals

1. Node/Bun divergences (implementation follows Node/V8, which matches the
   spec on each probed point): Bun accepts `[\q{\d}]`/`[\q{\w}]` (class
   escapes inside `\q`), Bun does not canonicalize strings before set
   operations (`[\q{ab}--[\q{AB}]]/iv` matches under Bun, not under
   Node/spec), Bun accepts `[a[b]--[c]]` (union as difference left
   operand), and Bun folds KELVIN SIGN into `[\q{k}]/iv`. The first three
   are pinned by the focused test as Node-behavior assertions.
2. v-mode inherits the engine's documented u-mode divergences: per-unit
   search advance (lone-surrogate lacunarity — a class containing a lone
   surrogate member can match a surrogate half inside a pair), the
   `{`-as-literal leniency, malformed `\x`/`\u` fallbacks, and octal
   escapes *outside* classes; plus the u-mode class identity-escape
   leniency (`[\q{a}]/u` parses as the identity escape 'q' plus literals;
   Node rejects under u — pre-existing, preserved).
3. v-class range-order (`[\u{10FFFF}-a]`) and a few malformed-escape
   shapes are rejected in pass 1 only, so a deferred-parse no-AST scan can
   miss them (they would surface as an internal error on the later
   reparse). The pinned corpus has no such scenarios.
4. The matchAll flag-on residue above: `String.prototype.matchAll` with
   u/v regexps advances per UTF-16 unit on empty matches (pre-existing
   symbols-substrate behavior, unchanged; base-identical).
5. The generated corpus is Unicode-17 data on an ICU 79.1 (Unicode 18.0)
   build: every v-flag scenario agrees today (verified against Node), but
   future Unicode drift is bounded by the pinned dependency (the B5
   decision), exactly as for S3-a's generated files.

## JIT (B4)

All evidence is Mac no-JIT interpreter. The slice adds no JS bytecode and
no regex instruction; B4 must still run the focused test, the v corpus and
the adjacent RegExp corpora under SimpleJIT/FullJIT (dynapogo) before any
JIT-configuration claim, per the standing C2 policy.
