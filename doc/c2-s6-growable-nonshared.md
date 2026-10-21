//-------------------------------------------------------------------------------------------------------
// Copyright (C) Microsoft. All rights reserved.
// Copyright (c) ChakraCore Project Contributors. All rights reserved.
// Licensed under the MIT license. See LICENSE.txt file in the project root for full license information.
//-------------------------------------------------------------------------------------------------------

# C2-S6a: growable SharedArrayBuffer and non-shared Atomics (flag-on slice)

Status: **landed** — merged as `e3371d313` on top of base `9a99ccbee`;
review fix `eaa526f89`, integration/debug fix `9fceb6fe4`. Final Mac ARM64
no-JIT gates are **2020/0 test** and **2072/0 debug-static**. Review and B4
JIT/Linux/multi-agent checks remain open. `ESSharedArrayBuffer` stays **off by
default**: without the flag both `SharedArrayBuffer` and `Atomics` are absent.
This receipt covers only the flag-on surface below; it does not propose a
default change.

## Scope

1. `new SharedArrayBuffer(length [, options])` with `options.maxByteLength`,
   `SharedArrayBuffer.prototype.maxByteLength`, `.growable` and
   `.grow(newLength)`.
2. Capacity is `maxByteLength`; `byteLength` tracks the current logical length;
   grow zero-fills newly exposed bytes and never reallocates or remaps.
3. `.slice` keeps its existing species/validation behavior; a normal slice
   result is fixed-length, a growable species result is preserved.
4. TypedArray views created from a growable SAB without an explicit length are
   length-tracking; explicit-length views stay fixed. DataView auto-length
   views track the current length (its existing dynamic implementation).
5. ES2020 non-shared Atomics: `Atomics.add`, `sub`, `and`, `or`, `xor`,
   `exchange`, `compareExchange`, `load` and `store` accept integer-indexed
   TypedArrays over non-shared ArrayBuffers. `isLockFree` is unchanged.
6. `Atomics.wait` stays shared-only. `Atomics.notify` accepts a non-shared
   Int32Array, evaluates `count`, then returns 0.
7. `AtomicReadModifyWrite` index validation now captures the TypedArray length
   **before** `ToIndex(index)`, matching the 2025 test262 revision; the raw
   operand is revalidated after value coercion so a detach/shrink cannot leave
   a stale pointer. The debug-only `accessIndex < GetLength()` assertion in
  `TypedArrayBeginStub` was removed for these value-taking entries: an index
  getter may legally shrink the buffer before the value is converted, so the
  post-conversion revalidation owns the live invariant. `Atomics.load`, which
  has no value coercion, keeps its immediate post-index check.

Out of scope: `Atomics.waitAsync`, `Atomics.pause`, BigInt typed arrays,
immutable ArrayBuffers, cross-agent grow synchronization, and any default
enablement.

## Design and ownership

Resource / creation / release:

- A JS growable SAB is a `JavascriptSharedArrayBuffer` whose `SharedContents`
  carries the existing ref-counted byte block plus two new facts:
  `isGrowable` and the existing `maxBufferLength` reinterpreted as the capacity
  limit. `bufferLength` is now explicitly the current
  `[[ArrayBufferByteLength]]`; before this slice it never changed after
  construction.
- Creation path: `SharedArrayBuffer::NewInstance` parses `length` and
  `options.maxByteLength`, checks `length <= maxByteLength`, resolves
  `newTarget.prototype`, checks the engine cap, then
  `JavascriptLibrary::CreateSharedArrayBuffer(length, max, growable)` →
  `JavascriptSharedArrayBuffer::Create` → `Init(length, max, growable)`.
- **Allocation domain / release authority.** A growable buffer requests
  `RequestExternalMemoryAllocation(maxByteLength)` once and allocates exactly
  that capacity through `HeapNewNoThrowArray`. The fixed-length asm.js virtual
  allocator is deliberately bypassed for growable buffers because growth must
  not remap. `FreeBuffer` deletes the same `maxByteLength` extent and
  `Finalize` reports the same extent to the recycler. Fixed SABs keep
  `length == maxLength`, so their allocation, accounting and free behavior are
  unchanged. WebAssembly SAB paths pass `isGrowable == false` and keep their
  existing `GrowMemory` accounting.
