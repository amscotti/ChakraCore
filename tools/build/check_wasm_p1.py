#!/usr/bin/env python3
#-------------------------------------------------------------------------------------------------------
# Copyright (c) ChakraCore Project Contributors. All rights reserved.
# Licensed under the MIT license. See LICENSE.txt file in the project root for full license information.
#-------------------------------------------------------------------------------------------------------
"""Checked-in P1 gate counter: arm64 interpreter wasm/asm.js manifest set.

Runs (or replays from a saved log) the doc/wasm-arm64-plan.md section 6 P1
command -- interpreted variant with -nonative (true interpreter-only) over
WasmSpec, wasm, AsmJs, ASMJSParser -- parses per-test pass/fail, joins
each log block to a test/wasm-arm64-manifest.json entry by test-id, and
prints the exact P1 pass count. This replaces the ad-hoc /tmp log joins:
runtests.py does not consume the manifest, so the join lives here.

Run (from repo root; runtests.py runs from test/ with an absolute
--binary, same requirement as tools/build/check_regression.py), with the
reporter/coverage flags this gate needs appended to the published P1
command (verbose per-test command lines + slow entries, so every P1
entry emits a joinable block):

    python3 tools/build/check_wasm_p1.py test/runtests.py out/jit/bin/ch

P2 FullJIT gate over the same 381-entry set (dynapogo variant):

    python3 tools/build/check_wasm_p1.py --jit test/runtests.py out/jit/bin/ch

Replay a saved verbose log without running the suite (positionals
optional; the test root is derived from the given runtests path or from
this file's repo root; --jit selects the FullJIT join):

    python3 tools/build/check_wasm_p1.py --log /tmp/p1.log [--manifest PATH]

Join (command-line -> test-id, 0-unmatched fidelity): runtests.py
assigns each loaded entry a global test.id (1..N, in folder-argument
order, rlexe document order within each suite) and prints it in the
verbose command line as -dynamicprofilecache:profile.dpl.<id>. The join
maps that id back to (suite, 0-based rlexe ordinal) -> test-id
"<suite>[<ordinal>]", then verifies the command-line tail (entry
compile-flags + file) equals the rlexe entry exactly and that the
[ n/N ] Passed/Failed -> folder/file short name agrees. Duplicate
(files, flags) entries (wasm binary.js x2, AsmJs relink/nanbug/etc.)
therefore disambiguate by id, never by guessing. Any block with no
command line, no/duplicate profile token, out-of-range id, folder or
flags mismatch, missing -nonative / -maxInterpretCount:1, unknown
test-id, duplicate test-id, or missing P1 entry fails loudly.

Exact counts (no silent defaults): the manifest P1 total must equal
381, exactly one "Total: passed N, failed M" summary line must be
present, and per-test statuses must sum exactly to it. Exit 0 iff all
381 P1 entries passed with a complete join; nonzero if any P1 entry
failed or the join is incomplete. Without -v, passed blocks have no
command line and the join fails loudly (rerun with -v).
"""

import json
import os
import re
import subprocess
import sys
import xml.etree.ElementTree as ET

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import common

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(HERE))

EXPECTED_P1_TOTAL = 381
P1_SUITES = ["WasmSpec", "wasm", "AsmJs", "ASMJSParser"]

TOTAL_RE = re.compile(r"^Total: passed (\d+), failed (\d+)\s*$")
RESULT_RE = re.compile(
    r"^\[(\d+)/(\d+)\s+([\d.]+)\]\s+(Passed|Failed)\s+->\s+(\S+)\s*$")
PROFILE_RE = re.compile(r"^-dynamicprofilecache:profile\.dpl\.(\d+)$")
PROFILE_JIT_RE = re.compile(r"^-dynamicprofileinput:profile\.dpl\.(\d+)$")
TESTID_RE = re.compile(r"^(.+)\[(\d+)\]$")


def load_manifest(path):
    """Load and validate the D7 manifest; SystemExit(2) if unreadable.

    Returns (entries, meta_dict). Validation covers shape, duplicate
    test-ids, and meta total/p1 consistency when meta is present. The
    P1-total pin (381) is checked by the grader, not here, so unit
    fixtures can use smaller manifests with their own pin.
    """
    try:
        with open(path, "r", encoding="utf-8") as f:
            data = json.load(f)
    except OSError as e:
        print("check_wasm_p1: FAIL: cannot read manifest %s: %s"
              % (path, e), file=sys.stderr)
        raise SystemExit(2)
    except ValueError as e:
        print("check_wasm_p1: FAIL: malformed manifest %s: %s"
              % (path, e), file=sys.stderr)
        raise SystemExit(2)
    if not isinstance(data, dict) or not isinstance(data.get("entries"), list):
        print("check_wasm_p1: FAIL: manifest %s needs "
              '{"entries": [...]}' % path, file=sys.stderr)
        raise SystemExit(2)
    entries = data["entries"]
    seen = set()
    for e in entries:
        if (not isinstance(e, dict) or not isinstance(e.get("test-id"), str)
                or not isinstance(e.get("suite"), str)
                or not isinstance(e.get("phase"), str)):
            print("check_wasm_p1: FAIL: manifest %s: entry needs "
                  "test-id/suite/phase strings: %r" % (path, e),
                  file=sys.stderr)
            raise SystemExit(2)
        if e["test-id"] in seen:
            print("check_wasm_p1: FAIL: manifest %s: duplicate test-id %s"
                  % (path, e["test-id"]), file=sys.stderr)
            raise SystemExit(2)
        seen.add(e["test-id"])
    meta = data.get("meta") if isinstance(data.get("meta"), dict) else {}
    if meta:
        if "total" in meta and meta["total"] != len(entries):
            print("check_wasm_p1: FAIL: manifest %s: meta.total %r "
                  "!= len(entries) %d" % (path, meta["total"], len(entries)),
                  file=sys.stderr)
            raise SystemExit(2)
        p1 = sum(1 for e in entries if e["phase"] == "P1")
        if "p1" in meta and meta["p1"] != p1:
            print("check_wasm_p1: FAIL: manifest %s: meta.p1 %r "
                  "!= counted %d" % (path, meta["p1"], p1), file=sys.stderr)
            raise SystemExit(2)
    return entries, meta


