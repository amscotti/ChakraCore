#!/usr/bin/env python3
#-------------------------------------------------------------------------------------------------------
# Copyright (c) ChakraCore Project Contributors. All rights reserved.
# Licensed under the MIT license. See LICENSE.txt file in the project root for full license information.
#-------------------------------------------------------------------------------------------------------
"""B6 / Windows x64 check gate: pure-Python PE32+ artifact metadata assertions.

Sub-checks against the install prefix (zig-out for test, out/<profile>
for debug-static / release-shared):

  1. Pure-Python PE32+ identity & header checks on bin/ch.exe,
     bin/GCStress.exe, and (shared profiles) bin/ChakraCore.dll:
       - DOS 'MZ' + PE signature 'PE\\0\\0'
       - COFF Machine == 0x8664 (AMD64), Optional Magic == 0x20B (PE32+)
       - IMAGE_FILE_DLL (0x2000) set on ChakraCore.dll and unset on .exe
       - IMAGE_SUBSYSTEM_WINDOWS_CUI (3) on ch.exe and GCStress.exe
  2. Shared profiles (test, release-shared):
       - bin/ChakraCore.dll present with required JsRT exports, export
         floor (>= 100), and zero leaked zig_u8a_* exports
       - lib/ChakraCore.lib present, non-empty, valid COFF archive magic
       - Optional --icu icu4c|windows|none import DLL validation
  3. Static profile (debug-static):
       - No ChakraCore.dll in bin/ or lib/
       - bin/ch.exe and bin/GCStress.exe do NOT import ChakraCore.dll
       - lib/ChakraCore.lib present, non-empty, valid COFF archive magic
  4. Staged public headers (4 files) present, non-empty, byte-identical
     to their lib/ sources.
  5. `.profile` marker matches `<profile>\\n` (tolerating CRLF).

Usage:
  python3 tools/build/check_metadata_windows.py <prefix>
      [--profile test|debug-static|release-shared]
      [--no-jit] [--icu icu4c|windows|none]
  python3 tools/build/check_metadata_windows.py --self-test

Exit code: 0 iff every sub-check passes.
"""

import hashlib
import os
import shutil
import struct
import sys
import tempfile

PROFILES = ("test", "debug-static", "release-shared")
ICU_MODES = ("icu4c", "windows", "none")

IMAGE_FILE_MACHINE_AMD64 = 0x8664
IMAGE_FILE_EXECUTABLE_IMAGE = 0x0002
IMAGE_FILE_DLL = 0x2000
PE32_PLUS_MAGIC = 0x020B
IMAGE_SUBSYSTEM_WINDOWS_CUI = 3

MIN_EXPORTS = 100
REQUIRED_EXPORTS = (
    "JsCreateRuntime",
    "JsCreateContext",
    "JsRunScriptWithParserState",
    "JsDisposeRuntime",
    "JsSetCurrentContext",
    "JsGetCurrentContext",
    "JsIdle",
    "JsCollectGarbage",
)

EXECUTABLES = ("bin/ch.exe", "bin/GCStress.exe")
SHARED_DLL = "bin/ChakraCore.dll"
FORBIDDEN_STATIC_DLLS = ("bin/ChakraCore.dll", "lib/ChakraCore.dll")
IMPORT_OR_STATIC_LIB = "lib/ChakraCore.lib"
HEADERS = (
    ("include/ChakraCore.h", "lib/Jsrt/ChakraCore.h"),
    ("include/ChakraCommon.h", "lib/Jsrt/ChakraCommon.h"),
    ("include/ChakraDebug.h", "lib/Jsrt/ChakraDebug.h"),
    ("include/ChakraCoreVersion.h", "lib/Common/ChakraCoreVersion.h"),
)


def repo_root():
    return os.path.dirname(os.path.dirname(os.path.dirname(
        os.path.abspath(__file__))))


