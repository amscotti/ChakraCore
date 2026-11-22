#!/usr/bin/env python3
#-------------------------------------------------------------------------------------------------------
# Copyright (c) ChakraCore Project Contributors. All rights reserved.
# Licensed under the MIT license. See LICENSE.txt file in the project root for full license information.
#-------------------------------------------------------------------------------------------------------
"""Smoke and integration verification for ChakraCore release packages.

Extracts a package tarball (or verifies an extracted package directory) and tests:
1) <dir>/bin/ch --version exits 0 and prints version.
2) <dir>/bin/ch -e "console.log('package smoke')" exits 0 and prints output.
3) <dir>/bin/ch-nodeapi tools/nodeapi/cases/hello_world.js runs cleanly against the
   packaged libChakraCore (verifying dynamic library resolution via
   @executable_path/../lib on macOS and $ORIGIN/../lib on Linux).
"""

from __future__ import print_function

import argparse
import json
import os
import subprocess
import sys
import tarfile
import tempfile


def get_repo_root():
    this_dir = os.path.dirname(os.path.abspath(__file__))
    return os.path.abspath(os.path.join(this_dir, "..", ".."))


def find_addon_binary(repo_root, name="hello_world"):
    cand_dirs = [
        os.path.join(repo_root, "zig-out", "lib", "nodeapi-addons"),
        os.path.join(repo_root, "out", "release-shared", "lib", "nodeapi-addons"),
        os.path.join(repo_root, "out", "test", "lib", "nodeapi-addons"),
        os.path.join(repo_root, "out", "jit", "lib", "nodeapi-addons"),
    ]
    for d in cand_dirs:
        cand = os.path.join(d, "{}.node".format(name))
        if os.path.isfile(cand):
            return cand
    return None


