#!/usr/bin/env python3
#-------------------------------------------------------------------------------------------------------
# Copyright (c) ChakraCore Project Contributors. All rights reserved.
# Licensed under the MIT license. See LICENSE.txt file in the project root for full license information.
#-------------------------------------------------------------------------------------------------------
"""Focused failure-path tests for the B1-B3 build verification tools.

Run with: python3 tools/build/test_build_tools.py
The fixtures are private temporary trees; no engine sources or installed
artifacts are changed.
"""

import contextlib
import copy
import hashlib
import importlib.util
import io
import json
import os
from pathlib import Path
import platform
import shutil
import signal
import stat
import subprocess
import sys
import tempfile
import time
import unittest
from unittest import mock

HERE = Path(__file__).resolve().parent
ROOT = HERE.parent.parent
sys.path.insert(0, str(HERE))
import common
import check_native
import check_regression
import check_regression_linux
import install_profile
import test_incremental


def load_verifier():
    spec = importlib.util.spec_from_file_location(
        "verify_b1_review_test", ROOT / "tools/build-contract/verify_b1.py")
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


class B1VerdictTests(unittest.TestCase):
    def run_verifier(self, reference=False, reference_matches=True, preserve_live=False):
        verifier = load_verifier()
        with tempfile.TemporaryDirectory(prefix="b1-verdict-test-") as tmp:
            root = Path(tmp)
            config = b"fixture config\n"
            doc_files = ["pal/src/fixture%d.cpp" % i for i in range(91)]
            if reference:
                ref = root / verifier.REFERENCE_CONFIG
                ref.parent.mkdir(parents=True)
                ref.write_bytes(config if reference_matches else b"wrong\n")
            log_path = root / "evidence.log"
            live_markers = [root / ".zig-cache/review-marker", root / "zig-out/review-marker"]
            if preserve_live:
                for path in live_markers:
                    path.parent.mkdir(parents=True, exist_ok=True)
                    path.write_bytes(b"existing profile output\n")

            def run(cmd, timeout, cwd=None):
                if cmd == verifier.ZIG + ["version"]:
                    return 0, "0.17.0\n", 0
                if cmd == verifier.ZIG + ["env"]:
                    return 0, "fixture zig env\n", 0
                if cmd[:len(verifier.ZIG) + 1] == verifier.ZIG + ["build"] and not any(
                        arg.startswith("-D") for arg in cmd):
                    prefix = Path(cmd[cmd.index("--prefix") + 1]) if "--prefix" in cmd else root / "zig-out"
                    gen = prefix / verifier.GENERATED_CONFIG.removeprefix("zig-out/")
                    gen.parent.mkdir(parents=True)
                    gen.write_bytes(config)
                    archive = prefix / verifier.ARCHIVE_B2.removeprefix("zig-out/")
                    archive.parent.mkdir(parents=True)
                    archive.write_bytes(b"fixture archive")
                    return 0, "95/95 checks match the contract\n", 0
                if cmd[:2] == ["ar", "t"]:
                    members = ["__.SYMDEF"] + [
                        "%04d-%s.o" % (i, Path(src).name)
                        for i, src in enumerate(doc_files)]
                    return 0, "\n".join(members), 0
                if cmd[0] == "cmp":
                    return (0 if reference_matches else 1), "", 0
                if cmd[:len(verifier.ZIG) + 1] == verifier.ZIG + ["build"]:
                    # Every negative gate emits its named diagnostics.
                    messages = [word for case in verifier.FAIL_CASES
                                for word in case["want"]]
                    return 1, "build error: " + " ".join(messages), 0
                raise AssertionError("unexpected command: %r" % cmd)

            output = io.StringIO()
            with mock.patch.object(verifier, "ROOT", str(root)), \
                    mock.patch.object(verifier, "EXPECTED_CONFIG_MD5",
                                      hashlib.md5(config).hexdigest()), \
                    mock.patch.object(verifier, "FAIL_CASES",
                                      copy.deepcopy(verifier.FAIL_CASES)), \
                    mock.patch.object(verifier, "run", side_effect=run), \
                    mock.patch.object(verifier, "expected_objects_from_doc",
                                      return_value=(doc_files, set(doc_files))), \
                    mock.patch.object(verifier.shutil, "which", return_value="mise"), \
                    mock.patch.object(sys, "argv", ["verify_b1.py", "--log", str(log_path)]), \
                    contextlib.redirect_stdout(output):
                rc = verifier.main()
            if preserve_live:
                self.assertTrue(all(path.is_file() and path.read_bytes() == b"existing profile output\n"
                                    for path in live_markers), "B1 verifier destroyed live profile state")
            return rc, output.getvalue()

    def test_verifier_preserves_existing_cache_and_installed_profiles(self):
        rc, _ = self.run_verifier(preserve_live=True)
        self.assertEqual(rc, 0)

    def test_clean_checkout_verdict_does_not_require_optional_reference(self):
        rc, output = self.run_verifier()
        self.assertEqual(rc, 0)
        self.assertIn("SUMMARY: 14/14 PASS", output)
        self.assertIn("verify_b1: PASS\n", output)

    def test_local_reference_comparison_keeps_its_own_verdict(self):
        rc, output = self.run_verifier(reference=True)
        self.assertEqual(rc, 0)
        self.assertIn("SUMMARY: 15/15 PASS", output)
        self.assertIn("verify_b1: PASS\n", output)

    def test_reference_mismatch_cannot_emit_success_verdict(self):
        rc, output = self.run_verifier(reference=True, reference_matches=False)
        self.assertEqual(rc, 1)
        self.assertNotIn("verify_b1: PASS\n", output)
        self.assertIn("verify_b1: FAIL\n", output)

    def test_source_validator_fails_on_missing_compile_database_members(self):
        with tempfile.TemporaryDirectory(prefix="contract-test-") as tmp:
            database = Path(tmp) / "compile_commands.json"
            database.write_text(json.dumps([]))
            proc = subprocess.run(
                [sys.executable, str(ROOT / "tools/build-contract/validate_contract.py"),
                 "--database", str(database)], capture_output=True, text=True,
                timeout=30)
            self.assertIn("MISSING from DB (540)", proc.stdout)
            self.assertEqual(proc.returncode, 1)

    @unittest.skipUnless(sys.platform == "darwin" and platform.machine() in ("arm64", "aarch64")
                         and shutil.which("mise"), "requires the native Mac Zig toolchain")
    def test_non_object_sdk_settings_fail_clearly(self):
        with tempfile.TemporaryDirectory(prefix="sdk-settings-test-") as tmp:
            (Path(tmp) / "SDKSettings.json").write_text("[]")
            proc = subprocess.run(["mise", "exec", "--", "zig", "build",
                                   "-Dsdk-path=" + tmp], cwd=ROOT,
                                  capture_output=True, text=True, timeout=30)
            self.assertNotEqual(proc.returncode, 0)
            self.assertIn("SDKSettings.json root must be an object", proc.stderr)

    @unittest.skipUnless(sys.platform == "darwin" and platform.machine() in ("arm64", "aarch64")
                         and shutil.which("mise"), "requires the native Mac Zig toolchain")
    def test_foreign_object_format_fails_before_probing(self):
        proc = subprocess.run(["mise", "exec", "--", "zig", "build", "-Dofmt=elf"],
                              cwd=ROOT, capture_output=True, text=True, timeout=30)
        self.assertNotEqual(proc.returncode, 0)
        self.assertIn("unsupported object format elf", proc.stderr)
        self.assertNotIn("pal-config:", proc.stderr)


