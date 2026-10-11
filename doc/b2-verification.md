# B2 verification record

Complete B2 evidence for the Mac reference profile
([modernization-plan.md](modernization-plan.md) §B2,
[build-contract-mac.md](build-contract-mac.md)): B2.1 artifacts and
metadata, B2.2 ordering-correctness proof and preserved-behavior
receipts, B2.3 check gates with legacy-identical selection, and the
B2.4 fresh-checkout proof.

Date: 2026-10-11. Branch: `modernize/baseline`. B2.2 ran ~13:00 EDT
from the main tree; B2.1/B2.3/B2.4 ran ~14:30–15:00 EDT from the
fresh clone `/tmp/b2-fresh` (left in place for reviewers; see B2.4).
Every command below was re-run on this date; observed values are
quoted verbatim.

## Toolchain

All builds use ONLY mise zig 0.17.0 (`mise exec -- zig ...`).

- `mise exec -- zig version` → `0.17.0`
- SDK: Xcode `MacOSX.sdk` 26.5 at
  `/Applications/Xcode.app/Contents/Developer/Platforms/MacOSX.platform/Developer/SDKs/MacOSX.sdk`
  (from the `b2 profile:` build line)
- ICU: Homebrew `U_ICU_VERSION "78.3"`
  (`otool -L` on fresh `ch`: `libicudata.78.dylib` current 78.3.0)
- Host: macOS 26.6, arm64, Apple M1 Max
- Python: mise 3.12 (`/Users/ascotti/.local/share/mise/installs/python/3.12/bin/python3`,
  from the fresh `b2 profile:` line; the legacy `ninja-check.log`
  used Homebrew 3.14 — interpreter path differs, test flags identical)

## B2.1 artifacts and metadata

Full graph: 538 TUs compiled once each (`build.zig:417` asserts the
count: 8 ch + 4 GCStress + 3 dylib + 523 static-archive members),
each to a zero-padded `NNNN-<base>.o` (`build.zig:270-297`) — 538
distinct numbered objects observed under the fresh `.zig-cache`.
Installed `zig-out` tree from the B2.4 fresh build:

```sh
zig-out/bin/ch                       # dynamic ch
zig-out/bin/GCStress
zig-out/bin/libChakraCore.dylib      # co-located copy
zig-out/lib/libChakraCoreStatic.a    # 523 .o + __.SYMDEF
zig-out/lib/libChakra.Pal.Singular.a # 91 .o + __.SYMDEF
zig-out/lib/libChakra.Common.Codex.Singular.a            # 1 .o + __.SYMDEF
zig-out/lib/libChakra.Runtime.PlatformAgnostic.Singular.a # 11 .o + __.SYMDEF
zig-out/lib/libChakraCore.1.0.0.dylib -> libChakraCore.1.dylib -> libChakraCore.dylib
zig-out/include/ChakraCore.h ChakraCommon.h ChakraDebug.h ChakraCoreVersion.h
zig-out/pal/src/config.h
```

```sh
for a in zig-out/lib/*.a; do echo "== $a"; ar t $a | wc -l; done
# libChakraCoreStatic.a: 524 lines (523 .o + __.SYMDEF)
# libChakra.Pal.Singular.a: 92 lines (91 .o + __.SYMDEF)
# libChakra.Common.Codex.Singular.a: 2 lines (1 .o + __.SYMDEF)
# libChakra.Runtime.PlatformAgnostic.Singular.a: 12 lines (11 .o + __.SYMDEF)
find .zig-cache -name '[0-9][0-9][0-9][0-9]-*.o' | sed 's/.*\///' | sort -u | wc -l
# → 538
```

Metadata (all asserted by the B2.3 metadata gate; otool values
re-checked directly):

- macOS minimum 26.0 on `ch`, `GCStress`, both dylibs
  (`otool -l` LC_BUILD_VERSION, cross-checked with `vtool
  -show-build`).
