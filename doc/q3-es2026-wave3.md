# Track Q3: ES2026 Wave 3 Built-ins (`+578` Broad Test262, `0` Engine Regressions)

> **Status:** **COMPLETED (`modernize/phase4`)** on macOS ARM64 (`af68716c8` code commit + docs receipt commit; Linux x64 revalidation queued in Track `G0-phase4`).
> **Scope invariants:** `modernize/baseline` remains untouched at `1bf42d1f63e3f09b13ac782504b8cb59479890db`. Bytecode-header purity is preserved (`lib/Runtime/Base/JnDirectFields.h`, `lib/Runtime/InternalPropertyList.h`, `lib/Runtime/Library/InJavascript/Intl.js`, and `lib/Runtime/Library/InJavascript/JsBuiltIn*.bc.*.h` are completely unchanged vs. `modernize/baseline`, and `verify-bytecode` exits `0` in both `test` and `jit -Dwasm=true` profiles).

---

## 1. Integrated Commits & Slices (`modernize/phase4`)

Built directly on Wave 0 (`87e7629ce`, layered over `Q1.5` `267f9321c`), all four file-disjoint Wave 1 slices cherry-picked cleanly onto `modernize/phase4`, followed by Wave 2 test/gate wiring (`af68716c8`):

| Slice | Wave 1 SHA | `modernize/phase4` SHA | Files Changed | Key Impact |
|---|---|---|---|---|
| **Wave 0. Built-in Registration & Disjoint Scaffold** | `87e7629ce` (`q3-scaffold`) | `87e7629ce` | `JavascriptBuiltInFunctionList.h`, `JavascriptLibrary.cpp`, `JavascriptIterator.{h,cpp}`, `JavascriptIteratorZip.{h,cpp}`, `JavascriptIteratorChunking.{h,cpp}`, `JavascriptPromise.{h,cpp}`, `build.zig`, `CMakeLists.txt`, `.vcxproj{,.filters}`, `tools/build/{tables,tables_linux,linux}.zig`, `tools/build/{check_metadata,check_metadata_linux,test_build_tools}.py`, `tools/linux-gate-tests.zig` | Appended 8 `.length === 1` `ErrorOnNew` built-ins after `JavascriptIterator::Concat` (before `#ifdef INTL_ICU`); wired `Iterator` ctor (`5->7`), `Iterator.prototype` (`17->21` under `iteratorHelpersEnabled`), `Promise` ctor (`9->14`/`13`), and profiler registration; added `IteratorHelperKind::{Zip,ZipKeyed,Chunks,Windows}`, `zipState`/`chunkState`, and +2 build TUs (`538` Mac archive members) |
| **1. `Iterator.prototype.{includes,join}`** | `f44189769` (`q3-iter-includes-join`) | `3420a008b` | `lib/Runtime/Library/JavascriptIterator.cpp`, `test/es7/q3_iterator_includes_join.js` | **`+112`** scenarios (`iterator-includes` **`88/0`**, `Iterator.prototype.join` **`36/0`**; `SameValueZero`, strict `skippedElements` validation + `CloseRecordSwallow` before `GetIteratorDirect`, immediate `CloseRecordNormal` on match, `CompoundString` accumulator) |
| **2. `Iterator.prototype.{chunks,windows}`** | `5853b7914` (`q3-iter-chunking`) | `068711379` | `lib/Runtime/Library/JavascriptIteratorChunking.{h,cpp}`, `test/es7/q3_iterator_chunking.js` | **`+138`** scenarios (`iterator-chunking` **`156/0`**; strict receiver/integer/`[1, 2^32 - 1]`/`undersized` validation with `CloseIteratorSwallow` before `next` lookup, lazy geometric ring buffer `JavascriptIteratorChunkState`, distinct `CreateArray` outputs) |
| **3. `Promise.{allKeyed,allSettledKeyed}`** | `62b9184a2` (`q3-promise-keyed`) | `72cb8c000` | `lib/Runtime/Library/JavascriptPromise.{h,cpp}`, `test/es7/q3_promise_keyed.js` | **`+174`** scenarios (`await-dictionary` **`178/0`**; `Promise` **`1,458/0`**; `GetPromiseResolve` ordering, `OwnPropertyKeys` + enumerable `GetOwnPropertyDescriptorHelper` + `GetProperty`, `OrdinaryObjectCreate(null)` result + own `__proto__` preservation, `ENABLE_TTD` visitor) |
| **4. `Iterator.{zip,zipKeyed}`** | `b0071c5d4` (`q3-iter-zip`) | `f676f838b` | `lib/Runtime/Library/JavascriptIteratorZip.{h,cpp}`, `test/es7/q3_iterator_zip.js` | **`+154`** scenarios (`joint-iteration` **`164/0`**; `zip` `74/0` + `zipKeyed` `90/0`; `GetOptionsObject`, `mode`/`padding` evaluation order, `GetIteratorFlattenableRejectPrimitives`, `shortest`/`longest`/`strict`, reverse-order `CloseZipEntriesNormal` preserving first abrupt completion) |
| **Wave 2. Test Wiring & Gate Pin Bump** | -- | `af68716c8` | `test/es7/rlexe.xml`, `tools/build/gate_counts.json` | Wired `q3_iterator_includes_join.js` (`-CollectGarbage -args summary -endargs`) and `q3_{iterator_chunking,promise_keyed,iterator_zip}.js` (`-CollectGarbage`) into `test/es7/rlexe.xml`; bumped `tools/build/gate_counts.json` (`2094/2146 -> 2098/2150`) |

---

## 2. Per-Proposal Implementation Highlights

