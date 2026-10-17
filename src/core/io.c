/*
 * SalsaDS — registros de E/S de las dos CPUs (0x04000000): reparto por dirección hacia el planificador, el
 * IPC, el cartucho, el SPI, las matemáticas, los bancos de VRAM y el resto. Los registros sin comportamiento
 * propio (motores 2D, 3D, sonido, SIO...) se guardan y se leen de vuelta en `gen`.
 *
 * Todo acceso se reparte en medias palabras (`rd16`/`wr16` con máscara de bytes escritos), salvo lo que
 * tiene efectos por acceso y se atiende completo: FIFO de IPC, ROMDATA, cartucho, SPI y matemáticas.
 * Un acceso de 32 bits a un registro de dos mitades escribe primero la baja y luego la alta (la cuenta de un
 * temporizador o de un DMA se fija antes de su control).
 *
 * Fuente: GBATEK (mapas de E/S de la DS). Código propio.
 */
#include "core/nds_priv.h"

#define GEN_MASK 0x1FFFu

static inline uint16_t gen_get(const struct nds *n, int cpu, uint32_t o)
{
	return o < 0x2000u ? n->io[cpu].gen[o >> 1] : 0;
}

static inline void gen_set(struct nds *n, int cpu, uint32_t o, uint16_t v, uint16_t mask)
{
	if (o < 0x2000u) {
		uint16_t *g = &n->io[cpu].gen[o >> 1];

		*g = (uint16_t)((*g & ~mask) | (v & mask));
	}
}

static const uint32_t ie_mask[2] = { 0x003F3FFFu, 0x01DF3FFFu };

static uint16_t vramio_read(const struct nds *n, uint32_t o)
{
	/* VRAMCNT_A..G en 0x240-0x246, WRAMCNT en 0x247, VRAMCNT_H e I en 0x248-0x249 */
	static const int8_t map[10] = { 0, 1, 2, 3, 4, 5, 6, -1, 7, 8 };
	const uint32_t i = o - 0x240u;
	uint16_t v = 0;

	for (uint32_t k = 0; k < 2; k++) {
		const uint32_t idx = i + k;
		uint8_t b = 0;

		if (idx == 7)
			b = n->wramcnt;
		else if (idx < 10 && map[idx] >= 0)
			b = n->vramcnt[map[idx]];
		v |= (uint16_t)(b << (8 * k));
	}
	return v;
}

static void vramio_write(struct nds *n, uint32_t o, uint16_t v, uint16_t mask)
{
	static const int8_t map[10] = { 0, 1, 2, 3, 4, 5, 6, -1, 7, 8 };
	const uint32_t i = o - 0x240u;

	for (uint32_t k = 0; k < 2; k++) {
		const uint32_t idx = i + k;
		const uint8_t b = (uint8_t)(v >> (8 * k));

		if (!(mask & (0xFFu << (8 * k))))
			continue;
		if (idx == 7)
			nds_mem_wramcnt(n, b);
		else if (idx < 10 && map[idx] >= 0)
			nds_vramcnt_write(n, (unsigned)map[idx], b);
	}
}

/* ---- lectura de medias palabras --------------------------------------------------------------------- */

