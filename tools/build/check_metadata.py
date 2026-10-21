#!/usr/bin/env python3
#-------------------------------------------------------------------------------------------------------
# Copyright (c) ChakraCore Project Contributors. All rights reserved.
# Licensed under the MIT license. See LICENSE.txt file in the project root for full license information.
#-------------------------------------------------------------------------------------------------------
"""B2.3 check gate (5): artifact metadata assertions.

Sub-checks against the install prefix (zig-out for test,
out/<profile> for non-test profiles via --profile; B3.4 side
prefixes, doc/b3-profiles.md section 10):

  1. macOS minimum 26.0 on ch, GCStress and (shared profiles)
     libChakraCore.dylib (parsed from `otool -l` LC_BUILD_VERSION,
     cross-checked with `vtool -show-build`). Legacy stamps minos
     11.0 (Apple ld floor from -mmacosx-version-min=10.9); 26.0 is
     our declared floor (it matches the Homebrew ICU dylib
     minimum), a deliberate B2-established deviation.
  2. LC_MAIN stacksize on ch and GCStress: 8 MiB (8388608) for
     shared profiles (doc/build.md section 14 stack contract), 0
     for debug-static — the U-B3.1-2 pin (legacy-exact: a nonzero
     LC_MAIN stacksize makes RLIMIT_STACK immutable on macOS, so
     PAL_Initialize's lowering fails and the Debug _ASSERTE
     aborts; this assertion goes red if the abort recurs, in ANY
     shell, without needing the 32MB-limit repro environment).
     Release-shared asserts its own 8 MiB by DIRECT otool reads of
     the release binaries — the 8MiB-via-shared-path assumption is
     observed, never inferred from the test profile.
  3. The 4 staged public headers present, nonempty and byte-identical
     to their lib/ sources in <prefix>/include.
  4. Shared profiles: JsCreateRuntime plus key JsRT exports present
     as dynamic text symbols (`nm -gU`: `T _Js...`) in
     libChakraCore.dylib, with a sanity floor on the total
     exported-symbol count. Static profiles: no dylib beside ch or
     in lib/; libChakraCoreStatic.a present with the legacy-exact
     526 members and the key JsRT text symbols (`nm -g`); ch links
     no libChakraCore (self-contained static executable).
  5. The `.profile` marker names this profile (B3.4 isolation pin:
     a prefix holding another profile's artifacts fails here, not
     silently downstream).

Every sub-check fails loudly with diagnostics on mismatch.

Usage: check_metadata.py <prefix> [--profile test|debug-static|
       release-shared] [--self-test]
Defaults: --profile test (the B2 assertions, unchanged).
Exit code: 0 iff every sub-check passes.
"""

import hashlib
import os
import subprocess
import sys
import tempfile

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import common

WANT_MINOS = "26.0"
WANT_STACK = 8388608
MIN_EXPORTS = 100
# Per-profile LC_MAIN stacksize contract (see module docstring §2).
# Release-shared repeats test's 8 MiB as its OWN pinned expectation
# (direct otool observation per run — never inferred from test).
WANT_STACK_BY_PROFILE = {"test": 8388608, "debug-static": 0,
                         "release-shared": 8388608}
# Static archive member count, legacy-exact on both sides
# (ar t: 524 members in build-legacy-debug and out/debug-static at the
# B3 baseline; C2-S13 added JavascriptWeakRef.cpp and
# JavascriptFinalizationRegistry.cpp to BOTH build lists, so the
# legacy-exact count is now 526).
WANT_STATIC_MEMBERS = 526
STATIC_ARCHIVE = "lib/libChakraCoreStatic.a"

EXECUTABLES = ("bin/ch", "bin/GCStress")
DYLIBS = ("bin/libChakraCore.dylib", "lib/libChakraCore.dylib")
HEADERS = (
    ("include/ChakraCore.h", "lib/Jsrt/ChakraCore.h"),
    ("include/ChakraCommon.h", "lib/Jsrt/ChakraCommon.h"),
    ("include/ChakraDebug.h", "lib/Jsrt/ChakraDebug.h"),
    ("include/ChakraCoreVersion.h", "lib/Common/ChakraCoreVersion.h"),
)
REQUIRED_EXPORTS = (
    "_JsCreateRuntime",
    "_JsCreateContext",
    "_JsRunScriptWithParserState",
    "_JsDisposeRuntime",
    "_JsSetCurrentContext",
    "_JsGetCurrentContext",
    "_JsIdle",
    "_JsCollectGarbage",
)


