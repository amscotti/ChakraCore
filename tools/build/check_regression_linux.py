#!/usr/bin/env python3
#-------------------------------------------------------------------------------------------------------
# Copyright (c) ChakraCore Project Contributors. All rights reserved.
# Licensed under the MIT license. See LICENSE.txt file in the project root for full license information.
#-------------------------------------------------------------------------------------------------------
"""B4.3 check gate (2): Linux regression suite, legacy-exact selection.

Runs the SAME selection the legacy `ninja check` runs for its regression
leg (build.ninja CUSTOM_COMMAND from test/CMakeLists.txt), once per
variant:

    runtests.py --test --binary=<ch> --variants=interpreted
    runtests.py --test --binary=<ch> --variants=dynapogo

The legacy command passes no --variants (runtests.py then runs both and
prints two Total summaries); the gate splits them so each variant's
`Total: passed N, failed M` line (runtests.py:567) parses and grades
separately, reusing the single-summary parse_total (same shape as Mac's
--variants=disable_jit run). --variants in extras is rejected: it would
collide with the loop's own flag. Other extra argv after <ch> passes
through to runtests.py, so bounded subsets stay expressible when the
full suite is too slow emulated.

The verdict per variant comes from parsing its real Total summary, not
from the exit code alone; a nonzero exit, a missing summary, or 2+
summary lines fails loudly. Overall: PASS iff every variant passes;
any FAIL verdict fails the gate; otherwise BLOCKED.

Linux has NO recorded baseline counts yet (the suite has never run
green here: doc/b4-linux.md section 8). Until counts are recorded in
RECORDED_BASELINE below, a completed run prints its OBSERVED counts
and exits 1 asking for them to be recorded after validation — a
completed run never silently passes, and an aborted run (engine init
abort under qemu) reports BLOCKED with 0 ran.

Usage: check_regression_linux.py <runtests.py> <ch> [extra runtests args...]
       [--self-test]
Exit code: 0 iff every variant ran green with exactly its recorded counts.
"""

import os
import subprocess
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import common
from check_regression import parse_total

# Variants the gate runs: the legacy default pair, split into two
# single-variant invocations (see module docstring).
VARIANTS = ("interpreted", "dynapogo")

# Recorded Linux baselines. Observed on the native x64 proof host
# (4 vCPU, glibc 2.39, Zig 0.17.0/clang 22) on 2026-11-09 after the Linux
# engine fixes recorded in doc/b4-linux.md: interpreted 2841/0 and
# dynapogo 2305/0. Counts differ from Mac (2051/0 and 2103/0) because the
# Linux selection runs the x64-eligible subset (include_slow off, platform
# exclusions); the two counts are the authoritative Linux baseline.
RECORDED_BASELINE = {"interpreted": (2841, 0), "dynapogo": (2305, 0)}


