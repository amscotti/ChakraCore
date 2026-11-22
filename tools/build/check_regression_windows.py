#!/usr/bin/env python3
#-------------------------------------------------------------------------------------------------------
# Copyright (c) ChakraCore Project Contributors. All rights reserved.
# Licensed under the MIT license. See LICENSE.txt file in the project root for full license information.
#-------------------------------------------------------------------------------------------------------
"""B6 Windows x64 regression suite gate (mirrors check_regression_linux.py).

Runs test/runtests.py natively on Windows x64 with explicit `--x64`,
`--test` or `--debug` (`--select`), optional `--not-tag exclude_noicu`
(`--no-icu`), `--binary=<abs_ch>`, and per-variant `--variants=<v>`:

  - JIT enabled (default):  interpreted, dynapogo
  - --no-jit (-Djit=false): disable_jit

Every variant's `Total: passed N, failed M` summary line is parsed and
graded separately via check_regression.parse_total:

  - Always fails loudly (exit 1) on timeout, rc != 0, missing/duplicate
    Total summary line, failed != 0, or passed == 0.
  - When RECORDED_BASELINE[(select, variant)] is None (prior to B6
    Windows x64 capture in doc/b6-windows-run.md):
      * default: fails loudly (`UNPINNED: ...`)
      * --allow-unpinned: PASSes iff rc == 0, failed == 0, and passed > 0,
        printing the exact pin tuple to record in RECORDED_BASELINE.
  - Once pinned (base = (bp, bf)):
      * default: requires (passed, failed) == base
      * --allow-higher: requires passed >= bp, failed == bf, and failed == 0.

Usage:
  check_regression_windows.py <runtests.py> <ch.exe>
      [--select test|debug] [--no-jit] [--no-icu]
      [--allow-unpinned] [--allow-higher] [extra runtests args...]
  check_regression_windows.py --self-test

Exit code: 0 iff every selected variant passes; 2 on bad CLI/missing file;
1 on regression/gate failure.
"""

import glob
import os
import subprocess
import sys
import tempfile
from unittest import mock

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import common
from check_regression import parse_total

SELECT_FLAGS = {"test": "--test", "debug": "--debug"}
JIT_VARIANTS = ("interpreted", "dynapogo")
NOJIT_VARIANTS = ("disable_jit",)

# Pinned Windows x64 baselines keyed by (select, variant). Initialized to
# None until first-run B6 certification captures native counts in
# doc/b6-windows-run.md.
RECORDED_BASELINE = {
    ("test", "interpreted"): None,
    ("test", "dynapogo"): None,
    ("test", "disable_jit"): None,
    ("debug", "interpreted"): None,
    ("debug", "dynapogo"): None,
    ("debug", "disable_jit"): None,
}


def variants_for_jit(no_jit):
    """Return tuple of runtests.py variants for the JIT configuration."""
    return NOJIT_VARIANTS if no_jit else JIT_VARIANTS


def build_runtests_cmd(runtests, ch, variant, *, select="test", no_icu=False,
                       extra=()):
    """Build the exact runtests.py argv for one Windows x64 variant."""
    if select not in SELECT_FLAGS:
        raise ValueError("bad select %r" % (select,))
    cmd = [
        sys.executable,
        runtests,
        "--x64",
        SELECT_FLAGS[select],
    ]
    if no_icu:
        cmd.extend(["--not-tag", "exclude_noicu"])
    cmd.append("--binary=" + os.path.abspath(ch))
    cmd.append("--variants=" + variant)
    cmd.extend(extra)
    return cmd


def cleanup_profile_dpl(runtests):
    """Remove any leftover profile.dpl.* files under test/ defensively."""
    test_root = os.path.dirname(os.path.abspath(runtests))
    removed = []
    for path in glob.glob(os.path.join(test_root, "*", "profile.dpl.*")):
        try:
            os.remove(path)
            print("check_regression_windows: removed leftover %s" % path)
            removed.append(path)
        except OSError as e:
            print("check_regression_windows: WARNING: cannot remove %s: %s"
                  % (path, e))
    return removed


