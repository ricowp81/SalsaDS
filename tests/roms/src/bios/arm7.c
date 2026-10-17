/*
 * SalsaDS — ROM bios, lado ARM7: las mismas SWI (biost.h) sin los filtros de diferencias, que el ARM7 no tiene.
 */
#define IS_ARM9 0
#define BUF 0x02260000u

#include "biost.h"

int main(void)
{
	__asm__ volatile("msr cpsr_c, #0xDF" : : : "cc");
	sds_init("BIOS ARM7");
	irqInit();
	MEM32(R_IME) = 1;
	__asm__ volatile("msr cpsr_c, #0x1F" : : : "cc");
	group_math();
	group_copy();
	group_decomp();
	group_regs();
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
