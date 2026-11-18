#!/bin/sh
#-------------------------------------------------------------------------------------------------------
# Copyright (c) ChakraCore Project Contributors. All rights reserved.
# Licensed under the MIT license. See LICENSE.txt file in the project root for full license information.
#-------------------------------------------------------------------------------------------------------
# Q1.1 Node-API v9/v10 + multi-addon probe runner.
#
# 1) Builds and runs test/nodeapi/probes/v10_multi_addon_probe.cpp (standalone
#    with lib/NodeApi provider TUs) against BOTH test (zig-out) and jit
#    (out/jit) profile engine dylibs.
# 2) Builds 3 temporary .node addons in a temp dir:
#      - Addon A: v9 via NAPI_MODULE_INIT()
#      - Addon B: v10 via NAPI_MODULE_INIT()
#      - Addon C: deprecated napi_module_register (no get_api_version symbol)
#    and executes ch-nodeapi --driver ... dynamically requiring all 3 addons,
#    verifying cross-addon Buffer sharing, distinct node_api_get_module_file_name
#    URLs, isolated instance_data, and LIFO cleanup hooks across both test and
#    jit ch-nodeapi binaries.
#
# Env overrides:
#   CC_GATE_LOCK=path  Optional gate-lock wrapper script (disabled by default)
#   SKIP_BUILD=1       reuse already-built engine dylibs + ch-nodeapi

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
  SDK=""
  ENGINE_NAME="libChakraCore.so"
  PROVIDER_NAME="libChakraNodeApi.so"
  ENGINE_TEST="zig-out/bin/libChakraCore.so"
  LINK_SYSTEM="-ldl -lpthread"
  RPATH_FLAG="-Wl,-rpath,\$ORIGIN"
  CXXFLAGS="-target $TARGET -std=gnu++17 -O3 -g -Wno-date-time -DNAPI_VERSION=10 -DCHAKRA_NODEAPI_TEST_HOOKS -Ilib/Jsrt -Iinclude/nodeapi -Ilib/NodeApi -isystem /usr/include"
  ADDON_BASE="-target $TARGET -std=gnu++17 -O2 -g -fPIC -shared -Iinclude/nodeapi -isystem /usr/include"
else
  TARGET="aarch64-macos.26.0"
  SDK=$(xcrun --show-sdk-path 2>/dev/null || true)
  if [ -z "$SDK" ]; then
    echo "run_v10_multi_addon_probe: cannot locate macOS SDK (xcrun failed)" >&2
    exit 1
  fi
  ENGINE_NAME="libChakraCore.dylib"
  PROVIDER_NAME="libChakraNodeApi.dylib"
  ENGINE_TEST="zig-out/bin/libChakraCore.dylib"
  LINK_SYSTEM="-framework CoreFoundation -framework Security"
  RPATH_FLAG="-Wl,-rpath,@executable_path"
  CXXFLAGS="-target $TARGET --sysroot $SDK -std=gnu++17 -O3 -g -mcpu=apple_m1 -Wno-date-time -DNAPI_VERSION=10 -DCHAKRA_NODEAPI_TEST_HOOKS -Ilib/Jsrt -Iinclude/nodeapi -Ilib/NodeApi -isystem $SDK/usr/include -F/System/Library/Frameworks"
  ADDON_BASE="-target $TARGET --sysroot $SDK -std=gnu++17 -O2 -g -mcpu=apple_m1 -fPIC -shared -Iinclude/nodeapi -isystem $SDK/usr/include -F/System/Library/Frameworks -Wl,-undefined,dynamic_lookup"
fi

run_cpp_profile() {
  profile="$1"
  engine_lib="$2"
  out_bin="$3"

  echo "===== C++ probe profile: $profile (engine: $engine_lib) ====="
  if [ ! -f "$engine_lib" ]; then
    echo "run_v10_multi_addon_probe: engine dylib missing for $profile: $engine_lib" >&2
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
    test/nodeapi/probes/v10_multi_addon_probe.cpp \
    "$engine_lib" \
    $LINK_SYSTEM \
    $RPATH_FLAG \
    -o "$out_bin"
  cp "$engine_lib" "$(dirname "$out_bin")/$ENGINE_NAME"
  "$out_bin"
}

