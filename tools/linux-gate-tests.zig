//-------------------------------------------------------------------------------------------------------
// Copyright (c) ChakraCore Project Contributors. All rights reserved.
// Licensed under the MIT license. See LICENSE.txt file in the project root for full license information.
//-------------------------------------------------------------------------------------------------------
//! Test root for the Linux gate unit tests, run by the Linux
//! `check-selftest` step (tools/build/linux.zig): the PAL-config
//! offline-render test in pal-config-linux/check.zig and the
//! gate-helper tests in build/linux.zig.
//!
//! This file lives directly under tools/ because Zig 0.17 roots a
//! module at its root file's directory and rejects imports outside
//! that directory: pal-config-linux/probes.zig imports
//! `../pal-config/probes.zig`, so a test root inside
//! pal-config-linux/ (or build/) cannot compile that graph (see
//! the header of pal-config-linux/probes.zig). Referencing the
//! files from a test block makes the test runner collect their
//! test declarations.

const std = @import("std");
const linux = @import("build/linux.zig");
const tables_linux = @import("build/tables_linux.zig");
const tables = @import("build/tables.zig");

test {
    _ = @import("pal-config-linux/check.zig");
    _ = linux;
}

fn containsStr(haystack: []const []const u8, needle: []const u8) bool {
    for (haystack) |item| {
        if (std.mem.eql(u8, item, needle)) return true;
    }
    return false;
}

test "linux defaults preserve x86_64-linux-gnu, glibc 2.39, ICU 74" {
    try std.testing.expectEqualStrings("x86_64-linux-gnu", linux.profile_target);
    try std.testing.expectEqualStrings("2.39", linux.default_glibc);
    try std.testing.expectEqualStrings("74", linux.default_icu_major);
    try std.testing.expectEqualStrings("test", linux.Profile.@"test".name());
    try std.testing.expectEqualStrings("debug-static", linux.Profile.debug_static.name());
    try std.testing.expectEqualStrings("release-shared", linux.Profile.release_shared.name());
}

test "linux parameterized profile, glibc, ICU, and no-jit contracts" {
    try std.testing.expect(linux.isStaticProfile(.debug_static));
    try std.testing.expect(!linux.isStaticProfile(.@"test"));
    try std.testing.expect(!linux.isStaticProfile(.release_shared));
    try std.testing.expect(linux.isReleaseProfile(.release_shared));
    try std.testing.expect(!linux.isReleaseProfile(.@"test"));

    try std.testing.expect(linux.glibcFirstLineMatches("ldd (GNU libc) 2.40", "2.40"));
    try std.testing.expect(!linux.glibcFirstLineMatches("ldd (GNU libc) 2.40", "2.39"));
    try std.testing.expect(linux.uvernumMatchesMajor("#define U_ICU_VERSION_SHORT \"72\"\n", "72"));
    try std.testing.expect(!linux.uvernumMatchesMajor("#define U_ICU_VERSION_SHORT \"72\"\n", "74"));

    try std.testing.expectEqual(@as(u32, 695), linux.expectedCompileCount(.@"test", true));
    try std.testing.expectEqual(@as(u32, 604), linux.expectedCompileCount(.@"test", false));
    try std.testing.expectEqual(@as(u32, 693), linux.expectedCompileCount(.debug_static, true));
    try std.testing.expectEqual(@as(u32, 658), linux.expectedCompileCount(.release_shared, true));
}

test "linux sanitizer options, install prefixes, and gate applicability" {
    try std.testing.expectEqual(linux.Sanitize.none, linux.parseSanitize("none").?);
    try std.testing.expectEqual(linux.Sanitize.address, linux.parseSanitize("address").?);
    try std.testing.expectEqual(linux.Sanitize.undefined, linux.parseSanitize("undefined").?);
    try std.testing.expectEqual(linux.Sanitize.thread, linux.parseSanitize("thread").?);
    try std.testing.expect(linux.parseSanitize("leak") == null);

    try std.testing.expect(linux.defaultJitForSanitize(.none));
    try std.testing.expect(!linux.defaultJitForSanitize(.address));
    try std.testing.expect(!linux.defaultJitForSanitize(.undefined));
    try std.testing.expect(!linux.defaultJitForSanitize(.thread));

    try std.testing.expectEqualStrings("-fsanitize=address", linux.sanitizeCompileFlag(.address).?);
    try std.testing.expectEqualStrings("-fsanitize=undefined", linux.sanitizeCompileFlag(.undefined).?);
    try std.testing.expectEqualStrings("-fsanitize=thread", linux.sanitizeCompileFlag(.thread).?);
    try std.testing.expect(linux.sanitizeCompileFlag(.none) == null);

    try std.testing.expect(linux.supportsWasmGate(.@"test", true));
    try std.testing.expect(linux.supportsWasmGate(.debug_static, true));
    try std.testing.expect(!linux.supportsWasmGate(.release_shared, true));
    try std.testing.expect(!linux.supportsWasmGate(.@"test", false));
    try std.testing.expect(linux.supportsFullJitGate(.@"test", true));
    try std.testing.expect(!linux.supportsFullJitGate(.release_shared, true));
    try std.testing.expect(!linux.supportsFullJitGate(.@"test", false));

    const arena = std.testing.allocator;
    try std.testing.expectEqualStrings("zig-out", linux.installPrefixForConfig(arena, .@"test", .none));
    const p_asan = linux.installPrefixForConfig(arena, .@"test", .address);
    defer arena.free(p_asan);
    try std.testing.expectEqualStrings("out/sanitize-address", p_asan);
    const p_tsan = linux.installPrefixForConfig(arena, .debug_static, .thread);
    defer arena.free(p_tsan);
    try std.testing.expectEqualStrings("out/sanitize-thread", p_tsan);
}

