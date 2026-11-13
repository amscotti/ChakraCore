#!/usr/bin/env python3
"""node-addon-api test matrix runner (plan card N5.3, stretch).

Builds a selected subset of the upstream ``node-addon-api`` C++ test suite
(pinned at tag ``v8.9.2``, commit
``2faf3294972dfcb3707fd5e28d451485f19e3dad``; vendored subset under
``test/nodeapi/vendor/node-addon-api/``) as per-test addons with
``zig c++`` against the vendored Node-API headers at explicit
``NAPI_VERSION=8`` (no network, no node-gyp), then runs each JS driver
through the ``ch-nodeapi`` provider host (real provider, not differential)
plus a ``node-direct`` (real Node) differential for comparison.

Every suite test ends pass / skip-with-reason / fail; the results JSON
(``test/nodeapi/results/node-addon-api.json``) records one entry per
(test, host) plus one skip entry per non-runnable suite driver, so there
are no silent skips. Skip taxonomy:

- ``needs-gc`` — driver drives ``testUtil.runGCTests`` (async GC loop);
  the host cannot run that harness (its async failures cannot be
  attributed — see below).
- ``needs-runtime-global`` — driver needs Node runtime surfaces
  (``child_process`` / ``async_hooks`` / ``worker_threads`` / ``Buffer``
  global / ``semver`` / ``fs-extra`` / ``process.version(s)`` gating).
- ``needs-node-api-version>8`` — target needs ``NAPI_EXPERIMENTAL`` /
  SharedArrayBuffer (absent from the v8 provider set).
- ``provider-gap`` — provider/host surface absent (async-work / TSFN /
  host loop services, build variants, async test bodies whose failure
  cannot be attributed through the host promise pump, upstream harness).

Why not adopt the upstream harness wholesale (plan N5.3): upstream's
``test/common/index.js`` ``runTest`` is ``async``, loads four binding
variants (``binding.node``, ``binding_noexcept.node``,
``binding_noexcept_maybe.node``, ``binding_custom_namespace.node``) via
``require.resolve``, and requires Node builtins (``node:fs/promises``,
``child_process``, ``readline``). The host's own require (verified in
``tools/nodeapi/ch-nodeapi.cpp`` `host_require_cb`) resolves ``assert``
via ``NODE_PATH`` shims, relative ``./x`` / ``../x`` via the driver
directory, and ``*.node`` / ``*/build/*`` to the live addon exports.
The runner therefore follows the existing js-native-api pattern: the
verbatim driver source runs as-is, and a tiny shim
(``test/nodeapi/shims/napi-common.js``) is materialized as
``test/common.js`` in the copied work tree, shadowing the vendored
upstream harness for both hosts. The shim's ``runTest`` calls
``test(globalThis.__exports)`` synchronously (or, under real Node where
that global is absent, ``require('./build/Release/binding.node')`` with
the per-test addon placed there). The differential path uses real
Node; the provider path uses ``ch-nodeapi`` + ``libChakraNodeApi``.

Async-driver discipline (verified during N5.3, ``rej1..3`` probes):
the Chakra promise pump swallows unhandled rejections and async-throw
failures (exit ``0``, no output), so an ``async function test()``
driver cannot distinguish pass from fail through this host. Any driver
with an async test body is pre-classified ``provider-gap`` rather than
run and reported as a (false) pass. ``mustCall`` count checks are
no-ops in the shim (passthrough); no runnable driver uses them
(grep-verified at the pin).

Also pre-classified: drivers gating on ``process.version(s)`` /
``process.release`` (``symbol.js`` / ``type_taggable.js`` /
``version_management.js``) — the host's ``process`` shim exposes
``env``/``cwd``/``argv``/``platform`` only, so those gates throw under
the host. They are ``needs-runtime-global`` skips, not failures.

Build discipline: every ``zig c++`` invocation goes through the
serialized gate lock (``/tmp/opencode/cc-gate-lock.sh`` or
``$CC_GATE_LOCK`` + ``mise exec``, mirroring
``tools/nodeapi/run_probe.sh``). Nothing is built by node-gyp/npm and
nothing is fetched at run time.

Quick start
-----------
    python3 test/nodeapi/run_node_addon_api_matrix.py --list
    python3 test/nodeapi/run_node_addon_api_matrix.py
    python3 test/nodeapi/run_matrix.py --verify-manifest   # vendor hashes

Exit code: 0 unless any test fails.
"""

from __future__ import annotations

