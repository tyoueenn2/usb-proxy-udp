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
        if fields[1] not in (1, 2) or fields[4] or fields[11] or fields[12]:
            return None
        return {
            'version': fields[1],
            'status': CLICK_STATUS.get(fields[2], f'unknown_{fields[2]}'),
            'status_code': fields[2], 'button': fields[3],
            'session': fields[5], 'command': fields[6],
            'accepted_clicks': fields[7], 'completed_clicks': fields[8],
            'server_epoch': fields[9], 'queue_depth': fields[10],
        }

    def _next_command(self):
        self.command += 1
        return self.command

    def _reliable_request(self, packet, command, wait_complete, timeout, retry_interval, protocol_version):
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
                if (ack['session'] == self.session and ack['command'] == command and
                        ack['version'] == protocol_version):
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
                if (ack['session'] != self.session or ack['command'] != command or
                        ack['version'] != protocol_version):
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
                        wait_complete=True, timeout=5.0, retry_interval=0.05,
                        protocol_version=1):
        """Reliably ask the Pi to schedule clicks.

        In protocol v1 interval_ms is press-to-press. In v2 it is the gap
        from successful release completion to submission of the next press.

        Retries reuse the same command ID, so a lost acknowledgment does not
        schedule the sequence twice. Waiting also renews the 250 ms controller
        lease. The returned dict reports accepted and completed counts separately.
        """
        if protocol_version not in (1, 2):
            raise ValueError('protocol_version must be 1 or 2')
        command = self._next_command()
        packet = _CLICK_REQUEST.pack(
            b'UPC1', protocol_version, 1, button, 0, self.session, command, count,
            round(press_ms * 1000), round(interval_ms * 1000), 0)
        return self._reliable_request(packet, command, wait_complete, timeout, retry_interval, protocol_version)

    def capabilities(self, timeout=1.0):
        """Negotiate move/return support without changing old telemetry formats."""
        token = secrets.randbits(64) or 1
        request = struct.pack('!4sB3xQQ', b'UPQ1', 1, self.session, token)
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            self.socket.send(request)
            self.socket.settimeout(min(0.05, max(0.001, deadline-time.monotonic())))
            try:
                packet = self.socket.recv(1024)
            except socket.timeout:
                continue
            if len(packet) != 80 or packet[:4] != b'UPI1' or packet[4] != 1:
                ack = self._parse_ack(packet)
                if ack:
                    self._acks.append(ack)
                continue
            session, echoed, epoch, generation = struct.unpack_from('!QQQQ', packet, 8)
            if session != self.session or echoed != token:
                continue
            poll, xmin, xmax, ymin, ymax = struct.unpack_from('!Iiiii', packet, 40)
            layout, segments = struct.unpack_from('!QI', packet, 64)
            if packet[5] > 1 or packet[7] or any(packet[60:64]) or any(packet[76:]):
                continue
            if not epoch or (packet[5] and (not generation or not layout or not poll or not xmin < 0 < xmax or not ymin < 0 < ymax or segments != 32)):
                continue
            return dict(ready=bool(packet[5]), buttons=packet[6], epoch=epoch,
                        generation=generation, poll_us=poll, xmin=xmin, xmax=xmax,
                        ymin=ymin, ymax=ymax, layout=layout, max_segments=segments)
        raise TimeoutError('Pi has no move/return capability response; update the proxy')

    @staticmethod
    def _parse_move_ack(packet):
        if len(packet) != 96 or packet[:4] != b'UPR1' or packet[4] != 1:
            return None
        if not 1 <= packet[5] <= 15 or packet[6] > 9 or packet[7] & ~7 or any(packet[72:]):
            return None
        session, command, epoch, generation = struct.unpack_from('!QQQQ', packet, 8)
        ox, oy, rx, ry, remaining_x, remaining_y, written = struct.unpack_from('!iiiiiiQ', packet, 40)
        names = ('unknown', 'accepted', 'active', 'completed', 'busy', 'queue_full',
                 'invalid', 'not_ready', 'stale', 'button_active', 'interrupted',
                 'cancelled', 'timeout', 'writer_failed', 'endpoint_changed', 'range')
        return dict(status=names[packet[5]], status_code=packet[5], stage=packet[6],
                    uncertain=bool(packet[7] & 1), pressed=bool(packet[7] & 2),
                    released=bool(packet[7] & 4), session=session, command=command,
                    epoch=epoch, generation=generation, outward=(ox, oy), returned=(rx, ry),
                    remaining=(remaining_x, remaining_y), written_ns=written)

    def move_action_return(self, dx, dy, button=1, settle_ms=2, press_ms=10,
                           return_ms=0, timeout=6.0, on_midpoint=None):
        """One ordered Pi operation in HID counts. button=0 performs no click.

        on_midpoint optionally verifies external position before the click. It must
        finish within one second; return False to skip the click and return.
        Retries use one immutable payload and command ID, never repeat the action.
        Completion proves USB-writer success, not application consumption.
        """
        caps = self.capabilities()
        if not caps['ready']:
            raise RuntimeError('Pi mouse endpoint is not ready')
        if not -32767 <= dx <= 32767 or not -32767 <= dy <= 32767 or not 0 <= button <= 8:
            raise ValueError('Invalid movement or button')
        timing = (round(settle_ms*1000), max(round(press_ms*1000), caps['poll_us']), round(return_ms*1000))
        if any(n < 0 or n > 1_000_000 for n in timing):
            raise ValueError('Timing must be between zero and 1000 milliseconds')
        command = self._next_command()
        base = bytearray(struct.pack('!4sBBBBQQQQiiIIIII12x', b'UPM1', 1, 1,
            int(on_midpoint is not None), button, self.session, command, caps['epoch'],
            caps['generation'], dx, dy, *timing, 5_000_000, 1_500_000))
        deadline = time.monotonic() + timeout
        next_send = 0.0
        handled_midpoint = False
        callback_error = None
        while time.monotonic() < deadline:
            now = time.monotonic()
            if now >= next_send:
                self.socket.send(base)
                next_send = now + 0.02
            self.socket.settimeout(max(0.001, min(next_send, deadline)-time.monotonic()))
            try:
                incoming = self.socket.recv(1024)
            except socket.timeout:
                continue
            ack = self._parse_move_ack(incoming)
            if not ack or ack['session'] != self.session or ack['command'] != command:
                continue
            if ack['epoch'] != caps['epoch'] or ack['generation'] != caps['generation']:
                raise RuntimeError('Pi restarted during the operation; return is unverified')
            if ack['status_code'] in (4, 5):
                continue  # Rejected before admission; retry the same request.
            if ack['status_code'] > 2:
                if callback_error:
                    raise callback_error
                return ack
            if ack['stage'] == 3 and on_midpoint is not None and not handled_midpoint:
                handled_midpoint = True
                try:
                    proceed = on_midpoint(ack) is not False
                except Exception as error:
                    callback_error = error
                    proceed = False
                base[5] = 2 if proceed else 3
                next_send = 0.0
        # ReleaseAll fences this command, so delayed Execute packets cannot repeat it.
        try:
            self.release_all(timeout=0.5, protocol_version=2)
        except (OSError, TimeoutError):
            pass
        raise TimeoutError('Move/return completion was not received; position is unverified')

    def release_all(self, wait_complete=True, timeout=1.0, retry_interval=0.05,
                    protocol_version=1):
        """Clear persistent holds, cancel scheduled clicks, and request a merged release."""
        if protocol_version not in (1, 2):
            raise ValueError('protocol_version must be 1 or 2')
        command = self._next_command()
        packet = _CLICK_REQUEST.pack(
            b'UPC1', protocol_version, 2, 0, 0, self.session, command, 0, 0, 0, 0)
        result = self._reliable_request(packet, command, wait_complete, timeout, retry_interval, protocol_version)
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
