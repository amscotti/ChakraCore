#!/usr/bin/env python3
#-------------------------------------------------------------------------------------------------------
# Copyright (c) ChakraCore Project Contributors. All rights reserved.
# Licensed under the MIT license. See LICENSE.txt file in the project root for full license information.
#-------------------------------------------------------------------------------------------------------
"""Release packaging tool for ChakraCore.

Assembles binaries, shared libraries, headers, notices, and MANIFEST.json into a
standard staging layout and generates a deterministic release tarball:
chakracore-<version>-<target>.tar.gz
"""

from __future__ import print_function

import argparse
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


def stage_package(prefix, staging_dir, repo_root, version, target):
    # Staging structure
    bin_dir = os.path.join(staging_dir, "bin")
    lib_dir = os.path.join(staging_dir, "lib")
    include_dir = os.path.join(staging_dir, "include")
    include_nodeapi_dir = os.path.join(include_dir, "nodeapi")

    for d in (bin_dir, lib_dir, include_dir, include_nodeapi_dir):
        if not os.path.isdir(d):
            os.makedirs(d)

    is_darwin = sys.platform == "darwin"
    core_dylib_name = "libChakraCore.dylib" if is_darwin else "libChakraCore.so"
    nodeapi_dylib_name = "libChakraNodeApi.dylib" if is_darwin else "libChakraNodeApi.so"

    bin_search_paths = [
        os.path.join(prefix, "bin"),
        os.path.join(prefix),
        os.path.join(repo_root, "zig-out", "bin"),
        os.path.join(repo_root, "out", "release-shared", "bin"),
        os.path.join(repo_root, "out", "test", "bin"),
    ]

    lib_search_paths = [
        os.path.join(prefix, "lib"),
        os.path.join(prefix, "bin"),
        os.path.join(prefix),
        os.path.join(repo_root, "zig-out", "lib"),
        os.path.join(repo_root, "zig-out", "bin"),
        os.path.join(repo_root, "out", "release-shared", "lib"),
        os.path.join(repo_root, "out", "release-shared", "bin"),
        os.path.join(repo_root, "out", "test", "lib"),
        os.path.join(repo_root, "out", "test", "bin"),
    ]

    # Stage bin/ch
    ch_src = find_file(bin_search_paths, "ch")
    if not ch_src:
        raise RuntimeError("Could not find 'ch' executable in search paths: {}".format(bin_search_paths))
    copy_with_mode(ch_src, os.path.join(bin_dir, "ch"), 0o755)

    # Stage bin/ch-nodeapi
    ch_nodeapi_src = find_file(bin_search_paths, "ch-nodeapi")
    if not ch_nodeapi_src:
        raise RuntimeError("Could not find 'ch-nodeapi' executable in search paths: {}".format(bin_search_paths))
    copy_with_mode(ch_nodeapi_src, os.path.join(bin_dir, "ch-nodeapi"), 0o755)

    # Stage lib/libChakraCore.*
    core_lib_src = find_file(lib_search_paths, core_dylib_name)
    if not core_lib_src:
        raise RuntimeError("Could not find '{}' in search paths: {}".format(core_dylib_name, lib_search_paths))
    copy_with_mode(core_lib_src, os.path.join(lib_dir, core_dylib_name), 0o755)

    # Stage lib/libChakraNodeApi.*
    nodeapi_lib_src = find_file(lib_search_paths, nodeapi_dylib_name)
    if not nodeapi_lib_src:
        raise RuntimeError("Could not find '{}' in search paths: {}".format(nodeapi_dylib_name, lib_search_paths))
    copy_with_mode(nodeapi_lib_src, os.path.join(lib_dir, nodeapi_dylib_name), 0o755)

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

    # Stage include headers:
    # 1) ChakraCore.h & ChakraCommon.h
    header_search_dirs = [
        os.path.join(prefix, "include"),
        os.path.join(repo_root, "lib", "Jsrt"),
    ]
    for hname in ("ChakraCore.h", "ChakraCommon.h"):
        h_src = find_file(header_search_dirs, hname)
        if not h_src:
            raise RuntimeError("Could not find '{}' in header search paths: {}".format(hname, header_search_dirs))
        copy_with_mode(h_src, os.path.join(include_dir, hname), 0o644)

    # 2) include/nodeapi/*.h
    nodeapi_header_dirs = [
        os.path.join(prefix, "include", "nodeapi"),
        os.path.join(repo_root, "include", "nodeapi"),
    ]
    nodeapi_hdrs_found = 0
    for nh_dir in nodeapi_header_dirs:
        if os.path.isdir(nh_dir):
            for f in os.listdir(nh_dir):
                if f.endswith(".h"):
                    src_f = os.path.join(nh_dir, f)
                    if os.path.isfile(src_f):
                        copy_with_mode(src_f, os.path.join(include_nodeapi_dir, f), 0o644)
                        nodeapi_hdrs_found += 1
            if nodeapi_hdrs_found > 0:
                break
    if nodeapi_hdrs_found == 0:
        raise RuntimeError("Could not find any Node-API headers (*.h) in {}".format(nodeapi_header_dirs))

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
        "files": manifest_records,
    }

    manifest_path = os.path.join(staging_dir, "MANIFEST.json")
    with open(manifest_path, "w", encoding="utf-8") as f:
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
    parser.add_argument("--out-dir", default=None,
                        help="Output directory (default: out/package)")
    parser.add_argument("--version", default=None,
                        help="Package version string (default: CHAKRA_CORE_VERSION or git describe)")
    parser.add_argument("--target", default=None,
                        help="Package target platform (default: auto-detected, e.g. darwin-arm64)")
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

    # Prefix resolution
    prefix = args.prefix
    if not prefix:
        candidates = [
            os.path.join(repo_root, "out", "release-shared"),
            os.path.join(repo_root, "zig-out"),
            os.path.join(repo_root, "out", "test"),
        ]
        for c in candidates:
            if os.path.isdir(c) and (os.path.isfile(os.path.join(c, "bin", "ch")) or os.path.isfile(os.path.join(c, "ch"))):
                prefix = c
                break
        if not prefix:
            prefix = os.path.join(repo_root, "out", "release-shared")
    else:
        prefix = os.path.abspath(prefix)

    out_dir = os.path.abspath(args.out_dir) if args.out_dir else os.path.join(repo_root, "out", "package")
    if not os.path.isdir(out_dir):
        os.makedirs(out_dir)

    pkg_name = "chakracore-{}-{}".format(version, target)
    staging_dir = os.path.join(out_dir, pkg_name)
    if os.path.isdir(staging_dir):
        shutil.rmtree(staging_dir)
    os.makedirs(staging_dir)

    print("Staging ChakraCore package '{}' from prefix '{}'...".format(pkg_name, prefix))
    manifest = stage_package(prefix, staging_dir, repo_root, version, target)
    print("Staged {} files in MANIFEST.json".format(len(manifest["files"])))

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
