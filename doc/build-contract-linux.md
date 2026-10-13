# Linux build contract (reference profile)

B4.3 — the source/flag/link contract for exactly ONE profile: native
x86_64 Linux, JIT enabled, RelWithDebInfo Test, system ICU, in the B4
container ([b4-linux.md](b4-linux.md)). Source of truth: the B4.1
reference `build.ninja` + `CMakeFiles/rules.ninja` at
`/tmp/chakra-b4/build-xplat` (VM-only; branch `modernize/baseline` at
`1eb0a7bc1`), cross-checked against `ninja -t commands` output.

Conventions: every value cites the reference edge or an observed
command line. Membership tables are GENERATED
(`tools/build/gen_tables_linux.py` → `tools/build/tables_linux.zig`),
so this doc records counts, flag sets, orders and deltas — not 682
pasted paths. Regenerate after any reference-graph change and `git
diff` the tables: drift shows up as a table change. Where a mechanism
is identical to the Mac contract, this doc cites
[build-contract-mac.md](build-contract-mac.md) and records only the
Linux shape.

## 0. Profile and toolchain

Reference profile (same environment as [b4-linux.md](b4-linux.md)):

- Target: `x86_64-linux-gnu`, ELF, native-only (build machine equals
  target; the build rejects any `-Dcpu` override and any non-gnu ABI).
- Configure: `cmake -S /src -B /src/build-xplat -GNinja
  -DCMAKE_BUILD_TYPE=RelWithDebInfo -DCMAKE_CXX_COMPILER=clang++
  -DCMAKE_C_COMPILER=clang`, `DISABLE_JIT:BOOL=OFF` (JIT on),
  `ENABLE_DEBUG_CONFIG_OPTIONS=1`, `CAN_BUILD_WABT` defined.
- Floor: Ubuntu 24.04, glibc 2.39, libicu-dev 74.2, clang 18.1.3
  (reference) / Zig 0.17.0 bundled Clang 22.1.8 (ours).
- Zig build: `zig build` / `zig build check` run natively
  in-container (see §7 for why in-container, not cross-from-Mac).

## 1. Global preprocessor defines

Engine base (22 flags, every non-PAL unit; cf. Mac §1 minus
`DISABLE_JIT=1`, minus ARM64 spellings, plus the Linux/x64 set):

```
-DBIT64=1 -DCAN_BUILD_WABT -DCLANG_HAS_DISABLE_TAIL_CALLS=1
-DENABLE_DEBUG_CONFIG_OPTIONS=1 -DFEATURE_PAL -DHAS_ICU
-DHAS_REAL_ICU=1 -DINTL_ICU=1 -DLINUX64 -DNO_PAL_MINMAX
-DPAL_STDCPP_COMPAT -DPLATFORM_UNIX=1 -DSTACK_ALIGN=16 -DTARGET_64
-DUNICODE -D_AMD64_ -D_CHAKRACOREBUILD -D_M_AMD64 -D_M_X64
-D_SAFECRT_USE_CPP_OVERLOADS=1 -D__LINUX__=1 -D__STDC_WANT_LIB_EXT1__=1
```

PAL base (25 flags): the engine base minus `NO_PAL_MINMAX` /
`PAL_STDCPP_COMPAT`, plus `-DFEATURE_PAL=1 -DLP64COMPATIBLE=1 -DPIC=1
-D_FILE_OFFSET_BITS=64 -D_WIN64=1`.

