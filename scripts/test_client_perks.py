"""Contract tests using synthetic OWLib exports, not proof of live extraction."""
import copy
import json
from pathlib import Path
import tempfile
import unittest
from unittest.mock import patch

import cv2
import numpy as np

from extract_client_perks import (build_info, plan_perks, resolve_texture, run_datatool,
                                  sha256, stage, write_json)
from verify_client_perks import evaluate
from build_resource_pack import client_glyph_vectors


class ClientPerkTests(unittest.TestCase):
    def setUp(self):
        self.hero_guid = '000000000001.075'
        perks = [{'GUID': f'{i:012X}.09F', 'TextureGUID': f'{i:012X}.004', 'Name': f'Perk {i}'}
                 for i in range(1, 5)]
        self.heroes = {self.hero_guid: {'Name': 'Lúcio', 'IsHero': True, 'Perks': perks}}
        self.talents = {f'{i:012X}.134': {'GUID': None, 'TalentType': 'Perk',
                        'Loadout': {'GUID': p['GUID'], 'HeroName': None},
                        'Major': i > 2, 'Level': 3 if i > 2 else 2}
                        for i, p in enumerate(perks, 1)}

    def plan(self):
        return plan_perks(self.heroes, self.talents, ['lucio'])

    def test_guid_join_ignores_retired_talent_and_unreliable_owner(self):
        self.talents['000000000099.134'] = {'TalentType': 'Perk', 'Major': True, 'Level': 3,
                                           'Loadout': {'GUID': '000000000099.09F'}}
        perks, _ = self.plan()
        self.assertEqual(len(perks), 4)
        self.assertEqual({p['hero'] for p in perks}, {'lucio'})
        self.assertEqual(perks[0]['perk_guids'], ['000000000003.134'])

    def test_conflicting_tier_is_rejected(self):
        other = copy.deepcopy(self.talents['000000000001.134'])
        other['Major'] = True
        self.talents['000000000099.134'] = other
        with self.assertRaisesRegex(ValueError, 'Conflicting'):
            self.plan()

    def test_current_reference_disambiguates_retired_tier(self):
        other = copy.deepcopy(self.talents['000000000001.134'])
        other['Major'] = True
        self.talents['000000000099.134'] = other
        current = {self.hero_guid: {'Perks': [
            {'LoadoutGUID': t['Loadout']['GUID'], 'PerkGUID': key, 'Major': t['Major'], 'Level': t['Level']}
            for key, t in self.talents.items() if key != '000000000099.134']}}
        planned, _ = plan_perks(self.heroes, self.talents, ['lucio'], current=current)
        first = next(p for p in planned if p['id'] == 'Perk 1')
        self.assertEqual(first['tier'], 'minor')
        self.assertEqual(first['perk_guids'], ['000000000001.134'])

    def test_missing_or_non_boolean_tier_is_rejected(self):
        self.talents['000000000001.134']['Major'] = 'false'
        with self.assertRaisesRegex(ValueError, 'Missing explicit tier'):
            self.plan()

    def test_incomplete_hero_is_rejected(self):
        self.heroes[self.hero_guid]['Perks'].pop()
        with self.assertRaisesRegex(ValueError, '2 minor'):
            self.plan()

    def test_missing_project_hero_is_rejected(self):
        with self.assertRaisesRegex(ValueError, 'missing project heroes'):
            plan_perks(self.heroes, self.talents, ['lucio', 'ana'])

    def test_names_and_ids_are_not_inferred_from_file_names(self):
        self.heroes[self.hero_guid]['Perks'][0]['Name'] = None
        with self.assertRaisesRegex(ValueError, 'Missing English'):
            self.plan()

    def test_localized_name_is_joined_by_guid(self):
        local = {self.hero_guid: {'Perks': [{'GUID': '000000000001.09F', 'Name': '测试威能'}]}}
        planned, _ = plan_perks(self.heroes, self.talents, ['lucio'], local)
        self.assertEqual(next(p for p in planned if p['id'] == 'Perk 1')['name_zhCN'], '测试威能')

    def test_unknown_client_hero_is_reported(self):
        self.heroes['000000000099.075'] = {'Name': 'Future Hero', 'IsHero': True, 'Perks': []}
        _, ignored = self.plan()
        self.assertEqual(ignored[0]['name'], 'Future Hero')

    def test_texture_arrays_and_conflicting_files_are_rejected(self):
        with tempfile.TemporaryDirectory() as directory:
            raw = Path(directory)
            first = raw / 'icons/HeroIcons/Lucio/Perks/000000000001.png'
            first.parent.mkdir(parents=True)
            first.write_bytes(b'png fixture')
            self.assertEqual(resolve_texture(raw, '000000000001.004'), first)
            first.with_name('000000000001_1.png').write_bytes(b'array')
            with self.assertRaisesRegex(ValueError, 'Multi-surface'):
                resolve_texture(raw, '000000000001.004')

    def test_datatool_zero_exit_without_output_is_failure(self):
        with tempfile.TemporaryDirectory() as directory:
            raw = Path(directory)
            with patch('extract_client_perks.subprocess.run') as run, \
                 patch('extract_client_perks.build_info', return_value={'Branch': 'cn'}):
                run.return_value.returncode = 0
                with self.assertRaisesRegex(RuntimeError, 'failed'):
                    run_datatool(Path('DataTool.exe'), raw, raw, 'list-heroes', [], 'heroes-enUS.json')

    def test_stage_keeps_source_unchanged_and_replaces_retired_mapping(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            raw, source, output = root / 'raw', root / 'source', root / 'candidate'
            raw.mkdir()
            source.mkdir()
            glyph = np.zeros((64, 64, 4), np.uint8)
            cv2.rectangle(glyph, (20, 12), (43, 51), (255, 255, 255, 255), -1)
            cv2.imwrite(str(source / 'hero.png'), glyph)
            old = {'source_format': 1, 'resource_version': 'old',
                   'heroes': [{'id': 'lucio', 'hud': 'hero.png', 'portraits': ['hero.png']}],
                   'perks': [{'hero': 'lucio', 'tier': 'minor', 'id': 'Retired',
                              'kind': 'scoreboard', 'file': 'hero.png'}],
                   'ui': {name: 'hero.png' for name in ('ready.png', 'unselected-hud.png', 'unselected-panel.png')},
                   'sources': []}
            write_json(source / 'manifest.json', old)
            (source / 'retired-unreferenced.png').write_bytes(b'obsolete')
            original_hash = sha256(source / 'manifest.json')
            write_json(raw / 'heroes-enUS.json', self.heroes)
            write_json(raw / 'heroes-zhCN.json', self.heroes)
            write_json(raw / 'talents-zhCN.json', self.talents)
            write_json(raw / 'current-perks.json', {self.hero_guid: {'Perks': [
                {'LoadoutGUID': t['Loadout']['GUID'], 'PerkGUID': key, 'Major': t['Major'], 'Level': t['Level']}
                for key, t in self.talents.items()]}})
            (raw / 'build.info').write_text('synthetic build')
            files = ('heroes-enUS.json', 'heroes-zhCN.json', 'talents-zhCN.json', 'current-perks.json', 'build.info')
            write_json(raw / 'extraction.json', {'status': 'extracted', 'client': {'Version': 'synthetic'},
                       'tool': {'version_line': 'synthetic'}, 'metadata_sha256': {f: sha256(raw / f) for f in files}})
            textures = raw / 'icons/HeroIcons/Lucio/Perks'
            textures.mkdir(parents=True)
            for i in range(1, 5):
                cv2.imwrite(str(textures / f'{i:012X}.png'), glyph)
            report = stage(raw, source, output, 'test-candidate')
            self.assertEqual(report['active_perks'], 4)
            self.assertIn(('lucio', 'minor', 'Retired'), report['removed'])
            self.assertEqual(sha256(source / 'manifest.json'), original_hash)
            self.assertTrue((source / 'retired-unreferenced.png').exists())
            self.assertFalse((output / 'retired-unreferenced.png').exists())
            manifest = json.loads((output / 'manifest.json').read_text(encoding='utf-8'))
            self.assertEqual(len(manifest['perks']), 4)
            for perk in manifest['perks']:
                self.assertEqual(sha256(output / perk['file']), perk['sha256'])
            # A modified metadata export cannot be silently re-imported.
            (raw / 'talents-zhCN.json').write_text('{}')
            with self.assertRaisesRegex(ValueError, 'hash mismatch'):
                stage(raw, source, root / 'other-candidate', 'test-candidate')

    def test_active_build_is_not_first_inactive_build(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            (root / '.build.info').write_text(
                'Active!DEC:1|Version!STRING:0|Build Key!HEX:16|CDN Key!HEX:16|Branch!STRING:0|Product!STRING:0\n'
                '0|old|a|b|cn|pro\n1|current|c|d|cn|pro\n')
            self.assertEqual(build_info(root)['Version'], 'current')

    def test_unknown_is_not_correct_unselected_even_with_null_id(self):
        labels = [{'file': 'a.png', 'kind': 'perk', 'expected': 'A', 'selection': 'selected',
                   'hero': 'lucio', 'tier': 'minor'},
                  {'file': 'b.png', 'kind': 'perk', 'expected': None, 'selection': 'unselected',
                   'hero': 'lucio', 'tier': 'minor'}]
        report = evaluate(labels, {'a.png': {'id': 'A', 'selection': 'selected'},
                                  'b.png': {'id': None, 'selection': 'unknown'}})
        self.assertEqual(report['counts']['negative_correct'], 0)
        self.assertEqual(len(report['failures']), 1)

    def test_client_canvas_position_is_not_discarded(self):
        glyph = np.zeros((128, 128, 4), np.uint8)
        cv2.rectangle(glyph, (20, 20), (45, 100), (255, 255, 255, 255), -1)
        shifted = np.roll(glyph, 24, axis=1)
        vectors = list(client_glyph_vectors(glyph))
        other = list(client_glyph_vectors(shifted))
        self.assertEqual(len(vectors), 5)
        self.assertLess(float(vectors[2] @ other[2]), .9)
        with self.assertRaisesRegex(ValueError, 'square canvas'):
            list(client_glyph_vectors(glyph[:, :64]))


if __name__ == '__main__':
    unittest.main()