import argparse
import json
import os
import platform
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

THIS_FILE = Path(__file__).resolve()
NODEAPI_TEST_DIR = THIS_FILE.parent
REPO_ROOT = NODEAPI_TEST_DIR.parent.parent
VENDOR_DIR = NODEAPI_TEST_DIR / "vendor"
NA_DIR = VENDOR_DIR / "node-addon-api"
SHIMS_DIR = NODEAPI_TEST_DIR / "shims"
HEADERS_INC = REPO_ROOT / "include" / "nodeapi"
HOST_CANDIDATES = [REPO_ROOT / "tools" / "nodeapi" / "ch-nodeapi",
                   REPO_ROOT / "zig-out" / "bin" / "ch-nodeapi"]
PROVIDER_PATH = (REPO_ROOT / "zig-out" / "lib" / "libChakraNodeApi.so"
                 if sys.platform.startswith("linux")
                 else REPO_ROOT / "zig-out" / "lib" / "libChakraNodeApi.dylib")
RESULTS_DIR = NODEAPI_TEST_DIR / "results"
DEFAULT_RESULTS = RESULTS_DIR / "node-addon-api.json"

NAPI_VERSION = 8
NA_TAG = "v8.9.2"
NA_COMMIT = "2faf3294972dfcb3707fd5e28d451485f19e3dad"
MISE_NODE_22 = Path("/Users/ascotti/.local/share/mise/installs/node/22.22.3/bin/node")

NEEDS_GC = "needs-gc"
NEEDS_RUNTIME = "needs-runtime-global"
NEEDS_VERSION = "needs-node-api-version>8"
PROVIDER_GAP = "provider-gap"

# ---------------------------------------------------------------------------
# Selected subset: (group -> (binding key, init fn, sources, drivers)).
# Sources/drivers are relative to vendor/node-addon-api/.
# ---------------------------------------------------------------------------

GROUPS: dict[str, dict] = {
    "basic_types_number": {
        "key": "basic_types_number", "init": "InitBasicTypesNumber",
        "sources": ["test/basic_types/number.cc"],
        "drivers": ["test/basic_types/number.js"],
    },
    "basic_types_boolean": {
        "key": "basic_types_boolean", "init": "InitBasicTypesBoolean",
        "sources": ["test/basic_types/boolean.cc"],
        "drivers": ["test/basic_types/boolean.js"],
    },
    "basic_types_array": {
        "key": "basic_types_array", "init": "InitBasicTypesArray",
        "sources": ["test/basic_types/array.cc"],
        "drivers": ["test/basic_types/array.js"],
    },
    "basic_types_value": {
        "key": "basic_types_value", "init": "InitBasicTypesValue",
        "sources": ["test/basic_types/value.cc"],
        "drivers": ["test/basic_types/value.js"],
    },
    "object": {
        "key": "object", "init": "InitObject",
        "sources": [
            "test/object/delete_property.cc",
            "test/object/finalizer.cc",
            "test/object/get_property.cc",
            "test/object/has_own_property.cc",
            "test/object/has_property.cc",
            "test/object/object.cc",
            "test/object/set_property.cc",
            "test/object/subscript_operator.cc",
        ],
        "drivers": [
            "test/object/object.js",
            "test/object/get_property.js",
            "test/object/set_property.js",
            "test/object/has_property.js",
            "test/object/delete_property.js",
            "test/object/has_own_property.js",
            "test/object/subscript_operator.js",
        ],
    },
    "object_freeze_seal": {
        "key": "object_freeze_seal", "init": "InitObjectFreezeSeal",
        "sources": ["test/object/object_freeze_seal.cc"],
        "drivers": ["test/object/object_freeze_seal.js"],
    },
    "object_deprecated": {
        "inits": [("object_deprecated", "InitObjectDeprecated"),
                ("object", "InitObject")],
        "sources": [
            "test/object/object_deprecated.cc",
            "test/object/delete_property.cc",
            "test/object/finalizer.cc",
            "test/object/get_property.cc",
            "test/object/has_own_property.cc",
            "test/object/has_property.cc",
            "test/object/object.cc",
            "test/object/set_property.cc",
            "test/object/subscript_operator.cc",
        ],
        "drivers": ["test/object/object_deprecated.js"],
    },
    "globalObject": {
        "key": "globalObject", "init": "InitGlobalObject",
        "sources": [
            "test/globalObject/global_object.cc",
            "test/globalObject/global_object_delete_property.cc",
            "test/globalObject/global_object_get_property.cc",
            "test/globalObject/global_object_has_own_property.cc",
            "test/globalObject/global_object_set_property.cc",
        ],
        "drivers": [
            "test/globalObject/global_object_get_property.js",
            "test/globalObject/global_object_set_property.js",
            "test/globalObject/global_object_has_own_property.js",
            "test/globalObject/global_object_delete_property.js",
        ],
    },
    "error_handling_for_primitives": {
        "key": "errorHandlingPrim", "init": "InitErrorHandlingPrim",
        "sources": ["test/error_handling_for_primitives.cc"],
        "drivers": ["test/error_handling_for_primitives.js"],
    },
    "objectwrap_multiple_inheritance": {
        "key": "objectwrap_multiple_inheritance",
        "init": "InitObjectWrapMultipleInheritance",
        "sources": ["test/objectwrap_multiple_inheritance.cc"],
        "drivers": ["test/objectwrap_multiple_inheritance.js"],
    },
    "function": {
        "key": "function", "init": "InitFunction",
        "sources": ["test/function.cc"],
        "drivers": ["test/function.js"],
    },
    "typedarray": {
        "key": "typedarray", "init": "InitTypedArray",
        "sources": ["test/typedarray.cc"],
        "drivers": ["test/typedarray.js", "test/typedarray-bigint.js"],
    },
    "dataview": {
        "key": "dataview", "init": "InitDataView",
        "sources": ["test/dataview/dataview.cc"],
        "drivers": ["test/dataview/dataview.js"],
    },
    "dataview_read_write": {
        "key": "dataview_read_write", "init": "InitDataViewReadWrite",
        "sources": ["test/dataview/dataview_read_write.cc"],
        "drivers": ["test/dataview/dataview_read_write.js"],
    },
    "name": {
        "key": "name", "init": "InitName",
        "sources": ["test/name.cc"],
        "drivers": ["test/name.js"],
    },
    "bigint": {
        "key": "bigint", "init": "InitBigInt",
        "sources": ["test/bigint.cc"],
        "drivers": ["test/bigint.js"],
    },
    "date": {
        "key": "date", "init": "InitDate",
        "sources": ["test/date.cc"],
        "drivers": ["test/date.js"],
    },
    "handlescope": {
        "key": "handlescope", "init": "InitHandleScope",
        "sources": ["test/handlescope.cc"],
        "drivers": ["test/handlescope.js"],
    },
    "memory_management": {
        "key": "memory_management", "init": "InitMemoryManagement",
        "sources": ["test/memory_management.cc"],
        "drivers": ["test/memory_management.js"],
    },
}

