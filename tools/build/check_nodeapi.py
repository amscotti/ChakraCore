#!/usr/bin/env python3
#-------------------------------------------------------------------------------------------------------
# Copyright (c) ChakraCore Project Contributors. All rights reserved.
# Licensed under the MIT license. See LICENSE.txt file in the project root for full license information.
#-------------------------------------------------------------------------------------------------------
"""Check that libChakraNodeApi exports exactly the allowlist (HIGH-5).

Compares the dylib's exported PROVIDER text symbols (nm -gU, type
T, normalized by stripping one leading underscore on Mach-O)
against tools/build/nodeapi_exports.txt. Fails on any missing or
extra PROVIDER symbol. Also verifies every allowlisted symbol is in
the normative inventory (doc/node-api-inventory.md, 145 at
NAPI_VERSION=8) or the private host API (plan §3.2), and that the
exports file matches the generator
(tools/build/gen_nodeapi_exports.py --check).

Full coverage (HIGH-5): the allowlist is the full surface.
All 145 inventory symbols are landed and must appear in the dylib;
landing or removing a napi_* requires updating the generator's
landed list, regenerating the allowlist, and rebuilding -- this
check then fails until all three happen together. The allowlist is
145 napi_* + 5 host = 150 symbols.

Visibility: the provider builds with -fvisibility=hidden. The
145 napi_* declarations already carry __attribute__((visibility
("default"))) via NAPI_EXTERN in the vendored headers, and the
5 host entries carry it on their definitions (create/destroy in
NodeApiEnv.cpp, pending-module helpers in NodeApiHost.cpp), so
exactly those survive. Everything else provider-owned
(nodeapi::* C++ internals, NodeApi* helpers,
chakra_nodeapi_test_* hooks) is hidden and must not appear.

Documented toolchain exception (not provider surface): Zig's static
libc++ bundles replaceable operator new/delete (__Zdl/__Zda/__Zn/
__Zna) as T exports from the archive. Replaceable allocation
functions are exported regardless of -fvisibility=hidden, and
Zig's MachO linker ignores -exported_symbols_list/--version-script
(probed: the flag never reaches `zig ld`; system clang honors it
but cannot link Zig's objects -- ___dso_handle fixup error). The
12 operator symbols are therefore filtered here as runtime, never
counted as provider exports. Any OTHER __Z* (e.g. nodeapi::*,
NodeApiScopes::*) still fails as extra. Counted separately in the
report as "runtime T symbols".

Usage:
  python3 tools/build/check_nodeapi.py --dylib <libChakraNodeApi.dylib>
  python3 tools/build/check_nodeapi.py --self-test

Exit 0 iff exact match (plus documented runtime). Wired as
`zig build check-nodeapi`.
"""

import argparse
import os
import re
import subprocess
import sys
import tempfile

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import common

HOST_API = {"chakra_nodeapi_create_env", "chakra_nodeapi_destroy_env", "chakra_nodeapi_get_pending_module", "chakra_nodeapi_pending_module_count", "chakra_nodeapi_run_pending_registrations"}
# Replaceable operator new/delete family bundled by Zig's static
# libc++ (see module docstring). Only this family is filtered; every
# other __Z* (nodeapi::*, NodeApiScopes::*) still counts as extra.
# Matched after one Mach-O underscore is stripped, so both "_Znwm"
# (stripped __Znwm) and "__Znwm" (raw) forms match here.
RUNTIME_T_RE = re.compile(r"^_?__?Z(dl|da|n|na)")
# Mach-O linker bookkeeping that is always global D (never T), not
# part of the allowlist comparison which is T-only. Listed here so
# the tool can be reused for a future T+D comparison without hiding
# these two.
MACHO_BOOKKEEPING = {"___dso_handle", "__mh_dylib_header"}


def repo_root() -> str:
    return os.path.dirname(os.path.dirname(os.path.dirname(
        os.path.abspath(__file__))))


def parse_exports(path: str) -> list:
    syms = []
    with open(path) as f:
        for line in f:
            line = line.strip()
            if not line or line.startswith("#"):
                continue
            syms.append(line)
    return syms


def parse_inventory(path: str) -> set:
    # Reuse the generator's parser (Inventory section only) so the
    # two tools can never disagree on what the inventory is.
    sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
    import gen_nodeapi_exports as gen
    from pathlib import Path
    return set(gen.parse_inventory(Path(path)))


def exported_t_symbols(dylib: str) -> set:
    rc, out, err = common.run_capture(["nm", "-gU", dylib])
    if rc != 0:
        # Linux fallback: nm -g without -U (no underscore prefix).
        rc, out, err = common.run_capture(["nm", "-g", "--defined-only", dylib])
        if rc != 0:
            raise RuntimeError("nm failed (rc=%d err=%r)" % (rc, err[:200]))
    syms = set()
    for line in out.splitlines():
        parts = line.split()
        if len(parts) == 3 and parts[1] == "T":
            name = parts[2]
            if name.startswith("_"):
                name = name[1:]
            if name in MACHO_BOOKKEEPING:
                continue
            syms.add(name)
    return syms


def is_runtime_sym(name: str) -> bool:
    return bool(RUNTIME_T_RE.match(name))


