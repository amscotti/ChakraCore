//-------------------------------------------------------------------------------------------------------
// Copyright (c) ChakraCore Project Contributors. All rights reserved.
// Licensed under the MIT license. See LICENSE.txt file in the project root for full license information.
//-------------------------------------------------------------------------------------------------------
//! Gate tests for Windows x86_64 (`x86_64-windows-msvc`) build configuration,
//! TU tables, option parsers, flag/define builders, and MASM `.asm` flags.
//!
//! Runnable directly on macOS, Linux, or Windows via:
//!   zig test tools/windows-gate-tests.zig

const std = @import("std");
const windows = @import("build/windows.zig");
const tables = @import("build/tables_windows.zig");

fn expectContains(slice: []const []const u8, needle: []const u8) !void {
    for (slice) |item| {
        if (std.mem.eql(u8, item, needle)) return;
    }
    return error.TestExpectedContains;
}

fn expectNotContains(slice: []const []const u8, needle: []const u8) !void {
    for (slice) |item| {
        if (std.mem.eql(u8, item, needle)) return error.TestUnexpectedElement;
    }
}

fn allTableFiles(arena: std.mem.Allocator) []const []const u8 {
    var list: std.ArrayList([]const u8) = .empty;
    for (tables.engine_groups) |g| {
        for (g.cxx) |f| list.append(arena, f) catch @panic("OOM");
        for (g.c) |f| list.append(arena, f) catch @panic("OOM");
        for (g.asm_files) |f| list.append(arena, f) catch @panic("OOM");
    }
    for (tables.dll_group.cxx) |f| list.append(arena, f) catch @panic("OOM");
    for (tables.ch_group.cxx) |f| list.append(arena, f) catch @panic("OOM");
    for (tables.gcstress_group.cxx) |f| list.append(arena, f) catch @panic("OOM");
    for (tables.nodeapi_provider_tus) |f| list.append(arena, f) catch @panic("OOM");
    list.append(arena, tables.zig_codec_source) catch @panic("OOM");
    return list.toOwnedSlice(arena) catch @panic("OOM");
}

test "every Windows table source file exists on disk" {
    const io = std.testing.io;
    const cwd = std.Io.Dir.cwd();
    const arena = std.testing.allocator;
    var arena_state = std.heap.ArenaAllocator.init(arena);
    defer arena_state.deinit();
    const files = allTableFiles(arena_state.allocator());

    for (files) |path| {
        cwd.access(io, path, .{}) catch |err| {
            std.debug.print("missing file in tables_windows.zig: {s} ({any})\n", .{ path, err });
            return err;
        };
    }
    for (tables.core_asm_files) |path| {
        try cwd.access(io, path, .{});
    }
    for (tables.backend_asm_files) |path| {
        try cwd.access(io, path, .{});
    }
    for (tables.ch_static_extra_cxx) |path| {
        try cwd.access(io, path, .{});
    }
}

test "zero duplicate TUs and no forbidden cross-platform directories" {
    var arena_state = std.heap.ArenaAllocator.init(std.testing.allocator);
    defer arena_state.deinit();
    const arena = arena_state.allocator();

    var seen: std.StringHashMap(void) = .init(arena);
    const files = allTableFiles(arena);
    for (files) |path| {
        if (seen.contains(path)) {
            std.debug.print("duplicate TU in tables_windows.zig: {s}\n", .{path});
            return error.DuplicateTU;
        }
        try seen.put(path, {});

        try std.testing.expect(!std.mem.startsWith(u8, path, "pal/"));
        try std.testing.expect(!std.mem.startsWith(u8, path, "lib/JITClient/"));
        try std.testing.expect(!std.mem.startsWith(u8, path, "lib/JITIDL/"));
        try std.testing.expect(!std.mem.startsWith(u8, path, "lib/JITServer/"));
        try std.testing.expect(std.mem.indexOf(u8, path, "StackFrame.SystemV.cpp") == null);
    }

    // HeapAllocatorOperators.cpp must be first in common_memory_group for link order.
    try std.testing.expectEqualStrings(
        tables.heap_allocator_operators_tu,
        tables.common_memory_group.cxx[0],
    );
}

