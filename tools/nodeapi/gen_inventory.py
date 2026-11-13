#!/usr/bin/env python3
"""Generate the normative Node-API inventory from vendored headers.

Parses every ``NAPI_EXTERN`` declaration whose name matches
``(napi_|node_api_)*`` in the vendored headers under ``include/nodeapi/``
(excluding the ``NAPI_EXTERN`` macro definitions themselves), filters to
``NAPI_VERSION=8`` visibility (excluding ``#ifdef NAPI_EXPERIMENTAL``
and ``#if NAPI_VERSION >= 9/10`` blocks), and writes
``doc/node-api-inventory.md``.

Plan: doc/node-api-plan.md N0.1. Expected counts re-derived here:
115 symbols from ``js_native_api.h`` + 30 from ``node_api.h`` = 145 at v8.

Usage:
    python3 tools/nodeapi/gen_inventory.py          # regenerate inventory
    python3 tools/nodeapi/gen_inventory.py --check  # verify (exit 1 on drift)

``--check`` exits 1 if the generated inventory differs from
``doc/node-api-inventory.md`` or if the headers on disk no longer match
the SHA-256 values recorded in ``include/nodeapi/MANIFEST.md``.
It never modifies files.
"""

import argparse
import bisect
import hashlib
import re
import sys
from pathlib import Path

