"""Build a standalone recognition pack; using only the versioned asset manifest and base models."""
import argparse
import hashlib
import json
from pathlib import Path
import re
import shutil
import struct
import zipfile
import zlib

import cv2
import numpy as np

ROOT = Path(__file__).resolve().parents[1]
GEOMETRY = [(1.84, 1.20, .37)] + [(s, x, y) for s in (1.78, 1.90)
                                      for x in (1.16, 1.24) for y in (.33, .41)]
MODEL_FILES = {'templates.qz', 'foreground.qz', 'manifest.json', 'names.onnx',
               'ready.png', 'unselected-hud.png', 'unselected-panel.png'}


def normalize(v):
    v = np.asarray(v, np.float32).ravel().copy()
    v -= v.mean()
    return v / max(float(np.linalg.norm(v)), 1e-6)


def source_path(root, name):
    if not isinstance(name, str) or not name:
        raise ValueError('Asset path must be a nonempty string')
    path = (root / name).resolve()
    if not path.is_relative_to(root.resolve()) or not path.is_file():
        raise ValueError(f'Missing asset or path outside source directory: {name}')
    return path


def read_image(root, name, alpha=False):
    image = cv2.imdecode(np.fromfile(source_path(root, name), np.uint8), cv2.IMREAD_UNCHANGED)
    if image is not None and image.ndim == 2:
        image = cv2.cvtColor(image, cv2.COLOR_GRAY2BGR)
    if image is None or image.ndim != 3 or image.shape[2] not in (3, 4):
        raise ValueError(f'Expected RGB/RGBA image: {name}')
    if image.shape[2] == 4 and not alpha:
        weight = image[:, :, 3:4].astype(np.float32) / 255
        image = np.uint8(image[:, :, :3] * weight + 127 * (1 - weight))
    return image


def validate_source(root, source=None):
    if source is None:
        source = json.loads((root / 'manifest.json').read_text(encoding='utf8'))
    if source.get('source_format') != 1 or not re.fullmatch(r'[A-Za-z0-9._-]+', source.get('resource_version', '')):
        raise ValueError('Unsupported source format or invalid resource version')
    ids = set()
    for hero in source['heroes']:
        name = hero['id']
        if not re.fullmatch(r'[a-z0-9]+(?:-[a-z0-9]+)*', name) or name in ids:
            raise ValueError(f'Invalid or duplicate hero ID: {name}')
        ids.add(name)
        images = ([hero['hud']] if hero.get('hud') else []) + hero['portraits']
        if not images:
            raise ValueError(f'Hero has no portrait: {name}')
        for image in images:
            source_path(root, image)
        if 'portrait_masks' in hero:
            if len(hero['portrait_masks']) != len(hero['portraits']):
                raise ValueError('Portrait masks must correspond to portrait variants')
            for mask in hero['portrait_masks']:
                source_path(root, mask)
    if not ids or not any(h.get('hud') for h in source['heroes']):
        raise ValueError('At least one hero and HUD illustration required')
    seen = set()
    for perk in source['perks']:
        key = (perk['hero'], perk['tier'], perk['id'], perk['file'])
        if perk['hero'] not in ids or perk['tier'] not in ('minor', 'major') or not perk['id'].strip():
            raise ValueError(f'Invalid perk mapping: {key}')
        if key in seen or perk['kind'] not in ('scoreboard', 'official-glyph', 'client-glyph'):
            raise ValueError(f'Duplicate or unsupported perk asset: {key}')
        seen.add(key)
        source_path(root, perk['file'])
    for name in ('ready.png', 'unselected-hud.png', 'unselected-panel.png'):
        source_path(root, source['ui'][name])
    return source


def localized_names(source):
    """Optional display metadata; never alter matching IDs or template order."""
    heroes, perks = {}, {}
    for hero in source['heroes']:
        name = hero.get('name_zhCN')
        if isinstance(name, str) and name.strip():
            heroes[hero['id']] = name.strip()
    for perk in source['perks']:
        name = perk.get('name_zhCN')
        if isinstance(name, str) and name.strip():
            tier = perks.setdefault(perk['hero'], {}).setdefault(perk['tier'], {})
            if perk['id'] in tier and tier[perk['id']] != name.strip():
                raise ValueError(f'Conflicting Chinese perk name: {perk["id"]}')
            tier[perk['id']] = name.strip()
    return {'heroes': heroes, 'perks': perks}


