# B6 Windows native bring-up — first-run fix log

Branch: `b6/windows-native-bringup`, forked from `modernize/phase4` @ `17e043056`.

This log records every source/build change made during the **first native
Windows x64 compile** of the `zig build` build graph, along with the error
seen, the root cause, why neither the macOS cross-check nor the legacy MSBuild
caught it, and the fix (with the codebase or platform precedent for each).

## Build environment

Windows 11 Enterprise Evaluation x64 (build 26100, "throwaway" test VM),
Visual Studio Build Tools 2022 (MSVC 14.44.35230, SDK 10.0.26100.0),
Zig 0.17.0, `zig build` only (no MSBuild/CMake on the native path).

## Fixes

### 1. `test_build_tools.py` / `test_package_tools.py` — POSIX-only test assumptions

**Error:** `test_build_tools.py` 4 failures (108 tests), `test_package_tools.py` failures.
**Root cause:** tests assumed POSIX semantics — Unix exec-bit/`0755` permission
checks on installed payloads, and fixture binaries with `#!/bin/sh` shebangs.
Windows has no exec-bit model, and the shell-script fixtures cannot run there.
**Why missed:** tests were authored/verified on macOS only.
**Fix:** `@unittest.skipUnless(os.name == "posix", ...)` guards, matching
existing patterns in the same files. Windows CI still exercises the Windows
branches of the tools.

### 2. `tools/build/check_metadata_linux.py` — self-test fixture SHA-256 mismatch

**Error:** `test_self_test_exits_zero` — `AssertionError: 1 != 0 : check_metadata_linux: elf...`
**Root cause:** the self-test wrote a fixture text blob with `io.open(...)`
(text mode); on Windows `\n` became `\r\n`, changing the SHA-256 under test.
**Why missed:** macOS `open()` does no newline translation.
**Fix:** binary-mode write for the fixture.

### 3. `tools/build/check_metadata_linux.py` / `check_nodeapi.py` — missing Unix tools

**Error:** `check_metadata_linux.py check_archive` raised `FileNotFoundError`
instead of returning a diagnostic; `check_nodeapi.py` same for `nm`.
**Root cause:** `ar`/`nm` are Unix tooling; `subprocess` raises OSError when
absent (Windows). The expected `RuntimeError` path was never reached.
**Why missed:** macOS always has `ar`/`nm`.
**Fix:** `try/except OSError` converts to the documented `RuntimeError`
("ar t ... failed ..."), preserving the gate's negative-control semantics.

### 4. `tools/build/tables_windows.zig` + `windows.zig` — SDK `winrt\` include dir

**Error:** 411 × `Windows.Globalization.h not found`.
**Root cause:** SDK 26100 ships `Windows.Globalization.h` under
`Include/10.0.26100.0/winrt/`; the probe confirmed Zig 0.17's `zig c++`
include search omits `winrt\` (it adds `um\`, `shared\`, `ucrt\`).
**Fix:** `windows.zig` adds the `winrt` dir when present (same fallback chain
style as the existing `um`→`shared` fallback). No header copying.

### 5. `lib/Common/CommonPal.h` — `_NOEXCEPT_` redefinition (511 errors)

**Error:** warning-as-error: `_NOEXCEPT_` redefined.
**Root cause:** Zig's libc++ headers define `_NOEXCEPT_` as a *function-like*
macro; the ChakraCore codebase uses `_NOEXCEPT_` as a *bare object-like*
macro identifier in type-trait contexts everywhere. The two collide.
**Why missed:** macOS builds use Apple's libc++ (no function-like define) and
MSVC's CRT never defines `_NOEXCEPT_` at all.
**Fix:** `#undef _NOEXCEPT_` immediately before the codebase's object-like
redefinition. Renaming would touch hundreds of sites; guarding one definition
is the minimal fix.

### 6. `lib/Common/Memory/DelayDeletingFunctionTable.cpp` — `JITClient.h` unguarded

**Error:** 137 × `ChakraJIT.h not found` (via `JITClient.h`).
**Root cause:** the include was unconditional, but the use is guarded by
`#if defined(_M_ARM)` (OOP-JIT presence check). `ChakraJIT.h` is
MIDL-generated, and `zig build` never runs `midl.exe`, so it does not exist.
**Why missed:** macOS builds use the non-MIDL path; MSBuild runs MIDL first.
**Fix:** guard the include with the same `#if defined(_M_ARM)` as the use
site. Zero behaviour change on ARM64 (still guarded off under the
`-DENABLE_OOP_NATIVE_CODEGEN=0` profile this build uses).

### 7. `tools/build/tables_windows.zig` — `-Wno-nonportable-include-path`