# --- Group mapping from plan §4 Tables A/B (verbatim) ---------------------
# Maps symbol -> group label. ``napi_is_error`` is listed in plan Tables A2
# and A4 (plan notes "also A2"); it is emitted once with "A2, A4".
GROUP_MAP = {
    # A1 Values
    "napi_get_undefined": "A1",
    "napi_get_null": "A1",
    "napi_get_global": "A1",
    "napi_get_boolean": "A1",
    "napi_create_double": "A1",
    "napi_create_int32": "A1",
    "napi_create_uint32": "A1",
    "napi_create_int64": "A1",
    "napi_get_value_bool": "A1",
    "napi_get_value_double": "A1",
    "napi_get_value_int32": "A1",
    "napi_get_value_int64": "A1",
    "napi_get_value_uint32": "A1",
    "napi_create_string_utf8": "A1",
    "napi_create_string_utf16": "A1",
    "napi_create_string_latin1": "A1",
    "napi_get_value_string_utf8": "A1",
    "napi_get_value_string_utf16": "A1",
    "napi_get_value_string_latin1": "A1",
    "napi_create_symbol": "A1",
    "napi_create_date": "A1",
    "napi_get_date_value": "A1",
    "napi_create_bigint_int64": "A1",
    "napi_create_bigint_uint64": "A1",
    "napi_create_bigint_words": "A1",
    "napi_get_value_bigint_int64": "A1",
    "napi_get_value_bigint_uint64": "A1",
    "napi_get_value_bigint_words": "A1",
    # A2 Objects/functions (napi_is_error also in A4)
    "napi_create_object": "A2",
    "napi_create_array": "A2",
    "napi_create_array_with_length": "A2",
    "napi_get_array_length": "A2",
    "napi_get_property_names": "A2",
    "napi_get_all_property_names": "A2",
    "napi_get_property": "A2",
    "napi_set_property": "A2",
    "napi_has_property": "A2",
    "napi_delete_property": "A2",
    "napi_has_own_property": "A2",
    "napi_get_named_property": "A2",
    "napi_set_named_property": "A2",
    "napi_has_named_property": "A2",
    "napi_get_element": "A2",
    "napi_set_element": "A2",
    "napi_has_element": "A2",
    "napi_delete_element": "A2",
    "napi_define_properties": "A2",
    "napi_get_prototype": "A2",
    "napi_object_freeze": "A2",
    "napi_object_seal": "A2",
    "napi_create_function": "A2",
    "napi_get_cb_info": "A2",
    "napi_get_new_target": "A2",
    "napi_new_instance": "A2",
    "napi_call_function": "A2",
    "napi_define_class": "A2",
    "napi_instanceof": "A2",
    "napi_typeof": "A2",
    "napi_is_array": "A2",
    "napi_is_date": "A2",
    "napi_is_error": "A2, A4",
    "napi_strict_equals": "A2",
    "napi_coerce_to_bool": "A2",
    "napi_coerce_to_number": "A2",
    "napi_coerce_to_object": "A2",
    "napi_coerce_to_string": "A2",
    # A3 Lifetime
    "napi_open_handle_scope": "A3",
    "napi_close_handle_scope": "A3",
    "napi_open_escapable_handle_scope": "A3",
    "napi_close_escapable_handle_scope": "A3",
    "napi_escape_handle": "A3",
    "napi_create_reference": "A3",
    "napi_delete_reference": "A3",
    "napi_reference_ref": "A3",
    "napi_reference_unref": "A3",
    "napi_get_reference_value": "A3",
    "napi_create_external": "A3",
    "napi_get_value_external": "A3",
    "napi_wrap": "A3",
    "napi_unwrap": "A3",
    "napi_remove_wrap": "A3",
    "napi_add_finalizer": "A3",
    "napi_type_tag_object": "A3",
    "napi_check_object_type_tag": "A3",
    "napi_set_instance_data": "A3",
    "napi_get_instance_data": "A3",
    # A4 Errors
    "napi_get_last_error_info": "A4",
    "napi_throw": "A4",
    "napi_throw_error": "A4",
    "napi_throw_type_error": "A4",
    "napi_throw_range_error": "A4",
    "napi_create_error": "A4",
    "napi_create_type_error": "A4",
    "napi_create_range_error": "A4",
    "napi_get_and_clear_last_exception": "A4",
    "napi_is_exception_pending": "A4",
    # A5 Binary
    "napi_create_arraybuffer": "A5",
    "napi_create_external_arraybuffer": "A5",
    "napi_get_arraybuffer_info": "A5",
    "napi_detach_arraybuffer": "A5",
    "napi_is_detached_arraybuffer": "A5",
    "napi_create_typedarray": "A5",
    "napi_get_typedarray_info": "A5",
    "napi_create_dataview": "A5",
    "napi_get_dataview_info": "A5",
    "napi_is_arraybuffer": "A5",
    "napi_is_typedarray": "A5",
    "napi_is_dataview": "A5",
    # A6 Script/promise/version
    "napi_create_promise": "A6",
    "napi_is_promise": "A6",
    "napi_resolve_deferred": "A6",
    "napi_reject_deferred": "A6",
    "napi_run_script": "A6",
    "napi_get_version": "A6",
    "napi_adjust_external_memory": "A6",
    # B1 Registration/fatal
    "napi_module_register": "B1",
    "napi_fatal_error": "B1",
    "napi_fatal_exception": "B1",
    # B2 Buffer
    "napi_create_buffer": "B2",
    "napi_create_external_buffer": "B2",
    "napi_create_buffer_copy": "B2",
    "napi_is_buffer": "B2",
    "napi_get_buffer_info": "B2",
    # B3 Async context
    "napi_async_init": "B3",
    "napi_async_destroy": "B3",
    "napi_make_callback": "B3",
    "napi_open_callback_scope": "B3",
    "napi_close_callback_scope": "B3",
    # B4 Async work + uv
    "napi_create_async_work": "B4",
    "napi_queue_async_work": "B4",
    "napi_cancel_async_work": "B4",
    "napi_delete_async_work": "B4",
    "napi_get_uv_event_loop": "B4",
    # B5 Thread-safe functions
    "napi_create_threadsafe_function": "B5",
    "napi_get_threadsafe_function_context": "B5",
    "napi_call_threadsafe_function": "B5",
    "napi_acquire_threadsafe_function": "B5",
    "napi_release_threadsafe_function": "B5",
    "napi_ref_threadsafe_function": "B5",
    "napi_unref_threadsafe_function": "B5",
    # B6 Cleanup hooks
    "napi_add_env_cleanup_hook": "B6",
    "napi_remove_env_cleanup_hook": "B6",
    "napi_add_async_cleanup_hook": "B6",
    "napi_remove_async_cleanup_hook": "B6",
    # B7 Node version
    "napi_get_node_version": "B7",
}

