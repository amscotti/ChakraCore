#!/bin/sh
# N5.4 stress + teardown campaign runner.
#
# Builds the three stress probes (each with the full reconciled provider
# TU set) against EACH profile's engine dylib and runs them REPEATS times
# per probe per profile (default 10; plan: >=10 repeats). Also builds the
# provider + ch-nodeapi and runs the gc-hook proof addon/driver REPEATS
# times. Any failure exits nonzero.
#
# All zig invocations go through the serialized gate lock with mise:
#   /tmp/opencode/cc-gate-lock.sh mise exec -- zig ...
#
# Env overrides:
#   CC_GATE_LOCK=path  GATE lock script (default /tmp/opencode/cc-gate-lock.sh)
#   SKIP_BUILD=1       reuse already-built engine dylibs + provider
#   REPEATS=N          process-level repeats (default 10; must be >=1)

set -eu

REPO=$(cd "$(dirname "$0")/../../.." && pwd)
cd "$REPO"

GATE=${CC_GATE_LOCK:-/tmp/opencode/cc-gate-lock.sh}
REPEATS=${REPEATS:-10}

# Platform branch (Linux x86_64 mirror; test leg only, no jit on Linux).
if [ "$(uname -s)" = "Linux" ]; then
  TARGET="x86_64-linux-gnu"
  SDK=""
  ENGINE_NAME="libChakraCore.so"
  ENGINE_TEST="zig-out/bin/libChakraCore.so"
  PROVIDER_NAME="libChakraNodeApi.so"
  LINK_SYSTEM="-ldl -lpthread"
  RPATH_FLAG="-Wl,-rpath,\$ORIGIN"
  CXXFLAGS="-target $TARGET -std=gnu++17 -O3 -g -Wno-date-time -DNAPI_VERSION=8 -DCHAKRA_NODEAPI_TEST_HOOKS -Ilib/Jsrt -Iinclude/nodeapi -Ilib/NodeApi -isystem /usr/include"
  ADDON_FLAGS="-target $TARGET -std=gnu++17 -O3 -g -Wno-date-time -DNAPI_VERSION=8 -fPIC -shared -Iinclude/nodeapi -Ilib/Jsrt -isystem /usr/include"
else
  TARGET="aarch64-macos.26.0"
  SDK=$(xcrun --show-sdk-path 2>/dev/null || true)
  if [ -z "$SDK" ]; then
    echo "run_stress: cannot locate macOS SDK (xcrun failed)" >&2
    exit 1
  fi
  ENGINE_NAME="libChakraCore.dylib"
  ENGINE_TEST="zig-out/bin/libChakraCore.dylib"
  PROVIDER_NAME="libChakraNodeApi.dylib"
  LINK_SYSTEM="-framework CoreFoundation -framework Security"
  RPATH_FLAG="-Wl,-rpath,@executable_path"
  CXXFLAGS="-target $TARGET --sysroot $SDK -std=gnu++17 -O3 -g -mcpu=apple_m1 -Wno-date-time -DNAPI_VERSION=8 -DCHAKRA_NODEAPI_TEST_HOOKS -Ilib/Jsrt -Iinclude/nodeapi -Ilib/NodeApi -isystem $SDK/usr/include -F/System/Library/Frameworks"
  ADDON_FLAGS="-target $TARGET --sysroot $SDK -std=gnu++17 -O3 -g -mcpu=apple_m1 -Wno-date-time -DNAPI_VERSION=8 -fPIC -shared -Iinclude/nodeapi -Ilib/Jsrt -isystem $SDK/usr/include -F/System/Library/Frameworks -Wl,-undefined,dynamic_lookup"
fi

PROVIDER_TUS="
  lib/NodeApi/NodeApiEnv.cpp
  lib/NodeApi/NodeApiLifetime.cpp
  lib/NodeApi/NodeApiRefs.cpp
  lib/NodeApi/NodeApiScopes.cpp
  lib/NodeApi/NodeApiValues.cpp
  lib/NodeApi/NodeApiErrorsImpl.cpp
  lib/NodeApi/NodeApiObjects.cpp
  lib/NodeApi/NodeApiFunctions.cpp
  lib/NodeApi/NodeApiBinary.cpp
  lib/NodeApi/NodeApiPromise.cpp
  lib/NodeApi/NodeApiScript.cpp
  lib/NodeApi/NodeApiWrap.cpp
  lib/NodeApi/NodeApiBuffers.cpp
  lib/NodeApi/NodeApiAsync.cpp
  lib/NodeApi/NodeApiAsyncContext.cpp
  lib/NodeApi/NodeApiHost.cpp
"

run_probe() {
  # $1 probe cpp, $2 engine lib, $3 out bin, $4 label
  # shellcheck disable=SC2086
  "$GATE" mise exec -- zig c++ $CXXFLAGS $PROVIDER_TUS "$1" "$2" \
    $LINK_SYSTEM \
    $RPATH_FLAG \
    -o "$3"
  cp "$2" "$(dirname "$3")/$ENGINE_NAME"
}

