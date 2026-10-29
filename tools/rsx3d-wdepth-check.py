#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""La W por fragmento (wdepth.cgi) debe dar lo mismo en todos los programas.

La DS resuelve el empate frente/espalda con «menor o igual»: dos programas
distintos (opaco, toon, sombra, máscara, rango, niebla, atributos) que escriben
la misma W entera deben producir exactamente el mismo valor de profundidad.
Se ejecuta el ensamblado fp40 de cada programa con el intérprete de
rsx3d-fog-check.py (fp32 por operación) sobre una rejilla de W, wshift y
selector de redondeo, y se exige que todos coincidan entre sí y con la fórmula
de wdepth.cgi. No simula el raster del RSX ni acredita paridad en consola."""
import importlib.util
import math
import pathlib
import sys

spec = importlib.util.spec_from_file_location("fogcheck", pathlib.Path(__file__).with_name("rsx3d-fog-check.py"))
fogcheck = importlib.util.module_from_spec(spec)
spec.loader.exec_module(fogcheck)
fp32 = fogcheck.fp32


def expected(w, shift, rounded):
    reciprocal = fp32(1 / fp32(1 / w))
    if rounded:
        return min(1.0, fp32(math.floor(reciprocal + 0.5) * fp32(fp32(1 << shift) / 16777215)))
    return min(1.0, fp32(math.floor(reciprocal) * fp32((1 << shift) / 16777215)))


def run(program, name, w, shift, rounded):
    translucent = "shadow" in name
    inputs = {"fragment.texcoord[0]": [0.5] * 4,
              "fragment.texcoord[1]": [0, 31, 0, 1 if translucent else 0],
              "fragment.texcoord[2]": [0, (1 << shift) / 16777215, 0, 1 << shift],
              "fragment.texcoord[3]": [0, 0, 1, 1],
              "fragment.texcoord[4]": [0, 1024, 0, 1],
              "fragment.texcoord[5]": [1 if rounded else 0, 0, 0, 0],
              "fragment.color": [1, 1, 1, 1],
              "fragment.position": [0, 0, 0.5, 1 / w]}
    textures = {0: lambda uv: [1.0] * 4, 1: lambda uv: [0.5] * 4, 2: lambda uv: [0.5] * 4}
    result = program.run(inputs, textures, with_depth=True)
    if result is None:
        sys.exit(f"wdepth: {name} descarta el fragmento de prueba")
    return result[1]


if __name__ == "__main__":
    programs = {pathlib.Path(p).name.split(".")[0]: fogcheck.Program(p) for p in sys.argv[1:]}
    cases = 0
    for shift in (0, 4, 8, 12):
        for w in (1, 2, 3, 4095, 4096, 6000, 7000, 8191, 8192, 8193, 9000, 12345, 32767, 32768, 65535):
            if (w << shift) > 16777215 * 2:
                continue
            for rounded in (False, True):
                want = expected(w, shift, rounded)
                for name, program in programs.items():
                    got = run(program, name, w, shift, rounded)
                    if got != want:
                        sys.exit(f"wdepth: {name} W {w} wshift {shift} redondeo {int(rounded)}: {got!r} frente a {want!r}")
                    cases += 1
    print(f"W por fragmento: {len(programs)} programas, {cases} casos iguales entre sí y a wdepth.cgi")