HEADER_FILES = ["js_native_api.h", "js_native_api_types.h", "node_api.h", "node_api_types.h"]
# Only these two headers declare functions; types headers are scanned to prove
# they contribute 0 declarations (and to keep "every header" literal).
API_HEADERS = ["js_native_api.h", "node_api.h"]

# Plan expectation (§1.1): 115 + 30 = 145 at NAPI_VERSION=8.
EXPECTED_JS = 115
EXPECTED_NODE = 30

# Macros considered defined when evaluating visibility at NAPI_VERSION=8
# with NAPI_EXPERIMENTAL undefined and NODE_API_NO_EXTERNAL_BUFFERS_ALLOWED
# undefined (provider never defines either; see plan §1.1/PR-2).
DEFINED_TRUE = {"NAPI_VERSION", "NAPI_VERSION_EXPERIMENTAL"}
NAPI_VERSION = 8

_DECL_RE = re.compile(
    r"NAPI_EXTERN\s+(?:NAPI_NO_RETURN\s+)?[\w\*]+\s+NAPI_CDECL\s+"
    r"((?:napi_|node_api_)[A-Za-z0-9_]+)"
)
_SHA_RE = re.compile(r"\b([0-9a-fA-F]{64})\b")


def _eval_ifdef(macro: str) -> bool:
    return macro in DEFINED_TRUE


def _eval_if_expr(expr: str) -> bool:
    """Evaluate a ``#if`` expression at NAPI_VERSION=8.

    Unknown identifiers evaluate to 0. If evaluation fails, default to
    True (visible) so an unrecognised guard never silently drops a
    symbol — the symbol set then differs loudly instead of silently.
    """
    e = expr.strip()
    # defined(X) and defined X forms.
    e = re.sub(r"defined\s*\(\s*([A-Za-z_][A-Za-z0-9_]*)\s*\)",
               lambda m: "True" if m.group(1) in DEFINED_TRUE else "False", e)
    e = re.sub(r"defined\s+([A-Za-z_][A-Za-z0-9_]*)",
               lambda m: "True" if m.group(1) in DEFINED_TRUE else "False", e)
    # NAPI_VERSION substitutions.
    e = re.sub(r"\bNAPI_VERSION_EXPERIMENTAL\b", "2147483647", e)
    e = re.sub(r"\bNAPI_VERSION\b", str(NAPI_VERSION), e)
    # Remaining bare identifiers -> 0.
    e = re.sub(r"\b[A-Za-z_][A-Za-z0-9_]*\b", "0", e)
    e = e.replace("&&", " and ").replace("||", " or ")
    # C ! -> Python not (but keep != intact).
    e = re.sub(r"!(\s*!=)", r"__NE__\1", e)
    e = e.replace("!", " not ")
    e = e.replace("__NE__", "!")
    try:
        return bool(eval(e, {"__builtins__": {}}, {}))
    except Exception:
        return True


def _eval_condition(line: str) -> bool:
    """Evaluate a single ``#if``/``#ifdef``/``#ifndef`` directive line."""
    s = line.strip()
    if s.startswith("#ifdef"):
        macro = s[len("#ifdef"):].strip().split()[0]
        return _eval_ifdef(macro)
    if s.startswith("#ifndef"):
        macro = s[len("#ifndef"):].strip().split()[0]
        return not _eval_ifdef(macro)
    if s.startswith("#if"):
        rest = s[len("#if"):].strip()
        # "#ifndef" handled above; plain "#if" / "#elif" land here.
        return _eval_if_expr(rest)
    return True


