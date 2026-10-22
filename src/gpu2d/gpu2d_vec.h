/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * SalsaDS — vectores de 8 píxeles de 16 bits para el compositor 2D (Fase 2.1).
 *
 * Un vector son 8 píxeles consecutivos de una línea. Cada píxel es un índice de la paleta unificada del compositor
 * (G2D_TRANSP = transparente; 0..20991 = índice; 0x8000 | BGR555 = color directo). En el SPE son un vec_ushort8; en
 * el host, un arreglo de 8 valores con los mismos resultados (para probar la estructura del compositor en la Mac y
 * en qemu-ppc64; los kernels SIMD solo se prueban en RPCS3 y en la PS3, contra la referencia).
 *
 * También: lectura little-endian de medias palabras de la memoria de la DS y las primitivas de DMA (en el host,
 * copias).
 */
#ifndef SALSADS_GPU2D_VEC_H
#define SALSADS_GPU2D_VEC_H

#include <stdint.h>
#include <string.h>

#define G2D_TRANSP 0x7FFFu

#ifdef __SPU__
#include <spu_intrinsics.h>
#include <spu_mfcio.h>

#define G2D_INLINE static inline __attribute__((always_inline))
typedef vec_ushort8 g2d_v8;

G2D_INLINE uint16_t g2d_ld16le(const void *p)
{
	const uint16_t v = *(const volatile uint16_t *)p;
	return (uint16_t)((v >> 8) | (v << 8));
}

G2D_INLINE g2d_v8 g2d_v8_splat(uint16_t x)
{
	return spu_splats(x);
}

