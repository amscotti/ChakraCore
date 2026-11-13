#!/bin/sh
# Host probe runner: N4.1 registration + fatal routing.
#
# Builds host_probe.cpp against each profile's engine dylib and runs it.
# Both profiles must pass:
#   test  (default)   -> zig-out/bin/libChakraCore.dylib
#   jit               -> out/jit/bin/libChakraCore.dylib
#
# All zig invocations go through the serialized gate lock with mise:
#   /tmp/opencode/cc-gate-lock.sh mise exec -- zig ...
#
# Env overrides:
#   CC_GATE_LOCK=path  GATE lock script (default /tmp/opencode/cc-gate-lock.sh)
#   SKIP_BUILD=1       reuse already-built engine dylibs (CI fast path)

set -eu

REPO=$(cd "$(dirname "$0")/../../.." && pwd)
cd "$REPO"

GATE=${CC_GATE_LOCK:-/tmp/opencode/cc-gate-lock.sh}

TARGET="aarch64-macos.26.0"
SDK=$(xcrun --show-sdk-path 2>/dev/null || true)
if [ -z "$SDK" ]; then
  echo "run_host_probe: cannot locate macOS SDK (xcrun failed)" >&2
  exit 1
fi

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
    echo "run_host_probe: engine dylib missing for $profile: $engine_lib" >&2
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
    lib/NodeApi/NodeApiObjects.cpp \
    lib/NodeApi/NodeApiFunctions.cpp \
    lib/NodeApi/NodeApiValues.cpp \
    lib/NodeApi/NodeApiErrorsImpl.cpp \
    lib/NodeApi/NodeApiAsyncContext.cpp \
    lib/NodeApi/NodeApiBuffers.cpp \
    lib/NodeApi/NodeApiHost.cpp \
    test/nodeapi/probes/host_probe.cpp \
    "$engine_lib" \
    -framework CoreFoundation -framework Security \
    -Wl,-rpath,@executable_path \
    -o "$out_bin"
  cp "$engine_lib" "$(dirname "$out_bin")/libChakraCore.dylib"
  echo "--- main probe ($profile) ---"
  if ! "$out_bin"; then
    echo "run_host_probe: main probe failed ($profile)" >&2
    return 1
  fi
  echo "--- fatal_exception without service ($profile, expect SIGABRT) ---"
  set +e
  "$out_bin" --fatal-exception-no-service
  abort_rc=$?
  set -e
  if [ "$abort_rc" -eq 0 ]; then
    echo "run_host_probe: fatal_exception without service returned (expected abort)" >&2
    return 1
  fi
  if [ "$abort_rc" -ne 134 ]; then
    echo "note: fatal_exception exit $abort_rc (expected 134/SIGABRT)" >&2
  fi
  echo "--- fatal_error ($profile, expect SIGABRT) ---"
  set +e
  "$out_bin" --fatal-error
  abort_rc=$?
  set -e
  if [ "$abort_rc" -eq 0 ]; then
    echo "run_host_probe: fatal_error returned (expected abort)" >&2
    return 1
  fi
  if [ "$abort_rc" -ne 134 ]; then
    echo "note: fatal_error exit $abort_rc (expected 134/SIGABRT)" >&2
  fi
}

WORK=$(mktemp -d "${TMPDIR:-/tmp}/host-probe.XXXXXX")
trap 'rm -rf "$WORK"' EXIT

if [ "${SKIP_BUILD:-0}" != "1" ]; then
  echo "===== building test-profile engine (zig build libChakraNodeApi) ====="
  "$GATE" mise exec -- zig build libChakraNodeApi
  echo "===== building jit-profile engine (zig build -Dprofile=jit libChakraNodeApi) ====="
  "$GATE" mise exec -- zig build -Dprofile=jit libChakraNodeApi
fi

rc=0
run_profile "test" "$REPO/zig-out/bin/libChakraCore.dylib" "$WORK/host-probe-test" || rc=$?
run_profile "jit" "$REPO/out/jit/bin/libChakraCore.dylib" "$WORK/host-probe-jit" || rc=$?

if [ $rc -ne 0 ]; then
  echo "run_host_probe: FAILED (see output above)" >&2
  exit 1
fi
echo "run_host_probe: ALL PASS (test + jit)"
