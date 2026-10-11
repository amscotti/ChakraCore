#!/usr/bin/env python3
#-------------------------------------------------------------------------------------------------------
# Copyright (c) ChakraCore Project Contributors. All rights reserved.
# Licensed under the MIT license. See LICENSE.txt file in the project root for full license information.
#-------------------------------------------------------------------------------------------------------
"""Run a pinned, control-checked test262 diagnostic baseline with ChakraCore."""
import argparse
from collections import Counter
from datetime import datetime, timezone
import hashlib
import json
import os
from pathlib import Path
import subprocess
import sys

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[1]
LOCK = json.loads((HERE / 'test262.lock.json').read_text())


def digest(path):
    with Path(path).open('rb') as source:
        return hashlib.file_digest(source, 'sha256').hexdigest()


def artifact_identity(binary):
    paths = [binary, HERE / 'package-lock.json', HERE / 'harness.js', HERE / 'host-filter.js']
    paths += [path for path in binary.parent.glob('libChakraCore.*')
              if path.suffix in ('.dylib', '.so')]
    return {str(path): digest(path) for path in paths}


def checkout(directory, prepare):
    if prepare:
        if not (directory / '.git').is_dir():
            directory.mkdir(parents=True, exist_ok=True)
            subprocess.run(['git', 'init', str(directory)], check=True)
            subprocess.run(['git', '-C', str(directory), 'remote', 'add', 'origin',
                            LOCK['repository']], check=True)
        subprocess.run(['git', '-C', str(directory), 'fetch', '--depth=1', 'origin',
                        LOCK['revision']], check=True)
        subprocess.run(['git', '-C', str(directory), 'checkout', '--detach',
                        LOCK['revision']], check=True)
    revision = subprocess.check_output(['git', '-C', str(directory), 'rev-parse', 'HEAD'], text=True).strip()
    if revision != LOCK['revision']:
        raise ValueError('test262 revision differs from test262.lock.json; run --prepare')
    dirty = subprocess.check_output(['git', '-C', str(directory), 'status', '--porcelain'], text=True)
    if dirty:
        raise ValueError('test262 checkout is modified; use a clean pinned checkout')


