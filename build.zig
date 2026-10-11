//-------------------------------------------------------------------------------------------------------
// Copyright (c) ChakraCore Project Contributors. All rights reserved.
// Licensed under the MIT license. See LICENSE.txt file in the project root for full license information.
//-------------------------------------------------------------------------------------------------------
//! B2.2/B2.3: full Mac compile+link graph — all 24 source groups, static
//! archive(s), libChakraCore.dylib, dynamic ch, GCStress, staged headers —
//! plus the B2.3 `check` aggregation (regression, bytecode, native
//! embedding, smoke, metadata) and `check-selftest` negative controls.
//!
//! Exactly ONE profile: native ARM64 macOS, shared/no-JIT, RelWithDebInfo Test, system ICU,
//! macOS minimum 26.0, MacOSX SDK 26.5, Zig 0.17.0. Anything else fails before doing work.
//! Profiles/incremental/CI are B3; the ordering-correctness proof (PAL
//! deviation dispositioned) lives in doc/b2-verification.md.
//!
//! Contracts: doc/build-contract-mac.md (sources/flags/includes/archives/links),
//! doc/pal-config-contract-mac.md (config.h checks), doc/build.md sections 13-14
//! (pilot findings), doc/modernization-plan.md sections B2/2.3 (Zig API facts).
//!
//! Link-order note: Zig 0.17's Mach-O linker lays out input objects (and hence
//! C++ static initializers) sorted lexicographically by object basename —
//! input order is ignored (proven by sorted/renamed-object probes). ChakraCore
//! relies on reference link order for initializer sequencing on Mach-O (its
//! INIT_PRIORITY attributes are flat there), so every TU compiles to a
//! zero-padded `NNNN-<base>.o` whose lexicographic order reproduces the
//! reference link order with two known deviations (see numbering site):
//! ChakraCoreStatic.cpp.o is numbered first-in-static while the reference
//! archives it last (static-link idx 522/523 — immaterial: the TU is a dummy
//! `void EMPTY_FUNCTION(){}` with no initializers), and the PAL section
//! differs (reference: 18 cruntime C++, 21 safecrt C, 47 C++ debug..threadsusp,
//! mach pair, 3 asm; ours: mach pair moved first, 65 C++ in
//! reference-relative order, 21 C, 3 asm — no dynamic initializers
//! in C/asm by construction; mach pair only constant-initialized POD statics).
//! One global numbering serves all links: ch objects, GCStress objects, dylib
//! objects, then the static archive members. The PAL deviation is
//! dispositioned in doc/b2-verification.md: the reordered C/asm/mach
//! inputs carry no dynamic initializers, so their relative order is
//! unobservable at startup.

const std = @import("std");
const builtin = @import("builtin");
const pal_config = @import("tools/pal-config/check.zig");
const tables = @import("tools/build/tables.zig");

const profile_target = "aarch64-macos.26.0";
const profile_sdk_version = "26.5";
const default_icu_include = "/opt/homebrew/opt/icu4c/include";

fn fail(comptime fmt: []const u8, args: anytype) noreturn {
    std.debug.print("build error: " ++ fmt ++ "\n", args);
    std.process.exit(1);
}

fn gateToolchain() void {
    if (builtin.zig_version.major != 0 or builtin.zig_version.minor != 17 or builtin.zig_version.patch != 0) {
        fail("requires Zig 0.17.0 (mise.toml pin); running {f}", .{builtin.zig_version});
    }
    if (builtin.os.tag != .macos or builtin.cpu.arch != .aarch64) {
        fail("profile is native-only: build machine must be ARM64 macOS for native run probes, have {t}-{t}", .{ builtin.cpu.arch, builtin.os.tag });
    }
}

