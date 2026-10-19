/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * SalsaDS — ROM ipc, lado ARM7: atiende las órdenes del ARM9 sobre IPCSYNC y las FIFO (ipc.h) y cuenta las IRQ
 * de sincronía que le llegan.
 */
#include <nds.h>

#include "../common/hw.h"
#include "ipc.h"

static volatile uint32_t n_sync;

static void h_sync(void) { n_sync++; }

static void service(void)
{
	for (;;) {
		const uint32_t cmd = MBOX->cmd;

		if (!cmd)
			continue;
		MBOX->cmd = 0;
		switch (cmd) {
		case C_SYNC_SET:
			REG16(R_IPCSYNC) = (uint16_t)((MBOX->arg[0] & 0xF) << 8);
			break;
		case C_SYNC_GET:
			MBOX->res[0] = REG16(R_IPCSYNC);
			break;
		case C_SYNC_PULSE:
			REG16(R_IPCSYNC) = (uint16_t)(0x2000 | ((MBOX->arg[0] & 0xF) << 8));
			break;
		case C_SYNC_ARM:
			n_sync = 0;
			irqSet(IRQ_IPC_SYNC, h_sync);
			MEM32(R_IE) |= IRQ_IPC_SYNC;
			REG16(R_IPCSYNC) = 0x4000;
			break;
		case C_SYNC_COUNT:
			MBOX->res[0] = n_sync;
			MBOX->res[1] = REG16(R_IPCSYNC);
			break;
		case C_FIFO_CNT:
			REG16(R_IPCFIFOCNT) = (uint16_t)MBOX->arg[0];
			break;
		case C_FIFO_RECV: {
			const uint32_t n = MBOX->arg[0];
			uint32_t valid = 0, last = 0, bad = 0, expect = 0xA0000001u, i;

			for (i = 0; i < n; i++) {
				const uint32_t empty = (REG16(R_IPCFIFOCNT) & 0x100) != 0;
				const uint32_t v = REG32(R_IPCFIFORECV);

				if (!empty) {
					valid++;
					if (v != expect && (v & 0xF0000000u) == 0xA0000000u)
						bad++;
					expect = v + 1;
				}
				last = v;
			}
			MBOX->res[0] = valid;
			MBOX->res[1] = last;
			MBOX->res[2] = REG16(R_IPCFIFOCNT);
			MBOX->res[3] = bad;
			REG16(R_IPCFIFOCNT) = 0x8000 | 0x4000;
			break;
		}
		case C_FIFO_SEND:
			for (uint32_t i = 0; i < MBOX->arg[0]; i++)
				REG32(R_IPCFIFOSEND) = MBOX->arg[1] + i;
			break;
		case CMD_QUIT:
			MBOX->done = 1;
			return;
		default:
			break;
		}
		MBOX->done = 1;
	}
}

int main(void)
{
	__asm__ volatile("msr cpsr_c, #0xDF" : : : "cc");
	sds_init("IPC ARM7");
	irqInit();
	MEM32(R_IME) = 1;
	__asm__ volatile("msr cpsr_c, #0x1F" : : : "cc");
	service();
	sds_finish();
}
