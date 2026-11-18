#!/usr/bin/env python3
#-------------------------------------------------------------------------------------------------------
# Copyright (c) ChakraCore Project Contributors. All rights reserved.
# Licensed under the MIT license. See LICENSE.txt file in the project root for full license information.
#-------------------------------------------------------------------------------------------------------
"""Sanitizer verification gate for -Dsanitize=address|undefined|thread.

Exercises the sanitizer-instrumented engine across:
  - smoke      (ch -v, inline snippet, test/Basics/hello.js)
  - GC         (WeakRef, FinalizationRegistry, medium-block GC, GSAB GC)
  - RegExp     (non-writable lastIndex, v-flag, modifiers, lookbehind, UnifiedRegex)
  - TypedArray (OOB delete/BigInt locale, Float16Array, typedarray.at)
  - ES2025     (Set methods, Object.groupBy, BigInt exact formatting, templates, modules)
  - WASM       (if WebAssembly is compiled into ch)
  - Node-API   (ch-nodeapi + libChakraNodeApi addons, plus compiled refs_probe/wrap_probe)

Every child process runs with:
  ASAN_OPTIONS=detect_leaks=0
  UBSAN_OPTIONS=print_stacktrace=1:halt_on_error=1
  TSAN_OPTIONS=halt_on_error=1

Because bin/ch/ch.cpp exits via pthread_exit(&retval) under
NO_SANITIZE_ADDRESS_CHECK (which returns exit code 0 even when retval != 0),
every JS leg asserts BOTH rc == 0 AND exact expected stdout / baseline match
plus zero sanitizer diagnostics.

Usage: check_sanitize.py <prefix_or_ch> [--mode address|undefined|thread]
       [--cxx COMPILER] [--sdk-path SDK] [--timeout SECS] [--self-test]
Exit code: 0 iff every leg passes with zero sanitizer diagnostics.
"""

import os
import re
import shutil
import subprocess
import sys
import tempfile

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import common

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(HERE))

VALID_MODES = ("address", "undefined", "thread")
DEFAULT_TIMEOUT = 120.0

SANITIZER_PATTERNS = (
    re.compile(r"ERROR:\s*AddressSanitizer"),
    re.compile(r"AddressSanitizer:"),
    re.compile(r"ERROR:\s*LeakSanitizer"),
    re.compile(r"ERROR:\s*ThreadSanitizer"),
    re.compile(r"WARNING:\s*ThreadSanitizer"),
    re.compile(r"UndefinedBehaviorSanitizer"),
    re.compile(r"SUMMARY:\s*(?:Address|Leak|Thread|UndefinedBehavior)Sanitizer"),
    re.compile(r"(?:^|\n)[^\n]*:\d+:\d+:\s*runtime error:"),
    re.compile(r"^\s*runtime error:", re.MULTILINE),
    re.compile(r"AddressSanitizer:DEADLYSIGNAL"),
    re.compile(r"ASAN:DEADLYSIGNAL"),
)

NODEAPI_PROVIDER_TUS = (
    "lib/NodeApi/NodeApiEnv.cpp",
    "lib/NodeApi/NodeApiLifetime.cpp",
    "lib/NodeApi/NodeApiRefs.cpp",
    "lib/NodeApi/NodeApiScopes.cpp",
    "lib/NodeApi/NodeApiBinary.cpp",
    "lib/NodeApi/NodeApiPromise.cpp",
    "lib/NodeApi/NodeApiScript.cpp",
    "lib/NodeApi/NodeApiWrap.cpp",
    "lib/NodeApi/NodeApiAsyncContext.cpp",
    "lib/NodeApi/NodeApiObjects.cpp",
    "lib/NodeApi/NodeApiFunctions.cpp",
    "lib/NodeApi/NodeApiValues.cpp",
    "lib/NodeApi/NodeApiErrorsImpl.cpp",
    "lib/NodeApi/NodeApiBuffers.cpp",
    "lib/NodeApi/NodeApiAsync.cpp",
    "lib/NodeApi/NodeApiHost.cpp",
)

VERSION_RE = re.compile(r"^ch version \d+\.\d+")
_CRLF_RE = re.compile(r"\r+\n")


def find_sanitizer_diagnostic(text):
    """Return a matching diagnostic snippet if sanitizer output appears."""
    for pat in SANITIZER_PATTERNS:
        m = pat.search(text)
        if m:
            start = text.rfind("\n", 0, m.start()) + 1
            end = text.find("\n", m.end())
            if end == -1:
                end = len(text)
            return text[start:end].strip()[:200]
    return None


