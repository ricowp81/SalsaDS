/* SPDX-License-Identifier: MIT */
/*
 * spukit — compresión de bloques sin pérdida, rápida, en formato LZ4.
 *
 * Solo cabecera, C99 puro: el mismo código corre en el PPE, en un SPE (el
 * compresor cabe en ~100 KB de local store) y en el host, y da los mismos
 * bytes en los tres (la tabla de dispersión se calcula con el orden de bytes
 * de la red, no con el de la máquina). Los bloques comprimidos son del
 * formato de bloque de LZ4 (la especificación pública de Yann Collet,
 * github.com/lz4/lz4, doc/lz4_Block_format.md): cualquier descompresor de
 * LZ4 los lee. El código es propio; no contiene fuentes de LZ4.
 *
 * Un bloque tiene a lo más SPUKIT_LZ_BLOCK (64 KB) bytes de entrada. Los
 * archivos grandes se parten en bloques y cada bloque se guarda con
 * spukit_lz_frame: u32 big-endian con el largo del contenido y, en el bit
 * 31, si el bloque va SIN comprimir (cuando comprimirlo no ayuda), seguido
 * de los bytes. Los bloques son independientes (nada de diccionario entre
 * ellos): se pueden comprimir en cualquier orden y en varios SPEs.
 *
 * Costo: la compresión es codiciosa (una tabla de 8 KB, sin cadenas) y se
 * salta más rápido las zonas que no comprimen; la descompresión copia a
 * palabras cuando puede. Ninguna función asigna memoria ni toca nada fuera
 * de los búferes que recibe; la descompresión valida todos los límites (un
 * archivo corrupto da un error, no un desborde).
 */
#ifndef SPUKIT_LZ_H
#define SPUKIT_LZ_H

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#define SPUKIT_LZ_BLOCK 65536u
/* Entradas de la tabla de dispersión del compresor (uint16_t cada una). */
#define SPUKIT_LZ_TABLE_BITS 13
#define SPUKIT_LZ_TABLE_ENTRIES (1u << SPUKIT_LZ_TABLE_BITS)
/* Lo más que puede ocupar un bloque comprimido de `n` bytes. */
#define SPUKIT_LZ_BOUND(n) ((n) + (n) / 255u + 16u)
/* Bit 31 del encabezado de un bloque: va sin comprimir. */
#define SPUKIT_LZ_STORED 0x80000000u

#define SPUKIT_LZ_MIN_MATCH 4u
#define SPUKIT_LZ_LAST_LITERALS 5u   /* los 5 últimos bytes del bloque van como literales */
#define SPUKIT_LZ_MF_LIMIT 12u       /* la última coincidencia empieza 12 bytes antes del final */

static inline uint32_t spukit_lz_rd32(const uint8_t *p)
{
	return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3];
}

/* Bytes iguales al principio de dos zonas, hasta `limit` (a lo más). */
static inline size_t spukit_lz_count(const uint8_t *a, const uint8_t *b, const uint8_t *limit)
{
	const uint8_t *s = a;
	while (a + 4 <= limit) {
		uint32_t x = spukit_lz_rd32(a) ^ spukit_lz_rd32(b);
		if (x)
			return (size_t)(a - s) + ((size_t)__builtin_clz(x) >> 3);
		a += 4;
		b += 4;
	}
	while (a < limit && *a == *b) {
		a++;
		b++;
	}
	return (size_t)(a - s);
}

/* Escribe un largo con el esquema de LZ4 (el 15 del token ya está puesto):
 * bytes de 255 y el resto. */
static inline uint8_t *spukit_lz_put_len(uint8_t *op, size_t v)
{
	while (v >= 255) {
		*op++ = 255;
		v -= 255;
	}
	*op++ = (uint8_t)v;
	return op;
}

/*
 * Comprime `n` bytes (≤ SPUKIT_LZ_BLOCK) de `src` a `dst`, que debe tener al
 * menos SPUKIT_LZ_BOUND(n) bytes de capacidad (`cap`). `table` son
 * SPUKIT_LZ_TABLE_ENTRIES uint16_t de trabajo (se ponen en cero aquí).
 * Devuelve los bytes escritos, o 0 si no cupo (cap menor que el límite) o n
 * es demasiado grande. No garantiza que sea menor que n: quien llama
 * compara y, si no ganó, guarda el bloque sin comprimir.
 */
