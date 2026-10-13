#!/usr/bin/env python3
#-------------------------------------------------------------------------------------------------------
# Copyright (c) ChakraCore Project Contributors. All rights reserved.
# Licensed under the MIT license. See LICENSE.txt file in the project root for full license information.
#-------------------------------------------------------------------------------------------------------
"""Analyze a recorded test262 results.json against the ES2025 edition policy.

Reads a results.json produced by run.py plus edition-policy.json and emits a
small JSON + text report: broad vs edition-selected totals, per-feature
edition counts, failure buckets, and a negative-phase audit heuristic.

Failure buckets are assigned by the first matching rule (failures only):
  1. post-ES2025            any feature tag classified post-ES2025-proposal
  2. host                   abnormal exit/timeout (harness message or
                            rawResult.exitStatus), or a URIError naming a
                            module path (ch host module loader failure)
  3. disabled-flag         any tag listed in the policy's
                            disabled_by_default_flags (tag-implicated, not
                            proven: an enabled-flag run is the proof)
  4. required-feature-missing  positive test whose error signals absence:
                            SyntaxError (valid syntax rejected), ReferenceError
                            "<name> is not defined", or a typeof/property
                            probe observing "undefined"
  5. engine-defect-on-existing  everything else (assertions, wrong error
                            types, wrong values, failing negatives)

Negative-phase audit (heuristic, passed negatives only). The upstream
validator checks the error *name*, not the phase. From rawResult we can only
observe: stdout (a parse/resolution error must not execute test code, so any
stdout is suspect), the stderr "thrown at <file>:" loader prefix, and the top
stack frame shape (this host reports parse errors as tab-prefixed
"\\tat code (f-...js:L:C)" — a runtime throw from a function literally named
``code`` frames as space-prefixed "   at code (...)" and does not match —
while runtime errors frame as "at Global code / Module code / eval code /
<function>"). Resolution evidence is the loader prefix *together with* an
empty JS stack; a loader prefix accompanied by a JS stack is classified by
its top frame instead, and an empty stack without the loader prefix counts
as no evidence. A passed negative is flagged suspect when its observable
shape contradicts the declared phase; for a declared runtime phase any
non-parse frame is accepted (runtime-frame, other-frame, loader-prefix and
empty-stack alike), matching run.py's control rule — only a parse-style
frame contradicts it. This cannot catch, e.g., a runtime
instantiation SyntaxError reported with a parse-style frame; see
doc/c1-triage.md.

Input validation is loud, never silent: empty/non-list results, a non-list
``features`` value, a non-boolean ``result.pass``, or a duplicate
(file, scenario) pair abort with a clear error instead of producing a
plausible-looking but wrong report.
"""
import argparse
from collections import Counter, defaultdict
import json
from pathlib import Path
import sys

HERE = Path(__file__).resolve().parent


def load_policy(path):
    policy = json.loads(Path(path).read_text())
    category = {}
    for tag, entry in policy['features'].items():
        category[tag] = entry['category']
    for entry in policy.get('host_optional', []):
        category[entry['name']] = 'host-optional'
    flag_tags = {}
    for flag, entry in policy.get('disabled_by_default_flags', {}).items():
        for tag in entry.get('features', []):
            flag_tags.setdefault(tag, flag)
    return policy, category, flag_tags


def tags_of(record):
    return list((record.get('attrs') or {}).get('features') or [])


def error_of(record):
    return (record.get('rawResult') or {}).get('error') or {}


def is_host_failure(record):
    raw = record.get('rawResult') or {}
    status = raw.get('exitStatus') or {}
    message = (record.get('result') or {}).get('message') or ''
    if raw.get('timeout') or message == 'Test timed out':
        return True
    if status.get('signal') or (status.get('code') not in (None, 0)):
        return True
    if message.startswith('Host exited abnormally'):
        return True
    error = error_of(record)
    if error.get('name') == 'URIError' and '.js' in (error.get('message') or ''):
        return True
    return False