def sanitizer_env(mode=None, base_env=None):
    """Return process env configured for ASan/UBSan/TSan execution."""
    env = dict(os.environ if base_env is None else base_env)
    env["ASAN_OPTIONS"] = "detect_leaks=0"
    env["UBSAN_OPTIONS"] = "print_stacktrace=1:halt_on_error=1"
    if mode == "thread":
        env["TSAN_OPTIONS"] = "halt_on_error=1"
    env["TZ"] = "America/Los_Angeles"
    return env


def run_env_capture(cmd, env, cwd=None, timeout=DEFAULT_TIMEOUT):
    """Run cmd with merged stdout+stderr and clean process-group cleanup."""
    proc = subprocess.Popen(
        cmd,
        cwd=cwd,
        env=env,
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        start_new_session=(os.name == "posix"),
        universal_newlines=True,
        errors="replace",
    )
    try:
        out, _ = proc.communicate(timeout=timeout)
        return proc.returncode, out or ""
    except subprocess.TimeoutExpired:
        common._kill_group(proc)
        proc.communicate()
        raise
    except BaseException:
        common._kill_group(proc)
        proc.communicate()
        raise


def normalize_text(text):
    return _CRLF_RE.sub("\n", text)


def check_pass_lines(text):
    lines = [ln for ln in normalize_text(text).split("\n") if ln != ""]
    if not lines:
        return "empty stdout (expected 'pass')"
    for ln in lines:
        if ln.lower() not in ("pass", "passed"):
            return "unexpected output line %r (expected 'pass')" % ln[:120]
    return None


def check_exact_lines(text, want_lines):
    got = [ln for ln in normalize_text(text).split("\n")]
    if got and got[-1] == "":
        got.pop()
    if got != list(want_lines):
        return "stdout %r != expected %r" % (got[:8], list(want_lines))
    return None


def check_baseline(text, baseline_path):
    with open(baseline_path, "r", encoding="utf-8", errors="replace") as f:
        want = normalize_text(f.read())
    got = normalize_text(text)
    if got != want:
        for idx, (g, w) in enumerate(zip(got.splitlines(), want.splitlines()), 1):
            if g != w:
                return "baseline mismatch at line %d: got %r, want %r" % (
                    idx, g[:120], w[:120])
        return "baseline length mismatch (got %d chars, want %d)" % (
            len(got), len(want))
    return None


def run_checked(label, cmd, env, check_fn, cwd=None, timeout=DEFAULT_TIMEOUT):
    """Run one command, assert rc==0, no sanitizer diagnostic, and check_fn."""
    try:
        rc, out = run_env_capture(cmd, env, cwd=cwd, timeout=timeout)
    except subprocess.TimeoutExpired:
        return False, "[%s] timed out after %.1fs" % (label, timeout), ""
    diag = find_sanitizer_diagnostic(out)
    if diag is not None:
        return False, "[%s] sanitizer diagnostic: %s" % (label, diag), out
    if rc != 0:
        return False, "[%s] exited rc=%d" % (label, rc), out
    err = check_fn(out)
    if err is not None:
        return False, "[%s] %s" % (label, err), out
    return True, "[%s] PASS" % label, out


def resolve_prefix(prefix_or_ch):
    """Return (prefix_dir, ch_path) from either a prefix dir or .../bin/ch."""
    abs_arg = os.path.abspath(prefix_or_ch)
    if os.path.isdir(abs_arg):
        return abs_arg, os.path.join(abs_arg, "bin", "ch")
    bin_dir = os.path.dirname(abs_arg)
    prefix = (os.path.dirname(bin_dir)
              if os.path.basename(bin_dir) == "bin" else bin_dir)
    return prefix, abs_arg


def probe_wasm_supported(ch, env, tmp_dir):
    probe_js = os.path.join(tmp_dir, "wasm_probe.js")
    with open(probe_js, "w", encoding="utf-8") as f:
        f.write(
            "print(typeof WebAssembly);\n"
            "var b = new Uint8Array([0, 97, 115, 109, 1, 0, 0, 0]);\n"
            "print(WebAssembly.validate(b));\n"
        )
    try:
        rc, out = run_env_capture([ch, probe_js], env, timeout=15)
    except subprocess.TimeoutExpired:
        return False
    diag = find_sanitizer_diagnostic(out)
    if diag is not None or rc != 0:
        return False
    lines = [ln.strip() for ln in normalize_text(out).splitlines() if ln.strip()]
    return lines == ["object", "true"]


