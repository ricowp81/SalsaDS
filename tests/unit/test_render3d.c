/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * SalsaDS — pruebas del render 3D (Fase 3.2): el rasterizador por bandas (r3d_core.c, el código del SPE con la E/S de
 * memcpy que comprueba las reglas del DMA) contra la referencia (r3d_ref.c) en escenas armadas con el motor de
 * geometría real: los mismos bits con cualquier reparto (bandas de 8 y 16 líneas, de 1 a 3 unidades), sin
 * transferencias inválidas, y el mismo valor en little y big endian (un hash fijo).
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "check.h"
#include "gpu3d/r3d.h"
#include "gpu3d/r3d_core.h"
#include "gpu3d/r3d_scene.h"

#define R3D_GOLDEN_SEED7 0xdf72628eff147546ull /* calculado en LE; BE tiene que dar lo mismo (cambia si cambian las reglas de r3d_px.h) */

static gx_t G;
static uint8_t vram[R3D_VRAM_ZERO_OFF + 128] __attribute__((aligned(128)));
static uint32_t ref[R3D_W * R3D_H] __attribute__((aligned(128)));
static uint32_t out[R3D_W * R3D_H] __attribute__((aligned(128)));
static r3d_ctx ctx;
static int use_bal; /* reparto por costo de las bandas (r3d_balance) en lugar del alterno */

static int render_core(const gx_frame *f, const uint32_t *ts, const uint32_t *ps, uint32_t band, uint32_t units, uint32_t flags,
		       r3d_stats *sum)
{
	r3d_job j;

	memset(&j, 0, sizeof j);
	j.frame_ea = (uint64_t)(uintptr_t)f;
	j.vram_ea = (uint64_t)(uintptr_t)vram;
	j.out_ea = (uint64_t)(uintptr_t)out;
	memcpy(j.texslot, ts, sizeof j.texslot);
	memcpy(j.palslot, ps, sizeof j.palslot);
	j.nunits = units;
	j.band = band;
	if (use_bal && units == 2)
		r3d_balance(f, band, &j, NULL, 0);
	j.flags = flags | (f ? 0u : R3D_F_BLANK);
	memset(out, 0xA5, sizeof out);
	memset(sum, 0, sizeof *sum);
	for (uint32_t u = 0; u < units; u++) {
		r3d_stats st;

		j.unit = u;
		if (r3d_core_render(&ctx, &j, &st) != 0)
			return -1;
		sum->tex_misses += st.tex_misses;
		sum->tex_evict += st.tex_evict;
		sum->tex_px += st.tex_px;
		sum->written += st.written;
	}
	return 0;
}

static int first_diff(void)
{
	for (int i = 0; i < R3D_W * R3D_H; i++)
		if (out[i] != ref[i])
			return i;
	return -1;
}

#ifdef R3D_VERIFY_DECODE
extern uint32_t r3d_decode_errors; /* r3d_core.c compilado con -DR3D_VERIFY_DECODE: bloques contra r3d_decode */
#endif

