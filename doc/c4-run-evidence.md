# C4 — Certification-grade full-corpus run: evidence record

Recorded 2026-11-02 on branch `c4-certification` (worktree
`ChakraCore-wt/c4-certification`), HEAD `ba796f74f` — the same commit as
main's latest integration checkpoint (gates 2038/2090). **No engine source
was changed in this lane** (`git diff HEAD -- lib pal bin` is empty; the
only tree additions are ignored `out/` artifacts). This file records the
commands, identity and receipts of the measurement runs; the result
manifest is [c4-manifest.md](c4-manifest.md) and the gate verdict is
[c4-gate.md](c4-gate.md).

## 1. Build and tool identity (observed)

| Input | Value |
|---|---|
| Engine commit | `ba796f74f` (base of this lane; = main) |
| Build | `sh /tmp/opencode/cc-gate-lock.sh mise exec -- zig build --cache-dir "$PWD/.zig-cache"` → **rc=0** (log `out/build-test.log`; one benign `__yield` implicit-declaration warning in `AtomicsObject.cpp`, pre-existing) |
| Profile | `test` (shared, no-JIT, RelWithDebInfo/Test) |
| Zig | 0.17.0 (mise-pinned; Homebrew zig is 0.16.0 and fails on `std.builtin.Optimize` — the mise pin is load-bearing) |
| Target / SDK | `aarch64-macos.26.0`, `MacOSX.sdk@26.5` (build log line `b2 profile:`) |
| ICU | Homebrew icu4c **79.1** (`/opt/homebrew/opt/icu4c`, `libicuuc.79.1.dylib`) |
| Python | 3.12.13 (`mise exec`) |
| `ch` SHA-256 | `bc74751bd857da0c6dab51d66d73761f18bc877f05aff9780eeefcfafce99f09` |
| `libChakraCore.dylib` SHA-256 | `cc89d40bc91aaccb6e2a5d0852e720f883a17477ff5da8b6de7f7334d57a287f` |

## 2. The certification run (observed)

Pinned corpus prepared once from the lockfile revision (local copy of the
shared clean checkout; `git -C out/test262 rev-parse HEAD` →
`2e0a56762801e275a9fdf96dc49d90ba0cddcf63`, `status --porcelain` empty).
Harness dependencies: `npm --prefix tools/test262 ci --ignore-scripts`.

Command (through the shared gate lock; default host flags, default
patterns, default `--jobs 8` / `--timeout 10000`):

```sh
sh /tmp/opencode/cc-gate-lock.sh python3 tools/test262/run.py \
    --binary zig-out/bin/ch --output out/test262-c4-cert --jobs 8 \
    > out/test262-c4-cert.log 2>&1        # rc=1 (failures exist, by design)
```

Log head/tail (verbatim):

```text
Running controls: ['test/**/*.js']
Controls passed: 25 scenarios (including deliberate failures)
Running corpus: ['test/language/**/*.js', 'test/built-ins/**/*.js', 'test/annexB/**/*.js']
...
test262: 77268 passed / 15873 failed; 93141 scenarios, 48598 files
Summary: .../out/test262-c4-cert/summary.json
```

- Node **v26.5.0** (identical to the marker-era baseline run — comparable).
- Host flags `-nonative -Test262 -CollectGarbage -NegativePhaseReport`;
  corpus wall ≈ **19 min** (`controls.json` 18:23:26Z → `summary.json`
  18:42:28Z mtimes), 8 jobs.
- Corpus revision verified again after the run by `run.py` (it re-checks
  HEAD and tree cleanliness, and re-fingerprints binary/dylib/harness
  inputs — all unchanged mid-run).

Analysis (committed output: [`tools/test262/c4-cert-analysis.json`](../tools/test262/c4-cert-analysis.json);
text report `out/test262-c4-cert-analysis.txt`):

