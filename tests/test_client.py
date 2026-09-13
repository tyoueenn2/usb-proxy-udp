import socket
import struct
import threading
import unittest
from client import MouseProxy, _CLICK_REQUEST, _CLICK_ACK


class ClientTest(unittest.TestCase):
    def test_upx_wire_and_button_snapshots(self):
        with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as server:
            server.bind(('127.0.0.1', 0))
            server.settimeout(1)
            client = MouseProxy('127.0.0.1', server.getsockname()[1])
            try:
                client.sequence = 0xffffffff
                client.move(-320, 320, -1, 1)
                packet, peer = server.recvfrom(100)
                self.assertEqual(struct.unpack('!4sIhhbbBB', packet),
                                 (b'UPX1', 0xffffffff, -320, 320, -1, 1, 0, 0))
                client.button(1, True)
                packet, second_peer = server.recvfrom(100)
                self.assertEqual(peer, second_peer)
                self.assertEqual(struct.unpack('!4sIhhbbBB', packet)[1], 0)
                self.assertEqual(packet[14], 1)
                client.move(2, 3)
                self.assertEqual(server.recv(100)[14], 1)
                client.button(1, False)
                self.assertEqual(server.recv(100)[14], 0)
                with self.assertRaises(ValueError):
                    client.button(9, True)
            finally:
                client.socket.close()

    def test_click_retry_and_separate_counts(self):
        with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as server:
            server.bind(('127.0.0.1', 0));server.settimeout(1)
            requests = []
            def responder():
                for status, completed in ((1, 0), (2, 0), (3, 5)):
                    packet, peer = server.recvfrom(100);fields = _CLICK_REQUEST.unpack(packet);requests.append(fields)
                    ack = _CLICK_ACK.pack(b'UPA1', 1, status, fields[3], 0, fields[5], fields[6],
                                          fields[7], completed, 77, 1, 0, 0)
                    # Drop the first accepted ACK to force an identical same-ID retry.
                    if status != 1:
                        server.sendto(ack, peer)
            thread = threading.Thread(target=responder);thread.start()
            client = MouseProxy('127.0.0.1', server.getsockname()[1])
            try:
                result = client.schedule_clicks(1, 5, 8, 20, timeout=2, retry_interval=0.02)
                self.assertEqual(result['status'], 'completed')
                self.assertEqual(result['accepted_clicks'], 5)
                self.assertEqual(result['completed_clicks'], 5)
            finally:
                client.socket.close();thread.join(1)
            self.assertEqual(len(requests), 3)
            self.assertEqual({request[6] for request in requests}, {1})
            self.assertEqual({request[5] for request in requests}, {client.session})

    def test_v2_release_gap_request_and_ack_version(self):
        with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as server:
            server.bind(('127.0.0.1', 0));server.settimeout(1)
            captured = []
            def responder():
                packet, peer = server.recvfrom(100);fields = _CLICK_REQUEST.unpack(packet);captured.append(fields)
                for status, completed in ((1, 0), (3, 2)):
                    ack = _CLICK_ACK.pack(b'UPA1', fields[1], status, fields[3], 0, fields[5], fields[6],
                                          fields[7], completed, 88, 0, 0, 0)
                    server.sendto(ack, peer)
            thread = threading.Thread(target=responder);thread.start()
            client = MouseProxy('127.0.0.1', server.getsockname()[1])
            try:
                result = client.schedule_clicks(2, 2, 1, 0, protocol_version=2, timeout=1)
                self.assertEqual(result['version'], 2)
                self.assertEqual(result['completed_clicks'], 2)
            finally:
                client.socket.close();thread.join(1)
            fields = captured[0]
            self.assertEqual(fields[1:4], (2, 1, 2))
            self.assertEqual(fields[7:11], (2, 1000, 0, 0))

    def test_rejects_unsupported_click_version(self):
        client = MouseProxy('127.0.0.1', 9)
        try:
            with self.assertRaises(ValueError):
                client.schedule_clicks(protocol_version=3)
            with self.assertRaises(ValueError):
                client.release_all(protocol_version=0)
        finally:
            client.socket.close()


if __name__ == '__main__':
    unittest.main()
