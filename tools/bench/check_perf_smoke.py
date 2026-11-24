#!/usr/bin/env python3
#-------------------------------------------------------------------------------------------------------
# Copyright (c) ChakraCore Project Contributors. All rights reserved.
# Licensed under the MIT license. See LICENSE.txt file in the project root for full license information.
#-------------------------------------------------------------------------------------------------------
"""Same-host interleaved A/B performance smoke gate (T-2 / PE-7).

Eliminates flaky cross-day or absolute wall-clock thresholds by running
benchmark iterations interleaved (A, B, A, B, ...) on the same host and
failing only on strict execution errors (rc != 0, timeout, missing or
mismatched ### TIME/SCORE marker) or median regression exceeding tolerance.

Modes:
  1. Interleaved A/B comparison (--binary-a / --binary-b):
       check_perf_smoke.py --binary-a out/jit/bin/ch --binary-b out/release-jit/bin/ch
  2. Single-binary A/B (-ESBigInt- vs default on flaglist builds, or
     smoke-invariant check on Release builds) (--binary):
       check_perf_smoke.py --binary out/jit/bin/ch
  3. Advisory baseline-JSON comparison (--binary + --baseline-json):
       check_perf_smoke.py --binary out/jit/bin/ch \\
           --baseline-json tools/bench/results/head-jit-darwin-arm64.json

Default fast subset (~52 s total at 3 iterations on ARM64 JIT):
  Kraken/stanford-crypto-ccm.js, Kraken/stanford-crypto-aes.js,
  Kraken/audio-fft.js, Octane/crypto.js, Octane/richards.js, Octane/splay.js,
  plus built-in Micro/postfix-incr.js (j++) and Micro/prefix-incr.js (++j).

Exit codes:
  0: all executed runs succeeded and every test stayed within tolerance.
  1: one or more tests regressed, crashed, timed out, or missed markers.
  2: invalid CLI arguments, missing binaries/files, or empty selection.
"""

import argparse
import contextlib
from dataclasses import dataclass
import hashlib
import io
import json
import math
import os
from pathlib import Path
import platform
import subprocess
import sys
import tempfile
import time

HERE = Path(__file__).resolve().parent
ROOT = HERE.parent.parent
sys.path.insert(0, str(HERE))
sys.path.insert(0, str(ROOT / "tools" / "build"))

import common
import run as bench_run

DEFAULT_FAST_TESTS = (
    "Kraken/stanford-crypto-ccm.js",
    "Kraken/stanford-crypto-aes.js",
    "Kraken/audio-fft.js",
    "Octane/crypto.js",
    "Octane/richards.js",
    "Octane/splay.js",
)

MICRO_SUITE = "Micro"
DEFAULT_ITERATIONS = 3
DEFAULT_MAX_REGRESSION_PCT = 15.0
DEFAULT_TIMEOUT = 60.0

POSTFIX_INCR_JS = """\
(function () {
    function runPostfix(n) {
        var s = 0;
        var j = 0;
        while (n-- > 0) {
            s = (s + j++) & 0xffff;
        }
        return s;
    }
    var warm = runPostfix(10000);
    if (warm !== 56568) {
        throw new Error("postfix warmup checksum mismatch: " + warm);
    }
    var t0 = Date.now();
    var res = 0;
    for (var k = 0; k < 3; k++) {
        res = (res + runPostfix(20000000)) & 0xffff;
    }
    var elapsed = Date.now() - t0;
    if (res !== 48256) {
        throw new Error("postfix checksum mismatch: " + res);
    }
    var line = "### TIME: " + elapsed + " ms (checksum=" + res + ")";
    if (typeof WScript !== "undefined") {
        WScript.Echo(line);
    } else {
        print(line);
    }
})();
"""

PREFIX_INCR_JS = """\
(function () {
    function runPrefix(n) {
        var s = 0;
        var j = 0;
        while (n-- > 0) {
            s = (s + ++j) & 0xffff;
        }
        return s;
    }
    var warm = runPrefix(10000);
    if (warm !== 1032) {
        throw new Error("prefix warmup checksum mismatch: " + warm);
    }
    var t0 = Date.now();
    var res = 0;
    for (var k = 0; k < 3; k++) {
        res = (res + runPrefix(20000000)) & 0xffff;
    }
    var elapsed = Date.now() - t0;
    if (res !== 17280) {
        throw new Error("prefix checksum mismatch: " + res);
    }
    var line = "### TIME: " + elapsed + " ms (checksum=" + res + ")";
    if (typeof WScript !== "undefined") {
        WScript.Echo(line);
    } else {
        print(line);
    }
})();
"""

MICRO_SPECS = (
    ("Micro/postfix-incr.js", "postfix-incr.js", "time", POSTFIX_INCR_JS),
    ("Micro/prefix-incr.js", "prefix-incr.js", "time", PREFIX_INCR_JS),
)


class UsageError(Exception):
    """Invalid CLI arguments, missing files, or empty test selection (exit 2)."""


@dataclass(frozen=True)
class BenchmarkEntry:
    suite: str
    fname: str
    label: str
    kind: str
    path: Path | None
    micro_source: str | None


def build_catalog(bench_dir=None):
    """Build ordered catalog of in-tree benchmarks plus built-in Micro probes."""
    bdir = Path(bench_dir) if bench_dir is not None else bench_run.BENCH_DIR
    entries = []
    for suite, (subdir, kind) in bench_run.SUITES.items():
        sdir = bdir / subdir
        if not sdir.is_dir():
            continue
        for p in sorted(sdir.glob("*.js")):
            entries.append(BenchmarkEntry(
                suite=suite,
                fname=p.name,
                label=f"{suite}/{p.name}",
                kind=kind,
                path=p,
                micro_source=None,
            ))
    for label, fname, kind, src in MICRO_SPECS:
        entries.append(BenchmarkEntry(
            suite=MICRO_SUITE,
            fname=fname,
            label=label,
            kind=kind,
            path=None,
            micro_source=src,
        ))
    return entries


def _split_csv_list(items):
    tokens = []
    for item in items:
        for part in item.split(","):
            tok = part.strip()
            if tok:
                tokens.append(tok)
    return tokens


