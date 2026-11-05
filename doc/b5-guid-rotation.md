# B5 slice: serialized-bytecode GUID rotation

Lane: `b5-guid-rotation` (base `5b0302bbd` = `modernize/baseline`). This
closes the B5-scoped hazard recorded in
[modernization-plan.md §2.2 B5](modernization-plan.md) and the
"GUID rotation still owed" bullet in
[arm64-macos-jit.md](arm64-macos-jit.md) merge closure.

## Hazard

The serialized bytecode format changed versus baseline `97e68c917`
(appended `ctBigInt`, new function-attribute bits, and with the
2026-10-31 JIT merge an appended built-in property `ENTRY(bigint)` and
an extended opcode). `tools/regenByteCode.py` mints a new
`byteCodeCacheReleaseFileVersion` GUID only when a **dual-variant
(JIT + no-JIT)** regen detects bytecode changes (script logic at
`tools/regenByteCode.py:254-282`), and every regen since the format
changes was single-variant, so no GUID was minted. Serialized caches
written by baseline builds and by this branch therefore share a GUID
despite incompatible formats: a stale cache could pass the version
check and be misread.

Rejection path (source): the serializer stamps the GUID as four DWORDs
(`ByteCodeSerializer.cpp:526-535`, `ReleaseVersioningScheme`) and the
loader compares the same four DWORDs when deserializing
(`ByteCodeSerializer.cpp:3099-3171`); any mismatch returns
`InvalidByteCode`, which `RunSerializedScriptCore` surfaces as
`JsErrorBadSerializedScript` (`lib/Jsrt/Jsrt.cpp:4083-4086`). A
correctly rotated GUID therefore turns cross-version cache loads into
clean rejections instead of misreads.

## Established state (before rotation)

- Current GUID: `d31c196c-a985-4c41-a9b3-5a895b7dc567` — identical at
  baseline `97e68c917` (inherited upstream; last minted upstream in
  `a8809424b "Bytecode Regen"`). Never rotated on this branch.
- Tracked GUID header path is `lib/Runtime/ByteCode/` (capital C;
  macOS case-insensitive FS accepts either spelling — use the exact
  case for git operations in this tree).
- Checked-in builtin-bytecode headers (sha256, pre-rotation):

  | file | sha256 | lines |
  |---|---|---|
  | `JsBuiltIn.bc.64b.h` (JIT) | `aac5b3e3588df08b3260681946ce083e7b7ee79d80270d167cad03685b1f9386` | 8553 |
  | `JsBuiltIn.bc.32b.h` (JIT) | `063b6cc46322eb8ffd60e2ca099afbd752ef34436abb2f63b07aa5475abbb3db` | 8553 |
  | `JsBuiltIn.nojit.bc.64b.h` | `baaa7ceda33558c8845a9dba203f9b54d06858a2a3b84fd65a5eaddf788a9442` | 7825 |
  | `JsBuiltIn.nojit.bc.32b.h` | `e31c83aaceeb2634fc3aa048b317df32954eeb1d5e1be44ee1e9e95af7cafe54` | 7826 |

- The two variants' serialized outputs genuinely differ (they are
  different formats, not a re-encoding of the same bytes): the 64b
  pair differs in 11,982 `diff` lines (32b pair: 11,983), starting at
  file offset 4 — immediately after the `ChBc` magic — where the JIT
  stream carries `0x39 0x32` and the no-JIT stream `0x5A 0x2E`
  (function-body/op-count size fields), and again at offset `0x1034`
  (`0x08` JIT vs `0x00` no-JIT, the JIT-attributed function-attribute
  bits). Within a variant the 64b/32b pair differs by 6,140 (JIT) /
  5,401 (no-JIT) diff lines (architecture-width encodings).
- `c69f32879` is the precedent for variant-correct binaries: the
  nojit headers regenerated with `zig-out/bin/ch` (no-JIT test
  profile), the JIT headers with the JIT binary; using the wrong
  binary for a variant writes ~11.3k wrong-format diff lines and
  breaks no-JIT builtins.

