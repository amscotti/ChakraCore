# B4 Linux Baseline (x64 under qemu-user emulation, JIT enabled)

B4 step 1: Docker Linux environment + legacy x64 build **with JIT** as the
behavior baseline for the Linux Zig port. Backend, WasmReader and WABT have
never been built in this project before this milestone.

> Status: environment FEASIBLE for configure+build, BLOCKED for execution:
> full x64/JIT build PASSES (690/690, 78m07s), but `ninja check` cannot run —
> qemu-user 8.2.2 crashes on any guest open of `/proc/self/maps`, which `ch`
> needs during runtime init. All timings measured 2026-10-12 (EDT).

## 1. Environment contract

| Item | Value |
|---|---|
| Dockerfile | `tools/docker/Dockerfile.ubuntu-x64` (landed with this doc) |
| Base image | `ubuntu:24.04`, digest `sha256:534baea6a22c03a63003dbc8dbe78fe34bc0d7e595d9a9dc9834884ff530eb55` |
| Built image | `chakracore/b4-linux-x64`, id `sha256:a03c82f0e46b7fe533ea8220464fc92c2dc0bee71dba1ce4e800fd7b84ad09e4` |
| clang | 18.1.3 (`clang` pkg `1:18.0-59~exp2`), target `x86_64-pc-linux-gnu` |
| cmake | 3.28.3 (`3.28.3-1build7`) |
| ninja | 1.11.1 (`1.11.1-2`) |
| python3 | 3.12.3 (`3.12.3-0ubuntu2.1`) |
| libicu-dev | 74.2 (`74.2-1ubuntu3.1`, amd64) |
| build-essential | `12.10ubuntu1` (gcc/g++ 13.2.0, binutils 2.42) |
| Host | ARM64 Mac (macOS 26.6), Docker via lima VM (`docker`, limactl 2.2.0, Ubuntu 24.04.2, kernel 6.8.0-139, 6 vCPU, 8 GiB) |
| Arch strategy | x64 execution under qemu-user emulation (`qemu-x86_64 8.2.2`, binfmt_misc `POF` flags with fix-binary) — emulated, not native; see §2 |
| Docker daemon | **rootful** system dockerd 28.5.1 in the lima VM (see §2) |
| qemu-user | `qemu-user-static 1:8.2.2+ds-0ubuntu1.18`, handler `/usr/libexec/qemu-binfmt/x86_64-binfmt-P` (symlink to `/usr/bin/qemu-x86_64-static`), flags `POF`. DEFECT: deterministic crash on guest open of `/proc/self/maps` — see §4. |

Image choice: legacy CI (`azure-pipelines.yml`, CMake job) runs its primary
Linux legs (Debug, NoJit, RelWithDebInfo, Release) on `ubuntu-24.04` with deps
`ninja-build clang libicu-dev`, configuring with
`cmake -GNinja -DCMAKE_BUILD_TYPE=<type> -DCMAKE_CXX_COMPILER=clang++
-DCMAKE_C_COMPILER=clang ..` then `ninja` / `ninja check`. The Dockerfile
installs exactly those CI deps plus the base-toolchain packages
(`build-essential`, `cmake`, `python3`) that GitHub-hosted runners provide
outside the deps line. A `ubuntu-22.04` leg exists in CI for
RelWithDebInfo/Release only; 24.04 covers the full matrix, so it is the B4
floor. Versions above are the apt-resolved values recorded at build time.

Source snapshot: branch `modernize/baseline` at
`1eb0a7bc168141855ad53dd3284bdcf853a7c708`, extracted with
`git archive HEAD` to VM-native ext4 (`/tmp/chakra-b4`) — the virtiofs
`/Users` mount is readable from the VM but too slow for build I/O.
Equivalent reproducible extraction (run in the VM; the author's exact
command was not recorded, but this was verified during B4.1 review to
reproduce the tree modulo build-generated files and the later-added
Dockerfile):
`git -C /Users/ascotti/Documents/code/ChakraCore archive HEAD | tar -x -C /tmp/chakra-b4`
(run after `mkdir -p /tmp/chakra-b4`).
Legacy-input drift vs the F0 engine baseline `97e68c917`: only 4 test files
(`test/native-tests/test-static-*/sample.cpp`, `test_native.sh`,
+27/−7) from B3; `lib/`, `pal/`, `bin/`, `CMakeLists.txt`, `build.sh`
identical. The image does NOT bake in the source; it is mounted at `/src`.

## 2. Emulation feasibility (measured)

- Rootless dockerd (the VM default, `lima-docker` context) **cannot** run
  amd64 containers here: `exec /usr/bin/uname: exec format error`, even with
  qemu-user-static installed, binfmt_misc registered with fix-binary (`F`),
  and `--privileged` / `seccomp=unconfined` / `apparmor=unconfined`.
  binfmt dispatch itself works in the VM (also inside a plain userns), so the
  failure is specific to the rootless container path. Verdict: rootless is
  BLOCKED for B4; this is an environment limitation, not an engine finding.
- Rootful system dockerd in the same VM (`sudo systemctl start docker`,
  socket `/var/run/docker.sock`) runs amd64 containers correctly
  (`uname -m` → `x86_64`). All B4 stages below use it. Starting it is
  additive: the existing rootless daemon and its containers are untouched.
- Emulation speed probe (single small `-O3` C++ TU in-container):
  author's run **4.2s** compile (run observation, no retained log —
  see §5 note), roughly 5–8× slower than native (estimate; no native
  baseline was measured). Runtime execution substantiated by a
  corroborating re-run during B4.1 review in a throwaway rootful
  amd64 container (since removed): `clang++ -O3 -o /w/hello
  /w/hello.cpp` (5-line `<cstdio>` hello with a 2M-iteration loop,
  1.6s wall incl. container startup) then `/w/hello` → exit 0,
  `hello sum=1999999000000`, 0.2s; output binary ELF x86-64,
  dynamically linked. Full-build verdict below.

