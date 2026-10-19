#!/usr/bin/env python3
#-------------------------------------------------------------------------------------------------------
# Copyright (c) ChakraCore Project Contributors. All rights reserved.
# Licensed under the MIT license. See LICENSE.txt file in the project root for full license information.
#-------------------------------------------------------------------------------------------------------
"""Run a pinned, control-checked test262 diagnostic baseline with ChakraCore."""
import argparse
import atexit
from collections import Counter
from datetime import datetime, timezone
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile

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


def acquire_output_lock(output):
    # Two runs sharing one --output interleave controls.json/results.json
    # into unparseable JSON while both exit 0; refuse instead. atexit covers
    # normal returns and the sys.exit error paths; a hard-killed run leaves
    # a stale lock, which the error message tells the user how to clear.
    lock = output / '.run.lock'
    try:
        fd = os.open(lock, os.O_CREAT | os.O_EXCL | os.O_WRONLY)
    except FileExistsError:
        try:
            holder = lock.read_text().strip()
        except OSError:
            holder = 'unknown pid'
        raise ValueError(f'output directory is locked by another run ({holder}): {output}; '
                         f'if no run is active, delete {lock} and retry')
    os.write(fd, f'pid {os.getpid()}\n'.encode())
    os.close(fd)
    atexit.register(lock.unlink, missing_ok=True)


def harness(binary, corpus, patterns, output, args, controls=False):
    # Unique temp dir per run: a shared, never-cleaned dir lets stale files
    # from an earlier (or concurrent) run leak into this one and flip
    # outcomes, so never reuse a fixed location.
    temporary = Path(tempfile.mkdtemp(
        prefix='control-temp-' if controls else 'harness-temp-', dir=output.parent))
    try:
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
    finally:
        shutil.rmtree(temporary, ignore_errors=True)


def phase_evidence(record):
    """Observable phase shape of a control's recorded error (C1).

    The upstream validator checks the negative error *name* only. For the
    qualification controls we additionally require the observable shape to be
    compatible with the declared phase: this host reports parse errors with a
    tab-prefixed "\\tat code (f-...js:L:C)" frame (a runtime throw from a
    function literally named ``code`` frames as "   at code (...)" with
    spaces and must not match), module-link (resolution) errors with a
    "thrown at <file>:" loader prefix *and* no JS stack (a loader prefix
    accompanied by a JS stack falls through to frame classification, and an
    empty stack without the loader prefix is no evidence at all), and
    runtime errors with any frame shape other than the parse frame (a
    Global/Module/eval frame is positive runtime evidence, but loader and
    empty-stack shapes are also accepted for runtime by design: in the
    recorded baseline, runtime negatives thrown before a frame is pushed
    surface through the loader channel — see doc/c1-triage.md §3). Test code
    must not have executed (no stdout) for parse/resolution phases. Keep in
    sync with the audit heuristic in analyze.py; corpus-wide limits are in
    doc/c1-triage.md.
    """
    raw = record.get('rawResult') or {}
    stack = (raw.get('error') or {}).get('stack') or []
    if (raw.get('stderr') or '').startswith('thrown at ') and not stack:
        return 'loader-prefix'
    if not stack:
        return 'empty-stack'
    source = stack[0].get('source') or ''
    if 'Global code' in source or 'Module code' in source or 'eval code' in source:
        return 'runtime-frame'
    if '\tat code (' in source:
        return 'parse-frame'
    return 'other-frame'


def control_passed(record):
    """Harness outcome downgraded by negative-phase evidence (controls only)."""
    if not record['result']['pass']:
        return False
    negative = (record.get('attrs') or {}).get('negative') or {}
    phase = negative.get('phase')
    if not phase:
        return True
    shape = phase_evidence(record)
    executed = bool((record.get('rawResult') or {}).get('stdout'))
    if phase == 'parse':
        return not executed and shape in ('parse-frame', 'loader-prefix')
    if phase == 'resolution':
        return not executed and shape == 'loader-prefix'
    if phase == 'runtime':
        return shape != 'parse-frame'
    return True


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
    acquire_output_lock(args.output)
    checkout(args.checkout, args.prepare)
    identity = artifact_identity(args.binary)
    controls, _, _ = harness(args.binary, HERE / 'controls', ['test/**/*.js'],
                             args.output / 'controls.json', args, controls=True)
    expected = {path.name: not path.name.startswith('fail-') for path in
                (HERE / 'controls/test').rglob('*.js') if '_FIXTURE' not in path.name}
    observed = Counter()
    for record in controls:
        name = Path(record['file']).name
        observed[name] += 1
        if control_passed(record) != expected.get(name):
            raise ValueError(f'control {name} produced the wrong outcome: {record["result"]} '
                             f'(phase evidence: {phase_evidence(record)})')
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
        'jobs': args.jobs, 'timeout_ms': args.timeout,
        'host_exclusions': ['IsHTMLDDA', 'CanBlockIsFalse'],
        'scenario_count': len(records), 'source_file_count': len({r['file'] for r in records}),
        'skipped_scenarios': len(skips), 'skip_reasons': dict(Counter(item['reason'] for item in skips)),
        'passed': totals['passed'], 'failed': totals['failed'], 'features': features,
        'controls': len(controls),
        'input_identity': identity,
        'limitations': ['upstream validator checks negative error type, not every negative phase; '
                        'controls add phase-evidence checks and analyze.py audits recorded phases '
                        '(see doc/c1-triage.md for the residual heuristic limits)',
                        'corpus includes post-ES2025 proposals; use edition-policy.json + analyze.py '
                        'for the edition-selected score'],
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