def select_tests(catalog, suites_arg=None, tests_arg=None, micro_probe=None):
    """Select runnable and excluded benchmark entries from catalog.

    micro_probe: None (auto: enabled for default subset, disabled when
                 --suite/--tests explicitly passed), True, or False.
    """
    valid_suites = set(bench_run.SUITES.keys()) | {MICRO_SUITE}
    suites = _split_csv_list(suites_arg or [])
    unknown_suites = [s for s in suites if s not in valid_suites]
    if unknown_suites:
        raise UsageError(
            f"unknown suite(s): {unknown_suites}; known: {sorted(valid_suites)}"
        )

    selectors = _split_csv_list(tests_arg or [])
    by_label = {e.label: e for e in catalog}

    selected = []
    seen = set()

    def _add(entry):
        if entry.label not in seen:
            seen.add(entry.label)
            selected.append(entry)

    if not suites and not selectors:
        for label in DEFAULT_FAST_TESTS:
            if label not in by_label:
                raise UsageError(f"default benchmark missing from tree: {label}")
            _add(by_label[label])
        if micro_probe is not False:
            for e in catalog:
                if e.suite == MICRO_SUITE:
                    _add(e)
    else:
        pool = [e for e in catalog if not suites or e.suite in suites]
        if selectors:
            for tok in selectors:
                exact_label = [e for e in pool if e.label == tok]
                if exact_label:
                    matches = exact_label
                else:
                    exact_fname = [e for e in pool if e.fname == tok]
                    if exact_fname:
                        matches = exact_fname
                    else:
                        matches = [e for e in pool if tok in e.label]
                if not matches:
                    raise UsageError(f"test selector matched nothing: {tok!r}")
                for e in matches:
                    _add(e)
        else:
            for e in pool:
                _add(e)

        if micro_probe is True:
            for e in catalog:
                if e.suite == MICRO_SUITE:
                    _add(e)
        elif micro_probe is False and not selectors and MICRO_SUITE not in suites:
            selected = [e for e in selected if e.suite != MICRO_SUITE]

    runnable = []
    excluded = []
    for e in selected:
        why = None if e.suite == MICRO_SUITE else bench_run.exclusion_reason(e.suite, e.fname)
        if why:
            excluded.append((e, why))
        else:
            runnable.append(e)

    if not runnable:
        if excluded:
            names = ", ".join(e.label for e, _ in excluded)
            raise UsageError(f"all selected tests are excluded on this platform: {names}")
        raise UsageError("no tests selected")
    return runnable, excluded


def parse_tolerances(tolerance_args):
    """Parse --tolerance TEST=PCT arguments into a mapping."""
    tol_map = {}
    for item in tolerance_args or []:
        if "=" not in item:
            raise UsageError(f"bad --tolerance {item!r}: expected TEST=PCT")
        name, raw_pct = item.split("=", 1)
        name = name.strip()
        if not name:
            raise UsageError(f"bad --tolerance {item!r}: empty test name")
        try:
            pct = float(raw_pct.strip())
        except ValueError as e:
            raise UsageError(f"bad --tolerance {item!r}: invalid float") from e
        if not math.isfinite(pct) or pct < 0.0:
            raise UsageError(f"bad --tolerance {item!r}: pct must be >= 0")
        tol_map[name] = pct
    return tol_map


def tolerance_for(entry, default_pct, tol_map):
    """Return regression tolerance percentage for entry."""
    if entry.label in tol_map:
        return tol_map[entry.label]
    if entry.fname in tol_map:
        return tol_map[entry.fname]
    return default_pct


def validate_binary(raw_path, role):
    """Verify binary exists, is executable, and return (Path, sha256)."""
    if not raw_path:
        raise UsageError(f"missing {role} path")
    p = Path(raw_path)
    if not p.exists() or not p.is_file():
        raise UsageError(f"{role} not found: {raw_path}")
    resolved = p.resolve()
    if not os.access(resolved, os.X_OK):
        raise UsageError(f"{role} is not executable: {resolved}")
    digest = hashlib.sha256(resolved.read_bytes()).hexdigest()
    return resolved, digest


def flaglist_supported(binary, timeout=10.0, runner=None):
    """Return False for Release builds where ch -? disables [flaglist]."""
    runner = runner or common.run_capture
    try:
        rc, out, err = runner([str(binary), "-?"], timeout=timeout, merge_stderr=True)
    except (subprocess.TimeoutExpired, OSError):
        return True
    if rc != 0:
        return True
    return "not supported in Release builds" not in ((out or "") + (err or ""))


def load_baseline_json(path):
    """Load median baseline map from a run.py or check_perf_smoke.py JSON file."""
    p = Path(path)
    if not p.is_file():
        raise UsageError(f"baseline JSON not found: {path}")
    try:
        data = json.loads(p.read_text(encoding="utf-8"))
    except (OSError, ValueError) as e:
        raise UsageError(f"malformed baseline JSON {path}: {e}") from e
    if not isinstance(data, dict) or not isinstance(data.get("results"), list):
        raise UsageError(f"malformed baseline JSON {path}: expected object with 'results' list")
    baseline = {}
    for row in data["results"]:
        if not isinstance(row, dict):
            continue
        name = row.get("test")
        kind = row.get("kind")
        med = row.get("median")
        if med is None and isinstance(row.get("b"), dict):
            med = row["b"].get("median")
        if med is None and isinstance(row.get("single"), dict):
            med = row["single"].get("median")
        if isinstance(name, str) and isinstance(kind, str) and isinstance(med, (int, float)):
            if math.isfinite(float(med)):
                baseline[name] = {
                    "kind": kind,
                    "median": float(med),
                    "mad": float(row.get("mad", 0.0))
                    if isinstance(row.get("mad"), (int, float)) else 0.0,
                }
    return baseline


def _first_error_line(text):
    for line in text.splitlines():
        s = line.strip()
        if s:
            return s[:120]
    return ""


