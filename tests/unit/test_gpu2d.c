/* SPDX-License-Identifier: GPL-2.0-or-later */
/* SalsaDS — pruebas del 2D (Fase 2.1): la referencia píxel por píxel contra el compositor por líneas (con kernels
 * escalares), en little-endian y en big-endian, y las huellas de las escenas fijas, que tienen que ser las mismas en los
 * dos órdenes de bytes. Las escenas aleatorias salen de una semilla: perfil (3 bits bajos) y características apagadas
 * (byte alto, ver gpu2d_scene.h). */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "check.h"
#include "gpu2d/gpu2d_compose.h"
#include "gpu2d/gpu2d_scene.h"

static void *amalloc(size_t n)
{
	void *p = NULL;

	if (posix_memalign(&p, 128, (n + 255) & ~(size_t)127))
		abort();
	memset(p, 0, n);
	return p;
}

typedef struct {
	gpu2d_scene_mem mem;
	gpu2d_frame frame;
	uint16_t ref[2][GPU2D_W * GPU2D_H];
	uint16_t out[2][GPU2D_W * GPU2D_H] __attribute__((aligned(128)));
	uint32_t img3d[GPU2D_W * GPU2D_H] __attribute__((aligned(128)));
} t_scene;

static void scene_init(t_scene *s)
{
	s->mem.vram = amalloc(G2D_VRAM_SIZE + G2D_VRAM_ZERO);
	s->mem.mmem = amalloc(GPU2D_H * 512);
	for (int e = 0; e < 2; e++) {
		s->mem.pal[e] = amalloc(1024);
		s->mem.oam[e] = amalloc(1024);
		s->mem.chg[e] = amalloc(sizeof(gpu2d_change) * (GPU2D_SCENE_CHG_CAP + 2));
	}
}

static void scene_free(t_scene *s)
{
	free(s->mem.vram);
	free(s->mem.mmem);
	for (int e = 0; e < 2; e++) {
		free(s->mem.pal[e]);
		free(s->mem.oam[e]);
		free(s->mem.chg[e]);
	}
}

/* Compara la referencia con el compositor en los dos motores; devuelve las diferencias. */
static int skipped;

static int compare(gpu2d_ctx *ctx, t_scene *s, int scene, uint32_t seed, uint64_t *hash, int with3d)
{
	int diffs = 0;

	gpu2d_scene_build(&s->frame, &s->mem, scene, seed);
	if (with3d) {
		/* Todos los alfas y bits bajos RGB; BG0 puede quedar detrás de otra capa. */
		for (unsigned i = 0; i < GPU2D_W * GPU2D_H; i++)
			s->img3d[i] = ((i + seed) & 31u) << 24 | ((i * 13u) & 63u) << 16 |
			               ((i * 7u) & 63u) << 8 | ((i * 3u) & 63u);
		s->frame.eng[0].regs[G2D_R_DISPCNT] |= G2D_DC_3D | (1u << 8);
		s->frame.eng[0].img3d_ea = (uint64_t)(uintptr_t)s->img3d;
	}
	for (int e = 0; e < 2; e++) {
		s->frame.eng[e].out_ea = (uint64_t)(uintptr_t)s->out[e];
		memset(s->out[e], 0xA5, sizeof s->out[e]);
		gpu2d_ref_engine(&s->frame.eng[e], s->ref[e]);
		CHECK_EQ(gpu2d_compose_engine(ctx, &s->frame.eng[e]), 0);
		if (ctx->stats.map_dropped) { /* límite conocido: dos mapas que no caben juntos en la arena */
			skipped++;
			continue;
		}
		for (int i = 0; i < GPU2D_W * GPU2D_H; i++) {
			if (s->out[e][i] != s->ref[e][i] && diffs++ == 0)
				printf("  primera diferencia: escena %d semilla 0x%x motor %d en (%d,%d): compositor 0x%04x, "
				       "referencia 0x%04x\n", scene, seed, e, i % GPU2D_W, i / GPU2D_W, s->out[e][i], s->ref[e][i]);
		}
		CHECK_EQ(gpu2d_compose_engine_ppe(ctx, &s->frame.eng[e]), 0);
		for (int i = 0; i < GPU2D_W * GPU2D_H; i++) {
			if (s->out[e][i] != s->ref[e][i] && diffs++ == 0)
				printf("  PPE distinto: escena %d semilla 0x%x motor %d en (%d,%d): 0x%04x != 0x%04x\n",
				       scene, seed, e, i % GPU2D_W, i / GPU2D_W, s->out[e][i], s->ref[e][i]);
		}
	}
	if (hash)
		*hash = gpu2d_hash_image(s->ref[0]) ^ (gpu2d_hash_image(s->ref[1]) * 31u);
	return diffs;
}

/* Objetivos ausentes, fondo como destino, semitransparencia y ventanas que
 * desactivan efectos: cubre las decisiones anticipadas del color final PPE. */
