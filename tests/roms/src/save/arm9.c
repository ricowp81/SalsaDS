/*
 * SalsaDS — ROM save: la memoria de guardado del cartucho por AUXSPI (EEPROM de 0,5 KB, de 8 y 64 KB, FRAM de 32 KB
 * y Flash). El tipo y el tamaño del chip los pone el emulador (un archivo de guardado de ese tamaño); la ROM los
 * averigua con RDSR, RDID y los espejos de direcciones, como describe GBATEK, y comprueba el comportamiento de
 * esa clase de chip: bits del registro de estado, WREN/WRDI, escritura sin WREN, lectura de vuelta, páginas que
 * dan la vuelta, vuelta al final de la memoria y, en Flash, borrado de página y de sector, programación que solo
 * baja bits y escritura de página con borrado.
 *
 * Deja en el bloque de resultados (+0x34 y +0x38) el tipo detectado y el tamaño, para que quien la corre compruebe
 * que es el esperado. Código propio de SalsaDS; fuente de los comportamientos: GBATEK, «DS Cartridge Backup».
 */
#include <stdint.h>

#include "../common/hw.h"

enum { T_NONE = 0, T_EEPROM512 = 1, T_EEPROM16 = 2, T_FRAM = 3, T_FLASH = 4 };

static const char *const type_name[] = { "NINGUNO", "EEPROM 512 B", "EEPROM 16 BITS", "FRAM", "FLASH" };

static int type;
static uint32_t size, page;

/* ---- un byte por AUXSPI --------------------------------------------------------------------------------- */

static uint8_t xfer(uint8_t out, int hold)
{
	uint32_t spin = 0;

	REG16(R_AUXSPICNT) = (uint16_t)(0xA000u | (hold ? 0x40u : 0u));
	REG16(R_AUXSPIDATA) = out;
	while ((REG16(R_AUXSPICNT) & 0x80u) && spin++ < 100000u)
		;
	return (uint8_t)REG16(R_AUXSPIDATA);
}

/* los bytes de dirección; el último suelta el chip si `hold_last` es 0 (órdenes que terminan en la dirección) */
static void addr_out(uint32_t a, int bytes, int hold_last)
{
	for (int i = bytes - 1; i >= 0; i--)
		xfer((uint8_t)(a >> (8 * i)), i > 0 ? 1 : hold_last);
}

static uint8_t simple(uint8_t cmd) /* orden sin dirección que contesta un byte (RDSR) */
{
	uint8_t v;

	xfer(cmd, 1);
	v = xfer(0, 0);
	return v;
}

static void wren(void)
{
	xfer(0x06, 0);
}

static void wrdi(void)
{
	xfer(0x04, 0);
}

static uint32_t abytes(void)
{
	return type == T_EEPROM512 ? 1 : type == T_FLASH ? 3 : 2;
}

/* lee n bytes desde a */
static void rd(uint32_t a, uint8_t *dst, uint32_t n)
{
	if (type == T_EEPROM512) {
		xfer((uint8_t)(a & 0x100u ? 0x0B : 0x03), 1);
		xfer((uint8_t)a, 1);
	} else {
		xfer(0x03, 1);
		addr_out(a, (int)abytes(), 1);
	}
	for (uint32_t i = 0; i < n; i++)
		dst[i] = xfer(0, i + 1 < n);
}

/* escribe n bytes en a con la orden de escritura propia del tipo (WREN antes) */
static void wr(uint32_t a, const uint8_t *src, uint32_t n)
{
	wren();
	if (type == T_EEPROM512) {
		xfer((uint8_t)(a & 0x100u ? 0x0A : 0x02), 1);
		xfer((uint8_t)a, 1);
	} else if (type == T_FLASH) {
		xfer(0x0A, 1); /* escribir página: borra y programa */
		addr_out(a, 3, 1);
	} else {
		xfer(0x02, 1);
		addr_out(a, (int)abytes(), 1);
	}
	for (uint32_t i = 0; i < n; i++)
		xfer(src[i], i + 1 < n);
}

static void flash_cmd(uint8_t cmd, uint32_t a, const uint8_t *src, uint32_t n)
{
	wren();
	xfer(cmd, 1);
	addr_out(a, 3, n != 0);
	for (uint32_t i = 0; i < n; i++)
		xfer(src[i], i + 1 < n);
}

static uint32_t wait_idle(void)
{
	uint32_t spin = 0;

	/* con tope: melonDS tarda de verdad en escribir y borrar (el tiempo real del chip), SalsaDS termina al instante */
	while ((simple(0x05) & 1u) && spin++ < 2000000u)
		;
	return spin < 2000000u;
}

/* ---- detección ---------------------------------------------------------------------------------------------- */

