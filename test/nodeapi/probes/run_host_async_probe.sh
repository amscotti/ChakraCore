#!/bin/sh
#-------------------------------------------------------------------------------------------------------
# Copyright (c) ChakraCore Project Contributors. All rights reserved.
# Licensed under the MIT license. See LICENSE.txt file in the project root for full license information.
#-------------------------------------------------------------------------------------------------------
# JS-driven async_work + threadsafe_function + --pump-timeout-ms probe runner.
# Builds host_async_addon.cpp as a .node module and executes host_async_driver.js
# via ch-nodeapi (provider libChakraNodeApi + host loop services).

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

if [ "$(uname -s)" = "Linux" ]; then
  TARGET="x86_64-linux-gnu"
  CXXFLAGS="-target $TARGET -std=gnu++17 -O2 -fPIC -shared -DNAPI_VERSION=10 -Iinclude/nodeapi -isystem /usr/include"
  PROVIDER="$REPO/zig-out/lib/libChakraNodeApi.so"
  HOST="$REPO/zig-out/bin/ch-nodeapi"
else
  TARGET="aarch64-macos.26.0"
  SDK=$(xcrun --show-sdk-path 2>/dev/null || true)
  if [ -z "$SDK" ]; then
    echo "run_host_async_probe: cannot locate macOS SDK (xcrun failed)" >&2
    exit 1
  fi
  CXXFLAGS="-target $TARGET --sysroot $SDK -std=gnu++17 -O2 -fPIC -shared -DNAPI_VERSION=10 -Iinclude/nodeapi -undefined dynamic_lookup"
  PROVIDER="$REPO/zig-out/lib/libChakraNodeApi.dylib"
  HOST="$REPO/zig-out/bin/ch-nodeapi"
fi

if [ "${SKIP_BUILD:-0}" != "1" ]; then
  echo "===== building libChakraNodeApi + ch-nodeapi ====="
  run_zig build ch-nodeapi libChakraNodeApi
fi

if [ ! -x "$HOST" ] || [ ! -f "$PROVIDER" ]; then
  echo "run_host_async_probe: ch-nodeapi or libChakraNodeApi missing" >&2
  exit 1
fi

WORK=$(mktemp -d "${TMPDIR:-/tmp}/host-async-probe.XXXXXX")
trap 'rm -rf "$WORK"' EXIT

ADDON="$WORK/host_async_addon.node"
# shellcheck disable=SC2086
run_zig c++ $CXXFLAGS \
  test/nodeapi/probes/host_async_addon.cpp \
  -o "$ADDON"

NODE_PATH="$REPO/test/nodeapi/shims"
export NODE_PATH

echo "===== case 1: async_work + TSFN FIFO + promise completion ====="
OUT=$("$HOST" --provider "$PROVIDER" --addon "$ADDON" \
  --driver "$REPO/test/nodeapi/probes/host_async_driver.js" \
  --pump-timeout-ms 5000)
echo "$OUT"
printf '%s\n' "$OUT" | grep -q "HOST_ASYNC_JS: PASS"

echo "===== case 2: --pump-timeout-ms enforcement on held loop ref ====="
set +e
ERR_OUT=$(HOST_ASYNC_EXPECT_TIMEOUT=1 "$HOST" --provider "$PROVIDER" \
  --addon "$ADDON" --driver "$REPO/test/nodeapi/probes/host_async_driver.js" \
  --pump-timeout-ms 120 2>&1)
RC=$?
set -e
echo "$ERR_OUT"
if [ "$RC" -eq 0 ]; then
  echo "run_host_async_probe: expected non-zero exit on pump timeout" >&2
  exit 1
fi
printf '%s\n' "$ERR_OUT" | grep -q "timed out"

echo "===== case 3: --pump-timeout-ms on active ExecuteWork (detached worker exit) ====="
set +e
WORK_OUT=$(HOST_ASYNC_EXPECT_TIMEOUT=work "$HOST" --provider "$PROVIDER" \
  --addon "$ADDON" --driver "$REPO/test/nodeapi/probes/host_async_driver.js" \
  --pump-timeout-ms 80 2>&1)
RC=$?
set -e
echo "$WORK_OUT"
if [ "$RC" -eq 0 ]; then
  echo "run_host_async_probe: expected non-zero exit on slow ExecuteWork timeout" >&2
  exit 1
fi
printf '%s\n' "$WORK_OUT" | grep -q "timed out"
printf '%s\n' "$WORK_OUT" | grep -q "active_works=1"

echo "===== case 4: --pump-limit overflow returns non-zero ====="
set +e
LIM_OUT=$(HOST_ASYNC_EXPECT_TIMEOUT=limit "$HOST" --provider "$PROVIDER" \
  --addon "$ADDON" --driver "$REPO/test/nodeapi/probes/host_async_driver.js" \
  --pump-limit 1 2>&1)
RC=$?
set -e
echo "$LIM_OUT"
if [ "$RC" -eq 0 ]; then
  echo "run_host_async_probe: expected non-zero exit on --pump-limit overflow" >&2
  exit 1
fi
printf '%s\n' "$LIM_OUT" | grep -q "exceeded limit"

echo "run_host_async_probe: ALL PASS"