# Skip map for the REST of the upstream suite at the pin (classified from
# driver source evidence; sources not vendored — see receipt).
# runner id -> (reason, detail). Runnable ids are NOT in this map.
SKIPS: dict[str, tuple[str, str]] = {
    # needs-gc: drivers use testUtil.runGCTests (async GC loop drive).
    "test/array_buffer.js": (
        NEEDS_GC, "driver uses testUtil.runGCTests (async, gc()-driven)"),
    "test/buffer.js": (
        NEEDS_RUNTIME, "driver uses testUtil.runGCTests and the Node "
        "Buffer global (Buffer.alloc); provider has no JS Buffer global "
        "(PR-6)"),
    "test/external.js": (
        NEEDS_RUNTIME, "driver requires child_process and uses "
        "testUtil.runGCTests"),
    "test/finalizer_order.js": (
        NEEDS_GC, "driver uses testUtil.runGCTests (async, gc()-driven)"),
    "test/movable_callbacks.js": (
        NEEDS_GC, "driver uses testUtil.runGCTests and an async test body"),
    "test/object/finalizer.js": (
        NEEDS_GC, "driver uses testUtil.runGCTests (async, gc()-driven)"),
    "test/object_reference.js": (
        NEEDS_GC, "driver uses testUtil.runGCTests (async, gc()-driven)"),
    "test/objectwrap.js": (
        NEEDS_GC, "driver uses testUtil.runGCTests (async, gc()-driven)"),
    "test/objectwrap_constructor_exception.js": (
        NEEDS_GC, "driver uses testUtil.runGCTests (async, gc()-driven)"),
    "test/objectwrap_removewrap.js": (
        NEEDS_GC, "driver uses testUtil.runGCTests (async, gc()-driven)"),
    "test/reference.js": (
        NEEDS_GC, "driver uses testUtil.runGCTests (async, gc()-driven)"),
    "test/run_script.js": (
        NEEDS_GC, "driver uses testUtil.runGCTests (async, gc()-driven)"),
    # needs-runtime-global: Node runtime surfaces.
    "test/addon.js": (
        NEEDS_RUNTIME, "driver uses runTestInChildProcess (spawns child "
        "node + stderr capture)"),
    "test/addon_data.js": (
        NEEDS_RUNTIME, "driver uses runTestInChildProcess"),
    "test/addon_build/index.js": (
        NEEDS_RUNTIME, "driver requires child_process + fs-extra "
        "(npm) to compile+load in a subprocess"),
    "test/async_context.js": (
        NEEDS_RUNTIME, "driver requires async_hooks (async test body)"),
    "test/async_progress_queue_worker.js": (
        NEEDS_RUNTIME, "driver requires async_hooks (async test body)"),
    "test/async_progress_worker.js": (
        NEEDS_RUNTIME, "driver requires async_hooks (async test body)"),
    "test/async_worker.js": (
        NEEDS_RUNTIME, "driver requires async_hooks (async test body)"),
    "test/callbackscope.js": (
        NEEDS_RUNTIME, "driver requires async_hooks"),
    "test/env_cleanup.js": (
        NEEDS_RUNTIME, "driver requires ./napi_child (spawns child node)"),
    "test/error.js": (
        NEEDS_RUNTIME, "driver requires ./napi_child (spawns child node)"),
    "test/error_terminating_environment.js": (
        NEEDS_RUNTIME, "driver requires ./napi_child + worker_threads "
        "(spawns child node/worker)"),
    "test/exports.js": (
        NEEDS_RUNTIME, "driver requires semver and ../package.json "
        "(npm surface)"),
    "test/function_reference.js": (
        NEEDS_RUNTIME, "driver requires async_hooks (async test body)"),
    "test/maybe/index.js": (
        NEEDS_RUNTIME, "driver requires ../napi_child (spawns child node)"),
    "test/napi_child.js": (
        NEEDS_RUNTIME, "child-process launcher (node index.js harness "
        "entry, not a test driver)"),
    "test/objectwrap_function.js": (
        NEEDS_RUNTIME, "driver uses runTestInChildProcess"),
    "test/objectwrap_worker_thread.js": (
        NEEDS_RUNTIME, "driver requires worker_threads (async test body)"),
    "test/require_basic_finalizers/index.js": (
        NEEDS_RUNTIME, "driver requires child_process + fs-extra "
        "(npm)"),
    "test/symbol.js": (
        NEEDS_RUNTIME, "driver gates on process.versions.node.split "
        "('.', host process shim exposes env/cwd/argv/platform only)"),
    "test/thunking_manual.js": (
        NEEDS_GC, "driver drives GC directly via global.gc()\n"
        "        (global.gc is not a function under real Node without "
        "        --expose-gc, and the host cannot attribute GC-ordering "
        "        failures to exit codes)"),
    "test/type_taggable.js": (
        NEEDS_RUNTIME, "driver gates on process.versions.node.split "
        "('.', host process shim exposes env/cwd/argv/platform only)"),
    "test/version_management.js": (
        NEEDS_RUNTIME, "driver reads process.versions / process.release "
        "(host process shim exposes env/cwd/argv/platform only)"),
    "test/value_type_cast.js": (
        NEEDS_RUNTIME, "driver requires ./napi_child (type-check build "
        "variant stays out of the per-test mini-binding subset)"),
    "test/child_processes/addon.js": (
        NEEDS_RUNTIME, "driver is a child-process worker entry "
        "(runTestInChildProcess suite)"),
    "test/child_processes/addon_data.js": (
        NEEDS_RUNTIME, "driver is a child-process worker entry"),
    "test/child_processes/objectwrap_function.js": (
        NEEDS_RUNTIME, "driver is a child-process worker entry using "
        "testUtil"),
    "test/child_processes/threadsafe_function_exception.js": (
        NEEDS_RUNTIME, "driver is a child-process worker entry"),
    "test/child_processes/typed_threadsafe_function_exception.js": (
        NEEDS_RUNTIME, "driver is a child-process worker entry"),
    "test/threadsafe_function/threadsafe_function_unref.js": (
        NEEDS_RUNTIME, "driver requires ../napi_child (spawns child node)"),
    # needs-node-api-version>8: experimental SharedArrayBuffer.
    "test/shared_array_buffer.js": (
        NEEDS_VERSION, "SharedArrayBuffer API is NAPI_EXPERIMENTAL "
        "(node_api_*sharedarraybuffer*), absent at NAPI_VERSION=8"),
    # provider-gap: async-work / TSFN host loop services (PR-7).
    "test/async_worker_nocallback.js": (
        PROVIDER_GAP, "async-work + async test body needs host loop "
        "services the test host does not inject (PR-7)"),
    "test/async_worker_persistent.js": (
        PROVIDER_GAP, "async-work needs host loop services the test "
        "host does not inject (PR-7)"),
    "test/threadsafe_function/threadsafe_function.js": (
        PROVIDER_GAP, "TSFN + async test body needs host loop/services "
        "(PR-7); producer-thread semantics untestable through the host "
        "without them"),
    "test/threadsafe_function/threadsafe_function_ctx.js": (
        PROVIDER_GAP, "TSFN needs host loop/services (PR-7)"),
    "test/threadsafe_function/threadsafe_function_exception.js": (
        PROVIDER_GAP, "TSFN + runTestInChildProcess needs host "
        "loop/services (PR-7)"),
    "test/threadsafe_function/threadsafe_function_existing_tsfn.js": (
        PROVIDER_GAP, "TSFN needs host loop/services (PR-7)"),
    "test/threadsafe_function/threadsafe_function_ptr.js": (
        PROVIDER_GAP, "TSFN needs host loop/services (PR-7)"),
    "test/threadsafe_function/threadsafe_function_sum.js": (
        PROVIDER_GAP, "TSFN needs host loop/services (PR-7)"),
    "test/typed_threadsafe_function/typed_threadsafe_function.js": (
        PROVIDER_GAP, "TSFN needs host loop/services (PR-7)"),
    "test/typed_threadsafe_function/typed_threadsafe_function_ctx.js": (
        PROVIDER_GAP, "TSFN needs host loop/services (PR-7)"),
    "test/typed_threadsafe_function/typed_threadsafe_function_exception.js": (
        PROVIDER_GAP, "TSFN needs host loop/services (PR-7)"),
    "test/typed_threadsafe_function/typed_threadsafe_function_existing_tsfn.js": (
        PROVIDER_GAP, "TSFN needs host loop/services (PR-7)"),
    "test/typed_threadsafe_function/typed_threadsafe_function_ptr.js": (
        PROVIDER_GAP, "TSFN needs host loop/services (PR-7)"),
    "test/typed_threadsafe_function/typed_threadsafe_function_sum.js": (
        PROVIDER_GAP, "TSFN needs host loop/services (PR-7)"),
    "test/typed_threadsafe_function/typed_threadsafe_function_unref.js": (
        PROVIDER_GAP, "TSFN needs host loop/services (PR-7)"),
    # provider-gap: async test bodies (host cannot attribute failure).
    "test/callbackInfo.js": (
        PROVIDER_GAP, "async test body: Chakra promise pump swallows "
        "unhandled rejection (N5.3 rej1-3 probes), so a failing async "
        "body cannot be attributed through the host exit code"),
    "test/promise.js": (
        PROVIDER_GAP, "async test body + mustCall counts: same "
        "attribution limit as callbackInfo (see above)"),
    # provider-gap: build variants (not one mini-binding).
    "test/except_all.js": (
        PROVIDER_GAP, "binding_except_all build variant (whole-addon "
        "except-all semantics), not a per-test mini-binding"),
    "test/index.js": (
        PROVIDER_GAP, "upstream test harness (fs, napi_child, "
        "matchModules) — harness not adopted wholesale (N5.3)"),
}

