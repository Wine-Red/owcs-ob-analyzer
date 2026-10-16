"""Audit normal/death appearance competition through the production classifier.

Clean inputs are reference portraits with rescaling and one-pixel translations.
Death inputs transplant circular badge regions from real labeled frames onto
those portraits. They test interference, NOT independent real-match accuracy.
"""
import argparse
from collections import Counter
import json
import os
from pathlib import Path
import subprocess

import cv2
import numpy as np

ROOT = Path(__file__).resolve().parents[1]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--exe', type=Path, default=ROOT / '.build/Release/owcs-recognizer-test.exe')
    parser.add_argument('--label', default='current')
    parser.add_argument('--obs', type=Path, default=Path('E:/SteamLibrary/steamapps/common/OBS Studio'))
    parser.add_argument('--report-only', action='store_true')
    args = parser.parse_args()
    out = ROOT / '.diagnostics/life-classifier'
    out.mkdir(exist_ok=True)
    env = dict(os.environ)
    env['PATH'] = str(ROOT / '.build/Release') + os.pathsep + str(args.obs / 'bin/64bit') + os.pathsep + env['PATH']

    def run(*arguments):
        return subprocess.run([str(args.exe), str(ROOT / 'data/models'), *map(str, arguments)],
                              env=env, check=True, capture_output=True, encoding='utf8').stdout

    # Independently labeled, held-out real frames remain in native lifeRegression.
    badges = []
    for name, row in [('death-cross', 5), ('countdown', 5), ('heldout-nine', 1)]:
        folder = out / name
        run(ROOT / 'tests/data/life' / (name + '.png'), '--inspect', folder)
        trace = json.loads((folder / 'crops.json').read_text(encoding='utf8'))
        crop = next(c for c in trace['crops'] if c['row'] == row and c['field'] == 'portrait')
        badge = cv2.imread(str(folder / crop['file']))
        badges.append((name, cv2.resize(badge, (54, 54))))

    sources = ROOT / 'assets/recognition'
    manifest = json.loads((sources / 'manifest.json').read_text(encoding='utf8'))
    annotations = []
    yy, xx = np.mgrid[:54, :54]
    # Copy the badge interior; preserve the destination hero outside it.
    mask = (xx - 27) ** 2 + (yy - 27) ** 2 <= 19 ** 2

    def save(image, name, hero, expected, group):
        cv2.imwrite(str(out / name), image)
        annotations.append({'file': name, 'kind': 'face', 'expected_hero': hero,
                            'expected_life': expected, 'group': group})

    for hero in manifest['heroes']:
        for variant, relative in enumerate(hero['portraits']):
            portrait = cv2.imdecode(np.fromfile(sources / relative, np.uint8), cv2.IMREAD_COLOR)
            for size in (32, 54):
                small = cv2.resize(portrait, (size, size), interpolation=cv2.INTER_AREA)
                for dx in (-1, 0, 1):
                    for dy in (-1, 0, 1):
                        shifted = cv2.warpAffine(small, np.float32([[1, 0, dx], [0, 1, dy]]),
                                                (size, size), borderMode=cv2.BORDER_REPLICATE)
                        name = f'clean-{hero["id"]}-{variant}-{size}-{dx+1}-{dy+1}.png'
                        save(shifted, name, hero['id'], 'alive', 'clean')
                for kind, badge in badges:
                    composite = cv2.resize(portrait, (54, 54))
                    composite[mask] = badge[mask]
                    composite = cv2.resize(composite, (size, size), interpolation=cv2.INTER_AREA)
                    name = f'{kind}-{hero["id"]}-{variant}-{size}.png'
                    save(composite, name, hero['id'], 'dead', kind)
    (out / 'patches.json').write_text(json.dumps(annotations), encoding='utf8')
    actual = run(out, '--patches')
    (out / f'{args.label}.jsonl').write_text(actual, encoding='utf8')
    readings = {v['file']: v['result'] for v in map(json.loads, actual.splitlines())}
    groups, failures = {}, []
    for item in annotations:
        value = readings[item['file']]
        group = groups.setdefault(item['group'], Counter())
        group['checks'] += 1
        group['correct' if value['life'] == item['expected_life'] else value['life']] += 1
        if value['life'] != item['expected_life']:
            failures.append({**item, **value})
    report = {'input_kind': 'synthetic reference transformations and real-badge composites',
              'heroes': len(manifest['heroes']), 'groups': groups, 'failures': failures}
    (out / f'{args.label}.json').write_text(json.dumps(report, indent=2), encoding='utf8')
    print(json.dumps({'heroes': report['heroes'], 'groups': groups, 'failures': len(failures)}, indent=2))
    if failures and not args.report_only:
        raise SystemExit(1)


if __name__ == '__main__':
    main()
