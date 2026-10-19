#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""PPM (P6) a PNG, con ampliación entera opcional.

  tools/ppm-png.py entrada.ppm salida.png [escala]
"""
import struct
import sys
import zlib


def main() -> int:
    if len(sys.argv) < 3:
        print(__doc__)
        return 2
    data = open(sys.argv[1], "rb").read()
    parts = data.split(b"\n", 3)
    w, h = map(int, parts[1].split())
    px = parts[3]
    k = int(sys.argv[3]) if len(sys.argv) > 3 else 1
    raw = bytearray()
    for y in range(h):
        row = bytearray()
        for x in range(w):
            row += px[(y * w + x) * 3:(y * w + x) * 3 + 3] * k
        for _ in range(k):
            raw += b"\0" + row

    def chunk(t, d):
        c = struct.pack(">I", len(d)) + t + d
        return c + struct.pack(">I", zlib.crc32(t + d) & 0xFFFFFFFF)

    png = b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", w * k, h * k, 8, 2, 0, 0, 0)) + chunk(b"IDAT", zlib.compress(bytes(raw), 9)) + chunk(b"IEND", b"")
    open(sys.argv[2], "wb").write(png)
    return 0


if __name__ == "__main__":
    sys.exit(main())
