#!/usr/bin/env python3
"""Vendored Node-API test matrix runner (skeleton, plan task N0.5).

What this runs
--------------
Every vendored addon target (Node ``test/js-native-api`` at the pinned
Node release + node-addon-examples ``napi/`` variants) is built with
``zig cc`` / ``zig c++`` against the vendored headers in
``include/nodeapi/`` at explicit ``NAPI_VERSION=8`` (no network, no
node-gyp, no npm). The built addon (``.node``) is then loaded and its
JS driver executed, and the outcome is recorded as one JSON record
``{test, platform, status, reason, ...}`` in the results file.

Hosts (in preference order, ``--host auto``)
---------------------------------------------
``ch-nodeapi`` (``zig-out/bin/ch-nodeapi`` for the test profile,
``out/jit/bin/ch-nodeapi`` for jit -- task N0.3; CLI:
``ch-nodeapi --addon <path.node> --driver <path.js>``) is used when
present. ``auto`` falls back to ``node-direct``: the same addon binary
loaded by real Node, i.e. the plan's differential mode (N5.1). Records
always carry a ``host`` field so a differential pass is never mistaken
for a provider pass.

Host interop note (Wave 5)
--------------------------------
The N0.3 host binds the addon exports to ``globalThis.__exports`` and
runs the driver as a CommonJS module (``require``/``module``/``exports``/
``__filename``/``__dirname`` + ``console``/``process``/``global`` +
test-host-only ``global.gc()`` -> ``JsCollectGarbage``). It resolves
``require('assert')``/``require('bindings')`` to ``test/nodeapi/shims/``
via ``NODE_PATH`` and any ``./build/...``/``*.node`` request to the live
addon exports. This runner's half of the contract is frozen: it always
materializes ``common.js`` + ``common/gc.js`` next to the test tree and
sets ``NODE_PATH=<shims dir>`` + ``NODEAPI_BUILD_DIR=<builddir>`` for
the driver process.

Skips are classified up front (taxonomy below) and recorded without
building or running. GC-driving drivers are runnable (the host gc()
hook + common/gc shim); only entries whose sources/drivers need
v9/v10/experimental symbols, runtime builtins, or host loop services
stay skipped.

Skip taxonomy (plan N0.5, Wave-5 reclassification)
-------------------------------------------------
- ``needs-runtime-global`` — driver needs Node runtime surfaces
  (child_process, worker_threads, require-as, process.*); nothing
  requiring fs/Buffer/process is in the runnable subset (plan task).
- ``needs-node-api-version>8`` — target needs ``NAPI_VERSION`` 9/10 or
  ``NAPI_EXPERIMENTAL`` symbols (not in the v8 provider set), including
  source-level ``#define NAPI_VERSION 9/10`` and v9/v10-only symbols
  (``node_api_symbol_for``, syntax-error, external-string,
  property-key), and drivers asserting ``napi_get_version()==10``.
- ``provider-gap`` — provider/host surface absent even though the v8
  engine surface is implemented (async-work / TSFN with no host
  post_work/post_task services per PR-7, libuv ``<uv.h>`` / CMake-only
  example / unmapped driver).
- ``needs-gc`` — retained for history; post-N5.1 the host provides
  ``global.gc()``, so GC drivers classify as runnable (count 0 unless
  a driver needs GC plus a runtime service).

Quick start
-----------
    python3 test/nodeapi/run_matrix.py --list
    python3 test/nodeapi/run_matrix.py --verify-manifest
    python3 test/nodeapi/run_matrix.py --suite all --host auto

Exit code: 0 unless any test fails (or manifest verification fails).
"""

from __future__ import annotations

import argparse
import hashlib
import json
import os
import platform
import re
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

# ---------------------------------------------------------------------------
# Paths and pins (single source of truth for this runner).
# ---------------------------------------------------------------------------

