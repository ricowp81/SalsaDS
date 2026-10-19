#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Compara las muestras del mezclador de SalsaDS con las de melonDS en la ROM `snd` (tests/roms/src/snd).

    tools/snd-compare.py [build/roms/snd.nds]            corre las dos y compara caso por caso
    tools/snd-compare.py --melon-dump VOLCADO.bin ...    usa un volcado de melonDS que ya se tiene (tools/melonds-dump.py)

La ROM captura con SNDCAP0/1 lo que sale del mezclador (izquierdo y derecho, PCM16; antes del volumen general) en
0x02300000, 0x400 bytes por caso (izquierdo +0x000, derecho +0x200, 128 muestras cada uno; en +0x300 el SOUNDxCNT de
los 16 canales y en +0x340 y +0x344 los SNDCAPxCNT leídos al final). Es lo único del sonido que un programa puede leer de
vuelta del hardware. Código de salida: 0 si todo coincide, 1 si no, 2 si no se pudo correr alguno.
"""
import argparse
import os
import pathlib
import struct
import subprocess
import sys
import tempfile

HERE = pathlib.Path(__file__).resolve().parent
ADDR = 0x02300000
# Casos donde melonDS 1.1 se aparta de lo que dice GBATEK (no se reproducen; se anotan en docs/fase2.md):
KNOWN = {
    34: "ADPCM con PNT = 0 (el bucle vuelve a la cabecera): melonDS sigue con otro estado de decodificación",
    57: "captura con SNDCAPxCNT pidiendo la salida de los canales 1 y 3: melonDS captura el mezclador",
}
START = 4  # diferencias solo en las primeras muestras: fase de arranque de varios canales (los arranques no caen en el mismo tick)
CASES = 58  # el caso 0 es de calentamiento (su fase respecto de los ticks es arbitraria) y no se compara
SIZE = CASES * 0x400


def run_sds(rom, frames):
    out = pathlib.Path(tempfile.mkdtemp()) / "sds.bin"
    r = subprocess.run([HERE / "cpurun.sh", "--frames", str(frames), "--peek", "0x02390834", "--dump",
                        f"0x{ADDR:08x}:0x{SIZE:x}:{out}", rom], capture_output=True, text=True)
    if not out.exists() or "peek 0x02390834 = 0x00000001" not in r.stdout:
        sys.exit(f"SalsaDS no terminó los casos en {frames} cuadros:\n{r.stdout[-300:]}{r.stderr[-300:]}")
    return out.read_bytes()


def run_melon(rom, wait):
    out = pathlib.Path(tempfile.mkdtemp()) / "mel.bin"
    r = subprocess.run([sys.executable, HERE / "melonds-dump.py", rom, "--wait", str(wait), "--range",
                        f"0x{ADDR:08x}:0x{SIZE:x}:{out}"], capture_output=True, text=True)
    if not out.exists():
        sys.exit(f"melonDS no dio el volcado:\n{r.stdout[-300:]}{r.stderr[-300:]}")
    return out.read_bytes()


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("rom", nargs="?", default=str(HERE.parent / "build/roms/snd.nds"))
    ap.add_argument("--melon-dump")
    ap.add_argument("--frames", type=int, default=600)
    ap.add_argument("--wait", type=float, default=45)
    ap.add_argument("--only", type=int, nargs="*")
    ap.add_argument("--show", type=int, default=0, help="en los casos distintos, muestra las primeras N muestras de cada uno")
    ap.add_argument("--keep", help="guarda los dos volcados con este prefijo")
    args = ap.parse_args()

    sds = run_sds(args.rom, args.frames)
    mel = pathlib.Path(args.melon_dump).read_bytes() if args.melon_dump else run_melon(args.rom, args.wait)
    if args.keep:
        pathlib.Path(args.keep + "-sds.bin").write_bytes(sds)
        pathlib.Path(args.keep + "-mel.bin").write_bytes(mel)
    bad = 0
    for k in range(1, CASES):
        if args.only and k not in args.only:
            continue
        a = sds[k * 0x400:(k + 1) * 0x400]
        b = mel[k * 0x400:(k + 1) * 0x400]
        sa = struct.unpack("<128h", a[:256]) + struct.unpack("<128h", a[0x200:0x300])
        sb = struct.unpack("<128h", b[:256]) + struct.unpack("<128h", b[0x200:0x300])
        d = [i for i in range(256) if sa[i] != sb[i]]
        cnt = a[0x300:0x340] != b[0x300:0x340]
        capc = a[0x340:0x348] != b[0x340:0x348]
        if k in KNOWN and (d or cnt or capc):
            print(f"caso {k:2d}: divergencia conocida ({KNOWN[k]})")
            continue
        if d and all(i % 128 < START for i in d) and not cnt and not capc:
            print(f"caso {k:2d}: igual salvo la fase de arranque ({len(d)} muestras entre las {START} primeras)")
            continue
        if d or cnt or capc:
            bad += 1
            print(f"caso {k:2d}: DIFERENTE ({len(d)} muestras" + (", SOUNDxCNT" if cnt else "") + (", SNDCAPxCNT" if capc else "") + ")")
            if args.show:
                print("   SalsaDS L:", list(sa[:args.show]))
                print("   melonDS L:", list(sb[:args.show]))
        else:
            print(f"caso {k:2d}: igual")
    print("TODO IGUAL" if not bad else f"{bad} casos distintos")
    return 0 if not bad else 1


if __name__ == "__main__":
    sys.exit(main())