class IncrementalFailureTests(unittest.TestCase):
    def test_touch_restores_source_and_propagates_failure(self):
        with tempfile.TemporaryDirectory(prefix="incremental-test-") as tmp:
            source = Path(tmp) / "build.zig"
            source.write_bytes(b"original\n")
            before = source.stat().st_mtime_ns
            with mock.patch.object(test_incremental, "ROOT", tmp):
                with self.assertRaisesRegex(RuntimeError, "build failed"):
                    with test_incremental.touched("build.zig", "// probe\n"):
                        raise RuntimeError("build failed")
            self.assertEqual(source.read_bytes(), b"original\n")
            self.assertEqual(source.stat().st_mtime_ns, before)

    def test_installer_repairs_execute_permission_without_changing_bytes(self):
        with tempfile.TemporaryDirectory(prefix="install-mode-test-") as tmp:
            src, dest = Path(tmp) / "source-ch", Path(tmp) / "installed-ch"
            src.write_bytes(b"same binary bytes")
            dest.write_bytes(src.read_bytes())
            src.chmod(0o755)
            dest.chmod(0o644)
            self.assertTrue(install_profile.copy_if_changed(str(src), str(dest)))
            self.assertEqual(stat.S_IMODE(dest.stat().st_mode), 0o755)
            self.assertEqual(dest.read_bytes(), src.read_bytes())
            self.assertFalse(install_profile.copy_if_changed(str(src), str(dest)))


