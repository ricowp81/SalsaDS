#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Genera tests/roms/src/tablas/ref.h: las tablas de las SWI 1Ah-1Ch del ARM7 (seno, tono y volumen) a partir de su
fórmula, y las compara con un volcado de lo que devuelve melonDS (ROM `tablas`).

    tools/gen-bios-tables.py > tests/roms/src/tablas/ref.h
    tools/gen-bios-tables.py --check volcado.bin    (volcado de 0x02300000: 1556 palabras, una por índice)

Las fórmulas se dedujeron del comportamiento de melonDS 1.1 (su BIOS libre) y coinciden en las 1556 entradas:
  seno (64):     round(sin(i * pi / 128) * 32767)
  tono (768):    round((2^(i / 768) - 1) * 65536)
  volumen (724): min(127, round(K * 10^((i - 723) / 200))), con K = 2048 (i < 483), 512 (i < 603), 256 (i < 663) y
                 128 (resto); el último elemento de los tres tramos de arriba (i = 602, 662, 722) vale 126.
No se copió ninguna tabla de una BIOS ni de otro emulador. Las tablas de una DS real no se pudieron comparar (sin BIOS
real): lo verificado es el comportamiento de melonDS.
"""
import math
import struct
import sys

SINE_N, PITCH_N, VOL_N = 64, 768, 724


def rnd(x: float) -> int:
    return int(math.floor(x + 0.5))


def sine():
    return [rnd(math.sin(i * math.pi / 128) * 32767) for i in range(SINE_N)]


def pitch():
    return [rnd((2 ** (i / 768) - 1) * 65536) for i in range(PITCH_N)]


def volume():
    out = []
    for i in range(VOL_N):
        k = 2048 if i < 483 else 512 if i < 603 else 256 if i < 663 else 128
        v = min(127, rnd(k * 10 ** ((i - 723) / 200)))
        if i in (602, 662, 722):
            v = 126
        out.append(v)
    return out


def main() -> int:
    s, p, v = sine(), pitch(), volume()
    if len(sys.argv) > 2 and sys.argv[1] == "--check":
        d = open(sys.argv[2], "rb").read()
        w = struct.unpack("<%dI" % (SINE_N + PITCH_N + VOL_N), d[: 4 * (SINE_N + PITCH_N + VOL_N)])
        ok = list(w) == s + p + v
        print("coincide en las %d entradas" % len(w) if ok else "NO coincide")
        if not ok:
            for i, (a, b) in enumerate(zip(w, s + p + v)):
                if a != b:
                    print("  índice %d: volcado %d, fórmula %d" % (i, a, b))
        return 0 if ok else 1
    print("/* Generado por tools/gen-bios-tables.py: tablas de las SWI 1Ah-1Ch del ARM7 (ver ese script). */")
    for name, t, ty in (("ref_sine", s, "uint16_t"), ("ref_pitch", p, "uint16_t"), ("ref_volume", v, "uint8_t")):
        print("static const %s %s[%d] = {" % (ty, name, len(t)))
        for i in range(0, len(t), 12):
            print("\t" + ", ".join("%d" % x for x in t[i : i + 12]) + ",")
        print("};")
    return 0


if __name__ == "__main__":
    sys.exit(main())