VARIANT_COPIES = ["binding_noexcept.node", "binding_noexcept_maybe.node",
                  "binding_custom_namespace.node"]


def host_platform() -> str:
    name = sys.platform
    machine = platform.machine().lower()
    machine = {"x86_64": "x64", "amd64": "x64", "arm64": "arm64",
               "aarch64": "arm64"}.get(machine, machine)
    return f"{name}-{machine}"


def runnable_ids() -> list[str]:
    out = []
    for g in GROUPS.values():
        out.extend(g["drivers"])
    return sorted(out)


def gate_lock_command(*args: str) -> list[str]:
    gate = os.environ.get("CC_GATE_LOCK") or "/tmp/opencode/cc-gate-lock.sh"
    return [gate, "mise", "exec", "--", *args]


def find_node(explicit: str | None) -> Path | None:
    if explicit:
        p = Path(explicit)
        return p if p.is_file() else None
    for cand in [os.environ.get("NODEAPI_NODE") or None, MISE_NODE_22, "node"]:
        if cand:
            p = Path(str(cand))
            if p.is_file():
                return p
            found = shutil.which(str(cand))
            if found:
                return Path(found)
    return None


def find_host() -> Path | None:
    for cand in [REPO_ROOT / "tools" / "nodeapi" / "ch-nodeapi",
                 REPO_ROOT / "zig-out" / "bin" / "ch-nodeapi"]:
        if cand.is_file() and os.access(cand, os.X_OK):
            return cand
    return None


