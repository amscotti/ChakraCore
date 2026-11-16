#!/usr/bin/env python3
#-------------------------------------------------------------------------------------------------------
# Copyright (C) Microsoft. All rights reserved.
# Copyright (c) ChakraCore Project Contributors. All rights reserved.
# Licensed under the MIT license. See LICENSE.txt file in the project root for full license information.
#-------------------------------------------------------------------------------------------------------
"""Portable ASCII and tab style checks for macOS and Linux (Python 3).

Replaces GNU `grep -P` in check_ascii.sh / check_file_ascii.sh / check_tabs.sh
so macOS BSD grep never silently fails with exit 2 and passes vacuously.
"""

import argparse
import os
import re
import shutil
import subprocess
import sys
import tempfile

# Directory and extension exclusions matching check_ascii.sh intent
# (fixing the historical `/*.md` typo that accidentally matched `amd64`/`Simd`).
ASCII_EXCLUDE_RE = re.compile(
    r"(?:^|/)(?:test|tools|Build)/|\.(?:md|txt|vcxproj)$"
)

# Exclusions matching check_tabs.sh plus vendored Node-API trees and
# committed log/timing captures (matching check_copyright.sh exclusions).
TABS_EXCLUDE_RE = re.compile(
    r"^pal/|(?:^|/)Makefile$|(?:^|/)Makefile\.sample$|\.sln$|\.js$|"
    r"\.baseline$|\.wasm$|\.wast$|\.png$|^lib/wabt/|^bin/External/|"
    r"^include/nodeapi/|^test/nodeapi/vendor/|\.(?:log|time)$"
)


def is_ascii_excluded(path):
    return bool(ASCII_EXCLUDE_RE.search(path))


def is_tabs_excluded(path):
    return bool(TABS_EXCLUDE_RE.search(path))


def split_lf_lines(data):
    """Split raw bytes strictly on LF so CR/VT/FF/FS/GS/RS are preserved."""
    if not data:
        return []
    lines = data.split(b"\n")
    if data.endswith(b"\n"):
        lines.pop()
    return lines


def find_non_ascii_lines(data):
    """Return list of (1-based line_no, raw_line_bytes) violating 0x09..0x7E."""
    bad = []
    for idx, line in enumerate(split_lf_lines(data), 1):
        for b in line:
            if b < 0x09 or b > 0x7E:
                bad.append((idx, line))
                break
    return bad


def format_hex_sample(line_no, line_bytes, max_bytes=48):
    chunk = line_bytes[:max_bytes]
    hex_part = " ".join("%02x" % b for b in chunk)
    if len(line_bytes) > max_bytes:
        hex_part += " ..."
    ascii_part = "".join(chr(b) if 0x20 <= b <= 0x7E else "." for b in chunk)
    return "%6d: %-48s  %s" % (line_no, hex_part, ascii_part)


def check_ascii_file(path, err_lines, verbose=True):
    """Check a single file for non-ASCII bytes. Returns True iff clean."""
    if verbose:
        print("Check ascii > Checking %s" % path)
    if not os.path.exists(path):
        if verbose:
            print("WARNING: file not found: %s" % path)
        return True
    if not os.path.isfile(path):
        return True
    with open(path, "rb") as f:
        data = f.read()
    bad = find_non_ascii_lines(data)
    if not bad:
        return True
    err_lines.append("ERROR: non-ascii characters were introduced in %s" % path)
    err_lines.append(
        "Displaying first 10 lines of text where non-ascii characters were found:"
    )
    for line_no, line_bytes in bad[:10]:
        err_lines.append(format_hex_sample(line_no, line_bytes))
    err_lines.append("Total lines containing non-ascii: %d" % len(bad))
    err_lines.append("--------------")
    return False


def find_tab_lines(data):
    """Return list of (1-based line_no, raw_line_bytes) containing a tab."""
    hits = []
    for idx, line in enumerate(split_lf_lines(data), 1):
        if b"\t" in line:
            hits.append((idx, line))
    return hits


def git_blame_tab_lines(path, blame_ref="HEAD"):
    """Run `git blame` for a file and return matching tab lines as str."""
    try:
        proc = subprocess.run(
            ["git", "blame", blame_ref, "--", path],
            capture_output=True,
            check=False,
        )
    except OSError:
        return None
    if proc.returncode != 0:
        return None
    lines = []
    for raw in proc.stdout.split(b"\n"):
        if b"\t" in raw:
            lines.append(raw.decode("utf-8", errors="replace"))
    return lines or None


def check_tabs_files(paths, err_lines, blame_ref="HEAD", use_git_blame=True):
    """Check files for literal tab bytes. Returns True iff all clean."""
    clean = True
    for path in paths:
        if not os.path.isfile(path):
            continue
        with open(path, "rb") as f:
            data = f.read()
        if b"\t" not in data:
            continue
        clean = False
        err_lines.append("--- IN FILE %s ---" % path)
        blamed = git_blame_tab_lines(path, blame_ref) if use_git_blame else None
        if blamed is not None:
            err_lines.extend(blamed)
        else:
            for line_no, line_bytes in find_tab_lines(data):
                err_lines.append(
                    "%d: %s"
                    % (line_no, line_bytes.decode("utf-8", errors="replace"))
                )
    return clean


