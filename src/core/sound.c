/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * SalsaDS — sonido de la máquina (Fase 2.2): los registros del ARM7 (0x04000400-0x0400051F) y el reparto del trabajo al
 * mezclador de src/audio. Ver el encabezado de audio/snd.h para el modelo.
 *
 * Cada escritura a un registro de sonido se anota con el tick en que se hizo (ciclos del ARM7 / 1024). Al cerrar un cuadro
 * de video se arma un trabajo con las escrituras que ya tienen efecto en los ticks de ese cuadro y se manda al mezclador
 * (en la PS3, a un SPE; en el host, se corre en el acto). El resultado (qué canales siguen encendidos, la salida de
 * audio) se recoge al cerrar el cuadro siguiente. Lo que el ARM7 lee de vuelta (el bit 31 de SOUNDxCNT, el de SNDCAPxCNT)
 * es, por eso, el estado del último trabajo recogido más los arranques que él mismo pidió desde entonces.
 *
 * Fuente: GBATEK (DS Sound). Código propio.
 */
#include <string.h>

#include "core/nds_priv.h"

/* ---- bus del mezclador en el host ------------------------------------------------------------------------ */

static void host_fetch(void *ctx, uint32_t addr, uint8_t *dst)
{
	struct nds *n = ctx;
	const struct nds_rgn *r = &n->rgn_rd[NDS_CPU7][(addr >> 23) & 511u];

	if (r->p)
		memcpy(dst, r->p + (addr & r->mask), SND_LINE);
	else
		memset(dst, 0, SND_LINE);
}

static void host_store(void *ctx, uint32_t addr, const uint8_t *src, uint32_t len)
{
	struct nds *n = ctx;
	const struct nds_rgn *r = &n->rgn_wr[NDS_CPU7][(addr >> 23) & 511u];

	if (r->p) {
		memcpy(r->p + (addr & r->mask), src, len);
		nds_code_write_range(n, r->p + (addr & r->mask), len);
	}
}

void nds_snd_init(struct nds *n)
{
	struct nds_snd *s = &n->snd;
	const struct nds_snd_ops ops = s->ops;
	const uint32_t mode = s->mode;

	memset(s, 0, sizeof *s);
	s->ops = ops;
	snd_reset(&s->st);
	s->mode = mode;
	s->cur = 0;
	s->prev = -1;
	s->bias = 0x200;
}

void nds_snd_set_mode(struct nds *n, uint32_t mode)
{
	n->snd.mode = mode;
}

void nds_snd_set_ratio_adj(struct nds *n, float adj)
{
	n->snd.ratio_adj = adj;
}

/* ---- registros ----------------------------------------------------------------------------------------------- */

uint16_t nds_snd_read(struct nds *n, uint32_t off)
{
	const struct nds_snd *s = &n->snd;

	if (off < 0x100u) {
		const unsigned c = off >> 4;

		switch (off & 15u) {
		case 0:
			return (uint16_t)s->cnt[c];
		case 2:
			return (uint16_t)(((s->cnt[c] >> 16) & 0x7FFFu) | ((s->busy >> c) & 1u ? 0x8000u : 0u));
		default:
			return 0;
		}
	}
	switch (off) {
	case 0x100:
		return s->soundcnt;
	case 0x104:
		return s->bias;
	case 0x108:
		return (uint16_t)((s->capcnt[0] & 0x0Fu) | ((s->capcnt[1] & 0x0Fu) << 8) | ((s->busy >> 16 & 1u) ? 0x80u : 0u) |
				  ((s->busy >> 17 & 1u) ? 0x8000u : 0u));
	default:
		return 0;
	}
}

