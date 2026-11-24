//-------------------------------------------------------------------------------------------------------
// Copyright (c) ChakraCore Project Contributors. All rights reserved.
// Licensed under the MIT license. See LICENSE.txt file in the project root for full license information.
//-------------------------------------------------------------------------------------------------------
//! Native Windows x86_64 (x86_64-windows-msvc) build graph.
//!
//! Compiles ChakraCore using Zig's bundled Clang-MSVC toolchain (`zig c++`,
//! `zig cc`, `zig rc`, `zig lib`, `u8a_codec.zig`) plus MSVC `ml64.exe` for
//! the 5 core and 2 backend MASM `.asm` files, with OOP-JIT opted out via
//! `-DENABLE_OOP_NATIVE_CODEGEN=0` and ETW opted out via `-DDISABLE_JS_ETW`.
//!
//! Supports:
//!   * -Dprofile=test|debug-static|release-shared
//!   * -Djit=true|false
//!   * -Dicu=icu4c|windows|none (plus -Dicu-include / -Dicu-libdir)
//!   * -Dml64=<path> and -Dwin-sdk-um=<dir>
//!   * -Dallow-unpinned-regression=true|false

const std = @import("std");
const builtin = @import("builtin");
const tables = @import("tables_windows.zig");

pub const profile_target = "x86_64-windows-msvc";

pub const Profile = enum {
    @"test",
    debug_static,
    release_shared,

    pub fn name(p: Profile) []const u8 {
        return switch (p) {
            .@"test" => "test",
            .debug_static => "debug-static",
            .release_shared => "release-shared",
        };
    }
};

pub const IcuMode = enum {
    icu4c,
    windows,
    none,

    pub fn name(m: IcuMode) []const u8 {
        return switch (m) {
            .icu4c => "icu4c",
            .windows => "windows",
            .none => "none",
        };
    }
};

pub const Lang = enum {
    cxx,
    c,
    asm_,
};

pub fn parseProfile(raw: []const u8) ?Profile {
    if (std.mem.eql(u8, raw, "test")) return .@"test";
    if (std.mem.eql(u8, raw, "debug-static")) return .debug_static;
    if (std.mem.eql(u8, raw, "release-shared")) return .release_shared;
    return null;
}

pub fn parseIcuMode(raw: []const u8) ?IcuMode {
    if (std.mem.eql(u8, raw, "icu4c")) return .icu4c;
    if (std.mem.eql(u8, raw, "windows")) return .windows;
    if (std.mem.eql(u8, raw, "none")) return .none;
    return null;
}

pub fn isStaticProfile(p: Profile) bool {
    return p == .debug_static;
}

pub fn isReleaseProfile(p: Profile) bool {
    return p == .release_shared;
}

pub fn expectedOptimize(p: Profile) std.builtin.Optimize {
    return switch (p) {
        .debug_static => .debug,
        .@"test", .release_shared => .fast,
    };
}

pub fn sideNameForConfig(profile: Profile) ?[]const u8 {
    return if (profile == .@"test") null else profile.name();
}

pub fn installPrefixForConfig(arena: std.mem.Allocator, profile: Profile) []const u8 {
    if (profile == .@"test") return "zig-out";
    return std.fmt.allocPrint(arena, "out/{s}", .{profile.name()}) catch @panic("OOM");
}

pub fn supportsWasmGate(profile: Profile, jit: bool) bool {
    return !isReleaseProfile(profile) and jit;
}

pub fn supportsFullJitGate(profile: Profile, jit: bool) bool {
    return !isReleaseProfile(profile) and jit;
}

pub fn supportsPackageGate(profile: Profile) bool {
    return !isStaticProfile(profile);
}

pub fn msRuntimeLibFlag(profile: Profile) []const u8 {
    return if (profile == .debug_static) "-fms-runtime-lib=static_dbg" else "-fms-runtime-lib=static";
}

pub fn shouldCompileGroup(g: *const tables.Group, profile: Profile, jit: bool, icu: IcuMode) bool {
    if (!jit and g == &tables.backend_group) return false;
    if (isReleaseProfile(profile) and g == &tables.wabt_group) return false;
    if (icu == .none and g == &tables.rt_platformagnostic_icu_group) return false;
    return true;
}

pub fn isDynamicThunkAsm(src: []const u8) bool {
    for (tables.dynamic_thunk_asm_files) |f| {
        if (std.mem.eql(u8, src, f)) return true;
    }
    return false;
}

pub fn isDateTimeSuppressTu(src: []const u8) bool {
    return std.mem.eql(u8, src, "lib/Jsrt/JsrtHelper.cpp") or
        std.mem.eql(u8, src, "bin/ChakraCore/ChakraCoreDllFunc.cpp");
}

pub const IcuPaths = struct {
    include: ?[]const u8,
    libdir: ?[]const u8,
    bin_dir: ?[]const u8,
};

fn trimPathTrailingSep(p: []const u8) []const u8 {
    return std.mem.trimEnd(u8, p, "/\\");
}

pub fn joinNativeOrSlash(arena: std.mem.Allocator, base: []const u8, sub: []const u8) []const u8 {
    const clean = trimPathTrailingSep(base);
    return std.fmt.allocPrint(arena, "{s}/{s}", .{ clean, sub }) catch @panic("OOM");
}

pub fn icuAutoCandidates(
    arena: std.mem.Allocator,
    chakra_icu_dir: ?[]const u8,
    vcpkg_root: ?[]const u8,
) []const IcuPaths {
    var list: std.ArrayList(IcuPaths) = .empty;
    if (chakra_icu_dir) |dir| {
        if (dir.len > 0) {
            list.append(arena, .{
                .include = joinNativeOrSlash(arena, dir, "include"),
                .libdir = joinNativeOrSlash(arena, dir, "lib"),
                .bin_dir = joinNativeOrSlash(arena, dir, "bin"),
            }) catch @panic("OOM");
        }
    }
    if (vcpkg_root) |root| {
        if (root.len > 0) {
            const inst = joinNativeOrSlash(arena, root, "installed/x64-windows");
            list.append(arena, .{
                .include = joinNativeOrSlash(arena, inst, "include"),
                .libdir = joinNativeOrSlash(arena, inst, "lib"),
                .bin_dir = joinNativeOrSlash(arena, inst, "bin"),
            }) catch @panic("OOM");
        }
    }
    return list.toOwnedSlice(arena) catch @panic("OOM");
}

pub fn ml64EnvCandidates(
    arena: std.mem.Allocator,
    explicit: ?[]const u8,
    vc_tools_dir: ?[]const u8,
) []const []const u8 {
    var list: std.ArrayList([]const u8) = .empty;
    if (explicit) |p| {
        if (p.len > 0) list.append(arena, p) catch @panic("OOM");
    }
    if (vc_tools_dir) |vc| {
        if (vc.len > 0) {
            list.append(arena, joinNativeOrSlash(arena, vc, "bin/Hostx64/x64/ml64.exe")) catch @panic("OOM");
        }
    }
    return list.toOwnedSlice(arena) catch @panic("OOM");
}

pub fn winSdkUmEnvCandidates(
    arena: std.mem.Allocator,
    explicit: ?[]const u8,
    win_sdk_dir: ?[]const u8,
    win_sdk_ver: ?[]const u8,
    ucrt_sdk_dir: ?[]const u8,
    ucrt_ver: ?[]const u8,
) []const []const u8 {
    var list: std.ArrayList([]const u8) = .empty;
    if (explicit) |p| {
        if (p.len > 0) list.append(arena, trimPathTrailingSep(p)) catch @panic("OOM");
    }
    if (win_sdk_dir) |sdk| {
        if (win_sdk_ver) |ver| {
            const clean_ver = trimPathTrailingSep(ver);
            if (sdk.len > 0 and clean_ver.len > 0) {
                const inc = joinNativeOrSlash(arena, sdk, "Include");
                const ver_dir = joinNativeOrSlash(arena, inc, clean_ver);
                list.append(arena, joinNativeOrSlash(arena, ver_dir, "um")) catch @panic("OOM");
            }
        }
    }
    if (ucrt_sdk_dir) |sdk| {
        if (ucrt_ver) |ver| {
            const clean_ver = trimPathTrailingSep(ver);
            if (sdk.len > 0 and clean_ver.len > 0) {
                const inc = joinNativeOrSlash(arena, sdk, "Include");
                const ver_dir = joinNativeOrSlash(arena, inc, clean_ver);
                list.append(arena, joinNativeOrSlash(arena, ver_dir, "um")) catch @panic("OOM");
            }
        }
    }
    return list.toOwnedSlice(arena) catch @panic("OOM");
}

