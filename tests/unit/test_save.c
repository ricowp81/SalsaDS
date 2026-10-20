/* SPDX-License-Identifier: GPL-2.0-or-later */
/* SalsaDS — pruebas del chip de guardado (src/core/save.c): tabla por código de juego, deducción del chip por conversaciones
 * SPI sintéticas (EEPROM de 0,5/8/64/128 KB, FRAM, Flash), guardado.cfg y el tamaño del .sav. Sin ROMs: solo un código de juego. */
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "check.h"
#include "core/nds_priv.h"

static char logbuf[4096];

void nds_logf(struct nds *n, const char *fmt, ...)
{
	char line[200];
	va_list ap;
	size_t l = strlen(logbuf);

	(void)n;
	va_start(ap, fmt);
	vsnprintf(line, sizeof line, fmt, ap);
	va_end(ap);
	if (l + strlen(line) + 2 < sizeof logbuf) {
		strcat(logbuf, line);
		strcat(logbuf, "\n");
	}
}

static struct nds M; /* grande (1 MB de guardado): estática */
static uint8_t rom[0x200];

static void game(const char *code)
{
	memset(&M, 0, sizeof M);
	memset(rom, 0, sizeof rom);
	memcpy(rom + 0x0C, code, 4);
	M.cart.rom = rom;
	logbuf[0] = 0;
	nds_save_default(&M);
}

/* Una conversación SPI: bytes con el chip retenido y deselección al final (como AUXSPICNT.6 = 0 en el último). */
static void conv(const uint8_t *b, size_t len, uint8_t *resp)
{
	for (size_t i = 0; i < len; i++) {
		const uint8_t r = nds_save_xfer(&M, b[i]);

		if (resp)
			resp[i] = r;
	}
	nds_save_deselect(&M);
}

static void wren(void)
{
	const uint8_t c = 0x06;

	conv(&c, 1, NULL);
}

/* Escritura 02h (o 0Ah) con `alen` bytes de dirección y `dlen` de datos con el valor inicial v (v, v+1, ...). */
static void write_conv(uint8_t cmd, int alen, uint32_t addr, int dlen, uint8_t v)
{
	static uint8_t b[1100];
	int k = 0;

	b[k++] = cmd;
	for (int i = alen - 1; i >= 0; i--)
		b[k++] = (uint8_t)(addr >> (8 * i));
	for (int i = 0; i < dlen; i++)
		b[k++] = (uint8_t)(v + i);
	wren();
	conv(b, (size_t)k, NULL);
}

static uint8_t read1(int alen, uint32_t addr)
{
	uint8_t b[6], r[6];
	int k = 0;

	b[k++] = 0x03;
	for (int i = alen - 1; i >= 0; i--)
		b[k++] = (uint8_t)(addr >> (8 * i));
	b[k++] = 0;
	conv(b, (size_t)k, r);
	return r[k - 1];
}

static int logged(const char *s)
{
	return strstr(logbuf, s) != NULL;
}

