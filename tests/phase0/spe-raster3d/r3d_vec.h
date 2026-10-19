/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * SalsaDS — Fase 0.4: vectores de 4 carriles para el rasterizador.
 *
 * El código del núcleo (r3d_core.c) se escribe con los vectores genéricos de
 * GCC (vector_size(16)), que spu-gcc traduce a registros del SPU y el gcc del
 * host (o del PPE) a carriles sueltos. Lo que en el SPU tiene una instrucción
 * propia y en C genérico sería lento o distinto va en estas funciones:
 * comparaciones y selección (cgt/clgt/selb), recíproco (frest + fi + un paso
 * de Newton), multiplicaciones de 16 bits (mpyu/mpyh: la de 32 bits son cinco
 * instrucciones), bytes de un texel (shufb) y máscaras a bits (gb).
 *
 * Diferencias conocidas entre el SPU y el host: el SPU redondea la simple
 * precisión hacia cero y su recíproco no es el correctamente redondeado; el
 * host redondea al más cercano. Por eso el resultado sombreado del SPE no es
 * idéntico bit a bit al del host (se compara con tolerancia), y todo lo que
 * decide qué píxel se dibuja (bordes, recorridos, z en capas separadas) va en
 * enteros o con margen.
 */
#ifndef SALSADS_R3D_VEC_H
#define SALSADS_R3D_VEC_H

#include <stdint.h>

typedef int32_t vi4 __attribute__((vector_size(16)));
typedef uint32_t vu4 __attribute__((vector_size(16)));
typedef float vf4 __attribute__((vector_size(16)));
typedef int16_t vs8 __attribute__((vector_size(16)));

#define R3D_INLINE static inline __attribute__((always_inline))

#if defined(__SPU__)
#include <spu_intrinsics.h>

R3D_INLINE vi4 v_splati(int32_t x) { return (vi4)spu_splats(x); }
R3D_INLINE vu4 v_splatu(uint32_t x) { return (vu4)spu_splats(x); }
R3D_INLINE vf4 v_splatf(float x) { return (vf4)spu_splats(x); }
R3D_INLINE vs8 v_splats16(int16_t x) { return (vs8)spu_splats(x); }

R3D_INLINE vu4 v_gt_s(vi4 a, vi4 b) { return (vu4)spu_cmpgt((vec_int4)a, (vec_int4)b); }
R3D_INLINE vu4 v_gt_u(vu4 a, vu4 b) { return (vu4)spu_cmpgt((vec_uint4)a, (vec_uint4)b); }
R3D_INLINE vu4 v_gt_f(vf4 a, vf4 b) { return (vu4)spu_cmpgt((vec_float4)a, (vec_float4)b); }
R3D_INLINE vs8 v_gt_s16(vs8 a, vs8 b) { return (vs8)spu_cmpgt((vec_short8)a, (vec_short8)b); }

/* m ? b : a, por bits */
R3D_INLINE vu4 v_selu(vu4 a, vu4 b, vu4 m) { return (vu4)spu_sel((vec_uint4)a, (vec_uint4)b, (vec_uint4)m); }
R3D_INLINE vi4 v_seli(vi4 a, vi4 b, vu4 m) { return (vi4)spu_sel((vec_int4)a, (vec_int4)b, (vec_uint4)m); }
R3D_INLINE vf4 v_self(vf4 a, vf4 b, vu4 m) { return (vf4)spu_sel((vec_float4)a, (vec_float4)b, (vec_uint4)m); }

/* spu_re ya es fi(x, frest(x)) (~12 bits; spu_internals.h de spu-gcc): otro
 * fi encima lo estropea. Un paso de Newton lo lleva a ~23 bits. */
R3D_INLINE vf4 v_recip(vf4 x)
{
	vec_float4 y = spu_re((vec_float4)x);
	vec_float4 e = spu_nmsub((vec_float4)x, y, spu_splats(1.0f)); /* 1 − x·y */
	return (vf4)spu_madd(e, y, y);
}

