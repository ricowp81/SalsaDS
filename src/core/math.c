/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * SalsaDS — división y raíz cuadrada por hardware del ARM9 (DIVCNT/DIV_*, SQRTCNT/SQRT_*).
 *
 * El resultado se calcula al escribir cualquiera de los registros de entrada y se lee ya listo; solo la
 * bandera de ocupado (bit 15) simula la duración (18 o 34 ciclos de bus la división, 13 la raíz). División
 * entre cero: cociente +1 o -1 de signo contrario al numerador y resto igual al numerador, con el bit 14 de
 * DIVCNT; -MAX / -1 da -MAX (en el modo 32/32 la mitad alta se invierte). GBATEK, «DS Maths».
 *
 * Código propio.
 */
#include "core/nds_priv.h"

static uint64_t isqrt64(uint64_t x)
{
	uint64_t r = 0, bit = 1ull << 62;

	while (bit > x)
		bit >>= 2;
	while (bit) {
		if (x >= r + bit) {
			x -= r + bit;
			r = (r >> 1) + bit;
		} else {
			r >>= 1;
		}
		bit >>= 2;
	}
	return r;
}

static void start_div(struct nds *n)
{
	struct nds_math *m = &n->math;
	const unsigned mode = m->divcnt & 3u;
	int64_t num, den, q, r;

	if (mode == 0) {
		num = (int32_t)(uint32_t)m->numer;
		den = (int32_t)(uint32_t)m->denom;
	} else if (mode == 1 || mode == 3) {
		num = (int64_t)m->numer;
		den = (int32_t)(uint32_t)m->denom;
	} else {
		num = (int64_t)m->numer;
		den = (int64_t)m->denom;
	}
	m->div0 = m->denom == 0;
	if (den == 0) {
		q = num < 0 ? 1 : -1;
		r = num;
		if (mode == 0)
			q ^= (int64_t)0xFFFFFFFF00000000ull;
	} else if (mode != 2 && num == (mode == 0 ? (int64_t)INT32_MIN : INT64_MIN) && den == -1) {
		q = num;
		r = 0;
		if (mode == 0)
			q = (int64_t)0x0000000080000000ull;
	} else if (mode == 2 && num == INT64_MIN && den == -1) {
		q = num;
		r = 0;
	} else {
		q = num / den;
		r = num % den;
	}
	m->quot = (uint64_t)q;
	m->rem = (uint64_t)r;
	m->div_done = nds_cpu_now(n, NDS_CPU9) + (uint64_t)(mode == 0 ? 18 : 34) * 2;
}

static void start_sqrt(struct nds *n)
{
	struct nds_math *m = &n->math;
	const uint64_t p = (m->sqrtcnt & 1u) ? m->sqrt_param : (m->sqrt_param & 0xFFFFFFFFu);

	m->sqrt_res = (uint32_t)isqrt64(p);
	m->sqrt_done = nds_cpu_now(n, NDS_CPU9) + 13u * 2;
}

static uint8_t byte_of(const struct nds *n, uint32_t off)
{
	const struct nds_math *m = &n->math;
	const uint64_t now = nds_cpu_now(n, NDS_CPU9);
	uint64_t v = 0;
	unsigned sh = (off & 7u) * 8u;

	switch (off & ~7u) {
	case 0x280:
		if ((off & 7u) < 2) {
			v = (m->divcnt & 3u) | (m->div0 ? 0x4000u : 0u) | (now < m->div_done ? 0x8000u : 0u);
			return (uint8_t)(v >> sh);
		}
		return 0;
	case 0x290: v = m->numer; break;
	case 0x298: v = m->denom; break;
	case 0x2A0: v = m->quot; break;
	case 0x2A8: v = m->rem; break;
	case 0x2B0:
		if ((off & 7u) < 2) {
			v = (m->sqrtcnt & 1u) | (now < m->sqrt_done ? 0x8000u : 0u);
			return (uint8_t)(v >> sh);
		}
		if ((off & 7u) >= 4)
			return (uint8_t)(m->sqrt_res >> ((off & 3u) * 8u));
		return 0;
	case 0x2B8: v = m->sqrt_param; break;
	default: return 0;
	}
	return (uint8_t)(v >> sh);
}

uint32_t nds_math_read(struct nds *n, uint32_t off, int sz)
{
	uint32_t v = 0;

	for (int i = 0; i < sz; i++)
		v |= (uint32_t)byte_of(n, off + (uint32_t)i) << (8 * i);
	return v;
}

static void put(uint64_t *reg, uint32_t byteoff, int sz, uint32_t v, unsigned width)
{
	for (int i = 0; i < sz; i++) {
		const unsigned b = byteoff + (unsigned)i;

		if (b >= width)
			break;
		*reg = (*reg & ~(0xFFull << (8 * b))) | ((uint64_t)((v >> (8 * i)) & 0xFFu) << (8 * b));
	}
}

void nds_math_write(struct nds *n, uint32_t off, int sz, uint32_t v)
{
	struct nds_math *m = &n->math;

	switch (off & ~7u) {
	case 0x280: {
		uint64_t c = m->divcnt;

		put(&c, off & 7u, sz, v, 2);
		m->divcnt = (uint16_t)(c & 3u);
		start_div(n);
		return;
	}
	case 0x290:
		put(&m->numer, off & 7u, sz, v, 8);
		start_div(n);
		return;
	case 0x298:
		put(&m->denom, off & 7u, sz, v, 8);
		start_div(n);
		return;
	case 0x2B0:
		if ((off & 7u) < 2) {
			uint64_t c = m->sqrtcnt;

			put(&c, off & 7u, sz, v, 2);
			m->sqrtcnt = (uint16_t)(c & 1u);
			start_sqrt(n);
		}
		return;
	case 0x2B8:
		put(&m->sqrt_param, off & 7u, sz, v, 8);
		start_sqrt(n);
		return;
	default:
		return;
	}
}
