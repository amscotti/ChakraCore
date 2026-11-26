//-------------------------------------------------------------------------------------------------------
// Copyright (c) ChakraCore Project Contributors. All rights reserved.
// Licensed under the MIT license. See LICENSE.txt file in the project root for full license information.
//-------------------------------------------------------------------------------------------------------
//! B4.3 / Linux x86_64 multi-profile build graph.
//!
//! Native x86_64 Linux targeting parameterized glibc (default 2.39) and
//! system ICU major (default 74), supporting profiles:
//!   * test           : RelWithDebInfo shared + optional JIT (default, zig-out/)
//!   * debug-static   : Debug static archive + TestHooks in ch (out/debug-static/)
//!   * release-shared : Release shared, no WABT (out/release-shared/)
//! Plus -Djit=true|false (default true; false sets -DDISABLE_JIT=1 and
//! omits lib/Backend and -D_ENABLE_DYNAMIC_THUNKS=1).
//!
//! Structure mirrors build.zig (Mac): tables-driven compiles with native
//! depfiles, generator/input edges (jstoc, PAL config, wabt config),
//! native installs for the test profile, install_profile.py side-prefix
//! copy-out for debug-static / release-shared, check steps off the default
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
//!   same reason, kept on static ch and GCStress where an archive is linked.
//! * Object files are NNNN-<base>.o for uniqueness only; ELF init order
//!   comes from the explicit join/archive order lists, not names.
//!
//! Contracts: doc/build-contract-linux.md (sources/flags/includes/links),
//! doc/pal-config-contract-linux.md (config.h checks), doc/b4-linux.md
//! (environment, qemu, baselines).

const std = @import("std");
const builtin = @import("builtin");
const pal_config = @import("../pal-config-linux/check.zig");
const tables = @import("tables_linux.zig");
const mac_tables = @import("tables.zig");

pub const profile_target = "x86_64-linux-gnu";
pub const default_glibc = "2.39";
pub const default_icu_major = "74";
const default_icu_include = "/usr/include";
const default_icu_libdir = "/usr/lib/x86_64-linux-gnu";

pub const Profile = enum {
    @"test",
    debug_static,
    release_shared,

    pub fn name(self: Profile) []const u8 {
        return switch (self) {
            .@"test" => "test",
            .debug_static => "debug-static",
            .release_shared => "release-shared",
        };
    }
};

pub fn parseProfile(raw: []const u8) ?Profile {
    if (std.mem.eql(u8, raw, "test")) return .@"test";
    if (std.mem.eql(u8, raw, "debug-static")) return .debug_static;
    if (std.mem.eql(u8, raw, "release-shared")) return .release_shared;
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
        .@"test", .release_shared => .fast,
        .debug_static => .debug,
    };
}

pub const Sanitize = enum {
    none,
    address,
    undefined,
    thread,

    pub fn name(self: Sanitize) []const u8 {
        return switch (self) {
            .none => "none",
            .address => "address",
            .undefined => "undefined",
            .thread => "thread",
        };
    }

    pub fn fsanitizeFlag(self: Sanitize) []const u8 {
        return switch (self) {
            .none => "",
            .address => "-fsanitize=address",
            .undefined => "-fsanitize=undefined",
            .thread => "-fsanitize=thread",
        };
    }
};

pub fn parseSanitize(raw: []const u8) ?Sanitize {
    if (std.mem.eql(u8, raw, "none")) return .none;
    if (std.mem.eql(u8, raw, "address")) return .address;
    if (std.mem.eql(u8, raw, "undefined")) return .undefined;
    if (std.mem.eql(u8, raw, "thread")) return .thread;
    return null;
}

pub fn defaultJitForSanitize(s: Sanitize) bool {
    return s == .none;
}

pub fn sanitizeCompileFlag(s: Sanitize) ?[]const u8 {
    return if (s == .none) null else s.fsanitizeFlag();
}

pub fn sideNameForConfig(arena: std.mem.Allocator, profile: Profile, sanitize: Sanitize) ?[]const u8 {
    if (sanitize != .none) return std.fmt.allocPrint(arena, "sanitize-{s}", .{sanitize.name()}) catch @panic("OOM");
    if (profile != .@"test") return profile.name();
    return null;
}

pub fn installPrefixForConfig(arena: std.mem.Allocator, profile: Profile, sanitize: Sanitize) []const u8 {
    if (sanitize != .none) return std.fmt.allocPrint(arena, "out/sanitize-{s}", .{sanitize.name()}) catch @panic("OOM");
    if (profile == .@"test") return "zig-out";
    return std.fmt.allocPrint(arena, "out/{s}", .{profile.name()}) catch @panic("OOM");
}

pub fn metadataProfileForConfig(arena: std.mem.Allocator, profile: Profile, sanitize: Sanitize) []const u8 {
    if (sanitize != .none and profile == .@"test") {
        return sideNameForConfig(arena, profile, sanitize).?;
    }
    return profile.name();
}

