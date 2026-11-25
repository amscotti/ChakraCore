# Benchmark baseline receipt -- darwin/arm64

Measured baseline receipts for the in-tree benchmark suites on this host.
These are receipts, not cross-machine or cross-engine performance claims:
no conclusion across different hosts or engines may be drawn from these
numbers. Use them for same-host, like-for-like A/B comparisons.

## Provenance (Integrated HEAD -- 2026-11-16)

| Field | Value |
|---|---|
| Date | 2026-11-16 |
| Host | Apple M1 Max (10 physical CPUs), macOS 26.6 (25G72), darwin/arm64 |
| Branch | `modernize/baseline` |
| Commit | `4b2077687abb0129cb690542fb56b2e4c0ac4615` (`c24266598` Stage 3 integration + Stage 4 `lib/Backend/Inline.cpp` `ENABLE_ASMJS` guard fix and `test/es7/asmjs-inline-const.js`; built from clean tree) |
| Build command | `mise exec -- zig build -Dprofile=jit -Dwasm=true` |
| Binary | `out/jit/bin/ch` |
| Binary SHA-256 | `7afbc2509d654c3ade1c5b84724b11e9b0db8defe27ce316b26c5864cbc6af1a` |
| Shared library SHA-256 (`out/jit/lib/libChakraCore.dylib`) | `a5b85e67e7e0de2e06790dd1c23a2094bb599db1b4301e7ed04d06fab96ef022` |
| Harness | `tools/bench/run.py` |
| Iterations | 5 per test; median and MAD reported |
| Timeout | 120 s (JIT tier), 180 s (interpreter tier) |

Raw results:

- `tools/bench/results/head-jit-darwin-arm64.json` -- tier `jit`, flags `[]`, sum run wall `333.5 s` (`64/64` applicable pass, `1` excluded, `0` unexpected failures)
- `tools/bench/results/head-interp-darwin-arm64.json` -- tier `interp`, flags `["-nonative"]`, sum run wall `2889.3 s` (`63/64` applicable pass, `1` excluded, `1` timeout on `JetStream/gcc-loops.cpp.js`, `0` crashes)

Reproduce:

```
mise exec -- zig build -Dprofile=jit -Dwasm=true
python3 tools/bench/run.py --binary out/jit/bin/ch --tier jit \
    --iterations 5 --timeout 120 \
    -o tools/bench/results/head-jit-darwin-arm64.json
python3 tools/bench/run.py --binary out/jit/bin/ch --tier interp \
    --iterations 5 --timeout 180 \
    -o tools/bench/results/head-interp-darwin-arm64.json
```

The harness never adds `-dynamicProfileCache` (the legacy
`test/benchmarks/perftest.pl` does for some suites). Scores across
tiers or machines are never mixed.

## Scope, exclusions, and Stage 2-4 fixes

Suites: SunSpider (26), Kraken (14), Octane (15), JetStream 1.1 C/C++
subset (10). 65 files total.

| Test | Status at HEAD (`4b2077687`) | Reason / history |
|---|---|---|
| `Octane/zlib.js` | excluded on darwin/arm64 | `use asm`; the asm.js/WASM backend is x86/x64-only (`ENABLE_WASM`), so it falls back to plain JS and times out. Run on Linux x64. Port plan: [wasm-arm64-plan.md](wasm-arm64-plan.md). |
| ARES-6 | not included | Browser harness; not runnable from `ch` in this form. |
| `JetStream/cdjs.js` | **FIXED (`5/5` PASS both tiers)** | Was a flaky SIGSEGV under `-nonative` (3/6, `2/5` pass in Nov 11 baseline) and `-off:fullJit` (4/6). Root-caused and fixed in `20ea96941` (`Stage 2 cdjs-debug`, integrated in `83ab4d28e`): `SmallHeapBlockT<TBlockAttributes>::GetRealAddressFromInterior` (`lib/Common/Memory/Recycler.inl`) truncated `(rawInteriorAddress - baseAddress)` to 16-bit `ushort` before `>> HeapConstants::ObjectAllocationShift` (`4`). On 16 KiB-page Apple Silicon, `MediumAllocationBlockAttributes::PageCount == 8` yields 128 KiB medium blocks (`> USHRT_MAX`), wrapping `[+64 KiB, +128 KiB)` interior pointers into `[0, +64 KiB)` and corrupting concurrent-GC exact mark scans. Fixed to shift in `uint` before narrowing and multiply `static_cast<size_t>(objectIndex) * objectSize`. Pinned by `test/Bugs/cdjs-mediumblock-interior-gc.js`; `KNOWN_ISSUES` cleared in `tools/bench/run.py`. Now `202.0 +/- 2.0` (JIT) and `22.5 +/- 0.1` (interp), `0` failures. |
| `JetStream/gcc-loops.cpp.js` (interp tier) | timeout at 180 s (`5/5`) | Interpreted run exceeds the 180 s per-run timeout on all 5 iterations (`wallSec = 180.0 s`). Pure slowness, not a hang (a Nov 11 900 s single run completed in 263 s). Passes cleanly on JIT at `385.6 +/- 0.7` (`wallSec ~ 4.76 s` per run). |
| `JetStream/bigfib.cpp.js` & `container.cpp.js` | **FIXED in Stage 4 (`4b2077687`)** | At `c24266598` (`-Dprofile=jit -Dwasm=true`), both crashed `5/5` (SIGSEGV at `0x8` in `JITTimeFunctionBody::GetConstantType`) because `Inline::BuildIRForInlinee` (`lib/Backend/Inline.cpp:1517`) and `Inline::InlineFunctionCommon` (`:4104`) gated `IRBuilderAsmJs` and `GetAsmJsInfo()->GetArgCount()` on `#ifdef ASMJS_PLAT` (x86/x64-only) instead of `#ifdef ENABLE_ASMJS` (defined on ARM64 with `-Dwasm=true`), falling into JS `IRBuilder::BuildConstantLoads()` on asm.js functions whose `constTableContent` is intentionally null. Switched both guards to `ENABLE_ASMJS` and pinned with `test/es7/asmjs-inline-const.js`; both now pass `5/5`. |
| `Octane/earley-boyer.js` | pass (`5/5` in receipt) | Quiet HEAD receipt passes `5/5` (`10971.0 +/- 32.0`), matching `10/10` post-fix standalone runs (`10519..11202`) and `15/15` pre-fix standalone runs on `c24266598`. An initial pre-fix 5-iteration benchmark probe on `c24266598` hit a single `1/5` `rc=-11` on run #4 (no `.ips` crash report; `lldb` launch timed out under sandbox); tracked as low-repro open card in [open-cards.md](open-cards.md). |

