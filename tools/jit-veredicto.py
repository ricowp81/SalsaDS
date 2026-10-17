#!/usr/bin/env python3
"""Veredicto de viabilidad del JIT (Fase 0.7) a partir del log de jit-arm.

  tools/jit-veredicto.py ruta/a/jit-arm.log

Lee las líneas `DATO jit …` y `DATO hilos …` y dice, con números: si el ARM9 a
67 MHz cabe en el hilo 0 del PPE con el código traducido (y con cuánta holgura),
cuánto le quita el ARM7 en el hilo 1 según cómo espere, y, si no cabe, cuánto
falta. Reglas (fase0.md, 0.7 «Veredicto»): el ARM9 cabe si el peor bucle del
mejor modo de banderas da ≥ 67 MIPS con 1 ciclo por instrucción; holgura = MIPS /
67; con el ARM7 en el hilo 1 se resta la pérdida medida del ARM9; se exige una
holgura de al menos 1,5× para dejar sitio al resto del núcleo (memoria con tabla
de páginas, E/S, IRQ, DMA, temporizadores).
"""
import re
import sys

META = 67.0
HOLGURA_MIN = 1.5

jit = {}      # bucle -> impl -> MIPS
hilos = []    # (espera, periodo, mips, delta%, lat_med_us, lat_p99_us)
base = None
for line in open(sys.argv[1], encoding="utf-8", errors="replace"):
    m = re.search(r"DATO jit (\w+)\s+(.+?)\s*: [\d.]+ ns/instr\. guest = ([\d.]+) MIPS", line)
    if m:
        jit.setdefault(m.group(1), {})[m.group(2).strip()] = float(m.group(3))
        continue
    m = re.search(r"DATO hilos sin hilo del ARM7\s+ARM9 ([\d.]+) MIPS", line)
    if m:
        base = float(m.group(1))
        continue
    m = re.search(r"DATO hilos (.+?)\s+despertar cada\s+(\d+) µs: ARM9 ([\d.]+) MIPS \(([+-][\d.]+) %.*mediana ([\d.]+) µs, p99 ([\d.]+)", line)
    if m:
        hilos.append((m.group(1).strip(), int(m.group(2)), float(m.group(3)), float(m.group(4)), float(m.group(5)), float(m.group(6))))

jits = {l: {k: v for k, v in d.items() if k.startswith("JIT")} for l, d in jit.items()}
jits = {l: d for l, d in jits.items() if d}
if not jits:
    sys.exit("el log no trae mediciones del JIT (¿corrió en RPCS3 o con jit-arm-nojit?)")

print("== (a) MIPS de ARM9 del código traducido (1 ciclo por instrucción), meta %.0f" % META)
modos = sorted({k for d in jits.values() for k in d})
for modo in modos:
    peor = min(d[modo] for d in jits.values() if modo in d)
    print("  %-34s peor bucle %7.1f MIPS = %.1f× la meta" % (modo, peor, peor / META))
mejor_modo = max(modos, key=lambda k: min(d[k] for d in jits.values() if k in d))
peor = min(d[mejor_modo] for d in jits.values() if mejor_modo in d)
print("  mejor modo: %s; peor bucle %.1f MIPS" % (mejor_modo, peor))
for l, d in jits.items():
    o = jit[l].get("intérprete de referencia (switch, banderas completas)")
    o3 = jit[l].get("intérprete de la 0.3 (goto calculado)")
    print("  %-8s %s%s" % (l, ", ".join("%s %.0f" % (k.replace("JIT, ", ""), v) for k, v in d.items()),
                           (" ; ref %.0f" % o if o else "") + (" ; 0.3 %.0f" % o3 if o3 else "")))

holg = peor / META
print("\nVEREDICTO ARM9: %s" % ("CABE" if peor >= META else "NO CABE"))
print("  holgura %.2f× (%.0f MIPS libres sobre los %.0f) %s" % (holg, peor - META, META,
      "≥ %.1f× exigido" % HOLGURA_MIN if holg >= HOLGURA_MIN else "< %.1f× exigido para el resto del núcleo" % HOLGURA_MIN))
if peor < META:
    print("  faltan %.1f MIPS (%.1f×): palancas: banderas fusionadas/muertas, enlace de bloques, registros guest fijos, menos stores" % (META - peor, META / peor))

if hilos and base:
    print("\n== (c) ARM7 en el hilo 1 (ARM9 sin hilo del ARM7: %.1f MIPS)" % base)
    for nombre, per, mips, delta, med, p99 in hilos:
        ok = (peor * (1 + delta / 100.0)) >= META
        print("  %-34s cada %4d µs: ARM9 %6.1f MIPS (%+5.1f %%), despertar mediana %6.1f µs, p99 %6.1f µs -> ARM9 en el JIT quedaría en %.1f MIPS: %s"
              % (nombre, per, mips, delta, med, p99, peor * (1 + delta / 100.0), "cabe" if ok else "NO cabe"))
    giran = [h for h in hilos if h[0].startswith("gira")]
    duermen = [h for h in hilos if h[0].startswith(("semáforo", "cola", "duerme"))]
    if giran and duermen:
        print("  girar cuesta %.1f %% al ARM9 (peor caso), dormir con una primitiva de lv2 %.1f %%" %
              (min(h[3] for h in giran), min(h[3] for h in duermen)))