pub fn metadataArgsForConfig(
    arena: std.mem.Allocator,
    profile: Profile,
    sanitize: Sanitize,
    icu_major: []const u8,
    jit: bool,
) []const []const u8 {
    var args: std.ArrayList([]const u8) = .empty;
    args.append(arena, "--profile") catch @panic("OOM");
    args.append(arena, metadataProfileForConfig(arena, profile, sanitize)) catch @panic("OOM");
    if (sanitize != .none) {
        args.append(arena, "--sanitize") catch @panic("OOM");
        args.append(arena, sanitize.name()) catch @panic("OOM");
    }
    args.append(arena, "--icu-major") catch @panic("OOM");
    args.append(arena, icu_major) catch @panic("OOM");
    if (!jit) {
        args.append(arena, "--no-jit") catch @panic("OOM");
    }
    return args.toOwnedSlice(arena) catch @panic("OOM");
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

pub fn isValidGlibcVersion(v: []const u8) bool {
    if (v.len == 0) return false;
    var parts: usize = 0;
    var it = std.mem.splitScalar(u8, v, '.');
    while (it.next()) |seg| {
        if (seg.len == 0) return false;
        for (seg) |c| {
            if (c < '0' or c > '9') return false;
        }
        parts += 1;
    }
    return parts == 2 or parts == 3;
}

pub fn targetTripleForGlibc(arena: std.mem.Allocator, glibc: []const u8) []const u8 {
    if (std.mem.eql(u8, glibc, default_glibc)) return profile_target;
    return std.fmt.allocPrint(arena, "{s}.{s}", .{ profile_target, glibc }) catch @panic("OOM");
}

pub fn isValidIcuMajor(v: []const u8) bool {
    if (v.len == 0) return false;
    var nonzero = false;
    for (v) |c| {
        if (c < '0' or c > '9') return false;
        if (c != '0') nonzero = true;
    }
    return nonzero;
}

pub fn parseUvernumMajorNum(bytes: []const u8) ?[]const u8 {
    var lines = std.mem.splitScalar(u8, bytes, '\n');
    while (lines.next()) |raw_line| {
        const line = std.mem.trim(u8, raw_line, " \t\r");
        if (!std.mem.startsWith(u8, line, "#")) continue;
        const rest = std.mem.trimStart(u8, line[1..], " \t");
        if (!std.mem.startsWith(u8, rest, "define")) continue;
        const after_def = std.mem.trimStart(u8, rest["define".len..], " \t");
        if (!std.mem.startsWith(u8, after_def, "U_ICU_VERSION_MAJOR_NUM")) continue;
        const tail = after_def["U_ICU_VERSION_MAJOR_NUM".len..];
        if (tail.len == 0 or (tail[0] != ' ' and tail[0] != '\t')) continue;
        const val = std.mem.trimStart(u8, tail, " \t");
        var end: usize = 0;
        while (end < val.len and val[end] >= '0' and val[end] <= '9') : (end += 1) {}
        if (end > 0) return val[0..end];
    }
    return null;
}

pub fn uvernumMatchesMajor(bytes: []const u8, icu_major: []const u8) bool {
    if (parseUvernumMajorNum(bytes)) |hdr_num| {
        if (!std.mem.eql(u8, hdr_num, icu_major)) return false;
    }
    var buf: [64]u8 = undefined;
    const needle = std.fmt.bufPrint(&buf, "U_ICU_VERSION_SHORT \"{s}\"", .{icu_major}) catch return false;
    return std.mem.indexOf(u8, bytes, needle) != null;
}

pub fn sonameMatchesMajor(soname: []const u8, stem: []const u8, icu_major: []const u8) bool {
    if (!std.mem.startsWith(u8, soname, stem)) return false;
    const after_stem = soname[stem.len..];
    if (!std.mem.startsWith(u8, after_stem, ".so.")) return false;
    const after_so = after_stem[".so.".len..];
    var end: usize = 0;
    while (end < after_so.len and after_so[end] >= '0' and after_so[end] <= '9') : (end += 1) {}
    if (end == 0) return false;
    return std.mem.eql(u8, after_so[0..end], icu_major);
}

pub fn parseElf64SonameFromSlice(ehdr_bytes: []const u8, shdrs_bytes: []const u8, dyn_bytes: []const u8, dynstr_bytes: []const u8) ?[]const u8 {
    if (ehdr_bytes.len < @sizeOf(std.elf.Elf64_Ehdr)) return null;
    const ehdr: *align(1) const std.elf.Elf64_Ehdr = @ptrCast(ehdr_bytes[0..@sizeOf(std.elf.Elf64_Ehdr)]);
    if (!std.mem.eql(u8, ehdr.e_ident[0..4], std.elf.MAGIC)) return null;
    if (ehdr.e_ident[std.elf.EI_CLASS] != std.elf.ELFCLASS64) return null;
    if (ehdr.e_ident[std.elf.EI_DATA] != std.elf.ELFDATA2LSB) return null;
    if (shdrs_bytes.len < ehdr.e_shnum * @sizeOf(std.elf.Elf64_Shdr)) return null;
    const dyn_cnt = dyn_bytes.len / @sizeOf(std.elf.Elf64_Dyn);
    var i: usize = 0;
    while (i < dyn_cnt) : (i += 1) {
        const off = i * @sizeOf(std.elf.Elf64_Dyn);
        const dyn: *align(1) const std.elf.Elf64_Dyn = @ptrCast(dyn_bytes[off..][0..@sizeOf(std.elf.Elf64_Dyn)]);
        if (dyn.d_tag == std.elf.DT_NULL) break;
        if (dyn.d_tag == std.elf.DT_SONAME) {
            if (dyn.d_val >= dynstr_bytes.len) return null;
            const tail = dynstr_bytes[dyn.d_val..];
            const nul = std.mem.indexOfScalar(u8, tail, 0) orelse return null;
            return tail[0..nul];
        }
    }
    return null;
}

fn readElfSoname(io: std.Io, arena: std.mem.Allocator, lib_path: []const u8) ?[]const u8 {
    const f = std.Io.Dir.openFileAbsolute(io, lib_path, .{}) catch return null;
    defer f.close(io);
    var ehdr_buf: [@sizeOf(std.elf.Elf64_Ehdr)]u8 = undefined;
    const n_ehdr = f.readPositionalAll(io, &ehdr_buf, 0) catch return null;
    if (n_ehdr < @sizeOf(std.elf.Elf64_Ehdr)) return null;
    const ehdr: *align(1) const std.elf.Elf64_Ehdr = @ptrCast(&ehdr_buf);
    if (!std.mem.eql(u8, ehdr.e_ident[0..4], std.elf.MAGIC)) return null;
    if (ehdr.e_ident[std.elf.EI_CLASS] != std.elf.ELFCLASS64) return null;
    if (ehdr.e_ident[std.elf.EI_DATA] != std.elf.ELFDATA2LSB) return null;
    if (ehdr.e_shoff == 0 or ehdr.e_shnum == 0 or ehdr.e_shnum > 4096 or ehdr.e_shentsize != @sizeOf(std.elf.Elf64_Shdr)) return null;
    const sh_bytes_len = @as(usize, ehdr.e_shnum) * @sizeOf(std.elf.Elf64_Shdr);
    const sh_buf = arena.alloc(u8, sh_bytes_len) catch @panic("OOM");
    const n_sh = f.readPositionalAll(io, sh_buf, ehdr.e_shoff) catch return null;
    if (n_sh < sh_bytes_len) return null;
    var i: usize = 0;
    while (i < ehdr.e_shnum) : (i += 1) {
        const sh_off = i * @sizeOf(std.elf.Elf64_Shdr);
        const sh: *align(1) const std.elf.Elf64_Shdr = @ptrCast(sh_buf[sh_off..][0..@sizeOf(std.elf.Elf64_Shdr)]);
        if (sh.sh_type != std.elf.SHT_DYNAMIC) continue;
        if (sh.sh_link >= ehdr.e_shnum) return null;
        if (sh.sh_size == 0 or sh.sh_size > (4 << 20)) return null;
        const dyn_buf = arena.alloc(u8, @intCast(sh.sh_size)) catch @panic("OOM");
        const n_dyn = f.readPositionalAll(io, dyn_buf, sh.sh_offset) catch return null;
        if (n_dyn < sh.sh_size) return null;
        const str_off = @as(usize, sh.sh_link) * @sizeOf(std.elf.Elf64_Shdr);
        const str_sh: *align(1) const std.elf.Elf64_Shdr = @ptrCast(sh_buf[str_off..][0..@sizeOf(std.elf.Elf64_Shdr)]);
        if (str_sh.sh_size == 0 or str_sh.sh_size > (16 << 20)) return null;
        const dynstr_buf = arena.alloc(u8, @intCast(str_sh.sh_size)) catch @panic("OOM");
        const n_str = f.readPositionalAll(io, dynstr_buf, str_sh.sh_offset) catch return null;
        if (n_str < str_sh.sh_size) return null;
        return parseElf64SonameFromSlice(&ehdr_buf, sh_buf, dyn_buf, dynstr_buf);
    }
    return null;
}

fn fail(comptime fmt: []const u8, args: anytype) noreturn {
    std.debug.print("build error: " ++ fmt ++ "\n", args);
    std.process.exit(1);
}

fn gateProfile(b: *std.Build) Profile {
    const raw = b.option([]const u8, "profile", "Build profile: test (default), debug-static, release-shared") orelse "test";
    return parseProfile(raw) orelse
        fail("unsupported -Dprofile={s} on Linux: expected test, debug-static, or release-shared", .{raw});
}

fn gateSanitize(b: *std.Build) Sanitize {
    const raw = b.option([]const u8, "sanitize", "Sanitizer mode: none (default), address, undefined, thread") orelse "none";
    return parseSanitize(raw) orelse
        fail("unsupported -Dsanitize={s} on Linux: expected none, address, undefined, or thread", .{raw});
}

const HostCompilers = struct {
    cc: []const u8,
    cxx: []const u8,
};

fn gateHostCompilers(b: *std.Build) HostCompilers {
    const cc = b.findProgram(.{ .names = &.{ "clang", "clang-18", "clang-19", "clang-17" } }) orelse
        fail("-Dsanitize requires host clang on PATH", .{});
    const cxx = b.findProgram(.{ .names = &.{ "clang++", "clang++-18", "clang++-19", "clang++-17" } }) orelse
        fail("-Dsanitize requires host clang++ on PATH", .{});
    return .{ .cc = cc, .cxx = cxx };
}

fn gateOptimize(b: *std.Build, profile: Profile) void {
    // The C/C++ -O flags are explicit on our command lines; pin the mode
    // so a mode name alone can never select another configuration.
    const want = expectedOptimize(profile);
    const choice = b.option(std.builtin.Optimize, "optimize", "Optimization (pinned per profile: fast for test/release-shared, debug for debug-static)") orelse want;
    if (choice != want)
        fail("unsupported -Doptimize={t} for -Dprofile={s} on Linux: requires {t}", .{ choice, profile.name(), want });
}

fn gateTarget(b: *std.Build) std.Build.ResolvedTarget {
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
    return rt;
}

pub fn glibcFirstLineMatches(first: []const u8, want_glibc: []const u8) bool {
    // First line looks like "ldd (Ubuntu GLIBC 2.39-0ubuntu8.9) 2.39".
    // Exact-match the trailing bare version token (the Mac gateSdk
    // exact-compare pattern in build.zig): a substring search for
    // "2.39" would also pass on e.g. a "12.39" vendor string or a
    // "2.390" token while the actual glibc version differs.
    const trimmed = std.mem.trimEnd(u8, first, " \t\r");
    const sp = std.mem.lastIndexOfScalar(u8, trimmed, ' ') orelse return false;
    return std.mem.eql(u8, trimmed[sp + 1 ..], want_glibc);
}

test "glibc gate exact-matches the trailing version token" {
    try std.testing.expect(glibcFirstLineMatches("ldd (Ubuntu GLIBC 2.39-0ubuntu8.9) 2.39", "2.39"));
    try std.testing.expect(glibcFirstLineMatches("ldd (Ubuntu GLIBC 2.35-0ubuntu3.8) 2.35", "2.35"));
    try std.testing.expect(!glibcFirstLineMatches("ldd (Ubuntu GLIBC 2.39-0ubuntu8.9) 2.40", "2.39"));
    try std.testing.expect(!glibcFirstLineMatches("ldd (vendor 12.39) 2.38", "2.39"));
    try std.testing.expect(!glibcFirstLineMatches("ldd (GNU libc) 2.390", "2.39"));
    try std.testing.expect(!glibcFirstLineMatches("", "2.39"));
}

test "linux parameter helpers validate profiles, glibc, and ICU major" {
    try std.testing.expectEqual(Profile.@"test", parseProfile("test").?);
    try std.testing.expectEqual(Profile.debug_static, parseProfile("debug-static").?);
    try std.testing.expectEqual(Profile.release_shared, parseProfile("release-shared").?);
    try std.testing.expect(parseProfile("jit") == null);

    try std.testing.expectEqual(std.builtin.Optimize.fast, expectedOptimize(.@"test"));
    try std.testing.expectEqual(std.builtin.Optimize.debug, expectedOptimize(.debug_static));
    try std.testing.expectEqual(std.builtin.Optimize.fast, expectedOptimize(.release_shared));

    try std.testing.expect(isValidGlibcVersion("2.39"));
    try std.testing.expect(isValidGlibcVersion("2.35"));
    try std.testing.expect(isValidGlibcVersion("2.39.0"));
    try std.testing.expect(!isValidGlibcVersion(""));
    try std.testing.expect(!isValidGlibcVersion("2"));
    try std.testing.expect(!isValidGlibcVersion("2.39-ubuntu"));

    try std.testing.expectEqualStrings("x86_64-linux-gnu", targetTripleForGlibc(std.testing.allocator, "2.39"));
    const custom_triple = targetTripleForGlibc(std.testing.allocator, "2.35");
    defer std.testing.allocator.free(custom_triple);
    try std.testing.expectEqualStrings("x86_64-linux-gnu.2.35", custom_triple);

    try std.testing.expect(isValidIcuMajor("74"));
    try std.testing.expect(isValidIcuMajor("76"));
    try std.testing.expect(!isValidIcuMajor("0"));
    try std.testing.expect(!isValidIcuMajor("74a"));
    try std.testing.expect(uvernumMatchesMajor("#define U_ICU_VERSION_SHORT \"74\"\n", "74"));
    try std.testing.expect(uvernumMatchesMajor("#define U_ICU_VERSION_SHORT \"76\"\n", "76"));
    try std.testing.expect(!uvernumMatchesMajor("#define U_ICU_VERSION_SHORT \"74\"\n", "76"));
}

fn gateGlibc(b: *std.Build) []const u8 {
    const want_glibc = b.option([]const u8, "glibc", "Target/host glibc version (default: " ++ default_glibc ++ ")") orelse default_glibc;
    if (!isValidGlibcVersion(want_glibc))
        fail("invalid -Dglibc={s}: expected major.minor (e.g. 2.39)", .{want_glibc});
    const res = b.runFallible(&.{ "ldd", "--version" }, .{});
    const out: []const u8 = switch (res) {
        .success => |o| o,
        else => fail("cannot run ldd --version: glibc floor unverifiable (need {s})", .{want_glibc}),
    };
    var it = std.mem.splitScalar(u8, out, '\n');
    const first = it.next() orelse fail("cannot parse ldd --version: glibc floor unverifiable (need {s})", .{want_glibc});
    if (!glibcFirstLineMatches(first, want_glibc))
        fail("unsupported glibc ({s}): expected {s} (pass -Dglibc=<major.minor> to target host glibc)", .{ first, want_glibc });
    return want_glibc;
}

fn gateIcu(b: *std.Build) struct { include: []const u8, libdir: []const u8, major: []const u8 } {
    const arena = b.graph.arena;
    const io = b.graph.io;
    const icu = b.option([]const u8, "icu-include", "ICU include dir (default: " ++ default_icu_include ++ ")") orelse default_icu_include;
    const libdir = b.option([]const u8, "icu-libdir", "ICU lib dir (default: " ++ default_icu_libdir ++ ")") orelse default_icu_libdir;
    const major = b.option([]const u8, "icu-major", "Expected ICU major version (default: " ++ default_icu_major ++ ")") orelse default_icu_major;
    if (!isValidIcuMajor(major))
        fail("invalid -Dicu-major={s}: expected positive integer (e.g. 74)", .{major});
    const uchar = std.fmt.allocPrint(arena, "{s}/unicode/uchar.h", .{icu}) catch @panic("OOM");
    std.Io.Dir.accessAbsolute(io, uchar, .{}) catch
        fail("Cannot find ICU (missing {s}). Try installing libicu-dev or the appropriate packages for your platform.", .{uchar});
    for ([_][]const u8{ "libicuuc.so", "libicui18n.so" }) |lib| {
        const p = std.fmt.allocPrint(arena, "{s}/{s}", .{ libdir, lib }) catch @panic("OOM");
        std.Io.Dir.accessAbsolute(io, p, .{}) catch
            fail("Cannot find ICU libraries (missing {s}). Try installing libicu-dev or the appropriate packages for your platform.", .{p});
    }
    const uvernum = std.fmt.allocPrint(arena, "{s}/unicode/uvernum.h", .{icu}) catch @panic("OOM");
    const vf = std.Io.Dir.openFileAbsolute(io, uvernum, .{}) catch
        fail("cannot read {s}: ICU version unverifiable (need major {s})", .{ uvernum, major });
    defer vf.close(io);
    var vfr = vf.readerStreaming(io, &.{});
    const bytes = vfr.interface.allocRemaining(arena, .limited(1 << 20)) catch
        fail("cannot read {s}: ICU version unverifiable (need major {s})", .{ uvernum, major });
    if (!uvernumMatchesMajor(bytes, major))
        fail("{s}: ICU major unverifiable or mismatched (need U_ICU_VERSION_SHORT \"{s}\" and matching U_ICU_VERSION_MAJOR_NUM; pass -Dicu-major=<N> to override)", .{ uvernum, major });
    const lib_checks = [_]struct { file: []const u8, stem: []const u8 }{
        .{ .file = "libicuuc.so", .stem = "libicuuc" },
        .{ .file = "libicui18n.so", .stem = "libicui18n" },
    };
    for (lib_checks) |chk| {
        const p = std.fmt.allocPrint(arena, "{s}/{s}", .{ libdir, chk.file }) catch @panic("OOM");
        if (readElfSoname(io, arena, p)) |soname| {
            if (!sonameMatchesMajor(soname, chk.stem, major)) {
                fail("ICU header/library version skew: {s} declares major {s}, but {s} has DT_SONAME '{s}' (pass matching -Dicu-include, -Dicu-libdir, and -Dicu-major)", .{ uvernum, major, p, soname });
            }
        } else {
            var link_buf: [1024]u8 = undefined;
            if (std.Io.Dir.readLinkAbsolute(io, p, &link_buf)) |target_len| {
                const target_base = std.fs.path.basename(link_buf[0..target_len]);
                if (std.mem.indexOf(u8, target_base, ".so.") != null and !sonameMatchesMajor(target_base, chk.stem, major)) {
                    fail("ICU header/library version skew: {s} declares major {s}, but {s} points to '{s}' (pass matching -Dicu-include, -Dicu-libdir, and -Dicu-major)", .{ uvernum, major, p, target_base });
                }
            } else |_| {}
        }
    }
    return .{ .include = icu, .libdir = libdir, .major = major };
}

const Ctx = struct {
    b: *std.Build,
    zig_exe: []const u8,
    host_cc: []const u8,
    host_cxx: []const u8,
    profile: Profile,
    sanitize: Sanitize,
    jit: bool,
    target_triple: []const u8,
    glibc: []const u8,
    icu_include: []const u8,
    icu_libdir: []const u8,
    icu_major: []const u8,
    config_dir: std.Build.LazyPath,
    config_h: std.Build.LazyPath,
    wabt_dir: ?std.Build.LazyPath,
    wabt_h: ?std.Build.LazyPath,
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

pub fn shouldIncludeDefine(d: []const u8, profile: Profile) bool {
    if (profile != .@"test" and std.mem.eql(u8, d, "-DENABLE_DEBUG_CONFIG_OPTIONS=1")) return false;
    if (isReleaseProfile(profile) and std.mem.eql(u8, d, "-DCAN_BUILD_WABT")) return false;
    return true;
}

pub fn isBackendPath(src: []const u8) bool {
    return std.mem.startsWith(u8, src, "lib/Backend/");
}

pub fn isWabtPath(src: []const u8) bool {
    return std.mem.startsWith(u8, src, "lib/wabt/");
}

pub fn shouldCompileGroup(g: *const tables.Group, profile: Profile, jit: bool) bool {
    if (!jit and g == &tables.backend_group) return false;
    if (isReleaseProfile(profile) and g == &tables.wabt_group) return false;
    if (isStaticProfile(profile) and g == &tables.dylib_group) return false;
    return true;
}

pub fn isExcludedStaticObj(src: []const u8, profile: Profile, jit: bool) bool {
    if (!jit and isBackendPath(src)) return true;
    if (isReleaseProfile(profile) and isWabtPath(src)) return true;
    return false;
}

pub const all_groups = [_]*const tables.Group{
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

pub fn expectedCompileCount(profile: Profile, jit: bool) u32 {
    var count: u32 = 695;
    if (!jit) count -= 91;
    if (isReleaseProfile(profile)) count -= 37;
    if (isStaticProfile(profile)) count = count - 3 + @as(u32, @intCast(mac_tables.ch_static_extra_cxx.len));
    return count;
}

test "linux group, define, and compile-count filtering across profiles" {
    try std.testing.expect(shouldIncludeDefine("-DCAN_BUILD_WABT", .@"test"));
    try std.testing.expect(shouldIncludeDefine("-DCAN_BUILD_WABT", .debug_static));
    try std.testing.expect(!shouldIncludeDefine("-DCAN_BUILD_WABT", .release_shared));
    try std.testing.expect(shouldIncludeDefine("-DENABLE_DEBUG_CONFIG_OPTIONS=1", .@"test"));
    try std.testing.expect(!shouldIncludeDefine("-DENABLE_DEBUG_CONFIG_OPTIONS=1", .debug_static));
    try std.testing.expect(!shouldIncludeDefine("-DENABLE_DEBUG_CONFIG_OPTIONS=1", .release_shared));

    try std.testing.expect(!shouldCompileGroup(&tables.backend_group, .@"test", false));
    try std.testing.expect(shouldCompileGroup(&tables.backend_group, .@"test", true));
    try std.testing.expect(!shouldCompileGroup(&tables.wabt_group, .release_shared, true));
    try std.testing.expect(shouldCompileGroup(&tables.wabt_group, .debug_static, true));
    try std.testing.expect(!shouldCompileGroup(&tables.dylib_group, .debug_static, true));
    try std.testing.expect(shouldCompileGroup(&tables.dylib_group, .release_shared, true));

    try std.testing.expect(isExcludedStaticObj("lib/Backend/GlobOpt.cpp", .@"test", false));
    try std.testing.expect(!isExcludedStaticObj("lib/Backend/GlobOpt.cpp", .@"test", true));
    try std.testing.expect(isExcludedStaticObj("lib/wabt/src/ast.cc", .release_shared, true));
    try std.testing.expect(!isExcludedStaticObj("lib/wabt/src/ast.cc", .debug_static, true));

    try std.testing.expectEqual(@as(u32, 695), expectedCompileCount(.@"test", true));
    try std.testing.expectEqual(@as(u32, 604), expectedCompileCount(.@"test", false));
    try std.testing.expectEqual(@as(u32, 658), expectedCompileCount(.release_shared, true));
    try std.testing.expectEqual(@as(u32, 567), expectedCompileCount(.release_shared, false));
    try std.testing.expectEqual(@as(u32, 693), expectedCompileCount(.debug_static, true));
    try std.testing.expectEqual(@as(u32, 602), expectedCompileCount(.debug_static, false));

    var seen: std.StringHashMap(void) = .init(std.testing.allocator);
    defer seen.deinit();
    var total: u32 = 0;
    for (all_groups) |g| {
        for (g.cxx) |src| {
            try std.testing.expect(!seen.contains(src));
            try seen.put(src, {});
            total += 1;
        }
        for (g.c) |src| {
            try std.testing.expect(!seen.contains(src));
            try seen.put(src, {});
            total += 1;
        }
        for (g.asm_files) |src| {
            try std.testing.expect(!seen.contains(src));
            try seen.put(src, {});
            total += 1;
        }
    }
    try std.testing.expectEqual(expectedCompileCount(.@"test", true), total);
}

fn appendTuFlags(ctx: *const Ctx, run: *std.Build.Step.Run, g: *const tables.Group, lang: Lang) void {
    const b = ctx.b;
    const defines = if (g.pal) tables.pal_defines else tables.engine_defines;
    for (defines) |d| {
        if (shouldIncludeDefine(d, ctx.profile)) run.addArg(d);
    }
    switch (ctx.profile) {
        .@"test" => {},
        .debug_static => {
            for ([_][]const u8{ "-DDBG=1", "-DDEBUG=1", "-D_DEBUG=1", "-DDBG_DUMP=1", "-DCHAKRA_STATIC_LIBRARY=1" }) |d| run.addArg(d);
        },
        .release_shared => run.addArg("-DNDEBUG"),
    }
    if (!ctx.jit) run.addArg("-DDISABLE_JIT=1");
    if (g.target_define) |d| {
        if (ctx.jit or !std.mem.eql(u8, d, "-D_ENABLE_DYNAMIC_THUNKS=1")) run.addArg(d);
    }

    if (g.pal) run.addPrefixedDirectoryArg("-I", ctx.config_dir);
    const rels = if (g.pal) mac_tables.pal_includes_rel else mac_tables.global_includes_rel;
    for (rels) |dir| run.addArg(b.fmt("-I{s}", .{dir}));
    for (g.extra_includes) |dir| {
        var substituted = false;
        for (gen_markers) |m| {
            if (std.mem.eql(u8, dir, m.marker)) {
                switch (m.which) {
                    .gen => run.addPrefixedDirectoryArg("-I", ctx.gen_dir),
                    .wabt => if (ctx.wabt_dir) |wd| run.addPrefixedDirectoryArg("-I", wd),
                }
                substituted = true;
            }
        }
        if (!substituted) run.addArg(b.fmt("-I{s}", .{dir}));
    }
    if (g.pal) run.addArg("-Ipal/src/include");
    // Static ch compiles TestHooks.cpp, whose BackendApi.h includes
    // EmitBuffer.h; the reference ch target only needs lib/Backend here on
    // the static profile (shared ch gets TestHooks via the dylib group).
    if (g == &tables.ch_group and isStaticProfile(ctx.profile))
    {
        run.addArg("-Ilib/Backend");
        run.addArg("-Ilib/Runtime/ByteCode");
    }
    // The reference passes no ICU -I (system clang searches /usr/include
    // by default); `zig cc` searches only its bundled headers, so the
    // default dir is spelled explicitly. -isystem, not -I: /usr/include
    // carries the full C library, and -I would shadow the bundled
    // headers; user -isystem does not shadow them for zig cc. System
    // clang++ already searches /usr/include after libstdc++/libc++ headers
    // and breaks #include_next if /usr/include is prepended via -isystem.
    if (ctx.sanitize == .none or !std.mem.eql(u8, ctx.icu_include, "/usr/include")) {
        run.addArg("-isystem");
        run.addArg(ctx.icu_include);
    }

    switch (lang) {
        .cxx => {
            run.addArg("-Werror");
            for (tables.warnings) |w| {
                if (ctx.profile == .@"test" or std.mem.startsWith(u8, w, "-Wno-")) run.addArg(w);
            }
            run.addArg("-fno-omit-frame-pointer");
            run.addArg("-fdelayed-template-parsing");
            if (ctx.sanitize != .none) {
                if (ctx.profile == .debug_static) {
                    run.addArg("-g");
                } else {
                    for ([_][]const u8{ "-O1", "-g" }) |f| run.addArg(f);
                }
                run.addArg(ctx.sanitize.fsanitizeFlag());
                if (ctx.sanitize == .undefined) run.addArg("-fno-sanitize=enum,alignment,vptr,pointer-overflow");
            } else {
                switch (ctx.profile) {
                    .@"test" => for ([_][]const u8{ "-O3", "-g" }) |f| run.addArg(f),
                    .debug_static => for ([_][]const u8{ "-g", "-fno-sanitize=undefined" }) |f| run.addArg(f),
                    .release_shared => for ([_][]const u8{ "-O3", "-DNDEBUG" }) |f| run.addArg(f),
                }
            }
            run.addArg("-std=gnu++17");
            if (g.pic == .pie) run.addArg("-fPIE");
            for ([_][]const u8{ "-msse4.2", "-fasm-blocks", "-fms-extensions", "-fwrapv" }) |f| run.addArg(f);
            if (g.pal) for ([_][]const u8{ "-fstack-protector", "-fno-builtin" }) |f| run.addArg(f);
            if (g.pic == .pic) run.addArg("-fPIC");
        },
        .c, .asm_ => {
            if (ctx.sanitize != .none) {
                if (lang == .c) {
                    if (ctx.profile != .debug_static) run.addArg("-O1");
                    for ([_][]const u8{ "-g", "-fno-omit-frame-pointer", ctx.sanitize.fsanitizeFlag() }) |f| run.addArg(f);
                    if (ctx.sanitize == .undefined) run.addArg("-fno-sanitize=enum,alignment,vptr,pointer-overflow");
                } else {
                    run.addArg("-g");
                }
            } else {
                switch (ctx.profile) {
                    .@"test" => for ([_][]const u8{ "-O2", "-g", "-DNDEBUG" }) |f| run.addArg(f),
                    .debug_static => {
                        run.addArg("-g");
                        if (lang == .c) run.addArg("-fno-sanitize=undefined");
                    },
                    .release_shared => for ([_][]const u8{ "-O3", "-DNDEBUG" }) |f| run.addArg(f),
                }
            }
            for ([_][]const u8{ "-msse4.2", "-fasm-blocks", "-fms-extensions", "-fwrapv" }) |f| run.addArg(f);
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
    const run = if (ctx.sanitize != .none) blk: {
        const drv = if (lang == .cxx) ctx.host_cxx else ctx.host_cc;
        break :blk b.addSystemCommand(&.{drv});
    } else blk: {
        const cmd = b.addSystemCommand(&.{ ctx.zig_exe, if (lang == .cxx) "c++" else "cc" });
        cmd.addArg("-target");
        cmd.addArg(ctx.target_triple);
        break :blk cmd;
    };
    appendTuFlags(ctx, run, g, lang);
    // Toolchain-drift parity (documented deviation, B4.3): clang 22
    // emits -Wdate-time for this TU's __DATE__/__TIME__ use (reference
    // clang 18 stays silent with identical flags), which -Werror
    // promotes. The reference has no such suppression because its
    // compiler never fires the warning. Scoped to the sole using TU
    // (verified by repo grep: the only other user,
    // bin/ChakraCore/ChakraCoreDllFunc.cpp, is not in the Linux
    // compile set); -Werror itself is untouched.
    if (lang == .cxx and (ctx.sanitize != .none or std.mem.eql(u8, src, "lib/Jsrt/JsrtHelper.cpp")))
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
    if (g == &tables.wabt_group) if (ctx.wabt_h) |wh| run.addFileInput(wh);
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
        // preserves the template's exact ending -- verified byte-equal).
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

fn registerFailStep(b: *std.Build, python: []const u8, name: []const u8, desc: []const u8, msg: []const u8) void {
    const cmd = b.addSystemCommand(&.{ python, "-c", "import sys; sys.exit(sys.argv[1])", msg });
    cmd.stdio = .inherit;
    const step = b.step(name, desc);
    step.dependOn(&cmd.step);
}

fn addBinArg(b: *std.Build, cmd: *std.Build.Step.Run, side_name: ?[]const u8, prefix_dir: []const u8, sub_path: []const u8, bin_art: std.Build.LazyPath, dep_step: *std.Build.Step, prefixed_flag: ?[]const u8) void {
    if (side_name == null) {
        const installed_art: std.Build.LazyPath = .{ .relative = .{ .base = .install_prefix, .sub_path = sub_path } };
        if (prefixed_flag) |flag| {
            cmd.addPrefixedFileArg(flag, installed_art);
        } else {
            cmd.addFileArg(installed_art);
        }
    } else {
        const bin_path = b.fmt("{s}/{s}", .{ prefix_dir, sub_path });
        if (prefixed_flag) |flag| {
            cmd.addArg(b.fmt("{s}{s}", .{ flag, bin_path }));
        } else {
            cmd.addArg(bin_path);
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
        "import os, shutil, sys, tempfile; src, dst = sys.argv[1], sys.argv[2]; d = os.path.dirname(os.path.abspath(dst)); os.makedirs(d, exist_ok=True); fd, tmp = tempfile.mkstemp(dir=d, prefix='.tmp.'); os.close(fd); shutil.copy2(src, tmp); os.replace(tmp, dst)",
    });
    cp.addFileArg(src);
    cp.addArg(dest_rel);
    cp.has_side_effects = true;
    cp.step.dependOn(prereq);
    return cp;
}

pub fn build(b: *std.Build) void {
    const profile = gateProfile(b);
    const sanitize = gateSanitize(b);
    if (sanitize != .none and isReleaseProfile(profile)) {
        fail("-Dsanitize={s} is not supported with -Dprofile=release-shared on Linux; use test (default) or debug-static", .{sanitize.name()});
    }
    gateOptimize(b, profile);
    const target = gateTarget(b);
    const want_glibc = gateGlibc(b);
    const icu = gateIcu(b);
    const jit = b.option(bool, "jit", "Enable JIT backend (default: true unsanitized, false when -Dsanitize!=none)") orelse defaultJitForSanitize(sanitize);
    const python = b.findProgram(.{ .names = &.{ "python3", "python" } }) orelse
        fail("cannot find python3 or python on PATH; needed to run bin/ch/jstoc.py for DbgController.js.h", .{});
    const host_compilers = if (sanitize != .none) gateHostCompilers(b) else HostCompilers{ .cc = "", .cxx = "" };

    const arena = b.graph.arena;
    const is_static = isStaticProfile(profile);
    const is_release = isReleaseProfile(profile);
    const target_triple = targetTripleForGlibc(arena, want_glibc);
    const side_name = sideNameForConfig(arena, profile, sanitize);
    const prefix_dir = installPrefixForConfig(arena, profile, sanitize);

    if (sanitize != .none) {
        std.debug.print("b4 sanitize={s} profile={s}: host-cxx={s} icu={s},{s} (major {s}) jit={} prefix={s}\n", .{
            sanitize.name(), profile.name(), host_compilers.cxx, icu.include, icu.libdir, icu.major, jit, prefix_dir,
        });
    } else {
        std.debug.print("b4 profile={s}: target={s} glibc={s} icu={s},{s} (major {s}) jit={} zig={f} python={s}\n", .{
            profile.name(), target_triple, want_glibc, icu.include, icu.libdir, icu.major, jit, builtin.zig_version, python,
        });
    }

    // Build-owned pal/src/config.h: 95 real checks incl. native run
    // probes, fail-clearly on deviation.
    const cfg = pal_config.generate(.{
        .io = b.graph.io,
        .zig_exe = b.graph.zig_exe,
        .target = target_triple,
        .profile = profile.name(),
    });
    defer std.heap.page_allocator.free(cfg.config_h);
    if (!cfg.tail_calls) fail("CLANG_HAS_DISABLE_TAIL_CALLS check failed: this profile requires a compiler with __attribute__((disable_tail_calls))", .{});

    const wf = b.addWriteFiles();
    const config_h = wf.add("config.h", arena.dupe(u8, cfg.config_h) catch @panic("OOM"));

    // Build-owned wabt config.h (skipped in Release where CAN_BUILD_WABT is off).
    const wabt_res: struct { dir: ?std.Build.LazyPath, file: ?std.Build.LazyPath } = if (is_release) .{ .dir = null, .file = null } else blk: {
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
        const wh = wabt_wf.add("config.h", wabt_bytes);
        std.debug.print("b4 wabt config.h: {d} bytes from lib/wabt/src/config.h.in\n", .{wabt_bytes.len});
        break :blk .{ .dir = wabt_wf.getDirectory(), .file = wh };
    };

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
        .host_cc = host_compilers.cc,
        .host_cxx = host_compilers.cxx,
        .profile = profile,
        .sanitize = sanitize,
        .jit = jit,
        .target_triple = target_triple,
        .glibc = want_glibc,
        .icu_include = icu.include,
        .icu_libdir = icu.libdir,
        .icu_major = icu.major,
        .config_dir = wf.getDirectory(),
        .config_h = config_h,
        .wabt_dir = wabt_res.dir,
        .wabt_h = wabt_res.file,
        .gen_dir = gen_dir,
        .dbg_header = dbg_header,
    };

    // Compile every TU once, keyed by source path; archives and links
    // assemble objects via the reference order lists (static_order /
    // singular_orders), which carry the initializer order.
    var n: u32 = 0;
    var objs: std.StringHashMap(std.Build.LazyPath) = .init(arena);
    for (all_groups) |g| {
        if (!shouldCompileGroup(g, profile, jit)) continue;
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
    if (is_static) {
        for (mac_tables.ch_static_extra_cxx) |src| {
            objs.put(src, compileTu(&ctx, &tables.ch_group, src, .cxx, n)) catch @panic("OOM");
            n += 1;
        }
    }
    const want_n = expectedCompileCount(profile, jit);
    std.debug.assert(n == want_n);
    std.debug.assert(objs.count() == want_n);

    // Z1 leaf Zig SIMD codec (doc/zig-strategy.md section 5.3). Built outside C/C++ TU count
    // (`want_n`) so compile_commands / contract TU count remains unchanged.
    const u8a_codec_obj = b.addObject(.{
        .name = "u8a_codec",
        .root_module = b.createModule(.{
            .root_source_file = b.path("lib/ZigCodec/u8a_codec.zig"),
            .target = target,
            .optimize = if (profile == .debug_static or sanitize != .none) .debug else .fast,
            .pic = true,
            .link_libc = false,
            .link_libcpp = false,
            .sanitize_c = if (sanitize == .undefined) .full else if (sanitize != .none) .trap else null,
            .sanitize_thread = if (sanitize == .thread) true else null,
        }),
    });

    // Archives via `zig ar` (reference: cmake `ar qc` + ranlib; member
    // order is the reference edge order).
    var archives: std.StringHashMap(std.Build.LazyPath) = .init(arena);
    {
        const ar = b.addSystemCommand(&.{ ctx.zig_exe, "ar", "rcs" });
        const out = ar.addOutputFileArg("libChakraCoreStatic.a");
        for (tables.static_order) |src| {
            if (isExcludedStaticObj(src, profile, jit)) continue;
            const o = objs.get(src) orelse fail("archive ChakraCoreStatic: no object for {s}", .{src});
            ar.addFileArg(o);
        }
        ar.addFileArg(u8a_codec_obj.getEmittedBin());
        archives.put("ChakraCoreStatic", out) catch @panic("OOM");
    }
    if (!is_static) {
        const singular_specs = [_]struct { name: []const u8, file: []const u8, order: []const []const u8 }{
            .{ .name = "pal", .file = "libChakra.Pal.Singular.a", .order = &tables.singular_orders.pal },
            .{ .name = "codex", .file = "libChakra.Common.Codex.Singular.a", .order = &tables.singular_orders.codex },
            .{ .name = "platformagnostic", .file = "libChakra.Runtime.PlatformAgnostic.Singular.a", .order = &tables.singular_orders.platformagnostic },
        };
        // `inline for` on both loops: `&tables.singular_orders.*` are
        // pointers to fields of a const anonymous struct, which have no
        // runtime address.
        inline for (singular_specs) |spec| {
            const ar = b.addSystemCommand(&.{ ctx.zig_exe, "ar", "rcs" });
            const out = ar.addOutputFileArg(spec.file);
            inline for (spec.order) |src| {
                const o = objs.get(src) orelse fail("archive {s}: no object for {s}", .{ spec.name, src });
                ar.addFileArg(o);
            }
            archives.put(spec.name, out) catch @panic("OOM");
        }
    }

    // Links via host clang++ (when -Dsanitize!=none) or the `zig c++` driver.
    const ch_link = if (sanitize != .none) blk: {
        const l = b.addSystemCommand(&.{ctx.host_cxx});
        l.addArg(sanitize.fsanitizeFlag());
        break :blk l;
    } else blk: {
        const l = b.addSystemCommand(&.{ ctx.zig_exe, "c++" });
        l.addArg("-target");
        l.addArg(target_triple);
        break :blk l;
    };
    for ([_][]const u8{ "-fPIE", "-pie", "-Wl,--export-dynamic", "-rdynamic" }) |f| ch_link.addArg(f);
    for (tables.ch_group.cxx) |src| ch_link.addFileArg(objs.get(src).?);
    if (is_static) {
        for (mac_tables.ch_static_extra_cxx) |src| ch_link.addFileArg(objs.get(src).?);
    }
    ch_link.addArg("-o");
    const ch_bin = ch_link.addOutputFileArg("ch");
    if (is_static) {
        ch_link.addArg("-Wl,--start-group");
        ch_link.addFileArg(archives.get("ChakraCoreStatic").?);
        ch_link.addArg("-Wl,--end-group");
    } else {
        for ([_][]const u8{ "pal", "codex", "platformagnostic" }) |a| ch_link.addFileArg(archives.get(a).?);
    }
    ch_link.addArg(b.fmt("-L{s}", .{ctx.icu_libdir}));
    ch_link.addArg("-Wl,--no-as-needed");
    for ([_][]const u8{ "-licuuc", "-licui18n" }) |f| ch_link.addArg(f);
    ch_link.addArg("-Wl,--as-needed");
    if (sanitize == .none) {
        ch_link.addPrefixedFileArg("-Wl,--version-script=", b.path("bin/ch/ch.version"));
    }
    for ([_][]const u8{ "-lpthread", "-ldl" }) |f| ch_link.addArg(f);
    ch_link.step.dependOn(&jstoc.step);

    const so_res: ?struct { bin: std.Build.LazyPath, link: *std.Build.Step.Run } = if (is_static) null else blk: {
        const so_link = if (sanitize != .none) cmd_blk: {
            const l = b.addSystemCommand(&.{ctx.host_cxx});
            l.addArg(sanitize.fsanitizeFlag());
            break :cmd_blk l;
        } else cmd_blk: {
            const l = b.addSystemCommand(&.{ ctx.zig_exe, "c++" });
            l.addArg("-target");
            l.addArg(target_triple);
            break :cmd_blk l;
        };
        for ([_][]const u8{ "-fPIC", "-shared", "-Wl,-soname,libChakraCore.so" }) |f| so_link.addArg(f);
        for (tables.dylib_group.cxx) |src| so_link.addFileArg(objs.get(src).?);
        for (tables.static_order) |src| {
            if (isExcludedStaticObj(src, profile, jit)) continue;
            so_link.addFileArg(objs.get(src).?);
        }
        so_link.addFileArg(u8a_codec_obj.getEmittedBin());
        so_link.addArg("-o");
        const so_bin = so_link.addOutputFileArg("libChakraCore.so");
        so_link.addArg(b.fmt("-L{s}", .{ctx.icu_libdir}));
        so_link.addArg("-Wl,--no-as-needed");
        for ([_][]const u8{ "-licuuc", "-licui18n" }) |f| so_link.addArg(f);
        so_link.addArg("-Wl,--as-needed");
        for ([_][]const u8{ "-lpthread", "-ldl" }) |f| so_link.addArg(f);
        so_link.addPrefixedFileArg("-Wl,--version-script=", b.path("bin/ChakraCore/libChakraCoreLib.version"));
        // Sanitizer shared libs resolve __asan_*/__ubsan_* from the host executable's runtime.
        if (sanitize == .none) so_link.addArg("-Wl,--no-undefined");
        break :blk .{ .bin = so_bin, .link = so_link };
    };

    const gs_link = if (sanitize != .none) blk: {
        const l = b.addSystemCommand(&.{ctx.host_cxx});
        l.addArg(sanitize.fsanitizeFlag());
        break :blk l;
    } else blk: {
        const l = b.addSystemCommand(&.{ ctx.zig_exe, "c++" });
        l.addArg("-target");
        l.addArg(target_triple);
        break :blk l;
    };
    for ([_][]const u8{ "-Wl,--export-dynamic", "-rdynamic" }) |f| gs_link.addArg(f);
    for (tables.gcstress_group.cxx) |src| gs_link.addFileArg(objs.get(src).?);
    gs_link.addArg("-o");
    const gs_bin = gs_link.addOutputFileArg("GCStress");
    gs_link.addArg("-pie");
    gs_link.addArg("-Wl,--start-group");
    gs_link.addFileArg(archives.get("ChakraCoreStatic").?);
    gs_link.addArg("-Wl,--end-group");
    for ([_][]const u8{ "-ldl", "-lpthread" }) |f| gs_link.addArg(f);

    // Install / side-prefix layout: unsanitized test uses zig-out via
    // native install steps; debug-static, release-shared, and sanitizer
    // lanes isolate into out/<side_name>/ via tools/build/install_profile.py.
    const install = b.getInstallStep();
    var copy_out: ?*std.Build.Step.Run = null;
    if (side_name == null) {
        install.dependOn(&b.addInstallFile(ch_bin, "bin/ch").step);
        install.dependOn(&b.addInstallFile(gs_bin, "bin/GCStress").step);
        if (so_res) |sr| {
            install.dependOn(&b.addInstallFile(sr.bin, "bin/libChakraCore.so").step);
            install.dependOn(&b.addInstallFile(sr.bin, "lib/libChakraCore.so").step);
        }
        install.dependOn(&b.addInstallFile(archives.get("ChakraCoreStatic").?, "lib/libChakraCoreStatic.a").step);
        if (!is_static) {
            install.dependOn(&b.addInstallFile(archives.get("pal").?, "lib/libChakra.Pal.Singular.a").step);
            install.dependOn(&b.addInstallFile(archives.get("codex").?, "lib/libChakra.Common.Codex.Singular.a").step);
            install.dependOn(&b.addInstallFile(archives.get("platformagnostic").?, "lib/libChakra.Runtime.PlatformAgnostic.Singular.a").step);
        }
        install.dependOn(&b.addInstallFile(config_h, "pal/src/config.h").step);
        if (ctx.wabt_h) |wh| install.dependOn(&b.addInstallFile(wh, "lib/wabt/config.h").step);
        install.dependOn(&b.addInstallFile(b.path("lib/Jsrt/ChakraCore.h"), "include/ChakraCore.h").step);
        install.dependOn(&b.addInstallFile(b.path("lib/Jsrt/ChakraCommon.h"), "include/ChakraCommon.h").step);
        install.dependOn(&b.addInstallFile(b.path("lib/Jsrt/ChakraDebug.h"), "include/ChakraDebug.h").step);
        install.dependOn(&b.addInstallFile(b.path("lib/Common/ChakraCoreVersion.h"), "include/ChakraCoreVersion.h").step);
        const marker = wf.add(".profile", arena.dupe(u8, "test\n") catch @panic("OOM"));
        install.dependOn(&b.addInstallFile(marker, ".profile").step);
    } else {
        const sname = side_name.?;
        const cp = b.addSystemCommand(&.{python});
        cp.addFileArg(b.path("tools/build/install_profile.py"));
        cp.addArg("--profile");
        cp.addArg(sname);
        cp.addArg("--dest");
        cp.addArg(prefix_dir);
        cp.addPrefixedFileArg("--map=bin/ch=", ch_bin);
        cp.addPrefixedFileArg("--map=bin/GCStress=", gs_bin);
        if (so_res) |sr| {
            cp.addPrefixedFileArg("--map=bin/libChakraCore.so=", sr.bin);
            cp.addPrefixedFileArg("--map=lib/libChakraCore.so=", sr.bin);
        }
        cp.addPrefixedFileArg("--map=lib/libChakraCoreStatic.a=", archives.get("ChakraCoreStatic").?);
        if (!is_static) {
            cp.addPrefixedFileArg("--map=lib/libChakra.Pal.Singular.a=", archives.get("pal").?);
            cp.addPrefixedFileArg("--map=lib/libChakra.Common.Codex.Singular.a=", archives.get("codex").?);
            cp.addPrefixedFileArg("--map=lib/libChakra.Runtime.PlatformAgnostic.Singular.a=", archives.get("platformagnostic").?);
        }
        cp.addPrefixedFileArg("--map=pal/src/config.h=", config_h);
        if (ctx.wabt_h) |wh| cp.addPrefixedFileArg("--map=lib/wabt/config.h=", wh);
        cp.addPrefixedFileArg("--map=include/ChakraCore.h=", b.path("lib/Jsrt/ChakraCore.h"));
        cp.addPrefixedFileArg("--map=include/ChakraCommon.h=", b.path("lib/Jsrt/ChakraCommon.h"));
        cp.addPrefixedFileArg("--map=include/ChakraDebug.h=", b.path("lib/Jsrt/ChakraDebug.h"));
        cp.addPrefixedFileArg("--map=include/ChakraCoreVersion.h=", b.path("lib/Common/ChakraCoreVersion.h"));
        cp.has_side_effects = true;
        cp.stdio = .inherit;
        install.dependOn(&cp.step);
        copy_out = cp;

        const rm = b.addSystemCommand(&.{ python, "-c", "import shutil,sys; shutil.rmtree(sys.argv[1], ignore_errors=True); print('removed ' + sys.argv[1] + ' (or already absent)')" });
        const root = b.root.root_dir.path orelse
            fail("cannot resolve the build root path; refusing a CWD-relative uninstall", .{});
        rm.addArg(b.fmt("{s}/{s}", .{ root, prefix_dir }));
        rm.stdio = .inherit;
        rm.has_side_effects = true;
        b.getUninstallStep().dependOn(&rm.step);
    }

    const artifacts_ready: *std.Build.Step = if (copy_out) |cp| &cp.step else install;

    // Node-API provider, check-nodeapi, and ch-nodeapi (shared profiles only).
    var prov_step_opt: ?*std.Build.Step = null;
    var nodeapi_step_opt: ?*std.Build.Step = null;
    if (so_res) |sr| {
        const so_bin = sr.bin;
        const addNodeapiTuDepFlags = struct {
            fn call(context: *const Ctx, tu: *std.Build.Step.Run, pic_flags: []const []const u8, dep_name: []const u8, include_internal: bool) void {
                if (context.sanitize != .none) {
                    tu.addArg("-std=gnu++17");
                    if (context.profile != .debug_static) tu.addArg("-O1");
                    tu.addArgs(&.{ "-g", "-fno-omit-frame-pointer", context.sanitize.fsanitizeFlag() });
                } else {
                    tu.addArgs(&.{ "-target", context.target_triple, "-std=gnu++17", "-O3", "-g" });
                }
                for (pic_flags) |f| tu.addArg(f);
                tu.addArgs(&.{ "-DNAPI_VERSION=10", "-Iinclude/nodeapi" });
                if (include_internal) {
                    tu.addArgs(&.{ "-Ilib/Jsrt", "-Ilib/NodeApi" });
                    if (context.sanitize == .none or !std.mem.eql(u8, context.icu_include, "/usr/include")) {
                        tu.addArg("-isystem");
                        tu.addArg(context.icu_include);
                    }
                    tu.addArg("-Wno-date-time");
                }
                tu.addArgs(&.{ "-MD", "-MF" });
                _ = tu.addDepFileOutputArg(dep_name);
            }
        }.call;

        const compileNodeapiObj = struct {
            fn call(
                context: *const Ctx,
                src_rel: []const u8,
                obj_name: []const u8,
                dep_name: []const u8,
                pic_flags: []const []const u8,
                include_internal: bool,
            ) std.Build.LazyPath {
                const tu = if (context.sanitize != .none)
                    context.b.addSystemCommand(&.{context.host_cxx})
                else
                    context.b.addSystemCommand(&.{ context.zig_exe, "c++" });
                addNodeapiTuDepFlags(context, tu, pic_flags, dep_name, include_internal);
                tu.addArg("-o");
                const out = tu.addOutputFileArg(obj_name);
                tu.addArg("-c");
                tu.addFileArg(context.b.path(src_rel));
                return out;
            }
        }.call;

        var prov_objs: std.ArrayList(std.Build.LazyPath) = .empty;
        for (mac_tables.nodeapi_provider_tus) |src| {
            const base = std.fs.path.basename(src);
            const out = compileNodeapiObj(
                &ctx,
                src,
                b.fmt("{s}.o", .{base}),
                b.fmt("nodeapi-prov-{s}.d", .{base}),
                &.{ "-fPIC", "-fvisibility=hidden" },
                true,
            );
            prov_objs.append(arena, out) catch @panic("OOM");
        }
        const prov_link = if (sanitize != .none) cmd_blk: {
            const l = b.addSystemCommand(&.{ctx.host_cxx});
            l.addArg(sanitize.fsanitizeFlag());
            break :cmd_blk l;
        } else cmd_blk: {
            const l = b.addSystemCommand(&.{ ctx.zig_exe, "c++" });
            l.addArg("-target");
            l.addArg(target_triple);
            break :cmd_blk l;
        };
        for ([_][]const u8{ "-fPIC", "-shared", "-Wl,-soname,libChakraNodeApi.so" }) |f| prov_link.addArg(f);
        prov_link.addArg("-Wl,-rpath,$ORIGIN");
        prov_link.addArg("-Wl,-rpath,$ORIGIN/../lib");
        for (prov_objs.items) |o| prov_link.addFileArg(o);
        prov_link.addFileArg(so_bin);
        prov_link.addArg("-o");
        const prov_so = prov_link.addOutputFileArg("libChakraNodeApi.so");
        for ([_][]const u8{ "-lpthread", "-ldl" }) |f| prov_link.addArg(f);
        if (sanitize == .none) prov_link.addArg("-Wl,--no-undefined");
        prov_link.step.dependOn(&sr.link.step);

        const prov_step = b.step("libChakraNodeApi", "Build libChakraNodeApi (N1.1 provider) linked against libChakraCore (not part of the default build)");
        prov_step_opt = prov_step;
        if (side_name == null) {
            install.dependOn(&b.addInstallFile(prov_so, "lib/libChakraNodeApi.so").step);
            prov_step.dependOn(&b.addInstallFile(prov_so, "lib/libChakraNodeApi.so").step);
            for ([_][]const u8{ "js_native_api.h", "js_native_api_types.h", "node_api.h", "node_api_types.h" }) |hdr| {
                prov_step.dependOn(&b.addInstallFile(
                    b.path(b.fmt("include/nodeapi/{s}", .{hdr})),
                    b.fmt("include/nodeapi/{s}", .{hdr}),
                ).step);
            }
        } else {
            prov_step.dependOn(&addSideCopy(b, python, b.fmt("{s}/lib/libChakraNodeApi.so", .{prefix_dir}), prov_so, artifacts_ready).step);
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
        exports_gen_check.addArg("--check");
        exports_gen_check.stdio = .inherit;
        const exports_check = b.addSystemCommand(&.{python});
        exports_check.addFileArg(b.path("tools/build/check_nodeapi.py"));
        exports_check.addPrefixedFileArg("--dylib=", prov_so);
        exports_check.addPrefixedFileArg("--exports=", b.path("tools/build/nodeapi_exports.txt"));
        exports_check.addPrefixedFileArg("--inventory=", b.path("doc/node-api-inventory.md"));
        exports_check.step.dependOn(&prov_link.step);
        exports_check.step.dependOn(&exports_gen_check.step);
        exports_check.stdio = .inherit;
        const check_nodeapi_top = b.step("check-nodeapi", "Verify libChakraNodeApi exports exactly tools/build/nodeapi_exports.txt (full 155 + 6 = 161; not part of the default build or check)");
        check_nodeapi_top.dependOn(&exports_gen_check.step);
        check_nodeapi_top.dependOn(&exports_check.step);

        const addNodeapiLinkPrefix = struct {
            fn call(context: *const Ctx, cmd: *std.Build.Step.Run, extra: []const []const u8) void {
                if (context.sanitize != .none) {
                    cmd.addArg("-std=gnu++17");
                    if (context.profile != .debug_static) cmd.addArg("-O1");
                    cmd.addArgs(&.{ "-g", "-fno-omit-frame-pointer", context.sanitize.fsanitizeFlag() });
                } else {
                    cmd.addArgs(&.{ "-target", context.target_triple, "-std=gnu++17", "-O3", "-g" });
                }
                for (extra) |f| cmd.addArg(f);
            }
        }.call;

        const nodeapi_host_obj = compileNodeapiObj(
            &ctx,
            "tools/nodeapi/ch-nodeapi.cpp",
            "ch-nodeapi.cpp.o",
            "nodeapi-host.d",
            &.{"-fPIE"},
            true,
        );
        const nodeapi_host = if (sanitize != .none) b.addSystemCommand(&.{ctx.host_cxx}) else b.addSystemCommand(&.{ ctx.zig_exe, "c++" });
        addNodeapiLinkPrefix(&ctx, nodeapi_host, &.{"-pie"});
        if (sanitize != .none) {
            nodeapi_host.addArg("-Wl,--export-dynamic");
            nodeapi_host.addArg("-rdynamic");
        }
        nodeapi_host.addArg("-o");
        const nodeapi_host_bin = nodeapi_host.addOutputFileArg("ch-nodeapi");
        nodeapi_host.addFileArg(nodeapi_host_obj);
        nodeapi_host.addFileArg(so_bin);
        nodeapi_host.addArg("-Wl,-rpath,$ORIGIN");
        nodeapi_host.addArg("-Wl,-rpath,$ORIGIN/../lib");
        for ([_][]const u8{ "-ldl", "-lpthread" }) |f| nodeapi_host.addArg(f);
        nodeapi_host.step.dependOn(&sr.link.step);

        const nodeapi_stub_obj = compileNodeapiObj(
            &ctx,
            "tools/nodeapi/stub/stub_provider.cpp",
            "stub_provider.cpp.o",
            "nodeapi-stub.d",
            &.{"-fPIC"},
            true,
        );
        const nodeapi_stub = if (sanitize != .none) b.addSystemCommand(&.{ctx.host_cxx}) else b.addSystemCommand(&.{ ctx.zig_exe, "c++" });
        addNodeapiLinkPrefix(&ctx, nodeapi_stub, &.{"-shared"});
        nodeapi_stub.addArg("-o");
        const nodeapi_stub_bin = nodeapi_stub.addOutputFileArg("libChakraNodeApiStub.so");
        nodeapi_stub.addFileArg(nodeapi_stub_obj);

        const nodeapi_step = b.step("ch-nodeapi", "Build the Node-API test host (N0.3): ch-nodeapi + test-only stub provider + hello/negative addons (shared profiles only; not part of the default build)");
        nodeapi_step_opt = nodeapi_step;
        if (side_name == null) {
            nodeapi_step.dependOn(&b.addInstallFile(nodeapi_host_bin, "bin/ch-nodeapi").step);
            nodeapi_step.dependOn(&b.addInstallFile(prov_so, "lib/libChakraNodeApi.so").step);
            nodeapi_step.dependOn(&b.addInstallFile(nodeapi_stub_bin, "lib/libChakraNodeApiStub.so").step);
        } else {
            nodeapi_step.dependOn(&addSideCopy(b, python, b.fmt("{s}/bin/ch-nodeapi", .{prefix_dir}), nodeapi_host_bin, artifacts_ready).step);
            nodeapi_step.dependOn(&addSideCopy(b, python, b.fmt("{s}/lib/libChakraNodeApi.so", .{prefix_dir}), prov_so, artifacts_ready).step);
            nodeapi_step.dependOn(&addSideCopy(b, python, b.fmt("{s}/lib/libChakraNodeApiStub.so", .{prefix_dir}), nodeapi_stub_bin, artifacts_ready).step);
            for ([_][]const u8{ "js_native_api.h", "js_native_api_types.h", "node_api.h", "node_api_types.h" }) |hdr| {
                nodeapi_step.dependOn(&addSideCopy(
                    b,
                    python,
                    b.fmt("{s}/include/nodeapi/{s}", .{ prefix_dir, hdr }),
                    b.path(b.fmt("include/nodeapi/{s}", .{hdr})),
                    artifacts_ready,
                ).step);
            }
        }
        for ([_][]const u8{ "hello_world", "version_reject", "missing_register", "promise_probe", "return_replaces" }) |name| {
            const addon_obj = compileNodeapiObj(
                &ctx,
                b.fmt("tools/nodeapi/addons/{s}.cpp", .{name}),
                b.fmt("{s}.cpp.o", .{name}),
                b.fmt("nodeapi-addon-{s}.d", .{name}),
                &.{"-fPIC"},
                false,
            );
            const addon = if (sanitize != .none) b.addSystemCommand(&.{ctx.host_cxx}) else b.addSystemCommand(&.{ ctx.zig_exe, "c++" });
            addNodeapiLinkPrefix(&ctx, addon, &.{"-shared"});
            addon.addArg("-o");
            const addon_bin = addon.addOutputFileArg(b.fmt("{s}.node", .{name}));
            addon.addFileArg(addon_obj);
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
        registerFailStep(b, python, "libChakraNodeApi", "Not applicable on debug-static (requires a shared profile)", "-Dprofile=debug-static has no shared libChakraCore.so; Node-API provider requires a shared profile (test or release-shared)");
        registerFailStep(b, python, "check-nodeapi", "Not applicable on debug-static (requires a shared profile)", "-Dprofile=debug-static has no shared libChakraCore.so; check-nodeapi requires a shared profile (test or release-shared)");
        registerFailStep(b, python, "ch-nodeapi", "Not applicable on debug-static (requires a shared profile)", "-Dprofile=debug-static has no shared libChakraCore.so; ch-nodeapi requires a shared profile (test or release-shared)");
    }

    // Negative controls for the Linux gates (mirrors check-selftest).
    const selftest_top = b.step("check-selftest", "Negative controls for every Linux check gate (not part of the default build)");
    for ([_][]const u8{ "check_regression_linux.py", "check_smoke_linux.py", "check_metadata_linux.py", "check_regression.py", "check_smoke.py", "check_native.py", "check_nodeapi.py", "gen_nodeapi_exports.py", "check_wasm_p1.py", "check_gcstress.py", "check_fulljit.py", "check_sanitize.py" }) |script| {
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
    const gate_tests = b.addTest(.{
        .root_module = b.createModule(.{
            .root_source_file = b.path("tools/linux-gate-tests.zig"),
            .target = b.graph.host,
            .optimize = .debug,
        }),
    });
    selftest_top.dependOn(&b.addRunArtifact(gate_tests).step);
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
            .root_source_file = b.path("lib/ZigCodec/u8a_codec.zig"),
            .target = b.graph.host,
            .optimize = .debug,
        }),
    });
    selftest_top.dependOn(&b.addRunArtifact(u8a_codec_tests).step);
    const tools_test_top = b.step("check-build-tools", "Failure-path tests for the verification tools (no engine build required)");
    tools_test_top.dependOn(&tools_test.step);

    // (1) Bytecode check & (2) Regression suite.
    var verify_bc_step: ?*std.Build.Step = null;
    var regression_step: ?*std.Build.Step = null;
    if (is_release) {
        registerFailStep(b, python, "verify-bytecode", "Not applicable: Release has no bytecode leg", "-Dprofile=release-shared: no bytecode leg (Release strips debug/bytecode tools)");
        registerFailStep(b, python, "check-regression", "Not applicable: Release is a smoke gate only", "-Dprofile=release-shared: no regression leg (Release is smoke + native + metadata only)");
    } else {
        const verify_bc = b.addSystemCommand(&.{python});
        verify_bc.addFileArg(b.path("tools/build/verify_bytecode_pure.py"));
        verify_bc.addFileArg(b.path("tools/regenByteCode.py"));
        verify_bc.addArg(if (jit) "--jit" else "--noJit");
        verify_bc.addArg("--verify");
        addBinArg(b, verify_bc, side_name, prefix_dir, "bin/ch", ch_bin, artifacts_ready, "--binary=");
        verify_bc.stdio = .inherit;
        const verify_bc_top = b.step("verify-bytecode", "Verify checked-in built-in bytecode with the just-built ch (not part of the default build)");
        verify_bc_top.dependOn(&verify_bc.step);
        verify_bc_step = &verify_bc.step;

        const regression = b.addSystemCommand(&.{python});
        if (profile == .@"test" and jit) {
            regression.addFileArg(b.path("tools/build/check_regression_linux.py"));
            regression.addFileArg(b.path("test/runtests.py"));
            addBinArg(b, regression, side_name, prefix_dir, "bin/ch", ch_bin, artifacts_ready, null);
        } else {
            regression.addFileArg(b.path("tools/build/check_regression.py"));
            regression.addFileArg(b.path("test/runtests.py"));
            addBinArg(b, regression, side_name, prefix_dir, "bin/ch", ch_bin, artifacts_ready, null);
            regression.addArg("--select");
            regression.addArg(if (is_static) "debug" else "test");
        }
        regression.stdio = .inherit;
        const regression_top = b.step("check-regression", "Run the regression suite against the zig-built ch (not part of the default build)");
        regression_top.dependOn(&regression.step);
        regression_step = &regression.step;
    }

    // (3) Native embedding test (per-profile build-type / static flags).
    const cc = b.findProgram(.{ .names = &.{"clang"} }) orelse "clang";
    const cxx = b.findProgram(.{ .names = &.{"clang++"} }) orelse "clang++";
    const native = b.addSystemCommand(&.{python});
    native.addFileArg(b.path("tools/build/check_native.py"));
    native.addFileArg(b.path("test/native-tests/test_native.sh"));
    addBinArg(b, native, side_name, prefix_dir, "bin/ch", ch_bin, artifacts_ready, null);
    native.addArg(if (is_static) b.fmt("{s} c++", .{ctx.zig_exe}) else cc);
    native.addArg(cxx);
    if (is_static) {
        native.addArg("--static");
        native.addArg("--archive");
        native.addArg(b.fmt("{s}/lib/libChakraCoreStatic.a", .{prefix_dir}));
        native.addFileInput(archives.get("ChakraCoreStatic").?);
    } else {
        native.addArg("--build-type");
        native.addArg(if (is_release) "Release" else "RelWithDebInfo");
    }
    native.stdio = .inherit;
    const native_top = b.step("check-native", "Run the native embedding test against zig artifacts (not part of the default build)");
    native_top.dependOn(&native.step);

    // (4) Smoke/flag output assertions.
    const smoke = b.addSystemCommand(&.{python});
    if (profile == .@"test") {
        smoke.addFileArg(b.path("tools/build/check_smoke_linux.py"));
        addBinArg(b, smoke, side_name, prefix_dir, "bin/ch", ch_bin, artifacts_ready, null);
    } else {
        smoke.addFileArg(b.path("tools/build/check_smoke.py"));
        addBinArg(b, smoke, side_name, prefix_dir, "bin/ch", ch_bin, artifacts_ready, null);
        smoke.addArg("--profile");
        smoke.addArg(profile.name());
    }
    smoke.stdio = .inherit;
    const smoke_top = b.step("check-smoke", "Assert ch version/flag output text and script execution (not part of the default build)");
    smoke_top.dependOn(&smoke.step);

    // (4b) JIT smoke (active whenever jit is enabled; fails loudly when -Djit=false).
    var jit_smoke_step: ?*std.Build.Step = null;
    if (jit) {
        const jit_smoke = b.addSystemCommand(&.{python});
        jit_smoke.addFileArg(b.path("tools/build/check_jit.py"));
        addBinArg(b, jit_smoke, side_name, prefix_dir, "bin/ch", ch_bin, artifacts_ready, null);
        jit_smoke.stdio = .inherit;
        const jit_top = b.step("check-jit", "JIT smoke: NativeCodeGenerator present; -mic and dynapogo class/helper checks (not part of the default build)");
        jit_top.dependOn(&jit_smoke.step);
        jit_smoke_step = &jit_smoke.step;
    } else {
        registerFailStep(b, python, "check-jit", "Not applicable when -Djit=false", "-Djit=false built without lib/Backend; check-jit requires JIT enabled");
    }

    // (4c) Wasm P1 gate (requires JIT + non-release profile): interpreted + native JIT tier-up.
    if (supportsWasmGate(profile, jit)) {
        const wasm_gate = b.addSystemCommand(&.{python});
        wasm_gate.addFileArg(b.path("tools/build/check_wasm_p1.py"));
        wasm_gate.addFileArg(b.path("test/runtests.py"));
        addBinArg(b, wasm_gate, side_name, prefix_dir, "bin/ch", ch_bin, artifacts_ready, null);
        wasm_gate.stdio = .inherit;
        const wasm_top = b.step("check-wasm", "Verify Wasm P1 suite against test/wasm-arm64-manifest.json (not part of the default build)");
        wasm_top.dependOn(&wasm_gate.step);

        const wasm_jit_gate = b.addSystemCommand(&.{python});
        wasm_jit_gate.addFileArg(b.path("tools/build/check_wasm_p1.py"));
        wasm_jit_gate.addArg("--jit");
        wasm_jit_gate.addFileArg(b.path("test/runtests.py"));
        addBinArg(b, wasm_jit_gate, side_name, prefix_dir, "bin/ch", ch_bin, artifacts_ready, null);
        wasm_jit_gate.stdio = .inherit;
        const wasm_jit_top = b.step("check-wasm-jit", "Verify Wasm P1 suite with native JIT tier-up (--jit dynapogo; not part of the default build)");
        wasm_jit_top.dependOn(&wasm_jit_gate.step);
    } else {
        registerFailStep(b, python, "check-wasm", "Not applicable without -Djit=true and a non-release profile", "check-wasm requires JIT enabled and -Dprofile=test or debug-static");
        registerFailStep(b, python, "check-wasm-jit", "Not applicable without -Djit=true and a non-release profile", "check-wasm-jit requires JIT enabled and -Dprofile=test or debug-static");
    }

    // (4d) Bounded GCStress heap-walk smoke gate.
    {
        const gc_step = b.addSystemCommand(&.{python});
        gc_step.addFileArg(b.path("tools/build/check_gcstress.py"));
        addBinArg(b, gc_step, side_name, prefix_dir, "bin/GCStress", gs_bin, artifacts_ready, null);
        gc_step.stdio = .inherit;
        const gc_top = b.step("check-gcstress", "Run bounded GCStress in-thread and concurrent heap walks (not part of the default build)");
        gc_top.dependOn(&gc_step.step);
    }

    // (4e) No-profile FullJit gate (B4-JIT-05; requires JIT + non-release).
    if (supportsFullJitGate(profile, jit)) {
        const fj_step = b.addSystemCommand(&.{python});
        fj_step.addFileArg(b.path("tools/build/check_fulljit.py"));
        fj_step.addFileArg(b.path("test/runtests.py"));
        addBinArg(b, fj_step, side_name, prefix_dir, "bin/ch", ch_bin, artifacts_ready, null);
        fj_step.addArg("--select");
        fj_step.addArg(if (is_static) "debug" else "test");
        fj_step.stdio = .inherit;
        const fj_top = b.step("check-fulljit", "Run no-profile FullJit regression suites (not part of the default build)");
        fj_top.dependOn(&fj_step.step);
    } else {
        registerFailStep(b, python, "check-fulljit", "Not applicable without JIT enabled on test/debug-static", "check-fulljit requires -Djit=true and -Dprofile=test or debug-static");
    }

    // (4f) Sanitizer verification gate (requires -Dsanitize=address|undefined|thread).
    if (sanitize != .none) {
        const san_step = b.addSystemCommand(&.{python});
        san_step.addFileArg(b.path("tools/build/check_sanitize.py"));
        san_step.addArg(prefix_dir);
        san_step.addArg("--mode");
        san_step.addArg(sanitize.name());
        san_step.addArg("--profile");
        san_step.addArg(profile.name());
        san_step.addArg(if (supportsWasmGate(profile, jit)) "--expect-wasm" else "--no-expect-wasm");
        san_step.addArg("--cxx");
        san_step.addArg(ctx.host_cxx);
        san_step.addFileInput(ch_bin);
        san_step.step.dependOn(artifacts_ready);
        if (prov_step_opt) |ps| san_step.step.dependOn(ps);
        if (nodeapi_step_opt) |ns| san_step.step.dependOn(ns);
        san_step.stdio = .inherit;
        const san_top = b.step("check-sanitize", b.fmt("Run sanitizer verification gate for -Dsanitize={s} (not part of the default build)", .{sanitize.name()}));
        san_top.dependOn(&san_step.step);
    } else {
        registerFailStep(b, python, "check-sanitize", "Not applicable without -Dsanitize=address|undefined|thread", "check-sanitize requires -Dsanitize=address, undefined, or thread");
    }

    // (5) Metadata gates: ELF, SONAME/NEEDED, exports, configs, headers, archives, .profile.
    const metadata = b.addSystemCommand(&.{python});
    metadata.addFileArg(b.path("tools/build/check_metadata_linux.py"));
    if (side_name == null) {
        metadata.addDirectoryArg(.{ .relative = .{ .base = .install_prefix } });
    } else {
        metadata.addArg(prefix_dir);
    }
    for (metadataArgsForConfig(arena, profile, sanitize, icu.major, jit)) |arg| {
        metadata.addArg(arg);
    }
    metadata.step.dependOn(artifacts_ready);
    metadata.stdio = .inherit;
    const metadata_top = b.step("check-metadata", "Assert artifact metadata: ELF, SONAME, exports, configs, headers, archives, .profile (not part of the default build)");
    metadata_top.dependOn(&metadata.step);

    const check_top = b.step("check", "Full verification for the selected Linux profile and JIT setting (not part of the default build)");
    check_top.dependOn(selftest_top);
    if (regression_step) |s| check_top.dependOn(s);
    if (verify_bc_step) |s| check_top.dependOn(s);
    check_top.dependOn(&native.step);
    check_top.dependOn(&smoke.step);
    if (jit_smoke_step) |s| check_top.dependOn(s);
    check_top.dependOn(&metadata.step);

    // Release Packaging & verification (b5-package)
    if (supportsPackageGate(profile)) {
        const pkg_profile = side_name orelse "test";
        const pkg_out_dir = b.fmt("out/package/{s}", .{pkg_profile});

        const pkg_cmd = b.addSystemCommand(&.{python});
        pkg_cmd.addFileArg(b.path("tools/package/build_package.py"));
        pkg_cmd.addArg("--prefix");
        pkg_cmd.addArg(prefix_dir);
        pkg_cmd.addArg("--profile");
        pkg_cmd.addArg(pkg_profile);
        pkg_cmd.addArg("--out-dir");
        pkg_cmd.addArg(pkg_out_dir);
        pkg_cmd.step.dependOn(artifacts_ready);
        if (prov_step_opt) |ps| pkg_cmd.step.dependOn(ps);
        if (nodeapi_step_opt) |ns| pkg_cmd.step.dependOn(ns);
        pkg_cmd.stdio = .inherit;
        pkg_cmd.has_side_effects = true;

        const pkg_top = b.step("package", b.fmt("Build release archive and MANIFEST.json in {s} (not part of the default build)", .{pkg_out_dir}));
        pkg_top.dependOn(&pkg_cmd.step);

        const verify_cmd = b.addSystemCommand(&.{python});
        verify_cmd.addFileArg(b.path("tools/package/verify_package.py"));
        verify_cmd.addArg(pkg_out_dir);
        verify_cmd.addArg("--profile");
        verify_cmd.addArg(pkg_profile);
        verify_cmd.step.dependOn(&pkg_cmd.step);
        verify_cmd.stdio = .inherit;
        verify_cmd.has_side_effects = true;

        const test_cmd = b.addSystemCommand(&.{python});
        test_cmd.addFileArg(b.path("tools/package/test_package.py"));
        test_cmd.addArg(pkg_out_dir);
        test_cmd.addArg("--profile");
        test_cmd.addArg(pkg_profile);
        test_cmd.step.dependOn(&verify_cmd.step);
        test_cmd.stdio = .inherit;
        test_cmd.has_side_effects = true;

        const check_pkg_top = b.step("check-package", "Verify package creation, manifest integrity, and end-to-end execution of packaged binaries (not part of the default build)");
        check_pkg_top.dependOn(&test_cmd.step);
    } else {
        registerFailStep(b, python, "package", "Not applicable on debug-static", "-Dprofile=debug-static has no shared libChakraCore.so; package requires a shared profile (test or release-shared)");
        registerFailStep(b, python, "check-package", "Not applicable on debug-static", "-Dprofile=debug-static has no shared libChakraCore.so; check-package requires a shared profile (test or release-shared)");
    }
}

test "linux sanitizer and gate helper contracts" {
    try std.testing.expect(uvernumMatchesMajor("#define U_ICU_VERSION_MAJOR_NUM 74\n#define U_ICU_VERSION_SHORT \"74\"\n", "74"));
    try std.testing.expect(!uvernumMatchesMajor("#define U_ICU_VERSION_MAJOR_NUM 79\n#define U_ICU_VERSION_SHORT \"74\"\n", "74"));
    try std.testing.expect(sonameMatchesMajor("libicuuc.so.74", "libicuuc", "74"));
    try std.testing.expect(sonameMatchesMajor("libicui18n.so.74.2", "libicui18n", "74"));
    try std.testing.expect(!sonameMatchesMajor("libicuuc.so.79", "libicuuc", "74"));
    try std.testing.expect(!sonameMatchesMajor("libicuuc.so.740", "libicuuc", "74"));

    // Synthetic minimal ELF64 LE header + dynamic + dynstr verifying DT_SONAME parsing.
    var ehdr = std.mem.zeroes(std.elf.Elf64_Ehdr);
    @memcpy(ehdr.e_ident[0..4], std.elf.MAGIC);
    ehdr.e_ident[std.elf.EI_CLASS] = std.elf.ELFCLASS64;
    ehdr.e_ident[std.elf.EI_DATA] = std.elf.ELFDATA2LSB;
    ehdr.e_shnum = 1;
    var shdr = std.mem.zeroes(std.elf.Elf64_Shdr);
    shdr.sh_type = std.elf.SHT_DYNAMIC;
    var dyns: [2]std.elf.Elf64_Dyn = .{
        .{ .d_tag = std.elf.DT_SONAME, .d_val = 1 },
        .{ .d_tag = std.elf.DT_NULL, .d_val = 0 },
    };
    const dynstr = "\x00libicuuc.so.74\x00";
    const parsed_soname = parseElf64SonameFromSlice(
        std.mem.asBytes(&ehdr),
        std.mem.asBytes(&shdr),
        std.mem.sliceAsBytes(&dyns),
        dynstr,
    );
    try std.testing.expectEqualStrings("libicuuc.so.74", parsed_soname.?);
    try std.testing.expectEqual(Sanitize.none, parseSanitize("none").?);
    try std.testing.expectEqual(Sanitize.address, parseSanitize("address").?);
    try std.testing.expectEqual(Sanitize.undefined, parseSanitize("undefined").?);
    try std.testing.expectEqual(Sanitize.thread, parseSanitize("thread").?);
    try std.testing.expect(parseSanitize("memory") == null);

    try std.testing.expect(defaultJitForSanitize(.none));
    try std.testing.expect(!defaultJitForSanitize(.address));
    try std.testing.expect(!defaultJitForSanitize(.undefined));
    try std.testing.expect(!defaultJitForSanitize(.thread));

    try std.testing.expect(sanitizeCompileFlag(.none) == null);
    try std.testing.expectEqualStrings("-fsanitize=address", sanitizeCompileFlag(.address).?);
    try std.testing.expectEqualStrings("-fsanitize=undefined", sanitizeCompileFlag(.undefined).?);
    try std.testing.expectEqualStrings("-fsanitize=thread", sanitizeCompileFlag(.thread).?);

    try std.testing.expect(supportsWasmGate(.@"test", true));
    try std.testing.expect(supportsWasmGate(.debug_static, true));
    try std.testing.expect(!supportsWasmGate(.release_shared, true));
    try std.testing.expect(!supportsWasmGate(.@"test", false));

    try std.testing.expect(supportsFullJitGate(.@"test", true));
    try std.testing.expect(!supportsFullJitGate(.release_shared, true));
    try std.testing.expect(!supportsFullJitGate(.@"test", false));

    try std.testing.expect(supportsPackageGate(.@"test"));
    try std.testing.expect(supportsPackageGate(.release_shared));
    try std.testing.expect(!supportsPackageGate(.debug_static));

    const arena = std.testing.allocator;
    try std.testing.expect(sideNameForConfig(arena, .@"test", .none) == null);
    try std.testing.expectEqualStrings("debug-static", sideNameForConfig(arena, .debug_static, .none).?);
    const san_side = sideNameForConfig(arena, .@"test", .address).?;
    defer arena.free(san_side);
    try std.testing.expectEqualStrings("sanitize-address", san_side);

    try std.testing.expectEqualStrings("zig-out", installPrefixForConfig(arena, .@"test", .none));
    const dbg_pref = installPrefixForConfig(arena, .debug_static, .none);
    defer arena.free(dbg_pref);
    try std.testing.expectEqualStrings("out/debug-static", dbg_pref);
    const san_pref = installPrefixForConfig(arena, .@"test", .undefined);
    defer arena.free(san_pref);
    try std.testing.expectEqualStrings("out/sanitize-undefined", san_pref);

    const meta_prof_asan = metadataProfileForConfig(arena, .@"test", .address);
    defer arena.free(meta_prof_asan);
    try std.testing.expectEqualStrings("sanitize-address", meta_prof_asan);
    try std.testing.expectEqualStrings("debug-static", metadataProfileForConfig(arena, .debug_static, .address));
    try std.testing.expectEqualStrings("test", metadataProfileForConfig(arena, .@"test", .none));
}
