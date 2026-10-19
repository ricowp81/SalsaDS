/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * SalsaDS — ROM tablas, lado ARM9: espera a que el ARM7 llene la tabla con las SWI 1Ah-1Ch y la deja en RAM
 * (0x02300000) y la compara con ref.h, que genera `tools/gen-bios-tables.py` a partir de la fórmula de cada tabla
 * (verificada contra melonDS con `tools/melonds-dump.py`).
 */
#include "../common/hw.h"
#include "tablas.h"
#include "ref.h"

int main(void)
{
	sds_init("TABLAS ARM7");
	sds_nocache();
	sds_group("TABLAS");
	SDS_CHECK(WAIT_UNTIL(MBOX->done, 40000000u), 1u);
	{
		const volatile uint32_t *t = (const volatile uint32_t *)TABLAS_ADDR;
		uint32_t bad;

		sds_group("SENO");
		bad = 0;
		for (uint32_t i = 0; i < TAB_SINE_N; i++)
			bad += t[i] != ref_sine[i];
		SDS_CHECK(bad, 0);
		sds_group("TONO");
		bad = 0;
		for (uint32_t i = 0; i < TAB_PITCH_N; i++)
			bad += t[TAB_SINE_N + i] != ref_pitch[i];
		SDS_CHECK(bad, 0);
		sds_group("VOLUMEN");
		bad = 0;
		for (uint32_t i = 0; i < TAB_VOL_N; i++)
			bad += t[TAB_SINE_N + TAB_PITCH_N + i] != ref_volume[i];
		SDS_CHECK(bad, 0);
	}
	sds_finish();
}
