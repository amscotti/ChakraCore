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
import subprocess
import sys
import tarfile
import tempfile
import unittest
from unittest import mock

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

        # Fake repo-root headers (decoy: build_package must NEVER fall back here)
        make_dummy_file(os.path.join(self.repo_root, "lib", "Jsrt", "ChakraCore.h"), b"// repo ChakraCore.h\n")
        make_dummy_file(os.path.join(self.repo_root, "lib", "Jsrt", "ChakraCommon.h"), b"// repo ChakraCommon.h\n")
        make_dummy_file(os.path.join(self.repo_root, "lib", "Jsrt", "ChakraDebug.h"), b"// repo ChakraDebug.h\n")
        make_dummy_file(os.path.join(self.repo_root, "lib", "Common", "ChakraCoreVersion.h"),
                        b"#define CHAKRA_CORE_MAJOR_VERSION 1\n#define CHAKRA_CORE_MINOR_VERSION 13\n#define CHAKRA_CORE_PATCH_VERSION 0\n")

        # Fake repo-root Node-API headers (decoy: must come from prefix only)
        for nh in ("js_native_api.h", "js_native_api_types.h", "node_api.h", "node_api_types.h"):
            make_dummy_file(os.path.join(self.repo_root, "include", "nodeapi", nh),
                            "// repo {}\n".format(nh).encode("ascii"))

        # Populate prefix headers (all 4 Chakra headers + all 4 Node-API headers)
        for hname in ("ChakraCore.h", "ChakraCommon.h", "ChakraDebug.h", "ChakraCoreVersion.h"):
            make_dummy_file(os.path.join(self.prefix, "include", hname),
                            "// prefix {}\n".format(hname).encode("ascii"))
        for nh in ("js_native_api.h", "js_native_api_types.h", "node_api.h", "node_api_types.h"):
            make_dummy_file(os.path.join(self.prefix, "include", "nodeapi", nh),
                            "// prefix {}\n".format(nh).encode("ascii"))

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

        # Check binary modes (POSIX only; Windows has no exec-bit semantics)
        for b in ("bin/ch", "bin/ch-nodeapi"):
            p = os.path.join(staging_dir, b)
            self.assertTrue(os.path.isfile(p))
            if os.name != "nt":
                st = os.stat(p)
                self.assertTrue(st.st_mode & stat.S_IXUSR)
                self.assertTrue(st.st_mode & stat.S_IXGRP)
                self.assertTrue(st.st_mode & stat.S_IXOTH)

        # Check libraries exist with 0755 (POSIX only)
        for lib in (self.core_lib, self.nodeapi_lib):
            p = os.path.join(staging_dir, "lib", lib)
            self.assertTrue(os.path.isfile(p))
            if os.name != "nt":
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

    @unittest.skipIf(os.name == "nt", "POSIX permission bits; verify skips them on Windows")
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

    @unittest.skipIf(os.name == "nt", "fake binaries are POSIX shell scripts (#!/bin/sh)")
    def test_test_package_smoke_runner(self):
        staging_dir = os.path.join(self.out_dir, "chakracore-smoke-test")
        build_package.stage_package(self.prefix, staging_dir, self.repo_root, "1.13.0", "test-plat")
        tb = os.path.join(self.out_dir, "smoke.tar.gz")
        build_package.create_deterministic_tarball(staging_dir, tb)

        # Run smoke tests via test_package.main
        rc = test_package.main(["--tarball", tb, "--repo-root", self.repo_root])
        self.assertEqual(rc, 0)

    def test_windows_x64_target_autodetect(self):
        with mock.patch.object(build_package.sys, "platform", "win32"), \
                mock.patch.object(build_package.platform, "machine", return_value="AMD64"):
            self.assertEqual(build_package.get_default_target(), "windows-x64")

    def test_windows_x64_staging_manifest_and_verify(self):
        win_prefix = os.path.join(self.tmpdir, "win_prefix")
        make_dummy_file(os.path.join(win_prefix, "bin", "ch.exe"), b"MZch", 0o755)
        make_dummy_file(os.path.join(win_prefix, "bin", "ch-nodeapi.exe"), b"MZchnodeapi", 0o755)
        make_dummy_file(os.path.join(win_prefix, "bin", "ChakraCore.dll"), b"MZcore", 0o755)
        make_dummy_file(os.path.join(win_prefix, "lib", "libChakraNodeApi.dll"), b"MZnodeapi", 0o755)
        make_dummy_file(os.path.join(win_prefix, "lib", "ChakraCore.lib"), b"!<arch>\ncore", 0o755)
        make_dummy_file(os.path.join(win_prefix, "lib", "libChakraNodeApi.lib"), b"!<arch>\nnodeapi", 0o755)
        for hname in ("ChakraCore.h", "ChakraCommon.h", "ChakraDebug.h", "ChakraCoreVersion.h"):
            make_dummy_file(os.path.join(win_prefix, "include", hname),
                            "// win {}\n".format(hname).encode("ascii"))
        for nh in ("js_native_api.h", "js_native_api_types.h", "node_api.h", "node_api_types.h"):
            make_dummy_file(os.path.join(win_prefix, "include", "nodeapi", nh),
                            "// win {}\n".format(nh).encode("ascii"))

        out_pkg = os.path.join(self.out_dir, "win_pkg")
        rc = build_package.main([
            "--prefix", win_prefix,
            "--out-dir", out_pkg,
            "--version", "1.13.0",
            "--target", "windows-x64",
            "--repo-root", self.repo_root,
        ])
        self.assertEqual(rc, 0)

        staging_dir = os.path.join(out_pkg, "chakracore-1.13.0-windows-x64")
        tarball = os.path.join(out_pkg, "chakracore-1.13.0-windows-x64.tar.gz")
        self.assertTrue(os.path.isfile(tarball))

        # DLLs must be regular copied files in bin/, never symlinks.
        for rel in (
            "bin/ch.exe",
            "bin/ch-nodeapi.exe",
            "bin/ChakraCore.dll",
            "bin/libChakraNodeApi.dll",
            "lib/ChakraCore.lib",
            "lib/libChakraNodeApi.lib",
            "include/ChakraCore.h",
            "include/ChakraCommon.h",
            "include/ChakraDebug.h",
            "include/ChakraCoreVersion.h",
            "include/nodeapi/js_native_api.h",
            "include/nodeapi/js_native_api_types.h",
            "include/nodeapi/node_api.h",
            "include/nodeapi/node_api_types.h",
            "LICENSE.txt",
            "THIRD-PARTY-NOTICES.txt",
            "README.md",
            "MANIFEST.json",
        ):
            full = os.path.join(staging_dir, *rel.split("/"))
            self.assertTrue(os.path.isfile(full), "missing staged Windows file: " + rel)
            self.assertFalse(os.path.islink(full), "unexpected symlink on Windows: " + rel)

        ok, errs = verify_package.verify_extracted_dir(staging_dir)
        self.assertTrue(ok, "Windows staging dir verify failed: {}".format(errs))
        ok_tb, errs_tb = verify_package.verify_package(tarball)
        self.assertTrue(ok_tb, "Windows tarball verify failed: {}".format(errs_tb))

        # Deterministic tarball reproducibility for windows-x64.
        tb2 = os.path.join(self.out_dir, "win_pkg2.tar.gz")
        build_package.create_deterministic_tarball(staging_dir, tb2, mtime=1767225600)
        self.assertEqual(
            build_package.compute_sha256(tarball),
            build_package.compute_sha256(tb2),
        )

        # Negative controls on Windows staging.
        os.remove(os.path.join(staging_dir, "bin", "ChakraCore.dll"))
        ok_miss, errs_miss = verify_package.verify_extracted_dir(staging_dir)
        self.assertFalse(ok_miss)
        self.assertTrue(any("bin/ChakraCore.dll" in e for e in errs_miss))

    def test_windows_x64_smoke_runner_mocked(self):
        win_prefix = os.path.join(self.tmpdir, "win_smoke_prefix")
        make_dummy_file(os.path.join(win_prefix, "bin", "ch.exe"), b"MZch", 0o755)
        make_dummy_file(os.path.join(win_prefix, "bin", "ch-nodeapi.exe"), b"MZchnodeapi", 0o755)
        make_dummy_file(os.path.join(win_prefix, "bin", "ChakraCore.dll"), b"MZcore", 0o755)
        make_dummy_file(os.path.join(win_prefix, "bin", "libChakraNodeApi.dll"), b"MZnodeapi", 0o755)
        make_dummy_file(os.path.join(win_prefix, "lib", "ChakraCore.lib"), b"!<arch>\ncore", 0o755)
        make_dummy_file(os.path.join(win_prefix, "lib", "libChakraNodeApi.lib"), b"!<arch>\nnodeapi", 0o755)
        for hname in ("ChakraCore.h", "ChakraCommon.h", "ChakraDebug.h", "ChakraCoreVersion.h"):
            make_dummy_file(os.path.join(win_prefix, "include", hname),
                            "// win {}\n".format(hname).encode("ascii"))
        for nh in ("js_native_api.h", "js_native_api_types.h", "node_api.h", "node_api_types.h"):
            make_dummy_file(os.path.join(win_prefix, "include", "nodeapi", nh),
                            "// win {}\n".format(nh).encode("ascii"))

        staging_dir = os.path.join(self.out_dir, "chakracore-1.13.0-windows-x64-smoke")
        build_package.stage_package(win_prefix, staging_dir, self.repo_root, "1.13.0", "windows-x64")

        def fake_subprocess_run(cmd, **kwargs):
            _ = kwargs
            exe = os.path.basename(cmd[0])
            if exe == "ch.exe":
                if cmd[1:] == ["--version"]:
                    return mock.Mock(returncode=0, stdout="ch.exe version 1.13.0.0-beta\n", stderr="")
                if cmd[1] == "-e":
                    return mock.Mock(returncode=0, stdout="package smoke\n", stderr="")
                if len(cmd) == 2 and cmd[1].endswith(".js"):
                    return mock.Mock(returncode=0, stdout="intl:1.234,50\n", stderr="")
            elif exe == "ch-nodeapi.exe":
                if cmd[1:] == ["--help"]:
                    return mock.Mock(returncode=0, stdout="usage: ch-nodeapi\n", stderr="")
                return mock.Mock(returncode=1, stdout="", stderr="ch-nodeapi: dlopen addon missing\n")
            raise AssertionError("unexpected command: %r" % (cmd,))

        with mock.patch.object(test_package.subprocess, "run", side_effect=fake_subprocess_run):
            self.assertTrue(test_package.run_package_smoke_tests(staging_dir, self.repo_root))

    def test_profile_flag_naming_manifest_and_mismatch(self):
        out_pkg = os.path.join(self.out_dir, "prof_test")
        make_dummy_file(os.path.join(self.prefix, ".profile"), b"jit\n")
        rc = build_package.main([
            "--prefix", self.prefix,
            "--profile", "jit",
            "--out-dir", out_pkg,
            "--version", "1.13.0",
            "--target", "custom-target",
            "--repo-root", self.repo_root,
        ])
        self.assertEqual(rc, 0)

        staging_dir = os.path.join(out_pkg, "chakracore-1.13.0-jit-custom-target")
        tarball = os.path.join(out_pkg, "chakracore-1.13.0-jit-custom-target.tar.gz")
        self.assertTrue(os.path.isdir(staging_dir))
        self.assertTrue(os.path.isfile(tarball))
        with open(os.path.join(staging_dir, "MANIFEST.json"), encoding="utf-8") as mf:
            manifest = json.load(mf)
        self.assertEqual(manifest.get("profile"), "jit")
        self.assertEqual(manifest.get("icu_mode"), "external")

        self.assertEqual(verify_package.main(["--profile", "jit", "--tarball", tarball]), 0)
        self.assertEqual(verify_package.main(["--profile", "test", "--tarball", tarball]), 1)
        self.assertEqual(test_package.main(["--profile", "jit", "--tarball", tarball, "--repo-root", self.repo_root]), 0)
        with self.assertRaises(RuntimeError):
            test_package.run_package_smoke_tests(staging_dir, self.repo_root, expected_profile="test")

        # Prefix .profile mismatch must fail loudly in build_package.
        rc_mismatch = build_package.main([
            "--prefix", self.prefix,
            "--profile", "release-shared",
            "--out-dir", out_pkg,
            "--version", "1.13.0",
            "--target", "custom-target",
            "--repo-root", self.repo_root,
        ])
        self.assertEqual(rc_mismatch, 1)

    def test_no_cross_prefix_fallback_and_missing_files_fail_loudly(self):
        # Populate decoy other prefixes in repo_root that must NEVER be used.
        make_dummy_file(os.path.join(self.repo_root, "zig-out", "bin", "ch"), b"decoy", 0o755)
        make_dummy_file(os.path.join(self.repo_root, "zig-out", "lib", self.nodeapi_lib), b"decoy", 0o755)
        make_dummy_file(os.path.join(self.repo_root, "out", "release-shared", "lib", self.nodeapi_lib), b"decoy", 0o755)

        # Missing both --prefix and --profile fails loudly.
        self.assertEqual(build_package.main(["--repo-root", self.repo_root]), 2)

        # Remove Node-API lib + ChakraDebug.h + node_api_types.h from self.prefix;
        # decoys in repo_root/out/release-shared, lib/Jsrt, include/nodeapi must NOT rescue it.
        os.remove(os.path.join(self.prefix, "lib", self.nodeapi_lib))
        os.remove(os.path.join(self.prefix, "include", "ChakraDebug.h"))
        os.remove(os.path.join(self.prefix, "include", "nodeapi", "node_api_types.h"))

        with self.assertRaises(RuntimeError) as ctx:
            build_package.stage_package(
                self.prefix, os.path.join(self.out_dir, "fail-staging"),
                self.repo_root, "1.13.0", "test-plat",
            )
        msg = str(ctx.exception)
        self.assertIn(self.nodeapi_lib, msg)
        self.assertIn("include/ChakraDebug.h", msg)
        self.assertIn("include/nodeapi/node_api_types.h", msg)

        self.assertEqual(build_package.main([
            "--prefix", self.prefix,
            "--out-dir", os.path.join(self.out_dir, "fail_pkg"),
            "--repo-root", self.repo_root,
        ]), 1)

    @unittest.skipUnless(
        sys.platform == "darwin"
        and os.path.isfile("/opt/homebrew/opt/icu4c/lib/libicui18n.dylib")
        and shutil.which("clang"),
        "requires macOS with Homebrew ICU and clang",
    )
    def test_macos_real_macho_icu_bundling_and_verification(self):
        macho_prefix = os.path.join(self.tmpdir, "macho_prefix")
        src_dir = os.path.join(self.tmpdir, "macho_src")
        os.makedirs(os.path.join(macho_prefix, "bin"))
        os.makedirs(os.path.join(macho_prefix, "lib"))
        os.makedirs(os.path.join(macho_prefix, "include", "nodeapi"))
        os.makedirs(src_dir)

        for hname in ("ChakraCore.h", "ChakraCommon.h", "ChakraDebug.h", "ChakraCoreVersion.h"):
            make_dummy_file(os.path.join(macho_prefix, "include", hname),
                            "// macho {}\n".format(hname).encode("ascii"))
        for nh in ("js_native_api.h", "js_native_api_types.h", "node_api.h", "node_api_types.h"):
            make_dummy_file(os.path.join(macho_prefix, "include", "nodeapi", nh),
                            "// macho {}\n".format(nh).encode("ascii"))
        make_dummy_file(os.path.join(macho_prefix, ".profile"), b"jit\n")

        core_c = os.path.join(src_dir, "core.c")
        with open(core_c, "w", encoding="utf-8") as f:
            f.write(
                "#include <unicode/unum.h>\n"
                "#include <stdio.h>\n"
                "int chakra_icu_probe(void) {\n"
                "    UErrorCode ec = U_ZERO_ERROR;\n"
                "    UNumberFormat* nf = unum_open(UNUM_CURRENCY, NULL, 0, \"de_DE\", NULL, &ec);\n"
                "    if (nf) unum_close(nf);\n"
                "    return U_SUCCESS(ec) ? 0 : 1;\n"
                "}\n"
            )
        napi_c = os.path.join(src_dir, "nodeapi.c")
        with open(napi_c, "w", encoding="utf-8") as f:
            f.write("extern int chakra_icu_probe(void);\nint napi_probe(void) { return chakra_icu_probe(); }\n")
        ch_c = os.path.join(src_dir, "ch.c")
        with open(ch_c, "w", encoding="utf-8") as f:
            f.write(
                "#include <dlfcn.h>\n"
                "#include <libgen.h>\n"
                "#include <mach-o/dyld.h>\n"
                "#include <stdio.h>\n"
                "#include <string.h>\n"
                "#include <unicode/uversion.h>\n"
                "int main(int argc, char** argv) {\n"
                "    UVersionInfo v;\n"
                "    u_getVersion(v);\n"
                "    if (argc > 1 && strcmp(argv[1], \"--version\") == 0) {\n"
                "        printf(\"ch icu %d.%d\\n\", v[0], v[1]);\n"
                "        return 0;\n"
                "    }\n"
                "    char buf[4096]; uint32_t sz = sizeof(buf);\n"
                "    _NSGetExecutablePath(buf, &sz);\n"
                "    char libp[4096];\n"
                "    snprintf(libp, sizeof(libp), \"%s/libChakraCore.dylib\", dirname(buf));\n"
                "    void* h = dlopen(libp, RTLD_NOW);\n"
                "    if (!h) { fprintf(stderr, \"dlopen: %s\\n\", dlerror()); return 2; }\n"
                "    int (*fn)(void) = (int(*)(void))dlsym(h, \"chakra_icu_probe\");\n"
                "    return fn ? fn() : 3;\n"
                "}\n"
            )
        chn_c = os.path.join(src_dir, "chn.c")
        with open(chn_c, "w", encoding="utf-8") as f:
            f.write("extern int napi_probe(void);\nint main(void) { return napi_probe(); }\n")

        icu_inc = "/opt/homebrew/opt/icu4c/include"
        icu_lib = "/opt/homebrew/opt/icu4c/lib"
        core_dy = os.path.join(macho_prefix, "lib", "libChakraCore.dylib")
        napi_dy = os.path.join(macho_prefix, "lib", "libChakraNodeApi.dylib")
        ch_bin = os.path.join(macho_prefix, "bin", "ch")
        chn_bin = os.path.join(macho_prefix, "bin", "ch-nodeapi")

        subprocess.run([
            "clang", "-shared", "-fPIC", "-I" + icu_inc, "-L" + icu_lib,
            "-licuuc", "-licui18n", "-licudata",
            "-Wl,-install_name,@rpath/libChakraCore.dylib", "-Wl,-headerpad_max_install_names",
            core_c, "-o", core_dy,
        ], check=True)
        subprocess.run([
            "clang", "-shared", "-fPIC", core_dy,
            "-Wl,-install_name,@rpath/libChakraNodeApi.dylib", "-Wl,-headerpad_max_install_names",
            napi_c, "-o", napi_dy,
        ], check=True)
        subprocess.run([
            "clang", "-I" + icu_inc, "-L" + icu_lib,
            "-licuuc", "-licui18n", "-licudata",
            "-Wl,-headerpad_max_install_names",
            ch_c, "-o", ch_bin,
        ], check=True)
        subprocess.run([
            "clang", napi_dy, core_dy,
            "-Wl,-rpath,@executable_path/../lib", "-Wl,-headerpad_max_install_names",
            chn_c, "-o", chn_bin,
        ], check=True)

        pkg_out = os.path.join(self.out_dir, "macho_bundled")
        rc = build_package.main([
            "--prefix", macho_prefix,
            "--profile", "jit",
            "--out-dir", pkg_out,
            "--version", "1.13.0",
            "--target", "darwin-arm64",
            "--repo-root", self.repo_root,
        ])
        self.assertEqual(rc, 0)

        staging = os.path.join(pkg_out, "chakracore-1.13.0-jit-darwin-arm64")
        tarball = os.path.join(pkg_out, "chakracore-1.13.0-jit-darwin-arm64.tar.gz")
        with open(os.path.join(staging, "MANIFEST.json"), encoding="utf-8") as mf:
            manifest = json.load(mf)
        self.assertEqual(manifest.get("icu_mode"), "bundled")
        self.assertEqual(manifest.get("profile"), "jit")
        icu_libs = manifest.get("icu_libraries", [])
        self.assertTrue(any(x.startswith("libicudata.") for x in icu_libs), icu_libs)
        self.assertTrue(any(x.startswith("libicuuc.") for x in icu_libs), icu_libs)
        self.assertTrue(any(x.startswith("libicui18n.") for x in icu_libs), icu_libs)

        ok, errs = verify_package.verify_package(tarball, expected_profile="jit")
        self.assertTrue(ok, "Bundled Mach-O package verify failed: {}".format(errs))

        # Bit-for-bit reproducibility across independent Mach-O ICU bundling + codesign runs.
        pkg_out2 = os.path.join(self.out_dir, "macho_bundled_2")
        rc2 = build_package.main([
            "--prefix", macho_prefix,
            "--profile", "jit",
            "--out-dir", pkg_out2,
            "--version", "1.13.0",
            "--target", "darwin-arm64",
            "--repo-root", self.repo_root,
        ])
        self.assertEqual(rc2, 0)
        tarball2 = os.path.join(pkg_out2, "chakracore-1.13.0-jit-darwin-arm64.tar.gz")
        self.assertEqual(
            build_package.compute_sha256(tarball),
            build_package.compute_sha256(tarball2),
            "Bundled Mach-O tarball must be bit-for-bit deterministic across runs",
        )

        # Extract tarball cleanly and execute packaged binaries with DYLD_* unset.
        with tempfile.TemporaryDirectory(prefix="macho-extract-") as ex_dir:
            with tarfile.open(tarball, "r:*") as tf:
                if hasattr(tarfile, "data_filter"):
                    tf.extractall(path=ex_dir, filter="data")
                else:
                    tf.extractall(path=ex_dir)
            clean_env = os.environ.copy()
            for k in ("DYLD_LIBRARY_PATH", "DYLD_FALLBACK_LIBRARY_PATH", "LD_LIBRARY_PATH"):
                clean_env.pop(k, None)
            r_ver = subprocess.run([os.path.join(ex_dir, "bin", "ch"), "--version"],
                                   env=clean_env, capture_output=True, text=True)
            self.assertEqual(r_ver.returncode, 0, r_ver.stderr)
            self.assertIn("ch icu ", r_ver.stdout)
            r_dl = subprocess.run([os.path.join(ex_dir, "bin", "ch")],
                                  env=clean_env, capture_output=True, text=True)
            self.assertEqual(r_dl.returncode, 0, r_dl.stderr)
            r_na = subprocess.run([os.path.join(ex_dir, "bin", "ch-nodeapi")],
                                  env=clean_env, capture_output=True, text=True)
            self.assertEqual(r_na.returncode, 0, r_na.stderr)
            test_package.check_macos_no_external_homebrew(
                ex_dir,
                [os.path.join(ex_dir, "bin", "ch"), os.path.join(ex_dir, "bin", "ch-nodeapi")],
                [os.path.join(ex_dir, "lib", "libChakraCore.dylib"),
                 os.path.join(ex_dir, "lib", "libChakraNodeApi.dylib")],
            )

        # Negative control: external ICU package tampered to claim icu_mode='bundled' fails verify_package.
        ext_out = os.path.join(self.out_dir, "macho_external")
        rc_ext = build_package.main([
            "--prefix", macho_prefix,
            "--profile", "jit",
            "--icu-mode", "external",
            "--out-dir", ext_out,
            "--version", "1.13.0",
            "--target", "darwin-arm64",
            "--repo-root", self.repo_root,
        ])
        self.assertEqual(rc_ext, 0)
        ext_staging = os.path.join(ext_out, "chakracore-1.13.0-jit-darwin-arm64")
        ok_ext, errs_ext = verify_package.verify_extracted_dir(ext_staging, expected_profile="jit")
        self.assertTrue(ok_ext, "External ICU verify failed: {}".format(errs_ext))

        mpath = os.path.join(ext_staging, "MANIFEST.json")
        with open(mpath, encoding="utf-8") as mf:
            mdata = json.load(mf)
        mdata["icu_mode"] = "bundled"
        mdata["icu_libraries"] = ["libicudata.79.dylib"]
        with open(mpath, "w", encoding="utf-8") as mf:
            json.dump(mdata, mf, indent=2, sort_keys=True)
        ok_tamper, errs_tamper = verify_package.verify_extracted_dir(ext_staging)
        self.assertFalse(ok_tamper)
        self.assertTrue(any("non-portable external dependency" in e for e in errs_tamper), errs_tamper)


