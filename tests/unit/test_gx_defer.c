/* SPDX-License-Identifier: GPL-2.0-or-later */
/* SalsaDS — el motor de geometría en modo diferido contra el directo (Fase 3.5).
 *
 * Dos motores reciben las mismas escrituras con los mismos instantes: uno ejecuta cada orden al empezar (directo) y el
 * otro las anota y las ejecuta por lotes en el ejecutor de prueba (ventanas de salida de 24 vértices y 3 polígonos, tramos
 * de 96 palabras). Las lecturas de GXSTAT, RAM_COUNT, resultados y matrices en instantes al azar y cada cuadro entregado
 * tienen que ser idénticos. Corre en little y big-endian. */
#include <stdint.h>
#include <string.h>

#include "check.h"
#include "gpu3d/gx.h"
#include "gpu3d/gx_loop.h"
#include "gpu3d/gx_selftest.h"

static gx_t A, B; /* A directo, B diferido */
static gx_loop L;
static uint64_t T;
static uint32_t rs = 12345;

static uint32_t rnd(void)
{
	rs = rs * 1664525u + 1013904223u;
	return rs >> 8;
}

static int32_t rr(int32_t range)
{
	return (int32_t)(rnd() % (uint32_t)(2 * range + 1)) - range;
}

static void both_port(uint32_t cmd, uint32_t p)
{
	gx_port_write(&A, T, 0x400u + 4u * cmd, p);
	gx_port_write(&B, T, 0x400u + 4u * cmd, p);
}

static void same_state(const char *what)
{
	uint32_t offs[] = { 0x600, 0x604, 0x620, 0x624, 0x628, 0x62C, 0x630, 0x634 };

	(void)what;
	for (unsigned i = 0; i < sizeof offs / sizeof offs[0]; i++)
		CHECK_EQ(gx_read32(&B, T, offs[i]), gx_read32(&A, T, offs[i]));
}

static void same_mats(void)
{
	for (uint32_t o = 0x640; o <= 0x67C; o += 4)
		CHECK_EQ(gx_read32(&B, T, o), gx_read32(&A, T, o));
	for (uint32_t o = 0x680; o <= 0x6A0; o += 4)
		CHECK_EQ(gx_read32(&B, T, o), gx_read32(&A, T, o));
}

static void same_frame(const gx_frame *a, const gx_frame *b)
{
	CHECK(a && b);
	if (!a || !b)
		return;
	CHECK_EQ(b->npolys, a->npolys);
	CHECK_EQ(b->nverts, a->nverts);
	CHECK_EQ(b->dropped_polys, a->dropped_polys);
	CHECK_EQ(b->swap_params, a->swap_params);
	CHECK(!memcmp(a->vtx, b->vtx, (size_t)a->nverts * sizeof a->vtx[0]));
	CHECK(!memcmp(a->poly, b->poly, (size_t)a->npolys * sizeof a->poly[0]));
}

static void rand_mat(uint32_t *p, int n)
{
	for (int i = 0; i < n; i++)
		p[i] = (uint32_t)rr(0x2000);
}

static void send_vertex(void)
{
	switch (rnd() % 5) {
	case 0: both_port(0x23, ((uint32_t)(uint16_t)rr(0x1000)) | ((uint32_t)(uint16_t)rr(0x1000) << 16)); both_port(0x23, (uint32_t)(uint16_t)rr(0x1000)); break;
	case 1: both_port(0x24, ((uint32_t)rr(60) & 0x3FF) | (((uint32_t)rr(60) & 0x3FF) << 10) | (((uint32_t)rr(60) & 0x3FF) << 20)); break;
	case 2: both_port(0x25, ((uint32_t)(uint16_t)rr(0x1000)) | ((uint32_t)(uint16_t)rr(0x1000) << 16)); break;
	case 3: both_port(0x28, ((uint32_t)rr(100) & 0x3FF) | (((uint32_t)rr(100) & 0x3FF) << 10) | (((uint32_t)rr(100) & 0x3FF) << 20)); break;
	default: both_port(0x26, ((uint32_t)(uint16_t)rr(0x1000)) | ((uint32_t)(uint16_t)rr(0x1000) << 16)); break;
	}
}

