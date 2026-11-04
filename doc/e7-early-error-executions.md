# E7 — Early-error `$DONOTEVALUATE` executions (C4 §2 engine-defect cluster)

Published 2026-11-04 by the `e7-early-errors` lane on `modernize/baseline`
(base `da0a72dbb`, B5 review-repairs head). Scope: the C4 manifest §2
engine-defect-on-existing cluster "early-error `$DONOTEVALUATE` executions
~70" as it lands in the two edition-relevant directories
(`test/language/statements/**`, `test/language/expressions/**`).

## 1. What the cluster is

test262 parse-negative tests place `$DONOTEVALUATE();` (harness `sta.js`,
throws `Test262Error("Test262: This statement should not be evaluated.")`)
as the first statement of a body that declares
`negative: phase: parse / type: SyntaxError`. When the engine fails to
reject the syntax at parse time it proceeds to execute, the sentinel throws,
and the harness reports
`Expected test to throw error of type SyntaxError, got Test262: This statement should not be evaluated`.
The host contract itself is sound: `run.py` re-judges every negative by the
`CH_NEGATIVE_PHASE` marker (0 phase-mismatch downgrades in either run
here), and the `pass-parse-negative-noexec` control models the shape —
these failures are the engine **parsing shapes it must reject**, never a
host/harness artifact.

## 2. Triage (fresh runs, not the C4 artifacts — they were rotated out)

Before run: `out/test262-e7-dne-before`, clean build of `da0a72dbb`,
selection `test/language/statements/**` + `test/language/expressions/**` =
39,301 scenarios (20,449 files); 40 scenarios / 27 unique files carry the
cluster signature, all edition-selected, 0 phase-mismatch downgrades.
Artifact: [`tools/test262/e7-early-error-triage.json`](../tools/test262/e7-early-error-triage.json).
The full-corpus C4 count (~70) additionally spans `test/language/**` dirs
outside this lane's globs; 40 is the exact in-scope population, taken to
zero here, and the whole-corpus V (§4) measures the remaining out-of-glob
residue at 16 files / 18 scenarios (card E7-2).

Families, counts (scenarios), spec rule, Node v26.5 oracle on the canonical
probe (all 13 probes rejected by Node):

