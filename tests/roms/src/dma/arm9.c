/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * SalsaDS — ROM dma, lado ARM9: las pruebas comunes (dmat.h) más lo propio del ARM9: DMAFILL, DMA a la VRAM y la
 * paleta, los modos de inicio en HBlank y al inicio de cada línea (solo en las líneas visibles) y que el DMA
 * no ve las TCM.
 */
#define IS_ARM9 1
#define BUF 0x02200000u
#define WAIT_VBLANK() sync_vblank()

#include <nds.h>

static inline void sync_vblank(void)
{
	while (*(volatile uint16_t *)0x04000006 == 192)
		;
	while (*(volatile uint16_t *)0x04000006 != 192)
		;
}

#include "dmat.h"

static void wait_line_at(uint32_t line)
{
	while (MEM16(R_VCOUNT) != line)
		;
}

static uint32_t count_nonzero(uint32_t off, uint32_t max)
{
	uint32_t n = 0;

	while (n < max && BUFW[off / 4 + n] != 0)
		n++;
	return n;
}

static void group_fill(void)
{
	uint32_t bad = 0;

	sds_group("FILL/VRAM");
	MEM32(R_DMAFILL(0)) = 0xDEADBEEFu;
	SDS_CHECK(MEM32(R_DMAFILL(0)), 0xDEADBEEFu);
	clear_words(0x400, 20);
	dma_set(0, R_DMAFILL(0), BUF + 0x400, D_ENABLE | D_WORD | D_SRC_FIX | 16);
	for (uint32_t i = 0; i < 16; i++)
		bad += BUFW[0x100 + i] != 0xDEADBEEFu;
	SDS_CHECK(bad, 0);
	SDS_CHECK(BUFW[0x110], 0);
	/* a la VRAM (banco B como LCDC) con 32 y con 16 bits, y a la paleta (16 bits) */
	REG8(R_VRAMCNT(1)) = 0x80;
	fill_words(0, 32, 0x40000000u);
	dma_set(1, BUF, 0x06820000u, D_ENABLE | D_WORD | 32);
	bad = 0;
	for (uint32_t i = 0; i < 32; i++)
		bad += MEM32(0x06820000u + 4 * i) != 0x40000000u + i;
	SDS_CHECK(bad, 0);
	dma_set(1, BUF, 0x06820100u, D_ENABLE | 64);
	bad = 0;
	for (uint32_t i = 0; i < 64; i++)
		bad += MEM16(0x06820100u + 2 * i) != (uint16_t)(BUFH[i]);
	SDS_CHECK(bad, 0);
	{
		const uint16_t save0 = MEM16(0x05000200), save1 = MEM16(0x05000202);

		BUFW[0] = 0x7FFF0001u;
		dma_set(2, BUF, 0x05000200u, D_ENABLE | 2);
		SDS_CHECK(MEM16(0x05000200), 0x0001);
		SDS_CHECK(MEM16(0x05000202), 0x7FFF);
		MEM16(0x05000200) = save0;
		MEM16(0x05000202) = save1;
	}
	/* el DMA no ve las TCM: lo que lee de ahí no es su contenido (GBATEK; informativo) */
	clear_words(0x400, 8);
	dma_set(3, 0x01000234u, BUF + 0x400, D_ENABLE | D_WORD | 8);
	SDS_DIVERGE(BUFW[0x100] == MEM32(0x01000234) && MEM32(0x01000234) != 0, 0);
	REG8(R_VRAMCNT(1)) = 0;
}

#define HB_WORDS 800u
static uint16_t hb_line[560], hb_cnt[560];