### 2.1 Wave 0 Scaffold (`87e7629ce`)
- **Slot & profiler wiring (`lib/Runtime/Library/JavascriptBuiltInFunctionList.h`, `lib/Runtime/Library/JavascriptLibrary.cpp`):** Appended `JavascriptIterator::{Zip, ZipKeyed, Chunks, Windows, Includes, Join}` and `JavascriptPromise::{AllKeyed, AllSettledKeyed}` after `JavascriptIterator::Concat` before `#ifdef INTL_ICU` so all prior `BuiltInEnum` values and precompiled `JsBuiltIn*.bc.*.h` bytecode headers stay stable. Registered `Iterator.{zip,zipKeyed}` (`length = 1`, `ErrorOnNew`, expanding `InitializeIteratorConstructor` `5 -> 7`), `Iterator.prototype.{chunks,windows,includes,join}` (`length = 1`, `ErrorOnNew`, expanding `InitializeIteratorPrototype` `17 -> 21` under `IsESIteratorHelpersEnabled()`), and `Promise.{allKeyed,allSettledKeyed}` (`length = 1`, `ErrorOnNew`, expanding `InitializePromiseConstructor` `9 -> (IsESPromiseAnyEnabled() ? 14 : 13)`), along with matching entries in `ProfilerRegisterIterator` and `ProfilerRegisterPromise`.
- **Helper lifecycle & build tables (`lib/Runtime/Library/JavascriptIterator.{h,cpp}` + build metadata):** Extended `IteratorHelperKind` with `Zip`, `ZipKeyed`, `Chunks`, and `Windows`; added `Field(JavascriptIteratorZipState*) zipState`, `Field(JavascriptIteratorChunkState*) chunkState`, static factories `CreateZipHelper` / `CreateChunkHelper`, and `Advance` / `EntryNext` / `EntryReturn` / `CloseAllSwallow` dispatch (setting `completed = true` before suspended-start `ReturnZip` and clearing `zipState`/`chunkState` on close or abrupt exit). Wired the 2 new translation units (`JavascriptIteratorZip.cpp`, `JavascriptIteratorChunking.cpp`) into `tools/build/tables.zig`, `tools/build/tables_linux.zig` (both lists), `CMakeLists.txt`, and `Chakra.Runtime.Library.vcxproj{,.filters}`, bumping archive/TU pins in `build.zig`, `tools/build/linux.zig`, `check_metadata{,_linux}.py`, `tools/linux-gate-tests.zig`, and `test_build_tools.py`.

### 2.2 `Iterator.prototype.includes` & `Iterator.prototype.join` (`3420a008b`)
- **`Iterator.prototype.includes(searchElement [, skippedElements])` (`lib/Runtime/Library/JavascriptIterator.cpp`):**
  - `ValidateIteratorThis` validates object receivers before `ValidateSkippedElementsOrClose`, which accepts `undefined` (`0.0`) or checks `JavascriptOperators::IsAnyNumberValue` before `ToNumber` (rejecting strings/objects/BigInt/Symbols/booleans without `valueOf`/`toString`, `NaN`, and non-integers with `TypeError`, and `< 0` or `> 2^53 - 1` with `RangeError`) while calling `CloseRecordSwallow` on `thisObject` before `GetIteratorDirect` (so `.next` is never read on invalid `skippedElements`).
  - Steps via `StepValue`, skips `skippedElements` elements (`+Infinity` skips all until exhaustion; `MAX_SAFE_INTEGER` tracked as exact `double` up to `2^53 - 1`), compares with `JavascriptConversion::SameValueZero`, calls `CloseRecordNormal(record, scriptContext)` immediately upon match, and returns `false` on natural exhaustion without accessing `.return`.
- **`Iterator.prototype.join([separator])` (`lib/Runtime/Library/JavascriptIterator.cpp`):**
  - Evaluates `separator` via `ToString` (default `library->GetCommaDisplayString()` on `undefined`) *before* `GetIteratorDirect`, invoking `CloseRecordSwallow(placeholder, scriptContext)` if `ToString(separator)` throws.
  - Converts each yielded value via `EmptyString` (`undefined`/`null`) or `ToString(value)`, closing the iterator via `CloseRecordSwallow(record, scriptContext)` if element conversion throws.
  - Avoids allocating `CompoundString` for 0-element (`EmptyString`) and 1-element (`firstString`) streams; for `>= 2` elements, lazily allocates `CompoundString::NewWithPointerCapacity(8, library)`, flattens any `CompoundString` `separator` once via `separator->GetString()`, and avoids returning zero-length builders by returning `EmptyString` when `resultBuilder->GetLength() == 0`.

### 2.3 `Iterator.prototype.chunks` & `Iterator.prototype.windows` (`068711379`)
- **Validation & helper creation (`lib/Runtime/Library/JavascriptIteratorChunking.cpp`):**
  - Validates object `this`, then validates `size` (`IsAnyNumberValue`, finite integral `TypeError`, `[1, 2^32 - 1]` `RangeError`) and, for `windows`, `undersized` (`undefined` / `"only-full"` -> `false`, `"allow-partial"` -> `true`, anything else `TypeError`). On any validation failure, calls `CloseIteratorSwallow(thisObject, scriptContext)` before touching `.next`.
  - Reads `next` once (`GetPropertyNoCache` + `CrossSite::MarshalVar`), constructs `JavascriptIteratorChunkState`, and returns `CreateChunkHelper(...)` inheriting `%IteratorHelperPrototype%`.
