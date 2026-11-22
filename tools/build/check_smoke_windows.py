#!/usr/bin/env python3
#-------------------------------------------------------------------------------------------------------
# Copyright (c) ChakraCore Project Contributors. All rights reserved.
# Licensed under the MIT license. See LICENSE.txt file in the project root for full license information.
#-------------------------------------------------------------------------------------------------------
"""B6 / Windows x64 smoke and flag-output verification gate.

Sub-checks against the zig-built ch.exe (and optional ch-nodeapi.exe),
asserting OUTPUT TEXT rather than exit code alone:

  1. `ch.exe -v` exits 0 and prints a `ch.exe version N.N...` line.
  2. `ch.exe --this-flag-does-not-exist <snippet>`: matches the profile
     flag-parser contract (test: nonzero + Invalid Flag + Usage:;
     debug-static: rc 0 + Invalid Flag + Usage:; release-shared: rc 0
     and byte-empty output).
  3. One-line JS snippet (`print("zig-smoke-ok " + (1+1));`) exits 0
     with stdout `zig-smoke-ok 2`.
  4. Core ES5/ES6/ES2025/ES2026 JS script exercising:
       - ES5 Array.prototype.map/reduce
       - ES6 const/let/arrow/Map/Symbol/template literals
       - ES2026 Iterator.zip
       - ES2025 Uint8Array.fromBase64
       - C3.1 Explicit Resource Management (`using` sync disposal)
       - I1/I2 Intl.Locale (when --icu != none)
       - ES2026 Promise.allKeyed
     exits 0 with stdout `windows-core-js-ok`.
  5. For release-shared: `ch.exe test/Basics/hello.js` exits 0 with
     lines `hello world` and `PASS`.
  6. When ch-nodeapi.exe is supplied via --ch-nodeapi (or present beside
     ch.exe on shared profiles): `ch-nodeapi.exe --help` and
     `ch-nodeapi.exe --version` both exit 0 with expected text.

Usage:
  python3 tools/build/check_smoke_windows.py <ch.exe|prefix>
      [--profile test|debug-static|release-shared]
      [--ch-nodeapi <ch-nodeapi.exe>] [--icu icu4c|windows|none]
  python3 tools/build/check_smoke_windows.py --self-test

Exit code: 0 iff every sub-check passes.
"""

import os
import re
import shutil
import subprocess
import sys
import tempfile
from unittest import mock

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import common

PROFILES = ("test", "debug-static", "release-shared")
ICU_MODES = ("icu4c", "windows", "none")

VERSION_RE = re.compile(r"^ch(?:\.exe)? version \d+\.\d+", re.MULTILINE)
BOGUS_FLAG = "--this-flag-does-not-exist"
SNIPPET = 'print("zig-smoke-ok " + (1+1));\n'
SNIPPET_WANT = "zig-smoke-ok 2"
INVALID_FLAG_WANT_RC = {"test": None, "debug-static": 0,
                        "release-shared": 0}
INVALID_FLAG_SILENT = frozenset({"release-shared"})
HELLO_REL = os.path.join("test", "Basics", "hello.js")
HELLO_WANT = ["hello world", "PASS"]
CORE_JS_WANT = "windows-core-js-ok"

CORE_JS_TEMPLATE = """\
(function () {
  var es5 = [1, 2, 3].map(function (x) { return x * 2; }).reduce(function (a, b) { return a + b; }, 0);
  if (es5 !== 12) throw new Error("es5 failed: " + es5);

  const sym = Symbol("k");
  const m = new Map([[sym, 42]]);
  let tag = `v=${m.get(sym)}`;
  const fn = ([a, b]) => a + b;
  if (tag !== "v=42" || fn([20, 22]) !== 42) throw new Error("es6 failed");

  const zipped = Array.from(Iterator.zip([[1, 2], ["a", "b"]]));
  if (zipped.length !== 2 || zipped[0][0] !== 1 || zipped[1][1] !== "b") {
    throw new Error("Iterator.zip failed");
  }

  const bytes = Uint8Array.fromBase64("aGVsbG8=");
  if (Array.from(bytes).join(",") !== "104,101,108,108,111") {
    throw new Error("Uint8Array.fromBase64 failed");
  }

  const disposed = [];
  {
    using _res = { [Symbol.dispose]() { disposed.push("sync"); } };
  }
  if (disposed.join(",") !== "sync") {
    throw new Error("using disposal failed: " + disposed.join(","));
  }

  if (typeof Intl === "object" && typeof Intl.Locale === "function") {
    const loc = new Intl.Locale("en-Latn-US");
    if (loc.language !== "en" || loc.script !== "Latn" || loc.region !== "US") {
      throw new Error("Intl.Locale failed");
    }
  } else if (__EXPECT_INTL__) {
    throw new Error("Intl.Locale missing");
  }

  Promise.allKeyed({ a: Promise.resolve(10), b: Promise.resolve(32) }).then(function (r) {
    if (r.a + r.b !== 42) {
      print("CORE_JS_FAIL: Promise.allKeyed");
      if (typeof WScript !== "undefined" && WScript.Quit) WScript.Quit(1);
      return;
    }
    print("windows-core-js-ok");
  }).catch(function (e) {
    print("CORE_JS_FAIL: " + e);
    if (typeof WScript !== "undefined" && WScript.Quit) WScript.Quit(1);
  });
})();
"""


