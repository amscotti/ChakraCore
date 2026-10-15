//-------------------------------------------------------------------------------------------------------
// Copyright (C) Microsoft. All rights reserved.
// Copyright (c) ChakraCore Project Contributors. All rights reserved.
// Licensed under the MIT license. See LICENSE.txt file in the project root for full license information.
//-------------------------------------------------------------------------------------------------------

# C2-S7: matchAll + the RegExp-symbol flag-on defect fixes

Status: landed on Mac no-JIT (committed as 2083551b7). All four matchAll surfaces
plus the two flag-on defect classes from
[c2-flag-audits.md](c2-flag-audits.md) §S7 are implemented behind
`ES6RegExSymbols` (still default-off; no default-flag flip in this
batch). The flags-on gates and the resym universe are green; the 42
audit regressions are 42/42 fixed with 0 new pass→fail. The four
review fixes (below) are in. JIT evidence is
not claimed; see B4 below.

## What landed

Four new surfaces, all created only when `ES6RegExSymbols` is on (the
audit measured `Symbol.matchAll` `undefined` under every flag
combination; with the flag off every one of these stays absent):

1. **Well-known symbol `Symbol.matchAll`** — `symbolMatchAll` in
   `JavascriptLibrary`, created in the same `IsES6RegExSymbolsEnabled()`
   block as `Symbol.match/replace/search/split` (nullptr with the flag
   off) and installed on the Symbol constructor next to `match` under
   the same guard. New built-in ids `_symbolMatchAll`
   ("Symbol.matchAll"), `_RuntimeFunctionNameId_matchAll`
   ("[Symbol.matchAll]") and `matchAll` were **appended at the end of
   `JnDirectFields.h`**: appending keeps every pre-existing built-in
   PropertyId stable, and those ids are serialized into the checked-in
   InJavascript bytecode headers. (An earlier draft of this batch
   inserted the entries mid-file; that shifted the compiler-internal
   `_super`/`_newTarget` ids and corrupted array join at runtime —
   the library bytecode is property-id-sensitive, exactly as the file's
   header comment warns.)
2. **`RegExp.prototype[Symbol.matchAll]`** (length 1, name
   "[Symbol.matchAll]", `ErrorOnNew`), installed in the same symbols
   block of `InitializeRegexPrototype`. Final-spec formulation, derived
   from the pinned corpus and cross-checked against Node 26.5.0: the
   this value must be an Object; `ToString(string)`;
   `SpeciesConstructor(R, %RegExp%)`; `ToString(Get(R, "flags"))`;
   `Construct(C, «R, flags»)`; global/fullUnicode derived from the
   **flags string** (the matcher's `global`/`unicode` properties are
   never read — corpus `species-regexp-get-{global,unicode}-throws`
   pins this); `ToLength(Get(R, "lastIndex"))` transferred to the
   matcher through the observable `[[Set]]`. R's `@@match` is read only
   inside `Construct` (corpus `isregexp-called-once`: exactly once,
   after the flags get/toString).
3. **`%RegExpStringIteratorPrototype%`** (prototype `%IteratorPrototype%`)
   with `next` (length 0) and `@@toStringTag` "RegExp String Iterator"
   (writable false, enumerable false, configurable true). The iterator
   object is a new class `JavascriptRegExpStringIterator` (TypeId 59,
   the long-deleted slot, reclaimed the same way S5 reclaimed 69 for
   `IteratorHelper`) holding the matcher, the string, global,
   fullUnicode and done slots — modelled on `JavascriptStringIterator`,
   living in `JavascriptRegularExpression.{h,cpp}` so no new translation
   units had to be added to the four build source tables. `next`
   re-Gets `exec` on every call (`CallExec`), validates the
   Object-or-Null return, marks done on null / non-global success, and
   for a global matcher's **empty** matches does
   `ToLength(Get(R,"lastIndex"))` → `AdvanceStringIndex` (code-unit +1
   today) → observable `Set(R,"lastIndex",nextIndex,true)`. No capture
   extraction or named-groups hook: `next` returns the exec result
   object itself (S9 debt is the surrogate-pair AdvanceStringIndex).
