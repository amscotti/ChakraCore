#!/usr/bin/env python3
"""Pilot: re-run CMake archive/link/custom edges with zig c++ over zig-built .o files.

Usage: link.py <build-dir> <zig-bin> <target>... [--extra-link-flags "..."]
Takes `ninja -t commands` per target, skips compile edges (validated by
replay.py), textually swaps the compiler for `zig c++` and every input .o for
its zig-built twin (aborting if any twin is missing), rewrites @rsp files
the same way (defensive: no rsp files appear in this pilot's edges), and runs
each edge via the shell. Outputs stay at natural paths in the (disposable)
build dir. Every link/archive edge must prove its -o/ar outputs exist and are
fresh (<20 min old); pure-custom edges without such outputs still pass on rc.
"""
import json, os, shlex, subprocess, sys

build = os.path.abspath(sys.argv[1])
zigbin = sys.argv[2]
args = sys.argv[3:]
link_extra = ''
if '--extra-link-flags' in args:
    i = args.index('--extra-link-flags')
    link_extra = args[i+1]
    args = args[:i] + args[i+2:]
targets = args
cc = json.load(open(os.path.join(build, 'compile_commands.json')))
outdir = os.path.join(build, 'zigpilot-obj')

# exact -o strings as written in compile_commands (relative) -> twin abspath,
# plus absolute variants, longest-first for safe textual replacement
repl = {}
for i, e in enumerate(cc):
    argv = shlex.split(e['command'])
    if '-o' in argv:
        o = argv[argv.index('-o')+1]
        twin = os.path.join(outdir, f'tu{i}.o')
        repl[o] = twin
        if not os.path.isabs(o):
            repl[os.path.normpath(os.path.join(e['directory'], o))] = twin
ordered = sorted(repl.items(), key=lambda kv: -len(kv[0]))
# fail fast on missing twins (clear error beats an opaque link failure)
missing_twins = sorted({new for _, new in ordered if not os.path.isfile(new)})
if missing_twins:
    print(f'ABORT: {len(missing_twins)} twin object(s) missing; re-run replay.py first.')
    print('e.g. ' + ', '.join(missing_twins[:5]))
    sys.exit(2)

def sub(text):
    for old, new in ordered:
        text = text.replace(old, new)
    return text

ZIG = os.path.join(zigbin, 'zig')
rc_total = 0
for t in targets:
    cmds = subprocess.run(['ninja', '-t', 'commands', t], cwd=build,
                          capture_output=True, text=True).stdout.splitlines()
    cmds = [c.strip() for c in cmds if c.strip() and not c.strip().startswith('#')]
    cmds = [c for c in cmds if ' -c ' not in f' {c} ']
    print(f'--- target {t}: {len(cmds)} non-compile edge(s)', flush=True)
    for ci, cmd in enumerate(cmds):
        # rewrite @rsp files referenced by this edge
        for tok in shlex.split(cmd):
            if tok.startswith('@') and not tok.startswith('@rpath'):
                rp = tok[1:] if os.path.isabs(tok[1:]) else os.path.join(build, tok[1:])
                if os.path.isfile(rp):
                    nrsp = os.path.join(outdir, f'{t.replace("/","_")}.{ci}.rsp')
                    open(nrsp, 'w').write(sub(open(rp).read()))
                    cmd = cmd.replace(tok, '@' + nrsp)
        cmd = sub(cmd)
        # zig cc rejects CMake's split `-Wl,-force_load,` + `<lib>` form
        # (trailing comma -> empty path); join them. build.zig will either
        # join the flag or link the objects directly instead of force-loading.
        import re as _re
        cmd = _re.sub(r'-Wl,-force_load,\s+(\S+)', r'-Wl,-force_load,\1', cmd)
        # swap compiler textually: first clang-family driver path -> zig c++.
        # run the raw string (re-quoting would neuter `&&` chains into no-ops)
        for drv in ('/usr/bin/clang++', '/usr/bin/clang', 'clang++', 'clang'):
            if drv in cmd:
                cmd = cmd.replace(drv, f'{ZIG} c++', 1)
                break
        if link_extra and (ZIG in cmd):
            # insert before the -o output flag (only zig link edges match)
            cmd = cmd.replace(' -o ', f' {link_extra} -o ', 1)
        import re, time
        p = subprocess.run(cmd, cwd=build, shell=True,
                           capture_output=True, text=True, timeout=900)
        # verify the edge actually produced its output (rc=0 is not proof)
        produced, missing = [], []
        m = re.search(r'-o (\S+)', cmd)
        if m and not cmd.strip().startswith('cd '):
            produced.append(m.group(1))
        m2 = re.search(r'\bar qc (\S+)', cmd)
        if m2:
            produced.append(m2.group(1))
        t0 = time.time()
        for f in produced:
            fp = f if os.path.isabs(f) else os.path.join(build, f)
            if not (os.path.isfile(fp) and t0 - os.path.getmtime(fp) < 1200):
                missing.append(f)
        ok = p.returncode == 0 and not missing
        print(f'  cmd{ci}: rc={p.returncode} outputs={produced} missing={missing}', flush=True)
        if not ok:
            rc_total += 1
            print('  ERR:', (p.stderr or '')[-2500:])
print('LINK-PILOT-OK' if rc_total == 0 else 'LINK-PILOT-FAIL')
sys.exit(1 if rc_total else 0)
