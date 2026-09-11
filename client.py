"""UDP mouse snapshots for the Pi proxy. One persistent socket per controller."""
import socket
import struct
import secrets
import time


CLICK_STATUS = {
    1: 'accepted', 2: 'duplicate', 3: 'completed', 4: 'busy',
    5: 'queue_full', 6: 'unsupported_button', 7: 'invalid',
    8: 'cancelled', 9: 'button_active', 10: 'not_ready',
    11: 'stale_command',
}
_CLICK_REQUEST = struct.Struct('!4sBBBBQQIIII')
_CLICK_ACK = struct.Struct('!4sBBBBQQIIQHHI')


class MouseProxy:
    def __init__(self, host, port=12345):
        self.socket = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        self.socket.connect((host, port))
        self.socket.settimeout(0.5)
        self.sequence = 0
        self.buttons = 0
        self.session = secrets.randbits(64) or 1
        self.command = 0
        self._acks = []

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
        while True:
            packet = self.socket.recv(1024)
            ack = self._parse_ack(packet)
            if ack:
                self._acks.append(ack)
            else:
                return packet.decode('ascii')

    @staticmethod
    def _parse_ack(packet):
        if len(packet) != _CLICK_ACK.size or packet[:4] != b'UPA1':
            return None
        fields = _CLICK_ACK.unpack(packet)
        if fields[1] != 1 or fields[4] or fields[11] or fields[12]:
            return None
        return {
            'status': CLICK_STATUS.get(fields[2], f'unknown_{fields[2]}'),
            'status_code': fields[2], 'button': fields[3],
            'session': fields[5], 'command': fields[6],
            'accepted_clicks': fields[7], 'completed_clicks': fields[8],
            'server_epoch': fields[9], 'queue_depth': fields[10],
        }

    def _next_command(self):
        self.command += 1
        return self.command

    def _reliable_request(self, packet, command, wait_complete, timeout, retry_interval):
        deadline = time.monotonic() + timeout
        terminal = {'completed', 'cancelled', 'busy', 'queue_full',
                    'unsupported_button', 'invalid', 'button_active',
                    'not_ready', 'stale_command'}
        accepted = None
        next_send = 0.0
        while time.monotonic() < deadline:
            now = time.monotonic()
            if now >= next_send:
                self.socket.send(packet)  # same ID: retry is idempotent within this Pi epoch
                next_send = now + retry_interval
            for i, ack in enumerate(self._acks):
                if ack['session'] == self.session and ack['command'] == command:
                    self._acks.pop(i)
                    break
            else:
                self.socket.settimeout(max(0.001, min(next_send, deadline) - time.monotonic()))
                try:
                    incoming = self.socket.recv(1024)
                except socket.timeout:
                    continue
                ack = self._parse_ack(incoming)
                if not ack:
                    continue
                if ack['session'] != self.session or ack['command'] != command:
                    self._acks.append(ack)
                    continue
            if ack['status'] in ('accepted', 'duplicate'):
                accepted = ack
                if not wait_complete:
                    return ack
                continue
            if ack['status'] in terminal:
                return ack
        if accepted:
            raise TimeoutError(f"click command {command} was accepted but no completion status arrived")
        raise TimeoutError(f"no acknowledgment for click command {command}")

    def schedule_clicks(self, button=1, count=1, press_ms=8, interval_ms=20,
                        wait_complete=True, timeout=5.0, retry_interval=0.05):
        """Reliably ask the Pi to schedule clicks. interval_ms is press-to-press.

        Retries reuse the same command ID, so a lost acknowledgment does not
        schedule the sequence twice. Waiting also renews the 250 ms controller
        lease. The returned dict reports accepted and completed counts separately.
        """
        command = self._next_command()
        packet = _CLICK_REQUEST.pack(
            b'UPC1', 1, 1, button, 0, self.session, command, count,
            round(press_ms * 1000), round(interval_ms * 1000), 0)
        return self._reliable_request(packet, command, wait_complete, timeout, retry_interval)

    def release_all(self, wait_complete=True, timeout=1.0, retry_interval=0.05):
        """Clear persistent holds, cancel scheduled clicks, and request a merged release."""
        command = self._next_command()
        packet = _CLICK_REQUEST.pack(
            b'UPC1', 1, 2, 0, 0, self.session, command, 0, 0, 0, 0)
        result = self._reliable_request(packet, command, wait_complete, timeout, retry_interval)
        self.buttons = 0
        return result

    def release(self):
        self.buttons = 0
        self.move()

    def close(self):
        try:
            try:
                self.release_all(timeout=0.5)
            except (OSError, TimeoutError):
                self.release()  # compatibility with proxy versions predating UPC1
        finally:
            self.socket.close()

    def __enter__(self):
        return self

    def __exit__(self, *_):
        self.close()