def is_missing_signal(record):
    error = error_of(record)
    name, message = error.get('name') or '', error.get('message') or ''
    result_message = (record.get('result') or {}).get('message') or ''
    if name == 'SyntaxError':
        return True
    if name == 'ReferenceError' and 'is not defined' in message:
        return True
    if name == 'TypeError' and ("'undefined'" in message or 'of undefined' in message):
        return True
    if '«"undefined"», «"function"»' in result_message:
        return True
    return False


def failure_bucket(record, category, flag_tags):
    tags = tags_of(record)
    if any(category.get(t) == 'post-ES2025-proposal' for t in tags):
        return 'post-ES2025'
    if is_host_failure(record):
        return 'host'
    if any(t in flag_tags for t in tags):
        return 'disabled-flag'
    negative = (record.get('attrs') or {}).get('negative')
    if not negative and is_missing_signal(record):
        return 'required-feature-missing'
    return 'engine-defect-on-existing'


def frame_shape(record):
    """Classify the top stack frame / loader prefix of a recorded error."""
    raw = record.get('rawResult') or {}
    stack = error_of(record).get('stack') or []
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


def phase_suspect_reasons(record):
    """Observable contradictions between a passed negative and its phase."""
    phase = ((record.get('attrs') or {}).get('negative') or {}).get('phase')
    shape = frame_shape(record)
    stdout = (record.get('rawResult') or {}).get('stdout') or ''
    reasons = []
    if phase in ('parse', 'resolution') and stdout:
        reasons.append('stdout-despite-no-execution-phase')
    if phase == 'parse':
        if shape == 'runtime-frame' or shape == 'other-frame':
            reasons.append(f'runtime-style-frame:{shape}')
        elif shape == 'empty-stack':
            reasons.append('no-parse-frame:empty-stack')
    elif phase == 'resolution':
        if shape == 'runtime-frame' or shape == 'other-frame':
            reasons.append(f'runtime-style-frame:{shape}')
        elif shape == 'parse-frame':
            reasons.append('parse-style-frame-for-resolution')
        elif shape == 'empty-stack':
            reasons.append('no-loader-evidence:empty-stack')
    elif phase == 'runtime':
        if shape == 'parse-frame':
            reasons.append('parse-style-frame-for-runtime')
    return reasons


def validate_records(records):
    """Reject malformed input loudly instead of silently miscounting it."""
    if not isinstance(records, list):
        raise ValueError('results must be a JSON array of scenario records')
    if not records:
        raise ValueError('results contain no scenarios; refusing to emit a zero report')
    seen = Counter()
    for record in records:
        if not isinstance(record, dict) or 'file' not in record:
            raise ValueError(f'record is missing its file field: {record!r:.120}')
        if not isinstance((record.get('result') or {}).get('pass'), bool):
            raise ValueError(f"record for {record['file']} has a non-boolean result.pass; "
                             'refusing to guess its outcome')
        features = (record.get('attrs') or {}).get('features')
        if features is not None and (not isinstance(features, list)
                                     or not all(isinstance(f, str) for f in features)):
            raise ValueError(f"record for {record['file']} has non-list features "
                             f'{features!r:.80}; refusing to iterate it as tags')
        seen[(record['file'], record.get('scenario'))] += 1
    duplicates = {key: count for key, count in seen.items() if count > 1}
    if duplicates:
        first = next(iter(sorted(duplicates)))
        raise ValueError(f'results contain {len(duplicates)} duplicate (file, scenario) '
                         f'pairs (e.g. {first} x{duplicates[first]}); counts would be wrong')