static void detect(void)
{
	uint8_t id[3];
	const uint8_t st = simple(0x05);
	const volatile char *code = (const volatile char *)0x027FFE0Cu; /* código de juego de la cabecera */

	/* melonDS contesta FFh al RDID de su Flash: para correr esta ROM con él se le da el código de juego de un
	 * cartucho con Flash de 256 KB (un archivo de cabecera cambiado, `tools/melonds-run.py --gamecode`) */
	if (code[0] == 'A' && code[1] == 'M' && code[2] == 'C') {
		type = T_FLASH;
		size = 262144;
		page = 256;
		return;
	}
	xfer(0x9F, 1);
	id[0] = xfer(0, 1);
	id[1] = xfer(0, 1);
	id[2] = xfer(0, 0);
	if (st == 0xFF && id[0] == 0xFF) {
		type = T_NONE;
		return;
	}
	if ((st & 0xF0u) == 0xF0u && id[0] == 0xFF) {
		type = T_EEPROM512;
		size = 512;
		page = 16;
		return;
	}
	if (id[0] != 0xFF) {
		type = T_FLASH;
		size = id[2] == 0x14 ? 1048576u : id[2] == 0x13 ? 524288u : 262144u;
		page = 256;
		return;
	}
	/* EEPROM o FRAM de 16 bits: el tamaño sale de los espejos de dirección (GBATEK: la lectura revela el tamaño).
	 * Se escribe un marcador en 0 y se mira si aparece también en 8 KB o en 32 KB. */
	{
		uint8_t keep, at8k, at32k, mark, b, c;

		type = T_EEPROM16;
		size = 65536;
		page = 128;
		rd(0, &keep, 1);
		rd(8192, &at8k, 1);
		rd(32768, &at32k, 1);
		mark = (uint8_t)(keep ^ 0x5A);
		while (mark == at8k || mark == at32k)
			mark++;
		wr(0, &mark, 1);
		wait_idle();
		rd(8192, &b, 1);
		rd(32768, &c, 1);
		if (b == mark) {
			size = 8192;
			page = 32;
		} else if (c == mark) {
			size = 32768;
			type = T_FRAM;
			page = 0;
		}
		wr(0, &keep, 1);
		wait_idle();
	}
}

/* ---- comprobaciones -------------------------------------------------------------------------------------------- */

static void buf_set(uint8_t *b, uint32_t n, uint8_t seed)
{
	for (uint32_t i = 0; i < n; i++)
		b[i] = (uint8_t)(seed + i * 7u);
}

static void group_status(void)
{
	uint8_t st;

	sds_group("ESTADO");
	st = simple(0x05);
	if (type == T_EEPROM512)
		SDS_CHECK(st & 0xFFu, 0xF0);
	else
		SDS_CHECK(st & 0x73u, 0); /* WIP = 0, WEL = 0 y los bits 4-6 en cero */
	wren();
	st = simple(0x05);
	SDS_CHECK(st & 3u, 2); /* WEL puesto */
	wrdi();
	st = simple(0x05);
	SDS_CHECK(st & 3u, 0);
}

static void group_rw(void)
{
	uint8_t w[8], r[8], orig[8];
	const uint32_t a = 0x40;

	sds_group("ESCRIBIR Y LEER");
	rd(a, orig, 8);
	buf_set(w, 8, 0x31);
	/* sin WREN no se escribe nada */
	if (type == T_FLASH) {
		xfer(0x0A, 1);
		addr_out(a, 3, 1);
	} else if (type == T_EEPROM512) {
		xfer(0x02, 1);
		xfer((uint8_t)a, 1);
	} else {
		xfer(0x02, 1);
		addr_out(a, 2, 1);
	}
	for (int i = 0; i < 8; i++)
		xfer(w[i], i < 7);
	rd(a, r, 8);
	for (int i = 0; i < 8; i++)
		SDS_CHECK_ID(r[i], orig[i], 100 + i);
	/* con WREN sí; y WEL se apaga solo al terminar */
	wr(a, w, 8);
	SDS_CHECK(wait_idle(), 1);
	SDS_CHECK(simple(0x05) & 2u, 0);
	rd(a, r, 8);
	for (int i = 0; i < 8; i++)
		SDS_CHECK_ID(r[i], w[i], 200 + i);
	/* una lectura que sigue pasa de un byte al siguiente sin repetir ni saltarse ninguno */
	rd(a + 3, r, 4);
	for (int i = 0; i < 4; i++)
		SDS_CHECK_ID(r[i], w[i + 3], 300 + i);
}

