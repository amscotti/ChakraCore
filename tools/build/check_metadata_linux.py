#!/usr/bin/env python3
#-------------------------------------------------------------------------------------------------------
# Copyright (c) ChakraCore Project Contributors. All rights reserved.
# Licensed under the MIT license. See LICENSE.txt file in the project root for full license information.
#-------------------------------------------------------------------------------------------------------
"""B4.3 / Linux multi-profile check gate: artifact metadata assertions.

Sub-checks against the install prefix (zig-out for test,
out/<profile> for debug-static / release-shared, out/sanitize-<mode>
for sanitizer lanes):

  1. ELF identity of ch, GCStress and (shared profiles) libChakraCore.so
     (pure-python ELF header parse): 64-bit LSB x86-64; ch and GCStress
     are DYN (PIE -- ch by explicit -fPIE/-pie, GCStress by explicit
     -pie spelling Ubuntu clang's link-PIE default, which zig lacks);
     the .so is DYN.
  2. Dynamic section (`readelf -d`): on shared profiles, SONAME ==
     libChakraCore.so on the .so; NEEDED carries the ICU pair
     (libicuuc.so.<N>, libicui18n.so.<N>). On non-sanitizer builds, NO
     C++ runtime (libc++ links statically; the reference .so has no
     libstdc++ NEEDED either) and NO sanitizer runtime may appear in
     NEEDED. On sanitizer builds (`address`|`undefined`|`thread`), host
     clang++ may dynamically link libstdc++ or libc++ (not both) and
     may dynamically link a sanitizer runtime matching the active mode
     (never a foreign sanitizer runtime); ICU requirements remain
     unchanged. On debug-static, ch and GCStress carry NO libChakraCore
     in NEEDED.
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
     On sanitizer builds, .so exports still require all committed
     symbols and forbid any non-sanitizer extra exports, while allowing
     only recognized sanitizer runtime/instrumentation symbols for the
     active mode if emitted dynamically. Sanitizer builds also assert
     mode-specific sanitizer ELF symbols (`__asan_*`, `__ubsan_*`,
     `__tsan_*` via `nm -D` / archive `nm -u`) and reject foreign
     sanitizer symbols; non-sanitizer builds assert zero sanitizer
     instrumentation symbols.
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
  7. The `.profile` marker reads `<profile>` (exact `sanitize-<mode>`
     or `<base>-sanitize-<mode>` on sanitizer builds).

Every sub-check fails loudly with diagnostics on mismatch.

Usage: check_metadata_linux.py <prefix>
       [--profile test|debug-static|release-shared|sanitize-<mode>|<base>-sanitize-<mode>]
       [--sanitize none|address|undefined|thread]
       [--icu-major N] [--no-jit] [--self-test]
Defaults: --profile test --icu-major 74 (JIT enabled, no sanitizer).
Exit code: 0 iff every sub-check passes.
"""

import hashlib
import os
import shutil
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
SANITIZE_MODES = ("address", "undefined", "thread")
SANITIZE_BASE_PROFILES = ("test", "debug-static")
DEFAULT_ICU_MAJOR = "74"
BACKEND_TU_COUNT = 91
WABT_TU_COUNT = 37
STATIC_ARCHIVE_BASE = 681

WANT_SONAME = "libChakraCore.so"
WANT_NODEAPI_SONAME = "libChakraNodeApi.so"
WANT_ICU_NEEDED = ("libicuuc.so.74", "libicui18n.so.74")
# No C++ runtime may appear in NEEDED on non-sanitizer builds (static linkage).
FORBID_NEEDED_SUBSTRS = ("libstdc++", "libc++")
SANITIZER_NEEDED_PREFIXES = {
    "address": ("libasan.so", "libclang_rt.asan"),
    "undefined": ("libubsan.so", "libclang_rt.ubsan"),
    "thread": ("libtsan.so", "libclang_rt.tsan"),
}
EXTRA_SANITIZER_NEEDED_PREFIXES = (
    "liblsan.so",
    "libclang_rt.lsan",
    "libmsan.so",
    "libclang_rt.msan",
    "libhwasan.so",
    "libclang_rt.hwasan",
)
SANITIZER_ALLOWED_EXPORT_PREFIXES = {
    "address": (
        "__asan_",
        "__lsan_",
        "__ubsan_",
        "__sanitizer_",
        "__sancov_",
        "__interceptor_",
        "___interceptor_",
        "__odr_asan_",
        "asan.",
    ),
    "undefined": (
        "__ubsan_",
        "__sanitizer_",
        "__sancov_",
        "__interceptor_",
        "___interceptor_",
        "ubsan.",
    ),
    "thread": (
        "__tsan_",
        "__ubsan_",
        "__sanitizer_",
        "__sancov_",
        "__interceptor_",
        "___interceptor_",
        "tsan.",
    ),
}
SANITIZER_REQUIRED_SYM_PREFIXES = {
    "address": ("__asan_",),
    "undefined": ("__ubsan_",),
    "thread": ("__tsan_",),
}
SANITIZER_FORBIDDEN_SYM_PREFIXES = {
    "address": ("__tsan_", "__msan_", "__hwasan_"),
    "undefined": ("__asan_", "__tsan_", "__msan_", "__hwasan_"),
    "thread": ("__asan_", "__msan_", "__hwasan_"),
}
ALL_SANITIZER_SYM_PREFIXES = (
    "__asan_",
    "__ubsan_",
    "__tsan_",
    "__msan_",
    "__hwasan_",
)
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