def grade_variant(select, variant, rc, lines, *, allow_unpinned=False,
                  allow_higher=False):
    """Grade one single-variant run. Returns (verdict, detail)."""
    try:
        parsed = parse_total(lines)
    except ValueError as e:
        return ("FAIL", "%s" % e)
    if parsed is None:
        return ("FAIL", "no 'Total: passed N, failed M' summary line (rc=%d)" % rc)

    passed, failed = parsed
    if rc != 0:
        return ("FAIL",
                "runtests.py exited rc=%d (passed %d / failed %d); "
                "a nonzero exit fails loudly regardless of counts"
                % (rc, passed, failed))
    if failed != 0:
        return ("FAIL",
                "nonzero test failures: passed %d / failed %d"
                % (passed, failed))
    if passed == 0:
        return ("FAIL",
                "zero tests passed: passed %d / failed %d"
                % (passed, failed))

    key = (select, variant)
    if key not in RECORDED_BASELINE:
        return ("FAIL", "unknown baseline key %r" % (key,))
    base = RECORDED_BASELINE[key]

    if base is None:
        pin_expr = "RECORDED_BASELINE[(%r, %r)] = (%d, %d)" % (
            select, variant, passed, failed)
        if not allow_unpinned:
            return ("FAIL",
                    "UNPINNED: %r observed passed %d / failed %d with no "
                    "recorded Windows baseline; run with --allow-unpinned "
                    "(or -Dallow-unpinned-regression=true) for B6 bootstrap, "
                    "then pin %s"
                    % (key, passed, failed, pin_expr))
        return ("PASS",
                "unpinned %r passed %d / failed %d (pin: %s)"
                % (key, passed, failed, pin_expr))

    bp, bf = base
    if allow_higher:
        if failed != bf or passed < bp:
            return ("FAIL",
                    "count regression for %r: got passed %d / failed %d, "
                    "require passed >= %d / failed %d (--allow-higher)"
                    % (key, passed, failed, bp, bf))
        return ("PASS",
                "passed %d / failed %d satisfy baseline >= %d / %d "
                "(--allow-higher)" % (passed, failed, bp, bf))

    if (passed, failed) != base:
        return ("FAIL",
                "count mismatch for %r: got passed %d / failed %d, "
                "require passed %d / failed %d"
                % (key, passed, failed, bp, bf))
    return ("PASS",
            "passed %d / failed %d match the recorded baseline"
            % (passed, failed))


def parse_args(args):
    """Parse CLI arguments, preserving unknown extra args for runtests.py."""
    select = "test"
    no_jit = False
    no_icu = False
    allow_unpinned = False
    allow_higher = False
    positional_and_extra = []

    i = 0
    while i < len(args):
        a = args[i]
        if a == "--select":
            if i + 1 >= len(args):
                return None, "--select needs a value (test or debug)"
            select = args[i + 1]
            i += 2
        elif a.startswith("--select="):
            select = a.split("=", 1)[1]
            i += 1
        elif a == "--no-jit":
            no_jit = True
            i += 1
        elif a == "--no-icu":
            no_icu = True
            i += 1
        elif a == "--allow-unpinned":
            allow_unpinned = True
            i += 1
        elif a == "--allow-higher":
            allow_higher = True
            i += 1
        else:
            positional_and_extra.append(a)
            i += 1

    if select not in SELECT_FLAGS:
        return None, ("bad --select %r: expected one of: %s"
                      % (select, ", ".join(sorted(SELECT_FLAGS))))
    if len(positional_and_extra) < 2:
        return None, (
            "usage: check_regression_windows.py <runtests.py> <ch.exe> "
            "[--select test|debug] [--no-jit] [--no-icu] "
            "[--allow-unpinned] [--allow-higher] [extra runtests args...] "
            "[--self-test]"
        )
    runtests = positional_and_extra[0]
    ch = positional_and_extra[1]
    extra = positional_and_extra[2:]
    if any(a == "--variants" or a.startswith("--variants=") for a in extra):
        return None, (
            "check_regression_windows: FAIL: --variants in extras collides "
            "with the per-variant loop; run a manual runtests.py invocation "
            "instead"
        )
    return {
        "runtests": runtests,
        "ch": ch,
        "select": select,
        "no_jit": no_jit,
        "no_icu": no_icu,
        "allow_unpinned": allow_unpinned,
        "allow_higher": allow_higher,
        "extra": extra,
    }, None


