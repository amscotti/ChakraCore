#!/usr/bin/env python3
#-------------------------------------------------------------------------------------------------------
# Copyright (c) ChakraCore Project Contributors. All rights reserved.
# Licensed under the MIT license. See LICENSE.txt file in the project root for full license information.
#-------------------------------------------------------------------------------------------------------
"""Runtime smoke for the worktree ARM64 macOS JIT profile.

The generic check_smoke.py intentionally uses interpreter-only snippets so
it can run against every profile. This gate is JIT-specific:

  1. the installed ch and its sibling dylib contain the native backend
     (nm finds NativeCodeGenerator; a DISABLE_JIT build has no such symbol).
     Static profiles have no sibling dylib; inspect the executable itself.
  2. a physical script runs under `-mic:1 -bgjit-` and under
     `-forceNative -off:simpleJit -bgJitDelay:0` (or with default flags on
     Release builds where the debug flaglist is disabled), producing the
     expected output. The script exercises a hot call, rest arguments,
     apply, and a class definition + instantiation, so the JIT-configured
     interpreter and the native compiler both run. The class leg pins
     LowerNewClassConstructor's aux-slot stores: e14
     (doc/e14-fulljit-sigsegv.md) showed a byte-offset/slot-index mixup
     there reads a null prototype Var out of the class constructor and
     SIGSEGVs every dynapogo-mode `new` -- invisible to a classless
     smoke, which is why this gate missed the 109-test fulljit family;
  3. `verify_bytecode_pure.py --jit --verify` (run as a separate build step)
     checks the JIT builtin bytecode headers consumed by this profile.

Usage: check_jit.py <ch> [--self-test]
Exit code: 0 iff every sub-check passes.
"""

import os
import shutil
import struct
import subprocess
import sys
import tempfile
from unittest import mock

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import check_metadata_windows
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


def _is_elf_or_macho_magic(path):
    """True for ELF or Mach-O (32/64/fat) header magic."""
    try:
        with open(path, "rb") as f:
            magic = f.read(4)
    except OSError:
        return False
    return magic in (
        b"\x7fELF",
        b"\xcf\xfa\xed\xfe",
        b"\xce\xfa\xed\xfe",
        b"\xfe\xed\xfa\xcf",
        b"\xfe\xed\xfa\xce",
        b"\xca\xfe\xba\xbe",
    )


def _has_pe_magic(path):
    """True if path has DOS 'MZ' magic and a valid 'PE\\x00\\x00' header."""
    try:
        with open(path, "rb") as f:
            data = f.read()
    except OSError:
        return False
    if len(data) < 64 or data[:2] != b"MZ":
        return False
    e_lfanew = struct.unpack_from("<I", data, 0x3C)[0]
    if e_lfanew < 64 or e_lfanew + 4 > len(data):
        return False
    return data[e_lfanew:e_lfanew + 4] == b"PE\x00\x00"


def _is_pe_binary(path):
    """True for a valid PE32+ image (MZ + PE\\x00\\x00 + magic 0x020B)."""
    try:
        check_metadata_windows.parse_pe(path)
        return True
    except (OSError, ValueError, UnicodeDecodeError):
        return False


def _is_native_binary(path):
    """True for an ELF, Mach-O, or PE32+ binary; false for test stubs/scripts."""
    if _is_elf_or_macho_magic(path):
        return True
    return _is_pe_binary(path)


def is_pe_target(ch):
    """Return True iff ch (or its sibling ChakraCore.dll) is a PE binary."""
    if _is_elf_or_macho_magic(ch):
        return False
    if _has_pe_magic(ch) or _is_pe_binary(ch):
        return True
    ch_dir = os.path.dirname(os.path.abspath(ch))
    win_dll = os.path.join(ch_dir, "ChakraCore.dll")
    if os.path.isfile(win_dll) and (_has_pe_magic(win_dll) or _is_pe_binary(win_dll)):
        return True
    return False


