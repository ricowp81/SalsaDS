/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * SalsaDS — ROM «gfxobjmap»: ¿qué ve el motor 2D en la parte de la VRAM de OBJ que no tiene banco? Solo el banco G (16 KB)
 * está mapeado como VRAM de OBJ del motor A; OBJ de 8×8 y 8 bits leen teselas por todo el espacio de 256 KB. Los colores
 * de las teselas sólidas dependen del número de tesela (1 + t % 250) → se ve si lo que aparece es un espejo del banco.
 * MAP = 1: con el banco F también (16 KB más, en el desplazamiento 16 KB).
 */
#include "gfx.h"

#ifndef MAP
#define MAP 0
#endif
#ifndef BPP4
#define BPP4 0
#endif
#ifndef SHIFT
#define SHIFT 1
#endif

int main(void)
{
	sds_init_quiet();
	gfx_power();
	for (int b = 0; b < 7; b++)
		REG8(R_VRAMCNT(b)) = 0;
	REG8(R_VRAMCNT(8)) = 0;
	REG8(R_VRAMCNT(9)) = 0;
	REG8(R_VRAMCNT(0)) = 0x80;
	REG8(R_VRAMCNT(5)) = 0x80;
	REG8(R_VRAMCNT(6)) = 0x80;
	/* G (LCDC 0x06894000) y F (0x06890000) llenos de teselas sólidas de 8 bits, la tesela t del banco con color 1 + (t % 250) */
	for (int t = 0; t < 256; t++) {
		uint8_t u[64];

		for (int i = 0; i < 64; i++)
			u[i] = BPP4 ? (uint8_t)((1 + (t % 15)) * 0x11) : (uint8_t)(1 + t % 250);
		gfx_copy16(0x06894000u + 64u * (uint32_t)t, u, 64);
		for (int i = 0; i < 64; i++)
			u[i] = BPP4 ? (uint8_t)((1 + ((t + 7) % 15)) * 0x11) : (uint8_t)(1 + (t + 100) % 250);
		gfx_copy16(0x06890000u + 64u * (uint32_t)t, u, 64);
	}
	REG8(R_VRAMCNT(6)) = 0x82; /* G: OBJ de A, desplazamiento 0 */
#if MAP
	REG8(R_VRAMCNT(5)) = 0x8A; /* F: OBJ de A, OFS 1 (+16 KB) */
#endif
	for (int i = 0; i < 128; i++) {
		OAM(0)[i * 4] = 0x200;
		OAM(0)[i * 4 + 1] = OAM(0)[i * 4 + 2] = OAM(0)[i * 4 + 3] = 0;
	}
	for (int i = 0; i < 256; i++)
		PAL_OBJ(0)[i] = (uint16_t)RGB5((i * 5) & 31, 31 - ((i * 7) & 31), (i * 11) & 31);
	PAL_BG(0)[0] = 0;
	{
		static const uint16_t tiles[16] = { 0, 100, 255, 256, 300, 511, 512, 600, 700, 767, 768, 900, 1023, 400, 450, 480 };
		/* con otras fronteras y profundidades se leen más allá: se reparten las 16 teselas por todo el espacio de 1024 */

		for (int i = 0; i < 16; i++) {
			OAM(0)[i * 4] = (uint16_t)((10 + (i / 8) * 20) | (BPP4 ? 0 : 0x2000));
			OAM(0)[i * 4 + 1] = (uint16_t)(10 + (i % 8) * 20);
			OAM(0)[i * 4 + 2] = tiles[i];
		}
	}
	ER32(0, E_DISPCNT) = DC_GRAPHICS | DC_OBJ | DC_OBJ1D | ((uint32_t)SHIFT << 20);
	gfx_finish();
}
