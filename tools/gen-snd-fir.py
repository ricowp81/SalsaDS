#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Genera src/audio/snd_fir.h: la tabla del FIR polifásico del remuestreo de la salida (32,7 kHz -> 48 kHz).

    tools/gen-snd-fir.py > src/audio/snd_fir.h

Pasabajos sinc con ventana de Kaiser, 32 coeficientes por fase (15 muestras antes y 16 después de la posición) y 64 fases
más una de cierre (65 filas; el remuestreador interpola linealmente entre dos filas). Corte (-6 dB) a 15 kHz: ninguna
imagen por encima de 16,36 kHz pasa sin atenuar. Cada fila se normaliza a suma 1 (una continua sale igual).
"""
import math

RATE = 33513982 / 1024.0
FC = 15000.0
TAPS = 32
BEFORE = 15
PHASES = 64
BETA = 7.0


def i0(x):
    s, t, k = 1.0, 1.0, 1
    while t > 1e-14 * s:
        t *= (x / (2 * k)) ** 2
        s += t
        k += 1
    return s


def h(d):
    fc = 2.0 * FC / RATE  # corte normalizado a la frecuencia de muestreo nativa (en ciclos por muestra, x2)
    half = TAPS / 2.0
    if abs(d) >= half:
        return 0.0
    x = fc * d
    s = fc if abs(x) < 1e-12 else math.sin(math.pi * x) / (math.pi * d)
    return s * i0(BETA * math.sqrt(1.0 - (d / half) ** 2)) / i0(BETA)


print("/* Generado por tools/gen-snd-fir.py: no editar a mano. */")
print("#define SND_FIR_TAPS %d" % TAPS)
print("#define SND_FIR_BEFORE %d" % BEFORE)
print("#define SND_FIR_PHASES %d" % PHASES)
print("static const float snd_fir[SND_FIR_PHASES + 1][SND_FIR_TAPS] __attribute__((aligned(16))) = {")
for p in range(PHASES + 1):
    f = p / PHASES
    row = [h(j - BEFORE - f) for j in range(TAPS)]
    s = sum(row)
    row = [v / s for v in row]
    print("\t{")
    for i in range(0, TAPS, 4):
        print("\t\t" + " ".join("%.9ef," % v for v in row[i:i + 4]))
    print("\t},")
print("};")
