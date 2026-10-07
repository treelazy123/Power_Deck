import struct
import unittest
import zlib
from decode_depth import HEADER, decode

def fixture():
    payload = struct.pack("<HBHB", 1234, 1, 0, 0)
    body = HEADER.pack(b"DPT1", 1, 1, 32, 7, 123, 2, 1, len(payload), 1, 1, 42) + payload
    return body + struct.pack("<I", zlib.crc32(body))
class ProtocolTests(unittest.TestCase):
    def test_valid_and_byte_order(self):
        frame = decode(fixture())
        self.assertEqual(frame["pixels"], [(1234, 1), (0, 0)])
        self.assertEqual(frame["sequence"], 7)
        self.assertEqual(frame["invalid"], 1)
    def test_truncated_extra_and_misaligned(self):
        packet = fixture()
        for bad in (packet[:12], packet[:-1], packet+b"x", packet[1:]+b"x"):
            with self.subTest(length=len(bad)), self.assertRaises(ValueError): decode(bad)
    def test_corruption(self):
        for offset in (8, 32, 41):
            packet = bytearray(fixture()); packet[offset] ^= 1
            with self.subTest(offset=offset), self.assertRaises(ValueError): decode(packet)
    def test_invalid_status_even_with_valid_crc(self):
        packet = bytearray(fixture()); packet[34] = 2
        packet[-4:] = struct.pack("<I", zlib.crc32(packet[:-4]))
        with self.assertRaises(ValueError): decode(packet)
if __name__ == "__main__": unittest.main()
