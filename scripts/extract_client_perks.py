"""Stage client-derived perk assets with OWLib DataTool; never install them into OBS.

DataTool must be downloaded separately from overtools/OWLib. Close Overwatch and
Battle.net before extraction. --preflight only checks local inputs and file locks.
"""
import argparse
from collections import Counter, defaultdict
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import unicodedata

from build_resource_pack import ROOT, client_glyph_vectors, read_image, validate_source, source_path


def sha256(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def read_json(path):
    return json.loads(path.read_text(encoding='utf-8-sig'))


def write_json(path, value):
    path.write_text(json.dumps(value, ensure_ascii=False, indent=2) + '\n', encoding='utf-8')


def build_info(client):
    lines = (client / '.build.info').read_text(encoding='utf-8-sig').splitlines()
    columns = [v.split('!')[0] for v in lines[0].split('|')]
    rows = [dict(zip(columns, line.split('|'))) for line in lines[1:] if line.strip()]
    active = [r for r in rows if r.get('Active') == '1' and r.get('Product') == 'pro']
    if len(active) != 1:
        raise ValueError('Expected exactly one active Overwatch build in .build.info')
    return {k: active[0][k] for k in ('Version', 'Build Key', 'CDN Key', 'Branch', 'Product')}


def check_index_locks(client):
    """Match DataTool's read sharing mode without reading or changing game data."""
    if os.name != 'nt':
        return []
    import ctypes
    from ctypes import wintypes
    kernel = ctypes.WinDLL('kernel32', use_last_error=True)
    kernel.CreateFileW.argtypes = [wintypes.LPCWSTR, wintypes.DWORD, wintypes.DWORD,
                                  wintypes.LPVOID, wintypes.DWORD, wintypes.DWORD, wintypes.HANDLE]
    kernel.CreateFileW.restype = wintypes.HANDLE
    kernel.CloseHandle.argtypes = [wintypes.HANDLE]
    failures = []
    indices = list((client / 'data/casc/data').glob('*.idx'))
    if not indices:
        raise ValueError('No local CASC indices found')
    for path in indices:
        handle = kernel.CreateFileW(str(path), 0x80000000, 1, None, 3, 0, None)
        if handle == ctypes.c_void_p(-1).value:
            failures.append({'file': path.name, 'winerror': ctypes.get_last_error()})
        else:
            kernel.CloseHandle(handle)
    return failures


def canonical_name(value):
    value = unicodedata.normalize('NFKD', value or '').encode('ascii', 'ignore').decode()
    return re.sub('[^a-z0-9]', '', value.lower())


def guid(value):
    if not isinstance(value, str) or not re.fullmatch(r'[0-9A-Fa-f]{12}\.[0-9A-Fa-f]{3}', value):
        raise ValueError(f'Invalid or missing DataTool GUID: {value!r}')
    return value.upper()


def plan_perks(heroes, talents, known_heroes, localized=None, current=None):
    """Join the current hero perk list to levels by loadout GUID, never by filename.

    list-talents includes retired entries, and Talent.GUID can be null in OWLib.
    Its dictionary key is the provenance GUID; Loadout.HeroName is not reliable
    for perks either. Ownership comes exclusively from Hero.Perks.
    """
    by_loadout = defaultdict(list)
    for key, talent in talents.items():
        if talent.get('TalentType') == 'Perk' and (talent.get('Loadout') or {}).get('GUID'):
            by_loadout[guid(talent['Loadout']['GUID'])].append((guid(key), talent))
    known = {canonical_name(h): h for h in known_heroes}
    if len(known) != len(known_heroes):
        raise ValueError('Ambiguous project hero names')
    planned, ignored, seen = [], [], set()
    for hero_guid, hero in heroes.items():
        if not hero.get('IsHero'):
            continue
        hero_id = known.get(canonical_name(hero.get('Name')))
        if hero_id is None:
            ignored.append({'guid': hero_guid, 'name': hero.get('Name')})
            continue
        if hero_id in seen:
            raise ValueError(f'Duplicate client hero: {hero_id}')
        seen.add(hero_id)
        tiers = Counter()
        loadouts = set()
        active = None
        if current is not None:
            active = {guid(p['LoadoutGUID']): p for p in current.get(hero_guid, {}).get('Perks', [])}
            if len(active) != len(hero.get('Perks') or []):
                raise ValueError(f'Current perk reference count mismatch: {hero_id}')
        for perk in hero.get('Perks') or []:
            loadout = guid(perk.get('GUID'))
            if loadout in loadouts:
                raise ValueError(f'Duplicate active loadout: {loadout}')
            loadouts.add(loadout)
            candidates = by_loadout.get(loadout, [])
            if active is not None:
                reference = active.get(loadout)
                if reference is None:
                    raise ValueError(f'Loadout is not currently active: {hero_id}/{loadout}')
                candidates = [(key, t) for key, t in candidates if key == guid(reference['PerkGUID'])]
                if not candidates or any(t['Major'] != reference['Major'] or t['Level'] != reference['Level']
                                         for _, t in candidates):
                    raise ValueError(f'Current perk reference disagrees with talent: {loadout}')
            if not candidates or any(type(t.get('Major')) is not bool for _, t in candidates):
                raise ValueError(f'Missing explicit tier for {hero_id}/{loadout}')
            classifications = {(t['Major'], t.get('Level')) for _, t in candidates}
            if len(classifications) != 1:
                raise ValueError(f'Conflicting tier/level for {hero_id}/{loadout}')
            major, level = next(iter(classifications))
            tier = 'major' if major else 'minor'
            # Preserve the raw level value. Its zero/one-based convention is not
            # established until a real client export is inspected; Major is explicit.
            if type(level) is not int or level < 0:
                raise ValueError(f'Unexpected perk level: {hero_id}/{tier}/{level}')
            name = perk.get('Name')
            if not isinstance(name, str) or not name.strip() or not canonical_name(name):
                raise ValueError(f'Missing English perk name: {loadout}; verify enUS client text is installed')
            texture = guid(perk.get('TextureGUID'))
            if not texture.endswith('.004'):
                raise ValueError(f'Unsupported texture type (needs explicit resolution): {texture}')
            translated = next((p for p in ((localized or {}).get(hero_guid, {}).get('Perks') or [])
                               if p.get('GUID') and guid(p['GUID']) == loadout), {})
            planned.append({'hero': hero_id, 'tier': tier, 'id': name.strip(),
                            'kind': 'client-glyph', 'hero_guid': guid(hero_guid),
                            'loadout_guid': loadout, 'texture_guid': texture,
                            'perk_guids': sorted(k for k, _ in candidates),
                            'level': level, 'name_zhCN': translated.get('Name'),
                            'perk_set_guid': (current or {}).get(hero_guid, {}).get('PerkSetGUID')})
            tiers[tier] += 1
        if tiers != Counter(minor=2, major=2):
            raise ValueError(f'Expected 2 minor + 2 major active perks for {hero_id}, got {dict(tiers)}')
    if seen != set(known_heroes):
        raise ValueError(f'Client is missing project heroes: {sorted(set(known_heroes) - seen)}')
    keys = [(p['hero'], p['tier'], p['id']) for p in planned]
    if len(set(keys)) != len(keys):
        raise ValueError('Duplicate active perk names within a hero/tier')
    return sorted(planned, key=lambda p: (p['hero'], p['tier'], p['id'])), ignored


def resolve_texture(raw, texture_guid):
    # --no-names uses the lower 48-bit key, not localized names. Reject arrays
    # instead of silently selecting an arbitrary surface for recognition.
    key = guid(texture_guid).split('.')[0]
    files = list((raw / 'icons/HeroIcons').glob(f'*/Perks/{key}.png'))
    if not files or len({sha256(p) for p in files}) != 1:
        raise ValueError(f'Missing or conflicting extracted PNG: {texture_guid}')
    if any(list(p.parent.glob(key + '_*.png')) for p in files):
        raise ValueError(f'Multi-surface texture needs manual review: {texture_guid}')
    return files[0]


def stage(raw, source, output, resource_version):
    if output.exists() or source.resolve() in output.resolve().parents:
        raise ValueError('Output must be a new directory outside the original source')
    metadata = read_json(raw / 'extraction.json')
    if metadata.get('status') != 'extracted':
        raise ValueError('Extraction did not complete')
    required = {'heroes-enUS.json', 'heroes-zhCN.json', 'talents-zhCN.json', 'current-perks.json', 'build.info'}
    for name, digest in metadata['metadata_sha256'].items():
        if name not in required:
            raise ValueError('Unexpected metadata file')
        if sha256(raw / name) != digest:
            raise ValueError(f'Metadata hash mismatch: {name}')
    if set(metadata['metadata_sha256']) != required:
        raise ValueError('Incomplete extraction provenance')
    original = validate_source(source)
    planned, ignored = plan_perks(read_json(raw / 'heroes-enUS.json'), read_json(raw / 'talents-zhCN.json'),
                                  [h['id'] for h in original['heroes']], read_json(raw / 'heroes-zhCN.json'),
                                  read_json(raw / 'current-perks.json'))
    images = []
    for perk in planned:
        image = resolve_texture(raw, perk['texture_guid'])
        # Validate with the same conversion used to build the runtime templates.
        vectors = list(client_glyph_vectors(read_image(image.parent, image.name, alpha=True)))
        if len(vectors) != 5 or any(float(v @ v) < .99 for v in vectors):
            raise ValueError(f'Degenerate glyph: {image}')
        perk['sha256'] = sha256(image)
        perk['file'] = f"perks/client/{perk['hero']}/{perk['tier']}/{perk['texture_guid']}.png"
        images.append(image)
    old_keys = {(p['hero'], p['tier'], p['id']) for p in original['perks']}
    new_keys = {(p['hero'], p['tier'], p['id']) for p in planned}
    report = {'client': metadata['client'], 'resource_version': resource_version,
              'heroes': len(original['heroes']), 'active_perks': len(planned),
              'added': sorted(new_keys - old_keys), 'removed': sorted(old_keys - new_keys),
              'unmapped_client_heroes': ignored, 'recognition_validation': 'pending-real-screenshot-regression'}
    original['resource_version'] = resource_version
    original['perks'] = planned
    original['sources'] = [s for s in original['sources']
                           if s['name'] not in ('Installed Overwatch client perk textures', 'Official perk glyphs')]
    original['sources'].append({'name': 'Installed Overwatch client perk textures',
                                'use': 'All active perk mappings and glyphs',
                                'copyright': 'Blizzard Entertainment',
                                'client': metadata['client'], 'tool': metadata['tool'],
                                'upstream': 'https://github.com/overtools/OWLib',
                                'scope': 'Project heroes only; see client-perks-report.json'})
    # Start a clean candidate: preserve only referenced hero/UI files, then
    # populate the current perk set. Never copy retired glyphs into later packs.
    output.mkdir(parents=True)
    keep = set(original['ui'].values())
    for hero in original['heroes']:
        keep.update(hero['portraits'])
        keep.update(hero.get('portrait_masks', []))
        if hero.get('hud'):
            keep.add(hero['hud'])
    for relative in keep:
        origin = source_path(source, relative)
        target = output / origin.relative_to(source.resolve())
        target.parent.mkdir(parents=True, exist_ok=True)
        shutil.copy2(origin, target)
    for perk, image in zip(planned, images):
        target = output / perk['file']
        target.parent.mkdir(parents=True, exist_ok=True)
        shutil.copy2(image, target)
    write_json(output / 'manifest.json', original)
    write_json(output / 'client-perks-report.json', report)
    validate_source(output)
    return report


def run_datatool(tool, client, raw, mode, arguments, expected=None, allow_cdn_text=False):
    english = expected == 'heroes-enUS.json'
    command = [str(tool), str(client), mode, *arguments, '--language=' + ('enUS' if english else 'zhCN'),
               '--speech-language=zhCN', '--online=' + ('true' if english and allow_cdn_text else 'false')]
    if build_info(client)['Branch'].lower() == 'cn':
        command.append('--rcn=true')
    with (raw / f'{expected or mode}.log').open('w', encoding='utf-8') as log:
        result = subprocess.run(command, cwd=raw, stdout=log, stderr=subprocess.STDOUT)
    # DataTool can report fatal errors but still exit 0. Require actual outputs.
    if result.returncode or (expected and not (raw / expected).is_file()):
        raise RuntimeError(f'DataTool {mode} failed; inspect logs in {raw}. Check file locks and locale availability.')
    if expected and not isinstance(read_json(raw / expected), dict):
        raise ValueError(f'Unexpected DataTool JSON schema: {expected}')


def export_current_references(tool, client, raw):
    # Use the installed C# compiler with the release's own .NET reference DLLs.
    # This avoids rebuilding OWLib or modifying its distributed assemblies.
    sdks = subprocess.check_output(['dotnet', '--list-sdks'], text=True).splitlines()
    matches = [re.fullmatch(r'(\S+) \[(.+)\]', line) for line in sdks]
    compilers = [Path(m[2]) / m[1] / 'Roslyn/bincore/csc.dll' for m in matches if m]
    if not compilers:
        raise RuntimeError('A .NET SDK is required to compile the current-perk reference helper')
    helper = tool.parent / 'CurrentPerks.dll'
    references = ['System.Private.CoreLib', 'System.Runtime', 'System.Console', 'System.Collections',
                  'System.IO.FileSystem', 'DataTool', 'TankLib', 'TACTLib', 'Newtonsoft.Json']
    source = ROOT / 'scripts/owlib_current_perks.cs'
    subprocess.run(['dotnet', str(compilers[-1]), '/nologo', '/target:exe', '/out:' + str(helper),
                    *['/reference:' + str(tool.parent / (name + '.dll')) for name in references],
                    str(source)], check=True)
    with (raw / 'current-perks.log').open('w', encoding='utf-8') as log:
        subprocess.run(['dotnet', 'exec', '--runtimeconfig', str(tool.parent / 'DataTool.runtimeconfig.json'),
                        '--depsfile', str(tool.parent / 'DataTool.deps.json'), str(helper),
                        str(client), str(raw / 'current-perks.json')], stdout=log, stderr=subprocess.STDOUT, check=True)
    if not isinstance(read_json(raw / 'current-perks.json'), dict):
        raise ValueError('Invalid current-perk reference export')
    return {'source_sha256': sha256(source), 'binary_sha256': sha256(helper)}


def extract(tool, client, raw, allow_cdn_text=False):
    if raw.exists():
        raise ValueError('Extraction directory must be new; never reuse potentially stale output')
    before = build_info(client)
    locks = check_index_locks(client)
    if locks:
        raise RuntimeError(f'Game archives are locked ({len(locks)} indices); close Overwatch and Battle.net first')
    raw.mkdir(parents=True)
    shutil.copy2(client / '.build.info', raw / 'build.info')
    for name, mode in [('heroes-enUS.json', 'list-heroes'), ('heroes-zhCN.json', 'list-heroes'),
                       ('talents-zhCN.json', 'list-talents')]:
        run_datatool(tool, client, raw, mode, ['--json', '--out=' + str(raw / name)], name, allow_cdn_text)
    helper = export_current_references(tool, client, raw)
    run_datatool(tool, client, raw, 'extract-hero-icons', [str(raw / 'icons'), '--no-names=true'])
    if build_info(client) != before or sha256(client / '.build.info') != sha256(raw / 'build.info'):
        raise RuntimeError('Client build changed during extraction; discard this candidate and retry')
    version = next((line for line in (raw / 'heroes-enUS.json.log').read_text(encoding='utf-8').splitlines()
                    if 'DataTool v' in line), None)
    write_json(raw / 'extraction.json', {'status': 'extracted', 'client': before,
               'tool': {'path': str(tool), 'datatool_sha256': sha256(tool),
                        'version_line': version,
                        'dll_sha256': {p.name: sha256(p) for p in tool.parent.glob('*.dll')},
                        'log': 'heroes-enUS.json.log', 'english_text_cdn_allowed': allow_cdn_text,
                        'textures_offline': True, 'current_reference_helper': helper},
               'metadata_sha256': {name: sha256(raw / name) for name in
                   ('heroes-enUS.json', 'heroes-zhCN.json', 'talents-zhCN.json', 'current-perks.json', 'build.info')}})


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--client', type=Path, default=Path('E:/Overwatch'))
    parser.add_argument('--tool', type=Path, default=ROOT / '.deps/owlib/bin/DataTool.exe')
    parser.add_argument('--source', type=Path, default=ROOT / 'assets/recognition')
    parser.add_argument('--raw', type=Path, default=ROOT / '.diagnostics/client-perks/extracted')
    parser.add_argument('--output-source', type=Path, default=ROOT / '.build/client-perks/final-source')
    parser.add_argument('--resource-version', default='2026.10.16.1')
    parser.add_argument('--preflight', action='store_true')
    parser.add_argument('--allow-cdn-text', action='store_true',
                        help='Allow DataTool to fetch missing enUS metadata for this exact installed build')
    parser.add_argument('--import-only', action='store_true', help='Stage a previously completed, hashed extraction')
    args = parser.parse_args()
    if not re.fullmatch(r'[A-Za-z0-9._-]+', args.resource_version):
        parser.error('Invalid resource version')
    if args.preflight and args.import_only:
        parser.error('--preflight and --import-only cannot be combined')
    if not args.preflight and args.output_source.exists():
        parser.error('Output source directory already exists; choose a new candidate directory')
    if not args.import_only:
        if not args.tool.is_file():
            parser.error('Download and unpack DataTool from https://github.com/overtools/OWLib/releases first')
        if args.preflight:
            print(json.dumps({'client': build_info(args.client), 'tool_sha256': sha256(args.tool),
                              'index_access_failures': check_index_locks(args.client),
                              'extracted': False}, ensure_ascii=False, indent=2))
            return
        extract(args.tool.resolve(), args.client.resolve(), args.raw.resolve(), args.allow_cdn_text)
    print(json.dumps(stage(args.raw, args.source, args.output_source, args.resource_version),
                     ensure_ascii=False, indent=2))


if __name__ == '__main__':
    main()
