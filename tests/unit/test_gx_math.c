/* SPDX-License-Identifier: GPL-2.0-or-later */
/* SalsaDS — la aritmética barata del ejecutor de geometría (Fase 3.5) contra la de 64 bits de C: multiplicaciones de 32 bits con
 * trozos de 16, cocientes con estimación en coma flotante, el producto de matrices y las transformaciones en vectores de 4. Se
 * compila con GX_VEC, GX_SPLIT_MUL y GX_TEST (tests/unit/Makefile): en el host corre lo mismo que corre el SPE. */
#include <stdint.h>
#include <string.h>

#include "check.h"
#include "gpu3d/gx_exec.h"

void gx_t_outcodes(gx_cvtx *const *v, int n, unsigned *all, unsigned *any);
uint64_t gx_t_mulu32(uint32_t a, uint32_t b);
int64_t gx_t_muls32(int32_t a, int32_t b);
int gx_t_udiv_small(uint64_t n, uint64_t d, uint32_t *q);
uint64_t gx_t_udiv_q32(uint64_t n, uint64_t d);
int64_t gx_t_div_trunc(int64_t n, int64_t d);
int32_t gx_t_lerp(int32_t a, int32_t b, int64_t da, int64_t db);
void gx_t_mat_mul(gx_mat *o, const gx_mat *a, const gx_mat *b);
void gx_t_xform_point(const int32_t *m, int32_t x, int32_t y, int32_t z, int32_t out[4]);
void gx_t_xform_normal(const int32_t *m, const int32_t n[3], int32_t t[3]);

static uint64_t rs = 0x9E3779B97F4A7C15ull;
static uint32_t r32(void)
{
	rs ^= rs << 13;
	rs ^= rs >> 7;
	rs ^= rs << 17;
	return (uint32_t)(rs >> 16);
}

/* valores de borde con mucha frecuencia */
static int32_t rv(void)
{
	static const int32_t edge[] = { 0, 1, -1, 0x7FFFFFFF, (int32_t)0x80000000, 0x8000, -0x8000, 0xFFFF, 0x10000, -0x10000, 0x1000, -0x1000, 0x7FFF };
	const uint32_t k = r32();

	switch (k & 3) {
	case 0: return edge[(k >> 4) % (sizeof edge / sizeof edge[0])];
	case 1: return (int32_t)(r32() % 0x20000) - 0x10000;
	default: return (int32_t)r32();
	}
}

