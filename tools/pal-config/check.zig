//-------------------------------------------------------------------------------------------------------
// Copyright (c) ChakraCore Project Contributors. All rights reserved.
// Licensed under the MIT license. See LICENSE.txt file in the project root for full license information.
//-------------------------------------------------------------------------------------------------------
//! Runs the 95 B1.3 PAL configuration checks through the repo-pinned Zig compiler and renders
//! pal/src/config.h. Any deviation from the contract fails the build with the setting name, the
//! contract value plus source line, and the observed value (doc/pal-config-contract-mac.md §5).
//!
//! Probe command lines mirror the reference try_compile/try_run invocations (observed in
//! build-zigpilot/CMakeFiles/CMakeConfigureLog.yaml): -O0, no -g, -std=gnu99 (C) / -std=gnu++17
//! (effective C++ standard), -D_FILE_OFFSET_BITS=64 (configure.cmake probes only; the
//! CMakeFeatureDetect tail-calls probe lacks it), per-probe -l flags. The inherited
//! -mmacosx-version-min=10.9/-arch arm64 are replaced by --target=aarch64-macos.26.0 plus
//! --sysroot (doc/build.md section 14). -isystem <sdk>/usr/include acts as a header fallback
//! after Zig's own Darwin headers: it fills the two gaps relevant here (sys/vmparam.h,
//! sys/user.h) while keeping system-header warning suppression, so the tail-calls -Werror
//! probe stays a real attribute check. Reference warning-suppression/-Werror/-Wno-error pairs
//! are net-off and omitted (verdict-neutral); the tail-calls probe keeps -Werror as run
//! (Build/CMakeFeatureDetect.cmake:12-14 runs it before the -Wno-error restore).

const std = @import("std");
const builtin = @import("builtin");
const contract = @import("contract.zig");
const probes = @import("probes.zig");

const embedded_check_src = @embedFile("check.zig");
const embedded_contract_src = @embedFile("contract.zig");
const embedded_probes_src = @embedFile("probes.zig");

const default_cache_dir = ".zig-cache/pal-config";
const cache_format_magic = "pal-config-cache-v1";
const max_sdk_meta_bytes: usize = 1024 * 1024;
const max_cache_file_bytes: usize = 1024 * 1024;
const max_payload_bytes: usize = 512 * 1024;

const compiler_env_vars = [_][:0]const u8{
    "CC",
    "CXX",
    "CFLAGS",
    "CXXFLAGS",
    "LDFLAGS",
    "SDKROOT",
    "MACOSX_DEPLOYMENT_TARGET",
    "C_INCLUDE_PATH",
    "CPLUS_INCLUDE_PATH",
    "LIBRARY_PATH",
    "CPATH",
    "ZIG_LIB_DIR",
};

pub const Options = struct {
    io: std.Io,
    zig_exe: []const u8,
    target: []const u8,
    sysroot: []const u8,
    /// B3.1 build profile name ("test", "debug-static", "release-shared").
    /// Provenance only: the probe set and fixed values are identical for
    /// every profile (pal/src/configure.cmake, pal/src/CMakeLists.txt and
    /// pal/CMakeLists.txt contain no CMAKE_BUILD_TYPE references), so the
    /// generated config.h bytes do not depend on it.
    profile: []const u8,
    /// Optional environment override (used by unit tests; null reads process env).
    environ_map: ?*const std.process.Environ.Map = null,
    /// Optional cache directory override (used by unit tests; null uses `.zig-cache/pal-config`).
    cache_dir: ?[]const u8 = null,
    /// Suppress stderr status/error messages in negative unit tests.
    quiet: bool = false,
};

pub const Output = struct {
    config_h: []u8,
    tail_calls: bool,
};

const GenerateError = error{
    ContractConflict,
    ContractDeviation,
    ScratchDirCreateFailed,
    ProbeWriteFailed,
    ProbeSpawnFailed,
    ProbeWaitFailed,
    ProbeExecFailed,
    ProbeOutputTooLong,
    ProbeReadFailed,
    OutOfMemory,
};

const Outcome = struct {
    passed: bool,
    log: []u8,
    observed_size: u8 = 0,
};

const ProbeRunner = *const fn (ctx: *Ctx, c: contract.Check, src: []const u8, tag: []const u8) GenerateError!Outcome;

const Ctx = struct {
    alloc: std.mem.Allocator,
    opt: Options,
    tmpdir: []const u8,
    outcomes: []Outcome,
    runner: ProbeRunner,
    user_data: ?*anyopaque = null,
};

fn logMsg(opt: Options, comptime fmt: []const u8, args: anytype) void {
    if (!opt.quiet) std.debug.print(fmt, args);
}

fn isCxx(c: contract.Check) bool {
    return switch (c.params) {
        .symbol, .run, .run_werror => true,
        else => false,
    };
}

fn lookupEnv(opt: Options, key: [:0]const u8) ?[]const u8 {
    if (opt.environ_map) |map| return map.get(key);
    if (opt.io.vtable == std.Io.Threaded.global_single_threaded.io().vtable) {
        const threaded: *const std.Io.Threaded = @ptrCast(@alignCast(opt.io.userdata.?));
        const env = threaded.environ.process_environ;
        if (!env.block.isEmpty() and builtin.os.tag != .windows) {
            return env.getPosix(key);
        }
    }
    if (builtin.link_libc and builtin.os.tag != .windows) {
        if (std.c.getenv(key)) |val| return std.mem.span(val);
    }
    return null;
}

fn trimTrailingSlashes(path: []const u8) []const u8 {
    var end = path.len;
    while (end > 1 and path[end - 1] == '/') : (end -= 1) {}
    return path[0..end];
}

/// Resolves the parent scratch directory from `$TMPDIR`, trimming trailing `/`
/// and falling back to `/tmp` when unset or empty.
fn resolveTmpBase(opt: Options) []const u8 {
    const raw = lookupEnv(opt, "TMPDIR") orelse return "/tmp";
    if (raw.len == 0) return "/tmp";
    const trimmed = trimTrailingSlashes(raw);
    if (trimmed.len == 0) return "/tmp";
    return trimmed;
}

fn joinPath(alloc: std.mem.Allocator, base: []const u8, sub: []const u8) ![]u8 {
    const clean = trimTrailingSlashes(base);
    if (std.mem.eql(u8, clean, "/")) {
        return std.fmt.allocPrint(alloc, "/{s}", .{sub});
    }
    return std.fmt.allocPrint(alloc, "{s}/{s}", .{ clean, sub });
}

fn hashTaggedBytes(h: *std.crypto.hash.sha2.Sha256, tag: []const u8, val: []const u8) void {
    var len_buf: [8]u8 = undefined;
    h.update(tag);
    h.update(&.{0});
    std.mem.writeInt(u64, &len_buf, @as(u64, val.len), .little);
    h.update(&len_buf);
    h.update(val);
}

fn hashOptBytes(h: *std.crypto.hash.sha2.Sha256, tag: []const u8, maybe_val: ?[]const u8) void {
    if (maybe_val) |val| {
        h.update(&.{1});
        hashTaggedBytes(h, tag, val);
    } else {
        h.update(&.{0});
        hashTaggedBytes(h, tag, "");
    }
}