def analyze(records, category, flag_tags, summary=None):
    validate_records(records)
    unknown = sorted({t for r in records for t in tags_of(r) if t not in category})

    def edition_selected(record):
        return all(category.get(t, 'required-ES2025') == 'required-ES2025'
                   for t in tags_of(record))

    def totals(selected):
        passed = sum(1 for r in selected if r['result']['pass'])
        return {
            'scenarios': len(selected),
            'passed': passed,
            'failed': len(selected) - passed,
            'unique_files': len({r['file'] for r in selected}),
        }

    edition = [r for r in records if edition_selected(r)]
    excluded = [r for r in records if not edition_selected(r)]
    excluded_counts = Counter()
    for record in excluded:
        cats = {category.get(t, 'required-ES2025') for t in tags_of(record)}
        if 'post-ES2025-proposal' in cats:
            excluded_counts['post-ES2025-proposal'] += 1
        if 'host-optional' in cats:
            excluded_counts['host-optional'] += 1

    per_feature = {}
    for record in records:
        for tag in tags_of(record) or ['untagged']:
            entry = per_feature.setdefault(tag, {
                'category': category.get(tag, 'required-ES2025 (untagged)' if tag == 'untagged' else 'UNKNOWN-treated-required'),
                'broad_passed': 0, 'broad_failed': 0,
                'edition_passed': 0, 'edition_failed': 0})
            key = 'passed' if record['result']['pass'] else 'failed'
            entry[f'broad_{key}'] += 1
            if edition_selected(record):
                entry[f'edition_{key}'] += 1

    buckets = Counter()
    bucket_messages = defaultdict(Counter)
    edition_messages = Counter()
    for record in records:
        if record['result']['pass']:
            continue
        bucket = failure_bucket(record, category, flag_tags)
        buckets[bucket] += 1
        message = ((record.get('result') or {}).get('message') or '')[:100]
        bucket_messages[bucket][message] += 1
        if edition_selected(record):
            edition_messages[message] += 1

    audit = {}
    for phase in ('parse', 'resolution', 'runtime'):
        passed_neg = [r for r in records if r['result']['pass']
                      and ((r.get('attrs') or {}).get('negative') or {}).get('phase') == phase]
        suspects, reasons = [], Counter()
        for record in passed_neg:
            found = phase_suspect_reasons(record)
            if found:
                reasons.update(found)
                suspects.append({'file': record['file'], 'scenario': record.get('scenario'),
                                 'reasons': found})
        audit[phase] = {
            'passed_negatives': len(passed_neg),
            'suspects': len(suspects),
            'suspect_reasons': dict(reasons.most_common()),
            'suspect_records': suspects,
        }
    audit['passed_negatives_total'] = sum(audit[p]['passed_negatives']
                                          for p in ('parse', 'resolution', 'runtime'))

    excluded_passed = sum(1 for r in excluded if r['result']['pass'])
    class_union = sum(1 for r in edition if not r['result']['pass']
                      and any(t.startswith('class') for t in tags_of(r)))
    derived = {
        'passed_negatives_total': audit['passed_negatives_total'],
        'excluded_passed': excluded_passed,
        'class_family_union_edition_failures': class_union,
        'class_family_union_rule': ('scenarios failing in the edition-selected set with at '
                                    'least one feature tag starting with "class"'),
    }

    report = {
        'records': len(records),
        'unknown_feature_tags_treated_required': unknown,
        'broad': totals(records),
        'edition_selected': totals(edition),
        'excluded_from_edition': {'scenarios': len(excluded),
                                  'passed': excluded_passed,
                                  'failed': len(excluded) - excluded_passed,
                                  **dict(excluded_counts)},
        'derived_aggregates': derived,
        'skipped': (None if summary is None else {
            'scenarios': summary.get('skipped_scenarios'),
            'reasons': summary.get('skip_reasons'),
            'note': 'host-filter skips recorded by run.py; absent from results.json by construction',
        }),
        'failure_buckets': dict(buckets.most_common()),
        'bucket_top_messages': {b: dict(c.most_common(8)) for b, c in bucket_messages.items()},
        'edition_top_failure_messages': dict(edition_messages.most_common(15)),
        'edition_top_message_tags': {
            message: dict(Counter(t for r in edition
                                  if not r['result']['pass']
                                  and ((r.get('result') or {}).get('message') or '')[:100] == message
                                  for t in (tags_of(r) or ['untagged'])).most_common(5))
            for message, _ in edition_messages.most_common(15)},
        'negative_phase_audit': audit,
        'per_feature': dict(sorted(per_feature.items())),
    }
    return report


