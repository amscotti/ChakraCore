#!/usr/bin/env python3
#-------------------------------------------------------------------------------------------------------
# Copyright (c) ChakraCore Project Contributors. All rights reserved.
# Licensed under the MIT license. See LICENSE.txt file in the project root for full license information.
#-------------------------------------------------------------------------------------------------------
"""Bounded liveness and heap-walk gate for bin/GCStress (open-cards.md:255).

Runs bounded (default 15 s) checks of both:
  1. `GCStress -js -off:ConcurrentCollect` (in-thread GC mode)
  2. `GCStress -js` (concurrent GC mode)

Each leg attaches a POSIX PTY (falling back to a pipe if PTYs are
unavailable) so GCStress's `wprintf` output arrives line-buffered in real
time without losing the last partial 4 KiB stdio block upon termination.
The leg succeeds as soon as `Initialization complete` and at least
`--min-walks` (default 2) complete heap walks (`Full heap walk starting.`
and `Full heap walk finished`) are observed with zero error/assert/
sanitizer markers, then terminates the child process group cleanly. Any
early exit (including exit 0 -- GCStress loops forever by design),
crash/signal, failure marker, or timeout before the required output is
observed fails loudly.

Usage: check_gcstress.py <GCStress> [--timeout SECS] [--min-walks N]
       [--self-test]
Exit code: 0 iff both legs observe initialization + repeated heap walks.
"""

import errno
import os
import selectors
import signal
import subprocess
import sys
import tempfile
import time

try:
    import pty
except ImportError:  # pragma: no cover - POSIX hosts always provide pty
    pty = None

DEFAULT_TIMEOUT = 15.0
DEFAULT_MIN_WALKS = 2

ERROR_MARKERS = (
    "==== FAILURE:",
    "==== Test completed.",
    "Invalid Flag",
    "Error: OOM",
    "ASSERTION",
    "Assertion failed",
    "AddressSanitizer",
    "UndefinedBehaviorSanitizer",
    "ThreadSanitizer",
    "runtime error:",
)


def find_error_marker(lines):
    """Return the first failure/assert/sanitizer marker line, or None."""
    for line in lines:
        for marker in ERROR_MARKERS:
            if marker in line:
                return "%s (%s)" % (marker, line.strip()[:160])
    return None


def _kill_process_group(proc, sig=signal.SIGTERM, wait_s=2.0):
    """Terminate proc's process group cleanly, escalating to SIGKILL."""
    if proc.poll() is not None:
        return
    if os.name == "posix":
        try:
            os.killpg(proc.pid, sig)
        except (ProcessLookupError, PermissionError):
            pass
    else:
        proc.terminate()
    deadline = time.monotonic() + wait_s
    while proc.poll() is None and time.monotonic() < deadline:
        time.sleep(0.05)
    if proc.poll() is None:
        if os.name == "posix":
            try:
                os.killpg(proc.pid, signal.SIGKILL)
            except (ProcessLookupError, PermissionError):
                pass
        else:
            proc.kill()
        proc.wait()


def _drain_fd(fd):
    """Best-effort non-blocking drain of remaining bytes from fd."""
    chunks = []
    for _ in range(32):
        try:
            data = os.read(fd, 4096)
        except BlockingIOError:
            break
        except OSError:
            break
        if not data:
            break
        chunks.append(data)
    return b"".join(chunks)


