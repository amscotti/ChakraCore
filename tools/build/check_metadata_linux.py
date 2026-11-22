#!/usr/bin/env python3
#-------------------------------------------------------------------------------------------------------
# Copyright (c) ChakraCore Project Contributors. All rights reserved.
# Licensed under the MIT license. See LICENSE.txt file in the project root for full license information.
#-------------------------------------------------------------------------------------------------------
"""B4.3 / Linux multi-profile check gate: artifact metadata assertions.

Sub-checks against the install prefix (zig-out for test,
out/<profile> for debug-static / release-shared):

  1. ELF identity of ch, GCStress and (shared profiles) libChakraCore.so
     (pure-python ELF header parse): 64-bit LSB x86-64; ch and GCStress
     are DYN (PIE -- ch by explicit -fPIE/-pie, GCStress by explicit
     -pie spelling Ubuntu clang's link-PIE default, which zig lacks);
     the .so is DYN.
  2. Dynamic section (`readelf -d`): on shared profiles, SONAME ==
     libChakraCore.so on the .so; NEEDED carries the ICU pair
     (libicuuc.so.<N>, libicui18n.so.<N>) and NO C++ runtime (libc++
     links statically; the reference .so has no libstdc++ NEEDED either,
     via -static-libstdc++ -- same property, different runtime). On
     debug-static, ch and GCStress carry NO libChakraCore in NEEDED and
     still carry no C++ runtime.
  3. Version-script exports (`nm -D --defined-only`) on shared profiles:
     the EXACT committed set for the .so (tools/build/linux_exports_so.txt:
     237 symbols incl. DllMain + Js* at B4 record time; release-shared is
     234 because non-test profiles omit the three JsPrivate* debug APIs
     guarded by ENABLE_DEBUG_CONFIG_OPTIONS -- export membership is source-
     and script-determined, so any other delta is a real finding). Provenance honesty: the B4 record does NOT
     document whether linux_exports_*.txt were captured from the
     legacy reference artifacts or from the Zig-built artifacts,
     so today this gate proves stability against the committed
     lists, not parity with the legacy reference. Before any
     parity claim, the lists must be regenerated from the legacy
     artifacts on the first native host and diffed against the
     committed files (the third field of every 3-field
     `nm -D --defined-only` line regardless of its type letter
     -- exactly what nm_defined below collects -- sorted):

         nm -D --defined-only <legacy libChakraCore.so> \
           | awk 'NF==3 {print $3}' | LC_ALL=C sort \
           > /tmp/linux_exports_so.txt
         nm -D --defined-only <legacy ch> \
           | awk 'NF==3 {print $3}' | LC_ALL=C sort \
           > /tmp/linux_exports_ch.txt
         diff /tmp/linux_exports_so.txt linux_exports_so.txt
         diff /tmp/linux_exports_ch.txt linux_exports_ch.txt

     An empty export reference fails loudly in both export checks:
     an emptied list must never make an exact-set or subset gate
     vacuous. ch is a MEMBERSHIP gate, not an
     exact-set gate: GNU ld constrains ch's --export-dynamic by the
     version script (reference: exactly OnChakraCoreLoadedEntry),
     but LLD has no equivalent spelling -- --export-dynamic exports
     everything (1248 here), without it nothing is dynamic (not even
     the required symbol), and both selective flags
     (--export-dynamic-symbol, --dynamic-list) are unsupported
     linker args (all probed B4.3). The gate asserts the reference
     set is covered; the surplus count is recorded, never gated
     (gating it would couple the gate to compiler internals).
  4. Configs: pal/src/config.h present with Linux markers (the
     online gate already verified values at configure time);
     lib/wabt/config.h sha256 == the reference hash (byte-exact) for
     test and debug-static, and absent on release-shared (Release
     disables WABT).
  5. The 4 staged public headers present, nonempty and byte-identical
     to their lib/ sources; on shared profiles, bin/ and lib/ .so copies
     identical; on debug-static, no .so and no singular archives, plus
     required JsRT symbols defined in libChakraCoreStatic.a; archives
     present with parameterized member counts (base static 681
     (680 static_order C/C++/ASM TUs + u8a_codec.o) minus 91 when
     --no-jit and minus 37 on release-shared; Pal 90, Codex 1,
     PlatformAgnostic 11 on shared profiles; `ar t`), and
     libChakraCoreStatic.a contains u8a_codec.o.
  6. Phase-4 symbol & Node-API checks: zero leaked `zig_u8a_*` dynamic
     exports on ch, GCStress, libChakraCore.so, or (when staged)
     libChakraNodeApi.so; tools/build/nodeapi_exports.txt carries the
     161 Node-API v10 + host exports (checked against libChakraNodeApi.so
     when present in the prefix).
  7. The `.profile` marker reads `<profile>`.

Every sub-check fails loudly with diagnostics on mismatch.

Usage: check_metadata_linux.py <prefix> [--profile test|debug-static|release-shared]
       [--icu-major N] [--no-jit] [--self-test]
Defaults: --profile test --icu-major 74 (JIT enabled).
Exit code: 0 iff every sub-check passes.
"""