class ProfileInstallerTests(unittest.TestCase):
    """Confinement and no-op tests for tools/build/install_profile.py.

    Every fixture lives in a private temporary root. main() derives the
    checkout root from __file__, so the fixture writes its own
    out/<profile> and never touches the real out/jit.
    """

    def setUp(self):
        self._tmp = tempfile.TemporaryDirectory(prefix="install-profile-test-")
        self.root = Path(self._tmp.name)
        (self.root / "tools/build").mkdir(parents=True)
        self.script = self.root / "tools/build/install_profile.py"
        self.script.write_text("# fixture installer path\n")
        self.src = self.root / "source/ch"
        self.src.parent.mkdir()
        self.src.write_bytes(b"fixture ch binary\n")
        self.src.chmod(0o755)
        self.dest = self.root / "out/jit"

    def tearDown(self):
        self._tmp.cleanup()

    def install(self, maps, symlinks=()):
        argv = ["install_profile.py", "--profile", "jit", "--dest", "out/jit"]
        argv.extend("--map=" + item for item in maps)
        argv.extend("--symlink=" + item for item in symlinks)
        stdout, stderr = io.StringIO(), io.StringIO()
        with mock.patch.object(install_profile, "__file__", str(self.script)), \
                contextlib.redirect_stdout(stdout), \
                contextlib.redirect_stderr(stderr):
            rc = install_profile.main(argv)
        return rc, stdout.getvalue(), stderr.getvalue()

    def assert_install_fails(self, result, needle):
        rc, stdout, stderr = result
        self.assertEqual(rc, 1, stdout + stderr)
        self.assertIn("install_profile: FAIL:", stderr)
        self.assertIn(needle, stderr)

    def test_normal_install_and_noop_preserve_inode_and_mtime(self):
        result = self.install(["ch=" + str(self.src)],
                              ["libch.dylib=ch"])
        self.assertEqual(result[0], 0, result[2])
        installed = self.dest / "ch"
        self.assertEqual(installed.read_bytes(), self.src.read_bytes())
        self.assertEqual(stat.S_IMODE(installed.stat().st_mode), 0o755)
        self.assertEqual(os.readlink(self.dest / "libch.dylib"), "ch")
        self.assertEqual((self.dest / ".profile").read_text(), "jit\n")
        self.assertEqual((self.dest / ".manifest").read_text(),
                         ".profile\nch\nlibch.dylib\n")
        first = installed.stat()

        result = self.install(["ch=" + str(self.src)],
                              ["libch.dylib=ch"])
        self.assertEqual(result[0], 0, result[2])
        self.assertIn("0 copied", result[1])
        second = installed.stat()
        self.assertEqual(first.st_ino, second.st_ino)
        self.assertEqual(first.st_mtime_ns, second.st_mtime_ns)
        self.assertEqual(os.readlink(self.dest / "libch.dylib"), "ch")

    def test_symlinked_profile_root_is_rejected_before_writing_target(self):
        target = self.root / "out/release-shared"
        target.mkdir(parents=True)
        sentinel = target / "keep"
        sentinel.write_bytes(b"release profile state\n")
        self.dest.symlink_to("release-shared", target_is_directory=True)

        self.assert_install_fails(self.install(["ch=" + str(self.src)]),
                                  "linked profile root")
        self.assertEqual(sentinel.read_bytes(), b"release profile state\n")
        self.assertEqual(sorted(path.name for path in target.iterdir()),
                         ["keep"])

    def test_symlinked_profile_root_to_external_path_is_rejected(self):
        escape = self.root / "escape"
        self.dest.parent.mkdir(parents=True)
        self.dest.symlink_to("../escape", target_is_directory=True)

        self.assert_install_fails(self.install(["ch=" + str(self.src)]),
                                  "linked profile root")
        self.assertFalse(escape.exists())

    def test_in_prefix_symlinked_parent_blocks_map_without_aliasing(self):
        (self.dest / "escape").mkdir(parents=True)
        (self.dest / "bin").symlink_to("escape", target_is_directory=True)

        self.assert_install_fails(
            self.install(["bin/ch=" + str(self.src)]), "symlinked component")
        self.assertFalse((self.dest / "escape/ch").exists())
        self.assertFalse((self.dest / ".profile").exists())
        self.assertFalse((self.dest / ".manifest").exists())

    def test_map_through_in_prefix_symlink_does_not_overwrite_target(self):
        real = self.dest / "real"
        real.mkdir(parents=True)
        victim = real / "userfile"
        victim.write_bytes(b"original user data\n")
        before = victim.stat()
        (self.dest / "linkdir").symlink_to("real", target_is_directory=True)

        self.assert_install_fails(
            self.install(["ch=" + str(self.src),
                          "linkdir/userfile=" + str(self.src)]),
            "symlinked component")
        self.assertEqual(victim.read_bytes(), b"original user data\n")
        after = victim.stat()
        self.assertEqual(before.st_ino, after.st_ino)
        self.assertEqual(before.st_mtime_ns, after.st_mtime_ns)
        self.assertFalse((self.dest / "ch").exists())
        self.assertFalse((self.dest / ".profile").exists())
        self.assertFalse((self.dest / ".manifest").exists())

    def test_symlinked_parent_resolving_outside_prefix_is_rejected(self):
        escape = self.root / "escape"
        escape.mkdir()
        self.dest.mkdir(parents=True)
        (self.dest / "bin").symlink_to(str(escape), target_is_directory=True)

        self.assert_install_fails(
            self.install(["bin/ch=" + str(self.src)]), "symlinked component")
        self.assertFalse((escape / "ch").exists())

    def test_orphan_through_in_prefix_symlink_does_not_delete_target(self):
        real = self.dest / "real"
        real.mkdir(parents=True)
        victim = real / "userfile"
        victim.write_bytes(b"keep this\n")
        before = victim.stat()
        (self.dest / "linkdir").symlink_to("real", target_is_directory=True)
        (self.dest / ".manifest").write_text("linkdir/userfile\n")

        self.assert_install_fails(self.install(["ch=" + str(self.src)]),
                                  "symlinked component")
        self.assertEqual(victim.read_bytes(), b"keep this\n")
        after = victim.stat()
        self.assertEqual(before.st_ino, after.st_ino)
        self.assertEqual(before.st_mtime_ns, after.st_mtime_ns)
        self.assertFalse((self.dest / "ch").exists())
        self.assertEqual((self.dest / ".manifest").read_text(),
                         "linkdir/userfile\n")

    def test_symlink_targets_must_be_bare_relative_names(self):
        targets = ("../escape", "sub/escape", "sub\\escape",
                   "/tmp/escape", "..", "lib..so")
        for target in targets:
            with self.subTest(target=target):
                self.assert_install_fails(
                    self.install(["ch=" + str(self.src)],
                                 ["esc=" + target]),
                    "bare relative name")
                self.assertFalse((self.dest / "esc").exists())
                self.assertFalse((self.dest / "esc").is_symlink())
                self.assertFalse((self.dest / "ch").exists())
                self.assertFalse((self.dest / ".profile").exists())

    def test_symlink_replacement_is_atomic_and_targets_untouched(self):
        self.dest.mkdir(parents=True)
        old = self.dest / "libch.1.dylib"
        new = self.dest / "libch.2.dylib"
        old.write_bytes(b"old target\n")
        new.write_bytes(b"new target\n")
        link = self.dest / "libch.dylib"
        link.symlink_to("libch.1.dylib")
        old_before = old.stat()
        new_before = new.stat()

        result = self.install(["ch=" + str(self.src)],
                              ["libch.dylib=libch.2.dylib"])
        self.assertEqual(result[0], 0, result[2])
        self.assertEqual(os.readlink(link), "libch.2.dylib")
        self.assertEqual(old.read_bytes(), b"old target\n")
        self.assertEqual(new.read_bytes(), b"new target\n")
        self.assertEqual(old.stat().st_mtime_ns, old_before.st_mtime_ns)
        self.assertEqual(new.stat().st_mtime_ns, new_before.st_mtime_ns)
        self.assertFalse(any(name.endswith(".tmp")
                             for name in os.listdir(self.dest)))

        # A failed swap leaves the previous link and both targets intact.
        link.unlink()
        link.symlink_to("libch.1.dylib")
        with mock.patch.object(install_profile.os, "replace",
                               side_effect=OSError("injected replace failure")):
            with self.assertRaises(OSError):
                install_profile.symlink_if_changed("libch.2.dylib",
                                                   str(link))
        self.assertEqual(os.readlink(link), "libch.1.dylib")
        self.assertEqual(old.read_bytes(), b"old target\n")
        self.assertEqual(new.read_bytes(), b"new target\n")
        self.assertFalse(any(name.endswith(".tmp")
                             for name in os.listdir(self.dest)))


