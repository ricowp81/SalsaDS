#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Evita que la salida COLOR (R0 en cgcomp) pise temporales aún vivos."""
import pathlib
import re
import sys

source = pathlib.Path(sys.argv[1]).read_text()
regs = [int(n) for n in re.findall(r"\bR(\d+)\b", source)]
# DEPTH es R1.z aunque el ensamblado no nombre R1. No usarlo para
# acumular COLOR: escribir su azul después de DEPTH borraría la Z/W.
if "result.depth" in source:
    regs.append(1)
out = "R" + str(max(regs, default=0) + 1)
if (int(out[1:]) >= 32 or source.count("END") != 1 or
        source.count("OUTPUT oCol = result.color;") != 1):
    sys.exit("programa fuera de los límites del corrector")
source = source.replace("OUTPUT oCol = result.color;", "OUTPUT oCol = result.color;\nTEMP " + out + ";")
lines = source.splitlines()
for i, line in enumerate(lines):
    if not line.startswith(("#", "OUTPUT")):
        lines[i] = re.sub(r"\boCol\b", out, line)
    if line == "END":
        lines[i] = "MOVR oCol, " + out + ";\nEND"
pathlib.Path(sys.argv[2]).write_text("\n".join(lines) + "\n")