import hashlib
import os
import struct
import subprocess
import sys
import tempfile

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import common

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(HERE))
EXPORTS_SO = os.path.join(HERE, "linux_exports_so.txt")
EXPORTS_CH = os.path.join(HERE, "linux_exports_ch.txt")
NODEAPI_EXPORTS = os.path.join(HERE, "nodeapi_exports.txt")
EXPECTED_NODEAPI_EXPORT_COUNT = 161
FORBIDDEN_EXPORT_PREFIXES = ("zig_u8a_",)
REQUIRED_STATIC_ARCHIVE_MEMBERS = ("u8a_codec.o",)

# Non-test profiles compile without ENABLE_DEBUG_CONFIG_OPTIONS, so these
# Jsrt.cpp CHAKRA_API definitions are absent by design. Keep the exact-set
# gate for every other symbol.
RELEASE_SHARED_ABSENT_EXPORTS = frozenset((
    "JsPrivateCollectGarbageSkipStack",
    "JsPrivateDetachArrayBuffer",
    "JsPrivateFreeDetachedArrayBuffer",
))

PROFILES = ("test", "debug-static", "release-shared")
DEFAULT_ICU_MAJOR = "74"
BACKEND_TU_COUNT = 91
WABT_TU_COUNT = 37
STATIC_ARCHIVE_BASE = 681

WANT_SONAME = "libChakraCore.so"
WANT_ICU_NEEDED = ("libicuuc.so.74", "libicui18n.so.74")
# No C++ runtime may appear in NEEDED (static linkage both sides).
FORBID_NEEDED_SUBSTRS = ("libstdc++", "libc++")
WANT_WABT_SHA256 = "4d365a89ec93f99a52137a5db99446514892cf928b2ff4a824ca997a271749d9"
WANT_MEMBERS = {
    # 680 static_order C/C++/ASM TUs + 1 u8a_codec.o = 681 members
    # (tables_linux.zig compiles 695 C/C++/ASM TUs + u8a_codec.zig).
    "lib/libChakraCoreStatic.a": STATIC_ARCHIVE_BASE,
    "lib/libChakra.Pal.Singular.a": 90,
    "lib/libChakra.Common.Codex.Singular.a": 1,
    "lib/libChakra.Runtime.PlatformAgnostic.Singular.a": 11,
}
SINGULAR_ARCHIVES = (
    "lib/libChakra.Pal.Singular.a",
    "lib/libChakra.Common.Codex.Singular.a",
    "lib/libChakra.Runtime.PlatformAgnostic.Singular.a",
)
REQUIRED_STATIC_JSRT_SYMBOLS = (
    "JsCreateRuntime",
    "JsCreateContext",
    "JsRunScriptWithParserState",
    "JsDisposeRuntime",
    "JsSetCurrentContext",
    "JsGetCurrentContext",
    "JsIdle",
    "JsCollectGarbage",
)
EXECUTABLES = ("bin/ch", "bin/GCStress")
SOLIBS = ("bin/libChakraCore.so", "lib/libChakraCore.so")
HEADERS = (
    ("include/ChakraCore.h", "lib/Jsrt/ChakraCore.h"),
    ("include/ChakraCommon.h", "lib/Jsrt/ChakraCommon.h"),
    ("include/ChakraDebug.h", "lib/Jsrt/ChakraDebug.h"),
    ("include/ChakraCoreVersion.h", "lib/Common/ChakraCoreVersion.h"),
)
PAL_MARKERS = (
    "#define PAL_PT_ATTACH PTRACE_ATTACH\n",
    "#define HAVE_SIGRETURN 1\n",
    "#define HAVE_SYSCTL 0\n",
)

ET_EXEC, ET_DYN, EM_X86_64 = 2, 3, 62


def parse_elf_header(path):
    """Return (class_bits, little, machine, etype) or raise ValueError."""
    with open(path, "rb") as f:
        hdr = f.read(64)
    if len(hdr) < 64 or hdr[:4] != b"\x7fELF":
        raise ValueError("%s: not an ELF file" % path)
    ei_class, ei_data = hdr[4], hdr[5]
    if ei_class == 1:
        bits = 32
    elif ei_class == 2:
        bits = 64
    else:
        raise ValueError("%s: bad EI_CLASS %d" % (path, ei_class))
    if ei_data == 1:
        endian = "<"
    elif ei_data == 2:
        endian = ">"
    else:
        raise ValueError("%s: bad EI_DATA %d" % (path, ei_data))
    etype, machine = struct.unpack(endian + "HH", hdr[16:20])
    return bits, ei_data == 1, machine, etype


def check_elf(path, want_type):
    try:
        bits, little, machine, etype = parse_elf_header(path)
    except (OSError, ValueError) as e:
        return str(e)
    print("check_metadata_linux: elf %s -> %d-bit %s machine=%d type=%d"
          % (path, bits, "LSB" if little else "MSB", machine, etype))
    if bits != 64 or not little or machine != EM_X86_64:
        return ("%s: want 64-bit LSB x86-64, have %d-bit %s machine=%d"
                % (path, bits, "LSB" if little else "MSB", machine))
    if etype != want_type:
        return ("%s: want ELF type %d, have %d"
                % (path, want_type, etype))
    return None


