#!/usr/bin/env python3
#-----------------------------------------------------------------------
# Copyright (c) ChakraCore Project Contributors. All rights reserved.
# Licensed under the MIT license. See LICENSE.txt file in the project root for full license information.
#-----------------------------------------------------------------------
"""One-command certified-baseline refresh: run + analyze + delta join + dated report.

Manual invocation only (nothing wires this into a gate). It collapses the
C4 certification procedure (doc/c4-run-evidence.md §2, doc/c4-manifest.md)
into a single command with the exact certified configuration:

  1. full certified corpus through run.py — default host flags
     (``-nonative -Test262 -CollectGarbage -NegativePhaseReport``), default
     patterns, ``--jobs 8``, ``--timeout 10000``, absolute binary, controls
     first, corpus pinned/verified by run.py;
  2. analyze.py over the fresh results.json + summary.json (edition policy,
     failure buckets, negative-phase audit);
  3. a key-join against the PREVIOUS certified run — key ``(file,
     scenario)`` — listing every fail→pass / pass→fail flip with
     attribution hints: the engine commits between the two runs' recorded
     engine_commits (git log, lib/pal/bin) and the doc/ receipts that name
     the flipped file;
  4. a dated markdown report (``<output>/refresh-report.md``) plus the
     analysis json/txt, echoed to stdout; optionally append the run to
     baseline-register.json (--publish, see below).

The previous run is resolved from --previous (a run directory holding
results.json/summary.json) or, by default, the newest baseline-register.json
entry whose results are still on disk. The c4-cert run itself (the register's
first entry) predates this tool and its 113 MB results.json was not retained
— when no previous results exist the report says so and the delta section is
skipped rather than guessed. Totals-only comparison against register entries
is always emitted (it needs no per-key data).

Usage:

    python3 tools/test262/refresh_baseline.py --binary "$PWD/zig-out/bin/ch"
    python3 tools/test262/refresh_baseline.py --binary ... --publish
    python3 tools/test262/refresh_baseline.py --skip-run --results out/run-new \\
        --previous out/run-old        # re-report an existing pair (join debug)

Exit code: 0 iff the certified run + analysis + report completed (a corpus
with conformance failures is expected — run.py's rc=1 is normal here); 2 on
any abort (failed controls, harness failure, missing previous data with
--previous, unwritable output, ...). This is a measurement tool: green exit
does not mean the engine conforms.
"""
import argparse
from collections import Counter
from datetime import datetime, timezone
import hashlib
import json
from pathlib import Path
import re
import subprocess
import sys

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[1]
sys.path.insert(0, str(HERE))

import analyze  # noqa: E402  (sibling module; stdlib-only like this tool)

REGISTER = HERE / 'baseline-register.json'
CERTIFIED_JOBS = 8
CERTIFIED_TIMEOUT_MS = 10000
# The certified host flags are run.py's defaults (c4-run-evidence.md §2);
# restated here so a future run.py default change cannot silently drift
# this procedure — the tool asserts they still match.
CERTIFIED_HOST_FLAGS = '-nonative -Test262 -CollectGarbage -NegativePhaseReport'
ENGINE_PATHS = ('lib', 'pal', 'bin')  # engine-source trees for attribution


def load_register():
    if not REGISTER.is_file():
        raise ValueError(f'baseline register missing: {REGISTER}')
    register = json.loads(REGISTER.read_text())
    if not isinstance(register.get('runs'), list) or not register['runs']:
        raise ValueError(f'baseline register has no runs: {REGISTER}')
    return register


def previous_from_register(register):
    """Newest register entry whose results.json still exists, else None."""
    for entry in reversed(register['runs']):
        if entry.get('results_retained') and \
                (ROOT / entry['results_dir'] / 'results.json').is_file():
            return entry
    return None


def run_corpus(binary, output, jobs, node):
    command = [sys.executable, str(HERE / 'run.py'), '--binary', str(binary),
               '--output', str(output), '--jobs', str(jobs)]
    if node:
        command += ['--node', node]
    print(f'[refresh] certified corpus run: {" ".join(command)}', flush=True)
    completed = subprocess.run(command, cwd=str(ROOT))
    if completed.returncode == 2:
        raise ValueError('run.py aborted (controls/harness/checkout failure; see output above)')
    if completed.returncode not in (0, 1):
        raise ValueError(f'run.py exited rc={completed.returncode}')
    # rc=1: conformance failures exist — expected for a diagnostic baseline.