def grade_variant(variant, rc, lines):
    """Grade one single-variant run. Returns (verdict, detail)."""
    try:
        parsed = parse_total(lines)
    except ValueError as e:
        return ("FAIL", "%s" % e)
    if parsed is None:
        # No summary: either the run aborted early (engine init abort
        # under qemu shows up here) or the harness errored. Report what
        # the log shows; grade BLOCKED only on a pure qemu signal-6
        # observation over the whole log (stderr-merged): at least
        # one `uncaught target signal 6 (` occurrence and zero
        # occurrences of any other qemu-phrased signal number, with
        # a nonzero rc — an observed qemu guest SIGABRT, aligned with
        # common.is_engine_init_abort (occurrence-based, so a mixed
        # signal-6 + signal-11 log vetos BLOCKED on separate lines
        # or on one line carrying both). Any other qemu-phrased
        # signal occurrence, rc == 0, a bare abort rc, or the shell's
        # "Aborted" echo without a qemu signal-6 occurrence is FAIL:
        # the death wasn't observed as a pure qemu guest SIGABRT.
        signals = common.qemu_target_signals(lines)
        if rc != 0 and signals and all(n == 6 for n in signals):
            return ("BLOCKED",
                    "no Total summary; engine init abort under qemu "
                    "(doc/b4-linux.md section 8); 0 ran, not 0/N")
        return ("FAIL", "no 'Total: passed N, failed M' summary "
                        "line (rc=%d)" % rc)
    passed, failed = parsed
    if passed == 0 and failed > 0:
        # A Total summary exists but nothing ran: every failure is
        # qemu-abort-signed (the §8.2 init abort prints one
        # `uncaught target signal 6 (` occurrence per test). Only
        # signal-6 occurrences count, and any other qemu-phrased
        # signal occurrence vetos BLOCKED — per occurrence, not per
        # line, so one line carrying both 6 and 11 vetos and one
        # line carrying two 6s counts two; a qemu signal-11 death
        # or a bare "Aborted" with no signal-6 occurrence is a
        # genuine failure. BLOCKED also requires rc != 0, aligned
        # with common.is_engine_init_abort. Residual limitations:
        # the >= counts signal-6 occurrences, not tests, so
        # duplicate echoes of a signal-6 occurrence (e.g. a
        # shell/tee repeat) could still inflate the count and mask
        # a genuinely unsigned failure; and a death with no qemu
        # signal line at all (e.g. a bare "Segmentation fault
        # (core dumped)") carries no occurrence to veto or count,
        # so it is not detectable by counting. The veto covers
        # qemu-phrased other-signal occurrences only; duplicate
        # signal-6 echoes and non-qemu-phrased deaths remain the
        # residual limitations. This BLOCKED case is evaluated
        # before the generic nonzero-rc FAIL: the harness exits
        # nonzero precisely because every test died.
        signals = common.qemu_target_signals(lines)
        aborts = signals.count(6)
        vetoed = any(n != 6 for n in signals)
        if rc != 0 and not vetoed and aborts >= failed:
            return ("BLOCKED",
                    "passed 0 / failed %d, every failure abort-signed "
                    "(engine init abort under qemu; doc/b4-linux.md "
                    "section 8); 0 ran, not 0/N" % failed)
    if rc != 0:
        # A Total parsed, but the harness itself exited nonzero: the
        # module contract fails loudly on nonzero exit regardless of
        # counts — PASS requires rc == 0.
        return ("FAIL",
                "runtests.py exited rc=%d (passed %d / failed %d); "
                "a nonzero exit fails loudly regardless of counts"
                % (rc, passed, failed))
    base = RECORDED_BASELINE[variant]
    if base is None:
        return ("FAIL",
                "completed run (passed %d / failed %d) but no Linux "
                "baseline is recorded in check_regression_linux.py "
                "(doc/b4-linux.md section 8); validate on an "
                "execution-capable host, then record it"
                % (passed, failed))
    if (passed, failed) != base:
        return ("FAIL",
                "count mismatch: got passed %d / failed %d, require "
                "passed %d / failed %d"
                % (passed, failed, base[0], base[1]))
    return ("PASS", "passed %d / failed %d match the recorded baseline"
                    % (passed, failed))


def main(argv):
    args = list(argv[1:])
    if "--self-test" in args:
        return self_test()
    if len(args) < 2:
        print("usage: check_regression_linux.py <runtests.py> <ch> "
              "[extra runtests args...]", file=sys.stderr)
        return 2
    runtests, ch, extra = args[0], args[1], args[2:]
    if any(a == "--variants" or a.startswith("--variants=") for a in extra):
        print("check_regression_linux: FAIL: --variants in extras "
              "collides with the per-variant loop; run one variant via a "
              "manual runtests.py invocation instead", file=sys.stderr)
        return 2
    # The legacy 60s default assumes the much faster reference host. On the
    # Linux x64 proof box (4 vCPUs), Function/FuncBody.bug232281.js completes
    # correctly in ~4m27s (it stack-overflows through an eval-per-frame
    # recursion), so a 60s deadline reports a false timeout. Keep the
    # selection identical and widen only the deadline; callers can override.
    if not any(a == "--timeout" or a.startswith("--timeout=") for a in extra):
        extra = list(extra) + ["--timeout", "480"]
    for p in (runtests, ch):
        if not os.path.isfile(p):
            print("check_regression_linux: missing file: %s" % p,
                  file=sys.stderr)
            return 2
    verdicts = {}
    for variant in VARIANTS:
        cmd = [sys.executable, runtests, "--test", "--binary=" + ch,
               "--variants=" + variant] + extra
        print("check_regression_linux: running: %s" % " ".join(cmd))
        try:
            rc, lines = common.run_tee(cmd, timeout=common.REGRESSION_TIMEOUT)
        except subprocess.TimeoutExpired:
            verdicts[variant] = (
                "FAIL", "suite exceeded the %ds ceiling"
                        % common.REGRESSION_TIMEOUT)
            continue
        print("check_regression_linux: runtests.py [%s] exited %d"
              % (variant, rc))
        verdict, detail = grade_variant(variant, rc, lines)
        if verdict == "PASS":
            print("check_regression_linux: observed [%s] %s"
                  % (variant, detail))
        verdicts[variant] = (verdict, detail)
    for variant in VARIANTS:
        verdict, detail = verdicts[variant]
        stream = sys.stdout if verdict == "PASS" else sys.stderr
        print("check_regression_linux: [%s] %s: %s"
              % (variant, verdict, detail), file=stream)
    if all(v == "PASS" for v, _ in verdicts.values()):
        print("check_regression_linux: PASS: every variant matches its "
              "recorded baseline")
        return 0
    overall = ("FAIL" if any(v == "FAIL" for v, _ in verdicts.values())
               else "BLOCKED")
    print("check_regression_linux: %s:" % overall, file=sys.stderr)
    return 1