- LC_MAIN `stacksize 8388608` on `ch` and `GCStress`.
- The 4 staged headers byte-identical to their `lib/` sources
  (81416 / 97376 / 47103 / 2734 bytes).
- 17599 exported `T` symbols in the dylib, required 8/8 JsRT
  exports present (`_JsCreateRuntime` at `T _JsCreateRuntime`).
- Dylib identity: `LC_ID_DYLIB @rpath/libChakraCore.dylib`,
  current 1.0.0 / compatibility 1.0.0 — matching the pilot
  `build-zigpilot/libChakraCore.dylib` on all three fields.

Minimum/sdk floors, disambiguated: ours stamps minos 26.0 / sdk
27.0, exactly matching the pilot `build-zigpilot/ch` and
`build-zigpilot/libChakraCore.dylib` (both minos 26.0 / sdk 27.0).
The legacy CMake `build-xplat` tree stamps minos 11.0 / sdk 26.5
(the CMake default floor — the 26.0 minimum is the deliberate B2
profile choice matching the ICU 78 dylib floor, plan §2.1).
The SDK gate pins MacOSX SDK 26.5 (`build.zig:45`) while the stamp
reads 27.0: with no `--sysroot` knob in the build API the stamp
comes from the toolchain default (`build.zig:166-172`); minos 26.0
holds on every artifact.

### Co-install edge

`ch` has no link-time dependency on the dylib (`otool -L` lists
only CoreFoundation, Security, libSystem, the three ICU 78 dylibs,
libobjc) and no `LC_RPATH` — the loader resolves
`<exe-dir>/libChakraCore.dylib` at runtime, so the install step
places a second copy of the dylib beside `ch` (`build.zig:503`).
Receipt (fresh tree; dylib moved aside and restored):

```sh
mv zig-out/bin/libChakraCore.dylib /tmp/b2-coinstall-hide.dylib
./zig-out/bin/ch /tmp/smoke.js  # → exit 5
# FATAL ERROR: Unable to load dlopen() failed; ... (no such file)
mv /tmp/b2-coinstall-hide.dylib zig-out/bin/libChakraCore.dylib
./zig-out/bin/ch /tmp/smoke.js  # → smoke:2, exit 0
```

`GCStress` links only system libraries (`otool -L`: CoreFoundation,
Security, libSystem, libobjc) — the engine arrives via the static
archive (`build.zig:479-490`). `GCStress -v` segfaults (exit 139)
identically on the fresh, legacy-CMake, and pilot binaries: a
pre-existing engine behavior, not a B2 regression (see Known
behaviors).

## B2.2 ordering proof (PAL deviation disposition)

The link-order deviation is confined to the PAL section (reference: 18
cruntime C++, 21 safecrt C, 47 C++ debug..threadsusp, mach pair, 3 asm;
ours: mach pair moved first, 65 C++ in reference-relative order (67 C++
total), 21 C, 3 asm), and
that deviation is unobservable at startup because none of the reordered
inputs contributes a dynamic initializer: `nm -m` reports zero
`mod_init`/`global_var_init`/`__GLOBAL` entries in both mach-pair
objects, zero across all 24 PAL C/asm objects, and source grep finds no
constructor/init-priority/mod-init markers in the 21 safecrt C units,
the 3 arm64 asm thunks, or the two mach TUs (the lone `constructor` hit
is a code comment), while a same-group positive control built with the
same flags shows the probe fires (count 3: `__GLOBAL__sub_I` plus a
`__DATA,__mod_init_func` entry), so the zeros are real absences, not a
dead probe — hence initializer sequencing is identical to the reference
order and the `NNNN-` numbering stands as laid out in `build.zig`.

