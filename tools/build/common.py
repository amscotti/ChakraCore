#!/usr/bin/env python3
#-------------------------------------------------------------------------------------------------------
# Copyright (c) ChakraCore Project Contributors. All rights reserved.
# Licensed under the MIT license. See LICENSE.txt file in the project root for full license information.
#-------------------------------------------------------------------------------------------------------
"""Shared subprocess helpers for the B2.3 check_* gates.

Single implementation of the run()/tee helpers previously copy-pasted
across check_regression.py, check_native.py, check_smoke.py and
check_metadata.py — same behavior, plus generous timeouts so a hung
child fails loudly instead of hanging the gate forever:

  run_capture(cmd, timeout=CAPTURE_TIMEOUT): capture stdout/stderr via
      subprocess.run, return (rc, out, err).
  run_tee(cmd, cwd=None, timeout=TEE_TIMEOUT): stream combined output
      live to stdout while collecting it, return (rc, lines).

Both raise subprocess.TimeoutExpired (after killing the child) on a
timeout; callers convert that into a loud FAIL verdict. Timeouts are
ceilings far above observed runtimes, not expectations.
"""

import subprocess
import sys
import threading

# Generous ceilings: ch/otool/vtool/nm probes answer in seconds, the
# native gate finishes in ~1-2 min, the regression suite in ~10 min.
CAPTURE_TIMEOUT = 600
TEE_TIMEOUT = 3600
# Outer ceiling over runtests.py as a whole; runtests.py's own
# per-test timeout (default 60s) is untouched.
REGRESSION_TIMEOUT = 5400


def run_capture(cmd, timeout=CAPTURE_TIMEOUT):
    """Run cmd with captured stdio, return (rc, out, err)."""
    proc = subprocess.run(cmd, stdout=subprocess.PIPE,
                          stderr=subprocess.PIPE, timeout=timeout,
                          universal_newlines=True, errors="replace")
    return proc.returncode, proc.stdout, proc.stderr


def run_tee(cmd, cwd=None, timeout=TEE_TIMEOUT):
    """Run cmd, tee combined output to stdout, return (rc, lines)."""
    proc = subprocess.Popen(
        cmd, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
        cwd=cwd, universal_newlines=True, errors="replace",
    )
    lines = []

    def _pump():
        try:
            for line in proc.stdout:
                sys.stdout.write(line)
                sys.stdout.flush()
                lines.append(line)
        except ValueError:
            pass  # stdout closed by the timeout kill below

    pump = threading.Thread(target=_pump, daemon=True)
    pump.start()
    try:
        rc = proc.wait(timeout=timeout)
    except subprocess.TimeoutExpired:
        proc.kill()
        pump.join(timeout=30)
        print("common: TIMEOUT after %ds: %s"
              % (timeout, " ".join(cmd)), file=sys.stderr)
        raise
    pump.join()
    return rc, lines
