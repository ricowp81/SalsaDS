/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * SalsaDS — ejecución de las órdenes del motor de geometría (Fase 3.5). Ver gx_exec.h. Antes era la primera mitad de gx.c
 * (Fase 3.1); el tiempo, la FIFO y GXSTAT siguen en gx.c.
 *
 * Código propio escrito desde GBATEK; las decisiones donde GBATEK no alcanza (y que se tomaron comparando con melonDS
 * ejecutándolo, nunca leyendo su código) están en docs/fase3.md. Lo que no se pudo contrastar en una DS real se marca
 * «[sin contrastar]» en los comentarios.
 */
#include "gpu3d/gx_exec.h"

#include <string.h>
#ifdef GX_PROF
#include <spu_intrinsics.h>
#endif

#if defined(__SIZEOF_INT128__)
typedef __int128 gx_wide;
#else
typedef int64_t gx_wide;
#endif


/* ---- aritmética de 64 bits barata en el SPE ----------------------------------------------------------------------- *
 * El SPU solo multiplica 16x16 y no divide: un int64 * int64 es una llamada a __muldi3 y una división de 64 bits cuesta cientos de
 * ciclos. Aquí van las versiones exactas (mismos bits que la aritmética de 64 bits) que usan solo multiplicaciones de 32 bits, y
 * la versión vectorial (cuatro columnas a la vez) de las matrices y las transformaciones. En el PPE y en el host se queda lo
 * simple (GX_VEC no se define) y las pruebas comparan las dos versiones. */

#if defined(__SPU__) || defined(GX_VEC)
#define GX_VEC 1
#endif

/* a * b sin signo, 32 x 32 -> 64 */
static inline uint64_t mulu32(uint32_t a, uint32_t b)
{
#if defined(__SPU__) || defined(GX_SPLIT_MUL)
	const uint32_t ah = a >> 16, al = a & 0xFFFFu, bh = b >> 16, bl = b & 0xFFFFu;
	const uint32_t hh = ah * bh, m1 = ah * bl, m2 = al * bh, ll = al * bl;
	uint32_t lo = ll + (m1 << 16);
	uint32_t hi = hh + (m1 >> 16) + (lo < ll);
	const uint32_t lo2 = lo + (m2 << 16);

	hi += (m2 >> 16) + (lo2 < lo);
	return ((uint64_t)hi << 32) | lo2;
#else
	return (uint64_t)a * b;
#endif
}

/* a * b con signo, 32 x 32 -> 64 */
static inline int64_t muls32(int32_t a, int32_t b)
{
#if defined(__SPU__) || defined(GX_SPLIT_MUL)
	/* (a + 2^32 * (a < 0)) * (b + 2^32 * (b < 0)) mod 2^64: a producto sin signo menos b * 2^32 si a < 0 y a * 2^32 si b < 0 */
	uint64_t p = mulu32((uint32_t)a, (uint32_t)b);
	uint32_t hi = (uint32_t)(p >> 32);

	hi -= (a < 0 ? (uint32_t)b : 0u) + (b < 0 ? (uint32_t)a : 0u);
	return (int64_t)(((uint64_t)hi << 32) | (uint32_t)p);
#else
	return (int64_t)a * b;
#endif
}

/* a * b de 64 bits (envuelve como la aritmética de C): con operandos de 32 bits, una multiplicación de 32 */
static inline int64_t mul_i64(int64_t a, int64_t b)
{
	if (a == (int32_t)a && b == (int32_t)b)
		return muls32((int32_t)a, (int32_t)b);
	return (int64_t)((uint64_t)a * (uint64_t)b);
}

/* a (64 bits sin signo, < 2^40) * b (32 bits) */
static inline uint64_t mulu64_32(uint64_t a, uint32_t b)
{
	return mulu32((uint32_t)a, b) + (mulu32((uint32_t)(a >> 32), b) << 32);
}

#ifdef GX_VEC
/* Cociente n / d con n >= 0, d > 0, cuando cabe en 22 bits (en las coordenadas de pantalla y la profundidad siempre cabe: la
 * posición está dentro del volumen de vista). Estima en coma flotante y corrige con enteros: el resultado es exacto. 0 = no
 * aplica (quien llama usa la división de 64 bits). */
static inline int udiv_small(uint64_t n, uint64_t d, uint32_t *q)
{
	float fn, fd;
	uint32_t q0;
	int64_t r;

	if ((n >> 22) >= d)
		return 0;
	fn = (float)(uint32_t)(n >> 32) * 4294967296.0f + (float)(uint32_t)n;
	fd = (float)(uint32_t)(d >> 32) * 4294967296.0f + (float)(uint32_t)d;
	q0 = (uint32_t)(fn / fd);
	r = (int64_t)(n - (mulu32(q0, (uint32_t)d) + (mulu32(q0, (uint32_t)(d >> 32)) << 32)));
	while (r < 0) {
		q0--;
		r += (int64_t)d;
	}
	while (r >= (int64_t)d) {
		q0++;
		r -= (int64_t)d;
	}
	*q = q0;
	return 1;
}

/* n / d sin signo con d > 0 cuando el cociente cabe en 32 bits (n >> 32 < d): estimaciones en coma flotante que siempre quedan
 * por debajo y se afinan en dos o tres vueltas; exacto. */
static inline uint64_t udiv_q32(uint64_t n, uint64_t d)
{
	uint64_t q = 0;
	float fd;

	if ((n >> 32) == 0 && (d >> 32) == 0)
		return (uint32_t)n / (uint32_t)d;
	fd = (float)(uint32_t)(d >> 32) * 4294967296.0f + (float)(uint32_t)d;
	while (n >= d) {
		const float fn = (float)(uint32_t)(n >> 32) * 4294967296.0f + (float)(uint32_t)n;
		const float e = fn / fd * 0.99999f; /* por debajo del cociente real (el error relativo de la coma flotante es ~2^-22) */
		uint32_t qe = e >= 4294967040.0f ? 0xFFFFFF00u : (uint32_t)e;

		if (qe == 0)
			qe = 1; /* queda menos de un cociente de margen: el resto se resuelve restando d */
		q += qe;
		n -= mulu32(qe, (uint32_t)d) + (mulu32(qe, (uint32_t)(d >> 32)) << 32);
	}
	return q;
}

#endif

/* n / d truncando hacia cero (d > 0) como la división de C */
static inline int64_t div_trunc(int64_t n, int64_t d)
{
#ifdef GX_VEC
	uint32_t q;

	if (n >= 0) {
		if (udiv_small((uint64_t)n, (uint64_t)d, &q))
			return (int64_t)q;
	} else if (udiv_small((uint64_t)-n, (uint64_t)d, &q)) {
		return -(int64_t)q;
	}
#endif
	return n / d;
}

/* ---- orden -> parámetros y coste (en ciclos del motor, 33,5 MHz) ------------------------------------------------ */

#define NP_BAD GX_NP_BAD

/* Parámetros por orden (255 = no existe), generada de la tabla de GBATEK */
const uint8_t gx_np_tab[0x80] = {
	0, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255,
	1, 0, 1, 1, 1, 0, 16, 12, 16, 12, 9, 3, 3, 255, 255, 255,
	1, 1, 1, 2, 1, 1, 1, 1, 1, 1, 1, 1, 255, 255, 255, 255,
	1, 1, 1, 1, 32, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255,
	1, 0, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255,
	1, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255,
	1, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255,
	3, 2, 1, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255,
};

