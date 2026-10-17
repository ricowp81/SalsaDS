/*
 * SalsaDS — ROM irq, lado ARM9. Las pruebas están en irqt.h (las comparte con el ARM7). Con libnds:
 * irqSet/irqEnable, swiWaitForVBlank (en el ARM9 es código de libnds con el WFI de CP15) y las SWI 4 y 5 de
 * la BIOS (IntrWait y VBlankIntrWait) llamadas a mano.
 */
#include <nds.h>

#define WAIT_VBLANK() swiWaitForVBlank()
#define SWI_VBLANK_INTR_WAIT() __asm__ volatile("swi 0x50000" ::: "r0", "r1", "r2", "r3", "memory", "cc")
#define SWI_INTR_WAIT(clear, mask) \
	__asm__ volatile("mov r0, %0\n\tmov r1, %1\n\tswi 0x40000" : : "i"(clear), "i"(mask) : "r0", "r1", "r2", "r3", "memory", "cc")
#define CPU_HALT() __asm__ volatile("mcr p15, 0, %0, c7, c0, 4" : : "r"(0) : "memory")

#include "irqt.h"

int main(void)
{
	sds_nocache();
	MEM32(R_IME) = 0;
	sds_init("IRQ ARM9+ARM7");
	MEM32(R_IME) = 1;
	ref_start();
	group_video();
	group_vblank();
	group_hvcount();
	group_timers();
	group_ime();
	group_halt();
	group_keys();
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