def load_rlexe(path):
    """Parse one rlexe.xml into (files, flags, baseline) per entry."""
    try:
        tree = ET.parse(path)
    except (OSError, ET.ParseError) as e:
        print("check_wasm_p1: FAIL: cannot parse rlexe %s: %s" % (path, e),
              file=sys.stderr)
        raise SystemExit(2)
    out = []
    for test in tree.getroot().findall(".//test"):
        default = test.find("default")
        if default is None:
            print("check_wasm_p1: FAIL: rlexe %s: entry without "
                  "<default>" % path, file=sys.stderr)
            raise SystemExit(2)
        out.append({
            "files": (default.findtext("files") or "").strip(),
            "flags": (default.findtext("compile-flags") or "").strip(),
            "baseline": (default.findtext("baseline") or "").strip(),
            "tags": (default.findtext("tags") or "").strip(),
        })
    return out


def build_id_map(test_root, suites=P1_SUITES):
    """Map global runtests test.id -> rlexe ref and test-id -> ref.

    Mirrors runtests.py TestLoader: suites in argument order, document
    order within each suite, ids starting at 1. Also returns the
    per-suite rlexe lists and the manifest-coverage check inputs.
    Paths are <test_root>/<suite>/rlexe.xml (git spellings).
    """
    id_to_ref = {}
    testid_to_ref = {}
    rlexe_by_suite = {}
    next_id = 1
    for suite in suites:
        path = os.path.join(test_root, suite, "rlexe.xml")
        raw = load_rlexe(path)
        rlexe_by_suite[suite] = raw
        for ordinal, entry in enumerate(raw):
            test_id = "%s[%d]" % (suite, ordinal)
            ref = {"id": next_id, "suite": suite, "ordinal": ordinal,
                   "test-id": test_id, "files": entry["files"],
                   "flags": entry["flags"],
                   "baseline": entry["baseline"],
                   "tags": entry["tags"]}
            id_to_ref[next_id] = ref
            testid_to_ref[test_id] = ref
            next_id += 1
    return id_to_ref, testid_to_ref, rlexe_by_suite


def normalize_lines(lines):
    """Expand \r line ends so verbose pass lines parse as lines.

    runtests.py ends pass progress lines with \\r (terminal overwrite);
    through a pipe that is a raw \\r separator, through
    common.run_tee it arrives already translated, and synthetic
    fixtures carry no trailing newline at all. Accept all three by
    joining with separators where missing and splitting on \\r/\\n.
    """
    parts = []
    for line in lines:
        parts.append(line if line.endswith("\n") else line + "\n")
    text = "".join(parts)
    text = text.replace("\r\n", "\n").replace("\r", "\n")
    return text.split("\n")


def parse_total(lines):
    """Return (passed, failed) from the Total line, or None.

    Exactly one summary is required: None when zero match, ValueError
    when 2+ match (mirrors check_regression.parse_total).
    """
    found = []
    for line in lines:
        m = TOTAL_RE.match(line.strip())
        if m:
            found.append((int(m.group(1)), int(m.group(2))))
    if len(found) > 1:
        raise ValueError("expected exactly one 'Total: passed N, failed M' "
                         "summary line, got %d: %r" % (len(found), found))
    return found[0] if found else None


def parse_blocks(lines):
    """Split log lines into per-test blocks (status + short + command).

    Each block is {"status", "short", "folder", "file", "cmd"} where
    cmd is the following command line or None when absent. A
    timeout failure prints "ERROR: Test timed out!" (or WARNING)
    between the result line and the command line (runtests.py
    TestVariant._show_failed); skip that one line. Non-result lines
    (headers, summaries, test output) are ignored here; the caller
    cross-checks the Total line separately.
    """
    norm = normalize_lines(lines)
    blocks = []
    for idx, raw in enumerate(norm):
        m = RESULT_RE.match(raw.strip())
        if not m:
            continue
        status = m.group(4)
        short = m.group(5).strip()
        folder, _, fname = short.partition("/")
        cmd = None
        nxt_idx = idx + 1
        if nxt_idx < len(norm) and norm[nxt_idx].strip() in (
                "ERROR: Test timed out!", "WARNING: Test timed out!"):
            nxt_idx += 1
        if nxt_idx < len(norm):
            nxt = norm[nxt_idx].strip()
            if nxt and not RESULT_RE.match(nxt) and not TOTAL_RE.match(nxt):
                cmd = nxt
        blocks.append({"status": status, "short": short,
                       "folder": folder, "file": fname, "cmd": cmd,
                       "line": idx + 1})
    return blocks


