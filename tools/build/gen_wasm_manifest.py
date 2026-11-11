#!/usr/bin/env python3
#-------------------------------------------------------------------------------------------------------
# Copyright (c) ChakraCore Project Contributors. All rights reserved.
# Licensed under the MIT license. See LICENSE.txt file in the project root for full license information.
#-------------------------------------------------------------------------------------------------------
"""D7 feature/test manifest generator for the macOS ARM64 wasm/asm.js port.

Generates ``test/wasm-arm64-manifest.json`` from the checked-in ``rlexe.xml``
files, per ``doc/wasm-arm64-plan.md`` D7 (as amended). The manifest is the
test-selection authority: phase sets are defined over it, and regeneration
diffs are reviewed like code. Generated ``rlexe.xml`` files are never
hand-edited; this script only reads them.

Location: ``tools/build/`` follows the ``gen_tables_linux.py`` precedent —
checked-in verified generators that only read checked-in inputs and emit
deterministic outputs live with the build tooling, while ``test/`` holds the
data (rlexe files, baselines) and the runner itself.

Rules implemented (all mechanical, per D7):

* ``compile-flags`` is split on whitespace into tokens (never substring
  match). Tokens are compared case-insensitively (the tree uses
  ``-wasmthreads`` / ``-wasmfastarray-`` / ``-wasmsimd`` alongside canonical
  case). A trailing ``-`` on a flag token is its disabled form
  (``-WasmFastArray-`` != ``-WasmFastArray``).
* Token -> feature map: ``wasmthreads`` / ``essharedarraybuffer`` -> threads;
  ``wasmsignextends`` -> sign-ext; ``wasmnontrapping`` -> nontrapping;
  ``wasmmultivalue`` -> multi-value; ``wasmsimd`` -> simd; ``wasmfastarray``
  (enabled form only) -> fast-array; ``--no-wabt`` -> wabt-free, and its
  sibling default regress entry (same files + baseline, no ``--no-wabt``)
  -> wabt. Featureless (recorded, never in an exclusion list):
  ``-wasmfastarray-`` / ``-wasmsharedarrayvirtualbuffer-`` (disabled forms --
  generic-path controls), ``-wasmmathexfilter`` (inert on this platform),
  ``-wasmi64`` (legacy i64 enable; i64 is MVP), ``-nonative`` (variant
  dimension), ``-wasmassignmoduleid`` (trace helper). Any other ``-wasm*``
  token fails loudly so a new flag forces an explicit classification.
* Suite/path rules: ``wasm.simd`` -> simd; ``WasmSpec.MultiValue`` ->
  multi-value; ``features/{extends,nontrapping,threads}`` and
  ``chakra_atomic_*`` -> sign-ext / nontrapping / threads respectively
  (matched against baseline and flags text).
* Per-file overrides live here with reason strings. Initially exactly two
  groups: ``test/wasm/signextend.js`` -> sign-ext (flagless in rlexe but
  tests sign-extension ops), and the 8 ``test/wasm`` entries with no
  ``-wasm`` token (``badfuncformat``, ``inlining``, ``polyinline``,
  ``loop*``) -> non-wasm (JS/JIT tests housed in this suite; not
  wasm-feature evidence). Both counts are asserted on every run.
* Runner applicability (so the D8 arch flip cannot silently import
  entries): ``require_debugger`` -> phase ``deferred`` (out of P1/P2 until
  the debugger harness is validated on this host); ``exclude_x64`` ->
  phase ``P2-pending`` (excluded from P1, assessed individually at P2;
  the runner today forces ``arch='x64'`` on non-Windows, so these entries
  would otherwise start running once ``arch='arm64'`` is real).
* Phase is a separate axis from features: ``AsmJSFloat`` is featureless but
  phase P2. Phase values: ``P1`` (in the D7 P1 set: features subset of
  {mvp, sign-ext, nontrapping, wabt-free}, no runner deferral),
  ``P2`` (AsmJSFloat), ``P2-pending`` (exclude_x64), ``P3`` (needs threads /
  simd / multi-value / wabt -- committed completion scope), ``deferred``
  (require_debugger), ``excluded`` (non-wasm, fast-array enabled form --
  ``ENABLE_FAST_ARRAYBUFFER`` is Windows-only, so this is applicability,
  not a porting gap).
* Suite directories are resolved via ``git ls-files`` (git tracks
  ``ASMJSParser`` / ``AsmJSFloat`` while ``test/rlexedirs.xml`` spells
  ``AsmJsParser`` / ``AsmJsFloat``; never trust the rlexedirs spelling on
  a case-sensitive filesystem).

Output per entry: ``{test-id, suite, features[], runnerTags[], phase}``
where ``test-id`` is ``"<suite>[<index>]"`` with ``index`` the 0-based
ordinal of the entry in ``rlexe.xml`` document order (counts are rlexe
entries including variants, never files). Output is deterministic and
re-runnable: running twice yields a byte-identical file (no timestamps,
no absolute paths, canonical key/feature/tag ordering).

Regenerate::

    python3 tools/build/gen_wasm_manifest.py

then ``git diff`` the output: any rlexe drift shows up as a manifest change.

Checks (no file writes): ``--selftest`` (unit asserts), ``--check``
(fail if the checked-in manifest differs from a fresh build).
"""

