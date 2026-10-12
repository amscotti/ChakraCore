#!/usr/bin/env python3
#-------------------------------------------------------------------------------------------------------
# Copyright (c) ChakraCore Project Contributors. All rights reserved.
# Licensed under the MIT license. See LICENSE.txt file in the project root for full license information.
#-------------------------------------------------------------------------------------------------------
"""B2.3 check gate (4): smoke and flag-output assertions.

Three sub-checks against the zig-built ch, asserting OUTPUT TEXT,
never the exit code alone:

  1. `ch -v` exits 0 and prints a `ch version N.N...` line.
  2. `ch --this-flag-does-not-exist <snippet>`: Test prints
     `Invalid Flag : --this-flag-does-not-exist` plus `Usage:` and
     exits 5; Debug prints the identical text and exits 0. Release
     (no ENABLE_DEBUG_CONFIG_OPTIONS flaglist) treats the unknown
     flag as a filename (bin/ch/ch.cpp main loop else-branch):
     a missing file runs silent with exit 0 — legacy-verified on
     build-legacy-release/ch (a file literally named
     `--this-flag-does-not-exist` DOES execute). The gate asserts
     the legacy-exact exit per --profile, the text for test/debug,
     and byte-empty output for release-shared.
  3. A one-line JS snippet (`print("zig-smoke-ok " + (1+1));`) written
     to a temp file runs with exit 0 and stdout exactly
     `zig-smoke-ok 2`.

Release-shared runs a 4th sub-check, the legacy smoke-check leg
itself (test/CMakeLists.txt:23-45): `ch test/Basics/hello.js`
exits 0 with stdout exactly `hello world` + `PASS`.

Usage: check_smoke.py <ch> [--profile test|debug-static|release-shared]
       [--self-test]
Defaults: --profile test (the B2 assertions, unchanged).
Exit code: 0 iff every sub-check passes.
"""

import os
import re
import subprocess
import sys
import tempfile

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import common

VERSION_RE = re.compile(r"^ch version \d+\.\d+")
BOGUS_FLAG = "--this-flag-does-not-exist"
SNIPPET = 'print("zig-smoke-ok " + (1+1));\n'
SNIPPET_WANT = "zig-smoke-ok 2"
# Legacy-verified invalid-flag exit per profile: Debug ch exits 0,
# Test ch exits 5 (build-legacy-debug/ch vs the B2 test ch; the
# printed text is identical); Release ch exits 0 SILENT (the flag is
# a filename there — see module docstring). None means
# "require nonzero".
INVALID_FLAG_WANT_RC = {"test": None, "debug-static": 0,
                        "release-shared": 0}
# Profiles where the invalid-flag probe must print NOTHING (Release:
# no flaglist, so no "Invalid Flag"/"Usage:" text exists to assert).
INVALID_FLAG_SILENT = frozenset({"release-shared"})
# Legacy smoke-check leg (test/CMakeLists.txt:23-45), asserted only
# for release-shared (the only profile whose legacy check IS this).
HELLO_REL = os.path.join("test", "Basics", "hello.js")
HELLO_WANT = ["hello world", "PASS"]


def check_version(ch):
    rc, out, err = common.run_capture([ch, "-v"])
    first = out.splitlines()[0] if out.splitlines() else ""
    print("check_smoke: ch -v -> rc=%d out=%r err=%r" % (rc, first, err))
    if rc != 0:
        return "ch -v exited %d, require 0" % rc
    if not VERSION_RE.search(out):
        return "ch -v output %r lacks a 'ch version N.N' line" % out
    return None


def check_invalid_flag(ch, snippet_path, profile="test"):
    rc, out, err = common.run_capture([ch, BOGUS_FLAG, snippet_path])
    combined = out + err
    print("check_smoke: ch %s -> rc=%d out=%r err=%r"
          % (BOGUS_FLAG, rc, out.splitlines()[:1], err.splitlines()[:1]))
    want_rc = INVALID_FLAG_WANT_RC[profile]
    if want_rc is None:
        if rc == 0:
            return "ch %s exited 0, require nonzero" % BOGUS_FLAG
    elif rc != want_rc:
        return ("ch %s exited %d, require %d (%s contract)"
                % (BOGUS_FLAG, rc, want_rc, profile))
    if profile in INVALID_FLAG_SILENT:
        # Release: the flag is a missing filename — legacy prints
        # nothing on either stream. A Test-style error here would
        # prove the wrong flag parser got built in.
        if combined != "":
            return ("ch %s must run silent in %s (got out=%r err=%r)"
                    % (BOGUS_FLAG, profile, out, err))
        return None
    want = "Invalid Flag : " + BOGUS_FLAG
    if want not in combined:
        return "invalid-flag output lacks %r (got %r)" % (want, combined)
    if "Usage:" not in combined:
        return "invalid-flag output lacks 'Usage:' (got %r)" % combined
    return None


