#!/usr/bin/env python3
#-------------------------------------------------------------------------------------------------------
# Copyright (c) ChakraCore Project Contributors. All rights reserved.
# Licensed under the MIT license. See LICENSE.txt file in the project root for full license information.
#-------------------------------------------------------------------------------------------------------
"""B2.3 check gate (5): artifact metadata assertions.

Four sub-checks against the install prefix (default zig-out):

  1. macOS minimum 26.0 on ch, GCStress and libChakraCore.dylib
     (parsed from `otool -l` LC_BUILD_VERSION, cross-checked with
     `vtool -show-build`).
  2. 8 MiB (8388608) LC_MAIN stacksize on ch and GCStress
     (doc/build.md section 14 stack contract).
  3. The 4 staged public headers present, nonempty and byte-identical
     to their lib/ sources in <prefix>/include.
  4. JsCreateRuntime plus key JsRT exports present as dynamic text
     symbols (`nm -gU`: `T _Js...`) in libChakraCore.dylib, with a
     sanity floor on the total exported-symbol count.

Every sub-check fails loudly with diagnostics on mismatch.

Usage: check_metadata.py <prefix> [--self-test]
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


def check_stack(path):
    rc, out, err = common.run_capture(["otool", "-l", path])
    if rc != 0:
        return "otool -l failed (rc=%d err=%r)" % (rc, err)
    _, stack = parse_load_commands(out)
    print("check_metadata: stack %s -> %r" % (path, stack))
    if stack != WANT_STACK:
        return "%s: LC_MAIN stacksize %r, require %d" % (path, stack, WANT_STACK)
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
        # Empty include dir fails the header gate.
        root = os.path.dirname(os.path.dirname(
            os.path.dirname(os.path.abspath(__file__))))
        bad += report("empty dir fails header gate (4 missing)",
                      len(check_headers(tmp, root)) == 4)
        # Parser unit checks on doctored otool text.
        doctored = ("      cmd LC_BUILD_VERSION\n    minos 99.99\n"
                    "      cmd LC_MAIN\n stacksize 1234\n")
        bad += report("parser reads doctored minos/stack",
                      parse_load_commands(doctored) == ("99.99", 1234))
        bad += report("parser yields None on garbage",
                      parse_load_commands("not mach-o\n") == (None, None))
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
    if len(args) != 1:
        print("usage: check_metadata.py <prefix>", file=sys.stderr)
        return 2
    prefix = os.path.abspath(args[0])
    root = os.path.dirname(os.path.dirname(
        os.path.dirname(os.path.abspath(__file__))))
    failures = []

    try:
        for rel in EXECUTABLES + DYLIBS:
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
            err = check_stack(path)
            if err is not None:
                failures.append(err)
        failures.extend(check_headers(prefix, root))
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
    print("check_metadata: PASS: minos 26.0 (otool+vtool), 8MiB stacks, "
          "4 headers byte-identical, %d JsRT exports present"
          % len(REQUIRED_EXPORTS))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
