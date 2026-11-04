# E9 / B8: the BigInt partial — fresh triage and the toLocaleString element-dispatch fix

Status: **landed** (lane `b8-bigint-partial`, mainline `modernize/baseline`, base
`e1a84575c`). Scope: the certified manifest §5c entry "BigInt 378" — the
biggest remaining documented default-enabled partial. One bounded shared fix
landed (element-level `toLocaleString` dispatch); everything else in the fresh
composition is a recorded flag/decision/host/typed-array-substrate card.
Committed triage artifact:
[`tools/test262/b8-bigint-triage.json`](../tools/test262/b8-bigint-triage.json).

## 1. Fresh triage (FRESH, per the lane order; not the manifest replay)

Selection: every corpus file under `test/language/**`, `test/built-ins/**`,
`test/annexB/**` whose frontmatter `features` include `BigInt` (1,453 files →
2,883 broad scenarios; 1,452 executed — the standard duplicate/agent skips).
Corpus `2e0a5676…`, default host flags `-nonative -Test262 -CollectGarbage
-NegativePhaseReport`, 8 jobs / 10 s, controls 25/25 per run. Edition-selected
per `edition-policy.json` (all-tags-required rule). Binary bookkeeping: the
launcher `ch` (`5e210836…`, loads the engine dynamically) is identical across
every run; the engine moved `libChakraCore.dylib e72c6861…` (clean `e1a84575c`
rebuild, the before half) → `04cb9a2c…` (the one-file fix, the after half);
debug-static `eaf67b68…`.

| Measure | Manifest (engine `ba796f74f`, 2026-11-02) | Fresh before (`e1a84575c`) | Fresh after (fix) |
|---|---:|---:|---:|
| BigInt edition-selected failures | 378 | **304** | **286** |

The 378 → 304 attribution: the manifest's engine checkpoint predates the B3
detached-buffer lane (`ac4c9d413`, whose families explicitly included the
BigInt64 views); later microtask/ordering and e-lanes did not move BigInt
counts. Fresh composition of the 304 (first-match buckets, analyze.py rules):

| Family | Scenarios | Bucket | Verdict (Node v26.5.0 / spec) | Disposition |
|---|---:|---|---|---|
| `%TypedArray%.prototype.toLocaleString` element override/abrupt semantics (`TypedArray/prototype/toLocaleString/BigInt/*`, 9 files) | 18 | engine-defect | spec §22.2.3.28 = `ToString(Invoke(element, "toLocaleString"))`; Node observes overrides and propagates throws; ch formatted primitives directly | **fixed** (this lane) |
| TypedArray integer-indexed internals `[[Set]]`/`[[Delete]]`/`[[DefineOwnProperty]]`/`[[GetOwnProperty]]` (`TypedArrayConstructors/internals/*/BigInt/*`, 11 files) | 22 | engine-defect | Node: canonical-non-index stores fall through to OrdinarySet (`Reflect.set("-0"|"1.1", …) === true`, no receiver property created), valid indices never reach a prototype setter, OOB `delete` is `true`, index descriptors `configurable: true` | **card B8-2** — inherited, not BigInt-specific (§4) |
| `ESSharedArrayBuffer` off: Atomics BigInt ops undefined + SAB-gated constructors/views (Atomics `*/bigint` 172, `ctors-bigint` 50, SAB view/prototype 42) | 264 | disabled-flag | flag-on ceiling measured: **234/264 pass** with `-ESSharedArrayBuffer`; the 30 residual are `Atomics.wait` 26 / `notify` 2 / `waitAsync` 2 big-int multi-agent waiter-list tests (lapse/notify-count assertions) | **cards B8-3/B8-4** — recorded product decision + host `$262.agent` class |
| Expected-but-absent families — JIT-gated BigInt ops, quadratic `ToString` timeouts, DataView BigInt edges, Map/Set identity, `BigInt.prototype.toString` radix edges, literal parse edges, `built-ins/BigInt/**`, `language/literals/bigint/**`, all arithmetic/comparison dirs | 0 | — | all green in this run (0 timeouts, 0 crashes; no JIT under the no-JIT gates) | no action; standing cards only (§5) |

