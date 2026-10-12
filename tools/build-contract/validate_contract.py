#!/usr/bin/env python3
#-------------------------------------------------------------------------------------------------------
# Copyright (c) ChakraCore Project Contributors. All rights reserved.
# Licensed under the MIT license. See LICENSE.txt file in the project root for full license information.
#-------------------------------------------------------------------------------------------------------
"""Validate CMake-enumerated sources for the Mac ARM64 shared/no-JIT
RelWithDebInfo + ICU-include-path profile against a compile database and
the Zig source tables. Any missing, extra or nonexistent source fails.

Tiny evaluator for the simple if/elseif/else/endif conditionals in the .txt files.
Usage: validate_contract.py [--database PATH] [--tables PATH]
"""
import argparse
import json
import os
import re
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
# Profile variable values (truthiness). *_SH vars are unset/absent in direct-CMake configure.
VARS = {
    'CC_TARGETS_ARM64': True, 'CC_TARGETS_AMD64': False, 'CC_TARGETS_ARM': False,
    'CC_TARGETS_X86': False, 'CC_TARGET_OS_OSX': True, 'CC_TARGET_OS_LINUX': False,
    'CC_TARGET_OS_ANDROID': False, 'BuildJIT': False, 'CAN_BUILD_WABT': False,
    'USE_LTTNG': False, 'STATIC_LIBRARY': False, 'NO_ICU': False, 'EMBED_ICU': False,
    'ICU_INCLUDE_PATH': True, 'CC_XCODE_PROJECT': False,
    # pal/src/CMakeLists.txt:20-24 sets this when CMAKE_SYSTEM_PROCESSOR is arm64/aarch64
    'PAL_CMAKE_PLATFORM_ARCH_ARM64': True,
}

def eval_cond(expr):
    expr = expr.strip()
    if expr.startswith('(') and expr.endswith(')'):
        expr = expr[1:-1].strip()
    # Split top-level OR / AND
    for op in (' OR ', ' AND '):
        if op in expr:
            parts = expr.split(op)
            vals = [eval_cond(p) for p in parts]
            return any(vals) if 'OR' in op else all(vals)
    if expr.startswith('NOT '):
        return not eval_cond(expr[4:])
    m = re.match(r'CMAKE_SYSTEM_PROCESSOR\s+STREQUAL\s+(\w+)', expr)
    if m:
        # root CMakeLists.txt:78 sets CMAKE_SYSTEM_PROCESSOR=arm64 for this profile
        return m.group(1) == 'arm64'
    m = re.match(r'(\w+)\s+STREQUAL\s+(.+)', expr)
    if m:
        return False  # no other STREQUAL conditions affect this profile's file lists
    tok = expr.strip().strip('"')
    if tok in ('1', 'ON', 'TRUE'):
        return True
    if tok in ('0', 'OFF', 'FALSE', ''):
        return False
    return bool(VARS.get(tok, False))

FILETOK = re.compile(r'[\w][\w\-./]*\.(?:cpp|c|S|gen)\b')

def parse_sources(path):
    """Return active files in listing order, honoring conditionals."""
    with open(path) as f:
        text = f.read()
    # strip full-line and trailing comments (no '#' inside strings in these files)
    lines = [l.split('#', 1)[0] for l in text.splitlines()]
    lines = [l for l in lines if l.strip()]
    # join into commands; split on newlines is fine for these files
    stack = [True]  # active levels; handle if/elseif/else/endif
    taken = []      # whether a branch was taken at each if-level
    files = []
    for raw in lines:
        l = raw.strip()
        m = re.match(r'if\s*\((.*)\)\s*$', l)
        if m:
            cond = eval_cond(m.group(1)) and stack[-1]
            stack.append(cond)
            taken.append(cond)
            continue
        m = re.match(r'elseif\s*\((.*)\)\s*$', l)
        if m:
            parent = stack[-2]
            cond = (not taken[-1]) and eval_cond(m.group(1)) and parent
            stack[-1] = cond
            taken[-1] = taken[-1] or cond
            continue
        if re.match(r'else\s*\(\s*\)\s*$', l):
            parent = stack[-2]
            cond = (not taken[-1]) and parent
            stack[-1] = cond
            taken[-1] = True
            continue
        if re.match(r'endif\s*\(', l):
            stack.pop()
            taken.pop()
            continue
        if stack[-1]:
            files.extend(FILETOK.findall(l))
    return files

