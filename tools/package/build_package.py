#!/usr/bin/env python3
#-------------------------------------------------------------------------------------------------------
# Copyright (c) ChakraCore Project Contributors. All rights reserved.
# Licensed under the MIT license. See LICENSE.txt file in the project root for full license information.
#-------------------------------------------------------------------------------------------------------
"""Release packaging tool for ChakraCore.

Assembles binaries, shared libraries, headers, notices, and MANIFEST.json into a
standard staging layout and generates a deterministic release tarball:
chakracore-<version>-<target>.tar.gz (or
chakracore-<version>-<profile>-<target>.tar.gz when --profile is set).
"""

from __future__ import print_function

import argparse
import collections
import datetime
import gzip
import hashlib
import json
import os
import platform
import re
import shutil
import stat
import subprocess
import sys
import tarfile

# Default clamped epoch for deterministic tarball generation (2026-01-01T00:00:00Z)
DEFAULT_SOURCE_DATE_EPOCH = 1767225600

REQUIRED_CHAKRA_HEADERS = (
    "ChakraCore.h",
    "ChakraCommon.h",
    "ChakraDebug.h",
    "ChakraCoreVersion.h",
)
REQUIRED_NODEAPI_HEADERS = (
    "js_native_api.h",
    "js_native_api_types.h",
    "node_api.h",
    "node_api_types.h",
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
_ICU_DYLIB_PREFIXES = (
    "libicudata",
    "libicuuc",
    "libicui18n",
    "libicuio",
    "libicutu",
    "libicutest",
)


def get_repo_root():
    # File is at tools/package/build_package.py -> parent of parent is repo root
    this_dir = os.path.dirname(os.path.abspath(__file__))
    return os.path.abspath(os.path.join(this_dir, "..", ".."))


def get_default_target():
    plat = sys.platform
    mach = platform.machine().lower()
    if plat == "darwin":
        os_name = "darwin"
        arch = "arm64" if mach in ("arm64", "aarch64") else "x64"
    elif plat.startswith("linux"):
        os_name = "linux"
        arch = "arm64" if mach in ("arm64", "aarch64") else "x64"
    elif plat == "win32":
        os_name = "windows"
        arch = "x64" if mach in ("amd64", "x86_64") else "arm64"
    else:
        os_name = plat
        arch = mach
    return "{}-{}".format(os_name, arch)


def get_default_version(repo_root):
    if os.environ.get("CHAKRA_CORE_VERSION"):
        return os.environ["CHAKRA_CORE_VERSION"].strip()
    try:
        res = subprocess.run(
            ["git", "describe", "--tags", "--always"],
            cwd=repo_root,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            check=True
        )
        v = res.stdout.decode("utf-8").strip()
        if v:
            return v
    except Exception:
        pass
    ver_h = os.path.join(repo_root, "lib", "Common", "ChakraCoreVersion.h")
    if os.path.isfile(ver_h):
        with open(ver_h, "r", encoding="utf-8") as f:
            content = f.read()
        maj = re.search(r"#define\s+CHAKRA_CORE_MAJOR_VERSION\s+(\d+)", content)
        mino = re.search(r"#define\s+CHAKRA_CORE_MINOR_VERSION\s+(\d+)", content)
        pat = re.search(r"#define\s+CHAKRA_CORE_PATCH_VERSION\s+(\d+)", content)
        if maj and mino and pat:
            return "{}.{}.{}".format(maj.group(1), mino.group(1), pat.group(1))
    return "1.13.0"


def get_git_commit(repo_root):
    try:
        res = subprocess.run(
            ["git", "rev-parse", "HEAD"],
            cwd=repo_root,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            check=True
        )
        commit = res.stdout.decode("utf-8").strip()
        if commit:
            return commit
    except Exception:
        pass
    return "unknown"


def compute_sha256(file_path):
    h = hashlib.sha256()
    with open(file_path, "rb") as f:
        while True:
            chunk = f.read(65536)
            if not chunk:
                break
            h.update(chunk)
    return h.hexdigest()


def find_file(search_paths, filename):
    for p in search_paths:
        cand = os.path.join(p, filename)
        if os.path.isfile(cand):
            return cand
    return None


def copy_with_mode(src, dst, mode):
    dst_dir = os.path.dirname(dst)
    if not os.path.isdir(dst_dir):
        os.makedirs(dst_dir)
    # Copy data dereferencing symlinks
    shutil.copyfile(src, dst)
    os.chmod(dst, mode)


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


def is_icu_dylib_basename(name):
    base = os.path.basename(name)
    return base.startswith(_ICU_DYLIB_PREFIXES) and base.endswith(".dylib")


def is_external_dylib_path(dep):
    return not dep.startswith(("@", "/usr/lib/", "/System/Library/"))


def parse_macho_load_commands(path):
    res = subprocess.run(["otool", "-l", path], capture_output=True, text=True)
    if res.returncode != 0 or "is not an object file" in (res.stdout or ""):
        raise RuntimeError("otool -l failed on '{}': {}".format(path, res.stderr.strip()))
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


def run_checked(cmd):
    res = subprocess.run(cmd, capture_output=True, text=True)
    if res.returncode != 0:
        raise RuntimeError("Command failed (rc={}): {}\nstdout: {}\nstderr: {}".format(
            res.returncode, " ".join(cmd), res.stdout.strip(), res.stderr.strip()))
    return res


def bundle_macos_icu(lib_dir, macho_bins, macho_libs, require_icu=False):
    """Bundle external Homebrew/local ICU dylibs into lib_dir and rewrite load paths."""
    queue = collections.deque()
    for p in macho_bins + macho_libs:
        info = parse_macho_load_commands(p)
        for dep in info["deps"]:
            if is_icu_dylib_basename(dep) and is_external_dylib_path(dep):
                queue.append((dep, None))

    if not queue:
        if require_icu:
            raise RuntimeError(
                "Requested --icu-mode=bundled, but no external ICU dylib dependencies were found in staged Mach-O binaries"
            )
        return "external", []

    bundled_icu = {}
    while queue:
        dep_path, referrer_dir = queue.popleft()
        base = os.path.basename(dep_path)
        if base in bundled_icu:
            continue

        if dep_path.startswith("@loader_path/"):
            if not referrer_dir:
                raise RuntimeError("Cannot resolve '{}' without referrer directory".format(dep_path))
            src_cand = os.path.join(referrer_dir, dep_path[len("@loader_path/"):])
        else:
            src_cand = dep_path

        if not os.path.isfile(src_cand):
            raise RuntimeError("External ICU library not found on disk: '{}' (referenced as '{}')".format(
                src_cand, dep_path))

        real_src = os.path.realpath(src_cand)
        dst_icu = os.path.join(lib_dir, base)
        copy_with_mode(real_src, dst_icu, 0o755)
        bundled_icu[base] = dst_icu

        icu_info = parse_macho_load_commands(dst_icu)
        src_dir = os.path.dirname(real_src)
        for sub_dep in icu_info["deps"]:
            if is_icu_dylib_basename(sub_dep):
                if is_external_dylib_path(sub_dep) or sub_dep.startswith("@loader_path/"):
                    queue.append((sub_dep, src_dir))
            elif is_external_dylib_path(sub_dep):
                raise RuntimeError(
                    "Bundled ICU library '{}' has unexpected external dependency '{}'".format(base, sub_dep)
                )

    for base in sorted(bundled_icu):
        dst_icu = bundled_icu[base]
        icu_info = parse_macho_load_commands(dst_icu)
        cmd = ["install_name_tool", "-id", "@rpath/{}".format(base)]
        for sub_dep in icu_info["deps"]:
            sub_base = os.path.basename(sub_dep)
            if sub_base in bundled_icu and sub_dep != "@loader_path/{}".format(sub_base):
                cmd.extend(["-change", sub_dep, "@loader_path/{}".format(sub_base)])
        cmd.append(dst_icu)
        run_checked(cmd)
        run_checked(["codesign", "-f", "-s", "-", dst_icu])
        run_checked(["codesign", "--verify", "--strict", dst_icu])

    targets = [(p, "@executable_path/../lib") for p in macho_bins] + [
        (p, "@loader_path") for p in macho_libs
    ]
    for target_p, want_rpath in targets:
        info = parse_macho_load_commands(target_p)
        cmd = ["install_name_tool"]
        changed = False
        if want_rpath not in info["rpaths"]:
            cmd.extend(["-add_rpath", want_rpath])
            changed = True
        for dep in info["deps"]:
            if is_icu_dylib_basename(dep) and is_external_dylib_path(dep):
                cmd.extend(["-change", dep, "@rpath/{}".format(os.path.basename(dep))])
                changed = True
        if changed:
            cmd.append(target_p)
            run_checked(cmd)
            run_checked(["codesign", "-f", "-s", "-", target_p])
            run_checked(["codesign", "--verify", "--strict", target_p])

    return "bundled", sorted(bundled_icu.keys())


def bundle_linux_icu(bin_dir, lib_dir):
    """Bundle ICU .so dependencies into lib_dir on Linux using ldd + patchelf."""
    patchelf = shutil.which("patchelf")
    ldd = shutil.which("ldd")
    if not patchelf or not ldd:
        raise RuntimeError(
            "Requested --icu-mode=bundled on Linux, but required tools are missing (patchelf={}, ldd={})".format(
                bool(patchelf), bool(ldd))
        )

    elf_bins = [os.path.join(bin_dir, f) for f in sorted(os.listdir(bin_dir))
                if is_elf_file(os.path.join(bin_dir, f))]
    elf_libs = [os.path.join(lib_dir, f) for f in sorted(os.listdir(lib_dir))
                if is_elf_file(os.path.join(lib_dir, f))]
    if not elf_bins and not elf_libs:
        raise RuntimeError("Requested --icu-mode=bundled, but no ELF binaries/libraries were found")

    bundled_icu = {}
    for target in elf_bins + elf_libs:
        res = run_checked([ldd, target])
        for line in res.stdout.splitlines():
            if " => " not in line:
                continue
            soname, rest = line.strip().split(" => ", 1)
            soname = soname.strip()
            if not (soname.startswith(_ICU_DYLIB_PREFIXES) and ".so" in soname):
                continue
            resolved = rest.split(" (", 1)[0].strip()
            if not resolved or resolved == "not found" or not os.path.isfile(resolved):
                raise RuntimeError("Could not resolve Linux ICU library '{}' via ldd on '{}'".format(soname, target))
            if soname not in bundled_icu:
                dst = os.path.join(lib_dir, soname)
                copy_with_mode(os.path.realpath(resolved), dst, 0o755)
                bundled_icu[soname] = dst

    if not bundled_icu:
        raise RuntimeError("Requested --icu-mode=bundled on Linux, but no ICU shared libraries were linked")

    for bin_p in elf_bins:
        run_checked([patchelf, "--set-rpath", "$ORIGIN/../lib", bin_p])
    for lib_p in elf_libs + list(bundled_icu.values()):
        run_checked([patchelf, "--set-rpath", "$ORIGIN", lib_p])
    return "bundled", sorted(bundled_icu.keys())


def stage_package(prefix, staging_dir, repo_root, version, target, profile=None, icu_mode="auto"):
    prefix = os.path.abspath(prefix)
    if not os.path.isdir(prefix):
        raise RuntimeError("Install prefix directory does not exist: {}".format(prefix))

    marker_path = os.path.join(prefix, ".profile")
    if profile and os.path.isfile(marker_path):
        with open(marker_path, "r", encoding="utf-8") as mf:
            marker_val = mf.read().strip()
        if marker_val != profile:
            raise RuntimeError(
                "Profile mismatch: --profile '{}' does not match {} ({!r})".format(
                    profile, marker_path, marker_val)
            )

    is_windows = target.startswith("windows-")
    is_darwin = (not is_windows) and (
        target.startswith("darwin-")
        or (not target.startswith("linux-") and sys.platform == "darwin")
    )

    bin_search_paths = [os.path.join(prefix, "bin"), prefix]
    lib_search_paths = [os.path.join(prefix, "lib"), os.path.join(prefix, "bin"), prefix]
    dll_search_paths = [os.path.join(prefix, "bin"), os.path.join(prefix, "lib"), prefix]

    missing = []
    staged_bins = []
    staged_libs = []

    if is_windows:
        ch_src = find_file(bin_search_paths, "ch.exe")
        if not ch_src:
            missing.append("bin/ch.exe (not found in prefix '{}')".format(prefix))
        ch_nodeapi_src = find_file(bin_search_paths, "ch-nodeapi.exe")
        if not ch_nodeapi_src:
            missing.append("bin/ch-nodeapi.exe (not found in prefix '{}')".format(prefix))
        core_dll_src = find_file(dll_search_paths, "ChakraCore.dll")
        if not core_dll_src:
            missing.append("bin/ChakraCore.dll (not found in prefix '{}')".format(prefix))
        nodeapi_dll_src = find_file(dll_search_paths, "libChakraNodeApi.dll")
        if not nodeapi_dll_src:
            missing.append("bin/libChakraNodeApi.dll (not found in prefix '{}')".format(prefix))
        core_imp_src = find_file(lib_search_paths, "ChakraCore.lib")
        if not core_imp_src:
            missing.append("lib/ChakraCore.lib (not found in prefix '{}')".format(prefix))
        nodeapi_imp_src = find_file(lib_search_paths, "libChakraNodeApi.lib")
        if not nodeapi_imp_src:
            missing.append("lib/libChakraNodeApi.lib (not found in prefix '{}')".format(prefix))
    else:
        core_dylib_name = "libChakraCore.dylib" if is_darwin else "libChakraCore.so"
        nodeapi_dylib_name = "libChakraNodeApi.dylib" if is_darwin else "libChakraNodeApi.so"

        ch_src = find_file(bin_search_paths, "ch")
        if not ch_src:
            missing.append("bin/ch (not found in prefix '{}')".format(prefix))
        ch_nodeapi_src = find_file(bin_search_paths, "ch-nodeapi")
        if not ch_nodeapi_src:
            missing.append("bin/ch-nodeapi (not found in prefix '{}')".format(prefix))
        core_lib_src = find_file(lib_search_paths, core_dylib_name)
        if not core_lib_src:
            missing.append("lib/{} (not found in prefix '{}')".format(core_dylib_name, prefix))
        nodeapi_lib_src = find_file(lib_search_paths, nodeapi_dylib_name)
        if not nodeapi_lib_src:
            missing.append("lib/{} (not found in prefix '{}')".format(nodeapi_dylib_name, prefix))

    # Strict prefix-only headers (no fallback to repo lib/Jsrt or include/nodeapi).
    header_dir = os.path.join(prefix, "include")
    chakra_headers = []
    for hname in REQUIRED_CHAKRA_HEADERS:
        h_src = find_file([header_dir], hname)
        if not h_src:
            missing.append("include/{} (not found in prefix '{}')".format(hname, prefix))
        else:
            chakra_headers.append((hname, h_src))

    nodeapi_header_dir = os.path.join(prefix, "include", "nodeapi")
    nodeapi_headers = []
    for nh in REQUIRED_NODEAPI_HEADERS:
        nh_src = os.path.join(nodeapi_header_dir, nh)
        if not os.path.isfile(nh_src):
            missing.append("include/nodeapi/{} (not found in prefix '{}')".format(nh, prefix))
    if os.path.isdir(nodeapi_header_dir):
        for fname in sorted(os.listdir(nodeapi_header_dir)):
            if fname.endswith(".h"):
                fpath = os.path.join(nodeapi_header_dir, fname)
                if os.path.isfile(fpath):
                    nodeapi_headers.append((fname, fpath))

    if missing:
        raise RuntimeError("Missing required artifact(s) in prefix '{}':\n  - {}".format(
            prefix, "\n  - ".join(missing)))

    # Staging structure
    bin_dir = os.path.join(staging_dir, "bin")
    lib_dir = os.path.join(staging_dir, "lib")
    include_dir = os.path.join(staging_dir, "include")
    include_nodeapi_dir = os.path.join(include_dir, "nodeapi")

    for d in (bin_dir, lib_dir, include_dir, include_nodeapi_dir):
        if not os.path.isdir(d):
            os.makedirs(d)

    if is_windows:
        copy_with_mode(ch_src, os.path.join(bin_dir, "ch.exe"), 0o755)
        copy_with_mode(ch_nodeapi_src, os.path.join(bin_dir, "ch-nodeapi.exe"), 0o755)
        # Copy DLLs directly beside bin/ on Windows (no symlinks).
        copy_with_mode(core_dll_src, os.path.join(bin_dir, "ChakraCore.dll"), 0o755)
        copy_with_mode(nodeapi_dll_src, os.path.join(bin_dir, "libChakraNodeApi.dll"), 0o755)
        copy_with_mode(core_imp_src, os.path.join(lib_dir, "ChakraCore.lib"), 0o755)
        copy_with_mode(nodeapi_imp_src, os.path.join(lib_dir, "libChakraNodeApi.lib"), 0o755)
    else:
        ch_dst = os.path.join(bin_dir, "ch")
        ch_nodeapi_dst = os.path.join(bin_dir, "ch-nodeapi")
        core_lib_dst = os.path.join(lib_dir, core_dylib_name)
        nodeapi_lib_dst = os.path.join(lib_dir, nodeapi_dylib_name)
        copy_with_mode(ch_src, ch_dst, 0o755)
        copy_with_mode(ch_nodeapi_src, ch_nodeapi_dst, 0o755)
        copy_with_mode(core_lib_src, core_lib_dst, 0o755)
        copy_with_mode(nodeapi_lib_src, nodeapi_lib_dst, 0o755)
        staged_bins.extend([ch_dst, ch_nodeapi_dst])
        staged_libs.extend([core_lib_dst, nodeapi_lib_dst])

        # Co-location link: ch dynamically dlopens libChakraCore from its own executable dir
        # via GetBinaryPathWithFileNameA. Stage relative symlink bin/libChakraCore.* -> ../lib/libChakraCore.*
        # so that bin/ch runs out-of-the-box while canonical shared libraries reside in lib/.
        bin_core_link = os.path.join(bin_dir, core_dylib_name)
        if os.path.islink(bin_core_link) or os.path.isfile(bin_core_link):
            os.remove(bin_core_link)
        try:
            os.symlink(os.path.join("..", "lib", core_dylib_name), bin_core_link)
        except OSError:
            copy_with_mode(core_lib_src, bin_core_link, 0o755)

    for hname, h_src in chakra_headers:
        copy_with_mode(h_src, os.path.join(include_dir, hname), 0o644)
    for fname, nh_src in nodeapi_headers:
        copy_with_mode(nh_src, os.path.join(include_nodeapi_dir, fname), 0o644)

    # ICU bundling / mode handling (B-1)
    effective_icu_mode = "external"
    bundled_icu_names = []
    if is_darwin:
        if icu_mode in ("auto", "bundled"):
            macho_bins = [p for p in staged_bins if is_macho_file(p)]
            macho_libs = [p for p in staged_libs if is_macho_file(p)]
            if macho_bins or macho_libs:
                effective_icu_mode, bundled_icu_names = bundle_macos_icu(
                    lib_dir, macho_bins, macho_libs, require_icu=(icu_mode == "bundled"))
            elif icu_mode == "bundled":
                raise RuntimeError(
                    "Requested --icu-mode=bundled, but no Mach-O binaries/libraries were found in prefix '{}'".format(
                        prefix)
                )
    elif is_windows:
        if icu_mode == "bundled":
            win_icu = []
            for d in (os.path.join(prefix, "bin"), prefix):
                if os.path.isdir(d):
                    for fn in sorted(os.listdir(d)):
                        low = fn.lower()
                        if low.endswith(".dll") and low.startswith(("icuuc", "icuin", "icudt")):
                            src_dll = os.path.join(d, fn)
                            if os.path.isfile(src_dll) and fn not in win_icu:
                                copy_with_mode(src_dll, os.path.join(bin_dir, fn), 0o755)
                                win_icu.append(fn)
            if not win_icu:
                raise RuntimeError(
                    "Requested --icu-mode=bundled on Windows, but no icu*.dll files were found in prefix '{}'".format(
                        prefix)
                )
            effective_icu_mode = "bundled"
            bundled_icu_names = sorted(win_icu)
    else:
        if icu_mode == "bundled":
            effective_icu_mode, bundled_icu_names = bundle_linux_icu(bin_dir, lib_dir)

    # Stage notices and documentation
    for doc_name in ("LICENSE.txt", "THIRD-PARTY-NOTICES.txt", "README.md"):
        doc_src = os.path.join(repo_root, doc_name)
        if not os.path.isfile(doc_src):
            raise RuntimeError("Required file '{}' not found at {}".format(doc_name, doc_src))
        copy_with_mode(doc_src, os.path.join(staging_dir, doc_name), 0o644)

    # Generate MANIFEST.json
    manifest_records = []
    for root, dirs, files in os.walk(staging_dir):
        dirs.sort()
        for fname in sorted(files):
            if fname == "MANIFEST.json":
                continue
            full_path = os.path.join(root, fname)
            rel_path = os.path.relpath(full_path, staging_dir).replace("\\", "/")
            fsize = os.path.getsize(full_path)
            fsha = compute_sha256(full_path)
            manifest_records.append({
                "path": rel_path,
                "size": fsize,
                "sha256": fsha,
            })

    manifest_records.sort(key=lambda x: x["path"])
    commit = get_git_commit(repo_root)
    raw_epoch = os.environ.get("SOURCE_DATE_EPOCH")
    if raw_epoch and raw_epoch.isdigit():
        epoch_sec = int(raw_epoch)
    else:
        epoch_sec = DEFAULT_SOURCE_DATE_EPOCH
    timestamp = datetime.datetime.fromtimestamp(epoch_sec, tz=datetime.timezone.utc).isoformat()

    manifest_data = {
        "version": version,
        "target": target,
        "platform": target,
        "git_commit": commit,
        "commit": commit,
        "timestamp": timestamp,
        "icu_mode": effective_icu_mode,
        "files": manifest_records,
    }
    if profile:
        manifest_data["profile"] = profile
    if bundled_icu_names:
        manifest_data["icu_libraries"] = sorted(bundled_icu_names)

    manifest_path = os.path.join(staging_dir, "MANIFEST.json")
    with open(manifest_path, "w", encoding="utf-8", newline="\n") as f:
        json.dump(manifest_data, f, indent=2, sort_keys=True)
        f.write("\n")
    os.chmod(manifest_path, 0o644)

    return manifest_data


def create_deterministic_tarball(staging_dir, tarball_path, mtime=None):
    if mtime is None:
        raw_epoch = os.environ.get("SOURCE_DATE_EPOCH")
        if raw_epoch and raw_epoch.isdigit():
            mtime = int(raw_epoch)
        else:
            mtime = DEFAULT_SOURCE_DATE_EPOCH

    parent_dir = os.path.dirname(tarball_path)
    if not os.path.isdir(parent_dir):
        os.makedirs(parent_dir)

    all_items = []
    for root, dirs, files in os.walk(staging_dir):
        dirs.sort()
        for d in dirs:
            full_p = os.path.join(root, d)
            rel_p = os.path.relpath(full_p, staging_dir).replace("\\", "/")
            all_items.append((rel_p, full_p, True))
        for f in sorted(files):
            full_p = os.path.join(root, f)
            rel_p = os.path.relpath(full_p, staging_dir).replace("\\", "/")
            all_items.append((rel_p, full_p, False))

    all_items.sort(key=lambda x: x[0])

    with open(tarball_path, "wb") as f_out:
        with gzip.GzipFile(filename="", mode="wb", fileobj=f_out, mtime=mtime) as gz_out:
            with tarfile.open(mode="w", fileobj=gz_out, format=tarfile.GNU_FORMAT) as tar:
                for rel_p, full_p, is_dir in all_items:
                    ti = tar.gettarinfo(full_p, arcname=rel_p)
                    ti.uid = 0
                    ti.gid = 0
                    ti.uname = "root"
                    ti.gname = "root"
                    ti.mtime = mtime
                    if is_dir:
                        ti.mode = 0o755
                        tar.addfile(ti)
                    elif ti.issym():
                        tar.addfile(ti)
                    else:
                        if rel_p.startswith("bin/") or rel_p.startswith("lib/"):
                            ti.mode = 0o755
                        else:
                            ti.mode = 0o644
                        with open(full_p, "rb") as f_in:
                            tar.addfile(ti, f_in)


def main(argv=None):
    parser = argparse.ArgumentParser(description="ChakraCore Release Packaging Tool")
    parser.add_argument("--prefix", default=None,
                        help="Input install prefix (e.g. out/release-shared or zig-out)")
    parser.add_argument("--profile", default=None,
                        help="Build profile name (recorded in MANIFEST.json and archive name)")
    parser.add_argument("--out-dir", default=None,
                        help="Output directory (default: out/package[/<profile>])")
    parser.add_argument("--version", default=None,
                        help="Package version string (default: CHAKRA_CORE_VERSION or git describe)")
    parser.add_argument("--target", default=None,
                        help="Package target platform (default: auto-detected, e.g. darwin-arm64)")
    parser.add_argument("--icu-mode", default="auto", choices=("auto", "bundled", "external"),
                        help="ICU packaging mode (default: auto - bundles Homebrew/local ICU on macOS)")
    parser.add_argument("--repo-root", default=None,
                        help="ChakraCore repository root")
    parser.add_argument("--stage-only", action="store_true",
                        help="Only create the staging directory, skip tarball generation")
    parser.add_argument("--keep-staging", action="store_true", default=True,
                        help="Keep staging directory after creating tarball (default: True)")

    args = parser.parse_args(argv)

    repo_root = os.path.abspath(args.repo_root) if args.repo_root else get_repo_root()
    target = args.target if args.target else get_default_target()
    version = args.version if args.version else get_default_version(repo_root)

    profile = args.profile.strip() if args.profile is not None else None
    if args.profile is not None and not profile:
        print("Error: --profile must not be empty", file=sys.stderr)
        return 2

    # Strict prefix resolution: never silently fall back across profiles.
    if args.prefix:
        prefix = os.path.abspath(args.prefix)
    elif profile:
        rel_prefix = "zig-out" if profile == "test" else os.path.join("out", profile)
        prefix = os.path.join(repo_root, rel_prefix)
    else:
        print("Error: --prefix is required (or pass --profile); cross-prefix fallbacks are disabled",
              file=sys.stderr)
        return 2

    if args.out_dir:
        out_dir = os.path.abspath(args.out_dir)
    elif profile:
        out_dir = os.path.join(repo_root, "out", "package", profile)
    else:
        out_dir = os.path.join(repo_root, "out", "package")

    pkg_name = (
        "chakracore-{}-{}-{}".format(version, profile, target)
        if profile
        else "chakracore-{}-{}".format(version, target)
    )
    staging_dir = os.path.join(out_dir, pkg_name)
    if os.path.isdir(staging_dir):
        shutil.rmtree(staging_dir)

    print("Staging ChakraCore package '{}' from prefix '{}'...".format(pkg_name, prefix))
    try:
        manifest = stage_package(
            prefix,
            staging_dir,
            repo_root,
            version,
            target,
            profile=profile,
            icu_mode=args.icu_mode,
        )
    except RuntimeError as e:
        if os.path.isdir(staging_dir):
            shutil.rmtree(staging_dir, ignore_errors=True)
        print("Error: {}".format(e), file=sys.stderr)
        return 1

    print("Staged {} files in MANIFEST.json (icu_mode={})".format(
        len(manifest["files"]), manifest.get("icu_mode", "unknown")))

    tarball_name = "{}.tar.gz".format(pkg_name)
    tarball_path = os.path.join(out_dir, tarball_name)

    if not args.stage_only:
        print("Generating deterministic tarball: {}".format(tarball_path))
        create_deterministic_tarball(staging_dir, tarball_path)
        tb_size = os.path.getsize(tarball_path)
        tb_sha = compute_sha256(tarball_path)
        print("Package created successfully: {} ({} bytes, sha256: {})".format(tarball_name, tb_size, tb_sha))
        if not args.keep_staging:
            shutil.rmtree(staging_dir)

    return 0


if __name__ == "__main__":
    sys.exit(main())
