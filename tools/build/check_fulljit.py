#!/usr/bin/env python3
#-------------------------------------------------------------------------------------------------------
# Copyright (c) ChakraCore Project Contributors. All rights reserved.
# Licensed under the MIT license. See LICENSE.txt file in the project root for full license information.
#-------------------------------------------------------------------------------------------------------
"""FullJit-without-dynamic-profile gate (open-cards.md B4-JIT-05).

Runs representative ChakraCore regression suites (default:
Basics Array Function Regex es6 es7) directly via `ch` under FullJit
flags:

    -ExtendedErrorStackForTestHost -BaselineMode
    -forceNative -off:simpleJit -bgJitDelay:0

Why direct `ch` invocation instead of `runtests.py --variants=dynapogo`:
`test/runtests.py` unconditionally appends
`-dynamicprofileinput:profile.dpl.${id}` to the `dynapogo` variant
command line (even when `--extra-flags` is supplied), which both runs the
profile-consuming path and forces DynamicProfileStorage::Initialize into
20 x 100 ms retries (2 seconds wasted per test) when no `.dpl` files exist.
Driving `ch` directly from `rlexe.xml` exercises the genuine no-profile
FullJit execution path cleanly.

Usage: check_fulljit.py <runtests_or_test_root> <ch>
       [--suites S1 S2 ...] [--select test|debug] [--expected-total N]
       [--jobs N] [--timeout SECS] [--self-test]
Exit code: 0 iff NativeCodeGenerator + flaglist + BackEnd trace are present,
>0 tests run, every executed test passes, and passed matches expected_total
(single-sourced from tools/build/gate_counts.json for default suites).
"""

from concurrent.futures import ThreadPoolExecutor, as_completed
import json
import os
import platform
import re
import shutil
import subprocess
import sys
import tempfile
from unittest import mock
import xml.etree.ElementTree as ET

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import check_jit
import check_metadata_windows
import common

DEFAULT_SUITES = ("Basics", "Array", "Function", "Regex", "es6", "es7")
FULLJIT_FLAGS = (
    "-ExtendedErrorStackForTestHost",
    "-BaselineMode",
    "-WERExceptionSupport",
    "-forceNative",
    "-off:simpleJit",
    "-bgJitDelay:0",
)
DEFAULT_TIMEOUT = 180.0
GATE_COUNTS_PATH = os.path.join(
    os.path.dirname(os.path.abspath(__file__)), "gate_counts.json"
)

_CRLF_RE = re.compile(b"[\r]+\n")


def _validate_pin_entry(entry, path, label):
    if (not isinstance(entry, dict)
            or not isinstance(entry.get("passed"), int)
            or not isinstance(entry.get("failed"), int)
            or isinstance(entry.get("passed"), bool)
            or isinstance(entry.get("failed"), bool)
            or entry["passed"] < 0 or entry["failed"] < 0):
        print("check_fulljit: FAIL: gate counts %s missing/invalid %r pin: "
              "need {\"passed\": int >= 0, \"failed\": int >= 0}"
              % (path, label), file=sys.stderr)
        raise SystemExit(2)
    return entry["passed"], entry["failed"]


def _load_fulljit_pins(path=GATE_COUNTS_PATH):
    try:
        with open(path, "r", encoding="utf-8") as f:
            data = json.load(f)
    except OSError as e:
        print("check_fulljit: FAIL: cannot read gate counts %s: %s"
              % (path, e), file=sys.stderr)
        raise SystemExit(2)
    except ValueError as e:
        print("check_fulljit: FAIL: malformed gate counts %s: %s"
              % (path, e), file=sys.stderr)
        raise SystemExit(2)
    if not isinstance(data, dict):
        print("check_fulljit: FAIL: malformed gate counts %s: root must be an object"
              % path, file=sys.stderr)
        raise SystemExit(2)
    pins = {"test": _validate_pin_entry(data.get("fulljit"), path, "fulljit")}
    variants = data.get("fulljit_variants")
    if not isinstance(variants, dict):
        print("check_fulljit: FAIL: gate counts %s missing/invalid "
              "'fulljit_variants' map" % path, file=sys.stderr)
        raise SystemExit(2)
    for key in ("debug", "win32_test", "win32_debug"):
        pins[key] = _validate_pin_entry(
            variants.get(key), path, "fulljit_variants." + key)
    return pins


