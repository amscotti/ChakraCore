# Memory management for coding assistants

Practical guidance for AI-assisted changes to ChakraCore's C++ engine and future
Zig/C-ABI slices. It applies to human-written patches too. Engine references are
pinned to `97e68c917`; Zig spellings use **0.17.0**. External lessons were checked
against primary sources on 2026-10-11 UTC. Project policy below is our adaptation,
not a claim that TigerBeetle, Ghostty or Bun endorses this guide.

## Start here: five questions before editing

1. **What owns these bytes or this handle?** Identify the allocation domain and
   the sole release authority, or the explicit shared-ownership protocol.
2. **What invalidates this borrow?** Include GC, detach, reallocation, container
   mutation, arena reset, cancellation and runtime disposal.
3. **Can this operation execute user code or run concurrently?** Include getters,
   coercion, finalizers, allocation callbacks and background completion.
4. **What happens at every failure/exit point?** Trace partial initialization,
   foreign status errors, OOM, exceptions, cancellation and shutdown.
5. **Which test observes the promised lifetime?** A successful compile or the
   1964-case regression baseline does not prove the new ownership contract.

If a lifetime cannot be established from declarations, implementations and callers,
record the uncertainty and narrow the change. Do not choose an allocator or add a
root merely because a nearby example did so.

## 1. What the reference projects actually teach

### TigerBeetle / TigerStyle: bound the design before allocating

TigerStyle requires startup allocation and no later dynamic allocation/reallocation
for its database, explicit limits, small scopes, paired invariants and handled
operating errors. It explicitly says assertions/simulation do not replace an
understood model [TB].

**Our adaptation:** apply bounded pools, queue limits and preallocation where the
engine's native control plane has a known workload. Define pool exhaustion and
backpressure behavior. JavaScript heaps and variable scripts require dynamic
allocation; adopting TigerStyle's entire static-allocation policy is not this
project's design. Input limits and OOM need controlled errors; assertions document
internal relationships, not a substitute for validating external requests.

### Ghostty: allocation provenance and logical content are separate facts

Ghostty's January 2026 postmortem describes large direct-`mmap` pages being reused
after metadata was reset to a standard size. The release path consequently treated
them as pooled pages and omitted `munmap`. Existing leak tooling missed the trigger
until a reproduction and allocation-specific VM tags exposed it [GT-LEAK].

The inspected PageList explicitly records `.pool` versus `.heap` ownership and
says it must never be inferred from memory length. It also distinguishes ordinary
pins invalidated by modification from tracked pins maintained by the owner, with
explicit track/untrack operations [GT-CODE].

**Our adaptation:** retain the original allocation descriptor and release mechanism
independently of logical length/views. Make pool ownership explicit. Use stable
handles, generations or an existing tracked-reference mechanism across mutation;
an ordinary pointer does not acquire that protection. Test large/exceptional
allocations entering reuse/prune/teardown paths, not only the normal pool size.

This was a long-standing Ghostty bug exposed by workloads such as Claude Code;
the postmortem does not attribute the bug's creation to AI code [GT-LEAK].

### Bun: manual ownership at a GC/native boundary needs observable contracts

Bun's July 2026 retrospective lists concrete bugs fixed in v1.3.14 [BUN]:

| Reported failure | Required review/test here |
|---|---|
| zlib reset/close while async writes or reentrant callbacks still use native state | Completion ownership; cancel/close waits for the last user |
| HTTP2 callbacks rehash a map and invalidate stream pointers | Stable identity and reacquisition after user code |
| Coercion callbacks detach/resize buffers or change a socket during iteration | Complete coercion, then reacquire/validate storage and state |
| scrypt output allocation fails after callback/password/salt protection | OOM rollback includes roots and foreign resources, not only bytes |
| Watcher reference-count underflow permanently pins a GC root | Balanced root accounting and explicit cycle-breaking close |
| Parser double-free and missing native cleanup | One release authority and partial-construction tests |
| GC marker sees a torn native variant during concurrent access | Publication/trace synchronization and valid tagged state |

