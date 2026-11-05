//-------------------------------------------------------------------------------------------------------
// Copyright (c) ChakraCore Project Contributors. All rights reserved.
// Licensed under the MIT license. See LICENSE.txt file in the project root for full license information.
//-------------------------------------------------------------------------------------------------------

# E10/B9: resizable-ArrayBuffer partial — path-to-green slice

Status: **landed** (commits on `modernize/baseline` after `d3533db71`); gates
**2049 test / 2101 debug** unchanged in count (three stale legacy expectations
updated in place). Mac ARM64 no-JIT `test` profile unless noted.

Lane objective (C4 path-to-green, manifest §5c): the `resizable-arraybuffer`
feature-tag population, manifest edition-failures **130** at engine checkpoint
`ba796f74f`. Triage was performed **fresh** at HEAD `d3533db71`: the B3
detach lane and S8 typedarray-tracking had already absorbed 98; the remaining
population is **32 failing scenarios / 16 files** (842 scenarios / 421 files
selected by the `features: [resizable-arraybuffer]` tag, minus the 50 files
that also require `SharedArrayBuffer`).

## 1. Fresh triage (all 32 scenarios)

| Family | Scen. | Verdict (Node v26.5.0 / ECMA-262 draft) | Disposition |
|---|---:|---|---|
| generic `Array.prototype.join` separator-order | 4 | 23.1.3.16 reads `length` **before** the separator `ToString`; Chakra read it after, so a resize inside the coercion re-based the iteration | **fixed** |
| generic `map`/`reduceRight` presence on TA receivers | 8 | 23.1.3.10/24: `HasProperty` per index; OOB indices skipped (result index stays a hole); fast paths called callbacks for OOB indices | **fixed** |
| `Object.{freeze,seal,preventExtensions}` on TAs | 2 | 10.4.5.1 `[[PreventExtensions]]` false for resizable-buffer views (can regain integer keys on grow); non-empty fixed views reject `configurable:false`; zero-length fixed views succeed | **fixed** |
| `%TypedArray%.prototype.fill` post-coercion bounds | 2 | 23.2.3.9 step 7 `ValidateTypedArrayBounds` after value/start/end coercion → TypeError | **fixed** |
| `new TA(srcTA)` with OOB fixed source | 2 | TypedArrayFromTypedArray rejects an out-of-bounds source with TypeError | **fixed** |
| OOB-index `[[Set]]` coercion order (`resized-out-of-bounds-to-in-bounds-index`) | 2 | 10.4.5.18 `TypedArraySetElement`: convert first, revalidate after (store when the coercion re-covers the index); was the S8 residual, double-carded as B8-2 | **fixed** |
| `Atomics.{notify,wait,waitAsync}` length-before-index-coercion | 10 | `ReferenceError: Atomics is not defined` under default flags; **flag-on ceiling 10/10 with `-ESSharedArrayBuffer`** | **card** → S6 disabled-flag decision (manifest §5b, 1,176 owed) |
| `options-maxbytelength-compared-before-object-creation` | 2 | inherited `Reflect.construct` pre-read of `newTarget.prototype` (S8-resizable Residuals); constructor-side RangeError cannot precede the getter | **card** → unchanged, unbounded (Reflect.construct redesign) |

Bonus fixes surfaced by the same population:

- **`CanonicalNumericIndexString("-0")` returned +0.0** (`*indexValue = -0`
  is the *integer* constant, which promotes to +0.0), so `ta["-0"]` stored and
  read index 0. Now `-0.0`; store/read/`in`/getOwnPropertyDescriptor probes
  match Node. All callers are TypedArray-scoped (audit in §4).
- `test/built-ins/Array/prototype/methods-called-as-functions.js` (2
  scenarios, not resizable-tagged) flipped fail→pass with the join/map fixes.
- `Reflect.preventExtensions` now reports `false` (no mutation) for a
  resizable-buffer TA instead of marking it non-extensible and returning
  `true` (no corpus coverage; asserted in the focused es7 test).

## 2. Fixes with anchors