def resolve_profile(profile, sanitize=None):
    """Return (base_profile, sanitize_mode_or_None, marker_profile) or raise ValueError."""
    saw_explicit_none = False
    opt_san = None
    if sanitize is not None:
        if sanitize == "none":
            saw_explicit_none = True
        elif sanitize in SANITIZE_MODES:
            opt_san = sanitize
        else:
            raise ValueError(
                "invalid --sanitize %r (expected one of: none, %s)"
                % (sanitize, ", ".join(SANITIZE_MODES)))

    if profile in PROFILES:
        if opt_san is not None:
            if profile not in SANITIZE_BASE_PROFILES:
                raise ValueError(
                    "--profile %r is not supported with --sanitize %s on Linux "
                    "(expected one of: %s)"
                    % (profile, opt_san, ", ".join(SANITIZE_BASE_PROFILES)))
            return profile, opt_san, "sanitize-%s" % opt_san
        return profile, None, profile

    for mode in SANITIZE_MODES:
        if profile == "sanitize-%s" % mode:
            if saw_explicit_none:
                raise ValueError(
                    "conflicting --profile %r and --sanitize none" % profile)
            if opt_san is not None and opt_san != mode:
                raise ValueError(
                    "conflicting --profile %r and --sanitize %s"
                    % (profile, opt_san))
            return "test", mode, profile

    for mode in SANITIZE_MODES:
        suffix = "-sanitize-%s" % mode
        if profile.endswith(suffix):
            base = profile[:-len(suffix)]
            if base in PROFILES:
                if base not in SANITIZE_BASE_PROFILES:
                    raise ValueError(
                        "--profile %r is not supported on Linux "
                        "(sanitizer builds support: %s)"
                        % (profile, ", ".join(SANITIZE_BASE_PROFILES)))
                if saw_explicit_none:
                    raise ValueError(
                        "conflicting --profile %r and --sanitize none" % profile)
                if opt_san is not None and opt_san != mode:
                    raise ValueError(
                        "conflicting --profile %r and --sanitize %s"
                        % (profile, opt_san))
                return base, mode, profile

    raise ValueError(
        "invalid --profile %r (expected one of: %s, sanitize-<%s>, "
        "or <%s>-sanitize-<%s>)"
        % (profile, ", ".join(PROFILES), "|".join(SANITIZE_MODES),
           "|".join(SANITIZE_BASE_PROFILES), "|".join(SANITIZE_MODES)))


def expected_archive_members(profile="test", jit=True):
    """Return {rel_archive_path: member_count} for the given profile/JIT mode."""
    base_profile, _, _ = resolve_profile(profile)
    static_count = STATIC_ARCHIVE_BASE
    if base_profile == "release-shared":
        static_count -= WABT_TU_COUNT
    if not jit:
        static_count -= BACKEND_TU_COUNT
    members = {"lib/libChakraCoreStatic.a": static_count}
    if base_profile != "debug-static":
        members["lib/libChakra.Pal.Singular.a"] = 90
        members["lib/libChakra.Common.Codex.Singular.a"] = 1
        members["lib/libChakra.Runtime.PlatformAgnostic.Singular.a"] = 11
    return members


def classify_sanitizer_runtime_needed(lib):
    """Return sanitizer mode ('address'|'undefined'|'thread'|'other') or None."""
    base = os.path.basename(lib)
    for mode, prefixes in SANITIZER_NEEDED_PREFIXES.items():
        if base.startswith(prefixes):
            return mode
    if base.startswith(EXTRA_SANITIZER_NEEDED_PREFIXES):
        return "other"
    return None


def check_dynamic_so(path, icu_major=DEFAULT_ICU_MAJOR, sanitize=None,
                     want_soname=WANT_SONAME, require_icu=True):
    try:
        soname, needed = readelf_dynamic(path)
    except (OSError, ValueError) as e:
        return str(e)
    print("check_metadata_linux: dynamic %s -> soname=%r needed=%r"
          % (path, soname, needed))
    if soname != want_soname:
        return "%s: want SONAME %r, have %r" % (path, want_soname, soname)
    return check_needed(path, needed, icu_major=icu_major,
                        require_icu=require_icu, sanitize=sanitize)


def check_needed(path, needed, icu_major=DEFAULT_ICU_MAJOR, require_icu=True,
                 sanitize=None):
    if sanitize is not None and sanitize not in SANITIZE_MODES:
        raise ValueError("invalid sanitize mode %r" % sanitize)
    if require_icu:
        for lib in want_icu_needed(icu_major):
            if lib not in needed:
                return ("%s: NEEDED lacks %s (have %r)"
                        % (path, lib, needed))
    if sanitize is None:
        for lib in needed:
            for bad in FORBID_NEEDED_SUBSTRS:
                if bad in lib:
                    return ("%s: NEEDED carries C++ runtime %s (want static)"
                            % (path, lib))
            if classify_sanitizer_runtime_needed(lib) is not None:
                return ("%s: NEEDED carries sanitizer runtime %s in "
                        "non-sanitizer profile" % (path, lib))
        return None

    has_libstdcxx = any("libstdc++" in lib for lib in needed)
    has_libcxx = any("libc++" in lib for lib in needed)
    if has_libstdcxx and has_libcxx:
        return ("%s: NEEDED mixes both libstdc++ and libc++ (%r)"
                % (path, needed))
    for lib in needed:
        rt_mode = classify_sanitizer_runtime_needed(lib)
        if rt_mode is not None and rt_mode != sanitize:
            return ("%s: NEEDED carries foreign sanitizer runtime %s "
                    "(want %s)" % (path, lib, sanitize))
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


def parse_nm_symbol_names(nm_out):
    """Extract symbol names from `nm -D` or `nm -u` output."""
    names = []
    for line in nm_out.splitlines():
        parts = line.split()
        if len(parts) == 3:
            names.append(parts[2].split("@", 1)[0])
        elif len(parts) == 2 and parts[0] in ("U", "w", "v"):
            names.append(parts[1].split("@", 1)[0])
    return names


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


