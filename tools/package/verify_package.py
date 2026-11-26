#!/usr/bin/env python3
#-------------------------------------------------------------------------------------------------------
# Copyright (c) ChakraCore Project Contributors. All rights reserved.
# Licensed under the MIT license. See LICENSE.txt file in the project root for full license information.
#-------------------------------------------------------------------------------------------------------
"""Package verification tool for ChakraCore release packages.

Verifies a release tarball or extracted staging directory against its MANIFEST.json:
- Validates manifest schema and required metadata
- Verifies SHA-256 checksums and file sizes for all packaged files
- Checks required executable permissions (0755) on binaries and shared libraries
- Flags any unexpected or missing files
"""

from __future__ import print_function

import argparse
import hashlib
import json
import os
import shutil
import stat
import subprocess
import sys
import tarfile
import tempfile

REQUIRED_CHAKRA_HEADERS = (
    "include/ChakraCore.h",
    "include/ChakraCommon.h",
    "include/ChakraDebug.h",
    "include/ChakraCoreVersion.h",
)
REQUIRED_NODEAPI_HEADERS = (
    "include/nodeapi/js_native_api.h",
    "include/nodeapi/js_native_api_types.h",
    "include/nodeapi/node_api.h",
    "include/nodeapi/node_api_types.h",
)
REQUIRED_NOTICES = (
    "LICENSE.txt",
    "THIRD-PARTY-NOTICES.txt",
    "README.md",
)
_MACHO_MAGICS = {
    b"\xfe\xed\xfa\xce",
    b"\xce\xfa\xed\xfe",
    b"\xfe\xed\xfa\xcf",
    b"\xcf\xfa\xed\xfe",
    b"\xca\xfe\xba\xbe",
    b"\xbe\xba\xfe\xca",
    b"\xca\xfe\xba\xbf",
    b"\xbf\xba\xfe\xca",
}
_ICU_PREFIXES = (
    "libicudata",
    "libicuuc",
    "libicui18n",
    "libicuio",
    "libicutu",
    "libicutest",
)


def is_macho_file(path):
    if not os.path.isfile(path) or os.path.islink(path):
        return False
    try:
        with open(path, "rb") as f:
            magic = f.read(4)
    except OSError:
        return False
    if magic not in _MACHO_MAGICS:
        return False
    if magic in (b"\xca\xfe\xba\xbe", b"\xbe\xba\xfe\xca"):
        res = subprocess.run(["otool", "-h", path], capture_output=True, text=True)
        return res.returncode == 0 and "Mach header" in (res.stdout or "")
    return True


def is_elf_file(path):
    if not os.path.isfile(path) or os.path.islink(path):
        return False
    try:
        with open(path, "rb") as f:
            return f.read(4) == b"\x7fELF"
    except OSError:
        return False


def is_icu_lib_basename(name):
    base = os.path.basename(name)
    return base.startswith(_ICU_PREFIXES) and (".dylib" in base or ".so" in base)


def parse_macho_load_commands(path):
    res = subprocess.run(["otool", "-l", path], capture_output=True, text=True)
    if res.returncode != 0 or "is not an object file" in (res.stdout or ""):
        raise RuntimeError("otool -l failed on {}: {}".format(path, res.stderr.strip()))
    dylib_id = None
    deps = []
    rpaths = []
    cmd = None
    for raw in res.stdout.splitlines():
        line = raw.strip()
        if line.startswith("cmd "):
            cmd = line[4:].strip()
        elif cmd == "LC_ID_DYLIB" and line.startswith("name "):
            dylib_id = line[5:].rsplit(" (offset ", 1)[0].strip()
        elif cmd in ("LC_LOAD_DYLIB", "LC_LOAD_WEAK_DYLIB",
                     "LC_REEXPORT_DYLIB", "LC_LAZY_LOAD_DYLIB") and line.startswith("name "):
            deps.append(line[5:].rsplit(" (offset ", 1)[0].strip())
        elif cmd == "LC_RPATH" and line.startswith("path "):
            rpaths.append(line[5:].rsplit(" (offset ", 1)[0].strip())
    return {"id": dylib_id, "deps": deps, "rpaths": rpaths}