### Build-profile change vs Nov 11 (`-Dwasm=true` / `ENABLE_ASMJS`)

The 2026-11-11 baseline was built with `zig build -Dprofile=jit`
(`ENABLE_ASMJS` disabled), so `"use asm"` benchmarks executed as plain
JavaScript. The integrated 2026-11-16 build uses
`zig build -Dprofile=jit -Dwasm=true`, which enables `ENABLE_ASMJS` on
ARM64:

- On **interpreter** (`-nonative`), total suite wall time dropped from
  `3535.3 s` to `2889.3 s` (`-646 s`), and SunSpider (`+3.0%`), Kraken
  (`+3.6%`), and Octane (`+1.4%`) also improved modestly across the board.
- On **JetStream**, asm.js execution drove large speedups on `dry.c.js`
  (`+206.3%` JIT, `+75.1%` interp), `bigfib.cpp.js` (`+103.0%` JIT,
  `+23.7%` interp), `gcc-loops.cpp.js` (`+89.0%` JIT), `n-body.c.js`
  (`+46.5%` JIT, `+129.9%` interp), `towers.c.js` (`+23.2%` JIT, `+36.9%`
  interp), `float-mm.c.js` (`+13.1%` JIT, `+56.8%` interp), and
  `quicksort.c.js` (`+13.1%` JIT, `+8.2%` interp).
- Conversely, `JetStream/container.cpp.js` scores `120.2 +/- 0.8` under
  `ENABLE_ASMJS` (`-Trace:Asmjs` prints `"Successfully compiled asm.js code"`
  -> `121.3`) vs `305.1 +/- 0.3` (`-60.6%`) on the plain-JS Nov 11 build
  (verified with `-AsmJs-` on `out/jit/bin/ch`: `326.7`), while improving
  `+22.7%` under `-nonative` (`12.4` vs `10.1`).
- Notable non-asm.js FullJIT shift: `Octane/box2d.js` improved `+10.6%`
  (`65410.0` vs `59158.0`); all other Octane tests stayed within `-2.0%..+4.1%`.

## Suite geometric means (2026-11-16 HEAD vs 2026-11-11 baseline)

Speedups and deltas are computed via geometric mean of matched tests:

- For `time` suites (`SunSpider`, `Kraken`, lower is better):
  `JIT/Interp = interp_ms / jit_ms`; `vs Nov 11 = (nov11_ms / head_ms) - 1`.
- For `score` suites (`Octane`, `JetStream`, higher is better):
  `JIT/Interp = jit_score / interp_score`; `vs Nov 11 = (head_score / nov11_score) - 1`.
- Positive percentages (`+`) mean **faster** than 2026-11-11; negative (`-`) mean **slower**.

| Suite | Metric | Applicable (HEAD JIT/Interp) | HEAD JIT geomean | HEAD Interp geomean | HEAD JIT/Interp | Nov 11 JIT geomean | Nov 11 Interp geomean | JIT vs Nov 11 | Interp vs Nov 11 |
|---|---|---:|---:|---:|---:|---:|---:|---:|---:|
| SunSpider | time (ms, lower is better) | 26/26 | 6.59 | 28.85 | 4.38x | 6.67 | 29.70 | +1.2% | +3.0% |
| Kraken | time (ms, lower is better) | 14/14 | 207.79 | 980.93 | 4.72x | 211.87 | 1015.99 | +2.0% | +3.6% |
| Octane | score (higher is better) | 14/14 | 22786.59 | 1550.23 | 14.70x | 22430.87 | 1528.08 | +1.6% | +1.4% |
| JetStream (all passing) | score (higher is better) | 10/9 | 311.53 | 8.53 | 35.65x | 245.12 | 6.33 | +27.1% | +34.9% |
| JetStream (matched JIT+Interp, n=9) | score (higher is better) | 9/9 | 304.23 | 8.53 | 35.65x | 250.17 | 6.33 | +21.6% | +34.9% |
| JetStream (matched non-flaky, n=8) | score (higher is better) | 8/8 | 320.21 | 7.56 | 42.36x | 257.27 | 5.41 | +24.5% | +39.6% |

Note on `JetStream`: `HEAD JIT/Interp` speedup on all 9 tests that finish
in both tiers (`n=9`, excluding `gcc-loops.cpp.js` timeout in interp) is
`35.65x` (matched `n=9` geomeans: HEAD `304.23 / 8.53` vs Nov 11
`250.17 / 6.33` = `39.55x`). Including `gcc-loops.cpp.js` on JIT only gives
the full-tier JIT geomean `311.53` (`n=10`, `+27.1%` vs Nov 11 `245.12`).
Excluding both `gcc-loops.cpp.js` and `cdjs.js` (flaky in Nov 11 interp)
leaves `n=8` matched non-flaky tests (`320.21` JIT / `7.56` interp,
`+24.5%` JIT / `+39.6%` interp vs Nov 11).

## Results (2026-11-16 HEAD and Nov 11 comparison)

