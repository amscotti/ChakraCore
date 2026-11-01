#!/usr/bin/env python3
#-------------------------------------------------------------------------------------------------------
# Copyright (c) ChakraCore Project Contributors. All rights reserved.
# Licensed under the MIT license. See LICENSE.txt file in the project root for full license information.
#-------------------------------------------------------------------------------------------------------
"""C2 per-batch test262 JIT measurement campaign driver (doc/c2-jit-test262.md).

Runs each batch's pinned test262 corpus (globs taken verbatim from the batch
receipt) on the Mac ARM64 JIT binary in three host modes and diffs per-scenario
outcomes:

  nojit   -nonative                                          (baseline, same binary)
  interp  -maxInterpretCount:1 -maxSimpleJitRunCount:1 -bgjit- (interpret once, then full JIT)
  fulljit -forceNative -off:simpleJit -bgJitDelay:0          (compile everything up front)

Usage:
  campaign.py run <batch> [<batch>...]     # run all three modes sequentially
  campaign.py diff <batch> [<batch>...]    # diff modes vs nojit, update summary table
  campaign.py list
"""
import json
import subprocess
import sys
import time
from pathlib import Path

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[1]
SPEC = json.loads((HERE / 'batches.json').read_text())
RUN = ROOT / 'tools/test262/run.py'
CHECKOUT = Path('/Users/ascotti/Documents/code/ChakraCore/out/test262')
BINARY = ROOT / 'out/jit/bin/ch'
OUTROOT = ROOT / 'out/c2jit'
MODES = ('nojit', 'interp', 'fulljit')


def batch_spec(name):
    for batch in SPEC['batches']:
        if batch['name'] == name:
            return batch
    raise SystemExit(f'unknown batch {name}; known: {", ".join(b["name"] for b in SPEC["batches"])}')


def batch_globs(batch):
    globs = list(batch['globs'])
    source = batch.get('globs_from_file')
    if source:
        globs += [line.strip() for line in (ROOT / source).read_text().splitlines()
                  if line.strip() and not line.startswith('#')]
    if not globs:
        raise SystemExit(f'batch {batch["name"]} has no globs')
    return globs


def run_mode(batch, mode):
    outdir = OUTROOT / batch['name'] / mode
    if (outdir / 'summary.json').is_file():
        print(f'[{batch["name"]}/{mode}] already complete, skipping', flush=True)
        return
    flags = f"{SPEC['modes'][mode]} {SPEC['common_flags']} {batch.get('extra_flags', '')}".strip()
    command = ['python3', str(RUN), '--binary', str(BINARY), '--checkout', str(CHECKOUT),
               '--output', str(outdir), '--jobs', '8', '--host-flags', flags] + batch_globs(batch)
    started = time.monotonic()
    print(f'[{batch["name"]}/{mode}] {flags}', flush=True)
    result = subprocess.run(command, cwd=ROOT, capture_output=True, text=True)
    seconds = round(time.monotonic() - started, 1)
    log = outdir.parent / f'{mode}.run.log'
    outdir.parent.mkdir(parents=True, exist_ok=True)
    log.write_text(f'$ {" ".join(command)}\nexit {result.returncode} in {seconds}s\n'
                   f'{result.stdout}\n{result.stderr}')
    print(f'[{batch["name"]}/{mode}] exit {result.returncode} in {seconds}s', flush=True)


def load(mode_dir):
    records = json.loads((mode_dir / 'results.json').read_text())
    return {(r['relative'], r['scenario']): r for r in records}


def outcome(record):
    return 'pass' if record['result']['pass'] else 'fail'


def failure_shape(record):
    """Classify a failure: crash / timeout / wrong-result (+ short evidence)."""
    raw = record.get('rawResult') or {}
    status = raw.get('exitStatus') or {}
    message = record['result'].get('message') or ''
    error = raw.get('error') or ''
    if isinstance(error, dict):
        error = error.get('message') or str(error)
    signal = status.get('signal')
    if signal:
        return f'crash:{signal}'
    if 'timed out' in message or 'timed out' in error:
        return 'timeout'
    return 'wrong-result'


def diff_batch(batch):
    name = batch['name']
    base = OUTROOT / name / 'nojit'
    rows = {}
    summary = {'receipt': batch['receipt'], 'receipt_totals': batch['receipt_totals'],
               'extra_flags': batch.get('extra_flags', '')}
    for mode in MODES:
        mode_dir = OUTROOT / name / mode
        summary_file = mode_dir / 'summary.json'
        if not summary_file.is_file():
            summary[mode] = None
            continue
        data = json.loads(summary_file.read_text())
        summary[mode] = {'passed': data['passed'], 'failed': data['failed'],
                         'scenarios': data['scenario_count'],
                         'files': data['source_file_count']}
    diffs = {}
    for mode in ('interp', 'fulljit'):
        mode_dir = OUTROOT / name / mode
        if not (mode_dir / 'results.json').is_file() or not (base / 'results.json').is_file():
            diffs[mode] = None
            continue
        nojit, other = load(base), load(mode_dir)
        if set(nojit) != set(other):
            only_base = sorted(set(nojit) - set(other))[:5]
            only_mode = sorted(set(other) - set(nojit))[:5]
            diffs[mode] = {'scenario_set_mismatch': {
                'only_in_nojit': only_base, 'only_in_mode': only_mode,
                'nojit_count': len(nojit), 'mode_count': len(other)}}
            continue
        entries = []
        for key in nojit:
            a, b = outcome(nojit[key]), outcome(other[key])
            if a != b:
                entries.append({
                    'file': key[0], 'scenario': key[1],
                    'nojit': a, 'mode': b,
                    'direction': f'{a}->{b}',
                    'mode_shape': failure_shape(other[key]),
                    'nojit_shape': failure_shape(nojit[key]),
                    'message': (other[key]['result'].get('message') or '')[:200],
                })
        diffs[mode] = {'diff_count': len(entries), 'entries': entries}
    summary['diffs'] = diffs
    (OUTROOT / name / 'campaign.json').write_text(json.dumps(summary, indent=1, sort_keys=True) + '\n')
    for mode in ('interp', 'fulljit'):
        info = diffs[mode]
        counts = summary.get(mode) or {}
        if info is None:
            print(f'{name}/{mode}: MISSING')
        elif 'scenario_set_mismatch' in info:
            print(f'{name}/{mode}: SCENARIO SET MISMATCH {info["scenario_set_mismatch"]}')
        else:
            print(f'{name}/{mode}: {counts.get("passed")}/{counts.get("failed")} of '
                  f'{counts.get("scenarios")} scenarios, {info["diff_count"]} diffs vs nojit')
    return summary


def main():
    if len(sys.argv) < 3 or sys.argv[1] not in ('run', 'diff', 'list'):
        raise SystemExit(__doc__)
    action, names = sys.argv[1], sys.argv[2:]
    if action == 'list':
        for batch in SPEC['batches']:
            print(batch['name'])
        return 0
    OUTROOT.mkdir(parents=True, exist_ok=True)
    if action == 'run':
        for name in names:
            batch = batch_spec(name)
            for mode in MODES:
                run_mode(batch, mode)
    table = {}
    table_file = OUTROOT / 'campaign-table.json'
    if table_file.is_file():
        table = json.loads(table_file.read_text())
    for name in names:
        table[name] = diff_batch(batch_spec(name))
    table_file.write_text(json.dumps(table, indent=1, sort_keys=True) + '\n')
    print(f'table: {table_file}', flush=True)
    return 0


if __name__ == '__main__':
    sys.exit(main())