Bun reports already using ASAN, fuzzing and leak tests. Its retrospective also
describes a Rust port bug where a `Box` dropped before an asynchronous C close
callback, leaving a use-after-free/double-free [BUN]. These are evidence about
boundary contracts and incomplete test coverage, not proof that a particular
language or a prose style guide guarantees safety.

## 2. Classify ChakraCore memory domains

| Domain | Lifetime and release authority | Assistant rule |
|---|---|---|
| Stack/synchronous scratch | Current frame; often shorter than the whole call chain | Never publish its address to a retained callback/job |
| Native C++ heap / library resources | Allocating subsystem and its paired delete/free/release | Preserve `HeapNew`/`HeapDelete`, allocator-specific RAII and library release APIs |
| Zig native allocation | Explicit allocator instance and owner | Keep instance, allocation extent/alignment and ownership transfer; free through its defining module |
| Arena/pool storage | Arena/job/session or explicit pool item owner | Reset/reuse only after every borrower is finished; bound retained capacity |
| Recycler/JS value | Runtime collector, tracing and deliberate roots | Do not Zig-free, C-free, or replace a Recycler object with an untraced native allocation |
| Borrowed JS buffer/string/container view | Underlying owner plus its invalidation rules | Rooting its owner and pointer stability are separate obligations |
| External JS object/buffer native state | Host allocation plus registered trace/finalize contract | Make callback state, transfer point and teardown order explicit |

`AutoPtr` and `AutoArrayPtr` retain allocator/count information for matching C++
deletion (`lib/Common/Memory/AutoPtr.h:7-61`). A pointer's ABI shape does not
authorize calling `free` in a different allocator, module or Windows CRT.
Prefer creator/destroyer pairs exported by the module that allocated it.

JsRT runtimes own `ThreadContext`/Recycler; TLS is the current binding, not the
owner. Rental-threading permits serial rebinding under its rules, not concurrent
script execution on one runtime (`ThreadContextTlsEntry.cpp:70-162`). An allocator
being thread-safe does not make a container, callback or JS handle thread-safe.

### Rooting and raw storage: two different obligations

- `JsRef` stored off-stack requires `JsAddRef` / `JsRelease`
  (`ChakraCommon.h:362-371,941-973`). The implementation uses Recycler roots
  (`Jsrt.cpp:601-720`); it is not a native pointer's reference-counted destructor.
  A zero returned count does not authorize a native free.
- In this project's C++/Zig boundary policy, **do not rely on a Zig local, slice,
  register or native container making a JS reference visible to conservative
  scanning**. Use an established C++ tracing/rooting path or explicit JsRT rooting
  with checked status and the correct runtime/context. Verify the exact API's
  thread/context requirements; ordinary values require a current context in
  `Jsrt.cpp:628-708`.
- `JsGetArrayBufferStorage` explicitly says the raw buffer pointer is not a GC
  reference (`ChakraCommon.h:2063-2080`). Rooting the ArrayBuffer prevents collection
  while held; it does **not** prevent `JsDetachArrayBuffer` or establish future
  resize/pointer stability (`ChakraCore.h:1959-1995`).
- Runtime disposal invalidates its resources (`ChakraCommon.h:800-812`). Roots
  must be released through the permitted owner context before disposal; they are
  not a license to use handles after teardown.
- For internal GC objects, preserve the actual trace/barrier mechanism. `Field`
  can be a write-barrier type on enabled configurations
  (`WriteBarrierMacros.h:18-39`). Do not replace it with a plain Zig pointer store.
  Keep this operation on the established C++ side until an equivalent contract is
  designed and tested.

## 3. Write the ownership record

For each changed retained allocation, buffer, root or foreign resource, include a
compact record in the code comment/work item/review. It need not be a new framework:

```text
Resource / creation call / source location:
Domain and allocator instance (including backing allocator lifetime):
Physical allocation base, extent, alignment, release callback or pool kind:
Logical length / borrowed views (kept separate from the allocation descriptor):
Owner; transfer/publication point; remaining aliases:
Borrow invalidation: GC / detach / growth / mutation / reset / close / disposal:
Potential user-code or reentrancy points:
Thread/context allowed for use, root changes and destruction:
Release on success, partial failure, exception, OOM, cancel and shutdown:
Memory accounting, capacity/retention bound and exhaustion behavior:
Tests / commands / measured results / remaining uncertainty:
```

Draw the small lifetime graph: producer → owner → borrowers/jobs → completion →
release. Identify a final release path and any cycle. A native object retaining a
rooted callback which captures its external JS wrapper can prevent its own
finalization; give explicit close/unregister a cycle-breaking path.

## 4. Choose allocators from lifetimes

### Synchronous scratch and arenas

Use stack/fixed-buffer scratch when bounded; handle exhaustion. Use an arena when
all allocations truly share a proven operation/job lifetime. Persistent results
are copied to the destination owner or the entire arena is owned by a longer-lived
job; they cannot borrow a function-local arena after return/reset.

- Do not assume parse data is call-local: this engine has deferred/background
  parsing and data that survives entry-point return. Inspect actual users first.
- An allocator value can hold a pointer to its implementation. Moving a struct
  containing an arena while retaining `arena.allocator()` can leave a handle
  pointing to the old arena address. Stabilize allocator state or reacquire the
  allocator after an explicitly safe move; do not return a dangling self-pointer.
- Retained capacity is not a live object. An arena reset invalidates allocations
  even if backing pages remain mapped. A cached borrow is still invalid.
- Repeated jobs need a retention bound and occasional capacity release, not a
  process-lifetime arena quietly growing with every request.

**Zig 0.17 specifics:** `ArenaAllocator.allocator()` stores `.ptr = arena`
(`ArenaAllocator.zig:34-43`). Its allocation interface is thread-safe if the child
allocator is, but `deinit`, `reset` and `queryCapacity` are documented **not
thread-safe** (:1-7,50-68,99-112). A reset mode such as `.retain_with_limit` controls
retention; it is not a substitute for synchronizing borrowers or a universal
hard bound on future allocations [ZIG-SRC].

### Persistent native state and pools

Pass the allocator explicitly. Allocate stable owner state, retain the full slice
for release, and expose a separate borrowed view. Zig slices are pointer/length
values, not ownership proof, and copying an owning struct does not perform a
reference-counted retain or ownership move.

Record provenance through the owner/factory contract, not by comparing an
allocator's erased pointer. `Allocator.ptr` may be `undefined` for stateless
implementations (`mem/Allocator.zig:17-22`); raw pointer comparison is not a valid
allocator-identity protocol.

Zig's allocator contract requires the most recent successful allocation/resize
extent and original alignment for release (`mem/Allocator.zig:76-86`). Save the
updated allocation descriptor after successful resize/remap/realloc. A short
logical subslice is not the allocation to free [ZIG-SRC].

Pools need explicit item kind, live/reserved bytes, generation/state and capacity.
Do not infer pool membership from length. Stable indices/generation handles or an
existing tracked reference make mutation semantics testable; neither a numeric ID
nor a pointer is inherently immune to reuse/ABA.

Choose a production allocator after profiling and contract review. Use leak-
detecting/failing allocators in tests; if libc interop is needed, link it and retain
matching release authority. Global `page_allocator` is not an automatic default
for every new unit and can bypass intended runtime accounting.

## 5. Cleanup and C-ABI publication

### Pair acquisition with cleanup immediately

In synchronous scope, `defer` expresses cleanup on normal/error exit. In a
fallible constructor, `errdefer` rolls back already acquired resources until
ownership is committed. Cleanup must cover native bytes, handles, roots and
registered callbacks. Do not call a full destructor on partly initialized state
unless it is explicitly designed for that state.

Zig `errdefer` runs when leaving with a Zig error. A foreign `JsErrorCode` is an
ordinary value; returning it directly does **not** invoke `errdefer`. Convert it
into the internal error path or perform explicit rollback before returning the
foreign status. Preserve the engine's exception/OOM policy instead of hiding an
OOM behind `catch unreachable` or silently changing a fatal/recoverable contract.