fn hashPathStat(h: *std.crypto.hash.sha2.Sha256, io: std.Io, tag: []const u8, path: []const u8) void {
    h.update(tag);
    if (std.Io.Dir.cwd().statFile(io, path, .{ .follow_symlinks = true })) |st| {
        var buf: [24]u8 = undefined;
        std.mem.writeInt(u64, buf[0..8], st.size, .little);
        std.mem.writeInt(i128, buf[8..24], @as(i128, st.mtime.nanoseconds), .little);
        h.update(&.{1});
        h.update(&buf);
    } else |err| {
        h.update(&.{0});
        hashTaggedBytes(h, "err", @errorName(err));
    }
}

fn hashFileContents(
    alloc: std.mem.Allocator,
    h: *std.crypto.hash.sha2.Sha256,
    io: std.Io,
    tag: []const u8,
    path: []const u8,
) !void {
    h.update(tag);
    const bytes = std.Io.Dir.cwd().readFileAlloc(io, path, alloc, .limited(max_sdk_meta_bytes)) catch |err| switch (err) {
        error.OutOfMemory => return error.OutOfMemory,
        else => {
            h.update(&.{0});
            hashTaggedBytes(h, "err", @errorName(err));
            hashPathStat(h, io, "fallback-stat", path);
            return;
        },
    };
    defer alloc.free(bytes);
    h.update(&.{1});
    hashTaggedBytes(h, "bytes", bytes);
}

/// Computes the SHA-256 cache key over all inputs that affect probe results:
/// Zig toolchain identity, target, profile, sysroot + SDK metadata, embedded
/// probe/contract/check sources, and compiler environment overrides.
pub fn computeCacheKey(alloc: std.mem.Allocator, opt: Options) ![64]u8 {
    var h = std.crypto.hash.sha2.Sha256.init(.{});
    h.update("pal-config-key-v1\n");
    hashTaggedBytes(&h, "host_arch", @tagName(builtin.cpu.arch));
    hashTaggedBytes(&h, "host_os", @tagName(builtin.os.tag));
    hashTaggedBytes(&h, "zig_version", builtin.zig_version_string);
    hashTaggedBytes(&h, "zig_exe", opt.zig_exe);
    hashPathStat(&h, opt.io, "zig_exe_stat", opt.zig_exe);
    hashTaggedBytes(&h, "target", opt.target);
    hashTaggedBytes(&h, "profile", opt.profile);
    hashTaggedBytes(&h, "sysroot", opt.sysroot);

    if (opt.sysroot.len > 0) {
        hashPathStat(&h, opt.io, "sysroot_stat", opt.sysroot);

        const usr_inc = try joinPath(alloc, opt.sysroot, "usr/include");
        defer alloc.free(usr_inc);
        hashPathStat(&h, opt.io, "sysroot_usr_include", usr_inc);

        const sdk_plist = try joinPath(alloc, opt.sysroot, "SDKSettings.plist");
        defer alloc.free(sdk_plist);
        try hashFileContents(alloc, &h, opt.io, "sdk_plist", sdk_plist);

        const sdk_json = try joinPath(alloc, opt.sysroot, "SDKSettings.json");
        defer alloc.free(sdk_json);
        try hashFileContents(alloc, &h, opt.io, "sdk_json", sdk_json);
    }

    hashTaggedBytes(&h, "src:check.zig", embedded_check_src);
    hashTaggedBytes(&h, "src:contract.zig", embedded_contract_src);
    hashTaggedBytes(&h, "src:probes.zig", embedded_probes_src);

    for (compiler_env_vars) |env_name| {
        hashOptBytes(&h, env_name, lookupEnv(opt, env_name));
    }

    var digest: [std.crypto.hash.sha2.Sha256.digest_length]u8 = undefined;
    h.final(&digest);
    return std.fmt.bytesToHex(digest, .lower);
}

fn computeRecordDigest(key_hex: *const [64]u8, tail_calls: bool, config_h: []const u8) [64]u8 {
    var h = std.crypto.hash.sha2.Sha256.init(.{});
    h.update("pal-config-record-v1\n");
    h.update(key_hex);
    h.update(if (tail_calls) "\n1\n" else "\n0\n");
    h.update(config_h);
    var digest: [std.crypto.hash.sha2.Sha256.digest_length]u8 = undefined;
    h.final(&digest);
    return std.fmt.bytesToHex(digest, .lower);
}

fn encodeCacheRecord(alloc: std.mem.Allocator, key_hex: *const [64]u8, out: Output) ![]u8 {
    const digest_hex = computeRecordDigest(key_hex, out.tail_calls, out.config_h);
    var buf: std.ArrayList(u8) = .empty;
    errdefer buf.deinit(alloc);
    try buf.print(
        alloc,
        "{s}\nkey={s}\ntail_calls={d}\npayload_len={d}\nrecord_sha256={s}\n---\n",
        .{
            cache_format_magic,
            key_hex,
            @as(u8, @intFromBool(out.tail_calls)),
            out.config_h.len,
            digest_hex,
        },
    );
    try buf.appendSlice(alloc, out.config_h);
    return buf.toOwnedSlice(alloc);
}

fn takeLine(rest: *[]const u8) ?[]const u8 {
    const idx = std.mem.findScalar(u8, rest.*, '\n') orelse return null;
    const line = rest.*[0..idx];
    rest.* = rest.*[idx + 1 ..];
    return line;
}

fn parseCacheRecord(
    out_alloc: std.mem.Allocator,
    bytes: []const u8,
    expected_key_hex: *const [64]u8,
) error{ CorruptCache, OutOfMemory }!Output {
    var rest = bytes;

    const magic = takeLine(&rest) orelse return error.CorruptCache;
    if (!std.mem.eql(u8, magic, cache_format_magic)) return error.CorruptCache;

    const key_line = takeLine(&rest) orelse return error.CorruptCache;
    const key_val = std.mem.cutPrefix(u8, key_line, "key=") orelse return error.CorruptCache;
    if (!std.mem.eql(u8, key_val, expected_key_hex)) return error.CorruptCache;

    const tc_line = takeLine(&rest) orelse return error.CorruptCache;
    const tc_val = std.mem.cutPrefix(u8, tc_line, "tail_calls=") orelse return error.CorruptCache;
    const tail_calls = if (std.mem.eql(u8, tc_val, "1"))
        true
    else if (std.mem.eql(u8, tc_val, "0"))
        false
    else
        return error.CorruptCache;

    const len_line = takeLine(&rest) orelse return error.CorruptCache;
    const len_val = std.mem.cutPrefix(u8, len_line, "payload_len=") orelse return error.CorruptCache;
    const payload_len = std.fmt.parseUnsigned(usize, len_val, 10) catch return error.CorruptCache;
    if (payload_len > max_payload_bytes) return error.CorruptCache;

    const digest_line = takeLine(&rest) orelse return error.CorruptCache;
    const digest_val = std.mem.cutPrefix(u8, digest_line, "record_sha256=") orelse return error.CorruptCache;
    if (digest_val.len != 64) return error.CorruptCache;

    const sep_line = takeLine(&rest) orelse return error.CorruptCache;
    if (!std.mem.eql(u8, sep_line, "---")) return error.CorruptCache;

    // Exact length check prevents both truncation and trailing-byte injection.
    if (rest.len != payload_len) return error.CorruptCache;

    const expected_digest = computeRecordDigest(expected_key_hex, tail_calls, rest);
    if (!std.mem.eql(u8, digest_val, &expected_digest)) return error.CorruptCache;

    const duped = try out_alloc.dupe(u8, rest);
    return .{
        .config_h = duped,
        .tail_calls = tail_calls,
    };
}

