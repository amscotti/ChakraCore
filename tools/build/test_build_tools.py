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
            self.assertIn("MISSING from DB (538)", proc.stdout)
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


@unittest.skipUnless(os.name == "posix", "Mac check helpers use POSIX process groups")
class SubprocessLifetimeTests(unittest.TestCase):
    def check_timeout(self, tee, parent_exits=False):
        with tempfile.TemporaryDirectory(prefix="check-timeout-test-") as tmp:
            marker = Path(tmp) / "surviving-descendant"
            grandchild = ("import time; from pathlib import Path; time.sleep(1); "
                          "Path(%r).write_text('still running')" % str(marker))
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
                        common.run_tee(cmd, timeout=0.3)
                    else:
                        common.run_capture(cmd, timeout=0.3)
            elapsed = time.monotonic() - start
            if tee:
                self.assertIn("descendant-ready", output.getvalue())
            else:
                self.assertIn(b"descendant-ready", raised.exception.output)
            self.assertLess(elapsed, 0.9, "timeout also has to bound pipe draining")
            time.sleep(1.1)
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
