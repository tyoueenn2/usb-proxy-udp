"""Loopback-only UDP Pi simulator. Never accesses USB or the OS mouse."""
import argparse
import json
import secrets
import socket
import threading
import time
from wire import MOVE, SUBSCRIBE, TELEMETRY, newer


class MockPi:
    def __init__(self, port=12345, physical=2):
        self.sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        self.sock.bind(('127.0.0.1', port)); self.sock.settimeout(.002)
        self.physical, self.ready = physical, True
        self.telemetry_enabled = True
        self.commands, self.transitions = [], []
        self.stop_event = threading.Event()
        self.session = secrets.randbits(64) or 1
        self.thread = threading.Thread(target=self.run, daemon=True)

    def start(self):
        self.thread.start(); return self

    def stop(self):
        self.stop_event.set(); self.thread.join(timeout=2); self.sock.close()

    def run(self):
        subscriber = None; client = token = 0; renewed = published = 0
        seq = 0; previous = None; owner = None; lease = 0; move_seq = None
        while not self.stop_event.is_set():
            now = time.perf_counter()
            if owner and now - lease > .25:
                owner = None; move_seq = None
            try:
                data, source = self.sock.recvfrom(2048)
                if len(data) == SUBSCRIBE.size and data[:4] == b'UPS1':
                    _, reserved, c, t = SUBSCRIBE.unpack(data)
                    if not reserved and c and t and (subscriber is None or source == subscriber or now - renewed > .1):
                        if c != client or now - renewed > .1 or t > token:
                            if c != client:
                                seq = 0
                            client, token, subscriber, renewed, published = c, t, source, now, 0
                elif len(data) == MOVE.size and data[:4] == b'UPX1':
                    _, ms, dx, dy, wheel, pan, buttons, reserved = MOVE.unpack(data)
                    if owner is not None and owner != source:
                        self.sock.sendto(b'busy', source)
                    elif not self.ready:
                        self.sock.sendto(b'error not_ready_range_or_queue_full', source)
                    elif not reserved and (move_seq is None or newer(ms, move_seq)):
                        owner, lease, move_seq = source, now, ms
                        self.commands.append(dict(time=now, sequence=ms, dx=dx, dy=dy, buttons=buttons,
                                                  physical=self.physical, combined=buttons | self.physical))
                elif data == b'+state':
                    self.sock.sendto(f'state {self.physical} 0 {self.physical} -127 127 -127 127'.encode() if self.ready else b'not_ready', source)
            except (socket.timeout, ConnectionResetError):
                pass
            now = time.perf_counter()
            state = (self.ready, self.physical)
            if subscriber and self.telemetry_enabled and now - renewed <= .1 and (now - published >= .01 or state != previous):
                self.sock.sendto(TELEMETRY.pack(b'UPT1', int(self.ready), self.physical, 0, client, self.session, token,
                                              seq, -127, 127, -127, 127, 0), subscriber)
                seq = (seq + 1) & 0xffffffff; published = now; previous = state


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--port', type=int, default=12345); p.add_argument('--buttons', type=int, default=2)
    p.add_argument('--seconds', type=float, default=30); p.add_argument('--log', default='mock_commands.json')
    p.add_argument('--release-after', type=float, default=-1)
    a = p.parse_args()
    if not 0 <= a.buttons <= 255:
        p.error('Button mask must be 0..255')
    pi = MockPi(a.port, a.buttons).start(); started = time.perf_counter()
    try:
        while time.perf_counter() - started < a.seconds:
            if a.release_after >= 0 and time.perf_counter() - started >= a.release_after:
                pi.physical = 0
            time.sleep(.01)
    except KeyboardInterrupt:
        pass
    finally:
        pi.stop()
    with open(a.log, 'w') as f:
        json.dump(pi.commands, f, indent=2)
    print(f'Recorded {len(pi.commands)} commands; no real mouse output occurred.')


if __name__ == '__main__':
    main()