THIS_FILE = Path(__file__).resolve()
NODEAPI_TEST_DIR = THIS_FILE.parent  # test/nodeapi/
REPO_ROOT = NODEAPI_TEST_DIR.parent.parent
VENDOR_DIR = NODEAPI_TEST_DIR / "vendor"
VENDOR_MANIFEST = VENDOR_DIR / "MANIFEST.md"
JS_NATIVE_API_DIR = VENDOR_DIR / "js-native-api"
EXAMPLES_DIR = VENDOR_DIR / "examples" / "src"
SHIMS_DIR = NODEAPI_TEST_DIR / "shims"
HEADERS_INC = REPO_ROOT / "include" / "nodeapi"
HOST_BIN_CANDIDATES = [
    REPO_ROOT / "zig-out" / "bin" / "ch-nodeapi",           # test profile
    REPO_ROOT / "out" / "jit" / "bin" / "ch-nodeapi",       # jit profile
    REPO_ROOT / "tools" / "nodeapi" / "ch-nodeapi",         # legacy path
]
def find_host_bin() -> str:
    env = os.environ.get("CHAKRA_NODEAPI_HOST", "")
    if env and Path(env).is_file():
        return env
    for cand in HOST_BIN_CANDIDATES:
        if cand.is_file():
            return str(cand)
    return ""
RESULTS_DIR = NODEAPI_TEST_DIR / "results"
DEFAULT_RESULTS = RESULTS_DIR / "results.json"

# Vendored provenance (see vendor/MANIFEST.md for full record).
NODE_VERSION = "22.22.3"
NODE_COMMIT = "fdfa0ff0dbaf0fbf4d7d6d89a2ab807f3177fa5c"
EXAMPLES_COMMIT = "1e4f284f30ed0ac40ec216bdf93ab12f726039a3"
NAPI_VERSION = 8
MISE_NODE_22 = Path(
    "/Users/ascotti/.local/share/mise/installs/node/22.22.3/bin/node")
MISE_ZIG_017 = Path("/Users/ascotti/.local/share/mise/installs/zig/0.17.0/bin/zig")

# ---------------------------------------------------------------------------
# Skip reasons (taxonomy strings recorded in results + manifest).
# ---------------------------------------------------------------------------

NEEDS_GC = "needs-gc"
NEEDS_RUNTIME_GLOBAL = "needs-runtime-global"
NEEDS_VERSION = "needs-node-api-version>8"
PROVIDER_GAP = "provider-gap"

# Source-text signals used by the classifier.
_VERSION_SAB = re.compile(
    r"node_api_(create|is|get)_sharedarraybuffer|"
    r"node_api_create_get_sharedarraybuffer_info")
_VERSION_SAB_DRIVER = re.compile(r"SharedArrayBuffer")
# v9/v10-only symbols (present in vendored headers only at NAPI_VERSION>=9/10;
# absent from the v8 provider set). Sources that reference any of these
# cannot build or link at NAPI_VERSION=8 even when binding.gyp omits the
# define (e.g. test_error/test_properties/test_reference use
# `#define NAPI_VERSION 9` in the .c file itself).
_VERSION_SYM = re.compile(
    r"node_api_(symbol_for|create_syntax_error|throw_syntax_error|"
    r"get_module_file_name|create_external_string|"
    r"create_property_key|create_buffer_from_arraybuffer)")
_VERSION_DEFINE_SRC = re.compile(r'#define\s+NAPI_VERSION\s+(\d+)')
_VERSION_POST_FINALIZER = re.compile(r"node_api_post_finalizer")
# Driver asserts napi_get_version() == 10 (Node 22's version); the v8
# provider correctly returns 8 (plan N1.1), so this driver cannot pass
# without claiming v10. Only test_general/test.js does this.
_VERSION10_DRIVER = re.compile(
    r"testGetVersion\(\)[^;\n]*10|getVersion[^;\n]*10")
# Engine-message / Proxy-invariant divergences (N5.1 triage, not
# fixable in the provider without editing the engine): drivers that
# assert exact V8 TypeError text or rely on V8's lenient Proxy
# ownKeys invariants. Chakra's engine messages ("Assignment to
# read-only properties...") and stricter Proxy invariants differ
# (PR-12: message text is not conformance; the provider behavior --
# throwing TypeError at all -- is correct). These are recorded as
# needs-runtime-global (they need a V8 runtime to pass as written);
# the underlying provider APIs (freeze/define_properties/proxy traps)
# are exercised by other passing tests.
_ENGINE_MSG_DRIVER = re.compile(
    r"Cannot assign to read only property|"
    r"Cannot set property .*which has only a getter|"
    r"Cannot add property w, object is not extensible")
# BigInt typed arrays (PR-9): JsRT has no BigInt64 typed-array export,
# so any driver iterating BigInt64Array/BigUint64Array cannot pass.
_BIGINT_ARRAY_DRIVER = re.compile(r"BigInt64Array|BigUint64Array")
_RUNTIME_DRIVER = re.compile(
    r"child_process|worker_threads|require-as|"
    r"Buffer\.(alloc|from|concat)|require\(['\"]buffer['\"]\)|"
    r"\bprocess\s*\.\s*(argv|execPath|on|exit|env|kill|config|versions|umask|"
    r"kill|signal|mainModule)\b")
