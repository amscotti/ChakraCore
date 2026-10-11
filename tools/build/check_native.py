#!/usr/bin/env python3
#-------------------------------------------------------------------------------------------------------
# Copyright (c) ChakraCore Project Contributors. All rights reserved.
# Licensed under the MIT license. See LICENSE.txt file in the project root for full license information.
#-------------------------------------------------------------------------------------------------------
"""B2.3 check gate (3): shared embedding (native) test, legacy method.

Runs the SAME script and arguments the legacy RelWithDebInfo shared/ICU
`ninja check` runs for its native leg (test/CMakeLists.txt:48-55):

    test/native-tests/test_native.sh <ch> RelWithDebInfo <cc> <cxx> <repo-root>

One deliberate deviation: the script generates Makefiles (and objects)
inside the directory it runs in, so it runs in a COPY of
test/native-tests under $TMPDIR instead of in-tree. The method (same
script, same arguments, same zig-built ch and dylib) is unchanged, and
the source tree is never written to. Stale generated files (Makefile,
sample.o) are excluded from the copy so the run starts clean.

Requires: rc 0, `test-shared-basic : PASS` in the output, exactly 8
`Ignoring test-static-...` lines (the 8 static-only suites are skipped
in a shared build — the legacy baseline behavior), and no failure line
of the script's exact `<suite> FAILED` form (test_native.sh:34).

The cc/cxx compilers resolve HERE at gate time (shutil.which, failing
loudly when absent) so a missing standalone clang breaks only this
gate — never configure time or plain `zig build`.

Usage: check_native.py <test_native.sh> <ch> <cc> <cxx> [--self-test]
Exit code: 0 iff the native leg ran green.
"""

import os
import re
import shutil
import subprocess
import sys
import tempfile

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import common

EXPECTED_IGNORES = 8
SHARED_PASS = "test-shared-basic : PASS"
# The script's exact failure-line format (`echo "${TEST_PATH} FAILED"`,
# test_native.sh:34): a bare "FAILED" substring would also match e.g.
# "XFAILED" notes or compiler chatter mid-line.
FAILED_RE = re.compile(r"^\S+ FAILED\s*$")


def run_native(sh_path, work_dir, ch, build_type, cc, cxx, repo_root):
    """Run test_native.sh in work_dir, tee output, return (rc, text)."""
    cmd = ["bash", sh_path, ch, build_type, cc, cxx, repo_root]
    print("check_native: running in %s: %s" % (work_dir, " ".join(cmd)))
    sys.stdout.flush()
    rc, lines = common.run_tee(cmd, cwd=work_dir)
    return rc, "".join(lines)


def check_output(text):
    """Return a list of failure strings (empty means green)."""
    failures = []
    ignores = sum(1 for line in text.splitlines()
                  if line.startswith("Ignoring test-static-"))
    if SHARED_PASS not in text:
        failures.append("missing %r in output" % SHARED_PASS)
    if ignores != EXPECTED_IGNORES:
        failures.append("got %d static-test Ignoring lines, require %d"
                        % (ignores, EXPECTED_IGNORES))
    failed_lines = [line.strip() for line in text.splitlines()
                    if FAILED_RE.match(line)]
    if failed_lines:
        failures.append("output contains %d FAILED line(s): %r"
                        % (len(failed_lines), failed_lines[:3]))
    return failures


