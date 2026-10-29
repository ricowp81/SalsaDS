#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Convierte una fuente BDF de celda fija y 8 píxeles de ancho en la tabla de
glifos de rsxkit (src/font_8x13.h).

  lib/rsxkit/tools/bdf2c.py 8x13.bdf > lib/rsxkit/src/font_8x13.h

Toma ASCII imprimible, Latin-1, el Latin Extended-A del polaco y el turco, el
cirílico básico y unos pocos símbolos para menús (flechas, triángulos, puntos,
rayas). Todos los glifos deben ocupar la celda entera
(BBX igual a FONTBOUNDINGBOX), como en la «fixed» 8x13 de X11.
"""
import sys

EXTRA = [
    0x2013, 0x2014, 0x2018, 0x2019, 0x201C, 0x201D, 0x2022, 0x2026,
    0x2190, 0x2191, 0x2192, 0x2193, 0x2500, 0x2502, 0x25A0, 0x25A1,
    0x25B2, 0x25B3, 0x25B6, 0x25BA, 0x25BC, 0x25C0, 0x25C4, 0x25CB,
    0x25CF, 0xFFFD,
]
# Latin Extended-A que piden el polaco (ĄąĆćĘęŁłŃńŚśŹźŻż) y el turco (ĞğİıŞş),
# y el cirílico básico (U+0400-U+045F: ruso, ucraniano, serbio, macedonio...).
LATIN_EXT_A = [0x104, 0x105, 0x106, 0x107, 0x118, 0x119, 0x11E, 0x11F, 0x130, 0x131, 0x141, 0x142, 0x143,
               0x144, 0x15A, 0x15B, 0x15E, 0x15F, 0x179, 0x17A, 0x17B, 0x17C]
CYRILLIC = list(range(0x400, 0x460))
WANTED = set(range(0x20, 0x7F)) | set(range(0xA0, 0x100)) | set(EXTRA) | set(LATIN_EXT_A) | set(CYRILLIC)


def parse(path):
    glyphs, props = {}, {}
    cell = None
    with open(path, encoding="latin-1") as f:
        lines = iter(f.read().splitlines())
    for line in lines:
        key, _, rest = line.partition(" ")
        if key == "FONTBOUNDINGBOX":
            cell = tuple(int(v) for v in rest.split())
        elif key in ("COPYRIGHT", "FONT", "FONT_ASCENT", "FONT_DESCENT"):
            props[key] = rest.strip('"')
        elif key == "STARTCHAR":
            enc, bbx, rows = None, None, []
            for line in lines:
                key, _, rest = line.partition(" ")
                if key == "ENCODING":
                    enc = int(rest.split()[0])
                elif key == "BBX":
                    bbx = tuple(int(v) for v in rest.split())
                elif key == "BITMAP":
                    for line in lines:
                        if line == "ENDCHAR":
                            break
                        rows.append(int(line, 16))
                    break
            if enc in WANTED:
                if bbx != cell or cell[0] != 8:
                    sys.exit(f"U+{enc:04X}: BBX {bbx} distinto de la celda {cell}")
                glyphs[enc] = rows
    return cell, props, glyphs


def main():
    cell, props, glyphs = parse(sys.argv[1])
    missing = sorted(WANTED - glyphs.keys())
    if missing:
        sys.exit("faltan glifos: " + " ".join(f"U+{c:04X}" for c in missing))
    w, h = cell[0], cell[1]
    print("/* SPDX-License-Identifier: MIT */")
    print("/*")
    print(" * Glifos de la fuente «fixed» 8x13 de X11 (paquete misc-misc de X.Org,")
    print(f" * {props.get('FONT', '')}).")
    print(f" * Su licencia: \"{props.get('COPYRIGHT', '')}\" (dominio público).")
    print(" * Generado con lib/rsxkit/tools/bdf2c.py: no editar a mano.")
    print(" * Una fila por byte, el bit 7 es la columna de la izquierda.")
    print(" */")
    print("#define FONT_GLYPHS %d" % len(glyphs))
    print("#define FONT_W %d" % w)
    print("#define FONT_H %d" % h)
    print("#define FONT_ASCENT %s" % props.get("FONT_ASCENT", h))
    print()
    print("/* Puntos de código, en orden (búsqueda binaria). */")
    print("static const uint16_t font_cp[FONT_GLYPHS] = {")
    cps = sorted(glyphs)
    for i in range(0, len(cps), 10):
        print("\t" + " ".join(f"0x{c:04X}," for c in cps[i:i + 10]))
    print("};")
    print()
    print("static const uint8_t font_rows[FONT_GLYPHS][FONT_H] = {")
    for c in cps:
        rows = ", ".join(f"0x{r:02X}" for r in glyphs[c])
        print(f"\t{{ {rows} }}, /* U+{c:04X} */")
    print("};")


if __name__ == "__main__":
    main()
