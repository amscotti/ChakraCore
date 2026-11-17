# B5: Release Packaging and Verification (`b5-package`)

This document records the architecture, directory structure, manifest schema,
deterministic archive generation, and verification tooling for ChakraCore release
packages (Wave 2 Slice B / Tier 5).

## 1. Overview and Architecture

The release packaging pipeline packages ChakraCore into self-contained, reproducible
tarballs ready for distribution on macOS and Linux. It packages the core engine, the
Node-API provider and test host launcher, standard public headers, license/notice
files, and a cryptographically verified `MANIFEST.json`.

Release packages are created from built installation prefixes (such as
`out/release-shared`, `out/test`, or `zig-out`) and validated end-to-end via
manifest checksum validation and real runtime execution of the packaged binaries.

### Supported Profiles and Targets

- **Supported Profiles**: `release-shared`, `test`, `jit` (profiles providing shared dynamic libraries).
- **Rejected Profiles**: `debug-static` (static archive builds have no shared `libChakraCore` or Node-API provider; `package` and `check-package` fail immediately with a descriptive error).
- **Supported Platforms**: macOS ARM64 / x86_64, Linux x86_64 ELF.

---

## 2. Directory and Staging Layout

The staged package directory adheres to standard UNIX prefix conventions:

```text
chakracore-<version>-<target>/
|-- bin/
|   |-- ch                          # Standalone ChakraCore engine CLI launcher (mode 0755)
|   |-- ch-nodeapi                  # Node-API host CLI launcher (mode 0755)
|   +-- libChakraCore.dylib (.so)   # Symlink -> ../lib/libChakraCore.dylib (.so)
|-- lib/
|   |-- libChakraCore.dylib (.so)   # ChakraCore shared dynamic library (mode 0755)
|   +-- libChakraNodeApi.dylib (.so)# Node-API provider dynamic library (mode 0755)
|-- include/
|   |-- ChakraCore.h                # Public JsRT C/C++ API header (mode 0644)
|   |-- ChakraCommon.h              # Public JsRT common type definitions (mode 0644)
|   +-- nodeapi/
|       |-- js_native_api.h         # Standard Node-API / JS native API header (mode 0644)
|       |-- js_native_api_types.h   # Standard Node-API types header (mode 0644)
|       |-- node_api.h              # Standard Node-API main header (mode 0644)
|       +-- node_api_types.h        # Standard Node-API types header (mode 0644)
|-- LICENSE.txt                     # Project MIT License (mode 0644)
|-- THIRD-PARTY-NOTICES.txt         # Third-party notices (mode 0644)
|-- README.md                       # Project README (mode 0644)
+-- MANIFEST.json                   # Cryptographic manifest and metadata (mode 0644)
```

### Co-Location Resolution for `ch`

The `ch` launcher dynamically loads `libChakraCore` via `GetBinaryPathWithFileNameA`
which probes the directory of the executable itself (`bin/`). To satisfy both standard
FHS layout (`lib/libChakraCore.*`) and out-of-the-box execution of `<dir>/bin/ch` without
requiring manual `DYLD_LIBRARY_PATH` or `LD_LIBRARY_PATH` overrides, the package includes
a relative symlink `bin/libChakraCore.* -> ../lib/libChakraCore.*`.

For `ch-nodeapi`, binaries are linked with `-Wl,-rpath,@executable_path/../lib` on macOS
and `-Wl,-rpath,$ORIGIN/../lib` on Linux, resolving libraries in `lib/` directly through
dynamic linker search paths.

---

## 3. Manifest Schema (`MANIFEST.json`)

Every package root includes a `MANIFEST.json` recording build metadata and the exact
file inventory with cryptographic checksums:

```json
{
  "version": "1.13.0.0-beta",
  "target": "darwin-arm64",
  "platform": "darwin-arm64",
  "git_commit": "8b073a4c3f59e0a0a811c0f1b212f46ee6c98ee4",
  "commit": "8b073a4c3f59e0a0a811c0f1b212f46ee6c98ee4",
  "timestamp": "2023-11-14T22:13:20+00:00",
  "files": [
    {
      "path": "bin/ch",
      "size": 258432,
      "sha256": "4b3e8e..."
    },
    {
      "path": "bin/ch-nodeapi",
      "size": 115296,
      "sha256": "8f2a1b..."
    },
    {
      "path": "lib/libChakraCore.dylib",
      "size": 15420160,
      "sha256": "a3b94c..."
    },
    {
      "path": "lib/libChakraNodeApi.dylib",
      "size": 384512,
      "sha256": "d4e21a..."
    },
    {
      "path": "include/ChakraCore.h",
      "size": 39502,
      "sha256": "9c12f0..."
    }
  ]
}
```

