/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * SalsaDS — ROM cpuarm, lado ARM9: ejecuta los grupos de cpu9 (ARM y Thumb en
 * ARMv5TE), los nuevos (alineación, LDM/STM, modos, cambio de estado, Thumb,
 * excepciones, CP15 y TCM) y suma al veredicto el del ARM7, que corre los
 * grupos comunes y los nuevos por su cuenta (arm7.c) y deja su bloque de
 * resultados en SDS_ARM7_RESULTS_ADDR.
 */
#pragma GCC diagnostic ignored "-Wunused-function"
#include "../cpu9/groups.h"
#include "groups2.h"
#include "groups9.h"

/* El ARM7 avisa de cómo terminó por IPCSYNC (E/S, que no pasa por la caché de datos del ARM9): 0xA = bien,
 * 0xB = fallos. El bloque de resultados del ARM7 (SDS_ARM7_RESULTS_ADDR) tiene el detalle y se lee desde el
 * emulador: la ARM9 puede tener una copia vieja en su caché. */
static void group_arm7(void)
{
	uint32_t spin = 0, nibble;

	sds_group("ARM7");
	/* El ARM7 termina antes. Se espera con un tope y sin acaparar la memoria principal: la ARM9 tiene prioridad en
	 * el bus y un giro continuo sobre ella le deja sin acceso. */
	for (;;) {
		uint32_t n = 100000;

		nibble = *(volatile uint32_t *)0x04000180 & 0xF;
		if (nibble == SDS_IPC_OK || nibble == SDS_IPC_FAIL || spin >= 20000u)
			break;
		__asm__ volatile("1: subs %0, %0, #1\n\t bne 1b" : "+r"(n) : : "cc");
		spin++;
	}
	SDS_CHECK(nibble, SDS_IPC_OK);
}

int main(void)
{
	/* sin interrupciones: libnds las deja habilitadas y una IRQ en medio de las pruebas que mueven las TCM o
	 * cambian de modo las rompería */
	__asm__ volatile("msr cpsr_c, #0xDF" : : : "cc");
	sds_init("CPU ARM9+ARM7");
	group_shifter();
	group_alu();
	group_cond();
	group_mul();
	group_v5();
	group_loadstore();
	group_ldm();
	group_interwork();
	group_thumb();
	group_align();
	group_ldm_edge();
	group_modes();
	group_interwork2();
	group_thumb2();
	group_exceptions();
	group_cp15();
	group_arm7();
	sds_finish();
}