/* Coste base en ciclos del motor (33,5 MHz, tabla de GBATEK); MULT en el modo 2 suma 30 y NORMAL suma una por luz encendida. */
const uint16_t gx_cost_tab[0x80] = {
	0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
	1, 17, 36, 17, 36, 19, 34, 30, 35, 31, 28, 22, 22, 0, 0, 0,
	1, 9, 1, 9, 8, 8, 8, 8, 8, 1, 1, 1, 0, 0, 0, 0,
	4, 4, 6, 1, 32, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
	1, 1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
	392, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
	1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
	103, 9, 5, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
};

/* ---- matrices ----------------------------------------------------------------------------------------------- */

static void mat_ident(gx_mat *m)
{
	memset(m, 0, sizeof *m);
	m->m[0] = m->m[5] = m->m[10] = m->m[15] = 0x1000;
}

#ifdef GX_VEC
typedef int32_t vi4 __attribute__((vector_size(16)));
typedef uint32_t vu4 __attribute__((vector_size(16)));
typedef vi4 vi4_a __attribute__((may_alias));

typedef struct v64 {
	vu4 hi; /* sin signo: las sumas envuelven módulo 2^64 como las de C con enteros de 64 bits */
	vu4 lo;
} v64;

static inline vi4 vsplat(int32_t x)
{
	return (vi4){ x, x, x, x };
}

static inline vi4 vload(const int32_t *p)
{
	return *(const vi4_a *)p;
}

/* producto con signo de cada carril, 32 x 32 -> 64 (hi, lo): cuatro multiplicaciones de 16 bits por carril */
static inline v64 v_mul(vi4 a, vi4 b)
{
	const vi4 ah = a >> 16, bh = b >> 16;
	const vu4 al = (vu4)a & 0xFFFFu, bl = (vu4)b & 0xFFFFu;
	const vi4 m1 = ah * (vi4)bl, m2 = (vi4)al * bh;
	vu4 lo = al * bl, t = (vu4)m1 << 16;
	vu4 hi = (vu4)(ah * bh + (m1 >> 16) + (m2 >> 16));

	lo += t;
	hi -= (vu4)(lo < t);
	t = (vu4)m2 << 16;
	lo += t;
	hi -= (vu4)(lo < t);
	return (v64){ hi, lo };
}

static inline v64 v_add(v64 a, v64 b)
{
	const vu4 lo = a.lo + b.lo;

	return (v64){ a.hi + b.hi - (vu4)(lo < a.lo), lo };
}

/* (suma >> 12) truncada a 32 bits */
static inline vi4 v_shr12(v64 a)
{
	return (vi4)((a.lo >> 12) | (a.hi << 20));
}

/* fila 3 por 4096 como entero de 64 bits */
static inline v64 v_row_x4096(vi4 r)
{
	return (v64){ (vu4)(r >> 20), (vu4)r << 12 };
}
#endif

/* out = a x b (a por la izquierda: v' = v x a x b con v fila). Cada elemento suma cuatro productos en 64 bits y trunca
 * con >> 12 (aritmético); el resultado se guarda en 32 bits. */
static void mat_mul(gx_mat *out, const gx_mat *a, const gx_mat *b)
{
	gx_mat r;

#ifdef GX_VEC
	const vi4 b0 = vload(&b->m[0]), b1 = vload(&b->m[4]), b2 = vload(&b->m[8]), b3 = vload(&b->m[12]);

	for (int i = 0; i < 4; i++) {
		v64 s = v_mul(vsplat(a->m[i * 4]), b0);

		s = v_add(s, v_mul(vsplat(a->m[i * 4 + 1]), b1));
		s = v_add(s, v_mul(vsplat(a->m[i * 4 + 2]), b2));
		s = v_add(s, v_mul(vsplat(a->m[i * 4 + 3]), b3));
		*(vi4_a *)&r.m[i * 4] = v_shr12(s);
	}
#else
	for (int i = 0; i < 4; i++)
		for (int j = 0; j < 4; j++) {
			int64_t s = 0;

			for (int k = 0; k < 4; k++)
				s += (int64_t)a->m[i * 4 + k] * b->m[k * 4 + j];
			r.m[i * 4 + j] = (int32_t)(s >> 12);
		}
#endif
	*out = r;
}

/* (x, y, z, 1) x m, cada componente en 64 bits y >> 12 (truncado a 32 bits): matriz clip por un vértice */
static void xform_point(const int32_t *m, int32_t x, int32_t y, int32_t z, int32_t out[4])
{
#ifdef GX_VEC
	v64 s = v_mul(vsplat(x), vload(&m[0]));
	int32_t t[4] __attribute__((aligned(16)));

	s = v_add(s, v_mul(vsplat(y), vload(&m[4])));
	s = v_add(s, v_mul(vsplat(z), vload(&m[8])));
	s = v_add(s, v_row_x4096(vload(&m[12])));
	*(vi4_a *)t = v_shr12(s);
	out[0] = t[0]; out[1] = t[1]; out[2] = t[2]; out[3] = t[3];
#else
	const int64_t px = x, py = y, pz = z;

	out[0] = (int32_t)((px * m[0] + py * m[4] + pz * m[8] + ((int64_t)m[12] * 4096)) >> 12);
	out[1] = (int32_t)((px * m[1] + py * m[5] + pz * m[9] + ((int64_t)m[13] * 4096)) >> 12);
	out[2] = (int32_t)((px * m[2] + py * m[6] + pz * m[10] + ((int64_t)m[14] * 4096)) >> 12);
	out[3] = (int32_t)((px * m[3] + py * m[7] + pz * m[11] + ((int64_t)m[15] * 4096)) >> 12);
#endif
}

/* normal (1.0.9) por la matriz vectorial 3x3 (>> 12), saturada a 1.0.9. La suma cabe en 32 bits después del desplazamiento
 * (|n| <= 512, 3 productos): se trunca sin perder nada. */
static void xform_normal(const int32_t *m, const int32_t n[3], int32_t t[3])
{
#ifdef GX_VEC
	v64 s = v_mul(vsplat(n[0]), vload(&m[0]));
	int32_t r[4] __attribute__((aligned(16)));

	s = v_add(s, v_mul(vsplat(n[1]), vload(&m[4])));
	s = v_add(s, v_mul(vsplat(n[2]), vload(&m[8])));
	*(vi4_a *)r = v_shr12(s);
	for (int i = 0; i < 3; i++)
		t[i] = r[i] < -512 ? -512 : r[i] > 511 ? 511 : r[i];
#else
	for (int i = 0; i < 3; i++) {
		const int64_t v = ((int64_t)n[0] * m[i] + (int64_t)n[1] * m[4 + i] + (int64_t)n[2] * m[8 + i]) >> 12;

		t[i] = (int32_t)(v < -512 ? -512 : v > 511 ? 511 : v);
	}
#endif
}

static void clip_update(gx_exec *g)
{
	if (g->clip_dirty) {
		mat_mul(&g->clip, &g->pos, &g->proj);
		g->clip_dirty = 0;
	}
}

static void mtx_load(gx_exec *g, const gx_mat *m)
{
	switch (g->sp.mode) {
	case 0:
		g->proj = *m;
		g->clip_dirty = 1;
		break;
	case 1:
		g->pos = *m;
		g->clip_dirty = 1;
		break;
	case 2:
		g->pos = *m;
		g->vec = *m;
		g->clip_dirty = 1;
		break;
	default:
		g->tex = *m;
		break;
	}
}