def run_gcstress_leg(label, cmd, timeout=DEFAULT_TIMEOUT,
                     min_walks=DEFAULT_MIN_WALKS):
    """Run one bounded GCStress leg. Returns (ok, detail, lines)."""
    if min_walks < 1:
        return (False, "min_walks must be >= 1, got %d" % min_walks, [])
    master_fd = None
    slave_fd = None
    if pty is not None:
        try:
            master_fd, slave_fd = pty.openpty()
        except OSError:
            master_fd, slave_fd = None, None
    if master_fd is not None:
        stdout_arg = slave_fd
        stderr_arg = slave_fd
    else:
        stdout_arg = subprocess.PIPE
        stderr_arg = subprocess.STDOUT

    proc = None
    sel = selectors.DefaultSelector()
    raw_buf = bytearray()
    lines = []
    init_seen = False
    walk_starts = 0
    walk_finishes = 0
    deadline = time.monotonic() + timeout
    t0 = time.monotonic()

    try:
        proc = subprocess.Popen(
            cmd,
            stdin=subprocess.DEVNULL,
            stdout=stdout_arg,
            stderr=stderr_arg,
            start_new_session=(os.name == "posix"),
            close_fds=True,
        )
        if slave_fd is not None:
            os.close(slave_fd)
            slave_fd = None
        read_fd = master_fd if master_fd is not None else proc.stdout.fileno()
        os.set_blocking(read_fd, False)
        sel.register(read_fd, selectors.EVENT_READ)

        eof = False
        while True:
            now = time.monotonic()
            remaining = deadline - now
            if remaining <= 0:
                _kill_process_group(proc, sig=signal.SIGKILL)
                return (
                    False,
                    "[%s] timed out after %.1fs (init=%s starts=%d "
                    "finishes=%d/%d)"
                    % (label, timeout, init_seen, walk_starts,
                       walk_finishes, min_walks),
                    lines,
                )

            ready = sel.select(timeout=min(remaining, 0.2))
            if ready:
                while True:
                    try:
                        chunk = os.read(read_fd, 4096)
                    except BlockingIOError:
                        break
                    except OSError as e:
                        if e.errno == errno.EIO:
                            eof = True
                            break
                        raise
                    if not chunk:
                        eof = True
                        break
                    raw_buf.extend(chunk)

                # Split complete lines, normalizing PTY CR+LF.
                text = raw_buf.decode("utf-8", errors="replace")
                text = text.replace("\r\n", "\n").replace("\r", "\n")
                if "\n" in text:
                    parts = text.split("\n")
                    new_lines = parts[:-1]
                    raw_buf = bytearray(parts[-1].encode("utf-8"))
                    for line in new_lines:
                        lines.append(line)
                        if "Initialization complete" in line:
                            init_seen = True
                        if "Full heap walk starting." in line:
                            walk_starts += 1
                        if "Full heap walk finished" in line:
                            walk_finishes += 1
                    err = find_error_marker(new_lines)
                    if err is not None:
                        _kill_process_group(proc, sig=signal.SIGKILL)
                        return (
                            False,
                            "[%s] error marker observed: %s" % (label, err),
                            lines,
                        )

            if init_seen and walk_starts >= min_walks and walk_finishes >= min_walks:
                # Confirm the infinite stress loop is still alive (not tearing
                # down right after printing) before we stop it cleanly.
                time.sleep(0.05)
                if proc.poll() is not None:
                    rc = proc.returncode
                    return (
                        False,
                        "[%s] exited prematurely (rc=%d) after %d walks"
                        % (label, rc, walk_finishes),
                        lines,
                    )
                _kill_process_group(proc, sig=signal.SIGTERM)
                tail = _drain_fd(read_fd)
                if tail:
                    tail_text = tail.decode("utf-8", errors="replace")
                    tail_text = tail_text.replace("\r\n", "\n").replace("\r", "\n")
                    tail_lines = [ln for ln in tail_text.splitlines() if ln]
                    lines.extend(tail_lines)
                    err = find_error_marker(tail_lines)
                    if err is not None:
                        return (
                            False,
                            "[%s] error marker during shutdown: %s"
                            % (label, err),
                            lines,
                        )
                elapsed = time.monotonic() - t0
                return (
                    True,
                    "[%s] PASS in %.2fs (init=True, walks=%d)"
                    % (label, elapsed, walk_finishes),
                    lines,
                )

            if eof or proc.poll() is not None:
                # Drain final incomplete line before failing.
                if raw_buf:
                    rem = raw_buf.decode("utf-8", errors="replace").strip()
                    if rem:
                        lines.append(rem)
                    raw_buf.clear()
                rc = proc.wait()
                err = find_error_marker(lines)
                suffix = (" (%s)" % err) if err else ""
                return (
                    False,
                    "[%s] exited early rc=%d before completing %d walks "
                    "(init=%s starts=%d finishes=%d)%s"
                    % (label, rc, min_walks, init_seen, walk_starts,
                       walk_finishes, suffix),
                    lines,
                )
    finally:
        sel.close()
        if proc is not None and proc.poll() is None:
            _kill_process_group(proc, sig=signal.SIGKILL)
        if slave_fd is not None:
            try:
                os.close(slave_fd)
            except OSError:
                pass
        if master_fd is not None:
            try:
                os.close(master_fd)
            except OSError:
                pass
        if proc is not None and proc.stdout is not None:
            try:
                proc.stdout.close()
            except OSError:
                pass


