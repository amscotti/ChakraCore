# B4 gate verdict (evidence consolidation, 2026-10-12)

B4.4 — final proof re-runs, evidence inventory, and the honest gate
verdict vs [modernization-plan.md](modernization-plan.md) §B4. B4.1–B4.3
are implemented and dual-reviewed (all ACCEPT); this file consolidates
their evidence and grades the milestone. Style mirrors the B3 gate
record ([b3-profiles.md](b3-profiles.md) §12).

> Verdict: **B4 is BLOCKED-with-green-build** — the Linux Zig build is
> green (682/682, 95/95 PAL checks, metadata PASS), but zero behavior
> checks have run anywhere: the plan's "Done when clean native Linux CI
> proves behavior/ABI parity" clause is unmet, and emulation cannot
> substitute. No Linux default rollout. See §3 for the clause-by-clause
> grade and §4 for the exact path to green.

## 1. Final proof re-runs (2026-10-12, this step)

Fast/green only — no full rebuilds, no full test suites. Every number
below was observed in this step's own runs.

### 1.1 Mac no-regression proof

`mise x -- zig build check` → exit 0, zero FAIL lines:

```
Total: passed 1959, failed 0
check_regression: exit=0 parsed=(1959, 0) require passed 1959 / failed 0
check_regression: PASS: passed 1959 / failed 0
```

(bytecode `up to date`, shared native PASS + 8 ignored, smoke PASS,
metadata PASS — same run, tail quoted in full before the Total grep.)

`python3 tools/build/test_build_tools.py` → exit 0:

```
Ran 27 tests in 9.528s

OK
```

The B4 diffs to shared Mac files (`build.zig` Linux dispatch,
`check_native.py`/`verify_bytecode_pure.py` abort-classifier,
`test_build_tools.py` new controls) change zero Mac behavior: the
Test-profile 1959/0 and the 27-test tools file are both green on the
end-state tree.

### 1.2 Linux proof (rootful daemon only)

All commands via `limactl shell docker -- sudo docker ...` against the
rootful system dockerd (rootless is BLOCKED for amd64 per
[b4-linux.md](b4-linux.md) §2; nothing here was concluded from it).
Containers `b4-zigbuild` / `b4-linkprobe` both `Up`; images
`chakracore/b4-linux-x64` (`a03c82f0e46b`),
`chakracore/b4-linux-x64-proof` (`004ef83bb9b2`) intact.

Warm `zig build` in `b4-zigbuild` → **rc=0**, no-op (configure lines
only, zero compiles):

```
b4 profile=test: target=x86_64-linux-gnu glibc=2.39 icu=/usr/include,/usr/lib/x86_64-linux-gnu zig=0.17.0 python=/usr/bin/python3
pal-config-linux: 95/95 checks match the contract (33 fixed-by-target, 62 compile-checked, 32 runtime-verified) [profile test]
b4 wabt config.h: 7973 bytes from lib/wabt/src/config.h.in
```

`zig build check-metadata` → **rc=0**:

```
check_metadata_linux: PASS: ELF + SONAME/NEEDED + exports (236/1) + configs + headers + archives + marker all match
```

`zig build check-selftest` → **rc=0**: 4/4 check-script suites report
`ALL NEGATIVE CONTROLS BEHAVE`, plus the tools file:

```
Ran 27 tests in 50.906s
OK (skipped=2)
```