run_host_profile() {
  profile="$1"
  host_bin="$2"
  provider_lib="$3"
  driver_js="$4"

  echo "===== ch-nodeapi multi-addon profile: $profile ====="
  if [ ! -x "$host_bin" ] || [ ! -f "$provider_lib" ]; then
    echo "run_v10_multi_addon_probe: missing host ($host_bin) or provider ($provider_lib)" >&2
    return 1
  fi
  OUT=$(NODE_PATH="$REPO/test/nodeapi/shims" "$host_bin" \
    --provider "$provider_lib" --driver "$driver_js")
  printf '%s\n' "$OUT"
  printf '%s\n' "$OUT" | grep -q "MULTI_ADDON_JS: PASS"
  CLEANUPS=$(printf '%s\n' "$OUT" | grep '^CLEANUP:' | tr '\n' ' ')
  if [ "$CLEANUPS" != "CLEANUP:C2 CLEANUP:C1 CLEANUP:B1 CLEANUP:A2 CLEANUP:A1 " ]; then
    echo "run_v10_multi_addon_probe: unexpected LIFO cleanup order: '$CLEANUPS'" >&2
    return 1
  fi
}

WORK=$(mktemp -d "${TMPDIR:-/tmp}/v10-multi-addon-probe.XXXXXX")
WORK_TEST="$WORK/test"
WORK_JIT="$WORK/jit"
ADDONS_DIR="$WORK/addons"
mkdir -p "$WORK_TEST" "$WORK_JIT" "$ADDONS_DIR"
trap 'rm -rf "$WORK"' EXIT

if [ "${SKIP_BUILD:-0}" != "1" ]; then
  echo "===== building test-profile stack (zig build ch-nodeapi libChakraNodeApi) ====="
  run_zig build ch-nodeapi libChakraNodeApi
  if [ "$(uname -s)" != "Linux" ]; then
    echo "===== building jit-profile stack (zig build -Dprofile=jit ch-nodeapi libChakraNodeApi) ====="
    run_zig build -Dprofile=jit ch-nodeapi libChakraNodeApi
  else
    echo "===== linux: test leg only ====="
  fi
fi

cat > "$ADDONS_DIR/addon_a.cpp" <<'EOF'
#include <node_api.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

static void CleanupHook(void* arg) {
  char* tag = static_cast<char*>(arg);
  std::printf("CLEANUP:%s\n", tag ? tag : "");
  std::fflush(stdout);
  std::free(tag);
}

static void FreeInst(napi_env, void* data, void*) {
  std::free(data);
}

static napi_value GetVersion(napi_env env, napi_callback_info) {
  uint32_t v = 0;
  if (napi_get_version(env, &v) != napi_ok) return nullptr;
  napi_value out = nullptr;
  napi_create_uint32(env, v, &out);
  return out;
}

static napi_value FileName(napi_env env, napi_callback_info) {
  const char* url = nullptr;
  if (node_api_get_module_file_name(env, &url) != napi_ok || !url) return nullptr;
  napi_value out = nullptr;
  napi_create_string_utf8(env, url, NAPI_AUTO_LENGTH, &out);
  return out;
}

static napi_value SymFor(napi_env env, napi_callback_info info) {
  size_t argc = 1;
  napi_value arg = nullptr;
  if (napi_get_cb_info(env, info, &argc, &arg, nullptr, nullptr) != napi_ok || argc < 1) {
    return nullptr;
  }
  char buf[128] = {};
  size_t n = 0;
  if (napi_get_value_string_utf8(env, arg, buf, sizeof(buf), &n) != napi_ok) return nullptr;
  napi_value sym = nullptr;
  if (node_api_symbol_for(env, buf, n, &sym) != napi_ok) return nullptr;
  return sym;
}