FULLJIT_PINS = _load_fulljit_pins()


def default_expected_counts(suites=DEFAULT_SUITES, select="test", plat=None,
                            pins=FULLJIT_PINS):
    """Return (expected_passed, expected_failed) for default suites, else (None, 0)."""
    if tuple(suites) != DEFAULT_SUITES:
        return None, 0
    plat = sys.platform if plat is None else plat
    if plat == "win32":
        key = "win32_debug" if select == "debug" else "win32_test"
    else:
        key = "debug" if select == "debug" else "test"
    return pins[key]


def normalize_newlines(data):
    """Mirror runtests.py normalize_new_line for binary baseline compares."""
    return _CRLF_RE.sub(b"\n", data)


def split_tags(text):
    if not text:
        return set()
    return {x.strip().lower() for x in text.split(",") if x.strip()}


def resolve_platform_tags(select="test", plat=None, machine=None):
    """Return (arch, arch_alias, not_tags) matching runtests.py dynapogo."""
    plat = sys.platform if plat is None else plat
    machine = platform.machine() if machine is None else machine
    is_darwin_arm64 = (plat == "darwin" and machine in ("arm64", "aarch64"))
    arch = "arm64" if is_darwin_arm64 else "x64"
    arch_alias = "amd64" if arch == "x64" else None
    flavor = "debug" if select == "debug" else "test"

    not_tags = {
        "fail",
        "slow",
        "exclude_" + arch,
        "exclude_" + flavor,
        "exclude_dynapogo",
        "fails_dynapogo",
        "require_disable_jit",
    }
    if arch_alias:
        not_tags.add("exclude_" + arch_alias)
    if plat == "win32":
        not_tags.add("exclude_windows")
    else:
        if plat == "darwin":
            not_tags.add("exclude_mac")
        if plat.startswith("linux"):
            not_tags.add("exclude_linux")
        if not is_darwin_arm64:
            not_tags.update(("exclude_xplat", "require_simd"))
    return arch, arch_alias, not_tags


def load_folder_tags(test_root):
    """Load folder tags from <test_root>/rlexedirs.xml (lowercase keys)."""
    xmlpath = os.path.join(test_root, "rlexedirs.xml")
    if not os.path.isfile(xmlpath):
        return None
    root = ET.parse(xmlpath).getroot()
    folder_tags = {}
    for x in root:
        d = x.find("default")
        if d is None:
            continue
        files_node = d.find("files")
        if files_node is None or not files_node.text:
            continue
        key = files_node.text.strip().lower()
        t_node = d.find("tags")
        folder_tags[key] = split_tags(t_node.text if t_node is not None else "")
    return folder_tags


def parse_rlexe_suite(suite_dir, arch, arch_alias):
    """Parse <suite_dir>/rlexe.xml into normalized test entry dicts."""
    xmlpath = os.path.join(suite_dir, "rlexe.xml")
    root = ET.parse(xmlpath).getroot()
    entries = []
    for test_xml in root:
        default = test_xml.find("default")
        if default is None:
            continue
        entry = {
            "folder": suite_dir,
            "files": "",
            "compile-flags": "",
            "baseline": "",
            "tags": "",
            "timeout": None,
            "custom-config-file": None,
        }
        for c in default:
            val = (c.text or "").strip()
            if c.tag == "timeout":
                entry["timeout"] = int(val)
            elif c.tag == "tags" and entry["tags"]:
                entry["tags"] = entry["tags"] + "," + val
            else:
                entry[c.tag] = val
        for cond in test_xml.findall("condition"):
            target = cond.find("target")
            tval = (target.text or "").strip() if target is not None else ""
            if tval and (tval == arch_alias or tval == arch):
                ov = cond.find("override")
                if ov is not None:
                    for item in ov:
                        ival = (item.text or "").strip()
                        if item.tag == "timeout":
                            entry["timeout"] = int(ival)
                        else:
                            entry[item.tag] = ival
        entries.append(entry)
    return entries


def should_run_test(entry, not_tags):
    tags = split_tags(entry.get("tags", ""))
    return tags.isdisjoint(not_tags)