def run_single(binary, flags, script_path, expected_kind, timeout, runner=None):
    """Execute one benchmark run and strictly validate rc, marker, and value."""
    runner = runner or common.run_capture
    cmd = [str(binary), *flags, script_path.name]
    start = time.monotonic()
    try:
        rc, out, err = runner(
            cmd, timeout=timeout, cwd=script_path.parent, merge_stderr=True
        )
    except subprocess.TimeoutExpired:
        wall = round(time.monotonic() - start, 3)
        return {
            "rc": None,
            "wallSec": wall,
            "marker": None,
            "value": None,
            "timeout": True,
            "ok": False,
            "error": f"timeout after {timeout:g}s",
        }
    except OSError as e:
        wall = round(time.monotonic() - start, 3)
        return {
            "rc": None,
            "wallSec": wall,
            "marker": None,
            "value": None,
            "ok": False,
            "error": f"exec error: {e}",
        }

    wall = round(time.monotonic() - start, 3)
    combined = (out or "") + (err or "")
    if rc != 0:
        snippet = _first_error_line(combined)
        detail = f"rc={rc}" + (f" ({snippet})" if snippet else "")
        return {
            "rc": rc,
            "wallSec": wall,
            "marker": None,
            "value": None,
            "ok": False,
            "error": detail,
        }
    if "*** FAIL ***" in combined:
        snippet = _first_error_line(combined)
        return {
            "rc": rc,
            "wallSec": wall,
            "marker": None,
            "value": None,
            "ok": False,
            "error": f"benchmark reported failure ({snippet})",
        }
    matches = bench_run.MARKER_RE.findall(combined)
    if not matches:
        return {
            "rc": rc,
            "wallSec": wall,
            "marker": None,
            "value": None,
            "ok": False,
            "error": "missing ### TIME/SCORE marker",
        }
    want_marker = expected_kind.upper()
    matching = [(mk, val_str) for mk, val_str in matches if mk == want_marker]
    if not matching:
        got_kinds = ",".join(sorted({mk for mk, _ in matches}))
        return {
            "rc": rc,
            "wallSec": wall,
            "marker": matches[0][0],
            "value": None,
            "ok": False,
            "error": f"expected {want_marker} marker, got {got_kinds}",
        }
    if len(matching) > 1:
        return {
            "rc": rc,
            "wallSec": wall,
            "marker": want_marker,
            "value": None,
            "ok": False,
            "error": f"ambiguous multiple {want_marker} markers ({len(matching)})",
        }
    val = float(matching[0][1])
    if not math.isfinite(val) or (want_marker == "SCORE" and val <= 0.0):
        return {
            "rc": rc,
            "wallSec": wall,
            "marker": want_marker,
            "value": val if math.isfinite(val) else None,
            "ok": False,
            "error": f"invalid {want_marker} value: {matching[0][1]}",
        }
    return {
        "rc": rc,
        "wallSec": wall,
        "marker": want_marker,
        "value": val,
        "ok": True,
        "error": None,
    }


def compute_comparison(kind, ref_med, cand_med, tol_pct):
    """Compare candidate median against reference median under tol_pct.

    Returns dict with ratioBA, deltaPct, regressionPct, speedupRatio, regressed.
    - For 'time' (lower is better): regressionPct = (cand/ref - 1)*100.
    - For 'score' (higher is better): regressionPct = (1 - cand/ref)*100.
    """
    if ref_med == 0.0:
        if cand_med == 0.0:
            return {
                "ratioBA": 1.0,
                "deltaPct": 0.0,
                "regressionPct": 0.0,
                "speedupRatio": 1.0,
                "regressed": False,
            }
        # cand_med > 0 against 0 baseline: for 'time' treat as regression.
        is_time = kind == "time"
        return {
            "ratioBA": None,
            "deltaPct": None,
            "regressionPct": None,
            "speedupRatio": 0.0 if is_time else None,
            "regressed": is_time,
        }

    ratio_ba = cand_med / ref_med
    delta_pct = (ratio_ba - 1.0) * 100.0
    if kind == "time":
        regression_pct = delta_pct
        speedup_ratio = (ref_med / cand_med) if cand_med > 0.0 else None
    else:
        regression_pct = -delta_pct
        speedup_ratio = ratio_ba
    regressed = regression_pct > (tol_pct + 1e-9)
    return {
        "ratioBA": ratio_ba,
        "deltaPct": delta_pct,
        "regressionPct": regression_pct,
        "speedupRatio": speedup_ratio,
        "regressed": regressed,
    }


def _json_safe_num(val):
    if val is None:
        return None
    if isinstance(val, float) and not math.isfinite(val):
        return None
    return val


def _format_med_mad(med, mad):
    return f"{med:.1f}+/-{mad:.1f}"


def _format_ratio_delta(cmp_info):
    ratio = cmp_info["ratioBA"]
    delta = cmp_info["deltaPct"]
    ratio_s = f"{ratio:.3f}x" if ratio is not None else "inf"
    delta_s = f"{delta:+.1f}%" if delta is not None else "+inf%"
    return ratio_s, delta_s


def _materialize_script(entry, micro_dir):
    if entry.path is not None:
        return entry.path
    p = micro_dir / entry.fname
    p.write_text(entry.micro_source, encoding="utf-8")
    return p


def _summarize_failures(side_label, runs):
    for idx, r in enumerate(runs, 1):
        if not r["ok"]:
            return f"{side_label} run #{idx}: {r['error']}"
    return None


def _advisory_note(entry, cand_med, tol_pct, baseline_map):
    if not baseline_map or entry.label not in baseline_map:
        return None
    base = baseline_map[entry.label]
    if base["kind"] != entry.kind:
        return {
            "baselineMedian": base["median"],
            "error": f"kind mismatch ({base['kind']} vs {entry.kind})",
            "regressed": False,
        }
    cmp_info = compute_comparison(entry.kind, base["median"], cand_med, tol_pct)
    return {
        "baselineMedian": base["median"],
        "ratioVsBaseline": _json_safe_num(cmp_info["ratioBA"]),
        "deltaPct": _json_safe_num(cmp_info["deltaPct"]),
        "regressionPct": _json_safe_num(cmp_info["regressionPct"]),
        "regressed": cmp_info["regressed"],
    }


def _micro_ratio(entries_by_label, key):
    post = entries_by_label.get("Micro/postfix-incr.js")
    pref = entries_by_label.get("Micro/prefix-incr.js")
    if not post or not pref:
        return None
    p_info = post.get(key)
    c_info = pref.get(key)
    if not p_info or not c_info:
        return None
    p_med = p_info.get("median")
    c_med = c_info.get("median")
    if p_med is None or c_med is None or c_med <= 0.0:
        return None
    return p_med / c_med