4. **`String.prototype.matchAll`** (length 1, name "matchAll",
   `ErrorOnNew`), installed with
   `AddFunctionToLibraryObjectWithPropertyName` like `replaceAll`.
   ES2025 algorithm: RequireObjectCoercable this; if regexp is neither
   undefined nor null: `IsRegExp` → flags Get +
   RequireObjectCoercible + ToString "g" check; `GetMethod(regexp,
   @@matchAll)` **only when regexp is an Object** (2025 primitive
   guard); on fall-through `RegExpCreate(regexp, "g")` — the value goes
   to the RegExp constructor as-is, so `undefined` yields the empty
   regex (`"a".matchAll(undefined)` iterates empty matches) and a
   regexp argument is cloned under the new flags (a same-object
   `CreateRegEx` fast path would keep an own `@@matchAll = undefined`
   shadowing the method Invoke is about to call) — then
   `Invoke(rx, @@matchAll, «S»)` through the real property.

## Review fixes — three flag-on behavioral bugs + a gated groups lookup

Four verified findings from the review of this batch, all Node-checked
(Node 26.5.0), all inside `ES6RegExSymbols` or behavior-neutral when it
is off:

1. **`@@search` set `lastIndex` unconditionally.** Spec: the reset
   `Set(rx, +0)` runs only `If SameValue(previousLastIndex, +0𝔽) is
   false`, and the post-exec restore `Set(rx, previousLastIndex)` only
   when the current value differs. `EntrySymbolSearch` now guards both
   Sets with `JavascriptConversion::SameValue`, so a non-writable
   `lastIndex` of +0 succeeds (`"bab".search(/a/)` → 1, lastIndex
   untouched) while a non-writable value ≠ 0 still throws on the reset.
   Both entry points are installed only under the flag. Corpus effect:
   `RegExp/prototype/Symbol.search/lastindex-no-restore.js` (2
   scenarios) fail→pass.
2. **`@@matchAll` clamped the transferred lastIndex.** The old code read
   `GetLastIndexProperty` (clamps to `MaxCharCount` = 2147483646) before
   the observable `Set(matcher, "lastIndex", …)`. The transfer now uses
   the raw `ToLength(Get(R, "lastIndex"))` int64 via
   `JavascriptNumber::ToVar(int64)`: a species setter observing a
   `1e300` lastIndex sees `9007199254740991` (Node-matching), and only
   the engine's internal consumers keep clamping.
3. **`GetInitialOffset`'s early-out wrote `lastIndex` internally.** When
   the cached lastIndex clamps above `MaxCharCount` (so it exceeds any
   input length), the reset for global/sticky regexps now goes through
   `SetLastIndexProperty` under the flag — observable, throws on a
   non-writable `lastIndex` (Node: `Set(R, "lastIndex", 0, true)`) —
   while flag-off keeps the byte-identical internal `SetLastIndex(0)`.
   The in-range `lastIndex > length` case was already observable through
   defect fix 2's `PropagateLastMatchToRegex`, and non-global
   non-sticky regexps never touch `lastIndex` at all (verified: exec
   with a non-writable lastIndex of 3 returns the match and keeps 3).
   `GetInitialOffset` gained a `ScriptContext*` parameter (3 call
   sites: exec, ES5 test, NonScript test).
4. **The `groups` lookup is flag-gated.** `RegexEs6ReplaceImpl` only
   computes `GetOrAddPropertyIdTracked("groups")` (and only runs the
   ES2021 Get + ToObject coercion) when `IsES6RegExSymbolsEnabled()`;
   the id is `Constants::NoProperty` otherwise, so no per-call
   property-id lookup or Get can occur on any flag-off path that
   reaches the impl (flag-off replace/replaceAll route to
   `RegexEs5ReplaceImpl`, and `@@replace` itself is flag-installed).
   Behavior unchanged.

