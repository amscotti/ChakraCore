# Track Q2: Everyday ES2026 Wave 2 & Conformance Tail

Status: Completed on `modernize/phase4` (macOS ARM64; Linux x64 revalidation queued in Track `G0-phase4`).
`modernize/baseline` remains untouched at `1bf42d1f63e3f09b13ac782504b8cb59479890db`.
`lib/Runtime/InternalPropertyList.h`, `lib/Runtime/Library/InJavascript/Intl.js`, and `lib/Runtime/Library/InJavascript/JsBuiltIn*.bc.*.h` remain completely unchanged relative to `modernize/baseline` (verified by `git diff` and `mise exec -- zig build verify-bytecode`).

---

## 1. Scope & Integrated Commits

Track `Q2` ships the three everyday ES2026 Wave 2 standard-library slices plus two targeted runtime/scanner fixes on `modernize/phase4`:

| Slice | Wave 1 SHA | `modernize/phase4` SHA | Files Changed | Key Impact |
|---|---|---|---|---|
| **Wave 0 scaffold & debugger baselines** | `8fc564fb3` (`q2-scaffold`) | `8fc564fb3` | `JavascriptBuiltInFunctionList.h`, `JSON.{h,cpp}`, `IntlEngineInterfaceExtensionObject.cpp`, `JavascriptMap.{h,cpp}`, `JavascriptWeakMap.{h,cpp}`, `JavascriptIterator.{h,cpp}`, `JavascriptLibrary.cpp`, `test/DebuggerCommon/{bug_543550,frames_values_mapES6}.js.dbg.baseline` | Built-in slot & profiler wiring (`Map.prototype` `13->15`, `WeakMap.prototype` `6->8`, `JSON` `3->5`, `Iterator` `4->5`), `kRawJsonHiddenSentinel = 0x124A534E`, `EntryIntl_GetHiddenObject` `DynamicObject` guard |
| **1. `Iterator.concat`** | `4231b5607` (`q2-iter-concat`) | `30bc5ad00` | `JavascriptIterator.{h,cpp}`, `test/es7/iterator_concat.js` | `IteratorHelperKind::Concat`, eager left-to-right `GetMethod` validation, lazy inner iterator opening & forwarding, **`64/64`** Test262 (`+58`) |
| **2. `Map` & `WeakMap` upsert** | `2b5e9d314` (`q2-upsert`) | `3fe1fb95a` | `JavascriptMap.cpp`, `JavascriptWeakMap.cpp`, `test/es7/map_weakmap_getorinsert.js` | `getOrInsert` & `getOrInsertComputed` (`-0 -> +0` canonicalization, `IsCallable` pre-check, `CanBeHeldWeakly` + symbol keys, TTD parity, safe reentrancy), **`142/142`** Test262 (`+96`) |
| **3. Cross-realm revoked Proxy `[[Construct]]` & `StrToDbl` separator scaling** | `7c2860b83` (`q2-buglets`) | `862882548` | `JavascriptProxy.cpp`, `JavascriptOperators.cpp`, `NumberUtilities_strtod.cpp`, `test/Number/numeric_separator_strtod_and_proxy_realm.js` | Caller-realm revoked-proxy `TypeError` (`base-ctor-revoked-proxy-realm.js` **`2/2`**) + `StrToDbl` separator skipping (`numeric-separator-literal` **`318/318`**, `duration-out-of-range-2.js` **`2/2`**) |
| **4. `JSON.rawJSON`, `isRawJSON`, & `JSON.parse` `context.source`** | `df9320520` (`q2-json-raw`) | `4508af170` | `JSON.{h,cpp}`, `JSONParser.{h,cpp}`, `JSONScanner.{h,cpp}`, `JSONStringBuilder.cpp`, `JSONStringifier.{h,cpp}`, `LazyJSONString.{h,cpp}`, `test/JSON/json_rawjson_and_source.js` | Null-prototype frozen `RawJSON` sentinel objects, verbatim stringify & `LazyJSONString::TryParse()` bypass, Recycler-traced `JsonParseNode` snapshot tree for `reviver(key, value, context)`, **`76/76`** targeted Test262 (`+42` `json-parse-with-source`) |
| **Wave 2 integration, test wiring, gate counts, & docs** | — | Wave 2 integration commit | `test/JSON/arguments.js`, `test/{es7,JSON,Number}/rlexe.xml`, `tools/build/gate_counts.json`, `doc/{q2-es2026-wave2,vision-and-roadmap,open-cards,i2-duration-enumeration}.md` | Updated `arguments.js` for 3rd `context` arg, wired 4 regression tests (`2082/2134 -> 2086/2138`), verified full matrix & Test262 refresh |

