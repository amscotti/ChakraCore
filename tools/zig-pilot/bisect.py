#!/usr/bin/env python3
"""Bisect the -Werror/-Wdate-time divergence between AppleClang and zig c++."""
import json, os, shlex, subprocess, sys

build = sys.argv[1]
zigbin = sys.argv[2]
d = json.load(open(f'{build}/compile_commands.json'))
e = [x for x in d if 'JsrtHelper' in x['file']][0]
base = shlex.split(e['command'])
cxx = base[0]

outdir = os.path.join(os.path.abspath(build), 'zigpilot-obj')
os.makedirs(outdir, exist_ok=True)
def run(name, argv):
    argv = [a for a in argv]
    if '-o' in argv:
        argv[argv.index('-o')+1] = os.path.join(outdir, f'bisect-{name}.o')
    p = subprocess.run(argv, cwd=e['directory'], capture_output=True, text=True, timeout=300)
    diag = 'clean' if p.returncode == 0 and 'warning' not in p.stderr.lower() and 'error' not in p.stderr.lower() else (p.stderr.strip().splitlines() or ['?'])[-1][:160]
    print(f'{name}: rc={p.returncode} :: {diag}')

nowerr = [a for a in base if a != '-Werror']
zig = lambda argv: [f'{zigbin}/zig', 'c++'] + [a for a in argv if a != cxx]

run('apple-as-is', base)
run('zig-as-is', zig(base))
run('zig-no-Werror-flag', zig(nowerr))
run('zig-plus-nodate', zig(base) + ['-Wno-error=date-time'])
run('zig-only-nodate', zig(nowerr) + ['-Wno-date-time'])