(The 2 skips are the pre-existing in-container skips recorded in
[b4-linux.md](b4-linux.md) §8.6; the one `FAIL:` line in the log is a
self-test control's expected output, not a failure — overall rc=0.)

Deliberately NOT re-run: full `zig build` from cold (~80 min
emulated), full regression (bounded 47×2 gate already graded BLOCKED —
an unbounded all-abort run adds zero information), smoke/bytecode/
native (all graded BLOCKED in §8.6; re-running aborts re-dumps ~77MB
cores each for no new fact).

### 1.3 Hygiene sweep

- Repo↔mirror `diff -rq`, run in the VM against the virtiofs-mounted
  repo (`/Users/ascotti/Documents/code/ChakraCore`) and the mirror
  (`/tmp/chakra-b4-zig`), excluding `.git`, `.zig-cache`, `zig-out`,
  `out`, `Build`, `build-*`, `__pycache__`, `._*`: **2 items, both
  docs** — `Only in <repo>/doc: b4-gate.md` (this file, written
  after the mirror copy) and `doc/b4-linux.md differs` (doc edits
  after the mirror copy). A second diff scoped to build inputs
  (`tools/build`, `tools/pal-config-linux`, `tools/docker`,
  `build.zig`, `mise.toml`) is **byte-identical** (rc=0); the
  full-tree diff likewise shows zero `lib/`/`pal/`/`bin/`/`test/`
  drift. No sync action needed (docs are not build inputs).
- Core-file sweep (repo to depth 3, VM `/tmp` to depth 2, both trees,
  both containers' `/tmp` and work mounts, VM scratch dirs,
  in-container `/src` full depth): **zero core dumps found** — only
  source files with "core" in the name (`test/WasmSpec/.../core`,
  Zig libc headers). Nothing deleted, nothing to delete.
- `git status` / diff file-set: 4 modified + 14 untracked entries —
  landed as `9cba073d0` (§5). `lib/`/`pal/`/`bin/`/`test/`/CMake clean.
- Legacy `/tmp/chakra-b4` intact: `build-xplat/ch` 2756104 bytes,
  `ELF 64-bit LSB pie executable, x86-64, ... with debug_info, not
  stripped`; `pal/src/config.h` MD5 `f73cd5b2968beaad6b6266a2cb61b06a`
  (both match [b4-linux.md](b4-linux.md) §3 and the PAL contract).
- Environment: binfmt `qemu-x86_64` enabled, interpreter
  `/usr/local/bin/qemu-x86_64-10.2.3`, flags `POF`; repo Dockerfile
  `cmp`-identical to `/tmp/b4rebuild/Dockerfile`.
- Observation (not a finding): image `tonistiigi/binfmt:latest` is
  present in the rootful daemon though §4.1 records no foreign binfmt
  images remaining — likely pulled by the unrecorded 10.2.3 spike
  (image-store entry 13:17:54, qemu binary 13:22, both 2026-10-12
  EDT, after the B4.1 window). It cannot be load-bearing: binfmt
  dispatches `qemu-x86_64` to the host file
  `/usr/local/bin/qemu-x86_64-10.2.3`, never to the image. Left
  untouched (unattributable); B5 owns Dockerfile/handler provenance.

### 1.4 Evidence-pointer spot check

Every B4.1–B4.3 doc pointer checked still resolves; **zero dangles**:

- All 15 repo paths cited (Dockerfile, 3 `tools/pal-config-linux/`
  files, 8 `tools/build` Linux files, 3 docs) exist.
- Host `/Users/ascotti/b4-pilot-tmp/`: `extract.py`, `run-all.sh`,
  `verify_contract.py` present.
- VM: `run-all.sh`, `toolchain-versions.txt`, `preflight.sh` present;
  `CMakeConfigureLog.yaml` 3975 lines (matches the contract doc);
  in-container `zig-out/` tree intact (`bin/`, `lib/` incl. `wabt/`,
  `include/`, `pal/src/config.h`, `.profile` = `test`).
- Content citations verified live: `RecyclerWriteBarrierManager.cpp`
  `:99-103` (the `lpMaximumApplicationAddress` ceiling check with
  "How did this happen?"), `pal/src/misc/sysinfo.cpp:104` (128TB),
  exports files 236/1 lines.

## 2. B4 evidence inventory (what each step proved)

| Step | Claim | Numbers | Evidence |
|---|---|---|---|
| B4.1 environment | amd64 container feasible for configure+build under rootful dockerd; rootless BLOCKED | image 81s, rebuild 76s, zero apt drift | [b4-linux.md](b4-linux.md) §§1–3 |
| B4.1 legacy build | full x64/JIT legacy build green | 690/690, exit 0, 78m07s, ch 2756104 B | [b4-linux.md](b4-linux.md) §3 |
| B4.1 execution | BLOCKED by qemu 8.2.2 `maps` SIGSEGV (Chakra-free repro, 4/4) | 0 tests ran | [b4-linux.md](b4-linux.md) §4 |
| B4.2 pilot | `zig c++` compiles+links a PAL/Backend/WasmReader slice, exact reference flags | 5/5 compiles, 1/1 link, 0-byte logs both sides | [b4-linux.md](b4-linux.md) §7 |
| B4.2 PAL contract | 127 settings classified, offline render byte-identical to reference | 4839 B, MD5 `f73cd5b2…` | [pal-config-contract-linux.md](pal-config-contract-linux.md), [b4-linux.md](b4-linux.md) §7.4 |
| B4.3 PAL online | first online execution of the 95 checks, in-container under qemu-user emulation (not native; PAL-contract S1); L14 link fix, no value changed | 94/95 → 95/95 | [b4-linux.md](b4-linux.md) §8.4 |
| B4.3 Zig build | full Linux graph green incl. Backend/WasmReader/wabt | 682/682, rc=0, 15 artifacts | [b4-linux.md](b4-linux.md) §8.5, [build-contract-linux.md](build-contract-linux.md) |
| B4.3 metadata | artifact-identity gate green | .so 236/236, ch 1248⊇1, archives 667/90/1/11 | [b4-linux.md](b4-linux.md) §8.6, re-run §1.2 |
| B4.3 execution | BLOCKED by root-caused qemu 10.2.3 address-layout abort (guest stack ~247TB vs PAL 128TB ceiling); legacy binary aborts identically | 0 JS ran on either binary | [b4-linux.md](b4-linux.md) §8.2 |
| B4.4 (this step) | Mac no-regression + Linux green-leg re-proof + hygiene + verdict | 1959/0, 27 OK, rc=0 ×3, 0 cores, 0 dangles | §1 above |

## 3. B4 Done, clause by clause (plan §B4)

| # | Plan requirement | Grade | Evidence |
|---|---|---|---|
| 1 | Linux runner/container environment + own legacy baseline | **PARTIAL** (build PASS / baseline checks BLOCKED) | container + 690/690 green ([b4-linux.md](b4-linux.md) §3); `ninja check` never ran (§4) |
| 2 | compiler/linker pilot before attributing failures to new logic | **PASS** | 5/5 + 1/1, zero warnings ([b4-linux.md](b4-linux.md) §7) |
| 3 | Backend, WasmReader, debug/Test WABT in the graph | **PASS** (built+linked) | 89+2 / 11 / 36+1 compiles in the green 682 ([build-contract-linux.md](build-contract-linux.md) §4) |
| 4 | Linux PAL config, assembly, archives/version scripts, system ICU, native loading in Zig | **PASS** | 95/95 online, 9 `.S`, archives 667/90/1/11, version scripts, ICU pair linked, `.so` co-location ([b4-linux.md](b4-linux.md) §§8.4–8.6) |
| 5 | bytecode + regression in interpreted/dynapogo variants | **BLOCKED** | 0 ran — verifier and suite abort in init (§8.6; bounded 47×2 gate 0/47 both variants) |
| 6 | focused JIT/WASM tests | **BLOCKED** | 0 ran — abort precedes any JS/JIT (§8.6, §8.8) |
| 7 | native embedding checks | **BLOCKED** | 0 ran — dies generating the first Makefile; compile+link half proven separately (§8.6) |
| 8 | applicable Debug/Test/Release coverage | **NOT STARTED** (Test only) | one profile by design; Debug/Release are future profiles ([build-contract-linux.md](build-contract-linux.md) §5.1) |
| 9 | "Done when clean native Linux CI proves behavior/ABI parity for the declared configuration and its floor. Runtime behavior must run on the target" | **NOT MET** | no native CI exists; no JS has executed on either binary; qemu is not the target |
| 10 | enable the Linux Zig default per verified profile | **NO** | zero profiles verified for behavior; Test is build-verified only |

> Errata (post-B4 review): see [b4-review.md](b4-review.md) — row 4 is
> corrected PASS → PARTIAL there (`dlopen` never returned, so native
> loading is unproven, and the PAL run probes were emulated per S1),
> with a note on row 1. The grades above are preserved as recorded.

Overall: **BLOCKED-with-green-build.** Build/probe/artifact clauses
(2, 3, 4) are genuinely PASS with quoted numbers (row 4 as recorded;
see the errata above — post-B4 review corrects row 4 PASS → PARTIAL
in [b4-review.md](b4-review.md), since `dlopen` never returned so
native loading is unproven and the PAL run probes were emulated per
S1); every
behavior clause (5, 6, 7) is BLOCKED with 0 ran; the "Done when"
clause (9) is therefore unmet — not partially, but structurally:
there is no behavior/ABI parity evidence at all, and the plan
explicitly bars counting anything short of target execution
("linking a cross-built binary on a Mac is not the gate" —
here the stronger case holds, since even in-container execution
aborts). This is not B3-style CONDITIONAL: B3's condition was
"all clauses green on this host, pending CI confirmation" —
here the behavior clauses are red, so there is nothing to
confirm yet. No PASS is claimed or smuggled; no Linux default
rollout is authorized.

## 4. Exact path to green

Two ordered options; either unblocks clauses 5–7 and 9. Both keep
the green build as the foundation — the graph, tables, PAL wiring
and gates need no rework, only an execution-capable host.

**(a) Native x64 runner (preferred).** A native Linux x64 machine
(GitHub-hosted `ubuntu-24.04` runner, a loaned box, or a cloud VM)
running the B4.1 legacy recipe then the B4.3 Zig graph natively:
record the first legacy `ninja check` counts (interpreted/dynapogo),
then the first Zig check counts, and require parity. Effort bound:
small — the Dockerfile, configure lines, and check commands already
exist; the work is provisioning + two `ninja`/`zig build` cycles
(legacy 78 min emulated → expect ~10–15 min native) plus count
recording. This also retires PAL-contract sensitivity S1 (run probes
under emulation) via the mandated native re-run.

**(b) qemu guest-base spike (bounded box).** Force the guest
stack/mmap below the 128TB PAL ceiling via qemu `-B`/guest-base or
stack-placement options through a binfmt wrapper, in this same
container. Success criterion: `ch test/Basics/hello.js` prints
`hello world` + `PASS` (either binary). Effort bound: time-box to
one session (~2 h); the mechanism is UNTRIED and qemu layout flags
under binfmt are fiddly — if the box expires, stop and do (a).
Even on success, counts recorded under emulation need a native
corroboration note before any release claim.

NOT on the path: more emulation tuning beyond the box (the 8.2.2→
10.2.3 upgrade already consumed its spike), an arm64-native
container (changes the arch the baseline covers — plan §B4 wants
x64), or weakening the gate to metadata-only (that is exactly what
the "Done when" clause forbids).

## 5. Scope audit

Landed as commit `9cba073d0` ("B4: Linux x64/JIT Zig build green, behavior checks blocked on qemu (BLOCKED)"); the pre-commit scope audit that stood here is superseded by that commit.

## 6. Lessons for future phases

- **Heartbeat rule for long calls:** any VM/container command that
  can exceed ~60s gets an explicit long yield and a progress
  expectation up front; a silent call is presumed still-running,
  never dead — no duplicate launches while one is pending.
- **Rootful-vs-rootless daemon discipline:** every docker command
  in B4+ work spells the daemon (`sudo docker` = rootful); results
  from the rootless list are never evidence for amd64 conclusions.
- **Mirror sync rule:** `/tmp/chakra-b4-zig` has no automatic sync —
  after every repo edit, `cp` the changed files over and re-verify
  with `diff -q`; build-input diffs must be empty before any
  in-container run counts as evidence.
- **Transcript retention:** B4.1 and B4.3 both lost their full-build
  stdout logs (pruned container, unattended run) — every future
  long build redirects to a retained log file first, or its counts
  are attested rather than log-backed.

## 7. Open uncertainties carried forward

- Linux/JIT check counts have never been observed green anywhere
  (no legacy baseline, no Zig parity) — recording is per variant
  (interpreted, dynapogo) on the first green run.
- The 8.2.2→10.2.3 qemu upgrade path is unrecorded (B5 owns the
  Dockerfile/handler provenance); `tonistiigi/binfmt:latest` in the
  rootful daemon is an unattributed leftover of that spike (§1.3).
- libc++-headers vs libstdc++-headers equivalence is assumed-sound
  (C-only ICU boundary) and metadata-gated, but no JS has executed
  under the zig-built engine — behavior parity unproven.
- Duplicate 5 C++ codegen flags on every compile line (cosmetic,
  behavior-neutral, dedup deferred to avoid a full rebuild —
  [build-contract-linux.md](build-contract-linux.md) §8).
- No `zig build` stdout transcript retained for the 682/682 build
  (triply corroborated instead: 682 table sources, gate-asserted
  membership, warm rc=0 re-run §1.2); in-container gate logs
  (23:23–23:26) predate the final script sync — the cycle-3 live
  re-runs supersede them.