def build_test_cmd(abs_ch, entry):
    """Build no-profile FullJit argv for one rlexe entry."""
    if entry.get("custom-config-file"):
        flags = ["-CustomConfigFile:" + entry["custom-config-file"]]
    else:
        extra = (entry.get("compile-flags") or "").split()
        flags = list(FULLJIT_FLAGS) + extra
    return [abs_ch] + flags + [entry["files"]]


def resolve_case_file(folder, filename):
    path = os.path.join(folder, filename)
    if os.path.isfile(path):
        return path
    target_lower = filename.lower()
    for name in os.listdir(folder):
        if name.lower() == target_lower:
            return os.path.join(folder, name)
    return path


def grade_test_output(rc, norm_output, baseline_bytes=None):
    """Return None on pass, or a concise failure string."""
    if rc != 0:
        return "exit code %d" % rc
    if baseline_bytes is not None:
        want = normalize_newlines(baseline_bytes)
        if norm_output != want:
            got_lines = norm_output.split(b"\n")
            want_lines = want.split(b"\n")
            for idx, (g, w) in enumerate(zip(got_lines, want_lines), 1):
                if g != w:
                    return ("baseline mismatch at line %d: got %r, want %r"
                            % (idx, g.decode("utf-8", "replace")[:120],
                               w.decode("utf-8", "replace")[:120]))
            return ("baseline length mismatch: got %d lines, want %d"
                    % (len(got_lines), len(want_lines)))
        return None
    passes = 0
    for line in norm_output.split(b"\n"):
        if line != b"":
            low = line.lower()
            if low in (b"pass", b"passed"):
                passes = 1
            else:
                return ("unexpected output line %r"
                        % line.decode("utf-8", "replace")[:120])
    if passes == 0:
        return "empty output (expected 'pass')"
    return None


def verify_jit_binary(ch):
    """Ensure ch carries NativeCodeGenerator, flaglist, and BackEnd trace."""
    err = check_jit.check_symbols(ch)
    if err is not None:
        return err
    if not check_jit.flaglist_supported(ch):
        return "%s does not support the test/debug flaglist (Release build?)" % ch
    err = check_jit.check_backend_trace(ch)
    if err is not None:
        return err
    return None


def _run_one(abs_ch, entry, default_timeout, env):
    folder = entry["folder"]
    js_path = resolve_case_file(folder, entry["files"])
    if not os.path.isfile(js_path):
        return False, entry, "missing test file %s" % js_path, ""
    cmd = build_test_cmd(abs_ch, entry)
    cmd[-1] = os.path.basename(js_path)
    timeout = float(entry["timeout"]) if entry["timeout"] is not None else default_timeout

    proc = subprocess.Popen(
        cmd,
        cwd=folder,
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        env=env,
        start_new_session=(os.name == "posix"),
    )
    timed_out = False
    try:
        raw_out, _ = proc.communicate(timeout=timeout)
        rc = proc.returncode
    except subprocess.TimeoutExpired:
        timed_out = True
        common._kill_group(proc)
        raw_out, _ = proc.communicate()
        rc = 124
    except BaseException:
        common._kill_group(proc)
        proc.communicate()
        raise

    norm_out = normalize_newlines(raw_out or b"")
    if timed_out:
        return (False, entry, "timed out after %.1fs" % timeout,
                norm_out.decode("utf-8", "replace"))

    baseline_bytes = None
    if entry.get("baseline"):
        bpath = resolve_case_file(folder, entry["baseline"])
        try:
            with open(bpath, "rb") as bf:
                baseline_bytes = bf.read()
        except OSError as e:
            return False, entry, "cannot read baseline %s: %s" % (bpath, e), ""

    err = grade_test_output(rc, norm_out, baseline_bytes)
    if err is not None:
        return (False, entry, err, norm_out.decode("utf-8", "replace"))
    return True, entry, None, ""