def harness(binary, corpus, patterns, output, args, controls=False):
    temporary = output.parent / ('control-temp' if controls else 'harness-temp')
    temporary.mkdir(exist_ok=True)
    command = [args.node, str(HERE / 'harness.js'), '--host-type=ch',
               f'--host-path={binary}', f'--host-args={args.host_flags}',
               f'--test262-dir={corpus}', f'--includes-dir={args.checkout / "harness"}',
               f'--temp-dir={temporary}', f'--threads={args.jobs}', f'--timeout={args.timeout}',
               '--reporter=json', '--reporter-keys=file,relative,attrs,scenario,result,rawResult',
               f'--preprocessor={HERE / "host-filter.js"}', *patterns]
    print(f'Running {"controls" if controls else "corpus"}: {patterns}', flush=True)
    records, opened, closed = [], False, False
    errors = output.with_suffix('.stderr.log')
    environment = os.environ.copy()
    environment['TMPDIR'] = str(temporary)
    with output.open('w') as raw, errors.open('w') as stderr:
        process = subprocess.Popen(command, cwd=corpus, stdout=subprocess.PIPE,
                                   stderr=stderr, text=True, encoding='utf-8', env=environment)
        try:
            for line in process.stdout:
                raw.write(line)
                content = line.strip().lstrip(',')
                if content == '[':
                    if opened:
                        raise ValueError('harness emitted a duplicate result-array start')
                    opened = True
                    continue
                if content == ']':
                    closed = True
                    continue
                if not content:
                    continue
                if not opened or closed:
                    raise ValueError('harness emitted results outside its result array')
                record = json.loads(content)
                if not isinstance(record.get('result', {}).get('pass'), bool):
                    raise ValueError('harness emitted an invalid result')
                records.append(record)
                if len(records) % 5000 == 0:
                    print(f'  {len(records)} scenarios completed', flush=True)
        except BaseException:
            process.kill()
            process.wait()
            raise
        rc = process.wait()
    if rc or not records or not opened or not closed:
        raise ValueError(f'harness failed or emitted no scenarios (rc={rc}); see {errors}')
    skips = [json.loads(line[len('TEST262_SKIP:'):]) for line in errors.read_text().splitlines()
             if line.startswith('TEST262_SKIP:')]
    return records, command, skips


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--binary', type=Path, required=True)
    parser.add_argument('--checkout', type=Path, default=ROOT / 'out/test262')
    parser.add_argument('--output', type=Path, default=ROOT / 'out/test262-results')
    parser.add_argument('--prepare', action='store_true', help='fetch/check out the lockfile revision')
    parser.add_argument('--controls-only', action='store_true')
    parser.add_argument('--node', default='node')
    parser.add_argument('--jobs', type=int, default=8)
    parser.add_argument('--timeout', type=int, default=10000, help='per-scenario timeout, milliseconds')
    parser.add_argument('--host-flags', default='-nonative -Test262 -CollectGarbage')
    parser.add_argument('patterns', nargs='*', help='quoted test262-relative globs')
    args = parser.parse_args()
    args.binary, args.checkout, args.output = (path.resolve() for path in (args.binary, args.checkout, args.output))
    if args.jobs <= 0 or args.timeout <= 0:
        parser.error('--jobs and --timeout must be positive')
    if not args.binary.is_file():
        parser.error('--binary does not exist')
    if not (HERE / 'node_modules/test262-harness/bin/run.js').is_file():
        parser.error('run npm ci --ignore-scripts in tools/test262 first')
    args.output.mkdir(parents=True, exist_ok=True)
    checkout(args.checkout, args.prepare)
    identity = artifact_identity(args.binary)
    controls, _, _ = harness(args.binary, HERE / 'controls', ['test/*.js'],
                             args.output / 'controls.json', args, controls=True)
    expected = {path.name: not path.name.startswith('fail-') for path in
                (HERE / 'controls/test').glob('*.js') if '_FIXTURE' not in path.name}
    observed = Counter()
    for record in controls:
        name = Path(record['file']).name
        observed[name] += 1
        if record['result']['pass'] != expected.get(name):
            raise ValueError(f'control {name} produced the wrong outcome: {record["result"]}')
    if set(observed) != set(expected) or any(count != 1 for count in observed.values()):
        raise ValueError('control coverage is incomplete')
    print(f'Controls passed: {len(controls)} scenarios (including deliberate failures)', flush=True)
    if args.controls_only:
        if artifact_identity(args.binary) != identity:
            raise ValueError('binary/library/harness inputs changed during controls')
        return 0
    patterns = args.patterns or ['test/language/**/*.js', 'test/built-ins/**/*.js', 'test/annexB/**/*.js']
    records, command, skips = harness(args.binary, args.checkout, patterns,
                                      args.output / 'results.json', args)
    checkout(args.checkout, prepare=False)
    if artifact_identity(args.binary) != identity:
        raise ValueError('binary/library/harness inputs changed during the baseline run')
    totals = Counter('passed' if record['result']['pass'] else 'failed' for record in records)
    features = {}
    for record in records:
        for feature in record['attrs'].get('features', []) or ['untagged']:
            counts = features.setdefault(feature, {'passed': 0, 'failed': 0})
            counts['passed' if record['result']['pass'] else 'failed'] += 1
    shared = [path for path in args.binary.parent.glob('libChakraCore.*')
              if path.suffix in ('.dylib', '.so')]
    summary = {
        'scope': LOCK['scope'], 'test262_revision': LOCK['revision'],
        'recorded_utc': datetime.now(timezone.utc).isoformat(),
        'binary_sha256': digest(args.binary),
        'shared_libraries': {path.name: digest(path) for path in shared},
        'package_lock_sha256': digest(HERE / 'package-lock.json'),
        'node_version': subprocess.check_output([args.node, '--version'], text=True).strip(),
        'host_flags': args.host_flags, 'patterns': patterns, 'command': command,
        'host_exclusions': ['IsHTMLDDA', 'CanBlockIsFalse'],
        'scenario_count': len(records), 'source_file_count': len({r['file'] for r in records}),
        'skipped_scenarios': len(skips), 'skip_reasons': dict(Counter(item['reason'] for item in skips)),
        'passed': totals['passed'], 'failed': totals['failed'], 'features': features,
        'controls': len(controls),
        'input_identity': identity,
        'limitations': ['upstream validator checks negative error type, not every negative phase',
                        'corpus includes post-ES2025 proposals; this is a diagnostic baseline'],
    }
    (args.output / 'summary.json').write_text(json.dumps(summary, indent=2, sort_keys=True) + '\n')
    print(f'test262: {totals["passed"]} passed / {totals["failed"]} failed; '
          f'{len(records)} scenarios, {summary["source_file_count"]} files')
    print(f'Summary: {args.output / "summary.json"}')
    # A diagnostic baseline with conformance failures is still a failed check.
    return 1 if totals['failed'] else 0


if __name__ == '__main__':
    try:
        sys.exit(main())
    except (ValueError, OSError, subprocess.SubprocessError) as error:
        print(f'ABORT: {error}', file=sys.stderr)
        sys.exit(2)