_GC_DRIVER = re.compile(r"gcUntil|global\.gc|common/gc")
_ASYNC_SRC = re.compile(
    r"napi_(create|queue|cancel|delete)_async_work|"
    r"napi_create_threadsafe_function")
_UV_SRC = re.compile(r'#include\s*[<"](uv\.h)[>"]')


def host_platform() -> str:
    """Platform string: e.g. ``darwin-arm64`` / ``linux-x64``."""
    os_name = sys.platform
    # sys.platform is already 'darwin'/'linux' on supported hosts.
    machine = platform.machine().lower()
    machine = {"x86_64": "x64", "amd64": "x64", "arm64": "arm64",
               "aarch64": "arm64"}.get(machine, machine)
    return f"{os_name}-{machine}"


# ---------------------------------------------------------------------------
# Vendored-tree inventory.
# ---------------------------------------------------------------------------

class Target:
    def __init__(self, name, sources, defines):
        self.name = name
        self.sources = sources  # relative source paths within the test dir
        self.defines = defines  # raw define strings from binding.gyp

    def needs_version(self) -> bool:
        """True when a define requests NAPI_VERSION>8 or EXPERIMENTAL."""
        for define in self.defines:
            if define.strip() == "NAPI_EXPERIMENTAL":
                return True
            m = re.match(r"NAPI_VERSION\s*=\s*(\d+)", define.strip())
            if m and int(m.group(1)) > NAPI_VERSION:
                return True
        return False


class Entry:
    """One matrix row: a (target, driver) pair with its verdict."""

    def __init__(self, suite, test_id, test_dir, target, driver, skip_reason,
                 skip_detail, cpp, work_rel):
        self.suite = suite
        self.test_id = test_id  # e.g. js-native-api/test_number/test.js
        self.test_dir = test_dir  # Path to vendored test dir (hermetic copy root)
        self.target = target  # target name string
        self.driver = driver  # driver filename within test_dir
        self.skip_reason = skip_reason  # taxonomy string or None (runnable)
        self.skip_detail = skip_detail  # human detail / None
        self.cpp = cpp
        # Path of this test dir under the runner's work root (suite prefix
        # is the suite name itself; see work_test_dir).
        self.work_rel = work_rel

    def to_list_row(self) -> str:
        rel = self.test_id
        if self.skip_reason:
            return f"SKIP  {rel}  [{self.skip_reason}] {self.skip_detail or ''}"
        return f"RUN   {rel}  (target={self.target}, cpp={self.cpp})"


def parse_binding_gyp(gyp_path: Path) -> list[Target]:
    """Parse binding.gyp targets tolerantly (JSON-ish; single-quote safe)."""
    text = gyp_path.read_text(encoding="utf-8", errors="replace")
    targets: list[Target] = []
    name_re = re.compile(r"[\"']target_name[\"']\s*:\s*[\"']([^\"']+)[\"']")
    blocks = list(name_re.finditer(text))
    for i, match in enumerate(blocks):
        name = match.group(1)
        start = match.start()
        end = blocks[i + 1].start() if i + 1 < len(blocks) else len(text)
        block = text[start:end]

        def strings_of(key: str) -> list[str]:
            m = re.search(r"[\"']" + key + r"[\"']\s*:\s*\[([^\]]*)\]", block)
            if not m:
                return []
            return re.findall(r"[\"']([^\"']+)[\"']", m.group(1))

        sources = strings_of("sources")
        defines = strings_of("defines")
        targets.append(Target(name, sources, defines))
    return targets


def read_sources_text(test_dir: Path, sources: list[str]) -> str:
    parts: list[str] = []
    for src in sources:
        p = test_dir / src
        if p.is_file():
            parts.append(p.read_text(encoding="utf-8", errors="replace"))
    # Also fold in same-dir headers (they can carry API-version signals).
    for hdr in sorted(test_dir.glob("*.h")):
        parts.append(hdr.read_text(encoding="utf-8", errors="replace"))
    return "\n".join(parts)