def grade_join(blocks, id_to_ref, manifest_test_ids, p1_test_ids,
               expected_total, mode="interp"):
    """Join blocks to test-ids and grade completeness and P1 passes.

    mode "interp" joins the P1 interpreted command (true
    interpreter-only: -nonative, -maxInterpretCount:1,
    -dynamicprofilecache token). mode "jit" joins the P2 FullJIT
    command instead (dynapogo variant: -forceNative, -off:simpleJit,
    no -nonative, -dynamicprofileinput token). Cross-mode logs are
    rejected: an interp log never joins under mode "jit" and vice
    versa.

    Returns a detail dict. ok is True only when every P1 entry passed
    with a complete, unambiguous join and the P1 pin matches.
    """
    joined = {}
    statuses = {}
    unmatched = []
    duplicates = []
    pinned_ids = set()
    for b in blocks:
        reason = None
        test_id = None
        cmd = b["cmd"]
        if cmd is None:
            reason = ("missing command line (not a verbose log; "
                      "rerun with -v)")
        else:
            tokens = cmd.split()
            if not tokens:
                reason = "empty command line"
            elif not os.path.isabs(tokens[0]):
                reason = "binary not absolute: %r" % tokens[0]
            else:
                fullpath = tokens[-1]
                if (os.path.basename(fullpath) != b["file"] or
                        os.path.basename(os.path.dirname(fullpath))
                        != b["folder"]):
                    reason = ("short name %r disagrees with command "
                              "path %r" % (b["short"], fullpath))
                elif mode != "jit" and "-nonative" not in tokens:
                    reason = "not a P1 -nonative command line"
                elif mode != "jit" and "-maxInterpretCount:1" not in tokens:
                    reason = "not an interpreted-variant command line"
                else:
                    prof_re = (PROFILE_JIT_RE if mode == "jit"
                               else PROFILE_RE)
                    prof_kind = ("-dynamicprofileinput" if mode == "jit"
                                 else "-dynamicprofilecache")
                    prof = [prof_re.match(t) for t in tokens]
                    prof = [m for m in prof if m]
                    if len(prof) != 1:
                        reason = ("expected exactly one %s token, got %d"
                                  % (prof_kind, len(prof)))
                    else:
                        pid = int(prof[0].group(1))
                        ref = id_to_ref.get(pid)
                        if ref is None:
                            reason = ("profile id %d out of range "
                                      "(%d loaded entries)"
                                      % (pid, len(id_to_ref)))
                        elif ref["suite"] != b["folder"]:
                            reason = ("folder %r disagrees with "
                                      "profile-id suite %r for id %d "
                                      "(test-id %s)"
                                      % (b["folder"], ref["suite"], pid,
                                         ref["test-id"]))
                        else:
                            exp_flags = (ref["flags"].split()
                                         if ref["flags"] else [])
                            k = len(exp_flags)
                            actual = tokens[-1 - k:-1] if k else []
                            if actual != exp_flags:
                                reason = ("flags mismatch for %s: "
                                          "expected %r, got %r"
                                          % (ref["test-id"], exp_flags,
                                             actual))
                            elif ref["test-id"] not in manifest_test_ids:
                                reason = ("test-id %s not in manifest "
                                          "(rlexe/manifest drift)"
                                          % ref["test-id"])
                            elif mode == "jit":
                                pinned_entry = ("nonative" in
                                                ref["flags"].lower())
                                has_nonative = "-nonative" in tokens
                                if has_nonative and not pinned_entry:
                                    reason = ("interp (-nonative) command "
                                              "line for non-pinned entry "
                                              "%s in jit mode"
                                              % ref["test-id"])
                                elif (not has_nonative and
                                        "-forceNative" not in tokens):
                                    reason = ("not a FullJIT (-forceNative) "
                                              "command line")
                                elif (not has_nonative and
                                        "-off:simpleJit" not in tokens):
                                    reason = ("not a full-JIT-only "
                                              "(-off:simpleJit) command "
                                              "line")
                                elif (has_nonative and "-forceNative"
                                        not in tokens):
                                    reason = ("pinned entry %s ran without "
                                              "FullJIT variant flags"
                                              % ref["test-id"])
                                else:
                                    test_id = ref["test-id"]
                                    if has_nonative:
                                        pinned_ids.add(test_id)
                            else:
                                test_id = ref["test-id"]
        if reason is not None:
            unmatched.append({"short": b["short"], "status": b["status"],
                              "reason": reason, "line": b["line"]})
        else:
            if test_id in joined:
                duplicates.append({"test-id": test_id,
                                   "shorts": [joined[test_id], b["short"]],
                                   "line": b["line"]})
            else:
                joined[test_id] = b["short"]
                statuses[test_id] = b["status"]
    p1_set = set(p1_test_ids)
    passed_ids = sorted(t for t in statuses
                        if t in p1_set and statuses[t] == "Passed"
                        and t not in pinned_ids)
    failed_ids = sorted(t for t in statuses
                        if t in p1_set and statuses[t] == "Failed")
    missing = sorted(set(p1_test_ids) - set(statuses))
    pinned_passed = sorted(t for t in pinned_ids
                           if t in p1_set and statuses.get(t) == "Passed")
    excluded = []
    still_missing = []
    if mode == "jit":
        by_test_id = {r["test-id"]: r for r in id_to_ref.values()}
        for t in missing:
            r = by_test_id.get(t)
            if r is not None and "exclude_dynapogo" in r["tags"]:
                excluded.append(t)
            else:
                still_missing.append(t)
    else:
        still_missing = missing
    detail = {"joined": joined, "statuses": statuses,
              "unmatched": unmatched, "duplicates": duplicates,
              "missing": still_missing, "failed": failed_ids,
              "passed": passed_ids, "pinned_passed": pinned_passed,
              "excluded": sorted(excluded),
              "p1_total": len(p1_test_ids),
              "expected_total": expected_total, "mode": mode}
    detail["ok"] = (len(p1_test_ids) == expected_total and not unmatched
                    and not duplicates and not still_missing
                    and not failed_ids)
    return detail