def run_probe_binary(label, probe_src, cxx, mode, engine_lib, sdk_path,
                     env, tmp_dir, root=ROOT):
    """Compile and run a Node-API C++ probe against engine_lib with sanitizer."""
    if mode not in VALID_MODES:
        return False, "[%s] invalid sanitizer mode %r" % (label, mode), ""
    out_dir = os.path.join(tmp_dir, label)
    os.makedirs(out_dir, exist_ok=True)
    out_bin = os.path.join(out_dir, label)
    lib_name = os.path.basename(engine_lib)
    shutil.copy2(engine_lib, os.path.join(out_dir, lib_name))

    cmd = [cxx]
    if sys.platform == "darwin":
        sdk = sdk_path
        if not sdk:
            rc_s, sdk_out = run_env_capture(["xcrun", "--show-sdk-path"], env)
            if rc_s != 0:
                return False, "[%s] xcrun --show-sdk-path failed" % label, sdk_out
            sdk = sdk_out.strip()
        cmd += ["-target", "arm64-apple-macos26.0", "-isysroot", sdk,
                "-mcpu=apple-m1"]
        link_sys = ["-framework", "CoreFoundation", "-framework", "Security",
                    "-Wl,-rpath,@executable_path"]
    else:
        cmd += ["-target", "x86_64-linux-gnu"]
        link_sys = ["-ldl", "-lpthread", "-Wl,-rpath,$ORIGIN"]

    cmd += [
        "-std=gnu++17", "-O1", "-g", "-fno-omit-frame-pointer",
        "-fsanitize=" + mode,
    ]
    if mode == "undefined":
        cmd += ["-fno-sanitize=enum,alignment,vptr,pointer-overflow"]
    cmd += [
        "-Wno-date-time", "-DNAPI_VERSION=10", "-DCHAKRA_NODEAPI_TEST_HOOKS",
        "-I" + os.path.join(root, "lib", "Jsrt"),
        "-I" + os.path.join(root, "include", "nodeapi"),
        "-I" + os.path.join(root, "lib", "NodeApi"),
        "-I" + os.path.join(root, "test", "nodeapi", "probes"),
    ]
    for tu in NODEAPI_PROVIDER_TUS:
        cmd.append(os.path.join(root, tu))
    cmd.append(os.path.join(root, probe_src))
    cmd.append(os.path.join(out_dir, lib_name))
    cmd += link_sys + ["-o", out_bin]

    rc_c, out_c = run_env_capture(cmd, env, timeout=DEFAULT_TIMEOUT)
    if rc_c != 0:
        return False, "[%s] compile failed rc=%d" % (label, rc_c), out_c

    return run_checked(
        label, [out_bin], env,
        lambda txt: None if ("PASS" in txt and "FAIL" not in txt)
        else "missing PASS / unexpected FAIL in probe output",
        cwd=out_dir,
    )


