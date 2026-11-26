#!/usr/bin/env python3
#-------------------------------------------------------------------------------------------------------
# Copyright (c) ChakraCore Project Contributors. All rights reserved.
# Licensed under the MIT license. See LICENSE.txt file in the project root for full license information.
#-------------------------------------------------------------------------------------------------------
"""Unit and negative-control tests for tools/bench/check_perf_smoke.py and run.py.

Run with: python3 tools/bench/test_bench_tools.py
Uses hermetic fake stub executables inside TemporaryDirectory; requires no
compiled ch binary and leaves the repository untouched.
"""

import contextlib
import io
import json
import os
from pathlib import Path
import platform
import subprocess
import sys
import tempfile
import unittest
from unittest import mock

HERE = Path(__file__).resolve().parent
ROOT = HERE.parent.parent
sys.path.insert(0, str(HERE))

import check_perf_smoke
import run as bench_run


def write_stub_ch(path, body):
    """Create an executable Python stub ch script."""
    path.write_text(f"#!{sys.executable}\n{body}\n", encoding="utf-8")
    os.chmod(path, 0o755)
    return path


def run_cli(func, argv, **kwargs):
    """Invoke a main(argv) function while capturing stdout and stderr."""
    out_buf, err_buf = io.StringIO(), io.StringIO()
    with contextlib.redirect_stdout(out_buf), contextlib.redirect_stderr(err_buf):
        rc = func(argv, **kwargs)
    return rc, out_buf.getvalue(), err_buf.getvalue()


class RunPyTests(unittest.TestCase):
    def test_normalize_flag_args_keeps_leading_dash_and_rejects_missing(self):
        norm = bench_run.normalize_flag_args(
            ["--binary", "ch", "--flags", "-ESBigInt-", "--tier", "jit"],
            {"--flags"},
        )
        self.assertEqual(norm, ["--binary", "ch", "--flags=-ESBigInt-", "--tier", "jit"])
        # Do not swallow a following --option as the value of --flags.
        norm2 = bench_run.normalize_flag_args(
            ["--binary", "ch", "--flags", "--tier", "jit"],
            {"--flags"},
        )
        self.assertEqual(norm2, ["--binary", "ch", "--flags", "--tier", "jit"])

    def test_run_py_flags_passed_and_schema_preserved(self):
        with tempfile.TemporaryDirectory(prefix="run-py-test-") as tmp:
            root = Path(tmp)
            trace = root / "args.log"
            stub = write_stub_ch(root / "stub_ch.py", f"""
import sys
with open({str(trace)!r}, "a", encoding="utf-8") as f:
    f.write("|".join(sys.argv[1:]) + "\\n")
print("### TIME: 42 ms")
""")
            out_json = root / "res.json"
            rc, out, _ = run_cli(bench_run.main, [
                "--binary", str(stub),
                "--tier", "jit",
                "--flags", "-ESBigInt-",
                "--flags", "-mic:1 -bgjit-",
                "--suite", "Kraken",
                "--test", "stanford-crypto-ccm.js",
                "--iterations", "2",
                "-o", str(out_json),
            ])
            self.assertEqual(rc, 0, out)
            payload = json.loads(out_json.read_text(encoding="utf-8"))
            self.assertEqual(payload["tier"], "jit")
            self.assertEqual(payload["flags"], ["-ESBigInt-", "-mic:1", "-bgjit-"])
            self.assertEqual(len(payload["results"]), 1)
            row = payload["results"][0]
            self.assertEqual(row["test"], "Kraken/stanford-crypto-ccm.js")
            self.assertEqual(row["status"], "pass")
            self.assertEqual(row["median"], 42.0)
            self.assertEqual(row["mad"], 0.0)
            lines = trace.read_text(encoding="utf-8").splitlines()
            self.assertEqual(len(lines), 2)
            self.assertEqual(lines[0], "-ESBigInt-|-mic:1|-bgjit-|stanford-crypto-ccm.js")

    def test_run_py_default_no_flags_backward_compatible(self):
        with tempfile.TemporaryDirectory(prefix="run-py-compat-") as tmp:
            root = Path(tmp)
            stub = write_stub_ch(root / "stub_ch.py", """
print("### SCORE: 12345")
""")
            out_json = root / "res.json"
            rc, _, _ = run_cli(bench_run.main, [
                "--binary", str(stub),
                "--suite", "Octane",
                "--test", "Octane/crypto.js",
                "--iterations", "1",
                "-o", str(out_json),
            ])
            self.assertEqual(rc, 0)
            payload = json.loads(out_json.read_text(encoding="utf-8"))
            self.assertEqual(payload["flags"], [])
            self.assertEqual(payload["results"][0]["median"], 12345.0)