| Fix | Site |
|---|---|
| join length-before-separator (generic receiver path only) | `JavascriptArray::EntryJoin` captures `JavascriptArray::OP_GetLength` before `JavascriptConversion::ToString(args[1])`; threaded `JoinHelper`/`JoinOtherHelper` optional `capturedLength` (`lib/Runtime/Library/JavascriptArray.cpp`, `JavascriptArray.h`) |
| generic map per-index presence + hole result | `JavascriptArray::MapHelper` TypedArray branch: no `mapLength` clamp; `kPresent = !detached && k < GetLength()` per iteration; absent → `continue` (result keeps the captured length; hole at absent indices — no 2³¹ materialization) |
| generic reduceRight per-index presence | `JavascriptArray::ReduceRightHelper`: `isTypedArrayEntryPoint` recorded before the receiver fallback; both the initial-value search and the loop skip `detached || index >= GetLength()` for the generic entry |
| fill post-coercion OOB revalidation | `JavascriptArray::FillHelper`: existing detach recheck extended to `typedArrayBase->IsOutOfBounds()` |
| ctor from OOB source | `TypedArrayBase::CreateNewInstance`: `IsOutOfBounds()` check beside the detached check (both throw `JSERR_DetachedTypedArray`) |
| OOB-set coercion order | `TypedArrayBase::ValidateAndReturnIndex` gains `indexOutOfBounds` out-param (set only for a canonical non-negative integer ≥ length; negative/-0/fractional stay on the convert-only path); `ValidateIndexAndDirectSetItem` routes those to `DirectSetItem`, which already converts then rechecks |
| `"-0"` canonicalization | `JavascriptConversion::CanonicalNumericIndexString`: `*indexValue = -0.0` |
| integrity level on TAs | `JavascriptObject.cpp` `ThrowIfTypedArrayCannotSetIntegrityLevel` + the three entries; RAB-backed (or growable-SAB) view → TypeError with no side effect; non-empty fixed-buffer view → `PreventExtensions()` runs first (ext=false observable, matching Node), then TypeError; `JavascriptReflect::EntryPreventExtensions` returns false for RAB-backed views |

Ownership (per [memory-management.md](memory-management.md) discipline): no
allocation, pointer-ownership, root or release-path change. All fixes are
order/validation changes on existing paths; the element store after a grow
writes into the S8 stable-capacity buffer (no remap), and every revalidation
re-reads `GetArrayBuffer()->GetByteLength()` rather than caching pointers
across user code.

## 3. Before / after (same selection, same corpus `2e0a5676`)

- **Selection (842 scenarios / 421 files)**: 810/32 before → **830/12** after;
  **20 fail→pass / 0 pass→fail**. Residual 12 = 10 Atomics (flag) + 2
  Reflect.construct-order (card).
- **Adjacent corpus** (`Array/prototype` + `Object` + `Reflect` + `TypedArray`
  + `TypedArrayConstructors` + `ArrayBuffer` + `DataView` = 18,593 scenarios),
  base `d3533db71` binary vs final: **22 fail→pass / 0 pass→fail** (the 20
  above + `methods-called-as-functions` ×2).
- Focused suites: `es7/resizable-arraybuffer.js` **15/15** (12→15 tests),
  `es7/resizable-typedarray.js` **26/26** (23→26), `es7/float16.js` 10/10,
  `es6/ES6TypedArrayExtensions.js` 70/70 (registered flags),
  `es6/ES6ArrayAPI.js` 45/45.
- Gates: `zig build check` **2049/0 PASS** (pin 2049); `-Dprofile=debug-static
  check-regression` **2101/0** (pin 2101). The initial run of this lane's
  build was 2046/3 — `es5/seal.js`, `es5/freeze.js` (legacy expectations whose
  own comments carried the open "what is the spec???" / "should it throw?"
  question, resolved here against the current spec and Node) and
  `es6/ES6TypedArrayExtensions.js` (a fast-fail abort when the generic map
  materialized a 2³¹-element result — fixed by the hole semantics above).
  All three updated/repaired in place; counts return to the pins.
- `zig build --summary line` 568/568; `-Dprofile=jit` 643/643.
- **s8 campaign batches** (`tools/jit-test262`, 3 modes, final binary):
  `s8-resizable` nojit 804/30, interp **+2 diffs**, fulljit **+152 diffs**;
  `s8-float16` nojit 5164/294, interp +0, fulljit **+202 diffs**. These
  mode-diffs are **inherited, not this lane's**: the same base `d3533db71`
  JIT build reproduces them with **0 per-scenario differences vs the after
  binary in all three modes** (evidence dirs `out/c2jit/s8-resizable-{base,after}`);
  the recorded table's 0-diff s8 rows predate the B3/B8 engine commits. The
  fulljit residue is the known S8 JIT gap (cached `GetOffsetOfLength` reads in
  the JIT for length-tracking views).
- Evidence hashes (final binary): launcher `ch` `5e210836…`, dylib
  `c8acfacd…` (full digests in `out/test262-b9-rab-final/summary.json`).

## 4. Residual cards (precise)