def readelf_dynamic(path):
    """Return (soname_or_None, [needed...]) via readelf -d."""
    rc, out, err = common.run_capture(["readelf", "-d", path])
    if rc != 0:
        raise ValueError("readelf -d %s failed (rc=%d err=%r)"
                         % (path, rc, err))
    soname, needed = None, []
    for line in out.splitlines():
        s = line.strip()
        if "(SONAME)" in s:
            soname = s.split("[", 1)[1].rstrip("]")
        elif "(NEEDED)" in s:
            needed.append(s.split("[", 1)[1].rstrip("]"))
    return soname, needed


def want_icu_needed(icu_major=DEFAULT_ICU_MAJOR):
    """Return expected ICU DT_NEEDED library names for the given major."""
    return ("libicuuc.so.%s" % icu_major, "libicui18n.so.%s" % icu_major)


def validate_icu_major(icu_major):
    """Return True if icu_major is a non-empty positive decimal integer."""
    return bool(icu_major) and icu_major.isdigit() and int(icu_major) > 0


def expected_archive_members(profile="test", jit=True):
    """Return {rel_archive_path: member_count} for the given profile/JIT mode."""
    if profile not in PROFILES:
        raise ValueError("unknown profile %r" % profile)
    static_count = STATIC_ARCHIVE_BASE
    if profile == "release-shared":
        static_count -= WABT_TU_COUNT
    if not jit:
        static_count -= BACKEND_TU_COUNT
    members = {"lib/libChakraCoreStatic.a": static_count}
    if profile != "debug-static":
        members["lib/libChakra.Pal.Singular.a"] = 90
        members["lib/libChakra.Common.Codex.Singular.a"] = 1
        members["lib/libChakra.Runtime.PlatformAgnostic.Singular.a"] = 11
    return members


def check_dynamic_so(path, icu_major=DEFAULT_ICU_MAJOR):
    try:
        soname, needed = readelf_dynamic(path)
    except (OSError, ValueError) as e:
        return str(e)
    print("check_metadata_linux: dynamic %s -> soname=%r needed=%r"
          % (path, soname, needed))
    if soname != WANT_SONAME:
        return "%s: want SONAME %r, have %r" % (path, WANT_SONAME, soname)
    return check_needed(path, needed, icu_major=icu_major)


def check_needed(path, needed, icu_major=DEFAULT_ICU_MAJOR, require_icu=True):
    if require_icu:
        for lib in want_icu_needed(icu_major):
            if lib not in needed:
                return ("%s: NEEDED lacks %s (have %r)"
                        % (path, lib, needed))
    for lib in needed:
        for bad in FORBID_NEEDED_SUBSTRS:
            if bad in lib:
                return ("%s: NEEDED carries C++ runtime %s (want static)"
                        % (path, lib))
    return None


def check_no_chakracore_needed(path, needed):
    """Static executables must not depend on libChakraCore.so."""
    hits = [lib for lib in needed if "libChakraCore" in lib]
    if hits:
        return "%s: NEEDED links %r (want self-contained static)" % (path, hits)
    return None


def check_no_so(prefix):
    """Static profile must not stage shared libraries or singular archives."""
    present = []
    for rel in SOLIBS + SINGULAR_ARCHIVES:
        p = os.path.join(prefix, rel)
        if os.path.lexists(p):
            present.append(rel)
    if present:
        return "static profile must not contain shared artifacts: %r" % present
    return None


def check_absent(prefix, rel):
    p = os.path.join(prefix, rel)
    if os.path.lexists(p):
        return "%s must not exist in this profile" % rel
    return None


def check_profile_marker(prefix, profile="test"):
    path = os.path.join(prefix, ".profile")
    try:
        with open(path) as f:
            marker = f.read()
    except OSError as e:
        return str(e)
    want = "%s\n" % profile
    if marker != want:
        return ".profile reads %r, want %r" % (marker, want)
    return None


def nm_defined(path):
    """Defined dynamic symbol names via nm -D --defined-only."""
    rc, out, err = common.run_capture(
        ["nm", "-D", "--defined-only", path])
    if rc != 0:
        raise ValueError("nm -D %s failed (rc=%d err=%r)"
                         % (path, rc, err))
    names = []
    for line in out.splitlines():
        parts = line.split()
        if len(parts) == 3:
            names.append(parts[2])
    return names


def read_export_ref(ref_file):
    """Sorted nonblank, non-comment symbol names from a committed export list."""
    with open(ref_file) as f:
        return sorted(
            line.strip() for line in f
            if line.strip() and not line.lstrip().startswith("#"))


def check_exports(path, ref_file, omit=()):
    try:
        want = [s for s in read_export_ref(ref_file) if s not in omit]
    except OSError as e:
        return "cannot read %s: %s" % (ref_file, e)
    if not want:
        return ("%s: export reference %s is empty; refusing a "
                "vacuous exact-set gate" % (path, ref_file))
    try:
        have = sorted(nm_defined(path))
    except (OSError, ValueError) as e:
        return str(e)
    print("check_metadata_linux: exports %s -> %d defined dynamic "
          "symbols (want %d)" % (path, len(have), len(want)))
    if have != want:
        only_have = sorted(set(have) - set(want))[:8]
        only_want = sorted(set(want) - set(have))[:8]
        return ("%s: export set differs: only-have=%r only-want=%r"
                % (path, only_have, only_want))
    return None


