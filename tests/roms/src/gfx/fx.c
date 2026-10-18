/*
 * SalsaDS — ROM «gfxfx»: caracteriza los efectos de color del motor A contra melonDS. Dos rampas de colores de 32 pasos
 * en el eje X (un fondo de 8 bits con una tesela sólida por color), y por bandas de líneas se cambia BLDALPHA/BLDY
 * escribiendo en el HBlank de la línea anterior. FX = 0: brillo (subir, bajar: 17 factores cada uno); FX = 1: mezcla
 * alfa de BG0 sobre BG1 con parejas (EVA, EVB).
 */
#include "gfx.h"

#ifndef FX
#define FX 0
#endif

static void at_hblank(int line)
{
	while (REG16(R_VCOUNT) != line)
		;
	while (!(REG16(R_DISPSTAT) & 2))
		;
}

int main(void)
{
	sds_init_quiet();
	gfx_power();
	gfx_banks();
	gfx_clear_all();
	/* BG0: tesela k (k = 0..31) = color k + 1; BG1: tesela k = color 40 + k */
	for (int k = 0; k < 32; k++) {
		gfx_tile8(BGV(0) + 0x10000 + 64 * (uint32_t)k, 1 + k);
		gfx_tile8(BGV(0) + 0x10000 + 64 * (uint32_t)(32 + k), 40 + k);
		PAL_BG(0)[1 + k] = RGB5(k, 31 - k, (k * 5) & 31);
		PAL_BG(0)[40 + k] = RGB5((k * 3 + 7) & 31, (k * 7) & 31, 31 - ((k * 2) & 31));
	}
	PAL_BG(0)[0] = RGB5(9, 4, 20);
	for (int y = 0; y < 32; y++)
		for (int x = 0; x < 32; x++) {
			V16(BGV(0) + 0x0000)[y * 32 + x] = (uint16_t)x;        /* BG0: mapa en 0x0000 */
			V16(BGV(0) + 0x0800)[y * 32 + x] = (uint16_t)(32 + x); /* BG1: mapa en 0x0800 */
		}
	ER16(0, E_BGCNT(0)) = 0x80 | (0 << 8) | (4 << 2); /* 256 colores, mapa 0x0000, teselas 0x10000 */
	ER16(0, E_BGCNT(1)) = 0x80 | (1 << 8) | (4 << 2) | 1;
	for (int frame = 0; frame < 8; frame++) {
		gfx_vsync();
		ER32(0, E_DISPCNT) = DC_GRAPHICS | DC_BG(0) | DC_BG(1);
#if FX == 0
		ER16(0, E_BLDCNT) = 0x0001 | (2 << 6);
		ER16(0, E_BLDY) = 0;
#else
		ER16(0, E_BLDCNT) = 0x0001 | (1 << 6) | (0x0002 << 8);
		ER16(0, E_BLDALPHA) = 0;
#endif
		if (frame == 2)
			gfx_capture();
		if (frame == 5)
			break;
#if FX == 0
		for (int g = 0; g < 34; g++) { /* g < 17 sube, g >= 17 baja; 5 líneas por grupo */
			if (g == 17) {
				at_hblank(g * 5 - 1);
				ER16(0, E_BLDCNT) = 0x0001 | (3 << 6);
			} else if (g > 0) {
				at_hblank(g * 5 - 1);
			}
			ER16(0, E_BLDY) = (uint16_t)(g % 17);
		}
#else
		{
			static const uint8_t ev[24][2] = { { 0, 16 }, { 1, 15 }, { 2, 14 }, { 3, 13 }, { 4, 12 }, { 5, 11 },
							   { 6, 10 }, { 7, 9 }, { 8, 8 }, { 9, 7 }, { 10, 6 }, { 11, 5 },
							   { 12, 4 }, { 13, 3 }, { 14, 2 }, { 15, 1 }, { 16, 0 }, { 16, 16 },
							   { 8, 16 }, { 16, 8 }, { 5, 5 }, { 3, 7 }, { 20, 20 }, { 31, 1 } };
			for (int g = 0; g < 24; g++) {
				if (g > 0)
					at_hblank(g * 8 - 1);
				ER16(0, E_BLDALPHA) = (uint16_t)(ev[g][0] | (ev[g][1] << 8));
			}
		}
#endif
	}
	sds_finish_quiet(1);
}
