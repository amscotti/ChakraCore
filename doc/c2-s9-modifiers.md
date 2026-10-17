//-------------------------------------------------------------------------------------------------------
// Copyright (C) Microsoft. All rights reserved.
// Copyright (c) ChakraCore Project Contributors. All rights reserved.
// Licensed under the MIT license. See LICENSE.txt file in the project root for full license information.
//-------------------------------------------------------------------------------------------------------

# C2-S9 sub-batch 5: ES2025 scoped RegExp modifiers

Status: landed in `724ad82dd` on Mac no-JIT (JIT gated at B4). ES2025 modifier groups `(?ims-ims:Disjunction)` are
**unconditional core syntax**: no config flag, no JS-bytecode change, no
static PropertyIds (`zig build verify-bytecode`: "source headers unchanged;
regen exit 0"). The `v` flag and property-of-strings escapes remain deferred
to S3; the only scoped-corpus residue is exactly `\p{...}`.

## What landed

1. **Parser** (`lib/Parser/RegexParser.{h,cpp}`). After `(`, a `?` followed
   by `i`, `m`, `s` or `-` starts a modifier group; both passes call one
   shared `TryParseModifierGroup` that consumes `?`, an optional add-flag
   string (`i`/`m`/`s`), an optional `-` and remove-flag string, and the
   required `:`. It rejects a flag repeated in one list or present in both
   lists, any non-flag character after the group started (`(?d:`, `(?g:`,
   `(?u:`, `(?y:`, `(?-x:`), a missing colon (`(?i)`, `(?im)`), and the
   arithmetic form with both lists empty (`(?-:)`). `(?i-:)` (empty remove)
   and `(?-m:)` (empty add) are accepted, matching the grammar and Node
   26.5.0. `(?:...)` remains the ordinary non-capturing group (the `:`
   branch is handled before the modifier scan), and `(?<name>`/lookbehind/
   lookahead branches are untouched.
   Pass 0 does **not** increment `numGroups` for a modifier group, so capture
   numbering and `\k` resolution agree across passes. Pass 1 wraps the
   modified disjunction in a transparent `ModifierNode` (below), after
   saving and restoring the effective flag state.
2. **Local flag state**. `multilineFlagPresent` joins the existing
   `caseInsensitiveFlagPresent`/`dotAllFlagPresent` parse-time state. A
   modifier group applies `UpdateModifiers` to that state only while its body
   is parsed:
   - `i` changes literal/escape case folding, character-class folding,
     `\w`/`\W` (`SetWordIUChars` for u+i), backreference matching and `\b`/`\B`
     word classification;
   - `s` selects the dot set;
   - `m` is recorded on each `^`/`$` node for the compiler.
   Nested groups update the current state, so an inner remove can override an
   outer add (and vice versa); restoring the saved state on `)` gives sibling
   and outer-alternative isolation. Top-level flags remain the base and are
   still the only values reported by `.ignoreCase`/`.multiline`/`.dotAll`
   (this build has no `.flags` accessor).
3. **Per-node state** (`lib/Parser/RegexCompileTime.{h,cpp}`). Because literal,
   character, set, backreference and word-boundary semantics are decided in
   the compiler/matcher, those nodes gained the effective state at their
   source position: `MatchLiteralNode`/`MatchCharNode`/`MatchSetNode`
   `isCaseInsensitive`, `MatchGroupNode`/`MatchNamedGroupNode`
   `isCaseInsensitive`, `WordBoundaryNode` `isWordIU`, and `SimpleNode`
   `isMultiline`. The compiler's case folding and the matcher's
   `MatchBackrefText` use the node/instruction state instead of
   `Program::flags`. `ModifierNode` is semantically transparent (no
   instruction, no capture, pass-through transfer/annotate/emit/sync
   methods), but exists so its body is not merged into a surrounding
   `MatchChar`/`MatchLiteral` run whose state differs; it is also handled by
   `MirrorLookbehindBody`. `Program::hasScopedModifiers` is set when any
   modifier group is parsed and disables the whole-program special cases
   (single char, bounded word, leading/trailing spaces, `^literal`,
   octoquad/trigram), which all assume the top-level flags apply everywhere.
4. **Anchors and word boundaries**. `^`/`$` emission chooses `BOLTest`/`EOLTest`
   vs `BOITest`/`EOITest` from the node's `isMultiline` (not the program
   flag). Inside lookbehinds the existing frame-aware instructions now carry
   the local `m` (`FrameBOLTestInst`/`FrameEOLTestInst::isMultiline`) and the
   local `i∧u` state (`FrameWordBoundaryTestInst::isWordIU`), so scoped
   state remains correct in a reversed frame. `StandardChars<char16>::
   IsWordIU` mirrors the existing `wordIUStr` table (ASCII word plus U+017F
   and U+212A). Class escapes `\w`/`\W` now use the IU sets when effective
   `u+i`, matching the atom path.
   Scoped patterns also keep the sticky `y` program tag in the
   non-optimized compile branch; without this the whole-program bypass
   turned `y` into a later-offset search.