`TIME` values are milliseconds (lower is better). `SCORE` values are
suite scores (higher is better). Median of 5 runs; MAD in the same
unit. Deltas show speedup relative to 2026-11-11 (`+` = faster, `-` = slower).

<!-- BENCH-TABLE-START -->
| Test | Kind | HEAD JIT median +/- MAD | HEAD Interp median +/- MAD | JIT/Interp | Nov 11 JIT | JIT vs Nov 11 | Nov 11 Interp | Interp vs Nov 11 |
|---|---|---|---|---:|---|---:|---|---:|
| `JetStream/bigfib.cpp.js` | score | 469.1 +/- 1.3 | 7.6 +/- 0.0 | 61.57x | 231.1 +/- 0.3 | +103.0% | 6.2 +/- 0.0 | +23.7% |
| `JetStream/cdjs.js` | score | 202.0 +/- 2.0 | 22.5 +/- 0.1 | 8.97x | 200.0 +/- 2.0 | +1.0% | 22.0 (flaky) | +2.4% |
| `JetStream/container.cpp.js` | score | 120.2 +/- 0.8 | 12.4 +/- 0.0 | 9.69x | 305.1 +/- 0.3 | -60.6% | 10.1 +/- 0.0 | +22.7% |
| `JetStream/dry.c.js` | score | 447.1 +/- 1.2 | 4.1 +/- 0.0 | 108.25x | 146.0 +/- 0.3 | +206.3% | 2.4 +/- 0.0 | +75.1% |
| `JetStream/float-mm.c.js` | score | 507.4 +/- 1.0 | 7.5 +/- 0.0 | 67.58x | 448.6 +/- 8.1 | +13.1% | 4.8 +/- 0.0 | +56.8% |
| `JetStream/gcc-loops.cpp.js` | score | 385.6 +/- 0.7 | timeout | -- | 204.0 +/- 0.7 | +89.0% | timeout | -- |
| `JetStream/hash-map.js` | score | 338.3 +/- 0.0 | 12.5 +/- 0.0 | 27.17x | 332.4 +/- 1.9 | +1.8% | 12.2 +/- 0.0 | +1.7% |
| `JetStream/n-body.c.js` | score | 299.1 +/- 0.7 | 6.6 +/- 0.0 | 45.20x | 204.2 +/- 0.7 | +46.5% | 2.9 +/- 0.0 | +129.9% |
| `JetStream/quicksort.c.js` | score | 312.6 +/- 0.8 | 8.4 +/- 0.0 | 37.22x | 276.3 +/- 1.2 | +13.1% | 7.8 +/- 0.0 | +8.2% |
| `JetStream/towers.c.js` | score | 273.0 +/- 1.0 | 5.2 +/- 0.0 | 52.00x | 221.7 +/- 1.9 | +23.2% | 3.8 +/- 0.0 | +36.9% |
| `Kraken/ai-astar.js` | time | 889.0 +/- 1.0 | 3501.0 +/- 7.0 | 3.94x | 934.0 +/- 1.0 | +5.1% | 3634.0 +/- 11.0 | +3.8% |
| `Kraken/audio-beat-detection.js` | time | 5594.0 +/- 32.0 | 2377.0 +/- 3.0 | 0.42x | 5497.0 +/- 23.0 | -1.7% | 2442.0 +/- 4.0 | +2.7% |
| `Kraken/audio-dft.js` | time | 29.0 +/- 0.0 | 1797.0 +/- 22.0 | 61.97x | 32.0 +/- 1.0 | +10.3% | 2033.0 +/- 10.0 | +13.1% |
| `Kraken/audio-fft.js` | time | 5539.0 +/- 16.0 | 2305.0 +/- 7.0 | 0.42x | 5454.0 +/- 56.0 | -1.5% | 2386.0 +/- 21.0 | +3.5% |
| `Kraken/audio-oscillator.js` | time | 1672.0 +/- 4.0 | 1865.0 +/- 1.0 | 1.12x | 1692.0 +/- 10.0 | +1.2% | 1971.0 +/- 10.0 | +5.7% |
| `Kraken/imaging-darkroom.js` | time | 67.0 +/- 0.0 | 3144.0 +/- 16.0 | 46.93x | 73.0 +/- 1.0 | +9.0% | 3286.0 +/- 8.0 | +4.5% |
| `Kraken/imaging-desaturate.js` | time | 91.0 +/- 0.0 | 4961.0 +/- 1.0 | 54.52x | 90.0 +/- 0.0 | -1.1% | 5094.0 +/- 110.0 | +2.7% |
| `Kraken/imaging-gaussian-blur.js` | time | 74.0 +/- 0.0 | 13903.0 +/- 6.0 | 187.88x | 74.0 +/- 0.0 | +0.0% | 14350.0 +/- 56.0 | +3.2% |
| `Kraken/json-parse-financial.js` | time | 25.0 +/- 0.0 | 26.0 +/- 0.0 | 1.04x | 26.0 +/- 0.0 | +4.0% | 26.0 +/- 0.0 | +0.0% |
| `Kraken/json-stringify-tinderbox.js` | time | 18.0 +/- 0.0 | 18.0 +/- 0.0 | 1.00x | 18.0 +/- 0.0 | +0.0% | 18.0 +/- 0.0 | +0.0% |
| `Kraken/stanford-crypto-aes.js` | time | 625.0 +/- 4.0 | 637.0 +/- 2.0 | 1.02x | 621.0 +/- 5.0 | -0.6% | 650.0 +/- 3.0 | +2.0% |
| `Kraken/stanford-crypto-ccm.js` | time | 169.0 +/- 0.0 | 406.0 +/- 2.0 | 2.40x | 171.0 +/- 1.0 | +1.2% | 425.0 +/- 3.0 | +4.7% |
| `Kraken/stanford-crypto-pbkdf2.js` | time | 217.0 +/- 3.0 | 1188.0 +/- 1.0 | 5.47x | 218.0 +/- 1.0 | +0.5% | 1212.0 +/- 1.0 | +2.0% |
| `Kraken/stanford-crypto-sha256-iterative.js` | time | 45.0 +/- 0.0 | 381.0 +/- 0.0 | 8.47x | 46.0 +/- 0.0 | +2.2% | 391.0 +/- 1.0 | +2.6% |
| `Octane/box2d.js` | score | 65410.0 +/- 1081.0 | 1981.0 +/- 13.0 | 33.02x | 59158.0 +/- 892.0 | +10.6% | 1956.0 +/- 4.0 | +1.3% |
| `Octane/code-load.js` | score | 28566.0 +/- 251.0 | 29299.0 +/- 79.0 | 0.97x | 28151.0 +/- 64.0 | +1.5% | 28473.0 +/- 277.0 | +2.9% |
| `Octane/crypto.js` | score | 2654.0 +/- 15.0 | 596.0 +/- 1.0 | 4.45x | 2648.0 +/- 33.0 | +0.2% | 585.0 +/- 1.0 | +1.9% |
| `Octane/deltablue.js` | score | 39704.0 +/- 582.0 | 307.0 +/- 1.0 | 129.33x | 39320.0 +/- 529.0 | +1.0% | 293.0 +/- 1.0 | +4.8% |
| `Octane/earley-boyer.js` | score | 10971.0 +/- 32.0 | 1250.0 +/- 1.0 | 8.78x | 11022.0 +/- 11.0 | -0.5% | 1225.0 +/- 3.0 | +2.0% |
| `Octane/gbemu.js` | score | 72663.0 +/- 144.0 | 3085.0 +/- 0.0 | 23.55x | 74160.0 +/- 72.0 | -2.0% | 3076.0 +/- 2.0 | +0.3% |
| `Octane/mandreel.js` | score | 33922.0 +/- 258.0 | 418.0 +/- 1.0 | 81.15x | 33463.0 +/- 427.0 | +1.4% | 415.0 +/- 0.0 | +0.7% |
| `Octane/navier-stokes.js` | score | 3262.0 +/- 10.0 | 831.0 +/- 0.0 | 3.93x | 3311.0 +/- 20.0 | -1.5% | 822.0 +/- 1.0 | +1.1% |
| `Octane/pdfjs.js` | score | 29867.0 +/- 88.0 | 3639.0 +/- 13.0 | 8.21x | 30015.0 +/- 29.0 | -0.5% | 3659.0 +/- 30.0 | -0.5% |
| `Octane/raytrace.js` | score | 69041.0 +/- 666.0 | 805.0 +/- 2.0 | 85.77x | 66451.0 +/- 666.0 | +3.9% | 804.0 +/- 1.0 | +0.1% |
| `Octane/regexp.js` | score | 6903.0 +/- 49.0 | 1458.0 +/- 10.0 | 4.73x | 6792.0 +/- 27.0 | +1.6% | 1450.0 +/- 11.0 | +0.6% |
| `Octane/richards.js` | score | 24489.0 +/- 39.0 | 348.0 +/- 2.0 | 70.37x | 24365.0 +/- 64.0 | +0.5% | 342.0 +/- 2.0 | +1.8% |
| `Octane/splay.js` | score | 22090.0 +/- 247.0 | 3137.0 +/- 10.0 | 7.04x | 21220.0 +/- 164.0 | +4.1% | 3040.0 +/- 9.0 | +3.2% |
| `Octane/typescript.js` | score | 76077.0 +/- 2544.0 | 6980.0 +/- 28.0 | 10.90x | 74217.0 +/- 1106.0 | +2.5% | 6955.0 +/- 42.0 | +0.4% |
| `Octane/zlib.js` |  | excluded | excluded | -- | excluded | -- | excluded | -- |
| `SunSpider/3d-cube.js` | time | 9.0 +/- 0.0 | 29.0 +/- 0.0 | 3.22x | 10.0 +/- 1.0 | +11.1% | 30.0 +/- 0.0 | +3.4% |
| `SunSpider/3d-morph.js` | time | 3.0 +/- 0.0 | 22.0 +/- 0.0 | 7.33x | 3.0 +/- 0.0 | +0.0% | 22.0 +/- 0.0 | +0.0% |
| `SunSpider/3d-raytrace.js` | time | 13.0 +/- 0.0 | 38.0 +/- 1.0 | 2.92x | 15.0 +/- 0.0 | +15.4% | 38.0 +/- 0.0 | +0.0% |
| `SunSpider/access-binary-trees.js` | time | 4.0 +/- 0.0 | 25.0 +/- 1.0 | 6.25x | 4.0 +/- 0.0 | +0.0% | 26.0 +/- 0.0 | +4.0% |
| `SunSpider/access-fannkuch.js` | time | 6.0 +/- 0.0 | 79.0 +/- 0.0 | 13.17x | 7.0 +/- 0.0 | +16.7% | 81.0 +/- 0.0 | +2.5% |
| `SunSpider/access-nbody.js` | time | 4.0 +/- 0.0 | 34.0 +/- 0.0 | 8.50x | 4.0 +/- 0.0 | +0.0% | 36.0 +/- 0.0 | +5.9% |
| `SunSpider/access-nsieve.js` | time | 3.0 +/- 0.0 | 26.0 +/- 0.0 | 8.67x | 3.0 +/- 0.0 | +0.0% | 27.0 +/- 0.0 | +3.8% |
| `SunSpider/bitops-3bit-bits-in-byte.js` | time | 2.0 +/- 0.0 | 31.0 +/- 1.0 | 15.50x | 1.0 +/- 0.0 | -50.0% | 31.0 +/- 0.0 | +0.0% |
| `SunSpider/bitops-bits-in-byte.js` | time | 2.0 +/- 0.0 | 50.0 +/- 0.0 | 25.00x | 2.0 +/- 0.0 | +0.0% | 50.0 +/- 0.0 | +0.0% |
| `SunSpider/bitops-bitwise-and.js` | time | 3.0 +/- 0.0 | 41.0 +/- 0.0 | 13.67x | 3.0 +/- 0.0 | +0.0% | 42.0 +/- 0.0 | +2.4% |
| `SunSpider/bitops-nsieve-bits.js` | time | 96.0 +/- 1.0 | 55.0 +/- 0.0 | 0.57x | 101.0 +/- 1.0 | +5.2% | 57.0 +/- 0.0 | +3.6% |
| `SunSpider/controlflow-recursive.js` | time | 6.0 +/- 0.0 | 42.0 +/- 1.0 | 7.00x | 6.0 +/- 0.0 | +0.0% | 44.0 +/- 0.0 | +4.8% |
| `SunSpider/crypto-aes.js` | time | 23.0 +/- 0.0 | 30.0 +/- 0.0 | 1.30x | 24.0 +/- 0.0 | +4.3% | 32.0 +/- 0.0 | +6.7% |
| `SunSpider/crypto-md5.js` | time | 11.0 +/- 0.0 | 26.0 +/- 0.0 | 2.36x | 11.0 +/- 0.0 | +0.0% | 27.0 +/- 0.0 | +3.8% |
| `SunSpider/crypto-sha1.js` | time | 7.0 +/- 0.0 | 26.0 +/- 0.0 | 3.71x | 8.0 +/- 0.0 | +14.3% | 27.0 +/- 0.0 | +3.8% |
| `SunSpider/date-format-tofte.js` | time | 13.0 +/- 0.0 | 30.0 +/- 0.0 | 2.31x | 15.0 +/- 1.0 | +15.4% | 32.0 +/- 1.0 | +6.7% |
| `SunSpider/date-format-xparb.js` | time | 22.0 +/- 1.0 | 22.0 +/- 0.0 | 1.00x | 22.0 +/- 0.0 | +0.0% | 23.0 +/- 0.0 | +4.5% |
| `SunSpider/math-cordic.js` | time | 5.0 +/- 0.0 | 61.0 +/- 0.0 | 12.20x | 5.0 +/- 0.0 | +0.0% | 58.0 +/- 1.0 | -4.9% |
| `SunSpider/math-partial-sums.js` | time | 4.0 +/- 0.0 | 22.0 +/- 0.0 | 5.50x | 4.0 +/- 0.0 | +0.0% | 22.0 +/- 0.0 | +0.0% |
| `SunSpider/math-spectral-norm.js` | time | 2.0 +/- 0.0 | 32.0 +/- 1.0 | 16.00x | 2.0 +/- 0.0 | +0.0% | 33.0 +/- 0.0 | +3.1% |
| `SunSpider/regexp-dna.js` | time | 4.0 +/- 0.0 | 4.0 +/- 0.0 | 1.00x | 4.0 +/- 0.0 | +0.0% | 4.0 +/- 0.0 | +0.0% |
| `SunSpider/string-base64.js` | time | 5.0 +/- 0.0 | 18.0 +/- 0.0 | 3.60x | 5.0 +/- 0.0 | +0.0% | 19.0 +/- 0.0 | +5.6% |
| `SunSpider/string-fasta.js` | time | 6.0 +/- 0.0 | 35.0 +/- 0.0 | 5.83x | 7.0 +/- 0.0 | +16.7% | 37.0 +/- 0.0 | +5.7% |
| `SunSpider/string-tagcloud.js` | time | 11.0 +/- 0.0 | 21.0 +/- 0.0 | 1.91x | 11.0 +/- 0.0 | +0.0% | 22.0 +/- 0.0 | +4.8% |
| `SunSpider/string-unpack-code.js` | time | 15.0 +/- 0.0 | 27.0 +/- 0.0 | 1.80x | 16.0 +/- 0.0 | +6.7% | 29.0 +/- 0.0 | +7.4% |
| `SunSpider/string-validate-input.js` | time | 7.0 +/- 0.0 | 18.0 +/- 0.0 | 2.57x | 7.0 +/- 0.0 | +0.0% | 18.0 +/- 0.0 | +0.0% |
<!-- BENCH-TABLE-END -->

