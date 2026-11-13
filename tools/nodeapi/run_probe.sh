#!/bin/sh
# N1.2 scope probe runner (plan doc/node-api-plan.md task N1.2).
#
# Builds the self-contained probe driver (test/nodeapi/probes/scopes_probe.cpp
# + lib/NodeApi/NodeApiScopes.cpp + scopes_shim.cpp) against EACH profile's
# engine dylib and runs it. Both profiles must pass:
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
# The driver is a self-contained C++ driver using the private host API shape
# (not an addon through ch-nodeapi) because the real provider (N1.1) has not
# landed; see test/nodeapi/probes/scopes_probe.cpp header. Exit code is
# nonzero if either profile's run fails.

set -eu

REPO=$(cd "$(dirname "$0")/../.." && pwd)
cd "$REPO"

GATE=${CC_GATE_LOCK:-/tmp/opencode/cc-gate-lock.sh}

TARGET="aarch64-macos.26.0"
SDK=$(xcrun --show-sdk-path 2>/dev/null || true)
if [ -z "$SDK" ]; then
  echo "run_probe: cannot locate macOS SDK (xcrun failed)" >&2
  exit 1
fi

# Mirror of build.zig's ch-nodeapi compile/link flags (test profile values):
# -std=gnu++17 -DNAPI_VERSION=8 -mcpu=apple_m1, includes lib/Jsrt +
# include/nodeapi + lib/NodeApi. Probe adds -DCHAKRA_NODEAPI_TEST_HOOKS
# (R-REF-14 root counter) and the probes include dir. The GC probes in
# scopes_probe.cpp are optnone (conservative stack scan is optimizer-
# sensitive at -O3; the provider TU stays at the profile default).
CXXFLAGS="-target $TARGET --sysroot $SDK -std=gnu++17 -O3 -g -mcpu=apple_m1 -Wno-date-time \
  -DNAPI_VERSION=8 -DCHAKRA_NODEAPI_TEST_HOOKS \
  -Ilib/Jsrt -Iinclude/nodeapi -Ilib/NodeApi -Itest/nodeapi/probes \
  -isystem $SDK/usr/include -F/System/Library/Frameworks"

run_profile() {
  profile="$1"
  engine_lib="$2"
  out_bin="$3"

  echo "===== profile: $profile (engine: $engine_lib) ====="
  if [ ! -f "$engine_lib" ]; then
    echo "run_probe: engine dylib missing for $profile: $engine_lib" >&2
    echo "hint: run it via: $GATE mise exec -- zig build ch-nodeapi" >&2
    return 1
  fi
  # shellcheck disable=SC2086
  "$GATE" mise exec -- zig c++ $CXXFLAGS \
    lib/NodeApi/NodeApiScopes.cpp \
    test/nodeapi/probes/scopes_shim.cpp \
    test/nodeapi/probes/scopes_probe.cpp \
    "$engine_lib" \
    -framework CoreFoundation -framework Security \
    -Wl,-rpath,"$(dirname "$engine_lib")" \
    -o "$out_bin"
  "$out_bin"
}

WORK=$(mktemp -d "${TMPDIR:-/tmp}/scopes-probe.XXXXXX")
trap 'rm -rf "$WORK"' EXIT

# --- Engine builds (serialized via the gate lock) ---
if [ "${SKIP_BUILD:-0}" != "1" ]; then
  echo "===== building test-profile engine (zig build ch-nodeapi) ====="
  "$GATE" mise exec -- zig build ch-nodeapi
  echo "===== building jit-profile engine (zig build -Dprofile=jit) ====="
  "$GATE" mise exec -- zig build -Dprofile=jit
fi

rc=0
run_profile "test" "$REPO/zig-out/bin/libChakraCore.dylib" "$WORK/scopes-probe-test" || rc=$?
run_profile "jit" "$REPO/out/jit/bin/libChakraCore.dylib" "$WORK/scopes-probe-jit" || rc=$?

if [ $rc -ne 0 ]; then
  echo "run_probe: FAILED (see output above)" >&2
  exit 1
fi
echo "run_probe: ALL PASS (test + jit)"