void test_save(void)
{
	/* ---- tabla ---- */
	game("IRDS");
	CHECK_EQ(M.save.type, NDS_SAVE_FLASH);
	CHECK_EQ(M.save.size, 524288);
	CHECK_EQ(M.save.src, NDS_SAVE_SRC_TABLA);
	CHECK_EQ(M.save.autod, 0);
	CHECK(logged("guardado: chip flash512k de 524288 bytes (tabla)"));
	game("IPKJ");
	CHECK_EQ(M.save.size, 524288);
	CHECK_EQ(M.save.type, NDS_SAVE_FLASH);
	{
		static const char *pk[] = { "ADAE", "APAP", "CPUE", "IPGE", "IRBJ", "IRAE", "IREE", "IRDP" };

		for (size_t i = 0; i < sizeof pk / sizeof pk[0]; i++) {
			game(pk[i]);
			CHECK_EQ(M.save.type, NDS_SAVE_FLASH);
			CHECK_EQ(M.save.size, 524288);
		}
	}
	game("ASME");
	CHECK_EQ(M.save.type, NDS_SAVE_EEPROM16);
	CHECK_EQ(M.save.size, 8192);
	game("ASMK");
	CHECK_EQ(M.save.type, NDS_SAVE_EEPROM512); /* la coreana va antes que ASM */
	game("A2DE");
	CHECK_EQ(M.save.size, 8192);
	game("AMCE");
	CHECK_EQ(M.save.type, NDS_SAVE_FLASH);
	CHECK_EQ(M.save.size, 262144);
	game("AL3E");
	CHECK_EQ(M.save.type, NDS_SAVE_EEPROM512);
	game("ZZZE");
	CHECK_EQ(M.save.type, NDS_SAVE_EEPROM16);
	CHECK_EQ(M.save.size, 65536);
	CHECK_EQ(M.save.autod, 1);
	CHECK_EQ(M.save.src, NDS_SAVE_SRC_DEFECTO);
	CHECK(logged("(defecto"));

	/* ---- Flash por RDID: lo primero que hace un juego de Flash ---- */
	game("ZZZE");
	{
		const uint8_t id[4] = { 0x9F, 0, 0, 0 };
		uint8_t r[4];

		conv(id, 4, r);
		CHECK_EQ(r[1], 0x20);
		CHECK_EQ(r[2], 0x40);
		CHECK_EQ(r[3], 0x13);
		CHECK_EQ(M.save.type, NDS_SAVE_FLASH);
		CHECK_EQ(M.save.size, 524288);
		CHECK_EQ(M.save.src, NDS_SAVE_SRC_DETECTADO);
		CHECK_EQ(M.save.autod, 0);
		CHECK(logged("(detectado)"));
		/* programar una página de Flash y leerla */
		write_conv(0x02, 3, 0x12300, 8, 0x40);
		CHECK_EQ(read1(3, 0x12303), 0x43);
		CHECK_EQ(M.save.dirty, 1);
	}
	/* borrado de sector sin RDID antes: también es Flash */
	game("ZZZE");
	wren();
	{
		const uint8_t e[4] = { 0xD8, 0, 0, 0 };

		conv(e, 4, NULL);
	}
	CHECK_EQ(M.save.type, NDS_SAVE_FLASH);
	CHECK_EQ(M.save.src, NDS_SAVE_SRC_DETECTADO);

	/* ---- EEPROM de 0,5 KB: lectura con un byte de dirección, o la orden con A8 ---- */
	game("ZZZE");
	{
		const uint8_t rd[3] = { 0x03, 0x10, 0 };

		conv(rd, 3, NULL);
	}
	CHECK_EQ(M.save.type, NDS_SAVE_EEPROM512);
	CHECK_EQ(M.save.size, 512);
	CHECK_EQ(M.save.src, NDS_SAVE_SRC_DETECTADO);
	game("ZZZE");
	write_conv(0x0A, 1, 0x20, 4, 0x70); /* escritura con A8 = 1 */
	CHECK_EQ(M.save.type, NDS_SAVE_EEPROM512);
	CHECK_EQ(M.save.autod, 0);
	CHECK_EQ(M.savemem[0x120], 0x70);
	CHECK_EQ(M.savemem[0x123], 0x73);

	/* ---- EEPROM de 64 KB: página de 128 con 2 de dirección (confirma el provisional) ---- */
	game("ZZZE");
	write_conv(0x02, 2, 0x1280, 128, 0x11);
	CHECK_EQ(M.save.type, NDS_SAVE_EEPROM16);
	CHECK_EQ(M.save.size, 65536);
	CHECK_EQ(M.save.src, NDS_SAVE_SRC_DETECTADO);
	CHECK_EQ(M.save.autod, 0);
	CHECK_EQ(read1(2, 0x1281), 0x12);
	CHECK_EQ(M.savemem[0x1280 + 127], 0x11 + 127);

	/* ---- EEPROM de 128 KB: página de 256 con 3 de dirección; lo escrito se repite sobre el chip nuevo ---- */
	game("ZZZE");
	write_conv(0x02, 3, 0x1FF00, 256, 0x05);
	CHECK_EQ(M.save.type, NDS_SAVE_EEPROM24);
	CHECK_EQ(M.save.size, 131072);
	CHECK_EQ(M.save.src, NDS_SAVE_SRC_DETECTADO);
	CHECK_EQ(M.savemem[0x1FF00], 0x05);
	CHECK_EQ(M.savemem[0x1FFFF], (uint8_t)(0x05 + 255));
	CHECK_EQ(read1(3, 0x1FF10), 0x15);
	CHECK_EQ(M.save.dirty, 1);

	/* ---- FRAM de 32 KB: escritura larga sin límite de página, decidida a media conversación ---- */
	game("ZZZE");
	write_conv(0x02, 2, 0x0100, 400, 0x30);
	CHECK_EQ(M.save.type, NDS_SAVE_FRAM);
	CHECK_EQ(M.save.size, 32768);
	CHECK_EQ(M.save.src, NDS_SAVE_SRC_DETECTADO);
	CHECK_EQ(M.savemem[0x0100], 0x30);
	CHECK_EQ(M.savemem[0x0100 + 399], (uint8_t)(0x30 + 399));
	CHECK_EQ(read1(2, 0x0100 + 300), (uint8_t)(0x30 + 300));

	/* ---- lo ambiguo no se decide: 8 KB y 64 KB se parecen, sigue el provisional ---- */
	game("ZZZE");
	write_conv(0x02, 2, 0x0040, 32, 0x80);
	CHECK_EQ(M.save.autod, 1);
	CHECK_EQ(M.save.type, NDS_SAVE_EEPROM16);
	CHECK_EQ(M.save.size, 65536);
	CHECK_EQ(M.savemem[0x40], 0x80);
	CHECK_EQ(read1(2, 0x41), 0x81);
	CHECK_EQ(M.save.autod, 1);

	/* ---- un .sav manda sobre todo (tamaño del archivo) ---- */
	{
		static uint8_t sav[NDS_SAVE_MAX];

		memset(sav, 0xAA, sizeof sav);
		game("ASME");
		CHECK_EQ(nds_save_attach(&M, sav, 8192), 0);
		CHECK_EQ(M.save.src, NDS_SAVE_SRC_SAV);
		CHECK_EQ(M.save.size, 8192);
		CHECK(logged("(.sav)"));
		game("AMCE");
		CHECK_EQ(nds_save_attach(&M, sav, 262144), 0);
		CHECK_EQ(M.save.type, NDS_SAVE_FLASH);
		game("ZZZE");
		CHECK_EQ(nds_save_attach(&M, sav, 1048576), 0); /* Flash de 8 Mbit = NDS_SAVE_MAX */
		CHECK_EQ(M.save.size, 1048576);
		CHECK_EQ(M.save.autod, 0);
		{
			const uint8_t id[4] = { 0x9F, 0, 0, 0 };
			uint8_t r[4];

			conv(id, 4, r);
			CHECK_EQ(r[3], 0x14);
		}
		game("ZZZE");
		CHECK_EQ(nds_save_attach(&M, sav, 1000), -1); /* 1000 bytes no es un chip */
		CHECK_EQ(M.save.autod, 1); /* el rechazo no cambia nada */
		/* .sav de 64 KB creado por el chip por omisión de versiones viejas para un juego de Flash: se migra */
		game("IRDE");
		CHECK_EQ(nds_save_attach(&M, sav, 65536), 0);
		CHECK_EQ(M.save.type, NDS_SAVE_FLASH);
		CHECK_EQ(M.save.size, 524288);
		CHECK_EQ(M.savemem[100], 0xAA);
		CHECK_EQ(M.savemem[70000], 0xFF);
		CHECK_EQ(M.save.dirty, 1);
		/* pero uno de otro tamaño válido sí manda */
		game("IRDE");
		CHECK_EQ(nds_save_attach(&M, sav, 262144), 0);
		CHECK_EQ(M.save.size, 262144);
		CHECK_EQ(M.save.src, NDS_SAVE_SRC_SAV);
	}

	/* ---- guardado.cfg ---- */
	{
		static const char cfg[] = "# juegos\nXYZ = flash512k  # comentario\r\nxyzJ=eeprom128k\nABC=flash1m\nQQQ=cinta\n";
		static const char cfg2[] = "IRD=eeprom8k\nIRDE=fram32k\n";
		static const char cfg3[] = "IRD=flash2m\n";
		static uint8_t sav[8192];

		memset(sav, 0x5A, sizeof sav);
		game("XYZE");
		CHECK_EQ(nds_save_cfg(&M, cfg, sizeof cfg - 1), 1);
		CHECK_EQ(M.save.type, NDS_SAVE_FLASH);
		CHECK_EQ(M.save.size, 524288);
		CHECK_EQ(M.save.src, NDS_SAVE_SRC_CFG);
		CHECK(logged("(cfg)"));
		/* el .sav se ajusta al chip forzado */
		CHECK_EQ(nds_save_attach(&M, sav, 8192), 0);
		CHECK_EQ(M.save.size, 524288);
		CHECK_EQ(M.save.src, NDS_SAVE_SRC_CFG);
		CHECK_EQ(M.savemem[8191], 0x5A);
		CHECK_EQ(M.savemem[8192], 0xFF);
		game("XYZJ"); /* la línea de 4 letras gana a la de 3 */
		CHECK_EQ(nds_save_cfg(&M, cfg, sizeof cfg - 1), 1);
		CHECK_EQ(M.save.type, NDS_SAVE_EEPROM24);
		game("QQQE");
		CHECK_EQ(nds_save_cfg(&M, cfg, sizeof cfg - 1), 0);
		CHECK(logged("desconocido"));
		game("IRDE"); /* gana a la tabla; la línea de 4 letras, a la de 3 */
		CHECK_EQ(nds_save_cfg(&M, cfg2, sizeof cfg2 - 1), 1);
		CHECK_EQ(M.save.type, NDS_SAVE_FRAM);
		game("IRDP");
		CHECK_EQ(nds_save_cfg(&M, cfg2, sizeof cfg2 - 1), 1);
		CHECK_EQ(M.save.type, NDS_SAVE_EEPROM16);
		CHECK_EQ(M.save.size, 8192);
		game("IRDE"); /* 2 MB no cabe: se avisa y queda la tabla */
		CHECK_EQ(nds_save_cfg(&M, cfg3, sizeof cfg3 - 1), 0);
		CHECK_EQ(M.save.src, NDS_SAVE_SRC_TABLA);
		CHECK_EQ(nds_save_cfg(&M, "", 0), 0);
	}

	/* ---- determinismo: la misma conversación da el mismo chip y el mismo contenido ---- */
	{
		static uint8_t a[NDS_SAVE_MAX];

		game("ZZZE");
		write_conv(0x02, 3, 0x100, 256, 0x01);
		memcpy(a, M.savemem, sizeof a);
		game("ZZZE");
		write_conv(0x02, 3, 0x100, 256, 0x01);
		CHECK(!memcmp(a, M.savemem, sizeof a));
	}
}