## Historical baseline reference (2026-11-11)

The original 2026-11-11 receipt files remain preserved for historical
provenance and pre-Stage-2/3 reference:

| Field | Value |
|---|---|
| Date | 2026-11-11 |
| Base commit | `61b6af3f6b32642033757667aaa8e5c2d786aa56` plus uncommitted ARM64 JIT write-barrier changes |
| Build command | `mise exec -- zig build -Dprofile=jit` (`ENABLE_ASMJS` off) |
| Binary SHA-256 | `6692be5ab852e3782db55d19e3b537f8fb5c913ac2ffce70ce2f869a5b6549cb` |
| Raw files | `tools/bench/results/baseline-jit-darwin-arm64.json`, `tools/bench/results/baseline-interp-darwin-arm64.json` |
| Key fix at record time | ARM64 full-JIT software write barrier implemented in `lib/Backend/arm64/LowerMD.cpp` (`LowererMD::ChangeToWriteBarrierAssign`), pinned by `test/es7/jit-write-barrier.js` (moved JIT tier from 4 concurrent-GC crashes to `64/64` pass). |

## How to use this receipt

- Compare only same-host, same-tier, A/B runs. Interleave A/B/A and
  compare per-test medians; treat a consistent >10-15% median shift
  as an investigation trigger, not an automatic fail.