run_repeated() {
  bin="$1"
  label="$2"
  i=1
  while [ "$i" -le "$REPEATS" ]; do
    echo "--- $label run $i/$REPEATS ---"
    "$bin" || return 1
    i=$((i + 1))
  done
}

WORK=$(mktemp -d "${TMPDIR:-/tmp}/nodeapi-stress.XXXXXX")
trap 'rm -rf "$WORK"' EXIT

if [ "${SKIP_BUILD:-0}" != "1" ]; then
  echo "===== building test-profile engine + provider ====="
  "$GATE" mise exec -- zig build libChakraNodeApi
  "$GATE" mise exec -- zig build ch-nodeapi
  if [ "$(uname -s)" != "Linux" ]; then
    echo "===== building jit-profile engine + provider ====="
    "$GATE" mise exec -- zig build -Dprofile=jit libChakraNodeApi
  else
    echo "===== linux: jit profile does not exist; test leg only ====="
  fi
fi

rc=0

echo "############ gc_churn_stress ############"
if [ "$(uname -s)" = "Linux" ]; then
  PAIRS="test:$REPO/$ENGINE_TEST"
else
  PAIRS="test:$REPO/zig-out/bin/libChakraCore.dylib jit:$REPO/out/jit/bin/libChakraCore.dylib"
fi
for pair in $PAIRS; do
  prof=${pair%%:*}
  eng=${pair#*:}
  if [ ! -f "$eng" ]; then
    echo "run_stress: engine dylib missing for $prof: $eng" >&2
    rc=1
    continue
  fi
  echo "===== profile: $prof ====="
  run_probe test/nodeapi/stress/gc_churn_stress.cpp "$eng" "$WORK/gc-churn-$prof" || rc=1
  run_repeated "$WORK/gc-churn-$prof" "gc_churn[$prof]" || rc=1
done

echo "############ env_churn_stress ############"
if [ "$(uname -s)" = "Linux" ]; then
  PAIRS="test:$REPO/$ENGINE_TEST"
else
  PAIRS="test:$REPO/zig-out/bin/libChakraCore.dylib jit:$REPO/out/jit/bin/libChakraCore.dylib"
fi
for pair in $PAIRS; do
  prof=${pair%%:*}
  eng=${pair#*:}
  if [ ! -f "$eng" ]; then
    echo "run_stress: engine dylib missing for $prof: $eng" >&2
    rc=1
    continue
  fi
  echo "===== profile: $prof ====="
  run_probe test/nodeapi/stress/env_churn_stress.cpp "$eng" "$WORK/env-churn-$prof" || rc=1
  run_repeated "$WORK/env-churn-$prof" "env_churn[$prof]" || rc=1
done

echo "############ tsfn_teardown_stress ############"
if [ "$(uname -s)" = "Linux" ]; then
  PAIRS="test:$REPO/$ENGINE_TEST"
else
  PAIRS="test:$REPO/zig-out/bin/libChakraCore.dylib jit:$REPO/out/jit/bin/libChakraCore.dylib"
fi
for pair in $PAIRS; do
  prof=${pair%%:*}
  eng=${pair#*:}
  if [ ! -f "$eng" ]; then
    echo "run_stress: engine dylib missing for $prof: $eng" >&2
    rc=1
    continue
  fi
  echo "===== profile: $prof ====="
  run_probe test/nodeapi/stress/tsfn_teardown_stress.cpp "$eng" "$WORK/tsfn-race-$prof" || rc=1
  run_repeated "$WORK/tsfn-race-$prof" "tsfn_race[$prof]" || rc=1
done

echo "############ ch-nodeapi gc-hook proof ############"
# Reuse the installed ch-nodeapi + provider build outputs. The stress
# addon is built on demand (not part of `zig build ch-nodeapi`) and
# linked with undefined napi_* (resolved from the provider RTLD_GLOBAL).
HOST="$REPO/zig-out/bin/ch-nodeapi"
PROV="$REPO/zig-out/lib/$PROVIDER_NAME"
if [ ! -f "$HOST" ] || [ ! -f "$PROV" ]; then
  echo "run_stress: ch-nodeapi/provider missing (build without SKIP_BUILD)" >&2
  rc=1
else
  "$GATE" mise exec -- zig c++ $ADDON_FLAGS \
    test/nodeapi/stress/gc_hook_addon.cpp \
    $LINK_SYSTEM \
    -o "$WORK/gc_hook_addon.node"
  i=1
  while [ "$i" -le "$REPEATS" ]; do
    echo "--- gc_hook[test] run $i/$REPEATS ---"
    "$HOST" --provider "$PROV" --addon "$WORK/gc_hook_addon.node" \
      --driver test/nodeapi/stress/gc_hook_driver.js || rc=1
    i=$((i + 1))
  done
fi

if [ "$rc" -ne 0 ]; then
  echo "run_stress: FAILED (see output above)" >&2
  exit 1
fi
if [ "$(uname -s)" = "Linux" ]; then echo "run_stress: ALL PASS (test, ${REPEATS} repeats)"; else echo "run_stress: ALL PASS (test + jit, ${REPEATS} repeats each)"; fi