def check_total(parsed_total, blocks):
    """Return an error string, or None when Total matches blocks."""
    if parsed_total is None:
        return ("no 'Total: passed N, failed M' summary line in "
                "runtests.py output")
    passed = sum(1 for b in blocks if b["status"] == "Passed")
    failed = sum(1 for b in blocks if b["status"] == "Failed")
    if passed != parsed_total[0] or failed != parsed_total[1]:
        return ("parsed per-test counts passed %d / failed %d do not "
                "match Total passed %d / failed %d"
                % (passed, failed, parsed_total[0], parsed_total[1]))
    return None


def group_ids(test_ids):
    """Group sorted test-ids by suite prefix (before '[')."""
    groups = {}
    for tid in test_ids:
        m = TESTID_RE.match(tid)
        suite = m.group(1) if m else "?"
        groups.setdefault(suite, []).append(tid)
    return {k: sorted(v) for k, v in sorted(groups.items())}


def print_report(detail, total_parsed, total_err):
    """Print the P1 pass count and grouped failing/missing/unmatched."""
    n = len(detail["passed"])
    total = detail["expected_total"]
    modelbl = "jit" if detail.get("mode") == "jit" else "interp"
    print("check_wasm_p1: mode %s" % modelbl)
    print("check_wasm_p1: P1 pass %d/%d (failed %d, missing %d, "
          "unmatched %d, duplicates %d)"
          % (n, total, len(detail["failed"]), len(detail["missing"]),
             len(detail["unmatched"]), len(detail["duplicates"])))
    if modelbl == "jit":
        print("check_wasm_p1: interp-pinned pass %d, excluded-absent %d"
              % (len(detail["pinned_passed"]), len(detail["excluded"])))
        accounted = (len(detail["passed"]) + len(detail["pinned_passed"])
                     + len(detail["excluded"]) + len(detail["failed"])
                     + len(detail["missing"]))
        if accounted != total:
            print("check_wasm_p1: WARN: accounted %d != P1 total %d"
                  % (accounted, total))
    if detail["failed"]:
        print("check_wasm_p1: failing P1 (%d):" % len(detail["failed"]))
        for suite, ids in group_ids(detail["failed"]).items():
            print("  %s (%d): %s" % (suite, len(ids), ", ".join(ids)))
    if detail["missing"]:
        print("check_wasm_p1: missing P1 (%d):" % len(detail["missing"]))
        for suite, ids in group_ids(detail["missing"]).items():
            print("  %s (%d): %s" % (suite, len(ids), ", ".join(ids)))
    if detail["unmatched"]:
        print("check_wasm_p1: unmatched log blocks (%d):"
              % len(detail["unmatched"]))
        for u in detail["unmatched"][:20]:
            print("  line %d: %s [%s] %s"
                  % (u["line"], u["status"], u["short"], u["reason"]))
        if len(detail["unmatched"]) > 20:
            print("  ... and %d more" % (len(detail["unmatched"]) - 20))
    if detail["duplicates"]:
        print("check_wasm_p1: duplicate/ambiguous test-ids (%d):"
              % len(detail["duplicates"]))
        for d in detail["duplicates"][:20]:
            print("  line %d: %s (%s)"
                  % (d["line"], d["test-id"], ", ".join(d["shorts"])))
        if len(detail["duplicates"]) > 20:
            print("  ... and %d more" % (len(detail["duplicates"]) - 20))
    if total_parsed is not None:
        print("check_wasm_p1: runtests Total passed %d, failed %d"
              % total_parsed)
    elif total_err:
        print("check_wasm_p1: %s" % total_err)
    reasons = []
    if detail["p1_total"] != detail["expected_total"]:
        reasons.append("manifest P1 count %d != expected %d"
                       % (detail["p1_total"], detail["expected_total"]))
    if detail["failed"]:
        reasons.append("%d P1 entry(s) failed" % len(detail["failed"]))
    if detail["missing"]:
        reasons.append("%d P1 entry(s) missing from log"
                       % len(detail["missing"]))
    if detail["unmatched"]:
        reasons.append("%d log block(s) unmatched"
                       % len(detail["unmatched"]))
    if detail["duplicates"]:
        reasons.append("%d duplicate test-id(s)"
                       % len(detail["duplicates"]))
    if total_err:
        reasons.append(total_err)
    if reasons:
        print("check_wasm_p1: FAIL: %s" % "; ".join(reasons),
              file=sys.stderr)
    else:
        print("check_wasm_p1: PASS: P1 pass %d/%d with complete join"
              % (n, total))


