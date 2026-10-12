#!/usr/bin/env python3
#-------------------------------------------------------------------------------------------------------
# Copyright (c) ChakraCore Project Contributors. All rights reserved.
# Licensed under the MIT license. See LICENSE.txt file in the project root for full license information.
#-------------------------------------------------------------------------------------------------------
"""B2.3 check gate (3): embedding (native) test, legacy method.

Runs the SAME script and arguments the legacy `ninja check` runs for
its native leg (test/CMakeLists.txt:48-55):

    test/native-tests/test_native.sh <ch> <build-type> <cc> <cxx> <repo-root>

with <build-type> RelWithDebInfo (shared test), Debug (static), or
Release (shared release, via --build-type Release — the script only
uses $2 in a commented-out line, but the gate passes the
legacy-exact argv). One
deliberate deviation: the script generates Makefiles (and objects)
inside the directory it runs in, so it runs in a COPY of
test/native-tests under $TMPDIR instead of in-tree. The method (same
script, same arguments, same zig-built ch and libraries) is unchanged,
and the source tree is never written to. Stale generated files
(Makefile, sample.o) are excluded from the copy so the run starts
clean.

Shared mode (default, the B2 test profile) requires: rc 0,
`test-shared-basic : PASS` in the output, exactly 8
`Ignoring test-static-...` lines (the 8 static-only suites are skipped
in a shared build — the legacy baseline behavior), and no failure line
of the script's exact `<suite> FAILED` form (test_native.sh:34).

Static mode (--static --archive <libChakraCoreStatic.a>) requires: rc
0, all 8 `test-static-<name> : PASS` lines (each PASS is printed only
after the test's sample binary is built AND run to SUCCESS — execution
proof, not assumption), `Ignoring test-shared-basic`, and no FAILED
line. Static linkage is proven, not assumed: (a) `nm -g` on the
archive shows the JsRT text symbols the samples bind; (b) a generated
per-test Makefile is captured before cleanup and must reference
libChakraCoreStatic.a via -force_load. The static samples link
<bindir>/lib/libChakraCoreStatic.a where <bindir> is ch's own
realpath'd directory (Platform.js via GetBinaryLocation), so the gate
stages a layout the script accepts: a COPY of ch plus a lib/ dir
symlinking the real archive bytes.

The cc/cxx compilers resolve HERE at gate time (shutil.which, failing
loudly when absent) so a missing standalone clang breaks only this
gate — never configure time or plain `zig build`.

Usage: check_native.py <test_native.sh> <ch> <cc> <cxx>
       [--static --archive <libChakraCoreStatic.a>]
       [--build-type <t>] [--self-test]
--build-type overrides the shared-mode build-type argv (default
RelWithDebInfo, the B2 test value, unchanged); it is rejected with
--static (whose Debug argv is fixed). Release passes Release.
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
SHARED_IGNORE = "Ignoring test-shared-basic"
# The 8 static suites (test/native-tests/test-static-*), each required
# to print `<name> : PASS` in static mode.
STATIC_SUITES = (
    "test-static-c98",
    "test-static-char",
    "test-static-char16",
    "test-static-external",
    "test-static-native",
    "test-static-pal",
    "test-static-property",
    "test-static-property-symbol",
)
# Archive symbols the static samples must bind (nm -g `T` lines).
STATIC_REQUIRED_SYMS = (
    "_JsCreateRuntime",
    "_JsCreateContext",
    "_JsDisposeRuntime",
)
# The script's exact failure-line format (`echo "${TEST_PATH} FAILED"`,
# test_native.sh:34): a bare "FAILED" substring would also match e.g.
# "XFAILED" notes or compiler chatter mid-line. Note the legacy debug
# run prints a lowercase "JsObjectDeleteProperty failed" note, which
# must NOT match this uppercase-anchored form.
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


def check_output_static(text):
    """Return a list of failure strings for static mode (empty: green)."""
    failures = []
    for suite in STATIC_SUITES:
        if (suite + " : PASS") not in text:
            failures.append("missing %r in output" % (suite + " : PASS"))
    if SHARED_IGNORE not in text:
        failures.append("missing %r in output" % SHARED_IGNORE)
    if SHARED_PASS in text:
        failures.append("shared suite ran PASS in a static build (want %r)"
                        % SHARED_IGNORE)
    failed_lines = [line.strip() for line in text.splitlines()
                    if FAILED_RE.match(line)]
    if failed_lines:
        failures.append("output contains %d FAILED line(s): %r"
                        % (len(failed_lines), failed_lines[:3]))
    return failures


def check_archive_symbols(archive):
    """Return a list of failure strings for the static archive (nm -g)."""
    rc, out, err = common.run_capture(["nm", "-g", archive])
    if rc != 0:
        return ["nm -g failed on %s (rc=%d err=%r)"
                % (archive, rc, err[:200])]
    syms = set()
    for line in out.splitlines():
        parts = line.split()
        if len(parts) == 3 and parts[1] == "T":
            syms.add(parts[2])
    print("check_native: archive %s -> %d T symbols" % (archive, len(syms)))
    missing = [s for s in STATIC_REQUIRED_SYMS if s not in syms]
    if missing:
        return ["%s: missing JsRT text symbols: %s"
                % (archive, ", ".join(missing))]
    return []


def check_makefile_links_static(makefile_text, suite):
    """The generated Makefile must link the static archive via force_load."""
    if "libChakraCoreStatic.a" not in makefile_text:
        return ("%s/Makefile: no libChakraCoreStatic.a reference "
                "(samples would not bind the static archive)" % suite)
    if "force_load" not in makefile_text:
        return ("%s/Makefile: no -force_load join (static link shape)"
                % suite)
    return None


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

    # Static-mode doctored logs.
    good_static = ("\n".join(s + " : PASS" for s in STATIC_SUITES) + "\n"
                   + SHARED_IGNORE + "\n")
    bad += report("static: 8 PASS + shared ignore accepted",
                  check_output_static(good_static) == [])
    bad += report("static: lowercase 'failed' note accepted",
                  check_output_static(good_static +
                                      "JsObjectDeleteProperty failed\n")
                  == [])
    bad += report("static: 7 PASS rejected",
                  check_output_static("\n".join(
                      s + " : PASS" for s in STATIC_SUITES[:-1]) + "\n"
                      + SHARED_IGNORE + "\n") != [])
    bad += report("static: missing shared ignore rejected",
                  check_output_static("\n".join(
                      s + " : PASS" for s in STATIC_SUITES) + "\n") != [])
    bad += report("static: shared PASS rejected",
                  check_output_static(good_static + SHARED_PASS + "\n") != [])
    bad += report("static: FAILED line rejected",
                  check_output_static(good_static + "test-static-char FAILED\n")
                  != [])
    bad += report("static: shared log rejected",
                  check_output_static(
                      good_ignores + "\n" + SHARED_PASS + "\n") != [])
    bad += report("makefile: archive + force_load accepted",
                  check_makefile_links_static(
                      "LDIR=$(LIB)/libChakraCoreStatic.a\n"
                      "FORCE_STARTS=-Wl,-force_load,\n",
                      "test-static-char") is None)
    bad += report("makefile: dylib link rejected",
                  check_makefile_links_static(
                      "LDIR=$(LIB)/libChakraCore.dylib\n", "test-static-char")
                  is not None)
    bad += report("makefile: no force_load rejected",
                  check_makefile_links_static(
                      "LDIR=$(LIB)/libChakraCoreStatic.a\n",
                      "test-static-char") is not None)

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
        err = _io.StringIO()
        with _ctx.redirect_stderr(err):
            rc = main(["check_native.py", decoy_sh, decoy_ch,
                       "clang", "clang++", "--static"])
        bad += report("--static without --archive fails loudly (rc=%d)" % rc,
                      rc == 2 and "--archive" in err.getvalue())
        err = _io.StringIO()
        with _ctx.redirect_stderr(err):
            rc = main(["check_native.py", decoy_sh, decoy_ch,
                       "clang", "clang++", "--static",
                       "--archive", decoy_ch, "--build-type", "Release"])
        bad += report("--build-type with --static fails loudly (rc=%d)" % rc,
                      rc == 2 and "--build-type" in err.getvalue())
        err = _io.StringIO()
        with _ctx.redirect_stderr(err):
            rc = main(["check_native.py", decoy_sh, decoy_ch,
                       "clang", "clang++", "--build-type"])
        bad += report("--build-type without value fails loudly (rc=%d)" % rc,
                      rc == 2 and "--build-type" in err.getvalue())
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
    static = "--static" in args
    args = [a for a in args if a != "--static"]
    archive = None
    build_type = None
    rest = []
    i = 0
    while i < len(args):
        if args[i] == "--archive":
            if i + 1 >= len(args):
                print("check_native: --archive needs a path",
                      file=sys.stderr)
                return 2
            archive = args[i + 1]
            i += 2
        elif args[i].startswith("--archive="):
            archive = args[i].split("=", 1)[1]
            i += 1
        elif args[i] == "--build-type":
            if i + 1 >= len(args):
                print("check_native: --build-type needs a value",
                      file=sys.stderr)
                return 2
            build_type = args[i + 1]
            i += 2
        elif args[i].startswith("--build-type="):
            build_type = args[i].split("=", 1)[1]
            i += 1
        else:
            rest.append(args[i])
            i += 1
    if static and not archive:
        print("check_native: --static requires --archive "
              "<libChakraCoreStatic.a>", file=sys.stderr)
        return 2
    if not static and archive:
        print("check_native: --archive without --static", file=sys.stderr)
        return 2
    if static and build_type:
        print("check_native: --build-type with --static (static argv is "
              "fixed to Debug)", file=sys.stderr)
        return 2
    if len(rest) != 4:
        print("usage: check_native.py <test_native.sh> <ch> <cc> <cxx> "
              "[--static --archive <libChakraCoreStatic.a>] "
              "[--build-type <t>]",
              file=sys.stderr)
        return 2
    sh_src, ch, cc, cxx = rest
    for path in ([sh_src, ch] + ([archive] if archive else [])):
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
    if static:
        failures = check_archive_symbols(os.path.abspath(archive))
        if failures:
            print("check_native: FAIL:", file=sys.stderr)
            for f in failures:
                print("check_native: FAIL: %s" % f, file=sys.stderr)
            return 1
    src_dir = os.path.dirname(os.path.abspath(sh_src))
    repo_root = os.path.dirname(os.path.dirname(src_dir))

    tmp = tempfile.mkdtemp(prefix="check-native-")
    try:
        try:
            work = os.path.join(tmp, "native-tests")
            shutil.copytree(src_dir, work,
                            ignore=shutil.ignore_patterns(
                                "Makefile", "sample.o", "*.o", "*.d"))
            if static:
                # Stage the <bindir>/lib layout the static Platform.js
                # links (see module docstring): GetBinaryLocation
                # realpath's the executable, so ch must be a real COPY
                # (a symlink would resolve back to the install dir);
                # the 1.7GB archive is symlinked, never copied.
                stage = os.path.join(tmp, "stage")
                stage_lib = os.path.join(stage, "lib")
                os.makedirs(stage_lib)
                stage_ch = os.path.join(stage, "ch")
                shutil.copy2(os.path.abspath(ch), stage_ch)
                os.symlink(os.path.abspath(archive),
                           os.path.join(stage_lib,
                                        "libChakraCoreStatic.a"))
                ch_arg, run_type = stage_ch, "Debug"
            else:
                ch_arg = os.path.abspath(ch)
                run_type = build_type or "RelWithDebInfo"
            rc, text = run_native(os.path.join(work, "test_native.sh"), work,
                                  ch_arg, run_type, cc, cxx, repo_root)
            makefile_err = None
            if static:
                # Capture a generated Makefile before cleanup: the
                # samples link through it, so it is linkage evidence.
                witness = os.path.join(work, "test-static-char",
                                       "Makefile")
                try:
                    with open(witness) as f:
                        makefile_text = f.read()
                except OSError as e:
                    makefile_err = ("cannot read generated %s: %s"
                                    % (witness, e))
                else:
                    print("check_native: witness Makefile: %d bytes, "
                          "archive=%s force_load=%s"
                          % (len(makefile_text),
                             "libChakraCoreStatic.a" in makefile_text,
                             "force_load" in makefile_text))
                    makefile_err = check_makefile_links_static(
                        makefile_text, "test-static-char")
        finally:
            shutil.rmtree(tmp, ignore_errors=True)
    except subprocess.TimeoutExpired:
        print("check_native: FAIL: test_native.sh exceeded the %ds ceiling"
              % common.TEE_TIMEOUT, file=sys.stderr)
        return 1

    print("check_native: exit=%d" % rc)
    failures = (check_output_static(text) if static
                else check_output(text))
    if static and makefile_err is not None:
        failures.append(makefile_err)
    if rc != 0:
        failures.insert(0, "test_native.sh exited %d" % rc)
    if failures:
        print("check_native: FAIL:", file=sys.stderr)
        for f in failures:
            print("check_native: FAIL: %s" % f, file=sys.stderr)
        return 1
    if static:
        print("check_native: PASS: 8 static suites PASS, shared ignored, "
              "archive symbols + Makefile linkage proven")
    else:
        print("check_native: PASS: %s, %d static suites ignored"
              % (SHARED_PASS, EXPECTED_IGNORES))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