def _make_fake_pkg_tree(td, profile="jit", is_darwin=True, is_windows=False):
    repo_root = os.path.join(td, "repo")
    prefix = os.path.join(td, "prefix")
    for fn, data in (
        ("LICENSE.txt", b"MIT\n"),
        ("THIRD-PARTY-NOTICES.txt", b"Notices\n"),
        ("README.md", b"# ChakraCore\n"),
        (os.path.join("lib", "Common", "ChakraCoreVersion.h"),
         b"#define CHAKRA_CORE_MAJOR_VERSION 1\n#define CHAKRA_CORE_MINOR_VERSION 13\n#define CHAKRA_CORE_PATCH_VERSION 0\n"),
    ):
        make_dummy_file(os.path.join(repo_root, fn), data)
    for hname in ("ChakraCore.h", "ChakraCommon.h", "ChakraDebug.h", "ChakraCoreVersion.h"):
        make_dummy_file(os.path.join(prefix, "include", hname), "// {}\n".format(hname).encode("ascii"))
    for nh in ("js_native_api.h", "js_native_api_types.h", "node_api.h", "node_api_types.h"):
        make_dummy_file(os.path.join(prefix, "include", "nodeapi", nh), "// {}\n".format(nh).encode("ascii"))
    make_dummy_file(os.path.join(prefix, ".profile"), (profile + "\n").encode("ascii"))
    if is_windows:
        make_dummy_file(os.path.join(prefix, "bin", "ch.exe"), b"MZch", 0o755)
        make_dummy_file(os.path.join(prefix, "bin", "ch-nodeapi.exe"), b"MZnode", 0o755)
        make_dummy_file(os.path.join(prefix, "bin", "ChakraCore.dll"), b"MZcore", 0o755)
        make_dummy_file(os.path.join(prefix, "bin", "libChakraNodeApi.dll"), b"MZnapi", 0o755)
        make_dummy_file(os.path.join(prefix, "lib", "ChakraCore.lib"), b"!<arch>\nc", 0o755)
        make_dummy_file(os.path.join(prefix, "lib", "libChakraNodeApi.lib"), b"!<arch>\nn", 0o755)
    else:
        ext = "dylib" if is_darwin else "so"
        make_dummy_file(os.path.join(prefix, "bin", "ch"), b"#!/bin/sh\necho 'ch version 1.13.0'\necho 'package smoke'\n", 0o755)
        make_dummy_file(os.path.join(prefix, "bin", "ch-nodeapi"), b"#!/bin/sh\necho 'hello: world'\n", 0o755)
        make_dummy_file(os.path.join(prefix, "lib", "libChakraCore." + ext), b"dummy-core", 0o755)
        make_dummy_file(os.path.join(prefix, "lib", "libChakraNodeApi." + ext), b"dummy-napi", 0o755)
    return repo_root, prefix