For a new native C-ABI constructor, define invalid outputs at entry, publish a
valid owned handle only on success, and provide its paired module destroy function.
Document `size_t`/alignment/calling convention, thread affinity and status codes.
No C++ exceptions, Zig errors-as-values, STL objects or undocumented slices cross
the ABI. A `(pointer, length)` alone does not state ownership.

### Checked native-owner example

[`examples/owned-bytes.zig`](examples/owned-bytes.zig) uses two fallible allocations:
stable owner state and an owned byte copy. Each acquisition gets rollback; the
returned handle owns both, and `destroy` frees through the captured allocator.
`view` is borrowed until destroy. This is a **native-only teaching example**, not
a Recycler object or a complete JsRT bridge. The allocator state must outlive it.

```sh
mise exec -- zig test doc/examples/owned-bytes.zig
```

The tests cover success, copy independence, repeated create/destroy, foreign-status
rejection before publication, and every
deterministic allocation-failure point using
`std.testing.checkAllAllocationFailures`. The helper covers allocations routed
through its injected allocator; it does not inject a C library's or Recycler's
OOM. Those require their own controlled failure paths.

### External JS objects/buffers and finalizers

`JsCreateExternalArrayBuffer` registers external storage/finalizer state, rather
than copying it into a Zig-owned buffer (`ChakraCommon.h:1960-1978`,
`Jsrt.cpp:2082-2107`). Read the exact callee/error/teardown contract before choosing
the transfer point. Traced external objects also expose a trace callback
(`ChakraCore.h:1128-1174`); native-held GC references need an established trace/root
path, not an assumption that external data is scanned.

Detachment has its own release path: `JsrtExternalArrayBuffer::Finalize` skips
the callback for an already detached buffer, and detached-state discard/create
can carry that callback/state onward (`JsrtExternalArrayBuffer.cpp:32-73`).
Trace this transfer when reviewing externalization/serialization; assuming the
original wrapper's finalizer always frees the data can leak or double-release it.

Requirements for a finalizer-owned state:

- state and backing allocator outlive every callback, including failed setup and
  runtime shutdown paths where applicable;
- explicit close/unregister and finalization agree on one release authority;
  counters/flags alone do not synchronize a concurrent user;
- shutdown stops new work, resolves/cancels jobs, releases roots in the permitted
  context, finalizes/disposes the runtime, then destroys dependent allocators;
- finalizers use only permitted APIs, threads and state. Inspect the implementation
  rather than assuming they are normal JavaScript callbacks; do not reenter JS or
  call unrelated JsRT APIs from cleanup without a documented allowance;
- do not keep a JS wrapper rooted solely so its finalizer can release that same root.

## 6. Treat reentrancy as an invalidation boundary

Getters, proxy traps, `valueOf`, `toString`, conversion and callbacks may mutate
containers, close native objects, trigger GC or detach buffers. Inspect the actual
API/operation; a name such as "get" or "convert" does not imply a pure read.

Conceptual sequence (pseudocode, not a universal implementation):

```text
1. Retain the native owner and establish required JS roots.
2. Perform user-visible coercion/callbacks; check exceptions/status immediately.
3. Re-find the object by validated stable identity/generation.
4. Re-check closed/state, buffer detachment, lengths, bounds and capacity.
5. Reacquire storage/container views after the last reentrant operation.
6. Use them in a region proven not to invoke user code or mutate that storage.
7. Commit/release through the owner's contract.
```

Never cache a hashmap entry pointer across a call that can rehash it. Never use a
pre-coercion ArrayBuffer pointer after a callback can detach it. Do not retain an
iterator while invoking arbitrary JS which can change the underlying structure.
If the algorithm cannot establish a non-reentrant borrow window, use a native
owned snapshot or an established stable-storage protocol. Copying from invalid
storage after the callback is not a fix.

Bounds calculations need checked conversions/arithmetic: sizes, offsets and
capacity are separate values. Check an offset before computing remaining length;
check byte-size multiplication and narrowing to JsRT's unsigned lengths. External
input failures return controlled errors, not a new process-aborting invariant.