```sh
M1=$(find .zig-cache/o -name '0016-machexception.cpp.o')
M2=$(find .zig-cache/o -name '0017-machmessage.cpp.o')
CTL=$(find .zig-cache/o -name '0081-pal_thread.cpp.o')
nm -m "$M1" | grep -c -e mod_init -e global_var_init -e __GLOBAL  # → 0
nm -m "$M2" | grep -c -e mod_init -e global_var_init -e __GLOBAL  # → 0
nm -m "$CTL" | grep -c -e mod_init -e global_var_init -e __GLOBAL # → 3
nm -m "$CTL" | grep -e mod_init -e __GLOBAL
# → 0000000000002020 (__TEXT,__StaticInit) non-external __GLOBAL__I_000200
# → 0000000000002084 (__TEXT,__StaticInit) non-external __GLOBAL__sub_I_pal_thread.cpp
# → 0000000000002298 (__DATA,__mod_init_func) non-external ltmp13
```

PAL C/asm sweep (21 safecrt C + 3 arm64 asm objects, resolved by
`NNNN-<basename>` under `.zig-cache/o`): 24 objects, 0 total init hits
with the same `nm -m | grep -c` probe (a graph-wide sweep over all 31
C/asm objects likewise reports 0).

```sh
grep -rn -e '__attribute__((constructor))' -e 'init_priority' -e '__mod_init' \
  pal/src/safecrt/ pal/src/arch/arm64/  # → no matches, exit 1
grep -rn -e constructor -e INIT_PRIORITY -e mod_init \
  pal/src/exception/machexception.cpp pal/src/exception/machmessage.cpp
# → pal/src/exception/machexception.cpp:1160: "MachExceptionInfo constructor"
#   (code comment only)
```

Tree-wide, `__attribute__((constructor))`/`init_priority` appears only
as the `INIT_PRIORITY` macro definition (`lib/Common/CommonPal.h:130`,
non-Windows branch), used once outside the PAL deviation
(`lib/Common/Core/SysInfo.cpp:36`).

## Preserved-behavior receipts

### Warnings: 25 `-Wno-*`, C++-only

Table size plus a live capture (`zig build --verbose` after touching
one C++ and one C TU; build exit 0, exactly the 2 touched TUs
recompiled):

```sh
grep -c '"-Wno-' tools/build/tables.zig  # → 25
# live C++ line (zig c++ … 0107-Api.cpp.o): 26 -Wno-* …
# live C line (zig cc … 0089-memcpy_s.c.o): exactly 1 (-Wno-date-time)
```

Observed: the C++ line carries 26 `-Wno-*` flags whose set minus
`-Wno-date-time` diffs clean against the 25 table entries (25/25); the
C line carries only `-Wno-date-time` (the build-required addition all
units share) — all 25 table warnings absent. Code path:
`build.zig:appendTuFlags` emits `tables.warnings` under `.cxx` only.

### Force-load: engine TUs linked directly into the dylib

Zig's Mach-O linker has no force-load spelling, so the dylib links
every engine TU directly (`build.zig:432-433`):

```sh
for (dylib_objs.items) |o| dylib_mod.addObjectFile(o);
for (static_objs.items) |o| dylib_mod.addObjectFile(o);
```