fn gateTarget(b: *std.Build) std.Build.ResolvedTarget {
    const rt = b.standardTargetOptions(.{.default_target = .{
        .cpu_arch = .aarch64,
        .cpu_model = .{ .explicit = &std.Target.aarch64.cpu.apple_m1 },
        .os_tag = .macos,
        .os_version_min = .{ .semver = .{ .major = 26, .minor = 0, .patch = 0 } },
        .abi = .none,
    }});
    // Note: an unparseable -Dtarget does NOT fail here. standardTargetOptions
    // marks the input invalid but returns the default query, so the 95 probes
    // below still run; Zig's deferred option validation then fails the build
    // afterwards. Known framework behavior; the exit code stays correct (1).
    if (b.user_input_options.get("cpu")) |provided| {
        const val: []const u8 = switch (provided) {
            .scalar => |s| s,
            .flag => "(flag)",
            else => "(non-scalar)",
        };
        fail("unsupported -Dcpu={s}: this profile pins the apple_m1 CPU model; -Dcpu overrides are rejected", .{val});
    }
    const t = rt.result;
    if (t.cpu.arch != .aarch64)
        fail("unsupported arch {t}: this profile builds only aarch64-macos.26.0", .{t.cpu.arch});
    if (t.os.tag != .macos)
        fail("unsupported OS {t}: this profile builds only aarch64-macos.26.0", .{t.os.tag});
    if (t.abi != .none)
        fail("unsupported ABI {t}: this profile builds only the default macOS ABI (none)", .{t.abi});
    if (!std.mem.eql(u8, t.cpu.model.name, "apple_m1"))
        fail("unsupported CPU model {s}: this profile builds only apple_m1 (the aarch64-macos baseline)", .{t.cpu.model.name});
    const min = t.os.version_range.semver.min;
    if (min.major != 26 or min.minor != 0 or min.patch != 0)
        fail("unsupported macOS minimum {d}.{d}.{d}: this profile builds only 26.0", .{ min.major, min.minor, min.patch });
    return rt;
}

fn gateOptimize(b: *std.Build) std.builtin.Optimize {
    const choice = b.option(std.builtin.Optimize, "optimize", "Optimization (this profile supports only fast)") orelse .fast;
    if (choice != .fast)
        fail("unsupported -Doptimize={t}: this profile (RelWithDebInfo Test, -O3 -g) supports only fast", .{choice});
    return choice;
}

fn gateSdk(b: *std.Build) []const u8 {
    const arena = b.graph.arena;
    const sdk_path = b.option([]const u8, "sdk-path", "MacOSX SDK path (default: xcrun --show-sdk-path)") orelse blk: {
        const res = b.runFallible(&.{ "xcrun", "--show-sdk-path" }, .{});
        switch (res) {
            .success => |out| break :blk std.mem.trim(u8, out, " \t\r\n"),
            else => fail("cannot resolve SDK: xcrun --show-sdk-path failed; pass -Dsdk-path=<MacOSX.sdk>", .{}),
        }
    };
    const io = b.graph.io;
    std.Io.Dir.accessAbsolute(io, sdk_path, .{}) catch
        fail("SDK not found: {s} does not exist; pass -Dsdk-path=<MacOSX.sdk>", .{sdk_path});
    const settings_path = std.fmt.allocPrint(arena, "{s}/SDKSettings.json", .{sdk_path}) catch @panic("OOM");
    const sf = std.Io.Dir.openFileAbsolute(io, settings_path, .{}) catch
        fail("cannot read {s}: SDK version unverifiable (need MacOSX SDK {s})", .{ settings_path, profile_sdk_version });
    defer sf.close(io);
    var sfr = sf.readerStreaming(io, &.{});
    const bytes = sfr.interface.allocRemaining(arena, .limited(1 << 20)) catch
        fail("cannot read {s}: SDK version unverifiable (need MacOSX SDK {s})", .{ settings_path, profile_sdk_version });
    const parsed = std.json.parseFromSlice(std.json.Value, arena, bytes, .{}) catch
        fail("cannot parse {s}: SDK version unverifiable (need MacOSX SDK {s})", .{ settings_path, profile_sdk_version });
    const version_value = parsed.value.object.get("Version") orelse
        fail("{s} has no Version key: SDK version unverifiable (need MacOSX SDK {s})", .{ settings_path, profile_sdk_version });
    const sdk_version = switch (version_value) {
        .string => |s| s,
        else => fail("{s} Version is not a string: SDK version unverifiable (need MacOSX SDK {s})", .{ settings_path, profile_sdk_version }),
    };
    if (!std.mem.eql(u8, sdk_version, profile_sdk_version))
        fail("unsupported SDK {s}: this profile requires MacOSX SDK {s} (have {s})", .{ sdk_path, profile_sdk_version, sdk_version });
    return sdk_path;
}