static napi_value MakeSyntaxErr(napi_env env, napi_callback_info info) {
  size_t argc = 1;
  napi_value arg = nullptr;
  if (napi_get_cb_info(env, info, &argc, &arg, nullptr, nullptr) != napi_ok || argc < 1) {
    return nullptr;
  }
  napi_value code = nullptr;
  napi_create_string_utf8(env, "ERR_ADDON_A", NAPI_AUTO_LENGTH, &code);
  napi_value err = nullptr;
  if (node_api_create_syntax_error(env, code, arg, &err) != napi_ok) return nullptr;
  return err;
}

static napi_value MakeBuf(napi_env env, napi_callback_info info) {
  size_t argc = 1;
  napi_value arg = nullptr;
  if (napi_get_cb_info(env, info, &argc, &arg, nullptr, nullptr) != napi_ok || argc < 1) {
    return nullptr;
  }
  uint32_t len = 0;
  if (napi_get_array_length(env, arg, &len) != napi_ok) return nullptr;
  void* data = nullptr;
  napi_value buf = nullptr;
  if (napi_create_buffer(env, len, &data, &buf) != napi_ok) return nullptr;
  auto* dst = static_cast<uint8_t*>(data);
  for (uint32_t i = 0; i < len; ++i) {
    napi_value el = nullptr;
    uint32_t v = 0;
    if (napi_get_element(env, arg, i, &el) != napi_ok ||
        napi_get_value_uint32(env, el, &v) != napi_ok) {
      return nullptr;
    }
    dst[i] = static_cast<uint8_t>(v & 0xff);
  }
  return buf;
}

static napi_value IsBuf(napi_env env, napi_callback_info info) {
  size_t argc = 1;
  napi_value arg = nullptr;
  if (napi_get_cb_info(env, info, &argc, &arg, nullptr, nullptr) != napi_ok || argc < 1) {
    return nullptr;
  }
  bool b = false;
  if (napi_is_buffer(env, arg, &b) != napi_ok) return nullptr;
  napi_value out = nullptr;
  napi_get_boolean(env, b, &out);
  return out;
}

static napi_value ReadBuf(napi_env env, napi_callback_info info) {
  size_t argc = 1;
  napi_value arg = nullptr;
  if (napi_get_cb_info(env, info, &argc, &arg, nullptr, nullptr) != napi_ok || argc < 1) {
    return nullptr;
  }
  void* data = nullptr;
  size_t len = 0;
  if (napi_get_buffer_info(env, arg, &data, &len) != napi_ok) return nullptr;
  napi_value arr = nullptr;
  if (napi_create_array_with_length(env, len, &arr) != napi_ok) return nullptr;
  const auto* bytes = static_cast<const uint8_t*>(data);
  for (size_t i = 0; i < len; ++i) {
    napi_value el = nullptr;
    napi_create_uint32(env, bytes[i], &el);
    napi_set_element(env, arr, static_cast<uint32_t>(i), el);
  }
  return arr;
}

static napi_value SetInst(napi_env env, napi_callback_info info) {
  size_t argc = 1;
  napi_value arg = nullptr;
  if (napi_get_cb_info(env, info, &argc, &arg, nullptr, nullptr) != napi_ok || argc < 1) {
    return nullptr;
  }
  char buf[128] = {};
  size_t n = 0;
  if (napi_get_value_string_utf8(env, arg, buf, sizeof(buf), &n) != napi_ok) return nullptr;
  char* copy = static_cast<char*>(std::malloc(n + 1));
  std::memcpy(copy, buf, n + 1);
  if (napi_set_instance_data(env, copy, FreeInst, nullptr) != napi_ok) {
    std::free(copy);
    return nullptr;
  }
  return nullptr;
}

static napi_value GetInst(napi_env env, napi_callback_info) {
  void* data = nullptr;
  if (napi_get_instance_data(env, &data) != napi_ok || !data) return nullptr;
  napi_value out = nullptr;
  napi_create_string_utf8(env, static_cast<const char*>(data), NAPI_AUTO_LENGTH, &out);
  return out;
}