def _reason_for(stack) -> str:
    """Human reason for an excluded declaration from its condition stack."""
    for frame in reversed(stack):
        cond = frame["condition"]
        if "NAPI_EXPERIMENTAL" in cond:
            return f"{cond}"
        m = re.search(r"NAPI_VERSION\s*>=\s*(\d+)", cond)
        if m and int(m.group(1)) > NAPI_VERSION:
            return f"v{m.group(1)}-only ({cond})"
    # Fallback: report the inactive condition.
    for frame in reversed(stack):
        if not frame["active"]:
            return frame["condition"]
    return "excluded (other)"


def parse_header(path: Path):
    """Return (all_decls, visible, excluded) for one header file.

    Each entry is (name, header, lineno, reason) where reason is ""
    for visible symbols.
    """
    text = path.read_text()
    lines = text.splitlines()
    header = path.name

    # Precompute active-stack snapshot per line number (1-based).
    stack_per_line = {}
    frames = []  # each: {condition, parent_active, active, seen_true}
    for idx, line in enumerate(lines, 1):
        s = line.strip()
        if s.startswith("#if") or s.startswith("#ifdef") or s.startswith("#ifndef"):
            parent_active = frames[-1]["active"] if frames else True
            try:
                val = _eval_condition(s)
            except Exception:
                val = True
            frames.append({
                "condition": s,
                "parent_active": parent_active,
                "active": parent_active and val,
                "seen_true": val,
            })
            stack_per_line[idx] = [f["condition"] for f in frames], [f["active"] for f in frames]
        elif s.startswith("#elif"):
            # elif condition is inactive unless no prior branch was taken.
            if frames:
                f = frames[-1]
                try:
                    val = _eval_condition(s)
                except Exception:
                    val = True
                f["active"] = f["parent_active"] and (not f["seen_true"]) and val
                f["seen_true"] = f["seen_true"] or val
                f["condition"] = s
            stack_per_line[idx] = [f["condition"] for f in frames], [f["active"] for f in frames]
        elif s.startswith("#else"):
            if frames:
                f = frames[-1]
                f["active"] = f["parent_active"] and (not f["seen_true"])
                f["seen_true"] = True
                f["condition"] = s + " (else)"
            stack_per_line[idx] = [f["condition"] for f in frames], [f["active"] for f in frames]
        elif s.startswith("#endif"):
            if frames:
                frames.pop()
            stack_per_line[idx] = [f["condition"] for f in frames], [f["active"] for f in frames]
        else:
            stack_per_line[idx] = [f["condition"] for f in frames], [f["active"] for f in frames]

    offsets = []
    pos = 0
    for line in lines:
        offsets.append(pos)
        pos += len(line) + 1

    all_decls = []
    visible = []
    excluded = []
    seen = set()
    for m in _DECL_RE.finditer(text):
        name = m.group(1)
        lineno = bisect.bisect_right(offsets, m.start())
        conds, actives = stack_per_line.get(lineno, ([], []))
        is_visible = all(actives)
        if name in seen:
            print(f"warning: duplicate declaration {name} in {header}", file=sys.stderr)
            continue
        seen.add(name)
        stack = [{"condition": c, "active": a} for c, a in zip(conds, actives)]
        reason = "" if is_visible else _reason_for(stack)
        all_decls.append((name, header, lineno, reason))
        if is_visible:
            visible.append((name, header, lineno))
        else:
            excluded.append((name, header, lineno, reason))
    return all_decls, visible, excluded