The fields are:
- `version`: Engine release or build version (from `CHAKRA_CORE_VERSION` or `git describe`).
- `target` / `platform`: Target tuple (`<os>-<arch>`).
- `git_commit` / `commit`: Full git commit hash of the source tree.
- `timestamp`: ISO-8601 UTC timestamp (derived deterministically from `SOURCE_DATE_EPOCH`).
- `files`: Sorted array of file records containing `path` (forward-slash normalized relative path), `size` (bytes), and `sha256` (hex-encoded SHA-256 digest).

---

## 4. Deterministic Tarball Generation

Packages are generated as gzip-compressed GNU tarballs: `chakracore-<version>-<target>.tar.gz`.
Deterministic reproducibility across runs and environments is achieved by:

1. **Deterministic Ordering**: All files and directories are added sorted alphabetically by relative path.
2. **Fixed Source Epoch**: Tar headers and `MANIFEST.json` timestamps use `SOURCE_DATE_EPOCH` (or the pinned default epoch `1700000000`, 2023-11-14T22:13:20Z) rather than the build wall-clock time.
3. **Normalized Ownership**: User ID `0`, Group ID `0`, user name `"root"`, group name `"root"`.
4. **Standardized File Modes**: `0755` for directories, executable binaries, and shared libraries; `0644` for headers, text files, and the manifest.
5. **Gzip Header Normalization**: Gzip mtime clamped to `SOURCE_DATE_EPOCH`, empty embedded header filename.
6. **Symlink Preservation**: Staged relative symlinks are added as `SYMTYPE` entries without dereferencing or duplicating target byte blocks.

Independent runs with identical inputs produce bit-for-bit identical SHA-256 archive digests.

---

## 5. Tooling (`tools/package/`)

### 5.1 `build_package.py`

Constructs the staging directory, computes the manifest, and packs the deterministic tarball.

```bash
python3 tools/package/build_package.py --prefix <install_prefix> --out-dir out/package [--version <ver>] [--target <target>]
```

Options:
- `--prefix`: Source directory containing built artifacts (e.g. `zig-out` or `out/release-shared`).
- `--out-dir`: Destination directory for the generated tarball and staged package (defaults to `out/package`).
- `--version`: Package version string (auto-detected if omitted).
- `--target`: Target platform identifier (auto-detected if omitted).

### 5.2 `verify_package.py`

Validates a package directory or tarball against its `MANIFEST.json`.

```bash
python3 tools/package/verify_package.py out/package
# OR directly against a tarball:
python3 tools/package/verify_package.py out/package/chakracore-1.13.0.0-beta-darwin-arm64.tar.gz
```

Verification assertions:
- Complete `MANIFEST.json` schema validation.
- Every manifest file exists in the package.
- File size matches manifest bytes exactly.
- File SHA-256 matches manifest digest exactly.
- Executable permissions (`0755`) are enforced on all files in `bin/` and `lib/`.
- No missing files and no unexpected extraneous files.

### 5.3 `test_package.py`

Performs end-to-end integration and smoke execution of the packaged binaries extracted into an isolated temporary directory with `DYLD_LIBRARY_PATH` and `LD_LIBRARY_PATH` explicitly unset:

```bash
python3 tools/package/test_package.py out/package
```

Tests performed:
1. `<dir>/bin/ch --version`: Exits 0 and outputs the ChakraCore version string.
2. `<dir>/bin/ch` snippet execution: Runs `console.log('package smoke')`, asserting exit code 0 and stdout content.
3. `<dir>/bin/ch-nodeapi` dynamic resolution: Executes `ch-nodeapi` with the compiled Node-API addon (`hello_world.node`) and driver script (`hello_world.js`), verifying dynamic loading of `libChakraCore` and `libChakraNodeApi` via `@executable_path/../lib` on macOS and `$ORIGIN/../lib` on Linux without dynamic linker failures.

### 5.4 `test_package_tools.py`

Hermetic unit test suite verifying packaging logic and negative controls:
- Staging directory structure and permission modes.
- Manifest schema validation and entry sorting.
- Deterministic archive bit-reproducibility across runs.
- Detection of corrupted or tampered file hashes.
- Detection of corrupted file sizes.
- Detection of missing files.
- Detection of unexpected/extraneous files.
- Detection of incorrect permission bits.
- End-to-end CLI roundtrip and smoke runner execution.

Run unit tests via:
```bash
python3 tools/package/test_package_tools.py
```

---

## 6. Build Integration (`build.zig` and `tools/build/linux.zig`)

### Build Steps

- `zig build package`: Builds the runtime, Node-API provider, and test host, then generates the release package in `out/package`.
- `zig build check-package`: Builds and packages artifacts, verifies the archive against `MANIFEST.json`, and executes end-to-end smoke tests on the packaged binaries.

### Running the Steps

```bash
# Package the default profile (test)
mise exec -- zig build package

# Run package build + manifest verification + smoke tests
mise exec -- zig build check-package

# Package and check release-shared profile
mise exec -- zig build -Dprofile=release-shared check-package

# Verification on debug-static fails cleanly as expected:
mise exec -- zig build -Dprofile=debug-static check-package
# error: check-package requires a shared profile (test, jit, or release-shared) with dynamic libraries
```
