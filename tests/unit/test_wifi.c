/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Diagnóstico de arranque del dispositivo local sin conexión inalámbrica.
 * Estímulos sintéticos por el bus y por SPI; no usa datos de una consola. */
#include <string.h>
#include "check.h"
#include "core/nds_priv.h"

static struct nds M;

static uint32_t rd(unsigned off)
{
	return nds_bus_read(&M, NDS_CPU7, 0x04808000u + off, 2);
}

static void wr(unsigned off, uint16_t v)
{
	nds_bus_write(&M, NDS_CPU7, 0x04808000u + off, 2, v);
}

static uint8_t spi(uint8_t v, int hold)
{
	nds_spi_write(&M, 0x1C0, 2, hold ? 0x8900 : 0x8100);
	nds_spi_write(&M, 0x1C2, 1, v);
	nds_spi_event(&M);
	return (uint8_t)nds_spi_read(&M, 0x1C2, 1);
}

void test_wifi(void)
{
	static const unsigned masks[][2] = {
		{6,0x7F}, {0x18,0xFFFF}, {0x1A,0xFFFF}, {0x1C,0xFFFF},
		{0x20,0xFFFF}, {0x22,0xFFFF}, {0x24,0xFFFF}, {0x2A,0x7FF},
		{0x50,0xFFFF}, {0x52,0xFFFF}, {0x56,0xFFF}, {0x58,0x1FFE},
		{0x5A,0xFFF}, {0x5C,0xFFF}, {0x62,0x1FFE}, {0x64,0xFFF},
		{0x68,0x1FFE}, {0x6C,0xFFF}, {0x74,0x1FFE}, {0x122,0xFFFF},
		{0x124,0xFFFF}, {0x128,0xFFFF}, {0x130,0xFFF}, {0x132,0x8FFF},
		{0x134,0xFFFF}, {0x140,0xFFFF}, {0x142,0xFFFF}
	};
	static const uint16_t patterns[] = {0xFFFF,0x5A5A,0xA5A5,0};
	uint8_t config[0x13A];

	memset(&M, 0, sizeof M);
	nds_init(&M);
	CHECK_EQ(nds_crc16(0, (const uint8_t *)"123456789", 9), 0xBB3D);
	/* Lectura SPI real de la cabecera y del bloque que valida el juego. */
	spi(3, 1); spi(0, 1); spi(0, 1); spi(0x2A, 1);
	for (unsigned i = 0; i < sizeof config; i++)
		config[i] = spi(0, i + 1 < sizeof config);
	CHECK_EQ(rd16le(config + 2), 0x138);
	CHECK_EQ(rd16le(config), nds_crc16(0, config + 2, sizeof config - 2));
	CHECK_EQ(rd16le(config + 0x12), 0x3FFE);
	CHECK_EQ(config[0x16], 2);
	CHECK_EQ(config[0x17], 24);
	CHECK_EQ(config[0x18], 12);
	CHECK_EQ(rd(0), 0x1440);
	wr(0, 0xFFFF);
	CHECK_EQ(rd(0), 0x1440); /* solo lectura */
	for (unsigned p = 0; p < sizeof patterns / sizeof patterns[0]; p++) {
		for (unsigned i = 0; i < sizeof masks / sizeof masks[0]; i++) {
			wr(masks[i][0], patterns[p]);
			CHECK_EQ(rd(masks[i][0]), patterns[p] & masks[i][1]);
		}
		for (unsigned i = 0; i < 0x2000; i += 2)
			nds_bus_write(&M, NDS_CPU7, 0x04804000u + i, 2, patterns[p]);
		for (unsigned i = 0; i < 0x2000; i += 2)
			CHECK_EQ(nds_bus_read(&M, NDS_CPU7, 0x04804000u + i, 2), patterns[p]);
	}
	nds_bus_write(&M, NDS_CPU7, 0x04804000, 4, 0x12345678);
	CHECK_EQ(nds_bus_read(&M, NDS_CPU7, 0x0480C000, 4), 0x12345678);
	nds_bus_write(&M, NDS_CPU7, 0x04804000, 1, 0);
	CHECK_EQ(nds_bus_read(&M, NDS_CPU7, 0x04804001, 1), 0x56);
	CHECK_EQ(nds_bus_read(&M, NDS_CPU9, 0x04804000, 4), 0);
	nds_bus_write(&M, NDS_CPU9, 0x04804000, 4, 0);
	CHECK_EQ(nds_bus_read(&M, NDS_CPU7, 0x04804000, 4), 0x12345678);
	nds_bus_write(&M, NDS_CPU7, 0x04800018, 2, 0x2468);
	CHECK_EQ(rd(0x18), 0x2468); /* espejo de registros */
	nds_bus_write(&M, NDS_CPU7, 0x04808018, 1, 0);
	CHECK_EQ(rd(0x18), 0x2468);
	wr(0x3E0, 0xFFFF);
	CHECK_EQ(rd(0x3E0), 0xFFFF); /* puerto no usado de DS, no memoria genérica */
	wr(0x15A, 0xA5); wr(0x158, 0x5001); wr(0x158, 0x6001);
	CHECK_EQ(rd(0x15C), 0xA5);
	wr(0x158, 0x5000); wr(0x158, 0x6000);
	CHECK_EQ(rd(0x15C), 0x6D); /* BB: identificación inmutable */
	wr(0x158, 0x5069); wr(0x158, 0x6069);
	CHECK_EQ(rd(0x15C), 0);
	CHECK_EQ(rd(0x15E), 0);
	CHECK_EQ(rd(0x180), 0);
	CHECK(rd(0x44) != rd(0x44));
	CHECK_EQ(rd(0x10), 0); /* sin paquetes ni interrupciones inventadas */
	CHECK_EQ(M.io[NDS_CPU7].ifl & NDS_IRQ_WIFI, 0);
	wr(0x40, 0x8001);
	CHECK_EQ(rd(0x3C) & 0x300, 0x200);
	wr(0x40, 0x8000);
	CHECK_EQ(rd(0x3C) & 0x300, 0x300);
	wr(0x2D0, 0);
	CHECK_EQ(rd(0x3C) & 0x300, 0);
	wr(0x40, 0x8001);
	wr(0x36, 0); wr(4, 1); wr(0x3C, 2);
	CHECK_EQ(rd(0x3C) & 0x300, 0);
	M.wifi_off = 1;
	nds_init(&M);
	CHECK_EQ(M.wifi_off, 1);
	CHECK_EQ(rd16le(M.fw + 0x2C), 0);
	CHECK_EQ(rd(0), 0);
	wr(0x18, 0xFFFF);
	CHECK_EQ(rd(0x18), 0);
	nds_bus_write(&M, NDS_CPU7, 0x04804000, 4, 0xFFFFFFFF);
	CHECK_EQ(nds_bus_read(&M, NDS_CPU7, 0x04804000, 4), 0);
	M.wifi_off = 0;
	nds_init(&M);
	CHECK_EQ(rd(0x18), 0);
	CHECK_EQ(nds_bus_read(&M, NDS_CPU7, 0x04804000, 4), 0);
}
