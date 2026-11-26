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
       [--release] [--self-test]
Exit code: 0 iff all active legs observe initialization + repeated heap walks.
"""

import errno
import os
import queue
import selectors
import signal
import subprocess
import sys
import tempfile
import threading
import time

try:
    import pty
except ImportError:  # pragma: no cover - POSIX hosts always provide pty
    pty = None

_KILL_SIG = getattr(signal, "SIGKILL", signal.SIGTERM)

DEFAULT_TIMEOUT = 15.0
DEFAULT_MIN_WALKS = 2
# Non-release flag present in ConfigFlagsList.h iff ENABLE_DEBUG_CONFIG_OPTIONS
# is compiled in (encoded as UTF-16LE _u(...) in ConfigFlagsTable.cpp).
_DEBUG_CONFIG_MARKER_UTF16LE = "ForceBlockingConcurrentCollect".encode("utf-16-le")
_NATIVE_MAGICS = (
    b"\x7fELF",
    b"\xcf\xfa\xed\xfe",
    b"\xce\xfa\xed\xfe",
    b"\xfe\xed\xfa\xcf",
    b"\xfe\xed\xfa\xce",
    b"\xca\xfe\xba\xbe",
)

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


def _kill_process_group(proc, sig=signal.SIGTERM, wait_s=2.0, force=False):
    """Terminate proc's process group cleanly, escalating to _KILL_SIG / kill()."""
    if proc.poll() is not None:
        return
    if force:
        if os.name == "posix":
            try:
                os.killpg(proc.pid, _KILL_SIG)
            except (ProcessLookupError, PermissionError, OSError):
                pass
        else:
            try:
                proc.kill()
            except OSError:
                pass
        try:
            proc.wait(timeout=wait_s)
        except subprocess.TimeoutExpired:
            pass
        return

    if os.name == "posix":
        try:
            os.killpg(proc.pid, sig)
        except (ProcessLookupError, PermissionError, OSError):
            pass
    else:
        try:
            proc.terminate()
        except OSError:
            pass
    deadline = time.monotonic() + wait_s
    while proc.poll() is None and time.monotonic() < deadline:
        time.sleep(0.05)
    if proc.poll() is None:
        if os.name == "posix":
            try:
                os.killpg(proc.pid, _KILL_SIG)
            except (ProcessLookupError, PermissionError, OSError):
                pass
        else:
            try:
                proc.kill()
            except OSError:
                pass
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


def _pump_pipe(stream, out_q):
    """Background daemon reader feeding raw pipe chunks into out_q."""
    try:
        while True:
            chunk = stream.read1(4096) if hasattr(stream, "read1") else stream.read(4096)
            if not chunk:
                break
            out_q.put(("data", chunk))
    except OSError as e:
        out_q.put(("error", e))
    finally:
        out_q.put(("eof", None))


