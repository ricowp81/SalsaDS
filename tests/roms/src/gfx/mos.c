/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * SalsaDS — ROM «gfxmos»: mosaico de fondos y de OBJ contra melonDS. Fondo de texto con teselas de píxeles todos
 * distintos y desplazamiento, y un OBJ; el mosaico se pone grande para ver cómo se agrupan los píxeles. MOS = 0: solo
 * fondo; 1: solo OBJ normal; 2: OBJ afín; 3: fondo afín (BG2, modo 2).
 */
#include "gfx.h"

#ifndef MOS
#define MOS 0
#endif

int main(void)
{
	const uint32_t cb = BGV(0), ob = OBJV(0);

	sds_init_quiet();
	gfx_power();
	gfx_banks();
	gfx_clear_all();
	/* paleta: 240 colores distintos en los 16 bancos */
	for (int i = 1; i < 256; i++)
		PAL_BG(0)[i] = (uint16_t)RGB5(((i * 7) & 31), ((i * 13) & 31), ((i * 29) & 31));
	for (int i = 0; i < 256; i++)
		PAL_OBJ(0)[i] = (uint16_t)RGB5(31 - ((i * 5) & 31), ((i * 11) & 31), ((i * 19) & 31));
	PAL_BG(0)[0] = RGB5(0, 0, 0);
	/* tesela 1: color = 1 + (x + 3y) % 15 (4 bits) */
	{
		uint8_t t[32], u[64];

		for (int y = 0; y < 8; y++)
			for (int x = 0; x < 8; x += 2) {
				const int a = 1 + (x + 3 * y) % 15, b = 1 + (x + 1 + 3 * y) % 15;

				t[y * 4 + x / 2] = (uint8_t)(a | (b << 4));
			}
		gfx_copy16(cb + 32, t, 32);
		gfx_copy16(ob + 32, t, 32);
		/* tesela 2: 8 bits, color = 1 + (x * 8 + y) */
		for (int y = 0; y < 8; y++)
			for (int x = 0; x < 8; x++)
				u[y * 8 + x] = (uint8_t)(1 + x * 8 + y);
		gfx_copy16(cb + 0x8000 + 64 * 2, u, 64);
	}
#if MOS == 0
	for (int i = 0; i < 1024; i++)
		V16(cb + 0x4000)[i] = (uint16_t)(1 | (((i + (i >> 5)) & 15) << 12));
	ER16(0, E_BGCNT(0)) = (8 << 8) | 0x40;
	ER16(0, E_BGHOFS(0)) = 3;
	ER16(0, E_BGVOFS(0)) = 5;
	ER16(0, E_MOSAIC) = (5 << 4) | 4; /* BG: 5 de ancho, 6 de alto */
	ER32(0, E_DISPCNT) = DC_GRAPHICS | DC_BG(0);
#elif MOS == 1 || MOS == 2
	/* un OBJ de 32×32 con mosaico y otro sin él, a la vista */
	for (int t = 0; t < 16; t++)
		for (int i = 0; i < 8; i++)
			V32(ob + 32 * (uint32_t)(1 + t))[i] = V32(ob + 32)[i];
	OAM(0)[0] = (uint16_t)(20 | (1 << 12) | (2 << 14) * 0 | 0); /* y = 20, mosaico, forma cuadrada */
	OAM(0)[1] = (uint16_t)(33 | (2 << 14));                      /* x = 33, tamaño 32×32 */
	OAM(0)[2] = 1;
	OAM(0)[4] = (uint16_t)(81 | (2 << 14) * 0 | (1 << 12));
	OAM(0)[5] = (uint16_t)(47 | (2 << 14) | (1 << 12));
	OAM(0)[6] = 1;
#if MOS == 2
	OAM(0)[0] |= 0x100;
	OAM(0)[4] |= 0x100;
	OAM(0)[3] = 0x0100; /* grupo 0 */
	OAM(0)[7] = 0x0000;
	OAM(0)[11] = 0x0000;
	OAM(0)[15] = 0x0100;
	OAM(0)[3] = 0x00D0;
	OAM(0)[7] = 0x0040;
	OAM(0)[11] = 0xFFC0;
	OAM(0)[15] = 0x00D0;
#endif
	ER16(0, E_MOSAIC) = (5 << 12) | (4 << 8); /* OBJ: 5 de ancho, 6 de alto */
	ER32(0, E_DISPCNT) = DC_GRAPHICS | DC_OBJ | DC_OBJ1D;
#elif MOS == 11
	/* OBJ de 32×64 y 4 bits con mosaico 14 × 13: con vflip, sin vflip, y a distintas alturas respecto del bloque */
	for (int t = 1; t <= 40; t++) {
		uint8_t u[32];

		for (int y = 0; y < 8; y++)
			for (int x = 0; x < 4; x++)
				u[y * 4 + x] = (uint8_t)((((t + x * 7 + y * 3) % 5) < 2 ? 0 : 1 + ((t * 5 + x * 3 + y * 7) % 15)) |
							 ((((t + x * 7 + y * 3 + 1) % 5) < 2 ? 0 : 1 + ((t * 5 + x * 3 + 1 + y * 7) % 15)) << 4));
		gfx_copy16(ob + 32u * (uint32_t)t, u, 32);
	}
	{
		static const uint8_t ys[4] = { 157, 14, 40, 105 }; /* y % 13 = 1, 1, 1, 1 */
		static const uint8_t vf[4] = { 1, 1, 0, 0 };
		static const uint8_t xs[4] = { 159, 30, 80, 200 };

		for (int i = 0; i < 4; i++) {
			OAM(0)[i * 4] = (uint16_t)(ys[i] | (1 << 12) | (2 << 14));
			OAM(0)[i * 4 + 1] = (uint16_t)(xs[i] | (3 << 14) | (vf[i] << 13));
			OAM(0)[i * 4 + 2] = 1;
		}
	}
	ER16(0, E_MOSAIC) = (12 << 12) | (13 << 8);
	ER32(0, E_DISPCNT) = DC_GRAPHICS | DC_OBJ | DC_OBJ1D;
#elif MOS == 10
	/* OBJ de 16×16 y 8 bits: normal y afín (identidad), con mosaico vertical 6 y horizontal 5, para ver qué fila y columna
	 * de origen usa cada píxel de pantalla. Color = 1 + fila * 16 + columna. */
	{
		uint8_t u[64];

		for (int ty = 0; ty < 2; ty++)
			for (int tx = 0; tx < 2; tx++) {
				for (int y = 0; y < 8; y++)
					for (int x = 0; x < 8; x++)
						u[y * 8 + x] = (uint8_t)(1 + (ty * 8 + y) * 16 + tx * 8 + x);
				gfx_copy16(ob + 64 * (uint32_t)(1 + ty * 2 + tx), u, 64);
			}
	}
	OAM(0)[0] = (uint16_t)(21 | (1 << 12) | (1 << 13));
	OAM(0)[1] = (uint16_t)(43 | (1 << 14));
	OAM(0)[2] = 1 * 2;
	OAM(0)[4] = (uint16_t)(21 | (1 << 12) | (1 << 13) | 0x100);
	OAM(0)[5] = (uint16_t)(103 | (1 << 14));
	OAM(0)[6] = 1 * 2;
	OAM(0)[3] = 0x0100;
	OAM(0)[7] = 0;
	OAM(0)[11] = 0;
	OAM(0)[15] = 0x0100;
	OAM(0)[8] = (uint16_t)(61 | (1 << 12) | (1 << 13) | 0x300); /* afín de doble tamaño */
	OAM(0)[9] = (uint16_t)(163 | (1 << 14));
	OAM(0)[10] = 1 * 2;
	ER16(0, E_MOSAIC) = (5 << 12) | (4 << 8);
	ER32(0, E_DISPCNT) = DC_GRAPHICS | DC_OBJ | DC_OBJ1D | (1u << 20);
#elif MOS == 9
	/* OBJ de 32×32 con texeles transparentes y mosaico, solo y solapado con otro sin mosaico */
	for (int t = 0; t < 32; t++) {
		uint8_t u[64];

		for (int y = 0; y < 8; y++)
			for (int x = 0; x < 8; x++)
				u[y * 8 + x] = (((t * 7 + x * 13 + y * 5 + x * y) % 5) < 2) ? 0 : (uint8_t)(1 + ((t * 5 + x * 3 + y * 7) % 250));
		gfx_copy16(ob + 64 * (uint32_t)(1 + t), u, 64);
	}
	OAM(0)[0] = (uint16_t)(20 | (1 << 12) | (1 << 13));
	OAM(0)[1] = (uint16_t)(33 | (2 << 14));
	OAM(0)[2] = 1 * 2;
	OAM(0)[4] = (uint16_t)(90 | (1 << 12) | (1 << 13));
	OAM(0)[5] = (uint16_t)(37 | (2 << 14));
	OAM(0)[6] = (uint16_t)(1 * 2 | (1 << 10));
	OAM(0)[8] = (uint16_t)(98 | (1 << 13));
	OAM(0)[9] = (uint16_t)(45 | (1 << 14));
	OAM(0)[10] = (uint16_t)(17 * 2);
	ER16(0, E_MOSAIC) = (0 << 12) | (4 << 8);
	ER32(0, E_DISPCNT) = DC_GRAPHICS | DC_OBJ | DC_OBJ1D | (1u << 20);
#elif MOS == 8
	/* OBJ de 8×8 con mosaico horizontal 10 en posiciones distintas */
	for (int t = 0; t < 16; t++) {
		uint8_t u[64];

		for (int y = 0; y < 8; y++)
			for (int x = 0; x < 8; x++)
				u[y * 8 + x] = (uint8_t)(1 + ((t * 5 + x * 3 + y * 7) % 250));
		gfx_copy16(ob + 64 * (uint32_t)(1 + t), u, 64);
	}
	{
		static const uint8_t xs[8] = { 141, 147, 159, 150, 33, 62, 85, 98 };

		for (int i = 0; i < 8; i++) {
			OAM(0)[i * 4] = (uint16_t)(10 + i * 12 | (1 << 12) | (1 << 13));
			OAM(0)[i * 4 + 1] = (uint16_t)xs[i];
			OAM(0)[i * 4 + 2] = (uint16_t)(2 * 1);
		}
	}
	ER16(0, E_MOSAIC) = (0 << 12) | (9 << 8);
	ER32(0, E_DISPCNT) = DC_GRAPHICS | DC_OBJ | DC_OBJ1D | (1u << 20);
#elif MOS == 7
	/* OBJ con mosaico solapados con otros sin él */
	for (int t = 0; t < 16; t++) {
		uint8_t u[64];

		for (int y = 0; y < 8; y++)
			for (int x = 0; x < 8; x++)
				u[y * 8 + x] = (uint8_t)(1 + ((t * 5 + x * 3 + y * 7) % 250));
		gfx_copy16(ob + 64 * (uint32_t)(1 + t), u, 64);
	}
	/* 0: con mosaico, 32×32, prio 1, en (41,31); 1: sin mosaico, 16×16, prio 0, en (53,43); 2: con mosaico, 16×16, prio
	 * 0, en (45,60) tapa a 3 (sin mosaico, 32x32 prio 1, en (40,55)) */
	OAM(0)[0] = (uint16_t)(31 | (1 << 12) | (1 << 13));
	OAM(0)[1] = (uint16_t)(41 | (2 << 14));
	OAM(0)[2] = (uint16_t)(1 * 2 | (1 << 10));
	OAM(0)[4] = (uint16_t)(43 | (1 << 13));
	OAM(0)[5] = (uint16_t)(53 | (1 << 14));
	OAM(0)[6] = (uint16_t)(5 * 2);
	OAM(0)[8] = (uint16_t)(60 | (1 << 12) | (1 << 13));
	OAM(0)[9] = (uint16_t)(45 | (1 << 14));
	OAM(0)[10] = (uint16_t)(9 * 2);
	OAM(0)[12] = (uint16_t)(55 | (1 << 13));
	OAM(0)[13] = (uint16_t)(40 | (2 << 14));
	OAM(0)[14] = (uint16_t)(1 * 2 | (1 << 10));
	ER16(0, E_MOSAIC) = (2 << 12) | (4 << 8);
	ER32(0, E_DISPCNT) = DC_GRAPHICS | DC_OBJ | DC_OBJ1D | (1u << 20);
#elif MOS == 6
	/* el OBJ de la semilla 0: 32×32, 8 bits, hflip, mosaico 10 × 2, en (93,161) */
	for (int t = 0; t < 16; t++) {
		uint8_t u[64];

		for (int y = 0; y < 8; y++)
			for (int x = 0; x < 8; x++)
				u[y * 8 + x] = (uint8_t)(1 + ((t * 5 + x * 3 + y * 7) % 250));
		gfx_copy16(ob + 64 * (uint32_t)(1 + t), u, 64);
	}
	OAM(0)[0] = (uint16_t)(161 | (1 << 12) | (1 << 13));
	OAM(0)[1] = (uint16_t)(93 | (2 << 14) | (1 << 12));
	OAM(0)[2] = 1 * 2;
	ER16(0, E_MOSAIC) = (1 << 12) | (9 << 8);
	ER32(0, E_DISPCNT) = DC_GRAPHICS | DC_OBJ | DC_OBJ1D | (1u << 20);
#elif MOS == 5
	/* OBJ de 32×32 y 8 bits con vflip y mosaico vertical */
	for (int t = 0; t < 16; t++) {
		uint8_t u[64];

		for (int y = 0; y < 8; y++)
			for (int x = 0; x < 8; x++)
				u[y * 8 + x] = (uint8_t)(1 + ((t * 5 + x * 3 + y * 7) % 250));
		gfx_copy16(ob + 64 * (uint32_t)(1 + t), u, 64);
	}
	OAM(0)[0] = (uint16_t)(21 | (1 << 12) | (1 << 13));
	OAM(0)[1] = (uint16_t)(40 | (2 << 14) | (1 << 13));  /* vflip */
	OAM(0)[2] = 1 * 2;
	OAM(0)[4] = (uint16_t)(100 | (1 << 12) | (1 << 13));
	OAM(0)[5] = (uint16_t)(40 | (2 << 14));              /* sin vflip */
	OAM(0)[6] = 1 * 2;
	ER16(0, E_MOSAIC) = (5 << 12);
	ER32(0, E_DISPCNT) = DC_GRAPHICS | DC_OBJ | DC_OBJ1D | (1u << 20);
#elif MOS == 4
	/* OBJ de 64×64 y 8 bits con mosaico que da la vuelta por arriba */
	for (int t = 0; t < 64; t++) {
		uint8_t u[64];

		for (int y = 0; y < 8; y++)
			for (int x = 0; x < 8; x++)
				u[y * 8 + x] = (uint8_t)(1 + ((t * 5 + x * 3 + y * 7) % 250));
		gfx_copy16(ob + 64 * (uint32_t)(1 + t), u, 64);
	}
	OAM(0)[0] = (uint16_t)(207 | (1 << 12) | (1 << 13));
	OAM(0)[1] = (uint16_t)(34 | (3 << 14));
	OAM(0)[2] = 1 * 2;
	ER16(0, E_MOSAIC) = (1 << 12) | (9 << 8);
	ER32(0, E_DISPCNT) = DC_GRAPHICS | DC_OBJ | DC_OBJ1D | (1u << 20);
#else
	for (int i = 0; i < 1024; i += 2)
		V16(cb + 0x4000)[i / 2] = 0x0202;
	ER16(0, E_BGCNT(2)) = (8 << 8) | 0x40 | (2 << 2) | (1 << 14); /* afín 256×256, mosaico */
	ER16(0, E_BG2PA) = 0x0100;
	ER16(0, E_BG2PB) = 0x0040;
	ER16(0, E_BG2PC) = 0x0030;
	ER16(0, E_BG2PD) = 0x0100;
	ER32(0, E_BG2X) = 5 << 8;
	ER32(0, E_BG2Y) = 9 << 8;
	ER16(0, E_MOSAIC) = (5 << 4) | 4;
	ER32(0, E_DISPCNT) = DC_GRAPHICS | DC_BG(2) | 2;
#endif
	gfx_finish();
}
