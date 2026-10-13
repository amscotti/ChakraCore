#!/usr/bin/env python3
#-------------------------------------------------------------------------------------------------------
# Copyright (c) ChakraCore Project Contributors. All rights reserved.
# Licensed under the MIT license. See LICENSE.txt file in the project root for full license information.
#-------------------------------------------------------------------------------------------------------
"""B4.3: generate tools/build/tables_linux.zig from the Linux reference build.ninja.

The Linux engine graph (682 compiles across 34 target/language rules, incl.
Backend/WasmReader/wabt) is too large to hand-transcribe like the Mac tables.
Instead this script extracts every compile edge's source list plus its
(DEFINES, INCLUDES, FLAGS) triple from a reference ``build.ninja`` (produced by
the B4.1 legacy configure: RelWithDebInfo, JIT on, clang 18.1.3) and emits the
Zig tables. The emission is VERIFIED, not trusted:

* every rule must carry exactly ONE distinct triple (the reference is uniform
  per target; any second triple fails loudly);
* every target's DEFINES must equal the engine or PAL base set plus only the
  known per-target extras (``ch_EXPORTS``/``GCStress_EXPORTS``/
  ``ChakraCore_EXPORTS``/``_ENABLE_DYNAMIC_THUNKS``);
* every target's FLAGS must equal its class template (engine/PAL x C++/C/ASM,
  ch PIE, GCStress non-PIC) modulo the cosmetic color flags;
* every INCLUDES list must start with the global or PAL prefix; the remainder
  becomes the group's ``extra_includes`` (string-faithful, ``/src/``-stripped,
  including the two reference include quirks, which are asserted by name);
* link/archive edges are extracted for their object ORDER (static archive
  order drives the .so's joined-object order for init-order parity).

Regenerate (build.ninja lives in the VM-only B4.1 tree)::

    python3 tools/build/gen_tables_linux.py /path/to/build.ninja tools/build/tables_linux.zig

then ``git diff`` the output: any reference drift shows up as a table change.
"""

import re
import sys

SRC_PREFIX = "/src/"

ENGINE_BASE_DEFINES = (
    "-DBIT64=1 -DCAN_BUILD_WABT -DCLANG_HAS_DISABLE_TAIL_CALLS=1 "
    "-DENABLE_DEBUG_CONFIG_OPTIONS=1 -DFEATURE_PAL -DHAS_ICU -DHAS_REAL_ICU=1 "
    "-DINTL_ICU=1 -DLINUX64 -DNO_PAL_MINMAX -DPAL_STDCPP_COMPAT "
    "-DPLATFORM_UNIX=1 -DSTACK_ALIGN=16 -DTARGET_64 -DUNICODE -D_AMD64_ "
    "-D_CHAKRACOREBUILD -D_M_AMD64 -D_M_X64 -D_SAFECRT_USE_CPP_OVERLOADS=1 "
    "-D__LINUX__=1 -D__STDC_WANT_LIB_EXT1__=1"
).split()

PAL_BASE_DEFINES = (
    "-DBIT64=1 -DCAN_BUILD_WABT -DCLANG_HAS_DISABLE_TAIL_CALLS=1 "
    "-DENABLE_DEBUG_CONFIG_OPTIONS=1 -DFEATURE_PAL -DFEATURE_PAL=1 -DHAS_ICU "
    "-DHAS_REAL_ICU=1 -DINTL_ICU=1 -DLINUX64 -DLP64COMPATIBLE=1 -DPIC=1 "
    "-DPLATFORM_UNIX=1 -DSTACK_ALIGN=16 -DTARGET_64 -DUNICODE -D_AMD64_ "
    "-D_CHAKRACOREBUILD -D_FILE_OFFSET_BITS=64 -D_M_AMD64 -D_M_X64 "
    "-D_SAFECRT_USE_CPP_OVERLOADS=1 -D_WIN64=1 -D__LINUX__=1 "
    "-D__STDC_WANT_LIB_EXT1__=1"
).split()

KNOWN_EXTRA_DEFINES = {
    "-Dch_EXPORTS",
    "-DGCStress_EXPORTS",
    "-DChakraCore_EXPORTS",
    "-D_ENABLE_DYNAMIC_THUNKS=1",
}