/* Multiplica el operando por la izquierda de la matriz actual. MTX_SCALE no toca la vectorial en el modo 2. */
static void mtx_mult(gx_exec *g, const gx_mat *op, int is_scale)
{
	switch (g->sp.mode) {
	case 0:
		mat_mul(&g->proj, op, &g->proj);
		g->clip_dirty = 1;
		break;
	case 1:
		mat_mul(&g->pos, op, &g->pos);
		g->clip_dirty = 1;
		break;
	case 2:
		mat_mul(&g->pos, op, &g->pos);
		if (!is_scale)
			mat_mul(&g->vec, op, &g->vec);
		g->clip_dirty = 1;
		break;
	default:
		mat_mul(&g->tex, op, &g->tex);
		break;
	}
}

/* Operando de MTX_LOAD/MULT desde los parámetros: 4x4 (16), 4x3 (12: 4 filas de 3, última columna 0,0,0,1) o 3x3 (9:
 * 3 filas de 3; la fila de traslación es 0,0,0 y 1). */
static void mat_from_params(gx_mat *m, const uint32_t *p, int kind)
{
	mat_ident(m);
	switch (kind) {
	case 44:
		for (int i = 0; i < 16; i++)
			m->m[i] = (int32_t)p[i];
		break;
	case 43:
		for (int r = 0; r < 4; r++)
			for (int c = 0; c < 3; c++)
				m->m[r * 4 + c] = (int32_t)p[r * 3 + c];
		break;
	default:
		for (int r = 0; r < 3; r++)
			for (int c = 0; c < 3; c++)
				m->m[r * 4 + c] = (int32_t)p[r * 3 + c];
		break;
	}
}

static int32_t sext(uint32_t v, unsigned bits)
{
	const unsigned sh = 32u - bits;

	return (int32_t)(v << sh) >> sh;
}

/* ---- recorte ------------------------------------------------------------------------------------------------- */

/* Distancia con signo de un vértice a cada uno de los seis planos del volumen de vista; dentro si >= 0. */
static int64_t plane_dist(const gx_cvtx *v, int plane)
{
	switch (plane) {
	case 0: return (int64_t)v->z + v->w; /* cercano */
	case 1: return (int64_t)v->w - v->z; /* lejano */
	case 2: return (int64_t)v->x + v->w; /* izquierdo */
	case 3: return (int64_t)v->w - v->x; /* derecho */
	case 4: return (int64_t)v->y + v->w; /* inferior */
	default: return (int64_t)v->w - v->y; /* superior */
	}
}

static unsigned outcode(const gx_cvtx *v)
{
	unsigned c = 0;

	for (int k = 0; k < 6; k++)
		if (plane_dist(v, k) < 0)
			c |= 1u << k;
	return c;
}

static int32_t lerp_field(int32_t a, int32_t b, int64_t da, int64_t db)
{
	const int64_t x = (int64_t)b - a, d = da - db;
	const uint64_t ax = (uint64_t)(x < 0 ? -x : x);

	/* a + (b - a) * da / (da - db) con la división truncada hacia cero. Con los factores de 32 bits (lo normal) el producto cabe
	 * en 64 bits y el cociente en 32: sin la división de 128 bits (en el SPE; el PPE divide de 64 bits en una instrucción) */
#ifdef GX_VEC
	if (da >= 0 && d > 0 && (ax >> 32) == 0 && ((uint64_t)da >> 32) == 0) {
		const uint64_t prod = mulu32((uint32_t)ax, (uint32_t)da);

		if ((prod >> 32) < (uint64_t)d) {
			const int64_t q = (int64_t)udiv_q32(prod, (uint64_t)d);

			return (int32_t)(a + (x < 0 ? -q : q));
		}
	}
#else
	(void)ax;
#endif
	return (int32_t)(a + (gx_wide)x * da / d);
}

static void clip_edge(gx_cvtx *o, const gx_cvtx *a, const gx_cvtx *b, int64_t da, int64_t db)
{
	o->x = lerp_field(a->x, b->x, da, db);
	o->y = lerp_field(a->y, b->y, da, db);
	o->z = lerp_field(a->z, b->z, da, db);
	o->w = lerp_field(a->w, b->w, da, db);
	o->s = lerp_field(a->s, b->s, da, db);
	o->t = lerp_field(a->t, b->t, da, db);
	o->r = lerp_field(a->r, b->r, da, db);
	o->g = lerp_field(a->g, b->g, da, db);
	o->b = lerp_field(a->b, b->b, da, db);
}

/* Sutherland-Hodgman contra los planos de `mask` (bit k = plano k). `in` y `out` de al menos GX_POLY_MAXV + 2
 * elementos; devuelve el número de vértices (puede ser 0) y deja el resultado en `out`. */
#define CLIP_BUF (GX_POLY_MAXV + 4u)
static int clip_polygon(const gx_cvtx *in, int n, gx_cvtx *out, unsigned mask)
{
	gx_cvtx buf[2][CLIP_BUF];
	int cur = 0;

	memcpy(buf[0], in, (size_t)n * sizeof *in);
	for (int k = 0; k < 6 && n > 0; k++) {
		int on = 0;
		const gx_cvtx *a = buf[cur];
		gx_cvtx *o = buf[cur ^ 1];

		if (!(mask & (1u << k)))
			continue;
		for (int i = 0; i < n; i++) {
			const gx_cvtx *p = &a[i], *q = &a[(i + 1) % n];
			const int64_t dp = plane_dist(p, k), dq = plane_dist(q, k);

			if (dp >= 0)
				o[on++] = *p;
			/* El vértice nuevo se interpola siempre desde el lado de adentro hacia el de afuera, sin importar en qué sentido
			 * recorra la arista este polígono: así dos polígonos que comparten una arista recortada obtienen el mismo vértice
			 * (con el sentido del recorrido salían uno o dos píxeles distintos y quedaban juntas entre ellos). */
			if ((dp >= 0) != (dq >= 0) && on < (int)CLIP_BUF) {
				gx_cvtx *n = &o[on++];

				if (dp >= 0)
					clip_edge(n, p, q, dp, dq);
				else
					clip_edge(n, q, p, dq, dp);
				/* y queda exactamente sobre el plano (la interpolación entera lo deja a veces una unidad dentro o fuera, y
				 * el viewport lo mandaba a un píxel de más o de menos: el borde de la pantalla sin dibujar) */
				switch (k) {
				case 0: n->z = -n->w; break;
				case 1: n->z = n->w; break;
				case 2: n->x = -n->w; break;
				case 3: n->x = n->w; break;
				case 4: n->y = -n->w; break;
				default: n->y = n->w; break;
				}
			}
		}
		cur ^= 1;
		n = on;
	}
	memcpy(out, buf[cur], (size_t)n * sizeof(gx_cvtx));
	return n;
}

/* ---- viewport y pantalla -------------------------------------------------------------------------------------- */

/* (a * b) con b en 1..256 y |a| < 2^40, como entero de 64 bits con signo */
static inline int64_t mul_small(int64_t a, int64_t b)
{
	return a >= 0 ? (int64_t)mulu64_32((uint64_t)a, (uint32_t)b) : -(int64_t)mulu64_32((uint64_t)-a, (uint32_t)b);
}

