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
const contract = @import("contract.zig");
const probes = @import("probes.zig");

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
};

pub const Output = struct {
    config_h: []u8,
    tail_calls: bool,
};

fn fail(comptime fmt: []const u8, args: anytype) noreturn {
    std.debug.print("pal-config error: " ++ fmt ++ "\n", args);
    std.process.exit(1);
}

const Outcome = struct {
    passed: bool,
    log: []u8,
    observed_size: u8 = 0,
};

const Ctx = struct {
    alloc: std.mem.Allocator,
    opt: Options,
    tmpdir: []const u8,
    outcomes: []Outcome,
};

fn isCxx(c: contract.Check) bool {
    return switch (c.params) {
        .symbol, .run, .run_werror => true,
        else => false,
    };
}

fn renderSource(alloc: std.mem.Allocator, c: contract.Check) []u8 {
    var buf: std.ArrayList(u8) = .empty;
    switch (c.params) {
        .include => |h| buf.print(alloc, "#include <{s}>\n\n\nint main(void){{return 0;}}\n", .{h}) catch @panic("OOM"),
        .func, .libfunc => buf.appendSlice(alloc, probes.func_tmpl) catch @panic("OOM"),
        .@"struct" => |p| {
            for (p.headers) |h| buf.print(alloc, "#include <{s}>\n", .{h}) catch @panic("OOM");
            buf.print(alloc, "\nint main(void)\n{{\n  (void)sizeof((({s} *)0)->{s});\n  return 0;\n}}\n", .{ p.st, p.member }) catch @panic("OOM");
        },
        .typesize => |p| {
            buf.appendSlice(alloc, "#include <sys/types.h>\n#include <stdint.h>\n#include <stddef.h>\n") catch @panic("OOM");
            for (p.extras) |h| buf.print(alloc, "#include \"{s}\"\n", .{h}) catch @panic("OOM");
            buf.print(alloc, "\nint main(void){{ (void)sizeof({s}); return 0; }}\n", .{p.typ}) catch @panic("OOM");
        },
        .sizeof_eq => |p| {
            buf.print(alloc, "#include <sys/types.h>\n#include <stdint.h>\n#include <stddef.h>\n_Static_assert(sizeof({s}) == {d}, \"{s} size\");\nint main(void){{return 0;}}\n", .{ p.typ, p.size, p.typ }) catch @panic("OOM");
        },
        .symbol => |p| {
            buf.print(alloc, "#include <{s}>\n\nint main(int argc, char** argv)\n{{\n  (void)argv;\n#ifndef {s}\n  return ((int*)(&{s}))[argc];\n#else\n  (void)argc;\n  return 0;\n#endif\n}}\n", .{ p.header, p.sym, p.sym }) catch @panic("OOM");
        },
        .run => |p| buf.appendSlice(alloc, p.body) catch @panic("OOM"),
        .run_werror => |body| buf.appendSlice(alloc, body) catch @panic("OOM"),
    }
    return buf.toOwnedSlice(alloc) catch @panic("OOM");
}

fn readPipeToEnd(alloc: std.mem.Allocator, io: std.Io, file: std.Io.File) []u8 {
    var r = file.readerStreaming(io, &.{});
    return r.interface.allocRemaining(alloc, .limited(64 * 1024)) catch |err| switch (err) {
        error.StreamTooLong => @panic("probe output exceeded the 64 KiB pipe cap"),
        else => @panic("OOM"),
    };
}

