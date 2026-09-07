"""UDP mouse snapshots for the Pi proxy. One persistent socket per controller."""
import socket
import struct


class MouseProxy:
    def __init__(self, host, port=12345):
        self.socket = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        self.socket.connect((host, port))
        self.socket.settimeout(0.5)
        self.sequence = 0
        self.buttons = 0

    def move(self, dx=0, dy=0, wheel=0, pan=0):
        """Latest correction in relative HID counts. Send at least every 100 ms
        while holding buttons; move() with zero deltas is a heartbeat.
        Corrections not yet sent to USB may be replaced by newer corrections.
        """
        packet = struct.pack('!4sIhhbbBB', b'UPX1', self.sequence,
                             dx, dy, wheel, pan, self.buttons, 0)
        self.socket.send(packet)
        self.sequence = (self.sequence + 1) & 0xffffffff

    def button(self, number, down):
        if not 1 <= number <= 8:
            raise ValueError('button number must be 1..8')
        old = self.buttons
        self.buttons = (old | (1 << (number - 1))) if down else (old & ~(1 << (number - 1)))
        try:
            self.move()
        except Exception:
            self.buttons = old
            raise

    def state(self):
        """Query processed physical/synthetic state and native X/Y limits.
        The result describes proxy report state, not OS cursor coordinates.
        """
        self.socket.send(b'+state')
        return self.socket.recv(1024).decode('ascii')

    def release(self):
        self.buttons = 0
        self.move()

    def close(self):
        try:
            self.release()
        finally:
            self.socket.close()

    def __enter__(self):
        return self

    def __exit__(self, *_):
        self.close()