/* Pantalla de un vértice, a la vez con 8 bits fraccionarios extra (sx8, sy8: para decidir la cara con más precisión) y sin ellos
 * (sx0, sy0: lo que se guarda). Con w <= 0 (detrás del ojo, sólo si el polígono no se recortó) devuelve el origen del viewport.
 * Una sola división por coordenada: la de 8 bits fraccionarios, y la entera sale de ella (truncar el cociente truncado da el
 * cociente truncado del cociente: igual que dividir sin los 8 bits). */
static void screen_both(const gx_exec *g, const gx_cvtx *v, int64_t *sx0, int64_t *sy0, int64_t *sx8, int64_t *sy8)
{
	const int64_t x1 = g->viewport & 0xFF, y1 = (g->viewport >> 8) & 0xFF;
	const int64_t x2 = (g->viewport >> 16) & 0xFF, y2 = (g->viewport >> 24) & 0xFF;
	const int64_t vw = x2 - x1 + 1, vh = y2 - y1 + 1; /* anchura y altura (pueden ser <= 0 si x2 < x1; entonces 0) */
	const int64_t w = v->w;
	int64_t tx, ty;

	if (w <= 0 || vw <= 0 || vh <= 0) {
		*sx0 = x1;
		*sy0 = 191 - y2;
		*sx8 = x1 * 256;
		*sy8 = (191 - y2) * 256;
		return;
	}
	/* y se mide desde abajo (y = 191 - y); la y de recorte positiva va hacia arriba */
	tx = div_trunc(mul_small((int64_t)v->x + w, vw) * 256, 2 * w);
	ty = div_trunc(mul_small(w - (int64_t)v->y, vh) * 256, 2 * w);
	*sx8 = tx + x1 * 256;
	*sy8 = ty + (191 - y2) * 256;
	*sx0 = (tx >= 0 ? tx >> 8 : -((-tx) >> 8)) + x1;
	*sy0 = (ty >= 0 ? ty >> 8 : -((-ty) >> 8)) + (191 - y2);
}

static int16_t clamp16(int64_t v)
{
	return (int16_t)(v < -32768 ? -32768 : v > 32767 ? 32767 : v);
}

static void make_out(const gx_cvtx *v, gx_vertex *o, int64_t sx, int64_t sy)
{
	int64_t z;

	o->sx = clamp16(sx);
	o->sy = clamp16(sy);
	if (v->w > 0) {
		z = div_trunc((int64_t)v->z * 0x4000, v->w) + 0x3FFF;
		z = z < 0 ? 0 : z > 0x7FFF ? 0x7FFF : z;
	} else {
		z = 0;
	}
	o->z = (uint32_t)z << 9;
	o->w = v->w;
	o->s = (int16_t)v->s;
	o->t = (int16_t)v->t;
	o->r = (uint8_t)v->r;
	o->g = (uint8_t)v->g;
	o->b = (uint8_t)v->b;
	o->pad = 0;
	o->rsv[0] = o->rsv[1] = 0;
}

/* ---- ensamblado y emisión de polígonos ------------------------------------------------------------------------ */

/* Recorta, descarta por cara y guarda un polígono de `n` vértices. Cada polígono lleva sus vértices propios en la RAM de
 * vértices (sin compartir los de las tiras): 6144 = 2048 triángulos x 3; sólo cuentan los polígonos que llegan a la
 * lista (los descartados por cara o por recorte no gastan RAM). */
static void emit_poly_inner(gx_exec *g, gx_cvtx *const *src, int n);
static void emit_poly(gx_exec *g, gx_cvtx *const *src, int n)
{
#ifdef GX_PROF
	const uint32_t t0 = spu_readch(SPU_RdDec);

	emit_poly_inner(g, src, n);
	g->prof[4] += t0 - spu_readch(SPU_RdDec);
#else
	emit_poly_inner(g, src, n);
#endif
}

static void emit_poly_inner(gx_exec *g, gx_cvtx *const *src, int n)
{
	gx_cvtx pv[CLIP_BUF];
	const uint32_t attr = g->polyattr_cur;
	unsigned oc_and = 0x3Fu, oc_or = 0;
	int m = n, clipped = 0, front;
	int64_t area2 = 0;
	gx_poly *p;
	uint32_t wmax = 0;
	int16_t xmin = 32767, xmax = -32768, ymin = 32767, ymax = -32768;

	gx_cvtx in[4];
	int64_t s0x[CLIP_BUF], s0y[CLIP_BUF], s8x[CLIP_BUF], s8y[CLIP_BUF];

	for (int i = 0; i < n; i++) {
		unsigned c;

		in[i] = *src[i];
		c = outcode(&in[i]);
		oc_and &= c;
		oc_or |= c;
	}
	if (oc_and) { /* entero fuera de algún plano */
		g->out.st.rejected++;
		g->out.st.rej_outside++;
		return;
	}
	/* El plano lejano: si el polígono lo cruza y POLYGON_ATTR bit 12 es 0, no se dibuja; con 1 se recorta. [sin contrastar] */
	if ((oc_or & 2u) && !(attr & 0x1000u)) {
		g->out.st.rejected++;
		g->out.st.rej_far++;
		return;
	}
	if (oc_or) {
		m = clip_polygon(in, n, pv, 0x3Fu);
		if (m < 3) {
			g->out.st.rejected++;
			g->out.st.rej_empty++;
			return;
		}
		clipped = 1;
	} else {
		memcpy(pv, in, (size_t)n * sizeof in[0]);
	}
	/* Cara: signo del área en pantalla (con 8 bits fraccionarios extra). Frontal = antihorario a la vista = área < 0
	 * con y hacia abajo. Área 0 cuenta como frontal. [sin contrastar] */
	for (int i = 0; i < m; i++)
		screen_both(g, &pv[i], &s0x[i], &s0y[i], &s8x[i], &s8y[i]);
	for (int i = 0; i < m; i++) {
		const int j = i + 1 == m ? 0 : i + 1;

		area2 += mul_i64(s8x[i], s8y[j]) - mul_i64(s8x[j], s8y[i]);
	}
	front = area2 <= 0;
	if (front ? !(attr & 0x80u) : !(attr & 0x40u)) {
		g->out.st.culled++;
		return;
	}
	if (g->out.npolys >= GX_MAX_POLYS) {
		g->out.dropped_polys++;
		g->out.st.dropped_poly++;
		return;
	}
	{
		uint32_t nnew = 0;

		for (int i = 0; i < m; i++)
			nnew += clipped || src[i]->ram < 0;
		if (g->out.nverts + nnew > GX_MAX_VERTS) {
			g->out.dropped_verts += nnew;
			g->out.st.dropped_vert += nnew;
			return;
		}
		/* sitio en las ventanas de salida: si no cabe lo que viene (un polígono y hasta 10 vértices) se vacían */
		if (g->flush && (g->out.nverts + nnew - g->vbase > g->vcap || g->out.npolys + 1u - g->pbase > g->pcap))
			g->flush(g, g->flush_ctx);
	}
	p = &g->poly[g->out.npolys++ - g->pbase];
	memset(p, 0, sizeof *p);
	p->attr = attr;
	p->texparam = g->texparam;
	p->pltt = g->pltt & 0x1FFFu;
	p->nverts = (uint8_t)m;
	p->flags = (uint8_t)((front ? GX_POLY_FRONT : 0) | (clipped ? GX_POLY_CLIPPED : 0));
	{
		const unsigned alpha = (attr >> 16) & 0x1Fu;

		if (alpha == 0)
			p->flags |= GX_POLY_WIRE;
		else if (alpha < 31)
			p->flags |= GX_POLY_TRANSLUCENT;
	}
	for (int i = 0; i < m; i++) {
		uint32_t id;
		gx_vertex *o;
		int16_t sx, sy;

		/* un polígono sin recortar reutiliza los vértices que ya guardó una tira; el recortado trae vértices nuevos */
		if (!clipped && src[i]->ram >= 0) {
			id = (uint32_t)src[i]->ram;
			sx = src[i]->sx; /* el vértice guardado puede estar ya en memoria principal: se usan los datos que se guardaron */
			sy = src[i]->sy;
		} else {
			id = g->out.nverts++;
			o = &g->vtx[id - g->vbase];
			make_out(&pv[i], o, s0x[i], s0y[i]);
			sx = o->sx;
			sy = o->sy;
			if (!clipped) {
				src[i]->ram = (int32_t)id;
				src[i]->sx = o->sx;
				src[i]->sy = o->sy;
			}
		}
		p->vidx[i] = (uint16_t)id;
		if (sx < xmin) xmin = sx;
		if (sx > xmax) xmax = sx;
		if (sy < ymin) ymin = sy;
		if (sy > ymax) ymax = sy;
		if (pv[i].w > 0 && (uint32_t)pv[i].w > wmax)
			wmax = (uint32_t)pv[i].w;
	}
	p->xmin = xmin; p->xmax = xmax; p->ymin = ymin; p->ymax = ymax;
	/* w se normaliza a 16 bits desplazando de 4 en 4. [sin contrastar] */
	while ((wmax >> p->wshift) > 0xFFFFu)
		p->wshift = (uint8_t)(p->wshift + 4u);
	g->out.st.polys++;
	if (clipped)
		g->out.st.clipped++;
}

