# Pinned test262 diagnostic baseline

The legacy `test/262` directory contains **one** fixture; the 1968-case regression
gate is not a current ECMAScript conformance score. This tooling establishes a
repeatable upstream baseline **before** feature implementation.

## Pins and scope

- Upstream corpus: `test262.lock.json`, revision
  `2e0a56762801e275a9fdf96dc49d90ba0cddcf63` (2026-10-08).
- Runner: upstream **test262-harness 10.0.0 / eshost**, exact dependency graph and
  integrity hashes in `package-lock.json`. Node 22+ and Python 3.11+ are required
  for this development tool, not for building or embedding the engine.
- Default selection: `test/language`, `test/built-ins`, `test/annexB`. Intl402 and
  staging can be selected explicitly with quoted patterns.
- Default host flags: `-nonative -Test262 -CollectGarbage -NegativePhaseReport`
  (the last one enables the host's negative-phase marker; see
  [doc/c1-negative-phase.md](../../doc/c1-negative-phase.md)). Language flags
  such as `ESBigInt` / `ESSharedArrayBuffer` keep the engine's defaults unless
  explicitly selected in a separately named run.

The corpus follows the current draft and includes **post-ES2025 proposals**.
Its broad pass rate is diagnostic, not an ES2025 conformance claim. The C1
edition policy (`edition-policy.json`, method in
[doc/c1-triage.md](../../doc/c1-triage.md)) plus `analyze.py` produce the
edition-selected score; full host validation and explicit negative-phase
reporting are still required before using any result as the Phase 3 exit gate.

The initial [recorded Mac baseline](../../doc/conformance-baseline.md) is
61,565 passed / 31,576 failed across 93,141 scenarios; edition-selected it is
60,896 / 19,648 across 80,544 scenarios (`c1-baseline-analysis.json`).
Passing negative tests for unsupported syntax do not prove that the feature
is implemented.

The npm overrides keep the pinned harness's dependency graph current at the
specific affected leaves: debug/parseuri/cookie and the js-yaml CLI's unused
argparse dependency. This runner uses the YAML library, not that CLI. Installation
and the native-host controls were checked with the resulting lockfile; `npm audit`
reports zero known findings at verification time. Browser-host support is outside
this tool's scope.

## Install and run

From the repository root:

```sh
npm --prefix tools/test262 ci --ignore-scripts
python3 tools/test262/run.py --prepare --binary build-zigpilot/ch
```

The first command installs only development tooling. `--prepare` initializes / fetches
the exact locked revision into ignored `out/test262/`; later runs verify its HEAD
and clean tree. Results go to ignored `out/test262-results/` by default. Use
separate output directories to retain comparisons:

```sh
python3 tools/test262/run.py --binary build-xplat/ch --output out/test262-legacy
python3 tools/test262/run.py --binary build-zigpilot/ch --output out/test262-zig
python3 tools/test262/run.py --binary build-zigpilot/ch --controls-only
python3 tools/test262/run.py --binary build-zigpilot/ch --output out/test262-regex \
  'test/built-ins/RegExp/**/*.js' 'test/language/literals/regexp/**/*.js'
```

`--jobs` defaults to 8; `--timeout` is per-scenario milliseconds (default 10000).
Both are recorded in `summary.json` (`jobs`, `timeout_ms`). Each agent writes
its scenario sources under its own `agent-N/` subdirectory of the per-run
temp dir (see `harness.js`), so parallel agents cannot overwrite each
other's same-relative-path files; `analyze.py` reports carry the
`edition-policy.json` SHA-256 they were computed against.
`--host-flags='…'` explicitly records an alternate feature configuration. A corpus
with conformance failures returns **1**; a failed control, modified checkout,
harness failure, invalid/no results returns **2**. Neither becomes a successful
conformance check just because a host invocation returned zero.

## Controls and host adaptation

Each run first verifies twenty-four fixtures under `controls/test/` (recursively): successful sync,
async, module, parse-negative, runtime-negative, realm and buffer-detachment cases;
plus deliberately failing assertions, wrong negative error types, and nonzero
empty-output exits. All expected outcomes must match. This catches the shell's
previously discovered silent rc=0 callback failure.

Eight C1 fixtures additionally qualify negative **phases**: a parse negative
whose preceding side-effect statements must not execute, a resolution
(module-link) negative, a runtime `eval` SyntaxError negative, and five
deliberate wrong-phase masquerades (code that executes and throws
`SyntaxError` where parse/resolution was declared — including silent
variants and a throw from a function named `code` — plus a parse error
declared as runtime). The upstream validator checks only the error name
and passes the masquerades, so `run.py` downgrades
a control pass whose recorded evidence contradicts the declared phase:
stdout execution markers, the tab-prefixed `\tat code (` parse frame, the
`thrown at …:` module-loader prefix together with an empty JS stack, and
Global/Module code runtime frames.

