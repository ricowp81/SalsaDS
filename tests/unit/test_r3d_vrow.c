/*
 * SalsaDS — línea del render 3D en vectores (src/gpu3d/r3d_vrow.h, Fase 3.4) contra la escalar (r3d_draw_row de r3d_px.h):
 * miles de líneas de polígonos al azar sin textura (modos, alfas, sombras, alambre, W/Z, translúcidos, niebla, prueba de
 * profundidad «igual», alpha test) con el contenido previo de los búferes al azar: el color, la profundidad y los
 * atributos tienen que quedar idénticos y los contadores iguales. Con textura, las escenas de test_render3d cubren el
 * resto (son las mismas líneas por el mismo camino, con el bloque de texturas).
 */
#include <stdio.h>
#include <string.h>

#include "check.h"
#include "gpu3d/r3d.h"
#include "gpu3d/r3d_core.h"
#include "gpu3d/r3d_px.h"

static uint32_t lcg_s;

static uint32_t rnd(void)
{
	lcg_s = lcg_s * 1664525u + 1013904223u;
	return lcg_s >> 8 | lcg_s << 24;
}

static uint32_t notex(void *ctx, const r3d_tex *tx, uint32_t s, uint32_t t)
{
	(void)ctx;
	(void)tx;
	return s ^ t;
}

/* ---- mallas sin juntas: triángulos vecinos que comparten aristas (cuadrículas de vértices al azar, con holgura de un cuarto
 * de celda para que no haya triángulos invertidos) cubren todos los píxeles de su interior al menos una vez ---- */
static uint32_t cover[R3D_H][R3D_W];

static void mesh_tri(int x0, int y0, int x1, int y1, int x2, int y2, int persp)
{
	r3d_poly p;
	const int xs[3] = { x0, x1, x2 }, ys[3] = { y0, y1, y2 };
	int64_t area = 0;

	memset(&p, 0, sizeof p);
	p.n = 3;
	p.alpha = 31;
	p.ytop = 0x7FFFFFFF;
	p.ybot = -0x7FFFFFFF;
	for (int k = 0; k < 3; k++) {
		r3d_v *d = &p.v[k];

		d->x = xs[k];
		d->y = ys[k];
		d->z = 1000;
		d->w = persp ? 1 + (rnd() % 300) : 100;
		d->r = d->g = d->b = 300;
		if (d->y < p.ytop) {
			p.ytop = d->y;
			p.top = k;
		}
		if (d->y > p.ybot)
			p.ybot = d->y;
	}
	for (int i = 0; i < 3; i++) {
		const r3d_v *a = &p.v[i], *b = &p.v[(i + 1) % 3];

		area += (int64_t)a->x * b->y - (int64_t)b->x * a->y;
	}
	p.ldir = area < 0 ? 1 : -1;
	for (int y = p.ytop < 0 ? 0 : p.ytop; y <= (p.ybot > R3D_H - 1 ? R3D_H - 1 : p.ybot); y++) {
		r3d_row row;

		if (!r3d_row_at(&p, y, &row))
			continue;
		for (int x = row.xl < 0 ? 0 : row.xl; x < row.xr && x < R3D_W; x++)
			cover[y][x]++;
	}
}

