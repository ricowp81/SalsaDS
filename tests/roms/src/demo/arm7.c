/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * SalsaDS — ROM demo, lado ARM7: cada VBlank lee EXTKEYIN y, si el lápiz toca, la pantalla táctil por SPI, la
 * convierte a píxeles con la calibración del firmware y se lo deja al ARM9 (shared.h).
 */
#include <nds.h>

#include "../common/hw.h"
#include "shared.h"

static volatile uint32_t vblanks;
static void h_vblank(void) { vblanks++; }

static uint8_t spi_xfer(uint32_t dev, uint32_t baud, int hold, uint8_t out)
{
	REG16(R_SPICNT) = (uint16_t)(0x8000 | (dev << 8) | baud | (hold ? 0x0800 : 0));
	REG16(R_SPIDATA) = out;
	while (REG16(R_SPICNT) & 0x80)
		;
	return (uint8_t)REG16(R_SPIDATA);
}

static uint32_t tsc_read(uint8_t cmd)
{
	uint32_t hi, lo;

	spi_xfer(2, 1, 1, cmd);
	hi = spi_xfer(2, 1, 1, 0);
	lo = spi_xfer(2, 1, 0, 0);
	return ((hi & 0x7F) << 5) | (lo >> 3);
}

static void fw_read(uint32_t addr, uint8_t *buf, uint32_t n)
{
	spi_xfer(1, 0, 1, 0x03);
	spi_xfer(1, 0, 1, (uint8_t)(addr >> 16));
	spi_xfer(1, 0, 1, (uint8_t)(addr >> 8));
	spi_xfer(1, 0, 1, (uint8_t)addr);
	for (uint32_t i = 0; i < n; i++)
		buf[i] = spi_xfer(1, 0, i + 1 < n, 0);
}

int main(void)
{
	struct demo_shared *sh = DEMO_SHARED;
	uint8_t u[0x70];
	int32_t ax1, ay1, sx1, sy1, ax2, ay2, sx2, sy2;
	uint8_t head[0x22];

	__asm__ volatile("msr cpsr_c, #0xDF" : : : "cc");
	irqInit();
	irqSet(IRQ_VBLANK, h_vblank);
	irqEnable(IRQ_VBLANK);
	MEM32(R_IME) = 1;
	__asm__ volatile("msr cpsr_c, #0x1F" : : : "cc");
	fw_read(0, head, sizeof head);
	fw_read((uint32_t)(head[0x20] | (head[0x21] << 8)) * 8u, u, sizeof u);
	ax1 = u[0x58] | (u[0x59] << 8);
	ay1 = u[0x5A] | (u[0x5B] << 8);
	sx1 = u[0x5C];
	sy1 = u[0x5D];
	ax2 = u[0x5E] | (u[0x5F] << 8);
	ay2 = u[0x60] | (u[0x61] << 8);
	sx2 = u[0x62];
	sy2 = u[0x63];
	sh->magic = DEMO_MAGIC;
	for (;;) {
		const uint32_t ext = MEM16(R_EXTKEYIN);

		swiWaitForVBlank();
		sh->ext = ext;
		sh->pen = !(ext & 0x40);
		if (sh->pen) {
			const uint32_t x = tsc_read(0xD0), y = tsc_read(0x90);
			int32_t px = ((int32_t)x - ax1) * (sx2 - sx1) / (ax2 - ax1) + sx1 - 1;
			int32_t py = ((int32_t)y - ay1) * (sy2 - sy1) / (ay2 - ay1) + sy1 - 1;

			if (px < 0)
				px = 0;
			if (px > 255)
				px = 255;
			if (py < 0)
				py = 0;
			if (py > 191)
				py = 191;
			sh->adc_x = x;
			sh->adc_y = y;
			sh->px = px;
			sh->py = py;
		}
		sh->frame = vblanks;
	}
}