def run_all(prefix_or_ch, mode="address", cxx=None, sdk_path=None,
            timeout=DEFAULT_TIMEOUT, root=ROOT):
    """Execute the full check-sanitize matrix. Returns 0 iff all legs pass."""
    prefix, ch = resolve_prefix(prefix_or_ch)
    if not os.path.isfile(ch):
        print("check_sanitize: missing binary: %s" % ch, file=sys.stderr)
        return 2

    env = sanitizer_env(mode=mode)
    tmp_dir = tempfile.mkdtemp(prefix="check-sanitize-")
    try:
        legs = []

        # 1. Smoke
        legs.append(("smoke:version", [ch, "-v"], None,
                     lambda txt: None if VERSION_RE.search(txt)
                     else "missing ch version line"))
        snippet_js = os.path.join(tmp_dir, "smoke.js")
        with open(snippet_js, "w", encoding="utf-8") as f:
            f.write('print("sanitize-smoke-ok " + (1 + 1));\n')
        legs.append(("smoke:snippet", [ch, snippet_js], None,
                     lambda txt: check_exact_lines(txt, ["sanitize-smoke-ok 2"])))
        legs.append(("smoke:hello", [ch, os.path.join(root, "test", "Basics", "hello.js")],
                     os.path.join(root, "test", "Basics"),
                     lambda txt: check_exact_lines(txt, ["hello world", "PASS"])))

        # 2. GC
        legs.extend([
            ("gc:weakref",
             [ch, "-CollectGarbage", "weakref.js", "-args", "summary", "-endargs"],
             os.path.join(root, "test", "es7"), check_pass_lines),
            ("gc:weakref-gc",
             [ch, "-CollectGarbage", "weakref-gc.js"],
             os.path.join(root, "test", "es7"), check_pass_lines),
            ("gc:cdjs-mediumblock",
             [ch, "cdjs-mediumblock-interior-gc.js"],
             os.path.join(root, "test", "Bugs"), check_pass_lines),
            ("gc:gsab-cross-agent",
             [ch, "-ESBigInt", "-ESSharedArrayBuffer", "-Test262", "-CollectGarbage",
              "s6-growable-cross-agent.js"],
             os.path.join(root, "test", "es7"), check_pass_lines),
        ])

        # 3. RegExp
        legs.extend([
            ("regexp:nonwritable-lastindex",
             [ch, "regex-nonwritable-lastindex-jit.js"],
             os.path.join(root, "test", "es7"), check_pass_lines),
            ("regexp:v-flag",
             [ch, "regexp-v-flag.js", "-args", "summary", "-endargs"],
             os.path.join(root, "test", "es7"), check_pass_lines),
            ("regexp:modifiers",
             [ch, "regexp-modifiers.js", "-args", "summary", "-endargs"],
             os.path.join(root, "test", "es7"), check_pass_lines),
            ("regexp:lookbehind",
             [ch, "-CollectGarbage", "regexp-lookbehind.js", "-args", "summary", "-endargs"],
             os.path.join(root, "test", "es7"), check_pass_lines),
            ("regexp:unified-acid",
             [ch, "acid.js"],
             os.path.join(root, "test", "UnifiedRegex"),
             lambda txt: check_baseline(
                 txt, os.path.join(root, "test", "UnifiedRegex", "acid.baseline"))),
        ])

        # 4. TypedArray
        legs.extend([
            ("typedarray:oob-delete-bigint-locale",
             [ch, "typedarray-oob-delete-and-bigint-locale.js", "-args", "summary", "-endargs"],
             os.path.join(root, "test", "es7"), check_pass_lines),
            ("typedarray:float16array",
             [ch, "float16array.js", "-args", "summary", "-endargs"],
             os.path.join(root, "test", "es7"), check_pass_lines),
            ("typedarray:float16-bounds",
             [ch, "-ESBigInt", "float16-jit-bounds.js"],
             os.path.join(root, "test", "es7"), check_pass_lines),
            ("typedarray:at",
             [ch, "typedarray_at.js", "-args", "summary", "-endargs"],
             os.path.join(root, "test", "typedarray"), check_pass_lines),
        ])

        # 5. ES2025
        legs.extend([
            ("es2025:setmethods",
             [ch, "setmethods.js", "-args", "summary", "-endargs"],
             os.path.join(root, "test", "es7"), check_pass_lines),
            ("es2025:groupby-warmed",
             [ch, "setmethods-groupby-warmed.js", "-args", "summary", "-endargs"],
             os.path.join(root, "test", "es7"), check_pass_lines),
            ("es2025:number-formatting-exact",
             [ch, "-ESBigInt", "number-formatting-exact.js", "-args", "summary", "-endargs"],
             os.path.join(root, "test", "es7"), check_pass_lines),
            ("es2025:tagged-template-cooked",
             [ch, "taggedTemplateCookedUndefined.js", "-args", "summary", "-endargs"],
             os.path.join(root, "test", "es6"), check_pass_lines),
            ("es2025:module-cycletdz",
             [ch, "module-cycletdz.js"],
             os.path.join(root, "test", "es6module"),
             lambda txt: check_baseline(
                 txt, os.path.join(root, "test", "es6module", "module-cycletdz.baseline"))),
        ])

        # 6. WASM (conditional on runtime support)
        if probe_wasm_supported(ch, env, tmp_dir):
            wasm_js = os.path.join(tmp_dir, "wasm_smoke.js")
            with open(wasm_js, "w", encoding="utf-8") as f:
                f.write(
                    "var mod = new WebAssembly.Module(new Uint8Array(["
                    "0x00,0x61,0x73,0x6d,0x01,0x00,0x00,0x00,"
                    "0x01,0x07,0x01,0x60,0x02,0x7f,0x7f,0x01,0x7f,"
                    "0x03,0x02,0x01,0x00,"
                    "0x07,0x07,0x01,0x03,0x61,0x64,0x64,0x00,0x00,"
                    "0x0a,0x09,0x01,0x07,0x00,0x20,0x00,0x20,0x01,0x6a,0x0b"
                    "]));\n"
                    "var inst = new WebAssembly.Instance(mod);\n"
                    "if (inst.exports.add(20, 22) !== 42) throw new Error('bad');\n"
                    "print('PASS');\n"
                )
            legs.append(("wasm:smoke", [ch, wasm_js], None, check_pass_lines))
            legs.append(("wasm:polyinline",
                         [ch, "-maxinterpretcount:2", "-off:simplejit", "polyinline.js"],
                         os.path.join(root, "test", "wasm"), check_pass_lines))
        else:
            print("check_sanitize: WASM skipped (WebAssembly disabled in this build)")

        # 7. Node-API (ch-nodeapi + compiled refs_probe / wrap_probe)
        ch_nodeapi = os.path.join(prefix, "bin", "ch-nodeapi")
        ext = "dylib" if sys.platform == "darwin" else "so"
        prov_lib = os.path.join(prefix, "lib", "libChakraNodeApi." + ext)
        addons_dir = os.path.join(prefix, "lib", "nodeapi-addons")
        if os.path.isfile(ch_nodeapi):
            prov_args = ["--provider", prov_lib] if os.path.isfile(prov_lib) else []
            for name, want in (
                ("hello_world", ["hello: world"]),
                ("promise_probe", ["promise: done:12"]),
                ("return_replaces", ["replaced: yes"]),
            ):
                addon = os.path.join(addons_dir, name + ".node")
                driver = os.path.join(addons_dir, name + ".js")
                if os.path.isfile(addon) and os.path.isfile(driver):
                    legs.append((
                        "nodeapi:" + name,
                        [ch_nodeapi] + prov_args + ["--addon", addon, "--driver", driver],
                        None,
                        lambda txt, w=want: check_exact_lines(txt, w),
                    ))

        failures = []
        passed = 0
        for label, cmd, cwd, check_fn in legs:
            ok, msg, out = run_checked(
                label, cmd, env, check_fn, cwd=cwd, timeout=timeout)
            print("check_sanitize: %s" % msg)
            sys.stdout.flush()
            if ok:
                passed += 1
            else:
                failures.append((msg, out))

        engine_lib = os.path.join(prefix, "bin", "libChakraCore." + ext)
        if cxx and os.path.isfile(engine_lib):
            probe_specs = [
                ("nodeapi:refs_probe", "test/nodeapi/probes/refs_probe.cpp"),
                ("nodeapi:wrap_probe", "test/nodeapi/probes/wrap_probe.cpp"),
            ]
            v10_probe = "test/nodeapi/probes/v10_multi_addon_probe.cpp"
            if os.path.isfile(os.path.join(root, v10_probe)):
                probe_specs.append(("nodeapi:v10_multi_addon_probe", v10_probe))
            for label, src in probe_specs:
                ok, msg, out = run_probe_binary(
                    label, src, cxx, mode, engine_lib, sdk_path,
                    env, tmp_dir, root=root)
                print("check_sanitize: %s" % msg)
                sys.stdout.flush()
                if ok:
                    passed += 1
                else:
                    failures.append((msg, out))

        if failures:
            print("check_sanitize: FAIL (%d/%d leg(s) failed):"
                  % (len(failures), passed + len(failures)), file=sys.stderr)
            for msg, out in failures:
                print("  %s" % msg, file=sys.stderr)
                for ln in out.splitlines()[:25]:
                    print("    | " + ln, file=sys.stderr)
            return 1

        print("check_sanitize: PASS (%d/%d legs clean under -Dsanitize=%s)"
              % (passed, passed, mode))
        return 0
    finally:
        shutil.rmtree(tmp_dir, ignore_errors=True)