static napi_value ArmCleanup(napi_env env, napi_callback_info info) {
  size_t argc = 1;
  napi_value arg = nullptr;
  if (napi_get_cb_info(env, info, &argc, &arg, nullptr, nullptr) != napi_ok || argc < 1) {
    return nullptr;
  }
  char buf[64] = {};
  size_t n = 0;
  if (napi_get_value_string_utf8(env, arg, buf, sizeof(buf), &n) != napi_ok) return nullptr;
  char* tag = static_cast<char*>(std::malloc(n + 1));
  std::memcpy(tag, buf, n + 1);
  if (napi_add_env_cleanup_hook(env, CleanupHook, tag) != napi_ok) {
    std::free(tag);
    return nullptr;
  }
  return nullptr;
}

NAPI_MODULE_INIT() {
  napi_property_descriptor props[] = {
      {"version", nullptr, GetVersion, nullptr, nullptr, nullptr, napi_default, nullptr},
      {"fileName", nullptr, FileName, nullptr, nullptr, nullptr, napi_default, nullptr},
      {"symFor", nullptr, SymFor, nullptr, nullptr, nullptr, napi_default, nullptr},
      {"makeSyntaxErr", nullptr, MakeSyntaxErr, nullptr, nullptr, nullptr, napi_default, nullptr},
      {"makeBuf", nullptr, MakeBuf, nullptr, nullptr, nullptr, napi_default, nullptr},
      {"isBuf", nullptr, IsBuf, nullptr, nullptr, nullptr, napi_default, nullptr},
      {"readBuf", nullptr, ReadBuf, nullptr, nullptr, nullptr, napi_default, nullptr},
      {"setInst", nullptr, SetInst, nullptr, nullptr, nullptr, napi_default, nullptr},
      {"getInst", nullptr, GetInst, nullptr, nullptr, nullptr, napi_default, nullptr},
      {"armCleanup", nullptr, ArmCleanup, nullptr, nullptr, nullptr, napi_default, nullptr},
  };
  napi_define_properties(env, exports, sizeof(props) / sizeof(props[0]), props);
  return exports;
}
EOF

cat > "$ADDONS_DIR/addon_b.cpp" <<'EOF'
#include <node_api.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>

static void CleanupHook(void* arg) {
  char* tag = static_cast<char*>(arg);
  std::printf("CLEANUP:%s\n", tag ? tag : "");
  std::fflush(stdout);
  std::free(tag);
}

static void FreeInst(napi_env, void* data, void*) {
  std::free(data);
}

static void ExtFin(node_api_basic_env, void*, void* hint) {
  auto* calls = static_cast<int*>(hint);
  if (calls) (*calls)++;
}

static napi_value FileName(napi_env env, napi_callback_info) {
  const char* url = nullptr;
  if (node_api_get_module_file_name(env, &url) != napi_ok || !url) return nullptr;
  napi_value out = nullptr;
  napi_create_string_utf8(env, url, NAPI_AUTO_LENGTH, &out);
  return out;
}

static napi_value SliceFromAB(napi_env env, napi_callback_info info) {
  size_t argc = 3;
  napi_value args[3] = {};
  if (napi_get_cb_info(env, info, &argc, args, nullptr, nullptr) != napi_ok || argc < 3) {
    return nullptr;
  }
  uint32_t off = 0, len = 0;
  if (napi_get_value_uint32(env, args[1], &off) != napi_ok ||
      napi_get_value_uint32(env, args[2], &len) != napi_ok) {
    return nullptr;
  }
  napi_value out = nullptr;
  if (node_api_create_buffer_from_arraybuffer(env, args[0], off, len, &out) != napi_ok) {
    return nullptr;
  }
  return out;
}

