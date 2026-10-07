"""Validate one raw SPI frame: python tools/decode_depth.py capture.bin [out.pgm]."""
import struct
import sys
import zlib
from pathlib import Path
HEADER = struct.Struct("<4sBBHIIHHIHHI")
def decode(data):
    if len(data) < 36:
        raise ValueError("Truncated frame")
    magic, version, fmt, hdr, seq, tick, w, h, payload, unit, flags, total = HEADER.unpack_from(data)
    if (magic, version, fmt, hdr) != (b"DPT1", 1, 1, 32):
        raise ValueError("Unsupported header / SPI alignment error")
    if not w or not h or unit != 1 or flags != 1:
        raise ValueError("Invalid dimensions, depth unit or flags")
    if payload != w*h*3 or total != hdr+payload+4 or len(data) != total:
        raise ValueError("Frame length mismatch")
    expected, = struct.unpack_from("<I", data, total-4)
    if zlib.crc32(data[:-4]) != expected:
        raise ValueError("CRC32 mismatch")
    pixels = list(struct.iter_unpack("<HB", data[hdr:-4]))
    if any(status not in (0, 1) or (status == 0 and depth != 0) for depth, status in pixels):
        raise ValueError("Invalid pixel status")
    return dict(sequence=seq, timestamp_ms=tick, width=w, height=h,
                invalid=sum(status == 0 for _, status in pixels), pixels=pixels)
if __name__ == "__main__":
    frame = decode(Path(sys.argv[1]).read_bytes())
    print({k: v for k, v in frame.items() if k != "pixels"})
    if len(sys.argv) > 2:
        # PGM uses big-endian u16 regardless of the little-endian SPI format.
        header = f"P5\n{frame['width']} {frame['height']}\n65535\n".encode()
        image = b"".join(struct.pack(">H", d) for d, _ in frame["pixels"])
        Path(sys.argv[2]).write_bytes(header + image)
