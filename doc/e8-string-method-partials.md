//-------------------------------------------------------------------------------------------------------
// Copyright (C) Microsoft. All rights reserved.
// Copyright (c) ChakraCore Project Contributors. All rights reserved.
// Licensed under the MIT license. See LICENSE.txt file in the project root for full license information.
//-------------------------------------------------------------------------------------------------------

# E8 — String/RegExp-method partials batch 1 (matchAll 30, replaceAll 28, match-indices 26)

Published 2026-11-04 by the `b7-partials-1` lane on `modernize/baseline`
(base `d8cd003a0`, clean tree). Scope: the three adjacent C4 §5c partials
**String.prototype.matchAll 30**, **String.prototype.replaceAll 28**,
**regexp-match-indices 26** — fresh triage, family classification against
Node v26.5.0 + spec, bounded fixes where any exist, cards for what the
recorded flag decisions own.

**Headline: all 84 scenarios are solely the recorded flag substrate.**
With the corresponding flag on, each of the three selections is **100 %
green** — 50/50, 92/92, 84/84 — so there is no bounded defect under the
flag either; nothing was fixable without crossing a recorded decision,
and **zero engine changes landed**. The receipts' residual fears
(S11's BigInt parse pair, S7's named-groups/species residuals) have all
been closed by later lanes; the numbers did not drift (30/28/26 exactly
reproduce).

## 1. Fresh triage (before == after; same binary, zero changes)

Binary `zig-out/bin/ch` SHA-256 `5e210836…7028`, corpus `2e0a567…`
(pinned), runner defaults `-nonative -Test262 -CollectGarbage
-NegativePhaseReport`, controls 25/25 on every run. Scenario-level
artifact: [`tools/test262/b7-string-method-partials-triage.json`](../tools/test262/b7-string-method-partials-triage.json).

| Partial | Selection | Scenarios | Pass | Fail | Tag-selected fail (C4) | Drift |
|---|---|---:|---:|---:|---:|---|
| matchAll | `String/prototype/matchAll/**` | 50 | 4 | 46 | **30** (`String.prototype.matchAll` tag) | none |
| replaceAll | `String/prototype/replaceAll/**` + `RegExp/named-groups/duplicate-names-replaceall.js` | 92 | 64 | 28 | **28** (tag) | none vs C4; S11's 30 → 28 because `ESBigInt` default-on now parses `cstm-replaceall-on-bigint-primitive` |
| match-indices | tag universe: `RegExp/match-indices/**`, `RegExp/prototype/hasIndices/**`, `RegExp/prototype/flags/*.js` + 4 stray files | 84 | 36 | 48 | **26** (`regexp-match-indices` tag; 36 tagged passes, identical to C4) | none |

## 2. Family classification

### matchAll — 30/30: method absent (one family)

`String.prototype.matchAll` is installed only inside
`InitializeStringPrototype`'s `IsES6RegExSymbolsEnabled()` block
(`JavascriptLibrary.cpp`, `AddFunctionToLibraryObjectWithPropertyName(
stringPrototype, _u("matchAll"), …)`). All 15 failing files fail on the
missing method (`Object doesn't support property or method 'matchAll'` /
`SameValue("undefined","function")`), including the four pure-string-path
behavioral files (`regexp-is-null`, `regexp-is-undefined`,
`regexp-is-undefined-or-null-invokes-matchAll`, `toString-this-val`) —
every spec path terminates in `Invoke(rx, @@matchAll, «S»)` (ES2025
22.1.3.7 step 5) or `GetMethod(regexp, @@matchAll)` (step 2.c), so the
method cannot exist spec-conformantly without the `@@matchAll` machinery.
**Card E8-1.**

Vacuous passes recorded: `flags-nonglobal-throws.js` and
`flags-undefined-throws.js` (4 scenarios) pass only because the missing
method throws the `TypeError` the test expects for a different reason;
they must be re-witnessed at any flag flip (they pass flag-on, §3).

### replaceAll — 28/28: every failing file is written through the well-known symbols (one family)

14 files × 2 scenarios, three observable shapes, one root cause — the
`Symbol.match`/`Symbol.replace` well-known symbols themselves are
`nullptr` flag-off (`InitializeGlobal`, `symbolMatch/symbolReplace =
nullptr` in the `else` branch), so the tests' computed keys
(`[Symbol.match]: true`, `get [Symbol.match]()`, `[Symbol.replace]()`,
`Object.defineProperty(customRE, Symbol.replace, {value: undefined})`)
define the **string key `"undefined"`** and never reach the algorithm:

- *flags-check family* (8 scenarios): `searchValue-flags-{null-undefined,
  toString-abrupt,get-flags}-throws`, `searchValue-isRegExp-abrupt` —
  need `IsRegExp` to consult `@@match` (step 2.a) before the flags Get.