def run_ab_gate(
    binary_a,
    digest_a,
    flags_a,
    label_a,
    binary_b,
    digest_b,
    flags_b,
    label_b,
    runnable,
    excluded,
    iterations=DEFAULT_ITERATIONS,
    warmup=0,
    timeout=DEFAULT_TIMEOUT,
    max_regression_pct=DEFAULT_MAX_REGRESSION_PCT,
    tol_map=None,
    baseline_map=None,
    runner=None,
):
    """Execute interleaved A/B smoke gate and return (exit_code, payload)."""
    tol_map = tol_map or {}
    print(
        f"check_perf_smoke: mode=ab iterations={iterations} warmup={warmup} "
        f"max_regression={max_regression_pct:g}% timeout={timeout:g}s"
    )
    print(
        f"check_perf_smoke: A ({label_a}): {binary_a} sha256:{digest_a} "
        f"flags={' '.join(flags_a) or '(none)'}"
    )
    print(
        f"check_perf_smoke: B ({label_b}): {binary_b} sha256:{digest_b} "
        f"flags={' '.join(flags_b) or '(none)'}"
    )
    print(
        f"{'test':36} {'kind':5} {'A (' + label_a + ')':>18} "
        f"{'B (' + label_b + ')':>18} {'B/A':>8} {'delta':>8} {'tol':>6} status"
    )
    print("-" * 114)

    results = []
    failed_tests = []
    advisory_warns = []

    with tempfile.TemporaryDirectory(prefix="perf-smoke-micro-") as tmp:
        micro_dir = Path(tmp)
        for entry, why in excluded:
            print(f"{entry.label:36} {entry.kind:5} {'EXCLUDED':>18} {'':>18} {'-':>8} {'-':>8} {'-':>6} EXCLUDED ({why[:40]})")
            results.append({"test": entry.label, "kind": entry.kind, "status": "excluded", "reason": why})

        for entry in runnable:
            script_path = _materialize_script(entry, micro_dir)
            tol_pct = tolerance_for(entry, max_regression_pct, tol_map)
            warm_err = None
            for w_idx in range(warmup):
                wa = run_single(binary_a, flags_a, script_path, entry.kind, timeout, runner=runner)
                if not wa["ok"] and warm_err is None:
                    warm_err = f"A warmup #{w_idx + 1}: {wa['error']}"
                wb = run_single(binary_b, flags_b, script_path, entry.kind, timeout, runner=runner)
                if not wb["ok"] and warm_err is None:
                    warm_err = f"B warmup #{w_idx + 1}: {wb['error']}"

            runs_a = []
            runs_b = []
            # Strict A, B, A, B, ... interleaved scheduling per benchmark.
            for _ in range(iterations):
                runs_a.append(run_single(binary_a, flags_a, script_path, entry.kind, timeout, runner=runner))
                runs_b.append(run_single(binary_b, flags_b, script_path, entry.kind, timeout, runner=runner))

            err_a = _summarize_failures("A", runs_a)
            err_b = _summarize_failures("B", runs_b)
            err_parts = [e for e in (warm_err, err_a, err_b) if e]
            exec_err = "; ".join(err_parts) if err_parts else None

            if exec_err is not None:
                a_disp = err_a if err_a else _format_med_mad(*bench_run.median_mad([r["value"] for r in runs_a]))
                b_disp = err_b if err_b else _format_med_mad(*bench_run.median_mad([r["value"] for r in runs_b]))
                status_text = f"FAIL ({exec_err})"
                print(
                    f"{entry.label:36} {entry.kind:5} {a_disp[:18]:>18} "
                    f"{b_disp[:18]:>18} {'-':>8} {'-':>8} {tol_pct:5.1f}% {status_text}"
                )
                failed_tests.append((entry.label, exec_err))
                results.append({
                    "test": entry.label,
                    "kind": entry.kind,
                    "status": "fail",
                    "error": exec_err,
                    "tolerancePct": tol_pct,
                    "a": {"runs": runs_a},
                    "b": {"runs": runs_b},
                })
                continue

            vals_a = [r["value"] for r in runs_a]
            vals_b = [r["value"] for r in runs_b]
            med_a, mad_a = bench_run.median_mad(vals_a)
            med_b, mad_b = bench_run.median_mad(vals_b)
            cmp_info = compute_comparison(entry.kind, med_a, med_b, tol_pct)
            adv = _advisory_note(entry, med_b, tol_pct, baseline_map)
            ratio_s, delta_s = _format_ratio_delta(cmp_info)

            if cmp_info["regressed"]:
                reg_pct = cmp_info["regressionPct"]
                reg_s = f"{reg_pct:.1f}%" if reg_pct is not None else "inf%"
                reason = f"regressed {reg_s} > {tol_pct:.1f}%"
                status_text = f"FAIL ({reason})"
                status_key = "fail"
                failed_tests.append((entry.label, reason))
            else:
                status_text = "PASS"
                status_key = "pass"
                if adv and adv.get("regressed"):
                    adv_d = adv["deltaPct"]
                    status_text = f"PASS [ADVISORY WARN vs baseline: {adv_d:+.1f}%]"
                    advisory_warns.append(entry.label)

            print(
                f"{entry.label:36} {entry.kind:5} "
                f"{_format_med_mad(med_a, mad_a):>18} "
                f"{_format_med_mad(med_b, mad_b):>18} "
                f"{ratio_s:>8} {delta_s:>8} {tol_pct:5.1f}% {status_text}"
            )
            row = {
                "test": entry.label,
                "kind": entry.kind,
                "status": status_key,
                "tolerancePct": tol_pct,
                "a": {"median": med_a, "mad": mad_a, "runs": runs_a},
                "b": {"median": med_b, "mad": mad_b, "runs": runs_b},
                "ratioBA": _json_safe_num(cmp_info["ratioBA"]),
                "deltaPct": _json_safe_num(cmp_info["deltaPct"]),
                "regressionPct": _json_safe_num(cmp_info["regressionPct"]),
                "speedupRatio": _json_safe_num(cmp_info["speedupRatio"]),
            }
            if adv is not None:
                row["baselineAdvisory"] = adv
            results.append(row)

    print("-" * 114)
    by_label = {r["test"]: r for r in results}
    micro_a = _micro_ratio(by_label, "a")
    micro_b = _micro_ratio(by_label, "b")
    if micro_a is not None and micro_b is not None:
        print(f"check_perf_smoke: micro j++/++j ratio: A={micro_a:.2f}x B={micro_b:.2f}x")

    passed_count = sum(1 for r in results if r["status"] == "pass")
    total_run = len(runnable)
    summary = {
        "mode": "ab",
        "passed": passed_count,
        "failed": len(failed_tests),
        "total": total_run,
        "excluded": len(excluded),
        "advisoryWarnings": advisory_warns,
        "microPostfixPrefixRatioA": micro_a,
        "microPostfixPrefixRatioB": micro_b,
        "verdict": "FAIL" if failed_tests else "PASS",
    }
    payload = {
        "mode": "ab",
        "platform": sys.platform,
        "machine": platform.machine(),
        "iterations": iterations,
        "warmup": warmup,
        "timeoutSec": timeout,
        "maxRegressionPct": max_regression_pct,
        "tolerances": tol_map,
        "a": {"label": label_a, "binary": str(binary_a), "sha256": digest_a, "flags": flags_a},
        "b": {"label": label_b, "binary": str(binary_b), "sha256": digest_b, "flags": flags_b},
        "results": results,
        "summary": summary,
    }
    if failed_tests:
        names = ", ".join(f"{name} ({why})" for name, why in failed_tests)
        msg = f"check_perf_smoke: FAIL ({len(failed_tests)}/{total_run} failed: {names})"
        print(msg, file=sys.stderr)
        return 1, payload
    print(f"check_perf_smoke: PASS ({passed_count}/{total_run} tests within threshold)")
    return 0, payload


