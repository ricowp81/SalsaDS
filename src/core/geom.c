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
	nds_gx_schedule(n);
}

void nds_gx_port_write(struct nds *n, uint32_t off, uint32_t w)
{
	if (!GX_ON(n))
		return;
	stall_cpu9(n, gx_port_write(&n->gx, nds_cpu_now(n, NDS_CPU9), off, w));
	nds_gx_schedule(n);
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
		nds_gx_schedule(n);
}

/* ¿La FIFO tiene sitio para la DMA de modo 7 (menos de la mitad)? `t` es el instante de ese acceso. */
int nds_gx_dma_ok(struct nds *n, uint64_t t)
{
	gx_advance(&n->gx, t);
	return gx_fifo_level(&n->gx) < GX_FIFO_ENTRIES / 2;
}

/* Pone el evento EV_GX donde el motor tiene algo que hacer: al empezar la siguiente orden (baja el nivel de la FIFO:
 * aviso a la IRQ y a la DMA) y, mientras la IRQ 21 esté pedida por nivel, cada NDS_GX_PERIOD. */
void nds_gx_schedule(struct nds *n)
{
	const uint64_t now = nds_cpu_now(n, NDS_CPU9);
	uint64_t when = gx_next_start(&n->gx);

	if (when != GX_NEVER && when <= now)
		when = now + 1; /* ya empezó: que el evento lo recoja en cuanto pase el tramo */
	if (n->gx.irqmode && gx_irq_level(&n->gx)) {
		const uint64_t p = now + 64u;

		if (p < when)
			when = p;
	}
	if (when == GX_NEVER)
		return;
	if (n->ev[EV_GX] == NDS_NEVER || when < n->ev[EV_GX])
		nds_event_set(n, EV_GX, when);
}

void nds_gx_event(struct nds *n, uint64_t bt)
{
	gx_t *g = &n->gx;
	uint64_t when;

	n->ev[EV_GX] = NDS_NEVER;
	gx_advance(g, bt);
	if (g->irqmode && gx_irq_level(g))
		nds_irq_raise(n, NDS_CPU9, NDS_IRQ_GXFIFO); /* por nivel: se repite mientras la condición siga */
	if (gx_fifo_level(g) < GX_FIFO_ENTRIES / 2)
		nds_dma_trigger(n, NDS_CPU9, NDS_DMA_GXFIFO);
	when = gx_next_start(g);
	if (g->irqmode && gx_irq_level(g)) {
		const uint64_t p = bt + NDS_GX_PERIOD;

		if (p < when)
			when = p;
	}
	if (when == GX_NEVER)
		return;
	if (when <= bt)
		when = bt + 1;
	if (n->ev[EV_GX] == NDS_NEVER || when < n->ev[EV_GX]) /* los empujes de la DMA de arriba ya pudieron fijarlo */
		nds_event_set(n, EV_GX, when);
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

	gx_advance(g, t);
	if (gx_swap_pending(g)) {
		gx_regs regs;

		snapshot_regs(n, &regs);
		gx_swap(g, t, &regs);
		n->gx_frames++;
	}
	nds_gx_schedule(n);
}

const gx_frame *nds_gx_front(const struct nds *n)
{
	return gx_front(&n->gx);
}