class NativeVerdictTests(unittest.TestCase):
    def test_property_assertion_diagnostic_cannot_pass(self):
        text = "\n".join(suite + " : PASS" for suite in check_native.STATIC_SUITES)
        text += "\n" + check_native.SHARED_IGNORE + "\nJsObjectDeleteProperty failed\n"
        self.assertTrue(check_native.check_output_static(text))

    def test_shared_gate_rejects_duplicate_ignores_hiding_a_missing_suite(self):
        ignores = list(check_native.STATIC_SUITES)
        ignores[-1] = ignores[0]
        text = check_native.SHARED_PASS + "\n" + "\n".join(
            "Ignoring " + suite for suite in ignores)
        self.assertTrue(check_native.check_output(text))

    def test_pass_text_must_be_a_complete_line(self):
        ignores = "\n".join("Ignoring " + suite for suite in check_native.STATIC_SUITES)
        self.assertTrue(check_native.check_output(
            "NOT " + check_native.SHARED_PASS + "\n" + ignores))
        static = "\n".join("NOT " + suite + " : PASS"
                           for suite in check_native.STATIC_SUITES)
        self.assertTrue(check_native.check_output_static(
            static + "\n" + check_native.SHARED_IGNORE))

    @unittest.skipUnless(shutil.which("make"), "native harness requires make")
    def test_native_sample_success_text_does_not_hide_nonzero_exit(self):
        with tempfile.TemporaryDirectory(prefix="native-status-test-") as tmp:
            root = Path(tmp)
            work = root / "native-tests"
            work.mkdir()
            for suite in ("test-static-char", "test-shared-basic"):
                (work / suite).mkdir()
                (work / suite / "Platform.js").write_text("// fixture\n")
            sample = work / "test-static-char/sample.o"
            sample.write_text("#!/bin/sh\nprintf 'SUCCESS\\n'\nexit 9\n")
            sample.chmod(0o755)
            ch = root / "ch"
            ch.write_text("#!%s\nimport os\n"
                          "if os.path.basename(os.getcwd()) == 'test-shared-basic':\n"
                          "    print('# IGNORE_THIS_TEST')\n"
                          "else:\n"
                          "    print('all:\\n\\t@true')\n" % sys.executable)
            ch.chmod(0o755)
            proc = subprocess.run(
                ["bash", str(ROOT / "test/native-tests/test_native.sh"),
                 str(ch), "Debug", "clang", "clang++", str(ROOT)],
                cwd=work, capture_output=True, text=True, timeout=30)
            self.assertNotEqual(proc.returncode, 0, proc.stdout + proc.stderr)
            self.assertNotIn("test-static-char : PASS", proc.stdout)
            self.assertIn("test-static-char FAILED", proc.stdout + proc.stderr)


class EngineInitAbortTests(unittest.TestCase):
    # Strict semantics: BLOCKED means an observed qemu guest SIGABRT
    # only — at least one qemu `uncaught target signal 6 (`
    # occurrence and zero occurrences of any other qemu-phrased
    # signal number (occurrence-based; 61 never reads as 6),
    # nonzero rc, and no ` : PASS` anywhere. A bare shell
    # `Aborted` with no signal-6 occurrence, any other
    # qemu-phrased signal, or a native abort grades FAIL.
    QEMU_SIGABRT = ("qemu-x86_64: uncaught target signal 6 (Aborted) - "
                    "core dumped\n")

    def test_qemu_signal6_string_is_engine_init_abort(self):
        text = "Testing test-shared-basic\n" + self.QEMU_SIGABRT
        self.assertTrue(common.is_engine_init_abort(134, text))
        self.assertTrue(common.is_engine_init_abort(-6, text))
        self.assertTrue(common.is_engine_init_abort(1, text))

    def test_qemu_signal6_lines_is_engine_init_abort(self):
        lines = ["Failed to generate 64bit JsBuiltin Bytecode\n",
                 self.QEMU_SIGABRT]
        self.assertTrue(common.is_engine_init_abort(1, lines))

    def test_bare_shell_aborted_is_not_engine_init_abort(self):
        self.assertFalse(common.is_engine_init_abort(
            134, "Aborted (core dumped)\n"))
        self.assertFalse(common.is_engine_init_abort(
            -6, "Aborted (core dumped)\n"))
        self.assertFalse(common.is_engine_init_abort(
            1, ["Aborted (core dumped)\n"]))

    def test_signal11_is_not_engine_init_abort(self):
        text = ("qemu-x86_64: uncaught target signal 11 "
                "(Segmentation fault) - core dumped\n")
        self.assertFalse(common.is_engine_init_abort(139, text))
        self.assertFalse(common.is_engine_init_abort(-11, text))

    def test_mixed_signal6_and_signal11_separate_lines_is_not_abort(self):
        sig11 = ("qemu-x86_64: uncaught target signal 11 "
                 "(Segmentation fault) - core dumped\n")
        self.assertFalse(common.is_engine_init_abort(
            134, self.QEMU_SIGABRT + sig11))
        self.assertFalse(common.is_engine_init_abort(
            134, sig11 + self.QEMU_SIGABRT))
        self.assertFalse(common.is_engine_init_abort(
            134, [sig11, self.QEMU_SIGABRT]))

    def test_mixed_signal6_and_signal11_same_line_is_not_abort(self):
        same_line = ("qemu-x86_64: uncaught target signal 6 (Aborted) - "
                     "core dumped; qemu-x86_64: uncaught target signal 11 "
                     "(Segmentation fault) - core dumped\n")
        self.assertFalse(common.is_engine_init_abort(134, same_line))
        reverse = ("qemu-x86_64: uncaught target signal 11 "
                   "(Segmentation fault) - core dumped; "
                   "qemu-x86_64: uncaught target signal 6 (Aborted) - "
                   "core dumped\n")
        self.assertFalse(common.is_engine_init_abort(134, reverse))

    def test_signal61_is_not_engine_init_abort(self):
        # "signal 6" must not substring-match signals 60-69.
        text = ("qemu-x86_64: uncaught target signal 61 "
                "(Real-time event 61) - core dumped\n")
        self.assertFalse(common.is_engine_init_abort(134, text))
        self.assertFalse(common.is_engine_init_abort(1, text))

    def test_pass_containing_output_is_not_engine_init_abort(self):
        text = "test-shared-basic : PASS\n" + self.QEMU_SIGABRT
        self.assertFalse(common.is_engine_init_abort(134, text))

    def test_abort_free_failure_is_not_engine_init_abort(self):
        self.assertFalse(common.is_engine_init_abort(
            1, "test-static-char FAILED\n"))
        self.assertFalse(common.is_engine_init_abort(134, "output\n"))
        self.assertFalse(common.is_engine_init_abort(
            0, "Aborted (core dumped)\n"))
        self.assertFalse(common.is_engine_init_abort(0, self.QEMU_SIGABRT))

    def test_bytecode_gate_uses_the_shared_predicate(self):
        import verify_bytecode_pure
        self.assertIs(verify_bytecode_pure.common.is_engine_init_abort,
                      common.is_engine_init_abort)
        self.assertFalse(hasattr(verify_bytecode_pure, "check_native"))