- **Lazy circular buffer & yield lifecycle (`lib/Runtime/Library/JavascriptIteratorChunking.{h,cpp}`):**
  - `JavascriptIteratorChunkState` starts with `buffer = nullptr` and `capacity = 0` (even when `targetSize == 2^32 - 1`), growing geometrically from `min(targetSize, 4)` up to `min(seen, targetSize)` via `EnsureCapacityForPush` and copying into a fresh `JavascriptArray` via `Materialize` on every yield so callers never alias the internal ring buffer.
  - `chunks` calls `ConsumeElements()` after each full chunk yield to drop retained `Var` references and yields any partial tail (`count > 0`) on exhaustion; `windows` uses `PushSliding` once filled and yields a partial window on exhaustion only when `allowPartial && count > 0 && count < targetSize`. Exhaustion, `EntryReturn` (`CloseChunkingNormal`), and abrupt unwind (`CloseChunkingSwallow`) call `Clear()` and set `chunkState = nullptr`.

### 2.4 `Promise.allKeyed` & `Promise.allSettledKeyed` (`72cb8c000`)
- **`AllKeyedHelper` & combinator ordering (`lib/Runtime/Library/JavascriptPromise.cpp`):**
  - Validates object `this`, creates `JavascriptPromiseCapability` via `NewPromiseCapability(constructor, scriptContext)`, resolves `GetPromiseResolve(constructorObject, scriptContext)` before checking `IsObject(promises)` (`IfAbruptRejectPromise` semantics), enumerates `GetOwnPropertyKeys`, and filters enumerable own properties via `GetOwnPropertyDescriptorHelper` + `GetProperty` in `OwnPropertyKeys` order.
  - Invokes `resolveFunc` and `nextPromise.then(onFulfilled, onRejected)` wrapped in `BEGIN_SAFE_REENTRANT_CALL`. Starts `state->remainingElements` at `1` and decrements after the loop so synchronous callbacks cannot resolve `promiseCapability` mid-iteration, and leaves `remainingElements > 0` on abrupt completion so `TryRejectWithExceptionObject` rejects once without late fulfillment races.
- **Element callbacks & null-prototype output (`lib/Runtime/Library/JavascriptPromise.{h,cpp}`):**
  - `CreateAllKeyedElementFunction` allocates `JavascriptPromiseAllKeyedElementFunction` (`length = 1`, `name = ""`, `FunctionInfo::ErrorOnNew`) sharing one `JavascriptPromiseResolveOrRejectFunctionAlreadyResolvedWrapper` per key for `allSettledKeyed`, building `{ status: "fulfilled" | "rejected", value | reason }` via `DefineDataProperty` (`DefineOwnPropertyHelper`).
  - `CreateKeyedPromiseCombinatorResultObject` returns `OrdinaryObjectCreate(null)` (`library->CreateObject(library->GetNull())`) with `DefineDataProperty` so keys (including `"__proto__"` and Symbols) are defined cleanly without prototype setters or `__proto__` mutation. `ENABLE_TTD` tracing is overridden in `JavascriptPromiseAllKeyedElementFunction::MarkVisitKindSpecificPtrs`.

### 2.5 `Iterator.zip` & `Iterator.zipKeyed` (`f676f838b`)
- **Options, iterables acquisition & padding (`lib/Runtime/Library/JavascriptIteratorZip.cpp`):**
  - Validates object `iterables` (`JSERR_NeedObject`) before `ParseZipOptions` (`GetOptionsObject`, reading `"mode"` [`"shortest"` | `"longest"` | `"strict"`] then `"padding"` only when `mode == ZipMode::Longest`).
  - `EntryZip` obtains `inputIter` via `GetIteratorSync`, reads elements via `StepRecordValue` + `GetIteratorFlattenableRejectPrimitives` (closing already-opened inner entries in reverse order plus `inputIter` last on error), and for `"longest"` pulls padding from `paddingOption` (via `GetIteratorSync` + `StepRecordValue` up to `iterCount`, then `CloseRecordNormal` if padding iterator did not exhaust).
  - `EntryZipKeyed` reads `GetOwnPropertyKeys`, checks `GetOwnPropertyDescriptorHelper` for `IsEnumerable()`, reads `GetPropertyNoCache` and skips properties whose *value* is `undefined` (closing opened entries on error), then reads `padding[key]` for `"longest"` using the collected `keyPropertyRecord`s.
- **Step modes & `IteratorCloseAll` (`lib/Runtime/Library/JavascriptIteratorZip.cpp`):**
  - `AdvanceZip` steps active entries in order into `resultsScratch`, marshaling cross-realm values via `CrossSite::MarshalVar`. `shortest` calls `CloseZipEntriesNormal` on the first `done: true`; `strict` verifies all remaining entries are `done: true` (via `StepEntryIsOpen` when entry `0` completes, or throws `TypeError` immediately when `i != 0`) and unwinds via `CloseZipSwallow`; `longest` substitutes `entry->padding` until `state->openCount == 0`.
  - `CloseZipEntriesNormal` (used by `shortest` completion, `CloseZipNormal`, and suspended-start `ReturnZip`) closes remaining open entries in reverse order, capturing the first thrown `JavascriptExceptionObject*`, continuing remaining closes in swallow mode (`CloseEntrySwallow`), and rethrowing the first error via `DoThrowCheckClone`. `BuildZipResult` creates fresh `JavascriptArray` or null-prototype `DynamicObject` outputs and nulls `resultsScratch[i]` immediately after each yield.

---

## 3. Memory Management & GC Ownership Records (`doc/memory-management.md` Section 3)

