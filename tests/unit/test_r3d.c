/* Rasterizador de tests/phase0/spe-raster3d: el núcleo que corre en el SPE,
 * compilado para el host con la E/S de memcpy (que solo copia al esperar
 * cada DMA), contra la referencia píxel a píxel.
 *
 * - modo id (qué triángulo queda en cada píxel): idéntico a la referencia;
 *   prueba bordes, regla de arriba-izquierda, z-buffer, orden de dibujo,
 *   recortes por banda y tile, reparto entre SPEs y los DMA;
 * - color: dentro de la tolerancia (el núcleo interpola en simple precisión;
 *   la referencia, en doble);
 * - el mismo cuadro con 1 o 2 «SPEs», bandas o tiles: los mismos bits;
 * - con más texturas de las que caben en la caché: misma imagen.
 *
 * Con SALSADS_R3D_IMG=<carpeta> deja las imágenes en PPM. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "check.h"
#include "r3d.h"
#include "r3d_core.h"
#include "r3d_ref.h"
#include "r3d_scene.h"

extern unsigned r3d_io_mem_errors;

#define NPIX_MAX ((size_t)R3D_W1 * R3D_MAX_SCALE * R3D_H1 * R3D_MAX_SCALE)
#define CLEAR_COLOR r3d_rgba(4, 8, 16, 0)
#define CLEAR_DEPTH 0xFFFFFFu

static uint8_t *vram_tex, *vram_pal;
static struct r3d_tri *tris;
static uint32_t *frame, *ref_color, *ref_id, *ref_depth, *first;
static uint8_t *scratch[2];
static struct r3d_texdef defs[R3D_SCENE_MAX_TEX];

static void *alloc128(size_t n)
{
	void *p = NULL;
	if (posix_memalign(&p, 128, n))
		abort();
	memset(p, 0, n);
	return p;
}

static void save_ppm(const char *name, const uint32_t *img, uint32_t w, uint32_t h)
{
	const char *dir = getenv("SALSADS_R3D_IMG");
	if (!dir)
		return;
	char path[512];
	snprintf(path, sizeof path, "%s/%s.ppm", dir, name);
	FILE *f = fopen(path, "wb");
	if (!f)
		return;
	fprintf(f, "P6\n%u %u\n255\n", w, h);
	for (uint32_t i = 0; i < w * h; i++) {
		uint8_t px[3] = { (uint8_t)((img[i] & 63u) << 2), (uint8_t)(((img[i] >> 8) & 63u) << 2),
				  (uint8_t)(((img[i] >> 16) & 63u) << 2) };
		fwrite(px, 1, 3, f);
	}
	fclose(f);
}

struct cfg {
	uint32_t spes, split, tile;
	const char *name;
};

/* Corre el cuadro con `c` (los «SPEs» uno detrás de otro) y deja el
 * resultado en `frame`; suma las estadísticas de todos en `sum`. */
static int run(uint32_t n, uint32_t scale, const struct cfg *c, uint32_t flags, struct r3d_stats *sum)
{
	const uint32_t W = r3d_frame_w(scale), H = r3d_frame_h(scale);
	memset(frame, 0xA5, (size_t)W * H * 4);
	memset(sum, 0, sizeof *sum);
	for (uint32_t i = 0; i < c->spes; i++) {
		struct r3d_job j = {
			.tris_ea = (uintptr_t)tris, .vram_tex_ea = (uintptr_t)vram_tex, .vram_pal_ea = (uintptr_t)vram_pal,
			.frame_ea = (uintptr_t)frame, .scratch_ea = (uintptr_t)scratch[i & 1],
			.vram_tex_size = R3D_VRAM_TEX_SIZE, .vram_pal_size = R3D_VRAM_PAL_SIZE, .ntris = n, .scale = scale,
			.unit_w = c->tile ? 256u : W, .spe_index = i, .spe_count = c->spes, .split = c->split,
			.flags = flags, .clear_color = CLEAR_COLOR, .clear_depth = CLEAR_DEPTH,
		};
		struct r3d_stats st;
		int e = r3d_frame(&j, &st);
		if (e)
			return e;
		sum->tris_mine += st.tris_mine;
		sum->units += st.units;
		sum->unit_tris += st.unit_tris;
		sum->quads += st.quads;
		sum->frags += st.frags;
		sum->tex_miss += st.tex_miss;
		sum->tex_fail += st.tex_fail;
	}
	return 0;
}