def render_inventory(visible, excluded, totals):
    """Render the full inventory markdown (deterministic, no timestamps)."""
    # Sort visible by symbol name for determinism.
    visible_sorted = sorted(visible, key=lambda t: t[0])
    excluded_sorted = sorted(excluded, key=lambda t: t[0])

    js_total, js_visible, js_excl = totals["js_native_api.h"]
    node_total, node_visible, node_excl = totals["node_api.h"]

    # Excluded breakdown.
    def breakdown(items):
        exp = sum(1 for _, _, _, r in items if "NAPI_EXPERIMENTAL" in r)
        v9 = sum(1 for _, _, _, r in items if "v9-only" in r)
        v10 = sum(1 for _, _, _, r in items if "v10-only" in r)
        other = len(items) - exp - v9 - v10
        parts = []
        if exp:
            parts.append(f"{exp} experimental")
        if v9:
            parts.append(f"{v9} v9-only")
        if v10:
            parts.append(f"{v10} v10-only")
        if other:
            parts.append(f"{other} other")
        return ", ".join(parts) if parts else "none"

    out = []
    out.append("# Node-API inventory (generated)")
    out.append("")
    out.append("Generated from the vendored Node 22.22.3 headers in")
    out.append("`include/nodeapi/` at `NAPI_VERSION=8` by")
    out.append("`tools/nodeapi/gen_inventory.py`. Do not edit by hand —")
    out.append("regenerate with `python3 tools/nodeapi/gen_inventory.py`.")
    out.append("Normative source: `doc/node-api-plan.md` §4 (plan §1.1 counts).")
    out.append("")
    out.append("## Summary")
    out.append("")
    out.append(f"- `js_native_api.h`: {js_visible} symbols visible at "
               f"`NAPI_VERSION=8` ({js_total} total declarations; "
               f"{len(js_excl)} excluded: {breakdown(js_excl)}).")
    out.append(f"- `node_api.h`: {node_visible} symbols visible at "
               f"`NAPI_VERSION=8` ({node_total} total declarations; "
               f"{len(node_excl)} excluded: {breakdown(node_excl)}).")
    out.append(f"- Provider target at v8: **{js_visible + node_visible} symbols** "
               f"(expected {EXPECTED_JS} + {EXPECTED_NODE} = "
               f"{EXPECTED_JS + EXPECTED_NODE}).")
    out.append("")
    out.append("Visibility rule: a declaration is listed iff it is outside "
               "`NAPI_EXPERIMENTAL` blocks and outside `#if NAPI_VERSION >= 9/10` "
               "blocks, with `NAPI_EXPERIMENTAL` undefined and "
               "`NODE_API_NO_EXTERNAL_BUFFERS_ALLOWED` undefined "
               "(the provider never defines it, so "
               "`napi_create_external_arraybuffer` and "
               "`napi_create_external_buffer` are included). "
               "`#if NAPI_VERSION >= 2/3/4/5/6/7/8` blocks are included at v8.")
    out.append("")
    out.append(f"## Inventory ({len(visible_sorted)} symbols at `NAPI_VERSION=8`)")
    out.append("")
    out.append("| Symbol | Header | Group |")
    out.append("|---|---|---|")
    for name, header, _lineno in visible_sorted:
        group = GROUP_MAP.get(name, "?unmapped?")
        out.append(f"| `{name}` | `{header}` | {group} |")
    out.append("")
    out.append("## Excluded at `NAPI_VERSION=8` (for audit)")
    out.append("")
    out.append("| Symbol | Header | Reason |")
    out.append("|---|---|---|")
    for name, header, _lineno, reason in excluded_sorted:
        out.append(f"| `{name}` | `{header}` | {reason} |")
    out.append("")
    return "\n".join(out)


def sha256_of(path: Path) -> str:
    h = hashlib.sha256()
    h.update(path.read_bytes())
    return h.hexdigest()


def verify_manifest(root: Path) -> list:
    """Verify header SHA-256 values against MANIFEST.md. Return errors."""
    manifest = root / "include" / "nodeapi" / "MANIFEST.md"
    errors = []
    if not manifest.exists():
        return [f"missing manifest: {manifest}"]
    text = manifest.read_text()
    # Parse the manifest table: rows containing a header filename.
    # Each row has the filename and its SHA-256 in separate `|` cells.
    for fname in HEADER_FILES:
        path = root / "include" / "nodeapi" / fname
        if not path.exists():
            errors.append(f"missing vendored header: {path}")
            continue
        actual = sha256_of(path)
        # Find table rows mentioning fname.
        recorded = None
        for line in text.splitlines():
            if "|" in line and fname in line:
                m = _SHA_RE.search(line)
                if m:
                    recorded = m.group(1).lower()
                    break
        if recorded is None:
            # Fallback: any 64-hex string near the filename in the file.
            idx = text.find(fname)
            window = text[idx:idx + 2000] if idx >= 0 else ""
            m = _SHA_RE.search(window)
            if m:
                recorded = m.group(1).lower()
        if recorded is None:
            errors.append(f"{fname}: no SHA-256 found in MANIFEST.md")
        elif recorded != actual:
            errors.append(
                f"{fname}: SHA-256 mismatch (manifest {recorded} != header {actual})")
    return errors