class RegressionLinuxGradeTests(unittest.TestCase):
    SIG6 = "qemu: uncaught target signal 6 (Aborted) - core dumped"
    SIG11 = "qemu: uncaught target signal 11 (Segmentation fault) - core dumped"
    SIG61 = "qemu: uncaught target signal 61 (Real-time event 61) - core dumped"

    def setUp(self):
        self._saved_baseline = check_regression_linux.RECORDED_BASELINE
        check_regression_linux.RECORDED_BASELINE = {
            "interpreted": (10, 0), "dynapogo": (10, 0)}

    def tearDown(self):
        check_regression_linux.RECORDED_BASELINE = self._saved_baseline

    def test_rc0_matching_counts_passes(self):
        verdict, _ = check_regression_linux.grade_variant(
            "interpreted", 0, ["Total: passed 10, failed 0"])
        self.assertEqual(verdict, "PASS")

    def test_nonzero_rc_with_matching_counts_fails_and_names_rc(self):
        verdict, detail = check_regression_linux.grade_variant(
            "interpreted", 139, ["Total: passed 10, failed 0"])
        self.assertEqual(verdict, "FAIL")
        self.assertIn("139", detail)

    def test_count_mismatch_at_rc0_fails(self):
        verdict, _ = check_regression_linux.grade_variant(
            "interpreted", 0, ["Total: passed 9, failed 1"])
        self.assertEqual(verdict, "FAIL")

    def test_all_signal6_zero_pass_is_blocked_despite_nonzero_rc(self):
        verdict, _ = check_regression_linux.grade_variant(
            "interpreted", 1,
            ["Total: passed 0, failed 2", self.SIG6, self.SIG6])
        self.assertEqual(verdict, "BLOCKED")

    def test_all_signal11_zero_pass_fails(self):
        verdict, _ = check_regression_linux.grade_variant(
            "interpreted", 139,
            ["Total: passed 0, failed 2", self.SIG11, self.SIG11])
        self.assertEqual(verdict, "FAIL")

    def test_summary_less_signal11_fails(self):
        verdict, _ = check_regression_linux.grade_variant(
            "interpreted", 139, ["some harness output", self.SIG11])
        self.assertEqual(verdict, "FAIL")

    def test_summary_less_signal6_is_blocked(self):
        verdict, _ = check_regression_linux.grade_variant(
            "interpreted", 134, ["some harness output", self.SIG6])
        self.assertEqual(verdict, "BLOCKED")

    def test_summary_less_signal61_fails(self):
        verdict, _ = check_regression_linux.grade_variant(
            "interpreted", 134, ["some harness output", self.SIG61])
        self.assertEqual(verdict, "FAIL")

    def test_all_signal61_zero_pass_fails(self):
        verdict, _ = check_regression_linux.grade_variant(
            "interpreted", 134,
            ["Total: passed 0, failed 2", self.SIG61, self.SIG61])
        self.assertEqual(verdict, "FAIL")

    def test_mixed_signal6_and_signal11_zero_pass_fails(self):
        # A non-signal-6 qemu death vetos BLOCKED even when the
        # signal-6 count alone covers `failed`.
        verdict, _ = check_regression_linux.grade_variant(
            "interpreted", 1,
            ["Total: passed 0, failed 2", self.SIG6, self.SIG11, self.SIG6])
        self.assertEqual(verdict, "FAIL")

    def test_mixed_same_line_zero_pass_fails(self):
        # One physical line carrying both signals must still veto:
        # the veto is per occurrence, not per line.
        mixed_line = self.SIG6 + "; " + self.SIG11
        verdict, _ = check_regression_linux.grade_variant(
            "interpreted", 1,
            ["Total: passed 0, failed 2", mixed_line, self.SIG6])
        self.assertEqual(verdict, "FAIL")

    def test_two_signal6_occurrences_on_one_line_count_as_two(self):
        # The counting path counts signal-6 occurrences, not lines.
        verdict, _ = check_regression_linux.grade_variant(
            "interpreted", 1,
            ["Total: passed 0, failed 2", self.SIG6 + "; " + self.SIG6])
        self.assertEqual(verdict, "BLOCKED")

    def test_summary_less_mixed_separate_lines_fails(self):
        for lines in (["some harness output", self.SIG11, self.SIG6],
                      ["some harness output", self.SIG6, self.SIG11]):
            verdict, _ = check_regression_linux.grade_variant(
                "interpreted", 134, lines)
            self.assertEqual(verdict, "FAIL")

    def test_summary_less_mixed_same_line_fails(self):
        mixed_line = self.SIG6 + "; " + self.SIG11
        verdict, _ = check_regression_linux.grade_variant(
            "interpreted", 134, ["some harness output", mixed_line])
        self.assertEqual(verdict, "FAIL")

    def test_all_signal6_zero_pass_at_rc0_is_not_blocked(self):
        verdict, _ = check_regression_linux.grade_variant(
            "interpreted", 0,
            ["Total: passed 0, failed 2", self.SIG6, self.SIG6])
        self.assertEqual(verdict, "FAIL")

    def test_summary_less_signal6_at_rc0_is_not_blocked(self):
        verdict, _ = check_regression_linux.grade_variant(
            "interpreted", 0, ["some harness output", self.SIG6])
        self.assertEqual(verdict, "FAIL")


