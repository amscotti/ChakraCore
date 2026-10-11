# B1 verification record

B1 step 4 — repeatable verification for the Mac reference profile
([modernization-plan.md](modernization-plan.md) §B1 "Done when",
[build-contract-mac.md](build-contract-mac.md),
[pal-config-contract-mac.md](pal-config-contract-mac.md) §5).

Date: 2026-10-11 (run at 01:02 EDT). Branch: `modernize/baseline`.
Verifier: `tools/build-contract/verify_b1.py` (run end-to-end; result
15/15 PASS, exit 0). Evidence log: `tools/build-contract/verify_b1.log`
(git-ignored `*.log`; regenerable by re-running the script).

## Toolchain

All invocations use ONLY mise zig 0.17.0 (`mise exec -- zig ...`);
PATH zig (0.16.0) is never used.

- `mise exec -- zig version` → `0.17.0`
- `mise exec -- zig env` → `.target = "aarch64-macos.26.6...26.6-none"`,
  `.zig_exe = "/Users/ascotti/.local/share/mise/installs/zig/0.17.0/zig"`
  (full output in the evidence log)
- SDK: Xcode `MacOSX.sdk` 26.5 at the `xcrun --show-sdk-path` location
- ICU: Homebrew `U_ICU_VERSION "78.3"`
- Host: macOS 26.6, Apple M1 Max

## Commands

```sh
python3 tools/build-contract/verify_b1.py          # full run (exit 0)
python3 tools/build-contract/verify_b1.py --log PATH  # alternate log path
```

The script wipes `.zig-cache/zig-out`, runs `mise exec -- zig build`,
asserts the generated `config.h` MD5 against the value pinned in both
contracts (`855152b0a1b6da1d8abfef52e1f45f4e`) and byte-compares with
`cmp` against the git-ignored `build-zigpilot` reference when it exists
(otherwise it logs `ref absent — MD5-only` and passes on MD5 alone),
lists members with `ar t`, and runs the fail-clearly subset, each as an
exact recorded command (see the log).

## Results

| Check | Expected | Observed |
|---|---|---|
| toolchain | mise zig 0.17.0 | 0.17.0 |
| clean-build | exit 0 | exit 0 in 25.9s |
| pal-config-checks | 95/95 checks match | `pal-config: 95/95 checks match the contract (33 fixed-by-target, 62 compile-checked, 32 runtime-verified)` |
| config.h-md5 | MD5 `855152b0a1b6da1d8abfef52e1f45f4e` (both contracts) | MD5 `855152b0a1b6da1d8abfef52e1f45f4e` |
| config.h-cmp | `cmp` clean vs `build-zigpilot` ref | identical; ref MD5 `855152b0a1b6da1d8abfef52e1f45f4e` |
| archive-objects | exactly the 91 §4.1 objects + only `__.SYMDEF` | 91 .o match (§4.1 parses to 91 files → 91 unique names; `ar t` shows 92 members = 91 .o + `__.SYMDEF`; non-`.o` asserted equal to `['__.SYMDEF']`) |
| fail:missing-ICU-header | nonzero + names missing header | exit 1, `Cannot find ICU (missing .../include/unicode/uchar.h). Try installing libicu-dev ...` (empty fake tree via `-Dicu-include`, 0.2s) |
| fail:wrong-arch-target | nonzero + names arch | exit 1, `unsupported arch x86_64: this profile builds only aarch64-macos.26.0` (0.2s) |
| fail:wrong-minimum-target | nonzero + names minimum | exit 1, `unsupported macOS minimum 25.0.0: this profile builds only 26.0` (0.2s) |
| fail:cpu-override | nonzero + names override | exit 1, `unsupported -Dcpu=native: this profile pins the apple_m1 CPU model; -Dcpu overrides are rejected` (0.2s) |
| fail:optimize-override | nonzero + names mode | exit 1, `unsupported -Doptimize=debug: this profile (RelWithDebInfo Test, -O3 -g) supports only fast` (0.2s) |
| fail:wrong-OS-target | nonzero + names OS | exit 1, `unsupported OS linux: this profile builds only aarch64-macos.26.0` (`-Dtarget=aarch64-linux-gnu`, 0.2s) |
| fail:missing-sdk-dir | nonzero + names missing SDK | exit 1, `SDK not found: /nonexistent/MacOSX.sdk does not exist; pass -Dsdk-path=<MacOSX.sdk>` (0.2s) |
| fail:wrong-sdk-version | nonzero + names versions | exit 1, `unsupported SDK <fake>/sdk: this profile requires MacOSX SDK 26.5 (have 15.0)` (fake dir with `SDKSettings.json` `{"Version": "15.0"}`, 0.2s) |
| fail:missing-icu-libs | nonzero + names missing lib | exit 1, `Cannot find ICU libraries (missing .../include/../lib/libicuuc.dylib). Try installing libicu-dev ...` (fake tree with `include/unicode/uchar.h` but no `lib/`, 0.2s) |