test "all Phase-4 TUs, u8a_codec, 7 MASM files, 16 NodeApi TUs, and bin targets present" {
    var arena_state = std.heap.ArenaAllocator.init(std.testing.allocator);
    defer arena_state.deinit();
    const files = allTableFiles(arena_state.allocator());

    const required = [_][]const u8{
        // Phase-4 Intl + ES2025/2026 TUs (SuppressedError lives in JavascriptError.cpp)
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
        "lib/Runtime/Library/CustomExternalIterator.cpp",
        // Windows-exact Wasm / wabt / PlatformAgnostic / Codec TUs
        "lib/WasmReader/WasmByteCodeGenerator.cpp",
        "lib/wabt/chakra/wabtapi.cc",
        "lib/wabt/src/opcode-code-table.c",
        "lib/Runtime/Language/amd64/StackFrame.cpp",
        "lib/Runtime/Language/AsmJsLink.cpp",
        "lib/Runtime/PlatformAgnostic/Platform/Windows/UnicodeText.cpp",
        "lib/Runtime/PlatformAgnostic/Platform/Common/UnicodeText.ICU.cpp",
        "lib/ZigCodec/u8a_codec.zig",
        // 5 core + 2 backend MASM .asm files
        "lib/Common/Memory/amd64/amd64_SAVE_REGISTERS.asm",
        "lib/Runtime/Language/amd64/JavascriptConversionA.asm",
        "lib/Runtime/Language/amd64/JavascriptOperatorsA.asm",
        "lib/Runtime/Language/amd64/amd64_Thunks.asm",
        "lib/Runtime/Library/amd64/JavascriptFunctionA.asm",
        "lib/Backend/amd64/LinearScanMdA.asm",
        "lib/Backend/amd64/Thunks.asm",
        // bin/ChakraCore, bin/ch, bin/GCStress
        "bin/ChakraCore/ChakraCoreDllFunc.cpp",
        "bin/ChakraCore/ConfigParserExternals.cpp",
        "bin/ChakraCore/TestHooks.cpp",
        "bin/ch/ch.cpp",
        "bin/ch/JITProcessManager.cpp",
        "bin/GCStress/GCStress.cpp",
        "bin/GCStress/StubExternalApi.cpp",
    };
    for (required) |req| try expectContains(files, req);

    try std.testing.expectEqual(@as(usize, 5), tables.core_asm_files.len);
    try std.testing.expectEqual(@as(usize, 2), tables.backend_asm_files.len);
    try std.testing.expectEqual(@as(usize, 16), tables.nodeapi_provider_tus.len);
    try std.testing.expectEqual(@as(usize, 9), tables.ch_group.cxx.len);
    try std.testing.expectEqual(@as(usize, 4), tables.gcstress_group.cxx.len);
    try std.testing.expectEqual(@as(usize, 3), tables.dll_group.cxx.len);
}

test "option parsers, profile prefixes, and gate predicates" {
    try std.testing.expectEqual(windows.Profile.@"test", windows.parseProfile("test").?);
    try std.testing.expectEqual(windows.Profile.debug_static, windows.parseProfile("debug-static").?);
    try std.testing.expectEqual(windows.Profile.release_shared, windows.parseProfile("release-shared").?);
    try std.testing.expect(windows.parseProfile("invalid") == null);

    try std.testing.expectEqual(windows.IcuMode.icu4c, windows.parseIcuMode("icu4c").?);
    try std.testing.expectEqual(windows.IcuMode.windows, windows.parseIcuMode("windows").?);
    try std.testing.expectEqual(windows.IcuMode.none, windows.parseIcuMode("none").?);
    try std.testing.expect(windows.parseIcuMode("system") == null);

    try std.testing.expect(windows.sideNameForConfig(.@"test") == null);
    try std.testing.expectEqualStrings("debug-static", windows.sideNameForConfig(.debug_static).?);
    try std.testing.expectEqualStrings("release-shared", windows.sideNameForConfig(.release_shared).?);

    var arena_state = std.heap.ArenaAllocator.init(std.testing.allocator);
    defer arena_state.deinit();
    const arena = arena_state.allocator();
    try std.testing.expectEqualStrings("zig-out", windows.installPrefixForConfig(arena, .@"test"));
    try std.testing.expectEqualStrings("out/debug-static", windows.installPrefixForConfig(arena, .debug_static));
    try std.testing.expectEqualStrings("out/release-shared", windows.installPrefixForConfig(arena, .release_shared));

    try std.testing.expectEqual(std.builtin.Optimize.fast, windows.expectedOptimize(.@"test"));
    try std.testing.expectEqual(std.builtin.Optimize.debug, windows.expectedOptimize(.debug_static));
    try std.testing.expectEqual(std.builtin.Optimize.fast, windows.expectedOptimize(.release_shared));

    try std.testing.expect(windows.supportsWasmGate(.@"test", true));
    try std.testing.expect(!windows.supportsWasmGate(.@"test", false));
    try std.testing.expect(windows.supportsWasmGate(.debug_static, true));
    try std.testing.expect(!windows.supportsWasmGate(.release_shared, true));

    try std.testing.expect(windows.supportsPackageGate(.@"test"));
    try std.testing.expect(!windows.supportsPackageGate(.debug_static));
    try std.testing.expect(windows.supportsPackageGate(.release_shared));

    try std.testing.expect(!windows.shouldCompileGroup(&tables.backend_group, .@"test", false, .icu4c));
    try std.testing.expect(windows.shouldCompileGroup(&tables.backend_group, .@"test", true, .icu4c));
    try std.testing.expect(!windows.shouldCompileGroup(&tables.wabt_group, .release_shared, true, .icu4c));
    try std.testing.expect(windows.shouldCompileGroup(&tables.wabt_group, .debug_static, true, .icu4c));
    try std.testing.expect(!windows.shouldCompileGroup(&tables.rt_platformagnostic_icu_group, .@"test", true, .none));
    try std.testing.expect(windows.shouldCompileGroup(&tables.rt_platformagnostic_icu_group, .@"test", true, .windows));
}