static void group_wrap(void)
{
	uint8_t w[8], r[8], both[4], first[2];

	sds_group("VUELTA DE PAGINA Y DE MEMORIA");
	if (page) {
		/* escribir 6 bytes desde 2 antes del fin de una página: los 4 últimos dan la vuelta al principio */
		const uint32_t base = size >= 4096 ? 4 * page : 0;

		buf_set(w, 6, 0x77);
		wr(base + page - 2, w, 6);
		SDS_CHECK(wait_idle(), 1);
		rd(base + page - 2, r, 2);
		SDS_CHECK(r[0], w[0]);
		SDS_CHECK(r[1], w[1]);
		rd(base, r, 4);
		for (int i = 0; i < 4; i++)
			SDS_DIVERGE_ID(r[i], w[2 + i], 400 + i); /* melonDS 1.1 no da la vuelta dentro de la página */
	}
	/* la lectura da la vuelta al final de la memoria */
	rd(0, first, 2);
	rd(size - 2, both, 4);
	SDS_CHECK(both[2], first[0]);
	SDS_CHECK(both[3], first[1]);
}

static void group_tiny(void)
{
	uint8_t w[4], r[4];

	if (type != T_EEPROM512)
		return;
	sds_group("EEPROM 0,5 KB: BIT DE DIRECCIÓN 8");
	buf_set(w, 4, 0x11);
	wr(0x120, w, 4); /* WRHI (0Ah): dirección 0x120 */
	wait_idle();
	rd(0x120, r, 4); /* RDHI (0Bh) */
	for (int i = 0; i < 4; i++)
		SDS_CHECK_ID(r[i], w[i], 500 + i);
	rd(0x20, r, 4); /* RDLO (03h): otra mitad */
	SDS_DIVERGE(r[0] == w[0], 0);
}

static void group_flash(void)
{
	uint8_t w[16], r[16];
	const uint32_t a = 0x1200;

	if (type != T_FLASH)
		return;
	sds_group("FLASH");
	/* identificador: 20h 40h y 12h/13h/14h según el tamaño */
	{
		uint8_t id[3];

		xfer(0x9F, 1);
		id[0] = xfer(0, 1);
		id[1] = xfer(0, 1);
		id[2] = xfer(0, 0);
		SDS_DIVERGE(id[0], 0x20); /* melonDS 1.1 contesta FFh a todo */
		SDS_DIVERGE(id[1], 0x40);
		SDS_DIVERGE(id[2], size >= 1048576u ? 0x14 : size >= 524288u ? 0x13 : 0x12);
	}
	/* borrar la página (DBh): queda en FFh (melonDS 1.1 la deja en 00h) */
	flash_cmd(0xDB, a, 0, 0);
	wait_idle();
	rd(a, r, 16);
	for (int i = 0; i < 16; i++)
		SDS_DIVERGE_ID(r[i], 0xFF, 600 + i);
	/* escribir página (0Ah) borra antes: sobre cualquier cosa queda exactamente lo escrito, y el resto de la
	 * página queda borrado */
	for (int i = 0; i < 16; i++)
		w[i] = 0x0F;
	wr(a, w, 16);
	wait_idle();
	rd(a, r, 16);
	for (int i = 0; i < 16; i++)
		SDS_CHECK_ID(r[i], 0x0F, 700 + i);
	rd(a + 16, r, 4);
	for (int i = 0; i < 4; i++)
		SDS_DIVERGE_ID(r[i], 0xFF, 750 + i); /* melonDS 1.1: 00h */
	/* programar página (02h) solo baja bits: 0x0F y 0x3C dan 0x0C */
	for (int i = 0; i < 16; i++)
		w[i] = 0x3C;
	flash_cmd(0x02, a, w, 16);
	wait_idle();
	rd(a, r, 16);
	for (int i = 0; i < 16; i++)
		SDS_DIVERGE_ID(r[i], 0x0C, 800 + i); /* melonDS 1.1 deja 00h */
	/* sin WREN no se borra */
	xfer(0xDB, 1);
	addr_out(a, 3, 0);
	rd(a, r, 4);
	SDS_DIVERGE(r[0], 0x0C);
	/* borrar el sector (D8h): todo el sector de 64 KB queda en FFh (melonDS 1.1: 00h) */
	wr(a + 0x80, w, 4);
	wait_idle();
	flash_cmd(0xD8, a, 0, 0);
	wait_idle();
	rd(a, r, 4);
	SDS_DIVERGE(r[0], 0xFF);
	rd(a + 0x80, r, 4);
	SDS_DIVERGE(r[0], 0xFF);
	rd(0x10000 - 4, r, 4); /* el último byte del sector 0 */
	SDS_DIVERGE(r[3], 0xFF);
}

int main(void)
{
	sds_init("SALSADS SAVE");
	sds_group("DETECCIÓN");
	detect();
	MEM32(0x02380034u) = (uint32_t)type;
	MEM32(0x02380038u) = size;
	sds_print_row(12, type_name[type]);
	SDS_CHECK(type != T_NONE, 1);
	if (type != T_NONE) {
		group_status();
		group_rw();
		group_wrap();
		group_tiny();
		group_flash();
	}
	sds_finish();
}