def run_single_mode(
    binary,
    digest,
    flags,
    runnable,
    excluded,
    iterations=DEFAULT_ITERATIONS,
    warmup=0,
    timeout=DEFAULT_TIMEOUT,
    max_regression_pct=DEFAULT_MAX_REGRESSION_PCT,
    tol_map=None,
    baseline_map=None,
    strict_baseline=False,
    note=None,
    runner=None,
):
    """Run single-binary smoke invariants and optional baseline-json comparison."""
    tol_map = tol_map or {}
    mode_name = "baseline-strict" if strict_baseline else ("baseline-advisory" if baseline_map is not None else "smoke-only")
    print(
        f"check_perf_smoke: mode={mode_name} iterations={iterations} warmup={warmup} "
        f"max_regression={max_regression_pct:g}% timeout={timeout:g}s"
    )
    print(
        f"check_perf_smoke: binary={binary} sha256:{digest} "
        f"flags={' '.join(flags) or '(none)'}"
    )
    if note:
        print(f"check_perf_smoke: note: {note}")
    print(f"{'test':36} {'kind':5} {'median+/-mad':>18} {'baseline':>12} {'delta':>8} {'tol':>6} status")
    print("-" * 102)

    results = []
    failed_tests = []
    advisory_warns = []

    with tempfile.TemporaryDirectory(prefix="perf-smoke-micro-") as tmp:
        micro_dir = Path(tmp)
        for entry, why in excluded:
            print(f"{entry.label:36} {entry.kind:5} {'EXCLUDED':>18} {'-':>12} {'-':>8} {'-':>6} EXCLUDED ({why[:40]})")
            results.append({"test": entry.label, "kind": entry.kind, "status": "excluded", "reason": why})

        for entry in runnable:
            script_path = _materialize_script(entry, micro_dir)
            tol_pct = tolerance_for(entry, max_regression_pct, tol_map)
            warm_err = None
            for w_idx in range(warmup):
                w = run_single(binary, flags, script_path, entry.kind, timeout, runner=runner)
                if not w["ok"] and warm_err is None:
                    warm_err = f"warmup #{w_idx + 1}: {w['error']}"

            runs = [
                run_single(binary, flags, script_path, entry.kind, timeout, runner=runner)
                for _ in range(iterations)
            ]
            exec_err = warm_err or _summarize_failures("run", runs)
            if exec_err is not None:
                print(f"{entry.label:36} {entry.kind:5} {exec_err[:18]:>18} {'-':>12} {'-':>8} {tol_pct:5.1f}% FAIL ({exec_err})")
                failed_tests.append((entry.label, exec_err))
                results.append({
                    "test": entry.label,
                    "kind": entry.kind,
                    "status": "fail",
                    "error": exec_err,
                    "tolerancePct": tol_pct,
                    "single": {"runs": runs},
                })
                continue

            vals = [r["value"] for r in runs]
            med, mad = bench_run.median_mad(vals)
            adv = _advisory_note(entry, med, tol_pct, baseline_map)
            base_s = f"{adv['baselineMedian']:.1f}" if adv else "-"
            delta_s = f"{adv['deltaPct']:+.1f}%" if (adv and adv.get("deltaPct") is not None) else "-"

            status_key = "pass"
            status_text = "PASS"
            if adv and adv.get("error"):
                status_text = f"FAIL ({adv['error']})" if strict_baseline else f"PASS [WARN: {adv['error']}]"
                if strict_baseline:
                    status_key = "fail"
                    failed_tests.append((entry.label, adv["error"]))
            elif adv and adv.get("regressed"):
                reg_pct = adv["regressionPct"]
                reg_s = f"{reg_pct:.1f}%" if reg_pct is not None else "inf%"
                reason = f"regressed {reg_s} > {tol_pct:.1f}% vs baseline"
                if strict_baseline:
                    status_key = "fail"
                    status_text = f"FAIL ({reason})"
                    failed_tests.append((entry.label, reason))
                else:
                    status_text = f"ADVISORY WARN ({reason})"
                    advisory_warns.append(entry.label)

            print(
                f"{entry.label:36} {entry.kind:5} {_format_med_mad(med, mad):>18} "
                f"{base_s:>12} {delta_s:>8} {tol_pct:5.1f}% {status_text}"
            )
            row = {
                "test": entry.label,
                "kind": entry.kind,
                "status": status_key,
                "tolerancePct": tol_pct,
                "single": {"median": med, "mad": mad, "runs": runs},
            }
            if adv is not None:
                row["baselineAdvisory"] = adv
            results.append(row)

    print("-" * 102)
    by_label = {r["test"]: r for r in results}
    micro_ratio = _micro_ratio(by_label, "single")
    if micro_ratio is not None:
        print(f"check_perf_smoke: micro j++/++j ratio: {micro_ratio:.2f}x")

    passed_count = sum(1 for r in results if r["status"] == "pass")
    total_run = len(runnable)
    summary = {
        "mode": mode_name,
        "passed": passed_count,
        "failed": len(failed_tests),
        "total": total_run,
        "excluded": len(excluded),
        "advisoryWarnings": advisory_warns,
        "microPostfixPrefixRatio": micro_ratio,
        "verdict": "FAIL" if failed_tests else "PASS",
    }
    payload = {
        "mode": mode_name,
        "platform": sys.platform,
        "machine": platform.machine(),
        "iterations": iterations,
        "warmup": warmup,
        "timeoutSec": timeout,
        "maxRegressionPct": max_regression_pct,
        "tolerances": tol_map,
        "binary": str(binary),
        "sha256": digest,
        "flags": flags,
        "results": results,
        "summary": summary,
    }
    if failed_tests:
        names = ", ".join(f"{name} ({why})" for name, why in failed_tests)
        print(f"check_perf_smoke: FAIL ({len(failed_tests)}/{total_run} failed: {names})", file=sys.stderr)
        return 1, payload
    warn_suffix = f", {len(advisory_warns)} advisory warning(s)" if advisory_warns else ""
    print(f"check_perf_smoke: PASS ({passed_count}/{total_run} smoke checks clean{warn_suffix})")
    return 0, payload


