/* SPDX-License-Identifier: GPL-2.0-or-later */
/* SalsaDS — ejecutor de prueba del modo diferido (Fase 3.5). Ver gx_loop.h. */
#include "gpu3d/gx_loop.h"

#include <string.h>

static void fetch(void *ctx, uint32_t pos, uint32_t *dst, uint32_t n)
{
	const gx_loop *lp = ctx;

	memcpy(dst, lp->log + pos, (size_t)n * 4u);
}

static void put_verts(void *ctx, uint32_t first, const gx_vertex *src, uint32_t n)
{
	gx_loop *lp = ctx;

	memcpy(&lp->f->vtx[first], src, (size_t)n * sizeof *src);
}

static void put_polys(void *ctx, uint32_t first, const gx_poly *src, uint32_t n)
{
	gx_loop *lp = ctx;

	memcpy(&lp->f->poly[first], src, (size_t)n * sizeof *src);
}

static uint32_t post(void *ctx, gx_t *g, uint32_t flags, const uint32_t *log, uint32_t nwords, gx_frame *f)
{
	gx_loop *lp = ctx;
	const gx_job_io io = { lp, fetch, put_verts, put_polys };

	lp->log = log;
	lp->f = f;
	gx_job_run(&lp->ex, flags, nwords, &io, lp->vst, GX_LOOP_VCAP, lp->pst, GX_LOOP_PCAP, lp->chunk, GX_LOOP_CHUNK);
	g->ex.out = lp->ex.out;
	lp->jobs++;
	return ++lp->seq;
}

static void wait(void *ctx, gx_t *g, uint32_t seq)
{
	(void)ctx;
	(void)g;
	(void)seq;
}

void gx_loop_attach(gx_t *g, gx_loop *lp)
{
	const gx_exec_ops ops = { lp, post, wait };

	memset(lp, 0, sizeof *lp);
	gx_set_executor(g, &ops);
}