---

## 2. Implementation Summary

### 2.1 `Iterator.concat` (`30bc5ad00`)
- Added `IteratorHelperKind::Concat`, Recycler-traced `Field(JavascriptArray*) concatIterables` and `Field(uint32) concatIndex` on `JavascriptIteratorHelper`, and `ClearConcatState()`.
- `JavascriptIterator::EntryConcat` eagerly validates every argument left-to-right (`JavascriptOperators::IsObject`, `CrossSite::MarshalVar`, `GetPropertyNoCache` for `PropertyIds::_symbolIterator`, `JavascriptConversion::IsCallable`) without invoking any `[Symbol.iterator]()` method, storing `{ openMethod, itemObj }` pairs into `concatIterables` via `GenericDirectSetItemAt`, and passes `concatIterables` via `CreateIteratorHelper`'s `callback` argument (moved into `concatIterables` with `callback = nullptr` in the constructor).
- `Advance` opens inner iterators lazily with 0 arguments, resolves `GetIteratorDirect`, steps via `StepInner` without reading `value` after `done: true`, clears `concatIterables` upon opening the last item or on close/completion/abrupt unwind (`ClearConcatState()`), and forwards `EntryReturn` with suspended-start no-op + started `executing` reentrancy checks.

### 2.2 `Map.prototype` & `WeakMap.prototype` `{getOrInsert, getOrInsertComputed}` (`3fe1fb95a`)
- `JavascriptMap.cpp`: `RequireMapReceiver`, `CanonicalizeMapKey` (`-0.0 -> +0.0` before lookup, `callbackfn`, and `Set`), `IsCallable(callbackfn)` checked before lookup, `AUTO_TAG_NATIVE_LIBRARY_ENTRY` + `BEGIN_SAFE_REENTRANT_CALL` around `CALL_FUNCTION(..., undefined, key)`, unconditional post-callback `map->Set(key, value)` (handling reentrant `set`/`delete`/`clear`/promotion/GC), and `CrossSite::MarshalVar`.
- `JavascriptWeakMap.cpp`: `RequireWeakMapReceiver`, `IsCallable(callbackfn)` checked before `TryExtractWeakMapKey` (`CanBeHeldWeakly`: unregistered `JavascriptSymbol` or `IsObject`), `WeakMapGetEntry` / `WeakMapSetEntry` preserving `ENABLE_TTD` record/replay and `TTDWeakReferencePinSet` semantics, `AUTO_TAG_NATIVE_LIBRARY_ENTRY` + `BEGIN_SAFE_REENTRANT_CALL`, fresh post-callback `WeakMap::Set`, and `CrossSite::MarshalVar`.

### 2.3 Cross-Realm Revoked Proxy `[[Construct]]` & `StrToDbl` Scaling (`862882548`)
- `JavascriptOperators::GetFunctionRealm` checks `proxy->IsRevoked()` before `proxy->GetTarget()` and throws `JSERR_ErrorOnRevokedProxy` in `requestContext` while walking `BoundFunction::GetDirectTargetFunction()`.
- `JavascriptOperators::NewScObjectHostDispatchOrProxy` passes `requestContext` to `GetPrototypeObject` and `CrossSite::MarshalVar(requestContext, prototype)`. `JavascriptProxy::FunctionCallTrap` passes `requestContext` to `NewScObjectNoCtorCommon` on the non-super-call path, marshals `newThisObject` to `scriptContext`, and omits the redundant post-lookup `IsRevoked()` throw so revoking while returning an object prototype succeeds per ES2025.
- `NumberUtilities::StrToDbl` (`lib/Common/Common/NumberUtilities_strtod.cpp`) walks `pchLimDig` backwards over `numExcessiveDigits` actual digits skipping `'.'` and `'_'`, and skips `'_'` between leading `'0'` digits after `'.'` in `LGetRight`.

