/*
 * SalsaDS — vectores de 8 píxeles de 16 bits para el compositor 2D.
 *
 * Un vector son 8 píxeles consecutivos de una línea. Cada píxel es el índice
 * de la paleta (0..511: fondos 0..255, OBJ 256..511) o 0xFFFF si es
 * transparente. En el SPE son un vec_ushort8; en el host, un arreglo de 8
 * valores con los mismos resultados (para probar la estructura del
 * compositor en la Mac y en qemu-ppc64; los kernels SIMD solo se prueban
 * en RPCS3 y en la PS3, contra la referencia).
 *
 * También: lectura little-endian de medias palabras de la memoria de la DS
 * y las primitivas de DMA (en el host, copias).
 */
#ifndef SALSADS_GPU2D_VEC_H
#define SALSADS_GPU2D_VEC_H

#include <stdint.h>
#include <string.h>

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

/* Donde `src` no es transparente (0xFFFF) gana `src`; si no, `dst`. */
G2D_INLINE g2d_v8 g2d_v8_merge(g2d_v8 dst, g2d_v8 src)
{
	return spu_sel(src, dst, spu_cmpeq(src, spu_splats((unsigned short)0xFFFF)));
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

/* Los 8 píxeles de una fila de tesela de 4 bits (4 bytes, alineados a 4;
 * el nibble bajo es el píxel de la izquierda). `base` se suma al número de
 * color (banco de paleta × 16, más 256 si es OBJ). */
G2D_INLINE g2d_v8 g2d_v8_expand4(const uint8_t *p, uint32_t base, int hflip)
{
	const vec_uchar16 v = (vec_uchar16)spu_promote(*(const uint32_t *)(const void *)p, 0);
	const vec_uchar16 sp = spu_shuffle(v, v, ((vec_uchar16){ 0x80, 0, 0x80, 0, 0x80, 1, 0x80, 1,
								0x80, 2, 0x80, 2, 0x80, 3, 0x80, 3 }));
	vec_ushort8 c = spu_rlmask((vec_ushort8)sp, ((vec_short8){ 0, -4, 0, -4, 0, -4, 0, -4 }));
	c = spu_and(c, (unsigned short)15);
	const vec_ushort8 tr = spu_cmpeq(c, (unsigned short)0);
	g2d_v8 r = spu_sel(spu_add(c, (unsigned short)base), spu_splats((unsigned short)0xFFFF), tr);
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
	g2d_v8 r = spu_sel(spu_add(c, (unsigned short)base), spu_splats((unsigned short)0xFFFF), tr);
	if (hflip)
		r = spu_shuffle(r, r, ((vec_uchar16){ 14, 15, 12, 13, 10, 11, 8, 9, 6, 7, 4, 5, 2, 3, 0, 1 }));
	return r;
}

/* Los 8 píxeles a partir del `off` (0..7) de la concatenación a|b. */
G2D_INLINE g2d_v8 g2d_v8_window(g2d_v8 a, g2d_v8 b, uint32_t off)
{
	return spu_shuffle(a, b, g2d_window_pat[off]);
}

/* Pone `t` desplazado `off` píxeles a la derecha sobre dos vectores
 * consecutivos: lo que no cubre queda transparente. */
G2D_INLINE void g2d_v8_place(g2d_v8 t, uint32_t off, g2d_v8 *lo, g2d_v8 *hi)
{
	const vec_uchar16 sel = g2d_place_pat[off];
	const g2d_v8 ff = spu_splats((unsigned short)0xFFFF);
	*lo = spu_shuffle(ff, t, sel);
	*hi = spu_shuffle(t, ff, sel);
}

G2D_INLINE uint32_t g2d_v8_lane(g2d_v8 v, int k)
{
	return spu_extract(v, k);
}

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
/* Trae `n` bloques de `size` bytes que están en `base + offs[i]` y los deja
 * seguidos en `ls`. Todos dentro de la misma ventana de 4 GB que `base`. */
G2D_INLINE void g2d_gather(void *ls, uint64_t base, const uint32_t *offs, uint32_t n, uint32_t size,
			   uint32_t tag, g2d_el *list)
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

G2D_INLINE g2d_v8 g2d_v8_merge(g2d_v8 dst, g2d_v8 src)
{
	for (int i = 0; i < 8; i++)
		if (src.l[i] != 0xFFFF)
			dst.l[i] = src.l[i];
	return dst;
}

G2D_INLINE g2d_v8 g2d_v8_expand4(const uint8_t *p, uint32_t base, int hflip)
{
	g2d_v8 r;
	for (int i = 0; i < 8; i++) {
		const uint32_t c = (i & 1) ? (uint32_t)(p[i >> 1] >> 4) : (uint32_t)(p[i >> 1] & 15);
		r.l[hflip ? 7 - i : i] = c ? (uint16_t)(c + base) : (uint16_t)0xFFFF;
	}
	return r;
}

G2D_INLINE g2d_v8 g2d_v8_expand8(const uint8_t *p, uint32_t base, int hflip)
{
	g2d_v8 r;
	for (int i = 0; i < 8; i++)
		r.l[hflip ? 7 - i : i] = p[i] ? (uint16_t)(p[i] + base) : (uint16_t)0xFFFF;
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
		lo->l[i] = i >= off ? t.l[i - off] : (uint16_t)0xFFFF;
		hi->l[i] = i < off ? t.l[8 - off + i] : (uint16_t)0xFFFF;
	}
}

G2D_INLINE uint32_t g2d_v8_lane(g2d_v8 v, int k)
{
	return v.l[k];
}

typedef struct g2d_el {
	uint32_t size, eal;
} g2d_el;

G2D_INLINE void g2d_get(void *ls, uint64_t ea, uint32_t size, uint32_t tag)
{
	(void)tag;
	memcpy(ls, (const void *)(uintptr_t)ea, size);
}
G2D_INLINE void g2d_put(const void *ls, uint64_t ea, uint32_t size, uint32_t tag)
{
	(void)tag;
	memcpy((void *)(uintptr_t)ea, ls, size);
}
G2D_INLINE void g2d_gather(void *ls, uint64_t base, const uint32_t *offs, uint32_t n, uint32_t size,
			   uint32_t tag, g2d_el *list)
{
	(void)tag;
	(void)list;
	for (uint32_t i = 0; i < n; i++)
		memcpy((uint8_t *)ls + (size_t)i * size, (const void *)(uintptr_t)(base + offs[i]), size);
}
G2D_INLINE void g2d_wait(uint32_t mask)
{
	(void)mask;
}

#endif

#endif
