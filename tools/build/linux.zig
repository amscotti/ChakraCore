//-------------------------------------------------------------------------------------------------------
// Copyright (c) ChakraCore Project Contributors. All rights reserved.
// Licensed under the MIT license. See LICENSE.txt file in the project root for full license information.
//-------------------------------------------------------------------------------------------------------
//! B4.3: Linux x86_64 build graph (test profile only).
//!
//! Native x86_64 Linux, shared/JIT-on, RelWithDebInfo Test, system ICU,
//! glibc 2.39 floor. 684 compiles (Backend/WasmReader/wabt included),
//! static + 3 singular archives, libChakraCore.so, dynamic ch, GCStress,
//! staged headers, plus the `check` aggregation (regression, bytecode,
//! native embedding, smoke, metadata) and `check-selftest` controls.
//!
//! Structure mirrors build.zig (Mac): tables-driven compiles with native
//! depfiles, generator/input edges (jstoc, PAL config, wabt config),
//! native installs for the test profile, check steps off the default
//! build. Linux differences, each justified once here:
//!
//! * Links run through `zig c++` CLI Run steps (not Module linking):
//!   Zig 0.17's Module API cannot express the reference's mixed C++
//!   standard-library linkage (shared libstdc++ on exes, static on the
//!   .so) or its --whole-archive/--start-group shape. The CLI driver
//!   reproduces the reference link lines (see the link site for the
//!   per-flag accounting, incl. the two LLD-parity spellings).
//! * C++ standard library: `zig c++` 0.17 uses its bundled libc++
//!   headers and links libc++ statically (-stdlib=libstdc++ is ignored;
//!   probed). The engine/ICU boundary is C-only (ChakraICU.h includes
//!   ucal/ucol/udat/uloc/ustring/unorm2 et al, no C++ API), so no C++
//!   ABI crosses into the libstdc++-built ICU. Behavior parity is proven
//!   by the checks, not assumed.
//! * Objects join the .so in reference archive order (static_order)
//!   instead of --whole-archive (same initializer order, no archive
//!   semantics needed); --start/end-group are dropped there for the
//!   same reason, kept on GCStress where an archive is linked.
//! * Object files are NNNN-<base>.o for uniqueness only; ELF init order
//!   comes from the explicit join/archive order lists, not names.
//!
//! Contracts: doc/build-contract-linux.md (sources/flags/includes/links),
//! doc/pal-config-contract-linux.md (config.h checks), doc/b4-linux.md
//! (environment, qemu, baselines). Only -Dprofile=test exists on Linux.

const std = @import("std");
const builtin = @import("builtin");
const pal_config = @import("../pal-config-linux/check.zig");
const tables = @import("tables_linux.zig");
const mac_tables = @import("tables.zig");

const profile_target = "x86_64-linux-gnu";
const profile_glibc = "2.39";
const profile_icu_short = "74";
const default_icu_include = "/usr/include";
const default_icu_libdir = "/usr/lib/x86_64-linux-gnu";

fn fail(comptime fmt: []const u8, args: anytype) noreturn {
    std.debug.print("build error: " ++ fmt ++ "\n", args);
    std.process.exit(1);
}

fn gateProfile(b: *std.Build) void {
    const raw = b.option([]const u8, "profile", "Build profile: test (only profile on Linux)") orelse "test";
    if (!std.mem.eql(u8, raw, "test"))
        fail("unsupported -Dprofile={s} on Linux: B4.3 implements only test (RelWithDebInfo shared/JIT)", .{raw});
}

fn gateOptimize(b: *std.Build) void {
    // The C/C++ -O flags are explicit on our command lines; pin the mode
    // so a mode name alone can never select another configuration.
    const choice = b.option(std.builtin.Optimize, "optimize", "Optimization (pinned: fast)") orelse .fast;
    if (choice != .fast)
        fail("unsupported -Doptimize={t} on Linux: this profile (RelWithDebInfo, -O3 -g) supports only fast", .{choice});
}

fn gateTarget(b: *std.Build) void {
    const rt = b.standardTargetOptions(.{ .default_target = .{
        .cpu_arch = .x86_64,
        .os_tag = .linux,
        .abi = .gnu,
    } });
    if (b.user_input_options.get("cpu")) |provided| {
        const val: []const u8 = switch (provided) {
            .scalar => |s| s,
            .flag => "(flag)",
            else => "(non-scalar)",
        };
        fail("unsupported -Dcpu={s} on Linux: the reference passes no -march (baseline + -msse4.2); -Dcpu overrides are rejected", .{val});
    }
    const t = rt.result;
    if (t.cpu.arch != .x86_64)
        fail("unsupported arch {t}: this profile builds only x86_64-linux-gnu", .{t.cpu.arch});
    if (t.os.tag != .linux)
        fail("unsupported OS {t}: this profile builds only x86_64-linux-gnu", .{t.os.tag});
    if (t.abi != .gnu)
        fail("unsupported ABI {t}: this profile builds only x86_64-linux-gnu", .{t.abi});
    if (t.ofmt != .elf)
        fail("unsupported object format {t}: this profile builds only ELF", .{t.ofmt});
}