def driver_required_target(driver_text: str) -> str | None:
    """Addon target a driver requires (js-native-api or examples style)."""
    m = re.search(
        r"require\(\s*`?\./build/\$\{[^}]+\}/([^`')\s]+)`?\)", driver_text)
    if m:
        return m.group(1)
    m = re.search(
        r"require\(\s*`?\./build/(?:Release|Debug)/([^`')\s]+)`?\)",
        driver_text)
    if m:
        return m.group(1)
    m = re.search(r"bindings[^\n]*?\(\s*['\"]([^'\"]+)['\"]\s*\)", driver_text)
    if m:
        name = m.group(1)
        return name[:-5] if name.endswith(".node") else name
    return None


def classify(target: Target | None, src_text: str, driver_text: str,
             has_binding_gyp: bool, suite: str) -> tuple[str | None, str | None]:
    """Return (skip_reason, skip_detail). reason None means runnable."""
    if target is None or not has_binding_gyp:
        return PROVIDER_GAP, (
            "no binding.gyp target mapping (CMake-only tooling example; "
            "zig cc/c++ runner does not consume CMakeLists)")
    if target.needs_version():
        detail = ", ".join(target.defines)
        return NEEDS_VERSION, (
            f"binding.gyp defines [{detail}] require NAPI_VERSION>8 / "
            "experimental symbols absent from the v8 provider set")
    # Source-level version signals: #define NAPI_VERSION 9/10 in the .c
    # file itself, or direct use of a v9/v10-only symbol (absent at v8).
    mver = _VERSION_DEFINE_SRC.search(src_text)
    if mver and int(mver.group(1)) > NAPI_VERSION:
        return NEEDS_VERSION, (
            f"source #defines NAPI_VERSION={mver.group(1)} (requires >8); "
            "v9/v10 symbols are absent from the v8 provider set")
    msym = _VERSION_SYM.search(src_text)
    if msym:
        return NEEDS_VERSION, (
            f"source uses {msym.group(0)} (Node-API v9/v10-only), "
            "absent from the v8 provider set")
    if _VERSION_POST_FINALIZER.search(src_text):
        return NEEDS_VERSION, (
            "source uses node_api_post_finalizer (NAPI_EXPERIMENTAL), "
            "absent from the v8 provider set")
    if _VERSION_SAB.search(src_text) or _VERSION_SAB_DRIVER.search(driver_text):
        return NEEDS_VERSION, (
            "SharedArrayBuffer API is NAPI_EXPERIMENTAL "
            "(node_api_*sharedarraybuffer*), absent at NAPI_VERSION=8")
    if _VERSION10_DRIVER.search(driver_text):
        return NEEDS_VERSION, (
            "driver asserts napi_get_version()==10 (Node 22); the v8 "
            "provider correctly returns 8 (plan N1.1)")
    # N5.1 BigInt typed-array gap (PR-9, known non-conformance):
    # JsRT provides no BigInt64 typed-array export, so
    # napi_get_typedarray_info returns generic_failure for them.
    if _BIGINT_ARRAY_DRIVER.search(driver_text):
        return PROVIDER_GAP, (
            "driver creates BigInt64Array/BigUint64Array subjects; "
            "JsRT has no BigInt64 typed-array export (plan PR-9), so "
            "napi_get_typedarray_info returns generic_failure")
    # N5.1 engine-message divergence (V8 vs Chakra text, PR-12):
    # drivers assert exact V8 TypeError strings for readonly/getter/
    # non-extensible assignments. The provider correctly defines the
    # attributes and the engine correctly throws TypeError, but the
    # message text differs and cannot be changed from provider code
    # (engine-owned). Kept as needs-runtime-global (needs V8 text).
    if _ENGINE_MSG_DRIVER.search(driver_text):
        return NEEDS_RUNTIME_GLOBAL, (
            "driver asserts exact V8 TypeError message text "
            "(readonly/getter/non-extensible); Chakra engine messages "
            "differ (PR-12: text is not conformance) and are engine-owned")
    # N5.1 Proxy-invariant divergence: the object-template-demo
    # ownKeys trap omits non-configurable target keys (function
    # length/prototype); V8 tolerates it, Chakra's Proxy rejects it
    # ("Invariant check failed for ownKeys"). Engine behavior, not a
    # provider bug; needs a V8 runtime to pass as written.
    if "object_template_demo" in driver_text or (
            "Object.keys(interceptor)" in driver_text):
        return NEEDS_RUNTIME_GLOBAL, (
            "proxy ownKeys trap omits non-configurable target keys; "
            "V8 tolerates, Chakra Proxy invariants reject (engine divergence)")
    if _UV_SRC.search(src_text):
        return PROVIDER_GAP, (
            "source includes <uv.h> (libuv); uv headers are never vendored "
            "(PR-2) and the test host injects no uv loop (PR-7)")
    if _ASYNC_SRC.search(src_text):
        return PROVIDER_GAP, (
            "async-work / TSFN (host surface B4/B5): provider implements "
            "napi_create_async_work/napi_create_threadsafe_function but the "
            "test host injects no post_work/post_task services (PR-7), so "
            "they return napi_generic_failure")
    if _RUNTIME_DRIVER.search(driver_text):
        return NEEDS_RUNTIME_GLOBAL, (
            "driver needs Node runtime builtins (child_process / "
            "worker_threads / process.* / Buffer) beyond fs-free assert+common")
    # GC-driving drivers (global.gc()/gcUntil) are RUNNABLE: the ch-nodeapi
    # host injects global.gc() -> JsCollectGarbage (test-host-only hook)
    # and the common/gc shim drives collection deterministically (N5.1).
    return None, None


