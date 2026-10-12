# Mac build contract (reference profile)

B1 step 1 — the complete build contract for exactly ONE profile, extracted from
the CMake sources. Source of truth: the `CMakeLists.txt` / `.cmake` files at
baseline `97e68c917` on branch `modernize/baseline`, cross-checked against the
`build-zigpilot/compile_commands.json` evidence (538 units) and
`build-zigpilot/build.ninja` link edges.

Reference profile (from [build.md](build.md) §13-14):

- Target: native ARM64 macOS (`CMAKE_SYSTEM_PROCESSOR=arm64`,
  `CMAKE_SYSTEM_NAME=Darwin`).
- Configure (direct CMake, no `build.sh`): `-DCMAKE_BUILD_TYPE=RelWithDebInfo
  -DDISABLE_JIT=ON -DICU_INCLUDE_PATH=/opt/homebrew/opt/icu4c/include
  -DCMAKE_CXX_COMPILER=clang++ -DCMAKE_C_COMPILER=clang` with `-GNinja`.
  No `STATIC_LIBRARY`, no `NO_ICU`/`EMBED_ICU`, no LTTng, no sanitizer, no
  write-barrier plugin, no ccache. Observed cache values in
  `build-zigpilot/CMakeCache.txt`: `DISABLE_JIT:BOOL=ON`,
  `CMAKE_BUILD_TYPE=RelWithDebInfo`, compilers `/usr/bin/clang{,++}`,
  `ICU_INCLUDE_PATH=/opt/homebrew/opt/icu4c/include`.
- Resolved feature vars: `CC_TARGETS_ARM64=1` (`CMakeLists.txt:55-64,74-78`),
  `CC_TARGET_OS_OSX=1` (`CMakeLists.txt:137-139`), `BuildJIT=0`
  (`CMakeLists.txt:522-523`), `IS_64BIT_BUILD=1` (`CMakeLists.txt:387-388`),
  `CAN_BUILD_WABT` unset (AMD64-only, `CMakeLists.txt:282-284`),
  `PAL_CMAKE_PLATFORM_ARCH_ARM64=1` (`pal/src/CMakeLists.txt:22-24`).

Conventions: every value cites `file:line`. `build-zigpilot/` paths are
validation evidence, not contract inputs. Items that could not be pinned to a
source line are marked **UNCERTAIN** with the reason. Nothing here is invented:
machine-specific values (SDK path, Homebrew prefix) are recorded as observed.

## 1. Global preprocessor defines

Defines on every compile unit of this profile, in observed command-line order
(sample: `lib/Runtime/Library/JavascriptArray.cpp` in `compile_commands.json`):

| Define | Value | Source |
|---|---|---|
| `BIT64` | `1` | `CMakeLists.txt:513` |
| `CLANG_HAS_DISABLE_TAIL_CALLS` | `1` | `Build/CMakeFeatureDetect.cmake:29-30`; probe `:17-27` passed on AppleClang, so `CXX_DO_NOT_OPTIMIZE_SIBLING_CALLS` is empty (`:31`) |
| `DISABLE_JIT` | `1` | `CMakeLists.txt:524` |
| `ENABLE_DEBUG_CONFIG_OPTIONS` | `1` | `CMakeLists.txt:507` (RelWithDebInfo branch) |
| `FEATURE_PAL` | (none) | `CMakeLists.txt:374` |
| `HAS_ICU` | (none) | `CMakeLists.txt:225` (`ICU_INCLUDE_PATH` branch) |
| `HAS_REAL_ICU` | `1` | `CMakeLists.txt:224` |
| `INTL_ICU` | `1` | `CMakeLists.txt:230` (`INTL_ICU` defaults ON, `:13`) |
| `PLATFORM_UNIX` | (none) | `CMakeLists.txt:340` |
| `PLATFORM_UNIX` | `1` | `CMakeLists.txt:375` (second, valued definition wins) |
| `STACK_ALIGN` | `16` | `CMakeLists.txt:514` |
| `TARGET_64` | (none) | `CMakeLists.txt:306` |
| `UNICODE` | (none) | `CMakeLists.txt:393` |
| `_ARM64_` | (none) | `CMakeLists.txt:389` |
| `_ARM64_` | `1` | `CMakeLists.txt:75` |
| `_CHAKRACOREBUILD` | (none) | `CMakeLists.txt:373` |
| `_M_ARM32_OR_ARM64` | (none) | `CMakeLists.txt:307` |
| `_M_ARM64` | (none) | `CMakeLists.txt:389` |
| `_SAFECRT_USE_CPP_OVERLOADS` | `1` | `CMakeLists.txt:394` |
| `__STDC_WANT_LIB_EXT1__` | `1` | `CMakeLists.txt:395` |
| `__aarch64__` | (none) | `CMakeLists.txt:305` |
| `__arm64__` | `1` | `CMakeLists.txt:76` |

Plus `-fdiagnostics-color=always`, passed as a flag via `add_definitions`
(`CMakeLists.txt:361-366`, AppleClang branch).

Defines on every unit compiled at/after `lib/` and `bin/` but NOT on PAL units
(added at `CMakeLists.txt:618-621`, after `add_subdirectory(pal)` at `:615`):

| Define | Value | Source |
|---|---|---|
| `NO_PAL_MINMAX` | (none) | `CMakeLists.txt:619` |
| `PAL_STDCPP_COMPAT` | (none) | `CMakeLists.txt:620` |

PAL-only defines (`pal/src/CMakeLists.txt`):

| Define | Value | Source |
|---|---|---|
| `_ARM64_` | (none) | `pal/src/CMakeLists.txt:24` (duplicate of global, harmless) |
| `PLATFORM_UNIX` | `1` | `pal/src/CMakeLists.txt:33` |
| `LP64COMPATIBLE` | `1` | `pal/src/CMakeLists.txt:34` |
| `FEATURE_PAL` | `1` | `pal/src/CMakeLists.txt:35` (valued; follows the global unvalued one, so it wins) |
| `PIC` | `1` | `pal/src/CMakeLists.txt:36` |
| `_FILE_OFFSET_BITS` | `64` | `pal/src/CMakeLists.txt:37` |
| `BIT64` | `1` | `pal/src/CMakeLists.txt:44` (duplicate of global) |
| `_WIN64` | `1` | `pal/src/CMakeLists.txt:45` |

Per-target defines added automatically by CMake (observed in
`compile_commands.json`; `CMAKE_ENABLE_EXPORTS` is ON at
`CMakeLists.txt:407`): `-Dch_EXPORTS` on `ch` units, `-DGCStress_EXPORTS` on
GCStress units, `-DChakraCore_EXPORTS` on dylib units. No extra link flag for
exports appears on macOS (observed `LINK_FLAGS` in §5).

NOT defined in this profile: `CAN_BUILD_WABT` (`CMakeLists.txt:282-284`,
AMD64-only), `_ENABLE_DYNAMIC_THUNKS`
(`lib/Runtime/Language/CMakeLists.txt:1-3`,
`lib/Runtime/Library/CMakeLists.txt:1-3`; `BuildJIT` is false),
`CHAKRA_STATIC_LIBRARY` (`CMakeLists.txt:368-370`),
`DBG`/`DEBUG`/`_DEBUG`/`DBG_DUMP` (`CMakeLists.txt:498-504`, Debug-only),
`NDEBUG` on C++ units (see §7 quirk Q4 for C/ASM).

## 2. Global compiler flags

### 2.1 C++ (`clang++`, 507 units)

In observed order (duplicates are real — both occurrences are on the command
line):

1. `-mmacosx-version-min=10.9 -std=gnu++11` — `CMakeLists.txt:347-351`
   (`MACOSX_DEPLOYMENT_TARGET` unset in the reference env).
2. 25 `-Wno-*` suppressions — `CMakeLists.txt:410-436`:
   `-Wno-ignored-attributes -Wno-parentheses-equality -Wno-reorder
   -Wno-microsoft -Wno-unused-value -Wno-int-to-void-pointer-cast
   -Wno-invalid-offsetof -Wno-undefined-inline
   -Wno-inconsistent-missing-override -Wno-macro-redefined -Wno-pragmas
   -Wno-invalid-token-paste -Wno-format -Wno-invalid-noreturn
   -Wno-null-arithmetic -Wno-tautological-constant-out-of-range-compare
   -Wno-tautological-undefined-compare -Wno-address-of-temporary
   -Wno-null-conversion -Wno-return-type -Wno-switch -Wno-int-to-pointer-cast
   -Wno-tautological-constant-compare -Wno-enum-compare-switch
   -Wno-unknown-warning-option`.
   (No `-Werror`: that is Linux-only, `CMakeLists.txt:327-337`.)
3. `-Werror -Wno-error` — `Build/CMakeFeatureDetect.cmake:12-14,39-41`
   (temporarily enabled for the OSX probe, then disabled; net effect off).
4. `-fno-omit-frame-pointer -fdelayed-template-parsing` —
   `CMakeLists.txt:453-457` (`CXX_DO_NOT_OPTIMIZE_SIBLING_CALLS` is empty
   because the probe passed).
5. `-O3 -g` — `CMakeLists.txt:4` (`CMAKE_CXX_FLAGS_RELWITHDEBINFO` override).
6. `-std=gnu++17` — `CMakeLists.txt:398-399` (`CMAKE_CXX_STANDARD 17`,
   required; wins over the earlier `-std=gnu++11`).