def render_text(report):
    lines = []
    for label in ('broad', 'edition_selected'):
        t = report[label]
        rate = (100.0 * t['passed'] / t['scenarios']) if t['scenarios'] else 0.0
        lines.append(f"{label}: {t['passed']} passed / {t['failed']} failed / "
                     f"{t['scenarios']} scenarios / {t['unique_files']} files ({rate:.2f}% passed)")
    lines.append(f"excluded_from_edition: {report['excluded_from_edition']}")
    lines.append(f"skipped: {report['skipped']}")
    lines.append(f"unknown_feature_tags_treated_required: {report['unknown_feature_tags_treated_required']}")
    lines.append('failure_buckets:')
    for bucket, count in report['failure_buckets'].items():
        lines.append(f'  {bucket}: {count}')
    lines.append(f"derived_aggregates: {report['derived_aggregates']}")
    lines.append('negative_phase_audit (passed negatives / suspects):')
    for phase in ('parse', 'resolution', 'runtime'):
        entry = report['negative_phase_audit'][phase]
        lines.append(f"  {phase}: {entry['passed_negatives']} passed, {entry['suspects']} suspects "
                     f"{entry['suspect_reasons']}")
    lines.append(f"  passed_negatives_total: {report['negative_phase_audit']['passed_negatives_total']}")
    lines.append('edition_top_failure_messages:')
    for message, count in report['edition_top_failure_messages'].items():
        lines.append(f'  {count}  {message}')
    lines.append('per_feature (category | broad P/F | edition P/F):')
    for tag, e in report['per_feature'].items():
        lines.append(f"  {tag} | {e['category']} | {e['broad_passed']}/{e['broad_failed']} | "
                     f"{e['edition_passed']}/{e['edition_failed']}")
    return '\n'.join(lines) + '\n'


def rec(file, passed, features=None, negative=None, error=None, message=None,
        stdout='', stderr='', stack=None, code=0):
    attrs = {'features': features} if features else {}
    if negative:
        attrs['negative'] = negative
    result = {'pass': passed}
    if message:
        result['message'] = message
    err = None
    if error:
        err = {'name': error[0], 'message': error[1], 'stack': stack or []}
    return {'file': file, 'scenario': 'default', 'attrs': attrs, 'result': result,
            'rawResult': {'stdout': stdout, 'stderr': stderr, 'error': err,
                          'exitStatus': {'code': code, 'signal': None}}}