def check_symbols(ch):
    ch_dir = os.path.dirname(os.path.abspath(ch))
    win_dll = os.path.join(ch_dir, "ChakraCore.dll")
    if is_pe_target(ch) or os.path.isfile(win_dll):
        expect_dll = os.path.isfile(win_dll)
        target = win_dll if expect_dll else ch
        try:
            info = check_metadata_windows.parse_pe(target)
        except (OSError, ValueError, UnicodeDecodeError) as e:
            return "PE32+ validation failed for %s: %s" % (target, e)
        if info["machine"] != check_metadata_windows.IMAGE_FILE_MACHINE_AMD64:
            return ("%s: expected AMD64 machine 0x%04x, got 0x%04x"
                    % (target, check_metadata_windows.IMAGE_FILE_MACHINE_AMD64,
                       info["machine"]))
        chars = info["characteristics"]
        if not (chars & check_metadata_windows.IMAGE_FILE_EXECUTABLE_IMAGE):
            return "%s: missing IMAGE_FILE_EXECUTABLE_IMAGE" % target
        is_dll = bool(chars & check_metadata_windows.IMAGE_FILE_DLL)
        if expect_dll and not is_dll:
            return "%s: expected IMAGE_FILE_DLL characteristic" % target
        if not expect_dll and is_dll:
            return "%s: unexpected IMAGE_FILE_DLL on executable" % target
        print("check_jit: PE32+ AMD64 target verified (%s)"
              % os.path.basename(target))
        return None

    dylib_name = ("libChakraCore.dylib" if sys.platform == "darwin"
                  else "libChakraCore.so")
    dylib = os.path.join(ch_dir, dylib_name)
    # Static profiles link the backend into ch and have no sibling dylib.
    # Inspect the executable itself in that case; a stripped binary without
    # the symbol still fails closed.
    if os.path.isfile(dylib):
        target = dylib
    elif _is_native_binary(ch):
        target = ch
    else:
        return "sibling dylib not found: %s" % dylib
    nm = shutil.which("nm") or "/usr/bin/nm"
    rc, out, err = common.run_capture([nm, target])
    if rc != 0:
        return "nm %s exited %d: %s" % (target, rc, err.strip())
    if "NativeCodeGenerator" not in out:
        return ("%s has no NativeCodeGenerator symbol; the JIT was compiled out"
                % target)
    print("check_jit: NativeCodeGenerator symbol present (%s)"
          % os.path.basename(target))
    return None


def flaglist_supported(ch):
    """Release builds intentionally expose no debug/test flaglist."""
    rc, out, err = common.run_capture([ch, "-?"])
    if rc != 0:
        return True
    return "not supported in Release builds" not in (out + err)


def check_backend_trace(ch, script_path=None):
    """Verify native codegen executes via -trace:BackEnd (fails closed on DISABLE_JIT)."""
    tmp = None
    try:
        if script_path is None:
            tmp = tempfile.mkdtemp(prefix="check-jit-trace-")
            script_path = os.path.join(tmp, "smoke.js")
            with open(script_path, "w", encoding="utf-8") as f:
                f.write(SCRIPT)
        rc, out, err = common.run_capture(
            [ch, "-mic:1", "-bgjit-", "-trace:BackEnd", script_path]
        )
    finally:
        if tmp is not None:
            shutil.rmtree(tmp, ignore_errors=True)
    combined = out + err
    if rc != 0:
        return "-trace:BackEnd exited %d (err=%r)" % (rc, err)
    if "BeginBackEnd" not in combined or "EndBackEnd" not in combined:
        return ("%s did not emit BeginBackEnd/EndBackEnd under -trace:BackEnd "
                "(DISABLE_JIT build?)" % ch)
    print("check_jit: BackEnd trace verified (%s)" % os.path.basename(ch))
    return None


def check_run(ch, flags, script_path):
    label = " ".join(flags) if flags else "<default>"
    rc, out, err = common.run_capture([ch] + flags + [script_path])
    print("check_jit: %s -> rc=%d out=%r err=%r"
          % (label, rc, out.strip(), err.strip()))
    if rc != 0:
        return "%s exited %d (err=%r)" % (label, rc, err)
    if out.splitlines() != WANT:
        return "%s stdout %r != %r" % (label, out.splitlines(), WANT)
    return None


