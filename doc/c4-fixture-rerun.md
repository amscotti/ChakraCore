# C4 fixture-cache subset rerun

Status: **complete for the selected subset; C4 remains uncertified**. The
host-reported negative phase is still missing, and this run is not a full
edition result.

## Scope

C1 identified 30 test262 directories whose recorded baseline could be affected
by eshost's process-wide fixture-source cache: 2,365 scenarios. The recorded
pre-fix combined run was **1,265 passed / 1,100 failed** at `--jobs=1`
(`out/test262-c1r3-j1-precombined`). The harness now clears the fixture cache
before each scenario and isolates per-agent temporary directories.

This rerun used the exact same 30 patterns, the current `modernize/baseline`
binary, the fixed `tools/test262/run.py` harness, `--jobs 1`, and the same
host flags (`-nonative -Test262 -CollectGarbage`). The 20 controls (including
the duplicate-basename fixture pair and deliberate negative controls) passed.

## Result

| run | passed | failed | scenarios |
|---|---:|---:|---:|
| C1 pre-fix precombined | 1,265 | 1,100 | 2,365 |
| current fixed-harness rerun | **1,367** | **998** | 2,365 |

Scenario-key comparison (file + scenario) against the old precombined run:
**0 pass→fail**, **102 fail→pass**. The 102 gains are expected from C2 feature
work and are not attributed solely to the cache fix; the zero pass→fail result
is the useful non-regression signal for this subset.

Binary: `zig-out/bin/ch` SHA-256
`cd0282a7032d5c3e0690c2a1104ad2199d5014af7c615a6dd50ade2221c18fa2`;
dylib SHA-256
`4a303d0520043cfc4ead2f4a71b173750bd2e39f15be12e7a7015c6c9d7cd2b7`.
Artifact: `out/test262-c4-fixture-rerun/summary.json` and `results.json`.

## Limits

- This is **not** a C4 qualification: the host still does not report parse vs
  resolution vs runtime phase explicitly, so wrong-phase passes cannot be
  fully ruled out (see [c1-triage.md](c1-triage.md) §3/§9).
- The rerun covers only the 30 fixture-cache-affected directories, not the full
  required ES2025 suite.
- The old and new binaries differ by the entire C2 stream, so the pass/fail
  delta is not a controlled measurement of the cache fix; the controls and the
  zero pass→fail comparison are the relevant evidence.
- Mac ARM64 no-JIT only; B4 Linux/JIT and the S3/S6 default decisions remain
  open.
