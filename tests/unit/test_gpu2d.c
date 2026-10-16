/* SalsaDS — pruebas del compositor 2D (Fase 0.5): la referencia píxel por
 * píxel contra el compositor por líneas (con kernels escalares), en
 * little-endian y en big-endian, y las huellas de las escenas fijas, que
 * tienen que ser las mismas en los dos órdenes de bytes. */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "check.h"
#include "gpu2d/gpu2d_compose.h"

typedef struct {
	gpu2d_scene_mem mem;
	gpu2d_engine eng;
	uint16_t ref[GPU2D_W * GPU2D_H];
	uint16_t out[GPU2D_W * GPU2D_H] __attribute__((aligned(128)));
} t_screen;

static void *amalloc(size_t n)
{
	void *p = NULL;
	if (posix_memalign(&p, 128, (n + 127) & ~(size_t)127))
		abort();
	return p;
}

static void screen_init(t_screen *s, int engine)
{
	s->mem.bg_size = gpu2d_scene_bg_size(engine);
	s->mem.obj_size = gpu2d_scene_obj_size(engine);
	s->mem.chg_cap = GPU2D_SCENE_CHG_CAP;
	s->mem.bg_vram = amalloc(s->mem.bg_size);
	s->mem.obj_vram = amalloc(s->mem.obj_size);
	s->mem.pal = amalloc(1024);
	s->mem.oam = amalloc(1024);
	s->mem.chg = amalloc(sizeof(gpu2d_change) * s->mem.chg_cap);
}

static void screen_free(t_screen *s)
{
	free(s->mem.bg_vram);
	free(s->mem.obj_vram);
	free(s->mem.pal);
	free(s->mem.oam);
	free(s->mem.chg);
}

/* Compara la referencia con el compositor; devuelve las diferencias. */
static int compare(gpu2d_ctx *ctx, t_screen *s, int engine, int scene, uint32_t seed, uint64_t *hash)
{
	gpu2d_scene_build(&s->eng, &s->mem, engine, scene, seed);
	s->eng.out_ea = (uint64_t)(uintptr_t)s->out;
	memset(s->out, 0xA5, sizeof s->out);
	gpu2d_ref_engine(&s->eng, s->ref);
	const int rc = gpu2d_compose_engine(ctx, &s->eng);
	CHECK_EQ(rc, 0);
	int diffs = 0;
	for (int i = 0; i < GPU2D_W * GPU2D_H; i++) {
		if (s->out[i] != s->ref[i] && diffs++ == 0)
			printf("  primera diferencia: escena %d motor %d semilla %u en (%d,%d): compositor 0x%04x, "
			       "referencia 0x%04x\n", scene, engine, seed, i % GPU2D_W, i / GPU2D_W, s->out[i], s->ref[i]);
	}
	if (hash)
		*hash = gpu2d_hash_image(s->ref);
	return diffs;
}

void test_gpu2d(void)
{
	gpu2d_ctx *ctx = amalloc(sizeof *ctx);
	t_screen *s = amalloc(sizeof *s);

	/* Las escenas fijas, en los dos motores (A: 512/256 KB, B: 128/128 KB). */
	static const uint64_t golden[GPU2D_SCENE_ALEATORIA][2] = {
		{ 0xa42f62107136ecc7ull, 0x9c88ae6ca6becf9full },
		{ 0x0139a224ffcf9f47ull, 0x7f7d2d99832537fdull },
		{ 0xfdbdf64ef4eff491ull, 0x9677fef7dc115197ull },
	};
	for (int engine = 0; engine < 2; engine++) {
		screen_init(s, engine);
		for (int scene = 0; scene < GPU2D_SCENE_ALEATORIA; scene++) {
			uint64_t h = 0;
			CHECK_EQ(compare(ctx, s, engine, scene, 1, &h), 0);
			/* La imagen no es trivial... */
			static uint8_t seen[65536 / 8];
			memset(seen, 0, sizeof seen);
			int colors = 0;
			for (int i = 0; i < GPU2D_W * GPU2D_H; i++) {
				if (!(seen[s->ref[i] >> 3] & (1u << (s->ref[i] & 7)))) {
					seen[s->ref[i] >> 3] |= (uint8_t)(1u << (s->ref[i] & 7));
					colors++;
				}
			}
			CHECK(colors > 100);
			/* ...y el control negativo: un píxel cambiado se detecta. */
			s->out[12345] ^= 0x0040;
			int diffs = 0;
			for (int i = 0; i < GPU2D_W * GPU2D_H; i++)
				diffs += s->out[i] != s->ref[i];
			CHECK_EQ(diffs, 1);
			if (getenv("GPU2D_PRINT_HASHES"))
				printf("HASH escena %d motor %d = 0x%016llxull\n", scene, engine, (unsigned long long)h);
			else
				CHECK_EQ(h, golden[scene][engine]);
		}
		/* Aleatorias: configuración, OAM y cambios al azar. */
		int bad = 0;
		for (uint32_t seed = 1; seed <= 60; seed++)
			bad += compare(ctx, s, engine, GPU2D_SCENE_ALEATORIA, seed, NULL) != 0;
		CHECK_EQ(bad, 0);
		screen_free(s);
	}
	free(s);
	free(ctx);
}