def git_changed_files(base_ref="origin/master", head_ref="HEAD"):
    """Return list of files changed between merge-base(base_ref, head_ref) and head_ref."""
    mb = subprocess.run(
        ["git", "merge-base", base_ref, head_ref],
        capture_output=True,
        text=True,
    )
    if mb.returncode != 0 or not mb.stdout.strip():
        raise RuntimeError(
            "git merge-base %s %s failed (rc=%d): %s"
            % (base_ref, head_ref, mb.returncode, mb.stderr.strip())
        )
    base = mb.stdout.strip()
    diff = subprocess.run(
        ["git", "diff", "--name-only", base, head_ref],
        capture_output=True,
        text=True,
    )
    if diff.returncode != 0:
        raise RuntimeError(
            "git diff --name-only %s %s failed (rc=%d): %s"
            % (base, head_ref, diff.returncode, diff.stderr.strip())
        )
    return [line for line in diff.stdout.splitlines() if line]


def write_errfile(errfile, err_lines, append=False):
    if not err_lines:
        return
    mode = "a" if append else "w"
    with open(errfile, mode, encoding="utf-8") as f:
        for line in err_lines:
            f.write(line + "\n")


def run_self_test():
    bad = 0

    def report(name, cond):
        print("self-test %-48s -> %s" % (name, "PASS" if cond else "FAIL"))
        return 0 if cond else 1

    tmp = tempfile.mkdtemp(prefix="style-portable-selftest-")
    try:
        clean_cpp = os.path.join(tmp, "clean.cpp")
        with open(clean_cpp, "wb") as f:
            f.write(b"// ASCII only\tint ok = 1;\r\n")
        bad += report("ascii clean file passes (tab + CRLF allowed)",
                      check_ascii_file(clean_cpp, [], verbose=False))

        utf8_cpp = os.path.join(tmp, "nonascii.cpp")
        with open(utf8_cpp, "wb") as f:
            f.write(b"// em-dash \xe2\x80\x94 here\n")
        errs = []
        ok = check_ascii_file(utf8_cpp, errs, verbose=False)
        bad += report("ascii non-UTF8/UTF-8 high byte fails",
                      (not ok) and any("non-ascii" in ln for ln in errs))

        ctrl_cpp = os.path.join(tmp, "ctrl.cpp")
        with open(ctrl_cpp, "wb") as f:
            f.write(b"// control \x07 bell\n")
        bad += report("ascii control char (<0x09) fails",
                      not check_ascii_file(ctrl_cpp, [], verbose=False))

        del_cpp = os.path.join(tmp, "del.cpp")
        with open(del_cpp, "wb") as f:
            f.write(b"// del \x7f char\n")
        bad += report("ascii DEL (0x7f) fails",
                      not check_ascii_file(del_cpp, [], verbose=False))

        bad += report("ascii LF split preserves line numbering across 0x1d",
                      find_non_ascii_lines(b"// ok\n// sep\x1d\x80\n")
                      == [(2, b"// sep\x1d\x80")]
                      and find_non_ascii_lines(b"// bad \x01\n")
                      == [(1, b"// bad \x01")])

        missing = os.path.join(tmp, "deleted.cpp")
        bad += report("ascii missing file warns and passes",
                      check_ascii_file(missing, [], verbose=False))

        tab_cpp = os.path.join(tmp, "tab.cpp")
        with open(tab_cpp, "wb") as f:
            f.write(b"int x = 1;\n\tint y = 2;\n")
        tab_errs = []
        tab_ok = check_tabs_files([tab_cpp], tab_errs, use_git_blame=False)
        bad += report("tabs file with literal tab fails",
                      (not tab_ok)
                      and any("--- IN FILE" in ln for ln in tab_errs)
                      and any("\t" in ln for ln in tab_errs))

        notab_cpp = os.path.join(tmp, "notab.cpp")
        with open(notab_cpp, "wb") as f:
            f.write(b"int x = 1;\n    int y = 2;\n")
        bad += report("tabs clean file passes",
                      check_tabs_files([notab_cpp], [], use_git_blame=False))

        # Exclusion checks
        bad += report("ascii excludes test/, tools/, Build/, .md/.txt",
                      is_ascii_excluded("test/foo.cpp")
                      and is_ascii_excluded("tools/foo.py")
                      and is_ascii_excluded("Build/foo.props")
                      and is_ascii_excluded("doc/foo.md")
                      and is_ascii_excluded("README.txt"))
        bad += report("ascii checks amd64/ and Simd* C++ files",
                      not is_ascii_excluded("lib/Backend/amd64/EncoderMD.cpp")
                      and not is_ascii_excluded("lib/Runtime/Language/SimdInt8x16Operation.cpp"))
        bad += report("tabs excludes pal/, .js, .log, .time, vendored",
                      is_tabs_excluded("pal/src/init/pal.cpp")
                      and is_tabs_excluded("test/a.js")
                      and is_tabs_excluded("out/e14.log")
                      and is_tabs_excluded("tools/legacy-baseline/logs/10.time")
                      and is_tabs_excluded("test/nodeapi/vendor/js-native-api/LICENSE")
                      and is_tabs_excluded("include/nodeapi/node_api.h"))
        bad += report("tabs checks C++/Zig/Python/YAML files",
                      not is_tabs_excluded("lib/Runtime/Library/JavascriptArray.cpp")
                      and not is_tabs_excluded("build.zig")
                      and not is_tabs_excluded(".github/workflows/zig-macos.yml"))

        # CLI negative controls via subprocess
        err_out = os.path.join(tmp, "cli.err")
        p1 = subprocess.run(
            [sys.executable, __file__, "--mode", "ascii",
             "--errfile", err_out, utf8_cpp],
            capture_output=True, text=True,
        )
        bad += report("CLI ascii exits nonzero on violation",
                      p1.returncode == 1 and os.path.isfile(err_out))
        err_out2 = os.path.join(tmp, "tabs.err")
        p2 = subprocess.run(
            [sys.executable, __file__, "--mode", "tabs",
             "--no-git-blame", "--errfile", err_out2, tab_cpp],
            capture_output=True, text=True,
        )
        bad += report("CLI tabs exits nonzero on violation",
                      p2.returncode == 1 and os.path.isfile(err_out2))
        err_out3 = os.path.join(tmp, "clean.err")
        p3 = subprocess.run(
            [sys.executable, __file__, "--mode", "tabs",
             "--no-git-blame", "--errfile", err_out3, notab_cpp],
            capture_output=True, text=True,
        )
        bad += report("CLI tabs exits 0 on clean file",
                      p3.returncode == 0 and not os.path.exists(err_out3))
    finally:
        shutil.rmtree(tmp, ignore_errors=True)

    print(
        "check_style_portable self-test: %s"
        % ("ALL NEGATIVE CONTROLS BEHAVE" if bad == 0
           else "%d CONTROL(S) MISBEHAVE" % bad)
    )
    return 1 if bad else 0