7. `-arch arm64` — `CMakeLists.txt:309`.
8. Per-target codegen: `-fPIE` on `ch` objects (plus `-fPIE` at link,
   `bin/ch/CMakeLists.txt:29-32,93`); `-fPIC` on all `lib/` objects
   (`lib/CMakeLists.txt:1`) and on the shared-lib objects (automatic for
   `SHARED`); neither on GCStress objects (observed).
9. Global `add_compile_options` — `-arch arm64` (dup of 7; only repo
   source is `CMakeLists.txt:309`), `-mmacosx-version-min=10.9` (dup of 1;
   `add_compile_options` source `CMakeLists.txt:347` plus the
   `CMAKE_CXX_FLAGS` copy at `:350-351`), `-fasm-blocks -fms-extensions
   -fwrapv` (`CMakeLists.txt:460-464`). The `-arch arm64` duplication
   mechanism is UNCERTAIN — one source line, two occurrences (see §8 U2).
10. PAL directory additions (PAL C++ units only): `-fstack-protector`
    (`pal/CMakeLists.txt:8`), `-fno-builtin -fPIC`
    (`pal/src/CMakeLists.txt:51-52`).
11. No `-fno-rtti` (static Release only, `CMakeLists.txt:469-473`).

### 2.2 C (`clang`, 21 units — all `pal/src/safecrt/*.c`)

`-mmacosx-version-min=10.9 -std=gnu99` (`CMakeLists.txt:348-349`),
`-O2 -g -DNDEBUG` (CMake builtin default `CMAKE_C_FLAGS_RELWITHDEBINFO`; the
repo overrides only the C++ variant), `-arch arm64`, then the same global
`add_compile_options` as §2.1 item 9 plus PAL `-fstack-protector -fno-builtin
-fPIC`. No `-Wno-*` set (C++-only), no `-fdelayed-template-parsing`, no
`-fno-omit-frame-pointer`. Full sample in §6.

### 2.3 Assembly (`clang`, 10 `.S` units)

`-O2 -g -DNDEBUG -arch arm64` (CMake builtin default config flags; the repo
sets no `CMAKE_ASM_FLAGS` for ARM64/OSX — only x86 at `CMakeLists.txt:291-293`
and ARM64-Linux comment-stripping at `:311-318`), plus global
`add_compile_options` (item 9) plus `-fPIC` (`lib` units) or PAL
`-fstack-protector -fno-builtin -fPIC`. Additionally every ASM command carries
`-isysroot /Applications/Xcode.app/Contents/Developer/Platforms/MacOSX.platform/Developer/SDKs/MacOSX.sdk`
— see UNCERTAIN U1 in §8 (observed on all 10 ASM edges in `build.ninja`, e.g.
line 1170; no repo CMake line sets it; C/C++ edges have no `-isysroot`).

## 3. Include directories

Global directory includes, in observed order (`CMakeLists.txt:588-596`):

- `.` (repo root), `lib/Common`, `lib/Common/PlaceHolder`, `pal`, `pal/inc`,
  `pal/inc/rt`, `${ICU_INCLUDE_PATH}` =
  `/opt/homebrew/opt/icu4c/include` plus `/opt/homebrew/opt/icu4c/include/../i18n`
  (second entry added when `INTL_ICU`, `CMakeLists.txt:235-238`),
- `-isystem /usr/local/include` (`CMakeLists.txt:605`).

Directory includes added by intermediate `CMakeLists.txt` files (apply to the
subtree below them):

- `lib/Common/CMakeLists.txt:3`: `lib/Common` (dup of global).
- `lib/Runtime/CMakeLists.txt:3-13`: `lib/Runtime/.`, `lib/Runtime/../JITIDL`
  (`lib/JITIDL` — observed on the command line),
  `lib/Runtime/../Common`, `lib/Runtime/../Backend`, `lib/Runtime/../Parser`,
  `lib/Runtime/../WasmReader` (the `-I` is observed on all 285 Runtime
  units; only the `WasmReader` library itself is unbuilt — AMD64-only, §5.1),
  `lib/Runtime/ByteCode`, `lib/Runtime/PlatformAgnostic`, `lib/Runtime/Math`.
- `lib/Common/DataStructures/CMakeLists.txt:16` and
  `lib/Common/Memory/CMakeLists.txt:64`: `..` (resolves to `lib/Common`).
- `lib/Runtime/Language/CMakeLists.txt:96-98`: `../Math`
  (`lib/Runtime/Math`, dup of the Runtime block).
- `bin/ch/CMakeLists.txt:34` and `bin/GCStress/CMakeLists.txt:8`: `..`
  (`bin/`).
- `pal/CMakeLists.txt:5-6` (PAL subtree only): `pal/inc`, `pal/src`.
- `pal/src/CMakeLists.txt:10` (PAL only): `pal/src/include`; plus
  `CMAKE_INCLUDE_CURRENT_DIR` (`:5`) puts the generated-header dir
  `<build>/pal/src` (for `config.h`, see §5.5) and source `pal/src` first.

Per-target `target_include_directories` (each OBJECT group's own `PUBLIC`
self-directory applies to that group's own units; propagation to link
consumers happens only through real libraries — `ChakraCoreStatic`,
whose `PUBLIC` includes at `lib/CMakeLists.txt:72-84` are `lib/`,
`lib/Backend`, `lib/Common`, `lib/Runtime`, `lib/Runtime/ByteCode`,
`lib/Parser`, `lib/Jsrt`, `lib/SCACore` with `wasm_includes`/`wabt_includes`
empty — observed on GCStress and dylib units, except `lib/Common` which
duplicates the identical global include and appears only once):

| Target | Own includes (PUBLIC) | Source |
|---|---|---|
| every `Chakra.*` OBJECT group | its own source dir | each `*/CMakeLists.txt` `target_include_directories` block |
| `Chakra.Jsrt` | own + `../Backend ../JITIDL ../Runtime ../Runtime/Base ../Runtime/ByteCode ../Runtime/Debug ../Parser ../SCACore` | `lib/Jsrt/CMakeLists.txt:20-30` |
| `Chakra.Jsrt.Core` | own + `../../Backend ../../JITIDL ../../Runtime ../../Runtime/ByteCode ../../Parser ../../SCACore ../` | `lib/Jsrt/Core/CMakeLists.txt:6-15` |
| `Chakra.Parser` | own + `../JITIDL ../Common ../Backend ../Runtime ../Runtime/ByteCode` | `lib/Parser/CMakeLists.txt:31-38` |
| `Chakra.SCACore` | own + `../Parser ../Common ../Backend ../JITIDL ../Runtime ../Runtime/ByteCode` | `lib/SCACore/CMakeLists.txt:14-22` |
| `ch` | `bin/ch`, `<build>/bin/ch` (generated `DbgController.js.h`), `bin/ChakraCore`, `lib/Common`, `lib/Jsrt`, `lib/Runtime`, `lib/Parser` (`Wasm_dep` empty — AMD64-only, `:36-38`) | `bin/ch/CMakeLists.txt:40-49` |
| `ChakraCore` (shared) | own dir + `lib/Backend lib/Common lib/Runtime lib/Runtime/ByteCode lib/Parser lib/Jsrt` + empty `wasm_includes` + bogus `-fvisibility=hidden` include (see §7 Q1) | `bin/ChakraCore/CMakeLists.txt:7-18` |
| `GCStress` | own dir + bogus `/lib/Common /lib/Common/Memory` (see §7 Q2) | `bin/GCStress/CMakeLists.txt:10-14` |

## 4. Source groups and membership

24 groups, 538 files: 507 C++, 21 C, 10 assembly. Order within a group follows
the listing order in its `CMakeLists.txt`. Every file below was verified
present in `build-zigpilot/compile_commands.json` exactly once, and every DB
entry is in exactly one group (§6). Citations give the `add_library` /
selection lines; per-group flags/defines beyond §1-3 are noted per group.

### 4.1 `Chakra.Pal` — OBJECT, 91 files (67 C++, 21 C, 3 ASM)

`pal/src/CMakeLists.txt:182-187`. OSX `PLATFORM_SOURCES` `:54-58` +
ARM64/OSX dispatch wrapper `:73-78`; ARM64 `ARCH_SOURCES` `:79-82`;
portable `SOURCES` `:93-180`. Group defines/flags: §1–2 (PAL-only defines,
`-fstack-protector`, `-fno-builtin -fPIC`); `NO_PAL_MINMAX`/`PAL_STDCPP_COMPAT`
absent (§1).

