//-------------------------------------------------------------------------------------------------------
// Copyright (c) ChakraCore Project Contributors. All rights reserved.
// Licensed under the MIT license. See LICENSE.txt file in the project root for full license information.
//-------------------------------------------------------------------------------------------------------
//! Runs the 95 B4.2 Linux PAL configuration checks through the repo-pinned Zig compiler and renders
//! pal/src/config.h. Any deviation from the contract fails the build with the setting name, the
//! contract value plus source line, and the observed value (doc/pal-config-contract-linux.md section 5).
//!
//! Probe command lines mirror the reference try_compile/try_run invocations (observed in
//! the B4.1 /tmp/chakra-b4/build-xplat/CMakeFiles/CMakeConfigureLog.yaml):
//! C probes (include/func/libfunc/struct/typesize/sizeof): the three required defines
//! (`-D_FILE_OFFSET_BITS=64 -D_DEFAULT_SOURCE -D_POSIX_C_SOURCE=200809L`, from
//! pal/src/configure.cmake:20-23), default C standard (both compilers default to gnu17; the
//! reference line carries no -std), no -Werror (CMAKE_C_FLAGS is empty), no -O flag.
//! C++ probes (symbol/run): the same three defines plus `-std=gnu++17` (CMAKE_CXX_STANDARD),
//! `-Werror` with the 25 `-Wno-*` suppressions (CMakeLists.txt:330-336, 410-436),
//! `-fno-omit-frame-pointer -fdelayed-template-parsing` (CMakeLists.txt:453-457).
//! The -Werror is verdict-critical on Linux: it flips HAVE_LARGE_SNPRINTF_SUPPORT,
//! SSCANF_CANNOT_HANDLE_MISSING_EXPONENT and FILE_OPS_CHECK_FERROR_OF_PREVIOUS_CALL to 0
//! (doc/pal-config-contract-linux.md section 3 L1-L3). The tail-calls probe keeps its own
//! as-run line: -std=gnu++17 -Werror plus suppressions, without the required defines and
//! without -fno-omit-frame-pointer/-fdelayed-template-parsing (Build/CMakeFeatureDetect.cmake
//! runs before those are appended). The reference passes no -O flag; the mirror passes an
//! explicit -O0, which is identical to the compilers' default. The per-check `-D<CHECK>`
//! macros on the reference lines are verdict-neutral (no probe source reads its own macro)
//! and omitted. Probes compile against `zig cc`'s bundled glibc headers (2.39, same
//! version as the system) and link the real system objects (contract quirk L14):
//! no --sysroot, no -isystem.
//!
//! `renderExpected` renders the header purely from the contract tables (no probing, no
//! execution) so the contract stays verifiable by config-resolution comparison where native
//! execution is unavailable (doc/b4-linux.md section 4.1); `checkExpectedAgainst` diffs such
//! a render against reference bytes and fails loudly on the first differing line.
//!
//! First online execution (B4.3, 2026-10-12): 94/95 green on the first run;
//! the single deviation (HAVE_SYSCTL) exposed a probe-link infidelity — the
//! bundled stubs over-export version-retired symbols — fixed by prepending
//! the real system link inputs to every probe link (contract quirk L14).
//! See doc/b4-linux.md section 8.

const std = @import("std");
const contract = @import("contract.zig");
const probes = @import("probes.zig");

