#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Receptor del logger UDP de SalsaDS (src/platform/ps3/log.c).

Escucha en 0.0.0.0:18195 (broadcast o unicast desde la PS3), imprime cada
línea con la IP de origen y, con --out, la agrega a un archivo.

  tools/udplog.py [--port 18195] [--out build/logs/ps3.log] [--until TEXTO]

--until termina (código 0) al recibir una línea que contenga TEXTO; útil
para automatizar pruebas: p. ej. --until "RESULTADO".

Al salir resume, por origen y programa, las líneas recibidas, duplicadas y
perdidas según el número de secuencia "#n" que agrega el logger.
"""
import argparse
import datetime
import re
import signal
import socket
import sys


SEQ = re.compile(r"^\[(\S+) +[\d.]+ #(\d+)\]")


def summarize(seen: dict) -> None:
    for (host, tag), seqs in sorted(seen.items()):
        unique = set(seqs)
        missing = (max(unique) - min(unique) + 1) - len(unique)
        print(f"resumen {host} {tag}: {len(seqs)} recibidas, "
              f"{len(seqs) - len(unique)} duplicadas, {missing} perdidas "
              f"(secuencia {min(unique)}..{max(unique)})", file=sys.stderr)


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--port", type=int, default=18195)
    ap.add_argument("--out", help="archivo donde agregar las líneas")
    ap.add_argument("--until", help="salir al recibir una línea con este texto")
    ap.add_argument("--timeout", type=float, help="segundos sin datos antes de salir con error")
    args = ap.parse_args()

    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    if hasattr(socket, "SO_REUSEPORT"):
        sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEPORT, 1)
    sock.bind(("0.0.0.0", args.port))
    if args.timeout:
        sock.settimeout(args.timeout)

    # SIGTERM también resume al salir (en sh no interactivo, un proceso en
    # segundo plano ignora SIGINT).
    signal.signal(signal.SIGTERM, lambda *_: sys.exit(0))
    out = open(args.out, "a", encoding="utf-8") if args.out else None
    seen: dict = {}
    print(f"escuchando UDP en 0.0.0.0:{args.port}", file=sys.stderr)
    try:
        while True:
            try:
                data, (host, _) = sock.recvfrom(65535)
            except socket.timeout:
                print("tiempo de espera agotado", file=sys.stderr)
                return 1
            stamp = datetime.datetime.now().strftime("%H:%M:%S.%f")[:-3]
            for line in data.decode("utf-8", "replace").splitlines():
                m = SEQ.match(line)
                if m:
                    seen.setdefault((host, m.group(1)), []).append(int(m.group(2)))
                text = f"{stamp} {host} {line}"
                print(text, flush=True)
                if out:
                    out.write(text + "\n")
                    out.flush()
                if args.until and args.until in line:
                    return 0
    except KeyboardInterrupt:
        return 0
    finally:
        summarize(seen)
        if out:
            out.close()


if __name__ == "__main__":
    sys.exit(main())