def check_exports_subset(path, ref_file):
    try:
        want = read_export_ref(ref_file)
    except OSError as e:
        return "cannot read %s: %s" % (ref_file, e)
    if not want:
        return ("%s: export reference %s is empty; refusing a "
                "vacuous subset gate" % (path, ref_file))
    try:
        have = sorted(nm_defined(path))
    except (OSError, ValueError) as e:
        return str(e)
    missing = sorted(set(want) - set(have))
    print("check_metadata_linux: exports %s -> %d defined dynamic "
          "symbols, required %d present (superset by design, see docstring)"
          % (path, len(have), len(want)))
    if missing:
        return ("%s: missing required exports: %r (have %d, want %d)"
                % (path, missing[:8], len(have), len(want)))
    return None


def check_file_bytes(path, want_sha=None, markers=()):
    try:
        with open(path, "rb") as f:
            data = f.read()
    except OSError as e:
        return "missing file: %s (%s)" % (path, e)
    if not data:
        return "%s: empty file" % path
    if want_sha is not None:
        have = hashlib.sha256(data).hexdigest()
        print("check_metadata_linux: sha256 %s -> %s" % (path, have[:16]))
        if have != want_sha:
            return ("%s: sha256 %s != reference %s"
                    % (path, have, want_sha))
    text = data.decode("utf-8", errors="replace")
    for m in markers:
        if m not in text:
            return "%s: lacks marker %r" % (path, m)
    return None


def check_header(prefix_rel, src_rel, prefix):
    staged = os.path.join(prefix, prefix_rel)
    src = os.path.join(ROOT, src_rel)
    try:
        with open(staged, "rb") as f:
            have = f.read()
        with open(src, "rb") as f:
            want = f.read()
    except OSError as e:
        return "%s: %s" % (prefix_rel, e)
    if not have:
        return "%s: empty file" % prefix_rel
    if have != want:
        return "%s: differs from %s" % (prefix_rel, src_rel)
    return None


def check_archive(path, want_members):
    rc, out, err = common.run_capture(["ar", "t", path])
    if rc != 0:
        return "ar t %s failed (rc=%d err=%r)" % (path, rc, err)
    members = [line for line in out.splitlines() if line.strip()]
    print("check_metadata_linux: ar %s -> %d members (want %d)"
          % (path, len(members), want_members))
    if len(members) != want_members:
        return ("%s: %d members, want %d"
                % (path, len(members), want_members))
    return None


def check_archive_contains(path, required_members=REQUIRED_STATIC_ARCHIVE_MEMBERS):
    """Archive must contain each expected member object basename (e.g. u8a_codec.o)."""
    rc, out, err = common.run_capture(["ar", "t", path])
    if rc != 0:
        return "ar t %s failed (rc=%d err=%r)" % (path, rc, err)
    basenames = {
        os.path.basename(line.strip())
        for line in out.splitlines()
        if line.strip()
    }
    missing = [m for m in required_members if m not in basenames]
    if missing:
        return "%s: missing required archive members: %r" % (path, missing)
    return None


def find_forbidden_symbol_prefixes(names, forbidden_prefixes=FORBIDDEN_EXPORT_PREFIXES):
    """Return sorted dynamic symbol names starting with any forbidden prefix."""
    return sorted(
        name for name in names
        if any(name.startswith(prefix) for prefix in forbidden_prefixes)
    )


def check_no_leaked_symbol_prefixes(path, forbidden_prefixes=FORBIDDEN_EXPORT_PREFIXES):
    """Fail if any defined dynamic symbol matches a forbidden internal prefix."""
    try:
        names = nm_defined(path)
    except (OSError, ValueError) as e:
        return str(e)
    leaked = find_forbidden_symbol_prefixes(names, forbidden_prefixes)
    if leaked:
        return "%s: leaked internal dynamic symbols %r" % (path, leaked[:8])
    return None


def check_nodeapi_exports_ref(ref_file=NODEAPI_EXPORTS, want_count=EXPECTED_NODEAPI_EXPORT_COUNT):
    """Verify nodeapi_exports.txt carries the exact unique Phase-4 export count."""
    try:
        with open(ref_file) as f:
            raw = [
                line.strip() for line in f
                if line.strip() and not line.lstrip().startswith("#")
            ]
    except OSError as e:
        return "cannot read %s: %s" % (ref_file, e)
    unique = set(raw)
    if len(raw) != len(unique):
        return "%s: contains duplicate symbols" % ref_file
    if len(unique) != want_count:
        return "%s: %d symbols, want %d" % (ref_file, len(unique), want_count)
    return None


def check_static_archive_exports(path, required=REQUIRED_STATIC_JSRT_SYMBOLS):
    """Static archive must define the public JsRT symbols (`nm -g --defined-only`)."""
    rc, out, err = common.run_capture(["nm", "-g", "--defined-only", path])
    if rc != 0:
        return "nm -g --defined-only %s failed (rc=%d err=%r)" % (path, rc, err)
    defined = set()
    for line in out.splitlines():
        parts = line.split()
        if len(parts) == 3:
            defined.add(parts[2])
    missing = [sym for sym in required if sym not in defined]
    if missing:
        return "%s: missing required JsRT symbols: %r" % (path, missing)
    return None