static void test_scene(uint32_t n, uint32_t scale, unsigned ntex)
{
	static const struct cfg cfgs[] = {
		{ 1, R3D_SPLIT_INTERLEAVE, 0, "1spe-banda" },
		{ 2, R3D_SPLIT_INTERLEAVE, 0, "2spe-alternas" },
		{ 2, R3D_SPLIT_HALVES, 0, "2spe-mitades" },
		{ 2, R3D_SPLIT_INTERLEAVE, 1, "2spe-tiles" },
		{ 3, R3D_SPLIT_HALVES, 1, "3spe-tiles" },
	};
	const uint32_t W = r3d_frame_w(scale), H = r3d_frame_h(scale), npix = W * H;
	r3d_scene_textures(vram_tex, vram_pal, ntex, defs);
	r3d_scene_tris(tris, n, 0x5A1D5000u + n, defs, ntex);

	struct r3d_ref_in in = { tris, n, vram_tex, R3D_VRAM_TEX_SIZE, vram_pal, R3D_VRAM_PAL_SIZE, scale,
				 CLEAR_COLOR, CLEAR_DEPTH, 1 };
	r3d_ref_render(&in, ref_id, ref_depth);
	in.idmode = 0;
	r3d_ref_render(&in, ref_color, ref_depth);

	char name[96];
	snprintf(name, sizeof name, "ref-%ux-%u-%utex", scale, n, ntex);
	save_ppm(name, ref_color, W, H);

	uint64_t h0 = 0;
	for (size_t k = 0; k < sizeof cfgs / sizeof cfgs[0]; k++) {
		const struct cfg *c = &cfgs[k];
		if (c->tile && scale == 1 && c->spes != 3)
			continue;  /* a 1× la banda ya mide 256 de ancho */
		struct r3d_stats st;
		unsigned errs0 = r3d_io_mem_errors;

		CHECK_EQ(run(n, scale, c, R3D_FLAG_ID, &st), 0);
		uint32_t bad = 0, first_bad = npix;
		for (uint32_t i = 0; i < npix; i++)
			if (frame[i] != ref_id[i] && !bad++)
				first_bad = i;
		if (bad)
			printf("r3d %ux N=%u %u tex %s id: %u píxeles distintos, el primero (%u,%u): %u en vez de %u\n", scale, n,
			       ntex, c->name, bad, first_bad % W, first_bad / W, frame[first_bad], ref_id[first_bad]);
		CHECK_EQ(bad, 0);

		CHECK_EQ(run(n, scale, c, 0, &st), 0);
		struct r3d_cmp cmp;
		r3d_compare(frame, ref_color, npix, &cmp);
		printf("r3d %ux N=%u %2u tex %-13s exactos %6u cerca %5u lejos %4u (%.3f %%), máx %u; "
		       "unidad·tri %u, grupos %u, frags %u, fallos de textura %u\n",
		       scale, n, ntex, c->name, cmp.exact, cmp.close, cmp.far, 100.0 * cmp.far / npix, cmp.maxdiff,
		       st.unit_tris, st.quads, st.frags, st.tex_miss);
		CHECK(cmp.far * 1000u <= npix);            /* ≤ 0,1 % lejos */
		CHECK(cmp.exact * 100u >= npix * 98u);     /* ≥ 98 % idénticos */
		CHECK_EQ(st.tex_fail, 0);
		CHECK(st.frags > npix / 2);
		CHECK_EQ(r3d_io_mem_errors, errs0);
		if (ntex <= 12)
			CHECK(st.tex_miss <= ntex * c->spes);  /* caben todas: una vez cada una */

		const uint64_t h = r3d_hash(frame, npix);
		if (!k) {
			h0 = h;
			memcpy(first, frame, (size_t)npix * 4);
			snprintf(name, sizeof name, "r3d-%ux-%u-%utex", scale, n, ntex);
			save_ppm(name, frame, W, H);
		} else {
			if (h != h0) {
				struct r3d_cmp d;
				r3d_compare(frame, first, npix, &d);
				printf("r3d %ux N=%u %s: distinto de %s en %u píxeles\n", scale, n, c->name, cfgs[0].name,
				       npix - d.exact);
			}
			CHECK_EQ(h, h0);
		}
	}
}

