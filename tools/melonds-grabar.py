#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Graba el audio digital de melonDS (emulador de referencia) mientras «pulsa» botones sin teclado.

  tools/melonds-grabar.py ROM.nds salida.wav --dur 90 --interp 0 \
      --keyinput-pc 0x0205164c --press 10:1 --press 10.33:0 ...

Audio: el SDL que trae melonDS incluye su controlador de audio «disk»
(variables SDL_AUDIODRIVER=disk y SDL_DISKAUDIOFILE): lo que el emulador
manda a la tarjeta de sonido se escribe tal cual en un archivo crudo, al ritmo
real, sin pasar por el mezclador del sistema. Aquí sale a 48000 Hz, PCM16
estéreo (se verifica el ritmo por el tamaño del archivo); el script lo
convierte en WAV. `--interp` fija su interpolación de audio (0 ninguna, como
el hardware; 1 lineal; 2 coseno; 3 cúbica; 4 gaussiana SNES).

Botones: enviar teclas a una aplicación gráfica exige permisos de
accesibilidad que no siempre están. En su lugar se usa el stub de GDB de
melonDS (puerto 3333, ARM9): la instrucción del juego que lee KEYINPUT
(`ldrh rd, [rn, #0x130]`, dirección en `--keyinput-pc`) se sustituye durante
la pulsación por `mvn rd, #teclas` (teclas activas a cero, como el registro) y
se restaura al soltar. Vale para botones de los bits 0-7 (A, B, Select, Start,
derecha, izquierda, arriba, abajo). Cada cambio detiene el ARM9 unos
milisegundos; conviene que las pulsaciones queden fuera del tramo que se
analice. El reloj de las pulsaciones es el audio ya escrito (segundos de
grabación), que avanza al ritmo real.

La dirección se busca en la ROM con `--buscar`: lista las `ldrh` con
desplazamiento 0x130 dentro del binario ARM9. Misma configuración temporal de
melonDS que tools/melonds-run.py (se respalda y restaura). Deja los archivos
de guardado en una copia temporal de la ROM; nunca toca la original.
"""
import argparse
import importlib.util
import os
import pathlib
import shutil
import socket
import struct
import subprocess
import sys
import tempfile
import time

here = pathlib.Path(__file__).resolve().parent
spec = importlib.util.spec_from_file_location("melonds_run", here / "melonds-run.py")
mr = importlib.util.module_from_spec(spec)
spec.loader.exec_module(mr)

RATE = 48000
FRAME_HZ = 59.8261  # cuadros por segundo de la DS, para traducir cuadros a segundos


def buscar(rom: bytes):
    """ldrh con desplazamiento #0x130 dentro del binario ARM9 de la ROM."""
    off, _entry, ram, size = struct.unpack_from("<IIII", rom, 0x20)
    out = []
    for i in range(off, off + size - 3, 4):
        w = struct.unpack_from("<I", rom, i)[0]
        if (w & 0xFFF00FFF) == 0xE1D001B0:
            out.append((ram + i - off, w))
    return out


def write_config(interp: int):
    before = mr.CONFIG.read_text() if mr.CONFIG.exists() else None
    made_dir = not mr.CONFIG.parent.exists()
    mr.CONFIG.parent.mkdir(parents=True, exist_ok=True)
    mr.CONFIG.write_text(
        "[Instance0]\nEnableCheats = false\nLimitFPS = true\nAudioSync = true\n\n"
        f"[Instance0.Audio]\nInterpolation = {interp}\nBitDepth = 2\nVolume = 256\n\n"
        "[Instance0.Emu]\nDirectBoot = true\nExternalBIOSEnable = false\n\n"
        f"[Instance0.Gdb]\nEnabled = true\n\n[Instance0.Gdb.ARM9]\nBreakOnStartup = false\nPort = {mr.PORT}\n\n"
        f"[Instance0.Gdb.ARM7]\nBreakOnStartup = false\nPort = {mr.PORT + 1}\n"
    )
    return before, made_dir


def halt_write(rsp, addr: int, word: int) -> None:
    rsp.sock.sendall(b"\x03")
    rsp.packets(0.15)
    rsp.send(f"M{addr:x},4:{struct.pack('<I', word).hex()}")
    rsp.packets(0.3, want=1)
    rsp.send("c")
    rsp.packets(0.05)


