/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * SalsaDS — motor de geometría de la DS: FIFO, tiempo, GXSTAT, pilas (punteros y error) y cambio de búfer (Fase 3.1; la
 * ejecución de las órdenes pasó a gx_exec.c en la Fase 3.5). Ver gx.h para el diseño y el tiempo.
 *
 * Código propio escrito desde GBATEK; las decisiones donde GBATEK no alcanza (y que se tomaron comparando con melonDS
 * ejecutándolo, nunca leyendo su código) están en docs/fase3.md. Lo que no se pudo contrastar en una DS real se marca
 * «[sin contrastar]» en los comentarios.
 */
#include "gpu3d/gx.h"

#include <string.h>

#include "util/endian.h"

/* ---- inicio y ejecutor ------------------------------------------------------------------------------------------ */

static void exec_point_at_frame(gx_t *g)
{
	g->ex.vtx = g->frame[g->bld].vtx;
	g->ex.poly = g->frame[g->bld].poly;
	g->ex.vbase = g->ex.pbase = 0;
}

void gx_init(gx_t *g)
{
	uint64_t (*const clock)(void) = g->clock;
	const gx_exec_ops ops = g->ops;
	const int deferred = g->deferred;

	if (deferred && ops.wait && g->last_seq)
		ops.wait(ops.ctx, g, g->last_seq); /* ningún trabajo debe seguir leyendo el registro */
	memset(g, 0, sizeof *g);
	g->clock = clock;
	g->ops = ops;
	g->deferred = deferred;
	gx_exec_init(&g->ex);
	g->pk_idx = 4;
	g->front = -1;
	g->nxt = GX_NEVER;
	exec_point_at_frame(g);
	if (deferred) {
		g->mats_valid = 1;
		g->last_seq = ops.post(ops.ctx, g, GX_JOB_RESET, g->log, 0, &g->frame[g->bld]);
	}
}

void gx_set_executor(gx_t *g, const gx_exec_ops *ops)
{
	g->ops = *ops;
	g->deferred = 1;
	gx_init(g);
}

/* Publica lo anotado y no publicado. */
void gx_kick(gx_t *g)
{
	uint32_t flags = 0;

	if (!g->deferred || g->log_wr == g->log_sub)
		return;
	if (g->newframe_pending) {
		flags |= GX_JOB_NEWFRAME;
		g->newframe_pending = 0;
	}
	g->last_seq = g->ops.post(g->ops.ctx, g, flags, g->log + g->log_sub, g->log_wr - g->log_sub, &g->frame[g->bld]);
	g->log_sub = g->log_wr;
}

void gx_sync(gx_t *g, int mats)
{
	if (!g->deferred)
		return;
	if (g->dirty || (mats && !g->mats_valid) || g->newframe_pending) {
		uint32_t flags = (mats ? GX_JOB_MATS : 0u) | (g->newframe_pending ? GX_JOB_NEWFRAME : 0u);

		/* lo que quede por publicar sale en un trabajo con las banderas que haga falta */
		g->newframe_pending = 0;
		g->last_seq = g->ops.post(g->ops.ctx, g, flags, g->log + g->log_sub, g->log_wr - g->log_sub, &g->frame[g->bld]);
		g->log_sub = g->log_wr;
		g->ops.wait(g->ops.ctx, g, g->last_seq);
		g->dirty = g->dirty_box = g->dirty_test = 0;
		g->mats_valid = mats;
	}
}

/* ---- cola, tiempo y registros ------------------------------------------------------------------------------- */

int gx_irq_level(const gx_t *g)
{
	const uint32_t lv = gx_fifo_level(g);

	switch (g->irqmode) {
	case 1: return lv < GX_FIFO_ENTRIES / 2;
	case 2: return lv == 0;
	default: return 0;
	}
}

uint64_t gx_next_start(const gx_t *g)
{
	if (g->swap_pending || g->qcount == 0)
		return GX_NEVER;
	{
		const unsigned ne = gx_cmd_entries(g->q[g->qhead].cmd);
		uint64_t t, cf;

		if (g->qcount < ne)
			return GX_NEVER;
		t = g->q[(g->qhead + ne - 1) & (GX_QRING - 1u)].t;
		cf = g->cycle_free;
		return t > cf ? t : cf;
	}
}

/* Lo que una orden cambia en el motor de tiempo: el modo de matriz, los punteros de las pilas y su error, POLYGON_ATTR (para
 * el coste de NORMAL) y el SWAP_BUFFERS. La ejecución de verdad (gx_exec_cmd) hace lo mismo con gx_sp_* y además mueve las
 * matrices. */