static napi_value IsBuf(napi_env env, napi_callback_info info) {
  size_t argc = 1;
  napi_value arg = nullptr;
  if (napi_get_cb_info(env, info, &argc, &arg, nullptr, nullptr) != napi_ok || argc < 1) {
    return nullptr;
  }
  bool b = false;
  if (napi_is_buffer(env, arg, &b) != napi_ok) return nullptr;
  napi_value out = nullptr;
  napi_get_boolean(env, b, &out);
  return out;
}

static napi_value ReadBuf(napi_env env, napi_callback_info info) {
  size_t argc = 1;
  napi_value arg = nullptr;
  if (napi_get_cb_info(env, info, &argc, &arg, nullptr, nullptr) != napi_ok || argc < 1) {
    return nullptr;
  }
  void* data = nullptr;
  size_t len = 0;
  if (napi_get_buffer_info(env, arg, &data, &len) != napi_ok) return nullptr;
  napi_value arr = nullptr;
  if (napi_create_array_with_length(env, len, &arr) != napi_ok) return nullptr;
  const auto* bytes = static_cast<const uint8_t*>(data);
  for (size_t i = 0; i < len; ++i) {
    napi_value el = nullptr;
    napi_create_uint32(env, bytes[i], &el);
    napi_set_element(env, arr, static_cast<uint32_t>(i), el);
  }
  return arr;
}

static napi_value CheckExtStrAndPropKey(napi_env env, napi_callback_info) {
  char16_t raw[] = {0x0076, 0x0031, 0x0030, 0x005F, 0xD83D, 0xDE80, 0x0000};
  bool copied = false;
  int fin_calls = 0;
  napi_value s = nullptr;
  if (node_api_create_external_string_utf16(
          env, raw, NAPI_AUTO_LENGTH, ExtFin, &fin_calls, &s, &copied) != napi_ok ||
      !copied || fin_calls != 1) {
    return nullptr;
  }
  napi_value key = nullptr;
  if (node_api_create_property_key_utf8(env, "prop_v10", NAPI_AUTO_LENGTH, &key) !=
      napi_ok) {
    return nullptr;
  }
  napi_value obj = nullptr;
  if (napi_create_object(env, &obj) != napi_ok) return nullptr;
  if (napi_set_property(env, obj, key, s) != napi_ok) return nullptr;
  return obj;
}

static napi_value SetInst(napi_env env, napi_callback_info info) {
  size_t argc = 1;
  napi_value arg = nullptr;
  if (napi_get_cb_info(env, info, &argc, &arg, nullptr, nullptr) != napi_ok || argc < 1) {
    return nullptr;
  }
  char buf[128] = {};
  size_t n = 0;
  if (napi_get_value_string_utf8(env, arg, buf, sizeof(buf), &n) != napi_ok) return nullptr;
  char* copy = static_cast<char*>(std::malloc(n + 1));
  std::memcpy(copy, buf, n + 1);
  if (napi_set_instance_data(env, copy, FreeInst, nullptr) != napi_ok) {
    std::free(copy);
    return nullptr;
  }
  return nullptr;
}

static napi_value GetInst(napi_env env, napi_callback_info) {
  void* data = nullptr;
  if (napi_get_instance_data(env, &data) != napi_ok || !data) return nullptr;
  napi_value out = nullptr;
  napi_create_string_utf8(env, static_cast<const char*>(data), NAPI_AUTO_LENGTH, &out);
  return out;
}

static napi_value ArmCleanup(napi_env env, napi_callback_info info) {
  size_t argc = 1;
  napi_value arg = nullptr;
  if (napi_get_cb_info(env, info, &argc, &arg, nullptr, nullptr) != napi_ok || argc < 1) {
    return nullptr;
  }
  char buf[64] = {};
  size_t n = 0;
  if (napi_get_value_string_utf8(env, arg, buf, sizeof(buf), &n) != napi_ok) return nullptr;
  char* tag = static_cast<char*>(std::malloc(n + 1));
  std::memcpy(tag, buf, n + 1);
  if (napi_add_env_cleanup_hook(env, CleanupHook, tag) != napi_ok) {
    std::free(tag);
    return nullptr;
  }
  return nullptr;
}