/* a·b con a y b < 2^16 (sin signo) */
R3D_INLINE vu4 v_mul16(vu4 a, vu4 b) { return (vu4)spu_mulo((vec_ushort8)a, (vec_ushort8)b); }

/* a·b módulo 2^32 con b < 2^16 (sin signo); a cualquiera */
R3D_INLINE vi4 v_mul32x16(vi4 a, vi4 b)
{
	vec_int4 hi = spu_mulh((vec_short8)a, (vec_short8)b);
	vec_uint4 lo = spu_mulo((vec_ushort8)a, (vec_ushort8)b);
	return (vi4)spu_add((vec_uint4)hi, lo);
}

/* Byte k (0 = el menos significativo) de cada palabra, en el byte bajo. */
R3D_INLINE vu4 v_byte0(vu4 v)
{
	const vec_uchar16 p = { 0x80, 0x80, 0x80, 3, 0x80, 0x80, 0x80, 7, 0x80, 0x80, 0x80, 11, 0x80, 0x80, 0x80, 15 };
	return (vu4)spu_shuffle((vec_uint4)v, (vec_uint4)v, p);
}
R3D_INLINE vu4 v_byte1(vu4 v)
{
	const vec_uchar16 p = { 0x80, 0x80, 0x80, 2, 0x80, 0x80, 0x80, 6, 0x80, 0x80, 0x80, 10, 0x80, 0x80, 0x80, 14 };
	return (vu4)spu_shuffle((vec_uint4)v, (vec_uint4)v, p);
}
R3D_INLINE vu4 v_byte2(vu4 v)
{
	const vec_uchar16 p = { 0x80, 0x80, 0x80, 1, 0x80, 0x80, 0x80, 5, 0x80, 0x80, 0x80, 9, 0x80, 0x80, 0x80, 13 };
	return (vu4)spu_shuffle((vec_uint4)v, (vec_uint4)v, p);
}
R3D_INLINE vu4 v_byte3(vu4 v)
{
	const vec_uchar16 p = { 0x80, 0x80, 0x80, 0, 0x80, 0x80, 0x80, 4, 0x80, 0x80, 0x80, 8, 0x80, 0x80, 0x80, 12 };
	return (vu4)spu_shuffle((vec_uint4)v, (vec_uint4)v, p);
}

/* r | g << 8 | b << 16 | a << 24 con cada uno < 256 */
R3D_INLINE vu4 v_pack4(vu4 r, vu4 g, vu4 b, vu4 a)
{
	const vec_uchar16 prg = { 0x80, 0x80, 0x13, 0x03, 0x80, 0x80, 0x17, 0x07,
				  0x80, 0x80, 0x1B, 0x0B, 0x80, 0x80, 0x1F, 0x0F };
	const vec_uchar16 pba = { 0x13, 0x03, 0x80, 0x80, 0x17, 0x07, 0x80, 0x80,
				  0x1B, 0x0B, 0x80, 0x80, 0x1F, 0x0F, 0x80, 0x80 };
	vec_uint4 rg = spu_shuffle((vec_uint4)r, (vec_uint4)g, prg);
	vec_uint4 ba = spu_shuffle((vec_uint4)b, (vec_uint4)a, pba);
	return (vu4)spu_or(rg, ba);
}

/* Un bit por carril de 16 bits (carril 0 = bit 7). */
R3D_INLINE uint32_t v_bits16(vs8 m) { return spu_extract(spu_gather((vec_short8)m), 0); }

#else /* host o PPE: carriles sueltos */

R3D_INLINE vi4 v_splati(int32_t x) { return (vi4){ x, x, x, x }; }
R3D_INLINE vu4 v_splatu(uint32_t x) { return (vu4){ x, x, x, x }; }
R3D_INLINE vf4 v_splatf(float x) { return (vf4){ x, x, x, x }; }
R3D_INLINE vs8 v_splats16(int16_t x) { return (vs8){ x, x, x, x, x, x, x, x }; }