def run_gcstress_leg(label, cmd, timeout=DEFAULT_TIMEOUT,
                     min_walks=DEFAULT_MIN_WALKS, use_pty=None):
    """Run one bounded GCStress leg. Returns (ok, detail, lines)."""
    if min_walks < 1:
        return (False, "min_walks must be >= 1, got %d" % min_walks, [])
    master_fd = None
    slave_fd = None
    want_pty = (pty is not None) if use_pty is None else (use_pty and pty is not None)
    if want_pty:
        try:
            master_fd, slave_fd = pty.openpty()
        except OSError:
            master_fd, slave_fd = None, None
    if master_fd is not None:
        stdout_arg = slave_fd
        stderr_arg = slave_fd
        popen_bufsize = -1
    else:
        stdout_arg = subprocess.PIPE
        stderr_arg = subprocess.STDOUT
        popen_bufsize = 0

    proc = None
    sel = None
    reader = None
    out_q = None
    raw_buf = bytearray()
    lines = []
    init_seen = False
    walk_starts = 0
    walk_finishes = 0
    deadline = time.monotonic() + timeout
    t0 = time.monotonic()

    def process_bytes(data):
        nonlocal raw_buf, init_seen, walk_starts, walk_finishes
        if not data:
            return None
        raw_buf.extend(data)
        text = raw_buf.decode("utf-8", errors="replace")
        text = text.replace("\r\n", "\n").replace("\r", "\n")
        if "\n" not in text:
            return None
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
        return find_error_marker(new_lines)

    try:
        proc = subprocess.Popen(
            cmd,
            stdin=subprocess.DEVNULL,
            stdout=stdout_arg,
            stderr=stderr_arg,
            bufsize=popen_bufsize,
            start_new_session=(os.name == "posix"),
            close_fds=True,
        )
        if slave_fd is not None:
            os.close(slave_fd)
            slave_fd = None

        if master_fd is not None:
            os.set_blocking(master_fd, False)
            sel = selectors.DefaultSelector()
            sel.register(master_fd, selectors.EVENT_READ)
        else:
            out_q = queue.Queue()
            reader = threading.Thread(
                target=_pump_pipe, args=(proc.stdout, out_q), daemon=True)
            reader.start()

        eof = False
        while True:
            now = time.monotonic()
            remaining = deadline - now
            if remaining <= 0:
                _kill_process_group(proc, force=True)
                return (
                    False,
                    "[%s] timed out after %.1fs (init=%s starts=%d "
                    "finishes=%d/%d)"
                    % (label, timeout, init_seen, walk_starts,
                       walk_finishes, min_walks),
                    lines,
                )

            if master_fd is not None:
                ready = sel.select(timeout=min(remaining, 0.2))
                if ready:
                    chunks = []
                    while True:
                        try:
                            chunk = os.read(master_fd, 4096)
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
                        chunks.append(chunk)
                    err = process_bytes(b"".join(chunks))
                    if err is not None:
                        _kill_process_group(proc, force=True)
                        return (
                            False,
                            "[%s] error marker observed: %s" % (label, err),
                            lines,
                        )
            else:
                chunks = []
                try:
                    kind, payload = out_q.get(timeout=min(remaining, 0.2))
                    if kind == "data":
                        chunks.append(payload)
                    elif kind in ("eof", "error"):
                        eof = True
                except queue.Empty:
                    pass
                while True:
                    try:
                        kind, payload = out_q.get_nowait()
                    except queue.Empty:
                        break
                    if kind == "data":
                        chunks.append(payload)
                    elif kind in ("eof", "error"):
                        eof = True
                if chunks:
                    err = process_bytes(b"".join(chunks))
                    if err is not None:
                        _kill_process_group(proc, force=True)
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
                tail = b""
                if master_fd is not None:
                    tail = _drain_fd(master_fd)
                else:
                    reader.join(timeout=1.0)
                    tail_chunks = []
                    while True:
                        try:
                            kind, payload = out_q.get_nowait()
                        except queue.Empty:
                            break
                        if kind == "data":
                            tail_chunks.append(payload)
                    tail = b"".join(tail_chunks)
                if tail or raw_buf:
                    tail_bytes = bytes(raw_buf) + tail
                    raw_buf.clear()
                    tail_text = tail_bytes.decode("utf-8", errors="replace")
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
                if master_fd is None and not eof:
                    reader.join(timeout=1.0)
                    rem_chunks = []
                    while True:
                        try:
                            kind, payload = out_q.get_nowait()
                        except queue.Empty:
                            break
                        if kind == "data":
                            rem_chunks.append(payload)
                    if rem_chunks:
                        process_bytes(b"".join(rem_chunks))
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
        if sel is not None:
            sel.close()
        if proc is not None and proc.poll() is None:
            _kill_process_group(proc, force=True)
        if reader is not None:
            reader.join(timeout=2.0)
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
            if reader is None or not reader.is_alive():
                try:
                    proc.stdout.close()
                except OSError:
                    pass


def is_release_binary(gcstress_path):
    """Return True iff gcstress_path is a Release binary lacking debug flags.

    Checks the sibling install prefix `.profile` marker first (`release-*`),
    then falls back to checking compiled native executables (ELF/Mach-O/PE)
    for the UTF-16LE `ENABLE_DEBUG_CONFIG_OPTIONS` flag table marker.
    """
    abs_bin = os.path.abspath(gcstress_path)
    bin_dir = os.path.dirname(abs_bin)
    prefix_dir = (os.path.dirname(bin_dir)
                  if os.path.basename(bin_dir) == "bin" else bin_dir)
    profile_marker = os.path.join(prefix_dir, ".profile")
    if os.path.isfile(profile_marker):
        try:
            with open(profile_marker, "r", encoding="utf-8",
                      errors="replace") as f:
                profile = f.read().strip()
            if profile:
                return profile.startswith("release")
        except OSError:
            pass

    if not os.path.isfile(abs_bin):
        return False
    try:
        with open(abs_bin, "rb") as f:
            head = f.read(4)
            is_native = (head in _NATIVE_MAGICS) or (head[:2] == b"MZ")
            if not is_native:
                return False
            data = head + f.read()
    except OSError:
        return False
    return _DEBUG_CONFIG_MARKER_UTF16LE not in data


