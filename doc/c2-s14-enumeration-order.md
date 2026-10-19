# C2-S14 P6: property enumeration order after delete/re-add

Status: **`<pending>`** (implementation and Mac no-JIT evidence recorded; B4 owns
Linux/JIT/GCStress claims). Base `b50fa4d94`; branch `s14-p6`. This slice fixes
only own-property/`for-in` enumeration order for an ordered dictionary after a
property is deleted and added back. The unordered dictionary path, integer-index
ordering and the `for-in` snapshot model are not redesigned.

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
3. The re-added descriptor is appended with the same attributes/fixed bits.
4. For `SimpleDictionaryTypeHandler`, the re-added property gets a fresh
   monotonically increasing slot (`nextPropertyIndex`) so map order and slot
   order stay compatible with `ConvertToTypeHandler` (which transfers values by
   slot). `DictionaryTypeHandler` keeps its existing data/getter/setter slots
   because enumeration is map-ordered and its accessor slots are independent.

The rebuild avoids `BaseDictionary::Remove`: removal frees an entry index in
place, leaving holes that `FindNextProperty`, `GetPropertyId`, and cache index
walks treat as compact. Purging tombstones also resets the simple handler's
`numDeletedProperties` to the remaining deleted let/const count. The map index
is deliberately no longer assumed to equal the property slot;
`GetAttributesWithPropertyIndex` now resolves the descriptor by property id
(the only caller supplies both a slot and the matching id).

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
  dictionary Add path (including string/symbol write barriers). No malloc,
  HeapNew or native free is introduced.
Aliases / publication:
  The map is private to the handler. Descriptor pointers returned by the helper
  are reacquired from the new map; no pointer into the old map is retained.
Borrow invalidation:
  Old propertyMap pointers and descriptor pointers are invalid immediately after
  the rebuild. Callers re-look-up the re-added descriptor. Enumeration caches
  are type-keyed and only exist for locked types; mutation of a locked handler
  already converts to a non-shared handler/type, so no stale cache outlives the
  rebuild.
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
  The rebuild is O(live properties) and runs only on a re-add. It drops ordinary
  tombstones, so repeated delete/readd of the same keys does not grow the map.
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
  --checkout "$PWD/out/test262" --output "$PWD/out/test262-p6-final2" \
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
`out/test262-p6-smoke-final2`).

Focused local test (`test/es7/property-enumeration-order.js`) exercises the
reproducer, integer-key ordering, overwrite non-movement, accessor-to-data
`[[DefineOwnProperty]]`, for-in shadowing, repeated delete/re-add, descriptor
attributes and JSON snapshot exclusion. It is registered in `test/es7/rlexe.xml`.
On the base binary: 3/7 bodies pass, 4 fail; on the fixed binary: 7/7 pass and
the runner reports 1 passed / 0 failed.

Full local regression (test profile, disable_jit):
`python3 test/runtests.py --test --variants=disable_jit --binary "$PWD/zig-out/bin/ch"`
=> **1989 passed / 0 failed**. This is 1,988 previous scenarios plus the new
es7 test. Integration owns the gate pin count; no pin was changed here.

Debug-static regression (assertions enabled):
`mise exec -- zig build -Dprofile=debug-static --cache-dir "$PWD/.zig-cache-debug"`
=> 545/545 steps; `python3 test/runtests.py --debug --variants=disable_jit
--binary "$PWD/out/debug-static/bin/ch"` => **2041 passed / 0 failed**. The P6
test262 selection on that binary is also 18/18 (`out/test262-p6-debug`).

Updated local expectations whose encoded order was the old bug:

- `test/Basics/DeleteProperty1.baseline`: `b` re-added after delete now follows
  `c`.
- `test/JSON/toJSON.js`: accessor deleted then redefined, and delete/re-add cases,
  now place the new property last.
- `test/Object/TypeSnapshotEnumeration.baseline`: tests 7/8 for
  Dictionary/ES5Array now omit the re-added `q` and show `x` (the latter is the
  pre-existing snapshot/add-during-enumeration limitation, not a new ordering
  claim).

Build: `mise exec -- zig build --cache-dir "$PWD/.zig-cache"` => 567/567 steps,
PAL-config 95/95.

## Residual

- `SimpleDictionaryUnorderedTypeHandler` remains nondeterministic by design. After
  an ordered rebuild, map indices and slots can diverge; deleted-slot free-list
  registration then skips those descriptors. This preserves correctness but can
  reduce the unordered handler's slot-reuse bound for a hashtable-like object
  after a re-add. The ordered map itself does not retain ordinary tombstones, so
  repeated delete/readd of the same key does not grow it.
- `PropertyLetConstGlobal` descriptors are never reordered. The global/let
  delete/readd interaction remains on its existing path; the new P6 tests do not
  claim a global-object ordering change.
- `for-in` snapshot behaviour for mutations during enumeration is not redesigned.
  The updated `TypeSnapshotEnumeration` baseline records the exact delta: a
  re-added property is no longer observed in the same enumeration, while
  properties added during enumeration can still appear under the existing
  snapshot limitation.
- `PathTypeHandler` still converts deletes to a dictionary handler; no path-handler
  re-add order change was needed.
- No TTD, debugger, JIT, GCStress or Linux evidence was produced. B4 owns those
  configurations.

## B4

B4 must run the new es7 test and the P6 test262 selection under the x64 Linux
JIT/WASM profiles and the available stress configurations (including GCStress
where B4 enables it). This slice adds a recycler-owned dictionary map and
slot-preserving descriptor copies, so B4 should specifically check:
delete/readd under GC, inline/property caches after map reindexing, and
simple-dictionary conversion to accessor/dictionary handlers after a rebuild.
No JIT/GC claim is made here.
