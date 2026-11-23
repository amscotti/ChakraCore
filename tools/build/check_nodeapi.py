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
the normative inventory (doc/node-api-inventory.md, 155 at
NAPI_VERSION=10) or the private host API (plan §3.2). The
exports-file-vs-generator check
(tools/build/gen_nodeapi_exports.py --check) is a separate step
owned by `zig build check-nodeapi`, not run by this script.

Full coverage (HIGH-5): the allowlist is the full surface.
All 155 inventory symbols are landed and must appear in the dylib;
landing or removing a napi_*/node_api_* requires updating the generator's
landed list, regenerating the allowlist, and rebuilding -- this
check then fails until all three happen together. The allowlist is
155 napi_*/node_api_* + 6 host = 161 symbols.

Visibility: the provider builds with -fvisibility=hidden. The
155 napi_*/node_api_* declarations already carry __attribute__((visibility
("default"))) via NAPI_EXTERN in the vendored headers, and the
6 host entries carry it on their definitions (create/destroy/set_version in
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
  python3 tools/build/check_nodeapi.py --dll <libChakraNodeApi.dll>
  python3 tools/build/check_nodeapi.py --self-test

Exit 0 iff exact match (plus documented runtime). Wired as
`zig build check-nodeapi`.
"""

import argparse
import os
import re
import struct
import subprocess
import sys
import tempfile

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import common

HOST_API = {
    "chakra_nodeapi_create_env",
    "chakra_nodeapi_destroy_env",
    "chakra_nodeapi_get_pending_module",
    "chakra_nodeapi_pending_module_count",
    "chakra_nodeapi_run_pending_registrations",
    "chakra_nodeapi_set_module_api_version",
}
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


def read_pe_exports(path: str) -> list:
    """Parse named exports from a PE32/PE32+ image's Export Directory."""
    with open(path, "rb") as f:
        data = f.read()
    if len(data) < 64 or data[:2] != b"MZ":
        raise ValueError("not a PE/MZ image")
    e_lfanew = struct.unpack_from("<I", data, 0x3C)[0]
    if e_lfanew < 64 or e_lfanew + 24 > len(data):
        raise ValueError("invalid PE e_lfanew 0x%x" % e_lfanew)
    if data[e_lfanew:e_lfanew + 4] != b"PE\x00\x00":
        raise ValueError("missing PE signature")
    coff_off = e_lfanew + 4
    _machine, num_sections, _ts, _sym_ptr, _num_syms, opt_size, _chars = (
        struct.unpack_from("<HHIIIHH", data, coff_off)
    )
    opt_off = coff_off + 20
    if opt_size < 2 or opt_off + opt_size > len(data):
        raise ValueError("invalid PE optional header size %d" % opt_size)
    magic = struct.unpack_from("<H", data, opt_off)[0]
    if magic == 0x20B:  # PE32+
        rva_count_rel = 108
        dd_rel = 112
    elif magic == 0x10B:  # PE32
        rva_count_rel = 92
        dd_rel = 96
    else:
        raise ValueError("unsupported PE optional magic 0x%x" % magic)
    if opt_size < dd_rel + 8:
        raise ValueError("PE optional header lacks DataDirectory[0]")
    num_rva = struct.unpack_from("<I", data, opt_off + rva_count_rel)[0]
    if num_rva < 1:
        return []
    export_rva, export_size = struct.unpack_from("<II", data, opt_off + dd_rel)
    if export_rva == 0 or export_size == 0:
        return []

    sec_off = opt_off + opt_size
    if sec_off + num_sections * 40 > len(data):
        raise ValueError("truncated PE section table")
    sections = []
    for i in range(num_sections):
        vsize, va, raw_size, raw_ptr = struct.unpack_from(
            "<IIII", data, sec_off + i * 40 + 8
        )
        sections.append((vsize, va, raw_size, raw_ptr))

    def rva_to_offset(rva: int) -> int:
        for vsize, va, raw_size, raw_ptr in sections:
            span = max(vsize, raw_size)
            if va <= rva < va + span:
                delta = rva - va
                if delta >= raw_size or raw_ptr + delta >= len(data):
                    raise ValueError("RVA 0x%x outside section raw data" % rva)
                return raw_ptr + delta
        if rva < sec_off + num_sections * 40 and rva < len(data):
            return rva
        raise ValueError("RVA 0x%x not mapped to any PE section" % rva)

    exp_off = rva_to_offset(export_rva)
    if exp_off + 40 > len(data):
        raise ValueError("truncated IMAGE_EXPORT_DIRECTORY")
    (_chars, _ts, _maj, _min, _name_rva, _base, _num_funcs, num_names,
     _addr_funcs, addr_names, _addr_ords) = struct.unpack_from(
        "<IIHHIIIIIII", data, exp_off
    )
    if num_names == 0:
        return []
    names_off = rva_to_offset(addr_names)
    if names_off + num_names * 4 > len(data):
        raise ValueError("truncated export AddressOfNames table")
    exports = []
    for i in range(num_names):
        name_rva = struct.unpack_from("<I", data, names_off + i * 4)[0]
        name_off = rva_to_offset(name_rva)
        nul = data.find(b"\x00", name_off)
        if nul == -1:
            raise ValueError("unterminated export name at 0x%x" % name_off)
        exports.append(data[name_off:nul].decode("utf-8", errors="strict"))
    return exports


def exported_t_symbols(dylib: str, force_pe: bool = False) -> set:
    if force_pe or dylib.lower().endswith(".dll"):
        try:
            return set(read_pe_exports(dylib))
        except (OSError, ValueError, UnicodeDecodeError) as e:
            raise RuntimeError("PE export parse failed for %s: %s" % (dylib, e))
    try:
        rc, out, err = common.run_capture(["nm", "-gU", dylib])
    except OSError as e:
        raise RuntimeError("nm failed: %s" % e)
    if rc != 0:
        # Linux fallback: nm -g without -U (no underscore prefix).
        try:
            rc, out, err = common.run_capture(["nm", "-g", "--defined-only", dylib])
        except OSError as e:
            raise RuntimeError("nm failed: %s" % e)
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
    # Garbage dylib/dll fails (nm rejects non-Mach-O; PE parser rejects non-MZ).
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
        garbage_dll = os.path.join(tmp, "garbage.dll")
        with open(garbage_dll, "wb") as f:
            f.write(b"not a PE file, just tampered bytes\n" * 16)
        try:
            read_pe_exports(garbage_dll)
            bad += report("garbage dll raises ValueError", False)
        except ValueError:
            bad += report("garbage dll raises ValueError", True)
        try:
            exported_t_symbols(garbage_dll)
            bad += report("garbage dll raises RuntimeError", False)
        except RuntimeError:
            bad += report("garbage dll raises RuntimeError", True)
        # Minimal valid synthetic PE32+ DLL with .edata exports.
        names_blob = b""
        name_offsets = []
        for s in (b"chakra_nodeapi_create_env", b"napi_get_version"):
            name_offsets.append(len(names_blob))
            names_blob += s + b"\x00"
        sec_va = 0x1000
        sec_raw = 0x200
        exp_dir_rel = 0
        addr_names_rel = 40
        strings_rel = addr_names_rel + 8
        name_rvas = [sec_va + strings_rel + off for off in name_offsets]
        edata = bytearray(strings_rel + len(names_blob))
        struct.pack_into("<IIHHIIIIIII", edata, exp_dir_rel,
                         0, 0, 0, 0, 0, 1, 2, 2,
                         sec_va + addr_names_rel, sec_va + addr_names_rel,
                         sec_va + addr_names_rel)
        for idx, nrva in enumerate(name_rvas):
            struct.pack_into("<I", edata, addr_names_rel + idx * 4, nrva)
        edata[strings_rel:] = names_blob
        pe = bytearray(sec_raw + len(edata))
        pe[0:2] = b"MZ"
        e_lfanew = 0x40
        struct.pack_into("<I", pe, 0x3C, e_lfanew)
        pe[e_lfanew:e_lfanew + 4] = b"PE\x00\x00"
        opt_size = 240
        struct.pack_into("<HHIIIHH", pe, e_lfanew + 4,
                         0x8664, 1, 0, 0, 0, opt_size, 0x2022)
        opt_off = e_lfanew + 24
        struct.pack_into("<H", pe, opt_off, 0x20B)
        struct.pack_into("<I", pe, opt_off + 108, 16)
        struct.pack_into("<II", pe, opt_off + 112, sec_va, len(edata))
        sec_off = opt_off + opt_size
        pe[sec_off:sec_off + 8] = b".edata\x00\x00"
        struct.pack_into("<IIII", pe, sec_off + 8,
                         len(edata), sec_va, len(edata), sec_raw)
        pe[sec_raw:] = edata
        pe_dll = os.path.join(tmp, "synthetic.dll")
        with open(pe_dll, "wb") as f:
            f.write(bytes(pe))
        got_pe = exported_t_symbols(pe_dll)
        bad += report("synthetic PE32+ DLL parses exports",
                      got_pe == {"chakra_nodeapi_create_env", "napi_get_version"})
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
    bad += report("inventory parse sees 155",
                  len(gen.parse_inventory(Path(repo_root()) / "doc" / "node-api-inventory.md")) == 155)

    print("check_nodeapi self-test: %s"
          % ("ALL NEGATIVE CONTROLS BEHAVE" if bad == 0
             else "%d CONTROL(S) MISBEHAVE" % bad))
    return 1 if bad else 0


def main(argv) -> int:
    args = list(argv[1:])
    if "--self-test" in args:
        return self_test()
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--dylib", default=None)
    ap.add_argument("--dll", default=None)
    ap.add_argument("--exports", default=None)
    ap.add_argument("--inventory", default=None)
    ns = ap.parse_args(args)
    if bool(ns.dylib) == bool(ns.dll):
        ap.error("pass exactly one of --dylib or --dll")
    target = ns.dll or ns.dylib
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
        actual = exported_t_symbols(target, force_pe=bool(ns.dll))
    except RuntimeError as e:
        print("check_nodeapi: FAIL: %s" % e, file=sys.stderr)
        return 1
    provider, runtime = provider_symbols(actual)
    print("check_nodeapi: exports %s -> %d T symbols "
          "(%d provider + %d runtime operator new/delete), allowlist %d"
          % (target, len(actual), len(provider), len(runtime),
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