Smoke (the dylib loads from the executable's own directory):

```sh
echo 'print("smoke:" + (1+1));' > /tmp/smoke.js && ./zig-out/bin/ch /tmp/smoke.js
# → smoke:2, exit 0
./zig-out/bin/ch -v  # → ch version 1.13.0.0-beta
nm -g zig-out/bin/libChakraCore.dylib | grep -c " T "  # → 17599
nm -g zig-out/bin/libChakraCore.dylib | grep JsCreateRuntime
# → 0000000000064998 T _JsCreateRuntime
```

### Callback: `_OnChakraCoreLoadedEntry` export

Retention comes from default Mach-O executable export of the defined
global (`bin/ch/ch.cpp:46`); the `forceUndefinedSymbol` call
(`build.zig:474`) is verified effective and harmless:

```sh
nm -g zig-out/bin/ch | grep OnChakraCoreLoadedEntry
# → 00000001000025b8 T _OnChakraCoreLoadedEntry
./zig-out/bin/ch --bogus-flag-xyz  # → exit 5
# → Invalid Flag : --bogus-flag-xyz (then usage text)
```

### Stack: 8 MiB on both executables

```sh
otool -l zig-out/bin/ch | grep -A3 LC_MAIN
# → stacksize 8388608
otool -l zig-out/bin/ch | grep -A5 LC_BUILD_VERSION
# → minos 26.0, sdk 27.0 (sdk stamp matches the reference; see build.zig)
otool -l zig-out/bin/GCStress | grep -A3 LC_MAIN
# → stacksize 8388608
```

Source: `ch.stack_size` / `gs.stack_size = 8 * 1024 * 1024`
(`build.zig:476,490`).

## verify-bytecode purity

`regenByteCode.py --verify` REWRITES the checked-in headers even on
success (observed pre-fix: green run, identical md5s, both nojit-header
mtimes bumped) and self-heals red-to-green (observed on a `/tmp`
scratch copy: first raw run exit 1 `Bytecode changes detected`,
second raw run exit 0 `Bytecode is up to date`, planted
`STALE-MARKER` destroyed). The `verify-bytecode` step therefore runs
through `tools/build/verify_bytecode_pure.py`, which snapshots the
four headers, runs the script, restores every header byte- and
mtime-identical, and exits with the script's verdict (refuses to run
without `--verify`: exit 2).

Green proof (real tree, wired step):

```sh
mise exec -- zig build verify-bytecode  # → exit 0, "Bytecode is up to date"
# + "verify_bytecode_pure: 4 header(s) restored byte- and mtime-identical; regen exit 0"
# md5 of all 4 JsBuiltIn.*.h before/after → identical
# mtimes of all 4 JsBuiltIn.*.h before/after (2s sleep) → identical
```

Red proof (`/tmp` scratch copy: `tools/regenByteCode.py` +
`lib/Runtime/Library/InJavascript/` mirrored, `STALE-MARKER` appended
to the `nojit.bc.64b.h` copy, real `zig-out/bin/ch` as `--binary`;
the real tree is never touched):

```sh
python3 tools/build/verify_bytecode_pure.py /tmp/bc-scratch/tools/regenByteCode.py \
  --noJit --verify --binary=$PWD/zig-out/bin/ch  # → exit 1
# → "Bytecode changes detected - the generated bytecode files are not up to date…"
# planted marker intact (grep count 1), md5 identical to planted state,
# mtimes identical (2s sleep)
```

B2.4 re-execution (fresh clone, ~14:45 EDT, new `/tmp/bc-scratch`
with `STALE-MARKER-B2PROOF` planted): wrapper exit 1 with the same
`Bytecode changes detected` verdict, marker count 1, md5
`f1b59412b9fc5e9412584a47845603c7` and mtime `1791743928` identical
before/after. Raw-script control on the same scratch: first raw run
exit 1, second raw run exit 0 (`Bytecode is up to date`), marker
destroyed — the self-heal the wrapper exists to prevent. Refusal
probe: wrapper without `--verify` exits 2
(`refusing to run without --verify …`).

## B2.3 check gates

`zig build check` aggregates five gates (`build.zig:598-603`), each
depending on the install step (fresh-checkout safe), each with
inherited stdio, none reachable from the default build. Fresh-clone
run: exit 0, wall 62s (regression leg 40.94s inside).

### (1) Regression: 1959 passed / 0 failed, legacy-identical selection

The gate runs the legacy-exact command (`test/CMakeLists.txt:39-40`;
`--test` from `:3-7`, empty ICU tag from `:9-11`,
`--variants=disable_jit` from `:13-18`):

```sh
python3 ./test/runtests.py --test --variants=disable_jit --binary=/private/tmp/b2-fresh/zig-out/bin/ch
Total: passed 1959, failed 0
check_regression: exit=0 parsed=(1959, 0) require passed 1959 / failed 0
check_regression: PASS: passed 1959 / failed 0
```

