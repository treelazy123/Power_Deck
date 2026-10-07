"""Network reception and capture writing for the Power Deck desktop app."""
import csv
import json
import math
import queue
import select
import socket
import struct
import threading
import time
from collections import deque
from datetime import datetime, timezone
from pathlib import Path

from .depth_protocol import Reassembler


def utc_now():
    return datetime.now(timezone.utc).isoformat()


def parse_power(packet):
    obj = json.loads(packet)
    if obj.get("type") != "power":
        raise ValueError("Unexpected JSON type")
    for key in ("session", "seq", "timestamp_ms"):
        if not isinstance(obj.get(key), int) or obj[key] < 0:
            raise ValueError("Invalid power counter")
    for key in ("voltage_v", "current_a", "power_w"):
        if not isinstance(obj.get(key), (int, float)) or not math.isfinite(obj[key]):
            raise ValueError("Invalid power value")
    return obj


class SequenceLossEstimator:
    """Estimate missing source frames from valid DPT1 sequence numbers."""
    MASK = 0xFFFFFFFF
    HALF_RANGE = 0x80000000

    def __init__(self, reorder_window=256, history_size=512):
        self.reorder_window = reorder_window
        self.history_size = history_size
        self.reset(None)

    def reset(self, stream_key):
        self.stream_key = stream_key
        self.first_seq = None
        self.last_seq = None
        self.received = 0
        self.lost = 0
        self.reordered = 0
        self.duplicates = 0
        self._history = deque()
        self._seen = set()

    def _remember(self, seq):
        if len(self._history) >= self.history_size:
            self._seen.discard(self._history.popleft())
        self._history.append(seq)
        self._seen.add(seq)

    def observe(self, source, session, seq):
        key = (source, session)
        if key != self.stream_key:
            self.reset(key)

        seq &= self.MASK
        if self.last_seq is None:
            self.first_seq = self.last_seq = seq
            self.received = 1
            self._remember(seq)
            return
        if seq in self._seen:
            self.duplicates += 1
            return

        forward = (seq - self.last_seq) & self.MASK
        if 0 < forward < self.HALF_RANGE:
            self.lost += forward - 1
            self.received += 1
            self.last_seq = seq
            self._remember(seq)
            return

        backward = (self.last_seq - seq) & self.MASK
        observed_span = (self.last_seq - self.first_seq) & self.MASK
        if backward <= self.reorder_window and backward <= observed_span:
            # A late frame can fill a gap already counted within the recent window.
            if self.lost:
                self.lost -= 1
            self.received += 1
            self._remember(seq)
        else:
            self.reordered += 1

    def snapshot(self):
        total = self.received + self.lost
        return dict(seq_received=self.received, seq_lost=self.lost,
                    seq_loss_pct=(100.0 * self.lost / total) if total else 0.0,
                    seq_reordered=self.reordered, seq_duplicates=self.duplicates)


class CaptureRecorder:
    """Writes complete frames and power samples without blocking UDP reception."""
    def __init__(self, folder: Path):
        self.folder = Path(folder)
        self.items = queue.Queue(maxsize=512)
        self.closing = threading.Event()
        self.closed = threading.Event()
        self.lock = threading.Lock()
        self.frames = 0
        self.power = 0
        self.dropped = 0
        self.error = None
        self.thread = threading.Thread(target=self._run, name="capture-writer", daemon=False)
        self.thread.start()

    def submit(self, kind, source, value, host_utc):
        with self.lock:
            if self.closing.is_set():
                return
            try:
                self.items.put_nowait((kind, source, value, host_utc))
            except queue.Full:
                self.dropped += 1

    def stop(self):
        with self.lock:
            self.closing.set()

    def _run(self):
        try:
            frames_dir = self.folder / "frames"
            frames_dir.mkdir()
            with (self.folder / "power.csv").open("w", newline="", encoding="utf-8") as power_file, \
                 (self.folder / "depth.csv").open("w", newline="", encoding="utf-8") as depth_file:
                pw = csv.writer(power_file)
                dw = csv.writer(depth_file)
                pw.writerow(("host_utc", "board", "session", "seq", "timestamp_ms",
                             "voltage_v", "current_a", "power_w"))
                dw.writerow(("host_utc", "board", "session", "seq", "camera_ms",
                             "format", "payload_size", "c6_rx_ms", "c6_queue_ms",
                             "reassembly_ms", "bin_file", "pgm_file"))
                while not self.closing.is_set() or not self.items.empty():
                    try:
                        kind, source, value, host_utc = self.items.get(timeout=0.1)
                    except queue.Empty:
                        continue
                    if kind == "power":
                        pw.writerow((host_utc, source, value["session"], value["seq"],
                                     value["timestamp_ms"], value["voltage_v"],
                                     value["current_a"], value["power_w"]))
                        self.power += 1
                    else:
                        stem = (f"{source.replace('.', '_')}_{value['session']:08x}_"
                                f"{value['c6_rx_ms']:010d}_{value['seq']:010d}")
                        bin_name = stem + ".bin"
                        (frames_dir / bin_name).write_bytes(value["raw"])
                        pgm_name = ""
                        if value["pixels"] is not None:
                            pgm_name = stem + ".pgm"
                            pgm = (f"P5\n{value['width']} {value['height']}\n65535\n".encode()
                                   + b"".join(struct.pack(">H", max(0, min(65535, depth)))
                                              for depth, _ in value["pixels"]))
                            (frames_dir / pgm_name).write_bytes(pgm)
                        dw.writerow((host_utc, source, value["session"], value["seq"],
                                     value["camera_ms"], value["format"], value["payload_size"],
                                     value["c6_rx_ms"], value["c6_queue_ms"],
                                     f"{value['reassembly_ms']:.3f}",
                                     "frames/" + bin_name,
                                     "frames/" + pgm_name if pgm_name else ""))
                        self.frames += 1
                power_file.flush()
                depth_file.flush()
        except (OSError, ValueError, struct.error) as exc:
            self.error = str(exc)
        finally:
            self.closed.set()


