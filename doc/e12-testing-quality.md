# E12 — Testing quality: baseline-refresh scripting, GCStress campaign, flake hardening

Lane receipt (2026-11-05, base `4e9f95da7`, mainline `modernize/baseline`).
Three deliverables, three commits; no engine-source changes in this lane
(`git diff HEAD -- lib pal bin` empty throughout). Gates stay pinned at
**2051/2103** — the one test-tree change is a `<timeout>` attribute, which
changes no entry count.

## 1. One-command certified-baseline refresh (`refresh_baseline.py`)

**The tool.** `tools/test262/refresh_baseline.py` collapses the C4
certification procedure (doc/c4-run-evidence.md §2) into one manual
command: (1) the certified full-corpus run through `run.py` — default
host flags `-nonative -Test262 -CollectGarbage -NegativePhaseReport`
(asserted, not just defaulted), default patterns, `--jobs 8`,
`--timeout 10000`, absolute binary, controls first; (2) `analyze.py`
(edition policy, buckets, negative-phase audit) with its json/txt written
beside the run; (3) a per-key `(file, scenario)` delta join against the
previous certified run with attribution hints — the engine commits
(`git log` lib/pal/bin) between the two runs' recorded `engine_commit`s
plus the doc/ receipts naming each flipped file; (4) a dated markdown
report. Previous runs resolve from `--previous` or the newest
`tools/test262/baseline-register.json` entry retaining its results.json;
`--publish` appends the run to the register. Nothing invokes it
automatically. `run.py` now records `engine_commit` +
`engine_tree_dirty` in `summary.json` (evidence metadata; the binary
hash remains the identity that matters). `--self-test` covers the join
(flip detection both directions, key-drift, duplicate-key rejection)
and the attribution helpers, including a regression pin that a commit
range placed after git's `--` pathspec separator returns the whole
history instead of the range (bug found and fixed while validating).

**Validation (the pre-C4 health run, 2026-11-05).** Built HEAD through
the shared gate lock (`zig build` rc=0, one known benign `__yield`
warning; `ch` SHA-256 `5e210836…fea7028`), then:

```sh
sh /tmp/opencode/cc-gate-lock.sh python3 tools/test262/refresh_baseline.py \
    --binary "$PWD/zig-out/bin/ch" --output out/test262-e12-refresh
```

Corpus `2e0a56762…` (unchanged), Node v26.5.0, controls 25/25, skips 902
(828 duplicate-module + 70 HTMLDDA + 4 CanBlockIsFalse — unchanged),
8,751 negatives marker-judged, 1 downgrade (the known post-ES2025-excluded
`import-defer` scenario), 0 shape suspects. Totals:

| measure | c4-cert (2026-11-02) | this run | delta |
|---|---:|---:|---:|
| broad | 77,268 / 15,873 (82.96%) | **78,399 / 14,742 (84.17%)** | **−1,131 failures** |
| edition-selected | 76,464 / 4,080 (94.93%) | **77,595 / 2,949 (96.34%)** | **−1,131 failures** |
| host bucket | 426 | **3** | −423 |
| engine-defect-on-existing | 1,700 | **1,010** | −690 |
| required-feature-missing | 220 | **202** | −18 |
| disabled-flag / post-ES2025 | 1,734 / 11,793 | 1,734 / 11,793 | 0 |