def self_test():
    """Positive and negative controls for sanitizer parsing and verdict logic."""
    bad = 0

    def report(name, cond, detail=""):
        print("self-test %-46s -> %s%s"
              % (name, "PASS" if cond else "FAIL",
                 (" (" + detail + ")") if detail else ""))
        return 0 if cond else 1

    # 1. Sanitizer diagnostic detection.
    bad += report(
        "detects ASan heap-use-after-free",
        find_sanitizer_diagnostic(
            "==123==ERROR: AddressSanitizer: heap-use-after-free on address 0x1") is not None,
    )
    bad += report(
        "detects UBSan runtime error",
        find_sanitizer_diagnostic(
            "lib/Runtime/Foo.cpp:42:5: runtime error: member call on null pointer") is not None,
    )
    bad += report(
        "detects TSan data race",
        find_sanitizer_diagnostic(
            "WARNING: ThreadSanitizer: data race (pid=123)") is not None,
    )
    bad += report(
        "clean output yields no diagnostic",
        find_sanitizer_diagnostic("hello world\nPASS\n") is None,
    )

    # 2. Environment variables.
    env = sanitizer_env("undefined")
    bad += report(
        "env sets ASAN_OPTIONS and UBSAN_OPTIONS",
        env.get("ASAN_OPTIONS") == "detect_leaks=0"
        and env.get("UBSAN_OPTIONS") == "print_stacktrace=1:halt_on_error=1",
    )

    # 3. Subprocess checks (even with rc == 0 if diagnostic or bad output).
    if os.name == "posix":
        with tempfile.TemporaryDirectory(prefix="check-sanitize-selftest-") as tmp:
            def write_stub(name, body):
                p = os.path.join(tmp, name)
                with open(p, "w", encoding="utf-8") as f:
                    f.write("#!/usr/bin/env python3\n" + body)
                os.chmod(p, 0o755)
                return p

            good = write_stub("good", "print('pass')\n")
            ok, msg, _ = run_checked("good", [good], env, check_pass_lines)
            bad += report("clean pass output succeeds", ok, msg)

            diag_rc0 = write_stub(
                "diag_rc0",
                "import sys\n"
                "print('pass')\n"
                "print('lib/Foo.cpp:10:3: runtime error: signed integer overflow', file=sys.stderr)\n",
            )
            ok, msg, _ = run_checked("diag_rc0", [diag_rc0], env, check_pass_lines)
            bad += report("sanitizer diagnostic with rc=0 fails", not ok, msg)

            # Simulates ch.cpp pthread_exit(&retval) returning 0 on error.
            wrong_rc0 = write_stub(
                "wrong_rc0",
                "print('Error: unhandled failure')\n",
            )
            ok, msg, _ = run_checked("wrong_rc0", [wrong_rc0], env, check_pass_lines)
            bad += report("wrong stdout with rc=0 fails", not ok, msg)

            nonzero = write_stub(
                "nonzero",
                "import sys\nprint('pass')\nsys.exit(1)\n",
            )
            ok, msg, _ = run_checked("nonzero", [nonzero], env, check_pass_lines)
            bad += report("nonzero exit with 'pass' fails", not ok, msg)

            hang = write_stub("hang", "import time\ntime.sleep(10)\n")
            ok, msg, _ = run_checked("hang", [hang], env, check_pass_lines, timeout=0.4)
            bad += report("hung child fails on timeout", not ok, msg)

    print("check_sanitize self-test: %s"
          % ("ALL NEGATIVE CONTROLS BEHAVE" if bad == 0
             else "%d CONTROL(S) MISBEHAVE" % bad))
    return 1 if bad else 0