/* `c` es el vértice nuevo (local de quien llama); las tiras guardan una copia en la ventana `asm_v` y la comparten con el
 * siguiente polígono, de modo que el índice de RAM que se le dio al guardarla se hereda. */
static void asm_push(gx_exec *g, gx_cvtx *c)
{
	gx_cvtx *a = g->asm_v;
	gx_cvtx *t[4];

	switch (g->prim & 3u) {
	case 0: /* triángulos */
		a[g->asm_n++] = *c;
		if (g->asm_n == 3) {
			t[0] = &a[0]; t[1] = &a[1]; t[2] = &a[2];
			emit_poly(g, t, 3);
			g->asm_n = 0;
		}
		break;
	case 1: /* cuadriláteros */
		a[g->asm_n++] = *c;
		if (g->asm_n == 4) {
			t[0] = &a[0]; t[1] = &a[1]; t[2] = &a[2]; t[3] = &a[3];
			emit_poly(g, t, 4);
			g->asm_n = 0;
		}
		break;
	case 2: /* tira de triángulos: el orden se invierte en los impares para conservar la cara */
		if (g->asm_n < 2) {
			a[g->asm_n++] = *c;
			break;
		}
		if (!g->asm_par) {
			t[0] = &a[0]; t[1] = &a[1];
		} else {
			t[0] = &a[1]; t[1] = &a[0];
		}
		t[2] = c;
		emit_poly(g, t, 3);
		g->asm_par ^= 1u;
		a[0] = a[1];
		a[1] = *c;
		break;
	default: /* tira de cuadriláteros: (a, b, d, c) */
		if (g->asm_n < 3) {
			a[g->asm_n++] = *c;
			break;
		}
		t[0] = &a[0]; t[1] = &a[1]; t[2] = c; t[3] = &a[2];
		emit_poly(g, t, 4);
		a[0] = a[2];
		a[1] = *c;
		g->asm_n = 2;
		break;
	}
}

/* ---- vértices ---------------------------------------------------------------------------------------------- */

static int32_t clamp_s(int64_t v, int32_t lo, int32_t hi)
{
	return (int32_t)(v < lo ? lo : v > hi ? hi : v);
}

/* Coordenadas de textura (TEXIMAGE_PARAM bits 30-31). `tex_s/t` son las de TEXCOORD sin transformar y `cur_s/t` las que
 * llevará el próximo vértice. Modo 0: las crudas. Modo 1: TEXCOORD las transforma por la matriz de textura al llegar.
 * Modo 2: NORMAL las calcula (crudas + normal x matriz); un TEXCOORD posterior las pisa con las crudas. Modo 3: cada
 * vértice las calcula (crudas + vértice x matriz). Orden de los efectos medido contra melonDS (docs/fase3.md, 3.3).
 * Unidades: 1.11.4. [Modos 1 y 3 en la traslación: ver docs/fase3.md] */
static void tex_mode1(gx_exec *g)
{
	const int32_t *m = g->tex.m;
	const int64_t s = g->tex_s, t = g->tex_t;

	g->cur_s = (int16_t)((s * m[0] + t * m[4] + (int64_t)m[8] + m[12]) >> 12);
	g->cur_t = (int16_t)((s * m[1] + t * m[5] + (int64_t)m[9] + m[13]) >> 12);
}

static void tex_mode2(gx_exec *g)
{
	const int32_t *m = g->tex.m;
	const int64_t nx = g->normal[0], ny = g->normal[1], nz = g->normal[2];

	g->cur_s = (int16_t)(g->tex_s + ((nx * m[0] + ny * m[4] + nz * m[8]) >> 21));
	g->cur_t = (int16_t)(g->tex_t + ((nx * m[1] + ny * m[5] + nz * m[9]) >> 21));
}

static void tex_mode3(gx_exec *g)
{
	const int32_t *m = g->tex.m;
	const int64_t vx = g->vx, vy = g->vy, vz = g->vz;

	g->cur_s = (int16_t)(g->tex_s + ((vx * m[0] + vy * m[4] + vz * m[8]) >> 24));
	g->cur_t = (int16_t)(g->tex_t + ((vx * m[1] + vy * m[5] + vz * m[9]) >> 24));
}

static uint32_t expand6(uint32_t c5)
{
	return (c5 << 1) + ((c5 + 31u) >> 5);
}

static void vertex_submit(gx_exec *g, int32_t x, int32_t y, int32_t z)
{
	gx_cvtx c;

	g->vx = x; g->vy = y; g->vz = z;
	clip_update(g);
	{
		int32_t r[4];

		xform_point(g->clip.m, x, y, z, r);
		c.x = r[0];
		c.y = r[1];
		c.z = r[2];
		c.w = r[3];
	}
	if ((g->texparam >> 30) == 3)
		tex_mode3(g);
	c.s = g->cur_s;
	c.t = g->cur_t;
	c.r = (int32_t)expand6(g->color5[0]);
	c.g = (int32_t)expand6(g->color5[1]);
	c.b = (int32_t)expand6(g->color5[2]);
	c.ram = -1;
	c.sx = c.sy = 0;
	asm_push(g, &c);
}

/* ---- iluminación ---------------------------------------------------------------------------------------------- */

/* Calcula el color del vértice (5 bits) con las luces habilitadas en POLYGON_ATTR bits 0-3 y la normal ya
 * transformada `n` (1.0.9). Niveles de 8 bits; productos (a * b * nivel) >> 13 y ambiente (a * b) >> 5; suma con
 * saturación a 31. [sin contrastar con una DS real: los anchos son la mejor lectura de GBATEK] */