**Host-reported phase (C4 preparation).** `ch -NegativePhaseReport` now
emits exactly one machine-readable marker line on stderr at the first
unhandled-error report — `CH_NEGATIVE_PHASE: parse|resolution|runtime` —
classified from *where the failure originated* (which JsRT entry point
failed and at which stage), never from error-message text; with the flag
off, `ch` output is byte-identical. `run.py` passes the flag by default
and re-judges **every** negative scenario (controls and corpus) against
the marker, keeping the shape heuristic above only as a fallback for
marker-less records (crashes, timeouts, pre-flag hosts). Four further
controls cover the marker itself, including deliberately misleading
shapes that only the real marker can catch: a dependency module that
fails to parse (a resolution-phase graph-loading failure whose
loader-prefix shape would also satisfy a declared parse phase), and a
stackless throw from module evaluation (a runtime-phase failure whose
`thrown at` loader shape would also satisfy a declared resolution
phase). The classification mapping, its source anchors and its honest
limits (deep-child link errors, deferred-parse early errors) are in
[doc/c1-negative-phase.md](../../doc/c1-negative-phase.md).

`harness.js` retains the upstream compiler, error parser and runner, adding only
testing-host adaptation:

- native `WScript.monotonicNow` instead of the agent's Date.now approximation;
- ArrayBuffer detachment through existing `SerializeObject`/`Deserialize` JsRT
  transfer hooks (`bin/ch/WScriptJsrt.cpp:308-405,443-485`);
- explicit child exit/signal capture so a crash or nonzero empty-output exit
  cannot pass (including negative tests);
- no fake HTMLDDA exotic.

`host-filter.js` reports every host skip to the stderr log: optional `IsHTMLDDA`
tests, `CanBlockIsFalse` tests for this blocking host, and the duplicate strict
module scenario generated by the upstream stream. It does not skip unsupported
language features. The original module source runs once, as test262 requires.

## Reports and interpretation

- `controls.json`: expected pass/fail controls, separate from corpus statistics.
- `results.json`: full JSON scenario results, metadata and raw host output.
- `*.stderr.log`: harness diagnostics and explicit skip records.
- `summary.json`: corpus revision, binary/shared-library hashes, dependency-lock
  hash, Node version, command/flags/patterns, scenario and unique source-file
  totals, pass/fail totals, skip reasons and per-feature counts.

The runner fingerprints binary/library and harness inputs before execution and
rejects a run if they or the pinned checkout change during it. Result arrays and
the complete, single-scenario control coverage must be well-formed.

Feature counts overlap because tests can carry several tags; they do not sum to
the corpus total. Strict/default scenarios count separately, except module/raw
tests. Preserve the revision, configuration and host skips when comparing runs.

The upstream validator checks negative **error names**, not every distinction
between parse, resolution and runtime phases. With the host-reported phase
marker (`-NegativePhaseReport`) the runner re-judges every negative scenario
by the stage the failure actually originated from; the C1 shape heuristic
remains only as the marker-less fallback. Schema additions for marker runs:
each negative record in `results.json` carries a `phase_evidence` object
(`declared`/`marker`/`shape`/`executed`/`judged_by`), downgraded mismatches
carry `result.message` starting `Negative phase mismatch:`, `summary.json`
gains a `negative_phase` section, and `analyze.py`'s audit adds
`marker`/`marker_judged_total`/`marker_downgraded_by_run_py` fields (legacy
records keep their exact C1 shape-audit semantics).

## Edition policy and analysis (C1)

`edition-policy.json` classifies every feature tag in the recorded baseline
(151 required-ES2025, 27 post-ES2025-proposal, plus the host-optional
`IsHTMLDDA`/`CanBlockIsFalse` skips), each exclusion with a one-line
rationale anchored to the TC39 finished-proposals list or the corpus's own
`features.txt`. A scenario is edition-selected iff none of its tags is
post-ES2025 or host-optional; untagged scenarios stay in, and unknown tags
are treated as required (never excluded on doubt).

```sh
python3 tools/test262/analyze.py --self-test
python3 tools/test262/analyze.py --results out/test262-zig/results.json \
  --summary out/test262-zig/summary.json \
  --out-json tools/test262/c1-baseline-analysis.json
```

`analyze.py` (Python stdlib only) emits a small JSON + text report: broad vs
edition-selected passed/failed/skipped and unique files, per-feature
edition counts, failure buckets (required-feature-missing,
engine-defect-on-existing, disabled-flag, host, post-ES2025 — first-match
rules in the module docstring), top failure messages with co-occurring
tags, and the negative-phase audit heuristic over passed negatives. The
recorded-baseline output is committed as `c1-baseline-analysis.json`; the
triage and shortlist built on it are in
[doc/c1-triage.md](../../doc/c1-triage.md).

## Small feature observations

```sh
./build-xplat/ch tools/test262/feature-probe.js
./build-zigpilot/ch tools/test262/feature-probe.js
```

Expect JSON observations plus `FEATURE-PROBE-COMPLETE`. These runtime probes confirm
inherited ES2018 RegExp gaps, later syntax/builtin gaps and default-off features;
they also positively exercise flat/flatMap and error cause. They are an inventory
aid, not a substitute for test262.
