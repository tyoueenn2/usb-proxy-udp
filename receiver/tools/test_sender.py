"""Synthetic/raw-file frame sender; deliberately does not capture the desktop."""
import argparse
import random
import secrets
import socket
import threading
import time
from pathlib import Path
from wire import HELLO, SYNC_REQUEST, SYNC_REPLY, fragments


class Sender:
    def __init__(self, host='127.0.0.1', port=5000, width=320, height=320, fps=120,
                 pixels=None, pixel_format=1, loss=0, duplicate=0, reorder=False,
                 clock_offset_ms=0, stamp_delay_ms=0):
        self.target = (socket.gethostbyname(host), port)
        self.width, self.height, self.fps = width, height, fps
        self.pixel_format = pixel_format
        self.pixels = pixels if pixels is not None else self.pattern(width, height, pixel_format)
        self.loss, self.duplicate, self.reorder = loss, duplicate, reorder
        self.offset = int(clock_offset_ms * 1e6)
        self.stamp_delay = int(stamp_delay_ms * 1e6)
        self.session = secrets.randbits(64) or 1
        self.sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        self.sock.bind(('0.0.0.0', 0))
        self.sock.settimeout(.01)
        self.stop_event = threading.Event()
        self.paused = False
        self.frames = 0
        self.threads = []

    @staticmethod
    def pattern(width, height, fmt):
        rgb = bytearray(width * height * (3 if fmt == 1 else 4))
        c = 3 if fmt == 1 else 4
        for y in range(height):
            for x in range(width):
                color = (x * 255 // width, y * 255 // height, 80)
                rgb[(y * width + x) * c:(y * width + x + 1) * c] = bytes(color if c == 3 else (color[2], color[1], color[0], 255))
        return bytes(rgb)

    def stamp(self):
        return time.perf_counter_ns() + self.offset

    def start(self):
        self.threads = [threading.Thread(target=self.clock_loop, daemon=True), threading.Thread(target=self.frame_loop, daemon=True)]
        for thread in self.threads:
            thread.start()
        return self

    def stop(self):
        self.stop_event.set()
        for thread in self.threads:
            thread.join(timeout=2)
        self.sock.close()

    def clock_loop(self):
        while not self.stop_event.is_set():
            try:
                packet, source = self.sock.recvfrom(2048)
                received = self.stamp()
                if source != self.target or len(packet) != SYNC_REQUEST.size:
                    continue
                magic, reserved, session, token = SYNC_REQUEST.unpack(packet)
                if magic == b'UVC1' and reserved == 0 and session == self.session:
                    self.sock.sendto(SYNC_REPLY.pack(b'UVS1', 0, session, token, received, self.stamp()), source)
            except (socket.timeout, ConnectionResetError):
                pass
            except OSError:
                if not self.stop_event.is_set():
                    raise

    def frame_loop(self):
        rng = random.Random(21)
        sequence, last_hello = 0, 0
        next_frame = time.perf_counter()
        while not self.stop_event.is_set():
            now = time.perf_counter()
            if now - last_hello >= .05:
                self.sock.sendto(HELLO.pack(b'UVH1', 0, self.session), self.target)
                last_hello = now
            if not self.paused:
                packets = list(fragments(self.pixels, self.width, self.height, self.session, sequence,
                                        self.stamp() - self.stamp_delay, self.pixel_format))
                if self.reorder:
                    rng.shuffle(packets)
                for packet in packets:
                    if rng.random() < self.loss:
                        continue
                    self.sock.sendto(packet, self.target)
                    if rng.random() < self.duplicate:
                        self.sock.sendto(packet, self.target)
                sequence = (sequence + 1) & 0xffffffff
                self.frames += 1
            next_frame += 1 / self.fps
            wait = next_frame - time.perf_counter()
            if wait < -1 / self.fps:
                next_frame = time.perf_counter()
            self.stop_event.wait(max(0, wait))


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--host', default='127.0.0.1'); p.add_argument('--port', type=int, default=5000)
    p.add_argument('--width', type=int, default=320); p.add_argument('--height', type=int, default=320)
    p.add_argument('--fps', type=float, default=120); p.add_argument('--seconds', type=float, default=30)
    p.add_argument('--raw', type=Path); p.add_argument('--bgra', action='store_true')
    p.add_argument('--loss', type=float, default=0); p.add_argument('--duplicate', type=float, default=0)
    p.add_argument('--reorder', action='store_true'); p.add_argument('--clock-offset-ms', type=float, default=0)
    p.add_argument('--stamp-delay-ms', type=float, default=0)
    a = p.parse_args()
    if not (1 <= a.width <= 1024 and 1 <= a.height <= 1024 and 1 <= a.fps <= 1000 and 0 <= a.loss <= 1 and 0 <= a.duplicate <= 1):
        p.error('Invalid dimensions, FPS, or fault probabilities')
    fmt = 2 if a.bgra else 1
    pixels = a.raw.read_bytes() if a.raw else None
    if pixels is not None and len(pixels) != a.width * a.height * (4 if a.bgra else 3):
        p.error('Raw file length does not match dimensions/format')
    sender = Sender(a.host, a.port, a.width, a.height, a.fps, pixels, fmt, a.loss, a.duplicate, a.reorder, a.clock_offset_ms, a.stamp_delay_ms).start()
    try:
        time.sleep(a.seconds)
    except KeyboardInterrupt:
        pass
    finally:
        sender.stop()
    print(f'Sent {sender.frames} frames. Synthetic sender throughput is not a desktop-capture benchmark.')


if __name__ == '__main__':
    main()