def build_legs(gcstress_path, release=None):
    """Return [(label, argv), ...] for GCStress configurations.

    Release builds omit ENABLE_DEBUG_CONFIG_OPTIONS so `-off:ConcurrentCollect`
    is an Invalid Flag; run only the default concurrent mode there.
    """
    abs_bin = os.path.abspath(gcstress_path)
    if release is None:
        release = is_release_binary(abs_bin)
    if release:
        return [("concurrent", [abs_bin, "-js"])]
    return [
        ("in-thread", [abs_bin, "-js", "-off:ConcurrentCollect"]),
        ("concurrent", [abs_bin, "-js"]),
    ]


def self_test():
    """Positive and negative controls on both PTY (POSIX) and pipe modes."""
    bad = 0

    def report(name, cond, detail=""):
        print("self-test %-46s -> %s%s"
              % (name, "PASS" if cond else "FAIL",
                 (" (" + detail + ")") if detail else ""))
        return 0 if cond else 1

    with tempfile.TemporaryDirectory(prefix="check-gcstress-selftest-") as tmp:
        def write_stub(name, body):
            p = os.path.join(tmp, name)
            with open(p, "w", encoding="utf-8") as f:
                f.write(body)
            return p

        good = write_stub(
            "good.py",
            "import signal, sys, time\n"
            "signal.signal(signal.SIGTERM, lambda *a: sys.exit(0))\n"
            "print('Recycler created, initializing heap...', flush=True)\n"
            "print('Initialization complete', flush=True)\n"
            "for g in range(1, 5):\n"
            "    print('Full heap walk starting. Current generation: %12d' % g, flush=True)\n"
            "    print('Full heap walk finished', flush=True)\n"
            "while True:\n"
            "    time.sleep(0.05)\n",
        )
        early_rc0 = write_stub(
            "early0.py",
            "import sys\n"
            "print('Initialization complete', flush=True)\n"
            "print('Full heap walk starting. Current generation: 2', flush=True)\n"
            "print('Full heap walk finished', flush=True)\n"
            "sys.exit(0)\n",
        )
        exit_after_walks = write_stub(
            "exitafter.py",
            "import sys\n"
            "print('Initialization complete', flush=True)\n"
            "print('Full heap walk starting. Current generation: 2', flush=True)\n"
            "print('Full heap walk finished', flush=True)\n"
            "print('Full heap walk starting. Current generation: 3', flush=True)\n"
            "print('Full heap walk finished', flush=True)\n"
            "sys.exit(0)\n",
        )
        crash = write_stub(
            "crash.py",
            "import os, signal, sys\n"
            "print('Initialization complete', flush=True)\n"
            "sig = getattr(signal, 'SIGUSR1', None)\n"
            "if sig is not None:\n"
            "    os.kill(os.getpid(), sig)\n"
            "sys.exit(139)\n",
        )
        fail_marker = write_stub(
            "marker.py",
            "import time\n"
            "print('Initialization complete', flush=True)\n"
            "print(\"==== FAILURE: 'ok' evaluated to false.\", flush=True)\n"
            "while True:\n"
            "    time.sleep(0.05)\n",
        )
        invalid_flag = write_stub(
            "invflag.py",
            "import time\n"
            "print('Invalid Flag : -off:ConcurrentCollect', flush=True)\n"
            "print('Initialization complete', flush=True)\n"
            "print('Full heap walk starting. Current generation: 2', flush=True)\n"
            "print('Full heap walk finished', flush=True)\n"
            "print('Full heap walk starting. Current generation: 3', flush=True)\n"
            "print('Full heap walk finished', flush=True)\n"
            "while True:\n"
            "    time.sleep(0.05)\n",
        )
        hang = write_stub(
            "hang.py",
            "import time\n"
            "print('Initialization complete', flush=True)\n"
            "print('Full heap walk starting. Current generation: 2', flush=True)\n"
            "print('Full heap walk finished', flush=True)\n"
            "while True:\n"
            "    time.sleep(0.2)\n",
        )

        modes = [("pty", None), ("pipe", False)] if pty is not None else [("pipe", False)]
        for mode_name, use_pty in modes:
            ok, detail, _ = run_gcstress_leg(
                "good-" + mode_name, [sys.executable, good],
                timeout=5.0, min_walks=2, use_pty=use_pty)
            bad += report("[%s] live loop init + 2 walks passes" % mode_name, ok, detail)

            ok, detail, _ = run_gcstress_leg(
                "early0-" + mode_name, [sys.executable, early_rc0],
                timeout=5.0, min_walks=2, use_pty=use_pty)
            bad += report("[%s] early rc=0 exit before walks rejected" % mode_name, not ok, detail)

            ok, detail, _ = run_gcstress_leg(
                "exitafter-" + mode_name, [sys.executable, exit_after_walks],
                timeout=5.0, min_walks=2, use_pty=use_pty)
            bad += report("[%s] early rc=0 exit after walks rejected" % mode_name, not ok, detail)

            ok, detail, _ = run_gcstress_leg(
                "crash-" + mode_name, [sys.executable, crash],
                timeout=5.0, min_walks=2, use_pty=use_pty)
            bad += report("[%s] fatal crash/nonzero exit rejected" % mode_name, not ok, detail)

            ok, detail, _ = run_gcstress_leg(
                "marker-" + mode_name, [sys.executable, fail_marker],
                timeout=5.0, min_walks=2, use_pty=use_pty)
            bad += report("[%s] FAILURE marker rejected" % mode_name, not ok, detail)

            ok, detail, _ = run_gcstress_leg(
                "invflag-" + mode_name, [sys.executable, invalid_flag],
                timeout=5.0, min_walks=2, use_pty=use_pty)
            bad += report("[%s] Invalid Flag marker rejected" % mode_name, not ok, detail)

            t0 = time.monotonic()
            ok, detail, _ = run_gcstress_leg(
                "hang-" + mode_name, [sys.executable, hang],
                timeout=0.6, min_walks=2, use_pty=use_pty)
            dt = time.monotonic() - t0
            bad += report("[%s] hang before min_walks times out" % mode_name,
                          (not ok) and dt < 3.0, detail)

        fake_bin = os.path.join(tmp, "GCStress")
        legs = build_legs(fake_bin)
        bad += report(
            "build_legs covers both GCStress modes",
            legs == [
                ("in-thread", [os.path.abspath(fake_bin), "-js", "-off:ConcurrentCollect"]),
                ("concurrent", [os.path.abspath(fake_bin), "-js"]),
            ],
        )

        # Release-binary controls: .profile release-* or native binary without
        # debug flag table skips -off:ConcurrentCollect cleanly.
        rel_pfx = os.path.join(tmp, "rel_pfx")
        os.makedirs(os.path.join(rel_pfx, "bin"))
        rel_bin = os.path.join(rel_pfx, "bin", "GCStress")
        with open(rel_bin, "wb") as f:
            f.write(b"\xcf\xfa\xed\xfe" + b"\x00" * 64)
        with open(os.path.join(rel_pfx, ".profile"), "w", encoding="utf-8") as f:
            f.write("release-shared\n")
        bad += report(
            "release .profile skips -off:ConcurrentCollect",
            is_release_binary(rel_bin)
            and build_legs(rel_bin) == [("concurrent", [os.path.abspath(rel_bin), "-js"])],
        )

        dbg_pfx = os.path.join(tmp, "dbg_pfx")
        os.makedirs(os.path.join(dbg_pfx, "bin"))
        dbg_bin = os.path.join(dbg_pfx, "bin", "GCStress")
        with open(dbg_bin, "wb") as f:
            f.write(b"\x7fELF" + _DEBUG_CONFIG_MARKER_UTF16LE)
        with open(os.path.join(dbg_pfx, ".profile"), "w", encoding="utf-8") as f:
            f.write("test\n")
        bad += report(
            "non-release .profile keeps both GCStress legs",
            (not is_release_binary(dbg_bin))
            and len(build_legs(dbg_bin)) == 2,
        )

        standalone_rel = os.path.join(tmp, "standalone_rel")
        with open(standalone_rel, "wb") as f:
            f.write(b"\x7fELF" + b"\x00" * 64)
        standalone_dbg = os.path.join(tmp, "standalone_dbg")
        with open(standalone_dbg, "wb") as f:
            f.write(b"\x7fELF" + _DEBUG_CONFIG_MARKER_UTF16LE)
        bad += report(
            "native binary without .profile detects debug vs release",
            is_release_binary(standalone_rel)
            and not is_release_binary(standalone_dbg),
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
    force_release = None
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
        elif args[i] == "--release":
            force_release = True
            i += 1
        else:
            rest.append(args[i])
            i += 1

    if len(rest) != 1:
        print("usage: check_gcstress.py <GCStress> [--timeout SECS] "
              "[--min-walks N] [--release] [--self-test]", file=sys.stderr)
        return 2

    gcstress = rest[0]
    if not os.path.isfile(gcstress):
        print("check_gcstress: missing binary: %s" % gcstress, file=sys.stderr)
        return 2

    legs = build_legs(gcstress, release=force_release)
    for label, cmd in legs:
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

    if len(legs) == 1:
        print("check_gcstress: PASS (concurrent bounded heap walks; "
              "release build skips -off:ConcurrentCollect)")
    else:
        print("check_gcstress: PASS (in-thread + concurrent bounded heap walks)")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
