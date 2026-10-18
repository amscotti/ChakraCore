# C2-S13: bounded WeakRef slice

Status: `<pending>` — implemented in the working tree on Mac no-JIT (JIT
evidence is not claimed; B4 owns the JIT configuration claims). This slice
covers `WeakRef` only. `FinalizationRegistry` remains a separate C3 follow-up.

## Scope

1. `new WeakRef(target)` for an Object or an unregistered Symbol target
   (ES2023 `CanBeHeldWeakly`); primitives and `Symbol.for(...)` targets throw
   `TypeError`.
2. `WeakRef.prototype.deref()` with the WeakRef brand check; returns the exact
   live target or `undefined` after the target is cleared.
3. Constructor/prototype shape: `length` 1, `name` `"WeakRef"`, non-writable
   non-configurable `WeakRef.prototype`, non-enumerable `deref` with `length`
   0, `prototype.constructor`, `Symbol.toStringTag`, subclassing and
   `Reflect.construct`/`NewTarget` prototype selection including a cross-realm
   fallback.
4. A recycled weak-reference handle that does not retain the target. No feature
   flag was added and no built-in bytecode property id changed.

Not in scope: `FinalizationRegistry`, `WeakMap.prototype.getOrInsert*`, and
cross-realm `deref` identity beyond the engine's existing `CrossSite` behavior.

## Ownership and lifecycle record

```text
Resource / creation call / source location:
  RecyclerWeakReference<RecyclableObject> entry created by
  Recycler::FindOrCreateWeakReferenceHandle (lib/Runtime/Library/
  JavascriptWeakRef.cpp::JavascriptLibrary::CreateWeakRef); the handle is
  allocated with AllocWeakReferenceEntry (WeakReferenceEntryBits = LeafBit).
Domain and owner:
  The ScriptContext's Recycler owns the object graph. JavascriptWeakRef owns
  the handle through Field(RecyclerWeakReference<RecyclableObject>*)
  targetWeakRef. The global Recycler::weakReferenceMap tracks the handle
  weakly for sweep; it is not a strong owner of the target.
Physical allocation / release:
  Recycler-managed leaf entry plus the normal recycler block containing the
  WeakRef object. Nothing is allocated with malloc/HeapNew and there is no
  native free. The handle is reclaimed by the recycler after the WeakRef is no
  longer marked; target bytes belong to the target's normal owner.
Aliases / publication:
  The handle pointer is private to the WeakRef. deref returns a target pointer
  (marshalled through CrossSite when needed) but does not publish the handle.
Borrow invalidation:
  GC mark/sweep (Recycler::SweepWeakReference) clears handle->strongRef when
  the target's heap block is no longer marked and removes the map entry;
  runtime disposal nulls every weak reference. Target collection and
  WeakRef-instance collection are independent transitions.
User code/reentrancy:
  NewTarget.prototype lookup can run a getter/proxy trap after the target and
  WeakRef have been created; exceptions from that lookup propagate. The
  target is validated before the lookup, per spec.
Thread/context:
  ScriptContext/Recycler and JS execution thread only. There is no
  cross-thread access or background mutation in this slice.
Failure/OOM/shutdown:
  Eligibility is checked before allocation. A target with no recycler
  allocation (host-dispatch/external surface) is rejected with TypeError
  instead of being retained strongly or published. OOM follows the recycler
  allocation path; no partial WeakRef is returned. Recycler shutdown clears
  all weak handles before finalization.
Memory accounting:
  One recycler leaf handle per distinct weak target (shared by
  FindOrCreateWeakReferenceHandle), plus the WeakRef object. No unbounded
  native owner and no explicit retention after the WeakRef dies.
Tests:
  test/es7/weakref.js and the pinned test262 WeakRef corpus; see Evidence.
```

## GC and cleanup contract

`JavascriptWeakRef` is a normal recycler block, so the conservative normal-block
scan reaches its `targetWeakRef` field and marks the handle. The handle is a
leaf block and is not scanned, so its `strongRef` does not trace the target.
During the collection's weak sweep, `Recycler::SweepWeakReference` keeps the
handle only while both the handle and its target are marked; a dead target
turns `deref()` into `undefined`, and a dead WeakRef lets the handle be
removed and reclaimed. The same sweep is used by the existing weak-key regions,
so object targets shared with `WeakMap`/`WeakSet` clear on the same collection.

Unregistered symbol targets use the same typed handle. The library's symbol
cache is already a weak dictionary, so an unregistered symbol with no strong
root can be cleared. Registered `Symbol.for` symbols are rejected before a
handle is created.

`WeakRefDeref`'s spec `AddToKeptObjects`/job-scoped retention is not implemented:
this engine has no job-boundary notification that would let a kept-object set be
cleared at the end of the current job. Deref still returns a target that is
otherwise alive, and an unreachable target clears normally. This is recorded as
a residual liveness refinement; it is not exercised by the pinned WeakRef
corpus and becomes relevant to the FinalizationRegistry/job work.

