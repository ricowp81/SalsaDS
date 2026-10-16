#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""SalsaDS — pila de un programa SPU: la cadena de llamadas más honda contra
lo que el local store deja libre.

    tools/spu-stack.py --bin spu_gfx.bin [--min BYTES] [--root FUNC]
                       [--edge DE:A ...] [--ignore FUNC ...] objeto.o ...

En el SPE la pila baja desde __stack (arriba del local store) hacia el final
del programa (_end): lo que no ocupan el código, los datos y el bss. Nada
avisa si se pasa: pisa las últimas variables del bss. Esta herramienta suma,
por la cadena de llamadas más honda desde --root (por omisión _start), el
marco de cada función:

- de los .su que deja -fstack-usage junto a cada objeto (objeto.c.o →
  objeto.c.su); un marco «dynamic» (alloca, arreglos variables) no se puede
  acotar y es un error;
- de las funciones sin .su (libgcc, newlib), del prólogo del desensamblado
  (ai $1,$1,-N).

Las llamadas salen de `spu-objdump -d`: brsl/brasl y los saltos de cola
(br/bra al comienzo de otra función). Las indirectas (bisl) no se pueden
seguir: una función alcanzable que las tenga se declara con --edge DE:A (a
quién llama por puntero) o se ignora con --ignore; si no, es un error. Una
recursión también (la cuenta no tendría tope).

Escribe la cadena, los bytes libres y la holgura; sale con 1 si la holgura es
menor que --min o si algo de lo anterior no se puede acotar.
"""
import argparse
import re
import subprocess
import sys

CALL = re.compile(r"\b(?:brsl|brasl)\s+\$\d+,[0-9a-f]+ <([^>+]+)>")
TAIL = re.compile(r"\b(?:br|bra)\s+[0-9a-f]+ <([^>+]+)>")
FUNC = re.compile(r"^([0-9a-f]+) <([^>]+)>:")
FRAME = re.compile(r"\bai\s+\$1,\$1,-(\d+)")
INDIRECT = re.compile(r"\bbisl\b")


def run(*cmd):
    return subprocess.run(cmd, check=True, capture_output=True, text=True).stdout


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--bin", required=True)
    ap.add_argument("--min", type=int, default=0, help="holgura mínima en bytes")
    ap.add_argument("--root", default="_start")
    ap.add_argument("--edge", action="append", default=[], help="DE:A, llamada indirecta conocida")
    ap.add_argument("--ignore", action="append", default=[], help="función cuyas llamadas indirectas no cuentan")
    ap.add_argument("objects", nargs="*")
    a = ap.parse_args()
    errors = []

    frames = {}
    for o in a.objects:
        su = o[:-2] + ".su" if o.endswith(".o") else o + ".su"
        try:
            lines = open(su).read().splitlines()
        except OSError:
            errors.append(f"falta {su} (¿compilado sin -fstack-usage?)")
            continue
        for line in lines:
            parts = line.split("\t")
            if len(parts) < 3:
                continue
            name = parts[0].rsplit(":", 1)[-1]
            if "dynamic" in parts[2] and "bounded" not in parts[2]:
                errors.append(f"marco sin tope: {name} ({su})")
            frames[name] = max(frames.get(name, 0), int(parts[1]))

    calls, indirect, prologue, cur = {}, {}, {}, None
    for line in run("spu-objdump", "-d", a.bin).splitlines():
        m = FUNC.match(line)
        if m:
            cur = m.group(2)
            calls.setdefault(cur, set())
            indirect.setdefault(cur, 0)
            continue
        if cur is None:
            continue
        m = CALL.search(line) or TAIL.search(line)
        if m and m.group(1) != cur:
            calls[cur].add(m.group(1))
        if INDIRECT.search(line):
            indirect[cur] += 1
        m = FRAME.search(line)
        if m and cur not in prologue:
            prologue[cur] = int(m.group(1))
    for e in a.edge:
        src, dst = e.split(":", 1)
        if src not in calls or dst not in calls:
            errors.append(f"--edge {e}: no está en el binario")
            continue
        calls[src].add(dst)

    syms = {}
    for line in run("spu-nm", a.bin).splitlines():
        p = line.split()
        if len(p) == 3:
            syms[p[2]] = int(p[0], 16)
    if "_end" not in syms or "__stack" not in syms:
        errors.append("sin _end o __stack en el binario")
        free = 0
    else:
        free = syms["__stack"] - syms["_end"]

    root = a.root if a.root in calls else "main"
    memo, unknown = {}, set()

    def frame(f):
        return frames.get(f, frames.get(f.split(".")[0], prologue.get(f, 0)))

    def depth(f, path):
        if f in path:
            errors.append("recursión: " + " → ".join(path[path.index(f):] + (f,)))
            return 0, []
        if f in memo:
            return memo[f]
        if indirect.get(f) and f not in a.ignore and not any(e.startswith(f + ":") for e in a.edge):
            unknown.add(f)
        best = (0, [])
        for c in sorted(calls.get(f, ())):
            r = depth(c, path + (f,))
            if r[0] > best[0]:
                best = r
        res = (frame(f) + best[0], [f"{f} {frame(f)}"] + best[1])
        memo[f] = res
        return res

    total, chain = depth(root, ())
    for f in sorted(unknown):
        errors.append(f"llamadas indirectas sin declarar en {f} (--edge o --ignore)")
    slack = free - total
    name = a.bin.rsplit("/", 1)[-1]
    print(f"          pila: {free} bytes libres, la cadena más honda usa {total}, holgura {slack}"
          f" (mínimo {a.min})")
    print("            " + " → ".join(chain))
    for e in errors:
        print(f"{name}: {e}", file=sys.stderr)
    if errors or slack < a.min:
        if slack < a.min:
            print(f"{name}: la pila del SPE no alcanza: holgura {slack} < {a.min} bytes", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