def nm_dynamic_symbols(path):
    """All dynamic symbol names (defined and undefined) via `nm -D`."""
    rc, out, err = common.run_capture(["nm", "-D", path])
    if rc != 0:
        raise ValueError("nm -D %s failed (rc=%d err=%r)"
                         % (path, rc, err))
    return parse_nm_symbol_names(out)


def parse_nm_archive_members(out):
    """Parse per-member symbol sets from GNU/LLVM/BSD `nm` output on an archive."""
    members = {}
    current = None
    for raw in out.splitlines():
        line = raw.strip()
        if not line:
            continue
        if line.startswith(("nm:", "llvm-nm:")):
            continue
        if line.endswith(":"):
            header = line[:-1]
            if header.endswith(")") and "(" in header:
                header = header.rsplit("(", 1)[1][:-1]
            member = os.path.basename(header.strip())
            if member and not member.startswith("__.SYMDEF"):
                current = member
                members.setdefault(current, set())
            else:
                current = None
            continue
        toks = line.split()
        if not toks:
            continue
        if current is None:
            continue
        if len(toks) == 1:
            sym = toks[0]
        elif toks[0] in ("U", "u", "w", "W"):
            sym = toks[1]
        elif len(toks) >= 3 and toks[1] in ("U", "u", "w", "W", "T", "t", "D", "d", "B", "b", "R", "r", "S", "s"):
            sym = toks[2]
        else:
            sym = toks[-1]
        members[current].add(sym)
    return members


def _is_ar_archive(path):
    try:
        if os.path.getsize(path) < 128:
            return False
        with open(path, "rb") as f:
            return f.read(8) == b"!<arch>\n"
    except OSError:
        return False


def nm_archive_undefined_by_member(path):
    """Per-member undefined symbol sets in an archive via `nm -u` (plus `ar t`)."""
    rc, out, err = common.run_capture(["nm", "-u", path])
    if rc != 0:
        raise ValueError("nm -u %s failed (rc=%d err=%r)"
                         % (path, rc, err))
    members = parse_nm_archive_members(out)
    if _is_ar_archive(path):
        ar_rc, ar_out, _ = common.run_capture(["ar", "t", path])
        if ar_rc == 0:
            for raw in ar_out.splitlines():
                m = os.path.basename(raw.strip())
                if m and m != "/" and not m.startswith("__.SYMDEF"):
                    members.setdefault(m, set())
    return members


def nm_archive_all_by_member(path):
    """Per-member defined+undefined symbol sets in an archive via `nm`."""
    rc, out, err = common.run_capture(["nm", path])
    if rc != 0:
        raise ValueError("nm %s failed (rc=%d err=%r)"
                         % (path, rc, err))
    return parse_nm_archive_members(out)


def nm_archive_undefined(path):
    """Undefined symbol names in an archive via `nm -u`."""
    rc, out, err = common.run_capture(["nm", "-u", path])
    if rc != 0:
        raise ValueError("nm -u %s failed (rc=%d err=%r)"
                         % (path, rc, err))
    return parse_nm_symbol_names(out)


def read_export_ref(ref_file):
    """Sorted nonblank, non-comment symbol names from a committed export list."""
    with open(ref_file) as f:
        return sorted(
            line.strip() for line in f
            if line.strip() and not line.lstrip().startswith("#"))


def check_export_sets(path, have, want, sanitize=None):
    """Validate defined dynamic symbols against reference list (sanitizer-aware)."""
    if sanitize is not None and sanitize not in SANITIZE_MODES:
        raise ValueError("invalid sanitize mode %r" % sanitize)
    have_sorted = sorted(have)
    want_sorted = sorted(want)
    print("check_metadata_linux: exports %s -> %d defined dynamic "
          "symbols (want %d)" % (path, len(have_sorted), len(want_sorted)))
    if sanitize is None:
        if have_sorted != want_sorted:
            only_have = sorted(set(have_sorted) - set(want_sorted))[:8]
            only_want = sorted(set(want_sorted) - set(have_sorted))[:8]
            return ("%s: export set differs: only-have=%r only-want=%r"
                    % (path, only_have, only_want))
        return None
    only_want = sorted(set(want_sorted) - set(have_sorted))
    if only_want:
        return ("%s: missing required exports (sanitize=%s): %r"
                % (path, sanitize, only_want[:8]))
    allowed = SANITIZER_ALLOWED_EXPORT_PREFIXES[sanitize]
    only_have = sorted(set(have_sorted) - set(want_sorted))
    unexpected = [s for s in only_have if not s.startswith(allowed)]
    if unexpected:
        return ("%s: unexpected non-sanitizer exports (sanitize=%s): %r"
                % (path, sanitize, unexpected[:8]))
    return None


def check_exports(path, ref_file, omit=(), sanitize=None):
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
    return check_export_sets(path, have, want, sanitize=sanitize)


def check_sanitizer_symbols_list(path, names, sanitize):
    """Assert ELF/archive symbols match the active sanitizer mode."""
    if sanitize not in SANITIZE_MODES:
        raise ValueError("invalid sanitize mode %r" % sanitize)
    req = SANITIZER_REQUIRED_SYM_PREFIXES[sanitize]
    forbid = SANITIZER_FORBIDDEN_SYM_PREFIXES[sanitize]
    foreign = sorted({n for n in names if n.startswith(forbid)})
    if foreign:
        return ("%s: carries foreign sanitizer symbols for %s: %r"
                % (path, sanitize, foreign[:8]))
    matched = sorted({n for n in names if n.startswith(req)})
    if not matched:
        return ("%s: lacks %s sanitizer instrumentation symbols (%s)"
                % (path, sanitize, ", ".join(req)))
    return None