static void random_step(void)
{
	uint32_t p[16];

	switch (rnd() % 44) {
	case 0: both_port(0x10, rnd() % 4); break;
	case 1: both_port(0x11, 0); break;
	case 2: both_port(0x12, rnd() % 3); break;
	case 3: both_port(0x13, rnd() % 33); break;
	case 4: both_port(0x14, rnd() % 33); break;
	case 5: both_port(0x15, 0); break;
	case 6:
		rand_mat(p, 12);
		for (int i = 0; i < 12; i++)
			both_port(0x19, p[i]);
		break;
	case 7:
		for (int i = 0; i < 3; i++)
			both_port(0x1C, (uint32_t)rr(0x400));
		break;
	case 8:
		for (int i = 0; i < 3; i++)
			both_port(0x1B, (uint32_t)(0x1000 + rr(0x600)));
		break;
	case 9: both_port(0x20, rnd() & 0x7FFF); break;
	case 10: both_port(0x21, rnd() & 0x3FFFFFFF); break;
	case 11: both_port(0x22, ((uint32_t)(uint16_t)rr(2000)) | ((uint32_t)(uint16_t)rr(2000) << 16)); break;
	case 12: both_port(0x29, (rnd() & 0x1F1FFFFF) | ((rnd() & 1) ? 0xC0u : 0u) | 0x1000u | (rnd() & 0xF)); break;
	case 13: both_port(0x2A, (rnd() & 0x3FFFFFFF) | ((rnd() % 4) << 30)); break;
	case 14: both_port(0x40, rnd() % 4); break;
	case 15: both_port(0x32, (rnd() & 0x3FFFFFFF) | ((rnd() % 4) << 30)); break;
	case 16: both_port(0x33, (rnd() & 0x7FFF) | ((rnd() % 4) << 30)); break;
	case 17: both_port(0x30, rnd()); both_port(0x31, rnd()); break;
	case 18: both_port(0x60, rnd() % 4 ? 0xBF000000u | 0x00FF0000u : rnd()); break;
	case 19:
		both_port(0x70, (uint32_t)(uint16_t)rr(0x1000) | ((uint32_t)(uint16_t)rr(0x1000) << 16));
		both_port(0x70, (uint32_t)(uint16_t)rr(0x1000) | ((uint32_t)(uint16_t)rr(0x1000) << 16));
		both_port(0x70, (uint32_t)(uint16_t)rr(0x1000) | ((uint32_t)(uint16_t)rr(0x1000) << 16));
		break;
	case 20: both_port(0x72, rnd()); break;
	case 21:
		both_port(0x71, (uint32_t)(uint16_t)rr(0x1000) | ((uint32_t)(uint16_t)rr(0x1000) << 16));
		both_port(0x71, (uint32_t)(uint16_t)rr(0x1000));
		break;
	default:
		for (int i = 0; i < 1 + (int)(rnd() % 12); i++)
			send_vertex();
		break;
	}
	T += 4 + (uint64_t)(rnd() % 40);
}

static void run_seed(uint32_t seed, int steps)
{
	rs = seed;
	gx_init(&A);
	gx_loop_attach(&B, &L);
	T = 1000;
	/* una proyección y vista razonables para que haya polígonos */
	both_port(0x10, 0);
	both_port(0x15, 0);
	both_port(0x10, 1);
	both_port(0x15, 0);
	for (int i = 0; i < steps; i++) {
		random_step();
		if (rnd() % 17 == 0) {
			T += (uint64_t)(rnd() % 3000);
			same_state("estado");
		}
		if (rnd() % 150 == 0) {
			T += 5000;
			same_mats();
		}
		if (rnd() % 300 == 0) { /* "VBlank": ambos entregan el cuadro */
			gx_regs regs;

			memset(&regs, 0, sizeof regs);
			regs.disp3dcnt = (uint16_t)rnd();
			T += 400000;
			gx_advance(&A, T);
			gx_advance(&B, T);
			CHECK_EQ(gx_swap_pending(&B), gx_swap_pending(&A));
			if (gx_swap_pending(&A)) {
				gx_swap(&A, T, &regs);
				gx_swap(&B, T, &regs);
				same_frame(gx_front(&A), gx_front(&B));
			}
		}
	}
	T += 4000000;
	same_state("final");
	same_mats();
	{
		const gx_stats *a = gx_stats_get(&A), *b = gx_stats_get(&B);

		CHECK_EQ(b->cmds, a->cmds);
		CHECK_EQ(b->polys, a->polys);
		CHECK_EQ(b->culled, a->culled);
		CHECK_EQ(b->clipped, a->clipped);
		CHECK_EQ(b->rejected, a->rejected);
		CHECK_EQ(b->dropped_poly, a->dropped_poly);
	}
}

void test_gx_defer(void)
{
	uint64_t polys = 0;

	for (uint32_t seed = 1; seed <= 24; seed++) {
		run_seed(seed * 7919u, 4000);
		polys += gx_stats_get(&A)->polys;
	}
	CHECK(polys > 5000); /* que las secuencias dibujen de verdad */
	CHECK(L.jobs > 50);
	printf("geometría diferida: 24 secuencias al azar, %llu polígonos, %u trabajos, idéntico al modo directo\n",
	       (unsigned long long)polys, L.jobs);
}

/* ---- gx_job_run (lo que ejecuta el SPE) contra gx_exec_run sobre el cuadro entero ---- */

static gx_exec JR, JJ; /* referencia y trabajo */
static gx_frame FR, FJ;
static uint32_t JLOG[40000];