def run_suites(test_root, ch, suites=DEFAULT_SUITES, select="test",
               jobs=None, timeout=DEFAULT_TIMEOUT, skip_binary_probe=False,
               expected_total=None, expected_failed=0):
    """Run no-profile FullJit across suites. Returns (rc, summary_dict)."""
    abs_ch = os.path.abspath(ch)
    abs_root = os.path.abspath(test_root)
    if expected_total is None and tuple(suites) == DEFAULT_SUITES:
        expected_total, expected_failed = default_expected_counts(
            suites=suites, select=select)

    if not skip_binary_probe:
        probe_err = verify_jit_binary(abs_ch)
        if probe_err is not None:
            print("check_fulljit: FAIL: %s" % probe_err, file=sys.stderr)
            return 1, {"passed": 0, "failed": 0, "selected": 0,
                       "skipped_suites": [], "error": probe_err}

    arch, arch_alias, not_tags = resolve_platform_tags(select=select)
    folder_tags = load_folder_tags(abs_root)

    parallel_tests = []
    sequential_tests = []
    per_suite_selected = {}
    skipped_suites = []
    for suite in suites:
        suite_dir = os.path.join(abs_root, suite)
        if not os.path.isfile(os.path.join(suite_dir, "rlexe.xml")):
            msg = "missing suite rlexe.xml: %s" % suite_dir
            print("check_fulljit: FAIL: %s" % msg, file=sys.stderr)
            return 2, {"passed": 0, "failed": 0, "selected": 0,
                       "skipped_suites": [s for s, _ in skipped_suites],
                       "error": msg}
        ftags = (folder_tags.get(suite.lower(), set())
                 if folder_tags is not None else set())
        if not ftags.isdisjoint(not_tags):
            matched = sorted(ftags & not_tags)
            skipped_suites.append((suite, matched))
            per_suite_selected[suite] = 0
            print("check_fulljit: SKIP suite %s (matched excluded tags: %s)"
                  % (suite, ", ".join(matched)))
            continue
        entries = parse_rlexe_suite(suite_dir, arch, arch_alias)
        selected = [e for e in entries if should_run_test(e, not_tags)]
        per_suite_selected[suite] = len(selected)
        if "sequential" in ftags:
            sequential_tests.extend(selected)
        else:
            parallel_tests.extend(selected)

    skipped_names = [s for s, _ in skipped_suites]
    total_selected = len(parallel_tests) + len(sequential_tests)
    sel_detail = ", ".join("%s=%d" % (s, per_suite_selected[s]) for s in suites)
    exp_str = str(expected_total) if expected_total is not None else "unpinned"
    print("check_fulljit: selection [select=%s arch=%s]: %s (total=%d, expected=%s)"
          % (select, arch, sel_detail, total_selected, exp_str))

    if total_selected == 0:
        msg = "zero tests selected for suites=%r" % (list(suites),)
        print("check_fulljit: FAIL: %s" % msg, file=sys.stderr)
        return 1, {"passed": 0, "failed": 0, "selected": 0,
                   "skipped_suites": skipped_names, "error": msg}

    if jobs is None:
        jobs = min(os.cpu_count() or 4, 8)
    env = dict(os.environ)
    env["TZ"] = "America/Los_Angeles"

    print("check_fulljit: running %d tests across %s (jobs=%d, no-profile FullJit)"
          % (total_selected, ", ".join(suites), jobs))
    sys.stdout.flush()

    per_suite = {s: [0, 0] for s in suites}
    failures = []

    def record(ok, entry, reason, out_text):
        sname = os.path.basename(entry["folder"])
        if ok:
            per_suite[sname][0] += 1
        else:
            per_suite[sname][1] += 1
            failures.append((sname + "/" + entry["files"], reason, out_text))

    if parallel_tests:
        with ThreadPoolExecutor(max_workers=jobs) as pool:
            futs = [pool.submit(_run_one, abs_ch, e, timeout, env)
                    for e in parallel_tests]
            for fut in as_completed(futs):
                ok, entry, reason, out_text = fut.result()
                record(ok, entry, reason, out_text)

    for entry in sequential_tests:
        ok, e, reason, out_text = _run_one(abs_ch, entry, timeout, env)
        record(ok, e, reason, out_text)

    passed = sum(v[0] for v in per_suite.values())
    failed = sum(v[1] for v in per_suite.values())
    for s in suites:
        sp, sf = per_suite[s]
        print("  %s: passed %d, failed %d (selected %d)"
              % (s, sp, sf, per_suite_selected[s]))
    print("Total: passed %d, failed %d" % (passed, failed))

    summary = {
        "passed": passed,
        "failed": failed,
        "selected": total_selected,
        "skipped_suites": skipped_names,
    }
    if expected_total is not None:
        summary["expected_total"] = expected_total

    if failures or failed != expected_failed:
        print("check_fulljit: FAIL (%d failing test(s)):" % len(failures),
              file=sys.stderr)
        for name, reason, out_text in failures[:20]:
            print("  FAIL %s: %s" % (name, reason), file=sys.stderr)
            if out_text:
                for ln in out_text.splitlines()[:8]:
                    print("    | " + ln, file=sys.stderr)
        if expected_total is not None and passed != expected_total:
            print("check_fulljit: FAIL: expected total %d, got passed=%d failed=%d"
                  % (expected_total, passed, failed), file=sys.stderr)
        return 1, summary

    if expected_total is not None and (passed != expected_total or skipped_suites):
        skip_note = (
            " (skipped suites: %s)" % ", ".join(
                "%s[%s]" % (s, ",".join(t)) for s, t in skipped_suites)
            if skipped_suites else ""
        )
        msg = ("count/suite mismatch: passed %d / failed %d, require passed %d / failed %d%s"
               % (passed, failed, expected_total, expected_failed, skip_note))
        print("check_fulljit: FAIL: %s" % msg, file=sys.stderr)
        summary["error"] = msg
        return 1, summary

    print("check_fulljit: PASS (passed %d, failed %d, no dynamic profile)"
          % (passed, failed))
    return 0, summary