## Procedure (on this machine, Zig builds)

The script's own build path is the legacy CMake/build.sh one and
cannot run here; the variant-correct procedure uses pre-built
binaries. Key script facts established by reading it:

- `--binary=<path>` **forces single-variant** (the script rejects
  `--binary` unless exactly one of `--jit`/`--noJit` is given), and
  the mint requires both variants true — so a `--binary` run can
  never mint. `--binary` is the verification path (used by the build's
  `verify-bytecode` steps via `tools/build/verify_bytecode_pure.py`).
- Without `--binary`, `--skip-build` runs dual-variant from the
  default binary paths `out/noJit/debug/ch` and `out/jit/debug/ch`
  (`tools/regenByteCode.py:215-216`) — this is the only invocation
  whose change detection can fire the mint.

Rotation steps (all builds/gates through the shared gate lock):

1. Build both binaries from this branch: `zig build` (test profile,
   no-JIT → `zig-out/bin/ch`) and `zig build -Dprofile=jit`
   (→ `out/jit/bin/ch`).
2. Restore all four headers to baseline `97e68c917` bytes — the honest
   pre-rotation state; the format divergence from baseline is exactly
   the change the mint must observe.
3. Copy the binaries to the script's dual-variant default paths
   (`out/noJit/debug/ch`, `out/jit/debug/ch`).
4. Run `python3 tools/regenByteCode.py --skip-build` (dual variant).
   Both variants regenerate from their correct binaries; change
   detection sees the baseline divergence; the script mints the new
   GUID itself through its own template logic
   (`tools/regenByteCode.py:262-282`).
5. Confirm the four regenerated headers are byte-identical to the
   pre-rotation table above (deterministic regeneration: library
   bytecode uses the fixed `LibraryByteCodeVersioningScheme` version
   0,0,0,0 — `ByteCodeSerializer.cpp:537-547` — so no timestamps are
   embedded). The net tree change must be the GUID header only.

Tool adjustments: none needed; the script was driven through its
existing options only (one placement detail: each copied `ch` needs
its `libChakraCore.dylib` beside it — the engine loads the dylib from
its own directory — so `out/noJit/debug/` got the no-JIT dylib from
`zig-out/bin/` and `out/jit/debug/` the JIT dylib from `out/jit/bin/`).

## Procedure actually used (receipt)

All builds and gates ran through the shared gate lock
(`sh /tmp/opencode/cc-gate-lock.sh`, zig 0.17.0 via mise). The lock was
orphaned three times by processes of other sessions reaped mid-gate
(lock dir stale, zero build/gate processes alive, `ps`-verified each
time); each orphan was reclaimed per the lock script's own reclaim
policy (owner provably dead) at 17:43:36, 18:35:26, 19:29:36 and
20:19:46 and 21:14:46. Commands:

1. `zig build` (test profile, no-JIT) → `zig-out/bin/ch` +
   `zig-out/bin/libChakraCore.dylib`.
2. `zig build -Dprofile=jit` → `out/jit/bin/ch` (+ dylib).
3. Variant-output evidence: `ch -GenerateLibraryByteCodeHeader
   -LdChakraLib -JsBuiltIn Array_prototype.js` from each binary —
   1,038 `diff` lines apart for that single entry.
4. Restore: the four `JsBuiltIn*.h` to exact `97e68c917` bytes
   (sha256-verified against the staged baseline blobs).
5. `cp` binaries + dylibs to `out/{noJit,jit}/debug/`.
6. `python3 tools/regenByteCode.py --skip-build` → all four headers
   regenerated, change detection fired, script printed "Generating
   new GUID for new bytecode" and minted through its own template.
7. Verification gates (below) and the runtime rejection demo.