5. **No leak across alternatives**. Literal accumulation and alternative
   unioning only combine nodes with the same effective state; the
   `ModifierNode` boundary plus the per-node fields keep `(?i:a)b` from
   folding `b`, and `(?i:a)|b` from folding `b`.

## Ownership / allocation

`ModifierNode` and the added per-node booleans live in the parser's
compile-time arena (`ctAllocator`) and are not retained after compilation;
`Program::hasScopedModifiers` is a plain bool on the existing
recycler-owned Program. No new allocation domain, no retained pointer, no
GC tracer/barrier or memory-accounting change; the runtime instruction
additions are booleans inside the existing regex instruction buffer, whose
lifetime and `FreeBody` walk are unchanged (variable-length
`MatchNamedGroupInst` strides use `sizeof` including the new bool).

## Tests and gates

- `test/es7/regexp-modifiers.js` (14 tests, default launch,
  `-args summary -endargs`, registered in `test/es7/rlexe.xml`): syntax
  accept/reject matrices (all accepted forms, empty add/remove lists,
  duplicates in/across lists, missing colon, `d/g/u/y/I`, invalid remove
  letters, nested/unterminated shapes), non-capturing/numbering and named
  groups, add/remove `i` on literals/classes/escapes/backreferences and
  u+i word classes/boundaries, add/remove `s`, add/remove `m` with anchors
  inside and outside, property reporting, nested add/remove, sibling and
  outer-alternative isolation, quantifiers on/inside groups, lookahead and
  lookbehind bodies (including frame-aware anchors and mirroring), and
  literal/constructor/compile parity. Every assertion was mirrored in Node
  26.5.0 first (14/14 pass in the Node shim), then on both `ch` binaries.
- Corpus (pinned test262, `-nonative -Test262 -CollectGarbage`):
  `test/built-ins/RegExp/regexp-modifiers/**/*.js` plus
  `test/language/literals/regexp/*modifiers*.js` = 306 scenarios.
  Before: **166 passed / 140 failed** (`out/test262-s9-modifiers-before`).
  After: **298 passed / 8 failed** (`out/test262-s9-modifiers-final2`, also
  298/8 on `out/test262-s9-modifiers-debug-final2`). Per-scenario
  join: **132 fail→pass, 0 pass→fail**; all **166 negative** scenarios in the scoped
  corpus (83 files × default/strict) stay green, none flipped. Separately,
  the 154 **top-level** negative modifier scenarios
  (`test/built-ins/RegExp/*modifiers*.js`, 77 files × 2 modes) still pass
  154/0 (`out/test262-s9-modifiers-top-final`), so no
  invalid form became accepted. The 8 residue are
  `add/remove-ignoreCase-affects-slash-{lower,upper}-p` (2 scenarios
  each): `\p{...}` property escapes, deferred to S3, unrelated to scoping.