Flag-off byte-identity re-proven with a 10-shape spot battery (exec
global/non-global/sticky with lastIndex beyond length and 1e300, test,
search, match, replace string+function, symbol surfaces absent, legacy
`defineProperty` lastIndex no-op) — stdout+stderr byte-identical to
`build-legacy-debug/ch`.

## Defect fix 1 — primitive `Symbol.*` reads (24 scenarios)

`JavascriptString::DelegateToRegExSymbolFunction` (the shared delegator
of `String.prototype.{match,replace,search,split}`) now looks up the
`Symbol.*` function only when the search value `IsObject`. A primitive
never consults `String.prototype`'s (or the other primitive
prototypes') `Symbol.*` property, so the logged getters in
`cstm-*-on-{string,number,boolean}-primitive.js` no longer run; the
primitives fall through to the same string paths `replaceAll` uses.
Before/after (audit artifact `out/test262-audit-resym` →
`out/test262-s7-resym`, 6,199 scenarios, `-ES6RegExSymbols`): 24/24
fail→pass.

## Defect fix 2 — lastIndex `[[Set]]` bypass (16 scenarios)

Two layers were broken:

- **The write path.** `RegexHelper::PropagateLastMatchToRegex` wrote
  `lastIndex` through the internal field (`SetLastIndex`), so a failed
  match under global/sticky reset it without an observable `[[Set]]`.
  Under `ES6RegExSymbols` both the failure reset (global or sticky
  only; non-global non-sticky failures no longer touch `lastIndex`,
  matching the ES2018+ `RegExpBuiltinExec`) and the success advance now
  go through `JavascriptRegExp::SetLastIndexProperty` (the ES6 paths'
  existing observable Set, `ThrowIfNonWritable`). With the flag off the
  writes are byte-for-byte the old internal ones.
- **The property layer.** `lastIndex` is a fully virtual own property
  (`HasPropertyQuery`/`GetPropertyBuiltIns`/`SetPropertyBuiltIns`
  answer for it; there is no type-handler slot), so
  `Object.defineProperty(r, "lastIndex", { writable: false })` was a
  silent no-op — `IsWritable(lastIndex)` hard-coded `true`, which is
  why no Set could ever throw. Under the flag a new
  `lastIndexNonWritable` field records the redefine
  (`SetWritable` override), `IsWritable` reports it, and
  `SetPropertyBuiltIns` fails/throws the assignment
  (`ThrowCantAssign` for engine Sets with `ThrowIfNonWritable`,
  strict-mode-only for user assignments; sloppy user assignment is a
  silent no-op like Node). The descriptor then reads
  `{writable: false}` and a `writable: true` re-redefine is rejected
  (non-configurable), both Node-matching. Flag off: the legacy no-op
  and the always-writable descriptor are unchanged.

Before/after: all 16 audit scenarios (7 `Symbol.match` lastIndex files
+ `Symbol.replace/y-fail-lastindex-no-write`, default+strict)
fail→pass. `g-match-empty-set-lastindex-err` also needed the iterator's
observable empty-match Set (surface 3) plus the observable exec write.

## The 2 groups-coercion scenarios (included)

`Symbol.replace/result-coerce-groups-err.js` needed the ES2021
`Get(result, "groups")` + `ToObject` when not undefined, after the
captures loop. There is no `PropertyIds::groups` (named groups are S9),
so the id is a tracked dynamic property id — computed only when
`IsES6RegExSymbolsEnabled()` (review fix 4), so no flag-off path pays
the lookup or the per-result Get. The coerced value itself
only feeds `$<name>` substitution (S9), but the coercion is observable
and now runs and throws for `groups: null`. Both scenarios pass — the
audit's "record as S9-adjacent debt" fallback was not needed.

## Flag matrix deviations (symbols without proto properties)