def parse_args(args):
    """Parse CLI args. Return (opts_dict, err_msg)."""
    profile = "test"
    icu_major = DEFAULT_ICU_MAJOR
    jit = True
    positional = []
    i = 0
    while i < len(args):
        a = args[i]
        if a == "--profile":
            if i + 1 >= len(args):
                return None, "--profile requires a value"
            profile = args[i + 1]
            i += 2
        elif a.startswith("--profile="):
            profile = a.split("=", 1)[1]
            i += 1
        elif a == "--icu-major":
            if i + 1 >= len(args):
                return None, "--icu-major requires a value"
            icu_major = args[i + 1]
            i += 2
        elif a.startswith("--icu-major="):
            icu_major = a.split("=", 1)[1]
            i += 1
        elif a == "--no-jit":
            jit = False
            i += 1
        elif a.startswith("-"):
            return None, "unknown option %r" % a
        else:
            positional.append(a)
            i += 1
    if profile not in PROFILES:
        return None, ("invalid --profile %r (expected one of: %s)"
                      % (profile, ", ".join(PROFILES)))
    if not validate_icu_major(icu_major):
        return None, "invalid --icu-major %r (expected positive integer)" % icu_major
    if len(positional) != 1:
        return None, ("usage: check_metadata_linux.py <prefix> "
                      "[--profile test|debug-static|release-shared] "
                      "[--icu-major N] [--no-jit]")
    return {
        "prefix": positional[0],
        "profile": profile,
        "icu_major": icu_major,
        "jit": jit,
    }, None


