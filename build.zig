//-------------------------------------------------------------------------------------------------------
// Copyright (c) ChakraCore Project Contributors. All rights reserved.
// Licensed under the MIT license. See LICENSE.txt file in the project root for full license information.
//-------------------------------------------------------------------------------------------------------
//! B1 step 3: first build.zig slice — build-owned pal/src/config.h plus the PAL object group.
//!
//! Exactly ONE profile: native ARM64 macOS, shared/no-JIT, RelWithDebInfo Test, system ICU,
//! macOS minimum 26.0, MacOSX SDK 26.5, Zig 0.17.0. Anything else fails before doing work.
//! Full-engine wiring is B2; profiles/incremental/CI are B3.
//!
//! Contracts: doc/build-contract-mac.md (sources/flags/includes), doc/pal-config-contract-mac.md
//! (config.h checks), doc/build.md sections 13-14 (pilot findings), doc/modernization-plan.md
//! section B1/2.3 (Zig API facts).

const std = @import("std");
const builtin = @import("builtin");
const pal_config = @import("tools/pal-config/check.zig");

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

// -D flags for every PAL unit, in alphabetical order (reference command line,
// e.g. pal/src/misc/time.cpp in build-zigpilot/compile_commands.json;
// doc/build-contract-mac.md section 1). Each -D appears exactly once;
// unvalued/valued pairs (FEATURE_PAL, PLATFORM_UNIX, _ARM64_) keep both
// spellings with the unvalued first, as on the reference line.
const pal_defines: []const []const u8 = &.{
    "-DBIT64=1",
    "-DCLANG_HAS_DISABLE_TAIL_CALLS=1", // from our attribute check (check.zig enforces pass)
    "-DDISABLE_JIT=1",
    "-DENABLE_DEBUG_CONFIG_OPTIONS=1",
    "-DFEATURE_PAL",
    "-DFEATURE_PAL=1",
    "-DHAS_ICU",
    "-DHAS_REAL_ICU=1",
    "-DINTL_ICU=1",
    "-DLP64COMPATIBLE=1",
    "-DPIC=1",
    "-DPLATFORM_UNIX",
    "-DPLATFORM_UNIX=1",
    "-DSTACK_ALIGN=16",
    "-DTARGET_64",
    "-DUNICODE",
    "-D_ARM64_",
    "-D_ARM64_=1",
    "-D_CHAKRACOREBUILD",
    "-D_FILE_OFFSET_BITS=64",
    "-D_M_ARM32_OR_ARM64",
    "-D_M_ARM64",
    "-D_SAFECRT_USE_CPP_OVERLOADS=1",
    "-D_WIN64=1",
    "-D__STDC_WANT_LIB_EXT1__=1",
    "-D__aarch64__",
    "-D__arm64__=1",
};

