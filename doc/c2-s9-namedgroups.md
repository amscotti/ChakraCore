//-------------------------------------------------------------------------------------------------------
// Copyright (C) Microsoft. All rights reserved.
// Copyright (c) ChakraCore Project Contributors. All rights reserved.
// Licensed under the MIT license. See LICENSE.txt file in the project root for full license information.
//-------------------------------------------------------------------------------------------------------

# C2-S9 sub-batch 1: RegExp named capture groups (core)

Status: landed on Mac no-JIT (committed as 1bae6c94a). ES2018 named
capture groups are **unconditional** (no flag, no new opcodes): the
`(?<name>...)` syntax, the `\k<name>` backreference, the `groups` property
on match/exec results (null-prototype object, `undefined` for
non-participating groups, property order = textual group order), and
`$<name>` substitution in `String.prototype.replace`/`replaceAll` on both
the ES5 legacy fast path and the `@@replace` symbol path. JIT evidence is
not claimed; see B4 below. Deferred sub-batches: lookbehind, `d`
flag/`indices`, `v` flag, duplicate named groups (ES2025) — recorded in
c1-triage §S9.

## What landed

1. **Syntax** (`lib/Parser/RegexParser.{h,cpp}`): `(?<Name>...)` parses in
   both passes as a capturing group (the existing `DefineGroupNode`
   machinery; no compiler change). Names are `RegExpIdentifierName`s
   validated with the scanner's `CharClassifier` (`IsIdStart`/`IsIdContinue`
   → full Unicode incl. astral via ICU, `$`/`_`, ZWJ/ZWNJ), decoded to
   UTF-16 through the encoding policy: `\uXXXX`, `\u{...}` (both modes, per
   corpus `non-unicode-property-names-valid.js`), escaped surrogate pairs,
   raw surrogate pairs, and astral UTF-8 source sequences all normalize to
   the same code point sequence. Lone surrogates (raw or escaped) are
   SyntaxError in both modes. `(?<=`/`(?<!` **remain SyntaxError** (the
   lookbehind sub-batch); an unparsable name after `(?<` (e.g. `(?<=`) is
   rejected by the ID_Start check, so no ambiguity. Duplicate names are
   SyntaxError until duplicate named groups land.
2. **`\k<Name>` backreference**: `\k` is a named backreference iff the
   pattern has any named group or the `u` flag (the spec's *N* parameter —
   unknowable at the escape site because references may precede
   definitions). Pass 0 therefore records only the **raw span** of each
   `\k<...>` (multi-unit-safe scan to the first `>`); a new
   `ResolveNamedGroupRefs()` runs after option parsing (both `ParseDynamic`
   and `ParseLiteral`, beside the deferred-unicode checks) and
   re-validates/resolves every span, throwing `JSERR_RegExpSyntax` on
   dangling or unterminated names or on a bare `\k` when *N*. When !*N*,
   pass 1 re-parses `\k` as the Annex B identity escape 'k' and any
   `<...>` tail as literal atoms (`/\k<a>/.test("k<a>")` is true; Node
   26.5.0 agrees). A resolved reference compiles to the existing
   `MatchGroupNode` — forward references and self-references work through
   the numbered-group machinery. `\k` inside a character class is **not** a
   backreference form anywhere (class escapes have no `NamedCaptureReference`);
   it is the Annex B identity escape 'k' only in a non-u pattern with no
   named captures, and a `SyntaxError` under the u flag or once named
   captures appear anywhere in the pattern (Annex B's
   `SourceCharacterIdentityEscape` reserves `k` when *N* — see review fixes
   below; Node 26.5.0 agrees on every combination, including non-u
   `(?<a>x)[\k]`).
