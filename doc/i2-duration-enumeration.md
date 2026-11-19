# Track I2: `Intl.supportedValuesOf` Enumeration & `Intl.DurationFormat`

Status: Completed on `modernize/phase4` (macOS ARM64; Linux x64 revalidation queued in Track `G0`). Follow-on Track `Q2` (`862882548`) subsequently closed the `StrToDbl` numeric-separator scaling defect (`duration-out-of-range-2.js` `2/2` pass, lifting `DurationFormat` `140 -> 142 / 220` and `--intl402` `1,884 -> 1,886 / 6,764`; see [q2-es2026-wave2.md](q2-es2026-wave2.md)).
`lib/Runtime/Library/InJavascript/Intl.js` and `lib/Runtime/Library/InJavascript/JsBuiltIn*.bc.*.h` remain completely unchanged relative to `modernize/baseline` (`35f53eae0a3599ea7e9c0baa6cf9331f7df3c351`) and `7519836ab3a302289f980e4396c6837ad33914bd` (verified by `git diff` and `mise exec -- zig build verify-bytecode`). Both built-ins are implemented strictly in C++ over ICU4C's C APIs (`U_SHOW_CPLUSPLUS_API 0`).

---

## 1. Architecture and Files

- **Wave 0 scaffold (`693f60994`, shared registration & C-ICU4C wiring):**
  - Header/enum/slot registration: [lib/Runtime/PlatformAgnostic/ChakraICU.h](../lib/Runtime/PlatformAgnostic/ChakraICU.h) (added `unicode/unumberformatter.h`, `ScopedUNumberFormatter`, `ScopedUFormattedNumber`), [lib/Runtime/Library/IntlModernShared.h](../lib/Runtime/Library/IntlModernShared.h) (`IntlModernKind::DurationFormat`), [lib/Runtime/Library/JavascriptBuiltInFunctionList.h](../lib/Runtime/Library/JavascriptBuiltInFunctionList.h), [lib/Runtime/Library/JavascriptBuiltInFunctions.cpp](../lib/Runtime/Library/JavascriptBuiltInFunctions.cpp), [lib/Runtime/Library/JavascriptLibrary.h](../lib/Runtime/Library/JavascriptLibrary.h), [lib/Runtime/Library/JavascriptLibrary.cpp](../lib/Runtime/Library/JavascriptLibrary.cpp) (`intlDurationFormatPrototype`, `initSlotCapacity` `12 -> 16`, `Initialize`), [lib/Runtime/Library/IntlEngineInterfaceExtensionObject.cpp](../lib/Runtime/Library/IntlEngineInterfaceExtensionObject.cpp) (`supportedValuesOf`, `DurationFormat` on `Intl`).
  - Build tables & metadata pins: [build.zig](../build.zig) (`expect_base` `544/546 -> 546/548`), [tools/build/linux.zig](../tools/build/linux.zig), [tools/build/tables.zig](../tools/build/tables.zig), [tools/build/tables_linux.zig](../tools/build/tables_linux.zig), [lib/Runtime/Library/CMakeLists.txt](../lib/Runtime/Library/CMakeLists.txt), [lib/Runtime/Library/Chakra.Runtime.Library.vcxproj](../lib/Runtime/Library/Chakra.Runtime.Library.vcxproj), [lib/Runtime/Library/Chakra.Runtime.Library.vcxproj.filters](../lib/Runtime/Library/Chakra.Runtime.Library.vcxproj.filters), [tools/build/check_metadata.py](../tools/build/check_metadata.py), [tools/build/check_metadata_linux.py](../tools/build/check_metadata_linux.py), [tools/build/test_build_tools.py](../tools/build/test_build_tools.py), [tools/linux-gate-tests.zig](../tools/linux-gate-tests.zig).
