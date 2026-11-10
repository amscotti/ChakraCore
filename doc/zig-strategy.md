# Zig integration strategy — incremental, C-ABI wedges only

Status: **recorded strategy (2026-11-09)**, following the B4 Linux x64
completion and the five-lens swarm review. This doc records *whether,
where and how* Zig should replace parts of the codebase over time. It is
deliberately conservative: it rejects a whole-engine conversion and
recommends a background, parity-gated wedge program outside the engine
core. Read together with [modernization-plan.md](modernization-plan.md)
§3 (Phase 2 / Z1) and [memory-management.md](memory-management.md).

## 1. The question this answers

Zig is already the build system and C/C++ compiler for every profile.
The open question is whether to *replace* C/C++ source with Zig source,
motivated by two goals:

1. **Memory safety** — avoid the memory defects the project keeps
   finding;
2. **Maintainability** — keep the codebase healthy over years.

## 2. Validated facts (measured on `modernize/baseline`, 2026-11-09)

| Fact | Measurement | How |
|---|---|---|
| Engine size | **~958K lines** of C/C++/headers across `lib bin pal` | `find … \| xargs wc -l` |
| Of which **C** | **30 files / 2,388 lines (~0.25%)** | `find -name '*.c'` |
| C++ translation units | **695** `.cpp` files | `find -name '*.cpp'` |
| JIT backend | **238,511 lines** (`lib/Backend`) | — |
| PAL | **104,796 lines** (`pal/`), 350 `PALAPI` declarations | `pal/inc/pal.h` |
| `ch` host | **8,368 lines** (`bin/ch`) | — |
| C++ exception sites | **646** `throw` statements in `lib/` | `grep -rn "throw "` |
| GC write-barrier machinery | `WriteBarrierPtr<T>` / `TypeWriteBarrierPolicy` — barriers inserted **by the type system** | `lib/Common/Memory/RecyclerPointers.h` |
| Engine C ABI | **237 exported symbols** (`tools/build/linux_exports_so.txt`), `extern "C"` JsRT headers | — |
| N-API surface | **none in-tree** | `grep -rn "napi_"` |
| Toolchain | Zig **0.17.0** pinned (`mise.toml`); C++ TUs compiled via `zig c++` | `build.zig` |
| `zig translate-c` | **C only** — a `.cpp` input fails with `unknown type name 'class'` (verified on the pinned 0.17.0) | empirical |

Upstream Zig posture (verified via docs/discussions, not assumptions):
`translate-c` is a **bootstrap aid for C definitions**, not a migration
tool; its output is explicitly not maintainable Zig, and the
translate-c/`@cImport` pipeline is being reworked. **No production
C++→Zig translator exists.**

## 3. Decision

**Do not convert or translate the engine core to Zig.** Convert, over
time and opportunistically, a small set of **C-ABI boundary
components** outside the engine core. This is the plan's Z1 philosophy
("one useful C ABI slice"), applied as a standing program.

Why the core is excluded — technical, not stylistic:

- **Exceptions are engine mechanics, not error style.** 646 throw sites
  drive parser early errors, OOM, JIT aborts (`OperationAbortedException`)
  and `TryCatch`/`TryFinally`. Zig has no exceptions and no unwind
  through FFI; the B4 Linux work just had to repair the CIE/FDE unwinder
  itself.
- **The GC is the memory model.** `Field(T)`/`WriteBarrierPtr<T>` make
  the compiler insert write barriers on assignment; the recycler scans
  C++ frames precisely (stack walker, debugger, TTD). Changing the
  implementation language changes frame layout and loses the
  "cannot forget the barrier" property — it *increases* memory risk.
- **Zig cannot import C++ headers** (`@cImport` is C-only). Any port of
  a C++-coupled component needs a hand-written `extern "C"` shim; for
  the core, the shim would dominate and the object graph (Var,
  ScriptContext, IR::Opnd/Instr, type handlers) would have to be
  redesigned, not translated.
- **Size**: the core is ~800K lines of coupled C++ (backend 238K,
  runtime/parser/GC the rest). A port is a multi-year rewrite, and
  "half-ported" is the least maintainable state available.

## 4. The wedge ladder (in order)