def foreground_masks(images):
    """Keep opaque person pixels; aligned team variants expose background contamination.

    No specific team hue is assumed. An opaque singleton needs an explicit mask
    instead of silently allowing its entire background into recognition.
    """
    if len(images) > 1:
        if any(im.shape[:2] != images[0].shape[:2] for im in images):
            raise ValueError('Portrait variants must be aligned and equally sized')
        rgb = np.stack([im[:, :, :3].astype(np.int16) for im in images])
        difference = np.uint8(np.max(rgb.max(axis=0) - rgb.min(axis=0), axis=2) > 18)
        # One-pixel capture/antialiasing differences also outline the face itself.
        # Only broad regions connected to the image border constitute background.
        broad = cv2.morphologyEx(difference, cv2.MORPH_OPEN, np.ones((3, 3), np.uint8))
        _, regions = cv2.connectedComponents(broad)
        border = np.unique(np.concatenate([regions[0], regions[-1], regions[:, 0], regions[:, -1]]))
        background = np.uint8(np.isin(regions, border[border != 0]))
        background = cv2.dilate(background, np.ones((3, 3), np.uint8))
        stable = background == 0
    else:
        stable = np.ones(images[0].shape[:2], bool)
        if images[0].shape[2] != 4 or np.all(images[0][:, :, 3] == 255):
            raise ValueError('Opaque portrait needs aligned team variants or an explicit foreground mask')
    return [np.uint8(stable & ((im[:, :, 3] >= 250) if im.shape[2] == 4 else True))
            for im in images]


def portrait_vectors(image, foreground):
    gray = cv2.cvtColor(cv2.resize(image, (48, 48)), cv2.COLOR_BGR2GRAY)
    mask = np.uint8(cv2.resize(foreground.astype(np.float32), (48, 48)) > .99)
    mask = cv2.erode(mask, np.ones((3, 3), np.uint8))
    if mask[3:45, 3:45].sum() < 180:
        raise ValueError('Too few foreground pixels in portrait')
    return gray[3:45, 3:45].astype(np.float32).ravel(), mask[3:45, 3:45].astype(np.float32).ravel()


def hud_vectors(image, foreground):
    for scale, x, y in GEOMETRY:
        for mirror in (False, True):
            face = cv2.flip(image, 1) if mirror else image
            mask = cv2.flip(foreground, 1) if mirror else foreground
            size = max(64, round(48 * scale))
            face = cv2.resize(face, (size, size), interpolation=cv2.INTER_AREA)
            mask = cv2.resize(mask.astype(np.float32), (size, size), interpolation=cv2.INTER_AREA)
            left, top = max(0, round(64 - 48 * x)), max(0, round(48 * y))
            face = face[top:top + 48, left:left + 64]
            mask = np.uint8(mask[top:top + 48, left:left + 64] > .99)
            # Blur/Sobel support must remain inside the person, not its background edge.
            mask = cv2.erode(mask, np.ones((5, 5), np.uint8), borderType=cv2.BORDER_CONSTANT, borderValue=0)
            gray = cv2.GaussianBlur(cv2.cvtColor(face, cv2.COLOR_BGR2GRAY), (3, 3), 0)
            yield (cv2.magnitude(cv2.Sobel(gray, cv2.CV_32F, 1, 0),
                                cv2.Sobel(gray, cv2.CV_32F, 0, 1)).ravel(), mask.astype(np.float32).ravel())


def perk_vector(image):
    gray = cv2.cvtColor(image, cv2.COLOR_BGR2GRAY)
    contours = cv2.findContours(np.uint8(gray > 200), cv2.RETR_EXTERNAL, cv2.CHAIN_APPROX_SIMPLE)[0]
    if contours:
        x, y, w, h = cv2.boundingRect(max(contours, key=cv2.contourArea))
        if w > gray.shape[1] * .4 and h > gray.shape[0] * .4:
            gray = gray[y:y+h, x:x+w]
    return normalize(cv2.resize(gray, (32, 32))[6:26, 6:26])