static void light_color(gx_exec *g, const int32_t n[3])
{
	uint32_t acc[3] = { g->emi[0], g->emi[1], g->emi[2] };
	const unsigned en = g->polyattr & 0xFu;

	for (unsigned i = 0; i < 4; i++) {
		const int32_t *l = g->light_vec[i];
		int32_t dot, ld, sd;
		int32_t hx, hy, hz;

		if (!(en & (1u << i)))
			continue;
		dot = -(l[0] * n[0] + l[1] * n[1] + l[2] * n[2]);
		ld = dot <= 0 ? 0 : dot >> 10;
		if (ld > 255)
			ld = 255;
		/* semivector: (luz + visual) / 2, con la visual (0, 0, -1) */
		hx = l[0] >> 1;
		hy = l[1] >> 1;
		hz = (l[2] - 0x200) >> 1;
		dot = -(hx * n[0] + hy * n[1] + hz * n[2]);
		if (dot <= 0) {
			sd = 0;
		} else {
			sd = dot >> 10;
			if (sd > 255)
				sd = 255;
			sd = ((sd * sd) >> 7) - 0x100;
			if (sd < 0)
				sd = 0;
			if (sd > 255)
				sd = 255;
		}
		if (g->shin_en)
			sd = g->shin[sd >> 1];
		for (int c = 0; c < 3; c++) {
			const uint32_t lc = g->light_col[i][c];

			acc[c] += (g->dif[c] * lc * (uint32_t)ld) >> 13;
			acc[c] += (g->spe[c] * lc * (uint32_t)sd) >> 13;
			acc[c] += (g->amb[c] * lc) >> 5;
		}
	}
	for (int c = 0; c < 3; c++)
		g->color5[c] = acc[c] > 31u ? 31u : acc[c];
}

/* ---- órdenes de prueba ------------------------------------------------------------------------------------- */

static void clip_vertex_of(const gx_exec *g, int32_t x, int32_t y, int32_t z, gx_cvtx *c)
{
	int32_t r[4];

	memset(c, 0, sizeof *c);
	xform_point(g->clip.m, x, y, z, r);
	c->x = r[0];
	c->y = r[1];
	c->z = r[2];
	c->w = r[3];
}

static int box_visible(gx_exec *g, const uint32_t *p)
{
	static const uint8_t face[6][4] = {
		{ 0, 1, 3, 2 }, { 4, 5, 7, 6 }, { 0, 1, 5, 4 }, { 2, 3, 7, 6 }, { 0, 2, 6, 4 }, { 1, 3, 7, 5 },
	};
	const int32_t x0 = (int16_t)p[0], y0 = (int16_t)(p[0] >> 16), z0 = (int16_t)p[1];
	const int32_t dw = (int16_t)(p[1] >> 16), dh = (int16_t)p[2], dd = (int16_t)(p[2] >> 16);
	gx_cvtx c[8];

	clip_update(g);
	for (int i = 0; i < 8; i++)
		clip_vertex_of(g, x0 + ((i & 1) ? dw : 0), y0 + ((i & 2) ? dh : 0), z0 + ((i & 4) ? dd : 0), &c[i]);
	for (int f = 0; f < 6; f++) {
		gx_cvtx q[4], o[CLIP_BUF];
		unsigned oa = 0x3Fu, oo = 0;

		for (int i = 0; i < 4; i++) {
			q[i] = c[face[f][i]];
			{
				const unsigned oc = outcode(&q[i]);

				oa &= oc;
				oo |= oc;
			}
		}
		if (oa)
			continue;
		if (!oo)
			return 1;
		if (clip_polygon(q, 4, o, 0x3Fu) > 0)
			return 1;
	}
	return 0;
}

/* ---- ejecución de órdenes ---------------------------------------------------------------------------------- */

/* Pilas. El puntero sigue contando aunque haya error (como en melonDS: tras el desbordamiento de la pila de posición
 * GXSTAT dice nivel 0 y el de la proyección vale 0 con dos PUSH); el error activa GXSTAT bit 15 y la orden no toca
 * las matrices. [melonDS; sin contrastar en una DS real] */
int gx_sp_push(gx_sp *s)
{
	int ok;

	switch (s->mode) {
	case 0:
		ok = s->sp_proj == 0;
		s->sp_proj = (s->sp_proj + 1) & 3u;
		break;
	case 1:
	case 2:
		ok = s->sp_pos <= GX_STACK_POS - 1;
		s->sp_pos = (s->sp_pos + 1) & 0x3Fu;
		break;
	default:
		ok = s->sp_tex == 0;
		s->sp_tex = (s->sp_tex + 1) & 3u;
		break;
	}
	return ok;
}

int gx_sp_pop(gx_sp *s, uint32_t param, int32_t *idx)
{
	const int32_t n = sext(param, 6);

	switch (s->mode) {
	case 0: {
		const int32_t ns = (int32_t)s->sp_proj - n;

		s->sp_proj = (uint32_t)ns & 3u;
		*idx = 0;
		return ns == 0;
	}
	case 1:
	case 2: {
		const int32_t ns = (int32_t)s->sp_pos - n;

		s->sp_pos = (uint32_t)ns & 0x3Fu;
		*idx = ns;
		return ns >= 0 && ns < (int32_t)GX_STACK_POS;
	}
	default: {
		const int32_t ns = (int32_t)s->sp_tex - n;

		s->sp_tex = (uint32_t)ns & 3u;
		*idx = 0;
		return ns == 0;
	}
	}
}

int gx_sp_store(const gx_sp *s, uint32_t param, uint32_t *idx)
{
	*idx = param & 0x1Fu;
	return !(s->mode == 1 || s->mode == 2) || *idx < GX_STACK_POS;
}

int gx_sp_restore(const gx_sp *s, uint32_t param, uint32_t *idx)
{
	return gx_sp_store(s, param, idx);
}

static void mtx_push(gx_exec *g)
{
	const uint32_t idx = g->sp.sp_pos;
	const unsigned mode = g->sp.mode;

	if (!gx_sp_push(&g->sp))
		return;
	switch (mode) {
	case 0:
		g->stk_proj = g->proj;
		break;
	case 1:
	case 2:
		g->stk_pos[idx] = g->pos;
		g->stk_vec[idx] = g->vec;
		break;
	default:
		g->stk_tex = g->tex;
		break;
	}
}

static void mtx_pop(gx_exec *g, uint32_t param)
{
	const unsigned mode = g->sp.mode;
	int32_t idx;

	if (!gx_sp_pop(&g->sp, param, &idx))
		return;
	switch (mode) {
	case 0:
		g->proj = g->stk_proj;
		g->clip_dirty = 1;
		break;
	case 1:
	case 2:
		g->pos = g->stk_pos[idx];
		g->vec = g->stk_vec[idx];
		g->clip_dirty = 1;
		break;
	default:
		g->tex = g->stk_tex;
		break;
	}
}

static void mtx_store(gx_exec *g, uint32_t param)
{
	uint32_t idx;

	if (!gx_sp_store(&g->sp, param, &idx))
		return;
	switch (g->sp.mode) {
	case 0:
		g->stk_proj = g->proj;
		break;
	case 1:
	case 2:
		g->stk_pos[idx] = g->pos;
		g->stk_vec[idx] = g->vec;
		break;
	default:
		g->stk_tex = g->tex;
		break;
	}
}