The verdict parses the real `Total:` line (exactly one required),
not rc alone (`tools/build/check_regression.py:44-58`). Legacy
invocation from `build-xplat/ninja-check.log` carries identical
flags (`runtests.py --test --variants=disable_jit
--binary=…/build-xplat/ch`); only the tree path and the interpreter
(3.14 vs mise 3.12) differ.

Multiset evidence (fresh `/tmp/b2-fresh-check.log` vs legacy
`build-xplat/ninja-check.log`, both observed this session):

- `exclude:` tag sets: sorted-diff clean — identical 13-tag
  selection (print order differs; set semantics).
- Per-folder result lines: ordered-diff clean — all 49 folder
  lines byte-identical with the same counts.
- `Total: passed 1959, failed 0` on both sides.

### (2) Bytecode verification

```sh
Bytecode is up to date
verify_bytecode_pure: 4 header(s) restored byte- and mtime-identical; regen exit 0
```

Legacy form `regenByteCode.py --noJit --verify --binary=<ch>`
(`test/CMakeLists.txt:33-34`); the purity wrapper rationale and proofs
are recorded under "verify-bytecode purity" above. This step is the
only `regenByteCode.py` reference in the graph (`build.zig:511-534`),
so no build edge regenerates bytecode with the not-yet-built engine.

### (3) Shared embedding (native) test

```sh
bash …/test_native.sh /private/tmp/b2-fresh/zig-out/bin/ch RelWithDebInfo \
  /usr/bin/clang /usr/bin/clang++ /private/tmp/b2-fresh
test-shared-basic : PASS
check_native: exit=0
check_native: PASS: test-shared-basic : PASS, 8 static suites ignored
```

Legacy method (`test/CMakeLists.txt:49-50`), run in a `$TMPDIR` copy of
`test/native-tests` so the tree is never written to. The script's
own output (`Testing`/`Ignoring`/`test-shared-basic` lines)
diffs clean against the legacy log; the only delta is the gate's
own `check_native: PASS:` summary line.

### (4) Smoke/flag output assertions

```sh
check_smoke: ch -v -> rc=0 out='ch version 1.13.0.0-beta' err=''
check_smoke: ch --this-flag-does-not-exist -> rc=5 out=['Invalid Flag : --this-flag-does-not-exist'] err=[]
check_smoke: snippet -> rc=0 out='zig-smoke-ok 2\n' err=''
check_smoke: PASS: version + invalid-flag + snippet outputs match
```

Output text asserted, never rc alone.

### (5) Metadata gates

```sh
check_metadata: minos …/bin/ch -> '26.0'            (×4: ch, GCStress, both dylibs)
check_metadata: stack …/bin/ch -> 8388608          (×2: ch, GCStress)
check_metadata: header include/ChakraCore.h -> 81416 bytes … byte-identical       (×4)
check_metadata: exports …/bin/libChakraCore.dylib -> 17599 T symbols, required 8/8 present
check_metadata: PASS: minos 26.0 (otool+vtool), 8MiB stacks, 4 headers byte-identical, 8 JsRT exports present
```

### Selftest: negative controls

`zig build check-selftest` (`build.zig:608-615`): exit 0, wall 21s,
28/28 controls PASS across the four scripts:

```sh
check_regression self-test: ALL NEGATIVE CONTROLS BEHAVE   (7: counts/parser/hung-child)
check_native self-test: ALL NEGATIVE CONTROLS BEHAVE       (8: doctored logs/bogus ch/missing compilers)
check_metadata self-test: ALL NEGATIVE CONTROLS BEHAVE     (6: garbage victim/header/parser)
check_smoke self-test: ALL NEGATIVE CONTROLS BEHAVE        (7: silent decoys/hung probe)
```

## B2.4 fresh-checkout proof

Clone (1.3s) from the main tree at `5e8c6f6dd`, then the working B2
delta applied without touching the main tree:

```sh
git clone /Users/ascotti/Documents/code/ChakraCore --branch modernize/baseline /tmp/b2-fresh
# HEAD 5e8c6f6dd, clean; no build-xplat/ build-zigpilot/ zig-out/ .zig-cache/
git diff HEAD > /tmp/b2-delta.patch   # 902 lines, 4 tracked files (made in the main tree)
cd /tmp/b2-fresh && git apply /tmp/b2-delta.patch
# + copy of the 9 untracked B2 files (see manifest; tools/build/__pycache__/ excluded)
for f in <13 B2 files>; do cmp main-tree/$f /tmp/b2-fresh/$f; done  # → 13/13 IDENTICAL
```

Proof commands (all in `/tmp/b2-fresh`, no `build-xplat/` or
`build-zigpilot/` present at any point):

```sh
mise x -- zig build                 # → exit 0, wall 205s
mise x -- zig build check            # → exit 0, wall 62s (1959/0 + all gates)
mise x -- zig build check-selftest   # → exit 0, wall 21s (28/28 controls)
python3 tools/build-contract/verify_b1.py  # → exit 0, wall 216s (14/14 + 1 documented skip)
```

The `verify_b1.py` run includes its own wiped clean-build (exit 0
in 213.4s), `config.h` MD5 `855152b0a1b6da1d8abfef52e1f45f4e`,
`archive-objects` 91/91 via the B2 branch
(`libChakra.Pal.Singular.a`), and 9/9 fail-clearly probes; the
unparseable-`-Dtarget` case is its documented skip (see Known
behaviors). An incremental touch-one-C-TU rebuild exits 0 in 23s.

No CMake/Ninja involvement, three ways:

1. Zero functional references: `grep -rni -e cmake -e ninja
   build.zig tools/build/` finds 13 matches, all inside `//` / `#`
   comments and docstrings (legacy-equivalence citations); filtering
   comment lines leaves zero. The graph's only spawned programs are
   `xcrun` (SDK path), `zig c++`/`zig cc` (538 TU compiles, plus
   the 95 pal-config probes), `python3` (jstoc generator + five
   check gates), and `clang`/`clang++` resolved lazily at
   native-gate time
   (`build.zig:107,273,305,340,525,547-548,556,568,580,590,610`) —
   no `cmake`/`ninja` binary is ever exec'd.
2. Zero configure outputs: no `CMakeCache.txt`,
   `cmake_install.cmake`, `build.ninja`, or `CMakeFiles` anywhere in
   the clone before or after build+check (verified by `find`).
3. Zero processes live: `pgrep -fl 'cmake|ninja'` polled every 2s
   across the full 23s supervised recompile+relink returned nothing
   (0 hits).

Build-log noise note: the fresh-build log contains 69 `failed
command:` blocks — each paired 1:1 with a warnings-only clang
diagnostic (`69` × `warnings generated`, zero `error:` lines; the
3 `error` substring hits are the filename `error.cpp`). The build
exits 0 with every artifact installed.

After build, check, selftest, verify_b1, probes, and the co-install
hide/restore cycle, `git status` in `/tmp/b2-fresh` shows exactly
the applied B2 delta (4 modified + `doc/b2-verification.md` +
`tools/build/`) — no `profile.dpl.*` residue, no header rewrites,
no `native-tests` residue. `/tmp/b2-fresh` (8.8G) is left in place
for reviewers.

## Known behaviors

- A no-op `zig build` exits 0 printing only the `b2 profile:` and
  `pal-config: 95/95 checks match …` lines: the configure phase (all
  gates plus all 95 pal-config probes) runs every time, and the
  install step re-creates the two versioned-dylib symlinks (same
  targets, fresh mtimes). All artifact bytes are unchanged (md5-clean)
  and all regular-file mtimes untouched.