Host-side one-time setup (lima `docker` VM, persists across reboots via
systemd binfmt.d + the installed package):

```sh
limactl shell docker -- sh -c "sudo apt-get update && sudo apt-get install -y qemu-user-static binfmt-support"
limactl shell docker -- sudo systemctl start docker   # rootful daemon
```

## 3. Stage record

All container commands run as (VM shell):

```sh
sudo docker run --rm --platform linux/amd64 -v /tmp/chakra-b4:/src \
    chakracore/b4-linux-x64 <cmd...>
```

### Stage 1 — env + deps (PASS)

Run in the VM from the virtiofs-mounted repo root
(`/Users/ascotti/Documents/code/ChakraCore`; the mount is read-only,
which suffices for a build context):
`sudo docker build --platform linux/amd64 -f tools/docker/Dockerfile.ubuntu-x64 \
-t chakracore/b4-linux-x64 tools/docker` → image id above, **81s**,
including the in-Dockerfile smoke gate (compiler/cmake/ninja/python/icu
versions).

Rebuild-from-Dockerfile proof: PASS (2026-10-12 11:50–11:52 EDT).
Byte-identical copy of the Dockerfile (`diff` clean) to VM-native
`/tmp/b4rebuild`, then `sudo docker build --platform linux/amd64 --no-cache \
--progress=plain -t chakracore/b4-linux-x64-proof /tmp/b4rebuild` → **76s**,
image `sha256:004ef83bb9b29dca0ad222077ebe854344a5684a6b2f07932d2e2cde03227d73`.
All five recorded dep versions reproduced exactly (zero apt drift):
clang `1:18.0-59~exp2`, cmake `3.28.3-1build7`, ninja-build `1.11.1-2`,
python3 `3.12.3-0ubuntu2.1`, libicu-dev `74.2-1ubuntu3.1`.

### Stage 2 — configure WITHOUT DISABLE_JIT (PASS)

```sh
cmake -S /src -B /src/build-xplat -GNinja \
  -DCMAKE_BUILD_TYPE=RelWithDebInfo \
  -DCMAKE_CXX_COMPILER=clang++ -DCMAKE_C_COMPILER=clang
```

Result: clean configure, **130s**. `DISABLE_JIT:BOOL=OFF`;
`Backend`, `WasmReader`, `wabt` subdirectories all in the graph
(172 matching `build` edges of 967 total, via `grep '^build '` over
`build.ninja` with a case-sensitive `Backend|WasmReader|wabt` match;
1135 total targets via `ninja -t targets all | wc -l`, including
phony/custom edges — the 690 below is the executed-step count from
the build log, a different number). PAL feature probes: 12
expected-absent failures (BSD/Mach/Solaris-only APIs such as
`kqueue`, `vm_allocate`, `directio`), all non-fatal
`check_function_exists` probes — configure green (full log kept at
`/tmp/chakra-b4/build-xplat/CMakeFiles/CMakeConfigureLog.yaml`
in the VM, not committed).

### Stage 3 — full build (PASS)

```sh
ninja -C /src/build-xplat
```

Container `b4-build`, detached (accidentally pruned post-verification
during B4.1 review — see §6; key facts attested in the reviews):
started 2026-10-12 09:33:46 EDT, finished 10:51:53 EDT, exit code 0.
Final lines `[689/690]` and
`[690/690] Linking CXX shared library bin/ChakraCore/libChakraCore.so`;
690/690 bracketed steps in the log. Only diagnostics: two ld notes
(`debugbreak.S.o`, `JavascriptFunctionA.S.o` missing `.note.GNU-stack`,
"behaviour is deprecated") — non-fatal, build green.

Artifact: `/src/build-xplat/ch`, 2756104 bytes, `ELF 64-bit LSB pie
executable, x86-64, ... with debug_info, not stripped` (VM `file`).

### Stage 4 — checks (BLOCKED: qemu defect, not an engine result)

Legacy Linux check for this configuration (`test/CMakeLists.txt:33-45,48-68`,
confirmed live via `ninja -t targets`: `test/check` ← `bytecode-check` +
`regression-check` + `native-check`):
`bytecode-check` + `regression-check` (interpreted/dynapogo variants via
`runtests.py --test`) + `native-check` (`test_native.sh`), all under
`ninja check`:

```sh
ninja -C /src/build-xplat check
```

This was never executed: every check path runs the just-built `ch`, and
`ch` cannot execute any script in this environment. `ch
test/Basics/hello.js` dies in qemu before running a line of JS (see
§4.1; run it under `timeout 60` — the aftermath usually hangs rather
than exiting).
Zero tests ran; there are no counts to record — BLOCKED, not 0/N.

## 4. Legacy baseline results

| Check | Command | Result |
|---|---|---|
| build | `ninja -C /src/build-xplat` | PASS, 690/690, exit 0, 78m07s |
| arg-parse | `ch --help` | PASS (fast, exit 0) — execution of trivial paths works |
| smoke | `ch test/Basics/hello.js` | BLOCKED (qemu crash, §4.1) |
| bytecode | `ninja -C /src/build-xplat check` (bytecode-check) | BLOCKED (needs `ch`) |
| regression | same (regression-check, interpreted/dynapogo) | BLOCKED (needs `ch`) |
| native | same (native-check) | BLOCKED (needs `ch`) |

### 4.1 Root cause: qemu-user crashes on guest open of `/proc/self/maps`

FINDING (environmental — no Chakra code is implicated):

