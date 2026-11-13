#!/bin/sh
# N2.6 coercion/type-check/symbol/Date/BigInt probe runner (plan task N2.6).
#
# Builds the self-contained probe driver (test/nodeapi/probes/
# coerce_probe.cpp + ALL of lib/NodeApi provider TUs: NodeApiEnv.cpp,
# NodeApiLifetime.cpp, NodeApiRefs.cpp, NodeApiScopes.cpp,
# NodeApiValues.cpp, NodeApiObjects.cpp, NodeApiFunctions.cpp,
# NodeApiErrorsImpl.cpp) against EACH profile's engine dylib and runs it. Both profiles must pass:
#   test  (default)   -> zig-out/bin/libChakraCore.dylib
#   jit               -> out/jit/bin/libChakraCore.dylib
#
# Linking the full provider TU set (not just NodeApiValues.cpp) also
# proves the N2.6 surface has no duplicate symbols with the landed
# napi_instanceof (N2.3) / napi_is_error (N2.5).
#
# All zig invocations go through the serialized gate lock with mise:
#   /tmp/opencode/cc-gate-lock.sh mise exec -- zig ...
#
# Env overrides:
#   CC_GATE_LOCK=path  GATE lock script (default /tmp/opencode/cc-gate-lock.sh)
#   SKIP_BUILD=1       reuse already-built engine dylibs (CI fast path)
#
# Usage (from the repo root; engines build first unless SKIP_BUILD=1):
#   sh test/nodeapi/probes/run_coerce_probe.sh
#   SKIP_BUILD=1 sh test/nodeapi/probes/run_coerce_probe.sh

set -eu

REPO=$(cd "$(dirname "$0")/../../.." && pwd)
cd "$REPO"

GATE=${CC_GATE_LOCK:-/tmp/opencode/cc-gate-lock.sh}

TARGET="aarch64-macos.26.0"
SDK=$(xcrun --show-sdk-path 2>/dev/null || true)
if [ -z "$SDK" ]; then
  echo "run_coerce_probe: cannot locate macOS SDK (xcrun failed)" >&2
  exit 1
fi

# Mirror of build.zig's ch-nodeapi compile/link flags (test profile values):
# -std=gnu++17 -DNAPI_VERSION=8 -mcpu=apple_m1, includes lib/Jsrt +
# include/nodeapi + lib/NodeApi. Probe adds -DCHAKRA_NODEAPI_TEST_HOOKS
# (R-REF-14 root counter) so the counter exists; it is never exported
# from release libChakraNodeApi (N1.6 inventory).
CXXFLAGS="-target $TARGET --sysroot $SDK -std=gnu++17 -O3 -g -mcpu=apple_m1 -Wno-date-time \
  -DNAPI_VERSION=8 -DCHAKRA_NODEAPI_TEST_HOOKS \
  -Ilib/Jsrt -Iinclude/nodeapi -Ilib/NodeApi \
  -isystem $SDK/usr/include -F/System/Library/Frameworks"

run_profile() {
  profile="$1"
  engine_lib="$2"
  out_bin="$3"

  echo "===== profile: $profile (engine: $engine_lib) ====="
  if [ ! -f "$engine_lib" ]; then
    echo "run_coerce_probe: engine dylib missing for $profile: $engine_lib" >&2
    echo "hint: run it via: $GATE mise exec -- zig build libChakraNodeApi" >&2
    return 1
  fi
  # shellcheck disable=SC2086
  "$GATE" mise exec -- zig c++ $CXXFLAGS \
    lib/NodeApi/NodeApiEnv.cpp \
    lib/NodeApi/NodeApiLifetime.cpp \
    lib/NodeApi/NodeApiRefs.cpp \
    lib/NodeApi/NodeApiScopes.cpp \
    lib/NodeApi/NodeApiBinary.cpp \
    lib/NodeApi/NodeApiPromise.cpp \
    lib/NodeApi/NodeApiScript.cpp \
    lib/NodeApi/NodeApiWrap.cpp \
    lib/NodeApi/NodeApiAsyncContext.cpp \
    lib/NodeApi/NodeApiValues.cpp \
    lib/NodeApi/NodeApiObjects.cpp \
    lib/NodeApi/NodeApiFunctions.cpp \
    lib/NodeApi/NodeApiErrorsImpl.cpp \
    test/nodeapi/probes/coerce_probe.cpp \
    "$engine_lib" \
    -framework CoreFoundation -framework Security \
    -Wl,-rpath,@executable_path \
    -o "$out_bin"
  # Co-locate the engine dylib with the probe binary (@rpath).
  cp "$engine_lib" "$(dirname "$out_bin")/libChakraCore.dylib"
  "$out_bin"
}

WORK=$(mktemp -d "${TMPDIR:-/tmp}/coerce-probe.XXXXXX")
trap 'rm -rf "$WORK"' EXIT

# --- Engine builds (serialized via the gate lock) ---
if [ "${SKIP_BUILD:-0}" != "1" ]; then
  echo "===== building test-profile engine (zig build libChakraNodeApi) ====="
  "$GATE" mise exec -- zig build libChakraNodeApi
  echo "===== building jit-profile engine (zig build -Dprofile=jit libChakraNodeApi) ====="
  "$GATE" mise exec -- zig build -Dprofile=jit libChakraNodeApi
fi

rc=0
run_profile "test" "$REPO/zig-out/bin/libChakraCore.dylib" "$WORK/coerce-probe-test" || rc=$?
run_profile "jit" "$REPO/out/jit/bin/libChakraCore.dylib" "$WORK/coerce-probe-jit" || rc=$?

if [ $rc -ne 0 ]; then
  echo "run_coerce_probe: FAILED (see output above)" >&2
  exit 1
fi
echo "run_coerce_probe: ALL PASS (test + jit)"