### 3.1 `JavascriptIterator.cpp`: `EntryIncludes` & `EntryJoin` (`CompoundString`)
```text
Resource / creation call / source location:
  1. Stack `IterRecord` (`placeholder` / `record`: `RecyclableObject* iterator`, `Var nextMethod`,
     `bool done`), `searchElement`, `skippedElements`, `value`, `separator`, and `firstString` in
     `JavascriptIterator::{EntryIncludes, EntryJoin}` (`lib/Runtime/Library/JavascriptIterator.cpp:759-930`).
  2. Lazy `CompoundString* resultBuilder = CompoundString::NewWithPointerCapacity(8, library)`
     (`JavascriptIterator.cpp:893`) allocated only when `EntryJoin` sees `>= 2` elements.
Domain and allocator instance (including backing allocator lifetime):
  - Owning `ScriptContext` `Recycler` GC heap (`CompoundString` uses `Field(BlockInfo)`,
    `Field(Block*)`, `RecyclerNewArray`/`RecyclerNewPlus` + `ArrayWriteBarrier`); stack locals are
    conservatively scanned by Recycler.
Physical allocation base, extent, alignment, release callback or pool kind:
  - Standard Recycler objects/strings; zero unmanaged native allocations or finalizers.
Logical length / borrowed views (kept separate from the allocation descriptor):
  - `CompoundString` internal block chain tracks character length independently of capacity; if
    `separator` is itself a `CompoundString`, `separator->GetString()` flattens it once before
    appending (`separator` remains immutable).
Owner; transfer/publication point; remaining aliases:
  - `IterRecord` and `firstString` remain stack-local; `resultBuilder` escapes only as the returned
    immutable `JavascriptString*` (or `EmptyString` when `resultBuilder->GetLength() == 0`).
Borrow invalidation: GC / detach / growth / mutation / reset / close / disposal:
  - Reentrant `next()`, `return()`, getter, or `ToString()` calls and `-CollectGarbage` can run on
    every step; stack conservative scanning + `CompoundString` `Field(...)` barriers preserve live
    string blocks and the underlying iterator across growth and GC.
Potential user-code or reentrancy points:
  - `ValidateSkippedElementsOrClose` (does not coerce non-numbers), `ToString(separatorArg)`,
    `GetIteratorDirect` (`GetProperty("next")`), `StepValue`, `ToString(value)`, and
    `CloseRecordNormal` / `CloseRecordSwallow` (`.return` lookup and call).
Thread/context allowed for use, root changes and destruction:
  - Bound `ScriptContext` script thread only (`CrossSite::MarshalVar` on iterator, `nextMethod`, and
    step values).
Release on success, partial failure, exception, OOM, cancel and shutdown:
  - Validation errors close `placeholder` via `CloseRecordSwallow` before `GetIteratorDirect`.
  - `EntryJoin` catches `ToString(separatorArg)` throws (closing `placeholder`) and loop
    `ToString(value)` / `CompoundString` throws (closing `record`) before `DoThrowCheckClone`.
  - `EntryIncludes` calls `CloseRecordNormal` only on `SameValueZero` match, never on exhaustion or
    `StepValue` exceptions; Recycler reclaims transient strings/builders.
Memory accounting, capacity/retention bound and exhaustion behavior:
  - Bounded by yielded element string sizes (`CompoundString` OOM raises engine OOM exception).
Tests / commands / measured results / remaining uncertainty:
  - `test/es7/q3_iterator_includes_join.js` (`-CollectGarbage` + 200-element `CompoundString` GC
    stress + 500x JIT warmup), Test262 `iterator-includes` `88/0` & `Iterator.prototype.join` `36/0`,
    `622/0` JIT parity, ASan & UBSan (`27/27`, `29/29`, `-ForceJITLoopBody -maxinterpretcount:1 -CollectGarbage`) PASS.
```