GLOBAL_INCLUDES = [
    ".",
    "lib/Common",
    "lib/Common/PlaceHolder",
    "pal",
    "pal/inc",
    "pal/inc/rt",
]

PAL_INCLUDES = ["pal/src"] + GLOBAL_INCLUDES

# Reference include quirks, asserted by name (replicated string-faithfully):
# Q1: ChakraCore target carries a -fvisibility=hidden flag leaked into an -I.
# Q2: GCStress carries absolute -I/lib/... dirs (nonexistent, ignored by clang).
QUIRK_VISIBILITY = "bin/ChakraCore/-fvisibility=hidden"
QUIRK_ABS_1 = "/lib/Common"
QUIRK_ABS_2 = "/lib/Common/Memory"

# Cosmetic flags dropped from FLAGS (diagnostic color only, no codegen effect).
COSMETIC_FLAGS = {"-fdiagnostics-color=always", "-fcolor-diagnostics"}

# Class templates for FLAGS (post-cosmetic-strip), keyed by (lang, cls).
WARNINGS = (
    "-Werror -Wno-ignored-attributes -Wno-parentheses-equality -Wno-reorder "
    "-Wno-microsoft -Wno-unused-value -Wno-int-to-void-pointer-cast "
    "-Wno-invalid-offsetof -Wno-undefined-inline "
    "-Wno-inconsistent-missing-override -Wno-macro-redefined -Wno-pragmas "
    "-Wno-invalid-token-paste -Wno-format -Wno-invalid-noreturn "
    "-Wno-null-arithmetic -Wno-tautological-constant-out-of-range-compare "
    "-Wno-tautological-undefined-compare -Wno-address-of-temporary "
    "-Wno-null-conversion -Wno-return-type -Wno-switch "
    "-Wno-int-to-pointer-cast -Wno-tautological-constant-compare "
    "-Wno-enum-compare-switch -Wno-unknown-warning-option "
    "-fno-omit-frame-pointer -fdelayed-template-parsing -O3 -g -std=gnu++17"
).split()
CXX_TAIL_ENGINE = "-msse4.2 -fasm-blocks -fms-extensions -fwrapv -fPIC".split()
CXX_TAIL_PAL = (
    "-msse4.2 -fasm-blocks -fms-extensions -fwrapv "
    "-fstack-protector -fno-builtin -fPIC"
).split()


def class_templates():
    cxx_eng = WARNINGS + CXX_TAIL_ENGINE
    cxx_pal = WARNINGS + CXX_TAIL_PAL
    return {
        ("CXX", "engine"): cxx_eng,
        ("CXX", "pal"): cxx_pal,
        ("CXX", "pie"): WARNINGS + ["-fPIE"] + CXX_TAIL_ENGINE[:-1],
        ("CXX", "nopic"): WARNINGS + CXX_TAIL_ENGINE[:-1],
        ("CXX", "wabt"): cxx_eng,  # + -fcolor-diagnostics, stripped as cosmetic
        ("C", "pal"): (
            "-O2 -g -DNDEBUG -msse4.2 -fasm-blocks -fms-extensions -fwrapv "
            "-fstack-protector -fno-builtin -fPIC"
        ).split(),
        ("C", "engine"): (
            "-O2 -g -DNDEBUG -msse4.2 -fasm-blocks -fms-extensions -fwrapv -fPIC"
        ).split(),
        ("ASM", "pal"): (
            "-O2 -g -DNDEBUG -msse4.2 -fasm-blocks -fms-extensions -fwrapv "
            "-fstack-protector -fno-builtin -fPIC"
        ).split(),
        ("ASM", "engine"): (
            "-O2 -g -DNDEBUG -msse4.2 -fasm-blocks -fms-extensions -fwrapv -fPIC"
        ).split(),
    }


