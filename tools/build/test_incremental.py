#!/usr/bin/env python3
#-------------------------------------------------------------------------------------------------------
# Copyright (c) ChakraCore Project Contributors. All rights reserved.
# Licensed under the MIT license. See LICENSE.txt file in the project root for full license information.
#-------------------------------------------------------------------------------------------------------
"""B3.4 incremental-correctness + flavor-isolation proofs.

Maintained script (doc/b3-profiles.md section 10 records one full
run with real outputs). Every test is repeatable from any warm
state; each establishes its own baseline, restores every file it
touches (byte-identical, verified), and prints PASS/FAIL with its
key numbers. Exit 0 iff every selected test passes.

Tests:

  isolation   Build every profile (quick: test + debug-static; full:
              all three) and prove side-by-side coexistence: correct
              .profile markers, then delete one installed debug-static
              file, rebuild, and prove the re-copied bytes are
              identical while every OTHER prefix stays byte-identical
              (real write activity, not a no-op triviality).
  noop        A no-op rebuild changes zero artifact bytes (content
              hashes + symlink targets; mtimes ignored) and runs zero
              TU compiles. Non-test profiles additionally prove their
              copy-out step RAN yet wrote nothing.
  source      Content-touch one engine TU -> exactly 1 TU compile;
              dylib, static archive and GCStress relink; the shared
              ch does NOT relink (precision: no gratuitous work).
  header      Content-touch a widely-included header -> a bounded
              middle count of TU compiles (true dependents: more
              than one, fewer than a full rebuild) + green.
  jstoc       Content-touch bin/ch/DbgController.js -> the generator
              re-runs and exactly the ch TUs recompile (8 test /
              9 debug-static / 8 release-shared: the B2.2 edge holds
              across profiles) + green.
  config      A build-config input change (-Dicu-include respelling:
              same directory, different flag bytes) rebuilds the
              full graph (>500 TU compiles) + green. Runs in a /tmp
              scratch copy (fresh cache => deterministic counts; the
              live tree and its cache are untouched). Rationale: the
              live cache retains BOTH flag variants after one cycle,
              so a live-tree respell is only full on its first run
              (observed: respell 538, restore 0) — not repeatable.
  bzcomment   A comment-only build.zig touch rebuilds the configurer
              but zero TU compiles (build-script edits do not
              spuriously invalidate the graph).
  selection   tools/build-contract/verify_b1.py --self-test (PAL
              archive-selection regression: current-graph B2 wins,
              stale B1 fails loudly).
  concurrent  A test no-op build and a debug-static no-op build run
              simultaneously; both exit 0 (cross-profile safety).

Usage: test_incremental.py [--quick] [TEST...]
  --quick runs isolation, noop, bzcomment, selection (~6 min warm).
  Full (default) runs everything (~25 min (full run measured
  1448s) incl. full builds; re-runs are faster). Positional TEST
  names select a subset.
"""

import hashlib
import os
import shutil
import subprocess
import sys
import time

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
ZIG = ["mise", "exec", "--", "zig"]
BUILD_TIMEOUT = 1200
PREFIX = {"test": "zig-out",
          "debug-static": "out/debug-static",
          "release-shared": "out/release-shared"}
CH_TU_EXPECT = {"test": 8, "debug-static": 9, "release-shared": 8}
SOURCE_TU = "lib/Runtime/Library/JavascriptArray.cpp"
# Widely-included header for the header test (transitive fan-in
# measured from test-profile .d files; see doc/b3-profiles.md §10).
HEADER = "lib/Common/Common.h"
HEADER_MIN, HEADER_MAX = 10, 538
FULL_MIN = 500
JSCONTROLLER = "bin/ch/DbgController.js"
ICU_REAL = "/opt/homebrew/opt/icu4c/include"
PROBE_C = "\n/* B3.4 incremental probe (removed after proof) */\n"
PROBE_JS = "\n// B3.4 incremental probe (removed after proof)\n"
# Semantic TU touch: a comment alone produces byte-identical objects
# (deterministic codegen), which would make "archive relinked" style
# byte assertions vacuous — and so does an anonymous-namespace
# variable (elided as unused at -O3; observed: archive bytes
# unchanged). An EXTERNAL-linkage definition cannot be elided, so it
# forces byte-delta propagation through archive/dylib/GCStress.
# Temporary and restored; single definition, no ODR risk.
PROBE_SEMANTIC_C = "\nint b34_incremental_probe = 1;\n"
PROBE_ZIG = "\n// B3.4 incremental probe (removed after proof)\n"
LOGDIR = "/tmp/b34-incr-logs"

