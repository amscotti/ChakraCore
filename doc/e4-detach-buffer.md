#-------------------------------------------------------------------------------------------------------
# Copyright (c) ChakraCore Project Contributors. All rights reserved.
# Licensed under the MIT license. See LICENSE.txt file in the project root for full license information.
#-------------------------------------------------------------------------------------------------------

# B3: detached-buffer TypedArray cluster — triage and align fix

Status: **landed** (lane `b3-detach-buffer`, mainline `modernize/baseline` @
`ac4c9d413`; commits (a) triage evidence, (b) fixes + tests, (c) this
receipt). This receipt covers the C4 path-to-green engine-defect cluster
"detached-buffer TypedArray/DataView ~104" (c4-manifest §2). The fix aligns
the engine's detached TypedArray element semantics with
tc39/ecma262#2164 ("align detached buffer semantics with web reality",
required-ES2025 per `edition-policy.json` — the tag's rationale cites the
normative pre-ES2025 change). Node v26.5.0 is the differential oracle; a
probe-transcript caveat is in §4.

## 1. Triage

Fresh runs on the pinned corpus revision `2e0a5676` (base `ac4c9d413`,
pre-fix binary snapshotted to `out/b3-base-bin/ch`, SHA-256
`f7072741…bbf58ad0`) over the detach-heavy globs
`test/built-ins/TypedArray/**/*detach*`, `test/built-ins/DataView/**/*detach*`,
`test/built-ins/ArrayBuffer/**/*detach*|*transfer*` (444 scenarios) and
`test/built-ins/TypedArrayConstructors/**/*detach*` (126 scenarios):
**124 failing scenarios, all edition-selected, all TypedArray** — DataView
and ArrayBuffer detach/transfer tests were already green. The certified
manifest's "~104" was an estimate inside this cluster; the exact measured
composition (committed: [`tools/test262/b3-detach-triage.json`](../tools/test262/b3-detach-triage.json)):

