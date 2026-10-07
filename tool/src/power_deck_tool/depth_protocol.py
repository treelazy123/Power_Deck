"""PDK1 UDP fragments and DPT1 synthetic depth/opaque benchmark frames."""
import math
import struct
import zlib
from collections import Counter, OrderedDict

HEADER = struct.Struct("<4sBBHIIIIHHHHII")
FRAME_HEADER = struct.Struct("<4sBBHIIHHIHHI")
FRAME_SIZES = {1: 6948, 2: 9252, 3: 14800}
PAYLOAD_SIZES = {1: 6912, 2: 9216, 3: 14764}
CHUNK = 1200


def decode_frame(data):
    if len(data) < FRAME_HEADER.size + 4:
        raise ValueError("Short DPT1 frame")
    magic, version, fmt, hlen, seq, tick, width, height, size, unit, flags, total = FRAME_HEADER.unpack_from(data)
    if (magic, version, hlen, width, height, flags) != (b"DPT1", 1, 32, 48, 48, 1):
        raise ValueError("Unsupported DPT1 frame")
    if fmt not in FRAME_SIZES or len(data) != FRAME_SIZES[fmt] or total != len(data):
        raise ValueError("Unexpected DPT1 format or frame size")
    if size != PAYLOAD_SIZES[fmt] or unit != (1 if fmt == 1 else 0):
        raise ValueError("Invalid DPT1 payload metadata")
    if zlib.crc32(data[:-4]) != struct.unpack_from("<I", data, len(data)-4)[0]:
        raise ValueError("Bad frame CRC32")
    payload = data[hlen:-4]
    if fmt == 1:
        pixels = list(struct.iter_unpack("<HB", payload))
        if any(s not in (0, 1) or (s == 0 and d != 0) for d, s in pixels):
            raise ValueError("Invalid pixel encoding")
    elif fmt == 2:
        metres = [v[0] for v in struct.iter_unpack("<f", payload)]
        if any(not math.isfinite(v) or v < 0 for v in metres):
            raise ValueError("Invalid float depth")
        pixels = [(round(v * 1000), int(v > 0)) for v in metres]
    else:
        pixels = None  # Opaque synthetic raw bytes have no known pixel layout.
    return dict(seq=seq, camera_ms=tick, width=width, height=height,
                format=fmt, payload_size=size, pixels=pixels)


class Reassembler:
    def __init__(self, timeout=0.3, max_pending=16):
        self.timeout = timeout
        self.max_pending = max_pending
        self.pending = OrderedDict()
        self.closed = OrderedDict()
        self.stats = Counter()

    def _close(self, key, now):
        self.closed[key] = now
        while len(self.closed) > 128:
            self.closed.popitem(last=False)

    def expire(self, now):
        for key, item in list(self.pending.items()):
            if now-item["start"] >= self.timeout:
                del self.pending[key]
                self._close(key, now)
                self.stats["incomplete"] += 1

    def feed(self, packet, source, now):
        self.expire(now)
        if len(packet) < HEADER.size:
            raise ValueError("Short UDP header")
        magic, ver, kind, hlen, session, seq, total, offset, length, index, count, reserved, rx_ms, queue_ms = HEADER.unpack_from(packet)
        if (magic, ver, kind, hlen, reserved) != (b"PDK1", 1, 2, 40, 0):
            raise ValueError("Invalid PDK1 header")
        if total not in FRAME_SIZES.values() or count != (total+CHUNK-1)//CHUNK:
            raise ValueError("Invalid frame size or fragment count")
        if index >= count or offset != index*CHUNK or length != min(CHUNK, total-offset) or len(packet) != hlen+length:
            raise ValueError("Invalid fragment bounds")
        key = (source, session, seq, rx_ms)
        if key in self.closed:
            self.stats["late_or_duplicate"] += 1
            return None
        if key not in self.pending:
            if len(self.pending) >= self.max_pending:
                old, _ = self.pending.popitem(last=False)
                self._close(old, now)
                self.stats["evicted"] += 1
            self.pending[key] = dict(start=now, parts={}, max_queue_ms=0, total=total)
        item = self.pending[key]
        if item["total"] != total:
            del self.pending[key]
            self._close(key, now)
            raise ValueError("Conflicting frame size")
        payload = packet[hlen:]
        if index in item["parts"]:
            if item["parts"][index] != payload:
                del self.pending[key]
                self._close(key, now)
                raise ValueError("Conflicting duplicate fragment")
            self.stats["duplicates"] += 1
            return None
        item["parts"][index] = payload
        item["max_queue_ms"] = max(item["max_queue_ms"], queue_ms)
        self.stats["pending_peak"] = max(self.stats["pending_peak"], len(self.pending))
        if len(item["parts"]) != count:
            return None
        del self.pending[key]
        self._close(key, now)
        raw = b"".join(item["parts"][i] for i in range(count))
        frame = decode_frame(raw)
        if frame["seq"] != seq:
            raise ValueError("Inner/outer frame sequence differs")
        frame.update(raw=raw, session=session, c6_rx_ms=rx_ms,
                     c6_queue_ms=item["max_queue_ms"], reassembly_ms=(now-item["start"])*1000)
        self.stats["complete"] += 1
        return frame


def fragment_frame(raw, session=1, rx_ms=100, queue_ms=0):
    """Reference encoder used by integration tests and the simulated board."""
    seq = decode_frame(raw)["seq"]
    count = (len(raw)+CHUNK-1)//CHUNK
    return [HEADER.pack(b"PDK1", 1, 2, 40, session, seq, len(raw), off,
                        len(raw[off:off+CHUNK]), off//CHUNK, count, 0, rx_ms, queue_ms)
            + raw[off:off+CHUNK] for off in range(0, len(raw), CHUNK)]