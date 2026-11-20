# C3.1 Explicit Resource Management (`modernize/phase4`)

> **Status:** **COMPLETED (`modernize/phase4`)** on macOS ARM64 (`410d4a198`, layered over Wave 0 `13ce5c11c` and `modernize/baseline` `1bf42d1f6`).
> **Scope invariant:** `modernize/baseline` remains untouched at `1bf42d1f6`. Bytecode-header purity is preserved (`lib/Runtime/Base/JnDirectFields.h`, `lib/Runtime/InternalPropertyList.h`, `lib/Runtime/Library/InJavascript/Intl.js`, and `lib/Runtime/Library/InJavascript/JsBuiltIn*.bc.*.h` are completely unchanged; `verify-bytecode` exits `0`).

---

## 1. Commit Lineage on `modernize/phase4`

1. **`13ce5c11c` (`Wave 0` scaffold):** Unregistered bound symbols `Symbol.dispose` and `Symbol.asyncDispose` via `ScriptContext::GetSymbol` + TTD replay in `TTSnapTypes::InflatePropertyRecord_CreateNew`, built-in scaffolds (`SuppressedError`, `DisposableStack`, `AsyncDisposableStack`, `Iterator.prototype[Symbol.dispose]`, `%AsyncIteratorPrototype%[Symbol.asyncDispose]`), `DisposeCapability` Recycler infrastructure, 7 extended opcodes (`AddSyncDisposableResource`, `AddAsyncDisposableResource`, `RecordDisposeError`, `DisposeResourcesSync`, `StepDisposableResource`, `TakeDisposeAwaitValue`, `FinalizeDisposeResources`), and interpreter handlers.
2. **`9216555d5` (`Wave 1A`, cherry-picked from `0ff6335ff`):** Edge-case completion for `SuppressedError` (`OrdinaryCreateFromConstructor` before `ToString(message)`, omit `options.cause`, non-enumerable `"message"`/`"error"`/`"suppressed"` via `SetPropertyWithAttributes`, attach `.stack` after `SetSuppressedErrorFields`), primitive `thisVar` receiver in `GetPropertyNoCache` + `library->GetThrowerFunction()` in `PerformPromiseThen` (`lib/Runtime/Library/JavascriptIterator.cpp`), `Field(bool) needsAwait`/`Field(bool) hasAwaited` deferred trailing `Await(undefined)` + `CrossSite::MarshalVar` across disposal boundaries (`lib/Runtime/Library/JavascriptDisposableStack.{h,cpp}`), and `test/es7/explicit_resource_management_builtins.js`.
3. **`c01e64d80` (`Wave 1B`, cherry-picked from `709485bc0`):** Parser, AST, and bytecode emission for `using` / `await using` (`WellKnownPropertyPids::_using`, `[no LineTerminator]` + `BindingIdentifier` lookahead, `for`/`for-of`/`for-await-of` header support including `for (using of ...)`, early syntax errors, `ParseNodeVar::isUsing`/`isAwaitUsing`, `ParseNodeBlock::hasUsing`/`hasAwaitUsing`/`HasDisposableResources()`, `MarkBlockDisposableResources`, `BeginDisposalScope`/`EndDisposalScope` `TryFinally`/`TryFinallyWithYield` + inner `TryCatch` unwind emission, `HasJumpCleanup()`, `InvertLoop` guard, and `test/es7/using_and_await_using.js`).
4. **`0d64f14ea` (`Wave 1C`, cherry-picked from `25eed65f3`):** Multi-tier JIT lowering in `lib/Backend/Lower.cpp` (`LowerBinaryHelperMem` for `AddSyncDisposableResource`, `AddAsyncDisposableResource`, `RecordDisposeError`; `LowerUnaryHelperMem` for `DisposeResourcesSync`, `StepDisposableResource`, `TakeDisposeAwaitValue`, `FinalizeDisposeResources`) and `test/Optimizer/c31_disposal_jit.js`.
5. **`410d4a198` (`Wave 2` integration, fixes, & gate wiring):**
   - **SimpleJIT `using` EH fix (`lib/Parser/ptree.{h,cpp}`, `lib/Runtime/ByteCode/ByteCodeGenerator.cpp`, `lib/Runtime/ByteCode/ByteCodeEmitter.cpp`):** Replaced `AcquireTmpRegister()` for disposal stack registers with a persistent var register (`ParseNodeBlock::disposalStackReg` allocated via `ByteCodeGenerator::NextVarRegister()`). Under `IRBuilder::BuildDstOpnd`, un-reused temp registers (`>= firstTmpReg`) mint a fresh `SymID` per bytecode def (`AddSyncDisposableResource`, `RecordDisposeError`), causing SimpleJIT (`!DoOptimizeTry()`) catch blocks to read an unexecuted post-throw `SymID` slot when a subsequent `using` initializer threw.
   - **DBG+JIT helper attribute alignment (`lib/Backend/JnHelperMethodList.h`, `lib/Runtime/Language/JavascriptOperators.cpp`):** Aligned `Op_TakeDisposeAwaitValue` (`AttrCanNotBeReentrant`) and `Op_FinalizeDisposeResources` (`AttrCanThrow | AttrCanNotBeReentrant`) with `JIT_HELPER_NOT_REENTRANT_NOLOCK_HEADER` (`OpCodes.h:959-960` intentionally omits `OpHasImplicitCall`; checked `AddSyncDisposableResource`..`StepDisposableResource` match `OpHasImplicitCall` + `AttrCanThrow`).
   - **Cross-realm intrinsic prototype marshal fix (`lib/Runtime/Language/JavascriptOperators.cpp`):** Marshaled `GetRealmIntrinsicPrototype(...)` results via `CrossSite::MarshalVar(scriptContext, realmProto)` before `JavascriptObject::ChangePrototype` inside `OrdinaryCreateFromConstructor`, fixing `debug-static` assertion `! (prototype && CrossSite::NeedMarshalVar(prototype, ...))` (`lib/Runtime/Types/Type.cpp:54`) on `Reflect.construct(SuppressedError, ..., crossRealmNewTarget)`.
   - **Style hygiene & regression gate wiring:** Added missing copyright header in `lib/Runtime/Debug/TTSnapTypes.cpp`; expanded `test/Optimizer/c31_disposal_jit.js` (`testAsyncStackWarm()` at `25` batches + `testSyncUsingInJit()` `300`-iter JIT/GC stress); registered all 3 new tests in `test/es7/rlexe.xml` and `test/Optimizer/rlexe.xml`; updated `tools/build/gate_counts.json` (`test` `2089/0`, `debug` `2141/0`).