# (target, lang) -> (cls, group_name, singular_name or None)
TARGETS = {
    ("Chakra.Pal", "CXX"): ("pal", "pal_group", "Chakra.Pal.Singular"),
    ("Chakra.Pal", "C"): ("pal", "pal_group", "Chakra.Pal.Singular"),
    ("Chakra.Pal", "ASM"): ("pal", "pal_group", "Chakra.Pal.Singular"),
    ("Chakra.Common.Codex", "CXX"): ("engine", "codex_group", "Chakra.Common.Codex.Singular"),
    ("Chakra.Common.Common", "CXX"): ("engine", "common_group", None),
    ("Chakra.Common.Core", "CXX"): ("engine", "core_group", None),
    ("Chakra.Common.DataStructures", "CXX"): ("engine", "datastructures_group", None),
    ("Chakra.Common.Exceptions", "CXX"): ("engine", "exceptions_group", None),
    ("Chakra.Common.Memory", "CXX"): ("engine", "memory_group", None),
    ("Chakra.Common.Memory", "ASM"): ("engine", "memory_group", None),
    ("Chakra.Common.Util", "CXX"): ("engine", "util_group", None),
    ("Chakra.Backend", "CXX"): ("engine", "backend_group", None),
    ("Chakra.Backend", "ASM"): ("engine", "backend_group", None),
    ("Chakra.Parser", "CXX"): ("engine", "parser_group", None),
    ("Chakra.SCACore", "CXX"): ("engine", "scacore_group", None),
    ("Chakra.WasmReader", "CXX"): ("engine", "wasmreader_group", None),
    ("Chakra.Runtime.Base", "CXX"): ("engine", "rt_base_group", None),
    ("Chakra.Runtime.ByteCode", "CXX"): ("engine", "rt_bytecode_group", None),
    ("Chakra.Runtime.Debug", "CXX"): ("engine", "rt_debug_group", None),
    ("Chakra.Runtime.Language", "CXX"): ("engine", "rt_language_group", None),
    ("Chakra.Runtime.Language", "ASM"): ("engine", "rt_language_group", None),
    ("Chakra.Runtime.Library", "CXX"): ("engine", "rt_library_group", None),
    ("Chakra.Runtime.Library", "ASM"): ("engine", "rt_library_group", None),
    ("Chakra.Runtime.Math", "CXX"): ("engine", "rt_math_group", None),
    ("Chakra.Runtime.PlatformAgnostic", "CXX"): (
        "engine",
        "platformagnostic_group",
        "Chakra.Runtime.PlatformAgnostic.Singular",
    ),
    ("Chakra.Runtime.Types", "CXX"): ("engine", "rt_types_group", None),
    ("Chakra.Jsrt", "CXX"): ("engine", "jsrt_group", None),
    ("Chakra.Jsrt.Core", "CXX"): ("engine", "jsrt_core_group", None),
    ("libwabt", "CXX"): ("wabt", "wabt_group", None),
    ("libwabt", "C"): ("engine", "wabt_group", None),
    ("ChakraCoreStatic", "CXX"): ("engine", "static_src_group", None),
    ("ChakraCore", "CXX"): ("engine", "dylib_group", None),
    ("ch", "CXX"): ("pie", "ch_group", None),
    ("GCStress", "CXX"): ("nopic", "gcstress_group", None),
}


def parse_ninja(path):
    edges = []
    with open(path) as f:
        for line in f:
            if line.startswith("build "):
                m = re.match(r"build (.*?): (\S+?)(?: (.*))?$", line.rstrip("\n"))
                assert m, line
                edges.append([m.group(1).split(), m.group(2), m.group(3) or "", {}])
            elif (line.startswith("  ") or line.startswith("\t")) and edges and "=" in line:
                k, v = line.strip().split("=", 1)
                edges[-1][3][k.strip()] = v.strip()
    return edges


def decode_target(rule):
    # CXX_COMPILER__Chakra.2eBackend_unscanned_RelWithDebInfo -> (Chakra.Backend, CXX)
    m = re.match(r"(CXX|C|ASM)_COMPILER__(.*)_unscanned_RelWithDebInfo$", rule)
    assert m, rule
    return m.group(2).replace(".2e", "."), m.group(1)


def strip_src(p):
    assert p.startswith(SRC_PREFIX), p
    return p[len(SRC_PREFIX):]