- `pal/src/exception/machexception.cpp`
- `pal/src/exception/machmessage.cpp`
- `pal/src/arch/arm64/dispatchexceptionwrapper.S`
- `pal/src/arch/arm64/context2.S`
- `pal/src/arch/arm64/debugbreak.S`
- `pal/src/cruntime/file.cpp`
- `pal/src/cruntime/filecrt.cpp`
- `pal/src/cruntime/finite.cpp`
- `pal/src/cruntime/lstr.cpp`
- `pal/src/cruntime/malloc.cpp`
- `pal/src/cruntime/mbstring.cpp`
- `pal/src/cruntime/misc.cpp`
- `pal/src/cruntime/misctls.cpp`
- `pal/src/cruntime/path.cpp`
- `pal/src/cruntime/printf.cpp`
- `pal/src/cruntime/printfcpp.cpp`
- `pal/src/cruntime/silent_printf.cpp`
- `pal/src/cruntime/string.cpp`
- `pal/src/cruntime/stringtls.cpp`
- `pal/src/cruntime/thread.cpp`
- `pal/src/cruntime/wchar.cpp`
- `pal/src/cruntime/wchartls.cpp`
- `pal/src/cruntime/runtime_proxy.cpp`
- `pal/src/safecrt/makepath_s.c`
- `pal/src/safecrt/mbusafecrt.c`
- `pal/src/safecrt/safecrt_input_s.c`
- `pal/src/safecrt/safecrt_output_s.c`
- `pal/src/safecrt/safecrt_winput_s.c`
- `pal/src/safecrt/safecrt_woutput_s.c`
- `pal/src/safecrt/memcpy_s.c`
- `pal/src/safecrt/memmove_s.c`
- `pal/src/safecrt/sprintf.c`
- `pal/src/safecrt/sscanf.c`
- `pal/src/safecrt/strcat_s.c`
- `pal/src/safecrt/strcpy_s.c`
- `pal/src/safecrt/strncat_s.c`
- `pal/src/safecrt/strncpy_s.c`
- `pal/src/safecrt/swprintf.c`
- `pal/src/safecrt/vsprintf.c`
- `pal/src/safecrt/vswprint.c`
- `pal/src/safecrt/wcscpy_s.c`
- `pal/src/safecrt/wcsncpy_s.c`
- `pal/src/safecrt/xtoa_s.c`
- `pal/src/safecrt/xtow_s.c`
- `pal/src/debug/debug.cpp`
- `pal/src/exception/seh.cpp`
- `pal/src/exception/signal.cpp`
- `pal/src/file/directory.cpp`
- `pal/src/file/pal_file.cpp`
- `pal/src/file/filetime.cpp`
- `pal/src/file/pal_path.cpp`
- `pal/src/file/shmfilelockmgr.cpp`
- `pal/src/handlemgr/handleapi.cpp`
- `pal/src/handlemgr/handlemgr.cpp`
- `pal/src/init/pal.cpp`
- `pal/src/init/sxs.cpp`
- `pal/src/loader/module.cpp`
- `pal/src/loader/modulename.cpp`
- `pal/src/locale/unicode.cpp`
- `pal/src/locale/unicode_data.cpp`
- `pal/src/locale/utf8.cpp`
- `pal/src/map/common.cpp`
- `pal/src/map/map.cpp`
- `pal/src/map/virtual.cpp`
- `pal/src/memory/heap.cpp`
- `pal/src/memory/local.cpp`
- `pal/src/misc/bstr.cpp`
- `pal/src/misc/dbgmsg.cpp`
- `pal/src/misc/error.cpp`
- `pal/src/misc/environ.cpp`
- `pal/src/misc/random.cpp`
- `pal/src/misc/strutil.cpp`
- `pal/src/misc/time.cpp`
- `pal/src/misc/sysinfo.cpp`
- `pal/src/misc/utils.cpp`
- `pal/src/objmgr/palobjbase.cpp`
- `pal/src/objmgr/shmobject.cpp`
- `pal/src/objmgr/shmobjectmanager.cpp`
- `pal/src/shmemory/shmemory.cpp`
- `pal/src/synchobj/event.cpp`
- `pal/src/synchobj/mutex.cpp`
- `pal/src/synchobj/semaphore.cpp`
- `pal/src/synchmgr/synchcontrollers.cpp`
- `pal/src/synchmgr/synchmanager.cpp`
- `pal/src/synchmgr/wait.cpp`
- `pal/src/sync/cs.cpp`
- `pal/src/sync/cclock.cpp`
- `pal/src/thread/context.cpp`
- `pal/src/thread/process.cpp`
- `pal/src/thread/pal_thread.cpp`
- `pal/src/thread/threadsusp.cpp`

### 4.2 `Chakra.Common.Codex` — OBJECT, 1 file

`lib/Common/Codex/CMakeLists.txt:1-2`. Also archived as
`Chakra.Common.Codex.Singular` STATIC (`:7-11`, shared builds) for `ch`.

- `lib/Common/Codex/Utf8Codex.cpp`

### 4.3 `Chakra.Common.Common` — OBJECT, 15 files (14 C++, 1 ASM)

`lib/Common/Common/CMakeLists.txt:7-23`. ARM64 `ARCH_SOURCES` `:3-4`.

- `lib/Common/Common/arm64/arm64_GET_CURRENT_FRAME.S`
- `lib/Common/Common/CfgLogger.cpp`
- `lib/Common/Common/CommonCommonPch.cpp`
- `lib/Common/Common/DateUtilities.cpp`
- `lib/Common/Common/Event.cpp`
- `lib/Common/Common/Int32Math.cpp`
- `lib/Common/Common/Int64Math.cpp`
- `lib/Common/Common/Jobs.cpp`
- `lib/Common/Common/MathUtil.cpp`
- `lib/Common/Common/NumberUtilities.cpp`
- `lib/Common/Common/NumberUtilities_strtod.cpp`
- `lib/Common/Common/RejitReason.cpp`
- `lib/Common/Common/SmartFpuControl.cpp`
- `lib/Common/Common/Tick.cpp`
- `lib/Common/Common/vtinfo.cpp`

### 4.4 `Chakra.Common.Core` — OBJECT, 21 files

`lib/Common/Core/CMakeLists.txt:1-23`.

- `lib/Common/Core/Api.cpp`
- `lib/Common/Core/BinaryFeatureControl.cpp`
- `lib/Common/Core/CmdParser.cpp`
- `lib/Common/Core/CodexAssert.cpp`
- `lib/Common/Core/CommonCorePch.cpp`
- `lib/Common/Core/ConfigFlagsTable.cpp`
- `lib/Common/Core/ConfigParser.cpp`
- `lib/Common/Core/CRC.cpp`
- `lib/Common/Core/DbgHelpSymbolManager.cpp`
- `lib/Common/Core/DelayLoadLibrary.cpp`
- `lib/Common/Core/EtwTraceCore.cpp`
- `lib/Common/Core/FaultInjection.cpp`
- `lib/Common/Core/GlobalSecurityPolicy.cpp`
- `lib/Common/Core/Output.cpp`
- `lib/Common/Core/PerfCounter.cpp`
- `lib/Common/Core/PerfCounterImpl.cpp`
- `lib/Common/Core/PerfCounterSet.cpp`
- `lib/Common/Core/ProfileInstrument.cpp`
- `lib/Common/Core/ProfileMemory.cpp`
- `lib/Common/Core/StackBackTrace.cpp`
- `lib/Common/Core/SysInfo.cpp`

### 4.5 `Chakra.Common.DataStructures` — OBJECT, 12 files

`lib/Common/DataStructures/CMakeLists.txt:1-14`.

- `lib/Common/DataStructures/BigUInt.cpp`
- `lib/Common/DataStructures/BufferBuilder.cpp`
- `lib/Common/DataStructures/CommonDataStructuresPch.cpp`
- `lib/Common/DataStructures/DictionaryStats.cpp`
- `lib/Common/DataStructures/FixedBitVector.cpp`
- `lib/Common/DataStructures/FixedBitVectorEnumerator.cpp`
- `lib/Common/DataStructures/GrowingArray.cpp`
- `lib/Common/DataStructures/ImmutableList.cpp`
- `lib/Common/DataStructures/InternalString.cpp`
- `lib/Common/DataStructures/InternalStringNoCaseComparer.cpp`
- `lib/Common/DataStructures/Interval.cpp`
- `lib/Common/DataStructures/SizePolicy.cpp`

### 4.6 `Chakra.Common.Exceptions` — OBJECT, 4 files

`lib/Common/Exceptions/CMakeLists.txt:1-7` (`CommonExceptionsPch.cpp` is
commented out at `:2`).

- `lib/Common/Exceptions/ExceptionCheck.cpp`
- `lib/Common/Exceptions/ExceptionBase.cpp`
- `lib/Common/Exceptions/ReportError.cpp`
- `lib/Common/Exceptions/Throw.cpp`

### 4.7 `Chakra.Common.Memory` — OBJECT, 41 files (40 C++, 1 ASM)

`lib/Common/Memory/CMakeLists.txt:60-62`. Portable list `:1-41`, ARM64
additions `:48-52`.