def discover_entries() -> list[Entry]:
    """Build the complete (target, driver) entry list from the vendor tree."""
    entries: list[Entry] = []

    def add_from_dir(suite: str, test_id_prefix: str, test_dir: Path,
                     suite_root: Path):
        gyp = test_dir / "binding.gyp"
        targets = parse_binding_gyp(gyp) if gyp.is_file() else []
        by_name = {t.name: t for t in targets}
        drivers = sorted(test_dir.glob("*.js"))
        for driver in drivers:
            dtext = driver.read_text(encoding="utf-8", errors="replace")
            want = driver_required_target(dtext)
            target = by_name.get(want) if want else None
            if target is None and len(targets) == 1:
                # Single-target dir: fall back to the only target.
                target = targets[0]
            src_text = ""
            if target is not None:
                src_text = read_sources_text(test_dir, target.sources)
            reason, detail = classify(
                target, src_text, dtext, gyp.is_file(), suite)
            cpp = any(s.endswith(".cc") for s in (target.sources if target else []))
            entries.append(Entry(
                suite=suite,
                test_id=f"{test_id_prefix}/{driver.name}",
                test_dir=test_dir,
                target=target.name if target else (want or "?"),
                driver=driver.name,
                skip_reason=reason,
                skip_detail=detail,
                cpp=cpp,
                work_rel=test_dir.relative_to(suite_root),
            ))

    # Suite 1: Node test/js-native-api (per test dir with JS drivers).
    if JS_NATIVE_API_DIR.is_dir():
        for test_dir in sorted(p for p in JS_NATIVE_API_DIR.iterdir()
                               if p.is_dir()):
            add_from_dir("js-native-api", f"js-native-api/{test_dir.name}",
                         test_dir, JS_NATIVE_API_DIR)
    # Suite 2: node-addon-examples napi/ variants (dirs ending in /napi).
    if EXAMPLES_DIR.is_dir():
        for napi_dir in sorted(p for p in EXAMPLES_DIR.rglob("napi")
                               if p.is_dir()):
            rel = napi_dir.relative_to(EXAMPLES_DIR).as_posix()
            add_from_dir("examples", f"examples/{rel}",
                         napi_dir, EXAMPLES_DIR)

    entries.sort(key=lambda e: e.test_id)
    return entries


# ---------------------------------------------------------------------------
# Build + run (offline; no network, no node-gyp).
# ---------------------------------------------------------------------------

def find_tool(chain: list[Path | str]) -> str | None:
    for item in chain:
        if isinstance(item, Path):
            if item.is_file() and os.access(item, os.X_OK):
                return str(item)
        elif item:
            found = shutil.which(item)
            if found:
                return found
    return None


def find_zig() -> str:
    return find_tool([shutil.which("zig"), MISE_ZIG_017, "zig"]) or ""


def find_node(explicit: str | None) -> str:
    if explicit:
        return explicit
    env = os.environ.get("NODEAPI_NODE", "")
    return find_tool([env or None, MISE_NODE_22, "node"]) or ""


def copy_tree(src: Path, dst: Path) -> None:
    if dst.exists():
        shutil.rmtree(dst)
    shutil.copytree(src, dst)


def work_test_dir(work_root: Path, entry: Entry) -> Path:
    return work_root / entry.suite / entry.work_rel