NAPI_MODULE_INIT() {
  napi_property_descriptor props[] = {
      {"fileName", nullptr, FileName, nullptr, nullptr, nullptr, napi_default, nullptr},
      {"sliceFromAB", nullptr, SliceFromAB, nullptr, nullptr, nullptr, napi_default, nullptr},
      {"isBuf", nullptr, IsBuf, nullptr, nullptr, nullptr, napi_default, nullptr},
      {"readBuf", nullptr, ReadBuf, nullptr, nullptr, nullptr, napi_default, nullptr},
      {"checkExtStrAndPropKey", nullptr, CheckExtStrAndPropKey, nullptr, nullptr, nullptr, napi_default, nullptr},
      {"setInst", nullptr, SetInst, nullptr, nullptr, nullptr, napi_default, nullptr},
      {"getInst", nullptr, GetInst, nullptr, nullptr, nullptr, napi_default, nullptr},
      {"armCleanup", nullptr, ArmCleanup, nullptr, nullptr, nullptr, napi_default, nullptr},
  };
  napi_define_properties(env, exports, sizeof(props) / sizeof(props[0]), props);
  return exports;
}
EOF

cat > "$ADDONS_DIR/addon_c.cpp" <<'EOF'
#include <node_api.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>

static void CleanupHook(void* arg) {
  char* tag = static_cast<char*>(arg);
  std::printf("CLEANUP:%s\n", tag ? tag : "");
  std::fflush(stdout);
  std::free(tag);
}

static void FreeInst(napi_env, void* data, void*) {
  std::free(data);
}

static napi_value FileName(napi_env env, napi_callback_info) {
  const char* url = nullptr;
  if (node_api_get_module_file_name(env, &url) != napi_ok || !url) return nullptr;
  napi_value out = nullptr;
  napi_create_string_utf8(env, url, NAPI_AUTO_LENGTH, &out);
  return out;
}

static napi_value IsBuf(napi_env env, napi_callback_info info) {
  size_t argc = 1;
  napi_value arg = nullptr;
  if (napi_get_cb_info(env, info, &argc, &arg, nullptr, nullptr) != napi_ok || argc < 1) {
    return nullptr;
  }
  bool b = false;
  if (napi_is_buffer(env, arg, &b) != napi_ok) return nullptr;
  napi_value out = nullptr;
  napi_get_boolean(env, b, &out);
  return out;
}

static napi_value ReadBuf(napi_env env, napi_callback_info info) {
  size_t argc = 1;
  napi_value arg = nullptr;
  if (napi_get_cb_info(env, info, &argc, &arg, nullptr, nullptr) != napi_ok || argc < 1) {
    return nullptr;
  }
  void* data = nullptr;
  size_t len = 0;
  if (napi_get_buffer_info(env, arg, &data, &len) != napi_ok) return nullptr;
  napi_value arr = nullptr;
  if (napi_create_array_with_length(env, len, &arr) != napi_ok) return nullptr;
  const auto* bytes = static_cast<const uint8_t*>(data);
  for (size_t i = 0; i < len; ++i) {
    napi_value el = nullptr;
    napi_create_uint32(env, bytes[i], &el);
    napi_set_element(env, arr, static_cast<uint32_t>(i), el);
  }
  return arr;
}

static napi_value SetInst(napi_env env, napi_callback_info info) {
  size_t argc = 1;
  napi_value arg = nullptr;
  if (napi_get_cb_info(env, info, &argc, &arg, nullptr, nullptr) != napi_ok || argc < 1) {
    return nullptr;
  }
  char buf[128] = {};
  size_t n = 0;
  if (napi_get_value_string_utf8(env, arg, buf, sizeof(buf), &n) != napi_ok) return nullptr;
  char* copy = static_cast<char*>(std::malloc(n + 1));
  std::memcpy(copy, buf, n + 1);
  if (napi_set_instance_data(env, copy, FreeInst, nullptr) != napi_ok) {
    std::free(copy);
    return nullptr;
  }
  return nullptr;
}

