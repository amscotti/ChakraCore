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
import stat
import sys
import tarfile
import tempfile


def compute_sha256(file_path):
    h = hashlib.sha256()
    with open(file_path, "rb") as f:
        while True:
            chunk = f.read(65536)
            if not chunk:
                break
            h.update(chunk)
    return h.hexdigest()


def verify_extracted_dir(target_dir):
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

        # Verify permissions: binaries in bin/ and libraries in lib/ require 0755
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

    return (len(errors) == 0), errors


def find_tarball_in_dir(d):
    try:
        # Prefer newest matching chakracore-*.tar.gz
        candidates = []
        for f in os.listdir(d):
            if f.endswith(".tar.gz") and (f.startswith("chakracore-") or "chakracore" in f):
                full = os.path.join(d, f)
                if os.path.isfile(full):
                    candidates.append((os.path.getmtime(full), full))
        if candidates:
            candidates.sort(reverse=True)
            return candidates[0][1]
    except Exception:
        pass
    return None


def verify_package(source_path):
    source_path = os.path.abspath(source_path)
    if not os.path.exists(source_path):
        return False, ["Source path does not exist: {}".format(source_path)]

    if os.path.isdir(source_path):
        # If the directory contains a tarball, verify the tarball
        tb = find_tarball_in_dir(source_path)
        if tb and not os.path.isfile(os.path.join(source_path, "MANIFEST.json")):
            return verify_package(tb)
        print("Verifying package directory: {}".format(source_path))
        return verify_extracted_dir(source_path)

    if os.path.isfile(source_path):
        print("Verifying package tarball: {}".format(source_path))
        with tempfile.TemporaryDirectory(prefix="chakra-verify-") as tmpdir:
            try:
                with tarfile.open(source_path, "r:*") as tar:
                    if hasattr(tarfile, "data_filter"):
                        tar.extractall(path=tmpdir, filter="data")
                    else:
                        tar.extractall(path=tmpdir)
            except Exception as e:
                return False, ["Failed to extract tarball '{}': {}".format(source_path, e)]
            return verify_extracted_dir(tmpdir)

    return False, ["Unknown source path type: {}".format(source_path)]


def main(argv=None):
    parser = argparse.ArgumentParser(description="ChakraCore Package Verification Tool")
    parser.add_argument("--tarball", default=None,
                        help="Path to release tarball (.tar.gz)")
    parser.add_argument("--dir", default=None,
                        help="Path to extracted package directory")
    parser.add_argument("target", nargs="?", default=None,
                        help="Path to tarball or directory to verify")

    args = parser.parse_args(argv)

    target_path = args.tarball or args.dir or args.target
    if not target_path:
        parser.print_help()
        sys.exit(1)

    ok, errors = verify_package(target_path)
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