pub const Options = struct {
    io: std.Io,
    zig_exe: []const u8,
    target: []const u8,
    /// Linux build profile name (single profile at B4.2: x64/JIT/RelWithDebInfo).
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
    std.debug.print("pal-config-linux error: " ++ fmt ++ "\n", args);
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

/// The 25 -Wno-* suppressions shared by every C++ probe (CMakeLists.txt:410-436,
/// observed verbatim on the reference probe lines).
const wno_suppressions: []const []const u8 = &.{
    "-Wno-ignored-attributes",
    "-Wno-parentheses-equality",
    "-Wno-reorder",
    "-Wno-microsoft",
    "-Wno-unused-value",
    "-Wno-int-to-void-pointer-cast",
    "-Wno-invalid-offsetof",
    "-Wno-undefined-inline",
    "-Wno-inconsistent-missing-override",
    "-Wno-macro-redefined",
    "-Wno-pragmas",
    "-Wno-invalid-token-paste",
    "-Wno-format",
    "-Wno-invalid-noreturn",
    "-Wno-null-arithmetic",
    "-Wno-tautological-constant-out-of-range-compare",
    "-Wno-tautological-undefined-compare",
    "-Wno-address-of-temporary",
    "-Wno-null-conversion",
    "-Wno-return-type",
    "-Wno-switch",
    "-Wno-int-to-pointer-cast",
    "-Wno-tautological-constant-compare",
    "-Wno-enum-compare-switch",
    "-Wno-unknown-warning-option",
};

/// Real system link inputs, prepended to every probe link (contract quirk
/// L14). Ubuntu 24.04 floor paths; existence-gated in generate().
pub const system_libc = "/lib/x86_64-linux-gnu/libc.so.6";
pub const system_libm = "/lib/x86_64-linux-gnu/libm.so.6";
pub const system_libpthread = "/lib/x86_64-linux-gnu/libpthread.so.0";
pub const system_ldso = "/lib64/ld-linux-x86-64.so.2";

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
    argv.append(alloc, target_flag) catch @panic("OOM");
    // Every configure.cmake probe inherits the three required defines via
    // CMAKE_REQUIRED_DEFINITIONS (configure.cmake:20-23), but the
    // CMakeFeatureDetect tail-calls probe lives in a separate directory scope
    // and its reference command line lacks them, so omit them there.
    if (c.params != .run_werror) argv.appendSlice(alloc, &.{
        "-D_FILE_OFFSET_BITS=64",
        "-D_DEFAULT_SOURCE",
        "-D_POSIX_C_SOURCE=200809L",
    }) catch @panic("OOM");
    if (isCxx(c)) {
        argv.appendSlice(alloc, &.{ "-std=gnu++17", "-O0", "-Werror" }) catch @panic("OOM");
        argv.appendSlice(alloc, wno_suppressions) catch @panic("OOM");
        // The tail-calls probe runs before -fno-omit-frame-pointer and
        // -fdelayed-template-parsing are appended (CMakeLists.txt:450-457),
        // and its reference line lacks them.
        if (c.params != .run_werror) argv.appendSlice(alloc, &.{
            "-fno-omit-frame-pointer",
            "-fdelayed-template-parsing",
        }) catch @panic("OOM");
    } else {
        // Reference C lines carry no -std (default gnu17 both compilers),
        // no -Werror (CMAKE_C_FLAGS is empty) and no -O flag (-O0 default).
        argv.append(alloc, "-O0") catch @panic("OOM");
    }
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
        else => {},
    }
    argv.append(alloc, src_path) catch @panic("OOM");
    const libs: []const []const u8 = switch (c.params) {
        .libfunc => |p| &.{p.lib},
        .run => |p| p.libs,
        else => &.{},
    };
    // Real system link inputs first (contract quirk L14): `zig cc` links
    // its bundled glibc stubs, which over-export version-retired symbols
    // (HAVE_SYSCTL binds there but must fail: the real libc keeps
    // `sysctl` only as the non-default `sysctl@GLIBC_2.2.5`, which no new
    // link may bind). The reference links every try_compile against the
    // system C library, so probe links carry the real system objects
    // ahead of everything else: LLD consults them first and reports
    // version-unusable symbols as undefined, exactly like the reference
    // GNU ld failure. The dynamic loader is an explicit input too,
    // otherwise libc.so.6's GLIBC_PRIVATE references fail zig's
    // --no-allow-shlib-undefined.
    for (libs) |lib| {
        if (std.mem.eql(u8, lib, "pthread")) {
            argv.append(alloc, system_libpthread) catch @panic("OOM");
        } else if (std.mem.eql(u8, lib, "m")) {
            argv.append(alloc, system_libm) catch @panic("OOM");
        } else fail("probe {s}: no system link input mapped for -l{s}", .{ c.name, lib });
    }
    argv.appendSlice(alloc, &.{ system_libc, system_ldso, "-o", exe_path }) catch @panic("OOM");
    var lib_flags: std.ArrayList([]u8) = .empty;
    defer {
        for (lib_flags.items) |f| alloc.free(f);
        lib_flags.deinit(alloc);
    }
    // The reference `-l` flags stay as fallback after the real objects;
    // any residual stub/system divergence trips the fail-clearly gate.
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
        // Relative filesystem probes must not inspect or write the checkout.
        // Each configuration owns this fresh directory (including concurrent
        // profile builds), so "a_nonexistent_file" is really nonexistent.
        .cwd = .{ .path = ctx.tmpdir },
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