fn readCacheEntry(
    out_alloc: std.mem.Allocator,
    scratch_alloc: std.mem.Allocator,
    io: std.Io,
    cache_dir: []const u8,
    key_hex: *const [64]u8,
) error{OutOfMemory}!?Output {
    var dir = std.Io.Dir.cwd().openDir(io, cache_dir, .{}) catch return null;
    defer dir.close(io);

    var file_name_buf: [68]u8 = undefined;
    const filename = std.fmt.bufPrint(&file_name_buf, "{s}.txt", .{key_hex}) catch unreachable;

    const bytes = dir.readFileAlloc(io, filename, scratch_alloc, .limited(max_cache_file_bytes)) catch |err| switch (err) {
        error.OutOfMemory => return error.OutOfMemory,
        else => return null,
    };
    defer scratch_alloc.free(bytes);

    return parseCacheRecord(out_alloc, bytes, key_hex) catch |err| switch (err) {
        error.CorruptCache => {
            // Drop corrupt cache entries best-effort so future reads start clean.
            dir.deleteFile(io, filename) catch {};
            return null;
        },
        error.OutOfMemory => return error.OutOfMemory,
    };
}

fn writeCacheEntryAtomic(
    scratch_alloc: std.mem.Allocator,
    io: std.Io,
    cache_dir: []const u8,
    key_hex: *const [64]u8,
    out: Output,
) !void {
    const encoded = try encodeCacheRecord(scratch_alloc, key_hex, out);
    defer scratch_alloc.free(encoded);

    var dir = try std.Io.Dir.cwd().createDirPathOpen(io, cache_dir, .{});
    defer dir.close(io);

    var file_name_buf: [68]u8 = undefined;
    const filename = std.fmt.bufPrint(&file_name_buf, "{s}.txt", .{key_hex}) catch unreachable;

    var atomic = try dir.createFileAtomic(io, filename, .{ .replace = true });
    defer atomic.deinit(io);

    try atomic.file.writeStreamingAll(io, encoded);
    try atomic.replace(io);
}

fn renderSource(alloc: std.mem.Allocator, c: contract.Check) error{OutOfMemory}![]u8 {
    var buf: std.ArrayList(u8) = .empty;
    errdefer buf.deinit(alloc);
    switch (c.params) {
        .include => |h| try buf.print(alloc, "#include <{s}>\n\n\nint main(void){{return 0;}}\n", .{h}),
        .func, .libfunc => try buf.appendSlice(alloc, probes.func_tmpl),
        .@"struct" => |p| {
            for (p.headers) |h| try buf.print(alloc, "#include <{s}>\n", .{h});
            try buf.print(alloc, "\nint main(void)\n{{\n  (void)sizeof((({s} *)0)->{s});\n  return 0;\n}}\n", .{ p.st, p.member });
        },
        .typesize => |p| {
            try buf.appendSlice(alloc, "#include <sys/types.h>\n#include <stdint.h>\n#include <stddef.h>\n");
            for (p.extras) |h| try buf.print(alloc, "#include \"{s}\"\n", .{h});
            try buf.print(alloc, "\nint main(void){{ (void)sizeof({s}); return 0; }}\n", .{p.typ});
        },
        .sizeof_eq => |p| {
            try buf.print(alloc, "#include <sys/types.h>\n#include <stdint.h>\n#include <stddef.h>\n_Static_assert(sizeof({s}) == {d}, \"{s} size\");\nint main(void){{return 0;}}\n", .{ p.typ, p.size, p.typ });
        },
        .symbol => |p| {
            try buf.print(alloc, "#include <{s}>\n\nint main(int argc, char** argv)\n{{\n  (void)argv;\n#ifndef {s}\n  return ((int*)(&{s}))[argc];\n#else\n  (void)argc;\n  return 0;\n#endif\n}}\n", .{ p.header, p.sym, p.sym });
        },
        .run => |p| try buf.appendSlice(alloc, p.body),
        .run_werror => |body| try buf.appendSlice(alloc, body),
    }
    return buf.toOwnedSlice(alloc);
}

fn readPipeToEnd(alloc: std.mem.Allocator, io: std.Io, file: std.Io.File) GenerateError![]u8 {
    var r = file.readerStreaming(io, &.{});
    return r.interface.allocRemaining(alloc, .limited(64 * 1024)) catch |err| switch (err) {
        error.StreamTooLong => return error.ProbeOutputTooLong,
        error.OutOfMemory => return error.OutOfMemory,
        else => return error.ProbeReadFailed,
    };
}