def parse_load_commands(otool_out):
    """Return (minos, stacksize): LC_BUILD_VERSION minos, LC_MAIN stacksize.

    Either is None when the corresponding load command is absent
    (dylibs have no LC_MAIN; static archives have neither).
    """
    minos, stack, cmd = None, None, None
    for raw in otool_out.splitlines():
        line = raw.strip()
        if line.startswith("cmd "):
            cmd = line[4:]
        elif cmd == "LC_BUILD_VERSION" and line.startswith("minos "):
            minos = line.split()[1]
        elif cmd == "LC_MAIN" and line.startswith("stacksize "):
            stack = int(line.split()[1])
    return minos, stack


def check_minos(path):
    rc, out, err = common.run_capture(["otool", "-l", path])
    if rc != 0:
        return "otool -l failed (rc=%d err=%r)" % (rc, err)
    minos, _ = parse_load_commands(out)
    print("check_metadata: minos %s -> %r" % (path, minos))
    if minos != WANT_MINOS:
        return "%s: minos %r, require %s" % (path, minos, WANT_MINOS)
    rc, vout, verr = common.run_capture(["vtool", "-show-build", path])
    if rc != 0:
        return "vtool -show-build failed (rc=%d err=%r)" % (rc, verr)
    if ("minos %s" % WANT_MINOS) not in vout or "platform MACOS" not in vout:
        return ("%s: vtool cross-check failed (want 'minos %s' + "
                "'platform MACOS', got %r)" % (path, WANT_MINOS, vout))
    return None


def check_stack(path, want_stack=WANT_STACK):
    rc, out, err = common.run_capture(["otool", "-l", path])
    if rc != 0:
        return "otool -l failed (rc=%d err=%r)" % (rc, err)
    _, stack = parse_load_commands(out)
    print("check_metadata: stack %s -> %r" % (path, stack))
    if stack != want_stack:
        return "%s: LC_MAIN stacksize %r, require %d" % (path, stack, want_stack)
    return None


def check_headers(prefix, root):
    failures = []
    for staged_rel, src_rel in HEADERS:
        staged = os.path.join(prefix, staged_rel)
        src = os.path.join(root, src_rel)
        if not os.path.isfile(staged):
            failures.append("%s: missing staged header" % staged_rel)
            continue
        with open(staged, "rb") as f:
            staged_bytes = f.read()
        if not staged_bytes:
            failures.append("%s: staged header is empty" % staged_rel)
            continue
        with open(src, "rb") as f:
            src_bytes = f.read()
        md5 = hashlib.md5(staged_bytes).hexdigest()[:12]
        print("check_metadata: header %s -> %d bytes md5=%s %s"
              % (staged_rel, len(staged_bytes), md5,
                 "byte-identical" if staged_bytes == src_bytes
                 else "DIFFERS FROM SOURCE"))
        if staged_bytes != src_bytes:
            failures.append("%s: staged bytes differ from %s"
                            % (staged_rel, src_rel))
    return failures


def exported_text_symbols(dylib):
    """Return the set of `T` symbol names from `nm -gU`."""
    rc, out, err = common.run_capture(["nm", "-gU", dylib])
    if rc != 0:
        raise RuntimeError("nm -gU failed (rc=%d err=%r)" % (rc, err))
    syms = set()
    for line in out.splitlines():
        parts = line.split()
        if len(parts) == 3 and parts[1] == "T":
            syms.add(parts[2])
    return syms


def check_exports(dylib):
    try:
        syms = exported_text_symbols(dylib)
    except RuntimeError as e:
        return ["%s: %s" % (dylib, e)]
    missing = [s for s in REQUIRED_EXPORTS if s not in syms]
    print("check_metadata: exports %s -> %d T symbols, "
          "required %d/%d present"
          % (dylib, len(syms), len(REQUIRED_EXPORTS) - len(missing),
             len(REQUIRED_EXPORTS)))
    failures = []
    if missing:
        failures.append("%s: missing exports: %s"
                        % (dylib, ", ".join(missing)))
    if len(syms) < MIN_EXPORTS:
        failures.append("%s: only %d exported text symbols, floor is %d"
                        % (dylib, len(syms), MIN_EXPORTS))
    return failures


def check_no_dylib(prefix):
    """Static profiles must ship no libChakraCore dylib anywhere."""
    failures = []
    for rel in DYLIBS:
        if os.path.exists(os.path.join(prefix, rel)):
            failures.append("%s: dylib present in a static profile" % rel)
    return failures


