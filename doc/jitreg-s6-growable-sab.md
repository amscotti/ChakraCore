# JIT-REG card: s6-growable-nonshared.js — stale auto-length TypedArray/DataView after SAB grow under FullJIT+PGO

Status: NOT a post-e14 regression. Bisected to pre-existing (present at e14-green
tree 69281b6e4 AND pre-b5 tree 6c1470f9d with identical failing lines).
Root cause (code-confirmed): `SharedArrayBuffer::Grow` does not refresh live
views' cached effective-length field. Carded for follow-up lane (fix needs SAB
parent tracking + GC-field review, not one-line). Do NOT fix in a JIT lane.

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

## Follow-up guidance

Fix = add SAB parent tracking (new GC-retained fields → read
`doc/memory-management.md` first per AGENTS.md ownership rule) + refresh walk in
`SharedArrayBuffer::Grow` (+ `GrowMemory`), then re-verify s6 PGO 13/13 plus the
RAB differential and the es7 folder. Deliberately NOT fixed here: not
one-line-class, touches GC fields and two growth paths.
