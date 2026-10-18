/*
 * SalsaDS — ROM «gfxprobe»: ¿desde qué línea se ve un cambio de registro? Un fondo rojo sobre el color de fondo azul,
 * y ocho veces por cuadro la ROM apaga o enciende el fondo (DISPCNT): cuatro veces justo después de ver la línea L
 * en VCOUNT (principio de la línea) y cuatro después de ver el HBlank de la línea L. Las bandas de la imagen
 * muestran la línea en que cada cambio se ve por primera vez.
 */
#include "gfx.h"

int main(void)
{
	static const uint8_t lines[8] = { 20, 40, 60, 80, 100, 120, 140, 160 };

	sds_init_quiet();
	gfx_power();
	gfx_banks();
	gfx_clear_all();
	PAL_BG(0)[0] = RGB5(0, 0, 31);
	PAL_BG(0)[1] = RGB5(31, 0, 0);
	gfx_tile4(BGV(0) + 32, 1);
	for (int i = 0; i < 1024; i++)
		V16(BGV(0) + 0x4000)[i] = 1;
	ER16(0, E_BGCNT(0)) = 8 << 8;
	for (int frame = 0; frame < 8; frame++) {
		gfx_vsync();
		ER32(0, E_DISPCNT) = DC_GRAPHICS | DC_BG(0);
		if (frame == 2)
			gfx_capture();
		if (frame == 5)
			break;
		for (int k = 0; k < 8; k++) {
			while (REG16(R_VCOUNT) != lines[k])
				;
			if (k & 1)
				while (!(REG16(R_DISPSTAT) & 2))
					;
			ER32(0, E_DISPCNT) ^= DC_BG(0);
		}
	}
	sds_finish_quiet(1);
}