- Guest-syscall trail (`QEMU_STRACE=1`) of `ch hello.js`: the last guest
  call is `openat(AT_FDCWD, "/proc/self/maps", O_RDONLY|O_CLOEXEC)`,
  then `x86_64-binfmt-P: QEMU internal SIGSEGV {code=MAPERR, addr=0x20}`
  (qemu null-deref, not a guest fault). Aftermath is racy: immediate
  SIGSEGV (rc=139, "core dumped") or a 0%-CPU deadlock (main thread in
  host `sigsuspend`, second thread in uninterruptible `futex WAIT`).
- Chakra-free repro: `timeout 60 cat /proc/self/maps` in the same
  image crashes identically, deterministically (4/4; `timeout`
  required — the aftermath usually hangs rather than exiting). `cat`
  of `/proc/version`,
  `/proc/self/status`, `/proc/self/cmdline`, `/proc/cpuinfo` all succeed
  (exit 0, correct content) — ONLY the `maps` open crashes. Other guest
  syscalls (incl. hundreds of `openat`s during compilation) are fine.
- `ch --help` works, so the `maps` open happens after arg parsing, in
  runtime-init (before any script runs). Chakra/PAL sources contain no
  `/proc/self/maps` reader and no direct-syscall `open` (grep over
  `lib/ pal/ bin/`; only
  `/proc/self/exe` readlink, `/proc/self/status` open, debugger paths),
  so the reader is a shared-library init path (`libicuuc`, `libicui18n`,
  `libstdc++`, `libm`, `libgcc_s`, `libc` per `readelf -d`).
- Attempted workarounds, all exhausted 2026-10-12 11:15–11:50 EDT:
  LD_PRELOAD `open/open64/openat/openat64` + `fopen/fopen64` →ENOENT shim
  (verified effective: preloaded `cat /proc/self/maps` survives with
  ENOENT; `ch`'s open bypasses all six interposed symbols, i.e. an
  intra-libc/hidden path); newer qemu via `multiarch/qemu-user-static`
  (pulled amd64-only copy, self-bricked the handler mid-install —
  restored via package reinstall) and `tonistiigi/binfmt` (skipped as
  "already registered", then registered a nonexistent interpreter path —
  reverted); Debian trixie `qemu-user-static` 10.x (restructured to
  dynamically-linked `qemu-user`, dependency spiral — abandoned).