// 25 -Wno-* suppressions, in observed order (root CMakeLists.txt:410-436).
const pal_warnings: []const []const u8 = &.{
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

// -I dirs after the generated-config dir, in observed order (section 3).
// Root-relative; the C step runs with cwd at the build root.
const pal_includes_rel: []const []const u8 = &.{
    "pal/src",
    ".",
    "lib/Common",
    "lib/Common/PlaceHolder",
    "pal",
    "pal/inc",
    "pal/inc/rt",
};

const pal_cxx_files: []const []const u8 = &.{
    "pal/src/exception/machexception.cpp",
    "pal/src/exception/machmessage.cpp",
    "pal/src/cruntime/file.cpp",
    "pal/src/cruntime/filecrt.cpp",
    "pal/src/cruntime/finite.cpp",
    "pal/src/cruntime/lstr.cpp",
    "pal/src/cruntime/malloc.cpp",
    "pal/src/cruntime/mbstring.cpp",
    "pal/src/cruntime/misc.cpp",
    "pal/src/cruntime/misctls.cpp",
    "pal/src/cruntime/path.cpp",
    "pal/src/cruntime/printf.cpp",
    "pal/src/cruntime/printfcpp.cpp",
    "pal/src/cruntime/silent_printf.cpp",
    "pal/src/cruntime/string.cpp",
    "pal/src/cruntime/stringtls.cpp",
    "pal/src/cruntime/thread.cpp",
    "pal/src/cruntime/wchar.cpp",
    "pal/src/cruntime/wchartls.cpp",
    "pal/src/cruntime/runtime_proxy.cpp",
    "pal/src/debug/debug.cpp",
    "pal/src/exception/seh.cpp",
    "pal/src/exception/signal.cpp",
    "pal/src/file/directory.cpp",
    "pal/src/file/pal_file.cpp",
    "pal/src/file/filetime.cpp",
    "pal/src/file/pal_path.cpp",
    "pal/src/file/shmfilelockmgr.cpp",
    "pal/src/handlemgr/handleapi.cpp",
    "pal/src/handlemgr/handlemgr.cpp",
    "pal/src/init/pal.cpp",
    "pal/src/init/sxs.cpp",
    "pal/src/loader/module.cpp",
    "pal/src/loader/modulename.cpp",
    "pal/src/locale/unicode.cpp",
    "pal/src/locale/unicode_data.cpp",
    "pal/src/locale/utf8.cpp",
    "pal/src/map/common.cpp",
    "pal/src/map/map.cpp",
    "pal/src/map/virtual.cpp",
    "pal/src/memory/heap.cpp",
    "pal/src/memory/local.cpp",
    "pal/src/misc/bstr.cpp",
    "pal/src/misc/dbgmsg.cpp",
    "pal/src/misc/error.cpp",
    "pal/src/misc/environ.cpp",
    "pal/src/misc/random.cpp",
    "pal/src/misc/strutil.cpp",
    "pal/src/misc/time.cpp",
    "pal/src/misc/sysinfo.cpp",
    "pal/src/misc/utils.cpp",
    "pal/src/objmgr/palobjbase.cpp",
    "pal/src/objmgr/shmobject.cpp",
    "pal/src/objmgr/shmobjectmanager.cpp",
    "pal/src/shmemory/shmemory.cpp",
    "pal/src/synchobj/event.cpp",
    "pal/src/synchobj/mutex.cpp",
    "pal/src/synchobj/semaphore.cpp",
    "pal/src/synchmgr/synchcontrollers.cpp",
    "pal/src/synchmgr/synchmanager.cpp",
    "pal/src/synchmgr/wait.cpp",
    "pal/src/sync/cs.cpp",
    "pal/src/sync/cclock.cpp",
    "pal/src/thread/context.cpp",
    "pal/src/thread/process.cpp",
    "pal/src/thread/pal_thread.cpp",
    "pal/src/thread/threadsusp.cpp",
};

const pal_c_files: []const []const u8 = &.{
    "pal/src/safecrt/makepath_s.c",
    "pal/src/safecrt/mbusafecrt.c",
    "pal/src/safecrt/safecrt_input_s.c",
    "pal/src/safecrt/safecrt_output_s.c",
    "pal/src/safecrt/safecrt_winput_s.c",
    "pal/src/safecrt/safecrt_woutput_s.c",
    "pal/src/safecrt/memcpy_s.c",
    "pal/src/safecrt/memmove_s.c",
    "pal/src/safecrt/sprintf.c",
    "pal/src/safecrt/sscanf.c",
    "pal/src/safecrt/strcat_s.c",
    "pal/src/safecrt/strcpy_s.c",
    "pal/src/safecrt/strncat_s.c",
    "pal/src/safecrt/strncpy_s.c",
    "pal/src/safecrt/swprintf.c",
    "pal/src/safecrt/vsprintf.c",
    "pal/src/safecrt/vswprint.c",
    "pal/src/safecrt/wcscpy_s.c",
    "pal/src/safecrt/wcsncpy_s.c",
    "pal/src/safecrt/xtoa_s.c",
    "pal/src/safecrt/xtow_s.c",
};

const pal_asm_files: []const []const u8 = &.{
    "pal/src/arch/arm64/dispatchexceptionwrapper.S",
    "pal/src/arch/arm64/context2.S",
    "pal/src/arch/arm64/debugbreak.S",
};

pub fn build(b: *std.Build) void {
    gateToolchain();
    const target = gateTarget(b);
    const optimize = gateOptimize(b);
    const sdk_path = gateSdk(b);
    const icu_include = gateIcu(b);

    std.debug.print("b1 profile: target={s} sdk={s}@{s} icu-include={s} zig={f}\n", .{
        profile_target, sdk_path, profile_sdk_version, icu_include, builtin.zig_version,
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

    const mod = b.createModule(.{
        .target = target,
        .optimize = optimize,
        .link_libc = true,
        .link_libcpp = true,
    });
    // Generated-config dir FIRST (cf. CMAKE_INCLUDE_CURRENT_DIR).
    mod.addIncludePath(wf.getDirectory());

    // -I flags in observed order (config dir already added above).
    var iflags: std.ArrayList([]const u8) = .empty;
    for (pal_includes_rel) |dir| {
        iflags.append(arena, b.fmt("-I{s}", .{dir})) catch @panic("OOM");
    }
    iflags.append(arena, b.fmt("-I{s}", .{icu_include})) catch @panic("OOM");
    iflags.append(arena, b.fmt("-I{s}/../i18n", .{icu_include})) catch @panic("OOM");
    iflags.append(arena, "-Ipal/src/include") catch @panic("OOM");
    iflags.append(arena, "-isystem") catch @panic("OOM");
    iflags.append(arena, "/usr/local/include") catch @panic("OOM");
    const sysroot_flag = b.fmt("--sysroot={s}", .{sdk_path});
    const sdk_isystem = b.fmt("{s}/usr/include", .{sdk_path});
    const sdk_frameworks = b.fmt("{s}/System/Library/Frameworks", .{sdk_path});

    // C++ flags: observed contract set, minus target-owned/dead/cosmetic entries
    // (-mmacosx-version-min, -arch, -std=gnu++11, -Werror/-Wno-error,
    // -fdiagnostics-color), plus build-required additions (see below).
    var cxx: std.ArrayList([]const u8) = .empty;
    cxx.appendSlice(arena, pal_defines) catch @panic("OOM");
    cxx.appendSlice(arena, iflags.items) catch @panic("OOM");
    cxx.appendSlice(arena, pal_warnings) catch @panic("OOM");
    cxx.appendSlice(arena, &.{
        "-fno-omit-frame-pointer", "-fdelayed-template-parsing", "-O3", "-g",
        "-std=gnu++17",           "-fasm-blocks",                 "-fms-extensions",
        "-fwrapv",                 "-fstack-protector",            "-fno-builtin",
        "-fPIC",
        // Build-required additions: explicit sysroot/SDK-framework paths (doc/build.md
        // section 14; -F must be the absolute SDK path since sysroot-relative -F does
        // not resolve on the build path), -UNDEBUG to counter Zig's ReleaseFast
        // -DNDEBUG injection (reference C++ has no NDEBUG, contract quirk Q4), and
        // -Wno-date-time (zig clang 22 promotes __DATE__/__TIME__; pilot finding).
        sysroot_flag,            "-isystem", sdk_isystem, "-F", sdk_frameworks,
        "-UNDEBUG",               "-Wno-date-time",
    }) catch @panic("OOM");

    // C flags (safecrt): -O2 -g -DNDEBUG, no warning set (section 2.2).
    var cc: std.ArrayList([]const u8) = .empty;
    cc.appendSlice(arena, pal_defines) catch @panic("OOM");
    cc.appendSlice(arena, iflags.items) catch @panic("OOM");
    cc.appendSlice(arena, &.{
        "-std=gnu99",      "-O2",           "-g",           "-DNDEBUG",
        "-fasm-blocks",    "-fms-extensions", "-fwrapv",    "-fstack-protector",
        "-fno-builtin",    "-fPIC",         sysroot_flag,  "-isystem",
        sdk_isystem,       "-F",            sdk_frameworks, "-Wno-date-time",
    }) catch @panic("OOM");

    // Assembly flags (section 2.3): -isysroot replaced by uniform --sysroot.
    var asm_flags: std.ArrayList([]const u8) = .empty;
    asm_flags.appendSlice(arena, pal_defines) catch @panic("OOM");
    asm_flags.appendSlice(arena, iflags.items) catch @panic("OOM");
    asm_flags.appendSlice(arena, &.{
        "-O2",             "-g",            "-DNDEBUG",    "-fasm-blocks",
        "-fms-extensions", "-fwrapv",       "-fstack-protector", "-fno-builtin",
        "-fPIC",           sysroot_flag,    "-isystem",    sdk_isystem,
        "-F",              sdk_frameworks,  "-Wno-date-time",
    }) catch @panic("OOM");

    mod.addCSourceFiles(.{ .files = pal_cxx_files, .flags = cxx.items });
    mod.addCSourceFiles(.{ .files = pal_c_files, .flags = cc.items });
    mod.addCSourceFiles(.{ .files = pal_asm_files, .flags = asm_flags.items });

    const pal = b.addLibrary(.{ .name = "Chakra.Pal", .root_module = mod });
    b.installArtifact(pal);
    b.getInstallStep().dependOn(&b.addInstallFile(config_h, "pal/src/config.h").step);
}