### 2.4 `JSON.rawJSON`, `JSON.isRawJSON`, & `JSON.parse` `context.source` (`4508af170`)
- `JSON::RawJSON` validates `ToString(text)`, rejects empty input, leading/trailing JSON whitespace (`0x09`/`0x0A`/`0x0D`/`0x20`), objects/arrays, whitespace after `'-'`, and trailing tokens/NULs while allowing arbitrary valid ECMA-404 primitives (including large integers, `BigInt`, and double-overflow numbers) under `TryFinally` scanner finalization; creates a null-prototype `DynamicObject` with `InternalPropertyIds::HiddenObject = TaggedInt::ToVarUnchecked(kRawJsonHiddenSentinel)` (`0x124A534E`), own `"rawJSON"` data property, and `Freeze()`.
- `IntlEngineInterfaceExtensionObject::EntryIntl_GetHiddenObject` (`8fc564fb3`) guards against `nullptr`, `TaggedInt::Is`, `JavascriptNumber::Is_NoTaggedIntCheck`, non-`DynamicObject` vtables, and `!DynamicObject::IsBaseDynamicObject`, isolating legacy `Intl.js` even under polluted `Number.prototype`.
- `JSONStringifier` / `LazyJSONString`: added `JSONContentType::RawJSON` and `hasRawJSON`; `ReadProperty` checks `IsRawJSONObject` before `ReadObject`; `LazyJSONString::TryParse()` returns `nullptr` when `hasRawJSON` is set; `JSON::Parse` bypasses `LazyJSONString::TryParse()` when `reviver != nullptr`.
- `JSONScanner` & `JSONParser`: tracks `tokenStartOffset` / `tokenEndOffset`, rejects whitespace between `'-'` (`tkSub`) and digits, allocates a Recycler-traced `JsonParseNode` tree only when `reviver != nullptr && inputString != nullptr`, matches objects by identity and primitives by `SameValue` (last key wins on duplicates), and passes `context` (`{ source }` or `{}`) as the 3rd `reviver` argument.

---

## 3. Ownership Records (`doc/memory-management.md` Section 3)