- **Track `I2` Enumeration (`Intl.supportedValuesOf`, `f3cbecabe` cherry-picked from `429dee005`):**
  - [lib/Runtime/Library/IntlEnumeration.h](../lib/Runtime/Library/IntlEnumeration.h), [lib/Runtime/Library/IntlEnumeration.cpp](../lib/Runtime/Library/IntlEnumeration.cpp): implements `Intl.supportedValuesOf(key)` across all 6 ECMA-402 keys (`"calendar"`, `"collation"`, `"currency"`, `"numberingSystem"`, `"timeZone"`, `"unit"`), UTF-16 code-unit heap sort & deduplication via `AsciiStringSet`, DisplayNames/ICU resource bundle cross-validation (`uldn_keyValueDisplayName`, `ucurr_openISOCurrencies` + `ures_open`/`ures_getByKey` `Currencies` + `ucurr_getName`, `unumsys_openAvailableNames`/`unumsys_openByName`, `ucal_openTimeZoneIDEnumeration` + UTC alias folding), and own-element creation via `CreateArray` + `DirectSetItemAt` (immune to `Array.prototype` index getters/setters).
  - [test/Intl/IntlSupportedValuesOfTest.js](../test/Intl/IntlSupportedValuesOfTest.js): 10 test blocks covering descriptors, `ToString` coercion & GC reentrancy, invalid key `RangeError`/`Symbol` `TypeError`, freshness/taint immunity, sorting/uniqueness, cross-API parity (`Locale`, `DisplayNames`, `RelativeTimeFormat`, `NumberFormat`, `DateTimeFormat`), and GC stress.
- **Track `I2` `Intl.DurationFormat` (`a83e2727d` cherry-picked from `47df29f0d`):**
  - [lib/Runtime/Library/IntlDurationFormat.h](../lib/Runtime/Library/IntlDurationFormat.h), [lib/Runtime/Library/IntlDurationFormat.cpp](../lib/Runtime/Library/IntlDurationFormat.cpp): implements `Intl.DurationFormat` constructor, `.supportedLocalesOf`, `.prototype.format`, `.prototype.formatToParts`, and `.prototype.resolvedOptions` over C ICU4C (`ulistfmt_openForType`/`ulistfmt_formatStringsToResult`, `unumf_openForSkeletonAndLocale`/`unumf_formatDecimal`, `ures_open` `timeSeparator` fallback walk, `ucfpos_*`/`ufmtval_*`), exact `unsigned __int128` IEEE-754 bit extraction (`ExtractExactInteger128`) and nanosecond validation (`kMaxTimeDurationNs = 2^53 * 10^9`), ISO 8601 duration string parser (`ParseIsoDurationString`), `rounding-mode-down` (`trunc`) subsecond combining, and `IntlDurationFormatState`.
  - [test/Intl/IntlDurationFormatTest.js](../test/Intl/IntlDurationFormatTest.js): 10 test blocks covering descriptors, 24-option read order, `resolvedOptions` key order/conflicts, `fractionalDigits`/`numberingSystem` reconciliation, alphabetical `ToDurationRecord` access, `unsigned __int128` range/precision boundaries, ISO 8601 duration strings, `trunc` & `-0`, `formatToParts` structure, multi-locale/numbering-system outputs, cross-realm `Reflect.construct`, GC reentrancy, and finalization churn.
- **Regression wiring & gate pins:**
  - [test/Intl/rlexe.xml](../test/Intl/rlexe.xml): wired `IntlSupportedValuesOfTest.js` and `IntlDurationFormatTest.js` with `<tags>Intl,exclude_windows</tags>` and `<compile-flags>-args summary -endargs</compile-flags>`.
  - [tools/build/gate_counts.json](../tools/build/gate_counts.json): bumped `test` (`2080 -> 2082`) and `debug` (`2132 -> 2134`).
- **C-only ICU ABI verification:** `nm -u` on `zig-out/bin/libChakraCore.dylib`, `out/release-shared/bin/libChakraCore.dylib`, and `out/jit/bin/libChakraCore.dylib` shows zero `icu::*` or `std::*` ICU symbols; `nm -u` on compiled `IntlEnumeration.cpp.o` and `IntlDurationFormat.cpp.o` objects shows zero `std::*` containers/algorithms and zero `icu::*` symbols (only `__ZSt9terminatev` from the C++ exception ABI).

---

## 2. Lifetime Graph and Write-Barrier Audit

### Lifetime Graph (`IntlDurationFormatState` & `IntlEnumeration`)