3. **Name registry on the pattern** (`lib/Parser/RegexRuntime.{h,cpp}`):
   `Program` gains `numGroupNames`, `groupNameEntries` (offset/len/groupId)
   and `groupNameChars` — recycler-owned leaves, immutable after
   `CaptureGroupNames` (called from `CaptureSourceAndGroups`, which covers
   both the dynamic and the literal entry points). No static PropertyIds,
   no bytecode change (`zig build check`: "source headers unchanged; regen
   exit 0" — unlike C2-S7, nothing to regenerate). `CopyToScriptContext`
   shares the Program, so names are shared safely.
4. **`groups` on results** (`JavascriptRegularExpressionResult`,
   `JavascriptLibrary`, `RegexHelper`): the regex-result type path gains a
   third entry, `groups`, as a **dynamic** property record
   (`GetOrAddPropertyRecord("groups")` cached on the library), and the
   result type's inline slot capacity goes from 2 (`ObjectGranularity /
   sizeof(Var)`) to 4 (3 entries rounded up to
   `RoundUpInlineSlotCapacity`; the debug build asserts the rounding —
   caught by the debug-static gate mid-batch). Exec results and non-global
   `String.prototype.match` results now always own a `groups` data property
   (writable/enumerable/configurable, per corpus
   `groups-object-undefined.js`): `undefined` for patterns without named
   groups — **no groups object is allocated on that fast path** (one
   SetSlot of undefined) — and a freshly built null-prototype object
   otherwise (`CreateGroupsObject`: `CreateObject(GetNull())` — note a C++
   `nullptr` prototype crashes `CreateObject`'s `IsNull`, the JS null *value*
   must be passed — then one ordinary `[[Set]]` per name, which on a
   null-prototype object is exactly CreateDataProperty, including for the
   name `__proto__`). Global-match arrays (plain arrays) get no `groups`,
   matching Node. Property order is the textual order of the definitions.
5. **`$<name>` substitution** (`RegexHelper::ReplaceFormatString`): the
   `$`-substitution switch gains a `'<'` arm behind a
   `namedSubstitutionEnabled` flag with a pluggable resolver:
   - *ES5 legacy path* (default flags, `RegexEs5ReplaceImpl`): enabled iff
     the pattern has named groups; names resolve against the pattern's
     static registry + live match (`GetString(pattern->GetGroup(id))`),
     so no groups object is materialized. Corpus
     `string-replace-{nocaptures,unclosed,missing,...}.js` pins the
     literal/empty-fallback semantics: unknown or empty name → consumed,
     contributes nothing; no closing `>` → literal `$`; no named captures
     → literal `$` (with `$1` digit fallbacks still working, e.g.
     `$<42$1>` → `$<42a`).
   - *`@@replace` path* (`RegexEs6ReplaceImpl`): the C2-S7 groups
     Get+ToObject coercion now **captures** the coerced object per result;
     `$<name>` resolves via `GetProperty(namedCaptures, name)` so custom
     exec results drive substitution (corpus `groups-object-subclass{,-sans}.js`:
     own, inherited, and `Array.prototype.groups` values all observed).
     `ToString` of non-string values is applied (spec GetSubstitution).
   - A `$` inside a consumed name (e.g. `$<42$1>`) was pre-registered by
     `GetReplaceSubstitutions`; the loop now skips offsets swallowed by a
     consumed `$<...>` block (negative-length Append → SIGABRT, found by
     corpus `string-replace-missing.js`).
6. **Replacement functions** receive the groups object as the final
   argument when the exec result's `groups` is not `undefined` — the ES6
   path appends the coerced object (6 visible args for
   `/(?<fst>.)(?<snd>.)/`), the ES5 path builds the groups object per
   match only for named-group patterns (3 visible args otherwise, both
   Node-matching). `String.prototype.replaceAll`'s regex path reaches both
   impls unchanged; its string-search path keeps `$<name>` literal
   (no named captures — Node-matching).

## Review fixes (post-verification, same batch)

1. **`new RegExp(pat, flags)` heisenbug with resolvable `\k<name>`**
   ([bug], high): in a fresh script `new RegExp("(?<a>x)\\k<a>", "g")` —
   or any flags — threw `SyntaxError`, while the same expression succeeded
   in a script that had built other regexes first; literals and
   `.compile()` were unaffected. Root cause: `ParseDynamic` parses the
   flags from a second string (`SetPosition(opts, optsLim, false)`), and
   `ResolveNamedGroupRefs()` then re-scanned the recorded `\k` spans with
   `next` pointing back into the *body* buffer while `IsEOF()` was still
   bound to `optsLim` — whether the scan terminated at `>` or immediately
   "hit EOF" depended on the two buffers' relative heap addresses. Fix
   (lib/Parser/RegexParser.cpp, `ResolveNamedGroupRefs`): the function now
   takes the pass-0 body range (`body`/`bodyLim`) from both entry points,
   `SetPosition`s to it for the re-scan, and restores the caller's full
   position (input/limit/next/inBody plus the encoding policy's
   multi-unit counter — `ParseLiteral` reads `next` and `Pos()` right
   after the call). Verified across fresh and warm scripts, every flag
   alone and combined (`i g m s y u`), `.compile()`, literals, forward
   references and two-arg `lastIndex` shapes — all matching Node 26.5.0.
2. **Class-context `\k` escape rule** ([bug]): `new RegExp("[\\k<a>]","u")`
   returned a pattern where Node throws. `ClassEscapePass0` now records
   `\k` (`sawClassK`, mirroring `sawBareK`) and `ResolveNamedGroupRefs`
   rejects it whenever *N* (named captures anywhere) or the u flag is
   present; without either, `[\k]` keeps the Annex B identity escape 'k'
   (`/[\k]/.test("k")` still true). Bare `/\k/u` outside classes still
   throws as before; `(?<a>x)[\k]` non-u also throws (Node-matching Annex B
   `SourceCharacterIdentityEscape` — verified against Node 26.5.0 before
   landing the assertions).

3. **`$` in `\k<name>` references** ([bug], found in cycle-3 review):
   `(?<$a>x)\k<$a>` threw `SyntaxError` where Node accepts it, because
   the pass-0 `\k` span scan listed `$` among its structural stop
   characters — but `$` is a valid identifier character in group names
   (the same grammar the `(?<name>...)` definitions already accept). Fix:
   `$` removed from the stop set (RegexParser.cpp, the span-scan switch);
   `\k<$a>`-shaped references now resolve, dollar-only names
   (`(?<$>x)\k<$>`) work, and unterminated-after-`$` shapes still throw.
   Matrix verified against Node 26.5.0 (leading/trailing/interior/dollar-
   only names, flags variants, identity fallback, unterminated rejects).

The three fixes are parser-only (no allocation, ownership, GC or barrier
change: the span re-scan rebinds existing scanner state and restores it;
`sawClassK` is an arena-lifetime bool on the compile-time parser). Fix 3
deletes one stop-character case; all remaining stops are structural
syntax that can never occur inside a name.

## Where groups live / allocation strategy

The registry lives on the recycler-owned `Program` (shared by pattern
clones across script contexts). Per-match runtime state stays where it
always was (matcher group arrays). The groups *object* exists only on
results of patterns with named groups: one `DynamicObject` + N property
adds per exec/match/replace-function iteration. No new allocation domain,
no barrier change (plain `RecyclerNewArrayLeaf` for chars/entries; the
object graph is ordinary recycler objects). Not claimed: OOM injection,
`-RecyclerStress`, or an allocation profile for the named-group path.

## Tests and gates

- `test/es7/namedgroups.js` (23 tests, default launch, `-args summary`):
  syntax accept/reject matrices (incl. escape forms, lookbehind-stays-
  error, `\k` identity fallback), groups-object semantics (null proto,
  undefined for non-participating, order, descriptors, `__proto__` name,
  CreateDataProperty-not-Set vs `Array.prototype`/`Object.prototype`
  setters), backreference semantics (alternation, forward, self,
  non-participating matches empty), `$<name>` in replace incl. literal
  fallbacks and `$`-in-name, replaceAll, replace-function arity and
  per-match groups, `lastIndex`/global interplay, dynamic RegExp, the
  fresh-dynamic-construction flag matrix (all single flags and several
  combinations incl. `igmsuy`, `.compile()`, literal, forward reference —
  regression for the re-scan heisenbug), and the class-`\k` matrix
  (identity without named captures/u; `SyntaxError` under u, under named
  captures non-u, in negated classes, as a range endpoint, and via both
  constructor and literal/`compile` forms; bare `/\k/u` still throws).
  Every added assertion was verified against Node 26.5.0 first
  (51/51 equal, incl. the named-captures-non-u throw Node requires).
- `test/es7/namedgroups-symbols.js` (7 tests, `-ES6RegExSymbols`
  registration, `matchall.js` precedent): matchAll iterator results carry
  per-step groups; `@@replace` with FakeRegExp custom exec results (own/
  inherited groups, `Array.prototype.groups` fallback); replaceAll through
  `@@replace`; exec under the flag.
- Gates bumped 1968→1970 (test) and 2020→2022 (debug-static) in
  `tools/build/check_regression.py` (docstring, `EXPECTED_PASSED`,
  `SELECT_COUNTS`, exact-baseline + debug self-test pins; the deliberate
  stale 2014/1962 fixtures stay), `build.zig` (both profiles),
  `.github/workflows/zig-macos.yml` (test, debug-static, legacy cmake
  legs), and the live baseline sentence in `doc/memory-management.md`.

## Evidence (final tree after review fixes, `zig-out/bin/ch` + `out/debug-static/bin/ch`)

Provenance for the review-fix reruns: launcher `zig-out/bin/ch`
sha256 `43ffa19e701b333be69ca8c5fe160e4b0f9987727a31ac890c79a6863ad3e679`
(unchanged across rebuilds — it is a thin launcher), engine
`zig-out/lib/libChakraCore.dylib` sha256
`b50cf68efc503f8ce8b7c305b18c8431876ae73aa0e143c526b037c01fa35d8f`
(the earlier receipts pinned the pre-fix dylib
`db2b531718f226fe5cb8fce774a8a932a6e0b1bbd26c0acb5a8f67d574f30464`; every
scoped artifact below was re-run into the same directories against the
fixed dylib, superseding the earlier in-flight artifact churn from the
stash/build cycle while the fixes were being made).

- `mise exec -- zig build` (default) and `-Dprofile=debug-static` — exit 0;
  `zig build check --summary line` — 572/572 steps, regression
  `Total: passed 1970, failed 0` (DebuggerCommon 208/0 — no baseline
  churn, bytecode "source headers unchanged; regen exit 0"), native,
  smoke, metadata PASS. `zig build check-regression -Dprofile=debug-static
  --summary line` — `Total: passed 2022, failed 0`, 545/545 steps.
- Focused tests: `pass` on both binaries (verbose 23/23 and 7/7).
- Corpus (pinned test262, `-nonative -Test262 -CollectGarbage`),
  default flags, artifact `out/test262-s9-namedgroups` (re-run after the
  review fixes):
  **158 passed / 26 failed** of 184 scenarios
  (`test/built-ins/RegExp/named-groups/**` +
  `test/language/literals/regexp/named-groups/**`; the String
  replace/match/matchAll `named-groups*` subdirs do not exist in this
  corpus — those cases live in the RegExp directory's `string-replace-*`
  files). With `-ES6RegExSymbols` (`out/test262-s9-namedgroups-sym`):
  **162/22**. Residue classification: 20 duplicate-names-* (ES2025
  duplicate named groups — deferred sub-batch), 2 lookbehind (deferred),
  4 `groups-object-subclass{,-sans}` scenarios that need the symbols flag
  and pass in the `-sym` run. Same 158/26 on the rebuilt debug-static
  binary (`out/test262-s9-namedgroups-dbg`, launcher sha
  `2d744a8c9d38f8ef…`) with asserts on.
- Resym universe (`test/built-ins/{RegExp,String}/**/*.js`, 6,199
  scenarios), join key `(file, scenario)` — both re-run after the review
  fixes, numbers identical to the pre-fix runs:
  - default flags (`out/test262-s9-resym-base`, 4,021/2,178) vs
    `out/test262-zig`: **168 fail→pass, 0 pass→fail**;
  - `-ES6RegExSymbols` (`out/test262-s9-resym`, 4,479/1,720) vs
    `out/test262-audit-resym`: **194 fail→pass, 0 pass→fail**.
- `analyze.py` on the corpus artifact (post-fix rerun):
  `regexp-named-groups` 158/6
  (the 6 = the flag-gated subclass scenarios); buckets
  `required-feature-missing` 22 (deferred sub-batches),
  `engine-defect-on-existing` 4 (same subclass scenarios; they pass under
  the flag). No bucket moved.
- Node 26.5.0 cross-checks: ~45 shapes probed equal pre-fix (groups presence/
  descriptors on results with and without named groups, global match
  arrays without `groups`, null prototype, `__proto__` name, order,
  non-participating→undefined/empty, `$<...>` accept/reject matrix incl.
  `$<42$1>`/`$<>`/unclosed/`$$`, replace-function arities 6/5/3,
  matchAll per-step groups, `\k` identity vs named mode, syntax
  accept/reject incl. `\k1`, class-`\k`, exotic/astral/escaped names).
  The review fixes added ~100 more probes (fresh/warm dynamic construction
  across every flag alone and combined, `.compile()`, literal, forward
  reference and `lastIndex` shapes; the class-`\k` matrix incl. `[\k<a>]`,
  negated classes, range endpoints, named-captures-non-u, and
  constructor/literal/`compile` forms — the 51 mirrored assertions in
  `namedgroups.js` were run green in Node first, 0 mismatches).
- Full corpus (`test/language` + `test/built-ins` + `test/annexB`,
  93,141 scenarios, `out/test262-s9-full`, final binary): 63,250 passed /
  29,891 failed vs the 61,565/31,576 baseline. Per-scenario join:
  **1,735 fail→pass, 50 pass→fail — every pass→fail lives in
  module-host files** (44 `dynamic-import/` scenarios plus 6
  ShadowRealm / `import-defer` / `module-code` scenarios), the
  documented c1-triage S12 host-bucket module-load instability.
  The set itself moves run-to-run on identical code (the first full
  run recorded 43 pass→fail with 38+5, this final-artifact run
  44+6 — same families, different members), and individual members
  flap True/False across 3× isolation reruns. Spot checks confirm the
  mechanism: `ShadowRealm/importValue/not-constructor` and
  `import-defer/…` fail deterministically *in isolation* on the
  current binary, so the C1-baseline passes were the flakes (that
  baseline is itself a single run of the same flaky harness).
  0 pass→fail outside the module-host family; fail→pass by area:
  built-ins 1,548 (unchanged from the first run), language 185 (190 in
  the first run — the −5 is the same dynamic-import flap set), annexB 2.

## Limits

- Lookbehind (`(?<=`, `(?<!`), `d` flag/`indices`, `v` flag/property
  escapes, and ES2025 duplicate named groups are **not implemented**;
  their corpus files are the classified residue. `duplicate-names-*` and
  `lookbehind.js` in the named-groups corpus fail by design.
- Group-name classification rides the same `CharClassifier` mode as JS
  identifiers: with `ES6Unicode` off (not a supported default) astral
  names would be rejected — the corpus and default flags keep ICU-based
  ES6 classification on.
- The `groups` property is now **unconditionally present (value
  `undefined` when no named groups) on every exec/match result** — an
  observable default-flag behavior change mandated by ES2018 (corpus
  `groups-object-undefined.js`; Node matches). No pass→fail anywhere in
  the joined universes.
- ES5 replace-with-function on a named-group pattern builds a groups
  object per match (observable via the final function argument); the
  string-replacement ES5 path never materializes one.
- `lastIndex`/capture plumbing, `split`, `search`, and `test` are
  untouched (named captures are ordinary numbered captures there).
- C2-S7's `Symbol.matchAll` note stands: `AdvanceStringIndex` is +1 per
  code unit (surrogate-pair spillover is pre-existing S7 debt).

## JIT (B4)

All evidence is Mac no-JIT interpreter. No new opcode, no bytecode
emission change, no PropertyId additions (dynamic records only — nothing
to regenerate, unlike C2-S7). B4 must (a) run `namedgroups.js`,
`namedgroups-symbols.js`, and the two corpus directories under
SimpleJIT/FullJIT before any JIT-configuration claim, and (b) re-audit
the JIT-inlined String/RegExp helpers now that plain `replace` results
and `$<name>` substitutions flow through `ReplaceFormatString` with the
new `'<'` arm (the ES5 fast path stays allocation-free for patterns
without named groups).
