# Track I1: Modern ECMA-402 `Intl` Wrapper Slices (`Locale`, `ListFormat`, `RelativeTimeFormat`, `DisplayNames`, `Segmenter`)

Status: Completed on `modernize/phase4` (macOS ARM64; Linux x64 revalidation queued in Track `G0`).
`Intl.js` and `lib/Runtime/Library/InJavascript/JsBuiltIn*.bc.*.h` are unchanged relative to `7519836ab3a302289f980e4396c6837ad33914bd` (all five modern built-ins live strictly in C++ over ICU4C's C APIs).

## 1. Architecture and Files

- Shared ECMA-402 scaffold and UTS-35 helpers: [lib/Runtime/Library/IntlModernShared.h](../lib/Runtime/Library/IntlModernShared.h), [lib/Runtime/Library/IntlModernShared.cpp](../lib/Runtime/Library/IntlModernShared.cpp) (plus `Intl.getCanonicalLocales` via `IntlModernBuiltIns::EntryGetCanonicalLocales`)
- Built-in implementations (strictly C ICU4C APIs under `U_SHOW_CPLUSPLUS_API 0` across `IntlModernShared.cpp` and all 5 `Intl<Class>.cpp` files; zero `icu::Locale` or other `icu::*` C++ calls and zero `std::` symbols):
  - `Intl.Locale` + Locale Info methods (`getCalendars`, `getCollations`, `getHourCycles`, `getNumberingSystems`, `getTimeZones`, `getTextInfo`, `getWeekInfo`): [lib/Runtime/Library/IntlLocale.cpp](../lib/Runtime/Library/IntlLocale.cpp), [test/Intl/IntlLocaleTest.js](../test/Intl/IntlLocaleTest.js)
  - `Intl.ListFormat`: [lib/Runtime/Library/IntlListFormat.cpp](../lib/Runtime/Library/IntlListFormat.cpp), [test/Intl/IntlListRelativeTimeTest.js](../test/Intl/IntlListRelativeTimeTest.js)
  - `Intl.RelativeTimeFormat`: [lib/Runtime/Library/IntlRelativeTimeFormat.cpp](../lib/Runtime/Library/IntlRelativeTimeFormat.cpp), [test/Intl/IntlListRelativeTimeTest.js](../test/Intl/IntlListRelativeTimeTest.js)
  - `Intl.DisplayNames` (v1 + v2): [lib/Runtime/Library/IntlDisplayNames.cpp](../lib/Runtime/Library/IntlDisplayNames.cpp), [test/Intl/IntlDisplayNamesTest.js](../test/Intl/IntlDisplayNamesTest.js)
  - `Intl.Segmenter`, `%Segments%`, `%SegmentIterator%`: [lib/Runtime/Library/IntlSegmenter.cpp](../lib/Runtime/Library/IntlSegmenter.cpp), [test/Intl/IntlSegmenterTest.js](../test/Intl/IntlSegmenterTest.js)
- Regression wiring & opt-in Test262 runner:
  - [test/Intl/rlexe.xml](../test/Intl/rlexe.xml) (4 new `<test>` entries tagged `Intl,exclude_windows` with `-args summary -endargs`)
  - [tools/build/gate_counts.json](../tools/build/gate_counts.json) (`2072/2124 -> 2076/2128`)
  - [tools/test262/run.py](../tools/test262/run.py) (`--intl402` flag via `resolve_patterns`), [tools/test262/test_run.py](../tools/test262/test_run.py), [tools/test262/README.md](../tools/test262/README.md)

### Cross-Realm `OrdinaryCreateFromConstructor` Fix

During `debug-static` testing, cross-realm `Reflect.construct(Intl.<Class>, args, otherRealmCtor)` with non-object `.prototype` (e.g. `null`) triggered `Assert(!CrossSite::NeedMarshalVar(prototype, scriptContext))` in `Type::Type` (`lib/Runtime/Types/Type.cpp:34`), because `IntlModern::OrdinaryCreateFromConstructor` (`lib/Runtime/Library/IntlModernShared.cpp`) passed the foreign realm's unmarshaled intrinsic prototype directly to `scriptContext->GetLibrary()->CreateObject(...)`. `OrdinaryCreateFromConstructor` now checks `CrossSite::NeedMarshalVar(targetProto, scriptContext)` and marshals `targetProto` via `CrossSite::MarshalVar(scriptContext, targetProto)` before constructing the instance.

---

## 2. Lifetime Graph and Write-Barrier Audit

### Lifetime Graph

```text
JS constructor / factory (new Intl.*, .segment(), %Segments%[@@iterator]())
  |
  |-- 1. OrdinaryCreateFromConstructor + user-visible coercions/getters complete
  |-- 2. Native ICU handle opened/cloned (or no handle for IntlLocaleState)
  |-- 3. State allocated via RecyclerNewWithBarrierFinalized(...)
  |        (if ICU open throws: state stays handle-less/unpublished;
  |         if state alloc throws after open: constructor closes ICU handle directly)
  |-- 4. State published onto JS instance via IntlModern::SetState(obj, state)
  |        (stores into InternalPropertyIds::HiddenObject)
  v
JS instance -> HiddenObject -> IntlModernState subclass (Recycler-managed)
  |                                |
  |                                +-- Field(JavascriptString*) slots (GC-traced)
  |                                +-- FieldNoBarrier(ICU handle*) (owned native handle)
  v
Method calls (format / formatToParts / of / segment / containing / next / resolvedOptions)
  |-- Coerce JS arguments first (may run arbitrary JS / GC)
  |-- Re-acquire & validate state via TryGetRawState / RequireState (checks magic + kind)
  |-- Leaf ICU window (no JS execution; Segmenter pins string + resets ubrk_setText(&kEmpty, 0))
  |-- Copy ICU results into fresh Recycler strings / plain JS objects
  v
Instance becomes unreachable -> Recycler sweep / runtime shutdown
  |-- Finalize(isShutdown) -> virtual Dispose(isShutdown)
  |-- Closes non-null ICU handle (uldn_close / udatpg_close / ulistfmt_close /
  |     ureldatefmt_close / ubrk_close), nulls handle pointer, calls InvalidateMagic()
```

### `Field(...)` Write-Barrier & Finalization Audit

All 8 state types inherit from `IntlModern::IntlModernState : public FinalizableObject` (`lib/Runtime/Library/IntlModernShared.h`) and are allocated with `RecyclerNewWithBarrierFinalized(...)`; `IntlModernShared.cpp` and all 5 `Intl<Class>.cpp` files use strictly C ICU4C APIs (`U_SHOW_CPLUSPLUS_API 0`, zero `icu::Locale` C++ calls, and zero `std::` symbols):

| State class | `IntlModernKind` | Traced GC slots (`Field(...)`) | Non-GC / ICU slots (`FieldNoBarrier(...)` / inline) | `Dispose(bool)` teardown |
|---|---|---|---|---|
| `IntlModernState` (base) | base | none (`Mark()` asserts unreachable; scanned directly as barriered finalized object) | `FieldNoBarrier(const void*) magic`, `FieldNoBarrier(IntlModernKind) kind` | `InvalidateMagic()` (`magic = nullptr`) |
| `IntlLocaleState` | `Locale` | `Field(JavascriptString*) tag` | none (ICU `UResourceBundle*` / `UEnumeration*` / `UDateTimePatternGenerator*` / `UNumberingSystem*` handles are call-local `ScopedICUObject`) | `InvalidateMagic()` (empty `Mark()` no-op) |
| `IntlListFormatState` | `ListFormat` | `Field(JavascriptString*) locale`, `type`, `style` | `FieldNoBarrier(UListFormatter*) formatter` | If non-null: `ulistfmt_close(formatter); formatter = nullptr;` + `InvalidateMagic()` |
| `IntlRelativeTimeFormatState` | `RelativeTimeFormat` | `Field(JavascriptString*) locale`, `style`, `numeric`, `numberingSystem` | `FieldNoBarrier(URelativeDateTimeFormatter*) formatter` (owns adopted `UNumberFormat*` inside ICU) | If non-null: `ureldatefmt_close(formatter); formatter = nullptr;` + `InvalidateMagic()` |
| `IntlDisplayNamesState` | `DisplayNames` | `Field(JavascriptString*) locale`, `style`, `type`, `fallback`, `languageDisplay` | `FieldNoBarrier(DisplayNamesType) typeKind`, `styleKind`, `fallbackKind`, `FieldNoBarrier(ULocaleDisplayNames*) displayNames`, `FieldNoBarrier(UDateTimePatternGenerator*) patternGenerator`, `char icuLocaleId[ULOC_FULLNAME_CAPACITY]` | Closes non-null `displayNames` (`uldn_close`), closes non-null `patternGenerator` (`udatpg_close`), nulls both, + `InvalidateMagic()` |
| `IntlSegmenterState` (`: IntlSegmentBreakStateBase`) | `Segmenter` | `Field(JavascriptString*) locale`, `granularityString`, `icuLocale` | `FieldNoBarrier(UBreakIterator*) breakIterator`, `FieldNoBarrier(SegmentGranularity) granularity` | Base `Dispose`: closes non-null `breakIterator` (`ubrk_close`), nulls pointer, + `InvalidateMagic()` |
| `IntlSegmentsState` (`: IntlSegmentBreakStateBase`) | `Segments` | `Field(JavascriptString*) string`, `icuLocale` | Inherited `breakIterator`, `granularity` | Inherited `IntlSegmentBreakStateBase::Dispose` |
| `IntlSegmentIteratorState` (`: IntlSegmentBreakStateBase`) | `SegmentIterator` | `Field(JavascriptString*) string` | Inherited `breakIterator`, `granularity`; `FieldNoBarrier(int32_t) index` | Inherited `IntlSegmentBreakStateBase::Dispose` |

Every `Dispose(bool)` is idempotent so repeated `Finalize`/`Dispose` calls during sweep and shutdown never double-close an ICU handle.

---

## 3. Ownership Record (`doc/memory-management.md` Section 3)

```text
Resource / creation call / source location:
  1) GC state wrappers: `IntlLocaleState` (`IntlLocale.cpp`), `IntlListFormatState` (`IntlListFormat.cpp`),
     `IntlRelativeTimeFormatState` (`IntlRelativeTimeFormat.cpp`), `IntlDisplayNamesState` (`IntlDisplayNames.cpp`),
     and `IntlSegmenterState` / `IntlSegmentsState` / `IntlSegmentIteratorState` (`IntlSegmenter.cpp`),
     all allocated via `RecyclerNewWithBarrierFinalized(...)` and attached to `DynamicObject` via
     `IntlModern::SetState(obj, state)` (`IntlModernShared.cpp`).
  2) Long-lived ICU handles (ICU library heap):
     - `UListFormatter*` via `ulistfmt_openForType` (`IntlListFormat.cpp`)
     - `URelativeDateTimeFormatter*` via `ureldatefmt_open` adopting a `UNumberFormat*` from `unum_open` (`IntlRelativeTimeFormat.cpp`)
     - `ULocaleDisplayNames*` via `uldn_openForContext` and `UDateTimePatternGenerator*` via `udatpg_open` (`IntlDisplayNames.cpp`)
     - `UBreakIterator*` via `ubrk_open` or `ubrk_clone` (ICU >= 69) (`IntlSegmenter.cpp`)
  3) Call-local ICU handles & results (`ChakraICU.h` `ScopedICUObject`; `IntlModernShared.cpp` and all 5 `Intl<Class>.cpp` use strictly C ICU4C APIs under `U_SHOW_CPLUSPLUS_API 0`, with zero `icu::Locale` C++ calls and zero `std::` symbols):
     - `ScopedUResourceBundle`, `ScopedUEnumeration`, `ScopedUDateTimePatternGenerator`, `ScopedUNumberingSystem` (`IntlLocale.cpp`, `IntlModernShared.cpp`; `ScopedULocale` and `ScopedULocaleBuilder` are only typedef'd in `ChakraICU.h:110-111` and unused in `.cpp`)
     - `ScopedUFormattedList`, `ScopedUConstrainedFieldPosition` (`IntlListFormat.cpp`)
     - `ScopedUFormattedRelativeDateTime`, `ScopedUNumberFormatter`, `ScopedUFormattedNumber`, `ScopedUConstrainedFieldPosition` (`IntlRelativeTimeFormat.cpp`)
  4) Native scratch buffers:
     - `IntlModern::ScopedAsciiString` (`stackBuf[256]` + `NoCheckHeapNewArray`/`NoCheckHeapDeleteArray`, `IntlModernShared.cpp`)
     - `TagBuilder` (`stackBuf[128]` + `NoCheckHeapNewArray`/`NoCheckHeapDeleteArray`, `IntlLocale.cpp`)
     - `ScopedStringViews` (`NoCheckHeapNewArray`/`NoCheckHeapDeleteArray`, `IntlListFormat.cpp`)
     - `ScopedNativeArray<T>` (`NoCheckHeapNewArray`/`NoCheckHeapDeleteArray`, `IntlRelativeTimeFormat.cpp`)
     - `AsciiBuffer` (`stackBuf[256]`) and `UCharBuffer` (`stackBuf[256]`) (`NoCheckHeapNewArray`/`NoCheckHeapDeleteArray`, `IntlDisplayNames.cpp`)

Domain and allocator instance (including backing allocator lifetime):
  - State objects and retained `JavascriptString*` fields live in the owning `ScriptContext`'s `Recycler` domain.
  - Persistent and call-local ICU handles live in ICU's native heap (`libicuuc` / `libicui18n`).
  - Temporary scratch allocations use `NoCheckHeap` (`NoCheckHeapNewArray` / `NoCheckHeapDeleteArray`) scoped to C++ stack frames.

Physical allocation base, extent, alignment, release callback or pool kind:
  - Recycler allocations are barriered finalized objects; their base/extent are managed by the `Recycler`,
    with `Finalize(bool)` -> `Dispose(bool)` as the sole finalizer release hook.
  - ICU opaque handles are freed strictly by their paired ICU C close APIs (`ulistfmt_close`, `ureldatefmt_close`,
    `uldn_close`, `udatpg_close`, `ubrk_close`, and `ScopedICUObject` destructors).
  - `NoCheckHeapNewArray` blocks store exact element count in the owning RAII wrapper (`len + 1`, `cap`, `count`)
    for paired `NoCheckHeapDeleteArray`.

Logical length / borrowed views (kept separate from the allocation descriptor):
  - `IntlListFormat::FormatList`: borrows `const UChar*` pointers from flattened `JavascriptString*` elements held in
    stack-rooted `JavascriptArray* list` inside `ScopedStringViews` only for the synchronous `ulistfmt_formatStringsToResult`
    leaf call (`list` keeps each `JavascriptString` and its `m_pszValue` buffer alive if a subsequent element's `GetString()` triggers GC).
  - `IntlSegmenter::FindSegmentBounds`: borrows `const char16*` from `pinnedString` inside `ENTER_PINNED_SCOPE(JavascriptString, pinnedString)`
    across `ubrk_setText(bi, text, length, &status)` -> `ubrk_isBoundary`/`ubrk_preceding`/`ubrk_following`/`ubrk_getRuleStatus`,
    and unconditionally calls `ubrk_setText(bi, &kEmpty, 0, &resetError)` before `LEAVE_PINNED_SCOPE()` and before allocating `{ segment, index, input[, isWordLike] }`.
  - `IntlDisplayNames`, `IntlLocale`, `IntlRelativeTimeFormat`: copy input strings out via `ScopedAsciiString` before ICU calls and
    copy ICU UTF-16 output buffers into fresh `Recycler` `JavascriptString` instances before returning.

Owner; transfer/publication point; remaining aliases:
  - Sole owner of each persistent ICU handle is its `Intl*State` instance, published onto the JS wrapper via
    `IntlModern::SetState(obj, state)` (`InternalPropertyIds::HiddenObject`) after construction coercions complete.
  - `IntlDisplayNamesState` and `IntlSegmentBreakStateBase` allocate the state first with null handles and transfer
    ownership via non-throwing `Adopt*` calls.
  - `IntlListFormatState` and `IntlRelativeTimeFormatState` allocate the state inside `try { ... } catch (...)` and
    explicitly close the ICU handle if `RecyclerNewWithBarrierFinalized` throws before state ownership is established.
  - `IntlRelativeTimeFormat::OpenFormatter` passes `UNumberFormat*` into `ureldatefmt_open(..., numberFormat, ...)`, which
    transfers ownership of `numberFormat` to ICU (even on failure).
  - `InternalPropertyIds::HiddenObject` is non-JS-enumerable; `TryGetRawState` validates object type, alignment,
    `IsValidState()` sentinel (`&gIntlModernStateSentinel`), and `IntlModernKind`.

Borrow invalidation: GC / detach / growth / mutation / reset / close / disposal:
  - No Recycler character pointer outlives a JS-reentrant boundary or survives into a `UBreakIterator` between calls.
  - `Segmenter` resets `ubrk_setText` to `&kEmpty` (`0` units) on every exit path of `FindSegmentBounds`, preventing
    `UBreakIterator` from holding a stale pointer if GC sweeps or moves/collects an unpinned buffer later.
  - `SegmentIterator` stores only scalar `int32_t index`, never an ICU internal cursor position, so concurrent iterators
    and interleaved `containing()` calls cannot corrupt one another.
  - `Dispose(bool)` clears ICU handle pointers to `nullptr` and calls `InvalidateMagic()`, preventing stale reads or double close.

Potential user-code or reentrancy points:
  - Constructors (`EntryNewInstance`): `newTarget.prototype` lookup (including cross-realm proxies), `CanonicalizeLocaleList`
    array/length/element coercion, `GetOptionsObject`/`CoerceOptionsToObject`, and every option getter/`ToString`/`ToBoolean`.
    All complete BEFORE ICU handle creation and `SetState` publication.
  - `Intl.DisplayNames.prototype.of`: `ToString(code)` runs first, then `RequireState` re-fetches and validates `state` from `this`.
  - `Intl.ListFormat.prototype.{format,formatToParts}`: `StringListFromIterable` runs full iterator protocol (`DoIteratorStepAndValue`,
    closing iterator on post-step errors) before `GetLiveFormatter(state, scriptContext)` re-validates `state`.
  - `Intl.RelativeTimeFormat.prototype.{format,formatToParts}`: `CoerceInputs` runs `ToNumber(value)` then `ToString(unit)` before
    `FormatRelative` checks `state->IsValidState()`.
  - `%Segments%.prototype.containing`: `ToInteger(index)` runs before reading `segments->GetString()` or touching `UBreakIterator*`.

Thread/context allowed for use, root changes and destruction:
  - All JS entry points execute strictly on the owning `ScriptContext` script thread.
  - `Finalize` / `Dispose` may run on the Recycler background/finalizer thread or at runtime shutdown; `Dispose` touches only
    the state's own ICU C handle (`ubrk_close`/`uldn_close`/`udatpg_close`/`ulistfmt_close`/`ureldatefmt_close`) and `magic` field,
    never touching `ScriptContext` or JS heap objects.

Release on success, partial failure, exception, OOM, cancel and shutdown:
  - Success: JS wrapper returned; call-local `ScopedICUObject` and `NoCheckHeap` RAII buffers destroyed on scope exit;
    owned ICU handles remain owned by `Intl*State` until GC finalization or `ScriptContext` shutdown.
  - Partial constructor failure / exception / OOM:
    - Any error during `CanonicalizeLocaleList` or option reads runs before ICU allocation.
    - Failed `uldn_openForContext`/`udatpg_open`/`ubrk_open`/`ubrk_clone`/`ulistfmt_openForType`/`ureldatefmt_open` cleans up any
      local handle immediately and throws JS `OutOfMemoryError` or `RangeError`.
    - Failed `RecyclerNewWithBarrierFinalized` in `ListFormat` / `RelativeTimeFormat` closes the opened handle in `catch (...)`.
    - `SetState` failure throws (`TypeError`/`OOM`), leaving any allocated state unpublished for normal Recycler finalization.

Memory accounting, capacity/retention bound and exhaustion behavior:
  - `IntlModernState` objects and cached `JavascriptString*` metadata are accounted under `Recycler` GC budgets.
  - Temporary scratch buffers (`ScopedAsciiString`, `TagBuilder`, `AsciiBuffer`, `UCharBuffer`) bound stack frames to 128-256 bytes/units
    and fall back to checked `NoCheckHeapNewArray` with `INT32_MAX / 2` / `OutOfMemoryError` guards.
  - ICU `U_MEMORY_ALLOCATION_ERROR` and `NoCheckHeap` OOM propagate as ChakraCore `JavascriptError::ThrowOutOfMemoryError`.

Tests / commands / measured results / remaining uncertainty:
  - ABI symbol check: `nm -u zig-out/lib/libChakraCore.dylib | grep icu | grep std` empty post-fix (and Wave 2 `out/release-shared` & `out/jit` dylibs clean prior to comment-only copyright edits; zero `std::` ICU imports and zero `icu::Locale` C++ calls across `IntlModernShared.cpp` and all 5 `Intl<Class>.cpp`).
  - StyleChecks & Linux table pins:
    - `tools/StyleChecks/{check_copyright,check_eol,check_tabs,check_ascii}.sh` all PASS (`exit 0`, 0 errors) across `origin/master..HEAD`, `7519836ab3a302289f980e4396c6837ad33914bd..HEAD`, and `35f53eae0a3599ea7e9c0baa6cf9331f7df3c351..HEAD` (added `// Copyright (c) ChakraCore Project Contributors. All rights reserved.` to `lib/Runtime/Library/JavascriptBuiltInFunctions.cpp` and `lib/Runtime/PlatformAgnostic/ChakraICU.h`).
    - `tools/build/tables_linux.zig` `rt_library_group.cxx` deduplicated from `696` to `690` TUs to match `tools/build/linux.zig` `expectedCompileCount(.@"test", true) == 690`, pinned by `mise exec -- zig test tools/linux-gate-tests.zig` (`9/9` passed) and `test_tables_linux_has_no_duplicate_sources_and_matches_compile_count` in `tools/build/test_build_tools.py`.
  - Unit & tooling checks: `python3 -m unittest discover -s tools/test262 -v` (`10` tests OK, covering `test_edition_policy.py` and `--intl402` in `test_run.py`); `mise exec -- zig build check-selftest` (PASS); `mise exec -- zig build check-build-tools` (`97` tests OK); `mise exec -- zig build verify-bytecode` (`Bytecode is up to date`, `Intl.js` & `JsBuiltIn*.bc.*.h` untouched vs `7519836ab3a302289f980e4396c6837ad33914bd`).
  - Multi-profile macOS ARM64 regression gates:
    - `mise exec -- zig build check`: `2076` passed / `0` failed (`+4`: `IntlLocaleTest.js`, `IntlListRelativeTimeTest.js`, `IntlDisplayNamesTest.js`, `IntlSegmenterTest.js`), `test-shared-basic` PASS, `check_metadata` PASS, `check_smoke` PASS.
    - `mise exec -- zig build -Dprofile=debug-static check`: `2128` passed / `0` failed (`+4`), `8/8` static native PASS, `533` archive members in `libChakraCoreStatic.a`, `check_metadata` PASS, `check_smoke` PASS.
    - Wave 2 pre-comment-edit `release-shared` & `jit` profiles (macOS ARM64): `zig build -Dprofile=release-shared check` (`test-shared-basic` PASS, `check_metadata` PASS, `check_smoke` PASS); `zig build -Dprofile=jit -Dwasm=true -p out/jit` `check-wasm` (`382/382` P1, `448/5` runtests), `check-fulljit` (`618/0`), `check` (`check_jit` PASS, JIT bytecode verify PASS, `check_smoke` PASS).
  - macOS ASan & UBSan sanitizer verification (`clang++` `-Dsanitize=address|undefined`, `ASAN_OPTIONS=detect_leaks=0`, `UBSAN_OPTIONS=print_stacktrace=1:halt_on_error=1`, `TZ=America/Los_Angeles`):
    - ASan (`mise exec -- zig build -Dsanitize=address check-sanitize`): `27/27` legs PASS; individual `-CollectGarbage` runs (`IntlLocaleTest.js`, `IntlListRelativeTimeTest.js`, `IntlDisplayNamesTest.js`, `IntlSegmenterTest.js`, `GetCanonicalLocales.js`) all `pass` (`rc=0`, zero ASan findings).
    - UBSan (`mise exec -- zig build -Dsanitize=undefined check-sanitize`): `27/27` legs PASS; individual `-CollectGarbage` runs on all 5 suites (plus `-noDeferParse -CollectGarbage` on `IntlListRelativeTimeTest.js`) all `pass` (`rc=0`, zero UBSan findings). Note: `test/Intl/IntlListRelativeTimeTest.js:210` replaced `p => ...` with `function (p) { ... }` to avoid pre-existing parser UB on unparenthesized single-argument `tkDArrow` (`lib/Parser/ptree.cpp:390` `PidRefStack::GetSymRef` with `ref == nullptr`, tracked separately in [open-cards.md](open-cards.md) §16).
  - Test262 `I1` targeted suites (527 files, 1054 scenarios, 25/25 controls; ICU 79.1, measured prior to comment-only copyright and test-arrow edits):
    - Interpreter (`./zig-out/bin/ch`, `-nonative -Test262 -CollectGarbage -NegativePhaseReport`): `1052 passed / 2 failed` (`DisplayNames` `114/0`, `Segmenter` `158/0`, `ListFormat` `162/0`, `RelativeTimeFormat` `160/0`, `Intl/getCanonicalLocales` `76/0`, `Intl/toStringTag` `4/0`, `Locale` `378/2`).
    - JIT (`./out/jit/bin/ch`, `-ForceJITLoopBody -maxinterpretcount:1 -Test262 -CollectGarbage -NegativePhaseReport`): `1052 passed / 2 failed` (identical per-directory results across all 527 files).
    - Sole 2 failures (`collations-supported-by-collator.js`, default + strict): pre-existing legacy `Intl.js` `Collator` ignores `{collation:"emoji"}` and returns `"default"`.
  - Full opt-in `test/intl402/**/*.js` (`./zig-out/bin/ch --intl402`): `1704 passed / 5060 failed` across `6764` scenarios (`3382` files, 25/25 controls).
  - Remaining uncertainty: Linux x64 build and test revalidation remain queued in Track `G0` (`tables_linux.zig` is deduplicated to `690` TUs and pinned locally by `linux-gate-tests.zig` `9/9` and `test_build_tools.py` `97/97`, awaiting native Linux execution); JS GC churn tests (`-CollectGarbage`) verify finalization survival/no-crash rather than directly observing Recycler finalizer execution counts.
```

---

## 4. Full `test/intl402/**/*.js` Breakdown (`out/test262-intl402-phase4-i1`)

Executed via:

```sh
python3 tools/test262/run.py --binary ./zig-out/bin/ch --intl402 \
  --output out/test262-intl402-phase4-i1 'test/intl402/**/*.js'
```

- **Total:** `1704 passed / 5060 failed` (`6764` scenarios, `3382` files, `25/25` controls, `0` skips).
- **Track `I1` areas (`1052 / 1054`):**
  - `DisplayNames`: `114 passed / 0 failed`
  - `ListFormat`: `162 passed / 0 failed`
  - `RelativeTimeFormat`: `160 passed / 0 failed`
  - `Segmenter`: `158 passed / 0 failed`
  - `Intl/getCanonicalLocales`: `76 passed / 0 failed`
  - `Intl/toStringTag`: `4 passed / 0 failed`
  - `Locale`: `378 passed / 2 failed` (`prototype/getCollations/collations-supported-by-collator.js` due to legacy `Intl.js` `Collator`)
- **Pre-existing non-`I1` areas & gaps (`652 / 5710`):**
  - `Temporal`: `0 passed / 4058 failed` (deferred C3 epic)
  - `DurationFormat`: `0 passed / 220 failed` (not in Track `I1`)
  - `Intl/supportedValuesOf`: `0 passed / 50 failed` (not in Track `I1`)
  - `FallbackSymbol`: `0 passed / 4 failed`
  - Legacy `Intl.js` constructors: `Collator` (`108/22`), `DateTimeFormat` (`170/320`), `NumberFormat` (`178/324`), `PluralRules` (`68/38`), `String` (`22/16`), plus top-level `language-tags-canonicalized.js` (`0/2`), `language-tags-invalid.js` (`0/2`), `supportedLocalesOf-returned-array-elements-are-not-frozen.js` (`0/2`).
  - Remaining passing legacy/core `intl402` checks: `Array` (`4/0`), `BigInt` (`22/0`), `Date` (`24/0`), `Intl` (`2/0`), `Number` (`14/0`), `TypedArray` (`2/0`), and 15 top-level `test/intl402/*.js` suites (`30/0`).