def _write_stub_ch(path, code):
    path.write_text(f"#!{sys.executable}\n{code}\n", encoding="utf-8")
    os.chmod(path, 0o755)


def self_test():
    """Positive and negative controls using hermetic Python stub ch executables."""
    bad = 0

    def report(name, cond, detail=""):
        print("self-test %-48s -> %s%s"
              % (name, "PASS" if cond else "FAIL",
                 (" (" + detail + ")") if detail else ""))
        return 0 if cond else 1

    with tempfile.TemporaryDirectory(prefix="perf-smoke-selftest-") as tmp:
        root = Path(tmp)
        trace_log = root / "schedule.log"

        stub_base = root / "stub_base.py"
        _write_stub_ch(stub_base, f"""
import os, sys
from pathlib import Path
script = sys.argv[-1]
flags = sys.argv[1:-1]
if script == "-?":
    print("Usage: ch [flaglist]")
    raise SystemExit(0)
with open({str(trace_log)!r}, "a", encoding="utf-8") as f:
    f.write("A:" + script + ":" + " ".join(flags) + "\\n")
if "crypto.js" in script or "richards.js" in script or "splay.js" in script:
    print("### SCORE: 10000")
else:
    print("### TIME: 100 ms")
""")

        stub_fast = root / "stub_fast.py"
        _write_stub_ch(stub_fast, f"""
import sys
script = sys.argv[-1]
flags = sys.argv[1:-1]
if script == "-?":
    print("Usage: ch [flaglist]")
    raise SystemExit(0)
with open({str(trace_log)!r}, "a", encoding="utf-8") as f:
    f.write("B:" + script + ":" + " ".join(flags) + "\\n")
if "crypto.js" in script or "richards.js" in script or "splay.js" in script:
    print("### SCORE: 10500")
else:
    print("### TIME: 92 ms")
""")

        stub_slow_time = root / "stub_slow_time.py"
        _write_stub_ch(stub_slow_time, """
import sys
script = sys.argv[-1]
if "stanford-crypto-ccm.js" in script:
    print("### TIME: 130 ms")
elif "crypto.js" in script or "richards.js" in script or "splay.js" in script:
    print("### SCORE: 10000")
else:
    print("### TIME: 100 ms")
""")

        stub_slow_score = root / "stub_slow_score.py"
        _write_stub_ch(stub_slow_score, """
import sys
script = sys.argv[-1]
if "crypto.js" in script:
    print("### SCORE: 8000")
elif "richards.js" in script or "splay.js" in script:
    print("### SCORE: 10000")
else:
    print("### TIME: 100 ms")
""")

        stub_crash = root / "stub_crash.py"
        _write_stub_ch(stub_crash, """
import sys
print("### TIME: 100 ms")
sys.stderr.write("fatal abort\\n")
raise SystemExit(11)
""")

        stub_no_marker = root / "stub_no_marker.py"
        _write_stub_ch(stub_no_marker, """
print("done without marker")
""")

        stub_wrong_marker = root / "stub_wrong_marker.py"
        _write_stub_ch(stub_wrong_marker, """
print("### TIME: 100 ms")
""")

        stub_hang = root / "stub_hang.py"
        _write_stub_ch(stub_hang, """
import time
time.sleep(10)
""")

        stub_esbigint = root / "stub_esbigint.py"
        _write_stub_ch(stub_esbigint, """
import sys
script = sys.argv[-1]
flags = sys.argv[1:-1]
if script == "-?":
    print("Usage: ch [flaglist]")
    raise SystemExit(0)
if "-ESBigInt-" in flags:
    print("### TIME: 50 ms")
else:
    print("### TIME: 150 ms")
""")

        # 1. Passing A/B + interleaved A, B, A, B scheduling verification.
        def run_quiet(argv):
            out_buf, err_buf = io.StringIO(), io.StringIO()
            with contextlib.redirect_stdout(out_buf), contextlib.redirect_stderr(err_buf):
                rc = main(argv)
            return rc, out_buf.getvalue(), err_buf.getvalue()

        trace_log.unlink(missing_ok=True)
        json_out = root / "report.json"
        rc_ok, out_ok, _ = run_quiet([
            "--binary-a", str(stub_base),
            "--binary-b", str(stub_fast),
            "--tests", "Kraken/stanford-crypto-ccm.js,Octane/crypto.js",
            "--iterations", "3",
            "--json-out", str(json_out),
        ])
        trace_lines = trace_log.read_text(encoding="utf-8").splitlines()
        want_order = [
            "A:stanford-crypto-ccm.js:",
            "B:stanford-crypto-ccm.js:",
            "A:stanford-crypto-ccm.js:",
            "B:stanford-crypto-ccm.js:",
            "A:stanford-crypto-ccm.js:",
            "B:stanford-crypto-ccm.js:",
            "A:crypto.js:",
            "B:crypto.js:",
            "A:crypto.js:",
            "B:crypto.js:",
            "A:crypto.js:",
            "B:crypto.js:",
        ]
        bad += report(
            "speedup/noise A/B passes rc=0",
            rc_ok == 0 and json_out.is_file() and "PASS (" in out_ok,
        )
        bad += report("interleaved A,B,A,B order verified", trace_lines == want_order,
                      detail="" if trace_lines == want_order else f"got {trace_lines}")

        # 2. Time regression > 15% fails rc=1 and names exact regressed test.
        rc_time_fail, _, err_time = run_quiet([
            "--binary-a", str(stub_base),
            "--binary-b", str(stub_slow_time),
            "--tests", "Kraken/stanford-crypto-ccm.js",
            "--iterations", "2",
        ])
        bad += report(
            "time regression (+30%) fails rc=1",
            rc_time_fail == 1 and "Kraken/stanford-crypto-ccm.js" in err_time,
        )

        # 3. Score regression > 15% fails rc=1 and names exact regressed test.
        rc_score_fail, _, err_score = run_quiet([
            "--binary-a", str(stub_base),
            "--binary-b", str(stub_slow_score),
            "--tests", "Octane/crypto.js",
            "--iterations", "2",
        ])
        bad += report(
            "score regression (-20%) fails rc=1",
            rc_score_fail == 1 and "Octane/crypto.js" in err_score,
        )

        # 4. Nonzero exit code fails hard even when marker was printed.
        rc_crash, _, _ = run_quiet([
            "--binary-a", str(stub_base),
            "--binary-b", str(stub_crash),
            "--tests", "Kraken/stanford-crypto-ccm.js",
            "--iterations", "2",
        ])
        bad += report("nonzero exit code fails rc=1", rc_crash == 1)

        # 5. Missing marker fails hard even when rc == 0.
        rc_nomark, _, _ = run_quiet([
            "--binary-a", str(stub_base),
            "--binary-b", str(stub_no_marker),
            "--tests", "Kraken/stanford-crypto-ccm.js",
            "--iterations", "2",
        ])
        bad += report("missing marker fails rc=1", rc_nomark == 1)

        # 6. Marker kind mismatch fails hard.
        rc_mismatch, _, _ = run_quiet([
            "--binary-a", str(stub_base),
            "--binary-b", str(stub_wrong_marker),
            "--tests", "Octane/crypto.js",
            "--iterations", "2",
        ])
        bad += report("marker kind mismatch fails rc=1", rc_mismatch == 1)

        # 7. Timeout fails hard without hanging.
        rc_timeout, _, _ = run_quiet([
            "--binary-a", str(stub_base),
            "--binary-b", str(stub_hang),
            "--tests", "Kraken/stanford-crypto-ccm.js",
            "--iterations", "1",
            "--timeout", "0.3",
        ])
        bad += report("timeout fails rc=1", rc_timeout == 1)

        # 8. Missing binary fails hard (rc=2).
        rc_missing, _, _ = run_quiet([
            "--binary-a", str(stub_base),
            "--binary-b", str(root / "does-not-exist"),
            "--tests", "Kraken/stanford-crypto-ccm.js",
        ])
        bad += report("missing binary fails rc=2", rc_missing == 2)

        # 9. Single --binary -ESBigInt- vs default detects regression.
        rc_esbigint, _, err_esb = run_quiet([
            "--binary", str(stub_esbigint),
            "--tests", "Micro/postfix-incr.js",
            "--iterations", "2",
        ])
        bad += report(
            "single --binary -ESBigInt- vs default detects reg",
            rc_esbigint == 1 and "Micro/postfix-incr.js" in err_esb,
        )

        # 10. Advisory --baseline-json warns without failing when runs pass.
        baseline_file = root / "baseline.json"
        baseline_file.write_text(json.dumps({
            "results": [
                {"test": "Kraken/stanford-crypto-ccm.js", "kind": "time", "median": 50.0, "mad": 0.0}
            ]
        }), encoding="utf-8")
        rc_adv, out_adv, _ = run_quiet([
            "--binary", str(stub_base),
            "--baseline-json", str(baseline_file),
            "--tests", "Kraken/stanford-crypto-ccm.js",
            "--iterations", "2",
        ])
        bad += report(
            "advisory --baseline-json passes on shift",
            rc_adv == 0 and "ADVISORY WARN" in out_adv,
        )

    print("check_perf_smoke self-test: %s"
          % ("ALL NEGATIVE CONTROLS BEHAVE" if bad == 0
             else "%d CONTROL(S) MISBEHAVE" % bad))
    return 1 if bad else 0