def build_core_js(icu_mode="icu4c"):
    expect_intl = "false" if icu_mode == "none" else "true"
    return CORE_JS_TEMPLATE.replace("__EXPECT_INTL__", expect_intl)


def check_version(ch):
    rc, out, err = common.run_capture([ch, "-v"])
    first = out.splitlines()[0] if out.splitlines() else ""
    print("check_smoke_windows: ch -v -> rc=%d out=%r err=%r"
          % (rc, first, err))
    if rc != 0:
        return "ch -v exited %d, require 0" % rc
    if not VERSION_RE.search(out):
        return "ch -v output %r lacks a 'ch(.exe) version N.N' line" % out
    return None


def check_invalid_flag(ch, snippet_path, profile="test"):
    rc, out, err = common.run_capture([ch, BOGUS_FLAG, snippet_path])
    combined = out + err
    print("check_smoke_windows: ch %s -> rc=%d out=%r err=%r"
          % (BOGUS_FLAG, rc, out.splitlines()[:1], err.splitlines()[:1]))
    want_rc = INVALID_FLAG_WANT_RC[profile]
    if want_rc is None:
        if rc == 0:
            return "ch %s exited 0, require nonzero" % BOGUS_FLAG
    elif rc != want_rc:
        return ("ch %s exited %d, require %d (%s contract)"
                % (BOGUS_FLAG, rc, want_rc, profile))
    if profile in INVALID_FLAG_SILENT:
        if combined != "":
            return ("ch %s must run silent in %s (got out=%r err=%r)"
                    % (BOGUS_FLAG, profile, out, err))
        return None
    want = "Invalid Flag : " + BOGUS_FLAG
    if want not in combined:
        return "invalid-flag output lacks %r (got %r)" % (want, combined)
    if "Usage:" not in combined:
        return "invalid-flag output lacks 'Usage:' (got %r)" % combined
    return None


def check_snippet(ch, snippet_path):
    rc, out, err = common.run_capture([ch, snippet_path])
    print("check_smoke_windows: snippet -> rc=%d out=%r err=%r"
          % (rc, out, err))
    if rc != 0:
        return "snippet exited %d, require 0 (err=%r)" % (rc, err)
    if out.strip() != SNIPPET_WANT:
        return ("snippet stdout %r != %r" % (out.strip(), SNIPPET_WANT))
    return None


def check_core_js(ch, core_path):
    rc, out, err = common.run_capture([ch, core_path])
    print("check_smoke_windows: core-js -> rc=%d out=%r err=%r"
          % (rc, out.strip(), err))
    if rc != 0:
        return "core-js exited %d, require 0 (err=%r)" % (rc, err)
    if out.strip() != CORE_JS_WANT:
        return ("core-js stdout %r != %r (err=%r)"
                % (out.strip(), CORE_JS_WANT, err))
    return None


def check_hello(ch, hello_path):
    rc, out, err = common.run_capture([ch, hello_path])
    print("check_smoke_windows: hello -> rc=%d out=%r err=%r" % (rc, out, err))
    if rc != 0:
        return "hello.js exited %d, require 0 (err=%r)" % (rc, err)
    if out.splitlines() != HELLO_WANT:
        return ("hello.js stdout %r != %r" % (out.splitlines(), HELLO_WANT))
    return None


