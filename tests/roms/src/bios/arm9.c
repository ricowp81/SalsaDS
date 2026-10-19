/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * SalsaDS — ROM bios, lado ARM9: las SWI de la BIOS (biost.h), con los filtros de diferencias que solo tiene
 * el ARM9, y el veredicto del ARM7.
 */
#define IS_ARM9 1
#define BUF 0x02210000u

#include "biost.h"

int main(void)
{
	sds_nocache();
	sds_init("BIOS ARM9+ARM7");
	MEM32(R_IME) = 1;
	group_math();
	group_copy();
	group_decomp();
	group_regs();
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