fn gateIcu(b: *std.Build) []const u8 {
    const arena = b.graph.arena;
    const io = b.graph.io;
    const icu = b.option([]const u8, "icu-include", "ICU include dir (default: " ++ default_icu_include ++ ")") orelse default_icu_include;
    const uchar = std.fmt.allocPrint(arena, "{s}/unicode/uchar.h", .{icu}) catch @panic("OOM");
    std.Io.Dir.accessAbsolute(io, uchar, .{}) catch
        fail("Cannot find ICU (missing {s}). Try installing libicu-dev or the appropriate packages for your platform. Pass -Dicu-include=<icu>/include to point at ICU headers.", .{uchar});
    const lib = std.fmt.allocPrint(arena, "{s}/../lib/libicuuc.dylib", .{icu}) catch @panic("OOM");
    std.Io.Dir.accessAbsolute(io, lib, .{}) catch
        fail("Cannot find ICU libraries (missing {s}). Try installing libicu-dev or the appropriate packages for your platform. Pass -Dicu-include=<icu>/include to point at the matching ICU.", .{lib});
    return icu;
}

const Ctx = struct {
    b: *std.Build,
    target: std.Build.ResolvedTarget,
    optimize: std.builtin.Optimize,
    sdk_path: []const u8,
    icu_include: []const u8,
    sdk_isystem: []const u8,
    sdk_frameworks: []const u8,
    sdk_lib: []const u8,
    zig_exe: []const u8,
    config_dir: std.Build.LazyPath,
    gen_dir: std.Build.LazyPath,
    dbg_header: std.Build.LazyPath,
};

const Lang = enum { cxx, c, asm_ };

// SDK search paths for the link: the build API has no --sysroot knob, so
// pass the absolute SDK -F/-L paths (the pilot's sysroot-relative -F
// /System/Library/Frameworks and -L /usr/lib, doc/build.md section 14).
// Known deviation: linked binaries stamp LC_BUILD_VERSION sdk 27.0 (otool)
// while the gate pins MacOSX SDK 26.5 — with no sysroot knob the stamp comes
// from the toolchain default; minos 26.0 holds. The reference stamps 27.0
// too, so this matches the reference.
fn addSdkLinkPaths(ctx: *const Ctx, mod: *std.Build.Module) void {
    mod.addFrameworkPath(.{ .cwd_relative = ctx.sdk_frameworks });
    mod.addLibraryPath(.{ .cwd_relative = ctx.sdk_lib });
}

fn createLinkModule(ctx: *const Ctx) *std.Build.Module {
    return ctx.b.createModule(.{
        .target = ctx.target,
        .optimize = ctx.optimize,
        .link_libc = true,
        .link_libcpp = true,
    });
}

// Append one TU's compile flags to a `zig c++|cc` run step, in observed
// order: defines, includes (generated dirs at their reference positions),
// warnings (C++ only), codegen tail. Drops target-owned/dead/cosmetic
// entries (-mmacosx-version-min, -arch, -std=gnu++11, -Werror/-Wno-error,
// -fdiagnostics-color); the driver supplies target/sysroot/CPU from argv.
fn appendTuFlags(ctx: *const Ctx, run: *std.Build.Step.Run, g: *const tables.Group, lang: Lang) void {
    const b = ctx.b;
    const defines = if (g.pal) tables.pal_defines else tables.engine_defines;
    for (defines) |d| run.addArg(d);
    if (g.target_define) |d| run.addArg(d);

    const is_ch = g == &tables.ch_group;
    if (g.pal) run.addPrefixedDirectoryArg("-I", ctx.config_dir);
    const rels = if (g.pal) tables.pal_includes_rel else tables.global_includes_rel;
    for (rels) |dir| run.addArg(b.fmt("-I{s}", .{dir}));
    run.addArg(b.fmt("-I{s}", .{ctx.icu_include}));
    run.addArg(b.fmt("-I{s}/../i18n", .{ctx.icu_include}));
    for (g.extra_includes, 0..) |dir, i| {
        if (is_ch and i == tables.gen_include_index)
            run.addPrefixedDirectoryArg("-I", ctx.gen_dir);
        run.addArg(b.fmt("-I{s}", .{dir}));
    }
    if (g.pal) run.addArg("-Ipal/src/include");
    run.addArg("-isystem");
    run.addArg("/usr/local/include");

    switch (lang) {
        .cxx => {
            for (tables.warnings) |w| run.addArg(w);
            for ([_][]const u8{ "-fno-omit-frame-pointer", "-fdelayed-template-parsing", "-O3", "-g", "-std=gnu++17" }) |f| run.addArg(f);
            // -fPIE sits mid-line on ch (target property); -fPIC sits
            // late on lib units, after -fwrapv (observed command lines).
            if (g.pic == .pie) run.addArg("-fPIE");
            for ([_][]const u8{ "-fasm-blocks", "-fms-extensions", "-fwrapv" }) |f| run.addArg(f);
            if (g.pal) {
                for ([_][]const u8{ "-fstack-protector", "-fno-builtin", "-fPIC" }) |f| run.addArg(f);
            } else if (g.pic == .pic) {
                run.addArg("-fPIC");
            }
            // Build-required additions: the SDK C++ headers
            // (-isystem, absolute) and SDK frameworks (-F, sysroot-
            // relative: the driver resolves -F against --sysroot, so an
            // absolute -F would double the prefix — pilot finding), the
            // pinned apple_m1 CPU, and -Wno-date-time (zig clang 22
            // promotes __DATE__/__TIME__; pilot finding). The c++ driver
            // injects no NDEBUG, matching the reference C++ (contract
            // quirk Q4); -UNDEBUG is a defensive no-op.
            run.addArg("-isystem");
            run.addArg(ctx.sdk_isystem);
            run.addArg("-F");
            run.addArg("/System/Library/Frameworks");
            run.addArg("-mcpu=apple_m1");
            run.addArg("-UNDEBUG");
            run.addArg("-Wno-date-time");
        },
        .c, .asm_ => {
            if (lang == .c) {
                run.addArg("-std=gnu99");
                run.addArg("-O2");
            } else {
                run.addArg("-O2");
            }
            for ([_][]const u8{ "-g", "-DNDEBUG", "-fasm-blocks", "-fms-extensions", "-fwrapv" }) |f| run.addArg(f);
            if (g.pal) {
                for ([_][]const u8{ "-fstack-protector", "-fno-builtin", "-fPIC" }) |f| run.addArg(f);
            } else if (g.pic == .pic) {
                run.addArg("-fPIC");
            }
            run.addArg("-isystem");
            run.addArg(ctx.sdk_isystem);
            run.addArg("-F");
            run.addArg("/System/Library/Frameworks");
            run.addArg("-mcpu=apple_m1");
            run.addArg("-Wno-date-time");
        },
    }
}

