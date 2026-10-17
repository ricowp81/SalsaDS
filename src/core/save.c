/*
 * SalsaDS — memoria de guardado del cartucho (AUXSPI): EEPROM, FRAM y Flash (Fase 1.3).
 *
 * El chip cuelga del bus serie del cartucho (AUXSPICNT/AUXSPIDATA). Se maneja byte a byte: cada byte que el
 * programa escribe en AUXSPIDATA es un byte de la conversación SPI y la respuesta del chip queda lista en
 * AUXSPIDATA. El chip se selecciona mientras el bit de retener (AUXSPICNT.6) siga puesto; al soltarlo, la
 * conversación termina y el chip vuelve a esperar un comando.
 *
 * Qué chip hay se decide en este orden (todo datos, nada de Nintendo):
 *   1. el tamaño del archivo de guardado, si lo hay (512 B, 8 KB, 32 KB, 64 KB, 128 KB, 256 KB, 512 KB, 1 MB);
 *   2. una tabla muy corta por código de juego (solo los juegos que el autor usa para probar);
 *   3. si no, EEPROM de 64 KB con dirección de 16 bits (la que contestan RDSR = 00 y RDID = FF FF FF).
 *
 * Fuente: GBATEK, «DS Cartridge Backup» (tipos, tamaños, juegos de órdenes, bits del registro de estado,
 * detección por RDSR/RDID y por el número de bytes de dirección). Código propio. Simplificaciones: las
 * escrituras y borrados terminan al instante (WIP siempre 0), la protección de bloques (BP) se guarda y se
 * lee pero no se aplica, y los chips con el puerto infrarrojo compartido no se imitan.
 */
#include <string.h>

#include "core/nds_priv.h"

enum { ST_CMD = 0, ST_ADDR, ST_DUMMY, ST_READ, ST_WRITE, ST_STATUS, ST_WRSR, ST_ID, ST_IGNORE };

/* Códigos de juego (primeros 3 caracteres, cualquier región) → tipo. Hechos: GBATEK (Super Mario 64 DS: EEPROM
 * de 8 KB; Mario Kart DS: Flash de 256 KB) y el archivo de guardado que crea melonDS (New Super Mario Bros.:
 * 8192 bytes). */
static const struct {
	char code[4];
	uint8_t type;
	uint32_t size;
} known[] = {
	{ "ASM", NDS_SAVE_EEPROM16, 8192 },
	{ "A2D", NDS_SAVE_EEPROM16, 8192 },
	{ "AMC", NDS_SAVE_FLASH, 262144 },
};

static void configure(struct nds_save *s, uint8_t type, uint32_t size)
{
	s->type = type;
	s->size = size;
	switch (type) {
	case NDS_SAVE_EEPROM512: s->addr_len = 1; s->page = 16; break;
	case NDS_SAVE_EEPROM16: s->addr_len = 2; s->page = size <= 8192 ? 32 : 128; break;
	case NDS_SAVE_FRAM: s->addr_len = 2; s->page = 0; break;
	case NDS_SAVE_EEPROM24: s->addr_len = 3; s->page = 256; break;
	default: s->addr_len = 3; s->page = 256; break; /* Flash */
	}
	s->state = ST_CMD;
	s->wel = 0;
}

static int type_of_size(size_t len, uint8_t *type)
{
	switch (len) {
	case 512: *type = NDS_SAVE_EEPROM512; return 1;
	case 8192: case 65536: *type = NDS_SAVE_EEPROM16; return 1;
	case 32768: *type = NDS_SAVE_FRAM; return 1;
	case 131072: *type = NDS_SAVE_EEPROM24; return 1;
	case 262144: case 524288: case 1048576: *type = NDS_SAVE_FLASH; return 1;
	default: return 0;
	}
}

/* Tipo por omisión según el código de juego de la cabecera. Se llama al cargar la ROM. */
void nds_save_default(struct nds *n)
{
	struct nds_save *s = &n->save;
	const uint8_t *rom = n->cart.rom;

	memset(n->savemem, 0xFF, sizeof n->savemem);
	configure(s, NDS_SAVE_EEPROM16, 65536);
	s->dirty = 0;
	if (!rom)
		return;
	for (size_t i = 0; i < sizeof known / sizeof known[0]; i++)
		if (!memcmp(rom + 0x0C, known[i].code, 3)) {
			configure(s, known[i].type, known[i].size);
			break;
		}
}

int nds_save_attach(struct nds *n, const uint8_t *data, size_t len)
{
	uint8_t type;

	if (!type_of_size(len, &type) || len > sizeof n->savemem)
		return -1;
	memset(n->savemem, 0xFF, sizeof n->savemem);
	memcpy(n->savemem, data, len);
	configure(&n->save, type, (uint32_t)len);
	n->save.dirty = 0;
	return 0;
}

const uint8_t *nds_save_data(const struct nds *n, size_t *len)
{
	*len = n->save.size;
	return n->savemem;
}

int nds_save_take_dirty(struct nds *n)
{
	const int d = n->save.dirty;

	n->save.dirty = 0;
	return d;
}

/* ---- el chip --------------------------------------------------------------------------------------------- */

static uint8_t status_byte(const struct nds_save *s)
{
	uint8_t v = (uint8_t)((s->wel << 1) | (s->wp << 2) | (s->srwd << 7));

	if (s->type == NDS_SAVE_EEPROM512)
		v |= 0xF0u;
	return v;
}