# ---------------------------------------------------------------------------
# Mini binding (generated per run; not vendored).
# ---------------------------------------------------------------------------

def mini_binding_source(entries: list[tuple[str, str]]) -> str:
    sets = "".join(
        f'  exports.Set("{key}", {init}(env));\n' for key, init in entries)
    decls = "".join(f'Object {init}(Env env);\n' for _, init in entries)
    return (
        '#include "napi.h"\n\n'
        'using namespace Napi;\n\n' + decls + '\n'
        'Object Init(Env env, Object exports) {\n'
        + sets +
        '  exports.Set("isExperimental", Boolean::New(env, false));\n'
        '  exports.Set("hasSharedArrayBuffer", Boolean::New(env, false));\n'
        '  return exports;\n'
        '}\n\n'
        'NODE_API_MODULE(addon, Init)\n'
    )


# ---------------------------------------------------------------------------
# Build + run (offline; gate-locked).
# ---------------------------------------------------------------------------

def copy_vendor_tree(work_root: Path) -> Path:
    """Copy the vendored NA tree; returns the NA root (work_test)."""
    work_test = work_root / "test"
    if work_test.exists():
        shutil.rmtree(work_test)
    shutil.copytree(NA_DIR, work_test)
    # Materialize the shared shim as test/common.js (shadows upstream
    # test/common/index.js for both hosts; see napi-common.js header).
    shutil.copy2(SHIMS_DIR / "napi-common.js", work_test / "test" / "common.js")
    return work_test


