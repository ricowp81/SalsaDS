/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * SalsaDS — ROM cpuarm, lado ARM7 (ARM7TDMI, ARMv4T): corre los grupos de
 * cpu9 que existen en ARMv4T y los nuevos, y deja el veredicto en su propio
 * bloque de resultados (sds7.c). El ARM9 lo lee y lo suma al suyo.
 */
#pragma GCC diagnostic ignored "-Wunused-function"
#include "../cpu9/groups.h"
#include "groups2.h"

int main(void)
{
	/* sin interrupciones: libnds las deja habilitadas y una IRQ en medio de las pruebas que mueven las TCM o
	 * cambian de modo las rompería */
	__asm__ volatile("msr cpsr_c, #0xDF" : : : "cc");
	sds_init("CPU ARM7");
	group_shifter();
	group_alu();
	group_cond();
	group_mul();
	group_loadstore();
	group_ldm();
	group_interwork();
	group_thumb();
	group_align();
	group_ldm_edge();
	group_modes();
	group_interwork2();
	group_thumb2();
	sds_finish();
}