def parse_pe(path):
    """Parse PE32+ header, exports, and imported DLL names purely in Python."""
    with open(path, "rb") as f:
        data = f.read()
    if len(data) < 64 or data[:2] != b"MZ":
        raise ValueError("%s: not a PE/MZ image" % path)
    e_lfanew = struct.unpack_from("<I", data, 0x3C)[0]
    if e_lfanew < 64 or e_lfanew + 24 > len(data):
        raise ValueError("%s: invalid e_lfanew 0x%x" % (path, e_lfanew))
    if data[e_lfanew:e_lfanew + 4] != b"PE\x00\x00":
        raise ValueError("%s: missing PE signature" % path)

    coff_off = e_lfanew + 4
    machine, num_sections, _ts, _sym_ptr, _num_syms, opt_size, chars = (
        struct.unpack_from("<HHIIIHH", data, coff_off)
    )
    opt_off = coff_off + 20
    if opt_size < 112 or opt_off + opt_size > len(data):
        raise ValueError("%s: invalid PE optional header size %d"
                         % (path, opt_size))
    magic = struct.unpack_from("<H", data, opt_off)[0]
    if magic != PE32_PLUS_MAGIC:
        raise ValueError("%s: expected PE32+ magic 0x%x, got 0x%x"
                         % (path, PE32_PLUS_MAGIC, magic))

    subsystem = struct.unpack_from("<H", data, opt_off + 68)[0]
    stack_reserve = struct.unpack_from("<Q", data, opt_off + 72)[0]
    num_rva = struct.unpack_from("<I", data, opt_off + 108)[0]
    dd_off = opt_off + 112

    sec_off = opt_off + opt_size
    if sec_off + num_sections * 40 > len(data):
        raise ValueError("%s: truncated PE section table" % path)
    sections = []
    for i in range(num_sections):
        vsize, va, raw_size, raw_ptr = struct.unpack_from(
            "<IIII", data, sec_off + i * 40 + 8
        )
        sections.append((vsize, va, raw_size, raw_ptr))

    def rva_to_offset(rva):
        for vsize, va, raw_size, raw_ptr in sections:
            span = max(vsize, raw_size)
            if va <= rva < va + span:
                delta = rva - va
                if delta >= raw_size or raw_ptr + delta >= len(data):
                    raise ValueError("%s: RVA 0x%x outside raw section data"
                                     % (path, rva))
                return raw_ptr + delta
        if rva < sec_off + num_sections * 40 and rva < len(data):
            return rva
        raise ValueError("%s: RVA 0x%x not mapped to any PE section"
                         % (path, rva))

    exports = []
    if num_rva >= 1 and opt_size >= 112 + 8:
        exp_rva, exp_size = struct.unpack_from("<II", data, dd_off)
        if exp_rva != 0 and exp_size != 0:
            exp_off = rva_to_offset(exp_rva)
            if exp_off + 40 > len(data):
                raise ValueError("%s: truncated IMAGE_EXPORT_DIRECTORY" % path)
            (_ec, _ets, _maj, _min, _nrva, _base, _nfuncs, num_names,
             _afuncs, addr_names, _aords) = struct.unpack_from(
                "<IIHHIIIIIII", data, exp_off
            )
            if num_names > 0:
                names_off = rva_to_offset(addr_names)
                if names_off + num_names * 4 > len(data):
                    raise ValueError("%s: truncated AddressOfNames" % path)
                for i in range(num_names):
                    nrva = struct.unpack_from("<I", data, names_off + i * 4)[0]
                    noff = rva_to_offset(nrva)
                    nul = data.find(b"\x00", noff)
                    if nul == -1:
                        raise ValueError("%s: unterminated export name" % path)
                    exports.append(data[noff:nul].decode("utf-8",
                                                         errors="strict"))

    imports = []
    if num_rva >= 2 and opt_size >= 112 + 16:
        imp_rva, imp_size = struct.unpack_from("<II", data, dd_off + 8)
        if imp_rva != 0 and imp_size != 0:
            imp_off = rva_to_offset(imp_rva)
            idx = 0
            while imp_off + (idx + 1) * 20 <= len(data):
                oft, ts, fc, name_rva, ft = struct.unpack_from(
                    "<IIIII", data, imp_off + idx * 20
                )
                if (oft, ts, fc, name_rva, ft) == (0, 0, 0, 0, 0):
                    break
                if name_rva != 0:
                    noff = rva_to_offset(name_rva)
                    nul = data.find(b"\x00", noff)
                    if nul == -1:
                        raise ValueError("%s: unterminated import DLL name"
                                         % path)
                    imports.append(data[noff:nul].decode("utf-8",
                                                         errors="strict"))
                idx += 1

    return {
        "machine": machine,
        "characteristics": chars,
        "magic": magic,
        "subsystem": subsystem,
        "stack_reserve": stack_reserve,
        "exports": exports,
        "imports": imports,
    }