void test_render3d(void);
void test_render3d(void)
{
	static const uint32_t bands[] = { 16, 8, 16, 16, 16, 8 }, units[] = { 1, 2, 2, 3, 2, 2 };
	static const int bal[] = { 0, 0, 0, 0, 1, 1 };
	const char *env = getenv("R3D_SEEDS");
	const uint32_t nseeds = env ? (uint32_t)atoi(env) : 40;
	uint32_t mism = 0, evict = 0, written = 0, cell_miss = 0;
	uint64_t h0 = 0;

	CHECK(sizeof(r3d_job) <= 112);
	CHECK(r3d_core_ctx_size() < 200u * 1024u);
	for (uint32_t seed = 0; seed < nseeds; seed++) {
		uint32_t ts[R3D_TEX_SLOTS], ps[R3D_PAL_SLOTS];
		const uint32_t np = seed < 4 ? 2000 : 30 + (seed * 37) % 400;
		const gx_frame *f = r3d_scene_build(&G, seed, np, vram, ts, ps);

		r3d_io_errors = 0;
		r3d_render_ref(f, vram, ts, ps, 0, ref);
		if (seed == 7)
			h0 = r3d_hash_image(ref);
		for (int k = 0; k < 12; k++) { /* 6 repartos × (celdas de 4×4, bloques de 256) */
			const uint32_t mode = (k & 1) ? R3D_F_TEXBLOCK : 0u;
			r3d_stats st;
			int d;

			use_bal = bal[k >> 1];
			CHECK_EQ(render_core(f, ts, ps, bands[k >> 1], units[k >> 1], mode, &st), 0);
			d = first_diff();
			if (d >= 0) {
				if (mism++ < 6)
					printf("FALLO render 3D semilla %u, bandas de %u, %u unidades, %s: primer píxel distinto (%d,%d) %08x, ref %08x\n",
					       seed, bands[k >> 1], units[k >> 1], mode ? "bloques" : "celdas", d % R3D_W, d / R3D_W, out[d], ref[d]);
			}
			CHECK(d < 0);
			evict += st.tex_evict;
			written += st.written;
			if (mode == 0)
				cell_miss += st.tex_misses;
		}
		CHECK_EQ(r3d_io_errors, 0);
	}
	/* sin cuadro: transparente */
	{
		uint32_t ts[R3D_TEX_SLOTS] = { 0 }, ps[R3D_PAL_SLOTS] = { 0 };
		r3d_stats st;

		CHECK_EQ(render_core(NULL, ts, ps, 16, 2, 0, &st), 0);
		CHECK_EQ(out[0] | out[R3D_W * R3D_H - 1], 0);
	}
#ifdef R3D_VERIFY_DECODE
	CHECK_EQ(r3d_decode_errors, 0);
	printf("render 3D: bloques de texturas verificados texel a texel contra r3d_decode, %u diferencias\n", r3d_decode_errors);
#endif
	CHECK(evict > 0);   /* la caché de texturas se llenó y desalojó (y la imagen no cambió) */
	CHECK(written > 0);
	CHECK(cell_miss > 0); /* la caché de celdas decodificó celdas */
	/* el mismo cuadro da el mismo valor en LE y en BE (r3d_px.h y el motor de geometría, en enteros) */
	printf("render 3D: %u semillas × 6 repartos × 2 cachés de texturas, %u desalojos de la caché, hash de la semilla 7 %016llx\n", nseeds, evict,
	       (unsigned long long)h0);
	if (nseeds > 7)
		CHECK_EQ(h0, R3D_GOLDEN_SEED7);
}

/* Caché de texturas automática (nds_video_r3d_feedback): bloques hasta que se desbordan, celdas después; histéresis */
static r3d_texpol Nfb;

static void feed(uint32_t req, uint32_t dec, int n)
{
	r3d_stats st;

	memset(&st, 0, sizeof st);
	st.tex_px = req;
	st.tex_misses = dec;
	for (int i = 0; i < n; i++)
		r3d_texpol_feed(&Nfb, &st);
}

void test_r3d_texmode(void);
void test_r3d_texmode(void)
{
	memset(&Nfb, 0, sizeof Nfb);
	CHECK_EQ(Nfb.cells, 0);   /* se empieza en bloques */
	feed(83000, 335, 40);              /* NSMB en un nivel: 1,0 texeles pedidos por texel decodificado: bloques */
	CHECK_EQ(Nfb.cells, 0);
	feed(65000, 3300, 40);             /* Mario Kart DS: 0,08: los bloques se desbordan */
	CHECK_EQ(Nfb.cells, 1);
	feed(65000, 10500, 40);            /* en celdas, 6 pedidos por celda: se queda */
	CHECK_EQ(Nfb.cells, 1);
	feed(50000, 850, 40);              /* SM64DS: 58 pedidos por celda: vuelve a bloques */
	CHECK_EQ(Nfb.cells, 0);
	feed(50000, 59, 40);               /* y ahí se queda (3,3 texeles pedidos por texel decodificado) */
	CHECK_EQ(Nfb.cells, 0);
	feed(0, 0, 10);                    /* sin texturas: no cambia nada */
	CHECK_EQ(Nfb.cells, 0);
	printf("render 3D: caché de texturas automática: bloques en NSMB y SM64DS, celdas en MKDS, sin vaivén\n");
}