def write_wav(path: pathlib.Path, raw: bytes) -> None:
    hdr = b"RIFF" + struct.pack("<I", 36 + len(raw)) + b"WAVEfmt " + struct.pack("<IHHIIHH", 16, 1, 2, RATE, RATE * 4, 4, 16)
    path.write_bytes(hdr + b"data" + struct.pack("<I", len(raw)) + raw)


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("rom")
    ap.add_argument("wav", nargs="?")
    ap.add_argument("--buscar", action="store_true", help="solo listar las lecturas de KEYINPUT del binario ARM9")
    ap.add_argument("--dur", type=float, default=60, help="segundos de audio a grabar")
    ap.add_argument("--interp", type=int, default=0, help="interpolación de audio de melonDS (0 = ninguna)")
    ap.add_argument("--keyinput-pc", type=lambda s: int(s, 0), action="append", default=[],
                    help="dirección de una ldrh que lee KEYINPUT (se puede repetir si el juego la lee en varios sitios)")
    ap.add_argument("--press", action="append", default=[], metavar="SEG:TECLAS_HEX",
                    help="desde ese segundo de grabación, ese estado de botones (0 = soltar); se puede repetir")
    ap.add_argument("--press-frames", action="append", default=[], metavar="CUADRO:TECLAS_HEX",
                    help="igual, en cuadros de la DS (como --press de salsads-cpurun)")
    ap.add_argument("--offset", type=float, default=0.0,
                    help="segundos que se suman a los tiempos de las pulsaciones (arranque de melonDS)")
    args = ap.parse_args()

    src = pathlib.Path(args.rom).resolve()
    rom = src.read_bytes()
    if args.buscar:
        for addr, w in buscar(rom):
            print(f"0x{addr:08x}: {w:08x}  ldrh r{(w >> 12) & 15}, [r{(w >> 16) & 15}, #0x130]")
        return 0
    if not args.wav:
        sys.exit("falta el WAV de salida")
    presses = []
    for p in args.press:
        t, k = p.split(":")
        presses.append((float(t) + args.offset, int(k, 16)))
    for p in args.press_frames:
        f, k = p.split(":")
        presses.append((float(f) / FRAME_HZ + args.offset, int(k, 16)))
    presses.sort()
    if presses and not args.keyinput_pc:
        sys.exit("las pulsaciones necesitan --keyinput-pc (ver --buscar)")
    for _, k in presses:
        if k & ~0xFF:
            sys.exit("solo botones de los bits 0-7")

    tmpdir = tempfile.mkdtemp(prefix="melonds-grabar-")
    rom_copy = pathlib.Path(tmpdir) / src.name
    shutil.copyfile(src, rom_copy)
    sav = src.with_suffix(".sav")
    if sav.exists():
        shutil.copyfile(sav, rom_copy.with_suffix(".sav"))
    raw_path = pathlib.Path(tmpdir) / "audio.raw"
    saved = write_config(args.interp)
    env = dict(os.environ, SDL_AUDIODRIVER="disk", SDL_DISKAUDIOFILE=str(raw_path))
    proc = subprocess.Popen([mr.find_melonds(), str(rom_copy)], env=env,
                            stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    rsp = None
    orig = {}  # dirección -> (instrucción original, rd)
    rc = 0
    try:
        t0 = time.monotonic()
        if presses:
            while rsp is None:
                if proc.poll() is not None or time.monotonic() - t0 > 20:
                    sys.exit("melonDS no abrió el stub de GDB")
                try:
                    rsp = mr.Rsp(socket.create_connection(("127.0.0.1", mr.PORT), timeout=1))
                except OSError:
                    time.sleep(0.3)
            # el binario ARM9 puede venir comprimido: esperar a que el juego lo haya expandido
            for pc in args.keyinput_pc:
                for _ in range(60):
                    word = struct.unpack("<I", rsp.snapshot(pc, 4, 4))[0]
                    if (word & 0x0E5000F0) == 0x005000B0:  # ldrh rd, [rn, #imm] (ARM)
                        break
                    time.sleep(0.25)
                else:
                    sys.exit(f"en 0x{pc:08x} hay {word:08x}, no una ldrh inmediata")
                orig[pc] = (word, (word >> 12) & 15)
        done = 0
        patched = False
        while True:
            if proc.poll() is not None:
                print("melonDS terminó antes de tiempo", file=sys.stderr)
                rc = 2
                break
            secs = raw_path.stat().st_size / (RATE * 4) if raw_path.exists() else 0.0
            if secs >= args.dur:
                break
            while done < len(presses) and presses[done][0] <= secs:
                keys = presses[done][1]
                for pc, (word, rd) in orig.items():
                    if keys:
                        halt_write(rsp, pc, 0xE3E00000 | (rd << 12) | keys)  # mvn rd, #keys
                    elif patched:
                        halt_write(rsp, pc, word)
                patched = bool(keys)
                print(f"{secs:7.2f} s: botones {keys:#x}", file=sys.stderr)
                done += 1
            time.sleep(0.05)
        if rsp and patched:
            for pc, (word, _) in orig.items():
                halt_write(rsp, pc, word)
    finally:
        if rsp:
            rsp.sock.close()
        if proc.poll() is None:
            proc.kill()  # no hay salida limpia sin teclado; la configuración se restaura abajo
            proc.wait()
        time.sleep(0.5)
        raw = raw_path.read_bytes() if raw_path.exists() else b""
        n = len(raw) // 4
        if n:
            write_wav(pathlib.Path(args.wav), raw[:n * 4])
            print(f"{args.wav}: {n} pares a {RATE} Hz ({n / RATE:.2f} s), interpolación {args.interp}", file=sys.stderr)
        else:
            rc = 2
        mr.restore_config(*saved)
        shutil.rmtree(tmpdir, ignore_errors=True)
    return rc


if __name__ == "__main__":
    sys.exit(main())