- **No invalidation of views.** SharedArrayBuffers are not detachable. Grow
  only writes `bufferLength` after zero-filling `[oldLength, newLength)`.
  TypedArray/DataView view pointers and captured byte offsets therefore stay
  valid; length-tracking TypedArrays read the new length from
  `GetByteLength()` on every length/access query. Fixed views keep their
  construction snapshot and cannot become out of bounds because a SAB cannot
  shrink.
- **Reentrancy / failure.** The constructor follows the ArrayBuffer order:
  `ToIndex(length)`, read and `ToIndex(options.maxByteLength)`, compare,
  resolve `newTarget.prototype`, check the engine cap, allocate. A failed
  allocation leaves no published `SharedContents`; `Init` publishes
  `sharedContents` only after the buffer is allocated and zeroed. `grow`
  checks the brand and growable slot before coercing its argument, then rejects
  shrink/over-cap with RangeError before touching bytes. Failure leaves the old
  length and contents intact. A user getter can run during coercion; `grow`
  re-reads the live lengths through the buffer object, not a cached copy.
- **Thread/context.** Atomics and grow remain script-context operations. The
  `SharedContents` block is already shared with other agents; `bufferLength`
  is a plain 32-bit field in this slice. Concurrent grow from two agents, and
  any agent reading/iterating a view while another grows, is a broader data
  race than grow-vs-grow: readers can observe a length without the matching
  zero-filled extent or synchronize with a waiter list. A multi-agent contract
  is **not** claimed here; it needs an atomically published length/capacity
  (an atomic 64-bit length in a future design) plus waiter coordination. The
  tested contract is single-agent grow-only with a stable pointer; the
  ordinary worker sharing path keeps working through the existing refcount.
- **WebAssembly SharedArrayBuffer.** `WebAssemblySharedArrayBuffer` keeps
  `isGrowable == false`: `.growable` is false, `.maxByteLength` reports the
  current byte length, and `.grow()` throws TypeError, while
  `WebAssembly.Memory.prototype.grow` keeps using `GrowMemory` and its
  existing reserved/committed accounting. This matches Node `v22.22.3` and Bun
  `1.4.3` probes, which report a wasm shared memory buffer as
  `growable === false` with `maxByteLength === byteLength`.
- **Eager capacity is a memory-exhaustion vector.** `maxByteLength` bytes are
  requested and zeroed at construction, not committed lazily. A small script
  can therefore ask for a GB-scale capacity (up to the 2^31-1 engine cap) and
  pressure the recycler/external-memory accounting even though only a few
  bytes are ever exposed. This slice keeps eager allocation because it is the
  invariant that makes grow non-remapping and pointer-stable; choosing a lazy
  reservation or a lower host limit is a product/security decision, not made
  here.
- **Atomics ownership.** No allocation. The validation helpers only borrow the
  TypedArray and its buffer for the duration of the entry point. `Atomics`
  helpers capture the live length before index coercion. For the eight
  value-coercing operations (`add`, `sub`, `and`, `or`, `xor`, `exchange`,
  `compareExchange`, `store`) the spec's `RevalidateAtomicAccess` follows the
  value conversion, so they do **not** revalidate between index and value
  coercion; the TypedArray helpers revalidate after converting the value
  (`TypedArrayCheckStillValidForAtomics`). `Atomics.load` has no value
  conversion and keeps its immediate post-index check. This makes an index
  getter that shrinks a non-shared resizable buffer, followed by a value getter
  that grows it back, succeed as in Node/Bun; an index error still precedes a
  value error, a value error is not replaced by an OOB TypeError, and a view
  still out of bounds after conversion raises TypeError. `compareExchange`
  coerces `expectedValue` before `replacementValue` explicitly rather than
  relying on argument evaluation order.
