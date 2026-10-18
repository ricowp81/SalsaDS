/*
 * SalsaDS — ROM snd, lado ARM9: espera a que el ARM7 termine los casos de sonido (que dejan en RAM lo capturado del
 * mezclador, 0x02400000). Sin veredicto propio: las muestras se comparan con tools/snd-compare.py contra melonDS.
 */
#include "../common/hw.h"

int main(void)
{
	sds_init("SONIDO");
	sds_nocache();
	sds_group("SONIDO");
	SDS_CHECK(WAIT_UNTIL(MBOX->done, 400000000u), 1u);
	sds_finish();
}