def main(argv):
    args = list(argv[1:])
    if "--self-test" in args:
        return self_test()

    mode = "address"
    cxx = None
    sdk_path = None
    timeout = DEFAULT_TIMEOUT
    rest = []
    i = 0
    while i < len(args):
        if args[i] == "--mode":
            if i + 1 >= len(args):
                print("check_sanitize: --mode needs a value", file=sys.stderr)
                return 2
            mode = args[i + 1]
            i += 2
        elif args[i] == "--cxx":
            if i + 1 >= len(args):
                print("check_sanitize: --cxx needs a value", file=sys.stderr)
                return 2
            cxx = args[i + 1]
            i += 2
        elif args[i] == "--sdk-path":
            if i + 1 >= len(args):
                print("check_sanitize: --sdk-path needs a value", file=sys.stderr)
                return 2
            sdk_path = args[i + 1]
            i += 2
        elif args[i] == "--timeout":
            if i + 1 >= len(args):
                print("check_sanitize: --timeout needs a value", file=sys.stderr)
                return 2
            timeout = float(args[i + 1])
            i += 2
        else:
            rest.append(args[i])
            i += 1

    if mode not in VALID_MODES:
        print("check_sanitize: bad --mode %r: expected %s"
              % (mode, ", ".join(VALID_MODES)), file=sys.stderr)
        return 2
    if len(rest) != 1:
        print("usage: check_sanitize.py <prefix_or_ch> "
              "[--mode address|undefined|thread] [--cxx COMPILER] "
              "[--sdk-path SDK] [--timeout SECS] [--self-test]", file=sys.stderr)
        return 2

    return run_all(
        rest[0], mode=mode, cxx=cxx, sdk_path=sdk_path, timeout=timeout,
    )


if __name__ == "__main__":
    sys.exit(main(sys.argv))