def check_no_sanitizer_symbols_list(path, names):
    """Assert ELF/archive symbols carry zero sanitizer instrumentation markers."""
    hits = sorted({n for n in names if n.startswith(ALL_SANITIZER_SYM_PREFIXES)})
    if hits:
        return ("%s: carries unexpected sanitizer symbols in non-sanitizer "
                "profile: %r" % (path, hits[:8]))
    return None


def check_elf_sanitizer_symbols(path, sanitize=None):
    try:
        names = nm_dynamic_symbols(path)
    except (OSError, ValueError) as e:
        return str(e)
    if sanitize is None:
        return check_no_sanitizer_symbols_list(path, names)
    return check_sanitizer_symbols_list(path, names, sanitize)


def _is_asm_archive_member(member):
    return os.path.basename(member).lower().endswith(".s.o")


def check_archive_members_sanitizer_symbols(path, member_undefs, sanitize=None, member_all=None):
    """Verify sanitizer instrumentation coverage per object member in an archive."""
    if not member_undefs:
        return "%s: archive yielded no object members" % path
    if sanitize is None:
        for member, names in sorted(member_undefs.items()):
            err = check_no_sanitizer_symbols_list("%s(%s)" % (path, member), names)
            if err is not None:
                return err
        return None
    if sanitize not in SANITIZE_MODES:
        raise ValueError("invalid sanitize mode %r" % sanitize)
    req = SANITIZER_REQUIRED_SYM_PREFIXES[sanitize]
    forbid = SANITIZER_FORBIDDEN_SYM_PREFIXES[sanitize]
    all_names = set()
    for names in member_undefs.values():
        all_names.update(names)
    if not any(n.startswith(req) for n in all_names):
        return ("%s: lacks %s sanitizer instrumentation symbols (%s)"
                % (path, sanitize, ", ".join(req)))
    for member, names in sorted(member_undefs.items()):
        label = "%s(%s)" % (path, member)
        foreign = sorted({n for n in names if n.startswith(forbid)})
        if foreign:
            return ("%s: carries foreign sanitizer symbols for %s: %r"
                    % (label, sanitize, foreign[:8]))
        if _is_asm_archive_member(member):
            continue
        if any(n.startswith(req) for n in names):
            continue
        if os.path.basename(member) == "u8a_codec.o":
            if member_all is None and os.path.isfile(path):
                try:
                    member_all = nm_archive_all_by_member(path)
                except (OSError, ValueError):
                    member_all = {}
            all_u8a = (member_all or {}).get(member, set()) | set(names)
            if any("debug.panic" in s or "zig_probe_stack" in s for s in all_u8a):
                continue
            return ("%s: lacks %s sanitizer or Zig safety-check symbols"
                    % (label, sanitize))
        if sanitize in ("address", "thread"):
            return ("%s: lacks %s sanitizer instrumentation symbols (%s)"
                    % (label, sanitize, ", ".join(req)))
    return None


