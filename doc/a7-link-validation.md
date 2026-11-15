# A7 link validation receipt (asm.js link + SIMD validation gates)

Worktree-only change on top of `modernize/baseline` @ `a1e1f6b1b`
(detached HEAD verified equal to the branch tip before editing).
Nothing committed. Main checkout untouched.

## Fixes (2 files, 2 lines of logic)

- A7-1 (medium) — `lib/Runtime/Language/AsmJsLink.cpp:196`:
  `CheckFFI(scriptContext, info, stdlib)` -> `CheckFFI(scriptContext, info, foreign)`.
  `CheckFFI`'s third parameter is `const Var foreign` (`AsmJsLink.h:16`,
  def `AsmJsLink.cpp:38`) and rejects non-object/static/Proxy FFI. Passing
  `stdlib` meant a non-object `foreign` escaped early rejection when the
  module has imports (fail-late at instantiation), and a Proxy `stdlib`
  with valid `foreign` was spuriously rejected.
- A7-2 (low) — `lib/WasmReader/WasmByteCodeGenerator.cpp:1640`:
  `const uint32 naturalAlignment = 16` -> `4` (+ comment). A wasm memarg
  alignment is a log2 exponent (`WasmBinaryReader.cpp:644` stores the raw
  `flags` LEB; trace print at `WasmByteCodeGenerator.cpp:174` uses
  `(1 << alignment) - 1`), and v128 natural alignment is 16 bytes =
  exponent 4. Exponents 5..16 used to validate. The scalar path is the
  precedent: `NaturalAlignment[viewType]` holds log2 exponents
  (`AsmJsTypes.h:67`, `(log2)` in `AsmJsArrayBufferViews.h`).
  Semantically inert (hints ignored); spectest-visible only.

## Coverage added (smallest maintained pin per fix)

- A7-1: `test/AsmJs/ffiLinkParam.js` + `test/AsmJs/ffiLinkParam.baseline`
  (CRLF, like `ffibug`), registered in `test/AsmJs/rlexe.xml` after
  `ffibug` with `-testtrace:asmjs`. Three link cases pin the routing:
  non-object foreign rejected early, Proxy stdlib + valid foreign links,
  Proxy foreign rejected early. The `AsmJs` suite dir is gated
  `require_backend,require_asmjs` (`test/rlexedirs.xml:300`), i.e. the
  x64 JIT CI legs. Baseline was generated from a verified-good local run
  (see below), not by analogy.
- A7-2: `test/wasm/simdMemalign.js`, registered in `test/wasm/rlexe.xml`
  after `deferred-invalid-body` with `-wasm -wasmsimd -args summary
  -endargs`. Inline hand-built modules (no checked-in `.wasm` needed):
  `v128.load` align=4 validates + instantiates, align=5 and align=16
  fail `WebAssembly.validate` and throw `CompileError` on construction.
  Byte layout independently re-parsed (section walk + body exact-match).
  Runs wherever `require_wasm` + backend legs run `test/wasm` (x64 JIT
  CI); expressible as upstream spectest once the vendored
  `test/WasmSpec/testsuite` gains SIMD align assertions (none vendored
  today — no `proposals/` dir, no simd `.wast`).

## Verification (this host, fresh `zig build -Dprofile=jit -Dwasm=true` binary)

The task premise ("no wasm/asm.js backend on this Mac") turned out to
cover only the stale prebuilt binaries: with `-Dwasm=true` the
`ENABLE_ASMJS`/`ENABLE_WASM`/`ENABLE_WASM_SIMD` path is live on ARM64
(`lib/Common/CommonDefines.h:741-751`), so full behavioral proof ran
locally on top of the build check. All builds serialized via
`/tmp/opencode/cc-gate-lock.sh`.

- `zig build -Dprofile=jit -Dwasm=true` green (final, `pipefail`-checked
  `BUILD_EXIT=0`; earlier `libtool -static` archive failures were flaky
  and environmental — same command fails and succeeds with no source
  change — and masked at first by a `| tail` pipeline swallowing the
  exit code; the archive step is unrelated to these edits).
- `test/wasm/simdMemalign.js` (`-wasm -wasmsimd`): 3/3 pass, summary
  `pass`, exit 0 on the fixed build.
- `test/AsmJs/ffiLinkParam.js` (`-testtrace:asmjs`): output byte-matches
  the checked-in baseline. (One iteration was needed: foreign imports
  only accept `extern` call args — `AsmJsImportFunction::SupportsArgCall`
  in `AsmJsTypes.cpp:925` — so the test calls with constants, and
  `new Proxy` as stdlib/foreign is what exercises the routing.)
- Negative controls on a temporarily reverted build (both bugs back):
  simdMemalign tests #2/#3 fail (`assert.isFalse`, validate returns
  true); ffiLinkParam output diverges on all three cases — case 1
  degrades to fail-late ("Foreign function import f is not a
  function"), case 2 spuriously fails ("FFI is not an object" on the
  Proxy stdlib), case 3 diverges ("Accessing foreign function import f
  has side effects"). Fixes then restored, rebuilt green, both tests
  re-verified.
- Regression spot-check on the fixed build: `ffibug.js` and
  `badFunctionType.js` baselines match; `wasm.simd` `loadTests.js` and
  `storeTests.js` print `Passed`.
- `git status`: 2 source lines, 2 new tests + 1 baseline, 2 rlexe
  entries, this receipt. No commit.

## Out of scope (deferred, not fixed)

- A7-3 limits-flags acceptance + shared->unshared silent downgrade
  (`WasmBinaryReader.cpp:1365`, `WebAssemblyModule.cpp:350`, `:731`):
  needs a wasm binary + spec tests to decide; not runnable here beyond
  what was done above, left for a follow-up with x64 CI.