### 3.2 `JavascriptIteratorChunkState` (`lib/Runtime/Library/JavascriptIteratorChunking.{h,cpp}`)
```text
Resource / creation call / source location:
  - `JavascriptIteratorChunkState` (`RecyclerNew` in `CreateChunkingHelper`,
    `lib/Runtime/Library/JavascriptIteratorChunking.cpp:121-133`) owning circular
    `Field(Field(Var)*) buffer` (`RecyclerNewArrayZ(scriptContext->GetRecycler(), Field(Var), newCap)`,
    `:175`), attached via `Field(JavascriptIteratorChunkState*) chunkState` on
    `JavascriptIteratorHelper` (`lib/Runtime/Library/JavascriptIterator.h:132`).
Domain and allocator instance (including backing allocator lifetime):
  - Owning `ScriptContext` `Recycler` GC heap with `Field(...)` write barriers; zero C/native heap
    allocations or finalizers.
Physical allocation base, extent, alignment, release callback or pool kind:
  - Recycler `JavascriptIteratorChunkState` + `Field(Var)` array of `capacity` slots (`uint32`),
    initially `nullptr` (`capacity = 0`) even when `targetSize == UINT32_MAX` (`2^32 - 1`), growing
    geometrically from `min(targetSize, 4)` up to `min(seenElements, targetSize)`.
Logical length / borrowed views (kept separate from the allocation descriptor):
  - `head` and `count` (`0 <= count <= capacity <= targetSize <= UINT32_MAX`) index live entries in
    the circular buffer; `Materialize` copies into a fresh `JavascriptArray` (`CreateArray(count)`)
    so JS callers never alias `buffer`.
Owner; transfer/publication point; remaining aliases:
  - Created in `CreateChunkingHelper` and transferred exclusively to `JavascriptIteratorHelper::chunkState`
    via `CreateChunkHelper`. No external aliases exist.
Borrow invalidation: GC / detach / growth / mutation / reset / close / disposal:
  - Reentrant `next()` / `return()` and `-CollectGarbage` can run across yields/steps;
    `JavascriptIteratorHelper::executing` rejects reentrant `next()`/`return()` with `TypeError`, while
    `Field(Var)` slots keep buffered items traced across GC and buffer growth (`EnsureCapacityForPush`
    nulls old buffer slots after copying).
Potential user-code or reentrancy points:
  - Validation close (`CloseIteratorSwallow`), `GetPropertyNoCache("next")`, `StepUnderlying`, and
    `EntryReturn` (`CloseInnerNormal` / `CloseAllSwallow`).
Thread/context allowed for use, root changes and destruction:
  - Single bound `ScriptContext` thread (`CrossSite::MarshalVar` on `thisObject`, `nextMethod`, and
    pushed/materialized `Var`s).
Release on success, partial failure, exception, OOM, cancel and shutdown:
  - Pre-helper validation failures call `CloseIteratorSwallow(thisObject)` before allocating `chunkState`.
  - Full `chunks` yields call `ConsumeElements()` to null `Field(Var)` slots between chunks.
  - Exhaustion, partial final yield, `EntryReturn` (`CloseChunkingNormal`), and abrupt unwind
    (`CloseChunkingSwallow`) call `Clear()` (nulling all slots and dropping `buffer`) and clear
    `helper->chunkState = nullptr`.
Memory accounting, capacity/retention bound and exhaustion behavior:
  - Bounded by `min(observed stream length, targetSize)` (lazy allocation avoids `2^32 - 1` OOM on
    empty/short streams).
Tests / commands / measured results / remaining uncertainty:
  - `test/es7/q3_iterator_chunking.js` (`-CollectGarbage`, growth across 17/25 items, `2^32 - 1` short
    streams), Test262 `iterator-chunking` `156/0`, `622/0` JIT parity, ASan & UBSan (`27/27`, `29/29`,
    `-ForceJITLoopBody -maxinterpretcount:1 -CollectGarbage`) PASS. Remaining uncertainty: materializing
    a full `2^32 - 1` chunk is unexercised due to physical RAM limits.
```

### 3.3 `JavascriptPromiseAllKeyedState` & `JavascriptPromiseAllKeyedElementFunction` (`lib/Runtime/Library/JavascriptPromise.{h,cpp}`)
```text
Resource / creation call / source location:
  - `JavascriptPromiseAllKeyedState` (`RecyclerNew`, `JavascriptPromise.cpp:2737`),
    `JsUtil::List<const PropertyRecord*, Recycler>* keys` & `JsUtil::List<Var, Recycler>* values`
    (`List::New`, `:2735-2736`), `JavascriptPromiseResolveOrRejectFunctionAlreadyResolvedWrapper`
    (`RecyclerNewStructZ`, `:2773`), and `JavascriptPromiseAllKeyedElementFunction`
    (`RecyclerNewEnumClass(..., JavascriptLibrary::EnumFunctionClass, ...)` via
    `CreateFunctionWithLengthAndNameType` + static `FunctionInfo::ErrorOnNew`, `:2853-2878`),
    attached in `AllKeyedHelper` (`JavascriptPromise.cpp:2713-2826`).
Domain and allocator instance (including backing allocator lifetime):
  - Owning `ScriptContext` `Recycler` GC heap; `Field(...)` write barriers on all retained pointers;
    zero unmanaged native heap allocations, custom finalizers, or persistent external roots.
Physical allocation base, extent, alignment, release callback or pool kind:
  - Recycler GC objects/lists; `JavascriptPromiseAllKeyedElementFunction` is a `RuntimeFunction`
    subclass whose `ENABLE_TTD` visitor `MarkVisitKindSpecificPtrs` delegates to `RuntimeFunction` and
    marks `state->capability` and `state->values`.
Logical length / borrowed views (kept separate from the allocation descriptor):
  - `keys` and `values` grow synchronously in lockstep (`index = keys->Count()`) during property
    enumeration in `AllKeyedHelper`; `keys` pins `const PropertyRecord*` pointers so
    `PropertyRecord::Finalize` cannot reclaim dynamic/Symbol `PropertyId`s across GC before settlement.
Owner; transfer/publication point; remaining aliases:
  - Stack-pinned during `AllKeyedHelper`, then published solely via `onFulfilled` / `onRejected`
    handlers passed to `nextPromise.then(...)`. For `allSettledKeyed`, the fulfillment and rejection
    functions for key `index` share one `alreadyCalledWrapper` so whichever settles first wins.
Borrow invalidation: GC / detach / growth / mutation / reset / close / disposal:
  - Reentrant getters, Proxy traps, `C.resolve`, and synchronous `.then` callbacks can mutate input
    objects or trigger GC mid-loop; `remainingElements` starts at `1` and decrements after the loop
    so synchronous callbacks cannot prematurely resolve `promiseCapability`.
Potential user-code or reentrancy points:
  - `NewPromiseCapability`, `GetPromiseResolve`, `GetOwnPropertyKeys`, `GetOwnPropertyDescriptorHelper`,
    `GetProperty`, `CALL_FUNCTION(resolveFunc)`, `GetProperty("then")`, and `CALL_FUNCTION(thenFunc)`
    (wrapped in `BEGIN_SAFE_REENTRANT_CALL`).
Thread/context allowed for use, root changes and destruction:
  - Bound `ScriptContext` thread only (`DEFINE_MARSHAL_OBJECT_TO_SCRIPT_CONTEXT` and
    `CrossSite::MarshalVar` on settled values).
Release on success, partial failure, exception, OOM, cancel and shutdown:
  - Abrupt completion inside `AllKeyedHelper` leaves `remainingElements > 0` (preventing straggler
    callbacks from later resolving) and rejects via `TryRejectWithExceptionObject`. On final
    settlement (`--state->remainingElements == 0`), `CreateKeyedPromiseCombinatorResultObject` constructs
    an `OrdinaryObjectCreate(null)` result via `DefineOwnPropertyHelper` and resolves `capability`;
    Recycler sweeps unreachable subgraphs.
Memory accounting, capacity/retention bound and exhaustion behavior:
  - Bounded by enumerable own property count under Recycler budgets.
Tests / commands / measured results / remaining uncertainty:
  - `test/es7/q3_promise_keyed.js` (1200x JIT warmup, GC across unsettled keyed Symbols/Promises,
    cross-realm `WScript.LoadScript`), Test262 `await-dictionary` `178/0` (`Promise` `1,458/0`),
    `622/0` JIT parity, ASan & UBSan (`27/27`, `29/29`, `-ForceJITLoopBody -maxinterpretcount:1 -CollectGarbage`)
    and `debug-static` (`ENABLE_TTD` compile + execution) PASS. Remaining uncertainty: live TTD
    snapshot/replay was not executed (`ENABLE_TTD` verified by `debug-static` build and test run).
```