def build_group(work_root: Path, work_test: Path, group: str, spec: dict,
                out_addon: Path) -> tuple[bool, str]:
    """Compile one group's sources + generated mini binding. Gate-locked."""
    mini = work_root / "groups" / group / "binding_mini.cc"
    mini.parent.mkdir(parents=True, exist_ok=True)
    mini.write_text(mini_binding_source(spec.get("inits", [(spec.get("key"), spec.get("init"))])))
    sources = [str(work_test / s) for s in spec["sources"]] + [str(mini)]
    if sys.platform.startswith("linux"):
        cxxflags = [
            "c++", "-target", "x86_64-linux-gnu",
            "-std=gnu++17", "-O2", "-fPIC", "-shared",
            f"-DNAPI_VERSION={NAPI_VERSION}",
            "-DNODE_GYP_MODULE_NAME=addon",
            f"-I{HEADERS_INC}", f"-I{work_test}",
            f"-I{work_test / 'test' / 'common'}",
            "-isystem", "/usr/include",
        ]
    else:
        target = "aarch64-macos.26.0"
        sdk = subprocess.run(["xcrun", "--show-sdk-path"],
                             capture_output=True, text=True).stdout.strip()
        cxxflags = [
            "c++", "-target", target, "--sysroot", sdk,
            "-std=gnu++17", "-O2", "-fPIC", "-shared",
            f"-DNAPI_VERSION={NAPI_VERSION}",
            "-DNODE_GYP_MODULE_NAME=addon",
            f"-I{HEADERS_INC}", f"-I{work_test}",
            f"-I{work_test / 'test' / 'common'}",
        ]
        if sys.platform == "darwin":
            cxxflags += ["-undefined", "dynamic_lookup"]
    cmd = gate_lock_command("zig", *cxxflags, "-o", str(out_addon), *sources)
    try:
        proc = subprocess.run(cmd, capture_output=True, text=True,
                              timeout=600)
    except subprocess.TimeoutExpired:
        return False, "build timed out after 600s (gate-lock queue)"
    log = (proc.stdout or "") + (proc.stderr or "")
    ok = proc.returncode == 0 and out_addon.is_file() and out_addon.stat().st_size > 0
    return ok, log


