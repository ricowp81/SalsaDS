#!/usr/bin/env python3
"""Ajusta las reglas de cobertura del rasterizador 3D contra una captura de melonDS (Fase 3.2).

    tools/r3d-fit.py mel-13.ppm            # escena 13 de la ROM gx3d (triángulos al azar, color plano)

Regenera los 48 triángulos de la escena 13 (el mismo LCG que tests/roms/src/gx3d/arm9.c), mide en la imagen el primer y
el último píxel cubierto de cada línea de cada triángulo y, para cada clase de arista (izquierda/derecha, x-mayor/
y-mayor, pendiente negativa/cero/positiva), cuenta cuántas líneas explica cada fórmula candidata. Solo ejecuta melonDS
(la imagen la saca tools/gfx-compare.py --keep): no se mira su código.
"""
import sys
from collections import defaultdict

BK = bytes.fromhex("202041")


def lcg_tris(seed):
    st = seed

    def rnd(n):
        nonlocal st
        st = (st * 1103515245 + 12345) & 0xFFFFFFFF
        return (st >> 16) % n

    tris = []
    for cy in range(6):
        for cx in range(8):
            v = []
            for _ in range(3):
                x = cx * 32 + 1 + rnd(30)
                y = cy * 32 + 1 + rnd(30)
                v.append((x, y))
            tris.append(((cx, cy), v))
    return tris


def read_ppm(p):
    d = open(p, "rb").read()
    return d.split(b"\n", 3)[3]


def covered(img, x, y):
    i = 3 * (y * 256 + x)
    return img[i:i + 3] != BK


def trunc_div(a, b):
    q = abs(a) // abs(b)
    return q if (a >= 0) == (b > 0) else -q


def chains(v):
    """aristas izquierda y derecha (listas de (a, b) con a.y < b.y) por el signo del área"""
    area = sum(v[i][0] * v[(i + 1) % 3][1] - v[(i + 1) % 3][0] * v[i][1] for i in range(3))
    top = min(range(3), key=lambda i: (v[i][1],))
    ldir = 1 if area < 0 else -1

    def walk(d):
        out, i = [], top
        for _ in range(3):
            j = (i + d) % 3
            if v[j][1] < v[i][1]:
                break
            if v[j][1] > v[i][1]:
                out.append((v[i], v[j]))
            i = j
        return out

    return walk(ldir), walk(-ldir)


def edge_at(edges, y):
    for a, b in edges:
        if a[1] <= y < b[1]:
            return a, b
    return None


def xfix(a, b, y):
    dx, dy = b[0] - a[0], b[1] - a[1]
    slope = trunc_div(dx << 18, dy)
    return (a[0] << 18) + slope * (y - a[1]), slope


# candidatos: función (a, b, y, side) -> x (inicio para la izquierda, final exclusivo para la derecha)
def cands():
    c = {}
    for off in (0, -1):
        c[f"floor(X(y){off:+d})"] = lambda a, b, y, off=off: (xfix(a, b, y)[0] + off) >> 18
    for off in (0, -1):
        c[f"floor(X(y+1){off:+d})"] = lambda a, b, y, off=off: (xfix(a, b, y + 1)[0] + off) >> 18 if y + 1 < b[1] else b[0]
        c[f"round(X(y+1){off:+d})"] = lambda a, b, y, off=off: (xfix(a, b, y + 1)[0] + off + (1 << 17)) >> 18 if y + 1 < b[1] else b[0]
    c["round(X(y))"] = lambda a, b, y: (xfix(a, b, y)[0] + (1 << 17)) >> 18
    c["floor(X(y+.5))"] = lambda a, b, y: (xfix(a, b, y)[0] + (xfix(a, b, y)[1] >> 1)) >> 18
    c["ceil(X(y))"] = lambda a, b, y: -((-xfix(a, b, y)[0]) >> 18)
    return c


def main():
    img = read_ppm(sys.argv[1])
    tris = lcg_tris(1234)
    C = cands()
    score = defaultdict(lambda: defaultdict(int))
    total = defaultdict(int)
    rows_bad_empty = 0
    for (cx, cy), v in tris:
        L, R = chains(v)
        ys = [p[1] for p in v]
        for y in range(min(ys) - 1, max(ys) + 2):
            xs = [x for x in range(cx * 32, cx * 32 + 32) if covered(img, x, y)] if 0 <= y < 192 else []
            el, er = edge_at(L, y), edge_at(R, y)
            if not el or not er:
                if xs:
                    rows_bad_empty += 1
                continue
            if not xs:
                continue
            first, last = xs[0], xs[-1] + 1
            for side, e, want in (("L", el, first), ("R", er, last)):
                a, b = e
                dx, dy = b[0] - a[0], b[1] - a[1]
                cls = (side, "x" if abs(dx) > dy else "y", "neg" if dx < 0 else "zero" if dx == 0 else "pos",
                       "y0" if y == a[1] else "")
                total[cls] += 1
                for name, f in C.items():
                    if f(a, b, y) == want:
                        score[cls][name] += 1
    print(f"líneas con píxeles fuera de las aristas (abajo/arriba): {rows_bad_empty}")
    for cls in sorted(total):
        best = sorted(score[cls].items(), key=lambda kv: -kv[1])[:4]
        print(cls, total[cls], " | ".join(f"{n}: {s}" for n, s in best))


if __name__ == "__main__":
    main()
