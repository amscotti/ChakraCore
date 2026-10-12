#!/usr/bin/env python3
#-------------------------------------------------------------------------------------------------------
# Copyright (c) ChakraCore Project Contributors. All rights reserved.
# Licensed under the MIT license. See LICENSE.txt file in the project root for full license information.
#-------------------------------------------------------------------------------------------------------
"""B3.5 CI input/output recorder (doc/b3-profiles.md section 11).

One committed recorder used BOTH by .github/workflows/zig-macos.yml
and by the local B3.5 simulation, so CI runs and the local proof
record identical fields by construction (no shell drift between the
workflow and the documented recipe).

Subcommands:

  inputs --profile <test|debug-static|release-shared> --out DIR
      Record the plan section 2.1 input set: zig version + full
      `zig env`, target, build flavor, feature defines (parsed from
      the same tools/build/tables.zig arrays build.zig consumes),
      dependency versions (Xcode/SDK, ICU, python, cmake, ninja,
      clang), host facts and the git revision. Writes inputs.json,
      inputs.md and zig-env.txt into DIR.

  manifest --prefix PATH --out DIR [--label L]
      Record artifact metadata for one install prefix (or legacy
      build dir): per-file sizes + sha256, symlink targets.
      Writes manifest-<label>.json + manifest-<label>.md into DIR.

  --self-test
      Hermetic checks: manifest of a synthetic tree, define-table
      parsing of the live tools/build/tables.zig.

Exit code: 0 iff the record was written. Every probe failure is
loud (missing tool versions record as "MISSING: <tool>" and fail
the inputs command, so a silently degraded record is impossible).
"""

import hashlib
import json
import os
import re
import shutil
import subprocess
import sys
import tempfile
from datetime import datetime, timezone

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import common

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
ZIG = ["mise", "exec", "--", "zig"]
TABLES = os.path.join(ROOT, "tools", "build", "tables.zig")

PROFILES = {
    "test": {"cmake_build_type": "RelWithDebInfo", "library": "shared",
             "optimize": "fast", "arrays": ("engine_defines", "pal_defines")},
    "debug-static": {"cmake_build_type": "Debug", "library": "static",
                     "optimize": "debug",
                     "arrays": ("engine_defines_debug_static",
                                "pal_defines_debug_static")},
    "release-shared": {"cmake_build_type": "Release", "library": "shared",
                       "optimize": "fast",
                       "arrays": ("engine_defines_release",
                                  "pal_defines_release")},
}
TARGET = "aarch64-macos.26.0"
MINOS = "26.0"


def run(cmd, timeout=120, cwd=ROOT):
    try:
        rc, out, _ = common.run_capture(cmd, timeout=timeout, cwd=cwd,
                                        merge_stderr=True)
        return rc, out
    except (OSError, subprocess.TimeoutExpired) as e:
        return None, "PROBE-FAILED: %r" % (e,)


def first_line(text):
    for line in text.splitlines():
        line = line.strip()
        if line:
            return line
    return ""


def zig_probe(args):
    if shutil.which("mise") is None:
        return None, "PROBE-FAILED: mise not found on PATH"
    return run(ZIG + args)


def parse_define_array(text, name):
    """Extract the -D strings from one `pub const <name>: []const []const u8` array.

    Fails loudly (ValueError) when the array or its entries cannot be
    found, so a tables.zig reformat breaks the recorder instead of
    silently recording an empty define set.
    """
    m = re.search(r"pub const %s: \[\]const \[\]const u8 = &\.\{(.*?)\};" % re.escape(name),
                  text, re.S)
    if not m:
        raise ValueError("define array %r not found in tools/build/tables.zig" % name)
    entries = re.findall(r'"(-D[^"]+)"', m.group(1))
    if not entries:
        raise ValueError("define array %r parsed to zero entries" % name)
    return entries


