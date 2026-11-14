#!/bin/sh
# N1.2 scope probe runner (plan task N1.2, semantics P2-*).
#
# Builds the self-contained probe driver (test/nodeapi/probes/
# scopes_probe.cpp + lib/NodeApi/NodeApiScopes.cpp + scopes_shim.cpp)
# against EACH profile's engine dylib and runs it. Both profiles must
# pass:
#   test  (default)   -> zig-out/bin/libChakraCore.dylib
#   jit               -> out/jit/bin/libChakraCore.dylib
#
# All zig invocations go through the serialized gate lock with mise:
#   /tmp/opencode/cc-gate-lock.sh mise exec -- zig ...
#
# Env overrides (same names as the commands they wrap):
#   CC_GATE_LOCK=path  GATE lock script (default /tmp/opencode/cc-gate-lock.sh)
#   SKIP_BUILD=1       reuse already-built engine dylibs (CI fast path)
#
# Usage (from the repo root; engines build first unless SKIP_BUILD=1):
#   sh test/nodeapi/probes/run_scopes_probe.sh
#   SKIP_BUILD=1 sh test/nodeapi/probes/run_scopes_probe.sh
#
# NOTE: the shim TU set (Scopes + scopes_shim) is intentional, not stale:
# scopes_probe drives the private host API shape (ScopesProbe_CreateEnv)
# because it predates the real N1.1 env; linking NodeApiEnv.cpp would
# duplicate the nodeapi::CheckAlive/SetLastError symbols the shim
# provides. See test/nodeapi/probes/scopes_shim.h. The legacy
# tools/nodeapi/run_probe.sh runs the same TU set and points here.
#
# The GC probes in scopes_probe.cpp are optnone (conservative stack scan
# is optimizer-sensitive at -O3; the provider TU stays at the profile
# default).

set -eu

REPO=$(cd "$(dirname "$0")/../../.." && pwd)
cd "$REPO"

GATE=${CC_GATE_LOCK:-/tmp/opencode/cc-gate-lock.sh}

# Platform branch (Linux x86_64 mirror of the macOS ARM64 shape).
# Linux: test profile only (tools/build/linux.zig has no jit);
# the jit leg below is skipped on Linux. Flags mirror linux.zig's
# provider/host compiles (ELF spellings: no sysroot/mcpu/frameworks).
if [ "$(uname -s)" = "Linux" ]; then
  TARGET="x86_64-linux-gnu"
  SDK=""
  ENGINE_NAME="libChakraCore.so"
  ENGINE_TEST="zig-out/bin/libChakraCore.so"
  LINK_SYSTEM="-ldl -lpthread"
  RPATH_FLAG="-Wl,-rpath,\$ORIGIN"
  CXXFLAGS="-target $TARGET -std=gnu++17 -O3 -g -Wno-date-time -DNAPI_VERSION=8 -DCHAKRA_NODEAPI_TEST_HOOKS -Ilib/Jsrt -Iinclude/nodeapi -Ilib/NodeApi -Itest/nodeapi/probes -isystem /usr/include"
else
  TARGET="aarch64-macos.26.0"
  SDK=$(xcrun --show-sdk-path 2>/dev/null || true)
  if [ -z "$SDK" ]; then
    echo "run_scopes_probe: cannot locate macOS SDK (xcrun failed)" >&2
    exit 1
  fi
  ENGINE_NAME="libChakraCore.dylib"
  ENGINE_TEST="zig-out/bin/libChakraCore.dylib"
  LINK_SYSTEM="-framework CoreFoundation -framework Security"
  RPATH_FLAG="-Wl,-rpath,@executable_path"
  CXXFLAGS="-target $TARGET --sysroot $SDK -std=gnu++17 -O3 -g -mcpu=apple_m1 -Wno-date-time -DNAPI_VERSION=8 -DCHAKRA_NODEAPI_TEST_HOOKS -Ilib/Jsrt -Iinclude/nodeapi -Ilib/NodeApi -Itest/nodeapi/probes -isystem $SDK/usr/include -F/System/Library/Frameworks"
fi

run_profile() {
  profile="$1"
  engine_lib="$2"
  out_bin="$3"

  echo "===== profile: $profile (engine: $engine_lib) ====="
  if [ ! -f "$engine_lib" ]; then
    echo "run_scopes_probe: engine dylib missing for $profile: $engine_lib" >&2
    echo "hint: run it via: $GATE mise exec -- zig build ch-nodeapi" >&2
    return 1
  fi
  # shellcheck disable=SC2086
  "$GATE" mise exec -- zig c++ $CXXFLAGS \
    lib/NodeApi/NodeApiScopes.cpp \
    test/nodeapi/probes/scopes_shim.cpp \
    test/nodeapi/probes/scopes_probe.cpp \
    "$engine_lib" \
    $LINK_SYSTEM \
    $RPATH_FLAG \
    -o "$out_bin"
  cp "$engine_lib" "$(dirname "$out_bin")/$ENGINE_NAME"
  "$out_bin"
}

WORK=$(mktemp -d "${TMPDIR:-/tmp}/scopes-probe.XXXXXX")
trap 'rm -rf "$WORK"' EXIT

if [ "${SKIP_BUILD:-0}" != "1" ]; then
  echo "===== building test-profile engine (zig build ch-nodeapi) ====="
  "$GATE" mise exec -- zig build ch-nodeapi
  if [ "$(uname -s)" != "Linux" ]; then
    echo "===== building jit-profile engine (zig build -Dprofile=jit) ====="
    "$GATE" mise exec -- zig build -Dprofile=jit
  else
    echo "===== linux: jit profile does not exist; test leg only ====="
  fi
fi

rc=0
run_profile "test" "$REPO/$ENGINE_TEST" "$WORK/scopes-probe-test" || rc=$?
if [ "$(uname -s)" != "Linux" ]; then
  run_profile "jit" "$REPO/out/jit/bin/libChakraCore.dylib" "$WORK/scopes-probe-jit" || rc=$?
fi

if [ $rc -ne 0 ]; then
  echo "run_scopes_probe: FAILED (see output above)" >&2
  exit 1
fi
if [ "$(uname -s)" = "Linux" ]; then echo "run_scopes_probe: ALL PASS (test)"; else echo "run_scopes_probe: ALL PASS (test + jit)"; fi
