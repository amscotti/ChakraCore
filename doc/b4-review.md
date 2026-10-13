# Post-B4 review record (gate-fix pass on `9cba073d0`)

Two independent post-B4 reviews converged: the
[B4 gate verdict](b4-gate.md) **BLOCKED-with-green-build** is honest
— the Linux Zig build is green and zero behavior checks have run —
but several check gates had defects that had to be fixed *before*
any native baseline is recorded, because a first native run graded
by a broken gate would record poisoned counts. This file records
the grade correction, the fixes applied by the gate-fix change,
and what is explicitly deferred. The change touches gates,
self-tests and docs only, changes no Linux compile or link
command line, and
performed no Linux execution (verification was Mac-host: Python
unit tests and `--self-test` controls, plus Zig unit tests of pure
gate logic compiled for the Mac host).

## Grade correction (b4-gate.md §3)

The §3 grades in [b4-gate.md](b4-gate.md) are preserved as
recorded; the correction lives here and in a §3 errata pointer.

- **Row 4: PASS → PARTIAL.** The clause covers "Linux PAL config,
  assembly, archives/version scripts, system ICU, native loading
  in Zig". Build- and metadata-level evidence stands (archives,
  version scripts, ICU NEEDED pair, `.so` co-location), but native
  *loading* was never observed to succeed: the §8.2 abort trace
  runs through `dlopen` into the `.so`'s static initializers and
  `dlopen` never returned, on either binary. Likewise the 95/95
  PAL online result rests on run probes executed under qemu-user
  emulation (PAL-contract sensitivity S1), not natively. What is
  proven is configuration/assembly/archive/link shape; native
  loading and native run-probe outcomes remain unproven.
- **Row 1 note (PARTIAL unchanged).** The "own legacy baseline"
  half remains build-only: the legacy build is 690/690 green, but
  `ninja check` never ran, so no legacy check counts exist
  anywhere for a future Zig run to be compared against. Recording
  them is part of the first native run, per gate §4(a).

No other §3 grade changes. Rows 5–7 stay BLOCKED (0 ran), row 9
stays NOT MET, row 10 stays NO.

## Fixes applied by this change

- **A. Abort classifier (`common.is_engine_init_abort`).** Moved
  from `check_native.py` to `tools/build/common.py` as the shared
  home for the native/bytecode callers and made strict,
  qemu-signature-only: BLOCKED now requires nonzero rc, at least
  one qemu `uncaught target signal 6 (` occurrence and zero
  occurrences of any other qemu-phrased signal number
  (occurrence-based via the shared `common.qemu_target_signals`
  parser, so signal 61 never reads as 6 and a mixed
  signal-6 + signal-11 log vetos BLOCKED on separate lines or on
  one line carrying both), and no ` : PASS`. A bare shell
  `Aborted (core dumped)` with no signal-6 occurrence, a qemu
  signal-11 occurrence (alone or mixed with signal 6), or a
  native abort with no qemu signal-6 occurrence grades FAIL (the
  old predicate accepted any `Aborted` substring, so a native
  SIGABRT would have been mis-graded BLOCKED).
  `verify_bytecode_pure.py` uses `common` (its
  `import check_native` is gone), and `check_native.py`'s BLOCKED
  branch now prints the computed failure diagnostics instead of
  discarding them. The smoke and regression gates keep gate-local
  predicates (`check_smoke_linux.is_abort`,
  `check_regression_linux.grade_variant`) rather than calling the
  shared classifier, but all three sites share the
  `qemu_target_signals` occurrence parser and the same
  pure-signal-6 rule (the regression gate additionally requires
  nonzero rc on both of its paths; see fix B for its residual
  limitation).