def check_pe_image(path, *, expect_dll, require_cui=False):
    """Validate PE32+ x64 identity and DLL/EXE flag; return (info, err_or_None)."""
    try:
        info = parse_pe(path)
    except (OSError, ValueError, UnicodeDecodeError) as e:
        return None, str(e)
    if info["machine"] != IMAGE_FILE_MACHINE_AMD64:
        return info, ("%s: expected AMD64 machine 0x%04x, got 0x%04x"
                      % (path, IMAGE_FILE_MACHINE_AMD64, info["machine"]))
    chars = info["characteristics"]
    if not (chars & IMAGE_FILE_EXECUTABLE_IMAGE):
        return info, "%s: missing IMAGE_FILE_EXECUTABLE_IMAGE" % path
    is_dll = bool(chars & IMAGE_FILE_DLL)
    if expect_dll and not is_dll:
        return info, "%s: expected IMAGE_FILE_DLL characteristic" % path
    if not expect_dll and is_dll:
        return info, "%s: unexpected IMAGE_FILE_DLL on executable" % path
    if require_cui and info["subsystem"] != IMAGE_SUBSYSTEM_WINDOWS_CUI:
        return info, ("%s: expected console subsystem (%d), got %d"
                      % (path, IMAGE_SUBSYSTEM_WINDOWS_CUI, info["subsystem"]))
    print("check_metadata_windows: pe %s -> x64 PE32+ %s subsys=%d exports=%d imports=%d"
          % (path, "DLL" if is_dll else "EXE", info["subsystem"],
             len(info["exports"]), len(info["imports"])))
    return info, None


def check_coff_lib(path, rel_label):
    """Verify a Windows .lib (import or static) has valid COFF archive header."""
    if not os.path.isfile(path):
        return "%s: missing library" % rel_label
    try:
        with open(path, "rb") as f:
            head = f.read(68)
        size = os.path.getsize(path)
    except OSError as e:
        return "%s: cannot read (%s)" % (rel_label, e)
    if size <= 8 or not head.startswith(b"!<arch>\n"):
        return "%s: invalid COFF archive header (expected !<arch>\\n)" % rel_label
    if len(head) >= 68 and head[66:68] != b"`\n":
        return "%s: malformed COFF archive first member header" % rel_label
    print("check_metadata_windows: lib %s -> %d bytes (COFF archive ok)"
          % (rel_label, size))
    return None


def check_dll_exports(info, rel_label):
    """Check required JsRT exports, export floor, and zero leaked zig_u8a_*."""
    exports = info["exports"]
    exp_set = set(exports)
    failures = []
    missing = [sym for sym in REQUIRED_EXPORTS if sym not in exp_set]
    leaked = sorted(sym for sym in exp_set if sym.startswith("zig_u8a_"))
    print("check_metadata_windows: exports %s -> %d symbols, required %d/%d, leaked_u8a=%d"
          % (rel_label, len(exports), len(REQUIRED_EXPORTS) - len(missing),
             len(REQUIRED_EXPORTS), len(leaked)))
    if missing:
        failures.append("%s: missing JsRT exports: %s"
                        % (rel_label, ", ".join(missing)))
    if len(exports) < MIN_EXPORTS:
        failures.append("%s: only %d exports, floor is %d"
                        % (rel_label, len(exports), MIN_EXPORTS))
    if leaked:
        failures.append("%s: leaked internal Zig codec exports: %s"
                        % (rel_label, ", ".join(leaked)))
    return failures


def check_icu_imports(info, rel_label, icu_mode):
    """Validate DLL imports match the selected -Dicu mode when specified."""
    if icu_mode is None:
        return None
    lowered = [dll.lower() for dll in info["imports"]]
    has_icu4c = any(d.startswith("icuuc") or d.startswith("icuin")
                    for d in lowered)
    has_win_icu = "icu.dll" in lowered
    if icu_mode == "icu4c":
        if not has_icu4c:
            return ("%s: --icu icu4c expected icuuc*/icuin* import, got %r"
                    % (rel_label, info["imports"]))
    elif icu_mode == "windows":
        if not has_win_icu:
            return ("%s: --icu windows expected icu.dll import, got %r"
                    % (rel_label, info["imports"]))
    elif icu_mode == "none":
        if has_icu4c or has_win_icu:
            return ("%s: --icu none forbids ICU DLL imports, got %r"
                    % (rel_label, info["imports"]))
    return None


