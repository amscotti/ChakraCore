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
import re
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


def engine_identity():
    """(commit, dirty) of the engine tree the binary was invoked from.

    Recorded in summary.json so later tools (refresh_baseline.py's delta
    join) can attribute flips to the commit range between two runs. The
    pair is evidence metadata only — the run never verifies the binary
    was built from this tree (the binary hash is the identity that
    matters); None/None when not a git checkout.
    """
    try:
        commit = subprocess.check_output(['git', '-C', str(ROOT), 'rev-parse', 'HEAD'],
                                         text=True, stderr=subprocess.DEVNULL).strip()
        dirty = bool(subprocess.check_output(['git', '-C', str(ROOT), 'status', '--porcelain'],
                                             text=True, stderr=subprocess.DEVNULL).strip())
    except (OSError, subprocess.SubprocessError):
        return None, None
    return commit, dirty


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


MARKER_PATTERN = re.compile(r'^CH_NEGATIVE_PHASE: (parse|resolution|runtime)$', re.MULTILINE)


def marker_evidence(record):
    """Host-reported negative phase from the CH_NEGATIVE_PHASE marker.

    ``ch -NegativePhaseReport`` emits exactly one marker line on stderr, at
    the first unhandled-error report, classified from where the failure
    originated (parse/compile stage of the root entry point, a module-graph
    failure witnessed while loading/linking, or evaluation) — never from
    error-message text. See doc/c1-negative-phase.md for the classification
    mapping and its honest limits.

    Spoofing defense (marker text is attacker-controlled once it travels in
    a thrown error's string): the REAL marker is the **first** marker-shaped
    stderr line and it must be **line 1** of stderr. Soundness rests on the
    emission-site audit: every ReportNegativePhaseMarker call site
    immediately precedes its PrintException, and no test-reachable API
    writes stderr before the first error report (print/console go to
    stdout; agent processes are separate and silent here) — so a forged
    marker-shaped line can only appear AFTER the real one, inside the error
    message text. Returns ``(phase, state)`` with state ``'line1'`` (trust
    the marker), ``'displaced'`` (a marker-shaped line exists but not at
    line 1 — never trusted; the caller falls back to the shape heuristic
    loudly), or ``'absent'`` (no marker: pre-flag host, or a crash/timeout
    that never reached an error report).
    """
    stderr = (record.get('rawResult') or {}).get('stderr') or ''
    first = MARKER_PATTERN.search(stderr)
    if first is None:
        return None, 'absent'
    if first.start() == 0:
        return first.group(1), 'line1'
    return None, 'displaced'


def marker_phase(record):
    """Phase of the trusted (line-1) host marker, or None."""
    return marker_evidence(record)[0]


