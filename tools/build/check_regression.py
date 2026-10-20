#!/usr/bin/env python3
#-------------------------------------------------------------------------------------------------------
# Copyright (c) ChakraCore Project Contributors. All rights reserved.
# Licensed under the MIT license. See LICENSE.txt file in the project root for full license information.
#-------------------------------------------------------------------------------------------------------
"""B2.3 check gate (1): regression suite, legacy-exact selection.

Runs the SAME command the legacy `ninja check` runs for its regression
leg (test/CMakeLists.txt:39-44), with the build-type flag selected by
--select (TEST_BUILD_TYPE, test/CMakeLists.txt:3-7):

    runtests.py --test --variants=disable_jit --binary=<ch>    (test)
    runtests.py --debug --variants=disable_jit --binary=<ch>   (debug-static)

No --not-tag: ICU is present in these profiles, so TEST_ICU is empty
(test/CMakeLists.txt:9-11); TEST_VARIANT is --variants=disable_jit
(:13-18). The in-script tag selection (exclude_xplat, exclude_mac,
Slow, ...) is runtests.py's own and therefore identical by
construction.

Requires the legacy baseline counts for the selection, single-sourced
from tools/build/gate_counts.json (test and debug pins with the full
why-chain in its _lineage key; receipts doc/c2-s2-bigint.md,
doc/c2-s9-modifiers.md, doc/c2-s13-symbol-keys.md,
doc/c2-s14-blockscope.md, doc/c2-s9-namedgroups.md). The --debug
selection honestly differs from Test's count (doc/b3-profiles.md §5).
The verdict comes from parsing the real `Total: passed N, failed M`
summary line (runtests.py:567), not from the exit code alone; a
nonzero exit, a missing summary, or 2+ summary lines also fails
loudly. The run has an outer ceiling (common.REGRESSION_TIMEOUT);
runtests.py's own per-test timeout is untouched.

Usage: check_regression.py <runtests.py> <ch> [--select test|debug]
       [--expected-passed N] [--expected-failed M] [--self-test]
Defaults: --select test with the gate_counts.json test pin (the B2
test profile). Exit code: 0 iff the suite ran green with exactly the
expected counts.
"""

import json
import os
import re
import subprocess
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import common

TOTAL_RE = re.compile(r"^Total: passed (\d+), failed (\d+)\s*$")
# Single-sourced gate pins: tools/build/gate_counts.json holds the
# per-selection legacy baselines (why-chain in its _lineage key;
# receipts doc/c2-s2-bigint.md, doc/c2-s9-modifiers.md,
# doc/c2-s13-symbol-keys.md, doc/c2-s14-blockscope.md,
# doc/c2-s9-namedgroups.md; why the selections differ:
# doc/b3-profiles.md §5). Missing/malformed source fails loudly here
# with no silent defaults, so a lost or hand-edited pin can never
# pass as the old hard-coded counts.
GATE_COUNTS_PATH = os.path.join(os.path.dirname(os.path.abspath(__file__)),
                                "gate_counts.json")


def _load_gate_counts(path=GATE_COUNTS_PATH):
    try:
        with open(path, "r", encoding="utf-8") as f:
            data = json.load(f)
    except OSError as e:
        print("check_regression: FAIL: cannot read gate counts %s: %s"
              % (path, e), file=sys.stderr)
        raise SystemExit(2)
    except ValueError as e:
        print("check_regression: FAIL: malformed gate counts %s: %s"
              % (path, e), file=sys.stderr)
        raise SystemExit(2)
    counts = {}
    for select in ("test", "debug"):
        entry = data.get(select) if isinstance(data, dict) else None
        if (not isinstance(entry, dict)
                or not isinstance(entry.get("passed"), int)
                or not isinstance(entry.get("failed"), int)
                or entry["passed"] < 0 or entry["failed"] < 0
                or isinstance(entry.get("passed"), bool)
                or isinstance(entry.get("failed"), bool)):
            print("check_regression: FAIL: gate counts %s missing/invalid "
                  "%r pin: need {\"passed\": int >= 0, \"failed\": int >= 0}"
                  % (path, select), file=sys.stderr)
            raise SystemExit(2)
        counts[select] = (entry["passed"], entry["failed"])
    return counts


SELECT_FLAGS = {"test": "--test", "debug": "--debug"}
SELECT_COUNTS = _load_gate_counts()
# Back-compat default pair (the B2 test profile); kept as names, not
# literals, so a pin bump cannot strand a stale default here.
EXPECTED_PASSED, EXPECTED_FAILED = SELECT_COUNTS["test"]