**Error:** 752 errors: SDK `winrt\` headers include each other with
different-case names (`Windows.Globalization.h` vs `windows.globalization.h`),
fatal under `-Werror`.
**Root cause:** the SDK headers themselves, not our code. Windows file
lookups are case-insensitive, so this is benign on Win32.
**Fix:** narrow `-Wno-nonportable-include-path` suppression alongside the
existing `-Wno-*` list (Build/Chakra.Build.Clang.props parity).

### 8. `lib/Parser/RegexUnicodeProperty.h` — ICU `UChar` is now `char16_t`

**Error:** `uset_applyPropertyAlias(set, _u("Script_Extensions"), ...)` —
"no known conversion from `const wchar_t[18]` to `const UChar*`
(aka `const char16_t*`)"; and `collector->Add(str, ...)` — `UChar[24]` to
`const char16*`.
**Root cause:** ICU 73.2+ defines `UChar` as `char16_t` on all platforms.
On Windows, ChakraCore's `_u(s)` is `L##s` (`wchar_t`) and `char16` is
`WCHAR` = `wchar_t` — a distinct type from `char16_t`, no implicit conversion.
**Why missed:** on macOS/Linux, `_u(s)` is `u##s` (`char16_t`), matching ICU.
**Fix:** `reinterpret_cast<const UChar*>` / `reinterpret_cast<const char16*>`
at the 4 call sites — the exact pattern already used throughout the Intl
code (`IntlListFormat.cpp`, `IntlDurationFormat.cpp`, `IntlSegmenter.cpp`).

### 9. `lib/Runtime/Library/JavascriptPromise.h` — comma inside `Field()` macro

**Error:** `Field(JsUtil::List<const PropertyRecord*, Recycler>*)` — "expected
'>'" / "non-friend class member 'List' cannot have template parameters".
**Root cause:** `Field(type, ...)` is variadic, and `GLOBAL_ENABLE_WRITE_BARRIER`
differs by platform: on Windows (0) the macro is `type` (discards `...`), so
the comma inside `List<..., Recycler>` splits the argument. On macOS/Linux
(1), the macro reassembles `FieldWithBarrier(type, ##__VA_ARGS__)` and the
comma is harmless — that is why macOS passed.
**Why missed:** first Windows compile; the lines were added in modernize
phase work and only ever built on macOS/Linux.
**Fix (attempted first, reverted):** `Field((T*))` parenthesization broke
decl-specifier parsing under clang-cl. **Final fix:** typedef aliases
(`PropertyRecordList*` / `VarList*`), matching the codebase precedent
`SpreadArgument.h:12-13`.

### 10. `lib/Runtime/PlatformAgnostic/` — explicit instantiation before definition

**Error:** `UnicodeTextInternal.h:13: error: explicit instantiation of
undefined function template 'ChangeStringLinguisticCase'`.
**Root cause:** `UnicodeTextInternal.h` (included at the top of each
implementation `.cpp`) contained explicit instantiation *definitions*, but the
template definitions appear later in those same files. The C++ standard
requires the definition to be visible first; **MSVC tolerates instantiation
before definition as a non-standard extension**, clang rejects it.
**Why missed:** only ever compiled by MSVC (legacy) — the macOS "verification"
ran Python gates, not a full C++ build.
**Fix:** header now declares `extern template` (valid without a visible
definition); the 4 instantiation definitions moved into each implementing
`.cpp`, immediately after the template definition. A follow-up correction:
in `Windows/UnicodeText.cpp` the template definition lives inside an
`#ifndef HAS_ICU` block, so the instantiations were initially appended at EOF
— which fails when `HAS_ICU=1` (no visible definition). They were moved
inside that guard, just before `#endif // HAS_ICU`.

### 11. 5 Intl files — `PAL_wcslen` / `PAL_wcscmp` do not exist on Windows

**Error:** `use of undeclared identifier 'PAL_wcslen'` in `IntlModernShared.cpp`
(11 sites), `IntlDisplayNames.cpp`, `IntlDurationFormat.cpp`,
`IntlEngineInterfaceExtensionObject.cpp`, `IntlSegmenter.cpp`; same for
`PAL_wcscmp` in `IntlDurationFormat.cpp`, `IntlListFormat.cpp`,
`IntlModernShared.cpp`, `IntlRelativeTimeFormat.cpp`.
**Root cause:** `PAL_wcslen`/`PAL_wcscmp` live in `pal/inc/pal.h`, which
`CommonPal.h` includes only in the non-Windows (`#else`) branch. These files
were added in the modernize branch and verified only on the PAL path.
**Fix:** `PAL_wcslen` → `wcslen`, `PAL_wcscmp` → `wcscmp`. Valid on both
platforms: `pal.h` defines `wcslen`/`wcscmp` *as* `PAL_wcslen`/`PAL_wcscmp`,
and on Windows they are the CRT functions (`Common.h` includes `<wchar.h>`
when `USING_PAL_STDLIB` is off).

### 12. `tools/build/windows.zig` — `-DNOMINMAX`

**Error:** bursty `min`/`max` macro collision diagnostics (masked initially).
**Root cause:** MSVC's `windows.h` maps `min`/`max` to macros unless
`NOMINMAX` is defined; zig's compile line previously omitted it.
**Fix:** `-DNOMINMAX` in `engine_base_defines` (mirrors the legacy
Chakra.Build.Clang.props define).