typedef struct jio {
	const uint32_t *log;
	gx_frame *f;
	uint32_t base; /* posición del trabajo dentro del registro */
} jio;

static void j_fetch(void *c, uint32_t pos, uint32_t *dst, uint32_t n)
{
	const jio *j = c;

	memcpy(dst, j->log + j->base + pos, (size_t)n * 4u);
}

static void j_pv(void *c, uint32_t first, const gx_vertex *src, uint32_t n)
{
	jio *j = c;

	memcpy(&j->f->vtx[first], src, (size_t)n * sizeof *src);
}

static void j_pp(void *c, uint32_t first, const gx_poly *src, uint32_t n)
{
	jio *j = c;

	memcpy(&j->f->poly[first], src, (size_t)n * sizeof *src);
}

static uint32_t split_at(const uint32_t *log, uint32_t n, uint32_t want)
{
	uint32_t i = 0;

	while (i < n && i < want)
		i += 1u + gx_cmd_np(log[i]);
	return i;
}

/* Las tiras conservan vértices al cambiar VIEWPORT, incluso con recorte y caras descartadas. */
static void projection_cmd(uint32_t cmd, uint32_t a, uint32_t b)
{
	const uint32_t p[2] = { a, b };

	gx_exec_cmd(&JR, cmd, p);
	gx_exec_cmd(&JJ, cmd, p);
	CHECK(!memcmp(&JR.out, &JJ.out, sizeof JR.out));
	CHECK(!memcmp(FR.vtx, FJ.vtx, JR.out.nverts * sizeof FR.vtx[0]));
	CHECK(!memcmp(FR.poly, FJ.poly, JR.out.npolys * sizeof FR.poly[0]));
}

static void projection_parity(void)
{
	for (unsigned prim = 0; prim < 4; prim++) {
		gx_exec_init(&JR);
		gx_exec_init(&JJ);
		JR.poly_plain = 1;
		JR.vtx = FR.vtx; JR.poly = FR.poly;
		JJ.vtx = FJ.vtx; JJ.poly = FJ.poly;
		for (unsigned attr = 0; attr < 4; attr++) {
			projection_cmd(0x29, 0x1F1000u | (attr << 6), 0);
			projection_cmd(0x40, prim, 0);
			for (unsigned i = 0; i < 48; i++) {
				const int x = i % 5 == 0 ? 6000 : (i & 1) ? -2048 : 2048;
				const int y = i & 2 ? -2048 : 2048;
				const uint32_t views[] = { 0xBFFF0000u, 0x80702010u, 0x0101FFFFu, 0xFFFFFFFFu };

				if (i % 3 == 0)
					projection_cmd(0x60, views[(i / 3) % 4], 0);
				projection_cmd(0x23, (uint16_t)x | ((uint32_t)(uint16_t)y << 16), 0);
			}
		}
	}
}

void test_gx_job(void)
{
	static gx_vertex vst[40];
	static gx_poly pst[5];
	static uint32_t chunk[80];
	uint64_t polys = 0;

	projection_parity();
	for (uint32_t seed = 1; seed <= 12; seed++) {
		const uint32_t n = gx_selftest_gen(seed, JLOG, 40000, 3000);
		const uint32_t cut = split_at(JLOG, n, n / 2 + seed * 7);
		jio j = { JLOG, &FJ, 0 };
		gx_job_io io = { &j, j_fetch, j_pv, j_pp };

		gx_exec_init(&JR);
		JR.poly_plain = 1; /* referencia sin caché contra trabajos con vértices compartidos */
		gx_exec_newframe(&JR);
		JR.vtx = FR.vtx;
		JR.poly = FR.poly;
		gx_exec_run(&JR, JLOG, n);
		gx_exec_flush_mats(&JR);

		memset(&JJ, 0, sizeof JJ);
		j.base = 0;
		gx_job_run(&JJ, GX_JOB_RESET, cut, &io, vst, 40, pst, 5, chunk, 80);
		j.base = cut;
		gx_job_run(&JJ, GX_JOB_MATS, n - cut, &io, vst, 40, pst, 5, chunk, 80);

		CHECK(n > 3000);
		CHECK_EQ(JJ.out.npolys, JR.out.npolys);
		CHECK_EQ(JJ.out.nverts, JR.out.nverts);
		CHECK(!memcmp(&JJ.out, &JR.out, sizeof JR.out));
		CHECK(!memcmp(FJ.vtx, FR.vtx, (size_t)JR.out.nverts * sizeof FR.vtx[0]));
		CHECK(!memcmp(FJ.poly, FR.poly, (size_t)JR.out.npolys * sizeof FR.poly[0]));
		polys += JR.out.npolys;
	}
	CHECK(polys > 3000);
	printf("geometría, trabajos del ejecutor: 12 registros partidos en dos, %llu polígonos, idénticos a la ejecución directa\n",
	       (unsigned long long)polys);
}