def check_no_dll(prefix):
    failures = []
    for rel in FORBIDDEN_STATIC_DLLS:
        if os.path.lexists(os.path.join(prefix, rel)):
            failures.append("%s: DLL present in a static profile" % rel)
    return failures


def check_standalone_exe(info, rel_label):
    hits = [dll for dll in info["imports"]
            if "chakracore" in dll.lower()]
    if hits:
        return ("%s: imports %r in debug-static (not standalone)"
                % (rel_label, hits))
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
        print("check_metadata_windows: header %s -> %d bytes md5=%s %s"
              % (staged_rel, len(staged_bytes), md5,
                 "byte-identical" if staged_bytes == src_bytes
                 else "DIFFERS FROM SOURCE"))
        if staged_bytes != src_bytes:
            failures.append("%s: staged bytes differ from %s"
                            % (staged_rel, src_rel))
    return failures


def check_profile_marker(prefix, profile):
    path = os.path.join(prefix, ".profile")
    if not os.path.isfile(path):
        return ".profile: missing marker (want %r)" % profile
    with open(path, "r", encoding="utf-8") as f:
        got = f.read().replace("\r\n", "\n").strip()
    print("check_metadata_windows: marker %s -> %r" % (path, got))
    if got != profile:
        return (".profile holds %r, require %r: this prefix was built "
                "for another profile" % (got, profile))
    return None


def build_synthetic_pe(*, exports=(), imports=(), is_dll=False,
                       machine=IMAGE_FILE_MACHINE_AMD64,
                       magic=PE32_PLUS_MAGIC,
                       subsystem=IMAGE_SUBSYSTEM_WINDOWS_CUI,
                       exec_flag=True):
    """Build a minimal valid PE32+ image for unit tests and self-test."""
    sec_va = 0x1000
    sec_raw = 0x200
    sec_data = bytearray()

    exp_rva = 0
    exp_size = 0
    if exports:
        names_blob = bytearray()
        name_offsets = []
        for sym in exports:
            name_offsets.append(len(names_blob))
            names_blob.extend(sym.encode("utf-8") + b"\x00")
        dir_rel = len(sec_data)
        addr_names_rel = dir_rel + 40
        strings_rel = addr_names_rel + 4 * len(exports)
        total_exp = 40 + 4 * len(exports) + len(names_blob)
        sec_data.extend(b"\x00" * total_exp)
        struct.pack_into(
            "<IIHHIIIIIII", sec_data, dir_rel,
            0, 0, 0, 0, 0, 1, len(exports), len(exports),
            sec_va + addr_names_rel, sec_va + addr_names_rel,
            sec_va + addr_names_rel,
        )
        for idx, off in enumerate(name_offsets):
            struct.pack_into("<I", sec_data, addr_names_rel + idx * 4,
                             sec_va + strings_rel + off)
        sec_data[strings_rel:strings_rel + len(names_blob)] = names_blob
        exp_rva = sec_va + dir_rel
        exp_size = total_exp

    imp_rva = 0
    imp_size = 0
    if imports:
        while len(sec_data) % 4 != 0:
            sec_data.append(0)
        imp_rel = len(sec_data)
        desc_bytes = (len(imports) + 1) * 20
        names_blob = bytearray()
        name_offsets = []
        for dll in imports:
            name_offsets.append(len(names_blob))
            names_blob.extend(dll.encode("utf-8") + b"\x00")
        sec_data.extend(b"\x00" * (desc_bytes + len(names_blob)))
        strings_rel = imp_rel + desc_bytes
        for idx, off in enumerate(name_offsets):
            struct.pack_into(
                "<IIIII", sec_data, imp_rel + idx * 20,
                1, 0, 0, sec_va + strings_rel + off, 1,
            )
        sec_data[strings_rel:strings_rel + len(names_blob)] = names_blob
        imp_rva = sec_va + imp_rel
        imp_size = desc_bytes

    if not sec_data:
        sec_data.extend(b"\x00" * 16)

    e_lfanew = 0x40
    opt_size = 240
    chars = (IMAGE_FILE_EXECUTABLE_IMAGE if exec_flag else 0) | (
        IMAGE_FILE_DLL if is_dll else 0x0020
    )
    pe = bytearray(sec_raw + len(sec_data))
    pe[0:2] = b"MZ"
    struct.pack_into("<I", pe, 0x3C, e_lfanew)
    pe[e_lfanew:e_lfanew + 4] = b"PE\x00\x00"
    struct.pack_into("<HHIIIHH", pe, e_lfanew + 4,
                     machine, 1, 0, 0, 0, opt_size, chars)
    opt_off = e_lfanew + 24
    struct.pack_into("<H", pe, opt_off, magic)
    struct.pack_into("<H", pe, opt_off + 68, subsystem)
    struct.pack_into("<Q", pe, opt_off + 72, 0x100000)
    struct.pack_into("<I", pe, opt_off + 108, 16)
    struct.pack_into("<II", pe, opt_off + 112, exp_rva, exp_size)
    struct.pack_into("<II", pe, opt_off + 120, imp_rva, imp_size)
    sec_off = opt_off + opt_size
    pe[sec_off:sec_off + 8] = b".rdata\x00\x00"
    struct.pack_into("<IIII", pe, sec_off + 8,
                     len(sec_data), sec_va, len(sec_data), sec_raw)
    pe[sec_raw:] = sec_data
    return bytes(pe)