fn compileAndMaybeRun(ctx: *Ctx, c: contract.Check, src: []const u8, tag: []const u8) Outcome {
    const alloc = ctx.alloc;
    const io = ctx.opt.io;
    const ext = if (isCxx(c)) ".cpp" else ".c";
    const src_path = std.fmt.allocPrint(alloc, "{s}/{s}{s}", .{ ctx.tmpdir, tag, ext }) catch @panic("OOM");
    defer alloc.free(src_path);
    const exe_path = std.fmt.allocPrint(alloc, "{s}/{s}.exe", .{ ctx.tmpdir, tag }) catch @panic("OOM");
    defer alloc.free(exe_path);
    {
        const f = std.Io.Dir.createFileAbsolute(io, src_path, .{}) catch |err|
            fail("cannot write probe source {s}: {t}", .{ src_path, err });
        defer f.close(io);
        f.writeStreamingAll(io, src) catch |err|
            fail("cannot write probe source {s}: {t}", .{ src_path, err });
    }

    var argv: std.ArrayList([]const u8) = .empty;
    defer argv.deinit(alloc);
    argv.appendSlice(alloc, &.{ ctx.opt.zig_exe, if (isCxx(c)) "c++" else "cc" }) catch @panic("OOM");
    const target_flag = std.fmt.allocPrint(alloc, "--target={s}", .{ctx.opt.target}) catch @panic("OOM");
    defer alloc.free(target_flag);
    const sysroot_flag = std.fmt.allocPrint(alloc, "--sysroot={s}", .{ctx.opt.sysroot}) catch @panic("OOM");
    defer alloc.free(sysroot_flag);
    const sdk_isystem = std.fmt.allocPrint(alloc, "{s}/usr/include", .{ctx.opt.sysroot}) catch @panic("OOM");
    defer alloc.free(sdk_isystem);
    argv.appendSlice(alloc, &.{ target_flag, sysroot_flag }) catch @panic("OOM");
    // Every configure.cmake probe inherits -D_FILE_OFFSET_BITS=64 via
    // CMAKE_REQUIRED_DEFINITIONS (configure.cmake:23), but the
    // CMakeFeatureDetect tail-calls probe lives in a separate directory scope
    // and its reference command line lacks it, so omit it there.
    if (c.params != .run_werror) argv.append(alloc, "-D_FILE_OFFSET_BITS=64") catch @panic("OOM");
    argv.appendSlice(alloc, &.{
        if (isCxx(c)) "-std=gnu++17" else "-std=gnu99",
        "-O0",         "-isystem", sdk_isystem,
    }) catch @panic("OOM");
    var extra_flag: ?[]u8 = null;
    defer if (extra_flag) |f| alloc.free(f);
    switch (c.params) {
        .func => |f| {
            extra_flag = std.fmt.allocPrint(alloc, "-DCHECK_FUNCTION_EXISTS={s}", .{f}) catch @panic("OOM");
            argv.append(alloc, extra_flag.?) catch @panic("OOM");
        },
        .libfunc => |p| {
            extra_flag = std.fmt.allocPrint(alloc, "-DCHECK_FUNCTION_EXISTS={s}", .{p.func}) catch @panic("OOM");
            argv.append(alloc, extra_flag.?) catch @panic("OOM");
        },
        .run_werror => argv.append(alloc, "-Werror") catch @panic("OOM"),
        else => {},
    }
    argv.appendSlice(alloc, &.{ src_path, "-o", exe_path }) catch @panic("OOM");
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
        const f = std.fmt.allocPrint(alloc, "-l{s}", .{lib}) catch @panic("OOM");
        lib_flags.append(alloc, f) catch @panic("OOM");
        argv.append(alloc, f) catch @panic("OOM");
    }

    // Probe stdout is always empty (diagnostics go to stderr), so reading
    // stderr first cannot deadlock: stderr stays far below the pipe buffer.
    var child = std.process.spawn(io, .{
        .argv = argv.items,
        .stdin = .ignore,
        .stdout = .pipe,
        .stderr = .pipe,
    }) catch |err| fail("cannot run {s} {s}: {t}", .{ ctx.opt.zig_exe, argv.items[1], err });
    const stderr = readPipeToEnd(alloc, io, child.stderr.?);
    const stdout = readPipeToEnd(alloc, io, child.stdout.?);
    alloc.free(stdout);
    // Note: child.wait owns pipe cleanup; closing here would double-close.
    const term = child.wait(io) catch |err| fail("probe {s} wait failed: {t}", .{ c.name, err });
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
        .stdin = .ignore,
        .stdout = .ignore,
        .stderr = .ignore,
    }) catch |err| fail("cannot execute probe {s}: {t} (native execution is required)", .{ exe_path, err });
    const rterm = run.wait(io) catch |err| fail("probe {s} wait failed: {t}", .{ c.name, err });
    if (!rterm.success()) {
        alloc.free(stderr);
        const what: []const u8 = switch (rterm) {
            .exited => |code| std.fmt.allocPrint(alloc, "exit code {d}", .{code}) catch @panic("OOM"),
            .signal => "terminating signal",
            .stopped => "stop signal",
            .unknown => "unknown termination",
        };
        const msg = std.fmt.allocPrint(alloc, "probe did not exit 0 ({s})", .{what}) catch @panic("OOM");
        if (rterm == .exited) alloc.free(what);
        return .{ .passed = false, .log = msg };
    }
    alloc.free(stderr);
    return .{ .passed = true, .log = &.{} };
}

