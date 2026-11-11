#!/usr/bin/env python3
# -------------------------------------------------------------------------------------------------------
# Copyright (c) ChakraCore Project Contributors. All rights reserved.
# Licensed under the MIT license. See LICENSE.txt file in the project root for full license information.
# -------------------------------------------------------------------------------------------------------
"""Benchmark smoke runner for the in-tree legacy suites.

Correctness first: every test records rc + parsed marker before any
timing is trusted. A crash or timeout is a result, not a harness
failure. Applicability is explicit per platform (see EXCLUDED /
KNOWN_ISSUES); scores across tiers or machines are never mixed.

Unlike test/benchmarks/perftest.pl this runner does not add
-dynamicProfileCache and does not need a baseline file. Use perftest.pl
for legacy A/B comparisons; use this for smoke matrices and baseline
receipts (doc/perf-baseline.md).

Usage:
  tools/bench/run.py --binary out/jit/bin/ch [--tier jit|simplejit|interp]
      [--suite SunSpider,Kraken,Octane,JetStream] [--iterations 3]
      [--timeout 60] [--test splay] [-o results.json]
"""

import argparse
import hashlib
import json
import platform
import re
import statistics
import subprocess
import sys
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
BENCH_DIR = ROOT / "test" / "benchmarks"

SUITES = {
    "SunSpider": ("SunSpider", "time"),
    "Kraken": ("Kraken", "time"),
    "Octane": ("Octane", "score"),
    "JetStream": ("jetstream", "score"),
}

TIER_FLAGS = {
    "jit": [],
    "simplejit": ["-off:fullJit"],
    "interp": ["-nonative"],
}

# Tests that cannot run meaningfully on a platform. Keys are
# (suite, filename, sys.platform-prefix, machine) tuples; machine ""
# matches any. Excluded tests are reported, never silently dropped.
EXCLUDED = [
    ("Octane", "zlib.js", "darwin", "arm64",
     "asm.js/WASM backend is x86/x64-only (ENABLE_WASM); falls back to "
     "plain JS and times out. Run on Linux x64."),
]

# Tests with a tracked defect. They still run; a crash is reported as
# KNOWN-CRASH instead of failing the harness exit code.
KNOWN_ISSUES = [
    # Octane/splay.js was tracked here (full-JIT + concurrent-GC crash:
    # missing ARM64 JIT software write barrier). Fixed; a crash now
    # fails the harness as a regression.
    ("JetStream", "cdjs.js", "interp",
     "intermittent SIGSEGV under -nonative with concurrent GC "
     "(pre-existing; clean with -off:ConcurrentMark and under full "
     "JIT). Carded in doc/open-cards.md."),
    ("JetStream", "cdjs.js", "simplejit",
     "same interpreter/concurrent-GC crash as the interp tier; "
     "carded in doc/open-cards.md."),
]

MARKER_RE = re.compile(r"###\s*(TIME|SCORE):\s*([0-9]+(?:\.[0-9]+)?)")


def suite_files(suite):
    subdir, _kind = SUITES[suite]
    return sorted(p.name for p in (BENCH_DIR / subdir).glob("*.js"))


def exclusion_reason(suite, fname):
    plat, mach = sys.platform, platform.machine()
    for s, f, p, m, why in EXCLUDED:
        if s == suite and f == fname and plat.startswith(p) and (m == "" or m == mach):
            return why
    return None


def known_issue(suite, fname, tier):
    for s, f, t, why in KNOWN_ISSUES:
        if s == suite and f == fname and t == tier:
            return why
    return None


def run_once(binary, flags, path, timeout):
    start = time.monotonic()
    try:
        proc = subprocess.run(
            [str(binary), *flags, path.name],
            cwd=path.parent, capture_output=True, text=True, timeout=timeout)
        wall = time.monotonic() - start
        out = proc.stdout + proc.stderr
        m = MARKER_RE.search(out)
        return {
            "rc": proc.returncode,
            "wallSec": round(wall, 3),
            "marker": m.group(1) if m else None,
            "value": float(m.group(2)) if m else None,
        }
    except subprocess.TimeoutExpired:
        return {"rc": None, "wallSec": round(time.monotonic() - start, 3),
                "marker": None, "value": None, "timeout": True}