Post-fix BigInt edition-selected failures: **286** = 264 SAB-flag + 22
ta-internals. Before/after join over the whole 2,883-scenario selection:
**18 fail→pass, 0 pass→fail** (exactly the toLocaleString family).

## 2. The fix — element-level toLocaleString dispatch

**Anchor**: `lib/Runtime/Language/JavascriptConversion.cpp`,
`JavascriptConversion::ToLocaleString` (its single caller is
`JavascriptArray::ToLocaleStringHelper`, which serves BOTH
`Array.prototype.toLocaleString` and `%TypedArray%.prototype.toLocaleString`).

**Defect**: the function switched on the element's type and formatted
primitives directly (`Number`/`Int64`/`UInt64` → `JavascriptNumber::
ToLocaleString`, `Boolean` → literal, `Symbol`/`BigInt` → `ToString`), so the
spec's `Invoke(element, "toLocaleString")` lookup was skipped entirely for
primitive elements — overrides of `Number.prototype.toLocaleString` /
`BigInt.prototype.toLocaleString` were unobservable through arrays and typed
arrays, and their throws/coercions never happened. Object elements already
took the correct GetProperty + call + ToString path.

**Fix**: every non-undefined/null element now goes through one path: box
primitives **only for the property lookup** (`JavascriptConversion::ToObject`),
`GetProperty(holder, "toLocaleString")`, `IsCallable` check (non-callable →
`TypeError`, matching `Invoke`), call with the **original value** as receiver
inside the existing `ExecuteImplicitCall(ImplicitCall_ToPrimitive)` wrapper
(identical to the former object path — sloppy callees see the boxed receiver,
strict ones the primitive), then `ToString` the result. Undefined/null keep
their display-string behavior (the caller filters them to empty
contributions). No JIT helper, bytecode, serializer or tracer change; the
S2 focused suites and `-Serialized` paths are untouched.

Behavior notes (verified, §4): default outputs are unchanged for every
element kind — `[1,2]`, `[true]`, `[1n]` format exactly as before because the
default prototype formatters produce the same strings as the removed direct
calls (19-value Number battery incl. `-0`/`1e21`/`±Infinity`: 0 divergences
between the array path and `(v).toLocaleString()` post-fix, matching Node,
which also prints `∞`/`-0`). Two strict improvements: `[Symbol()]` no longer
throws (`Symbol.prototype.toLocaleString` is `ToString`), and `±Infinity` /
`-0` elements in arrays now match `Number.prototype.toLocaleString` (= Node)
instead of the legacy direct formatter. With the method deleted the element
falls back to `Object.prototype.toLocaleString` → `ToString`, as in Node.

**Ownership record** (doc/memory-management.md discipline):

```text
Resource / creation: one Recycler wrapper object per PRIMITIVE element via
  JavascriptConversion::ToObject (NumberObject/BooleanObject/SymbolObject/
  BigIntObject) inside JavascriptConversion::ToLocaleString; plus the return
  value's ToString string when coercion is needed. No new allocation sites
  elsewhere.
Domain/allocator: the owning ScriptContext's Recycler. No foreign allocator,
  finalizer, external buffer or root is added.
Logical length / borrowed views: the wrapper is used for exactly one property
  lookup and is then unreachable garbage; nothing retains it. The element Var
  itself is passed as the call receiver unchanged.
Owner; transfer/publication: none — the wrapper never escapes the call; the
  function's return value (a JavascriptString) is the only published object,
  same as before.
Borrow invalidation / reentrancy: ExecuteImplicitCall runs user code (the
  element's toLocaleString) — a NEW user-code point for primitive elements,
  the same class the object path always had. The caller's loop holds only
  recycler-traced Vars (arr, element, res); length is captured at entry (spec
  captured-length semantics) and each ItemTrace::GetItem re-reads through
  [[Get]], so detach/shrink mid-iteration yields empty contributions —
  exercised by the new test's detach (transfer) and RAB-shrink cases, and by
  the B3 detached-buffer semantics suite (still green).
Thread/context: owning ScriptContext thread only.
Failure/exception paths: a non-callable method or uncoercible return throws
  TypeError; abrupt throws from the method or its ToString propagate — no
  partial state exists (no mutation anywhere).
Memory accounting: one wrapper per primitive element per toLocaleString call
  — the same order as the separator/result string concatenations the loop
  already performs; unbounded only in the pre-existing unbounded-length
  sense, bounded per element.
Tests: test/es7/tolocalestring-element-invoke.js (-CollectGarbage), the
  corpus flips, Node differential battery, debug-static reruns.
```