### 3.1 `Map` / `WeakMap` `getOrInsertComputed` (`JavascriptMap.{h,cpp}`, `JavascriptWeakMap.{h,cpp}`)
```text
Resource / creation call / source location:
  - `JavascriptMap` (`JavascriptMap.h:34-45`): `Field(MapDataList) list`, `Field(MapUnion) u`
    (`SimpleVarDataMap*` / `ComplexVarDataMap*` Recycler dictionaries), `Field(MapKind) kind`.
  - `JavascriptWeakMap` (`JavascriptWeakMap.h:62-63`): `Field(KeySet) keySet` (weakly referenced
    `RecyclableObject*` set) + `Field(SymbolMap*) symbolMap` (strong Recycler dictionary for
    unregistered `JavascriptSymbol` keys); object-key values live in per-key `WeakMapKeyMap`
    (`JsUtil::BaseDictionary<WeakMapId, Var, Recycler, ...>`) on `InternalPropertyIds::WeakMapKeyMap`.
Domain and allocator instance (including backing allocator lifetime):
  - Owning `ScriptContext`'s `Recycler` GC heap; C++ stack locals (`map`, `weakMap`, `key`,
    `keySym`, `keyObj`, `callbackfn`, `value`) are conservatively stack-scanned.
Physical allocation base, extent, alignment, release callback or pool kind:
  - Recycler-managed objects and `BaseDictionary` buckets with `Field(...)` write barriers;
    `JavascriptWeakMap::Finalize` calls `Clear()` on non-shutdown GC finalization to scrub
    per-key `WeakMapKeyMap` slots.
Logical length / borrowed views (kept separate from the allocation descriptor):
  - No dictionary node pointer (`MapDataNode*`), bucket pointer, or `WeakMapKeyMap*` is cached
    or retained across `CALL_FUNCTION(..., callbackfn, ..., undefined, key)`.
Owner; transfer/publication point; remaining aliases:
  - Publication occurs solely after `callbackfn` returns normally via `map->Set(key, value)` or
    `WeakMapSetEntry(weakMap, scriptContext, keySym, keyObj, value)`.
Borrow invalidation: GC / detach / growth / mutation / reset / close / disposal:
  - Reentrant `callbackfn` may call `set`, `delete`, `clear`, insert non-simple keys (promoting
    `EmptyMap`/`SimpleVarMap -> ComplexVarMap`), trigger GC, or mutate cross-realm objects.
    Post-callback `map->Set` / `WeakMapSetEntry` re-reads `kind` / `u.*` / `WeakMapKeyMap` /
    `symbolMap` fresh from the receiver/key, overwriting in place if inserted or appending if absent.
Potential user-code or reentrancy points:
  - `CALL_FUNCTION(scriptContext->GetThreadContext(), callbackfn, CallInfo(CallFlags_Value, 2), undefined, key)`
    wrapped in `AUTO_TAG_NATIVE_LIBRARY_ENTRY` + `BEGIN_SAFE_REENTRANT_CALL`.
Thread/context allowed for use, root changes and destruction:
  - Bound `ScriptContext` script thread (`CrossSite::MarshalVar` on return; `WeakMapSetEntry`
    preserves `ENABLE_TTD` `TTDWeakReferencePinSet->AddNew(keyObj)` in replay mode).
Release on success, partial failure, exception, OOM, cancel and shutdown:
  - If `Require*Receiver`, `IsCallable`, `TryExtractWeakMapKey`, or `callbackfn` throws, or
    `CanonicalizeMapKey` / `Set` hits OOM, no entry is committed; Recycler reclaims unreachable
    containers on sweep/shutdown.
Memory accounting, capacity/retention bound and exhaustion behavior:
  - Bounded by Recycler GC budgets (`PowerOf2SizePolicy` / `PrimePolicy` dictionaries); symbol
    keys remain strongly held in `symbolMap` per ES2026 spec until `delete`/`clear`/finalization.
Tests / commands / measured results / remaining uncertainty:
  - `test/es7/map_weakmap_getorinsert.js` (reentrant `set`/`delete`/`clear`/resize, `CollectGarbage()`,
    JIT-warmed loops, cross-realm), Test262 `upsert` `142/142`, `symbols-as-weakmap-keys` `55/55`,
    ASan & UBSan `-CollectGarbage` PASS.
```