def self_test():
    """Positive and negative controls using hermetic fixture trees."""
    bad = 0

    def report(name, cond, detail=""):
        print("self-test %-44s -> %s%s"
              % (name, "PASS" if cond else "FAIL",
                 (" (" + detail + ")") if detail else ""))
        return 0 if cond else 1

    # 1. Command builder must omit -dynamicprofileinput and -dynamicprofilecache.
    cmd = build_test_cmd("/fake/ch", {"files": "a.js", "compile-flags": "-mic:1"})
    bad += report(
        "cmd includes FullJit flags and omits profile",
        "-forceNative" in cmd
        and "-off:simpleJit" in cmd
        and "-bgJitDelay:0" in cmd
        and "-mic:1" in cmd
        and not any("dynamicprofile" in x.lower() for x in cmd),
        " ".join(cmd),
    )

    # 2. Tag filter checks (linux-x64, darwin-arm64, and win32).
    _, _, not_tags = resolve_platform_tags(select="test", plat="linux", machine="x86_64")
    bad += report(
        "linux-x64 filters exclude_dynapogo, exclude_linux, exclude_xplat, require_simd",
        not should_run_test({"tags": "exclude_dynapogo"}, not_tags)
        and not should_run_test({"tags": "exclude_linux"}, not_tags)
        and not should_run_test({"tags": "exclude_xplat"}, not_tags)
        and not should_run_test({"tags": "require_simd"}, not_tags)
        and not should_run_test({"tags": "Slow"}, not_tags)
        and should_run_test({"tags": "require_backend"}, not_tags),
    )
    mac_arch, mac_alias, mac_tags = resolve_platform_tags(
        select="test", plat="darwin", machine="arm64")
    bad += report(
        "darwin-arm64 uses arm64, exclude_mac, keeps xplat & simd",
        mac_arch == "arm64"
        and mac_alias is None
        and "exclude_mac" in mac_tags
        and "exclude_xplat" not in mac_tags
        and "require_simd" not in mac_tags,
    )
    win_arch, win_alias, win_tags = resolve_platform_tags(
        select="test", plat="win32", machine="AMD64")
    bad += report(
        "win32 uses x64/amd64, exclude_windows, keeps xplat & simd",
        win_arch == "x64"
        and win_alias == "amd64"
        and "exclude_windows" in win_tags
        and "exclude_xplat" not in win_tags
        and "require_simd" not in win_tags
        and not should_run_test({"tags": "exclude_windows"}, win_tags)
        and should_run_test({"tags": "exclude_xplat"}, win_tags)
        and should_run_test({"tags": "require_simd"}, win_tags),
    )

    # 2b. PE32+ verify_jit_binary fails closed on DISABLE_JIT / Release.
    with tempfile.TemporaryDirectory(prefix="check-fulljit-pe-") as pe_tmp:
        pe_ch = os.path.join(pe_tmp, "ch.exe")
        pe_dll = os.path.join(pe_tmp, "ChakraCore.dll")
        with open(pe_ch, "wb") as f:
            f.write(check_metadata_windows.build_synthetic_pe(is_dll=False))
        with open(pe_dll, "wb") as f:
            f.write(check_metadata_windows.build_synthetic_pe(is_dll=True))

        def fake_pe_jit(cmd, **_kw):
            if "-?" in cmd:
                return 0, "Usage: ch [flaglist] <source file>\n", ""
            return 0, "---BeginBackEnd: add---\n---EndBackEnd---\n", ""

        def fake_pe_nojit(cmd, **_kw):
            if "-?" in cmd:
                return 0, "Usage: ch [flaglist] <source file>\n", ""
            return 0, "jit-add 5\n", ""

        with mock.patch.object(common, "run_capture", side_effect=fake_pe_jit):
            bad += report("verify_jit_binary accepts PE32+ with BackEnd trace",
                          verify_jit_binary(pe_ch) is None)
        with mock.patch.object(common, "run_capture", side_effect=fake_pe_nojit):
            bad += report("verify_jit_binary rejects PE32+ DISABLE_JIT",
                          verify_jit_binary(pe_ch) is not None)

        # Non-PE (Mach-O / ELF) verify_jit_binary must also enforce BackEnd trace (B-8).
        macho_ch = os.path.join(pe_tmp, "ch.macho")
        with open(macho_ch, "wb") as f:
            f.write(b"\xcf\xfa\xed\xfe" + b"\x00" * 60)

        def fake_macho_jit(cmd, **_kw):
            if os.path.basename(cmd[0]) == "nm":
                return 0, "0000000000001000 T __ZN2Js19NativeCodeGeneratorC1Ev\n", ""
            return fake_pe_jit(cmd)

        def fake_macho_nojit(cmd, **_kw):
            if os.path.basename(cmd[0]) == "nm":
                return 0, "0000000000001000 T __ZN2Js19NativeCodeGeneratorC1Ev\n", ""
            return fake_pe_nojit(cmd)

        with mock.patch.object(common, "run_capture", side_effect=fake_macho_jit):
            bad += report("verify_jit_binary accepts non-PE with BackEnd trace",
                          verify_jit_binary(macho_ch) is None)
        with mock.patch.object(common, "run_capture", side_effect=fake_macho_nojit):
            bad += report("verify_jit_binary rejects non-PE without BackEnd trace",
                          verify_jit_binary(macho_ch) is not None)

    # 2c. Gate counts & default expected count selection (B-6).
    bad += report(
        "gate_counts.json fulljit test pin is 634/0",
        FULLJIT_PINS.get("test") == (634, 0)
        and default_expected_counts(DEFAULT_SUITES, "test", plat="darwin") == (634, 0)
        and default_expected_counts(DEFAULT_SUITES, "test", plat="linux") == (634, 0)
        and default_expected_counts(DEFAULT_SUITES, "debug", plat="darwin") == (653, 0)
        and default_expected_counts(DEFAULT_SUITES, "test", plat="win32") == (633, 0)
        and default_expected_counts(("S1",), "test") == (None, 0),
        repr(FULLJIT_PINS),
    )

    # 3. Grade checks.
    bad += report(
        "baseline CRLF normalization accepts LF/CRLF",
        grade_test_output(0, b"ok\nline2\n", b"ok\r\nline2\r\n") is None,
    )
    bad += report(
        "wrong baseline fails",
        grade_test_output(0, b"wrong\n", b"ok\n") is not None,
    )
    bad += report(
        "nonzero rc with 'pass' stdout still fails",
        grade_test_output(1, b"pass\n", None) is not None,
    )
    bad += report(
        "empty stdout fails",
        grade_test_output(0, b"\n", None) is not None,
    )

    if os.name == "posix":
        with tempfile.TemporaryDirectory(prefix="check-fulljit-selftest-") as tmp:
            troot = os.path.join(tmp, "test")
            s1 = os.path.join(troot, "S1")
            os.makedirs(s1)
            with open(os.path.join(troot, "rlexedirs.xml"), "w", encoding="utf-8") as f:
                f.write('<?xml version="1.0" encoding="utf-8"?><dirs>'
                        '<test><default><files>S1</files></default></test>'
                        '</dirs>\n')
            with open(os.path.join(s1, "rlexe.xml"), "w", encoding="utf-8") as f:
                f.write(
                    '<?xml version="1.0" encoding="utf-8"?><regress-exe>'
                    '<test><default><files>a.js</files></default></test>'
                    '<test><default><files>b.js</files><baseline>b.baseline</baseline></default></test>'
                    '<test><default><files>skip.js</files><tags>exclude_dynapogo</tags></default></test>'
                    '</regress-exe>\n'
                )
            with open(os.path.join(s1, "a.js"), "w", encoding="utf-8") as f:
                f.write("pass\n")
            with open(os.path.join(s1, "b.js"), "w", encoding="utf-8") as f:
                f.write("hello\n")
            with open(os.path.join(s1, "b.baseline"), "wb") as f:
                f.write(b"hello\r\n")
            with open(os.path.join(s1, "skip.js"), "w", encoding="utf-8") as f:
                f.write("FAIL\n")

            log_file = os.path.join(tmp, "argv.log")
            stub = os.path.join(tmp, "stub-ch")
            with open(stub, "w", encoding="utf-8") as f:
                f.write(
                    "#!/usr/bin/env python3\n"
                    "import sys\n"
                    "with open(%r, 'a') as lf:\n"
                    "    lf.write(' '.join(sys.argv[1:]) + '\\n')\n"
                    "if sys.argv[-1] == 'a.js':\n"
                    "    print('pass')\n"
                    "elif sys.argv[-1] == 'b.js':\n"
                    "    print('hello')\n"
                    "else:\n"
                    "    print('UNEXPECTED')\n"
                    "    sys.exit(2)\n" % log_file
                )
            os.chmod(stub, 0o755)

            rc, summary = run_suites(
                troot, stub, suites=("S1",), jobs=1, skip_binary_probe=True)
            with open(log_file, encoding="utf-8") as lf:
                logged = lf.read()
            bad += report(
                "stub green suite passes 2/0 without profile",
                rc == 0 and summary["passed"] == 2 and summary["failed"] == 0
                and summary["skipped_suites"] == []
                and "dynamicprofile" not in logged,
                repr(summary),
            )

            # Expected-total match passes; mismatch fails hard (B-6).
            rc_pin_ok, summary_pin_ok = run_suites(
                troot, stub, suites=("S1",), jobs=1, skip_binary_probe=True,
                expected_total=2)
            bad += report(
                "exact expected_total=2 passes",
                rc_pin_ok == 0 and summary_pin_ok["passed"] == 2,
                repr(summary_pin_ok),
            )
            rc_pin_bad, summary_pin_bad = run_suites(
                troot, stub, suites=("S1",), jobs=1, skip_binary_probe=True,
                expected_total=3)
            bad += report(
                "expected_total=3 mismatch fails on 2-test suite",
                rc_pin_bad == 1 and summary_pin_bad["passed"] == 2,
                repr(summary_pin_bad),
            )

            # Skipped suite by folder tags is logged and fails against expected_total.
            s2 = os.path.join(troot, "S2")
            os.makedirs(s2)
            with open(os.path.join(s2, "rlexe.xml"), "w", encoding="utf-8") as f:
                f.write('<?xml version="1.0" encoding="utf-8"?><regress-exe>'
                        '<test><default><files>c.js</files></default></test>'
                        '</regress-exe>\n')
            with open(os.path.join(s2, "c.js"), "w", encoding="utf-8") as f:
                f.write("pass\n")
            with open(os.path.join(troot, "rlexedirs.xml"), "w", encoding="utf-8") as f:
                f.write('<?xml version="1.0" encoding="utf-8"?><dirs>'
                        '<test><default><files>S1</files></default></test>'
                        '<test><default><files>S2</files><tags>exclude_dynapogo</tags></default></test>'
                        '</dirs>\n')
            rc_skip_bad, summary_skip_bad = run_suites(
                troot, stub, suites=("S1", "S2"), jobs=1, skip_binary_probe=True,
                expected_total=3)
            bad += report(
                "skipped suite mismatch fails against expected_total",
                rc_skip_bad == 1 and summary_skip_bad["skipped_suites"] == ["S2"],
                repr(summary_skip_bad),
            )
            rc_skip_sneaky, summary_skip_sneaky = run_suites(
                troot, stub, suites=("S1", "S2"), jobs=1, skip_binary_probe=True,
                expected_total=2)
            bad += report(
                "skipped suite fails even if remaining tests match expected_total",
                rc_skip_sneaky == 1 and summary_skip_sneaky["skipped_suites"] == ["S2"],
                repr(summary_skip_sneaky),
            )

            # Restore rlexedirs.xml and test doctored baseline failure.
            with open(os.path.join(troot, "rlexedirs.xml"), "w", encoding="utf-8") as f:
                f.write('<?xml version="1.0" encoding="utf-8"?><dirs>'
                        '<test><default><files>S1</files></default></test>'
                        '</dirs>\n')
            with open(os.path.join(s1, "b.baseline"), "wb") as f:
                f.write(b"different\n")
            rc_bad, summary_bad = run_suites(
                troot, stub, suites=("S1",), jobs=1, skip_binary_probe=True)
            bad += report(
                "doctored baseline rejects suite",
                rc_bad == 1 and summary_bad["failed"] == 1,
                repr(summary_bad),
            )

    print("check_fulljit self-test: %s"
          % ("ALL NEGATIVE CONTROLS BEHAVE" if bad == 0
             else "%d CONTROL(S) MISBEHAVE" % bad))
    return 1 if bad else 0