fn compileAndMaybeRun(ctx: *Ctx, c: contract.Check, src: []const u8, tag: []const u8) GenerateError!Outcome {
    const alloc = ctx.alloc;
    const io = ctx.opt.io;
    const ext = if (isCxx(c)) ".cpp" else ".c";
    const src_path = try std.fmt.allocPrint(alloc, "{s}/{s}{s}", .{ ctx.tmpdir, tag, ext });
    defer alloc.free(src_path);
    const exe_path = try std.fmt.allocPrint(alloc, "{s}/{s}.exe", .{ ctx.tmpdir, tag });
    defer alloc.free(exe_path);
    {
        const f = std.Io.Dir.createFileAbsolute(io, src_path, .{}) catch |err| {
            logMsg(ctx.opt, "pal-config error: cannot write probe source {s}: {t}\n", .{ src_path, err });
            return error.ProbeWriteFailed;
        };
        defer f.close(io);
        f.writeStreamingAll(io, src) catch |err| {
            logMsg(ctx.opt, "pal-config error: cannot write probe source {s}: {t}\n", .{ src_path, err });
            return error.ProbeWriteFailed;
        };
    }

    var argv: std.ArrayList([]const u8) = .empty;
    defer argv.deinit(alloc);
    try argv.appendSlice(alloc, &.{ ctx.opt.zig_exe, if (isCxx(c)) "c++" else "cc" });
    const target_flag = try std.fmt.allocPrint(alloc, "--target={s}", .{ctx.opt.target});
    defer alloc.free(target_flag);
    const sysroot_flag = try std.fmt.allocPrint(alloc, "--sysroot={s}", .{ctx.opt.sysroot});
    defer alloc.free(sysroot_flag);
    const sdk_isystem = try std.fmt.allocPrint(alloc, "{s}/usr/include", .{ctx.opt.sysroot});
    defer alloc.free(sdk_isystem);
    try argv.appendSlice(alloc, &.{ target_flag, sysroot_flag });
    // Every configure.cmake probe inherits -D_FILE_OFFSET_BITS=64 via
    // CMAKE_REQUIRED_DEFINITIONS (configure.cmake:23), but the
    // CMakeFeatureDetect tail-calls probe lives in a separate directory scope
    // and its reference command line lacks it, so omit it there.
    if (c.params != .run_werror) try argv.append(alloc, "-D_FILE_OFFSET_BITS=64");
    try argv.appendSlice(alloc, &.{
        if (isCxx(c)) "-std=gnu++17" else "-std=gnu99",
        "-O0",
        "-isystem",
        sdk_isystem,
    });
    var extra_flag: ?[]u8 = null;
    defer if (extra_flag) |f| alloc.free(f);
    switch (c.params) {
        .func => |f| {
            extra_flag = try std.fmt.allocPrint(alloc, "-DCHECK_FUNCTION_EXISTS={s}", .{f});
            try argv.append(alloc, extra_flag.?);
        },
        .libfunc => |p| {
            extra_flag = try std.fmt.allocPrint(alloc, "-DCHECK_FUNCTION_EXISTS={s}", .{p.func});
            try argv.append(alloc, extra_flag.?);
        },
        .run_werror => try argv.append(alloc, "-Werror"),
        else => {},
    }
    try argv.appendSlice(alloc, &.{ src_path, "-o", exe_path });
    var lib_flags: std.ArrayList([]u8) = .empty;
    defer {
        for (lib_flags.items) |f| alloc.free(f);
        lib_flags.deinit(alloc);
    }
    const libs: []const []const u8 = switch (c.params) {
        .libfunc => |p| &.{p.lib},
        .run => |p| p.libs,
        else => &.{},
    };
    for (libs) |lib| {
        const f = try std.fmt.allocPrint(alloc, "-l{s}", .{lib});
        try lib_flags.append(alloc, f);
        try argv.append(alloc, f);
    }

    // Probe stdout is always empty (diagnostics go to stderr), so reading
    // stderr first cannot deadlock: stderr stays far below the pipe buffer.
    var child = std.process.spawn(io, .{
        .argv = argv.items,
        .stdin = .ignore,
        .stdout = .pipe,
        .stderr = .pipe,
    }) catch |err| {
        logMsg(ctx.opt, "pal-config error: cannot run {s} {s}: {t}\n", .{ ctx.opt.zig_exe, argv.items[1], err });
        return error.ProbeSpawnFailed;
    };
    const stderr = readPipeToEnd(alloc, io, child.stderr.?) catch |err| {
        _ = child.wait(io) catch {};
        logMsg(ctx.opt, "pal-config error: probe {s} stderr read failed: {t}\n", .{ c.name, err });
        return err;
    };
    errdefer alloc.free(stderr);
    const stdout = readPipeToEnd(alloc, io, child.stdout.?) catch |err| {
        _ = child.wait(io) catch {};
        logMsg(ctx.opt, "pal-config error: probe {s} stdout read failed: {t}\n", .{ c.name, err });
        return err;
    };
    alloc.free(stdout);
    // Note: child.wait owns pipe cleanup; closing here would double-close.
    const term = child.wait(io) catch |err| {
        logMsg(ctx.opt, "pal-config error: probe {s} wait failed: {t}\n", .{ c.name, err });
        return error.ProbeWaitFailed;
    };
    if (!term.success()) return .{ .passed = false, .log = stderr };

    const must_run = switch (c.params) {
        .run, .run_werror => true,
        else => false,
    };
    if (!must_run) {
        alloc.free(stderr);
        return .{ .passed = true, .log = &.{} };
    }
    var run = std.process.spawn(io, .{
        .argv = &.{exe_path},
        // Relative filesystem probes must not inspect or write the checkout.
        // Each configuration owns this fresh directory (including concurrent
        // profile builds), so "a_nonexistent_file" is really nonexistent.
        .cwd = .{ .path = ctx.tmpdir },
        .stdin = .ignore,
        .stdout = .ignore,
        .stderr = .ignore,
    }) catch |err| {
        alloc.free(stderr);
        logMsg(ctx.opt, "pal-config error: cannot execute probe {s}: {t} (native execution is required)\n", .{ exe_path, err });
        return error.ProbeExecFailed;
    };
    const rterm = run.wait(io) catch |err| {
        alloc.free(stderr);
        logMsg(ctx.opt, "pal-config error: probe {s} wait failed: {t}\n", .{ c.name, err });
        return error.ProbeWaitFailed;
    };
    if (!rterm.success()) {
        alloc.free(stderr);
        const what: []const u8 = switch (rterm) {
            .exited => |code| try std.fmt.allocPrint(alloc, "exit code {d}", .{code}),
            .signal => "terminating signal",
            .stopped => "stop signal",
            .unknown => "unknown termination",
        };
        defer if (rterm == .exited) alloc.free(what);
        const msg = try std.fmt.allocPrint(alloc, "probe did not exit 0 ({s})", .{what});
        return .{ .passed = false, .log = msg };
    }
    alloc.free(stderr);
    return .{ .passed = true, .log = &.{} };
}

fn diagnoseSize(ctx: *Ctx, typ: []const u8) u8 {
    // The ==8 assertion failed: find the actual size for a precise report.
    for ([_]u8{ 1, 2, 4, 16 }) |n| {
        var buf: std.ArrayList(u8) = .empty;
        defer buf.deinit(ctx.alloc);
        buf.print(ctx.alloc, "#include <sys/types.h>\n#include <stdint.h>\n#include <stddef.h>\n_Static_assert(sizeof({s}) == {d}, \"size\");\nint main(void){{return 0;}}\n", .{ typ, n }) catch return 0;
        const src = buf.toOwnedSlice(ctx.alloc) catch return 0;
        defer ctx.alloc.free(src);
        const tag = std.fmt.allocPrint(ctx.alloc, "sizeof_{s}_{d}", .{ typ, n }) catch return 0;
        defer ctx.alloc.free(tag);
        const probe = contract.Check{
            .name = tag,
            .cite = "diagnostic",
            .expected = true,
            .params = .{ .typesize = .{ .typ = typ, .extras = &.{} } },
        };
        const o = ctx.runner(ctx, probe, src, tag) catch return 0;
        freeLog(ctx.alloc, o.log);
        if (o.passed) return n;
    }
    return 0;
}

fn runOne(ctx: *Ctx, i: usize, checks: []const contract.Check) GenerateError!void {
    const c = checks[i];
    const src = try renderSource(ctx.alloc, c);
    defer ctx.alloc.free(src);
    var o = try ctx.runner(ctx, c, src, c.name);
    if (c.params == .sizeof_eq and !o.passed) {
        o.observed_size = diagnoseSize(ctx, c.params.sizeof_eq.typ);
    }
    ctx.outcomes[i] = o;
}

fn freeLog(alloc: std.mem.Allocator, log: []u8) void {
    // Success outcomes carry a static empty log; only free real allocations.
    if (log.len > 0) alloc.free(log);
}

fn printLogHead(opt: Options, log: []const u8) void {
    var lines: usize = 0;
    var it = std.mem.splitScalar(u8, log, '\n');
    while (it.next()) |line| {
        if (lines >= 8) {
            logMsg(opt, "      ...\n", .{});
            break;
        }
        if (line.len > 0) {
            const show = if (line.len > 200) line[0..200] else line;
            logMsg(opt, "      | {s}\n", .{show});
            lines += 1;
        }
    }
}