class UdpReceiver:
    def __init__(self, board, events, recorder_getter, port=5005, discovery_port=5006):
        # An empty board means discover through limited UDP broadcast.
        self.board = board.strip()
        self.auto_discover = not self.board
        self.events = events
        self.recorder_getter = recorder_getter
        self.port = port
        self.discovery_port = discovery_port
        self.stop_event = threading.Event()
        self.thread = threading.Thread(target=self._run, name="deck-udp", daemon=True)
        self.thread.start()

    def stop(self):
        self.stop_event.set()
        self.thread.join(timeout=1.0)

    def _event(self, kind, value):
        try:
            self.events.put_nowait((kind, value))
            return True
        except queue.Full:
            return False

    def _run(self):
        reassembler = Reassembler()
        sequence_loss = SequenceLossEstimator()
        power_count = invalid = discovery_errors = ui_dropped = 0
        previous = 0
        try:
            with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as sock:
                sock.setsockopt(socket.SOL_SOCKET, socket.SO_BROADCAST, 1)
                sock.setsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF, 1024 * 1024)
                sock.bind(("0.0.0.0", self.port))
                sock.setblocking(False)
                self._event("listening", f"0.0.0.0:{self.port}")
                next_hello = 0.0
                next_report = time.monotonic() + 1.0
                while not self.stop_event.is_set():
                    now = time.monotonic()
                    if now >= next_hello:
                        try:
                            destination = self.board or "255.255.255.255"
                            sock.sendto(b"PDHELLO1", (destination, self.discovery_port))
                        except OSError:
                            discovery_errors += 1
                        next_hello = now + 1.0
                    readable, _, _ = select.select([sock], [], [], 0.03)
                    if readable:
                        for _ in range(256):
                            try:
                                raw, address = sock.recvfrom(65535)
                            except BlockingIOError:
                                break
                            source = address[0]
                            if self.board and self.board != "255.255.255.255" and source != self.board:
                                continue
                            arrival = time.monotonic()
                            host_utc = utc_now()
                            try:
                                if raw.startswith(b"{"):
                                    sample = parse_power(raw)
                                    self._select_discovered_board(source)
                                    power_count += 1
                                    if not self._event("power", (arrival, source, sample)):
                                        ui_dropped += 1
                                    recorder = self.recorder_getter()
                                    if recorder:
                                        recorder.submit("power", source, sample, host_utc)
                                else:
                                    frame = reassembler.feed(raw, address, arrival)
                                    if frame is None:
                                        continue
                                    self._select_discovered_board(source)
                                    sequence_loss.observe(source, frame["session"], frame["seq"])
                                    if not self._event("frame", (arrival, source, frame)):
                                        ui_dropped += 1
                                    recorder = self.recorder_getter()
                                    if recorder:
                                        recorder.submit("frame", source, frame, host_utc)
                            except (ValueError, KeyError, TypeError, AttributeError,
                                    UnicodeError, struct.error):
                                invalid += 1
                    now = time.monotonic()
                    reassembler.expire(now)
                    if now >= next_report:
                        complete = reassembler.stats["complete"]
                        elapsed = now - (next_report - 1.0)
                        self._event("stats", dict(depth=complete, fps=(complete-previous)/max(elapsed, 0.001),
                                                   power=power_count, invalid=invalid,
                                                   incomplete=reassembler.stats["incomplete"],
                                                   evicted=reassembler.stats["evicted"],
                                                   pending_peak=reassembler.stats["pending_peak"],
                                                   discovery_errors=discovery_errors,
                                                   ui_dropped=ui_dropped,
                                                   **sequence_loss.snapshot()))
                        previous = complete
                        next_report = now + 1.0
        except OSError as exc:
            self._event("error", f"UDP 接收失败：{exc}")
        finally:
            self._event("stopped", None)

    def _select_discovered_board(self, source):
        if self.auto_discover:
            self.auto_discover = False
            self.board = source
            self._event("discovered", source)