test "compile define and Clang-MSVC flag builders" {
    var arena_state = std.heap.ArenaAllocator.init(std.testing.allocator);
    defer arena_state.deinit();
    const arena = arena_state.allocator();

    const defs_test = windows.buildCompileDefines(arena, .@"test", true, .icu4c, true, null);
    try expectContains(defs_test, "-DENABLE_OOP_NATIVE_CODEGEN=0");
    try expectContains(defs_test, "-DDISABLE_JS_ETW");
    try expectContains(defs_test, "-DHAS_ICU=1");
    try expectContains(defs_test, "-DINTL_ICU=1");
    try expectContains(defs_test, "-DU_DISABLE_RENAMING=1");
    try expectContains(defs_test, "-DCAN_BUILD_WABT=1");
    try expectContains(defs_test, "-DENABLE_DEBUG_CONFIG_OPTIONS=1");
    try expectNotContains(defs_test, "-DDISABLE_JIT=1");
    try expectNotContains(defs_test, "-DCHAKRA_STATIC_LIBRARY=1");

    const defs_win_nojit = windows.buildCompileDefines(arena, .debug_static, false, .windows, true, "-DINC_OLE2");
    try expectContains(defs_win_nojit, "-DDISABLE_JIT=1");
    try expectContains(defs_win_nojit, "-DCHAKRA_STATIC_LIBRARY=1");
    try expectContains(defs_win_nojit, "-D_DEBUG");
    try expectContains(defs_win_nojit, "-DDBG=1");
    try expectContains(defs_win_nojit, "-DWINDOWS10_ICU=1");
    try expectContains(defs_win_nojit, "-DINC_OLE2");

    const defs_no_icu = windows.buildCompileDefines(arena, .release_shared, true, .none, false, null);
    try expectNotContains(defs_no_icu, "-DHAS_ICU=1");
    try expectNotContains(defs_no_icu, "-DINTL_ICU=1");
    try expectNotContains(defs_no_icu, "-DWINDOWS10_ICU=1");
    try expectNotContains(defs_no_icu, "-DCAN_BUILD_WABT=1");
    try expectNotContains(defs_no_icu, "-DENABLE_DEBUG_CONFIG_OPTIONS=1");
    try expectContains(defs_no_icu, "-DNDEBUG");

    const cxx_flags = windows.buildCompilerFlags(arena, .@"test", .cxx, false, true);
    try expectContains(cxx_flags, "-fms-runtime-lib=static");
    try expectContains(cxx_flags, "-O2");
    try expectContains(cxx_flags, "-Werror");
    try expectContains(cxx_flags, "-Wno-date-time");
    try expectContains(cxx_flags, "-msse4.2");
    try expectContains(cxx_flags, "-fdelayed-template-parsing");

    const dbg_flags = windows.buildCompilerFlags(arena, .debug_static, .cxx, true, false);
    try expectContains(dbg_flags, "-fms-runtime-lib=static_dbg");
    try expectContains(dbg_flags, "-O0");
    try expectContains(dbg_flags, "-w");
    try expectNotContains(dbg_flags, "-Werror");

    const c_flags = windows.buildCompilerFlags(arena, .@"test", .c, true, false);
    try expectContains(c_flags, "-std=c11");
    try expectNotContains(c_flags, "-std=c++17");
}