fn glibcFirstLineMatches(first: []const u8) bool {
    // First line looks like "ldd (Ubuntu GLIBC 2.39-0ubuntu8.9) 2.39".
    // Exact-match the trailing bare version token (the Mac gateSdk
    // exact-compare pattern in build.zig): a substring search for
    // "2.39" would also pass on e.g. a "12.39" vendor string or a
    // "2.390" token while the actual glibc version differs.
    const trimmed = std.mem.trimEnd(u8, first, " \t\r");
    const sp = std.mem.lastIndexOfScalar(u8, trimmed, ' ') orelse return false;
    return std.mem.eql(u8, trimmed[sp + 1 ..], profile_glibc);
}

test "glibc gate exact-matches the trailing version token" {
    try std.testing.expect(glibcFirstLineMatches("ldd (Ubuntu GLIBC 2.39-0ubuntu8.9) 2.39"));
    try std.testing.expect(!glibcFirstLineMatches("ldd (Ubuntu GLIBC 2.39-0ubuntu8.9) 2.40"));
    try std.testing.expect(!glibcFirstLineMatches("ldd (vendor 12.39) 2.38"));
    try std.testing.expect(!glibcFirstLineMatches("ldd (GNU libc) 2.390"));
    try std.testing.expect(!glibcFirstLineMatches(""));
}

fn gateGlibc(b: *std.Build) void {
    const res = b.runFallible(&.{ "ldd", "--version" }, .{});
    const out: []const u8 = switch (res) {
        .success => |o| o,
        else => fail("cannot run ldd --version: glibc floor unverifiable (need {s})", .{profile_glibc}),
    };
    var it = std.mem.splitScalar(u8, out, '\n');
    const first = it.next() orelse fail("cannot parse ldd --version: glibc floor unverifiable (need {s})", .{profile_glibc});
    if (!glibcFirstLineMatches(first))
        fail("unsupported glibc ({s}): this profile requires the Ubuntu 24.04 floor glibc {s}", .{ first, profile_glibc });
}

fn gateIcu(b: *std.Build) struct { include: []const u8, libdir: []const u8 } {
    const arena = b.graph.arena;
    const io = b.graph.io;
    const icu = b.option([]const u8, "icu-include", "ICU include dir (default: " ++ default_icu_include ++ ")") orelse default_icu_include;
    const libdir = b.option([]const u8, "icu-libdir", "ICU lib dir (default: " ++ default_icu_libdir ++ ")") orelse default_icu_libdir;
    const uchar = std.fmt.allocPrint(arena, "{s}/unicode/uchar.h", .{icu}) catch @panic("OOM");
    std.Io.Dir.accessAbsolute(io, uchar, .{}) catch
        fail("Cannot find ICU (missing {s}). Try installing libicu-dev or the appropriate packages for your platform.", .{uchar});
    for ([_][]const u8{ "libicuuc.so", "libicui18n.so" }) |lib| {
        const p = std.fmt.allocPrint(arena, "{s}/{s}", .{ libdir, lib }) catch @panic("OOM");
        std.Io.Dir.accessAbsolute(io, p, .{}) catch
            fail("Cannot find ICU libraries (missing {s}). Try installing libicu-dev or the appropriate packages for your platform.", .{p});
    }
    // ICU major gate (contract floor: libicu-dev 74.x).
    const uvernum = std.fmt.allocPrint(arena, "{s}/unicode/uvernum.h", .{icu}) catch @panic("OOM");
    const vf = std.Io.Dir.openFileAbsolute(io, uvernum, .{}) catch
        fail("cannot read {s}: ICU version unverifiable (need major {s})", .{ uvernum, profile_icu_short });
    defer vf.close(io);
    var vfr = vf.readerStreaming(io, &.{});
    const bytes = vfr.interface.allocRemaining(arena, .limited(1 << 20)) catch
        fail("cannot read {s}: ICU version unverifiable (need major {s})", .{ uvernum, profile_icu_short });
    const needle = std.fmt.allocPrint(arena, "U_ICU_VERSION_SHORT \"{s}\"", .{profile_icu_short}) catch @panic("OOM");
    if (std.mem.indexOf(u8, bytes, needle) == null)
        fail("{s}: ICU major unverifiable (need U_ICU_VERSION_SHORT \"{s}\")", .{ uvernum, profile_icu_short });
    return .{ .include = icu, .libdir = libdir };
}

const Ctx = struct {
    b: *std.Build,
    zig_exe: []const u8,
    icu_include: []const u8,
    icu_libdir: []const u8,
    config_dir: std.Build.LazyPath,
    config_h: std.Build.LazyPath,
    wabt_dir: std.Build.LazyPath,
    wabt_h: std.Build.LazyPath,
    gen_dir: std.Build.LazyPath,
    dbg_header: std.Build.LazyPath,
};

