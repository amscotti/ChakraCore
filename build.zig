//-------------------------------------------------------------------------------------------------------
// Copyright (c) ChakraCore Project Contributors. All rights reserved.
// Licensed under the MIT license. See LICENSE.txt file in the project root for full license information.
//-------------------------------------------------------------------------------------------------------
//! B2.2/B2.3: full Mac compile+link graph - all 24 source groups, static
//! archive(s), libChakraCore.dylib, dynamic ch, GCStress, staged headers -
//! plus the B2.3 `check` aggregation (regression, bytecode, native
//! embedding, smoke, metadata) and `check-selftest` negative controls.
//!
//! B3.1 adds `-Dprofile=test|debug-static|release-shared` (default test).
//! The test profile is the B2 graph bit-identical: native ARM64 macOS,
//! shared/no-JIT, RelWithDebInfo Test, system ICU, macOS minimum 26.0,
//! MacOSX SDK 26.5, Zig 0.17.0. debug-static (Debug + static embedding, no
//! dylib, ch links the whole engine) and release-shared (Release -O3
//! -DNDEBUG, smoke-gate checks) select their legacy-exact defines/flags/
//! graph shape; their full green is B3.2/B3.3's job. `-Dprofile=jit` is
//! the worktree ARM64 macOS JIT port: the test graph with DISABLE_JIT
//! removed and the ARM64 backend group added. It installs to `out/jit/`
//! and does not replace the test profile's zig-out tree. Anything else
//! fails before doing work. B3.4 isolation: test installs natively into
//! `--prefix` (default zig-out); non-test profiles copy out to fixed
//! `out/<profile>/` side prefixes (build.zig cannot read --prefix in
//! 0.17, so fixed destinations are the only enforceable isolation).
//! Switching flavors never touches another profile's outputs; the
//! ordering-correctness proof (PAL deviation dispositioned) lives in
//! doc/b2-verification.md. B3.4 also tracks header dependencies via
//! native depfiles, so touching a header rebuilds exactly its
//! dependent TUs.
//!
//! Contracts: doc/build-contract-mac.md (sources/flags/includes/archives/links),
//! doc/b3-profiles.md (per-profile defines/flags/graph/check mapping),
//! doc/pal-config-contract-mac.md (config.h checks), doc/build.md sections 13-14
//! (pilot findings), doc/modernization-plan.md sections B2-B3/2.3 (Zig API facts).
//!
//! Link-order note: Zig 0.17's Mach-O linker lays out input objects (and hence
//! C++ static initializers) sorted lexicographically by object basename -
//! input order is ignored (proven by sorted/renamed-object probes). ChakraCore
//! relies on reference link order for initializer sequencing on Mach-O (its
//! INIT_PRIORITY attributes are flat there), so every TU compiles to a
//! zero-padded `NNNN-<base>.o` whose lexicographic order reproduces the
//! reference link order with two known deviations (see numbering site):
//! ChakraCoreStatic.cpp.o is numbered first-in-static while the reference
//! archives it last (static-link idx 522/523 - immaterial: the TU is a dummy
//! `void EMPTY_FUNCTION(){}` with no initializers), and the PAL section
//! differs (reference: 18 cruntime C++, 21 safecrt C, 47 C++ debug..threadsusp,
//! mach pair, 3 asm; ours: mach pair moved first, 65 C++ in
//! reference-relative order, 21 C, 3 asm - no dynamic initializers
//! in C/asm by construction; mach pair only constant-initialized POD statics).
//! One global numbering serves all links: ch objects, GCStress objects, dylib
//! objects, then the static archive members. The PAL deviation is
//! dispositioned in doc/b2-verification.md: the reordered C/asm/mach
//! inputs carry no dynamic initializers, so their relative order is
//! unobservable at startup. On the jit profile, the 89 backend TUs
//! compile and link immediately after Jsrt.Core (before Common.Common),
//! matching the reference ${chakra_backend_objects} position in
//! lib/CMakeLists.txt:21-28.

const std = @import("std");
const builtin = @import("builtin");
const pal_config = @import("tools/pal-config/check.zig");
const tables = @import("tools/build/tables.zig");

const profile_target = "aarch64-macos.26.0";
const host_clang_target = "arm64-apple-macos26.0";
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
    // B4.3: the Linux x86_64 graph (tools/build/linux.zig) runs natively
    // in-container; the Windows x86_64 graph (tools/build/windows.zig) runs
    // natively on x86_64-windows-msvc; the Mac graph below keeps ARM64 macOS.
    const is_mac = builtin.os.tag == .macos and builtin.cpu.arch == .aarch64;
    const is_linux = builtin.os.tag == .linux and builtin.cpu.arch == .x86_64;
    const is_win = builtin.os.tag == .windows and builtin.cpu.arch == .x86_64;
    if (!is_mac and !is_linux and !is_win) {
        fail("profile is native-only: build machine must be ARM64 macOS, x86_64 Linux, or x86_64 Windows for native run probes, have {t}-{t}", .{ builtin.cpu.arch, builtin.os.tag });
    }
}

fn gateTarget(b: *std.Build) std.Build.ResolvedTarget {
    const rt = b.standardTargetOptions(.{ .default_target = .{
        .cpu_arch = .aarch64,
        .cpu_model = .{ .explicit = &std.Target.aarch64.cpu.apple_m1 },
        .os_tag = .macos,
        .os_version_min = .{ .semver = .{ .major = 26, .minor = 0, .patch = 0 } },
        .abi = .none,
    } });
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
    if (t.ofmt != .macho)
        fail("unsupported object format {t}: this profile builds only Mach-O", .{t.ofmt});
    if (!std.mem.eql(u8, t.cpu.model.name, "apple_m1"))
        fail("unsupported CPU model {s}: this profile builds only apple_m1 (the aarch64-macos baseline)", .{t.cpu.model.name});
    const min = t.os.version_range.semver.min;
    if (min.major != 26 or min.minor != 0 or min.patch != 0)
        fail("unsupported macOS minimum {d}.{d}.{d}: this profile builds only 26.0", .{ min.major, min.minor, min.patch });
    return rt;
}

const Profile = enum {
    @"test",
    debug_static,
    release_shared,
    /// Worktree-only ARM64 macOS JIT. Same flags as test, without DISABLE_JIT.
    jit,
    /// Worktree-only ARM64 macOS Debug+JIT (static, asserts enabled, backend asserts active).
    jit_debug,
    /// Worktree-only ARM64 macOS Release+JIT (-O3 -DNDEBUG -g0, no debug flaglist).
    release_jit,

    fn name(self: Profile) []const u8 {
        return switch (self) {
            .@"test" => "test",
            .debug_static => "debug-static",
            .release_shared => "release-shared",
            .jit => "jit",
            .jit_debug => "jit-debug",
            .release_jit => "release-jit",
        };
    }

    fn isStatic(self: Profile) bool {
        return self == .debug_static or self == .jit_debug;
    }

    fn isJit(self: Profile) bool {
        return self == .jit or self == .jit_debug or self == .release_jit;
    }

    fn isDebug(self: Profile) bool {
        return self == .debug_static or self == .jit_debug;
    }

    fn isRelease(self: Profile) bool {
        return self == .release_shared or self == .release_jit;
    }

    fn hasDebugFlags(self: Profile) bool {
        return !self.isRelease();
    }
};

fn gateProfile(b: *std.Build) Profile {
    const raw = b.option([]const u8, "profile", "Build profile: test (default), debug-static, release-shared, jit, jit-debug, release-jit") orelse "test";
    if (std.mem.eql(u8, raw, "test")) return .@"test";
    if (std.mem.eql(u8, raw, "debug-static")) return .debug_static;
    if (std.mem.eql(u8, raw, "release-shared")) return .release_shared;
    if (std.mem.eql(u8, raw, "jit")) return .jit;
    if (std.mem.eql(u8, raw, "jit-debug")) return .jit_debug;
    if (std.mem.eql(u8, raw, "release-jit")) return .release_jit;
    fail("unsupported -Dprofile={s}: expected one of: test, debug-static, release-shared, jit, jit-debug, release-jit", .{raw});
}

const Sanitize = enum {
    none,
    address,
    undefined,
    thread,

    fn name(self: Sanitize) []const u8 {
        return switch (self) {
            .none => "none",
            .address => "address",
            .undefined => "undefined",
            .thread => "thread",
        };
    }

    fn fsanitizeFlag(self: Sanitize) []const u8 {
        return switch (self) {
            .none => unreachable,
            .address => "-fsanitize=address",
            .undefined => "-fsanitize=undefined",
            .thread => "-fsanitize=thread",
        };
    }
};

fn gateSanitize(b: *std.Build) Sanitize {
    const raw = b.option([]const u8, "sanitize", "Sanitizer mode: none (default), address, undefined, thread (Linux only)") orelse "none";
    if (std.mem.eql(u8, raw, "none")) return .none;
    if (std.mem.eql(u8, raw, "address")) return .address;
    if (std.mem.eql(u8, raw, "undefined")) return .undefined;
    if (std.mem.eql(u8, raw, "thread")) {
        fail("unsupported -Dsanitize=thread on macOS: ChakraCore PAL Mach exception/thread-suspension hooks are not TSan-clean on Darwin; use -Dsanitize=address or -Dsanitize=undefined on macOS, or -Dsanitize=thread on Linux", .{});
    }
    fail("unsupported -Dsanitize={s}: expected one of: none, address, undefined, thread", .{raw});
}

const HostCompilers = struct {
    cc: []const u8,
    cxx: []const u8,
};

fn gateHostCompilers(b: *std.Build) HostCompilers {
    const cc_res = b.runFallible(&.{ "xcrun", "--find", "clang" }, .{});
    const cc = switch (cc_res) {
        .success => |out| std.mem.trim(u8, out, " \t\r\n"),
        else => fail("cannot resolve host clang via xcrun --find clang (required for -Dsanitize)", .{}),
    };
    const cxx_res = b.runFallible(&.{ "xcrun", "--find", "clang++" }, .{});
    const cxx = switch (cxx_res) {
        .success => |out| std.mem.trim(u8, out, " \t\r\n"),
        else => fail("cannot resolve host clang++ via xcrun --find clang++ (required for -Dsanitize)", .{}),
    };
    return .{ .cc = cc, .cxx = cxx };
}

fn gateOptimize(b: *std.Build, profile: Profile) std.builtin.Optimize {
    // The C/C++ -O flags are explicit on our compiler command lines; the
    // Zig mode is pinned per profile so a mode name alone can never select
    // a different engine configuration (modernization-plan.md section B3).
    const want: std.builtin.Optimize = if (profile.isDebug()) .debug else .fast;
    const choice = b.option(std.builtin.Optimize, "optimize", "Optimization (pinned per profile: fast for test/release-shared/jit/release-jit, debug for debug-static/jit-debug)") orelse want;
    if (choice == want) return choice;
    switch (profile) {
        .@"test" => fail("unsupported -Doptimize={t}: this profile (RelWithDebInfo Test, -O3 -g) supports only fast", .{choice}),
        else => fail("unsupported -Doptimize={t} for -Dprofile={s}: this profile requires {t}", .{ choice, profile.name(), want }),
    }
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
    if (parsed.value != .object)
        fail("{s}: SDKSettings.json root must be an object (need MacOSX SDK {s})", .{ settings_path, profile_sdk_version });
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

fn gatePin(obj: std.json.ObjectMap, path: []const u8, select: []const u8, field: []const u8) u32 {
    const entry = obj.get(select) orelse
        fail("malformed gate counts {s}: missing {s} pin", .{ path, select });
    if (entry != .object)
        fail("malformed gate counts {s}: {s} pin must be an object", .{ path, select });
    const v = entry.object.get(field) orelse
        fail("malformed gate counts {s}: {s} pin missing {s}", .{ path, select, field });
    if (v != .integer or v.integer < 0 or v.integer > std.math.maxInt(u32))
        fail("malformed gate counts {s}: {s}.{s} must be an integer in [0, {d}]", .{ path, select, field, std.math.maxInt(u32) });
    return @intCast(v.integer);
}

const GateCounts = struct {
    test_passed: u32,
    test_failed: u32,
    debug_passed: u32,
    debug_failed: u32,
};

// Single-sourced regression gate pins (tools/build/gate_counts.json):
// read at build-graph time so the check-regression args and step
// descriptions below always match check_regression.py. Missing or
// malformed source fails the configure loudly (same fail() style as
// the SDK/ICU gates) - never silent defaults.
fn loadGateCounts(b: *std.Build) GateCounts {
    const arena = b.graph.arena;
    const root = b.root.root_dir.path orelse
        fail("cannot resolve the build root path; refusing a CWD-relative gate counts read", .{});
    const path = std.fmt.allocPrint(arena, "{s}/tools/build/gate_counts.json", .{root}) catch @panic("OOM");
    const file = std.Io.Dir.openFileAbsolute(b.graph.io, path, .{}) catch
        fail("cannot read gate counts {s}: missing tools/build/gate_counts.json", .{path});
    defer file.close(b.graph.io);
    var fr = file.readerStreaming(b.graph.io, &.{});
    const bytes = fr.interface.allocRemaining(arena, .limited(1 << 20)) catch
        fail("cannot read gate counts {s}", .{path});
    const parsed = std.json.parseFromSlice(std.json.Value, arena, bytes, .{}) catch
        fail("malformed gate counts {s}: invalid JSON", .{path});
    if (parsed.value != .object)
        fail("malformed gate counts {s}: root must be an object", .{path});
    return .{
        .test_passed = gatePin(parsed.value.object, path, "test", "passed"),
        .test_failed = gatePin(parsed.value.object, path, "test", "failed"),
        .debug_passed = gatePin(parsed.value.object, path, "debug", "passed"),
        .debug_failed = gatePin(parsed.value.object, path, "debug", "failed"),
    };
}

const IcuPaths = struct {
    include: []const u8,
    libdir: []const u8,
    major: []const u8,
};

fn parseIcuHeaderMajor(arena: std.mem.Allocator, bytes: []const u8) ?[]const u8 {
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
        if (end > 0) return arena.dupe(u8, val[0..end]) catch @panic("OOM");
    }
    return null;
}

fn extractDylibMajorFromId(id_out: []const u8, stem: []const u8) ?[]const u8 {
    var lines = std.mem.splitScalar(u8, id_out, '\n');
    while (lines.next()) |raw_line| {
        const line = std.mem.trim(u8, raw_line, " \t\r");
        if (line.len == 0 or std.mem.endsWith(u8, line, ":")) continue;
        const slash = std.mem.lastIndexOfScalar(u8, line, '/');
        const base = if (slash) |idx| line[idx + 1 ..] else line;
        if (!std.mem.startsWith(u8, base, stem)) continue;
        const after_stem = base[stem.len..];
        if (!std.mem.startsWith(u8, after_stem, ".")) continue;
        const rest = after_stem[1..];
        var end: usize = 0;
        while (end < rest.len and rest[end] >= '0' and rest[end] <= '9') : (end += 1) {}
        if (end > 0 and std.mem.endsWith(u8, rest[end..], ".dylib")) {
            return rest[0..end];
        }
    }
    return null;
}

