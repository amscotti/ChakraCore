#!/usr/bin/env python3
"""Validate CMake-enumerated sources for the Mac ARM64 shared/no-JIT
RelWithDebInfo + ICU-include-path profile against build-zigpilot/compile_commands.json.
Tiny evaluator for the simple if/elseif/else/endif conditionals in the .txt files."""
import json, os, re, sys

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
    """Return (active_files_in_order, all_seen) honoring conditionals."""
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

cc = json.load(open(os.path.join(ROOT, 'build-zigpilot/compile_commands.json')))
db_files = set(u['file'] for u in cc)

expected = {}
for gdir, txt in GROUPS.items():
    toks = parse_sources(os.path.join(ROOT, txt))
    # bin/ch static-only TestHooks.cpp uses relative ../ path; STATIC_LIBRARY off so excluded by conditional parse
    for t in toks:
        if t.endswith('.gen'):
            continue
        ap = os.path.normpath(os.path.join(ROOT, gdir, t))
        expected.setdefault(ap, []).append(f'{txt}:{t}')

# lib/ChakraCoreStatic.cpp is listed directly in lib/CMakeLists.txt
expected[os.path.join(ROOT, 'lib/ChakraCoreStatic.cpp')] = ['lib/CMakeLists.txt:22']

missing = sorted(set(expected) - db_files)   # in CMake, not compiled
extra = sorted(db_files - set(expected))     # compiled, not in CMake lists
nonexistent = [f for f in expected if not os.path.exists(f)]

print(f'expected={len(expected)} db={len(db_files)}')
print(f'MISSING from DB ({len(missing)}):')
for f in missing:
    print('  ', f.replace(ROOT + '/', ''), '<-', expected[f])
print(f'EXTRA in DB ({len(extra)}):')
for f in extra:
    print('  ', f.replace(ROOT + '/', ''))
print(f'NONEXISTENT on disk ({len(nonexistent)}):')
for f in nonexistent:
    print('  ', f.replace(ROOT + '/', ''), '<-', expected[f])

# Per-group counts
from collections import Counter
print()
print('per-group counts:')
for gdir in list(GROUPS) + ['lib(ChakraCoreStatic.cpp)']:
    n = sum(1 for f in expected if f.startswith(os.path.join(ROOT, gdir.split('(')[0]) + '/') or f == os.path.join(ROOT, 'lib/ChakraCoreStatic.cpp') and 'ChakraCoreStatic' in gdir)
    print(f'  {gdir}: {n}')