// Compile one TU to a sequence-numbered object: `NNNN-<base>.o`. The
// zero-padded number is the TU's reference link order (see the file doc
// comment for the two known deviations); the linker sorts objects
// lexicographically by basename, so the numbers reproduce the reference
// layout and initializer order modulo those deviations.
fn compileTu(ctx: *const Ctx, g: *const tables.Group, src: []const u8, lang: Lang, number: u32) std.Build.LazyPath {
    const b = ctx.b;
    std.debug.assert(number < 10000);
    const run = b.addSystemCommand(&.{ ctx.zig_exe, if (lang == .cxx) "c++" else "cc" });
    run.addArg("-target");
    run.addArg(profile_target);
    run.addArg("--sysroot");
    run.addArg(ctx.sdk_path);
    appendTuFlags(ctx, run, g, lang);
    run.addArg("-o");
    var digits: [4]u8 = .{ '0', '0', '0', '0' };
    var n = number;
    var i: usize = 4;
    while (i > 0) {
        i -= 1;
        digits[i] = '0' + @as(u8, @intCast(n % 10));
        n /= 10;
    }
    const out = run.addOutputFileArg(b.fmt("{s}-{s}.o", .{ digits, std.fs.path.basename(src) }));
    run.addArg("-c");
    run.addFileArg(b.path(src));
    // The generated DbgController.js.h is a TRACKED build input of every
    // ch compile (contract section 5.4 edge): addFileInput both orders
    // the compile after the generator and rebuilds it when the header
    // bytes change (e.g. after touching bin/ch/DbgController.js).
    if (g == &tables.ch_group) run.addFileInput(ctx.dbg_header);
    return out;
}