test "MASM ml64 flag construction and env auto-discovery candidates" {
    var arena_state = std.heap.ArenaAllocator.init(std.testing.allocator);
    defer arena_state.deinit();
    const arena = arena_state.allocator();

    const thunks_jit = windows.buildMasmArgs(arena, "C:/WinSDK/10/um", "lib/Runtime/Language/amd64/amd64_Thunks.asm", true);
    try expectContains(thunks_jit, "/nologo");
    try expectContains(thunks_jit, "/c");
    try expectContains(thunks_jit, "/Cx");
    try expectContains(thunks_jit, "/IC:/WinSDK/10/um");
    try expectContains(thunks_jit, "-D_ENABLE_DYNAMIC_THUNKS=1");

    const jfn_jit = windows.buildMasmArgs(arena, "C:/WinSDK/10/um", "lib/Runtime/Library/amd64/JavascriptFunctionA.asm", true);
    try expectContains(jfn_jit, "-D_ENABLE_DYNAMIC_THUNKS=1");

    const thunks_nojit = windows.buildMasmArgs(arena, "C:/WinSDK/10/um", "lib/Runtime/Language/amd64/amd64_Thunks.asm", false);
    try expectNotContains(thunks_nojit, "-D_ENABLE_DYNAMIC_THUNKS=1");

    const save_regs = windows.buildMasmArgs(arena, "C:/WinSDK/10/um", "lib/Common/Memory/amd64/amd64_SAVE_REGISTERS.asm", true);
    try expectNotContains(save_regs, "-D_ENABLE_DYNAMIC_THUNKS=1");

    const icu_cands = windows.icuAutoCandidates(arena, "C:\\icu\\", "C:\\vcpkg");
    try std.testing.expectEqual(@as(usize, 2), icu_cands.len);
    try std.testing.expectEqualStrings("C:\\icu/include", icu_cands[0].include.?);
    try std.testing.expectEqualStrings("C:\\icu/lib", icu_cands[0].libdir.?);
    try std.testing.expectEqualStrings("C:\\vcpkg/installed/x64-windows/include", icu_cands[1].include.?);

    const sdk_cands = windows.winSdkUmEnvCandidates(arena, null, "C:\\Program Files (x86)\\Windows Kits\\10\\", "10.0.22621.0\\", null, null);
    try std.testing.expectEqual(@as(usize, 1), sdk_cands.len);
    try std.testing.expectEqualStrings("C:\\Program Files (x86)\\Windows Kits\\10/Include/10.0.22621.0/um", sdk_cands[0]);

    const ml_cands = windows.ml64EnvCandidates(arena, null, "C:\\VS\\VC\\Tools\\MSVC\\14.40\\");
    try std.testing.expectEqual(@as(usize, 1), ml_cands.len);
    try std.testing.expectEqualStrings("C:\\VS\\VC\\Tools\\MSVC\\14.40/bin/Hostx64/x64/ml64.exe", ml_cands[0]);
}

test "wabt config.h template renders valid 64-bit MSVC config" {
    var arena_state = std.heap.ArenaAllocator.init(std.testing.allocator);
    defer arena_state.deinit();
    const arena = arena_state.allocator();

    const file = try std.Io.Dir.cwd().openFile(std.testing.io, "lib/wabt/src/config.h.in", .{});
    defer file.close(std.testing.io);
    var r = file.readerStreaming(std.testing.io, &.{});
    const tmpl = try r.interface.allocRemaining(arena, .limited(1 << 20));
    const rendered = windows.renderWabtConfig(arena, tmpl);

    try std.testing.expect(std.mem.indexOf(u8, rendered, "#define COMPILER_IS_MSVC 1") != null);
    try std.testing.expect(std.mem.indexOf(u8, rendered, "#define COMPILER_IS_CLANG 0") != null);
    try std.testing.expect(std.mem.indexOf(u8, rendered, "#define SIZEOF_SIZE_T 8") != null);
    try std.testing.expect(std.mem.indexOf(u8, rendered, "#define HAVE_SNPRINTF 1") != null);
    try std.testing.expect(std.mem.indexOf(u8, rendered, "#define HAVE_STRCASECMP 0") != null);
    try std.testing.expect(std.mem.indexOf(u8, rendered, "#cmakedefine01") == null);
    try std.testing.expect(std.mem.indexOf(u8, rendered, "@SIZEOF_SIZE_T@") == null);
}

test "windows.build entry point compiles and typechecks completely" {
    try std.testing.expect(@TypeOf(windows.build) == fn (*std.Build) void);
    comptime {
        _ = &windows.build;
    }
}