G2D_INLINE g2d_v8 g2d_v8_and(g2d_v8 a, g2d_v8 b)
{
	return spu_and(a, b);
}
G2D_INLINE g2d_v8 g2d_v8_or(g2d_v8 a, g2d_v8 b)
{
	return spu_or(a, b);
}
G2D_INLINE g2d_v8 g2d_v8_andc(g2d_v8 a, g2d_v8 b) /* a & ~b */
{
	return spu_andc(a, b);
}
/* Donde la máscara es 1, `b`; si no, `a`. */
G2D_INLINE g2d_v8 g2d_v8_sel(g2d_v8 a, g2d_v8 b, g2d_v8 m)
{
	return spu_sel(a, b, m);
}
G2D_INLINE g2d_v8 g2d_v8_eq(g2d_v8 a, g2d_v8 b)
{
	return spu_cmpeq(a, b);
}
G2D_INLINE g2d_v8 g2d_v8_gt(g2d_v8 a, g2d_v8 b) /* sin signo */
{
	return spu_cmpgt(a, b);
}
G2D_INLINE g2d_v8 g2d_v8_add(g2d_v8 a, g2d_v8 b)
{
	return spu_add(a, b);
}
G2D_INLINE g2d_v8 g2d_v8_shr(g2d_v8 a, int n)
{
	return spu_rlmask(a, (vec_short8)spu_splats((short)-n));
}
G2D_INLINE g2d_v8 g2d_v8_sub(g2d_v8 a, g2d_v8 b)
{
	return spu_sub(a, b);
}
G2D_INLINE g2d_v8 g2d_v8_shl(g2d_v8 a, int n)
{
	return spu_sl(a, (unsigned int)n);
}
/* Desplazamiento a la derecha con un contador por carril (0..15). */
G2D_INLINE g2d_v8 g2d_v8_shrv(g2d_v8 a, g2d_v8 n)
{
	return spu_rlmask(a, (vec_short8)spu_sub((vec_ushort8)spu_splats((unsigned short)0), n));
}
/* Las 16 bajas de a * b por carril (sin signo). */
G2D_INLINE g2d_v8 g2d_v8_mul(g2d_v8 a, g2d_v8 b)
{
	const vec_uint4 e = spu_mule(a, b), o = spu_mulo(a, b);

	return (g2d_v8)spu_shuffle(e, o, ((vec_uchar16){ 2, 3, 18, 19, 6, 7, 22, 23, 10, 11, 26, 27, 14, 15, 30, 31 }));
}
/* Un byte por carril: data[offs[k]] (offs dentro de la tabla) y, con `le16`, un par de bytes little-endian. */
G2D_INLINE g2d_v8 g2d_v8_gather8(const uint8_t *data, g2d_v8 offs)
{
	g2d_v8 r = spu_splats((unsigned short)0);

	r = spu_insert((unsigned short)data[spu_extract(offs, 0)], r, 0);
	r = spu_insert((unsigned short)data[spu_extract(offs, 1)], r, 1);
	r = spu_insert((unsigned short)data[spu_extract(offs, 2)], r, 2);
	r = spu_insert((unsigned short)data[spu_extract(offs, 3)], r, 3);
	r = spu_insert((unsigned short)data[spu_extract(offs, 4)], r, 4);
	r = spu_insert((unsigned short)data[spu_extract(offs, 5)], r, 5);
	r = spu_insert((unsigned short)data[spu_extract(offs, 6)], r, 6);
	r = spu_insert((unsigned short)data[spu_extract(offs, 7)], r, 7);
	return r;
}
G2D_INLINE g2d_v8 g2d_v8_gather16le(const uint8_t *data, g2d_v8 offs)
{
	g2d_v8 r = spu_splats((unsigned short)0);

	r = spu_insert(g2d_ld16le(data + spu_extract(offs, 0)), r, 0);
	r = spu_insert(g2d_ld16le(data + spu_extract(offs, 1)), r, 1);
	r = spu_insert(g2d_ld16le(data + spu_extract(offs, 2)), r, 2);
	r = spu_insert(g2d_ld16le(data + spu_extract(offs, 3)), r, 3);
	r = spu_insert(g2d_ld16le(data + spu_extract(offs, 4)), r, 4);
	r = spu_insert(g2d_ld16le(data + spu_extract(offs, 5)), r, 5);
	r = spu_insert(g2d_ld16le(data + spu_extract(offs, 6)), r, 6);
	r = spu_insert(g2d_ld16le(data + spu_extract(offs, 7)), r, 7);
	return r;
}
/* pal[idx] por carril (idx dentro de la tabla). */
G2D_INLINE g2d_v8 g2d_v8_lookup(const uint16_t *pal, g2d_v8 idx)
{
	g2d_v8 r = spu_splats((unsigned short)0);

	r = spu_insert(pal[spu_extract(idx, 0)], r, 0);
	r = spu_insert(pal[spu_extract(idx, 1)], r, 1);
	r = spu_insert(pal[spu_extract(idx, 2)], r, 2);
	r = spu_insert(pal[spu_extract(idx, 3)], r, 3);
	r = spu_insert(pal[spu_extract(idx, 4)], r, 4);
	r = spu_insert(pal[spu_extract(idx, 5)], r, 5);
	r = spu_insert(pal[spu_extract(idx, 6)], r, 6);
	r = spu_insert(pal[spu_extract(idx, 7)], r, 7);
	return r;
}
G2D_INLINE int g2d_v8_any(g2d_v8 m)
{
	return spu_extract(spu_gather(m), 0) != 0;
}
G2D_INLINE uint32_t g2d_v8_lane(g2d_v8 v, int k)
{
	return spu_extract(v, k);
}
G2D_INLINE g2d_v8 g2d_v8_put(g2d_v8 v, int k, uint16_t x)
{
	return spu_insert(x, v, k);
}
/* Ocho palabras de 32 bits (q alineado a 16) separadas en sus mitades altas y bajas. */
G2D_INLINE void g2d_v8_split32(const uint32_t *q, g2d_v8 *hi, g2d_v8 *lo)
{
	static const vec_uchar16 phi = { 0, 1, 4, 5, 8, 9, 12, 13, 16, 17, 20, 21, 24, 25, 28, 29 };
	static const vec_uchar16 plo = { 2, 3, 6, 7, 10, 11, 14, 15, 18, 19, 22, 23, 26, 27, 30, 31 };
	const vec_uint4 a = *(const vec_uint4 *)(const void *)q, b = *(const vec_uint4 *)(const void *)(q + 4);

	*hi = (g2d_v8)spu_shuffle(a, b, phi);
	*lo = (g2d_v8)spu_shuffle(a, b, plo);
}

