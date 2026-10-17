"""Run the integration suite in a prepared, disposable OBS portable directory."""
import argparse
import ctypes
from ctypes import wintypes
import json
from pathlib import Path
import socket
import subprocess
import sys
import time
import urllib.request
import urllib.error

ROOT = Path(__file__).resolve().parents[1]


def close_obs(process):
    user32 = ctypes.windll.user32
    callback_type = ctypes.WINFUNCTYPE(wintypes.BOOL, wintypes.HWND, wintypes.LPARAM)
    user32.PostMessageW.argtypes = [wintypes.HWND, wintypes.UINT, wintypes.WPARAM, wintypes.LPARAM]

    @callback_type
    def close_window(hwnd, _):
        pid = wintypes.DWORD()
        user32.GetWindowThreadProcessId(hwnd, ctypes.byref(pid))
        if pid.value == process.pid:
            user32.PostMessageW(hwnd, 0x0010, 0, 0)  # WM_CLOSE
        return True

    user32.EnumWindows(close_window, 0)
    try:
        return process.wait(timeout=25)
    except subprocess.TimeoutExpired:
        process.kill()
        process.wait()
        raise AssertionError('Isolated OBS did not shut down cleanly')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--obs-dir', required=True, type=Path)
    parser.add_argument('--version', required=True)
    parser.add_argument('--websocket-port', type=int, default=4457)
    parser.add_argument('--api-port', type=int, default=8766)
    args = parser.parse_args()
    host = args.obs_dir.resolve()
    assert host.is_relative_to(ROOT / '.diagnostics'), 'Use a disposable directory under .diagnostics'
    marker = host / '.owcs-compat-test'
    assert marker.is_file(), 'Create .owcs-compat-test only in a disposable test copy'
    for port in [args.websocket_port, args.api_port]:
        with socket.socket() as sock:
            sock.bind(('127.0.0.1', port))
    config = host / 'config/obs-studio'

    def write(relative, value):
        path = config / relative
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(value, encoding='utf8')

    write('global.ini', '[General]\nFirstRun=true\nEnableAutoUpdates=false\nConfirmOnExit=false\n'
          '[Basic]\nProfile=Test\nProfileDir=Test\nSceneCollection=Test\nSceneCollectionFile=Test\n'
          '[BasicWindow]\nPreviewEnabled=true\n[Video]\nRenderer=Direct3D 11\n'
          '[OBSWebSocket]\nFirstLoad=false\nServerEnabled=true\nAuthRequired=false\n'
          f'AlertsEnabled=false\nServerPort={args.websocket_port}\n')
    write('basic/profiles/Test/basic.ini', '[General]\nName=Test\n[Video]\nBaseCX=1600\nBaseCY=1000\n'
          'OutputCX=1600\nOutputCY=1000\nFPSType=0\nFPSCommon=30\n'
          '[Audio]\nSampleRate=48000\nChannelSetup=Stereo\n')
    write('basic/scenes/Test.json', json.dumps({
        'name': 'Test', 'current_scene': 'Test', 'current_program_scene': 'Test',
        'scene_order': [{'name': 'Test'}],
        'sources': [{'name': 'Test', 'id': 'scene', 'settings': {'items': []}}]}))
    write('plugin_config/obs-websocket/config.json', json.dumps({
        'alerts_enabled': False, 'auth_required': False, 'first_load': False,
        'server_enabled': True, 'server_port': args.websocket_port}))
    write('plugin_config/owcs-ob-analyzer/settings.json', json.dumps({'port': args.api_port, 'lan': False}))
    (host / 'portable_mode.txt').touch()
    (host / 'disable_updater').touch()
    output = host / 'verification'
    output.mkdir(exist_ok=True)
    startup = subprocess.STARTUPINFO()
    startup.dwFlags |= subprocess.STARTF_USESHOWWINDOW
    startup.wShowWindow = 0
    process = subprocess.Popen([str(host / 'bin/64bit/obs64.exe'), '--portable', '--multi',
                                '--disable-updater', '--disable-shutdown-check',
                                '--profile', 'Test', '--collection', 'Test'],
                               cwd=host / 'bin/64bit', startupinfo=startup)
    try:
        for _ in range(90):
            assert process.poll() is None, f'OBS exited early: {process.returncode}'
            try:
                with urllib.request.urlopen(f'http://127.0.0.1:{args.api_port}/v1/state', timeout=1):
                    break
            except urllib.error.HTTPError as error:
                if error.code == 503:  # Server is ready; no video filter exists yet.
                    break
                raise
            except (OSError, TimeoutError):
                time.sleep(1)
        else:
            raise AssertionError('Plugin HTTP server did not start')
        for _ in range(30):
            try:
                with socket.create_connection(('127.0.0.1', args.websocket_port), timeout=1):
                    break
            except OSError:
                time.sleep(1)
        else:
            raise AssertionError('OBS WebSocket server did not start')
        subprocess.run([sys.executable, str(ROOT / 'scripts/test-obs-integration.py'),
                        '--websocket-port', str(args.websocket_port), '--api-port', str(args.api_port),
                        '--output', str(output), '--expect-obs-version', args.version],
                       check=True, timeout=240)
    finally:
        exit_code = close_obs(process) if process.poll() is None else process.returncode
    assert exit_code == 0, f'OBS shutdown exit code: {exit_code}'
    report_path = output / 'native-integration-report.json'
    report = json.loads(report_path.read_text(encoding='utf8'))
    report['clean_shutdown'] = True
    report_path.write_text(json.dumps(report, ensure_ascii=False, indent=2), encoding='utf8')
    print(f'PASS: OBS {args.version}, integration and clean shutdown; {report_path}')


if __name__ == '__main__':
    main()