| Family | # | Rule | Fix site |
|---|---:|---|---|
| A1 strict call-result targets (`f() = 1`, `(f()) += 1`, `f()++`, `++f()`, `for (f() in/of …)`) | 8 | ES2027 13.15.1/13.4.1/14.7.5.1: strict CallExpression target is *invalid*; Annex B.3.9 web-compat return is sloppy-only | `ParsePostfixOperators` `*pfCanAssign` now `&& !IsStrictMode()` |
| A2 `new C(...) = 1` / `(new C()) = 1` | 4 | new-expressions are not one of the two web-compat CallExpression productions → *invalid* every mode | same site: new-args completion sets `*pfCanAssign = FALSE` |
| B `++import('')` / `import('')++` | 4 | ImportCall target *invalid* every mode (extends card B1-2's diagnosis) | `ParseTerm` tkIMPORT: `fCanAssign = FALSE` after `ParseImportCall` |
| C template `` `\8` `` / `` `\9` `` | 4 | 13.2.8.1 (citation corrected in the repair, §8/N5): NotEscapeSequence → cooked `undefined` is an early error in untagged templates | `Scanner` template-mode `'8'/'9'` cases → `ERRES5NoOctal` |
| D `for (var x = 1 of [])` (+ for-await-of) | 2 | for-of heads admit no binding initializer; Annex B B.3.5 extends **for-in** only, sloppy `var` only | `ParseVariableDeclaration` new `pfForOfOk` out-param; for-site check |
| E labelled function as if/else clause | 3 | 14.6.1 (citation corrected in the repair, §8/N5): `IsLabelledFunction(Statement)` early error; B.3.3 admits only the sole unlabelled form | `ParseStatement` LFunctionStatement: `m_pstmtCur->GetNop() == knopIf` guard (mirrors the existing `knopWith` check), new `ERRStmtOfIfIsLabelledFunc` (perrors 1106) |
| F `(async function* await(){})` | 2 | 15.8/15.9: async (generator) function **expression** names are `BindingIdentifier[+Await]` regardless of ambient | `ParseFncDeclHelper` name check: expression + `IsAsync` + tkID-pid-await → `ERRInvalidIdentifier` |
| G `import.m\u0065ta` / `new.t\u0061rget` | 3 | 13.3.12/13.3.1.1 (citation corrected in the repair, §8/N5): meta properties must not contain escapes | `ParseMetaProperty`: reject on `LastIdentifierHasEscape()` (stale-bit repair in §8) |
| H `{…rest,} = {}` (+ for-in/of heads) | 6 | no `{ BindingRestElement , }` production in object patterns (literals keep it) | `ParseMemberList`: rest + trailing comma in pattern context → `ERRDestructRestLast` |
| I `for (yield * '' in {}; ; )` | 2 | `yield *` operand threads the ambient `[?In]`; under for-head `[~In]` the `in` exposes the invalid for-in target | `ParseExpr` unary/yield\* operand: `TRUE` → `fAllowIn` |
| J `({async async})`, `{async foo: 1}` | 2 | async-prefixed members must be methods (no shorthand/colon production after a committed `async`) | `ParseMemberList`: `(isGenerator || isAsyncMethod) && m_token.tk != tkLParen` → `ERRnoLparen` |

Not-fixed residuals (cards, §6): tagged-template cooked semantics for
invalid escapes; the remaining ~30 corpus DNE failures outside the lane
globs.

## 3. Fixes in detail (one commit)

- `lib/Parser/Parse.cpp` — families A1/A2/B/D/E/F/G/H/I/J as above.
  The assignment-target work is deliberately routed through the existing
  `fCanAssign` + default-on `EarlyReferenceErrorsPhase` early-error sites
  (prefix/postfix update, assignment operators, for-in/of head) instead of
  adding new checks at each site: the three source-level corrections
  (strict call results, new-args completion, ImportCall) make every
  existing gated site fire, in both AST and no-AST (deferred fast-scan)
  modes — verified by `-force:deferparse` probes.
- `lib/Parser/Scan.cpp` — `\8`/`\9` template rejection (matches the
  existing scan-time treatment of `\1`–`\7`), and a latent bug found by the
  debug gate: `m_lastIdentifierHasEscape` was **never initialized**
  (uninitialized 1-bit read by the new `ParseMetaProperty` consult;
  garbage-true made plain `new.target` in eval contexts throw a
  spurious `Syntax error` — the debug-profile assertion caught the shape
  first). Zeroed in `ClearStates()`.
- `lib/Parser/Parse.h` / `perrors.h` — `pfForOfOk` out-param; error 1106.
- `test/es6module/bug_OS14562349.js` — fixture updated: `import('…') *= 2`
  was moved inside `eval` and the test now observes the spec-mandated
  parse SyntaxError (Node-verified). The legacy runtime-error expectation
  encoded pre-web-compat-spec behavior.
- `test/es6/early-error-targets.js` — focused regression, 12 sub-tests,
  all families + positive controls pinning preserved behavior (sloppy
  web-compat runtime ReferenceErrors, Annex B B.3.5 for-in var init, sole
  Annex B.3.3 if-clause functions, object-literal spread trailing comma,
  plain `new f`, `{async: 1}`/`{async,}`/async methods, template `\0`,
  plain `new.target`, sloppy `async function await(){}` declaration,
  non-async `function await(){}` expression, plain `yield` for-init).
  Repair iteration (§8) extended families 7/8 with poisoned-escape
  controls (stale escape bit), escaped-await spellings, module shapes
  through `LoadModule`, and a deferred-reparse shape.

## 4. Before/after evidence

Affected selection (39,301 scenarios, same globs):

| Run | Passed | Failed | DNE cluster |
|---|---:|---:|---:|
| before (`da0a72dbb` clean engine) | 38,452 | 849 | 40 |
| after (lane build) | 38,492 | 809 | **0** |

Key-join `(file, scenario)`: **+40 fail→pass, 0 pass→fail**; the 40 are
exactly the triaged families (12 assignmenttargettype, 4 template-literal,
4 dynamic-import, 3+3 for-of/if, 6 rest, … see triage JSON).

**Whole-corpus V (definitive).** The `ch` shell is a thin host that loads
the engine from `libChakraCore.dylib` next to the binary: a first V
attempt copied the host to /tmp beside an earlier fixed dylib and silently
ran the *fixed* engine for both legs (identical totals exposed it; the
summaries' recorded dylib digests confirmed it — both legs `fe5a451d…`).
Redone with matched pairs built in `zig-out/bin` from stash/pop states,
distinct dylib digests per leg:

| Leg | Passed | Failed | Dylib |
|---|---:|---:|---|
| clean `da0a72dbb` | 78,283 | 14,858 | `65907b2e…` |
| lane build | 78,323 | 14,818 | `cc8f8476…` |

Key-join over all 93,141 scenarios: **+40 fail→pass, 0 pass→fail** — the
same 40, nothing else moved in `test/language/**`, `test/built-ins/**` or
`test/annexB/**`. Logs: `out/test262-e7-{v-clean,v-fixed}.log`.

## 5. Gates and focused verification

- `zig build check` **rc=0**: regression **2048/0**, bytecode nojit 32b/64b
  up to date, native `test-shared-basic` PASS, controls 25/25 inside the
  test262 runs.
- `zig build -Dprofile=debug-static check-regression` **rc=0**: **2100/0**.
- Final-state gate logs: `out/e7-gate-test.log`, `out/e7-gate-debug.log`
  (re-run on the exact committed tree after the V legs).
- **Pin delta: 2045/2097 → 2048/2100** (+3 each; the three rlexe variants
  of `early-error-targets.js`: default, `-force:deferparse`,
  `-noDeferParse`) — `tools/build/gate_counts.json` lineage updated.
- Debug gate earned its keep once: the first cut of family F used
  `IdentifierExpectedError` on a tkID token and the debug-profile
  `Assert(token.tk != tkID)` (Parse.cpp:449) rejected it before any corpus
  run could; reworked to `ERRInvalidIdentifier`.

## 6. Residual cards

- **CARD E7-1 — tagged-template cooked `undefined` (pre-existing,
  amplified by C):** `` tag`\1` ``/`` tag`\8` `` still throw at scan time
  (`tagged-template/invalid-escape-sequences.js`, 2 scenarios, failing
  before and after this lane). Spec (ES2018 template revision) requires
  tagged templates to *evaluate* with cooked `undefined` for invalid
  escapes; implementing that needs the dead `m_fNextStringTemplateIsTagged`
  plumbing revived plus a cooked-undefined representation — distinct work,
  carded. This lane's C family matches the existing `\1`–`\7` scan-time
  treatment so untagged negatives are uniform.
- **CARD E7-2 — corpus DNE residue outside the lane globs (measured: 16
  files / 18 scenarios, whole corpus):** the fixed full-corpus run leaves
  exactly these `$DONOTEVALUATE` failures: strict-mode legacy non-octal
  **numeric** literals (`08`/`09`, incl. numeric-separator forms; 6
  scenarios), strict **string** `\8`/`\9` escapes (4; the scanner accepts
  NonOctalDecimalEscapeSequence in strict strings), and 8 module-code
  shapes (4 duplicate top-level function declarations — the B1-1 module
  name-tracking family, `function f(){} function f(){}`; escaped
  `new.target`; export-labelled declarations; let/function hoist
  conflicts; parameter semi/default errors). Distinct bounded parser
  families, none in this lane's globs; carded for their own slices.
- **B1-2 narrowed:** the dynamic-import invalid-assignmenttarget family
  (13 tests carded) is now fixed for the two update-expression shapes in
  scope; the card's remaining shapes (e.g. `import()` in for-heads/`=` at
  statement level) ride the same parser rule and are expected green on the
  next full manifest refresh.

Repair-iteration cards (both B6 reviews; §8 has the fixes and evidence):

- **CARD R1 — template `\08`/`\09` boundary hole (family C complement):**
  `` `\08` `` currently *accepts* in ch, cooking `\x00` + `'8'` (probe:
  `eval('`\\08`')` → ok) where Node v26.5 SyntaxErrors ("Octal escape
  sequences are not allowed in template strings") because `\0` followed by
  `8`/`9` is a NonOctalDecimalEscapeSequence, not a LegacyOctalEscapeSequence.
  No test262 file in the lane globs covers the compound spelling (the C
  family's 4 scenarios are bare `\8`/`\9`), which is why the lane's C fix
  missed it. Fold into family C's rule when picked up; verified divergence
  on zig-out + debug-static, repair HEAD.
- **CARD N2 — module `async function await(){}` / `var await = 1`
  (review claim not reproduced):** the review reported both accepted in
  modules where Node rejects. At repair HEAD both module entry paths
  **reject**, matching `node --check` `.mjs`: `ch -module file.mjs` →
  `SyntaxError: The use of a keyword for an identifier is invalid`, and
  `WScript.LoadModule(…, 'samethread')` likewise (shapes F50/F48 of the
  repair corpus). Likely source of the observation: the `ch` host exits
  rc=0 even when it prints a SyntaxError, so scripted probes that test the
  exit code alone misjudge acceptance. No engine change needed.
- **CARD R2 addendum — sloppy call-target evaluation order diverges from
  Node:** for the Annex B.3.9 web-compat `f() = g();` parse, ch evaluates
  the RHS first and never calls the LHS function (probe prints
  `g`/`ReferenceError`) while Node v26.5 calls `f` first
  (`f`/`ReferenceError`). Both verdicts are ReferenceError; only the
  observable order differs (spec order for `=` is LHS-reference then RHS,
  so Node's order is the spec one). This is the residual behind the 6
  pre-existing `test/annexB/**` complement failures observed in the V
  runs; pre-existing, unrelated to the parse-time families, carded here so
  the next Annex B slice owns it.
- **CARD N5 — citation corrections (this document, §2 table):**
  meta-property escapes are **13.3.12/13.3.1.1** (`new.target`/
  `import.meta` static semantics), not 15.8.2/16.2.1; template `\8`/`\9`
  is **13.2.8.1** (NoSubstitutionTemplate early errors), not 12.3.9.1;
  the if-clause IsLabelledFunction rule is **14.6.1**, not 14.7.1. The
  family-A "ES2027 15.8" citation was re-checked and stands. §2 rows carry
  the corrected numbers.
- **CARD N4 — import.meta module reachability (review claim corrected):**
  the review claimed import.meta is disabled in ch modules and reachable
  only via the test262 syntax path. Not the case at repair HEAD:
  `DEFAULT_CONFIG_ESImportMeta` is `true`
  (lib/Common/ConfigFlagsList.h:707), plain `import.meta` evaluates and
  `import.m\u0065ta` SyntaxErrors on **both** module entry paths
  (`ch -module` and `WScript.LoadModule(…, 'samethread')`), so family G's
  import.meta rejection is unit-test reachable — the repair's test
  additions now cover it directly (the old file comment claiming
  test262-only coverage was wrong and is gone).
- **CARD N1 — module `f() = 1` runtime-vs-early (matches oracle, out of
  scope):** in module code ch treats call-result assignment targets like
  sloppy classic script (Annex B web-compat: runtime ReferenceError, no
  parse error); Node's module behavior matches ch's here, so no divergence
  to fix; recorded because the boundary reviews probed it.
- **CARD F-esc — escaped `yield`/`await` as inner function-expression
  names in keyword regions (pre-existing, found by the repair corpus):**
  `async function outer(){ var f = function aw\u0061it(){ }; }` and
  `function* outer(){ var f = async function yi\u0065ld(){ }; }` reject in
  ch but parse in Node: the literal spellings take the tkAWAIT/tkYIELD
  path and are accepted ([~Await]/[~Yield] of the name position), while
  the escaped spellings fold to the reserved pid and hit a region check
  keyed on pid. Reproduced on the pre-lane Oct-31 binary → pre-existing,
  not introduced by the e7 or repair changes; corpus shapes F24/F36, the
  two allowlisted ch↔Node mismatches.

## 7. What this lane does and does not claim

It takes the statements+expressions `$DONOTEVALUATE` cluster to zero with
eleven bounded, spec-cited parse-time rejections, preserves every
web-compat/Annex B behavior it touched (positive controls + 0 pass→fail
over 39,301 and 93,141 scenarios), and lands a focused three-variant rlexe
regression with the gate pins bumped and reported. It does not claim the
ES2025 suite green (14,818 broad failures remain on this baseline,
post-ES2025 excluded by policy), does not revive tagged-template cooked
semantics (E7-1), and does not claim the 16-file out-of-glob DNE residue
(E7-2).

## 8. Repair iteration (B6 review repairs, same lane)

Both B6 reviews rejected narrowly; the two engine repairs below land on
`modernize/baseline` as two commits (fix+tests, then this receipt's cards).

### 8.1 Family G — stale `m_lastIdentifierHasEscape` on the meta-property fast paths

`ParseMetaProperty` rejects `new.t\u0061rget` by consulting the scanner's
per-identifier escape marker, but the `LTarget`/`LEval`/`LArguments` fast
paths in `lib/Parser/Scan.cpp` (jumped to from `kwd-swtch.h:388/172/26`)
emit pre-interned PIDs for the *literal* spellings and never wrote the
bit — so the marker kept the value of the *previous* identifier. Any
escaped identifier earlier in the same parse poisoned a later plain
`new.target`/`import.meta` into a spurious `Syntax error`.

- Fix: the three fast paths now write `m_lastIdentifierHasEscape = false`
  (they are reachable only for unescaped spellings — escapes take the
  `\\` case into `ScanIdentifierContinue`, which writes `true`). This is
  the per-token carry the review asked for, and it is inherently
  effective across the deferred re-parse: the re-parse scanner writes the
  bit for the `target` token itself, so no cross-pass state is involved.
- Before → after (zig-out and debug-static):
  - `eval("x\u0062; (function(){new.target;})();")`: SyntaxError →
    **ReferenceError: 'xb' is not defined** (Node-verified).
  - `var q\u0071 = 1; new.target;` (file, global): generic `Syntax error` →
    **`Invalid use of the 'new.target' keyword`**, identical across
    default/`-force:deferparse`/`-noDeferParse`.
  - `function g() { var q\u0071 = 1; return new.target; }`: SyntaxError →
    parses/runs, returns `undefined`.
  - Deferred re-parse: `var o = { \u0061(){ return new.target; } }; o.a();`
    under `-force:deferparse`: debug-static hit the
    `FunctionBody.cpp:2594` "Syntax errors should never be detected on
    deferred re-parse" assertion (assert id 20778 in this tree) and
    zig-out an Internal Error — the initial deferred parse accepted, the
    re-parse "discovered" the stale-bit error for the first time. After:
    clean on all three modes, both binaries, no assert.

### 8.2 Family F — escaped `await` as an async function-expression name

`ParseFncName`'s rejection used `CheckContextualKeyword`, which requires
the escape marker to be false — so `(async function aw\u0061it(){})` and
`(async function* \u0061wait(){})` escaped the BindingIdentifier[+Await]
check (Node: SyntaxError). The scanner interns escaped spellings under the
pid of the *folded* name, so the fix compares pid identity directly
(StringValue equality): `(async function aw\u0061it(){})` and
`(async function* \u0061wait(){})` now reject; the unescaped controls stay
as pinned (sloppy `async function await(){}` declaration legal, plain
`function await(){}` expression legal, declaration-in-await-region
rejected, async methods named await legal — PropertyName, not a binding).

### 8.3 Verification

- `test/es6/early-error-targets.js` extended to 12 sub-tests (poisoned
  eval/file/module controls incl. the specific global verdict message,
  escaped-await shapes, `LoadModule` import.meta/target shapes, one
  deferred-reparse shape, escaped file-level identifiers above a plain
  `new.target` function): **12/12 on zig-out and debug-static × default/
  `-force:deferparse`/`-noDeferParse`** (6/6 combinations).
- Boundary matrix spot: the ten family probes plus the two repaired
  shapes vs Node v26.5 — every negative and every repaired shape aligns;
  the four non-alignments are the lane's documented positive controls
  (V8 keeps strict call-targets as runtime ReferenceErrors and does not
  implement Annex B B.3.3/B.3.5), untouched by this repair.
- 116-shape G/F stability corpus (60 meta-property escape/poisoning
  shapes + 56 await/yield naming shapes, modules via `LoadModule`/
  `node --check .mjs`): verdicts **byte-identical across both binaries ×
  all three parse modes**; 114/116 match Node, the 2 mismatches are
  card F-esc (pre-existing, reproduced on the pre-lane Oct-31 binary).
- Gates at the committed pins, no pin change (tests extended, not added):
  `zig build check` rc=0 — regression **2048/0**, bytecode up to date,
  `test-shared-basic` PASS; `zig build -Dprofile=debug-static
  check-regression` rc=0 — **2100/0**.
