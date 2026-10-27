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
    otherwise **one new `MatchCodePointSetNode`** (see "Perf repair" below —
    the mixed case originally reused the existing prefix/pair-arms/suffix
    alternation with no new node kinds, which is also what introduced the
    exponential-backtracking defect). No JS-bytecode or PropertyId changes;
    property escapes reuse the existing set machinery, so lookbehind
    mirroring, scoped modifiers, quantifiers, capture/backreference and
    `compile()` interplay come for free.
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

## Perf repair (iteration 2): deterministic whole-code-point-set matcher

**Defect (integration-blocking, found by combined review):** quantified
complement property sets backtracked **exponentially** on failing
continuations (`^\P{Lu}+z$`, `^[\P{Lu}]+$`, `^\P{Script=...}+z$`):
n=20 ≈ 544 ms, n=22 ≈ 2.2 s, n≥24 > 60 s (Node/Bun < 1 ms). Root cause:
`TranslateCodePointSet` expanded any u-mode set with astral members into an
alternation (BMP prefix + surrogate-pair range arms + lone-surrogate
suffix). A surrogate pair in the input could be consumed either as one pair
arm (2 units) or as lone surrogate units (1 unit each), so a quantifier
over the atom carried several live choices per iteration and a failing
continuation enumerated the partitions (2^n). The same defect shape existed
in the pre-existing u-mode class translation (`^[^a]+z$` u-mode showed the
same curve on the base binary), and `CharSet::CloneSurrogateCodeUnitsTo`
additionally leaked non-surrogate units into its "surrogate-only" result
through the unfiltered first-256 bitmap (now fixed; the function is no
longer on any live path).