With `ES6RegExSymbols` on but `ES6RegExPrototypeProperties` off,
`RegExp.prototype.flags` does not exist, so spec-literal
`Get(R, "flags")` would be `undefined` and every real-regexp matchAll
would construct `new RegExp(R, "undefined")`. Following the
`replaceAll` precedent (doc/c2-s11.md "missing flags"), both
`@@matchAll` and `String.prototype.matchAll` fall back to the pattern's
internal flags when the value is undefined, the receiver is a real
`JavascriptRegExp`, and the property is absent from the whole prototype
chain (a non-observable `HasProperty` check). An own or inherited
`flags` value (including `-ES6RegExPrototypeProperties`) is still
`Get`'d and `ToString`'d. Same for the legacy own-instance
`global`/`unicode`/`source` built-ins: `Object.defineProperty(r,
'global', …)` on a pristine regexp throws "Cannot redefine
non-configurable property" until the proto flag removes them, which is
why `species-regexp-get-{global,unicode}-throws` stays red under
symbols-only (see the corpus table below).

## Test

`test/es7/matchall.js`, registered once in `test/es7/rlexe.xml` with
`-ES6RegExSymbols -args summary -endargs` (mirroring
`replaceall-symbols.js`; the default launch never runs it). It covers:
installation/descriptors/length/name of all four surfaces, the iterator
protocol (global, non-global, sticky, empty-match advance, done
stickiness, custom exec per-next, exec return validation), species /
flags-string / own-flags / cached-lastIndex semantics, the
matcher-global-never-read shape, brand checks, `@@toStringTag`,
ancestry, the String.prototype protocol (null/undefined/primitive
regexp, custom matcher, flags-g and RequireObjectCoercible throws,
@@matchAll dispatch and prototype restoration), primitive
`Symbol.*` non-reads with logged getters (including `@@matchAll`
itself), the defect-2 shapes (non-writable lastIndex throws on
`@@match`/`@@replace` failure resets and on the iterator's exec/advance
Sets, sloppy-assign no-op, non-reconfigurable), the
groups-coercion throw, and the three review-fix cases: `@@search`
SameValue-guarded Sets (+0 non-writable succeeds, value≠0 throws,
writable restore), the unclamped `1e300` → `9007199254740991`
transfer observed by a species setter, and exec with lastIndex beyond
the string length (non-global untouched; global/sticky reset
observably, throwing on non-writable; the `MaxCharCount` clamp
early-out included). 22 tests total. One rlexe file: gates 1967→1968 (test) and
2019→2020 (debug-static), bumped in `tools/build/check_regression.py`
(docstring, `EXPECTED_PASSED`, `SELECT_COUNTS`, exact-baseline and
debug self-test pins; the deliberate stale 2014/1962 fixtures stay),
`build.zig` (both steps), `.github/workflows/zig-macos.yml` (test,
debug-static, legacy cmake legs), and the live baseline sentence in
`doc/memory-management.md`.

## Property ids, bytecode, TypeId

- Three new built-in PropertyIds **appended at the end** of
  `JnDirectFields.h`: no pre-existing id moves, but `_countJSOnlyProperty`
  itself grows, and the library bytecode serializer treats
  ids ≥ the old count as dynamic-with-name — so the checked-in
  `JsBuiltIn.nojit.bc.{64b,32b}.h` had to be regenerated
  (`tools/regenByteCode.py --skip-build --binary zig-out/bin/ch --noJit`);
  `zig build check`'s `verify-bytecode` then reports
  "source headers unchanged; regen exit 0". The **jit-variant headers
  (`JsBuiltIn.bc.*.h`) are NOT regenerated** (no JIT build on this Mac
  no-JIT toolchain; nothing on this platform loads them); B4 must
  regenerate both variants before any JIT claim.