pub fn buildCompileDefines(
    arena: std.mem.Allocator,
    profile: Profile,
    jit: bool,
    icu: IcuMode,
    has_wabt: bool,
    target_define: ?[]const u8,
) []const []const u8 {
    var out: std.ArrayList([]const u8) = .empty;
    for (tables.engine_base_defines) |d| out.append(arena, d) catch @panic("OOM");
    out.append(arena, "-DUSE_STATIC_RUNTIMELIB") catch @panic("OOM");
    if (has_wabt) out.append(arena, "-DCAN_BUILD_WABT=1") catch @panic("OOM");
    switch (profile) {
        .@"test" => {
            out.append(arena, "-DNDEBUG") catch @panic("OOM");
            out.append(arena, "-DENABLE_DEBUG_CONFIG_OPTIONS=1") catch @panic("OOM");
        },
        .debug_static => {
            for ([_][]const u8{
                "-D_DEBUG",
                "-DDBG=1",
                "-DDEBUG=1",
                "-DDBG_DUMP=1",
                "-DENABLE_DEBUG_CONFIG_OPTIONS=1",
                "-DCHAKRA_STATIC_LIBRARY=1",
            }) |d| out.append(arena, d) catch @panic("OOM");
        },
        .release_shared => {
            out.append(arena, "-DNDEBUG") catch @panic("OOM");
        },
    }
    if (!jit) out.append(arena, "-DDISABLE_JIT=1") catch @panic("OOM");
    switch (icu) {
        .icu4c => {
            for ([_][]const u8{ "-DHAS_ICU=1", "-DINTL_ICU=1", "-DU_DISABLE_RENAMING=1" }) |d|
                out.append(arena, d) catch @panic("OOM");
        },
        .windows => {
            for ([_][]const u8{ "-DHAS_ICU=1", "-DINTL_ICU=1", "-DWINDOWS10_ICU=1", "-DU_DISABLE_RENAMING=1" }) |d|
                out.append(arena, d) catch @panic("OOM");
        },
        .none => {},
    }
    if (target_define) |td| out.append(arena, td) catch @panic("OOM");
    return out.toOwnedSlice(arena) catch @panic("OOM");
}

pub fn buildMasmArgs(
    arena: std.mem.Allocator,
    win_sdk_um: []const u8,
    src: []const u8,
    jit: bool,
) []const []const u8 {
    var out: std.ArrayList([]const u8) = .empty;
    out.appendSlice(arena, &.{ "/nologo", "/c", "/Cx" }) catch @panic("OOM");
    out.append(arena, std.fmt.allocPrint(arena, "/I{s}", .{win_sdk_um}) catch @panic("OOM")) catch @panic("OOM");
    if (jit and isDynamicThunkAsm(src)) {
        out.append(arena, "-D_ENABLE_DYNAMIC_THUNKS=1") catch @panic("OOM");
    }
    return out.toOwnedSlice(arena) catch @panic("OOM");
}

pub fn buildCompilerFlags(
    arena: std.mem.Allocator,
    profile: Profile,
    lang: Lang,
    is_wabt: bool,
    suppress_date_time: bool,
) []const []const u8 {
    var out: std.ArrayList([]const u8) = .empty;
    out.append(arena, msRuntimeLibFlag(profile)) catch @panic("OOM");
    switch (profile) {
        .@"test" => out.appendSlice(arena, &.{ "-O2", "-g" }) catch @panic("OOM"),
        .debug_static => out.appendSlice(arena, &.{ "-O0", "-g" }) catch @panic("OOM"),
        .release_shared => out.append(arena, "-O2") catch @panic("OOM"),
    }
    switch (lang) {
        .cxx => {
            if (is_wabt) {
                out.append(arena, "-w") catch @panic("OOM");
            } else {
                out.append(arena, "-Werror") catch @panic("OOM");
                for (tables.clang_msvc_warnings) |w| out.append(arena, w) catch @panic("OOM");
                if (suppress_date_time) out.append(arena, "-Wno-date-time") catch @panic("OOM");
            }
            out.appendSlice(arena, &.{
                "-std=c++17",
                "-msse4.2",
                "-fno-omit-frame-pointer",
                "-fdelayed-template-parsing",
                "-fms-extensions",
                "-fms-compatibility",
                "-fwrapv",
            }) catch @panic("OOM");
        },
        .c => {
            out.appendSlice(arena, &.{ "-std=c11", "-w" }) catch @panic("OOM");
        },
        .asm_ => {},
    }
    return out.toOwnedSlice(arena) catch @panic("OOM");
}

