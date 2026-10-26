# C2-S3-a: Unicode property escapes under the `u` flag

Status: **pending** (implemented on branch `s3-property-escapes`, base
`084dcf06a`; evidence below; awaiting integration review and the gate-count
bump the integrator owns).

## Scope

`\p{...}` / `\P{...}` Unicode property escapes in RegExp patterns **under the
existing `u` flag** (ES2018 feature, current-spec semantics):

- Binary properties with canonical names and aliases (`Alphabetic`/`Alpha`,
  `White_Space`/`WSpace`/`space`, `ID_Start`/`IDS`, ..., plus the
  RegExp-specific `Any`, `Assigned`, `ASCII`).
- Non-binary `Property=Value` forms limited to `General_Category`/`gc`,
  `Script`/`sc`, `Script_Extensions`/`scx`, with canonical and alias values
  (`gc=Lu`/`gc=Uppercase_Letter`, `sc=Greek`/`sc=Grek`/`sc=Qaai`,
  `scx=Katakana`, grouped `gc=L`/`gc=Letter`/`gc=LC`/`gc=Other` and legacy
  aliases `cntrl`/`digit`/`punct`).
- Lone values resolve as binary property, then General_Category value.
  **Lone script values are rejected** (current spec removed them; Node 26.5.0
  and Bun 1.4.3 agree, and the pinned corpus's `Script_-_*.js` only asserts
  `Script=`/`sc=` forms).
- **Exact, case-sensitive name matching** (ES2024 removed loose matching):
  the pinned corpus's `loose-matching-01..14` are all negative tests
  (`\p{ANY}`, `\p{ lowercase }`, `\p{gC=uppercase_letter}` → SyntaxError).
- `\P` negation, lone `\p`/`\P`, empty/`=`-only/double-`=`/unterminated
  bodies, unknown properties and values, values under the wrong property
  (`gc=Greek`, `sc=Lu`), non-ES properties (`Block=`, `Age=`, `Hyphen`,
  `Other_*`, `Grapheme_Link`, binary-name=value) → `SyntaxError` under `u`.
- Outside `u`, existing Annex B behavior is **preserved exactly**: `\p`/`\P`
  are identity escapes and `{...}` re-parses as literal characters
  (`/\p{Lu}/.test('p{Lu}')` stays true). One deliberate divergence from
  V8/JSC kept: they reject an unterminated `\p{...` even without `u`; we keep
  accepting it as literals because this slice pins existing non-u behavior.
  (`[\p{Lu]` without `u` parses as the literal characters `{Lu` in Chakra — base and this change — and in Node/Bun alike; only a class with no closing `]` at all is a SyntaxError everywhere)

**Out of scope** (later S3 slices): the `v` flag (still rejected as an
invalid flag), `--`/`&&`/nested-class set notation, and property-of-strings
forms (`\p{RGI_Emoji}`, `Basic_Emoji`, ...). The pinned corpus's
`property-escapes/generated/strings/` directory (14 scenarios) keeps failing
on that basis.

## Implementation

- `lib/Parser/RegexUnicodeProperty.h` (new, header-only, consumed only by
  `RegexParser.cpp`; deliberately **no new translation unit** so the static
  archive member pin (526) and build tables are untouched): the ECMAScript
  name allow-lists (binary spellings; `General_Category`/`gc`,
  `Script`/`sc`, `Script_Extensions`/`scx`; grouped-category masks and the
  legacy gc aliases `cntrl`/`digit`/`punct`; the two script aliases `Qaai`/
  `Qaac` ICU does not expose by enumeration) and code-point-set construction
  through the **ICU 79.1 C APIs** (`uset_applyIntPropertyValue` for
  binary/gc/script values and gc group masks, `uset_applyPropertyAlias` for
  `Script_Extensions` and the `Assigned`/`Any`/`ASCII` special IDs,
  `uset_getItemCount`/`uset_getItem` to copy ranges). **No Unicode property
  data is copied into the engine.** gc and script values resolve by exact
  comparison against `u_getPropertyValueName` long/short spellings, so the
  accepted spellings track the ICU build.
