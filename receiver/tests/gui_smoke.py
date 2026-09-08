"""Render the app's own hidden DirectX framebuffer; no desktop capture or UI automation."""
import argparse
import subprocess
import sys
from pathlib import Path
sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'tools'))
from mock_pi import MockPi
from test_sender import Sender

def main():
    p = argparse.ArgumentParser(); p.add_argument('executable'); a = p.parse_args()
    pi = MockPi().start(); sender = Sender().start()
    try:
        result = subprocess.run([str(Path(a.executable).resolve()), '--smoke-test'], timeout=15,
                                creationflags=getattr(subprocess, 'CREATE_NO_WINDOW', 0))
        assert result.returncode == 0, f'GUI smoke test failed: {result.returncode}'
        assert not pi.commands, 'GUI unexpectedly started armed'
        assert Path('gui-smoke.bmp').stat().st_size > 10000
        print('GUI smoke test passed: rendering, live preview, clock sync, telemetry, disarmed startup.')
    finally:
        sender.stop(); pi.stop()

if __name__ == '__main__':
    main()
