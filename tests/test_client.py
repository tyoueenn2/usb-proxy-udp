import socket
import struct
import unittest
from client import MouseProxy


class ClientTest(unittest.TestCase):
    def test_wire_and_button_snapshots(self):
        with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as server:
            server.bind(('127.0.0.1', 0))
            server.settimeout(1)
            with MouseProxy('127.0.0.1', server.getsockname()[1]) as client:
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
            self.assertEqual(server.recv(100)[14], 0)


if __name__ == '__main__':
    unittest.main()
