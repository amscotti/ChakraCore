# Benchmark baseline receipt — darwin/arm64

First measured baseline for the in-tree benchmark suites. This is a
receipt, not a performance claim: no cross-machine or cross-engine
conclusion may be drawn from these numbers. Use it as the same-host
reference for future A/B comparisons.

## Provenance

| Field | Value |
|---|---|
| Date | 2026-11-11 |
| Host | Apple M1 Max, macOS 26.6 (25G72), darwin/arm64 |
| Branch | `modernize/baseline` |
| Base commit | `61b6af3f6b32642033757667aaa8e5c2d786aa56` plus uncommitted changes |
| Uncommitted changes at record time | `lib/Backend/Lower.cpp`, `lib/Backend/arm64/LowerMD.cpp`, `lib/Backend/arm64/LowerMD.h` (ARM64 JIT write barrier), `test/es7/jit-write-barrier.js`, `test/es7/jit-write-barrier.baseline`, `test/es7/rlexe.xml`, `tools/bench/` |
| Binary | `out/jit/bin/ch` (`zig build -Dprofile=jit`) |
| Binary SHA-256 | `6692be5ab852e3782db55d19e3b537f8fb5c913ac2ffce70ce2f869a5b6549cb` |
| Harness | `tools/bench/run.py` |
| Iterations | 5 per test; median and MAD reported |
| Timeout | 120 s (JIT tier), 180 s (interpreter tier) |

Raw results:

- `tools/bench/results/baseline-jit-darwin-arm64.json` — tier `jit`, flags `[]`
- `tools/bench/results/baseline-interp-darwin-arm64.json` — tier `interp`, flags `["-nonative"]`

Reproduce:

```
mise exec -- zig build -Dprofile=jit
python3 tools/bench/run.py --binary out/jit/bin/ch --tier jit \
    --iterations 5 --timeout 120 \
    -o tools/bench/results/baseline-jit-darwin-arm64.json
python3 tools/bench/run.py --binary out/jit/bin/ch --tier interp \
    --iterations 5 --timeout 180 \
    -o tools/bench/results/baseline-interp-darwin-arm64.json
```

The harness never adds `-dynamicProfileCache` (the legacy
`perftest.pl` does for some suites). Scores across tiers or machines
are never mixed.

## Scope and exclusions

Suites: SunSpider (26), Kraken (14), Octane (15), JetStream 1.1 C/C++
subset (10). 65 files total.

| Test | Status | Reason |
|---|---|---|
| `Octane/zlib.js` | excluded on darwin/arm64 | `use asm`; the asm.js/WASM backend is x86/x64-only (`ENABLE_WASM`), so it falls back to plain JS and times out. Run on Linux x64. Port plan: [wasm-arm64-plan.md](wasm-arm64-plan.md). |
| ARES-6 | not included | Browser harness; not runnable from `ch` in this form. |
| `JetStream/cdjs.js` (interp + simplejit tiers) | flaky crash | Intermittent SIGSEGV under `-nonative` (3/6) and `-off:fullJit` (4/6); clean with `-off:ConcurrentMark` (10/10) and clean under full JIT (5/5 in this receipt). Pre-existing: also crashes 5/6 on a test-profile binary built before the write-barrier change. Carded in [open-cards.md](open-cards.md); JIT-tier numbers are unaffected. |
| `JetStream/gcc-loops.cpp.js` (interp tier) | timeout at 180 s | Interpreted run exceeds the 180 s timeout on all 5 iterations (JIT wall ≈ 8.9 s per run). Verified pure slowness, not a hang: a single run with a 900 s timeout completed in 263 s, score 13.9. No interp median recorded at the 180 s budget. |

Correctness came first: every applicable test must exit 0 and print
its `### TIME:` / `### SCORE:` marker before any number is recorded.
The first smoke run surfaced four full-JIT crashes (`Octane/splay.js`,
`richards.js`, `box2d.js`, `earley-boyer.js`). Root cause: the ARM64
full JIT emitted no software write barrier
(`LowererMD::ChangeToWriteBarrierAssign` was a no-barrier stub), so
JIT stores into already-scanned objects were invisible to concurrent
marking on software-card-table platforms. Windows write-watch had
historically masked this. The barrier is now implemented in
`lib/Backend/arm64/LowerMD.cpp`; regression coverage is
`test/es7/jit-write-barrier.js`. After the fix the JIT tier is 64/64
applicable tests passing. The interpreter tier is 62 pass + 1 flaky
crash + 1 timeout (table above); the crash is a separate pre-existing
interpreter/concurrent-GC defect, carded rather than hidden.