**New GUID: `55c5da6d-dcde-42f9-8112-f7f698fcc96c`**
(was `d31c196c-a985-4c41-a9b3-5a895b7dc567`). Net tree change of the
rotation: `lib/Runtime/ByteCode/ByteCodeCacheReleaseFileVersion.h`
only (2 lines); every regenerated header is byte-identical to the
pre-rotation table above (all four sha256 match), which is the
determinism proof and confirms the headers were already
variant-correct after `c69f32879`.

## Verification results

| check | result |
|---|---|
| `zig build check` (test profile, new GUID compiled in) | **2036 passed / 0 failed** (pin 2036/0), bytecode verify green |
| `zig build -Dprofile=debug-static check` | **2088 passed / 0 failed** (pin 2088/0), bytecode verify green |
| `zig build -Dprofile=jit check` | smoke PASS, `check_jit` PASS (NativeCodeGenerator symbol, `-mic`/`-forceNative` hot-call smokes), `verify_bytecode_pure.py --jit --verify` with `out/jit/bin/ch` green ("Bytecode is up to date") |
| `regenByteCode.py --verify` no-JIT variant (`zig-out/bin/ch`, through the test-profile check's verify leg) | green, source headers unchanged |
| `regenByteCode.py --verify` JIT variant (`out/jit/bin/ch`, through the jit check's verify leg) | green, source headers unchanged |

## Cross-version rejection demo (runtime)

Source path: serializer stamps the GUID as 4 DWORDs
(`ByteCodeSerializer.cpp:526-535`); loader compares V1..V4
(`ByteCodeSerializer.cpp:3099-3171`), mismatch → `InvalidByteCode` →
`JsErrorBadSerializedScript` (`lib/Jsrt/Jsrt.cpp:4083-4086`).

Runtime proof (host links `zig-out/bin/libChakraCore.dylib`, built
after the rotation; release scheme forced with
`-ForceSerializedBytecodeVersionSchema:21` — 21 is the non-NTBUILD
`ReleaseVersioningScheme`, fed to the engine through the exported
`SetConfigFlags` test hook, the same mechanism `ch` uses;
scaffolding preserved in [b5-guid-demo/](b5-guid-demo/)):

```
$ ./demo serialize cache-new.bin -ForceSerializedBytecodeVersionSchema:21
SERIALIZED 326 bytes                       # header: "ChBc", size, scheme 0x15,
                                            # then 6ddac555 dedcf942 8112f7f6 98fcc96c
$ ./demo run cache-new.bin -...            # same GUID: accepted
RUN_OK CACHE_OK:4950
$ python3 patch_guid.py cache-new.bin 55c5da6d-... d31c196c-...   # stamps -> baseline GUID
patched GUID stamps at file offset 9
$ ./demo run cache-new.bin -...
RUN_REJECTED JsErrorCode=65544 (JsErrorBadSerializedScript=65544)
```

Controls: an arbitrary third GUID is rejected identically (the
version-stamp check is what rejects); the JIT-profile dylib (same
rotated GUID) serializes 333 bytes and runs its own cache. Bonus
boundary observed: a cache written by the JIT dylib is rejected by the
no-JIT engine even though both now share the GUID — the deeper format
checks (function-body size/op-count/attribute fields, which differ per
variant) reject it independently, so variant-mismatched caches cannot
be misread either.

`doc/memory-management.md`: N/A for this lane — generated bytecode
headers and a version GUID constant; no allocation, ownership,
retained-pointer, callback, async, GC or memory-accounting boundary is
touched. Verified by reading the full changed set (2 lines, one
header constant).

## Residual

- The regen tool still cannot mint from `--binary` runs; any future
  single-binary regen workflow that needs a mint must repeat this
  dual-variant default-path procedure. (Not fixed here: tool changes
  were out of scope and the existing flow works.)
- Engineering-scheme caches (default for these non-release builds)
  are versioned by `__DATE__`/`__TIME__` hashes, not the GUID; the
  GUID boundary governs the release scheme. A release-profile build
  (`CHAKRA_CORE_VERSION_RELEASE 1`) picks it up automatically since
  both schemes compile the same `ByteCodeCacheReleaseFileVersion.h`.
- B5 package/floor work (controlled dependencies, clean-machine
  floor, artifact manifest, two-build reproducibility proof) remains
  open — see §2.2 B5.

## Verification plan

- `zig build check` (no-JIT test profile): regression 2036/0 plus
  no-JIT bytecode verify (compiles the rotated GUID + headers in).
- `zig build -Dprofile=debug-static check`: regression 2088/0.
- `zig build -Dprofile=jit check`: JIT smoke, JIT runtime, JIT
  bytecode verify (`verify_bytecode_pure.py --jit --verify` with
  `out/jit/bin/ch`).
- `tools/regenByteCode.py --verify` equivalent for both variants from
  their respective binaries (the two build verify legs above).
- Cross-version rejection: source path cited above, plus a runtime
  demo (below) — serialize with the rotated build under the release
  scheme, patch the GUID DWORDs in the cache file back to
  `d31c196c...`, load → `JsErrorBadSerializedScript` (reject), while
  the unpatched cache runs.

## What B5 still owes beyond this lane

The broader B5 package/floor work (controlled dependencies, clean
machine floor, artifact manifest) is untouched — see §2.2 B5. This
lane only rotates the GUID and proves the rejection boundary.

### Review outcome (dual ACCEPT, hazards folded)

Both reviewers ACCEPTed with zero `[bug]`. Independently verified: the
mint's honesty (`--binary` can never mint — script defaults + dual-flag
requirement; committed header byte-exact 742/742 to the script's
template output for the minted GUID), meaningful detection (baseline
divergence 15.5-16.9k lines/header), determinism (adversarial reran the
dual-variant regen twice — no-mint path, byte-identical headers), the
GUID boundary (stamps before body interpretation; engineering scheme
unaffected; old GUID only in prose; new GUID proven embedded at runtime
in both dylibs via serialized-cache stamps), cross-variant rejection in
BOTH directions plus end-to-end deserialization (not just acceptance),
and the gates (2036/0 twice; jit verify leg green).

**Folded hazards/nits (both reviews):**
- **Single-variant `--binary` OVERWRITE hazard (required before B5
  package/floor work):** a `--skip-build --binary=<wrong-variant>` run
  trusts the FLAG, not the binary — it silently rewrites that variant's
  tracked headers with wrong-format content (~15.8k diff lines, variant
  signature at byte offset 4), warning-only, **exit 0**, GUID kept (mint
  is dual-variant-only), and builtin headers carry fixed `0,0,0,0`
  stamps so NO version check rejects a wrong-variant builtin at load —
  only the next gate's variant-correct verify leg backstops it. Rule for
  future rotations: pair `--binary` with the correct variant flag and
  verify all four header sha256 against the receipt table before
  committing; a tooling guard (variant-signature check, or nonzero exit
  on single-variant content change) is the required fix before B5
  package/floor.
  → (2026-11-05 C sweep) the **guard has landed**: a single-variant run
  whose content differs from the checked-in headers now refuses (exit 1)
  and restores the headers byte-identical, with an
  `--allow-single-variant-write` scratch escape hatch; see
  doc/open-cards.md.
- `regenByteCode.py:263` writes the GUID header via lowercase
  `lib/Runtime/Bytecode/` — same file on macOS's case-insensitive FS,
  but on **Linux** the mint would create a NEW untracked file and leave
  the tracked header unrotated, undetectable by `--verify`. Pre-existing
  upstream; flagged to the B5 package/Linux lane.
- The stale pre-rotation copies under `out/{noJit,jit}/debug/` (18:40 <
  mint) would stamp the OLD GUID in any future release-scheme demo —
  use `zig-out/bin` / `out/jit/bin` (post-rotation) for demos.
- Engineering-scheme collision safety rests on `__TIME__` granularity
  (two format-changing builds in the same clock minute share stamps) —
  pre-existing; noted for B5 package work.
- Receipt prose fix: the lock was orphaned five times (17:43, 18:35,
  19:29, 20:19, 21:14), not three; the doubled "and" is corrected here.