- TypeId 59 (`TypeIds_RegExpStringIterator`) reclaims the deleted slot
  next to S5's TypeId 69 reclaim; the class is registered in
  `JavascriptOperators::IsJsNativeType` and `JsrtDebugUtils` exactly
  where `IteratorHelper` is.

## test262 (pinned corpus 2e0a567…)

MatchAll corpus (four directories, 140 scenarios, 70 files),
`--host-flags='-nonative -Test262 -CollectGarbage -ES6RegExSymbols'`,
artifact `out/test262-s7-matchall`:

| Run | Passed | Failed |
| --- | --- | --- |
| audit (c) baseline (2 surfaces dirs) | 2 | 36 |
| this batch (4 dirs) | **132** | 8 |

The 8 residual scenarios: `cstm-matchall-on-bigint-primitive` ×2
(`ESBigInt` parse), `regexp-prototype-matchAll-v-u-flag` ×2 (`v` flag =
S3 property escapes; SyntaxError), and
`species-regexp-get-{global,unicode}-throws` ×4 — the legacy
own-instance `global`/`unicode` built-ins make the tests'
`Object.defineProperty(r, 'global', {get(){throw}})` itself throw
"Cannot redefine non-configurable"; fixed by the proto-properties
substrate (audit (b)), not by matchAll. **No named-groups-dependent
failures remain** in these directories (S9 hook debt is only the
surrogate-pair `AdvanceStringIndex`).

Resym universe (`test/built-ins/{RegExp,String}/**/*.js`, 6,199
scenarios, `-ES6RegExSymbols`), artifact `out/test262-s7-resym` vs the
audit's same-binary artifacts:

| Comparison | fail→pass | pass→fail |
| --- | --- | --- |
| vs audit flag-on run (`test262-audit-resym`) | 136 | **0** |
| the 42 audit regressions (vs `…-base`) | **42 fixed, 0 remain** | — |
| vs audit default-flags base | — | **0** |

Net pass count 4,285 → 4,421 on the universe (the review fixes added
`Symbol.search/lastindex-no-restore.js` ×2 to the batch's original
134). The 136 movers decompose as: the 42 audit regressions, ~84
matchAll-surface and search-guard scenarios (76 in-set
`Symbol.matchAll` + iterator/String dispatch, 2
`lastindex-no-restore`), plus the lastIndex observability now honored
in the ES5 fast paths under the flag. Classification of the remaining
~1,778 universe failures is unchanged from the audit (S3 `\p{…}`
escapes, S9 named groups, BigInt spillover, `hasIndices`/`unicodeSets`,
flags/unicode getter-order under the missing proto substrate).

## Node 26.5.0 cross-checks

Probed equal on both engines: iterator `@@toStringTag`/ancestry,
global/non-global/sticky/empty-match iteration orders and lastIndex
values, flags-string species args, matcher `global` never read,
lastIndex cached at @@matchAll time, non-regexp this reaching
`ToString` inside Construct, primitive/null/undefined
`String.prototype.matchAll` fall-through (including the undefined→empty
regex), non-writable lastIndex descriptors, silent sloppy assignment,
strict assignment throw, and the `writable: true` re-redefine
rejection. Message texts differ (ch: "invalid argument"/"Unable to get
property…" vs Node's phrasing); the corpus checks error types only.

## Ownership

No new allocation domain. The iterator is a recycler object
(`RecyclerNew` in `CreateRegExpStringIterator`) holding traced
`RecyclableObject*`/`JavascriptString*` fields (write-barriered
`Field(...)` members like `JavascriptStringIterator`); `@@matchAll`
allocates the flags string and the species matcher through the normal
constructor machinery; `StringFromFlags` returns a recycler string. No
temp arena, no stack-retained state, no barrier change. The
`lastIndexNonWritable` bool is a plain field copied by the boxing
constructor. Not claimed: OOM injection, `-RecyclerStress`, or a JIT
allocation profile.

## Limits

- ~~`GetInitialOffset`'s lastIndex > length early-out (`SetLastIndex(0)`
  internal) is not routed through the observable `[[Set]]`~~ — fixed by
  review fix 3 (above): global/sticky resets through
  `SetLastIndexProperty` under the flag, flag-off byte-identical.