def check_static_archive(path):
    """The static archive: legacy-exact member count + JsRT symbols."""
    failures = []
    if not os.path.isfile(path):
        return ["%s: missing static archive" % STATIC_ARCHIVE]
    size = os.path.getsize(path)
    print("check_metadata: archive %s -> %d bytes" % (STATIC_ARCHIVE, size))
    if size == 0:
        return ["%s: static archive is empty" % STATIC_ARCHIVE]
    rc, out, err = common.run_capture(["ar", "t", path])
    if rc != 0:
        return ["ar t failed on %s (rc=%d err=%r)"
                % (STATIC_ARCHIVE, rc, err[:200])]
    members = [line for line in out.splitlines() if line.strip()]
    print("check_metadata: archive members -> %d" % len(members))
    if len(members) != WANT_STATIC_MEMBERS:
        failures.append("%s: %d members, require %d (legacy-exact)"
                        % (STATIC_ARCHIVE, len(members),
                           WANT_STATIC_MEMBERS))
    rc, out, err = common.run_capture(["nm", "-g", path])
    if rc != 0:
        failures.append("nm -g failed on %s (rc=%d err=%r)"
                        % (STATIC_ARCHIVE, rc, err[:200]))
        return failures
    syms = set()
    for line in out.splitlines():
        parts = line.split()
        if len(parts) == 3 and parts[1] == "T":
            syms.add(parts[2])
    missing = [s for s in REQUIRED_EXPORTS if s not in syms]
    print("check_metadata: archive %s -> %d T symbols, required %d/%d"
          % (STATIC_ARCHIVE, len(syms),
             len(REQUIRED_EXPORTS) - len(missing), len(REQUIRED_EXPORTS)))
    if missing:
        failures.append("%s: missing JsRT text symbols: %s"
                        % (STATIC_ARCHIVE, ", ".join(missing)))
    js_syms = sum(1 for s in syms if s.startswith("_Js"))
    if js_syms < MIN_EXPORTS:
        failures.append("%s: only %d _Js text symbols, floor is %d"
                        % (STATIC_ARCHIVE, js_syms, MIN_EXPORTS))
    return failures


def check_profile_marker(prefix, profile):
    """The .profile marker must name this profile (B3.4 pin)."""
    path = os.path.join(prefix, ".profile")
    if not os.path.isfile(path):
        return ".profile: missing marker (want %r)" % profile
    with open(path) as f:
        got = f.read().strip()
    print("check_metadata: marker %s -> %r" % (path, got))
    if got != profile:
        return (".profile holds %r, require %r: this prefix was built "
                "for another profile" % (got, profile))
    return None


def check_self_contained(ch_path):
    """A static ch must not link any libChakraCore (otool -L)."""
    rc, out, err = common.run_capture(["otool", "-L", ch_path])
    if rc != 0:
        return "otool -L failed on %s (rc=%d err=%r)" % (ch_path, rc, err)
    # otool -L exits 0 on garbage ("<path>: is not an object file"),
    # so the marker — not the exit code — proves a real header read.
    if "is not an object file" in out:
        return "otool -L on %s: not an object file" % ch_path
    print("check_metadata: linked libs for %s:\n%s"
          % (ch_path, "\n".join("   " + line for line in out.splitlines())))
    hits = [line.strip() for line in out.splitlines()
            if "libChakraCore" in line]
    if hits:
        return "%s: links libChakraCore (not self-contained): %r" % (ch_path, hits)
    return None