def check_ch_nodeapi(ch_nodeapi):
    rc_h, out_h, err_h = common.run_capture([ch_nodeapi, "--help"])
    comb_h = out_h + err_h
    print("check_smoke_windows: ch-nodeapi --help -> rc=%d out=%r"
          % (rc_h, out_h.splitlines()[:1]))
    if rc_h != 0:
        return "ch-nodeapi --help exited %d, require 0 (err=%r)" % (rc_h, err_h)
    if "usage: ch-nodeapi" not in comb_h.lower():
        return "ch-nodeapi --help output lacks 'usage: ch-nodeapi'"

    rc_v, out_v, err_v = common.run_capture([ch_nodeapi, "--version"])
    comb_v = out_v + err_v
    print("check_smoke_windows: ch-nodeapi --version -> rc=%d out=%r"
          % (rc_v, out_v.splitlines()[:1]))
    if rc_v != 0:
        return ("ch-nodeapi --version exited %d, require 0 (err=%r)"
                % (rc_v, err_v))
    if "ch-nodeapi" not in comb_v.lower():
        return "ch-nodeapi --version output lacks 'ch-nodeapi'"
    return None


def parse_args(args):
    profile = "test"
    icu_mode = "icu4c"
    ch_nodeapi = None
    positional = []
    i = 0
    while i < len(args):
        a = args[i]
        if a == "--profile":
            if i + 1 >= len(args):
                return None, "--profile needs a value"
            profile = args[i + 1]
            i += 2
        elif a.startswith("--profile="):
            profile = a.split("=", 1)[1]
            i += 1
        elif a == "--icu":
            if i + 1 >= len(args):
                return None, "--icu needs a value"
            icu_mode = args[i + 1]
            i += 2
        elif a.startswith("--icu="):
            icu_mode = a.split("=", 1)[1]
            i += 1
        elif a == "--ch-nodeapi":
            if i + 1 >= len(args):
                return None, "--ch-nodeapi needs a value"
            ch_nodeapi = args[i + 1]
            i += 2
        elif a.startswith("--ch-nodeapi="):
            ch_nodeapi = a.split("=", 1)[1]
            i += 1
        elif a.startswith("-"):
            return None, "unknown option %r" % a
        else:
            positional.append(a)
            i += 1
    if profile not in PROFILES:
        return None, ("bad --profile %r: expected one of: %s"
                      % (profile, ", ".join(PROFILES)))
    if icu_mode not in ICU_MODES:
        return None, ("bad --icu %r: expected one of: %s"
                      % (icu_mode, ", ".join(ICU_MODES)))
    if len(positional) != 1:
        return None, ("usage: check_smoke_windows.py <ch.exe|prefix> "
                      "[--profile test|debug-static|release-shared] "
                      "[--ch-nodeapi <ch-nodeapi.exe>] "
                      "[--icu icu4c|windows|none]")
    return {
        "target": positional[0],
        "profile": profile,
        "icu": icu_mode,
        "ch_nodeapi": ch_nodeapi,
    }, None