static void test_mesh_gaps(void)
{
	uint32_t gaps = 0, total = 0, dbl = 0;

	lcg_s = 99;
	for (int sd = 0; sd < 1200; sd++) {
		enum { N = 6 };
		const int cell = 12 + sd % 17, jit = 1 + (cell / 4 - 1) * (sd % 5) / 4, persp = sd % 3 == 0;
		int vx[N + 1][N + 1], vy[N + 1][N + 1];

		memset(cover, 0, sizeof cover);
		for (int j = 0; j <= N; j++)
			for (int i = 0; i <= N; i++) {
				vx[j][i] = 30 + i * cell + ((i == 0 || i == N) ? 0 : (int)(rnd() % (2 * jit + 1)) - jit);
				vy[j][i] = 20 + j * cell + ((j == 0 || j == N) ? 0 : (int)(rnd() % (2 * jit + 1)) - jit);
			}
		for (int j = 0; j < N; j++)
			for (int i = 0; i < N; i++) {
				if (rnd() & 1) {
					mesh_tri(vx[j][i], vy[j][i], vx[j][i + 1], vy[j][i + 1], vx[j + 1][i], vy[j + 1][i], persp);
					mesh_tri(vx[j][i + 1], vy[j][i + 1], vx[j + 1][i + 1], vy[j + 1][i + 1], vx[j + 1][i], vy[j + 1][i], persp);
				} else {
					mesh_tri(vx[j][i], vy[j][i], vx[j][i + 1], vy[j][i + 1], vx[j + 1][i + 1], vy[j + 1][i + 1], persp);
					mesh_tri(vx[j][i], vy[j][i], vx[j + 1][i + 1], vy[j + 1][i + 1], vx[j + 1][i], vy[j + 1][i], persp);
				}
			}
		for (int y = 22; y < 20 + N * cell - 2; y++)
			for (int x = 32; x < 30 + N * cell - 2; x++) {
				total++;
				if (cover[y][x] == 0)
					gaps++;
				else if (cover[y][x] > 1)
					dbl++;
			}
	}
	CHECK_EQ(gaps, 0);
	CHECK(total > 5000000);
	printf("render 3D mallas sin juntas: %u píxeles interiores de 1200 mallas, %u sin cubrir, %u cubiertos dos veces\n", total, gaps, dbl);
}

