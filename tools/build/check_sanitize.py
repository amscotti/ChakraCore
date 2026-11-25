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
  - WASM       (when enabled, or required via --expect-wasm)
  - Node-API   (ch-nodeapi runtime, libChakraNodeApi addons, stub provider,
                negative addons, and compiled C++ probes on shared profiles)

Every child process runs with:
  ASAN_OPTIONS=detect_leaks=0
  UBSAN_OPTIONS=print_stacktrace=1:halt_on_error=1
  TSAN_OPTIONS=halt_on_error=1

Because bin/ch/ch.cpp exits via pthread_exit(&retval) under
NO_SANITIZE_ADDRESS_CHECK (which returns exit code 0 even when retval != 0),
every JS leg asserts BOTH rc == 0 AND exact expected stdout / baseline match
plus zero sanitizer diagnostics.

Expected legs are deterministically planned from (profile, expect_wasm,
host_platform, cxx); missing binaries/addons/probes and sanitizer diagnostics in
the WASM probe fail hard instead of passing vacuously.

Usage: check_sanitize.py <prefix_or_ch> [mode] [--mode address|undefined|thread]
       [--profile NAME] [--expect-wasm | --wasm | --no-expect-wasm]
       [--expect-legs N] [--cxx COMPILER] [--sdk-path SDK] [--timeout SECS]
       [--self-test]
Exit code: 0 iff every expected leg runs and passes with zero sanitizer diagnostics.
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
SHARED_PROFILES = frozenset({
    "test", "jit", "release-shared", "release-jit",
})
STATIC_PROFILES = frozenset({"debug-static", "jit-debug"})
ALL_PROFILES = SHARED_PROFILES | STATIC_PROFILES
# Probe compilation and execution timeout. Slow 4 vCPU hosts can need more
# than 120 s to compile Node-API probe addons under clang++ sanitizer
# instrumentation; callers can override via --timeout.
DEFAULT_TIMEOUT = 600.0

CORE_LEG_NAMES = (
    "smoke:version",
    "smoke:snippet",
    "smoke:hello",
    "gc:weakref",
    "gc:weakref-gc",
    "gc:cdjs-mediumblock",
    "gc:gsab-cross-agent",
    "regexp:nonwritable-lastindex",
    "regexp:v-flag",
    "regexp:modifiers",
    "regexp:lookbehind",
    "regexp:unified-acid",
    "typedarray:oob-delete-bigint-locale",
    "typedarray:float16array",
    "typedarray:float16-bounds",
    "typedarray:at",
    "es2025:setmethods",
    "es2025:groupby-warmed",
    "es2025:number-formatting-exact",
    "es2025:tagged-template-cooked",
    "es2025:module-cycletdz",
)
WASM_LEG_NAMES = ("wasm:smoke", "wasm:polyinline")
NODEAPI_RUNTIME_LEG_NAMES = ("nodeapi:version", "nodeapi:help")
NODEAPI_ADDON_SPECS = (
    ("hello_world", ["hello: world"]),
    ("promise_probe", ["promise: done:12"]),
    ("return_replaces", ["replaced: yes"]),
)
NODEAPI_STUB_LEG_NAME = "nodeapi:stub_provider"
NODEAPI_NEGATIVE_SPECS = (
    ("neg_missing_register", "missing_register",
     "napi_register_module_v1", None),
    ("neg_version_reject", "version_reject",
     "N-API v11", "version_reject register called (BUG)"),
)
NODEAPI_PROBE_SPECS = (
    ("nodeapi:refs_probe", "test/nodeapi/probes/refs_probe.cpp"),
    ("nodeapi:wrap_probe", "test/nodeapi/probes/wrap_probe.cpp"),
    ("nodeapi:v10_multi_addon_probe", "test/nodeapi/probes/v10_multi_addon_probe.cpp"),
)


class WasmProbeError(RuntimeError):
    """Raised when the WASM capability probe crashes, hangs, or hits a sanitizer."""

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


def run_checked(label, cmd, env, check_fn, cwd=None, timeout=DEFAULT_TIMEOUT,
                expect_nonzero=False):
    """Run one command, assert rc, no sanitizer diagnostic, and check_fn."""
    try:
        rc, out = run_env_capture(cmd, env, cwd=cwd, timeout=timeout)
    except subprocess.TimeoutExpired:
        return False, "[%s] timed out after %.1fs" % (label, timeout), ""
    except OSError as e:
        return False, "[%s] failed to execute: %s" % (label, e), ""
    diag = find_sanitizer_diagnostic(out)
    if diag is not None:
        return False, "[%s] sanitizer diagnostic: %s" % (label, diag), out
    if expect_nonzero:
        if rc == 0:
            return False, "[%s] expected nonzero exit, got rc=0" % label, out
        if rc < 0 or rc >= 128:
            return False, "[%s] crashed with exit/signal rc=%d" % (label, rc), out
    elif rc != 0:
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