def build_legs(gcstress_path):
    """Return [(label, argv), ...] for both GCStress configurations."""
    abs_bin = os.path.abspath(gcstress_path)
    return [
        ("in-thread", [abs_bin, "-js", "-off:ConcurrentCollect"]),
        ("concurrent", [abs_bin, "-js"]),
    ]


def self_test():
    """Positive and negative controls using hermetic stub executables."""
    if os.name != "posix":
        print("check_gcstress self-test: skipped on non-POSIX")
        return 0
    bad = 0

    def report(name, cond, detail=""):
        print("self-test %-42s -> %s%s"
              % (name, "PASS" if cond else "FAIL",
                 (" (" + detail + ")") if detail else ""))
        return 0 if cond else 1

    with tempfile.TemporaryDirectory(prefix="check-gcstress-selftest-") as tmp:
        def write_stub(name, body):
            p = os.path.join(tmp, name)
            with open(p, "w", encoding="utf-8") as f:
                f.write("#!/usr/bin/env python3\n" + body)
            os.chmod(p, 0o755)
            return p

        good = write_stub(
            "good.py",
            "import signal, sys, time\n"
            "signal.signal(signal.SIGTERM, lambda *a: sys.exit(0))\n"
            "print('Recycler created, initializing heap...')\n"
            "print('Initialization complete')\n"
            "for g in range(1, 5):\n"
            "    print('Full heap walk starting. Current generation: %12d' % g)\n"
            "    print('Full heap walk finished')\n"
            "while True:\n"
            "    time.sleep(0.05)\n",
        )
        ok, detail, _ = run_gcstress_leg("good", [good], timeout=5.0, min_walks=2)
        bad += report("live loop init + 2 walks passes", ok, detail)

        early_rc0 = write_stub(
            "early0.py",
            "import sys\n"
            "print('Initialization complete')\n"
            "print('Full heap walk starting. Current generation: 2')\n"
            "print('Full heap walk finished')\n"
            "sys.exit(0)\n",
        )
        ok, detail, _ = run_gcstress_leg("early0", [early_rc0], timeout=5.0, min_walks=2)
        bad += report("early rc=0 exit before walks is rejected", not ok, detail)

        exit_after_walks = write_stub(
            "exitafter.py",
            "import sys\n"
            "print('Initialization complete')\n"
            "print('Full heap walk starting. Current generation: 2')\n"
            "print('Full heap walk finished')\n"
            "print('Full heap walk starting. Current generation: 3')\n"
            "print('Full heap walk finished')\n"
            "sys.exit(0)\n",
        )
        ok, detail, _ = run_gcstress_leg("exitafter", [exit_after_walks], timeout=5.0, min_walks=2)
        bad += report("early rc=0 exit after walks is rejected", not ok, detail)

        # Use SIGUSR1 rather than SIGSEGV so the stub dies with rc < 0 without
        # triggering macOS CrashReporter Python-*.ips logs / crash dialogs.
        crash = write_stub(
            "crash.py",
            "import os, signal\n"
            "print('Initialization complete', flush=True)\n"
            "os.kill(os.getpid(), signal.SIGUSR1)\n",
        )
        ok, detail, _ = run_gcstress_leg("crash", [crash], timeout=5.0, min_walks=2)
        bad += report("fatal signal SIGUSR1 is rejected", not ok, detail)

        fail_marker = write_stub(
            "marker.py",
            "import time\n"
            "print('Initialization complete')\n"
            "print(\"==== FAILURE: 'ok' evaluated to false.\")\n"
            "while True:\n"
            "    time.sleep(0.05)\n",
        )
        ok, detail, _ = run_gcstress_leg("marker", [fail_marker], timeout=5.0, min_walks=2)
        bad += report("FAILURE marker is rejected", not ok, detail)

        invalid_flag = write_stub(
            "invflag.py",
            "import time\n"
            "print('Invalid Flag : -off:ConcurrentCollect')\n"
            "print('Initialization complete')\n"
            "print('Full heap walk starting. Current generation: 2')\n"
            "print('Full heap walk finished')\n"
            "print('Full heap walk starting. Current generation: 3')\n"
            "print('Full heap walk finished')\n"
            "while True:\n"
            "    time.sleep(0.05)\n",
        )
        ok, detail, _ = run_gcstress_leg("invflag", [invalid_flag], timeout=5.0, min_walks=2)
        bad += report("Invalid Flag marker is rejected", not ok, detail)

        hang = write_stub(
            "hang.py",
            "import time\n"
            "print('Initialization complete')\n"
            "print('Full heap walk starting. Current generation: 2')\n"
            "print('Full heap walk finished')\n"
            "while True:\n"
            "    time.sleep(0.2)\n",
        )
        t0 = time.monotonic()
        ok, detail, _ = run_gcstress_leg("hang", [hang], timeout=0.6, min_walks=2)
        dt = time.monotonic() - t0
        bad += report("hang before min_walks times out", (not ok) and dt < 3.0, detail)

        legs = build_legs("/tmp/fake/bin/GCStress")
        bad += report(
            "build_legs covers both GCStress modes",
            legs == [
                ("in-thread", ["/tmp/fake/bin/GCStress", "-js", "-off:ConcurrentCollect"]),
                ("concurrent", ["/tmp/fake/bin/GCStress", "-js"]),
            ],
        )

    print("check_gcstress self-test: %s"
          % ("ALL NEGATIVE CONTROLS BEHAVE" if bad == 0
             else "%d CONTROL(S) MISBEHAVE" % bad))
    return 1 if bad else 0


