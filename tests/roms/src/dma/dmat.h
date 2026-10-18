/*
 * SalsaDS — ROM dma: pruebas comunes del ARM9 y el ARM7. Quien la incluya define IS_ARM9 (0 o 1), BUF (zona de
 * memoria principal propia de la CPU, 64 KB) y WAIT_VBLANK() (espera de VBlank por VCOUNT).
 *
 * DMACNT: bits 0-20 cuenta (ARM9; el ARM7 usa 14 bits en los canales 0-2 y 16 en el 3), 21-22 destino
 * (0 +, 1 -, 2 fijo, 3 +/recarga), 23-24 origen, 25 repetir, 26 palabra de 32 bits, 27-29 (ARM9) o 28-29
 * (ARM7) inicio, 30 IRQ, 31 habilitar.
 */
#ifndef DMAT_H
#define DMAT_H

#include <nds.h>

#include "../common/hw.h"

#define D_DST_INC (0u << 21)
#define D_DST_DEC (1u << 21)
#define D_DST_FIX (2u << 21)
#define D_DST_RELOAD (3u << 21)
#define D_SRC_INC (0u << 23)
#define D_SRC_DEC (1u << 23)
#define D_SRC_FIX (2u << 23)
#define D_REPEAT (1u << 25)
#define D_WORD (1u << 26)
#define D_START_NOW (0u)
#define D_START_VBLANK (1u << (IS_ARM9 ? 27 : 28))
#define D_START_HBLANK (2u << 27) /* solo ARM9 */
#define D_START_DISP (3u << 27)   /* solo ARM9 */
#define D_IRQ (1u << 30)
#define D_ENABLE (1u << 31)

static volatile uint32_t n_dma[4];
static void h_dma0(void) { n_dma[0]++; }
static void h_dma1(void) { n_dma[1]++; }
static void h_dma2(void) { n_dma[2]++; }
static void h_dma3(void) { n_dma[3]++; }

static volatile uint32_t *const BUFW = (volatile uint32_t *)BUF;
static volatile uint16_t *const BUFH = (volatile uint16_t *)BUF;

static void dma_set(unsigned ch, uint32_t src, uint32_t dst, uint32_t cnt)
{
	MEM32(R_DMA(ch, 0)) = src;
	MEM32(R_DMA(ch, 1)) = dst;
	MEM32(R_DMA(ch, 2)) = cnt;
	/* el DMA empieza ~2 ciclos después de habilitarlo y la CPU puede ejecutar todavía una o dos instrucciones: sin esto,
	 * en una DS real el ARM7 leía a veces la primera palabra del destino antes de que llegara (ver docs/ds-real.md) */
	__asm__ volatile("nop\n\tnop\n\tnop\n\tnop\n\tnop\n\tnop" : : : "memory");
}

/* primer elemento que no coincide en un bucle de comprobación: se anota como divergencia (grupo, línea*1000+índice) */
static uint32_t dbg_set, dbg_i, dbg_got, dbg_want;
#define NOTE_BAD(i, g, w)                         \
	do {                                      \
		if ((g) != (w) && !dbg_set) {     \
			dbg_set = 1;              \
			dbg_i = (i);              \
			dbg_got = (g);            \
			dbg_want = (w);           \
		}                                 \
	} while (0)
#define REPORT_BAD(line)                                                                                   \
	do {                                                                                               \
		if (dbg_set) {                                                                             \
			sds_diverge_at(dbg_got, dbg_want, SDS_MELON_NA, (line) * 1000u + dbg_i, __FILE__); \
			dbg_set = 0;                                                                       \
		}                                                                                          \
	} while (0)

static void fill_words(uint32_t off, uint32_t n, uint32_t start)
{
	for (uint32_t i = 0; i < n; i++)
		BUFW[off / 4 + i] = start + i;
}

static void clear_words(uint32_t off, uint32_t n)
{
	for (uint32_t i = 0; i < n; i++)
		BUFW[off / 4 + i] = 0;
}