def self_test():
    """Negative controls: parser/verdict behavior on doctored logs."""
    bad = 0

    def report(name, cond):
        print("self-test %-40s -> %s"
              % (name, "PASS" if cond else "FAIL"))
        return 0 if cond else 1

    cases = [
        ("single summary parses",
         ["Total: passed 10, failed 0"], (10, 0)),
        ("missing summary is None",
         ["############ Results ###########"], None),
        ("lookalike rejected",
         ["XTotal: passed 10, failed 0"], None),
    ]
    for name, lines, want in cases:
        try:
            got = parse_total(lines)
        except ValueError:
            got = "ValueError"
        bad += report(name, got == want)
    try:
        parse_total(["Total: passed 10, failed 0",
                     "Total: passed 10, failed 0"])
        bad += report("duplicate summary raises", False)
    except ValueError:
        bad += report("duplicate summary raises", True)
    # No-baseline pin: with RECORDED_BASELINE all-None the gate cannot
    # pass. Temporarily clear the recorded baseline, assert the old
    # behavior, then restore the real pins.
    saved_for_none = RECORDED_BASELINE
    try:
        globals()["RECORDED_BASELINE"] = {"interpreted": None, "dynapogo": None}
        verdict, detail = grade_variant(
            "interpreted", 0, ["Total: passed 10, failed 0"])
        bad += report("no baseline recorded (gate cannot pass yet)",
                      verdict == "FAIL" and "no Linux baseline" in detail)
    finally:
        globals()["RECORDED_BASELINE"] = saved_for_none
    # Variants-extras guard: --variants in extras is rejected before
    # any file check or suite launch (existing files prove the guard,
    # not the isfile check, fires).
    here = os.path.dirname(os.path.abspath(__file__))
    real_rt = os.path.join(here, "..", "..", "test", "runtests.py")
    bad += report("--variants in extras rejected",
                  main(["check_regression_linux.py", real_rt, __file__,
                        "--variants=disable_jit"]) == 2)
    abort2 = ["qemu: uncaught target signal 6 (Aborted) - core dumped"] * 2
    bad += report("all-abort grades BLOCKED",
                  grade_variant("interpreted", 1,
                                ["Total: passed 0, failed 2"] + abort2)[0]
                  == "BLOCKED")
    bad += report("genuine all-fail grades FAIL",
                  grade_variant("interpreted", 1,
                                ["Total: passed 0, failed 2"])[0]
                  == "FAIL")
    bad += report("mixed pass+abort grades FAIL",
                  grade_variant("interpreted", 1,
                                ["Total: passed 1, failed 1",
                                 abort2[0]])[0]
                  == "FAIL")
    bad += report("summary-less qemu abort grades BLOCKED",
                  grade_variant("interpreted", 134,
                                ["some harness output",
                                 abort2[0]])[0]
                  == "BLOCKED")
    bad += report("summary-less bare abort rc grades FAIL",
                  grade_variant("interpreted", 134,
                                ["some harness output"])[0]
                  == "FAIL")
    bad += report("summary-less shell-Aborted-only grades FAIL",
                  grade_variant("interpreted", 134,
                                ["Aborted (core dumped)"])[0]
                  == "FAIL")
    bad += report("all-fail shell-Aborted-only grades FAIL",
                  grade_variant("interpreted", 1,
                                ["Total: passed 0, failed 2",
                                 "Aborted", "Aborted"])[0]
                  == "FAIL")
    # Baseline-fed controls: temporarily monkeypatch RECORDED_BASELINE
    # with a synthetic recorded pair, then restore it (the all-None
    # pin above must still hold afterwards).
    segv2 = ["qemu: uncaught target signal 11 (Segmentation fault) - "
             "core dumped"] * 2
    saved_baseline = RECORDED_BASELINE
    try:
        globals()["RECORDED_BASELINE"] = {
            "interpreted": (10, 0), "dynapogo": (10, 0)}
        bad += report("recorded baseline + rc=0 grades PASS",
                      grade_variant("interpreted", 0,
                                    ["Total: passed 10, failed 0"])[0]
                      == "PASS")
        verdict, detail = grade_variant("interpreted", 139,
                                        ["Total: passed 10, failed 0"])
        bad += report("matching counts + rc=139 grades FAIL naming rc",
                      verdict == "FAIL" and "139" in detail)
        bad += report("all-signal-11 0/N grades FAIL",
                      grade_variant("interpreted", 139,
                                    ["Total: passed 0, failed 2"]
                                    + segv2)[0]
                      == "FAIL")
        bad += report("summary-less signal-11 grades FAIL",
                      grade_variant("interpreted", 139,
                                    ["some harness output", segv2[0]])[0]
                      == "FAIL")
        sig61 = ("qemu: uncaught target signal 61 (Real-time event 61) "
                 "- core dumped")
        bad += report("all-signal-61 0/N grades FAIL",
                      grade_variant("interpreted", 134,
                                    ["Total: passed 0, failed 2",
                                     sig61, sig61])[0]
                      == "FAIL")
        bad += report("summary-less signal-61 grades FAIL",
                      grade_variant("interpreted", 134,
                                    ["some harness output", sig61])[0]
                      == "FAIL")
        bad += report("mixed sig6+sig11 0/N grades FAIL (veto)",
                      grade_variant("interpreted", 1,
                                    ["Total: passed 0, failed 2",
                                     abort2[0], segv2[0], abort2[0]])[0]
                      == "FAIL")
        mixed_line = abort2[0] + "; " + segv2[0]
        bad += report("mixed sig6+sig11 same-line 0/N grades FAIL (veto)",
                      grade_variant("interpreted", 1,
                                    ["Total: passed 0, failed 2",
                                     mixed_line, abort2[0]])[0]
                      == "FAIL")
        bad += report("two sig6 on one line 0/2 grades BLOCKED",
                      grade_variant("interpreted", 1,
                                    ["Total: passed 0, failed 2",
                                     abort2[0] + "; " + abort2[0]])[0]
                      == "BLOCKED")
        bad += report("summary-less mixed sig6+sig11 grades FAIL (veto)",
                      grade_variant("interpreted", 134,
                                    ["some harness output", segv2[0],
                                     abort2[0]])[0]
                      == "FAIL")
        bad += report("summary-less mixed same-line grades FAIL (veto)",
                      grade_variant("interpreted", 134,
                                    ["some harness output", mixed_line])[0]
                      == "FAIL")
        bad += report("all-signal-6 0/N at rc=0 grades FAIL not BLOCKED",
                      grade_variant("interpreted", 0,
                                    ["Total: passed 0, failed 2"]
                                    + abort2)[0]
                      == "FAIL")
        bad += report("summary-less signal-6 at rc=0 grades FAIL",
                      grade_variant("interpreted", 0,
                                    ["some harness output", abort2[0]])[0]
                      == "FAIL")
    finally:
        globals()["RECORDED_BASELINE"] = saved_baseline
    bad += report("baseline restored after controls",
                  RECORDED_BASELINE == saved_baseline)
    print("check_regression_linux self-test: %s"
          % ("ALL NEGATIVE CONTROLS BEHAVE" if bad == 0
             else "%d CONTROL(S) MISBEHAVE" % bad))
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