class SelectionAndMathTests(unittest.TestCase):
    def test_default_fast_subset_and_micro_probes(self):
        cat = check_perf_smoke.build_catalog()
        runnable, excluded = check_perf_smoke.select_tests(cat)
        labels = [e.label for e in runnable]
        self.assertEqual(labels, [
            "Kraken/stanford-crypto-ccm.js",
            "Kraken/stanford-crypto-aes.js",
            "Kraken/audio-fft.js",
            "Octane/crypto.js",
            "Octane/richards.js",
            "Octane/splay.js",
            "Micro/postfix-incr.js",
            "Micro/prefix-incr.js",
        ])
        self.assertEqual(excluded, [])

        runnable_nomicro, _ = check_perf_smoke.select_tests(cat, micro_probe=False)
        self.assertEqual([e.label for e in runnable_nomicro], list(check_perf_smoke.DEFAULT_FAST_TESTS))

    def test_exact_filename_selector_does_not_overmatch(self):
        cat = check_perf_smoke.build_catalog()
        runnable, _ = check_perf_smoke.select_tests(cat, tests_arg=["crypto.js"])
        self.assertEqual([e.label for e in runnable], ["Octane/crypto.js"])

    def test_platform_exclusion_behavior(self):
        cat = check_perf_smoke.build_catalog()
        with mock.patch.object(sys, "platform", "darwin"), \
                mock.patch.object(platform, "machine", return_value="arm64"):
            runnable, excluded = check_perf_smoke.select_tests(cat, suites_arg=["Octane"])
            self.assertEqual([e.label for e, _ in excluded], ["Octane/zlib.js"])
            self.assertNotIn("Octane/zlib.js", [e.label for e in runnable])
            with self.assertRaises(check_perf_smoke.UsageError):
                check_perf_smoke.select_tests(cat, tests_arg=["Octane/zlib.js"])

        with mock.patch.object(sys, "platform", "linux"), \
                mock.patch.object(platform, "machine", return_value="x86_64"):
            runnable_lx, excluded_lx = check_perf_smoke.select_tests(cat, suites_arg=["Octane"])
            self.assertEqual(excluded_lx, [])
            self.assertIn("Octane/zlib.js", [e.label for e in runnable_lx])

    def test_compare_math_time_and_score(self):
        # Time: +15.0% passes, +15.1% fails, speedup passes.
        self.assertFalse(check_perf_smoke.compute_comparison("time", 100.0, 115.0, 15.0)["regressed"])
        self.assertTrue(check_perf_smoke.compute_comparison("time", 100.0, 115.1, 15.0)["regressed"])
        self.assertFalse(check_perf_smoke.compute_comparison("time", 100.0, 75.0, 15.0)["regressed"])
        # Score: -15.0% passes, -15.1% fails, speedup passes.
        self.assertFalse(check_perf_smoke.compute_comparison("score", 1000.0, 850.0, 15.0)["regressed"])
        self.assertTrue(check_perf_smoke.compute_comparison("score", 1000.0, 849.0, 15.0)["regressed"])
        self.assertFalse(check_perf_smoke.compute_comparison("score", 1000.0, 1200.0, 15.0)["regressed"])
        # Zero time baseline: 0 -> 0 passes, 0 -> 50 fails without ZeroDivisionError.
        self.assertFalse(check_perf_smoke.compute_comparison("time", 0.0, 0.0, 15.0)["regressed"])
        self.assertTrue(check_perf_smoke.compute_comparison("time", 0.0, 50.0, 15.0)["regressed"])


