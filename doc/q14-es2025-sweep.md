# Track Q1.4: ES2025 Conformance Sweep II (`Date`, `Function.prototype.toString` + Parser UB, `JSON.parse`, `AsyncFromSyncIterator`)

Status: Completed on `modernize/phase4` (macOS ARM64; Linux x64 revalidation queued in Track `G0`).
`modernize/baseline` remains untouched at `35f53eae0a3599ea7e9c0baa6cf9331f7df3c351`.
`lib/Runtime/Library/InJavascript/Intl.js` and `lib/Runtime/Library/InJavascript/JsBuiltIn*.bc.*.h` are unchanged relative to `35f53eae0a3599ea7e9c0baa6cf9331f7df3c351` (confirmed by `git diff` and `zig build verify-bytecode`).

---

## 1. Integrated Commits & Slices

Wave 1 slices cherry-picked cleanly onto `modernize/phase4` (parent `d583957d89987688f54db8041154df905f9f131e`) with zero merge conflicts and zero overlapping files:

| Slice | Wave 1 SHA | `modernize/phase4` SHA | Files Changed | Targeted ES2025 Test262 |
|---|---|---|---|---|
| **1. `Date.prototype.set*` & `TimeClip`** | `a5543ede3` (`q14-date`) | `fcdc0b74f` | `DateImplementation.{h,cpp}`, `JavascriptDate.cpp`, `test/Date/date_set_es2025.js` | `+52` scenarios (`Date/prototype`: `954/954` ES2025 edition, `954/16` broad w/ `Temporal`) |
| **2. `Function.prototype.toString`, `Parse.cpp` & `ptree.cpp:390` UB** | `ef8420375` (`q14-fn-parser`) | `2b8c304c8` | `Parse.cpp`, `ptree.cpp`, `JavascriptFunction.{h,cpp}`, `test/Function/toString_es2025_and_arrow_ub.js`, 10 legacy tests/baselines | `+44` scenarios (`Function/prototype/toString`: `160/160`, 100%) |
| **3. `JSON.parse` `InternalizeJSONProperty` (`Walk`)** | `26212762f` (`q14-json`) | `3c4dc0521` | `JSONParser.{h,cpp}`, `test/JSON/json_parse_reviver_es2025.js` | `+28` scenarios (`JSON/parse`: `144/144` ES2025 edition, `144/10` broad w/ `json-parse-with-source`) |
| **4. `%AsyncFromSyncIteratorPrototype%` `closeOnRejection` & `.return`/`.throw`** | `16c0fd57c` (`q14-async-iter`) | `7a001431c` | `JavascriptAsyncFromSyncIterator.{h,cpp}`, `test/es7/async_from_sync_iterator_es2025.js` | `+32` scenarios (`AsyncFromSyncIteratorPrototype`: `76/76` + `for-await-of/async-from-sync-*`: `2/2` = `78/78`, 100%) |

### Slice 1 — `Date.prototype.set*` & `TimeClip` (`fcdc0b74f`)
- **`-0.0` -> `+0.0` normalization:** Normalized `-0.0` to `+0.0` in `TimeClip` (`lib/Runtime/Library/JavascriptDate.cpp`), `GetTvUtc` (`lib/Runtime/Library/DateImplementation.h`), and `DoubleToTvUtc` / `DateFncUTC` (`lib/Runtime/Library/DateImplementation.cpp`) so inputs like `-1.23e-15` or `-0` clip to `+0.0` per ES2025 `TimeClip`. Updated `JavascriptDate::EntrySetTime` to return `date->m_date.GetMilliSeconds()`.
- **`SetDateData` side-effect & coercion order (`DateImplementation.cpp:1661`):**
  - Captures `origTvUtc = m_tvUtc` before coercing all supplied arguments up to `cvarMax` left-to-right without short-circuiting on `NaN`/`Infinity`.
  - When `origTvUtc` is `NaN`: only `DateData::FullYear` and `DateData::Year` (`setFullYear`/`setUTCFullYear`/`setYear`) fall back to the `+0` base; all other setters return `NaN` without clobbering any `m_tvUtc` mutation performed by `valueOf()`.
  - When `origTvUtc` is finite and `valueOf()` mutated `m_tvUtc != origTvUtc`, recomputes `origYmd` / `tv` from `origTvUtc` (`GetTvLcl` + `GetYmdFromTv`) rather than reading mutated fields.