A wedge qualifies only if **all** hold: its interface is already a C
ABI (or wraps trivially); it crosses no C++ exceptions; it holds no
GC-visible objects; it is independently testable; it is bounded
(≲10K lines per step); and it is performance-benchmarkable.

| # | Wedge | Why it qualifies | Notes |
|---|---|---|---|
| 1 | **N-API / Node-API layer** (new code) | Pure C ABI on both sides; nothing to port; serves the "embeddable / Node-like" requirement directly | The one integration investment that matters; see §6 |
| 2 | **`ch` host, in slices** | Speaks JsRT + PAL only (8.4K lines); no GC/exception coupling on the engine side | Start with arg parsing / file+stream helpers / serialization glue; the module loader next; agent/message-queue plumbing last (the most delicate — see E12/E13 receipts) |
| 3 | **PAL, subsystem by subsystem** | 105K lines but modular and C-shaped; self-contained platform code; where platform bugs cluster | Virtual memory first (the MAP_JIT permission state machine is self-contained and B4-hardened), then file I/O, threads/synch, time, environment. The long-term prize |
| 4 | **Opportunistic small fry** | C ABI, tiny | `bin/NativeTests`, GCStress (repair-by-replacement is acceptable — its startup SIGSEGV is carded), new diagnostics/tools |

**Never**: parser, GC/recycler, interpreter, JIT backend, debugger/TTD,
runtime object model.

## 5. Preconditions for wedge #1

1. **Parity harness per wedge** — the replacement runs the same
   behavioral tests as the original; during transition both
   implementations are buildable behind one flag.
2. **Delete-the-old discipline** — the change that lands a wedge deletes
   the C++ copy it replaces (or a committed removal date). "Both
   forever" is how porting programs rot.
3. **Boundary contract doc** — per wedge: C header, ownership/lifetime
   rules (per [memory-management.md](memory-management.md)), allocator
   contract, failure behavior; benchmark result if hot.
4. **Zig version policy** — Zig is pre-1.0 and pinned at 0.17.0; every
   wedge carries a recurring upgrade cost. Do not port code larger than
   the team can keep current through Zig releases.
5. **Background cadence** — one wedge per cycle; never displacing engine
   work (conformance cards, flag decisions, N-API).

## 6. The integration axis (why N-API is first)

Language is invisible to embedders: the JsRT C ABI (237 exports, now
pinned and tested natively on Mac ARM64 and Linux x64) is the contract.
The historical Node integration (node-chakracore) implemented the V8 API
shim — a large separate project. The modern, bounded path is
**N-API**: one C-level implementation gives Node compatibility without
touching V8 internals, and it is fresh code that can be written in Zig
from day one with no porting risk.

## 7. Decision point

After **two or three wedges** (N-API, one host slice, one PAL
subsystem), make an explicit go/no-go call on measured facts:
parity-test burden, per-wedge Zig-upgrade cost, and whether the ported
wedges are actually easier to change. If yes — continue at the same
cadence. If no — stop with what exists; every wedge is reversible and
the engine is untouched. Either outcome is acceptable by construction.

## 8. Consequences of the two goals (recorded honestly)

- **Memory safety** is bought by the discipline and tooling, not the
  language: [memory-management.md](memory-management.md) ownership
  records in every lane, the implement → dual-review → repair workflow,
  gate pins, plus the recommended tooling campaigns (ASan/UBSan runs,
  GCStress repair, recycler verification). C++'s implicit-behavior class
  (e.g. the E1 write-barrier stack temporary) is real but narrow.
- **Maintainability** is bought by the receipts/register/restart-point/
  gate system this project now has, and by keeping the engine's tested
  core intact. A conversion would reset that asset's clock for years.
- **When the engine does need new low-level code**, write it in Zig at a
  C-ABI boundary (as N-API and the host slices will demonstrate) rather
  than reaching into C++ internals.

## 9. Validation record

Facts in §2 were re-measured on 2026-11-09 at `4779e73bc`/local HEAD;
the `translate-c` C-only behavior was verified empirically on the pinned
Zig 0.17.0 (`class` input → translation failure); the N-API absence and
237-export count were verified against the tree. Upstream
translate-c/`@cImport` direction was checked against Zig documentation
and community/issue discussion current as of this date. Re-validate the
tooling facts when the Zig pin moves.