def zig_list(items, indent="        "):
    return "".join(f'{indent}"{x}",\n' for x in items)


def main():
    ninja_path, out_path = sys.argv[1], sys.argv[2]
    edges = parse_ninja(ninja_path)
    templates = class_templates()

    triples = {}  # (target, lang) -> set of (defines, includes, flags)
    sources = {}  # (target, lang) -> [src...] in ninja order
    for outs, rule, ins, variables in edges:
        if "_COMPILER__" not in rule:
            continue
        target, lang = decode_target(rule)
        key = (target, lang)
        assert key in TARGETS, f"unexpected rule target: {key}"
        triples.setdefault(key, set()).add(
            (variables["DEFINES"], variables["INCLUDES"], variables["FLAGS"])
        )
        src = ins.split()[0]
        assert src.startswith(SRC_PREFIX), src
        sources.setdefault(key, []).append(strip_src(src))

    assert set(triples) == set(TARGETS), (
        f"target mismatch: missing={set(TARGETS) - set(triples)} "
        f"extra={set(triples) - set(TARGETS)}"
    )
    for key, seen in triples.items():
        assert len(seen) == 1, f"{key}: {len(seen)} distinct flag triples (want 1)"

    # Verify defines/includes/flags per target; collect group data.
    groups = {}  # group_name -> dict(cxx=[], c=[], asm=[], extras, target_define, pal, pic, singular)
    for key in sorted(triples):
        target, lang = key
        cls, group_name, singular = TARGETS[key]
        defines, includes, flags = next(iter(triples[key]))
        base = PAL_BASE_DEFINES if cls == "pal" else ENGINE_BASE_DEFINES
        toks = defines.split()
        assert [t for t in base if t not in toks] == [], f"{key}: base define missing"
        extra = [t for t in toks if t not in base]
        assert len(extra) <= 1, f"{key}: >1 extra define: {extra}"
        assert all(e in KNOWN_EXTRA_DEFINES for e in extra), f"{key}: unknown extra {extra}"
        # NOTE: mid-list extras (ChakraCore_EXPORTS, GCStress_EXPORTS) are
        # emitted last via target_define; position is semantically nil
        # (same documented deviation as tables.zig's string-fidelity caveat).

        inc_toks = includes.split()
        # Split -I dirs (ASM carries a trailing "-isystem /usr/local/include").
        isystem = None
        if "-isystem" in inc_toks:
            i = inc_toks.index("-isystem")
            assert lang == "ASM" and i == len(inc_toks) - 2, f"{key}: unexpected -isystem"
            isystem = inc_toks[i + 1]
            assert isystem == "/usr/local/include", key
            inc_toks = inc_toks[:i]
        assert all(t.startswith("-I") for t in inc_toks), (key, inc_toks)
        dirs = [t[2:] for t in inc_toks]
        if cls == "pal":
            assert dirs[0] == "/src/build-xplat/pal/src", f"{key}: no generated-config first -I"
            rest = [d if d.startswith("/") else d for d in dirs[1:]]
            rest = [strip_src(d) for d in rest]
            assert rest[: len(PAL_INCLUDES)] == PAL_INCLUDES, f"{key}: pal prefix: {rest}"
            extras = rest[len(PAL_INCLUDES):]
            assert extras == ["pal/src/include"], f"{key}: pal extras: {extras}"
        else:
            rest = [strip_src(d) if d.startswith(SRC_PREFIX) else d for d in dirs]
            assert rest[: len(GLOBAL_INCLUDES)] == GLOBAL_INCLUDES, f"{key}: global prefix: {rest}"
            extras = rest[len(GLOBAL_INCLUDES):]

        flag_toks = [t for t in flags.split() if t not in COSMETIC_FLAGS]
        template = templates[(lang, cls)]
        if key == ("ChakraCore", "CXX"):
            # Q3: mid-line -fPIC (after -std=gnu++17); order-independent,
            # emitted late uniformly (documented in doc/build-contract-linux.md).
            mid = WARNINGS + ["-fPIC"] + CXX_TAIL_ENGINE[:-1]
            assert flag_toks == mid, f"{key}: FLAGS mismatch vs Q3 shape"
        else:
            assert flag_toks == template, (
            f"{key}: FLAGS mismatch:\n  ref: {flag_toks}\n  tpl: {template}"
        )

        g = groups.setdefault(
            group_name,
            {
                "pal": cls == "pal",
                "pic": {"pie": ".pie", "nopic": ".none"}.get(cls, ".pic"),
                "singular": singular,
                "extras": extras,
                "target_define": extra[0] if extra else None,
                "cxx": [],
                "c": [],
                "asm": [],
                "isystem": isystem,
            },
        )
        assert g["extras"] == extras, f"{group_name}: extras differ across langs"
        assert (g["target_define"] == (extra[0] if extra else None)), f"{group_name}: define differs"
        if lang == "CXX":
            g["cxx"] = sources[key]
        elif lang == "C":
            g["c"] = sources[key]
        else:
            g["asm"] = sources[key]
            assert isystem == "/usr/local/include", key

    # Assert the two include quirks are present (and only where expected).
    assert QUIRK_VISIBILITY in groups["dylib_group"]["extras"], "Q1 visibility quirk missing"
    assert QUIRK_ABS_1 in groups["gcstress_group"]["extras"], "Q2 abs quirk missing"
    assert QUIRK_ABS_2 in groups["gcstress_group"]["extras"], "Q2 abs quirk missing"

    n_compile = sum(len(v) for v in sources.values())
    print(f"targets: {len(triples)}, compile edges: {n_compile}")
    for key in sorted(sources):
        print(f"  {key[0]}/{key[1]}: {len(sources[key])}")

    # Object order for archives (member order of each `ar` edge; the .so
    # joins the static members in the same order for initializer parity).
    out2src = {}
    for outs, rule, ins, variables in edges:
        if "_COMPILER__" in rule:
            out2src[outs[0]] = strip_src(ins.split()[0])
    archive_edges = {
        "static": "lib/libChakraCoreStatic.a",
        "pal": "pal/src/libChakra.Pal.Singular.a",
        "codex": "lib/Common/Codex/libChakra.Common.Codex.Singular.a",
        "platformagnostic": (
            "lib/Runtime/PlatformAgnostic/Platform/"
            "libChakra.Runtime.PlatformAgnostic.Singular.a"
        ),
    }
    orders = {}
    for key, out in archive_edges.items():
        members = None
        for outs, rule, ins, variables in edges:
            if outs == [out]:
                members = [t for t in ins.split() if t.endswith(".o")]
        assert members, f"archive edge {out} not found"
        orders[key] = [out2src[o] for o in members]
        print(f"archive {key}: {len(members)} members")
    # The static archive holds every compile except ch/GCStress/dylib TUs.
    non_archived = set(groups["ch_group"]["cxx"]) | set(groups["gcstress_group"]["cxx"]) | set(
        groups["dylib_group"]["cxx"]
    )
    assert len(non_archived) == 15, len(non_archived)
    assert set(orders["static"]) == (
        set().union(*(set(s) for s in sources.values())) - non_archived
    ), "static membership mismatch"
    assert set(orders["pal"]) == (
        set(groups["pal_group"]["cxx"])
        | set(groups["pal_group"]["c"])
        | set(groups["pal_group"]["asm"])
    ), "pal singular membership mismatch"
    assert orders["codex"] == groups["codex_group"]["cxx"], "codex order mismatch"
    assert (
        orders["platformagnostic"] == groups["platformagnostic_group"]["cxx"]
    ), "platformagnostic order mismatch"

    with open(out_path, "w") as f:
        f.write(
            """//-------------------------------------------------------------------------------------------------------
// Copyright (c) ChakraCore Project Contributors. All rights reserved.
// Licensed under the MIT license. See LICENSE.txt file in the project root for full license information.
//-------------------------------------------------------------------------------------------------------
//! B4.3: contract tables for the Linux x86_64 compile graph (test profile).
//!
//! GENERATED by tools/build/gen_tables_linux.py from the B4.1 reference
//! build.ninja - do not hand-edit. Regenerate after any reference-graph
//! change and `git diff` the result: drift shows up as a table change.
//!
//! Group shape reuses tables.Group; only Linux membership/flags live here.
//! Reference quirks replicated string-faithfully: Q1 (ChakraCore target's
//! `-fvisibility=hidden` leaked into an -I dir) and Q2 (GCStress absolute
//! `-I/lib/...` dirs) - both nonexistent, ignored by clang, asserted by
//! name in the generator. ChakraCore's mid-line -fPIC position is NOT
//! replicated (order-independent flag; documented in doc/build-contract-linux.md).

const tables = @import("tables.zig");

pub const Group = tables.Group;
pub const Pic = tables.Pic;

// -D flags for every non-PAL unit (generator-verified against the reference;
// unvalued/valued pairs keep reference order).
pub const engine_defines: []const []const u8 = &.{
"""
        )
        for d in ENGINE_BASE_DEFINES:
            f.write(f'    "{d}",\n')
        f.write("};\n\n// -D flags for every PAL unit.\n")
        f.write("pub const pal_defines: []const []const u8 = &.{\n")
        for d in PAL_BASE_DEFINES:
            f.write(f'    "{d}",\n')
        f.write("};\n\n// 30 entries in observed order: 25 -Wno-* suppressions "
                "(root CMakeLists.txt:410-436) plus 5 codegen flags.\n"
                "// (The 5 are also re-emitted by linux.zig appendTuFlags: "
                "duplicate emission, deliberately deferred; see "
                "doc/build-contract-linux.md section 8.)\n")
        f.write("pub const warnings: []const []const u8 = &.{\n")
        for w in WARNINGS:
            if w != "-Werror":
                f.write(f'    "{w}",\n')
        f.write("};\n\n")
        order = [
            "pal_group", "codex_group", "common_group", "core_group",
            "datastructures_group", "exceptions_group", "memory_group",
            "util_group", "backend_group", "parser_group", "scacore_group",
            "wasmreader_group", "rt_base_group", "rt_bytecode_group",
            "rt_debug_group", "rt_language_group", "rt_library_group",
            "rt_math_group", "platformagnostic_group", "rt_types_group",
            "jsrt_group", "jsrt_core_group", "wabt_group", "static_src_group",
            "dylib_group", "ch_group", "gcstress_group",
        ]
        assert set(order) == set(groups), set(groups) ^ set(order)
        for name in order:
            g = groups[name]
            f.write(f"pub const {name} = Group{{\n")
            if g["pal"]:
                f.write("    .pal = true,\n")
            if g["singular"]:
                f.write(f'    .singular_name = "{g["singular"]}",\n')
            if g["pic"] != ".pic":
                f.write(f"    .pic = {g['pic']},\n")
            if g["target_define"]:
                f.write(f'    .target_define = "{g["target_define"]}",\n')
            if g["cxx"]:
                f.write("    .cxx = &.{\n")
                f.write(zig_list(g["cxx"]))
                f.write("    },\n")
            if g["c"]:
                f.write("    .c = &.{\n")
                f.write(zig_list(g["c"]))
                f.write("    },\n")
            if g["asm"]:
                f.write("    .asm_files = &.{\n")
                f.write(zig_list(g["asm"]))
                f.write("    },\n")
            if g["extras"]:
                f.write("    .extra_includes = &.{\n")
                f.write(zig_list(g["extras"]))
                f.write("    },\n")
            f.write("};\n\n")
        # Archive member order lists (source paths in reference `ar` order).
        # (No object_groups table: linux.zig drives membership from its
        # own all_groups + static_order instead; the emitted table was dead.)
        f.write(
            """// Archive member order, in reference `ar` edge order (source paths).
// static_order also drives the .so's joined-object order.
pub const static_order: []const []const u8 = &.{
"""
        )
        f.write(zig_list(orders["static"], indent="    "))
        f.write("};\n\npub const singular_orders = .{\n")
        for key in ("pal", "codex", "platformagnostic"):
            f.write(f"    .{key} = [_][]const u8{{\n")
            f.write(zig_list(orders[key], indent="        "))
            f.write("    },\n")
        f.write("};\n")
    print(f"wrote {out_path}")


if __name__ == "__main__":
    main()
