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

# Recorded Linux baselines: none yet (see module docstring). A (passed,
# failed) pair per variant goes here once observed on an
# execution-capable host and validated; until then completed runs exit 1
# with observed counts.
RECORDED_BASELINE = {"interpreted": None, "dynapogo": None}


def grade_variant(variant, rc, lines):
    """Grade one single-variant run. Returns (verdict, detail)."""
    try:
        parsed = parse_total(lines)
    except ValueError as e:
        return ("FAIL", "%s" % e)
    if parsed is None:
        # No summary: either the run aborted early (engine init abort
        # under qemu shows up here) or the harness errored. Report what
        # the log shows; grade BLOCKED only on qemu's `uncaught target
        # signal` line (stderr-merged into the log). A bare abort rc,
        # or the shell's "Aborted" echo without qemu's own line, is
        # FAIL: the death wasn't observed as a qemu guest abort.
        tail = "".join(lines[-15:]) if lines else ""
        if "uncaught target signal" in tail:
            return ("BLOCKED",
                    "no Total summary; engine init abort under qemu "
                    "(doc/b4-linux.md section 8); 0 ran, not 0/N")
        return ("FAIL", "no 'Total: passed N, failed M' summary "
                        "line (rc=%d)" % rc)
    passed, failed = parsed
    if passed == 0 and failed > 0:
        # A Total summary exists but nothing ran: every failure is
        # qemu-abort-signed (the §8.2 init abort prints one `uncaught
        # target signal` line per test; the qemu line itself contains
        # `(Aborted)`). The >= is self-checking: a genuine
        # failure without the signature falls through to FAIL below.
        aborts = sum(1 for line in lines if "uncaught target signal" in line)
        if aborts >= failed:
            return ("BLOCKED",
                    "passed 0 / failed %d, every failure abort-signed "
                    "(engine init abort under qemu; doc/b4-linux.md "
                    "section 8); 0 ran, not 0/N" % failed)
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
    # No-baseline pin: with RECORDED_BASELINE all-None the gate
    # cannot pass.
    bad += report("no baseline recorded (gate cannot pass yet)",
                  all(v is None for v in RECORDED_BASELINE.values()))
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
    print("check_regression_linux self-test: %s"
          % ("ALL NEGATIVE CONTROLS BEHAVE" if bad == 0
             else "%d CONTROL(S) MISBEHAVE" % bad))
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