---

## 2. Pre-existing ARM64 JIT Write-Barrier Card (Resolved)

Stress testing `testAsyncStackWarm()` at `n = 100..300` concurrent `AsyncDisposableStack` chains uncovered a **pre-existing, non-C3.1 ARM64 JIT software write-barrier bug** in `lib/Backend/arm64/LowerMD.cpp` (`LowererMD::ChangeToWriteBarrierAssign`). Per coordinator guidance (`modernize/phase4` strict C3.1 scoping), `LowerMD.cpp` was left untouched in `410d4a198` with `testAsyncStackWarm()` scoped to `n = 25`. That card is now **resolved** on `modernize/phase4` ([arm64-write-barrier-jit.md](arm64-write-barrier-jit.md); [open-cards.md](open-cards.md) §2 & §22) and `testAsyncStackWarm()` in [c31_disposal_jit.js](../test/Optimizer/c31_disposal_jit.js) is restored to `n = 200` (`20/20` clean across default, ForceJITLoopBody, ForceNative, simpleJit-only, fullJit-only, `-CollectGarbage`, ASan, and UBSan).

---

## 3. Memory Management & GC Ownership Record (`doc/memory-management.md` section 3)

| Type / Surface | Allocation Domain | Owner & Lifetime Transfer | Barrier / Pinning Invariant | Invalidation & Thread Context |
|---|---|---|---|---|
| `JavascriptDisposableStack` / `JavascriptAsyncDisposableStack` (`JavascriptDisposableStack.{h,cpp}`) | Script `Recycler` heap (`DynamicObject`) | Created in current `ScriptContext` (`NewDisposableStackImpl` -> `OrdinaryCreateFromConstructor`); holds `Field(DisposeCapability*)`. Non-finalizable `DynamicObject` collected normally once unreachable. | All internal pointers (`DisposeCapability*`, resource slots, error values) use `Field(...)` write-barriered fields; JS resource/callback arguments crossing realms pass through `CrossSite::MarshalVar`. | Stack state transitions to disposed via `move()` or `[Symbol.dispose]`/`[Symbol.asyncDispose]` (`DisposeCapability` pops/clears stack entries LIFO). Single-thread script/GC safepoint context only. |
| `DisposeCapability` (`JavascriptDisposableStack.{h,cpp}`) | Script `Recycler` heap | Allocated for built-in stacks and `using`/`await using` lexical scopes (`BeginDisposalScope`). Held either on a JS frame register (`disposalStackReg`) or on `JavascriptDisposableStack` / `JavascriptAsyncDisposableStackStepFunction`. | Traces resource array/entries, pending exception `Var`, and `await` value via `Field(...)`. `Field(bool) needsAwait` / `Field(bool) hasAwaited` track deferred trailing `Await(undefined)`. Snapshots/pops entry LIFO before invoking user disposer callbacks so reentrant GC or stack mutation cannot double-dispose or read stale slots. | Empties entries incrementally during disposal; `FinalizeDisposeResources` clears remaining pending state or rethrows pending error / `SuppressedError`. |
| `JavascriptAsyncDisposableStackStepFunction` (`JavascriptDisposableStack.{h,cpp}`) | Script `Recycler` heap (`JavascriptFunction`) | Allocated per `AsyncDisposableStack.prototype.disposeAsync()` step continuation and retained solely by `Promise` reaction jobs while suspended across `await`. | Holds `Field(DisposeCapability*)` and resolver callbacks with Recycler `Field(...)` barriers so concurrent/GC sweeps (`-CollectGarbage`) trace the live capability chain across microtask turns. | Releases capability naturally once final resolve/reject continuation finishes and promise reactions drain. |
| `ParseNodeBlock::disposalStackReg` (`ptree.h`, `ByteCodeGenerator.cpp`, `ByteCodeEmitter.cpp`) | Compile-time AST node; runtime bytecode var slot (`< firstTmpReg`) | Allocated once per disposable block/loop head in `MarkBlockDisposableResources` via `ByteCodeGenerator::NextVarRegister()`. Holds runtime `DisposeCapability*` `Var` on interpreter/JIT frame. | Living in the function's traced var register window keeps the `DisposeCapability*` visible to conservative/exact stack scanning across `await`/`yield`, implicit calls, and EH catch/finally blocks, and prevents SimpleJIT `SymID` splitting across `try`/`catch`. | Overwritten/released when enclosing disposal scope exits or function frame returns. |
| `SuppressedError` & cross-realm prototypes (`JavascriptError.cpp`, `JavascriptOperators.cpp`) | Script `Recycler` heap (`JavascriptError`) | `NewSuppressedErrorInstance` creates `pError` in caller `ScriptContext`; `OrdinaryCreateFromConstructor` resolves `newTarget.prototype` or `GetRealmIntrinsicPrototype(...)`. | `GetRealmIntrinsicPrototype` cross-realm prototypes are marshaled via `CrossSite::MarshalVar(scriptContext, realmProto)` before `ChangePrototype`, maintaining `!CrossSite::NeedMarshalVar(prototype, scriptContext)` for `Type::Type`. | Single-thread JS execution and `Recycler` tracing; `SetSuppressedErrorFields` runs before `.stack` attachment. |