def place_addon(work_test: Path, addon: Path) -> Path:
    """Install as test/build/Release/{binding,<variants>}.node (upstream layout)."""
    rel = work_test / "test" / "build" / "Release"
    rel.mkdir(parents=True, exist_ok=True)
    dest = rel / "binding.node"
    shutil.copy2(addon, dest)
    for name in VARIANT_COPIES:
        shutil.copy2(addon, rel / name)
    return dest


def run_ch_nodeapi(host: Path, provider: Path, addon: Path, driver: Path,
                   timeout: int) -> tuple[int, str, str]:
    env = dict(os.environ)
    env["NODE_PATH"] = str(SHIMS_DIR) + (
        os.pathsep + env["NODE_PATH"] if env.get("NODE_PATH") else "")
    env["NODEAPI_BUILD_DIR"] = str(addon.parent)
    cmd = [str(host), "--provider", str(provider),
           "--addon", str(addon), "--driver", str(driver)]
    proc = subprocess.run(cmd, capture_output=True, text=True,
                          timeout=timeout, cwd=str(driver.parent), env=env)
    return proc.returncode, (proc.stdout or ""), (proc.stderr or "")


def run_node_direct(node: Path, driver: Path, cwd: Path,
                    timeout: int) -> tuple[int, str, str]:
    proc = subprocess.run([str(node), str(driver)], capture_output=True,
                          text=True, timeout=timeout, cwd=str(cwd))
    return proc.returncode, (proc.stdout or ""), (proc.stderr or "")