def median_mad(values):
    med = statistics.median(values)
    mad = statistics.median([abs(v - med) for v in values]) if len(values) > 1 else 0.0
    return med, mad


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--binary", required=True, help="path to ch binary")
    ap.add_argument("--tier", default="jit", choices=sorted(TIER_FLAGS))
    ap.add_argument("--suite", default=",".join(SUITES),
                    help="comma-separated suites (default: all)")
    ap.add_argument("--iterations", type=int, default=3)
    ap.add_argument("--timeout", type=float, default=60.0,
                    help="per-run timeout in seconds")
    ap.add_argument("--test", default="",
                    help="substring filter on suite/file (e.g. splay)")
    ap.add_argument("-o", "--output", default="",
                    help="write JSON results to this path")
    args = ap.parse_args()

    binary = Path(args.binary).resolve()
    if not binary.exists():
        print(f"binary not found: {binary}")
        return 2
    digest = hashlib.sha256(binary.read_bytes()).hexdigest()
    flags = TIER_FLAGS[args.tier]
    suites = [s.strip() for s in args.suite.split(",") if s.strip()]
    unknown = [s for s in suites if s not in SUITES]
    if unknown:
        print(f"unknown suite(s): {unknown}; known: {sorted(SUITES)}")
        return 2

    print(f"binary: {binary} sha256:{digest}")
    print(f"host: {sys.platform} {platform.machine()} tier: {args.tier} "
          f"flags: {' '.join(flags) or '(none)'} iterations: {args.iterations} "
          f"timeout: {args.timeout:g}s")
    print(f"{'test':44} {'status':12} {'median':>12} {'mad':>10}")
    print("-" * 82)

    results = []
    unexpected = 0
    for suite in suites:
        subdir, kind = SUITES[suite]
        for fname in suite_files(suite):
            label = f"{suite}/{fname}"
            if args.test and args.test not in label:
                continue
            path = BENCH_DIR / subdir / fname
            why = exclusion_reason(suite, fname)
            if why:
                print(f"{label:44} {'EXCLUDED':12} {'':>12}  {why[:60]}")
                results.append({"test": label, "status": "excluded", "reason": why})
                continue
            issue = known_issue(suite, fname, args.tier)
            runs = [run_once(binary, flags, path, args.timeout)
                    for _ in range(args.iterations)]
            ok = [r for r in runs if r["rc"] == 0 and r["value"] is not None]
            if ok:
                med, mad = median_mad([r["value"] for r in ok])
                status = "PASS" if len(ok) == len(runs) else "FLAKY"
                if status == "FLAKY" and not issue:
                    unexpected += 1
                print(f"{label:44} {status:12} {med:12.1f} {mad:10.1f}  ({kind})")
                results.append({"test": label, "status": status.lower(),
                                "kind": kind, "median": med, "mad": mad, "runs": runs,
                                **({"knownIssue": issue} if issue else {})})
            else:
                crashed = any(r["rc"] not in (0, None) for r in runs)
                status = "KNOWN-CRASH" if (crashed and issue) else ("TIMEOUT" if runs[0].get("timeout") else "CRASH")
                if not issue:
                    unexpected += 1
                print(f"{label:44} {status:12} {'':>12}  rc={runs[0]['rc']} "
                      f"wall={runs[0]['wallSec']}s" + (f"  [{issue[:40]}]" if issue else ""))
                results.append({"test": label, "status": status.lower(),
                                "kind": kind, "runs": runs,
                                **({"knownIssue": issue} if issue else {})})

    passed = sum(1 for r in results if r["status"] in ("pass", "flaky"))
    total = sum(1 for r in results if r["status"] != "excluded")
    print("-" * 82)
    print(f"{passed}/{total} applicable tests produced a score; "
          f"{unexpected} unexpected failure(s)")
    if args.output:
        payload = {
            "binary": str(binary), "binarySha256": digest,
            "platform": sys.platform, "machine": platform.machine(),
            "tier": args.tier, "flags": flags,
            "iterations": args.iterations, "timeoutSec": args.timeout,
            "results": results,
        }
        Path(args.output).write_text(json.dumps(payload, indent=1) + "\n")
        print(f"wrote {args.output}")
    return 1 if unexpected else 0


if __name__ == "__main__":
    raise SystemExit(main())