/* Patrones de shuffle por desplazamiento (no hay suma de bytes en el SPU). */
static const vec_uchar16 g2d_window_pat[8] = {
	{ 0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15 },
	{ 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16, 17 },
	{ 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16, 17, 18, 19 },
	{ 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16, 17, 18, 19, 20, 21 },
	{ 8, 9, 10, 11, 12, 13, 14, 15, 16, 17, 18, 19, 20, 21, 22, 23 },
	{ 10, 11, 12, 13, 14, 15, 16, 17, 18, 19, 20, 21, 22, 23, 24, 25 },
	{ 12, 13, 14, 15, 16, 17, 18, 19, 20, 21, 22, 23, 24, 25, 26, 27 },
	{ 14, 15, 16, 17, 18, 19, 20, 21, 22, 23, 24, 25, 26, 27, 28, 29 },
};
static const vec_uchar16 g2d_place_pat[8] = {
	{ 16, 17, 18, 19, 20, 21, 22, 23, 24, 25, 26, 27, 28, 29, 30, 31 },
	{ 14, 15, 16, 17, 18, 19, 20, 21, 22, 23, 24, 25, 26, 27, 28, 29 },
	{ 12, 13, 14, 15, 16, 17, 18, 19, 20, 21, 22, 23, 24, 25, 26, 27 },
	{ 10, 11, 12, 13, 14, 15, 16, 17, 18, 19, 20, 21, 22, 23, 24, 25 },
	{ 8, 9, 10, 11, 12, 13, 14, 15, 16, 17, 18, 19, 20, 21, 22, 23 },
	{ 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16, 17, 18, 19, 20, 21 },
	{ 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16, 17, 18, 19 },
	{ 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16, 17 },
};

/* Los 8 píxeles de una fila de tesela de 4 bits (4 bytes, alineados a 4; el nibble bajo es el píxel de la izquierda).
 * `base` se suma al número de color (banco de paleta × 16, más 256 si es OBJ); el color 0 es transparente. */
G2D_INLINE g2d_v8 g2d_v8_expand4(const uint8_t *p, uint32_t base, int hflip)
{
	const vec_uchar16 v = (vec_uchar16)spu_promote(*(const uint32_t *)(const void *)p, 0);
	const vec_uchar16 sp = spu_shuffle(v, v, ((vec_uchar16){ 0x80, 0, 0x80, 0, 0x80, 1, 0x80, 1,
								0x80, 2, 0x80, 2, 0x80, 3, 0x80, 3 }));
	vec_ushort8 c = spu_rlmask((vec_ushort8)sp, ((vec_short8){ 0, -4, 0, -4, 0, -4, 0, -4 }));
	c = spu_and(c, (unsigned short)15);
	const vec_ushort8 tr = spu_cmpeq(c, (unsigned short)0);
	g2d_v8 r = spu_sel(spu_add(c, (unsigned short)base), spu_splats((unsigned short)G2D_TRANSP), tr);
	if (hflip)
		r = spu_shuffle(r, r, ((vec_uchar16){ 14, 15, 12, 13, 10, 11, 8, 9, 6, 7, 4, 5, 2, 3, 0, 1 }));
	return r;
}

/* Fila de tesela de 8 bits (8 bytes, alineados a 8). */
G2D_INLINE g2d_v8 g2d_v8_expand8(const uint8_t *p, uint32_t base, int hflip)
{
	const vec_uchar16 v = (vec_uchar16)spu_promote(*(const unsigned long long *)(const void *)p, 0);
	const vec_ushort8 c = (vec_ushort8)spu_shuffle(v, v, ((vec_uchar16){ 0x80, 0, 0x80, 1, 0x80, 2, 0x80, 3,
									     0x80, 4, 0x80, 5, 0x80, 6, 0x80, 7 }));
	const vec_ushort8 tr = spu_cmpeq(c, (unsigned short)0);
	g2d_v8 r = spu_sel(spu_add(c, (unsigned short)base), spu_splats((unsigned short)G2D_TRANSP), tr);
	if (hflip)
		r = spu_shuffle(r, r, ((vec_uchar16){ 14, 15, 12, 13, 10, 11, 8, 9, 6, 7, 4, 5, 2, 3, 0, 1 }));
	return r;
}

/* Ocho píxeles de color directo (16 bytes little-endian, alineados a 16): 0x8000 | color si el bit 15 está, si no
 * transparente. */