import argparse
import json
import subprocess
import sys
import xml.etree.ElementTree as ET
from pathlib import Path

HERE = Path(__file__).resolve().parent
ROOT = HERE.parent.parent
DEFAULT_OUTPUT = ROOT / "test/wasm-arm64-manifest.json"

# Canonical suite order: manifest entry order and suite ordering in output.
# Dir names are the git-tracked spellings (see module docstring).
SUITES = [
    ("WasmSpec", "test/WasmSpec"),
    ("WasmSpec.MultiValue", "test/WasmSpec.MultiValue"),
    ("wasm", "test/wasm"),
    ("wasm.simd", "test/wasm.simd"),
    ("AsmJs", "test/AsmJs"),
    ("AsmJSFloat", "test/AsmJSFloat"),
    ("ASMJSParser", "test/ASMJSParser"),
]

# Canonical feature order for deterministic output.
FEATURE_ORDER = [
    "mvp",
    "sign-ext",
    "nontrapping",
    "threads",
    "simd",
    "multi-value",
    "fast-array",
    "wabt",
    "wabt-free",
    "non-wasm",
]

# Enabled-form token name (lowercased, leading dash stripped) -> feature.
TOKEN_FEATURES = {
    "wasmthreads": "threads",
    "essharedarraybuffer": "threads",
    "wasmsignextends": "sign-ext",
    "wasmnontrapping": "nontrapping",
    "wasmmultivalue": "multi-value",
    "wasmsimd": "simd",
    "wasmfastarray": "fast-array",
}

# Enabled-form token names that are recorded but add no feature.
TOKEN_FEATURELESS = {
    "wasm",                      # plain wasm mode marker; mvp assigned separately
    "wasmmathexfilter",          # Windows/x86-only code sites; inert here
    "wasmi64",                   # legacy i64 enable; i64 is MVP
    "wasmassignmoduleid",        # CSE trace helper
}

# Disabled-form (trailing dash) token bases recorded as featureless.
# The enabled form of wasmsharedarrayvirtualbuffer is intentionally NOT
# listed: it does not occur in the tree and would be an error until
# classified, rather than a guess.
DISABLED_FEATURELESS_BASES = {
    "wasmfastarray",
    "wasmsharedarrayvirtualbuffer",
}

NONATIVE_TOKEN = "nonative"


def tokenize(flags):
    """Split compile-flags on whitespace into tokens (never substring match)."""
    return flags.split() if flags else []


def split_token(token):
    """Return (base, disabled) for one token.

    Base is the lowercased token including its leading dash(es); disabled is
    True when the token carries the trailing-dash disabled form.
    """
    name = token.lower()
    if name.endswith("-") and len(name) > 1:
        return name[:-1], True
    return name, False


def canonical_features(features):
    """Sort features into canonical order for deterministic output."""
    ordered = [f for f in FEATURE_ORDER if f in features]
    ordered += sorted(features - set(FEATURE_ORDER))
    return ordered


def parse_runner_tags(tags_text):
    """Parse rlexe tag text (comma-separated) into a sorted unique list."""
    if not tags_text:
        return []
    return sorted({t.strip().lower() for t in tags_text.split(",") if t.strip()})