fn fixedValue(name: []const u8) ?bool {
    for (contract.fixed01) |f| {
        if (std.mem.eql(u8, f.name, name)) return f.value;
    }
    return null;
}

fn emitHeader(
    alloc: std.mem.Allocator,
    opt: Options,
    checks: []const contract.Check,
    outcomes: []const Outcome,
) GenerateError![]u8 {
    var hdr: std.ArrayList(u8) = .empty;
    errdefer hdr.deinit(alloc);
    for (contract.layout) |item| {
        switch (item) {
            .lit => |s| try hdr.print(alloc, "{s}\n", .{s}),
            .b01 => |name| {
                const v: bool = if (fixedValue(name)) |f| f else blk: {
                    var found: ?bool = null;
                    for (checks, outcomes) |c, o| {
                        if (std.mem.eql(u8, c.name, name)) {
                            found = o.passed;
                            break;
                        }
                    }
                    break :blk found orelse {
                        logMsg(opt, "pal-config error: contract error: no value for {s}\n", .{name});
                        return error.ContractConflict;
                    };
                };
                try hdr.print(alloc, "#define {s} {d}\n", .{ name, @as(u8, @intFromBool(v)) });
            },
            .off_t => {
                var size: u8 = 0;
                for (checks) |c| {
                    if (c.params == .sizeof_eq) size = c.params.sizeof_eq.size;
                }
                try hdr.print(alloc, "#define SIZEOF_OFF_T {d}\n", .{size});
            },
            .val => |p| try hdr.print(alloc, "#define {s} {s}\n", .{ p.name, p.value }),
            .undef => |name| try hdr.print(alloc, "/* #undef {s} */\n", .{name}),
            .ptrace => try hdr.print(alloc, "{s}\n", .{contract.ptrace_line}),
        }
    }
    return hdr.toOwnedSlice(alloc);
}

fn generateWithRunner(
    out_alloc: std.mem.Allocator,
    scratch_alloc: std.mem.Allocator,
    opt: Options,
    checks: []const contract.Check,
    runner: ProbeRunner,
    user_data: ?*anyopaque,
) GenerateError!Output {
    for (checks) |c| {
        if (fixedValue(c.name) != null) {
            logMsg(opt, "pal-config error: contract error: {s} is both fixed and checked\n", .{c.name});
            return error.ContractConflict;
        }
    }

    const cache_dir = opt.cache_dir orelse default_cache_dir;
    const key_hex = try computeCacheKey(scratch_alloc, opt);
    if (try readCacheEntry(out_alloc, scratch_alloc, opt.io, cache_dir, &key_hex)) |cached| {
        logMsg(
            opt,
            "pal-config: {d}/{d} checks match the contract ({d} fixed-by-target, {d} compile-checked, {d} runtime-verified) [profile {s}, cached]\n",
            .{ checks.len, checks.len, contract.num_fixed_by_target, contract.num_compile_checked, contract.num_runtime_verified, opt.profile },
        );
        return cached;
    }

    const tmp_base = resolveTmpBase(opt);
    var parent_dir = std.Io.Dir.cwd().openDir(opt.io, tmp_base, .{}) catch |err| {
        logMsg(opt, "pal-config error: cannot open probe parent dir {s}: {t}\n", .{ tmp_base, err });
        return error.ScratchDirCreateFailed;
    };
    defer parent_dir.close(opt.io);

    var parent_abs_buf: [std.Io.Dir.max_path_bytes]u8 = undefined;
    const parent_abs: []const u8 = if (std.Io.Dir.path.isAbsolute(tmp_base))
        tmp_base
    else blk: {
        const n = parent_dir.realPath(opt.io, &parent_abs_buf) catch |err| {
            logMsg(opt, "pal-config error: cannot resolve probe parent dir {s}: {t}\n", .{ tmp_base, err });
            return error.ScratchDirCreateFailed;
        };
        break :blk trimTrailingSlashes(parent_abs_buf[0..n]);
    };

    var leaf_buf: [64]u8 = undefined;
    var tmpdir_buf: [std.Io.Dir.max_path_bytes]u8 = undefined;
    var created_leaf: ?[]const u8 = null;
    var tmpdir: []const u8 = undefined;
    for (0..8) |_| {
        var seed: u64 = undefined;
        std.Io.random(opt.io, std.mem.asBytes(&seed));
        const leaf = std.fmt.bufPrint(&leaf_buf, "chakra-pal-config-{x}", .{seed}) catch unreachable;
        const sep = if (std.mem.eql(u8, parent_abs, "/")) "" else "/";
        tmpdir = std.fmt.bufPrint(&tmpdir_buf, "{s}{s}{s}", .{ parent_abs, sep, leaf }) catch {
            logMsg(opt, "pal-config error: probe temp dir path too long under {s}\n", .{parent_abs});
            return error.ScratchDirCreateFailed;
        };
        parent_dir.createDir(opt.io, leaf, .default_dir) catch |err| switch (err) {
            error.PathAlreadyExists => continue,
            else => {
                logMsg(opt, "pal-config error: cannot create probe dir {s}: {t}\n", .{ tmpdir, err });
                return error.ScratchDirCreateFailed;
            },
        };
        created_leaf = leaf;
        break;
    } else {
        logMsg(opt, "pal-config error: cannot create unique probe dir under {s}\n", .{parent_abs});
        return error.ScratchDirCreateFailed;
    }
    defer if (created_leaf) |leaf| {
        parent_dir.deleteTree(opt.io, leaf) catch {};
    };

    const outcomes = try scratch_alloc.alloc(Outcome, checks.len);
    for (outcomes) |*o| o.* = .{ .passed = false, .log = &.{} };
    defer {
        for (outcomes) |o| freeLog(scratch_alloc, o.log);
        scratch_alloc.free(outcomes);
    }

    var ctx = Ctx{
        .alloc = scratch_alloc,
        .opt = opt,
        .tmpdir = tmpdir,
        .outcomes = outcomes,
        .runner = runner,
        .user_data = user_data,
    };

    for (0..checks.len) |i| try runOne(&ctx, i, checks);

    var failed: usize = 0;
    for (checks, outcomes) |c, o| {
        if (o.passed != c.expected) failed += 1;
    }
    if (failed > 0) {
        logMsg(opt, "pal-config: {d}/{d} checks deviate from doc/pal-config-contract-mac.md section 2\n", .{ failed, checks.len });
        for (checks, outcomes) |c, o| {
            if (o.passed == c.expected) continue;
            if (c.params == .sizeof_eq) {
                if (o.observed_size == 0) {
                    logMsg(opt, "  {s}: contract {s}={d} but size unidentified (tried 1,2,4,8,16)\n", .{ c.name, c.cite, c.params.sizeof_eq.size });
                } else {
                    logMsg(opt, "  {s}: contract {s}={d} but observed {d}\n", .{ c.name, c.cite, c.params.sizeof_eq.size, o.observed_size });
                }
            } else {
                logMsg(opt, "  {s}: contract {s}={d} but observed {d}\n", .{ c.name, c.cite, @as(u8, @intFromBool(c.expected)), @as(u8, @intFromBool(o.passed)) });
            }
            printLogHead(opt, o.log);
        }
        logMsg(opt, "pal-config: resolve by contract update with fresh evidence, never by editing generated output\n", .{});
        return error.ContractDeviation;
    }

    var tail_calls = false;
    for (checks, outcomes) |c, o| {
        if (std.mem.eql(u8, c.name, "CLANG_HAS_DISABLE_TAIL_CALLS")) tail_calls = o.passed;
    }

    const config_h = try emitHeader(out_alloc, opt, checks, outcomes);
    errdefer out_alloc.free(config_h);
    const result = Output{
        .config_h = config_h,
        .tail_calls = tail_calls,
    };

    writeCacheEntryAtomic(scratch_alloc, opt.io, cache_dir, &key_hex, result) catch |err| {
        logMsg(opt, "pal-config warning: cannot write cache under {s}: {t}\n", .{ cache_dir, err });
    };

    logMsg(
        opt,
        "pal-config: {d}/{d} checks match the contract ({d} fixed-by-target, {d} compile-checked, {d} runtime-verified) [profile {s}]\n",
        .{ checks.len, checks.len, contract.num_fixed_by_target, contract.num_compile_checked, contract.num_runtime_verified, opt.profile },
    );
    return result;
}

