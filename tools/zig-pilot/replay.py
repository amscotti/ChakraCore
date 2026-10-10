#!/usr/bin/env python3
"""Replay compile_commands.json with zig cc/c++ instead of system clang.

Usage: replay.py <build-dir> <zig-bin> [--limit N] [--jobs J] [--fail-fast]
Compiles to .o files under <build-dir>/zigpilot-obj/ (never touches real outputs).
Reports per-TU pass/fail; exit 0 iff all compiled.
"""
import json, os, shlex, subprocess, sys
from concurrent.futures import ThreadPoolExecutor

def main():
    build = sys.argv[1]
    zig = sys.argv[2]
    limit = int(sys.argv[sys.argv.index('--limit')+1]) if '--limit' in sys.argv else None
    jobs = int(sys.argv[sys.argv.index('--jobs')+1]) if '--jobs' in sys.argv else 8
    fail_fast = '--fail-fast' in sys.argv
    extra = sys.argv[sys.argv.index('--extra-flags')+1].split() if '--extra-flags' in sys.argv else []
    cc = json.load(open(os.path.join(build, 'compile_commands.json')))
    if limit: cc = cc[:limit]
    outdir = os.path.join(os.path.abspath(build), 'zigpilot-obj')
    os.makedirs(outdir, exist_ok=True)

    def one(i, e):
        argv = shlex.split(e['command'])
        is_cxx = 'clang++' in os.path.basename(argv[0]) or 'c++' in os.path.basename(argv[0])
        argv[0] = os.path.join(zig, 'zig') + ('++' if False else '')
        # use `zig cc` / `zig c++`
        argv = [os.path.join(zig, 'zig'), 'c++' if is_cxx else 'cc'] + argv[1:]
        argv += extra
        # redirect -o to pilot dir
        if '-o' in argv:
            oi = argv.index('-o')
            argv[oi+1] = os.path.join(outdir, f'tu{i}.o')
        else:
            argv += ['-o', os.path.join(outdir, f'tu{i}.o')]
        p = subprocess.run(argv, cwd=e['directory'], capture_output=True, text=True, timeout=600)
        return (i, e['file'], p.returncode, (p.stderr or '')[-3000:])

    fails = []
    with ThreadPoolExecutor(max_workers=jobs) as ex:
        futs = [ex.submit(one, i, e) for i, e in enumerate(cc)]
        for f in futs:
            i, src, rc, err = f.result()
            status = 'OK ' if rc == 0 else 'FAIL'
            print(f'[{status}] {i}: {src}', flush=True)
            if rc != 0:
                fails.append((i, src, err))
                if fail_fast: break
    print(f'=== {len(cc)-len(fails)}/{len(cc)} compiled ===')
    for i, src, err in fails[:10]:
        print(f'--- FAIL {i}: {src}\n{err[-1500:]}')
    return 1 if fails else 0

sys.exit(main())
