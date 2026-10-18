#!/usr/bin/env python3
"""Copia una ROM de prueba cambiando los cuatro bytes del código de juego de la cabecera (0x0C) por una semilla.

    tools/rom-seed.py gfxrand.nds salida.nds SEMILLA [--keep A:B] [--dbg N]

Con --keep A:B la ROM solo deja los OBJ A..B-1 (bytes reservados 0x15 y 0x16 de la cabecera). Con --dbg N pone
el byte 0x17 (banderas de depuración de gfxrand: 1 sin fondos, 2 datos de OBJ opacos, 4 ceros tras el OBJ 0).

La ROM `gfxrand` lee esa semilla de la copia de la cabecera que hay en RAM (0x027FFE0C). Los dos emuladores cargan la
cabecera igual, así que la misma copia sirve para SalsaDS y para melonDS. La semilla va en little-endian.
"""
import struct
import sys


def main() -> int:
    args = sys.argv[1:]
    if len(args) < 3:
        print(__doc__)
        return 2
    data = bytearray(open(args[0], "rb").read())
    data[0x0C:0x10] = struct.pack("<I", int(args[2], 0) & 0xFFFFFFFF)
    rest = args[3:]
    while rest:
        if rest[0] == "--keep" and len(rest) >= 2:
            a, b = (int(v) for v in rest[1].split(":"))
            data[0x15], data[0x16] = a, b
        elif rest[0] == "--dbg" and len(rest) >= 2:
            data[0x17] = int(rest[1], 0)
        else:
            print(__doc__)
            return 2
        rest = rest[2:]
    open(args[1], "wb").write(data)
    return 0


if __name__ == "__main__":
    sys.exit(main())
