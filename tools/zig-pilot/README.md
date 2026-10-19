# Zig pilot harness

**Archived:** this F0-era de-risk harness is not part of the build
(`build.zig` superseded it). It is retained as the port recipe and
evidence record for future platforms; nothing invokes it.

Replay a CMake/Ninja configuration with the pinned Zig C/C++ toolchain, separating
compiler/linker capability from authoring `build.zig`. The current proof is ARM64
macOS, shared/no-JIT, RelWithDebInfo, system ICU; see `doc/build.md` §13–14.
The harness requires **Python 3.11+** and a POSIX shell/Ninja build. Windows MSBuild
and other architectures are separate migration milestones.

## Evidence contract

- `replay.py` creates a fresh `zigpilot-obj/<generation>/` with `tuN.o`, compiler
  dependency files and complete diagnostic logs. It never redirects compilation
  into the reference objects. A success requires a nonempty object and readable
  dependencies, not just rc=0.
- `zigpilot-obj/manifest.json` records the database digest, explicit target/SDK,
  extra flags, Zig executable hash/version/environment, all source/header/SDK
  dependency hashes and object hashes. It is published atomically. Each new
  attempt invalidates the previous manifest; `--limit` is diagnostic and cannot
  authorize a full link. Failed/interrupted generations are not linkable.
- `link.py` validates that complete generation against the current database,
  toolchain, inputs and objects before planning any edges. Ninja enumeration
  failure, an empty target, an unmapped `.o` or a missing response file aborts.
- Archive/link/post-build-copy outputs are removed **before their edge executes**,
  then checked for nonempty recreation. A recently produced stale file cannot
  satisfy the check. Pure generators without discoverable outputs are checked on
  rc plus the compile-input hashes; a regenerated header invalidates the objects.
- Quoted paths and arguments are parsed separately from `&&`. Only individual
  argv commands are rewritten; shell operators retain their semantics. This
  supports the simple POSIX `&&` chains emitted by the reference CMake build;
  other shell constructs/nested response files fail explicitly.
- Replays and links in one build directory are locked against concurrent runs.
  `last-link.json` records the latest invocation, its commands, artifact hashes,
  metadata and complete/failed status.

## Explicit modern macOS policy

The revival deliberately chooses **macOS 26.0** for this pilot, matching the
current Homebrew ICU 78 dylibs' minimum. Legacy 10.9/11.0 recipes are reference
history, not a deployment commitment. Target and SDK must be explicit; `zig env`
may report the host as 26.6, but that must not select the artifact's floor.
Other platform floors are declared as their builds are verified.

`replay.py` replaces inherited target/deployment/sysroot flags with
`--target aarch64-macos.26.0 --sysroot <SDK>`. Zig resolves
`-F /System/Library/Frameworks` relative to that sysroot. Compilation also needs
the SDK's `usr/include` (e.g. Security's libDER headers), and linking its `usr/lib`.
These settings come from the manifest at link time, so stages cannot silently
choose different targets. SDK settings and headers are fingerprinted; current
system ICU is recorded as an external dependency, not a hermetic release pin.

Mac executables use an explicit **8 MiB main-thread stack** (`--stack-size`, bytes;
default `8388608`). Zig's original 16 MiB default caused the lone regex test delta;
relinking the same objects at 8 MiB restored that test and a full 1959/0 suite.
`link.py` checks `LC_BUILD_VERSION.minos` and `LC_MAIN.stacksize` with `otool`.

In `build.zig`, retain the dynamic TestHooks callback on `ch` and preserve its stack:

```zig
ch.forceUndefinedSymbol("_OnChakraCoreLoadedEntry");
ch.stack_size = 8 * 1024 * 1024;
```

Both APIs were exercised under Zig 0.17.0. `Module.export_symbol_names` is
WASM-only; it did not preserve a Mach-O dynamic lookup. `GCStress` lacks the
callback, so the callback flag belongs only on the `ch` invocation.

## Usage (from the repository root)