- Keep build flags identical (`-Dwasm=true` vs plain JS changes `"use asm"`
  execution paths substantially on JetStream C/C++ kernels).
- SunSpider/Kraken tests run 1-10 ms; noise dominates there. Prefer
  Octane/JetStream medians for JIT decisions.
- Like-for-like tiers only: JIT vs another JIT engine; `-nonative`
  vs interpreter-only engines. Cross-engine receipts (Node/V8,
  QuickJS-ng) are separate future receipts, not part of this one.
- Record separate receipts per platform. This one says nothing about
  Linux x64, where `Octane/zlib.js` becomes applicable.
- No CI gate reads this file yet. The measured-perf-gate card in
  `doc/open-cards.md` tracks adding one (fast subset, same-host A/B
  threshold, no absolute score gates).

---

## Post-P1 Addendum (2026-11-19, `modernize/phase4`)

On `modernize/phase4` (Apple M1 Max, `darwin/arm64`, built with `mise exec -- zig build -Dprofile=jit -Dwasm=true`; `out/jit/bin/ch` SHA-256 `e43f7e87126b79dadbb97a11f2409e8af699b6e70a04d6bbd768092ef3a6c798`, `out/jit/lib/libChakraCore.dylib` SHA-256 `14eda23edff2f611a9ebe5f738c8d13493934942e750d4282c05a8b0234fb8d3`), Track `P1` (`a6d3a6fe8`, `628cf5391`, `7723bd804` -- full write-up & invariants in [p1-jit-bailouts.md](p1-jit-bailouts.md)) eliminated the pathological JIT bailout, loop-body abort retry storm, and ARM64 asm.js BGJIT tier-up stalls.