---

## 4. Verification Summary (macOS ARM64, post-edit `410d4a198`)

### 4.1 Targeted `explicit-resource-management` Test262 Matrix

- **Corpus slice (`483` files, `947` scenarios, `25/25` controls):** `built-ins/Symbol/{dispose,asyncDispose}`, `built-ins/SuppressedError`, `built-ins/Iterator/prototype/Symbol.dispose`, `built-ins/AsyncIteratorPrototype/Symbol.asyncDispose`, `built-ins/DisposableStack`, `built-ins/AsyncDisposableStack`, `language/statements/{using,await-using,for-of/**/*using*,for-await-of/**/*using*}`, and `staging/explicit-resource-management`.
- **Interpreter (`./out/jit/bin/ch -nonative -Test262 -CollectGarbage -NegativePhaseReport`, `out/test262-c31-interp-final`):** **`947 passed / 0 failed`** (`483` files).
- **Forced FullJIT (`./out/jit/bin/ch -ForceJITLoopBody -maxinterpretcount:1 -Test262 -CollectGarbage -NegativePhaseReport`, `out/test262-c31-fulljit-final`):** **`947 passed / 0 failed`** (`483` files).
- **`-Trace:BackEnd -ForceNative -bgjit-` & direct JIT/GC modes:** Confirmed `EndBackEnd` completes on `c31_disposal_jit.js` functions (`SimpleJit` / `FullJit`); all 3 C3.1 regression tests (`test/es7/explicit_resource_management_builtins.js`, `test/es7/using_and_await_using.js`, `test/Optimizer/c31_disposal_jit.js`) pass under `-nonative -CollectGarbage`, `-ForceNative -bgjit- -off:fullJit -CollectGarbage`, `-ForceNative -bgjit- -off:simpleJit -CollectGarbage`, `debug-static -CollectGarbage`, ASan `-CollectGarbage`, and UBSan `-CollectGarbage`.