```text
new Intl.DurationFormat(locales, options)
  |
  |-- 1. OrdinaryCreateFromConstructor (newTarget.prototype, CrossSite::MarshalVar)
  |-- 2. CanonicalizeLocaleList + GetOptionsObject + 24 option reads in spec order
  |      (all user-visible JS coercion / Proxy traps / GC complete here)
  |-- 3. ResolveLocale + ResolveTimeSeparator (ScopedUResourceBundle) +
  |      ulistfmt_openForType(..., &status) -> opens UListFormatter*
  |-- 4. try { IntlDurationFormatState::New(recycler, listFormatter, ...) }
  |      catch (...) { ulistfmt_close(listFormatter); throw; }
  |-- 5. IntlModern::SetState(durationFormat, state) -> HiddenObject
  |      (if SetState returns false, state remains unpublished for normal GC sweep)
  v
JS instance -> HiddenObject -> IntlDurationFormatState (RecyclerNewWithBarrierFinalized)
  |                                |
  |                                +-- Field(JavascriptString*) locale, numberingSystem,
  |                                |   styleStr, timeSeparator (GC-traced)
  |                                +-- FieldNoBarrier(UListFormatter*) listFormatter
  |                                +-- FieldNoBarrier(DurationFormatConfig) config
  v
.format(duration) / .formatToParts(duration)
  |-- RequireState validates receiver magic & IntlModernKind::DurationFormat
  |-- ToDurationRecord completes JS string parse or alphabetical GetProperty + ToNumber
  |-- FormatDurationImpl re-acquires GetLiveListFormatter(state, scriptContext),
  |   copies timeSeparator into native UCharBuffer before GC allocations,
  |   runs synchronous non-reentrant ICU calls (ScopedUFormattedNumber,
  |   ScopedUNumberFormatter, ScopedUFormattedList, ScopedUConstrainedFieldPosition),
  |   and copies UChar* output into fresh Recycler strings / plain JS part objects
  v
Instance unreachable / ScriptContext shutdown
  |-- Recycler Finalize(isShutdown) -> IntlDurationFormatState::Dispose(isShutdown)
  |-- If listFormatter != nullptr: ulistfmt_close(listFormatter); listFormatter = nullptr;
  |-- InvalidateMagic() (idempotent)

Intl.supportedValuesOf(key)
  |-- JavascriptConversion::ToString(keyArg, scriptContext) before any ICU/native alloc
  |-- Stack-local ScopedICUObject handles + AsciiStringSet (stack + NoCheckHeap fallback)
  |-- Returned fresh JavascriptArray populated via CreateArray + DirectSetItemAt
  |-- All native handles/buffers destroyed on scope exit (zero retained off-stack state)
```

### `Field(...)` Write-Barrier & `Dispose(bool)` Audit

| Type | Kind / Allocation | Traced GC slots (`Field(...)`) | Non-GC / ICU slots (`FieldNoBarrier(...)` / inline) | Cleanup / `Dispose(bool)` |
|---|---|---|---|---|
| `IntlDurationFormatState` (`: IntlModernState`) | `IntlModernKind::DurationFormat`, `RecyclerNewWithBarrierFinalized` | `Field(JavascriptString*) locale`, `Field(JavascriptString*) numberingSystem`, `Field(JavascriptString*) styleStr`, `Field(JavascriptString*) timeSeparator` | Inherited `magic`, `kind`; `FieldNoBarrier(UListFormatter*) listFormatter`; `FieldNoBarrier(DurationFormatConfig) config` (`DurationStyle`, `UnitStyle[10]`, `UnitDisplay[10]`, `int8 fractionalDigits`, `char chosenNu[32]`, `char icuLocale[ULOC_FULLNAME_CAPACITY]`) | Idempotent: if `listFormatter != nullptr`, `ulistfmt_close(listFormatter); listFormatter = nullptr;` + `InvalidateMagic()` |
| `IntlEnumeration` (`EntrySupportedValuesOf`) | Stateless static class (no GC state object) | None (`JavascriptArray*` / `JavascriptString*` live on stack & returned to JS) | Stack-scoped `AsciiStringSet` (`stackPool[2048]`, `stackOffsets[128]`, optional `NoCheckHeapNewArray` growth) and `ScopedICUObject`s | Deterministic C++ destructor unwinding (`~AsciiStringSet`, `~ScopedICUObject`) |

---

## 3. Ownership Record (`doc/memory-management.md` Section 3)

