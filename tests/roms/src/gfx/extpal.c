/*
 * SalsaDS — ROM «gfxextpal»: paletas extendidas de fondos y de OBJ del motor A. Los bancos E (fondos) y F (OBJ) se
 * llenan como LCDC con colores que dependen de (ranura, número de paleta, color) y se mapean como paletas extendidas;
 * cuatro fondos de texto de 8 bits (ranuras 0..3, una con el bit 13 de BG1CNT que la manda a la 3) y ocho OBJ de 8 bits
 * con números de paleta distintos. Con EXT = 1 se apaga el bit de paletas extendidas de los fondos (se ve la
 * paleta normal). Vale la pena compararlo con melonDS (captura del motor A).
 */
#include "gfx.h"

#ifndef EXT
#define EXT 0
#endif
#ifndef BIT15
#define BIT15 0
#endif

static uint16_t ecol(int slot, int pal, int c)
{
	return RGB5((slot * 8 + pal * 2 + c * 5) & 31, (slot * 3 + pal * 5 + c * 7) & 31, (slot * 11 + pal * 13 + c * 3 + 5) & 31);
}

int main(void)
{
	const uint32_t cb = BGV(0), ob = OBJV(0);

	sds_init_quiet();
	gfx_power();
	/* A LCDC; B fondos de A; E LCDC (para llenarlo); F LCDC; G OBJ de A (16 KB) */
	REG8(R_VRAMCNT(0)) = 0x80;
	REG8(R_VRAMCNT(1)) = 0x81;
	REG8(R_VRAMCNT(4)) = 0x80;
	REG8(R_VRAMCNT(5)) = 0x80;
	REG8(R_VRAMCNT(6)) = 0x82;
	gfx_clear32(0x06000000u, 0x20000u);
	gfx_clear32(0x06400000u, 0x4000u);
	for (int i = 0; i < 128; i++) {
		OAM(0)[i * 4] = 0x200;
		OAM(0)[i * 4 + 1] = OAM(0)[i * 4 + 2] = OAM(0)[i * 4 + 3] = 0;
	}
	/* paletas extendidas de fondo (bank E, 4 ranuras de 4096 colores... 8 KB cada una) y de OBJ (bank F) */
	for (int slot = 0; slot < 4; slot++)
		for (int pal = 0; pal < 16; pal++)
			for (int c = 0; c < 256; c++)
				V16(0x06880000u + (uint32_t)slot * 0x2000u)[pal * 256 + c] = ecol(slot, pal, c & 15) | (uint16_t)((BIT15 && (pal & 1)) ? 0x8000 : 0);
	for (int pal = 0; pal < 16; pal++)
		for (int c = 0; c < 256; c++)
			V16(0x06890000u)[pal * 256 + c] = ecol(4, pal, c & 15) | (uint16_t)((BIT15 && (pal & 1)) ? 0x8000 : 0);
	REG8(R_VRAMCNT(4)) = 0x84; /* E: paletas extendidas de fondo */
	REG8(R_VRAMCNT(5)) = 0x85; /* F: paleta extendida de OBJ */
	PAL_BG(0)[0] = RGB5(0, 0, 0);
	for (int i = 1; i < 256; i++) {
		PAL_BG(0)[i] = RGB5(31, (i * 3) & 31, 0); /* la paleta normal es naranja: se nota si se usa */
		PAL_OBJ(0)[i] = RGB5(0, (i * 3) & 31, 31);
	}
	/* teselas de 8 bits: la tesela t es de color 1 + t % 15 */
	for (int t = 1; t < 16; t++) {
		uint8_t u[64];

		for (int i = 0; i < 64; i++)
			u[i] = (uint8_t)(1 + t % 15);
		gfx_copy16(cb + 64u * (uint32_t)t, u, 64);
		gfx_copy16(ob + 64u * (uint32_t)t, u, 64);
	}
	/* cuatro fondos: mapa en los bloques 8..11 (0x4000...), 8 bits, char base 0 */
	for (int b = 0; b < 4; b++) {
		uint16_t cnt = (uint16_t)((8 + b) << 8) | 0x80 | (uint16_t)b; /* prioridad b */

		if (b == 1)
			cnt |= 0x2000; /* BG1: ranura 3 */
		ER16(0, E_BGCNT(b)) = cnt;
		for (int ty = 0; ty < 4; ty++)
			for (int tx = 0; tx < 32; tx++)
				V16(cb + 0x4000u + 0x800u * (uint32_t)b)[(b * 6 + ty) * 32 + tx] = (uint16_t)((1 + ((tx + ty) % 15)) | (((tx * 3 + ty + b) & 15) << 12));
	}
	/* ocho OBJ de 8×8, 8 bits, 1D con frontera de 64 bytes (tesela n en el desplazamiento n * 64) */
	for (int i = 0; i < 8; i++) {
		OAM(0)[i * 4] = (uint16_t)((150 + (i / 4) * 12) | 0x2000);
		OAM(0)[i * 4 + 1] = (uint16_t)(20 + (i % 4) * 14 + (i / 4) * 7);
		OAM(0)[i * 4 + 2] = (uint16_t)((1 + i) * 2 | (i * 2 << 12));
	}
	ER32(0, E_DISPCNT) = DC_GRAPHICS | DC_BG(0) | DC_BG(1) | DC_BG(2) | DC_BG(3) | DC_OBJ | DC_OBJ1D | (1u << 20) |
			     (EXT ? 0 : DC_BGEXTPAL) | DC_OBJEXTPAL;
	gfx_finish();
}