def check_snippet(ch, snippet_path):
    rc, out, err = common.run_capture([ch, snippet_path])
    print("check_smoke: snippet -> rc=%d out=%r err=%r" % (rc, out, err))
    if rc != 0:
        return "snippet exited %d, require 0 (err=%r)" % (rc, err)
    if out.strip() != SNIPPET_WANT:
        return ("snippet stdout %r != %r"
                % (out.strip(), SNIPPET_WANT))
    return None


def check_hello(ch, hello_path):
    """The legacy Release smoke leg: ch hello.js prints 2 exact lines."""
    rc, out, err = common.run_capture([ch, hello_path])
    print("check_smoke: hello -> rc=%d out=%r err=%r" % (rc, out, err))
    if rc != 0:
        return "hello.js exited %d, require 0 (err=%r)" % (rc, err)
    if out.splitlines() != HELLO_WANT:
        return ("hello.js stdout %r != %r"
                % (out.splitlines(), HELLO_WANT))
    return None


def self_test():
    """Negative controls: decoy binaries must fail the TEXT assertions."""
    bad = 0

    def report(name, cond):
        print("self-test %-40s -> %s"
              % (name, "PASS" if cond else "FAIL"))
        return 0 if cond else 1

    tmp = tempfile.mkdtemp(prefix="check-smoke-selftest-")
    try:
        snippet = os.path.join(tmp, "snippet.js")
        with open(snippet, "w") as f:
            f.write(SNIPPET)
        # Hermetic decoys (no /bin/false on macOS): a silent
        # nonzero-exit binary and a silent rc-0 binary.
        decoy_false = os.path.join(tmp, "decoy-false.sh")
        with open(decoy_false, "w") as f:
            f.write("#!/bin/sh\nexit 3\n")
        os.chmod(decoy_false, 0o755)
        decoy_true = os.path.join(tmp, "decoy-true.sh")
        with open(decoy_true, "w") as f:
            f.write("#!/bin/sh\nexit 0\n")
        os.chmod(decoy_true, 0o755)
        bad += report("silent rc-3 decoy fails version gate",
                      check_version(decoy_false) is not None)
        bad += report("silent rc-3 decoy fails invalid-flag gate",
                      check_invalid_flag(decoy_false, snippet) is not None)
        bad += report("silent rc-3 decoy fails snippet gate",
                      check_snippet(decoy_false, snippet) is not None)
        # The rc-0 decoy proves the gates assert text, not rc alone:
        # version and snippet must reject a silent rc-0 binary, and
        # invalid-flag must reject rc 0.
        bad += report("silent rc-0 decoy fails version gate",
                      check_version(decoy_true) is not None)
        bad += report("silent rc-0 decoy fails invalid-flag gate",
                      check_invalid_flag(decoy_true, snippet) is not None)
        bad += report("silent rc-0 decoy fails snippet gate",
                      check_snippet(decoy_true, snippet) is not None)
        # Profile-awareness pins: decoys printing the REAL invalid-flag
        # text with legacy-exact exits must pass their own profile and
        # fail the other (exit asserted per profile, text always).
        decoy_dbg = os.path.join(tmp, "decoy-dbg.sh")
        with open(decoy_dbg, "w") as f:
            f.write("#!/bin/sh\necho 'Invalid Flag : %s'\n"
                    "echo 'Usage: ch'\nexit 0\n" % BOGUS_FLAG)
        os.chmod(decoy_dbg, 0o755)
        decoy_tst = os.path.join(tmp, "decoy-tst.sh")
        with open(decoy_tst, "w") as f:
            f.write("#!/bin/sh\necho 'Invalid Flag : %s'\n"
                    "echo 'Usage: ch'\nexit 5\n" % BOGUS_FLAG)
        os.chmod(decoy_tst, 0o755)
        bad += report("rc-0 text decoy passes debug invalid-flag",
                      check_invalid_flag(decoy_dbg, snippet,
                                         "debug-static") is None)
        bad += report("rc-0 text decoy fails test invalid-flag",
                      check_invalid_flag(decoy_dbg, snippet,
                                         "test") is not None)
        bad += report("rc-5 text decoy passes test invalid-flag",
                      check_invalid_flag(decoy_tst, snippet,
                                         "test") is None)
        bad += report("rc-5 text decoy fails debug invalid-flag",
                      check_invalid_flag(decoy_tst, snippet,
                                         "debug-static") is not None)
        bad += report("silent rc-0 decoy fails debug invalid-flag",
                      check_invalid_flag(decoy_true, snippet,
                                         "debug-static") is not None)
        # Release-silence pins: a silent rc-0 binary passes release
        # invalid-flag, but any Test/Debug-style error text fails it
        # (the gate asserts silence, not just the exit code); the
        # release-silent binary fails the test and debug gates.
        bad += report("silent rc-0 decoy passes release invalid-flag",
                      check_invalid_flag(decoy_true, snippet,
                                         "release-shared") is None)
        bad += report("rc-0 text decoy fails release invalid-flag",
                      check_invalid_flag(decoy_dbg, snippet,
                                         "release-shared") is not None)
        bad += report("rc-5 text decoy fails release invalid-flag",
                      check_invalid_flag(decoy_tst, snippet,
                                         "release-shared") is not None)
        bad += report("silent rc-0 decoy fails test invalid-flag "
                      "(already pinned above)",
                      check_invalid_flag(decoy_true, snippet,
                                         "test") is not None)
        # Hello-leg pins: a decoy printing the exact legacy lines
        # passes; wrong text and nonzero exit fail. The decoys ignore
        # argv, so any existing path stands in for hello.js.
        decoy_hello = os.path.join(tmp, "decoy-hello.sh")
        with open(decoy_hello, "w") as f:
            f.write("#!/bin/sh\necho 'hello world'\necho 'PASS'\nexit 0\n")
        os.chmod(decoy_hello, 0o755)
        bad += report("exact hello decoy passes hello gate",
                      check_hello(decoy_hello, snippet) is None)
        bad += report("silent rc-0 decoy fails hello gate",
                      check_hello(decoy_true, snippet) is not None)
        bad += report("silent rc-3 decoy fails hello gate",
                      check_hello(decoy_false, snippet) is not None)
        # Timeout-ceiling pin: a hung probe must raise loudly, not hang.
        try:
            common.run_capture([sys.executable, "-c",
                                "import time; time.sleep(30)"], timeout=1)
            bad += report("hung probe raises TimeoutExpired", False)
        except subprocess.TimeoutExpired:
            bad += report("hung probe raises TimeoutExpired", True)
    finally:
        import shutil
        shutil.rmtree(tmp, ignore_errors=True)

    print("check_smoke self-test: %s"
          % ("ALL NEGATIVE CONTROLS BEHAVE" if bad == 0
             else "%d CONTROL(S) MISBEHAVE" % bad))
    return 1 if bad else 0