```text
Resource / creation call / source location:
  1) GC-managed state wrapper:
     - `IntlDurationFormatState` (`lib/Runtime/Library/IntlDurationFormat.cpp:156-222`),
       allocated via `RecyclerNewWithBarrierFinalized` (`IntlDurationFormatState::New`) and
       attached to `DynamicObject` via `IntlModern::SetState(durationFormat, state)`
       (`InternalPropertyIds::HiddenObject`).
  2) Persistent ICU handle (ICU native heap):
     - `UListFormatter*` opened via `ulistfmt_openForType` (`IntlDurationFormat.cpp:1896`)
       and retained in `IntlDurationFormatState::listFormatter`.
  3) Call-local ICU handles & result buffers (`ChakraICU.h` `ScopedICUObject`, C-only APIs):
     - `IntlDurationFormat.cpp`: `ScopedUResourceBundle` (`ures_open`, `ures_getByKey` for
       CLDR `NumberElements/<nu>/symbols/timeSeparator` fallback), `ScopedUFormattedNumber`
       (`unumf_openResult`), `ScopedUNumberFormatter` (`unumf_openForSkeletonAndLocale`),
       `ScopedUFormattedList` (`ulistfmt_openResult`), `ScopedUConstrainedFieldPosition`
       (`ucfpos_open`).
     - `IntlEnumeration.cpp`: `ScopedULocaleDisplayNames` (`uldn_openForContext`),
       `ScopedUEnumeration` (`ucal_getKeywordValuesForLocale`, `ucol_getKeywordValues`,
       `ucurr_openISOCurrencies`, `unumsys_openAvailableNames`, `ucal_openTimeZoneIDEnumeration`),
       `ScopedUResourceBundle` (`ures_open`, `ures_openDirect`, `ures_getByKey`, `ures_getByIndex`),
       `ScopedUNumberingSystem` (`unumsys_openByName`).
  4) Native scratch buffers (`NoCheckHeap` with stack inline fast paths):
     - `AsciiStringSet` (`IntlEnumeration.cpp:154-378`): `stackPool[2048]` + `stackOffsets[128]`
       with checked `NoCheckHeapNewArray` / `NoCheckHeapDeleteArray` doubling.
     - `UCharBuffer` (`IntlDurationFormat.cpp:900-1007`): `stackBuf[128]` + `NoCheckHeapNewArray` /
       `NoCheckHeapDeleteArray` doubling (at most 10 group buffers + `timeSepNative` + skeleton).
     - `ScopedNativeArray<NumberFieldSpan>` & `ScopedNativeArray<int32_t>` (`IntlDurationFormat.cpp:1010-1039`)
       for constrained field position run-ownership reconstruction in `AppendNumberParts`.
     - `IntlModern::ScopedAsciiString` (`IntlModernShared.h:142-160`).

Domain and allocator instance (including backing allocator lifetime):
  - `IntlDurationFormatState` and retained `JavascriptString*` fields live in the owning
    `ScriptContext`'s `Recycler` domain.
  - All ICU handles and formatted results live in ICU4C's native heap (`libicuuc` / `libicui18n`).
  - Temporary C++ scratch buffers live on the C++ stack with bounded `NoCheckHeap` fallback.

Physical allocation base, extent, alignment, release callback or pool kind:
  - `IntlDurationFormatState` is a barriered finalized `Recycler` object whose sole finalizer
    teardown path is `Finalize(bool)` -> `IntlDurationFormatState::Dispose(bool)`.
  - `UListFormatter*` is freed once via `ulistfmt_close`; transient handles are freed by their
    paired `ScopedICUObject` close callbacks (`unumf_close`, `unumf_closeResult`, `ulistfmt_closeResult`,
    `ucfpos_close`, `ures_close`, `uenum_close`, `unumsys_close`, `uldn_close`).
  - `AsciiStringSet`, `UCharBuffer`, and `ScopedNativeArray<T>` store exact allocation capacities
    (`poolCap`, `offsetsCap`, `cap`, `count`) for paired `NoCheckHeapDeleteArray`.

Logical length / borrowed views (kept separate from the allocation descriptor):
  - `FormatDurationImpl` (`IntlDurationFormat.cpp:1363-1370`) immediately copies `timeSepStr` into
    native `UCharBuffer timeSepNative` before creating any JS result objects or `groupParts` arrays.
  - `listInputs[g]` (`IntlDurationFormat.cpp:1521-1527`) borrows `UChar*` pointers strictly from
    stack-owned `UCharBuffer groupStrings[Unit_Count]` only across `ulistfmt_formatStringsToResult`.
  - ICU `ufmtval_getString` / `ucurr_getName` / `ures_getStringByKey` pointers are read only
    within their owning `ScopedICUObject` lifetime and copied into fresh `Recycler` strings via
    `IntlModern::CreateUtf16String` / `IntlModern::CreateAsciiString` before scope exit.
  - `IntlEnumeration` borrows `JavascriptString*` key characters only during `TryClassifyKey` before
    starting enumeration; ICU enumeration pointers (`uenum_next`, `ures_getKey`) are copied immediately
    into `AsciiStringSet`.

Owner; transfer/publication point; remaining aliases:
  - Sole owner of `UListFormatter*` is `IntlDurationFormatState`, published onto the JS object via
    `IntlModern::SetState(durationFormat, state)` (`InternalPropertyIds::HiddenObject`) after
    all constructor coercions and ICU openings succeed.
  - `try { state = IntlDurationFormatState::New(...); } catch (...) { ulistfmt_close(listFormatter); throw; }`
    ensures OOM in `RecyclerNewWithBarrierFinalized` never leaks `listFormatter`.
  - If `SetState` returns `false`, `state` stays unpublished and is reclaimed by Recycler sweep.
  - `IntlEnumeration` retains zero off-stack state or aliases after returning.

Borrow invalidation: GC / detach / growth / mutation / reset / close / disposal:
  - No ICU handle or Recycler raw character buffer is held across any user-observable JS call.
  - `EntryFormat` / `EntryFormatToParts` validate `this` (`RequireState`), complete `ToDurationRecord`
    (which may trigger user getters/valueOf/GC), then re-acquire and re-validate `GetLiveListFormatter`
    before calling ICU.
  - `IntlDurationFormatState::Dispose(bool)` clears `listFormatter = nullptr` and calls
    `InvalidateMagic()`, preventing stale reuse or double close.

Potential user-code or reentrancy points:
  - `IntlDurationFormat::EntryNewInstance`: `OrdinaryCreateFromConstructor` (`newTarget.prototype` +
    cross-realm `CrossSite::MarshalVar`), `CanonicalizeLocaleList`, `GetOptionsObject`, and the
    24 option property reads (`localeMatcher`, `numberingSystem`, `style`, 10 `{unit}` + `{unit}Display`
    pairs, `fractionalDigits`) all finish before `ResolveLocale`, `ResolveTimeSeparator`,
    `ulistfmt_openForType`, and `SetState`.
  - `EntryFormat` / `EntryFormatToParts`: `ToDurationRecord` checks `VarIs<JavascriptString>`,
    else runs `GetProperty` + `JavascriptConversion::ToNumber` across all 10 units in exact
    alphabetical order (`days`, `hours`, `microseconds`, `milliseconds`, `minutes`, `months`,
    `nanoseconds`, `seconds`, `weeks`, `years`) before touching ICU.
  - `IntlEnumeration::EntrySupportedValuesOf`: `JavascriptConversion::ToString(keyArg, scriptContext)`
    completes before any ICU handle or `AsciiStringSet` allocation; returned array uses
    `CreateArray` + `DirectSetItemAt`, bypassing `Array.prototype` setters.

Thread/context allowed for use, root changes and destruction:
  - All JS entry points run strictly on the owning `ScriptContext` script thread.
  - `Finalize(bool)` / `Dispose(bool)` may execute on the Recycler finalizer thread or at
    runtime shutdown and touches only `listFormatter` (`ulistfmt_close`) and `magic`.

Release on success, partial failure, exception, OOM, cancel and shutdown:
  - Success: transient ICU handles and `NoCheckHeap` buffers unwind on scope exit; `listFormatter`
    lives on `IntlDurationFormatState` until GC finalization or `ScriptContext` shutdown.
  - Partial failure / exception / OOM:
    - Any user-code exception in constructor coercions throws before `ulistfmt_openForType`.
    - Failed `ulistfmt_openForType` closes non-null `listFormatter` before `ThrowIcuFailure`.
    - `RecyclerNewWithBarrierFinalized` OOM closes `listFormatter` inside `catch (...)`.
    - Any `U_MEMORY_ALLOCATION_ERROR` or `UCharBuffer`/`ScopedNativeArray`/`AsciiStringSet` OOM
      throws JS `OutOfMemoryError` while all `ScopedICUObject` and RAII locals unwind cleanly.

Memory accounting, capacity/retention bound and exhaustion behavior:
  - `IntlDurationFormatState` and cached `JavascriptString*`s are accounted under `Recycler` GC budgets.
  - `UCharBuffer` / `AsciiStringSet` check `INT32_MAX` overflow before doubling and convert failed
    `NoCheckHeapNewArray` allocations into `JavascriptError::ThrowOutOfMemoryError`.
  - `IntlDurationFormat` integer extraction and range bounds:
    - `ExtractExactInteger128` (`IntlDurationFormat.cpp:534-549`) decodes exact IEEE-754 mantissa
      and exponent bits into `unsigned __int128` after validating `mag < 2^32` for date units
      (`years`, `months`, `weeks`) and `mag < 1e26` for time units (`days`..`nanoseconds`),
      avoiding undefined `double -> uint64_t` casts on large finite values.
    - `ValidateDurationRecordRanges` (`IntlDurationFormat.cpp:551-578`) enforces `< 2^32` on
      `years`/`months`/`weeks` and exact `< kMaxTimeDurationNs` (`9007199254740992 * 10^9` ns = `2^53` s)
      in `unsigned __int128` across `days`..`nanoseconds` before overflowing multiplication.
    - `ParseIsoDurationString` (`IntlDurationFormat.cpp:590-816`) bounds integer component parsing
      at `kMaxTimeDurationNs` (`ParseAsciiDigits128`) and fractional components at at most 9 digits
      (`frac9`), checking `ValidateDurationRecordRanges` before returning.

Tests / commands / measured results / remaining uncertainty:
  - See §4 and §5 below (`2082/0` `test`, `2134/0` `debug-static` with `535` archive members,
    `release-shared check`, `jit -Dwasm=true` `382/382` P1 + `620/0` fulljit + `check`,
    `IntlSupportedValuesOfTest.js` & `IntlDurationFormatTest.js` `-CollectGarbage` in both
    interpreter & JIT, targeted Test262 `180/90` with `0` interp-vs-JIT diffs, and full
    `--intl402` `1,884/4,880` with `0` `pass->fail` regressions vs `I1`).
  - Remaining uncertainty: Linux x64 build & execution remain queued in Track `G0` (`tables_linux.zig`
    and `check_metadata_linux.py` are updated and locally pinned by `check-build-tools` `97/97`
    and `linux-gate-tests.zig` `9/9`); standalone ASan/UBSan rerun for `I2` can be executed during `G0`.
```