static void group_copy(void)
{
	uint32_t bad;

	sds_group("COPIA");
	for (unsigned ch = 0; ch < 4; ch++) {
		/* palabras de 32 bits, canal ch */
		fill_words(0, 64, 0xA0000000u + ch * 0x100);
		clear_words(0x400, 66);
		dma_set(ch, BUF, BUF + 0x400, D_ENABLE | D_WORD | 64);
		bad = 0;
		for (uint32_t i = 0; i < 64; i++)
			{ const uint32_t g_ = (uint32_t)(BUFW[0x100 + i]), w_ = (uint32_t)(0xA0000000u + ch * 0x100 + i); bad += g_ != w_; NOTE_BAD(i, g_, w_); }
		SDS_CHECK(bad, 0);
		REPORT_BAD(__LINE__);
		SDS_CHECK(BUFW[0x100 + 64], 0); /* no pasa de la cuenta */
		SDS_CHECK(MEM32(R_DMA(ch, 2)) & D_ENABLE, 0); /* termina y se deshabilita */
		SDS_CHECK(MEM32(R_DMA(ch, 0)), BUF + 4 * 64 - 4 * 64 + 0); /* SAD no cambia */
	}
	/* 16 bits */
	for (uint32_t i = 0; i < 100; i++)
		BUFH[i] = (uint16_t)(0x1000 + i);
	clear_words(0x400, 60);
	dma_set(0, BUF, BUF + 0x400, D_ENABLE | 100);
	bad = 0;
	for (uint32_t i = 0; i < 100; i++)
		{ const uint32_t g_ = (uint32_t)(BUFH[0x200 + i]), w_ = (uint32_t)(0x1000 + i); bad += g_ != w_; NOTE_BAD(i, g_, w_); }
	SDS_CHECK(bad, 0);
	REPORT_BAD(__LINE__);
	SDS_CHECK(BUFH[0x200 + 100], 0);
	/* una unidad */
	BUFW[0] = 0xCAFEF00Du;
	BUFW[0x100] = 0;
	BUFW[0x101] = 0;
	dma_set(1, BUF, BUF + 0x400, D_ENABLE | D_WORD | 1);
	SDS_CHECK(BUFW[0x100], 0xCAFEF00Du);
	SDS_CHECK(BUFW[0x101], 0);
	/* las direcciones se alinean a la unidad */
	BUFW[0x10] = 0x11223344u;
	BUFW[0x11] = 0x55667788u;
	BUFW[0x100] = 0;
	dma_set(2, BUF + 0x40 + 1, BUF + 0x400 + 1, D_ENABLE | D_WORD | 1);
	SDS_CHECK(BUFW[0x100], 0x11223344u);
}

static void group_modes(void)
{
	uint32_t bad;

	sds_group("MODOS");
	/* destino decreciente */
	fill_words(0, 8, 100);
	clear_words(0x400, 16);
	dma_set(0, BUF, BUF + 0x400 + 4 * 7, D_ENABLE | D_WORD | D_DST_DEC | 8);
	bad = 0;
	for (uint32_t i = 0; i < 8; i++)
		{ const uint32_t g_ = (uint32_t)(BUFW[0x100 + 7 - i]), w_ = (uint32_t)(100 + i); bad += g_ != w_; NOTE_BAD(i, g_, w_); }
	SDS_CHECK(bad, 0);
	REPORT_BAD(__LINE__);
	/* origen decreciente */
	clear_words(0x400, 16);
	dma_set(0, BUF + 4 * 7, BUF + 0x400, D_ENABLE | D_WORD | D_SRC_DEC | 8);
	bad = 0;
	for (uint32_t i = 0; i < 8; i++)
		{ const uint32_t g_ = (uint32_t)(BUFW[0x100 + i]), w_ = (uint32_t)(107 - i); bad += g_ != w_; NOTE_BAD(i, g_, w_); }
	SDS_CHECK(bad, 0);
	REPORT_BAD(__LINE__);
	/* destino fijo */
	clear_words(0x400, 16);
	dma_set(0, BUF, BUF + 0x400, D_ENABLE | D_WORD | D_DST_FIX | 8);
	SDS_CHECK(BUFW[0x100], 107);
	SDS_CHECK(BUFW[0x101], 0);
	/* origen fijo: rellena */
	BUFW[0x20] = 0xF111F111u;
	clear_words(0x400, 16);
	dma_set(0, BUF + 0x80, BUF + 0x400, D_ENABLE | D_WORD | D_SRC_FIX | 12);
	bad = 0;
	for (uint32_t i = 0; i < 12; i++)
		{ const uint32_t g_ = (uint32_t)(BUFW[0x100 + i]), w_ = (uint32_t)(0xF111F111u); bad += g_ != w_; NOTE_BAD(i, g_, w_); }
	SDS_CHECK(bad, 0);
	REPORT_BAD(__LINE__);
	SDS_CHECK(BUFW[0x100 + 12], 0);
	/* los registros de dirección guardan lo escrito (con la máscara de la CPU) y la cuenta se lee de vuelta */
	MEM32(R_DMA(3, 0)) = 0x02001234;
	MEM32(R_DMA(3, 1)) = 0x02005678;
	SDS_CHECK(MEM32(R_DMA(3, 0)), 0x02001234);
	SDS_CHECK(MEM32(R_DMA(3, 1)), 0x02005678);
	MEM32(R_DMA(3, 2)) = 0x00000123;
	SDS_CHECK(MEM32(R_DMA(3, 2)), 0x123);
	MEM32(R_DMA(3, 2)) = 0;
	/* DMA con cuenta que no termina por un cambio de CNT mientras corre: con cnt = 0 en el canal 3 del ARM7 son
	 * 0x10000 unidades (se prueba aparte con una cuenta pequeña) */
}

