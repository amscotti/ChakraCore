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
       [--suites S1 S2 ...] [--select test|debug] [--jobs N]
       [--timeout SECS] [--self-test]
Exit code: 0 iff NativeCodeGenerator + flaglist are present, >0 tests
run, and every executed test passes.
"""

from concurrent.futures import ThreadPoolExecutor, as_completed
import os
import platform
import re
import shutil
import subprocess
import sys
import tempfile
import xml.etree.ElementTree as ET

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import check_jit
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

_CRLF_RE = re.compile(b"[\r]+\n")


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
    """Ensure ch carries NativeCodeGenerator and supports the debug flaglist."""
    dylib_name = ("libChakraCore.dylib" if sys.platform == "darwin"
                  else "libChakraCore.so")
    sibling = os.path.join(os.path.dirname(os.path.abspath(ch)), dylib_name)
    target_for_nm = sibling if os.path.isfile(sibling) else ch
    nm = shutil.which("nm") or "/usr/bin/nm"
    rc, out, err = common.run_capture([nm, target_for_nm])
    if rc != 0:
        return "nm %s exited %d: %s" % (target_for_nm, rc, err.strip())
    if "NativeCodeGenerator" not in out:
        return ("%s has no NativeCodeGenerator symbol; JIT is disabled"
                % target_for_nm)
    if not check_jit.flaglist_supported(ch):
        return "%s does not support the test/debug flaglist (Release build?)" % ch
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
               jobs=None, timeout=DEFAULT_TIMEOUT, skip_binary_probe=False):
    """Run no-profile FullJit across suites. Returns (rc, summary_dict)."""
    abs_ch = os.path.abspath(ch)
    abs_root = os.path.abspath(test_root)
    if not skip_binary_probe:
        probe_err = verify_jit_binary(abs_ch)
        if probe_err is not None:
            print("check_fulljit: FAIL: %s" % probe_err, file=sys.stderr)
            return 1, {"passed": 0, "failed": 0, "error": probe_err}

    arch, arch_alias, not_tags = resolve_platform_tags(select=select)
    folder_tags = load_folder_tags(abs_root)

    parallel_tests = []
    sequential_tests = []
    for suite in suites:
        suite_dir = os.path.join(abs_root, suite)
        if not os.path.isfile(os.path.join(suite_dir, "rlexe.xml")):
            msg = "missing suite rlexe.xml: %s" % suite_dir
            print("check_fulljit: FAIL: %s" % msg, file=sys.stderr)
            return 2, {"passed": 0, "failed": 0, "error": msg}
        ftags = (folder_tags.get(suite.lower(), set())
                 if folder_tags is not None else set())
        if not ftags.isdisjoint(not_tags):
            continue
        entries = parse_rlexe_suite(suite_dir, arch, arch_alias)
        selected = [e for e in entries if should_run_test(e, not_tags)]
        if "sequential" in ftags:
            sequential_tests.extend(selected)
        else:
            parallel_tests.extend(selected)

    total_selected = len(parallel_tests) + len(sequential_tests)
    if total_selected == 0:
        msg = "zero tests selected for suites=%r" % (list(suites),)
        print("check_fulljit: FAIL: %s" % msg, file=sys.stderr)
        return 1, {"passed": 0, "failed": 0, "error": msg}

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
        print("  %s: passed %d, failed %d" % (s, sp, sf))
    print("Total: passed %d, failed %d" % (passed, failed))

    if failures:
        print("check_fulljit: FAIL (%d failing test(s)):" % len(failures),
              file=sys.stderr)
        for name, reason, out_text in failures[:20]:
            print("  FAIL %s: %s" % (name, reason), file=sys.stderr)
            if out_text:
                for ln in out_text.splitlines()[:8]:
                    print("    | " + ln, file=sys.stderr)
        return 1, {"passed": passed, "failed": failed}

    print("check_fulljit: PASS (passed %d, failed %d, no dynamic profile)"
          % (passed, failed))
    return 0, {"passed": passed, "failed": failed}


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

    # 2. Tag filter checks.
    _, _, not_tags = resolve_platform_tags(select="test", plat="linux", machine="x86_64")
    bad += report(
        "exclude_dynapogo and exclude_linux filtered",
        not should_run_test({"tags": "exclude_dynapogo"}, not_tags)
        and not should_run_test({"tags": "exclude_linux"}, not_tags)
        and not should_run_test({"tags": "Slow"}, not_tags)
        and should_run_test({"tags": "require_backend"}, not_tags),
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
                rc == 0 and summary == {"passed": 2, "failed": 0}
                and "dynamicprofile" not in logged,
                repr(summary),
            )

            # Doctored baseline fails loudly.
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
              "[--suites S1 S2 ...] [--select test|debug] [--jobs N] "
              "[--timeout SECS] [--self-test]", file=sys.stderr)
        return 2

    target, ch = rest[0], rest[1]
    if not os.path.exists(target):
        print("check_fulljit: missing test path: %s" % target, file=sys.stderr)
        return 2
    if not os.path.isfile(ch):
        print("check_fulljit: missing binary: %s" % ch, file=sys.stderr)
        return 2

    test_root = target if os.path.isdir(target) else os.path.dirname(os.path.abspath(target))
    rc, _ = run_suites(
        test_root, ch, suites=tuple(suites), select=select,
        jobs=jobs, timeout=timeout,
    )
    return rc


if __name__ == "__main__":
    sys.exit(main(sys.argv))