---

## 4. Multi-Profile Verification (`modernize/phase4`, macOS ARM64)

All gates executed on `modernize/phase4` after wiring [test/Intl/rlexe.xml](../test/Intl/rlexe.xml) and [tools/build/gate_counts.json](../tools/build/gate_counts.json):

| Command / Profile | Result | Observed Details |
|---|---|---|
| `mise exec -- zig build verify-bytecode` | PASS (`exit 0`) | `Bytecode is up to date`, `source headers unchanged; regen exit 0` (`Intl.js` & `JsBuiltIn*.bc.*.h` untouched) |
| `mise exec -- zig build check-selftest` | PASS | `pal-config: 95/95` (`test`), `u8a_codec.zig`, `owned-bytes.zig`, build-tool negative controls |
| `mise exec -- zig build check-build-tools` | PASS | `Ran 97 tests in 16.358s`, `OK` |
| `mise exec -- zig test tools/linux-gate-tests.zig` | PASS | `All 9 tests passed.` |
| `mise exec -- zig build check` (`test` profile) | PASS | `Total: passed 2082, failed 0` (`Intl: passed 23, failed 0`), `test-shared-basic : PASS`, `check_metadata: PASS`, `check_smoke: PASS` |
| `mise exec -- zig build -Dprofile=debug-static -p out/debug-static check` | PASS | `Total: passed 2134, failed 0` (`Intl: passed 23, failed 0`), `archive members -> 535`, `61531 T symbols`, `8/8` static native PASS, `check_metadata: PASS`, `check_smoke: PASS` |
| `mise exec -- zig build -Dprofile=release-shared -p out/release-shared check` | PASS | `test-shared-basic : PASS`, `check_metadata: PASS`, `check_smoke: PASS` |
| `mise exec -- zig build -Dprofile=jit -Dwasm=true -p out/jit check-wasm check-fulljit check` | PASS | `check_wasm_p1: PASS: P1 pass 382/382` (`runtests Total: passed 448, failed 5`), `check_fulljit: PASS (passed 620, failed 0)`, JIT `verify-bytecode` PASS, `check_jit: PASS`, `check_smoke: PASS` |
| Targeted `-CollectGarbage` (`./zig-out/bin/ch` & `./out/jit/bin/ch -ForceJITLoopBody -maxinterpretcount:1`) | PASS (`rc=0`) | Both `IntlSupportedValuesOfTest.js` and `IntlDurationFormatTest.js` pass (`pass`) under `-CollectGarbage` in both interpreter and forced JIT |

