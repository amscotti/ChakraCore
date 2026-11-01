# C2-JIT-1/2 fix: Set methods / groupBy reentrancy arming (jit-reentrancy-fix)

Recorded 2026-11-01, lane `jit-reentrancy-fix` (worktree
`ChakraCore-wt/jit-reentrancy-fix`, base `37123ee48` = main tip = the
`c2-jit-test262` merge). Fixes the class (c) defects filed by
[c2-jit-test262.md](c2-jit-test262.md): all seven `Set.prototype` set methods
(C2-JIT-1) and `Object.groupBy`/`Map.groupBy` (C2-JIT-2) SIGABRT when called
from a JIT-compiled caller. Severity per that receipt's dual-ACCEPT review:
reachable from default-configuration user code (deterministic ≥1000-iteration
warm loop), not an exotic configuration.

## Root cause (confirmed in source)

Every direct `CALL_FUNCTION` first executes
`ThreadContext::CheckAndResetReentrancySafeOrHandled()`
(`lib/Runtime/Language/Arguments.h:152-196` in both the
`ENABLE_JS_REENTRANCY_CHECK` and fallback branches), which
`AssertOrFailFast(reentrancySafeOrHandled || isInReentrancySafeRegion)`
(`lib/Runtime/Base/ThreadContext.h:1825-1829`). The interpreter's `OP_Call`
handlers wrap every call in `BEGIN_SAFE_REENTRANT_REGION`
(`lib/Runtime/Language/InterpreterStackFrame.cpp:3957/4000`), so an
interpreted caller enters the builtin with `isInReentrancySafeRegion` set and
the un-armed `CALL_FUNCTION` inside the helper still passes. A JIT-compiled
caller enters the C++ builtin with **neither** flag set, and the first
C++→JS `CALL_FUNCTION` fails fast (`FatalInternalError` → `abort`, exit 134).

Which transitions were un-armed: exactly the **direct `CALL_FUNCTION` uses**
in the Set-method/groupBy helpers. The other user-code transitions in the
same builtins were already armed by machinery this fix does not touch:

- getter/valueOf/toString and `next`/`done`/`value` property reads route
  through `ThreadContext::ExecuteImplicitCall`
  (`ThreadContext.h:1609-1611`), whose first statement installs an
  `AutoReentrancyHandler` — this is why `SetMethodGetProperty`
  (JavascriptSet.cpp:~300), the `size` ToNumber (JavascriptSet.cpp:~341),
  `JavascriptOperators::GetIterator`, `CacheIteratorNext` and the
  `IteratorStepAndValue` internals are clean under JIT callers;
- `JavascriptMap::EntryForEach` (JavascriptMap.cpp:179) and the Set/Map
  constructor adders (JavascriptMap.cpp:99, JavascriptSet.cpp:77) show the
  `BEGIN_SAFE_REENTRANT_CALL` idiom these helpers lacked — which is why
  sort comparators, `forEach` and the constructor walks never aborted.

## Fix (the reviewers' complete sweep, per site)

