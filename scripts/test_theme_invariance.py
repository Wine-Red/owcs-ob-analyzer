"""Synthetic background changes through the native recognizer, not a matcher replica.

Preserves reference artwork. This tests palette robustness, not unseen screenshot accuracy.
"""
import argparse
import json
import os
from pathlib import Path
import subprocess
import cv2
import numpy as np

ROOT = Path(__file__).resolve().parents[1]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--models', type=Path, default=ROOT / 'data/models')
    parser.add_argument('--obs', type=Path, default=Path('E:/SteamLibrary/steamapps/common/OBS Studio'))
    args = parser.parse_args()
    out = ROOT / '.diagnostics/theme-regression'
    out.mkdir(exist_ok=True)
    source = ROOT / 'assets/recognition'
    manifest = json.loads((source / 'manifest.json').read_text(encoding='utf-8'))
    baseline = cv2.imread(str(ROOT / 'tests/data/scoreboard/ready.png'))
    baseline = cv2.resize(baseline, (1600, round(baseline.shape[0] * 1600 / baseline.shape[1])))
    # Native normalized coordinates of the supplied ready.png fixture.
    left, top, right, scale = 458, 181, 1141, 1
    palettes = [(32, 32, 32), (230, 230, 230), (30, 200, 230),
                (150, 60, 170), (60, 190, 40), (180, 180, 180)] # BGR
    annotations = []
    heroes = [h for h in manifest['heroes'] if len(h['portraits']) == 2]
    for offset in range(0, len(heroes), 10):
        group = [heroes[(offset + i) % len(heroes)] for i in range(10)]
        for color in palettes:
            frame = baseline.copy()
            for row, hero in enumerate(group):
                pair = [cv2.imread(str(source / p)) for p in hero['portraits']]
                # Broad changing regions touching a border, obtained independently
                # of the runtime HUD-alpha masks. Do not replace thin facial edges.
                diff = np.uint8(np.max(np.abs(pair[0].astype(np.int16)-pair[1]), axis=2) > 24)
                broad = cv2.morphologyEx(diff, cv2.MORPH_OPEN, np.ones((3, 3), np.uint8))
                _, labels = cv2.connectedComponents(broad)
                border = np.unique(np.concatenate([labels[0], labels[-1], labels[:, 0], labels[:, -1]]))
                background = np.isin(labels, border[border != 0])
                portrait = pair[row // 5].copy()
                portrait[background] = color
                cy = top + 54 + 55 * (row % 5) + (339 if row >= 5 else 0)
                frame[cy-27:cy+27, left+23:left+77] = cv2.resize(portrait, (54, 54))
            name = f'frame-{len(annotations):04d}.png'
            cv2.imwrite(str(out / name), frame)
            annotations.append({'file': name, 'palette_bgr': color, 'heroes': [h['id'] for h in group]})
    (out / 'expected.json').write_text(json.dumps(annotations, indent=2))
    env = dict(os.environ)
    env['PATH'] = str(args.obs / 'bin/64bit') + os.pathsep + env['PATH']
    result = subprocess.run([str(ROOT / '.build/Release/owcs-recognizer-test.exe'), str(args.models), str(out), '--sequence'],
                            env=env, capture_output=True, text=True, encoding='utf-8', check=True)
    (out / 'actual.jsonl').write_text(result.stdout, encoding='utf-8')
    actual = {r['file']: r for r in map(json.loads, result.stdout.splitlines())}
    failures = []
    for e in annotations:
        rows = actual[e['file']]['raw']
        for i, hero in enumerate(e['heroes']):
            if len(rows) != 10 or rows[i]['hero'] != hero:
                failures.append({'file': e['file'], 'row': i, 'expected': hero,
                                 'actual': rows[i]['hero'] if len(rows) == 10 else 'panel missing',
                                 'life': rows[i]['life_state'] if len(rows) == 10 else None})
    report = {'frames': len(annotations), 'hero_checks': len(annotations) * 10, 'failures': failures}
    (out / 'report.json').write_text(json.dumps(report, indent=2))
    print(json.dumps(report, indent=2))
    raise SystemExit(bool(failures))


if __name__ == '__main__':
    main()
