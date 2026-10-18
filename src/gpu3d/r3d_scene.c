/*
 * SalsaDS — escenas sintéticas del render 3D (Fase 3.2): un cuadro de geometría armado con el motor de geometría
 * real (gx.c, por sus puertos de órdenes) y una VRAM de prueba, deterministas a partir de una semilla. Las usan las
 * pruebas unitarias (referencia contra el código del SPE en el host, LE y BE) y la autoprueba de la PS3 (el SPE contra
 * el PPE al arrancar). Código propio.
 */
#include <string.h>

#include "gpu3d/r3d_scene.h"

static uint32_t lcg;

static uint32_t rnd(uint32_t n)
{
	lcg = lcg * 1103515245u + 12345u;
	return n ? (lcg >> 8) % n : 0;
}

static int32_t rnds(int32_t a) /* -a..a */
{
	return (int32_t)rnd((uint32_t)(2 * a + 1)) - a;
}

typedef struct sb {
	gx_t *g;
	uint64_t t;
} sb;

static void w(sb *s, uint32_t c, uint32_t p)
{
	const uint64_t until = gx_port_write(s->g, s->t, 0x400u + 4u * c, p);

	s->t = (until > s->t ? until : s->t) + 8;
}

static void vtx(sb *s, int32_t x, int32_t y, int32_t z)
{
	w(s, 0x23, ((uint32_t)x & 0xFFFFu) | ((uint32_t)y << 16));
	w(s, 0x23, (uint32_t)z & 0xFFFFu);
}

const gx_frame *r3d_scene_build(gx_t *g, uint32_t seed, uint32_t npolys, uint8_t *vram, uint32_t texslot[R3D_TEX_SLOTS],
				uint32_t palslot[R3D_PAL_SLOTS])
{
	sb s = { g, 1000 };
	gx_regs regs;
	const uint32_t persp = seed & 1u;

	lcg = seed * 2654435761u + 0x9E3779B9u;
	/* VRAM: bytes al azar (la instantánea de 656 KB y 128 en cero) y las ranuras en bancos al azar o sin mapear */
	for (uint32_t i = 0; i < R3D_VRAM_ZERO_OFF; i += 4) {
		const uint32_t v = rnd(0x1000000u) ^ (rnd(256) << 24);

		memcpy(vram + i, &v, 4);
	}
	memset(vram + R3D_VRAM_ZERO_OFF, 0, 128);
	for (int i = 0; i < R3D_TEX_SLOTS; i++)
		texslot[i] = rnd(8) == 0 ? R3D_UNMAPPED : 0x20000u * rnd(4);
	for (int i = 0; i < R3D_PAL_SLOTS; i++)
		palslot[i] = rnd(8) == 0 ? R3D_UNMAPPED : 0x80000u + 0x4000u * rnd(9);

	gx_init(g);
	w(&s, 0x10, 0);
	w(&s, 0x15, 0);
	if (persp) { /* x' = x, y' = y, z' = -1,06·z - 0,25, w = -z */
		static const int32_t m[16] = { 0x1000, 0, 0, 0, 0, 0x1000, 0, 0, 0, 0, -0x1100, -0x1000, 0, 0, -0x0400, 0 };

		for (int i = 0; i < 16; i++)
			w(&s, 0x16, (uint32_t)m[i]);
	}
	w(&s, 0x10, 3);
	w(&s, 0x15, 0);
	w(&s, 0x10, 2);
	w(&s, 0x15, 0);
	w(&s, 0x60, rnd(4) ? (255u << 16) | (191u << 24) : (rnd(64) | (rnd(64) << 8) | ((192u + rnd(64)) << 16) | ((128u + rnd(64)) << 24)));
	for (uint32_t k = 0; k < npolys; k++) {
		const uint32_t prim = rnd(4), nv = prim == 0 ? 3 : prim == 1 ? 4 : 3 + rnd(4);
		const uint32_t mode = rnd(6) < 3 ? 0 : rnd(4);
		uint32_t alpha = rnd(3) == 0 ? 1 + rnd(30) : rnd(10) == 0 ? 0 : 31;
		uint32_t attr = 0xC0u | (mode << 4) | (alpha << 16) | (rnd(64) << 24);

		if (rnd(4) == 0)
			attr |= 1u << 11; /* los translúcidos escriben profundidad */
		attr |= 1u << 12;         /* recortar contra el plano lejano */
		if (rnd(6) == 0)
			attr |= 1u << 14; /* prueba «igual» */
		if (rnd(2))
			attr |= 1u << 15; /* niebla */
		w(&s, 0x29, attr);
		if (rnd(3)) {
			const uint32_t fmt = 1 + rnd(7), ss = rnd(5), ts = rnd(5);
			const uint32_t tp = rnd(0x10000) | (rnd(16) << 16) | (ss << 20) | (ts << 23) | (fmt << 26) | (rnd(2) << 29);

			w(&s, 0x2A, tp);
			w(&s, 0x2B, rnd(0x2000));
		} else {
			w(&s, 0x2A, 0);
		}
		w(&s, 0x40, prim);
		{
			/* un centro y vértices alrededor: chicos y grandes */
			const int32_t big = rnd(4) == 0 ? 0x1000 : 0x0400;
			const int32_t cx = rnds(0x1000), cy = rnds(0x1000), cz = persp ? -(0x0800 + (int32_t)rnd(0x5000)) : rnds(0x0F00);

			for (uint32_t i = 0; i < nv; i++) {
				w(&s, 0x20, rnd(0x8000));
				w(&s, 0x22, (rnd(0x10000) & 0xFFFFu) | (rnd(0x10000) << 16));
				vtx(&s, cx + rnds(big), cy + rnds(big), cz + rnds(persp ? 0x0800 : 0x0100));
			}
		}
		w(&s, 0x41, 0);
	}
	w(&s, 0x50, rnd(4));
	gx_advance(g, s.t + 100000000ull);
	memset(&regs, 0, sizeof regs);
	regs.disp3dcnt = (uint16_t)(rnd(256) | (rnd(16) << 8));
	regs.clear_depth = (uint16_t)(rnd(4) ? 0x7FFF : rnd(0x8000));
	regs.clear_color = rnd(0x8000) | (rnd(2) << 15) | ((rnd(3) ? 0 : rnd(32)) << 16) | (rnd(64) << 24);
	regs.fog_color = rnd(0x8000) | (rnd(32) << 16);
	regs.fog_offset = (uint16_t)rnd(0x8000);
	regs.alpha_test_ref = (uint16_t)rnd(32);
	for (int i = 0; i < 8; i++)
		regs.edge_color[i] = (uint16_t)rnd(0x8000);
	for (int i = 0; i < 32; i++) {
		regs.fog_table[i] = (uint8_t)rnd(128);
		regs.toon_table[i] = (uint16_t)rnd(0x8000);
	}
	if (!gx_swap_pending(g))
		return NULL;
	return gx_swap(g, s.t + 100000000ull, &regs);
}
