# E5 — B4 lane: compound-assignment reference semantics + let-ASI over-rejection

Status: **landed** — triage commit `f83328580`, fixes+tests commit `375995aec`,
this receipt completes the lane. Base `e720da0d2` (gates 2041 test / 2093
debug at start; both bumped to **2042/2094** by this lane's focused test).

Lane `b4-compound-assign-let-asi`, mainline `modernize/baseline`, Mac ARM64
no-JIT declared configuration. Triage artifact with all 80 per-scenario
records: [`tools/test262/b4-compound-letasi-triage.json`](../tools/test262/b4-compound-letasi-triage.json).

## 1. Triage

Fresh pinned runs (`2e0a5676…`, `-nonative -Test262 -CollectGarbage
-NegativePhaseReport`) at the base commit reproduce both C4 families
exactly. Oracle: Node v26.5.0 plus the published ES2025 spec snapshot
(tc39/ecma262 `es2025` tag).

| Family | Count | Test shape | Engine (before) | Node v26.5.0 | ES2025 verdict |
|---|---:|---|---|---|---|
| nullish-base-before-ToPropertyKey | 44 | `S11.13.2_A7.{1..11}_{T1,T2}` ×2: `base[prop] op= expr`, null/undefined base, `prop.toString` throws | key's `toString` runs to format the TypeError message; its exception replaces the TypeError | TypeError, zero key conversions | **Defect** — `sec-evaluate-property-access-with-expression-key` stores the raw key; `sec-getvalue`/`sec-putvalue` run `ToObject(base)` (throws) before `ToPropertyKey` |
| ToPropertyKey-exactly-once | 22 | `S11.13.2_A7.{1..11}_T4` ×2: key `toString` asserts exactly-once across load+store | converts at `LdElemI_A` and again at `StElemI_A` | also twice (V8 deviation) | **Defect** — `sec-getvalue` step 7 memoizes the converted key into the Reference Record; `sec-putvalue` reuses it |
| let-block-ASI | 7 | `statements/{for,for-in,for-of,for-await-of,if,while,with}/let-block-with-newline.js` | commits to `let {` destructuring ("Destructuring declarations must have an initializer") | parses | **Defect** — a bare statement body requires a Statement, not a Declaration; `let` is a valid ExpressionStatement start (lookahead restricts only `let [`); the newline triggers ASI |
| let-identifier-ASI | 7 | same contexts, `let \n x = 1` | commits to a declaration ("Declaration outside statement context") | parses | same rule |

Node-divergence note (deliberate): after this lane the engine converts an
element compound-assignment key exactly once while Node v26.5.0 still
converts twice; the ES2025 memoization text and the tests are followed over
the oracle. The engine keeps Node's `plain =` ordering (key converted after
the RHS, per the NOTE in `sec-evaluate-property-access-with-expression-key`).

## 2. Fixes

**(1) Nullish base precedes ToPropertyKey (44).**
`JavascriptOperators::GetPropertyDisplayNameForError`
(`lib/Runtime/Language/JavascriptOperators.cpp`): the property name used to
format a *pending* TypeError no longer runs user code. Strings, symbols,
numbers, booleans, null and undefined still name the property (their
conversion cannot call or throw user code); every other key is reported
without a name. Covers element load, store, delete, typeof and method-load
nullish-base errors — all of which decide the TypeError before their real
key conversion.

**(2) Compound element reference semantics (22).**
`lib/Runtime/ByteCode/ByteCodeEmitter.cpp`, `EmitLoad` (knopIndex),
threaded through `EmitBinaryReference`/`EmitAssignment` and the
compound-operator, logical-assignment and ++/-- statement emitters via a new
`convertedKeyLocation` out-parameter (the P5 `referenceBaseLocation`
pattern): when the load feeds a load-then-store, the base is guarded (two
`BrNeq_A` against the null/undefined constant registers +
`RuntimeTypeError JSERR_ObjectCoercible` — the ToObject failure message),
and the key is copied to a temp and converted once with the existing
`Conv_Prop` opcode — but only when the key is a heap object
(`BrOnObject_A`), so tagged-int/string/number/symbol keys keep their raw
representations and the element fast paths (verified: `arr[i] += x` loop
keys stay tagged ints). `StElemI_A` reuses the memoized key register. The
temp is released after the store, above the reference registers (LIFO
discipline; debug builds assert it). Constant keys skip the copy entirely.
The null/undefined constant registers are pre-allocated in
`ByteCodeGenerator::Visit` for the compound shapes (the `knopOptChain`/
`knopNull` pre-emission pattern), because assigning const registers
mid-emission is illegal once temp allocation has started (first attempt hit
`Shouldn't assign var register after we start allocating temp reg`).
Plain `=`, destructuring and for-head targets keep their existing paths.