static void test_effect_policy(gpu2d_ctx *ctx, t_scene *s)
{
	static const unsigned targets[] = { 0, 1, 16, 32, 63 };
	gpu2d_engine *e = &s->frame.eng[0];

	gpu2d_scene_build(&s->frame, &s->mem, GPU2D_SCENE_EFECTOS, 1);
	for (unsigned i = 0; i < GPU2D_W * GPU2D_H; i++)
		s->img3d[i] = ((i & 31u) << 24) | ((i * 13u & 63u) << 16) |
		               ((i * 7u & 63u) << 8) | (i * 3u & 63u);
	e->img3d_ea = (uint64_t)(uintptr_t)s->img3d;
	e->out_ea = (uint64_t)(uintptr_t)s->out[0];
	e->nchg = 0;
	e->regs[G2D_R_DISPCNT] |= G2D_DC_3D | (1u << 8);
	const uint16_t dc = e->regs[G2D_R_DISPCNT];

	/* Repetir con 3D totalmente opaco: la segunda capa puede seguir siendo
	 * necesaria para decidir si BLDCNT aplica brillo o lo suprime. */
	for (unsigned opaque = 0; opaque < 2; opaque++) {
		if (opaque)
			for (unsigned i = 0; i < GPU2D_W * GPU2D_H; i++)
				s->img3d[i] |= 31u << 24;
		for (unsigned win = 0; win < 2; win++)
			for (unsigned mode = 0; mode < 4; mode++)
				for (unsigned a = 0; a < 5; a++)
					for (unsigned b = 0; b < 5; b++) {
						e->regs[G2D_R_DISPCNT] = win ? dc :
							(uint16_t)(dc & ~(G2D_DC_WIN0 | G2D_DC_WIN1 | G2D_DC_OBJWIN));
						e->regs[G2D_R_BLDCNT] = (uint16_t)(targets[a] | mode << 6 | targets[b] << 8);
						e->regs[G2D_R_BLDY] = (a + b) & 1u ? 0 : 19;
						gpu2d_ref_engine(e, s->ref[0]);
						CHECK_EQ(gpu2d_compose_engine(ctx, e), 0);
						CHECK_EQ(ctx->stats.map_dropped, 0);
						CHECK(memcmp(s->out[0], s->ref[0], sizeof s->ref[0]) == 0);
						CHECK_EQ(gpu2d_compose_engine_ppe(ctx, e), 0);
						CHECK(memcmp(s->out[0], s->ref[0], sizeof s->ref[0]) == 0);
					}
	}
}

/* Una sola capa 3D: fondo visible, alfa propio, RGB de seis bits, desplazamiento,
 * efectos y brillo maestro. Después mezcla líneas especializadas y generales. */
static void test_3d_only(gpu2d_ctx *ctx, t_scene *s)
{
	static const uint16_t masters[] = { 0, 0x4000, 0x4009, 0x401F, 0x800F, 0x801F, 0xC01F };
	static const unsigned offsets[] = { 0, 1, 255, 256, 257, 511 };
	static const unsigned targets[] = { 0, 1, 32, 63 };
	gpu2d_engine *e = &s->frame.eng[0];
	unsigned n = 0;

	gpu2d_scene_build(&s->frame, &s->mem, GPU2D_SCENE_EFECTOS, 1);
	const uint16_t original_dc = e->regs[G2D_R_DISPCNT];
	const uint16_t dc = (uint16_t)((original_dc & ~0xFF07u) | G2D_DC_3D | (1u << 8));
	e->regs[G2D_R_DISPCNT] = dc;
	e->nchg = 0;
	e->out_ea = (uint64_t)(uintptr_t)s->out[0];
	for (unsigned i = 0; i < GPU2D_W * GPU2D_H; i++)
		s->img3d[i] = ((i & 31u) << 24) | ((i * 13u & 63u) << 16) |
		               ((i * 7u & 63u) << 8) | (i * 3u & 63u);
	for (unsigned mode = 0; mode < 4; mode++)
		for (unsigned a = 0; a < 4; a++)
			for (unsigned b = 0; b < 4; b++)
				for (unsigned m = 0; m < sizeof masters / sizeof *masters; m++, n++) {
					e->img3d_ea = n % 17 ? (uint64_t)(uintptr_t)s->img3d : 0;
					e->regs[G2D_R_BGHOFS] = (uint16_t)offsets[n % 6];
					e->regs[G2D_R_BLDCNT] = (uint16_t)(targets[a] | mode << 6 | targets[b] << 8);
					e->regs[G2D_R_BLDALPHA] = (uint16_t)(((n * 7) & 31) << 8 | ((n * 13) & 31));
					e->regs[G2D_R_BLDY] = (uint16_t)((n * 11) & 31);
					e->regs[G2D_R_MASTER_BRIGHT] = masters[m];
					gpu2d_ref_engine(e, s->ref[0]);
					CHECK_EQ(gpu2d_compose_engine(ctx, e), 0);
					CHECK(memcmp(s->out[0], s->ref[0], sizeof s->ref[0]) == 0);
					CHECK_EQ(gpu2d_compose_engine_ppe(ctx, e), 0);
					CHECK(memcmp(s->out[0], s->ref[0], sizeof s->ref[0]) == 0);
				}
	/* Cambiar dentro del cuadro activa otras capas, OBJ y ventanas. La vuelta
	 * a BG0 sola debe usar la paleta y los efectos de esa línea. */
	for (unsigned variant = 0; variant < 4; variant++) {
		gpu2d_change *ch = s->mem.chg[0];
		unsigned count = 0;
		const uint16_t general = variant == 0 ? (uint16_t)(dc | G2D_DC_OBJ) :
			variant == 1 ? (uint16_t)(dc | G2D_DC_WIN0) :
			variant == 2 ? (uint16_t)(dc | (1u << 9)) : original_dc;
		e->img3d_ea = (uint64_t)(uintptr_t)s->img3d;
		ch[count++] = (gpu2d_change){ 30, G2D_R_DISPCNT, general };
		ch[count++] = (gpu2d_change){ 60, G2D_R_DISPCNT, dc };
		ch[count++] = (gpu2d_change){ 60, G2D_CHG_PAL, 0x7C1Fu };
		ch[count++] = (gpu2d_change){ 80, G2D_R_BLDCNT, 0x2061u };
		ch[count++] = (gpu2d_change){ 100, G2D_R_MASTER_BRIGHT, 0x401Fu };
		ch[count++] = (gpu2d_change){ 120, G2D_R_BGHOFS, 255 };
		ch[count++] = (gpu2d_change){ 140, G2D_R_DISPCNT, dc | G2D_DC_BLANK };
		ch[count++] = (gpu2d_change){ 160, G2D_R_DISPCNT, dc };
		e->nchg = count;
		gpu2d_ref_engine(e, s->ref[0]);
		CHECK_EQ(gpu2d_compose_engine(ctx, e), 0);
		CHECK_EQ(ctx->stats.map_dropped, 0);
		CHECK(memcmp(s->out[0], s->ref[0], sizeof s->ref[0]) == 0);
		CHECK_EQ(gpu2d_compose_engine_ppe(ctx, e), 0);
		CHECK(memcmp(s->out[0], s->ref[0], sizeof s->ref[0]) == 0);
	}
}