def main(argv):
    args = list(argv[1:])
    if "--self-test" in args:
        return self_test()

    suites = list(DEFAULT_SUITES)
    select = "test"
    expected_total = None
    jobs = None
    timeout = DEFAULT_TIMEOUT
    rest = []
    i = 0
    while i < len(args):
        if args[i] == "--select":
            if i + 1 >= len(args):
                print("check_fulljit: --select needs a value", file=sys.stderr)
                return 2
            select = args[i + 1]
            i += 2
        elif args[i] == "--expected-total":
            if i + 1 >= len(args):
                print("check_fulljit: --expected-total needs a value", file=sys.stderr)
                return 2
            try:
                expected_total = int(args[i + 1])
            except ValueError:
                print("check_fulljit: bad --expected-total %r: expected positive int"
                      % args[i + 1], file=sys.stderr)
                return 2
            if expected_total <= 0:
                print("check_fulljit: bad --expected-total %d: must be > 0"
                      % expected_total, file=sys.stderr)
                return 2
            i += 2
        elif args[i] == "--jobs":
            if i + 1 >= len(args):
                print("check_fulljit: --jobs needs a value", file=sys.stderr)
                return 2
            jobs = int(args[i + 1])
            i += 2
        elif args[i] == "--timeout":
            if i + 1 >= len(args):
                print("check_fulljit: --timeout needs a value", file=sys.stderr)
                return 2
            timeout = float(args[i + 1])
            i += 2
        elif args[i] == "--suites":
            i += 1
            suites = []
            while i < len(args) and not args[i].startswith("--"):
                suites.append(args[i])
                i += 1
        else:
            rest.append(args[i])
            i += 1

    if select not in ("test", "debug"):
        print("check_fulljit: bad --select %r: expected test or debug" % select,
              file=sys.stderr)
        return 2
    if len(rest) != 2:
        print("usage: check_fulljit.py <runtests_or_test_root> <ch> "
              "[--suites S1 S2 ...] [--select test|debug] [--expected-total N] "
              "[--jobs N] [--timeout SECS] [--self-test]", file=sys.stderr)
        return 2

    target, ch = rest[0], rest[1]
    if not os.path.exists(target):
        print("check_fulljit: missing test path: %s" % target, file=sys.stderr)
        return 2
    if not os.path.isfile(ch):
        print("check_fulljit: missing binary: %s" % ch, file=sys.stderr)
        return 2

    test_root = target if os.path.isdir(target) else os.path.dirname(os.path.abspath(target))
    expected_failed = 0
    if expected_total is None:
        expected_total, expected_failed = default_expected_counts(
            suites=tuple(suites), select=select)
    rc, _ = run_suites(
        test_root, ch, suites=tuple(suites), select=select,
        jobs=jobs, timeout=timeout, expected_total=expected_total,
        expected_failed=expected_failed,
    )
    return rc


if __name__ == "__main__":
    sys.exit(main(sys.argv))