def normalize_profile(profile=None, prefix=None):
    """Resolve base profile name from --profile or <prefix>/.profile."""
    raw = profile
    if raw is None and prefix is not None:
        marker = os.path.join(prefix, ".profile")
        if os.path.isfile(marker):
            with open(marker, "r", encoding="utf-8", errors="replace") as f:
                text = f.read().strip()
            if text:
                raw = text
    if raw is None:
        raw = "test"
    if raw in ALL_PROFILES:
        return raw
    if raw.startswith("sanitize-") and raw[len("sanitize-"):] in VALID_MODES:
        return "test"
    if "-sanitize-" in raw:
        base, san = raw.rsplit("-sanitize-", 1)
        if base in ALL_PROFILES and san in VALID_MODES:
            return base
    raise ValueError(
        "bad --profile %r: expected one of %s (or <base>-sanitize-<mode>)"
        % (raw, ", ".join(sorted(ALL_PROFILES)))
    )


def expected_leg_names(profile, expect_wasm=False, has_cxx=False,
                       host_platform=None):
    """Build deterministic expected leg list for (profile, expect_wasm, host_platform)."""
    _plat = sys.platform if host_platform is None else host_platform
    base_profile = normalize_profile(profile)
    legs = list(CORE_LEG_NAMES)
    if expect_wasm:
        legs.extend(WASM_LEG_NAMES)
    if base_profile in SHARED_PROFILES:
        legs.extend(NODEAPI_RUNTIME_LEG_NAMES)
        for name, _ in NODEAPI_ADDON_SPECS:
            legs.append("nodeapi:" + name)
        legs.append(NODEAPI_STUB_LEG_NAME)
        for leg_suffix, _, _, _ in NODEAPI_NEGATIVE_SPECS:
            legs.append("nodeapi:" + leg_suffix)
        if has_cxx:
            for label, _ in NODEAPI_PROBE_SPECS:
                legs.append(label)
    return legs


def probe_wasm_supported(ch, env, tmp_dir):
    """Return True iff WebAssembly works, False iff genuinely disabled.

    Raises WasmProbeError on any sanitizer diagnostic, timeout, crash, or
    unexpected output so memory errors can never masquerade as disabled WASM.
    """
    probe_js = os.path.join(tmp_dir, "wasm_probe.js")
    with open(probe_js, "w", encoding="utf-8") as f:
        f.write(
            "print(typeof WebAssembly);\n"
            "if (typeof WebAssembly !== 'object') {\n"
            "  print('wasm-disabled');\n"
            "} else {\n"
            "  var b = new Uint8Array([0, 97, 115, 109, 1, 0, 0, 0]);\n"
            "  print(WebAssembly.validate(b));\n"
            "}\n"
        )
    try:
        rc, out = run_env_capture([ch, probe_js], env, timeout=15)
    except subprocess.TimeoutExpired:
        raise WasmProbeError("wasm probe timed out after 15.0s")
    except OSError as e:
        raise WasmProbeError("wasm probe failed to execute %s: %s" % (ch, e))
    diag = find_sanitizer_diagnostic(out)
    if diag is not None:
        raise WasmProbeError("sanitizer diagnostic in wasm probe: %s" % diag)
    if rc < 0 or rc >= 128:
        raise WasmProbeError("wasm probe crashed with exit/signal rc=%d" % rc)
    lines = [ln.strip() for ln in normalize_text(out).splitlines() if ln.strip()]
    if rc == 0:
        if lines == ["object", "true"]:
            return True
        if lines and lines[0] == "undefined":
            return False
        raise WasmProbeError(
            "unexpected wasm probe output (rc=0): %r" % out[:200]
        )
    if (lines and lines[0] == "undefined") or "webassembly" in out.lower():
        return False
    raise WasmProbeError(
        "wasm probe exited rc=%d without disabled-WASM output: %r"
        % (rc, out[:200])
    )


def run_probe_binary(label, probe_src, cxx, mode, engine_lib, sdk_path,
                     env, tmp_dir, root=ROOT, timeout=DEFAULT_TIMEOUT):
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

    rc_c, out_c = run_env_capture(cmd, env, timeout=timeout)
    if rc_c != 0:
        return False, "[%s] compile failed rc=%d" % (label, rc_c), out_c

    return run_checked(
        label, [out_bin], env,
        lambda txt: None if ("PASS" in txt and "FAIL" not in txt)
        else "missing PASS / unexpected FAIL in probe output",
        cwd=out_dir, timeout=timeout,
    )