pub fn generate(opt: Options) Output {
    // Build scripts run single-threaded, so checks run serially (~0.2s each).
    // Page-backed allocation avoids tying scratch memory to the build arena.
    // The returned config_h is page-backed; the caller copies it and frees it.
    const alloc = std.heap.page_allocator;
    return generateWithRunner(alloc, alloc, opt, contract.checks, compileAndMaybeRun, null) catch {
        std.process.exit(1);
    };
}

const TestRunnerState = struct {
    calls: usize = 0,
    fail_name: ?[]const u8 = null,
    error_on_name: ?[]const u8 = null,
    observed_tmpdir_prefix: ?[]const u8 = null,
    saw_tmpdir_exists: bool = false,
};

fn mockContractRunner(ctx: *Ctx, c: contract.Check, _: []const u8, _: []const u8) GenerateError!Outcome {
    const state: *TestRunnerState = @ptrCast(@alignCast(ctx.user_data.?));
    state.calls += 1;
    if (state.observed_tmpdir_prefix) |prefix| {
        const expected_prefix = if (std.mem.eql(u8, prefix, "/")) "/chakra-pal-config-" else prefix;
        if (std.mem.startsWith(u8, ctx.tmpdir, expected_prefix) and
            std.mem.indexOf(u8, ctx.tmpdir, "/chakra-pal-config-") != null)
        {
            if (std.Io.Dir.openDirAbsolute(ctx.opt.io, ctx.tmpdir, .{})) |d| {
                var dir = d;
                dir.close(ctx.opt.io);
                state.saw_tmpdir_exists = true;
            } else |_| {}
        }
    }
    if (state.error_on_name) |err_name| {
        if (std.mem.eql(u8, c.name, err_name)) return error.ProbeSpawnFailed;
    }
    const passed = if (state.fail_name) |fnm|
        (if (std.mem.eql(u8, c.name, fnm)) !c.expected else c.expected)
    else
        c.expected;
    return .{ .passed = passed, .log = &.{} };
}

fn panicIfCalledRunner(_: *Ctx, _: contract.Check, _: []const u8, _: []const u8) GenerateError!Outcome {
    @panic("probe runner must not execute on cache hit");
}

fn countScratchDirs(io: std.Io, dir: std.Io.Dir) !usize {
    var count: usize = 0;
    var it = dir.iterate();
    while (try it.next(io)) |entry| {
        if (std.mem.startsWith(u8, entry.name, "chakra-pal-config-")) count += 1;
    }
    return count;
}

test "TMPDIR selection trims trailing slashes and falls back to /tmp" {
    const alloc = std.testing.allocator;
    var env_map: std.process.Environ.Map = .init(alloc);
    defer env_map.deinit();

    const base_opt = Options{
        .io = std.testing.io,
        .zig_exe = "zig",
        .target = "aarch64-macos.26.0",
        .sysroot = "/nonexistent-sdk",
        .profile = "test",
        .environ_map = &env_map,
        .quiet = true,
    };

    try std.testing.expectEqualStrings("/tmp", resolveTmpBase(base_opt));

    try env_map.put("TMPDIR", "");
    try std.testing.expectEqualStrings("/tmp", resolveTmpBase(base_opt));

    try env_map.put("TMPDIR", "/var/folders/xx/T/");
    try std.testing.expectEqualStrings("/var/folders/xx/T", resolveTmpBase(base_opt));

    try env_map.put("TMPDIR", "/var/folders/xx/T///");
    try std.testing.expectEqualStrings("/var/folders/xx/T", resolveTmpBase(base_opt));

    try env_map.put("TMPDIR", "/");
    try std.testing.expectEqualStrings("/", resolveTmpBase(base_opt));
}

test "warm cache hit skips probes and miss populates atomically under TMPDIR" {
    const alloc = std.testing.allocator;
    const io = std.testing.io;

    var tmp_root = std.testing.tmpDir(.{ .iterate = true });
    defer tmp_root.cleanup();

    try tmp_root.dir.createDir(io, "cache", .default_dir);
    try tmp_root.dir.createDir(io, "scratch", .default_dir);
    try tmp_root.dir.createDir(io, "sdk", .default_dir);
    try tmp_root.dir.writeFile(io, .{ .sub_path = "sdk/SDKSettings.plist", .data = "<plist>26.5</plist>\n" });
    try tmp_root.dir.writeFile(io, .{ .sub_path = "sdk/SDKSettings.json", .data = "{\"Version\":\"26.5\"}\n" });

    const root_abs = try tmp_root.dir.realPathFileAlloc(io, ".", alloc);
    defer alloc.free(root_abs);
    const cache_dir = try std.fmt.allocPrint(alloc, "{s}/cache", .{root_abs});
    defer alloc.free(cache_dir);
    const scratch_dir = try std.fmt.allocPrint(alloc, "{s}/scratch", .{root_abs});
    defer alloc.free(scratch_dir);
    const scratch_with_slashes = try std.fmt.allocPrint(alloc, "{s}///", .{scratch_dir});
    defer alloc.free(scratch_with_slashes);
    const sdk_dir = try std.fmt.allocPrint(alloc, "{s}/sdk", .{root_abs});
    defer alloc.free(sdk_dir);

    var env_map: std.process.Environ.Map = .init(alloc);
    defer env_map.deinit();
    try env_map.put("TMPDIR", scratch_with_slashes);

    const opt = Options{
        .io = io,
        .zig_exe = "zig",
        .target = "aarch64-macos.26.0",
        .sysroot = sdk_dir,
        .profile = "test",
        .environ_map = &env_map,
        .cache_dir = cache_dir,
        .quiet = true,
    };

    var state = TestRunnerState{ .observed_tmpdir_prefix = scratch_dir };
    const cold = try generateWithRunner(alloc, alloc, opt, contract.checks, mockContractRunner, &state);
    defer alloc.free(cold.config_h);

    try std.testing.expectEqual(contract.checks.len, state.calls);
    try std.testing.expect(state.saw_tmpdir_exists);
    try std.testing.expect(cold.tail_calls);

    // Scratch directory must be cleaned up immediately after cold probe run.
    var scratch_open = try tmp_root.dir.openDir(io, "scratch", .{ .iterate = true });
    defer scratch_open.close(io);
    try std.testing.expectEqual(@as(usize, 0), try countScratchDirs(io, scratch_open));

    // Cache hit must not invoke the probe runner at all.
    const warm = try generateWithRunner(alloc, alloc, opt, contract.checks, panicIfCalledRunner, null);
    defer alloc.free(warm.config_h);

    try std.testing.expectEqual(cold.tail_calls, warm.tail_calls);
    try std.testing.expectEqualSlices(u8, cold.config_h, warm.config_h);
}