def cmd_inputs(args):
    profile = None
    outdir = None
    i = 0
    while i < len(args):
        if args[i] == "--profile" and i + 1 < len(args):
            profile = args[i + 1]
            i += 2
        elif args[i] == "--out" and i + 1 < len(args):
            outdir = args[i + 1]
            i += 2
        else:
            sys.exit("usage: ci_record.py inputs --profile P --out DIR")
    if profile not in PROFILES:
        sys.exit("unsupported --profile=%r: expected one of: %s"
                 % (profile, ", ".join(sorted(PROFILES))))
    if not outdir:
        sys.exit("usage: ci_record.py inputs --profile P --out DIR")
    os.makedirs(outdir, exist_ok=True)

    missing = []

    def need(label, rc, text):
        if rc != 0:
            missing.append(label)
            return "MISSING: %s (exit %s)" % (label, rc)
        line = first_line(text)
        if not line:
            missing.append(label)
            return "MISSING: %s (empty output)" % label
        return line

    rc, zig_version = zig_probe(["version"])
    zig_version = need("zig version", rc, zig_version)
    rc, zig_env = zig_probe(["env"])
    if rc != 0:
        missing.append("zig env")
        zig_env = "MISSING: zig env (exit %s)" % rc
    with open(os.path.join(outdir, "zig-env.txt"), "w") as f:
        f.write(zig_env if zig_env.endswith("\n") else zig_env + "\n")

    with open(TABLES) as f:
        tables_text = f.read()
    try:
        engine_defs, pal_defs = (parse_define_array(tables_text, n)
                                 for n in PROFILES[profile]["arrays"])
    except ValueError as e:
        sys.exit("build error: %s" % e)

    rc, out = run(["xcodebuild", "-version"])
    xcode = need("xcodebuild", rc, out)
    rc, out = run(["xcrun", "--show-sdk-path"])
    sdk_path = need("xcrun sdk path", rc, out)
    sdk_version = "MISSING: SDKSettings.json unreadable"
    if rc == 0:
        try:
            with open(os.path.join(first_line(out), "SDKSettings.json")) as f:
                sdk_version = json.load(f)["Version"]
        except (OSError, ValueError, KeyError) as e:
            missing.append("SDK version")
            sdk_version = "MISSING: %r" % (e,)
    rc, out = run(["brew", "list", "--versions", "icu4c"])
    icu = need("icu4c", rc, out)
    icu_header = "/opt/homebrew/opt/icu4c/include/unicode/uchar.h"
    icu_lib = "/opt/homebrew/opt/icu4c/lib/libicuuc.dylib"
    for label, path in (("ICU header", icu_header), ("ICU lib", icu_lib)):
        if not os.path.exists(path):
            missing.append(label)
    rc, out = run(["python3", "--version"])
    python = need("python3", rc, out)
    rc, out = run(["cmake", "--version"])
    cmake = need("cmake", rc, out)
    rc, out = run(["ninja", "--version"])
    ninja = need("ninja", rc, out)
    rc, out = run(["clang", "--version"])
    clang = need("clang", rc, out)
    rc, out = run(["sw_vers", "-productVersion"])
    macos = need("sw_vers", rc, out)
    rc, out = run(["sysctl", "-n", "hw.ncpu"])
    ncpu = need("ncpu", rc, out)
    rc, out = run(["git", "rev-parse", "--short", "HEAD"])
    git_rev = first_line(out) if rc == 0 else "unknown"
    rc, out = run(["git", "branch", "--show-current"])
    git_branch = first_line(out) if rc == 0 else "unknown"

    now = datetime.now(timezone.utc).astimezone()
    rec = {
        "recorded_at": now.strftime("%Y-%m-%d %H:%M:%S %Z"),
        "git": {"rev": git_rev, "branch": git_branch},
        "zig": {"version": zig_version, "env_file": "zig-env.txt"},
        "target": TARGET,
        "macos_minimum": MINOS,
        "profile": {"name": profile,
                    **{k: v for k, v in PROFILES[profile].items()
                         if k != "arrays"}},
        "defines": {"engine": engine_defs, "pal": pal_defs},
        "dependencies": {
            "xcode": xcode, "sdk_path": sdk_path,
            "sdk_version": sdk_version, "icu4c": icu,
            "icu_header": icu_header, "icu_lib": icu_lib,
            "python": python, "cmake": cmake, "ninja": ninja,
            "clang": clang},
        "host": {"macos": macos, "ncpu": ncpu},
    }
    with open(os.path.join(outdir, "inputs.json"), "w") as f:
        json.dump(rec, f, indent=2)
        f.write("\n")
    with open(os.path.join(outdir, "inputs.md"), "w") as f:
        f.write("# Build inputs (%s, %s)\n\n" % (profile, rec["recorded_at"]))
        f.write("| Field | Value |\n|---|---|\n")
        for key in ("git", "zig", "target", "macos_minimum", "profile",
                    "dependencies", "host"):
            val = rec[key]
            if isinstance(val, dict):
                for k2, v2 in val.items():
                    f.write("| %s.%s | %s |\n" % (key, k2, v2))
            else:
                f.write("| %s | %s |\n" % (key, val))
        f.write("\nEngine defines (%d): %s\n" % (len(engine_defs),
                                                 " ".join(engine_defs)))
        f.write("\nPAL defines (%d): %s\n" % (len(pal_defs),
                                              " ".join(pal_defs)))
        f.write("\nFull `zig env`: zig-env.txt\n")
    print("inputs: profile %s, zig %s, sdk %s, icu %s (%s)"
          % (profile, zig_version, sdk_version, icu, outdir))
    if missing:
        sys.exit("build error: %d input probe(s) missing: %s"
                 % (len(missing), ", ".join(missing)))
    return 0


def walk_prefix(root):
    """Map relpath -> ("f", size, sha256) / ("l", target) / ("d", None)."""
    out = {}
    for dirpath, dirnames, filenames in os.walk(root, followlinks=False):
        dirnames.sort()
        for name in sorted(filenames):
            full = os.path.join(dirpath, name)
            rel = os.path.relpath(full, root)
            if os.path.islink(full):
                out[rel] = ("l", os.readlink(full))
            elif os.path.isfile(full):
                h = hashlib.sha256()
                with open(full, "rb") as f:
                    for chunk in iter(lambda: f.read(1 << 20), b""):
                        h.update(chunk)
                out[rel] = ("f", os.path.getsize(full), h.hexdigest())
        rel_dir = os.path.relpath(dirpath, root)
        if rel_dir != ".":
            out[rel_dir + "/"] = ("d", None)
    return out


