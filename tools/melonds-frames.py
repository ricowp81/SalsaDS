#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Secuencia cuadro a cuadro de melonDS (las dos pantallas y los registros de video) para comparar con SalsaDS.

  tools/melonds-frames.py ROM.nds salida/ --wait 35 --break 0x020802dc --break 0x02080384 --frames 60

Lanza melonDS (programa de referencia, caja negra) con el stub de GDB, espera `--wait` segundos, pone puntos de parada
(Z0) en las direcciones de `--break` (p. ej. las escrituras de POWCNT1 que el juego hace una vez por cuadro en su
VBlank) y en cada parada: lee POWCNT1, DISPCAPCNT, DISPCNT A/B, VRAMCNT_A–I y VCOUNT, espera a que la ventana
termine de dibujar el último cuadro y la fotografía con `screencapture -l` (la ventana entera: las dos pantallas tal
como las ve el jugador). Escribe salida/regs.txt (una línea por parada) y salida/NNN.png. Con --banks también guarda
los bancos C y D tal como los ve el ARM9 en la dirección LCDC (solo si están en LCDC en ese momento).

No lee ni toca el estado del juego más allá de detenerlo; mismo archivo de configuración temporal que
tools/melonds-run.py. Necesita permiso de grabación de pantalla para el terminal. Una corrida a la vez.
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

SWIFT_WINDOWS = r'''
import CoreGraphics
let opts = CGWindowListOption(arrayLiteral: .optionAll)
if let list = CGWindowListCopyWindowInfo(opts, kCGNullWindowID) as? [[String: Any]] {
    for w in list {
        let owner = w[kCGWindowOwnerName as String] as? String ?? ""
        let num = w[kCGWindowNumber as String] as? Int ?? 0
        let layer = w[kCGWindowLayer as String] as? Int ?? 0
        let b = w[kCGWindowBounds as String] as? [String: Any] ?? [:]
        let h = b["Height"] as? Double ?? 0
        let name = w[kCGWindowName as String] as? String ?? ""
        let shown = w[kCGWindowIsOnscreen as String] as? Bool ?? false
        print("\(num)\t\(layer)\t\(h)\t\(shown ? 1 : 0)\t\(name)\t\(owner)")
    }
}
'''


def melonds_window(pid_hint: str = "melonDS", verbose: bool = False) -> int:
    r = subprocess.run(["swift", "-e", SWIFT_WINDOWS], capture_output=True, text=True, timeout=120)
    out = r.stdout
    if verbose:
        print("ventanas:", r.returncode, r.stderr.strip()[:300], file=sys.stderr)
        print("\n".join(l for l in out.splitlines() if pid_hint.lower() in l.lower()), file=sys.stderr)
    best = (0, 0)
    for line in out.splitlines():
        num, layer, h, shown, name, owner = line.split("\t", 5)
        # la ventana principal: la que lleva el nombre del programa en el título (las auxiliares no lo llevan)
        if (pid_hint.lower() in owner.lower() and pid_hint.lower() in name.lower() and int(layer) == 0
                and float(h) > best[1]):
            best = (int(num), float(h))
    return best[0]


def read_mem(rsp, addr, length, chunk=0x100):
    data = b""
    for off in range(0, length, chunk):
        n = min(chunk, length - off)
        rsp.send(f"m{addr + off:x},{n:x}")
        rep = [r for r in rsp.packets(1.0, want=1) if len(r) == 2 * n]
        if not rep:
            raise OSError(f"el stub no contestó en 0x{addr + off:x}")
        data += bytes.fromhex(rep[0])
    return data


def wait_stop(rsp, timeout):
    """Espera el paquete de parada (S/T) tras un `c`."""
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        for p in rsp.packets(0.5, want=1):
            if p[:1] in ("S", "T"):
                return p
    return None