def glyph_vectors(glyph):
    ink = cv2.cvtColor(glyph[:, :, :3], cv2.COLOR_BGR2GRAY).astype(np.float32)
    if glyph.shape[2] == 4:
        ink *= glyph[:, :, 3] / 255
    x, y, w, h = cv2.boundingRect(np.uint8(ink > 64))
    if not w or not h:
        raise ValueError('Official glyph has no visible white artwork')
    ink = ink[y:y+h, x:x+w]
    for size in (20, 22, 24, 26):
        canvas = np.zeros((40, 40), np.float32)
        cv2.circle(canvas, (20, 20), 16, 255, -1)
        width, height = round(size * w / max(w, h)), round(size * h / max(w, h))
        left, top = 20 - width // 2, 20 - height // 2
        canvas[top:top+height, left:left+width] *= 1 - cv2.resize(ink, (width, height), interpolation=cv2.INTER_AREA) / 255
        x, y, w2, h2 = cv2.boundingRect(np.uint8(canvas > 200))
        yield normalize(cv2.resize(canvas[y:y+h2, x:x+w2], (32, 32))[6:26, 6:26])


def client_glyph_vectors(glyph):
    """Keep the game texture's canvas, including intentional asymmetric padding.

    Tight-cropping and centering the artwork shifts icons such as Life Hack and
    Sanguine Siphon relative to the scoreboard disk. All variants use the same
    geometry policy; no perk-specific adjustment or acceptance threshold change.
    """
    if glyph.shape[0] != glyph.shape[1]:
        raise ValueError('Client glyph must retain its original square canvas')
    ink = cv2.cvtColor(glyph[:, :, :3], cv2.COLOR_BGR2GRAY).astype(np.float32)
    if glyph.shape[2] == 4:
        ink *= glyph[:, :, 3] / 255
    if not np.any(ink > 64) or np.all(ink > 200):
        raise ValueError('Client glyph has no usable white artwork')
    for size in (22, 24, 26, 28, 30):
        canvas = np.zeros((40, 40), np.float32)
        cv2.circle(canvas, (20, 20), 16, 255, -1)
        start = 20 - size // 2
        canvas[start:start+size, start:start+size] *= 1 - cv2.resize(ink, (size, size), interpolation=cv2.INTER_AREA) / 255
        x, y, w, h = cv2.boundingRect(np.uint8(canvas > 200))
        yield normalize(cv2.resize(canvas[y:y+h, x:x+w], (32, 32))[6:26, 6:26])


def pack(raw):
    return struct.pack('>I', len(raw)) + zlib.compress(raw, 9)


