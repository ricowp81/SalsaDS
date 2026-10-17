/*
 * SalsaDS — ranura del cartucho (Fase 1.2): ROMCTRL, ROMCMD, ROMDATA y AUXSPI, para una ROM ya
 * desencriptada.
 *
 * No hay KEY1 ni KEY2: el arranque directo carga los binarios de la cabecera y los comandos que el juego
 * manda después (lectura B7h, cabecera 00h, identificador 90h/B8h) se atienden como si el cifrado estuviera
 * apagado; los bits de cifrado de ROMCTRL se guardan y se ignoran. Lectura B7h: dirección de 32 bits
 * (MSB primero) en ROMCMD[1..4]; por debajo de 0x8000 se lee 0x8000 + (dir & 0x1FF); un bloque no cruza un
 * límite de 4 KB (da la vuelta dentro de su página); más allá de la capacidad se espeja. El bloque se
 * entrega palabra a palabra: DRQ (bit 23) se levanta cuando hay una palabra lista y el bloque termina (bit
 * 31 a 0, IRQ 19 si AUXSPICNT.14) cuando se leyó la última; las lecturas por DMA (modo cartucho) sacan una
 * palabra por disparo. Sin chip de guardado: AUXSPIDATA contesta 0xFF.
 *
 * Fuente: GBATEK (DS Cartridge I/O Ports, Protocol, Header). Código propio.
 */
#include <string.h>

#include "core/nds_priv.h"

int nds_cart_owner(const struct nds *n)
{
	return (n->io[NDS_CPU9].exmem >> 11) & 1; /* 0 = ARM9, 1 = ARM7 */
}

static uint8_t rom_byte(const struct nds_cart *c, uint32_t addr)
{
	if (addr < 0x8000u)
		addr = 0x8000u + (addr & 0x1FFu);
	if (c->capacity)
		addr &= c->capacity - 1u;
	return addr < c->size ? c->rom[addr] : 0xFF;
}

uint32_t nds_cart_chipid(const struct nds_cart *c)
{
	const uint32_t mb = c->capacity >> 20;

	return 0xC2u | ((mb ? mb - 1u : 0u) << 8);
}

static void finish(struct nds *n)
{
	struct nds_cart *c = &n->cart;

	c->busy = 0;
	c->drq = 0;
	if (c->auxcnt & 0x4000u)
		nds_irq_raise(n, nds_cart_owner(n), NDS_IRQ_CARDDONE);
}

static void cart_start(struct nds *n, int cpu)
{
	struct nds_cart *c = &n->cart;
	const uint32_t ctl = c->romctl;
	const unsigned blk = (ctl >> 24) & 7u;
	const uint32_t nbytes = blk == 0 ? 0 : blk == 7 ? 4 : 0x100u << blk;
	const unsigned clk = (ctl & (1u << 27)) ? 8u : 5u; /* ciclos de bus por byte */
	const uint32_t gap1 = ctl & 0x1FFFu;
	uint32_t addr;

	if (!(c->auxcnt & 0x8000u) || c->busy)
		return;
	switch (c->cmd[0]) {
	case 0xB7:
		addr = (uint32_t)c->cmd[1] << 24 | (uint32_t)c->cmd[2] << 16 | (uint32_t)c->cmd[3] << 8 | c->cmd[4];
		{
			const uint32_t page = addr & ~0xFFFu, off = addr & 0xFFFu;

			for (uint32_t i = 0; i < nbytes; i++)
				c->buf[i] = rom_byte(c, page + ((off + i) & 0xFFFu));
		}
		break;
	case 0x00: /* cabecera, repetida cada 4 KB */
		for (uint32_t i = 0; i < nbytes; i++)
			c->buf[i] = (i & 0xFFFu) < c->size ? c->rom[i & 0xFFFu] : 0xFF;
		break;
	case 0x90:
	case 0xB8: {
		const uint32_t id = nds_cart_chipid(c);

		for (uint32_t i = 0; i < nbytes; i++)
			c->buf[i] = (uint8_t)(id >> (8 * (i & 3u)));
		break;
	}
	case 0x9F:
		memset(c->buf, 0xFF, nbytes);
		break;
	default:
		memset(c->buf, 0, nbytes);
		break;
	}
	c->nwords = nbytes / 4;
	c->widx = 0;
	c->drq = 0;
	c->busy = 1;
	nds_event_set(n, EV_CART, nds_cpu_now(n, cpu) + (uint64_t)(8u * clk + gap1 + (nbytes ? 4u * clk : 0u)) * 2u);
}