def grade_verdict(passed, expected_total, failures, missing_legs, mode):
    """Print and grade the overall verdict against expected_total."""
    if missing_legs:
        print("check_sanitize: missing expected leg(s) (%d): %s"
              % (len(missing_legs), ", ".join(missing_legs)), file=sys.stderr)
    if (failures or missing_legs or expected_total <= 0
            or passed != expected_total):
        print("check_sanitize: FAIL (%d/%d leg(s) passed, %d failed, "
              "%d missing under -Dsanitize=%s):"
              % (passed, expected_total, len(failures),
                 len(missing_legs), mode),
              file=sys.stderr)
        for msg, out in failures:
            print("  %s" % msg, file=sys.stderr)
            for ln in out.splitlines()[:25]:
                print("    | " + ln, file=sys.stderr)
        return 1

    print("check_sanitize: PASS (%d/%d legs clean under -Dsanitize=%s)"
          % (passed, expected_total, mode))
    return 0


def _check_negative_addon(out, want_substr, forbid_substr):
    if want_substr and want_substr not in out:
        return "missing expected rejection text %r" % want_substr
    if forbid_substr and forbid_substr in out:
        return "unexpected text %r (addon init should not run)" % forbid_substr
    return None


def _resolve_cxx(cxx):
    if not isinstance(cxx, str) or not cxx.strip():
        return None
    if os.path.isfile(cxx):
        return cxx
    return shutil.which(cxx)


def _resolve_engine_lib(prefix, ext):
    for sub in ("bin", "lib"):
        cand = os.path.join(prefix, sub, "libChakraCore." + ext)
        if os.path.isfile(cand):
            return cand
    return None