def build(source_dir, base, output):
    source = validate_source(source_dir)
    # Keep the validated numeric model and OCR baseline; hero/perk banks are rebuilt.
    raw = zlib.decompress((base / 'templates.qz').read_bytes()[4:])
    if raw[:8] != b'OWCSTM01':
        raise ValueError('Invalid numeric baseline')
    matrices, at = [], 8
    for _ in range(2):
        rows, cols, kind = struct.unpack_from('<III', raw, at)
        at += 12
        matrices.append(np.frombuffer(raw, np.int32 if kind else np.float32, rows * cols, at).reshape(rows, cols).copy())
        at += rows * cols * 4
    portraits, masks, names, hud, hud_masks, hud_ids, hud_names = [], [], [], [], [], [], []
    for hero in source['heroes']:
        paths = hero['portraits']
        images = [read_image(source_dir, path, alpha=True) for path in paths]
        explicit = hero.get('portrait_masks')
        if explicit is not None:
            if len(explicit) != len(images):
                raise ValueError('Portrait masks must correspond to portrait variants')
            person = [np.uint8(read_image(source_dir, path)[:, :, 0] > 127) for path in explicit]
            if any(mask.shape != im.shape[:2] for mask, im in zip(person, images)):
                raise ValueError('Portrait mask size mismatch')
        else:
            person = foreground_masks(images) if images and not hero.get('hud') else []
        if hero.get('hud'):
            illustration = read_image(source_dir, hero['hud'], alpha=True)
            silhouette = foreground_masks([illustration])[0]
            # Use the transparent illustration silhouette for aligned scoreboard
            # references as well; unlike pixel equality it preserves antialiased
            # facial detail in the separately captured team variants.
            if explicit is None:
                person = [np.uint8(cv2.resize(silhouette.astype(np.float32), (im.shape[1], im.shape[0])) > .99)
                          for im in images]
            paths = [hero['hud']] + paths
            person = [silhouette] + person
        for path, mask in zip(paths, person):
            vector, mask = portrait_vectors(read_image(source_dir, path), mask)
            portraits.append(vector); masks.append(mask); names.append(hero['id'])
        if hero.get('hud'):
            for vector, mask in hud_vectors(read_image(source_dir, hero['hud']), silhouette):
                hud.append(vector); hud_masks.append(mask); hud_ids.append(len(hud_names))
            hud_names.append(hero['id'])
    perks, perk_names = [], []
    for item in source['perks']:
        image = read_image(source_dir, item['file'], alpha=item['kind'] in ('official-glyph', 'client-glyph'))
        if item['kind'] == 'client-glyph':
            vectors = client_glyph_vectors(image)
        else:
            vectors = glyph_vectors(image) if item['kind'] == 'official-glyph' else [perk_vector(image)]
        for vector in vectors:
            perks.append(vector)
            perk_names.append({k: item[k] for k in ('hero', 'tier', 'id')})
    matrices += [np.asarray(portraits, np.float32), np.asarray(hud, np.float32),
                 np.asarray(hud_ids, np.int32).reshape(-1, 1), np.asarray(perks, np.float32).reshape(-1, 400)]
    binary = b'OWCSTM01'
    for matrix in matrices:
        if max(matrix.shape, default=0) > 10000:
            raise ValueError('Asset bank exceeds native loader dimensions')
        binary += struct.pack('<III', *matrix.shape, int(matrix.dtype == np.int32)) + matrix.tobytes()
    output.mkdir(parents=True, exist_ok=True)
    (output / 'templates.qz').write_bytes(pack(binary))
    mask_blob = b'OWCSFG01'
    for bank in (masks, hud_masks):
        bank = np.asarray(bank, np.float32)
        mask_blob += struct.pack('<II', *bank.shape) + bank.tobytes()
    (output / 'foreground.qz').write_bytes(pack(mask_blob))
    metadata = {'version': 1, 'asset_format': 3, 'resource_version': source['resource_version'],
                'portrait_names': names, 'hud_names': hud_names, 'perks': perk_names, 'sources': source['sources'],
                'names_zhCN': localized_names(source)}
    (output / 'manifest.json').write_text(json.dumps(metadata, ensure_ascii=False, indent=2), encoding='utf8')
    if (base / 'names.onnx').resolve() != (output / 'names.onnx').resolve():
        shutil.copy2(base / 'names.onnx', output / 'names.onnx')
    for name, relative in source['ui'].items():
        if name not in MODEL_FILES:
            raise ValueError(f'Unexpected UI asset: {name}')
        shutil.copy2(source_path(source_dir, relative), output / name)
    return metadata


def archive(models, destination):
    metadata = json.loads((models / 'manifest.json').read_text(encoding='utf8'))
    manifest = {'type': 'owcs-recognition-pack', 'asset_format': 3,
                'resource_version': metadata['resource_version'], 'minimum_plugin_version': '0.5.0',
                'files': [{'path': 'models/' + name, 'sha256': hashlib.sha256((models / name).read_bytes()).hexdigest()}
                          for name in sorted(MODEL_FILES)]}
    destination.parent.mkdir(parents=True, exist_ok=True)
    with zipfile.ZipFile(destination, 'w', zipfile.ZIP_DEFLATED) as zip:
        for name in sorted(MODEL_FILES): zip.write(models / name, 'models/' + name)
        zip.writestr('manifest.json', json.dumps(manifest, ensure_ascii=False, indent=2))
        zip.write(ROOT / 'packaging/Install-Resources.ps1', 'Install-Resources.ps1')
        zip.write(ROOT / 'docs/RESOURCE_PACKS.md', 'README.md')
        zip.write(ROOT / 'docs/CLIENT_PERKS.md', 'CLIENT_PERKS.md')
        zip.write(ROOT / 'data/THIRD_PARTY_NOTICES.md', 'THIRD_PARTY_NOTICES.md')
    destination.with_suffix('.sha256').write_text(hashlib.sha256(destination.read_bytes()).hexdigest() + '  ' + destination.name + '\n')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--source', type=Path, default=ROOT / 'assets/recognition')
    parser.add_argument('--base', type=Path, default=ROOT / 'data/models')
    parser.add_argument('--output', type=Path, default=ROOT / '.build/resource-pack/models')
    parser.add_argument('--zip', type=Path)
    args = parser.parse_args()
    metadata = build(args.source, args.base, args.output)
    if args.zip: archive(args.output, args.zip)
    print(json.dumps({'resource_version': metadata['resource_version'], 'portraits': len(metadata['portrait_names']),
                      'perks': len(metadata['perks']), 'output': str(args.output)}, ensure_ascii=False))


if __name__ == '__main__':
    main()
