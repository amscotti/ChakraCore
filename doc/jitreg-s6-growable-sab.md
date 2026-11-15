# JIT-REG card: s6-growable-nonshared.js — stale auto-length TypedArray/DataView after SAB grow under FullJIT+PGO

Status: FIXED by the S6 follow-up lane (this card). Was NOT a post-e14
regression — bisected to pre-existing (present at e14-green tree 69281b6e4 AND
pre-b5 tree 6c1470f9d with identical failing lines).
Root cause (code-confirmed): `SharedArrayBuffer::Grow` did not refresh live
views' cached effective-length field. Fixed as designed below (SAB parent
tracking on the shared base + refresh walks in both growth paths).

## Failing tests (3 subtests, ONE mechanism)

`test/es7/s6-growable-nonshared.js`, compile flags `-ESSharedArrayBuffer`:

- test #6 "TypedArray and DataView views track growable SharedArrayBuffer length"
  — first throw at line 210 (`assert.areEqual(3, auto32.length)` post-`grow(12)`,
  actual stale `1`).
- test #12 "growable buffer virtual-length invariant and getter-only setters" —
  first throw at line 518 (`assert.areEqual(131072, autoBig.length)` post-grow).
- test #13 "Atomics on growable SharedArrayBuffer views" — first throw at line 550
  (`assert.areEqual(4, auto.length)` post-`grow(16)`, actual stale `1`).
  (Atomics are incidental; the failing assert is the same length staleness.)

## Repro (deterministic, PGO path only)

Two-step recipe — profiles are path-keyed, run from `test/es7/`:

```
CH=out/jit/bin/ch
$CH -ESSharedArrayBuffer -maxInterpretCount:1 -maxSimpleJitRunCount:1 -bgjit- \
  -dynamicprofilecache:/tmp/jitreg/p_s6 s6-growable-nonshared.js -args summary -endargs  # pass
$CH -ESSharedArrayBuffer -forceNative -off:simpleJit -bgJitDelay:0 \
  -dynamicprofileinput:/tmp/jitreg/p_s6 s6-growable-nonshared.js -args summary -endargs   # 10/13 fail
```

Minimal repro (`/tmp/jitreg/min-s6.js`, kept out of tree):

```js
var buffer = new SharedArrayBuffer(4, { maxByteLength: 12 });
var auto32 = new Int32Array(buffer);
if (auto32.length !== 1) { print("FAIL pre: " + auto32.length); throw new Error("pre"); }
buffer.grow(12);
if (auto32.length !== 3) { print("FAIL post: expected 3 got " + auto32.length); throw new Error("post"); }
print("pass");
```

- With `-dynamicprofileinput`: `FAIL post: expected 3 got 1` (deterministic:
  3/3 fresh profiles, 6/6 same-profile repeats).
- Without profile input (`-forceNative -off:simpleJit -bgJitDelay:0` alone): pass.
- Interpreted: pass.
- Removing the pre-grow length read still fails (`FAIL: 1`) — NOT same-function
  CSE-forwarding of the earlier load; the post-grow load itself reads stale state.

## Evidence it predates e14

- Fails identically (same 3 lines) on worktree binaries built at 69281b6e4
  (e14 receipt tree, "es7 108/0") and at 6c1470f9d (pre-b5, before the e12
  aux-slot corruption of 9b9ef5d50).
- Test file unchanged since e14 (only `rlexe.xml` gained an unrelated entry in A2).
- The e14 dynapogo leg ran `runtests.py --variants dynapogo` WITHOUT an
  interpreted leg, so no `profile.dpl` existed and the PGO path was never
  exercised — the 108/0 green is vacuous for PGO-only miscompiles. Canonical
  `runtests.py --variants interpreted dynapogo` on these files fails 0/3 at HEAD.

## Narrowed suspect (root cause, code-confirmed)

- `SharedArrayBuffer::Grow` (`lib/Runtime/Library/SharedArrayBuffer.cpp:373-387`)
  bumps `sharedContents->bufferLength` only. Contrast `ArrayBuffer::Resize`
  (`lib/Runtime/Library/ArrayBuffer.cpp:642-660`), which walks
  `primaryParent`/`otherParents` calling `TypedArrayBase::RefreshEffectiveLength()`
  (`this->length = GetLength()`, `lib/Runtime/Library/TypedArray.h:206`).
- The invariant is documented at `TypedArray.h:278-282`: the ArrayObject length
  field holds the *effective* length precisely so "direct field readers —
  notably JIT inline-cache length loads and bounds checks, which read the field
  without calling GetLength — observe current values".
- `ArrayBufferBase::AddParent` is a no-op (`ArrayBuffer.h:99`); only `ArrayBuffer`
  overrides it (`ArrayBuffer.h:260`, `ArrayBuffer.cpp:318`). SharedArrayBuffer
  keeps no parent list, so neither shared growth path can enumerate views today.
- Differential proof: the RAB mirror mini (auto-length `Int32Array` +
  `rab.resize(12)` + length read) PASSES under the identical PGO recipe, while
  the SAB version FAILS. Same JIT, same shape — the only delta is Resize's
  refresh walk vs Grow's missing walk.

