#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Reduce los OBJ de una escena de `gfxrand` al grupo más chico que sigue haciendo diferir a SalsaDS y melonDS.

    tools/gfx-bisect-obj.py build/roms/gfxrand.nds SEMILLA

Prueba mitades del rango de OBJ [0, 128) y se queda con la que sigue difiriendo; cuando ninguna mitad lo hace, el
grupo lo forman OBJ de las dos mitades (se muestra el rango actual). Después prueba a quitar OBJ uno por uno.
"""
import subprocess
import sys

HERE = __file__.rsplit("/", 1)[0]


def differs(rom, seed, a, b):
    out = subprocess.run([sys.executable, HERE + "/gfx-compare.py", rom, str(seed), "--keep-obj", f"{a}:{b}"],
                         capture_output=True, text=True).stdout.strip()
    return "DIFERENTES" in out, out


def main():
    rom, seed = sys.argv[1], int(sys.argv[2], 0)
    a, b = 0, 128
    ok, out = differs(rom, seed, a, b)
    print(f"[{a},{b}) {out}", flush=True)
    if not ok:
        return
    while b - a > 1:
        mid = (a + b) // 2
        d1, o1 = differs(rom, seed, a, mid)
        print(f"[{a},{mid}) {o1}", flush=True)
        if d1:
            b = mid
            continue
        d2, o2 = differs(rom, seed, mid, b)
        print(f"[{mid},{b}) {o2}", flush=True)
        if d2:
            a = mid
            continue
        break
    print(f"rango mínimo contiguo que difiere: [{a},{b})")


main()