void test_gx_math(void)
{
	int bad = 0;

	for (int i = 0; i < 300000; i++) {
		const uint32_t a = (uint32_t)rv(), b = (uint32_t)rv();

		bad += gx_t_mulu32(a, b) != (uint64_t)a * b;
		bad += gx_t_muls32((int32_t)a, (int32_t)b) != (int64_t)(int32_t)a * (int32_t)b;
	}
	CHECK_EQ(bad, 0);

	/* Los seis planos, incluidos W negativo, extremos y puntos sobre el plano. */
	{
		const uint64_t saved = rs;
		bad = 0;
		for (int i = 0; i < 100000; i++) {
			gx_cvtx v[4] = { 0 }, *src[4] = { &v[0], &v[1], &v[2], &v[3] };
			unsigned all, any, expected_all = 63, expected_any = 0;
			const int n = 3 + (i & 1);
			for (int j = 0; j < n; j++) {
				gx_cvtx *p = &v[j];
				p->x = rv(); p->y = rv(); p->z = rv(); p->w = rv();
				if (i % 3 == 0) p->x = p->w;
				if (i % 5 == 0) p->y = (int32_t)(0u - (uint32_t)p->w);
				if (j && i % 7 == 0) *p = v[0];
				const int64_t w = p->w;
				const unsigned code = ((int64_t)p->z + w < 0) | ((w - p->z < 0) << 1)
					| (((int64_t)p->x + w < 0) << 2) | ((w - p->x < 0) << 3)
					| (((int64_t)p->y + w < 0) << 4) | ((w - p->y < 0) << 5);
				expected_all &= code; expected_any |= code;
			}
			gx_t_outcodes(src, n, &all, &any);
			bad += all != expected_all || any != expected_any;
		}
		CHECK_EQ(bad, 0);
		rs = saved;
	}

	/* cocientes pequeños (pantalla y profundidad): n / d con n < d * 2^22 */
	bad = 0;
	for (int i = 0; i < 300000; i++) {
		uint64_t d = (uint64_t)(r32() % 0x200000000ull) + 1u, n;
		uint32_t q;

		if (i % 7 == 0)
			d = 1 + (r32() % 16);
		n = ((uint64_t)r32() << 20 ^ r32()) % (d << 22);
		if (i % 5 == 0)
			n = (d << 22) - 1 - (r32() % 3);
		if (!gx_t_udiv_small(n, d, &q))
			bad++;
		else
			bad += q != n / d;
		bad += gx_t_div_trunc((int64_t)n, (int64_t)d) != (int64_t)(n / d);
		bad += gx_t_div_trunc(-(int64_t)n, (int64_t)d) != -(int64_t)(n / d);
	}
	CHECK_EQ(bad, 0);

	/* cociente de hasta 32 bits */
	bad = 0;
	for (int i = 0; i < 300000; i++) {
		uint64_t d = (uint64_t)r32() % 0x800000000ull + 1u, n;

		if (i % 3 == 0)
			d = (r32() % 100) + 1;
		n = (((uint64_t)r32() << 32) | r32());
		n %= (d << 32);
		bad += gx_t_udiv_q32(n, d) != n / d;
	}
	CHECK_EQ(bad, 0);

	/* interpolación del recorte contra la de 128 bits */
	bad = 0;
	for (int i = 0; i < 300000; i++) {
		const int32_t a = rv(), b = rv();
		int64_t da = (int64_t)(uint32_t)r32() + (i % 4 == 0 ? (int64_t)r32() : 0), db = -((int64_t)r32() % 0x100000000ll) - 1;

		if (i % 9 == 0)
			da = r32() % 100;
		{
			const int64_t d = da - db;
			const int32_t want = (int32_t)(a + (__int128)((int64_t)b - a) * da / d);

			bad += gx_t_lerp(a, b, da, db) != want;
		}
	}
	CHECK_EQ(bad, 0);

	/* matrices y transformaciones */
	bad = 0;
	for (int i = 0; i < 100000; i++) {
		gx_mat a, b, o, want;
		int32_t out[4], wo[4], n[3], t[3], wt[3];
		int32_t x = rv(), y = rv(), z = rv();

		for (int k = 0; k < 16; k++) {
			a.m[k] = rv();
			b.m[k] = rv();
		}
		gx_t_mat_mul(&o, &a, &b);
		for (int r = 0; r < 4; r++)
			for (int c = 0; c < 4; c++) {
				int64_t s = 0;

				for (int k = 0; k < 4; k++)
					s = (int64_t)((uint64_t)s + (uint64_t)((int64_t)a.m[r * 4 + k] * b.m[k * 4 + c]));
				want.m[r * 4 + c] = (int32_t)(s >> 12);
			}
		bad += memcmp(&o, &want, sizeof o) != 0;
		gx_t_xform_point(a.m, x, y, z, out);
		for (int c = 0; c < 4; c++) {
			const uint64_t s = (uint64_t)((int64_t)x * a.m[c]) + (uint64_t)((int64_t)y * a.m[4 + c]) + (uint64_t)((int64_t)z * a.m[8 + c]) +
					   (uint64_t)((int64_t)a.m[12 + c] * 4096);

			wo[c] = (int32_t)((int64_t)s >> 12);
		}
		bad += memcmp(out, wo, sizeof out) != 0;
		for (int k = 0; k < 3; k++)
			n[k] = (int32_t)(r32() % 1024) - 512;
		gx_t_xform_normal(a.m, n, t);
		for (int c = 0; c < 3; c++) {
			int64_t v = ((int64_t)n[0] * a.m[c] + (int64_t)n[1] * a.m[4 + c] + (int64_t)n[2] * a.m[8 + c]) >> 12;

			wt[c] = (int32_t)(v < -512 ? -512 : v > 511 ? 511 : v);
		}
		bad += memcmp(t, wt, sizeof t) != 0;
	}
	CHECK_EQ(bad, 0);
	printf("geometría, aritmética barata: multiplicaciones de 32 bits, cocientes, interpolación del recorte, matrices y transformaciones: iguales a la de 64 bits\n");
}