- `lib/Parser/RegexParser.{h,cpp}`:
  - **Pass 0** (`AtomEscapePass0` new `p`/`P` branch; `ClassEscapePass0` new
    cases): `\p`/`\P` not followed by `{` records a deferred-if-unicode
    invalid-escape error; `\p{...}` bodies are scanned to the closing `}`
    (stopping at an unescaped `/` in literals so the literal's real end stays
    reachable), names validated inline, and malformed/unknown-name cases
    recorded as deferred-if-unicode errors at the escape's start. The class
    context shares the tail scanner (`ScanPropertyEscapeBodyPass0`) because
    its switch has already consumed the `p`/`P`. Unterminated bodies rewind
    the cursor just past `p`/`P` so the non-unicode re-parse in both passes
    sees the same input.
  - **Flags known** (all three entry points — `ParseDynamic`, `ParseLiteral`,
    `ParseLiteralNoAST`): the existing deferred-error mechanism raises the
    SyntaxError under `u`; the no-AST literal scan therefore reports bad
    property escapes as early errors exactly like named groups.
  - **Pass 1** (`AtomEscapePass1` new branch under `u`;
    `CharacterClassPass1` new hook under `u`): `ParsePropertyEscapePass1`
    (atom) and `UnionPropertyEscapePass1` (class member) re-derive the body,
    resolve it, and compute a `CharSet<codepoint_t>` via ICU. Without `u`
    nothing changes: `p`/`P` fall through to the existing identity-escape
    switch and the braces re-parse as literals. In classes the escape is a
    set-valued atom: `[A-\p{Lu}]` and `[\p{Lu}-Z]` are SyntaxError under `u`
    through the same `JSERR_UnicodeRegExpRangeContainsCharClass` path as
    `\d`; trailing/leading `-` stay literal.
  - `\P{...}` complements over all code points **first**; the effective `i`
    state (top-level or scoped by ES2025 modifiers) then applies the
    engine's case-equivalence closure to the complemented set — so
    `/\P{Lu}/iu` matches `'A'` — while the class path `[^\p{Lu}]` keeps the
    class machinery's closure-then-complement order (`[^\p{Lu}]/iu` does not
    match `'A'`); both verified against Node 26.5.0/Bun 1.4.3.
  - `TranslateCodePointSet` is the translation tail factored out of
    `CharacterClassPass1` and shared with the atom path: singleton →
    `MatchCharNode` or surrogate-pair atom; all-BMP → one `MatchSetNode`;
    otherwise BMP prefix (+ lone-surrogate suffix) plus surrogate-range
    disjunction arms. No new AST node kinds, no new instructions, no
    bytecode or PropertyId changes; property escapes reuse the existing
    set/alternation machinery, so lookbehind mirroring, scoped modifiers,
    quantifiers, capture/backreference and `compile()` interplay come for
    free.
  - The moved code now complements an **empty** u-mode class body before
    translation, so `/[^]/u` matches any code point as the spec requires;
    the previous behavior (never matching anything) was a pre-existing
    corner (`[]` is unchanged). This is the only adjacent behavior change
    and is Node-verified (`'abc'.replace(/[^]/gu,'x') === 'xxx'`).

### Ownership / allocation (doc/memory-management.md record)

- Parse-time nodes/sets live in the parser's compile-time arena
  (`ctAllocator`) exactly as class sets do today; the runtime copies are
  made by the unchanged node transfer passes. No new GC root, tracer,
  barrier or recycler allocation is introduced, and no object outlives the
  pattern compile.
- ICU resources: `BuildCodePointSet` opens a `USet` (ICU heap), copies its
  ranges into the arena `CharSet<codepoint_t>`, and `uset_close`s it before
  returning; open and close sit on one straight-line path, so no failure
  path can leak. Measured worst case (the 735-range `Assigned` set) is
  ~35 µs open+apply+copy+close (ICU probe), once per escape occurrence per
  pattern compile, on the calling script thread; all ICU APIs used are
  const table lookups or local objects, so rental-threading/TLS rules are
  untouched.