def token_features(tokens):
    """Feature set implied by compile-flag tokens, or raise on unknown tokens."""
    features = set()
    for token in tokens:
        name, disabled = split_token(token)
        if name == "--no-wabt":
            if disabled:
                raise ValueError(f"unknown disabled token: {token!r}")
            features.add("wabt-free")
            continue
        stripped = name[1:] if name.startswith("-") else name
        if stripped == NONATIVE_TOKEN:
            continue  # variant dimension, not a feature
        if disabled:
            if stripped in DISABLED_FEATURELESS_BASES:
                continue  # disabled form = generic-path control
            if stripped.startswith("wasm") or stripped == "essharedarraybuffer":
                raise ValueError(f"unknown disabled token: {token!r}")
            continue  # non-wasm disabled tokens (e.g. -oopjit-, -bgjit-) carry no feature
        if stripped in TOKEN_FEATURES:
            features.add(TOKEN_FEATURES[stripped])
            continue
        if stripped in TOKEN_FEATURELESS:
            continue
        if stripped.startswith("wasm") or stripped == "essharedarraybuffer":
            raise ValueError(f"unknown wasm token (classify explicitly): {token!r}")
        # Non-wasm tokens (e.g. -maic:0, -testtrace:asmjs, suite-local args)
        # carry no feature.
    return features


def path_features(suite, flags, baseline):
    """Feature set implied by suite and path rules (baseline + flags text)."""
    features = set()
    if suite == "wasm.simd":
        features.add("simd")
    if suite == "WasmSpec.MultiValue":
        features.add("multi-value")
    haystacks = " ".join(x for x in (flags, baseline) if x).lower()
    if "features/extends" in haystacks or "chakra_extends" in haystacks:
        features.add("sign-ext")
    if "features/nontrapping" in haystacks:
        features.add("nontrapping")
    if "features/threads" in haystacks or "chakra_atomic_" in haystacks:
        features.add("threads")
    return features


def has_wasm_token(tokens):
    """True when any token is a -wasm* token (case-insensitive)."""
    for token in tokens:
        name = token.lower()
        if name.startswith("-wasm"):
            return True
    return False


def is_p1_features(features):
    """True when features are within the P1 set {mvp, sign-ext, nontrapping, wabt-free}."""
    return set(features) <= {"mvp", "sign-ext", "nontrapping", "wabt-free"}


def assign_phase(suite, features, runner_tags):
    """Assign the phase axis (separate from features) per D7."""
    tags = set(runner_tags)
    if "require_debugger" in tags:
        return "deferred"
    if "exclude_x64" in tags:
        return "P2-pending"
    if suite == "AsmJSFloat":
        return "P2"
    feature_set = set(features)
    if "non-wasm" in feature_set or "fast-array" in feature_set:
        return "excluded"
    if is_p1_features(feature_set):
        return "P1"
    if feature_set & {"threads", "simd", "multi-value", "wabt"}:
        return "P3"
    return "excluded"


def resolve_suites():
    """Resolve suite dirs via git ls-files; fail if a spelling is wrong."""
    resolved = []
    for name, rel_dir in SUITES:
        rlexe_rel = f"{rel_dir}/rlexe.xml"
        proc = subprocess.run(
            ["git", "ls-files", "--", rlexe_rel],
            cwd=str(ROOT), capture_output=True, text=True,
        )
        tracked = [line.strip() for line in proc.stdout.splitlines() if line.strip()]
        if proc.returncode != 0 or tracked != [rlexe_rel]:
            raise RuntimeError(
                f"suite dir for {name} ({rel_dir}) not tracked as "
                f"{rlexe_rel} in git (case mismatch?): got {tracked!r}"
            )
        resolved.append((name, rel_dir))
    return resolved


def load_rlexe(rlexe_rel):
    """Parse one rlexe.xml into (files, flags, baseline, tags) per entry."""
    tree = ET.parse(ROOT / rlexe_rel)
    entries = []
    for test in tree.getroot().findall(".//test"):
        default = test.find("default")
        if default is None:
            raise RuntimeError(f"rlexe entry without <default> in {rlexe_rel}")
        entries.append({
            "files": (default.findtext("files") or "").strip(),
            "flags": (default.findtext("compile-flags") or "").strip(),
            "baseline": (default.findtext("baseline") or "").strip(),
            "tags": parse_runner_tags(default.findtext("tags")),
        })
    return entries