static void group_irq(void)
{
	sds_group("IRQ");
	irqSet(IRQ_DMA0, h_dma0);
	irqSet(IRQ_DMA1, h_dma1);
	irqSet(IRQ_DMA2, h_dma2);
	irqSet(IRQ_DMA3, h_dma3);
	irqEnable(IRQ_DMA0 | IRQ_DMA1 | IRQ_DMA2 | IRQ_DMA3);
	MEM32(R_IME) = 1;
	for (unsigned ch = 0; ch < 4; ch++) {
		n_dma[ch] = 0;
		dma_set(ch, BUF, BUF + 0x400, D_ENABLE | D_WORD | D_IRQ | 16);
		SDS_CHECK(WAIT_UNTIL(n_dma[ch] != 0, 100000), 1);
		SDS_CHECK(n_dma[ch], 1);
	}
	/* sin el bit de IRQ no hay IRQ */
	n_dma[0] = 0;
	dma_set(0, BUF, BUF + 0x400, D_ENABLE | D_WORD | 16);
	for (volatile int i = 0; i < 2000; i++)
		;
	SDS_CHECK(n_dma[0], 0);
	irqDisable(IRQ_DMA0 | IRQ_DMA1 | IRQ_DMA2 | IRQ_DMA3);
}

static void group_priority(void)
{
	sds_group("CANALES");
	/* dos canales inmediatos: se corren en orden y los dos terminan */
	fill_words(0, 16, 500);
	clear_words(0x400, 40);
	MEM32(R_DMA(0, 0)) = BUF;
	MEM32(R_DMA(0, 1)) = BUF + 0x400;
	MEM32(R_DMA(1, 0)) = BUF + 0x40;
	MEM32(R_DMA(1, 1)) = BUF + 0x440;
	MEM32(R_DMA(0, 2)) = D_ENABLE | D_WORD | 16;
	MEM32(R_DMA(1, 2)) = D_ENABLE | D_WORD | 16;
	SDS_CHECK(BUFW[0x100], 500);
	SDS_CHECK(BUFW[0x100 + 15], 515);
	SDS_CHECK(BUFW[0x110], BUFW[16]);
	SDS_CHECK((MEM32(R_DMA(0, 2)) | MEM32(R_DMA(1, 2))) & D_ENABLE, 0);
}

static void group_vblank(void)
{
	uint32_t bad, k;

	sds_group("VBLANK");
	/* inicio en VBlank, repetir, destino creciente: cada VBlank añade cuatro palabras */
	for (uint32_t i = 0; i < 16; i++)
		BUFW[i] = 0x7000 + i;
	clear_words(0x400, 40);
	WAIT_VBLANK();
	dma_set(0, BUF, BUF + 0x400, D_ENABLE | D_WORD | D_REPEAT | D_START_VBLANK | 4);
	SDS_CHECK(BUFW[0x100], 0); /* todavía no */
	SDS_CHECK(MEM32(R_DMA(0, 2)) & D_ENABLE, D_ENABLE);
	for (k = 1; k <= 3; k++) {
		WAIT_VBLANK();
		for (volatile int i = 0; i < 400; i++)
			;
		bad = 0;
		for (uint32_t i = 0; i < 4 * k; i++)
			{ const uint32_t g_ = (uint32_t)(BUFW[0x100 + i]), w_ = (uint32_t)(0x7000 + i); bad += g_ != w_; NOTE_BAD(i, g_, w_); }
		SDS_CHECK(bad, 0);
		REPORT_BAD(__LINE__);
		SDS_CHECK(BUFW[0x100 + 4 * k], 0);
	}
	MEM32(R_DMA(0, 2)) = 0; /* se detiene escribiendo cero */
	WAIT_VBLANK();
	for (volatile int i = 0; i < 400; i++)
		;
	SDS_CHECK(BUFW[0x100 + 12], 0);
	/* sin repetir: una sola vez y se deshabilita */
	clear_words(0x400, 40);
	WAIT_VBLANK();
	dma_set(1, BUF, BUF + 0x400, D_ENABLE | D_WORD | D_START_VBLANK | 4);
	WAIT_VBLANK();
	for (volatile int i = 0; i < 400; i++)
		;
	SDS_CHECK(BUFW[0x103], 0x7003);
	SDS_CHECK(MEM32(R_DMA(1, 2)) & D_ENABLE, 0);
	/* destino con recarga: cada VBlank escribe en el mismo sitio y la fuente sigue */
	clear_words(0x400, 40);
	WAIT_VBLANK();
	dma_set(2, BUF, BUF + 0x400, D_ENABLE | D_WORD | D_REPEAT | D_START_VBLANK | D_DST_RELOAD | 2);
	WAIT_VBLANK();
	for (volatile int i = 0; i < 400; i++)
		;
	SDS_CHECK(BUFW[0x100], 0x7000);
	SDS_CHECK(BUFW[0x101], 0x7001);
	WAIT_VBLANK();
	for (volatile int i = 0; i < 400; i++)
		;
	SDS_CHECK(BUFW[0x100], 0x7002); /* la fuente avanzó, el destino se recargó */
	SDS_CHECK(BUFW[0x102], 0);
	MEM32(R_DMA(2, 2)) = 0;
}

#endif
