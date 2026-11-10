#!/usr/bin/env python3
#-------------------------------------------------------------------------------------------------------
# Copyright (c) ChakraCore Project Contributors. All rights reserved.
# Licensed under the MIT license. See LICENSE.txt file in the project root for full license information.
#-------------------------------------------------------------------------------------------------------
"""B4.3 check gate (5): Linux artifact metadata assertions.

Sub-checks against the install prefix (zig-out):

  1. ELF identity of ch, GCStress and libChakraCore.so (pure-python
     ELF header parse): 64-bit LSB x86-64; ch and GCStress are DYN
     (PIE — ch by explicit -fPIE/-pie, GCStress by explicit -pie
     spelling Ubuntu clang's link-PIE default, which zig lacks);
     the .so is DYN.
  2. Dynamic section (`readelf -d`): SONAME == libChakraCore.so on
     the .so; NEEDED carries the ICU pair (libicuuc.so.74,
     libicui18n.so.74) and NO C++ runtime (libc++ links statically;
     the reference .so has no libstdc++ NEEDED either, via
     -static-libstdc++ — same property, different runtime).
  3. Version-script exports (`nm -D --defined-only`): the EXACT
     committed set for the .so (tools/build/linux_exports_so.txt:
     237 symbols incl. DllMain + Js* at B4 record time — export
     membership is source- and script-determined, so any delta is
     a real finding). Provenance honesty: the B4 record does NOT
     document whether linux_exports_*.txt were captured from the
     legacy reference artifacts or from the Zig-built artifacts,
     so today this gate proves stability against the committed
     lists, not parity with the legacy reference. Before any
     parity claim, the lists must be regenerated from the legacy
     artifacts on the first native host and diffed against the
     committed files (the third field of every 3-field
     `nm -D --defined-only` line regardless of its type letter
     — exactly what nm_defined below collects — sorted):

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
     but LLD has no equivalent spelling — --export-dynamic exports
     everything (1248 here), without it nothing is dynamic (not even
     the required symbol), and both selective flags
     (--export-dynamic-symbol, --dynamic-list) are unsupported
     linker args (all probed B4.3). The gate asserts the reference
     set is covered; the surplus count is recorded, never gated
     (gating it would couple the gate to compiler internals).
  4. Configs: pal/src/config.h present with Linux markers (the
     online gate already verified values at configure time);
     lib/wabt/config.h sha256 == the reference hash (byte-exact).
  5. The 4 staged public headers present, nonempty and byte-identical
     to their lib/ sources; bin/ and lib/ .so copies identical;
     archives present with legacy-exact member counts
     (static 669 after the two C2-S13 TUs, Pal 90, Codex 1,
     PlatformAgnostic 11; `ar t`).
  6. The `.profile` marker reads `test`.

Every sub-check fails loudly with diagnostics on mismatch.

Usage: check_metadata_linux.py <prefix> [--self-test]
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

WANT_SONAME = "libChakraCore.so"
WANT_ICU_NEEDED = ("libicuuc.so.74", "libicui18n.so.74")
# No C++ runtime may appear in NEEDED (static linkage both sides).
FORBID_NEEDED_SUBSTRS = ("libstdc++", "libc++")
WANT_WABT_SHA256 = "4d365a89ec93f99a52137a5db99446514892cf928b2ff4a824ca997a271749d9"
WANT_MEMBERS = {
    # 667 at the B4.3 reference graph; +2 for the C2-S13 WeakRef/
    # FinalizationRegistry TUs added on the branch after that reference
    # (tables_linux.zig now compiles 684 TUs), asserted by this gate.
    "lib/libChakraCoreStatic.a": 669,
    "lib/libChakra.Pal.Singular.a": 90,
    "lib/libChakra.Common.Codex.Singular.a": 1,
    "lib/libChakra.Runtime.PlatformAgnostic.Singular.a": 11,
}
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


def check_dynamic_so(path):
    try:
        soname, needed = readelf_dynamic(path)
    except (OSError, ValueError) as e:
        return str(e)
    print("check_metadata_linux: dynamic %s -> soname=%r needed=%r"
          % (path, soname, needed))
    if soname != WANT_SONAME:
        return "%s: want SONAME %r, have %r" % (path, WANT_SONAME, soname)
    return check_needed(path, needed)


def check_needed(path, needed):
    for lib in WANT_ICU_NEEDED:
        if lib not in needed:
            return ("%s: NEEDED lacks %s (have %r)"
                    % (path, lib, needed))
    for lib in needed:
        for bad in FORBID_NEEDED_SUBSTRS:
            if bad in lib:
                return ("%s: NEEDED carries C++ runtime %s (want static)"
                        % (path, lib))
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
    """Sorted nonblank symbol names from a committed export list."""
    with open(ref_file) as f:
        return sorted(line.strip() for line in f if line.strip())


def check_exports(path, ref_file):
    try:
        want = read_export_ref(ref_file)
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
    if len(args) != 1:
        print("usage: check_metadata_linux.py <prefix>", file=sys.stderr)
        return 2
    prefix = args[0]
    failures = []
    # 1. ELF identity.
    for rel in EXECUTABLES:
        failures.append((rel, check_elf(os.path.join(prefix, rel), ET_DYN)))
    for rel in SOLIBS:
        failures.append((rel, check_elf(os.path.join(prefix, rel), ET_DYN)))
    # 2. Dynamic section.
    failures.append(("soname+needed",
                     check_dynamic_so(os.path.join(prefix, SOLIBS[1]))))
    try:
        _, ch_needed = readelf_dynamic(os.path.join(prefix, "bin/ch"))
        print("check_metadata_linux: dynamic bin/ch -> needed=%r"
              % (ch_needed,))
        failures.append(("ch-needed", check_needed("bin/ch", ch_needed)))
    except (OSError, ValueError) as e:
        failures.append(("ch-needed", str(e)))
    # 3. Exports.
    failures.append(("so-exports", check_exports(
        os.path.join(prefix, SOLIBS[1]), EXPORTS_SO)))
    failures.append(("ch-exports", check_exports_subset(
        os.path.join(prefix, "bin/ch"), EXPORTS_CH)))
    # 4. Configs.
    failures.append(("pal-config", check_file_bytes(
        os.path.join(prefix, "pal/src/config.h"), markers=PAL_MARKERS)))
    failures.append(("wabt-config", check_file_bytes(
        os.path.join(prefix, "lib/wabt/config.h"),
        want_sha=WANT_WABT_SHA256)))
    # 5. Headers, .so copies, archives.
    for staged, src in HEADERS:
        failures.append((staged, check_header(staged, src, prefix)))
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
    for rel, want in WANT_MEMBERS.items():
        failures.append((rel, check_archive(os.path.join(prefix, rel), want)))
    # 6. Profile marker.
    try:
        with open(os.path.join(prefix, ".profile")) as f:
            marker = f.read()
        failures.append((".profile", None if marker == "test\n"
                         else ".profile reads %r, want 'test\\n'" % marker))
    except OSError as e:
        failures.append((".profile", str(e)))

    bad = [(name, err) for name, err in failures if err is not None]
    if bad:
        print("check_metadata_linux: FAIL:", file=sys.stderr)
        for name, err in bad:
            print("check_metadata_linux: FAIL: [%s] %s" % (name, err),
                  file=sys.stderr)
        return 1
    print("check_metadata_linux: PASS: ELF + SONAME/NEEDED + exports "
          "(%d/%d) + configs + headers + archives + marker all match"
          % (len(read_export_ref(EXPORTS_SO)),
             len(read_export_ref(EXPORTS_CH))))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
