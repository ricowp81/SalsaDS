/* SPDX-License-Identifier: GPL-2.0-or-later */
/* SalsaDS — DISPCAPCNT: la composición externa síncrona debe escribir lo mismo que el PPE y volver a él si falla. */
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "check.h"
#include "core/nds_priv.h"
#include "gpu2d/gpu2d_compose.h"
#include "gpu2d/gpu2d_scene.h"

struct capture_backend {
	gpu2d_ctx comp;
	int fail;
	int calls;
	uintptr_t engine_ea;
	uintptr_t out_ea;
};

struct capture_run {
	struct nds *nds;
	struct capture_backend backend;
};

static void *amalloc(size_t size)
{
	void *p = NULL;

	if (posix_memalign(&p, 128, (size + 127u) & ~(size_t)127u))
		abort();
	memset(p, 0, size);
	return p;
}

static void scene_init(gpu2d_scene_mem *m)
{
	m->vram = amalloc(G2D_VRAM_SIZE + G2D_VRAM_ZERO);
	m->mmem = amalloc(GPU2D_H * 512u);
	for (int e = 0; e < 2; e++) {
		m->pal[e] = amalloc(1024);
		m->oam[e] = amalloc(1024);
		m->chg[e] = amalloc(sizeof(*m->chg[e]) * (GPU2D_SCENE_CHG_CAP + 2u));
	}
}

static void scene_free(gpu2d_scene_mem *m)
{
	free(m->vram);
	free(m->mmem);
	for (int e = 0; e < 2; e++) {
		free(m->pal[e]);
		free(m->oam[e]);
		free(m->chg[e]);
	}
}

static int compose_external(void *ctx, const gpu2d_engine *engine)
{
	struct capture_backend *b = ctx;

	b->calls++;
	b->engine_ea = (uintptr_t)engine;
	b->out_ea = (uintptr_t)engine->out_ea;
	if (b->fail)
		return -1;
	return gpu2d_compose_engine(&b->comp, engine);
}

static void capture_run(struct capture_run *r, const gpu2d_frame *frame, uint32_t cap, int external, int fail, int classic)
{
	struct nds *n = amalloc(sizeof *n);
	struct nds_vset *s;

	if (!n)
		abort();
	r->nds = n;
	nds_vramcnt_write(n, (cap >> 16) & 3u, 0x80);
	n->vid.capture_classic = classic;
	n->vid.cur = 0;
	n->vid.recording = 1;
	s = &n->vid.set[0];
	s->frame = *frame;
	s->frame.eng[0].regs[G2D_R_DISPCAP] = (uint16_t)cap;
	s->frame.eng[0].regs[G2D_R_DISPCAP + 1] = (uint16_t)(cap >> 16);
	n->io[NDS_CPU9].gen[G2D_R_DISPCAP] = (uint16_t)cap;
	n->io[NDS_CPU9].gen[G2D_R_DISPCAP + 1] = (uint16_t)(cap >> 16);
	if (external) {
		r->backend.fail = fail;
		n->vid.ops.capture = compose_external;
		n->vid.ops.ctx = &r->backend;
	}
	nds_video_frame_end(n);
}

static void capture_free(struct capture_run *r)
{
	free(r->nds);
	r->nds = NULL;
}

static void check_capture_path(const gpu2d_frame *frame)
{
	const uint32_t cap = 0x80000000u | (3u << 20); /* fuente A, 256 x 192, banco A */
	struct capture_run ppe = { 0 }, spe = { 0 }, fallback = { 0 }, classic = { 0 };
	const size_t output_size = GPU2D_W * GPU2D_H * sizeof(uint16_t);

	capture_run(&ppe, frame, cap, 0, 0, 0);
	capture_run(&spe, frame, cap, 1, 0, 0);
	capture_run(&fallback, frame, cap, 1, 1, 0);
	capture_run(&classic, frame, cap, 0, 0, 1);

	CHECK(memcmp(ppe.nds->vram, spe.nds->vram, output_size) == 0);
	CHECK(memcmp(ppe.nds->vram, fallback.nds->vram, output_size) == 0);
	CHECK(memcmp(ppe.nds->vram, classic.nds->vram, output_size) == 0);
	CHECK(memcmp(ppe.nds->vid.capimg, spe.nds->vid.capimg, sizeof ppe.nds->vid.capimg) == 0);
	CHECK(memcmp(ppe.nds->vid.capimg, fallback.nds->vid.capimg, sizeof ppe.nds->vid.capimg) == 0);
	CHECK(memcmp(ppe.nds->vid.capimg, classic.nds->vid.capimg, sizeof ppe.nds->vid.capimg) == 0);
	CHECK_EQ(spe.backend.calls, 1);
	CHECK_EQ(fallback.backend.calls, 1);
	CHECK((spe.backend.engine_ea & 127u) == 0);
	CHECK((spe.backend.out_ea & 127u) == 0);
	CHECK_EQ(ppe.nds->vid.cap.compose, 1);
	CHECK_EQ(ppe.nds->vid.cap.offload, 0);
	CHECK_EQ(spe.nds->vid.cap.compose, 0);
	CHECK_EQ(spe.nds->vid.cap.offload, 1);
	CHECK_EQ(fallback.nds->vid.cap.compose, 1);
	CHECK_EQ(fallback.nds->vid.cap.offload, 0);
	CHECK_EQ(fallback.nds->vid.cap.offload_fail, 1);
	CHECK((ppe.nds->io[NDS_CPU9].gen[G2D_R_DISPCAP + 1] & 0x8000u) == 0);
	CHECK((spe.nds->io[NDS_CPU9].gen[G2D_R_DISPCAP + 1] & 0x8000u) == 0);
	CHECK((fallback.nds->io[NDS_CPU9].gen[G2D_R_DISPCAP + 1] & 0x8000u) == 0);
	CHECK_EQ(ppe.nds->vid.dirty[0], 0xFFu);
	CHECK_EQ(ppe.nds->vid.dirty[1], 0xFFu);
	nds_init(classic.nds);
	CHECK_EQ(classic.nds->vid.capture_classic, 1);

	capture_free(&ppe);
	capture_free(&spe);
	capture_free(&fallback);
	capture_free(&classic);
}

void test_video_capture(void)
{
	static const struct {
		int scene;
		uint32_t seed;
	} cases[] = {
		{ GPU2D_SCENE_TIPICA, 1 },
		{ GPU2D_SCENE_EFECTOS, 2 },
		{ GPU2D_SCENE_ALEATORIA, 0x00034567u },
	};
	gpu2d_scene_mem mem = { 0 };
	gpu2d_frame frame;
	struct capture_run source_b = { 0 };

	scene_init(&mem);
	for (size_t i = 0; i < sizeof cases / sizeof cases[0]; i++) {
		gpu2d_scene_build(&frame, &mem, cases[i].scene, cases[i].seed);
		check_capture_path(&frame);
	}
	/* La fuente B no compone el motor A y por tanto nunca debe enviar un trabajo externo. */
	gpu2d_scene_build(&frame, &mem, GPU2D_SCENE_TIPICA, 3);
	capture_run(&source_b, &frame, 0x80000000u | (1u << 29) | (3u << 20), 1, 0, 0);
	CHECK_EQ(source_b.backend.calls, 0);
	CHECK_EQ(source_b.nds->vid.cap.compose, 0);
	CHECK_EQ(source_b.nds->vid.cap.offload, 0);
	CHECK((source_b.nds->io[NDS_CPU9].gen[G2D_R_DISPCAP + 1] & 0x8000u) == 0);
	capture_free(&source_b);
	scene_free(&mem);
}