G2D_INLINE g2d_v8 g2d_v8_expand16(const uint8_t *p, int hflip)
{
	const vec_uchar16 v = *(const vec_uchar16 *)(const void *)p;
	vec_ushort8 c = (vec_ushort8)spu_shuffle(v, v, ((vec_uchar16){ 1, 0, 3, 2, 5, 4, 7, 6, 9, 8, 11, 10, 13, 12, 15, 14 }));
	const vec_ushort8 op = spu_cmpgt(c, (unsigned short)0x7FFF);
	g2d_v8 r = spu_sel(spu_splats((unsigned short)G2D_TRANSP), c, op);
	if (hflip)
		r = spu_shuffle(r, r, ((vec_uchar16){ 14, 15, 12, 13, 10, 11, 8, 9, 6, 7, 4, 5, 2, 3, 0, 1 }));
	return r;
}

/* Los 8 píxeles a partir del `off` (0..7) de la concatenación a|b. */
G2D_INLINE g2d_v8 g2d_v8_window(g2d_v8 a, g2d_v8 b, uint32_t off)
{
	return spu_shuffle(a, b, g2d_window_pat[off]);
}

/* Pone `t` desplazado `off` píxeles a la derecha sobre dos vectores consecutivos: lo que no cubre queda transparente. */
G2D_INLINE void g2d_v8_place(g2d_v8 t, uint32_t off, g2d_v8 *lo, g2d_v8 *hi)
{
	const vec_uchar16 sel = g2d_place_pat[off];
	const g2d_v8 ff = spu_splats((unsigned short)G2D_TRANSP);
	*lo = spu_shuffle(ff, t, sel);
	*hi = spu_shuffle(t, ff, sel);
}

#define G2D_STAGE(x) ((void)0)

#ifdef G2D_PROF
#define G2D_T() spu_read_decrementer()
#define G2D_ACC(c, field, t0) ((c)->prof.field += (uint32_t)((t0) - spu_read_decrementer()))
#else
#define G2D_T() 0u
#define G2D_ACC(c, field, t0) ((void)(t0))
#endif

/* DMA. `tag` 0..30. */
typedef struct g2d_el {
	uint32_t size;  /* bit 31: detener y avisar (no se usa) */
	uint32_t eal;
} __attribute__((aligned(8))) g2d_el;

G2D_INLINE void g2d_get(void *ls, uint64_t ea, uint32_t size, uint32_t tag)
{
	mfc_get(ls, ea, size, tag, 0, 0);
}
G2D_INLINE void g2d_put(const void *ls, uint64_t ea, uint32_t size, uint32_t tag)
{
	mfc_put((volatile void *)ls, ea, size, tag, 0, 0);
}
/* Trae `n` bloques de `size` bytes que están en `base + offs[i]` y los deja seguidos en `ls`. Todos dentro de la
 * misma ventana de 4 GB que `base`. (Con `size` distinto de múltiplo de 16 el DMA no sirve: aquí 16, 32, 64.) */
G2D_INLINE void g2d_gather(void *ls, uint64_t base, const uint32_t *offs, uint32_t n, uint32_t size, uint32_t tag,
			   g2d_el *list)
{
	const uint32_t lo = (uint32_t)base;
	for (uint32_t i = 0; i < n; i++) {
		list[i].size = size;
		list[i].eal = lo + offs[i];
	}
	mfc_getl(ls, base, list, n * sizeof(g2d_el), tag, 0, 0);
}
G2D_INLINE void g2d_wait(uint32_t mask)
{
	if (mask) {
		mfc_write_tag_mask(mask);
		mfc_read_tag_status_all();
	}
}

#else /* host y PPE: versión portable */

#include "util/endian.h"

#define G2D_INLINE static inline
typedef struct g2d_v8 {
	uint16_t l[8];
} g2d_v8;

G2D_INLINE uint16_t g2d_ld16le(const void *p)
{
	return rd16le(p);
}

G2D_INLINE g2d_v8 g2d_v8_splat(uint16_t x)
{
	g2d_v8 r;
	for (int i = 0; i < 8; i++)
		r.l[i] = x;
	return r;
}

#define G2D_V8_BIN(NAME, EXPR)                                                                              \
	G2D_INLINE g2d_v8 NAME(g2d_v8 a, g2d_v8 b)                                                          \
	{                                                                                                   \
		g2d_v8 r;                                                                                   \
		for (int i = 0; i < 8; i++)                                                                 \
			r.l[i] = (uint16_t)(EXPR);                                                          \
		return r;                                                                                   \
	}
