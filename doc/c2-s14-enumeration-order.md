# C2-S14 P6: property enumeration order after delete/re-add

Status: **landed in `8b52cc063`** (feature `25318a151`; base `b50fa4d94`).
Mac no-JIT evidence recorded; B4 owns Linux/JIT/GCStress claims. Targeted
P6 selection **18/18**, **10 fail→pass / 0 pass→fail**; broad
Object/JSON/for-in **7,150/190** with the same 10 flips. This slice fixes
only own-property/`for-in` enumeration order for an ordered dictionary after a
property is deleted and added back, plus the in-flight enumeration snapshot that
mutation exposes. The unordered dictionary path and integer-index ordering are
not redesigned.

## Problem

For an ordinary object:

```js
o = {}; o.p1=1; o.p2=2; o.p3=3; o.p4=4;
o[0]=0; o[1]=1; o[2]=2;
delete o.p1; delete o.p3; o.p1=1;
```

`for-in`, `Object.keys/values/entries` and `JSON.stringify` must see
`0,1,2,p2,p4,p1`. Both dictionary handlers kept a deleted descriptor in place and
undeleted it at its old position, producing `0,1,2,p1,p2,p4`. The deletion
tombstone is needed for slot reuse and let/const shadowing, but it must not
determine the enumeration position of a new property.

## Design

Two handlers own the ordered insertion sequence:

- `SimpleDictionaryTypeHandlerBase` (`SimpleDictionaryTypeHandler.cpp`)
- `DictionaryTypeHandlerBase` (`DictionaryTypeHandler.cpp`)

On the undelete paths only, the handler now rebuilds `propertyMap` in one pass:

1. Live entries are copied in their current map order.
2. Ordinary deleted tombstones are dropped; deleted `PropertyLetConstGlobal`
   descriptors stay in place because they still represent a live binding.
3. The re-added descriptor is appended with the same attributes/fixed bits and
   **reuses its old value/accessor slots**. Reusing the slot keeps
   `nextPropertyIndex` bounded and avoids the former fresh-slot growth on
   repeated delete/re-add. `ConvertToTypeHandler` now accepts a descriptor whose
   slot is below the high-water mark by adding it with its explicit slot, so
   instance values stay with their descriptors even when map order and slot order
   diverge.
4. If no live property follows the deleted descriptor, its position is already
   the last insertion position; the handler returns without rebuilding or
   allocating a replacement map. This makes repeated delete/re-add of the same
   last key allocation-free.

The rebuild avoids `BaseDictionary::Remove`: removal frees an entry index in
place, leaving holes that `FindNextProperty`, `GetPropertyId`, and cache index
walks treat as compact. Purging tombstones also resets the simple handler's
`numDeletedProperties` to the remaining deleted let/const count. The map index
is deliberately no longer assumed to equal the property slot;
`GetAttributesWithPropertyIndex` now resolves the descriptor by property id
(the only caller supplies both a slot and the matching id).

Enumeration is now explicitly snapshot-safe for both shared and local caches.
`DynamicObjectPropertyEnumerator` fills the entire key cache at
`Initialize`, so no in-flight walk consults a map that a getter/setter can
rebuild. A local per-enumerator snapshot is used whenever snapshot semantics
were requested but `PrepareForTypeSnapshotEnumeration()` did not lock the type:
the dictionary handlers, and every handler under `-TypeSnapshotEnumeration-`.
Cached keys are checked for current existence and enumerability when returned: a
key deleted before its turn is skipped, a deleted-and-re-added key is returned
exactly once, and keys added after enumeration starts are absent from the
snapshot. Shared caches remain keyed by the initial locked type and are still
used after the object converts to a different type. `Object.assign`/spread skip
a snapshot key whose own property disappeared before the get step, per the
spec's descriptor check.

Enumerability is evaluated against the live property at visit time. Consequently
a property made non-enumerable after the snapshot but before its turn is skipped,
where V8/base may still visit it from the snapshot attributes; this is
spec-defensible and covered by the focused test, as is the reverse case
(non-enumerable at snapshot, made enumerable during the walk) which stays out.

Two exceptions preserve non-insertion semantics:

- `PropertyOperation_Force` is used by `[[DefineOwnProperty]]` when converting an
  accessor to a data property via delete + set. That is a redefinition, so the
  original position is retained. `Object.defineProperty` on a deleted property
  is a new insertion and does move to the end.
- The unordered type handler is untouched: its documented behaviour is
  nondeterministic and it reuses deleted slots.

Integer keys remain handled by `DynamicObject`'s object array and are emitted
ascending before string keys. The string portion follows map order, which is now
creation order for live properties.

## Ownership record

