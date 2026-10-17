/*
 * SalsaDS — IPC entre el ARM9 y el ARM7 (Fase 1.2): IPCSYNC (4 bits en cada sentido más una IRQ) y las dos
 * colas FIFO de 16 palabras (IPCFIFOCNT, IPCFIFOSEND, IPCFIFORECV).
 *
 * Es uno de los tres caminos compartidos entre las CPUs (junto con la memoria y las IRQ): cada cola tiene un
 * productor y un consumidor, así que con las CPUs en hilos distintos se vuelve una cola de un productor y un
 * consumidor sin cerrojo; las IRQ de «vacía» y «no vacía» se disparan por flanco comparando el estado de
 * antes y de después de cada operación.
 *
 * Fuente: GBATEK (DS Inter Process Communication). Código propio.
 */
#include "core/nds_priv.h"

#define CNT_SEND_EMPTY_IRQ 0x0004u
#define CNT_RECV_NE_IRQ 0x0400u
#define CNT_ERROR 0x4000u
#define CNT_ENABLE 0x8000u

void nds_ipc_init(struct nds *n)
{
	for (int i = 0; i < 2; i++) {
		n->ipcsync_out[i] = 0;
		n->ipcsync_irqen[i] = 0;
		n->io[i].ipcfifocnt = 0;
		n->fifo[i].head = n->fifo[i].count = n->fifo[i].last = 0;
	}
}

/* Condiciones de las IRQ de las dos CPUs: bit 2*cpu = «cola de envío vacía» habilitada, bit 2*cpu+1 =
 * «cola de recepción no vacía» habilitada. */
static unsigned conds(const struct nds *n)
{
	unsigned m = 0;

	for (int cpu = 0; cpu < 2; cpu++) {
		const uint16_t cnt = n->io[cpu].ipcfifocnt;

		if ((cnt & CNT_SEND_EMPTY_IRQ) && n->fifo[cpu].count == 0)
			m |= 1u << (2 * cpu);
		if ((cnt & CNT_RECV_NE_IRQ) && n->fifo[1 - cpu].count != 0)
			m |= 2u << (2 * cpu);
	}
	return m;
}

static void edges(struct nds *n, unsigned old)
{
	const unsigned rising = conds(n) & ~old;

	for (int cpu = 0; cpu < 2; cpu++) {
		if (rising & (1u << (2 * cpu)))
			nds_irq_raise(n, cpu, NDS_IRQ_IPCSEND);
		if (rising & (2u << (2 * cpu)))
			nds_irq_raise(n, cpu, NDS_IRQ_IPCRECV);
	}
}

uint16_t nds_ipcsync_read(struct nds *n, int cpu)
{
	return (uint16_t)((n->ipcsync_out[1 - cpu] & 0xFu) | ((n->ipcsync_out[cpu] & 0xFu) << 8) |
			  ((unsigned)n->ipcsync_irqen[cpu] << 14));
}

void nds_ipcsync_write(struct nds *n, int cpu, uint16_t v, uint16_t mask)
{
	if (mask & 0x0F00u)
		n->ipcsync_out[cpu] = (v >> 8) & 0xFu;
	if (mask & 0x4000u)
		n->ipcsync_irqen[cpu] = (v >> 14) & 1u;
	if ((mask & 0x2000u) && (v & 0x2000u) && n->ipcsync_irqen[1 - cpu])
		nds_irq_raise(n, 1 - cpu, NDS_IRQ_IPCSYNC);
}

uint16_t nds_ipcfifocnt_read(struct nds *n, int cpu)
{
	const struct nds_fifo *s = &n->fifo[cpu], *r = &n->fifo[1 - cpu];
	const uint16_t cnt = n->io[cpu].ipcfifocnt;

	return (uint16_t)((s->count == 0 ? 1u : 0u) | (s->count == 16 ? 2u : 0u) | (cnt & CNT_SEND_EMPTY_IRQ) |
			  (r->count == 0 ? 0x100u : 0u) | (r->count == 16 ? 0x200u : 0u) | (cnt & CNT_RECV_NE_IRQ) |
			  (cnt & CNT_ERROR) | (cnt & CNT_ENABLE));
}

void nds_ipcfifocnt_write(struct nds *n, int cpu, uint16_t v, uint16_t mask)
{
	struct nds_cpuio *io = &n->io[cpu];
	const unsigned old = conds(n);

	if (mask & 0x00FFu) {
		io->ipcfifocnt = (uint16_t)((io->ipcfifocnt & ~CNT_SEND_EMPTY_IRQ) | (v & CNT_SEND_EMPTY_IRQ));
		if (v & 0x0008u) { /* vaciar la cola de envío */
			n->fifo[cpu].head = n->fifo[cpu].count = n->fifo[cpu].last = 0;
		}
	}
	if (mask & 0xFF00u) {
		io->ipcfifocnt = (uint16_t)((io->ipcfifocnt & ~(CNT_RECV_NE_IRQ | CNT_ENABLE)) |
					    (v & (CNT_RECV_NE_IRQ | CNT_ENABLE)));
		if (v & CNT_ERROR) /* escribir 1 reconoce el error */
			io->ipcfifocnt &= (uint16_t)~CNT_ERROR;
	}
	edges(n, old);
}

void nds_ipcfifo_send(struct nds *n, int cpu, uint32_t v)
{
	struct nds_fifo *s = &n->fifo[cpu];
	struct nds_cpuio *io = &n->io[cpu];
	const unsigned old = conds(n);

	if (!(io->ipcfifocnt & CNT_ENABLE))
		return; /* deshabilitada: el dato se descarta, sin error */
	if (s->count == 16) {
		io->ipcfifocnt |= CNT_ERROR;
		return;
	}
	s->q[(s->head + s->count) & 15u] = v;
	s->count++;
	edges(n, old);
}

uint32_t nds_ipcfifo_recv(struct nds *n, int cpu)
{
	struct nds_fifo *r = &n->fifo[1 - cpu];
	struct nds_cpuio *io = &n->io[cpu];
	const unsigned old = conds(n);
	uint32_t v;

	if (!(io->ipcfifocnt & CNT_ENABLE)) /* deshabilitada: el dato más viejo, sin sacarlo */
		return r->count ? r->q[r->head] : r->last;
	if (r->count == 0) {
		io->ipcfifocnt |= CNT_ERROR;
		return r->last;
	}
	v = r->q[r->head];
	r->head = (r->head + 1) & 15u;
	r->count--;
	r->last = v;
	edges(n, old);
	return v;
}
