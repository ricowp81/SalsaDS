/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * SalsaDS — pegamento del motor de geometría (src/gpu3d/gx.c) con la máquina: escrituras a GXFIFO y a los puertos de
 * órdenes, lectura de GXSTAT y de los resultados, IRQ 21 por nivel, DMA de modo 7 con la FIFO, y el cambio de búfer
 * de SWAP_BUFFERS en el VBlank (Fase 3.1; docs/fase3.md).
 */
#include <string.h>

#include "core/nds_priv.h"

#define GX_ON(n) ((n)->powcnt1 & 0x0008u) /* POWCNT1 bit 3: motor de geometría encendido */

void nds_gx_init(struct nds *n)
{
	gx_init(&n->gx);
}

/* El motor corre a su ritmo en el tiempo de la CPU que lo toca. Una escritura que llena la FIFO frena a esa CPU. */
static void stall_cpu9(struct nds *n, uint64_t until)
{
	struct acore *c = nds_core(n, NDS_CPU9);

	if (until > c->cycles)
		c->cycles = until;
}

void nds_gx_fifo_write(struct nds *n, uint32_t w)
{
	if (!GX_ON(n))
		return;
	stall_cpu9(n, gx_fifo_write(&n->gx, nds_cpu_now(n, NDS_CPU9), w));
	nds_gx_schedule(n, NDS_GX_POLL);
}

void nds_gx_port_write(struct nds *n, uint32_t off, uint32_t w)
{
	if (!GX_ON(n))
		return;
	stall_cpu9(n, gx_port_write(&n->gx, nds_cpu_now(n, NDS_CPU9), off, w));
	nds_gx_schedule(n, NDS_GX_POLL);
}

uint32_t nds_gx_read32(struct nds *n, uint32_t off)
{
	return gx_read32(&n->gx, nds_cpu_now(n, NDS_CPU9), off);
}

void nds_gx_gxstat_write(struct nds *n, uint32_t v, uint32_t mask)
{
	const uint32_t old = n->gx.irqmode;

	gx_gxstat_write(&n->gx, v, mask);
	if (n->gx.irqmode != old)
		nds_gx_schedule(n, 64u);
}

/* ¿La FIFO tiene sitio para la DMA de modo 7 (menos de la mitad)? `t` es el instante de ese acceso. */
int nds_gx_dma_ok(struct nds *n, uint64_t t)
{
	gx_advance(&n->gx, t);
	return gx_fifo_level(&n->gx) < GX_FIFO_ENTRIES / 2;
}

/* El motor corre perezoso (gx_advance en cada acceso); el evento EV_GX sólo hace falta para lo que no depende de que la CPU
 * lo mire: la IRQ 21 por nivel y los avisos a la DMA de modo 7 cuando la FIFO baja de la mitad. Sin ninguna de las dos
 * cosas no hay evento (antes había uno por orden: ≈ 16 000 por cuadro en New Super Mario Bros.).
 *
 * Con alguna de las dos, el evento era una mirada cada NDS_GX_POLL ciclos (≈ 4400 por cuadro, 9 de cada 10 de todos los
 * eventos). Ahora las miradas que no pueden hacer nada se saltan: la condición (nivel de la FIFO) sólo cambia cuando el motor
 * empieza una orden, o cuando se cumple el periodo de la IRQ si el nivel ya está; el evento va al primer punto de la rejilla
 * de NDS_GX_POLL ciclos (anclada en la última mirada, como antes) que llega a ese instante. El resultado es el mismo que
 * mirando en todos los puntos. */
static int gx_needs_poll(const struct nds *n)
{
	return n->gx.irqmode != 0 || nds_dma_waits(n, NDS_CPU9, NDS_DMA_GXFIFO);
}

/* Primer instante en que una mirada podría hacer algo, o NDS_NEVER. `now` es el instante de la CPU o del evento. */
static uint64_t gx_poll_need(const struct nds *n, uint64_t now)
{
	const gx_t *g = &n->gx;
	uint64_t need = NDS_NEVER;
	const uint64_t ns = gx_next_start(g);

	if (g->irqmode) {
		if (gx_irq_level(g))
			need = n->gx_irq_t + NDS_GX_PERIOD;
		else if (ns < need)
			need = ns;
	}
	if (nds_dma_waits(n, NDS_CPU9, NDS_DMA_GXFIFO)) {
		if (gx_fifo_level(g) < GX_FIFO_ENTRIES / 2)
			need = now < need ? now : need;
		else if (ns < need)
			need = ns;
	}
	return need;
}