def analyze_run(results_path, summary_path):
    records = json.loads(results_path.read_text())
    summary = json.loads(summary_path.read_text())
    _, category, flag_tags = analyze.load_policy(HERE / 'edition-policy.json')
    report = analyze.analyze(records, category, flag_tags, summary)
    report['policy_sha256'] = hashlib.sha256(
        (HERE / 'edition-policy.json').read_bytes()).hexdigest()
    return records, summary, report


def join_runs(previous_records, records):
    """Key-join two results lists on (file, scenario).

    Returns a dict with symmetric-difference counts and the flip lists;
    pure function (no I/O) so --self-test can exercise it on synthetic
    data. Messages/tags are captured per flip for the report and for
    attribution greps; a scenario mismatch on the same file (corpus
    revision drift) is reported as key drift, never silently dropped.
    """
    def keyed(entries):
        table = {}
        for record in entries:
            key = (record['file'], record.get('scenario'))
            if key in table:
                raise ValueError(f'duplicate (file, scenario) key in previous results: {key}')
            table[key] = record
        return table

    prev, new = keyed(previous_records), keyed(records)
    common = set(prev) & set(new)
    flips = {'fail->pass': [], 'pass->fail': []}
    for key in sorted(common):
        before, after = prev[key]['result']['pass'], new[key]['result']['pass']
        if before and not after:
            flips['pass->fail'].append(flip_entry(new[key]))
        elif after and not before:
            flips['fail->pass'].append(flip_entry(new[key]))
    return {
        'previous_scenarios': len(prev), 'new_scenarios': len(new),
        'common_keys': len(common),
        'only_in_previous': sorted(set(prev) - set(new)),
        'only_in_new': sorted(set(new) - set(prev)),
        'fail_to_pass': flips['fail->pass'], 'pass_to_fail': flips['pass->fail'],
    }


def flip_entry(record):
    return {'file': record['file'], 'scenario': record.get('scenario'),
            'tags': (record.get('attrs') or {}).get('features') or [],
            'message': (((record.get('result') or {}).get('message') or '')[:160]) or None}


def commit_range(previous_commit, current_commit):
    """Engine commits (lib/pal/bin) between the two runs' engine commits."""
    if not previous_commit or not current_commit:
        return None
    if previous_commit == current_commit:
        return []
    command = ['git', '-C', str(ROOT), 'log', '--oneline', '--no-merges',
               f'{previous_commit}..{current_commit}', '--', *ENGINE_PATHS]
    try:
        out = subprocess.check_output(command, text=True, stderr=subprocess.DEVNULL)
    except (OSError, subprocess.SubprocessError):
        return None
    return out.strip().splitlines()


def receipt_hints(files):
    """doc/ receipts naming each flipped file basename (attribution hints).

    Lane receipts record their corpus witnesses by filename; a basename
    grep over doc/*.md is a hint, not proof — the report says so.
    """
    docs = sorted(HERE.parents[1].joinpath('doc').glob('*.md'))
    corpus = {path.name: path for path in docs}
    hints = {}
    for name in files:
        basename = Path(name).name
        hits = [corpus[doc.name].name for doc in docs
                if basename in doc.read_text(errors='replace')]
        if hits:
            hints[name] = hits
    return hints


def totals_line(totals):
    return (f"{totals['passed']} passed / {totals['failed']} failed / "
            f"{totals['scenarios']} scenarios ({totals['unique_files']} files)")