R3D_INLINE vu4 v_gt_s(vi4 a, vi4 b) { return (vu4)(a > b); }
R3D_INLINE vu4 v_gt_u(vu4 a, vu4 b) { return (vu4)(a > b); }
R3D_INLINE vu4 v_gt_f(vf4 a, vf4 b) { return (vu4)(a > b); }
R3D_INLINE vs8 v_gt_s16(vs8 a, vs8 b) { return (vs8)(a > b); }

R3D_INLINE vu4 v_selu(vu4 a, vu4 b, vu4 m) { return (a & ~m) | (b & m); }
R3D_INLINE vi4 v_seli(vi4 a, vi4 b, vu4 m) { return (vi4)(((vu4)a & ~m) | ((vu4)b & m)); }
R3D_INLINE vf4 v_self(vf4 a, vf4 b, vu4 m) { return (vf4)(((vu4)a & ~m) | ((vu4)b & m)); }

R3D_INLINE vf4 v_recip(vf4 x) { return v_splatf(1.0f) / x; }

R3D_INLINE vu4 v_mul16(vu4 a, vu4 b) { return (a & 0xFFFFu) * (b & 0xFFFFu); }
R3D_INLINE vi4 v_mul32x16(vi4 a, vi4 b) { return (vi4)((vu4)a * ((vu4)b & 0xFFFFu)); }

R3D_INLINE vu4 v_byte0(vu4 v) { return v & 0xFFu; }
R3D_INLINE vu4 v_byte1(vu4 v) { return (v >> 8) & 0xFFu; }
R3D_INLINE vu4 v_byte2(vu4 v) { return (v >> 16) & 0xFFu; }
R3D_INLINE vu4 v_byte3(vu4 v) { return v >> 24; }

R3D_INLINE vu4 v_pack4(vu4 r, vu4 g, vu4 b, vu4 a) { return r | g << 8 | b << 16 | a << 24; }

R3D_INLINE uint32_t v_bits16(vs8 m)
{
	uint32_t b = 0;
	for (int i = 0; i < 8; i++)
		b |= (uint32_t)(m[i] & 1) << (7 - i);
	return b;
}

#endif

/* Comunes */
R3D_INLINE vu4 v_ge0(vi4 a) { return v_gt_s(a, v_splati(-1)); }
R3D_INLINE vi4 v_maxi(vi4 a, vi4 b) { return v_seli(a, b, v_gt_s(b, a)); }
R3D_INLINE vi4 v_mini(vi4 a, vi4 b) { return v_seli(a, b, v_gt_s(a, b)); }
R3D_INLINE vf4 v_maxf(vf4 a, vf4 b) { return v_self(a, b, v_gt_f(b, a)); }
R3D_INLINE vf4 v_minf(vf4 a, vf4 b) { return v_self(a, b, v_gt_f(a, b)); }
R3D_INLINE vu4 v_maxu(vu4 a, vu4 b) { return v_selu(a, b, v_gt_u(b, a)); }

/* Truncamiento hacia cero (cflts/cfltu en el SPU; conversión de C en el host). */
R3D_INLINE vi4 v_cvt_i(vf4 a) { return __builtin_convertvector(a, vi4); }
R3D_INLINE vu4 v_cvt_u(vf4 a) { return __builtin_convertvector(a, vu4); }
R3D_INLINE vf4 v_cvt_f(vi4 a) { return __builtin_convertvector(a, vf4); }

/* Recoge cuatro palabras de 32 bits en `base + off[k]` (bytes, alineados a 4). */
R3D_INLINE vu4 v_gather32(const uint8_t *base, vu4 off)
{
	return (vu4){ *(const uint32_t *)(base + off[0]), *(const uint32_t *)(base + off[1]),
		      *(const uint32_t *)(base + off[2]), *(const uint32_t *)(base + off[3]) };
}

#endif