No check failed. Success build line (from the clean-build output):

```text
b1 profile: target=aarch64-macos.26.0 sdk=/Applications/Xcode.app/Contents/Developer/Platforms/MacOSX.platform/Developer/SDKs/MacOSX.sdk@26.5 icu-include=/opt/homebrew/opt/icu4c/include zig=0.17.0
```

## Fail-clearly matrix disposition

Script-run (asserted on every run): missing ICU header, wrong-arch
target, wrong-minimum target, `-Dcpu` override, `-Doptimize` override,
wrong-OS target, missing SDK dir, wrong SDK version, missing ICU
libraries (see table above).

N/A: no such option exists in the B1 slice — JIT-mode: the B1 slice
pins `-DDISABLE_JIT=1` with no JIT-mode option, so there is no JIT-mode
combination for the §5 item-1 target gate to reject.

Manually verified during review (same machine/date, `mise exec -- zig
build ...`; outputs captured to `/tmp/b1-*.log`, quoted verbatim):

- `-Dtarget=native`: exit 1, `unsupported CPU model apple_a14: this
  profile builds only apple_m1 (the aarch64-macos baseline)` — native
  resolves to apple_a14 even on this Apple M1 Max host, so it is
  rejected (see known behaviors).
- PATH zig 0.16.0 (`/opt/homebrew/bin/zig build`): exit 2,
  `build.zig:50:41: error: switch on struct with auto layout` — the
  build script itself no longer compiles under 0.16.0, so the script
  pins and asserts mise zig 0.17.0 up front instead of relying on the
  in-build version gate.

Code-reviewed, not executed: non-ARM64-macOS build host
(`gateToolchain`), non-`none` ABI (`gateTarget`), unreadable/unparseable
`SDKSettings.json` (`gateSdk` — same fail site as the executed SDK
cases), config-value deviation (`tools/pal-config/check.zig` fail path —
requires an environment/SDK change to trigger).

Deliberately not run: unparseable `-Dtarget` (garbage string) — see
known behaviors.

## Known behaviors

- Unparseable-target probe run: an unparseable `-Dtarget` does NOT fail
  at the target gate. `standardTargetOptions` marks the input invalid
  but returns the default query, so all 95 probes run (~30s wasted)
  before Zig's deferred option validation fails the build; the exit
  code stays correct (1). Documented in the `build.zig` gateTarget
  comment; the script skips this case to keep runtime sane.
- `-Dtarget=native` is rejected on hosts where native does not resolve
  to apple_m1 — observed here as apple_a14 on an Apple M1 Max host
  (exit 1, message above). Only the literal default target
  (aarch64-macos.26.0/apple_m1) is accepted.

## B2 transition note (2026-10-11)

The B1 results recorded above are the historical B1 record and are
unchanged. `tools/build-contract/verify_b1.py` is now B2-aware in one
check only: `archive-objects` accepts EITHER the B1 artifact
(`zig-out/lib/libChakra.Pal.a`, `stem.o` members) OR the B2 artifact
(`zig-out/lib/libChakra.Pal.Singular.a`, `NNNN-basename.o` members with
the source extension kept); both require exactly the 91 §4.1 objects
plus only `__.SYMDEF`, and the B2 branch additionally requires every
member to carry the `NNNN-` prefix. All other checks (toolchain,
clean-build, pal-config-checks, config.h-md5/cmp, the nine fail-clearly
cases) are byte-for-byte the B1 checks. Rationale: the B2 build graph
no longer produces `libChakra.Pal.a`, so the unmodified check fails
14/15 on a B2 tree for artifact-name reasons only; the B2-aware form
restores the script as a regression gate on both trees.