fn checkIndex(name: []const u8) ?usize {
    for (contract.checks, 0..) |c, i| {
        if (std.mem.eql(u8, c.name, name)) return i;
    }
    return null;
}

/// Shared header emission: `values[i]` is the 0/1 outcome for
/// `contract.checks[i]` (observed outcomes from generate, or contract
/// expectations from renderExpected).
fn emitHeader(alloc: std.mem.Allocator, values: []const bool) []u8 {
    var hdr: std.ArrayList(u8) = .empty;
    for (contract.layout) |item| {
        switch (item) {
            .lit => |s| hdr.print(alloc, "{s}\n", .{s}) catch @panic("OOM"),
            .b01 => |name| {
                const v: bool = if (fixedValue(name)) |f| f else blk: {
                    const i = checkIndex(name) orelse fail("contract error: no value for {s}", .{name});
                    break :blk values[i];
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
    return hdr.toOwnedSlice(alloc) catch @panic("OOM");
}

/// Offline render: the header as the contract tables alone resolve it, with
/// no probing and no execution. Used for config-resolution comparison where
/// native execution is unavailable.
pub fn renderExpected(alloc: std.mem.Allocator) []u8 {
    var values: [contract.checks.len]bool = undefined;
    for (contract.checks, 0..) |c, i| values[i] = c.expected;
    return emitHeader(alloc, &values);
}

/// Fails loudly unless `reference` is byte-identical to the offline render:
/// prints the first differing line number with both sides, then exits 1.
pub fn checkExpectedAgainst(alloc: std.mem.Allocator, reference: []const u8) void {
    const rendered = renderExpected(alloc);
    defer alloc.free(rendered);
    if (std.mem.eql(u8, rendered, reference)) {
        std.debug.print("pal-config-linux: offline render matches the reference header ({d} bytes)\n", .{rendered.len});
        return;
    }
    var r_it = std.mem.splitScalar(u8, rendered, '\n');
    var f_it = std.mem.splitScalar(u8, reference, '\n');
    var lineno: usize = 1;
    while (true) {
        const r = r_it.next();
        const f = f_it.next();
        if (r == null and f == null) break;
        if (r == null or f == null or !std.mem.eql(u8, r.?, f.?)) {
            std.debug.print("pal-config-linux: offline render differs from the reference header at line {d}\n", .{lineno});
            std.debug.print("  rendered : {s}\n", .{r orelse "<missing>"});
            std.debug.print("  reference: {s}\n", .{f orelse "<missing>"});
            std.process.exit(1);
        }
        lineno += 1;
    }
    unreachable; // equal lines but unequal bytes is impossible
}

pub fn generate(opt: Options) Output {
    // Build scripts run single-threaded, so checks run serially.
    // Page-backed allocation avoids tying scratch memory to the build arena.
    // The returned config_h is page-backed; the caller copies it and frees it.
    const alloc = std.heap.page_allocator;

    // Missing system link inputs (contract quirk L14) fail before probing.
    for ([_]struct { what: []const u8, path: []const u8 }{
        .{ .what = "system libc link input", .path = system_libc },
        .{ .what = "system libm link input", .path = system_libm },
        .{ .what = "system libpthread link input", .path = system_libpthread },
        .{ .what = "system dynamic-loader link input", .path = system_ldso },
    }) |need| {
        std.Io.Dir.accessAbsolute(opt.io, need.path, .{}) catch
            fail("cannot access {s} {s}: probe links require the Ubuntu 24.04 floor", .{ need.what, need.path });
    }

    for (contract.checks) |c| {
        if (fixedValue(c.name) != null) fail("contract error: {s} is both fixed and checked", .{c.name});
    }

    var seed: u64 = undefined;
    std.Io.random(opt.io, std.mem.asBytes(&seed));
    const tmpdir = std.fmt.allocPrint(alloc, "/tmp/chakra-pal-config-linux-{x}", .{seed}) catch @panic("OOM");
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
        std.debug.print("pal-config-linux: {d}/{d} checks deviate from doc/pal-config-contract-linux.md section 2\n", .{ failed, contract.checks.len });
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
    var values: [contract.checks.len]bool = undefined;
    for (contract.checks, outcomes, 0..) |c, o, i| {
        if (std.mem.eql(u8, c.name, "CLANG_HAS_DISABLE_TAIL_CALLS")) tail_calls = o.passed;
        values[i] = o.passed;
        freeLog(alloc, o.log);
    }

    const hdr = emitHeader(alloc, &values);

    std.debug.print("pal-config-linux: {d}/{d} checks match the contract ({d} fixed-by-target, {d} compile-checked, {d} runtime-verified) [profile {s}]\n", .{ contract.checks.len, contract.checks.len, contract.num_fixed_by_target, contract.num_compile_checked, contract.num_runtime_verified, opt.profile });
    if (std.Io.Dir.openDirAbsolute(opt.io, "/tmp", .{})) |parent| {
        defer parent.close(opt.io);
        parent.deleteTree(opt.io, tmpdir["/tmp/".len..]) catch {};
    } else |_| {}
    alloc.free(tmpdir);
    alloc.free(outcomes);
    return .{
        .config_h = hdr,
        .tail_calls = tail_calls,
    };
}

test "offline render carries the Linux contract values" {
    // In-memory assertions only (no probing, no file or environment access):
    // the byte-exact comparison against the reference header is performed by
    // checkExpectedAgainst through the offline verification procedure in
    // doc/pal-config-contract-linux.md section 6.
    const alloc = std.testing.allocator;
    const hdr = renderExpected(alloc);
    defer alloc.free(hdr);
    for ([_][]const u8{
        "#define PAL_PT_ATTACH PTRACE_ATTACH\n",
        "#define JA_JP_LOCALE_NAME \"ja_JP_LOCALE_NOT_FOUND\"\n",
        "#define PAL_PTRACE(cmd, pid, addr, data) ptrace((cmd), (pid), (void*)(addr), (data))\n",
        "#define HAVE_SIGRETURN 1\n",
        "#define HAVE_SYSCTL 0\n",
        "#define HAVE_PT_REGS 1\n",
        "#define HAVE_GREGSET_T 1\n",
        "#define HAVE__SC_AVPHYS_PAGES 1\n",
        "#define HAVE_SCHED_GETCPU 1\n",
        "#define HAVE_LARGE_SNPRINTF_SUPPORT 0\n",
        "#define UNGETC_NOT_RETURN_EOF 1\n",
        "#define HAS_POSIX_SEMAPHORES 1\n",
        "#define GETPWUID_R_SETS_ERRNO 1\n",
        "#define SIZEOF_OFF_T 8\n",
        "/* #undef FREEBSD_LIBC */\n",
    }) |line| {
        try std.testing.expect(std.mem.indexOf(u8, hdr, line) != null);
    }
}