- **B. Regression gate (`check_regression_linux.grade_variant`).**
  PASS now requires rc == 0, as its module docstring already
  promised: the 0-pass/all-signal-6 BLOCKED case is evaluated
  first, any other nonzero rc with a Total summary grades FAIL
  with the rc named in the detail, then baseline/count checks
  run. Both paths (Total-present counting and summary-less) are
  occurrence-based over the shared `qemu_target_signals` parser:
  only signal-6 occurrences count (occurrences, not lines, so
  one line carrying two 6s counts two), BLOCKED requires
  rc != 0 on both paths, and any other qemu-phrased signal
  occurrence vetos BLOCKED on both paths (separate lines or one
  line carrying both); a qemu signal-11 occurrence or a bare
  `Aborted` with no signal-6 occurrence grades FAIL — previously
  an all-signal-11 run graded BLOCKED and a nonzero-rc run with
  matching counts graded PASS. Residual limitation: the veto
  covers qemu-phrased other-signal occurrences only — a death
  with no qemu signal line at all (e.g. a bare
  `Segmentation fault (core dumped)`) carries no occurrence to
  veto or count and is not detectable by counting, and duplicate
  signal-6 echoes can still inflate the occurrence count; both
  remain documented residual limitations of the counting path.
  Controls added both as `--self-test` cases (with
  `RECORDED_BASELINE` monkeypatched and restored; the all-None
  pin still passes) and as focused unittests in
  `test_build_tools.py`.
- **C. Metadata gate (`check_metadata_linux.py`).** The PASS
  banner's export counts are derived from the committed export
  files instead of a hardcoded `(236/1)`. An empty export
  reference now fails loudly in both `check_exports` and
  `check_exports_subset` (an emptied ch list previously made the
  subset gate vacuous). Self-test gains negative controls for
  `check_exports`/`check_archive`/`check_header` on synthetic tmp
  files. The docstring now states plainly that the B4 record does
  not document whether `linux_exports_*.txt` were captured from
  the legacy reference or the Zig artifact — today the gate
  proves stability against the committed lists — and gives the
  exact regeneration-from-legacy recipe (`nm -D --defined-only`,
  third field of every 3-field line regardless of type letter,
  sorted, then diff) required on the first
  native host before any parity claim.
- **D. Dead PAL offline-render test wired.**
  `tools/pal-config-linux/check.zig`'s `offline render` test had
  no `addTest` anywhere. It is now wired into the Linux
  `check-selftest` step in `tools/build/linux.zig` via `addTest` +
  `createModule` + `addRunArtifact`, with the new test root
  `tools/linux-gate-tests.zig` — wired, not yet Linux-run: that
  Linux step has not been executed on a Linux host (the same test
  root was run green on the Mac host only; see below). The root must live directly under
  `tools/`: Zig 0.17 roots a module at its root file's directory,
  and `pal-config-linux/probes.zig` imports
  `../pal-config/probes.zig`, so standalone
  `zig test tools/pal-config-linux/check.zig` fails with
  `error: import of file outside module path` (observed verbatim
  on the Mac host), while `zig test tools/linux-gate-tests.zig`
  runs the render test green. Two facts the fix had to work
  around: the Mac `build.zig` has no pal-config unit test or
  `addTest` precedent to mirror (its `createModule` shape at
  `build.zig:237` is the local pattern used), and the wired test
  asserts contract values only — byte-exact comparison against
  the reference header remains the documented offline/VM
  procedure (pal-config-contract-linux.md §6 wording corrected
  accordingly).
- **E. Dockerfile.** `tools/docker/Dockerfile.ubuntu-x64` pins the
  base image by the digest recorded in b4-linux.md §1
  (`ubuntu:24.04@sha256:534baea6…`) and comments that apt package
  versions are recorded in b4-linux.md §1 but deliberately not
  version-pinned (controlled dependencies are B5), and that
  qemu-user/binfmt is host-VM provisioning outside the image, so
  execution verdicts are not reproducible from the image alone.
- **glibc gate (applied).** `gateGlibc` in
  `tools/build/linux.zig` substring-searched the first `ldd
  --version` line for `2.39`; it now exact-matches the trailing
  bare version token (the Mac `gateSdk` exact-compare pattern),
  factored as `glibcFirstLineMatches` with a unit test collected
  through the same test root. Gate-only: no compile or link line
  is affected. The helper and test are semantically analyzed and
  green on the Mac host; the `ldd` invocation itself still only
  runs on a Linux host.