- `lib/Common/Memory/Allocator.cpp`
- `lib/Common/Memory/ArenaAllocator.cpp`
- `lib/Common/Memory/CustomHeap.cpp`
- `lib/Common/Memory/CommonMemoryPch.cpp`
- `lib/Common/Memory/DelayDeletingFunctionTable.cpp`
- `lib/Common/Memory/EtwMemoryTracking.cpp`
- `lib/Common/Memory/ForcedMemoryConstraints.cpp`
- `lib/Common/Memory/HeapAllocator.cpp`
- `lib/Common/Memory/HeapAllocatorOperators.cpp`
- `lib/Common/Memory/HeapBlock.cpp`
- `lib/Common/Memory/HeapBlockMap.cpp`
- `lib/Common/Memory/HeapBucket.cpp`
- `lib/Common/Memory/HeapInfo.cpp`
- `lib/Common/Memory/HeapInfoManager.cpp`
- `lib/Common/Memory/IdleDecommitPageAllocator.cpp`
- `lib/Common/Memory/LargeHeapBlock.cpp`
- `lib/Common/Memory/LargeHeapBucket.cpp`
- `lib/Common/Memory/LeakReport.cpp`
- `lib/Common/Memory/MarkContext.cpp`
- `lib/Common/Memory/MemoryLogger.cpp`
- `lib/Common/Memory/MemoryTracking.cpp`
- `lib/Common/Memory/PageAllocator.cpp`
- `lib/Common/Memory/Recycler.cpp`
- `lib/Common/Memory/RecyclerHeuristic.cpp`
- `lib/Common/Memory/RecyclerObjectDumper.cpp`
- `lib/Common/Memory/RecyclerObjectGraphDumper.cpp`
- `lib/Common/Memory/RecyclerPageAllocator.cpp`
- `lib/Common/Memory/RecyclerSweep.cpp`
- `lib/Common/Memory/RecyclerSweepManager.cpp`
- `lib/Common/Memory/RecyclerWriteBarrierManager.cpp`
- `lib/Common/Memory/SmallFinalizableHeapBlock.cpp`
- `lib/Common/Memory/SmallFinalizableHeapBucket.cpp`
- `lib/Common/Memory/SmallHeapBlockAllocator.cpp`
- `lib/Common/Memory/SmallLeafHeapBlock.cpp`
- `lib/Common/Memory/SmallLeafHeapBucket.cpp`
- `lib/Common/Memory/SmallNormalHeapBlock.cpp`
- `lib/Common/Memory/SmallNormalHeapBucket.cpp`
- `lib/Common/Memory/StressTest.cpp`
- `lib/Common/Memory/VirtualAllocWrapper.cpp`
- `lib/Common/Memory/arm64/XDataAllocator.cpp`
- `lib/Common/Memory/arm64/arm64_SAVE_REGISTERS.S`

### 4.8 `Chakra.Common.Util` — OBJECT, 1 file

`lib/Common/Util/CMakeLists.txt:1-2`.

- `lib/Common/Util/Pinned.cpp`

### 4.9 `Chakra.Jsrt` — OBJECT, 14 files

`lib/Jsrt/CMakeLists.txt:1-16`. Extra own includes §3.

- `lib/Jsrt/Jsrt.cpp`
- `lib/Jsrt/JsrtDebugUtils.cpp`
- `lib/Jsrt/JsrtDebugManager.cpp`
- `lib/Jsrt/JsrtDebuggerObject.cpp`
- `lib/Jsrt/JsrtDiag.cpp`
- `lib/Jsrt/JsrtContext.cpp`
- `lib/Jsrt/JsrtExternalArrayBuffer.cpp`
- `lib/Jsrt/JsrtExternalObject.cpp`
- `lib/Jsrt/JsrtDebugEventObject.cpp`
- `lib/Jsrt/JsrtHelper.cpp`
- `lib/Jsrt/JsrtPch.cpp`
- `lib/Jsrt/JsrtRuntime.cpp`
- `lib/Jsrt/JsrtSourceHolder.cpp`
- `lib/Jsrt/JsrtThreadService.cpp`

### 4.10 `Chakra.Jsrt.Core` — OBJECT, 2 files

`lib/Jsrt/Core/CMakeLists.txt:1-4`. Extra own includes §3.

- `lib/Jsrt/Core/JsrtContextCore.cpp`
- `lib/Jsrt/Core/JsrtCore.cpp`

### 4.11 `Chakra.Parser` — OBJECT, 26 files

`lib/Parser/CMakeLists.txt:1-29`. 27 entries listed; `jserr.gen` (`:26`)
is not a compilable unit and has no compile-DB entry, so 26 compile.
Extra own includes §3.

- `lib/Parser/Alloc.cpp`
- `lib/Parser/BackgroundParser.cpp`
- `lib/Parser/BGParseManager.cpp`
- `lib/Parser/CaseInsensitive.cpp`
- `lib/Parser/CharClassifier.cpp`
- `lib/Parser/CharSet.cpp`
- `lib/Parser/CharTrie.cpp`
- `lib/Parser/DebugWriter.cpp`
- `lib/Parser/Hash.cpp`
- `lib/Parser/OctoquadIdentifier.cpp`
- `lib/Parser/Parse.cpp`
- `lib/Parser/ParserPch.cpp`
- `lib/Parser/ptree.cpp`
- `lib/Parser/RegexCompileTime.cpp`
- `lib/Parser/RegexParser.cpp`
- `lib/Parser/RegexPattern.cpp`
- `lib/Parser/RegexRuntime.cpp`
- `lib/Parser/RegexStats.cpp`
- `lib/Parser/Scan.cpp`
- `lib/Parser/StandardChars.cpp`
- `lib/Parser/TextbookBoyerMoore.cpp`
- `lib/Parser/cmperr.cpp`
- `lib/Parser/errstr.cpp`
- `lib/Parser/globals.cpp`
- `lib/Parser/rterror.cpp`
- `lib/Parser/screrror.cpp`

### 4.12 `Chakra.SCACore` — OBJECT, 9 files

`lib/SCACore/CMakeLists.txt:1-11`. Extra own includes §3.

- `lib/SCACore/SCACore.cpp`
- `lib/SCACore/SCACorePch.cpp`
- `lib/SCACore/SCADeserialization.cpp`
- `lib/SCACore/SCAEngine.cpp`
- `lib/SCACore/SCAPropBag.cpp`
- `lib/SCACore/SCASerialization.cpp`
- `lib/SCACore/StreamHelper.cpp`
- `lib/SCACore/StreamReader.cpp`
- `lib/SCACore/StreamWriter.cpp`

### 4.13 `Chakra.Runtime.Base` — OBJECT, 34 files

`lib/Runtime/Base/CMakeLists.txt:1-39` (`DelayLoadLibrary.cpp`,
`Entropy.cpp`, `WindowsFoundationAdapter.cpp` commented out at `:7-8,36`).

- `lib/Runtime/Base/CallInfo.cpp`
- `lib/Runtime/Base/CharStringCache.cpp`
- `lib/Runtime/Base/Constants.cpp`
- `lib/Runtime/Base/CrossSite.cpp`
- `lib/Runtime/Base/Debug.cpp`
- `lib/Runtime/Base/EtwTrace.cpp`
- `lib/Runtime/Base/Exception.cpp`
- `lib/Runtime/Base/ExpirableObject.cpp`
- `lib/Runtime/Base/FunctionBody.cpp`
- `lib/Runtime/Base/FunctionExecutionStateMachine.cpp`
- `lib/Runtime/Base/FunctionInfo.cpp`
- `lib/Runtime/Base/LeaveScriptObject.cpp`
- `lib/Runtime/Base/LineOffsetCache.cpp`
- `lib/Runtime/Base/PerfHint.cpp`
- `lib/Runtime/Base/PropertyRecord.cpp`
- `lib/Runtime/Base/RuntimeBasePch.cpp`
- `lib/Runtime/Base/ScriptContext.cpp`
- `lib/Runtime/Base/ScriptContextOptimizationOverrideInfo.cpp`
- `lib/Runtime/Base/ScriptContextProfiler.cpp`
- `lib/Runtime/Base/ScriptMemoryDumper.cpp`
- `lib/Runtime/Base/SourceContextInfo.cpp`
- `lib/Runtime/Base/SourceHolder.cpp`
- `lib/Runtime/Base/StackProber.cpp`
- `lib/Runtime/Base/TempArenaAllocatorObject.cpp`
- `lib/Runtime/Base/TestEtwEventSink.cpp`
- `lib/Runtime/Base/ThreadBoundThreadContextManager.cpp`
- `lib/Runtime/Base/ThreadContextInfo.cpp`
- `lib/Runtime/Base/ThreadContext.cpp`
- `lib/Runtime/Base/ThreadContextTlsEntry.cpp`
- `lib/Runtime/Base/ThreadServiceWrapperBase.cpp`
- `lib/Runtime/Base/Utf8SourceInfo.cpp`
- `lib/Runtime/Base/VTuneChakraProfile.cpp`
- `lib/Runtime/Base/WindowsGlobalizationAdapter.cpp`
- `lib/Runtime/Base/jitprofiling.cpp`

### 4.14 `Chakra.Runtime.ByteCode` — OBJECT, 20 files

`lib/Runtime/ByteCode/CMakeLists.txt:1-22`.

- `lib/Runtime/ByteCode/AsmJsByteCodeDumper.cpp`
- `lib/Runtime/ByteCode/AsmJsByteCodeWriter.cpp`
- `lib/Runtime/ByteCode/WasmByteCodeWriter.cpp`
- `lib/Runtime/ByteCode/BackendOpCodeAttr.cpp`
- `lib/Runtime/ByteCode/ByteBlock.cpp`
- `lib/Runtime/ByteCode/ByteCodeDumper.cpp`
- `lib/Runtime/ByteCode/ByteCodeEmitter.cpp`
- `lib/Runtime/ByteCode/ByteCodeGenerator.cpp`
- `lib/Runtime/ByteCode/ByteCodeReader.cpp`
- `lib/Runtime/ByteCode/ByteCodeSerializer.cpp`
- `lib/Runtime/ByteCode/ByteCodeWriter.cpp`
- `lib/Runtime/ByteCode/FuncInfo.cpp`
- `lib/Runtime/ByteCode/OpCodeUtil.cpp`
- `lib/Runtime/ByteCode/OpCodeUtilAsmJs.cpp`
- `lib/Runtime/ByteCode/OpCodes.cpp`
- `lib/Runtime/ByteCode/RuntimeByteCodePch.cpp`
- `lib/Runtime/ByteCode/Scope.cpp`
- `lib/Runtime/ByteCode/ScopeInfo.cpp`
- `lib/Runtime/ByteCode/StatementReader.cpp`
- `lib/Runtime/ByteCode/Symbol.cpp`