static void gx_poll_set(struct nds *n, uint64_t now, uint64_t first)
{
	uint64_t need = gx_poll_need(n, now), when;

	if (need == NDS_NEVER)
		return;
	if (need < first)
		need = first;
	when = n->gx_anchor;
	if (need > when)
		when += ((need - when + NDS_GX_POLL - 1u) / NDS_GX_POLL) * NDS_GX_POLL;
	if (when < n->ev[EV_GX])
		nds_event_set(n, EV_GX, when);
}

void nds_gx_schedule(struct nds *n, uint32_t delay)
{
	uint64_t now;

	/* con una mirada ya pendiente no hace falta nada: lo que escribe la CPU no adelanta el instante en que el nivel de la FIFO
	 * puede cambiar (la mirada pendiente, si llega antes de tiempo, recalcula) */
	if (n->ev[EV_GX] != NDS_NEVER || !gx_needs_poll(n))
		return;
	now = nds_cpu_now(n, NDS_CPU9);
	gx_advance(&n->gx, now); /* el nivel de la FIFO tiene que ser el de ahora */
	n->gx_anchor = now + delay;
	gx_poll_set(n, now, n->gx_anchor);
}

void nds_gx_event(struct nds *n, uint64_t bt)
{
	gx_t *g = &n->gx;

	n->ev[EV_GX] = NDS_NEVER;
	n->gx_anchor = bt;
	gx_advance(g, bt);
	/* IRQ por nivel: se repite cada NDS_GX_PERIOD mientras la condición siga, aunque el programa la confirme */
	if (g->irqmode && gx_irq_level(g) && bt >= n->gx_irq_t + NDS_GX_PERIOD) {
		n->gx_irq_t = bt;
		nds_irq_raise(n, NDS_CPU9, NDS_IRQ_GXFIFO);
	}
	if (gx_fifo_level(g) < GX_FIFO_ENTRIES / 2)
		nds_dma_trigger(n, NDS_CPU9, NDS_DMA_GXFIFO);
	if (gx_needs_poll(n))
		gx_poll_set(n, bt + NDS_GX_POLL, bt + NDS_GX_POLL);
}

/* Foto de los registros del 3D que lee el rasterizador (en 0x04000000 del ARM9). */
static void snapshot_regs(const struct nds *n, gx_regs *r)
{
	const uint16_t *h = n->io[NDS_CPU9].gen;

#define H(off) h[(off) >> 1]
	memset(r, 0, sizeof *r);
	r->disp3dcnt = H(0x60);
	r->clear_color = (uint32_t)H(0x350) | ((uint32_t)H(0x352) << 16);
	r->clear_depth = H(0x354);
	r->clrimage_offset = H(0x356);
	r->fog_color = (uint32_t)H(0x358) | ((uint32_t)H(0x35A) << 16);
	r->fog_offset = H(0x35C);
	r->alpha_test_ref = H(0x340);
	for (int i = 0; i < 8; i++)
		r->edge_color[i] = H(0x330 + 2 * i);
	for (int i = 0; i < 16; i++) {
		r->fog_table[2 * i] = (uint8_t)H(0x360 + 2 * i);
		r->fog_table[2 * i + 1] = (uint8_t)(H(0x360 + 2 * i) >> 8);
	}
	for (int i = 0; i < 32; i++)
		r->toon_table[i] = H(0x380 + 2 * i);
#undef H
}

/* VBlank (línea 192): si el motor ejecutó un SWAP_BUFFERS, el cuadro construido se entrega al rasterizador y el motor
 * sigue con lo que había detrás. */
void nds_gx_vblank(struct nds *n, uint64_t t)
{
	gx_t *g = &n->gx;

	/* El motor solo escribe el cuadro en construcción, no el que lee el render 3D en segundo plano (PS3). Con tres cuadros
	 * el SWAP_BUFFERS no devuelve al motor el que se está rasterizando, así que aquí no se espera al render: la espera pasa
	 * a la línea 262 (`nds_video_submit_pending`), antes de entregar el 2D (docs/fase3.md, 3.2). */
	gx_advance(g, t);
	if (gx_swap_pending(g)) {
		gx_regs regs;

		snapshot_regs(n, &regs);
		gx_swap(g, t, &regs);
		n->gx_frames++;
	}
	nds_gx_schedule(n, NDS_GX_POLL);
}

const gx_frame *nds_gx_front(const struct nds *n)
{
	return gx_front(&n->gx);
}