`tools/bench/results/head-jit-darwin-arm64.json` was refreshed with the 3-iteration post-P1 `--tier jit` run (`64/64` applicable PASS, `Octane/zlib.js` excluded on arm64, `0` unexpected failures). Historical `4b2077687` JIT (`2026-11-16`, `7afbc2509d65`, predating `Q1.4`/`I1`/`I2`/`Q2`) and `head-interp-darwin-arm64.json` baselines remain listed above/in-table for A/B reference.

### Focused 5-Iteration P1 Improvements (`--timeout 120`, same host)

| Test | Unit | Pre-P1 JIT (`4b2077687`) | Post-P1 JIT | Post-P1 Interp (`-nonative`) | Speedup vs Pre-P1 | Speedup vs Interp | Post-P1 `-bgjit-` (5x med) |
|---|---|---|---|---|---|---|---|
| `Kraken/audio-fft.js` | ms | `5539.0` | **`84.0`** (`mad 0.0`) | `2287.0` (`mad 4.0`) | **`65.94x`** | **`27.23x`** | **`89.0`** ms |
| `Kraken/audio-beat-detection.js` | ms | `5594.0` | **`110.0`** (`mad 0.0`) | `2391.0` (`mad 14.0`) | **`50.85x`** | **`21.74x`** | -- |
| `Kraken/audio-oscillator.js` | ms | `1672.0` | **`202.0`** (`mad 4.0`) | `1836.0` (`mad 1.0`) | **`8.28x`** | **`9.09x`** | -- |
| `Kraken/stanford-crypto-aes.js` | ms | `625.0` | **`89.0`** (`mad 1.0`) | `627.0` (`mad 3.0`) | **`7.02x`** | **`7.04x`** | -- |
| `SunSpider/bitops-nsieve-bits.js` | ms | `96.0` | **`6.0`** (`mad 0.0`) | `55.0` (`mad 0.0`) | **`16.00x`** | **`9.17x`** | **`6.0`** ms |
| `JetStream/container.cpp.js` | score | `120.2` | **`364.8`** (`mad 2.1`) | `12.2` (`mad 0.0`) | **`3.03x`** | **`29.92x`** | -- |

- `-trace:BailOut,ReJIT` on `Kraken/stanford-crypto-aes.js`: `59` `BailOut` lines + `17` `Rejit` lines, finishing in `90 ms`.

### 4-Suite Geomean & Total Comparison (`tools/bench/results/head-jit-darwin-arm64.json`, 3 iterations)

| Suite | Metric (`n`) | Pre-P1 JIT (`4b2077687`) | Post-P1 JIT | Interp (`head-interp`) | Ratio vs Pre-P1 | Ratio vs Interp |
|---|---|---|---|---|---|---|
| `SunSpider` | sum ms (`26`) | `286.0` ms (`gm 6.59`) | **`172.0` ms** (`gm 5.30`) | `844.0` ms (`gm 28.85`) | **`1.66x`** sum (`1.244x` gm) | **`4.91x`** sum (`5.45x` gm) |
| `Kraken` | sum ms (`14`) | `15054.0` ms (`gm 207.79`) | **`2081.0` ms** (`gm 86.58`) | `36509.0` ms (`gm 980.93`) | **`7.23x`** sum (`2.400x` gm) | **`17.54x`** sum (`11.33x` gm) |
| `Octane` | geomean score (`14`) | `22786.6` | **`32382.1`** | `1550.2` | **`1.421x`** | **`20.89x`** |
| `JetStream` | geomean score (`10`/`9`) | `311.5` | **`346.5`** | `8.5` | **`1.112x`** | **`40.12x`** |
| **Overall** | geomean (`64` / `63` tests) | -- | -- | -- | **`1.453x`** (`64` tests) | **`11.49x`** (`63` tests) |

Notable deltas & notes:
- `Octane/navier-stokes.js` jumped `3262.0 -> 31990.0` (**`9.81x`**); `Octane/crypto.js` jumped `2654.0 -> 24830.0` (**`9.36x`**); `SunSpider/crypto-aes.js` dropped `23.0 -> 8.0 ms` (**`2.88x`**); `Octane/splay.js` `22090 -> 31295` (**`1.42x`**); `Octane/pdfjs.js` `29867 -> 35402` (**`1.19x`**); `Kraken/stanford-crypto-pbkdf2.js` `217 -> 147 ms` (**`1.48x`**).
- Two benchmarks scored lower than the Nov 16 `4b2077687` baseline: `Octane/regexp.js` (`6903 -> 5513` 3-iter / `5551` 5-iter, `3.78x` vs interp, `28` bailout/rejit trace lines total) and `Kraken/stanford-crypto-ccm.js` (`169 -> 205` ms 3-iter / `210` ms 5-iter, `1.98x` vs interp; runs in `61.0 ms` with `-ESBigInt-` when `LossyIntTypeSpec` is not inhibited after `AggressiveIntTypeSpec`+`FloatTypeSpec` disablement under `ESBigInt`).