fn gateIcu(b: *std.Build) IcuPaths {
    const arena = b.graph.arena;
    const io = b.graph.io;
    const icu = b.option([]const u8, "icu-include", "ICU include dir (default: " ++ default_icu_include ++ ")") orelse default_icu_include;
    const default_libdir = std.fmt.allocPrint(arena, "{s}/../lib", .{icu}) catch @panic("OOM");
    const libdir = b.option([]const u8, "icu-libdir", "ICU lib dir (default: <icu-include>/../lib)") orelse default_libdir;
    const uchar = std.fmt.allocPrint(arena, "{s}/unicode/uchar.h", .{icu}) catch @panic("OOM");
    std.Io.Dir.accessAbsolute(io, uchar, .{}) catch
        fail("Cannot find ICU (missing {s}). Try installing libicu-dev or the appropriate packages for your platform. Pass -Dicu-include=<icu>/include to point at ICU headers.", .{uchar});
    for ([_][]const u8{ "libicuuc.dylib", "libicui18n.dylib", "libicudata.dylib" }) |libname| {
        const lib = std.fmt.allocPrint(arena, "{s}/{s}", .{ libdir, libname }) catch @panic("OOM");
        std.Io.Dir.accessAbsolute(io, lib, .{}) catch
            fail("Cannot find ICU libraries (missing {s}). Try installing libicu-dev or the appropriate packages for your platform. Pass -Dicu-include=<icu>/include or -Dicu-libdir=<icu>/lib to point at the matching ICU.", .{lib});
    }
    const uvernum = std.fmt.allocPrint(arena, "{s}/unicode/uvernum.h", .{icu}) catch @panic("OOM");
    const vf = std.Io.Dir.openFileAbsolute(io, uvernum, .{}) catch
        fail("Cannot verify ICU header version (missing {s}). Pass -Dicu-include=<icu>/include to point at valid ICU headers.", .{uvernum});
    defer vf.close(io);
    var vfr = vf.readerStreaming(io, &.{});
    const uvernum_bytes = vfr.interface.allocRemaining(arena, .limited(1 << 20)) catch
        fail("Cannot read {s} to verify ICU version.", .{uvernum});
    const hdr_major = parseIcuHeaderMajor(arena, uvernum_bytes) orelse
        fail("Cannot parse U_ICU_VERSION_MAJOR_NUM in {s}.", .{uvernum});

    const checks = [_]struct { file: []const u8, stem: []const u8 }{
        .{ .file = "libicuuc.dylib", .stem = "libicuuc" },
        .{ .file = "libicui18n.dylib", .stem = "libicui18n" },
        .{ .file = "libicudata.dylib", .stem = "libicudata" },
    };
    var saw_lib_major = false;
    for (checks) |chk| {
        const lib_path = std.fmt.allocPrint(arena, "{s}/{s}", .{ libdir, chk.file }) catch @panic("OOM");
        const id_res = b.runFallible(&.{ "otool", "-D", lib_path }, .{});
        const id_out: []const u8 = switch (id_res) {
            .success => |o| o,
            else => fail("Cannot inspect ICU library install_name via otool -D {s}.", .{lib_path}),
        };
        if (extractDylibMajorFromId(id_out, chk.stem)) |lib_major| {
            saw_lib_major = true;
            if (!std.mem.eql(u8, lib_major, hdr_major)) {
                fail("ICU header/library version skew: {s} declares U_ICU_VERSION_MAJOR_NUM={s}, but {s} links as major {s}. Pass matching -Dicu-include and -Dicu-libdir.", .{ uvernum, hdr_major, lib_path, lib_major });
            }
        }
    }
    if (!saw_lib_major) {
        const i18n_path = std.fmt.allocPrint(arena, "{s}/libicui18n.dylib", .{libdir}) catch @panic("OOM");
        const deps_res = b.runFallible(&.{ "otool", "-L", i18n_path }, .{});
        const deps_out: []const u8 = switch (deps_res) {
            .success => |o| o,
            else => fail("Cannot inspect ICU dependencies via otool -L {s}.", .{i18n_path}),
        };
        var lines = std.mem.splitScalar(u8, deps_out, '\n');
        while (lines.next()) |raw_line| {
            const trimmed = std.mem.trim(u8, raw_line, " \t\r");
            const paren = std.mem.indexOf(u8, trimmed, " (compatibility") orelse continue;
            if (extractDylibMajorFromId(trimmed[0..paren], "libicuuc")) |lib_major| {
                saw_lib_major = true;
                if (!std.mem.eql(u8, lib_major, hdr_major)) {
                    fail("ICU header/library version skew: {s} declares U_ICU_VERSION_MAJOR_NUM={s}, but {s} depends on libicuuc major {s}. Pass matching -Dicu-include and -Dicu-libdir.", .{ uvernum, hdr_major, i18n_path, lib_major });
                }
            }
        }
    }
    if (!saw_lib_major) {
        // Fallback for unversioned custom dylibs whose LC_ID_DYLIB omits the major suffix:
        // verify runtime major version via u_getVersion in libicuuc.
        const py_bin = b.findProgram(.{ .names = &.{ "python3", "python" } }) orelse
            fail("Cannot verify unversioned ICU library {s}/libicuuc.dylib against {s} (major {s}).", .{ libdir, uvernum, hdr_major });
        const uc_path = std.fmt.allocPrint(arena, "{s}/libicuuc.dylib", .{libdir}) catch @panic("OOM");
        const probe_src = "import ctypes, sys\nlib = ctypes.CDLL(sys.argv[1])\nfn = getattr(lib, 'u_getVersion_' + sys.argv[2], None)\nif fn is None:\n    for cand in dir(lib): pass\n    # Fall back to unversioned u_getVersion if built with --disable-renaming\n    fn = getattr(lib, 'u_getVersion', None)\n    if fn is None:\n        sys.stderr.write('missing u_getVersion_' + sys.argv[2] + ' and u_getVersion')\n        sys.exit(2)\nver = (ctypes.c_uint8 * 4)()\nfn(ver)\nif str(ver[0]) != sys.argv[2]:\n    sys.stderr.write(f'runtime major {ver[0]} != header major {sys.argv[2]}')\n    sys.exit(3)\n";
        const probe_res = b.runFallible(&.{ py_bin, "-c", probe_src, uc_path, hdr_major }, .{});
        switch (probe_res) {
            .success => {},
            else => fail("ICU header/library version skew: {s} declares U_ICU_VERSION_MAJOR_NUM={s}, which does not match {s}. Pass matching -Dicu-include and -Dicu-libdir.", .{ uvernum, hdr_major, uc_path }),
        }
    }
    return .{ .include = icu, .libdir = libdir, .major = hdr_major };
}

const Ctx = struct {
    b: *std.Build,
    profile: Profile,
    sanitize: Sanitize,
    wasm: bool,
    target: std.Build.ResolvedTarget,
    optimize: std.builtin.Optimize,
    sdk_path: []const u8,
    icu_include: []const u8,
    sdk_isystem: []const u8,
    sdk_frameworks: []const u8,
    sdk_lib: []const u8,
    zig_exe: []const u8,
    host_cc: []const u8,
    host_cxx: []const u8,
    config_dir: std.Build.LazyPath,
    config_h: std.Build.LazyPath,
    // wabt config.h (generated only under -Dwasm=true; otherwise
    // aliases of the pal-config paths above, never consumed).
    wabt_dir: std.Build.LazyPath,
    wabt_h: std.Build.LazyPath,
    gen_dir: std.Build.LazyPath,
    dbg_header: std.Build.LazyPath,
};

const LinkedArtifact = struct {
    bin: std.Build.LazyPath,
    step: *std.Build.Step,
    compile: ?*std.Build.Step.Compile = null,
    name: []const u8 = "",
};

const Lang = enum { cxx, c, asm_ };

// SDK search paths for the link: the build API has no --sysroot knob, so
// pass the absolute SDK -F/-L paths (the pilot's sysroot-relative -F
// /System/Library/Frameworks and -L /usr/lib, doc/build.md section 14).
// Known deviation: linked binaries stamp LC_BUILD_VERSION sdk 27.0 (otool)
// while the gate pins MacOSX SDK 26.5 - with no sysroot knob the stamp comes
// from the toolchain default; minos 26.0 holds. The reference stamps 27.0
// too, so this matches the reference.
fn addSdkLinkPaths(ctx: *const Ctx, mod: *std.Build.Module) void {
    mod.addFrameworkPath(.{ .cwd_relative = ctx.sdk_frameworks });
    mod.addLibraryPath(.{ .cwd_relative = ctx.sdk_lib });
}

fn addSanitizeHostLinkPrefix(ctx: *const Ctx, cmd: *std.Build.Step.Run) void {
    cmd.addArgs(&.{
        "-target",
        host_clang_target,
        "-isysroot",
        ctx.sdk_path,
        "-mcpu=apple-m1",
        ctx.sanitize.fsanitizeFlag(),
        "-Wl,-headerpad_max_install_names",
    });
}

fn registerFailStep(b: *std.Build, python: []const u8, name: []const u8, desc: []const u8, msg: []const u8) void {
    const cmd = b.addSystemCommand(&.{ python, "-c", "import sys; sys.exit(sys.argv[1])", msg });
    cmd.stdio = .inherit;
    const step = b.step(name, desc);
    step.dependOn(&cmd.step);
}

fn registerPackageSteps(
    b: *std.Build,
    python: []const u8,
    is_supported: bool,
    prefix_dir: []const u8,
    profile_or_side: []const u8,
    artifacts_ready: *std.Build.Step,
    prov_step_opt: ?*std.Build.Step,
    nodeapi_step_opt: ?*std.Build.Step,
) void {
    if (!is_supported) {
        registerFailStep(b, python, "package", "Not applicable on static profiles", "package requires a shared profile (test, release-shared, jit, or release-jit) with dynamic libraries");
        registerFailStep(b, python, "check-package", "Not applicable on static profiles", "check-package requires a shared profile (test, release-shared, jit, or release-jit) with dynamic libraries");
        return;
    }

    const out_dir = b.fmt("out/package/{s}", .{profile_or_side});

    const pkg_cmd = b.addSystemCommand(&.{python});
    pkg_cmd.addFileArg(b.path("tools/package/build_package.py"));
    pkg_cmd.addArg("--prefix");
    pkg_cmd.addArg(prefix_dir);
    pkg_cmd.addArg("--profile");
    pkg_cmd.addArg(profile_or_side);
    pkg_cmd.addArg("--out-dir");
    pkg_cmd.addArg(out_dir);
    pkg_cmd.step.dependOn(artifacts_ready);
    if (prov_step_opt) |ps| pkg_cmd.step.dependOn(ps);
    if (nodeapi_step_opt) |ns| pkg_cmd.step.dependOn(ns);
    pkg_cmd.stdio = .inherit;
    pkg_cmd.has_side_effects = true;

    const pkg_top = b.step("package", b.fmt("Build release archive and MANIFEST.json in {s} (not part of the default build)", .{out_dir}));
    pkg_top.dependOn(&pkg_cmd.step);

    const verify_cmd = b.addSystemCommand(&.{python});
    verify_cmd.addFileArg(b.path("tools/package/verify_package.py"));
    verify_cmd.addArg(out_dir);
    verify_cmd.addArg("--profile");
    verify_cmd.addArg(profile_or_side);
    verify_cmd.step.dependOn(&pkg_cmd.step);
    verify_cmd.stdio = .inherit;
    verify_cmd.has_side_effects = true;

    const test_cmd = b.addSystemCommand(&.{python});
    test_cmd.addFileArg(b.path("tools/package/test_package.py"));
    test_cmd.addArg(out_dir);
    test_cmd.addArg("--profile");
    test_cmd.addArg(profile_or_side);
    test_cmd.step.dependOn(&verify_cmd.step);
    test_cmd.stdio = .inherit;
    test_cmd.has_side_effects = true;

    const check_pkg_top = b.step("check-package", "Verify package creation, manifest integrity, and end-to-end execution of packaged binaries (not part of the default build)");
    check_pkg_top.dependOn(&test_cmd.step);
}

fn createLibtoolArchive(b: *std.Build, name: []const u8, objs: []const std.Build.LazyPath) LinkedArtifact {
    const lt = b.addSystemCommand(&.{ "/usr/bin/env", "ZERO_AR_DATE=1", "/usr/bin/libtool", "-static", "-no_warning_for_no_symbols", "-o" });
    const out = lt.addOutputFileArg(b.fmt("lib{s}.a", .{name}));
    for (objs) |o| lt.addFileArg(o);
    return .{ .bin = out, .step = &lt.step, .name = name };
}

fn createLinkModule(ctx: *const Ctx) *std.Build.Module {
    return ctx.b.createModule(.{
        .target = ctx.target,
        .optimize = ctx.optimize,
        .link_libc = true,
        .link_libcpp = true,
    });
}

// B3.4 install isolation (supersedes the B3.1 `<profile>/` subdirs, which
// Zig's per-configuration prefix ownership purged on every profile
// switch): the test profile owns the native install system (default
// prefix zig-out, honoring --prefix); non-test profiles install
// NOTHING natively and instead copy out to fixed `out/<profile>/`
// side prefixes via tools/build/install_profile.py. build.zig cannot
// read --prefix in 0.17 (no such field on Build/Graph in the shipped
// std), so fixed destinations are the only enforceable isolation -
// and the mechanism needs no enforcement: every destination is a pure
// function of -Dprofile, so no invocation can cross the streams.