## Results

`TIME` values are milliseconds (lower is better). `SCORE` values are
suite scores (higher is better). Median of 5 runs; MAD in the same
unit.

<!-- BENCH-TABLE-START -->
| Test | Kind | JIT median ± MAD | Interp median ± MAD |
|---|---|---|---|
| `JetStream/bigfib.cpp.js` | score | 231.1 ± 0.3 | 6.2 ± 0.0 |
| `JetStream/cdjs.js` | score | 200.0 ± 2.0 | 22.0 (flaky) |
| `JetStream/container.cpp.js` | score | 305.1 ± 0.3 | 10.1 ± 0.0 |
| `JetStream/dry.c.js` | score | 146.0 ± 0.3 | 2.4 ± 0.0 |
| `JetStream/float-mm.c.js` | score | 448.6 ± 8.1 | 4.8 ± 0.0 |
| `JetStream/gcc-loops.cpp.js` | score | 204.0 ± 0.7 | timeout |
| `JetStream/hash-map.js` | score | 332.4 ± 1.9 | 12.2 ± 0.0 |
| `JetStream/n-body.c.js` | score | 204.2 ± 0.7 | 2.9 ± 0.0 |
| `JetStream/quicksort.c.js` | score | 276.3 ± 1.2 | 7.8 ± 0.0 |
| `JetStream/towers.c.js` | score | 221.7 ± 1.9 | 3.8 ± 0.0 |
| `Kraken/ai-astar.js` | time | 934.0 ± 1.0 | 3634.0 ± 11.0 |
| `Kraken/audio-beat-detection.js` | time | 5497.0 ± 23.0 | 2442.0 ± 4.0 |
| `Kraken/audio-dft.js` | time | 32.0 ± 1.0 | 2033.0 ± 10.0 |
| `Kraken/audio-fft.js` | time | 5454.0 ± 56.0 | 2386.0 ± 21.0 |
| `Kraken/audio-oscillator.js` | time | 1692.0 ± 10.0 | 1971.0 ± 10.0 |
| `Kraken/imaging-darkroom.js` | time | 73.0 ± 1.0 | 3286.0 ± 8.0 |
| `Kraken/imaging-desaturate.js` | time | 90.0 ± 0.0 | 5094.0 ± 110.0 |
| `Kraken/imaging-gaussian-blur.js` | time | 74.0 ± 0.0 | 14350.0 ± 56.0 |
| `Kraken/json-parse-financial.js` | time | 26.0 ± 0.0 | 26.0 ± 0.0 |
| `Kraken/json-stringify-tinderbox.js` | time | 18.0 ± 0.0 | 18.0 ± 0.0 |
| `Kraken/stanford-crypto-aes.js` | time | 621.0 ± 5.0 | 650.0 ± 3.0 |
| `Kraken/stanford-crypto-ccm.js` | time | 171.0 ± 1.0 | 425.0 ± 3.0 |
| `Kraken/stanford-crypto-pbkdf2.js` | time | 218.0 ± 1.0 | 1212.0 ± 1.0 |
| `Kraken/stanford-crypto-sha256-iterative.js` | time | 46.0 ± 0.0 | 391.0 ± 1.0 |
| `Octane/box2d.js` | score | 59158.0 ± 892.0 | 1956.0 ± 4.0 |
| `Octane/code-load.js` | score | 28151.0 ± 64.0 | 28473.0 ± 277.0 |
| `Octane/crypto.js` | score | 2648.0 ± 33.0 | 585.0 ± 1.0 |
| `Octane/deltablue.js` | score | 39320.0 ± 529.0 | 293.0 ± 1.0 |
| `Octane/earley-boyer.js` | score | 11022.0 ± 11.0 | 1225.0 ± 3.0 |
| `Octane/gbemu.js` | score | 74160.0 ± 72.0 | 3076.0 ± 2.0 |
| `Octane/mandreel.js` | score | 33463.0 ± 427.0 | 415.0 ± 0.0 |
| `Octane/navier-stokes.js` | score | 3311.0 ± 20.0 | 822.0 ± 1.0 |
| `Octane/pdfjs.js` | score | 30015.0 ± 29.0 | 3659.0 ± 30.0 |
| `Octane/raytrace.js` | score | 66451.0 ± 666.0 | 804.0 ± 1.0 |
| `Octane/regexp.js` | score | 6792.0 ± 27.0 | 1450.0 ± 11.0 |
| `Octane/richards.js` | score | 24365.0 ± 64.0 | 342.0 ± 2.0 |
| `Octane/splay.js` | score | 21220.0 ± 164.0 | 3040.0 ± 9.0 |
| `Octane/typescript.js` | score | 74217.0 ± 1106.0 | 6955.0 ± 42.0 |
| `Octane/zlib.js` |  | excluded | excluded |
| `SunSpider/3d-cube.js` | time | 10.0 ± 1.0 | 30.0 ± 0.0 |
| `SunSpider/3d-morph.js` | time | 3.0 ± 0.0 | 22.0 ± 0.0 |
| `SunSpider/3d-raytrace.js` | time | 15.0 ± 0.0 | 38.0 ± 0.0 |
| `SunSpider/access-binary-trees.js` | time | 4.0 ± 0.0 | 26.0 ± 0.0 |
| `SunSpider/access-fannkuch.js` | time | 7.0 ± 0.0 | 81.0 ± 0.0 |
| `SunSpider/access-nbody.js` | time | 4.0 ± 0.0 | 36.0 ± 0.0 |
| `SunSpider/access-nsieve.js` | time | 3.0 ± 0.0 | 27.0 ± 0.0 |
| `SunSpider/bitops-3bit-bits-in-byte.js` | time | 1.0 ± 0.0 | 31.0 ± 0.0 |
| `SunSpider/bitops-bits-in-byte.js` | time | 2.0 ± 0.0 | 50.0 ± 0.0 |
| `SunSpider/bitops-bitwise-and.js` | time | 3.0 ± 0.0 | 42.0 ± 0.0 |
| `SunSpider/bitops-nsieve-bits.js` | time | 101.0 ± 1.0 | 57.0 ± 0.0 |
| `SunSpider/controlflow-recursive.js` | time | 6.0 ± 0.0 | 44.0 ± 0.0 |
| `SunSpider/crypto-aes.js` | time | 24.0 ± 0.0 | 32.0 ± 0.0 |
| `SunSpider/crypto-md5.js` | time | 11.0 ± 0.0 | 27.0 ± 0.0 |
| `SunSpider/crypto-sha1.js` | time | 8.0 ± 0.0 | 27.0 ± 0.0 |
| `SunSpider/date-format-tofte.js` | time | 15.0 ± 1.0 | 32.0 ± 1.0 |
| `SunSpider/date-format-xparb.js` | time | 22.0 ± 0.0 | 23.0 ± 0.0 |
| `SunSpider/math-cordic.js` | time | 5.0 ± 0.0 | 58.0 ± 1.0 |
| `SunSpider/math-partial-sums.js` | time | 4.0 ± 0.0 | 22.0 ± 0.0 |
| `SunSpider/math-spectral-norm.js` | time | 2.0 ± 0.0 | 33.0 ± 0.0 |
| `SunSpider/regexp-dna.js` | time | 4.0 ± 0.0 | 4.0 ± 0.0 |
| `SunSpider/string-base64.js` | time | 5.0 ± 0.0 | 19.0 ± 0.0 |
| `SunSpider/string-fasta.js` | time | 7.0 ± 0.0 | 37.0 ± 0.0 |
| `SunSpider/string-tagcloud.js` | time | 11.0 ± 0.0 | 22.0 ± 0.0 |
| `SunSpider/string-unpack-code.js` | time | 16.0 ± 0.0 | 29.0 ± 0.0 |
| `SunSpider/string-validate-input.js` | time | 7.0 ± 0.0 | 18.0 ± 0.0 |
<!-- BENCH-TABLE-END -->

## How to use this receipt

- Compare only same-host, same-tier, A/B runs. Interleave A/B/A and
  compare per-test medians; treat a consistent >10–15% median shift
  as an investigation trigger, not an automatic fail.
- SunSpider/Kraken tests run 1–10 ms; noise dominates there. Prefer
  Octane/JetStream medians for JIT decisions.
- Like-for-like tiers only: JIT vs another JIT engine; `-nonative`
  vs interpreter-only engines. Cross-engine receipts (Node/V8,
  QuickJS-ng) are separate future receipts, not part of this one.
- Record separate receipts per platform. This one says nothing about
  Linux x64, where `Octane/zlib.js` becomes applicable.
- No CI gate reads this file yet. The measured-perf-gate card in
  `doc/open-cards.md` tracks adding one (fast subset, same-host A/B
  threshold, no absolute score gates).
