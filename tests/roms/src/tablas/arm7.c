/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * SalsaDS — ROM tablas, lado ARM7: llama a las tres SWI de tablas de la BIOS del ARM7 (1Ah GetSineTable, 1Bh
 * GetPitchTable, 1Ch GetVolumeTable) con todos los índices válidos y deja los resultados en RAM principal
 * (TABLAS_ADDR, una palabra por índice) para que el ARM9 los compare con las tablas generadas por su fórmula.
 */
#include "../common/hw.h"
#include "tablas.h"

#define OUT ((volatile uint32_t *)TABLAS_ADDR)

static inline uint32_t swi_1a(uint32_t i)
{
	register uint32_t r0 __asm__("r0") = i;

	__asm__ volatile("swi 0x1A0000" : "+r"(r0) : : "r1", "r2", "r3", "r12", "lr", "memory", "cc");
	return r0;
}

static inline uint32_t swi_1b(uint32_t i)
{
	register uint32_t r0 __asm__("r0") = i;

	__asm__ volatile("swi 0x1B0000" : "+r"(r0) : : "r1", "r2", "r3", "r12", "lr", "memory", "cc");
	return r0;
}

static inline uint32_t swi_1c(uint32_t i)
{
	register uint32_t r0 __asm__("r0") = i;

	__asm__ volatile("swi 0x1C0000" : "+r"(r0) : : "r1", "r2", "r3", "r12", "lr", "memory", "cc");
	return r0;
}

int main(void)
{
	__asm__ volatile("msr cpsr_c, #0xDF" : : : "cc");
	MEM32(R_IME) = 0;
	for (uint32_t i = 0; i < TAB_SINE_N; i++)
		OUT[i] = swi_1a(i);
	for (uint32_t i = 0; i < TAB_PITCH_N; i++)
		OUT[TAB_SINE_N + i] = swi_1b(i);
	for (uint32_t i = 0; i < TAB_VOL_N; i++)
		OUT[TAB_SINE_N + TAB_PITCH_N + i] = swi_1c(i);
	/* índices fuera de rango (comportamiento de la BIOS que se compara con melonDS, no con documentación) */
	{
		volatile uint32_t *x = OUT + TAB_SINE_N + TAB_PITCH_N + TAB_VOL_N;

		x[0] = swi_1a(64);
		x[1] = swi_1a(100);
		x[2] = swi_1a(0xFFFFFFFFu);
		x[3] = swi_1b(768);
		x[4] = swi_1b(1000);
		x[5] = swi_1b(0xFFFFFFFFu);
		x[6] = swi_1c(724);
		x[7] = swi_1c(1000);
		x[8] = swi_1c(0xFFFFFFFFu);
	}
	MBOX->done = 1;
	for (;;)
		;
}