def self_test():
    """Positive and negative controls for command construction and grading."""
    bad = 0

    def report(name, cond, detail=""):
        print("self-test %-48s -> %s%s"
              % (name, "PASS" if cond else "FAIL",
                 (" (" + detail + ")") if detail else ""))
        return 0 if cond else 1

    # 1. Command construction and CLI validation.
    with tempfile.TemporaryDirectory(prefix="check-reg-win-selftest-") as tmp:
        fake_rt = os.path.join(tmp, "test", "runtests.py")
        fake_ch = os.path.join(tmp, "zig-out", "bin", "ch.exe")
        os.makedirs(os.path.dirname(fake_rt), exist_ok=True)
        os.makedirs(os.path.dirname(fake_ch), exist_ok=True)
        with open(fake_rt, "w", encoding="utf-8") as f:
            f.write("# stub\n")
        with open(fake_ch, "w", encoding="utf-8") as f:
            f.write("MZ")

        opts, err = parse_args([fake_rt, fake_ch])
        bad += report("parse_args defaults (test, jit, icu, pinned)",
                      err is None
                      and opts["select"] == "test"
                      and not opts["no_jit"]
                      and not opts["no_icu"]
                      and not opts["allow_unpinned"]
                      and not opts["allow_higher"])

        bad += report("--variants in extras rejected (exit 2)",
                      main(["check_regression_windows.py", fake_rt, fake_ch,
                            "--variants=disable_jit"]) == 2)
        bad += report("bad --select rejected (exit 2)",
                      main(["check_regression_windows.py", fake_rt, fake_ch,
                            "--select", "release"]) == 2)

        cmd_test = build_runtests_cmd(
            fake_rt, fake_ch, "interpreted", select="test", no_icu=False)
        bad += report("test cmd passes --x64 --test --binary --variants",
                      "--x64" in cmd_test
                      and "--test" in cmd_test
                      and "--debug" not in cmd_test
                      and "--not-tag" not in cmd_test
                      and ("--binary=" + os.path.abspath(fake_ch)) in cmd_test
                      and "--variants=interpreted" in cmd_test)

        cmd_dbg_noicu = build_runtests_cmd(
            fake_rt, fake_ch, "disable_jit", select="debug", no_icu=True,
            extra=["Basics"])
        bad += report("debug+--no-icu cmd passes --x64 --debug --not-tag exclude_noicu",
                      "--x64" in cmd_dbg_noicu
                      and "--debug" in cmd_dbg_noicu
                      and "--test" not in cmd_dbg_noicu
                      and cmd_dbg_noicu[cmd_dbg_noicu.index("--not-tag") + 1] == "exclude_noicu"
                      and "--variants=disable_jit" in cmd_dbg_noicu
                      and cmd_dbg_noicu[-1] == "Basics")

        bad += report("variants_for_jit splits JIT vs --no-jit",
                      variants_for_jit(False) == ("interpreted", "dynapogo")
                      and variants_for_jit(True) == ("disable_jit",))

        # 2. Unpinned baseline controls.
        saved = dict(RECORDED_BASELINE)
        try:
            for k in RECORDED_BASELINE:
                RECORDED_BASELINE[k] = None

            v, d = grade_variant("test", "interpreted", 0,
                                 ["Total: passed 1234, failed 0"],
                                 allow_unpinned=False)
            bad += report("unpinned without --allow-unpinned rejects",
                          v == "FAIL" and d.startswith("UNPINNED:"), d)

            v, d = grade_variant("test", "interpreted", 0,
                                 ["Total: passed 1234, failed 0"],
                                 allow_unpinned=True)
            bad += report("unpinned + --allow-unpinned accepts green run",
                          v == "PASS"
                          and "RECORDED_BASELINE[('test', 'interpreted')] = (1234, 0)" in d,
                          d)

            v, _ = grade_variant("test", "interpreted", 0,
                                 ["Total: passed 1234, failed 1"],
                                 allow_unpinned=True)
            bad += report("unpinned + --allow-unpinned rejects failed > 0",
                          v == "FAIL")

            v, _ = grade_variant("test", "interpreted", 0,
                                 ["Total: passed 0, failed 0"],
                                 allow_unpinned=True)
            bad += report("unpinned + --allow-unpinned rejects passed == 0",
                          v == "FAIL")

            v, _ = grade_variant("test", "interpreted", 1,
                                 ["Total: passed 1234, failed 0"],
                                 allow_unpinned=True)
            bad += report("unpinned + --allow-unpinned rejects rc != 0",
                          v == "FAIL")

            # End-to-end main() with unpinned mocked runtests + profile.dpl cleanup.
            dpl_dir = os.path.join(os.path.dirname(fake_rt), "Basics")
            os.makedirs(dpl_dir, exist_ok=True)
            dpl_file = os.path.join(dpl_dir, "profile.dpl.1")
            with open(dpl_file, "w", encoding="utf-8") as f:
                f.write("dpl")

            with mock.patch.object(
                common, "run_tee",
                return_value=(0, ["Total: passed 500, failed 0\n"]),
            ):
                rc_unpinned = main(["check_regression_windows.py",
                                    fake_rt, fake_ch])
                bad += report("main() fails on unpinned without --allow-unpinned",
                              rc_unpinned == 1 and not os.path.exists(dpl_file))

                rc_boot = main(["check_regression_windows.py",
                                fake_rt, fake_ch, "--allow-unpinned"])
                bad += report("main() passes on unpinned with --allow-unpinned",
                              rc_boot == 0)

            with mock.patch.object(
                common, "run_tee",
                side_effect=subprocess.TimeoutExpired(["runtests.py"], 10),
            ):
                rc_to = main(["check_regression_windows.py",
                              fake_rt, fake_ch, "--allow-unpinned", "--no-jit"])
                bad += report("main() fails loudly on TimeoutExpired",
                              rc_to == 1)

            # 3. Pinned baseline controls.
            RECORDED_BASELINE[("test", "interpreted")] = (100, 0)
            RECORDED_BASELINE[("test", "dynapogo")] = (90, 0)

            bad += report("pinned exact match passes",
                          grade_variant("test", "interpreted", 0,
                                        ["Total: passed 100, failed 0"])[0] == "PASS")
            bad += report("pinned lower passed fails",
                          grade_variant("test", "interpreted", 0,
                                        ["Total: passed 99, failed 0"])[0] == "FAIL")
            bad += report("pinned higher passed without --allow-higher fails",
                          grade_variant("test", "interpreted", 0,
                                        ["Total: passed 101, failed 0"])[0] == "FAIL")
            bad += report("pinned nonzero failed fails even if matched",
                          grade_variant("test", "interpreted", 0,
                                        ["Total: passed 100, failed 1"])[0] == "FAIL")
            bad += report("pinned + --allow-higher accepts passed >= bp",
                          grade_variant("test", "interpreted", 0,
                                        ["Total: passed 105, failed 0"],
                                        allow_higher=True)[0] == "PASS")
            bad += report("pinned + --allow-higher rejects passed < bp",
                          grade_variant("test", "interpreted", 0,
                                        ["Total: passed 99, failed 0"],
                                        allow_higher=True)[0] == "FAIL")
            bad += report("pinned + --allow-higher rejects failed != 0",
                          grade_variant("test", "interpreted", 0,
                                        ["Total: passed 105, failed 1"],
                                        allow_higher=True)[0] == "FAIL")
            bad += report("pinned + --allow-higher rejects rc != 0",
                          grade_variant("test", "interpreted", 2,
                                        ["Total: passed 105, failed 0"],
                                        allow_higher=True)[0] == "FAIL")

            # 4. Missing / duplicate Total controls.
            bad += report("missing Total summary fails",
                          grade_variant("test", "interpreted", 0,
                                        ["############ Results ###########"])[0] == "FAIL")
            bad += report("duplicate Total summary fails",
                          grade_variant("test", "interpreted", 0,
                                        ["Total: passed 100, failed 0",
                                         "Total: passed 100, failed 0"])[0] == "FAIL")
        finally:
            RECORDED_BASELINE.clear()
            RECORDED_BASELINE.update(saved)

        bad += report("RECORDED_BASELINE restored after self-test",
                      RECORDED_BASELINE == saved)

    print("check_regression_windows self-test: %s"
          % ("ALL NEGATIVE CONTROLS BEHAVE" if bad == 0
             else "%d CONTROL(S) MISBEHAVE" % bad))
    return 1 if bad else 0