def classify_suite(suite, raw_entries):
    """Classify every rlexe entry; returns manifest entries (5 keys each)."""
    # Classify tokens/paths first so the wabt sibling rule sees full features.
    staged = []
    for index, raw in enumerate(raw_entries):
        tokens = tokenize(raw["flags"])
        staged.append({
            "index": index,
            "raw": raw,
            "tokens": tokens,
            "features": token_features(tokens) | path_features(
                suite, raw["flags"], raw["baseline"]),
        })

    # wabt sibling rule (test/wasm regress pair): --no-wabt present ->
    # wabt-free; the default sibling (same files + baseline) -> wabt.
    # Group only entries that opted into --no-wabt; other suites have none.
    if suite == "wasm":
        groups = {}
        for item in staged:
            key = (item["raw"]["files"], item["raw"]["baseline"])
            groups.setdefault(key, []).append(item)
        for group in groups.values():
            if any(
                "--no-wabt" in [t.lower() for t in item["tokens"]]
                for item in group
            ):
                for item in group:
                    if "--no-wabt" in [t.lower() for t in item["tokens"]]:
                        item["features"].add("wabt-free")
                        item["features"].discard("wabt")
                    else:
                        item["features"].add("wabt")

    entries = []
    for item in staged:
        raw, tokens, features = item["raw"], item["tokens"], set(item["features"])
        non_wasm_override = False
        if suite == "wasm":
            # Override 1: test/wasm/signextend.js -> sign-ext.
            if raw["files"] == "signextend.js":
                features.add("sign-ext")
            # Override 2: the 8 test/wasm entries with no -wasm token.
            if not has_wasm_token(tokens):
                features = {"non-wasm"}
                non_wasm_override = True
        if not non_wasm_override:
            features.add("mvp")
        phase = assign_phase(suite, canonical_features(features), raw["tags"])
        entries.append({
            "test-id": f"{suite}[{item['index']}]",
            "suite": suite,
            "features": canonical_features(features),
            "runnerTags": raw["tags"],
            "phase": phase,
        })
    return entries


def build_manifest():
    """Build the manifest dict from all suites."""
    suites_meta = {}
    all_entries = []
    for suite, rel_dir in resolve_suites():
        raw_entries = load_rlexe(f"{rel_dir}/rlexe.xml")
        entries = classify_suite(suite, raw_entries)
        phases = {}
        for entry in entries:
            phases[entry["phase"]] = phases.get(entry["phase"], 0) + 1
        suites_meta[suite] = {
            "rlexe": f"{rel_dir}/rlexe.xml",
            "entries": len(entries),
            "p1": phases.get("P1", 0),
            "phases": {k: phases[k] for k in sorted(phases)},
        }
        all_entries.extend(entries)
    return {
        "meta": {
            "generator": "tools/build/gen_wasm_manifest.py",
            "plan": "doc/wasm-arm64-plan.md D7",
            "counts": "rlexe entries (variants included), never files",
            "test-id": "<suite>[<0-based rlexe entry ordinal>]",
            "suites": suites_meta,
            "total": len(all_entries),
            "p1": sum(1 for e in all_entries if e["phase"] == "P1"),
        },
        "entries": all_entries,
    }


def manifest_bytes(manifest):
    return (json.dumps(manifest, indent=2, sort_keys=True) + "\n").encode("utf-8")