### 4.15 `Chakra.Runtime.Debug` — OBJECT, 25 files

`lib/Runtime/Debug/CMakeLists.txt:1-27`.

- `lib/Runtime/Debug/BreakpointProbe.cpp`
- `lib/Runtime/Debug/DebugContext.cpp`
- `lib/Runtime/Debug/DebugDocument.cpp`
- `lib/Runtime/Debug/DebugManager.cpp`
- `lib/Runtime/Debug/DebuggingFlags.cpp`
- `lib/Runtime/Debug/DiagHelperMethodWrapper.cpp`
- `lib/Runtime/Debug/DiagObjectModel.cpp`
- `lib/Runtime/Debug/DiagProbe.cpp`
- `lib/Runtime/Debug/DiagStackFrame.cpp`
- `lib/Runtime/Debug/MutationBreakpoint.cpp`
- `lib/Runtime/Debug/ProbeContainer.cpp`
- `lib/Runtime/Debug/RuntimeDebugPch.cpp`
- `lib/Runtime/Debug/TTActionEvents.cpp`
- `lib/Runtime/Debug/TTEventLog.cpp`
- `lib/Runtime/Debug/TTEvents.cpp`
- `lib/Runtime/Debug/TTExecutionInfo.cpp`
- `lib/Runtime/Debug/TTInflateMap.cpp`
- `lib/Runtime/Debug/TTRuntimeInfoTracker.cpp`
- `lib/Runtime/Debug/TTSerialize.cpp`
- `lib/Runtime/Debug/TTSnapObjects.cpp`
- `lib/Runtime/Debug/TTSnapshot.cpp`
- `lib/Runtime/Debug/TTSnapshotExtractor.cpp`
- `lib/Runtime/Debug/TTSnapTypes.cpp`
- `lib/Runtime/Debug/TTSnapValues.cpp`
- `lib/Runtime/Debug/TTSupport.cpp`

### 4.16 `Chakra.Runtime.Language` — OBJECT, 64 files (62 C++, 2 ASM)

`lib/Runtime/Language/CMakeLists.txt:94`. Portable list `:5-67`, ARM64
additions `:81-86`. No `-D_ENABLE_DYNAMIC_THUNKS` (`BuildJIT` false, `:1-3`).

- `lib/Runtime/Language/AsmJs.cpp`
- `lib/Runtime/Language/AsmJsByteCodeGenerator.cpp`
- `lib/Runtime/Language/AsmJsCodeGenerator.cpp`
- `lib/Runtime/Language/AsmJsEncoder.cpp`
- `lib/Runtime/Language/AsmJsLink.cpp`
- `lib/Runtime/Language/AsmJsModule.cpp`
- `lib/Runtime/Language/AsmJsTypes.cpp`
- `lib/Runtime/Language/AsmJsUtils.cpp`
- `lib/Runtime/Language/WAsmjsUtils.cpp`
- `lib/Runtime/Language/WebAssemblySource.cpp`
- `lib/Runtime/Language/CacheOperators.cpp`
- `lib/Runtime/Language/ConstructorCache.cpp`
- `lib/Runtime/Language/CodeGenRecyclableData.cpp`
- `lib/Runtime/Language/DynamicProfileInfo.cpp`
- `lib/Runtime/Language/DynamicProfileMutator.cpp`
- `lib/Runtime/Language/DynamicProfileStorage.cpp`
- `lib/Runtime/Language/ExecutionMode.cpp`
- `lib/Runtime/Language/FunctionCodeGenRuntimeData.cpp`
- `lib/Runtime/Language/InlineCache.cpp`
- `lib/Runtime/Language/InterpreterStackFrame.cpp`
- `lib/Runtime/Language/JavascriptConversion.cpp`
- `lib/Runtime/Language/JavascriptExceptionObject.cpp`
- `lib/Runtime/Language/JavascriptExceptionOperators.cpp`
- `lib/Runtime/Language/JavascriptMathOperators.cpp`
- `lib/Runtime/Language/JavascriptOperators.cpp`
- `lib/Runtime/Language/JavascriptStackWalker.cpp`
- `lib/Runtime/Language/ModuleNamespace.cpp`
- `lib/Runtime/Language/ModuleNamespaceEnumerator.cpp`
- `lib/Runtime/Language/ProfilingHelpers.cpp`
- `lib/Runtime/Language/PrototypeChainCache.cpp`
- `lib/Runtime/Language/RuntimeLanguagePch.cpp`
- `lib/Runtime/Language/SimdBool16x8Operation.cpp`
- `lib/Runtime/Language/SimdBool16x8OperationX86X64.cpp`
- `lib/Runtime/Language/SimdBool32x4Operation.cpp`
- `lib/Runtime/Language/SimdBool32x4OperationX86X64.cpp`
- `lib/Runtime/Language/SimdBool8x16Operation.cpp`
- `lib/Runtime/Language/SimdBool8x16OperationX86X64.cpp`
- `lib/Runtime/Language/SimdFloat32x4Operation.cpp`
- `lib/Runtime/Language/SimdFloat32x4OperationX86X64.cpp`
- `lib/Runtime/Language/SimdInt64x2Operation.cpp`
- `lib/Runtime/Language/SimdFloat64x2Operation.cpp`
- `lib/Runtime/Language/SimdFloat64x2OperationX86X64.cpp`
- `lib/Runtime/Language/SimdInt16x8Operation.cpp`
- `lib/Runtime/Language/SimdInt16x8OperationX86X64.cpp`
- `lib/Runtime/Language/SimdInt32x4Operation.cpp`
- `lib/Runtime/Language/SimdInt32x4OperationX86X64.cpp`
- `lib/Runtime/Language/SimdInt8x16Operation.cpp`
- `lib/Runtime/Language/SimdInt8x16OperationX86X64.cpp`
- `lib/Runtime/Language/SimdUint16x8Operation.cpp`
- `lib/Runtime/Language/SimdUint16x8OperationX86X64.cpp`
- `lib/Runtime/Language/SimdUint32x4Operation.cpp`
- `lib/Runtime/Language/SimdUint32x4OperationX86X64.cpp`
- `lib/Runtime/Language/SimdUint8x16Operation.cpp`
- `lib/Runtime/Language/SimdUint8x16OperationX86X64.cpp`
- `lib/Runtime/Language/SimdUtils.cpp`
- `lib/Runtime/Language/SourceDynamicProfileManager.cpp`
- `lib/Runtime/Language/SimpleDataCacheWrapper.cpp`
- `lib/Runtime/Language/SourceTextModuleRecord.cpp`
- `lib/Runtime/Language/StackTraceArguments.cpp`
- `lib/Runtime/Language/TaggedInt.cpp`
- `lib/Runtime/Language/ValueType.cpp`
- `lib/Runtime/Language/arm64/StackFrame.cpp`
- `lib/Runtime/Language/arm64/arm64_Thunks.S`
- `lib/Runtime/Language/arm64/arm64_CallEhFrame.S`

### 4.17 `Chakra.Runtime.Library` — OBJECT, 102 files (99 C++, 3 ASM)

`lib/Runtime/Library/CMakeLists.txt:144`. Portable list `:24-126`
(`JavascriptErrorDebug.cpp` commented out at `:66`); `Wasm_dep` empty
(AMD64-only, `:5-16`); ARM64 additions `:136-141`. No
`-D_ENABLE_DYNAMIC_THUNKS` (`BuildJIT` false, `:1-3`); no wabt include
(`CAN_BUILD_WABT` unset, `:18-22`).

