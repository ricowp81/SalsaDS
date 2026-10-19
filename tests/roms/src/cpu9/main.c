/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * SalsaDS — ROM de pruebas de CPU del ARM9 (ARM946E-S, ARMv5TE). Los grupos
 * están en groups.h (los comparte la ROM cpuarm, que también los corre en el
 * ARM7).
 */
#include "groups.h"

int main(void)
{
	sds_init("CPU ARM9 ARMV5TE");
	group_shifter();
	group_alu();
	group_cond();
	group_mul();
	group_v5();
	group_loadstore();
	group_ldm();
	group_interwork();
	group_thumb();
	sds_finish();
}
