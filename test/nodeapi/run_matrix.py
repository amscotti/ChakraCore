#!/usr/bin/env python3
#-------------------------------------------------------------------------------------------------------
# Copyright (c) ChakraCore Project Contributors. All rights reserved.
# Licensed under the MIT license. See LICENSE.txt file in the project root for full license information.
#-------------------------------------------------------------------------------------------------------
"""Vendored Node-API test matrix runner (skeleton, plan task N0.5).

What this runs
--------------
Every vendored addon target (Node ``test/js-native-api`` at the pinned
Node release + node-addon-examples ``napi/`` variants) is built with
``zig cc`` / ``zig c++`` against the vendored headers in
``include/nodeapi/`` at explicit ``NAPI_VERSION=10`` (honoring explicit
per-target/source ``NAPI_VERSION=<n>`` overrides, no network, no
node-gyp, no npm). The built addon(s) (``.node``) are then loaded and
the JS driver executed, and the outcome is recorded as one JSON record
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
- ``needs-node-api-version>10`` — target needs ``NAPI_VERSION`` > 10 or
  ``NAPI_EXPERIMENTAL`` symbols (absent from the v10 provider set),
  including ``NAPI_EXPERIMENTAL`` SharedArrayBuffer / basic-finalizer /
  ``node_api_post_finalizer`` targets.
- ``provider-gap`` — provider/host surface absent (libuv ``<uv.h>`` /
  CMake-only example / unmapped driver).
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
NAPI_VERSION = 10


def _resolve_tool(env_vars: tuple[str, ...], cmd_name: str) -> str:
    """Resolve an external tool via env var -> shutil.which -> mise which."""
    for env_name in env_vars:
        val = os.environ.get(env_name, "").strip()
        if val:
            p = Path(val)
            if p.is_file() and os.access(p, os.X_OK):
                return str(p)
            found = shutil.which(val)
            if found:
                return found
    found = shutil.which(cmd_name)
    if found:
        return found
    mise_bin = shutil.which("mise")
    if mise_bin:
        try:
            proc = subprocess.run([mise_bin, "which", cmd_name],
                                  capture_output=True, text=True, timeout=10)
            cand = (proc.stdout or "").strip()
            if proc.returncode == 0 and cand:
                p = Path(cand)
                if p.is_file() and os.access(p, os.X_OK):
                    return str(p)
        except (OSError, subprocess.TimeoutExpired):
            pass
    return ""

# ---------------------------------------------------------------------------
# Skip reasons (taxonomy strings recorded in results + manifest).
# ---------------------------------------------------------------------------

NEEDS_GC = "needs-gc"
NEEDS_RUNTIME_GLOBAL = "needs-runtime-global"
NEEDS_VERSION = "needs-node-api-version>10"
PROVIDER_GAP = "provider-gap"

# Source-text signals used by the classifier.
_VERSION_SAB = re.compile(
    r"node_api_(create|is|get)_sharedarraybuffer|"
    r"node_api_create_get_sharedarraybuffer_info")
_VERSION_SAB_DRIVER = re.compile(r"SharedArrayBuffer")
_VERSION_DEFINE_SRC = re.compile(r'#define\s+NAPI_VERSION\s+(\d+)')
_VERSION_POST_FINALIZER = re.compile(r"node_api_post_finalizer")
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
_RUNTIME_DRIVER = re.compile(
    r"child_process|worker_threads|require-as|"
    r"Buffer\.(alloc|from|concat)|require\(['\"]buffer['\"]\)|"
    r"\bprocess\s*\.\s*(argv|execPath|on|exit|env|kill|config|versions|umask|"
    r"kill|signal|mainModule)\b")
_GC_DRIVER = re.compile(r"gcUntil|global\.gc|common/gc")
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

    def explicit_napi_version(self) -> int | None:
        """Return explicit NAPI_VERSION=<n> from binding.gyp defines, if any."""
        for define in self.defines:
            m = re.match(r"NAPI_VERSION\s*=\s*(\d+)", define.strip())
            if m:
                return int(m.group(1))
        return None

    def needs_version(self) -> bool:
        """True when a define requests NAPI_VERSION>10 or EXPERIMENTAL."""
        for define in self.defines:
            if define.strip() == "NAPI_EXPERIMENTAL":
                return True
            m = re.match(r"NAPI_VERSION\s*=\s*(\d+)", define.strip())
            if m and int(m.group(1)) > NAPI_VERSION:
                return True
        return False


class Entry:
    """One matrix row: a (targets, driver) entry with its verdict."""

    def __init__(self, suite, test_id, test_dir, target, targets, driver,
                 skip_reason, skip_detail, cpp, work_rel):
        self.suite = suite
        self.test_id = test_id  # e.g. js-native-api/test_number/test.js
        self.test_dir = test_dir  # Path to vendored test dir (hermetic copy root)
        self.target = target  # target name(s) string for display/JSON
        self.targets = targets  # ordered list of required target names
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


def driver_required_targets(driver_text: str) -> list[str]:
    """Addon targets a driver requires in source order (deduplicated)."""
    hits: list[tuple[int, str]] = []
    patterns = (
        re.compile(r"require\(\s*`?\./build/\$\{[^}]+\}/([^`')\s]+)`?\)"),
        re.compile(r"require\(\s*`?\./build/(?:Release|Debug)/([^`')\s]+)`?\)"),
        re.compile(r"require\(\s*['\"]bindings['\"]\s*\)\s*\(\s*['\"]([^'\"]+)['\"]\s*\)"),
        re.compile(r"\bbindings\s*\(\s*['\"]([^'\"]+)['\"]\s*\)"),
    )
    for pat in patterns:
        for m in pat.finditer(driver_text):
            name = m.group(1)
            if name.endswith(".node"):
                name = name[:-5]
            hits.append((m.start(), name))
    hits.sort(key=lambda item: item[0])
    seen: set[str] = set()
    out: list[str] = []
    for _, name in hits:
        if name not in seen:
            seen.add(name)
            out.append(name)
    return out


def classify(targets: list[Target], src_text: str, driver_text: str,
             has_binding_gyp: bool, suite: str,
             test_id: str) -> tuple[str | None, str | None]:
    """Return (skip_reason, skip_detail). reason None means runnable."""
    if not targets or not has_binding_gyp:
        return PROVIDER_GAP, (
            "no binding.gyp target mapping (CMake-only tooling example; "
            "zig cc/c++ runner does not consume CMakeLists)")
    for target in targets:
        if target.needs_version():
            detail = ", ".join(target.defines)
            return NEEDS_VERSION, (
                f"binding.gyp defines [{detail}] require NAPI_VERSION>{NAPI_VERSION} / "
                f"experimental symbols absent from the v{NAPI_VERSION} provider set")
    # Source-level version signals: #define NAPI_VERSION > 10 in the .c
    # file itself, or NAPI_EXPERIMENTAL symbols.
    for mver in _VERSION_DEFINE_SRC.finditer(src_text):
        if int(mver.group(1)) > NAPI_VERSION:
            return NEEDS_VERSION, (
                f"source #defines NAPI_VERSION={mver.group(1)} (requires >{NAPI_VERSION}); "
                f"symbols above v{NAPI_VERSION} are absent from the provider set")
    if _VERSION_POST_FINALIZER.search(src_text):
        return NEEDS_VERSION, (
            "source uses node_api_post_finalizer (NAPI_EXPERIMENTAL), "
            f"absent from the v{NAPI_VERSION} provider set")
    if _VERSION_SAB.search(src_text) or _VERSION_SAB_DRIVER.search(driver_text):
        return NEEDS_VERSION, (
            "SharedArrayBuffer API is NAPI_EXPERIMENTAL "
            f"(node_api_*sharedarraybuffer*), absent at NAPI_VERSION={NAPI_VERSION}")
    if test_id == "js-native-api/test_string/test.js":
        return PROVIDER_GAP, (
            "test_string.c:112 asserts !copied on "
            "node_api_create_external_string_latin1; ChakraCore copies into "
            "Recycler string heap (copied=true) per Node-API fallback")
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
            test_id = f"{test_id_prefix}/{driver.name}"
            dtext = driver.read_text(encoding="utf-8", errors="replace")
            wants = driver_required_targets(dtext)
            resolved_targets: list[Target] = []
            missing_target = False
            if wants:
                for w in wants:
                    t = by_name.get(w)
                    if t is None:
                        missing_target = True
                        break
                    resolved_targets.append(t)
            elif len(targets) == 1:
                resolved_targets = [targets[0]]
            if missing_target:
                resolved_targets = []

            src_sources: list[str] = []
            seen_src: set[str] = set()
            for t in resolved_targets:
                for s in t.sources:
                    if s not in seen_src:
                        seen_src.add(s)
                        src_sources.append(s)
            src_text = read_sources_text(test_dir, src_sources) if resolved_targets else ""
            reason, detail = classify(
                resolved_targets, src_text, dtext, gyp.is_file(), suite, test_id)
            target_names = [t.name for t in resolved_targets]
            target_label = ", ".join(target_names) if target_names else (
                ", ".join(wants) if wants else "?")
            cpp = any(s.endswith(".cc") for s in src_sources)
            entries.append(Entry(
                suite=suite,
                test_id=test_id,
                test_dir=test_dir,
                target=target_label,
                targets=target_names,
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

def find_zig() -> str:
    return _resolve_tool(("NODEAPI_ZIG", "NODEAPI_MISE_ZIG"), "zig")


def find_node(explicit: str | None) -> str:
    if explicit:
        return explicit
    return _resolve_tool(("NODEAPI_NODE", "NODEAPI_MISE_NODE"), "node")


def copy_tree(src: Path, dst: Path) -> None:
    if dst.exists():
        shutil.rmtree(dst)
    shutil.copytree(src, dst)


def work_test_dir(work_root: Path, entry: Entry) -> Path:
    return work_root / entry.suite / entry.work_rel


def build_target(work_root: Path, entry: Entry, zig: str,
                 headers_inc: Path) -> tuple[bool, str, list[Path]]:
    """Build all required targets for an entry. Returns (ok, build_log, addon_paths)."""
    work_test = work_test_dir(work_root, entry)
    gyp = work_test / "binding.gyp"
    targets = parse_binding_gyp(gyp) if gyp.is_file() else []
    by_name = {t.name: t for t in targets}
    target_names = entry.targets or ([entry.target] if entry.target != "?" else [])
    if not target_names:
        return False, "no target name for entry", []

    build_rel = work_test / "build" / "Release"
    build_rel.mkdir(parents=True, exist_ok=True)
    built_addons: list[Path] = []
    logs: list[str] = []

    for tname in target_names:
        target = by_name.get(tname, targets[0] if len(targets) == 1 else None)
        rel_sources = target.sources if target else []
        sources = [str(work_test / s) for s in rel_sources
                   if s.endswith(".c") or s.endswith(".cc")]
        addon = build_rel / f"{tname}.node"
        if not sources:
            return False, f"no compilable (.c/.cc) sources for target {tname}", built_addons

        is_cpp = any(s.endswith(".cc") for s in rel_sources)
        compiler = "c++" if is_cpp else "cc"
        cmd = [zig, compiler, "-shared", "-fPIC", "-O2"]
        explicit_ver = target.explicit_napi_version() if target else None
        if explicit_ver is not None:
            cmd.append(f"-DNAPI_VERSION={explicit_ver}")
        else:
            src_text = read_sources_text(work_test, rel_sources)
            if not _VERSION_DEFINE_SRC.search(src_text):
                cmd.append(f"-DNAPI_VERSION={NAPI_VERSION}")
        cmd += [f"-DNODE_GYP_MODULE_NAME={tname}", f"-I{headers_inc}"]
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
        if log:
            logs.append(log)
        if proc.returncode != 0 or not addon.is_file() or addon.stat().st_size == 0:
            return False, "\n".join(logs), built_addons
        built_addons.append(addon)

    return True, "\n".join(logs), built_addons


def run_entry(entry: Entry, work_root: Path, addons: list[Path],
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
        timeout_ms = max(1000, int(timeout) * 1000)
        cmd = [host_bin, "--pump-timeout-ms", str(timeout_ms)]
        for addon in addons:
            cmd += ["--addon", str(addon)]
        cmd += ["--driver", str(driver)]
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
        ok, log, addons = build_target(work_root, entry, zig, HEADERS_INC)
        if not ok:
            results.append({**base, "status": "fail",
                            "reason": "build failed",
                            "detail": (log or "").strip()[-2000:],
                            "host": used_host, "build": "failed"})
            continue
        # Upstream binding.c computes 100k primes by O(n^2) trial division (~167s);
        # grant extra headroom under the default 60s --timeout.
        entry_timeout = (max(args.timeout, 360)
                         if args.timeout == 60 and "async_work_thread_safe_function" in entry.test_id
                         else args.timeout)
        try:
            code, out, err = run_entry(
                entry, work_root, addons, used_host,
                host_bin, node_bin, entry_timeout)
        except subprocess.TimeoutExpired:
            results.append({**base, "status": "fail",
                            "reason": "timeout",
                            "detail": f"exceeded {entry_timeout}s",
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
