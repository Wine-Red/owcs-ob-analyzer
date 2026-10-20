"""Developer integration test against an isolated OBS instance, never the user's session."""
import argparse
import asyncio
import base64
import json
import subprocess
from pathlib import Path
import aiohttp
import cv2
import numpy as np

ROOT = Path(__file__).resolve().parents[1]
parser = argparse.ArgumentParser()
parser.add_argument('--websocket-port', type=int, default=4457)
parser.add_argument('--api-port', type=int, default=8766)
parser.add_argument('--output', type=Path, default=ROOT / '.diagnostics')
parser.add_argument('--expect-obs-version')
parser.add_argument('--obs-pid', type=int)
args = parser.parse_args()
OUT = args.output.resolve()
OUT.mkdir(parents=True, exist_ok=True)
API = f'http://127.0.0.1:{args.api_port}'

async def main():
    async with aiohttp.ClientSession() as http:
        ws = await http.ws_connect(f'http://127.0.0.1:{args.websocket_port}')
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
        version = await request('GetVersion')
        if args.expect_obs_version:
            assert version['obsVersion'] == args.expect_obs_version, version
        async def screenshot():
            r = await request('GetSourceScreenshot', sourceName='Fixture', imageFormat='png')
            return cv2.imdecode(np.frombuffer(base64.b64decode(r['imageData'].split(',')[1]), np.uint8), 1)
        expected = json.loads((ROOT / 'tests/data/scoreboard/expected.json').read_text())
        initial_heroes = [None if life == 'dead' else hero for hero, life in zip(expected['perks']['heroes'], expected['perks']['life'])]
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
            async with http.get(API + '/v1/state') as response:
                return await response.json()
        for _ in range(100):
            s = await state()
            if (s.get('panel_visible') and len(s['players']) == 10
                    and s['players'][0]['stats']['damage'] == 4350
                    and [p['ultimate_charge'] for p in s['players']] == expected['perks']['charge']
                    and [p['hero'] for p in s['players']] == initial_heroes):
                break
            await asyncio.sleep(.25)
        assert s.get('panel_visible'), s
        import jsonschema
        schema = json.loads((ROOT / 'schema/player-state-v4.schema.json').read_text())
        jsonschema.validate(s, schema)
        assert s['schema_version'] == '4.0'
        assert not {'source', 'hud', 'recognition', 'players_by_id', 'player_order', 'retained_players'} & s.keys()
        assert len({p['player_id'] for p in s['players']}) == len(s['players'])
        assert [p['ultimate_charge'] for p in s['players']] == expected['perks']['charge']
        assert [p['hero'] for p in s['players']] == initial_heroes
        after = await screenshot()
        assert before.shape == after.shape
        difference = np.abs(before.astype(float) - after.astype(float))
        assert difference.max() == 0, ('Filter changed pixels', difference.mean(), difference.max())
        cv2.imwrite(str(OUT / 'native-obs-source.png'), after)
        async with http.ws_connect(API + '/v1/state-stream') as stream:
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
            async with http.get(API + '/v1/streams') as resp: streams = (await resp.json())['streams']
            for item in streams:
                if item.get('name') == 'MediaFixture':
                    async with http.get(API + '/v1/state', params={'stream': item['stream_id']}) as resp: media_state = await resp.json()
            if media_state and media_state.get('panel_visible'): break
            await asyncio.sleep(.25)
        assert media_state and media_state['panel_visible'], media_state
        assert media_state['players'] == [], 'Without OCR identity, physical rows stay internal'
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
        (OUT / 'no-hud-timing.json').write_text(json.dumps([
            {k: x.get(k) for k in ['status', 'sequence', 'observed_at', 'panel_visible']}
            for x in samples], indent=2), encoding='utf8')
        assert all(x['status'] == 'online' for x in samples), [(x['status'], x['sequence']) for x in samples]
        assert samples[-1]['sequence'] - samples[0]['sequence'] >= 5
        assert not samples[-1]['panel_visible']
        assert samples[-1]['players'][0]['stats'] == s['players'][0]['stats']
        assert 'stats.damage' in samples[-1]['players'][0]['stale_fields']
        async def new_match(file):
            # These fixtures are from different matches. Explicitly reset the
            # session; identity protection must not silently replace unread names.
            previous_session = (await state()).get('session_id')
            await request('SetSourceFilterSettings', sourceName='Fixture', filterName='Analyzer', filterSettings={'enabled': False})
            # OBS applies source settings asynchronously. Wait for the worker's
            # acknowledgment, otherwise false/true updates can be coalesced.
            for _ in range(100):
                await asyncio.sleep(.1)
                if (await state()).get('status') == 'stopped': break
            else:
                raise AssertionError('Worker did not acknowledge disabled filter')
            await request('SetInputSettings', inputName='Fixture', inputSettings={'file': str(file)})
            await asyncio.sleep(.3)
            await request('SetSourceFilterSettings', sourceName='Fixture', filterName='Analyzer', filterSettings={'enabled': True})
            for _ in range(50):
                await asyncio.sleep(.2)
                current = await state()
                if current.get('status') == 'online' and current.get('session_id') != previous_session:
                    return
            failure = {'state': await state(), 'filter': await request('GetSourceFilter', sourceName='Fixture', filterName='Analyzer'),
                       'input': await request('GetInputSettings', inputName='Fixture')}
            (OUT / 'reset-failure.json').write_text(json.dumps(failure,ensure_ascii=False,indent=2),encoding='utf8')
            raise AssertionError(('New match did not start a new session', failure['state'].get('status'), str(file)))
        await request('SetInputSettings', inputName='Fixture', inputSettings={'file': str(ROOT / 'tests/data/scoreboard/live-new-heroes-hud.png')})
        for _ in range(50):
            await asyncio.sleep(.2)
            live_hud = await state()
            if live_hud['status'] == 'online' and not live_hud['panel_visible']: break
        assert live_hud['status'] == 'online' and not live_hud['panel_visible']
        await new_match(ROOT / 'tests/data/scoreboard/header-touching-portrait.png')
        live_charge = [24,35,42,31,27,33,40,53,35,25]
        for _ in range(50):
            await asyncio.sleep(.2)
            live_panel = await state()
            if (live_panel['panel_visible'] and len(live_panel['players']) == 10 and
                    [p['ultimate_charge'] for p in live_panel['players']] == live_charge): break
        assert live_panel['panel_visible']
        assert [p['ultimate_charge'] for p in live_panel['players']] == live_charge, live_panel
        # A 2560px source exercises high-resolution OBS readback and detail crops.
        await new_match(ROOT / 'tests/data/scoreboard/stability-single-perk.png')
        stat_keys = ['eliminations', 'assists', 'deaths', 'damage', 'healing', 'mitigated']
        def stats_match(value):
            rows = value.get('players', [])
            return len(rows) == 10 and all(
                [p['stats'][k] for k in stat_keys] == expected['stability-single-perk']['stats'][i]
                for i, p in enumerate(rows))
        for _ in range(150):
            await asyncio.sleep(.2)
            stability = await state()
            if (stats_match(stability) and (stability['players'][6]['perks']['minor'] or {}).get('id') == 'Ulterior Motive'
                    and all(p['presence_status'] == 'present' for p in stability['players'])): break
        assert stats_match(stability), [('row', i, p['stats']) for i, p in enumerate(stability['players'])]
        assert (stability['players'][6]['perks']['minor'] or {}).get('id') == 'Ulterior Motive'
        # Historical heroes can only be attributed across a visual transition
        # after their original player identity has actually been established.
        assert all(p['presence_status'] == 'present' for p in stability['players']), stability
        (OUT / 'stability-obs-state.json').write_text(json.dumps(stability,ensure_ascii=False,indent=2),encoding='utf8')
        # The connected-digits fixture has LOWER totals than the preceding
        # screenshot. After the interruption, accept the newly confirmed values
        # without requiring them to catch up to the old counters.
        await request('SetInputSettings', inputName='Fixture', inputSettings={'file': str(negative_path)})
        for _ in range(75):
            await asyncio.sleep(.2)
            gap = await state()
            if gap['status'] == 'online' and not gap['panel_visible']: break
        assert not gap['panel_visible'], gap
        await request('SetInputSettings', inputName='Fixture', inputSettings={'file': str(ROOT / 'tests/data/scoreboard/stability-connected-digits.png')})
        def connected_match(value, history=True):
            rows = value.get('players', [])
            e = expected['stability-connected-digits']
            if len(rows) != 10: return False
            for i, p in enumerate(rows):
                old = expected['stability-single-perk']['stats'][i]
                totals = e['stats'][i]
                retained = set()
                fresh = set(['stats.' + k for k in stat_keys] + ['life_state'] + ([] if e['life'][i] == 'dead' else ['hero'])) - retained
                if (p['hero'] != (e['heroes'][i] if history or e['life'][i] != 'dead' else None)
                        or p['life_state'] != e['life'][i] or [p['stats'][k] for k in stat_keys] != totals
                        or any(k in p['stale_fields'] for k in fresh)
                        or not retained.issubset(p['stale_fields'])): return False
            return True
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
        await new_match(ROOT / 'tests/data/scoreboard/unselected-panel.png')
        for _ in range(75):
            await asyncio.sleep(.2)
            preselection = await state()
            if (preselection['panel_visible'] and len(preselection['players']) == 10 and
                    all(p.get('hero_selection') == 'unselected' for p in preselection['players']) and
                    preselection['players'][0]['stats']['damage'] == 5514): break
        assert preselection['panel_visible']
        for p in preselection['players']:
            assert p['hero_selection'] == 'unselected' and p['hero'] is None, p
            assert p['life_state'] is None and p['ultimate_charge'] is None, p
            assert p['perks']['minor'] is None
            assert 'hero_selection' not in p['stale_fields']
        import jsonschema
        jsonschema.validate(preselection, json.loads((ROOT / 'schema/player-state-v4.schema.json').read_text()))
        (OUT / 'unselected-obs-state.json').write_text(json.dumps(preselection,ensure_ascii=False,indent=2),encoding='utf8')
        await new_match(ROOT / 'tests/data/scoreboard/stability-connected-digits.png')
        for _ in range(75):
            await asyncio.sleep(.2)
            selected = await state()
            if connected_match(selected, history=False): break
        assert connected_match(selected, history=False), selected
        await new_match(ROOT / 'tests/data/interruptions/disconnected.png')
        for _ in range(100):
            await asyncio.sleep(.2)
            disconnected = await state()
            if (disconnected['panel_visible'] and len(disconnected['players']) == 9
                    and all(p['presence_status'] == 'present' for p in disconnected['players'])): break
        assert disconnected['panel_visible'] and len(disconnected['players']) == 9, disconnected
        assert all(p['display_name'] for p in disconnected['players'][:9]), disconnected
        expected_names = ['支援1', '支援2', '输出1', '输出2', '重装1', '支援3', '输出3', '输出4', '重装2']
        assert [p['display_name'] for p in disconnected['players'][:9]] == expected_names, disconnected
        identity_samples = []
        # Cross at least two periodic OCR verification intervals in real OBS.
        for _ in range(55):
            await asyncio.sleep(.2)
            sample = await state()
            assert [p['display_name'] for p in sample['players'][:9]] == expected_names, sample
            identity_samples.append({'sequence': sample['sequence'], 'names': expected_names})
        assert len({s['sequence'] for s in identity_samples}) >= 20
        (OUT / 'name-stability.json').write_text(json.dumps(identity_samples, ensure_ascii=False), encoding='utf8')
        jsonschema.validate(disconnected, schema)
        def by_id(snapshot):
            rows = snapshot.get('players', [])
            result = {p['player_id']: p for p in rows}
            assert len(result) == len(rows), 'Every identity appears once'
            return result
        named_order = list(by_id(disconnected))
        named_ids = {'name:' + n for n in expected_names}
        assert set(by_id(disconnected)) == named_ids
        assert set(named_order) == named_ids
        assert all(p['presence_status'] == 'present' for p in by_id(disconnected).values())
        expected_teams = {'name:' + n: ('A' if i < 5 else 'B') for i, n in enumerate(expected_names)}
        assert {k: p['team'] for k, p in by_id(disconnected).items()} == expected_teams
        assert [p['team'] for p in disconnected['players']] == ['A'] * 5 + ['B'] * 4
        # Synthetic compaction of REAL scoreboard pixels: second blue player
        # departs, remaining rows shift up, empty slot moves to the bottom.
        original = cv2.imread(str(ROOT / 'tests/data/interruptions/disconnected.png'))
        board = cv2.resize(original, (1600, 1000))
        compacted_image = board.copy()
        for i in range(1, 4):
            y = 208 + 55 * i
            compacted_image[y:y+55,459:1142] = board[y+55:y+110,459:1142]
        compacted_image[428:483,459:1142] = board[767:822,459:1142]
        compacted_path = OUT / 'synthetic-compacted.png'
        assert cv2.imwrite(str(compacted_path), compacted_image)
        await request('SetInputSettings', inputName='Fixture', inputSettings={'file': str(compacted_path)})
        for _ in range(100):
            await asyncio.sleep(.2)
            compacted = await state()
            if by_id(compacted).get('name:支援2', {}).get('presence_status') == 'missing': break
        departed = by_id(compacted)['name:支援2']
        assert departed['presence_status'] == 'missing' and 'position' not in departed, compacted
        assert departed['team'] == 'A'
        assert {k: p['team'] for k, p in by_id(compacted).items()} == expected_teams
        assert departed['stats'] == by_id(disconnected)['name:支援2']['stats']
        assert set('stats.'+k for k in stat_keys).issubset(departed['stale_fields'])
        assert set(by_id(compacted)) == named_ids
        assert by_id(compacted)['name:输出1']['presence_status'] == 'present'
        assert by_id(compacted)['name:重装1']['presence_status'] == 'present'
        jsonschema.validate(compacted, schema)
        (OUT / 'named-compaction.json').write_text(json.dumps(compacted, ensure_ascii=False, indent=2), encoding='utf8')
        await request('SetInputSettings', inputName='Fixture', inputSettings={'file': str(ROOT / 'tests/data/interruptions/disconnected.png')})
        for _ in range(100):
            await asyncio.sleep(.2)
            rejoined = await state()
            if set(by_id(rejoined)) == named_ids and all(p['presence_status'] == 'present' for p in by_id(rejoined).values()): break
        assert list(by_id(rejoined)) == named_order and set(by_id(rejoined)) == named_ids
        assert all(p['presence_status'] == 'present' for p in by_id(rejoined).values()), rejoined
        assert by_id(rejoined)['name:支援2']['presence_status'] == 'present'
        assert {k: p['team'] for k, p in by_id(rejoined).items()} == expected_teams
        jsonschema.validate(rejoined, schema)
        if args.obs_pid:
            old_session = rejoined['session_id']
            invoked = subprocess.run(['powershell.exe', '-NoProfile', '-ExecutionPolicy', 'Bypass',
                                      '-File', str(ROOT / 'scripts/invoke-obs-clear.ps1'),
                                      '-ObsProcessId', str(args.obs_pid)],
                                     capture_output=True, text=True, timeout=30,
                                     creationflags=subprocess.CREATE_NO_WINDOW)
            assert invoked.returncode == 0 and 'CLEAR_INVOKED' in invoked.stdout, (invoked.stdout, invoked.stderr)
            for _ in range(100):
                await asyncio.sleep(.2)
                cleared = await state()
                if cleared.get('session_id') != old_session and set(by_id(cleared)) == named_ids: break
            assert cleared.get('session_id') != old_session and set(by_id(cleared)) == named_ids, cleared
            assert cleared['status'] == 'online'
            jsonschema.validate(cleared, schema)
            (OUT / 'clear-button-state.json').write_text(json.dumps(cleared, ensure_ascii=False, indent=2), encoding='utf8')
        await request('SetInputSettings', inputName='Fixture', inputSettings={'file': str(ROOT / 'tests/data/interruptions/paused.png')})
        for _ in range(100):
            await asyncio.sleep(.2)
            game_pause = await state()
            if (game_pause.get('game_paused') and game_pause['players'][0]['hero'] == 'moira'
                    and game_pause['players'][6]['hero'] == 'junkrat'
                    and 'hero' not in game_pause['players'][6]['stale_fields']): break
        assert game_pause['panel_visible'] and game_pause['game_paused'], game_pause
        assert len(game_pause['players']) == 9 and set(by_id(game_pause)) == named_ids
        assert by_id(game_pause)['name:重装1']['presence_status'] == 'unconfirmed' and 'stats.damage' in game_pause['players'][4]['stale_fields']
        assert game_pause['players'][6]['hero'] == 'junkrat' and 'hero' not in game_pause['players'][6]['stale_fields']
        jsonschema.validate(game_pause, schema)
        (OUT / 'interruption-obs-state.json').write_text(json.dumps(game_pause,ensure_ascii=False,indent=2),encoding='utf8')
        await request('SetSourceFilterSettings', sourceName='Fixture', filterName='Analyzer', filterSettings={'enabled': False})
        await asyncio.sleep(.5)
        paused = await state()
        assert paused['status'] == 'stopped'
        assert 'stats.damage' in paused['players'][0]['stale_fields']
        await request('RemoveInput', inputName='Fixture')
        await asyncio.sleep(.5)
        assert (await state())['players'] == []
        report = {'obs_version': version['obsVersion'], 'plugin_loaded': True, 'image_source': True, 'media_source': True, 'pixels_unchanged': True, 'http_websocket': True, 'pause_remove': True, 'search_remains_online': True, 'touching_header_panel': True, 'recognized_names': [p['display_name'] for p in s['players']]}
        report['connected_digits_and_death_portraits'] = True
        report['named_compaction_and_return'] = True
        report['clear_button_ui_and_resume'] = bool(args.obs_pid)
        report['schema_version'] = '4.0'
        report['hidden_retains_history_and_match_reset'] = True
        report['disconnect_empty_slot_and_pause_row_isolation'] = True
        report['unselected_hud_panel_and_selection_transition'] = 'public panel transitions; HUD detection covered by native tests'
        (OUT / 'native-integration-report.json').write_text(json.dumps(report, ensure_ascii=False, indent=2), encoding='utf8')
        print(json.dumps(report, ensure_ascii=False))
        await ws.close()

asyncio.run(main())