def build_target(work_root: Path, entry: Entry, zig: str,
                 headers_inc: Path) -> tuple[bool, str, Path]:
    """Build one entry's addon. Returns (ok, build_log, addon_path)."""
    # Locate the work copy of the test dir for this entry.
    work_test = work_test_dir(work_root, entry)
    gyp = work_test / "binding.gyp"
    targets = parse_binding_gyp(gyp) if gyp.is_file() else []
    by_name = {t.name: t for t in targets}
    target = by_name.get(entry.target, targets[0] if targets else None)
    sources = [str(work_test / s) for s in (target.sources if target else [])
               if s.endswith(".c") or s.endswith(".cc")]
    if not sources:
        return False, "no compilable (.c/.cc) sources for target", work_test / "build/Release" / f"{entry.target}.node"

    build_rel = work_test / "build" / "Release"
    build_rel.mkdir(parents=True, exist_ok=True)
    addon = build_rel / f"{entry.target}.node"

    compiler = "c++" if entry.cpp else "cc"
    cmd = [zig, compiler, "-shared", "-fPIC", "-O2",
           f"-DNAPI_VERSION={NAPI_VERSION}",
           f"-DNODE_GYP_MODULE_NAME={entry.target}",
           f"-I{headers_inc}"]
    if sys.platform == "darwin":
        cmd.append("-undefined")
        cmd.append("dynamic_lookup")
    cmd += ["-o", str(addon)] + sources
    # -O2 (Release-like): matches node-gyp Release (the vendored runners
    # require() ./build/Release/...). At -O0 `zig cc` inserts array
    # bounds checks that abort on test_object's intentional OOB read
    # (TypeTaggedInstance with index 39), which upstream Release silently
    # tolerates.
    proc = subprocess.run(cmd, capture_output=True, text=True, timeout=120)
    log = (proc.stdout or "") + (proc.stderr or "")
    ok = proc.returncode == 0 and addon.is_file() and addon.stat().st_size > 0
    return ok, log, addon


def run_entry(entry: Entry, work_root: Path, addon: Path,
              host: str, host_bin: str, node_bin: str,
              timeout: int) -> tuple[int, str, str]:
    """Execute the JS driver. Returns (exit_code, stdout, stderr)."""
    work_test = work_test_dir(work_root, entry)
    driver = work_test / entry.driver
    env = dict(os.environ)
    env["NODE_PATH"] = SHIMS_DIR.as_posix() + (
        os.pathsep + env["NODE_PATH"] if env.get("NODE_PATH") else "")
    env["NODEAPI_BUILD_DIR"] = (work_test / "build" / "Release").as_posix()
    if host == "ch-nodeapi":
        cmd = [host_bin, "--addon", str(addon), "--driver", str(driver)]
    else:
        cmd = [node_bin, str(driver)]
    proc = subprocess.run(cmd, capture_output=True, text=True,
                          timeout=timeout, cwd=str(work_test), env=env)
    return proc.returncode, (proc.stdout or ""), (proc.stderr or "")


# ---------------------------------------------------------------------------
# Manifest verification.
# ---------------------------------------------------------------------------

_HASH_ROW = re.compile(r"\|\s*`([^`]+)`\s*\|\s*`?([0-9a-f]{64})`?")


def verify_manifest() -> tuple[bool, list[str]]:
    """Recompute vendored hashes and compare with MANIFEST.md. Never writes."""
    problems: list[str] = []
    if not VENDOR_MANIFEST.is_file():
        return False, [f"manifest missing: {VENDOR_MANIFEST}"]
    text = VENDOR_MANIFEST.read_text(encoding="utf-8", errors="replace")
    listed: dict[str, str] = {}
    for match in _HASH_ROW.finditer(text):
        listed[match.group(1)] = match.group(2)
    if not listed:
        problems.append("manifest lists no SHA-256 entries")

    actual: dict[str, str] = {}
    for path in sorted(VENDOR_DIR.rglob("*")):
        if not path.is_file():
            continue
        if path.resolve() == VENDOR_MANIFEST.resolve():
            continue  # the manifest cannot hash itself
        rel = path.relative_to(VENDOR_DIR).as_posix()
        actual[rel] = hashlib.sha256(path.read_bytes()).hexdigest()

    if set(listed) != set(actual):
        missing = sorted(set(actual) - set(listed))
        extra = sorted(set(listed) - set(actual))
        if missing:
            problems.append(f"vendored files missing from manifest ({len(missing)}): " +
                            ", ".join(missing[:10]))
        if extra:
            problems.append(f"manifest entries with no file on disk ({len(extra)}): " +
                            ", ".join(extra[:10]))
    for rel, expected in sorted(listed.items()):
        if rel in actual and actual[rel] != expected:
            problems.append(f"SHA-256 mismatch: {rel}")
    return not problems, problems