def collect(root: Path):
    headers_dir = root / "include" / "nodeapi"
    all_visible = []
    all_excluded = []
    totals = {}
    for fname in API_HEADERS:
        path = headers_dir / fname
        if not path.exists():
            print(f"error: missing vendored header: {path}", file=sys.stderr)
            sys.exit(1)
        all_decls, visible, excluded = parse_header(path)
        totals[fname] = (len(all_decls), len(visible), excluded)
        all_visible.extend(visible)
        all_excluded.extend(excluded)
    # Types headers must contribute zero declarations.
    for fname in ["js_native_api_types.h", "node_api_types.h"]:
        path = headers_dir / fname
        if not path.exists():
            print(f"error: missing vendored header: {path}", file=sys.stderr)
            sys.exit(1)
        all_decls, visible, excluded = parse_header(path)
        if all_decls:
            print(f"warning: {fname} unexpectedly declares "
                  f"{len(all_decls)} symbols: {[n for n, _, _, _ in all_decls]}",
                  file=sys.stderr)
        totals[fname] = (len(all_decls), len(visible), excluded)
    return all_visible, all_excluded, totals


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--check", action="store_true",
                    help="exit 1 if inventory or manifest is stale (no writes)")
    ap.add_argument("--root", type=Path, default=None,
                    help="repo root (default: two levels above this script)")
    args = ap.parse_args()

    if args.root is not None:
        root = args.root.resolve()
    else:
        root = Path(__file__).resolve().parent.parent.parent

    visible, excluded, totals = collect(root)

    js_visible = totals["js_native_api.h"][1]
    node_visible = totals["node_api.h"][1]
    print(f"js_native_api.h: {js_visible} visible at NAPI_VERSION=8 "
          f"({totals['js_native_api.h'][0]} total, "
          f"{len(totals['js_native_api.h'][2])} excluded)")
    print(f"node_api.h: {node_visible} visible at NAPI_VERSION=8 "
          f"({totals['node_api.h'][0]} total, "
          f"{len(totals['node_api.h'][2])} excluded)")
    print(f"total: {len(visible)} symbols "
          f"(expected {EXPECTED_JS}+{EXPECTED_NODE}={EXPECTED_JS+EXPECTED_NODE})")
    if (js_visible, node_visible) != (EXPECTED_JS, EXPECTED_NODE):
        print(f"WARNING: counts {js_visible}+{node_visible} differ from "
              f"plan §1.1 expectation {EXPECTED_JS}+{EXPECTED_NODE}; "
              f"investigate, do not adjust the expectation to fit.",
              file=sys.stderr)

    unmapped = [n for n, _, _ in visible if n not in GROUP_MAP]
    if unmapped:
        print(f"WARNING: symbols without a §4 group: {sorted(unmapped)}",
              file=sys.stderr)

    content = render_inventory(visible, excluded, totals)
    out_path = root / "doc" / "node-api-inventory.md"

    if args.check:
        errors = []
        errors.extend(verify_manifest(root))
        if not out_path.exists():
            errors.append(f"missing generated file: {out_path}")
        elif out_path.read_text() != content:
            errors.append(f"generated file differs: {out_path} "
                          f"(regenerate with `python3 tools/nodeapi/gen_inventory.py`)")
        for e in errors:
            print(f"error: {e}", file=sys.stderr)
        if errors:
            return 1
        print("check ok: inventory and manifest match headers")
        return 0

    out_path.parent.mkdir(parents=True, exist_ok=True)
    out_path.write_text(content)
    print(f"wrote {out_path}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