test "cache key invalidates on target, profile, SDK metadata, zig_exe, and CC/CXX changes" {
    const alloc = std.testing.allocator;
    const io = std.testing.io;

    var tmp_root = std.testing.tmpDir(.{ .iterate = true });
    defer tmp_root.cleanup();

    try tmp_root.dir.createDir(io, "cache", .default_dir);
    try tmp_root.dir.createDir(io, "scratch", .default_dir);
    try tmp_root.dir.createDir(io, "sdk", .default_dir);
    try tmp_root.dir.writeFile(io, .{ .sub_path = "sdk/SDKSettings.plist", .data = "<plist>26.5</plist>\n" });
    try tmp_root.dir.writeFile(io, .{ .sub_path = "sdk/SDKSettings.json", .data = "{\"Version\":\"26.5\"}\n" });
    try tmp_root.dir.writeFile(io, .{ .sub_path = "zig-bin", .data = "zig-v1" });

    const root_abs = try tmp_root.dir.realPathFileAlloc(io, ".", alloc);
    defer alloc.free(root_abs);
    const cache_dir = try std.fmt.allocPrint(alloc, "{s}/cache", .{root_abs});
    defer alloc.free(cache_dir);
    const scratch_dir = try std.fmt.allocPrint(alloc, "{s}/scratch", .{root_abs});
    defer alloc.free(scratch_dir);
    const sdk_dir = try std.fmt.allocPrint(alloc, "{s}/sdk", .{root_abs});
    defer alloc.free(sdk_dir);
    const zig_path = try std.fmt.allocPrint(alloc, "{s}/zig-bin", .{root_abs});
    defer alloc.free(zig_path);

    var env_map: std.process.Environ.Map = .init(alloc);
    defer env_map.deinit();
    try env_map.put("TMPDIR", scratch_dir);

    const base_opt = Options{
        .io = io,
        .zig_exe = zig_path,
        .target = "aarch64-macos.26.0",
        .sysroot = sdk_dir,
        .profile = "test",
        .environ_map = &env_map,
        .cache_dir = cache_dir,
        .quiet = true,
    };

    const k0 = try computeCacheKey(alloc, base_opt);

    // TMPDIR change alone must NOT invalidate the cache key.
    try env_map.put("TMPDIR", "/tmp");
    const k_tmpdir = try computeCacheKey(alloc, base_opt);
    try std.testing.expectEqualSlices(u8, &k0, &k_tmpdir);
    try env_map.put("TMPDIR", scratch_dir);

    // Warm the cache for base_opt.
    var st0 = TestRunnerState{};
    const r0 = try generateWithRunner(alloc, alloc, base_opt, contract.checks, mockContractRunner, &st0);
    alloc.free(r0.config_h);
    try std.testing.expectEqual(contract.checks.len, st0.calls);

    // Target change -> new key & miss.
    var target_opt = base_opt;
    target_opt.target = "x86_64-macos.26.0";
    const k_target = try computeCacheKey(alloc, target_opt);
    try std.testing.expect(!std.mem.eql(u8, &k0, &k_target));

    // Profile change -> new key & miss.
    var profile_opt = base_opt;
    profile_opt.profile = "debug-static";
    const k_profile = try computeCacheKey(alloc, profile_opt);
    try std.testing.expect(!std.mem.eql(u8, &k0, &k_profile));
    var st_prof = TestRunnerState{};
    const r_prof = try generateWithRunner(alloc, alloc, profile_opt, contract.checks, mockContractRunner, &st_prof);
    alloc.free(r_prof.config_h);
    try std.testing.expectEqual(contract.checks.len, st_prof.calls);

    // SDKSettings.plist mutation under the same sysroot path -> new key & miss.
    try tmp_root.dir.writeFile(io, .{ .sub_path = "sdk/SDKSettings.plist", .data = "<plist>26.6</plist>\n" });
    const k_sdk = try computeCacheKey(alloc, base_opt);
    try std.testing.expect(!std.mem.eql(u8, &k0, &k_sdk));
    var st_sdk = TestRunnerState{};
    const r_sdk = try generateWithRunner(alloc, alloc, base_opt, contract.checks, mockContractRunner, &st_sdk);
    alloc.free(r_sdk.config_h);
    try std.testing.expectEqual(contract.checks.len, st_sdk.calls);

    // SDKSettings.json mutation under the same sysroot path -> new key & miss.
    try tmp_root.dir.writeFile(io, .{ .sub_path = "sdk/SDKSettings.json", .data = "{\"Version\":\"26.6\"}\n" });
    const k_sdk_json = try computeCacheKey(alloc, base_opt);
    try std.testing.expect(!std.mem.eql(u8, &k_sdk, &k_sdk_json));

    // zig_exe content/size mutation -> new key.
    try tmp_root.dir.writeFile(io, .{ .sub_path = "zig-bin", .data = "zig-v2-different-size" });
    const k_zig = try computeCacheKey(alloc, base_opt);
    try std.testing.expect(!std.mem.eql(u8, &k_sdk_json, &k_zig));

    // CC and CXX env overrides -> new keys.
    try env_map.put("CC", "/custom/clang");
    const k_cc = try computeCacheKey(alloc, base_opt);
    try std.testing.expect(!std.mem.eql(u8, &k_zig, &k_cc));

    try env_map.put("CXX", "/custom/clang++");
    const k_cxx = try computeCacheKey(alloc, base_opt);
    try std.testing.expect(!std.mem.eql(u8, &k_cc, &k_cxx));
}

