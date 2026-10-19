#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Compara la imagen del motor A de SalsaDS con la de melonDS en una ROM de gráficos (tests/roms/src/gfx).

    tools/gfx-compare.py build/roms/gfxrand.nds 1 2 3 ...          # una corrida por semilla (ROM con semilla)
    tools/gfx-compare.py build/roms/gfxprobe.nds                    # una sola corrida (ROM sin semilla)
    opciones: --frames N (cuadros que corre SalsaDS antes de sacar la imagen; 6)  --keep DIR (deja las imágenes)

SalsaDS corre con `salsads-cpurun` (tools/cpurun.sh, nativo); melonDS con tools/melonds-run.py (MELONDS en el entorno
o en /Applications) y su captura del motor A en el banco A de VRAM (la ROM la dispara). Salida: una línea por corrida.
Código de salida: 0 si todas son iguales, 1 si alguna difiere, 2 si alguna no se pudo correr.
"""
import argparse
import os
import pathlib
import subprocess
import sys
import tempfile

HERE = pathlib.Path(__file__).resolve().parent


def read_ppm(path):
    data = pathlib.Path(path).read_bytes()
    parts = data.split(b"\n", 3)
    w, h = map(int, parts[1].split())
    return w, h, parts[3][: w * h * 3]


def run_one(rom, seed, args, tmp):
    tag = f"semilla {seed}" if seed is not None else "sin semilla"
    src = rom
    if seed is not None:
        src = tmp / f"rom-{seed}.nds"
        cmd = [sys.executable, HERE / "rom-seed.py", rom, src, str(seed)]
        if args.keep_obj:
            cmd += ["--keep", args.keep_obj]
        if args.dbg:
            cmd += ["--dbg", args.dbg]
        subprocess.run(cmd, check=True)
    sds = tmp / f"sds-{seed}.ppm"
    mel = tmp / f"mel-{seed}.ppm"
    r = subprocess.run([HERE / "cpurun.sh", "--frames", str(args.frames), "--screens", sds] +
                       (["--ref"] if args.ref else []) + [src], capture_output=True, text=True)
    if r.returncode != 0 or not sds.exists():
        return 2, f"{tag}: SalsaDS no corrió ({r.stdout[-200:]}{r.stderr[-200:]})"
    cached = pathlib.Path(args.mel_cache) / mel.name if args.mel_cache else None
    if cached and cached.exists():
        mel.write_bytes(cached.read_bytes())  # la imagen de melonDS de otra corrida (la ROM no cambió): sin volver a lanzarlo
        r = None
    else:
        r = subprocess.run([sys.executable, HERE / "melonds-run.py", src, "--vram", mel, "--timeout", "40"],
                           capture_output=True, text=True)
        if cached and mel.exists():
            cached.parent.mkdir(parents=True, exist_ok=True)
            cached.write_bytes(mel.read_bytes())
    if not mel.exists():
        why = (r.stdout[-200:] + r.stderr[-200:]) if r else ""
        return 2, f"{tag}: melonDS no dio imagen ({why})"
    if args.skip_unmapped:
        r2 = subprocess.run([HERE / "cpurun.sh", "--frames", str(args.frames), "--unmapped", src], capture_output=True, text=True)
        for line in r2.stdout.splitlines():
            if line.startswith("VRAM sin banco:") and int(line.split()[3]) > 0:
                return 3, f"{tag}: OMITIDA ({line.strip()}: melonDS espeja zonas sin banco)"
    w1, _, a = read_ppm(sds)
    w2, _, b = read_ppm(mel)
    a = a[: 256 * 192 * 3]
    b = b[: 256 * 192 * 3]
    diff = [i for i in range(256 * 192) if a[3 * i:3 * i + 3] != b[3 * i:3 * i + 3]]
    if args.keep:
        d = pathlib.Path(args.keep)
        d.mkdir(parents=True, exist_ok=True)
        for p in (sds, mel):
            (d / p.name).write_bytes(p.read_bytes())
    if not diff:
        return 0, f"{tag}: IGUALES"
    i = diff[0]
    maxd = max(abs(a[3 * j + c] - b[3 * j + c]) for j in diff[:2000] for c in range(3))
    return 1, (f"{tag}: DIFERENTES {len(diff)} px (máx. delta de canal {maxd}/255); primero en ({i % 256},{i // 256}): "
               f"SalsaDS {a[3*i:3*i+3].hex()}, melonDS {b[3*i:3*i+3].hex()}")


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("rom")
    ap.add_argument("seeds", nargs="*")
    ap.add_argument("--frames", type=int, default=6)
    ap.add_argument("--keep")
    ap.add_argument("--mel-cache", help="carpeta donde guardar y reusar las imágenes de melonDS (mel-<semilla>.ppm): con la misma ROM, iterar sobre SalsaDS sin lanzar melonDS")
    ap.add_argument("--skip-unmapped", action="store_true", help="omite la escena si el motor A lee VRAM sin banco (melonDS espeja esas zonas)")
    ap.add_argument("--ref", action="store_true", help="SalsaDS dibuja con la referencia píxel por píxel (no con el compositor)")
    ap.add_argument("--dbg", help="banderas de depuración de gfxrand (byte 0x17 de la cabecera)")
    ap.add_argument("--keep-obj", help="A:B — solo los OBJ A..B-1 (ROM gfxrand)")
    args = ap.parse_args()
    worst = 0
    with tempfile.TemporaryDirectory(prefix="gfx-compare-") as t:
        tmp = pathlib.Path(t)
        for seed in (args.seeds or [None]):
            code, msg = run_one(os.path.abspath(args.rom), seed, args, tmp)
            print(msg, flush=True)
            worst = max(worst, code if code != 3 else 0)
    return worst


if __name__ == "__main__":
    sys.exit(main())