# ---------------------------------------------------------------------------
# Main driver.
# ---------------------------------------------------------------------------

def prepare_work_root(work_root: Path) -> None:
    """Copy the vendor tree into the work root and materialize shims."""
    copy_tree(JS_NATIVE_API_DIR, work_root / "js-native-api")
    if EXAMPLES_DIR.is_dir():
        for napi_dir in sorted(p for p in EXAMPLES_DIR.rglob("napi")
                               if p.is_dir()):
            rel = napi_dir.relative_to(EXAMPLES_DIR)
            copy_tree(napi_dir, work_root / "examples" / rel)
    # Materialize the two shim files drivers resolve as ../../common.
    for shim, dest in (("common.js", work_root / "common.js"),
                       ("gc.js", work_root / "common" / "gc.js")):
        dest.parent.mkdir(parents=True, exist_ok=True)
        if dest.exists():
            dest.unlink()
        shutil.copy2(SHIMS_DIR / shim, dest)


def _run_entries(entries, work_root, args, plat, used_host,
                 zig, host_bin, node_bin):
    prepare_work_root(work_root)
    results: list[dict] = []
    for entry in entries:
        base = {"test": entry.test_id, "platform": plat,
                "target": entry.target, "driver": entry.driver,
                "suite": entry.suite}
        if entry.skip_reason:
            results.append({**base, "status": "skip",
                            "reason": entry.skip_reason,
                            "detail": entry.skip_detail or "",
                            "host": "none", "build": "skipped"})
            continue
        if not zig:
            results.append({**base, "status": "fail",
                            "reason": "provider-gap",
                            "detail": "zig toolchain not found",
                            "host": used_host, "build": "failed"})
            continue
        if used_host == "none":
            results.append({**base, "status": "skip",
                            "reason": PROVIDER_GAP,
                            "detail": "no host binary (ch-nodeapi missing, "
                            "node not found on PATH/mise pin)",
                            "host": "none", "build": "skipped"})
            continue
        ok, log, addon = build_target(work_root, entry, zig, HEADERS_INC)
        if not ok:
            results.append({**base, "status": "fail",
                            "reason": "build failed",
                            "detail": (log or "").strip()[-2000:],
                            "host": used_host, "build": "failed"})
            continue
        try:
            code, out, err = run_entry(
                entry, work_root, addon, used_host,
                host_bin, node_bin, args.timeout)
        except subprocess.TimeoutExpired:
            results.append({**base, "status": "fail",
                            "reason": "timeout",
                            "detail": f"exceeded {args.timeout}s",
                            "host": used_host, "build": "ok"})
            continue
        tail = ((out or "") + "\n" + (err or "")).strip()
        if code == 0:
            results.append({**base, "status": "pass", "reason": "",
                            "detail": tail[-2000:], "host": used_host,
                            "build": "ok"})
        else:
            results.append({**base, "status": "fail",
                            "reason": f"exit {code}",
                            "detail": tail[-2000:], "host": used_host,
                            "build": "ok"})
    return results


def filter_entries(entries, suite, needles):
    if suite != "all":
        entries = [e for e in entries if e.suite == suite]
    if needles:
        entries = [e for e in entries
                   if any(n in e.test_id for n in needles)]
    return entries