- `lib/Runtime/Library/ArgumentsObject.cpp`
- `lib/Runtime/Library/ArgumentsObjectEnumerator.cpp`
- `lib/Runtime/Library/DelayFreeArrayBufferHelper.cpp`
- `lib/Runtime/Library/ArrayBuffer.cpp`
- `lib/Runtime/Library/AtomicsOperations.cpp`
- `lib/Runtime/Library/AtomicsObject.cpp`
- `lib/Runtime/Library/BoundFunction.cpp`
- `lib/Runtime/Library/BufferStringBuilder.cpp`
- `lib/Runtime/Library/CommonExternalApiImpl.cpp`
- `lib/Runtime/Library/CompoundString.cpp`
- `lib/Runtime/Library/ConcatString.cpp`
- `lib/Runtime/Library/CustomExternalWrapperObject.cpp`
- `lib/Runtime/Library/DataView.cpp`
- `lib/Runtime/Library/DateImplementation.cpp`
- `lib/Runtime/Library/ES5Array.cpp`
- `lib/Runtime/Library/ES5ArrayIndexEnumerator.cpp`
- `lib/Runtime/Library/EngineInterfaceObject.cpp`
- `lib/Runtime/Library/ExternalLibraryBase.cpp`
- `lib/Runtime/Library/ForInObjectEnumerator.cpp`
- `lib/Runtime/Library/GlobalObject.cpp`
- `lib/Runtime/Library/IntlEngineInterfaceExtensionObject.cpp`
- `lib/Runtime/Library/JSON.cpp`
- `lib/Runtime/Library/JSONParser.cpp`
- `lib/Runtime/Library/JSONScanner.cpp`
- `lib/Runtime/Library/JSONStack.cpp`
- `lib/Runtime/Library/JSONStringBuilder.cpp`
- `lib/Runtime/Library/JSONStringifier.cpp`
- `lib/Runtime/Library/JavascriptArray.cpp`
- `lib/Runtime/Library/JavascriptArrayIndexEnumerator.cpp`
- `lib/Runtime/Library/JavascriptArrayIndexEnumeratorBase.cpp`
- `lib/Runtime/Library/JavascriptArrayIndexSnapshotEnumerator.cpp`
- `lib/Runtime/Library/JavascriptArrayIterator.cpp`
- `lib/Runtime/Library/JavascriptAsyncFromSyncIterator.cpp`
- `lib/Runtime/Library/JavascriptAsyncFunction.cpp`
- `lib/Runtime/Library/JavascriptAsyncGenerator.cpp`
- `lib/Runtime/Library/JavascriptAsyncGeneratorFunction.cpp`
- `lib/Runtime/Library/JavascriptBoolean.cpp`
- `lib/Runtime/Library/JavascriptBooleanObject.cpp`
- `lib/Runtime/Library/JavascriptBuiltInFunctions.cpp`
- `lib/Runtime/Library/JavascriptDate.cpp`
- `lib/Runtime/Library/JavascriptError.cpp`
- `lib/Runtime/Library/JavascriptExceptionMetadata.cpp`
- `lib/Runtime/Library/JavascriptExternalFunction.cpp`
- `lib/Runtime/Library/JavascriptFunction.cpp`
- `lib/Runtime/Library/JavascriptGenerator.cpp`
- `lib/Runtime/Library/JavascriptGeneratorFunction.cpp`
- `lib/Runtime/Library/JavascriptIterator.cpp`
- `lib/Runtime/Library/JavascriptLibrary.cpp`
- `lib/Runtime/Library/JavascriptListIterator.cpp`
- `lib/Runtime/Library/JavascriptMap.cpp`
- `lib/Runtime/Library/JavascriptMapIterator.cpp`
- `lib/Runtime/Library/JavascriptNumber.cpp`
- `lib/Runtime/Library/JavascriptNumberObject.cpp`
- `lib/Runtime/Library/JavascriptObject.cpp`
- `lib/Runtime/Library/JavascriptPromise.cpp`
- `lib/Runtime/Library/JavascriptProxy.cpp`
- `lib/Runtime/Library/JavascriptReflect.cpp`
- `lib/Runtime/Library/JavascriptRegExpConstructor.cpp`
- `lib/Runtime/Library/JavascriptRegExpEnumerator.cpp`
- `lib/Runtime/Library/JavascriptRegularExpression.cpp`
- `lib/Runtime/Library/JavascriptRegularExpressionResult.cpp`
- `lib/Runtime/Library/JavascriptSet.cpp`
- `lib/Runtime/Library/JavascriptSetIterator.cpp`
- `lib/Runtime/Library/JavascriptString.cpp`
- `lib/Runtime/Library/JavascriptStringEnumerator.cpp`
- `lib/Runtime/Library/JavascriptStringIterator.cpp`
- `lib/Runtime/Library/JavascriptStringObject.cpp`
- `lib/Runtime/Library/JavascriptBigInt.cpp`
- `lib/Runtime/Library/JavascriptBigIntObject.cpp`
- `lib/Runtime/Library/JavascriptSymbol.cpp`
- `lib/Runtime/Library/JavascriptSymbolObject.cpp`
- `lib/Runtime/Library/JavascriptTypedNumber.cpp`
- `lib/Runtime/Library/JavascriptWeakMap.cpp`
- `lib/Runtime/Library/JavascriptWeakSet.cpp`
- `lib/Runtime/Library/JsBuiltInEngineInterfaceExtensionObject.cpp`
- `lib/Runtime/Library/LazyJSONString.cpp`
- `lib/Runtime/Library/LiteralString.cpp`
- `lib/Runtime/Library/MathLibrary.cpp`
- `lib/Runtime/Library/ModuleRoot.cpp`
- `lib/Runtime/Library/ObjectPrototypeObject.cpp`
- `lib/Runtime/Library/ProfileString.cpp`
- `lib/Runtime/Library/PropertyRecordUsageCache.cpp`
- `lib/Runtime/Library/PropertyString.cpp`
- `lib/Runtime/Library/RegexHelper.cpp`
- `lib/Runtime/Library/RootObjectBase.cpp`
- `lib/Runtime/Library/RuntimeFunction.cpp`
- `lib/Runtime/Library/RuntimeLibraryPch.cpp`
- `lib/Runtime/Library/ScriptFunction.cpp`
- `lib/Runtime/Library/SharedArrayBuffer.cpp`
- `lib/Runtime/Library/SingleCharString.cpp`
- `lib/Runtime/Library/SparseArraySegment.cpp`
- `lib/Runtime/Library/StackScriptFunction.cpp`
- `lib/Runtime/Library/StringCopyInfo.cpp`
- `lib/Runtime/Library/SubString.cpp`
- `lib/Runtime/Library/ThrowErrorObject.cpp`
- `lib/Runtime/Library/TypedArray.cpp`
- `lib/Runtime/Library/TypedArrayIndexEnumerator.cpp`
- `lib/Runtime/Library/UriHelper.cpp`
- `lib/Runtime/Library/VerifyMarkFalseReference.cpp`
- `lib/Runtime/Library/arm64/arm64_CallFunction.S`
- `lib/Runtime/Library/arm64/arm64_DeferredDeserializeThunk.S`
- `lib/Runtime/Library/arm64/arm64_DeferredParsingThunk.S`

### 4.18 `Chakra.Runtime.Math` — OBJECT, 2 files

`lib/Runtime/Math/CMakeLists.txt:1-4`.

- `lib/Runtime/Math/AsmJsMath.cpp`
- `lib/Runtime/Math/RuntimeMathPch.cpp`

### 4.19 `Chakra.Runtime.Types` — OBJECT, 27 files

`lib/Runtime/Types/CMakeLists.txt:1-29`.

- `lib/Runtime/Types/ActivationObject.cpp`
- `lib/Runtime/Types/ArrayObject.cpp`
- `lib/Runtime/Types/DeferredTypeHandler.cpp`
- `lib/Runtime/Types/DictionaryTypeHandler.cpp`
- `lib/Runtime/Types/DynamicObject.cpp`
- `lib/Runtime/Types/DynamicObjectPropertyEnumerator.cpp`
- `lib/Runtime/Types/DynamicType.cpp`
- `lib/Runtime/Types/ES5ArrayTypeHandler.cpp`
- `lib/Runtime/Types/JavascriptEnumerator.cpp`
- `lib/Runtime/Types/JavascriptStaticEnumerator.cpp`
- `lib/Runtime/Types/MissingPropertyTypeHandler.cpp`
- `lib/Runtime/Types/NullTypeHandler.cpp`
- `lib/Runtime/Types/PathTypeHandler.cpp`
- `lib/Runtime/Types/PropertyDescriptor.cpp`
- `lib/Runtime/Types/RecyclableObject.cpp`
- `lib/Runtime/Types/RuntimeTypePch.cpp`
- `lib/Runtime/Types/ScriptFunctionType.cpp`
- `lib/Runtime/Types/SimpleDictionaryTypeHandler.cpp`
- `lib/Runtime/Types/SimpleDictionaryUnorderedTypeHandler.cpp`
- `lib/Runtime/Types/SimpleTypeHandler.cpp`
- `lib/Runtime/Types/SpreadArgument.cpp`
- `lib/Runtime/Types/StaticType.cpp`
- `lib/Runtime/Types/Type.cpp`
- `lib/Runtime/Types/TypeHandler.cpp`
- `lib/Runtime/Types/TypePath.cpp`
- `lib/Runtime/Types/TypePropertyCache.cpp`
- `lib/Runtime/Types/UnscopablesWrapperObject.cpp`

### 4.20 `Chakra.Runtime.PlatformAgnostic` — OBJECT, 11 files

`lib/Runtime/PlatformAgnostic/Platform/CMakeLists.txt:39-41` (parent at
`lib/Runtime/PlatformAgnostic/CMakeLists.txt:3` only adds the subdirectory).
Portable list `:3-12`; ICU variant `:19-24` (`NO_ICU` unset, so
`Common/UnicodeText.ICU.cpp`); OSX additions `:31-37` (`Linux/PerfTrace.cpp`
commented out at `:35`).

