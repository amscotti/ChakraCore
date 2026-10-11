#!/usr/bin/env python3
"""B1 step 4 — repeatable B1 verification for the Mac reference profile.

Checks (doc/modernization-plan.md section B1 "Done when",
doc/build-contract-mac.md, doc/pal-config-contract-mac.md section 5):

  1. Clean slice build from source plus declared dependencies exits 0.
  2. Generated zig-out/pal/src/config.h matches the MD5 pinned in both
     contracts; byte-identical to the build-zigpilot/pal/src/config.h
     reference (cmp) when that git-ignored ref exists.
  3. B2-aware archive check: EITHER zig-out/lib/libChakra.Pal.a (B1
     artifact, stem.o members) OR
     zig-out/lib/libChakra.Pal.Singular.a (B2 artifact, NNNN-basename.o
     members, basename keeping its source extension) holds exactly the
     91 contract objects (names vs doc/build-contract-mac.md section
     4.1 PAL group: stem.o for B1, NNNN- stripped then basename.o for
     B2) and no non-object member besides __.SYMDEF.
  4. A fast fail-clearly subset exits nonzero with a loud message:
     missing ICU header, wrong-arch target, wrong-minimum target,
     -Dcpu override, -Doptimize=Debug, wrong-OS target, missing SDK
     dir, wrong SDK version, missing ICU libraries.

Uses ONLY mise zig 0.17.0 (PATH zig is ignored: 0.16.0 cannot even
compile build.zig). The garbage-target case is deliberately NOT run:
an unparseable -Dtarget still runs all 95 probes before Zig's deferred
option validation fails it (~30s of wasted probing, exit code stays
correct; see build.zig gateTarget comment). Prints a PASS/FAIL summary
and writes an evidence log (default tools/build-contract/verify_b1.log).

Usage: python3 tools/build-contract/verify_b1.py [--log PATH]
Exit code: 0 iff every check passes.
"""
import hashlib
import os
import re
import shutil
import subprocess
import sys
import tempfile
import time
from datetime import datetime, timezone

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
ZIG = ["mise", "exec", "--", "zig"]
EXPECTED_ZIG = "0.17.0"
EXPECTED_OBJECTS = 91
REFERENCE_CONFIG = "build-zigpilot/pal/src/config.h"
GENERATED_CONFIG = "zig-out/pal/src/config.h"
EXPECTED_CONFIG_MD5 = "855152b0a1b6da1d8abfef52e1f45f4e"
ARCHIVE_B1 = "zig-out/lib/libChakra.Pal.a"
ARCHIVE_B2 = "zig-out/lib/libChakra.Pal.Singular.a"
BUILD_TIMEOUT = 1200
FAIL_TIMEOUT = 180

FAIL_CASES = [
    {
        "name": "missing-ICU-header",
        "args": None,  # filled in: -Dicu-include=<empty fake tree>/include
        "want": ["Cannot find ICU", "unicode/uchar.h"],
    },
    {
        "name": "wrong-arch-target",
        "args": ["-Dtarget=x86_64-macos.26.0"],
        "want": ["unsupported arch x86_64"],
    },
    {
        "name": "wrong-minimum-target",
        "args": ["-Dtarget=aarch64-macos.25.0"],
        "want": ["unsupported macOS minimum 25.0.0"],
    },
    {
        "name": "cpu-override",
        "args": ["-Dcpu=native"],
        "want": ["unsupported -Dcpu=native"],
    },
    {
        "name": "optimize-override",
        "args": ["-Doptimize=Debug"],
        "want": ["unsupported -Doptimize=debug"],
    },
    {
        "name": "wrong-OS-target",
        "args": ["-Dtarget=aarch64-linux-gnu"],
        "want": ["unsupported OS", "linux"],
    },
    {
        "name": "missing-sdk-dir",
        "args": ["-Dsdk-path=/nonexistent/MacOSX.sdk"],
        "want": ["SDK not found"],
    },
    {
        "name": "wrong-sdk-version",
        "args": None,  # filled in: -Dsdk-path=<fake dir with SDKSettings.json Version 15.0>
        "want": ["unsupported SDK", "have 15.0"],
    },
    {
        "name": "missing-icu-libs",
        "args": None,  # filled in: -Dicu-include=<fake tree>/include (header present, no lib/)
        "want": ["Cannot find ICU libraries", "libicuuc.dylib"],
    },
]