The −1,131 matches the e/b-lane receipts' booked fail→pass deltas
(dynamic-import host −423 host-bucket + others, e3 +130, e4 +124,
e5 +112, e6 +156 wide, e7 +40, e9 +18, e10 +22, e11 +8, plus the
card-sweep's TA-OOB-delete corpus flips — the wide-selection overlaps
resolve inside the 23 engine commits `ba796f74f..4e9f95da7`). Honesty
notes: the tree was dirty at run time with this lane's tool/doc files
only (`git status`: `tools/test262/*`, `doc/README.md`; zero lib/pal/bin
changes — the binary was built at HEAD before the run, and run.py's
artifact fingerprints verified it mid-run); and the per-key join vs
c4-cert could not run because that worktree's 113 MB results.json was
not retained — the join machinery is instead validated on a real
on-disk pair (the b9 lane's 18,593-key before/after: exactly the
receipt's 22 fail→pass / 0 pass→fail, e10 receipt hint matched) plus the
self-test; **this run's results are retained** and registered, so the
C4 refresh joins against it per-key. Analysis committed:
`tools/test262/e12-refresh-analysis.json`; report
`out/test262-e12-refresh/refresh-report.md`.

## 2. GCStress campaign (bounded, rlexe core)

**Flag surface, honestly enumerated** (observed `ch -?` + source anchors on
this exact build; the flag list is generated from the compiled table, so
`-?` is the compiled-in truth):

- The classic full-Chakra host flags **do not exist here**:
  `-ForceGCTimeout:100 …` → `Invalid Flag : -ForceGCTimeout:100` (receipt
  `out/e12-ch-flaglist.txt`); `-TrackGCTimeout`/`-GcStress` likewise, and
  `git log -S ForceGCTimeout -- lib/Common/ConfigFlagsList.h` is empty —
  they never existed in this tree (they are MSHTML-host ch flags).
- Compile-out on this build: `-RecyclerVerifyMark`
  (`#ifdef RECYCLER_VERIFY_MARK`, ConfigFlagsList.h:1524),
  `-DumpFragmentationStats` (`#ifdef DUMP_FRAGMENTATION_STATS`, :1067) —
  neither appears in `ch -?`.
- Compiled in and used below: `-CollectGarbage`, `-GCMemoryThreshold:N`
  (bounds uncollected-MB before an allocation-initiated collection;
  Recycler.cpp:337-341 → RecyclerHeuristic `MaxUncollectedAllocBytes`),
  `-RecyclerProtectPagesOnRescan`, `-ConcurrentRuntime`,
  `-ForceBlockingConcurrentCollect`. `-RecyclerTest` parses but its
  `StressTester` is `#if DBG` (ThreadContext.cpp:803-808) — inert in the
  test profile.

**Suite campaign** (test-profile binary of §1, gate-exact
`--variants=disable_jit -j 4`, folders `es5 es6 es7 TypedArray Array` —
582 entries; first attempt with bare folder names selected 0 tests: the
loader resolves folder args against CWD, use `test/<folder>` paths):

| config | flags (added to `-WERExceptionSupport`) | result |
|---|---|---|
| baseline | none | **582/0** |
| A — allocation-forced GC | `-CollectGarbage -GCMemoryThreshold:1` | **582/0** identical |
| B — concurrent axes | `-ConcurrentRuntime -ForceBlockingConcurrentCollect -CollectGarbage` | **582/0** identical |
| C — A + rescan page protection | `-CollectGarbage -GCMemoryThreshold:1 -RecyclerProtectPagesOnRescan` | **582/0** identical |

Zero failures, crashes, asserts or timeout-kills in every config; the
pass set is identical per folder (Array 120, TypedArray 60, es5 56,
es6 238, es7 108). Config A provably stresses the GC: a churn probe
(10M objects, 40 rounds) peaks at **237.8 MB RSS by default vs 63.5 MB
with `-GCMemoryThreshold:1`** — collections really fire; the suites just
hold under them. Total campaign wall ≈ 21 min (well inside the ~2 h bound).
`-ForceGCAfterJSONParse` was enumerated but not campaigned (JSON-parse-
scoped; no JSON folder in the rlexe core selection).

**Two findings, both carded (§2.1), neither fixed here:**

1. **`bin/GCStress` standalone SIGSEGVs at startup on this build** —
   100% deterministic, ~0.02 s, empty output (block-buffered stdout lost).
   Repro: `zig-out/bin/GCStress` → rc=139; macOS crash report stack:
   `SimpleRecyclerTest` → `Recycler::EnableConcurrent` →
   `Recycler::InitializeConcurrent` → `JsUtil::ThreadService::HasCallback()`
   with `threadService == nullptr` (KERN_INVALID_ADDRESS at 0x0). Root
   cause: GCStress.cpp constructs the recycler with
   `recyclerInstance->Initialize(false, nullptr /* threadService */)` and
   this `ENABLE_CONCURRENT_GC` + debug-config build takes the default-
   concurrent branch (Recycler.cpp:823-855) → `EnableConcurrent(nullptr)`
   → unconditional `threadService->HasCallback()` (Recycler.cpp:4818).
   Confirmed by workaround: `zig-out/bin/GCStress -js -off:ConcurrentCollect`
   disables the concurrent path and the tool runs (heap walk over 599,967
   objects in a 120 s bound; 10-min bounded run in
   `out/e12-gcstress-binary-10min.log`). The engine API contract (null
   ThreadService requires forceInThread) is violated by the *tool*, which
   predates the zig build that first shipped it on this platform.
2. **`ch -RecyclerTest` on the debug-static profile asserts deterministically**:
   `ASSERTION 41943/41946 (lib/Common/Memory/StressTest.cpp:48)
   (addr & (OBJALIGN-1)) == 0` in `TestObject::Get`, rc=133 (SIGTRAP),
   same random seed every run. Repro:
   `out/debug-static/bin/ch -RecyclerTest test/Basics/hello.js` (any
   script). The DBG StressTester trips over an object pointer that fails
   its OBJALIGN alignment check — either a recycler alignment contract
   issue on this target or StressTester bit-rot; owner: GC lane. Not
   reachable from any gate (no gate leg passes `-RecyclerTest`).

## 2.1 Cards filed

Added to [open-cards.md](open-cards.md): GCStress-tool null-ThreadService
startup crash (tooling, workaround recorded); `-RecyclerTest` StressTester
OBJALIGN assert (GC owner). Updated the c2-jit-test262 waitAsync flake
card with the §3.1 findings.

## 3. Flake-family hardening

*(filled by commit 3/3 — see §3 below as amended)*

## 4. Gates

*(recorded with commit 3/3)*