**(3) let-ASI over-rejection (14).** `Parser::ParseStatement`'s `let`
dispatch (`lib/Parser/Parse.cpp`): `NextTokenConfirmsLetDecl()` alone no
longer commits to a declaration. The declaration parse additionally
requires the confirming token to be `[` (the ExpressionStatement lookahead
restriction is newline-independent, so `let \n [` stays a SyntaxError in
bare position), or no newline after `let`, or a block StatementList context
(`m_pstmtCur->GetNop() == knopBlock`, the same predicate `AddDeclForPid`
uses) — where `let \n x` is still the declaration per the grammar and Node.
The labelled-statement branch just above already had the newline carve-out,
which is why the 2 labeled scenarios were not failing.

## 3. Ownership (memory-management.md read first)

No allocation, ownership, retained-pointer, GC-barrier or memory-accounting
change. (1) removes a user-code reentrancy point from an error-message
path. (2) is emitter register discipline: one ordinary bytecode temp per
compound site, acquired/released with the existing LIFO helpers that the
debug gates assert; no runtime object, parse-node mutation or serialized
state is added (the memoized key register is threaded through a parameter;
parse nodes are not re-pointed). (3) is parser acceptance logic only —
parser changes are N/A for ownership here.

## 4. Before/after (same source tree, stash/rebuild, pinned corpus)

| Selection | Before | After | Flips |
|---|---|---|---|
| compound-assignment (786 scenarios) | 720 / **66** | **786 / 0** | 66 fail→pass, 0 pass→fail |
| let-ASI glob (25 scenarios) | 11 / **14** | **25 / 0** | 14 fail→pass, 0 pass→fail |
| Wide: `expressions/**` + `statements/**` + `eval-code/**` (39,757 scenarios) | 38,638 / 1,119 | 38,750 / **1,007** | **112 fail→pass, 0 pass→fail** |

The 112 wide flips = 66 compound-assignment + 24 postfix/prefix-increment/
decrement exactly-once (`S11.13.2_A7`-style, same fix through `EmitLoad`)
+ 6 logical-assignment (`&&=`) exactly-once + 2 member-expression nullish
ordering + 14 let-ASI. Zero pass→fail anywhere, including `dynamic-import/`.

Parse-mode variants: both selections (811 scenarios) are **811/811** under
`-force:deferparse` and `-off:deferparse`; focused test 8/8 in all three
modes on both binaries.

S4/S14 focused suites: `optional-chaining/**`, `logical-assignment/**`,
`statements/let/**`, `statements/const/**`, `expressions/assignment/**` →
1,591/25, every failure a P5-documented destructuring residual
(fn-name-lhs-cover, keyed-destructuring evaluation order, obj-prop
__proto__ dup, array-elem-put-obj-literal, obj-id-init-fn-name,
obj-rest-before-comma-invalid) — none exercise compound assignment. In-tree
S4/S14 focused tests (`optionalchaining.js`, `logicalassignment.js`,
`assignment-reference-base.js`, `blockscope-early-errors.js`,
`s14-eval-nonsimple-var.js`) pass on test and debug-static.

JIT consistency spot-probes (`-mic:1 -bgjit-` and `-forceNative
-off:simpleJit -bgJitDelay:0`, 100k-iteration warmup): exactly-once and
nullish-TypeError-before-conversion hold in both JIT tiers — the semantics
live in the bytecode stream (`BrNeq_A` guard, `Conv_Prop`, existing
lowering), not the interpreter.

## 5. Gates (all through the shared gate lock)

| Gate | Result |
|---|---|
| `zig build` (test profile) | 568/568 steps |
| `zig build check-regression` | **2042 / 0** (pin bumped 2041→2042) |
| `zig build check` | **583/583 steps**: regression 2042/0, `verify_bytecode_pure: source headers unchanged` (the built-in library contains no compound element assignment, so no JsBuiltIn regeneration), native, smoke, metadata PASS |
| `zig build check-regression -Dprofile=debug-static` | **2094 / 0** (pin bumped 2093→2094; exercises the temp-LIFO asserts) |
| `python3 tools/build/check_regression.py --self-test` | all negative controls behave |
| `zig build -Dprofile=jit check` | **646/646 steps** (JIT bytecode verification up to date, native JIT smoke PASS) — required because compound-element bytecode emission changed |
| gate bookkeeping | `tools/build/gate_counts.json` single-source bump + lineage; workflow/build.zig read it dynamically |

