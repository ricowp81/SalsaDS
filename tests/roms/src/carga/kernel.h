/*
 * SalsaDS — ROM carga: el trabajo que hacen las dos CPUs. C99 portable (sin dependencias del orden de bytes ni
 * de la ISA): se compila para el ARM9 (ARMv5TE), para el ARM7 (ARMv4T) y, para sacar los valores de referencia,
 * en el host (`tools/carga-gold.sh`).
 *
 * Una iteración mezcla lo que hace un juego: cadena de operaciones de datos con desplazamientos, copia y suma de
 * bloques de palabras (LDM/STM en ARM), recorridos de bytes (LDRB/STRB), multiplicaciones, una ordenación de
 * mitades (rutina en Thumb) y llamadas con salto condicional. El resultado de una iteración es el mismo siempre
 * (los búferes se reinician); las constantes esperadas salen de compilar este archivo en el host.
 */
#ifndef CARGA_KERNEL_H
#define CARGA_KERNEL_H

#include <stdint.h>

/* En las ROMs, la ordenación se compila en Thumb (K_THUMB); en el host no hace falta. */
#ifndef K_THUMB
#define K_THUMB
#endif

#define K_WORDS 1024u /* 4 KB */
#define K_BYTES 2048u
#define K_HALFS 192u

struct k_buf {
	uint32_t w[K_WORDS], w2[K_WORDS];
	uint8_t b[K_BYTES];
	uint16_t h[K_HALFS];
};

static inline uint32_t k_xs(uint32_t x)
{
	x ^= x << 13;
	x ^= x >> 17;
	x ^= x << 5;
	return x;
}

/* cadena de datos: desplazamientos, rotaciones, suma, xor y comparaciones con salto */
static uint32_t k_alu(uint32_t n, uint32_t seed)
{
	uint32_t a = seed, b = seed ^ 0x9E3779B9u, c = 0;

	for (uint32_t i = 0; i < n; i++) {
		a = (a << 7) | (a >> 25);
		b += a ^ (b >> 3);
		c ^= (a + b) << (i & 7u);
		if ((c & 0x100u) != 0)
			a += c >> 5;
		else
			b -= c >> 11;
		c += a - (b >> 2);
	}
	return a ^ b ^ c;
}

/* relleno, copia y suma de bloques de palabras */
static uint32_t k_mem(struct k_buf *p, uint32_t seed)
{
	uint32_t s = seed, sum = 0;

	for (uint32_t i = 0; i < K_WORDS; i++) {
		s = k_xs(s);
		p->w[i] = s;
	}
	for (uint32_t r = 0; r < 4; r++)
		for (uint32_t i = 0; i < K_WORDS; i++)
			p->w2[i] = p->w[(i + r * 3u) & (K_WORDS - 1u)] + i;
	for (uint32_t i = 0; i < K_WORDS; i += 4)
		sum += p->w2[i] ^ p->w2[i + 1] ^ (p->w2[i + 2] + p->w2[i + 3]);
	return sum;
}

/* recorrido de bytes: tabla de 256 entradas, lecturas y escrituras de a un byte */
static uint32_t k_bytes(struct k_buf *p, uint32_t seed)
{
	uint32_t s = seed, h = 0x811C9DC5u;

	for (uint32_t i = 0; i < K_BYTES; i++) {
		s = k_xs(s);
		p->b[i] = (uint8_t)(s >> 11);
	}
	for (uint32_t i = 1; i < K_BYTES; i++) {
		p->b[i] = (uint8_t)(p->b[i] + p->b[i - 1u]);
		h = (h ^ p->b[i]) * 0x01000193u;
	}
	return h;
}

/* multiplicaciones: la acumulación con MLA y el producto largo */
static uint32_t k_mul(uint32_t n, uint32_t seed)
{
	uint32_t a = seed | 1u, acc = 0, hi = 0;

	for (uint32_t i = 0; i < n; i++) {
		const uint64_t p = (uint64_t)a * (uint64_t)(i + 0x1234567u);

		acc += a * (i | 3u);
		hi ^= (uint32_t)(p >> 32);
		a = (uint32_t)p ^ (a >> 1);
	}
	return acc ^ hi ^ a;
}

/* ordenación por inserción de mitades (se compila en Thumb en las ROMs) */
K_THUMB __attribute__((noinline)) static uint32_t k_sort(struct k_buf *p, uint32_t seed)
{
	uint32_t s = seed, sum = 0;

	for (uint32_t i = 0; i < K_HALFS; i++) {
		s = k_xs(s);
		p->h[i] = (uint16_t)(s >> 9);
	}
	for (uint32_t i = 1; i < K_HALFS; i++) {
		const uint16_t v = p->h[i];
		uint32_t j = i;

		while (j > 0 && p->h[j - 1u] > v) {
			p->h[j] = p->h[j - 1u];
			j--;
		}
		p->h[j] = v;
	}
	for (uint32_t i = 0; i < K_HALFS; i++)
		sum = sum * 31u + p->h[i];
	return sum;
}

#endif