```text
Resource / creation call / source location:
  New JsUtil::BaseDictionary propertyMap allocated with RecyclerNew
  (RecyclerNonLeafAllocator) inside SimpleDictionaryTypeHandler::
  ReAddPropertyAtEnd and DictionaryTypeHandlerBase::ReAddPropertyAtEnd.
Domain and owner:
  ScriptContext Recycler; the type handler owns the map through
  Field(SimplePropertyDescriptorMap*) / Field(PropertyDescriptorMap*).
  The old map becomes unreachable and is collected normally.
Physical allocation / release:
  Recycler non-leaf blocks; keys/descriptors are copied through the existing
  dictionary Add path (including string/symbol write barriers). A local
  enumerator CachedData block is also recycler-owned and referenced by the
  enumerator. No malloc, HeapNew or native free is introduced.
Aliases / publication:
  The map is private to the handler. Descriptor pointers returned by the helper
  are reacquired from the new map; no pointer into the old map is retained.
Borrow invalidation:
  Old propertyMap pointers and descriptor pointers are invalid immediately after
  a rebuild. Callers re-look-up the re-added descriptor. In-flight enumerators
  hold completed key snapshots (strings, stable property indexes and attributes)
  and never dereference the old map during the walk.
User code / reentrancy:
  No user code runs inside the rebuild. The surrounding setter may have already
  invoked prototype caches/invalidation; getters are not called by the copy.
Thread/context:
  Same JS thread and ScriptContext/Recycler as the owning object.
Failure/OOM/shutdown:
  OOM follows the recycler allocation path; the old map is still valid until the
  new map is fully built, and assignment happens only after the appended entry
  is added. Shutdown is unchanged.
Memory accounting:
  The rebuild is O(live properties) and runs only when a re-add has live
  successors in the map. It drops ordinary tombstones and reuses the old slot, so
  repeated delete/readd of the same key does not grow the map or the slot range.
Tests:
  test/es7/property-enumeration-order.js plus the pinned test262 P6 selection
  and the Object/JSON/for-in smoke selection; see Evidence.
```

## Evidence

Pinned corpus `2e0a56762801e275a9fdf96dc49d90ba0cddcf63`, checkout
`out/test262`, local cache/prefix, all `zig build`/test262 commands under
`/tmp/opencode/cc-gate-lock.sh`.

P6 selection (18 scenarios, 9 files):

```sh
python3 tools/test262/run.py --binary "$PWD/out/p6-before-bin/ch" \
  --checkout "$PWD/out/test262" --output "$PWD/out/test262-p6-before" \
  'test/language/statements/for-in/order-*.js' \
  'test/built-ins/Object/keys/return-order.js' \
  'test/built-ins/Object/values/return-order.js' \
  'test/built-ins/Object/entries/return-order.js' \
  'test/built-ins/JSON/stringify/property-order.js'
# before: 8 passed / 10 failed; controls 18/18

python3 tools/test262/run.py --binary "$PWD/zig-out/bin/ch" \
  --checkout "$PWD/out/test262" --output "$PWD/out/test262-p6-final6" \
  'test/language/statements/for-in/order-*.js' \
  'test/built-ins/Object/keys/return-order.js' \
  'test/built-ins/Object/values/return-order.js' \
  'test/built-ins/Object/entries/return-order.js' \
  'test/built-ins/JSON/stringify/property-order.js'
# after: 18 passed / 0 failed; controls 18/18; 10 fail->pass, 0 pass->fail
```

Broad smoke `test/built-ins/Object/**/*.js`, `test/built-ins/JSON/**/*.js`,
`test/language/statements/for-in/**/*.js` (7,340 scenarios, 3,696 files):
before 7,140 passed / 200 failed; after 7,150 passed / 190 failed. Exact result
diff: **10 fail->pass, 0 pass->fail** (`out/test262-p6-smoke-before`,
`out/test262-p6-smoke-final6`).

Focused local test (`test/es7/property-enumeration-order.js`) exercises the
reproducer, integer-key ordering, overwrite non-movement, accessor-to-data
`[[DefineOwnProperty]]`, for-in shadowing, repeated delete/re-add, descriptor
attributes, JSON snapshot exclusion, the two in-flight mutation repros, and
mutation during for-in/JSON/`Object.assign`/spread including a target setter. It
is registered twice in `test/es7/rlexe.xml`, once with default flags and once
with `-TypeSnapshotEnumeration-`. On the base binary: 7/13 bodies pass, 6 fail;
on the fixed binary: 13/13 pass under both entries and the runner reports 2
passed / 0 failed. The registry mutation reproducer is also checked directly:

```sh
ch -TypeSnapshotEnumeration- /tmp/p6-simplemut-min.js
# ["k0","k1",...,"k9"]; before the fix ["k0","k2",...,"k9","k0"]
```

Full local regression (test profile, disable_jit):
`python3 test/runtests.py --test --variants=disable_jit --binary "$PWD/zig-out/bin/ch"`
=> **1990 passed / 0 failed**. This is 1,988 previous scenarios plus the two
new es7 entries. Integration owns the gate pin count; no pin was changed here.

Debug-static regression (assertions enabled):
`mise exec -- zig build -Dprofile=debug-static --cache-dir "$PWD/.zig-cache-debug"`
=> 545/545 steps; `python3 test/runtests.py --debug --variants=disable_jit
--binary "$PWD/out/debug-static/bin/ch"` => **2042 passed / 0 failed**. The P6
test262 selection on that binary is also 18/18
(`out/test262-p6-debug-flag`). One earlier debug run hit a pre-existing
`DebuggerCommon/evaluate.js` exit-code -11 after printing `pass`; that file
passed standalone and the full debug rerun was 2042/0.

Updated local expectations whose encoded order was the old bug:

- `test/Basics/DeleteProperty1.baseline`: `b` re-added after delete now follows
  `c`.
- `test/JSON/toJSON.js`: accessor deleted then redefined, and delete/re-add cases,
  now place the new property last.
- `test/Object/TypeSnapshotEnumeration.baseline`: deleted-before-visit keys are
  skipped (test3) and a key deleted then re-added during enumeration is returned
  once while an added key stays out (tests 7/8), matching the Node behaviour
  checked for the same loop.

Build: `mise exec -- zig build --cache-dir "$PWD/.zig-cache"` => 567/567 steps,
PAL-config 95/95.

## Residual

- `SimpleDictionaryUnorderedTypeHandler` remains nondeterministic by design.
  After an ordered rebuild, map indices and slots can diverge; deleted-slot
  free-list registration then skips those descriptors. This preserves
  correctness but can reduce the unordered handler's reuse bound for a
  hashtable-like object after a re-add. The ordered map drops ordinary
  tombstones, reuses the old slot and avoids the rebuild entirely when the key
  is already last.
- `PropertyLetConstGlobal` descriptors are never reordered. The global/let
  delete/readd interaction remains on its existing path; the new P6 tests do not
  claim a global-object ordering change.
- Shared type caches are now filled eagerly on first enumeration instead of
  lazily. This is required for snapshot safety, but a caller that breaks out of a
  very large `for-in` after the first key still pays one full key scan per type.
  The cache is then reused for later enumerations of that type.
- Under `-TypeSnapshotEnumeration-`, lockable handlers also need a local snapshot
  per enumeration because their type is not locked. This is correct but not
  cached: 20,000 properties and 200 early-exit `for-in` loops take about 242 ms
  versus 4 ms with the default snapshot path and 5 ms on the base binary
  (roughly 60x). The snapshot allocates O(live properties) recycler memory,
  reserving `GetPropertyCountForEnum()` entries (tombstones included), and is
  reclaimed by the collector. This is a performance residual, not a correctness
  caveat; reserving it here avoids any resize during the scan. A future
  type/handler generation counter could cache the local snapshot safely.
- Memory churn: reusing the old slot keeps `nextPropertyIndex` constant, so
  2,000 delete/readds of `k0` stay at about 24 MB RSS (base 24 MB), and 65,000
  stay at about 45 MB (base 45 MB); 65,000 no longer crosses the small-dictionary
  slot limit. A fully populated 1,000-property object plus 2,000 re-adds is about
  25 MB (base 25 MB). A 200,000-iteration synthetic loop reaches about 90 MB in
  both base and fixed binaries from pre-existing delete/type-change allocation;
  `-CollectGarbage` returns it to about 25 MB. Remaining map rebuilds are
  O(live properties) and only occur when the re-added key has a live successor.
- `PathTypeHandler` still converts deletes to a dictionary handler; no
  path-handler re-add order change was needed.
- No TTD, debugger, JIT, GCStress or Linux evidence was produced. B4 owns those
  configurations.

## B4

B4 must run the new es7 test and the P6 test262 selection under the x64 Linux
JIT/WASM profiles and the available stress configurations (including GCStress
where B4 enables it). This slice adds recycler-owned map rebuilds, local
enumeration snapshots and slot-preserving conversion, so B4 should specifically
check: delete/readd under GC, inline/property caches after map reindexing,
simple-dictionary conversion to accessor/dictionary handlers after a rebuild, and
for-in/JSON/Object.assign/spread while a getter mutates the source. No JIT/GC
claim is made here.