**Fix:** mixed BMP/astral sets now compile to **one** new AST node
`MatchCodePointSetNode` (`lib/Parser/RegexCompileTime.{h,cpp}`; tag
appended after the ES2018 set) and **one** new fixed-size instruction
`MatchCodePointSetInst` (`lib/Parser/RegexRuntime.{h,cpp}`; opcode
appended after `FrameNegatedWordBoundaryTest`, so no earlier tag moves).
The node holds, in the parser's compile-time arena: the BMP member units as
a `CharSet<Char>` (including lone-surrogate units), a `firstUnits` upper
bound (BMP units plus all high surrogates when astral members exist;
`isFirstExact` false), and the sorted astral member ranges as a flat
`codepoint_t[2n]` array. `Emit` allocates the instruction in the existing
packed instruction buffer (fixed `sizeof(MatchCodePointSetInst)`; the
ranges are **not** in-line — the instruction carries an
arena-`AnewArray`'d pointer, mirroring the `ScannersMixin` pointer pattern),
clones the set into the run-time allocator like every `SetMixin` user, and
copies the ranges there; the node's compile-time arena data dies with the
parser as before. No ICU object, GC root, tracer or barrier is involved
(the arena-wholesale ranges array needs no per-node free, matching the
`ScannersMixin` infos precedent). Matching is **deterministic**: at each
position a complete surrogate pair that is a member consumes 2 units
(binary range search over the ranges); otherwise the single unit stands for
its own code point and consumes 1 — no choicepoints per iteration, so
quantifiers greedy/lazy/bounded/optional, groups, backreferences and
lookbehind bodies backtrack linearly. The former
`AppendSurrogateRangeToDisjunction`/`AppendSurrogatePairToDisjunction`
builders became dead and were removed (~10.8 KB). The node reports
`isDeterministic` and rejoins the Guarded/GreedyNoBacktrack loop schemes
for suitable follows; it is **deliberately excluded** from prefix-skipping
(`SupportsPrefixSkipping` false — a 1-or-2-unit atom defeats fixed-prefix
accounting) and from set-scan synchronization
(`HeadSyncronizingNode`/`EmitScan` unreachable — a first-unit scan cannot
decide pair membership and consuming on sync would accept non-members),
and `IsCharOrPositiveSet` is false so the concat canonical form (no two
adjacent unit-set atoms) and the char/trie recognizers leave it alone.
All-BMP sets keep the single `MatchSetNode` path and astral singletons
keep the surrogate-pair atom.

**Semantics: one intentional, spec-convergent delta.** The old surrogate
disjunction allowed a quantifier to backtrack into a *consumed member pair*
and re-split it into two lone-unit atoms. `\uD800\uDC00` is U+10000, a
**member** of the complements `[\u{10400}]`-excluded and
`[\u{10400}-\u{10427}]`-excluded, so on the base engine
`/^[^\u{10400}]{2}$/u.test("\uD800\uDC00")` returned `true` (two lone
units) while Node 26.5.0/Bun 1.4.3/spec say `false`; likewise
`/^[^\u{10400}-\u{10427}]{2,}$/u` (base `true`, spec `false`) and
`/^[^\u{10400}]*\uDC00$/u.exec("\uD800\uDC00")` (base matched,
returning the whole pair; spec/Node/Bun return `null`).
`MatchCodePointSet` deterministically consumes a member pair as one
2-unit code point and never reinterprets it, so those now return
`false`/`null` — a deliberate convergence to the spec matching
Node/Bun, **not** exact base preservation; the focused test pins it
(group "Member pairs are never re-split into lone units", which also
verifies a member pair still counts as exactly one iteration spanning
both units). Everything else is preserved: the pair-then-lone-unit *preference
order* at a single position is unchanged, and a 30-probe quantifier battery
(greedy/lazy/bounded/optional, atoms and classes, Lu/Script/scx/gc groups,
astral and lone-surrogate runs, iu closure, capture groups, backreference,
lookbehind, split/replace) matches Node and Bun on every outcome
(`s3a-sem`). The 79-probe oracle matrix and the name/alias/Annex-B/
class-without-brace/unassigned-script batteries are unchanged (only the
three documented pre-existing u-mode search divergences remain).

**Perf evidence** (`s3a-bign`/`s3a-redos`, deterministic local probes,
not gates): property-set failing continuations are now linear — ≤1 ms at
n=26..1024 for `^\P{Lu}+z$`/`^[\P{Lu}]+z$` atom and class forms, astral
runs at n=512, `^\p{Script=Latin}+z$` n=1024 — with outputs identical to
Node 26.5.0/Bun 1.4.3 (0-1 ms there). Before the repair the same inputs
took 544 ms at n=20 and 2.2 s at n=22 on this engine (measured), i.e.
exponential; no property-set shape times out in the local ladder. The
focused gate test asserts the n=256..1024 failing continuations (correct
`false` results, no wall-clock dependency).

**Scope of the perf claim:** only property/class-set atoms are covered.
Generic nested-quantifier and alternation ReDoS shapes remain exponential
**engine-wide** (base-identical backtracking, not property-specific):
`^(?:\P{Lu}|\w)+z$` takes ~9 ms at n=16 and ~101 ms at n=20 here, and
`^(?:\P{Lu}+)+z$` ~0/46 ms at n=16/20 (Node mitigates these partially at
5-10 ms; both remain superlinear there too). Fixing generic backtracking is
out of scope for this slice.

## Tests and evidence

- **Focused regression test** `test/es7/regexp-property-escapes.js`
  (registered once in `test/es7/rlexe.xml`, default launch,
  `-args summary -endargs`): 17 test groups / 400 assertions covering the
  full accept matrix (binary canonical+alias spellings, gc values incl.
  groups and legacy aliases, sc/scx incl. ISO codes and `Qaai`/`Qaac`,
  `Any`/`Assigned`/`ASCII`), the reject matrix (wrong case, spaces/hyphens
  not stripped, lone scripts, property-name-alone, value-under-wrong-
  property, non-ES properties, empty/`=`-only/two-`=`/unterminated bodies,
  `\p` without braces, class range endpoints, script values with no
  assigned code points), non-u identity-escape preservation (atom and class
  `p`/`P` without braces, short forms like `[\pL]`, trailing brace
  characters like `[\pxNd}]`, `[\p{Lu]` literal re-parse), quantified-set
  semantics and linear backtracking (greedy/lazy/bounded/optional over
  \P/\p atoms and classes across Lu/Script/scx/gc, astral and
  lone-surrogate runs, iu closure, groups, backreferences, lookbehind,
  split/replace, and failing continuations at n=256..1024 that were
  exponential before the perf repair), matching
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
  Bun 1.4.3 through a compatibility shim — 400/400 assertions each
  (round-1 file: 15 groups / 365 assertions; the perf repair added the
  quantified-set group, and this review iteration adds the member-pair
  re-splitting group).
- **Oracle batteries** (`/tmp/opencode/s3a-*.js`, kept outside the repo):
  a 79-probe semantic matrix, five name/alias/Annex-B batteries, a
  20-probe class-without-brace battery (`[\p]`, `[\pL]`, `[\P]`,
  `[\p]x{2}`, `[\pxNd}]`, `[\p{Lu]`, u-mode rejections) and a 70-probe
  unassigned-script battery (35 ISO codes × sc/scx) — ch matches Node 26.5.0
  and Bun 1.4.3 on every outcome of **these batteries** (Node and Bun also
  agreed with each other on all of them), and every non-u row is also
  identical to the base ChakraCore binary, so Annex B behavior is
  unchanged. This is a claim about the listed batteries only — the engines
  are not universally identical, and the remaining traced differences are
  the documented engine divergences below (u-mode per-unit search/lone-
  surrogate lacunarity; `v`-flag acceptance). The only textual
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
  `-nonative -Test262 -CollectGarbage`; the runner's own 20 harness
  control scenarios — sync/async/module/parse-negative/runtime-negative/
  realm/detachment plus deliberate failures — passed 20/20 in every run
  cited here), full
  bounded selection: `test/built-ins/RegExp/property-escapes/**` (all 613
  files), `match-indices/**`, `lookBehind/**`, `named-groups/**`,
  `regexp-modifiers/**`, `test/language/literals/regexp/**` = **1,980
  scenarios (990 files)**. Run identity (recorded in each `summary.json`;
  the `ch` launcher embeds a stable content hash, so the **dylib** hash is
  the distinguishing artifact): base run = launcher `688e3044…` +
  dylib `59194083…`; final run (`out/test262-s3a-after3`) = launcher
  `ac858c67…` + **dylib `c92cf6c9…`** (the pre-perf-repair
  `...-after2` run used dylib `1b025b98…`). Reproducible results:
  base **1,020 passed / 960 failed**; final **1,814 passed / 166 failed**
  with **0 of the 1,980 scenarios carrying `timeout: true`** in the final
  `results.json` (the per-scenario `rawResult.timeout` field is recorded
  by the harness; the 96 prior timeouts were identified from the same
  field in the earlier runs). Per-scenario joins: versus base **794
  fail→pass, 0 pass→fail**; versus the pre-perf-repair run **0 flips in
  either direction** — the 166 residue scenarios are the same set
  throughout, all failing as fast assertions:
  - 112 `generated/` scenarios (56 unique files) = pure **data drift**
    between the pinned corpus's newer generated property tables and the
    pinned **ICU 79.1** build: the corpus asserts scripts the ICU build
    does not enumerate (Garay, Gurung Khema, Kirat Rai, Sunuwar, Todhri,
    Ol Onal, ...) and assignments it does not have (observed: the corpus
    requires `\P{Assigned}` to match U+208F ₏, and `Assigned`/
    `gc=Unassigned`/`gc=Other`/`Grapheme_Base`/`Extended_Pictographic`/
    `sc=Unknown`/`scx=Devanagari` files fail on their first drifted
    symbol). No exact Unicode version is claimed for either side beyond
    these observed traits. 96 of the 112 previously exceeded the 10 s
    harness limit — that was the exponential-backtracking defect below,
    not data drift: after the perf repair every one of them completes
    quickly and fails on the same drifted symbols the 16 already-asserting
    files report.
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
- **Adjacent legacy/es7 controls** (separate from the runner's 20 harness
  controls above): 8 gate files — es6 `regex-set`, `regex-annex-b`,
  `regex-case-folding`, `regex-quantifiers`, `regex-code-point`, es7
  `regexp-modifiers`, `regexp-lookbehind`, `namedgroups` — produce
  **byte-identical output on the worktree and base binaries** (including
  lookbehind's shared 15/16). Inside the test262 selection:
  match-indices 48/48, lookBehind 100% (both modes), regexp-modifiers 100%,
  named-groups residue unchanged (6 pre-existing), literals/regexp residue
  unchanged (22 pre-existing).
- **Gate impact**: `regexp-property-escapes.js` adds one rlexe entry → the
  regression gates move 2022/2074 → **2023/2075** (both profiles +1;
  `tools/build/gate_counts.json` intentionally not edited by this batch —
  the integrator's single-source bump, per the standing process).
- **Debug-static**: `zig build -Dprofile=debug-static` green; the focused
  test (16 groups incl. the quantified-set group) passes on
  `out/debug-static/bin/ch` on default, `-noDeferParse`, `-CollectGarbage`,
  `-noDeferParse -CollectGarbage` and `-force:deferParse` launches with
  debug asserts active (one assert fix during the loop: the new node must
  not report `IsCharOrPositiveSet`, or the concat canonical-form assert
  fires on adjacent set atoms). A
  bounded debug-profile corpus run (all 144 non-generated
  `property-escapes/*.js` + representative generated files `Any.js`,
  `Script_-_Greek.js`, `General_Category_-_Surrogate.js`) gives
  **282 passed / 12 failed (294 scenarios)** — the 12 are exactly the
  pre-existing double-escaped-literal files; no debug assert fired
  (`out/test262-s3a-debug`, re-verified post-repair in
  `out/test262-s3a-debug2` and post-perf-repair with the quantified-set
  tests in `out/test262-s3a-debug3` — same 282/12).

## Residuals

1. `v` flag and set notation: entirely unimplemented (rejected as today);
   `generated/strings/**` keeps failing. Next S3 slice.
2. ICU 79.1 data vs the pinned corpus's newer generated tables: 56
   generated files fail on scripts/assignments the ICU build does not know
   (observed drift traits above; exact Unicode versions deliberately not
   asserted). Bounded by the pinned dependency; lifting it needs the B5
   dependency decision. Exact residual at this pin: 112 generated scenarios
   (56 files) failing as instant drift assertions — all complete quickly
   since the perf repair; 96 of them previously exceeded the harness
   timeout.
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
6. ~~Perf: exponential backtracking on quantified complement sets~~ —
   **fixed** by the deterministic whole-code-point-set matcher (see "Perf
   repair" above); the former 96 corpus timeouts now complete instantly
   and fail on drift data alone. **Scope:** generic nested-quantifier /
   alternation ReDoS shapes (`^(?:\P{Lu}|\w)+z$`,
   `^(?:\P{Lu}+)+z$`) remain exponential engine-wide — base-identical
   backtracking, not property-specific; separate engine work.
7. Member-pair re-splitting (old base u-mode class/property sets could
   backtrack a consumed member pair into two lone-unit atoms) —
   **intentionally removed** as part of the perf repair; the new behavior
   matches spec/Node/Bun and is pinned by the focused test (see "Perf
   repair" for the delta cases). The complementary pre-existing divergence
   remains: a non-member pair whose lone units are members still matches
   the lone units here (Node/spec fail) — residual 3.
8. Pre-existing, deliberately untouched this iteration: the malformed
   u-mode `{`/`]` quantifier leniency (base-identical; separate follow-up
   per the review instruction).

## JIT (B4)

All evidence is Mac no-JIT interpreter. The slice adds no JS bytecode but
**does add one regex instruction** (`MatchCodePointSet`; fixed-size,
interpreter-executed like its siblings); B4 must run
`regexp-property-escapes.js`, the property
corpus and the adjacent RegExp corpora under SimpleJIT/FullJIT (dynapogo)
before any JIT-configuration claim, per the standing C2 policy.