## Sibling input (A10 buffers review) — confirmed, carded

- `WebAssemblySharedArrayBuffer::GrowMemory` (`SharedArrayBuffer.cpp:902-943`)
  has the IDENTICAL omission (ZeroMemory + `bufferLength` bump at :940 +
  `ValidateBuffer`, no view walk). Code fact verified at HEAD.
- It needs the same treatment (parent tracking first — `AddParent` is a no-op
  for SAB today). Observability NOT verified here (jit-profile `ch` reports
  WebAssembly unsupported, so no WASM+JIT confirmation possible on this host).
- Recommendation: same follow-up lane fixes both; do not split.

## Fix (S6 lane) — LANDED, uncommitted in lane worktree

Design (per `doc/memory-management.md` §2/§3/§7; ownership record lives in
`ArrayBuffer.h` on the hoisted fields):

- Parent tracking could NOT live on `SharedContents`: it is `HeapNew` native
  memory shared across runtimes, while parent entries are per-runtime
  `RecyclerWeakReference` handles. So tracking is per-wrapper, on the
  `SharedArrayBuffer` wrapper's own Recycler — the same domain/owner as the
  existing `ArrayBuffer` mechanism (guide §2: Recycler/JS value domain).
- No parallel rooting scheme: the fields (`primaryParent`,
  `OtherParents : SList<RecyclerWeakReference<…>*>`, `ParentsCleanupThreshold`),
  `AddParent`, and the refresh walk were hoisted verbatim from `ArrayBuffer`
  into `ArrayBufferBase` (`ArrayBuffer.h`, `ArrayBuffer.cpp`). `ArrayBuffer`
  behavior is a pure move (same code, same GC tracing/barriers via `Field`).
  Views already register through `ArrayBufferParent`'s ctor — the SAB no-op
  `AddParent` was the only blocker.
- `SharedArrayBuffer::Grow` and `WebAssemblySharedArrayBuffer::GrowMemory`
  (same omission — the WASM path does NOT differ, fixed identically) call the
  shared `ArrayBufferBase::RefreshParentsEffectiveLength()` after bumping
  `bufferLength`. DataView needs no refresh (getters compute live).
- Reentrancy/threading: the walk calls only `RefreshEffectiveLength` (pure
  field store, no user code, no allocation) on the owning runtime's script
  thread — same contract as `ArrayBuffer::Resize`.
- Known limitation: a second wrapper over the same `SharedContents` in
  ANOTHER runtime keeps its own list; cross-runtime growth does not refresh
  views there (touching another runtime's GC objects from the growing thread
  is not permitted). Single-runtime s6 tests are fully covered.

Files: `lib/Runtime/Library/ArrayBuffer.h`, `ArrayBuffer.cpp`
(`ArrayBufferBase::AddParent` + `RefreshParentsEffectiveLength`, hoist),
`lib/Runtime/Library/SharedArrayBuffer.cpp` (`Grow`, `GrowMemory` refresh
calls). No test files added: `test/es7/s6-growable-nonshared.js` IS the
regression (now 13/13).

Verification (ARM64 macOS, `zig build -Dprofile=jit` → `out/jit/bin/ch`,
base 9d12aebf3; all under the shared gate lock):

- Pre-fix repro on this host: PGO recipe fails exactly as carded (tests #6
  @line 210, #12 @line 518, #13 @line 550; 10/13).
- Post-fix: min-repro `pass`; full file PGO 13/13
  (`total executed: 13; passed: 13; failed: 0`); interpreted leg `pass`.
- Hoist regressions: RAB min-repro + `resizable-arraybuffer.js` +
  `resizable-typedarray.js` under PGO all `pass`; interpreted green:
  `sharedarraybuffer.js`, `s6-bigint-atomics.js`, `atomics_test.js`,
  `immutable-arraybuffer.js`, `typedarray/detached-buffer-semantics.js`,
  `typedarray/dataview{,1,2}.js` (2 baselines MATCH, dataview2 8/8),
  `typedarray/arraybufferType.js` (baseline MATCH),
  `typedarray/arraybuffer.js` (content-identical modulo CRLF runner artifact).
- GC-stress spot (`-CollectGarbage`, 50 buffers × views + DataViews, 8 grow/
  collect/verify rounds, view-drop + regrow exercising nulled weak handles):
  `pass` interpreted AND under PGO.
- Build: both edited TUs compile clean under the exact jit flags (no errors,
  no file-specific warnings); `RefreshParentsEffectiveLength` confirmed in
  the shipped dylib.

NOT verified (explicit): `WebAssemblySharedArrayBuffer::GrowMemory` call site
did not compile in this profile (`ENABLE_WASM_THREADS` off on ARM64 without
`-Dwasm`; no WASM symbols in dylib) — validated by identical-pattern call
compiled in `Grow`, unconditional in-scope base-method declaration, and no new
names/headers. Needs a `-Dwasm=true` build or x64 host. `zig build check`
(test profile) not run: disk at ~300 MB free vs multi-GB full-profile
compile; skipped rather than risk the shared gate + disk. Cross-runtime
(agent-to-agent) view refresh remains a documented non-goal (see limitation).