### 3.2 `JSON.rawJSON` `HiddenObject` Sentinel, `JsonParseNode` Tree, & `arguments.js` (`JSON.{h,cpp}`, `IntlEngineInterfaceExtensionObject.cpp`, `JSONParser.{h,cpp}`)
```text
Resource / creation call / source location:
  - `JSON::RawJSON` (`JSON.cpp:258-266`): null-prototype frozen `DynamicObject` carrying
    `InternalPropertyIds::HiddenObject = TaggedInt::ToVarUnchecked(kRawJsonHiddenSentinel)`
    (`0x124A534E`, `JSON.h:16`) and own `"rawJSON"` data property (`JavascriptString*`).
  - `JSONParser` (`JSONParser.h:10-35`, `JSONParser.cpp:309-504`): `RecyclerNew` `JsonParseNode`
    tree (`Field(Js::Var) parsedValue`, `Field(JsonParsePropertyMap*) properties`,
    `Field(JsonParseElementList*) elements`) allocated only when `reviver != nullptr`.
  - `test/JSON/arguments.js`: legacy regression updated from 2-arg `reviver` expectation to
    ES2026 3-arg `reviver(key, value, context)` (`{"source":"\"first\""}`).
Domain and allocator instance (including backing allocator lifetime):
  - `ScriptContext`'s `Recycler` GC heap (with `Field(...)` write barriers on `JsonParseNode`,
    `BaseDictionary<PropertyId, JsonParseNode*, Recycler>`, `List<JsonParseNode*, Recycler>`, and
    `LazyJSONString::hasRawJSON`); `JSONParser` itself is stack-allocated.
  - `arenaAllocatorObject` (`TempGuestArenaAllocatorObject`) holds only non-GC `JsonTypeCache`
    and scanner unescape character-range pairs.
Physical allocation base, extent, alignment, release callback or pool kind:
  - Tagged sentinel (`TaggedInt`, `0x124A534E`) is a non-heap immediate `Var`; `DynamicObject`,
    `JsonParseNode` tree, and `sourceStr` (`JavascriptString::NewCopyBuffer`) are Recycler-owned.
  - `EntryIntl_GetHiddenObject` (`IntlEngineInterfaceExtensionObject.cpp:3531-3545`) explicitly
    rejects `nullptr`, `TaggedInt::Is`, `FLOATVAR` numbers, and non-`DynamicObject` vtables,
    returning `undefined` if `Intl` methods are called on a `RawJSON` object.
Logical length / borrowed views (kept separate from the allocation descriptor):
  - `JsonParseNode` records `uint32 sourceStart` and `uint32 sourceLength` offsets into
    `JSONParser::inputString`; slices are materialized via `JavascriptString::NewCopyBuffer(
    this->inputString->GetSz() + parseNode->sourceStart, parseNode->sourceLength, scriptContext)`
    only when `SameValue(val, parseNode->parsedValue)` holds in `WalkProperty`.
Owner; transfer/publication point; remaining aliases:
  - `JSON::RawJSON` returns the frozen `DynamicObject*` to JS; no off-stack native handles remain.
  - Stack-allocated `JSONParser` holds `inputString` and `rootParseNode` across `Parse` and `Walk`;
    each `WalkProperty` frame holds stack-scanned locals (`holder`, `val`, `parseNode`, `keys`,
    `keyStr`, `keyRecord`, `newElement`, `context`) across child walks and `reviver` calls.
  - `JSONParser::Finalizer()` clears `rootParseNode = nullptr`, `inputString = nullptr`, and
    releases `arenaAllocatorObject` in `TryFinally`.
Borrow invalidation: GC / detach / growth / mutation / reset / close / disposal:
  - `inputString` stays pinned on `JSONParser` on stack so `GetSz()` remains valid across reentrant
    `reviver` / getter / Proxy traps / GC.
  - Object `parseNode` matching checks pointer identity (`parseNode->parsedValue == val`);
    primitive matching checks `JavascriptConversion::SameValue(val, parseNode->parsedValue)`,
    returning `{}` (without `source`) when a getter/forward-reviver swaps the value.
Potential user-code or reentrancy points:
  - `JSON::RawJSON`: `JavascriptConversion::ToString(textArg, scriptContext)` completes before
    `JSONScanner` runs; `TryFinally` ensures `scanner.Finalizer()` runs on syntax errors.
  - `JSONParser::WalkProperty`: `GetItem`/`GetProperty`, `IsArrayOrThrowIfRevoked`, `OP_GetLength`,
    `GetOwnEnumerablePropertyNames`, `DeleteItem`/`DeleteProperty`, `CreateDataProperty`, and
    `BEGIN_SAFE_REENTRANT_CALL` around `CallFunction(reviver)`.
Thread/context allowed for use, root changes and destruction:
  - Bound `ScriptContext` script thread (`CrossSite::MarshalVar` in `IsRawJSONObject`).
Release on success, partial failure, exception, OOM, cancel and shutdown:
  - `TryFinally` unwinds `JSONScanner` / `JSONParser` guest arenas on normal return, syntax
    error, or JS exception; Recycler collects unreachable `JsonParseNode` trees and `RawJSON` objects.
Memory accounting, capacity/retention bound and exhaustion behavior:
  - `PROBE_STACK` guards recursive `ParseObject` and `WalkProperty`; `sourceStart + sourceLength <=
    inputString->GetLength()`; `ReadProperty` uses `UInt32Math::Add` for `totalStringLength` overflow.
Tests / commands / measured results / remaining uncertainty:
  - `test/JSON/json_rawjson_and_source.js` (with `-CollectGarbage` & `-ForceGCAfterJSONParse`),
    `test/JSON/arguments.js`, Test262 `JSON/{rawJSON,isRawJSON,parse/reviver-*}` `76/76`,
    `json-parse-with-source` `42/42`, `check-gcstress`, ASan & UBSan `-CollectGarbage` PASS.
```