static uint16_t rd16(struct nds *n, int cpu, uint32_t o)
{
	struct nds_cpuio *io = &n->io[cpu];

	if (o >= 0xB0u && o < 0xE0u) { /* DMA 0-3 */
		const unsigned reg = (o - 0xB0u) >> 2;
		const struct nds_dma *d = &io->dma[reg / 3];
		const uint32_t w = reg % 3 == 0 ? d->sad : reg % 3 == 1 ? d->dad : d->cnt;

		return (uint16_t)(w >> ((o & 2u) * 8u));
	}
	if (o >= 0x100u && o < 0x110u) { /* temporizadores */
		const int i = (int)((o - 0x100u) >> 2);

		return (o & 2u) ? (uint16_t)io->tm[i].ctl : nds_timer_read(n, cpu, i);
	}
	switch (o) {
	case 0x004:
		return nds_dispstat_read(n, cpu);
	case 0x006:
		return (uint16_t)nds_line_at(nds_cpu_now(n, cpu));
	case 0x130:
		return n->keyinput & 0x3FFu;
	case 0x132:
		return io->keycnt;
	case 0x180:
		return nds_ipcsync_read(n, cpu);
	case 0x184:
		return nds_ipcfifocnt_read(n, cpu);
	case 0x204:
		if (cpu == NDS_CPU9)
			return n->io[NDS_CPU9].exmem;
		return (uint16_t)((n->io[NDS_CPU7].exmem & 0x7Fu) | (n->io[NDS_CPU9].exmem & 0xE880u));
	case 0x208:
		return (uint16_t)(io->ime & 1u);
	case 0x20A:
		return 0;
	case 0x210:
		return (uint16_t)io->ie;
	case 0x212:
		return (uint16_t)(io->ie >> 16);
	case 0x214:
		return (uint16_t)io->ifl;
	case 0x216:
		return (uint16_t)(io->ifl >> 16);
	case 0x240: case 0x242: case 0x244: case 0x246: case 0x248:
		if (cpu == NDS_CPU7) /* VRAMSTAT en el byte bajo y WRAMSTAT en el alto */
			return o == 0x240 ? (uint16_t)(n->vramstat | ((unsigned)n->wramcnt << 8)) : 0;
		return vramio_read(n, o);
	case 0x300:
		return io->postflg;
	default:
		break;
	}
	if (cpu == NDS_CPU9) {
		if (o == 0x304)
			return n->powcnt1;
	} else {
		switch (o) {
		case 0x136:
			return n->extkeyin;
		case 0x138:
			return nds_rtc_read(n);
		case 0x304:
			return n->powcnt2;
		default:
			break;
		}
	}
	return gen_get(n, cpu, o);
}

uint32_t nds_io_read(struct nds *n, int cpu, uint32_t a, int sz)
{
	const uint32_t o = a & 0xFFFFFFu;

	if (o >= 0x100000u) { /* 0x04100000: FIFO de recepción del IPC; 0x04100010: datos del cartucho */
		if (o == 0x100000u)
			return nds_ipcfifo_recv(n, cpu);
		if (o == 0x100010u)
			return nds_cart_data(n, cpu);
		return 0;
	}
	if (o >= 0x1A0u && o < 0x1C0u)
		return nds_cart_read(n, cpu, o, sz);
	if (cpu == NDS_CPU9 && o >= 0x280u && o < 0x2C0u)
		return nds_math_read(n, o, sz);
	if (cpu == NDS_CPU7 && o >= 0x1C0u && o < 0x1C4u)
		return nds_spi_read(n, o, sz);
	if (sz == 4)
		return (uint32_t)rd16(n, cpu, o) | ((uint32_t)rd16(n, cpu, o + 2) << 16);
	if (sz == 2)
		return rd16(n, cpu, o);
	return (rd16(n, cpu, o & ~1u) >> ((o & 1u) * 8u)) & 0xFFu;
}

/* ---- escritura de medias palabras --------------------------------------------------------------------- */

