#!/usr/bin/env python3
"""Compara la pantalla de arriba de una imagen de SalsaDS con el banco A de VRAM que dejó melonDS.

    salsads-cpurun --screens sds.ppm ROM.nds          # 256×384: arriba y abajo
    tools/melonds-run.py ROM.nds --vram melon.ppm     # 256×192: banco A como LCDC
    tools/compare-image.py sds.ppm melon.ppm

Imprime cuántos píxeles difieren y, si hay, el primero y un mapa grueso. Código de salida 0 si son iguales.
"""
import sys


def read_ppm(path):
    with open(path, "rb") as f:
        data = f.read()
    parts = data.split(b"\n", 3)
    assert parts[0] == b"P6", "no es un PPM P6"
    w, h = map(int, parts[1].split())
    return w, h, parts[3][: w * h * 3]


def main() -> int:
    if len(sys.argv) != 3:
        print(__doc__)
        return 2
    w1, h1, a = read_ppm(sys.argv[1])
    w2, h2, b = read_ppm(sys.argv[2])
    assert w1 == w2 == 256, "se esperaba ancho 256"
    a = a[: 256 * 192 * 3]  # la pantalla de arriba
    b = b[: 256 * 192 * 3]
    diff = [i for i in range(256 * 192) if a[3 * i:3 * i + 3] != b[3 * i:3 * i + 3]]
    if not diff:
        print("IGUALES: 256x192 píxeles idénticos")
        return 0
    i = diff[0]
    print("DIFERENTES: %d de %d píxeles; primero en (%d,%d): SalsaDS %s, melonDS %s" %
          (len(diff), 256 * 192, i % 256, i // 256, a[3 * i:3 * i + 3].hex(), b[3 * i:3 * i + 3].hex()))
    cells = {}
    for i in diff:
        cells[(i % 256 // 32, i // 256 // 24)] = cells.get((i % 256 // 32, i // 256 // 24), 0) + 1
    for cy in range(8):
        print("".join("#" if (cx, cy) in cells else "." for cx in range(8)))
    return 1


if __name__ == "__main__":
    sys.exit(main())
