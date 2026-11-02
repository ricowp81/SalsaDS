#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Resume --rsx-censo: admisión por modos y dibujos, nunca tiempos de consola."""
import argparse
import collections
import csv
import pathlib
import statistics


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("csv", type=pathlib.Path)
    parser.add_argument("--inicio", type=int, default=0)
    parser.add_argument("--fin", type=int)
    args = parser.parse_args()
    with args.csv.open(newline="") as stream:
        reader = csv.DictReader(stream)
        required = {"respaldo-antes", "respaldo-sin-bordes", "grupo-bordes", "ampliado", "color", "atributos-bordes"}
        if not required.issubset(reader.fieldnames or ()):
            parser.error("el CSV requiere las columnas del censo de bordes")
        rows = [r for r in reader
                if int(r["cuadro"]) >= args.inicio
                and (args.fin is None or int(r["cuadro"]) < args.fin)]
    print(f"{args.csv.name}: {len(rows)} cuadros con capa 3D")
    print("Admisión por modos; excluye costo, caché de texturas y degradación gráfica.")
    if not rows:
        return
    for key in ("respaldo-antes", "respaldo-sin-bordes"):
        counts = collections.Counter(r[key] for r in rows)
        print(f"{key}: " + "; ".join(f"{name}={n}" for name, n in counts.items()))
    for key in ("bordes-activos", "niebla-activa", "w", "aa", "ampliado"):
        print(f"{key}: {sum(int(r[key]) for r in rows)}")
    groups = collections.Counter(r["grupo-bordes"] for r in rows if int(r["bordes-activos"]))
    print("grupos de bordes: " + "; ".join(f"{group}={n}" for group, n in groups.items()))
    eligible = [r for r in rows if r["respaldo-sin-bordes"] == "admitido"]
    counted = [r for r in eligible if int(r["dibujos-contados"])]
    edge = [r for r in counted if int(r["bordes-activos"])]
    print(f"Candidatos RSX: {len(eligible)}; dibujos contados: {len(counted)}; con bordes: {len(edge)}")
    for name, subset in (("candidatos", counted), ("bordes", edge)):
        if not subset:
            continue
        for key in ("dibujables", "translucidos", "lotes", "color", "atributos-antes", "atributos-bordes", "posprocesos-bordes"):
            values = [int(r[key]) for r in subset]
            print(f"{name} {key}: total {sum(values)}, mínimo {min(values)}, "
                  f"mediana {statistics.median(values):g}, máximo {max(values)}")
        total = sum(int(r["color"]) + int(r["atributos-bordes"])
                    + int(r["posprocesos-bordes"]) for r in subset)
        print(f"{name} dibujos totales por escala: {total}; más un quad de exportación por cuadro")
    print("HD sin captura: una escala; con captura o autoprueba 2×: dos rasterizados.")


if __name__ == "__main__":
    main()
