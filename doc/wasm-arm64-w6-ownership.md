# W6 ownership record — ARM64 wasm frames, reserve floor, walker skip

Scope: `doc/wasm-arm64-plan.md` W6 DoD ("no new `Recycler`-visible
raw-pointer shape without an ownership record") and the W6 reference in
`doc/wasm-arm64-abi.md` §9 ("explicitly rooted in the W6 ownership
record"). Covers the ARM64 wasm entry shapes at HEAD (`025e24851`):
slot-image alloca + 64KB reserve, `WasmStackReserveFloorGuard`, return
boxing, and the stack-walker skip rule. Template per
`doc/memory-management.md` §3.

## R1 — Slot-image alloca + 64KB JIT-frame reserve (stack, untraced)

Resource / creation call / source location:
`_alloca(unboxSize + 16 + 65536)` in `AsmJsExternalEntryPoint`
(`lib/Runtime/Language/AsmJsUtils.cpp:664-666`); parallel site
`_alloca(byteSize + 3*sizeof(Var) + 16 + 65536)` in
`JavascriptFunction::CallAsmJsFunction` (`lib/Runtime/Library/JavascriptFunction.cpp:1629-1633`).
Domain and allocator instance: native stack memory of the entering
thread; no `Recycler` allocation, no backing allocator lifetime.
Physical base/extent/alignment/release: `raw` (post-alloca stack
bottom) .. `raw + unboxSize + 16 + 65536`, 16B-aligned `base = raw +
65536`. Released by stack unwind on return; nothing to free.
Logical length / views: `[func][0][0][unboxed args...]` slot image at
`base` (wasm param k at slot 3+k); the 64KB below `base` is free
reserve for the JIT callee frame (`AsmJsJitCall` sets sp = base,
prologue sub's into the reserve).
Owner; transfer/publication: the entering C++ frame owns it; `base`
is passed by address to `AsmJsJitCall` (borrow, not ownership).
Aliases: `*(ScriptFunction**)base = func` is a **raw, untraced**
reference. It is safe because `func` is rooted by the caller's own
`argv`/frame for the duration of the call; the slot image itself is
never scanned as `Var` roots (walker skip, R4).
Borrow invalidation (GC / detach / growth / ...): a GC during
`AsmJsJitCall` (via import call or allocation) does not move stack
memory; unboxed arg slots are never traced as pointers (proven by
`test/wasm/gc_lookalike.js`, R5). `memory.grow` replaces the wasm
`ArrayBuffer`, never the slot image. No detach/close path applies.
User-code/reentrancy: wasm→JS import calls re-enter with their own
`JavascriptCallStackLayout` frames; re-entrant wasm entry creates a
new nested alloca + guard (LIFO, R2).
Thread/context: entering thread only; never shared cross-thread
(threads/W9 keeps shared memories, not slot images, behind flags).
Release on all exits: automatic (stack); C++ unwind through the region
is normal and expected (guard dtor runs, R2).
Accounting/bound: 64KB + unboxSize per live external entry, bounded
by call depth; depth itself is bounded by the reserve-floor probe (R3).
Tests: `gc_lookalike` (pointer-shaped i32 live across forced GC),
`gc_churn` (48 rounds instantiate/call/grow under GC), 68/68 green
across tiers × flags (see Tests below).

## R2 — `WasmStackReserveFloorGuard` + `wasmStackReserveFloor` (plain integer, no GC)

Resource: `ThreadContext::wasmStackReserveFloor` (`uintptr_t`,
`lib/Runtime/Base/ThreadContext.h:443-444`); set/restore by RAII
`WasmStackReserveFloorGuard` (`ThreadContext.h:1514-1529`) at both
entry sites (`AsmJsUtils.cpp:679-682`,
`JavascriptFunction.cpp:1638-1641`).
Domain: per-`ThreadContext` scalar; not a pointer, never traced,
never a root. Zero disables the probe (default state: floor 0).
Owner/transfer: the guard instance owns the mutation; previous value
saved at construction, restored on destruction including C++ unwind
paths. Nesting is LIFO for re-entrant wasm→JS→wasm.
Invalidation: none GC-related; `ServerThreadContext` mirrors the
field for the JIT-server path (`ServerThreadContext.cpp/h`).
Reentrancy: each nested entry pushes its own floor; restore order is
destructor order.
Release: automatic; no OOM/cancel path (no allocation).
Tests: fac-450 terminates ~0.05s (was infinite walker cycle);
fac1/fac200, f32/f64 2501/2501 unaffected.

## R3 — Reserve-floor probe (noreturn slow path, no allocation)

Resource: `ThreadContext::ProbeWasmReserveFloor`
(`lib/Runtime/Base/ThreadContext.cpp:1877-1896`), called from the
inline `GenerateWasmReserveFloorCheck` after the ARM64 prolog
(`lib/Backend/arm64/LowerMD.cpp`). Throws `StackOverflow` when
`sp < floor + frameSize`; no allocation, no rooting, no state change.
Slow path only; fast path is a register compare in FDE-covered code.

## R4 — Walker/GC treatment of wasm frames (skip, never decode)

Rule: `CheckJavascriptFrame` returns false for asm.js frames
(`funcObj->GetFunctionBody()->GetIsAsmjsMode()`,
`lib/Runtime/Language/JavascriptStackWalker.cpp:1017-1020`) before
any `CallInfo` decode — wasm frames are skipped, never decoded as JS.
Consequence: unboxed i32/i64/f64 slots are never traced as pointers;
wasm frames contribute no `Var` roots. Every JS reference reachable
from wasm execution is rooted elsewhere: the external thunk's
`argv`/frame, import-call JS frames, or explicit rooting at the
`WebAssembly` API boundary during instantiation. The planned
per-function extent/arg side table (`wasm-arm64-abi.md` §9/Q5) is not
needed for the skip itself and remains future work; the skip rule is
closed and tested.

## Tests / commands / measured results

- `test/wasm/gc_lookalike.js` + `.baseline` (`pass:true`),
  `test/wasm/rlexe.xml:498-505`, flags `-wasm -CollectGarbage`,
  `require_backend`. Asserts exit 0 with a pointer-shaped i32
  (`0x7F001234`) and f64 NaN live across a forced full collection
  inside an import, value integrity after GC, and 8 `WeakRef`
  victims reading `undefined` after job-boundary + clobber + double
  GC. Exact-address limitation documented in the probe header.
- `test/wasm/gc_churn.js` + `.baseline` (`pass:true`),
  `test/wasm/rlexe.xml:485-492`. 48 rounds instantiate/call/
  `memory.grow`/export-read plus deterministic JS graph churn.
- Results on `out/jit/bin/ch` at HEAD: 40/40 (2 probes ×
  interpreter/JIT × 10 runs) + 28/28 flag subsets
  (`-GCMemoryThreshold:1`, `-ConcurrentRuntime
  -ForceBlockingConcurrentCollect`,
  `-RecyclerProtectPagesOnRescan`, JIT-tier `-forceNative
  -off:simpleJit -bgJitDelay:0`). Total 68/68 green.
- Remaining uncertainty: exact-address slot-equality is unprovable
  from JS (no address-disclosure API); the DoD as written
  (exit-0 + cleared `WeakRef`) is fully proven. A native harness
  with address observability would be needed for the stronger claim.
- Separate carded issue (not covered here): C++ unwinding through
  ~240+ consecutive JIT frames fails inside Apple system libunwind
  phase-1; see open-cards register. Shallow delivery and all probes
  above are unaffected.