def self_test():
    """Cross-platform negative controls (no /bin/sh dependency)."""
    bad = 0

    def report(name, cond):
        print("self-test %-52s -> %s" % (name, "PASS" if cond else "FAIL"))
        return 0 if cond else 1

    tmp = tempfile.mkdtemp(prefix="check-smoke-win-selftest-")
    try:
        ch_exe = os.path.join(tmp, "ch.exe")
        ch_nodeapi = os.path.join(tmp, "ch-nodeapi.exe")
        snippet = os.path.join(tmp, "snippet.js")
        core_js = os.path.join(tmp, "core.js")
        for p in (ch_exe, ch_nodeapi):
            with open(p, "wb") as f:
                f.write(b"MZstub")
        with open(snippet, "w", encoding="utf-8") as f:
            f.write(SNIPPET)
        with open(core_js, "w", encoding="utf-8") as f:
            f.write(build_core_js("icu4c"))

        def fake_run(table):
            def _runner(cmd, timeout=common.CAPTURE_TIMEOUT, cwd=None,
                        merge_stderr=False):
                _ = timeout, cwd, merge_stderr
                key = os.path.basename(cmd[0]) + " " + " ".join(cmd[1:])
                for prefix, res in table:
                    if key.startswith(prefix):
                        return res
                return 0, "", ""
            return _runner

        # 1. Version checks.
        with mock.patch.object(
            common, "run_capture",
            side_effect=fake_run([("ch.exe -v", (0, "ch.exe version 1.13.0.0-beta\r\n", ""))])
        ):
            bad += report("ch.exe version text passes", check_version(ch_exe) is None)

        with mock.patch.object(
            common, "run_capture",
            side_effect=fake_run([("ch.exe -v", (0, "", ""))])
        ):
            bad += report("silent rc-0 fails version gate", check_version(ch_exe) is not None)

        with mock.patch.object(
            common, "run_capture",
            side_effect=fake_run([("ch.exe -v", (1, "ch.exe version 1.13.0\n", ""))])
        ):
            bad += report("nonzero rc fails version gate", check_version(ch_exe) is not None)

        # 2. Invalid flag profile contracts.
        flag_text = "Invalid Flag : %s\r\nUsage: ch.exe\r\n" % BOGUS_FLAG
        with mock.patch.object(
            common, "run_capture",
            side_effect=fake_run([("ch.exe " + BOGUS_FLAG, (5, flag_text, ""))])
        ):
            bad += report("rc-5 flag text passes test",
                          check_invalid_flag(ch_exe, snippet, "test") is None)
            bad += report("rc-5 flag text fails debug-static",
                          check_invalid_flag(ch_exe, snippet, "debug-static") is not None)
            bad += report("rc-5 flag text fails release-shared",
                          check_invalid_flag(ch_exe, snippet, "release-shared") is not None)

        with mock.patch.object(
            common, "run_capture",
            side_effect=fake_run([("ch.exe " + BOGUS_FLAG, (0, flag_text, ""))])
        ):
            bad += report("rc-0 flag text passes debug-static",
                          check_invalid_flag(ch_exe, snippet, "debug-static") is None)
            bad += report("rc-0 flag text fails test",
                          check_invalid_flag(ch_exe, snippet, "test") is not None)
            bad += report("rc-0 flag text fails release-shared",
                          check_invalid_flag(ch_exe, snippet, "release-shared") is not None)

        with mock.patch.object(
            common, "run_capture",
            side_effect=fake_run([("ch.exe " + BOGUS_FLAG, (0, "", ""))])
        ):
            bad += report("silent rc-0 passes release-shared",
                          check_invalid_flag(ch_exe, snippet, "release-shared") is None)
            bad += report("silent rc-0 fails test",
                          check_invalid_flag(ch_exe, snippet, "test") is not None)

        # 3. Snippet + Core JS + Hello checks (CRLF tolerant).
        with mock.patch.object(
            common, "run_capture",
            side_effect=fake_run([
                ("ch.exe " + snippet, (0, "zig-smoke-ok 2\r\n", "")),
                ("ch.exe " + core_js, (0, "windows-core-js-ok\r\n", "")),
                ("ch.exe hello.js", (0, "hello world\r\nPASS\r\n", "")),
            ])
        ):
            bad += report("CRLF snippet output passes", check_snippet(ch_exe, snippet) is None)
            bad += report("CRLF core-js output passes", check_core_js(ch_exe, core_js) is None)
            bad += report("CRLF hello.js output passes", check_hello(ch_exe, "hello.js") is None)

        with mock.patch.object(
            common, "run_capture",
            side_effect=fake_run([
                ("ch.exe " + core_js, (0, "CORE_JS_FAIL: Promise.allKeyed\r\n", "")),
                ("ch.exe hello.js", (0, "hello world\r\n", "")),
            ])
        ):
            bad += report("wrong core-js stdout fails", check_core_js(ch_exe, core_js) is not None)
            bad += report("incomplete hello.js stdout fails", check_hello(ch_exe, "hello.js") is not None)

        # 4. Core JS template Intl toggle.
        bad += report("icu4c sets __EXPECT_INTL__ true",
                      "else if (true)" in build_core_js("icu4c"))
        bad += report("icu=none sets __EXPECT_INTL__ false",
                      "else if (false)" in build_core_js("none"))

        # 5. ch-nodeapi --help and --version controls.
        with mock.patch.object(
            common, "run_capture",
            side_effect=fake_run([
                ("ch-nodeapi.exe --help", (0, "usage: ch-nodeapi [opts]\r\n", "")),
                ("ch-nodeapi.exe --version", (0, "ch-nodeapi 1.0.0\r\n", "")),
            ])
        ):
            bad += report("ch-nodeapi --help/--version passes",
                          check_ch_nodeapi(ch_nodeapi) is None)

        with mock.patch.object(
            common, "run_capture",
            side_effect=fake_run([
                ("ch-nodeapi.exe --help", (0, "usage: ch-nodeapi\r\n", "")),
                ("ch-nodeapi.exe --version", (1, "unknown option --version\r\n", "")),
            ])
        ):
            bad += report("ch-nodeapi failing --version fails",
                          check_ch_nodeapi(ch_nodeapi) is not None)

        # 6. CLI argument parsing controls.
        opts, err = parse_args(["zig-out", "--profile=release-shared",
                                "--icu", "windows", "--ch-nodeapi", ch_nodeapi])
        bad += report("parse_args accepts valid options",
                      err is None and opts == {
                          "target": "zig-out",
                          "profile": "release-shared",
                          "icu": "windows",
                          "ch_nodeapi": ch_nodeapi,
                      })
        bad += report("parse_args rejects invalid profile",
                      parse_args(["ch.exe", "--profile", "bogus"])[1] is not None)
        bad += report("parse_args rejects invalid icu",
                      parse_args(["ch.exe", "--icu", "bogus"])[1] is not None)

        # 7. Real subprocess timeout ceiling pin.
        try:
            common.run_capture(
                [sys.executable, "-c", "import time; time.sleep(30)"],
                timeout=1,
            )
            bad += report("hung probe raises TimeoutExpired", False)
        except subprocess.TimeoutExpired:
            bad += report("hung probe raises TimeoutExpired", True)
    finally:
        shutil.rmtree(tmp, ignore_errors=True)

    print("check_smoke_windows self-test: %s"
          % ("ALL NEGATIVE CONTROLS BEHAVE" if bad == 0
             else "%d CONTROL(S) MISBEHAVE" % bad))
    return 1 if bad else 0


