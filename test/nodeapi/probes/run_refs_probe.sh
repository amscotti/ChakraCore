#!/bin/sh
# N1.3 references probe runner (plan task N1.3, P3-* probes).
#
# Builds the self-contained probe driver (test/nodeapi/probes/refs_probe.cpp
# + lib/NodeApi/NodeApiRefs.cpp + the reconciled N1.1/N1.2/N1.4 provider TUs
# it now links against: NodeApiEnv.cpp, NodeApiScopes.cpp,
# NodeApiLifetime.cpp) against EACH profile's engine dylib and runs it.
# Both profiles must pass:
#   test  (default)   -> zig-out/bin/libChakraCore.dylib
#   jit               -> out/jit/bin/libChakraCore.dylib
#
# The probe exercises the reconciled env (nodeapi_refs_test_create_env is a
# thin adapter over chakra_nodeapi_create_env; destroy runs the full §3.6
# sequence with refs teardown at step 6a), so the provider TUs must be
# linked alongside NodeApiRefs.cpp -- building NodeApiRefs.cpp alone would
# now leave NodeApiEnv_RefsState / CheckAlive / CreateState unresolved.
#
# Zig invocations run directly via mise/zig (optionally serialized if
# CC_GATE_LOCK points to an executable gate-lock wrapper script).
#
# Env overrides (same names as the commands they wrap):
#   CC_GATE_LOCK=path  Optional gate-lock wrapper script (disabled by default)
#   SKIP_BUILD=1       reuse already-built engine dylibs (CI fast path)
#
# Usage (from the repo root; engines build first unless SKIP_BUILD=1):
#   sh test/nodeapi/probes/run_refs_probe.sh
#   SKIP_BUILD=1 sh test/nodeapi/probes/run_refs_probe.sh

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
  CXXFLAGS="-target $TARGET -std=gnu++17 -O3 -g -Wno-date-time -DNAPI_VERSION=8 -DCHAKRA_NODEAPI_TEST_HOOKS -Ilib/Jsrt -Iinclude/nodeapi -Ilib/NodeApi -isystem /usr/include"
else
  TARGET="aarch64-macos.26.0"
  SDK=$(xcrun --show-sdk-path 2>/dev/null || true)
  if [ -z "$SDK" ]; then
    echo "run_refs_probe: cannot locate macOS SDK (xcrun failed)" >&2
    exit 1
  fi
  ENGINE_NAME="libChakraCore.dylib"
  ENGINE_TEST="zig-out/bin/libChakraCore.dylib"
  LINK_SYSTEM="-framework CoreFoundation -framework Security"
  RPATH_FLAG="-Wl,-rpath,@executable_path"
  CXXFLAGS="-target $TARGET --sysroot $SDK -std=gnu++17 -O3 -g -mcpu=apple_m1 -Wno-date-time -DNAPI_VERSION=8 -DCHAKRA_NODEAPI_TEST_HOOKS -Ilib/Jsrt -Iinclude/nodeapi -Ilib/NodeApi -isystem $SDK/usr/include -F/System/Library/Frameworks"
fi

run_profile() {
  profile="$1"
  engine_lib="$2"
  out_bin="$3"

  echo "===== profile: $profile (engine: $engine_lib) ====="
  if [ ! -f "$engine_lib" ]; then
    echo "run_refs_probe: engine dylib missing for $profile: $engine_lib" >&2
    echo "hint: run it via: zig build ch-nodeapi" >&2
    return 1
  fi
  # shellcheck disable=SC2086
  run_zig c++ $CXXFLAGS \
    lib/NodeApi/NodeApiEnv.cpp \
    lib/NodeApi/NodeApiLifetime.cpp \
    lib/NodeApi/NodeApiRefs.cpp \
    lib/NodeApi/NodeApiScopes.cpp \
    lib/NodeApi/NodeApiBinary.cpp \
    lib/NodeApi/NodeApiPromise.cpp \
    lib/NodeApi/NodeApiScript.cpp \
    lib/NodeApi/NodeApiWrap.cpp \
    lib/NodeApi/NodeApiAsyncContext.cpp \
    lib/NodeApi/NodeApiObjects.cpp \
    lib/NodeApi/NodeApiFunctions.cpp \
    lib/NodeApi/NodeApiValues.cpp \
    lib/NodeApi/NodeApiErrorsImpl.cpp \
    test/nodeapi/probes/refs_probe.cpp \
    "$engine_lib" \
    $LINK_SYSTEM \
    $RPATH_FLAG \
    -o "$out_bin"
  # Co-locate the engine dylib with the probe binary (@rpath).
  cp "$engine_lib" "$(dirname "$out_bin")/$ENGINE_NAME"
  "$out_bin"
}

WORK=$(mktemp -d "${TMPDIR:-/tmp}/refs-probe.XXXXXX")
trap 'rm -rf "$WORK"' EXIT

# --- Engine builds (serialized via the gate lock) ---
if [ "${SKIP_BUILD:-0}" != "1" ]; then
  echo "===== building test-profile engine (zig build ch-nodeapi) ====="
  run_zig build ch-nodeapi
  if [ "$(uname -s)" != "Linux" ]; then
    echo "===== building jit-profile engine (zig build -Dprofile=jit) ====="
    run_zig build -Dprofile=jit
  else
    echo "===== linux: test leg only ====="
  fi
fi

rc=0
run_profile "test" "$REPO/$ENGINE_TEST" "$WORK/refs-probe-test" || rc=$?
if [ "$(uname -s)" != "Linux" ]; then
  run_profile "jit" "$REPO/out/jit/bin/libChakraCore.dylib" "$WORK/refs-probe-jit" || rc=$?
fi

if [ $rc -ne 0 ]; then
  echo "run_refs_probe: FAILED (see output above)" >&2
  exit 1
fi
if [ "$(uname -s)" = "Linux" ]; then echo "run_refs_probe: ALL PASS (test)"; else echo "run_refs_probe: ALL PASS (test + jit)"; fi