def render_report(summary, report, join, attribution, register_entry, results_dir):
    now = datetime.now(timezone.utc).strftime('%Y-%m-%d %H:%M UTC')
    lines = [f'# Certified-baseline refresh report — {now}', '']
    lines += ['- Generated by `tools/test262/refresh_baseline.py` (manual tool; this is a',
              '  measurement, not a conformance claim).', '']
    lines += ['## 1. Run identity', '']
    engine = summary.get('engine_commit') or 'not recorded'
    dirty = ' (tree dirty at run time!)' if summary.get('engine_tree_dirty') else ''
    dylib = summary.get('engine_dylib_sha256')
    lines += [f"- Engine commit: `{engine}`{dirty}",
              f"- Binary SHA-256 (launcher): `{summary['binary_sha256']}`",
              f"- Engine dylib SHA-256: `{dylib if dylib else 'none recorded (static build or pre-recording run)'}`",
              f"- Corpus revision: `{summary['test262_revision']}` (patterns: "
              f"{', '.join(summary['patterns'])})",
              f"- Host flags: `{summary['host_flags']}` · jobs {summary['jobs']} · "
              f"timeout {summary['timeout_ms']} ms · Node {summary['node_version']}",
              f"- Controls: {summary['controls']} scenarios · skipped scenarios: "
              f"{summary['skipped_scenarios']} {summary['skip_reasons']}",
              f"- Negative-phase downgrades (pass→fail, host marker): "
              f"{summary['negative_phase']['downgraded_pass_to_fail']}", '']
    lines += ['## 2. Totals', '']
    for label in ('broad', 'edition_selected'):
        t = report[label]
        rate = (100.0 * t['passed'] / t['scenarios']) if t['scenarios'] else 0.0
        lines.append(f"- **{label}**: {totals_line(t)} — {rate:.2f}% passed")
    lines += [f"- excluded_from_edition: {report['excluded_from_edition']}", '']
    lines += ['## 3. Failure buckets (analyze.py first-match rules)', '',
              '| bucket | scenarios |', '|---|---:|']
    for bucket, count in report['failure_buckets'].items():
        lines.append(f'| {bucket} | {count} |')
    lines.append('')
    audit = report['negative_phase_audit']
    lines += ['## 4. Negative-phase audit', '',
              f"- passed negatives (total): {audit['passed_negatives_total']}",
              f"- marker-judged: {audit['marker_judged_total']}; "
              f"run.py downgrades: {len(audit['marker_downgraded_by_run_py'])}"]
    for phase in ('parse', 'resolution', 'runtime'):
        entry = audit[phase]
        lines.append(f"- {phase}: {entry['passed_negatives']} passed, "
                     f"{entry['suspects']} shape-suspects "
                     f"{entry['suspect_reasons']}")
    lines += ['']
    lines += ['## 5. Delta vs the previous certified run', '']
    if register_entry is None:
        lines += ['- No comparable previous run resolved (no --previous given and no '
                  'register entry retains results on disk). Totals-only comparison '
                  'against every register entry follows; per-key deltas skipped.',
                  '']
    else:
        prev = register_entry
        comparable = not join['only_in_previous'] and not join['only_in_new']
        note = '' if comparable else \
            ' **key sets differ (corpus revision drift) — flips below are join-limited, not certified deltas**'
        lines += [f"- Previous: {prev['id']} ({prev.get('date')}, engine "
                  f"`{str(prev.get('engine_commit'))[:12]}`, "
                  f"{prev.get('broad', {}).get('passed')}/{prev.get('broad', {}).get('failed')} broad)"
                  + note,
                  f"- Keys: {join['previous_scenarios']} previous / {join['new_scenarios']} new / "
                  f"{join['common_keys']} common; only-previous {len(join['only_in_previous'])}, "
                  f"only-new {len(join['only_in_new'])}",
                  f"- **fail→pass: {len(join['fail_to_pass'])} · pass→fail: "
                  f"{len(join['pass_to_fail'])}**", '']
        for direction, key in (('fail→pass', 'fail_to_pass'), ('pass→fail', 'pass_to_fail')):
            flips = join[key]
            lines.append(f"### {direction} ({len(flips)})")
            if not flips:
                lines.append('')
                continue
            hints = attribution.get('receipt_hints') or {}
            for flip in flips[:400]:
                name = flip['file']
                tag = f" [{', '.join(flip['tags'][:3])}]" if flip['tags'] else ''
                docs = hints.get(name)
                owned = f" — receipts: {', '.join(docs[:4])}" if docs else ''
                lines.append(f"- `{name}` ({flip['scenario']}){tag}{owned}")
            if len(flips) > 400:
                lines.append(f"- … and {len(flips) - 400} more (see analysis json)")
            lines.append('')
        commits = attribution.get('commits')
        if commits is None:
            lines.append('- Attribution commit range unavailable (a run lacks engine_commit);'
                         ' receipt hints above are filename-based only.')
        elif commits:
            lines.append(f"- Engine commits between the runs ({len(commits)}, lib/pal/bin):")
            lines += [f"  - {c}" for c in commits]
        else:
            lines.append('- No engine commits between the runs (same engine_commit).')
        lines.append('')
    register = load_register()['runs']
    lines += ['Register totals comparison:', '', '| run | date | broad | edition |', '|---|---|---|---|']
    for entry in register:
        b, e = entry.get('broad', {}), entry.get('edition_selected', {})
        lines.append(f"| {entry['id']} | {entry.get('date')} | "
                     f"{b.get('passed')}/{b.get('failed')} | "
                     f"{e.get('passed')}/{e.get('failed')} |")
    lines.append('')
    lines += ['## 6. Artifacts', '',
              f'- Results: `{results_dir}/results.json`, `summary.json`, `controls.json`',
              f'- Analysis: `{results_dir}/refresh-analysis.json` / `.txt`',
              f'- This report: `{results_dir}/refresh-report.md`', '',
              'Raw `out/` artifacts are untracked; commit the numbers (not the 100 MB-class',
              'results.json) into the lane receipt. Full per-feature table: the analysis json.']
    return '\n'.join(lines) + '\n'