def main(argv):
    args = list(argv[1:])
    if "--self-test" in args:
        return self_test()
    profile = "test"
    rest = []
    i = 0
    while i < len(args):
        if args[i] == "--profile":
            if i + 1 >= len(args):
                print("check_smoke: --profile needs a value",
                      file=sys.stderr)
                return 2
            profile = args[i + 1]
            i += 2
        else:
            rest.append(args[i])
            i += 1
    if profile not in INVALID_FLAG_WANT_RC:
        print("check_smoke: bad --profile %r: expected one of: %s"
              % (profile, ", ".join(sorted(INVALID_FLAG_WANT_RC))),
              file=sys.stderr)
        return 2
    if len(rest) != 1:
        print("usage: check_smoke.py <ch> "
              "[--profile test|debug-static|release-shared]",
              file=sys.stderr)
        return 2
    ch = rest[0]
    if not os.path.isfile(ch):
        print("check_smoke: missing file: %s" % ch, file=sys.stderr)
        return 2
    hello_path = None
    if profile == "release-shared":
        root = os.path.dirname(os.path.dirname(
            os.path.dirname(os.path.abspath(__file__))))
        hello_path = os.path.join(root, HELLO_REL)
        if not os.path.isfile(hello_path):
            print("check_smoke: missing file: %s" % hello_path,
                  file=sys.stderr)
            return 2

    tmp = tempfile.mkdtemp(prefix="check-smoke-")
    try:
        snippet = os.path.join(tmp, "snippet.js")
        with open(snippet, "w") as f:
            f.write(SNIPPET)
        try:
            failures = [
                ("version", check_version(ch)),
                ("invalid-flag", check_invalid_flag(ch, snippet, profile)),
                ("snippet", check_snippet(ch, snippet)),
            ]
            if hello_path is not None:
                failures.append(("hello", check_hello(ch, hello_path)))
        except subprocess.TimeoutExpired as e:
            failures = [("timeout", "ch probe exceeded the %ds ceiling: %s"
                         % (e.timeout, " ".join(e.cmd)))]
    finally:
        import shutil
        shutil.rmtree(tmp, ignore_errors=True)

    bad = [(name, err) for name, err in failures if err is not None]
    if bad:
        print("check_smoke: FAIL:", file=sys.stderr)
        for name, err in bad:
            print("check_smoke: FAIL: [%s] %s" % (name, err),
                  file=sys.stderr)
        return 1
    if hello_path is not None:
        print("check_smoke: PASS: version + silent invalid-flag + "
              "snippet + hello.js outputs match")
    else:
        print("check_smoke: PASS: version + invalid-flag + snippet "
              "outputs match")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