### 3.3 `JavascriptIteratorHelper` `Concat` (`lib/Runtime/Library/JavascriptIterator.{h,cpp}`)
```text
Resource / creation call / source location:
  - `JavascriptIteratorHelper` (`JavascriptIterator.h:84-103`) allocated via
    `JavascriptLibrary::CreateIteratorHelper` with `IteratorHelperKind::Concat`:
    traced fields `Field(JavascriptArray*) concatIterables`, `Field(RecyclableObject*) innerIterator`,
    `Field(Var) innerNextMethod`, `Field(uint32) concatIndex`, and `Field(bool) hasInner`.
Domain and allocator instance (including backing allocator lifetime):
  - `ScriptContext`'s `Recycler` GC heap (`DynamicObject` subclass with `Field(...)` write barriers).
Physical allocation base, extent, alignment, release callback or pool kind:
  - One Recycler `JavascriptArray*` of length `itemCount * 2` (`openMethod`, `itemObj` pairs via
    `GenericDirectSetItemAt`) plus the `JavascriptIteratorHelper` `DynamicObject`.
Logical length / borrowed views (kept separate from the allocation descriptor):
  - `concatIndex` steps by `2` over `concatIterables->GetLength()`; `CrossSite::MarshalVar` runs
    eagerly on each `itemObj` in `EntryConcat` and lazily on `openMethod`, `iterable`,
    `innerIterator`, `innerNextMethod` before calls.
Owner; transfer/publication point; remaining aliases:
  - `EntryConcat` passes `concatIterables` through `CreateIteratorHelper`'s `callback` argument;
    `JavascriptIteratorHelper` constructor moves it into `concatIterables` and clears `callback = nullptr`
    and `iteratorDone = true`.
  - No native heap memory or external handles are allocated; sole owner is the JS helper object.
Borrow invalidation: GC / detach / growth / mutation / reset / close / disposal:
  - All stored GC pointers (`concatIterables`, `innerIterator`, `innerNextMethod`) carry `Field(...)`
    write barriers across user-code calls (`openMethod.call(iterable)`, `innerNext.call(innerIter)`,
    and `return()`).
  - Once `concatIndex >= concatIterables->GetLength()`, `Advance` immediately drops `concatIterables = nullptr`;
    `ClearConcatState()` clears `concatIterables`, `concatIndex`, `hasInner`, `innerIterator`, and
    `innerNextMethod` on exhaustion, suspended-start/started `EntryReturn`, or `EntryNext`/`EntryReturn`
    exception unwinds after `CloseAllSwallow`.
Potential user-code or reentrancy points:
  - Eager `GetPropertyNoCache(itemObj, _symbolIterator)` in `EntryConcat`; lazy `CallVar(openMethod, iterable)`,
    `GetIteratorDirect` (`GetProperty("next")`), `StepInner`, and `CloseInnerNormal` / `CloseAllSwallow`
    guarded by `executing` (`JSERR_GeneratorAlreadyExecuting`) and `started` / `completed` flags.
Thread/context allowed for use, root changes and destruction:
  - Bound `ScriptContext` script thread (`DEFINE_MARSHAL_OBJECT_TO_SCRIPT_CONTEXT(JavascriptIteratorHelper)`).
Release on success, partial failure, exception, OOM, cancel and shutdown:
  - Eager validation failure in `EntryConcat` throws before creating `JavascriptIteratorHelper`,
    leaving `concatIterables` stack-unwound for Recycler collection; runtime errors in `EntryNext` /
    `EntryReturn` close the active inner iterator, call `ClearConcatState()`, reset `executing = false`,
    and rethrow.
Memory accounting, capacity/retention bound and exhaustion behavior:
  - `itemCount > UINT32_MAX / 2` throws `RangeError` before allocating `concatIterables`.
Tests / commands / measured results / remaining uncertainty:
  - `test/es7/iterator_concat.js` (`-CollectGarbage`), Test262 `Iterator/concat` `64/64` (`iterator-sequencing`
    `64/64`), `check_fulljit` `es7` `119/0`, ASan & UBSan `-CollectGarbage` PASS.
```

---

## 4. Multi-Profile Verification (`modernize/phase4`, macOS ARM64)