1. **`-ESSharedArrayBuffer`-gated Atomics (10 scenarios, this population).**
   Flag-on ceiling 10/10. Owner: the recorded S6 product decision (manifest
   §5b); nothing further for C4 unless the flag flips default.
2. **`options-maxbytelength-compared-before-object-creation` (2).**
   Inherited `Reflect.construct` ordering (S8-resizable Residuals); needs the
   pre-created-result read moved after constructor execution. Unbounded here.
3. **Plain-`Array` join separator order (unobserved by the corpus).** The
   Array fast path (`JoinArrayHelper`) still reads `arr->length` after the
   separator coercion; Node iterates the captured length (`[1,2,3].join(evil)
   → "1.."` vs ch `"1"`). Only observable with a length-mutating separator on
   a real array; no corpus test discriminates. Fix = captured length through
   `JoinArrayHelper` segment iteration.
4. **`Reflect.set(ta, "-0", v)` and `SetItem` early-out (B8-2 family).**
   `TypedArrayBase::SetItem` skips the observable conversion for non-BigInt
   OOB stores (`PropertyOperation_None` early-out), and a `String`-object key
   `ta[new String("-0")]` neither converts nor stores (Node converts). Same
   family as the double-carded B8-2 internals card (canonical-non-index
   `[[Set]]` fall-through, prototype-setter bypass, OOB delete,
   `configurable:true` descriptors).
5. **JIT fulljit diffs (s8 batches).** Inherited at `d3533db71` (verified
   base-vs-after per-scenario equality); the length-tracking JIT gap remains
   S8/B4 work per the S8 receipt.
6. **`Object.isSealed`/`isFrozen` on non-extensible TAs** report through the
   ordinary object walk (Node's `isSealed=true` post-failed-seal quirk not
   replicated); no corpus coverage either way.

## 5. Expected C4 delta

Manifest §5c `resizable-arraybuffer` edition-failures **130 → 12** under the
fresh-count mapping (20 scenarios fixed in the tag population; 98 were already
absorbed by B3/S8 before this lane). Plus one adjacent edition-selected flip:
`Array/prototype/methods-called-as-functions.js` (2 scenarios, counted under
its own feature tags in the manifest join). The remaining 12 split 10
SAB-flag-gated (§5b decision bucket) + 2 inherited Reflect.construct order
(card 2). The manifest's `resizable-arraybuffer 130` line and §5c partial
table should be refreshed by the next measurement lane.

### Review outcome (dual REJECT → repair landed)

Both reviews found the same **lane-introduced regression**: explicit-
length views over growable SharedArrayBuffers wrongly refused
`preventExtensions`/returned false (the new `canRegainProperties`
sites keyed on buffer growability with no view-length check; the base
binary was Node-correct). Per ES2027 10.4.5.1 + IsTypedArrayFixedLength,
an explicit-length growable-SAB view IS fixed-length (growable SABs
never shrink) and follows OrdinaryPreventExtensions; only
length-tracking growable-SAB views stay non-preventable. **Repaired**
at both sites (`typedArray->IsLengthTracking()` required in the SAB
arm; the ArrayBuffer arm untouched — the 30-shape integrity matrix
verified it Node-matching) with a flag-on regression test added to
`resizable-typedarray.js`. Node-divergence note: `Object.seal` on the
same view throws in ch post-fix (spec-correct configurable:false walk)
where Node does not — spec followed, carded.

**Folded cards:** B9-7 — gOPD on TA integer indices reports
`configurable:false` (spec/Node: `true`; `TypedArrayBase::
IsConfigurable` pre-existing; this lane's seal/freeze rationale
assumed c:true — the honest card is the gOPD divergence, superseding
the old card-6 wording; fix at a future slice since it changes
engine-wide numeric-property semantics). B9-8 — `Reflect.set`
conversion-only stores return false for ALL canonical non-indices
(OOB/negative/-0/fractional; spec 10.4.5.18 returns true; pre-existing
shape touched-but-not-closed; the Proxy-set-delegation TypeError
cascade included). Receipt corrections: the "or growable-SAB view →
TypeError" claim in the commit/receipt was wrong (now fixed in code);
spec anchors corrected (join 23.1.3.18, map 23.1.3.21, reduceRight
23.1.3.25, ValidateTypedArrayBounds 23.2.4.5); the s8 nojit row is
806/28; the map-loop comment (holes, not CreateDataPropertyOrThrow
undefined); the EntryJoin fast-path comment's "cannot observe" claim
false (receipt card §4.3 stands); `test/es5/seal.js` duplicate block.