## 6. Residual cards

- **CARD B4-1 — `for (let \n of []) {}` accepted.** ChakraCore parses it;
  Node v26.5.0 and the grammar reject (`let` then newline then `of` in a
  for-of head). No test262 scenario in the pinned corpus covers it; fix with
  any future for-head `let` work (the `for`-head site at `Parse.cpp:13608`
  still uses `NextTokenConfirmsLetDecl()` without the newline carve-out,
  which is correct for `let \n x` declarations but permissive for `of`).
- **CARD B4-2 — Node exactly-once divergence.** Node v26.5.0 (V8) converts
  compound element-assignment keys twice (verified: `o[k()] *= v` → 2 on
  Node, 1 on this engine). ES2025 memoization + test262 followed; recorded
  so future oracle comparisons don't read it as an engine bug.
- **CARD B4-3 — destructuring memoization unprobed upstream.**
  `[o[k]] = [v]`, object destructuring and for-of/for-in head targets
  measure exactly-once on both this engine and Node today; the emission
  path (`EmitDestructuredObjectGet`) was deliberately not changed. If a
  future corpus revision adds ToPropertyKey-count assertions there, re-check.
- **Pre-existing (inherited, unchanged):** the 25 P5 destructuring residuals
  in `expressions/assignment/**`; `GetPropertyObjectForElementAccess` still
  returns undefined instead of throwing when
  `ThreadContext::RecordImplicitException()` is false (debug/probe
  contexts), a mode the new bytecode guard (`RuntimeTypeError`) does not
  consult — unreachable for the corpus and the gates.

## 7. Expected C4 delta (record; apply after acceptance)

Edition-selected failures 4,080 → expected **−112**: the 66
compound-assignment + 14 let-ASI families this lane owns, plus the 32
bonus flips the same fixes produce (24 postfix/prefix-increment/decrement,
6 logical-assignment, 2 member-expression). All 112 were edition failures
before (untagged semantic tests of default features); the next C4
measurement should confirm 4,080 − 112 = **3,968** with zero regressions.
The let-ASI 14 leave the required-feature-missing bucket
(review-corrected misclassification per c4-manifest §2) and the 98 others
leave the engine-defect bucket.

### Review outcome (dual ACCEPT with conditions, folded)

Both reviewers ACCEPTed with zero lane-introduced `[bug]`. The general
review verified all three fixes from source and spec (the BrNeq_A×2
guard is exactly null/undefined; ToPropertyKey-before-RHS ordering;
heap-object gating keeps primitives raw — patched-BigInt-toString runs
zero times; const registers pre-allocated for every compound shape)
and recomputed the full arithmetic (112 f→p / 0 p→f over 39,757,
composition exact). The adversarial review attacked with mutating-key
discriminators across all 13 compound operators + logical assignment +
pre/post inc/dec (engine converts exactly once AND stores through the
FIRST key — Node stores through the second; the engine even survives
generator suspension with the memoized temp intact: `o[k] += (yield)`
resumes correctly where Node corrupts the store), reentrancy
(throwing toString depth-3, nested compounds, 60-100k warm bailout
loops — exact in all modes), and a 45-shape parser battery (≡ Node in
all three parse modes).

**Folded conditions:**
1. **B4-4 card (pre-existing):** `super[k] += v` (and `super.k +=`)
   runs the getter AND — for computed/name keys — the STORE with the
   **home-object prototype as receiver** (instance reads the pollution
   via the chain); even dot-form getters get the wrong `this`.
   Pre-existence confirmed (receiver semantics unchanged by this
   lane); wide-run 0-p2f confirms no corpus masking; fix belongs to a
   future super-semantics lane. 2. **B4-1 corrected:** the card's
   `for (let \n of [])` claim does NOT reproduce (both engines
   reject); the accurate kernel is `for (let \n in {})` accepted by
   BOTH engine and Node (matching oracle, no action) — card rewritten
   as a non-issue. 3. **Gate logs archived** under `out/b4-gate-logs/`.

**Folded nits:** the AssignRegisters↔emission pairing risk comment
(future compound opcodes without an AssignRegisters case assert in
debug and emit BrNeq_A against NoRegister in release — noted at the
guard's next touch); error-text asymmetry (compound vs plain nullish
messages — cosmetic, nothing pins either); knopCoalesce's idempotent
undefined-const reservation; non-const primitive keys pay one extra
Ld_A copy (perf-only); the stale `ca-after` mid-lane artifact
annotated (786/0 comes from `after2`).