def self_test():
    """Negative controls: every gate must fail against tampered victims."""
    bad = 0

    def report(name, cond):
        print("self-test %-52s -> %s"
              % (name, "PASS" if cond else "FAIL"))
        return 0 if cond else 1

    # Hermetic tampered victim: synthetic garbage bytes (no host
    # binary — /bin/ls's minos/stack values drift with the host OS).
    # otool/nm reject the garbage, so every gate fails loudly.
    tmp = tempfile.mkdtemp(prefix="check-metadata-selftest-")
    try:
        src = os.path.join(tmp, "garbage.dylib")
        with open(src, "wb") as f:
            f.write(b"not a mach-o file, just tampered bytes\n" * 16)
        bad += report("garbage victim fails minos gate",
                      check_minos(src) is not None)
        bad += report("garbage victim fails stack gate",
                      check_stack(src) is not None)
        bad += report("garbage victim fails exports gate",
                      check_exports(src) != [])
        bad += report("garbage victim fails static-archive gate",
                      check_static_archive(src) != [])
        bad += report("garbage victim fails self-contained gate",
                      check_self_contained(src) is not None)
        # A prefix containing a dylib fails the static no-dylib gate.
        os.makedirs(os.path.join(tmp, "bin"))
        with open(os.path.join(tmp, "bin", "libChakraCore.dylib"),
                  "wb") as f:
            f.write(b"decoy\n")
        bad += report("dylib present fails static no-dylib gate",
                      check_no_dylib(tmp) != [])
        # Empty include dir fails the header gate.
        root = os.path.dirname(os.path.dirname(
            os.path.dirname(os.path.abspath(__file__))))
        bad += report("empty dir fails header gate (4 missing)",
                      len(check_headers(tmp, root)) == 4)
        # .profile marker controls (B3.4 isolation pin).
        bad += report("missing marker fails marker gate",
                      check_profile_marker(tmp, "test") is not None)
        with open(os.path.join(tmp, ".profile"), "w") as f:
            f.write("debug-static\n")
        bad += report("own marker passes marker gate",
                      check_profile_marker(tmp, "debug-static") is None)
        bad += report("foreign marker fails marker gate",
                      check_profile_marker(tmp, "test") is not None)
        # Parser unit checks on doctored otool text.
        doctored = ("      cmd LC_BUILD_VERSION\n    minos 99.99\n"
                    "      cmd LC_MAIN\n stacksize 1234\n")
        bad += report("parser reads doctored minos/stack",
                      parse_load_commands(doctored) == ("99.99", 1234))
        bad += report("parser yields None on garbage",
                      parse_load_commands("not mach-o\n") == (None, None))
        # Profile-contract pins (pure table checks, hermetic).
        bad += report("debug-static wants stacksize 0",
                      WANT_STACK_BY_PROFILE.get("debug-static") == 0)
        bad += report("test wants stacksize 8388608",
                      WANT_STACK_BY_PROFILE.get("test") == 8388608)
        bad += report("release-shared wants stacksize 8388608",
                      WANT_STACK_BY_PROFILE.get("release-shared")
                      == 8388608)
        bad += report("static archive wants 526 members",
                      WANT_STATIC_MEMBERS == 526)
    finally:
        import shutil
        shutil.rmtree(tmp, ignore_errors=True)

    print("check_metadata self-test: %s"
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
                print("check_metadata: --profile needs a value",
                      file=sys.stderr)
                return 2
            profile = args[i + 1]
            i += 2
        else:
            rest.append(args[i])
            i += 1
    if profile not in WANT_STACK_BY_PROFILE:
        print("check_metadata: bad --profile %r: expected one of: %s"
              % (profile, ", ".join(sorted(WANT_STACK_BY_PROFILE))),
              file=sys.stderr)
        return 2
    if len(rest) != 1:
        print("usage: check_metadata.py <prefix> "
              "[--profile test|debug-static|release-shared]",
              file=sys.stderr)
        return 2
    prefix = os.path.abspath(rest[0])
    root = os.path.dirname(os.path.dirname(
        os.path.dirname(os.path.abspath(__file__))))
    failures = []
    want_stack = WANT_STACK_BY_PROFILE[profile]
    is_static = profile == "debug-static"

    err = check_profile_marker(prefix, profile)
    if err is not None:
        failures.append(err)

    try:
        minos_targets = EXECUTABLES if is_static else EXECUTABLES + DYLIBS
        for rel in minos_targets:
            path = os.path.join(prefix, rel)
            if not os.path.exists(path):
                failures.append("%s: missing artifact" % rel)
                continue
            err = check_minos(path if not os.path.islink(path)
                              else os.path.realpath(path))
            if err is not None:
                failures.append(err)
        for rel in EXECUTABLES:
            path = os.path.join(prefix, rel)
            if not os.path.exists(path):
                continue  # already reported above
            err = check_stack(path, want_stack)
            if err is not None:
                failures.append(err)
        failures.extend(check_headers(prefix, root))
        if is_static:
            failures.extend(check_no_dylib(prefix))
            failures.extend(check_static_archive(
                os.path.join(prefix, STATIC_ARCHIVE)))
            ch_path = os.path.join(prefix, EXECUTABLES[0])
            if os.path.exists(ch_path):
                err = check_self_contained(ch_path)
                if err is not None:
                    failures.append(err)
        else:
            dylib = os.path.join(prefix, DYLIBS[0])
            if os.path.exists(dylib):
                failures.extend(check_exports(os.path.realpath(dylib)))
    except subprocess.TimeoutExpired as e:
        failures.append("probe exceeded the %ds ceiling: %s"
                        % (e.timeout, " ".join(e.cmd)))

    if failures:
        print("check_metadata: FAIL:", file=sys.stderr)
        for f in failures:
            print("check_metadata: FAIL: %s" % f, file=sys.stderr)
        return 1
    if is_static:
        print("check_metadata: PASS: minos 26.0 (otool+vtool), stacksize 0, "
              "4 headers byte-identical, no dylib, static archive %d "
              "members + %d JsRT symbols, ch self-contained"
              % (WANT_STATIC_MEMBERS, len(REQUIRED_EXPORTS)))
    else:
        print("check_metadata: PASS: minos 26.0 (otool+vtool), 8MiB stacks, "
              "4 headers byte-identical, %d JsRT exports present"
              % len(REQUIRED_EXPORTS))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