static void mtx_restore(gx_exec *g, uint32_t param)
{
	uint32_t idx;

	if (!gx_sp_restore(&g->sp, param, &idx))
		return;
	switch (g->sp.mode) {
	case 0:
		g->proj = g->stk_proj;
		g->clip_dirty = 1;
		break;
	case 1:
	case 2:
		g->pos = g->stk_pos[idx];
		g->vec = g->stk_vec[idx];
		g->clip_dirty = 1;
		break;
	default:
		g->tex = g->stk_tex;
		break;
	}
}

static void do_normal(gx_exec *g, uint32_t p)
{
	int32_t n[3], t[3];

	n[0] = sext(p, 10);
	n[1] = sext(p >> 10, 10);
	n[2] = sext(p >> 20, 10);
	g->normal[0] = n[0]; g->normal[1] = n[1]; g->normal[2] = n[2];
	if ((g->texparam >> 30) == 2)
		tex_mode2(g);
	/* se transforma con la matriz vectorial (3x3) y se satura a 1.0.9 */
	xform_normal(g->vec.m, n, t);
	light_color(g, t);
}

#ifdef GX_PROF
#define PROF_T() spu_readch(SPU_RdDec)
#else
#define PROF_T() 0u
#endif

static void gx_exec_cmd_inner(gx_exec *g, uint32_t cmd, const uint32_t *p);

/* Ejecuta una orden con sus parámetros. El coste en ciclos lo da gx_cmd_cost. */
void gx_exec_cmd(gx_exec *g, uint32_t cmd, const uint32_t *p)
{
#ifdef GX_PROF
	const uint32_t t0 = PROF_T();
	const unsigned cls = (cmd >= 0x10 && cmd <= 0x1C) ? 0u : (cmd >= 0x23 && cmd <= 0x28) ? 1u : (cmd == 0x21 || cmd == 0x20 || cmd == 0x22) ? 2u : 3u;

	gx_exec_cmd_inner(g, cmd, p);
	g->prof[cls] += t0 - PROF_T();
#else
	gx_exec_cmd_inner(g, cmd, p);
#endif
}

static void gx_exec_cmd_inner(gx_exec *g, uint32_t cmd, const uint32_t *p)
{
	gx_mat m;

	g->by_cmd[cmd & 0x7Fu]++;
	switch (cmd) {
	case 0x00:
		return;
	case 0x10:
		g->sp.mode = p[0] & 3u;
		return;
	case 0x11:
		mtx_push(g);
		return;
	case 0x12:
		mtx_pop(g, p[0]);
		return;
	case 0x13:
		mtx_store(g, p[0]);
		return;
	case 0x14:
		mtx_restore(g, p[0]);
		return;
	case 0x15:
		mat_ident(&m);
		mtx_load(g, &m);
		return;
	case 0x16:
		mat_from_params(&m, p, 44);
		mtx_load(g, &m);
		return;
	case 0x17:
		mat_from_params(&m, p, 43);
		mtx_load(g, &m);
		return;
	case 0x18:
		mat_from_params(&m, p, 44);
		mtx_mult(g, &m, 0);
		return;
	case 0x19:
		mat_from_params(&m, p, 43);
		mtx_mult(g, &m, 0);
		return;
	case 0x1A:
		mat_from_params(&m, p, 33);
		mtx_mult(g, &m, 0);
		return;
	case 0x1B:
		mat_ident(&m);
		m.m[0] = (int32_t)p[0];
		m.m[5] = (int32_t)p[1];
		m.m[10] = (int32_t)p[2];
		mtx_mult(g, &m, 1);
		return;
	case 0x1C:
		mat_ident(&m);
		m.m[12] = (int32_t)p[0];
		m.m[13] = (int32_t)p[1];
		m.m[14] = (int32_t)p[2];
		mtx_mult(g, &m, 0);
		return;
	case 0x20:
		g->color5[0] = p[0] & 31u;
		g->color5[1] = (p[0] >> 5) & 31u;
		g->color5[2] = (p[0] >> 10) & 31u;
		return;
	case 0x21:
		do_normal(g, p[0]);
		return;
	case 0x22:
		g->tex_s = (int16_t)p[0];
		g->tex_t = (int16_t)(p[0] >> 16);
		if ((g->texparam >> 30) == 1) {
			tex_mode1(g);
		} else {
			g->cur_s = g->tex_s;
			g->cur_t = g->tex_t;
		}
		return;
	case 0x23:
		vertex_submit(g, (int16_t)p[0], (int16_t)(p[0] >> 16), (int16_t)p[1]);
		return;
	case 0x24:
		vertex_submit(g, sext(p[0], 10) * 64, sext(p[0] >> 10, 10) * 64, sext(p[0] >> 20, 10) * 64);
		return;
	case 0x25:
		vertex_submit(g, (int16_t)p[0], (int16_t)(p[0] >> 16), g->vz);
		return;
	case 0x26:
		vertex_submit(g, (int16_t)p[0], g->vy, (int16_t)(p[0] >> 16));
		return;
	case 0x27:
		vertex_submit(g, g->vx, (int16_t)p[0], (int16_t)(p[0] >> 16));
		return;
	case 0x28:
		vertex_submit(g, g->vx + sext(p[0], 10), g->vy + sext(p[0] >> 10, 10), g->vz + sext(p[0] >> 20, 10));
		return;
	case 0x29:
		g->polyattr = p[0];
		return;
	case 0x2A:
		g->texparam = p[0];
		return;
	case 0x2B:
		g->pltt = p[0] & 0x1FFFu;
		return;
	case 0x30:
		g->dif[0] = p[0] & 31u; g->dif[1] = (p[0] >> 5) & 31u; g->dif[2] = (p[0] >> 10) & 31u;
		g->amb[0] = (p[0] >> 16) & 31u; g->amb[1] = (p[0] >> 21) & 31u; g->amb[2] = (p[0] >> 26) & 31u;
		if (p[0] & 0x8000u) { /* bit 15: el color del vértice pasa a ser el difuso */
			g->color5[0] = g->dif[0]; g->color5[1] = g->dif[1]; g->color5[2] = g->dif[2];
		}
		return;
	case 0x31:
		g->spe[0] = p[0] & 31u; g->spe[1] = (p[0] >> 5) & 31u; g->spe[2] = (p[0] >> 10) & 31u;
		g->shin_en = (p[0] >> 15) & 1u;
		g->emi[0] = (p[0] >> 16) & 31u; g->emi[1] = (p[0] >> 21) & 31u; g->emi[2] = (p[0] >> 26) & 31u;
		return;
	case 0x32: {
		const unsigned i = p[0] >> 30;
		const int32_t *d = g->vec.m;
		const int32_t v[3] = { sext(p[0], 10), sext(p[0] >> 10, 10), sext(p[0] >> 20, 10) };

		/* la luz se guarda ya transformada por la matriz vectorial del momento */
		g->light_vec[i][0] = clamp_s(((int64_t)v[0] * d[0] + (int64_t)v[1] * d[4] + (int64_t)v[2] * d[8]) >> 12, -512, 511);
		g->light_vec[i][1] = clamp_s(((int64_t)v[0] * d[1] + (int64_t)v[1] * d[5] + (int64_t)v[2] * d[9]) >> 12, -512, 511);
		g->light_vec[i][2] = clamp_s(((int64_t)v[0] * d[2] + (int64_t)v[1] * d[6] + (int64_t)v[2] * d[10]) >> 12, -512, 511);
		return;
	}
	case 0x33: {
		const unsigned i = p[0] >> 30;

		g->light_col[i][0] = p[0] & 31u;
		g->light_col[i][1] = (p[0] >> 5) & 31u;
		g->light_col[i][2] = (p[0] >> 10) & 31u;
		return;
	}
	case 0x34:
		for (unsigned i = 0; i < 32; i++)
			for (unsigned b = 0; b < 4; b++)
				g->shin[i * 4 + b] = (uint8_t)(p[i] >> (8 * b));
		return;
	case 0x40:
		g->polyattr_cur = g->polyattr;
		g->prim = p[0] & 3u;
		g->asm_n = 0;
		g->asm_par = 0;
		return;
	case 0x41:
		return;
	case 0x50: /* SWAP_BUFFERS: lo atiende el motor de tiempo (gx.c) */
		return;
	case 0x60:
		g->viewport = p[0];
		return;
	case 0x70:
		g->out.box_result = (uint32_t)box_visible(g, p);
		return;
	case 0x71: {
		gx_cvtx c;

		g->vx = (int16_t)p[0]; g->vy = (int16_t)(p[0] >> 16); g->vz = (int16_t)p[1];
		clip_update(g);
		clip_vertex_of(g, g->vx, g->vy, g->vz, &c);
		g->out.pos_result[0] = c.x; g->out.pos_result[1] = c.y; g->out.pos_result[2] = c.z; g->out.pos_result[3] = c.w;
		return;
	}
	case 0x72: {
		const int32_t *d = g->vec.m;
		const int64_t v[3] = { sext(p[0], 10) * 8, sext(p[0] >> 10, 10) * 8, sext(p[0] >> 20, 10) * 8 };

		/* melonDS: si el bit 12 del resultado está encendido se le encienden también los bits 13 a 15 (13 bits con signo
		 * a medias); con el bit 12 apagado el resultado queda como salió. [sin contrastar en una DS real] */
		for (int c = 0; c < 3; c++) {
			uint16_t r = (uint16_t)((v[0] * d[c] + v[1] * d[4 + c] + v[2] * d[8 + c]) >> 12);

			if (r & 0x1000u)
				r |= 0xE000u;
			g->out.vec_result[c] = (int16_t)r;
		}
		return;
	}
	default:
		return;
	}
}