class GateCountsTests(unittest.TestCase):
    """tools/build/gate_counts.json is the single source of the Mac
    regression pins: check_regression.py must follow it exactly, and a
    missing/malformed source must fail loudly (no silent defaults)."""

    def load_source(self):
        with open(HERE / "gate_counts.json", encoding="utf-8") as f:
            return json.load(f)

    def test_source_shape(self):
        data = self.load_source()
        for select in ("test", "debug"):
            self.assertIsInstance(data[select]["passed"], int)
            self.assertIsInstance(data[select]["failed"], int)
            self.assertNotIsInstance(data[select]["passed"], bool)
            self.assertNotIsInstance(data[select]["failed"], bool)
            self.assertGreaterEqual(data[select]["passed"], 0)
            self.assertGreaterEqual(data[select]["failed"], 0)

    def test_check_regression_follows_source(self):
        data = self.load_source()
        for select in ("test", "debug"):
            self.assertEqual(check_regression.SELECT_COUNTS[select],
                             (data[select]["passed"], data[select]["failed"]))
        self.assertEqual((check_regression.EXPECTED_PASSED,
                          check_regression.EXPECTED_FAILED),
                         check_regression.SELECT_COUNTS["test"])

    def test_wrong_counts_rejected(self):
        tp, tf = check_regression.SELECT_COUNTS["test"]
        line = "Total: passed %d, failed %d" % (tp, tf)
        self.assertIsNone(check_regression.check_counts(
            check_regression.parse_total([line]), tp, tf))
        self.assertIsNotNone(check_regression.check_counts(
            check_regression.parse_total(
                ["Total: passed %d, failed %d" % (tp + 1, tf)]), tp, tf))
        if (check_regression.SELECT_COUNTS["test"]
                != check_regression.SELECT_COUNTS["debug"]):
            dp, df = check_regression.SELECT_COUNTS["debug"]
            dbg = "Total: passed %d, failed %d" % (dp, df)
            self.assertIsNotNone(check_regression.check_counts(
                check_regression.parse_total([dbg]), tp, tf))
            self.assertIsNotNone(check_regression.check_counts(
                check_regression.parse_total([line]), dp, df))

    def run_isolated_selftest(self, gate_counts_text=None):
        with tempfile.TemporaryDirectory(prefix="gate-counts-test-") as tmp:
            root = Path(tmp)
            shutil.copy(HERE / "check_regression.py",
                        root / "check_regression.py")
            shutil.copy(HERE / "common.py", root / "common.py")
            if gate_counts_text is not None:
                (root / "gate_counts.json").write_text(gate_counts_text)
            return subprocess.run(
                [sys.executable, str(root / "check_regression.py"),
                 "--self-test"],
                capture_output=True, text=True, timeout=60)

    def test_missing_source_fails_loudly(self):
        proc = self.run_isolated_selftest()
        self.assertNotEqual(proc.returncode, 0)
        self.assertIn("gate_counts.json", proc.stderr)

    def test_malformed_source_fails_loudly(self):
        proc = self.run_isolated_selftest('{"test": {"passed": 1}}')
        self.assertNotEqual(proc.returncode, 0)
        self.assertIn("gate_counts.json", proc.stderr)


class BytecodePurityTests(unittest.TestCase):
    def make_fixture(self, root, verdict=0, interrupt=False):
        js_dir = root / "lib/Runtime/Library/InJavascript"
        js_dir.mkdir(parents=True)
        original = js_dir / "JsBuiltIn.nojit.bc.64b.h"
        original.write_bytes(b"original header\n")
        for name in ("JsBuiltIn.nojit.bc.32b.h", "JsBuiltIn.bc.64b.h", "JsBuiltIn.bc.32b.h"):
            (js_dir / name).write_bytes(b"another header\n")
        (js_dir / "Intl.js").write_text("// fixture\n")
        tools = root / "tools"
        tools.mkdir()
        regen = tools / "regenByteCode.py"
        regen.write_text(
            "import os, signal, sys, time\nfrom pathlib import Path\n"
            "root = Path(__file__).resolve().parent.parent\n"
            "header = root / 'lib/Runtime/Library/InJavascript/JsBuiltIn.nojit.bc.64b.h'\n"
            "header.write_bytes(b'regenerated header\\n')\n"
            + ("os.kill(os.getppid(), signal.SIGINT)\ntime.sleep(0.2)\n" if interrupt else "")
            + "if Path(%r).read_bytes() != b'original header\\n':\n"
              "    print('SOURCE HEADER MODIFIED DURING VERIFICATION', flush=True)\n"
              "    sys.exit(7)\n"
              "sys.exit(%d)\n" % (str(original), verdict))
        return regen, original

    def invoke(self, regen, binary=True):
        cmd = [sys.executable, str(HERE / "verify_bytecode_pure.py"),
               str(regen), "--noJit", "--verify"]
        if binary:
            cmd += ["--binary=" + sys.executable]
        return subprocess.run(cmd, capture_output=True, text=True, timeout=10)

    def test_verification_never_exposes_regenerated_source_headers(self):
        with tempfile.TemporaryDirectory(prefix="bytecode-purity-test-") as tmp:
            regen, original = self.make_fixture(Path(tmp))
            before = original.stat().st_mtime_ns
            proc = self.invoke(regen)
            self.assertEqual(proc.returncode, 0, proc.stdout + proc.stderr)
            self.assertEqual(original.read_bytes(), b"original header\n")
            self.assertEqual(original.stat().st_mtime_ns, before)

    def test_failed_verification_stays_failed_on_repeat(self):
        with tempfile.TemporaryDirectory(prefix="bytecode-failure-test-") as tmp:
            regen, original = self.make_fixture(Path(tmp), verdict=1)
            for _ in range(2):
                proc = self.invoke(regen)
                self.assertEqual(proc.returncode, 1, proc.stdout + proc.stderr)
                self.assertEqual(original.read_bytes(), b"original header\n")

    def test_interrupted_verification_keeps_source_intact(self):
        with tempfile.TemporaryDirectory(prefix="bytecode-interrupt-test-") as tmp:
            regen, original = self.make_fixture(Path(tmp), interrupt=True)
            proc = self.invoke(regen)
            self.assertNotEqual(proc.returncode, 0)
            self.assertEqual(original.read_bytes(), b"original header\n")

    def test_pure_check_requires_a_built_binary(self):
        with tempfile.TemporaryDirectory(prefix="bytecode-args-test-") as tmp:
            regen, original = self.make_fixture(Path(tmp))
            proc = self.invoke(regen, binary=False)
            self.assertEqual(proc.returncode, 2)
            self.assertEqual(original.read_bytes(), b"original header\n")