### 3.4 `JavascriptIteratorZipState` & `ZipIterEntry` (`lib/Runtime/Library/JavascriptIteratorZip.{h,cpp}`)
```text
Resource / creation call / source location:
  - `JavascriptIteratorZipState` (`RecyclerNew`, `JavascriptIteratorZip.cpp:469,604`), `ZipIterList`
    (`JsUtil::List<ZipIterEntry*, Recycler>`, `JavascriptIteratorZip.h:55`), `ZipIterEntry` elements
    (`RecyclerNew`, `JavascriptIteratorZip.cpp:506,634`), and `resultsScratch`
    (`RecyclerNewArrayZ(scriptContext->GetRecycler(), Field(Var), count)` when `count > 0`, `:409`),
    transferred to `JavascriptIteratorHelper::zipState` (`JavascriptIterator.h:131`).
Domain and allocator instance (including backing allocator lifetime):
  - Owning `ScriptContext` `Recycler` GC heap with `Field(...)` barriers on every retained pointer
    (`entries`, `resultsScratch`, `iterator`, `nextMethod`, `padding`, `keyPropertyRecord`, `keyVar`);
    zero unmanaged native allocations or finalizers.
Physical allocation base, extent, alignment, release callback or pool kind:
  - Recycler GC state, `ZipIterList`, `ZipIterEntry` structs, and fixed `iterCount`-slot `Field(Var)`
    scratch array (`nullptr` when `iterCount == 0`).
Logical length / borrowed views (kept separate from the allocation descriptor):
  - `iterCount` records total entries; `openCount` tracks open entries (`0 <= openCount <= iterCount`).
    `BuildZipResult` copies `resultsScratch` into a fresh `JavascriptArray` (`CreateArray`) or
    null-prototype `DynamicObject` (`CreateObject(GetNull())` + `DefineOwnPropertyHelper`) and
    immediately nulls `resultsScratch[i]` after each yield so prior yields are not retained.
Owner; transfer/publication point; remaining aliases:
  - Stack-pinned (`state`) throughout `EntryZip`/`EntryZipKeyed` setup, then transferred exclusively
    to `JavascriptIteratorHelper::zipState` via `CreateZipHelper`. Exhausted/closed entries immediately
    null `iterator` and `nextMethod` while keeping `padding`/`keyPropertyRecord` if needed for `"longest"`.
Borrow invalidation: GC / detach / growth / mutation / reset / close / disposal:
  - Reentrant `next()` / `return()` / options / padding getters and `-CollectGarbage` can run during
    setup or `AdvanceZip`; `JavascriptIteratorHelper::executing` throws `TypeError` on reentrant
    `next()`/`return()` during `AdvanceZip`/`CloseZipNormal`, whereas suspended-start `ReturnZip`
    marks `completed = true` beforehand so reentrancy returns `{value: undefined, done: true}`.
Potential user-code or reentrancy points:
  - `ParseZipOptions` (`mode`/`padding`), `GetIteratorSync`, `GetIteratorFlattenableRejectPrimitives`,
    `StepRecordValue`, `OwnPropertyKeys`/`GetOwnPropertyDescriptorHelper`/`GetPropertyNoCache`
    (`EntryZipKeyed`), `StepEntryValue`/`StepEntryIsOpen`, and `CloseRecordNormal`/`CloseRecordSwallow`.
Thread/context allowed for use, root changes and destruction:
  - Bound `ScriptContext` script thread (`CrossSite::MarshalVar` on inputs, iterators, `nextMethod`,
    keys, padding, and stepped values).
Release on success, partial failure, exception, OOM, cancel and shutdown:
  - Setup exceptions close already-opened entries in reverse order (`CloseZipEntriesSwallow`) plus
    `inputIter` last if `GetIteratorFlattenableRejectPrimitives` throws in `EntryZip`, rethrowing the
    original error.
  - Runtime `AdvanceZip` throws clear `resultsScratch` and unwind to `EntryNext`'s `CloseZipSwallow` +
    `zipState = nullptr`. `shortest` exhaustion, `CloseZipNormal`, and suspended-start `ReturnZip`
    run `CloseZipEntriesNormal` (reverse-order close preserving the first abrupt completion and
    swallowing subsequent close errors), and `EntryReturn` clears `zipState = nullptr`.
Memory accounting, capacity/retention bound and exhaustion behavior:
  - Bounded by number of zipped input iterables; no persistent caches retained post-completion.
Tests / commands / measured results / remaining uncertainty:
  - `test/es7/q3_iterator_zip.js` (dynamic Symbol keys, `gcNow()` across setup/yield, reentrancy,
    cross-realm, 60x JIT warmup), Test262 `joint-iteration` `164/0`, `622/0` JIT parity, ASan & UBSan
    (`27/27`, `29/29`, `-ForceJITLoopBody -maxinterpretcount:1 -CollectGarbage`) and `debug-static`
    (`ENABLE_TTD` compile + execution) PASS. Remaining uncertainty: live TTD snapshot/replay was not
    executed (`ENABLE_TTD` verified by `debug-static` build and test run).
```

