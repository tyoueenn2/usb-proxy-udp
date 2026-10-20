import socket
import struct
import threading
import unittest
from client import MouseProxy


class Peer:
    def __init__(self):
        self.sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        self.sock.bind(('127.0.0.1', 0))
        self.sock.settimeout(.02)
        self.stop = threading.Event()
        self.requests = {}
        self.executions = 0
        self.clicks = 0
        self.thread = threading.Thread(target=self.run)
        self.thread.start()

    def run(self):
        while not self.stop.is_set():
            try:
                data, source = self.sock.recvfrom(1024)
            except socket.timeout:
                continue
            if data[:4] == b'UPQ1':
                session, token = struct.unpack_from('!QQ', data, 8)
                self.sock.sendto(struct.pack('!4sBBBBQQQQIiiii4xQI4x', b'UPI1', 1, 1, 7, 0,
                    session, token, 123, 456, 1000, -127, 127, -127, 127, 789, 32), source)
            elif data[:4] == b'UPM1':
                (_, _, op, flags, button, session, command, epoch, generation, dx, dy,
                 *_) = struct.unpack('!4sBBBBQQQQiiIIIII12x', data)
                key = session, command
                normalized = data[:5] + b'\x01' + data[6:]
                if key not in self.requests:
                    self.requests[key] = normalized
                    self.executions += 1
                    continue  # Lose the first request's response: the retry must not execute again.
                if normalized != self.requests[key]:
                    raise AssertionError('Retry payload changed')
                waiting = flags & 1 and op == 1
                status = 2 if waiting else 11 if op == 3 else 3
                pressed = 6 if button and status == 3 else 0
                if pressed:
                    self.clicks += 1
                self.sock.sendto(struct.pack('!4sBBBBQQQQiiiiiiQ24x', b'UPR1', 1, status,
                    3 if waiting else 9, pressed, session, command, epoch, generation,
                    dx, dy, 0 if waiting else -dx, 0 if waiting else -dy,
                    dx if waiting else 0, dy if waiting else 0, 1), source)

    def close(self):
        self.stop.set()
        self.thread.join(1)
        self.sock.close()


class MoveClientTests(unittest.TestCase):
    def test_retry_and_verified_midpoint(self):
        peer = Peer()
        try:
            mouse = MouseProxy('127.0.0.1', peer.sock.getsockname()[1])
            called = []
            result = mouse.move_action_return(300, -200, on_midpoint=lambda ack: called.append(ack['outward']))
            self.assertEqual(result['status'], 'completed')
            self.assertEqual(result['remaining'], (0, 0))
            self.assertEqual(called, [(300, -200)])
            self.assertEqual(peer.executions, 1)
            self.assertEqual(peer.clicks, 1)
            mouse.socket.close()
        finally:
            peer.close()

    def test_failed_verification_skips_click(self):
        peer = Peer()
        try:
            mouse = MouseProxy('127.0.0.1', peer.sock.getsockname()[1])
            result = mouse.move_action_return(20, 0, on_midpoint=lambda ack: False)
            self.assertEqual(result['status'], 'cancelled')
            self.assertEqual(result['remaining'], (0, 0))
            self.assertEqual(peer.clicks, 0)
            mouse.socket.close()
        finally:
            peer.close()


if __name__ == '__main__':
    unittest.main()
