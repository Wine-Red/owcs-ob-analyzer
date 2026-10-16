"""Exercise the production recognizer on resized/subpixel-shifted reviewed frames.

These are synthetic variants of six fixtures, not independent match samples.
Outputs separate incorrect values, abstentions and death-occluded hero identities.
Use --exe and --label to compare an immutable baseline binary with a new build.
"""
import argparse
from collections import Counter, defaultdict
import hashlib
import json
import os
from pathlib import Path
import subprocess
import time

import cv2
import numpy as np

ROOT = Path(__file__).resolve().parents[1]
STATS = ('eliminations', 'assists', 'deaths', 'damage', 'healing', 'mitigated')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--exe', type=Path, default=ROOT / '.build/Release/owcs-recognizer-test.exe')
    parser.add_argument('--models', type=Path, default=ROOT / 'data/models')
    parser.add_argument('--obs', type=Path, default=Path('E:/SteamLibrary/steamapps/common/OBS Studio'))
    parser.add_argument('--label', default='current')
    parser.add_argument('--report-only', action='store_true', help='Record a baseline without enforcing regression limits')
    args = parser.parse_args()
    out = ROOT / '.diagnostics/geometry-regression'
    inputs = out / 'inputs'
    inputs.mkdir(parents=True, exist_ok=True)
    expected = json.loads((ROOT / 'tests/data/scoreboard/expected.json').read_text())
    cases = []
    for name, labels in expected.items():
        source = ROOT / 'tests/data/scoreboard' / f'{name}.png'
        original = cv2.imdecode(np.fromfile(source, np.uint8), 1)
        variants = [('original', original)]
        for width in (1280, 1440, 1728, 1920, 2048, 2560):
            variants.append((f'width{width}', cv2.resize(original,
                (width, round(original.shape[0] * width / original.shape[1])), interpolation=cv2.INTER_AREA)))
        for dx, dy in ((.45, .45), (-.3, .7)):
            variants.append((f'shift{dx},{dy}', cv2.warpAffine(original,
                np.float32([[1, 0, dx], [0, 1, dy]]), (original.shape[1], original.shape[0]))))
        for variant, image in variants:
            file = f'frame-{len(cases):04d}.png'
            cv2.imwrite(str(inputs / file), image)
            cases.append({'file': file, 'fixture': name, 'variant': variant,
                          'source_sha256': hashlib.sha256(source.read_bytes()).hexdigest(), 'expected': labels})
    (out / 'cases.json').write_text(json.dumps(cases, indent=2), encoding='utf-8')
    env = dict(os.environ)
    env['PATH'] = str(args.obs / 'bin/64bit') + os.pathsep + env['PATH']
    started = time.perf_counter()
    result = subprocess.run([str(args.exe.resolve()), str(args.models.resolve()), str(inputs), '--sequence', '--settled'],
                            env=env, capture_output=True, text=True, encoding='utf-8', check=True)
    elapsed = time.perf_counter() - started
    (out / f'{args.label}.jsonl').write_text(result.stdout, encoding='utf-8')
    actual = {row['file']: row for row in map(json.loads, result.stdout.splitlines())}
    groups = defaultdict(Counter)
    failures = []
    update_failures = []
    upstream_failures = []
    published_correct = 0
    settled_frames = 0
    for case in cases:
        rows = actual[case['file']]['raw']
        published = actual[case['file']]['state']['players']
        settled = actual[case['file']].get('settled_observations') == 4
        settled_frames += int(settled)
        group = groups[case['variant']]
        group['frames'] += 1
        group['panels'] += int(len(rows) == 10)
        for i in range(10):
            row = rows[i] if len(rows) == 10 else {}
            target = case['expected']
            fields = {key: (target['stats'][i][j], row.get('stats', {}).get(key)) for j, key in enumerate(STATS)}
            fields['charge'] = (target['charge'][i], row.get('ultimate_charge'))
            fields['hero'] = (None if target['life'][i] == 'dead' else target['heroes'][i], row.get('hero'))
            fields['life'] = (target['life'][i], row.get('life_state'))
            for field, (want, got) in fields.items():
                kind = 'stats' if field in STATS else field
                status = 'correct' if got == want else 'miss' if got is None or got == 'unknown' else 'wrong'
                group[f'{kind}_{status}'] += 1
                if status != 'correct':
                    failures.append({'file': case['file'], 'fixture': case['fixture'], 'variant': case['variant'],
                                     'row': i, 'field': field, 'expected': want, 'actual': got, 'status': status})
                if not settled:
                    continue  # Older baseline binaries only support raw comparisons.
                player = published[i] if len(published) == 10 else {}
                board = player.get('scoreboard', {})
                final = (board.get('stats', {}).get(field) if field in STATS else
                         board.get('ultimate_charge') if field == 'charge' else
                         player.get('life_state') if field == 'life' else player.get('hero'))
                if final == want:
                    published_correct += 1
                else:
                    key = 'ultimate_charge' if field == 'charge' else field
                    item = {'file': case['file'], 'row': i, 'field': field, 'expected': want,
                            'raw': got, 'published': final, 'decision': board.get('fields', {}).get(key, {})}
                    (update_failures if got == want else upstream_failures).append(item)
    totals = Counter()
    for group in groups.values():
        totals.update(group)
    report = {'frames': len(cases), 'elapsed_seconds': elapsed, 'totals': dict(totals),
              'groups': dict(groups), 'failures': failures,
              'binary_sha256': hashlib.sha256(args.exe.read_bytes()).hexdigest(),
              'state_validation': {'mode': 'each_image_fresh_state_four_observations',
                                   'settled_frames': settled_frames,
                                   'published_correct': published_correct,
                                   'raw_correct_state_wrong': update_failures,
                                   'raw_and_state_incorrect': upstream_failures}}
    (out / f'{args.label}.json').write_text(json.dumps(report, indent=2), encoding='utf-8')
    summary = {k: v for k, v in report.items() if k not in ('groups', 'failures', 'state_validation')}
    summary['state_validation'] = {'settled_frames': settled_frames, 'published_correct': published_correct,
                                   'raw_correct_state_wrong': len(update_failures),
                                   'raw_and_state_incorrect': len(upstream_failures)}
    print(json.dumps(summary, indent=2))
    if not args.report_only:
        assert totals['panels'] == len(cases), 'Panel localization regression'
        unexpected = [f for f in failures if f['status'] == 'wrong']
        assert not unexpected, f'Incorrect value regression: {unexpected}'
        assert totals['stats_miss'] <= 10, 'Numeric recall regression'
        assert totals['charge_miss'] == 0, 'Ultimate recall regression'
        assert totals['hero_miss'] == 0, 'Hero recall regression'
        assert totals['life_miss'] == 0, 'Life recall regression'
        assert not update_failures, f'State rejected correct raw readings: {update_failures}'
        assert settled_frames == len(cases), 'Binary does not support independent settled-state validation'


if __name__ == '__main__':
    main()