static void group_hblank(void)
{
	uint32_t n = 0, m = 0, k = 0, last = 0xFFFF, total = 0, d0 = 0, d1 = 0, d2 = 0, dmax = 0, vb_total = 0;
	uint32_t at100 = 0, at200 = 0, at191 = 0, at192 = 0, first_vis = 0xFFFF;

	sds_group("HBLANK");
	/* en HBlank, una palabra por línea visible (0-191), nada durante el VBlank (GBATEK). El búfer de destino es de 800 palabras
	 * (más de dos cuadros de líneas) y se anota en cada línea cuántas transferencias van, durante poco más de un cuadro y medio. */
	for (uint32_t i = 0; i < HB_WORDS; i++)
		BUFW[i] = 0x9000 + i;
	clear_words(0x1000, HB_WORDS);
	sync_vblank();
	dma_set(0, BUF, BUF + 0x1000, D_ENABLE | D_WORD | D_REPEAT | D_START_HBLANK | D_SRC_INC | 1);
	{
		uint32_t prev_cnt = 0, frames_lines = 0;

		while (k < 400 && frames_lines < 420) {
			const uint32_t v = MEM16(R_VCOUNT);

			while (n < HB_WORDS && BUFW[0x400 + n] != 0)
				n++;
			if (v != last) {
				last = v;
				frames_lines++;
				hb_line[k] = (uint16_t)v;
				hb_cnt[k] = (uint16_t)n;
				k++;
			}
		}
		MEM32(R_DMA(0, 2)) = 0;
		(void)prev_cnt;
	}
	/* análisis: transferencias por línea en cada región (visibles 0-191 y VBlank 192-262) y cuentas en líneas clave */
	for (uint32_t i = 1; i < k; i++) {
		const uint32_t delta = hb_cnt[i] - hb_cnt[i - 1], v = hb_line[i];

		if (v < 192) {
			total += delta;
			d0 += delta == 0;
			d1 += delta == 1;
			d2 += delta >= 2;
		} else {
			vb_total += delta;
		}
		if (delta > dmax)
			dmax = delta;
		if (v == 100 && !at100)
			at100 = hb_cnt[i];
		if (v == 200 && !at200)
			at200 = hb_cnt[i];
		if (v == 191 && !at191)
			at191 = hb_cnt[i];
		if (v == 192 && !at192)
			at192 = hb_cnt[i];
		if (v < 192 && first_vis == 0xFFFF)
			first_vis = hb_cnt[i];
	}
	sds_notef("HBLANK DMA desde la linea 192 (%lu lineas vistas): en 100=%lu 191=%lu 192=%lu 200=%lu; lineas visibles con 0/1/>=2 transferencias=%lu/%lu/%lu "
		  "(total %lu); transferencias en lineas de VBlank=%lu; maximo por linea=%lu",
		  (unsigned long)k, (unsigned long)at100, (unsigned long)at191, (unsigned long)at192, (unsigned long)at200, (unsigned long)d0,
		  (unsigned long)d1, (unsigned long)d2, (unsigned long)total, (unsigned long)vb_total, (unsigned long)dmax);
	for (uint32_t i = 0; i < k && i < 280; i += 14)
		sds_notef("  linea %3u cuenta %3u | %3u %3u | %3u %3u | %3u %3u | %3u %3u", hb_line[i], hb_cnt[i], i + 2 < k ? hb_line[i + 2] : 0,
			  i + 2 < k ? hb_cnt[i + 2] : 0, i + 5 < k ? hb_line[i + 5] : 0, i + 5 < k ? hb_cnt[i + 5] : 0,
			  i + 8 < k ? hb_line[i + 8] : 0, i + 8 < k ? hb_cnt[i + 8] : 0, i + 11 < k ? hb_line[i + 11] : 0,
			  i + 11 < k ? hb_cnt[i + 11] : 0);
	m = at200;
	n = at100;
	SDS_DIVERGE(n >= 100 && n <= 106, 1); /* melonDS 1.1 cuenta unas pocas más que las líneas */
	SDS_DIVERGE(m, 192);
	MEM32(R_DMA(0, 2)) = 0;
	/* al inicio de cada línea visible (modo 3): GBATEK dice «inicio de pantalla»; SalsaDS lo trata como una
	 * transferencia por línea visible. melonDS 1.1 hace una sola por habilitación: informativo */
	clear_words(0x1000, 300);
	sync_vblank();
	dma_set(0, BUF, BUF + 0x1000, D_ENABLE | D_WORD | D_REPEAT | D_START_DISP | D_SRC_INC | 1);
	wait_line_at(100);
	n = count_nonzero(0x1000, 300);
	wait_line_at(200);
	m = count_nonzero(0x1000, 300);
	MEM32(R_DMA(0, 2)) = 0;
	SDS_DIVERGE(n >= 100 && n <= 106, 1);
	SDS_DIVERGE(m >= 191 && m <= 194, 1);
}

int main(void)
{
	sds_nocache();
	MEM32(R_IME) = 0;
	sds_init("DMA ARM9+ARM7");
	group_copy();
	group_modes();
	group_fill();
	group_irq();
	group_priority();
	group_vblank();
	group_hblank();
	{
		uint32_t spin = 0, nibble;

		MBOX->cmd = CMD_QUIT;
		for (;;) {
			uint32_t n = 100000;

			nibble = REG32(R_IPCSYNC) & 0xF;
			if (nibble == SDS_IPC_OK || nibble == SDS_IPC_FAIL || spin >= 40000u)
				break;
			__asm__ volatile("1: subs %0, %0, #1\n\t bne 1b" : "+r"(n) : : "cc");
			spin++;
		}
		sds_group("ARM7");
		SDS_CHECK(nibble, SDS_IPC_OK);
	}
	sds_finish();
}