def self_test():
    """Positive and negative controls for PE32+/ELF/Mach-O, trace, and run."""
    bad = 0

    def report(name, cond, detail=""):
        print("self-test %-48s -> %s%s"
              % (name, "PASS" if cond else "FAIL",
                 (" (" + detail + ")") if detail else ""))
        return 0 if cond else 1

    with tempfile.TemporaryDirectory(prefix="check-jit-selftest-") as tmp:
        pe_exe = os.path.join(tmp, "ch.exe")
        with open(pe_exe, "wb") as f:
            f.write(check_metadata_windows.build_synthetic_pe(is_dll=False))
        elf_bin = os.path.join(tmp, "ch.elf")
        with open(elf_bin, "wb") as f:
            f.write(b"\x7fELF" + b"\x00" * 60)
        macho_bin = os.path.join(tmp, "ch.macho")
        with open(macho_bin, "wb") as f:
            f.write(b"\xcf\xfa\xed\xfe" + b"\x00" * 60)
        script_stub = os.path.join(tmp, "ch.sh")
        with open(script_stub, "w", encoding="utf-8") as f:
            f.write("#!/bin/sh\necho ok\n")
        pe32_exe = os.path.join(tmp, "x86.exe")
        with open(pe32_exe, "wb") as f:
            f.write(check_metadata_windows.build_synthetic_pe(
                is_dll=False, magic=0x10B))
        corrupt_mz = os.path.join(tmp, "corrupt.exe")
        with open(corrupt_mz, "wb") as f:
            f.write(b"MZ" + b"\x00" * 128)

        bad += report("PE32+ detected by _is_pe_binary & _is_native",
                      _is_pe_binary(pe_exe) and _is_native_binary(pe_exe))
        bad += report("ELF accepted by _is_native, not _is_pe",
                      _is_native_binary(elf_bin) and not _is_pe_binary(elf_bin))
        bad += report("Mach-O accepted by _is_native, not _is_pe",
                      _is_native_binary(macho_bin) and not _is_pe_binary(macho_bin))
        bad += report("shell script rejected by both",
                      not _is_native_binary(script_stub)
                      and not _is_pe_binary(script_stub))
        bad += report("PE32 (0x10B) & corrupt MZ rejected",
                      not _is_pe_binary(pe32_exe)
                      and not _is_native_binary(pe32_exe)
                      and not _is_pe_binary(corrupt_mz)
                      and not _is_native_binary(corrupt_mz))

        with mock.patch.object(sys, "platform", "win32"):
            bad += report(
                "is_pe_target inspects magic on win32 (PE vs ELF/Mach-O/corrupt)",
                is_pe_target(pe_exe)
                and is_pe_target(pe32_exe)
                and not is_pe_target(elf_bin)
                and not is_pe_target(macho_bin)
                and not is_pe_target(corrupt_mz)
                and not is_pe_target(script_stub),
            )

        # Static PE validation vs sibling ChakraCore.dll PE validation.
        static_dir = os.path.join(tmp, "static_win")
        os.makedirs(static_dir)
        static_ch = os.path.join(static_dir, "ch.exe")
        with open(static_ch, "wb") as f:
            f.write(check_metadata_windows.build_synthetic_pe(is_dll=False))
        bad += report("static PE ch.exe passes check_symbols",
                      check_symbols(static_ch) is None)

        bad_static_ch = os.path.join(static_dir, "bad_dll_bit.exe")
        with open(bad_static_ch, "wb") as f:
            f.write(check_metadata_windows.build_synthetic_pe(is_dll=True))
        bad += report("static exe with DLL bit fails check_symbols",
                      check_symbols(bad_static_ch) is not None)

        arm_static_ch = os.path.join(static_dir, "arm64.exe")
        with open(arm_static_ch, "wb") as f:
            f.write(check_metadata_windows.build_synthetic_pe(
                is_dll=False, machine=0xAA64))
        bad += report("ARM64 PE fails check_symbols",
                      check_symbols(arm_static_ch) is not None)

        shared_dir = os.path.join(tmp, "shared_win")
        os.makedirs(shared_dir)
        shared_ch = os.path.join(shared_dir, "ch.exe")
        shared_dll = os.path.join(shared_dir, "ChakraCore.dll")
        with open(shared_ch, "wb") as f:
            f.write(check_metadata_windows.build_synthetic_pe(is_dll=False))
        with open(shared_dll, "wb") as f:
            f.write(check_metadata_windows.build_synthetic_pe(is_dll=True))
        bad += report("shared sibling ChakraCore.dll passes check_symbols",
                      check_symbols(shared_ch) is None)

        with open(shared_dll, "wb") as f:
            f.write(check_metadata_windows.build_synthetic_pe(is_dll=False))
        bad += report("ChakraCore.dll without DLL bit fails check_symbols",
                      check_symbols(shared_ch) is not None)
        with open(shared_dll, "wb") as f:
            f.write(check_metadata_windows.build_synthetic_pe(is_dll=True))

        smoke_js = os.path.join(tmp, "smoke.js")
        with open(smoke_js, "w", encoding="utf-8") as f:
            f.write(SCRIPT)
        want_out = "\n".join(WANT) + "\n"

        # BackEnd trace probe controls.
        with mock.patch.object(
            common, "run_capture",
            return_value=(0, "---BeginBackEnd: add---\n---EndBackEnd---\n" + want_out, ""),
        ):
            bad += report("check_backend_trace accepts BeginBackEnd/EndBackEnd",
                          check_backend_trace(shared_ch, smoke_js) is None)
        with mock.patch.object(
            common, "run_capture",
            return_value=(0, want_out, ""),
        ):
            bad += report("check_backend_trace rejects DISABLE_JIT (no trace)",
                          check_backend_trace(shared_ch, smoke_js) is not None)
        with mock.patch.object(
            common, "run_capture",
            return_value=(0, "---BeginBackEnd: add---\n" + want_out, ""),
        ):
            bad += report("check_backend_trace rejects missing EndBackEnd",
                          check_backend_trace(shared_ch, smoke_js) is not None)
        with mock.patch.object(
            common, "run_capture",
            return_value=(1, "---BeginBackEnd: add---\n---EndBackEnd---\n", "crash"),
        ):
            bad += report("check_backend_trace rejects nonzero rc",
                          check_backend_trace(shared_ch, smoke_js) is not None)

        # check_run positive & negative controls.
        with mock.patch.object(common, "run_capture", return_value=(0, want_out, "")):
            bad += report("check_run accepts exact WANT output",
                          check_run(shared_ch, ["-mic:1", "-bgjit-"], smoke_js) is None)
        with mock.patch.object(common, "run_capture", return_value=(0, "wrong\n", "")):
            bad += report("check_run rejects wrong stdout",
                          check_run(shared_ch, ["-mic:1", "-bgjit-"], smoke_js) is not None)
        with mock.patch.object(common, "run_capture", return_value=(1, want_out, "err")):
            bad += report("check_run rejects nonzero rc",
                          check_run(shared_ch, [], smoke_js) is not None)

        # End-to-end main() on PE target: debug-flaglist fails closed on DISABLE_JIT,
        # passes with BackEnd trace, and Release PE passes without debug flaglist.
        def fake_debug_jit(cmd, **_kw):
            if "-?" in cmd:
                return 0, "Usage: ch [flaglist] <source file>\n", ""
            if "-trace:BackEnd" in cmd:
                return 0, "---BeginBackEnd: add---\n---EndBackEnd---\n" + want_out, ""
            return 0, want_out, ""

        def fake_disable_jit(cmd, **_kw):
            if "-?" in cmd:
                return 0, "Usage: ch [flaglist] <source file>\n", ""
            return 0, want_out, ""

        def fake_release_jit(cmd, **_kw):
            if "-?" in cmd:
                return 0, "Note: [flaglist] is not supported in Release builds.\n", ""
            return 0, want_out, ""

        with mock.patch.object(common, "run_capture", side_effect=fake_debug_jit):
            bad += report("main() passes PE debug/test JIT build",
                          main(["check_jit.py", shared_ch]) == 0)
        with mock.patch.object(common, "run_capture", side_effect=fake_disable_jit):
            bad += report("main() fails closed on PE DISABLE_JIT build",
                          main(["check_jit.py", shared_ch]) == 1)
        with mock.patch.object(common, "run_capture", side_effect=fake_release_jit):
            bad += report("main() passes PE Release build w/ default flags",
                          main(["check_jit.py", shared_ch]) == 0)

        # Non-PE (Mach-O / ELF) controls: check_symbols and BackEnd trace (B-8).
        def is_nm(cmd):
            return os.path.splitext(os.path.basename(cmd[0]))[0] == "nm"

        def fake_nm_with_jit(cmd, **_kw):
            if is_nm(cmd):
                return 0, "0000000000001000 T __ZN2Js19NativeCodeGeneratorC1Ev\n", ""
            return fake_debug_jit(cmd)

        def fake_nm_no_symbol(cmd, **_kw):
            if is_nm(cmd):
                return 0, "0000000000001000 T _main\n", ""
            return fake_debug_jit(cmd)

        def fake_nm_fake_nojit(cmd, **_kw):
            if is_nm(cmd):
                return 0, "0000000000001000 T __ZN2Js19NativeCodeGeneratorC1Ev\n", ""
            return fake_disable_jit(cmd)

        def fake_nm_release_jit(cmd, **_kw):
            if is_nm(cmd):
                return 0, "0000000000001000 T __ZN2Js19NativeCodeGeneratorC1Ev\n", ""
            return fake_release_jit(cmd)

        def fake_nm_bad_help_release(cmd, **_kw):
            if is_nm(cmd):
                return 0, "0000000000001000 T __ZN2Js19NativeCodeGeneratorC1Ev\n", ""
            if "-?" in cmd:
                return 1, "Note: [flaglist] is not supported in Release builds.\n", ""
            return 0, want_out, ""

        for kind_name, bin_path in (("Mach-O", macho_bin), ("ELF", elf_bin)):
            with mock.patch.object(common, "run_capture", side_effect=fake_nm_no_symbol):
                bad += report("%s missing NativeCodeGenerator fails check_symbols" % kind_name,
                              check_symbols(bin_path) is not None)
            with mock.patch.object(common, "run_capture", side_effect=fake_nm_with_jit):
                bad += report("main() passes %s JIT build with BackEnd trace" % kind_name,
                              main(["check_jit.py", bin_path]) == 0)
            with mock.patch.object(common, "run_capture", side_effect=fake_nm_fake_nojit):
                bad += report("main() fails closed on %s fake no-JIT (no trace)" % kind_name,
                              main(["check_jit.py", bin_path]) == 1)
            with mock.patch.object(common, "run_capture", side_effect=fake_nm_release_jit):
                bad += report("main() passes %s Release JIT build w/ default flags" % kind_name,
                              main(["check_jit.py", bin_path]) == 0)
            with mock.patch.object(common, "run_capture", side_effect=fake_nm_bad_help_release):
                bad += report("main() rejects %s nonzero -? claiming Release" % kind_name,
                              main(["check_jit.py", bin_path]) == 1)

    print("check_jit self-test: %s"
          % ("ALL NEGATIVE CONTROLS BEHAVE" if bad == 0
             else "%d CONTROL(S) MISBEHAVE" % bad))
    return 1 if bad else 0


