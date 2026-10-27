#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Alinea dos WAV por correlación y compara saltos por fase, envolvente y espectro."""

import argparse
import json
import struct
import sys
from pathlib import Path

import numpy as np


def read_wav(path):
    data = path.read_bytes()
    if data[:4] != b"RIFF" or data[8:12] != b"WAVE":
        raise ValueError("no es un WAV RIFF")
    pos, fmt, raw = 12, None, None
    while pos + 8 <= len(data):
        kind, size = struct.unpack_from("<4sI", data, pos)
        body = data[pos + 8:pos + 8 + size]
        if kind == b"fmt ": fmt = body
        if kind == b"data": raw = body
        pos += 8 + size + (size & 1)
    if fmt is None or raw is None:
        raise ValueError("faltan fmt o data")
    code, channels, rate, _, align_bytes, bits = struct.unpack_from("<HHIIHH", fmt)
    width = bits // 8
    if channels != 2 or (code, width) not in ((1, 2), (3, 4)) or align_bytes != channels * width:
        raise ValueError("se requiere WAV estéreo PCM16 o float32")
    data = np.frombuffer(raw, dtype="<i2" if width == 2 else "<f4").astype(np.float64)
    if width == 2:
        data /= 32768.0
    return rate, data.reshape(-1, 2)


def score_at(a, b, lag):
    if lag >= 0:
        x, y = a[lag:], b[:len(a) - lag]
    else:
        x, y = a[:len(a) + lag], b[-lag:]
    n = min(len(x), len(y))
    x, y = x[:n], y[:n]
    return float(np.dot(x, y) / max(np.linalg.norm(x) * np.linalg.norm(y), 1e-30))


def align(a, b, rate, max_lag):
    dec = 8
    n = min(len(a), len(b), 12 * rate)
    x, y = a[:n:dec].mean(axis=1), b[:n:dec].mean(axis=1)
    x -= x.mean(); y -= y.mean()
    size = 1 << (len(x) + len(y) - 1).bit_length()
    corr = np.fft.irfft(np.fft.rfft(x, size) * np.conj(np.fft.rfft(y, size)), size)
    limit = min(int(max_lag * rate / dec), len(x) - 1, len(y) - 1)
    candidates = np.r_[np.arange(0, limit + 1), np.arange(size - limit, size)]
    coarse = int(candidates[np.argmax(corr[candidates])])
    if coarse > size // 2:
        coarse -= size
    coarse *= dec
    am, bm = a.mean(axis=1), b.mean(axis=1)
    lags = range(coarse - 2 * dec, coarse + 2 * dec + 1)
    lag = max(lags, key=lambda value: score_at(am, bm, value))
    if lag >= 0:
        a, b = a[lag:], b[:len(a) - lag]
    else:
        a, b = a[:len(a) + lag], b[-lag:]
    n = min(len(a), len(b))
    return lag, a[:n], b[:n], score_at(am, bm, lag)


def phase_jumps(x, rate, frame_rate):
    d2 = np.diff(x.mean(axis=1), 2)
    threshold = max(40 * np.median(np.abs(d2)), 0.05)
    events = np.flatnonzero(np.abs(d2) > threshold) + 1
    bins = 64
    hist, _ = np.histogram(np.mod(events, rate / frame_rate), bins=bins,
                           range=(0, rate / frame_rate))
    return {"saltos": int(len(events)),
            "pico_media": float(hist.max() / max(hist.mean(), 1e-30)),
            "fase_pico": float(np.argmax(hist) * rate / frame_rate / bins)}


def envelope_modulation(x, rate):
    block = max(1, rate // 100)
    x = x[:len(x) // block * block]
    env = np.sqrt(np.mean(x.reshape(-1, block, 2) ** 2, axis=(1, 2)))
    active = env > np.quantile(env, 0.1)
    env = env[active]
    return float(np.std(env) / max(np.mean(env), 1e-30))


def spectrum(x, rate):
    mono = x.mean(axis=1)
    size, hop = 4096, 2048
    count = max(0, 1 + (len(mono) - size) // hop)
    if not count:
        raise ValueError("el tramo es demasiado corto")
    frames = np.stack([mono[i * hop:i * hop + size] for i in range(count)])
    power = np.mean(np.abs(np.fft.rfft(frames * np.hanning(size), axis=1)) ** 2, axis=0)
    hz = np.fft.rfftfreq(size, 1 / rate)
    bands = ((20, 250), (250, 1000), (1000, 4000), (4000, 12000), (12000, 20000))
    return [10 * np.log10(max(float(power[(hz >= lo) & (hz < hi)].sum()), 1e-30)) for lo, hi in bands]


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("ps3", type=Path)
    ap.add_argument("host", type=Path)
    ap.add_argument("--frame-rate", type=float, default=59.8261)
    ap.add_argument("--max-lag", type=float, default=3.0)
    args = ap.parse_args()
    rp, ps3 = read_wav(args.ps3)
    rh, host = read_wav(args.host)
    if rp != rh:
        raise ValueError("los WAV deben tener la misma tasa")
    lag, ps3, host, correlation = align(ps3, host, rp, args.max_lag)
    residual = ps3 - host
    signal = np.sqrt(np.mean(host ** 2))
    noise = np.sqrt(np.mean(residual ** 2))
    sp, sh = spectrum(ps3, rp), spectrum(host, rp)
    result = {
        "desfase_muestras": lag, "desfase_ms": 1000 * lag / rp,
        "correlacion": correlation, "segundos_comparados": len(ps3) / rp,
        "snr_db": 20 * np.log10(max(signal / max(noise, 1e-30), 1e-30)),
        "saltos_ps3": phase_jumps(ps3, rp, args.frame_rate),
        "saltos_host": phase_jumps(host, rp, args.frame_rate),
        "modulacion_ps3": envelope_modulation(ps3, rp),
        "modulacion_host": envelope_modulation(host, rp),
        "bandas_hz": ["20-250", "250-1000", "1000-4000", "4000-12000", "12000-20000"],
        "espectro_ps3_db": sp, "espectro_host_db": sh,
        "diferencia_espectral_db": [a - b for a, b in zip(sp, sh)],
    }
    print(json.dumps(result, indent=2, ensure_ascii=False))
    return 0


if __name__ == "__main__":
    sys.exit(main())