Per-target extras (each target carries at most one; emitted last via
`target_define` — position is semantically nil, same documented
deviation as the Mac tables' string-fidelity caveat):

| Target | Extra |
|---|---|
| `ch` | `-Dch_EXPORTS` |
| `GCStress` | `-DGCStress_EXPORTS` |
| `ChakraCore` | `-DChakraCore_EXPORTS` |
| `Chakra.Runtime.Language`, `Chakra.Runtime.Library` (C++ and ASM) | `-D_ENABLE_DYNAMIC_THUNKS=1` |

The generator asserts every target's DEFINES equals its base plus only
these extras (unknown extras fail the generation).

## 2. Global compiler flags

### 2.1 C++ (`zig c++`, 651 units)

```
-Werror <25 -Wno-* (root CMakeLists.txt:410-436, same list as Mac)>
-fno-omit-frame-pointer -fdelayed-template-parsing -O3 -g -std=gnu++17
[-fPIE on ch]
-msse4.2 -fasm-blocks -fms-extensions -fwrapv
[-fstack-protector -fno-builtin on PAL]
[-fPIC unless ch (PIE) or GCStress (none)]
```

Notes: `-Werror` is KEPT (reference-exact; the reference build is
warning-free, attested in b4-linux.md §3). No `-mcpu` (the reference
passes no `-march`; baseline x86-64 + `-msse4.2`). No `-stdlib`: `zig
c++` 0.17 uses bundled libc++ headers and links libc++ statically
(`-stdlib=libstdc++` is ignored — probed; see §5.6). Dropped as
cosmetic: `-fdiagnostics-color=always` (all units) and
`-fcolor-diagnostics` (wabt only, lib/wabt/CMakeLists.txt:65). One
per-TU deviation (B4.3): `lib/Jsrt/JsrtHelper.cpp` carries an extra
`-Wno-date-time` — clang 22 emits `-Wdate-time` for its
`__DATE__`/`__TIME__` use (reference clang 18 stays silent with
identical flags), which `-Werror` would promote; the reference has
no such suppression because its compiler never fires the warning.
Scoped to the sole using TU (verified by grep); `-Werror` itself
is untouched.

### 2.2 C (`zig cc`, 22 units — 21 `pal/src/safecrt/*.c` + wabt `opcode-code-table.c`)

```
-O2 -g -DNDEBUG -msse4.2 -fasm-blocks -fms-extensions -fwrapv
[-fstack-protector -fno-builtin on PAL] -fPIC
```

No `-std` (reference-exact; both compilers default to gnu17).

### 2.3 Assembly (`zig cc`, 9 `.S` units)

Same tail as C, plus trailing `-isystem /usr/local/include` (only ASM
carries it — observed on all 5 ASM rules).

## 3. Include directories

Global prefix (identical to Mac): `.`, `lib/Common`,
`lib/Common/PlaceHolder`, `pal`, `pal/inc`, `pal/inc/rt`. PAL prefix:
generated `pal/src/config.h` dir first, then `pal/src`, the global
six, and trailing `pal/src/include`. Per-target extras follow the
prefix in reference order (generator-verified); two carry
generated-dir markers the build substitutes (`build-xplat/bin/ch` →
jstoc output dir, `build-xplat/lib/wabt` → wabt config dir).

ICU headers (documented deviation, B4.3): the reference passes no ICU
`-I` (system clang default-searches `/usr/include`). Explicit-target
`zig cc` (`-target x86_64-linux-gnu`, which this build uses
everywhere) searches only its bundled headers — verified: a
`<unicode/uchar.h>` TU compiles under the native (no-`-target`)
driver and fails under the explicit-target driver — so the build
spells the default dir explicitly as `-isystem <icu.include>` (last,
after all engine `-I`; honors `-Dicu-include`). `-isystem`, not
`-I`: `/usr/include` carries the full C library, and `-I` would
shadow the bundled headers (probed with a poisoned `stdio.h`: `-I`
shadows, `-isystem`/`-idirafter` do not). Dozens of TUs need it
(`ChakraICU.h` via `bin/ch/stdafx.h`, `UnicodeText.h`, the Intl
engine, the PlatformAgnostic PCH).

## 4. Source groups and membership

34 target/language rules, 682 compiles. Counts per rule
(generator-verified; full paths in `tables_linux.zig`):

| Group | CXX | C | ASM |
|---|---|---|---|
| pal | 66 | 21 | 3 |
| codex | 1 | | |
| common | 14 | | |
| core | 21 | | |
| datastructures | 12 | | |
| exceptions | 4 | | |
| memory | 40 | | 1 |
| util | 1 | | |
| backend | 89 | | 2 |
| parser | 26 | | |
| scacore | 9 | | |
| wasmreader | 11 | | |
| rt_base | 34 | | |
| rt_bytecode | 20 | | |
| rt_debug | 25 | | |
| rt_language | 63 | | 2 |
| rt_library | 107 | | 1 |
| rt_math | 2 | | |
| platformagnostic | 11 | | |
| rt_types | 27 | | |
| jsrt | 14 | | |
| jsrt_core | 2 | | |
| wabt | 36 | 1 | |
| static_src | 1 | | |
| dylib | 3 | | |
| ch | 8 | | |
| gcstress | 4 | | |

Total: 651 CXX + 22 C + 9 ASM = 682. Backend (89+2),
WasmReader (11) and wabt (36+1) are new vs the Mac graph.

## 5. Archives, link inputs and generator steps

### 5.1 Excluded from this profile

Nothing reference-built is excluded: the graph covers all 682
reference compiles. Debug/static/Release variants are future
profiles, not silent fallbacks (any non-`test` `-Dprofile` fails).

### 5.2 `libChakraCoreStatic.a`

667 members in reference `ar` edge order (`tables.static_order`):
all compiles except ch (8), GCStress (4) and dylib (3) TUs. Member
order block-starts: PAL, Core, Jsrt(+Core), Backend, Common, Codex,
DataStructures, Exceptions, Memory, Util, Runtime Base/ByteCode/
Debug/Language/Library/Math/Types/PlatformAgnostic, Parser, SCACore,
WasmReader, wabt, `ChakraCoreStatic.cpp` last.

### 5.3 Singular archives

- `libChakra.Pal.Singular.a`: 90 (reference edge order)
- `libChakra.Common.Codex.Singular.a`: 1
- `libChakra.Runtime.PlatformAgnostic.Singular.a`: 11

### 5.4 Generated `DbgController.js.h`

Same edge as Mac: `bin/ch/jstoc.py bin/ch/DbgController.js
controllerScript` via `tools/build/jstoc_run.py`; tracked input of
all 8 ch compiles.

### 5.5 `pal/src/config.h` and wabt `config.h`

PAL config: 95 online checks (`tools/pal-config-linux`), incl.
native run probes — the contract's fail-clearly gate
([pal-config-contract-linux.md](pal-config-contract-linux.md) §5).
First online execution (B4.3, in-container): 95/95 match — the first
run was 94/95 with only HAVE_SYSCTL deviating, which exposed a
probe-link infidelity fixed by prepending the real system link
inputs (contract quirk L14; no contract value changed).
wabt config: fixed-by-profile substitution of
`lib/wabt/src/config.h.in` (11 `#cmakedefine01` + `SIZEOF_SIZE_T=8`;
values in §7 of this doc); template drift fails the build, and the
metadata check re-verifies the output sha256
(`4d365a89…49d9`) against the reference bytes.

### 5.6 Link: `libChakraCore.so`

Reference: 3 dylib objects + `--whole-archive
libChakraCoreStatic.a` + `-licuuc -licui18n` + `-static-libstdc++
-lpthread -ldl` + `--version-script=bin/ChakraCore/
libChakraCoreLib.version`, `-shared -Wl,-soname,libChakraCore.so
-Wl,-undefined,error`. Ours: 3 dylib objects + the 667 static
members joined directly in `static_order` (same initializer order,
no archive semantics — the Mac join-instead-of-force-load pattern)
+ `-L<icu.libdir>` + `-licuuc -licui18n -lpthread -ldl` + version
script + `-Wl,--no-undefined` (the linker's spelling of
`-undefined,error`; probed: a `.so` link with a genuinely-undefined
symbol succeeds by default and fails with the flag, rc=1).
`--whole-archive/--start/end-group` dropped (nothing archived to
group); `-static-libstdc++` dropped: `zig c++` links its libc++
statically by default (our `.so` NEEDED carries no C++ runtime,
verified by the metadata gate), which is the same self-containment
property via the bundled runtime. The `-L` is a documented
deviation (B4.3): `zig c++` searches no system library dirs by
default (probed: `-licuuc` without `-L` fails "unable to find
dynamic system library"), so the reference's default-searched ICU
dir is spelled explicitly (honors `-Dicu-libdir`). Scoped
`-Wl,--no-as-needed` around the ICU pair forces both NEEDEDs (the
reference carries both; without the scope LLD's as-needed drops an
unreferenced one — observed on `ch`, whose objects reference no
icui18n symbol). Compile FLAGS on the reference link line are nil
without LTO and dropped (documented, not silent).

C++ standard library note: the engine compiles against libc++
headers while the reference used libstdc++ headers. This is sound
because the engine/ICU boundary is C-only (ChakraICU.h: ucal,
ucol, udat, udatpg, uloc, ulocdata, unumsys, ustring, unorm2,
upluralrules, uvernum, umachine — no C++ API), so no C++ ABI
crosses into the libstdc++-built ICU; behavior parity is proven by
the checks (§6), and forcing libstdc++ headers would need unproven
driver-flag surgery (`-nostdinc++` drops only the C++ headers —
probed: `<string>` not found while `<stdio.h>` still resolves — so
a libstdc++ `-isystem` stack would have to be hand-built against
the static libc++ link).

### 5.7 Link: `ch`

8 objects + 3 singular archives + `-L<icu.libdir>` +
`-licuuc -licui18n` + `--version-script=bin/ch/ch.version` +
`-lpthread -ldl`, with `-fPIE -pie -Wl,--export-dynamic -rdynamic`
(all reference-exact; the `-L` and the scoped `--no-as-needed` are
the same B4.3 deviations as the `.so`). Our `ch` NEEDED additionally
carries `libpthread.so.0`/`libdl.so.2`, which the reference drops:
ours are genuinely referenced (36 undefined pthread/dl symbols) and
kept by as-needed; glibc ≥2.34 merges them into libc regardless, so
this is loader trivia, not a behavior delta. `ch` dynamic exports
are a superset deviation (B4.3): GNU ld constrains
`--export-dynamic` by the version script (reference: exactly
`OnChakraCoreLoadedEntry`), but LLD has no equivalent spelling —
ours exports 1248 symbols (the required one included), without
`--export-dynamic` nothing is dynamic, and both selective flags
are unsupported linker args (all probed). The surplus is harmless
(nothing depends on hiding); the metadata gate asserts membership,
not the exact set. Co-location: a second `.so` copy installs
beside `bin/ch` (the loader resolves
`<exe-dir>/libChakraCore.so`, as the reference POST_BUILD copy
does).

### 5.8 Link: `GCStress`

4 objects + `--start-group libChakraCoreStatic.a --end-group` +
`-ldl -lpthread`, with `-Wl,--export-dynamic -rdynamic` +
`-pie` (reference-exact except `-pie`, which spells Ubuntu
clang's link-PIE default explicitly: `zig c++` does not
default-pie, and ours came out EXEC without it; reference is DYN).
`-undefined,error` covered by the exe default.

### 5.9 Staged public headers

The same four, unchanged, byte-compared by the metadata check.

### 5.10 Check targets (verification edges, not build inputs)

Legacy-exact commands (from the reference `build.ninja`
CUSTOM_COMMANDs): bytecode `regenByteCode.py --jit --verify
--binary=<ch>` (note `--jit`: the JIT-on profile), regression
`runtests.py --test --binary=<ch>` (JIT-default variants), native
`test_native.sh <ch> RelWithDebInfo <cc> <c++> <src>`; plus
output-asserted smoke and metadata gates. None is reachable from
the default build.

## 6. Validation against reference evidence

- Generator asserts (§4 counts, §1/§2 flag triples, §3 prefixes,
  §5.2/§5.3 membership+order) run on every regeneration; B4.3
  re-ran the generator against the reference `build.ninja`: exit 0,
  output byte-identical to the committed tables. The §1 define sets
  (22/25), §4 per-group counts (682) and §5.2 block order were
  additionally re-verified against the tables by script (one doc
  typo fixed: PAL base is 25 flags, not 24).
- 6-TU pre-flight (exact reference flags, driver swap only):
  Runtime Library/Parser/wabt/ch TUs + PAL/Backend `.S` — 6/6 rc=0,
  empty logs, nonempty objects (method + artifacts re-verified;
  b4-linux.md §8.7). Note the pre-flight ran the native no-`-target`
  driver (default-searches /usr/include); the build's explicit
  target needs the §3/§5.6 `-isystem`/`-L` spellings.
- Link mechanism: `--version-script` + `-soname` accepted by our
  real links; `-Wl,--no-undefined` probed effective both ways (a
  `.so` link with a genuinely-undefined symbol succeeds by default,
  fails rc=1 with the flag; exe links fail by default);
  `-stdlib=libstdc++` probed ignored; `zig ar` present (rc=0).
  `DllMain` is export #1 of the committed set and defined in the
  real link: the .so links clean under `--no-undefined`, so
  strictness needs no workaround.
- Metadata gate GREEN (`zig build check-metadata`, rc=0): .so
  exports 236/236 exact, ch exports membership (1248 ⊇ 1; LLD
  cannot express GNU ld's constrained exe export — §5.7),
  SONAME/NEEDED mechanics, wabt sha256 exact, pal markers, staged
  headers byte-identical, archives 667/90/1/11.
- Behavior parity (regression/bytecode/native) is graded in
  [b4-linux.md](b4-linux.md) §8.6 — BLOCKED on the root-caused
  qemu-user engine-init abort (guest stack mapped above PAL's
  128TB ceiling; legacy binary aborts identically).

## 7. Reference quirks (recorded as-is)

- **Q1** — ChakraCore target carries `-Ibin/ChakraCore/
  -fvisibility=hidden` (a flag leaked into an include dir).
  Replicated string-faithfully; nonexistent, ignored by clang.
- **Q2** — GCStress carries absolute `-I/lib/Common` and
  `-I/lib/Common/Memory` (nonexistent, ignored). Replicated
  exactly (absolute spelling).
- **Q3** — ChakraCore's `-fPIC` sits mid-line (after `-std=gnu++17`)
  while every other engine target carries it late. NOT replicated:
  the flag is order-independent; emitted late uniformly.
- **Q4** — Mid-list extras (`ChakraCore_EXPORTS`,
  `GCStress_EXPORTS`) are emitted last via `target_define` (same
  documented deviation as the Mac tables).
- **wabt values** (fixed-by-profile): `HAVE_ALLOCA_H`,
  `HAVE_UNISTD_H`, `HAVE_SNPRINTF`, `HAVE_SYSCONF`,
  `HAVE_SSIZE_T`, `HAVE_STRCASECMP`, `COMPILER_IS_CLANG` = 1;
  `HAVE_WIN32_VT100`, `COMPILER_IS_GNU`, `COMPILER_IS_MSVC`,
  `WITH_EXCEPTIONS` = 0; `SIZEOF_SIZE_T` = 8.
- **In-container, not cross-from-Mac**: the reference profile is
  native-only (run probes, system ICU/glibc); the container is the
  same image the baseline was built in, so inputs match by
  construction — a Mac cross would need a Linux sysroot + ICU
  replica with no execution path.

## 8. Open uncertainties

- Regression/bytecode/native counts: never observed on Linux
  (BLOCKED, §6) — no baseline recorded anywhere; Mac's 1959/0 is
  not the Linux count. Recording is per variant (interpreted,
  dynapogo) on the first green run.
- libc++-headers vs libstdc++-headers behavioral equivalence is
  still assumed-sound (C-only ICU boundary) and now
  metadata-gated — but no JS has executed under the zig-built
  engine, so behavior parity is unproven.
- `zig ar` (LLVM) vs GNU `ar qc` archive-byte differences are not
  analyzed (member counts asserted by the metadata gate;
  construction order comes from the generated tables; bytes not
  compared).
- Binary size (+44–102% vs reference, all debug info; ch
  roughly doubles at +101.5%, the rest +44–57%) is unattributed
  in detail (apparent DWARF-format verbosity; uninvestigated)
  and ungated.
- Duplicate C++ codegen flags (deferred, behavior-neutral):
  `tables.warnings` carries 30 entries (25 `-Wno-*` + 5 codegen)
  while `linux.zig` re-emits the same 5, so all 651 C++ lines
  carry them twice (identical values, last-wins). Filtering the
  5 in the generator is the correct fix but changes every C++
  command line, needing a full ~80min rebuild to re-verify for
  zero behavior gain — deferred deliberately. The emission
  comment states the 30.