static void fe_track(gx_t *g, uint32_t cmd, const uint32_t *p)
{
	uint32_t idx;
	int32_t sidx;

	switch (cmd) {
	case 0x10:
		g->fe.mode = p[0] & 3u;
		break;
	case 0x11:
		if (!gx_sp_push(&g->fe))
			g->err = 1;
		break;
	case 0x12:
		if (!gx_sp_pop(&g->fe, p[0], &sidx))
			g->err = 1;
		break;
	case 0x13:
	case 0x14:
		if (!gx_sp_store(&g->fe, p[0], &idx))
			g->err = 1;
		break;
	case 0x29:
		g->fe_polyattr = p[0];
		break;
	case 0x50:
		g->swap_pending = 1;
		g->swap_param = p[0] & 3u;
		break;
	default:
		break;
	}
}

/* Anota la orden en el registro del modo diferido. */
static void log_append(gx_t *g, uint32_t cmd, const uint32_t *p, unsigned np)
{
	if (g->log_wr + 1u + np > GX_LOG_WORDS) {
		/* registro lleno: se publica lo pendiente, se espera a que el ejecutor lo termine y se vuelve al inicio */
		gx_kick(g);
		if (g->last_seq)
			g->ops.wait(g->ops.ctx, g, g->last_seq);
		g->log_wr = g->log_sub = 0;
	}
	g->log[g->log_wr++] = cmd;
	for (unsigned i = 0; i < np; i++)
		g->log[g->log_wr++] = p[i];
	g->dirty = 1;
	g->mats_valid = 0;
	if (cmd == 0x70)
		g->dirty_box = 1;
	else if (cmd == 0x71 || cmd == 0x72)
		g->dirty_test = 1;
}

void gx_advance_run(gx_t *g, uint64_t now)
{
	const uint64_t c0 = g->clock ? g->clock() : 0;

	for (;;) {
		uint32_t p[32];
		uint64_t start;
		unsigned ne, cost, np;
		uint8_t cmd;

		start = gx_next_start(g);
		if (start == GX_NEVER || start > now)
			break;
		cmd = g->q[g->qhead].cmd;
		ne = gx_cmd_entries(cmd);
		np = gx_cmd_np(cmd);
		for (unsigned i = 0; i < np; i++)
			p[i] = g->q[(g->qhead + i) & (GX_QRING - 1u)].param;
		if (cmd >= 0x70 && cmd <= 0x72)
			g->test_pending = g->test_pending >= ne ? g->test_pending - ne : 0u;
		g->qhead = (g->qhead + ne) & (GX_QRING - 1u);
		g->qcount -= ne;
		cost = gx_cmd_cost(cmd, g->fe.mode, g->fe_polyattr);
		g->st.cmds++;
		g->st.by_cmd[cmd & 0x7Fu]++;
		fe_track(g, cmd, p);
		if (g->deferred)
			log_append(g, cmd, p, np);
		else
			gx_exec_cmd(&g->ex, cmd, p);
		g->cycle_free = start + 2ull * cost;
		g->st.cycles += 2ull * cost;
		if (cmd == 0x11 || cmd == 0x12 || cmd == 0x13 || cmd == 0x14 || cmd == 0x15)
			g->stack_busy_until = g->cycle_free;
		if (cmd >= 0x70 && cmd <= 0x72)
			g->test_busy_until = g->cycle_free;
	}
	g->nxt = gx_next_start(g);
	if (g->deferred && g->log_wr - g->log_sub >= GX_KICK_WORDS)
		gx_kick(g);
	if (g->clock)
		g->st.host_ticks += g->clock() - c0;
}

static uint64_t q_push(gx_t *g, uint64_t now, uint8_t cmd, uint32_t param)
{
	uint64_t t = now;

	while (g->qcount >= GX_QUEUE_MAX) {
		const uint64_t ns = gx_next_start(g);

		if (ns == GX_NEVER) { /* parado en un SWAP_BUFFERS (o con una orden incompleta): la escritura se pierde */
			g->st.fifo_overflow++;
			return t;
		}
		if (ns > t)
			t = ns;
		gx_advance(g, t);
	}
	{
		gx_qent *e = &g->q[(g->qhead + g->qcount) & (GX_QRING - 1u)];

		e->cmd = cmd;
		e->param = param;
		e->t = t;
		g->qcount++;
		if (cmd >= 0x70 && cmd <= 0x72)
			g->test_pending++;
	}
	if (g->nxt == GX_NEVER) /* con la orden de la cabeza completa, otra entrada no cambia cuándo empieza */
		g->nxt = gx_next_start(g);
	return t;
}