G2D_V8_BIN(g2d_v8_and, a.l[i] & b.l[i])
G2D_V8_BIN(g2d_v8_or, a.l[i] | b.l[i])
G2D_V8_BIN(g2d_v8_andc, a.l[i] & ~b.l[i])
G2D_V8_BIN(g2d_v8_eq, a.l[i] == b.l[i] ? 0xFFFF : 0)
G2D_V8_BIN(g2d_v8_gt, a.l[i] > b.l[i] ? 0xFFFF : 0)
G2D_V8_BIN(g2d_v8_add, a.l[i] + b.l[i])

G2D_INLINE g2d_v8 g2d_v8_sel(g2d_v8 a, g2d_v8 b, g2d_v8 m)
{
	g2d_v8 r;
	for (int i = 0; i < 8; i++)
		r.l[i] = (uint16_t)((a.l[i] & ~m.l[i]) | (b.l[i] & m.l[i]));
	return r;
}
G2D_INLINE g2d_v8 g2d_v8_shr(g2d_v8 a, int n)
{
	g2d_v8 r;
	for (int i = 0; i < 8; i++)
		r.l[i] = (uint16_t)(a.l[i] >> n);
	return r;
}
G2D_V8_BIN(g2d_v8_sub, a.l[i] - b.l[i])
G2D_V8_BIN(g2d_v8_mul, (uint32_t)a.l[i] * b.l[i])
G2D_INLINE g2d_v8 g2d_v8_shl(g2d_v8 a, int n)
{
	g2d_v8 r;
	for (int i = 0; i < 8; i++)
		r.l[i] = (uint16_t)(a.l[i] << n);
	return r;
}
G2D_INLINE g2d_v8 g2d_v8_shrv(g2d_v8 a, g2d_v8 n)
{
	g2d_v8 r;
	for (int i = 0; i < 8; i++)
		r.l[i] = (uint16_t)(a.l[i] >> (n.l[i] & 15));
	return r;
}
G2D_INLINE g2d_v8 g2d_v8_lookup(const uint16_t *pal, g2d_v8 idx)
{
	g2d_v8 r;
	for (int i = 0; i < 8; i++)
		r.l[i] = pal[idx.l[i]];
	return r;
}
G2D_INLINE g2d_v8 g2d_v8_gather8(const uint8_t *data, g2d_v8 offs)
{
	g2d_v8 r;
	for (int i = 0; i < 8; i++)
		r.l[i] = data[offs.l[i]];
	return r;
}
G2D_INLINE g2d_v8 g2d_v8_gather16le(const uint8_t *data, g2d_v8 offs)
{
	g2d_v8 r;
	for (int i = 0; i < 8; i++)
		r.l[i] = rd16le(data + offs.l[i]);
	return r;
}
G2D_INLINE int g2d_v8_any(g2d_v8 m)
{
	for (int i = 0; i < 8; i++)
		if (m.l[i])
			return 1;
	return 0;
}
G2D_INLINE uint32_t g2d_v8_lane(g2d_v8 v, int k)
{
	return v.l[k];
}
G2D_INLINE g2d_v8 g2d_v8_put(g2d_v8 v, int k, uint16_t x)
{
	v.l[k] = x;
	return v;
}
G2D_INLINE void g2d_v8_split32(const uint32_t *q, g2d_v8 *hi, g2d_v8 *lo)
{
	for (int i = 0; i < 8; i++) {
		hi->l[i] = (uint16_t)(q[i] >> 16);
		lo->l[i] = (uint16_t)q[i];
	}
}

G2D_INLINE g2d_v8 g2d_v8_expand4(const uint8_t *p, uint32_t base, int hflip)
{
	g2d_v8 r;
	for (int i = 0; i < 8; i++) {
		const uint32_t c = (i & 1) ? (uint32_t)(p[i >> 1] >> 4) : (uint32_t)(p[i >> 1] & 15);
		r.l[hflip ? 7 - i : i] = c ? (uint16_t)(c + base) : (uint16_t)G2D_TRANSP;
	}
	return r;
}

G2D_INLINE g2d_v8 g2d_v8_expand8(const uint8_t *p, uint32_t base, int hflip)
{
	g2d_v8 r;
	for (int i = 0; i < 8; i++)
		r.l[hflip ? 7 - i : i] = p[i] ? (uint16_t)(p[i] + base) : (uint16_t)G2D_TRANSP;
	return r;
}