- **Virtual-buffer classification.** `SharedArrayBuffer::IsValidVirtualBufferLength`
  returns false while the buffer is growable, mirroring the
  `!isResizable && IsValidVirtualBufferLength(...)` rule used for resizable
  ArrayBuffers. A growable SAB is heap-allocated to `maxByteLength`, not
  virtually reserved, and must not select the asm.js `*VirtualArray` vtables
  even when its current length is a power-of-two 64 KiB-style value. Fixed SAB
  classification is unchanged.

Property installation is flag-gated through the existing
`IsESSharedArrayBufferEnabled` block. New built-ins
(`SharedArrayBuffer` maxByteLength/growable getters and `grow`) are appended at
the end of `JavascriptBuiltInFunctionList.h`; no existing built-in id shifts,
and the property ids `maxByteLength`/`growable` are dynamic tracked names, so
no new static `PropertyId` or bytecode-header change is introduced.

## Tests

New `test/es7/s6-growable-nonshared.js`, registered once in
`test/es7/rlexe.xml` with `-ESSharedArrayBuffer`, covers:

- flag-on precondition (globals and `grow` present) and fixed SAB shape;
- growable construction, options objects/non-objects, absent/undefined/
  fractional/NaN/negative/excessive `maxByteLength`, comparison ordering and
  getter/coercion order;
- `grow`: grow/same, shrink and over-cap RangeError, fractional truncation,
  NaN/undefined behavior, non-growable TypeErrors, brand and constructor
  checks; byteLength tracking and multi-grow zero-fill with existing bytes
  preserved;
- `maxByteLength`/`growable` accessor descriptors and brand checks; `grow`
  descriptor/name/length and non-constructor behavior; getter-only setters
  ignore assignment, and a later grow still observes the original limit;
- auto-length TypedArrays (including offset and subarray) growing, fixed views
  staying fixed, auto/fixed DataView tracking, and range errors on construction;
  a 64 KiB growable SAB (a valid asm.js virtual-buffer length) keeps an
  explicit full-length view fixed across grow, confirming the virtual-path
  exclusion is behaviorally safe;
- `slice` result fixedness/copy, species growable result, and species
  smaller/same-buffer TypeErrors;
- all nine non-shared Atomics operations for `Int8Array` through
  `Uint32Array`, cross-checking the atomic store with a normal TypedArray read;
- non-shared validation/order: type before index/value coercion, RangeError
  for bad index, detached and OOB view TypeErrors, `wait` shared-only,
  `notify` count-before-return-0;
- `Atomics` revalidation order after value conversion: index coercion shrinks
  and the value getter regrows (store/add/compareExchange succeed), index error
  precedes value error, value error precedes OOB revalidation, and a view that
  stays OOB after conversion throws TypeError. Detach here is
  `ArrayBuffer.prototype.transfer()` on a non-shared buffer; SABs never detach;
- growable-SAB Atomics before and after growth, zero-filled new elements and
  preserved existing elements.

## Evidence

Mac ARM64, test profile (RelWithDebInfo, no JIT, DISABLE_JIT), shared gate
lock, local `.zig-cache`/`out`. Every authoritative `zig build`/test262 command
ran through `sh /tmp/opencode/cc-gate-lock.sh`.

- Lane `zig build --summary line`: **568/568 steps succeeded**. After
  integration with the eval-super lane, the authoritative full
  `zig build check`: **583/583 steps succeeded**; final regression gates
  **2020/0 test** and **2072/0 debug-static** (the S6 entry moved
  2016/2068 -> 2017/2069; the three eval-super entries then moved it to
  2020/2072).
- Base binary: `ChakraCore-wt/evalsuper` `zig-out/bin/ch` at the same base
  `9a99ccbee` (copied with its dylibs to `out/base-bin`), launcher SHA-256
  `76e5f0734b3afe511ea0b309f8ddc62306387661d9880ff998aa85112b1ecbea`.
  After binary: `zig-out/bin/ch` SHA-256
  `02c64783232e101b7cf32816a3f0cbed3286168addd6038bc8ecbe8d5e555f69`;
  dylib SHA-256 `838e6b997281da39dcb09010aa34e13817324730d1f80fbc4ceedea5e30f8c59`.
