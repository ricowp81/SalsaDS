#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Comprueba el fp40 de bordes con COLOR/R0 y DEPTH/R1.z reales.

Reutiliza el intérprete fp32 de niebla. Verifica atributos Z24, candidatos
geométricos, OID, descartes y los cuatro vecinos, incluidos los exteriores.
No simula cobertura/interpolación del raster RSX ni acredita paridad PS3.
"""
import importlib.util
from itertools import product
import math
import pathlib
import sys

spec = importlib.util.spec_from_file_location("fogcheck", pathlib.Path(__file__).with_name("rsx3d-fog-check.py"))
fogcheck = importlib.util.module_from_spec(spec)
spec.loader.exec_module(fogcheck)
fp32 = fogcheck.fp32


def bytes24(value):
    return [value >> 16, (value >> 8) & 255, value & 255]


def rgba(value):
    return [c / 255 for c in value]


def check_attributes(program, wbuffer, shadow):
    cases = 0
    depths = (1, 255, 256, 511, 512, 65535, 65536, 0x7fffff, 0x800001, 0xfffffe, 0xffffff)
    # Cada máscara tiene interiores y aristas; la última marca toda la fila.
    spans = ((0, 256), (80, 150), (256, 0))
    positions = (0, 79, 80, 149, 150, 255)
    for index, (z, span, x, scale, flat) in enumerate(product(depths, spans, positions, (1, 2, 4), (0, 1))):
        polyid = (index * 7) % 64
        polygon = (index * 23) % 2048
        y = (index * 11) % 192
        alpha = (0, 13, 31)[index % 3]
        render_pass = (0, 1, 2)[(index // 3) % 3]
        textured = (index // 9) % 2
        decal = (index // 18) % 2
        texture_alpha = (0, 12, 31)[(index // 36) % 3]
        shaded_alpha = alpha if not textured or decal else ((texture_alpha + 1) * (alpha + 1) - 1) // 32
        alpha_ref = (0, 9, 14)[(index // 108) % 3]
        shift = (0, 4, 8)[index % 3] if wbuffer else 0
        vertex_w = max(1, z >> shift)
        source_z = max(0, z - 1)  # el plano debe recuperar también los nueve bits bajos
        inputs = {
            "fragment.texcoord[0]": [0.5] * 4,
            "fragment.texcoord[1]": [textured, alpha, decal, render_pass],
            "fragment.texcoord[2]": [alpha_ref, (1 << shift) / 16777215, 0, 1 << shift],
            "fragment.texcoord[3]": [polygon >> 8, polygon & 255, polyid, 0],
            "fragment.texcoord[4]": [scale, 0, 0, 0],
            "fragment.texcoord[5]": [1, 0, 0, 0],
            "fragment.position": [x * scale + 0.5, y * scale + 0.5, source_z / 16777215, 1 / vertex_w],
            "fragment.color": [1, 1, 1, 1],
        }
        row = [span[0] >> 8, span[0] & 255, span[1] >> 8, span[1] & 255]

        def rows(uv):
            px, py = math.floor(uv[0] * 256), math.floor(uv[1] * 2048)
            assert py == polygon, ("índice de polígono", py, polygon)
            assert px in (y, 192), ("fila nativa", px, y, scale)
            return rgba(bytes24(z) + [flat] if px == 192 else row)

        result = program.run(inputs, {0: lambda uv: [1, 1, 1, texture_alpha / 255], 1: rows}, with_depth=True)
        translucent = shadow or shaded_alpha < 31
        survives = shaded_alpha > alpha_ref and translucent == bool(render_pass)
        assert (result is not None) == survives, ("descarte", wbuffer, shadow, index, shaded_alpha, render_pass, result)
        cases += 1
        if not survives:
            continue
        color, depth = result
        if wbuffer:
            expected_z = min(16777215, math.floor(fp32(1 / fp32(1 / vertex_w)) + 0.5) * (1 << shift))
            assert depth == fogcheck.w_depth(vertex_w, shift, True), ("DEPTH W", index, depth)
        elif flat:
            expected_z = z
        else:
            expected_z = min(16777215, math.floor(fp32(fp32(source_z / 16777215) * 16777215 + 0.5)))
        edge = x < span[0] or x >= span[1]
        expected_a = 255 if translucent else polyid | (64 if edge else 0)
        actual = [round(c * 255) for c in color]
        expected = bytes24(expected_z) + [expected_a]
        assert actual == expected, ("atributos Z24/arista/OID", wbuffer, shadow, index, actual, expected)
    return cases


def check_final(program):
    cases = 0
    depths = (0, 1, 255, 256, 65535, 65536, 0x7fffff, 0x800000, 0xfffffe, 0xffffff)
    locations = ((0, 0), (0, 91), (255, 91), (120, 0), (120, 191), (255, 191), (120, 91))
    colors = [[16 + i * 7, 79 - i * 5, 19 + i * 8, 0] for i in range(8)]
    for index, (z, location, direction, relation, scale) in enumerate(product(depths, locations, range(4), (-1, 0, 1), (1, 2, 4))):
        polyid = (index * 13) % 64
        candidate = bool((index // 3) % 2)
        changed = bool((index // 6) % 2)
        center = bytes24(z) + [polyid | (64 if candidate else 0)]
        neighbor_z = max(0, min(16777215, z + relation))
        neighbor_id = (polyid + 1) % 64 if changed else polyid
        neighbor = bytes24(neighbor_z) + [neighbor_id | (64 if index % 2 else 0)]
        clear_z = (0, z, min(z + 1, 16777215), 16777215)[index % 4]
        clear_id = (polyid, 63, 0, (polyid + 7) % 64)[(index // 4) % 4]
        clear = bytes24(clear_z) + [clear_id]
        x, y = location[0] * scale + scale // 2, location[1] * scale + scale // 2
        width, height = 256 * scale, 192 * scale
        coords = ((x - scale, y), (x + scale, y), (x, y - scale), (x, y + scale))
        grid = {(x, y): center}
        adjacent = []
        for i, coord in enumerate(coords):
            value = neighbor if i == direction else center
            if coord[0] < 0 or coord[0] >= width or coord[1] < 0 or coord[1] >= height:
                value = clear
            else:
                grid[coord] = value
            adjacent.append(value)

        def attributes(uv):
            coord = (max(0, min(width - 1, math.floor(uv[0]))), max(0, min(height - 1, math.floor(uv[1]))))
            return rgba(grid.get(coord, center))

        def table(uv):
            i = math.floor(uv[0])
            assert 0 <= i < 8, ("tabla de bordes", uv)
            return rgba(colors[i])

        source = [21, 53, 77, (0, 8, 128, 247, 255)[index % 5]]
        inputs = {"fragment.texcoord[0]": [x + 0.5, y + 0.5, 0, 0],
                  "fragment.texcoord[1]": [width, height, scale, 0],
                  "fragment.texcoord[2]": clear}
        result = program.run(inputs, {0: lambda uv: rgba(source), 1: attributes, 2: table})
        mark = candidate and any((at[3] & 63) != polyid and z < (at[0] << 16 | at[1] << 8 | at[2]) for at in adjacent)
        expected = (colors[polyid >> 3][:3] if mark else source[:3]) + [source[3]]
        actual = [round(c * 255) for c in result]
        assert actual == expected, ("vecinos/color/alfa", index, center, adjacent, actual, expected)
        cases += 1
    return cases


if __name__ == "__main__":
    if len(sys.argv) != 6:
        sys.exit("uso: rsx3d-edge-check.py edge_attr.fixed edge_attr_w.fixed edge_attr_shadow.fixed edge_attr_shadow_w.fixed edge_final.fixed")
    programs = [fogcheck.Program(p) for p in sys.argv[1:]]
    total = sum(check_attributes(p, bool(i & 1), bool(i & 2)) for i, p in enumerate(programs[:4]))
    total += check_final(programs[4])
    print(f"Bordes fp40: 5 programas, {total} casos de atributos Z24, geometría, vecinos y alfa correctos")