def check_archive_sanitizer_symbols(path, sanitize=None):
    try:
        member_undefs = nm_archive_undefined_by_member(path)
    except (OSError, ValueError) as e:
        return str(e)
    return check_archive_members_sanitizer_symbols(path, member_undefs, sanitize=sanitize)


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
    try:
        rc, out, err = common.run_capture(["ar", "t", path])
    except OSError as e:
        return "ar t %s failed (%s)" % (path, e)
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
    try:
        rc, out, err = common.run_capture(["ar", "t", path])
    except OSError as e:
        return "ar t %s failed (%s)" % (path, e)
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
    sanitize = None
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
        elif a == "--sanitize":
            if i + 1 >= len(args):
                return None, "--sanitize requires a value"
            sanitize = args[i + 1]
            i += 2
        elif a.startswith("--sanitize="):
            sanitize = a.split("=", 1)[1]
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
    try:
        base_profile, sanitize_mode, marker_profile = resolve_profile(
            profile, sanitize=sanitize)
    except ValueError as e:
        return None, str(e)
    if not validate_icu_major(icu_major):
        return None, "invalid --icu-major %r (expected positive integer)" % icu_major
    if len(positional) != 1:
        return None, ("usage: check_metadata_linux.py <prefix> "
                      "[--profile test|debug-static|release-shared|"
                      "sanitize-<mode>|<base>-sanitize-<mode>] "
                      "[--sanitize none|address|undefined|thread] "
                      "[--icu-major N] [--no-jit]")
    return {
        "prefix": positional[0],
        "profile": profile,
        "base_profile": base_profile,
        "sanitize": sanitize_mode,
        "marker_profile": marker_profile,
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
        with open(p_cfg, "wb") as f:
            f.write(b"#define HAVE_SNPRINTF 1\n")
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
        have_ar = shutil.which("ar") is not None
        p_member = p_ar = None
        rc_ar = 1
        bad += report("text file fails archive gate",
                      check_archive(p_txt, 1) is not None)
        if have_ar:
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
        else:
            print("self-test synthetic archive passes at its member count"
                  "  -> SKIP (no ar)")
            print("self-test archive count mismatch fails archive gate"
                  "  -> SKIP (no ar)")
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
                          "base_profile": "release-shared",
                          "sanitize": None,
                          "marker_profile": "release-shared",
                          "icu_major": "76",
                          "jit": False,
                      })
        bad += report("parse_args defaults to test/74/jit=True",
                      parse_args(["zig-out"])[0] == {
                          "prefix": "zig-out",
                          "profile": "test",
                          "base_profile": "test",
                          "sanitize": None,
                          "marker_profile": "test",
                          "icu_major": "74",
                          "jit": True,
                      })
        bad += report("parse_args rejects invalid profile",
                      parse_args(["p", "--profile", "bogus"])[1] is not None)
        bad += report("parse_args rejects invalid icu-major",
                      parse_args(["p", "--icu-major", "nope"])[1] is not None)
        bad += report("parse_args rejects unknown flag",
                      parse_args(["p", "--bogus"])[1] is not None)

        # Sanitizer profile resolution, DT_NEEDED, and ELF symbol controls.
        for mode in SANITIZE_MODES:
            sname = "sanitize-%s" % mode
            bad += report("resolve_profile(%s) -> (test, %s, %s)" % (sname, mode, sname),
                          resolve_profile(sname) == ("test", mode, sname))
            bad += report("resolve_profile(test, %s) -> (test, %s, %s)" % (mode, mode, sname),
                          resolve_profile("test", mode) == ("test", mode, sname))
            bad += report("resolve_profile(debug-static, %s) -> (debug-static, %s, %s)"
                          % (mode, mode, sname),
                          resolve_profile("debug-static", mode)
                          == ("debug-static", mode, sname))
            base_san = "debug-static-sanitize-%s" % mode
            bad += report("resolve_profile(%s) -> (debug-static, %s, %s)"
                          % (base_san, mode, base_san),
                          resolve_profile(base_san)
                          == ("debug-static", mode, base_san))
            bad += report("expected_archive_members(%s, jit=False) uses test counts" % sname,
                          expected_archive_members(sname, jit=False)["lib/libChakraCoreStatic.a"] == 590
                          and len(expected_archive_members(sname, jit=False)) == 4)
            bad += report("expected_archive_members(%s, jit=False) uses static counts" % base_san,
                          expected_archive_members(base_san, jit=False)
                          == {"lib/libChakraCoreStatic.a": 590})

        bad += report("resolve_profile(test, none) -> (test, None, test)",
                      resolve_profile("test", "none") == ("test", None, "test"))
        for bad_args in (
            ("release-shared", "address"),
            ("release-shared-sanitize-address", None),
            ("sanitize-address", "thread"),
            ("sanitize-address", "none"),
            ("test-sanitize-address", "undefined"),
            ("test-sanitize-address", "none"),
            ("sanitize-leak", None),
            ("bogus-sanitize-address", None),
            ("test-sanitize-bogus", None),
            ("test", "leak"),
        ):
            rejected = False
            try:
                resolve_profile(*bad_args)
            except ValueError:
                rejected = True
            bad += report("resolve_profile rejects %r" % (bad_args,), rejected)

        opts_san, err_san = parse_args([
            "out/sanitize-address", "--profile", "sanitize-address",
            "--sanitize", "address", "--no-jit",
        ])
        bad += report("parse_args accepts sanitize-address + --sanitize address",
                      err_san is None and opts_san == {
                          "prefix": "out/sanitize-address",
                          "profile": "sanitize-address",
                          "base_profile": "test",
                          "sanitize": "address",
                          "marker_profile": "sanitize-address",
                          "icu_major": "74",
                          "jit": False,
                      })
        opts_dbg_san, err_dbg_san = parse_args([
            "out/sanitize-thread", "--profile=debug-static",
            "--sanitize=thread", "--no-jit",
        ])
        bad += report("parse_args accepts debug-static + --sanitize=thread",
                      err_dbg_san is None and opts_dbg_san == {
                          "prefix": "out/sanitize-thread",
                          "profile": "debug-static",
                          "base_profile": "debug-static",
                          "sanitize": "thread",
                          "marker_profile": "sanitize-thread",
                          "icu_major": "74",
                          "jit": False,
                      })
        bad += report("parse_args rejects missing --sanitize value",
                      parse_args(["p", "--sanitize"])[1] is not None)
        bad += report("parse_args rejects release-shared + --sanitize address",
                      parse_args(["p", "--profile", "release-shared",
                                  "--sanitize", "address"])[1] is not None)
        bad += report("parse_args rejects conflicting profile and --sanitize",
                      parse_args(["p", "--profile", "sanitize-address",
                                  "--sanitize", "thread"])[1] is not None)
        bad += report("CLI returns rc=2 on conflicting sanitize flags",
                      main(["check_metadata_linux.py", tmp, "--profile",
                            "sanitize-address", "--sanitize", "undefined"]) == 2)
        bad += report("CLI returns rc=2 on release-shared + sanitize",
                      main(["check_metadata_linux.py", tmp, "--profile",
                            "release-shared-sanitize-address"]) == 2)

        # Sanitizer DT_NEEDED positive and negative controls.
        bad += report("non-sanitizer rejects libc++ in NEEDED",
                      check_needed("x", ["libicuuc.so.74", "libicui18n.so.74",
                                         "libc++.so.1"]) is not None)
        bad += report("non-sanitizer rejects libasan in NEEDED",
                      check_needed("x", ["libicuuc.so.74", "libicui18n.so.74",
                                         "libasan.so.8"]) is not None)
        bad += report("non-sanitizer rejects libclang_rt.ubsan in NEEDED",
                      check_needed("x", ["libicuuc.so.74", "libicui18n.so.74",
                                         "libclang_rt.ubsan_standalone-x86_64.so"])
                      is not None)
        bad += report("asan accepts ICU + libstdc++.so.6",
                      check_needed("x", ["libicuuc.so.74", "libicui18n.so.74",
                                         "libstdc++.so.6", "libc.so.6"],
                                   sanitize="address") is None)
        bad += report("asan accepts ICU + libc++.so.1 + libc++abi.so.1",
                      check_needed("x", ["libicuuc.so.74", "libicui18n.so.74",
                                         "libc++.so.1", "libc++abi.so.1"],
                                   sanitize="address") is None)
        bad += report("asan accepts ICU + libstdc++.so.6 + libasan.so.8",
                      check_needed("x", ["libicuuc.so.74", "libicui18n.so.74",
                                         "libstdc++.so.6", "libasan.so.8"],
                                   sanitize="address") is None)
        bad += report("ubsan accepts libclang_rt.ubsan runtime",
                      check_needed("x", ["libicuuc.so.74", "libicui18n.so.74",
                                         "libstdc++.so.6",
                                         "libclang_rt.ubsan_standalone-x86_64.so"],
                                   sanitize="undefined") is None)
        bad += report("tsan accepts libtsan.so.2 runtime",
                      check_needed("x", ["libicuuc.so.74", "libicui18n.so.74",
                                         "libstdc++.so.6", "libtsan.so.2"],
                                   sanitize="thread") is None)
        bad += report("asan still requires ICU in NEEDED",
                      check_needed("x", ["libicuuc.so.74", "libstdc++.so.6"],
                                   sanitize="address") is not None)
        bad += report("asan rejects mixed libstdc++ and libc++",
                      check_needed("x", ["libicuuc.so.74", "libicui18n.so.74",
                                         "libstdc++.so.6", "libc++.so.1"],
                                   sanitize="address") is not None)
        bad += report("asan rejects foreign libtsan.so.2",
                      check_needed("x", ["libicuuc.so.74", "libicui18n.so.74",
                                         "libstdc++.so.6", "libtsan.so.2"],
                                   sanitize="address") is not None)
        bad += report("asan rejects foreign libubsan.so.1",
                      check_needed("x", ["libicuuc.so.74", "libicui18n.so.74",
                                         "libstdc++.so.6", "libubsan.so.1"],
                                   sanitize="address") is not None)
        bad += report("ubsan rejects foreign libasan.so.8",
                      check_needed("x", ["libicuuc.so.74", "libicui18n.so.74",
                                         "libstdc++.so.6", "libasan.so.8"],
                                   sanitize="undefined") is not None)
        bad += report("tsan rejects foreign libasan.so.8",
                      check_needed("x", ["libicuuc.so.74", "libicui18n.so.74",
                                         "libstdc++.so.6", "libasan.so.8"],
                                   sanitize="thread") is not None)

        # Sanitizer ELF symbol parsing, exports, and instrumentation controls.
        nm_sample = (
            "0000-Pal.cpp.o:\n"
            "\n"
            "0000000000001000 T JsCreateRuntime\n"
            "0000000000002000 T __asan_init@@ASAN_1\n"
            "                 U __asan_report_load8\n"
            "                 w __gmon_start__\n"
        )
        bad += report("parse_nm_symbol_names parses defined and U/w symbols",
                      parse_nm_symbol_names(nm_sample) == [
                          "JsCreateRuntime", "__asan_init",
                          "__asan_report_load8", "__gmon_start__",
                      ])
        bad += report("asan symbols pass address check (incl. bundled ubsan)",
                      check_sanitizer_symbols_list(
                          "ch", ["__asan_init", "__ubsan_handle_add_overflow"],
                          "address") is None)
        bad += report("unsanitized symbols fail address check",
                      check_sanitizer_symbols_list(
                          "ch", ["JsCreateRuntime", "OnChakraCoreLoadedEntry"],
                          "address") is not None)
        bad += report("ubsan-only symbols fail address check",
                      check_sanitizer_symbols_list(
                          "ch", ["__ubsan_handle_add_overflow"],
                          "address") is not None)
        bad += report("asan+tsan symbols fail address check",
                      check_sanitizer_symbols_list(
                          "ch", ["__asan_init", "__tsan_init"],
                          "address") is not None)
        bad += report("ubsan symbols pass undefined check",
                      check_sanitizer_symbols_list(
                          "ch", ["__ubsan_handle_type_mismatch_v1"],
                          "undefined") is None)
        bad += report("asan symbols fail undefined check",
                      check_sanitizer_symbols_list(
                          "ch", ["__asan_init", "__ubsan_handle_type_mismatch_v1"],
                          "undefined") is not None)
        bad += report("tsan symbols pass thread check",
                      check_sanitizer_symbols_list(
                          "ch", ["__tsan_init", "__tsan_read8"],
                          "thread") is None)
        bad += report("asan symbols fail thread check",
                      check_sanitizer_symbols_list(
                          "ch", ["__asan_init", "__tsan_init"],
                          "thread") is not None)
        bad += report("clean symbols pass check_no_sanitizer_symbols_list",
                      check_no_sanitizer_symbols_list(
                          "ch", ["JsCreateRuntime", "OnChakraCoreLoadedEntry"]) is None)
        for bad_sym in ("__asan_init", "__ubsan_handle_add_overflow",
                        "__tsan_init", "__msan_init", "__hwasan_init"):
            bad += report("non-sanitizer check rejects %s" % bad_sym,
                          check_no_sanitizer_symbols_list(
                              "ch", ["JsCreateRuntime", bad_sym]) is not None)

        want_exp = ["DllMain", "JsCreateRuntime"]
        bad += report("non-sanitizer check_export_sets rejects extra __asan_init",
                      check_export_sets("so", want_exp + ["__asan_init"], want_exp,
                                        sanitize=None) is not None)
        bad += report("asan check_export_sets accepts exact set",
                      check_export_sets("so", want_exp, want_exp,
                                        sanitize="address") is None)
        bad += report("asan check_export_sets accepts allowed sanitizer symbols",
                      check_export_sets(
                          "so", want_exp + ["__asan_init", "__sanitizer_cov_trace_pc"],
                          want_exp, sanitize="address") is None)
        bad += report("asan check_export_sets rejects missing JsCreateRuntime",
                      check_export_sets("so", ["DllMain", "__asan_init"], want_exp,
                                        sanitize="address") is not None)
        bad += report("asan check_export_sets rejects non-sanitizer extra export",
                      check_export_sets("so", want_exp + ["InternalCppLeak"],
                                        want_exp, sanitize="address") is not None)
        bad += report("asan check_export_sets rejects foreign __tsan_init",
                      check_export_sets("so", want_exp + ["__tsan_init"],
                                        want_exp, sanitize="address") is not None)
        bad += report("ubsan check_export_sets rejects __asan_init",
                      check_export_sets("so", want_exp + ["__asan_init"],
                                        want_exp, sanitize="undefined") is not None)

        with open(os.path.join(static_dir, ".profile"), "w") as f:
            f.write("sanitize-address\n")
        bad += report("sanitize-address .profile passes marker gate",
                      check_profile_marker(static_dir, "sanitize-address") is None)
        bad += report("sanitize-address .profile fails plain test gate",
                      check_profile_marker(static_dir, "test") is not None)

        # Phase-4 u8a_codec.o archive membership, zero zig_u8a_* leak, and 161 Node-API exports ref.
        if have_ar:
            p_u8a = os.path.join(tmp, "u8a_codec.o")
            with open(p_u8a, "wb") as f:
                f.write(good)
            p_ar_u8a = os.path.join(tmp, "static_with_u8a.a")
            rc_ar_u8a, _, _ = common.run_capture(["ar", "rcS", p_ar_u8a, p_member, p_u8a])
            bad += report("archive with u8a_codec.o passes member-presence gate",
                          rc_ar_u8a == 0 and check_archive_contains(p_ar_u8a) is None)
            bad += report("archive without u8a_codec.o fails member-presence gate",
                          rc_ar == 0 and check_archive_contains(p_ar) is not None)
        else:
            print("self-test archive with u8a_codec.o passes member-presence"
                  " gate  -> SKIP (no ar)")
            print("self-test archive without u8a_codec.o fails"
                  " member-presence gate  -> SKIP (no ar)")
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
        shutil.rmtree(tmp, ignore_errors=True)

    # RV-10: Per-member archive sanitizer coverage checks (nm -u per member,
    # accounting cleanly for assembly-only .S.o members and Zig u8a_codec.o).
    from unittest import mock as _mock
    res_single_missing = check_archive_members_sanitizer_symbols(
        "fake.a",
        {
            "a.cpp.o": {"__asan_init", "__asan_handle_no_return"},
            "b.cpp.o": {"malloc", "free"},
            "Thunks.S.o": set(),
            "u8a_codec.o": set(),
        },
        sanitize="address",
        member_all={"u8a_codec.o": {"zig_u8a_encode_hex", "debug.panicExtra__func_0"}},
    )
    bad += report(
        "per-member catches single unsanitized C++ member",
        res_single_missing is not None and "b.cpp.o" in res_single_missing,
    )
    res_u8a_unsanitized = check_archive_members_sanitizer_symbols(
        "fake.a",
        {
            "a.cpp.o": {"__asan_init"},
            "Thunks.S.o": set(),
            "u8a_codec.o": set(),
        },
        sanitize="address",
        member_all={"u8a_codec.o": {"zig_u8a_encode_hex"}},
    )
    bad += report(
        "per-member catches unsanitized u8a_codec.o",
        res_u8a_unsanitized is not None and "u8a_codec.o" in res_u8a_unsanitized,
    )
    res_per_member_ok = check_archive_members_sanitizer_symbols(
        "fake.a",
        {
            "a.cpp.o": {"__asan_init"},
            "b.c.o": {"__asan_init"},
            "Thunks.S.o": set(),
            "arm64_SAVE_REGISTERS.S.o": set(),
            "u8a_codec.o": {"__zig_probe_stack"},
        },
        sanitize="address",
        member_all={"u8a_codec.o": {"zig_u8a_encode_hex", "debug.panicExtra__func_0"}},
    )
    bad += report(
        "per-member allows .S.o and debug-checked u8a_codec.o",
        res_per_member_ok is None,
    )
    res_asm_foreign = check_archive_members_sanitizer_symbols(
        "fake.a",
        {
            "a.cpp.o": {"__asan_init"},
            "Thunks.S.o": {"__tsan_init"},
            "u8a_codec.o": {"__asan_init"},
        },
        sanitize="address",
        member_all={},
    )
    bad += report(
        "per-member rejects foreign sanitizer in .S.o",
        res_asm_foreign is not None and "Thunks.S.o" in res_asm_foreign,
    )
    res_non_san_leak = check_archive_members_sanitizer_symbols(
        "fake.a",
        {
            "a.cpp.o": {"malloc"},
            "u8a_codec.o": {"__asan_init"},
        },
        sanitize=None,
    )
    bad += report(
        "non-sanitizer catches single member sanitizer leak",
        res_non_san_leak is not None and "u8a_codec.o" in res_non_san_leak,
    )
    parsed_members = parse_nm_archive_members(
        "libChakraCoreStatic.a(Foo.cpp.o):\n"
        "                 U __asan_init\n"
        "Thunks.S.o:\n"
        "u8a_codec.o:\n"
        "                 U memcpy\n"
    )
    bad += report(
        "parse_nm_archive_members handles GNU/LLVM headers",
        set(parsed_members.keys()) == {"Foo.cpp.o", "Thunks.S.o", "u8a_codec.o"}
        and "__asan_init" in parsed_members["Foo.cpp.o"]
        and parsed_members["Thunks.S.o"] == set(),
    )
    with _mock.patch.object(
        common,
        "run_capture",
        return_value=(
            0,
            "Foo.cpp.o:\n                 U __asan_init\n"
            "Bar.cpp.o:\n                 U free\n"
            "Thunks.S.o:\n",
            "",
        ),
    ):
        err_mock = check_archive_sanitizer_symbols("nonexistent.a", sanitize="address")
        bad += report(
            "check_archive_sanitizer_symbols rejects Bar.cpp.o via mocked nm -u",
            err_mock is not None and "Bar.cpp.o" in err_mock,
        )

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
    base_profile = opts["base_profile"]
    sanitize = opts["sanitize"]
    marker_profile = opts["marker_profile"]
    icu_major = opts["icu_major"]
    jit = opts["jit"]
    is_static = (base_profile == "debug-static")
    is_release = (base_profile == "release-shared")

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
                                              require_icu=(exe == "bin/ch"),
                                              sanitize=sanitize)))
                failures.append((exe + "-self-contained",
                                 check_no_chakracore_needed(exe, exe_needed)))
            except (OSError, ValueError) as e:
                failures.append((exe + "-needed", str(e)))
    else:
        failures.append(("soname+needed",
                         check_dynamic_so(os.path.join(prefix, SOLIBS[1]),
                                          icu_major=icu_major,
                                          sanitize=sanitize)))
        try:
            _, ch_needed = readelf_dynamic(os.path.join(prefix, "bin/ch"))
            print("check_metadata_linux: dynamic bin/ch -> needed=%r"
                  % (ch_needed,))
            failures.append(("ch-needed",
                             check_needed("bin/ch", ch_needed,
                                          icu_major=icu_major,
                                          sanitize=sanitize)))
        except (OSError, ValueError) as e:
            failures.append(("ch-needed", str(e)))
        if sanitize is not None:
            try:
                _, gs_needed = readelf_dynamic(os.path.join(prefix, "bin/GCStress"))
                print("check_metadata_linux: dynamic bin/GCStress -> needed=%r"
                      % (gs_needed,))
                failures.append(("GCStress-needed",
                                 check_needed("bin/GCStress", gs_needed,
                                              icu_major=icu_major,
                                              require_icu=False,
                                              sanitize=sanitize)))
                failures.append(("GCStress-self-contained",
                                 check_no_chakracore_needed("bin/GCStress", gs_needed)))
            except (OSError, ValueError) as e:
                failures.append(("GCStress-needed", str(e)))

    # 3. Exports, leak checks & sanitizer ELF symbols.
    failures.append(("nodeapi-exports-ref", check_nodeapi_exports_ref()))
    if is_static:
        static_archive = os.path.join(prefix, "lib/libChakraCoreStatic.a")
        failures.append(("static-archive-exports",
                         check_static_archive_exports(static_archive)))
        failures.append(("static-archive-sanitizer-symbols",
                         check_archive_sanitizer_symbols(
                             static_archive, sanitize=sanitize)))
    else:
        failures.append(("so-exports", check_exports(
            os.path.join(prefix, SOLIBS[1]), EXPORTS_SO,
            omit=(RELEASE_SHARED_ABSENT_EXPORTS if is_release else ()),
            sanitize=sanitize)))
        failures.append(("ch-exports", check_exports_subset(
            os.path.join(prefix, "bin/ch"), EXPORTS_CH)))
        failures.append(("so-no-zig-u8a-leak",
                         check_no_leaked_symbol_prefixes(
                             os.path.join(prefix, SOLIBS[1]))))
        failures.append(("so-sanitizer-symbols",
                         check_elf_sanitizer_symbols(
                             os.path.join(prefix, SOLIBS[1]),
                             sanitize=sanitize)))
    for exe in EXECUTABLES:
        exe_path = os.path.join(prefix, exe)
        failures.append((exe + "-no-zig-u8a-leak",
                         check_no_leaked_symbol_prefixes(exe_path)))
        failures.append((exe + "-sanitizer-symbols",
                         check_elf_sanitizer_symbols(
                             exe_path, sanitize=sanitize)))
    nodeapi_so = os.path.join(prefix, "lib/libChakraNodeApi.so")
    if os.path.lexists(nodeapi_so):
        failures.append(("nodeapi-so-elf", check_elf(nodeapi_so, ET_DYN)))
        failures.append(("nodeapi-so-soname+needed",
                         check_dynamic_so(nodeapi_so,
                                          icu_major=icu_major,
                                          sanitize=sanitize,
                                          want_soname=WANT_NODEAPI_SONAME,
                                          require_icu=False)))
        failures.append(("nodeapi-so-exports",
                         check_exports_subset(nodeapi_so, NODEAPI_EXPORTS)))
        failures.append(("nodeapi-so-no-zig-u8a-leak",
                         check_no_leaked_symbol_prefixes(nodeapi_so)))
        failures.append(("nodeapi-so-sanitizer-symbols",
                         check_elf_sanitizer_symbols(
                             nodeapi_so, sanitize=sanitize)))

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
    for rel, want in expected_archive_members(base_profile, jit=jit).items():
        archive_path = os.path.join(prefix, rel)
        failures.append((rel, check_archive(archive_path, want)))
        if rel == "lib/libChakraCoreStatic.a":
            failures.append((rel + "-u8a-codec",
                             check_archive_contains(archive_path)))

    # 6. Profile marker.
    failures.append((".profile", check_profile_marker(prefix, marker_profile)))

    bad = [(name, err) for name, err in failures if err is not None]
    if bad:
        print("check_metadata_linux: FAIL:", file=sys.stderr)
        for name, err in bad:
            print("check_metadata_linux: FAIL: [%s] %s" % (name, err),
                  file=sys.stderr)
        return 1
    if is_static:
        print("check_metadata_linux: PASS: profile=%s sanitize=%s icu=%s jit=%s static ELF + "
              "NEEDED + archive exports + configs + headers + archives + marker match"
              % (marker_profile, sanitize or "none", icu_major, jit))
    else:
        print("check_metadata_linux: PASS: profile=%s sanitize=%s icu=%s jit=%s ELF + SONAME/NEEDED + "
              "exports (%d/%d) + configs + headers + archives + marker all match"
              % (marker_profile, sanitize or "none", icu_major, jit,
                 len(read_export_ref(EXPORTS_SO)) -
                 (len(RELEASE_SHARED_ABSENT_EXPORTS) if is_release else 0),
                 len(read_export_ref(EXPORTS_CH))))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
