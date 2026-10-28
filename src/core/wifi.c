/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Inicialización local Wi-Fi, sin radio ni paquetes. GBATEK: DS Wifi I/O Map,
 * Control, Baseband Chip y Unused Registers. Código propio desde la descripción
 * del hardware. RAM, registros y BB permiten el diagnóstico de arranque de los
 * juegos aunque no se conecten. No hay IRQ de recepción ni éxito de envío falso.
 * BB/RF completan en el acto; no se modelan temporizadores ni el MAC inalámbrico.
 */
#include <string.h>
#include "core/nds_priv.h"

/* Solo los puertos implementados: máscara de escritura y valor de encendido.
 * Los huecos y los puertos de estado no adquieren capacidad de lectura/escritura. */
static const struct { uint16_t off, mask, initial; } ports[] = {
	{0x004,0x9FFF,0}, {0x006,0x007F,0}, {0x008,0xFFFF,0}, {0x00A,0xFFFF,0},
	{0x012,0xFFFF,0}, {0x018,0xFFFF,0}, {0x01A,0xFFFF,0}, {0x01C,0xFFFF,0},
	{0x020,0xFFFF,0}, {0x022,0xFFFF,0}, {0x024,0xFFFF,0}, {0x028,0x000F,0},
	{0x02A,0x07FF,0}, {0x02C,0xFFFF,0x0707}, {0x02E,1,0},
	{0x030,0xFF0E,0}, {0x032,0xFFFF,0}, {0x036,3,1}, {0x038,7,3},
	{0x03C,3,0x0200}, {0x040,0x8001,0}, {0x048,3,0},
	{0x050,0xFFFF,0x4000}, {0x052,0xFFFF,0x4800}, {0x056,0x0FFF,0},
	{0x058,0x1FFE,0}, {0x05A,0x0FFF,0}, {0x05C,0x0FFF,0},
	{0x062,0x1FFE,0}, {0x064,0x0FFF,0}, {0x068,0x1FFE,0},
	{0x06C,0x0FFF,0}, {0x074,0x1FFE,0}, {0x076,0x0FFF,0},
	{0x080,0xFFFF,0}, {0x084,0x00FF,0}, {0x088,0x00FF,0}, {0x08C,0x03FF,0x64},
	{0x08E,0x00FF,0}, {0x090,0xFFFF,0}, {0x094,0xFFFF,0}, {0x09C,0xFFFF,0x50},
	{0x0A0,0xFFFF,0}, {0x0A4,0xFFFF,0}, {0x0A8,0xFFFF,0}, {0x0BC,3,1},
	{0x0C0,0xFFFF,0}, {0x0C4,0xFFFF,0}, {0x0D0,0x1FFF,0x0401},
	{0x0D4,3,1}, {0x0D8,0x0FFF,4}, {0x0DA,0xFFFF,0x0602}, {0x0E0,15,8},
	{0x120,0x81FF,0x48}, {0x122,0xFFFF,0x4840}, {0x124,0xFFFF,0},
	{0x128,0xFFFF,0}, {0x130,0x0FFF,0x142}, {0x132,0x8FFF,0x8064},
	{0x134,0xFFFF,0xFFFF}, {0x140,0xFFFF,0}, {0x142,0xFFFF,0x2443},
	{0x144,0x00FF,0x42}, {0x146,0x00FF,0x16}, {0x148,0x00FF,0x16},
	{0x14A,0x00FF,0x16}, {0x14C,0xFFFF,0x162C}, {0x150,0xFF3F,0x0204},
	{0x154,0x7A7F,0x58}, {0x15A,0x00FF,0}, {0x160,0x4100,0x0100},
	{0x168,0x800F,0x800D}, {0x17C,0xFFFF,0x0800}, {0x17E,0xFFFF,0xC008},
	{0x184,0x413F,0x18}, {0x194,7,0}, {0x198,15,0},
	{0x1A0,0x1933,0}, {0x1A2,3,1}, {0x1A4,0xFFFF,0},
	{0x1AA,0xFFFF,0}, {0x1AE,0xFFFF,0}, {0x220,0xFFFF,0}, {0x224,3,3},
};

void nds_wifi_init(struct nds *n)
{
	struct nds_wifi *w = &n->wifi;

	memset(w, 0, sizeof *w);
	for (unsigned i = 0; i < sizeof ports / sizeof ports[0]; i++)
		wr16le(w->regs + ports[i].off, ports[i].initial);
	w->bb[0] = 0x6D; /* identificación, solo lectura */
	w->bb[0x5D] = 1;
	w->bb[0x64] = 0xFF;
	w->random = 1;
}

