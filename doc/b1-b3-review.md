# B1-B3 review receipt

Reviewed on 2026-10-12, ARM64 macOS, Zig 0.17.0, SDK 26.5, system ICU 78.3:

- B1: `5e8c6f6dd0868f93367d0510f02acf24f8297651`
- B2: `c3d11e7db` (source-only build and checks)
- B3: `8bc30f0aa` (profiles, incremental checks and CI)

## Findings and fixes

| Finding | Correction |
|---|---|
| Clean CI expected `15/15`, but the optional, git-ignored CMake reference makes a clean checkout's tally `14/14` | `verify_b1.py` emits a final machine verdict; the workflow checks it and records the actual tally |
| B1 verification deleted the live cache/install tree, racing other profile builds | Fresh private `--cache-dir` and `--prefix`; only verifier-owned outputs are cleaned up |
| `touched.__exit__` returned true after restoration, suppressing exceptions and allowing false-green incremental results | Restore bytes/mtime, validate restoration, propagate the original exception |
| Bytecode verification rewrote source headers temporarily; interruption could leave them changed | Run the original regeneration script against a private copy of its inputs and an absolute built-shell path |
| Timeout helpers killed only the parent, could leave workers running, hang on inherited pipes, and omit reaping/closing | Private process groups, an output-drain deadline, group termination/reaping on failure or interruption; B1/incremental/CI capture helpers reuse this lifecycle |
| Native samples could print `SUCCESS`, exit nonzero and still pass | `test_native.sh` checks sample exit status; the Python gate requires exact suite/skip lines and rejects assertion diagnostics |
| Property-symbol sample treated deletion's Boolean result as the previous property value; property assertions only printed errors | Consume `JsObjectDeleteProperty` through `JsBooleanToBool`; assertion failures exit nonzero |
| External native sample allocated `size_t[123]` with `new[]` and finalized it with scalar `delete` | Pair the allocation with `delete[]` |
| PAL probes inherited the checkout CWD; an existing `a_nonexistent_file` changed the realpath verdict | Native probes run in their fresh private directory; close the directory handle used for cleanup |
| Source-contract validator could print mismatches and exit zero; reported overlapping directory counts | Fail on mismatches, compare Zig tables too, and report exact source-group counts |
| Side-prefix installer accepted equal bytes with a missing executable bit | Repair permissions without recopying unchanged bytes |
| Valid JSON with a non-object SDK root panicked; foreign object formats could reach later build stages | Explicit descriptive SDK-root and Mach-O target gates before probing |

The failure controls live in `tools/build/test_build_tools.py` and are included
in `zig build check-selftest`. `zig build check-build-tools` runs them separately.

## Coverage actually exercised

| Command / experiment | Result |
|---|---|
| `python3 tools/build/test_build_tools.py` | 22 tests passed: missing/local/mismatched B1 reference, preservation of live profile state, bad SDK/object format, missing compile DB members, exception propagation, executable-mode repair, native false-pass controls, bytecode success/repeated failure/interruption, and descendant/pipe timeout controls |
| `python3 tools/build-contract/validate_contract.py` | CMake/compile DB/Zig source unions all 538; zero missing/extra/nonexistent files |
| B1 verifier in a fresh local clone without `build-xplat/` or `build-zigpilot/` | 14/14; clean source-only build, config MD5 `855152b0a1b6da1d8abfef52e1f45f4e`, 91 PAL objects |
| B1 verifier with final private-cache/prefix mechanism on the working checkout | 15/15 including optional reference comparison; clean build 184.8s; concurrent debug-static metadata gate passed |
| `mise exec -- zig build check check-selftest --summary all` | Test regression 1959/0; bytecode, shared embedding, smoke, metadata and self-tests green |
| Fresh-clone `mise exec -- zig build check check-selftest --summary line` | 1959/0 plus all Test checks; no CMake/Ninja or legacy build artifacts |
| `mise exec -- zig build check -Dprofile=debug-static --summary line` | 2011/0 plus bytecode, static native, smoke and metadata |
| `mise exec -- zig build check -Dprofile=release-shared --summary line` | Release hello/version/flag/snippet, shared native and metadata checks green |
| Debug-static `check-native` after the native-sample corrections | All eight static samples pass; the old property-symbol diagnostic is gone |
| Updated native gate against `build-legacy-debug/ch` and its static archive | All eight static samples pass with the same assertion/finalizer corrections |
| `python3 tools/build/test_incremental.py --quick` | 0 failures; isolation, deleted-artifact recovery, no-op zero TU/byte changes, build-comment and archive-selection checks |
| `python3 tools/build/test_incremental.py --quick concurrent` | Test and debug-static no-op builds both green concurrently |
| Realpath negative control: temporarily create `a_nonexistent_file` in the checkout | Original probe failed with a false deviation; private-CWD implementation passed 95/95 with the same fixture |
| `zig fmt --check`, `actionlint` (ShellCheck integration disabled), Bash syntax, copyright and whitespace checks | Passed on the edited code/workflow; root `build.zig` also satisfies the ASCII source policy |

Regression counts and full-profile checks were exercised before the final
private-B1/output-format changes; their added behavior was then covered by the
focused controls and a complete private B1 build. Subsequent native-sample and
process-lifecycle changes were rechecked through static native/self-test gates.
The full source/header/jstoc/config incremental matrix was not rerun; the quick
and concurrent subsets above are the coverage added by this review.

## Ownership / lifecycle receipt

- **Child processes and scratch outputs:** the synchronous helper owns the
  freshly created POSIX process group and pipes. Keep its leader unreaped while
  draining the pipe, so the group identity cannot be reused before timeout
  cleanup. Terminate/reap on timeout or interruption before releasing temporary
  directories. B1 owns only its private cache/prefix; bytecode checks own their
  copied script/JS/headers. The controls exercise inherited pipes after parent
  exit and an interrupted writer without altering original headers.
- **Deletion result:** a runtime-owned `JsValueRef` Boolean, borrowed on the
  native C++ stack and immediately converted in the active context. It is not
  native memory to free. Contract/implementation: `ChakraCore.h`'s
  `JsObjectDeleteProperty` and `Jsrt.cpp`'s `JsDeletePropertyCommon`.
- **External sample array:** host C++ `new size_t[123]` owns the physical base
  and extent; successful external-object creation registers the sole finalizer
  release path. `JsrtExternalObject::Create` stores the pointer and
  `Finalize` invokes the callback; runtime disposal in this sample triggers it.
  The callback checks both sentinels and now uses the matching `delete[]`, then
  emits the final `CCESS` of the sample's `SUCCESS` witness. This successful
  finalization path was exercised with both Zig and legacy Debug/static engines;
  allocation/registration failure, concurrent GC and sanitizer instrumentation
  were not exercised by this release-operator correction.

B3's hosted-CI clause remains conditional on the first green Actions run. Local
execution and workflow lint establish the evidence above, not runner-image reality.