def self_test():
    """Negative controls: non-ELF/tampered/foreign inputs must fail."""
    bad = 0

    def report(name, cond):
        print("self-test %-44s -> %s"
              % (name, "PASS" if cond else "FAIL"))
        return 0 if cond else 1

    tmp = tempfile.mkdtemp(prefix="check-metadata-linux-selftest-")
    try:
        # ELF header pins on synthetic headers (no toolchain needed).
        good = (b"\x7fELF" + bytes([2, 1, 1, 0]) + b"\x00" * 8 +
                struct.pack("<HHIQQQIHHHHHH", ET_DYN, EM_X86_64,
                            1, 0, 0, 0, 0, 64, 56, 1, 64, 0, 0))
        p_good = os.path.join(tmp, "good.so")
        with open(p_good, "wb") as f:
            f.write(good)
        bad += report("synthetic ELF64/DYN passes elf gate",
                      check_elf(p_good, ET_DYN) is None)
        bad += report("synthetic ELF64/DYN fails EXEC gate",
                      check_elf(p_good, ET_EXEC) is not None)
        p_txt = os.path.join(tmp, "text.txt")
        with open(p_txt, "w") as f:
            f.write("not an elf\n")
        bad += report("text file fails elf gate",
                      check_elf(p_txt, ET_DYN) is not None)
        arm = bytearray(good)
        struct.pack_into("<H", arm, 18, 183)
        p_arm = os.path.join(tmp, "arm.so")
        with open(p_arm, "wb") as f:
            f.write(bytes(arm))
        bad += report("aarch64 ELF fails machine gate",
                      check_elf(p_arm, ET_DYN) is not None)
        # NEEDED pins (pure function, no readelf).
        bad += report("icu pair, no c++ runtime passes needed gate",
                      check_needed("x", ["libicuuc.so.74",
                                        "libicui18n.so.74",
                                        "libc.so.6"]) is None)
        bad += report("missing icu18n fails needed gate",
                      check_needed("x", ["libicuuc.so.74"]) is not None)
        bad += report("libstdc++ NEEDED fails needed gate",
                      check_needed("x", ["libicuuc.so.74",
                                        "libicui18n.so.74",
                                        "libstdc++.so.6"]) is not None)
        # File-bytes pins.
        p_cfg = os.path.join(tmp, "wabt.h")
        with open(p_cfg, "w") as f:
            f.write("#define HAVE_SNPRINTF 1\n")
        want = hashlib.sha256(b"#define HAVE_SNPRINTF 1\n").hexdigest()
        bad += report("matching sha passes bytes gate",
                      check_file_bytes(p_cfg, want_sha=want) is None)
        bad += report("wrong sha fails bytes gate",
                      check_file_bytes(p_cfg, want_sha="0" * 64) is not None)
        bad += report("present marker passes marker gate",
                      check_file_bytes(
                          p_cfg, markers=("#define HAVE_SNPRINTF 1\n",)) is None)
        bad += report("absent marker fails marker gate",
                      check_file_bytes(p_cfg, markers=("NOPE\n",)) is not None)
        bad += report("missing file fails bytes gate",
                      check_file_bytes(os.path.join(tmp, "nope")) is not None)
        # Export-reference pins: an empty reference must fail loudly
        # in both export gates before any nm invocation (an emptied
        # ch list previously made the subset gate vacuous).
        p_empty = os.path.join(tmp, "empty-exports.txt")
        with open(p_empty, "w") as f:
            f.write("\n")
        # Assert the guard's own "empty" wording, not mere failure:
        # a plain `is not None` also passes via the nm error on this
        # text file, so it would not notice the guard being removed.
        empty_exact = check_exports(p_txt, p_empty)
        bad += report("empty ref fails exact-set export gate naming empty",
                      empty_exact is not None and "empty" in empty_exact)
        empty_subset = check_exports_subset(p_txt, p_empty)
        bad += report("empty ref fails subset export gate naming empty",
                      empty_subset is not None and "empty" in empty_subset)
        bad += report("missing ref fails exact-set export gate",
                      check_exports(p_txt, os.path.join(tmp, "nope.txt"))
                      is not None)
        bad += report("missing ref fails subset export gate",
                      check_exports_subset(p_txt,
                                           os.path.join(tmp, "nope.txt"))
                      is not None)
        p_ref = os.path.join(tmp, "exports.txt")
        with open(p_ref, "w") as f:
            f.write("JsCreateRuntime\n")
        bad += report("non-ELF target fails exact-set export gate",
                      check_exports(p_txt, p_ref) is not None)
        bad += report("non-ELF target fails subset export gate",
                      check_exports_subset(p_txt, p_ref) is not None)
        # Archive pins: a synthetic archive passes only at its real
        # member count; a non-archive fails outright. The member
        # count is observed via `ar t` itself (the tool the gate
        # already assumes), so no count convention is hard-coded.
        bad += report("text file fails archive gate",
                      check_archive(p_txt, 1) is not None)
        p_member = os.path.join(tmp, "member.o")
        with open(p_member, "wb") as f:
            f.write(good)
        p_ar = os.path.join(tmp, "synthetic.a")
        rc_ar, _, _ = common.run_capture(["ar", "rc", p_ar, p_member])
        rc_t, out_t, _ = ((common.run_capture(["ar", "t", p_ar])
                           if rc_ar == 0 else (1, "", "")))
        n_members = len([line for line in out_t.splitlines()
                         if line.strip()])
        bad += report("synthetic archive passes at its member count",
                      rc_ar == 0 and rc_t == 0 and n_members > 0
                      and check_archive(p_ar, n_members) is None)
        bad += report("archive count mismatch fails archive gate",
                      rc_ar == 0 and rc_t == 0
                      and check_archive(p_ar, n_members + 1) is not None)
        # Header pins against a real lib/ source: identical staged
        # bytes pass, tampered or missing staged bytes fail.
        staged_rel, src_rel = HEADERS[0]
        with open(os.path.join(ROOT, src_rel), "rb") as f:
            real_header = f.read()
        staged_path = os.path.join(tmp, staged_rel)
        os.makedirs(os.path.dirname(staged_path))
        with open(staged_path, "wb") as f:
            f.write(real_header)
        bad += report("identical staged header passes header gate",
                      check_header(staged_rel, src_rel, tmp) is None)
        with open(staged_path, "wb") as f:
            f.write(real_header + b"\n// tampered\n")
        bad += report("tampered staged header fails header gate",
                      check_header(staged_rel, src_rel, tmp) is not None)
        bad += report("missing staged header fails header gate",
                      check_header("include/NoSuchHeader.h", src_rel, tmp)
                      is not None)

        # ICU major parameterization (default 74 vs explicit e.g. 76).
        bad += report("icu-major 76 tuple matches .76",
                      want_icu_needed("76")
                      == ("libicuuc.so.76", "libicui18n.so.76"))
        bad += report("icu-major 76 passes needed with .76",
                      check_needed("x", ["libicuuc.so.76",
                                         "libicui18n.so.76",
                                         "libc.so.6"], icu_major="76") is None)
        bad += report("icu-major 76 rejects .74 libs",
                      check_needed("x", ["libicuuc.so.74",
                                         "libicui18n.so.74"],
                                   icu_major="76") is not None)
        bad += report("validate_icu_major accepts digits and rejects bad",
                      validate_icu_major("74") and validate_icu_major("76")
                      and not validate_icu_major("")
                      and not validate_icu_major("0")
                      and not validate_icu_major("abc"))

        # Archive member parameterization across profiles and --no-jit.
        bad += report("test JIT expects 681 static + 3 singular archives",
                      expected_archive_members("test", jit=True) == WANT_MEMBERS)
        bad += report("test --no-jit expects 590 static + 3 singular",
                      expected_archive_members("test", jit=False)[
                          "lib/libChakraCoreStatic.a"] == 590
                      and len(expected_archive_members("test", jit=False)) == 4)
        bad += report("release-shared JIT expects 644 static + 3 singular",
                      expected_archive_members("release-shared", jit=True)[
                          "lib/libChakraCoreStatic.a"] == 644
                      and len(expected_archive_members("release-shared", jit=True)) == 4)
        bad += report("release-shared --no-jit expects 553 static",
                      expected_archive_members("release-shared", jit=False)[
                          "lib/libChakraCoreStatic.a"] == 553)
        bad += report("debug-static JIT expects 681 static, no singulars",
                      expected_archive_members("debug-static", jit=True)
                      == {"lib/libChakraCoreStatic.a": 681})
        bad += report("debug-static --no-jit expects 590 static, no singulars",
                      expected_archive_members("debug-static", jit=False)
                      == {"lib/libChakraCoreStatic.a": 590})

        # Static no-so & self-contained controls.
        static_dir = os.path.join(tmp, "static-clean")
        os.makedirs(static_dir)
        bad += report("clean static dir passes check_no_so",
                      check_no_so(static_dir) is None)
        os.makedirs(os.path.join(static_dir, "lib"))
        with open(os.path.join(static_dir, "lib/libChakraCore.so"), "wb") as f:
            f.write(b"decoy\n")
        bad += report("static dir with .so fails check_no_so",
                      check_no_so(static_dir) is not None)
        bad += report("self-contained NEEDED passes without libChakraCore",
                      check_no_chakracore_needed(
                          "bin/ch", ["libicuuc.so.74", "libc.so.6"]) is None)
        bad += report("NEEDED with libChakraCore.so fails self-contained",
                      check_no_chakracore_needed(
                          "bin/ch", ["libChakraCore.so", "libc.so.6"]) is not None)
        bad += report("GCStress static NEEDED passes without ICU",
                      check_needed("bin/GCStress", ["libc.so.6", "libpthread.so.0"],
                                   require_icu=False) is None)
        bad += report("GCStress static NEEDED rejects libstdc++",
                      check_needed("bin/GCStress", ["libstdc++.so.6"],
                                   require_icu=False) is not None)

        # Profile marker & absent file controls.
        bad += report("missing .profile fails marker gate",
                      check_profile_marker(static_dir, "debug-static")
                      is not None)
        with open(os.path.join(static_dir, ".profile"), "w") as f:
            f.write("debug-static\n")
        bad += report("matching .profile passes marker gate",
                      check_profile_marker(static_dir, "debug-static") is None)
        bad += report("foreign .profile fails marker gate",
                      check_profile_marker(static_dir, "test") is not None)
        bad += report("present wabt/config.h fails check_absent",
                      check_absent(static_dir, ".profile") is not None)
        bad += report("missing wabt/config.h passes check_absent",
                      check_absent(static_dir, "lib/wabt/config.h") is None)

        # CLI argument parsing controls.
        opts, err = parse_args(["out/jit", "--profile", "release-shared",
                                "--icu-major=76", "--no-jit"])
        bad += report("parse_args accepts valid profile/icu/no-jit flags",
                      err is None and opts == {
                          "prefix": "out/jit",
                          "profile": "release-shared",
                          "icu_major": "76",
                          "jit": False,
                      })
        bad += report("parse_args defaults to test/74/jit=True",
                      parse_args(["zig-out"])[0] == {
                          "prefix": "zig-out",
                          "profile": "test",
                          "icu_major": "74",
                          "jit": True,
                      })
        bad += report("parse_args rejects invalid profile",
                      parse_args(["p", "--profile", "bogus"])[1] is not None)
        bad += report("parse_args rejects invalid icu-major",
                      parse_args(["p", "--icu-major", "nope"])[1] is not None)
        bad += report("parse_args rejects unknown flag",
                      parse_args(["p", "--bogus"])[1] is not None)

        # Phase-4 u8a_codec.o archive membership, zero zig_u8a_* leak, and 161 Node-API exports ref.
        p_u8a = os.path.join(tmp, "u8a_codec.o")
        with open(p_u8a, "wb") as f:
            f.write(good)
        p_ar_u8a = os.path.join(tmp, "static_with_u8a.a")
        rc_ar_u8a, _, _ = common.run_capture(["ar", "rcS", p_ar_u8a, p_member, p_u8a])
        bad += report("archive with u8a_codec.o passes member-presence gate",
                      rc_ar_u8a == 0 and check_archive_contains(p_ar_u8a) is None)
        bad += report("archive without u8a_codec.o fails member-presence gate",
                      rc_ar == 0 and check_archive_contains(p_ar) is not None)
        bad += report("zig_u8a_* leak prefix detected",
                      find_forbidden_symbol_prefixes(["JsCreateRuntime", "zig_u8a_encode_base64"])
                      == ["zig_u8a_encode_base64"])
        bad += report("clean symbols pass zig_u8a_* leak filter",
                      find_forbidden_symbol_prefixes(["JsCreateRuntime", "napi_create_int32"]) == [])
        bad += report("non-ELF target fails no-leaked-prefix gate",
                      check_no_leaked_symbol_prefixes(p_txt) is not None)
        bad += report("nodeapi_exports.txt passes at 161 exports",
                      check_nodeapi_exports_ref(NODEAPI_EXPORTS, EXPECTED_NODEAPI_EXPORT_COUNT) is None)
        bad += report("nodeapi_exports.txt fails at mismatched count 160",
                      check_nodeapi_exports_ref(NODEAPI_EXPORTS, 160) is not None)
    finally:
        import shutil
        shutil.rmtree(tmp, ignore_errors=True)

    print("check_metadata_linux self-test: %s"
          % ("ALL NEGATIVE CONTROLS BEHAVE" if bad == 0
             else "%d CONTROL(S) MISBEHAVE" % bad))
    return 1 if bad else 0