def parse_total(lines):
    """Return (passed, failed) from the Total summary line, or None.

    Requires EXACTLY ONE summary line: None when zero match, ValueError
    when 2+ match (a rerun/appended log must not pass on one lucky line).
    """
    found = []
    for line in lines:
        m = TOTAL_RE.match(line.strip())
        if m:
            found.append((int(m.group(1)), int(m.group(2))))
    if len(found) > 1:
        raise ValueError("expected exactly one 'Total: passed N, failed M' "
                         "summary line, got %d: %r" % (len(found), found))
    return found[0] if found else None


def check_counts(parsed, exp_passed, exp_failed):
    """Return an error string, or None when counts match."""
    if parsed is None:
        return "no 'Total: passed N, failed M' summary line in runtests.py output"
    passed, failed = parsed
    if passed != exp_passed or failed != exp_failed:
        return ("count mismatch: got passed %d / failed %d, "
                "require passed %d / failed %d"
                % (passed, failed, exp_passed, exp_failed))
    return None


def self_test():
    """Negative controls: the parser/verdict must reject doctored logs."""
    # All vectors derive from the gate_counts.json pins: the exact
    # baseline must accept, and every doctored variant (off-by-N passed,
    # nonzero failed, duplicated/missing/malformed summary) must reject.
    tp, tf = SELECT_COUNTS["test"]
    exact = "Total: passed %d, failed %d" % (tp, tf)
    cases = [
        ("exact baseline",
         [exact], True),
        ("one failure",
         ["Total: passed %d, failed %d" % (tp, tf + 1)], False),
        ("wrong passed",
         ["Total: passed %d, failed %d" % (tp + 4, tf)], False),
        ("wrong total",
         [exact, "Total: passed 10, failed 0"], False),
        ("duplicate summary",
         [exact, exact], False),
        ("missing summary",
         ["############ Results ###########", "Success!"], False),
        ("lookalike line",
         ["X" + exact, exact + " wisely"], False),
    ]
    bad = 0
    for name, lines, want_ok in cases:
        try:
            err = check_counts(parse_total(lines),
                               EXPECTED_PASSED, EXPECTED_FAILED)
        except ValueError as e:
            err = str(e)
        ok = err is None
        status = "PASS" if ok == want_ok else "FAIL"
        if ok != want_ok:
            bad += 1
        print("self-test %-16s -> %s (verdict=%s err=%r)"
              % (name, status, "accept" if ok else "reject", err))
    # Profile-awareness pins, derived from both gate_counts.json pins:
    # each selection's baseline must be accepted under its own
    # expectations and rejected under the other's, so a count/selection
    # mismatch (debug flag with test counts or vice versa) can never
    # pass. Skipped when both pins coincide (then swapping is a no-op).
    dp, df = SELECT_COUNTS["debug"]
    dbg_line = "Total: passed %d, failed %d" % (dp, df)
    try:
        err = check_counts(parse_total([dbg_line]), *SELECT_COUNTS["debug"])
    except ValueError as e:
        err = str(e)
    if err is None:
        print("self-test %-16s -> PASS (verdict=accept)" % "debug baseline")
    else:
        print("self-test %-16s -> FAIL (verdict=reject err=%r)"
              % ("debug baseline", err))
        bad += 1
    for name, line, expect in (("debug vs test", dbg_line, SELECT_COUNTS["test"]),
                               ("test vs debug", exact, SELECT_COUNTS["debug"])):
        if SELECT_COUNTS["test"] == SELECT_COUNTS["debug"]:
            print("self-test %-16s -> PASS (pins coincide, skip)" % name)
            continue
        try:
            err = check_counts(parse_total([line]), *expect)
        except ValueError as e:
            err = str(e)
        if err is not None:
            print("self-test %-16s -> PASS (verdict=reject)" % name)
        else:
            print("self-test %-16s -> FAIL (verdict=accept)" % name)
            bad += 1
    if SELECT_FLAGS.get("debug") == "--debug":
        print("self-test %-16s -> PASS" % "debug flag map")
    else:
        print("self-test %-16s -> FAIL" % "debug flag map")
        bad += 1
    # Outer-ceiling pin: a hung child must raise loudly, not hang forever.
    try:
        common.run_tee([sys.executable, "-c",
                        "import time; time.sleep(30)"], timeout=1)
        print("self-test %-16s -> FAIL (no timeout raised)" % "hung child")
        bad += 1
    except subprocess.TimeoutExpired:
        print("self-test %-16s -> PASS (TimeoutExpired raised)"
              % "hung child")
    print("check_regression self-test: %s"
          % ("ALL NEGATIVE CONTROLS BEHAVE" if bad == 0
             else "%d CONTROL(S) MISBEHAVE" % bad))
    return 1 if bad else 0