static int bb_writable(unsigned i)
{
	return (i >= 1 && i <= 0x0C) || (i >= 0x13 && i <= 0x15) ||
	       (i >= 0x1B && i <= 0x26) || (i >= 0x28 && i <= 0x4C) ||
	       (i >= 0x4E && i <= 0x5C) || i == 0x62 || i == 0x63 ||
	       i == 0x65 || i == 0x67 || i == 0x68;
}

static uint16_t read16(struct nds_wifi *w, unsigned off)
{
	if (off == 0) return 0x1440;
	if (off == 0x044) {
		/* Secuencia de 11 bits de GBATEK; avance por acceso, no por ciclo. */
		uint16_t v = w->random;
		w->random = (uint16_t)(((v << 1) | (v >> 10)) ^ (v & 1u)) & 0x7FFu;
		return v;
	}
	if (off == 0x078) off = 0x068;
	if (off == 0x178) off = 0x17C;
	if (off == 0x158) off = 0x15C;
	if (off == 0x19C) return 0x0046;
	if (off == 0x214) return 9;
	if (off == 0x254) return 0xFFFF;
	return off < sizeof w->regs ? rd16le(w->regs + off) : 0xFFFF;
}

static void write16(struct nds_wifi *w, unsigned off, uint16_t v)
{
	if (off == 0x03C) {
		uint16_t state = rd16le(w->regs + off) & 0x0300u;
		/* Sin tráfico: la petición de encendido se atiende de inmediato. */
		if ((v & 2u) && !(rd16le(w->regs + 0x036) & 1u) && (rd16le(w->regs + 4) & 1u))
			state = 0;
		wr16le(w->regs + off, state | (v & 3u));
		return;
	}
	if (off == 0x040 && (v & 0x8000u)) {
		uint16_t state = rd16le(w->regs + 0x03C);
		if (v & 1u) state = (state & 3u) | 0x0200u;
		else if (state & 0x0200u) state |= 0x0100u;
		wr16le(w->regs + 0x03C, state);
	}
	if (off == 0x2D0 && (rd16le(w->regs + 0x03C) & 0x0100u)) {
		if (!v) wr16le(w->regs + 0x03C, rd16le(w->regs + 0x03C) & 3u);
		return;
	}
	if (off == 0x158) {
		unsigned index = v & 0xFFu;
		if ((v >> 12) == 5 && bb_writable(index))
			w->bb[index] = w->regs[0x15A];
		else if ((v >> 12) == 6)
			wr16le(w->regs + 0x15C, w->bb[index]);
		return;
	}
	if (off == 0x010) { /* IF: reconocer únicamente los bits escritos. */
		wr16le(w->regs + off, rd16le(w->regs + off) & ~v);
		return;
	}
	for (unsigned i = 0; i < sizeof ports / sizeof ports[0]; i++)
		if (ports[i].off == off) {
			wr16le(w->regs + off, v & ports[i].mask);
			return;
		}
}

uint32_t nds_wifi_read(struct nds *n, uint32_t a, int sz)
{
	struct nds_wifi *w = &n->wifi;
	unsigned off = a & 0x7FFFu;
	uint32_t v;

	if (n->wifi_off) return 0;
	if (off >= 0x4000 && off < 0x6000) {
		off &= 0x1FFEu;
		v = rd16le(w->ram + off);
		if (sz == 4) v |= (uint32_t)rd16le(w->ram + ((off + 2) & 0x1FFFu)) << 16;
	} else if (off < 0x2000 || off >= 0x6000) {
		off &= 0xFFEu;
		v = read16(w, off);
		if (sz == 4) v |= (uint32_t)read16(w, (off + 2) & 0xFFFu) << 16;
	} else return 0;
	if (sz == 1) v = (v >> ((a & 1u) * 8)) & 0xFFu;
	NDS_TRACE(n, NDS_DBG_WIFI, "WIFI R %08x %d = %08x", a, sz, v);
	return v;
}

void nds_wifi_write(struct nds *n, uint32_t a, int sz, uint32_t v)
{
	struct nds_wifi *w = &n->wifi;
	unsigned off = a & 0x7FFFu;

	if (n->wifi_off || sz == 1) return; /* STRB no escribe RAM ni puertos. */
	NDS_TRACE(n, NDS_DBG_WIFI, "WIFI W %08x %d = %08x", a, sz, v);
	if (off >= 0x4000 && off < 0x6000) {
		off &= 0x1FFEu;
		wr16le(w->ram + off, (uint16_t)v);
		if (sz == 4) wr16le(w->ram + ((off + 2) & 0x1FFFu), (uint16_t)(v >> 16));
	} else if (off < 0x2000 || off >= 0x6000) {
		off &= 0xFFEu;
		write16(w, off, (uint16_t)v);
		if (sz == 4) write16(w, (off + 2) & 0xFFFu, (uint16_t)(v >> 16));
	}
}
