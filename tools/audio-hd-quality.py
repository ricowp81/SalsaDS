#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Mide audio-hd-quality.c: ajuste senoidal, ruido residual, THD e imágenes.
SNR incluye distorsión e imágenes (SINAD); SNR sin armónicos se informa aparte.
Silencio se expresa en dBFS RMS: no tiene SNR finita. Python + numpy.
"""
import sys
from pathlib import Path
import numpy as np

root = Path(sys.argv[1])

def db(x):
    return 20*np.log10(max(float(x), 1e-30)) if x else -np.inf

for rate in (8000, 16000, 22000):
    native = float(np.float32(32728.498))
    fs = native*512/round(native*512/rate)
    f = fs/16
    for level in (0, 1, 2):
        results = []
        for mode in range(3):
            x = np.fromfile(root/f'{rate}-{level}-{mode}.f32', dtype='<f4').reshape(-1, 2)[48000:144000, 0].astype(float)
            assert np.isfinite(x).all()
            if level == 2:
                rms = np.sqrt(np.mean(x*x))
                print(f'{rate} silencio modo {mode}: {db(rms):.2f} dBFS, pico {max(abs(x)):.9g}')
                assert rms == 0
                continue
            t = np.arange(len(x))/48000
            # Ajuste conjunto: evita fuga de la fundamental hacia los armónicos.
            frequencies = [f*k for k in range(1, 11)]
            frequencies += [fs-f]  # primera imagen de la retención de orden cero
            cols = [np.ones(len(x))]
            for hz in frequencies:
                cols.extend([np.sin(2*np.pi*hz*t), np.cos(2*np.pi*hz*t)])
            design = np.array(cols).T
            coef = np.linalg.lstsq(design, x, rcond=None)[0]
            amplitude = np.hypot(coef[1::2], coef[2::2])
            noise = np.sqrt(np.mean((x-design@coef)**2))
            signal = amplitude[0]/np.sqrt(2)
            residual = np.sqrt(np.mean((x-design[:, :3]@coef[:3])**2))
            thd = np.linalg.norm(amplitude[1:10])/amplitude[0]
            image = db(amplitude[-1]/amplitude[0])
            sinad = db(signal/residual)
            window = np.hanning(len(x))
            spectrum = 2*abs(np.fft.rfft((x-coef[0])*window, 4*len(x)))/sum(window)
            hz = np.fft.rfftfreq(4*len(x), 1/48000)
            spectrum[hz < f+20] = 0
            peak = int(np.argmax(spectrum))
            spur = db(spectrum[peak]/amplitude[0])
            print(f'{rate} nivel {level} modo {mode}: SINAD {sinad:.2f} dB, SNR {db(signal/noise):.2f} dB, ruido {db(noise):.2f} dBFS, imagen {image:.2f} dBc, THD2..10 {100*thd:.5f}%, espurio superior {spur:.2f} dBc a {hz[peak]:.2f} Hz')
            results.append((sinad, image))
        if level == 0:
            assert results[2][0] > results[0][0] + 15
            # A 22 kHz el FIR fiel ya elimina la primera imagen; mirar también SINAD.
            if rate < 22000:
                assert results[2][1] < results[0][1] - 20
print('HD: guardas de señal y silencio correctas')