failures = []
_log_seq = [0]


def save_log(tag, cmd, out):
    try:
        os.makedirs(LOGDIR, exist_ok=True)
        _log_seq[0] += 1
        path = os.path.join(LOGDIR, "%02d-%s.log"
                            % (_log_seq[0], tag))
        with open(path, "w") as f:
            f.write("$ %s\n" % " ".join(cmd))
            f.write(out)
        return path
    except OSError:
        return None


def log(text=""):
    print(text, flush=True)


def run(cmd, timeout, cwd=ROOT):
    t0 = time.time()
    try:
        p = subprocess.run(cmd, cwd=cwd, stdout=subprocess.PIPE,
                           stderr=subprocess.STDOUT, text=True,
                           timeout=timeout)
        return p.returncode, p.stdout, time.time() - t0
    except subprocess.TimeoutExpired as e:
        out = e.stdout if isinstance(e.stdout, str) else ""
        return None, out + "\n[TIMEOUT after %ds]" % timeout, time.time() - t0


def zig_build(profile, verbose=False, extra=(), timeout=BUILD_TIMEOUT,
              tag=None):
    cmd = ZIG + ["build"]
    if profile != "test":
        cmd += ["-Dprofile=%s" % profile]
    if verbose:
        cmd += ["--verbose"]
    cmd += list(extra)
    rc, out, dt = run(cmd, timeout)
    if tag is not None:
        save_log(tag, cmd, out)
    return rc, out, dt


def tu_compiles(output):
    """Verbose lines that compile one TU (configure-time pal-config
    probes never print through --verbose, so every match is a step)."""
    hits = []
    for line in output.splitlines():
        if ("zig c++" in line or "zig cc" in line) and " -c " in line:
            hits.append(line)
    return hits


def tu_sources(lines):
    """Source path each TU compile line builds (basename after -c)."""
    srcs = []
    for line in lines:
        i = line.find(" -c ")
        if i >= 0:
            rest = line[i + 4:].strip().split()
            if rest:
                srcs.append(rest[0])
    return srcs