---

## 5. Test262 Targeted & Full `--intl402` Breakdown

### 5.1 Targeted `I2` Suites (`out/test262-i2-interp` vs `out/test262-i2-jit`)

Patterns: `test/intl402/Intl/supportedValuesOf/*.js` + `test/intl402/DurationFormat/**/*.js` (`270` scenarios across `135` files, `25/25` controls, ICU `79.1`):

- **Interpreter (`./zig-out/bin/ch`, `-nonative -Test262 -CollectGarbage -NegativePhaseReport`):** **`180 passed / 90 failed`**
  - `intl402/Intl/supportedValuesOf/`: **`40 passed / 10 failed`** (`20/25` files, `50` scenarios)
  - `intl402/DurationFormat/`: **`140 passed / 80 failed`** (`70/110` files, `220` scenarios — note `constructor-options-numberingSystem-valid.js` [`2` scenarios] turned green once `Intl.supportedValuesOf` and `Intl.DurationFormat` were integrated together)
- **JIT (`./out/jit/bin/ch`, `-ForceJITLoopBody -maxinterpretcount:1 -Test262 -CollectGarbage -NegativePhaseReport`):** **`180 passed / 90 failed`**
- **Interpreter vs. JIT diff:** **`0` differences across all `270` scenarios**.

### 5.2 Full Opt-In `--intl402` Refresh (`out/test262-intl402-phase4-i2` vs `out/test262-intl402-phase4-i1`)