def parse_elf_dynamic(path):
    res = subprocess.run(["readelf", "-d", path], capture_output=True, text=True)
    if res.returncode != 0:
        raise RuntimeError("readelf -d failed on {}: {}".format(path, res.stderr.strip()))
    needed = []
    rpaths = []
    for raw in res.stdout.splitlines():
        if "(NEEDED)" in raw and "[" in raw:
            needed.append(raw.split("[", 1)[1].rstrip("] \t"))
        elif ("(RPATH)" in raw or "(RUNPATH)" in raw) and "[" in raw:
            val = raw.split("[", 1)[1].rstrip("] \t")
            rpaths.extend(p for p in val.split(":") if p)
    return {"needed": needed, "rpaths": rpaths}


def icu_major_from_name(name):
    base = os.path.basename(name)
    parts = base.split(".")
    for part in parts:
        if part.isdigit():
            return part
    return None


def compute_sha256(file_path):
    h = hashlib.sha256()
    with open(file_path, "rb") as f:
        while True:
            chunk = f.read(65536)
            if not chunk:
                break
            h.update(chunk)
    return h.hexdigest()


def verify_extracted_dir(target_dir, expected_profile=None):
    errors = []

    # Check for nested root if tarball had single top-level directory
    manifest_path = os.path.join(target_dir, "MANIFEST.json")
    if not os.path.isfile(manifest_path):
        subdirs = [d for d in os.listdir(target_dir) if os.path.isdir(os.path.join(target_dir, d))]
        if len(subdirs) == 1:
            cand = os.path.join(target_dir, subdirs[0], "MANIFEST.json")
            if os.path.isfile(cand):
                target_dir = os.path.join(target_dir, subdirs[0])
                manifest_path = cand

    if not os.path.isfile(manifest_path):
        return False, ["MANIFEST.json not found in package directory: {}".format(target_dir)]

    try:
        with open(manifest_path, "r", encoding="utf-8") as f:
            manifest = json.load(f)
    except Exception as e:
        return False, ["Failed to parse MANIFEST.json: {}".format(e)]

    # Validate schema
    for field in ("version", "files"):
        if field not in manifest:
            errors.append("MANIFEST.json missing required field '{}'".format(field))

    if "target" not in manifest and "platform" not in manifest:
        errors.append("MANIFEST.json missing 'target' or 'platform' field")

    if "git_commit" not in manifest and "commit" not in manifest:
        errors.append("MANIFEST.json missing 'git_commit' or 'commit' field")

    if not isinstance(manifest.get("files"), list):
        errors.append("MANIFEST.json 'files' must be a list")
        return False, errors

    files_list = manifest.get("files", [])
    if len(files_list) == 0:
        errors.append("MANIFEST.json 'files' list is empty")

    expected_paths = set()

    for item in files_list:
        if not isinstance(item, dict):
            errors.append("Invalid item in 'files': expected dictionary, got {}".format(type(item)))
            continue
        rel_path = item.get("path")
        exp_size = item.get("size")
        exp_sha = item.get("sha256")

        if not rel_path:
            errors.append("Manifest file record missing 'path': {}".format(item))
            continue
        if exp_size is None or not isinstance(exp_size, int):
            errors.append("Manifest file record for '{}' invalid size: {}".format(rel_path, exp_size))
        if not exp_sha or not isinstance(exp_sha, str):
            errors.append("Manifest file record for '{}' invalid sha256: {}".format(rel_path, exp_sha))

        norm_rel = os.path.normpath(rel_path).replace("\\", "/")
        if norm_rel.startswith("../") or norm_rel.startswith("/"):
            errors.append("Manifest file path '{}' attempts directory traversal".format(rel_path))
            continue

        expected_paths.add(norm_rel)
        actual_path = os.path.join(target_dir, norm_rel)

        if not os.path.isfile(actual_path):
            errors.append("Missing expected file: {}".format(norm_rel))
            continue

        # Verify file size
        actual_size = os.path.getsize(actual_path)
        if actual_size != exp_size:
            errors.append("File size mismatch for '{}': expected {} bytes, got {} bytes".format(
                norm_rel, exp_size, actual_size))

        # Verify SHA-256
        actual_sha = compute_sha256(actual_path)
        if actual_sha.lower() != exp_sha.lower():
            errors.append("SHA-256 mismatch for '{}': expected {}, got {}".format(
                norm_rel, exp_sha, actual_sha))

        # Verify permissions: binaries in bin/ and libraries in lib/ require 0755 on POSIX
        if os.name != "nt":
            st_mode = os.stat(actual_path).st_mode
            if norm_rel.startswith("bin/") or norm_rel.startswith("lib/"):
                # Check executable bits
                if not (st_mode & stat.S_IXUSR):
                    errors.append("File '{}' missing user executable permission (expected 0755, mode: {:o})".format(
                        norm_rel, st_mode & 0o777))
                if not (st_mode & stat.S_IXGRP):
                    errors.append("File '{}' missing group executable permission (expected 0755, mode: {:o})".format(
                        norm_rel, st_mode & 0o777))
                if not (st_mode & stat.S_IXOTH):
                    errors.append("File '{}' missing others executable permission (expected 0755, mode: {:o})".format(
                        norm_rel, st_mode & 0o777))

    # Check for unexpected files
    actual_files = set()
    for root, dirs, files in os.walk(target_dir):
        for f in files:
            full_p = os.path.join(root, f)
            rel_p = os.path.relpath(full_p, target_dir).replace("\\", "/")
            if rel_p == "MANIFEST.json":
                continue
            actual_files.add(rel_p)

    unexpected = actual_files - expected_paths
    for unexp in sorted(unexpected):
        errors.append("Unexpected file found in package: {}".format(unexp))

    missing = expected_paths - actual_files
    for miss in sorted(missing):
        errors.append("Expected file not found in package: {}".format(miss))

    # Required layout: headers, notices, binaries, and shared/import libraries
    pkg_target = manifest.get("target") or manifest.get("platform") or ""
    is_windows = pkg_target.startswith("windows-") or os.path.isfile(os.path.join(target_dir, "bin", "ch.exe"))
    is_darwin = (not is_windows) and (
        pkg_target.startswith("darwin-")
        or (not pkg_target.startswith("linux-") and sys.platform == "darwin")
    )
    for req_rel in REQUIRED_CHAKRA_HEADERS + REQUIRED_NODEAPI_HEADERS + REQUIRED_NOTICES:
        if req_rel not in actual_files:
            errors.append("Missing required package file: {}".format(req_rel))
    if is_windows:
        for req_rel in ("bin/ch.exe", "bin/ch-nodeapi.exe", "bin/ChakraCore.dll",
                        "bin/libChakraNodeApi.dll", "lib/ChakraCore.lib", "lib/libChakraNodeApi.lib"):
            if req_rel not in actual_files:
                errors.append("Missing required Windows package file: {}".format(req_rel))
    else:
        ext = "dylib" if is_darwin else "so"
        for req_rel in ("bin/ch", "bin/ch-nodeapi",
                        "lib/libChakraCore.{}".format(ext),
                        "lib/libChakraNodeApi.{}".format(ext),
                        "bin/libChakraCore.{}".format(ext)):
            if req_rel not in actual_files:
                errors.append("Missing required package file: {}".format(req_rel))

    if "profile" in manifest and (not isinstance(manifest["profile"], str) or not manifest["profile"].strip()):
        errors.append("MANIFEST.json 'profile' must be a non-empty string when present")
    if expected_profile is not None and manifest.get("profile") != expected_profile:
        errors.append("MANIFEST.json profile mismatch: expected {!r}, got {!r}".format(
            expected_profile, manifest.get("profile")))

    icu_mode = manifest.get("icu_mode")
    if icu_mode is not None and icu_mode not in ("bundled", "external", "none"):
        errors.append("MANIFEST.json 'icu_mode' invalid: {!r}".format(icu_mode))

    if icu_mode == "bundled":
        icu_libs = manifest.get("icu_libraries")
        if not isinstance(icu_libs, list) or not icu_libs:
            errors.append("MANIFEST.json 'icu_mode' is 'bundled' but 'icu_libraries' is missing or empty")
        else:
            for lib_name in icu_libs:
                if not isinstance(lib_name, str) or "/" in lib_name or "\\" in lib_name:
                    errors.append("Invalid bundled ICU library entry: {!r}".format(lib_name))
                    continue
                exp_rel = "bin/{}".format(lib_name) if is_windows else "lib/{}".format(lib_name)
                if exp_rel not in actual_files:
                    errors.append("Manifest declares bundled ICU '{}' missing at '{}'".format(lib_name, exp_rel))

    # Mach-O / ELF ICU linkage verification (runs on native binaries only, skipped on dummy fixtures)
    if sys.platform == "darwin" and not is_windows:
        ext_icu_majors = set()
        for rel_p in sorted(actual_files):
            if not (rel_p.startswith("bin/") or rel_p.startswith("lib/")):
                continue
            full_p = os.path.join(target_dir, *rel_p.split("/"))
            if not is_macho_file(full_p):
                continue
            cs_res = subprocess.run(["codesign", "--verify", "--strict", full_p], capture_output=True, text=True)
            if cs_res.returncode != 0:
                errors.append("Code signature verification failed for '{}': {}".format(
                    rel_p, cs_res.stderr.strip() or cs_res.stdout.strip()))
            try:
                info = parse_macho_load_commands(full_p)
            except RuntimeError as e:
                errors.append(str(e))
                continue
            for rp in info.get("rpaths", []):
                if ".zig-cache" in rp:
                    errors.append("Mach-O '{}' has forbidden .zig-cache LC_RPATH '{}'".format(rel_p, rp))
                elif rp.startswith(("/opt/", "/usr/local/")):
                    errors.append("Mach-O '{}' has forbidden external LC_RPATH '{}'".format(rel_p, rp))
                elif not rp.startswith("@"):
                    errors.append("Mach-O '{}' has non-relocatable LC_RPATH '{}' (must start with '@')".format(
                        rel_p, rp))
            uses_rpath_icu = False
            for dep in info["deps"]:
                is_external = not dep.startswith(("@rpath/", "@loader_path/", "@executable_path/",
                                                  "/usr/lib/", "/System/Library/"))
                if is_external:
                    if icu_mode != "external":
                        errors.append("Mach-O '{}' has non-portable external dependency '{}'".format(rel_p, dep))
                    elif not is_icu_lib_basename(dep):
                        errors.append("Mach-O '{}' has unexpected non-system dependency '{}'".format(rel_p, dep))
                    else:
                        maj = icu_major_from_name(dep)
                        if maj:
                            ext_icu_majors.add(maj)
                elif dep.startswith(("@rpath/", "@loader_path/", "@executable_path/")):
                    dep_base = os.path.basename(dep)
                    if is_icu_lib_basename(dep_base):
                        uses_rpath_icu = uses_rpath_icu or dep.startswith("@rpath/")
                        if "lib/{}".format(dep_base) not in actual_files:
                            errors.append("Mach-O '{}' references '{}' missing from package lib/".format(rel_p, dep))
            if uses_rpath_icu:
                if rel_p.startswith("bin/") and "@executable_path/../lib" not in info["rpaths"]:
                    errors.append("Executable '{}' references @rpath ICU without @executable_path/../lib LC_RPATH".format(
                        rel_p))
                elif rel_p.startswith("lib/") and "@loader_path" not in info["rpaths"]:
                    errors.append("Library '{}' references @rpath ICU without @loader_path LC_RPATH".format(rel_p))
        if len(ext_icu_majors) > 1:
            errors.append("Inconsistent external ICU major versions across package binaries: {}".format(
                sorted(ext_icu_majors)))
    elif sys.platform.startswith("linux") and not is_windows and shutil.which("readelf"):
        ext_icu_majors = set()
        for rel_p in sorted(actual_files):
            if not (rel_p.startswith("bin/") or rel_p.startswith("lib/")):
                continue
            full_p = os.path.join(target_dir, *rel_p.split("/"))
            if not is_elf_file(full_p):
                continue
            try:
                elf_info = parse_elf_dynamic(full_p)
            except RuntimeError as e:
                errors.append(str(e))
                continue
            for rp in elf_info["rpaths"]:
                if ".zig-cache" in rp:
                    errors.append("ELF '{}' has forbidden .zig-cache RPATH/RUNPATH '{}'".format(rel_p, rp))
                elif rp.startswith(("/opt/", "/usr/local/")):
                    errors.append("ELF '{}' has forbidden /opt or /usr/local RPATH/RUNPATH '{}'".format(rel_p, rp))
                elif not rp.startswith(("$ORIGIN", "/")):
                    errors.append("ELF '{}' has relative RPATH/RUNPATH '{}' (must start with '$ORIGIN' or '/')".format(
                        rel_p, rp))
            for dep in elf_info["needed"]:
                if "libstdc++" in dep or "libc++" in dep:
                    errors.append("ELF '{}' has unexpected C++ runtime NEEDED '{}'".format(rel_p, dep))
                if is_icu_lib_basename(dep):
                    maj = icu_major_from_name(dep)
                    if maj:
                        ext_icu_majors.add(maj)
                    if icu_mode == "bundled" and "lib/{}".format(dep) not in actual_files:
                        errors.append("ELF '{}' NEEDED '{}' missing from bundled package lib/".format(rel_p, dep))
            if icu_mode == "bundled":
                want_rp = "$ORIGIN/../lib" if rel_p.startswith("bin/") else "$ORIGIN"
                if any(is_icu_lib_basename(d) for d in elf_info["needed"]) and want_rp not in elf_info["rpaths"]:
                    errors.append("ELF '{}' missing '{}' RPATH/RUNPATH for bundled ICU".format(rel_p, want_rp))
        if len(ext_icu_majors) > 1:
            errors.append("Inconsistent ICU major versions across ELF binaries: {}".format(sorted(ext_icu_majors)))

    return (len(errors) == 0), errors