@unittest.skipUnless(os.name == "posix", "the regen stub binary is a POSIX shell script")
class RegenSingleVariantGuardTests(unittest.TestCase):
    """The regenByteCode.py single-variant overwrite guard (b5-guid-rotation
    hazard): a --jit/--noJit-only run whose content differs from the checked-in
    headers must refuse (exit 1) and restore the headers instead of silently
    rewriting them."""

    def make_fixture(self, root):
        js_dir = root / "lib/Runtime/Library/InJavascript"
        js_dir.mkdir(parents=True)
        for name in ("JsBuiltIn.nojit.bc.64b.h", "JsBuiltIn.nojit.bc.32b.h",
                     "JsBuiltIn.bc.64b.h", "JsBuiltIn.bc.32b.h"):
            (js_dir / name).write_bytes(b"original header\n")
        (js_dir / "Intl.js").write_text("// fixture\n")
        (js_dir / "Array_prototype.js").write_text("// fixture\n")
        (js_dir / "Math_object.js").write_text("// fixture\n")
        tools = root / "tools"
        tools.mkdir()
        shutil.copyfile(HERE.parent / "regenByteCode.py", tools / "regenByteCode.py")
        stub = root / "stub-ch.sh"
        stub.write_text("#!/bin/sh\nprintf 'stub bytecode output\\\\n'\n")
        stub.chmod(0o755)
        return js_dir, stub

    def invoke(self, root, extra):
        cmd = [sys.executable, str(root / "tools/regenByteCode.py"),
               "--skip-build", "--noJit", "--binary=" + str(root / "stub-ch.sh")] + extra
        return subprocess.run(cmd, capture_output=True, text=True, timeout=60)

    def test_single_variant_content_change_is_refused_and_restored(self):
        with tempfile.TemporaryDirectory(prefix="regen-guard-test-") as tmp:
            root = Path(tmp)
            js_dir, stub = self.make_fixture(root)
            proc = self.invoke(root, [])
            self.assertEqual(proc.returncode, 1, proc.stdout + proc.stderr)
            self.assertIn("REFUSED", proc.stdout + proc.stderr)
            for name in ("JsBuiltIn.nojit.bc.64b.h", "JsBuiltIn.nojit.bc.32b.h",
                         "JsBuiltIn.bc.64b.h", "JsBuiltIn.bc.32b.h"):
                self.assertEqual((js_dir / name).read_bytes(), b"original header\n",
                                 name + " restored byte-identical")

    def test_single_variant_no_change_stays_green(self):
        with tempfile.TemporaryDirectory(prefix="regen-guard-green-test-") as tmp:
            root = Path(tmp)
            js_dir, stub = self.make_fixture(root)
            # Pre-write the exact content the stub generates so the run is a
            # no-op: generate once with the escape hatch, snapshot, restore the
            # snapshot as the baseline, then run guarded.
            proc = self.invoke(root, ["--allow-single-variant-write"])
            self.assertEqual(proc.returncode, 0, proc.stdout + proc.stderr)
            snapshot = {name: (js_dir / name).read_bytes()
                        for name in ("JsBuiltIn.nojit.bc.64b.h", "JsBuiltIn.nojit.bc.32b.h")}
            proc = self.invoke(root, [])
            self.assertEqual(proc.returncode, 0, proc.stdout + proc.stderr)
            self.assertIn("not required", proc.stdout + proc.stderr)
            for name, content in snapshot.items():
                self.assertEqual((js_dir / name).read_bytes(), content)

    def test_escape_hatch_allows_scratch_single_variant_writes(self):
        with tempfile.TemporaryDirectory(prefix="regen-guard-escape-test-") as tmp:
            root = Path(tmp)
            js_dir, stub = self.make_fixture(root)
            proc = self.invoke(root, ["--allow-single-variant-write"])
            self.assertEqual(proc.returncode, 0, proc.stdout + proc.stderr)
            self.assertIn("one variant only", proc.stdout + proc.stderr)
            self.assertNotEqual((js_dir / "JsBuiltIn.nojit.bc.64b.h").read_bytes(),
                                b"original header\n")


class RegenGuidPathTests(unittest.TestCase):
    """The GUID mint path (swarm-trie mint-path fix, must-before-Linux): the
    release-file-version header must be written through the TRACKED casing
    lib/Runtime/ByteCode/ (capital C). A lowercase 'Bytecode' component
    wrote the same file on case-insensitive macOS but created an UNTRACKED
    sibling on case-sensitive Linux, leaving the tracked header stale."""

    TRACKED = "lib/Runtime/ByteCode/ByteCodeCacheReleaseFileVersion.h"

    def make_fixture(self, root):
        js_dir = root / "lib/Runtime/Library/InJavascript"
        js_dir.mkdir(parents=True)
        for name in ("JsBuiltIn.nojit.bc.64b.h", "JsBuiltIn.nojit.bc.32b.h",
                     "JsBuiltIn.bc.64b.h", "JsBuiltIn.bc.32b.h"):
            (js_dir / name).write_bytes(b"original header\n")
        (js_dir / "Intl.js").write_text("// fixture\n")
        (js_dir / "Array_prototype.js").write_text("// fixture\n")
        (js_dir / "Math_object.js").write_text("// fixture\n")
        tools = root / "tools"
        tools.mkdir()
        shutil.copyfile(HERE.parent / "regenByteCode.py", tools / "regenByteCode.py")
        # Dual-variant stub binaries at the default out/ paths, so the run
        # passes the single-variant guard and reaches the GUID mint. Their
        # output differs from the pre-written headers => changes detected.
        for variant in ("noJit", "jit"):
            stub = root / "out" / variant / "debug" / "ch"
            stub.parent.mkdir(parents=True)
            stub.write_text("#!/bin/sh\nprintf 'stub bytecode output\\\\n'\n")
            stub.chmod(0o755)
        guid_header = root / self.TRACKED
        guid_header.parent.mkdir(parents=True)
        guid_header.write_text("// old version header\n")
        return js_dir, guid_header

    def test_mint_writes_the_tracked_cased_path(self):
        with tempfile.TemporaryDirectory(prefix="regen-guid-path-test-") as tmp:
            root = Path(tmp)
            js_dir, guid_header = self.make_fixture(root)
            proc = subprocess.run(
                [sys.executable, str(root / "tools/regenByteCode.py"), "--skip-build"],
                capture_output=True, text=True, timeout=60)
            self.assertEqual(proc.returncode, 0, proc.stdout + proc.stderr)
            self.assertIn("Generating new GUID", proc.stdout + proc.stderr)
            # The tracked-path file was overwritten in place (not shadowed).
            content = guid_header.read_text()
            self.assertNotIn("old version header", content)
            self.assertIn("byteCodeCacheReleaseFileVersion", content)
            # Exact-cased directory entries: on a case-sensitive filesystem a
            # lowercase-writing script leaves a 'Bytecode' sibling; on a
            # case-insensitive one the constant check below still pins it.
            self.assertIn("ByteCode", os.listdir(root / "lib/Runtime"))
            self.assertNotIn("Bytecode", os.listdir(root / "lib/Runtime"))
            self.assertIn("ByteCodeCacheReleaseFileVersion.h",
                          os.listdir(root / "lib/Runtime/ByteCode"))
            self.assertNotIn("Bytecode", os.listdir(root / "lib/Runtime/ByteCode"))

    def test_script_uses_the_exact_tracked_cased_constant(self):
        source = (HERE.parent / "regenByteCode.py").read_text()
        self.assertIn("'/../lib/Runtime/ByteCode/ByteCodeCacheReleaseFileVersion.h'",
                      source)
        self.assertNotIn("'/../lib/Runtime/Bytecode/ByteCodeCacheReleaseFileVersion.h'",
                         source)
        # And the constant names a file git actually tracks.
        tracked = subprocess.run(["git", "-C", str(ROOT), "ls-files", "--",
                                  self.TRACKED], capture_output=True, text=True)
        self.assertEqual(tracked.stdout.strip(), self.TRACKED)