- `test/es7/s6-growable-nonshared.js`: base **0/13 → 13/13** after (with
  `-CollectGarbage` also 13/13). Default (no flag) run: **0/13** — test #1
  fails its precondition assertion (`typeof SharedArrayBuffer` is
  `undefined`, not a ReferenceError), tests #2–#13 fail with
  `ReferenceError: 'SharedArrayBuffer' is not defined` at their first SAB
  use. A direct probe prints `SAB=undefined Atomics=undefined`, while
  `-ESSharedArrayBuffer` prints `SAB=function Atomics=object` with `grow`,
  `maxByteLength` and `growable` present.
- Adjacent tests after the change (same flag): `sharedarraybuffer.js` 7/7,
  `atomics_test.js` 16/16, `resizable-arraybuffer.js` 12/12,
  `resizable-typedarray.js` 23/23. `ES6TypedArrayExtensions.js` with its
  registered `-CollectGarbage -ES6ObjectLiterals -ESSharedArrayBuffer` flags:
  70/70; `ES6ArrayAPI.js` 45/45.
- Pinned test262 revision `2e0a56762801e275a9fdf96dc49d90ba0cddcf63`,
  20 controls passed. Focused selection
  `test/built-ins/Atomics/**/*.js` and
  `test/built-ins/SharedArrayBuffer/**/*.js`
  (982 scenarios / 491 files):
  **462/982 before → 574/982 after**, **112 fail→pass / 0 pass→fail**.
  Re-run on the merged binary 2026-10-21: **574 passed / 408 failed**
  (`out/test262-final-s6`).
  Subsets:
  - grow methods: 2/30 → **28/30** (2 residual are the BigInt-syntax
    `this-is-not-object` scenario);
  - growable accessor: 0/18 → **18/18**;
  - maxByteLength accessor: 0/20 → **20/20**;
  - constructor maxByteLength options: 2/18 → **16/18** (the 2 residual
    scenarios are `options-maxbytelength-compared-before-object-creation.js`,
    the inherited Reflect.construct prototype-read order; `options-non-object.js`
    is not part of this filter and its 2 scenarios are BigInt-syntax);
  - Atomics files whose names contain `non-shared`: 24/86 → **54/86**
    (30 residual are BigInt files; 2 are `waitAsync`).
- Non-regression for the S8 TypedArray length-tracking surface, same checkout
  and flags (`test/built-ins/TypedArray/**/*resizable*` +
  `.../**/*out-of-bounds*`, 340 scenarios / 170 files): **132/340 → 132/340,
  0 fail→pass / 0 pass→fail**.
- Differential (same probe in `/tmp/opencode/cc-diff.js`):
  Node `v22.22.3` and Bun `1.4.3` both report `shape 4 16 true`,
  `grow 8 7,0,0,0,0,0,0,0`, auto-length views `2→4` and fixed view `1`,
  auto DataView `8→16`, fixed slice `16 false 16`, shrink RangeError,
  fixed-grow TypeError, non-shared `Atomics.add/load` 0/5, `notify` 0,
  and `wait` TypeError; Chakra matches all of these. For the 2025
  length-before-index ordering, Bun and the pinned test262 agree with this
  implementation (non-shared `notify` after resize-to-zero returns 0; a
  growable-SAB view that had length 0 before index coercion throws RangeError).
  Node 22.22.3 predates that revision and differs on those two ordering
  probes, so it is not used as an oracle there.
- Revalidation-after-value differential (`/tmp/opencode/cc-revalidate.js`):
  Node `v22.22.3`, Bun `1.4.3` and this binary all report store shrink→regrow
  `7` (stored at index 1), add shrink→regrow `0`/value `5`, value-getter error
  identity, index-getter RangeError precedence, no-regrow TypeError, and
  store-after-regrow `7`. Before the fix Chakra threw the early OOB TypeError
  on the add case because it revalidated before value coercion.
- The 64 KiB virtual-length case is exercised by the new test (explicit
  full-length view fixed, auto view 65536→131072, no crash); fixed SABs and
  WebAssembly SABs keep the pre-existing virtual classification paths.

