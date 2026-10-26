#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Métricas del banco audio-quality.c (Python + numpy); no son MOS ni escucha.
Uso: python3 tools/audio-quality.py build/audio2
"""
import sys
from pathlib import Path
import numpy as np


def measure(path):
    x = np.fromfile(path, dtype='<f4').reshape(-1, 2).astype(float)
    # Descartar el segundo de convergencia; Hann 2048, paso 120 (2,5 ms).
    if not np.isfinite(x).all():
        raise ValueError(f'{path}: muestras no finitas')
    x = x[48000:]
    frames = np.lib.stride_tricks.sliding_window_view(x, 2048, axis=0)[::120]
    spectrum = np.abs(np.fft.rfft(frames * np.hanning(2048), axis=-1))
    power = np.mean(spectrum**2, axis=1)
    magnitude = np.sqrt(power)
    normalized = magnitude / np.maximum(np.linalg.norm(magnitude, axis=1)[:, None], 1e-12)
    flux = np.sum(np.diff(normalized, axis=0)**2, axis=1)
    # Centros de las dos ventanas que rodean cada empalme registrado.
    marks = np.array([int(t) for t in path.with_suffix('.seams').read_text().split()])
    seams = np.zeros(len(flux), dtype=bool)
    if len(marks):
        indices = np.rint((marks - 48000 - 1024 - 60)/120).astype(int)
        indices = indices[(indices >= 0) & (indices < len(flux))]
        seams[indices] = True
    else:
        seams[:] = True  # referencia sin solapes
    freq = np.fft.rfftfreq(2048, 1/48000)
    bands = [(80, 250), (250, 500), (500, 1000), (1000, 4000)]
    # Percusión: banda >6 kHz; excluir 30 ms alrededor de los ataques.
    high = np.sum(power[:, freq > 6000], axis=1)
    quiet = high < np.quantile(high, 0.8)
    quiet = np.convolve(quiet.astype(int), np.ones(25), 'same') == 25
    modulation = []
    for lo, hi in bands:
        env = np.sqrt(np.sum(power[:, (freq >= lo) & (freq < hi)], axis=1))
        env = env[quiet]
        modulation.append(float(np.std(env)/np.mean(env)))
    # Ataques con ventana aparte de 512 muestras (~10,7 ms), para no
    # confundir el ancho de la ventana de análisis musical con el del golpe.
    short = np.lib.stride_tricks.sliding_window_view(x, 512, axis=0)[::120]
    transient = np.abs(np.fft.rfft(short * np.hanning(512), axis=-1))**2
    high = np.sum(np.mean(transient, axis=1)[:, np.fft.rfftfreq(512, 1/48000) > 6000], axis=1)
    peaks = [i for i in range(20, len(high)-20)
             if high[i] == max(high[i-20:i+21]) and high[i] > max(high)*0.15]
    widths, sharpness = [], []
    for i in peaks:
        l, r = i, i
        while l > 0 and high[l] > high[i]/2:
            l -= 1
        while r < len(high)-1 and high[r] > high[i]/2:
            r += 1
        widths.append((r-l)*2.5)
        sharpness.append(high[i]/sum(high[i-20:i+21]))
    return dict(flux=float(np.mean(flux[seams & quiet[1:]])), bands=modulation,
                width_ms=float(np.mean(widths)), sharpness=float(np.mean(sharpness)), attacks=len(peaks))


if __name__ == '__main__':
    for speed in (80, 67, 100):
        results = [measure(Path(sys.argv[1])/f'v{v}-{speed}.f32') for v in (1, 2)]
        rate = 2/3 if speed == 67 else speed/100
        print(f'Producción {100*rate:.3f}% (extensión {100/rate-100:.2f}%):')
        for v, m in enumerate(results, 1):
            print(f'  v{v}: {m}')
        a, b = results
        print(f"  v2/v1 flujo {b['flux']/a['flux']:.4f}, modulación "
              f"{np.mean(b['bands'])/np.mean(a['bands']):.4f}, "
              f"nitidez {b['sharpness']/a['sharpness']:.4f}")

        if speed != 100:
            # Guardas de regresión: mejora musical >=10%, ancho de ataque
            # <=+10% y concentración de energía >=90% de v1.
            assert b['flux'] < 0.9*a['flux']
            assert np.mean(b['bands']) < 0.9*np.mean(a['bands'])
            assert b['width_ms'] <= 1.1*a['width_ms']
            assert b['sharpness'] >= 0.9*a['sharpness']
        else:
            assert (Path(sys.argv[1])/'v1-100.f32').read_bytes() == (Path(sys.argv[1])/'v2-100.f32').read_bytes()
    print('Calidad: guardas e identidad nominal correctas')