def run_suite(runtests, ch, jit=False):
    """Run the gated P1 command with reporter flags; return (rc, lines).

    jit=False runs the P1 interpreted gate (--variants=interpreted plus
    -nonative). jit=True runs the P2 FullJIT gate over the same entry
    set (--variants=dynapogo: -forceNative -off:simpleJit).
    """
    abs_ch = os.path.abspath(ch)
    abs_runtests = os.path.abspath(runtests)
    test_root = os.path.dirname(abs_runtests)
    # -j 3: Slow P1 entries (limits.js segments) carry fixed 300s
    # per-test rlexe timeouts that override --timeout; solo they take
    # ~110-215s interpreted, but full-core parallelism starves them
    # past 300s (observed wasm[56]/[58] timeouts at default -j).
    # Bounded parallelism keeps the gate deterministic; the outer
    # ceiling (common.REGRESSION_TIMEOUT=5400) has ample headroom.
    if jit:
        variant_args = ["--variants=dynapogo"]
    else:
        variant_args = ["--variants=interpreted",
                        "--extra-flags=-nonative"]
    cmd = ([sys.executable, abs_runtests, "--test", "--arm64"]
           + variant_args
           + ["--include-slow", "-v", "-j", "3",
              "--binary=" + abs_ch] + P1_SUITES)
    print("check_wasm_p1: running: %s" % " ".join(cmd))
    print("check_wasm_p1: cwd %s (-v + --include-slow added so every "
          "P1 entry emits a joinable block)" % test_root)
    sys.stdout.flush()
    try:
        rc, lines = common.run_tee(cmd, cwd=test_root,
                                   timeout=common.REGRESSION_TIMEOUT)
    except subprocess.TimeoutExpired:
        print("check_wasm_p1: FAIL: runtests.py exceeded the %ds outer "
              "ceiling" % common.REGRESSION_TIMEOUT, file=sys.stderr)
        return 124, []
    # Defensive cleanup (mirrors check_regression / runtests.py end):
    # an interrupted run must not leave profile.dpl residue in test/.
    import glob as _glob
    for f in _glob.glob(os.path.join(test_root, "*", "profile.dpl.*")):
        try:
            os.remove(f)
            print("check_wasm_p1: removed leftover %s" % f)
        except OSError as e:
            print("check_wasm_p1: WARNING: cannot remove %s: %s" % (f, e))
    return rc, lines


def grade_log(lines, test_root, manifest_path, expected_total=None,
              mode="interp"):
    """Load manifest/rlexe, join log lines, and return (rc, detail).

    rc 0 iff the gate passes, 1 for P1 verdict failure (failed,
    missing, unmatched, duplicate, Total mismatch, P1-pin drift),
    2 for unparseable inputs. mode selects the joined command shape
    ("interp" or "jit"); see grade_join.
    """
    if expected_total is None:
        expected_total = EXPECTED_P1_TOTAL
    entries, _meta = load_manifest(manifest_path)
    manifest_ids = {e["test-id"] for e in entries}
    p1_ids = [e["test-id"] for e in entries if e["phase"] == "P1"]
    id_to_ref, _t2r, _by = build_id_map(test_root)
    blocks = parse_blocks(lines)
    try:
        total_parsed = parse_total(normalize_lines(lines))
    except ValueError as e:
        # No trustworthy Total: the join is incomplete by definition.
        detail = grade_join(blocks, id_to_ref, manifest_ids, p1_ids,
                            expected_total, mode)
        print_report(detail, None, "check_wasm_p1: FAIL: %s" % e)
        return 1, detail
    total_err = check_total(total_parsed, blocks)
    detail = grade_join(blocks, id_to_ref, manifest_ids, p1_ids,
                        expected_total, mode)
    detail["total_parsed"] = total_parsed
    detail["total_err"] = total_err
    print_report(detail, total_parsed, total_err)
    if total_err is not None:
        return 1, detail
    return (0 if detail["ok"] else 1), detail


def _cmd_line(binary, ref, test_root, mode="interp"):
    """Build the verbose command line runtests.py would print.

    mode "interp" is the P1 interpreted command, "jit" the P2
    FullJIT (dynapogo) command, "wrong" a command with neither
    variant's markers (negative control).
    """
    prefix = ["-ExtendedErrorStackForTestHost", "-BaselineMode"]
    if mode == "interp":
        prefix += ["-nonative", "-maxInterpretCount:1",
                   "-maxSimpleJitRunCount:1", "-bgjit-"]
        prefix += ["-dynamicprofilecache:profile.dpl.%d" % ref["id"]]
    elif mode == "jit":
        prefix += ["-forceNative", "-off:simpleJit", "-bgJitDelay:0"]
        prefix += ["-dynamicprofileinput:profile.dpl.%d" % ref["id"]]
    flags = prefix + (ref["flags"].split() if ref["flags"] else [])
    fullpath = os.path.join(test_root, ref["suite"], ref["files"])
    return " ".join([binary] + flags + [fullpath])


def _log_for(refs, status_by_testid, test_root, binary="/fake/bin/ch",
             mode="interp"):
    """Synthetic verbose log for given refs in id order."""
    lines = []
    for i, ref in enumerate(refs, start=1):
        status = status_by_testid.get(ref["test-id"], "Passed")
        lines.append("[%d/%d 0.01] %s -> %s/%s"
                     % (i, len(refs), status, ref["suite"], ref["files"]))
        lines.append(_cmd_line(binary, ref, test_root, mode))
    passed = sum(1 for ref in refs
                 if status_by_testid.get(ref["test-id"], "Passed")
                 == "Passed")
    lines.append("Total: passed %d, failed %d"
                 % (passed, len(refs) - passed))
    return lines


