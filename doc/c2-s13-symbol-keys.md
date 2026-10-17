# C2-S13: symbols as WeakMap/WeakSet keys

Status: landed in `9856166dc` on Mac no-JIT; JIT evidence is not claimed
(see B4). No feature flag was added. This slice covers
`symbols-as-weakmap-keys` only; `WeakRef` and `FinalizationRegistry`
remain C3 work.

## Scope

ES2023 `CanBeHeldWeakly` admits an Object or an unregistered Symbol as a
weak-collection key. This batch:

- accepts unregistered symbols in `WeakMap.prototype.set/get/has/delete`
  and `WeakSet.prototype.add/has/delete`, including constructor iterables;
- keeps registered symbols (`Symbol.for(...)`) ineligible: `set`/`add`
  throw `TypeError`, while `get`/`has`/`delete` miss, as the spec's
  `CanBeHeldWeakly` requires;
- keeps object keys on the existing weak path (per-key `WeakMapKeyMap`
  internal property plus weak keySet for `WeakMap`, weak keySet for
  `WeakSet`), including the existing HostDispatch rejection;
- treats `Object(symbol)` as an ordinary object key, not as its primitive
  symbol.

Not in scope: `WeakMap.prototype.getOrInsert`/`getOrInsertComputed`
(ES2026), `WeakRef`/`FinalizationRegistry`, and any change to object-key
weakness.

## Design and ownership record

`JavascriptWeakMap` and `JavascriptWeakSet` each gained a lazy,
recycler-owned symbol dictionary pointer:

```text
JsUtil::BaseDictionary<JavascriptSymbol*, Var /* WeakMap */,
                       Recycler, PowerOf2SizePolicy, RecyclerPointerComparer>
JsUtil::BaseDictionary<JavascriptSymbol*, bool /* WeakSet */,
                       RecyclerNonLeafAllocator, PowerOf2SizePolicy, RecyclerPointerComparer>
```

- **Resource / creation:** the `Field(SymbolMap*) symbolMap` is null until
  the first symbol key is inserted; `EnsureSymbolMap` allocates the
  dictionary from the collection's `Recycler` on that first insertion. The
  vtable-only constructor sets it to null and performs no allocation.
  Empty WeakMaps/WeakSets pay nothing for the symbol feature.
- **Domain and owner:** the collection's `ScriptContext` recycler is the
  allocation domain. The `JavascriptWeakMap`/`JavascriptWeakSet` is the
  sole owner and release authority. No pointer to dictionary storage is
  published outside the collection; `get` returns a value copy and `has`
  a boolean.
- **Tracing:** the WeakMap dictionary's `Var` value makes its entry array
  non-leaf; the WeakSet uses the explicit `RecyclerNonLeafAllocator`
  because its `bool` value would otherwise select leaf storage and leave
  raw `JavascriptSymbol*` keys untraced. Both symbol keys and WeakMap
  values are therefore traced strongly. A dropped-key regression test
  forces GC and verifies fresh symbols cannot hit stale WeakSet entries.
- **Transfer/publication:** `set`/`add` insert key and value; a symbol key
  becomes reachable from the map. Constructor iteration and cross-context
  `get` use the same path, and `EntryGet` still applies
  `CrossSite::MarshalVar` to the returned value.
- **Invalidation:** `delete` removes one entry; `Clear` and
  `Finalize(false)` empty the dictionary. GC never removes a symbol entry,
  because the entry holds the symbol strongly. No dictionary entry pointer
  is retained across user code or a rehash.
- **Release:** `Clear()` clears the symbol dictionary if allocated, which
  drops strong references; the recycler reclaims the dictionary and arrays
  with the collection. There is no native heap allocation to free.
- **Failure:** a rejected key is checked before insertion, so no partial
  entry is published. OOM follows the existing dictionary/recycler OOM
  policy and leaves the dictionary valid. Lazy allocation is not attempted
  until a symbol key is actually accepted.

**Why symbols are strong:** symbols are primitives, and this engine has no
`WeakRef`/`FinalizationRegistry` observable that can see a symbol become
unreachable. The slice therefore keeps symbol entries strongly and bounds
them only by explicit `delete`, `Clear`, or collection lifetime; this
matches all currently observable Node behavior. If WeakRef/FR lands, the
strong-symbol retention must be revisited against their liveness contract
(recorded in Residual). Object keys retain their existing weak semantics
unchanged.