def build_synthetic_coff_lib(member_name="stub.obj", payload=b"OBJ\x00"):
    """Build a minimal COFF archive ('!<arch>\\n') for unit tests."""
    hdr = (
        ("%-16s" % (member_name + "/")).encode("ascii")[:16]
        + b"0           "
        + b"0     "
        + b"0     "
        + b"100644  "
        + ("%-10d" % len(payload)).encode("ascii")[:10]
        + b"`\n"
    )
    data = b"!<arch>\n" + hdr + payload
    if len(payload) % 2 == 1:
        data += b"\n"
    return data


def parse_args(args):
    profile = "test"
    jit = True
    icu_mode = None
    positional = []
    i = 0
    while i < len(args):
        a = args[i]
        if a == "--profile":
            if i + 1 >= len(args):
                return None, "--profile needs a value"
            profile = args[i + 1]
            i += 2
        elif a.startswith("--profile="):
            profile = a.split("=", 1)[1]
            i += 1
        elif a == "--no-jit":
            jit = False
            i += 1
        elif a == "--icu":
            if i + 1 >= len(args):
                return None, "--icu needs a value"
            icu_mode = args[i + 1]
            i += 2
        elif a.startswith("--icu="):
            icu_mode = a.split("=", 1)[1]
            i += 1
        elif a.startswith("-"):
            return None, "unknown option %r" % a
        else:
            positional.append(a)
            i += 1
    if profile not in PROFILES:
        return None, ("bad --profile %r: expected one of: %s"
                      % (profile, ", ".join(PROFILES)))
    if icu_mode is not None and icu_mode not in ICU_MODES:
        return None, ("bad --icu %r: expected one of: %s"
                      % (icu_mode, ", ".join(ICU_MODES)))
    if len(positional) != 1:
        return None, ("usage: check_metadata_windows.py <prefix> "
                      "[--profile test|debug-static|release-shared] "
                      "[--no-jit] [--icu icu4c|windows|none]")
    return {
        "prefix": positional[0],
        "profile": profile,
        "jit": jit,
        "icu": icu_mode,
    }, None


def run_checks(prefix, profile="test", jit=True, icu_mode=None, root=None):
    """Run all Windows metadata checks against prefix; return failure list."""
    _ = jit  # reserved for profile/JIT-specific archive or flag assertions
    if root is None:
        root = repo_root()
    is_static = (profile == "debug-static")
    failures = []

    err = check_profile_marker(prefix, profile)
    if err is not None:
        failures.append(err)

    for rel in EXECUTABLES:
        path = os.path.join(prefix, *rel.split("/"))
        if not os.path.isfile(path):
            failures.append("%s: missing artifact" % rel)
            continue
        info, err = check_pe_image(path, expect_dll=False, require_cui=True)
        if err is not None:
            failures.append(err)
            continue
        if is_static:
            err = check_standalone_exe(info, rel)
            if err is not None:
                failures.append(err)

    failures.extend(check_headers(prefix, root))

    lib_path = os.path.join(prefix, *IMPORT_OR_STATIC_LIB.split("/"))
    err = check_coff_lib(lib_path, IMPORT_OR_STATIC_LIB)
    if err is not None:
        failures.append(err)

    if is_static:
        failures.extend(check_no_dll(prefix))
    else:
        dll_path = os.path.join(prefix, *SHARED_DLL.split("/"))
        if not os.path.isfile(dll_path):
            failures.append("%s: missing artifact" % SHARED_DLL)
        else:
            info, err = check_pe_image(dll_path, expect_dll=True,
                                       require_cui=False)
            if err is not None:
                failures.append(err)
            else:
                failures.extend(check_dll_exports(info, SHARED_DLL))
                icu_err = check_icu_imports(info, SHARED_DLL, icu_mode)
                if icu_err is not None:
                    failures.append(icu_err)

    return failures