pub fn renderWabtConfig(arena: std.mem.Allocator, template: []const u8) []const u8 {
    const values = [_]struct { name: []const u8, val: []const u8 }{
        .{ .name = "HAVE_ALLOCA_H", .val = "0" },
        .{ .name = "HAVE_UNISTD_H", .val = "0" },
        .{ .name = "HAVE_SNPRINTF", .val = "1" },
        .{ .name = "HAVE_SYSCONF", .val = "0" },
        .{ .name = "HAVE_SSIZE_T", .val = "0" },
        .{ .name = "HAVE_STRCASECMP", .val = "0" },
        .{ .name = "HAVE_WIN32_VT100", .val = "0" },
        .{ .name = "COMPILER_IS_CLANG", .val = "0" },
        .{ .name = "COMPILER_IS_GNU", .val = "0" },
        .{ .name = "COMPILER_IS_MSVC", .val = "1" },
        .{ .name = "WITH_EXCEPTIONS", .val = "0" },
    };
    var out: std.ArrayList(u8) = .empty;
    var it = std.mem.splitScalar(u8, template, '\n');
    var n_sub: usize = 0;
    var first = true;
    while (it.next()) |raw_line| {
        const line = std.mem.trimEnd(u8, raw_line, "\r");
        if (!first) out.append(arena, '\n') catch @panic("OOM");
        first = false;
        if (std.mem.startsWith(u8, line, "#cmakedefine01 ")) {
            const sym = line["#cmakedefine01 ".len..];
            var found = false;
            for (values) |v| {
                if (std.mem.eql(u8, sym, v.name)) {
                    out.print(arena, "#define {s} {s}", .{ sym, v.val }) catch @panic("OOM");
                    found = true;
                    n_sub += 1;
                }
            }
            if (!found) fail("wabt config template drift: unknown #cmakedefine01 {s}", .{sym});
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

fn fail(comptime fmt: []const u8, args: anytype) noreturn {
    std.debug.print("build error: " ++ fmt ++ "\n", args);
    std.process.exit(1);
}

fn fileExists(io: std.Io, path: []const u8) bool {
    std.Io.Dir.accessAbsolute(io, path, .{}) catch return false;
    return true;
}

fn resolveIcu(b: *std.Build, mode: IcuMode) IcuPaths {
    if (mode != .icu4c) {
        return .{ .include = null, .libdir = null, .bin_dir = null };
    }
    const arena = b.graph.arena;
    const io = b.graph.io;
    const inc_opt = b.option([]const u8, "icu-include", "ICU4C include dir containing unicode/uchar.h (-Dicu=icu4c)");
    const lib_opt = b.option([]const u8, "icu-libdir", "ICU4C lib dir containing icuuc.lib and icuin.lib (-Dicu=icu4c)");

    if (inc_opt != null or lib_opt != null) {
        const inc = inc_opt orelse fail("-Dicu-libdir requires -Dicu-include when overriding ICU paths", .{});
        const lib = lib_opt orelse fail("-Dicu-include requires -Dicu-libdir when overriding ICU paths", .{});
        validateIcu4cDir(arena, io, inc, lib);
        const bin_cand = std.fmt.allocPrint(arena, "{s}/../bin", .{trimPathTrailingSep(lib)}) catch @panic("OOM");
        return .{ .include = inc, .libdir = lib, .bin_dir = if (fileExists(io, bin_cand)) bin_cand else null };
    }

    const candidates = icuAutoCandidates(
        arena,
        b.graph.environ_map.get("CHAKRA_ICU_DIR"),
        b.graph.environ_map.get("VCPKG_ROOT"),
    );
    for (candidates) |cand| {
        const inc = cand.include orelse continue;
        const lib = cand.libdir orelse continue;
        const uchar = std.fmt.allocPrint(arena, "{s}/unicode/uchar.h", .{inc}) catch @panic("OOM");
        const uc_lib = std.fmt.allocPrint(arena, "{s}/icuuc.lib", .{lib}) catch @panic("OOM");
        const in_lib = std.fmt.allocPrint(arena, "{s}/icuin.lib", .{lib}) catch @panic("OOM");
        if (fileExists(io, uchar) and fileExists(io, uc_lib) and fileExists(io, in_lib)) {
            const bin_dir = if (cand.bin_dir) |bd| (if (fileExists(io, bd)) bd else null) else null;
            return .{ .include = inc, .libdir = lib, .bin_dir = bin_dir };
        }
    }
    fail("cannot find ICU4C headers/libs for -Dicu=icu4c: pass -Dicu-include=<dir> -Dicu-libdir=<dir>, set CHAKRA_ICU_DIR or VCPKG_ROOT (installed/x64-windows), or use -Dicu=windows|none", .{});
}

fn validateIcu4cDir(arena: std.mem.Allocator, io: std.Io, inc: []const u8, lib: []const u8) void {
    const uchar = std.fmt.allocPrint(arena, "{s}/unicode/uchar.h", .{trimPathTrailingSep(inc)}) catch @panic("OOM");
    if (!fileExists(io, uchar)) fail("Cannot find ICU4C header {s}", .{uchar});
    for ([_][]const u8{ "icuuc.lib", "icuin.lib" }) |name| {
        const p = std.fmt.allocPrint(arena, "{s}/{s}", .{ trimPathTrailingSep(lib), name }) catch @panic("OOM");
        if (!fileExists(io, p)) fail("Cannot find ICU4C library {s}", .{p});
    }
}

fn resolveMl64(b: *std.Build) []const u8 {
    const arena = b.graph.arena;
    const io = b.graph.io;
    const explicit = b.option([]const u8, "ml64", "Path to MASM ml64.exe (default: auto-discovered from VCToolsInstallDir, PATH, or vswhere.exe)");
    if (explicit) |p| {
        if (!fileExists(io, p)) fail("specified -Dml64={s} does not exist", .{p});
        return p;
    }

    const env_cands = ml64EnvCandidates(arena, null, b.graph.environ_map.get("VCToolsInstallDir"));
    for (env_cands) |cand| {
        if (fileExists(io, cand)) return cand;
    }

    if (b.findProgram(.{ .names = &.{ "ml64.exe", "ml64" } })) |p| return p;

    const pf86 = b.graph.environ_map.get("ProgramFiles(x86)");
    const vswhere: ?[]const u8 = if (pf86) |root| blk: {
        const p = joinNativeOrSlash(arena, root, "Microsoft Visual Studio/Installer/vswhere.exe");
        break :blk if (fileExists(io, p)) p else null;
    } else b.findProgram(.{ .names = &.{ "vswhere.exe", "vswhere" } });

    if (vswhere) |vw| {
        if (std.process.run(arena, io, .{
            .argv = &.{
                vw,
                "-latest",
                "-products",
                "*",
                "-requires",
                "Microsoft.VisualStudio.Component.VC.Tools.x86.x64",
                "-find",
                "VC\\Tools\\MSVC\\**\\bin\\Hostx64\\x64\\ml64.exe",
            },
        })) |res| {
            if (res.term == .exited and res.term.exited == 0) {
                var lines = std.mem.splitScalar(u8, res.stdout, '\n');
                while (lines.next()) |raw| {
                    const line = std.mem.trim(u8, raw, " \t\r");
                    if (line.len > 0 and fileExists(io, line)) return line;
                }
            }
        } else |_| {}
    }

    fail("cannot find ml64.exe for MASM .asm TUs: pass -Dml64=<path> or run from an MSVC Developer Command Prompt (or install VS Build Tools x64)", .{});
}

fn resolveWinSdkUm(b: *std.Build) []const u8 {
    const arena = b.graph.arena;
    const io = b.graph.io;
    const explicit = b.option([]const u8, "win-sdk-um", "Windows SDK um include directory containing ksamd64.inc");
    if (explicit) |p| {
        const clean = trimPathTrailingSep(p);
        const ksamd = joinNativeOrSlash(arena, clean, "ksamd64.inc");
        if (!fileExists(io, ksamd)) fail("specified -Dwin-sdk-um={s} is missing ksamd64.inc", .{clean});
        return clean;
    }

    const env_cands = winSdkUmEnvCandidates(
        arena,
        null,
        b.graph.environ_map.get("WindowsSdkDir"),
        b.graph.environ_map.get("WindowsSDKVersion"),
        b.graph.environ_map.get("UniversalCRTSdkDir"),
        b.graph.environ_map.get("UCRTVersion"),
    );
    for (env_cands) |cand| {
        if (fileExists(io, joinNativeOrSlash(arena, cand, "ksamd64.inc"))) return cand;
        // SDK 26100+ moved ksamd64.inc from um\ to shared\.
        if (std.fs.path.dirname(cand)) |parent| {
            const shared = joinNativeOrSlash(arena, parent, "shared");
            if (fileExists(io, joinNativeOrSlash(arena, shared, "ksamd64.inc"))) return shared;
        }
    }

    if (comptime builtin.os.tag == .windows) {
        if (std.zig.WindowsSdk.find(arena, io, .x86_64, &b.graph.environ_map)) |sdk| {
            if (sdk.windows10sdk) |w10| {
                const inc = joinNativeOrSlash(arena, w10.path, "Include");
                const ver_dir = joinNativeOrSlash(arena, inc, trimPathTrailingSep(w10.version));
                const um = joinNativeOrSlash(arena, ver_dir, "um");
                if (fileExists(io, joinNativeOrSlash(arena, um, "ksamd64.inc"))) return um;
                // SDK 26100+ moved ksamd64.inc from um\ to shared\.
                const shared = joinNativeOrSlash(arena, ver_dir, "shared");
                if (fileExists(io, joinNativeOrSlash(arena, shared, "ksamd64.inc"))) return shared;
            }
        } else |_| {}
    }

    fail("cannot find Windows SDK um include directory containing ksamd64.inc: pass -Dwin-sdk-um=<dir> or set WindowsSdkDir/WindowsSDKVersion", .{});
}

fn windowsSdkLibUmHasIcuLib(arena: std.mem.Allocator, io: std.Io, win_sdk_um: []const u8) bool {
    // Expect <root>/Include/<ver>/um -> check <root>/Lib/<ver>/um/x64/icu.lib.
    const um_parent = std.fs.path.dirname(win_sdk_um) orelse return true;
    const ver_name = std.fs.path.basename(um_parent);
    const inc_dir = std.fs.path.dirname(um_parent) orelse return true;
    const sdk_root = std.fs.path.dirname(inc_dir) orelse return true;
    const lib_path = std.fmt.allocPrint(arena, "{s}/Lib/{s}/um/x64/icu.lib", .{ trimPathTrailingSep(sdk_root), ver_name }) catch @panic("OOM");
    return fileExists(io, lib_path);
}

const Ctx = struct {
    b: *std.Build,
    zig_exe: []const u8,
    ml64_exe: []const u8,
    win_sdk_um: []const u8,
    profile: Profile,
    jit: bool,
    icu: IcuMode,
    icu_paths: IcuPaths,
    has_win10_icu_lib: bool,
    wabt_dir: ?std.Build.LazyPath,
    wabt_h: ?std.Build.LazyPath,
    gen_dir: std.Build.LazyPath,
    dbg_header: std.Build.LazyPath,
};

fn addIcuLinkFlags(ctx: *const Ctx, cmd: *std.Build.Step.Run) void {
    switch (ctx.icu) {
        .icu4c => {
            if (ctx.icu_paths.libdir) |ld| {
                cmd.addArg(ctx.b.fmt("-L{s}", .{ld}));
            }
            cmd.addArgs(&.{ "-licuuc", "-licuin" });
        },
        .windows => {
            if (ctx.has_win10_icu_lib) {
                cmd.addArg("-licu");
            } else {
                cmd.addArgs(&.{ "-licuuc", "-licuin" });
            }
        },
        .none => {},
    }
}

fn addWinLibs(cmd: *std.Build.Step.Run, libs: []const []const u8) void {
    for (libs) |lib| {
        const stem = if (std.mem.endsWith(u8, lib, ".lib")) lib[0 .. lib.len - ".lib".len] else lib;
        cmd.addArg(std.fmt.allocPrint(cmd.step.owner.graph.arena, "-l{s}", .{stem}) catch @panic("OOM"));
    }
}

fn addIcuRuntimePath(ctx: *const Ctx, cmd: *std.Build.Step.Run, prefix_bin_dir: ?[]const u8) void {
    const arena = ctx.b.graph.arena;
    var parts: std.ArrayList([]const u8) = .empty;
    if (prefix_bin_dir) |pb| parts.append(arena, pb) catch @panic("OOM");
    if (ctx.icu == .icu4c) {
        if (ctx.icu_paths.bin_dir) |bd| parts.append(arena, bd) catch @panic("OOM");
    }
    if (parts.items.len == 0) return;
    if (ctx.b.graph.environ_map.get("PATH")) |cur| {
        if (cur.len > 0) parts.append(arena, cur) catch @panic("OOM");
    }
    const sep = if (builtin.os.tag == .windows) ";" else ":";
    const joined = std.mem.join(arena, sep, parts.items) catch @panic("OOM");
    cmd.setEnvironmentVariable("PATH", joined);
}

fn compileTu(ctx: *const Ctx, g: *const tables.Group, src: []const u8, lang: Lang, number: u32) std.Build.LazyPath {
    const b = ctx.b;
    const arena = b.graph.arena;
    std.debug.assert(number < 10000);
    var digits: [4]u8 = .{ '0', '0', '0', '0' };
    var n = number;
    var i: usize = 4;
    while (i > 0) {
        i -= 1;
        digits[i] = '0' + @as(u8, @intCast(n % 10));
        n /= 10;
    }
    const base = std.fs.path.basename(src);

    if (lang == .asm_) {
        const run = b.addSystemCommand(&.{ctx.ml64_exe});
        for (buildMasmArgs(arena, ctx.win_sdk_um, src, ctx.jit)) |a| run.addArg(a);
        const out = run.addPrefixedOutputFileArg("/Fo", b.fmt("{s}-{s}.obj", .{ digits, base }));
        run.addFileArg(b.path(src));
        return out;
    }

    const run = b.addSystemCommand(&.{ ctx.zig_exe, if (lang == .cxx) "c++" else "cc" });
    run.addArg("-target");
    run.addArg(profile_target);
    // Zig's bundled clang emits /DEFAULTLIB:libc++.lib into every C++ object
    // unless told not to link its bundled libc++ at driver level. Without
    // this, every object demands libc++.lib at link time (which does not
    // exist for the windows-msvc target) even though all objects target the
    // MSVC CRT ABI (-fms-runtime-lib=static). See doc/b6-windows-fixes.md #15.
    if (lang == .cxx) run.addArg("-nostdlib++");

    for (buildCompileDefines(arena, ctx.profile, ctx.jit, ctx.icu, !isReleaseProfile(ctx.profile), g.target_define)) |d| {
        run.addArg(d);
    }

    for (tables.global_includes) |dir| run.addArg(b.fmt("-I{s}", .{dir}));
    // SDK 26100+ ships Windows.Globalization.h (WinRT) under winrt\, which
    // zig c++ does not add to the MSVC include path; add it when present.
    if (std.fs.path.dirname(ctx.win_sdk_um)) |ver_dir| {
        const winrt = joinNativeOrSlash(arena, ver_dir, "winrt");
        if (fileExists(b.graph.io, winrt)) {
            run.addArg(b.fmt("-I{s}", .{winrt}));
        }
    }
    if (g == &tables.wabt_group or g == &tables.rt_library_group) {
        if (ctx.wabt_dir) |wd| run.addPrefixedDirectoryArg("-I", wd);
    }
    for (g.extra_includes) |dir| run.addArg(b.fmt("-I{s}", .{dir}));
    if (g == &tables.ch_group) {
        run.addPrefixedDirectoryArg("-I", ctx.gen_dir);
        if (isStaticProfile(ctx.profile)) {
            run.addArg("-Ilib/Backend");
            run.addArg("-Ilib/Runtime/ByteCode");
        }
    }
    if (ctx.icu == .icu4c) {
        if (ctx.icu_paths.include) |inc| {
            run.addArg("-isystem");
            run.addArg(inc);
        }
    }

    for (buildCompilerFlags(arena, ctx.profile, lang, g == &tables.wabt_group, isDateTimeSuppressTu(src))) |f| {
        run.addArg(f);
    }

    run.addArg("-MD");
    run.addArg("-MF");
    _ = run.addDepFileOutputArg(b.fmt("{s}-{s}.d", .{ digits, base }));
    run.addArg("-o");
    const out = run.addOutputFileArg(b.fmt("{s}-{s}.obj", .{ digits, base }));
    run.addArg("-c");
    run.addFileArg(b.path(src));

    if (g == &tables.ch_group) run.addFileInput(ctx.dbg_header);
    if (g == &tables.wabt_group or g == &tables.rt_library_group) {
        if (ctx.wabt_h) |wh| run.addFileInput(wh);
    }
    return out;
}

fn archiveWithLib(
    b: *std.Build,
    zig_exe: []const u8,
    archive_name: []const u8,
    items: []const std.Build.LazyPath,
) std.Build.LazyPath {
    const chunk_cap: usize = 50;
    if (items.len <= chunk_cap) {
        const ar = b.addSystemCommand(&.{zig_exe});
        ar.addArg("lib");
        const out = ar.addPrefixedOutputFileArg("/OUT:", archive_name);
        for (items) |item| ar.addFileArg(item);
        return out;
    }

    const arena = b.graph.arena;
    var chunks: std.ArrayList(std.Build.LazyPath) = .empty;
    var start: usize = 0;
    var idx: usize = 0;
    while (start < items.len) : (idx += 1) {
        const end = @min(start + chunk_cap, items.len);
        const chunk_ar = b.addSystemCommand(&.{zig_exe});
        chunk_ar.addArg("lib");
        const chunk_out = chunk_ar.addPrefixedOutputFileArg("/OUT:", b.fmt("part-{d}-{s}", .{ idx, archive_name }));
        for (items[start..end]) |item| chunk_ar.addFileArg(item);
        chunks.append(arena, chunk_out) catch @panic("OOM");
        start = end;
    }

    const merge = b.addSystemCommand(&.{zig_exe});
    merge.addArg("lib");
    const final_out = merge.addPrefixedOutputFileArg("/OUT:", archive_name);
    for (chunks.items) |chunk| merge.addFileArg(chunk);
    return final_out;
}

fn registerFailStep(b: *std.Build, python: []const u8, name: []const u8, desc: []const u8, msg: []const u8) void {
    const cmd = b.addSystemCommand(&.{ python, "-c", "import sys; sys.exit(sys.argv[1])", msg });
    cmd.stdio = .inherit;
    const step = b.step(name, desc);
    step.dependOn(&cmd.step);
}

fn addBinArg(
    b: *std.Build,
    cmd: *std.Build.Step.Run,
    side_name: ?[]const u8,
    prefix_dir: []const u8,
    sub_path: []const u8,
    bin_art: std.Build.LazyPath,
    dep_step: *std.Build.Step,
    prefixed_flag: ?[]const u8,
) void {
    if (side_name == null) {
        const installed_art: std.Build.LazyPath = .{ .relative = .{ .base = .install_prefix, .sub_path = sub_path } };
        if (prefixed_flag) |flag| {
            cmd.addPrefixedFileArg(flag, installed_art);
        } else {
            cmd.addFileArg(installed_art);
        }
    } else {
        const arg_val = b.fmt("{s}/{s}", .{ prefix_dir, sub_path });
        if (prefixed_flag) |flag| {
            cmd.addArg(b.fmt("{s}{s}", .{ flag, arg_val }));
        } else {
            cmd.addArg(arg_val);
        }
        cmd.addFileInput(bin_art);
    }
    cmd.step.dependOn(dep_step);
}

fn addSideCopy(b: *std.Build, python: []const u8, dest_rel: []const u8, src: std.Build.LazyPath, prereq: *std.Build.Step) *std.Build.Step.Run {
    // Copy into out/<profile>/ atomically without disturbing install_profile.py's
    // .manifest (so base rebuilds do not purge ch-nodeapi outputs).
    const cp = b.addSystemCommand(&.{
        python,
        "-c",
        "import os, shutil, sys, tempfile; src, dst = sys.argv[1], sys.argv[2]; d = os.path.dirname(os.path.abspath(dst)); os.makedirs(d, exist_ok=True); fd, tmp = tempfile.mkstemp(dir=d, prefix='.tmp.'); os.close(fd); shutil.copy2(src, tmp); (os.chmod(dst, 0o666) if (os.name == 'nt' and os.path.isfile(dst)) else None); os.replace(tmp, dst)",
    });
    cp.addFileArg(src);
    cp.addArg(dest_rel);
    cp.step.dependOn(prereq);
    cp.has_side_effects = true;
    return cp;
}

fn gateProfile(b: *std.Build) Profile {
    const raw = b.option([]const u8, "profile", "Build profile: test (default), debug-static, release-shared") orelse "test";
    return parseProfile(raw) orelse
        fail("unsupported -Dprofile={s} on Windows: expected test, debug-static, or release-shared", .{raw});
}

fn gateIcu(b: *std.Build) IcuMode {
    const raw = b.option([]const u8, "icu", "ICU backend: icu4c (default), windows, or none") orelse "icu4c";
    return parseIcuMode(raw) orelse
        fail("unsupported -Dicu={s} on Windows: expected icu4c, windows, or none", .{raw});
}

fn gateOptimize(b: *std.Build, profile: Profile) void {
    const want = expectedOptimize(profile);
    const choice = b.option(std.builtin.Optimize, "optimize", "Optimization (pinned per profile: fast for test/release-shared, debug for debug-static)") orelse want;
    if (choice != want)
        fail("unsupported -Doptimize={t} for -Dprofile={s} on Windows: requires {t}", .{ choice, profile.name(), want });
}

pub fn build(b: *std.Build) void {
    const profile = gateProfile(b);
    gateOptimize(b, profile);
    const jit = b.option(bool, "jit", "Enable JIT backend (default: true; false defines -DDISABLE_JIT=1)") orelse true;
    const allow_unpinned_regression = b.option(bool, "allow-unpinned-regression", "Pass --allow-unpinned to check_regression_windows.py during B6 bootstrap") orelse false;
    const icu = gateIcu(b);
    const icu_paths = resolveIcu(b, icu);
    const ml64_exe = resolveMl64(b);
    const win_sdk_um = resolveWinSdkUm(b);
    const python = b.findProgram(.{ .names = &.{ "python", "python3" } }) orelse
        fail("cannot find python or python3 on PATH; needed for build/check tooling", .{});

    const arena = b.graph.arena;
    const is_static = isStaticProfile(profile);
    const is_release = isReleaseProfile(profile);
    const side_name = sideNameForConfig(profile);
    const prefix_dir = installPrefixForConfig(arena, profile);
    const has_win10_icu_lib = windowsSdkLibUmHasIcuLib(arena, b.graph.io, win_sdk_um);

    const target = b.standardTargetOptions(.{ .default_target = .{
        .cpu_arch = .x86_64,
        .os_tag = .windows,
        .abi = .msvc,
    } });

    std.debug.print("windows profile={s}: target={s} jit={} icu={s} prefix={s} ml64={s} sdk-um={s}\n", .{
        profile.name(), profile_target, jit, icu.name(), prefix_dir, ml64_exe, win_sdk_um,
    });

    // Generated wabt config.h (skipped in release-shared where WABT is disabled).
    const wabt_res: struct { dir: ?std.Build.LazyPath, file: ?std.Build.LazyPath } = if (is_release) .{ .dir = null, .file = null } else blk: {
        const root = b.root.root_dir.path orelse
            fail("cannot resolve build root path for wabt config.h.in", .{});
        const wabt_in_path = joinNativeOrSlash(arena, root, "lib/wabt/src/config.h.in");
        const wabt_in_file = std.Io.Dir.openFileAbsolute(b.graph.io, wabt_in_path, .{}) catch
            fail("cannot read {s}", .{wabt_in_path});
        defer wabt_in_file.close(b.graph.io);
        var wabt_in_r = wabt_in_file.readerStreaming(b.graph.io, &.{});
        const wabt_template = wabt_in_r.interface.allocRemaining(arena, .limited(1 << 20)) catch
            fail("cannot read {s}", .{wabt_in_path});
        const wabt_bytes = renderWabtConfig(arena, wabt_template);
        const wabt_wf = b.addWriteFiles();
        const wh = wabt_wf.add("config.h", wabt_bytes);
        break :blk .{ .dir = wabt_wf.getDirectory(), .file = wh };
    };

    // Generate DbgController.js.encoded without requiring cscript.exe.
    const dbg_gen = b.addSystemCommand(&.{
        python,
        "-c",
        "import sys; data = open(sys.argv[1], 'rb').read(); " ++
            "rows = [', '.join(f'0x{b:02x}' for b in data[i:i+16]) + ',' for i in range(0, len(data), 16)]; " ++
            "open(sys.argv[2], 'w', encoding='ascii', newline='\\n').write('\\n'.join(rows) + '\\n')",
    });
    dbg_gen.addFileArg(b.path("bin/ch/DbgController.js"));
    const dbg_header = dbg_gen.addOutputFileArg("DbgController.js.encoded");
    const gen_dir = dbg_header.dirname();

    // Generate jserr.obj COFF RT_STRING resource natively via zig cc + zig rc.
    const jserr_pp = b.addSystemCommand(&.{ b.graph.zig_exe, "cc" });
    jserr_pp.addArgs(&.{ "-E", "-P", "-Ilib/Parser", "-x", "c" });
    jserr_pp.addFileArg(b.path("lib/Parser/jserr.gen"));
    jserr_pp.addArg("-o");
    const jserr_rc = jserr_pp.addOutputFileArg("jserr.rc");

    const jserr_res_cmd = b.addSystemCommand(&.{ b.graph.zig_exe, "rc" });
    jserr_res_cmd.addArgs(&.{ "/:auto-includes", "none", "/:output-format", "coff", "/:target", "x86_64" });
    const jserr_obj = jserr_res_cmd.addPrefixedOutputFileArg("/fo", "jserr.obj");
    jserr_res_cmd.addArg("--");
    jserr_res_cmd.addFileArg(jserr_rc);

    // Preprocess bin/ChakraCore/ChakraCore.def with ENABLE_OOP_NATIVE_CODEGEN=0.
    const def_pp = b.addSystemCommand(&.{ b.graph.zig_exe, "cc" });
    def_pp.addArgs(&.{ "-E", "-P", "-x", "c", "-Ilib/Jsrt", "-DENABLE_OOP_NATIVE_CODEGEN=0" });
    def_pp.addFileArg(b.path("bin/ChakraCore/ChakraCore.def"));
    def_pp.addArg("-o");
    const chakracore_def = def_pp.addOutputFileArg("ChakraCore.def");

    const ctx = Ctx{
        .b = b,
        .zig_exe = b.graph.zig_exe,
        .ml64_exe = ml64_exe,
        .win_sdk_um = win_sdk_um,
        .profile = profile,
        .jit = jit,
        .icu = icu,
        .icu_paths = icu_paths,
        .has_win10_icu_lib = has_win10_icu_lib,
        .wabt_dir = wabt_res.dir,
        .wabt_h = wabt_res.file,
        .gen_dir = gen_dir,
        .dbg_header = dbg_header,
    };

    var num: u32 = 0;
    var group_idx: usize = 0;
    var engine_group_libs: std.ArrayList(std.Build.LazyPath) = .empty;
    var gcstress_dep_libs: std.ArrayList(std.Build.LazyPath) = .empty;
    var heap_op_obj_opt: ?std.Build.LazyPath = null;
    var common_codex_lib_opt: ?std.Build.LazyPath = null;
    var static_engine_items: std.ArrayList(std.Build.LazyPath) = .empty;

    for (tables.engine_groups) |g| {
        if (!shouldCompileGroup(g, profile, jit, icu)) continue;
        defer group_idx += 1;
        var grp_objs_for_dll: std.ArrayList(std.Build.LazyPath) = .empty;
        var grp_objs_all: std.ArrayList(std.Build.LazyPath) = .empty;

        for (g.cxx) |src| {
            const obj = compileTu(&ctx, g, src, .cxx, num);
            num += 1;
            grp_objs_all.append(arena, obj) catch @panic("OOM");
            if (std.mem.eql(u8, src, tables.heap_allocator_operators_tu)) {
                heap_op_obj_opt = obj;
            } else {
                grp_objs_for_dll.append(arena, obj) catch @panic("OOM");
            }
        }
        for (g.c) |src| {
            const obj = compileTu(&ctx, g, src, .c, num);
            num += 1;
            grp_objs_all.append(arena, obj) catch @panic("OOM");
            grp_objs_for_dll.append(arena, obj) catch @panic("OOM");
        }
        for (g.asm_files) |src| {
            const obj = compileTu(&ctx, g, src, .asm_, num);
            num += 1;
            grp_objs_all.append(arena, obj) catch @panic("OOM");
            grp_objs_for_dll.append(arena, obj) catch @panic("OOM");
        }

        const lib_name = b.fmt("grp-{d}.lib", .{group_idx});
        const grp_lib_all = archiveWithLib(b, b.graph.zig_exe, lib_name, grp_objs_all.items);
        if (g == &tables.common_codex_group) {
            common_codex_lib_opt = grp_lib_all;
        }
        if (is_static) {
            static_engine_items.append(arena, grp_lib_all) catch @panic("OOM");
        } else {
            const dll_lib = if (grp_objs_for_dll.items.len == grp_objs_all.items.len)
                grp_lib_all
            else
                archiveWithLib(b, b.graph.zig_exe, b.fmt("dll-{s}", .{lib_name}), grp_objs_for_dll.items);
            engine_group_libs.append(arena, dll_lib) catch @panic("OOM");
        }

        for (tables.gcstress_dep_groups) |gd| {
            if (gd == g) {
                gcstress_dep_libs.append(arena, grp_lib_all) catch @panic("OOM");
                break;
            }
        }
    }

    const heap_op_obj = heap_op_obj_opt orelse fail("missing {s} object", .{tables.heap_allocator_operators_tu});
    const common_codex_lib = common_codex_lib_opt orelse fail("missing common_codex_group archive", .{});

    // Z1 leaf Zig SIMD codec compiled directly to x86_64-windows-msvc COFF.
    const u8a_codec_obj = b.addObject(.{
        .name = "u8a_codec",
        .root_module = b.createModule(.{
            .root_source_file = b.path(tables.zig_codec_source),
            .target = target,
            .optimize = if (is_static) .debug else .fast,
            .link_libc = false,
            .link_libcpp = false,
        }),
    });

    // Compile ch, GCStress, and DLL-only / static-extra TUs.
    var ch_objs: std.ArrayList(std.Build.LazyPath) = .empty;
    for (tables.ch_group.cxx) |src| {
        ch_objs.append(arena, compileTu(&ctx, &tables.ch_group, src, .cxx, num)) catch @panic("OOM");
        num += 1;
    }
    if (is_static) {
        for (tables.ch_static_extra_cxx) |src| {
            ch_objs.append(arena, compileTu(&ctx, &tables.ch_group, src, .cxx, num)) catch @panic("OOM");
            num += 1;
        }
    }

    var gcstress_objs: std.ArrayList(std.Build.LazyPath) = .empty;
    for (tables.gcstress_group.cxx) |src| {
        gcstress_objs.append(arena, compileTu(&ctx, &tables.gcstress_group, src, .cxx, num)) catch @panic("OOM");
        num += 1;
    }

    var dll_res: ?struct { dll: std.Build.LazyPath, implib: std.Build.LazyPath, link: *std.Build.Step.Run } = null;
    var static_chakracore_lib: ?std.Build.LazyPath = null;

    if (is_static) {
        static_engine_items.append(arena, u8a_codec_obj.getEmittedBin()) catch @panic("OOM");
        static_chakracore_lib = archiveWithLib(b, b.graph.zig_exe, "ChakraCore.lib", static_engine_items.items);
    } else {
        var dll_objs: std.ArrayList(std.Build.LazyPath) = .empty;
        for (tables.dll_group.cxx) |src| {
            dll_objs.append(arena, compileTu(&ctx, &tables.dll_group, src, .cxx, num)) catch @panic("OOM");
            num += 1;
        }

        const dll_link = b.addSystemCommand(&.{ b.graph.zig_exe, "c++", "-nostdlib++" });
        dll_link.addArgs(&.{ "-target", profile_target, "-shared", msRuntimeLibFlag(profile) });
        if (profile == .@"test") dll_link.addArg("-g");
        // ChakraCore.vcxproj: HeapAllocatorOperators.obj must appear first on link line.
        dll_link.addFileArg(heap_op_obj);
        dll_link.addFileArg(jserr_obj);
        for (dll_objs.items) |o| dll_link.addFileArg(o);
        dll_link.addFileArg(u8a_codec_obj.getEmittedBin());
        dll_link.addFileArg(chakracore_def);
        const dll_out = dll_link.addPrefixedOutputFileArg("-o", "ChakraCore.dll");
        const implib_out = dll_link.addOutputFileArg2("ChakraCore.lib", .{ .prefix = "-Wl,--out-implib=" });
        dll_link.addArg("-Wl,--whole-archive");
        for (engine_group_libs.items) |gl| dll_link.addFileArg(gl);
        dll_link.addArg("-Wl,--no-whole-archive");
        addWinLibs(dll_link, tables.chakracore_win_libs);
        addIcuLinkFlags(&ctx, dll_link);
        dll_res = .{ .dll = dll_out, .implib = implib_out, .link = dll_link };
    }

    // Link ch.exe.
    const ch_link = b.addSystemCommand(&.{ b.graph.zig_exe, "c++", "-nostdlib++" });
    ch_link.addArgs(&.{ "-target", profile_target, msRuntimeLibFlag(profile) });
    if (profile != .release_shared) ch_link.addArg("-g");
    for (ch_objs.items) |o| ch_link.addFileArg(o);
    if (is_static) {
        ch_link.addFileArg(jserr_obj);
        ch_link.addFileArg(static_chakracore_lib.?);
        addWinLibs(ch_link, tables.ch_win_libs);
        addIcuLinkFlags(&ctx, ch_link);
    } else {
        ch_link.addFileArg(b.path("bin/ch/ch.def"));
        ch_link.addFileArg(dll_res.?.implib);
        ch_link.addFileArg(common_codex_lib);
        addWinLibs(ch_link, tables.ch_win_libs);
        addIcuLinkFlags(&ctx, ch_link);
    }
    const ch_bin = ch_link.addPrefixedOutputFileArg("-o", "ch.exe");
    if (dll_res) |dr| ch_link.step.dependOn(&dr.link.step);

    // Link GCStress.exe (mirrors bin/GCStress/GCStress.vcxproj).
    const gs_link = b.addSystemCommand(&.{ b.graph.zig_exe, "c++", "-nostdlib++" });
    gs_link.addArgs(&.{ "-target", profile_target, msRuntimeLibFlag(profile) });
    if (profile != .release_shared) gs_link.addArg("-g");
    for (gcstress_objs.items) |o| gs_link.addFileArg(o);
    for (gcstress_dep_libs.items) |dl| gs_link.addFileArg(dl);
    addWinLibs(gs_link, tables.gcstress_win_libs);
    addIcuLinkFlags(&ctx, gs_link);
    const gs_bin = gs_link.addPrefixedOutputFileArg("-o", "GCStress.exe");

    // Stage artifacts into zig-out/ (test) or out/<profile>/ (debug-static, release-shared).
    const install = b.getInstallStep();
    const chakracore_lib_out = if (dll_res) |dr| dr.implib else static_chakracore_lib.?;
    const wf = b.addWriteFiles();
    const marker = wf.add(".profile", b.fmt("{s}\n", .{profile.name()}));

    var copy_out: ?*std.Build.Step.Run = null;
    if (side_name == null) {
        install.dependOn(&b.addInstallFile(ch_bin, "bin/ch.exe").step);
        install.dependOn(&b.addInstallFile(gs_bin, "bin/GCStress.exe").step);
        if (dll_res) |dr| {
            install.dependOn(&b.addInstallFile(dr.dll, "bin/ChakraCore.dll").step);
        }
        install.dependOn(&b.addInstallFile(chakracore_lib_out, "lib/ChakraCore.lib").step);
        if (wabt_res.file) |wh| install.dependOn(&b.addInstallFile(wh, "lib/wabt/config.h").step);
        install.dependOn(&b.addInstallFile(b.path("lib/Jsrt/ChakraCore.h"), "include/ChakraCore.h").step);
        install.dependOn(&b.addInstallFile(b.path("lib/Jsrt/ChakraCommon.h"), "include/ChakraCommon.h").step);
        install.dependOn(&b.addInstallFile(b.path("lib/Jsrt/ChakraDebug.h"), "include/ChakraDebug.h").step);
        install.dependOn(&b.addInstallFile(b.path("lib/Common/ChakraCoreVersion.h"), "include/ChakraCoreVersion.h").step);
        install.dependOn(&b.addInstallFile(marker, ".profile").step);
    } else {
        const sname = side_name.?;
        const cp = b.addSystemCommand(&.{python});
        cp.addFileArg(b.path("tools/build/install_profile.py"));
        cp.addArg("--profile");
        cp.addArg(sname);
        cp.addArg("--dest");
        cp.addArg(prefix_dir);
        cp.addPrefixedFileArg("--map=bin/ch.exe=", ch_bin);
        cp.addPrefixedFileArg("--map=bin/GCStress.exe=", gs_bin);
        if (dll_res) |dr| {
            cp.addPrefixedFileArg("--map=bin/ChakraCore.dll=", dr.dll);
        }
        cp.addPrefixedFileArg("--map=lib/ChakraCore.lib=", chakracore_lib_out);
        if (wabt_res.file) |wh| cp.addPrefixedFileArg("--map=lib/wabt/config.h=", wh);
        cp.addPrefixedFileArg("--map=include/ChakraCore.h=", b.path("lib/Jsrt/ChakraCore.h"));
        cp.addPrefixedFileArg("--map=include/ChakraCommon.h=", b.path("lib/Jsrt/ChakraCommon.h"));
        cp.addPrefixedFileArg("--map=include/ChakraDebug.h=", b.path("lib/Jsrt/ChakraDebug.h"));
        cp.addPrefixedFileArg("--map=include/ChakraCoreVersion.h=", b.path("lib/Common/ChakraCoreVersion.h"));
        cp.has_side_effects = true;
        cp.stdio = .inherit;
        install.dependOn(&cp.step);
        copy_out = cp;

        const root = b.root.root_dir.path orelse
            fail("cannot resolve build root path for uninstall", .{});
        const rm = b.addSystemCommand(&.{
            python,
            "-c",
            "import shutil, sys; shutil.rmtree(sys.argv[1], ignore_errors=True); print('removed ' + sys.argv[1] + ' (or already absent)')",
            b.fmt("{s}/{s}", .{ root, prefix_dir }),
        });
        rm.stdio = .inherit;
        rm.has_side_effects = true;
        b.getUninstallStep().dependOn(&rm.step);
    }

    const artifacts_ready: *std.Build.Step = if (copy_out) |cp| &cp.step else install;

    // Node-API provider, check-nodeapi, and ch-nodeapi (shared profiles only).
    var nodeapi_step_opt: ?*std.Build.Step = null;
    var check_nodeapi_step_opt: ?*std.Build.Step = null;
    var nodeapi_host_bin_opt: ?std.Build.LazyPath = null;
    if (dll_res) |dr| {
        const gen_def_cmd = b.addSystemCommand(&.{python});
        gen_def_cmd.addFileArg(b.path("tools/build/gen_nodeapi_exports.py"));
        gen_def_cmd.addArg("--def");
        const nodeapi_def = gen_def_cmd.addOutputFileArg("libChakraNodeApi.def");

        var prov_objs: std.ArrayList(std.Build.LazyPath) = .empty;
        for (tables.nodeapi_provider_tus) |src| {
            const tu = b.addSystemCommand(&.{ b.graph.zig_exe, "c++", "-nostdlib++" });
            tu.addArgs(&.{
                "-target",
                profile_target,
                msRuntimeLibFlag(profile),
                "-std=c++17",
                if (profile == .debug_static) "-O0" else "-O2",
            });
            if (profile != .release_shared) tu.addArg("-g");
            tu.addArgs(&.{
                "-DNAPI_VERSION=10",
                "-DNOMINMAX",
                "-Ilib/Jsrt",
                "-Iinclude/nodeapi",
                "-Ilib/NodeApi",
                "-Wno-date-time",
            });
            if (icu == .icu4c) {
                if (icu_paths.include) |inc| {
                    tu.addArg("-isystem");
                    tu.addArg(inc);
                }
            }
            const base = std.fs.path.basename(src);
            tu.addArg("-MD");
            tu.addArg("-MF");
            _ = tu.addDepFileOutputArg(b.fmt("nodeapi-{s}.d", .{base}));
            tu.addArg("-o");
            const out = tu.addOutputFileArg(b.fmt("nodeapi-{s}.obj", .{base}));
            tu.addArg("-c");
            tu.addFileArg(b.path(src));
            prov_objs.append(arena, out) catch @panic("OOM");
        }

        const prov_link = b.addSystemCommand(&.{ b.graph.zig_exe, "c++", "-nostdlib++" });
        prov_link.addArgs(&.{ "-target", profile_target, "-shared", msRuntimeLibFlag(profile) });
        if (profile == .@"test") prov_link.addArg("-g");
        for (prov_objs.items) |o| prov_link.addFileArg(o);
        prov_link.addFileArg(nodeapi_def);
        prov_link.addFileArg(dr.implib);
        const prov_dll = prov_link.addPrefixedOutputFileArg("-o", "libChakraNodeApi.dll");
        const prov_lib = prov_link.addOutputFileArg2("libChakraNodeApi.lib", .{ .prefix = "-Wl,--out-implib=" });
        addWinLibs(prov_link, tables.nodeapi_win_libs);
        prov_link.step.dependOn(&dr.link.step);

        const prov_step = b.step("libChakraNodeApi", "Build libChakraNodeApi.dll/.lib linked against ChakraCore (shared profiles only)");
        if (side_name == null) {
            const inst_dll = b.addInstallFile(prov_dll, "bin/libChakraNodeApi.dll");
            const inst_lib = b.addInstallFile(prov_lib, "lib/libChakraNodeApi.lib");
            install.dependOn(&inst_dll.step);
            install.dependOn(&inst_lib.step);
            prov_step.dependOn(&inst_dll.step);
            prov_step.dependOn(&inst_lib.step);
            for ([_][]const u8{ "js_native_api.h", "js_native_api_types.h", "node_api.h", "node_api_types.h" }) |hdr| {
                const inst_h = b.addInstallFile(b.path(b.fmt("include/nodeapi/{s}", .{hdr})), b.fmt("include/nodeapi/{s}", .{hdr}));
                install.dependOn(&inst_h.step);
                prov_step.dependOn(&inst_h.step);
            }
        } else {
            const cp_dll = addSideCopy(b, python, b.fmt("{s}/bin/libChakraNodeApi.dll", .{prefix_dir}), prov_dll, artifacts_ready);
            const cp_lib = addSideCopy(b, python, b.fmt("{s}/lib/libChakraNodeApi.lib", .{prefix_dir}), prov_lib, artifacts_ready);
            prov_step.dependOn(&cp_dll.step);
            prov_step.dependOn(&cp_lib.step);
            for ([_][]const u8{ "js_native_api.h", "js_native_api_types.h", "node_api.h", "node_api_types.h" }) |hdr| {
                prov_step.dependOn(&addSideCopy(
                    b,
                    python,
                    b.fmt("{s}/include/nodeapi/{s}", .{ prefix_dir, hdr }),
                    b.path(b.fmt("include/nodeapi/{s}", .{hdr})),
                    artifacts_ready,
                ).step);
            }
        }

        const exports_gen_check = b.addSystemCommand(&.{python});
        exports_gen_check.addFileArg(b.path("tools/build/gen_nodeapi_exports.py"));
        exports_gen_check.addArg("--self-test");
        exports_gen_check.stdio = .inherit;

        const exports_check = b.addSystemCommand(&.{python});
        exports_check.addFileArg(b.path("tools/build/check_nodeapi.py"));
        exports_check.addPrefixedFileArg("--dll=", prov_dll);
        exports_check.addPrefixedFileArg("--exports=", b.path("tools/build/nodeapi_exports.txt"));
        exports_check.addPrefixedFileArg("--inventory=", b.path("doc/node-api-inventory.md"));
        exports_check.step.dependOn(&prov_link.step);
        exports_check.step.dependOn(&exports_gen_check.step);
        exports_check.stdio = .inherit;
        const check_nodeapi_top = b.step("check-nodeapi", "Verify libChakraNodeApi.dll exports exactly tools/build/nodeapi_exports.txt (161 symbols)");
        check_nodeapi_top.dependOn(&exports_gen_check.step);
        check_nodeapi_top.dependOn(&exports_check.step);
        check_nodeapi_step_opt = &exports_check.step;

        const nodeapi_host = b.addSystemCommand(&.{ b.graph.zig_exe, "c++", "-nostdlib++" });
        nodeapi_host.addArgs(&.{
            "-target",
            profile_target,
            msRuntimeLibFlag(profile),
            "-std=c++17",
            "-O2",
            "-g",
            "-DNAPI_VERSION=10",
            "-Ilib/Jsrt",
            "-Iinclude/nodeapi",
            "-Ilib/NodeApi",
            "-Wno-date-time",
        });
        nodeapi_host.addFileArg(b.path("tools/nodeapi/ch-nodeapi.cpp"));
        nodeapi_host.addFileArg(dr.implib);
        addWinLibs(nodeapi_host, tables.nodeapi_win_libs);
        const nodeapi_host_bin = nodeapi_host.addPrefixedOutputFileArg("-o", "ch-nodeapi.exe");
        nodeapi_host.step.dependOn(&dr.link.step);
        nodeapi_host_bin_opt = nodeapi_host_bin;

        const nodeapi_stub = b.addSystemCommand(&.{ b.graph.zig_exe, "c++", "-nostdlib++" });
        nodeapi_stub.addArgs(&.{
            "-target",
            profile_target,
            "-shared",
            msRuntimeLibFlag(profile),
            "-std=c++17",
            "-O2",
            "-g",
            "-DNAPI_VERSION=10",
            "-Ilib/Jsrt",
            "-Iinclude/nodeapi",
            "-Ilib/NodeApi",
            "-Wno-date-time",
        });
        nodeapi_stub.addFileArg(b.path("tools/nodeapi/stub/stub_provider.cpp"));
        // Stub provider exports only its minimal subset (via NAPI_EXTERN and
        // STUB_HOST_EXPORT) and resolves Js* symbols against ChakraCore.lib;
        // never link the full 161-symbol libChakraNodeApi.def here.
        nodeapi_stub.addFileArg(dr.implib);
        addWinLibs(nodeapi_stub, tables.nodeapi_win_libs);
        const nodeapi_stub_bin = nodeapi_stub.addPrefixedOutputFileArg("-o", "libChakraNodeApiStub.dll");
        nodeapi_stub.step.dependOn(&dr.link.step);

        const nodeapi_step = b.step("ch-nodeapi", "Build ch-nodeapi.exe + libChakraNodeApiStub.dll + Node-API .node addons");
        nodeapi_step_opt = nodeapi_step;
        nodeapi_step.dependOn(prov_step);
        if (side_name == null) {
            nodeapi_step.dependOn(&b.addInstallFile(nodeapi_host_bin, "bin/ch-nodeapi.exe").step);
            nodeapi_step.dependOn(&b.addInstallFile(nodeapi_stub_bin, "lib/libChakraNodeApiStub.dll").step);
        } else {
            nodeapi_step.dependOn(&addSideCopy(b, python, b.fmt("{s}/bin/ch-nodeapi.exe", .{prefix_dir}), nodeapi_host_bin, artifacts_ready).step);
            nodeapi_step.dependOn(&addSideCopy(b, python, b.fmt("{s}/lib/libChakraNodeApiStub.dll", .{prefix_dir}), nodeapi_stub_bin, artifacts_ready).step);
        }

        for ([_][]const u8{ "hello_world", "missing_register", "promise_probe", "return_replaces", "version_reject" }) |name| {
            const addon = b.addSystemCommand(&.{ b.graph.zig_exe, "c++", "-nostdlib++" });
            addon.addArgs(&.{
                "-target",
                profile_target,
                "-shared",
                msRuntimeLibFlag(profile),
                "-std=c++17",
                "-O2",
                "-g",
                "-DNAPI_VERSION=10",
                "-DBUILDING_NODE_EXTENSION",
                "-Iinclude/nodeapi",
            });
            addon.addFileArg(b.path(b.fmt("tools/nodeapi/addons/{s}.cpp", .{name})));
            addon.addFileArg(prov_lib);
            const addon_bin = addon.addPrefixedOutputFileArg("-o", b.fmt("{s}.node", .{name}));
            addon.step.dependOn(&prov_link.step);
            if (side_name == null) {
                nodeapi_step.dependOn(&b.addInstallFile(addon_bin, b.fmt("lib/nodeapi-addons/{s}.node", .{name})).step);
            } else {
                nodeapi_step.dependOn(&addSideCopy(b, python, b.fmt("{s}/lib/nodeapi-addons/{s}.node", .{ prefix_dir, name }), addon_bin, artifacts_ready).step);
            }
        }
        for ([_][]const u8{ "hello_world", "promise_probe", "return_replaces" }) |name| {
            const src_js = b.path(b.fmt("tools/nodeapi/addons/{s}.js", .{name}));
            if (side_name == null) {
                nodeapi_step.dependOn(&b.addInstallFile(src_js, b.fmt("lib/nodeapi-addons/{s}.js", .{name})).step);
            } else {
                nodeapi_step.dependOn(&addSideCopy(b, python, b.fmt("{s}/lib/nodeapi-addons/{s}.js", .{ prefix_dir, name }), src_js, artifacts_ready).step);
            }
        }
    } else {
        registerFailStep(b, python, "libChakraNodeApi", "Not applicable on debug-static", "-Dprofile=debug-static has no shared ChakraCore.dll");
        registerFailStep(b, python, "ch-nodeapi", "Not applicable on debug-static", "-Dprofile=debug-static has no shared ChakraCore.dll");
        registerFailStep(b, python, "check-nodeapi", "Not applicable on debug-static", "-Dprofile=debug-static has no shared ChakraCore.dll");
    }

    // (0) Self-tests and build-tool verification.
    const tools_test = b.addSystemCommand(&.{python});
    tools_test.addFileArg(b.path("tools/build/test_build_tools.py"));
    tools_test.stdio = .inherit;
    const tools_test_top = b.step("check-build-tools", "Run verification-tool unit tests");
    tools_test_top.dependOn(&tools_test.step);

    const selftest_top = b.step("check-selftest", "Run tool self-tests and Zig Windows gate tests");
    for ([_][]const u8{
        "check_smoke_windows.py",
        "check_metadata_windows.py",
        "check_regression.py",
        "check_regression_windows.py",
        "check_jit.py",
        "check_nodeapi.py",
        "gen_nodeapi_exports.py",
        "check_wasm_p1.py",
        "check_gcstress.py",
        "check_fulljit.py",
    }) |script| {
        const st = b.addSystemCommand(&.{python});
        st.addFileArg(b.path(b.fmt("tools/build/{s}", .{script})));
        st.addArg("--self-test");
        st.stdio = .inherit;
        selftest_top.dependOn(&st.step);
    }
    selftest_top.dependOn(&tools_test.step);
    const win_gate_tests = b.addTest(.{
        .root_module = b.createModule(.{
            .root_source_file = b.path("tools/windows-gate-tests.zig"),
            .target = b.graph.host,
            .optimize = .debug,
        }),
    });
    selftest_top.dependOn(&b.addRunArtifact(win_gate_tests).step);
    const owned_bytes_tests = b.addTest(.{
        .root_module = b.createModule(.{
            .root_source_file = b.path("doc/examples/owned-bytes.zig"),
            .target = b.graph.host,
            .optimize = .debug,
        }),
    });
    selftest_top.dependOn(&b.addRunArtifact(owned_bytes_tests).step);
    const u8a_codec_tests = b.addTest(.{
        .root_module = b.createModule(.{
            .root_source_file = b.path(tables.zig_codec_source),
            .target = b.graph.host,
            .optimize = .debug,
        }),
    });
    selftest_top.dependOn(&b.addRunArtifact(u8a_codec_tests).step);

    const icu_bin_prefix = b.fmt("{s}/bin", .{prefix_dir});

    // (1) Bytecode check and (2) Regression suite.
    var verify_bc_step: ?*std.Build.Step = null;
    var regression_step: ?*std.Build.Step = null;
    if (is_release) {
        registerFailStep(b, python, "verify-bytecode", "Not applicable on release-shared", "-Dprofile=release-shared has no bytecode leg");
        registerFailStep(b, python, "check-regression", "Not applicable on release-shared", "-Dprofile=release-shared has no regression leg");
    } else {
        const verify_bc = b.addSystemCommand(&.{python});
        verify_bc.addFileArg(b.path("tools/build/verify_bytecode_pure.py"));
        verify_bc.addFileArg(b.path("tools/regenByteCode.py"));
        verify_bc.addArg(if (jit) "--jit" else "--noJit");
        verify_bc.addArg("--verify");
        addBinArg(b, verify_bc, side_name, prefix_dir, "bin/ch.exe", ch_bin, artifacts_ready, "--binary=");
        addIcuRuntimePath(&ctx, verify_bc, icu_bin_prefix);
        verify_bc.stdio = .inherit;
        const verify_bc_top = b.step("verify-bytecode", "Verify checked-in built-in bytecode with ch.exe");
        verify_bc_top.dependOn(&verify_bc.step);
        verify_bc_step = &verify_bc.step;

        const regression = b.addSystemCommand(&.{python});
        regression.addFileArg(b.path("tools/build/check_regression_windows.py"));
        regression.addFileArg(b.path("test/runtests.py"));
        addBinArg(b, regression, side_name, prefix_dir, "bin/ch.exe", ch_bin, artifacts_ready, null);
        regression.addArg("--select");
        regression.addArg(if (is_static) "debug" else "test");
        if (!jit) regression.addArg("--no-jit");
        if (icu == .none) regression.addArg("--no-icu");
        if (allow_unpinned_regression) regression.addArg("--allow-unpinned");
        addIcuRuntimePath(&ctx, regression, icu_bin_prefix);
        regression.stdio = .inherit;
        const regression_top = b.step("check-regression", "Run Windows x64 regression suite against ch.exe");
        regression_top.dependOn(&regression.step);
        regression_step = &regression.step;
    }

    // (3) Windows smoke gate.
    const smoke = b.addSystemCommand(&.{python});
    smoke.addFileArg(b.path("tools/build/check_smoke_windows.py"));
    addBinArg(b, smoke, side_name, prefix_dir, "bin/ch.exe", ch_bin, artifacts_ready, null);
    smoke.addArg("--profile");
    smoke.addArg(profile.name());
    smoke.addArg("--icu");
    smoke.addArg(icu.name());
    if (nodeapi_host_bin_opt) |nh| {
        if (side_name == null) {
            const installed_nh: std.Build.LazyPath = .{ .relative = .{ .base = .install_prefix, .sub_path = "bin/ch-nodeapi.exe" } };
            smoke.addPrefixedFileArg("--ch-nodeapi=", installed_nh);
        } else {
            smoke.addArg(b.fmt("--ch-nodeapi={s}/bin/ch-nodeapi.exe", .{prefix_dir}));
            smoke.addFileInput(nh);
        }
        if (nodeapi_step_opt) |ns| smoke.step.dependOn(ns);
    }
    addIcuRuntimePath(&ctx, smoke, icu_bin_prefix);
    smoke.stdio = .inherit;
    const smoke_top = b.step("check-smoke", "Run Windows ch.exe and ch-nodeapi.exe smoke assertions");
    smoke_top.dependOn(&smoke.step);

    // (4) JIT smoke gate.
    var jit_smoke_step: ?*std.Build.Step = null;
    if (jit) {
        const jit_smoke = b.addSystemCommand(&.{python});
        jit_smoke.addFileArg(b.path("tools/build/check_jit.py"));
        addBinArg(b, jit_smoke, side_name, prefix_dir, "bin/ch.exe", ch_bin, artifacts_ready, null);
        addIcuRuntimePath(&ctx, jit_smoke, icu_bin_prefix);
        jit_smoke.stdio = .inherit;
        const jit_top = b.step("check-jit", "Verify JIT codegen works on Windows ch.exe");
        jit_top.dependOn(&jit_smoke.step);
        jit_smoke_step = &jit_smoke.step;
    } else {
        registerFailStep(b, python, "check-jit", "Not applicable when -Djit=false", "-Djit=false built without lib/Backend");
    }

    // (5) Wasm P1 gate.
    if (supportsWasmGate(profile, jit)) {
        const wasm_gate = b.addSystemCommand(&.{python});
        wasm_gate.addFileArg(b.path("tools/build/check_wasm_p1.py"));
        wasm_gate.addFileArg(b.path("test/runtests.py"));
        addBinArg(b, wasm_gate, side_name, prefix_dir, "bin/ch.exe", ch_bin, artifacts_ready, null);
        addIcuRuntimePath(&ctx, wasm_gate, icu_bin_prefix);
        wasm_gate.stdio = .inherit;
        const wasm_top = b.step("check-wasm", "Run Wasm P1 test suite");
        wasm_top.dependOn(&wasm_gate.step);
    } else {
        registerFailStep(b, python, "check-wasm", "Not applicable without -Djit=true on test/debug-static", "check-wasm requires -Djit=true and non-release profile");
    }

    // (6) Bounded GCStress heap-walk gate.
    const gc_step = b.addSystemCommand(&.{python});
    gc_step.addFileArg(b.path("tools/build/check_gcstress.py"));
    addBinArg(b, gc_step, side_name, prefix_dir, "bin/GCStress.exe", gs_bin, artifacts_ready, null);
    addIcuRuntimePath(&ctx, gc_step, icu_bin_prefix);
    gc_step.stdio = .inherit;
    const gc_top = b.step("check-gcstress", "Run bounded GCStress heap walks");
    gc_top.dependOn(&gc_step.step);

    // (7) No-profile FullJit gate.
    if (supportsFullJitGate(profile, jit)) {
        const fj_step = b.addSystemCommand(&.{python});
        fj_step.addFileArg(b.path("tools/build/check_fulljit.py"));
        fj_step.addFileArg(b.path("test/runtests.py"));
        addBinArg(b, fj_step, side_name, prefix_dir, "bin/ch.exe", ch_bin, artifacts_ready, null);
        fj_step.addArg("--select");
        fj_step.addArg(if (is_static) "debug" else "test");
        addIcuRuntimePath(&ctx, fj_step, icu_bin_prefix);
        fj_step.stdio = .inherit;
        const fj_top = b.step("check-fulljit", "Run no-profile FullJit regression suites");
        fj_top.dependOn(&fj_step.step);
    } else {
        registerFailStep(b, python, "check-fulljit", "Not applicable without -Djit=true on test/debug-static", "check-fulljit requires -Djit=true and non-release profile");
    }

    // (8) PE/COFF Metadata gate.
    const metadata = b.addSystemCommand(&.{python});
    metadata.addFileArg(b.path("tools/build/check_metadata_windows.py"));
    if (side_name == null) {
        metadata.addDirectoryArg(.{ .relative = .{ .base = .install_prefix } });
    } else {
        metadata.addArg(prefix_dir);
    }
    metadata.addArg("--profile");
    metadata.addArg(profile.name());
    metadata.addArg("--icu");
    metadata.addArg(icu.name());
    if (!jit) metadata.addArg("--no-jit");
    metadata.step.dependOn(artifacts_ready);
    if (nodeapi_step_opt) |ns| metadata.step.dependOn(ns);
    metadata.stdio = .inherit;
    const metadata_top = b.step("check-metadata", "Verify Windows PE32+/COFF artifact metadata and exports");
    metadata_top.dependOn(&metadata.step);

    // (9) Aggregate check gate.
    const check_top = b.step("check", "Full verification for selected Windows profile and JIT setting");
    check_top.dependOn(selftest_top);
    if (verify_bc_step) |s| check_top.dependOn(s);
    if (regression_step) |s| check_top.dependOn(s);
    check_top.dependOn(&smoke.step);
    if (jit_smoke_step) |s| check_top.dependOn(s);
    if (check_nodeapi_step_opt) |s| check_top.dependOn(s);
    check_top.dependOn(&gc_step.step);
    check_top.dependOn(&metadata.step);

    // (10) Release Packaging and package verification.
    if (supportsPackageGate(profile)) {
        const pkg_cmd = b.addSystemCommand(&.{python});
        pkg_cmd.addFileArg(b.path("tools/package/build_package.py"));
        pkg_cmd.addArg("--prefix");
        if (side_name == null) {
            pkg_cmd.addDirectoryArg(.{ .relative = .{ .base = .install_prefix } });
        } else {
            pkg_cmd.addArg(prefix_dir);
        }
        pkg_cmd.addArg("--out-dir");
        pkg_cmd.addArg("out/package");
        pkg_cmd.addArg("--target");
        pkg_cmd.addArg("windows-x64");
        pkg_cmd.step.dependOn(artifacts_ready);
        if (nodeapi_step_opt) |ns| pkg_cmd.step.dependOn(ns);
        pkg_cmd.stdio = .inherit;
        pkg_cmd.has_side_effects = true;
        const pkg_top = b.step("package", "Build chakracore-<version>-windows-x64.tar.gz in out/package");
        pkg_top.dependOn(&pkg_cmd.step);

        const verify_cmd = b.addSystemCommand(&.{python});
        verify_cmd.addFileArg(b.path("tools/package/verify_package.py"));
        verify_cmd.addArg("out/package");
        verify_cmd.step.dependOn(&pkg_cmd.step);
        verify_cmd.stdio = .inherit;
        verify_cmd.has_side_effects = true;

        const test_cmd = b.addSystemCommand(&.{python});
        test_cmd.addFileArg(b.path("tools/package/test_package.py"));
        test_cmd.addArg("out/package");
        test_cmd.step.dependOn(&verify_cmd.step);
        addIcuRuntimePath(&ctx, test_cmd, null);
        test_cmd.stdio = .inherit;
        test_cmd.has_side_effects = true;

        const check_pkg_top = b.step("check-package", "Verify Windows package manifest and packaged binaries");
        check_pkg_top.dependOn(&test_cmd.step);
    } else {
        registerFailStep(b, python, "package", "Not applicable on debug-static", "-Dprofile=debug-static has no shared ChakraCore.dll");
        registerFailStep(b, python, "check-package", "Not applicable on debug-static", "-Dprofile=debug-static has no shared ChakraCore.dll");
    }
}