## Host/test262 GC behavior

- `$262.gc` is not provided by this host. `tools/test262/harness.js` adds only
  `detachArrayBuffer`; test262 GC-timing tests cannot use `$262.gc`.
- The `ch` host provides `-CollectGarbage`, which installs the
  `CollectGarbage()` global (`GlobalObject::EntryCollectGarbage`). The focused
  test uses that flag and performs real exhaustive collections; it does not
  simulate or time a fake collection.
- The pinned `test/built-ins/WeakRef/**/*.js` corpus contains 29 files and 58
  strict/default scenarios. It has no target-clearing scenario, so the focused
  test supplies the object/symbol clearing evidence. One file
  (`prototype/deref/this-does-not-have-internal-target-throws.js`) also requires
  `FinalizationRegistry`, which is intentionally absent and remains a residual
  failure.

## Evidence

Pinned test262 `2e0a5676…`; Mac test profile, no JIT. The before run used the
base-commit binary copied to `out/s13-weakref-before-bin/` so the measured
later build could not replace it.

| Corpus | Before | After | Fail→pass | Pass→fail |
|---|---:|---:|---:|---:|
| `built-ins/WeakRef` (29 files / 58 scenarios) | 0 | **56** | 56 | 0 |

The exact before/after per-file result sets are in
`out/test262-s13-weakref-before/results.json` and
`out/test262-s13-weakref-after2/results.json`. The controls gate passed 18/18
scenarios on both runs. The only remaining failures are the two scenarios of
`prototype/deref/this-does-not-have-internal-target-throws.js`, both
`ReferenceError: 'FinalizationRegistry' is not defined` (the file's
FinalizationRegistry assertion is outside this slice).

Focused `test/es7/weakref.js` covers constructor/call behavior, target identity,
unregistered/registered symbols, descriptors, brand checks, subclassing,
`Reflect.construct`, abrupt `NewTarget.prototype` lookup, coexistence with
WeakMap/WeakSet keys, and object/symbol clearing after two exhaustive
collections. It is registered with `-CollectGarbage -args summary -endargs`
and passes on the test-profile binary. The same script's independent probe
reported 999/1000 object targets and 999/1000 unregistered symbol targets
cleared after two collections; the surviving entries are the expected
conservative stack remnants, not strong retention by the WeakRef.

Commands (all builds/test262 runs serialized by
`/tmp/opencode/cc-gate-lock.sh`; cache and checkout are local):

```sh
mise exec -- zig build --cache-dir "$PWD/.zig-cache"
python3 tools/test262/run.py --binary "$PWD/out/s13-weakref-before-bin/ch" \
  --checkout "$PWD/out/test262" --output "$PWD/out/test262-s13-weakref-before" \
  'test/built-ins/WeakRef/**/*.js'
python3 tools/test262/run.py --binary "$PWD/zig-out/bin/ch" \
  --checkout "$PWD/out/test262" --output "$PWD/out/test262-s13-weakref-after2" \
  'test/built-ins/WeakRef/**/*.js'
(cd test/es7 && ../../zig-out/bin/ch -CollectGarbage -args summary -endargs weakref.js)
```

The test262 host adds `-nonative -Test262 -CollectGarbage` by default; the
WeakRef corpus itself does not call `CollectGarbage`.

## Residual

- `FinalizationRegistry`: not implemented; its cleanup-job contract and
  host-observable finalization remain a separate C3 project. The one WeakRef
  corpus scenario that constructs a FinalizationRegistry stays failed.
- `AddToKeptObjects`/end-of-job kept set: not implemented because this engine
  does not expose a job-end notification to the runtime; `deref` can therefore
  allow a target that is otherwise unreachable to clear before the end of the
  current job. No pinned WeakRef test observes this, and there is no
  FinalizationRegistry observer in this slice.
- S13's symbol entries in `WeakMap`/`WeakSet` remain strongly held by their
  per-collection symbol dictionaries. A symbol that is both a WeakMap key and a
  WeakRef target is therefore kept alive by the WeakMap; this is the
  already-recorded S13 memory-bound residual, now observable through WeakRef.
  Object keys retain the existing weak path.
- Host-dispatch/external JS objects without a recycler allocation are rejected
  rather than weakly retained (TypeError), matching the existing WeakSet
  HostDispatch limitation. No test262 scenario exercises that host surface.
- JIT/TTD/debugger evidence: no JIT claim; no TTD snapshot support was added
  for `[[Target]]` beyond the normal dynamic-object shape. B4 owns the JIT
  matrix. No concurrent-GC/GCStress run was performed; the handle uses the
  existing JsRT `RecyclerWeakReference` primitive, but its concurrent-GC
  behavior must be covered by the B4/GCStress matrix before any such claim.
- The repository's build-contract documents record their pinned 538/682 unit
  baselines; this new TU updates the build/test-table assertions to
  Mac 539 (537 static) and Linux 683 but does not rewrite those historical
  measurement sections.
