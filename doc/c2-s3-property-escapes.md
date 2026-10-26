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
  (`[\p{Lu]` without `u` is still a SyntaxError in both, as before.)

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

## Tests and evidence

- **Focused regression test** `test/es7/regexp-property-escapes.js`
  (registered once in `test/es7/rlexe.xml`, default launch,
  `-args summary -endargs`): 14 test groups / 262 assertions covering the
  full accept matrix (binary canonical+alias spellings, gc values incl.
  groups and legacy aliases, sc/scx incl. ISO codes and `Qaai`/`Qaac`,
  `Any`/`Assigned`/`ASCII`), the reject matrix (wrong case, spaces/hyphens
  not stripped, lone scripts, property-name-alone, value-under-wrong-
  property, non-ES properties, empty/`=`-only/two-`=`/unterminated bodies,
  `\p` without braces, class range endpoints), non-u identity-escape
  preservation, matching semantics (binary, gc, sc vs scx on U+30FC,
  astral members, negation, surrogate code points), case-insensitive
  closure (atom-vs-class ordering, scoped modifier `i`), class
  unions/negations/dash literals, composition (quantifiers, alternation,
  lookbehind, named groups, split/replace, sticky), dynamic construction,
  `compile()` and `source` round-trip. **Node 26.5.0 and Bun 1.4.3 pass the
  same file through a shim: 262/262 both.** `ch` (test profile): `pass` on
  default, `-noDeferParse`, `-CollectGarbage`, `-noDeferParse
  -CollectGarbage` and `-force:deferParse` launches.
- **Oracle batteries** (`/tmp/opencode/s3a-*.js`, kept outside the repo):
  79-probe semantic matrix and five name/alias/Annex-B batteries — ch
  matches Node on every outcome; the only textual difference is SyntaxError
  message wording. Remaining outcome-level differences vs Node, all in the
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
  (`out/test262-s3a-before`). After (worktree binary
  `9bf4a43e…`→final `ac858c67…`): **1,814 passed / 166 failed**
  (`out/test262-s3a-after`). Per-scenario join: **794 fail→pass,
  0 pass→fail**; the 166 residue all failed before too:
  - 96 timeouts + 16 assertion failures in `generated/` = 56 unique files:
    Unicode **17** (the corpus's generated tables) vs **ICU 79.1 = Unicode
    15.1** drift — new scripts (Garay, Gurung Khema, Kirat Rai, Sunuwar,
    Todhri, Ol Onal, ...), new assignments (`Assigned`/`gc=Unassigned`/
    `gc=Other`/`Grapheme_Base`/`Extended_Pictographic`/`sc=Unknown`/
    `scx=Devanagari`). The timeouts are drift-triggered: one drifted symbol
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
  (`out/test262-s3a-debug`).

## Residuals

1. `v` flag and set notation: entirely unimplemented (rejected as today);
   `generated/strings/**` keeps failing. Next S3 slice.
2. ICU 79.1 (Unicode 15.1) data vs the corpus's Unicode 17 tables: 56
   generated files fail on new scripts/new assignments. Bounded by the
   pinned dependency; lifting it needs the B5 dependency decision (ICU ≥ 76
   would add Unicode 16/17 scripts). Exact residual at this pin: 16
   assertion failures + 96 drift-triggered timeouts as above.
3. Half-pair/lone-surrogate matching in u mode for sets that contain
   surrogate code points (exec positions and match spans; see oracle
   battery) — the engine's pre-existing per-unit u-mode lacunarity, now
   reachable via `\p{Cs}`/`\P{...}`/`\p{Any}`; not fixed by this slice.
4. Unterminated `\p{` without `u` is accepted as literals (V8/JSC reject);
   deliberate preservation of existing non-u behavior.
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