def check_overrides(entries):
    """Assert the exactly-two override groups and other D7 anchors."""
    wasm = [e for e in entries if e["suite"] == "wasm"]
    non_wasm = [e for e in wasm if e["features"] == ["non-wasm"]]
    if len(non_wasm) != 8:
        raise AssertionError(f"non-wasm override: expected 8, got {len(non_wasm)}")
    sign_ext = [e for e in wasm if e["features"] == ["mvp", "sign-ext"]]
    if len(sign_ext) != 1:
        raise AssertionError(
            f"signextend override: expected 1, got {len(sign_ext)}")
    wabt = [e for e in entries if "wabt" in e["features"]]
    wabt_free = [e for e in entries if "wabt-free" in e["features"]]
    if len(wabt) != 1 or len(wabt_free) != 1:
        raise AssertionError(
            f"wabt pairing: expected 1 wabt / 1 wabt-free, "
            f"got {len(wabt)} / {len(wabt_free)}")
    deferred = [e for e in entries if e["phase"] == "deferred"]
    if len(deferred) != 4:
        raise AssertionError(f"deferred: expected 4, got {len(deferred)}")
    pending = [e for e in entries if e["phase"] == "P2-pending"]
    # 28 at W0 + 1 W2 arm64_w2.js guardrail probe (exclude_x64: ARM64-only
    # until W3; retagged when the probe converges with x64 behavior).
    if len(pending) != 29:
        raise AssertionError(f"P2-pending: expected 29, got {len(pending)}")
    float_entries = [e for e in entries if e["suite"] == "AsmJSFloat"]
    if len(float_entries) != 14 or any(
            e["phase"] != "P2" for e in float_entries):
        raise AssertionError(
            "AsmJSFloat: expected 14 entries all phase P2")
    wasmspec = [e for e in entries if e["suite"] == "WasmSpec"]
    threads = [e for e in wasmspec if "threads" in e["features"]]
    if len(threads) != 12 or any(e["phase"] == "P1" for e in threads):
        raise AssertionError("WasmSpec threads: expected 12 all out of P1")
    if sum(1 for e in wasmspec if e["phase"] == "P1") != 219:
        raise AssertionError("WasmSpec P1 count != 219")


def print_counts(manifest):
    meta = manifest["meta"]
    print("P1-set entry counts (rlexe entries, variants included):")
    for suite, info in meta["suites"].items():
        print(f"  {suite}: {info['p1']} P1 of {info['entries']} "
              f"({info['phases']})")
    print(f"  total: {meta['p1']} P1 of {meta['total']}")
    anchors = [
        "threads excluded from P1 (WasmSpec 12 + wasm 2)",
        "lowercase -wasmthreads classified threads",
        "enabled fast-array excluded while disabled stays in P1",
        "mathEx featureless included in P1",
        "signextend.js classified sign-ext in P1",
    ]
    print("sanity anchors (asserted in check_overrides + main):")
    for anchor in anchors:
        print(f"  - {anchor}")


def run_anchor_checks(entries):
    """Detailed sanity anchors required by the task."""
    checks = []
    wasmspec_threads = [e for e in entries
                        if e["suite"] == "WasmSpec" and "threads" in e["features"]]
    checks.append(("WasmSpec threads excluded from P1",
                   len(wasmspec_threads) == 12 and all(
                       e["phase"] != "P1" for e in wasmspec_threads)))
    wasm_threads = [e for e in entries
                    if e["suite"] == "wasm" and "threads" in e["features"]]
    checks.append(("lowercase -wasmthreads classified threads",
                   len(wasm_threads) == 2 and all(
                       e["phase"] == "P3" for e in wasm_threads)))
    fast_excluded = [e for e in entries if "fast-array" in e["features"]]
    checks.append(("enabled fast-array excluded",
                   len(fast_excluded) == 1 and fast_excluded[0]["phase"] == "excluded"))
    dis_fast = [e for e in entries
                if e["suite"] == "wasm" and e["features"] == ["mvp"]
                and e["phase"] == "P1" and e["test-id"] in ("wasm[8]", "wasm[51]")]
    checks.append(("disabled fast-array stays in P1", len(dis_fast) == 2))
    mathex = [e for e in entries
              if e["suite"] == "WasmSpec" and e["features"] == ["mvp"]
              and e["phase"] == "P1"]
    # mathEx entries are a subset of the plain-mvp WasmSpec P1 entries;
    # the 6 mathexfilter entries are verified by token absence of extra
    # features plus the fixture-count assert in the script self-review.
    checks.append(("mathEx featureless included in P1", len(mathex) > 0))
    signext = [e for e in entries if e["test-id"] == "wasm[49]"]
    checks.append(("signextend.js sign-ext",
                   len(signext) == 1 and signext[0]["features"] == ["mvp", "sign-ext"]
                   and signext[0]["phase"] == "P1"))
    failed = [name for name, ok in checks if not ok]
    for name, ok in checks:
        print(f"  [{'ok' if ok else 'FAIL'}] {name}")
    if failed:
        raise AssertionError(f"sanity anchors failed: {failed}")
    # mathEx resolution: 6 WasmSpec entries carry -wasmmathexfilter; all six
    # must be plain-mvp P1 entries (no extra feature added).
    raw = load_rlexe("test/WasmSpec/rlexe.xml")
    mathex_indices = [i for i, r in enumerate(raw)
                      if "-wasmmathexfilter" in [t.lower() for t in tokenize(r["flags"])]]
    if len(mathex_indices) != 6:
        raise AssertionError(f"mathEx fixtures: expected 6, got {len(mathex_indices)}")
    for i in mathex_indices:
        entry = next(e for e in entries if e["test-id"] == f"WasmSpec[{i}]")
        if entry["features"] != ["mvp"] or entry["phase"] != "P1":
            raise AssertionError(f"mathEx entry {entry['test-id']} misclassified")
    print("  [ok] mathEx fixtures (6) plain-mvp in P1")


