/* SalsaDS — ROM ocio, lado ARM7: espera el VBlank sin hacer nada más. */
#include <nds.h>

#include "../common/hw.h"

static volatile uint32_t n_vblank;
static void h_vblank(void) { n_vblank++; }

int main(void)
{
	irqInit();
	irqSet(IRQ_VBLANK, h_vblank);
	irqEnable(IRQ_VBLANK);
	MEM32(R_IME) = 1;
	for (;;)
		swiWaitForVBlank();
}