def self_test():
    """Negative controls: doctored logs rejected, bogus ch fails loudly."""
    bad = 0

    def report(name, cond):
        nonlocal_bad = cond
        print("self-test %-28s -> %s"
              % (name, "PASS" if nonlocal_bad else "FAIL"))
        return 0 if nonlocal_bad else 1

    good = ("Testing test-static-char\nIgnoring test-static-char\n"
            "Testing test-shared-basic\ntest-shared-basic : PASS\n")
    bad += report("8 ignores + PASS accepted",
                  check_output(good.replace(
                      "Ignoring test-static-char",
                      "\n".join("Ignoring test-static-%d" % i
                                for i in range(8)))) == [])
    bad += report("missing PASS line rejected",
                  check_output("Ignoring test-static-x\n") != [])
    bad += report("7 ignores rejected",
                  check_output("\n".join("Ignoring test-static-%d" % i
                                         for i in range(7))
                               + "\n" + SHARED_PASS + "\n") != [])
    bad += report("FAILED line rejected",
                  check_output(SHARED_PASS + "\nsomething FAILED\n") != [])
    bad += report("exact-format FAILED rejected",
                  check_output(SHARED_PASS + "\ntest-shared-basic FAILED\n"
                               ) != [])
    good_ignores = "\n".join("Ignoring test-static-%d" % i for i in range(8))
    bad += report("mid-line FAILED substring accepted",
                  check_output(good_ignores + "\n" + SHARED_PASS + "\n"
                               "note: 0 XFAILED suites seen\n") == [])

    # Bogus ch: the script must fail, not silently pass.
    tmp = tempfile.mkdtemp(prefix="check-native-selftest-")
    try:
        src = os.path.normpath(os.path.join(
            os.path.dirname(os.path.abspath(__file__)),
            "..", "..", "test", "native-tests"))
        work = os.path.join(tmp, "native-tests")
        shutil.copytree(src, work,
                        ignore=shutil.ignore_patterns(
                            "Makefile", "sample.o", "*.o", "*.d"))
        rc, _ = run_native(os.path.join(work, "test_native.sh"), work,
                           "/nonexistent/ch", "RelWithDebInfo",
                           "clang", "clang++", os.path.dirname(src))
        bad += report("bogus ch exits nonzero (rc=%d)" % rc, rc != 0)
    finally:
        shutil.rmtree(tmp, ignore_errors=True)

    # Lazy-clang pin: missing compilers fail THIS gate loudly (rc 2) —
    # the only clang failure mode; configure never probes fatally.
    import contextlib as _ctx
    import io as _io
    tmp2 = tempfile.mkdtemp(prefix="check-native-selftest-clang-")
    try:
        decoy_sh = os.path.join(tmp2, "test_native.sh")
        decoy_ch = os.path.join(tmp2, "ch")
        open(decoy_sh, "w").close()
        open(decoy_ch, "w").close()
        err = _io.StringIO()
        with _ctx.redirect_stderr(err):
            rc = main(["check_native.py", decoy_sh, decoy_ch,
                       "/nonexistent-cc", "/nonexistent-cxx"])
        bad += report("missing compilers fail gate loudly (rc=%d)" % rc,
                      rc == 2 and "cannot find compiler" in err.getvalue())
    finally:
        shutil.rmtree(tmp2, ignore_errors=True)

    print("check_native self-test: %s"
          % ("ALL NEGATIVE CONTROLS BEHAVE" if bad == 0
             else "%d CONTROL(S) MISBEHAVE" % bad))
    return 1 if bad else 0


def main(argv):
    args = list(argv[1:])
    if "--self-test" in args:
        return self_test()
    if len(args) != 4:
        print("usage: check_native.py <test_native.sh> <ch> <cc> <cxx>",
              file=sys.stderr)
        return 2
    sh_src, ch, cc, cxx = args
    for path in (sh_src, ch):
        if not os.path.isfile(path):
            print("check_native: missing file: %s" % path, file=sys.stderr)
            return 2
    # Lazy compiler resolution (see build.zig): fail HERE at gate time,
    # never at configure time.
    for tool in (cc, cxx):
        if shutil.which(tool) is None:
            print("check_native: cannot find compiler %r on PATH; needed "
                  "to build the native embedding test" % tool,
                  file=sys.stderr)
            return 2
    src_dir = os.path.dirname(os.path.abspath(sh_src))
    repo_root = os.path.dirname(os.path.dirname(src_dir))

    tmp = tempfile.mkdtemp(prefix="check-native-")
    try:
        try:
            work = os.path.join(tmp, "native-tests")
            shutil.copytree(src_dir, work,
                            ignore=shutil.ignore_patterns(
                                "Makefile", "sample.o", "*.o", "*.d"))
            rc, text = run_native(os.path.join(work, "test_native.sh"), work,
                                  os.path.abspath(ch), "RelWithDebInfo",
                                  cc, cxx, repo_root)
        finally:
            shutil.rmtree(tmp, ignore_errors=True)
    except subprocess.TimeoutExpired:
        print("check_native: FAIL: test_native.sh exceeded the %ds ceiling"
              % common.TEE_TIMEOUT, file=sys.stderr)
        return 1

    print("check_native: exit=%d" % rc)
    failures = check_output(text)
    if rc != 0:
        failures.insert(0, "test_native.sh exited %d" % rc)
    if failures:
        print("check_native: FAIL:", file=sys.stderr)
        for f in failures:
            print("check_native: FAIL: %s" % f, file=sys.stderr)
        return 1
    print("check_native: PASS: %s, %d static suites ignored"
          % (SHARED_PASS, EXPECTED_IGNORES))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