- *dispatch family* (14 scenarios): `searchValue-replacer-{call,
  call-abrupt,method-abrupt,RegExp-call,RegExp-call-fn}`,
  `searchValue-replacer-before-tostring`, `searchValue-tostring-regexp` —
  need `GetMethod(searchValue, @@replace)` (step 2.c) before the string
  path; flag-off the engine's documented deviation runs `RegexReplace`/
  the string path instead (`JavascriptString.cpp` `EntryReplaceAll`, the
  `IsES6RegExSymbolsEnabled()` else-branch).
- *own-`@@replace:undefined` family* (6 scenarios): the
  `getSubstitution-0x0024-0x003C/0x0024N/0x0024NN` finals — a global
  regexp with own `@@replace = undefined` must fall through
  `GetMethod` to the **string path** (`ToString(rx) = "/./g"`), e.g.
  `'--- /./g ---'.replaceAll(customRE, 'a($<$<)')` → `'--- a($<$<) ---'`.
  All their string-path `$`-substitution samples **pass** today; only the
  symbol-shaped final sample fails. With `Symbol.replace` nonexistent the
  property the test defines is `"undefined"`, unrecoverable.

**Card E8-2** (same recorded decision as E8-1: C4 §5b `-ES6RegExSymbols`,
558 owed; creating the well-known symbols by default would observably
change `IsRegExp` everywhere and eat into the recorded bucket without the
JIT re-check the decision owns — substrate, not fix).

### match-indices — 26/26: `flags`/`hasIndices` accessor substrate (one family)

Every tagged failure reads one of the two prototype accessors that
`-ES6RegExPrototypeProperties` installs (`InitializeRegexPrototype`):
`hasIndices/{cross-realm,length,name,prop-desc,this-val-{regexp,
regexp-prototype,invalid-obj,non-obj}}` ×16 and
`flags/{coercion-hasIndices,get-order,rethrow,return-order,
this-val-regexp}` ×10 (`RegExp.prototype.flags` getter, descriptor
`undefined` flag-off). The **d-flag runtime itself is green**: all 28
`RegExp/match-indices/**` scenarios plus `duplicate-flags`,
`duplicate-names-match-indices` and both duplicate-named-indices files
pass on default flags (the 36 tagged passes, identical to C4).
**Card E8-3** (C4 §5b `-ES6RegExPrototypeProperties`, ≥156 rfm).

## 3. Flag-on ceilings (the datum this lane adds to c4-gate §3.1(b))

Same binary, one flag added per run; these quantify exactly what each
recorded decision still owes and prove no second defect hides underneath:

| Selection | Flag | Result |
|---|---|---|
| matchAll 50 scenarios | `-ES6RegExSymbols` | **50/0** (S7's in-selection residual 8 → 0: BigInt and named-groups/v-flag legs closed by later lanes; review-corrected: the 4 species-get scenarios are OUTSIDE this selection and still fail 0/4 under symbols-only — they close via the proto-props substrate, verified 4/4 two-flag) |
| replaceAll 92 scenarios | `-ES6RegExSymbols` | **92/0** (S11's residual 4 → 0) |
| match-indices 84 scenarios | `-ES6RegExPrototypeProperties` | **84/0** |

For the C4 path to green: flipping the two RegExp substrate flags alone
zeros all three partials (84 edition failures) with **no flag-on repair
work** in these directories; the remaining owed work is the recorded
`ConfigFlagsList.h:1177` JIT-inlined String/RegExp helper re-check and
the B4/JIT-corpus obligations, exactly as c4-gate §3.1(b) states.

## 4. Node v26.5.0 + spec differentials on the green surfaces

Adversarial batteries beyond the corpus on the surfaces that ARE
default-enabled (or flag-on for matchAll), all output-identical to Node
unless noted:

- **d-flag indices structure** (15 shapes): fresh pair objects per exec,
  `indices`/`groups` descriptor attributes `{w,e,c} = {true,true,true}`,
  `groups: undefined` without named groups, unmatched-group `undefined`
  entries, `lastIndex` past end, sticky, `u`-flag code-unit (not
  code-point) pair positions, `String.prototype.match` with `g`
  (strings, no indices), function-replacer `groups` argument, `split`/
  `search` neutrality, `indices` prototype, `delete indices.groups`.
  Byte-identical ch(default) vs Node.
- **replaceAll paths** (31 shapes): the full `$`-substitution table on
  the string path (`$$`, `$&`, `` $` ``, `$'`, bare/unclosed `$<`,
  `$<x>` without named captures, `$0`/`$01`/`$9`/`$99` out-of-range,
  trailing `$`), functional replacer arity/`this`/empty-match advance,
  `ToString` coercion order (searchValue before replaceValue),
  `RequireObjectCoercible` receivers, `Symbol.toPrimitive` on both
  receiver and replacer, flags shapes (`G` uppercase, absent-with-g,
  object-with-`toString`), descriptors, `new`/null-this throws.
  Identical to Node **except one Node-side defect**: Node v26.5.0
  **OOM-crashes** on `'aa'.replaceAll(r, 'z')` with `r = /a/` and own
  `flags = {toString(){return 'g'}}` — spec (`RegExp.prototype
  [@@replace]`, `global` = `Get(rx,"global")` = false) mandates a single
  replace → `"za"`; ch returns `"za"`. (Second Node-side oddity: ch's
  uncaught-error formatting differs; behavior classes identical.)
- **matchAll flag-on** (15 shapes): iterator protocol basics,
  `undefined`/primitive searchValue fall-through (empty regex),
  sticky+global, empty-match advance incl. surrogate pairs,
  `lastIndex` cached at `@@matchAll` time, `exec` re-read per `next`
  (on the species clone), `@@toStringTag`/ancestry, non-object exec
  return validation, receiver coercion. Byte-identical ch
  (`-ES6RegExSymbols`) vs Node — the S7 `AdvanceStringIndex`
  surrogate TODO stays a recorded card with no corpus or probe witness.

## 5. Fixes

**None.** Triage found zero bounded defects: no failing scenario in any
of the three partials is blocked by anything other than the recorded
flag substrate, and each selection is fully green with its flag on. No
engine, test, or gate file changed in this lane; the gates and
selections are therefore identical before/after by binary identity
(SHA-256 above). The RegExp campaign batches (s9/s3a/s3b) were **not**
re-run because no engine RegExp path moved — the standing trigger
("if engine RegExp paths move") does not fire on a zero-change lane.

## 6. Residual cards

| Card | Scenarios | Substrate / decision | Owner |
|---|---:|---|---|
| E8-1 `String.prototype.matchAll` absent by default (with `@@matchAll` machinery, `Symbol.matchAll`, RegExp String Iterator) | 30 | `-ES6RegExSymbols` (C4 §5b, 558; S7 receipt) | recorded decision + JIT re-check, then flag flip |
| E8-2 replaceAll `Symbol.match`/`Symbol.replace` well-known symbols `nullptr` flag-off (flags-check, dispatch, own-`@@replace:undefined` shapes) | 28 | same | same |
| E8-3 `RegExp.prototype.flags`/`hasIndices` accessors absent | 26 | `-ES6RegExPrototypeProperties` (C4 §5b, ≥156 rfm; S9 sub-batch 2) | same |
| E8-4 (carried, unchanged) S7 `AdvanceStringIndex` +1-per-code-unit for `u` regexes; S11 absent-`flags` internal-bit deviation | 0 corpus | pre-existing recorded debt; no corpus or probe witness today | S9/S7 stream |
| E8-5 vacuous passes: matchAll `flags-nonglobal-throws`, `flags-undefined-throws` pass flag-off by missing-method TypeError | 4 passing | re-witness at any flag flip (flag-on they pass: §3) | flag-flip lane checklist |

Expected **C4 delta: 0** (measurement lane; the 84 stay in the recorded
disabled-flag/rfm buckets they already occupy).

## 7. Commands and evidence

- Triage (before): `run.py --output out/test262-b7-{matchall,replaceall,indices}-before` — 50 (4/46), 92 (64/28), 84 (36/48); controls 25/25 each.
- Ceilings: `--host-flags '… -ES6RegExSymbols'` → `out/test262-b7-{matchall,replaceall}-symbols` (50/0, 92/0); `… -ES6RegExPrototypeProperties'` → `out/test262-b7-indices-protoprops` (84/0).
- Focused rlexe, both binaries, rlexe-registered flag sets: `replaceall`, `s11-builtins`, `regexp-indices` (default), `matchall`, `replaceall-symbols`, `regexp-indices-symbols` (`-ES6RegExSymbols`) — all print `pass`.
- Node differentials: the three batteries of §4 (scripts in the run log; two Node-side defects found, ch spec-correct on both).
- Gates (unchanged tree, same binaries): `zig build check` regression **2048/0**, verify-bytecode "source headers unchanged", native/smoke/metadata PASS; `zig build check-regression -Dprofile=debug-static` **2100/0**. No test landed; pin delta N/A.
- `git status` clean before and after; only this receipt and the triage artifact are committed.

### Review outcome (ACCEPT; corrections folded)

Single-review certification (zero-change lane; classification risk
covered by spot-reading). All six measured numbers, the tag counts,
ceilings, controls, and the zero-change surface reproduced exactly on
ONE current binary. Folded: (1) the species-legs parenthetical
corrected (they need proto-props, not symbols — verified 4/4 two-flag);
(2) binary bookkeeping: the before/ceilings halves ran on two builds
(`30c7e1ec`/`5e210836` — the named binary no longer exists); the
review's single-binary reproduction discharges the data, but triage
lanes must name both builds or explain the rebuild — recorded as the
standard here; (3) artifact label (matchIndices failing_files lists
the 13 TAGGED files; 24 files fail incl. 11 same-substrate untagged
`flags/*`); (4) "two Node-side defects" — defect 2 is a formatting
difference, not a behavioral defect (corrected); (5) matchAll-30 is a
manifest §5b disabled-flag entry (inside the 558), not §5c; (6) "15
failing files" → 23 (15 = the tagged subset).
