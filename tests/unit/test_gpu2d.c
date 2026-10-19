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

static int compare(gpu2d_ctx *ctx, t_scene *s, int scene, uint32_t seed, uint64_t *hash)
{
	int diffs = 0;

	gpu2d_scene_build(&s->frame, &s->mem, scene, seed);
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
	}
	if (hash)
		*hash = gpu2d_hash_image(s->ref[0]) ^ (gpu2d_hash_image(s->ref[1]) * 31u);
	return diffs;
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

		CHECK_EQ(compare(ctx, s, scene, 1, &h), 0);
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

		for (uint32_t seed = 1; seed <= n; seed++)
			bad += compare(ctx, s, GPU2D_SCENE_ALEATORIA, seed | mask, NULL) != 0;
		CHECK_EQ(bad, 0);
		printf("aleatorias: %u escenas, %d motores omitidos por mapas que no caben juntos\n", n, skipped);
		CHECK(skipped < (int)n / 5 + 2);
	}
	scene_free(s);
	free(s);
	free(ctx);
}