def run_all(prefix_or_ch, mode="address", cxx=None, sdk_path=None,
            timeout=DEFAULT_TIMEOUT, root=ROOT, profile=None,
            expect_wasm=None, expect_legs=None, host_platform=None):
    """Execute the full check-sanitize matrix. Returns 0 iff all expected legs pass."""
    if mode not in VALID_MODES:
        print("check_sanitize: bad --mode %r: expected %s"
              % (mode, ", ".join(VALID_MODES)), file=sys.stderr)
        return 2
    prefix, ch = resolve_prefix(prefix_or_ch)
    try:
        base_profile = normalize_profile(profile, prefix=prefix)
    except ValueError as e:
        print("check_sanitize: %s" % e, file=sys.stderr)
        return 2
    if not os.path.isfile(ch):
        print("check_sanitize: missing binary: %s" % ch, file=sys.stderr)
        return 2

    plat = sys.platform if host_platform is None else host_platform
    cxx_raw = cxx.strip() if isinstance(cxx, str) and cxx.strip() else None
    has_cxx = cxx_raw is not None
    is_shared = base_profile in SHARED_PROFILES

    env = sanitizer_env(mode=mode)
    tmp_dir = tempfile.mkdtemp(prefix="check-sanitize-")
    try:
        failures = []
        missing_legs = []
        passed = 0

        wasm_probe_err = None
        try:
            wasm_supported = probe_wasm_supported(ch, env, tmp_dir)
        except WasmProbeError as e:
            wasm_supported = False
            wasm_probe_err = str(e)

        if wasm_probe_err is not None:
            wasm_active = bool(expect_wasm is True)
        elif wasm_supported:
            wasm_active = True
        elif expect_wasm is True:
            wasm_active = True
            wasm_probe_err = ("--expect-wasm set, but WebAssembly is "
                              "disabled in this build")
        else:
            print("check_sanitize: WASM skipped (WebAssembly disabled in this build)")
            wasm_active = False

        expected_legs = expected_leg_names(
            base_profile,
            expect_wasm=wasm_active,
            has_cxx=has_cxx,
            host_platform=plat,
        )
        expected_total = len(expected_legs)
        if wasm_probe_err is not None and not wasm_active:
            expected_total += 1
        if expect_legs is not None:
            if expected_total != expect_legs:
                failures.append((
                    "[plan] expected %d legs (--expect-legs), planned %d"
                    % (expect_legs, expected_total),
                    "",
                ))
            expected_total = max(expected_total, expect_legs)

        print("check_sanitize: profile=%s mode=%s expect_wasm=%s expected_legs=%d"
              % (base_profile, mode, wasm_active, expected_total))
        sys.stdout.flush()

        if wasm_probe_err is not None:
            msg = "[wasm:probe] %s" % wasm_probe_err
            print("check_sanitize: FAIL %s" % msg)
            failures.append((msg, ""))

        snippet_js = os.path.join(tmp_dir, "smoke.js")
        with open(snippet_js, "w", encoding="utf-8") as f:
            f.write('print("sanitize-smoke-ok " + (1 + 1));\n')

        core_specs = [
            # 1. Smoke
            ("smoke:version", [ch, "-v"], None, [],
             lambda txt: None if VERSION_RE.search(txt)
             else "missing ch version line"),
            ("smoke:snippet", [ch, snippet_js], None, [snippet_js],
             lambda txt: check_exact_lines(txt, ["sanitize-smoke-ok 2"])),
            ("smoke:hello", [ch, os.path.join(root, "test", "Basics", "hello.js")],
             os.path.join(root, "test", "Basics"),
             [os.path.join(root, "test", "Basics", "hello.js")],
             lambda txt: check_exact_lines(txt, ["hello world", "PASS"])),
            # 2. GC
            ("gc:weakref",
             [ch, "-CollectGarbage", "weakref.js", "-args", "summary", "-endargs"],
             os.path.join(root, "test", "es7"),
             [os.path.join(root, "test", "es7", "weakref.js")], check_pass_lines),
            ("gc:weakref-gc",
             [ch, "-CollectGarbage", "weakref-gc.js"],
             os.path.join(root, "test", "es7"),
             [os.path.join(root, "test", "es7", "weakref-gc.js")], check_pass_lines),
            ("gc:cdjs-mediumblock",
             [ch, "cdjs-mediumblock-interior-gc.js"],
             os.path.join(root, "test", "Bugs"),
             [os.path.join(root, "test", "Bugs", "cdjs-mediumblock-interior-gc.js")],
             check_pass_lines),
            ("gc:gsab-cross-agent",
             [ch, "-ESBigInt", "-ESSharedArrayBuffer", "-Test262",
              "-CollectGarbage", "s6-growable-cross-agent.js"],
             os.path.join(root, "test", "es7"),
             [os.path.join(root, "test", "es7", "s6-growable-cross-agent.js")],
             check_pass_lines),
            # 3. RegExp
            ("regexp:nonwritable-lastindex",
             [ch, "regex-nonwritable-lastindex-jit.js"],
             os.path.join(root, "test", "es7"),
             [os.path.join(root, "test", "es7", "regex-nonwritable-lastindex-jit.js")],
             check_pass_lines),
            ("regexp:v-flag",
             [ch, "regexp-v-flag.js", "-args", "summary", "-endargs"],
             os.path.join(root, "test", "es7"),
             [os.path.join(root, "test", "es7", "regexp-v-flag.js")], check_pass_lines),
            ("regexp:modifiers",
             [ch, "regexp-modifiers.js", "-args", "summary", "-endargs"],
             os.path.join(root, "test", "es7"),
             [os.path.join(root, "test", "es7", "regexp-modifiers.js")], check_pass_lines),
            ("regexp:lookbehind",
             [ch, "-CollectGarbage", "regexp-lookbehind.js",
              "-args", "summary", "-endargs"],
             os.path.join(root, "test", "es7"),
             [os.path.join(root, "test", "es7", "regexp-lookbehind.js")],
             check_pass_lines),
            ("regexp:unified-acid",
             [ch, "acid.js"],
             os.path.join(root, "test", "UnifiedRegex"),
             [os.path.join(root, "test", "UnifiedRegex", "acid.js"),
              os.path.join(root, "test", "UnifiedRegex", "acid.baseline")],
             lambda txt: check_baseline(
                 txt, os.path.join(root, "test", "UnifiedRegex", "acid.baseline"))),
            # 4. TypedArray
            ("typedarray:oob-delete-bigint-locale",
             [ch, "typedarray-oob-delete-and-bigint-locale.js",
              "-args", "summary", "-endargs"],
             os.path.join(root, "test", "es7"),
             [os.path.join(root, "test", "es7",
                           "typedarray-oob-delete-and-bigint-locale.js")],
             check_pass_lines),
            ("typedarray:float16array",
             [ch, "float16array.js", "-args", "summary", "-endargs"],
             os.path.join(root, "test", "es7"),
             [os.path.join(root, "test", "es7", "float16array.js")], check_pass_lines),
            ("typedarray:float16-bounds",
             [ch, "-ESBigInt", "float16-jit-bounds.js"],
             os.path.join(root, "test", "es7"),
             [os.path.join(root, "test", "es7", "float16-jit-bounds.js")],
             check_pass_lines),
            ("typedarray:at",
             [ch, "typedarray_at.js", "-args", "summary", "-endargs"],
             os.path.join(root, "test", "typedarray"),
             [os.path.join(root, "test", "typedarray", "typedarray_at.js")],
             check_pass_lines),
            # 5. ES2025
            ("es2025:setmethods",
             [ch, "setmethods.js", "-args", "summary", "-endargs"],
             os.path.join(root, "test", "es7"),
             [os.path.join(root, "test", "es7", "setmethods.js")], check_pass_lines),
            ("es2025:groupby-warmed",
             [ch, "setmethods-groupby-warmed.js", "-args", "summary", "-endargs"],
             os.path.join(root, "test", "es7"),
             [os.path.join(root, "test", "es7", "setmethods-groupby-warmed.js")],
             check_pass_lines),
            ("es2025:number-formatting-exact",
             [ch, "-ESBigInt", "number-formatting-exact.js",
              "-args", "summary", "-endargs"],
             os.path.join(root, "test", "es7"),
             [os.path.join(root, "test", "es7", "number-formatting-exact.js")],
             check_pass_lines),
            ("es2025:tagged-template-cooked",
             [ch, "taggedTemplateCookedUndefined.js",
              "-args", "summary", "-endargs"],
             os.path.join(root, "test", "es6"),
             [os.path.join(root, "test", "es6", "taggedTemplateCookedUndefined.js")],
             check_pass_lines),
            ("es2025:module-cycletdz",
             [ch, "module-cycletdz.js"],
             os.path.join(root, "test", "es6module"),
             [os.path.join(root, "test", "es6module", "module-cycletdz.js"),
              os.path.join(root, "test", "es6module", "module-cycletdz.baseline")],
             lambda txt: check_baseline(
                 txt, os.path.join(root, "test", "es6module", "module-cycletdz.baseline"))),
        ]

        # 6. WASM
        if wasm_active:
            if wasm_probe_err is not None:
                for label in WASM_LEG_NAMES:
                    missing_legs.append(label)
            else:
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
                core_specs.append((
                    "wasm:smoke", [ch, wasm_js], None, [wasm_js], check_pass_lines))
                core_specs.append((
                    "wasm:polyinline",
                    [ch, "-maxinterpretcount:2", "-off:simplejit", "polyinline.js"],
                    os.path.join(root, "test", "wasm"),
                    [os.path.join(root, "test", "wasm", "polyinline.js")],
                    check_pass_lines))

        # 7. Node-API (shared profiles: runtime, addons, stub provider, negative addons)
        exec_specs = list(core_specs)
        if is_shared:
            ext = "dylib" if plat == "darwin" else "so"
            ch_nodeapi = os.path.join(prefix, "bin", "ch-nodeapi")
            prov_lib = os.path.join(prefix, "lib", "libChakraNodeApi." + ext)
            stub_lib = os.path.join(prefix, "lib", "libChakraNodeApiStub." + ext)
            addons_dir = os.path.join(prefix, "lib", "nodeapi-addons")

            exec_specs.append((
                "nodeapi:version",
                [ch_nodeapi, "--version"],
                None,
                [ch_nodeapi],
                lambda txt: None if "ch-nodeapi" in txt
                else "missing 'ch-nodeapi' in --version output",
            ))
            exec_specs.append((
                "nodeapi:help",
                [ch_nodeapi, "--help"],
                None,
                [ch_nodeapi],
                lambda txt: None if "usage: ch-nodeapi" in txt.lower()
                else "missing 'usage: ch-nodeapi' in --help output",
            ))
            for name, want in NODEAPI_ADDON_SPECS:
                addon = os.path.join(addons_dir, name + ".node")
                driver = os.path.join(addons_dir, name + ".js")
                exec_specs.append((
                    "nodeapi:" + name,
                    [ch_nodeapi, "--provider", prov_lib,
                     "--addon", addon, "--driver", driver],
                    None,
                    [ch_nodeapi, prov_lib, addon, driver],
                    lambda txt, w=want: check_exact_lines(txt, w),
                ))
            hello_addon = os.path.join(addons_dir, "hello_world.node")
            hello_driver = os.path.join(addons_dir, "hello_world.js")
            exec_specs.append((
                NODEAPI_STUB_LEG_NAME,
                [ch_nodeapi, "--provider", stub_lib,
                 "--addon", hello_addon, "--driver", hello_driver],
                None,
                [ch_nodeapi, stub_lib, hello_addon, hello_driver],
                lambda txt: check_exact_lines(txt, ["hello: world"]),
            ))
            for leg_suffix, addon_name, want_sub, forbid_sub in NODEAPI_NEGATIVE_SPECS:
                neg_addon = os.path.join(addons_dir, addon_name + ".node")
                exec_specs.append((
                    "nodeapi:" + leg_suffix,
                    [ch_nodeapi, "--provider", prov_lib, "--addon", neg_addon],
                    None,
                    [ch_nodeapi, prov_lib, neg_addon],
                    lambda txt, ws=want_sub, fs=forbid_sub: _check_negative_addon(
                        txt, ws, fs),
                    True,
                ))

        for spec in exec_specs:
            if len(spec) == 5:
                label, cmd, cwd, req_files, check_fn = spec
                expect_nonzero = False
            else:
                label, cmd, cwd, req_files, check_fn, expect_nonzero = spec
            absent = [p for p in req_files if not os.path.isfile(p)]
            if absent:
                missing_legs.append(label)
                msg = "[%s] missing required file(s): %s" % (
                    label, ", ".join(absent))
                print("check_sanitize: FAIL %s" % msg)
                failures.append((msg, ""))
                continue
            ok, msg, out = run_checked(
                label, cmd, env, check_fn, cwd=cwd, timeout=timeout,
                expect_nonzero=expect_nonzero)
            print("check_sanitize: %s" % msg)
            sys.stdout.flush()
            if ok:
                passed += 1
            else:
                failures.append((msg, out))

        if is_shared and has_cxx:
            ext = "dylib" if plat == "darwin" else "so"
            engine_lib = _resolve_engine_lib(prefix, ext)
            cxx_bin = _resolve_cxx(cxx_raw)
            for label, src in NODEAPI_PROBE_SPECS:
                src_abs = os.path.join(root, src)
                missing_items = []
                if engine_lib is None:
                    missing_items.append(
                        os.path.join(prefix, "bin", "libChakraCore." + ext))
                if cxx_bin is None:
                    missing_items.append("cxx:%s" % cxx_raw)
                if not os.path.isfile(src_abs):
                    missing_items.append(src_abs)
                if missing_items:
                    missing_legs.append(label)
                    msg = "[%s] missing required probe dependency: %s" % (
                        label, ", ".join(missing_items))
                    print("check_sanitize: FAIL %s" % msg)
                    failures.append((msg, ""))
                    continue
                ok, msg, out = run_probe_binary(
                    label, src, cxx_bin, mode, engine_lib, sdk_path,
                    env, tmp_dir, root=root, timeout=timeout)
                print("check_sanitize: %s" % msg)
                sys.stdout.flush()
                if ok:
                    passed += 1
                else:
                    failures.append((msg, out))

        return grade_verdict(
            passed, expected_total, failures, missing_legs, mode)
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

            # 4. WASM probe negative & positive controls (B-3).
            wasm_ok = write_stub("wasm_ok", "print('object')\nprint('true')\n")
            bad += report("wasm probe accepts object/true",
                          probe_wasm_supported(wasm_ok, env, tmp) is True)

            wasm_off = write_stub("wasm_off", "print('undefined')\nprint('wasm-disabled')\n")
            bad += report("wasm probe returns False for disabled WASM (rc=0)",
                          probe_wasm_supported(wasm_off, env, tmp) is False)

            wasm_off_nz = write_stub(
                "wasm_off_nz",
                "import sys\n"
                "print('undefined')\n"
                "print(\"ReferenceError: 'WebAssembly' is not defined\")\n"
                "sys.exit(1)\n",
            )
            bad += report("wasm probe returns False for disabled WASM (rc=1)",
                          probe_wasm_supported(wasm_off_nz, env, tmp) is False)

            for label_asan, body in (
                ("asan_rc1", "import sys\nprint('==123==ERROR: AddressSanitizer: heap-buffer-overflow', file=sys.stderr)\nsys.exit(1)\n"),
                ("asan_rc0", "print('undefined')\nprint('==123==ERROR: AddressSanitizer: use-after-free')\n"),
                ("ubsan", "import sys\nprint('lib/Wasm/Foo.cpp:5:2: runtime error: load of null pointer', file=sys.stderr)\nsys.exit(1)\n"),
                ("crash", "import os, signal\nos.kill(os.getpid(), signal.SIGSEGV)\n"),
            ):
                stub_bad = write_stub("wasm_" + label_asan, body)
                try:
                    probe_wasm_supported(stub_bad, env, tmp)
                    bad += report("wasm probe fails hard on " + label_asan, False)
                except WasmProbeError:
                    bad += report("wasm probe fails hard on " + label_asan, True)

            # 5. Expected-leg planning & verdict controls (B-3).
            bad += report(
                "expected_leg_names deterministic counts",
                len(expected_leg_names("debug-static", False, True)) == 21
                and len(expected_leg_names("test", False, False)) == 29
                and len(expected_leg_names("test", False, True)) == 32
                and len(expected_leg_names("jit-sanitize-address", True, True)) == 34,
            )
            bad += report(
                "truncated leg list cannot pass (21/29)",
                grade_verdict(21, 29, [], [], "address") == 1,
            )
            bad += report(
                "missing_legs cannot pass (28/29)",
                grade_verdict(28, 29, [], ["nodeapi:stub_provider"], "address") == 1,
            )

            # 6. End-to-end hermetic run_all negative & positive controls.
            fake_root = os.path.join(tmp, "repo")
            for rel in (
                "test/Basics/hello.js",
                "test/es7/weakref.js",
                "test/es7/weakref-gc.js",
                "test/Bugs/cdjs-mediumblock-interior-gc.js",
                "test/es7/s6-growable-cross-agent.js",
                "test/es7/regex-nonwritable-lastindex-jit.js",
                "test/es7/regexp-v-flag.js",
                "test/es7/regexp-modifiers.js",
                "test/es7/regexp-lookbehind.js",
                "test/UnifiedRegex/acid.js",
                "test/UnifiedRegex/acid.baseline",
                "test/es7/typedarray-oob-delete-and-bigint-locale.js",
                "test/es7/float16array.js",
                "test/es7/float16-jit-bounds.js",
                "test/typedarray/typedarray_at.js",
                "test/es7/setmethods.js",
                "test/es7/setmethods-groupby-warmed.js",
                "test/es7/number-formatting-exact.js",
                "test/es6/taggedTemplateCookedUndefined.js",
                "test/es6module/module-cycletdz.js",
                "test/es6module/module-cycletdz.baseline",
                "test/wasm/polyinline.js",
                "test/nodeapi/probes/refs_probe.cpp",
                "test/nodeapi/probes/wrap_probe.cpp",
                "test/nodeapi/probes/v10_multi_addon_probe.cpp",
            ):
                p = os.path.join(fake_root, rel)
                os.makedirs(os.path.dirname(p), exist_ok=True)
                with open(p, "w", encoding="utf-8") as f:
                    f.write("PASS\n")

            ext = "dylib" if sys.platform == "darwin" else "so"
            fake_pfx = os.path.join(tmp, "pfx")
            os.makedirs(os.path.join(fake_pfx, "bin"), exist_ok=True)
            os.makedirs(os.path.join(fake_pfx, "lib", "nodeapi-addons"), exist_ok=True)

            stub_ch = os.path.join(fake_pfx, "bin", "ch")
            with open(stub_ch, "w", encoding="utf-8") as f:
                f.write(
                    "#!/usr/bin/env python3\n"
                    "import os, sys\n"
                    "args = sys.argv[1:]\n"
                    "if '-v' in args:\n"
                    "    print('ch version 1.13.0.0-beta')\n"
                    "    sys.exit(0)\n"
                    "target = args[-1]\n"
                    "if os.path.basename(target) == 'wasm_probe.js':\n"
                    "    mode = os.environ.get('FAKE_WASM_MODE', 'disabled')\n"
                    "    if mode == 'asan':\n"
                    "        print('==123==ERROR: AddressSanitizer: heap-buffer-overflow', file=sys.stderr)\n"
                    "        sys.exit(1)\n"
                    "    elif mode == 'enabled':\n"
                    "        print('object')\n"
                    "        print('true')\n"
                    "        sys.exit(0)\n"
                    "    else:\n"
                    "        print('undefined')\n"
                    "        print('wasm-disabled')\n"
                    "        sys.exit(0)\n"
                    "if os.path.basename(target) == 'smoke.js':\n"
                    "    print('sanitize-smoke-ok 2')\n"
                    "elif os.path.basename(target) == 'hello.js':\n"
                    "    print('hello world')\n"
                    "    print('PASS')\n"
                    "else:\n"
                    "    print('PASS')\n"
                )
            os.chmod(stub_ch, 0o755)

            stub_na = os.path.join(fake_pfx, "bin", "ch-nodeapi")
            with open(stub_na, "w", encoding="utf-8") as f:
                f.write(
                    "#!/usr/bin/env python3\n"
                    "import os, sys\n"
                    "args = sys.argv[1:]\n"
                    "if '--version' in args:\n"
                    "    print('ch-nodeapi 1.0.0')\n"
                    "    sys.exit(0)\n"
                    "if '--help' in args:\n"
                    "    print('usage: ch-nodeapi [--provider PATH]')\n"
                    "    sys.exit(0)\n"
                    "addon = args[args.index('--addon') + 1]\n"
                    "base = os.path.basename(addon)\n"
                    "if base == 'hello_world.node':\n"
                    "    print('hello: world')\n"
                    "elif base == 'promise_probe.node':\n"
                    "    print('promise: done:12')\n"
                    "elif base == 'return_replaces.node':\n"
                    "    print('replaced: yes')\n"
                    "elif base == 'missing_register.node':\n"
                    "    print('lacks napi_register_module_v1', file=sys.stderr)\n"
                    "    sys.exit(1)\n"
                    "elif base == 'version_reject.node':\n"
                    "    print('requests N-API v11 > v10', file=sys.stderr)\n"
                    "    sys.exit(1)\n"
                )
            os.chmod(stub_na, 0o755)

            for libname in ("libChakraNodeApi." + ext, "libChakraNodeApiStub." + ext):
                with open(os.path.join(fake_pfx, "lib", libname), "wb") as f:
                    f.write(b"stub")
            for name in ("hello_world", "promise_probe", "return_replaces"):
                for suf in (".node", ".js"):
                    with open(os.path.join(fake_pfx, "lib", "nodeapi-addons", name + suf), "wb") as f:
                        f.write(b"stub")
            for name in ("missing_register", "version_reject"):
                with open(os.path.join(fake_pfx, "lib", "nodeapi-addons", name + ".node"), "wb") as f:
                    f.write(b"stub")

            bad += report(
                "run_all debug-static passes 21/21",
                run_all(fake_pfx, mode="address", profile="debug-static", root=fake_root) == 0,
            )
            bad += report(
                "run_all shared test passes 29/29",
                run_all(fake_pfx, mode="address", profile="test", root=fake_root) == 0,
            )
            old_wasm = os.environ.get("FAKE_WASM_MODE")
            try:
                os.environ["FAKE_WASM_MODE"] = "enabled"
                bad += report(
                    "run_all jit --expect-wasm passes 31/31",
                    run_all(fake_pfx, mode="address", profile="jit",
                            expect_wasm=True, root=fake_root) == 0,
                )
                os.environ["FAKE_WASM_MODE"] = "disabled"
                bad += report(
                    "run_all --expect-wasm fails when WASM disabled",
                    run_all(fake_pfx, mode="address", profile="jit",
                            expect_wasm=True, root=fake_root) == 1,
                )
                os.environ["FAKE_WASM_MODE"] = "asan"
                bad += report(
                    "run_all fails hard when WASM probe emits ASan error",
                    run_all(fake_pfx, mode="address", profile="test",
                            root=fake_root) == 1,
                )
            finally:
                if old_wasm is None:
                    os.environ.pop("FAKE_WASM_MODE", None)
                else:
                    os.environ["FAKE_WASM_MODE"] = old_wasm

            # Missing engine_lib with --cxx fails loudly instead of skipping probes.
            bad += report(
                "run_all fails when --cxx set and engine_lib missing",
                run_all(fake_pfx, mode="address", profile="test",
                        cxx=sys.executable, root=fake_root) == 1,
            )
            # Missing negative addon fails loudly instead of skipping.
            neg_path = os.path.join(fake_pfx, "lib", "nodeapi-addons", "version_reject.node")
            os.remove(neg_path)
            bad += report(
                "run_all fails when expected addon missing (28/29)",
                run_all(fake_pfx, mode="address", profile="test", root=fake_root) == 1,
            )

    print("check_sanitize self-test: %s"
          % ("ALL NEGATIVE CONTROLS BEHAVE" if bad == 0
             else "%d CONTROL(S) MISBEHAVE" % bad))
    return 1 if bad else 0


