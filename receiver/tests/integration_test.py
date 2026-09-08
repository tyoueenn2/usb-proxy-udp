"""Exercise the actual C++ receiver over loopback, including fail-closed output."""
import argparse
import json
import socket
import subprocess
import sys
import tempfile
import time
from pathlib import Path
sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'tools'))
from mock_pi import MockPi
from test_sender import Sender


def free_port():
    with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as s:
        s.bind(('127.0.0.1', 0)); return s.getsockname()[1]


def wait_for(predicate, description, timeout=3):
    until = time.perf_counter() + timeout
    while time.perf_counter() < until:
        if predicate():
            return
        time.sleep(.01)
    raise AssertionError(description)


def main():
    p = argparse.ArgumentParser(); p.add_argument('executable'); a = p.parse_args()
    with tempfile.TemporaryDirectory() as tmp:
        tmp = Path(tmp); frame_port, pi_port = free_port(), free_port()
        cfg = dict(version=1, frame_port=frame_port, pi_port=pi_port, sender_ip='127.0.0.1', pi_ip='127.0.0.1')
        profile = tmp / 'profile.json'; profile.write_text(json.dumps(cfg))
        metrics = tmp / 'metrics.csv'
        pi = MockPi(pi_port).start()
        sender = Sender(port=frame_port, width=160, height=160, fps=120, duplicate=.1, reorder=True, clock_offset_ms=-1500).start()
        process = subprocess.Popen([a.executable, '--profile', str(profile), '--simulate', '--arm', '--seconds', '8', '--metrics', str(metrics)], stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
        try:
            wait_for(lambda: len(pi.commands) >= 10, 'No active corrections received')
            assert all(c['dx'] > 0 and c['dy'] == 0 and c['buttons'] == 0 and c['combined'] == 2 for c in pi.commands)
            pi.physical = 0; time.sleep(.1); count = len(pi.commands); time.sleep(.15)
            assert len(pi.commands) == count, 'Output continued after physical release'
            pi.physical = 2; wait_for(lambda: len(pi.commands) > count + 5, 'Did not resume after physical activation')
            sender.paused = True; time.sleep(.1); count = len(pi.commands); time.sleep(.15)
            assert len(pi.commands) == count, 'Output repeated after capture stopped'
            sender.paused = False; wait_for(lambda: len(pi.commands) > count + 5, 'Did not recover after frames resumed')
            pi.telemetry_enabled = False; time.sleep(.1); count = len(pi.commands); time.sleep(.15)
            assert len(pi.commands) == count, 'Output continued after telemetry expired'
            pi.telemetry_enabled = True; wait_for(lambda: len(pi.commands) > count + 5, 'Did not recover after telemetry resumed')
            sender.stamp_delay = 200_000_000; time.sleep(.1); count = len(pi.commands); time.sleep(.15)
            assert len(pi.commands) == count, 'Old capture timestamp accepted despite fresh packets'
            sender.stamp_delay = 0; wait_for(lambda: len(pi.commands) > count + 5, 'Fresh capture did not recover')
            sender.stop(); sender = Sender(port=frame_port, width=320, height=160, fps=120, pixel_format=2, clock_offset_ms=2500).start()
            count = len(pi.commands); wait_for(lambda: len(pi.commands) > count + 5, 'New sender session/shape/clock did not recover')
            pi.ready = False; time.sleep(.1); count = len(pi.commands); time.sleep(.15)
            assert len(pi.commands) == count, 'Output continued after Pi became not ready'
            output = process.communicate(timeout=10)[0]
            assert process.returncode == 0 or (process.returncode == 1 and 'Pi: error not_ready_range_or_queue_full' in output), output
            assert metrics.exists() and 'receiver_to_submission' in metrics.read_text()
            print(output); print(f'Integration passed: {len(pi.commands)} commands, release, frame stall, telemetry expiry, old capture, restart, BGRA, readiness.')
        finally:
            if process.poll() is None:
                process.terminate(); print(process.communicate(timeout=5)[0])
            sender.stop(); pi.stop()
        # Default disarmed operation must never produce movement.
        pi = MockPi(pi_port).start(); sender = Sender(port=frame_port, width=160, height=160).start()
        try:
            result = subprocess.run([a.executable, '--profile', str(profile), '--simulate', '--seconds', '1', '--metrics', str(metrics)], capture_output=True, text=True, timeout=5)
            assert result.returncode == 0, result.stdout + result.stderr
            assert not pi.commands, 'Default disarmed run emitted movement'
        finally:
            sender.stop(); pi.stop()
        print('Default-disarmed integration passed.')


if __name__ == '__main__':
    main()
