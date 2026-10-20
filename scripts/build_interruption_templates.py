"""Extract UI evidence from user-provided 2026-10-19 screenshots (no generated art)."""
import hashlib
import json
from pathlib import Path

import cv2
import numpy as np

ROOT = Path(__file__).resolve().parents[1]
FIXTURES = ROOT / 'tests/data/interruptions'
OUT = ROOT / 'data/interruptions'
OUT.mkdir(parents=True, exist_ok=True)


def read(name):
    image = cv2.imdecode(np.frombuffer((FIXTURES / name).read_bytes(), np.uint8), 1)
    return cv2.resize(image, (1600, round(image.shape[0] * 1600 / image.shape[1])))


def save(name, image):
    (OUT / name).write_bytes(cv2.imencode('.png', image)[1].tobytes())


disconnected = read('disconnected.png')
# Production Panel coordinates: left=459, top=181, scale=1, last row cy=794.
save('empty-slot-portrait.png', cv2.cvtColor(disconnected[767:821, 482:536], cv2.COLOR_BGR2GRAY))
save('empty-slot-name.png', cv2.cvtColor(disconnected[777:805, 578:726], cv2.COLOR_BGR2GRAY))
paused = read('paused.png')
hsv = cv2.cvtColor(paused, cv2.COLOR_BGR2HSV)
orange = cv2.inRange(hsv, (8, 140, 130), (30, 255, 255))
# Bound only the Chinese pause banner; exclude charge rings and HUD.
region = orange[435:530, 620:980]
x, y, w, h = cv2.boundingRect(region)
save('pause-zhCN.png', region[y:y+h, x:x+w])
manifest = {
    'source': 'User-provided Overwatch screenshots, 2026-10-19',
    'coordinate_width': 1600,
    'pause_language': 'zh-CN',
    'files': {p.name: hashlib.sha256(p.read_bytes()).hexdigest()
              for p in sorted(FIXTURES.glob('*.png'))},
    'templates': {name: hashlib.sha256((OUT / name).read_bytes()).hexdigest()
                  for name in ['empty-slot-portrait.png', 'empty-slot-name.png', 'pause-zhCN.png']},
}
(FIXTURES / 'provenance.json').write_text(json.dumps(manifest, indent=2) + '\n', encoding='utf-8')
print(json.dumps(manifest, indent=2))