const Lang = enum { cxx, c, asm_ };

// Generated-dir markers carried in tables_linux extra_includes (stripped
// /src/build-xplat/... spellings); the build substitutes its own dirs.
const gen_markers = [_]struct { marker: []const u8, which: enum { gen, wabt } }{
    .{ .marker = "build-xplat/bin/ch", .which = .gen },
    .{ .marker = "build-xplat/lib/wabt", .which = .wabt },
};

fn appendTuFlags(ctx: *const Ctx, run: *std.Build.Step.Run, g: *const tables.Group, lang: Lang) void {
    const b = ctx.b;
    const defines = if (g.pal) tables.pal_defines else tables.engine_defines;
    for (defines) |d| run.addArg(d);
    if (g.target_define) |d| run.addArg(d);

    if (g.pal) run.addPrefixedDirectoryArg("-I", ctx.config_dir);
    const rels = if (g.pal) mac_tables.pal_includes_rel else mac_tables.global_includes_rel;
    for (rels) |dir| run.addArg(b.fmt("-I{s}", .{dir}));
    for (g.extra_includes) |dir| {
        var substituted = false;
        for (gen_markers) |m| {
            if (std.mem.eql(u8, dir, m.marker)) {
                run.addPrefixedDirectoryArg("-I", if (m.which == .gen) ctx.gen_dir else ctx.wabt_dir);
                substituted = true;
            }
        }
        if (!substituted) run.addArg(b.fmt("-I{s}", .{dir}));
    }
    if (g.pal) run.addArg("-Ipal/src/include");
    // The reference passes no ICU -I (system clang searches /usr/include
    // by default); `zig cc` searches only its bundled headers, so the
    // default dir is spelled explicitly. -isystem, not -I: /usr/include
    // carries the full C library, and -I would shadow the bundled
    // headers (probed with a poisoned stdio.h); user -isystem does not
    // shadow them. Honors -Dicu-include.
    run.addArg("-isystem");
    run.addArg(ctx.icu_include);

    switch (lang) {
        .cxx => {
            run.addArg("-Werror");
            for (tables.warnings) |w| run.addArg(w);
            for ([_][]const u8{ "-fno-omit-frame-pointer", "-fdelayed-template-parsing", "-O3", "-g", "-std=gnu++17" }) |f| run.addArg(f);
            if (g.pic == .pie) run.addArg("-fPIE");
            for ([_][]const u8{ "-msse4.2", "-fasm-blocks", "-fms-extensions", "-fwrapv" }) |f| run.addArg(f);
            if (g.pal) for ([_][]const u8{ "-fstack-protector", "-fno-builtin" }) |f| run.addArg(f);
            if (g.pic == .pic) run.addArg("-fPIC");
        },
        .c, .asm_ => {
            // Reference C/ASM carry no -std (default gnu17) and -O2 -g
            // -DNDEBUG (CMAKE_C_FLAGS_RELWITHDEBINFO builtins).
            for ([_][]const u8{ "-O2", "-g", "-DNDEBUG", "-msse4.2", "-fasm-blocks", "-fms-extensions", "-fwrapv" }) |f| run.addArg(f);
            if (g.pal) for ([_][]const u8{ "-fstack-protector", "-fno-builtin" }) |f| run.addArg(f);
            run.addArg("-fPIC");
            // Only ASM carries -isystem /usr/local/include (observed).
            if (lang == .asm_) {
                run.addArg("-isystem");
                run.addArg("/usr/local/include");
            }
        },
    }
}

fn compileTu(ctx: *const Ctx, g: *const tables.Group, src: []const u8, lang: Lang, number: u32) std.Build.LazyPath {
    const b = ctx.b;
    std.debug.assert(number < 10000);
    const run = b.addSystemCommand(&.{ ctx.zig_exe, if (lang == .cxx) "c++" else "cc" });
    run.addArg("-target");
    run.addArg(profile_target);
    appendTuFlags(ctx, run, g, lang);
    // Toolchain-drift parity (documented deviation, B4.3): clang 22
    // emits -Wdate-time for this TU's __DATE__/__TIME__ use (reference
    // clang 18 stays silent with identical flags), which -Werror
    // promotes. The reference has no such suppression because its
    // compiler never fires the warning. Scoped to the sole using TU
    // (verified by repo grep: the only other user,
    // bin/ChakraCore/ChakraCoreDllFunc.cpp, is not in the Linux
    // compile set); -Werror itself is untouched.
    if (lang == .cxx and std.mem.eql(u8, src, "lib/Jsrt/JsrtHelper.cpp"))
        run.addArg("-Wno-date-time");
    var digits: [4]u8 = .{ '0', '0', '0', '0' };
    var n = number;
    var i: usize = 4;
    while (i > 0) {
        i -= 1;
        digits[i] = '0' + @as(u8, @intCast(n % 10));
        n /= 10;
    }
    const base = std.fs.path.basename(src);
    run.addArg("-MD");
    run.addArg("-MF");
    _ = run.addDepFileOutputArg(b.fmt("{s}-{s}.d", .{ digits, base }));
    run.addArg("-o");
    const out = run.addOutputFileArg(b.fmt("{s}-{s}.o", .{ digits, base }));
    run.addArg("-c");
    run.addFileArg(b.path(src));
    if (g == &tables.ch_group) run.addFileInput(ctx.dbg_header);
    if (g.pal) run.addFileInput(ctx.config_h);
    if (g == &tables.wabt_group) run.addFileInput(ctx.wabt_h);
    return out;
}