| Family | Scen. | Node v26.5.0 verdict (spec) | Engine before | Disposition |
|---|---:|---|---|---|
| A `[[Get]]`/`[[GetOwnProperty]]` detached (incl. `-realm`) | 16 | `undefined` / `undefined` | TypeError | **fixed** |
| A2 `[[HasProperty]]` detached (incl. `-realm`) | 8 | `false` | TypeError | **fixed** |
| B `[[Set]]`/`[[DefineOwnProperty]]` detached (incl. `-realm`; value-conversion-first shapes; contains the pre-existing sloppy-detached-store card from `doc/c2-jit-immutable-store.md`) | 16 | store silent no-op (sloppy **and** strict); `defineProperty` → `true` when the value conversion detaches, `TypeError` when detached at entry | TypeError / `false` | **fixed** |
| C `[[Delete]]` detached (incl. `-realm`, strict+sloppy) | 10 | `true`, no strict-mode throw | `false` / strict TypeError | **fixed** |
| D iteration callbacks detach mid-loop (every/some/forEach/reduce/reduceRight/find/findIndex/findLast/findLastIndex × Float+BigInt) | 36 | loop continues over the captured length, elements read `undefined`, no `HasProperty` gate | TypeError on next read | **fixed** |
| D2 filter detach (incl. BigInt kept-value order: all callbacks, then convert — `ToBigInt(undefined)` throws after the loop) | 6 | loops 2/3; result `[0, NaN]` / TypeError after callbacks | TypeError mid-loop | **fixed** (engine's raw-keep + post-species convert order was already correct; unblocked by the read fix) |
| E includes/indexOf/lastIndexOf detach during `fromIndex` coercion | 24 | captured-length loop reads `undefined`: `includes(undefined)`→`true`, `includes(0)`→`false`, `indexOf`/`lastIndexOf`→`-1` | TypeError | **fixed** |
| F `set(array)` getter detaches target | 2 | all source getters run; remaining stores silent | TypeError | **fixed** |
| G `TypedArray.from` mapper detaches the result | 6 | result returned; stores no-op; `length` 0 | TypeError | **fixed** |
| DataView getters/byteLength after detach | 0 failing | TypeError per spec | conformant | none |
| ArrayBuffer `detach`/`transfer` family | 0 failing | — (S8 receipts) | conformant | none |

**124 → 0** on both selections after the fix (§3).

## 2. Fix

All families funnel through the element-access primitives, so the change is
bounded and uniform — "a detached view behaves like a length-0 view; value
conversions stay observable and run before the detached check":

- `lib/Runtime/Library/TypedArray.h`
  - `BaseTypedDirectGetItem`, `TypedDirectGetItemWithCheck`: drop the
    detached throw; the `index < GetLength()` bound returns `undefined`
    without touching `buffer` (detach already cleared `length`/`buffer`).
  - `BaseTypedDirectSetItem`, `BaseTypedBigIntDirectSetItem`: the observable
    `ToNumber`/`ToBigInt` conversion still runs first (it may itself detach);
    detached after conversion → **silent success** — `IntegerIndexedElementSet`
    returns a normal completion, so neither sloppy nor strict assignment
    throws (the BigInt overload previously returned `FALSE`, which broke
    `Reflect.defineProperty`'s `true`).
  - `BaseTypedDirectSetItemNoSet`: conversion first, then `FALSE` without
    throwing (OOB/detached index store, sloppy and strict).
  - `DirectSetItemAtRange` (both MemSet/MemCopy fusion overloads): detached
    source/destination → `FALSE` — the established bail-out contract
    (`BailOutOnMemOpError` replays the per-element stores through the helper
    path), mirroring the immutable-buffer rejection directly above each check.
- `lib/Runtime/Library/TypedArray.cpp`
  - `HasItemQuery`: detached → `Property_NotFound_NoProto` (was: throw).
  - `DeleteItem`: detached → `TRUE` (including strict mode); attached keeps
    the cant-delete throw/`FALSE` (an interim draft returned `TRUE` for both
    and flipped 4 attached-sloppy Delete corpus tests — caught by the broad
    key-join and corrected before commit).
  - `Int64Array`/`Uint64Array`/`Float16Array::DirectGetItem`: detached →
    `undefined` (hand-specialized getters not covered by the numeric base).
- `lib/Runtime/Library/JavascriptArray.cpp`
  - `FillHelper`: `%TypedArray%.prototype.fill` re-checks detachment after
    the value/start/end coercions and before any write (even an empty range)
    → `TypeError`, scoped to the TypedArray entry point only — the generic
    `Array.prototype.fill` path keeps ordinary `[[Set]]` semantics and stays
    silent (Node-verified). Without this, the aligned silent stores removed
    the old incidental throw that 6 `fill/coerced-*-detach.js` corpus tests
    rely on.

Every consumer inherits the semantics through these primitives (iteration
helpers `EveryHelper`/`SomeHelper`/`ForEachHelper`/`Reduce*`/`Find*` with
their typed-array branches, `TemplatedIndexOfHelper`/`LastIndexOfHelper`,
filter's raw-keep + post-species convert, `SetObject`, `TypedArray.from`,
`ValidateIndexAndDirectSetItem`, `DefineOwnPropertyForTypedArray`,
`GetItemQuery`/`GetPropertyQuery`) — no per-builtin edits beyond `fill`.

Deliberately **unchanged** (spec-mandated TypeErrors, green before and
after): `ValidateTypedArray` entry checks (length/byteLength/byteOffset
getters, slice, subarray, sort at entry, entries/keys/values iterators,
`at`), `DataView` detached accessors, `ArrayBuffer.prototype.*`, sort's
post-comparator detach check, `toSorted`'s finalizer-window check,
`SpreadArgument` (spread of a detached view goes through the array iterator
whose length getter validates), `Atomics`, JsRT detach paths.

### Ownership record (doc/memory-management.md)

No allocation, ownership, retained-pointer, GC-visible-state, callback,
thread or context change: pure control-flow/return-value edits. Detach-time
invalidation remains exactly
`TypedArrayBase::ClearLengthAndBufferOnDetach` (`length = 0`,
`buffer = nullptr`/tagged-0, `ArrayBuffer.cpp` parent notification): every
changed early-return happens **before** any `buffer` dereference (the
`index < GetLength()` bound gates the load), so no path can touch freed
storage; the store-side early-returns never write. Value conversions that
may reenter user code still execute before the detached check in every
store primitive — the "complete coercion, then re-validate storage"
discipline. The `DirectSetItemAtRange` `FALSE` returns reuse the existing
JIT bail-out/replay mechanism (same contract as the immutable-store lane);
no new invalidation event, and the JIT store fast path is untouched (its
length bound already routes detached views to the helper).

## 3. Verification

Mac ARM64, no-JIT `test` profile, shared gate lock, mise-pinned Zig 0.17.0,
corpus `2e0a5676`. Before = pre-fix snapshot of the same base commit.

| Selection / gate | Before | After | fail→pass | pass→fail |
|---|---|---|---:|---:|
| detach globs (TypedArray/DataView/ArrayBuffer, 444 scen.) | 370/74 | **444/0** | 74 | 0 |
| TypedArrayConstructors detach glob (126 scen.) | 76/50 | **126/0** | 50 | 0 |
| broad TypedArray + TypedArrayConstructors + DataView (5,458 scen.) | 4,988/470 | **5,112/346** | **124** | **0** |
| focused rlexe `test/typedarray` (disable_jit) | 59/0 | **60/0** (+1 = new test) | — | 0 |
| focused rlexe `test/es7` (disable_jit) | 101/3 stale pins | **101/0** (3 tests updated to aligned semantics) | — | 0 |
| `zig build check` (test profile, no-JIT) | 2040/0 | **2041/0** (rc=0; native/smoke/metadata PASS) | — | — |
| `zig build -Dprofile=debug-static check-regression` | 2092/0 | **2093/0** (rc=0) | — | — |
| `zig build -Dprofile=jit check` | — | **646/646** (check_jit/check_smoke PASS, bytecode up to date) | — | — |
| `zig build check-build-tools` | — | **2/2 OK** (gate_counts consumers) | — | — |

The broad key-join (`(file, scenario)`, 0 asymmetric keys) shows exactly
**124 fail→pass, 0 pass→fail**; the 124 are precisely the triage table's
files. An interim build (before the `DeleteItem` attached-case correction
and the `fill` post-coercion check) showed the value of the broad join: 4
pass→fail (attached sloppy `delete`) and 6 pass→fail (`fill` coercion
detach) — both corrected in the final build and re-verified to zero.

## 4. Node differential and probe methodology

Node v26.5.0 was run on a per-shape probe (get/has/gOPD/delete/store
sloppy+strict/defineProperty conversion-order/every/includes×2/indexOf/
set-getter/from-mapper/filter) using **explicit off-heap buffers**
(`new Float64Array(ab, 0, n)` + `structuredClone(0, {transfer:[ab]})`):
the post-fix engine matches Node **line for line**. Methodology caveat,
recorded so it is not rediscovered: Node/V8 typed arrays constructed from
array literals (`new Float64Array([5,6])`) use **on-heap** backing stores,
and transferring their lazily-created buffer leaves the view reading old
bytes and `length` unchanged (`ab.byteLength === 0` while `t.length === 2`)
— a V8 implementation quirk, not spec; probes must use explicit buffers.
This engine's typed-array views are always backed by an external
recycler-tracked `ArrayBuffer`, so the quirk does not exist here.
Two spec corners were corrected against Node during development: strict
`delete`/store on detached views are silent (not TypeError), and
`%TypedArray%.prototype.reduce` has no `HasProperty` gate (all captured
iterations call back with `undefined`).

## 5. Residuals / cards

- **`fill` empty-range check scope.** The post-coercion detached check
  throws even for an empty fill range (Node-verified, spec step order);
  the analogous immutable-buffer rejection deliberately skips empty ranges
  (c2-s8-immutable). The asymmetry is intentional (detach check is spec
  text; the immutable range carve-out is proposal text) — carded here for
  awareness only.
- **`FindMinOrMax` and `CharArray` paths unchanged.** The sort min/max fast
  path (no comparator ⇒ no user code mid-sort) and the internal CharArray
  element paths keep their detached throws; they are not reachable from the
  failing corpus shapes. No behavioral claim either way.
- **`GetPropertyQuery` numeric-PropertyId proto walk.** A detached (or OOB)
  numeric read through the *fixed-PropertyId* path returns
  `Property_NotFound` (proto walk continues), while the uint32 element path
  correctly stops at `NotFound_NoProto`; a prototype-installed numeric
  property (`Float64Array.prototype[0] = …`, itself exotic-to-define) could
  be observed through the former. Pre-existing, tier-shared, no corpus
  witness — carded as low-priority.
- **V8 on-heap TA quirk** (§4): differential only; no engine action.
- **JIT:** this lane is no-JIT like its predecessors; the primitives are
  shared interpreter/JIT helpers, and the JIT store fast path's length bound
  already routes detached views to the helper, so no fast-path reorder was
  made and no s8/JIT campaign is owed by this change (the detach-check
  *order* inside fast paths was not touched).

## 6. Expected C4 delta and gate delta

- Expected certified edition-selected delta: **−124 engine-defect failures**
  (the manifest's "~104" estimate of this cluster measured 124; the
  remainder of the engine-defect bucket is untouched). Apply to
  `c4-gate.md` only after review acceptance, by re-running the c4 analysis
  on a fresh certified run (per-record join required — the recorded
  estimate is not a per-record baseline).
- Gate delta: the new deterministic regression test moves the pins by one:
  `test` **2040 → 2041**, `debug-static` **2092 → 2093** (recorded; applied
  to `tools/build/gate_counts.json` in this lane's commit (b) — final gate
  runs below).

| gate | result |
|---|---|
| `zig build check` (test profile, no-JIT) | **2041/0** (was 2040/0; rc=0, native `test-shared-basic` PASS, smoke PASS, metadata PASS) |
| `zig build -Dprofile=debug-static check-regression` | **2093/0** (was 2092/0; rc=0) |
| `zig build -Dprofile=jit check` | **646/646**, check_jit PASS (`-mic:1 -bgjit-` and `-forceNative` probes), check_smoke PASS, bytecode up to date — run because TypedArray.h/.cpp are shared engine paths; no detach-check reorder was made in fast paths, so no s8 campaign re-run is owed |
| `zig build check-build-tools` | **2/2 OK** (gate_counts consumers, updated pins) |

### Review outcome (dual ACCEPT-conditional, folded + repair iteration)

The general review ACCEPTed (spec-verified step-for-step against the
ES2025 published snapshot; engine ≡ Node across 39 shapes; funnel
proven). The adversarial review ACCEPTed conditionally and found two
REAL pre-existing spec bugs, now handled:

**Repaired in this fold:** `[[DefineOwnProperty]]`'s stale ES2016
`configurable` rule (JavascriptOperators.cpp — the main TypedArray
branch rejected present-and-TRUE; post-#2164 spec/Node reject only
present-and-FALSE). One-character fix; the IMMUTABLE branch's
present-and-TRUE rejection is **S8-intentional** (modeled as
non-configurable storage; its committed test pins it — deliberately
NOT changed). Legacy 2016-era pins updated to spec/Node:
`objectproperty.js` (configurable:true now no-throw — verifyNoThrow +
regenerated baseline), `reflect_defineProperty.js` (the "valid
descriptor" test's descriptor corrected to configurable:true + a new
cfg-false rejection test), `FilterWithTypedArray.js` (filter with TA
species completes per Node), `toPrimitive.js` (callback descriptor
made spec-valid). Engine ≡ Node verified for every changed shape.

**Cards filed (adversarial findings):** B1 — `Reflect.set` with an
invalid index at entry returns false and SKIPS the value conversion
(spec: converts first, returns true; entry plumbing bypasses the
primitives; corpus-invisible); B3 — RAB-shrunk-to-OOB `delete` returns
false/throws-strict where spec says true (only `IsDetachedBuffer` is
keyed; `IsValidIntegerIndex` OOB half unimplemented in DeleteItem);
B4 — `sort()` comparator-detach: engine throws (spec-correct), Node
doesn't (V8 divergence; recorded like the on-heap caveat); B6 —
`Object.freeze` on attached non-empty TA doesn't throw (Node does);
general-review nit — attached-OOB `delete` returns false (spec: true;
same IsValidIntegerIndex half as B3's card).

**Receipt corrections:** §2's "every consumer inherits through these
primitives" is false for the Reflect.set/defineProperty ENTRY
plumbing (B1's root); §4's "line for line" excludes B1/B2/B4 shapes;
spec citations updated to ES2025 numbering (10.4.5.18
TypedArraySetElement / 10.4.5.6 [[Set]]); §1's defineProperty row —
Reflect returns false at entry (the TypeError belongs to the
Object.defineProperty surface); the memcopy-fusion Assert-placement
observation recorded (pre-detached arrays reach the Assert before the
FALSE return in asserts builds — unchanged shape, unreachable from
corpus paths).