---

## 4. Certified Test262 Refresh & JIT Parity (`out/test262-refresh-phase4-q3`)

Run via `python3 tools/test262/refresh_baseline.py --binary "$PWD/out/jit/bin/ch" --output out/test262-refresh-phase4-q3 --previous out/test262-refresh-phase4-q15` on clean non-sanitized commit `af68716c893afc1f1d2b8ff2de579d1eab6c1bb1` (`engine_tree_dirty: false`, corpus `2e0a56762801e275a9fdf96dc49d90ba0cddcf63`, `-nonative -Test262 -CollectGarbage -NegativePhaseReport`, `--jobs 8`, `--timeout 10000`, `25/25` controls, `902` host-filter skips):

| Metric | `Q1.5` (`267f9321c`) | `Q3` (`af68716c8`) | Delta |
|---|---:|---:|---:|
| **Certified ES2025 edition (`80,544` scenarios, `42,203` files)** | `80,189 / 355` (`99.56%`) | **`80,195 / 349` (`99.57%`)** | **`+6`** (`Atomics/waitAsync` `--jobs 8` variance; `0` engine regressions) |
| **Broad (`93,141` scenarios, `48,598` files)** | `82,282 / 10,859` (`88.34%`) | **`82,866 / 10,275` (`88.97%`)** | **`+584` net** (`+578` `Q3` + `+6` `Atomics/waitAsync`) |
| Bucket: `post-ES2025` | `10,504` | **`9,926`** | **`-578`** |
| Bucket: `engine-defect-on-existing` | `338` | `332` | `-6` (`Atomics/waitAsync` `--jobs 8` variance) |
| Bucket: `required-feature-missing` | `1` | `1` | `0` |
| Bucket: `host` | `16` | `16` | `0` |
| Negative phase (`passed` / `suspects` / `downgraded`) | `8,746` / `0` / `1` | **`8,746`** (`8,651` parse, `33` resolution, `62` runtime) / **`0`** / `1` | `0` (**`0` shape suspects**) |

### 4.1 Per-Scenario Delta & `Atomics/waitAsync` Verification (`93,141` Joined Scenarios)
- **`fail -> pass`: `587` scenarios:**
  - **`578` deterministic `Q3` proposal passes** (`post-ES2025` `10,504 -> 9,926`):
    - `test/built-ins/Iterator/zip/` (`+70` -> **`74/0`**) and `test/built-ins/Iterator/zipKeyed/` (`+84` -> **`90/0`**) (`joint-iteration` **`164/0`**)
    - `test/built-ins/Iterator/prototype/chunks/` (`+68` -> **`78/0`**) and `test/built-ins/Iterator/prototype/windows/` (`+70` -> **`78/0`**) (`iterator-chunking` **`156/0`**)
    - `test/built-ins/Iterator/prototype/includes/` (`+80` -> **`88/0`**) (`iterator-includes` **`88/0`**)
    - `test/built-ins/Iterator/prototype/join/` (`+32` -> **`36/0`**) (`Iterator.prototype.join` **`36/0`**)
    - `test/built-ins/Promise/allKeyed/` (`+88` -> **`90/0`**) and `test/built-ins/Promise/allSettledKeyed/` (`+86` -> **`88/0`**) (`await-dictionary` **`178/0`**; full `test/built-ins/Promise/**/*.js` **`1,458/0`**)
  - **`9` pre-existing `Atomics/waitAsync/*no-spurious-wakeup*`** scheduler timing flips.
- **`pass -> fail`: `3` scenarios** (`3` files, **100% pre-existing `Atomics/waitAsync/*no-spurious-wakeup*` `(lapse >= TIMEOUT)` host-scheduler timer flakes under `--jobs 8`**; zero `Atomics`/`SharedArrayBuffer` files changed in `Q3`):
  - `test/built-ins/Atomics/waitAsync/bigint/no-spurious-wakeup-on-and.js` (`strict mode`): verified **`2/0`** twice standalone at `--jobs 1` (`1/1` & `2/0` on initial pair, then `2/0` & `2/0` cleanly).
  - `test/built-ins/Atomics/waitAsync/bigint/no-spurious-wakeup-on-compareExchange.js` (`default`): verified **`2/0`** & **`2/0`** at `--jobs 1`.
  - `test/built-ins/Atomics/waitAsync/no-spurious-wakeup-no-operation.js` (`default`): verified **`2/0`** & **`2/0`** at `--jobs 1`.
  - Confirmed **`0` engine `pass->fail` regressions**.