def register_entry_for(summary, report, results_dir):
    return {'id': f"refresh-{datetime.now(timezone.utc).strftime('%Y-%m-%d')}",
            'date': datetime.now(timezone.utc).strftime('%Y-%m-%d'),
            'engine_commit': summary.get('engine_commit'),
            'binary_sha256': summary['binary_sha256'],
            'engine_dylib_sha256': summary.get('engine_dylib_sha256'),
            'results_dir': str(Path(results_dir).relative_to(ROOT)),
            'results_retained': True,
            'broad': {k: report['broad'][k] for k in ('passed', 'failed', 'scenarios')},
            'edition_selected': {k: report['edition_selected'][k]
                                 for k in ('passed', 'failed', 'scenarios')},
            'evidence': 'doc/e12-testing-quality.md'}


def self_test():
    def rec(file, passed, scenario='default'):
        return {'file': file, 'scenario': scenario,
                'attrs': {'features': ['x'] if passed else ['y']},
                'result': {'pass': passed, 'message': None if passed else 'boom'}}
    old = [rec('a.js', False), rec('b.js', True), rec('c.js', False)]
    new = [rec('a.js', True), rec('b.js', True), rec('c.js', False)]
    join = join_runs(old, new)
    assert join['common_keys'] == 3 and not join['only_in_new'] and not join['only_in_previous'], join
    assert [f['file'] for f in join['fail_to_pass']] == ['a.js'], join
    assert join['pass_to_fail'] == [], join
    drift = join_runs(old, new + [rec('d.js', True)])
    assert drift['only_in_new'] == [('d.js', 'default')], drift
    gone = join_runs(old + [rec('e.js', True)], new)
    assert gone['only_in_previous'] == [('e.js', 'default')], gone
    regressive = join_runs(old, [rec('a.js', False), rec('b.js', False), rec('c.js', False)])
    assert [f['file'] for f in regressive['pass_to_fail']] == ['b.js'], regressive
    try:
        join_runs(old, old + [dict(old[0])])
        raise AssertionError('join accepted a duplicate key')
    except ValueError:
        pass
    # attribution helpers
    hints = receipt_hints(['instn-star-err-not-found.js', 'zzz-no-such-file-xy.js'])
    assert 'instn-star-err-not-found.js' in hints and \
        any('c4-manifest' in d or 's12' in d for d in hints['instn-star-err-not-found.js']), hints
    assert 'zzz-no-such-file-xy.js' not in hints, hints
    assert commit_range(None, 'abc') is None and commit_range('abc', 'abc') == []
    # Range must precede the pathspec separator: a range after '--' is a
    # path, and git then returns the whole tree history (regression pin
    # for the bug observed while validating: 6,464 commits instead of 60).
    head = subprocess.check_output(['git', '-C', str(ROOT), 'rev-parse', 'HEAD'],
                                   text=True).strip()
    real = commit_range('ba796f74f', head)
    assert real is not None and 0 < len(real) < 500, f'engine commit range looks wrong: {len(real) if real else None}'
    assert all(c.split()[1] not in ('Initial',) for c in real[:3]), real[:3]
    print('refresh_baseline.py self-test: OK')
    return 0


