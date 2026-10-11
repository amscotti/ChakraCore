#!/usr/bin/env python3
#-------------------------------------------------------------------------------------------------------
# Copyright (c) ChakraCore Project Contributors. All rights reserved.
# Licensed under the MIT license. See LICENSE.txt file in the project root for full license information.
#-------------------------------------------------------------------------------------------------------
"""B2.3 check gate (4): smoke and flag-output assertions.

Three sub-checks against the zig-built ch, asserting OUTPUT TEXT,
never the exit code alone:

  1. `ch -v` exits 0 and prints a `ch version N.N...` line.
  2. `ch --this-flag-does-not-exist <snippet>` exits NONZERO and its
     combined output contains both `Invalid Flag :
     --this-flag-does-not-exist` and `Usage:`.
  3. A one-line JS snippet (`print("zig-smoke-ok " + (1+1));`) written
     to a temp file runs with exit 0 and stdout exactly
     `zig-smoke-ok 2`.

Usage: check_smoke.py <ch> [--self-test]
Exit code: 0 iff all three sub-checks pass.
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


def check_version(ch):
    rc, out, err = common.run_capture([ch, "-v"])
    first = out.splitlines()[0] if out.splitlines() else ""
    print("check_smoke: ch -v -> rc=%d out=%r err=%r" % (rc, first, err))
    if rc != 0:
        return "ch -v exited %d, require 0" % rc
    if not VERSION_RE.search(out):
        return "ch -v output %r lacks a 'ch version N.N' line" % out
    return None


def check_invalid_flag(ch, snippet_path):
    rc, out, err = common.run_capture([ch, BOGUS_FLAG, snippet_path])
    combined = out + err
    print("check_smoke: ch %s -> rc=%d out=%r err=%r"
          % (BOGUS_FLAG, rc, out.splitlines()[:1], err.splitlines()[:1]))
    if rc == 0:
        return "ch %s exited 0, require nonzero" % BOGUS_FLAG
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
    if len(args) != 1:
        print("usage: check_smoke.py <ch>", file=sys.stderr)
        return 2
    ch = args[0]
    if not os.path.isfile(ch):
        print("check_smoke: missing file: %s" % ch, file=sys.stderr)
        return 2

    tmp = tempfile.mkdtemp(prefix="check-smoke-")
    try:
        snippet = os.path.join(tmp, "snippet.js")
        with open(snippet, "w") as f:
            f.write(SNIPPET)
        try:
            failures = [
                ("version", check_version(ch)),
                ("invalid-flag", check_invalid_flag(ch, snippet)),
                ("snippet", check_snippet(ch, snippet)),
            ]
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
    print("check_smoke: PASS: version + invalid-flag + snippet outputs match")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