/* Siguiente orden no nula de la palabra empaquetada; las que no llevan parámetros entran de una vez. */
static uint64_t pk_advance(gx_t *g, uint64_t now, uint64_t stall)
{
	while (g->pk_idx < 4) {
		const uint32_t c = (g->pk_word >> (8u * g->pk_idx)) & 0xFFu;
		const unsigned np = c < 0x80 ? gx_cmd_np(c) : 0xFFu;

		if (c == 0) {
			g->pk_idx++;
			continue;
		}
		if (np == 0xFFu) { /* orden inexistente: se ignora */
			g->st.unknown_cmds++;
			g->pk_idx++;
			continue;
		}
		if (np == 0) {
			const uint64_t t = q_push(g, now > stall ? now : stall, (uint8_t)c, 0);

			if (t > stall)
				stall = t;
			g->pk_idx++;
			continue;
		}
		g->pk_cmd = (uint8_t)c;
		g->pk_left = np;
		return stall;
	}
	g->pk_left = 0;
	return stall;
}

static inline __attribute__((always_inline)) uint64_t fifo_write_i(gx_t *g, uint64_t now, uint32_t w)
{
	uint64_t stall = now;

	gx_advance(g, now);
	if (g->pk_left) {
		const uint64_t t = q_push(g, now, g->pk_cmd, w);

		if (t > stall)
			stall = t;
		if (--g->pk_left == 0) {
			g->pk_idx++;
			stall = pk_advance(g, stall, stall);
		}
	} else {
		g->pk_word = w;
		g->pk_idx = 0;
		stall = pk_advance(g, now, stall);
	}
	return stall;
}

uint64_t gx_fifo_write(gx_t *g, uint64_t now, uint32_t w)
{
	return fifo_write_i(g, now, w);
}

uint32_t gx_dma_burst(gx_t *g, uint64_t *cycles, const uint8_t *src, uint32_t count, int on, const uint64_t *ev_gx,
		      void (*sched)(void *ctx, uint64_t t), void *ctx)
{
	uint64_t t = *cycles;
	uint32_t i;

	for (i = 0; i < count; i++) {
		gx_advance(g, t);
		if (gx_fifo_level(g) >= GX_FIFO_ENTRIES / 2)
			break;
		if (on) {
			const uint64_t st = fifo_write_i(g, t, rd32le(src + 4u * i));

			if (st > t)
				t = st;
			if (*ev_gx == GX_NEVER)
				sched(ctx, t);
		}
		t += 4u;
	}
	*cycles = t;
	return i;
}

uint64_t gx_port_write(gx_t *g, uint64_t now, uint32_t off, uint32_t w)
{
	const uint32_t cmd = (off - 0x400u) >> 2;

	gx_advance(g, now);
	if (off < 0x440u || off > 0x5CCu || !gx_cmd_valid(cmd) || cmd == 0)
		return now;
	return q_push(g, now, (uint8_t)cmd, gx_cmd_np(cmd) ? w : 0u);
}

uint32_t gx_gxstat(gx_t *g, uint64_t now)
{
	uint32_t v = 0;
	const uint32_t lv = gx_fifo_level(g);

	gx_advance(g, now);
	if (g->dirty_box)
		gx_sync(g, 0); /* el resultado de BOX_TEST sale de la ejecución */
	if (g->test_pending || g->test_busy_until > now)
		v |= GXSTAT_TEST_BUSY;
	if (g->ex.out.box_result)
		v |= GXSTAT_BOX_RESULT;
	v |= (g->fe.sp_pos & 0x1Fu) << 8;
	v |= (g->fe.sp_proj & 1u) << 13;
	if (g->stack_busy_until > now)
		v |= GXSTAT_STACK_BUSY;
	if (g->err)
		v |= GXSTAT_STACK_ERR;
	v |= lv << 16;
	if (lv < GX_FIFO_ENTRIES / 2)
		v |= GXSTAT_HALF;
	if (lv == 0)
		v |= GXSTAT_EMPTY;
	if (g->qcount || g->cycle_free > now || g->swap_pending)
		v |= GXSTAT_BUSY;
	v |= g->irqmode << 30;
	return v;
}