- Environment left intact and verified: handler is the original Debian
  `qemu-x86_64` (`POF`, `qemu-user-static 1:8.2.2+ds-0ubuntu1.18`),
  `uname -m` → `x86_64`. Workaround scratch: B4.1 review found
  leftovers (`/tmp/qemutest/`, `/tmp/dockerfile.wt`, `/tmp/m1.log`,
  `/tmp/m1b.log`, image `chakracore/b4-linux-x64:rebuild-nocache`,
  `preload-probe` container) — all removed during review (the
  container by the reviewer's prune). No `.deb`s, shim sources, or
  foreign binfmt images remain. (Temporal annotation: true at
  B4.1 review time; superseded later — a `tonistiigi/binfmt`
  image was subsequently observed in the rootful daemon, recorded
  in [b4-gate.md](b4-gate.md) §1.3.)

Consequence: no `ch`-executing baseline (smoke or `ninja check`) is
obtainable under this qemu. The build itself is a valid artifact
(x86-64 ELF, JIT enabled); it just cannot be *run* here.

## 5. Timings

Stage wall-times are run observations without retained log artifacts
(the `b4-build` log was pruned post-verification — see §6); the
09:33:46–10:51:53 build window and exit 0 are attested in the B4.1
reviews.

| Stage | Wall time |
|---|---|
| Image build (amd64, emulated apt) | 81s |
| Image rebuild proof (`--no-cache`) | 76s |
| cmake configure (RelWithDebInfo, JIT on) | 130s |
| Full `ninja` build (690/690, JIT on) | 78m07s (09:33:46–10:51:53 EDT) |
| `ninja check` | BLOCKED (0 tests ran) |

## 6. Open uncertainties / follow-ups

- `ninja check` has never run for this baseline: exact Linux/JIT test
  counts TBD once an execution-capable environment exists. Mac's 1959/0
  (no-JIT) is NOT the Linux acceptance count ([modernization-plan.md](modernization-plan.md) §B4).
- Execution options for B4's next phase: (a) native x64 runner
  (no emulation — preferred for timing fidelity); (b) newer qemu-user
  (whether 9.x/10.x fixes the `maps` crash is UNTESTED — the attempts
  above failed logistically, not empirically); (c) arm64-native
  container (changes the arch the baseline covers).
- Whether the qemu `maps` crash also affects the rootless daemon path
  (moot — rootless is already BLOCKED for amd64, §2).
- Rootless-docker foreign-arch failure root cause (likely AppArmor/userns
  interaction on Ubuntu 24.04); rootful is the documented workaround.
- VM scratch retained: `/tmp/chakra-b4` (unmodified `git archive`
  sources plus in-tree `build-xplat/`; 9 stray qemu core dumps
  removed during review), proof image
  `chakracore/b4-linux-x64-proof`, `/tmp/b4rebuild` (byte-identical
  Dockerfile). The exited-0 `b4-build` container (build log) was
  accidentally pruned post-verification during review; key facts
  (exit 0, timestamps, 690/690 tail, diagnostics scan) are attested
  in the B4.1 reviews, and build artifacts survive in `build-xplat/`.
  The image and source tree are reproducible from the Dockerfile +
  `git archive` above; the qemu-user/binfmt handler is host-VM
  provisioning outside the image, so execution results are not
  reproducible from the image alone.

## 7. B4.2 toolchain pilot (appended 2026-10-12; reviewed §§1–6 above untouched)

Why here rather than a new doc: this file is the single reviewed
record of the B4 environment, and the pilot is environment/toolchain
evidence about that same container, not a new subject.

Result: **5/5 compiles PASS on both compilers, 1/1 link PASS on both.**
`zig c++` (Zig 0.17.0) compiles a PAL/Backend/WasmReader slice with the
exact reference flags and links the slice, with zero warnings on both
sides. No built artifact was executed (all inspection is
compile/link/config-only, per the §4.1 constraint).

### 7.1 Toolchain

- Zig 0.17.0 for the pilot only: official tarball
  `https://ziglang.org/download/0.17.0/zig-x86_64-linux-0.17.0.tar.xz`
  (57,332,648 bytes), downloaded inside the throwaway `b4-pilot`
  container, sha256
  `1cbe9df9f27e6b78d14ccbca43b6703a404ef79ef1c463de901d7f088d4e2026`
  verified against the published
  `https://ziglang.org/download/index.json` on two independent routes
  (host curl and in-container fetch — identical), `zig version` →
  `0.17.0`, `zig env` target `x86_64-linux.6.8...6.8-gnu.2.39`
  (bundled glibc 2.39 = system glibc 2.39).
- The image lacks CA certificates and curl, so the throwaway
  container (only) got `ca-certificates curl xz-utils` via apt; the
  image and `tools/docker/Dockerfile.ubuntu-x64` are untouched.
- Reference compiler: system clang++ 18.1.3 (same as §1). Note Zig
  0.17.0 bundles Clang 22.1.8: the pilot compares toolchain
  *capability* on identical flags, not frontend identity.

### 7.2 Method

`build-xplat/` has no `compile_commands.json` (B4.1 configured without
`-DCMAKE_EXPORT_COMPILE_COMMANDS`), so the five reference commands
were reconstructed VM-natively from `build-xplat/build.ninja` +
`CMakeFiles/rules.ninja` (rule template + per-edge `DEFINES`/
`INCLUDES`/`FLAGS`, ninja variable expansion). Each TU yields a
baseline/pilot pair differing ONLY in argv[0]
(`/usr/bin/clang{,++}` vs `zig {cc,c++}` — verified by normalized
diff); only `-o`/`-MT`/`-MF` are redirected from the read-only `/src`
tree to `/work` (documented deviation, applies equally to both
sides). Reconstruction cross-checked on one TU against `ninja -t
commands` output: identical modulo output paths and log redirection.
Reused [zig-pilot](../tools/zig-pilot/README.md) conventions: output
isolation (per-compiler objects, never into reference paths),
nonempty-object-plus-dependency success (not just rc=0), stale-output
removal before each edge.

PASS criteria (fixed before running): (1) exit 0 both compilers;
(2) nonempty object, valid ELF64 relocatable x86-64; (3) no NEW
warnings vs the baseline — baseline method is the reference clang++
run itself, and `-Werror` is active on Linux, so rc=0 already proves
zero warnings (for the C TU, the empty log); logs retained for
inspection; (4) link step exit 0 both sides with a valid ELF64
shared x86-64 object.

Re-run commands (reconstructed after the fact — the launch and
invocation lines were not retained; see the honesty note below). VM
shell unless marked:

```sh
# 1. Launch the throwaway pilot container (rootful daemon, §3 pattern):
sudo docker run -d --platform linux/amd64 --name b4-pilot \
  -v /tmp/chakra-b4:/src:ro -v /tmp/b4-pilot:/work \
  chakracore/b4-linux-x64 sleep infinity
# 2. Container-only prerequisites (the image lacks CA certs, curl, xz):
sudo docker exec b4-pilot sh -c "apt-get update && apt-get install -y ca-certificates curl xz-utils"
# 3. Zig 0.17.0 tarball into /work, hash-checked (§7.1):
sudo docker exec b4-pilot curl -o /work/zig-x86_64-linux-0.17.0.tar.xz \
  https://ziglang.org/download/0.17.0/zig-x86_64-linux-0.17.0.tar.xz
echo "1cbe9df9f27e6b78d14ccbca43b6703a404ef79ef1c463de901d7f088d4e2026  /tmp/b4-pilot/zig-x86_64-linux-0.17.0.tar.xz" \
  | sha256sum -c -
sudo docker exec b4-pilot tar -xf /work/zig-x86_64-linux-0.17.0.tar.xz -C /work
# 4. VM-native staging (prerequisite, mechanism not retained — see note):
#    extract.py reads /tmp/chakra-b4 (read-only) and writes the ten paired
#    run-*.sh scripts to /tmp/b4-pilot/; run-all.sh (kept at
#    /Users/ascotti/b4-pilot-tmp/run-all.sh) goes alongside them.
# 5. Run the pilot:
sudo docker exec b4-pilot bash /work/run-all.sh
# 6. Teardown after verification:
sudo docker rm -f b4-pilot
```

Honesty note — verified vs assumed. Verified from retained
evidence: the image (`chakracore/b4-linux-x64`); the in-container
endpoints `/src` (every run script reads reference sources there)
and `/work` (all script I/O; the VM-side `/tmp/b4-pilot/` holds
root-owned outputs); the container name `b4-pilot` (removed by name
after verification, §7.3); the apt set, tarball URL, byte count, and
sha256 (§7.1); `run-all.sh` (identical host and VM copies); the
12:36–12:38 artifact timestamps consistent with one `run-all.sh`
pass. Assumed (reconstructed, not retained): the launch flags (`-d`,
`sleep infinity`, exec-driven — no launch line survives); `:ro` on
`/src` (inferred from "the read-only `/src` tree" above); the exact
curl/tar/sha256 spellings (the sha256 value itself is verified);
`bash /work/run-all.sh` as the invoking line; the file-transfer
mechanism for step 4; the teardown spelling.

### 7.3 Evidence

Slice (small sources; header inclusion dominates cost): one C TU, two
PAL C++ TUs, one Backend (JIT) TU, one WasmReader TU — the last two
cover previously unbuilt subsystems. Link step: `-shared` over the
five same-toolchain objects (undefined engine symbols allowed, as in
a real shared link). Times are per-command wall (`date +%s.%N`
deltas), whole run 136.0s (well under the 30-min box):

| TU / step | clang++ rc / size / time | zig rc / size / time |
|---|---|---|
| `pal/src/safecrt/memcpy_s.c` | 0 / 4848 B / 1.0s | 0 / 4464 B / 1.3s |
| `pal/src/cruntime/finite.cpp` | 0 / 17616 B / 2.7s | 0 / 8072 B / 2.2s |
| `pal/src/misc/utils.cpp` | 0 / 14920 B / 2.0s | 0 / 13456 B / 2.3s |
| `lib/Backend/IntConstMath.cpp` | 0 / 2030824 B / 31.3s | 0 / 2999912 B / 31.3s |
| `lib/WasmReader/WasmSection.cpp` | 0 / 1979520 B / 30.2s | 0 / 2919008 B / 29.3s |
| `-shared` link of the five | 0 / 2206656 B / 0.7s | 0 / 4457200 B / 0.7s |

Warnings: all 12 logs (10 compiles + 2 links) are 0 bytes —
zero warnings/errors on both toolchains. Objects: all 10 `.o`
verified ELF relocatable x86-64, both `.so` ELF shared x86-64
(VM-native ELF header parse), 10/10 `.d` depfiles produced.
Evidence kept in the VM at `/tmp/b4-pilot/` (paired `run-*.sh`
scripts, `run-all.sh`, 12 logs, objects, depfiles,
`toolchain-versions.txt`, `targets.txt`, `cmd-utils.txt`); the
throwaway `b4-pilot` container was removed by name after
verification. Extraction/verification scripts kept on the host at
`/Users/ascotti/b4-pilot-tmp/` (`extract.py`, `run-all.sh`,
`verify_contract.py`), outside the repo.

Environment notes: in-container `grep` proved unreliable under qemu
(intermittent SIGSEGV, including one solo crash over 12 empty
files); `ninja` itself is fine (`ninja -t targets all` → rc=0,
113149 bytes), and all text/ELF inspection was redone with VM-native
tools. Object sizes differ between toolchains (notably the two large
TUs); sizes are recorded, not explained — the pilot claims
capability, not byte-identity.

### 7.4 Companion deliverable

The Linux PAL configuration contract derived from this same
environment's `config.h`, `CMakeCache.txt` and
`CMakeConfigureLog.yaml` is specified in
[pal-config-contract-linux.md](pal-config-contract-linux.md) and
implemented as `tools/pal-config-linux/` (`contract.zig`,
`probes.zig`, `check.zig`): 95 checks (62 compile-checked, 32
runtime-verified, 1 tail-calls attribute) plus 33 fixed-by-target
settings. Offline render of the contract tables is byte-identical to
the reference header (4839 bytes, MD5 `f73cd5b2…`), and both entry
files (`contract.zig`, `check.zig`) compile clean under the
in-container Zig 0.17.0
(`build-obj`, exit 0). The online (probing) check path has never
executed here; first native execution belongs to the Linux build
milestone. Recorded run-probe exit codes were observed under
qemu-user emulation and must be re-verified on native x64 (contract
doc §6 S1).

## 8. B4.3 Linux Zig build (appended 2026-10-12; reviewed §§1–7 above untouched)

Result: the Linux Zig build is GREEN — 682/682 compiles, all
archives/links/installs, 95/95 PAL-config checks, metadata gate
PASS. Execution-dependent checks are BLOCKED (0 ran) by a
root-caused qemu-user address-layout abort (§8.2) that affects the
legacy binary identically; trivial execution (version/help) and all
non-execution checks pass for real. No Linux behavior baseline
exists yet; recording one needs an execution-capable host.

Authorship: the B4.3 graph, tables generator, PAL-config wiring and
check scripts were built by a predecessor agent on 2026-10-12; a
finisher agent fixed the remaining build-script errors (5 sites in
`tools/build/linux.zig` / `tools/pal-config-linux/*` plus 2
probed default-search gaps), root-caused the execution abort below,
ran the build and checks, and wrote this section. Every number
below was observed by one of the two; finisher re-verifications are
marked.

### 8.1 Environment delta vs §§1–4

- binfmt handler is now qemu-user **10.2.3**
  (`/usr/local/bin/qemu-x86_64-10.2.3`, `qemu-x86_64 version 10.2.3
  (v10.2.3)`, 8137328 bytes, flags `POF` — same flags as §1). The
  8.2.2 handler from §1 is no longer registered. Install method was
  not recorded (predecessor spike, 2026-10-12 ~13:22 EDT); only the
  binary's presence and version are verified.
- `timeout 60 cat /proc/self/maps` → **rc=0**, 24 lines (same
  image, `chakracore/b4-linux-x64`). The §4.1 qemu SIGSEGV on
  `maps` open is GONE under 10.2.3.
- Trivial execution works: `ch --help` rc=0, `ch -v` rc=0
  (`ch version 1.13.0.0-beta`) — on the legacy §3 binary AND the
  B4.3 zig-built `ch` alike.
- Full execution does NOT work: any script aborts in engine init
  (§8.2). The environment went from "cannot execute" (§4.1) to
  "executes trivial paths, aborts in engine init" — partial
  restoration, not a regression to 8.2.2 behavior.
- Newly observed qemu differences (guest strace of the §8.2 abort
  run): `set_robust_list` and `rseq` fail ENOSYS (tolerated — init
  continues past them); `mmap(NULL,0,…)` returns SUCCESS (native
  Linux returns EINVAL) — an observed difference, not the abort
  cause (see §8.2).

### 8.2 Execution verdict: engine-init abort (root-caused)

- `ch test/Basics/hello.js` → **rc=134 (SIGABRT), empty stdout**,
  on the legacy §3 binary (traced) and the zig-built `ch` (same
  signature; §8.6). No script line runs.
- Root cause (guest abort, deterministic): `ch` `dlopen`s
  `libChakraCore.so`, whose static initializer runs
  `X64WriteBarrierCardTableManager::OnThreadInit()` →
  `OnSegmentAlloc(stackEnd, …)`, which finds `stackEnd >=
  lpMaximumApplicationAddress` and calls
  `Js::Throw::FatalInternalError()` → `abort()`
  (`lib/Common/Memory/RecyclerWriteBarrierManager.cpp:99-103`).
- Method: `LD_PRELOAD` interposer printing a backtrace from
  `abort()`/`raise()`, symbolized with `addr2line` (engine ships
  debug info): `main → LoadChakraDll (ChakraRtInterface.cpp:65) →
  LoadLibraryExA → dlopen → _GLOBAL__sub_I_… → OnThreadInit →
  OnSegmentAlloc → FatalInternalError (Throw.cpp:73) →
  ReportFatalException (ReportError.cpp:39) → PROCEndProcess
  (process.cpp:1303) → abort()`. All guest syscalls succeed up to
  the abort (`maps`/`status` reads complete). Line-note (all in
  `lib/Common/Memory/RecyclerWriteBarrierManager.cpp`): the
  source-level fatal path is the `OnSegmentAlloc(stackEnd, …)`
  call at `OnThreadInit:85`, tripping the `:99-103` ceiling
  check. The backtrace frame above `OnSegmentAlloc` symbolizes
  to `:398` (identically on both binaries) — but `:398` is the
  `CONFIG_FLAG(StrictWriteBarrierCheck)` line inside
  `RecyclerWriteBarrierManager::ToggleBarrier`, a sibling call
  from `OnThreadInit:87` (`ENABLE_DEBUG_CONFIG_OPTIONS`): it is
  the optimized frame's line attribution, not the
  `OnSegmentAlloc` call site.
- Why the check trips: PAL hardcodes `lpMaximumApplicationAddress`
  to the x86-64 Linux ABI ceiling of 128TB
  (`pal/src/misc/sysinfo.cpp:104`, `MAX_PROCESS_VA_SPACE_LINUX`),
  but qemu-user 10.2.3 maps the guest stack and `mmap`s at
  ~0xF6AAxxxxxxxx (~247TB): observed stack `0xF6aa0edff160`, mmap
  base `0xF6aa0ae00000` (guest strace). The card-table manager
  cannot cover such addresses by design ("How did this happen?",
  `:101`) and fatals.
- Environmental, not an engine defect: the same source profile is
  green on native x64 CI (`azure-pipelines.yml` ubuntu-24.04 legs,
  §1); native Linux maps stack/mmap below 128TB. The aborting
  binary traced above IS the legacy cmake-built `ch` — the Zig
  build is not implicated (it aborts identically because the cause
  is the address layout, not the toolchain).
- Set aside: the `mmap(NULL,0)`-success hypothesis (predecessor's
  `mmap0` probe in `/tmp/b4-linkprobe`): an observed qemu
  difference, but the backtrace shows the fatal is the
  deterministic stack-address check, reached with all syscalls
  succeeding.
- Consequence for grading: every leg that executes JS is BLOCKED
  (0 ran, not 0/N); legs that only parse args or inspect artifacts
  run for real. A qemu layout workaround (guest-base/stack
  placement flags) is UNTRIED — see §8.8.

### 8.3 Containers, mounts and the sync rule

- `b4-zigbuild` (the build container): `/src` ←
  `/tmp/chakra-b4-zig` (read-write), `/zig` ←
  `/tmp/b4-zig/zig-x86_64-linux-0.17.0` (read-only). Zig 0.17.0
  (`/zig/zig version`).
- `b4-linkprobe` (probes + legacy execution): `/src` ←
  `/tmp/chakra-b4` (read-only: the §1 legacy tree, still intact
  with `build-xplat/`), `/pilot` ← `/tmp/b4-pilot` (read-only:
  B4.2 artifacts), `/work` ← `/tmp/b4-linkprobe` (read-write
  scratch: preflight + link probes + abort-triage artifacts).
- `/tmp/chakra-b4-zig` is a MANUAL COPY of the repo working tree
  (no `.git` inside); there is no automatic sync. Sync rule: after
  every repo edit, `cp` the changed files into the mirror and
  re-verify with `diff -q`. All B4.3 build inputs reached the
  container this way. End-state repo↔mirror diff: build inputs
  byte-identical (`diff -rq` excluding caches, `zig-out`,
  `__pycache__`, `._*`); remaining differences are Mac-only scratch
  (`Build/`, `build-*/`, `out/`) and AppleDouble files. One mid-run
  gap (`tools/build-contract/` missing from the mirror) was caught
  by the self-test step and repaired by copy.
- Zig provenance (finisher-verified): the build's Zig is the B4.2
  tarball re-staged — `/tmp/b4-zig/*.tar.xz` sha256
  `1cbe9df9…e2026` (identical to §7.1), extracted tree
  byte-identical to B4.2's (`diff -rq` clean), `zig version`
  `0.17.0`.

### 8.4 First online PAL-config execution: 95/95

- The B4.3 configure runs all 95 PAL checks in-container under
  qemu-user emulation — online, but emulated, not native execution
  (PAL-contract sensitivity S1; first online execution; B4.2
  verified offline only). First run:
  **94/95** — only `HAVE_SYSCTL` deviated (contract 0, observed
  1).
- The deviation exposed a probe-link infidelity (contract quirk
  L14): `sysctl@GLIBC_2.2.5` survives only as a non-default symbol
  version (no new link may bind it; the reference fails at link),
  while `zig cc`'s bundled stubs over-export it. Fixed Zig-side by
  prepending the real system link objects to every probe link; no
  contract value changed. Re-run: **95/95**
  (`pal-config-linux: 95/95 checks match the contract (33
  fixed-by-target, 62 compile-checked, 32 runtime-verified)
  [profile test]`).
- The 32 run probes executed for real (exit codes reproduced under
  qemu-user 10.2.3, including the four mmap outcomes) — the first
  execution of built code beyond trivial cases in this project.
  The failed 94/95 configure kept its probe sources under
  `/tmp/chakra-pal-config-linux-*` in `b4-zigbuild` (148 files;
  the 95/95 run deletes its own dir on success).

### 8.5 Full build record

- Command (in `b4-zigbuild`, `/src` = the §8.3 mirror): `/zig/zig
  build` (defaults: test profile, prefix `zig-out/`). No `-j` flag:
  Zig uses all 6 cores; emulated throughput held ~6 engine objects
  per minute in the big-TU tail.
- Timeline (UTC 2026-10-12): relaunch 21:27:08 → 22:44:37
  **rc=1 at 681/682** (77.5 min; single failure below) →
  diagnosis/fix → incremental 22:50:38 → 22:55:19 **rc=0**
  (4m41s: 1 recompile + 37 wabt recompiles + archives + 3 links +
  installs). First green 88 min after launch (incl. diagnosis).
  (Attested, not log-backed: no retained `zig build` stdout log
  shows the 681/682 or 682/682 counts; the total is triply
  corroborated instead — 682 unique sources in the tables,
  gate-asserted membership summing to 682, warm re-run rc=0.)
  (An earlier 21:08 invocation was killed by the finisher at ~21:26
  to fold in a link-line fix; its ~470 cached objects were reused.)
- The 681/682 failure: `lib/Jsrt/JsrtHelper.cpp` — clang 22 emits
  `-Wdate-time` for its `__DATE__`/`__TIME__` use (reference clang
  18 stays silent with identical flags), promoted by `-Werror`.
  Fixed Zig-side with a per-TU `-Wno-date-time` (sole using TU,
  verified by grep; see build-contract §2.1). No engine source
  touched.
- Two more rebuilds after green (both rc=0, minutes each on the
  warm cache): a comment-only no-op (1m53s; finisher forgot an
  addArg line, caught by the still-red metadata gate) and the
  GCStress `-pie` relink (~2 min; §8.6).
- Artifacts (`zig-out/`, 15 files): `bin/ch` (5.5MB),
  `bin/GCStress` (9.4MB), `bin/`+`lib/` `libChakraCore.so` (589MB
  each, identical), `lib/libChakraCoreStatic.a` (1.83GB), 3
  singular archives (3.4/0.09/3.5MB), 4 staged headers,
  `pal/src/config.h` (md5-identical to reference),
  `lib/wabt/config.h` (sha256-identical), `.profile` (`test`).
- Size vs reference: ours is 44–102% larger across the board
  (.so 589 vs 376MB; ch 5.5 vs 2.7MB (+101.5%); GCStress 9.4 vs
  6.5MB; static.a 1.83 vs 1.27GB). The delta is ~entirely debug
  info
  (.debug_loc 239 vs 67MB, .debug_info 245 vs 184MB — apparent
  DWARF-format verbosity difference, uninvestigated);
  behavior-neutral (debug sections don't execute) and ungated.
- The .so links clean under `-Wl,--no-undefined` (670 objects,
  version script, DllMain defined — strictness needed no
  workaround).

### 8.6 Checks

Per-check grades (all run in `b4-zigbuild` against `zig-out/`; steps
stay red (rc=1) wherever anything is BLOCKED or FAILed):

- Smoke (`zig build check-smoke`, rc=1): version PASS (rc=0, exact
  `ch version 1.13.0.0-beta`), help PASS (rc=0), invalid-flag
  BLOCKED, snippet BLOCKED, hello BLOCKED (all three rc=-6, empty
  stdout, qemu abort line — the §8.2 signature; the bogus flag
  reaches engine init before any flag error). 2 legs green, 0 JS ran.
- Metadata (`zig build check-metadata`, rc=0): PASS — ELF identity
  (ch/GCStress DYN, .so DYN), SONAME, ICU pair on .so+ch with no C++
  runtime NEEDED, .so exports 236/236 exact, ch exports 1248 ⊇
  required 1 (membership gate, see note below), wabt sha256 exact,
  pal markers, 4 headers byte-identical, .so copies identical,
  archives 667/90/1/11. Observed NEEDED deltas vs reference (all
  loader trivia, none gated): ours carries libpthread/libdl where
  the reference drops them (genuinely referenced here), and no
  libgcc_s (compiler-rt links statically).
- Bytecode (`zig build verify-bytecode`, rc=1): BLOCKED — the
  verifier ch aborts in init (`Failed to generate 64bit JsBuiltin
  Bytecode`); 0 verified; source headers confirmed unchanged.
- Regression: full suite NOT run (an unbounded all-abort run adds
  zero information). Bounded gate run
  (`check_regression_linux.py … test/Basics`, 47 tests × 2
  variants, ~15s): interpreted BLOCKED (0/47, every failure
  abort-signed), dynapogo BLOCKED (0/47, same) — overall BLOCKED,
  rc=1. The gate script was reworked for per-variant runs (it
  crashed on runtests.py's two Total lines); its 13 self-test
  controls pass.
- Native (`zig build check-native`, rc=1): BLOCKED — dies
  generating the first Makefile (`ch Platform.js` aborts); 0 ran.
  Non-execution half proven separately: `test-shared-basic`
  compiles+links against our .so (system clang++, rc=0, 16KB
  sample, NEEDED libChakraCore.so); the sample is correctly left
  unrun.
- Focused JIT/WASM: BLOCKED — `ch test/wasm/api.js` shows the
  identical init-abort signature (rc=134, 0 bytes); the abort
  precedes any JS/JIT, so no variant distinction is observable.
- Self-tests (`zig build check-selftest`, rc=0): all 5 programs
  green — the 4 check-script self-tests plus the 27-test tools
  file (2 pre-existing skips in-container), the tools file green
  both in-container and on Mac.

ch-exports note: GNU ld constrains ch's --export-dynamic by the
version script (reference: exactly 1 export) but LLD has no
equivalent spelling (probed: all-or-nothing, both selective flags
unsupported) — ours exports 1248 including the required symbol.
The surplus is harmless; the gate asserts membership (see
build-contract §5.7).

Core dumps: every aborting ch dumps a ~77MB qemu core in CWD
(image default); ~15GB accumulated across the subset runs and was
deleted after (finisher hygiene).

### 8.7 Pre-flight and link probes (predecessor, finisher re-verified)

- 6-TU pre-flight (`/tmp/b4-linkprobe/preflight.sh`, exact
  reference command lines incl. `-fdiagnostics-color`, zig 0.17.0
  driver swap only): `JavascriptFunction.cpp` (Runtime Library),
  `Alloc.cpp` (Parser), `wast-parser.cc` (wabt), `ch.cpp` (ch),
  `context2.S` (PAL), `LinearScanMdA.S` (Backend) — 6/6 rc=0,
  0-byte logs, nonempty objects (`pf0.o`–`pf5.o` + depfiles kept).
  Finisher note: the pre-flight ran the NATIVE no-`-target`
  driver, which default-searches `/usr/include` (verified: a
  `<unicode/uchar.h>` TU compiles natively, fails under explicit
  `-target x86_64-linux-gnu`) — the build's explicit target needs
  the §3/§5.6 `-isystem`/`-L` spellings the pre-flight got
  implicitly.
- C++ standard library: `-stdlib=libstdc++` is IGNORED
  (finisher re-probed: `_LIBCPP_VERSION` defined with and without
  the flag); the engine compiles against libc++ headers and links
  libc++ statically (our `.so`/ch NEEDED carry no C++ runtime —
  §8.6). `-nostdinc++` drops only the C++ headers (`<string>`
  not found, `<stdio.h>` still resolves).
- Link strictness: `--no-undefined` verified effective both ways
  (finisher: `.so` link with a genuinely-undefined symbol
  succeeds by default, fails rc=1 with the flag; exe links fail
  by default). `zig ar` present (rc=0). `DllMain` is export #1 of
  the committed 236-set and defined in the real link.
- Trivial execution probes (`h`/`m` binaries) and the constructor
  probe (`ctor.so`) predate the §8.2 root cause; the `mmap0`
  probe documents an observed-but-irrelevant qemu difference.

### 8.8 Open uncertainties / follow-ups

- Linux/JIT check counts are still never observed green anywhere
  (BLOCKED here by §8.2; no native run exists). Recording Linux
  baselines needs an execution-capable host.
- qemu layout workaround UNTRIED: forcing the guest stack/mmap
  below 128TB (qemu `-B`/guest-base/stack-placement options via a
  binfmt wrapper) might clear §8.2 and unlock real checks in this
  same container. A bounded box is suggested; if it fails, native
  x64 remains the path.
- libc++-headers vs libstdc++-headers behavioral equivalence is
  still assumed-sound (C-only ICU boundary) and now
  metadata-gated — but no JS has executed under the zig-built
  engine, so behavior parity is unproven.
- `zig ar` (LLVM) vs GNU `ar qc` archive-byte differences are not
  analyzed (member counts asserted by the metadata gate;
  construction order comes from the generated tables; bytes not
  compared).
- The 8.2.2→10.2.3 qemu upgrade path is unrecorded (predecessor
  spike): a from-scratch B4 rebuild would need to re-derive the
  10.2.3 install (B5 owns the Dockerfile).
- The §8.2 abort precedes any JS execution or JIT compilation
  (static-init time), so no JIT/WASM behavior has been observed
  under emulation at all — focused JIT/WASM checks are BLOCKED
  with the rest.
- Known blemish (cosmetic, behavior-neutral): the 5 C++ codegen
  flags (`-fno-omit-frame-pointer -fdelayed-template-parsing -O3
  -g -std=gnu++17`) appear TWICE on every C++ compile line —
  `tables.warnings` carries 30 entries (25 `-Wno-*` + the 5
  codegen) while `linux.zig` re-emits the 5 explicitly (Mac's
  tables keep 25). Values are identical (last-wins), so compiles
  are unaffected. Dedup deferred deliberately: filtering the 5
  in the generator changes all 651 C++ command lines and needs a
  full ~80min rebuild to re-verify for zero behavior gain (see
  build-contract §8). The emission comment now states the 30.
- Evidence hygiene: no `zig build` stdout transcript was
  retained (B4.1 lost its log the same way) — keep the build
  transcript next milestone. The in-container retained gate logs
  (23:23–23:26) likewise predate the final 23:56 script sync (they
  show the older 9-control/22-test self-tests); the cycle-3 live
  re-runs (13/13 controls, 27-test tools file) supersede them.
