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
  qemu_target_signals(text): signal numbers of every qemu
      `uncaught target signal N (` occurrence in the joined text.
  is_engine_init_abort(rc, text): the shared qemu-signature-only
      BLOCKED classifier for the Linux engine-init abort (see below).

Both runners own a private POSIX process group and raise subprocess.TimeoutExpired
after killing/reaping the child and terminating its descendants. The
deadline includes draining inherited pipes, even after the parent exits.
Interrupts also terminate the group before callers clean up their work dirs.
Timeouts are ceilings far above observed runtimes, not expectations.
"""

import os
import re
import signal
import subprocess
import sys
import threading
import time

# Generous ceilings: ch/otool/vtool/nm probes answer in seconds, the
# native gate finishes in ~1-2 min, the regression suite in ~10 min.
CAPTURE_TIMEOUT = 600
TEE_TIMEOUT = 3600
# Post-kill pipe-drain ceiling: after the group is killed the pump
# should see EOF promptly; a daemon that escaped the process group and
# still holds the pipe open would otherwise hang the join forever.
PUMP_DRAIN_TIMEOUT = 30
# Outer ceiling over runtests.py as a whole; runtests.py's own
# per-test timeout (default 60s) is untouched.
REGRESSION_TIMEOUT = 5400


_QEMU_TARGET_SIGNAL_RE = re.compile(r"uncaught target signal (\d+) \(")


def qemu_target_signals(text):
    """Signal numbers of every qemu `uncaught target signal N (` hit.

    Occurrence-based over the joined text (one string, or a line
    list): one physical line carrying two occurrences yields two
    numbers, so a mixed death cannot hide behind per-line matching.
    The captured digits are the whole number (the trailing ` (`
    anchors it), so signal 61 never reads as signal 6.
    """
    joined = text if isinstance(text, str) else "".join(text)
    return [int(n) for n in _QEMU_TARGET_SIGNAL_RE.findall(joined)]


def is_engine_init_abort(rc, text):
    """True only for an observed qemu guest SIGABRT (qemu-signature-only).

    B4.3 (doc/b4-linux.md section 8): under qemu-user every ch
    invocation aborts in engine init (SIGABRT before main), and qemu
    itself prints an `uncaught target signal 6 (` line into the
    (stderr-merged) output. BLOCKED means exactly that observation:
    nonzero rc, at least one signal-6 occurrence and zero
    occurrences of any other qemu-phrased signal number (so a mixed
    signal-6 + signal-11 log grades FAIL, on separate lines or on
    one line carrying both), and no " : PASS" line (any pass means
    something executed past init, so the run grades on its merits
    instead). A bare shell "Aborted (core dumped)", a lone other
    qemu signal (e.g. 11), or a native abort with no qemu signal-6
    occurrence is NOT this predicate and grades FAIL. Residual
    limitation: a co-occurring death with no qemu signal line at
    all (e.g. a bare "Segmentation fault (core dumped)") carries
    no signal number to veto on and is not detectable here. text
    is one string (check_native) or a line list
    (verify_bytecode_pure).
    """
    if rc == 0:
        return False
    joined = text if isinstance(text, str) else "".join(text)
    if " : PASS" in joined:
        return False
    signals = qemu_target_signals(joined)
    return bool(signals) and all(n == 6 for n in signals)


def _kill_group(proc):
    """Only this helper's freshly created process group is ours to kill."""
    if os.name == "posix":
        # Kill the group only while the leader is unreaped: killpg on a
        # zombie leader is safe (its PID is still reserved), but once
        # the leader is reaped the PID can be recycled and killpg could
        # hit an unrelated process group. Check the returncode attribute,
        # not poll(): poll() would reap a zombie leader as a side effect
        # and wrongly skip the descendant kill the timeout path relies on.
        if proc.returncode is None:
            try:
                # The group can still contain descendants after proc has exited.
                os.killpg(proc.pid, signal.SIGKILL)
            except (ProcessLookupError, PermissionError):
                # EPERM beside ESRCH: on Darwin, killpg on a zombie-only
                # group raises EPERM rather than ESRCH. Swallowing it is
                # sound ONLY because the unreaped-PID guard above rules out
                # a foreign group — the leader's reserved PID proves the
                # group is (or was) ours, so there is nothing else to kill.
                pass
    else:
        proc.kill()
    proc.wait()


def run_capture(cmd, timeout=CAPTURE_TIMEOUT, cwd=None, merge_stderr=False):
    """Run cmd with captured stdio, return (rc, out, err)."""
    with subprocess.Popen(cmd, stdout=subprocess.PIPE,
                          stderr=(subprocess.STDOUT if merge_stderr else subprocess.PIPE),
                          cwd=cwd,
                          start_new_session=(os.name == "posix"),
                          universal_newlines=True, errors="replace") as proc:
        try:
            out, err = proc.communicate(timeout=timeout)
        except BaseException:
            _kill_group(proc)
            proc.communicate()
            raise
        return proc.returncode, out, err or ""


def run_tee(cmd, cwd=None, timeout=TEE_TIMEOUT):
    """Run cmd, tee combined output to stdout, return (rc, lines)."""
    proc = subprocess.Popen(
        cmd, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
        cwd=cwd, start_new_session=(os.name == "posix"),
        universal_newlines=True, errors="replace",
    )
    lines = []
    pump_errors = []
    deadline = time.monotonic() + timeout

    def _pump():
        try:
            for line in proc.stdout:
                sys.stdout.write(line)
                sys.stdout.flush()
                lines.append(line)
        except Exception as e:
            pump_errors.append(e)

    pump = threading.Thread(target=_pump, daemon=True)
    pump.start()
    try:
        # Keep the leader unreaped until its pipe is drained. Its reserved PID
        # prevents process-group identity reuse if descendants hold the pipe
        # open after the leader exits and the deadline triggers group cleanup.
        pump.join(timeout=max(0, deadline - time.monotonic()))
        if pump.is_alive():
            raise subprocess.TimeoutExpired(cmd, timeout,
                                            output="".join(lines))
        if pump_errors:
            raise pump_errors[0]
        rc = proc.wait(timeout=max(0, deadline - time.monotonic()))
    except BaseException as e:
        _kill_group(proc)
        # Bounded drain: a group-escaping daemon child holding the pipe
        # open would otherwise hang this join forever.
        pump.join(timeout=PUMP_DRAIN_TIMEOUT)
        if pump.is_alive():
            print("common: TIMEOUT draining child output after %gs: %s"
                  % (PUMP_DRAIN_TIMEOUT, " ".join(cmd)), file=sys.stderr)
            raise subprocess.TimeoutExpired(
                cmd, timeout, output="".join(lines)) from e
        if isinstance(e, subprocess.TimeoutExpired):
            print("common: TIMEOUT after %gs: %s"
                  % (timeout, " ".join(cmd)), file=sys.stderr)
        raise
    finally:
        # Skip close() while the pump is mid-read: BufferedReader.close()
        # blocks until EOF while another thread is blocked in read(), so
        # closing here would hang until the daemon holding the pipe exits.
        # fd ownership passes to the daemon pump thread; EOF/GC reclaims it.
        if not pump.is_alive():
            proc.stdout.close()
    return rc, lines