class PerfSmokeGateTests(unittest.TestCase):
    def test_interleaved_ab_order_with_warmup_and_full_default_subset(self):
        with tempfile.TemporaryDirectory(prefix="perf-gate-order-") as tmp:
            root = Path(tmp)
            trace = root / "order.log"
            stub_a = write_stub_ch(root / "ch_a.py", f"""
import sys
script = sys.argv[-1]
flags = sys.argv[1:-1]
with open({str(trace)!r}, "a", encoding="utf-8") as f:
    f.write("A:" + script + ":" + " ".join(flags) + "\\n")
if script in ("crypto.js", "richards.js", "splay.js"):
    print("### SCORE: 20000")
else:
    print("### TIME: 100 ms")
""")
            stub_b = write_stub_ch(root / "ch_b.py", f"""
import sys
script = sys.argv[-1]
flags = sys.argv[1:-1]
with open({str(trace)!r}, "a", encoding="utf-8") as f:
    f.write("B:" + script + ":" + " ".join(flags) + "\\n")
if script in ("crypto.js", "richards.js", "splay.js"):
    print("### SCORE: 21000")
else:
    print("### TIME: 90 ms")
""")
            json_out = root / "ab.json"
            rc, out, err = run_cli(check_perf_smoke.main, [
                "--binary-a", str(stub_a),
                "--binary-b", str(stub_b),
                "--flags-a", "-mic:1",
                "--flags-b", "-bgjit-",
                "--iterations", "3",
                "--warmup", "1",
                "--json-out", str(json_out),
            ])
            self.assertEqual(rc, 0, out + err)
            self.assertIn("PASS (8/8 tests within threshold)", out)
            self.assertIn("micro j++/++j ratio:", out)

            lines = trace.read_text(encoding="utf-8").splitlines()
            # 8 default tests * (1 warmup + 3 iters) * 2 configs = 64 invocations.
            self.assertEqual(len(lines), 64)
            # Every benchmark executes strictly A, B, A, B, ...
            for i in range(0, len(lines), 2):
                parts_a = lines[i].split(":", 2)
                parts_b = lines[i + 1].split(":", 2)
                self.assertEqual(parts_a[0], "A", lines[i])
                self.assertEqual(parts_b[0], "B", lines[i + 1])
                self.assertEqual(parts_a[1], parts_b[1])
                self.assertEqual(parts_a[2], "-mic:1")
                self.assertEqual(parts_b[2], "-bgjit-")

            payload = json.loads(json_out.read_text(encoding="utf-8"))
            self.assertEqual(payload["summary"]["verdict"], "PASS")
            self.assertEqual(payload["summary"]["passed"], 8)
            self.assertEqual(payload["summary"]["failed"], 0)
            self.assertEqual(len(payload["a"]["sha256"]), 64)
            self.assertEqual(len(payload["b"]["sha256"]), 64)

    def test_time_and_score_regressions_fail_loudly_naming_test(self):
        with tempfile.TemporaryDirectory(prefix="perf-gate-reg-") as tmp:
            root = Path(tmp)
            stub_a = write_stub_ch(root / "ch_a.py", """
import sys
s = sys.argv[-1]
print("### SCORE: 20000" if s == "crypto.js" else "### TIME: 100 ms")
""")
            stub_b = write_stub_ch(root / "ch_b.py", """
import sys
s = sys.argv[-1]
if s == "stanford-crypto-ccm.js":
    print("### TIME: 120 ms")  # +20% slower
elif s == "crypto.js":
    print("### SCORE: 16000")  # -20% lower
else:
    print("### TIME: 100 ms")
""")
            rc, out, err = run_cli(check_perf_smoke.main, [
                "--binary-a", str(stub_a),
                "--binary-b", str(stub_b),
                "--tests", "Kraken/stanford-crypto-ccm.js,Kraken/audio-fft.js,Octane/crypto.js",
                "--iterations", "3",
                "--max-regression-pct", "15.0",
            ])
            self.assertEqual(rc, 1)
            self.assertIn("Kraken/stanford-crypto-ccm.js", err)
            self.assertIn("Octane/crypto.js", err)
            self.assertNotIn("Kraken/audio-fft.js", err)
            self.assertIn("PASS", out)

    def test_within_threshold_noise_and_per_test_tolerance(self):
        with tempfile.TemporaryDirectory(prefix="perf-gate-tol-") as tmp:
            root = Path(tmp)
            stub_a = write_stub_ch(root / "ch_a.py", """
import sys
s = sys.argv[-1]
print("### SCORE: 10000" if s == "splay.js" else "### TIME: 100 ms")
""")
            stub_b = write_stub_ch(root / "ch_b.py", """
import sys
s = sys.argv[-1]
if s == "splay.js":
    print("### SCORE: 8100")  # -19% score shift
else:
    print("### TIME: 112 ms")   # +12% noise within 15%
""")
            # Default 15% rejects splay.js (-19%).
            rc_fail, _, err_fail = run_cli(check_perf_smoke.main, [
                "--binary-a", str(stub_a),
                "--binary-b", str(stub_b),
                "--tests", "Kraken/audio-fft.js,Octane/splay.js",
                "--iterations", "2",
            ])
            self.assertEqual(rc_fail, 1)
            self.assertIn("Octane/splay.js", err_fail)

            # Per-test tolerance allows splay.js up to 25%.
            rc_pass, out_pass, _ = run_cli(check_perf_smoke.main, [
                "--binary-a", str(stub_a),
                "--binary-b", str(stub_b),
                "--tests", "Kraken/audio-fft.js,Octane/splay.js",
                "--iterations", "2",
                "--tolerance", "Octane/splay.js=25.0",
            ])
            self.assertEqual(rc_pass, 0, out_pass)

    def test_hard_failures_nonzero_rc_flaky_missing_wrong_duplicate_timeout_missing_bin(self):
        with tempfile.TemporaryDirectory(prefix="perf-gate-hardfail-") as tmp:
            root = Path(tmp)
            stub_ok = write_stub_ch(root / "ch_ok.py", """
import sys
s = sys.argv[-1]
print("### SCORE: 10000" if s == "crypto.js" else "### TIME: 100 ms")
""")
            # 1. Nonzero exit code even with valid marker printed.
            stub_rc = write_stub_ch(root / "ch_rc.py", """
import sys
print("### TIME: 100 ms")
raise SystemExit(7)
""")
            rc, _, err = run_cli(check_perf_smoke.main, [
                "--binary-a", str(stub_ok), "--binary-b", str(stub_rc),
                "--tests", "Kraken/audio-fft.js", "--iterations", "2",
            ])
            self.assertEqual(rc, 1)
            self.assertIn("rc=7", err)

            # 2. Flaky 1-of-3 failure must FAIL hard, never silently skip failed run.
            count_file = root / "count.txt"
            stub_flaky = write_stub_ch(root / "ch_flaky.py", f"""
from pathlib import Path
import sys
p = Path({str(count_file)!r})
n = int(p.read_text()) + 1 if p.exists() else 1
p.write_text(str(n))
if n == 2:
    raise SystemExit(11)
print("### TIME: 100 ms")
""")
            rc, _, err = run_cli(check_perf_smoke.main, [
                "--binary-a", str(stub_ok), "--binary-b", str(stub_flaky),
                "--tests", "Kraken/audio-fft.js", "--iterations", "3", "--warmup", "0",
            ])
            self.assertEqual(rc, 1)
            self.assertIn("B run #2: rc=11", err)

            # 3. Missing marker fails hard.
            stub_nomark = write_stub_ch(root / "ch_nomark.py", "print('ok')")
            rc, _, err = run_cli(check_perf_smoke.main, [
                "--binary-a", str(stub_ok), "--binary-b", str(stub_nomark),
                "--tests", "Kraken/audio-fft.js", "--iterations", "2",
            ])
            self.assertEqual(rc, 1)
            self.assertIn("missing ### TIME/SCORE marker", err)

            # 4. Marker kind mismatch fails hard.
            stub_wrong_kind = write_stub_ch(root / "ch_wrong_kind.py", "print('### TIME: 100 ms')")
            rc, _, err = run_cli(check_perf_smoke.main, [
                "--binary-a", str(stub_ok), "--binary-b", str(stub_wrong_kind),
                "--tests", "Octane/crypto.js", "--iterations", "2",
            ])
            self.assertEqual(rc, 1)
            self.assertIn("expected SCORE marker", err)

            # 5. Duplicate marker fails hard.
            stub_dup = write_stub_ch(root / "ch_dup.py", "print('### TIME: 100 ms\\n### TIME: 100 ms')")
            rc, _, err = run_cli(check_perf_smoke.main, [
                "--binary-a", str(stub_ok), "--binary-b", str(stub_dup),
                "--tests", "Kraken/audio-fft.js", "--iterations", "1",
            ])
            self.assertEqual(rc, 1)
            self.assertIn("ambiguous multiple TIME markers", err)

            # 6. Non-positive score fails hard.
            stub_zero_score = write_stub_ch(root / "ch_zero_score.py", "print('### SCORE: 0')")
            rc, _, err = run_cli(check_perf_smoke.main, [
                "--binary-a", str(stub_ok), "--binary-b", str(stub_zero_score),
                "--tests", "Octane/crypto.js", "--iterations", "1",
            ])
            self.assertEqual(rc, 1)
            self.assertIn("invalid SCORE value", err)

            # 7. Timeout with descendant process is killed and fails hard.
            stub_hang = write_stub_ch(root / "ch_hang.py", """
import subprocess, sys, time
subprocess.Popen([sys.executable, "-c", "import time; time.sleep(30)"])
time.sleep(30)
""")
            rc, _, err = run_cli(check_perf_smoke.main, [
                "--binary-a", str(stub_ok), "--binary-b", str(stub_hang),
                "--tests", "Kraken/audio-fft.js", "--iterations", "1", "--timeout", "0.3",
            ])
            self.assertEqual(rc, 1)
            self.assertIn("timeout", err)

            # 8. Missing binary fails hard (rc=2).
            rc, _, err = run_cli(check_perf_smoke.main, [
                "--binary-a", str(stub_ok), "--binary-b", str(root / "missing_ch"),
                "--tests", "Kraken/audio-fft.js",
            ])
            self.assertEqual(rc, 2)
            self.assertIn("not found", err)

            # 9. Non-executable binary fails hard (rc=2).
            nonexec = root / "nonexec_ch"
            nonexec.write_text("#!/bin/sh\nexit 0\n", encoding="utf-8")
            os.chmod(nonexec, 0o644)
            rc, _, err = run_cli(check_perf_smoke.main, [
                "--binary-a", str(stub_ok), "--binary-b", str(nonexec),
                "--tests", "Kraken/audio-fft.js",
            ])
            self.assertEqual(rc, 2)
            self.assertIn("not executable", err)

    def test_single_binary_esbigint_release_and_baseline_modes(self):
        with tempfile.TemporaryDirectory(prefix="perf-gate-modes-") as tmp:
            root = Path(tmp)
            # Flaglist build: default matches -ESBigInt- -> PASS.
            stub_fixed = write_stub_ch(root / "ch_fixed.py", """
import sys
s = sys.argv[-1]
if s == "-?":
    print("Usage: ch [flaglist]")
    raise SystemExit(0)
print("### TIME: 60 ms")
""")
            rc, out, _ = run_cli(check_perf_smoke.main, [
                "--binary", str(stub_fixed),
                "--tests", "Micro/postfix-incr.js,Micro/prefix-incr.js",
                "--iterations", "2",
            ])
            self.assertEqual(rc, 0, out)
            self.assertIn("ESBigInt-off", out)
            self.assertIn("PASS (2/2", out)

            # Release build (flaglist disabled): runs smoke invariants cleanly.
            stub_release = write_stub_ch(root / "ch_release.py", """
import sys
s = sys.argv[-1]
if s == "-?":
    print("Note: [flaglist] is not supported in Release builds.")
    raise SystemExit(0)
if "-ESBigInt-" in sys.argv:
    raise SystemExit(2)
print("### TIME: 55 ms")
""")
            rc_rel, out_rel, _ = run_cli(check_perf_smoke.main, [
                "--binary", str(stub_release),
                "--tests", "Micro/postfix-incr.js,Micro/prefix-incr.js",
                "--iterations", "2",
            ])
            self.assertEqual(rc_rel, 0, out_rel)
            self.assertIn("Release build (flaglist disabled)", out_rel)

            # Advisory baseline-json using actual checked-in schema (skipping excluded zlib).
            head_json = ROOT / "tools" / "bench" / "results" / "head-jit-darwin-arm64.json"
            rc_adv, out_adv, _ = run_cli(check_perf_smoke.main, [
                "--binary", str(stub_release),
                "--baseline-json", str(head_json),
                "--tests", "Kraken/stanford-crypto-ccm.js",
                "--iterations", "2",
            ])
            self.assertEqual(rc_adv, 0, out_adv)

            # Strict baseline catches regression against baseline JSON.
            base_json = root / "base.json"
            base_json.write_text(json.dumps({
                "results": [{"test": "Kraken/stanford-crypto-ccm.js", "kind": "time", "median": 20.0}]
            }), encoding="utf-8")
            rc_adv2, out_adv2, _ = run_cli(check_perf_smoke.main, [
                "--binary", str(stub_release),
                "--baseline-json", str(base_json),
                "--tests", "Kraken/stanford-crypto-ccm.js",
                "--iterations", "2",
            ])
            self.assertEqual(rc_adv2, 0, out_adv2)
            self.assertIn("ADVISORY WARN", out_adv2)

            rc_strict, _, err_strict = run_cli(check_perf_smoke.main, [
                "--binary", str(stub_release),
                "--baseline-json", str(base_json),
                "--strict-baseline",
                "--tests", "Kraken/stanford-crypto-ccm.js",
                "--iterations", "2",
            ])
            self.assertEqual(rc_strict, 1)
            self.assertIn("Kraken/stanford-crypto-ccm.js", err_strict)

    def test_cli_validation_errors_exit_2(self):
        with tempfile.TemporaryDirectory(prefix="perf-gate-cli-") as tmp:
            root = Path(tmp)
            stub = write_stub_ch(root / "ch.py", "print('### TIME: 10 ms')")
            bad_invocations = [
                [],
                ["--binary-a", str(stub)],
                ["--binary", str(stub), "--binary-a", str(stub), "--binary-b", str(stub)],
                ["--binary", str(stub), "--suite", "NoSuchSuite"],
                ["--binary", str(stub), "--tests", "NoSuchBench.js"],
                ["--binary", str(stub), "--iterations", "0"],
                ["--binary", str(stub), "--timeout", "-1"],
                ["--binary", str(stub), "--max-regression-pct", "-5"],
                ["--binary", str(stub), "--tolerance", "badformat"],
                ["--binary", str(stub), "--tolerance", "a=-1"],
                ["--binary", str(stub), "--baseline-json", str(root / "missing.json")],
            ]
            for argv in bad_invocations:
                rc, _, _ = run_cli(check_perf_smoke.main, argv)
                self.assertEqual(rc, 2, f"expected rc=2 for {argv!r}, got {rc}")

    def test_self_test_subprocess_passes(self):
        proc = subprocess.run(
            [sys.executable, str(HERE / "check_perf_smoke.py"), "--self-test"],
            capture_output=True, text=True, timeout=30,
        )
        self.assertEqual(proc.returncode, 0, proc.stdout + proc.stderr)
        self.assertIn("check_perf_smoke self-test: ALL NEGATIVE CONTROLS BEHAVE", proc.stdout)