- A `defineProperty(r, "lastIndex", { value: N, writable: false })`
  redefine under the flag records only the writability: the **value
  half is still dropped** (the virtual property has no type-handler
  slot, so `SetPropertyWithAttributes` cannot store it; `r.lastIndex =
  N` followed by `{ writable: false }` does store N). Pre-existing
  scope of defect fix 2's `SetWritable`-only honoring, left as-is in
  this batch; the review tests use the set-then-freeze shape.
- `AdvanceStringIndex` is +1 per code unit even for `u`-flag regexes
  (pre-existing `RegexHelper` TODO; empty-match iteration over
  surrogate pairs can re-match the second half).
- With the flag off, `Object.defineProperty(r,'lastIndex',{writable:false})`
  remains the legacy silent no-op (deliberate: the default gates run
  unflagged; the 1968/2020 baselines are untouched by the new paths).
- `@@matchAll`/`String.prototype.matchAll` flags fallback (above) is a
  spec deviation only observable on the symbols-without-proto flag
  matrix.

## JIT (B4)

All evidence is Mac no-JIT interpreter. No new opcode, no bytecode
emission change, no Backend helper, nothing to inline — but B4 must (a)
regenerate **both** bytecode header variants (the jit headers are stale
relative to the new PropertyIds), (b) run `test/es7/matchall.js` and
the four corpus directories under SimpleJIT/FullJIT before any
JIT-configuration claim, and (c) per the standing
`ConfigFlagsList.h:1180` warning, re-audit the JIT-inlined
String/RegExp helpers (`String.prototype.match` lowering calls
`RegexHelper::RegexMatch` directly) for implicit-call flags now that
`PropagateLastMatchToRegex` can run user setters under the flag.
`ES6RegExSymbols` stays off by default; no kill switch exists for the
new surface beyond the flag itself.

## Commands (final tree)

- `mise exec -- zig build` — exit 0.
- `zig build check --summary line` — 572/572 steps; regression
  `Total: passed 1968, failed 0` (DebuggerCommon 208/0, es7 33/0),
  `verify-bytecode: source headers unchanged; regen exit 0`, native,
  smoke, metadata PASS.
- `mise exec -- zig build check-regression -Dprofile=debug-static
  --summary line` — `Total: passed 2020, failed 0`, 545/545 steps.
- `zig-out/bin/ch -ES6RegExSymbols test/es7/matchall.js -args summary
  -endargs` and `out/debug-static/bin/ch …` — exit 0, printed `pass`
  (verbose: 22/22).
- Corpus runs and the resym join as tabulated above; join keys
  `(file, scenario)` against `out/test262-audit-resym{,-base}`; fresh
  reruns of both into /tmp reproduce 132/8 (identical residue) and the
  136/0 join.
- Flag-off spot battery (10 shapes: exec global/non-global/sticky with
  lastIndex beyond length and 1e300, test, search, match, replace
  string+function, symbol surfaces absent, legacy lastIndex
  defineProperty no-op) — stdout+stderr byte-identical between
  `zig-out/bin/ch` and `build-legacy-debug/ch`.
- Node 26.5.0 adversarial probes, all equal: `@@search` +0
  non-writable → 1, value≠0 → TypeError, writable restore;
  `@@matchAll` transfer observes `9007199254740991`; exec li>length —
  non-global untouched (3 stays 3), global/sticky throw on
  non-writable and reset to 0 when writable (both the in-range and the
  1e300 clamp shapes).
- `python3 tools/StyleChecks/check_copyright.py` on every new/changed
  file — exit 0; `mise exec -- python3 tools/build/check_regression.py
  --self-test` — ALL NEGATIVE CONTROLS BEHAVE; `git diff --check` —
  exit 0.