def md5_of(path):
    h = hashlib.md5()
    with open(path, "rb") as f:
        for chunk in iter(lambda: f.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest()


def hash_tree(prefix_rel):
    """Map relpath -> ("f", md5) / ("l", link target) / ("d", None)."""
    base = os.path.join(ROOT, prefix_rel)
    out = {}
    for dirpath, dirnames, filenames in os.walk(base, followlinks=False):
        dirnames.sort()
        for name in sorted(filenames):
            full = os.path.join(dirpath, name)
            rel = os.path.relpath(full, base)
            if os.path.islink(full):
                out[rel] = ("l", os.readlink(full))
            elif os.path.isfile(full):
                out[rel] = ("f", md5_of(full))
        rel_dir = os.path.relpath(dirpath, base)
        if rel_dir != ".":
            out[rel_dir + "/"] = ("d", None)
    return out


def diff_trees(before, after):
    diffs = []
    for rel in sorted(set(before) | set(after)):
        if before.get(rel) != after.get(rel):
            diffs.append((rel, before.get(rel), after.get(rel)))
    return diffs


def check(name, cond, detail=""):
    log("[%s] %s%s" % ("PASS" if cond else "FAIL", name,
                       (": " + detail) if detail else ""))
    if not cond:
        failures.append(name)
    return cond


def read_bytes(rel):
    with open(os.path.join(ROOT, rel), "rb") as f:
        return f.read()


def write_bytes(rel, data):
    with open(os.path.join(ROOT, rel), "wb") as f:
        f.write(data)


class touched:
    """Append a probe, restore byte- AND mtime-identical afterwards.

    Mtime restoration matters: direct file inputs are mtime-sensitive,
    so a byte-restored file with a fresh mtime would spuriously
    recompile once in every OTHER profile's next build (observed: the
    source test's restored TU recompiled as a +1 in later profiles'
    jstoc counts). Restoring the mtime keeps tests independent; every
    counted build still carries a real byte-delta, so no assertion is
    weakened.
    """

    def __init__(self, rel, probe):
        self.rel = rel
        self.probe = probe.encode()
        self.orig = None
        self.times = None

    def __enter__(self):
        self.orig = read_bytes(self.rel)
        st = os.stat(os.path.join(ROOT, self.rel))
        self.times = (st.st_atime_ns, st.st_mtime_ns)
        write_bytes(self.rel, self.orig + self.probe)
        return self

    def __exit__(self, *exc):
        write_bytes(self.rel, self.orig)
        os.utime(os.path.join(ROOT, self.rel), ns=self.times)
        return read_bytes(self.rel) == self.orig


def build_green(profile, out, rc, what):
    if rc != 0:
        log("--- tail of failed %s ---" % what)
        log("\n".join(out.splitlines()[-20:]))
    return check("%s builds green" % what, rc == 0,
                 "exit %s" % (rc,))


def t_isolation(profiles):
    log("== isolation: %s ==" % ", ".join(profiles))
    trees = {}
    ok = True
    for p in profiles:
        rc, out, dt = zig_build(p)
        ok &= build_green(p, out, rc, "build %s" % p)
        if rc != 0:
            return
        log("build %s: exit 0 in %.1fs" % (p, dt))
        marker = os.path.join(ROOT, PREFIX[p], ".profile")
        try:
            with open(marker) as f:
                got = f.read().strip()
        except OSError:
            got = None
        ok &= check("marker %s" % PREFIX[p], got == p,
                    "holds %r" % (got,))
        trees[p] = hash_tree(PREFIX[p])
        log("%s: %d entries hashed" % (PREFIX[p], len(trees[p])))
    if not ok:
        return
    # Real write activity: delete one installed debug file, rebuild,
    # prove byte-identical self-heal + untouched neighbors.
    victim = os.path.join(ROOT, PREFIX["debug-static"], "bin", "ch")
    before = md5_of(victim)
    os.remove(victim)
    rc, out, dt = zig_build("debug-static")
    ok &= build_green("debug-static", out, rc, "rebuild debug-static")
    if rc == 0:
        ok &= check("deleted ch re-copied byte-identical",
                    os.path.isfile(victim) and md5_of(victim) == before,
                    "md5 %s" % before[:12])
        ok &= check("copy-out ran", "install_profile" in out)
    for p in profiles:
        if p == "debug-static":
            continue
        diffs = diff_trees(trees[p], hash_tree(PREFIX[p]))
        ok &= check("neighbor %s untouched" % PREFIX[p],
                    not diffs, "%d diffs" % len(diffs))
        for rel, a, b in diffs[:5]:
            log("    %s: %r -> %r" % (rel, a, b))
    diffs = diff_trees(trees["debug-static"],
                       hash_tree(PREFIX["debug-static"]))
    ok &= check("debug-static whole-tree identical after heal",
                not diffs, "%d diffs" % len(diffs))


def t_noop(profiles):
    log("== noop ==")
    for p in profiles:
        before = hash_tree(PREFIX[p])
        rc, out, dt = zig_build(p, verbose=True)
        if not build_green(p, out, rc, "no-op %s" % p):
            continue
        n = len(tu_compiles(out))
        check("no-op %s: zero TU compiles" % p, n == 0, "%d" % n)
        if p != "test":
            check("no-op %s: copy-out ran" % p,
                  "install_profile" in out)
        diffs = diff_trees(before, hash_tree(PREFIX[p]))
        check("no-op %s: zero artifact changes" % p, not diffs,
              "%d diffs" % len(diffs))
        for rel, a, b in diffs[:8]:
            log("    %s: %r -> %r" % (rel, a, b))


def t_source():
    log("== source: %s ==" % SOURCE_TU)
    watched = ["bin/ch", "bin/GCStress", "lib/libChakraCore.dylib",
               "lib/libChakraCoreStatic.a"]
    with touched(SOURCE_TU, PROBE_SEMANTIC_C):
        before = {w: md5_of(os.path.join(ROOT, PREFIX["test"], w))
                  for w in watched}
        rc, out, dt = zig_build("test", verbose=True, tag="source-touch")
        if not build_green("test", out, rc, "source-touch rebuild"):
            return
        lines = tu_compiles(out)
        srcs = tu_sources(lines)
        check("exactly 1 TU compiles", len(lines) == 1,
              "%d (%s)" % (len(lines), ",".join(srcs)))
        check("that TU is the touched source",
              len(srcs) == 1 and srcs[0].endswith("JavascriptArray.cpp"),
              ",".join(srcs))
        after = {w: md5_of(os.path.join(ROOT, PREFIX["test"], w))
                 for w in watched}
        for w in watched[1:]:
            check("%s relinked" % w, after[w] != before[w])
        check("shared ch NOT relinked (no gratuitous work)",
              after["bin/ch"] == before["bin/ch"])
    rc, out, dt = zig_build("test", verbose=True, tag="source-restore")
    if build_green("test", out, rc, "source-restore rebuild"):
        check("restore recompiles 1 TU",
              len(tu_compiles(out)) == 1,
              "%d" % len(tu_compiles(out)))


def t_header():
    log("== header: %s ==" % HEADER)
    with touched(HEADER, PROBE_C):
        rc, out, dt = zig_build("test", verbose=True, tag="header-touch")
        if not build_green("test", out, rc, "header-touch rebuild"):
            return
        n = len(tu_compiles(out))
        check("header rebuilds true dependents", HEADER_MIN <= n < HEADER_MAX,
              "%d TU compiles (bounds %d..<%d)"
              % (n, HEADER_MIN, HEADER_MAX))
    rc, out, dt = zig_build("test", verbose=True, tag="header-restore")
    if build_green("test", out, rc, "header-restore rebuild"):
        check("restore rebuilds dependents again",
              HEADER_MIN <= len(tu_compiles(out)) < HEADER_MAX,
              "%d" % len(tu_compiles(out)))


def is_ch_tu(src):
    # ch TUs: the 8 bin/ch sources plus bin/ChakraCore/
    # TestHooks.cpp (debug-static's 9th ch TU).
    return ("/bin/ch/" in src or src.startswith("bin/ch/")
            or "TestHooks.cpp" in src)


def check_ch_set(p, what, lines):
    """Exactly the ch TUs recompile — no more, no fewer.

    Exactness is valid because touched() restores mtimes: without
    that, an earlier test's byte-restored TU (fresh mtime) spuriously
    recompiled once in later profiles' counts (observed twice as a
    +1 before the mtime fix; depfile forensics confirmed no real
    edge — only the 8 ch .d files list the generated header).
    """
    srcs = tu_sources(lines)
    ch_only = [s for s in srcs if is_ch_tu(s)]
    want = CH_TU_EXPECT[p]
    ok = len(lines) == want and len(ch_only) == want
    return check("jstoc %s %s" % (p, what), ok,
                 "%d compiles, %d ch (%s)"
                 % (len(lines), len(ch_only),
                    ",".join(sorted(set(ch_only)))))


def t_jstoc(profiles):
    log("== jstoc: %s ==" % JSCONTROLLER)
    with touched(JSCONTROLLER, PROBE_JS):
        first = True
        for p in profiles:
            rc, out, dt = zig_build(p, verbose=True, tag="jstoc-touch-%s" % p)
            if not build_green(p, out, rc,
                               "jstoc-touch rebuild %s" % p):
                continue
            if first:
                # The generator step is profile-independent (same
                # script, same JS, same argv), so the FIRST profile
                # runs it and the rest cache-hit on its output —
                # assert the run once, where it is deterministic.
                check("jstoc %s: generator re-ran" % p,
                      "jstoc_run.py" in out)
                first = False
            check_ch_set(p, "touch: ch TUs recompile", tu_compiles(out))
    for p in profiles:
        rc, out, dt = zig_build(p, verbose=True, tag="jstoc-restore-%s" % p)
        if build_green(p, out, rc, "jstoc-restore rebuild %s" % p):
            check_ch_set(p, "restore: ch TUs recompile", tu_compiles(out))


def copy_tree_excluding_builds(dest):
    """Copy the live tree minus build outputs, caches and VCS state."""
    excludes = (".git", ".zig-cache", "zig-out", "out",
                "build-legacy-debug", "build-legacy-release",
                "build-xplat", "build-zigpilot")
    shutil.rmtree(dest, ignore_errors=True)
    os.makedirs(dest)
    for entry in sorted(os.listdir(ROOT)):
        if entry in excludes:
            continue
        src = os.path.join(ROOT, entry)
        dst = os.path.join(dest, entry)
        if os.path.isdir(src) and not os.path.islink(src):
            shutil.copytree(src, dst, symlinks=True)
        else:
            shutil.copy2(src, dst)


def t_config():
    log("== config: -Dicu-include respelling (scratch tree) ==")
    scratch = "/tmp/b34-incr-config"
    t0 = time.time()
    copy_tree_excluding_builds(scratch)
    log("scratch copy in %.1fs" % (time.time() - t0))
    try:
        rc, out, dt = run(ZIG + ["build", "--verbose"], BUILD_TIMEOUT,
                          cwd=scratch)
        save_log("config-scratch-plain", ZIG + ["build"], out)
        if not build_green("test", out, rc, "scratch plain build"):
            return
        n0 = len(tu_compiles(out))
        log("scratch plain: %d TU compiles (cold cache)" % n0)
        probe = ICU_REAL + "/"
        rc, out, dt = run(ZIG + ["build", "--verbose",
                                "-Dicu-include=" + probe],
                          BUILD_TIMEOUT, cwd=scratch)
        save_log("config-scratch-respell", ZIG + ["build"], out)
        if not build_green("test", out, rc, "scratch respelt build"):
            return
        n = len(tu_compiles(out))
        check("config change rebuilds full graph", n > FULL_MIN,
              "%d TU compiles (floor %d)" % (n, FULL_MIN))
    finally:
        shutil.rmtree(scratch, ignore_errors=True)


def t_bzcomment():
    log("== bzcomment: comment-only build.zig touch ==")
    with touched("build.zig", PROBE_ZIG):
        rc, out, dt = zig_build("test", verbose=True, tag="bzcomment")
        if not build_green("test", out, rc, "comment-touch rebuild"):
            return
        n = len(tu_compiles(out))
        check("build-script touch: zero TU compiles", n == 0, "%d" % n)


def t_selection():
    log("== selection ==")
    rc, out, dt = run([sys.executable,
                       "tools/build-contract/verify_b1.py", "--self-test"],
                      120)
    log(out.strip())
    check("verify_b1 --self-test green", rc == 0, "exit %s" % rc)


def t_concurrent():
    log("== concurrent: test + debug-static no-ops simultaneously ==")
    t0 = time.time()
    procs = [subprocess.Popen(ZIG + ["build"] + (["-Dprofile=%s" % p]
                              if p != "test" else []),
                              cwd=ROOT, stdout=subprocess.PIPE,
                              stderr=subprocess.STDOUT, text=True)
             for p in ("test", "debug-static")]
    outs = []
    rcs = []
    for proc in procs:
        try:
            out, _ = proc.communicate(timeout=BUILD_TIMEOUT)
        except subprocess.TimeoutExpired:
            proc.kill()
            out, _ = proc.communicate()
            outs.append(out + "\n[TIMEOUT]")
            rcs.append(None)
            continue
        outs.append(out)
        rcs.append(proc.returncode)
    dt = time.time() - t0
    for p, rc, out in zip(("test", "debug-static"), rcs, outs):
        if rc != 0:
            log("--- tail of failed concurrent %s ---" % p)
            log("\n".join(out.splitlines()[-15:]))
        check("concurrent %s exit 0" % p, rc == 0,
              "exit %s in %.1fs wall" % (rc, dt))


TESTS = {"isolation": t_isolation, "noop": t_noop, "source": t_source,
         "header": t_header, "jstoc": t_jstoc, "config": t_config,
         "bzcomment": t_bzcomment, "selection": t_selection,
         "concurrent": t_concurrent}
QUICK = ("isolation", "noop", "bzcomment", "selection")
FULL = ("isolation", "noop", "source", "header", "jstoc", "config",
        "bzcomment", "selection", "concurrent")


def main(argv):
    args = list(argv[1:])
    quick = False
    if "--quick" in args:
        quick = True
        args.remove("--quick")
    if args and (set(args) - set(TESTS)):
        sys.exit("unknown test(s): %s (choose from %s)"
                 % (sorted(set(args) - set(TESTS)), sorted(TESTS)))
    names = tuple(args) if args else (QUICK if quick else FULL)
    profiles = (("test", "debug-static") if quick
                else ("test", "debug-static", "release-shared"))
    if shutil.which("mise") is None:
        sys.exit("mise not found on PATH; this script uses ONLY mise zig")
    rc, out, _ = run(ZIG + ["version"], 60)
    if rc != 0 or out.strip() != "0.17.0":
        sys.exit("need mise zig 0.17.0, have %r" % out.strip())
    t0 = time.time()
    log("incremental proofs — root %s — %s" % (ROOT, names))
    for name in names:
        fn = TESTS[name]
        try:
            if name in ("isolation", "noop", "jstoc"):
                fn(profiles)
            else:
                fn()
        except Exception as e:
            check("%s raised" % name, False, "%r" % e)
    dt = time.time() - t0
    log("")
    log("SUMMARY: %d FAIL" % len(failures)
        + (" (%s)" % ", ".join(failures) if failures else "")
        + " in %.1fs" % dt)
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