static void jedec(const struct nds_save *s, uint8_t id[3])
{
	id[0] = id[1] = id[2] = 0xFF;
	if (s->type == NDS_SAVE_FLASH) { /* ST M45PE: 20h 40h y 12h (256 KB), 13h (512 KB), 14h (1 MB) */
		id[0] = 0x20;
		id[1] = 0x40;
		id[2] = s->size >= 1048576 ? 0x14 : s->size >= 524288 ? 0x13 : 0x12;
	}
}

static void put(struct nds_save *s, struct nds *n, uint32_t addr, uint8_t v, int program_and)
{
	uint8_t *p = &n->savemem[addr % s->size];

	*p = program_and ? (uint8_t)(*p & v) : v;
	s->dirty = 1;
}

static void erase(struct nds_save *s, struct nds *n, uint32_t base, uint32_t len)
{
	base = (base / len) * len % s->size;
	memset(&n->savemem[base], 0xFF, len);
	s->dirty = 1;
}

/* Un byte de la conversación. Devuelve lo que el chip contesta en ese byte. */
uint8_t nds_save_xfer(struct nds *n, uint8_t in)
{
	struct nds_save *s = &n->save;
	uint8_t out = 0xFF;

	switch (s->state) {
	case ST_CMD: {
		const uint8_t c = in;
		const int tiny = s->type == NDS_SAVE_EEPROM512, flash = s->type == NDS_SAVE_FLASH;

		s->cmd = c;
		s->addr = 0;
		s->nbytes = 0;
		s->first = 1;
		s->a8 = 0;
		switch (c) {
		case 0x06: s->wel = 1; s->state = ST_IGNORE; break;
		case 0x04: s->wel = 0; s->state = ST_IGNORE; break;
		case 0x05: s->state = ST_STATUS; break;
		case 0x01: s->state = ST_WRSR; break;
		case 0x9F: s->state = ST_ID; break;
		case 0x03: s->state = ST_ADDR; break;
		case 0x0B: /* EEPROM pequeña: lectura con A8 = 1; Flash: lectura rápida (un byte de relleno) */
			s->a8 = 1;
			s->state = (tiny || flash) ? ST_ADDR : ST_IGNORE;
			break;
		case 0x02: /* escritura (EEPROM, FRAM; en la pequeña con A8 = 0) o programar página (Flash) */
			s->state = s->wel ? ST_ADDR : ST_IGNORE;
			break;
		case 0x0A: /* escritura con A8 = 1 (EEPROM pequeña) o escribir página, borrando antes (Flash) */
			s->a8 = 1;
			s->state = (s->wel && (tiny || flash)) ? ST_ADDR : ST_IGNORE;
			break;
		case 0xDB: case 0xD8: /* borrar página de 256 B / sector de 64 KB */
			s->state = (flash && s->wel) ? ST_ADDR : ST_IGNORE;
			break;
		case 0xC7: /* borrar todo */
			if (flash && s->wel) {
				erase(s, n, 0, s->size);
				s->wel = 0;
			}
			s->state = ST_IGNORE;
			break;
		default: s->state = ST_IGNORE; break;
		}
		return 0xFF;
	}
	case ST_ADDR:
		s->addr = (s->addr << 8) | in;
		if (++s->nbytes == s->addr_len) {
			if (s->type == NDS_SAVE_EEPROM512)
				s->addr |= (uint32_t)s->a8 << 8;
			s->addr %= s->size;
			s->nbytes = 0;
			if (s->cmd == 0x0B && s->type == NDS_SAVE_FLASH)
				s->state = ST_DUMMY;
			else if (s->cmd == 0x03 || s->cmd == 0x0B)
				s->state = ST_READ;
			else if (s->cmd == 0xDB) {
				erase(s, n, s->addr, 256);
				s->wel = 0;
				s->state = ST_IGNORE;
			} else if (s->cmd == 0xD8) {
				erase(s, n, s->addr, 65536);
				s->wel = 0;
				s->state = ST_IGNORE;
			} else {
				s->state = ST_WRITE;
			}
		}
		return 0xFF;
	case ST_DUMMY:
		s->state = ST_READ;
		return 0xFF;
	case ST_READ:
		out = n->savemem[s->addr % s->size];
		s->addr = (s->addr + 1) % s->size;
		return out;
	case ST_WRITE: {
		const uint32_t page = s->page ? s->page : s->size;
		const uint32_t base = s->addr - s->addr % page;

		if (s->first && s->type == NDS_SAVE_FLASH && s->cmd == 0x0A)
			erase(s, n, s->addr, 256); /* escribir página = borrar y programar */
		s->first = 0;
		put(s, n, s->addr, in, s->type == NDS_SAVE_FLASH && s->cmd == 0x02);
		s->addr = base + ((s->addr + 1) % page);
		s->wrote = 1;
		return 0xFF;
	}
	case ST_STATUS:
		return status_byte(s);
	case ST_WRSR:
		s->wp = (in >> 2) & 3u;
		s->srwd = (in >> 7) & 1u;
		s->wel = 0;
		s->state = ST_IGNORE;
		return 0xFF;
	case ST_ID: {
		uint8_t id[3];

		jedec(s, id);
		out = id[s->nbytes % 3];
		s->nbytes++;
		return out;
	}
	default:
		return 0xFF;
	}
}

/* El chip se deselecciona: termina la conversación. Una escritura o un borrado apaga WEL. */
void nds_save_deselect(struct nds *n)
{
	struct nds_save *s = &n->save;

	if (s->wrote) {
		s->wel = 0;
		s->wrote = 0;
	}
	s->state = ST_CMD;
}