## 7. Async jobs, cancellation and concurrent GC

A `defer` tied to the submitting function cannot free memory which the worker/C
library still retains. Passing a raw pointer is not a retain. Define the state
machine and its release authority:

```text
constructed → queued → running → completion posted → owner-context cleanup
                         ↘ cancel requested → worker acknowledgement / drained completion
```

Cancellation/close prevents new work but is not proof that the worker stopped
touching memory. Account for enqueue failure, cancel-before-run, cancel-while-run,
reentrant close during completion, repeated close, and shutdown with queued work.
One terminal cleanup path owns resources; each in-flight user holds the required
ownership until its last access. Reference counting still needs synchronization,
overflow/underflow checks and a cycle policy.

Workers normally consume owned native bytes. JS handles remain rooted under the
runtime's contract and are accessed/released on an allowed bound context; having
a pointer in a worker queue does not grant a second runtime execution thread.
`JsThreadServiceCallback` supplies background work, not blanket permission to use
arbitrary JS values (`ChakraCommon.h:704-725`).

For collector-visible native variants, publication, tag and payload must remain
consistent for the tracing observer. A union tag, an atomic refcount or a thread-
safe allocator alone does not publish the whole object safely. Preserve the
existing barrier/trace/synchronization mechanism. Do not hold a non-reentrant
native lock while calling arbitrary JS and then assume a recursive callback is safe.

## 8. Verification: test the model and the instrumentation

Select tests based on the changed lifetime:

| Change | Minimum targeted evidence |
|---|---|
| New fallible native owner | Leak-detecting allocator + each allocation failure + release on success |
| Borrow/mutable container | Growth/rehash/erase during reentrancy; stable-ID reuse/generation checks |
| JS buffer use | GC while only the intended root remains; detach in coercion/callback; zero/large/offset bounds |
| External state/finalizer | Failed registration, explicit close, GC finalization, repeated close, runtime disposal |
| Arena/pool reuse | Reset only after borrowers drain; exceptional-size allocation followed by reuse/prune/destroy |
| Async ownership | Controlled completion/cancel schedules and shutdown with in-flight users |
| GC-visible field/publication | Tracing/barrier and allowed-thread tests, including relevant concurrent configurations |

Use deterministic callbacks/schedulers and reproduce the invalidating transition.
Do not construct a test that dereferences known freed memory and calls a crash a
proof of correctness. Memory-instrumentation negative controls belong in isolated
test processes; require a diagnostic and failure status from the instrumentation.

**Zig 0.17 tool choices:** `std.testing.allocator` uses `SafeAllocator` in the
installed source. `std.heap.DebugAllocator` is deprecated in favor of
`SafeAllocator`. `checkAllAllocationFailures` needs deterministic allocations and
test state reset between runs (`std/testing.zig:1070-1191`). These checks cover
their domain, not every C++/mmap/Recycler allocation [ZIG-SRC].

For ASAN/other sanitizers, capture actual compile **and link** flags, runtime and
target; prove the configuration with an isolated known-invalid access. The
legacy sanitizer wiring is suspect (`CMakeLists.txt:48-51,610-613`), and Bun's
patched-compiler ASAN claim does not prove stock Zig 0.17 instrumentation works
here. A Debug/ReleaseSafe build is not an ASAN build. Do not report sanitizer
coverage from the option name or a green uninstrumented suite.

For long-running leaks, distinguish live allocations, roots, reserved/retained
pool or arena memory, mapped bytes and RSS. Record warm-up/steady-state and an
allocation domain/tag; Ghostty's VM tags made its mmap owner observable [GT-LEAK].
Track new Zig native memory separately unless it is deliberately integrated with
the engine's allocation policy: runtime usage/limits query that policy manager
(`Jsrt.cpp:3252-3275`), not arbitrary process allocations. Repeat the workload and
destroy its owner; explain retained capacity rather than labeling every plateau
a leak or every flat RSS line proof of balanced ownership.

