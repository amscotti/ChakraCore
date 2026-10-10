# Zig pilot harness

Replays the existing CMake build's compile and link edges under `zig cc` /
`zig c++` to isolate **toolchain risk** (can Zig's toolchain build this code?)
from **build-logic risk** (authoring `build.zig`) during Phase 1. See
`doc/build.md` §14 for the pilot results this harness produced.

## Files

- `replay.py` — recompiles every TU in `compile_commands.json` with `zig c++`
  / `zig cc`, writing objects to `<build>/zigpilot-obj/tu{N}.o` (never touches
  the real outputs). Supports `--limit N`, `--jobs J`, `--fail-fast`,
  `--extra-flags "..."`.
- `link.py` — re-runs the archive/link/custom edges from `ninja -t commands`
  for given targets, swapping the linker for `zig c++` and every input `.o`
  for its zig-built twin (aborting upfront if any twin is missing). Rewrites
  `@rsp` files the same way (defensive: no rsp files appear in this pilot's
  edges). Supports `--extra-link-flags "..."`. **Every link/archive edge must
  prove its `-o`/`ar` outputs exist and are fresh (<20 min old) — a bare rc=0
  is not accepted** (see issue 1). Pure-custom edges without such outputs
  (e.g. the `jstoc.py` regen) still pass on rc alone.
- `bisect.py` — one-off differential harness used to bisect a warning-flag
  divergence between AppleClang and `zig c++` (kept as a template).

## Usage (macOS ARM64 pilot)

```sh
ZIG=$(mise which zig | xargs dirname)   # Zig 0.17.0 via repo mise.toml
mkdir -p build-zigpilot && cd build-zigpilot
cmake -GNinja -DCMAKE_BUILD_TYPE=RelWithDebInfo \
  -DICU_INCLUDE_PATH=/opt/homebrew/opt/icu4c/include -DDISABLE_JIT=ON \
  -DCMAKE_CXX_COMPILER=clang++ -DCMAKE_C_COMPILER=clang \
  -DCMAKE_EXPORT_COMPILE_COMMANDS=ON ..
ninja dbg_controller_h                    # generated headers first (issue 4)
python3 ../tools/zig-pilot/replay.py . $ZIG --jobs 8 --extra-flags -Wno-date-time
ls zigpilot-obj/tu*.o | wc -l             # must equal TU count (issue 6)
python3 ../tools/zig-pilot/link.py . $ZIG \
  bin/ChakraCore/libChakraCore.dylib bin/GCStress/GCStress
python3 ../tools/zig-pilot/link.py . $ZIG bin/ch/ch \
  --extra-link-flags "-Wl,-u,_OnChakraCoreLoadedEntry"
./ch ../test/Basics/hello.js
python3 ../test/runtests.py --test --variants=disable_jit --binary=$PWD/ch
```

Notes: do NOT configure in a `build/` dir on macOS (collides with tracked
`Build/`); the `-Wno-date-time` and `-u` flags are the pilot's documented
findings (build.md §14), not harness workarounds; the link runs are split
because `--extra-link-flags` applies to every target in one invocation and
`-u _OnChakraCoreLoadedEntry` breaks targets lacking that symbol (GCStress
fails with `undefined symbol` — verified); the `disable_jit` variant is
required for no-JIT builds (default dynapogo flags are rejected by any build
of `ch`, AppleClang included — verified on the baseline).

## Issues found in the harness itself (log)

1. **Fake rc=0 from shell re-quoting (critical, fixed).** An early `link.py`
   split each edge with `shlex`, substituted argv tokens, re-quoted, and ran
   with `shell=True`. Quoting turned the `&&` operators into literal arguments,
   so every `: && ...` edge degraded to the `:` no-op — all "passed" while
   producing nothing, including a bogus `LINK-PILOT-OK`. Fix: substitute
   textually on the raw command string (compiler path, `.o` paths, rsp files)
   and execute it unmodified; additionally every edge must now prove its `-o`
   / `ar` outputs exist and are fresh, else the edge fails regardless of rc.
   Lesson: **verify artifacts on disk, never trust rc alone.**
2. **`ninja -t commands` returns every edge, not just the link.** Output
   includes all compile commands plus archive/link/custom edges, with `: && `
   prefixes and `&&`-chained compounds (`cmake -E rm ... && ar qc ... &&
   ranlib ...`) that cannot be argv-exec'd. `link.py` therefore filters to
   non-compile edges and runs each through the shell.
3. **`@rpath/` is not a response file.** macOS link lines contain
   `-install_name @rpath/libChakraCore.dylib`; the `@`-rsp rewriter skips
   `@rpath` tokens explicitly (belt-and-suspenders: the is-file guard alone
   would already skip them since `build/@rpath/...` doesn't exist) plus any
   `@`-token whose file doesn't exist.
4. **Generated-header ordering.** `replay.py` needs `DbgController.js.h`
   (produced by the `jstoc.py` build step) present before compiling `bin/ch`
   TUs; run `ninja dbg_controller_h` (or a full `ninja`) first. The 8 initial
   `bin/ch` replay failures were this harness gap, not toolchain failures.
5. **`-o` redirect discipline.** Compile replay redirects `-o` into
   `zigpilot-obj/` to keep twins separate from tree outputs; link/archive
   replay deliberately keeps natural paths so dependent post-build copy edges
   keep working.
6. **Twin objects went missing once; cause undetermined.** After a reported
   538/538 compile replay, only 15 of the `tu{N}.o` files were later found on
   disk; no executed command in the session explains a deletion (all candidate
   deleters were audited). A clean re-run reproduced 538/538 with all artifacts
   verified present. Mitigation: the usage above includes an on-disk count
   check, and `link.py` aborts upfront if any twin is missing. If it recurs,
   suspect tooling outside this harness and preserve the full command history.
7. **Environment incident (non-harness, noted for the record).** Mid-pilot,
   Homebrew's `/opt/homebrew/bin/cmake` symlink vanished (Cellar intact);
   restored via `brew unlink cmake && brew link cmake`. Cause unknown; no
   pilot command touches Homebrew. If CMake edges fail with "No such file",
   check the link first.
