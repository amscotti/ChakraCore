#!/bin/sh
#-------------------------------------------------------------------------------------------------------
# Copyright (c) ChakraCore Project Contributors. All rights reserved.
# Licensed under the MIT license. See LICENSE.txt file in the project root for full license information.
#-------------------------------------------------------------------------------------------------------
# N4.4 async work + TSFN + uv probe runner.
#
# Builds async_probe.cpp + the reconciled provider TUs against EACH
# profile's engine dylib and runs it. Both profiles must pass:
#   test  (default)   -> zig-out/bin/libChakraCore.dylib
#   jit               -> out/jit/bin/libChakraCore.dylib
#
# Zig invocations run directly via mise/zig (optionally serialized if
# CC_GATE_LOCK points to an executable gate-lock wrapper script).
#
# Env overrides:
#   CC_GATE_LOCK=path  Optional gate-lock wrapper script (disabled by default)
#   SKIP_BUILD=1       reuse already-built engine dylibs (CI fast path)
#
# Usage (from the repo root; engines build first unless SKIP_BUILD=1):
#   sh test/nodeapi/probes/run_async_probe.sh

set -eu

REPO=$(cd "$(dirname "$0")/../../.." && pwd)
cd "$REPO"

GATE=${CC_GATE_LOCK:-}

run_zig() {
  if [ -n "$GATE" ] && [ -x "$GATE" ]; then
    "$GATE" mise exec -- zig "$@"
  elif command -v mise >/dev/null 2>&1; then
    mise exec -- zig "$@"
  else
    zig "$@"
  fi
}

# Platform branch (Linux x86_64 mirror of the macOS ARM64 shape).
# Linux: run the default test profile leg (`zig-out/bin/libChakraCore.so`);
# the macOS-only jit leg below is skipped on Linux. Flags mirror linux.zig's
# provider/host compiles (ELF spellings: no sysroot/mcpu/frameworks).
if [ "$(uname -s)" = "Linux" ]; then
  TARGET="x86_64-linux-gnu"
  SDK=""
  ENGINE_NAME="libChakraCore.so"
  ENGINE_TEST="zig-out/bin/libChakraCore.so"
  LINK_SYSTEM="-ldl -lpthread"
  RPATH_FLAG="-Wl,-rpath,\$ORIGIN"
  CXXFLAGS="-target $TARGET -std=gnu++17 -O3 -g -Wno-date-time -DNAPI_VERSION=10 -DCHAKRA_NODEAPI_TEST_HOOKS -Ilib/Jsrt -Iinclude/nodeapi -Ilib/NodeApi -isystem /usr/include"
else
  TARGET="aarch64-macos.26.0"
  SDK=$(xcrun --show-sdk-path 2>/dev/null || true)
  if [ -z "$SDK" ]; then
    echo "run_async_probe: cannot locate macOS SDK (xcrun failed)" >&2
    exit 1
  fi
  ENGINE_NAME="libChakraCore.dylib"
  ENGINE_TEST="zig-out/bin/libChakraCore.dylib"
  LINK_SYSTEM="-framework CoreFoundation -framework Security"
  RPATH_FLAG="-Wl,-rpath,@executable_path"
  CXXFLAGS="-target $TARGET --sysroot $SDK -std=gnu++17 -O3 -g -mcpu=apple_m1 -Wno-date-time -DNAPI_VERSION=10 -DCHAKRA_NODEAPI_TEST_HOOKS -Ilib/Jsrt -Iinclude/nodeapi -Ilib/NodeApi -isystem $SDK/usr/include -F/System/Library/Frameworks"
fi

run_profile() {
  profile="$1"
  engine_lib="$2"
  out_bin="$3"

  echo "===== profile: $profile (engine: $engine_lib) ====="
  if [ ! -f "$engine_lib" ]; then
    echo "run_async_probe: engine dylib missing for $profile: $engine_lib" >&2
    echo "hint: run it via: zig build libChakraNodeApi" >&2
    return 1
  fi
  # shellcheck disable=SC2086
  run_zig c++ $CXXFLAGS \
    lib/NodeApi/NodeApiEnv.cpp \
    lib/NodeApi/NodeApiLifetime.cpp \
    lib/NodeApi/NodeApiRefs.cpp \
    lib/NodeApi/NodeApiScopes.cpp \
    lib/NodeApi/NodeApiValues.cpp \
    lib/NodeApi/NodeApiErrorsImpl.cpp \
    lib/NodeApi/NodeApiObjects.cpp \
    lib/NodeApi/NodeApiFunctions.cpp \
    lib/NodeApi/NodeApiBinary.cpp \
    lib/NodeApi/NodeApiPromise.cpp \
    lib/NodeApi/NodeApiScript.cpp \
    lib/NodeApi/NodeApiWrap.cpp \
    lib/NodeApi/NodeApiBuffers.cpp \
    lib/NodeApi/NodeApiAsync.cpp \
    lib/NodeApi/NodeApiAsyncContext.cpp \
    lib/NodeApi/NodeApiHost.cpp \
    test/nodeapi/probes/async_probe.cpp \
    "$engine_lib" \
    $LINK_SYSTEM \
    $RPATH_FLAG \
    -o "$out_bin"
  # Co-locate the engine dylib with the probe binary (@rpath).
  cp "$engine_lib" "$(dirname "$out_bin")/$ENGINE_NAME"
  "$out_bin"
}

WORK=$(mktemp -d "${TMPDIR:-/tmp}/async-probe.XXXXXX")
trap 'rm -rf "$WORK"' EXIT

# --- Engine builds (serialized via the gate lock) ---
if [ "${SKIP_BUILD:-0}" != "1" ]; then
  echo "===== building test-profile engine (zig build libChakraNodeApi) ====="
  run_zig build libChakraNodeApi
  if [ "$(uname -s)" != "Linux" ]; then
    echo "===== building jit-profile engine (zig build -Dprofile=jit libChakraNodeApi) ====="
    run_zig build -Dprofile=jit libChakraNodeApi
  else
    echo "===== linux: test leg only ====="
  fi
fi

rc=0
run_profile "test" "$REPO/$ENGINE_TEST" "$WORK/async-probe-test" || rc=$?
if [ "$(uname -s)" != "Linux" ]; then
  run_profile "jit" "$REPO/out/jit/bin/libChakraCore.dylib" "$WORK/async-probe-jit" || rc=$?
fi

if [ $rc -ne 0 ]; then
  echo "run_async_probe: FAILED (see output above)" >&2
  exit 1
fi
if [ "$(uname -s)" = "Linux" ]; then echo "run_async_probe: ALL PASS (test)"; else echo "run_async_probe: ALL PASS (test + jit)"; fi