def phase_evidence(record):
    """Observable phase shape of a recorded error (C1 heuristic; now the
    marker-less fallback for :func:`negative_phase_judgement`).

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
    # Strip the marker line(s) before shape checks: with the flag on the
    # marker precedes the error report, which would otherwise break the
    # loader-prefix detection (marker-judged records never consult the
    # shape, but the recorded evidence should reflect the true shape).
    stderr = re.sub(r'^CH_NEGATIVE_PHASE: (?:parse|resolution|runtime)\r?\n?', '',
                    raw.get('stderr') or '', count=1)
    if stderr.startswith('thrown at ') and not stack:
        return 'loader-prefix'
    if not stack:
        return 'empty-stack'
    source = stack[0].get('source') or ''
    if 'Global code' in source or 'Module code' in source or 'eval code' in source:
        return 'runtime-frame'
    if '\tat code (' in source:
        return 'parse-frame'
    return 'other-frame'


def negative_phase_judgement(record):
    """Re-judge a negative scenario by the host-reported phase.

    Primary evidence is the host marker (:func:`marker_phase`): a harness
    pass stands only if the marker's phase equals the declared
    ``negative.phase`` and, for parse/resolution, no test code executed
    (which the host's classification already guarantees by construction;
    the stdout check stays as a second signal). When the marker is absent —
    a pre-flag host, or a crash/timeout that never reached an error report —
    fall back to the C1 output-shape heuristic (:func:`phase_evidence`).
    Returns ``(ok, evidence)``; ``evidence['judged_by']`` records which
    path judged the scenario ('marker', 'shape', or 'not-negative').
    """
    negative = (record.get('attrs') or {}).get('negative') or {}
    phase = negative.get('phase')
    executed = bool((record.get('rawResult') or {}).get('stdout'))
    if not phase:
        return True, {'declared': None, 'marker': None, 'marker_state': None,
                      'shape': None, 'executed': executed, 'judged_by': 'not-negative'}
    shape = phase_evidence(record)
    marker, marker_state = marker_evidence(record)
    if marker_state == 'line1':
        ok = marker == phase and not (marker in ('parse', 'resolution') and executed)
        return ok, {'declared': phase, 'marker': marker, 'marker_state': marker_state,
                    'shape': shape, 'executed': executed, 'judged_by': 'marker'}
    if phase == 'parse':
        ok = not executed and shape in ('parse-frame', 'loader-prefix')
    elif phase == 'resolution':
        ok = not executed and shape == 'loader-prefix'
    else:
        ok = shape != 'parse-frame'
    # Loud fallback: distinguish a marker-less record (pre-flag host,
    # crash/timeout) from one carrying a displaced — untrusted —
    # marker-shaped line.
    return ok, {'declared': phase, 'marker': None, 'marker_state': marker_state,
                'shape': shape, 'executed': executed,
                'judged_by': 'shape' if marker_state == 'absent'
                else 'shape:marker-displaced'}


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
    parser.add_argument('--host-flags', default='-nonative -Test262 -CollectGarbage -NegativePhaseReport')
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
        ok, evidence = negative_phase_judgement(record)
        # Emit-once verification: a failing run carries exactly one host
        # marker line, at line 1 of stderr. The spoof control deliberately
        # adds a forged marker-shaped line inside its error text, so there
        # the count is exactly two and the first (trusted) marker must be
        # the real phase — never the forged one.
        marker_lines = MARKER_PATTERN.findall((record.get('rawResult') or {}).get('stderr') or '')
        if name == 'fail-marker-spoof.js':
            if marker_lines != ['runtime', 'parse']:
                raise ValueError(f'control {name}: expected the real runtime marker followed '
                                 f'by exactly the forged parse line, found {marker_lines}')
        elif len(marker_lines) > 1:
            raise ValueError(f'control {name}: expected at most one host marker line '
                             f'(emit-once), found {len(marker_lines)}: {marker_lines}')
        if (record['result']['pass'] and ok) != expected.get(name):
            raise ValueError(f'control {name} produced the wrong outcome: {record["result"]} '
                             f'(phase evidence: {evidence})')
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
    # Re-judge every negative scenario against the host-reported phase: a
    # harness pass is downgraded when the marker (or, in its absence, the
    # legacy shape heuristic) contradicts the declared negative.phase. Each
    # negative record carries its evidence in results.json (schema addition
    # 'phase_evidence': declared/marker/shape/executed/judged_by).
    downgraded = Counter()
    judged_by = Counter()
    for record in records:
        negative = (record.get('attrs') or {}).get('negative') or {}
        if not negative.get('phase'):
            continue
        ok, evidence = negative_phase_judgement(record)
        record['phase_evidence'] = evidence
        judged_by[evidence['judged_by'] if record['result']['pass']
                  else f'{evidence["judged_by"]}+harness-failed'] += 1
        if record['result']['pass'] and not ok:
            record['result'] = {
                'pass': False,
                'message': (f'Negative phase mismatch: declared {evidence["declared"]}, '
                            f'host reported {evidence["marker"] or evidence["shape"]} '
                            f'(judged by {evidence["judged_by"]})'),
            }
            downgraded[evidence['judged_by']] += 1
    if judged_by:
        # Rewrite results.json so the recorded records carry the re-judged
        # outcomes and phase evidence (the harness wrote its raw stream
        # before judging).
        (args.output / 'results.json').write_text(json.dumps(records) + '\n')
    totals = Counter('passed' if record['result']['pass'] else 'failed' for record in records)
    engine_commit, engine_dirty = engine_identity()
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
        'engine_commit': engine_commit, 'engine_tree_dirty': engine_dirty,
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
        'negative_phase': {
            'mechanism': ('host marker CH_NEGATIVE_PHASE (ch -NegativePhaseReport), one stderr '
                          'line per failing run classified from where the failure originated; '
                          'the C1 output-shape heuristic is the fallback only when the marker '
                          'is absent (crash/timeout/pre-flag host)'),
            'downgraded_pass_to_fail': dict(downgraded),
            'judged': dict(judged_by),
        },
        'limitations': ['negative scenarios are re-judged against the host-reported phase '
                        'marker (schema addition: per-record phase_evidence; see '
                        'doc/c1-negative-phase.md for the classification mapping and the '
                        'module-graph limits that remain); the shape heuristic only judges '
                        'marker-less records',
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