def self_test():
    """Negative controls: the join/verdict must reject doctored logs."""
    # Fixture: two suites with a duplicate (files, flags) pair so the
    # profile-id join is exercised the way the real suites need it
    # (wasm binary.js x2, AsmJs 5 duplicate groups in the real tree).
    import tempfile

    def write_rlexe(root, suite, entries):
        path = os.path.join(root, suite, "rlexe.xml")
        os.makedirs(os.path.dirname(path), exist_ok=True)
        body = ['<?xml version="1.0" encoding="utf-8"?>', "<regress-exe>"]
        for entry in entries:
            files, flags, baseline = entry[0], entry[1], entry[2]
            tags = entry[3] if len(entry) > 3 else ""
            body.append("  <test><default>")
            body.append("    <files>%s</files>" % files)
            if flags:
                body.append("    <compile-flags>%s</compile-flags>" % flags)
            if baseline:
                body.append("    <baseline>%s</baseline>" % baseline)
            if tags:
                body.append("    <tags>%s</tags>" % tags)
            body.append("  </default></test>")
        body.append("</regress-exe>")
        with open(path, "w", encoding="utf-8") as f:
            f.write("\n".join(body) + "\n")

    bad = 0

    def check(name, lines, test_root, man_entries, expected, want_ok,
              mode="interp"):
        nonlocal bad
        ids = {e["test-id"] for e in man_entries}
        p1 = [e["test-id"] for e in man_entries if e["phase"] == "P1"]
        id_map, _t2, _by = build_id_map(test_root, suites=["S1", "S2"])
        blocks = parse_blocks(lines)
        try:
            total = parse_total(normalize_lines(lines))
        except ValueError as e:
            total = None
            total_err = str(e)
        else:
            total_err = check_total(total, blocks)
        detail = grade_join(blocks, id_map, ids, p1, expected, mode)
        ok = bool(detail["ok"] and total_err is None)
        if ok == want_ok:
            print("self-test %-24s -> PASS (verdict=%s)"
                  % (name, "accept" if ok else "reject"))
        else:
            print("self-test %-24s -> FAIL (verdict=%s want %s)"
                  % (name, "accept" if ok else "reject",
                     "accept" if want_ok else "reject"))
            bad += 1

    with tempfile.TemporaryDirectory(prefix="wasm-p1-selftest-") as tmp:
        test_root = os.path.join(tmp, "test")
        # S1: distinct entries. S2: one duplicate (files, flags) pair.
        write_rlexe(test_root, "S1", [
            ("a.js", "-wasm", "a.baseline"),
            ("b.js", "", ""),
        ])
        write_rlexe(test_root, "S2", [
            ("dup.js", "-testtrace:asmjs", "dup.baseline"),
            ("dup.js", "-testtrace:asmjs", "dup.baseline"),
        ])
        man = [{"test-id": "S1[0]", "suite": "S1", "features": ["mvp"],
                "runnerTags": [], "phase": "P1"},
               {"test-id": "S1[1]", "suite": "S1", "features": ["mvp"],
                "runnerTags": [], "phase": "P1"},
               {"test-id": "S2[0]", "suite": "S2", "features": ["mvp"],
                "runnerTags": [], "phase": "P1"},
               {"test-id": "S2[1]", "suite": "S2", "features": ["mvp"],
                "runnerTags": [], "phase": "P1"}]
        id_map, _t2, _by = build_id_map(test_root, suites=["S1", "S2"])
        refs = [id_map[i] for i in sorted(id_map)]
        binary = "/fake/bin/ch"
        # Note: fixture test_root suffix is test/, so _cmd_line
        # fullpaths read .../test/S1/a.js, matching short names S1/a.js.
        good = _log_for(refs, {}, test_root, binary)
        check("exact green", good, test_root, man, expected=4, want_ok=True)
        check("duplicate pair joins", good, test_root, man, expected=4,
              want_ok=True)
        check("one failure",
              _log_for(refs, {"S1[1]": "Failed"}, test_root, binary),
              test_root, man, expected=4, want_ok=False)
        # Timeout block shape: runtests prints the timeout notice
        # between the result line and the command line. It must
        # still join (as a failure), never as unmatched.
        tout = _log_for(refs, {"S1[1]": "Failed"}, test_root, binary)
        tout_idx = 2 * 1 + 1  # command line for S1[1] (2nd block)
        timeout_lines = (tout[:tout_idx] + ["ERROR: Test timed out!"]
                         + tout[tout_idx:])
        ids = {e["test-id"] for e in man}
        p1 = [e["test-id"] for e in man if e["phase"] == "P1"]
        blocks = parse_blocks(timeout_lines)
        detail = grade_join(blocks, id_map, ids, p1, 4)
        if (detail["failed"] == ["S1[1]"] and not detail["unmatched"]
                and len(detail["passed"]) == 3):
            print("self-test %-24s -> PASS (timeout joins as "
                  "failure)" % "timeout block")
        else:
            print("self-test %-24s -> FAIL (failed=%r unmatched=%r)"
                  % ("timeout block", detail["failed"],
                     detail["unmatched"][:2]))
            bad += 1
        check("missing entry",
              _log_for(refs[:3], {}, test_root, binary),
              test_root, man, expected=4, want_ok=False)
        check("duplicate block", good + [good[0], good[1]],
              test_root, man, expected=4, want_ok=False)
        # Flags mismatch: rewrite one command's entry flags.
        bad_flags = list(good)
        ref0 = refs[0]
        bad_flags[1] = _cmd_line(binary, ref0, test_root).replace(
            ref0["flags"] if ref0["flags"] else "a.js",
            ref0["flags"] + " -bogus" if ref0["flags"] else "-bogus")
        check("flags mismatch", bad_flags, test_root, man, expected=4,
              want_ok=False)
        # Missing command line (non-verbose log).
        no_cmd = [good[0], ""] + good[2:]
        check("missing command", no_cmd, test_root, man, expected=4,
              want_ok=False)
        # Missing profile token.
        no_prof = list(good)
        no_prof[1] = no_prof[1].replace(
            "-dynamicprofilecache:profile.dpl.%d" % refs[0]["id"], "")
        check("missing profile", no_prof, test_root, man, expected=4,
              want_ok=False)
        # Profile id out of range.
        out_of_range = list(good)
        out_of_range[1] = out_of_range[1].replace(
            "profile.dpl.%d" % refs[0]["id"], "profile.dpl.999")
        check("id out of range", out_of_range, test_root, man, expected=4,
              want_ok=False)
        # Folder mismatch: keep id, change short folder.
        folded = list(good)
        folded[0] = folded[0].replace("S1/a.js", "S2/a.js", 1)
        check("folder mismatch", folded, test_root, man, expected=4,
              want_ok=False)
        # Relative binary.
        rel = list(good)
        rel[1] = rel[1].replace(binary, "bin/ch", 1)
        check("relative binary", rel, test_root, man, expected=4,
              want_ok=False)
        # Wrong variant (no -nonative).
        wrong_var = list(good)
        wrong_var[1] = _cmd_line(binary, refs[0], test_root,
                                 mode="wrong")
        check("wrong variant", wrong_var, test_root, man, expected=4,
              want_ok=False)
        # JIT mode: exact green joins; cross-mode logs reject.
        good_jit = _log_for(refs, {}, test_root, binary, mode="jit")
        check("jit exact green", good_jit, test_root, man, expected=4,
              want_ok=True, mode="jit")
        check("jit one failure",
              _log_for(refs, {"S1[1]": "Failed"}, test_root, binary,
                       mode="jit"),
              test_root, man, expected=4, want_ok=False, mode="jit")
        check("interp log under jit mode", good, test_root, man,
              expected=4, want_ok=False, mode="jit")
        check("jit log under interp mode", good_jit, test_root, man,
              expected=4, want_ok=False)
        # JIT missing profile token / missing -forceNative reject.
        no_prof_jit = list(good_jit)
        no_prof_jit[1] = no_prof_jit[1].replace(
            "-dynamicprofileinput:profile.dpl.%d" % refs[0]["id"], "")
        check("jit missing profile", no_prof_jit, test_root, man,
              expected=4, want_ok=False, mode="jit")
        no_force = list(good_jit)
        no_force[1] = no_force[1].replace(" -forceNative", "")
        check("jit missing forceNative", no_force, test_root, man,
              expected=4, want_ok=False, mode="jit")
        # Pinned entries (entry-level -nonative): ran interp-pinned
        # passes, dynapogo-excluded entries are expected absent.
        write_rlexe(test_root, "S3", [
            ("p.js", "-wasm -nonative", "p.baseline", "exclude_dynapogo"),
            ("q.js", "-testtrace:asmjs -nonative", "q.baseline"),
        ])
        write_rlexe(test_root, "S4", [
            ("r.js", "-wasm -nonative", "r.baseline"),
            ("s.js", "-wasm", "s.baseline", "exclude_dynapogo"),
        ])
        man34 = [{"test-id": "S3[0]", "suite": "S3", "features": ["mvp"],
                  "runnerTags": [], "phase": "P1"},
                 {"test-id": "S3[1]", "suite": "S3", "features": ["mvp"],
                  "runnerTags": [], "phase": "P1"},
                 {"test-id": "S4[0]", "suite": "S4", "features": ["mvp"],
                  "runnerTags": [], "phase": "P1"},
                 {"test-id": "S4[1]", "suite": "S4", "features": ["mvp"],
                  "runnerTags": [], "phase": "P1"}]
        id_map34, _t2, _by = build_id_map(test_root,
                                          suites=["S3", "S4"])
        refs34 = [id_map34[i] for i in sorted(id_map34)]
        # refs34 order (ids restart per build_id_map call over the
        # given suites): S3[0]=1, S3[1]=2, S4[0]=3, S4[1]=4.
        qref = refs34[1]
        qlog = _log_for([qref], {}, test_root, binary, mode="jit")
        ids34 = {e["test-id"] for e in man34}
        p34 = [e["test-id"] for e in man34 if e["phase"] == "P1"]
        blocks = parse_blocks(qlog)
        detail = grade_join(blocks, id_map34, ids34, p34, 4, "jit")
        if (detail["ok"] is False
                and detail["pinned_passed"] == ["S3[1]"]
                and detail["excluded"] == ["S3[0]", "S4[1]"]
                and detail["missing"] == ["S4[0]"]):
            print("self-test %-24s -> PASS (pinned/excluded split)"
                  % "jit pinned mix")
        else:
            print("self-test %-24s -> FAIL (pinned=%r excluded=%r "
                  "missing=%r ok=%r)"
                  % ("jit pinned mix", detail["pinned_passed"],
                     detail["excluded"], detail["missing"], detail["ok"]))
            bad += 1
        check("jit pinned failure",
              _log_for([qref], {"S3[1]": "Failed"}, test_root, binary,
                       mode="jit"),
              test_root, man34, expected=4, want_ok=False, mode="jit")
        # Pinned entry missing without an exclusion tag must fail.
        rlog = _log_for([], {}, test_root, binary, mode="jit")
        check("jit pinned missing untagged", rlog, test_root, man34,
              expected=4, want_ok=False, mode="jit")
        # -nonative command for a NON-pinned entry must not join.
        sneaky = list(good_jit)
        sneaky[1] = sneaky[1].replace(" -bgJitDelay:0",
                                      " -bgJitDelay:0 -nonative", 1)
        check("jit nonative nonpinned", sneaky, test_root, man,
              expected=4, want_ok=False, mode="jit")
        # Total doctored: duplicate / missing / off-by-one.
        check("duplicate Total", good + [good[-1]], test_root, man,
              expected=4, want_ok=False)
        check("missing Total", good[:-1], test_root, man, expected=4,
              want_ok=False)
        off = list(good)
        off[-1] = "Total: passed 3, failed 0"
        check("Total off-by-one", off, test_root, man, expected=4,
              want_ok=False)
        # P1 pin drift: manifest has 4, pin demands 3.
        check("pin drift", good, test_root, man, expected=3,
              want_ok=False)
        # Lookalike Total must not count as the summary.
        look = [good[0], good[1], "X" + good[-1], good[-1] + " wisely",
                ] + good[2:-1]
        check("lookalike Total", look, test_root, man, expected=4,
              want_ok=False)
        # Real-manifest pin: the checked-in manifest must hold 381 P1.
        real_path = os.path.join(ROOT, "test", "wasm-arm64-manifest.json")
        entries, _m = load_manifest(real_path)
        p1n = sum(1 for e in entries if e["phase"] == "P1")
        if p1n == EXPECTED_P1_TOTAL:
            print("self-test %-24s -> PASS (P1 pin %d)" % ("real pin",
                                                         EXPECTED_P1_TOTAL))
        else:
            print("self-test %-24s -> FAIL (P1 pin got %d)"
                  % ("real pin", p1n))
            bad += 1
    # Outer-ceiling pin: a hung child must raise loudly, not hang.
    try:
        common.run_tee([sys.executable, "-c",
                        "import time; time.sleep(30)"], timeout=1)
        print("self-test %-24s -> FAIL (no timeout raised)" % "hung child")
        bad += 1
    except subprocess.TimeoutExpired:
        print("self-test %-24s -> PASS (TimeoutExpired raised)"
              % "hung child")
    print("check_wasm_p1 self-test: %s"
          % ("ALL NEGATIVE CONTROLS BEHAVE" if bad == 0
             else "%d CONTROL(S) MISBEHAVE" % bad))
    return 1 if bad else 0