static napi_value GetInst(napi_env env, napi_callback_info) {
  void* data = nullptr;
  if (napi_get_instance_data(env, &data) != napi_ok || !data) return nullptr;
  napi_value out = nullptr;
  napi_create_string_utf8(env, static_cast<const char*>(data), NAPI_AUTO_LENGTH, &out);
  return out;
}

static napi_value ArmCleanup(napi_env env, napi_callback_info info) {
  size_t argc = 1;
  napi_value arg = nullptr;
  if (napi_get_cb_info(env, info, &argc, &arg, nullptr, nullptr) != napi_ok || argc < 1) {
    return nullptr;
  }
  char buf[64] = {};
  size_t n = 0;
  if (napi_get_value_string_utf8(env, arg, buf, sizeof(buf), &n) != napi_ok) return nullptr;
  char* tag = static_cast<char*>(std::malloc(n + 1));
  std::memcpy(tag, buf, n + 1);
  if (napi_add_env_cleanup_hook(env, CleanupHook, tag) != napi_ok) {
    std::free(tag);
    return nullptr;
  }
  return nullptr;
}

static napi_value InitC(napi_env env, napi_value exports) {
  napi_property_descriptor props[] = {
      {"fileName", nullptr, FileName, nullptr, nullptr, nullptr, napi_default, nullptr},
      {"isBuf", nullptr, IsBuf, nullptr, nullptr, nullptr, napi_default, nullptr},
      {"readBuf", nullptr, ReadBuf, nullptr, nullptr, nullptr, napi_default, nullptr},
      {"setInst", nullptr, SetInst, nullptr, nullptr, nullptr, napi_default, nullptr},
      {"getInst", nullptr, GetInst, nullptr, nullptr, nullptr, napi_default, nullptr},
      {"armCleanup", nullptr, ArmCleanup, nullptr, nullptr, nullptr, napi_default, nullptr},
  };
  napi_define_properties(env, exports, sizeof(props) / sizeof(props[0]), props);
  return exports;
}

static napi_module g_addon_c_module = {
    NAPI_MODULE_VERSION,
    0,
    __FILE__,
    InitC,
    "addon_c",
    nullptr,
    {nullptr, nullptr, nullptr, nullptr},
};

__attribute__((constructor)) static void RegisterAddonC() {
  napi_module_register(&g_addon_c_module);
}
EOF

cat > "$ADDONS_DIR/driver.js" <<'EOF'
'use strict';
const assert = require('assert');

const a = require('./addon_a.node');
const b = require('./addon_b.node');
const c = require('./addon_c.node');

assert.strictEqual(require('./addon_a.node'), a);
assert.strictEqual(require('./addon_b.node'), b);
assert.strictEqual(require('./addon_c.node'), c);

assert.strictEqual(a.version(), 10);

const fa = a.fileName();
const fb = b.fileName();
const fc = c.fileName();
assert(fa.startsWith('file:///') && fa.endsWith('/addon_a.node'), fa);
assert(fb.startsWith('file:///') && fb.endsWith('/addon_b.node'), fb);
assert(fc.startsWith('file:///') && fc.endsWith('/addon_c.node'), fc);
assert.notStrictEqual(fa, fb);
assert.notStrictEqual(fb, fc);
assert.notStrictEqual(fa, fc);

a.setInst('inst-A');
b.setInst('inst-B');
c.setInst('inst-C');
assert.strictEqual(a.getInst(), 'inst-A');
assert.strictEqual(b.getInst(), 'inst-B');
assert.strictEqual(c.getInst(), 'inst-C');

assert.strictEqual(a.symFor('q1.shared'), Symbol.for('q1.shared'));
const syn = a.makeSyntaxErr('bad syntax');
assert(syn instanceof SyntaxError);
assert.strictEqual(syn.code, 'ERR_ADDON_A');
assert.strictEqual(syn.message, 'bad syntax');