```sh
ZIG=$(mise which zig)
SDK=$(xcrun --show-sdk-path)
cmake -S . -B build-zigpilot -GNinja -DCMAKE_BUILD_TYPE=RelWithDebInfo \
  -DICU_INCLUDE_PATH=/opt/homebrew/opt/icu4c/include -DDISABLE_JIT=ON \
  -DCMAKE_CXX_COMPILER=clang++ -DCMAKE_C_COMPILER=clang \
  -DCMAKE_EXPORT_COMPILE_COMMANDS=ON
ninja -C build-zigpilot dbg_controller_h
python3 tools/zig-pilot/replay.py build-zigpilot "$ZIG" \
  --target aarch64-macos.26.0 --sysroot "$SDK" --jobs 8 \
  --extra-flags=-Wno-date-time
python3 tools/zig-pilot/link.py build-zigpilot "$ZIG" \
  bin/ChakraCore/libChakraCore.dylib bin/GCStress/GCStress
python3 tools/zig-pilot/link.py build-zigpilot "$ZIG" bin/ch/ch \
  --extra-link-flags=-Wl,-u,_OnChakraCoreLoadedEntry
./build-zigpilot/ch -nonative test/Basics/hello.js  # must print hello world / PASS
python3 tools/regenByteCode.py --noJit --verify --binary="$PWD/build-zigpilot/ch"
python3 test/runtests.py --test --variants=disable_jit --binary="$PWD/build-zigpilot/ch"
```

Run the shared embedding check from its runner directory:

```sh
# REPO is the absolute repository root; invoke in test/native-tests/
./test_native.sh "$REPO/build-zigpilot/ch" RelWithDebInfo \
  /usr/bin/clang /usr/bin/clang++ "$REPO"
```

The manifest verifies object coverage; a manual object count alone is insufficient.
No-JIT builds require `disable_jit`; dynapogo flags are rejected by the reference
shell too. `ninja check` in the disposable pilot directory would rebuild missing
reference objects and overwrite Zig outputs, so invoke the proven check commands
above directly. Use `build-zigpilot/`, never `build/` on a case-insensitive Mac
filesystem: the latter collides with tracked `Build/`.

Diagnostics:

```sh
python3 tools/zig-pilot/replay.py build-zigpilot "$ZIG" \
  --target aarch64-macos.26.0 --sysroot "$SDK" --limit 1 --fail-fast
python3 tools/zig-pilot/warning_bisect.py build-zigpilot "$ZIG" \
  --target aarch64-macos.26.0 --sysroot "$SDK"
python3 tools/zig-pilot/test_harness.py
```

`warning_bisect.py` writes to `zigpilot-bisect/` and does not certify or change a
replay generation. Its controlled warning failures are expected diagnostic output.
The tests exercise real Ninja edges and controlled compilers: missing artifacts,
recent stale outputs, partial runs, input/toolchain/database changes, missing or
tampered objects, response files, quoted paths/`&&`, locks and fail-fast accounting.

## Issue history and corrections

1. **Quoted `&&` gave fake rc=0.** Re-quoting entire shell chains turned the `:`
   prefix into a no-op. Rewrite individual commands while preserving operators,
   and prove output recreation.
2. **Ninja returns compile and non-compile edges.** Identify compiler `-c` commands
   rather than filtering arbitrary text (e.g. Python's `-c`). Enumerate every
   requested target successfully before deleting outputs; share repeated edges.
3. **`@rpath/` is not a response file.** Preserve linker path tokens, rewrite
   actual response-file objects, and abort on missing response files.
4. **Generated-header ordering.** Generate `DbgController.js.h` before replaying
   `bin/ch`. A generator that changes it later requires recompilation.
5. **Output isolation.** Compile twins stay separate. Link/archive/copy edges use
   their natural paths inside the disposable build directory.
6. **Missing twins were once reported after 538/538.** Cause was undetermined.
   Object existence, size, digest and generation provenance now replace the
   original manual-count mitigation.
7. **Homebrew CMake symlink disappeared mid-pilot.** Cellar remained intact;
   `brew unlink cmake && brew link cmake` restored it. No pilot command touched
   Homebrew; this remains an environment incident, not a compiler finding.
8. **Invalid Ninja targets reported success.** Check enumeration rc and reject
   targets without output-producing edges; never treat zero edges as a proof.
9. **Twenty-minute freshness was insufficient.** A no-op could accept a recent
   output. Remove declared outputs before execution and require new artifacts.
10. **Indexed twins could mix old and new flags.** Fresh generations, complete
    manifests and input/object hashes prevent partial replay from authorizing links.
11. **Fail-fast counted uncollected results as successes.** Bound submitted work,
    drain running tasks, count actual outcomes and report not-run units separately.
12. **`bisect.py` shadowed Python's standard library.** `tempfile`/`random` imported
    the diagnostic script as stdlib `bisect`. Rename it to `warning_bisect.py` and
    guard its entry point.
13. **Native defaults differed despite green compilation.** Explicit macOS target,
    SDK/header/framework paths, callback retention and 8 MiB stack are now verified
    inputs. The regex delta was stack reservation, not a justified test waiver.