def main(argv):
    args = list(argv[1:])
    if "--self-test" in args:
        return self_test()

    opts, err = parse_args(args)
    if err is not None:
        print(err, file=sys.stderr)
        return 2

    runtests = opts["runtests"]
    ch = opts["ch"]
    for p in (runtests, ch):
        if not os.path.isfile(p):
            print("check_regression_windows: missing file: %s" % p,
                  file=sys.stderr)
            return 2

    variants = variants_for_jit(opts["no_jit"])
    verdicts = {}
    for variant in variants:
        cmd = build_runtests_cmd(
            runtests, ch, variant,
            select=opts["select"],
            no_icu=opts["no_icu"],
            extra=opts["extra"],
        )
        print("check_regression_windows: running: %s" % " ".join(cmd))
        sys.stdout.flush()
        try:
            rc, lines = common.run_tee(cmd, timeout=common.REGRESSION_TIMEOUT)
        except subprocess.TimeoutExpired:
            print("check_regression_windows: FAIL: [%s] exceeded %ds ceiling"
                  % (variant, common.REGRESSION_TIMEOUT), file=sys.stderr)
            verdicts[variant] = (
                "FAIL",
                "suite exceeded the %ds ceiling" % common.REGRESSION_TIMEOUT,
            )
            continue
        finally:
            cleanup_profile_dpl(runtests)

        print("check_regression_windows: runtests.py [%s] exited %d"
              % (variant, rc))
        verdict, detail = grade_variant(
            opts["select"], variant, rc, lines,
            allow_unpinned=opts["allow_unpinned"],
            allow_higher=opts["allow_higher"],
        )
        if verdict == "PASS":
            print("check_regression_windows: observed [%s] %s"
                  % (variant, detail))
        verdicts[variant] = (verdict, detail)

    for variant in variants:
        verdict, detail = verdicts[variant]
        stream = sys.stdout if verdict == "PASS" else sys.stderr
        print("check_regression_windows: [%s %s] %s: %s"
              % (opts["select"], variant, verdict, detail), file=stream)

    if all(v == "PASS" for v, _ in verdicts.values()):
        print("check_regression_windows: PASS (%s: %s)"
              % (opts["select"], ", ".join(variants)))
        return 0

    print("check_regression_windows: FAIL", file=sys.stderr)
    return 1


if __name__ == "__main__":
    sys.exit(main(sys.argv))