def main(argv):
    args = list(argv[1:])
    if "--self-test" in args:
        return self_test()
    exp_passed, exp_failed = None, None
    select = "test"
    rest = []
    i = 0
    while i < len(args):
        if args[i] == "--expected-passed":
            exp_passed = int(args[i + 1])
            i += 2
        elif args[i] == "--expected-failed":
            exp_failed = int(args[i + 1])
            i += 2
        elif args[i] == "--select":
            select = args[i + 1]
            i += 2
        else:
            rest.append(args[i])
            i += 1
    if select not in SELECT_FLAGS:
        print("check_regression: bad --select %r: expected one of: %s"
              % (select, ", ".join(sorted(SELECT_FLAGS))), file=sys.stderr)
        return 2
    # Fill only the options the caller omitted, so partial overrides such as
    # --expected-failed alone remain meaningful to the verdict.
    default_passed, default_failed = SELECT_COUNTS[select]
    if exp_passed is None:
        exp_passed = default_passed
    if exp_failed is None:
        exp_failed = default_failed
    if len(rest) != 2:
        print("usage: check_regression.py <runtests.py> <ch> "
              "[--select test|debug] [--expected-passed N] "
              "[--expected-failed M]",
              file=sys.stderr)
        return 2
    runtests, ch = rest[0], rest[1]
    for path in (runtests, ch):
        if not os.path.isfile(path):
            print("check_regression: missing file: %s" % path, file=sys.stderr)
            return 2

    # Legacy-exact invocation (test/CMakeLists.txt:39-44). The binary
    # MUST be absolute: runtests.py spawns it with cwd set to each
    # test's folder (runtests.py:475), so a relative path fails every
    # test with FileNotFoundError (observed 0 passed). Legacy passes
    # ${CMAKE_BINARY_DIR}/ch, which is absolute.
    cmd = [sys.executable, runtests, SELECT_FLAGS[select],
           "--variants=disable_jit",
           "--binary=" + os.path.abspath(ch)]
    print("check_regression: running: %s" % " ".join(cmd))
    sys.stdout.flush()
    timed_out = False
    try:
        rc, lines = common.run_tee(cmd, timeout=common.REGRESSION_TIMEOUT)
    except subprocess.TimeoutExpired:
        print("check_regression: FAIL: runtests.py exceeded the %ds outer "
              "ceiling" % common.REGRESSION_TIMEOUT, file=sys.stderr)
        timed_out = True
        rc, lines = 124, []

    # Defensive: mirror runtests.py's own profile.dpl.* cleanup so an
    # interrupted/failed run cannot leave residue in test/ (legacy
    # method leaves the same files behind on the same paths).
    test_root = os.path.dirname(os.path.abspath(runtests))
    import glob as _glob
    for f in _glob.glob(os.path.join(test_root, "*", "profile.dpl.*")):
        try:
            os.remove(f)
            print("check_regression: removed leftover %s" % f)
        except OSError as e:
            print("check_regression: WARNING: cannot remove %s: %s" % (f, e))

    if timed_out:
        return 1
    try:
        parsed = parse_total(lines)
    except ValueError as e:
        print("check_regression: FAIL: %s" % e, file=sys.stderr)
        return 1
    print("check_regression: exit=%d parsed=%r require passed %d / failed %d"
          % (rc, parsed, exp_passed, exp_failed))
    err = check_counts(parsed, exp_passed, exp_failed)
    if rc != 0:
        print("check_regression: FAIL: runtests.py exited %d%s"
              % (rc, "" if err is None else "; " + err), file=sys.stderr)
        return 1
    if err is not None:
        print("check_regression: FAIL: %s (exit was 0)" % err, file=sys.stderr)
        print("check_regression: last 40 output lines:", file=sys.stderr)
        for line in lines[-40:]:
            sys.stderr.write("  | " + line)
        return 1
    print("check_regression: PASS: passed %d / failed %d"
          % (parsed[0], parsed[1]))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