## 3. Tests

New `test/es7/tolocalestring-element-invoke.js` (registered once in
`test/es7/rlexe.xml` with `-CollectGarbage -args summary -endargs`): override
observation through arrays, Float64Array and BigInt64Array; deleted-method
fallback; returned-object coercion via valueOf/toString and the uncoercible
TypeError; abrupt throws (RangeError override, BigInt override);
undefined/null skipping; non-callable TypeError incl. Symbol elements;
symbol formatting; strict/sloppy receiver boxing; and element overrides that
detach (`ArrayBuffer.prototype.transfer`) or shrink a RAB mid-iteration
(captured length, empty contributions). Passes on test and debug-static,
plain and `-CollectGarbage`.

## 4. Evidence

- BigInt selection before/after (above): 2,883 scenarios, **18 fail→pass /
  0 pass→fail**; edition-selected BigInt failures 304 → 286.
- Analog selection (non-BigInt `TypedArray/prototype/toLocaleString`,
  `Array/prototype/toLocaleString`, internals Set/Delete/DefineOwnProperty/
  GetOwnProperty; 286 scenarios): 216/70 → 240/46, **24 fail→pass /
  0 pass→fail** — the 18 non-BigInt TypedArray toLocaleString scenarios, the
  2 `Array …/primitive_this_value{,_getter}.js`, and both
  `user-provided-tolocalestring-shrink.js` variants (Array + TypedArray,
  resizable-arraybuffer-tagged: the Invoke fix unblocks them and the
  captured-length + bounds-re-read semantics already produce `"0,0,,"`).
  The 46 residual there are the inherited ta-internals family plus 2
  SAB-ReferenceError scenarios (flag class).
- Node differential: 17-shape battery (overrides per primitive type, wrapper
  detection, both coercions, uncoercible, throw propagation, non-callable,
  deleted, holes, strict/sloppy receiver) — byte-identical ch vs Node on the
  test binary and on debug-static. Number battery (19 values incl. `-0`,
  `1e21`, `±Infinity`): 0 array-vs-element-method divergences post-fix.
- Focused rlexe (`--variants=disable_jit`): `test/es7` + `test/BigInt`
  **117/0** (S2 focused suites; includes the new entry);
  `test/Array/toLocaleString.js` + `test/Object` + `test/typedarray`
  **126/0**; `test/Intl` **17/0**; `test/Date` + `test/es6` green.
- Gates: `zig build check` **583/583** steps, regression **2049/0**;
  `-Dprofile=debug-static check-regression` **2101/0** (547/547);
  `-Dprofile=jit check` **646/646** (shared path moved —
  `JavascriptConversion.cpp` hosts JIT conversion helpers, none touched;
  bytecode "up to date", smoke OK). Pin delta **2048→2049 / 2100→2101**
  (`tools/build/gate_counts.json` lineage updated; +1 = the new entry).
- `git diff --check` clean.

## 5. Residual cards

| Card | Scenarios | Substrate / decision | Owner |
|---|---:|---|---|
| B8-1 (closed) `%TypedArray%.prototype.toLocaleString` override family | was 18 BigInt + 24 analogs | **fixed this lane** | — |
| B8-2 typed-array integer-indexed internals semantics: canonical-non-index `[[Set]]` fall-through (`Reflect.set("-0"/"1.1") → true`, no receiver property), valid-index stores bypassing prototype setters, OOB `delete → true` (incl. strict), index descriptor `configurable: true` | 22 BigInt-tagged + ~40 non-BigInt analogs (inherited — non-BigInt analogs fail identically, verified same-binary) | typed-array `[[Set]]`/`[[Delete]]`/descriptor substrate; belongs to the §5c "TypedArray surfaces ~1,010" partial, not BigInt | typed-array internals lane |
| B8-3 `ESSharedArrayBuffer` off (Atomics BigInt ops, SAB-gated ctors/views) | 264 | recorded product/security decision (plan §4.1 S6, §5b table); flag-on ceiling **234/264** | flag-flip decision lane |
| B8-4 Atomics big-int multi-agent waiter-list (`wait`/`notify`/`waitAsync`: lapse, notify-count, waiter identity) | 30 (flag-on) | host class: `$262.agent` cross-agent support absent; non-BigInt analogs fail identically (S2 A3b/A3c 148/68 vs 146/70) | host/agent lane |
| B8-5 (carried, unchanged) JIT BigInt surfaces (`Lower.cpp:23967` `!ESBigInt` equality fast path; no BigInt JIT lowering/IC evidence), TTD BigInt recording, `JsGetValueType` = `JsObject`, giant-decimal quadratic `ToString`/parse (performance only — 0 corpus failures, 0 timeouts) | 0 corpus | S2-default residuals, restated as still-true after this lane's run | B4/TTD/JsRT/perf lanes |