Executed via:

```sh
python3 tools/test262/run.py --binary ./zig-out/bin/ch --intl402 \
  --output out/test262-intl402-phase4-i2 'test/intl402/**/*.js'
```

- **Totals:** `1,704 passed / 5,060 failed` (`I1`) -> **`1,884 passed / 4,880 failed`** (`I2`) across all **`6,764` scenarios** (`3,382` files, `25/25` controls, `0` skips).
- **Exact join (`6,764` scenarios):**
  - **`fail -> pass`: `180` scenarios** (`+140` `DurationFormat`, `+40` `Intl/supportedValuesOf`).
  - **`pass -> fail`: `0` regressions**.
- **Per-area breakdown (`out/test262-intl402-phase4-i2`):**
  - **Track `I2` areas (`180 / 270`):** `DurationFormat` `140/80` (was `0/220`), `Intl/supportedValuesOf` `40/10` (was `0/50`).
  - **Track `I1` areas (`1,052 / 1,054`, unchanged):** `DisplayNames` `114/0`, `ListFormat` `162/0`, `RelativeTimeFormat` `160/0`, `Segmenter` `158/0`, `Intl/getCanonicalLocales` `76/0`, `Intl/toStringTag` `4/0`, `Locale` `378/2` (`collations-supported-by-collator.js` from legacy `Intl.js` `Collator`).
  - **Pre-existing non-`I1`/`I2` areas (`652 / 5,440`):** `Temporal` `0/4,058` (deferred Track `C3`), `FallbackSymbol` `0/4`, legacy `Intl.js` (`Collator` `108/22`, `DateTimeFormat` `170/320`, `NumberFormat` `178/324`, `PluralRules` `68/38`, `String` `22/16`, `(root)` `38/6`), plus core `Array` `4/0`, `BigInt` `22/0`, `Date` `24/0`, `Intl` `2/0`, `Number` `14/0`, `TypedArray` `2/0`.

### 5.3 Root-Cause Audit of the `90` Remaining `I2` Scenarios (`45` Files)

1. **`Intl/supportedValuesOf` (`10` scenarios = `5` files):**
   All `5` failing files test legacy `Intl.js` constructor option acceptance rather than `Intl.supportedValuesOf` itself (all `18` core/DisplayNames/RTF/Locale/currency/timeZone files pass):
   - `calendars-accepted-by-DateTimeFormat.js`, `numberingSystems-accepted-by-DateTimeFormat.js` (legacy `Intl.js` `DateTimeFormat` falls back to `"gregory"` / `"latn"`)
   - `collations-accepted-by-Collator.js` (legacy `Intl.js` `Collator` ignores non-default collations)
   - `numberingSystems-accepted-by-NumberFormat.js`, `units-accepted-by-NumberFormat.js` (legacy `Intl.js` `NumberFormat` rejects `style: "unit"` and ignores modern numbering systems)