- Name buffers are fixed 64-char stack arrays (`propertyEscapeMaxNameLen`,
  longest real spelling is 28); longer bodies are simply invalid names, so
  scanning is bounded and cannot be driven to overflow or unbounded
  allocation. The review's failure-path checklist: every early exit in the
  property paths either throws `ParseError` (same mechanism as all regex
  syntax errors), records a deferred error, or returns a node; no partial
  state survives (arena allocation is transactional by design).

## Review repairs (combined general+adversarial review, commit after `b9bb103c3`)

1. **Class `\p`/`\P` without a body** (blocking): `ClassEscapePass0`
   called the shared body scanner unconditionally for `p`/`P`, so
   `[\p]`/`[\pL]`/`[\P]` hit the scanner's `{` assertion (debug) or
   misparsed (release). Both passes now mirror the atom-path guards:
   pass 0 records a deferred-if-unicode invalid-escape error and keeps the
   Annex B identity escape (`singleton = 'p'/'P'`), and the pass-1 class
   hook additionally requires the `{` before engaging (its remaining shape
   assert became a defensive `Fail`). Non-u class forms are byte-identical
   to the base binary again; u-mode forms are SyntaxError (20-probe
   battery: ch == Node == Bun; non-u rows == base).
2. **Script-value over-acceptance**: `sc=`/`scx=` accepted ISO codes with
   no assigned code points in the ICU build (`Hanb`, `Kpel`, `Loma`, and
   34 more enumerated spellings). Resolution now rejects any script value
   whose `UCHAR_SCRIPT` USet is empty — a data-derived check, no name
   list — applied to `Script` and `Script_Extensions` (and the `Qaai`/
   `Qaac` aliases). The 70-probe battery matches Node/Bun exactly
   (all three engines reject all 35 codes for both properties);
   assigned scripts incl. `Unknown`/`Zzzz`/`Qaai` stay accepted.
3. **ICU failure handling**: `BuildCodePointSet` now returns success;
   `U_FAILURE` at apply time or mid-iteration propagates to the parser's
   existing `Fail(JSERR_RegExpSyntax)` path in both the atom and class
   callers, so an ICU error can no longer silently produce an empty or
   truncated set. The canonical-name buffer for `Script_Extensions`
   detects overflow instead of truncating (a truncated name could
   silently resolve to a different script). No ICU object is retained
   across calls (USet open/close stay on one straight-line path).
4. **Receipt corrections**: the false `[\p{Lu]`-without-`u` SyntaxError
   claim is corrected (that form parses as literal `{Lu` in Chakra, Node
   and Bun alike; only a class missing its `]` errors, everywhere); exact
   Unicode version labels are replaced by observed drift traits; the
   focused-test counts are exact (16 groups / 365 assertions) and CH gate
   runs are distinguished from the separate Node/Bun shim battery.

## Tests and evidence

- **Focused regression test** `test/es7/regexp-property-escapes.js`
  (registered once in `test/es7/rlexe.xml`, default launch,
  `-args summary -endargs`): 16 test groups / 365 assertions covering the
  full accept matrix (binary canonical+alias spellings, gc values incl.
  groups and legacy aliases, sc/scx incl. ISO codes and `Qaai`/`Qaac`,
  `Any`/`Assigned`/`ASCII`), the reject matrix (wrong case, spaces/hyphens
  not stripped, lone scripts, property-name-alone, value-under-wrong-
  property, non-ES properties, empty/`=`-only/two-`=`/unterminated bodies,
  `\p` without braces, class range endpoints, script values with no
  assigned code points), non-u identity-escape preservation (atom and class
  `p`/`P` without braces, short forms like `[\pL]`, trailing brace
  characters like `[\pxNd}]`, `[\p{Lu]` literal re-parse), matching
  semantics (binary, gc, sc vs scx on U+30FC, astral members, negation,
  surrogate code points), case-insensitive closure (atom-vs-class ordering,
  scoped modifier `i`), class unions/negations/dash literals, composition
  (quantifiers, alternation, lookbehind, named groups, split/replace,
  sticky), dynamic construction, `compile()` and `source` round-trip.
  **CH gate runs**: `ch` test-profile `pass` on default, `-noDeferParse`,
  `-CollectGarbage`, `-noDeferParse -CollectGarbage` and
  `-force:deferParse` launches, and the same five launches pass on the
  debug-static profile. **Separate Node/Bun oracle battery** (not the gate
  harness): the same file's assertions executed under Node 26.5.0 and
  Bun 1.4.3 through a compatibility shim — 365/365 assertions each.