def save_bank_png(path, raw):
    px = bytearray()
    for (v,) in struct.iter_unpack("<H", raw):
        px += bytes(((v & 31) * 255 // 31, ((v >> 5) & 31) * 255 // 31, ((v >> 10) & 31) * 255 // 31))
    path.write_bytes(b"P6\n256 192\n255\n" + bytes(px))


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("rom")
    ap.add_argument("out")
    ap.add_argument("--wait", type=float, default=30.0, help="segundos antes de empezar")
    ap.add_argument("--break", dest="brk", action="append", default=[], metavar="DIR",
                    help="dirección (ARM) donde parar; cada parada es una fila")
    ap.add_argument("--frames", type=int, default=60, help="cuántas paradas registrar")
    ap.add_argument("--settle", type=float, default=0.15, help="segundos de espera antes de fotografiar la ventana")
    ap.add_argument("--banks", action="store_true", help="guardar también los bancos C y D (si están en LCDC)")
    ap.add_argument("--no-shot", action="store_true", help="sin fotografías de la ventana")
    args = ap.parse_args()

    outdir = pathlib.Path(args.out)
    outdir.mkdir(parents=True, exist_ok=True)
    tmpdir = tempfile.mkdtemp(prefix="melonds-frames-")
    rom = pathlib.Path(tmpdir) / pathlib.Path(args.rom).name
    shutil.copyfile(args.rom, rom)
    saved = mr.write_config()
    proc = subprocess.Popen([mr.find_melonds(), str(rom)], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    rsp = None
    rc = 0
    try:
        time.sleep(args.wait)
        win = 0
        for _ in range(15 if not args.no_shot else 0):
            win = melonds_window(verbose=_ == 14)
            if win:
                break
            time.sleep(1)
        if not args.no_shot and not win:
            print("no encontré la ventana de melonDS", file=sys.stderr)
            return 2
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
        for b in args.brk:
            rsp.send(f"Z0,{int(b, 0):x},4")
            rep = rsp.packets(1.0, want=1)
            if not rep or rep[0] != "OK":
                print(f"el stub no aceptó el punto de parada {b}: {rep}", file=sys.stderr)
                return 2
        log = open(outdir / "regs.txt", "w")
        for i in range(args.frames):
            rsp.send("c")
            stop = wait_stop(rsp, 10.0)
            if stop is None:
                print(f"sin parada en la fila {i}", file=sys.stderr)
                rc = 2
                break
            rsp.send("g")
            rep = [r for r in rsp.packets(1.0, want=1) if len(r) >= 16 * 8]
            pc = struct.unpack("<16I", bytes.fromhex(rep[0][:128]))[15] if rep else 0
            io = read_mem(rsp, 0x04000000, 0x70)
            vram = read_mem(rsp, 0x04000240, 0x10)
            pow1 = read_mem(rsp, 0x04000304, 4)
            iob = read_mem(rsp, 0x04001000, 4)
            dispcnt_a, = struct.unpack_from("<I", io, 0)
            vcount, = struct.unpack_from("<H", io, 6)
            cap, = struct.unpack_from("<I", io, 0x64)
            dispcnt_b, = struct.unpack_from("<I", iob, 0)
            powcnt, = struct.unpack_from("<H", pow1, 0)
            line = ("%03d pc=%08x vcount=%3d pow=%04x pow15=%d cap=%08x cap_on=%d banco=%c dispcntA=%08x dispcntB=%08x "
                    "vramcnt=%s" % (i, pc, vcount, powcnt, powcnt >> 15, cap, cap >> 31, "ABCD"[(cap >> 16) & 3],
                                    dispcnt_a, dispcnt_b, vram[:10].hex()))
            if args.banks:
                for bank, cnt in (("C", vram[2]), ("D", vram[3])):
                    if (cnt & 0x87) == 0x80:
                        base = 0x06840000 if bank == "C" else 0x06860000
                        save_bank_png(outdir / f"{i:03d}-{bank}.ppm", read_mem(rsp, base, 256 * 192 * 2))
            if not args.no_shot:
                time.sleep(args.settle)
                subprocess.run(["screencapture", "-x", "-o", f"-l{win}", str(outdir / f"{i:03d}.png")], check=False)
            print(line)
            log.write(line + "\n")
            log.flush()
        log.close()
        for b in args.brk:
            rsp.send(f"z0,{int(b, 0):x},4")
            rsp.packets(0.5, want=1)
        rsp.send("c")
        rsp.packets(0.1)
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