- `lib/Runtime/PlatformAgnostic/Platform/Common/UnicodeText.Common.cpp`
- `lib/Runtime/PlatformAgnostic/Platform/Common/HiResTimer.cpp`
- `lib/Runtime/PlatformAgnostic/Platform/Common/DateTime.cpp`
- `lib/Runtime/PlatformAgnostic/Platform/Linux/NumbersUtility.cpp`
- `lib/Runtime/PlatformAgnostic/Platform/POSIX/ArraysUtility.cpp`
- `lib/Runtime/PlatformAgnostic/Platform/Common/Thread.cpp`
- `lib/Runtime/PlatformAgnostic/Platform/Common/Trace.cpp`
- `lib/Runtime/PlatformAgnostic/Platform/Common/SystemInfo.Common.cpp`
- `lib/Runtime/PlatformAgnostic/Platform/Common/UnicodeText.ICU.cpp`
- `lib/Runtime/PlatformAgnostic/Platform/Unix/AssemblyCommon.cpp`
- `lib/Runtime/PlatformAgnostic/Platform/Unix/SystemInfo.cpp`

Also archived as `Chakra.Runtime.PlatformAgnostic.Singular` STATIC
(`:47-52`, shared builds) for `ch`.

### 4.21 `ChakraCoreStatic.cpp` — 1 file

Compiled directly into the `ChakraCoreStatic` STATIC library
(`lib/CMakeLists.txt:21-22`).

- `lib/ChakraCoreStatic.cpp`

### 4.22 `ch` — executable, 8 files

`bin/ch/CMakeLists.txt:8-17,25`. Static-only `../ChakraCore/TestHooks.cpp`
(`:19-23`) excluded. Own includes §3; `-fPIE` compile+link (§2.1);
`-Dch_EXPORTS` (§1). Ordering: `add_dependencies(ch dbg_controller_h)`
(`:27`) — the generated header (§5.4) exists before linking.

- `bin/ch/ch.cpp`
- `bin/ch/ChakraRtInterface.cpp`
- `bin/ch/CodexAssert.cpp`
- `bin/ch/Debugger.cpp`
- `bin/ch/Helpers.cpp`
- `bin/ch/HostConfigFlags.cpp`
- `bin/ch/WScriptJsrt.cpp`
- `bin/ch/RuntimeThreadData.cpp`

### 4.23 `ChakraCore` — SHARED library sources, 3 files

`bin/ChakraCore/CMakeLists.txt:1-5`. Own includes §3 (incl. Q1 quirk);
`-fPIC` automatic for `SHARED`; `-DChakraCore_EXPORTS` (§1).

- `bin/ChakraCore/ChakraCoreShared.cpp`
- `bin/ChakraCore/ConfigParserExternals.cpp`
- `bin/ChakraCore/TestHooks.cpp`

### 4.24 `GCStress` — executable, 4 files

`bin/GCStress/CMakeLists.txt:1-6`. Built because NOT `CC_LIBS_ONLY_BUILD`
(`bin/CMakeLists.txt:1`) and neither Android nor Valgrind (`:2-4`). Own
includes §3 (incl. Q2 quirk);
no `-fPIC`/`-fPIE`; `-DGCStress_EXPORTS` (§1).

- `bin/GCStress/GCStress.cpp`
- `bin/GCStress/RecyclerTestObject.cpp`
- `bin/GCStress/stdafx.cpp`
- `bin/GCStress/StubExternalApi.cpp`

## 5. Archives, link inputs and generator steps

### 5.1 Excluded from this profile

| Candidate | Verdict | Reason |
|---|---|---|
| `Chakra.Backend` (`lib/Backend`) | excluded, directory not added | `BuildJIT` false (`lib/CMakeLists.txt:3-5,86-88`) |
| `Chakra.WasmReader` (`lib/WasmReader`) | excluded, directory not added | ARM64, not AMD64 (`lib/CMakeLists.txt:7-10,95-100`) |
| `libwabt` (`lib/wabt`) | excluded, directory not added | `CAN_BUILD_WABT` unset (`lib/CMakeLists.txt:12-15,102-104`) |
| `Chakra.LTTng` | excluded | `USE_LTTNG` unset (`lib/CMakeLists.txt:17-19`) |
| `rl` (`bin/rl`) | excluded, directory never added | `bin/CMakeLists.txt` has no `add_subdirectory(rl)` |
| `jserr.gen` | listed but not compiled | `lib/Parser/CMakeLists.txt:26`; no compile-DB entry (consistent with CMake ignoring unknown source extensions) |

### 5.2 `libChakraCoreStatic.a`

`add_library(ChakraCoreStatic STATIC …)` (`lib/CMakeLists.txt:21-47`):
`ChakraCoreStatic.cpp` (§4.21) plus `$<TARGET_OBJECTS:…>` of all 20 OBJECT
groups in §4.1–4.20 (`chakra_backend_objects`, `wasm_objects`,
`wabt_objects`, `lttng_objects` all empty, `:3-19`). OSX link interface:
`-framework CoreFoundation -framework Security`
(`lib/CMakeLists.txt:56-60`); `PUBLIC` includes §3.

### 5.3 `Chakra.Pal.Singular`, `Chakra.Common.Codex.Singular`, `Chakra.Runtime.PlatformAgnostic.Singular`

STATIC archives of the corresponding OBJECT groups, created only for shared
builds (`pal/src/CMakeLists.txt:193-198`,
`lib/Common/Codex/CMakeLists.txt:7-11`,
`lib/Runtime/PlatformAgnostic/Platform/CMakeLists.txt:47-52`). Link
interface: `Chakra.Pal.Singular` carries `-framework CoreFoundation
-framework Security` (`pal/src/CMakeLists.txt:199-204`) plus `pthread dl`
(`:206-213`, `PTHREAD` set because not Android); the other two carry no
link interface.

### 5.4 Generated `DbgController.js.h`

`bin/ch/CMakeLists.txt:2-6`: `add_custom_target(dbg_controller_h COMMAND
python jstoc.py DbgController.js controllerScript` with
`WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}`. `jstoc.py:26` writes
`<basename>.h` into the CWD, i.e. `<build>/bin/ch/DbgController.js.h`, a
`static const char controllerScript[]` byte array (`jstoc.py:33-51`).
Consumed via the `<build>/bin/ch` include dir (`bin/ch/CMakeLists.txt:42`);
ordered before `ch` by `add_dependencies` (`:27`, observed as an order-only
`|| bin/ch/dbg_controller_h` dep at `build-zigpilot/build.ninja:7764`).
The custom target declares no `OUTPUT`, so the command re-runs every build;
the script itself skips the rewrite when outputs are newer (`jstoc.py:28-36`).

### 5.5 `pal/src/config.h`

Generated at configure time by
`pal/src/configure.cmake:1045` (`configure_file(…/config.h.in
<build>/pal/src/config.h)`) from `pal/src/config.h.in` (149 lines); found
via `CMAKE_INCLUDE_CURRENT_DIR` (`pal/src/CMakeLists.txt:5`). Per-value
classification (fixed / compile-checked / runtime-verified) is B1 item 2
scope and is NOT in this file. Configure-input note:
`HAVE_LIBICU_UCHAR_H` is pre-set as a plain CMake variable (not a compile
definition) at `CMakeLists.txt:598-602`, which short-circuits the
`check_include_files(unicode/uchar.h …)` probe at
`pal/src/configure.cmake:36` (cache guard `:918-919`).

### 5.6 Link: `libChakraCore.dylib`

CMake inputs (`bin/ChakraCore/CMakeLists.txt:40-71`): 3 objects (§4.23) +
`-Wl,-undefined,error` (`:46`) + `-Wl,-force_load,` split form (`:41`, with
trailing comma — Zig pilot joins it, see [build.md](build.md) §14) +
`ChakraCoreStatic` + `${ICU_LIBRARIES}` + `pthread dl` (`:51-52`).
Observed link (`build-zigpilot/build.ninja:7866-7880`):

- `LINK_LIBRARIES = -Wl,-undefined,error -Wl,-force_load,
  lib/libChakraCoreStatic.a /opt/homebrew/opt/icu4c/lib/libicuuc.dylib
  /opt/homebrew/opt/icu4c/lib/libicui18n.dylib
  /opt/homebrew/opt/icu4c/lib/libicudata.dylib -lpthread -ldl
  -framework CoreFoundation -framework Security`
  (ICU paths resolved by `find_library` at `CMakeLists.txt:227,231,243`,
  observed in `CMakeCache.txt` as `ICUUC`/`ICUIN`/`ICUDATA`; frameworks via
  §5.2).
- `LINK_FLAGS = -dynamiclib -Wl,-headerpad_max_install_names`;
  `ARCH_FLAGS = -arch arm64`; install name `@rpath/libChakraCore.dylib`
  (`INSTALLNAME_DIR = @rpath/`, `SONAME_FLAG = -install_name`) — all CMake
  Darwin builtins, no repo source lines.
- No version script on macOS (Linux/Android only, `:55-59`); default
  visibility (`CMakeLists.txt:401-404`); the `"-fvisibility=hidden"` entry
  is a bogus include dir, not a flag (§7 Q1).
- Post-build: copy to build root (`:73-80`, observed `POST_BUILD`).

### 5.7 Link: `ch`

CMake inputs, shared branch (`bin/ch/CMakeLists.txt:92-107`): 8 objects
(§4.22) + `Chakra.Pal.Singular` + `Chakra.Common.Codex.Singular` +
`Chakra.Runtime.PlatformAgnostic.Singular` + `${ICU_LIBRARIES}` (explicit,
for `u_getVersion`, `:105-106`) + `-fPIE` (`:93`; `-pie` is Linux/Android
only, `:95-97`). No link-time dependency on `libChakraCore.dylib`: `ch`
loads it at runtime — the name is the `chakraDllName =
"libChakraCore.dylib"` string literal
(`bin/ch/ChakraRtInterface.cpp:14`); the load call is
`LoadLibraryExA(libPath, nullptr, 0)` (`:41`). Observed link
(`build-zigpilot/build.ninja:7764-7780`):