def main(argv):
    if "--self-test" in argv[1:]:
        return self_test()
    if len(argv) != 2:
        print("usage: check_jit.py <ch> [--self-test]", file=sys.stderr)
        return 2
    ch = argv[1]
    if not os.path.isfile(ch):
        print("check_jit: missing ch %r" % ch, file=sys.stderr)
        return 2

    pe_mode = is_pe_target(ch)
    failure = check_symbols(ch)
    if failure:
        print("check_jit: FAIL: %s" % failure, file=sys.stderr)
        return 1

    has_flaglist = flaglist_supported(ch)
    flag_sets = (
        (["-mic:1", "-bgjit-"],
         ["-forceNative", "-off:simpleJit", "-bgJitDelay:0"])
        if has_flaglist else ([],)
    )

    tmp = tempfile.mkdtemp(prefix="check-jit-")
    try:
        script_path = os.path.join(tmp, "smoke.js")
        with open(script_path, "w", encoding="utf-8") as f:
            f.write(SCRIPT)
        if has_flaglist:
            failure = check_backend_trace(ch, script_path)
            if failure:
                print("check_jit: FAIL: %s" % failure, file=sys.stderr)
                return 1
        for flags in flag_sets:
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

    if not has_flaglist:
        kind = "PE32+ validation" if pe_mode else "symbol"
        print("check_jit: PASS (release build: %s + default-flags runtime "
              "verified; debug flaglist skipped)" % kind)
    else:
        print("check_jit: PASS")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