def main():
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('--binary', type=Path, help='absolute ch path (certified run)')
    parser.add_argument('--output', type=Path,
                        default=ROOT / f"out/test262-refresh-{datetime.now():%Y%m%d}")
    parser.add_argument('--jobs', type=int, default=CERTIFIED_JOBS,
                        help=f'parallel scenarios (certified default {CERTIFIED_JOBS})')
    parser.add_argument('--node', default='node')
    parser.add_argument('--previous', type=Path,
                        help='previous run directory (results.json + summary.json)')
    parser.add_argument('--previous-commit', help='override the previous run engine commit')
    parser.add_argument('--skip-run', action='store_true',
                        help='refresh an existing run dir (--results) instead of running')
    parser.add_argument('--results', type=Path, help='the run dir to (re)analyze')
    parser.add_argument('--publish', action='store_true',
                        help='append this run to baseline-register.json (commits are manual)')
    parser.add_argument('--self-test', action='store_true')
    args = parser.parse_args()
    if args.self_test:
        return self_test()
    if args.jobs != CERTIFIED_JOBS or args.skip_run:
        print(f'[refresh] NOTE: non-certified configuration '
              f'(jobs={args.jobs}, skip_run={args.skip_run}); report will say so', flush=True)

    # 1. The certified corpus run (unless re-reporting an existing one).
    if not args.skip_run:
        if args.binary is None:
            parser.error('--binary is required (or pass --skip-run --results DIR)')
        binary = args.binary.resolve()
        if not binary.is_file():
            parser.error(f'--binary does not exist: {binary}')
        if args.results is not None:
            parser.error('--results belongs to --skip-run mode')
        args.output = args.output.resolve()
        args.output.mkdir(parents=True, exist_ok=True)
        run_corpus(binary, args.output, args.jobs, args.node)
        results_dir = args.output
    else:
        if args.results is None:
            parser.error('--skip-run requires --results DIR')
        results_dir = args.results.resolve()
    results_path = results_dir / 'results.json'
    summary_path = results_dir / 'summary.json'
    for path in (results_path, summary_path):
        if not path.is_file():
            raise ValueError(f'missing {path}')

    # 2. Analysis.
    records, summary, report = analyze_run(results_path, summary_path)
    if not args.skip_run:
        host_flags = summary['host_flags']
        if host_flags != CERTIFIED_HOST_FLAGS:
            raise ValueError(f'run recorded host flags {host_flags!r}, expected the '
                             f'certified {CERTIFIED_HOST_FLAGS!r}')
        if summary['jobs'] != CERTIFIED_JOBS or summary['timeout_ms'] != CERTIFIED_TIMEOUT_MS:
            raise ValueError(f'run recorded jobs={summary["jobs"]} '
                             f'timeout={summary["timeout_ms"]}, expected the certified '
                             f'{CERTIFIED_JOBS}/{CERTIFIED_TIMEOUT_MS}')
    (results_dir / 'refresh-analysis.json').write_text(
        json.dumps({'results': str(results_path), **report}, indent=2, sort_keys=True) + '\n')
    (results_dir / 'refresh-analysis.txt').write_text(analyze.render_text(report))

    # 3. Delta join vs the previous certified run.
    register = load_register()
    register_entry, previous_records, previous_commit = None, None, None
    if args.previous is not None:
        prev_dir = args.previous.resolve()
        prev_results = prev_dir / 'results.json'
        if not prev_results.is_file():
            raise ValueError(f'--previous has no results.json: {args.previous}')
        previous_records = json.loads(prev_results.read_text())
        prev_summary_path = prev_dir / 'summary.json'
        previous_commit = (json.loads(prev_summary_path.read_text())
                           .get('engine_commit') if prev_summary_path.is_file() else None)
        if args.previous_commit:
            previous_commit = args.previous_commit
        register_entry = {'id': prev_dir.name, 'date': '(explicit --previous)',
                          'engine_commit': previous_commit,
                          'broad': {'passed': '(join-only)', 'failed': '(join-only)'}}
    else:
        entry = previous_from_register(register)
        if entry is not None:
            previous_records = json.loads(
                (ROOT / entry['results_dir'] / 'results.json').read_text())
            previous_commit = entry.get('engine_commit')
            register_entry = entry
    join = None
    attribution = {}
    if previous_records is not None:
        join = join_runs(previous_records, records)
        attribution['commits'] = commit_range(previous_commit, summary.get('engine_commit'))
        attribution['receipt_hints'] = receipt_hints(
            [f['file'] for f in join['fail_to_pass'] + join['pass_to_fail']])

    # 4. Dated report.
    text = render_report(summary, report, join, attribution, register_entry, results_dir)
    (results_dir / 'refresh-report.md').write_text(text)
    print(text, end='')
    entry = register_entry_for(summary, report, results_dir)
    if args.publish:
        if not any(run['id'] == entry['id'] and run['engine_commit'] == entry['engine_commit']
                   for run in register['runs']):
            register['runs'].append(entry)
            REGISTER.write_text(json.dumps(register, indent=2) + '\n')
            print(f'[refresh] published register entry {entry["id"]} '
                  f'(commit the register file manually)', flush=True)
    else:
        print(f'[refresh] register entry NOT appended (pass --publish to add):\n'
              f'{json.dumps(entry, indent=2)}', flush=True)
    return 0


if __name__ == '__main__':
    try:
        sys.exit(main())
    except (ValueError, OSError, subprocess.SubprocessError) as error:
        print(f'ABORT: {error}', file=sys.stderr)
        sys.exit(2)