### 4.2 Full Certified Test262 Refresh (`out/test262-refresh-phase4-c31` vs `out/test262-refresh-phase4-q2`)

Run via `python3 tools/test262/refresh_baseline.py --binary "$PWD/out/jit/bin/ch" --output out/test262-refresh-phase4-c31 --previous out/test262-refresh-phase4-q2` against clean commit `410d4a198` (`engine_tree_dirty: false`, corpus `2e0a5676`, `-nonative -Test262 -CollectGarbage -NegativePhaseReport`, `25/25` controls, `902` skipped):

| Metric | `Q2` (`4508af170`) | `C3.1` (`410d4a198`) | Delta |
|---|---:|---:|---:|
| **Broad (`93,141` scenarios, `48,598` files)** | `81,332 / 11,809` (`87.32%`) | **`82,043 / 11,098` (`88.08%`)** | **`+711` net** (`712` `fail->pass`, `1` `pass->fail`) |
| **ES2025 edition (`80,544` scenarios, `42,203` files)** | `79,948 / 596` (`99.26%`) | **`79,950 / 594` (`99.26%`)** | **`+2` net** |
| `post-ES2025` bucket | `11,213` | **`10,504`** | `-709` |
| `engine-defect-on-existing` bucket | `559` | **`557`** | `-2` |
| `required-feature-missing` / `host` | `21` / `16` | **`21` / `16`** | `0` |
| `explicit-resource-management` (refresh) | - | **`842 / 0`** | `+709` `fail->pass` (+ `133` prior passes) |
| `Error.isError` | `24 / 0` | **`24 / 0`** | `0` |
| Negative-phase audit | `0` suspects | **`8,745` passed (`0` suspects, `1` marker downgrade)** | Clean |