### 12b. `lib/Common/CommonPal.h` — `rand_s` undeclared on Windows

**Error:** `lib/Backend/Encoder.cpp:92: error: use of undeclared identifier
'rand_s'`.
**Root cause:** UCRT `stdlib.h` declares `rand_s` only when `_CRT_RAND_S` (or
`_CRTBLD`) is defined before `stdlib.h` is first included (SDK stdlib.h:354).
`CommonBasic.h:9` defines `_CRT_RAND_S`, but only *after* `Backend.h` has
already included `<wchar.h>` — and with zig clang, libc++'s wrapper headers
shift `stdlib.h` inclusion ordering vs MSVC, so the gate can be missed.
The legacy MSVC build got away with it because include ordering there happened
to reach `CommonBasic.h` first; nothing guarantees the order on first-run
clang builds.
**Fix:** declare `errno_t rand_s(unsigned int*);` explicitly in the `_WIN32`
branch of `CommonPal.h` (after `windows.h`). This mirrors the identical
declaration already present in the non-Windows branch (CommonPal.h:454).
A same-signature redeclaration from the CRT is legal C++, and on x86_64
`__cdecl` is the default calling convention, so there is no MSVC conflict.

### 13. `tools/build/tables_windows.zig` — `-Wno-unused-command-line-argument`

**Error:** `clang: warning: '-nostdinc++': unused argument; treating as
error` (warnings-as-errors).
**Root cause:** `zig c++` injects `-nostdinc++` for the MSVC target; the
bundled clang reports it as unused, and `-Werror` escalates it in every TU.
**Fix:** one-line suppression, same list as fix 7.

### 14. `tools/build/windows.zig` — `-nostdlib++` on all `zig c++` link/compile invocations

**Error:** after all 606 TUs compiled, the link of `ChakraCore.dll`/`GCStress.exe`
failed: `vcruntime_typeinfo.h:137: error: target of using declaration conflicts
with declaration already in scope` via `C:\tools\zig\lib\libcxx\include/typeinfo`.
**Root cause:** Zig's `c++` driver, when linking C++ for `windows-msvc`, compiles
its bundled **libcxxabi runtime from source** (`cxa_aux_runtime.cpp` et al.)
to link into the target. Those sources include Zig's libc++ headers, and MSVC
CRT headers (present in the same search paths) then declare `::type_info`
twice — a hard conflict. Nothing here needs Zig's libc++ at all: every object
was compiled against MSVC CRT headers with `-fms-runtime-lib=static`.
**Why missed:** first Windows link of any kind.
**Fix:** add `-nostdlib++` to every `zig c++` invocation in `windows.zig`
(8 sites: dll/ch/gcstress/prov/nodeapi-host/stub/addon links + nodeapi TU
compiles). Probes confirmed this both skips the libcxxabi build AND makes
`zig cc`/link equivalent behavior match (`zig cc` alone still drags in a
`libc++.lib` link reference; `-nostdlib++` is the suppressor that works).
Documents the same failure mode for the earlier bare probe (libcxxabi +
vcruntime conflict reproduced with plain `zig c++ -target x86_64-windows-msvc`).

### 15. `tools/build/windows.zig` — `-nostdlib++` on the TU compile path too

**Error:** with fix 14, links progressed past libcxxabi but then failed with
`lld-link: error: could not open 'libc++.lib': No such file or directory`.
**Root cause:** every already-cached `.obj` carries a baked-in
`/DEFAULTLIB:libc++.lib` directive (confirmed with `dumpbin /DIRECTIVES`).
Fix 14 covered only the *link* invocations — the main TU compile path
(`addCompileCommand`, line 616, using `ctx.zig_exe`) still compiled without
`-nostdlib++`, and Zig's clang emits the directive for C++ objects unless told
not to link its bundled libc++. The objects reference zero libc++ symbols
(MSVC-mangled only; verified with `dumpbin /SYMBOLS`), so the directive is
purely vestigial — but lld-link hard-errors on a missing default lib.
**Why missed:** fix 14's grep covered only `b.graph.zig_exe` invocations, not
the `ctx.zig_exe` compile path.
**Fix:** add `-nostdlib++` for `.cxx` in `addCompileCommand` (same patch style
as fix 14), wipe `.zig-cache` (all cached objects are poisoned), rebuild.
A stub-lib workaround (empty `libc++.lib` via `-L`) was verified to link but
is rejected: it would mask genuine libc++ references if any future TU ever
actually needed them, silently, instead of failing loudly.


## Retained / verified non-changes

- Bytecode-header purity: `JnDirectFields.h`, `InternalPropertyList.h`,
  `Intl.js`, `JsBuiltIn*.bc.*.h` — 0 diff vs `modernize/baseline`; will be
  re-verified with `verify-bytecode` at Step 3.
- No MSBuild/CMake introduced; everything remains `zig build`.
- No pushes; local-only branch pending PR.