---

## Wave 4 Addendum (2026-11-24, `modernize/phase4`)

Wave 4 recovered JIT integer/lossy specialization with default `ESBigInt` enabled, implemented ARM64 `AtomTag_Int` fast power-of-two division (`J-8`, `PE-1`, `PE-2`, `J-5`), and added the same-host interleaved A/B smoke gate (`T-2` / `PE-7`):

- **`J-8` (postfix `x++` / `x--` expression specialization):** Modeled `Conv_Numeric`, `Incr_Num_A`, and `Decr_Num_A` alongside `Conv_Num` / `Incr_A` / `Decr_A` in `lib/Backend/GlobOpt.cpp`, `GlobOptIntBounds.cpp`, and `IR.cpp`, preserving BigInt semantics while eliminating the `Conv_Numeric` penalty on expression-used `j++` / `j--`.
- **`PE-1` / `PE-2` (lossy int32 specialization & BigInt-safe slow path):** Decoupled `CanLossyIntTypeSpecForInstr` / `DoLossyIntTypeSpec` from `AggressiveIntTypeSpec` / `FloatTypeSpec`, updated bitwise/`Not_A`/`Incr*`/`Decr*` `ValueNumberDst` guards to `IsPrimitive()` / `IsLikelyNumber()`, removed forced `AggressiveInt` -> `Float` downgrades in `ToTypeSpecUse` and hoisting, and added a `TypeIds_BigInt` guard before `HelperConv_ToInt32_NoObjects` in `lib/Backend/Lower.cpp` (`LowerUnaryHelperMemWithBoolReference`).
- **`J-5` (ARM64 `AtomTag_Int` `GenerateFastDivByPow2`):** Replaced the commented 31-bit stub in `lib/Backend/arm64/LowerMD.cpp` with a 64-bit `AtomTag_Int` fast path (`ASR` for exact multiples, `HelperOp_FinishOddDivByPow2*` for exact `.5` halves, early return for `src2Value == 1`).
- **`T-2` / `PE-7` (`tools/bench/check_perf_smoke.py`):** Added an interleaved `A, B, A, B, ...` same-host performance smoke gate (wired as opt-in `zig build -Dprofile=jit check-perf-smoke`), unit/negative-control suite `tools/bench/test_bench_tools.py`, and `--flags` support in `tools/bench/run.py`.

### Provenance (`modernize/phase4`, same host Apple M1 Max, `darwin/arm64`)

| Build | Command | Binary SHA-256 (`ch`) | Shared library SHA-256 (`libChakraCore.dylib`) |
|---|---|---|---|
| `jit` (`-Dwasm=true`) | `mise exec -- zig build -Dprofile=jit -Dwasm=true` | `2cfa9f1e305494474323ac00ae3616a510e03ce9125660c490642ff45c2f1e5b` | `5b6c3ad7567108a76176da6372fab9a5177aae17998da97f727dcc872f1914da` |
| `release-jit` (plain JS) | `mise exec -- zig build -Dprofile=release-jit` | `764ad24599882bb57e0687927dcdc730fc45acd6a0e52b4af3ecb5b26b93e6d2` | `960d7c23da4badc2056de2e3611a4896a074f34318c7d3d2f8732083741311ee` |

### Interleaved `check_perf_smoke.py` A/B Receipts (3 iterations)

1. **`out/jit/bin/ch` `-ESBigInt-` (A) vs default `ESBigInt` (B)** via `mise exec -- zig build -Dprofile=jit -Dwasm=true check-perf-smoke`:

| Test | Kind | A (`ESBigInt-off`) | B (`default`) | B/A | Delta | Status |
|---|---|---|---|---:|---:|---|
| `Kraken/stanford-crypto-ccm.js` | time (ms) | `63.0 +/- 0.0` | `62.0 +/- 1.0` | `0.984x` | `-1.6%` | PASS |
| `Kraken/stanford-crypto-aes.js` | time (ms) | `81.0 +/- 0.0` | `82.0 +/- 1.0` | `1.012x` | `+1.2%` | PASS |
| `Kraken/audio-fft.js` | time (ms) | `83.0 +/- 1.0` | `84.0 +/- 2.0` | `1.012x` | `+1.2%` | PASS |
| `Octane/crypto.js` | score | `34470.0 +/- 108.0` | `34553.0 +/- 163.0` | `1.002x` | `+0.2%` | PASS |
| `Octane/richards.js` | score | `24740.0 +/- 21.0` | `24570.0 +/- 21.0` | `0.993x` | `-0.7%` | PASS |
| `Octane/splay.js` | score | `31392.0 +/- 294.0` | `31252.0 +/- 116.0` | `0.996x` | `-0.4%` | PASS |
| `Micro/postfix-incr.js` | time (ms) | `52.0 +/- 0.0` | `52.0 +/- 0.0` | `1.000x` | `+0.0%` | PASS |
| `Micro/prefix-incr.js` | time (ms) | `51.0 +/- 0.0` | `52.0 +/- 0.0` | `1.020x` | `+2.0%` | PASS |

- Micro `j++/++j` ratio: `A=1.02x`, `B=1.00x` (`8/8 PASS`, 15% tolerance).

2. **`out/jit/bin/ch` (A) vs `out/release-jit/bin/ch` (B)** via `python3 tools/bench/check_perf_smoke.py --binary-a out/jit/bin/ch --binary-b out/release-jit/bin/ch --label-a jit --label-b release-jit --iterations 3`:

