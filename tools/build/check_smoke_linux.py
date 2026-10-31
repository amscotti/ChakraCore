#!/usr/bin/env python3
#-------------------------------------------------------------------------------------------------------
# Copyright (c) ChakraCore Project Contributors. All rights reserved.
# Licensed under the MIT license. See LICENSE.txt file in the project root for full license information.
#-------------------------------------------------------------------------------------------------------
"""B4.3 check gate (4): Linux smoke and flag-output assertions.

Five sub-checks against the zig-built ch, asserting OUTPUT TEXT,
never the exit code alone:

  1. `ch -v` exits 0 and prints a `ch version N.N...` line.
  2. `ch --help` exits 0 and prints a `Usage:` line.
  3. `ch --this-flag-does-not-exist <snippet>`: the shared ch.cpp
     flag parser (ENABLE_DEBUG_CONFIG_OPTIONS, same define as the
     Mac Test profile) prints `Invalid Flag : ...` plus `Usage:`
     and exits nonzero. Code-derived expectation, unverified on
     Linux (see below).
  4. A one-line JS snippet runs with exit 0 and stdout exactly
     `zig-smoke-ok 2`.
  5. `ch test/Basics/hello.js` exits 0 with stdout exactly
     `hello world` + `PASS`.

Legs 3-5 need engine execution, which currently aborts during
runtime init under qemu-user (see doc/b4-linux.md stage 4: a
qemu-user defect on guest open of /proc/self/maps, believed
environmental — but no native Linux behavior run exists yet, so
this is unproven; B4 remains BLOCKED-with-green-build). A leg that observes the abort signature (SIGABRT,
silent stdout, and a pure qemu signal-6 stderr observation — at
least one `uncaught target signal 6 (` occurrence and no other
qemu-phrased signal occurrence)
reports BLOCKED, not FAIL — but the gate still exits 1: BLOCKED
is not green. The qemu-line requirement pins BLOCKED to guest
deaths observed by qemu-user; a silent SIGABRT without it (a
native abort, a host-side kill) is a plain FAIL. Legs 1-2 pass
on trivial execution.

Usage: check_smoke_linux.py <ch> [--self-test]
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
HELLO_REL = os.path.join("test", "Basics", "hello.js")
HELLO_WANT = ["hello world", "PASS"]
# SIGABRT exit presentations: 134 via shells, -6 via subprocess on signal.
ABORT_RCS = (134, -6)


def is_abort(rc, out, err):
    """True when the probe died in engine-init abort (qemu; b4-linux §8).

    Pure signal 6 only: at least one qemu signal-6 occurrence in
    err and zero occurrences of any other qemu-phrased signal
    number (occurrence-based via common.qemu_target_signals, so a
    mixed signal-6 + signal-11 stderr vetos BLOCKED on separate
    lines or on one line carrying both), on top of the abort-rc
    and empty-stdout conditions.
    """
    if rc not in ABORT_RCS:
        return False
    if out.strip() != "":
        return False
    signals = common.qemu_target_signals(err)
    return bool(signals) and all(n == 6 for n in signals)


def blocked(name):
    return ("BLOCKED: [%s] engine init abort under qemu-user "
            "(doc/b4-linux.md section 8); 0 ran, not 0/N" % name)


def check_version(ch):
    rc, out, err = common.run_capture([ch, "-v"])
    first = out.splitlines()[0] if out.splitlines() else ""
    print("check_smoke_linux: ch -v -> rc=%d out=%r err=%r" % (rc, first, err))
    if rc != 0:
        return "ch -v exited %d, require 0" % rc
    if not VERSION_RE.search(out):
        return "ch -v output %r lacks a 'ch version N.N' line" % out
    return None


def check_help(ch):
    rc, out, err = common.run_capture([ch, "--help"])
    print("check_smoke_linux: ch --help -> rc=%d lines=%d"
          % (rc, len(out.splitlines())))
    if rc != 0:
        return "ch --help exited %d, require 0" % rc
    if "Usage:" not in out:
        return "ch --help output lacks 'Usage:'"
    return None


def check_invalid_flag(ch, snippet_path):
    rc, out, err = common.run_capture([ch, BOGUS_FLAG, snippet_path])
    combined = out + err
    print("check_smoke_linux: ch %s -> rc=%d out=%r err=%r"
          % (BOGUS_FLAG, rc, out.splitlines()[:1], err.splitlines()[:1]))
    if is_abort(rc, out, err):
        return blocked("invalid-flag")
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
    print("check_smoke_linux: snippet -> rc=%d out=%r err=%r" % (rc, out, err))
    if is_abort(rc, out, err):
        return blocked("snippet")
    if rc != 0:
        return "snippet exited %d, require 0 (err=%r)" % (rc, err)
    if out.strip() != SNIPPET_WANT:
        return ("snippet stdout %r != %r"
                % (out.strip(), SNIPPET_WANT))
    return None


def check_hello(ch, hello_path):
    rc, out, err = common.run_capture([ch, hello_path])
    print("check_smoke_linux: hello -> rc=%d out=%r err=%r" % (rc, out, err))
    if is_abort(rc, out, err):
        return blocked("hello")
    if rc != 0:
        return "hello.js exited %d, require 0 (err=%r)" % (rc, err)
    if out.splitlines() != HELLO_WANT:
        return ("hello.js stdout %r != %r"
                % (out.splitlines(), HELLO_WANT))
    return None


def self_test():
    """Negative controls: decoys must fail the TEXT assertions; the
    abort signature must grade BLOCKED (and BLOCKED must not pass)."""
    bad = 0

    def report(name, cond):
        print("self-test %-44s -> %s"
              % (name, "PASS" if cond else "FAIL"))
        return 0 if cond else 1

    tmp = tempfile.mkdtemp(prefix="check-smoke-linux-selftest-")
    try:
        snippet = os.path.join(tmp, "snippet.js")
        with open(snippet, "w") as f:
            f.write(SNIPPET)

        def decoy(name, body):
            p = os.path.join(tmp, name)
            with open(p, "w") as f:
                f.write(body)
            os.chmod(p, 0o755)
            return p

        decoy_false = decoy("decoy-false.sh", "#!/bin/sh\nexit 3\n")
        decoy_true = decoy("decoy-true.sh", "#!/bin/sh\nexit 0\n")
        decoy_abort = decoy(
            "decoy-abort.sh",
            "#!/bin/sh\necho 'qemu-x86_64: uncaught target signal 6 "
            "(Aborted) - core dumped' >&2\nexit 134\n")
        bad += report("silent rc-3 decoy fails version gate",
                      check_version(decoy_false) is not None)
        bad += report("silent rc-3 decoy fails help gate",
                      check_help(decoy_false) is not None)
        bad += report("silent rc-3 decoy fails invalid-flag gate",
                      check_invalid_flag(decoy_false, snippet) is not None)
        bad += report("silent rc-3 decoy fails snippet gate",
                      check_snippet(decoy_false, snippet) is not None)
        bad += report("silent rc-0 decoy fails version gate",
                      check_version(decoy_true) is not None)
        bad += report("silent rc-0 decoy fails help gate",
                      check_help(decoy_true) is not None)
        bad += report("silent rc-0 decoy fails snippet gate",
                      check_snippet(decoy_true, snippet) is not None)
        bad += report("silent rc-0 decoy fails invalid-flag gate",
                      check_invalid_flag(decoy_true, snippet) is not None)
        # Abort grading: silent rc-134 with qemu's stderr line grades
        # BLOCKED (a string, so the gate still exits 1) on execution
        # legs only; it must fail the trivial legs as a plain
        # mismatch, never as BLOCKED.
        for leg, fn in (("invalid-flag", check_invalid_flag),
                        ("snippet", check_snippet),
                        ("hello", check_hello)):
            err = fn(decoy_abort, snippet)
            bad += report("qemu-signed rc-134 decoy grades BLOCKED on %s" % leg,
                          err is not None and err.startswith("BLOCKED"))
        bad += report("qemu-signed rc-134 decoy fails version (not BLOCKED)",
                      (lambda e: e is not None and not e.startswith("BLOCKED"))(
                          check_version(decoy_abort)))
        # A silent rc-134 WITHOUT qemu's line is a plain failure, not
        # BLOCKED (native abort, host-side kill).
        decoy_bare = decoy("decoy-bare-abort.sh", "#!/bin/sh\nexit 134\n")
        bad += report("rc-134 decoy without qemu line is FAIL not BLOCKED",
                      (lambda e: e is not None and not e.startswith("BLOCKED"))(
                          check_snippet(decoy_bare, snippet)))
        # A noisy abort (output + rc 134) is a plain failure, not
        # BLOCKED, even carrying qemu's line.
        decoy_noisy = decoy(
            "decoy-noisy.sh",
            "#!/bin/sh\necho junk\necho 'qemu-x86_64: uncaught target "
            "signal 6 (Aborted) - core dumped' >&2\nexit 134\n")
        bad += report("noisy rc-134 decoy is FAIL not BLOCKED",
                      (lambda e: e is not None and not e.startswith("BLOCKED"))(
                          check_snippet(decoy_noisy, snippet)))
        # Signal 61 must not substring-match the signal-6 signature.
        decoy_sig61 = decoy(
            "decoy-sig61.sh",
            "#!/bin/sh\necho 'qemu-x86_64: uncaught target signal 61 "
            "(Real-time event 61) - core dumped' >&2\nexit 134\n")
        bad += report("signal-61 rc-134 decoy is FAIL not BLOCKED",
                      (lambda e: e is not None and not e.startswith("BLOCKED"))(
                          check_snippet(decoy_sig61, snippet)))
        bad += report("is_abort is False for signal 61",
                      not is_abort(134, "",
                                   "qemu-x86_64: uncaught target signal 61 "
                                   "(Real-time event 61) - core dumped"))
        sig6_line = ("qemu-x86_64: uncaught target signal 6 (Aborted) "
                     "- core dumped")
        sig11_line = ("qemu-x86_64: uncaught target signal 11 "
                      "(Segmentation fault) - core dumped")
        bad += report("is_abort is True for pure signal 6",
                      is_abort(134, "", sig6_line))
        bad += report("is_abort is False for mixed sig6+sig11 lines",
                      not is_abort(134, "", sig6_line + "\n" + sig11_line))
        bad += report("is_abort is False for mixed sig11+sig6 lines",
                      not is_abort(134, "", sig11_line + "\n" + sig6_line))
        bad += report("is_abort is False for mixed sig6+sig11 same line",
                      not is_abort(134, "", sig6_line + "; " + sig11_line))
        # Positive pins: exact-text decoys pass their gates.
        decoy_ver = decoy("decoy-ver.sh",
                          "#!/bin/sh\necho 'ch version 1.13.0.0-beta'\nexit 0\n")
        bad += report("version-text decoy passes version gate",
                      check_version(decoy_ver) is None)
        decoy_help = decoy("decoy-help.sh",
                           "#!/bin/sh\necho 'Usage: ch'\nexit 0\n")
        bad += report("usage-text decoy passes help gate",
                      check_help(decoy_help) is None)
        decoy_flag = decoy("decoy-flag.sh",
                           "#!/bin/sh\necho 'Invalid Flag : %s'\n"
                           "echo 'Usage: ch'\nexit 5\n" % BOGUS_FLAG)
        bad += report("flag-text rc-5 decoy passes invalid-flag gate",
                      check_invalid_flag(decoy_flag, snippet) is None)
        decoy_snip = decoy("decoy-snip.sh",
                           "#!/bin/sh\necho 'zig-smoke-ok 2'\nexit 0\n")
        bad += report("snippet-text decoy passes snippet gate",
                      check_snippet(decoy_snip, snippet) is None)
        decoy_hello = decoy("decoy-hello.sh",
                            "#!/bin/sh\necho 'hello world'\necho 'PASS'\nexit 0\n")
        bad += report("hello-text decoy passes hello gate",
                      check_hello(decoy_hello, snippet) is None)
        try:
            common.run_capture([sys.executable, "-c",
                                "import time; time.sleep(30)"], timeout=1)
            bad += report("hung probe raises TimeoutExpired", False)
        except subprocess.TimeoutExpired:
            bad += report("hung probe raises TimeoutExpired", True)
    finally:
        import shutil
        shutil.rmtree(tmp, ignore_errors=True)

    print("check_smoke_linux self-test: %s"
          % ("ALL NEGATIVE CONTROLS BEHAVE" if bad == 0
             else "%d CONTROL(S) MISBEHAVE" % bad))
    return 1 if bad else 0


def main(argv):
    args = list(argv[1:])
    if "--self-test" in args:
        return self_test()
    if len(args) != 1:
        print("usage: check_smoke_linux.py <ch>", file=sys.stderr)
        return 2
    ch = args[0]
    if not os.path.isfile(ch):
        print("check_smoke_linux: missing file: %s" % ch, file=sys.stderr)
        return 2
    root = os.path.dirname(os.path.dirname(
        os.path.dirname(os.path.abspath(__file__))))
    hello_path = os.path.join(root, HELLO_REL)
    if not os.path.isfile(hello_path):
        print("check_smoke_linux: missing file: %s" % hello_path,
              file=sys.stderr)
        return 2

    tmp = tempfile.mkdtemp(prefix="check-smoke-linux-")
    try:
        snippet = os.path.join(tmp, "snippet.js")
        with open(snippet, "w") as f:
            f.write(SNIPPET)
        try:
            failures = [
                ("version", check_version(ch)),
                ("help", check_help(ch)),
                ("invalid-flag", check_invalid_flag(ch, snippet)),
                ("snippet", check_snippet(ch, snippet)),
                ("hello", check_hello(ch, hello_path)),
            ]
        except subprocess.TimeoutExpired as e:
            failures = [("timeout", "ch probe exceeded the %ds ceiling: %s"
                         % (e.timeout, " ".join(e.cmd)))]
    finally:
        import shutil
        shutil.rmtree(tmp, ignore_errors=True)

    bad = [(name, err) for name, err in failures if err is not None]
    if bad:
        print("check_smoke_linux: FAIL:", file=sys.stderr)
        for name, err in bad:
            print("check_smoke_linux: FAIL: [%s] %s" % (name, err),
                  file=sys.stderr)
        return 1
    print("check_smoke_linux: PASS: version + help + invalid-flag + "
          "snippet + hello.js outputs match")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
