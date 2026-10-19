/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * SalsaDS — ROM ocio: las dos CPUs esperando el VBlank sin hacer nada más (IntrWait de la BIOS). Sirve para medir
 * cuánto cuesta un cuadro ocioso del planificador: por cuadro cada CPU ejecuta unas decenas de instrucciones
 * (la entrada a la IRQ, un contador y la vuelta a Halt).
 */
#include <nds.h>

#include "../common/hw.h"

static volatile uint32_t n_vblank;
static void h_vblank(void) { n_vblank++; }

int main(void)
{
	MEM32(R_IME) = 0;
	irqSet(IRQ_VBLANK, h_vblank);
	irqEnable(IRQ_VBLANK);
	MEM32(R_IME) = 1;
	for (;;)
		swiWaitForVBlank();
}
