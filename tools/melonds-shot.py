#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Imagen del motor A de melonDS en un juego (sin que la ROM la pida): para comparar escenas de juegos con SalsaDS.

  tools/melonds-shot.py ROM.nds salida.ppm --wait 25

Lanza melonDS con una copia de la ROM, espera `--wait` segundos, detiene el ARM9 por el stub de GDB, pone el banco A de
VRAM en modo LCDC (VRAMCNT_A = 0x80) y dispara una captura de la pantalla gráfica del motor A (DISPCAPCNT = 0x80300000:
fondos, OBJ y 3D), deja correr unos cuadros, vuelve a detener y lee el banco A (BGR555). Toca el estado del juego (el
banco A deja de servirle para lo que lo usara): la imagen es la del cuadro capturado, no se sigue jugando. El tiempo es en
segundos, no en cuadros: sirve para escenas quietas o casi (pantallas de título, demostraciones), a ojo. Solo ejecuta
melonDS; misma configuración temporal que tools/melonds-run.py.
"""
import argparse
import importlib.util
import pathlib
import shutil
import socket
import subprocess
import sys
import tempfile
import time

here = pathlib.Path(__file__).resolve().parent
spec = importlib.util.spec_from_file_location("melonds_run", here / "melonds-run.py")
mr = importlib.util.module_from_spec(spec)
spec.loader.exec_module(mr)


def write_mem(rsp, addr, data: bytes):
    rsp.send(f"M{addr:x},{len(data):x}:{data.hex()}")
    rsp.packets(0.5, want=1)


def read_mem(rsp, addr, length):
    data = b""
    for off in range(0, length, 0x100):
        n = min(0x100, length - off)
        rsp.send(f"m{addr + off:x},{n:x}")
        rep = [r for r in rsp.packets(1.0, want=1) if len(r) == 2 * n]
        if not rep:
            return None
        data += bytes.fromhex(rep[0])
    return data


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("rom")
    ap.add_argument("out")
    ap.add_argument("--wait", type=float, default=20.0, help="segundos antes de capturar")
    args = ap.parse_args()

    tmpdir = tempfile.mkdtemp(prefix="melonds-shot-")
    rom = pathlib.Path(tmpdir) / pathlib.Path(args.rom).name
    shutil.copyfile(args.rom, rom)
    saved = mr.write_config()
    proc = subprocess.Popen([mr.find_melonds(), str(rom)], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    rsp = None
    rc = 0
    try:
        time.sleep(args.wait)
        for _ in range(20):
            try:
                rsp = mr.Rsp(socket.create_connection(("127.0.0.1", mr.PORT), timeout=2))
                break
            except OSError:
                time.sleep(0.5)
        if rsp is None:
            print("no pude conectar con el stub de GDB", file=sys.stderr)
            return 2
        rsp.sock.sendall(b"\x03")
        rsp.packets(0.5)
        write_mem(rsp, 0x04000240, bytes([0x80]))
        write_mem(rsp, 0x04000064, (0x80300000).to_bytes(4, "little"))
        rsp.send("c")
        rsp.packets(0.1)
        time.sleep(0.3)
        rsp.sock.sendall(b"\x03")
        rsp.packets(0.5)
        data = read_mem(rsp, 0x06800000, 256 * 192 * 2)
        if data is None:
            print("el stub no devolvió el banco A", file=sys.stderr)
            return 2
        px = bytearray()
        for i in range(256 * 192):
            c = data[2 * i] | (data[2 * i + 1] << 8)
            px += bytes([(c & 31) << 3, ((c >> 5) & 31) << 3, ((c >> 10) & 31) << 3])
        pathlib.Path(args.out).write_bytes(b"P6\n256 192\n255\n" + bytes(px))
        print(f"imagen del motor A en {args.out}")
    finally:
        if rsp:
            rsp.sock.close()
        if proc.poll() is None:
            proc.terminate()
            try:
                proc.wait(3)
            except subprocess.TimeoutExpired:
                proc.kill()
                proc.wait()
        time.sleep(0.5)
        mr.restore_config(*saved)
        shutil.rmtree(tmpdir, ignore_errors=True)
    return rc


if __name__ == "__main__":
    sys.exit(main())