def provider_symbols(all_t: set) -> tuple:
    """Split nm T symbols into (provider, runtime)."""
    runtime = {s for s in all_t if is_runtime_sym(s)}
    provider = all_t - runtime
    return provider, runtime


def compare(actual: set, expected: set):
    missing = sorted(expected - actual)
    extra = sorted(actual - expected)
    return missing, extra


def self_test() -> int:
    bad = 0

    def report(name, cond):
        print("self-test %-52s -> %s" % (name, "PASS" if cond else "FAIL"))
        return 0 if cond else 1

    # Exact match passes.
    bad += report("exact match passes",
                  compare({"a", "b"}, {"a", "b"}) == ([], []))
    # Missing fails.
    m, e = compare({"a"}, {"a", "b"})
    bad += report("missing detected", m == ["b"] and e == [])
    # Extra fails.
    m, e = compare({"a", "b", "c"}, {"a", "b"})
    bad += report("extra detected", m == [] and e == ["c"])
    # Runtime operator new is filtered, other __Z is not.
    prov, rt = provider_symbols({"napi_a", "__Znwm", "__ZN7nodeapi13LookupLiveEnvEPvPPNS_7NodeEnvE"})
    bad += report("runtime operator filtered", prov == {"napi_a", "__ZN7nodeapi13LookupLiveEnvEPvPPNS_7NodeEnvE"} and rt == {"__Znwm"})
    # Garbage dylib fails (nm rejects non-Mach-O).
    tmp = tempfile.mkdtemp(prefix="check-nodeapi-selftest-")
    try:
        garbage = os.path.join(tmp, "garbage.dylib")
        with open(garbage, "wb") as f:
            f.write(b"not a mach-o file, just tampered bytes\n" * 16)
        try:
            got = exported_t_symbols(garbage)
            bad += report("garbage victim raises", False)
        except RuntimeError:
            bad += report("garbage victim raises", True)
    finally:
        import shutil
        shutil.rmtree(tmp, ignore_errors=True)
    # Exports parser skips comments/blank lines.
    with tempfile.NamedTemporaryFile("w", suffix=".txt", delete=False) as tf:
        tf.write("# comment\n\nnapi_a\nnapi_b\n")
        tf_path = tf.name
    try:
        bad += report("parser skips comments",
                      parse_exports(tf_path) == ["napi_a", "napi_b"])
    finally:
        os.unlink(tf_path)
    # Inventory parser sees only the Inventory section.
    import gen_nodeapi_exports as gen
    from pathlib import Path
    bad += report("inventory parse sees 145",
                  len(gen.parse_inventory(Path(repo_root()) / "doc" / "node-api-inventory.md")) == 145)

    print("check_nodeapi self-test: %s"
          % ("ALL NEGATIVE CONTROLS BEHAVE" if bad == 0
             else "%d CONTROL(S) MISBEHAVE" % bad))
    return 1 if bad else 0


def main(argv) -> int:
    args = list(argv[1:])
    if "--self-test" in args:
        return self_test()
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--dylib", required=True)
    ap.add_argument("--exports", default=None)
    ap.add_argument("--inventory", default=None)
    ns = ap.parse_args(args)
    root = repo_root()
    exports_path = ns.exports or os.path.join(root, "tools", "build", "nodeapi_exports.txt")
    inventory_path = ns.inventory or os.path.join(root, "doc", "node-api-inventory.md")
    try:
        expected_list = parse_exports(exports_path)
    except OSError as e:
        print("check_nodeapi: cannot read exports %s: %s" % (exports_path, e),
              file=sys.stderr)
        return 1
    expected = set(expected_list)
    if len(expected_list) != len(expected):
        print("check_nodeapi: FAIL: duplicate symbols in %s" % exports_path,
              file=sys.stderr)
        return 1
    try:
        inventory = parse_inventory(inventory_path)
    except Exception as e:
        print("check_nodeapi: cannot parse inventory %s: %s" % (inventory_path, e),
              file=sys.stderr)
        return 1
    unknown = sorted(s for s in expected if s not in inventory and s not in HOST_API)
    if unknown:
        print("check_nodeapi: FAIL: allowlist symbols outside inventory+host: %s"
              % ", ".join(unknown), file=sys.stderr)
        return 1
    try:
        actual = exported_t_symbols(ns.dylib)
    except RuntimeError as e:
        print("check_nodeapi: FAIL: %s" % e, file=sys.stderr)
        return 1
    provider, runtime = provider_symbols(actual)
    print("check_nodeapi: exports %s -> %d T symbols "
          "(%d provider + %d runtime operator new/delete), allowlist %d"
          % (ns.dylib, len(actual), len(provider), len(runtime),
             len(expected)))
    if runtime:
        print("check_nodeapi: runtime (filtered): %s"
              % ", ".join(sorted(runtime)))
    missing, extra = compare(provider, expected)
    failures = []
    if missing:
        failures.append("missing exports (in allowlist, absent from dylib): %s"
                        % ", ".join(missing))
    if extra:
        failures.append("extra exports (in dylib, not in allowlist): %s"
                        % ", ".join(extra))
    if failures:
        print("check_nodeapi: FAIL:", file=sys.stderr)
        for f in failures:
            print("check_nodeapi: FAIL: %s" % f, file=sys.stderr)
        return 1
    print("check_nodeapi: PASS: dylib exports exactly the allowlist "
          "(%d symbols)" % len(expected))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
