#!/usr/bin/env python3
"""Corre una ROM de prueba en melonDS (emulador de referencia) y lee su veredicto.

  tools/melonds-run.py build/roms/hola.nds [--timeout 30] [--keep] [--vram FILE.ppm]

La ROM usa el marco de pruebas de tests/roms/src/common/sds.h: deja un bloque
de resultados en RAM principal (SDS_RESULTS_ADDR). Este script lanza melonDS
con el stub de GDB activado (puerto 3333, ARM9) y lee ese bloque con el
protocolo remoto de GDB (comando `m`) hasta que la ROM termina. melonDS lee su
configuración de ~/Library/Preferences/melonDS/melonDS.toml (no respeta HOME):
el script la respalda, escribe una mínima con el stub activado y la restaura o
borra al terminar. Una corrida a la vez.

melonDS se busca en $MELONDS (ruta al ejecutable o al .app) o en
/Applications/melonDS.app. Su BIOS libre integrada (FreeBIOS) y el arranque
directo bastan: no hacen falta archivos de la DS.

Código de salida: 0 = OK, 1 = FALLO, 2 = sin veredicto (tiempo agotado o no
se pudo leer).
"""
import argparse
import os
import pathlib
import re
import socket
import struct
import subprocess
import sys
import time

SDS_ADDR = 0x02380000
SDS_LEN = 0x30
SDS_FAILLOG = 0x40  # registro de fallos tras el bloque: hasta 16 entradas (grupo, id, obtenido, esperado)
SDS_DIVERGE = 0x140  # divergencias respecto de la documentación (informativas): hasta 32 entradas
SDS_TOTAL = 0x340
SDS_MAGIC = 0x52534453
PORT = 3333
SDS_ARM7_ADDR = 0x02390000  # bloque de resultados del ARM7 en las ROMs que corren pruebas en las dos CPUs (cpuarm)


def find_melonds() -> str:
    env = os.environ.get("MELONDS")
    cands = [env] if env else []
    cands += ["/Applications/melonDS.app"]
    for c in cands:
        if not c:
            continue
        p = pathlib.Path(c)
        if p.suffix == ".app":
            p = p / "Contents/MacOS/melonDS"
        if p.exists():
            return str(p)
    sys.exit("no encuentro melonDS: fijar MELONDS con la ruta del ejecutable o del .app")


class Rsp:
    """Cliente mínimo del protocolo remoto de GDB (paquetes $...#xx con acuse)."""

    def __init__(self, sock: socket.socket):
        self.sock = sock
        self.buf = b""
        sock.sendall(b"+")  # el stub espera un acuse inicial

    def send(self, data: str) -> None:
        raw = data.encode()
        self.sock.sendall(b"$" + raw + b"#" + b"%02x" % (sum(raw) & 0xFF))

    def packets(self, timeout: float, want: int = 0) -> list:
        """Lee paquetes hasta `want` (o hasta que pase `timeout` sin más datos)."""
        out = []
        deadline = time.monotonic() + timeout
        while True:
            while True:
                m = re.search(rb"\$([^#]*)#..", self.buf)
                if not m:
                    break
                out.append(m.group(1).decode())
                self.buf = self.buf[m.end():]
                self.sock.sendall(b"+")
            left = deadline - time.monotonic()
            if (want and len(out) >= want) or left <= 0:
                return out
            self.sock.settimeout(left)
            try:
                chunk = self.sock.recv(65536)
            except socket.timeout:
                return out
            if not chunk:
                raise ConnectionError("el stub cerró la conexión")
            self.buf += chunk

    def dump(self, addr: int, length: int, chunk: int = 256) -> bytes:
        """Detiene la CPU una vez y lee una zona grande de memoria en trozos."""
        self.sock.sendall(b"\x03")
        self.packets(0.4)
        out = b""
        for off in range(0, length, chunk):
            n = min(chunk, length - off)
            self.send(f"m{addr + off:x},{n:x}")
            replies = [r for r in self.packets(1.0, want=1) if len(r) == 2 * n]
            if not replies:
                raise OSError("el stub no contestó la lectura")
            out += bytes.fromhex(replies[0])
        self.send("c")
        self.packets(0.1)
        return out

    def snapshot(self, addr: int, length: int, chunk: int = 0x100) -> bytes:
        """Detiene la CPU (Ctrl-C), lee memoria en trozos y la deja seguir."""
        self.sock.sendall(b"\x03")
        self.packets(0.4)
        out = b""
        try:
            for off in range(0, length, chunk):
                n = min(chunk, length - off)
                self.send(f"m{addr + off:x},{n:x}")
                replies = [r for r in self.packets(1.0, want=1) if len(r) == 2 * n]
                if not replies:
                    raise OSError(f"respuesta inesperada del stub en 0x{addr + off:x}")
                out += bytes.fromhex(replies[0])
        finally:
            self.send("c")
            self.packets(0.1)
        return out


