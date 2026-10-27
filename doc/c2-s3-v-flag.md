# C2-S3-b: RegExp `v` flag (Unicode sets)

Status: **pending review** — implementation plus the combined
general+adversarial review repairs (second commit on the branch, on top
of `7bd66a59d`; branch `s3-v-flag`, base `0a5006399`); focused test green on
both profiles and all launch variants; pinned corpus and adjacent controls
below.

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
    and `-force:deferParse` launches — re-verified after the review
    repairs on the final binary.
  - **Debug-static** (`out/debug-static/bin/ch`, asserts active): `pass`
    on the same five launches (final repaired binary).
  - **Node/Bun oracle battery** (not the gate harness): the same file's
    assertions executed under Node 26.5.0 and Bun 1.4.3 through a
    compatibility shim — 463 assertions. After the review repairs the
    engine passes 463/463; Node differs on 6 and Bun on 10, every one of
    them a documented oracle divergence (below), not an engine defect.
- **Pinned test262** (`tools/test262/run.py`, default host flags
  `-nonative -Test262 -CollectGarbage`; 20/20 harness controls in each
  run): the **exact 187-file v-flag universe** (`test/built-ins/RegExp/
  unicodeSets/**` incl. `generated/` (114 files) and the prototype
  directory, `property-escapes/generated/strings/**` (28),
  `RegExp/prototype/exec/regexp-builtin-exec-v-u-flag.js`,
  `RegExp/prototype/flags/this-val-regexp.js`, and the five
  `String/prototype/{match,matchAll,replace,search}` v-flag files) =
  **374 scenarios (187 files × 2 modes)**, re-run after the review
  repairs (`out/test262-s3b2-*`; identical totals to the first
  iteration):
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
  `language/literals/regexp/**`), re-run after the review repairs:
  **1,828 passed / 152 failed** — identical totals and residue
  decomposition to the first iteration (112 generated data drift,
  12 double-escaped literals, 6 named-groups symbols substrate, 22
  u-literal leniency) and vs the S3-a final 1,814/166 **+14 fail→pass,
  0 pass→fail** (the 14 = `generated/strings/` scenarios deferred to this
  slice). The 13 adjacent legacy/es7 gate files also re-verified
  byte-identical on the repaired binary.
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

## Review repairs (combined general+adversarial review, second iteration)

1. **Negated shorthand classes never terminated** (`[\S]`, `[\D]`, `[\W]`
   and their i/union/negation/`--`/`&&` combinations): the char16 range
   cursor in the `\d`-family copy loop wrapped at 0xFFFF (`hi + 1`
   truncated) so the loop restarted forever. Fixed by stopping after a
   range that ends at 0xFFFF. Perf ladder: `[\S]+`/`[\D]+`/`[\W]+` over
   20k inputs now 0-3 ms (was: no return).
2. **Character escapes wrongly rejected as `--` right operands**
   (`[a--\t]`, `[\s--\n]`, `[a--\0]`, `\x09`, `\u0009`, `\u{9}`,
   `[a--\b]`, `\q`-left forms): the pass-0 `--` softening consumed only
   the first dash, so the second opened a bogus range against the escape
   and hard-failed. The softening now consumes both dashes and clears the
   pending item; a second softening at the legacy range-resolution fail
   site covers set-valued left operands (`[\s--\n]` shape: a range
   starting at literal `-` with a smaller end is invalid in every legacy
   mode, so deferring is behavior-preserving for non-v). Range left
   operands (`[a-c--\t]`) stay rejected, and `[]`-legacy shapes
   (`[--a]` without flags) stay accepted.
3. **Escaped surrogate pair range endpoints**
   (`[\uD83D\uDE00-\uD83D\uDE01]`): singleton escape decoding in
   `VParseItem`/`VParseOperand` now combines an escaped high surrogate
   with a following escaped low surrogate into one astral code point
   (`VDecodeEscapeCodePoint`), so astral ranges, negations and set
   operations over them work; astral range-order rejection preserved.
4. **Kelvin sign in `\q` strings under `i`**
   (`[\q{k}]/iv` on U+212A must be null): v string members now use the
   spec Canonicalize rule (simple uppercase; non-ASCII never canonicalizes
   into ASCII) instead of case-fold closure. Multi-code-point members
   match through **one new fixed-size instruction**
   `MatchLiteralCanonicalInst` (opcode appended after `MatchCodePointSet`
   — no earlier tag moves; literal bytes ride the unchanged literal-buffer
   transfer, so the ownership record is exactly MatchLiteralInst's; the
   ICU `u_toupper` call is a const table lookup on the calling thread).
   Single-code-point members join the code point set through their
   canonical equivalence (`VAddCanonEquivalenceCodePoints`). Canonical
   literals opt out of every scan/prefix-skip candidate and publish a
   sound non-exact firstSet. Literal-run accumulation
   (`AccumLiteral`/`FinalTerm`) preserves the canonical flag and never
   merges canonical with ordinary runs.
