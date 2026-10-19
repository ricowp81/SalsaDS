/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * SalsaDS — ROM periph: órdenes del ARM9 al ARM7 y utilidades comunes (CRC16, lectura del cartucho).
 */
#ifndef PERIPH_H
#define PERIPH_H

#include "../common/hw.h"

#define P_CART_OWN 1   /* el ARM7 lee con el cartucho a su nombre: cabecera, id y un bloque */
#define P_CART_RES 2

static inline uint32_t crc16(uint32_t crc, const uint8_t *p, uint32_t n)
{
	while (n--) {
		crc ^= *p++;
		for (int i = 0; i < 8; i++)
			crc = (crc & 1) ? (crc >> 1) ^ 0xA001u : crc >> 1;
	}
	return crc & 0xFFFFu;
}

/* ROMCTRL: bloque de 0x200 (1 << 24), reloj lento (bit 27), inicio (bit 31) */
#define ROMCTRL_START(blk) (0x80000000u | ((uint32_t)(blk) << 24) | 0x00001FFFu)
#define ROMCTRL_BUSY 0x80000000u
#define ROMCTRL_DRQ 0x00800000u

/* Pone el comando de 8 bytes (el primero en la dirección más baja) */
static inline void rom_cmd(uint8_t c0, uint32_t a)
{
	REG8(R_ROMCMD + 0) = c0;
	REG8(R_ROMCMD + 1) = (uint8_t)(a >> 24);
	REG8(R_ROMCMD + 2) = (uint8_t)(a >> 16);
	REG8(R_ROMCMD + 3) = (uint8_t)(a >> 8);
	REG8(R_ROMCMD + 4) = (uint8_t)a;
	REG8(R_ROMCMD + 5) = 0;
	REG8(R_ROMCMD + 6) = 0;
	REG8(R_ROMCMD + 7) = 0;
}

/* Lee nbytes (múltiplo de 4) con el comando dado a un búfer por sondeo; devuelve las palabras leídas (con tope). */
static inline uint32_t rom_read(uint8_t c0, uint32_t addr, uint32_t blk, uint32_t nbytes, uint32_t *dst)
{
	uint32_t n = 0, spin = 0;

	REG16(R_AUXSPICNT) = 0x8000;
	rom_cmd(c0, addr);
	REG32(R_ROMCTRL) = ROMCTRL_START(blk);
	while ((REG32(R_ROMCTRL) & ROMCTRL_BUSY) && spin < 2000000u) {
		if (REG32(R_ROMCTRL) & ROMCTRL_DRQ) {
			const uint32_t v = REG32(R_ROMDATA);

			if (n < nbytes / 4)
				dst[n] = v;
			n++;
		}
		spin++;
	}
	return n;
}

#endif