test "libChakraNodeApiStub links ChakraCore.lib without full 161-export .def" {
    const win_src = @embedFile("build/windows.zig");
    const stub_idx = std.mem.indexOf(u8, win_src, "nodeapi_stub.addFileArg(b.path(\"tools/nodeapi/stub/stub_provider.cpp\"));") orelse return error.MissingStubLink;
    const out_idx = std.mem.indexOfPos(u8, win_src, stub_idx, "libChakraNodeApiStub.dll") orelse return error.MissingStubOutput;
    const stub_block = win_src[stub_idx..out_idx];
    try std.testing.expect(std.mem.indexOf(u8, stub_block, "dr.implib") != null);
    try std.testing.expect(std.mem.indexOf(u8, stub_block, "nodeapi_def") == null);

    const stub_src = @embedFile("nodeapi/stub/stub_provider.cpp");
    try std.testing.expect(std.mem.indexOf(u8, stub_src, "STUB_HOST_EXPORT") != null);
    try std.testing.expect(std.mem.indexOf(u8, stub_src, "__declspec(dllexport)") != null);
}

fn checkGroupExtensions(g: *const tables.Group) !void {
    for (g.cxx) |f| {
        if (std.mem.eql(u8, f, "lib/Parser/jserr.gen")) return error.UnexpectedJserrGen;
        const ok = std.mem.endsWith(u8, f, ".cpp") or std.mem.endsWith(u8, f, ".cc");
        if (!ok) {
            std.debug.print("invalid cxx extension in Windows table: {s}\n", .{f});
            return error.InvalidCxxExtension;
        }
    }
    for (g.c) |f| {
        if (std.mem.eql(u8, f, "lib/Parser/jserr.gen")) return error.UnexpectedJserrGen;
        if (!std.mem.endsWith(u8, f, ".c")) {
            std.debug.print("invalid c extension in Windows table: {s}\n", .{f});
            return error.InvalidCExtension;
        }
    }
    for (g.asm_files) |f| {
        if (std.mem.eql(u8, f, "lib/Parser/jserr.gen")) return error.UnexpectedJserrGen;
        if (!std.mem.endsWith(u8, f, ".asm")) {
            std.debug.print("invalid asm extension in Windows table: {s}\n", .{f});
            return error.InvalidAsmExtension;
        }
    }
}

test "strict extensions, no jserr.gen in groups, and exact non-overlapping TU counts" {
    var arena_state = std.heap.ArenaAllocator.init(std.testing.allocator);
    defer arena_state.deinit();
    const files = allTableFiles(arena_state.allocator());
    try expectNotContains(files, "lib/Parser/jserr.gen");

    var engine_cxx: usize = 0;
    var engine_c: usize = 0;
    var engine_asm: usize = 0;
    for (tables.engine_groups) |g| {
        try checkGroupExtensions(g);
        engine_cxx += g.cxx.len;
        engine_c += g.c.len;
        engine_asm += g.asm_files.len;
    }
    try checkGroupExtensions(&tables.dll_group);
    try checkGroupExtensions(&tables.ch_group);
    try checkGroupExtensions(&tables.gcstress_group);
    try std.testing.expectEqual(@as(usize, 0), tables.dll_group.c.len + tables.dll_group.asm_files.len);
    try std.testing.expectEqual(@as(usize, 0), tables.ch_group.c.len + tables.ch_group.asm_files.len);
    try std.testing.expectEqual(@as(usize, 0), tables.gcstress_group.c.len + tables.gcstress_group.asm_files.len);

    try std.testing.expectEqual(@as(usize, 582), engine_cxx);
    try std.testing.expectEqual(@as(usize, 1), engine_c);
    try std.testing.expectEqual(@as(usize, 7), engine_asm);
    try std.testing.expectEqual(@as(usize, 3), tables.dll_group.cxx.len);
    try std.testing.expectEqual(@as(usize, 9), tables.ch_group.cxx.len);
    try std.testing.expectEqual(@as(usize, 4), tables.gcstress_group.cxx.len);
    try std.testing.expectEqual(@as(usize, 16), tables.nodeapi_provider_tus.len);

    const engine_bin_total = engine_cxx + engine_c + engine_asm +
        tables.dll_group.cxx.len + tables.ch_group.cxx.len + tables.gcstress_group.cxx.len;
    try std.testing.expectEqual(@as(usize, 606), engine_bin_total);
    try std.testing.expectEqual(@as(usize, 606 + 1 + 16), files.len);

    const win_src = @embedFile("build/windows.zig");
    try std.testing.expect(std.mem.indexOf(u8, win_src, "ch_link.addFileArg(common_codex_lib);") != null);
    try std.testing.expect(std.mem.indexOf(u8, win_src, "tools/build/check_regression_windows.py") != null);
    try std.testing.expect(std.mem.indexOf(u8, win_src, "allow-unpinned-regression") != null);
    try std.testing.expect(std.mem.indexOf(u8, win_src, "\"check_regression_windows.py\"") != null);
}