def parse_args(argv):
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--binary", default="", help="single ch binary (runs -ESBigInt- vs default A/B, or smoke/baseline)")
    ap.add_argument("--binary-a", default="", help="baseline ch binary A for interleaved A/B mode")
    ap.add_argument("--binary-b", default="", help="candidate ch binary B for interleaved A/B mode")
    ap.add_argument("--flags", action="append", default=[], help="extra ch flags for single-binary baseline/smoke mode")
    ap.add_argument("--flags-a", action="append", default=[], help="extra ch flags for A (repeatable)")
    ap.add_argument("--flags-b", action="append", default=[], help="extra ch flags for B (repeatable)")
    ap.add_argument("--label-a", default="", help="display label for configuration A")
    ap.add_argument("--label-b", default="", help="display label for configuration B")
    ap.add_argument("--baseline-json", default="", help="baseline JSON for advisory comparison")
    ap.add_argument("--strict-baseline", action="store_true",
                    help="fail rc=1 on regressions vs --baseline-json")
    ap.add_argument("--suite", action="append", default=[],
                    help="comma-separated or repeated suites (SunSpider,Kraken,Octane,JetStream,Micro)")
    ap.add_argument("--tests", "--test", dest="tests", action="append", default=[],
                    help="comma-separated or repeated test names/substrings")
    mp = ap.add_mutually_exclusive_group()
    mp.add_argument("--micro-probe", dest="micro_probe", action="store_const", const=True, default=None,
                    help="include built-in j++ / ++j micro-probes")
    mp.add_argument("--no-micro-probe", dest="micro_probe", action="store_const", const=False,
                    help="exclude built-in micro-probes from default selection")
    ap.add_argument("--iterations", type=int, default=DEFAULT_ITERATIONS,
                    help=f"measured iterations per config per test (default: {DEFAULT_ITERATIONS})")
    ap.add_argument("--warmup", type=int, default=0,
                    help="unmeasured warmup iterations per test (default: 0)")
    ap.add_argument("--timeout", type=float, default=DEFAULT_TIMEOUT,
                    help=f"per-run timeout in seconds (default: {DEFAULT_TIMEOUT:g})")
    ap.add_argument("--max-regression-pct", type=float, default=DEFAULT_MAX_REGRESSION_PCT,
                    help=f"maximum allowed regression %% (default: {DEFAULT_MAX_REGRESSION_PCT:g})")
    ap.add_argument("--tolerance", action="append", default=[],
                    help="per-test tolerance override TEST=PCT (repeatable)")
    ap.add_argument("--json-out", "-o", "--output", dest="json_out", default="",
                    help="write JSON report to this path")
    ap.add_argument("--self-test", action="store_true",
                    help="run built-in negative and positive controls")

    norm = bench_run.normalize_flag_args(list(argv), {"--flags", "--flags-a", "--flags-b"})
    return ap.parse_args(norm)


