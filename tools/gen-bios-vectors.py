#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Genera tests/roms/src/bios/vectors.h: datos de prueba de los descompresores de la BIOS (LZ77, RLE, Huffman,
BitUnPack y los filtros de diferencias) con compresores escritos aquí desde la descripción de GBATEK. Los
resultados esperados salen del dato original, no de ningún emulador.

    tools/gen-bios-vectors.py > tests/roms/src/bios/vectors.h
    tools/gen-bios-vectors.py --pattern > tests/roms/src/periph/pattern.h   (20 KB de bytes conocidos para leer el cartucho)
"""
import heapq
import struct
import sys


def plain_text() -> bytes:
    # texto con repeticiones cortas y largas, ceros y un tramo de bytes iguales
    base = b"SalsaDS prueba de la BIOS. "
    data = base * 6 + bytes(range(40)) + b"\x00" * 33 + b"ab" * 40 + b"Z" * 29 + bytes([(i * 37) & 0xFF for i in range(61)])
    return data[:316]  # múltiplo de 4


def pad4(b: bytes) -> bytes:
    return b + b"\x00" * (-len(b) % 4)


def lz77(data: bytes, min_disp: int = 1) -> bytes:
    out = bytearray(struct.pack("<I", 0x10 | (len(data) << 8)))
    i = 0
    blocks, flags, nflags = bytearray(), 0, 0

    def flush():
        nonlocal blocks, flags, nflags
        if nflags:
            out.append(flags << (8 - nflags) & 0xFF)
            out.extend(blocks)
        blocks, flags, nflags = bytearray(), 0, 0

    while i < len(data):
        best_len, best_disp = 0, 0
        for disp in range(min_disp, min(i, 4096) + 1):
            n = 0
            while n < 18 and i + n < len(data) and data[i + n - disp] == data[i + n]:
                n += 1
            if n > best_len:
                best_len, best_disp = n, disp
        if best_len >= 3:
            flags = (flags << 1) | 1
            blocks.append(((best_len - 3) << 4) | ((best_disp - 1) >> 8))
            blocks.append((best_disp - 1) & 0xFF)
            i += best_len
        else:
            flags <<= 1
            blocks.append(data[i])
            i += 1
        nflags += 1
        if nflags == 8:
            flush()
    flush()
    return pad4(bytes(out))


def rle(data: bytes) -> bytes:
    out = bytearray(struct.pack("<I", 0x30 | (len(data) << 8)))
    i, lit = 0, bytearray()

    def flush_lit():
        nonlocal lit
        while lit:
            chunk = lit[:128]
            out.append(len(chunk) - 1)
            out.extend(chunk)
            lit = lit[128:]

    while i < len(data):
        n = 1
        while i + n < len(data) and data[i + n] == data[i] and n < 130:
            n += 1
        if n >= 3:
            flush_lit()
            out.append(0x80 | (n - 3))
            out.append(data[i])
            i += n
        else:
            lit.append(data[i])
            i += 1
    flush_lit()
    return pad4(bytes(out))


def huffman(data: bytes, bits: int) -> bytes:
    syms = []
    for b in data:
        if bits == 8:
            syms.append(b)
        else:
            syms += [b & 0xF, b >> 4]
    freq = {}
    for s in syms:
        freq[s] = freq.get(s, 0) + 1
    if len(freq) == 1:  # el formato necesita al menos dos hojas
        freq[(next(iter(freq)) + 1) % (1 << bits)] = 1
    heap = [(f, i, ("leaf", s)) for i, (s, f) in enumerate(sorted(freq.items()))]
    heapq.heapify(heap)
    cnt = len(heap)
    while len(heap) > 1:
        f0, _, n0 = heapq.heappop(heap)
        f1, _, n1 = heapq.heappop(heap)
        heapq.heappush(heap, (f0 + f1, cnt, ("node", n0, n1)))
        cnt += 1
    root = heap[0][2]
    # tabla: [tamaño][raíz][par 0][par 1]...; los hijos de un nodo son un par contiguo en posición par
    table = bytearray(2)  # índice 0: tamaño (se rellena al final), índice 1: raíz
    queue = [(root, 1)]   # (nodo, índice en la tabla)
    codes = {}
    paths = {id(root): ()}
    # BFS asignando pares
    order = []
    while queue:
        node, idx = queue.pop(0)
        if node[0] == "leaf":
            continue
        pair = len(table)
        table.extend(b"\x00\x00")
        c0, c1 = node[1], node[2]
        order.append((node, idx, pair, c0, c1))
        for k, c in enumerate((c0, c1)):
            paths[id(c)] = paths[id(node)] + (k,)
            if c[0] == "leaf":
                table[pair + k] = c[1]
                codes[c[1]] = paths[id(c)]
            else:
                queue.append((c, pair + k))
    for node, idx, pair, c0, c1 in order:
        off = (pair - (idx & ~1) - 2) // 2
        assert 0 <= off < 64
        table[idx] = off | (0x80 if c0[0] == "leaf" else 0) | (0x40 if c1[0] == "leaf" else 0)
    assert len(table) % 2 == 0
    table[0] = len(table) // 2 - 1
    bitstr = "".join("".join(str(b) for b in codes[s]) for s in syms)
    bitstr += "0" * (-len(bitstr) % 32)
    stream = b"".join(struct.pack("<I", int(bitstr[i:i + 32], 2)) for i in range(0, len(bitstr), 32))
    hdr = struct.pack("<I", 0x20 | bits | (len(data) << 8))
    return pad4(hdr + bytes(table)) + stream


def diff8(data: bytes) -> bytes:
    out = bytearray(struct.pack("<I", 0x81 | (len(data) << 8)))
    prev = 0
    for b in data:
        out.append((b - prev) & 0xFF)
        prev = b
    return pad4(bytes(out))


def diff16(data: bytes) -> bytes:
    out = bytearray(struct.pack("<I", 0x82 | (len(data) << 8)))
    prev = 0
    for i in range(0, len(data), 2):
        v = struct.unpack_from("<H", data, i)[0]
        out += struct.pack("<H", (v - prev) & 0xFFFF)
        prev = v
    return pad4(bytes(out))


def bitunpack(src: bytes, sw: int, dw: int, off: int, zero: int) -> bytes:
    out, acc, nb = bytearray(), 0, 0
    for b in src:
        for sh in range(0, 8, sw):
            v = (b >> sh) & ((1 << sw) - 1)
            if v or zero:
                v += off
            acc |= (v & ((1 << dw) - 1)) << nb
            nb += dw
            if nb >= 32:
                out += struct.pack("<I", acc & 0xFFFFFFFF)
                acc, nb = 0, 0
    return bytes(out)


def arr(name: str, b: bytes) -> str:
    lines = []
    for i in range(0, len(b), 16):
        lines.append("\t" + ", ".join("0x%02X" % x for x in b[i:i + 16]) + ",")
    return "static const uint8_t %s[%d] __attribute__((aligned(4))) = {\n%s\n};\n" % (name, len(b), "\n".join(lines))


def pattern(n: int) -> bytes:
    x, out = 0x1234567, bytearray()
    for _ in range(n):
        x = (x * 1103515245 + 12345) & 0xFFFFFFFF
        out.append((x >> 16) & 0xFF)
    return bytes(out)


def main() -> None:
    if len(sys.argv) > 1 and sys.argv[1] == "--pattern":
        sys.stdout.write("/* Generado por tools/gen-bios-vectors.py --pattern: no editar a mano. */\n#ifndef PATTERN_H\n#define PATTERN_H\n\n#include <stdint.h>\n\n")
        sys.stdout.write(arr("v_pattern", pattern(0x5000)))
        sys.stdout.write("\n#endif\n")
        return
    d = plain_text()
    bu_src = bytes([0x1B, 0xE4, 0x00, 0xFF, 0x39, 0xC6, 0x55, 0xAA])
    sys.stdout.write("/* Generado por tools/gen-bios-vectors.py: no editar a mano. */\n#ifndef VECTORS_H\n#define VECTORS_H\n\n#include <stdint.h>\n\n")
    sys.stdout.write(arr("v_plain", d))
    # distancia mínima 2: la variante de 16 bits (callbacks) no puede copiar de la posición anterior (GBATEK)
    sys.stdout.write(arr("v_lz77", lz77(d, 2)))
    sys.stdout.write(arr("v_rle", rle(d)))
    sys.stdout.write(arr("v_huff8", huffman(d, 8)))
    sys.stdout.write(arr("v_huff4", huffman(d, 4)))
    sys.stdout.write(arr("v_diff8", diff8(d)))
    sys.stdout.write(arr("v_diff16", diff16(d)))
    sys.stdout.write(arr("v_bu_src", bu_src))
    sys.stdout.write(arr("v_bu_2to8", bitunpack(bu_src, 2, 8, 0x10, 0)))
    sys.stdout.write(arr("v_bu_2to8z", bitunpack(bu_src, 2, 8, 0x10, 1)))
    sys.stdout.write(arr("v_bu_4to16", bitunpack(bu_src, 4, 16, 0x100, 0)))
    sys.stdout.write(arr("v_bu_1to4", bitunpack(bu_src, 1, 4, 1, 0)))
    sys.stdout.write("\n#endif\n")


if __name__ == "__main__":
    main()
