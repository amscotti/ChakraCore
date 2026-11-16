#!/bin/sh
# N1.2 scope probe runner (plan doc/node-api-plan.md task N1.2).
#
# Builds the self-contained probe driver (test/nodeapi/probes/scopes_probe.cpp
# + lib/NodeApi/NodeApiScopes.cpp + scopes_shim.cpp) against EACH profile's
# engine dylib and runs it. Both profiles must pass:
#   test  (default)   -> zig-out/bin/libChakraCore.dylib
#   jit               -> out/jit/bin/libChakraCore.dylib
#
# Zig invocations run directly via mise/zig (optionally serialized if
# CC_GATE_LOCK points to an executable gate-lock wrapper script).
#
# Env overrides (same names as the commands they wrap):
#   CC_GATE_LOCK=path  Optional gate-lock wrapper script (disabled by default)
#   SKIP_BUILD=1       reuse already-built engine dylibs (CI fast path)
#   NODEAPI_TARGET=t   zig -target (default per platform branch below)
#   NODEAPI_SDK=path   SDK sysroot (default: xcrun on macOS, empty on Linux)
#
# The driver is a self-contained C++ driver using the private host API shape
# (not an addon through ch-nodeapi): scopes_probe predates the real N1.1 env
# and drives ScopesProbe_CreateEnv via scopes_shim.cpp, which provides the
# nodeapi::CheckAlive/SetLastError symbols NodeApiScopes.cpp links against.
# Linking NodeApiEnv.cpp would duplicate those symbols, so the Scopes+shim
# TU set is the current (correct) set for this probe, not a pre-Env stale
# set; see test/nodeapi/probes/scopes_shim.h. Exit code is nonzero if either
# profile's run fails.
#
# Supported entry point: the canonical per-probe runner is
# test/nodeapi/probes/run_scopes_probe.sh (same TU set, platform-branched).
# This file is kept for history and kept runnable; new automation should call
# the probes-dir runner. Note: test/nodeapi/run_matrix.py does NOT supersede
# this probe -- it runs vendored addon targets (.node) via ch-nodeapi, not
# this self-contained scopes driver.
#
# The GC probes in scopes_probe.cpp are optnone (conservative stack scan is
# optimizer-sensitive at -O3; the provider TU stays at the profile default).

set -eu

REPO=$(cd "$(dirname "$0")/../.." && pwd)
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
  TARGET="${NODEAPI_TARGET:-x86_64-linux-gnu}"
  SDK="${NODEAPI_SDK:-}"
  ENGINE_NAME="libChakraCore.so"
  ENGINE_TEST="zig-out/bin/libChakraCore.so"
  LINK_SYSTEM="-ldl -lpthread"
  RPATH_FLAG="-Wl,-rpath,\$ORIGIN"
  CXXFLAGS="-target $TARGET -std=gnu++17 -O3 -g -Wno-date-time -DNAPI_VERSION=8 -DCHAKRA_NODEAPI_TEST_HOOKS -Ilib/Jsrt -Iinclude/nodeapi -Ilib/NodeApi -Itest/nodeapi/probes -isystem /usr/include"
else
  TARGET="${NODEAPI_TARGET:-aarch64-macos.26.0}"
  SDK="${NODEAPI_SDK:-$(xcrun --show-sdk-path 2>/dev/null || true)}"
  if [ -z "$SDK" ]; then
    echo "run_probe: cannot locate macOS SDK (xcrun failed)" >&2
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
    echo "run_probe: engine dylib missing for $profile: $engine_lib" >&2
    echo "hint: run it via: zig build ch-nodeapi" >&2
    return 1
  fi
  # shellcheck disable=SC2086
  run_zig c++ $CXXFLAGS \
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
run_profile "test" "$REPO/$ENGINE_TEST" "$WORK/scopes-probe-test" || rc=$?
if [ "$(uname -s)" != "Linux" ]; then
  run_profile "jit" "$REPO/out/jit/bin/libChakraCore.dylib" "$WORK/scopes-probe-jit" || rc=$?
fi

if [ $rc -ne 0 ]; then
  echo "run_probe: FAILED (see output above)" >&2
  exit 1
fi
if [ "$(uname -s)" = "Linux" ]; then echo "run_probe: ALL PASS (test)"; else echo "run_probe: ALL PASS (test + jit)"; fi