| Test | Kind | A (`jit`) | B (`release-jit`) | B/A | Delta | Status |
|---|---|---|---|---:|---:|---|
| `Kraken/stanford-crypto-ccm.js` | time (ms) | `64.0 +/- 1.0` | `62.0 +/- 0.0` | `0.969x` | `-3.1%` | PASS |
| `Kraken/stanford-crypto-aes.js` | time (ms) | `84.0 +/- 1.0` | `82.0 +/- 0.0` | `0.976x` | `-2.4%` | PASS |
| `Kraken/audio-fft.js` | time (ms) | `85.0 +/- 0.0` | `85.0 +/- 0.0` | `1.000x` | `+0.0%` | PASS |
| `Octane/crypto.js` | score | `34340.0 +/- 245.0` | `34211.0 +/- 126.0` | `0.996x` | `-0.4%` | PASS |
| `Octane/richards.js` | score | `24017.0 +/- 149.0` | `24514.0 +/- 116.0` | `1.021x` | `+2.1%` | PASS |
| `Octane/splay.js` | score | `31525.0 +/- 261.0` | `35074.0 +/- 130.0` | `1.113x` | `+11.3%` | PASS |
| `Micro/postfix-incr.js` | time (ms) | `51.0 +/- 0.0` | `51.0 +/- 0.0` | `1.000x` | `+0.0%` | PASS |
| `Micro/prefix-incr.js` | time (ms) | `54.0 +/- 0.0` | `53.0 +/- 0.0` | `0.981x` | `-1.9%` | PASS |

- Micro `j++/++j` ratio: `A=0.94x`, `B=0.96x` (`8/8 PASS`, 15% tolerance).

### 4-Suite `run.py --tier jit --iterations 3` Receipt (`jit` vs `release-jit` vs Post-P1)

Both `out/jit/bin/ch` (`140.9 s` total run wall) and `out/release-jit/bin/ch` (`160.0 s` total run wall) passed `64/64` applicable tests (`Octane/zlib.js` excluded on `darwin/arm64`, `0` failures). Note `out/release-jit/bin/ch` was built without `-Dwasm=true` (`ENABLE_ASMJS` off), so `JetStream` C/C++ kernels run as plain JS there.

| Suite | Metric (`n`) | Post-P1 `jit` (`head-jit`) | Wave 4 `jit` (`-Dwasm=true`) | Wave 4 `release-jit` (plain JS) | Wave 4 `jit` vs Post-P1 | `release-jit` vs Wave 4 `jit` |
|---|---|---:|---:|---:|---:|---:|
| `SunSpider` | sum ms (`26`) | `172.0` (`gm 5.30`) | **`170.0`** (`gm 5.25`) | **`166.0`** (`gm 5.20`) | `1.008x` gm | `1.011x` gm |
| `Kraken` | sum ms (`14`) | `2081.0` (`gm 86.58`) | **`1948.0`** (`gm 80.46`) | **`1955.0`** (`gm 77.98`) | **`1.076x`** gm | `1.032x` gm |
| `Octane` | score geomean (`14`) | `32382.13` | **`32590.89`** | **`33618.25`** | `1.006x` | **`1.032x`** |
| `JetStream` | score geomean (`10`) | `346.45` | `341.22` | `243.42` (plain JS) | `0.985x` | `0.713x` (`ENABLE_ASMJS` off) |
| **Non-JetStream** | geomean (`54` tests) | -- | -- | -- | **`1.0250x`** | **`1.0215x`** |
| **Overall** | geomean (`64` tests) | -- | -- | -- | **`1.0186x`** | `0.9657x` |

Key Wave 4 shifts:
- `Kraken/stanford-crypto-ccm.js`: `205.0 ms` -> **`65.0 ms`** (`3.15x` faster in `jit`, `61.0 ms` in `release-jit`; `196` `BailOut` / `29` `Rejit` lines under `-trace:BailOut,ReJIT` vs `64 ms` execution).
- `Octane/crypto.js`: `24830.0` -> **`34442.0`** (`1.39x` faster in `jit`, `34198.0` in `release-jit`).
- `Kraken/imaging-desaturate.js`: `90.0 ms` -> **`77.0 ms`** (`1.17x` in `jit`, `76.0 ms` in `release-jit`); `SunSpider/crypto-sha1.js`: `5.0 -> 3.0 ms` (`1.67x`); `SunSpider/3d-raytrace.js`: `13.0 -> 9.0 ms` (`1.44x`).
- `Octane/splay.js`: `30419.0` (`jit`) -> **`34670.0`** (`release-jit`, **`+14.0%`** from Release `-DNDEBUG`).

### Using `tools/bench/check_perf_smoke.py`

```sh
# 1. Single-binary A/B (-ESBigInt- vs default on flaglist JIT builds; smoke invariants on Release):
mise exec -- zig build -Dprofile=jit -Dwasm=true check-perf-smoke
# or directly:
python3 tools/bench/check_perf_smoke.py --binary out/jit/bin/ch --iterations 3

# 2. Same-host interleaved A/B across two binaries (e.g. jit vs release-jit):
python3 tools/bench/check_perf_smoke.py \
    --binary-a out/jit/bin/ch --binary-b out/release-jit/bin/ch \
    --label-a jit --label-b release-jit --iterations 3

# 3. Advisory comparison against a recorded run.py JSON receipt (pass --strict-baseline to fail on regression):
python3 tools/bench/check_perf_smoke.py --binary out/jit/bin/ch \
    --baseline-json tools/bench/results/head-jit-darwin-arm64.json

# 4. Self-test and unit suite (also wired into `zig build check-selftest`):
python3 tools/bench/check_perf_smoke.py --self-test
python3 tools/bench/test_bench_tools.py
```