test "Phase-4 TUs present in tables_linux.zig / linux.zig with zero duplicates and on disk" {
    const allocator = std.testing.allocator;
    const io = std.testing.io;
    const cwd = std.Io.Dir.cwd();

    // Note: SuppressedError is implemented inside JavascriptError.cpp and
    // JavascriptDisposableStack.cpp (no separate JavascriptSuppressedError.cpp TU exists).
    const phase4_library_tus = [_][]const u8{
        "lib/Runtime/Library/IntlDisplayNames.cpp",
        "lib/Runtime/Library/IntlDurationFormat.cpp",
        "lib/Runtime/Library/IntlEngineInterfaceExtensionObject.cpp",
        "lib/Runtime/Library/IntlEnumeration.cpp",
        "lib/Runtime/Library/IntlListFormat.cpp",
        "lib/Runtime/Library/IntlLocale.cpp",
        "lib/Runtime/Library/IntlModernShared.cpp",
        "lib/Runtime/Library/IntlRelativeTimeFormat.cpp",
        "lib/Runtime/Library/IntlSegmenter.cpp",
        "lib/Runtime/Library/JavascriptDisposableStack.cpp",
        "lib/Runtime/Library/JavascriptError.cpp",
        "lib/Runtime/Library/JavascriptIteratorZip.cpp",
        "lib/Runtime/Library/JavascriptIteratorChunking.cpp",
    };

    for (phase4_library_tus) |tu| {
        try std.testing.expect(containsStr(tables_linux.rt_library_group.cxx, tu));
        try std.testing.expect(containsStr(tables_linux.static_order, tu));
        try cwd.access(io, tu, .{});
    }

    // Zero duplicates in static_order and every entry exists on disk.
    var seen_static: std.StringHashMap(void) = .init(allocator);
    defer seen_static.deinit();
    for (tables_linux.static_order) |tu| {
        try std.testing.expect(!seen_static.contains(tu));
        try seen_static.put(tu, {});
        try cwd.access(io, tu, .{});
    }

    // Zero duplicates across all_groups and every group TU exists on disk.
    var seen_groups: std.StringHashMap(void) = .init(allocator);
    defer seen_groups.deinit();
    for (linux.all_groups) |g| {
        for (g.cxx) |tu| {
            try std.testing.expect(!seen_groups.contains(tu));
            try seen_groups.put(tu, {});
            try cwd.access(io, tu, .{});
        }
        for (g.c) |tu| {
            try std.testing.expect(!seen_groups.contains(tu));
            try seen_groups.put(tu, {});
            try cwd.access(io, tu, .{});
        }
        for (g.asm_files) |tu| {
            try std.testing.expect(!seen_groups.contains(tu));
            try seen_groups.put(tu, {});
            try cwd.access(io, tu, .{});
        }
    }

    // Z1 leaf Zig codec exists on disk and is wired in linux.zig.
    try cwd.access(io, "lib/ZigCodec/u8a_codec.zig", .{});
    const linux_src = @embedFile("build/linux.zig");
    try std.testing.expect(std.mem.indexOf(u8, linux_src, "\"lib/ZigCodec/u8a_codec.zig\"") != null);

    // Node-API v10 provider TUs (used by linux.zig): zero duplicates and all exist on disk.
    var seen_nodeapi: std.StringHashMap(void) = .init(allocator);
    defer seen_nodeapi.deinit();
    try std.testing.expectEqual(@as(usize, 16), tables.nodeapi_provider_tus.len);
    for (tables.nodeapi_provider_tus) |tu| {
        try std.testing.expect(!seen_nodeapi.contains(tu));
        try seen_nodeapi.put(tu, {});
        try std.testing.expect(std.mem.startsWith(u8, tu, "lib/NodeApi/"));
        try cwd.access(io, tu, .{});
    }
    try std.testing.expect(std.mem.indexOf(u8, linux_src, "mac_tables.nodeapi_provider_tus") != null);
}