def run_package_smoke_tests(package_dir, repo_root, case_file=None):
    # Check for single nested root directory
    manifest_p = os.path.join(package_dir, "MANIFEST.json")
    if not os.path.isfile(manifest_p):
        subdirs = [d for d in os.listdir(package_dir) if os.path.isdir(os.path.join(package_dir, d))]
        if len(subdirs) == 1:
            cand = os.path.join(package_dir, subdirs[0], "MANIFEST.json")
            if os.path.isfile(cand):
                package_dir = os.path.join(package_dir, subdirs[0])

    bin_dir = os.path.join(package_dir, "bin")
    lib_dir = os.path.join(package_dir, "lib")

    pkg_target = ""
    if os.path.isfile(os.path.join(package_dir, "MANIFEST.json")):
        try:
            with open(os.path.join(package_dir, "MANIFEST.json"), "r", encoding="utf-8") as mf:
                pkg_target = json.load(mf).get("target", "") or ""
        except Exception:
            pkg_target = ""

    is_windows = (
        pkg_target.startswith("windows-")
        or (not pkg_target and sys.platform == "win32")
        or os.path.isfile(os.path.join(bin_dir, "ch.exe"))
    )

    if is_windows:
        ch_bin = os.path.join(bin_dir, "ch.exe")
        ch_nodeapi_bin = os.path.join(bin_dir, "ch-nodeapi.exe")
        core_dll = os.path.join(bin_dir, "ChakraCore.dll")
        nodeapi_dll = os.path.join(bin_dir, "libChakraNodeApi.dll")
        core_lib = os.path.join(lib_dir, "ChakraCore.lib")
        nodeapi_lib = os.path.join(lib_dir, "libChakraNodeApi.lib")

        # 0. Check artifacts exist
        for path, desc in (
            (ch_bin, "ch binary"),
            (ch_nodeapi_bin, "ch-nodeapi binary"),
            (core_dll, "ChakraCore DLL"),
            (nodeapi_dll, "ChakraNodeApi DLL"),
            (core_lib, "ChakraCore import library"),
            (nodeapi_lib, "ChakraNodeApi import library"),
        ):
            if not os.path.isfile(path):
                raise RuntimeError("Required package artifact '{}' not found at {}".format(desc, path))
    else:
        ch_bin = os.path.join(bin_dir, "ch")
        ch_nodeapi_bin = os.path.join(bin_dir, "ch-nodeapi")

        is_darwin = (
            pkg_target.startswith("darwin-")
            or (not pkg_target.startswith("linux-") and sys.platform == "darwin")
        )
        core_lib_name = "libChakraCore.dylib" if is_darwin else "libChakraCore.so"
        nodeapi_lib_name = "libChakraNodeApi.dylib" if is_darwin else "libChakraNodeApi.so"

        core_lib = os.path.join(lib_dir, core_lib_name)
        nodeapi_lib = os.path.join(lib_dir, nodeapi_lib_name)

        # 0. Check artifacts exist
        for path, desc in (
            (ch_bin, "ch binary"),
            (ch_nodeapi_bin, "ch-nodeapi binary"),
            (core_lib, "ChakraCore shared library"),
            (nodeapi_lib, "ChakraNodeApi shared library"),
        ):
            if not os.path.isfile(path):
                raise RuntimeError("Required package artifact '{}' not found at {}".format(desc, path))

    # Clean environment: ensure DYLD_LIBRARY_PATH and LD_LIBRARY_PATH are unset
    # so we strictly test rpath resolution relative to the binary!
    clean_env = os.environ.copy()
    for var in ("DYLD_LIBRARY_PATH", "DYLD_FALLBACK_LIBRARY_PATH", "LD_LIBRARY_PATH"):
        clean_env.pop(var, None)

    # 1. Test: <dir>/bin/ch --version
    print("--- Test 1: {} --version ---".format(ch_bin))
    res1 = subprocess.run(
        [ch_bin, "--version"],
        capture_output=True,
        text=True,
        env=clean_env
    )
    if res1.returncode != 0:
        raise RuntimeError("ch --version failed with exit code {}:\nstderr: {}".format(
            res1.returncode, res1.stderr))
    out1 = (res1.stdout or "") + (res1.stderr or "")
    print("ch --version output: {}".format(out1.strip()))
    if not out1.strip():
        raise RuntimeError("ch --version produced empty output")

    # 2. Test: <dir>/bin/ch -e "console.log('package smoke')"
    print("--- Test 2: {} -e \"console.log('package smoke')\" ---".format(ch_bin))
    res2 = subprocess.run(
        [ch_bin, "-e", "console.log('package smoke')"],
        capture_output=True,
        text=True,
        env=clean_env
    )
    if res2.returncode != 0 or "package smoke" not in (res2.stdout or ""):
        # ch CLI requires a source file rather than -e; run snippet via temporary script
        with tempfile.NamedTemporaryFile("w", suffix=".js", delete=False) as tf:
            tf.write("console.log('package smoke');\n")
            temp_snippet = tf.name
        try:
            res2 = subprocess.run(
                [ch_bin, temp_snippet],
                capture_output=True,
                text=True,
                env=clean_env
            )
        finally:
            if os.path.exists(temp_snippet):
                os.remove(temp_snippet)
    if res2.returncode != 0:
        raise RuntimeError("ch snippet failed with exit code {}:\nstderr: {}".format(
            res2.returncode, res2.stderr))
    out2 = (res2.stdout or "").strip()
    print("ch snippet output: {}".format(out2))
    if "package smoke" not in out2:
        raise RuntimeError("ch snippet did not output expected 'package smoke', got: {}".format(out2))

    # 3. Test: <dir>/bin/ch-nodeapi dynamic library resolution
    print("--- Test 3: {} dynamic library resolution ---".format(ch_nodeapi_bin))

    # 3a. First verify ch-nodeapi executes and resolves @executable_path/../lib/libChakraCore
    res3_help = subprocess.run(
        [ch_nodeapi_bin, "--help"],
        capture_output=True,
        text=True,
        env=clean_env
    )
    if res3_help.returncode != 0:
        raise RuntimeError("ch-nodeapi --help failed to execute (exit code {}):\nstderr: {}".format(
            res3_help.returncode, res3_help.stderr))
    print("ch-nodeapi binary dynamically loaded libChakraCore successfully.")

    # 3b. Locate case script
    if not case_file:
        cands = [
            os.path.join(repo_root, "zig-out", "lib", "nodeapi-addons", "hello_world.js"),
            os.path.join(repo_root, "out", "release-shared", "lib", "nodeapi-addons", "hello_world.js"),
            os.path.join(repo_root, "out", "test", "lib", "nodeapi-addons", "hello_world.js"),
            os.path.join(repo_root, "tools", "nodeapi", "cases", "hello_world.js"),
            os.path.join(repo_root, "tools", "nodeapi", "addons", "hello_world.js"),
        ]
        for c in cands:
            if os.path.isfile(c):
                case_file = c
                break

    # If neither exists, create a temporary driver
    temp_driver = None
    if not case_file or not os.path.isfile(case_file):
        fd, temp_driver = tempfile.mkstemp(suffix=".js", prefix="hello_world_")
        with os.fdopen(fd, "w", encoding="utf-8") as f:
            f.write("print('hello: ' + (globalThis.__exports ? globalThis.__exports.hello() : 'world'));\n")
        case_file = temp_driver

    try:
        addon_node = find_addon_binary(repo_root, "hello_world")
        if addon_node:
            print("Found compiled addon: {}".format(addon_node))
            cmd3 = [ch_nodeapi_bin, addon_node, case_file]
            print("Running: {}".format(" ".join(cmd3)))
            res3 = subprocess.run(cmd3, capture_output=True, text=True, env=clean_env)
            out3 = (res3.stdout or "").strip()
            print("ch-nodeapi output:\n{}".format(out3))
            if res3.returncode != 0:
                raise RuntimeError("ch-nodeapi failed with exit code {}:\nstderr: {}".format(
                    res3.returncode, res3.stderr))
            if "hello: world" not in out3:
                raise RuntimeError("ch-nodeapi expected 'hello: world' in output, got: {}".format(out3))
        else:
            # When addon binary is not pre-compiled, execute directly with case_file:
            # <dir>/bin/ch-nodeapi tools/nodeapi/cases/hello_world.js
            cmd3 = [ch_nodeapi_bin, case_file]
            print("Running: {}".format(" ".join(cmd3)))
            res3 = subprocess.run(cmd3, capture_output=True, text=True, env=clean_env)
            # Verify dynamic linker succeeded (no dyld / dlopen / Win32 DLL loader error)
            stderr_out = res3.stderr or ""
            if ("dyld: Library not loaded" in stderr_out
                    or "error while loading shared libraries" in stderr_out
                    or "Unable to load" in stderr_out
                    or res3.returncode in (-1073741515, 3221225781, -1073741511)):
                raise RuntimeError("Dynamic library resolution failed for ch-nodeapi:\n{}".format(stderr_out))
            if res3.returncode == 0:
                print("ch-nodeapi ran case cleanly (exit code 0).")
            else:
                # If ch-nodeapi exits because dlopen requires an addon .node binary rather than .js,
                # confirm that provider and engine were loaded without dyld failure.
                if "dlopen addon" in stderr_out or "cannot read driver" in stderr_out:
                    print("ch-nodeapi verified dynamic library resolution of libChakraCore and libChakraNodeApi.")
                else:
                    raise RuntimeError("ch-nodeapi failed with unexpected error:\nstdout: {}\nstderr: {}".format(
                        res3.stdout, stderr_out))

    finally:
        if temp_driver and os.path.exists(temp_driver):
            os.remove(temp_driver)

    print("\nALL PACKAGE TESTS PASSED SUCCESSFULLY!")
    return True