- **Doc fixes.** B4 status propagated in
  [modernization-plan.md](modernization-plan.md) (header,
  milestone row, §1 limits, §6 checklist — B4 stays unchecked,
  marked BLOCKED-with-green-build — next-implementation pointer
  to gate §4(a), reference list); [b4-gate.md](b4-gate.md) §5
  replaced by the landed-as-`9cba073d0` note, §2 "first native
  execution" corrected to online-under-qemu with the S1 pointer,
  §3 errata pointer added; [b4-linux.md](b4-linux.md) §8.4/§4.1/
  §8.8/§6 wording corrected (emulated execution, temporal binfmt
  annotation, archive counts vs construction order, Dockerfile
  reproducibility scope), mirrored in
  [build-contract-linux.md](build-contract-linux.md) §8 and
  [pal-config-contract-linux.md](pal-config-contract-linux.md)
  §6/S2. Observed historical outputs (e.g. the quoted `(236/1)`
  banner in gate §1.2) are preserved as recorded.

Verification of this change on the Mac host: test-first sequence
observed failing against the old code (the new strict-classifier
and grade tests failed against the old code, including old-code
`PASS` at rc=139 and `BLOCKED` for all-signal-11), then green
after the fixes — `test_build_tools.py` 48 tests OK (was 27 at the B4
record), all four gate `--self-test`s report `ALL NEGATIVE
CONTROLS BEHAVE`, and `zig test tools/linux-gate-tests.zig`
passes 3/3 (root + offline render + glibc helper) with
`zig fmt --check` clean on touched `.zig` files. The Linux build
graph itself was not re-run: this change cannot re-verify the
Linux build, by design.

## Explicitly deferred (with reasons)

- **Bounded qemu guest-base spike** — gate §4(b) (`-B`/guest-base
  to force the guest layout below the 128TB PAL ceiling) is still
  untried; the ~2 h bounded box remains the only emulation path
  left, and even on success its counts need native corroboration.
- **Transcript retention on the first native run** — the first
  native legacy + Zig runs (gate §4(a)) must retain their full
  transcripts/counts as the recorded baseline evidence; nothing
  in this pass records or retains any native transcript.
- **Archive member counts** — the recorded archive counts
  (667/90/1/11) share the export lists' undocumented-provenance
  caveat: the B4 record does not document whether they were
  taken from the legacy reference or the Zig artifacts, and the
  metadata gate checks member counts only (member names are not
  gated), so they prove stability against the recorded counts,
  not legacy parity, until regenerated on the first native host.
- **Cold build and all native counts** — need a native x64 host
  (gate §4(a)); emulation cannot substitute, and no baseline may
  be recorded from emulated runs.
- **Exports / archive-name regeneration from the legacy
  artifacts** — needs the legacy binaries on an execution-capable
  (native) host; recipe recorded in `check_metadata_linux.py`.
  Until then the metadata gate proves stability against the
  committed lists, not legacy parity.
- **Distinct focused JIT/WASM leg** — needs native execution; the
  §8.2 abort precedes any JS/JIT, so no JIT/WASM behavior has
  been observed at all.
- **Duplicate codegen flags and duplicate `-Ipal/src/include`** —
  known cosmetic compile-line duplication (b4-linux.md §8.8);
  fixing either changes Linux compile command lines and needs a
  full rebuild to re-verify, which this gate-only pass must not
  do unobserved.
- **Shared Mac/Linux build core before B6** — the two graphs
  still duplicate structure; factoring is a build-graph change
  requiring full re-verification on both platforms, not a gate
  fix.
- **Baselines-as-data** — `RECORDED_BASELINE` remains a code
  literal in `check_regression_linux.py`; moving counts to a
  data file should happen when the first real counts are
  recorded natively, not before there is anything to record.
- **Linux CI** — B5 / the plan's "Done when clean native Linux
  CI proves behavior/ABI parity" clause; no CI is added here.