class TestWave2PerfSmokeVariance(unittest.TestCase):
    def test_defaults_iterations_and_warmup(self):
        self.assertGreaterEqual(check_perf_smoke.DEFAULT_ITERATIONS, 9)
        self.assertGreaterEqual(check_perf_smoke.DEFAULT_WARMUP, 1)
        args = check_perf_smoke.parse_args(["--binary", "/bin/true"])
        self.assertEqual(args.iterations, check_perf_smoke.DEFAULT_ITERATIONS)
        self.assertEqual(args.warmup, check_perf_smoke.DEFAULT_WARMUP)
        self.assertGreater(args.max_mad_pct, 0.0)

    def test_high_mad_median_ab_returns_inconclusive_zero(self):
        entry = check_perf_smoke.BenchmarkEntry("Kraken", "kraken.js", "Kraken/kraken.js", "time", None, "/* stub */")
        base_vals = [100.0] * 9
        noisy_vals = [90.0, 110.0, 130.0, 150.0, 170.0, 190.0, 210.0, 230.0, 250.0]
        seq = {"i": 0}

        def fake_runner(cmd, **kwargs):
            _ = cmd, kwargs
            idx = seq["i"]
            seq["i"] += 1
            if idx < 2:
                val = 100.0 if idx == 0 else 170.0
            else:
                pair_idx = idx - 2
                val = base_vals[pair_idx // 2] if (pair_idx % 2 == 0) else noisy_vals[pair_idx // 2]
            return 0, f"### TIME: {val} ms\n", ""

        rc, payload = check_perf_smoke.run_ab_gate(
            "a", "da", [], "A", "b", "db", [], "B",
            [entry], [], iterations=9, warmup=1, max_regression_pct=15.0, max_mad_pct=15.0,
            runner=fake_runner,
        )
        self.assertEqual(rc, 0)
        self.assertEqual(payload["summary"]["failed"], 0)
        self.assertEqual(payload["summary"]["inconclusive"], 1)
        self.assertEqual(payload["summary"]["verdict"], "INCONCLUSIVE")

    def test_high_mad_median_strict_baseline_inconclusive(self):
        entry = check_perf_smoke.BenchmarkEntry("Kraken", "kraken.js", "Kraken/kraken.js", "time", None, "/* stub */")
        noisy_vals = [90.0, 110.0, 130.0, 150.0, 170.0, 190.0, 210.0, 230.0, 250.0]
        seq = {"i": 0}

        def fake_runner(cmd, **kwargs):
            _ = cmd, kwargs
            idx = seq["i"]
            seq["i"] += 1
            val = 170.0 if idx == 0 else noisy_vals[idx - 1]
            return 0, f"### TIME: {val} ms\n", ""

        rc, payload = check_perf_smoke.run_single_mode(
            "b", "db", [], [entry], [],
            iterations=9, warmup=1, max_regression_pct=15.0, max_mad_pct=15.0,
            baseline_map={"Kraken/kraken.js": {"kind": "time", "median": 100.0}},
            strict_baseline=True,
            runner=fake_runner,
        )
        self.assertEqual(rc, 0)
        self.assertEqual(payload["summary"]["failed"], 0)
        self.assertEqual(payload["summary"]["inconclusive"], 1)


if __name__ == "__main__":
    unittest.main()