Keep applicable bytecode, regression, native API and conformance checks alongside
focused tests ([build reference](build.md), [roadmap](modernization-plan.md)).
An ownership-focused patch must explain its model and provide observable evidence;
a large passing suite or an AI review is not a substitute for those proof obligations.

## 9. Assistant implementation and review receipt

**Before:** read the actual owner/destructor, callers, callback/tracing path and
the nearby allocator convention. Write the ownership record and identify the
invalidating transitions. Choose a small C-ABI/leaf change consistent with Z1.

**While editing:** pair acquisitions with cleanup, keep owners/views distinct,
check every foreign result and leave a valid unpublished failure state. Preserve
C++ ownership/barriers and thread requirements until the selected slice's contract
is proved. Avoid unrelated refactors which hide lifetime differences.

**Review:** trace success and each error, every potential call to user code, and
each async terminal path from first acquisition to final release. Re-check that a
root has a release and that its release is reachable even if the operation fails.
Inspect the physical allocation descriptor, not only the view. Challenge any claim
that an allocator/type change by itself fixed the bug.

**Report:** changed domains/transfer points, exact focused test commands and
results, relevant toolchain/instrumentation, accounting observations and explicit
untested cases. If only native-owner unit tests ran, say so; do not imply Recycler,
JIT, concurrency or whole-process sanitizer coverage.

## Sources and source-verification map

- **[TB]** [TigerStyle, inspected commit](https://github.com/tigerbeetle/tigerbeetle/blob/c95d7a53a3d044741409b03b2f334a8ed033fa3c/docs/TIGER_STYLE.md):
  startup allocation, bounds, handled errors, paired assertions and model-first review.
- **[GT-LEAK]** Mitchell Hashimoto,
  [Finding and Fixing Ghostty's Largest Memory Leak](https://mitchellh.com/writing/ghostty-memory-leak-fix)
  (2026-01-10), [fix PR #10251](https://github.com/ghostty-org/ghostty/pull/10251).
- **[GT-CODE]** [Ghostty PageList, inspected commit](https://github.com/ghostty-org/ghostty/blob/e3ac62001ef2a6c9c4b4bb4231535c1c3c3e4ae3/src/terminal/PageList.zig):
  explicit backing ownership (:65-93), release/accounting (:5095-5129),
  tracked pin acquisition/release (:5711-5745), invalidation/garbage state (:7191-7211).
- **[BUN]** Jarred Sumner, [Rewriting Bun in Rust](https://bun.com/blog/bun-in-rust)
  (2026-07-08): reported v1.3.14 bugs, existing instrumentation and
  GC/native cleanup/async-close ownership lessons. Conclusions here are our
  application of the reported incidents, not an endorsement of that rewrite.
- **[ZIG]** [Zig 0.17.0 language reference](https://ziglang.org/documentation/0.17.0/):
  Memory / Lifetime and Ownership / Choosing an Allocator / defer and errdefer.
- **[ZIG-SRC]** Installed 0.17.0 `lib/std` checked directly:
  `heap.zig:12-26`, `heap/ArenaAllocator.zig:1-112`,
  `mem/Allocator.zig:17-86,199-205,408-496`, `testing.zig:20-21,1070-1191`,
  and `heap/SafeAllocator.zig:1-29`. Exact older-version tutorials are not API proof.
- **ChakraCore baseline:** `lib/Jsrt/ChakraCommon.h`, `ChakraCore.h`, `Jsrt.cpp`,
  `lib/Common/Memory/AutoPtr.h`, `WriteBarrierMacros.h`,
  `lib/Jsrt/JsrtExternalArrayBuffer.cpp`,
  `lib/Runtime/Base/ThreadContextTlsEntry.cpp` and
  [the architecture ownership model](architecture.md#6-threading-and-hosting).

The policy is deliberately layered: explicit ownership and bounded resources,
source-verified GC/FFI contracts, focused failure/reentrancy/concurrency tests,
qualified instrumentation and comparable baseline checks. No single layer proves
the absence of memory bugs.