G2D_INLINE g2d_v8 g2d_v8_expand16(const uint8_t *p, int hflip)
{
	g2d_v8 r;
	for (int i = 0; i < 8; i++) {
		const uint16_t c = rd16le(p + 2 * i);
		r.l[hflip ? 7 - i : i] = (c & 0x8000u) ? c : (uint16_t)G2D_TRANSP;
	}
	return r;
}

G2D_INLINE g2d_v8 g2d_v8_window(g2d_v8 a, g2d_v8 b, uint32_t off)
{
	g2d_v8 r;
	for (uint32_t i = 0; i < 8; i++)
		r.l[i] = i + off < 8 ? a.l[i + off] : b.l[i + off - 8];
	return r;
}

G2D_INLINE void g2d_v8_place(g2d_v8 t, uint32_t off, g2d_v8 *lo, g2d_v8 *hi)
{
	for (uint32_t i = 0; i < 8; i++) {
		lo->l[i] = i >= off ? t.l[i - off] : (uint16_t)G2D_TRANSP;
		hi->l[i] = i < off ? t.l[8 - off + i] : (uint16_t)G2D_TRANSP;
	}
}

typedef struct g2d_el {
	uint32_t size, eal;
} g2d_el;

/* Contadores de DMA del host (diagnóstico: cuántas órdenes y cuántos bytes pide cada etapa de un cuadro; el costo en el
 * SPE depende de las órdenes, del tamaño de cada trozo y de las esperas, no solo de los bytes). `g2d_stage` lo fija
 * el compositor al entrar a cada etapa. */
enum { G2D_ST_INIT, G2D_ST_PLAN, G2D_ST_TEXT, G2D_ST_AFFINE, G2D_ST_OBJ, G2D_ST_COMPOSE, G2D_ST_EXTPAL, G2D_ST_COUNT };
enum { G2D_C_GET, G2D_C_GET_BYTES, G2D_C_LIST, G2D_C_LIST_EL, G2D_C_LIST_BYTES, G2D_C_PUT, G2D_C_PUT_BYTES, G2D_C_WAIT, G2D_C_COUNT };
extern uint64_t g2d_dma_cnt[G2D_ST_COUNT][G2D_C_COUNT];
extern int g2d_stage;
#define G2D_STAGE(x) (g2d_stage = (x))
#ifdef G2D_PROF_HOST
#include <time.h>
static inline uint32_t g2d_host_ticks(void)
{
	struct timespec t;
	clock_gettime(CLOCK_MONOTONIC, &t);
	return (uint32_t)((uint64_t)t.tv_sec * 1000000000u + (uint32_t)t.tv_nsec);
}
#define G2D_T() g2d_host_ticks()
#define G2D_ACC(c, field, t0) ((c)->prof.field += G2D_T() - (t0))
#else
#define G2D_T() 0u
#define G2D_ACC(c, field, t0) ((void)(t0))
#endif

G2D_INLINE void g2d_get(void *ls, uint64_t ea, uint32_t size, uint32_t tag)
{
	(void)tag;
	g2d_dma_cnt[g2d_stage][G2D_C_GET]++;
	g2d_dma_cnt[g2d_stage][G2D_C_GET_BYTES] += size;
	memcpy(ls, (const void *)(uintptr_t)ea, size);
}
G2D_INLINE void g2d_put(const void *ls, uint64_t ea, uint32_t size, uint32_t tag)
{
	(void)tag;
	g2d_dma_cnt[g2d_stage][G2D_C_PUT]++;
	g2d_dma_cnt[g2d_stage][G2D_C_PUT_BYTES] += size;
	memcpy((void *)(uintptr_t)ea, ls, size);
}
G2D_INLINE void g2d_gather(void *ls, uint64_t base, const uint32_t *offs, uint32_t n, uint32_t size, uint32_t tag,
			   g2d_el *list)
{
	(void)tag;
	(void)list;
	g2d_dma_cnt[g2d_stage][G2D_C_LIST]++;
	g2d_dma_cnt[g2d_stage][G2D_C_LIST_EL] += n;
	g2d_dma_cnt[g2d_stage][G2D_C_LIST_BYTES] += (uint64_t)n * size;
	for (uint32_t i = 0; i < n; i++)
		memcpy((uint8_t *)ls + (size_t)i * size, (const void *)(uintptr_t)(base + offs[i]), size);
}
G2D_INLINE void g2d_wait(uint32_t mask)
{
	if (mask)
		g2d_dma_cnt[g2d_stage][G2D_C_WAIT]++;
}

#endif

#endif