pub fn build(b: *std.Build) void {
    gateToolchain();
    const target = gateTarget(b);
    const optimize = gateOptimize(b);
    const sdk_path = gateSdk(b);
    const icu_include = gateIcu(b);
    const python = b.findProgram(.{ .names = &.{ "python3", "python" } }) orelse
        fail("cannot find python3 or python on PATH; needed to run bin/ch/jstoc.py for DbgController.js.h", .{});

    std.debug.print("b2 profile: target={s} sdk={s}@{s} icu-include={s} zig={f} python={s}\n", .{
        profile_target, sdk_path, profile_sdk_version, icu_include, builtin.zig_version, python,
    });

    // Build-owned pal/src/config.h: 95 real checks, fail-clearly on deviation.
    const cfg = pal_config.generate(.{
        .io = b.graph.io,
        .zig_exe = b.graph.zig_exe,
        .target = profile_target,
        .sysroot = sdk_path,
    });
    defer std.heap.page_allocator.free(cfg.config_h);
    if (!cfg.tail_calls) fail("CLANG_HAS_DISABLE_TAIL_CALLS check failed: this profile requires a compiler with __attribute__((disable_tail_calls))", .{});

    const arena = b.graph.arena;
    const wf = b.addWriteFiles();
    const config_h = wf.add("config.h", arena.dupe(u8, cfg.config_h) catch @panic("OOM"));

    // jstoc.py generator (contract section 5.4): the script writes
    // <basename>.h into its CWD, which cannot be a declared step output,
    // so tools/build/jstoc_run.py runs it in a temp dir and delivers the
    // header to this step's declared output. Inputs (jstoc.py,
    // DbgController.js, the wrapper) and the header are all tracked. A
    // no-op rebuild changes no artifact bytes (zero hash delta;
    // regular-file mtimes untouched) — but it still runs the configure
    // phase (all gates plus the 95 pal-config probes) and re-creates
    // the two versioned-dylib symlinks (same targets, fresh mtimes).
    // Touching the JS re-runs the generator and recompiles all 8 ch
    // TUs via addFileInput — required-correct, not just conservative:
    // every ch TU includes stdafx.h -> Debugger.h ->
    // DbgController.js.h (only Debugger.cpp names the controllerScript
    // variable itself).
    const jstoc = b.addSystemCommand(&.{python});
    jstoc.addFileArg(b.path("tools/build/jstoc_run.py"));
    jstoc.addFileArg(b.path("bin/ch/jstoc.py"));
    jstoc.addFileArg(b.path("bin/ch/DbgController.js"));
    jstoc.addArg("controllerScript");
    const dbg_header = jstoc.addOutputFileArg("DbgController.js.h");
    const gen_dir = dbg_header.dirname();

    const ctx = Ctx{
        .b = b,
        .target = target,
        .optimize = optimize,
        .sdk_path = sdk_path,
        .icu_include = icu_include,
        .sdk_isystem = b.fmt("{s}/usr/include", .{sdk_path}),
        .sdk_frameworks = b.fmt("{s}/System/Library/Frameworks", .{sdk_path}),
        .sdk_lib = b.fmt("{s}/usr/lib", .{sdk_path}),
        .zig_exe = b.graph.zig_exe,
        .config_dir = wf.getDirectory(),
        .gen_dir = gen_dir,
        .dbg_header = dbg_header,
    };

    // Compile every TU once, numbered in reference link order: ch objects,
    // GCStress objects, dylib objects, then the static archive members
    // (ChakraCoreStatic.cpp plus the 20 OBJECT groups in TARGET_OBJECTS
    // order). Each link consumes its own subsequence; relative order is
    // the reference order in every link, modulo the two deviations in the
    // file doc comment (ChakraCoreStatic.cpp.o first vs ar-last; PAL
    // section layout — dispositioned in doc/b2-verification.md: the
    // reordered inputs carry no dynamic initializers).
    var n: u32 = 0;
    var ch_objs: std.ArrayList(std.Build.LazyPath) = .empty;
    for (tables.ch_group.cxx) |src| {
        ch_objs.append(arena, compileTu(&ctx, &tables.ch_group, src, .cxx, n)) catch @panic("OOM");
        n += 1;
    }
    var gs_objs: std.ArrayList(std.Build.LazyPath) = .empty;
    for (tables.gcstress_group.cxx) |src| {
        gs_objs.append(arena, compileTu(&ctx, &tables.gcstress_group, src, .cxx, n)) catch @panic("OOM");
        n += 1;
    }
    var dylib_objs: std.ArrayList(std.Build.LazyPath) = .empty;
    for (tables.dylib_group.cxx) |src| {
        dylib_objs.append(arena, compileTu(&ctx, &tables.dylib_group, src, .cxx, n)) catch @panic("OOM");
        n += 1;
    }
    var static_objs: std.ArrayList(std.Build.LazyPath) = .empty;
    // Singular archives for ch (contract section 5.3), one per group whose
    // Group.singular_name is set — a single loop, in object_groups order
    // (Pal, Codex, PlatformAgnostic), driving creation, ch linkage, and
    // install below.
    var singular_libs: std.ArrayList(*std.Build.Step.Compile) = .empty;
    for (tables.static_src_group.cxx) |src| {
        static_objs.append(arena, compileTu(&ctx, &tables.static_src_group, src, .cxx, n)) catch @panic("OOM");
        n += 1;
    }
    for (tables.object_groups) |g| {
        const start = static_objs.items.len;
        for (g.cxx) |src| {
            static_objs.append(arena, compileTu(&ctx, g, src, .cxx, n)) catch @panic("OOM");
            n += 1;
        }
        for (g.c) |src| {
            static_objs.append(arena, compileTu(&ctx, g, src, .c, n)) catch @panic("OOM");
            n += 1;
        }
        for (g.asm_files) |src| {
            static_objs.append(arena, compileTu(&ctx, g, src, .asm_, n)) catch @panic("OOM");
            n += 1;
        }
        if (g.singular_name) |name| {
            const mod = createLinkModule(&ctx);
            for (static_objs.items[start..]) |o| mod.addObjectFile(o);
            singular_libs.append(arena, b.addLibrary(.{ .name = name, .root_module = mod })) catch @panic("OOM");
        }
    }
    std.debug.assert(n == 538);
    std.debug.assert(singular_libs.items.len == 3);

    // libChakraCoreStatic.a (contract section 5.2).
    const static_mod = createLinkModule(&ctx);
    for (static_objs.items) |o| static_mod.addObjectFile(o);
    const static_lib = b.addLibrary(.{ .name = "ChakraCoreStatic", .root_module = static_mod });

    // libChakraCore.dylib (contract section 5.6): 3 objects plus every
    // engine TU linked directly — the equivalent of the reference
    // -force_load of the static archive (Zig's Mach-O linker has no
    // force-load spelling; the pilot joins the flag instead, doc/build.md
    // section 14). ICU via -L + -l; pthread/dl fold into libc (libSystem).
    const icu_lib = b.fmt("{s}/../lib", .{icu_include});
    const dylib_mod = createLinkModule(&ctx);
    for (dylib_objs.items) |o| dylib_mod.addObjectFile(o);
    for (static_objs.items) |o| dylib_mod.addObjectFile(o);
    addSdkLinkPaths(&ctx, dylib_mod);
    dylib_mod.addLibraryPath(.{ .cwd_relative = icu_lib });
    dylib_mod.linkSystemLibrary("icuuc", .{});
    dylib_mod.linkSystemLibrary("icui18n", .{});
    dylib_mod.linkSystemLibrary("icudata", .{});
    dylib_mod.linkFramework("CoreFoundation", .{});
    dylib_mod.linkFramework("Security", .{});
    // .version 1.0.0: otool confirms LC_ID_DYLIB current 1.0.0 /
    // compatibility 1.0.0 on both our dylib and the reference
    // build-zigpilot/libChakraCore.dylib (minos 26.0 both sides). The
    // reference CMake sets no VERSION/SOVERSION — 1.0.0 is the toolchain
    // default on both sides, so the values coincide. Keep.
    const dylib = b.addLibrary(.{
        .name = "ChakraCore",
        .root_module = dylib_mod,
        .linkage = .dynamic,
        .version = .{ .major = 1, .minor = 0, .patch = 0 },
    });
    dylib.install_name = "@rpath/libChakraCore.dylib";
    dylib.headerpad_max_install_names = true;

    // ch (contract section 5.7): 8 objects plus the 3 singular archives
    // plus ICU; no link-time dependency on the dylib (loaded at runtime
    // from the executable's own directory).
    const ch_mod = createLinkModule(&ctx);
    for (ch_objs.items) |o| ch_mod.addObjectFile(o);
    addSdkLinkPaths(&ctx, ch_mod);
    ch_mod.addLibraryPath(.{ .cwd_relative = icu_lib });
    ch_mod.linkSystemLibrary("icuuc", .{});
    ch_mod.linkSystemLibrary("icui18n", .{});
    ch_mod.linkSystemLibrary("icudata", .{});
    ch_mod.linkFramework("CoreFoundation", .{});
    ch_mod.linkFramework("Security", .{});
    for (singular_libs.items) |lib| ch_mod.linkLibrary(lib);
    const ch = b.addExecutable(.{ .name = "ch", .root_module = ch_mod });
    // ch DEFINES _OnChakraCoreLoadedEntry (bin/ch/ch.cpp:46, extern "C") for
    // the dylib to import at runtime; retention in the executable comes from
    // default Mach-O executable export of a defined global, not from -u.
    // The call stays: verified effective and harmless (nm shows the as-built
    // ch exporting T _OnChakraCoreLoadedEntry).
    ch.forceUndefinedSymbol("_OnChakraCoreLoadedEntry");
    ch.headerpad_max_install_names = true;
    ch.stack_size = 8 * 1024 * 1024;
    ch.step.dependOn(&jstoc.step);

    // GCStress (contract section 5.8): 4 objects plus the static archive.
    // 8 MiB stack matches the reference behavior (the pilot applied the
    // same stack contract to every Mach-O executable edge).
    const gs_mod = createLinkModule(&ctx);
    for (gs_objs.items) |o| gs_mod.addObjectFile(o);
    addSdkLinkPaths(&ctx, gs_mod);
    gs_mod.linkLibrary(static_lib);
    gs_mod.linkFramework("CoreFoundation", .{});
    gs_mod.linkFramework("Security", .{});
    const gs = b.addExecutable(.{ .name = "GCStress", .root_module = gs_mod });
    gs.headerpad_max_install_names = true;
    gs.stack_size = 8 * 1024 * 1024;

    b.installArtifact(ch);
    b.installArtifact(gs);
    b.installArtifact(dylib);
    b.installArtifact(static_lib);
    for (singular_libs.items) |lib| b.installArtifact(lib);
    // Co-location edge (contract §5.6–5.7): the reference POST_BUILD copies
    // both ch and libChakraCore.dylib to the build root so the loader finds
    // the dylib beside the executable (ChakraRtInterface resolves
    // <exe-dir>/libChakraCore.dylib); mirror that by installing a second
    // copy of the dylib beside zig-out/bin/ch. Without this, as-installed
    // ch exits 5 (dlopen "no such file").
    b.getInstallStep().dependOn(&b.addInstallFile(dylib.getEmittedBin(), "bin/libChakraCore.dylib").step);
    b.getInstallStep().dependOn(&b.addInstallFile(config_h, "pal/src/config.h").step);
    // Staged public headers, unchanged (contract section 5.9).
    b.getInstallStep().dependOn(&b.addInstallFile(b.path("lib/Jsrt/ChakraCore.h"), "include/ChakraCore.h").step);
    b.getInstallStep().dependOn(&b.addInstallFile(b.path("lib/Jsrt/ChakraCommon.h"), "include/ChakraCommon.h").step);
    b.getInstallStep().dependOn(&b.addInstallFile(b.path("lib/Jsrt/ChakraDebug.h"), "include/ChakraDebug.h").step);
    b.getInstallStep().dependOn(&b.addInstallFile(b.path("lib/Common/ChakraCoreVersion.h"), "include/ChakraCoreVersion.h").step);

    // (2) Bytecode check (contract section 5.10): the build consumes the
    // CHECKED-IN JsBuiltIn bytecode headers to bootstrap — this check
    // step is the ONLY regenByteCode.py reference in the graph, so no
    // build edge regenerates bytecode with the not-yet-built engine
    // (the script gets --binary=<installed ch>, which exists only after
    // the install step this check depends on). NOT a build dependency:
    // gate (2) of the B2.3 `check` aggregation below (check_top.dependOn),
    // also runnable standalone via `zig build verify-bytecode`.
    // Purity: regenByteCode.py --verify REWRITES the checked-in headers
    // even on success (fresh mtimes; on mismatch it leaves the new bytes
    // behind, self-healing red-to-green), so the step runs through
    // tools/build/verify_bytecode_pure.py, which snapshots the headers,
    // runs the script, restores every header byte- and mtime-identical,
    // and exits with the script's verdict.
    const verify_bc = b.addSystemCommand(&.{python});
    verify_bc.addFileArg(b.path("tools/build/verify_bytecode_pure.py"));
    verify_bc.addFileArg(b.path("tools/regenByteCode.py"));
    verify_bc.addArg("--noJit");
    verify_bc.addArg("--verify");
    verify_bc.addPrefixedFileArg("--binary=", .{ .relative = .{ .base = .install_bin, .sub_path = "ch" } });
    verify_bc.step.dependOn(b.getInstallStep());
    verify_bc.stdio = .inherit;
    const verify_bc_top = b.step("verify-bytecode", "Verify checked-in built-in bytecode with the just-built ch (contract 5.10; pure check, headers restored byte- and mtime-identical; not part of the default build)");
    verify_bc_top.dependOn(&verify_bc.step);

    // B2.3 full check aggregation (modernization-plan.md section B2
    // "Done when"): every sub-check depends on the install step (so
    // `zig build check` is fresh-checkout safe with no CMake/Ninja),
    // runs with inherited stdio (always executes when in the graph,
    // loud live output), and fails the step on mismatch. None of
    // these steps is reachable from the default build.
    // Native-gate compilers resolve LAZILY: the absolute path when
    // present, else the bare tool name, which check_native.py resolves
    // via shutil.which when the gate actually runs (failing loudly when
    // absent). A configure-time findProgram-or-fail here would break even
    // plain `zig build` on machines without standalone clang.
    const cc = b.findProgram(.{ .names = &.{"clang"} }) orelse "clang";
    const cxx = b.findProgram(.{ .names = &.{"clang++"} }) orelse "clang++";
    const installed_ch: std.Build.LazyPath = .{ .relative = .{ .base = .install_bin, .sub_path = "ch" } };

    // (1) Regression suite: runtests.py --test --variants=disable_jit
    // --binary=<installed ch> — the legacy-exact selection for this
    // profile (test/CMakeLists.txt:39-44; no --not-tag since ICU is
    // present). Requires 1959 passed / 0 failed, parsed from the
    // real `Total:` summary line.
    const regression = b.addSystemCommand(&.{python});
    regression.addFileArg(b.path("tools/build/check_regression.py"));
    regression.addFileArg(b.path("test/runtests.py"));
    regression.addFileArg(installed_ch);
    regression.step.dependOn(b.getInstallStep());
    regression.stdio = .inherit;
    const regression_top = b.step("check-regression", "Run the regression suite against the zig-built ch, require 1959 passed / 0 failed (not part of the default build)");
    regression_top.dependOn(&regression.step);

    // (3) Shared embedding test: test_native.sh with the zig-built ch
    // (legacy method, test/CMakeLists.txt:48-55), run in a temp copy
    // of test/native-tests so the tree is never written to.
    const native = b.addSystemCommand(&.{python});
    native.addFileArg(b.path("tools/build/check_native.py"));
    native.addFileArg(b.path("test/native-tests/test_native.sh"));
    native.addFileArg(installed_ch);
    native.addArg(cc);
    native.addArg(cxx);
    native.step.dependOn(b.getInstallStep());
    native.stdio = .inherit;
    const native_top = b.step("check-native", "Run the shared embedding test against zig artifacts (not part of the default build)");
    native_top.dependOn(&native.step);

    // (4) Smoke/flag output assertions (ch -v, invalid flag, JS snippet).
    const smoke = b.addSystemCommand(&.{python});
    smoke.addFileArg(b.path("tools/build/check_smoke.py"));
    smoke.addFileArg(installed_ch);
    smoke.step.dependOn(b.getInstallStep());
    smoke.stdio = .inherit;
    const smoke_top = b.step("check-smoke", "Assert ch version/flag/snippet output text (not part of the default build)");
    smoke_top.dependOn(&smoke.step);

    // (5) Metadata gates: macOS minimum, executable stacks, staged
    // headers, dylib exports.
    const metadata = b.addSystemCommand(&.{python});
    metadata.addFileArg(b.path("tools/build/check_metadata.py"));
    metadata.addDirectoryArg(.{ .relative = .{ .base = .install_prefix } });
    metadata.step.dependOn(b.getInstallStep());
    metadata.stdio = .inherit;
    const metadata_top = b.step("check-metadata", "Assert artifact metadata: minos, stacks, headers, exports (not part of the default build)");
    metadata_top.dependOn(&metadata.step);

    const check_top = b.step("check", "Full legacy-equivalent verification: regression 1959/0, bytecode, native embedding, smoke, metadata (not part of the default build)");
    check_top.dependOn(&regression.step);
    check_top.dependOn(&verify_bc.step);
    check_top.dependOn(&native.step);
    check_top.dependOn(&smoke.step);
    check_top.dependOn(&metadata.step);

    // Negative controls: each check script's --self-test proves its
    // gates CAN fail (doctored logs rejected, tampered victims fail).
    // Hermetic (no install dependency); run `zig build check-selftest`.
    const selftest_top = b.step("check-selftest", "Negative controls for every check gate (not part of the default build)");
    for ([_][]const u8{ "check_regression.py", "check_native.py", "check_smoke.py", "check_metadata.py" }) |script| {
        const st = b.addSystemCommand(&.{python});
        st.addFileArg(b.path(b.fmt("tools/build/{s}", .{script})));
        st.addArg("--self-test");
        st.stdio = .inherit;
        selftest_top.dependOn(&st.step);
    }
}