def main(argv):
    args = list(argv[1:])
    if "--self-test" in args:
        return self_test()

    timeout = DEFAULT_TIMEOUT
    min_walks = DEFAULT_MIN_WALKS
    rest = []
    i = 0
    while i < len(args):
        if args[i] == "--timeout":
            if i + 1 >= len(args):
                print("check_gcstress: --timeout needs a value", file=sys.stderr)
                return 2
            timeout = float(args[i + 1])
            i += 2
        elif args[i] == "--min-walks":
            if i + 1 >= len(args):
                print("check_gcstress: --min-walks needs a value", file=sys.stderr)
                return 2
            min_walks = int(args[i + 1])
            i += 2
        else:
            rest.append(args[i])
            i += 1

    if len(rest) != 1:
        print("usage: check_gcstress.py <GCStress> [--timeout SECS] "
              "[--min-walks N] [--self-test]", file=sys.stderr)
        return 2

    gcstress = rest[0]
    if not os.path.isfile(gcstress):
        print("check_gcstress: missing binary: %s" % gcstress, file=sys.stderr)
        return 2

    for label, cmd in build_legs(gcstress):
        print("check_gcstress: running [%s] %s (timeout=%.1fs min_walks=%d)"
              % (label, " ".join(cmd), timeout, min_walks))
        sys.stdout.flush()
        ok, detail, lines = run_gcstress_leg(
            label, cmd, timeout=timeout, min_walks=min_walks)
        if not ok:
            print("check_gcstress: FAIL: %s" % detail, file=sys.stderr)
            if lines:
                print("check_gcstress: last 25 output lines:", file=sys.stderr)
                for ln in lines[-25:]:
                    print("  | " + ln, file=sys.stderr)
            return 1
        print("check_gcstress: %s" % detail)

    print("check_gcstress: PASS (in-thread + concurrent bounded heap walks)")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