Idiom: `JS_REENTRANT(jsReentLock, BEGIN_SAFE_REENTRANT_CALL(tc) { CALL_FUNCTION(...); } END_SAFE_REENTRANT_CALL);`
— the established combination from `JavascriptArray.cpp:8522-8531`
(`FindHelper`'s callback), matching `EntryForEach`'s semantics.

| Site (file:old line) | Transition armed | Notes |
|---|---|---|
| `SetMethodCallHas` (JavascriptSet.cpp:374) | `record.has` `CALL_FUNCTION` | used by intersection/difference (has-walk), isSubsetOf, isDisjointFrom |
| `GetSetLikeKeysIterator` (JavascriptSet.cpp:385) | `record.keys()` `CALL_FUNCTION` | the `next` Get below routes through self-arming `ExecuteImplicitCall` |
| `SetLikeKeysStep` (JavascriptSet.cpp:404) | wrapper arm around `IteratorStepAndValue` | defense-in-depth: its next/done/value transitions self-arm via `ExecuteImplicitCall` today; the wrapper keeps a future direct-call fast path (the `CONSIDER` at JavascriptOperators.cpp:12371) armed for Set-method callers |
| `CloseKeysIteratorNormalCompletion` (JavascriptSet.cpp:431) | `return()` `CALL_FUNCTION` | reached on isSupersetOf/isDisjointFrom early-normal-exit |
| `MapGroupByCallCallback` (JavascriptMap.cpp:356) | groupBy callback `CALL_FUNCTION` | `Object.groupBy` has its own copy — both patched |
| `GroupByIteratorStepAndValue` (JavascriptObject.cpp:1222) | wrapper arm as `SetLikeKeysStep` | |
| `GroupByCallCallback` (JavascriptObject.cpp:1232) | groupBy callback `CALL_FUNCTION` | |

The already-armed sites (`SetMethodGetProperty`, `GetSetLikeRecord`'s
ToNumber, `MapGroupByGetIterator`/`CacheIteratorNext`/`IteratorStepAndValue`
internals, both constructors' adders, `EntryForEach`) are **unchanged**, per
the sweep instruction.

### Memory-management statement (doc/memory-management.md)

`BEGIN_SAFE_REENTRANT_CALL`/`END_SAFE_REENTRANT_CALL` are scoped RAII guards
(`AutoReentrancyHandler`, ThreadContext.h:1964-1981) that save, set and
restore the ThreadContext `reentrancySafeOrHandled` **bool**. **No
allocation, ownership, retained pointer, root, write barrier, GC tracing or
memory-accounting change is involved**; destructors run on exception
unwinding, so the saved flag is restored on every exit path. The independent
`noJsReentrancy` protection (`AssertJsReentrancy` /
`FatalJsReentrancyError`, ThreadContext.h:1813-1819) is untouched, so the
arming cannot mask legitimate re-entrancy prevention — confirmed by the
51-probe sweep staying clean (below).

## Exception-semantics analysis (arming must not change spec behavior)

`AutoReentrancyHandler` is a plain stack guard: a user exception (C++
`JavascriptException` unwinding) propagates through it unchanged, with the
flag restored by the destructor. Verified per shape by
`tools/jit-test262/battery/exn-set-methods.js` and `exn-groupby.js`
(sentinel-object identity asserted, not just "threw"):

- **throwing `has`** — the sentinel propagates; `keys()` is never read; no
  iterator exists to close (per-method callback counts asserted).
- **throwing `keys()`** — sentinel propagates from all six keys-reading
  methods (isSubsetOf never reads keys); no `next`/`return` calls happen.
- **throwing `next()`** — sentinel propagates and the iterator is **not**
  closed (`IteratorStepValue` has no `IfAbruptCloseIterator`); done/value
  getter throws likewise propagate un-closed.
- **`keys()` returning a non-object** — `TypeError` (JSERR_NeedObject).
- **normal-completion close** (`CloseKeysIteratorNormalCompletion`) —
  `return()` runs observably on the isSupersetOf/isDisjointFrom early-false
  paths; a throwing `return()` propagates (this close site is on a *normal*
  completion); a non-object `return()` result is a `TypeError`.
- **`ToBoolean(has())`** — a result object with throwing `valueOf` is truthy
  **without** `valueOf` being called (ToBool runs no user code).
- **groupBy callback throw** — sentinel propagates, the items iterator is
  closed exactly once (`IfAbruptCloseIterator` at the Call site), and a
  throwing `return()` **loses to the original error**. `Object.groupBy`'s
  `ToPropertyKey` site behaves the same (close + original wins).
- **groupBy `next()`/done/value throws** — propagate **without** close
  (step sits outside the close-wrapping try). `@@iterator` getter throw and
  the non-callable-callback `TypeError` (before items is touched) unchanged.

All of the above asserted identical under nojit (oracle), default, `-mic:1`,
interp, forced-SimpleJit and fulljit flags.

## Verification

### Battery (tools/jit-test262/battery, run.sh; each cell = exit code)

Pre-fix (base binary, commit b25be05ca's matrix) → post-fix:

| mode | warmed-set-methods | warmed-groupby | exn-set-methods | exn-groupby | callback-probes |
|---|---|---|---|---|---|
| default (no flags) | 134 → **0** | 134 → **0** | 134 → **0** | 134 → **0** | 0 → 0 |
| `-mic:1` | 134 → **0** | 134 → **0** | 134 → **0** | 134 → **0** | 0 → 0 |
| interp flags | 134 → **0** | 134 → **0** | 134 → **0** | 134 → **0** | 0 → 0 |
| `-forceNative -off:fulljit` (SimpleJit) | 134 → **0** | 134 → **0** | 134 → **0** | 134 → **0** | 0 → 0 |
| fulljit flags | 134 → **0** | 134 → **0** | 134 → **0** | 134 → **0** | 0 → 0 |
| `-nonative` (oracle) | 0 → 0 | 0 → 0 | 0 → 0 | 0 → 0 | 0 → 0 |

The warmed batteries assert **values**, not just exit codes: for every one
of the seven methods, exact result contents for both the keys-walk
(receiver larger) and the has-walk (receiver not larger), exact
`size`/`has`/`keys`/`next`/`return` call counts per method (proving the
armed transitions actually executed), real-Set arguments, −0 key
canonicalization; groupBy key order/group contents/-0 normalization/empty
and symbol-key shapes with callback and iterator call counts. The
**default-flags** column is the severity-upgrade reproduction: ≥1000-iteration
**top-level** warm loops deterministically tier the per-pass helpers by
invocation count (a `warm()`-wrapped loop does *not* tier — established
empirically in this lane; the batteries were restructured accordingly).
The 51-probe callback sweep (rebuilt from the adversarial review's 42-probe
list: sort/toSorted/map/filter/forEach/reduce(+Right)/find*
/some/every/flatMap/Array.from, replace/replaceAll with functions, TypedArray
callback set, iterator helpers, Proxy traps, JSON reviver/replacer/toJSON,
Date.toJSON, getters, valueOf/toString coercion, Map/Set forEach and
constructors, Promise.then, bound functions, generators, species subclass)
stayed **clean in every mode before and after the fix** — the arming masked
nothing.

Repros A/B on the fixed binary:
`tools/jit-test262/repros/*.js` now exit 0 with correct output (`2` / `1`)
under nojit, interp **and fulljit** flags (was 0/0/134).

### Corpora (tools/jit-test262/campaign.py, re-scoped batches)

Rebuilt the re-scoped batches on the fixed `out/jit/bin/ch`
(`campaign.py run s10-setmethods s11-remainder` + `diff`, through the gate
lock; the pinned test262 checkout verified clean; 25 controls passed in
every run):

| Batch | nojit | interp | fulljit | diffs vs nojit |
|---|---|---|---|---|
| `s10-setmethods` | 372/0 of 372 | 372/0 of 372 | **372/0 of 372** (was 218/154) | interp: 0; fulljit: **0** (was 154) |
| `s11-remainder` | 170/0 of 170 | 170/0 of 170 | **170/0 of 170** (was 152/18) | interp: 0; fulljit: **0** (was 18) |

Both recoveries are exact nojit parity: the 154 + 18 class-(c) SIGABRT
scenarios now pass under fulljit with zero mode divergence anywhere — the
C2 §"RE-SCOPED, not discharged" residue for these two batches is discharged.
Per-mode wall ≈ 2-3 s per batch/mode (`--jobs 8`); evidence in
`out/c2jit/<batch>/<mode>/{results,summary}.json` and `<batch>/*.run.log`;
committed snapshot table updated at `tools/jit-test262/campaign-table.json`
(live copy `out/c2jit/campaign-table.json`).

### Gates

- `zig build -Dprofile=jit` (fixed engine, through the gate lock):
  `BUILD_EXIT:0`; `zig build -Dprofile=jit check`: **green** —
  `check_smoke` PASS, `check_jit` PASS (NativeCodeGenerator symbol present;
  hot call/rest/apply correct under both `-mic:1 -bgjit-` and
  `-forceNative -off:simpleJit -bgJitDelay:0`), JIT builtin bytecode verify
  green.
- rlexe fulljit spot (dynapogo flags on `out/jit/bin/ch`, outputs compared
  against `-nonative`): `test/es7/setmethods.js`, `test/es7/s11-builtins.js`
  and the new `test/es7/setmethods-groupby-warmed.js` all exit 0 with
  **byte-identical** nojit/fulljit output.
- `zig build check-regression` (no-JIT test profile, through the gate lock):
  **`Total: passed 2034, failed 0`** — `check_regression: PASS` at the
  bumped pin: the 2033 legacy tests are untouched by the arming and the one
  added entry passes.
- `zig build -Dprofile=debug-static check-regression` (cold Debug build +
  `--debug --variants=disable_jit`): **`Total: passed 2086, failed 0`** —
  `check_regression: PASS` at the bumped debug pin (also exercises the
  `ENABLE_JS_REENTRANCY_CHECK`+DBG macro shape of the edit).
- Battery + corpus runs above also went through the shared gate lock.

## New regression test

`test/es7/setmethods-groupby-warmed.js` + rlexe entry
(`-args summary -endargs`), one test per selection: gate pins bumped
**2033→2034 (test) / 2085→2086 (debug-static)**, `_lineage` updated. The
test's warm phase is a ≥1000-iteration top-level loop (the deterministic
tier-up shape) over all seven set methods (real-Set and recorder set-like
arguments), both groupBy entries, and two throwing-transition probes; the
UnitTestFramework tests then assert the recorded evidence (contents,
callback counts, sentinel identities, close semantics), so value regressions
fail even when nothing aborts. Measured 0.17-0.20 s (release, per mode) —
well inside runtests' 60 s per-test timeout even at debug-build speeds.

## Residuals

- The two defense-in-depth wrapper arms (`SetLikeKeysStep`,
  `GroupByIteratorStepAndValue`) are redundant with `ExecuteImplicitCall`'s
  internal arming today; they exist so the sweep instruction's complete
  enumeration is implemented verbatim and so a future built-in-iterator
  fast path inside `IteratorStepAndValue` (the CONSIDER at
  JavascriptOperators.cpp:12371) cannot reopen this hole for these callers.
- Linux JIT evidence remains owed via B4 (unchanged from the campaign
  receipt).
- Lane process residue: during battery construction this lane twice
  mis-shaped test iterators (a `value:` expression that did not advance the
  index → an apparent engine "hang" that was the test looping forever).
  No engine defect was found in the Set/Map constructors or
  `DoIteratorStepAndValue`; both were test bugs, fixed in the committed
  battery (advance-then-yield recorders throughout).