void gx_gxstat_write(gx_t *g, uint32_t v, uint32_t mask)
{
	if ((mask & 0x8000u) && (v & 0x8000u))
		g->err = 0;
	if (mask & 0xC0000000u)
		g->irqmode = (v >> 30) & 3u;
}

uint32_t gx_ram_count(gx_t *g)
{
	if (g->dirty)
		gx_sync(g, 0);
	return (g->ex.out.npolys & 0xFFFu) | ((g->ex.out.nverts & 0x1FFFu) << 16);
}

uint32_t gx_read32(gx_t *g, uint64_t now, uint32_t off)
{
	gx_advance(g, now);
	if (off == 0x600u)
		return gx_gxstat(g, now);
	if (off == 0x604u)
		return gx_ram_count(g);
	if (off >= 0x620u && off <= 0x634u) {
		if (g->dirty_test)
			gx_sync(g, 0);
		if (off <= 0x62Cu)
			return (uint32_t)g->ex.out.pos_result[(off - 0x620u) >> 2];
		if (off == 0x630u)
			return (uint32_t)(uint16_t)g->ex.out.vec_result[0] | ((uint32_t)(uint16_t)g->ex.out.vec_result[1] << 16);
		if (off == 0x634u)
			return (uint32_t)(uint16_t)g->ex.out.vec_result[2];
		return 0;
	}
	if (off >= 0x640u && off <= 0x6A0u) {
		if (g->deferred) {
			gx_sync(g, 1);
		} else {
			gx_exec_flush_mats(&g->ex);
		}
		if (off <= 0x67Cu)
			return (uint32_t)g->ex.out.clip[(off - 0x640u) >> 2];
		if (off >= 0x680u) {
			const uint32_t k = (off - 0x680u) >> 2;

			return (uint32_t)g->ex.out.vec[(k / 3u) * 4u + k % 3u];
		}
	}
	return 0;
}

int gx_swap_pending(const gx_t *g)
{
	return g->swap_pending;
}

const gx_frame *gx_swap(gx_t *g, uint64_t now, const gx_regs *regs)
{
	gx_frame *f = &g->frame[g->bld];

	if (now > g->now)
		g->now = now;
	if (g->cycle_free < now)
		g->cycle_free = now; /* lo que sigue al SWAP_BUFFERS arranca en el VBlank */
	/* todo lo que el motor ejecutó hasta aquí tiene que estar en el cuadro */
	gx_sync(g, 0);
	f->npolys = g->ex.out.npolys;
	f->nverts = g->ex.out.nverts;
	f->dropped_polys = g->ex.out.dropped_polys;
	f->dropped_verts = g->ex.out.dropped_verts;
	g->st.frame_polys += f->npolys;
	g->st.frame_verts += f->nverts;
	if (f->npolys > g->st.max_polys)
		g->st.max_polys = f->npolys;
	if (f->nverts > g->st.max_verts)
		g->st.max_verts = f->nverts;

	f->swap_params = g->swap_param;
	if (regs)
		f->regs = *regs;
	g->swaps++;
	f->seq = g->swaps;
	g->front = g->bld;
	g->bld = (g->bld + 1) % 3; /* ni el entregado ni el anterior (que el render 3D todavía lee) */
	g->swap_pending = 0;
	g->st.swaps++;
	g->nxt = gx_next_start(g);
	gx_exec_newframe(&g->ex); /* el contador de RAM_COUNT vuelve a 0 (en el modo diferido, también el espejo) */
	exec_point_at_frame(g);
	if (g->deferred)
		g->newframe_pending = 1;
	return f;
}

const gx_frame *gx_front(const gx_t *g)
{
	return g->front < 0 ? NULL : &g->frame[g->front];
}

const gx_stats *gx_stats_get(gx_t *g)
{
	gx_sync(g, 0);
	g->st.polys = g->ex.out.st.polys;
	g->st.culled = g->ex.out.st.culled;
	g->st.clipped = g->ex.out.st.clipped;
	g->st.rejected = g->ex.out.st.rejected;
	g->st.rej_outside = g->ex.out.st.rej_outside;
	g->st.rej_far = g->ex.out.st.rej_far;
	g->st.rej_empty = g->ex.out.st.rej_empty;
	g->st.dropped_poly = g->ex.out.st.dropped_poly;
	g->st.dropped_vert = g->ex.out.st.dropped_vert;
	return &g->st;
}