/* ---- registro de órdenes, coste y arranque -------------------------------------------------------------------- */

void gx_exec_run(gx_exec *g, const uint32_t *log, uint32_t nwords)
{
	uint32_t i = 0;

	while (i < nwords) {
		const uint32_t cmd = log[i];
		const unsigned np = gx_cmd_np(cmd);

		gx_exec_cmd(g, cmd, log + i + 1);
		i += 1u + (np == NP_BAD ? 0u : np);
	}
}

void gx_exec_flush_mats(gx_exec *g)
{
	clip_update(g);
	memcpy(g->out.clip, g->clip.m, sizeof g->out.clip);
	memcpy(g->out.vec, g->vec.m, sizeof g->out.vec);
}

void gx_exec_newframe(gx_exec *g)
{
	g->out.npolys = g->out.nverts = 0;
	g->out.dropped_polys = g->out.dropped_verts = 0;
	g->vbase = g->pbase = 0;
}

void gx_exec_init(gx_exec *g)
{
	memset(g, 0, sizeof *g);
	mat_ident(&g->proj);
	mat_ident(&g->pos);
	mat_ident(&g->vec);
	mat_ident(&g->tex);
	mat_ident(&g->clip);
	mat_ident(&g->stk_proj);
	mat_ident(&g->stk_tex);
	for (unsigned i = 0; i < GX_STACK_POS; i++) {
		mat_ident(&g->stk_pos[i]);
		mat_ident(&g->stk_vec[i]);
	}
	g->viewport = 0u | (0u << 8) | (255u << 16) | (191u << 24);
	g->color5[0] = g->color5[1] = g->color5[2] = 31u;
	g->vcap = GX_MAX_VERTS;
	g->pcap = GX_MAX_POLYS;
}

/* ---- trabajos ---------------------------------------------------------------------------------------------- */

typedef struct job_state {
	const gx_job_io *io;
	gx_vertex *vstage;
	gx_poly *pstage;
} job_state;

static void job_flush(gx_exec *ex, void *ctx)
{
	job_state *js = ctx;

	if (ex->out.nverts > ex->vbase) {
		js->io->put_verts(js->io->ctx, ex->vbase, js->vstage, ex->out.nverts - ex->vbase);
		ex->vbase = ex->out.nverts;
	}
	if (ex->out.npolys > ex->pbase) {
		js->io->put_polys(js->io->ctx, ex->pbase, js->pstage, ex->out.npolys - ex->pbase);
		ex->pbase = ex->out.npolys;
	}
}

void gx_job_run(gx_exec *ex, uint32_t flags, uint32_t nwords, const gx_job_io *io, gx_vertex *vstage, uint32_t vcap,
		gx_poly *pstage, uint32_t pcap, uint32_t *chunk, uint32_t chunk_words)
{
	job_state js = { io, vstage, pstage };
	uint32_t pos = 0;

	if (flags & GX_JOB_RESET) {
		gx_exec_init(ex);
		gx_exec_newframe(ex);
	} else if (flags & GX_JOB_NEWFRAME) {
		gx_exec_newframe(ex);
	}
	ex->vtx = vstage;
	ex->poly = pstage;
	ex->vbase = ex->out.nverts;
	ex->pbase = ex->out.npolys;
	ex->vcap = vcap;
	ex->pcap = pcap;
	ex->flush = job_flush;
	ex->flush_ctx = &js;
	while (pos < nwords) {
		uint32_t n = nwords - pos, i = 0;

		if (n > chunk_words)
			n = chunk_words;
		io->fetch(io->ctx, pos, chunk, n);
		while (i < n) {
			const unsigned np = gx_cmd_np(chunk[i]);
			const uint32_t need = 1u + (np == NP_BAD ? 0u : np);

			if (i + need > n && pos + n < nwords)
				break; /* registro partido por el final del tramo: se pide de nuevo desde él */
			gx_exec_cmd(ex, chunk[i], chunk + i + 1);
			i += need;
		}
		pos += i;
	}
	job_flush(ex, &js);
	ex->flush = NULL;
	if (flags & GX_JOB_MATS)
		gx_exec_flush_mats(ex);
}

#ifdef GX_TEST
/* ganchos de las pruebas unitarias (tests/unit/test_gx_math.c): la aritmética barata contra la de 64 bits */
uint64_t gx_t_mulu32(uint32_t a, uint32_t b) { return mulu32(a, b); }
int64_t gx_t_muls32(int32_t a, int32_t b) { return muls32(a, b); }
int gx_t_udiv_small(uint64_t n, uint64_t d, uint32_t *q) { return udiv_small(n, d, q); }
uint64_t gx_t_udiv_q32(uint64_t n, uint64_t d) { return udiv_q32(n, d); }
int64_t gx_t_div_trunc(int64_t n, int64_t d) { return div_trunc(n, d); }
int32_t gx_t_lerp(int32_t a, int32_t b, int64_t da, int64_t db) { return lerp_field(a, b, da, db); }
void gx_t_mat_mul(gx_mat *o, const gx_mat *a, const gx_mat *b) { mat_mul(o, a, b); }
void gx_t_xform_point(const int32_t *m, int32_t x, int32_t y, int32_t z, int32_t out[4]) { xform_point(m, x, y, z, out); }
void gx_t_xform_normal(const int32_t *m, const int32_t n[3], int32_t t[3]) { xform_normal(m, n, t); }
#endif
