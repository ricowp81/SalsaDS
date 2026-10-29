#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Comprueba el ensamblado fp40 real con COLOR alias de R0.

Intérprete de las operaciones usadas por niebla; no simula el raster RSX.
Detecta colisiones con COLOR/R0 y DEPTH/R1.z, y bordes Z de la LUT.
No acredita paridad PS3.
"""
import math
from itertools import product
import pathlib
import re
import struct
import sys


def fp32(value):
    return struct.unpack("f", struct.pack("f", value))[0]


class Program:
    def __init__(self, path):
        text = pathlib.Path(path).read_text()
        self.constants = {}
        for index, values in re.findall(r"#const c\[(\d+)\] = ([^\n]+)", text):
            v = [float(x) for x in values.split()]
            self.constants[f"c[{index}]"] = (v + [0.0] * 4)[:4]
        self.code = []
        for line in text.splitlines():
            m = re.fullmatch(r"(TEX|KIL|[A-Z]+R[CH]?)[ ]+(.+);", line)
            if m:
                self.code.append((m[1], m[2].split(", ")))

    def run(self, inputs, textures, with_depth=False):
        registers = {name: [fp32(x) for x in value] for name,value in {**self.constants, **inputs}.items()}
        condition = [0.0] * 4

        def source(token):
            negative = token.startswith("-")
            token = token.removeprefix("-")
            absolute = token.startswith("|")
            if absolute:
                token = token.strip("|")
            m = re.fullmatch(r"(.+?)(?:\.([xyzw]+))?", token)
            name, swizzle = m.groups()
            if name == "oCol":
                name = "R0"
            v = registers.get(name, [0.0] * 4)
            swizzle = swizzle or "xyzw"
            swizzle = (swizzle + swizzle[-1] * 4)[:4]
            v = [v["xyzw".index(c)] for c in swizzle]
            if absolute:
                v = [abs(x) for x in v]
            return [-x for x in v] if negative else v

        for op, args in self.code:
            if op == "KIL":
                if condition[0] != 0:
                    return None
                continue
            dest, *operands = args
            if op == "TEX":
                unit = int(re.fullmatch(r"texture\[(\d+)\]", operands[1])[1])
                value = [fp32(v) for v in textures[unit](source(operands[0]))]
            else:
                a = [source(t) for t in operands]
                fn = {
                    "MOVR": lambda x: x[0], "ADDR": lambda x: x[0] + x[1],
                    "MULR": lambda x: x[0] * x[1], "MADR": lambda x: x[0] * x[1] + x[2],
                    "FLRR": lambda x: math.floor(x[0]), "RCPR": lambda x: 1.0 / x[0],
                    "MINR": lambda x: min(x), "MAXR": lambda x: max(x),
                    "SGER": lambda x: float(x[0] >= x[1]), "SLTR": lambda x: float(x[0] < x[1]),
                }[op.removesuffix("C")]
                value = [fp32(fn([v[c] for v in a])) for c in range(4)]
            if op.endswith("C"):
                condition = value
            # cgcomp exporta DEPTH desde R1.z, también si una escritura
            # posterior a un temporal lo pisa. COLOR comparte R0.
            if dest == "result.depth.z":
                dest = "R1.z"
            name, _, mask = dest.partition(".")
            if name == "oCol":
                name = "R0"
            v = registers.setdefault(name, [0.0] * 4)
            for c in mask or "xyzw":
                i = "xyzw".index(c)
                v[i] = value[i]
        color = registers["R0"]
        return (color, registers.get("R1", [0.0]*4)[2]) if with_depth else color


def density(z15, offset, step, table):
    k = z15 - offset
    i = k // step
    d = table[0] if k < 0 else table[31] if i >= 31 else table[i] + (table[i+1]-table[i])*(k % step)//step
    return 128 if d == 127 else d


def check_attributes(attr, attr_w):
    total = 0
    for program, w in ((attr, False), (attr_w, True)):
        cases = product((1, 3, 12, 127, 255, 256, 4095, 7000, 16384, 32767),
                        (-1, 0, 1, 128, 511), (0, 1), (0, 13, 31), (0, 1, 2))
        for z15, low, flag, alpha, render_pass in cases:
            z = z15 * 512 + low
            shift = 8 if w else 0
            vertex_w = max(1, z >> shift)
            inputs = {"fragment.texcoord[0]": [0.5]*4,
                      "fragment.texcoord[1]": [0, alpha, 0, render_pass],
                      "fragment.texcoord[2]": [0, (1<<shift)/16777215, 0, 1<<shift],
                      # fc: Z>>9 en dos bytes, bandera de plano y bandera de
                      # niebla, por constantes o por vértice (mismo formato).
                      "fragment.texcoord[3]": [(z>>9)//256, (z>>9)%256, 1, flag],
                      "fragment.texcoord[4]": [0, 1024, 0, 0],
                      # WPOS Z-1 reproduce el error medido en planos; la
                      # constante debe conservar ambos lados del borde.
                      "fragment.position": [0, 0, (z-1)/16777215, 1/vertex_w]}
            result = program.run(inputs, {0: lambda uv: [31/255]*4}, with_depth=True)
            survives = alpha > 0 and (alpha < 31) == bool(render_pass)
            assert (result is not None) == survives, ("descarte", w, alpha, render_pass, result)
            total += 1
            if not survives:
                continue
            color, depth = result
            expected_z15 = ((vertex_w << shift) if w else z) >> 9
            want = [expected_z15//256, expected_z15%256, 0, flag*255]
            assert [round(c*255) for c in color] == want, ("borde Z/W", w, z, color, want)
            if w:
                reciprocal = fp32(1/fp32(1/vertex_w))
                expected_depth = min(1.0, fp32(math.floor(reciprocal)*fp32((1<<shift)/16777215)))
                assert depth == expected_depth, ("DEPTH W", z, depth, expected_depth)
            else:
                # Con el indicador de plano apagado se sigue usando WPOS,
                # aunque la constante contenga un valor muy diferente.
                inputs["fragment.texcoord[3]"] = [127, 255, 0, flag]
                fallback = program.run(inputs, {0: lambda uv: [31/255]*4})
                raster_z = fp32(fp32((z-1)/16777215)*16777215 + 0.5)
                expected_z15 = math.floor(raster_z)//512
                want = [expected_z15//256, expected_z15%256, 0, flag*255]
                assert [round(c*255) for c in fallback] == want, ("Z inclinada", z, fallback, want)
                total += 1
    return total


def check(attr, attr_w, final, probe=None):
    total = check_attributes(attr, attr_w)
    for program, w in ((attr, False), (attr_w, True)):
        for z15 in (0, 1, 127, 255, 256, 4095, 7000, 16384, 32767):
            z = z15 * 512 + 128
            for flag in (0, 1):
                inputs = {"fragment.texcoord[0]": [0.5]*4,
                          "fragment.texcoord[1]": [0, 31, 0, 0],
                          "fragment.texcoord[2]": [0, 1/16777215, 0, 1],
                          "fragment.texcoord[3]": [0, 0, 0, flag],
                          "fragment.texcoord[4]": [0, 1024, 0, 0],
                          "fragment.position": [0, 0, z/16777215, 1/z]}
                result, depth = program.run(inputs, {0: lambda uv: [31/255]*4}, with_depth=True)
                if w:
                    expected_depth = fp32(math.floor(fp32(1/fp32(1/z))) * fp32(1/16777215))
                    assert depth == expected_depth, ("DEPTH W", z15, depth, expected_depth)
                got = [round(v*255) for v in result]
                assert got == [z15//256, z15%256, 0, flag*255], ("atributos", w, z15, flag, got)
                total += 1
    cases = product((0, 1, 255, 256, 4096, 7000, 16384, 32767), (0, 7000),
                    (1, 16, 1024), (0, 1), (0, 1), (0, 1, 13, 31))
    for z15, offset, step, flag, alpha_only, alpha in cases:
        table = [(i*37)&127 for i in range(31)] + [127]
        src = [7, 41, 63, alpha]
        fc = [63, 0, 63, 7]
        inputs = {"fragment.texcoord[0]": [0.5]*4,
                  "fragment.texcoord[1]": fc,
                  "fragment.texcoord[2]": [offset, step, alpha_only, 0]}
        textures = {0: lambda uv: [(c+16)/255 for c in src[:3]] + [(src[3]+1)/32 if src[3] else 0.0],
                    1: lambda uv: [z15//256/255, z15%256/255, 0, flag],
                    2: lambda uv: [table[max(0,min(31,int(uv[0])))]/255, 0, 0, 0]}
        result = final.run(inputs, textures)
        got = [round(c*255)-16 for c in result[:3]] + [max(0,round(result[3]*32)-1)]
        d = density(z15, offset, step, table)*flag
        expected = [(c*(128-d)+f*d)//128 for c,f in zip(src,fc)]
        if alpha_only:
            expected[:3] = src[:3]
        assert got == expected, ("final", z15, offset, step, flag, alpha_only, got, expected)
        total += 1
        if probe:
            i = max(0,min(31,(z15-offset)//step))
            stages = [textures[0](None), textures[1](None), [c/255 for c in fc],
                      [table[i]/255,table[min(i+1,31)]/255,d/255,i/255],
                      [offset%256/255,offset//256/255,alpha_only/255,0], result,
                      [step%256/255,step//256/255,alpha_only/255,0]]
            for mode, stage in enumerate(stages,1):
                inputs["fragment.texcoord[3]"] = [0.5,0.5,mode,0]
                for column in (0,1):
                    inputs["fragment.texcoord[0]"][2] = 0.25 + 0.5*column
                    got = probe.run(inputs,textures)
                    want = stage[:3] if not column else [stage[3],0,0]
                    assert [round(c*255) for c in got[:3]] == [round(c*255) for c in want], ("sonda",mode,column,got,want)
                    total += 1
    print(f"niebla fp40: {total} casos, 0 fallos (COLOR comparte R0, DEPTH comparte R1.z)")


if __name__ == "__main__":
    check(*(Program(path) for path in sys.argv[1:]))