fn diagnoseSize(ctx: *Ctx, typ: []const u8) u8 {
    // The ==8 assertion failed: find the actual size for a precise report.
    for ([_]u8{ 1, 2, 4, 16 }) |n| {
        var buf: std.ArrayList(u8) = .empty;
        buf.print(ctx.alloc, "#include <sys/types.h>\n#include <stdint.h>\n#include <stddef.h>\n_Static_assert(sizeof({s}) == {d}, \"size\");\nint main(void){{return 0;}}\n", .{ typ, n }) catch @panic("OOM");
        const src = buf.toOwnedSlice(ctx.alloc) catch @panic("OOM");
        defer ctx.alloc.free(src);
        const tag = std.fmt.allocPrint(ctx.alloc, "sizeof_{s}_{d}", .{ typ, n }) catch @panic("OOM");
        defer ctx.alloc.free(tag);
        const probe = contract.Check{
            .name = tag,
            .cite = "diagnostic",
            .expected = true,
            .params = .{ .typesize = .{ .typ = typ, .extras = &.{} } },
        };
        const o = compileAndMaybeRun(ctx, probe, src, tag);
        freeLog(ctx.alloc, o.log);
        if (o.passed) return n;
    }
    return 0;
}

fn runOne(ctx: *Ctx, i: usize) void {
    const c = contract.checks[i];
    const src = renderSource(ctx.alloc, c);
    defer ctx.alloc.free(src);
    var o = compileAndMaybeRun(ctx, c, src, c.name);
    if (c.params == .sizeof_eq and !o.passed) {
        o.observed_size = diagnoseSize(ctx, c.params.sizeof_eq.typ);
    }
    ctx.outcomes[i] = o;
}

fn freeLog(alloc: std.mem.Allocator, log: []u8) void {
    // Success outcomes carry a static empty log; only free real allocations.
    if (log.len > 0) alloc.free(log);
}

