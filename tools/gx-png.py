#!/usr/bin/env python3
"""Dibuja a PNG el cuadro de geometría que vuelca `salsads-cpurun --gx-dump` (depuración de la Fase 3.1).

  tools/gx-png.py gx.txt salida.png [--wire]

Relleno plano con el color del primer vértice, en el orden de la lista (sin z-buffer, sin textura: sólo para ver si la
geometría tiene sentido); con --wire sólo los bordes. Las caras traseras (bandera 0x01 apagada) salen en rojo tenue.
"""
import struct
import sys
import zlib

W, H = 256, 192


def png(path, pix):
    raw = b"".join(b"\x00" + bytes(pix[y * W * 3:(y + 1) * W * 3]) for y in range(H))
    def chunk(t, d):
        c = struct.pack(">I", len(d)) + t + d
        return c + struct.pack(">I", zlib.crc32(t + d) & 0xFFFFFFFF)
    with open(path, "wb") as f:
        f.write(b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", W, H, 8, 2, 0, 0, 0)) +
                chunk(b"IDAT", zlib.compress(raw)) + chunk(b"IEND", b""))


def fill(pix, pts, col):
    ys = [p[1] for p in pts]
    for y in range(max(0, min(ys)), min(H - 1, max(ys)) + 1):
        xs = []
        n = len(pts)
        for i in range(n):
            (x0, y0), (x1, y1) = pts[i], pts[(i + 1) % n]
            if (y0 <= y < y1) or (y1 <= y < y0):
                xs.append(x0 + (y - y0) * (x1 - x0) / (y1 - y0))
        xs.sort()
        for a, b in zip(xs[0::2], xs[1::2]):
            for x in range(max(0, int(a)), min(W - 1, int(b)) + 1):
                o = (y * W + x) * 3
                pix[o:o + 3] = bytes(col)


def line(pix, a, b, col):
    (x0, y0), (x1, y1) = a, b
    n = max(abs(x1 - x0), abs(y1 - y0), 1)
    for i in range(n + 1):
        x = x0 + (x1 - x0) * i // n
        y = y0 + (y1 - y0) * i // n
        if 0 <= x < W and 0 <= y < H:
            o = (y * W + x) * 3
            pix[o:o + 3] = bytes(col)


def main():
    if len(sys.argv) < 3:
        print(__doc__)
        return 2
    wire = "--wire" in sys.argv
    pix = bytearray(W * H * 3)
    polys = []
    for ln in open(sys.argv[1]):
        t = ln.split()
        if not t:
            continue
        if t[0] == "poly":
            fl = int([x for x in t if x.startswith("flags=")][0][6:], 16)
            polys.append([fl, []])
        elif t[0] == "v":
            rgb = [int(x) for x in t[-1][4:].split(",")]
            polys[-1][1].append((int(t[1]), int(t[2]), rgb))
    for fl, vs in polys:
        pts = [(x, y) for x, y, _ in vs]
        col = [min(255, c * 4) for c in vs[0][2]]
        if not fl & 1:
            col = [c // 2 + 60 for c in col[:1]] + [c // 4 for c in col[1:]]
        if not wire:
            fill(pix, pts, col)
        for i in range(len(pts)):
            line(pix, pts[i], pts[(i + 1) % len(pts)], (255, 255, 255) if wire else tuple(c // 2 for c in col))
    png(sys.argv[2], pix)
    print(f"{len(polys)} polígonos -> {sys.argv[2]}")
    return 0


sys.exit(main())