5. **v+i set operations now close operands before `&&`/`--`**: every
   ordinary operand (chars, ranges, `\d`&co, `\p`, nested classes) is
   fold-closed at insertion (`VCloseOperandIfInsensitive`), so
   `[k--[k-k]]/iv`, `[\q{k}--\p{Ll}]/iv`, `[\q{k}--[k-k]]/iv`,
   `[\q{k}--[\u{17F}-\u{17F}]]/iv` and the nested-class form compute over
   closed operands; `\q` members keep the canonical equivalence (no
   over-fold). `VBuildNode` no longer re-closes (that would re-admit
   U+212A). No-`i`, singleton-vs-singleton and ordinary `[k-k]/iv`
   behavior unchanged (class chars keep the engine's fold closure:
   `[k]/iv` still matches U+212A).
6. **Truncated nested class + operator** (`[[a]--`, `&&` variant):
   `VReadRawCodePoint` and the operand/item entry points guard EOF and
   fail with a clean `SyntaxError`; verified on debug-static (asserts
   active) across a 10-case truncation ladder.
7. **Deep nested v classes** (`"[".repeat(N)+"a"+"]".repeat(N)`):
   pass 0/1 class recursion now probes the stack
   (`PROBE_STACK_NO_DISPOSE`, the standing parser mechanism) and the
   per-level `VCSet`s moved from C++ stack frames into the compile-time
   arena, so nesting compiles cleanly within the stack budget and raises
   the engine's standard out-of-stack error beyond it — no SIGSEGV.
   Measured: test profile OK through 8000 levels, clean error at 16000;
   debug-static OK through 3000, clean error at 4000 (larger debug
   frames). Node accepts 4000 and rejects at 16000 (its own parser
   limit); Bun accepts 16000.

### Receipt-only items from the review

- The ~50x scan cliff for `[\p{RGI_Emoji}]gv` (44 ms vs Node/Bun ~1 ms per
  5k-char scan) is the inherited literal-alternation prefilter gap: the
  property's ~2770 string arms are tried per position because none of the
  engines' literal scan/prefilter instructions apply to an
  `AltNode` of v string arms. Correctness is unaffected; bounded by the
  corpus (each scenario completes quickly).
- Deferred-reparse residual examples corrected: the pass-1-only rejections
  are malformed `[\x]`/`[\u]` escape shapes (not range-order examples;
  range order is checked in pass 1 only for *valid-shaped* classes, which
  the no-AST scan accepts structurally).
- u/v leniency notes completed: `]` as an Annex B atom outside classes,
  `\q` outside classes (invalid escape under v, identity under Annex B),
  `\1`-`\9` outside classes (Annex B), malformed `{`-quantifier leniency.
- Node/Bun Kelvin claims corrected: Bun (JSC) *over*-folds (U+212A and
  U+017F match `k`/`s` members — 7 battery divergences); Node (V8) is
  asymmetric on single-character `\q` members under `i` (matches the raw
  pattern character only: `[\q{K}]/iv` on `"K"` is null) and folds
  U+017F inside multi-char members — 6 battery divergences. This engine
  implements the spec's symmetric Canonicalize for both.
- Duplicate `\q` string members (repeated alternatives, overlapping
  property-of-strings sets) are now deduplicated in `VBuildNode` after
  the descending-length sort (adjacent equal members are unobservable),
  bounded by the sort itself.

## Residuals

1. Oracle divergences on `\q` under `i` (all documented, engine implements
   the spec): Bun (JSC) over-folds U+212A/U+017F into `k`/`s` members and
   accepts `[\q{\d}]`/`[\q{\w}]`; Node (V8) is asymmetric on
   single-character members (raw-pattern-character matching: `[\q{K}]/iv`
   on `"K"` is null) and folds U+017F inside multi-char members; this
   engine (and the ES spec) canonicalize both sides symmetrically. Six
   Node-side and ten Bun-side assertions in the differential battery are
   these divergences.
    these divergences. Also: Bun accepts `[a[b]--[c]]` (union as
    difference left operand) and Node rejects `[\q{\d}]` — both pinned
    as Node-behavior assertions in the focused test.
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