def main(argv=None):
    parser = argparse.ArgumentParser(
        description="Portable ASCII and tab style checker for ChakraCore."
    )
    parser.add_argument(
        "--mode",
        choices=("ascii", "ascii-file", "tabs"),
        help="Check mode",
    )
    parser.add_argument(
        "--stdin",
        action="store_true",
        help="Read newline-separated file paths from stdin",
    )
    parser.add_argument(
        "--git-diff",
        action="store_true",
        help="Collect changed paths from git merge-base <base> <head>",
    )
    parser.add_argument(
        "--base",
        default=os.environ.get("STYLECHECK_BASE", "origin/master"),
        help="Base ref for --git-diff (default: origin/master)",
    )
    parser.add_argument(
        "--head",
        default="HEAD",
        help="Head ref for --git-diff (default: HEAD)",
    )
    parser.add_argument(
        "--errfile",
        default=None,
        help="Write error report to this file",
    )
    parser.add_argument(
        "--no-git-blame",
        action="store_true",
        help="Do not invoke git blame for tab diagnostics",
    )
    parser.add_argument(
        "--quiet",
        action="store_true",
        help="Suppress per-file progress stdout",
    )
    parser.add_argument(
        "--self-test",
        action="store_true",
        help="Run built-in positive and negative controls",
    )
    parser.add_argument("files", nargs="*", help="Files to inspect")
    args = parser.parse_args(argv)

    if args.self_test:
        return run_self_test()

    if not args.mode:
        parser.error("--mode is required unless --self-test is specified")

    paths = list(args.files)
    if args.stdin:
        paths.extend(line.rstrip("\r\n") for line in sys.stdin if line.strip())
    if args.git_diff:
        try:
            paths.extend(git_changed_files(args.base, args.head))
        except RuntimeError as exc:
            print("style check error: %s" % exc, file=sys.stderr)
            return 2

    if args.mode == "ascii-file":
        if len(paths) != 1:
            print("ascii-file mode requires exactly one file path", file=sys.stderr)
            return 2
        err_lines = []
        check_ascii_file(paths[0], err_lines, verbose=not args.quiet)
        if args.errfile and err_lines:
            write_errfile(args.errfile, err_lines, append=True)
        return 0

    if args.mode == "ascii":
        filtered = [p for p in paths if not is_ascii_excluded(p)]
        err_lines = []
        clean = True
        for p in filtered:
            if not check_ascii_file(p, err_lines, verbose=not args.quiet):
                clean = False
        if args.errfile and not clean:
            write_errfile(args.errfile, err_lines, append=False)
        return 0 if clean else 1

    # args.mode == "tabs"
    filtered = [p for p in paths if not is_tabs_excluded(p)]
    err_lines = []
    clean = check_tabs_files(
        filtered,
        err_lines,
        blame_ref=args.head,
        use_git_blame=not args.no_git_blame,
    )
    if args.errfile and not clean:
        write_errfile(args.errfile, err_lines, append=False)
    return 0 if clean else 1


if __name__ == "__main__":
    sys.exit(main())