All 4 new regression tests are registered in `test/{es7,JSON,Number}/rlexe.xml` (`-CollectGarbage`), `test/JSON/arguments.js` is updated for the ES2026 3-argument `reviver(key, value, context)`, and [tools/build/gate_counts.json](../tools/build/gate_counts.json) is bumped to `2086/0` (`test`) and `2138/0` (`debug`).

| Command / Profile | Result | Observed Details |
|---|---|---|
| `mise exec -- zig build verify-bytecode` | PASS (`exit 0`) | `Bytecode is up to date`, `verify_bytecode_pure: source headers unchanged; regen exit 0` |
| `mise exec -- zig build check-selftest` | PASS | `pal-config: 95/95` (`test`), `u8a_codec.zig`, `owned-bytes.zig`, build-tool negative controls |
| `mise exec -- zig build check-build-tools` | PASS | `Ran 97 tests in 16.602s`, `OK` |
| `mise exec -- zig test tools/linux-gate-tests.zig` | PASS | `All 9 tests passed.` |
| `mise exec -- zig build check` (`test` profile) | PASS | `Total: passed 2086, failed 0` (`JSON 16/0`, `Number 13/0`, `es7 118/0`), `test-shared-basic : PASS`, `verify-bytecode` exit 0, `check_smoke: PASS`, `check_metadata: PASS` (`18495 T symbols`) |
| `mise exec -- zig build -Dprofile=debug-static check` | PASS | `Total: passed 2138, failed 0` (`JSON 18/0`, `Number 13/0`, `es7 118/0`), `535` archive members (`61655 T symbols`), `8/8` static native suites PASS, `verify-bytecode` exit 0, `check_metadata: PASS` |
| `mise exec -- zig build -Dprofile=release-shared check` | PASS | `test-shared-basic : PASS`, `check_metadata: PASS`, `check_smoke: PASS`; all 4 Q2 regression tests pass directly on `out/release-shared/bin/ch` |
| `mise exec -- zig build -Dprofile=jit -Dwasm=true check-wasm check-fulljit check` | PASS | `check_wasm_p1: PASS: P1 pass 382/382` (`runtests Total passed 448, failed 5`), `check_fulljit: PASS (passed 622, failed 0)`: `Basics 48/0`, `Array 123/0`, `Function 68/0`, `Regex 40/0`, `es6 224/0`, `es7 119/0` (`+2`), JIT `verify-bytecode` PASS, `check_jit: PASS`, `check_smoke: PASS`; `runtests.py --test --variants=disable_jit --binary out/jit/bin/ch` -> **`Total: passed 2086, failed 0`** |
| `mise exec -- zig build check-gcstress` | PASS | `in-thread` PASS in `0.98s` (`walks=2`), `concurrent` PASS in `0.76s` (`walks=2`) |
| `mise exec -- zig build -Dsanitize=address check-sanitize` + `-CollectGarbage` | PASS | `27/27` legs clean; all 5 Q2/JSON tests (`iterator_concat.js`, `map_weakmap_getorinsert.js`, `json_rawjson_and_source.js`, `numeric_separator_strtod_and_proxy_realm.js`, `arguments.js`) pass under ASan + `-CollectGarbage` |
| `mise exec -- zig build -Dsanitize=undefined check-sanitize` + `-CollectGarbage` | PASS | `27/27` legs clean; all 5 Q2/JSON tests pass under UBSan (`print_stacktrace=1:halt_on_error=1`) + `-CollectGarbage` |

---

## 5. Test262 Targeted, `--intl402`, & Certified Full Refresh

### 5.1 Targeted `Q2` Matrix (`out/jit/bin/ch`, `286` scenarios across `144` files, `25/25` controls)
Identical **`286 passed / 0 failed`** in both default (`-nonative -Test262 -CollectGarbage -NegativePhaseReport`) and forced-JIT (`-ForceJITLoopBody -maxinterpretcount:1 -Test262 -CollectGarbage -NegativePhaseReport`) modes:
- `test/built-ins/Iterator/concat/**/*.js`: **`64/64`**
- `test/built-ins/Map/prototype/getOrInsert*/**/*.js` + `test/built-ins/WeakMap/prototype/getOrInsert*/**/*.js`: **`142/142`**
- `test/built-ins/JSON/rawJSON/**/*.js` + `JSON/isRawJSON/**/*.js` + `JSON/parse/reviver-*.js`: **`76/76`**
- `test/built-ins/Function/internals/Construct/base-ctor-revoked-proxy-realm.js`: **`2/2`**
- `test/intl402/DurationFormat/prototype/format/duration-out-of-range-2.js`: **`2/2`**