def main(argv):
    args = list(argv[1:])
    if "--selftest" in args or "--self-test" in args:
        return self_test()
    log_path = None
    manifest_path = None
    jit = False
    rest = []
    i = 0
    while i < len(args):
        if args[i] == "--log":
            log_path = args[i + 1]
            i += 2
        elif args[i] == "--manifest":
            manifest_path = args[i + 1]
            i += 2
        elif args[i] == "--jit":
            jit = True
            i += 1
        else:
            rest.append(args[i])
            i += 1
    mode = "jit" if jit else "interp"
    if log_path is not None:
        # Replay mode: positionals optional (0 or 2). With 2, the
        # runtests path locates the test root; the binary is unused.
        if len(rest) == 0:
            test_root = os.path.join(ROOT, "test")
        elif len(rest) == 2:
            runtests = rest[0]
            if not os.path.isfile(runtests):
                print("check_wasm_p1: missing file: %s" % runtests,
                      file=sys.stderr)
                return 2
            test_root = os.path.dirname(os.path.abspath(runtests))
        else:
            print("usage: check_wasm_p1.py [--jit] [--log SAVED.log] "
                  "[<runtests.py> <ch>] [--manifest PATH]", file=sys.stderr)
            return 2
        if manifest_path is None:
            manifest_path = os.path.join(test_root,
                                         "wasm-arm64-manifest.json")
        if not os.path.isfile(log_path):
            print("check_wasm_p1: missing file: %s" % log_path,
                  file=sys.stderr)
            return 2
        if not os.path.isfile(manifest_path):
            print("check_wasm_p1: missing file: %s" % manifest_path,
                  file=sys.stderr)
            return 2
        with open(log_path, "r", encoding="utf-8", errors="replace") as f:
            lines = f.readlines()
        print("check_wasm_p1: replaying log %s (mode %s)"
              % (log_path, mode))
        rc, _detail = grade_log(lines, test_root, manifest_path,
                                mode=mode)
        return rc
    if len(rest) != 2:
        print("usage: check_wasm_p1.py [--jit] <runtests.py> <ch> "
              "[--manifest PATH]", file=sys.stderr)
        return 2
    runtests, ch = rest[0], rest[1]
    for path in (runtests, ch):
        if not os.path.isfile(path):
            print("check_wasm_p1: missing file: %s" % path, file=sys.stderr)
            return 2
    test_root = os.path.dirname(os.path.abspath(runtests))
    if manifest_path is None:
        manifest_path = os.path.join(test_root,
                                     "wasm-arm64-manifest.json")
    if not os.path.isfile(manifest_path):
        print("check_wasm_p1: missing file: %s" % manifest_path,
              file=sys.stderr)
        return 2
    rc, lines = run_suite(runtests, ch, jit)
    if rc == 124:
        return 1
    print("check_wasm_p1: exit=%d" % rc)
    grc, _detail = grade_log(lines, test_root, manifest_path,
                             mode=mode)
    # Non-P1 failures also set runtests exit 1; they are not the P1
    # verdict (task contract: nonzero iff P1 failed or join broke).
    # Only an unexpected runtests exit (not 0/1) overrides the grade.
    if rc not in (0, 1):
        print("check_wasm_p1: FAIL: runtests.py exited unexpectedly %d"
              % rc, file=sys.stderr)
        return 1
    return grc


if __name__ == "__main__":
    sys.exit(main(sys.argv))