def cmd_manifest(args):
    prefix = None
    outdir = None
    label = None
    i = 0
    while i < len(args):
        if args[i] == "--prefix" and i + 1 < len(args):
            prefix = args[i + 1]
            i += 2
        elif args[i] == "--out" and i + 1 < len(args):
            outdir = args[i + 1]
            i += 2
        elif args[i] == "--label" and i + 1 < len(args):
            label = args[i + 1]
            i += 2
        else:
            sys.exit("usage: ci_record.py manifest --prefix PATH --out DIR "
                     "[--label L]")
    if not prefix or not outdir:
        sys.exit("usage: ci_record.py manifest --prefix PATH --out DIR "
                 "[--label L]")
    if not os.path.isdir(prefix):
        sys.exit("build error: prefix %r is not a directory" % prefix)
    label = label or os.path.basename(os.path.normpath(prefix))
    os.makedirs(outdir, exist_ok=True)
    tree = walk_prefix(prefix)
    files = sorted(k for k, v in tree.items() if v[0] == "f")
    total = sum(tree[k][1] for k in files)
    now = datetime.now(timezone.utc).astimezone()
    rec = {"recorded_at": now.strftime("%Y-%m-%d %H:%M:%S %Z"),
           "prefix": prefix, "label": label,
           "files": len(files), "bytes_total": total,
           "entries": {k: tree[k] for k in sorted(tree)}}
    base = os.path.join(outdir, "manifest-%s" % label)
    with open(base + ".json", "w") as f:
        json.dump(rec, f, indent=2)
        f.write("\n")
    with open(base + ".md", "w") as f:
        f.write("# Artifact manifest (%s, %s)\n\n" % (label, rec["recorded_at"]))
        f.write("Prefix: `%s` — %d files, %d bytes total.\n\n" % (prefix, len(files), total))
        f.write("| Path | Size | SHA-256 |\n|---|---|---|\n")
        for k in files:
            _, size, sha = tree[k]
            f.write("| %s | %d | %s |\n" % (k, size, sha))
        links = sorted(k for k, v in tree.items() if v[0] == "l")
        if links:
            f.write("\nSymlinks:\n\n| Path | Target |\n|---|---|\n")
            for k in links:
                f.write("| %s | %s |\n" % (k, tree[k][1]))
    print("manifest: %s: %d files, %d bytes (%s.json)"
          % (label, len(files), total, base))
    return 0


def self_test():
    bad = 0

    def report(name, cond):
        print("self-test %-52s -> %s" % (name, "PASS" if cond else "FAIL"))
        return 0 if cond else 1

    with tempfile.TemporaryDirectory(prefix="ci-record-selftest-") as tmp:
        os.makedirs(os.path.join(tmp, "bin"))
        with open(os.path.join(tmp, "bin", "ch"), "wb") as f:
            f.write(b"fake-ch-bytes")
        os.symlink("ch", os.path.join(tmp, "bin", "ch-link"))
        tree = walk_prefix(tmp)
        bad += report("manifest finds the file",
                      tree.get("bin/ch", (None,))[0] == "f")
        bad += report("manifest records its size",
                      tree.get("bin/ch") == (
                          "f", 13, hashlib.sha256(b"fake-ch-bytes").hexdigest()))
        bad += report("manifest records the symlink",
                      tree.get("bin/ch-link") == ("l", "ch"))
    with open(TABLES) as f:
        tables_text = f.read()
    try:
        defs = parse_define_array(tables_text, "engine_defines")
        bad += report("tables parse finds engine defines", len(defs) > 20)
        dbg = parse_define_array(tables_text, "engine_defines_debug_static")
        bad += report("tables parse keeps legacy order",
                      dbg.index("-DDBG_DUMP=1") < dbg.index("-DDEBUG=1"))
    except ValueError:
        bad += report("tables parse finds engine defines", False)
        bad += report("tables parse keeps legacy order", False)
    try:
        parse_define_array(tables_text, "no_such_array")
        bad += report("tables parse fails loudly on drift", False)
    except ValueError:
        bad += report("tables parse fails loudly on drift", True)
    print("ci_record self-test: %s"
          % ("ALL CONTROLS BEHAVE" if bad == 0
             else "%d CONTROL(S) MISBEHAVE" % bad))
    return 1 if bad else 0


def main(argv):
    if len(argv) == 2 and argv[1] == "--self-test":
        return self_test()
    if len(argv) >= 2 and argv[1] == "inputs":
        return cmd_inputs(argv[2:])
    if len(argv) >= 2 and argv[1] == "manifest":
        return cmd_manifest(argv[2:])
    sys.exit("usage: ci_record.py {inputs|manifest} ... | --self-test")


if __name__ == "__main__":
    sys.exit(main(sys.argv))