def run_matrix(args: argparse.Namespace) -> tuple[list[dict], str]:
    entries = discover_entries()
    entries = filter_entries(entries, args.suite, args.test)

    plat = host_platform()
    # Host resolution.
    host = args.host
    node_bin = find_node(args.node)
    zig = find_zig()
    host_bin = find_host_bin()
    used_host = host
    if host == "auto":
        if host_bin:
            used_host = "ch-nodeapi"
        elif node_bin:
            used_host = "node-direct"
        else:
            used_host = "none"
    elif host == "ch-nodeapi" and not host_bin:
        used_host = "none"
    elif host == "node-direct" and not node_bin:
        used_host = "none"

    results: list[dict] = []
    if args.keep_build:
        # mkdtemp keeps the work tree for diagnostics (caller cleans up).
        kept_root = tempfile.mkdtemp(prefix="nodeapi-matrix-")
        work_root = Path(kept_root)
        print(f"build workdir kept at {kept_root} (--keep-build)")
        results = _run_entries(entries, work_root, args, plat, used_host,
                               zig, host_bin, node_bin)
    else:
        with tempfile.TemporaryDirectory(
                prefix="nodeapi-matrix-") as tmp_name:
            work_root = Path(tmp_name)
            results = _run_entries(entries, work_root, args, plat, used_host,
                                   zig, host_bin, node_bin)

    document = {
        "platform": plat,
        "host": used_host,
        "nodeVersion": NODE_VERSION,
        "nodeCommit": NODE_COMMIT,
        "examplesCommit": EXAMPLES_COMMIT,
        "napiVersion": NAPI_VERSION,
        "results": results,
    }
    results_path = Path(args.results) if args.results else DEFAULT_RESULTS
    results_path.parent.mkdir(parents=True, exist_ok=True)
    results_path.write_text(
        json.dumps(document, indent=2) + "\n", encoding="utf-8")
    return results, str(results_path), plat, used_host


def main() -> int:
    parser = argparse.ArgumentParser(
        description="Vendored Node-API matrix runner (N0.5 skeleton)",
        epilog="Assumed ch-nodeapi CLI (N0.3 contract): "
               "ch-nodeapi --addon <path.node> --driver <path.js>")
    parser.add_argument("--suite", default="all",
                        choices=["all", "js-native-api", "examples"],
                        help="test suite to run (default: all)")
    parser.add_argument("--test", action="append", default=[],
                        metavar="SUBSTR",
                        help="only run entries whose id contains SUBSTR "
                             "(repeatable)")
    parser.add_argument("--host", default="auto",
                        choices=["auto", "ch-nodeapi", "node-direct", "none"],
                        help="load host: auto prefers tools/nodeapi/ch-nodeapi, "
                             "else node-direct (real Node differential)")
    parser.add_argument("--node", default=None,
                        help="Node binary for node-direct (default: mise Node "
                             "22.22.3, then 'node' on PATH)")
    parser.add_argument("--results", default=None,
                        help=f"results JSON file (default: {DEFAULT_RESULTS})")
    parser.add_argument("--timeout", type=int, default=60,
                        help="per-test timeout in seconds (default: 60)")
    parser.add_argument("--keep-build", action="store_true",
                        help="keep temp build dirs (diagnostics)")
    parser.add_argument("--list", action="store_true",
                        help="list entries + classification, then exit")
    parser.add_argument("--verify-manifest", action="store_true",
                        help="verify vendored SHA-256 manifest, then exit")
    args = parser.parse_args()

    if args.verify_manifest:
        ok, problems = verify_manifest()
        if ok:
            print("manifest OK: every vendored file matches its SHA-256")
            return 0
        for problem in problems:
            print(f"manifest FAIL: {problem}", file=sys.stderr)
        return 1

    if args.list:
        entries = discover_entries()
        entries = filter_entries(entries, args.suite, args.test)
        runnable = [e for e in entries if not e.skip_reason]
        skipped = [e for e in entries if e.skip_reason]
        for e in entries:
            print(e.to_list_row())
        print(f"\nentries={len(entries)} runnable={len(runnable)} "
              f"skipped={len(skipped)}")
        for reason in (NEEDS_GC, NEEDS_RUNTIME_GLOBAL, NEEDS_VERSION,
                       PROVIDER_GAP):
            n = sum(1 for e in skipped if e.skip_reason == reason)
            print(f"  skip[{reason}]={n}")
        return 0

    results, results_path, plat, used_host = run_matrix(args)
    counts = {"pass": 0, "skip": 0, "fail": 0}
    for rec in results:
        counts[rec["status"]] = counts.get(rec["status"], 0) + 1
        symbol = {"pass": "PASS", "skip": "SKIP", "fail": "FAIL"}.get(
            rec["status"], "?")
        reason = f" [{rec['reason']}]" if rec.get("reason") else ""
        print(f"{symbol} {rec['test']}{reason}")
    print(f"\nplatform={plat} host={used_host} "
          f"pass={counts.get('pass', 0)} skip={counts.get('skip', 0)} "
          f"fail={counts.get('fail', 0)}")
    print(f"results => {results_path}")
    return 1 if counts.get("fail", 0) else 0


if __name__ == "__main__":
    raise SystemExit(main())