// Append one TU's compile flags to a `zig c++|cc` run step, in observed
// order: defines, includes (generated dirs at their reference positions),
// warnings (C++ only), codegen tail. Drops target-owned/dead/cosmetic
// entries (-mmacosx-version-min, -arch, -std=gnu++11, -Werror/-Wno-error,
// -fdiagnostics-color); the driver supplies target/sysroot/CPU from argv.
fn appendTuFlags(ctx: *const Ctx, run: *std.Build.Step.Run, g: *const tables.Group, lang: Lang) void {
    const b = ctx.b;
    // Per-profile defines (doc/b3-profiles.md): test keeps the B2 tables;
    // debug-static swaps ENABLE_DEBUG_CONFIG_OPTIONS for the Debug set plus
    // CHAKRA_STATIC_LIBRARY; release-shared drops the Test define (NDEBUG
    // arrives via the flag slot below, as in CMAKE_*_FLAGS_RELEASE).
    const defines = switch (ctx.profile) {
        // jit reuses the test tables and drops DISABLE_JIT so every TU,
        // PAL included, is compiled with native codegen enabled.
        .@"test", .jit => if (g.pal) tables.pal_defines else tables.engine_defines,
        .debug_static, .jit_debug => if (g.pal) tables.pal_defines_debug_static else tables.engine_defines_debug_static,
        .release_shared, .release_jit => if (g.pal) tables.pal_defines_release else tables.engine_defines_release,
    };
    for (defines) |d| {
        if (ctx.profile.isJit() and std.mem.eql(u8, d, "-DDISABLE_JIT=1")) continue;
        if (!ctx.profile.isStatic() and std.mem.eql(u8, d, "-DCHAKRA_STATIC_LIBRARY=1")) continue;
        run.addArg(d);
    }
    if (g.target_define) |d| run.addArg(d);
    // W0 -Dwasm (doc/wasm-arm64-plan.md): only on jit+wasm, so the
    // option-off command lines are byte-identical to today.
    if (ctx.wasm) run.addArg("-DWASM_ARM64_PORT=1");
    // CAN_BUILD_WABT mirrors the CMake define (CMakeLists.txt:283 and
    // the Linux engine_defines): on the wasm build the vendored WABT
    // compiles and CommonDefines enables ENABLE_WABT via its ARM64
    // disjunct. Wasm-conditional only, so option-off lines are
    // byte-identical to today.
    if (ctx.wasm) run.addArg("-DCAN_BUILD_WABT");
    // CMake sets this on the Language and Library targets when BuildJIT
    // is on (lib/Runtime/Language/CMakeLists.txt, Library/CMakeLists.txt).
    // arm64_Thunks.S emits the dynamic-interpreter entry points only
    // under this define.
    if (ctx.profile.isJit() and (g == &tables.language_group or g == &tables.library_group))
        run.addArg("-D_ENABLE_DYNAMIC_THUNKS=1");

    const is_ch = g == &tables.ch_group;
    if (g.pal) run.addPrefixedDirectoryArg("-I", ctx.config_dir);
    const rels = if (g.pal) tables.pal_includes_rel else tables.global_includes_rel;
    for (rels) |dir| run.addArg(b.fmt("-I{s}", .{dir}));
    run.addArg(b.fmt("-I{s}", .{ctx.icu_include}));
    run.addArg(b.fmt("-I{s}/../i18n", .{ctx.icu_include}));
    for (g.extra_includes, 0..) |dir, i| {
        if (is_ch and i == tables.gen_include_index)
            run.addPrefixedDirectoryArg("-I", ctx.gen_dir);
        // Generated-dir marker (tables.zig wabt_group carries the
        // Linux spelling): substituted with the build-owned wabt
        // config dir, same convention as the gen_dir marker above.
        if (std.mem.eql(u8, dir, "build-xplat/lib/wabt"))
            run.addPrefixedDirectoryArg("-I", ctx.wabt_dir)
        else
            run.addArg(b.fmt("-I{s}", .{dir}));
    }
    if (g.pal) run.addArg("-Ipal/src/include");

    switch (lang) {
        .cxx => {
            for (tables.warnings) |w| run.addArg(w);
            for ([_][]const u8{ "-fno-omit-frame-pointer", "-fdelayed-template-parsing" }) |f| run.addArg(f);
            if (ctx.sanitize != .none) {
                if (ctx.profile.isDebug()) {
                    run.addArg("-g");
                } else {
                    for ([_][]const u8{ "-O1", "-g" }) |f| run.addArg(f);
                }
                run.addArg(ctx.sanitize.fsanitizeFlag());
                if (ctx.sanitize == .undefined) {
                    run.addArg("-fno-sanitize=enum,alignment,vptr,pointer-overflow");
                }
            } else {
                // Config flags in the CMAKE_CXX_FLAGS_<CONFIG> slot: test -O3
                // -g (repo override), Debug -g (CMake builtin, no -O),
                // Release -O3 -DNDEBUG (CMake builtin, no -g) plus an
                // explicit -g0: `zig c++` emits DWARF unless told
                // otherwise (probed: -O3 -DNDEBUG alone still ships 4
                // __debug sections), while legacy AppleClang defaults
                // to none - a driver-default correction in the same
                // category as -UNDEBUG/-fno-sanitize below.
                switch (ctx.profile) {
                    .@"test", .jit => for ([_][]const u8{ "-O3", "-g" }) |f| run.addArg(f),
                    .debug_static, .jit_debug => run.addArg("-g"),
                    .release_shared, .release_jit => for ([_][]const u8{ "-O3", "-DNDEBUG", "-g0" }) |f| run.addArg(f),
                }
            }
            run.addArg("-std=gnu++17");
            // -fPIE sits mid-line on ch (target property); -fPIC sits
            // late on lib units, after -fwrapv (observed command lines).
            if (g.pic == .pie) run.addArg("-fPIE");
            for ([_][]const u8{ "-fasm-blocks", "-fms-extensions", "-fwrapv" }) |f| run.addArg(f);
            if (g.pal) {
                for ([_][]const u8{ "-fstack-protector", "-fno-builtin", "-fPIC" }) |f| run.addArg(f);
            } else if (g.pic == .pic) {
                run.addArg("-fPIC");
            }
            if (ctx.sanitize != .none) {
                // Host Apple clang with -isysroot handles SDK C++ stdlib and
                // system framework resolution directly; -mcpu uses hyphen.
                run.addArg("-mcpu=apple-m1");
                if (!ctx.profile.isRelease()) run.addArg("-UNDEBUG");
                if (is_ch and ctx.sanitize == .address) {
                    run.addArg("-include");
                    run.addArg("tools/build/asan_apple_prefix.h");
                }
                // Place /usr/local/include strictly after SDK system includes (B-14).
                run.addArg("-idirafter");
                run.addArg("/usr/local/include");
            } else {
                // Build-required additions: the SDK C++ headers
                // (-isystem, absolute) and SDK frameworks (-F, sysroot-
                // relative: the driver resolves -F against --sysroot, so an
                // absolute -F would double the prefix - pilot finding), the
                // pinned apple_m1 CPU, and -Wno-date-time (zig clang 22
                // promotes __DATE__/__TIME__; pilot finding). The c++ driver
                // injects no NDEBUG, matching the reference C++ (contract
                // quirk Q4); -UNDEBUG is a defensive no-op, kept for every
                // profile except release-shared/release-jit (whose -DNDEBUG above is real).
                // -fno-sanitize=undefined on debug-static/jit-debug: `zig c++` with no
                // -O flag defaults to a UBSan-instrumented build (80 ubsan
                // symbols observed in ch, trapping on engine null+offset
                // arithmetic), while legacy AppleClang instruments nothing;
                // -O3/-O2 on the other profiles already disable it (0
                // symbols). Probed: the spelling removes all ubsan refs.
                run.addArg("-isystem");
                run.addArg(ctx.sdk_isystem);
                // B-14: /usr/local/include follows SDK system includes so stale host headers cannot shadow SDK headers.
                run.addArg("-isystem");
                run.addArg("/usr/local/include");
                run.addArg("-F");
                run.addArg("/System/Library/Frameworks");
                run.addArg("-mcpu=apple_m1");
                if (!ctx.profile.isRelease()) run.addArg("-UNDEBUG");
                if (ctx.profile.isDebug()) run.addArg("-fno-sanitize=undefined");
            }
            run.addArg("-Wno-date-time");
        },
        .c, .asm_ => {
            if (lang == .c) run.addArg("-std=gnu99");
            if (ctx.sanitize != .none) {
                if (lang == .c) {
                    if (!ctx.profile.isDebug()) run.addArg("-O1");
                    run.addArg("-g");
                    run.addArg("-fno-omit-frame-pointer");
                    run.addArg(ctx.sanitize.fsanitizeFlag());
                    if (ctx.sanitize == .undefined) {
                        run.addArg("-fno-sanitize=enum,alignment,vptr,pointer-overflow");
                    }
                } else {
                    run.addArg("-g");
                }
            } else {
                // CMAKE_C_FLAGS_<CONFIG> builtins (the repo overrides only the
                // C++ RelWithDebInfo variant): test -O2 -g -DNDEBUG, Debug -g,
                // Release -O3 -DNDEBUG. Same builtins cover ASM. Release
                // takes an explicit -g0 (`zig cc` emits DWARF unless told
                // otherwise - same driver-default correction as C++ above).
                const opt_flag: ?[]const u8 = switch (ctx.profile) {
                    .@"test", .jit => "-O2",
                    .debug_static, .jit_debug => null,
                    .release_shared, .release_jit => "-O3",
                };
                if (opt_flag) |f| run.addArg(f);
                run.addArg(if (ctx.profile.isRelease()) "-g0" else "-g");
            }
            if (!ctx.profile.isDebug()) run.addArg("-DNDEBUG");
            for ([_][]const u8{ "-fasm-blocks", "-fms-extensions", "-fwrapv" }) |f| run.addArg(f);
            if (g.pal) {
                for ([_][]const u8{ "-fstack-protector", "-fno-builtin", "-fPIC" }) |f| run.addArg(f);
            } else if (g.pic == .pic) {
                run.addArg("-fPIC");
            }
            if (ctx.sanitize != .none) {
                run.addArg("-mcpu=apple-m1");
                run.addArg("-idirafter");
                run.addArg("/usr/local/include");
            } else {
                run.addArg("-isystem");
                run.addArg(ctx.sdk_isystem);
                // B-14: /usr/local/include follows SDK system includes.
                run.addArg("-isystem");
                run.addArg("/usr/local/include");
                run.addArg("-F");
                run.addArg("/System/Library/Frameworks");
                run.addArg("-mcpu=apple_m1");
                // Same UBSan default-correction as C++ (see above), C only -
                // assembler lines take no sanitizer flags.
                if (lang == .c and ctx.profile.isDebug()) run.addArg("-fno-sanitize=undefined");
            }
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
    const run = if (ctx.sanitize != .none) blk: {
        const cmd = b.addSystemCommand(&.{if (lang == .cxx) ctx.host_cxx else ctx.host_cc});
        cmd.addArg("-target");
        cmd.addArg(host_clang_target);
        cmd.addArg("-isysroot");
        cmd.addArg(ctx.sdk_path);
        break :blk cmd;
    } else blk: {
        const cmd = b.addSystemCommand(&.{ ctx.zig_exe, if (lang == .cxx) "c++" else "cc" });
        cmd.addArg("-target");
        cmd.addArg(profile_target);
        cmd.addArg("--sysroot");
        cmd.addArg(ctx.sdk_path);
        break :blk cmd;
    };
    appendTuFlags(ctx, run, g, lang);
    var digits: [4]u8 = .{ '0', '0', '0', '0' };
    var n = number;
    var i: usize = 4;
    while (i > 0) {
        i -= 1;
        digits[i] = '0' + @as(u8, @intCast(n % 10));
        n /= 10;
    }
    const base = std.fs.path.basename(src);
    // B3.4 header-dependency tracking: clang writes a Makefile-format
    // .d depfile beside the object; the runner adds every listed header
    // as a tracked input, so touching a header rebuilds exactly its
    // dependent TUs (micro-proven on this toolchain: a header content
    // change recompiles its includers; a no-op rebuild runs zero
    // compiles). Depfiles are per-step cache outputs keyed by the full
    // command line, so profiles never share them. Same -MD/-MF shape
    // for C, C++ and preprocessed assembly.
    run.addArg("-MD");
    run.addArg("-MF");
    _ = run.addDepFileOutputArg(b.fmt("{s}-{s}.d", .{ digits, base }));
    run.addArg("-o");
    const out = run.addOutputFileArg(b.fmt("{s}-{s}.o", .{ digits, base }));
    run.addArg("-c");
    run.addFileArg(b.path(src));
    // The generated DbgController.js.h is a TRACKED build input of every
    // ch compile (contract section 5.4 edge): addFileInput both orders
    // the compile after the generator and rebuilds it when the header
    // bytes change (e.g. after touching bin/ch/DbgController.js).
    if (g == &tables.ch_group) run.addFileInput(ctx.dbg_header);
    // The generated config.h is an explicit input of every PAL compile.
    // Depfiles already list it for includers; the explicit edge keeps
    // the configuration->dependent contract obvious and covers
    // non-includers conservatively (config.h bytes change only when
    // probe results change, so the cost is nil).
    if (g.pal) run.addFileInput(ctx.config_h);
    // The generated wabt config.h is an explicit input of every wabt
    // compile (same pattern as the PAL edge above; tools/build/linux.zig
    // wires the identical edge for wabt_group).
    if (g == &tables.wabt_group) run.addFileInput(ctx.wabt_h);
    return out;
}

// wabt config.h: fixed-by-profile substitution of lib/wabt/src/config.h.in
// (configure_file, lib/wabt/CMakeLists.txt:41-44). Copied from
// tools/build/linux.zig renderWabtConfig: the CMake checks evaluate
// identically on macOS ARM64 (alloca.h and strings.h ship in the Mac
// SDK; sysconf/snprintf/ssize_t/strcasecmp all present; SIZEOF_SIZE_T 8).
// Any template drift fails loudly, so the substitution cannot silently
// diverge from the vendored template.
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
    // Gate order: cheap toolchain/host check first, profile second --
    // a -Dprofile typo fails in ~0.2s, before any slow gate.
    gateToolchain();
    // B4.3: Linux builds dispatch to the Linux graph (own gates inside).
    if (builtin.os.tag == .linux) {
        @import("tools/build/linux.zig").build(b);
        return;
    }
    if (builtin.os.tag == .windows) {
        @import("tools/build/windows.zig").build(b);
        return;
    }
    const profile = gateProfile(b);
    const sanitize = gateSanitize(b);
    if (sanitize != .none and profile.isRelease()) {
        fail("-Dsanitize={s} is not supported with -Dprofile={s}; use test (default), debug-static, jit, or jit-debug", .{ sanitize.name(), profile.name() });
    }
    const target = gateTarget(b);
    const optimize = gateOptimize(b, profile);
    const sdk_path = gateSdk(b);
    const icu = gateIcu(b);
    const icu_include = icu.include;
    const icu_lib = icu.libdir;
    const install_static_opt = b.option(bool, "install-static-archive", "Install/copy libChakraCoreStatic.a on shared profiles (default: false; always true on debug-static)") orelse false;
    const install_static_archive = profile.isStatic() or install_static_opt;
    const python = b.findProgram(.{ .names = &.{ "python3", "python" } }) orelse
        fail("cannot find python3 or python on PATH; needed to run bin/ch/jstoc.py for DbgController.js.h", .{});
    const gates = loadGateCounts(b);
    const host_compilers = if (sanitize != .none) gateHostCompilers(b) else HostCompilers{ .cc = "", .cxx = "" };
    const side_name: ?[]const u8 = if (sanitize != .none)
        b.fmt("{s}-sanitize-{s}", .{ profile.name(), sanitize.name() })
    else if (profile != .@"test")
        profile.name()
    else
        null;

    // The test line preserves B2 keys while exposing icu-libdir; other profiles
    // and sanitizer lanes label their isolated side prefixes.
    if (sanitize != .none) {
        std.debug.print("sanitize={s} profile={s}: target={s} sdk={s}@{s} icu-include={s} icu-libdir={s} host-cxx={s} side prefix out/{s}\n", .{
            sanitize.name(), profile.name(), host_clang_target, sdk_path, profile_sdk_version, icu_include, icu_lib, host_compilers.cxx, side_name.?,
        });
    } else if (profile == .@"test") {
        std.debug.print("b2 profile: target={s} sdk={s}@{s} icu-include={s} icu-libdir={s} zig={f} python={s}\n", .{
            profile_target, sdk_path, profile_sdk_version, icu_include, icu_lib, builtin.zig_version, python,
        });
    } else {
        std.debug.print("b3 profile={s}: target={s} sdk={s}@{s} icu-include={s} icu-libdir={s} zig={f} python={s}\n", .{
            profile.name(), profile_target, sdk_path, profile_sdk_version, icu_include, icu_lib, builtin.zig_version, python,
        });
        std.debug.print("b3 profile={s}: side prefix out/{s} (--prefix is ignored for this profile; only test uses the install prefix)\n", .{
            profile.name(), profile.name(),
        });
    }

    // Build-owned pal/src/config.h: 95 real checks, fail-clearly on deviation.
    const cfg = pal_config.generate(.{
        .io = b.graph.io,
        .zig_exe = b.graph.zig_exe,
        .target = profile_target,
        .sysroot = sdk_path,
        .profile = profile.name(),
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
    // regular-file mtimes untouched) - but it still runs the configure
    // phase (all gates plus the 95 pal-config probes) and re-creates
    // the two versioned-dylib symlinks (same targets, fresh mtimes).
    // Touching the JS re-runs the generator and recompiles all 8 ch
    // TUs via addFileInput - required-correct, not just conservative:
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

    // W0 ARM64 WebAssembly option (doc/wasm-arm64-plan.md W0 task 2):
    // -Dwasm=true, default off. Decided here: option-on under a
    // DISABLE_JIT profile fails loudly (test/debug-static/release-shared
    // all define DISABLE_JIT, where ASMJS_PLAT/ENABLE_ASMJS are off by
    // construction). No silent gate-off.
    const wasm = b.option(bool, "wasm", "ARM64 WebAssembly/asm.js port (jit/jit-debug/release-jit profiles only, default off)") orelse false;
    if (wasm and !profile.isJit())
        fail("-Dwasm=true requires a JIT profile (-Dprofile=jit, jit-debug, or release-jit; DISABLE_JIT profiles cannot enable ENABLE_WASM/ENABLE_ASMJS on ARM64)", .{});

    // Build-owned wabt config.h (fixed-by-profile substitution of
    // lib/wabt/src/config.h.in, same generator as the Linux graph).
    // Generated only under -Dwasm=true: every consumer is wasm-conditional,
    // so the option-off graph (including zig build check) never reads the
    // template. The template read is root-anchored, never CWD-relative.
    var wabt_dir: std.Build.LazyPath = wf.getDirectory();
    var wabt_h: std.Build.LazyPath = config_h;
    if (wasm) {
        const root = b.root.root_dir.path orelse
            fail("cannot resolve the build root path; refusing a CWD-relative wabt template read", .{});
        const wabt_in_path = std.fmt.allocPrint(arena, "{s}/lib/wabt/src/config.h.in", .{root}) catch @panic("OOM");
        const wabt_in_file = std.Io.Dir.openFileAbsolute(b.graph.io, wabt_in_path, .{}) catch
            fail("cannot read {s}", .{wabt_in_path});
        defer wabt_in_file.close(b.graph.io);
        var wabt_in_r = wabt_in_file.readerStreaming(b.graph.io, &.{});
        const wabt_template = wabt_in_r.interface.allocRemaining(arena, .limited(1 << 20)) catch
            fail("cannot read {s}", .{wabt_in_path});
        const wabt_bytes = renderWabtConfig(arena, wabt_template);
        const wabt_wf = b.addWriteFiles();
        wabt_h = wabt_wf.add("config.h", wabt_bytes);
        wabt_dir = wabt_wf.getDirectory();
        std.debug.print("wabt config.h: {d} bytes from lib/wabt/src/config.h.in\n", .{wabt_bytes.len});
    }

    const ctx = Ctx{
        .b = b,
        .profile = profile,
        .sanitize = sanitize,
        .wasm = wasm,
        .target = target,
        .optimize = optimize,
        .sdk_path = sdk_path,
        .icu_include = icu_include,
        .sdk_isystem = b.fmt("{s}/usr/include", .{sdk_path}),
        .sdk_frameworks = b.fmt("{s}/System/Library/Frameworks", .{sdk_path}),
        .sdk_lib = b.fmt("{s}/usr/lib", .{sdk_path}),
        .zig_exe = b.graph.zig_exe,
        .host_cc = host_compilers.cc,
        .host_cxx = host_compilers.cxx,
        .config_dir = wf.getDirectory(),
        .config_h = config_h,
        .wabt_dir = wabt_dir,
        .wabt_h = wabt_h,
        .gen_dir = gen_dir,
        .dbg_header = dbg_header,
    };

    // Compile every TU once, numbered in reference link order: ch objects,
    // GCStress objects, dylib objects (shared profiles only - static
    // builds never add bin/ChakraCore, bin/CMakeLists.txt:8-10), then the
    // static archive members (ChakraCoreStatic.cpp plus the 20 OBJECT
    // groups in TARGET_OBJECTS order). Static ch compiles TestHooks.cpp
    // as a 9th TU (bin/ch/CMakeLists.txt:19-23). Each link consumes its
    // own subsequence; relative order is the reference order in every
    // link, modulo the two deviations in the file doc comment
    // (ChakraCoreStatic.cpp.o first vs ar-last; PAL section layout -
    // dispositioned in doc/b2-verification.md: the reordered inputs
    // carry no dynamic initializers).
    const is_static = profile.isStatic();
    var n: u32 = 0;
    var ch_objs: std.ArrayList(std.Build.LazyPath) = .empty;
    for (tables.ch_group.cxx) |src| {
        ch_objs.append(arena, compileTu(&ctx, &tables.ch_group, src, .cxx, n)) catch @panic("OOM");
        n += 1;
    }
    if (is_static) {
        for (tables.ch_static_extra_cxx) |src| {
            ch_objs.append(arena, compileTu(&ctx, &tables.ch_group, src, .cxx, n)) catch @panic("OOM");
            n += 1;
        }
    }
    var gs_objs: std.ArrayList(std.Build.LazyPath) = .empty;
    for (tables.gcstress_group.cxx) |src| {
        gs_objs.append(arena, compileTu(&ctx, &tables.gcstress_group, src, .cxx, n)) catch @panic("OOM");
        n += 1;
    }
    var dylib_objs: std.ArrayList(std.Build.LazyPath) = .empty;
    if (!is_static) {
        for (tables.dylib_group.cxx) |src| {
            dylib_objs.append(arena, compileTu(&ctx, &tables.dylib_group, src, .cxx, n)) catch @panic("OOM");
            n += 1;
        }
    }
    var static_objs: std.ArrayList(std.Build.LazyPath) = .empty;
    // Singular archives for ch (contract section 5.3), one per group whose
    // Group.singular_name is set - a single loop, in object_groups order
    // (Pal, Codex, PlatformAgnostic), driving creation, ch linkage, and
    // install below.
    var singular_libs: std.ArrayList(LinkedArtifact) = .empty;
    for (tables.static_src_group.cxx) |src| {
        static_objs.append(arena, compileTu(&ctx, &tables.static_src_group, src, .cxx, n)) catch @panic("OOM");
        n += 1;
    }
    var backend_n: u32 = 0;
    var backend_expected: u32 = 0;
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
        // Singular archives exist only for shared builds (contract 5.3).
        if (!is_static) {
            if (g.singular_name) |name| {
                if (sanitize != .none) {
                    singular_libs.append(arena, createLibtoolArchive(b, name, static_objs.items[start..])) catch @panic("OOM");
                } else {
                    const mod = createLinkModule(&ctx);
                    for (static_objs.items[start..]) |o| mod.addObjectFile(o);
                    const lib = b.addLibrary(.{ .name = name, .root_module = mod });
                    singular_libs.append(arena, .{ .bin = lib.getEmittedBin(), .step = &lib.step, .compile = lib, .name = name }) catch @panic("OOM");
                }
            }
        }
        // JIT-only: ARM64 backend objects join static_objs right after
        // Jsrt.Core (before Common.Common), matching lib/CMakeLists.txt:21-28.
        // Non-jit profiles skip this block so their numbering and archive
        // layout remain unchanged.
        if (profile.isJit() and g == &tables.jsrt_core_group) {
            const bg = &tables.backend_group;
            backend_expected = @intCast(bg.cxx.len + bg.c.len + bg.asm_files.len);
            // A source added to backend_group must be reflected here.
            std.debug.assert(backend_expected == 89);
            for (bg.cxx) |src| {
                static_objs.append(arena, compileTu(&ctx, bg, src, .cxx, n)) catch @panic("OOM");
                n += 1;
                backend_n += 1;
            }
            for (bg.c) |src| {
                static_objs.append(arena, compileTu(&ctx, bg, src, .c, n)) catch @panic("OOM");
                n += 1;
                backend_n += 1;
            }
            for (bg.asm_files) |src| {
                static_objs.append(arena, compileTu(&ctx, bg, src, .asm_, n)) catch @panic("OOM");
                n += 1;
                backend_n += 1;
            }
            std.debug.assert(backend_n == backend_expected);
        }
    }
    // W0 wasm extras (jit+wasm only): WasmReader (11) + WasmLibrary (8,
    // incl. WabtInterface.cpp) + WABT (37) join the static graph after the
    // backend, numbered contiguously. Option-off adds nothing, so
    // expect_total is the historical total.
    var wasm_expected: u32 = 0;
    if (ctx.wasm) {
        const wasm_groups = [_]*const tables.Group{ &tables.wasmreader_group, &tables.wasm_library_group, &tables.wabt_group };
        for (wasm_groups) |g| {
            wasm_expected += @intCast(g.cxx.len + g.c.len + g.asm_files.len);
        }
        std.debug.assert(wasm_expected == 56);
        for (wasm_groups) |g| {
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
        }
    }
    // Static: 551 - 3 dylib TUs + TestHooks in ch = 549.
    // jit adds the 89 backend TUs on top of the shared 551 => 640.
    // jit+wasm adds 56 more => 696.
    const expect_base: u32 = if (is_static) 549 else 551;
    const expect_total: u32 = expect_base + backend_expected + wasm_expected;
    std.debug.assert(n == expect_total);
    const expect_singular: usize = if (is_static) 0 else 3;
    std.debug.assert(singular_libs.items.len == expect_singular);

    // Z1 leaf Zig SIMD codec (doc/zig-strategy.md section 5.3). Built outside C/C++ TU count
    // (`n`) so compile_commands / contract TU count remains 551.
    const u8a_codec_obj = b.addObject(.{
        .name = "u8a_codec",
        .root_module = b.createModule(.{
            .root_source_file = b.path("lib/ZigCodec/u8a_codec.zig"),
            .target = target,
            .optimize = .fast,
            .pic = true,
            .link_libc = false,
            .link_libcpp = false,
        }),
    });
    static_objs.append(arena, u8a_codec_obj.getEmittedBin()) catch @panic("OOM");

    // libChakraCoreStatic.a (contract section 5.2).
    // The jit profile's -g objects sum past 2GB. Zig's archiver then
    // fails with "failed to write: Unexpected" (the partial file is a
    // zero-filled multi-gigabyte blob). libtool writes that archive,
    // and sanitizer lanes also use libtool directly.
    const static_archive: LinkedArtifact = if (profile.isJit() or sanitize != .none) blk: {
        break :blk createLibtoolArchive(b, "ChakraCoreStatic", static_objs.items);
    } else blk: {
        const static_mod = createLinkModule(&ctx);
        for (static_objs.items) |o| static_mod.addObjectFile(o);
        const lib = b.addLibrary(.{ .name = "ChakraCoreStatic", .root_module = static_mod });
        break :blk .{ .bin = lib.getEmittedBin(), .step = &lib.step, .compile = lib, .name = "ChakraCoreStatic" };
    };

    // libChakraCore.dylib (contract section 5.6, shared profiles only):
    // 3 objects plus every engine TU linked directly - the equivalent
    // of the reference -force_load of the static archive (Zig's Mach-O
    // linker has no force-load spelling; the pilot joins the flag
    // instead, doc/build.md section 14). ICU via -L + -l; pthread/dl
    // fold into libc (libSystem).
    const dylib: ?LinkedArtifact = if (is_static) null else if (sanitize != .none) blk: {
        const link = b.addSystemCommand(&.{ctx.host_cxx});
        addSanitizeHostLinkPrefix(&ctx, link);
        link.addArgs(&.{
            "-dynamiclib",
            "-Wl,-compatibility_version,1.0.0",
            "-Wl,-current_version,1.0.0",
            "-Wl,-x",
            "-install_name",
            "@rpath/libChakraCore.dylib",
        });
        for (dylib_objs.items) |o| link.addFileArg(o);
        for (static_objs.items) |o| link.addFileArg(o);
        link.addArg(b.fmt("-L{s}", .{icu_lib}));
        link.addArgs(&.{ "-licuuc", "-licui18n", "-licudata", "-framework", "CoreFoundation", "-framework", "Security" });
        link.addArg("-o");
        const out = link.addOutputFileArg("libChakraCore.dylib");
        break :blk .{ .bin = out, .step = &link.step, .name = "ChakraCore" };
    } else blk: {
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
        // reference CMake sets no VERSION/SOVERSION - 1.0.0 is the toolchain
        // default on both sides, so the values coincide. Keep.
        const lib = b.addLibrary(.{
            .name = "ChakraCore",
            .root_module = dylib_mod,
            .linkage = .dynamic,
            .version = .{ .major = 1, .minor = 0, .patch = 0 },
        });
        lib.install_name = "@rpath/libChakraCore.dylib";
        lib.headerpad_max_install_names = true;
        lib.discard_local_symbols = true;
        break :blk .{ .bin = lib.getEmittedBin(), .step = &lib.step, .compile = lib, .name = "ChakraCore" };
    };

    // ch (contract section 5.7): shared profiles link 8 objects plus
    // the 3 singular archives plus ICU, with no link-time dependency on
    // the dylib (loaded at runtime from the executable's own directory);
    // static profiles link 9 objects (TestHooks in ch) plus every engine
    // TU directly - the -force_load equivalent, same join as the dylib.
    const ch: LinkedArtifact = if (sanitize != .none) blk: {
        const link = b.addSystemCommand(&.{ctx.host_cxx});
        addSanitizeHostLinkPrefix(&ctx, link);
        if (!profile.isDebug()) link.addArg("-Wl,-stack_size,0x800000");
        link.addArg("-Wl,-u,_OnChakraCoreLoadedEntry");
        for (ch_objs.items) |o| link.addFileArg(o);
        if (is_static) {
            for (static_objs.items) |o| link.addFileArg(o);
        } else {
            for (singular_libs.items) |lib| link.addFileArg(lib.bin);
        }
        link.addArg(b.fmt("-L{s}", .{icu_lib}));
        link.addArgs(&.{ "-licuuc", "-licui18n", "-licudata", "-framework", "CoreFoundation", "-framework", "Security" });
        link.addArg("-o");
        const out = link.addOutputFileArg("ch");
        link.step.dependOn(&jstoc.step);
        break :blk .{ .bin = out, .step = &link.step, .name = "ch" };
    } else blk: {
        const ch_mod = createLinkModule(&ctx);
        for (ch_objs.items) |o| ch_mod.addObjectFile(o);
        addSdkLinkPaths(&ctx, ch_mod);
        ch_mod.addLibraryPath(.{ .cwd_relative = icu_lib });
        ch_mod.linkSystemLibrary("icuuc", .{});
        ch_mod.linkSystemLibrary("icui18n", .{});
        ch_mod.linkSystemLibrary("icudata", .{});
        ch_mod.linkFramework("CoreFoundation", .{});
        ch_mod.linkFramework("Security", .{});
        if (is_static) {
            for (static_objs.items) |o| ch_mod.addObjectFile(o);
        } else {
            for (singular_libs.items) |lib| ch_mod.addObjectFile(lib.bin);
        }
        const exe = b.addExecutable(.{ .name = "ch", .root_module = ch_mod });
        // ch DEFINES _OnChakraCoreLoadedEntry (bin/ch/ch.cpp:46, extern "C") for
        // the dylib to import at runtime; retention in the executable comes from
        // default Mach-O executable export of a defined global, not from -u.
        // The call stays: verified effective and harmless (nm shows the as-built
        // ch exporting T _OnChakraCoreLoadedEntry).
        exe.forceUndefinedSymbol("_OnChakraCoreLoadedEntry");
        exe.headerpad_max_install_names = true;
        // LC_MAIN stacksize (U-B3.1-2): macOS makes RLIMIT_STACK immutable
        // once LC_MAIN stacksize is nonzero - Apple-clang probes show EVERY
        // setrlimit(RLIMIT_STACK) fails EINVAL (even a no-op set to the
        // current value), while stacksize 0 allows any set. PAL_Initialize
        // (pal/src/init/pal.cpp:140-155) lowers the inherited soft limit
        // (32MB here) to 8MB; the Debug-only _ASSERTE(err == 0) therefore
        // aborts any Debug ch linked with a nonzero stacksize, while
        // non-Debug builds fail the call silently and run fine. Legacy
        // links stacksize 0 on both executables, so debug profiles
        // (debug-static, jit-debug) set an explicit 0 (Zig's default would
        // be 16MB - null is NOT zero); test/release/jit keep the B2 8MB
        // contract (silent-failure path).
        exe.stack_size = if (profile.isDebug()) 0 else 8 * 1024 * 1024;
        exe.step.dependOn(&jstoc.step);
        break :blk .{ .bin = exe.getEmittedBin(), .step = &exe.step, .compile = exe, .name = "ch" };
    };

    // GCStress (contract section 5.8): 4 objects plus the static archive.
    // 8 MiB stack matches the reference behavior (the pilot applied the
    // same stack contract to every Mach-O executable edge).
    const gs: LinkedArtifact = if (sanitize != .none) blk: {
        const link = b.addSystemCommand(&.{ctx.host_cxx});
        addSanitizeHostLinkPrefix(&ctx, link);
        if (!profile.isDebug()) link.addArg("-Wl,-stack_size,0x800000");
        for (gs_objs.items) |o| link.addFileArg(o);
        link.addFileArg(static_archive.bin);
        link.addArgs(&.{ "-framework", "CoreFoundation", "-framework", "Security" });
        link.addArg("-o");
        const out = link.addOutputFileArg("GCStress");
        break :blk .{ .bin = out, .step = &link.step, .name = "GCStress" };
    } else blk: {
        const gs_mod = createLinkModule(&ctx);
        for (gs_objs.items) |o| gs_mod.addObjectFile(o);
        addSdkLinkPaths(&ctx, gs_mod);
        // Archive semantics: GCStress's stubs (StubExternalApi.cpp)
        // satisfy symbols that the engine also defines. Linking every
        // object would duplicate those symbols. A normal .a search
        // keeps the stubs and pulls the rest.
        gs_mod.addObjectFile(static_archive.bin);
        gs_mod.linkFramework("CoreFoundation", .{});
        gs_mod.linkFramework("Security", .{});
        const exe = b.addExecutable(.{ .name = "GCStress", .root_module = gs_mod });
        exe.headerpad_max_install_names = true;
        // Same U-B3.1-2 contract as ch above: GCStress links the same Debug
        // PAL_Initialize, so debug profiles stamp LC_MAIN stacksize 0 like
        // legacy; test/release/jit keep 8MB.
        exe.stack_size = if (profile.isDebug()) 0 else 8 * 1024 * 1024;
        break :blk .{ .bin = exe.getEmittedBin(), .step = &exe.step, .compile = exe, .name = "GCStress" };
    };

    // Node-API provider (plan N1.1 / PR-3, Wave 1): libChakraNodeApi, a
    // standalone shared library built from lib/NodeApi provider TUs and
    // linked against libChakraCore via Module/linkLibrary (the same Module
    // path as the dylib/ch links) -- NOT dynamic_lookup: the provider is a
    // dependent dylib of libChakraCore (LC_LOAD_DYLIB
    // @rpath/libChakraCore.dylib, resolved from the host's rpaths at
    // dlopen). Shared profiles only (test + jit); debug-static has no dylib
    // to link. Not part of the default build and alters no existing
    // artifact. The TU list is shared with the Linux graph via
    // tools/build/tables.zig (nodeapi_provider_tus). Provider TUs compile
    // via `zig c++` SystemCommands, mirroring the ch-nodeapi step below.
    //
    // Source selection: NodeApiEnv.cpp + NodeApiScopes.cpp +
    // NodeApiLifetime.cpp + NodeApiRefs.cpp + NodeApiValues.cpp +
    // NodeApiErrorsImpl.cpp + NodeApiObjects.cpp +
    // NodeApiFunctions.cpp + NodeApiBinary.cpp + NodeApiPromise.cpp +
    // NodeApiScript.cpp + NodeApiWrap.cpp + NodeApiAsyncContext.cpp
    // (all of lib/NodeApi). An
    // earlier draft excluded NodeApiRefs.cpp because its stand-in host
    // cast napi_env to a prefix-compatible NodeApiRefsEnv; N1.3 has since
    // reconciled onto an opaque NodeApiRefsState stored in
    // NodeEnv::refs_state with registry-resolved accessors (NodeApiEnv.h),
    // so it joins this library now. NodeApiFunctions.cpp (N2.4) joins
    // because the N2.3 joint stub it replaced was deleted: without it
    // NodeApiObjects.cpp's napi_define_properties has no definition of
    // napi_create_function and the library does not link (landed wiring
    // by N2.6; N2.4 owns the TU, not this list).
    const prov_lib: ?LinkedArtifact = if (is_static) null else blk: {
        var prov_objs: std.ArrayList(std.Build.LazyPath) = .empty;
        for (tables.nodeapi_provider_tus) |src| {
            const tu = if (sanitize != .none) cmd_blk: {
                const c = b.addSystemCommand(&.{ctx.host_cxx});
                c.addArg("-target");
                c.addArg(host_clang_target);
                c.addArg("-isysroot");
                c.addArg(ctx.sdk_path);
                c.addArg("-std=gnu++17");
                if (!profile.isDebug()) c.addArg("-O1");
                c.addArgs(&.{ "-g", "-fno-omit-frame-pointer", sanitize.fsanitizeFlag() });
                break :cmd_blk c;
            } else cmd_blk: {
                const c = b.addSystemCommand(&.{ ctx.zig_exe, "c++" });
                c.addArg("-target");
                c.addArg(profile_target);
                c.addArg("--sysroot");
                c.addArg(ctx.sdk_path);
                c.addArg("-std=gnu++17");
                c.addArg("-O3");
                c.addArg("-g");
                break :cmd_blk c;
            };
            tu.addArg("-fPIC");
            // HIGH-5: hidden by default. The 155 napi_*/node_api_*
            // declarations already carry __attribute__((visibility("default")))
            // via NAPI_EXTERN in the vendored headers, and the private
            // host entries carry it on their NodeApiEnv.cpp / NodeApiHost.cpp
            // definitions, so exactly those survive. nodeapi::* C++
            // internals, NodeApi* helpers, chakra_nodeapi_test_*
            // hooks, and operator new/delete stay hidden. The
            // provider explicitly does NOT define
            // CHAKRA_NODEAPI_TEST_HOOKS (release must not export
            // the root-counter hooks).
            tu.addArg("-fvisibility=hidden");
            tu.addArg("-DNAPI_VERSION=10");
            tu.addArg("-Ilib/Jsrt");
            tu.addArg("-Iinclude/nodeapi");
            tu.addArg("-Ilib/NodeApi");
            if (sanitize != .none) {
                tu.addArg("-mcpu=apple-m1");
            } else {
                tu.addArg("-isystem");
                tu.addArg(ctx.sdk_isystem);
                tu.addArg("-F");
                tu.addArg("/System/Library/Frameworks");
                tu.addArg("-mcpu=apple_m1");
            }
            tu.addArg("-Wno-date-time");
            tu.addArg("-o");
            const out = tu.addOutputFileArg(b.fmt("{s}.o", .{std.fs.path.basename(src)}));
            tu.addArg("-c");
            tu.addFileArg(b.path(src));
            prov_objs.append(arena, out) catch @panic("OOM");
        }
        if (sanitize != .none) {
            const link = b.addSystemCommand(&.{ctx.host_cxx});
            addSanitizeHostLinkPrefix(&ctx, link);
            link.addArgs(&.{
                "-dynamiclib",
                "-Wl,-compatibility_version,1.0.0",
                "-Wl,-current_version,1.0.0",
                "-install_name",
                "@rpath/libChakraNodeApi.dylib",
            });
            for (prov_objs.items) |o| link.addFileArg(o);
            link.addFileArg(dylib.?.bin);
            link.addArg("-o");
            const out = link.addOutputFileArg("libChakraNodeApi.dylib");
            link.step.dependOn(dylib.?.step);
            break :blk .{ .bin = out, .step = &link.step, .name = "ChakraNodeApi" };
        } else {
            const mod = createLinkModule(&ctx);
            for (prov_objs.items) |o| mod.addObjectFile(o);
            mod.linkLibrary(dylib.?.compile.?);
            addSdkLinkPaths(&ctx, mod);
            // No direct ICU / CoreFoundation / Security edges: the
            // provider references no ICU or framework symbols (verified
            // via `nm -u` on the built dylib: only Js* + libc/libSystem),
            // and the engine dylib already carries ICU as LC_LOAD_DYLIB.
            // Linking ICU here would add hardcoded -L paths that cannot
            // work under --sysroot (the ch-nodeapi host documents the
            // same for its SystemCommand link) and would duplicate the
            // engine's dependency. The Module link therefore keeps only
            // the dependent-dylib edge to libChakraCore.
            const lib = b.addLibrary(.{
                .name = "ChakraNodeApi",
                .root_module = mod,
                .linkage = .dynamic,
                .version = .{ .major = 1, .minor = 0, .patch = 0 },
            });
            lib.install_name = "@rpath/libChakraNodeApi.dylib";
            lib.headerpad_max_install_names = true;
            break :blk .{ .bin = lib.getEmittedBin(), .step = &lib.step, .compile = lib, .name = "ChakraNodeApi" };
        }
    };

    // B3.4 isolation: the unsanitized test profile owns the native install
    // system (the exact B2 tree, honoring --prefix); non-test profiles and
    // sanitizer lanes install NOTHING natively and copy out to fixed
    // out/<side_name>/ side prefixes instead (see copy_out below).
    if (side_name == null) {
        b.installArtifact(ch.compile.?);
        b.installArtifact(gs.compile.?);
        if (dylib) |lib| b.installArtifact(lib.compile.?);
        if (install_static_archive) {
            if (static_archive.compile) |c| {
                b.installArtifact(c);
            } else {
                b.getInstallStep().dependOn(&b.addInstallFile(static_archive.bin, "lib/libChakraCoreStatic.a").step);
            }
        }
        for (singular_libs.items) |lib| b.installArtifact(lib.compile.?);
        // Co-location edge (contract sections 5.6-5.7, shared profiles only): the
        // reference POST_BUILD copies both ch and libChakraCore.dylib to the
        // build root so the loader finds the dylib beside the executable
        // (ChakraRtInterface resolves <exe-dir>/libChakraCore.dylib); mirror
        // that by installing a second copy of the dylib beside zig-out/bin/ch.
        // Without this, as-installed ch exits 5 (dlopen "no such file").
        // Static ch needs no dylib (the engine is linked in).
        if (dylib) |lib| {
            b.getInstallStep().dependOn(&b.addInstallFile(lib.bin, "bin/libChakraCore.dylib").step);
        }
        b.getInstallStep().dependOn(&b.addInstallFile(config_h, "pal/src/config.h").step);
        // Staged public headers, unchanged (contract section 5.9).
        b.getInstallStep().dependOn(&b.addInstallFile(b.path("lib/Jsrt/ChakraCore.h"), "include/ChakraCore.h").step);
        b.getInstallStep().dependOn(&b.addInstallFile(b.path("lib/Jsrt/ChakraCommon.h"), "include/ChakraCommon.h").step);
        b.getInstallStep().dependOn(&b.addInstallFile(b.path("lib/Jsrt/ChakraDebug.h"), "include/ChakraDebug.h").step);
        b.getInstallStep().dependOn(&b.addInstallFile(b.path("lib/Common/ChakraCoreVersion.h"), "include/ChakraCoreVersion.h").step);
        // .profile marker: names the profile whose artifacts this prefix
        // holds (asserted by check_metadata.py; non-test markers are
        // written by install_profile.py instead).
        const marker = wf.add(".profile", arena.dupe(u8, "test\n") catch @panic("OOM"));
        b.getInstallStep().dependOn(&b.addInstallFile(marker, ".profile").step);
    }

    // B3.4 copy-out (non-test profiles and sanitizer lanes): install_profile.py
    // mirrors the B2 relative tree into out/<side_name>/. The step runs on
    // every build (has_side_effects) but writes only changed bytes, so no-op
    // rebuilds change zero artifact bytes while manually deleted artifacts
    // self-heal. `--map=` entries are prefixed file args, so every source
    // artifact both orders and fingerprints this step.
    const copy_out: ?*std.Build.Step.Run = if (side_name == null) null else blk: {
        const sname = side_name.?;
        const cp = b.addSystemCommand(&.{python});
        cp.addFileArg(b.path("tools/build/install_profile.py"));
        cp.addArg("--profile");
        cp.addArg(sname);
        cp.addArg("--dest");
        cp.addArg(b.fmt("out/{s}", .{sname}));
        cp.addPrefixedFileArg("--map=bin/ch=", ch.bin);
        cp.addPrefixedFileArg("--map=bin/GCStress=", gs.bin);
        if (dylib) |lib| {
            cp.addPrefixedFileArg("--map=lib/libChakraCore.1.0.0.dylib=", lib.bin);
            cp.addPrefixedFileArg("--map=bin/libChakraCore.dylib=", lib.bin);
            cp.addArg("--symlink=lib/libChakraCore.dylib=libChakraCore.1.dylib");
            cp.addArg("--symlink=lib/libChakraCore.1.dylib=libChakraCore.1.0.0.dylib");
        }
        // Node-API provider (N1.1, N5.5): out/<side_name>/lib/libChakraNodeApi.dylib
        // plus the vendored N-API headers at out/<side_name>/include/nodeapi/
        // (consumer builds need both; the private host header
        // lib/NodeApi/NodeApiHostApi.h is intentionally NOT staged).
        if (prov_lib) |lib| {
            cp.addPrefixedFileArg("--map=lib/libChakraNodeApi.dylib=", lib.bin);
            for ([_][]const u8{ "include/nodeapi/js_native_api.h", "include/nodeapi/js_native_api_types.h", "include/nodeapi/node_api.h", "include/nodeapi/node_api_types.h" }) |hdr| {
                cp.addPrefixedFileArg(b.fmt("--map={s}=", .{hdr}), b.path(b.fmt("include/nodeapi/{s}", .{std.fs.path.basename(hdr)})));
            }
        }
        if (install_static_archive) {
            cp.addPrefixedFileArg("--map=lib/libChakraCoreStatic.a=", static_archive.bin);
        }
        for (singular_libs.items) |lib| {
            cp.addPrefixedFileArg(b.fmt("--map=lib/lib{s}.a=", .{lib.name}), lib.bin);
        }
        cp.addPrefixedFileArg("--map=pal/src/config.h=", config_h);
        cp.addPrefixedFileArg("--map=include/ChakraCore.h=", b.path("lib/Jsrt/ChakraCore.h"));
        cp.addPrefixedFileArg("--map=include/ChakraCommon.h=", b.path("lib/Jsrt/ChakraCommon.h"));
        cp.addPrefixedFileArg("--map=include/ChakraDebug.h=", b.path("lib/Jsrt/ChakraDebug.h"));
        cp.addPrefixedFileArg("--map=include/ChakraCoreVersion.h=", b.path("lib/Common/ChakraCoreVersion.h"));
        cp.stdio = .inherit;
        cp.has_side_effects = true;
        b.getInstallStep().dependOn(&cp.step);
        break :blk cp;
    };

    // Per-profile / per-sanitizer uninstall: test uses the native uninstall
    // (installed files only); side-prefix builds remove their whole side prefix.
    if (side_name) |sname| {
        const rm = b.addSystemCommand(&.{ python, "-c", "import shutil,sys; shutil.rmtree(sys.argv[1], ignore_errors=True); print('removed ' + sys.argv[1] + ' (or already absent)')" });
        const root = b.root.root_dir.path orelse
            fail("cannot resolve the build root path; refusing a CWD-relative uninstall", .{});
        rm.addArg(b.fmt("{s}/out/{s}", .{ root, sname }));
        rm.stdio = .inherit;
        rm.has_side_effects = true;
        b.getUninstallStep().dependOn(&rm.step);
    }

    // Node-API provider step (N1.1, PR-3, N5.5): `zig build libChakraNodeApi`
    // builds libChakraNodeApi (shared profiles only). Test profile installs
    // it to <prefix>/lib/libChakraNodeApi.dylib (beside the ch-nodeapi
    // host's default provider lookup) plus the vendored headers to
    // <prefix>/include/nodeapi/ (N5.5 consumer builds need both);
    // side-prefix builds copy dylib + headers to out/<side_name>/ via copy_out.
    var prov_step_opt: ?*std.Build.Step = null;
    if (prov_lib) |lib| {
        const prov_step = b.step("libChakraNodeApi", "Build libChakraNodeApi (N1.1 provider) linked against libChakraCore (shared profiles; not part of the default build)");
        prov_step_opt = prov_step;
        if (side_name == null) {
            prov_step.dependOn(&b.addInstallFile(lib.bin, "lib/libChakraNodeApi.dylib").step);
            for ([_][]const u8{ "js_native_api.h", "js_native_api_types.h", "node_api.h", "node_api_types.h" }) |hdr| {
                prov_step.dependOn(&b.addInstallFile(
                    b.path(b.fmt("include/nodeapi/{s}", .{hdr})),
                    b.fmt("include/nodeapi/{s}", .{hdr}),
                ).step);
            }
        } else {
            prov_step.dependOn(&copy_out.?.step);
        }
    }

    // Node-API exports gate (N1.6/N5.5, HIGH-5): `zig build check-nodeapi`
    // verifies libChakraNodeApi exports EXACTLY the allowlist in
    // tools/build/nodeapi_exports.txt (full 155 napi_*/node_api_* + 6 host = 161)
    // and that the allowlist matches its generator.
    if (prov_lib) |lib| {
        const exports_gen_check = b.addSystemCommand(&.{python});
        exports_gen_check.addFileArg(b.path("tools/build/gen_nodeapi_exports.py"));
        exports_gen_check.addArg("--check");
        exports_gen_check.stdio = .inherit;
        const exports_check = b.addSystemCommand(&.{python});
        exports_check.addFileArg(b.path("tools/build/check_nodeapi.py"));
        exports_check.addPrefixedFileArg("--dylib=", lib.bin);
        exports_check.addPrefixedFileArg("--exports=", b.path("tools/build/nodeapi_exports.txt"));
        exports_check.addPrefixedFileArg("--inventory=", b.path("doc/node-api-inventory.md"));
        exports_check.step.dependOn(lib.step);
        exports_check.step.dependOn(&exports_gen_check.step);
        exports_check.stdio = .inherit;
        const check_nodeapi_top = b.step("check-nodeapi", "Verify libChakraNodeApi exports exactly tools/build/nodeapi_exports.txt (full 155 + 6 = 161; not part of the default build or check)");
        check_nodeapi_top.dependOn(&exports_gen_check.step);
        check_nodeapi_top.dependOn(&exports_check.step);
    }

    // N0.3 Node-API test host (doc/node-api-plan.md N0.3, shared profiles):
    // standalone ch-nodeapi executable + test-only stub provider + addons.
    var nodeapi_step_opt: ?*std.Build.Step = null;
    if (dylib) |engine_compile| {
        const nodeapi_step = b.step("ch-nodeapi", "Build the Node-API test host (N0.3): ch-nodeapi + test-only stub provider + hello/negative addons (shared profiles only; not part of the default build)");
        nodeapi_step_opt = nodeapi_step;
        const engine_lib = engine_compile.bin;

        const addSideCopy = struct {
            fn call(builder: *std.Build, py: []const u8, dest_rel: []const u8, src: std.Build.LazyPath, prereq: *std.Build.Step) *std.Build.Step.Run {
                const scp = builder.addSystemCommand(&.{
                    py,
                    "-c",
                    "import os, shutil, sys, tempfile; src, dst = sys.argv[1], sys.argv[2]; d = os.path.dirname(os.path.abspath(dst)); os.makedirs(d, exist_ok=True); fd, tmp = tempfile.mkstemp(dir=d, prefix='.tmp.'); os.close(fd); shutil.copy2(src, tmp); os.replace(tmp, dst)",
                });
                scp.addFileArg(src);
                scp.addArg(dest_rel);
                scp.has_side_effects = true;
                scp.step.dependOn(prereq);
                return scp;
            }
        }.call;

        const addNodeapiCompilePrefix = struct {
            fn call(context: *const Ctx, cmd: *std.Build.Step.Run) void {
                if (context.sanitize != .none) {
                    cmd.addArgs(&.{ "-target", host_clang_target, "-isysroot", context.sdk_path, "-std=gnu++17" });
                    if (!context.profile.isDebug()) cmd.addArg("-O1");
                    cmd.addArgs(&.{ "-g", "-fno-omit-frame-pointer", context.sanitize.fsanitizeFlag() });
                } else {
                    cmd.addArgs(&.{ "-target", profile_target, "--sysroot", context.sdk_path, "-std=gnu++17", "-O3", "-g" });
                }
            }
        }.call;
        const addNodeapiTail = struct {
            fn call(context: *const Ctx, cmd: *std.Build.Step.Run) void {
                if (context.sanitize != .none) {
                    cmd.addArg("-mcpu=apple-m1");
                } else {
                    cmd.addArg("-isystem");
                    cmd.addArg(context.sdk_isystem);
                    cmd.addArg("-F");
                    cmd.addArg("/System/Library/Frameworks");
                    cmd.addArg("-mcpu=apple_m1");
                }
                cmd.addArg("-Wno-date-time");
            }
        }.call;

        const nodeapi_host = if (sanitize != .none) b.addSystemCommand(&.{ctx.host_cxx}) else b.addSystemCommand(&.{ ctx.zig_exe, "c++" });
        addNodeapiCompilePrefix(&ctx, nodeapi_host);
        nodeapi_host.addArg("-DNAPI_VERSION=10");
        nodeapi_host.addArg("-Ilib/Jsrt");
        nodeapi_host.addArg("-Iinclude/nodeapi");
        nodeapi_host.addArg("-Ilib/NodeApi");
        addNodeapiTail(&ctx, nodeapi_host);
        nodeapi_host.addArg("-o");
        const nodeapi_host_bin = nodeapi_host.addOutputFileArg("ch-nodeapi");
        nodeapi_host.addFileArg(b.path("tools/nodeapi/ch-nodeapi.cpp"));
        nodeapi_host.addFileArg(engine_lib);
        nodeapi_host.addArg("-framework");
        nodeapi_host.addArg("CoreFoundation");
        nodeapi_host.addArg("-framework");
        nodeapi_host.addArg("Security");
        nodeapi_host.addArg("-Wl,-undefined,dynamic_lookup");
        nodeapi_host.addArg("-Wl,-rpath,@executable_path");
        nodeapi_host.addArg("-Wl,-rpath,@executable_path/../lib");
        nodeapi_host.step.dependOn(engine_compile.step);

        const nodeapi_stub = if (sanitize != .none) b.addSystemCommand(&.{ctx.host_cxx}) else b.addSystemCommand(&.{ ctx.zig_exe, "c++" });
        addNodeapiCompilePrefix(&ctx, nodeapi_stub);
        nodeapi_stub.addArg("-fPIC");
        nodeapi_stub.addArg("-shared");
        nodeapi_stub.addArg("-Wl,-undefined,dynamic_lookup");
        nodeapi_stub.addArg("-DNAPI_VERSION=10");
        nodeapi_stub.addArg("-Ilib/Jsrt");
        nodeapi_stub.addArg("-Iinclude/nodeapi");
        nodeapi_stub.addArg("-Ilib/NodeApi");
        addNodeapiTail(&ctx, nodeapi_stub);
        nodeapi_stub.addArg("-o");
        const nodeapi_stub_bin = nodeapi_stub.addOutputFileArg("libChakraNodeApiStub.dylib");
        nodeapi_stub.addFileArg(b.path("tools/nodeapi/stub/stub_provider.cpp"));

        if (side_name == null) {
            nodeapi_step.dependOn(&b.addInstallFile(nodeapi_host_bin, "bin/ch-nodeapi").step);
            nodeapi_step.dependOn(&b.addInstallFile(engine_lib, "bin/libChakraCore.dylib").step);
            nodeapi_step.dependOn(&b.addInstallFile(nodeapi_stub_bin, "lib/libChakraNodeApiStub.dylib").step);
        } else {
            const side_root = b.fmt("out/{s}", .{side_name.?});
            const cp_step = &copy_out.?.step;
            nodeapi_step.dependOn(&addSideCopy(b, python, b.fmt("{s}/bin/ch-nodeapi", .{side_root}), nodeapi_host_bin, cp_step).step);
            nodeapi_step.dependOn(&addSideCopy(b, python, b.fmt("{s}/lib/libChakraNodeApiStub.dylib", .{side_root}), nodeapi_stub_bin, cp_step).step);
        }
        for ([_][]const u8{ "hello_world", "version_reject", "missing_register", "promise_probe", "return_replaces" }) |name| {
            const addon = if (sanitize != .none) b.addSystemCommand(&.{ctx.host_cxx}) else b.addSystemCommand(&.{ ctx.zig_exe, "c++" });
            addNodeapiCompilePrefix(&ctx, addon);
            addon.addArg("-fPIC");
            addon.addArg("-shared");
            addon.addArg("-Wl,-undefined,dynamic_lookup");
            addon.addArg("-DNAPI_VERSION=10");
            addon.addArg("-Iinclude/nodeapi");
            addNodeapiTail(&ctx, addon);
            addon.addArg("-o");
            const addon_bin = addon.addOutputFileArg(b.fmt("{s}.node", .{name}));
            addon.addFileArg(b.path(b.fmt("tools/nodeapi/addons/{s}.cpp", .{name})));
            if (side_name == null) {
                nodeapi_step.dependOn(&b.addInstallFile(addon_bin, b.fmt("lib/nodeapi-addons/{s}.node", .{name})).step);
            } else {
                const side_root = b.fmt("out/{s}", .{side_name.?});
                nodeapi_step.dependOn(&addSideCopy(b, python, b.fmt("{s}/lib/nodeapi-addons/{s}.node", .{ side_root, name }), addon_bin, &copy_out.?.step).step);
            }
        }
        for ([_][]const u8{ "hello_world", "promise_probe", "return_replaces" }) |name| {
            const src_js = b.path(b.fmt("tools/nodeapi/addons/{s}.js", .{name}));
            if (side_name == null) {
                nodeapi_step.dependOn(&b.addInstallFile(src_js, b.fmt("lib/nodeapi-addons/{s}.js", .{name})).step);
            } else {
                const side_root = b.fmt("out/{s}", .{side_name.?});
                nodeapi_step.dependOn(&addSideCopy(b, python, b.fmt("{s}/lib/nodeapi-addons/{s}.js", .{ side_root, name }), src_js, &copy_out.?.step).step);
            }
        }
    } else {
        const no_nodeapi = b.addSystemCommand(&.{python});
        no_nodeapi.addArg("-c");
        no_nodeapi.addArg("import sys; sys.exit('-Dprofile=debug-static has no shared dylib; ch-nodeapi requires a shared profile (test, jit, or release-shared)')");
        no_nodeapi.stdio = .inherit;
        const no_nodeapi_top = b.step("ch-nodeapi", "Not applicable on debug-static (requires a shared profile)");
        no_nodeapi_top.dependOn(&no_nodeapi.step);
    }

    // Negative controls: each check script's --self-test proves its
    // gates CAN fail (doctored logs rejected, tampered victims fail).
    // Hermetic (no install dependency, no profile dependence), so every
    // profile registers it. Run `zig build check-selftest`.
    const selftest_top = b.step("check-selftest", "Negative controls for every check gate (not part of the default build)");
    for ([_][]const u8{ "check_regression.py", "check_native.py", "check_smoke.py", "check_metadata.py", "check_regression_linux.py", "check_smoke_linux.py", "check_metadata_linux.py", "check_nodeapi.py", "gen_nodeapi_exports.py", "check_wasm_p1.py", "check_gcstress.py", "check_fulljit.py", "check_sanitize.py" }) |script| {
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
    // Native-owner example tests (doc/memory-management.md section 5):
    // hermetic, host-only, no engine build required.
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
    const tools_test_top = b.step("check-build-tools", "Failure-path tests for the B1-B3 verification tools (no engine build required)");
    tools_test_top.dependOn(&tools_test.step);

    // Wasm P1 gate: requires -Dprofile=jit|jit-debug -Dwasm=true; fails loudly otherwise.
    var jit_wasm_step: ?*std.Build.Step = null;
    var jit_wasm_jit_step: ?*std.Build.Step = null;
    if (profile.isJit() and profile.hasDebugFlags() and ctx.wasm) {
        const jit_wasm = b.addSystemCommand(&.{python});
        jit_wasm.addFileArg(b.path("tools/build/check_wasm_p1.py"));
        jit_wasm.addFileArg(b.path("test/runtests.py"));
        jit_wasm.addArg(b.fmt("out/{s}/bin/ch", .{side_name.?}));
        jit_wasm.addFileInput(ch.bin);
        jit_wasm.step.dependOn(&copy_out.?.step);
        jit_wasm.stdio = .inherit;
        const wasm_top = b.step("check-wasm", "Verify Wasm P1 suite against test/wasm-arm64-manifest.json (-Dprofile=jit|jit-debug -Dwasm=true; not part of the default build)");
        wasm_top.dependOn(&jit_wasm.step);
        jit_wasm_step = &jit_wasm.step;

        const jit_wasm_jit = b.addSystemCommand(&.{python});
        jit_wasm_jit.addFileArg(b.path("tools/build/check_wasm_p1.py"));
        jit_wasm_jit.addArg("--jit");
        jit_wasm_jit.addFileArg(b.path("test/runtests.py"));
        jit_wasm_jit.addArg(b.fmt("out/{s}/bin/ch", .{side_name.?}));
        jit_wasm_jit.addFileInput(ch.bin);
        jit_wasm_jit.step.dependOn(&copy_out.?.step);
        jit_wasm_jit.stdio = .inherit;
        const wasm_jit_top = b.step("check-wasm-jit", "Verify Wasm P1 suite with native JIT tier-up (--jit dynapogo; -Dprofile=jit|jit-debug -Dwasm=true; not part of the default build)");
        wasm_jit_top.dependOn(&jit_wasm_jit.step);
        jit_wasm_jit_step = &jit_wasm_jit.step;
    } else {
        registerFailStep(b, python, "check-wasm", "Not applicable without -Dprofile=jit|jit-debug -Dwasm=true", "check-wasm requires -Dprofile=jit or jit-debug with -Dwasm=true");
        registerFailStep(b, python, "check-wasm-jit", "Not applicable without -Dprofile=jit|jit-debug -Dwasm=true", "check-wasm-jit requires -Dprofile=jit or jit-debug with -Dwasm=true");
    }

    // Bounded GCStress smoke gate (in-thread + concurrent full heap walks).
    {
        const gc_step = b.addSystemCommand(&.{python});
        gc_step.addFileArg(b.path("tools/build/check_gcstress.py"));
        if (side_name) |sname| {
            gc_step.addArg(b.fmt("out/{s}/bin/GCStress", .{sname}));
            gc_step.addFileInput(gs.bin);
            gc_step.step.dependOn(&copy_out.?.step);
        } else {
            gc_step.addFileArg(.{ .relative = .{ .base = .install_bin, .sub_path = "GCStress" } });
            gc_step.step.dependOn(b.getInstallStep());
        }
        gc_step.stdio = .inherit;
        const gc_top = b.step("check-gcstress", "Run bounded GCStress in-thread and concurrent heap walks (not part of the default build)");
        gc_top.dependOn(&gc_step.step);
    }

    // No-profile FullJit correctness gate (B4-JIT-05): requires JIT with debug/test flags (-Dprofile=jit|jit-debug).
    var jit_fulljit_step: ?*std.Build.Step = null;
    if (profile.isJit() and profile.hasDebugFlags()) {
        const fj_step = b.addSystemCommand(&.{python});
        fj_step.addFileArg(b.path("tools/build/check_fulljit.py"));
        fj_step.addFileArg(b.path("test/runtests.py"));
        fj_step.addArg(b.fmt("out/{s}/bin/ch", .{side_name.?}));
        fj_step.addArg("--select");
        fj_step.addArg(if (profile.isDebug()) "debug" else "test");
        fj_step.addFileInput(ch.bin);
        fj_step.step.dependOn(&copy_out.?.step);
        fj_step.stdio = .inherit;
        const fj_top = b.step("check-fulljit", "Run no-profile FullJit regression suites (-Dprofile=jit|jit-debug; not part of the default build)");
        fj_top.dependOn(&fj_step.step);
        jit_fulljit_step = &fj_step.step;
    } else {
        registerFailStep(b, python, "check-fulljit", "Not applicable without -Dprofile=jit|jit-debug", "check-fulljit requires -Dprofile=jit or -Dprofile=jit-debug (JIT must be enabled with debug/test flags)");
    }

    // Sanitizer verification gate: requires -Dsanitize=address|undefined.
    if (sanitize != .none) {
        const san_step = b.addSystemCommand(&.{python});
        san_step.addFileArg(b.path("tools/build/check_sanitize.py"));
        san_step.addArg(b.fmt("out/{s}", .{side_name.?}));
        san_step.addArg("--mode");
        san_step.addArg(sanitize.name());
        san_step.addArg("--profile");
        san_step.addArg(profile.name());
        san_step.addArg(if (ctx.wasm) "--expect-wasm" else "--no-expect-wasm");
        san_step.addArg("--cxx");
        san_step.addArg(ctx.host_cxx);
        san_step.addArg("--sdk-path");
        san_step.addArg(ctx.sdk_path);
        san_step.addFileInput(ch.bin);
        san_step.step.dependOn(&copy_out.?.step);
        if (prov_step_opt) |ps| san_step.step.dependOn(ps);
        if (nodeapi_step_opt) |ns| san_step.step.dependOn(ns);
        san_step.stdio = .inherit;
        const san_top = b.step("check-sanitize", b.fmt("Run ASan/UBSan verification gate for -Dsanitize={s} (not part of the default build)", .{sanitize.name()}));
        san_top.dependOn(&san_step.step);
    } else {
        registerFailStep(b, python, "check-sanitize", "Not applicable without -Dsanitize=address|undefined", "check-sanitize requires -Dsanitize=address or -Dsanitize=undefined");
    }

    // B3.3: release-shared checks (legacy baselines in
    // doc/b3-profiles.md section 5). The legacy Release `ninja check` is a
    // SMOKE GATE, not the full suite (test/CMakeLists.txt:23-45):
    // smoke-check (ch hello.js) plus native-check, and nothing
    // else - no regression leg, no bytecode leg. The zig release
    // check mirrors that scope EXACTLY: output-asserted smoke (ch
    // -v, the Release-silent invalid flag, snippet, hello.js),
    // shared embedding with the legacy-exact Release argv, and
    // metadata with DIRECT otool observation of the release
    // binaries' 8 MiB stacks. check-regression and verify-bytecode
    // fail loudly (by legacy design, not a missing feature) so a
    // "green by omission" mistake is impossible. B3.4 paths: plain
    // out/release-shared strings (the side prefix is fixed, not
    // --prefix-relative); each step depends on the copy-out (ordering)
    // and fingerprints the pre-copy emitted artifacts (same bytes).
    if (profile.isRelease()) {
        const cp_step = &copy_out.?.step;
        const sname = side_name.?;
        const rel_ch_str = b.fmt("out/{s}/bin/ch", .{sname});
        const rel_prefix_str = b.fmt("out/{s}", .{sname});
        const rel_cc = b.findProgram(.{ .names = &.{"clang"} }) orelse "clang";
        const rel_cxx = b.findProgram(.{ .names = &.{"clang++"} }) orelse "clang++";

        const rel_native = b.addSystemCommand(&.{python});
        rel_native.addFileArg(b.path("tools/build/check_native.py"));
        rel_native.addFileArg(b.path("test/native-tests/test_native.sh"));
        rel_native.addArg(rel_ch_str);
        rel_native.addFileInput(ch.bin);
        rel_native.addArg(rel_cc);
        rel_native.addArg(rel_cxx);
        rel_native.addArg("--build-type");
        rel_native.addArg("Release");
        rel_native.step.dependOn(cp_step);
        rel_native.stdio = .inherit;
        const rel_native_top = b.step("check-native", b.fmt("Run the shared embedding tests against {s} artifacts (legacy-exact Release argv; not part of the default build)", .{sname}));
        rel_native_top.dependOn(&rel_native.step);

        const rel_smoke = b.addSystemCommand(&.{python});
        rel_smoke.addFileArg(b.path("tools/build/check_smoke.py"));
        rel_smoke.addArg(rel_ch_str);
        rel_smoke.addFileInput(ch.bin);
        rel_smoke.addArg("--profile");
        rel_smoke.addArg("release-shared");
        rel_smoke.step.dependOn(cp_step);
        rel_smoke.stdio = .inherit;
        const rel_smoke_top = b.step("check-smoke", b.fmt("Assert {s} ch version/silent-flag/snippet/hello.js output text (not part of the default build)", .{sname}));
        rel_smoke_top.dependOn(&rel_smoke.step);

        const rel_metadata = b.addSystemCommand(&.{python});
        rel_metadata.addFileArg(b.path("tools/build/check_metadata.py"));
        rel_metadata.addArg(rel_prefix_str);
        rel_metadata.addFileInput(ch.bin);
        rel_metadata.addFileInput(config_h);
        rel_metadata.addArg("--profile");
        rel_metadata.addArg(sname);
        rel_metadata.step.dependOn(cp_step);
        rel_metadata.stdio = .inherit;
        const rel_metadata_top = b.step("check-metadata", b.fmt("Assert {s} artifact metadata: minos, 8MiB stacks (direct otool), headers, exports (not part of the default build)", .{sname}));
        rel_metadata_top.dependOn(&rel_metadata.step);

        const rel_check_top = b.step("check", b.fmt("Verification for {s}: smoke gate, shared native embedding, metadata{s} (not part of the default build)", .{
            sname,
            if (profile == .release_jit) ", JIT symbols/smoke, JIT bytecode" else "",
        }));
        rel_check_top.dependOn(&rel_smoke.step);
        rel_check_top.dependOn(&rel_native.step);
        rel_check_top.dependOn(&rel_metadata.step);
        rel_check_top.dependOn(selftest_top);

        if (profile == .release_jit) {
            const rel_jit = b.addSystemCommand(&.{python});
            rel_jit.addFileArg(b.path("tools/build/check_jit.py"));
            rel_jit.addArg(rel_ch_str);
            rel_jit.addFileInput(ch.bin);
            rel_jit.step.dependOn(cp_step);
            rel_jit.stdio = .inherit;
            const rel_jit_top = b.step("check-jit", "Verify Release+JIT backend symbols and non-flaglist hot-call JIT execution (not part of the default build)");
            rel_jit_top.dependOn(&rel_jit.step);
            rel_check_top.dependOn(&rel_jit.step);

            const rel_verify_bc = b.addSystemCommand(&.{python});
            rel_verify_bc.addFileArg(b.path("tools/build/verify_bytecode_pure.py"));
            rel_verify_bc.addFileArg(b.path("tools/regenByteCode.py"));
            rel_verify_bc.addArg("--jit");
            rel_verify_bc.addArg("--verify");
            rel_verify_bc.addArg(b.fmt("--binary={s}", .{rel_ch_str}));
            rel_verify_bc.addFileInput(ch.bin);
            rel_verify_bc.step.dependOn(cp_step);
            rel_verify_bc.stdio = .inherit;
            const rel_verify_bc_top = b.step("verify-bytecode", "Verify checked-in JIT built-in bytecode with the release-jit ch (pure scratch-tree check; not part of the default build)");
            rel_verify_bc_top.dependOn(&rel_verify_bc.step);
            rel_check_top.dependOn(&rel_verify_bc.step);
        } else {
            registerFailStep(
                b,
                python,
                "verify-bytecode",
                "Not applicable: legacy Release check has no bytecode leg",
                "-Dprofile=release-shared: no bytecode leg - the legacy Release check has no bytecode-check (test/CMakeLists.txt:23-45)",
            );
        }

        registerFailStep(
            b,
            python,
            "check-regression",
            b.fmt("Not applicable for {s}: check_regression.py targets disable_jit test/debug pins", .{sname}),
            b.fmt("-Dprofile={s}: no regression leg (Release builds omit debug config flaglist)", .{sname}),
        );
        registerPackageSteps(b, python, true, rel_prefix_str, sname, cp_step, prov_step_opt, nodeapi_step_opt);
        return;
    }

    // B3.2: debug-static checks (legacy baselines in
    // doc/b3-profiles.md section 5, pins in
    // tools/build/gate_counts.json). Same five gates as test, with the
    // debug/static expectations: `--debug` regression (debug pin),
    // bytecode --noJit --verify, 8 EXECUTED static native tests
    // (PASS requires build+run to SUCCESS, plus archive-symbol and
    // Makefile linkage evidence), output-asserted smoke, and
    // static-aware metadata (minos 26.0, LC_MAIN stacksize 0 - the
    // U-B3.1-2 pin - headers, no dylib, 526-member archive with JsRT
    // symbols, self-contained ch). B3.4 paths: plain out/debug-static
    // strings (the side prefix is fixed, not --prefix-relative); each
    // step depends on the copy-out (ordering) and fingerprints the
    // pre-copy emitted artifacts (same bytes). The test block below
    // is untouched.
    if (profile == .debug_static) {
        const cp_step = &copy_out.?.step;
        const sname = side_name.?;
        const dbg_ch_str = b.fmt("out/{s}/bin/ch", .{sname});
        const dbg_archive_str = b.fmt("out/{s}/lib/libChakraCoreStatic.a", .{sname});
        const dbg_prefix_str = b.fmt("out/{s}", .{sname});
        const dbg_cc = b.findProgram(.{ .names = &.{"clang"} }) orelse "clang";
        const dbg_cxx = b.findProgram(.{ .names = &.{"clang++"} }) orelse "clang++";

        const dbg_verify_bc = b.addSystemCommand(&.{python});
        dbg_verify_bc.addFileArg(b.path("tools/build/verify_bytecode_pure.py"));
        dbg_verify_bc.addFileArg(b.path("tools/regenByteCode.py"));
        dbg_verify_bc.addArg("--noJit");
        dbg_verify_bc.addArg("--verify");
        dbg_verify_bc.addArg(b.fmt("--binary={s}", .{dbg_ch_str}));
        dbg_verify_bc.addFileInput(ch.bin);
        dbg_verify_bc.step.dependOn(cp_step);
        dbg_verify_bc.stdio = .inherit;
        const dbg_verify_bc_top = b.step("verify-bytecode", "Verify checked-in built-in bytecode with the debug-static ch (contract 5.10; pure check; not part of the default build)");
        dbg_verify_bc_top.dependOn(&dbg_verify_bc.step);

        const dbg_regression = b.addSystemCommand(&.{python});
        dbg_regression.addFileArg(b.path("tools/build/check_regression.py"));
        dbg_regression.addFileArg(b.path("test/runtests.py"));
        dbg_regression.addArg(dbg_ch_str);
        dbg_regression.addFileInput(ch.bin);
        dbg_regression.addArg("--select");
        dbg_regression.addArg("debug");
        dbg_regression.addArg("--expected-passed");
        dbg_regression.addArg(b.fmt("{d}", .{gates.debug_passed}));
        dbg_regression.addArg("--expected-failed");
        dbg_regression.addArg(b.fmt("{d}", .{gates.debug_failed}));
        dbg_regression.step.dependOn(cp_step);
        dbg_regression.stdio = .inherit;
        const dbg_regression_top = b.step("check-regression", b.fmt("Run the regression suite (--debug) against the debug-static ch, require {d} passed / {d} failed (not part of the default build)", .{ gates.debug_passed, gates.debug_failed }));
        dbg_regression_top.dependOn(&dbg_regression.step);

        const dbg_native = b.addSystemCommand(&.{python});
        dbg_native.addFileArg(b.path("tools/build/check_native.py"));
        dbg_native.addFileArg(b.path("test/native-tests/test_native.sh"));
        dbg_native.addArg(dbg_ch_str);
        dbg_native.addFileInput(ch.bin);
        dbg_native.addArg(dbg_cc);
        dbg_native.addArg(dbg_cxx);
        dbg_native.addArg("--static");
        dbg_native.addArg(b.fmt("--archive={s}", .{dbg_archive_str}));
        dbg_native.addFileInput(static_archive.bin);
        dbg_native.step.dependOn(cp_step);
        dbg_native.stdio = .inherit;
        const dbg_native_top = b.step("check-native", "Run the static embedding tests against debug-static artifacts, with linkage proof (not part of the default build)");
        dbg_native_top.dependOn(&dbg_native.step);

        const dbg_smoke = b.addSystemCommand(&.{python});
        dbg_smoke.addFileArg(b.path("tools/build/check_smoke.py"));
        dbg_smoke.addArg(dbg_ch_str);
        dbg_smoke.addFileInput(ch.bin);
        dbg_smoke.addArg("--profile");
        dbg_smoke.addArg("debug-static");
        dbg_smoke.step.dependOn(cp_step);
        dbg_smoke.stdio = .inherit;
        const dbg_smoke_top = b.step("check-smoke", "Assert debug-static ch version/flag/snippet output text (not part of the default build)");
        dbg_smoke_top.dependOn(&dbg_smoke.step);

        const dbg_metadata = b.addSystemCommand(&.{python});
        dbg_metadata.addFileArg(b.path("tools/build/check_metadata.py"));
        dbg_metadata.addArg(dbg_prefix_str);
        dbg_metadata.addFileInput(ch.bin);
        dbg_metadata.addFileInput(config_h);
        dbg_metadata.addArg("--profile");
        dbg_metadata.addArg(sname);
        dbg_metadata.step.dependOn(cp_step);
        dbg_metadata.stdio = .inherit;
        const dbg_metadata_top = b.step("check-metadata", "Assert debug-static artifact metadata: minos, stacksize 0, headers, no dylib, archive, self-containment (not part of the default build)");
        dbg_metadata_top.dependOn(&dbg_metadata.step);

        const dbg_check_top = b.step("check", b.fmt("Full legacy-equivalent verification for debug-static: regression {d}/{d}, bytecode, static native embedding, smoke, metadata (not part of the default build)", .{ gates.debug_passed, gates.debug_failed }));
        dbg_check_top.dependOn(&dbg_regression.step);
        dbg_check_top.dependOn(&dbg_verify_bc.step);
        dbg_check_top.dependOn(&dbg_native.step);
        dbg_check_top.dependOn(&dbg_smoke.step);
        dbg_check_top.dependOn(&dbg_metadata.step);
        dbg_check_top.dependOn(selftest_top);
        registerPackageSteps(b, python, false, "", sname, cp_step, null, null);
        return;
    }

    // ARM64 macOS JIT profiles with debug/test flaglist (jit and jit-debug).
    if (profile.isJit() and profile.hasDebugFlags()) {
        const cp_step = &copy_out.?.step;
        const sname = side_name.?;
        const jit_prefix_str = b.fmt("out/{s}", .{sname});
        const jit_ch_str = b.fmt("out/{s}/bin/ch", .{sname});
        const jit_archive_str = b.fmt("out/{s}/lib/libChakraCoreStatic.a", .{sname});
        const jit_cc = b.findProgram(.{ .names = &.{"clang"} }) orelse "clang";
        const jit_cxx = b.findProgram(.{ .names = &.{"clang++"} }) orelse "clang++";

        const jit_smoke = b.addSystemCommand(&.{python});
        jit_smoke.addFileArg(b.path("tools/build/check_smoke.py"));
        jit_smoke.addArg(jit_ch_str);
        jit_smoke.addFileInput(ch.bin);
        jit_smoke.addArg("--profile");
        jit_smoke.addArg(if (profile.isDebug()) "debug-static" else "test");
        jit_smoke.step.dependOn(cp_step);
        jit_smoke.stdio = .inherit;
        const jit_smoke_top = b.step("check-smoke", b.fmt("Assert {s} ch version/flag/snippet output (not part of the default build)", .{sname}));
        jit_smoke_top.dependOn(&jit_smoke.step);

        // JIT-specific runtime smoke: require the native backend symbol,
        // backend trace execution, and hot call/rest/apply under -mic/-forceNative.
        const jit_runtime = b.addSystemCommand(&.{python});
        jit_runtime.addFileArg(b.path("tools/build/check_jit.py"));
        jit_runtime.addArg(jit_ch_str);
        jit_runtime.addFileInput(ch.bin);
        jit_runtime.step.dependOn(cp_step);
        jit_runtime.stdio = .inherit;
        const jit_runtime_top = b.step("check-jit", "Require the ARM64 JIT backend symbol, trace compilation, and hot-call/rest/apply smoke (not part of the default build)");
        jit_runtime_top.dependOn(&jit_runtime.step);

        // The JIT profiles consume the checked-in JIT builtin bytecode headers.
        const jit_verify_bc = b.addSystemCommand(&.{python});
        jit_verify_bc.addFileArg(b.path("tools/build/verify_bytecode_pure.py"));
        jit_verify_bc.addFileArg(b.path("tools/regenByteCode.py"));
        jit_verify_bc.addArg("--jit");
        jit_verify_bc.addArg("--verify");
        jit_verify_bc.addArg(b.fmt("--binary={s}", .{jit_ch_str}));
        jit_verify_bc.addFileInput(ch.bin);
        jit_verify_bc.step.dependOn(cp_step);
        jit_verify_bc.stdio = .inherit;
        const jit_verify_bc_top = b.step("verify-bytecode", b.fmt("Verify checked-in JIT built-in bytecode with the just-built {s} ch (pure scratch-tree check; not part of the default build)", .{sname}));
        jit_verify_bc_top.dependOn(&jit_verify_bc.step);

        const jit_native = b.addSystemCommand(&.{python});
        jit_native.addFileArg(b.path("tools/build/check_native.py"));
        jit_native.addFileArg(b.path("test/native-tests/test_native.sh"));
        jit_native.addArg(jit_ch_str);
        jit_native.addFileInput(ch.bin);
        jit_native.addArg(jit_cc);
        jit_native.addArg(jit_cxx);
        if (is_static) {
            jit_native.addArg("--static");
            jit_native.addArg(b.fmt("--archive={s}", .{jit_archive_str}));
            jit_native.addFileInput(static_archive.bin);
        }
        jit_native.step.dependOn(cp_step);
        jit_native.stdio = .inherit;
        const jit_native_top = b.step("check-native", b.fmt("Run the {s} embedding tests against {s} artifacts (not part of the default build)", .{ if (is_static) "static" else "shared", sname }));
        jit_native_top.dependOn(&jit_native.step);

        const jit_metadata = b.addSystemCommand(&.{python});
        jit_metadata.addFileArg(b.path("tools/build/check_metadata.py"));
        jit_metadata.addArg(jit_prefix_str);
        jit_metadata.addFileInput(ch.bin);
        jit_metadata.addFileInput(config_h);
        jit_metadata.addArg("--profile");
        jit_metadata.addArg(sname);
        if (is_static) {
            jit_metadata.addArg("--static-members");
            jit_metadata.addArg(b.fmt("{d}", .{static_objs.items.len}));
            jit_metadata.addFileInput(static_archive.bin);
        }
        jit_metadata.step.dependOn(cp_step);
        jit_metadata.stdio = .inherit;
        const jit_metadata_top = b.step("check-metadata", b.fmt("Assert {s} artifact metadata: minos, {s}, headers, {s} (not part of the default build)", .{ sname, if (is_static) "stacksize 0" else "8MiB stacks", if (is_static) "no dylib, archive, self-containment" else "exports" }));
        jit_metadata_top.dependOn(&jit_metadata.step);

        registerFailStep(
            b,
            python,
            "check-regression",
            "Not applicable on JIT profiles: check_regression.py targets disable_jit pins; use check-fulljit",
            b.fmt("-Dprofile={s}: check_regression.py pins --variants=disable_jit only; use zig build -Dprofile={s} check-fulljit", .{ sname, sname }),
        );

        const jit_check_top = b.step("check", "Full ARM64 macOS JIT verification: smoke, JIT trace/runtime, JIT bytecode, native embedding, metadata, FullJIT, and Wasm/Wasm-JIT (when -Dwasm=true)");
        jit_check_top.dependOn(&jit_smoke.step);
        jit_check_top.dependOn(&jit_runtime.step);
        jit_check_top.dependOn(&jit_verify_bc.step);
        jit_check_top.dependOn(&jit_native.step);
        jit_check_top.dependOn(&jit_metadata.step);
        if (jit_fulljit_step) |fjs| jit_check_top.dependOn(fjs);
        if (jit_wasm_step) |ws| jit_check_top.dependOn(ws);
        if (jit_wasm_jit_step) |wjs| jit_check_top.dependOn(wjs);
        jit_check_top.dependOn(selftest_top);
        registerPackageSteps(b, python, !is_static, if (is_static) "" else jit_prefix_str, sname, cp_step, prov_step_opt, nodeapi_step_opt);
        return;
    }

    // (2) Bytecode check (contract section 5.10): the build consumes the
    // CHECKED-IN JsBuiltIn bytecode headers to bootstrap - this check
    // step is the ONLY regenByteCode.py reference in the graph, so no
    // build edge regenerates bytecode with the not-yet-built engine
    // (the script gets --binary=<installed ch>, which exists only after
    // the install step this check depends on). NOT a build dependency:
    // gate (2) of the B2.3 `check` aggregation below (check_top.dependOn),
    // also runnable standalone via `zig build verify-bytecode`.
    // Purity: regenByteCode.py --verify REWRITES the checked-in headers
    // even on success (fresh mtimes; on mismatch it leaves the new bytes
    // behind, self-healing red-to-green), so the step runs through
    // tools/build/verify_bytecode_pure.py, which runs the script against
    // a private copy of its inputs and returns its verdict. The checked-in
    // headers are never overwritten, even temporarily or on interruption.
    const default_install_step = if (copy_out) |cp| &cp.step else b.getInstallStep();
    const verify_bc = b.addSystemCommand(&.{python});
    verify_bc.addFileArg(b.path("tools/build/verify_bytecode_pure.py"));
    verify_bc.addFileArg(b.path("tools/regenByteCode.py"));
    verify_bc.addArg("--noJit");
    verify_bc.addArg("--verify");
    if (side_name) |sname| {
        verify_bc.addArg(b.fmt("--binary=out/{s}/bin/ch", .{sname}));
        verify_bc.addFileInput(ch.bin);
    } else {
        verify_bc.addPrefixedFileArg("--binary=", .{ .relative = .{ .base = .install_bin, .sub_path = "ch" } });
    }
    verify_bc.step.dependOn(default_install_step);
    verify_bc.stdio = .inherit;
    const verify_bc_top = b.step("verify-bytecode", "Verify checked-in built-in bytecode with the just-built ch (contract 5.10; pure scratch-tree check; not part of the default build)");
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
    const addDefaultCh = struct {
        fn call(builder: *std.Build, cmd: *std.Build.Step.Run, sname_opt: ?[]const u8, ch_art: std.Build.LazyPath, inst_ch: std.Build.LazyPath) void {
            if (sname_opt) |sname| {
                cmd.addArg(builder.fmt("out/{s}/bin/ch", .{sname}));
                cmd.addFileInput(ch_art);
            } else {
                cmd.addFileArg(inst_ch);
            }
        }
    }.call;

    // (1) Regression suite: runtests.py --test --variants=disable_jit
    // --binary=<installed ch> - the legacy-exact selection for this
    // profile (test/CMakeLists.txt:39-44; no --not-tag since ICU is
    // present). Requires the gate_counts.json test pin, parsed from
    // the real `Total:` summary line (check_regression.py defaults to
    // --select test when no explicit counts are passed).
    const regression = b.addSystemCommand(&.{python});
    regression.addFileArg(b.path("tools/build/check_regression.py"));
    regression.addFileArg(b.path("test/runtests.py"));
    addDefaultCh(b, regression, side_name, ch.bin, installed_ch);
    regression.step.dependOn(default_install_step);
    regression.stdio = .inherit;
    const regression_top = b.step("check-regression", b.fmt("Run the regression suite against the zig-built ch, require {d} passed / {d} failed (not part of the default build)", .{ gates.test_passed, gates.test_failed }));
    regression_top.dependOn(&regression.step);

    // (3) Shared embedding test: test_native.sh with the zig-built ch
    // (legacy method, test/CMakeLists.txt:48-55), run in a temp copy
    // of test/native-tests so the tree is never written to.
    const native = b.addSystemCommand(&.{python});
    native.addFileArg(b.path("tools/build/check_native.py"));
    native.addFileArg(b.path("test/native-tests/test_native.sh"));
    addDefaultCh(b, native, side_name, ch.bin, installed_ch);
    native.addArg(cc);
    native.addArg(cxx);
    native.step.dependOn(default_install_step);
    native.stdio = .inherit;
    const native_top = b.step("check-native", "Run the shared embedding test against zig artifacts (not part of the default build)");
    native_top.dependOn(&native.step);

    // (4) Smoke/flag output assertions (ch -v, invalid flag, JS snippet).
    const smoke = b.addSystemCommand(&.{python});
    smoke.addFileArg(b.path("tools/build/check_smoke.py"));
    addDefaultCh(b, smoke, side_name, ch.bin, installed_ch);
    smoke.step.dependOn(default_install_step);
    smoke.stdio = .inherit;
    const smoke_top = b.step("check-smoke", "Assert ch version/flag/snippet output text (not part of the default build)");
    smoke_top.dependOn(&smoke.step);

    // (5) Metadata gates: macOS minimum, executable stacks, staged
    // headers, dylib exports.
    const metadata = b.addSystemCommand(&.{python});
    metadata.addFileArg(b.path("tools/build/check_metadata.py"));
    if (side_name) |sname| {
        metadata.addArg(b.fmt("out/{s}", .{sname}));
        metadata.addFileInput(ch.bin);
        metadata.addFileInput(config_h);
        metadata.addArg("--profile");
        metadata.addArg(sname);
    } else {
        metadata.addDirectoryArg(.{ .relative = .{ .base = .install_prefix } });
    }
    metadata.step.dependOn(default_install_step);
    metadata.stdio = .inherit;
    const metadata_top = b.step("check-metadata", "Assert artifact metadata: minos, stacks, headers, exports (not part of the default build)");
    metadata_top.dependOn(&metadata.step);

    const check_top = b.step("check", b.fmt("Full legacy-equivalent verification: regression {d}/{d}, bytecode, native embedding, smoke, metadata (not part of the default build)", .{ gates.test_passed, gates.test_failed }));
    check_top.dependOn(&regression.step);
    check_top.dependOn(&verify_bc.step);
    check_top.dependOn(&native.step);
    check_top.dependOn(&smoke.step);
    check_top.dependOn(&metadata.step);
    check_top.dependOn(selftest_top);

    const test_prefix_str = if (side_name) |sname| b.fmt("out/{s}", .{sname}) else "zig-out";
    const test_pkg_profile = side_name orelse "test";
    registerPackageSteps(b, python, true, test_prefix_str, test_pkg_profile, default_install_step, prov_step_opt, nodeapi_step_opt);
}