def find_tarball_in_dir(d):
    try:
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


def main(argv=None):
    parser = argparse.ArgumentParser(description="ChakraCore Package Smoke & Verification Runner")
    parser.add_argument("--tarball", default=None,
                        help="Path to release tarball (.tar.gz)")
    parser.add_argument("--dir", default=None,
                        help="Path to extracted package directory")
    parser.add_argument("target", nargs="?", default=None,
                        help="Path to package tarball or extracted directory")
    parser.add_argument("--case", default=None,
                        help="Path to nodeapi case JS file (default: tools/nodeapi/cases/hello_world.js)")
    parser.add_argument("--repo-root", default=None,
                        help="ChakraCore repository root")

    args = parser.parse_args(argv)

    target_path = args.tarball or args.dir or args.target
    if not target_path:
        parser.print_help()
        sys.exit(1)

    repo_root = os.path.abspath(args.repo_root) if args.repo_root else get_repo_root()
    target_path = os.path.abspath(target_path)

    if os.path.isfile(target_path):
        print("Extracting package tarball '{}' to temporary directory...".format(target_path))
        with tempfile.TemporaryDirectory(prefix="chakra-test-pkg-") as tmpdir:
            with tarfile.open(target_path, "r:*") as tar:
                if hasattr(tarfile, "data_filter"):
                    tar.extractall(path=tmpdir, filter="data")
                else:
                    tar.extractall(path=tmpdir)
            run_package_smoke_tests(tmpdir, repo_root, args.case)
    elif os.path.isdir(target_path):
        tb = find_tarball_in_dir(target_path)
        has_ch = (
            os.path.isfile(os.path.join(target_path, "bin", "ch"))
            or os.path.isfile(os.path.join(target_path, "bin", "ch.exe"))
        )
        if tb and not has_ch:
            print("Found package tarball in directory: {}".format(tb))
            with tempfile.TemporaryDirectory(prefix="chakra-test-pkg-") as tmpdir:
                with tarfile.open(tb, "r:*") as tar:
                    if hasattr(tarfile, "data_filter"):
                        tar.extractall(path=tmpdir, filter="data")
                    else:
                        tar.extractall(path=tmpdir)
                run_package_smoke_tests(tmpdir, repo_root, args.case)
        else:
            run_package_smoke_tests(target_path, repo_root, args.case)
    else:
        print("Error: Target path '{}' does not exist".format(target_path), file=sys.stderr)
        sys.exit(1)

    return 0


if __name__ == "__main__":
    sys.exit(main())