def run_selftest():
    """Unit checks for the classifier functions (no tree reads)."""
    n = 0

    def check(condition, message):
        nonlocal n
        n += 1
        if not condition:
            raise AssertionError(message)

    # Tokenization: whitespace split, case-insensitive, no substring match.
    check(tokenize("-wasm -args a.wast -endargs") == ["-wasm", "-args", "a.wast", "-endargs"],
          "tokenize splits on whitespace")
    check(tokenize("") == [], "empty flags -> no tokens")
    check(token_features(tokens_ := tokenize("-wasm -WasmThreads -ESSharedArrayBuffer"))
          == {"threads"}, f"canonical threads tokens: {tokens_!r}")
    check(token_features(tokenize("-wasmthreads")) == {"threads"},
          "lowercase -wasmthreads -> threads")
    check(token_features(tokenize("-WasmFastArray")) == {"fast-array"},
          "enabled fast-array")
    check(token_features(tokenize("-WasmFastArray-")) == set(),
          "disabled fast-array featureless")
    check(token_features(tokenize("-wasmfastarray-")) == set(),
          "lowercase disabled fast-array featureless")
    check(token_features(tokenize("-wasmMathExFilter")) == set(), "mathEx inert")
    check(token_features(tokenize("-wasmi64")) == set(), "wasmi64 inert")
    check(token_features(tokenize("-nonative")) == set(), "nonative inert")
    check(token_features(tokenize("-WasmSharedArrayVirtualBuffer-")) == set(),
          "disabled virtualbuffer featureless")
    check(token_features(tokenize("-WasmSignExtends")) == {"sign-ext"}, "sign-ext")
    check(token_features(tokenize("-wasmsignextends")) == {"sign-ext"},
          "lowercase sign-ext")
    check(token_features(tokenize("-WasmNontrapping")) == {"nontrapping"}, "nontrapping")
    check(token_features(tokenize("-wasmsimd")) == {"simd"}, "simd lowercase")
    check(token_features(tokenize("-WasmMultiValue")) == {"multi-value"}, "multi-value")
    check(token_features(tokenize("-args --no-wabt -endargs")) == {"wabt-free"}, "wabt-free")
    check(token_features(tokenize("-wasmassignmoduleid")) == set(), "moduleid inert")
    for bad in ("-wasmthreadsx", "-wasmunknown", "-WasmSharedArrayVirtualBuffer"):
        try:
            token_features(tokenize(bad))
        except ValueError:
            n += 1
        else:
            raise AssertionError(f"unknown token accepted: {bad}")
    check(has_wasm_token(tokenize("-wasmthreads")) is True, "threads has wasm token")
    check(has_wasm_token(tokenize("-forcejitloopbody")) is False, "loop has no wasm token")
    check(has_wasm_token([]) is False, "no flags has no wasm token")

    # Path and suite rules.
    check("sign-ext" in path_features(
        "WasmSpec", "-wasm", "baselines/features/extends/extends_i32.baseline"),
        "features/extends -> sign-ext")
    check("sign-ext" in path_features(
        "WasmSpec", "-wasm -args chakra_generated/chakra_extends_i32.wast",
        "baselines/chakra_generated/chakra_extends_i32.baseline"),
        "chakra_extends -> sign-ext")
    check("nontrapping" in path_features(
        "WasmSpec", "-wasm -args features/nontrapping/nontrapping_conversions.wast",
        "baselines/features/nontrapping/nontrapping_conversions.baseline"),
        "features/nontrapping -> nontrapping")
    check("threads" in path_features(
        "WasmSpec", "-wasm -args chakra/chakra_atomic_load.wast",
        "baselines/chakra/chakra_atomic_load.baseline"),
        "chakra_atomic_ -> threads")
    check("simd" in path_features("wasm.simd", "-wasm", ""), "wasm.simd -> simd")
    check("multi-value" in path_features("WasmSpec.MultiValue", "-wasm", ""),
          "MultiValue -> multi-value")

    # Runner tags and P1 predicate.
    check(parse_runner_tags("exclude_win7, require_debugger") ==
          ["exclude_win7", "require_debugger"], "tags parsed + sorted")
    check(parse_runner_tags(None) == [], "no tags")
    for feats, expected in [
        (["mvp"], True), (["mvp", "sign-ext"], True), (["mvp", "nontrapping"], True),
        (["mvp", "wabt-free"], True), (["mvp", "threads"], False),
        (["mvp", "simd"], False), (["mvp", "multi-value"], False),
        (["mvp", "wabt"], False), (["mvp", "fast-array"], False),
        (["non-wasm"], False),
    ]:
        check(is_p1_features(feats) is expected, f"P1 predicate {feats}={expected}")

    # Phase axis (separate from features).
    check(assign_phase("wasm", ["mvp"], ["require_debugger"]) == "deferred",
          "require_debugger deferred")
    check(assign_phase("wasm", ["mvp"], ["exclude_x64"]) == "P2-pending",
          "exclude_x64 P2-pending")
    check(assign_phase("AsmJSFloat", ["mvp"], []) == "P2", "AsmJSFloat P2")
    check(assign_phase("wasm", ["non-wasm"], []) == "excluded", "non-wasm excluded")
    check(assign_phase("wasm", ["mvp", "fast-array"], []) == "excluded",
          "fast-array excluded")
    check(assign_phase("wasm", ["mvp"], []) == "P1", "mvp P1")
    check(assign_phase("wasm", ["mvp", "threads"], []) == "P3", "threads P3")
    check(assign_phase("wasm", ["mvp", "simd"], []) == "P3", "simd P3")
    check(assign_phase("WasmSpec.MultiValue", ["mvp", "multi-value"], []) == "P3",
          "multi-value P3")
    check(assign_phase("wasm", ["mvp", "wabt"], []) == "P3", "wabt P3")
    check(canonical_features({"non-wasm", "mvp"}) == ["mvp", "non-wasm"],
          "canonical feature order")
    check(set(parse_runner_tags("Slow,exclude_x64")) == {"slow", "exclude_x64"},
          "tags lowercased")

    print(f"selftest OK ({n} checks)")
    return 0


def main(argv):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output", default=str(DEFAULT_OUTPUT),
                        help="manifest output path (default: test/wasm-arm64-manifest.json)")
    parser.add_argument("--check", action="store_true",
                        help="fail if the checked-in manifest differs from a fresh build")
    parser.add_argument("--selftest", action="store_true",
                        help="run classifier unit asserts only")
    args = parser.parse_args(argv)
    if args.selftest:
        return run_selftest()
    manifest = build_manifest()
    check_overrides(manifest["entries"])
    payload = manifest_bytes(manifest)
    # Determinism: a second in-memory build must be byte-identical.
    if manifest_bytes(build_manifest()) != payload:
        raise AssertionError("generator is not deterministic")
    output = Path(args.output)
    if args.check:
        if not output.is_file() or output.read_bytes() != payload:
            print(f"stale: {output} differs from a fresh build", file=sys.stderr)
            return 1
        print(f"fresh: {output}")
    else:
        output.write_bytes(payload)
        print(f"wrote {output} ({len(manifest['entries'])} entries)")
    print_counts(manifest)
    run_anchor_checks(manifest["entries"])
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
