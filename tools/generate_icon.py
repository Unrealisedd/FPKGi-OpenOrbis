#!/usr/bin/env python3
"""Generate the dependency-free 512x512 24-bit RGB PNG application icon."""
from __future__ import annotations

import struct
import sys
import zlib
from pathlib import Path

W = H = 512


def chunk(kind: bytes, data: bytes) -> bytes:
    return struct.pack(">I", len(data)) + kind + data + struct.pack(">I", zlib.crc32(kind + data) & 0xFFFFFFFF)


def pixel(x: int, y: int) -> tuple[int, int, int]:
    red, green, blue = 13, 18, 28
    if 70 <= x <= 442 and (120 <= y <= 155 or 245 <= y <= 280 or 370 <= y <= 405):
        red, green, blue = 82, 193, 170
    if 170 <= x <= 342 and 165 <= y <= 365:
        red, green, blue = 240, 244, 250
    if 215 <= x <= 297 and 210 <= y <= 320:
        red, green, blue = 45, 112, 196
    return red, green, blue


def main(path: str) -> None:
    raw = bytearray()
    for y in range(H):
        raw.append(0)
        for x in range(W):
            raw.extend(pixel(x, y))
    png = b"\x89PNG\r\n\x1a\n"
    png += chunk(b"IHDR", struct.pack(">IIBBBBB", W, H, 8, 2, 0, 0, 0))
    png += chunk(b"IDAT", zlib.compress(bytes(raw), 9))
    png += chunk(b"IEND", b"")
    Path(path).write_bytes(png)


if __name__ == "__main__":
    main(sys.argv[1])