void nds_cart_event(struct nds *n)
{
	struct nds_cart *c = &n->cart;

	if (!c->busy)
		return;
	if (c->nwords == 0) {
		finish(n);
		return;
	}
	if (c->widx < c->nwords && !c->drq) {
		c->drq = 1;
		nds_dma_trigger(n, nds_cart_owner(n), NDS_DMA_CART);
	}
}

/* ROMDATA (0x04100010): sale la palabra lista y se programa la siguiente. */
uint32_t nds_cart_data(struct nds *n, int cpu)
{
	struct nds_cart *c = &n->cart;
	uint32_t v;

	if (cpu != nds_cart_owner(n) || !c->busy || !c->drq)
		return 0;
	v = rd32le(c->buf + 4u * c->widx);
	c->widx++;
	c->drq = 0;
	if (c->widx >= c->nwords) {
		finish(n);
	} else {
		const unsigned clk = (c->romctl & (1u << 27)) ? 8u : 5u;

		nds_event_set(n, EV_CART, nds_cpu_now(n, cpu) + (uint64_t)(4u * clk) * 2u);
	}
	return v;
}

uint32_t nds_cart_read(struct nds *n, int cpu, uint32_t off, int sz)
{
	struct nds_cart *c = &n->cart;
	uint8_t img[0x20];
	uint32_t v = 0;

	if (cpu != nds_cart_owner(n) || off < 0x1A0u || off + (uint32_t)sz > 0x1C0u)
		return 0;
	memset(img, 0, sizeof img);
	wr16le(img + 0, (uint16_t)(c->auxcnt & 0xE043u));
	wr16le(img + 2, c->auxdata);
	wr32le(img + 4, (c->romctl & ~((1u << 31) | (1u << 23))) | ((uint32_t)c->busy << 31) | ((uint32_t)c->drq << 23));
	memcpy(img + 8, c->cmd, 8);
	for (int i = 0; i < sz; i++)
		v |= (uint32_t)img[off - 0x1A0u + (uint32_t)i] << (8 * i);
	return v;
}

void nds_cart_write(struct nds *n, int cpu, uint32_t off, int sz, uint32_t v)
{
	struct nds_cart *c = &n->cart;
	int start = 0;

	if (cpu != nds_cart_owner(n))
		return;
	for (int i = 0; i < sz; i++) {
		const uint32_t o = off + (uint32_t)i - 0x1A0u;
		const uint8_t b = (uint8_t)(v >> (8 * i));

		if (o >= 0x20u)
			break;
		switch (o) {
		case 0:
			c->auxcnt = (uint16_t)((c->auxcnt & ~0x0043u) | (b & 0x43u));
			break;
		case 1:
			c->auxcnt = (uint16_t)((c->auxcnt & 0x00FFu) | ((b & 0xE0u) << 8));
			break;
		case 2: /* AUXSPIDATA: sin chip de guardado, contesta 0xFF */
			c->auxdata = 0xFF;
			break;
		case 4: case 5: case 6:
			c->romctl = (c->romctl & ~(0xFFu << (8 * (o - 4)))) | ((uint32_t)b << (8 * (o - 4)));
			break;
		case 7:
			c->romctl = (c->romctl & 0x00FFFFFFu) | ((uint32_t)(b & 0x7Fu) << 24);
			if (b & 0x80u)
				start = 1;
			break;
		default:
			if (o >= 8 && o < 16)
				c->cmd[o - 8] = b;
			break;
		}
	}
	if (start)
		cart_start(n, cpu);
}
