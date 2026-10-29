#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Mide discontinuidades y tonos ligados al cuadro de la DS en un WAV."""

import argparse
import array
import math
import struct
import sys


def read_wav(path):
    with open(path, "rb") as stream:
        data = stream.read()
    if data[:4] != b"RIFF" or data[8:12] != b"WAVE":
        raise ValueError("no es un WAV RIFF")
    pos = 12
    fmt = raw = None
    while pos + 8 <= len(data):
        kind, size = struct.unpack_from("<4sI", data, pos)
        body = data[pos + 8:pos + 8 + size]
        if kind == b"fmt ":
            fmt = body
        elif kind == b"data":
            raw = body
        pos += 8 + size + (size & 1)
    if fmt is None or raw is None or len(fmt) < 16:
        raise ValueError("faltan fmt o data")
    code, channels, rate, _, align, bits = struct.unpack_from("<HHIIHH", fmt)
    if channels < 1 or align * 8 != channels * bits:
        raise ValueError("formato de muestras no admitido")
    if code == 1 and bits == 16:
        values = array.array("h")
        values.frombytes(raw)
        if sys.byteorder != "little":
            values.byteswap()
        scale = 32768.0
    elif code == 3 and bits == 32:
        values = array.array("f")
        values.frombytes(raw)
        if sys.byteorder != "little":
            values.byteswap()
        scale = 1.0
    else:
        raise ValueError("solo se admiten PCM16 y float32")
    frames = len(values) // channels
    signal = [[values[i * channels + c] / scale for i in range(frames)] for c in range(channels)]
    return rate, signal


def percentile_middle(values):
    values.sort()
    return values[len(values) // 2]


def phase_metric(samples, period, width, factor, absolute):
    second = [samples[i + 1] - 2.0 * samples[i] + samples[i - 1] for i in range(1, len(samples) - 1)]
    median = percentile_middle([abs(x) for x in second])
    limit = max(factor * median, absolute)
    bins = max(1, int(math.ceil(period / width)))
    hist = [0] * bins
    events = 0
    for i, value in enumerate(second, 1):
        if abs(value) > limit:
            phase = int((i % period) / width)
            if phase >= bins:
                phase = bins - 1
            hist[phase] += 1
            events += 1
    mean = events / bins if events else 0.0
    peak = max(hist) if hist else 0
    top = max(range(bins), key=hist.__getitem__) if events else 0
    return median, events, peak / mean if mean else 0.0, top * width, hist


def goertzel(samples, window, rate, frequency):
    omega = 2.0 * math.pi * frequency / rate
    coeff = 2.0 * math.cos(omega)
    a = b = 0.0
    for x, w in zip(samples, window):
        v = x * w + coeff * a - b
        b, a = a, v
    power = a * a + b * b - coeff * a * b
    return 10.0 * math.log10(max(power, 1e-30))


def harmonics(samples, rate, frame_rate, count):
    n = len(samples)
    window = [0.5 - 0.5 * math.cos(2.0 * math.pi * i / max(1, n - 1)) for i in range(n)]
    result = []
    for harmonic in range(1, count + 1):
        frequency = frame_rate * harmonic
        if frequency + 3.0 >= rate / 2.0:
            break
        tone = goertzel(samples, window, rate, frequency)
        floor = 0.5 * (goertzel(samples, window, rate, frequency - 3.0) +
                       goertzel(samples, window, rate, frequency + 3.0))
        result.append((harmonic, frequency, tone, tone - floor))
    return result


def analyze(path, args):
    rate, channels = read_wav(path)
    effective_rate = 32728.498 if rate == 32728 else float(rate)
    period = args.period or effective_rate / args.frame_rate
    print(f"{path}: {rate} Hz, {len(channels)} canales, {len(channels[0]) / rate:.3f} s; periodo {period:.5f} muestras")
    ratios = []
    for number, samples in enumerate(channels):
        median, events, ratio, phase, _ = phase_metric(samples, period, args.bin, args.factor, args.absolute)
        ratios.append(ratio)
        print(f"  canal {number}: mediana |d2| {median:.8f}, saltos {events}, pico/media {ratio:.3f}, fase pico {phase:.1f}")
    mono = [sum(values) / len(values) for values in zip(*channels)]
    tones = harmonics(mono, effective_rate, args.frame_rate, args.harmonics)
    for harmonic, frequency, tone, prominence in tones:
        print(f"  H{harmonic:02d} {frequency:8.3f} Hz: nivel {tone:8.2f} dB, sobre vecinos {prominence:6.2f} dB")
    return max(ratios, default=0.0), tones, rate, channels, effective_rate


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("wav")
    parser.add_argument("--reference", help="WAV alineado de referencia; analiza el espectro del residuo")
    parser.add_argument("--frame-rate", type=float, default=59.8261)
    parser.add_argument("--period", type=float, help="periodo del cuadro en muestras")
    parser.add_argument("--bin", type=float, default=10.0, help="ancho de la cubeta de fase en muestras")
    parser.add_argument("--factor", type=float, default=40.0)
    parser.add_argument("--absolute", type=float, default=0.05)
    parser.add_argument("--harmonics", type=int, default=12)
    parser.add_argument("--check", action="store_true", help="falla si pico/media supera 1,5 o aparece un armónico nuevo")
    args = parser.parse_args()
    ratio, tones, rate, channels, effective_rate = analyze(args.wav, args)
    bad_tones = []
    if args.reference:
        ref_rate, reference = read_wav(args.reference)
        if ref_rate != rate or len(reference) != len(channels) or len(reference[0]) != len(channels[0]):
            raise ValueError("la referencia debe tener la misma tasa, canales y longitud")
        residual = [sum(channels[c][i] - reference[c][i] for c in range(len(channels))) / len(channels)
                    for i in range(len(channels[0]))]
        residual_tones = harmonics(residual, effective_rate, args.frame_rate, args.harmonics)
        print("residuo frente a la referencia:")
        for harmonic, frequency, tone, prominence in residual_tones:
            print(f"  H{harmonic:02d} {frequency:8.3f} Hz: nivel {tone:8.2f} dB, sobre vecinos {prominence:6.2f} dB")
            if prominence > 10.0:
                bad_tones.append((frequency, prominence))
        if bad_tones:
            print("armónicos en el residuo:", ", ".join(f"{f:.2f} Hz (+{db:.1f} dB)" for f, db in bad_tones))
    if args.check and (ratio > 1.5 or bad_tones):
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