// wabt config.h: fixed-by-profile substitution of lib/wabt/src/config.h.in
// (configure_file, lib/wabt/CMakeLists.txt:41-44). Values observed in the
// reference generated header (sha256 4d365a89ec93f99a52137a5db9944651
// 4892cf928b2ff4a824ca997a271749d9); any template drift fails loudly
// below, and the metadata check re-verifies the output hash.
fn renderWabtConfig(arena: std.mem.Allocator, template: []const u8) []const u8 {
    const values = [_]struct { name: []const u8, val: []const u8 }{
        .{ .name = "HAVE_ALLOCA_H", .val = "1" },
        .{ .name = "HAVE_UNISTD_H", .val = "1" },
        .{ .name = "HAVE_SNPRINTF", .val = "1" },
        .{ .name = "HAVE_SYSCONF", .val = "1" },
        .{ .name = "HAVE_SSIZE_T", .val = "1" },
        .{ .name = "HAVE_STRCASECMP", .val = "1" },
        .{ .name = "HAVE_WIN32_VT100", .val = "0" },
        .{ .name = "COMPILER_IS_CLANG", .val = "1" },
        .{ .name = "COMPILER_IS_GNU", .val = "0" },
        .{ .name = "COMPILER_IS_MSVC", .val = "0" },
        .{ .name = "WITH_EXCEPTIONS", .val = "0" },
    };
    var out: std.ArrayList(u8) = .empty;
    var it = std.mem.splitScalar(u8, template, '\n');
    var n_sub: usize = 0;
    var first = true;
    while (it.next()) |line| {
        // Join, don't terminate: splitScalar yields a final empty piece
        // when the template ends with '\n', and terminating every piece
        // would emit a spurious blank line (cmake configure_file
        // preserves the template's exact ending — verified byte-equal).
        if (!first) out.append(arena, '\n') catch @panic("OOM");
        first = false;
        if (std.mem.startsWith(u8, line, "#cmakedefine01 ")) {
            const name = line["#cmakedefine01 ".len..];
            var found = false;
            for (values) |v| {
                if (std.mem.eql(u8, name, v.name)) {
                    out.print(arena, "#define {s} {s}", .{ name, v.val }) catch @panic("OOM");
                    found = true;
                    n_sub += 1;
                }
            }
            if (!found) fail("wabt config template drift: unknown #cmakedefine01 {s}", .{name});
        } else if (std.mem.indexOf(u8, line, "@SIZEOF_SIZE_T@") != null) {
            const rep = std.mem.replaceOwned(u8, arena, line, "@SIZEOF_SIZE_T@", "8") catch @panic("OOM");
            out.appendSlice(arena, rep) catch @panic("OOM");
            n_sub += 1;
        } else {
            if (std.mem.indexOfScalar(u8, line, '@') != null)
                fail("wabt config template drift: unhandled @VAR@ in: {s}", .{line});
            out.appendSlice(arena, line) catch @panic("OOM");
        }
    }
    if (n_sub != values.len + 1)
        fail("wabt config template drift: {d} substitutions (want {d})", .{ n_sub, values.len + 1 });
    return out.toOwnedSlice(arena) catch @panic("OOM");
}