log_lines = []


def log(text=""):
    print(text, flush=True)
    log_lines.append(text)


def run(cmd, timeout, cwd=ROOT):
    t0 = time.time()
    try:
        p = subprocess.run(cmd, cwd=cwd, stdout=subprocess.PIPE,
                           stderr=subprocess.STDOUT, text=True, timeout=timeout)
        return p.returncode, p.stdout, time.time() - t0
    except subprocess.TimeoutExpired as e:
        out = (e.stdout or "") if isinstance(e.stdout, str) else ""
        return None, out + f"\n[TIMEOUT after {timeout}s]", time.time() - t0


def md5_of(path):
    h = hashlib.md5()
    with open(path, "rb") as f:
        for chunk in iter(lambda: f.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest()


def expected_objects_from_doc():
    """Basename-derived .o names for the section 4.1 PAL group file list."""
    path = os.path.join(ROOT, "doc/build-contract-mac.md")
    with open(path) as f:
        text = f.read()
    m = re.search(r"### 4\.1 .*?\n(.*?)\n### 4\.2 ", text, re.S)
    if not m:
        raise SystemExit("cannot locate section 4.1 PAL group in doc/build-contract-mac.md")
    files = re.findall(r"^- `([^`]+)`$", m.group(1), re.M)
    objs = set()
    for f in files:
        base = os.path.basename(f)
        stem = base.rsplit(".", 1)[0]
        objs.add(stem + ".o")
    return files, objs


results = []


def record(name, expected, observed, ok, detail=""):
    results.append((name, expected, observed, ok))
    log(f"[{'PASS' if ok else 'FAIL'}] {name}: expected {expected}; observed {observed}")
    if detail:
        log(f"       {detail}")


def main():
    log_path = os.path.join(ROOT, "tools/build-contract/verify_b1.log")
    if len(sys.argv) == 3 and sys.argv[1] == "--log":
        log_path = sys.argv[2]
    elif len(sys.argv) != 1:
        sys.exit("usage: verify_b1.py [--log PATH]")

    if shutil.which("mise") is None:
        sys.exit("mise not found on PATH; this script uses ONLY mise zig 0.17.0")

    now = datetime.now(timezone.utc).astimezone()
    log(f"B1 verification — {now.strftime('%Y-%m-%d %H:%M:%S %Z')} — root {ROOT}")

    # 0. Toolchain pin.
    rc, out, _ = run(ZIG + ["version"], FAIL_TIMEOUT)
    zig_version = out.strip()
    log(f"$ {' '.join(ZIG + ['version'])} -> exit {rc}: {zig_version}")
    if rc != 0 or zig_version != EXPECTED_ZIG:
        record("toolchain", f"mise zig {EXPECTED_ZIG}",
               f"exit {rc} version {zig_version!r}", False)
    else:
        record("toolchain", f"mise zig {EXPECTED_ZIG}", zig_version, True)
    rc, env_out, _ = run(ZIG + ["env"], FAIL_TIMEOUT)
    log("--- zig env ---")
    log(env_out.strip())
    log("--- end zig env ---")

    # 1. Wipe caches and run the clean build.
    for d in (".zig-cache", "zig-out"):
        shutil.rmtree(os.path.join(ROOT, d), ignore_errors=True)
    log("wiped .zig-cache/zig-out")
    rc, build_out, dt = run(ZIG + ["build"], BUILD_TIMEOUT)
    log(f"$ {' '.join(ZIG + ['build'])} -> exit {rc} in {dt:.1f}s")
    log("--- zig build output (tail 15 lines) ---")
    log("\n".join(build_out.splitlines()[-15:]))
    log("--- end zig build output ---")
    if rc is None:
        record("clean-build", "exit 0", "TIMEOUT", False)
        build_ok = False
    else:
        build_ok = rc == 0
        record("clean-build", "exit 0", f"exit {rc} in {dt:.1f}s", build_ok)
    probes_ok = "95/95 checks match" in build_out
    record("pal-config-checks", "95/95 checks match the contract",
           "found" if probes_ok else "marker missing", probes_ok and build_ok)

    # 2. Generated config.h matches the MD5 pinned in both contracts;
    # byte-compare against the git-ignored ref when it exists.
    gen = os.path.join(ROOT, GENERATED_CONFIG)
    ref = os.path.join(ROOT, REFERENCE_CONFIG)
    if not os.path.exists(gen):
        record("config.h-md5", f"MD5 {EXPECTED_CONFIG_MD5}",
               "missing generated config.h", False)
    else:
        gen_md5 = md5_of(gen)
        log(f"md5 gen={gen_md5}")
        record("config.h-md5", f"MD5 {EXPECTED_CONFIG_MD5}",
               f"MD5 {gen_md5}", gen_md5 == EXPECTED_CONFIG_MD5)
    if os.path.exists(gen) and os.path.exists(ref):
        rc, cmp_out, _ = run(["cmp", gen, ref], FAIL_TIMEOUT)
        detail = f"md5 ref={md5_of(ref)}"
        log(detail)
        record("config.h-cmp", "cmp clean",
               "identical" if rc == 0 else f"cmp exit {rc}: {cmp_out.strip()}", rc == 0, detail)
    elif os.path.exists(gen):
        log("config.h-cmp: ref absent — MD5-only")

    # 3. Archive holds exactly the 91 contract objects (B2-aware: accept
    # EITHER the B1 or the B2 artifact; both require 91 + only __.SYMDEF).
    try:
        doc_files, expected = expected_objects_from_doc()
        log(f"doc section 4.1 PAL group: {len(doc_files)} files -> {len(expected)} unique .o names")
        doc_ok = len(doc_files) == EXPECTED_OBJECTS and len(expected) == EXPECTED_OBJECTS
        if not doc_ok:
            record("archive-objects", f"exactly {EXPECTED_OBJECTS} contract objects",
                   f"doc parse gave {len(doc_files)} files/{len(expected)} names", False)
        else:
            path_b1 = os.path.join(ROOT, ARCHIVE_B1)
            path_b2 = os.path.join(ROOT, ARCHIVE_B2)
            if os.path.exists(path_b1):
                archive, is_b2 = path_b1, False
            elif os.path.exists(path_b2):
                archive, is_b2 = path_b2, True
            else:
                archive, is_b2 = None, False
            if archive is None:
                record("archive-objects", f"exactly {EXPECTED_OBJECTS} contract objects + only __.SYMDEF",
                       f"neither {ARCHIVE_B1} nor {ARCHIVE_B2} exists", False)
            else:
                log(f"archive under test: {archive} ({'B2 NNNN- members' if is_b2 else 'B1 basename members'})")
                rc, ar_out, _ = run(["ar", "t", archive], FAIL_TIMEOUT)
                members = [l for l in ar_out.splitlines() if l]
                objects = sorted(m for m in members if m.endswith(".o"))
                non_obj = sorted(m for m in members if not m.endswith(".o"))
                log(f"ar t: {len(members)} members ({len(objects)} .o, non-.o: {non_obj})")
                names = set(objects)
                want = expected
                bad_prefix = []
                if is_b2:
                    bad_prefix = sorted(m for m in objects if not re.match(r"^\d{4}-", m))
                    names = set(re.sub(r"^\d{4}-", "", m) for m in objects)
                    # B2 members keep the source extension (NNNN-file.cpp.o),
                    # so compare against basename.o, not stem.o.
                    want = set(os.path.basename(f) + ".o" for f in doc_files)
                missing = sorted(want - names)
                extra = sorted(names - want)
                ok = (rc == 0 and not missing and not extra and not bad_prefix
                      and len(objects) == EXPECTED_OBJECTS and non_obj == ["__.SYMDEF"])
                detail = ""
                if missing:
                    detail += f" missing={missing}"
                if extra:
                    detail += f" extra={extra}"
                if bad_prefix:
                    detail += f" members without NNNN- prefix={bad_prefix}"
                if non_obj != ["__.SYMDEF"]:
                    detail += f" non-object members={non_obj}"
                record("archive-objects", f"exactly {EXPECTED_OBJECTS} contract objects + only __.SYMDEF",
                       f"{len(objects)} .o match" if ok else
                       f"{len(objects)} .o (ar exit {rc})", ok, detail.strip())
    except SystemExit as e:
        record("archive-objects", f"exactly {EXPECTED_OBJECTS} contract objects", str(e), False)

    # 4. Fast fail-clearly subset (garbage-target case deliberately skipped).
    log("fail-clearly subset (each must exit nonzero with a loud message):")
    with tempfile.TemporaryDirectory(prefix="b1-fake-") as fake:
        by_name = {c["name"]: c for c in FAIL_CASES}
        fake_include = os.path.join(fake, "icu-empty", "include")
        os.makedirs(fake_include)  # exists but holds no unicode/uchar.h
        by_name["missing-ICU-header"]["args"] = [f"-Dicu-include={fake_include}"]
        fake_sdk = os.path.join(fake, "sdk")
        os.makedirs(fake_sdk)
        with open(os.path.join(fake_sdk, "SDKSettings.json"), "w") as f:
            f.write('{"Version": "15.0"}')
        by_name["wrong-sdk-version"]["args"] = [f"-Dsdk-path={fake_sdk}"]
        fake_icu = os.path.join(fake, "icu-nolib", "include")
        os.makedirs(os.path.join(fake_icu, "unicode"))
        with open(os.path.join(fake_icu, "unicode", "uchar.h"), "w") as f:
            f.write("/* fake header: libs intentionally absent */\n")
        by_name["missing-icu-libs"]["args"] = [f"-Dicu-include={fake_icu}"]
        for case in FAIL_CASES:
            cmd = ZIG + ["build"] + case["args"]
            rc, out, dt = run(cmd, FAIL_TIMEOUT)
            log(f"$ {' '.join(cmd)} -> exit {rc} in {dt:.1f}s")
            for line in out.splitlines():
                if "build error" in line:
                    log(f"    {line[:300]}")
            if rc is None:
                record(f"fail:{case['name']}", "nonzero exit + loud message",
                       "TIMEOUT", False)
                continue
            missing_msg = [w for w in case["want"] if w not in out]
            ok = rc != 0 and not missing_msg
            observed = f"exit {rc} in {dt:.1f}s"
            if rc == 0:
                observed += " (UNEXPECTED SUCCESS)"
            if missing_msg:
                observed += f" (message missing: {missing_msg})"
            record(f"fail:{case['name']}", "nonzero exit + " + " & ".join(case["want"]),
                   observed, ok)
    log("skipped (documented-known): unparseable -Dtarget runs all 95 probes "
        "before Zig's deferred option validation fails it; see build.zig gateTarget.")

    # Summary.
    log("")
    log("check | expected | observed | result")
    for name, expected, observed, ok in results:
        log(f"{name} | {expected} | {observed} | {'PASS' if ok else 'FAIL'}")
    failed = sum(1 for r in results if not r[3])
    log(f"SUMMARY: {len(results) - failed}/{len(results)} PASS"
        + (f", {failed} FAIL" if failed else ""))
    try:
        with open(log_path, "w") as f:
            f.write("\n".join(log_lines) + "\n")
        print(f"evidence log: {log_path}")
    except OSError as e:
        print(f"WARNING: cannot write evidence log {log_path}: {e}")
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())