fn printLogHead(log: []const u8) void {
    var lines: usize = 0;
    var it = std.mem.splitScalar(u8, log, '\n');
    while (it.next()) |line| {
        if (lines >= 8) {
            std.debug.print("      ...\n", .{});
            break;
        }
        if (line.len > 0) {
            const show = if (line.len > 200) line[0..200] else line;
            std.debug.print("      | {s}\n", .{show});
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

pub fn generate(opt: Options) Output {
    // Build scripts run single-threaded, so checks run serially (~0.2s each).
    // Page-backed allocation avoids tying scratch memory to the build arena.
    // The returned config_h is page-backed; the caller copies it and frees it.
    const alloc = std.heap.page_allocator;

    for (contract.checks) |c| {
        if (fixedValue(c.name) != null) fail("contract error: {s} is both fixed and checked", .{c.name});
    }

    var seed: u64 = undefined;
    std.Io.random(opt.io, std.mem.asBytes(&seed));
    const tmpdir = std.fmt.allocPrint(alloc, "/tmp/chakra-pal-config-{x}", .{seed}) catch @panic("OOM");
    std.Io.Dir.createDirAbsolute(opt.io, tmpdir, .default_dir) catch |err|
        fail("cannot create probe dir {s}: {t}", .{ tmpdir, err });

    const outcomes = alloc.alloc(Outcome, contract.checks.len) catch @panic("OOM");
    var ctx = Ctx{
        .alloc = alloc,
        .opt = opt,
        .tmpdir = tmpdir,
        .outcomes = outcomes,
    };

    for (0..contract.checks.len) |i| runOne(&ctx, i);

    var failed: usize = 0;
    for (contract.checks, outcomes) |c, o| {
        if (o.passed != c.expected) failed += 1;
    }
    if (failed > 0) {
        std.debug.print("pal-config: {d}/{d} checks deviate from doc/pal-config-contract-mac.md section 2\n", .{ failed, contract.checks.len });
        for (contract.checks, outcomes) |c, o| {
            if (o.passed == c.expected) continue;
            if (c.params == .sizeof_eq) {
                if (o.observed_size == 0) {
                    std.debug.print("  {s}: contract {s}={d} but size unidentified (tried 1,2,4,8,16)\n", .{ c.name, c.cite, c.params.sizeof_eq.size });
                } else {
                    std.debug.print("  {s}: contract {s}={d} but observed {d}\n", .{ c.name, c.cite, c.params.sizeof_eq.size, o.observed_size });
                }
            } else {
                std.debug.print("  {s}: contract {s}={d} but observed {d}\n", .{ c.name, c.cite, @as(u8, @intFromBool(c.expected)), @as(u8, @intFromBool(o.passed)) });
            }
            printLogHead(o.log);
        }
        std.debug.print("probe sources kept in {s}; resolve by contract update with fresh evidence, never by editing generated output\n", .{tmpdir});
        std.process.exit(1);
    }

    var tail_calls = false;
    for (contract.checks, outcomes) |c, o| {
        if (std.mem.eql(u8, c.name, "CLANG_HAS_DISABLE_TAIL_CALLS")) tail_calls = o.passed;
        freeLog(alloc, o.log);
    }

    var hdr: std.ArrayList(u8) = .empty;
    for (contract.layout) |item| {
        switch (item) {
            .lit => |s| hdr.print(alloc, "{s}\n", .{s}) catch @panic("OOM"),
            .b01 => |name| {
                const v: bool = if (fixedValue(name)) |f| f else blk: {
                    var found: ?bool = null;
                    for (contract.checks, outcomes) |c, o| {
                        if (std.mem.eql(u8, c.name, name)) {
                            found = o.passed;
                            break;
                        }
                    }
                    break :blk found orelse fail("contract error: no value for {s}", .{name});
                };
                hdr.print(alloc, "#define {s} {d}\n", .{ name, @as(u8, @intFromBool(v)) }) catch @panic("OOM");
            },
            .off_t => {
                var size: u8 = 0;
                for (contract.checks) |c| {
                    if (c.params == .sizeof_eq) size = c.params.sizeof_eq.size;
                }
                hdr.print(alloc, "#define SIZEOF_OFF_T {d}\n", .{size}) catch @panic("OOM");
            },
            .val => |p| hdr.print(alloc, "#define {s} {s}\n", .{ p.name, p.value }) catch @panic("OOM"),
            .undef => |name| hdr.print(alloc, "/* #undef {s} */\n", .{name}) catch @panic("OOM"),
            .ptrace => hdr.print(alloc, "{s}\n", .{contract.ptrace_line}) catch @panic("OOM"),
        }
    }

    std.debug.print("pal-config: {d}/{d} checks match the contract ({d} fixed-by-target, {d} compile-checked, {d} runtime-verified) [profile {s}]\n", .{ contract.checks.len, contract.checks.len, contract.num_fixed_by_target, contract.num_compile_checked, contract.num_runtime_verified, opt.profile });
    if (std.Io.Dir.openDirAbsolute(opt.io, "/tmp", .{})) |parent| {
        parent.deleteTree(opt.io, tmpdir["/tmp/".len..]) catch {};
    } else |_| {}
    alloc.free(tmpdir);
    alloc.free(outcomes);
    return .{
        .config_h = hdr.toOwnedSlice(alloc) catch @panic("OOM"),
        .tail_calls = tail_calls,
    };
}