GROUPS = {
    'pal/src': 'pal/src/CMakeLists.txt',
    'lib/Common/Codex': 'lib/Common/Codex/CMakeLists.txt',
    'lib/Common/Common': 'lib/Common/Common/CMakeLists.txt',
    'lib/Common/Core': 'lib/Common/Core/CMakeLists.txt',
    'lib/Common/DataStructures': 'lib/Common/DataStructures/CMakeLists.txt',
    'lib/Common/Exceptions': 'lib/Common/Exceptions/CMakeLists.txt',
    'lib/Common/Memory': 'lib/Common/Memory/CMakeLists.txt',
    'lib/Common/Util': 'lib/Common/Util/CMakeLists.txt',
    'lib/Jsrt': 'lib/Jsrt/CMakeLists.txt',
    'lib/Jsrt/Core': 'lib/Jsrt/Core/CMakeLists.txt',
    'lib/Parser': 'lib/Parser/CMakeLists.txt',
    'lib/SCACore': 'lib/SCACore/CMakeLists.txt',
    'lib/Runtime/Base': 'lib/Runtime/Base/CMakeLists.txt',
    'lib/Runtime/ByteCode': 'lib/Runtime/ByteCode/CMakeLists.txt',
    'lib/Runtime/Debug': 'lib/Runtime/Debug/CMakeLists.txt',
    'lib/Runtime/Language': 'lib/Runtime/Language/CMakeLists.txt',
    'lib/Runtime/Library': 'lib/Runtime/Library/CMakeLists.txt',
    'lib/Runtime/Math': 'lib/Runtime/Math/CMakeLists.txt',
    'lib/Runtime/Types': 'lib/Runtime/Types/CMakeLists.txt',
    'lib/Runtime/PlatformAgnostic/Platform': 'lib/Runtime/PlatformAgnostic/Platform/CMakeLists.txt',
    'bin/ch': 'bin/ch/CMakeLists.txt',
    'bin/ChakraCore': 'bin/ChakraCore/CMakeLists.txt',
    'bin/GCStress': 'bin/GCStress/CMakeLists.txt',
}

def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--database', default=os.path.join(ROOT, 'build-zigpilot/compile_commands.json'))
    parser.add_argument('--tables', default=os.path.join(ROOT, 'tools/build/tables.zig'))
    args = parser.parse_args(argv)
    try:
        with open(args.database) as f:
            cc = json.load(f)
        db_files = set(os.path.normpath(os.path.join(u.get('directory', ROOT), u['file'])) for u in cc)
        with open(args.tables) as f:
            table_text = f.read()
    except (OSError, ValueError, KeyError, TypeError) as e:
        print('validate_contract: FAIL: %s' % e, file=sys.stderr)
        return 1

    expected = {}
    counts = {}
    for gdir, txt in GROUPS.items():
        toks = [t for t in parse_sources(os.path.join(ROOT, txt)) if not t.endswith('.gen')]
        counts[gdir] = len(toks)
        # TestHooks.cpp is static-only in ch; shared membership comes from the dylib.
        for t in toks:
            ap = os.path.normpath(os.path.join(ROOT, gdir, t))
            expected.setdefault(ap, []).append(f'{txt}:{t}')
    expected[os.path.join(ROOT, 'lib/ChakraCoreStatic.cpp')] = ['lib/CMakeLists.txt:22']
    counts['lib(ChakraCoreStatic.cpp)'] = 1

    # These explicit tables contain quoted repo-relative source paths. Compare
    # the union: TestHooks.cpp intentionally appears in both profile choices.
    table_files = set(os.path.normpath(os.path.join(ROOT, src)) for src in
                      re.findall(r'"((?:pal|lib|bin)/[^"\n]+\.(?:cpp|c|S))"', table_text))
    missing = sorted(set(expected) - db_files)
    extra = sorted(db_files - set(expected))
    nonexistent = sorted(f for f in expected if not os.path.exists(f))
    table_missing = sorted(set(expected) - table_files)
    table_extra = sorted(table_files - set(expected))

    print(f'expected={len(expected)} db={len(db_files)} zig={len(table_files)}')
    for label, files in [('MISSING from DB', missing), ('EXTRA in DB', extra),
                         ('NONEXISTENT on disk', nonexistent),
                         ('MISSING from Zig tables', table_missing),
                         ('EXTRA in Zig tables', table_extra)]:
        print(f'{label} ({len(files)}):')
        for path in files:
            print('  ', path.replace(ROOT + '/', ''))
    print('\nper-group counts:')
    for group, count in counts.items():
        print(f'  {group}: {count}')
    failed = any((missing, extra, nonexistent, table_missing, table_extra))
    print('validate_contract: ' + ('FAIL' if failed else 'PASS'))
    return 1 if failed else 0


if __name__ == '__main__':
    sys.exit(main())