Expected **C4 delta** (for the re-measurement lane): BigInt edition-failures
378 (manifest) → **286** at this HEAD (−74 B3 detach, −18 this lane); whole-
corpus edition total expected **−42** (18 BigInt-tagged + 24 analog flips;
the analogs debit the TypedArray/resizable partials' counts).

## 6. Commands

```
# triage / after / analogs / ceiling
tools/test262/run.py --binary $PWD/zig-out/bin/ch --checkout $PWD/out/test262 \
  --output out/test262-b8-bigint-before  <bigint-tagged file list>   # 2217/666
… same selection on the fixed build → out/test262-b8-bigint-after       # 2235/648
… analog globs → out/test262-b8-analogs{,-after}                        # 216/70 → 240/46
… --host-flags '… -ESSharedArrayBuffer' → out/test262-b8-sab-ceiling    # 234/30
# focused + gates
test/runtests.py --test --variants=disable_jit --binary …/ch test/es7 test/BigInt
zig build check;  zig build -Dprofile=debug-static check-regression;  zig build -Dprofile=jit check
```

### Review outcome (dual ACCEPT with mandatory cards, folded)

The general review verified the fix spec-exact on both served paths
(including the primitive-receiver subtlety: sloppy callees box, strict
see the primitive — callee-side ToThis per spec), ownership clean
(per-element boxing is the spec-forced shape; wrapper never escapes),
and all arithmetic reproduced. The adversarial review ran a 75+-shape
×4-binary matrix (reentrancy to 1451-deep, 300k churn in the
per-element window, detach+RAB mid-iteration, Proxy/null-proto/sparse,
JIT forceNative warm loops — all byte-identical to Node), reran the
full 2,883-scenario selection byte-identical, and confirmed the
B8-2/B8-3 cards' exact file sets and ceilings.

**Folded mandatory cards:**
- **B8-6 (locale-arg forwarding, pre-existing, THIS FAMILY):**
  `EntryToLocaleString` never reads `args[1..2]` — the element call is
  `CallInfo(1)` with zero arguments. ES2023+ Array §23.1.3.29 and the
  %TypedArray% path forward «locales, options» to each element call;
  the fix's comment cited the pre-2021 `Invoke` text. Node forwards
  (`[1,2].toLocaleString("de-DE",{...})` → each element sees the args);
  ch passes none (so `[123456789n].toLocaleString("de-DE")` ignores
  the locale). Pre-existing (old paths never forwarded either), 0
  corpus coverage — but this lane owns the family's semantics, so the
  card is mandatory: thread locales/options through
  `EntryToLocaleString`/`TypedArrayBase::EntryToLocaleString` →
  `ToLocaleString` (`CallInfo(3)` when supplied).
- **B8-7 (default-locale BigInt grouping):** `[123456789n].toLocaleString()`
  → ch `"123456789"`, Node `"123,456,789"` (numbers DO group) —
  pre-existing in `BigInt.prototype.toLocaleString` itself, zero
  corpus coverage; carded.
- Attribution hedge: 378→304 = ≥58 documented by B3's artifact, the
  remainder by exclusion (b2/b5 wide flips incl. the
  subclass-BigInt64Array files) — not a flat 74.
- "17-shape byte-identical" scoped: the non-callable TypeError MESSAGE
  differs from Node (type identical, messages not spec'd). The
  `out/release-shared` dir is a stale pre-lane build (hygiene note;
  authoritative runs use `zig-out`, sha-matched).