def main(argv):
    args = list(argv[1:])
    if "--self-test" in args:
        return self_test()
    opts, err = parse_args(args)
    if err is not None:
        print("check_smoke_windows: %s" % err, file=sys.stderr)
        return 2

    target = opts["target"]
    profile = opts["profile"]
    icu_mode = opts["icu"]
    ch_nodeapi = opts["ch_nodeapi"]

    if os.path.isdir(target):
        cand_exe = os.path.join(target, "bin", "ch.exe")
        cand_posix = os.path.join(target, "bin", "ch")
        ch = cand_exe if os.path.isfile(cand_exe) else cand_posix
    else:
        ch = target

    if not os.path.isfile(ch):
        print("check_smoke_windows: missing ch executable: %s" % ch,
              file=sys.stderr)
        return 2

    explicit_nodeapi = ch_nodeapi is not None
    if ch_nodeapi is None and profile != "debug-static":
        bin_dir = os.path.dirname(os.path.abspath(ch))
        for cand_name in ("ch-nodeapi.exe", "ch-nodeapi"):
            cand = os.path.join(bin_dir, cand_name)
            if os.path.isfile(cand):
                ch_nodeapi = cand
                break
    if explicit_nodeapi and not os.path.isfile(ch_nodeapi):
        print("check_smoke_windows: missing ch-nodeapi executable: %s"
              % ch_nodeapi, file=sys.stderr)
        return 2

    hello_path = None
    if profile == "release-shared":
        root = os.path.dirname(os.path.dirname(
            os.path.dirname(os.path.abspath(__file__))))
        hello_path = os.path.join(root, HELLO_REL)
        if not os.path.isfile(hello_path):
            print("check_smoke_windows: missing file: %s" % hello_path,
                  file=sys.stderr)
            return 2

    tmp = tempfile.mkdtemp(prefix="check-smoke-windows-")
    try:
        snippet = os.path.join(tmp, "snippet.js")
        with open(snippet, "w", encoding="utf-8", newline="\n") as f:
            f.write(SNIPPET)
        core_js = os.path.join(tmp, "core_es.js")
        with open(core_js, "w", encoding="utf-8", newline="\n") as f:
            f.write(build_core_js(icu_mode))
        try:
            failures = [
                ("version", check_version(ch)),
                ("invalid-flag", check_invalid_flag(ch, snippet, profile)),
                ("snippet", check_snippet(ch, snippet)),
                ("core-js", check_core_js(ch, core_js)),
            ]
            if hello_path is not None:
                failures.append(("hello", check_hello(ch, hello_path)))
            if ch_nodeapi is not None:
                failures.append(("ch-nodeapi", check_ch_nodeapi(ch_nodeapi)))
            else:
                print("check_smoke_windows: ch-nodeapi not present beside ch "
                      "(skipping optional ch-nodeapi check)")
        except subprocess.TimeoutExpired as e:
            failures = [("timeout", "probe exceeded the %ds ceiling: %s"
                         % (e.timeout, " ".join(e.cmd)))]
    finally:
        shutil.rmtree(tmp, ignore_errors=True)

    bad = [(name, e) for name, e in failures if e is not None]
    if bad:
        print("check_smoke_windows: FAIL:", file=sys.stderr)
        for name, e in bad:
            print("check_smoke_windows: FAIL: [%s] %s" % (name, e),
                  file=sys.stderr)
        return 1
    print("check_smoke_windows: PASS: version + invalid-flag + snippet + "
          "core ES5/ES6/ES2025/ES2026%s verified"
          % (" + ch-nodeapi" if ch_nodeapi else ""))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