def main(argv=None, bench_dir=None, runner=None):
    args = parse_args(sys.argv[1:] if argv is None else argv)
    if args.self_test:
        return self_test()

    try:
        if args.iterations < 1:
            raise UsageError(f"--iterations must be >= 1, got {args.iterations}")
        if args.warmup < 0:
            raise UsageError(f"--warmup must be >= 0, got {args.warmup}")
        if not math.isfinite(args.timeout) or args.timeout <= 0.0:
            raise UsageError(f"--timeout must be > 0, got {args.timeout}")
        if not math.isfinite(args.max_regression_pct) or args.max_regression_pct < 0.0:
            raise UsageError(f"--max-regression-pct must be >= 0, got {args.max_regression_pct}")

        tol_map = parse_tolerances(args.tolerance)
        baseline_map = load_baseline_json(args.baseline_json) if args.baseline_json else None
        catalog = build_catalog(bench_dir=bench_dir)
        runnable, excluded = select_tests(
            catalog, suites_arg=args.suite, tests_arg=args.tests, micro_probe=args.micro_probe
        )

        has_single = bool(args.binary)
        has_pair = bool(args.binary_a) or bool(args.binary_b)
        if has_single and has_pair:
            raise UsageError("--binary cannot be combined with --binary-a / --binary-b")
        if not has_single and not has_pair:
            raise UsageError("specify --binary-a and --binary-b, or --binary, or --self-test")
        if has_pair and (not args.binary_a or not args.binary_b):
            raise UsageError("both --binary-a and --binary-b are required for A/B mode")

        flags_a = bench_run.parse_flag_values(args.flags_a)
        flags_b = bench_run.parse_flag_values(args.flags_b)
        single_flags = bench_run.parse_flag_values(args.flags)

        if has_pair:
            bin_a, dig_a = validate_binary(args.binary_a, "--binary-a")
            bin_b, dig_b = validate_binary(args.binary_b, "--binary-b")
            rc, payload = run_ab_gate(
                bin_a, dig_a, flags_a, args.label_a or "A",
                bin_b, dig_b, flags_b, args.label_b or "B",
                runnable, excluded,
                iterations=args.iterations,
                warmup=args.warmup,
                timeout=args.timeout,
                max_regression_pct=args.max_regression_pct,
                tol_map=tol_map,
                baseline_map=baseline_map,
                runner=runner,
            )
        else:
            bin_s, dig_s = validate_binary(args.binary, "--binary")
            if args.baseline_json and not flags_a and not flags_b:
                rc, payload = run_single_mode(
                    bin_s, dig_s, single_flags, runnable, excluded,
                    iterations=args.iterations,
                    warmup=args.warmup,
                    timeout=args.timeout,
                    max_regression_pct=args.max_regression_pct,
                    tol_map=tol_map,
                    baseline_map=baseline_map,
                    strict_baseline=args.strict_baseline,
                    runner=runner,
                )
            elif flags_a or flags_b:
                rc, payload = run_ab_gate(
                    bin_s, dig_s, flags_a, args.label_a or "A",
                    bin_s, dig_s, flags_b, args.label_b or "B",
                    runnable, excluded,
                    iterations=args.iterations,
                    warmup=args.warmup,
                    timeout=args.timeout,
                    max_regression_pct=args.max_regression_pct,
                    tol_map=tol_map,
                    baseline_map=baseline_map,
                    runner=runner,
                )
            elif flaglist_supported(bin_s, timeout=min(args.timeout, 10.0), runner=runner):
                rc, payload = run_ab_gate(
                    bin_s, dig_s, ["-ESBigInt-"], args.label_a or "ESBigInt-off",
                    bin_s, dig_s, [], args.label_b or "default",
                    runnable, excluded,
                    iterations=args.iterations,
                    warmup=args.warmup,
                    timeout=args.timeout,
                    max_regression_pct=args.max_regression_pct,
                    tol_map=tol_map,
                    baseline_map=baseline_map,
                    runner=runner,
                )
            else:
                rc, payload = run_single_mode(
                    bin_s, dig_s, single_flags, runnable, excluded,
                    iterations=args.iterations,
                    warmup=args.warmup,
                    timeout=args.timeout,
                    max_regression_pct=args.max_regression_pct,
                    tol_map=tol_map,
                    baseline_map=None,
                    strict_baseline=False,
                    note="Release build (flaglist disabled); ran default smoke invariants without -ESBigInt- A/B",
                    runner=runner,
                )
    except UsageError as e:
        print(f"check_perf_smoke: error: {e}", file=sys.stderr)
        return 2

    if args.json_out:
        out_path = Path(args.json_out)
        out_path.parent.mkdir(parents=True, exist_ok=True)
        out_path.write_text(
            json.dumps(payload, indent=1, allow_nan=False) + "\n",
            encoding="utf-8",
        )
        print(f"check_perf_smoke: wrote {out_path}")
    return rc


if __name__ == "__main__":
    raise SystemExit(main())
