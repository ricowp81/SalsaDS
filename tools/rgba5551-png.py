#!/usr/bin/env python3
"""Convierte una imagen RGBA5551 cruda (2 bytes por píxel, big-endian, como
la que escriben las pruebas de la Fase 0 con /dev_hdd0/salsads/gpu2d-dump) a PNG.

  tools/rgba5551-png.py imagen.rgba5551 salida.png [ancho alto]   (256×192 por omisión)
"""
import struct
import sys
import zlib


def main() -> int:
    if len(sys.argv) < 3:
        print(__doc__)
        return 2
    w, h = (int(sys.argv[3]), int(sys.argv[4])) if len(sys.argv) >= 5 else (256, 192)
    raw = open(sys.argv[1], "rb").read()
    if len(raw) < w * h * 2:
        sys.exit(f"{sys.argv[1]}: {len(raw)} bytes, hacen falta {w * h * 2}")
    rows = bytearray()
    for y in range(h):
        rows.append(0)
        for x in range(w):
            (v,) = struct.unpack_from(">H", raw, (y * w + x) * 2)
            r, g, b = (v >> 11) & 31, (v >> 6) & 31, (v >> 1) & 31
            rows += bytes(((r << 3) | (r >> 2), (g << 3) | (g >> 2), (b << 3) | (b >> 2)))

    def chunk(tag: bytes, data: bytes) -> bytes:
        return struct.pack(">I", len(data)) + tag + data + struct.pack(">I", zlib.crc32(tag + data))

    png = b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 2, 0, 0, 0))
    png += chunk(b"IDAT", zlib.compress(bytes(rows), 9)) + chunk(b"IEND", b"")
    open(sys.argv[2], "wb").write(png)
    return 0


if __name__ == "__main__":
    sys.exit(main())
