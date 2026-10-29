#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Separa SND_TRACE en WAV por voz y mide energía/saltos, sin confundirlos con SNR.

Uso: audio-voice-report.py PREFIJO --native mezcla.wav --from-tick N
SND_TRACE_FROM debe ser el mismo N. Requiere numpy. Todo archivo es local.
"""
import argparse
import csv
import json
import wave
from pathlib import Path
import numpy as np


def wav(path, x, rate=32728):
    with wave.open(str(path), "wb") as f:
        f.setparams((x.shape[1], 2, rate, 0, "NONE", "not compressed"))
        f.writeframes(x.astype("<i2").tobytes())


def metrics(x):
    x = x.astype(np.float64)
    rms = np.sqrt(np.mean(x*x, axis=0))
    d2 = np.diff(x, n=2, axis=0)
    n = 4096
    blocks = x[:len(x)//n*n].reshape(-1, n, x.shape[1])
    p = np.mean(abs(np.fft.rfft(blocks*np.hanning(n)[None, :, None], axis=1))**2, axis=0)
    f = np.fft.rfftfreq(n, 1/32728)
    bands = {}
    for lo, hi in ((0, 1000), (1000, 4000), (4000, 8000), (8000, 16365)):
        energy = p[(f >= lo) & (f < hi)].sum(axis=0)
        bands[f"{lo}-{hi}_fraction"] = (energy / np.maximum(p.sum(axis=0), 1e-30)).tolist()
    return {"rms": rms.tolist(), "rms_d2": np.sqrt(np.mean(d2*d2, axis=0)).tolist(),
            "p999_abs_d2": np.quantile(abs(d2), .999, axis=0).tolist(), **bands}


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("prefix", type=Path)
    ap.add_argument("--native", type=Path, required=True)
    ap.add_argument("--from-tick", type=int, required=True)
    args = ap.parse_args()
    prefix = str(args.prefix)
    raw = np.fromfile(prefix+".raw.pcm", dtype="<i2").reshape(-1, 16)
    voices = np.fromfile(prefix+".voices.pcm", dtype="<i2").reshape(-1, 16, 2)
    if len(raw) != len(voices) or len(raw) < 4096:
        raise ValueError("traza incompleta o demasiado corta")
    with wave.open(str(args.native)) as f:
        if f.getnchannels() != 2 or f.getsampwidth() != 2 or f.getframerate() != 32728:
            raise ValueError("se requiere mezcla nativa PCM16 estéreo a 32728 Hz")
        f.setpos(args.from_tick)
        mix = np.frombuffer(f.readframes(len(raw)), dtype="<i2").reshape(-1, 2)
    # La máquina recoge la mezcla un cuadro después de generarla. La traza
    # puede incluir ese último trabajo aún pendiente al terminar el corredor.
    pending = len(raw)-len(mix)
    if pending < 0 or pending > 640:
        raise ValueError("mezcla y traza no corresponden al mismo tramo")
    raw, voices = raw[:len(mix)], voices[:len(mix)]
    formats = [set() for _ in range(16)]
    timers = [set() for _ in range(16)]
    bias, controls, capture = set(), set(), set()
    with open(prefix+".state.tsv") as f:
        for row in csv.DictReader(f, delimiter="\t"):
            if int(row["on"]):
                i = int(row["ch"])
                formats[i].add(int(row["fmt"]))
                timers[i].add(int(row["tmr"]))
            bias.add(int(row["bias"]))
            controls.add(int(row["soundcnt"]))
            capture.add((int(row["cap0"]), int(row["cap1"])))
    result = {"from_tick": args.from_tick, "samples": len(raw), "pending_ticks": pending,
              "seconds": len(raw)/32728, "bias": sorted(bias),
              "soundcnt": sorted(controls), "capture": sorted(capture), "voices": []}
    for i in range(16):
        wav(f"{prefix}-voz{i:02}.wav", voices[:, i])
        wav(f"{prefix}-cruda{i:02}.wav", raw[:, i:i+1])
        result["voices"].append({"voice": i, "formats": sorted(formats[i]),
                                  "timers": sorted(timers[i]), "weighted": metrics(voices[:, i]),
                                  "raw": metrics(raw[:, i:i+1])})
    wav(prefix+"-tramo.wav", mix)
    result["mix"] = metrics(mix)
    print(json.dumps(result, indent=2))


if __name__ == "__main__":
    main()