Diagnostics and TTD enumerate the strong symbol entries (and count them
in `Size`): the diagnostic `Map` paths now also yield symbol keys, and
TTD `Map`/snapshot/inflate paths handle them as strong entries rather
than adding them to the weak-reference pin set. Object keys are still
pinned as before.

## Evidence

Pinned test262 `2e0a5676…`, `-nonative -Test262 -CollectGarbage`, final
test binary (`/tmp/s13-weakmap-final3`, `/tmp/s13-weakset-final3`);
debug-static reruns produced the same per-scenario outcomes.

| Corpus | Before | After | Fail→pass | Pass→fail |
|---|---:|---:|---:|---:|
| `built-ins/WeakMap` (281 scenarios) | 214 | **232** | 18 | 0 |
| `built-ins/WeakSet` (170) | 154 | **168** | 14 | 0 |

Every flip is a symbol-key scenario (`iterable-with-symbol-keys`,
`adds-symbol-element`, `delete-*symbol*`, `get/has/*symbol*`,
`returns-this-symbol`, duplicate-symbol and initial-iterable forms).
`throw-if-key-cannot-be-held-weakly` / `throw-when-value-cannot-be-held-weakly`
were already passing and remain passing now that `Symbol.for` is rejected.
The four `getOrInsert*` can-be-held-weakly files are absent-method throws and
are incidental passes, not evidence for the symbol check.

Remaining WeakMap failures are 47 ES2026 `getOrInsert`/
`getOrInsertComputed` scenarios (method absent; the 4
callback-error assertions also fail for that reason) and 2 pre-existing
`proto-from-ctor-realm` SameValue failures. Remaining WeakSet failures are
the same 2 pre-existing `proto-from-ctor-realm` failures. No remaining
failure is a symbol-key rejection or a symbol-entry lookup defect.

Focused `test/es7/weakcollections-symbols.js` (registered with
`-CollectGarbage -args summary -endargs`) covers set/has/get/delete,
overwrite, well-known symbols, wrapper-vs-primitive separation,
registered-symbol rejection, non-key primitives, WeakSet add/has/delete,
constructor iterables, cross-collection isolation, a GC loop where symbol
entries and a value held only by the map survive, and a dropped-key
WeakSet regression that allocates 20,000 symbols, forces two collections,
then verifies 50,000 fresh symbols do not hit stale entries. It does not
assert object-key collection (that is not observable here). It passes on
the test and debug-static binaries. A direct probe run under both
`ch -CollectGarbage` and Node v26.5.0 `--expose-gc` produced byte-identical
output for the same operations.

## Gates

- `mise exec -- zig build`: pass.
- `mise exec -- zig build check --summary line`: 1977/0, bytecode up to
  date, 572/572 steps.
- `mise exec -- zig build check-regression -Dprofile=debug-static
  --summary line`: 2029/0, 545/545 steps.
- `python3 tools/build/check_regression.py --self-test`: all negative
  controls behave.
- No bytecode or `PropertyId` change; no flag change.

## Residual

- ES2026 `WeakMap.prototype.getOrInsert` / `getOrInsertComputed` (47
  scoped failures) stay out of this slice; their symbol-key cases will
  exercise the same symbol map once the methods exist.
- `WeakRef` and `FinalizationRegistry` remain C3; this receipt makes no
  cleanup-job, retention or finalization claim.
- The 2 `proto-from-ctor-realm` failures per collection are pre-existing
  cross-realm prototype identity defects, not weak-key behavior.
- Symbol-keyed entries are strongly retained. This is not observable with
  today's language surface (WeakRef/FR absent), but it is a memory bound:
  a long-lived collection keeps symbol keys and WeakMap values until
  delete/Clear/collection death. Revisit when S13 C3 adds WeakRef/FR.
- Registered-symbol rejection reuses the existing
  `WeakMapSetKeyNotAnObject` message ("key is not an object"), which is
  type-correct but wording-wise inaccurate for a registered symbol.

## B4 note

The change is runtime-library/ThreadContext C++ only; no JS bytecode, JIT
helper, or common data-structure constructor changed. `Map`/TTD paths are
not on the JIT execution path. Execution evidence is Mac no-JIT only; B4
still owns JIT-configuration and native-runner claims.
