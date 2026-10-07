import csv
import json
import queue
import socket
import tempfile
import time
import unittest
from pathlib import Path

from power_deck_tool.deck_gui import depth_ppm
from power_deck_tool.deck_gui_backend import CaptureRecorder, UdpReceiver
from power_deck_tool.depth_protocol import fragment_frame
from test_stream import fixture, float_fixture, raw_fixture


class GuiPipelineTests(unittest.TestCase):
    def test_udp_reception_and_recording(self):
        with tempfile.TemporaryDirectory() as tmp, socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as board:
            board.bind(("127.0.0.1", 0))
            board.settimeout(3)
            with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as reserved:
                reserved.bind(("127.0.0.1", 0))
                pc_port = reserved.getsockname()[1]
            folder = Path(tmp) / "capture"
            folder.mkdir()
            recorder = CaptureRecorder(folder)
            events = queue.Queue(maxsize=1024)
            receiver = UdpReceiver("127.0.0.1", events, lambda: recorder,
                                   port=pc_port, discovery_port=board.getsockname()[1])
            try:
                hello, peer = board.recvfrom(32)
                self.assertEqual(hello, b"PDHELLO1")
                sample = dict(type="power", session=1, seq=1, timestamp_ms=10,
                              voltage_v=4.1, current_a=0.1, power_w=0.41)
                board.sendto(json.dumps(sample).encode(), peer)
                for raw in (fixture(), float_fixture(), raw_fixture()):
                    for part in fragment_frame(raw):
                        board.sendto(part, peer)
                seen = []
                deadline = time.monotonic() + 3
                while time.monotonic() < deadline and (len(seen) < 3 or recorder.power < 1):
                    try:
                        kind, payload = events.get(timeout=0.1)
                        if kind == "frame":
                            seen.append(payload[2])
                    except queue.Empty:
                        pass
                self.assertEqual([f["format"] for f in seen], [1, 2, 3])
                self.assertIsNotNone(depth_ppm(seen[0]))
                self.assertIsNotNone(depth_ppm(seen[1]))
                self.assertIsNone(depth_ppm(seen[2]))
            finally:
                receiver.stop()
                recorder.stop()
                recorder.thread.join(timeout=5)
            self.assertTrue(recorder.closed.is_set())
            self.assertIsNone(recorder.error)
            self.assertEqual((recorder.frames, recorder.power, recorder.dropped), (3, 1, 0))
            self.assertEqual(len(list((folder / "frames").glob("*.bin"))), 3)
            self.assertEqual(len(list((folder / "frames").glob("*.pgm"))), 2)
            with (folder / "power.csv").open(newline="", encoding="utf-8") as f:
                self.assertEqual(len(list(csv.reader(f))), 2)
            with (folder / "depth.csv").open(newline="", encoding="utf-8") as f:
                self.assertEqual(len(list(csv.reader(f))), 4)


if __name__ == "__main__":
    unittest.main(verbosity=2)
