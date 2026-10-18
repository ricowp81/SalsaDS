#!/usr/bin/env python3
"""Genera tests/roms/src/snd/adpcm.h: un trozo de IMA-ADPCM (cabecera de 32 bits y nibbles) con una onda de prueba, con el
codificador estándar de IMA (tabla de pasos de 89 entradas y ajuste de índice -1,-1,-1,-1,2,4,6,8), para la ROM `snd`.

    tools/gen-snd-data.py > tests/roms/src/snd/adpcm.h
"""
import math

STEP = [7, 8, 9, 10, 11, 12, 13, 14, 16, 17, 19, 21, 23, 25, 28, 31, 34, 37, 41, 45, 50, 55, 60, 66, 73, 80, 88, 97, 107,
        118, 130, 143, 157, 173, 190, 209, 230, 253, 279, 307, 337, 371, 408, 449, 494, 544, 598, 658, 724, 796, 876, 963,
        1060, 1166, 1282, 1411, 1552, 1707, 1878, 2066, 2272, 2499, 2749, 3024, 3327, 3660, 4026, 4428, 4871, 5358, 5894,
        6484, 7132, 7845, 8630, 9493, 10442, 11487, 12635, 13899, 15289, 16818, 18500, 20350, 22385, 24623, 27086, 29794,
        32767]
ADJ = [-1, -1, -1, -1, 2, 4, 6, 8]
N = 248  # muestras (32 palabras de datos después de la cabecera de una: múltiplo de 8)


def wave2(i):
    # otro trozo con valor inicial distinto de cero (la cabecera lo fija) y escalones grandes
    return int(5000 + 9000 * math.sin(i * 0.31) + (3000 if (i // 24) % 2 else -3000))


def wave(i):
    # un barrido con caída de amplitud: usa índices de paso altos y bajos
    return int(12000 * math.sin(2 * math.pi * i * (0.01 + i * 0.0004)) * (1 - i / 400.0))


def decode_nibble(val, idx, n):
    step = STEP[idx]
    diff = step >> 3
    if n & 1:
        diff += step >> 2
    if n & 2:
        diff += step >> 1
    if n & 4:
        diff += step
    val += -diff if n & 8 else diff
    val = max(-32767, min(32767, val))
    idx = max(0, min(88, idx + ADJ[n & 7]))
    return val, idx


def encode(samples, idx0):
    val, idx = samples[0], idx0
    header = (val & 0xFFFF) | (idx << 16)
    nibs = []
    for s_ in samples[1:]:
        step = STEP[idx]
        d = s_ - val
        n = 0
        if d < 0:
            n = 8
            d = -d
        if d >= step:
            n |= 4
            d -= step
        if d >= step >> 1:
            n |= 2
            d -= step >> 1
        if d >= step >> 2:
            n |= 1
        nibs.append(n)
        val, idx = decode_nibble(val, idx, n)
    while len(nibs) % 8:
        nibs.append(0)
    data = [header & 0xFF, (header >> 8) & 0xFF, (header >> 16) & 0xFF, (header >> 24) & 0xFF]
    for i in range(0, len(nibs), 2):
        data.append(nibs[i] | (nibs[i + 1] << 4))
    return data, len(nibs) + 1


def emit(name, data):
    print("static const unsigned char %s[%d] __attribute__((aligned(4))) = {" % (name, len(data)))
    for i in range(0, len(data), 16):
        print("\t" + ", ".join("0x%02X" % b for b in data[i:i + 16]) + ",")
    print("};")


def main():
    d1, n1 = encode([wave(i) for i in range(N)], 20)
    d2, n2 = encode([wave2(i) for i in range(N)], 40)
    print("/* Generado por tools/gen-snd-data.py: IMA-ADPCM de prueba (cabecera + nibbles, bajo primero). */")
    print("#define ADPCM_SAMPLES %d" % n1)
    emit("adpcm_data", d1)
    print("#define ADPCM2_SAMPLES %d" % n2)
    emit("adpcm_data2", d2)


if __name__ == "__main__":
    main()