void nds_snd_write(struct nds *n, uint32_t off, uint16_t v, uint16_t mask)
{
	struct nds_snd *s = &n->snd;
	snd_ent *e;

	if (off >= 0x120u)
		return;
	/* lo que se lee de vuelta */
	if (off < 0x100u) {
		const unsigned c = off >> 4;

		if ((off & 15u) == 0) {
			s->cnt[c] = (s->cnt[c] & ~(uint32_t)mask) | (v & mask);
		} else if ((off & 15u) == 2) {
			const uint32_t m = mask & 0x7FFFu;

			s->cnt[c] = (s->cnt[c] & 0xFFFFu) | ((((s->cnt[c] >> 16) & ~m) | (v & m)) << 16);
			if (mask & 0x8000u) {
				if (v & 0x8000u)
					s->busy |= 1u << c;
				else
					s->busy &= ~(1u << c);
			}
		}
	} else if (off == 0x100u) {
		s->soundcnt = (uint16_t)(((s->soundcnt & ~mask) | (v & mask)) & 0xBF7Fu);
	} else if (off == 0x104u) {
		s->bias = (uint16_t)(((s->bias & ~mask) | (v & mask)) & 0x3FFu);
	} else if (off == 0x108u) {
		for (int i = 0; i < 2; i++) {
			const uint32_t bm = (mask >> (8 * i)) & 0xFFu, bv = (v >> (8 * i)) & 0xFFu;

			if (bm) {
				const uint32_t nw = (s->capcnt[i] & ~bm) | (bv & bm);

				s->capcnt[i] = (uint8_t)(nw & 0x0Fu);
				if (bv & bm & 0x80u)
					s->busy |= 1u << (16 + i);
				else if (bm & 0x80u)
					s->busy &= ~(1u << (16 + i));
			}
		}
	}
	if (s->nplog >= SND_LOG_MAX) {
		s->dropped++;
		return;
	}
	e = &s->plog[s->nplog++];
	e->tick = (uint32_t)(n->a7.cycles >> 10);
	e->off = (uint16_t)off;
	e->val = v;
	e->mask = mask;
	e->pad = 0;
	e->pad2 = 0;
}

/* ---- trabajos -------------------------------------------------------------------------------------------------- */

static void collect(struct nds *n, int set)
{
	struct nds_snd *s = &n->snd;
	struct nds_snd_job *j = &s->job[set];

	/* lo que el mezclador dice que sigue encendido, salvo lo que el ARM7 arrancó o paró después del corte (las
	 * escrituras que todavía esperan en `plog`): de esas manda el ARM7 */
	uint32_t touched = 0;

	for (uint32_t i = 0; i < s->nplog; i++) {
		const snd_ent *e = &s->plog[i];

		if (e->off < 0x100u && (e->off & 15u) == 2u && (e->mask & 0x8000u))
			touched |= 1u << (e->off >> 4);
		else if (e->off == 0x108u) {
			if (e->mask & 0x0080u)
				touched |= 1u << 16;
			if (e->mask & 0x8000u)
				touched |= 1u << 17;
		}
	}
	s->busy = (s->st.busy & ~touched) | (s->busy & touched);
	s->applied += j->nlog;
	if (s->ops.sink)
		s->ops.sink(s->ops.ctx, j->out, j->out_n, j->nat, j->nticks);
}

void nds_snd_frame(struct nds *n)
{
	struct nds_snd *s = &n->snd;
	struct nds_snd_job *j;
	const uint64_t target = n->time / SND_TICK_ARM9; /* muestras cuyo intervalo ya terminó */
	uint32_t nticks, k;

	if (s->prev >= 0) {
		if (s->ops.submit && s->ops.wait)
			s->ops.wait(s->ops.ctx, s->prev);
		collect(n, s->prev);
		s->prev = -1;
	}
	if (target <= s->tick_sub)
		return;
	nticks = (uint32_t)(target - s->tick_sub);
	if (nticks > SND_TICKS_MAX)
		nticks = SND_TICKS_MAX;
	j = &s->job[s->cur];
	/* escrituras que tienen efecto en estos ticks: tick < T para algún T en [tick_sub, tick_sub + nticks) */
	for (k = 0; k < s->nplog && (uint64_t)s->plog[k].tick + 2u <= s->tick_sub + nticks; k++)
		;
	memcpy(j->log, s->plog, k * sizeof(snd_ent));
	memmove(s->plog, s->plog + k, (s->nplog - k) * sizeof(snd_ent));
	s->nplog -= k;
	j->nlog = k;
	j->nticks = nticks;
	j->mode = s->mode;
	j->ratio_adj = s->ratio_adj;
	j->out_n = 0;
	for (int i = 0; i < 4; i++) {
		const struct nds_rgn *rr = &n->rgn_rd[NDS_CPU7][4 + i], *rw = &n->rgn_wr[NDS_CPU7][4 + i];

		j->rgn[i].rd = (uint64_t)(uintptr_t)rr->p;
		j->rgn[i].wr = (uint64_t)(uintptr_t)rw->p;
		j->rgn[i].mask = rr->mask;
	}
	s->tick_sub += nticks;
	s->tasks++;
	if (s->ops.submit) {
		s->ops.submit(s->ops.ctx, s->cur, j, &s->st);
	} else {
		snd_bus bus = { n, host_fetch, host_store };

		s->st.mode = j->mode;
		s->st.ratio_adj = j->ratio_adj;
		snd_run(&s->st, j->log, j->nlog, j->nticks, &bus, j->nat, j->out, SND_OUT_MAX, &j->out_n);
	}
	s->prev = s->cur;
	s->cur ^= 1;
}