```text
broad: 77268 passed / 15873 failed / 93141 scenarios / 48598 files (82.96% passed)
edition_selected: 76464 passed / 4080 failed / 80544 scenarios / 42203 files (94.93% passed)
excluded_from_edition: {'scenarios': 12597, 'passed': 804, 'failed': 11793, 'post-ES2025-proposal': 12597}
skipped: {'scenarios': 902, 'reasons': {'blocking-host': 4, 'duplicate-module-scenario': 828, 'unsupported-optional-HTMLDDA-host': 70}, ...}
unknown_feature_tags_treated_required: []
failure_buckets:
  post-ES2025: 11793
  disabled-flag: 1734
  engine-defect-on-existing: 1700
  host: 426
  required-feature-missing: 220
```

## 3. Delta join vs the marker-era baseline (observed)

Key-join of `out/test262-c4-cert/results.json` against the
c1-negative-phase baseline `results.json` (77,257/15,884; read-only from
that worktree's `out/test262-np-baseline`), key `(file, scenario)`:

- 93,141 common keys, identical key sets.
- **fail→pass 11, pass→fail 0.**
- All 11 are the s12-module-phase lane's exact fixes
  (merge `8f17ba38d`): 9 × `test/language/module-code/import-attributes/*`
  + `instn-star-err-not-found.js` + `test/language/import/import-attributes/json-invalid.js`.
- The lanes merged after that baseline with **zero** corpus flips:
  gc-async-continuation (`a5b37ed44` — its microtask-ordering repair is
  covered by the regression gate's `es7/async-microtask-order.js`, not by
  any corpus scenario), dbg-async-attach, E1/S6e repair lanes.
- Negative-phase downgrades 12 → **1** (the survivor is the post-ES2025-
  excluded `import-defer` scenario, per the s12 receipt); the 10
  module-code wrong-phase passes and `json-invalid` now genuinely pass.

## 4. Feature probes (observed)

- Committed probe: `zig-out/bin/ch feature-probe.js  # CWD must be tools/test262 (module fixtures resolve against CWD)` →
  `FEATURE-PROBE-COMPLETE`; transcript quoted in
  [c4-manifest.md](c4-manifest.md) §5.
- Extended ES2025 probe (`out/test262-c4-cert/es2025-probe.js`, saved from
  `out/scratch/`, fixture `c4-probe-fixture.json` beside it; default-output
  transcript `out/test262-c4-cert/es2025-probe-default.txt`), run three
  times: default flags, `-ESSharedArrayBuffer`, and
  `-ES6RegExSymbols -ES6RegExPrototypeProperties`. Two probe notes for
  honesty: `new RegExp('(?i:a)b').test('A')` → `false` is the *correct*
  result (the modifier scopes to `a` only; the parse itself succeeding is
  the feature evidence), and the import-attributes probe value
  `"undefined"` is the module expression-statement completion (the module
  loaded, linked and evaluated).

## 5. Gate receipts on this lane's build (observed)

All through the shared gate lock (`out/run-gates.sh`); logs
`out/check-test.log`, `out/check-debug-regression.log`.

| Gate | Result |
|---|---|
| `zig build` (test profile) | rc=0 |
| `zig build check` (test profile) | **rc=0** — `check_regression: PASS: passed 2038 / failed 0`; `verify_bytecode_pure: source headers unchanged; regen exit 0` ("Bytecode is up to date", nojit 32b/64b headers); `check_native: PASS: test-shared-basic` (8 static suites ignored in the shared profile, covered by debug-static below); `check_smoke` PASS; `check_metadata` PASS |
| `zig build -Dprofile=debug-static check-regression` | **rc=0** — `Total: passed 2090, failed 0`, `check_regression: PASS` |

(The `FAIL:` lines inside `check-test.log` are the check scripts'
self-test *negative controls* — each is followed by `-> PASS`; the step
exit codes and the quoted totals above are the authoritative receipts.)

## 6. Raw artifacts (ignored `out/`, not committed)

`out/test262-c4-cert/` (`results.json` 113 MB-class, `summary.json`,
`controls.json`, `*.stderr.log`, probe transcript),
`out/test262-c4-cert.log`, `out/test262-c4-cert-analysis.{json,txt}`,
`out/build-test.log`, `out/check-test.log`,
`out/check-debug-regression.log`, `out/run-gates.sh`.