- `LINK_LIBRARIES = pal/src/libChakra.Pal.Singular.a
  lib/Common/Codex/libChakra.Common.Codex.Singular.a
  lib/Runtime/PlatformAgnostic/Platform/libChakra.Runtime.PlatformAgnostic.Singular.a
  /opt/homebrew/opt/icu4c/lib/libicu{uc,i18n,data}.dylib
  -framework CoreFoundation -framework Security -lpthread -ldl`
  (frameworks/pthread/dl transitive from `Chakra.Pal.Singular`, §5.3).
- `LINK_FLAGS = -Wl,-search_paths_first -Wl,-headerpad_max_install_names
  -fPIE` (first two are CMake builtins).
- Post-build: copy to build root (`:127-135`, observed `POST_BUILD`).

### 5.8 Link: `GCStress`

CMake inputs (`bin/GCStress/CMakeLists.txt:16-48`): 4 objects (§4.24) +
`-Wl,-undefined,error` + `ChakraCoreStatic` + `dl` (`:34`) + OSX
frameworks (`:37-41`). No ICU on the link line (observed; the pulled
archive subset needs no ICU symbols — the link succeeds as specified).
Observed (`build-zigpilot/build.ninja:7603-7615`):
`LINK_LIBRARIES = -Wl,-undefined,error lib/libChakraCoreStatic.a -ldl
-framework CoreFoundation -framework Security`;
`LINK_FLAGS = -Wl,-search_paths_first -Wl,-headerpad_max_install_names`
(CMake builtins). No post-build copy (`POST_BUILD = :`; stays at
`bin/GCStress/GCStress`).

### 5.9 Staged public headers

Post-build of `ChakraCoreStatic` (`lib/CMakeLists.txt:106-122`,
non-Xcode): `make_directory <build>/include` + `copy_if_different` of
`lib/Jsrt/ChakraCore.h`, `lib/Jsrt/ChakraCommon.h`,
`lib/Jsrt/ChakraDebug.h`, `lib/Common/ChakraCoreVersion.h`.

### 5.10 Check targets (verification edges, not build inputs)

For RelWithDebInfo (`test/CMakeLists.txt:32-45`): `bytecode-check`
(`regenByteCode.py --noJit --verify --binary=<build>/ch`, `:13-18,33-38`)
and `regression-check`
(`runtests.py --test --variants=disable_jit --binary=<build>/ch`, no
`--not-tag` since ICU present, `:9-17,39-44`); `native-check` enabled
(ICU present, not embedded, `:48-55`); `check` also depends on
`ChakraCore` when shared (`:66-68`).

## 6. Validation against reference evidence

Method: `tools/build-contract/validate_contract.py` (committed in-tree)
parses every `add_library` /
`set` file list in the 23 group `CMakeLists.txt` files with a small
evaluator for the `if/elseif/else` profile conditionals
(`CC_TARGETS_ARM64`, `CC_TARGET_OS_OSX`, `BuildJIT=0`, `NO_ICU` unset,
etc.), resolves them against the group directories plus
`lib/ChakraCoreStatic.cpp`, and diffs the set against the 538 entries of
`build-zigpilot/compile_commands.json` (a configure identical to §13 of
[build.md](build.md) plus `-DCMAKE_EXPORT_COMPILE_COMMANDS=ON`;
`build-xplat/` holds no compile DB).

- Expected 538 units, DB 538 units: **0 missing, 0 extra, 0 nonexistent
  on disk.** Per-group counts: Pal 91 (67/21/3), Codex 1, Common 15,
  Core 21, DataStructures 12, Exceptions 4, Memory 41, Util 1, Jsrt 14,
  Jsrt.Core 2, Parser 26, SCACore 9, Base 34, ByteCode 20, Debug 25,
  Language 64, Library 102, Math 2, Types 27, PlatformAgnostic 11,
  ChakraCoreStatic.cpp 1, ch 8, ChakraCore 3, GCStress 4.
- Flag/define samples verified field-by-field against §1–2 (all match):
  `lib/Runtime/Library/JavascriptArray.cpp` (C++ engine unit),
  `pal/src/safecrt/memcpy_s.c` (C unit: `-O2 -g -DNDEBUG`, PAL defines, no
  `-Wno-*`),
  `lib/Common/Common/arm64/arm64_GET_CURRENT_FRAME.S` (ASM unit),
  `bin/ch/ch.cpp` (own includes + `-fPIE` + `-Dch_EXPORTS`),
  `bin/GCStress/GCStress.cpp` (transitive `ChakraCoreStatic` includes +
  bogus Q2 paths), `bin/ChakraCore/ChakraCoreShared.cpp` (own + transitive
  includes + bogus Q1 path + `-fPIC`).
- Link lines verified in `build-zigpilot/build.ninja:7603-7615` (GCStress),
  `:7764-7780` (ch), `:7866-7880` (dylib) — §5.6–5.8 quote them.
- No mismatch between CMake sources and reference evidence was found; the
  items below are quirks faithfully present in BOTH, i.e. the contract
  records the reference as-is rather than as intended.

## 7. Reference quirks (recorded as-is)

- **Q1** — `bin/ChakraCore/CMakeLists.txt:17` passes `"-fvisibility=hidden"`
  inside `target_include_directories`, producing a bogus
  `-I…/bin/ChakraCore/-fvisibility=hidden` (observed) and NO visibility
  flag. The dylib builds with default visibility.
- **Q2** — `ROOT_SOURCE_DIR` is used at `bin/GCStress/CMakeLists.txt:12-13`
  but never defined anywhere (repo-wide search: only those two uses), so
  GCStress compiles with nonexistent `-I/lib/Common -I/lib/Common/Memory`
  (observed). Harmless, and a Zig build should drop them rather than
  replicate them.
- **Q3** — `-Werror` + `-Wno-error` both on every C++ line
  (`Build/CMakeFeatureDetect.cmake:12-14,39-41`); net effect is warnings
  not errors. `-Werror` alone would only come from the Linux branch
  (`CMakeLists.txt:334-336`).
- **Q4** — C and ASM units build `-O2 -g -DNDEBUG` (CMake builtin config
  defaults; the repo overrides only `CMAKE_CXX_FLAGS_RELWITHDEBINFO` at
  `CMakeLists.txt:4`) while C++ units build `-O3 -g` with
  `ENABLE_DEBUG_CONFIG_OPTIONS=1` and no `NDEBUG`. So `NDEBUG` is defined
  for the 21 safecrt C files and 10 ASM files only.
- **Q5** — `-std=gnu++11` (`CMakeLists.txt:351`) is always overridden by
  the later `-std=gnu++17` (`CMAKE_CXX_STANDARD`, `:398-399`); likewise
  `-arch arm64` (`:309`) and `-mmacosx-version-min=10.9` (`:347`, plus the
  `CMAKE_CXX_FLAGS` copy at `:350-351`) each appear twice on the command
  line. The `-arch arm64` duplication mechanism is UNCERTAIN (see §8 U2).
- **Q6** — `HAVE_LIBICU_UCHAR_H` (`CMakeLists.txt:598-602`) is a plain
  CMake variable short-circuiting the configure probe
  (`pal/src/configure.cmake:36`), never a `-D` on any command line
  (confirmed absent in all samples).

## 8. Open uncertainties

- **U1 (ASM `-isysroot` mechanism)** — Every ASM command line (all 10)
  contains
  `-isysroot /Applications/Xcode.app/Contents/Developer/Platforms/MacOSX.platform/Developer/SDKs/MacOSX.sdk`
  (observed in `build-zigpilot/build.ninja`, e.g. line 1170, and in the
  compile DB); no C or C++ line has it, and no repo CMake line sets it
  (`CMAKE_OSX_SYSROOT` is empty in `CMakeCache.txt`). Attributed to
  CMake's Darwin ASM-language handling, but not pinned to a CMake module
  line — hence UNCERTAIN on mechanism. The value itself is machine-local
  (Xcode install path) and observed, not invented. The Zig build supplies
  its own explicit sysroot contract ([build.md](build.md) §14), so this
  affects comparison methodology, not the Zig flag set.
- **U2 (`-arch arm64` duplication mechanism)** — Every C++ command line
  carries `-arch arm64` twice (see §2.1 items 7 and 9), yet the only repo
  source line for this profile is the single
  `add_compile_options(-arch arm64)` at `CMakeLists.txt:309` (repo-wide
  search: the only other `-arch` sources are the inactive `:288` i386 and
  `:299` arm branches). Why one source line yields two occurrences is
  UNCERTAIN on mechanism. The value itself (`-arch arm64`, twice) is
  observed, not invented. (`-mmacosx-version-min=10.9` also appears twice
  but has two distinct sources — `:347` and `:350-351` — so its
  duplication is explained.)

No further uncertainties beyond U1–U2: every other value in this contract
is cited to a repo source line and, for membership/flags/links, confirmed
in the reference evidence with zero diffs.
