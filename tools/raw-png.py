#!/usr/bin/env python3
"""Convierte una imagen cruda de las pruebas de la Fase 0 a PNG.

  tools/raw-png.py rgba5551 imagen.rgba5551 salida.png [ancho alto]  (256×192 por omisión)
  tools/raw-png.py xrgb8888 imagen.xrgb8888 salida.png ancho alto

Formatos (big-endian, como en la PS3): rgba5551 = 2 bytes por píxel
(R[15:11] G[10:6] B[5:1] A[0]); xrgb8888 = 4 bytes por píxel (X, R, G, B).
Las pruebas escriben estas imágenes en /dev_hdd0/salsads/ si existe el
archivo gpu2d-dump o dual-screen-dump.
"""
import struct
import sys
import zlib


def main() -> int:
    if len(sys.argv) < 4:
        print(__doc__)
        return 2
    fmt, src, dst = sys.argv[1:4]
    if fmt == "rgba5551":
        w, h = (int(sys.argv[4]), int(sys.argv[5])) if len(sys.argv) >= 6 else (256, 192)
        bpp = 2
    elif fmt == "xrgb8888":
        if len(sys.argv) < 6:
            sys.exit("xrgb8888 necesita ancho y alto")
        w, h = int(sys.argv[4]), int(sys.argv[5])
        bpp = 4
    else:
        sys.exit(f"formato desconocido: {fmt}")
    raw = open(src, "rb").read()
    if len(raw) < w * h * bpp:
        sys.exit(f"{src}: {len(raw)} bytes, hacen falta {w * h * bpp}")
    rows = bytearray()
    for y in range(h):
        rows.append(0)
        base = y * w * bpp
        if fmt == "xrgb8888":
            for x in range(w):
                rows += raw[base + x * 4 + 1 : base + x * 4 + 4]
        else:
            for x in range(w):
                (v,) = struct.unpack_from(">H", raw, base + x * 2)
                r, g, b = (v >> 11) & 31, (v >> 6) & 31, (v >> 1) & 31
                rows += bytes(((r << 3) | (r >> 2), (g << 3) | (g >> 2), (b << 3) | (b >> 2)))

    def chunk(tag: bytes, data: bytes) -> bytes:
        return struct.pack(">I", len(data)) + tag + data + struct.pack(">I", zlib.crc32(tag + data))

    png = b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 2, 0, 0, 0))
    png += chunk(b"IDAT", zlib.compress(bytes(rows), 9)) + chunk(b"IEND", b"")
    open(dst, "wb").write(png)
    return 0


if __name__ == "__main__":
    sys.exit(main())