### 4.2 Interpreter vs. Forced-JIT Parity (`622 / 622` `Q3` Scenarios)
Ran all `311` `Q3` Test262 files (`622` scenarios, `25/25` controls) across `test/built-ins/Iterator/zip/**/*.js`, `test/built-ins/Iterator/zipKeyed/**/*.js`, `test/built-ins/Iterator/prototype/{chunks,windows,includes,join}/**/*.js`, and `test/built-ins/Promise/{allKeyed,allSettledKeyed}/**/*.js` using `tools/test262/run.py --binary "$PWD/out/jit/bin/ch"`:
- **`-nonative -Test262 -CollectGarbage -NegativePhaseReport`:** **`622 passed / 0 failed`** (`622` scenarios, `311` files)
- **`-ForceJITLoopBody -maxinterpretcount:1 -Test262 -CollectGarbage -NegativePhaseReport`:** **`622 passed / 0 failed`** (`622` scenarios, `311` files)
- **Interpreter vs. Forced-JIT diff join:** **`0` diffs** across all `622` scenarios.

---

## 5. Full macOS ARM64 Certification Matrix (`af68716c8` Code Commit)

| Gate | Command / Scope | Result |
|---|---|---|
| Bytecode-header purity | `mise exec -- zig build verify-bytecode` & `-Dprofile=jit -Dwasm=true verify-bytecode` | **PASS** (exit `0` on `af68716c8` and final `HEAD`; `JnDirectFields.h`, `InternalPropertyList.h`, `Intl.js`, `JsBuiltIn*.bc.*.h` unchanged vs. `modernize/baseline`) |
| StyleChecks | `STYLECHECK_BASE={origin/master,modernize/baseline}` `tools/StyleChecks/check_{eol,tabs,ascii,copyright}.sh` | **PASS** (`8/8` `NO PROBLEMS DETECTED`) |
| Build & gate unit tests | `zig test tools/linux-gate-tests.zig`; `check-selftest`; `check-build-tools`; `check_regression.py --self-test` | **PASS** (`9/9` Linux gate tests; `check-selftest` `pal-config 95/95` & negative controls PASS; **`98` tests OK**) |
| `test` profile | `mise exec -- zig build check` | **PASS** (**`2098/0`** [`es7: 129/0`]; `check_smoke` PASS; `check_metadata` `18,606` `T` symbols `8/8` JsRT exports; `test-shared-basic` PASS) |
| `debug-static` profile | `mise exec -- zig build -Dprofile=debug-static check` | **PASS** (**`2150/0`** [`es7: 129/0`]; **`538`** archive members [`1,881,731,376` bytes]; `62,624` `T` symbols `8/8` JsRT; `8/8` static native suites PASS; no dylib) |
| `release-shared` profile | `mise exec -- zig build -Dprofile=release-shared check` | **PASS** (`17,202` `T` symbols `8/8` JsRT exports; `check_smoke` PASS; `test-shared-basic` PASS) |
| `jit -Dwasm=true` gates | `mise exec -- zig build -Dprofile=jit -Dwasm=true check-wasm check-fulljit check` | **PASS** (`check-wasm` P1 **`382/382`** / runtests `448/5`; `check-fulljit` **`633/0`** [`Basics 48/0`, `Array 123/0`, `Function 68/0`, `Regex 40/0`, `es6 224/0`, `es7 130/0`]; `check_jit` PASS; `check_smoke` PASS; JIT `verify-bytecode` exit `0`) |
| `dynapogo` JIT suites | `(cd test && python3 runtests.py -t --variants=dynapogo -b ../out/jit/bin/ch Optimizer Bugs Array es6 es7 Function Object Strings Operators LetConst Closures)` (+ standalone `es5`) | **PASS** (**`1053/0`**: `Array 123/0`, `Bugs 101/0`, `Closures 23/0`, `Function 68/0`, `LetConst 50/0`, `Object 60/0`, `Operators 29/0`, `Optimizer 202/0`, `Strings 43/0`, `es6 224/0`, `es7 130/0`; plus `es5` `56/0`) |
| `GCStress` & `Node-API` | `mise exec -- zig build check-gcstress && mise exec -- zig build check-nodeapi` | **PASS** (`GCStress`: `in-thread` `0.98s` / `concurrent` `0.76s`, `walks=2` each; `Node-API`: **`161`** allowlisted exports = `155` napi + `6` host) |
| ASan (`profile=test` & `jit -Dwasm=true`) | `-Dsanitize=address check-sanitize` + `ASAN_OPTIONS=detect_leaks=0 ./out/sanitize-address/bin/ch -ForceJITLoopBody -maxinterpretcount:1 -CollectGarbage` (`4` `q3_*.js`) | **PASS** (`profile=test`: **`27/27`**; `jit -Dwasm=true`: **`29/29`**; **`4/4`** `q3_*.js` JIT/GC runs clean) |
| UBSan (`profile=test` & `jit -Dwasm=true`, `halt_on_error=1`) | `UBSAN_OPTIONS=print_stacktrace=1:halt_on_error=1` `-Dsanitize=undefined check-sanitize` + `./out/sanitize-undefined/bin/ch -ForceJITLoopBody -maxinterpretcount:1 -CollectGarbage` (`4` `q3_*.js`) | **PASS** (`profile=test`: **`27/27`**; `jit -Dwasm=true`: **`29/29`**; **`4/4`** `q3_*.js` JIT/GC runs clean) |
| Clean rebuild & package | Clean non-sanitized `zig build -Dprofile=jit -Dwasm=true` + `zig build` + `zig build check-package` | **PASS** (code commit `af68716c8`: `chakracore-af68716c8-darwin-arm64.tar.gz`, **`5,505,065`** bytes, `sha256: be9af70dbf2fadbff0f5f43f430bb601b3c10fdcb8d45ca8199015d987bfbb68`; final docs commit re-verified with `check-package` `VERIFY PASS`, `ALL PACKAGE TESTS PASSED SUCCESSFULLY!`) |