### Residual classification of the 408 remaining scenarios

- **202 `Atomics.waitAsync`** — deliberately not implemented in this slice
  (`Atomics.waitAsync` is absent).
- **10 `Atomics.pause`** — not implemented in this slice.
- **124 BigInt** — 92 BigInt-literal parse failures plus 28 BigInt-typed-array
  scenarios plus 4 other BigInt/parse cases; BigInt64Array/BigUint64Array are
  not implemented.
- **18 immutable-arraybuffer** — `transferToImmutable`/immutable buffer
  proposal is out of scope.
- **50 multi-agent wait/notify** — agent tests whose waiters do not suspend:
  `WaiterList::AddAndSuspendWaiter` is a no-op returning false on non-Windows
  (`// TODO for xplat`), so notify observes 0 waiters. This is pre-existing
  xplat wait support, not a regression from this slice.
- **2 Reflect.construct prototype-order** — the inherited Chakra
  `Reflect.construct` pre-read of `newTarget.prototype` (same residual recorded
  in [c2-s8-resizable.md](c2-s8-resizable.md)).
- **2 cross-realm prototype** — pre-existing cross-realm prototype handling.

## Residuals and risks

- **waitAsync/pause** stay absent; no Promise-returning wait and no
  `Atomics.pause`. Documented above.
- **BigInt Atomics/typed arrays** remain unimplemented; BigInt `non-shared`
  coverage is unexecuted, not proven.
- **Multi-agent grow and read.** `bufferLength` is a plain 32-bit field, so
  grow-vs-grow and grow-vs-read/iterate are both unsynchronized data races:
  a reader may see a new length without the matching zero-filled publication,
  or a waiter list may be observed while another agent grows. A multi-agent
  contract needs an atomically published length/capacity (an atomic 64-bit
  length in a future design) plus waiter coordination; this slice claims only
  the single-agent grow-only, stable-pointer, zero-fill contract.
- **Eager-capacity DoS.** `maxByteLength` is allocated and zeroed at
  construction (up to 2^31-1 bytes), so a script can request GB-scale memory
  while exposing a tiny `byteLength`. Lazy commit or a host-configurable limit
  is a product/security decision; no limit was added here.
- **WebAssembly SharedArrayBuffer.** Wasm buffers stay non-JS-growable
  (`growable === false`, `maxByteLength === byteLength`, `grow()` TypeError)
  and keep their reserved/committed `GrowMemory` accounting; Node/Bun agree on
  those JS-visible values. Cross-checking wasm `maxLength` reservation against
  the new JS fields is out of scope.
- **Detach scope.** Only non-shared `ArrayBuffer`s detach (the test uses
  `ArrayBuffer.prototype.transfer()`); shared buffers are never detachable, so
  the SAB view-tracking path has no detach invalidation to handle.
- **Grow shrink semantics.** This slice rejects `grow(newLength < byteLength)`
  with RangeError, matching Node/Bun. Current test262 permits a host-handled
  abrupt completion for `grow-smaller-size`, so the test does not pin the
  error name; the behavior is a deliberate web-reality choice.
- **Engine cap.** `maxByteLength` is capped at `MaxSharedArrayBufferLength`
  (2^31-1 on 64-bit), so values above it are RangeError even where Node may
  accept them. Allocation-limit tests still pass.
- **Reflect.construct/cross-realm order.** The inherited prototype-read
  behavior above is untouched; a SAB-specific constructor-order test still
  fails for that engine reason, not for the new option parsing.
- **TypedArray JIT/TTD.** No bytecode or JIT helper was changed. The test
  profile is no-JIT; TTD/SCA snapshots retain the shared `SharedContents`
  pointer, so `isGrowable`/`maxBufferLength` survive an in-process snapshot,
  but a cross-process snapshot reconstructing `SharedContents` was not
  exercised.
- **B4.** This is Mac ARM64 no-JIT evidence only. Before any B4 claim the
  focused es7 test and the two test262 selections must run on the native Linux
  gate and on JIT profiles; the flag-on S6 surface remains a product/security
  decision and no default is changed here.