pub fn build(b: *std.Build) void {
    gateProfile(b);
    gateOptimize(b);
    gateTarget(b);
    gateGlibc(b);
    const icu = gateIcu(b);
    const python = b.findProgram(.{ .names = &.{ "python3", "python" } }) orelse
        fail("cannot find python3 or python on PATH; needed to run bin/ch/jstoc.py for DbgController.js.h", .{});

    std.debug.print("b4 profile=test: target={s} glibc={s} icu={s},{s} zig={f} python={s}\n", .{
        profile_target, profile_glibc, icu.include, icu.libdir, builtin.zig_version, python,
    });

    // Build-owned pal/src/config.h: 95 real checks incl. native run
    // probes, fail-clearly on deviation (first online execution: B4.2
    // verified the contract offline only).
    const cfg = pal_config.generate(.{
        .io = b.graph.io,
        .zig_exe = b.graph.zig_exe,
        .target = profile_target,
        .profile = "test",
    });
    defer std.heap.page_allocator.free(cfg.config_h);
    if (!cfg.tail_calls) fail("CLANG_HAS_DISABLE_TAIL_CALLS check failed: this profile requires a compiler with __attribute__((disable_tail_calls))", .{});

    const arena = b.graph.arena;
    const wf = b.addWriteFiles();
    const config_h = wf.add("config.h", arena.dupe(u8, cfg.config_h) catch @panic("OOM"));

    // Build-owned wabt config.h (fixed-by-profile substitution).
    const root = b.root.root_dir.path orelse
        fail("cannot resolve the build root path; refusing a CWD-relative template read", .{});
    const wabt_in_path = std.fmt.allocPrint(arena, "{s}/lib/wabt/src/config.h.in", .{root}) catch @panic("OOM");
    const wabt_in_file = std.Io.Dir.openFileAbsolute(b.graph.io, wabt_in_path, .{}) catch
        fail("cannot read {s}", .{wabt_in_path});
    defer wabt_in_file.close(b.graph.io);
    var wabt_in_r = wabt_in_file.readerStreaming(b.graph.io, &.{});
    const wabt_template = wabt_in_r.interface.allocRemaining(arena, .limited(1 << 20)) catch
        fail("cannot read {s}", .{wabt_in_path});
    const wabt_bytes = renderWabtConfig(arena, wabt_template);
    const wabt_wf = b.addWriteFiles();
    const wabt_h = wabt_wf.add("config.h", wabt_bytes);
    std.debug.print("b4 wabt config.h: {d} bytes from lib/wabt/src/config.h.in\n", .{wabt_bytes.len});

    // jstoc.py generator (same edge as the Mac build).
    const jstoc = b.addSystemCommand(&.{python});
    jstoc.addFileArg(b.path("tools/build/jstoc_run.py"));
    jstoc.addFileArg(b.path("bin/ch/jstoc.py"));
    jstoc.addFileArg(b.path("bin/ch/DbgController.js"));
    jstoc.addArg("controllerScript");
    const dbg_header = jstoc.addOutputFileArg("DbgController.js.h");
    const gen_dir = dbg_header.dirname();

    const ctx = Ctx{
        .b = b,
        .zig_exe = b.graph.zig_exe,
        .icu_include = icu.include,
        .icu_libdir = icu.libdir,
        .config_dir = wf.getDirectory(),
        .config_h = config_h,
        .wabt_dir = wabt_wf.getDirectory(),
        .wabt_h = wabt_h,
        .gen_dir = gen_dir,
        .dbg_header = dbg_header,
    };

    // Compile every TU once, keyed by source path; archives and links
    // assemble objects via the reference order lists (static_order /
    // singular_orders), which carry the initializer order.
    var n: u32 = 0;
    var objs: std.StringHashMap(std.Build.LazyPath) = .init(arena);
    const all_groups = [_]*const tables.Group{
        &tables.pal_group,              &tables.codex_group,
        &tables.common_group,           &tables.core_group,
        &tables.datastructures_group,   &tables.exceptions_group,
        &tables.memory_group,           &tables.util_group,
        &tables.backend_group,          &tables.parser_group,
        &tables.scacore_group,          &tables.wasmreader_group,
        &tables.rt_base_group,          &tables.rt_bytecode_group,
        &tables.rt_debug_group,         &tables.rt_language_group,
        &tables.rt_library_group,       &tables.rt_math_group,
        &tables.platformagnostic_group, &tables.rt_types_group,
        &tables.jsrt_group,             &tables.jsrt_core_group,
        &tables.wabt_group,             &tables.static_src_group,
        &tables.dylib_group,            &tables.ch_group,
        &tables.gcstress_group,
    };
    for (all_groups) |g| {
        for (g.cxx) |src| {
            objs.put(src, compileTu(&ctx, g, src, .cxx, n)) catch @panic("OOM");
            n += 1;
        }
        for (g.c) |src| {
            objs.put(src, compileTu(&ctx, g, src, .c, n)) catch @panic("OOM");
            n += 1;
        }
        for (g.asm_files) |src| {
            objs.put(src, compileTu(&ctx, g, src, .asm_, n)) catch @panic("OOM");
            n += 1;
        }
    }
    std.debug.assert(n == 684);
    std.debug.assert(objs.count() == 684);

    // Archives via `zig ar` (reference: cmake `ar qc` + ranlib; member
    // order is the reference edge order).
    const ar_specs = [_]struct { name: []const u8, file: []const u8, order: []const []const u8 }{
        .{ .name = "ChakraCoreStatic", .file = "libChakraCoreStatic.a", .order = tables.static_order },
        .{ .name = "pal", .file = "libChakra.Pal.Singular.a", .order = &tables.singular_orders.pal },
        .{ .name = "codex", .file = "libChakra.Common.Codex.Singular.a", .order = &tables.singular_orders.codex },
        .{ .name = "platformagnostic", .file = "libChakra.Runtime.PlatformAgnostic.Singular.a", .order = &tables.singular_orders.platformagnostic },
    };
    var archives: std.StringHashMap(std.Build.LazyPath) = .init(arena);
    // `inline for` on both loops: `&tables.singular_orders.*` are
    // pointers to fields of a const anonymous struct, which have no
    // runtime address (a runtime `for` over ar_specs fails with
    // "runtime value contains reference to comptime var"). Unrolling
    // keeps every order element comptime-known at its use site.
    inline for (ar_specs) |spec| {
        const ar = b.addSystemCommand(&.{ ctx.zig_exe, "ar", "rcs" });
        const out = ar.addOutputFileArg(spec.file);
        inline for (spec.order) |src| {
            const o = objs.get(src) orelse fail("archive {s}: no object for {s}", .{ spec.name, src });
            ar.addFileArg(o);
        }
        archives.put(spec.name, out) catch @panic("OOM");
    }

    // Links via the `zig c++` driver. Per-flag accounting vs the
    // reference link lines (from build.ninja LINK_FLAGS/LINK_LIBRARIES):
    // * compile FLAGS (-Werror/-Wno/-O3/-g/-std/-msse4.2/...) also sit on
    //   the reference link lines; they are nil without LTO and dropped.
    // * .so: -fPIC -shared -soname kept; -Wl,-undefined,error becomes
    //   -Wl,--no-undefined (the linker's spelling; probed); -licuuc
    //   -licui18n -lpthread -ldl kept (default search); version script
    //   kept; --whole-archive/--start/end-group dropped (joined objects
    //   need no archive semantics); -static-libstdc++ dropped (libc++
    //   links statically by default; probed NEEDED has no C++ runtime).
    // * ch: -fPIE -pie --export-dynamic/-rdynamic kept; version script
    //   kept; singular archives + ICU/pthread/dl kept.
    // * GCStress: --export-dynamic/-rdynamic kept; --start/end-group kept
    //   (an archive is linked); -undefined,error covered by the exe
    //   default (undefined symbols always error on exes).
    const ch_link = b.addSystemCommand(&.{ctx.zig_exe});
    ch_link.addArg("c++");
    ch_link.addArg("-target");
    ch_link.addArg(profile_target);
    for ([_][]const u8{ "-fPIE", "-pie", "-Wl,--export-dynamic", "-rdynamic" }) |f| ch_link.addArg(f);
    for (tables.ch_group.cxx) |src| ch_link.addFileArg(objs.get(src).?);
    ch_link.addArg("-o");
    const ch_bin = ch_link.addOutputFileArg("ch");
    for ([_][]const u8{ "pal", "codex", "platformagnostic" }) |a| ch_link.addFileArg(archives.get(a).?);
    // `zig c++` searches no system library dirs by default (probed:
    // -licuuc without -L fails "unable to find dynamic system
    // library"), so the reference's default-searched ICU dir is spelled
    // explicitly. Honors -Dicu-libdir.
    ch_link.addArg(b.fmt("-L{s}", .{ctx.icu_libdir}));
    // Scoped --no-as-needed: LLD's as-needed would drop the
    // vestigial-but-reference-present libicui18n NEEDED (our ch
    // objects reference no icui18n symbol under libc++ headers, and
    // nothing else pulls it); the reference carries both ICU NEEDEDs,
    // so both are forced. pthread/dl stay under as-needed: they are
    // genuinely referenced (36 undefined pthread/dl symbols) and kept.
    ch_link.addArg("-Wl,--no-as-needed");
    for ([_][]const u8{ "-licuuc", "-licui18n" }) |f| ch_link.addArg(f);
    ch_link.addArg("-Wl,--as-needed");
    ch_link.addPrefixedFileArg("-Wl,--version-script=", b.path("bin/ch/ch.version"));
    for ([_][]const u8{ "-lpthread", "-ldl" }) |f| ch_link.addArg(f);
    ch_link.step.dependOn(&jstoc.step);

    const so_link = b.addSystemCommand(&.{ctx.zig_exe});
    so_link.addArg("c++");
    so_link.addArg("-target");
    so_link.addArg(profile_target);
    for ([_][]const u8{ "-fPIC", "-shared", "-Wl,-soname,libChakraCore.so" }) |f| so_link.addArg(f);
    for (tables.dylib_group.cxx) |src| so_link.addFileArg(objs.get(src).?);
    for (tables.static_order) |src| so_link.addFileArg(objs.get(src).?);
    so_link.addArg("-o");
    const so_bin = so_link.addOutputFileArg("libChakraCore.so");
    // Same default library search as ch above (probed, honors -Dicu-libdir).
    so_link.addArg(b.fmt("-L{s}", .{ctx.icu_libdir}));
    // Same scoped --no-as-needed as ch (belt-and-suspenders here: the
    // engine references icui18n symbols, so as-needed would keep it
    // anyway; the scope makes the reference NEEDED shape explicit).
    so_link.addArg("-Wl,--no-as-needed");
    for ([_][]const u8{ "-licuuc", "-licui18n" }) |f| so_link.addArg(f);
    so_link.addArg("-Wl,--as-needed");
    for ([_][]const u8{ "-lpthread", "-ldl" }) |f| so_link.addArg(f);
    so_link.addPrefixedFileArg("-Wl,--version-script=", b.path("bin/ChakraCore/libChakraCoreLib.version"));
    so_link.addArg("-Wl,--no-undefined");

    const gs_link = b.addSystemCommand(&.{ctx.zig_exe});
    gs_link.addArg("c++");
    gs_link.addArg("-target");
    gs_link.addArg(profile_target);
    for ([_][]const u8{ "-Wl,--export-dynamic", "-rdynamic" }) |f| gs_link.addArg(f);
    for (tables.gcstress_group.cxx) |src| gs_link.addFileArg(objs.get(src).?);
    gs_link.addArg("-o");
    const gs_bin = gs_link.addOutputFileArg("GCStress");
    // -pie spells the reference toolchain's default explicitly (B4.3):
    // Ubuntu clang links PIE by default (reference GCStress is DYN),
    // `zig c++` does not (ours came out EXEC). Same artifact class;
    // non-PIC objects link into PIE on x86-64 under both linkers.
    gs_link.addArg("-pie");
    gs_link.addArg("-Wl,--start-group");
    gs_link.addFileArg(archives.get("ChakraCoreStatic").?);
    gs_link.addArg("-Wl,--end-group");
    for ([_][]const u8{ "-ldl", "-lpthread" }) |f| gs_link.addArg(f);

    // Native installs (test profile owns the install system, as on Mac):
    // co-located ch + .so (the loader resolves the .so beside the exe),
    // archives, configs, staged headers, .profile marker.
    const install = b.getInstallStep();
    install.dependOn(&b.addInstallFile(ch_bin, "bin/ch").step);
    install.dependOn(&b.addInstallFile(gs_bin, "bin/GCStress").step);
    install.dependOn(&b.addInstallFile(so_bin, "bin/libChakraCore.so").step);
    install.dependOn(&b.addInstallFile(so_bin, "lib/libChakraCore.so").step);
    install.dependOn(&b.addInstallFile(archives.get("ChakraCoreStatic").?, "lib/libChakraCoreStatic.a").step);
    install.dependOn(&b.addInstallFile(archives.get("pal").?, "lib/libChakra.Pal.Singular.a").step);
    install.dependOn(&b.addInstallFile(archives.get("codex").?, "lib/libChakra.Common.Codex.Singular.a").step);
    install.dependOn(&b.addInstallFile(archives.get("platformagnostic").?, "lib/libChakra.Runtime.PlatformAgnostic.Singular.a").step);
    install.dependOn(&b.addInstallFile(config_h, "pal/src/config.h").step);
    install.dependOn(&b.addInstallFile(wabt_h, "lib/wabt/config.h").step);
    install.dependOn(&b.addInstallFile(b.path("lib/Jsrt/ChakraCore.h"), "include/ChakraCore.h").step);
    install.dependOn(&b.addInstallFile(b.path("lib/Jsrt/ChakraCommon.h"), "include/ChakraCommon.h").step);
    install.dependOn(&b.addInstallFile(b.path("lib/Jsrt/ChakraDebug.h"), "include/ChakraDebug.h").step);
    install.dependOn(&b.addInstallFile(b.path("lib/Common/ChakraCoreVersion.h"), "include/ChakraCoreVersion.h").step);
    const marker = wf.add(".profile", arena.dupe(u8, "test\n") catch @panic("OOM"));
    install.dependOn(&b.addInstallFile(marker, ".profile").step);

    const installed_ch: std.Build.LazyPath = .{ .relative = .{ .base = .install_bin, .sub_path = "ch" } };

    // Negative controls for the Linux gates (mirrors check-selftest).
    const selftest_top = b.step("check-selftest", "Negative controls for every Linux check gate (not part of the default build)");
    for ([_][]const u8{ "check_regression_linux.py", "check_smoke_linux.py", "check_metadata_linux.py", "check_native.py" }) |script| {
        const st = b.addSystemCommand(&.{python});
        st.addFileArg(b.path(b.fmt("tools/build/{s}", .{script})));
        st.addArg("--self-test");
        st.stdio = .inherit;
        selftest_top.dependOn(&st.step);
    }
    const tools_test = b.addSystemCommand(&.{python});
    tools_test.addFileArg(b.path("tools/build/test_build_tools.py"));
    tools_test.stdio = .inherit;
    selftest_top.dependOn(&tools_test.step);
    // Zig unit tests for the Linux gates: the PAL-config offline
    // render test (pal-config-linux/check.zig) and the gate-helper
    // tests above, collected via the tools/linux-gate-tests.zig
    // test root (see that file for why the root lives at tools/).
    const gate_tests = b.addTest(.{
        .root_module = b.createModule(.{
            .root_source_file = b.path("tools/linux-gate-tests.zig"),
            .target = b.graph.host,
            .optimize = .debug,
        }),
    });
    selftest_top.dependOn(&b.addRunArtifact(gate_tests).step);
    const tools_test_top = b.step("check-build-tools", "Failure-path tests for the verification tools (no engine build required)");
    tools_test_top.dependOn(&tools_test.step);

    // (1) Bytecode check (legacy-exact: --jit --verify, JIT-on profile).
    const verify_bc = b.addSystemCommand(&.{python});
    verify_bc.addFileArg(b.path("tools/build/verify_bytecode_pure.py"));
    verify_bc.addFileArg(b.path("tools/regenByteCode.py"));
    verify_bc.addArg("--jit");
    verify_bc.addArg("--verify");
    verify_bc.addPrefixedFileArg("--binary=", installed_ch);
    verify_bc.step.dependOn(install);
    verify_bc.stdio = .inherit;
    const verify_bc_top = b.step("verify-bytecode", "Verify checked-in built-in bytecode with the just-built ch (--jit; pure scratch-tree check; not part of the default build)");
    verify_bc_top.dependOn(&verify_bc.step);

    // (2) Regression suite (legacy-exact: runtests.py --test).
    const regression = b.addSystemCommand(&.{python});
    regression.addFileArg(b.path("tools/build/check_regression_linux.py"));
    regression.addFileArg(b.path("test/runtests.py"));
    regression.addFileArg(installed_ch);
    regression.step.dependOn(install);
    regression.stdio = .inherit;
    const regression_top = b.step("check-regression", "Run the regression suite (--test, interpreted/dynapogo) against the zig-built ch (not part of the default build)");
    regression_top.dependOn(&regression.step);

    // (3) Shared embedding test (legacy-exact RelWithDebInfo argv; the
    // Mac check_native.py is reused: same script, same harness).
    const cc = b.findProgram(.{ .names = &.{"clang"} }) orelse "clang";
    const cxx = b.findProgram(.{ .names = &.{"clang++"} }) orelse "clang++";
    const native = b.addSystemCommand(&.{python});
    native.addFileArg(b.path("tools/build/check_native.py"));
    native.addFileArg(b.path("test/native-tests/test_native.sh"));
    native.addFileArg(installed_ch);
    native.addArg(cc);
    native.addArg(cxx);
    native.addArg("--build-type");
    native.addArg("RelWithDebInfo");
    native.step.dependOn(install);
    native.stdio = .inherit;
    const native_top = b.step("check-native", "Run the shared embedding test against zig artifacts (not part of the default build)");
    native_top.dependOn(&native.step);

    // (4) Smoke/flag output assertions.
    const smoke = b.addSystemCommand(&.{python});
    smoke.addFileArg(b.path("tools/build/check_smoke_linux.py"));
    smoke.addFileArg(installed_ch);
    smoke.step.dependOn(install);
    smoke.stdio = .inherit;
    const smoke_top = b.step("check-smoke", "Assert ch version/flag output text and script execution (not part of the default build)");
    smoke_top.dependOn(&smoke.step);

    // (4b) JIT smoke: the test profile is shared/JIT-on, so prove the native
    // backend is present and both the -mic and dynapogo class/helper paths
    // run. check_jit.py is shared with the macOS jit profile; it selects
    // libChakraCore.so on Linux. The e14 class probe pins the aux-slot
    // store that produced the fulljit SIGSEGV family.
    const jit_smoke = b.addSystemCommand(&.{python});
    jit_smoke.addFileArg(b.path("tools/build/check_jit.py"));
    jit_smoke.addFileArg(installed_ch);
    jit_smoke.step.dependOn(install);
    jit_smoke.stdio = .inherit;
    const jit_top = b.step("check-jit", "JIT smoke: NativeCodeGenerator present; -mic and dynapogo class/helper checks (not part of the default build)");
    jit_top.dependOn(&jit_smoke.step);

    // (5) Metadata gates: ELF, SONAME/NEEDED, version-script exports,
    // configs, headers, archives.
    const metadata = b.addSystemCommand(&.{python});
    metadata.addFileArg(b.path("tools/build/check_metadata_linux.py"));
    metadata.addDirectoryArg(.{ .relative = .{ .base = .install_prefix } });
    metadata.step.dependOn(install);
    metadata.stdio = .inherit;
    const metadata_top = b.step("check-metadata", "Assert artifact metadata: ELF, SONAME, exports, configs, headers, archives (not part of the default build)");
    metadata_top.dependOn(&metadata.step);

    const check_top = b.step("check", "Full legacy-equivalent verification: regression, bytecode (--jit), native embedding, smoke, metadata (not part of the default build)");
    // Negative controls ride along with the full check, mirroring the
    // Mac graph ("Wired into every profile's `check`" in build.zig);
    // without this the Linux gates' can-fail proofs never ran under
    // `zig build check` on Linux.
    check_top.dependOn(selftest_top);
    check_top.dependOn(&regression.step);
    check_top.dependOn(&verify_bc.step);
    check_top.dependOn(&native.step);
    check_top.dependOn(&smoke.step);
    check_top.dependOn(&jit_smoke.step);
    check_top.dependOn(&metadata.step);
}