2. **`DurationFormat` (`80` scenarios = `40` files):**
   - **`35` files (`70` scenarios) blocked by legacy `Intl.js` `NumberFormat` inside `testIntl.js` assertions:**
     - `31` files throw `RangeError: Option value 'unit' for 'style' is outside of valid range. Expected: ['decimal', 'percent', 'currency']`.
     - `2` files (`negative-duration-with-leading-zero-style-digital-en.js` in `format` and `formatToParts`): legacy `Intl.js` `NumberFormat` ignores `signDisplay: "never"` so the Test262 harness helper computes `"-0:00:-01"` as expected while ChakraCore outputs the spec-correct `"-0:00:01"`.
     - `1` file (`rounding-mode-trunc-for-seconds.js`): legacy `Intl.js` `NumberFormat` ignores `roundingMode: "trunc"` (computing expected `"2"` via `halfEven`), while ChakraCore outputs the spec-correct `"1"`.
     - `1` file (`precision-exact-mathematical-values.js`): legacy `Intl.js` `NumberFormat` coerces string `"10000000.000000001"` through `ToNumber` (float64 rounding to `"10,000,000.000000002"`), while ChakraCore outputs the exact `"0:00:10000000.000000001"`.
     - **Node `v26.12.1` parity proof across all `39` `testIntl.js` `DurationFormat` formatting files (`35` failing + `4` digital-only passing):** Replacing `testIntl.js`'s `formatDurationFormatPattern` / `partitionDurationFormatPattern` assertion builders with passthrough comparisons of raw `Intl.DurationFormat.prototype.{format,formatToParts}` outputs between `./zig-out/bin/ch` and `/opt/homebrew/opt/node@26/bin/node` (`v26.12.1`) shows **`38/39` files (`94/96` calls) match Node `v26.12.1` identically**. The sole `2` differences are in `precision-exact-mathematical-values.js`, where ChakraCore returns the exact ECMA-402 mathematical values (`"0:00:9007200.256743991"` and `"0:00:9007199254740991.975424"`) while Node `v26.12.1` fails Test262 (`"0:00:9007200.256743992"` and `"0:00:9223372036.854775807"`) due to double/int64 overflow.
   - **`4` files (`8` scenarios) blocked by missing `Temporal`:**
     - `format/{taint-temporal-duration-prototype,temporal-duration-object-arg}.js` and `formatToParts/{taint-temporal-duration-prototype,temporal-duration-object-arg}.js` fail with `ReferenceError: 'Temporal' is not defined` (deferred to Track `C3` `Temporal`).
   - **`1` pure file (`2` scenarios) blocked by pre-existing JS numeric-separator scanner bug (now CLOSED by `Q2` commit `862882548`):**
     - `prototype/format/duration-out-of-range-2.js` line 96 evaluates `nextDown(9007199254740992_000_000_000)`. Prior to `Q2`, in [lib/Common/Common/NumberUtilities_strtod.cpp](../lib/Common/Common/NumberUtilities_strtod.cpp#L1130-L1182), `StrToDbl` stepped `pchLimDig -= numExcessiveDigits` (`25 - 20 = 5` chars) without skipping `'_'` separators, parsing `9007199254740992_000_000_000` as `9.007199254740992e+25` (`10x` larger than `9007199254740992000000000` = `9.007199254740992e+24`), causing `RangeError`. Track `Q2` (`862882548`) fixed `StrToDbl` separator skipping, lifting `DurationFormat` from `140 -> 142 / 220` and full `--intl402` from `1,884 -> 1,886 / 6,764` (`0` regressions; receipt in [q2-es2026-wave2.md](q2-es2026-wave2.md)). Logged in [open-cards.md](open-cards.md) §§18, 19; `Temporal` (`C3`) and legacy `Intl.js` `NumberFormat({style: "unit"})`/modern constructors (`Collator`, `DateTimeFormat`, `NumberFormat`, `PluralRules`) await Track `I3` / `C3`.
