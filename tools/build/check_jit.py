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
     output. The script exercises a hot call, rest arguments, apply, and
     a class definition + instantiation, so the JIT-configured interpreter
     and the native compiler both run. The class leg pins
     LowerNewClassConstructor's aux-slot stores: e14
     (doc/e14-fulljit-sigsegv.md) showed a byte-offset/slot-index mixup
     there reads a null prototype Var out of the class constructor and
     SIGSEGVs every dynapogo-mode `new` -- invisible to a classless
     smoke, which is why this gate missed the 109-test fulljit family;
  3. `verify_bytecode_pure.py --jit --verify` (run as a separate build step)
     checks the JIT builtin bytecode headers consumed by this profile.

Usage: check_jit.py <ch>
Exit code: 0 iff every sub-check passes.
"""

import os
import shutil
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
// e14: a JIT-created class constructor (LowerNewClassConstructor writes the
// prototype slot from lowered code) whose slot VALUES are read and whose
// result is constructed. The fulljit family crashed in
// NewScObjectCommon -> GetTypeId(null) because a byte-vs-slot offset mixup
// left the prototype slot zeroed; reading length/name/prototype and calling
// new covers that path under both flag sets.
class P { constructor() { this.x = 7; } }
print("jit-class-keys " + Object.getOwnPropertyNames(P).join(","));
print("jit-class-vals " + P.length + "," + P.name + "," + (P.prototype === P.prototype));
var p = new P();
print("jit-class-new " + p.x + "," + (p instanceof P));
var Anon = class extends P {};
var a = new Anon();
print("jit-class-derived " + (a instanceof Anon) + "," + (a instanceof P) + "," + Anon.name);
"""
WANT = ["jit-add 5", "jit-rest 2", "jit-apply 28",
        "jit-class-keys length,name,prototype", "jit-class-vals 0,P,true",
        "jit-class-new 7,true", "jit-class-derived true,true,Anon"]


def check_symbols(ch):
    dylib_name = ("libChakraCore.dylib" if sys.platform == "darwin"
                  else "libChakraCore.so")
    dylib = os.path.join(os.path.dirname(os.path.abspath(ch)), dylib_name)
    if not os.path.isfile(dylib):
        return "sibling dylib not found: %s" % dylib
    nm = shutil.which("nm") or "/usr/bin/nm"
    rc, out, err = common.run_capture([nm, dylib])
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
