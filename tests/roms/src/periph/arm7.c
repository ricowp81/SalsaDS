/*
 * SalsaDS — ROM periph, lado ARM7: el bus SPI (firmware, pantalla táctil, gestión de energía), el RTC por el
 * puerto 0x04000138, EXTKEYIN y el cartucho cuando el ARM9 le cede la ranura. El firmware de las dos máquinas
 * es el que genera cada emulador: se comprueba su estructura (versión, CRC16, datos de usuario), no su contenido.
 */
#include <nds.h>

#include "periph.h"

static uint8_t spi_xfer(uint32_t dev, uint32_t baud, int hold, uint8_t out)
{
	REG16(R_SPICNT) = (uint16_t)(0x8000 | (dev << 8) | baud | (hold ? 0x0800 : 0));
	REG16(R_SPIDATA) = out;
	while (REG16(R_SPICNT) & 0x80)
		;
	return (uint8_t)REG16(R_SPIDATA);
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

static uint32_t tsc_read(uint8_t cmd)
{
	uint32_t hi, lo;

	spi_xfer(2, 1, 1, cmd);
	hi = spi_xfer(2, 1, 1, 0);
	lo = spi_xfer(2, 1, 0, 0);
	return ((hi & 0x7F) << 5) | (lo >> 3);
}

static uint8_t pm_read(uint32_t reg)
{
	spi_xfer(0, 2, 1, (uint8_t)(0x80 | reg));
	return spi_xfer(0, 2, 0, 0);
}

static void pm_write(uint32_t reg, uint8_t v)
{
	spi_xfer(0, 2, 1, (uint8_t)reg);
	spi_xfer(0, 2, 0, v);
}

static uint8_t fw_user[2][0x100];

static void group_fw(void)
{
	uint8_t head[0x30], jedec[3];
	uint32_t i, ok0, ok1;

	sds_group("FIRMWARE");
	fw_read(0, head, sizeof head);
	SDS_DIVERGE(head[0x20] | (head[0x21] << 8), 0x7FC0); /* dirección de los datos de usuario / 8 (256 KB) */
	{
		const uint32_t user = (uint32_t)(head[0x20] | (head[0x21] << 8)) * 8u;

		SDS_CHECK(user >= 0x1000 && user <= 0x7FE00 && (user & 0xFF) == 0, 1);
		fw_read(user, fw_user[0], 0x100);
		fw_read(user + 0x100, fw_user[1], 0x100);
	}
	ok0 = fw_user[0][0] == 5 && fw_user[0][1] == 0;
	ok1 = fw_user[1][0] == 5 && fw_user[1][1] == 0;
	SDS_CHECK(ok0 && ok1, 1); /* versión 5 en las dos copias */
	SDS_CHECK(crc16(0xFFFF, fw_user[0], 0x70), fw_user[0][0x72] | (fw_user[0][0x73] << 8));
	SDS_CHECK(crc16(0xFFFF, fw_user[1], 0x70), fw_user[1][0x72] | (fw_user[1][0x73] << 8));
	/* la copia en RAM (0x027FFC80) es una de las dos */
	{
		uint32_t same0 = 1, same1 = 1;

		for (i = 0; i < 0x70; i++) {
			same0 &= MEM8(0x027FFC80 + i) == fw_user[0][i];
			same1 &= MEM8(0x027FFC80 + i) == fw_user[1][i];
		}
		SDS_CHECK(same0 || same1, 1);
	}
	spi_xfer(1, 0, 1, 0x9F);
	for (i = 0; i < 3; i++)
		jedec[i] = spi_xfer(1, 0, i < 2, 0);
	SDS_CHECK(jedec[0] != 0 && jedec[0] != 0xFF, 1);
	/* estado: WEL apagado; WREN lo enciende; WRDI lo apaga */
	spi_xfer(1, 0, 1, 0x05);
	SDS_CHECK(spi_xfer(1, 0, 0, 0) & 2, 0);
	spi_xfer(1, 0, 0, 0x06);
	spi_xfer(1, 0, 1, 0x05);
	SDS_CHECK(spi_xfer(1, 0, 0, 0) & 2, 2);
	spi_xfer(1, 0, 0, 0x04);
	spi_xfer(1, 0, 1, 0x05);
	SDS_CHECK(spi_xfer(1, 0, 0, 0) & 2, 0);
}

static void group_touch(void)
{
	const int pen_down = !(MEM16(R_EXTKEYIN) & 0x40);
	const uint8_t *u = fw_user[0];
	uint32_t x, y, z1;

	sds_group("TACTIL");
	x = tsc_read(0xD0); /* X, 12 bits, diferencial */
	y = tsc_read(0x90); /* Y */
	z1 = tsc_read(0xB0);
	if (!pen_down) {
		SDS_CHECK(x, 0);      /* sin tocar: X = 0, Y = 0xFFF (GBATEK) */
		SDS_CHECK(y, 0xFFF);
		SDS_DIVERGE_M(z1, 0, 0xFFF); /* Z1 sin tocar: GBATEK no dice; melonDS da 0xFFF */
	} else {
		/* calibración del firmware: pixel = (adc - adc1) * (scr2 - scr1) / (adc2 - adc1) + scr1 - 1 */
		const int32_t ax1 = u[0x58] | (u[0x59] << 8), ay1 = u[0x5A] | (u[0x5B] << 8), sx1 = u[0x5C], sy1 = u[0x5D];
		const int32_t ax2 = u[0x5E] | (u[0x5F] << 8), ay2 = u[0x60] | (u[0x61] << 8), sx2 = u[0x62], sy2 = u[0x63];
		const int32_t px = ((int32_t)x - ax1) * (sx2 - sx1) / (ax2 - ax1) + sx1 - 1;
		const int32_t py = ((int32_t)y - ay1) * (sy2 - sy1) / (ay2 - ay1) + sy1 - 1;

		MEM32(0x02390FC0) = (uint32_t)px; /* para quien corre con --touch (salsads-cpurun --peek) */
		MEM32(0x02390FC4) = (uint32_t)py;
		MEM32(0x02390FC8) = x;
		MEM32(0x02390FCC) = y;
	}
	/* el canal de batería (2) da 0 y la conversión de 8 bits es la de 12 bits sin los 4 bajos (informativo) */
	SDS_DIVERGE(tsc_read(0xA0), 0);
}

static void group_pm(void)
{
	sds_group("ENERGIA");
	SDS_CHECK(pm_read(0) & 0x40, 0); /* apagado del sistema: no */
	SDS_CHECK(pm_read(1) & 1, 0);    /* batería buena */
	pm_write(3, 2);                  /* ganancia del micrófono: viaja de ida y vuelta */
	SDS_CHECK(pm_read(3) & 3, 2);
	pm_write(3, 1);
	SDS_CHECK(pm_read(3) & 3, 1);
	SDS_DIVERGE(pm_read(4) & 0xF0, 0x40); /* DS Lite: los bits altos del registro 4 valen 4 */
}

#define RTC_DIR_W 0x70
#define RTC_DIR_R 0x60

static void rtc_delay(void)
{
	for (volatile int i = 0; i < 40; i++)
		;
}

static void rtc_begin(void)
{
	REG16(R_RTC) = RTC_DIR_W | 2;
	rtc_delay();
	REG16(R_RTC) = RTC_DIR_W | 2 | 4;
	rtc_delay();
}

static void rtc_end(void)
{
	REG16(R_RTC) = RTC_DIR_W | 2;
	rtc_delay();
}

static void rtc_send(uint8_t b)
{
	for (int i = 0; i < 8; i++) {
		const uint32_t bit = (b >> i) & 1;

		REG16(R_RTC) = (uint16_t)(RTC_DIR_W | 4 | bit);
		rtc_delay();
		REG16(R_RTC) = (uint16_t)(RTC_DIR_W | 4 | 2 | bit);
		rtc_delay();
	}
}

static uint8_t rtc_recv(void)
{
	uint8_t v = 0;

	for (int i = 0; i < 8; i++) {
		REG16(R_RTC) = RTC_DIR_R | 4;
		rtc_delay();
		REG16(R_RTC) = RTC_DIR_R | 4 | 2;
		rtc_delay();
		v |= (uint8_t)((REG16(R_RTC) & 1) << i);
	}
	return v;
}

static void rtc_read(uint8_t cmd, uint8_t *buf, uint32_t n)
{
	rtc_begin();
	rtc_send(cmd);
	for (uint32_t i = 0; i < n; i++)
		buf[i] = rtc_recv();
	rtc_end();
}

static int bcd_ok(uint8_t v, uint32_t lo, uint32_t hi)
{
	const uint32_t d = (uint32_t)(v >> 4) * 10 + (v & 15);

	return (v & 15) <= 9 && (v >> 4) <= 9 && d >= lo && d <= hi;
}

static void group_rtc(void)
{
	uint8_t dt[7], t[3], st1, st2;

	sds_group("RTC");
	rtc_read(0x86, &st1, 1); /* estado 1 */
	rtc_read(0xC6, &st2, 1); /* estado 2 */
	(void)st2;
	SDS_DIVERGE(st1 & 2, 2); /* formato de 24 horas (informativo) */
	rtc_read(0xA6, dt, 7);   /* fecha y hora: año, mes, día, día de la semana, hora, minuto, segundo */
	SDS_CHECK(bcd_ok(dt[0], 0, 99), 1);
	SDS_CHECK(bcd_ok(dt[1], 1, 12), 1);
	SDS_CHECK(bcd_ok(dt[2], 1, 31), 1);
	SDS_CHECK(dt[3] <= 6, 1);
	SDS_CHECK(bcd_ok(dt[4] & 0x3F, 0, 23), 1);
	SDS_CHECK(bcd_ok(dt[5] & 0x7F, 0, 59), 1);
	SDS_CHECK(bcd_ok(dt[6] & 0x7F, 0, 59), 1);
	rtc_read(0xE6, t, 3);    /* hora */
	SDS_CHECK(bcd_ok(t[0] & 0x3F, 0, 23), 1);
	SDS_CHECK(bcd_ok(t[1] & 0x7F, 0, 59), 1);
	SDS_CHECK(bcd_ok(t[2] & 0x7F, 0, 59), 1);
	SDS_CHECK(t[0] == dt[4] || ((dt[5] == 0x59) && 1), 1); /* misma hora (salvo cambio de minuto entre las dos lecturas) */
	MBOX->res[5] = (uint32_t)dt[0] << 16 | (uint32_t)dt[1] << 8 | dt[2];
}

static void group_keys(void)
{
	sds_group("EXTKEYIN");
	SDS_CHECK(MEM16(R_EXTKEYIN) & 0x03, 0x03); /* X e Y sueltos */
	SDS_CHECK(MEM16(R_EXTKEYIN) & 0x80, 0);    /* tapa abierta */
}

static void service(void)
{
	static uint32_t buf[0x200 / 4], idb[2];

	for (;;) {
		const uint32_t cmd = MBOX->cmd;

		if (!cmd)
			continue;
		MBOX->cmd = 0;
		switch (cmd) {
		case P_CART_OWN: {
			uint32_t i, n;

			n = rom_read(0xB7, MBOX->arg[0], 1, 0x200, buf);
			for (i = 0; i < 0x200 / 4; i++)
				MEM32(0x02390A00 + 4 * i) = buf[i];
			rom_read(0xB8, 0, 7, 4, idb);
			MBOX->res[0] = n;
			MBOX->res[2] = idb[0];
			break;
		}
		case P_CART_RES: {
			uint32_t same0 = 1, same1 = 1, i;

			for (i = 0; i < 0x70; i++) {
				same0 &= MEM8(0x027FFC80 + i) == fw_user[0][i];
				same1 &= MEM8(0x027FFC80 + i) == fw_user[1][i];
			}
			MBOX->res[0] = (same0 || same1) && crc16(0xFFFF, (const uint8_t *)(uintptr_t)0x027FFC80, 0x70) == MBOX->arg[1];
			break;
		}
		case CMD_QUIT:
			MBOX->done = 1;
			return;
		default:
			break;
		}
		MBOX->done = 1;
	}
}

int main(void)
{
	__asm__ volatile("msr cpsr_c, #0xDF" : : : "cc");
	sds_init("PERIPH ARM7");
	group_fw();
	group_touch();
	group_pm();
	group_rtc();
	group_keys();
	service();
	sds_finish();
}
