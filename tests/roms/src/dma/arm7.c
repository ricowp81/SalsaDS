/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * SalsaDS — ROM dma, lado ARM7: las pruebas comunes (dmat.h) y el ancho de la cuenta (14 bits en los canales
 * 0 a 2, 16 en el 3). El ARM7 espera el VBlank mirando VCOUNT.
 */
#define IS_ARM9 0
#define BUF 0x02240000u
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

static void group_width(void)
{
	sds_group("ANCHO");
	/* cuenta de 14 bits en los canales 0-2: 0x4002 son 2 unidades */
	fill_words(0, 8, 0x600);
	clear_words(0x400, 8);
	dma_set(0, BUF, BUF + 0x400, D_ENABLE | D_WORD | 0x4002);
	SDS_CHECK(BUFW[0x100], 0x600);
	SDS_CHECK(BUFW[0x101], 0x601);
	SDS_CHECK(BUFW[0x102], 0);
	/* el canal 3 tiene 16 bits */
	clear_words(0x400, 8);
	dma_set(3, BUF, BUF + 0x400, D_ENABLE | D_WORD | 0x3);
	SDS_CHECK(BUFW[0x102], 0x602);
	SDS_CHECK(BUFW[0x103], 0);
	/* lo que se lee de vuelta de la cuenta (informativo) */
	MEM32(R_DMA(0, 2)) = 0xFFFF;
	SDS_DIVERGE(MEM32(R_DMA(0, 2)) & 0xFFFF, 0x3FFF);
	MEM32(R_DMA(0, 2)) = 0;
	MEM32(R_DMA(3, 2)) = 0xFFFF;
	SDS_DIVERGE(MEM32(R_DMA(3, 2)) & 0xFFFF, 0xFFFF);
	MEM32(R_DMA(3, 2)) = 0;
}

int main(void)
{
	__asm__ volatile("msr cpsr_c, #0xDF" : : : "cc");
	sds_init("DMA ARM7");
	irqInit();
	MEM32(R_IME) = 1;
	__asm__ volatile("msr cpsr_c, #0x1F" : : : "cc");
	group_copy();
	group_modes();
	group_irq();
	group_priority();
	group_vblank();
	group_width();
	{
		volatile uint32_t spin = 0;

		for (;;) {
			if (MBOX->cmd == CMD_QUIT) {
				MBOX->done = 1;
				break;
			}
			if (++spin > 400000000u)
				break;
		}
	}
	sds_finish();
}
