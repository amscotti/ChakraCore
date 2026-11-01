#!/usr/bin/env python3
#-------------------------------------------------------------------------------------------------------
# Copyright (c) ChakraCore Project Contributors. All rights reserved.
# Licensed under the MIT license. See LICENSE.txt file in the project root for full license information.
#-------------------------------------------------------------------------------------------------------
"""Render doc/c2-jit-test262.md's results table from out/c2jit/campaign-table.json."""
import json
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
TABLE = ROOT / 'out/c2jit/campaign-table.json'
RECEIPT = ROOT / 'doc/c2-jit-test262.md'


def cell(batch, mode):
    info = batch.get(mode)
    if not info:
        return '—'
    return (f'{info["passed"]}/{info["failed"]} of {info["scenarios"]}')


def main():
    table = json.loads(TABLE.read_text())
    order = [b['name'] for b in json.loads((ROOT / 'tools/jit-test262/batches.json').read_text())['batches']]
    rows = []
    total_diffs = 0
    for name in order:
        batch = table.get(name)
        if not batch:
            continue
        line = [f'`{name}`']
        mode_diffs = []
        for mode in ('nojit', 'interp', 'fulljit'):
            line.append(cell(batch, mode))
        for mode in ('interp', 'fulljit'):
            diff = batch['diffs'].get(mode)
            if diff is None:
                mode_diffs.append(f'{mode}: MISSING')
                total_diffs += 0
            elif 'scenario_set_mismatch' in diff:
                mode_diffs.append(f'{mode}: SET MISMATCH')
                total_diffs += 0
            else:
                mode_diffs.append(f'{mode}: {diff["diff_count"]}')
                total_diffs += diff['diff_count']
        line.append('; '.join(mode_diffs))
        rows.append('| ' + ' | '.join(line) + ' |')
    header = ('| Batch (receipt) | nojit P/F of N | interp P/F of N | fulljit P/F of N | '
              'diffs vs nojit |\n|---|---|---|---|---|')
    body = '\n'.join(rows) + f'\n\nTotal mode-vs-nojit diffs: **{total_diffs}**.\n'
    text = RECEIPT.read_text()
    text = re.sub(r'<!-- RESULTS-TABLE -->.*?(?=\n## )', '<!-- RESULTS-TABLE -->\n\n' + header + '\n' + body + '\n',
                  text, flags=re.S)
    RECEIPT.write_text(text)
    print(f'rendered {len(rows)} rows, {total_diffs} total diffs -> {RECEIPT}')


if __name__ == '__main__':
    sys.exit(main())