def self_test():
    category = {'Temporal': 'post-ES2025-proposal', 'BigInt': 'required-ES2025',
                'class': 'required-ES2025', 'IsHTMLDDA': 'host-optional'}
    flag_tags = {'BigInt': 'ESBigInt'}
    parse_stack = [{'source': '\tat code (f-1.js:2:3)'}]
    global_stack = [{'source': '   at Global code (f-1.js:2:3)'}]
    codefn_stack = [{'source': '   at code (f-1.js:2:3)'}]  # function named `code`
    records = [
        rec('a.js', True),                                                        # untagged pass
        rec('b.js', False, ['Temporal'], error=('ReferenceError', "'Temporal' is not defined")),  # post bucket
        rec('c.js', False, ['BigInt'], error=('ReferenceError', "'BigInt' is not defined")),      # disabled-flag
        rec('d.js', False, ['class'], error=('SyntaxError', 'Invalid character')),                # missing
        rec('e.js', False, ['class'], error=('Test262Error', 'wrong value')),                     # defect
        rec('f.js', False, message='Host exited abnormally: {}', code=7),                         # host
        rec('g.js', True, negative={'phase': 'parse', 'type': 'SyntaxError'},
            error=('SyntaxError', 'bad'), stack=parse_stack),                                     # clean parse pass
        rec('h.js', True, negative={'phase': 'parse', 'type': 'SyntaxError'},
            error=('SyntaxError', 'thrown at runtime'), stack=global_stack),                      # suspect
        rec('i.js', True, negative={'phase': 'parse', 'type': 'SyntaxError'},
            error=('SyntaxError', 'bad'), stack=parse_stack, stdout='EXECUTED'),                  # suspect
        rec('j.js', True, ['mystery-tag']),                                                       # unknown -> required
    ]
    r = analyze(records, category, flag_tags)
    assert r['broad'] == {'scenarios': 10, 'passed': 5, 'failed': 5, 'unique_files': 10}, r['broad']
    assert r['edition_selected']['scenarios'] == 9, r['edition_selected']
    assert r['excluded_from_edition']['scenarios'] == 1
    assert r['failure_buckets'] == {'post-ES2025': 1, 'disabled-flag': 1,
                                    'required-feature-missing': 1,
                                    'engine-defect-on-existing': 1, 'host': 1}, r['failure_buckets']
    assert r['negative_phase_audit']['parse']['passed_negatives'] == 3
    assert r['negative_phase_audit']['parse']['suspects'] == 2, r['negative_phase_audit']
    assert r['unknown_feature_tags_treated_required'] == ['mystery-tag']
    assert r['per_feature']['untagged']['edition_passed'] >= 1

    loader = 'thrown at /tmp/f-1.js:1:1\n'
    audit_records = [
        # parse declared, runtime throw from a function named `code` (F1): suspect
        rec('p1.js', True, negative={'phase': 'parse', 'type': 'SyntaxError'},
            error=('SyntaxError', 'runtime masquerade'), stack=codefn_stack),
        # resolution, genuine loader channel (prefix + empty stack): clean
        rec('r1.js', True, negative={'phase': 'resolution', 'type': 'SyntaxError'},
            error=('SyntaxError', 'Module import x cannot be resolved'), stderr=loader),
        # resolution, empty stack but no loader prefix: no evidence -> suspect
        rec('r2.js', True, negative={'phase': 'resolution', 'type': 'SyntaxError'},
            error=('SyntaxError', 'silent')),
        # resolution, loader prefix accompanied by a runtime frame: suspect
        rec('r3.js', True, negative={'phase': 'resolution', 'type': 'SyntaxError'},
            error=('SyntaxError', 'thrown while evaluating'), stderr=loader, stack=global_stack),
        # resolution, parse-style frame (parser rejected module syntax): suspect
        rec('r4.js', True, negative={'phase': 'resolution', 'type': 'SyntaxError'},
            error=('SyntaxError', "Expected ';'"), stack=parse_stack),
        # runtime, genuine runtime frame: clean
        rec('t1.js', True, negative={'phase': 'runtime', 'type': 'SyntaxError'},
            error=('SyntaxError', 'eval failed'), stack=global_stack),
        # runtime declared, actual parse error (F3 shape): suspect
        rec('t2.js', True, negative={'phase': 'runtime', 'type': 'SyntaxError'},
            error=('SyntaxError', 'bad syntax'), stack=parse_stack),
        # runtime throw from function `code`: other-frame, not parse -> clean
        rec('t3.js', True, negative={'phase': 'runtime', 'type': 'SyntaxError'},
            error=('SyntaxError', 'runtime throw'), stack=codefn_stack),
        # host bucket via ch module-loader URIError naming a module path
        rec('u1.js', False, error=('URIError', './missing_FIXTURE.js could not be loaded')),
    ]
    a = analyze(audit_records, category, flag_tags)['negative_phase_audit']
    assert a['parse']['passed_negatives'] == 1 and a['parse']['suspects'] == 1, a['parse']
    assert a['parse']['suspect_reasons'] == {'runtime-style-frame:other-frame': 1}, a['parse']
    assert a['resolution']['passed_negatives'] == 4 and a['resolution']['suspects'] == 3, a['resolution']
    assert a['resolution']['suspect_reasons'] == {
        'no-loader-evidence:empty-stack': 1, 'runtime-style-frame:runtime-frame': 1,
        'parse-style-frame-for-resolution': 1}, a['resolution']
    assert a['runtime']['passed_negatives'] == 3 and a['runtime']['suspects'] == 1, a['runtime']
    assert a['runtime']['suspect_reasons'] == {'parse-style-frame-for-runtime': 1}, a['runtime']
    assert analyze(audit_records, category, flag_tags)['failure_buckets'] == {'host': 1}

    agg_records = [
        rec('k1.js', False, ['class-fields-public'], error=('SyntaxError', 'bad')),
        rec('k2.js', False, ['class', 'class-methods-private'], error=('SyntaxError', 'bad')),
        rec('k3.js', True, ['Temporal']),  # excluded but passing
        rec('k4.js', False, ['Temporal'], error=('ReferenceError', "'Temporal' is not defined")),
    ] + [rec(f's{n}.js', True, negative={'phase': 'parse', 'type': 'SyntaxError'},
             error=('SyntaxError', 'masquerade'), stack=codefn_stack) for n in range(12)]
    g = analyze(agg_records, category, flag_tags)
    assert g['excluded_from_edition'] == {'scenarios': 2, 'passed': 1, 'failed': 1,
                                          'post-ES2025-proposal': 2}, g['excluded_from_edition']
    assert g['derived_aggregates']['passed_negatives_total'] == 12, g['derived_aggregates']
    assert g['derived_aggregates']['excluded_passed'] == 1, g['derived_aggregates']
    assert g['derived_aggregates']['class_family_union_edition_failures'] == 2, g['derived_aggregates']
    assert g['negative_phase_audit']['passed_negatives_total'] == 12
    parse_audit = g['negative_phase_audit']['parse']
    assert parse_audit['suspects'] == 12 and len(parse_audit['suspect_records']) == 12, parse_audit

    def must_raise(bad, why):
        try:
            analyze(bad, category, flag_tags)
        except ValueError:
            return
        raise AssertionError(f'analyze accepted {why}')

    must_raise([], 'empty results')
    must_raise({}, 'non-list results')
    must_raise([rec('x.js', True, features='BigInt')], 'non-list features (string)')
    must_raise([rec('x.js', 'yes')], 'non-boolean result.pass')
    must_raise([rec('x.js', True), rec('x.js', False)], 'duplicate (file, scenario)')
    print('analyze.py self-test: OK')
    return 0


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--results', type=Path, help='results.json from run.py')
    parser.add_argument('--policy', type=Path, default=HERE / 'edition-policy.json')
    parser.add_argument('--summary', type=Path, help='optional summary.json (skip counts)')
    parser.add_argument('--out-json', type=Path)
    parser.add_argument('--out-text', type=Path)
    parser.add_argument('--self-test', action='store_true')
    args = parser.parse_args()
    if args.self_test:
        return self_test()
    if not args.results:
        parser.error('--results is required unless --self-test')
    _, category, flag_tags = load_policy(args.policy)
    try:
        records = json.loads(args.results.read_text())
        summary = json.loads(args.summary.read_text()) if args.summary else None
        report = analyze(records, category, flag_tags, summary)
    except ValueError as error:
        print(f'ABORT: {error}', file=sys.stderr)
        return 2
    report = {'results': str(args.results), 'policy': str(args.policy), **report}
    text = render_text(report)
    if args.out_json:
        args.out_json.write_text(json.dumps(report, indent=2, sort_keys=True) + '\n')
    if args.out_text:
        args.out_text.write_text(text)
    print(text, end='')
    return 0


if __name__ == '__main__':
    sys.exit(main())
