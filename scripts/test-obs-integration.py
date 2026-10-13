"""Developer integration test against an isolated OBS instance, never the user's session."""
import asyncio
import base64
import json
from pathlib import Path
import aiohttp
import cv2
import numpy as np

ROOT = Path(__file__).resolve().parents[1]
OUT = ROOT / '.diagnostics'

async def main():
    async with aiohttp.ClientSession() as http:
        ws = await http.ws_connect('http://127.0.0.1:4457')
        hello = await ws.receive_json()
        assert 'authentication' not in hello['d'], 'Use only the isolated, no-auth test OBS'
        await ws.send_json({'op': 1, 'd': {'rpcVersion': 1, 'eventSubscriptions': 0}})
        assert (await ws.receive_json())['op'] == 2
        seq = 0
        async def request(kind, **data):
            nonlocal seq
            seq += 1
            await ws.send_json({'op': 6, 'd': {'requestType': kind, 'requestId': str(seq), 'requestData': data}})
            result = (await ws.receive_json())['d']
            assert result['requestStatus']['result'], result
            return result.get('responseData', {})
        async def screenshot():
            r = await request('GetSourceScreenshot', sourceName='Fixture', imageFormat='png')
            return cv2.imdecode(np.frombuffer(base64.b64decode(r['imageData'].split(',')[1]), np.uint8), 1)
        expected = json.loads((ROOT / 'tests/data/scoreboard/expected.json').read_text())
        existing = await request('GetInputList')
        for item in existing['inputs']:
            if item['inputName'] in {'Fixture', 'MediaFixture'}:
                await request('RemoveInput', inputName=item['inputName'])
        if existing['inputs']:
            await asyncio.sleep(.5)  # OBS releases source references on its render thread.
        await request('CreateInput', sceneName='Test', inputName='Fixture', inputKind='image_source', inputSettings={'file': str(ROOT / 'tests/data/scoreboard/perks.png')}, sceneItemEnabled=True)
        await asyncio.sleep(1)
        before = await screenshot()
        await request('CreateSourceFilter', sourceName='Fixture', filterName='Analyzer', filterKind='owcs_ob_analyzer_filter', filterSettings={'enabled': True, 'recognize_names': True, 'fps': 5, 'auto_tab': False})
        async def state():
            async with http.get('http://127.0.0.1:8766/v1/state') as response:
                return await response.json()
        for _ in range(100):
            s = await state()
            if (s.get('panel_visible')
                    and s['players'][0]['stats']['damage'] == 4350
                    and [p['ultimate_charge'] for p in s['players']] == expected['perks']['charge']
                    and [p['hero'] for p in s['players']] == expected['perks']['heroes']):
                break
            await asyncio.sleep(.25)
        assert s.get('panel_visible'), s
        import jsonschema
        schema = json.loads((ROOT / 'schema/player-state-v3.schema.json').read_text())
        jsonschema.validate(s, schema)
        assert s['schema_version'] == '3.0'
        assert 'source' not in s and 'hud' not in s and 'recognition' not in s
        assert [p['ultimate_charge'] for p in s['players']] == expected['perks']['charge']
        assert [p['hero'] for p in s['players']] == expected['perks']['heroes']
        after = await screenshot()
        assert before.shape == after.shape
        difference = np.abs(before.astype(float) - after.astype(float))
        assert difference.max() == 0, ('Filter changed pixels', difference.mean(), difference.max())
        cv2.imwrite(str(OUT / 'native-obs-source.png'), after)
        async with http.ws_connect('http://127.0.0.1:8766/v1/state-stream') as stream:
            first = await stream.receive_json(timeout=3)
            await stream.ping(b'test')
            second = await stream.receive_json(timeout=3)
            jsonschema.validate(first, schema)
            jsonschema.validate(second, schema)
            assert first['stream_id'] == second['stream_id'] == s['stream_id']
            assert second['players'][1]['ultimate_ready'] is True
        # Exercise a different OBS source implementation through a short media file.
        movie = OUT / 'scoreboard-fixture.mp4'
        writer = cv2.VideoWriter(str(movie), cv2.VideoWriter_fourcc(*'mp4v'), 10, (1600,1000))
        assert writer.isOpened()
        for _ in range(30): writer.write(before)
        writer.release()
        media = await request('CreateInput', sceneName='Test', inputName='MediaFixture', inputKind='ffmpeg_source', inputSettings={'local_file': str(movie), 'is_local_file': True, 'looping': True}, sceneItemEnabled=True)
        await request('CreateSourceFilter', sourceName='MediaFixture', filterName='AnalyzerMedia', filterKind='owcs_ob_analyzer_filter', filterSettings={'enabled': True, 'recognize_names': False, 'fps': 5, 'auto_tab': False})
        media_state = None
        for _ in range(80):
            async with http.get('http://127.0.0.1:8766/v1/streams') as resp: streams = (await resp.json())['streams']
            for item in streams:
                if item.get('name') == 'MediaFixture':
                    async with http.get('http://127.0.0.1:8766/v1/state', params={'stream': item['stream_id']}) as resp: media_state = await resp.json()
            if (media_state and media_state.get('panel_visible') and
                    media_state['players'][1]['ultimate_charge'] == 100): break
            await asyncio.sleep(.25)
        assert media_state and media_state['panel_visible'], media_state
        assert media_state['players'][1]['ultimate_charge'] == 100
        await request('RemoveInput', inputName='MediaFixture')
        # Names are sampled one row per recognition cycle.
        await asyncio.sleep(9)
        s = await state()
        (OUT / 'native-obs-state.json').write_text(json.dumps(s, ensure_ascii=False, indent=2), encoding='utf8')
        # Searching a scene without the HUD must keep both input and analysis moving.
        negative = cv2.imread(str(ROOT / 'tests/data/scoreboard/hud.png'))
        negative[:180] = 0
        negative_path = OUT / 'integration-no-hud.png'
        cv2.imwrite(str(negative_path), negative)
        await request('SetInputSettings', inputName='Fixture', inputSettings={'file': str(negative_path)})
        samples = []
        for _ in range(20):
            await asyncio.sleep(.2)
            samples.append(await state())
        assert all(x['status'] == 'online' for x in samples)
        assert samples[-1]['sequence'] - samples[0]['sequence'] >= 5
        assert not samples[-1]['panel_visible']
        await request('SetInputSettings', inputName='Fixture', inputSettings={'file': str(ROOT / 'tests/data/scoreboard/live-new-heroes-hud.png')})
        for _ in range(50):
            await asyncio.sleep(.2)
            live_hud = await state()
            if live_hud['status'] == 'online' and not live_hud['panel_visible']: break
        assert live_hud['status'] == 'online' and not live_hud['panel_visible']
        await request('SetInputSettings', inputName='Fixture', inputSettings={'file': str(ROOT / 'tests/data/scoreboard/header-touching-portrait.png')})
        for _ in range(50):
            await asyncio.sleep(.2)
            live_panel = await state()
            if live_panel['panel_visible'] and live_panel['players'][0]['ultimate_charge'] == 24: break
        assert live_panel['panel_visible']
        assert [p['ultimate_charge'] for p in live_panel['players']] == [24,35,42,31,27,33,40,53,35,25]
        # A 2560px source exercises OBS's GPU downscale, not just CPU fixture resizing.
        await request('SetInputSettings', inputName='Fixture', inputSettings={'file': str(ROOT / 'tests/data/scoreboard/stability-single-perk.png')})
        stat_keys = ['eliminations', 'assists', 'deaths', 'damage', 'healing', 'mitigated']
        def stats_match(value):
            rows = value.get('players', [])
            return len(rows) == 10 and all(
                [p['stats'][k] for k in stat_keys] == expected['stability-single-perk']['stats'][i]
                for i, p in enumerate(rows))
        for _ in range(100):
            await asyncio.sleep(.2)
            stability = await state()
            if stats_match(stability) and stability['players'][6]['perks']['minor']['id'] == 'Ulterior Motive': break
        assert stats_match(stability), [('row', i, p['stats']) for i, p in enumerate(stability['players'])]
        assert stability['players'][6]['perks']['minor']['id'] == 'Ulterior Motive'
        (OUT / 'stability-obs-state.json').write_text(json.dumps(stability,ensure_ascii=False,indent=2),encoding='utf8')
        await request('SetInputSettings', inputName='Fixture', inputSettings={'file': str(ROOT / 'tests/data/scoreboard/stability-connected-digits.png')})
        def connected_match(value):
            rows = value.get('players', [])
            e = expected['stability-connected-digits']
            return len(rows) == 10 and all(
                p['hero'] == e['heroes'][i] and p['life_state'] == e['life'][i]
                and [p['stats'][k] for k in stat_keys] == e['stats'][i]
                and all(k not in p['stale_fields'] + p['unknown_fields'] for k in ['stats.' + k for k in stat_keys] + ['hero', 'life_state'])
                for i, p in enumerate(rows))
        for _ in range(75):
            await asyncio.sleep(.2)
            connected = await state()
            if connected_match(connected): break
        assert connected_match(connected), connected
        (OUT / 'connected-obs-state.json').write_text(json.dumps(connected,ensure_ascii=False,indent=2),encoding='utf8')
        await request('SetInputSettings', inputName='Fixture', inputSettings={'file': str(ROOT / 'tests/data/scoreboard/unselected-hud.png')})
        for _ in range(75):
            await asyncio.sleep(.2)
            preselection_hud = await state()
            if preselection_hud['status'] == 'online' and not preselection_hud['panel_visible']: break
        assert not preselection_hud['panel_visible'], preselection_hud
        await request('SetInputSettings', inputName='Fixture', inputSettings={'file': str(ROOT / 'tests/data/scoreboard/unselected-panel.png')})
        for _ in range(75):
            await asyncio.sleep(.2)
            preselection = await state()
            if (preselection['panel_visible'] and
                    all(p.get('hero_selection') == 'unselected' for p in preselection['players']) and
                    preselection['players'][0]['stats']['damage'] == 5514): break
        assert preselection['panel_visible']
        for p in preselection['players']:
            assert p['hero_selection'] == 'unselected' and p['hero'] is None, p
            assert p['life_state'] == 'unknown' and p['ultimate_charge'] is None, p
            assert p['perks']['minor']['id'] is None
            assert 'hero_selection' not in p['stale_fields'] + p['unknown_fields']
        import jsonschema
        jsonschema.validate(preselection, json.loads((ROOT / 'schema/player-state-v3.schema.json').read_text()))
        (OUT / 'unselected-obs-state.json').write_text(json.dumps(preselection,ensure_ascii=False,indent=2),encoding='utf8')
        await request('SetInputSettings', inputName='Fixture', inputSettings={'file': str(ROOT / 'tests/data/scoreboard/stability-connected-digits.png')})
        for _ in range(75):
            await asyncio.sleep(.2)
            selected = await state()
            if connected_match(selected): break
        assert connected_match(selected), selected
        await request('SetSourceFilterSettings', sourceName='Fixture', filterName='Analyzer', filterSettings={'enabled': False})
        await asyncio.sleep(.5)
        paused = await state()
        assert paused['status'] == 'stopped'
        assert 'stats.damage' in paused['players'][0]['stale_fields']
        await request('RemoveInput', inputName='Fixture')
        await asyncio.sleep(.5)
        assert (await state())['players'] == []
        report = {'plugin_loaded': True, 'image_source': True, 'media_source': True, 'pixels_unchanged': True, 'http_websocket': True, 'pause_remove': True, 'search_remains_online': True, 'touching_header_panel': True, 'recognized_names': [p['display_name'] for p in s['players']]}
        report['connected_digits_and_death_portraits'] = True
        report['schema_version'] = '3.0'
        report['unselected_hud_panel_and_selection_transition'] = 'public panel transitions; HUD detection covered by native tests'
        (OUT / 'native-integration-report.json').write_text(json.dumps(report, ensure_ascii=False, indent=2), encoding='utf8')
        print(json.dumps(report, ensure_ascii=False))
        await ws.close()

asyncio.run(main())
