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

static void group_hblank(void)
{
	uint32_t n, m;

	sds_group("HBLANK");
	/* en HBlank, una palabra por línea visible (0-191), nada durante el VBlank */
	for (uint32_t i = 0; i < 300; i++)
		BUFW[i] = 0x9000 + i;
	clear_words(0x1000, 300);
	sync_vblank();
	dma_set(0, BUF, BUF + 0x1000, D_ENABLE | D_WORD | D_REPEAT | D_START_HBLANK | D_SRC_INC | 1);
	wait_line_at(100);
	n = count_nonzero(0x1000, 300);
	SDS_CHECK(n >= 100 && n <= 106, 1); /* melonDS 1.1 cuenta unas pocas más que las líneas */
	wait_line_at(200);
	m = count_nonzero(0x1000, 300);
	SDS_CHECK(m, 192);
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