static void test_args(void)
{
	struct r3d_job j = { .tris_ea = (uintptr_t)tris, .vram_tex_ea = (uintptr_t)vram_tex,
			     .vram_pal_ea = (uintptr_t)vram_pal, .frame_ea = (uintptr_t)frame,
			     .scratch_ea = (uintptr_t)scratch[0], .ntris = 0, .scale = 3, .unit_w = 256, .spe_count = 1 };
	struct r3d_stats st;
	CHECK_EQ(r3d_frame(&j, &st), R3D_EARG);
	j.scale = 2;
	j.unit_w = 128;
	CHECK_EQ(r3d_frame(&j, &st), R3D_EARG);
	j.unit_w = 512;
	j.spe_index = 1;
	CHECK_EQ(r3d_frame(&j, &st), R3D_EARG);
	j.spe_index = 0;
	j.frame_ea += 8;
	CHECK_EQ(r3d_frame(&j, &st), R3D_EALIGN);
	j.frame_ea -= 8;
	j.ntris = R3D_MAX_TRIS + 1;
	CHECK_EQ(r3d_frame(&j, &st), R3D_ETRIS);
	j.ntris = 0;  /* cuadro vacío: solo el fondo */
	CHECK_EQ(r3d_frame(&j, &st), 0);
	CHECK_EQ(frame[0], 0);
	CHECK_EQ(st.units, 48);
}

void test_r3d(void)
{
	vram_tex = alloc128(R3D_VRAM_TEX_SIZE + 64);
	vram_pal = alloc128(R3D_VRAM_PAL_SIZE + 64);
	tris = alloc128(R3D_MAX_TRIS * sizeof *tris);
	frame = alloc128(NPIX_MAX * 4 + 64);
	ref_color = alloc128(NPIX_MAX * 4);
	ref_id = alloc128(NPIX_MAX * 4);
	ref_depth = alloc128(NPIX_MAX * 4);
	first = alloc128(NPIX_MAX * 4);
	scratch[0] = alloc128(R3D_MAX_TRIS * R3D_REC_SIZE);
	scratch[1] = alloc128(R3D_MAX_TRIS * R3D_REC_SIZE);

	test_args();
	test_scene(500, 1, 12);
	test_scene(2048, 1, 12);
	test_scene(2048, 1, 24);  /* más texturas que caché: desalojos */
	test_scene(1000, 2, 12);
	test_scene(500, 4, 12);

	free(vram_tex);
	free(vram_pal);
	free(tris);
	free(frame);
	free(ref_color);
	free(ref_id);
	free(ref_depth);
	free(first);
	free(scratch[0]);
	free(scratch[1]);
}

/* Las funciones vectoriales genéricas (las del PPE y el host) contra valores
 * calculados aparte; en la consola, main.c compara las del SPE con estas. */
void test_r3d_selftest(void)
{
	struct r3d_selftest t __attribute__((aligned(16)));
	r3d_selftest(&t);
	const float x[4] = { 1.0f, 3.0f, 0.7f, 123456.0f };
	for (int k = 0; k < 4; k++)
		CHECK(t.recip[k] == 1.0f / x[k]);
	CHECK_EQ(t.mul16[0], 65535u * 65535u);
	CHECK_EQ(t.mul16[1], 21);
	CHECK_EQ(t.mul16[2], 2000000);
	CHECK_EQ(t.mul16[3], 0x5678u * 0xDEF0u);
	CHECK_EQ((uint32_t)t.mul32x16[0], (uint32_t)-28);
	CHECK_EQ((uint32_t)t.mul32x16[1], 0x12345678u * 300u);
	CHECK_EQ((uint32_t)t.mul32x16[2], 327680u * 1300u);
	CHECK_EQ((uint32_t)t.mul32x16[3], (uint32_t)(-2000000000ll * 65535ll));
	CHECK_EQ(t.bytes[0], 0x11223344u ^ 0x44332211u);
	CHECK_EQ(t.bytes[1], 0xA1B2C3D4u ^ 0xD4C3B2A1u);
	CHECK_EQ(t.bytes[2], 0x00FF00FFu ^ 0xFF00FF00u);
	CHECK_EQ(t.bytes[3], 0x80000001u ^ 0x01000080u);
	/* a > b sin signo | con signo << 1 | x > 2 << 2 | máx(a, b) & 0xF0 */
	CHECK_EQ(t.cmp[0], 0u | 0u | 0u | (65535u & 0xF0u));
	CHECK_EQ(t.cmp[1], 0u | 0u | (0xFFFFFFFFu << 2) | (7u & 0xF0u));
	CHECK_EQ(t.cmp[2], 0u | 0u | 0u | (2000u & 0xF0u));
	CHECK_EQ(t.cmp[3], 0u | (0xFFFFFFFFu << 1) | (0xFFFFFFFFu << 2) | (0x9ABCDEF0u & 0xF0u));
	CHECK_EQ(t.cvt[0], 2 + 0);
	CHECK_EQ(t.cvt[1], -2 + 7);
	CHECK_EQ(t.cvt[2], 8191 + 65535);
	CHECK_EQ(t.cvt[3], 540000 + 1000000);
	CHECK_EQ(t.bits, 0xA1u | 0x8Fu << 8);
}