- **Non-C3.1 delta audit:** The `712` `fail->pass` flips comprise `709` `explicit-resource-management` scenarios plus `3` pre-existing `Atomics/waitAsync` parallel-scheduler variance scenarios (`no-spurious-wakeup-on-store.js`, `bigint/no-spurious-wakeup-on-compareExchange.js`, `bigint/no-spurious-wakeup-on-sub.js`). The lone `pass->fail` (`test/built-ins/Atomics/waitAsync/bigint/no-spurious-wakeup-on-exchange.js`, strict mode) passed **`2/2` (`0` failed)** standalone at `--jobs 1` -> **`0` engine regressions**.

### 4.3 Full macOS ARM64 Gate Certification Matrix

| Gate | Command / Profile | Result |
|---|---|---|
| Bytecode-header purity | `mise exec -- zig build verify-bytecode` & `-Dprofile=jit -Dwasm=true verify-bytecode` | **PASS** (exit `0`, headers unchanged) |
| StyleChecks | `STYLECHECK_BASE=1bf42d1f6` & `origin/master` `tools/StyleChecks/check_{eol,tabs,ascii,copyright}.sh` | **PASS** (`0` issues) |
| Build & Linux gate tests | `zig test tools/linux-gate-tests.zig`; `zig build check-selftest`; `zig build check-build-tools` | **PASS** (`9/9` Linux gate tests; selftests PASS; `97` build-tool tests OK) |
| `test` profile gate | `mise exec -- zig build check` | **PASS** (**`2089/0`**: `es7: 120/0`, `Debugger: 19/0`, `DebuggerCommon: 211/0`, `test-shared-basic: PASS`, `check_smoke` PASS, `check_metadata` PASS `18,562` `T` symbols `8/8`) |
| `debug-static` gate | `mise exec -- zig build -Dprofile=debug-static check` | **PASS** (**`2141/0`**: `536` archive members, `62,409` `T` symbols `8/8`, `8/8` static native suites, no dylib, `check_smoke` PASS, `check_metadata` PASS) |
| `release-shared` gate | `mise exec -- zig build -Dprofile=release-shared check` | **PASS** (`17,158` `T` symbols `8/8`, `check_metadata` PASS) |
| `jit -Dwasm=true` gates | `mise exec -- zig build -Dprofile=jit -Dwasm=true check-wasm check-fulljit check` + `dynapogo Optimizer es7` | **PASS** (`check-wasm` P1 **`382/382`** / runtests `448/5`; `check-fulljit` **`624/0`** [`es7: 121/0`]; `check` PASS; `dynapogo Optimizer+es7` **`321/0`** [`Optimizer: 200/0`, `es7: 121/0`]) |
| `GCStress` gate | `mise exec -- zig build check-gcstress` | **PASS** (`in-thread` `0.97s` / `concurrent` `0.73s`, `walks=2` each) |
| `Node-API` gate | `mise exec -- zig build check-nodeapi` | **PASS** (`155` N-API + `6` host = `161` exported symbols, `libChakraNodeApi.1.0.0.dylib` allowlist clean) |
| `ASan` gate + C3.1 `-CollectGarbage` | `mise exec -- zig build -Dsanitize=address check-sanitize` + `./out/sanitize-address/bin/ch -CollectGarbage` (3 C3.1 tests) | **PASS** (`27/27` legs clean + `3/3` C3.1 tests clean) |
| `UBSan` gate + C3.1 `-CollectGarbage` | `mise exec -- zig build -Dsanitize=undefined check-sanitize` + `./out/sanitize-undefined/bin/ch -CollectGarbage` (3 C3.1 tests) | **PASS** (`27/27` legs clean + `3/3` C3.1 tests clean) |
| Packaging gate | `mise exec -- zig build check-package` | **PASS** (`VERIFY PASS`, `0` errors, all package smoke/nodeapi checks PASS) |
