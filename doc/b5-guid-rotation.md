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

Tool adjustments: none planned; the script is driven through its
existing options only.

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
  demo (JsRT host against the debug-static archive): serialize a
  script with the rotated build, patch the GUID DWORDs in the cache
  file back to `d31c196c...`, load → expect `JsErrorBadSerializedScript`
  (reject), while the unpatched cache runs.

Results: recorded below after the rotation (added by the receipt
commit).

## What B5 still owes beyond this lane

The broader B5 package/floor work (controlled dependencies, clean
machine floor, artifact manifest) is untouched — see §2.2 B5. This
lane only rotates the GUID and proves the rejection boundary.