class TestWave2RpathAndIcu(unittest.TestCase):
    def test_verify_rejects_bad_macho_rpaths_mocked(self):
        with tempfile.TemporaryDirectory(prefix="chakra-rv05-macho-") as td:
            repo_root, prefix = _make_fake_pkg_tree(td, profile="jit")
            out_dir = os.path.join(td, "pkg")
            self.assertEqual(build_package.main([
                "--prefix", prefix, "--profile", "jit", "--out-dir", out_dir,
                "--version", "1.13.0", "--target", "darwin-arm64",
                "--icu-mode", "external", "--repo-root", repo_root,
            ]), 0)
            archive = os.path.join(out_dir, "chakracore-1.13.0-jit-darwin-arm64.tar.gz")
            for bad_rp, needle in (
                (".zig-cache/o/abc123", ".zig-cache"),
                ("/opt/homebrew/opt/icu4c/lib", "external LC_RPATH"),
                ("relative/lib", "non-relocatable LC_RPATH"),
            ):
                with unittest.mock.patch.object(sys, "platform", "darwin"), \
                     unittest.mock.patch.object(verify_package, "is_macho_file", return_value=True), \
                     unittest.mock.patch.object(
                         verify_package.subprocess,
                         "run",
                         return_value=unittest.mock.Mock(returncode=0, stdout="", stderr=""),
                     ), \
                     unittest.mock.patch.object(
                         verify_package,
                         "parse_macho_load_commands",
                         return_value={"deps": ["/usr/lib/libSystem.B.dylib"], "rpaths": [bad_rp]},
                     ):
                    ok, errs = verify_package.verify_package(archive, expected_profile="jit")
                    self.assertFalse(ok, "Expected failure for rpath {!r}".format(bad_rp))
                    self.assertTrue(any(needle in e for e in errs), repr(errs))

    def test_verify_rejects_bad_elf_runpath_mocked(self):
        with tempfile.TemporaryDirectory(prefix="chakra-rv05-elf-") as td:
            repo_root, prefix = _make_fake_pkg_tree(td, profile="release-shared", is_darwin=False)
            out_dir = os.path.join(td, "pkg")
            self.assertEqual(build_package.main([
                "--prefix", prefix, "--profile", "release-shared", "--out-dir", out_dir,
                "--version", "1.13.0", "--target", "linux-x64",
                "--icu-mode", "external", "--repo-root", repo_root,
            ]), 0)
            archive = os.path.join(out_dir, "chakracore-1.13.0-release-shared-linux-x64.tar.gz")
            for bad_rp, needle in (
                (".zig-cache/o/deadbeef", ".zig-cache"),
                ("lib/relative", "relative RPATH/RUNPATH"),
                ("/opt/custom/lib", "/opt or /usr/local"),
            ):
                with unittest.mock.patch.object(sys, "platform", "linux"), \
                     unittest.mock.patch.object(verify_package.shutil, "which", return_value="/usr/bin/readelf"), \
                     unittest.mock.patch.object(verify_package, "is_elf_file", return_value=True), \
                     unittest.mock.patch.object(
                         verify_package,
                         "parse_elf_dynamic",
                         return_value={"rpaths": [bad_rp], "needed": ["libc.so.6"]},
                     ):
                    ok, errs = verify_package.verify_package(archive, expected_profile="release-shared")
                    self.assertFalse(ok, "Expected ELF rejection for {!r}".format(bad_rp))
                    self.assertTrue(any(needle in e for e in errs), repr(errs))

    @unittest.skipUnless(sys.platform == "darwin" and shutil.which("clang"), "requires macOS clang")
    def test_macho_rpath_and_out_of_tree_provider_dlopen(self):
        with tempfile.TemporaryDirectory(prefix="chakra-rv05-real-") as td:
            repo_root, prefix = _make_fake_pkg_tree(td, profile="release-shared")
            core_c = os.path.join(td, "core.c")
            prov_c = os.path.join(td, "prov.c")
            with open(core_c, "w", encoding="utf-8") as f:
                f.write("int chakra_core_ping(void) { return 42; }\n")
            with open(prov_c, "w", encoding="utf-8") as f:
                f.write(
                    "extern int chakra_core_ping(void);\n"
                    "int napi_get_version(void* env, unsigned int* res) { (void)env; if (res) *res = (unsigned int)chakra_core_ping(); return 0; }\n"
                )
            core_dylib = os.path.join(prefix, "lib", "libChakraCore.dylib")
            prov_dylib = os.path.join(prefix, "lib", "libChakraNodeApi.dylib")
            subprocess.check_call([
                "clang", "-dynamiclib", "-install_name", "@rpath/libChakraCore.dylib",
                core_c, "-o", core_dylib,
            ])
            subprocess.check_call([
                "clang", "-dynamiclib", "-install_name", "@rpath/libChakraNodeApi.dylib",
                "-Wl,-rpath,@loader_path", prov_c, core_dylib, "-o", prov_dylib,
            ])
            out_dir = os.path.join(td, "pkg_good")
            self.assertEqual(build_package.main([
                "--prefix", prefix, "--profile", "release-shared", "--out-dir", out_dir,
                "--version", "1.13.0", "--target", "darwin-arm64",
                "--icu-mode", "external", "--repo-root", repo_root,
            ]), 0)
            archive = os.path.join(out_dir, "chakracore-1.13.0-release-shared-darwin-arm64.tar.gz")
            ok, errs = verify_package.verify_package(archive, expected_profile="release-shared")
            self.assertTrue(ok, errs)
            self.assertEqual(test_package.main(["--tarball", archive, "--repo-root", repo_root]), 0)

            # Negative control: provider with .zig-cache rpath (missing @loader_path) fails both verify and Test 5 dlopen.
            subprocess.check_call([
                "clang", "-dynamiclib", "-install_name", "@rpath/libChakraNodeApi.dylib",
                "-Wl,-rpath,.zig-cache/o/bad123", prov_c, core_dylib, "-o", prov_dylib,
            ])
            out_bad = os.path.join(td, "pkg_bad")
            self.assertEqual(build_package.main([
                "--prefix", prefix, "--profile", "release-shared", "--out-dir", out_bad,
                "--version", "1.13.0", "--target", "darwin-arm64",
                "--icu-mode", "external", "--repo-root", repo_root,
            ]), 0)
            bad_archive = os.path.join(out_bad, "chakracore-1.13.0-release-shared-darwin-arm64.tar.gz")
            ok_bad, errs_bad = verify_package.verify_package(bad_archive, expected_profile="release-shared")
            self.assertFalse(ok_bad)
            self.assertTrue(any(".zig-cache" in e for e in errs_bad), errs_bad)
            staging_bad = os.path.join(out_bad, "chakracore-1.13.0-release-shared-darwin-arm64")
            with self.assertRaises(RuntimeError) as cm:
                test_package.run_package_smoke_tests(staging_bad, repo_root)
            self.assertIn("dlopen failed", str(cm.exception))

    def test_windows_auto_and_env_icu_bundling(self):
        for env_key in ("CHAKRA_ICU_DIR", "ICU_LIB_DIR"):
            with self.subTest(env_key=env_key), tempfile.TemporaryDirectory(prefix="chakra-rv17-") as td:
                repo_root, prefix = _make_fake_pkg_tree(td, profile="release-shared", is_windows=True)
                icu_root = os.path.join(td, "icu_stage")
                icu_bin = os.path.join(icu_root, "bin")
                os.makedirs(icu_bin, exist_ok=True)
                for dll in ("icuuc74.dll", "icuin74.dll", "icudt74.dll"):
                    make_dummy_file(os.path.join(icu_bin, dll), b"MZ" + dll.encode("ascii"))
                env_val = icu_root if env_key == "CHAKRA_ICU_DIR" else os.path.join(icu_root, "lib")
                os.makedirs(env_val, exist_ok=True)
                out_dir = os.path.join(td, "pkg_win")
                with unittest.mock.patch.dict(os.environ, {env_key: env_val}, clear=False):
                    self.assertEqual(build_package.main([
                        "--prefix", prefix, "--profile", "release-shared", "--out-dir", out_dir,
                        "--version", "1.13.0", "--target", "windows-x64",
                        "--icu-mode", "auto", "--repo-root", repo_root,
                    ]), 0)
                archive = os.path.join(out_dir, "chakracore-1.13.0-release-shared-windows-x64.tar.gz")
                ok, errs = verify_package.verify_package(archive, expected_profile="release-shared")
                self.assertTrue(ok, errs)
                with tarfile.open(archive, "r:gz") as tf:
                    names = tf.getnames()
                    mf = [n for n in names if os.path.basename(n) == "MANIFEST.json"][0]
                    manifest = json.loads(tf.extractfile(mf).read().decode("utf-8"))
                    self.assertEqual(manifest["icu_mode"], "bundled")
                    for dll in ("icuuc74.dll", "icuin74.dll", "icudt74.dll"):
                        self.assertTrue(any(n == "bin/" + dll or n.endswith("/bin/" + dll) for n in names), names)

    def test_smoke_subprocess_timeout_raises_runtime_error(self):
        with unittest.mock.patch.object(
            test_package.subprocess, "run",
            side_effect=subprocess.TimeoutExpired(["ch", "--version"], 0.5),
        ):
            with self.assertRaises(RuntimeError) as cm:
                test_package._run_subprocess(["ch", "--version"], timeout=0.5)
        self.assertIn("timed out", str(cm.exception))


if __name__ == "__main__":
    unittest.main()