static void wr16(struct nds *n, int cpu, uint32_t o, uint16_t v, uint16_t mask)
{
	struct nds_cpuio *io = &n->io[cpu];

	if (o >= 0xB0u && o < 0xE0u) {
		const unsigned half = (o & 2u) * 8u;

		nds_dma_write(n, cpu, (o - 0xB0u) >> 2, (uint32_t)v << half, (uint32_t)mask << half);
		return;
	}
	if (o >= 0x100u && o < 0x110u) {
		nds_timer_write(n, cpu, (int)((o - 0x100u) >> 2), (int)((o >> 1) & 1u), v, mask);
		return;
	}
	switch (o) {
	case 0x004:
		nds_dispstat_write(n, cpu, v, mask);
		return;
	case 0x006:
		return;
	case 0x130:
		return;
	case 0x132:
		nds_keycnt_write(n, cpu, v, mask);
		return;
	case 0x180:
		nds_ipcsync_write(n, cpu, v, mask);
		return;
	case 0x184:
		nds_ipcfifocnt_write(n, cpu, v, mask);
		return;
	case 0x204:
		if (cpu == NDS_CPU9) {
			const uint16_t m = mask & 0xE8FFu;

			io->exmem = (uint16_t)((io->exmem & ~m) | (v & m));
		} else {
			const uint16_t m = mask & 0x007Fu;

			io->exmem = (uint16_t)((io->exmem & ~m) | (v & m));
		}
		return;
	case 0x208:
		if (mask & 0xFFu) {
			io->ime = v & 1u;
			nds_irq_update(n, cpu);
		}
		return;
	case 0x210:
	case 0x212: {
		const unsigned sh = (o & 2u) * 8u;
		const uint32_t m = (uint32_t)mask << sh;

		io->ie = ((io->ie & ~m) | ((uint32_t)v << sh & m)) & ie_mask[cpu];
		nds_irq_update(n, cpu);
		return;
	}
	case 0x214:
	case 0x216: {
		const unsigned sh = (o & 2u) * 8u;

		io->ifl &= ~(((uint32_t)v & mask) << sh);
		nds_irq_update(n, cpu);
		return;
	}
	case 0x240: case 0x242: case 0x244: case 0x246: case 0x248:
		if (cpu == NDS_CPU9)
			vramio_write(n, o, v, mask);
		return;
	case 0x300:
		if (mask & 0x00FFu) {
			if (cpu == NDS_CPU9)
				io->postflg = (uint8_t)((io->postflg & 1u) | (v & 3u));
			else
				io->postflg = (uint8_t)(io->postflg | (v & 1u));
		}
		if (cpu == NDS_CPU7 && (mask & 0xFF00u)) {
			const unsigned mode = (v >> 14) & 3u; /* HALTCNT bits 6-7 */

			if (mode == 2 || mode == 3)
				nds_cpu_halt(n, cpu);
		}
		return;
	default:
		break;
	}
	if (cpu == NDS_CPU9) {
		if (o == 0x304) {
			n->powcnt1 = (uint16_t)(((n->powcnt1 & ~mask) | (v & mask)) & 0x820Fu);
			return;
		}
	} else {
		switch (o) {
		case 0x138:
			nds_rtc_write(n, v, mask & 0xFFu);
			return;
		case 0x304:
			n->powcnt2 = (uint16_t)(((n->powcnt2 & ~mask) | (v & mask)) & 3u);
			return;
		default:
			break;
		}
	}
	gen_set(n, cpu, o, v, mask);
}

void nds_io_write(struct nds *n, int cpu, uint32_t a, int sz, uint32_t v)
{
	const uint32_t o = a & 0xFFFFFFu;

	if (o >= 0x100000u)
		return;
	if (o == 0x188u && sz == 4) {
		nds_ipcfifo_send(n, cpu, v);
		return;
	}
	if (o >= 0x1A0u && o < 0x1C0u) {
		nds_cart_write(n, cpu, o, sz, v);
		return;
	}
	if (cpu == NDS_CPU9 && o >= 0x280u && o < 0x2C0u) {
		nds_math_write(n, o, sz, v);
		return;
	}
	if (cpu == NDS_CPU7 && o >= 0x1C0u && o < 0x1C4u) {
		nds_spi_write(n, o, sz, v);
		return;
	}
	if (sz == 4) {
		wr16(n, cpu, o, (uint16_t)v, 0xFFFFu);
		wr16(n, cpu, o + 2, (uint16_t)(v >> 16), 0xFFFFu);
	} else if (sz == 2) {
		wr16(n, cpu, o, (uint16_t)v, 0xFFFFu);
	} else {
		const unsigned sh = (o & 1u) * 8u;

		wr16(n, cpu, o & ~1u, (uint16_t)((v & 0xFFu) << sh), (uint16_t)(0xFFu << sh));
	}
}