CONFIG = pathlib.Path.home() / "Library/Preferences/melonDS/melonDS.toml"


def write_config() -> tuple:
    """Devuelve (contenido anterior o None, si se creó la carpeta)."""
    before = CONFIG.read_text() if CONFIG.exists() else None
    made_dir = not CONFIG.parent.exists()
    CONFIG.parent.mkdir(parents=True, exist_ok=True)
    # Con el [Instance0] explícito: sin él, melonDS aborta al reescribir su
    # configuración (el serializador de TOML lanza una excepción).
    CONFIG.write_text(
        "[Instance0]\nEnableCheats = false\n\n[Instance0.Gdb]\nEnabled = true\n\n"
        f"[Instance0.Gdb.ARM9]\nBreakOnStartup = false\nPort = {PORT}\n\n"
        f"[Instance0.Gdb.ARM7]\nBreakOnStartup = false\nPort = {PORT + 1}\n"
    )
    return before, made_dir


def restore_config(before, made_dir) -> None:
    if before is not None:
        CONFIG.write_text(before)
    else:
        CONFIG.unlink(missing_ok=True)
        if made_dir:
            try:
                CONFIG.parent.rmdir()
            except OSError:
                pass


def save_ppm(path: str, raw: bytes) -> None:
    """VRAM de 256x192 en RGB555 little-endian -> PPM."""
    px = bytearray()
    for (v,) in struct.iter_unpack("<H", raw):
        px += bytes(((v & 31) * 255 // 31, ((v >> 5) & 31) * 255 // 31, ((v >> 10) & 31) * 255 // 31))
    with open(path, "wb") as f:
        f.write(b"P6\n256 192\n255\n" + bytes(px))


def print_regs(rsp) -> None:
    """Imprime los registros de la imagen con el formato de `salsads-cpurun --regs`."""
    def w32(addr):
        return struct.unpack("<I", rsp.snapshot(addr, 4, 4))[0]

    def w16(addr):
        return struct.unpack("<H", rsp.snapshot(addr, 4, 4)[:2])[0] if addr & 3 == 0 else \
            struct.unpack("<I", rsp.snapshot(addr & ~3, 4, 4))[0] >> 16

    vram = rsp.snapshot(0x04000240, 16, 16)[:10]
    print("regs DISPCNT_A=%08x DISPCNT_B=%08x BRIGHT_A=%04x BRIGHT_B=%04x POWCNT1=%04x" %
          (w32(0x04000000), w32(0x04001000), w16(0x0400006C), w16(0x0400106C), w16(0x04000304)))
    print("regs VRAMCNT=" + "".join("%02x" % b for b in vram))


def print_arm7(arm7) -> None:
    if not arm7:
        return
    w7, log7, div7 = arm7
    print(f"  ARM7: estado {w7[2]}, {w7[5]} grupos, {w7[3]} comprobaciones, {w7[4]} fallos, {w7[11]} divergencias",
          file=sys.stderr)
    for group, ident, got, want in log7:
        print(f"  ARM7 fallo: grupo {group}, id {ident}, obtuvo 0x{got:08x}, esperaba 0x{want:08x}", file=sys.stderr)
    for group, ident, got, want in div7:
        print(f"  ARM7 divergencia: grupo {group}, id {ident}, obtuvo 0x{got:08x}, documentado 0x{want:08x}",
              file=sys.stderr)


def print_log(log, div=()) -> None:
    for group, ident, got, want in log:
        print(f"  fallo: grupo {group}, id {ident}, obtuvo 0x{got:08x}, esperaba 0x{want:08x}", file=sys.stderr)
    for group, ident, got, want in div:
        print(f"  divergencia: grupo {group}, id {ident}, obtuvo 0x{got:08x}, documentado 0x{want:08x}", file=sys.stderr)


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("rom")
    ap.add_argument("--timeout", type=float, default=30)
    ap.add_argument("--keep", action="store_true", help="dejar melonDS abierto al terminar")
    ap.add_argument("--arm7", action="store_true",
                    help="al terminar (o al agotar el tiempo) mostrar también el bloque de resultados del ARM7")
    ap.add_argument("--regs", action="store_true",
                    help="al terminar, imprimir los registros que deciden la imagen (mismo formato que "
                    "`salsads-cpurun --regs`) leídos por el stub de GDB")
    ap.add_argument("--vram", metavar="FILE.ppm",
                    help="guardar como imagen PPM el banco A de VRAM (la pantalla de arriba de las ROMs "
                    "de prueba, en modo de visualización directa), leído por el stub de GDB")
    args = ap.parse_args()

    rom = pathlib.Path(args.rom).resolve()
    if not rom.exists():
        sys.exit(f"no existe {rom}")
    saved = write_config()
    proc = subprocess.Popen([find_melonds(), str(rom)],
                            stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    rsp = None
    result = None
    log = []
    div = []
    arm7 = None
    last = None  # última lectura del bloque de resultados, aunque la ROM no haya terminado
    try:
        deadline = time.monotonic() + args.timeout
        while time.monotonic() < deadline and result is None:
            if proc.poll() is not None:
                print(f"melonDS terminó antes de dar un veredicto (código {proc.returncode})", file=sys.stderr)
                break
            try:
                if rsp is None:
                    rsp = Rsp(socket.create_connection(("127.0.0.1", PORT), timeout=1))
                raw = rsp.snapshot(SDS_ADDR, SDS_TOTAL)
                words = struct.unpack("<13I", raw[:SDS_LEN + 4])
                if words[0] == SDS_MAGIC:
                    last = words
                    log = list(struct.iter_unpack("<4I", raw[SDS_FAILLOG:SDS_DIVERGE]))[:min(words[4], 16)]
                    div = list(struct.iter_unpack("<4I", raw[SDS_DIVERGE:SDS_TOTAL]))[:min(words[11], 32)]
                if args.arm7 and words[0] == SDS_MAGIC:
                    raw7 = rsp.snapshot(SDS_ARM7_ADDR, SDS_TOTAL)
                    w7 = struct.unpack("<12I", raw7[:SDS_LEN])
                    if w7[0] == SDS_MAGIC:
                        arm7 = (w7, list(struct.iter_unpack("<4I", raw7[SDS_FAILLOG:SDS_DIVERGE]))[:min(w7[4], 16)],
                                list(struct.iter_unpack("<4I", raw7[SDS_DIVERGE:SDS_TOTAL]))[:min(w7[11], 32)])
                if words[0] == SDS_MAGIC and words[2] == 1:
                    result = words
                    if args.vram:
                        try:
                            save_ppm(args.vram, rsp.dump(0x06800000, 256 * 192 * 2))
                        except (OSError, ConnectionError) as e:
                            print(f"no pude leer la VRAM: {e}", file=sys.stderr)
                    if args.regs:
                        try:
                            print_regs(rsp)
                        except (OSError, ConnectionError) as e:
                            print(f"no pude leer los registros: {e}", file=sys.stderr)
                    break
            except (OSError, ConnectionError, ValueError):
                if rsp:
                    rsp.sock.close()
                    rsp = None
            time.sleep(0.5)
    finally:
        if rsp and result is None and args.vram and proc.poll() is None:
            try:  # una ROM sin veredicto (una demostración): la imagen del momento
                save_ppm(args.vram, rsp.dump(0x06800000, 256 * 192 * 2))
                if args.regs:
                    print_regs(rsp)
            except (OSError, ConnectionError) as e:
                print(f"no pude leer la VRAM: {e}", file=sys.stderr)
        if rsp:
            rsp.sock.close()
        if not args.keep and proc.poll() is None:
            proc.terminate()
            try:
                proc.wait(3)
            except subprocess.TimeoutExpired:
                proc.kill()  # melonDS ignora SIGTERM a veces
                proc.wait()
        time.sleep(0.5)  # melonDS reescribe su configuración al salir
        restore_config(*saved)

    if result is None:
        print(f"SIN VEREDICTO {rom.name}")
        if last:
            print_log(log, div)
            print_arm7(arm7)
            print(f"  progreso: {last[5]} grupos, {last[3]} comprobaciones, {last[4]} fallos, marca {last[12]}"
                  + (f"; primero en el grupo {last[6]}, id {last[7]}, obtuvo 0x{last[8]:08x}, esperaba 0x{last[9]:08x}"
                     if last[4] else ""), file=sys.stderr)
        return 2
    _, version, _, total, failed, groups, fg, fid, fgot, fwant, verdict, *_ = result
    name = rom.name
    print_log(log, div)
    print_arm7(arm7)
    if verdict == 1 and failed == 0:
        print(f"OK {name}: {total} comprobaciones en {groups} grupos")
        return 0
    print(f"FALLO {name}: {failed} de {total}; primero en el grupo {fg}, id {fid}, "
          f"obtuvo 0x{fgot:08x}, esperaba 0x{fwant:08x}")
    return 1


if __name__ == "__main__":
    sys.exit(main())