def main(argv):
    args = list(argv[1:])
    if "--self-test" in args:
        return self_test()
    opts, err = parse_args(args)
    if err is not None:
        print("check_metadata_linux: %s" % err, file=sys.stderr)
        return 2
    prefix = opts["prefix"]
    profile = opts["profile"]
    icu_major = opts["icu_major"]
    jit = opts["jit"]
    is_static = (profile == "debug-static")
    is_release = (profile == "release-shared")

    failures = []
    # 1. ELF identity.
    for rel in EXECUTABLES:
        failures.append((rel, check_elf(os.path.join(prefix, rel), ET_DYN)))
    if not is_static:
        for rel in SOLIBS:
            failures.append((rel, check_elf(os.path.join(prefix, rel), ET_DYN)))

    # 2. Dynamic section.
    if is_static:
        failures.append(("no-so", check_no_so(prefix)))
        for exe in EXECUTABLES:
            try:
                _, exe_needed = readelf_dynamic(os.path.join(prefix, exe))
                print("check_metadata_linux: dynamic %s -> needed=%r"
                      % (exe, exe_needed))
                failures.append((exe + "-needed",
                                 check_needed(exe, exe_needed,
                                              icu_major=icu_major,
                                              require_icu=(exe == "bin/ch"))))
                failures.append((exe + "-self-contained",
                                 check_no_chakracore_needed(exe, exe_needed)))
            except (OSError, ValueError) as e:
                failures.append((exe + "-needed", str(e)))
    else:
        failures.append(("soname+needed",
                         check_dynamic_so(os.path.join(prefix, SOLIBS[1]),
                                          icu_major=icu_major)))
        try:
            _, ch_needed = readelf_dynamic(os.path.join(prefix, "bin/ch"))
            print("check_metadata_linux: dynamic bin/ch -> needed=%r"
                  % (ch_needed,))
            failures.append(("ch-needed",
                             check_needed("bin/ch", ch_needed,
                                          icu_major=icu_major)))
        except (OSError, ValueError) as e:
            failures.append(("ch-needed", str(e)))

    # 3. Exports & leak checks.
    failures.append(("nodeapi-exports-ref", check_nodeapi_exports_ref()))
    if is_static:
        failures.append(("static-archive-exports",
                         check_static_archive_exports(
                             os.path.join(prefix, "lib/libChakraCoreStatic.a"))))
    else:
        failures.append(("so-exports", check_exports(
            os.path.join(prefix, SOLIBS[1]), EXPORTS_SO,
            omit=(RELEASE_SHARED_ABSENT_EXPORTS if is_release else ()))))
        failures.append(("ch-exports", check_exports_subset(
            os.path.join(prefix, "bin/ch"), EXPORTS_CH)))
        failures.append(("so-no-zig-u8a-leak",
                         check_no_leaked_symbol_prefixes(
                             os.path.join(prefix, SOLIBS[1]))))
    for exe in EXECUTABLES:
        failures.append((exe + "-no-zig-u8a-leak",
                         check_no_leaked_symbol_prefixes(
                             os.path.join(prefix, exe))))
    nodeapi_so = os.path.join(prefix, "lib/libChakraNodeApi.so")
    if os.path.lexists(nodeapi_so):
        failures.append(("nodeapi-so-exports",
                         check_exports_subset(nodeapi_so, NODEAPI_EXPORTS)))
        failures.append(("nodeapi-so-no-zig-u8a-leak",
                         check_no_leaked_symbol_prefixes(nodeapi_so)))

    # 4. Configs.
    failures.append(("pal-config", check_file_bytes(
        os.path.join(prefix, "pal/src/config.h"), markers=PAL_MARKERS)))
    if is_release:
        failures.append(("wabt-config-absent",
                         check_absent(prefix, "lib/wabt/config.h")))
    else:
        failures.append(("wabt-config", check_file_bytes(
            os.path.join(prefix, "lib/wabt/config.h"),
            want_sha=WANT_WABT_SHA256)))

    # 5. Headers, .so copies, archives.
    for staged, src in HEADERS:
        failures.append((staged, check_header(staged, src, prefix)))
    if not is_static:
        try:
            with open(os.path.join(prefix, SOLIBS[0]), "rb") as f:
                a = f.read()
            with open(os.path.join(prefix, SOLIBS[1]), "rb") as f:
                b = f.read()
            failures.append(("so-copies-identical",
                             None if a == b and a else
                             "bin/ and lib/ .so copies differ or empty"))
        except OSError as e:
            failures.append(("so-copies-identical", str(e)))
    for rel, want in expected_archive_members(profile, jit=jit).items():
        archive_path = os.path.join(prefix, rel)
        failures.append((rel, check_archive(archive_path, want)))
        if rel == "lib/libChakraCoreStatic.a":
            failures.append((rel + "-u8a-codec",
                             check_archive_contains(archive_path)))

    # 6. Profile marker.
    failures.append((".profile", check_profile_marker(prefix, profile)))

    bad = [(name, err) for name, err in failures if err is not None]
    if bad:
        print("check_metadata_linux: FAIL:", file=sys.stderr)
        for name, err in bad:
            print("check_metadata_linux: FAIL: [%s] %s" % (name, err),
                  file=sys.stderr)
        return 1
    if is_static:
        print("check_metadata_linux: PASS: profile=%s icu=%s jit=%s static ELF + "
              "NEEDED + archive exports + configs + headers + archives + marker match"
              % (profile, icu_major, jit))
    else:
        print("check_metadata_linux: PASS: profile=%s icu=%s jit=%s ELF + SONAME/NEEDED + "
              "exports (%d/%d) + configs + headers + archives + marker all match"
              % (profile, icu_major, jit,
                 len(read_export_ref(EXPORTS_SO)) -
                 (len(RELEASE_SHARED_ABSENT_EXPORTS) if is_release else 0),
                 len(read_export_ref(EXPORTS_CH))))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
