"""Run reviewed screenshot crops through the native recognizer and report coverage.

This is not a synthetic glyph self-match test. The supplied patches.json must
contain manually reviewed labels and real screenshot crops. Related resize
variants are reported separately, not presented as independent game samples.
"""
import argparse
from collections import Counter
import json
import os
from pathlib import Path
import subprocess

from extract_client_perks import ROOT, read_json, sha256, write_json


def evaluate(labels, actual):
    counts = Counter()
    failures = []
    covered = set()
    for item in labels:
        if item['kind'] != 'perk':
            continue
        key = item['file']
        if key not in actual:
            raise ValueError(f'Missing native result: {key}')
        result = actual[key]
        expected = item['expected']
        selection = item['selection']
        positive = selection == 'selected'
        if positive != (expected is not None):
            raise ValueError(f'Inconsistent reviewed label: {key}')
        group = 'positive' if positive else 'negative'
        counts[group] += 1
        correct = result.get('id') == expected and result.get('selection') == selection
        counts[group + '_correct'] += int(correct)
        if positive:
            covered.add((item['hero'], item['tier'], expected))
        if not correct:
            failures.append({'file': key, 'hero': item['hero'], 'tier': item['tier'],
                             'expected': expected, 'selection': selection, 'actual': result})
    if not counts['positive'] or not counts['negative']:
        raise ValueError('Regression must include both visible perks and unselected slots')
    return {'counts': dict(counts), 'covered_perks': sorted(covered), 'failures': failures}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--models', type=Path, required=True)
    parser.add_argument('--patches', type=Path, required=True)
    parser.add_argument('--exe', type=Path, default=ROOT / '.build/Release/owcs-recognizer-test.exe')
    parser.add_argument('--obs', type=Path, default=Path('E:/SteamLibrary/steamapps/common/OBS Studio'))
    parser.add_argument('--report', type=Path, required=True)
    args = parser.parse_args()
    labels = read_json(args.patches / 'patches.json')
    env = dict(os.environ)
    env['PATH'] = str(args.obs / 'bin/64bit') + os.pathsep + env['PATH']
    output = subprocess.run([str(args.exe.resolve()), str(args.models.resolve()),
                             str(args.patches.resolve()), '--patches'],
                            env=env, capture_output=True, text=True, encoding='utf-8', check=True)
    rows = [json.loads(line) for line in output.stdout.splitlines()]
    actual = {r['file']: r['result'] for r in rows}
    if len(actual) != len(rows):
        raise ValueError('Duplicate native results')
    report = evaluate(labels, actual)
    report['resource_version'] = read_json(args.models / 'manifest.json')['resource_version']
    report['sha256'] = {'executable': sha256(args.exe), 'templates': sha256(args.models / 'templates.qz'),
                        'manifest': sha256(args.models / 'manifest.json'),
                        'labels': sha256(args.patches / 'patches.json')}
    report['patch_sha256'] = {p['file']: sha256(args.patches / p['file']) for p in labels if p['kind'] == 'perk'}
    report['variants'] = dict(Counter(p.get('variant', 'unspecified') for p in labels if p['kind'] == 'perk'))
    report['limitations'] = ['Only the listed historical screenshot crops were evaluated.',
                            'Resize variants are correlated; this does not validate all client perks.',
                            'Full-frame localization and upstream hero identification are tested separately.']
    args.report.parent.mkdir(parents=True, exist_ok=True)
    write_json(args.report, report)
    args.report.with_suffix('.jsonl').write_text(output.stdout, encoding='utf-8')
    print(json.dumps({'counts': report['counts'], 'distinct_perks': len(report['covered_perks']),
                      'failures': len(report['failures']), 'report': str(args.report)}, indent=2))
    if report['failures']:
        raise SystemExit(1)


if __name__ == '__main__':
    main()