- **Oracle batteries** (`/tmp/opencode/s3a-*.js`, kept outside the repo):
  a 79-probe semantic matrix, five name/alias/Annex-B batteries, a
  20-probe class-without-brace battery (`[\p]`, `[\pL]`, `[\P]`,
  `[\p]x{2}`, `[\pxNd}]`, `[\p{Lu]`, u-mode rejections) and a 70-probe
  unassigned-script battery (35 ISO codes × sc/scx) — ch matches Node 26.5.0
  and Bun 1.4.3 on every outcome (Bun and Node are identical to each other
  on all of them), and every non-u row is also identical to the base
  ChakraCore binary, so Annex B behavior is unchanged. The only textual
  difference anywhere is SyntaxError message wording. Remaining outcome-level differences vs Node, all in the
  engine's documented u-mode per-unit-matching lacunarity class
  (doc/c2-s9-modifiers.md "Limits"): `/\P{L}/u.exec('𝐀')` returns
  `[0,1]` ('\uD835') where Node returns null, `/\p{Cs}/u.exec('\uD800\uDC00')`
  matches the lone high surrogate at 0, and `/\p{Any}/u.exec('\uD83D\uDE00')`
  can match a single half of a pair when the set contains surrogate code
  points and the pair itself (the engine's search advances per UTF-16 unit;
  V8 advances per code point). None of these shapes is exercised by the
  generated corpus's anchored/per-symbol assertions.
- **Pinned test262** (`tools/test262/run.py`, default host flags
  `-nonative -Test262 -CollectGarbage`, controls 20/20 both runs), full
  bounded selection: `test/built-ins/RegExp/property-escapes/**` (all 613
  files), `match-indices/**`, `lookBehind/**`, `named-groups/**`,
  `regexp-modifiers/**`, `test/language/literals/regexp/**` = **1,980
  scenarios (990 files)**. Before (base binary `688e304462b85669` at the
  identical HEAD `084dcf06a`): **1,020 passed / 960 failed**
  (`out/test262-s3a-before`). After the initial implementation:
  **1,814 passed / 166 failed** (`out/test262-s3a-after`); after the review
  repairs, an identical rerun gives **1,814 passed / 166 failed**
  (`out/test262-s3a-after2`), and the repair join is **0 fail→pass /
  0 pass→fail** versus the pre-repair run (the fixed bugs are not exercised
  by this corpus). Combined per-scenario join versus the base binary:
  **794 fail→pass, 0 pass→fail**; the 166 residue all failed before too:
  - 96 timeouts + 16 assertion failures in `generated/` = 56 unique files:
    drift between the pinned corpus's newer generated property tables and
    the pinned **ICU 79.1** build's data — the corpus asserts scripts the
    ICU build does not enumerate (Garay, Gurung Khema, Kirat Rai, Sunuwar,
    Todhri, Ol Onal, ...) and assignments the ICU build does not have
    (e.g. `\P{Assigned}` must match U+208F; `Assigned`/`gc=Unassigned`/
    `gc=Other`/`Grapheme_Base`/`Extended_Pictographic`/`sc=Unknown`/
    `scx=Devanagari` files). No exact Unicode version is claimed for either
    side beyond the observed drift traits. The timeouts are drift-triggered: one drifted symbol
    fails `/^\p{X}+$/u`, the harness falls back to per-symbol tests over
    >100K symbols, and the interpreter's quadratic backtracking on the
    failing anchored loop exceeds the 10 s harness limit (Node completes
    the same files).
  - 14 `generated/strings/` scenarios: `v`-flag property-of-strings — out
    of scope by design.
  - 12 `non-existent-property-value-*` scenarios: the generated files
    contain a double-escaped `\\p{...}` literal; Node rejects them only
    because a bare `{` inside a u-mode literal is invalid
    (`{`-as-literal leniency is this engine's pre-existing gap, shared with
    the 22 `language/literals/regexp/u-invalid-*`/`u-astral-*` residue).
  - 6 `named-groups` scenarios: pre-existing `matchAll`/symbols-flag
    substrate (unchanged; same failures before).
  - The 8 `\p` scenarios that were the S9 modifiers receipt's residue are
    now green (`regexp-modifiers/**` is 100% after).