- Touching `bin/ch/DbgController.js` (mtime-only) re-runs the
  generator and recompiles all 8 ch TUs via `addFileInput`
  (verified: `zig build --verbose` shows exactly the 8 `zig c++`
  invocations for `0000`-`0007`, deterministic over two trials) —
  required-correct: every ch TU includes `stdafx.h` → `Debugger.h`
  → `DbgController.js.h` (only `Debugger.cpp` names the
  `controllerScript` variable itself).
- `GCStress -v` segfaults (exit 139, `Segmentation fault: 11`)
  identically on the fresh zig-built, legacy-CMake
  (`build-xplat/bin/GCStress/GCStress`), and pilot
  (`build-zigpilot/bin/GCStress/GCStress`) binaries — pre-existing
  engine behavior, not a B2 regression. The metadata gate still
  asserts the GCStress artifact (minos, 8 MiB stack); no gate runs
  `GCStress -v`.
- An unparseable `-Dtarget` runs the full configure phase before
  failing: `zig build -Dtarget='!!!bogus@@@'` exits 1 printing
  `unable to parse target … UnknownArchitecture`, then the `b2
  profile:` and `pal-config: 95/95` lines, then Zig's deferred
  option-validation error (`build.zig:70-73`). Exit code stays
  correct; `verify_b1.py` records this case as a documented skip.
- The dylib/executable `LC_BUILD_VERSION` stamps sdk 27.0 while the
  gate pins MacOSX SDK 26.5: the stamp comes from the toolchain
  default (no `--sysroot` knob), minos 26.0 holds, and the pilot
  reference stamps 27.0 too (see B2.1 for the legacy-floor
  disambiguation).
- `-Dtarget=native` is rejected (exit 1): `build error:
  unsupported CPU model apple_a14: this profile builds only
  apple_m1 (the aarch64-macos baseline)` — the native CPU resolves
  to the host chip, not the pinned model (`build.zig:89-90`).
- `regenByteCode.py --verify` cannot be run raw as a gate: it
  rewrites the checked-in headers even on success and self-heals
  red-to-green (observed: raw run exit 1 → raw run exit 0, planted
  marker destroyed), so the `verify-bytecode` step runs through
  `tools/build/verify_bytecode_pure.py`, which restores all four
  headers byte- and mtime-identical, exits with the script's
  verdict, and refuses to run without `--verify` (exit 2).
- `verify_b1.py` on a B2 tree reports 14/14 PASS plus the one
  documented unparseable-target skip (the historical B1 label was
  15/15; the archive check now takes its B2 branch).

## B2 file manifest

Tracked files modified by B2 (the `/tmp/b2-delta.patch`, 902 lines):

- `build.zig` — full Mac graph + gates (was: B1 PAL slice)
- `tools/build-contract/verify_b1.py` — B2-aware `archive-objects` only
- `doc/b1-verification.md` — B2 transition note only
- `.gitignore` — `__pycache__/` only

Untracked B2 files (copied into `/tmp/b2-fresh` for the proof;
`tools/build/__pycache__/` excluded):

- `doc/b2-verification.md` — this record
- `tools/build/tables.zig` — source/flag/include tables
- `tools/build/jstoc_run.py` — `DbgController.js.h` generator wrapper
- `tools/build/verify_bytecode_pure.py` — pure bytecode wrapper
- `tools/build/common.py` — shared subprocess helpers
- `tools/build/check_regression.py` — gate (1)
- `tools/build/check_native.py` — gate (3)
- `tools/build/check_smoke.py` — gate (4)
- `tools/build/check_metadata.py` — gate (5)

Zero engine, CMake, test-suite, or other-doc diffs.

## Commands

```sh
mise exec -- zig build                 # incremental build (exit 0)
mise exec -- zig build verify-bytecode # pure bytecode check (exit 0)
mise exec -- zig build check           # five gates: 1959/0 + bytecode + native + smoke + metadata (exit 0)
mise exec -- zig build check-selftest  # negative controls (exit 0)
python3 tools/build-contract/verify_b1.py  # regression gate (14/14 + 1 documented skip, exit 0)
```
