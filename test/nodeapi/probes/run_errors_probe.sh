#!/bin/sh
# Errors + throw helpers probe runner (plan doc/node-api-plan.md task N2.5).
#
# Builds errors_probe.cpp against each profile's engine dylib and runs it.
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
  CXXFLAGS="-target $TARGET -std=gnu++17 -O3 -g -Wno-date-time -DNAPI_VERSION=8 -DCHAKRA_NODEAPI_TEST_HOOKS -Ilib/Jsrt -Iinclude/nodeapi -Ilib/NodeApi -isystem /usr/include"
else
  TARGET="aarch64-macos.26.0"
  SDK=$(xcrun --show-sdk-path 2>/dev/null || true)
  if [ -z "$SDK" ]; then
    echo "run_errors_probe: cannot locate macOS SDK (xcrun failed)" >&2
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
    echo "run_errors_probe: engine dylib missing for $profile: $engine_lib" >&2
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
    test/nodeapi/probes/errors_probe.cpp \
    "$engine_lib" \
    $LINK_SYSTEM \
    $RPATH_FLAG \
    -o "$out_bin"
  cp "$engine_lib" "$(dirname "$out_bin")/$ENGINE_NAME"
  echo "--- main probe ($profile) ---"
  if ! "$out_bin"; then
    echo "run_errors_probe: main probe failed ($profile)" >&2
    return 1
  fi
  echo "--- fatal_exception without service ($profile, expect SIGABRT) ---"
  set +e
  "$out_bin" --fatal-exception-no-service
  abort_rc=$?
  set -e
  # SIGABRT is 134 (128+6) on macOS zsh/sh; non-zero + no output "returned" is the gate.
  if [ "$abort_rc" -eq 0 ]; then
    echo "run_errors_probe: fatal_exception without service returned (expected abort)" >&2
    return 1
  fi
  # 134 is SIGABRT; other non-zero is also acceptable if it aborted via abort(), but
  # flag anything else as suspicious (still a failure to return normally).
  if [ "$abort_rc" -ne 134 ]; then
    echo "note: fatal_exception exit $abort_rc (expected 134/SIGABRT)" >&2
  fi
  echo "--- fatal_error ($profile, expect SIGABRT) ---"
  set +e
  "$out_bin" --fatal-error
  abort_rc=$?
  set -e
  if [ "$abort_rc" -eq 0 ]; then
    echo "run_errors_probe: fatal_error returned (expected abort)" >&2
    return 1
  fi
  if [ "$abort_rc" -ne 134 ]; then
    echo "note: fatal_error exit $abort_rc (expected 134/SIGABRT)" >&2
  fi
}

WORK=$(mktemp -d "${TMPDIR:-/tmp}/errors-probe.XXXXXX")
trap 'rm -rf "$WORK"' EXIT

if [ "${SKIP_BUILD:-0}" != "1" ]; then
  echo "===== building test-profile engine (zig build libChakraNodeApi) ====="
  "$GATE" mise exec -- zig build libChakraNodeApi
  if [ "$(uname -s)" != "Linux" ]; then
    echo "===== building jit-profile engine (zig build -Dprofile=jit libChakraNodeApi) ====="
    "$GATE" mise exec -- zig build -Dprofile=jit libChakraNodeApi
  else
    echo "===== linux: jit profile does not exist; test leg only ====="
  fi
fi

rc=0
run_profile "test" "$REPO/$ENGINE_TEST" "$WORK/errors-probe-test" || rc=$?
if [ "$(uname -s)" != "Linux" ]; then
  run_profile "jit" "$REPO/out/jit/bin/libChakraCore.dylib" "$WORK/errors-probe-jit" || rc=$?
fi

if [ $rc -ne 0 ]; then
  echo "run_errors_probe: FAILED (see output above)" >&2
  exit 1
fi
if [ "$(uname -s)" = "Linux" ]; then echo "run_errors_probe: ALL PASS (test)"; else echo "run_errors_probe: ALL PASS (test + jit)"; fi
