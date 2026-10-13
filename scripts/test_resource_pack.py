"""Validate resource mappings and the standalone archive without touching OBS."""
import copy
import hashlib
import json
import unittest
import zipfile
from build_resource_pack import ROOT, MODEL_FILES, validate_source


class ResourcePackTests(unittest.TestCase):
    def setUp(self):
        self.root = ROOT / 'assets/recognition'
        self.source = json.loads((self.root / 'manifest.json').read_text(encoding='utf8'))

    def test_valid_mapping(self):
        self.assertEqual(validate_source(self.root)['resource_version'], self.source['resource_version'])

    def test_duplicate_hero(self):
        self.source['heroes'].append(copy.deepcopy(self.source['heroes'][0]))
        with self.assertRaisesRegex(ValueError, 'duplicate hero'):
            validate_source(self.root, self.source)

    def test_missing_image(self):
        self.source['heroes'][0]['hud'] = 'missing.png'
        with self.assertRaisesRegex(ValueError, 'Missing asset'):
            validate_source(self.root, self.source)

    def test_new_hero_and_perk_mapping(self):
        hero = copy.deepcopy(self.source['heroes'][0])
        hero['id'] = 'fixture-new-hero'
        self.source['heroes'].append(hero)
        perk = copy.deepcopy(self.source['perks'][0])
        perk['hero'] = hero['id']
        self.source['perks'].append(perk)
        self.assertEqual(validate_source(self.root, self.source)['heroes'][-1]['id'], hero['id'])

    def test_foreign_hero_perk(self):
        self.source['perks'][0]['hero'] = 'nonexistent-hero'
        with self.assertRaisesRegex(ValueError, 'Invalid perk mapping'):
            validate_source(self.root, self.source)

    def test_path_escape(self):
        self.source['heroes'][0]['hud'] = '../../README.md'
        with self.assertRaisesRegex(ValueError, 'outside source'):
            validate_source(self.root, self.source)

    def test_archive_complete_and_consistent(self):
        archive = ROOT / 'release' / f"OWCS-Recognition-{self.source['resource_version']}.zip"
        with zipfile.ZipFile(archive) as pack:
            manifest = json.loads(pack.read('manifest.json'))
            self.assertEqual(manifest['asset_format'], 2)
            self.assertEqual({f['path'] for f in manifest['files']}, {'models/' + n for n in MODEL_FILES})
            for entry in manifest['files']:
                self.assertEqual(hashlib.sha256(pack.read(entry['path'])).hexdigest(), entry['sha256'])
            self.assertEqual(json.loads(pack.read('models/manifest.json'))['resource_version'], manifest['resource_version'])
            self.assertIn('Install-Resources.ps1', pack.namelist())


if __name__ == '__main__':
    unittest.main()