def main() -> int:
    parser = argparse.ArgumentParser(
        description="node-addon-api subset matrix runner (N5.3)")
    parser.add_argument("--test", action="append", default=[],
                        metavar="SUBSTR",
                        help="only run entries whose test id contains SUBSTR")
    parser.add_argument("--node", default=None,
                        help="Node binary for the differential "
                             "(default: mise Node 22.22.3, then 'node')")
    parser.add_argument("--results", default=None,
                        help=f"results JSON file (default: {DEFAULT_RESULTS})")
    parser.add_argument("--timeout", type=int, default=60,
                        help="per-test timeout in seconds (default: 60)")
    parser.add_argument("--hosts", default="both",
                        choices=["both", "ch-nodeapi", "node-direct"],
                        help="which hosts to run each runnable test on "
                             "(default: both)")
    parser.add_argument("--keep-build", action="store_true",
                        help="keep temp build dirs (diagnostics)")
    parser.add_argument("--list", action="store_true",
                        help="list entries + classification, then exit")
    args = parser.parse_args()

    runnable = runnable_ids()
    skip_ids = sorted(SKIPS)
    if args.test:
        runnable = [t for t in runnable if any(n in t for n in args.test)]
        skip_ids = [t for t in skip_ids if any(n in t for n in args.test)]

    if args.list:
        for tid in runnable:
            print(f"RUN   {tid}  (provider ch-nodeapi + node-direct)")
        for tid in skip_ids:
            reason, detail = SKIPS[tid]
            print(f"SKIP  {tid}  [{reason}] {detail}")
        print(f"\nentries={len(runnable) + len(skip_ids)} "
              f"runnable={len(runnable)} skipped={len(skip_ids)} "
              f"(+{len(runnable)} differential records when --hosts=both)")
        return 0

    plat = host_platform()
    host = find_host()
    provider = PROVIDER_PATH if PROVIDER_PATH.is_file() else None
    node = find_node(args.node)

    results: list[dict] = []

    def record_skip(tid: str) -> None:
        reason, detail = SKIPS[tid]
        results.append({"test": tid, "platform": plat, "status": "skip",
                        "reason": reason, "detail": detail,
                        "host": "none", "build": "skipped",
                        "group": "unselected"})

    def run_entries(work_root: Path) -> None:
        work_test = copy_vendor_tree(work_root)
        built: dict[str, Path | None] = {}  # group -> compiled .node
        placed: dict[str, Path] = {}        # group -> placed copy
        group_of = {}
        for name, spec in GROUPS.items():
            for d in spec["drivers"]:
                group_of[d] = name
        for tid in runnable:
            group = group_of[tid]
            spec = GROUPS[group]
            if group not in built:
                out = work_root / "built" / f"{group}.node"
                out.parent.mkdir(parents=True, exist_ok=True)
                ok, log = build_group(work_root, work_test, group, spec,
                                      out)
                if ok:
                    built[group] = out
                else:
                    built[group] = None
                    # One build-failure record per driver in the group.
                    for d in spec["drivers"]:
                        results.append(
                            {"test": d, "platform": plat, "status": "fail",
                             "reason": "build failed",
                             "detail": log.strip()[-1500:],
                             "host": "ch-nodeapi", "build": "failed",
                             "group": group})
            addon = built[group]
            if addon is None:
                continue
            # Re-place the group's addon so test/build/Release matches
            # this driver (groups overwrite that path; cache would
            # otherwise leave another group's binary in place).
            if placed.get(group) != addon:
                placed[group] = place_addon(work_test, addon)
            addon = placed[group]
            driver = work_test / tid
            if args.hosts in ("both", "ch-nodeapi"):
                rec = {"test": tid, "platform": plat, "group": group,
                       "host": "ch-nodeapi", "build": "ok"}
                if host is None or provider is None:
                    rec.update(status="skip", reason=PROVIDER_GAP,
                               detail="ch-nodeapi host binary and/or "
                                      "zig-out/lib/libChakraNodeApi.so (Linux) / .dylib (macOS) "
                                      "not found (build via gate-locked "
                                      "`zig build ch-nodeapi`)")
                else:
                    try:
                        code, out, err = run_ch_nodeapi(
                            host, provider, addon, driver, args.timeout)
                    except subprocess.TimeoutExpired:
                        results.append({**rec, "status": "fail",
                                        "reason": "timeout",
                                        "detail": f"exceeded {args.timeout}s"})
                        continue
                    tail = ((out or "") + "\n" + (err or "")).strip()
                    if code == 0:
                        rec.update(status="pass", reason="",
                                   detail=tail[-1500:])
                    else:
                        rec.update(status="fail",
                                   reason=f"exit {code}",
                                   detail=tail[-1500:])
                results.append(rec)
            if args.hosts in ("both", "node-direct"):
                rec = {"test": tid, "platform": plat, "group": group,
                       "host": "node-direct", "build": "ok"}
                if node is None:
                    rec.update(status="skip", reason=PROVIDER_GAP,
                               detail="node binary not found (set "
                                      "NODEAPI_NODE or install mise Node "
                                      "22.22.3)")
                else:
                    try:
                        code, out, err = run_node_direct(node, driver,
                                                         work_test,
                                                         args.timeout)
                    except subprocess.TimeoutExpired:
                        results.append({**rec, "status": "fail",
                                        "reason": "timeout",
                                        "detail": f"exceeded {args.timeout}s"})
                        continue
                    tail = ((out or "") + "\n" + (err or "")).strip()
                    if code == 0:
                        rec.update(status="pass", reason="",
                                   detail=tail[-1500:])
                    else:
                        rec.update(status="fail",
                                   reason=f"exit {code}",
                                   detail=tail[-1500:])
                results.append(rec)
        for tid in skip_ids:
            record_skip(tid)

    if args.keep_build:
        kept = tempfile.mkdtemp(prefix="nodeapi-na-")
        print(f"build workdir kept at {kept} (--keep-build)")
        run_entries(Path(kept))
    else:
        with tempfile.TemporaryDirectory(prefix="nodeapi-na-") as tmp:
            run_entries(Path(tmp))

    document = {
        "platform": plat,
        "napiVersion": NAPI_VERSION,
        "nodeAddonApiTag": NA_TAG,
        "nodeAddonApiCommit": NA_COMMIT,
        "vendor": "test/nodeapi/vendor/node-addon-api",
        "nodeVersion": "22.22.3 (mise pin, differential host only)",
        "results": results,
    }
    results_path = Path(args.results) if args.results else DEFAULT_RESULTS
    results_path.parent.mkdir(parents=True, exist_ok=True)
    results_path.write_text(json.dumps(document, indent=2) + "\n",
                            encoding="utf-8")

    counts: dict[str, int] = {}
    for rec in results:
        counts[rec["status"]] = counts.get(rec["status"], 0) + 1
        symbol = {"pass": "PASS", "skip": "SKIP", "fail": "FAIL"}.get(
            rec["status"], "?")
        reason = f" [{rec['reason']}]" if rec.get("reason") else ""
        print(f"{symbol} {rec['group']}/{rec['host'] if rec.get('host') != 'none' else ''} {rec['test']}{reason}")
    print(f"\nplatform={plat} node-addon-api={NA_TAG} "
          f"pass={counts.get('pass', 0)} skip={counts.get('skip', 0)} "
          f"fail={counts.get('fail', 0)}")
    print(f"results => {results_path}")
    return 1 if counts.get("fail", 0) else 0


if __name__ == "__main__":
    raise SystemExit(main())