const extObj = b.checkExtStrAndPropKey();
assert.strictEqual(extObj.prop_v10, 'v10_\u{1F680}');

const bufA = a.makeBuf([10, 20, 30, 40]);
assert.strictEqual(a.isBuf(bufA), true);
assert.strictEqual(b.isBuf(bufA), true);
assert.strictEqual(c.isBuf(bufA), true);
assert.deepStrictEqual(b.readBuf(bufA), [10, 20, 30, 40]);
assert.deepStrictEqual(c.readBuf(bufA), [10, 20, 30, 40]);
bufA[1] = 99;
assert.deepStrictEqual(a.readBuf(bufA), [10, 99, 30, 40]);
assert.deepStrictEqual(c.readBuf(bufA), [10, 99, 30, 40]);

const ab = new ArrayBuffer(8);
const view = new Uint8Array(ab);
for (let i = 0; i < 8; i++) view[i] = 50 + i;
const sliceB = b.sliceFromAB(ab, 2, 3);
assert.strictEqual(a.isBuf(sliceB), true);
assert.strictEqual(b.isBuf(sliceB), true);
assert.strictEqual(c.isBuf(sliceB), true);
assert.deepStrictEqual(a.readBuf(sliceB), [52, 53, 54]);
assert.deepStrictEqual(c.readBuf(sliceB), [52, 53, 54]);
view[3] = 77;
assert.deepStrictEqual(a.readBuf(sliceB), [52, 77, 54]);

const plain = new Uint8Array([1, 2, 3]);
assert.strictEqual(a.isBuf(plain), false);
assert.strictEqual(b.isBuf(plain), false);
assert.strictEqual(c.isBuf(plain), false);

a.armCleanup('A1');
a.armCleanup('A2');
b.armCleanup('B1');
c.armCleanup('C1');
c.armCleanup('C2');

print('MULTI_ADDON_JS: PASS');
EOF

# Compile the 3 temporary .node addons (v9 NAPI_MODULE_INIT, v10 NAPI_MODULE_INIT,
# deprecated napi_module_register).
# shellcheck disable=SC2086
run_zig c++ $ADDON_BASE -DNAPI_VERSION=9 "$ADDONS_DIR/addon_a.cpp" -o "$ADDONS_DIR/addon_a.node"
# shellcheck disable=SC2086
run_zig c++ $ADDON_BASE -DNAPI_VERSION=10 "$ADDONS_DIR/addon_b.cpp" -o "$ADDONS_DIR/addon_b.node"
# shellcheck disable=SC2086
run_zig c++ $ADDON_BASE -DNAPI_VERSION=10 "$ADDONS_DIR/addon_c.cpp" -o "$ADDONS_DIR/addon_c.node"

rc=0
run_cpp_profile "test" "$REPO/$ENGINE_TEST" "$WORK_TEST/v10-multi-addon-probe-test" || rc=$?
run_host_profile "test" "$REPO/zig-out/bin/ch-nodeapi" "$REPO/zig-out/lib/$PROVIDER_NAME" "$ADDONS_DIR/driver.js" || rc=$?
if [ "$(uname -s)" != "Linux" ]; then
  run_cpp_profile "jit" "$REPO/out/jit/bin/libChakraCore.dylib" "$WORK_JIT/v10-multi-addon-probe-jit" || rc=$?
  run_host_profile "jit" "$REPO/out/jit/bin/ch-nodeapi" "$REPO/out/jit/lib/$PROVIDER_NAME" "$ADDONS_DIR/driver.js" || rc=$?
fi

if [ "$rc" -ne 0 ]; then
  echo "run_v10_multi_addon_probe: FAILED (see output above)" >&2
  exit 1
fi
if [ "$(uname -s)" = "Linux" ]; then
  echo "run_v10_multi_addon_probe: ALL PASS (test)"
else
  echo "run_v10_multi_addon_probe: ALL PASS (test + jit)"
fi
