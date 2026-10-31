#!/usr/bin/env python3
#-------------------------------------------------------------------------------------------------------
# Copyright (c) ChakraCore Project Contributors. All rights reserved.
# Licensed under the MIT license. See LICENSE.txt file in the project root for full license information.
#-------------------------------------------------------------------------------------------------------
"""Runtime smoke for the worktree ARM64 macOS JIT profile.

The generic check_smoke.py intentionally uses interpreter-only snippets so
it can run against every profile. This gate is JIT-specific:

  1. the installed ch and its sibling dylib contain the native backend
     (nm finds NativeCodeGenerator; a DISABLE_JIT build has no such symbol);
  2. a physical script runs under `-mic:1 -bgjit-` and under
     `-forceNative -off:simpleJit -bgJitDelay:0`, producing the expected
     output. The script exercises a hot call, rest arguments, and apply,
     so the JIT-configured interpreter and the native compiler both run;
  3. `verify_bytecode_pure.py --jit --verify` (run as a separate build step)
     checks the JIT builtin bytecode headers consumed by this profile.

Usage: check_jit.py <ch>
Exit code: 0 iff every sub-check passes.
"""

import os
import subprocess
import sys
import tempfile

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import common

SCRIPT = """\
function add(a, b) { return a + b; }
function rest(a, ...r) { return r.length; }
function sum(a, b, c, d, e, f, g) { return a + b + c + d + e + f + g; }
var total = 0;
for (var i = 0; i < 200; i++) { total = add(2, 3); }
print("jit-add " + total);
print("jit-rest " + rest(1, 2, 3));
print("jit-apply " + sum.apply(undefined, [1, 2, 3, 4, 5, 6, 7]));
"""
WANT = ["jit-add 5", "jit-rest 2", "jit-apply 28"]


def check_symbols(ch):
    dylib = os.path.join(os.path.dirname(os.path.abspath(ch)), "libChakraCore.dylib")
    if not os.path.isfile(dylib):
        return "sibling dylib not found: %s" % dylib
    rc, out, err = common.run_capture(["/usr/bin/nm", dylib])
    if rc != 0:
        return "nm %s exited %d: %s" % (dylib, rc, err.strip())
    if "NativeCodeGenerator" not in out:
        return ("%s has no NativeCodeGenerator symbol; the JIT was compiled out"
                % dylib)
    print("check_jit: NativeCodeGenerator symbol present")
    return None


def check_run(ch, flags, script_path):
    rc, out, err = common.run_capture([ch] + flags + [script_path])
    print("check_jit: %s -> rc=%d out=%r err=%r"
          % (" ".join(flags), rc, out.strip(), err.strip()))
    if rc != 0:
        return "%s exited %d (err=%r)" % (" ".join(flags), rc, err)
    if out.splitlines() != WANT:
        return "%s stdout %r != %r" % (" ".join(flags), out.splitlines(), WANT)
    return None


def main(argv):
    if len(argv) != 2:
        print("usage: check_jit.py <ch>", file=sys.stderr)
        return 2
    ch = argv[1]
    if not os.path.isfile(ch):
        print("check_jit: missing ch %r" % ch, file=sys.stderr)
        return 2

    failure = check_symbols(ch)
    if failure:
        print("check_jit: FAIL: %s" % failure, file=sys.stderr)
        return 1

    tmp = tempfile.mkdtemp(prefix="check-jit-")
    try:
        script_path = os.path.join(tmp, "smoke.js")
        with open(script_path, "w") as f:
            f.write(SCRIPT)
        for flags in (
            ["-mic:1", "-bgjit-"],
            ["-forceNative", "-off:simpleJit", "-bgJitDelay:0"],
        ):
            failure = check_run(ch, flags, script_path)
            if failure:
                print("check_jit: FAIL: %s" % failure, file=sys.stderr)
                return 1
    finally:
        for name in os.listdir(tmp):
            try:
                os.remove(os.path.join(tmp, name))
            except OSError:
                pass
        try:
            os.rmdir(tmp)
        except OSError:
            pass

    print("check_jit: PASS")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