static inline size_t spukit_lz_compress(const uint8_t *src, size_t n, uint8_t *dst, size_t cap, uint16_t *table)
{
	if (n > SPUKIT_LZ_BLOCK || cap < SPUKIT_LZ_BOUND(n))
		return 0;
	uint8_t *op = dst;
	size_t ip = 0, anchor = 0;
	if (n >= SPUKIT_LZ_MF_LIMIT + 1) {
		const size_t mflimit = n - SPUKIT_LZ_MF_LIMIT;       /* la coincidencia empieza antes */
		const size_t matchlimit = n - SPUKIT_LZ_LAST_LITERALS; /* y termina antes de aquí */
		unsigned miss = 0;
		memset(table, 0, SPUKIT_LZ_TABLE_ENTRIES * sizeof *table);
		while (ip < mflimit) {
			uint32_t w = spukit_lz_rd32(src + ip);
			uint32_t h = (w * 2654435761u) >> (32 - SPUKIT_LZ_TABLE_BITS);
			size_t ref = table[h];
			table[h] = (uint16_t)ip;
			if (ref >= ip || spukit_lz_rd32(src + ref) != w) {
				/* Sin coincidencia: cada 64 fallos seguidos, un paso más largo. */
				ip += 1u + (miss++ >> 6);
				continue;
			}
			miss = 0;
			/* Hacia atrás, mientras los bytes anteriores también coincidan. */
			while (ip > anchor && ref > 0 && src[ip - 1] == src[ref - 1]) {
				ip--;
				ref--;
			}
			size_t mlen = SPUKIT_LZ_MIN_MATCH +
				      spukit_lz_count(src + ip + SPUKIT_LZ_MIN_MATCH, src + ref + SPUKIT_LZ_MIN_MATCH,
						      src + matchlimit);
			size_t lit = ip - anchor, off = ip - ref;
			uint8_t *token = op++;
			*token = (uint8_t)((lit >= 15 ? 15 : lit) << 4);
			if (lit >= 15)
				op = spukit_lz_put_len(op, lit - 15);
			memcpy(op, src + anchor, lit);
			op += lit;
			*op++ = (uint8_t)off;
			*op++ = (uint8_t)(off >> 8);
			size_t ml = mlen - SPUKIT_LZ_MIN_MATCH;
			if (ml >= 15) {
				*token |= 15;
				op = spukit_lz_put_len(op, ml - 15);
			} else {
				*token |= (uint8_t)ml;
			}
			ip += mlen;
			anchor = ip;
			/* Un par de posiciones dentro de lo recién cubierto, para la siguiente. */
			if (ip >= 2 && ip - 2 < mflimit)
				table[(spukit_lz_rd32(src + ip - 2) * 2654435761u) >> (32 - SPUKIT_LZ_TABLE_BITS)] =
					(uint16_t)(ip - 2);
		}
	}
	/* Los últimos literales. */
	size_t lit = n - anchor;
	uint8_t *token = op++;
	*token = (uint8_t)((lit >= 15 ? 15 : lit) << 4);
	if (lit >= 15)
		op = spukit_lz_put_len(op, lit - 15);
	memcpy(op, src + anchor, lit);
	op += lit;
	return (size_t)(op - dst);
}

/*
 * Descomprime un bloque de `clen` bytes a exactamente `n` bytes en `dst`.
 * Devuelve true si el bloque es válido y da justo `n` bytes; nunca lee
 * fuera de [src, src + clen) ni escribe fuera de [dst, dst + n).
 */
static inline int spukit_lz_decompress(const uint8_t *src, size_t clen, uint8_t *dst, size_t n)
{
	const uint8_t *ip = src, *iend = src + clen;
	uint8_t *op = dst, *oend = dst + n;
	if (clen == 0)
		return n == 0;
	for (;;) {
		if (ip >= iend)
			return 0;
		unsigned token = *ip++;
		size_t lit = token >> 4;
		if (lit == 15) {
			unsigned b;
			do {
				if (ip >= iend)
					return 0;
				b = *ip++;
				lit += b;
			} while (b == 255);
		}
		if (lit > (size_t)(iend - ip) || lit > (size_t)(oend - op))
			return 0;
		memcpy(op, ip, lit);
		op += lit;
		ip += lit;
		if (ip == iend)  /* el último tramo: solo literales */
			return op == oend;
		if (iend - ip < 2)
			return 0;
		size_t off = (size_t)ip[0] | (size_t)ip[1] << 8;
		ip += 2;
		if (off == 0 || off > (size_t)(op - dst))
			return 0;
		size_t ml = token & 15;
		if (ml == 15) {
			unsigned b;
			do {
				if (ip >= iend)
					return 0;
				b = *ip++;
				ml += b;
			} while (b == 255);
		}
		ml += SPUKIT_LZ_MIN_MATCH;
		if (ml > (size_t)(oend - op))
			return 0;
		const uint8_t *m = op - off;
		if (off >= 8) {
			/* Palabra a palabra: no se solapan dentro de cada copia de 8. */
			while (ml >= 8) {
				memcpy(op, m, 8);
				op += 8;
				m += 8;
				ml -= 8;
			}
		}
		while (ml--)
			*op++ = *m++;
	}
}

/* ---- Bloques con encabezado (archivos) ----------------------------------------- */

/* Encabezado de un bloque de `len` bytes de contenido (sin comprimir o ya
 * comprimidos), en big-endian. */
static inline void spukit_lz_frame(uint8_t hdr[4], uint32_t len, int stored)
{
	uint32_t v = len | (stored ? SPUKIT_LZ_STORED : 0u);
	hdr[0] = (uint8_t)(v >> 24);
	hdr[1] = (uint8_t)(v >> 16);
	hdr[2] = (uint8_t)(v >> 8);
	hdr[3] = (uint8_t)v;
}

static inline uint32_t spukit_lz_frame_len(const uint8_t hdr[4])
{
	return spukit_lz_rd32(hdr) & ~SPUKIT_LZ_STORED;
}

static inline int spukit_lz_frame_stored(const uint8_t hdr[4])
{
	return (spukit_lz_rd32(hdr) & SPUKIT_LZ_STORED) != 0;
}

#endif