test "corrupt cache record is rejected, triggers fresh probes, and heals atomically" {
    const alloc = std.testing.allocator;
    const io = std.testing.io;

    var tmp_root = std.testing.tmpDir(.{ .iterate = true });
    defer tmp_root.cleanup();

    try tmp_root.dir.createDir(io, "cache", .default_dir);
    try tmp_root.dir.createDir(io, "scratch", .default_dir);
    try tmp_root.dir.createDir(io, "sdk", .default_dir);

    const root_abs = try tmp_root.dir.realPathFileAlloc(io, ".", alloc);
    defer alloc.free(root_abs);
    const cache_dir = try std.fmt.allocPrint(alloc, "{s}/cache", .{root_abs});
    defer alloc.free(cache_dir);
    const scratch_dir = try std.fmt.allocPrint(alloc, "{s}/scratch", .{root_abs});
    defer alloc.free(scratch_dir);
    const sdk_dir = try std.fmt.allocPrint(alloc, "{s}/sdk", .{root_abs});
    defer alloc.free(sdk_dir);

    var env_map: std.process.Environ.Map = .init(alloc);
    defer env_map.deinit();
    try env_map.put("TMPDIR", scratch_dir);

    const opt = Options{
        .io = io,
        .zig_exe = "zig",
        .target = "aarch64-macos.26.0",
        .sysroot = sdk_dir,
        .profile = "test",
        .environ_map = &env_map,
        .cache_dir = cache_dir,
        .quiet = true,
    };

    var st1 = TestRunnerState{};
    const initial = try generateWithRunner(alloc, alloc, opt, contract.checks, mockContractRunner, &st1);
    defer alloc.free(initial.config_h);
    try std.testing.expectEqual(contract.checks.len, st1.calls);

    const key_hex = try computeCacheKey(alloc, opt);
    const rel_cache_file = try std.fmt.allocPrint(alloc, "cache/{s}.txt", .{key_hex});
    defer alloc.free(rel_cache_file);

    // 1. Corrupt payload byte without updating digest -> must recover cleanly.
    const valid_bytes = try tmp_root.dir.readFileAlloc(io, rel_cache_file, alloc, .limited(max_cache_file_bytes));
    const tampered = try alloc.dupe(u8, valid_bytes);
    alloc.free(valid_bytes);
    tampered[tampered.len - 2] = if (tampered[tampered.len - 2] == 'X') 'Y' else 'X';
    try tmp_root.dir.writeFile(io, .{ .sub_path = rel_cache_file, .data = tampered });
    alloc.free(tampered);

    var st2 = TestRunnerState{};
    const recovered1 = try generateWithRunner(alloc, alloc, opt, contract.checks, mockContractRunner, &st2);
    defer alloc.free(recovered1.config_h);
    try std.testing.expectEqual(contract.checks.len, st2.calls);
    try std.testing.expectEqualSlices(u8, initial.config_h, recovered1.config_h);

    // Healed cache file must now hit cleanly without running probes.
    const healed = try generateWithRunner(alloc, alloc, opt, contract.checks, panicIfCalledRunner, null);
    defer alloc.free(healed.config_h);
    try std.testing.expectEqualSlices(u8, initial.config_h, healed.config_h);

    // 2. Truncated / garbage file -> must fail-closed and re-run probes.
    try tmp_root.dir.writeFile(io, .{ .sub_path = rel_cache_file, .data = "pal-config-cache-v1\nbroken" });
    var st3 = TestRunnerState{};
    const recovered2 = try generateWithRunner(alloc, alloc, opt, contract.checks, mockContractRunner, &st3);
    defer alloc.free(recovered2.config_h);
    try std.testing.expectEqual(contract.checks.len, st3.calls);
    try std.testing.expectEqualSlices(u8, initial.config_h, recovered2.config_h);

    // 3. Unit-level parse checks: wrong key, flipped tail_calls, invalid bool, trailing byte.
    const good_rec = try encodeCacheRecord(alloc, &key_hex, initial);
    defer alloc.free(good_rec);
    var other_key = key_hex;
    other_key[0] = if (other_key[0] == 'a') 'b' else 'a';
    try std.testing.expectError(error.CorruptCache, parseCacheRecord(alloc, good_rec, &other_key));

    const flipped_tc = try std.mem.replaceOwned(u8, alloc, good_rec, "\ntail_calls=1\n", "\ntail_calls=0\n");
    defer alloc.free(flipped_tc);
    try std.testing.expectError(error.CorruptCache, parseCacheRecord(alloc, flipped_tc, &key_hex));

    const bad_tc = try std.mem.replaceOwned(u8, alloc, good_rec, "\ntail_calls=1\n", "\ntail_calls=2\n");
    defer alloc.free(bad_tc);
    try std.testing.expectError(error.CorruptCache, parseCacheRecord(alloc, bad_tc, &key_hex));

    const with_trailing = try std.fmt.allocPrint(alloc, "{s}!", .{good_rec});
    defer alloc.free(with_trailing);
    try std.testing.expectError(error.CorruptCache, parseCacheRecord(alloc, with_trailing, &key_hex));
}

test "failed probes never cache and always clean up scratch dir via defer" {
    const alloc = std.testing.allocator;
    const io = std.testing.io;

    var tmp_root = std.testing.tmpDir(.{ .iterate = true });
    defer tmp_root.cleanup();

    try tmp_root.dir.createDir(io, "cache", .default_dir);
    try tmp_root.dir.createDir(io, "scratch", .default_dir);
    try tmp_root.dir.createDir(io, "sdk", .default_dir);

    const root_abs = try tmp_root.dir.realPathFileAlloc(io, ".", alloc);
    defer alloc.free(root_abs);
    const cache_dir = try std.fmt.allocPrint(alloc, "{s}/cache", .{root_abs});
    defer alloc.free(cache_dir);
    const scratch_dir = try std.fmt.allocPrint(alloc, "{s}/scratch", .{root_abs});
    defer alloc.free(scratch_dir);
    const sdk_dir = try std.fmt.allocPrint(alloc, "{s}/sdk", .{root_abs});
    defer alloc.free(sdk_dir);

    var env_map: std.process.Environ.Map = .init(alloc);
    defer env_map.deinit();
    try env_map.put("TMPDIR", scratch_dir);

    const opt = Options{
        .io = io,
        .zig_exe = "/nonexistent/zig-does-not-exist",
        .target = "aarch64-macos.26.0",
        .sysroot = sdk_dir,
        .profile = "test",
        .environ_map = &env_map,
        .cache_dir = cache_dir,
        .quiet = true,
    };

    // Case 1: contract deviation cleans up scratch dir and writes no cache entry.
    var st_dev = TestRunnerState{ .fail_name = "HAVE_KQUEUE", .observed_tmpdir_prefix = scratch_dir };
    try std.testing.expectError(
        error.ContractDeviation,
        generateWithRunner(alloc, alloc, opt, contract.checks, mockContractRunner, &st_dev),
    );
    try std.testing.expect(st_dev.saw_tmpdir_exists);

    // Case 2: probe spawn error mid-run cleans up scratch dir and writes no cache entry.
    var st_err = TestRunnerState{ .error_on_name = "HAVE_KQUEUE", .observed_tmpdir_prefix = scratch_dir };
    try std.testing.expectError(
        error.ProbeSpawnFailed,
        generateWithRunner(alloc, alloc, opt, contract.checks, mockContractRunner, &st_err),
    );

    // Case 3: real compileAndMaybeRun failure (missing zig_exe) cleans up scratch dir.
    try std.testing.expectError(
        error.ProbeSpawnFailed,
        generateWithRunner(alloc, alloc, opt, contract.checks, compileAndMaybeRun, null),
    );

    var scratch_open = try tmp_root.dir.openDir(io, "scratch", .{ .iterate = true });
    defer scratch_open.close(io);
    try std.testing.expectEqual(@as(usize, 0), try countScratchDirs(io, scratch_open));

    var cache_open = try tmp_root.dir.openDir(io, "cache", .{ .iterate = true });
    defer cache_open.close(io);
    var cache_entries: usize = 0;
    var cit = cache_open.iterate();
    while (try cit.next(io)) |_| cache_entries += 1;
    try std.testing.expectEqual(@as(usize, 0), cache_entries);
}
