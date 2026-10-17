/*
 * SalsaDS — ROM irq, lado ARM7: las mismas pruebas que el ARM9 (irqt.h) con el manejador de libnds7, las SWI
 * de la BIOS del ARM7 (Halt, IntrWait, VBlankIntrWait) y el HALT con IME = 0, que en el ARM7 termina con
 * IE & IF != 0 aunque la IRQ no se entregue (GBATEK).
 */
#include <nds.h>

#define WAIT_VBLANK() swiWaitForVBlank()
#define SWI_VBLANK_INTR_WAIT() swiWaitForVBlank()
#define SWI_INTR_WAIT(clear, mask) swiIntrWait(clear, mask)
/* SWI 6 (Halt) con todos los registros como destruidos: la BIOS libre de melonDS 1.1 destruye uno de r4-r11
 * y la real los conserva; así la ROM no depende de ello (ver docs/fase1.md) */
#define CPU_HALT() \
	__asm__ volatile("swi 0x60000" ::: "r0", "r1", "r2", "r3", "r4", "r5", "r6", "r7", "r8", "r9", "r10", "r11", \
			 "r12", "memory", "cc")

#include "irqt.h"

static void group_halt_noime(void)
{
	uint32_t t0, t1, saved_ie;

	sds_group("HALT IME0");
	MEM32(R_IME) = 0;
	saved_ie = MEM32(R_IE);
	MEM32(R_IE) = IRQ_TIMER0; /* solo el temporizador: otra fuente terminaría el HALT antes */
	MEM16(R_TM_H(0)) = 0;
	MEM16(R_TM_L(0)) = 0xF000; /* 4096 ciclos de bus */
	MEM32(R_IF) = IRQ_TIMER0;
	t0 = ref_now();
	MEM16(R_TM_H(0)) = 0xC0;
	CPU_HALT(); /* con IME = 0 termina igual al llegar la IRQ */
	t1 = ref_now();
	MEM16(R_TM_H(0)) = 0;
	SDS_CHECK(t1 - t0 >= 4096 - 300 && t1 - t0 <= 4096 + 3000, 1);
	SDS_CHECK(MEM32(R_IF) & IRQ_TIMER0, IRQ_TIMER0);
	MEM32(R_IF) = IRQ_TIMER0;
	MEM32(R_IE) = saved_ie;
	MEM32(R_IME) = 1;
}

int main(void)
{
	__asm__ volatile("msr cpsr_c, #0xDF" : : : "cc"); /* IRQ enmascaradas hasta que haya manejadores */
	sds_init("IRQ ARM7");
	irqInit();
	MEM32(R_IME) = 1;
	__asm__ volatile("msr cpsr_c, #0x1F" : : : "cc");
	ref_start();
	group_video();
	group_vblank();
	group_hvcount();
	group_timers();
	group_ime();
	group_halt();
	group_halt_noime();
	group_keys();
	/* EXTKEYIN: ni X, ni Y, ni el lápiz apretados, la tapa abierta */
	sds_group("EXTKEYIN");
	SDS_CHECK(MEM16(R_EXTKEYIN) & 0xC3, 0x43);
	{
		volatile uint32_t spin = 0;

		for (;;) {
			const uint32_t cmd = MBOX->cmd;

			if (cmd == CMD_QUIT) {
				MBOX->done = 1;
				break;
			}
			if (++spin > 400000000u)
				break;
		}
	}
	sds_finish();
}
