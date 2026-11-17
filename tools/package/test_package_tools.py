#!/usr/bin/env python3
#-------------------------------------------------------------------------------------------------------
# Copyright (c) ChakraCore Project Contributors. All rights reserved.
# Licensed under the MIT license. See LICENSE.txt file in the project root for full license information.
#-------------------------------------------------------------------------------------------------------
"""Unit tests for ChakraCore packaging tools.

Verifies:
- Package staging layout and manifest generation
- Deterministic tarball generation (bit-for-bit reproducible output)
- Manifest checksum, size, and permission verification
- Error handling on tampered, missing, unexpected, and invalid permission files
"""

from __future__ import print_function

import hashlib
import json
import os
import shutil
import stat
import sys
import tempfile
import unittest

# Ensure tools/package is on sys.path
THIS_DIR = os.path.dirname(os.path.abspath(__file__))
if THIS_DIR not in sys.path:
    sys.path.insert(0, THIS_DIR)

import build_package
import verify_package
import test_package


def make_dummy_file(path, content=b"dummy content", mode=0o644):
    d = os.path.dirname(path)
    if not os.path.isdir(d):
        os.makedirs(d)
    with open(path, "wb") as f:
        f.write(content)
    os.chmod(path, mode)


class PackagingToolsTest(unittest.TestCase):
    def setUp(self):
        self.tmpdir = tempfile.mkdtemp(prefix="chakra-pkg-test-")
        self.repo_root = os.path.join(self.tmpdir, "repo")
        self.prefix = os.path.join(self.tmpdir, "prefix")
        self.out_dir = os.path.join(self.tmpdir, "out_pkg")

        os.makedirs(self.repo_root)
        os.makedirs(self.prefix)
        os.makedirs(self.out_dir)

        is_darwin = sys.platform == "darwin"
        self.core_lib = "libChakraCore.dylib" if is_darwin else "libChakraCore.so"
        self.nodeapi_lib = "libChakraNodeApi.dylib" if is_darwin else "libChakraNodeApi.so"

        # Populate fake repo root docs and headers
        make_dummy_file(os.path.join(self.repo_root, "LICENSE.txt"), b"MIT License\n")
        make_dummy_file(os.path.join(self.repo_root, "THIRD-PARTY-NOTICES.txt"), b"Third Party Notices\n")
        make_dummy_file(os.path.join(self.repo_root, "README.md"), b"# ChakraCore\n")

        # Fake ChakraCore headers
        make_dummy_file(os.path.join(self.repo_root, "lib", "Jsrt", "ChakraCore.h"), b"// ChakraCore.h\n")
        make_dummy_file(os.path.join(self.repo_root, "lib", "Jsrt", "ChakraCommon.h"), b"// ChakraCommon.h\n")
        make_dummy_file(os.path.join(self.repo_root, "lib", "Common", "ChakraCoreVersion.h"),
                        b"#define CHAKRA_CORE_MAJOR_VERSION 1\n#define CHAKRA_CORE_MINOR_VERSION 13\n#define CHAKRA_CORE_PATCH_VERSION 0\n")

        # Fake Node-API headers
        make_dummy_file(os.path.join(self.repo_root, "include", "nodeapi", "js_native_api.h"), b"// js_native_api.h\n")
        make_dummy_file(os.path.join(self.repo_root, "include", "nodeapi", "node_api.h"), b"// node_api.h\n")

        # Populate fake prefix binaries and libraries
        fake_ch_script = (
            b"#!/bin/sh\n"
            b"if [ \"$1\" = \"--version\" ]; then echo 'ch version 1.13.0'; exit 0; fi\n"
            b"if [ \"$1\" = \"-e\" ]; then echo 'package smoke'; exit 0; fi\n"
            b"exit 0\n"
        )
        fake_ch_nodeapi_script = (
            b"#!/bin/sh\n"
            b"if [ \"$1\" = \"--help\" ]; then echo 'usage: ch-nodeapi'; exit 0; fi\n"
            b"echo 'hello: world'\n"
            b"exit 0\n"
        )
        make_dummy_file(os.path.join(self.prefix, "bin", "ch"), fake_ch_script, 0o755)
        make_dummy_file(os.path.join(self.prefix, "bin", "ch-nodeapi"), fake_ch_nodeapi_script, 0o755)
        make_dummy_file(os.path.join(self.prefix, "lib", self.core_lib), b"\x7fELFfake_or_macho_core", 0o755)
        make_dummy_file(os.path.join(self.prefix, "lib", self.nodeapi_lib), b"\x7fELFfake_or_macho_nodeapi", 0o755)

    def tearDown(self):
        shutil.rmtree(self.tmpdir)

    def test_version_and_target_detection(self):
        target = build_package.get_default_target()
        self.assertIn("-", target)
        self.assertTrue(target.startswith("darwin-") or target.startswith("linux-") or target.startswith("windows-"))

        version = build_package.get_default_version(self.repo_root)
        self.assertEqual(version, "1.13.0")

    def test_package_staging_and_manifest(self):
        staging_dir = os.path.join(self.out_dir, "chakracore-1.13.0-test")
        manifest = build_package.stage_package(
            self.prefix, staging_dir, self.repo_root, "1.13.0", "test-plat"
        )

        self.assertEqual(manifest["version"], "1.13.0")
        self.assertEqual(manifest["target"], "test-plat")
        self.assertIsInstance(manifest["files"], list)
        self.assertGreater(len(manifest["files"]), 5)

        # Check files on disk
        manifest_path = os.path.join(staging_dir, "MANIFEST.json")
        self.assertTrue(os.path.isfile(manifest_path))

        # Check binary modes
        for b in ("bin/ch", "bin/ch-nodeapi"):
            p = os.path.join(staging_dir, b)
            self.assertTrue(os.path.isfile(p))
            st = os.stat(p)
            self.assertTrue(st.st_mode & stat.S_IXUSR)
            self.assertTrue(st.st_mode & stat.S_IXGRP)
            self.assertTrue(st.st_mode & stat.S_IXOTH)

        # Check libraries exist with 0755
        for lib in (self.core_lib, self.nodeapi_lib):
            p = os.path.join(staging_dir, "lib", lib)
            self.assertTrue(os.path.isfile(p))
            st = os.stat(p)
            self.assertTrue(st.st_mode & stat.S_IXUSR)

        # Check docs exist
        for doc in ("LICENSE.txt", "THIRD-PARTY-NOTICES.txt", "README.md"):
            self.assertTrue(os.path.isfile(os.path.join(staging_dir, doc)))

        # Verify package directory passes verification
        ok, errs = verify_package.verify_extracted_dir(staging_dir)
        self.assertTrue(ok, "Expected verify to pass, got errors: {}".format(errs))
        self.assertEqual(len(errs), 0)

    def test_deterministic_tarball_reproducibility(self):
        staging_dir = os.path.join(self.out_dir, "chakracore-1.13.0-test")
        build_package.stage_package(self.prefix, staging_dir, self.repo_root, "1.13.0", "test-plat")

        tb1 = os.path.join(self.out_dir, "pkg1.tar.gz")
        tb2 = os.path.join(self.out_dir, "pkg2.tar.gz")

        build_package.create_deterministic_tarball(staging_dir, tb1, mtime=1700000000)
        build_package.create_deterministic_tarball(staging_dir, tb2, mtime=1700000000)

        sha1 = build_package.compute_sha256(tb1)
        sha2 = build_package.compute_sha256(tb2)
        self.assertEqual(sha1, sha2, "Tarballs generated from identical staging must be byte-identical")

        # Verify the tarball itself passes verify_package
        ok, errs = verify_package.verify_package(tb1)
        self.assertTrue(ok, "Expected tarball verify to pass, got errors: {}".format(errs))

    def test_verify_detects_checksum_tampering(self):
        staging_dir = os.path.join(self.out_dir, "chakracore-tampered")
        build_package.stage_package(self.prefix, staging_dir, self.repo_root, "1.13.0", "test-plat")

        # Tamper with README.md
        readme = os.path.join(staging_dir, "README.md")
        with open(readme, "ab") as f:
            f.write(b"tampered byte\n")

        ok, errs = verify_package.verify_extracted_dir(staging_dir)
        self.assertFalse(ok)
        self.assertTrue(any("mismatch for 'README.md'" in e for e in errs),
                        "Expected SHA-256 or size mismatch error for README.md in: {}".format(errs))

    def test_verify_detects_missing_file(self):
        staging_dir = os.path.join(self.out_dir, "chakracore-missing")
        build_package.stage_package(self.prefix, staging_dir, self.repo_root, "1.13.0", "test-plat")

        # Remove bin/ch
        ch_file = os.path.join(staging_dir, "bin", "ch")
        os.remove(ch_file)

        ok, errs = verify_package.verify_extracted_dir(staging_dir)
        self.assertFalse(ok)
        self.assertTrue(any("Missing expected file: bin/ch" in e for e in errs),
                        "Expected missing file error for bin/ch in: {}".format(errs))

    def test_verify_detects_unexpected_file(self):
        staging_dir = os.path.join(self.out_dir, "chakracore-unexpected")
        build_package.stage_package(self.prefix, staging_dir, self.repo_root, "1.13.0", "test-plat")

        # Add unmanifested file
        make_dummy_file(os.path.join(staging_dir, "unmanifested.txt"), b"surplus")

        ok, errs = verify_package.verify_extracted_dir(staging_dir)
        self.assertFalse(ok)
        self.assertTrue(any("Unexpected file found in package: unmanifested.txt" in e for e in errs),
                        "Expected unexpected file error for unmanifested.txt in: {}".format(errs))

    def test_verify_detects_permissions_error(self):
        staging_dir = os.path.join(self.out_dir, "chakracore-perms")
        build_package.stage_package(self.prefix, staging_dir, self.repo_root, "1.13.0", "test-plat")

        # Drop executable permission on bin/ch
        ch_file = os.path.join(staging_dir, "bin", "ch")
        os.chmod(ch_file, 0o644)

        ok, errs = verify_package.verify_extracted_dir(staging_dir)
        self.assertFalse(ok)
        self.assertTrue(any("missing" in e and "executable permission" in e for e in errs),
                        "Expected permission error for bin/ch in: {}".format(errs))

    def test_cli_build_and_verify_roundtrip(self):
        out_pkg = os.path.join(self.out_dir, "cli_test")
        rc = build_package.main([
            "--prefix", self.prefix,
            "--out-dir", out_pkg,
            "--version", "2.0.0",
            "--target", "custom-target",
            "--repo-root", self.repo_root,
        ])
        self.assertEqual(rc, 0)

        tarball = os.path.join(out_pkg, "chakracore-2.0.0-custom-target.tar.gz")
        self.assertTrue(os.path.isfile(tarball))

        v_rc = verify_package.main(["--tarball", tarball])
        self.assertEqual(v_rc, 0)

    def test_test_package_smoke_runner(self):
        staging_dir = os.path.join(self.out_dir, "chakracore-smoke-test")
        build_package.stage_package(self.prefix, staging_dir, self.repo_root, "1.13.0", "test-plat")
        tb = os.path.join(self.out_dir, "smoke.tar.gz")
        build_package.create_deterministic_tarball(staging_dir, tb)

        # Run smoke tests via test_package.main
        rc = test_package.main(["--tarball", tb, "--repo-root", self.repo_root])
        self.assertEqual(rc, 0)


if __name__ == "__main__":
    unittest.main()