### 5.2 Full `--intl402` Check (`out/test262-intl402-phase4-q2` vs `out/test262-intl402-phase4-i2`)
- Command: `python3 tools/test262/run.py --binary "$PWD/out/jit/bin/ch" --intl402 --output out/test262-intl402-phase4-q2 'test/intl402/**/*.js'`
- **`DurationFormat` (`test/intl402/DurationFormat/`):** `140/220` -> **`142/220`** (`+2`: `duration-out-of-range-2.js` default + strict mode)
- **Full `test/intl402/**/*.js` (`6,764` scenarios, `3,382` files):** `1,884 / 4,880` (`I2`) -> **`1,886 passed / 4,878 failed`** (`+2` `fail->pass`, **`0` `pass->fail` regressions**)

### 5.3 Certified Full Test262 Refresh (`out/test262-refresh-phase4-q2` vs `out/test262-refresh-phase4-q14`)
Command: `python3 tools/test262/run.py --binary "$PWD/out/jit/bin/ch" --output out/test262-refresh-phase4-q2` (frozen `edition-policy.json`, Test262 corpus `2e0a5676`).

| Metric | `Q1.4` (`phase4-q14`) | `Q2` (`phase4-q2`) | Delta |
|---|---:|---:|---:|
| **Certified ES2025 edition (`80,544` scenarios)** | `79,938 / 606` (`99.25%`) | **`79,948 / 596` (`99.26%`)** | **`+10` net** (`-10` failures) |
| **Broad (`93,141` scenarios / `48,598` files)** | `81,126 / 12,015` (`87.10%`) | **`81,332 / 11,809` (`87.32%`)** | **`+206` net** (`-206` failures) |
| Bucket: `post-ES2025` | `11,409` | **`11,213`** | **`-196`** |
| Bucket: `engine-defect-on-existing` | `565` | **`559`** | **`-6`** |
| Bucket: `required-feature-missing` | `25` | **`21`** | **`-4`** |
| Bucket: `host` | `16` | `16` | `0` |
| Negative phase (`passed` / `suspects` / `downgraded`) | `8,741` / `0` / `1` | `8,741` (`8,650` parse, `33` resolution, `58` runtime) / `0` / `1` | `0` |

- **Feature tallies in `out/test262-refresh-phase4-q2/summary.json`:** `iterator-sequencing` **`64/0`**, `json-parse-with-source` **`42/0`**, `upsert` **`142/0`**, `symbols-as-weakmap-keys` **`55/0`**, `numeric-separator-literal` **`318/0`**.
- **Exact join (`93,141` common scenarios):**
  - **`fail -> pass`: `207` scenarios:** `+96` `upsert` (`Map`/`WeakMap` `getOrInsert*`), `+58` `Iterator/concat`, `+42` `JSON/{rawJSON,isRawJSON,parse/reviver-*}` (`json-parse-with-source`), `+4` `numeric-separator-literal-dot-{dd-nsl-dd,dds-nsl-dds}-ep.js`, `+2` `Function/internals/Construct/base-ctor-revoked-proxy-realm.js` (closing [open-cards.md](open-cards.md) §12), `+2` `Proxy/set/trap-is-missing-target-is-proxy.js` (from the `28d3b585c` `modernize/baseline` merge), and `+3` `Atomics/waitAsync` parallel-scheduler timing variance.
  - **`pass -> fail`: `1` scenario** — `test/built-ins/Atomics/waitAsync/bigint/no-spurious-wakeup-on-sub.js` (`default`, `lapse >= TIMEOUT` under `--jobs 8`). Re-running `test/built-ins/Atomics/waitAsync/bigint/no-spurious-wakeup-on-sub.js` isolated at `--jobs 1` passed **`2/2`** (`0` failed), confirming **`0` engine regressions**.