void test_gpu2d(void)
{
	gpu2d_ctx *ctx = amalloc(sizeof *ctx);
	t_scene *s = amalloc(sizeof *s);
	/* huellas de las escenas fijas (referencia); se imprimen con GPU2D_PRINT_HASHES=1 */
	static const uint64_t golden[GPU2D_SCENE_FIJAS] = { 0, 0, 0, 0, 0 };

	scene_init(s);
	CHECK(sizeof(gpu2d_engine) % 16 == 0);
	for (int scene = 0; scene < GPU2D_SCENE_FIJAS; scene++) {
		uint64_t h = 0;
		static uint8_t seen[65536 / 8];
		int colors = 0;

		CHECK_EQ(compare(ctx, s, scene, 1, &h, 0), 0);
		/* la imagen no es trivial... */
		memset(seen, 0, sizeof seen);
		for (int i = 0; i < GPU2D_W * GPU2D_H; i++) {
			if (!(seen[s->ref[0][i] >> 3] & (1u << (s->ref[0][i] & 7)))) {
				seen[s->ref[0][i] >> 3] |= (uint8_t)(1u << (s->ref[0][i] & 7));
				colors++;
			}
		}
		CHECK(colors > 20);
		/* ...y el control negativo: un píxel cambiado se detecta */
		s->out[0][12345] ^= 0x0040;
		{
			int diffs = 0;

			for (int i = 0; i < GPU2D_W * GPU2D_H; i++)
				diffs += s->out[0][i] != s->ref[0][i];
			CHECK_EQ(diffs, 1);
		}
		if (getenv("GPU2D_PRINT_HASHES"))
			printf("HASH escena %d = 0x%016llxull\n", scene, (unsigned long long)h);
		else if (golden[scene])
			CHECK_EQ(h, golden[scene]);
	}
	/* aleatorias */
	{
		int bad = 0;
		const char *e = getenv("GPU2D_SEEDS");
		const uint32_t n = e ? (uint32_t)strtoul(e, NULL, 0) : 120;
		const char *mk = getenv("GPU2D_MASK");
		const uint32_t mask = mk ? (uint32_t)strtoul(mk, NULL, 16) << 20 : 0;

		for (uint32_t seed = 1; seed <= n; seed++) {
			bad += compare(ctx, s, GPU2D_SCENE_ALEATORIA, seed | mask, NULL, 0) != 0;
			bad += compare(ctx, s, GPU2D_SCENE_ALEATORIA, seed | mask, NULL, 1) != 0;
		}
		CHECK_EQ(bad, 0);
		printf("aleatorias: %u escenas, %d motores omitidos por mapas que no caben juntos\n", n, skipped);
		CHECK(skipped < (int)n * 2 / 5 + 2);
	}
	test_effect_policy(ctx, s);
	test_3d_only(ctx, s);
	scene_free(s);
	free(s);
	free(ctx);
}
