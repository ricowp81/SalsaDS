/* SalsaDS — secuencias de órdenes de prueba del ejecutor de geometría (Fase 3.5). Ver gx_selftest.h. */
#include "gpu3d/gx_selftest.h"

#include "gpu3d/gx_exec.h"

typedef struct gen {
	uint32_t *log;
	uint32_t n, max, rs;
} gen;

static uint32_t rnd(gen *g)
{
	g->rs = g->rs * 1664525u + 1013904223u;
	return g->rs >> 8;
}

static int32_t rr(gen *g, int32_t range)
{
	return (int32_t)(rnd(g) % (uint32_t)(2 * range + 1)) - range;
}

static void cmd(gen *g, uint32_t c, const uint32_t *p)
{
	const unsigned np = gx_cmd_np(c);

	if (g->n + 1u + np > g->max)
		return;
	g->log[g->n++] = c;
	for (unsigned i = 0; i < np; i++)
		g->log[g->n++] = p[i];
}

static void cmd1(gen *g, uint32_t c, uint32_t p)
{
	cmd(g, c, &p);
}

static uint32_t u16(int32_t v)
{
	return (uint32_t)(uint16_t)v;
}

static uint32_t pack10(gen *g, int32_t range)
{
	return ((uint32_t)rr(g, range) & 0x3FFu) | (((uint32_t)rr(g, range) & 0x3FFu) << 10) | (((uint32_t)rr(g, range) & 0x3FFu) << 20);
}

static void vertex(gen *g)
{
	uint32_t p[2];

	switch (rnd(g) % 5) {
	case 0:
		p[0] = u16(rr(g, 0x1000)) | (u16(rr(g, 0x1000)) << 16);
		p[1] = u16(rr(g, 0x1000));
		cmd(g, 0x23, p);
		break;
	case 1: cmd1(g, 0x24, pack10(g, 60)); break;
	case 2: cmd1(g, 0x25, u16(rr(g, 0x1000)) | (u16(rr(g, 0x1000)) << 16)); break;
	case 3: cmd1(g, 0x28, pack10(g, 100)); break;
	default: cmd1(g, 0x26, u16(rr(g, 0x1000)) | (u16(rr(g, 0x1000)) << 16)); break;
	}
}

uint32_t gx_selftest_gen(uint32_t seed, uint32_t *log, uint32_t maxw, uint32_t steps)
{
	gen gg = { log, 0, maxw, seed * 2654435761u + 1u }, *g = &gg;
	uint32_t p[16];

	cmd1(g, 0x10, 0);
	cmd1(g, 0x15, 0);
	cmd1(g, 0x10, 1);
	cmd1(g, 0x15, 0);
	cmd1(g, 0x60, 0xBF00FF00u);
	for (uint32_t i = 0; i < steps; i++) {
		switch (rnd(g) % 44) {
		case 0: cmd1(g, 0x10, rnd(g) % 4); break;
		case 1: cmd(g, 0x11, p); break;
		case 2: cmd1(g, 0x12, rnd(g) % 3); break;
		case 3: cmd1(g, 0x13, rnd(g) % 33); break;
		case 4: cmd1(g, 0x14, rnd(g) % 33); break;
		case 5: cmd(g, 0x15, p); break;
		case 6:
			for (int k = 0; k < 12; k++)
				p[k] = (uint32_t)rr(g, 0x2000);
			cmd(g, 0x19, p);
			break;
		case 7:
			for (int k = 0; k < 3; k++)
				p[k] = (uint32_t)rr(g, 0x400);
			cmd(g, 0x1C, p);
			break;
		case 8:
			for (int k = 0; k < 3; k++)
				p[k] = (uint32_t)(0x1000 + rr(g, 0x600));
			cmd(g, 0x1B, p);
			break;
		case 9: cmd1(g, 0x20, rnd(g) & 0x7FFF); break;
		case 10: cmd1(g, 0x21, rnd(g) & 0x3FFFFFFF); break;
		case 11: cmd1(g, 0x22, u16(rr(g, 2000)) | (u16(rr(g, 2000)) << 16)); break;
		case 12: cmd1(g, 0x29, (rnd(g) & 0x1F1FFFFFu) | ((rnd(g) & 1) ? 0xC0u : 0u) | 0x1000u | (rnd(g) & 0xF)); break;
		case 13: cmd1(g, 0x2A, (rnd(g) & 0x3FFFFFFFu) | ((rnd(g) % 4) << 30)); break;
		case 14: cmd1(g, 0x40, rnd(g) % 4); break;
		case 15: cmd1(g, 0x32, (rnd(g) & 0x3FFFFFFFu) | ((rnd(g) % 4) << 30)); break;
		case 16: cmd1(g, 0x33, (rnd(g) & 0x7FFFu) | ((rnd(g) % 4) << 30)); break;
		case 17: cmd1(g, 0x30, rnd(g)); cmd1(g, 0x31, rnd(g)); break;
		case 18: cmd1(g, 0x60, rnd(g) % 4 ? 0xBF00FF00u : rnd(g)); break;
		case 19:
			p[0] = u16(rr(g, 0x1000)) | (u16(rr(g, 0x1000)) << 16);
			p[1] = u16(rr(g, 0x1000)) | (u16(rr(g, 0x1000)) << 16);
			p[2] = u16(rr(g, 0x1000)) | (u16(rr(g, 0x1000)) << 16);
			cmd(g, 0x70, p);
			break;
		case 20: cmd1(g, 0x72, rnd(g)); break;
		case 21:
			p[0] = u16(rr(g, 0x1000)) | (u16(rr(g, 0x1000)) << 16);
			p[1] = u16(rr(g, 0x1000));
			cmd(g, 0x71, p);
			break;
		case 22: /* un registro de sombreado completo */
			for (int k = 0; k < 32; k++)
				p[k & 15] = rnd(g);
			break;
		default:
			for (uint32_t k = 0, nv = 1 + rnd(g) % 12; k < nv; k++)
				vertex(g);
			break;
		}
	}
	return g->n;
}