@unittest.skipUnless(os.name == "posix", "Mac check helpers use POSIX process groups")
class SubprocessLifetimeTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        # The budgets below assume a fast interpreter startup; under
        # emulation (qemu-user) startup alone can exceed them, so measure
        # once and scale every budget. The mechanism under test (kill the
        # group, bound the drain) is identical at any scale, and scaling
        # up is assertion-safe (every bound is an upper bound or a
        # presence check). Fast hosts keep SCALE=1: byte-identical budgets.
        t0 = time.monotonic()
        subprocess.run([sys.executable, "-c", "pass"], check=True,
                       stdout=subprocess.DEVNULL)
        cls.SCALE = 10 if time.monotonic() - t0 > 0.1 else 1

    def check_timeout(self, tee, parent_exits=False):
        s = self.SCALE
        with tempfile.TemporaryDirectory(prefix="check-timeout-test-") as tmp:
            marker = Path(tmp) / "surviving-descendant"
            grandchild = ("import time; from pathlib import Path; time.sleep(%d); "
                          "Path(%r).write_text('still running')" % (s, str(marker)))
            parent = ("import subprocess, sys, time; "
                      "subprocess.Popen([sys.executable, '-c', %r]); "
                      "print('descendant-ready', flush=True); "
                      "time.sleep(%s)" % (grandchild, "0" if parent_exits else "5"))
            cmd = [sys.executable, "-c", parent]
            output = io.StringIO()
            start = time.monotonic()
            with contextlib.redirect_stdout(output):
                with self.assertRaises(subprocess.TimeoutExpired) as raised:
                    if tee:
                        common.run_tee(cmd, timeout=0.3 * s)
                    else:
                        common.run_capture(cmd, timeout=0.3 * s)
            elapsed = time.monotonic() - start
            if tee:
                self.assertIn("descendant-ready", output.getvalue())
            else:
                self.assertIn(b"descendant-ready", raised.exception.output)
            self.assertLess(elapsed, 0.9 * s, "timeout also has to bound pipe draining")
            time.sleep(1.1 * s)
            self.assertFalse(marker.exists(), "descendant survived the failed check")

    def test_tee_timeout_kills_descendants(self):
        self.check_timeout(tee=True)

    def test_capture_timeout_kills_descendants(self):
        self.check_timeout(tee=False)

    def test_tee_timeout_bounds_inherited_pipe_after_parent_exit(self):
        self.check_timeout(tee=True, parent_exits=True)

    def test_tee_timeout_bounds_group_escaping_daemon_loudly(self):
        with tempfile.TemporaryDirectory(prefix="check-daemon-test-") as tmp:
            pidfile = str(Path(tmp) / "daemon.pid")
            daemon = ("import os, time; from pathlib import Path; "
                      "Path(%r).write_text(str(os.getpid())); time.sleep(30)"
                      % pidfile)
            parent = ("import subprocess, sys; "
                      "subprocess.Popen([sys.executable, '-c', %r], "
                      "stdout=sys.stdout, start_new_session=True); " % daemon
                      + "print('decoy-ready', flush=True)")
            cmd = [sys.executable, "-c", parent]
            out, err = io.StringIO(), io.StringIO()
            start = time.monotonic()
            try:
                with contextlib.redirect_stdout(out), contextlib.redirect_stderr(err), \
                        mock.patch.object(common, "PUMP_DRAIN_TIMEOUT", 2):
                    with self.assertRaises(subprocess.TimeoutExpired) as raised:
                        common.run_tee(cmd, timeout=1)
            finally:
                try:
                    pid = int(Path(pidfile).read_text().strip())
                except OSError:
                    pid = None
                if pid:
                    try:
                        os.kill(pid, signal.SIGKILL)
                    except ProcessLookupError:
                        pass
            elapsed = time.monotonic() - start
            output = raised.exception.output
            self.assertIn("decoy-ready", "".join(
                output if isinstance(output, list) else [output or ""]))
            self.assertLess(elapsed, 20, "drain ceiling did not bound delivery")
            self.assertIn("TIMEOUT draining child output", err.getvalue())
            self.assertIsInstance(raised.exception.__cause__,
                                  subprocess.TimeoutExpired)

    def test_kill_group_tolerates_zombie_only_group(self):
        # Deterministic core: killpg raising EPERM (Darwin zombie-only
        # groups) is swallowed like ESRCH, and the leader is still reaped.
        proc = subprocess.Popen([sys.executable, "-c", "pass"],
                                start_new_session=True)
        try:
            time.sleep(0.5)  # let the leader exit; no reap -> zombie
            self.assertIsNone(proc.returncode)
            with mock.patch.object(os, "killpg",
                                   side_effect=PermissionError) as killpg:
                common._kill_group(proc)
            self.assertEqual(killpg.call_count, 1)
            self.assertIsNotNone(proc.returncode)
        finally:
            try:
                proc.wait(timeout=5)
            except Exception:
                pass
        # Live probe: a real zombie-only group must not leak PermissionError
        # (killpg raises ESRCH on Linux, EPERM on Darwin here).
        live = subprocess.Popen([sys.executable, "-c", "pass"],
                                start_new_session=True)
        try:
            time.sleep(0.5)
            self.assertIsNone(live.returncode)
            common._kill_group(live)
            self.assertIsNotNone(live.returncode)
        finally:
            try:
                live.wait(timeout=5)
            except Exception:
                pass


if __name__ == "__main__":
    unittest.main()
