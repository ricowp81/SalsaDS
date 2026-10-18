#!/usr/bin/env python3
"""Aísla qué característica de una escena de `gfxrand` hace que SalsaDS y melonDS difieran.

    tools/gfx-bisect.py build/roms/gfxrand.nds SEMILLA [--only 0x01,0x02,...]

Corre la escena con todas las características apagadas (máscara 0xFF en el byte alto de la semilla), con cada una
encendida sola y con todas menos una (ver el encabezado de tests/roms/src/gfx/rand.c). Sirve para saber en qué familia buscar.
"""
import argparse
import subprocess
import sys

NAMES = {0x001: "capa de OBJ", 0x002: "ventanas", 0x004: "efectos de color", 0x008: "mosaico de fondos",
         0x010: "OBJ afines", 0x020: "OBJ semitransparentes", 0x040: "OBJ de ventana", 0x080: "OBJ de mapa de bits",
         0x100: "fondos afines/extendidos/grandes", 0x200: "capa 3D", 0x800: "mosaico de OBJ"}
ALL = 0xFFF


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("rom")
    ap.add_argument("seed", type=lambda s: int(s, 0))
    ap.add_argument("--only")
    args = ap.parse_args()
    base = args.seed & 0xFFFFF
    bits = [int(x, 0) for x in args.only.split(",")] if args.only else sorted(NAMES)
    # las características de OBJ necesitan la capa de OBJ encendida
    need_obj = 0x010 | 0x020 | 0x040 | 0x080 | 0x800
    runs = ([("todo apagado", ALL)] +
            [("solo " + NAMES[b], ALL & ~b & (~0x001 if b & need_obj else ALL)) for b in bits] +
            [("todo encendido", 0)] + [("todo menos " + NAMES[b], b) for b in bits])
    for name, mask in runs:
        seed = (mask << 20) | base
        out = subprocess.run([sys.executable, __file__.replace("gfx-bisect", "gfx-compare"), args.rom, str(seed)],
                             capture_output=True, text=True).stdout.strip()
        print(f"{name:55s} {out}", flush=True)


if __name__ == "__main__":
    main()