- **Node 26.5.0 oracle**: a 55-probe script covering add/remove/nested
  `i`/`m`/`s`, sibling and outer isolation, quantifiers, backreferences,
  u+i word classes/boundaries, frame-aware lookbehind anchors, property
  values, constructor/compile/toString and 13 malformed forms was run on
  both engines; outputs are byte-identical (`ORACLE MATCH`).
- Gates moved 1977/2029 → **1978/2030** in
  `tools/build/check_regression.py` (docstring, `EXPECTED_PASSED`,
  `SELECT_COUNTS`, exact-baseline and debug self-test pins),
  `build.zig` (both profiles), all three `.github/workflows/zig-macos.yml`
  legs, and the live baseline sentence in `doc/memory-management.md`.
  `mise exec -- zig build check --summary line`: 572/572 steps,
  `Total: passed 1978, failed 0`; debug-static
  `check-regression`: 545/545 steps, `Total: passed 2030, failed 0`.
  `zig build verify-bytecode`: `source headers unchanged; regen exit 0`.
  Final provenance pins: launcher `zig-out/bin/ch` sha256 `3d1bd3db…`,
  engine `zig-out/lib/libChakraCore.dylib` `bff955cd…`, debug-static
  `out/debug-static/bin/ch` `898e4683…`.

## Limits / residual

- `v` flag parsing and `\p{...}`/`\P{...}` property escapes are **not**
  implemented; the 8 remaining scoped-corpus failures need S3. Modifier `i`
  around a property escape therefore cannot be exercised yet.
- The grammar is implemented in the regex parser, not the scanner: a regex
  literal remains scanned as before, and `/.../` flag strings still reject
  `(?` syntax as they did. No config gate exists.
- `d`/`u`/`g`/`y` are not modifier flags; `u` remains global-only (the
  grammar's modifier alphabet is exactly `i`, `m`, `s`).
- Pre-existing group-boundary surrogate handling remains: in u mode a
  high surrogate inside a group and a low surrogate outside can still be
  treated as a pair by the parser/matcher (`(?i:\uD801)\uDC00` and the
  unmodified `(?:\uD801)\uDC00` match an astral input in release; the
  debug parser asserts at the group close). This is the inherited u-mode
  surrogate/dot lacunarity, not modifier state; it is not claimed fixed.
- The shared u-mode astral dot/class lacunarity remains (`(?s:..)` u and
  top-level `/../su` match a surrogate pair as two code units; a negated
  astral class can match a lone surrogate). This predates modifiers and is
  outside this batch.
- `.flags` is absent in this build; `.ignoreCase`/`.multiline`/`.dotAll`
  are the observable top-level flag properties.
- No JS-bytecode or PropertyId change; the special-case compiler paths are
  simply skipped for patterns containing any modifier group (a performance
  choice, not a semantic gate).
- Pre-existing, untouched: the trigram/octoquad and other whole-program
  paths are unavailable for modified patterns; `String`/`RegExp` symbol
  surfaces remain behind `ES6RegExSymbols`.

## JIT (B4)

All evidence is Mac no-JIT interpreter. The change adds regex-internal
instruction fields (`FrameBOLTestInst`/`FrameEOLTestInst` carry a bool,
`WordBoundaryTestInst`/`FrameWordBoundaryTestInst` carry a bool,
`MatchGroupInst`/`MatchNamedGroupInst` carry a bool; the named-group
instruction's inline `int` ids still follow the header) and no JS bytecode.
B4 must (a) run `regexp-modifiers.js` and the modifier corpus under
SimpleJIT/FullJIT, and (b) re-audit any JIT-inlined String/RegExp helper that
inlines the instruction stream or assumes the old instruction sizes; the
regex matcher itself remains the interpreter path.