- **Focused regression:** [test/Date/date_set_es2025.js](../test/Date/date_set_es2025.js) (registered in [test/Date/rlexe.xml](../test/Date/rlexe.xml)).

### Slice 2 — `Function.prototype.toString`, Parser Extents & `ptree.cpp:390` Arrow UB (`2b8c304c8`)
- **`ptree.cpp:390` UBSan fix & `Parse.cpp` `PidRefStack*` guards:**
  - `ParseNodeName::SetSymRef(PidRefStack * ref)` ([lib/Parser/ptree.cpp:387-391](../lib/Parser/ptree.cpp#L387-L391)) now checks `ref != nullptr ? ref->GetSymRef() : nullptr` before calling `PidRefStack::GetSymRef`, eliminating the null-`this` UBSan diagnostic triggered by unparenthesized single-argument arrow parameters (`p => ...`, tracked in [open-cards.md](open-cards.md) §16).
  - `Parser::AddDeclForPid` (`lib/Parser/Parse.cpp:1455`) and `Parser::FinishDeferredFunction` (`lib/Parser/Parse.cpp:15213,15254`) guard `GetSymRef()` against `nullptr` (`Error(ERRnoMemory)`), and `Parser::PushPidRef` (`lib/Parser/Parse.cpp:12935`) routes `ParallelParsePhase` through `this->FindOrAddPidRef(...)`.
- **`Function.prototype.toString` (`JavascriptFunction.{h,cpp}` & `Parse.cpp`):**
  - `JavascriptFunction::EntryToString` ([lib/Runtime/Library/JavascriptFunction.cpp:1891-1925](../lib/Runtime/Library/JavascriptFunction.cpp#L1891-L1925)) removes `JavascriptProxy` target unwrapping: `JavascriptFunction` subclasses return `CrossSite::MarshalVar(scriptContext, pFunc->EnsureSourceString())`, while any non-`JavascriptFunction` callable (including active or revoked callable `Proxy` objects) returns `scriptContext->GetLibrary()->GetFunctionDisplayString()` (`"function() {\n    [native code]\n}"`), throwing `TypeError` for non-callables per ES2025 20.2.3.5.
  - `JavascriptFunction::NewInstanceHelper` (`lib/Runtime/Library/JavascriptFunction.cpp:170-189`) appends `\n` after `" {"` (`"anonymous(...) {\n...\n}"`) and increments `numberLinesPrependedToAnonymousFunction` from `1` to `2` (`lib/Runtime/Library/JavascriptFunction.h:96`), matching ES2025 `CreateDynamicFunction` source text and keeping error stack line/column mapping consistent.
  - `Parser::ParseMemberList` (`lib/Parser/Parse.cpp:6118`) preserves the `async` token start for `async *` object/class generator methods (`!isAsyncMethod`), and `Parser::ParseClassDecl` (`lib/Parser/Parse.cpp:10795-10815,11313,11363`) preserves `async`/`*` starts on private methods and sets `cbStringMin = iecpMin` / `cbStringLim = pnodeFnc->cbLim` for class public/private `get`/`set` accessors while keeping `ichMin`/`cbMin` intact for deferred reparse.
- **10 pre-ES2019 legacy tests/baselines updated in `2b8c304c8`:**
  - `test/Basics/ScriptFunctionToStrings.js` (expects `"get getter(){}"` / `"set setter(v){}"`)
  - `test/es6/ProxyInProxy.baseline` (callable `Proxy` `toString()` returns NativeFunction string instead of target source)
  - `test/es6/generators-functionality.js`, `test/es7/asyncawait-apis.js`, `test/es7/async-generator-apis.js` (`NewInstanceHelper` `{\n` body newline)
  - `test/Lib/noargs_2.baseline`, `test/Debugger/JsDiagGetScripts.js.dbg.baseline`, `test/DebuggerCommon/parentedDynamicCode2.js.dbg.baseline`, `test/StackTrace/FunctionName.js.baseline`, `test/StackTrace/dynamic.js.baseline` (`NewInstanceHelper` `{\n` body newline / lineCount `3 -> 4`, sourceLength `+1`, column offset `-3`).
- **Focused regression:** [test/Function/toString_es2025_and_arrow_ub.js](../test/Function/toString_es2025_and_arrow_ub.js) (`-CollectGarbage`, registered in [test/Function/rlexe.xml](../test/Function/rlexe.xml)).
- **Pre-existing UBSan note:** Wave 1 (`q14-fn-parser`, `#302`) confirmed `toString_es2025_and_arrow_ub.js`, `toStringAll.js`, and `ProxyInProxy.js` pass under `UBSAN_OPTIONS=print_stacktrace=1:halt_on_error=1`, while noting a pre-existing `-ParserStateCache` `ImmutableList::Prepend` null-`this` call at `lib/Runtime/ByteCode/ByteCodeSerializer.cpp:762` (`PrependConstantInt32`) in `test/Function` (`bug_os17698041.js`).

### Slice 3 — `JSON.parse` `InternalizeJSONProperty` (`Walk`) (`3c4dc0521`)
- **`JSONParser::Walk` / `WalkProperty` ([lib/Runtime/Library/JSONParser.{h,cpp}](../lib/Runtime/Library/JSONParser.cpp)):**
  - Replaces `DirectGetItem` with `JavascriptOperators::GetItem` / `GetProperty` so prototype getters and `Proxy` `get` traps run and propagate abrupt completions.
  - Uses `IsArrayOrThrowIfRevoked` (ES2025 7.2.2 `IsArray` step 3a, throwing `JSERR_ErrorOnRevokedProxy` in caller realm) and `OP_GetLength` + `ToLength` over full `uint64` `[0, 2^53 - 1]` range with `UInt64ToPropertyString`.
  - Uses `GetOwnEnumerablePropertyNames` snapshot for non-array objects (invoking `Proxy` `ownKeys` + `getOwnPropertyDescriptor` traps).
  - Uses non-strict `CreateDataProperty` (`DefineOwnPropertyHelper` with `throwOnError = false`) and `DeleteItem`/`DeleteProperty` (`PropertyOperation_None`) so `false` returns on non-configurable/non-extensible targets or Proxy traps are ignored while abrupt throws propagate; re-checks non-ES5 array fast path and converts `CopyOnAccessNativeIntArray` before mutation.
- **Focused regression:** [test/JSON/json_parse_reviver_es2025.js](../test/JSON/json_parse_reviver_es2025.js) (`-CollectGarbage`, registered in [test/JSON/rlexe.xml](../test/JSON/rlexe.xml)).

### Slice 4 — `%AsyncFromSyncIteratorPrototype%` `closeOnRejection` & `.return`/`.throw` (`7a001431c`)
- **`.return` and `.throw` (`lib/Runtime/Library/JavascriptAsyncFromSyncIterator.cpp`):**
  - `GetMethod` treats both `undefined` and `null` as missing (`IsUndefinedOrNull`) and uses spec API names (`AsyncFromSyncIterator.prototype.return` / `.throw`).
  - Missing/`undefined`/`null` `.throw` invokes `IteratorCloseNormalCompletion` (ES2025 7.4.7 `IteratorClose(syncIteratorRecord, NormalCompletion(empty))`), propagating inner exceptions / non-callable `return` / non-object `return()` results, then rejects with a fresh `TypeError` (`JSERR_YieldStarThrowMissing`).
- **`AsyncFromSyncIteratorContinuation(result, syncIteratorRecord, closeOnRejection, scriptContext)`:**
  - Passes `closeOnRejection = true` for `Next` and `Throw`, `false` for `Return`.
  - Wraps `InternalPromiseResolve(value, scriptContext)` in `try/catch`; when `!done && closeOnRejection`, calls `JavascriptOperators::IteratorClose(syncIteratorRecord, scriptContext)` and rejects with the preserved original `reason`.
  - When `!done && closeOnRejection`, creates `AsyncFromSyncCloseOnRejectFunction` via `CreateCloseOnRejectFunction` and attaches it via `NewPromiseCapability(%Promise%)` + `PerformPromiseThen`; `EntryAsyncFromSyncCloseOnRejectFunction` calls `JavascriptOperators::IteratorClose` (which clears inner exceptions via `err.GetAndClear()`) and rethrows the cross-site-marshaled original rejection error.
- **Focused regression:** [test/es7/async_from_sync_iterator_es2025.js](../test/es7/async_from_sync_iterator_es2025.js) (`-CollectGarbage`, registered in [test/es7/rlexe.xml](../test/es7/rlexe.xml)).

---

## 2. Ownership Records (`doc/memory-management.md` Section 3)

### 2.1 `AsyncFromSyncCloseOnRejectFunction` (`lib/Runtime/Library/JavascriptAsyncFromSyncIterator.{h,cpp}`)
```text
Resource / creation call / source location:
  `AsyncFromSyncCloseOnRejectFunction` (subclass of `RuntimeFunction`), allocated via
  `RecyclerNewEnumClass(..., JavascriptLibrary::EnumFunctionClass, ...)` in
  `JavascriptAsyncFromSyncIterator::CreateCloseOnRejectFunction` when
  `AsyncFromSyncIteratorContinuation` executes with `!done && closeOnRejection`.
Domain and allocator instance (including backing allocator lifetime):
  `ScriptContext`'s `Recycler` GC heap (lifetime bound to the owning `ScriptContext`).
Physical allocation base, extent, alignment, release callback or pool kind:
  Single Recycler-allocated `EnumFunctionClass` object whose `DynamicType` comes from
  `JavascriptLibrary::CreateFunctionWithLengthAndNameType(&s_closeOnRejectInfo)`; holds
  no native heap memory, no `FinalizableObject` hook, and no native pool entry.
Logical length / borrowed views:
  `Field(RecyclableObject*) syncIterator` with GC write-barrier protection, plus
  configurable `length` (`1`) and `name` (`""`) JS own properties.
Owner; transfer/publication point; remaining aliases:
  Created inside `AsyncFromSyncIteratorContinuation` and published solely as the
  `onRejected` handler passed to `JavascriptPromise::PerformPromiseThen(valueWrapper, ...)`;
  no off-stack native pointers or explicit roots are kept.
Borrow invalidation: GC / detach / growth / mutation / reset / close / disposal:
  Recycler tracing of `Field(RecyclableObject*) syncIterator` keeps `syncIterator` alive
  across GC until the promise reaction settles or becomes unreachable; `ScriptContext`
  disposal destroys all remaining GC state.
Potential user-code or reentrancy points:
  `EntryAsyncFromSyncCloseOnRejectFunction` reads `syncIterator`, then calls
  `JavascriptOperators::IteratorClose(syncIterator, scriptContext)` (`GetProperty("return")`
  and `ExecuteImplicitCall`), and finally throws the marshaled original error (`args[1]`).
Thread/context allowed for use, root changes and destruction:
  Single bound `ScriptContext` execution thread; supports cross-realm invocations via
  `DEFINE_MARSHAL_OBJECT_TO_SCRIPT_CONTEXT(AsyncFromSyncCloseOnRejectFunction)` and
  `CrossSite::MarshalVar(scriptContext, args[1])`.
Release on success, partial failure, exception, OOM, cancel and shutdown:
  Reclaimed automatically by Recycler sweep once reaction drops; inner exceptions thrown
  from `syncIterator.return()` are caught and cleared by `IteratorClose` (`err.GetAndClear()`)
  so the original rejection error always propagates; OOM throws before publication.
Memory accounting, capacity/retention bound and exhaustion behavior:
  At most one callback object per `!done && closeOnRejection` continuation, bounded by
  Recycler GC heap policy (zero persistent cache/queue).
Tests / commands / measured results / remaining uncertainty:
  Verified by `test/es7/async_from_sync_iterator_es2025.js` (GC stress + cross-realm
  `WScript.LoadScript`), `test/es7` regression (`117/0` in `check-fulljit`), and Test262
  `AsyncFromSyncIteratorPrototype/**` + `for-await-of/async-from-sync-*` (`78/78` in
  both interpreter and JIT `-ForceJITLoopBody -maxinterpretcount:1 -CollectGarbage`).
```

### 2.2 `JSONParser::Walk` / `WalkProperty` (`lib/Runtime/Library/JSONParser.{h,cpp}`)
```text
Resource / creation call / source location:
  Recycler-managed JS values (`holder`, `val`, `newElement`, `propName`, `reviver`),
  enumerable key snapshot array (`keys = JavascriptOperators::GetOwnEnumerablePropertyNames`),
  and interned `PropertyRecord*` instances (`scriptContext->GetOrAddPropertyRecord`).
Domain and allocator instance (including backing allocator lifetime):
  ChakraCore `Recycler` GC heap (conservative stack scan); no untraced native C++
  heap or arena containers retain `Var` or object pointers across `WalkProperty` or
  `reviver` calls.
Physical allocation base, extent, alignment, release callback or pool kind:
  Recycler owns all JS objects, strings, `keys` snapshot arrays, and finalizable
  `PropertyRecord` entries; C++ stack frames hold no owning native buffers.
Logical length / borrowed views:
  `uint64 len` is captured once via `ToLength(OP_GetLength(valObj))` per array walk;
  `keys` snapshot array is internal and never exposed to JS user code.
Owner; transfer/publication point; remaining aliases:
  Each recursive `WalkProperty` stack frame retains stack-scanned locals (`holder`,
  `val`, `keys`, `keyStr`, `keyRecord`, `newElement`) across child walks and `reviver`
  invocations. Keeping `keyStr` and `keyRecord` live on the C++ stack prevents
  `PropertyRecord` finalization if user code triggers GC.
Borrow invalidation: GC / detach / growth / mutation / reset / close / disposal:
  Reentrant `reviver`/getter/Proxy traps can trigger GC, resize/freeze/seal objects,
  convert `JavascriptArray` to `ES5Array` or `CopyOnAccessNativeIntArray`, or revoke
  Proxies. `TryVarToNonES5Array` is re-checked after each child `WalkProperty`, and
  `CheckAndConvertCopyOnAccessNativeIntArray` runs before `DeleteItem` and inside
  `CreateDataProperty`.
Potential user-code or reentrancy points:
  `GetItem`/`GetProperty`, `IsArrayOrThrowIfRevoked`, `OP_GetLength` + `ToLength`,
  `GetOwnEnumerablePropertyNames` (`ownKeys`/`getOwnPropertyDescriptor`), recursive
  `WalkProperty`, `DeleteItem`/`DeleteProperty`, `CreateDataProperty`
  (`DefineOwnPropertyHelper`), and `JavascriptFunction::CallFunction(reviver)`.
Thread/context allowed for use, root changes and destruction:
  Single bound script thread with active `ScriptContext` (`BEGIN_SAFE_REENTRANT_CALL`
  around `CallFunction`).
Release on success, partial failure, exception, OOM, cancel and shutdown:
  C++ stack frames unwind normally on return or JS exception; Recycler collects
  unreachable intermediate objects, `keys` arrays, and `PropertyRecord`s.
Memory accounting, capacity/retention bound and exhaustion behavior:
  `PROBE_STACK(scriptContext, Js::Constants::MinStackDefault)` bounds recursion depth;
  array indices span `[0, 2^53 - 1]` with indices `>= JavascriptArray::InvalidIndex`
  using `GetProperty`/`DeleteItem(uint64)`/`CreateDataProperty`.
Tests / commands / measured results / remaining uncertainty:
  Verified by `test/JSON/json_parse_reviver_es2025.js` (`-CollectGarbage` proxy/GC
  stress), `JSON` regression (`14/0` in `--test`, `17/0` in `--debug`), and Test262
  `JSON/parse/**/*.js` (`144/144` ES2025 edition in both interpreter and JIT).
```

---

## 3. Multi-Profile Verification (macOS ARM64, `modernize/phase4`)

All 4 new focused tests are wired into `test/{Date,Function,JSON,es7}/rlexe.xml` and pinned in [tools/build/gate_counts.json](../tools/build/gate_counts.json) (`2076/2128 -> 2080/2132`):

| Command / Profile | Result | Details |
|---|---|---|
| `mise exec -- zig build check-selftest` | PASS | `pal-config` `95/95`, `u8a_codec.zig`, `owned-bytes.zig`, all build-tool negative controls |
| `mise exec -- zig build check-build-tools` | PASS | `Ran 97 tests in 16.745s`, `OK` |
| `mise exec -- zig test tools/linux-gate-tests.zig` | PASS | `All 9 tests passed.` |
| `mise exec -- zig build verify-bytecode` | PASS | `Bytecode is up to date`, `source headers unchanged; regen exit 0` |
| `mise exec -- zig build check` (`test` profile) | PASS | `Total: passed 2080, failed 0` (`Function 72/0`, `es7 116/0`, etc.), `test-shared-basic : PASS`, `check_metadata: PASS`, `check_smoke: PASS` |
| `mise exec -- zig build -Dprofile=debug-static check` | PASS | `Total: passed 2132, failed 0` (`Date 11/0`, `Function 79/0`, `JSON 17/0`, `es7 116/0`), `533` archive members (`61515` T symbols), `8/8` static native PASS, `check_metadata: PASS`, `check_smoke: PASS` |
| `mise exec -- zig build -Dprofile=release-shared check` | PASS | `test-shared-basic : PASS`, `check_metadata: PASS`, `check_smoke: PASS`, `check_gcstress` PASS |
| `mise exec -- zig build -Dprofile=jit -Dwasm=true -p out/jit check-wasm` | PASS | `P1 pass 382/382` (`runtests Total passed 448, failed 5`) |
| `mise exec -- zig build -Dprofile=jit -Dwasm=true -p out/jit check-fulljit` | PASS | `Total: passed 620, failed 0` (`Basics 48/0`, `Array 123/0`, `Function 68/0`, `Regex 40/0`, `es6 224/0`, `es7 117/0`; up from `618/0`) |
| `mise exec -- zig build -Dprofile=jit -Dwasm=true -p out/jit check` | PASS | `check_smoke: PASS`, `check_jit: PASS`, JIT `verify-bytecode` PASS, `check_wasm_p1: PASS: P1 pass 382/382` |

---

## 4. Test262 Targeted JIT & Certified Full Refresh Receipt

### 4.1 Targeted `Q1.4` JIT Run (`out/test262-q14-jit`)
Run on `./out/jit/bin/ch` with `--host-flags "-ForceJITLoopBody -maxinterpretcount:1 -Test262 -CollectGarbage -NegativePhaseReport"` across `test/built-ins/Date/prototype/**/*.js`, `test/built-ins/Function/prototype/toString/**/*.js`, `test/built-ins/JSON/parse/**/*.js`, `test/built-ins/AsyncFromSyncIteratorPrototype/**/*.js`, and `test/language/statements/for-await-of/async-from-sync-*.js`:
- **Controls:** `25/25` passed.
- **Broad:** `1,336 passed / 26 failed` across `1,362` scenarios (`681` files, `98.09%`).
- **ES2025 edition:** **`1,336 passed / 0 failed` (`100.00%`)** across `1,336` edition scenarios (`668` files); all `26` broad failures are `post-ES2025-proposal` (`16` `Date.prototype.toTemporalInstant` + `10` `JSON/parse` `json-parse-with-source`).
- **JIT vs. Interpreter join (`out/test262-q14-jit` vs. `out/test262-refresh-phase4-q14`):** **`0` differences across all `1,362` scenarios**.

### 4.2 Full Certified Test262 Refresh (`out/test262-refresh-phase4-q14` vs. `out/test262-refresh-phase4-q13`)
Command: `python3 tools/test262/run.py --binary ./zig-out/bin/ch --output out/test262-refresh-phase4-q14` (frozen `edition-policy.json`, Test262 corpus `2e0a5676`).

| Metric | `Q1.3` (`phase4-q13`) | `Q1.4` (`phase4-q14`) | Delta |
|---|---:|---:|---:|
| **Certified ES2025 edition** | `79,784 / 760` (`99.06%`) | **`79,938 / 606` (`99.25%`)** | **`+154` net** (`-154` failures) |
| **Broad (`93,141` scenarios / `48,598` files)** | `80,972 / 12,169` (`86.93%`) | **`81,126 / 12,015` (`87.10%`)** | **`+154` net** |
| Bucket: `post-ES2025` | `11,409` | `11,409` | `0` |
| Bucket: `engine-defect-on-existing` | `719` | **`565`** | **`-154`** |
| Bucket: `required-feature-missing` | `25` | `25` | `0` |
| Bucket: `host` | `16` | `16` | `0` |
| Negative phase (`passed` / `suspects` / `downgraded`) | `8,741` / `0` / `1` | `8,741` (`8,650` parse, `33` resolution, `58` runtime) / `0` / `1` | `0` |

### 4.3 Exact Per-Scenario Join (`93,141` Joined Scenarios)
- **`fail -> pass`: `159` scenarios**
  - `test/built-ins/Date/prototype`: `52` scenarios (`26` files: `set{Date,Hours,Milliseconds,Minutes,Month,Seconds,UTCDate,UTCHours,UTCMilliseconds,UTCMinutes,UTCMonth,UTCSeconds}/date-value-read-before-tonumber-when-date-is-{valid,invalid}.js` [`48`], `setTime/new-value-time-clip.js` [`2`], `valueOf/S9.4_A3_T1.js` [`2`])
  - `test/built-ins/Function/prototype/toString`: `44` scenarios (`22` files: `Function.js`, `GeneratorFunction.js`, `AsyncFunction.js`, `AsyncGenerator.js`, `async-generator-method-object.js`, `{getter,setter}-class-{expression,statement}{,-static}.js` [`8`], `proxy-{arrow-function,async-function,async-generator-function,async-generator-method-definition,async-method-definition,class,function-expression,generator-function,method-definition}.js` [`9`])
  - `test/built-ins/JSON/parse`: `28` scenarios (`14` files: `revived-proxy-revoked.js`, `revived-proxy.js`, `reviver-array-{define-prop-err,delete-err,length-coerce-err,length-get-err,non-configurable-prop-create,non-configurable-prop-delete}.js`, `reviver-get-name-err.js`, `reviver-object-{define-prop-err,delete-err,get-prop-from-prototype,non-configurable-prop-create,own-keys-err}.js`)
  - `test/built-ins/AsyncFromSyncIteratorPrototype`: `28` scenarios (`14` files: `next/{for-await-iterator-next-rejected-promise-close,for-await-next-rejected-promise-close,iterator-result-poisoned-wrapper,next-result-poisoned-wrapper,yield-iterator-next-rejected-promise-close,yield-next-rejected-promise-close}.js` [`12`], `return/return-null.js` [`2`], `throw/{iterator-result-rejected-promise-close,throw-result-poisoned-wrapper,throw-undefined-get-return-undefined,throw-undefined-poisoned-return,throw-undefined-return-not-object,throw-undefined-return-object,throw-undefined}.js` [`14`])
  - `test/language/statements/for-await-of`: `4` scenarios (`2` files: `async-from-sync-iterator-continuation-abrupt-completion-get-constructor.js`, `ticks-with-sync-iter-resolved-promise-and-constructor-lookup.js`)
  - `test/built-ins/Atomics/waitAsync` parallel-scheduler timing variance (`3` scenarios): `bigint/no-spurious-wakeup-no-operation.js` (`default`), `bigint/no-spurious-wakeup-on-add.js` (`default`), `no-spurious-wakeup-on-and.js` (`default`)
- **`pass -> fail`: `5` scenarios** (all 5 are pre-existing `Atomics/waitAsync/*no-spurious-wakeup*` `(lapse >= TIMEOUT)` timer-jitter flakes under `--jobs 8`; zero `Atomics`/`ThreadContext`/`WScript` files were touched in `Q1.4`):
  - `test/built-ins/Atomics/waitAsync/bigint/no-spurious-wakeup-on-add.js` (`strict mode`)
  - `test/built-ins/Atomics/waitAsync/bigint/no-spurious-wakeup-on-compareExchange.js` (`default`, `strict mode`)
  - `test/built-ins/Atomics/waitAsync/no-spurious-wakeup-on-store.js` (`strict mode`)
  - `test/built-ins/Atomics/waitAsync/no-spurious-wakeup-on-xor.js` (`default`)
- **`--jobs 1` flake verification:** Re-running all 4 `pass -> fail` files at `--jobs 1` passed `7/8` (`on-store` `2/2`, `on-xor` `2/2`, `bigint/on-add` `2/2`, `bigint/on-compareExchange` `strict mode` pass / `default` `lapse >= TIMEOUT`). Re-running `bigint/no-spurious-wakeup-on-compareExchange.js` standalone at `--jobs 1` twice passed `default` in the second run and `strict mode` in the first run, confirming **`0` engine pass->fail regressions** (matching [open-cards.md](open-cards.md) §1 `c2-jit-test262`).

### 4.4 Per-Area Final Totals (`out/test262-refresh-phase4-q14`)
- `Function/prototype/toString`: **`160/0`** (`160` scenarios, `80` files)
- `AsyncFromSyncIteratorPrototype`: **`76/0`** + `for-await-of/async-from-sync-*`: **`2/0`** (**`78/0`**; remaining `for-await-of`: `2,409/22`, all `22` unrelated `async-*-dstr-obj-id-init-fn-name-*` & `head-lhs-async.js`)
- `JSON/parse`: **`144/10`** broad (**`144/0`** ES2025 edition; `10` `json-parse-with-source`; total `JSON/**` **`288/44`** broad, **`288/2`** ES2025 edition with only `JSON/stringify/replacer-function-object-deleted-property.js`)
- `Date/prototype`: **`954/16`** broad (**`954/0`** ES2025 edition; `16` `Temporal` `toTemporalInstant`; total `Date/**` + `annexB/Date/**` **`1,216/20`** broad, **`1,216/4`** ES2025 edition with `Date/UTC/fp-evaluation-order.js` `2` + `Date/parse/time-value-maximum-range.js` `2`)