- **Adjacent controls** prove no regression: match-indices 48/48,
  lookBehind 100% (both modes), regexp-modifiers 100%, named-groups residue
  unchanged (6 pre-existing), literals/regexp residue unchanged (22
  pre-existing).
- **Gate impact**: `regexp-property-escapes.js` adds one rlexe entry → the
  regression gates move 2022/2074 → **2023/2075** (both profiles +1;
  `tools/build/gate_counts.json` intentionally not edited by this batch —
  the integrator's single-source bump, per the standing process).
- **Debug-static**: `zig build -Dprofile=debug-static` green; the focused
  test passes on `out/debug-static/bin/ch` (default and
  `-noDeferParse -CollectGarbage` launches) with debug asserts active. A
  bounded debug-profile corpus run (all 144 non-generated
  `property-escapes/*.js` + representative generated files `Any.js`,
  `Script_-_Greek.js`, `General_Category_-_Surrogate.js`) gives
  **282 passed / 12 failed (294 scenarios)** — the 12 are exactly the
  pre-existing double-escaped-literal files; no debug assert fired
  (`out/test262-s3a-debug`, re-verified post-repair in
  `out/test262-s3a-debug2`).

## Residuals

1. `v` flag and set notation: entirely unimplemented (rejected as today);
   `generated/strings/**` keeps failing. Next S3 slice.
2. ICU 79.1 data vs the pinned corpus's newer generated tables: 56
   generated files fail on scripts/assignments the ICU build does not know
   (observed drift traits above; exact Unicode versions deliberately not
   asserted). Bounded by the pinned dependency; lifting it needs the B5
   dependency decision. Exact residual at this pin: 16 assertion failures +
   96 drift-triggered timeouts as above.
3. Half-pair/lone-surrogate matching in u mode for sets that contain
   surrogate code points (exec positions and match spans; see oracle
   battery) — the engine's pre-existing per-unit u-mode lacunarity, now
   reachable via `\p{Cs}`/`\P{...}`/`\p{Any}`; not fixed by this slice.
4. Unterminated `\p{` bodies without `u`: every probed form (atom and
   class, literal and dynamic) is accepted as literals by Chakra, Node and
   Bun identically; a class that never closes is a SyntaxError everywhere.
   No known divergence remains here (an earlier draft of this receipt
   misattributed a missing-`]` error to the escape).
5. Pre-existing, untouched: `[^]` u-mode behavior change noted above
   (spec-correct now); `[^\u{10000}-\u{10FFFF}]`-style BMP-only complements
   still double-negate (pre-existing corner, preserved verbatim);
   `{`-as-literal leniency in u mode; named-groups symbols-flag residue.
6. Perf: big-set property escapes compile to multi-arm alternations; the
   interpreter's backtracking on failing anchored `^\p{X}+$` strings is
   quadratic (the 96 timeouts above). A bounded perf follow-up (loop or
   set-matching specialization) belongs to a later slice with the JIT (B4).

## JIT (B4)

All evidence is Mac no-JIT interpreter. The slice adds no JS bytecode and no
regex instruction; B4 must run `regexp-property-escapes.js`, the property
corpus and the adjacent RegExp corpora under SimpleJIT/FullJIT (dynapogo)
before any JIT-configuration claim, per the standing C2 policy.