def find_tarball_in_dir(d, expected_profile=None):
    try:
        # Prefer newest matching chakracore-*.tar.gz (and profile match if requested)
        candidates = []
        for f in os.listdir(d):
            if f.endswith(".tar.gz") and (f.startswith("chakracore-") or "chakracore" in f):
                full = os.path.join(d, f)
                if os.path.isfile(full):
                    prof_match = 1 if (expected_profile and "-{}-".format(expected_profile) in f) else 0
                    candidates.append((prof_match, os.path.getmtime(full), full))
        if candidates:
            candidates.sort(reverse=True)
            return candidates[0][2]
    except Exception:
        pass
    return None


def verify_package(source_path, expected_profile=None):
    source_path = os.path.abspath(source_path)
    if not os.path.exists(source_path):
        return False, ["Source path does not exist: {}".format(source_path)]

    if os.path.isdir(source_path):
        # If the directory contains a tarball, verify the tarball
        tb = find_tarball_in_dir(source_path, expected_profile=expected_profile)
        if tb and not os.path.isfile(os.path.join(source_path, "MANIFEST.json")):
            return verify_package(tb, expected_profile=expected_profile)
        print("Verifying package directory: {}".format(source_path))
        return verify_extracted_dir(source_path, expected_profile=expected_profile)

    if os.path.isfile(source_path):
        print("Verifying package tarball: {}".format(source_path))
        with tempfile.TemporaryDirectory(prefix="chakra-verify-") as tmpdir:
            tar_mode_errors = []
            try:
                with tarfile.open(source_path, "r:*") as tar:
                    for ti in tar.getmembers():
                        if ti.isreg():
                            arc = ti.name.replace("\\", "/")
                            while arc.startswith("./"):
                                arc = arc[2:]
                            parts = [p for p in arc.split("/") if p]
                            is_bin_or_lib = (
                                (len(parts) >= 2 and parts[0] in ("bin", "lib"))
                                or (len(parts) >= 3 and parts[1] in ("bin", "lib"))
                            )
                            if is_bin_or_lib and (ti.mode & 0o755) != 0o755:
                                tar_mode_errors.append(
                                    "Tar entry '{}' missing 0755 permission (mode: {:o})".format(
                                        arc, ti.mode & 0o777))
                    if hasattr(tarfile, "data_filter"):
                        tar.extractall(path=tmpdir, filter="data")
                    else:
                        tar.extractall(path=tmpdir)
            except Exception as e:
                return False, ["Failed to extract tarball '{}': {}".format(source_path, e)]
            ok, errs = verify_extracted_dir(tmpdir, expected_profile=expected_profile)
            if tar_mode_errors:
                errs = tar_mode_errors + errs
                ok = False
            return ok, errs

    return False, ["Unknown source path type: {}".format(source_path)]


def main(argv=None):
    parser = argparse.ArgumentParser(description="ChakraCore Package Verification Tool")
    parser.add_argument("--tarball", default=None,
                        help="Path to release tarball (.tar.gz)")
    parser.add_argument("--dir", default=None,
                        help="Path to extracted package directory")
    parser.add_argument("--profile", default=None,
                        help="Expected profile name in MANIFEST.json")
    parser.add_argument("target", nargs="?", default=None,
                        help="Path to tarball or directory to verify")

    args = parser.parse_args(argv)

    target_path = args.tarball or args.dir or args.target
    if not target_path:
        parser.print_help()
        sys.exit(1)

    ok, errors = verify_package(target_path, expected_profile=args.profile)
    if ok:
        print("VERIFY PASS: Package '{}' verified successfully against MANIFEST.json (0 errors).".format(target_path))
        return 0
    else:
        print("VERIFY FAILED: {} error(s) found in package '{}':".format(len(errors), target_path), file=sys.stderr)
        for err in errors:
            print("  - {}".format(err), file=sys.stderr)
        return 1


if __name__ == "__main__":
    sys.exit(main())
