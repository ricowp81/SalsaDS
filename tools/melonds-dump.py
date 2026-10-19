#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Lanza melonDS con una ROM, espera unos segundos y vuelca trozos de la memoria (y los registros) que ve el
stub de GDB: sirve para comparar el estado de un juego con el de SalsaDS (`salsads-cpurun --dump`).

  tools/melonds-dump.py ROM.nds --wait 6 --range 0x027FF000:0x1000:top.bin [--range ...] [--regs] [--cpu 9|7]

Cada --range es DIRECCIÓN:LONGITUD[:ARCHIVO]; sin archivo se imprime en hexadecimal. --regs imprime r0-r15 y
CPSR de la CPU elegida (9 por omisión). Usa la misma configuración temporal que tools/melonds-run.py (respalda y
restaura ~/Library/Preferences/melonDS/melonDS.toml). Una corrida a la vez.
"""
import argparse
import importlib.util
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


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("rom")
    ap.add_argument("--wait", type=float, default=6.0, help="segundos antes de detener la CPU")
    ap.add_argument("--range", action="append", default=[], metavar="DIR:LONG[:ARCHIVO]")
    ap.add_argument("--regs", action="store_true")
    ap.add_argument("--cpu", type=int, choices=(7, 9), default=9)
    ap.add_argument("--keep", action="store_true")
    ap.add_argument("--save-out", metavar="FILE", help="copiar aquí el .sav que cree melonDS (su tamaño dice el chip)")
    args = ap.parse_args()

    # melonDS escribe el .sav junto a la ROM: se corre sobre una copia en una carpeta temporal para no ensuciar la
    # carpeta de origen (ni pisar una partida)
    tmpdir = tempfile.mkdtemp(prefix="melonds-dump-")
    rom = pathlib.Path(tmpdir) / pathlib.Path(args.rom).name
    shutil.copyfile(args.rom, rom)
    saved = mr.write_config()
    proc = subprocess.Popen([mr.find_melonds(), str(rom)], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    rsp = None
    rc = 0
    try:
        time.sleep(args.wait)
        port = mr.PORT if args.cpu == 9 else mr.PORT + 1
        for _ in range(20):
            try:
                rsp = mr.Rsp(socket.create_connection(("127.0.0.1", port), timeout=2))
                break
            except OSError:
                time.sleep(0.5)
        if rsp is None:
            print("no pude conectar con el stub de GDB", file=sys.stderr)
            return 2
        rsp.sock.sendall(b"\x03")
        rsp.packets(0.5)
        if args.regs:
            rsp.send("g")
            rep = [r for r in rsp.packets(1.0, want=1) if len(r) >= 16 * 8]
            if rep:
                raw = bytes.fromhex(rep[0])
                regs = struct.unpack("<%dI" % (len(raw) // 4), raw)
                print("regs " + " ".join("r%d=%08x" % (i, regs[i]) for i in range(16)))
                if len(regs) > 25:
                    print("cpsr=%08x" % regs[25])
        for spec_ in args.range:
            parts = spec_.split(":")
            addr, length = int(parts[0], 0), int(parts[1], 0)
            data = b""
            for off in range(0, length, 0x100):
                n = min(0x100, length - off)
                rsp.send(f"m{addr + off:x},{n:x}")
                rep = [r for r in rsp.packets(1.0, want=1) if len(r) == 2 * n]
                if not rep:
                    print(f"el stub no contestó en 0x{addr + off:x}", file=sys.stderr)
                    rc = 2
                    break
                data += bytes.fromhex(rep[0])
            if len(parts) > 2:
                pathlib.Path(parts[2]).write_bytes(data)
                print(f"0x{addr:08x} +0x{length:x} -> {parts[2]}")
            else:
                for i in range(0, len(data), 16):
                    print("%08x: %s" % (addr + i, data[i:i + 16].hex(" ")))
        rsp.send("c")
        rsp.packets(0.1)
    finally:
        if rsp:
            rsp.sock.close()
        if not args.keep and proc.poll() is None:
            proc.terminate()
            try:
                proc.wait(3)
            except subprocess.TimeoutExpired:
                proc.kill()
                proc.wait()
        time.sleep(0.5)
        mr.restore_config(*saved)
        sav = rom.with_suffix(".sav")
        if sav.exists():
            print(f"melonDS creó {sav.name}: {sav.stat().st_size} bytes")
            if args.save_out:
                shutil.copyfile(sav, args.save_out)
        shutil.rmtree(tmpdir, ignore_errors=True)
    return rc


if __name__ == "__main__":
    sys.exit(main())