void test_r3d_vrow(void);
void test_r3d_vrow(void)
{
	test_mesh_gaps();
	static uint32_t ca[R3D_W] __attribute__((aligned(16))), da[R3D_W] __attribute__((aligned(16))), aa[R3D_W] __attribute__((aligned(16)));
	static uint32_t cb[R3D_W] __attribute__((aligned(16))), db[R3D_W] __attribute__((aligned(16))), ab[R3D_W] __attribute__((aligned(16)));
	uint32_t bad = 0, rows = 0, lin_rows = 0, written = 0;

	lcg_s = 12345;
	for (int it = 0; it < 400000; it++) {
		r3d_rstate rs;
		r3d_poly p;
		r3d_row row;
		r3d_stats sa, sb;
		const uint32_t spread = (it & 7) == 0 ? 600 : 300;
		const uint32_t kind = rnd() & 7;
		int32_t y;

		memset(&rs, 0, sizeof rs);
		memset(&p, 0, sizeof p);
		rs.d3 = (rnd() & 1 ? D3_BLEND : 0u) | ((rnd() & 3) == 0 ? D3_ALPHATEST : 0u) | ((rnd() & 3) == 0 ? D3_HIGHLIGHT : 0u);
		rs.alpha_ref = rnd() & 31;
		rs.wbuf = rnd() & 1;
		for (int k = 0; k < 32; k++)
			rs.toon[k] = rnd() & 0x3F3F3Fu;
		p.n = 3 + (kind == 7 ? 1 : 0);
		p.alpha = (rnd() & 3) == 0 ? 31 : 1 + (rnd() % 31);
		p.wire = (rnd() & 15) == 0;
		if (p.wire)
			p.alpha = 31;
		p.id = (rnd() & 7) == 0 ? 0 : rnd() & 63;
		p.mode = rnd() & 3;
		p.wshift = (rnd() & 3) == 0 ? 4 : 0;
		p.attr = (p.alpha << 16) | (p.id << 24) | (p.mode << 4) | ((rnd() & 1) ? PA_TDEPTH : 0u) | ((rnd() & 7) == 0 ? PA_DEQUAL : 0u) |
			 ((rnd() & 1) ? PA_FOG : 0u);
		p.ytop = 0x7FFFFFFF;
		p.ybot = -0x7FFFFFFF;
		{
			const int ortho = (rnd() & 3) == 0;
			const uint32_t w0 = 1 + (rnd() & 0x7FFF);

			for (int k = 0; k < p.n; k++) {
				r3d_v *d = &p.v[k];

				d->x = (int32_t)(rnd() % spread) - 20;
				d->y = (int32_t)(rnd() % 120) - 4;
				d->z = rnd() & 0xFFFFFF;
				d->w = ortho ? w0 : 1 + (rnd() & ((rnd() & 1) ? 0xFFFF : 0x7F));
				d->r = (int32_t)((rnd() & 63) << 3 | 7);
				d->g = (int32_t)((rnd() & 63) << 3 | 7);
				d->b = (int32_t)((rnd() & 63) << 3 | 7);
				if ((rnd() & 7) == 0)
					d->r = d->g = d->b = 0;
				if (d->y < p.ytop) {
					p.ytop = d->y;
					p.top = k;
				}
				if (d->y > p.ybot)
					p.ybot = d->y;
			}
		}
		{
			int64_t area = 0;

			for (int k = 0; k < p.n; k++) {
				const r3d_v *a = &p.v[k], *b = &p.v[(k + 1) % p.n];

				area += (int64_t)a->x * b->y - (int64_t)b->x * a->y;
			}
			p.ldir = area < 0 ? 1 : -1;
		}
		if (p.ytop == p.ybot)
			continue;
		y = p.ytop + (int32_t)(rnd() % (uint32_t)(p.ybot - p.ytop));
		for (int x = 0; x < R3D_W; x++) {
			const uint32_t r = rnd();

			ca[x] = cb[x] = (r & 1) ? r : (r & 0x1F3F3F3Fu);
			da[x] = db[x] = (r & 0x1000) ? (rnd() & 0xFFFFFF) : 0xFFFFFFu;
			aa[x] = ab[x] = rnd() & 0xFFFF;
		}
		memset(&row, 0, sizeof row);
		if (!r3d_row_at(&p, y, &row))
			continue;
		rows++;
		if (row.l.w == row.r.w)
			lin_rows++;
		memset(&sa, 0, sizeof sa);
		memset(&sb, 0, sizeof sb);
		r3d_draw_row(&p, &row, &rs, ca, da, aa, notex, NULL, &sa);
		r3d_core_row_notex(&rs, &sb, &p, &row, cb, db, ab);
		written += sa.written;
		if (memcmp(ca, cb, sizeof ca) || memcmp(da, db, sizeof da) || memcmp(aa, ab, sizeof aa) || sa.written != sb.written ||
		    sa.pixels != sb.pixels) {
			if (bad++ < 5) {
				int x;

				for (x = 0; x < R3D_W; x++)
					if (ca[x] != cb[x] || da[x] != db[x] || aa[x] != ab[x])
						break;
				printf("FALLO vrow it %d: modo %u alfa %u id %u wire %u wbuf %u w %u/%u L %d x %d..%d: píxel %d col %08x/%08x dep %06x/%06x "
				       "att %04x/%04x escritos %u/%u\n",
				       it, p.mode, p.alpha, p.id, p.wire, rs.wbuf, row.l.w, row.r.w, row.den, row.xl, row.xr, x, x < R3D_W ? ca[x] : 0,
				       x < R3D_W ? cb[x] : 0, x < R3D_W ? da[x] : 0, x < R3D_W ? db[x] : 0, x < R3D_W ? aa[x] : 0, x < R3D_W ? ab[x] : 0,
				       sa.written, sb.written);
			}
		}
	}
	CHECK_EQ(bad, 0);
	CHECK(rows > 100000);
	CHECK(lin_rows > 10000);
	CHECK(written > 1000000);
	printf("render 3D vectorial: %u líneas (%u con w iguales), %u píxeles escritos, %u distintas de la escalar\n", rows, lin_rows, written, bad);
}