def self_test():
    """Negative controls: every gate must fail against tampered victims."""
    bad = 0

    def report(name, cond):
        print("self-test %-54s -> %s" % (name, "PASS" if cond else "FAIL"))
        return 0 if cond else 1

    tmp = tempfile.mkdtemp(prefix="check-metadata-win-selftest-")
    try:
        root = repo_root()
        garbage = os.path.join(tmp, "garbage.dll")
        with open(garbage, "wb") as f:
            f.write(b"not a PE file\n" * 16)
        _, err = check_pe_image(garbage, expect_dll=True)
        bad += report("garbage victim fails PE parse", err is not None)

        # Machine and PE32 (32-bit) negative controls.
        arm_dll = os.path.join(tmp, "arm64.dll")
        with open(arm_dll, "wb") as f:
            f.write(build_synthetic_pe(is_dll=True, machine=0xAA64))
        _, err = check_pe_image(arm_dll, expect_dll=True)
        bad += report("ARM64 PE fails AMD64 machine gate", err is not None)

        pe32_exe = os.path.join(tmp, "x86.exe")
        with open(pe32_exe, "wb") as f:
            f.write(build_synthetic_pe(is_dll=False, magic=0x10B))
        _, err = check_pe_image(pe32_exe, expect_dll=False)
        bad += report("PE32 (0x10B) fails PE32+ gate", err is not None)

        # DLL vs EXE flag mismatch.
        bad_exe = os.path.join(tmp, "dll_as_exe.exe")
        with open(bad_exe, "wb") as f:
            f.write(build_synthetic_pe(is_dll=True))
        _, err = check_pe_image(bad_exe, expect_dll=False)
        bad += report("DLL image fails EXE gate", err is not None)

        # Valid shared export list (100+ with REQUIRED_EXPORTS) passes.
        full_exports = list(REQUIRED_EXPORTS) + [
            "JsStub%03d" % i for i in range(100)
        ]
        good_dll = os.path.join(tmp, "good.dll")
        with open(good_dll, "wb") as f:
            f.write(build_synthetic_pe(
                exports=full_exports,
                imports=("KERNEL32.dll", "icuuc74.dll", "icuin74.dll"),
                is_dll=True,
            ))
        info, err = check_pe_image(good_dll, expect_dll=True)
        bad += report("synthetic shared DLL passes PE & export gates",
                      err is None and not check_dll_exports(info, "good.dll"))
        bad += report("synthetic shared DLL passes icu4c import gate",
                      check_icu_imports(info, "good.dll", "icu4c") is None)
        bad += report("synthetic shared DLL fails icu=none import gate",
                      check_icu_imports(info, "good.dll", "none") is not None)

        # Missing required JsRT export fails.
        missing_dll = os.path.join(tmp, "missing_jsrt.dll")
        with open(missing_dll, "wb") as f:
            f.write(build_synthetic_pe(
                exports=[s for s in full_exports if s != "JsCreateRuntime"],
                is_dll=True,
            ))
        info_m, _ = check_pe_image(missing_dll, expect_dll=True)
        bad += report("missing JsCreateRuntime fails export gate",
                      any("JsCreateRuntime" in e
                          for e in check_dll_exports(info_m, "missing.dll")))

        # Leaked zig_u8a_* export fails.
        leaked_dll = os.path.join(tmp, "leaked.dll")
        with open(leaked_dll, "wb") as f:
            f.write(build_synthetic_pe(
                exports=full_exports + ["zig_u8a_decode_b64"],
                is_dll=True,
            ))
        info_l, _ = check_pe_image(leaked_dll, expect_dll=True)
        bad += report("leaked zig_u8a_* fails export gate",
                      any("zig_u8a_decode_b64" in e
                          for e in check_dll_exports(info_l, "leaked.dll")))

        # COFF archive check.
        good_lib = os.path.join(tmp, "good.lib")
        with open(good_lib, "wb") as f:
            f.write(build_synthetic_coff_lib())
        bad += report("valid COFF .lib passes lib gate",
                      check_coff_lib(good_lib, "lib/ChakraCore.lib") is None)
        bad += report("garbage file fails lib gate",
                      check_coff_lib(garbage, "lib/ChakraCore.lib") is not None)

        # Full end-to-end synthetic shared prefix passes and static catches DLL.
        shared_prefix = os.path.join(tmp, "shared_prefix")
        for sub in ("bin", "lib", "include"):
            os.makedirs(os.path.join(shared_prefix, sub), exist_ok=True)
        for exe_rel in EXECUTABLES:
            with open(os.path.join(shared_prefix, *exe_rel.split("/")), "wb") as f:
                f.write(build_synthetic_pe(
                    imports=("KERNEL32.dll",), is_dll=False
                ))
        shutil.copy2(good_dll, os.path.join(shared_prefix, "bin", "ChakraCore.dll"))
        shutil.copy2(good_lib, os.path.join(shared_prefix, "lib", "ChakraCore.lib"))
        for staged_rel, src_rel in HEADERS:
            shutil.copy2(os.path.join(root, src_rel),
                         os.path.join(shared_prefix, *staged_rel.split("/")))
        with open(os.path.join(shared_prefix, ".profile"), "w",
                  encoding="utf-8", newline="\n") as f:
            f.write("test\n")
        bad += report("synthetic shared prefix passes full check",
                      run_checks(shared_prefix, profile="test",
                                 icu_mode="icu4c", root=root) == [])

        # Foreign profile marker fails.
        bad += report("foreign profile marker fails",
                      any(".profile" in e
                          for e in run_checks(shared_prefix,
                                              profile="release-shared",
                                              root=root)))

        # Debug-static profile: must fail if DLL is present or exe imports it.
        static_prefix = os.path.join(tmp, "static_prefix")
        for sub in ("bin", "lib", "include"):
            os.makedirs(os.path.join(static_prefix, sub), exist_ok=True)
        for exe_rel in EXECUTABLES:
            with open(os.path.join(static_prefix, *exe_rel.split("/")), "wb") as f:
                f.write(build_synthetic_pe(
                    imports=("KERNEL32.dll",), is_dll=False
                ))
        shutil.copy2(good_lib, os.path.join(static_prefix, "lib", "ChakraCore.lib"))
        for staged_rel, src_rel in HEADERS:
            shutil.copy2(os.path.join(root, src_rel),
                         os.path.join(static_prefix, *staged_rel.split("/")))
        with open(os.path.join(static_prefix, ".profile"), "w",
                  encoding="utf-8", newline="\n") as f:
            f.write("debug-static\n")
        bad += report("synthetic debug-static prefix passes full check",
                      run_checks(static_prefix, profile="debug-static",
                                 root=root) == [])
        with open(os.path.join(static_prefix, "bin", "ch.exe"), "wb") as f:
            f.write(build_synthetic_pe(
                imports=("KERNEL32.dll", "ChakraCore.dll"), is_dll=False
            ))
        bad += report("debug-static rejects ch.exe importing ChakraCore.dll",
                      any("standalone" in e
                          for e in run_checks(static_prefix,
                                              profile="debug-static",
                                              root=root)))
    finally:
        shutil.rmtree(tmp, ignore_errors=True)

    print("check_metadata_windows self-test: %s"
          % ("ALL NEGATIVE CONTROLS BEHAVE" if bad == 0
             else "%d CONTROL(S) MISBEHAVE" % bad))
    return 1 if bad else 0


def main(argv):
    args = list(argv[1:])
    if "--self-test" in args:
        return self_test()
    opts, err = parse_args(args)
    if err is not None:
        print("check_metadata_windows: %s" % err, file=sys.stderr)
        return 2
    prefix = os.path.abspath(opts["prefix"])
    failures = run_checks(
        prefix,
        profile=opts["profile"],
        jit=opts["jit"],
        icu_mode=opts["icu"],
    )
    if failures:
        print("check_metadata_windows: FAIL:", file=sys.stderr)
        for msg in failures:
            print("check_metadata_windows: FAIL: %s" % msg, file=sys.stderr)
        return 1
    print("check_metadata_windows: PASS: PE32+ x64, %s profile, headers, "
          "marker verified" % opts["profile"])
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