def main(argv):
    args = list(argv[1:])
    if "--self-test" in args:
        return self_test()

    mode = None
    profile = None
    expect_wasm = None
    expect_legs = None
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
        elif args[i] == "--profile":
            if i + 1 >= len(args):
                print("check_sanitize: --profile needs a value", file=sys.stderr)
                return 2
            profile = args[i + 1]
            i += 2
        elif args[i] in ("--expect-wasm", "--wasm"):
            if expect_wasm is False:
                print("check_sanitize: cannot combine --expect-wasm and --no-expect-wasm",
                      file=sys.stderr)
                return 2
            expect_wasm = True
            i += 1
        elif args[i] == "--no-expect-wasm":
            if expect_wasm is True:
                print("check_sanitize: cannot combine --expect-wasm and --no-expect-wasm",
                      file=sys.stderr)
                return 2
            expect_wasm = False
            i += 1
        elif args[i] == "--expect-legs":
            if i + 1 >= len(args):
                print("check_sanitize: --expect-legs needs a value", file=sys.stderr)
                return 2
            try:
                expect_legs = int(args[i + 1])
            except ValueError:
                print("check_sanitize: bad --expect-legs %r: expected positive int"
                      % args[i + 1], file=sys.stderr)
                return 2
            if expect_legs <= 0:
                print("check_sanitize: bad --expect-legs %d: must be > 0"
                      % expect_legs, file=sys.stderr)
                return 2
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
        elif args[i].startswith("--"):
            print("check_sanitize: unknown option %r" % args[i], file=sys.stderr)
            return 2
        else:
            rest.append(args[i])
            i += 1

    if len(rest) == 2:
        if mode is not None and mode != rest[1]:
            print("check_sanitize: conflicting positional mode %r and --mode %r"
                  % (rest[1], mode), file=sys.stderr)
            return 2
        mode = rest[1]
    elif len(rest) != 1:
        print("usage: check_sanitize.py <prefix_or_ch> [mode] "
              "[--mode address|undefined|thread] [--profile NAME] "
              "[--expect-wasm|--wasm|--no-expect-wasm] [--expect-legs N] "
              "[--cxx COMPILER] [--sdk-path SDK] [--timeout SECS] [--self-test]",
              file=sys.stderr)
        return 2

    if mode is None:
        mode = "address"
    if mode not in VALID_MODES:
        print("check_sanitize: bad --mode %r: expected %s"
              % (mode, ", ".join(VALID_MODES)), file=sys.stderr)
        return 2

    return run_all(
        rest[0], mode=mode, cxx=cxx, sdk_path=sdk_path, timeout=timeout,
        profile=profile, expect_wasm=expect_wasm, expect_legs=expect_legs,
    )


if __name__ == "__main__":
    sys.exit(main(sys.argv))
