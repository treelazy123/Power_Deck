import csv
import json
import socket
import struct
import subprocess
import sys
import tempfile
import unittest
import zlib
from pathlib import Path
from power_deck_tool.depth_protocol import FRAME_HEADER, HEADER, Reassembler, decode_frame, fragment_frame

def fixture(seq=7):
    body=FRAME_HEADER.pack(b"DPT1",1,1,32,seq,123,48,48,6912,1,1,6948)
    body+=b"".join(struct.pack("<HB",1000+i%100,1) for i in range(2304))
    return body+struct.pack("<I",zlib.crc32(body))
def float_fixture(seq=11):
    body=FRAME_HEADER.pack(b"DPT1",1,2,32,seq,123,48,48,9216,0,1,9252)
    body+=b"".join(struct.pack("<f",1.0+i/10000) for i in range(2304))
    return body+struct.pack("<I",zlib.crc32(body))

def raw_fixture(seq=12):
    body=FRAME_HEADER.pack(b"DPT1",1,3,32,seq,123,48,48,14764,0,1,14800)
    body+=bytes((i*17+seq*13)&255 for i in range(14764))
    return body+struct.pack("<I",zlib.crc32(body))
class ProtocolTests(unittest.TestCase):
    def test_reverse_order_duplicates(self):
        r=Reassembler(); parts=fragment_frame(fixture())
        self.assertIsNone(r.feed(parts[-1],"a",0))
        self.assertIsNone(r.feed(parts[-1],"a",0.01))
        result=None
        for i,part in enumerate(reversed(parts[:-1])): result=r.feed(part,"a",0.02+i*0.01)
        self.assertEqual(result["raw"],fixture())
        self.assertEqual(r.stats["duplicates"],1)
        self.assertIsNone(r.feed(parts[0],"a",0.1))
        self.assertEqual(r.stats["complete"],1)
    def test_timeout_and_late(self):
        r=Reassembler(); parts=fragment_frame(fixture())
        r.feed(parts[0],"a",0); r.expire(0.4)
        self.assertEqual(r.stats["incomplete"],1)
        for part in parts[1:]: self.assertIsNone(r.feed(part,"a",0.41))
        self.assertEqual(len(r.pending),0)
    def test_conflicting_duplicate(self):
        r=Reassembler(); part=fragment_frame(fixture())[0]
        r.feed(part,"a",0)
        bad=bytearray(part); bad[-1]^=1
        with self.assertRaises(ValueError): r.feed(bad,"a",0.01)
    def test_corrupted_payload_crc(self):
        r=Reassembler(); parts=fragment_frame(fixture()); bad=bytearray(parts[-1]); bad[-1]^=1; parts[-1]=bad
        with self.assertRaises(ValueError):
            for part in parts: r.feed(part,"a",0)
    def test_header_bounds(self):
        part=fragment_frame(fixture())[0]
        for offset in (0,4,5,6,16,20,24,26,28,30):
            bad=bytearray(part); bad[offset]^=128
            with self.subTest(offset=offset),self.assertRaises(ValueError): Reassembler().feed(bad,"a",0)
        for bad in (b"",part[:39],part[:-1],part+b"x"):
            with self.assertRaises(ValueError): Reassembler().feed(bad,"a",0)
    def test_bound_memory(self):
        r=Reassembler(max_pending=2)
        for i in range(10): r.feed(fragment_frame(fixture(i))[0],"a",0)
        self.assertEqual(len(r.pending),2); self.assertEqual(r.stats["evicted"],8)
    def test_session_and_source_isolation(self):
        r=Reassembler(); p1=fragment_frame(fixture(),session=1); p2=fragment_frame(fixture(),session=2)
        for part in p1[:3]: r.feed(part,"a",0)
        for part in p2[3:]: r.feed(part,"a",0)
        for part in p1[3:]: r.feed(part,"b",0)
        self.assertEqual(r.stats["complete"],0)
        for part in p1[3:]: result=r.feed(part,"a",0)
        self.assertEqual(result["session"],1)
    def test_sequence_crosscheck(self):
        r=Reassembler(); parts=fragment_frame(fixture())
        for i,part in enumerate(parts):
            bad=bytearray(part); struct.pack_into("<I",bad,12,99); parts[i]=bad
        with self.assertRaises(ValueError):
            for part in parts: r.feed(part,"a",0)
    def test_frame_status(self):
        bad=bytearray(fixture()); bad[34]=3
        bad[-4:]=struct.pack("<I",zlib.crc32(bad[:-4]))
        with self.assertRaises(ValueError): decode_frame(bad)
    def test_float_and_raw_roundtrip(self):
        for expected_format,source,part_count in ((2,float_fixture(),8),(3,raw_fixture(),13)):
            with self.subTest(fmt=expected_format):
                self.assertEqual(len(source),9252 if expected_format==2 else 14800)
                parts=fragment_frame(source)
                self.assertEqual(len(parts),part_count)
                r=Reassembler()
                result=None
                for index,part in enumerate(reversed(parts)):
                    result=r.feed(part,"board",index*0.001)
                self.assertEqual(result["raw"],source)
                self.assertEqual(result["format"],expected_format)
                self.assertEqual(r.stats["complete"],1)
                if expected_format==2:
                    self.assertEqual(len(result["pixels"]),2304)
                    self.assertEqual(result["pixels"][0],(1000,1))
                else:
                    self.assertIsNone(result["pixels"])

    def test_invalid_float_and_raw_crc(self):
        bad=bytearray(float_fixture())
        bad[32:36]=struct.pack("<f",float("nan"))
        bad[-4:]=struct.pack("<I",zlib.crc32(bad[:-4]))
        with self.assertRaises(ValueError): decode_frame(bad)
        bad=bytearray(raw_fixture())
        bad[100]^=1
        with self.assertRaises(ValueError): decode_frame(bad)
    def test_udp_cli(self):
        with tempfile.TemporaryDirectory() as tmp, socket.socket(socket.AF_INET,socket.SOCK_DGRAM) as board:
            board.bind(("127.0.0.1",0)); board.settimeout(5)
            with socket.socket(socket.AF_INET,socket.SOCK_DGRAM) as reserve:
                reserve.bind(("127.0.0.1",0)); port=reserve.getsockname()[1]
            env = dict(__import__("os").environ, PYTHONPATH=str(Path(__file__).parents[1] / "src"))
            proc=subprocess.Popen([sys.executable,"-m","power_deck_tool.deck_receiver","--bind","127.0.0.1","--board","127.0.0.1",
                "--port",str(port),"--discovery-port",str(board.getsockname()[1]),"--duration","2","--save",tmp],
                stdout=subprocess.PIPE,stderr=subprocess.STDOUT,text=True,env=env)
            try:
                hello,peer=board.recvfrom(32); self.assertEqual(hello,b"PDHELLO1")
                board.sendto(json.dumps(dict(type="power",session=1,seq=1,timestamp_ms=10,voltage_v=4.1,current_a=0.1,power_w=0.41)).encode(),peer)
                for part in reversed(fragment_frame(fixture())): board.sendto(part,peer)
                for source in (float_fixture(),raw_fixture()):
                    for part in fragment_frame(source): board.sendto(part,peer)
                board.sendto(b"broken",peer)
                output,_=proc.communicate(timeout=8)
                self.assertEqual(proc.returncode,0,output)
                self.assertIn("complete=3 power=1 invalid=1",output)
                root=next(Path(tmp).iterdir())
                self.assertEqual(len(list(root.glob("*.bin"))),3)
                self.assertEqual(next(root.glob("*.bin")).read_bytes(),fixture())
                self.assertTrue(next(root.glob("*.pgm")).read_bytes().startswith(b"P5\n48 48\n65535\n"))
                self.assertEqual(len(list(root.glob("*.pgm"))),2)
                with (root/"power.csv").open() as f: self.assertEqual(len(list(csv.reader(f))),2)
            finally:
                if proc.poll() is None: proc.kill(); proc.communicate()
if __name__=="__main__": unittest.main(verbosity=2)
