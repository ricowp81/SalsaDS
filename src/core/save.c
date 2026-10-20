/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * SalsaDS — memoria de guardado del cartucho (AUXSPI): EEPROM, FRAM y Flash (Fase 1.3).
 *
 * El chip cuelga del bus serie del cartucho (AUXSPICNT/AUXSPIDATA). Se maneja byte a byte: cada byte que el
 * programa escribe en AUXSPIDATA es un byte de la conversación SPI y la respuesta del chip queda lista en
 * AUXSPIDATA. El chip se selecciona mientras el bit de retener (AUXSPICNT.6) siga puesto; al soltarlo, la
 * conversación termina y el chip vuelve a esperar un comando.
 *
 * Qué chip hay se decide en este orden (todo datos, nada de Nintendo; el log dice cuál valió):
 *   1. guardado.cfg del usuario («CODIGO=flash512k»), si tiene una línea de este juego: «cfg»;
 *   2. el tamaño del archivo de guardado, si lo hay (512 B, 8 KB, 32 KB, 64 KB, 128 KB, 256 KB, 512 KB, 1 MB): «.sav»;
 *   3. la tabla por código de juego (`known`): «tabla»;
 *   4. si no, EEPROM de 64 KB provisional (la que contestan RDSR = 00 y RDID = FF FF FF) y, mientras el juego no
 *      haya dejado claro qué chip espera, se deduce por sus primeras conversaciones SPI (`detect_*`): «detectado».
 *
 * Fuente: GBATEK, «DS Cartridge Backup» (tipos, tamaños, juegos de órdenes, bits del registro de estado,
 * detección por RDSR/RDID y por el número de bytes de dirección). Las excepciones por código de juego con
 * dirección de un byte vienen de la lista blanca de DeSmuME (src/mc.cpp, GPL-2.0-o-posterior); la idea de deducir por
 * el número de bytes de dirección, de su autodetección. Código propio. Simplificaciones: las escrituras y borrados
 * terminan al instante (WIP siempre 0), la protección de bloques (BP) se guarda y se lee pero no se aplica, y los
 * chips con el puerto infrarrojo compartido no se imitan.
 */
#include <ctype.h>
#include <string.h>

#include "core/nds_priv.h"

enum { ST_CMD = 0, ST_ADDR, ST_DUMMY, ST_READ, ST_WRITE, ST_STATUS, ST_WRSR, ST_ID, ST_IGNORE };

/* Códigos de juego (los 3 primeros caracteres de la cabecera, cualquier región; con 4 si la región cambia el chip, y
 * entonces va antes) → tipo. Es un dato de compatibilidad, no obra de nadie:
 *   - GBATEK: Super Mario 64 DS, EEPROM de 8 KB; Mario Kart DS, Flash de 256 KB; el archivo que crea melonDS para New
 *     Super Mario Bros. mide 8192 bytes (hecho observable, no se tomó código).
 *   - DeSmuME, mc.cpp (lista blanca por código, la única tabla por juego que trae; GPL-2.0-o-posterior): ASMK (Super Mario
 *     64 DS coreano), AL3, AH5, AVH y AQ3 usan dirección de un byte (EEPROM de 4 Kbit = 512 B); BDE y BPV, de dos.
 *   - Pokémon (Diamante, Perla, Platino, HeartGold, SoulSilver, Negro, Blanco, Negro 2 y Blanco 2): Flash de 4 Mbit
 *     = 512 KB en todas las regiones (por eso el código de 3 letras).
 * Un juego que no esté aquí no se pierde: la detección automática lo deduce. Una EEPROM de 8 KB funciona igual como de 64 KB
 * (las direcciones del juego caben y sus páginas de 32 B caben en las de 128 B), así que esta tabla solo importa para lo
 * que cambia el protocolo: dirección de un byte, Flash y tamaños mayores. */
static const struct {
	char code[5];
	uint8_t type;
	uint32_t size;
} known[] = {
	{ "ASMK", NDS_SAVE_EEPROM512, 512 },
	{ "ASM", NDS_SAVE_EEPROM16, 8192 },
	{ "A2D", NDS_SAVE_EEPROM16, 8192 },
	{ "AMC", NDS_SAVE_FLASH, 262144 },
	{ "AL3", NDS_SAVE_EEPROM512, 512 },
	{ "AH5", NDS_SAVE_EEPROM512, 512 },
	{ "AVH", NDS_SAVE_EEPROM512, 512 },
	{ "AQ3", NDS_SAVE_EEPROM512, 512 },
	{ "BDE", NDS_SAVE_EEPROM16, 65536 },
	{ "BPV", NDS_SAVE_EEPROM16, 65536 },
	{ "ADA", NDS_SAVE_FLASH, 524288 }, /* Pokémon Diamante */
	{ "APA", NDS_SAVE_FLASH, 524288 }, /* Pokémon Perla */
	{ "CPU", NDS_SAVE_FLASH, 524288 }, /* Pokémon Platino */
	{ "IPK", NDS_SAVE_FLASH, 524288 }, /* Pokémon HeartGold */
	{ "IPG", NDS_SAVE_FLASH, 524288 }, /* Pokémon SoulSilver */
	{ "IRB", NDS_SAVE_FLASH, 524288 }, /* Pokémon Negro */
	{ "IRA", NDS_SAVE_FLASH, 524288 }, /* Pokémon Blanco */
	{ "IRE", NDS_SAVE_FLASH, 524288 }, /* Pokémon Negro 2 */
	{ "IRD", NDS_SAVE_FLASH, 524288 }, /* Pokémon Blanco 2 */
};

/* Nombres de los chips (los de guardado.cfg y los del log). */
static const struct {
	const char *name;
	uint8_t type;
	uint32_t size;
} chips[] = {
	{ "eeprom512b", NDS_SAVE_EEPROM512, 512 },
	{ "eeprom8k", NDS_SAVE_EEPROM16, 8192 },
	{ "eeprom64k", NDS_SAVE_EEPROM16, 65536 },
	{ "eeprom128k", NDS_SAVE_EEPROM24, 131072 },
	{ "fram32k", NDS_SAVE_FRAM, 32768 },
	{ "flash256k", NDS_SAVE_FLASH, 262144 },
	{ "flash512k", NDS_SAVE_FLASH, 524288 },
	{ "flash1m", NDS_SAVE_FLASH, 1048576 },
};

static const char *chip_name(uint8_t type, uint32_t size)
{
	for (size_t i = 0; i < sizeof chips / sizeof chips[0]; i++)
		if (chips[i].type == type && chips[i].size == size)
			return chips[i].name;
	return "desconocido";
}

static void say(struct nds *n, const char *how)
{
	nds_logf(n, "guardado: chip %s de %u bytes (%s)", chip_name(n->save.type, n->save.size), (unsigned)n->save.size, how);
}

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

/* ¿El código `code` (3 o 4 caracteres, en mayúsculas) es el del juego de la cabecera? Devuelve su largo o 0. */
static size_t code_match(const uint8_t *rom, const char *code)
{
	const size_t l = strlen(code);

	return (l == 3 || l == 4) && !memcmp(rom + 0x0C, code, l) ? l : 0;
}

/* Tipo por omisión según el código de juego de la cabecera. Se llama al cargar la ROM. */
void nds_save_default(struct nds *n)
{
	struct nds_save *s = &n->save;
	const uint8_t *rom = n->cart.rom;

	memset(n->savemem, 0xFF, sizeof n->savemem);
	configure(s, NDS_SAVE_EEPROM16, 65536);
	s->dirty = 0;
	s->src = NDS_SAVE_SRC_DEFECTO;
	s->autod = 1;
	s->nrec = 0;
	s->nconv = 0;
	if (rom)
		for (size_t i = 0; i < sizeof known / sizeof known[0]; i++)
			if (code_match(rom, known[i].code)) {
				configure(s, known[i].type, known[i].size);
				s->src = NDS_SAVE_SRC_TABLA;
				s->autod = 0;
				break;
			}
	if (s->src == NDS_SAVE_SRC_TABLA)
		say(n, "tabla");
	else if (rom)
		say(n, "defecto, provisional: se deduce por los comandos SPI del juego");
}

/* guardado.cfg: líneas «CODIGO=tipo» (# comenta). Gana la línea cuyo código es más largo; a igual largo, la última. */
int nds_save_cfg(struct nds *n, const char *text, size_t len)
{
	struct nds_save *s = &n->save;
	const uint8_t *rom = n->cart.rom;
	int best = -1;
	size_t best_len = 0;
	size_t i = 0;

	if (!rom || !text)
		return 0;
	while (i < len) {
		char code[8], name[24];
		size_t a = 0, b = 0, l;

		while (i < len && (text[i] == ' ' || text[i] == '\t'))
			i++;
		while (i < len && text[i] != '=' && text[i] != '\n' && text[i] != '\r' && a < sizeof code - 1)
			code[a++] = (char)toupper((unsigned char)text[i++]);
		code[a] = 0;
		while (a && (code[a - 1] == ' ' || code[a - 1] == '\t'))
			code[--a] = 0;
		if (i < len && text[i] == '=') {
			i++;
			while (i < len && (text[i] == ' ' || text[i] == '\t'))
				i++;
			while (i < len && text[i] != '\n' && text[i] != '\r' && text[i] != '#' && text[i] != ' ' && text[i] != '\t' &&
			       b < sizeof name - 1)
				name[b++] = (char)tolower((unsigned char)text[i++]);
			name[b] = 0;
			if (code[0] != '#' && (l = code_match(rom, code)) != 0) {
				int k = -1;

				for (size_t j = 0; j < sizeof chips / sizeof chips[0]; j++)
					if (!strcmp(name, chips[j].name))
						k = (int)j;
				if (k < 0)
					nds_logf(n, "guardado.cfg: tipo «%s» desconocido para %s (tipos: eeprom512b eeprom8k eeprom64k eeprom128k fram32k flash256k flash512k flash1m)",
						 name, code);
				else if (l >= best_len) {
					best = k;
					best_len = l;
				}
			}
		}
		while (i < len && text[i] != '\n') /* hasta el final de la línea */
			i++;
		i++;
	}
	if (best < 0)
		return 0;
	memset(n->savemem, 0xFF, sizeof n->savemem);
	configure(s, chips[best].type, chips[best].size);
	s->dirty = 0;
	s->src = NDS_SAVE_SRC_CFG;
	s->autod = 0;
	say(n, "cfg");
	return 1;
}

int nds_save_attach(struct nds *n, const uint8_t *data, size_t len)
{
	struct nds_save *s = &n->save;
	uint8_t type;

	if (len > sizeof n->savemem)
		return -1;
	if (s->src == NDS_SAVE_SRC_CFG) { /* el usuario fijó el chip: el archivo se ajusta a él (sobra se corta, falta se rellena) */
		memset(n->savemem, 0xFF, sizeof n->savemem);
		memcpy(n->savemem, data, len < s->size ? len : s->size);
		s->dirty = len != s->size;
		s->state = ST_CMD;
		if (len != s->size)
			nds_logf(n, "guardado: el .sav mide %u bytes y el chip de guardado.cfg %u: se ajusta", (unsigned)len, (unsigned)s->size);
		return 0;
	}
	if (!type_of_size(len, &type))
		return -1;
	if (s->src == NDS_SAVE_SRC_TABLA && s->type == NDS_SAVE_FLASH && len == 65536) {
		/* Un .sav de 64 KB para un juego que la tabla da como Flash es el que creó el chip por omisión de versiones
		 * anteriores: se conserva su contenido en el chip correcto (y se reescribe con el tamaño bueno). */
		memset(n->savemem, 0xFF, sizeof n->savemem);
		memcpy(n->savemem, data, len);
		s->state = ST_CMD;
		s->dirty = 1;
		nds_logf(n, "guardado: el .sav de 64 KB venía del chip por omisión; se pasa al %s de la tabla", chip_name(s->type, s->size));
		return 0;
	}
	memset(n->savemem, 0xFF, sizeof n->savemem);
	memcpy(n->savemem, data, len);
	configure(s, type, (uint32_t)len);
	s->dirty = 0;
	s->src = NDS_SAVE_SRC_SAV;
	s->autod = 0;
	say(n, ".sav");
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
static uint8_t xfer_core(struct nds *n, uint8_t in)
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

static void deselect_core(struct nds *n)
{
	struct nds_save *s = &n->save;

	if (s->wrote) {
		s->wel = 0;
		s->wrote = 0;
	}
	s->state = ST_CMD;
}

/* ---- deducir el chip por lo que el juego le dice ------------------------------------------------------------ */
/* Mientras `autod` sea 1 el chip es el provisional (EEPROM de 64 KB). Se guardan los bytes de cada conversación y, en cuanto una
 * deja clara la clase de chip, se cambia (sin tocar nada si el provisional ya era el bueno) y se repite esa conversación sobre el
 * chip nuevo, de modo que lo que el juego escribió cae donde debe. Las reglas son conservadoras (solo lo que no admite otra
 * lectura); lo ambiguo sigue con el provisional, que sirve a cualquier EEPROM de hasta 64 KB:
 *   - RDID (9Fh), borrar página (DBh), borrar sector (D8h) o borrar todo (C7h): Flash, que se supone de 512 KB (la tabla o guardado.cfg
 *     dicen otro tamaño; el .sav también);
 *   - lectura/escritura con A8 (0Bh, 0Ah): EEPROM de 512 B;
 *   - lectura (03h) de solo 2 bytes tras la orden (un byte de dirección y uno de datos): EEPROM de 512 B (un chip de 2 bytes de
 *     dirección no puede leer con menos de 3);
 *   - escritura (02h) de 131 a 259 bytes tras la orden: dirección de 3 bytes y página de 256 (EEPROM de 128 KB; ningún chip
 *     de 2 bytes de dirección escribe más de 128 + 2); de más de 259: FRAM (sin límite de página);
 *   - escritura de exactamente 130 bytes: página de 128 con 2 de dirección, o sea el provisional queda confirmado. */
static void retype(struct nds *n, uint8_t type, uint32_t size, int replay, const char *why)
{
	struct nds_save *s = &n->save;
	uint8_t rec[NDS_SAVE_REC_MAX];
	const unsigned nrec = s->nrec;
	const uint8_t wel = s->rec_wel, wel_now = s->wel;
	const int changes = s->type != type || s->size != size;

	memcpy(rec, s->rec, sizeof rec);
	if (changes) {
		if (s->dirty)
			nds_logf(n, "guardado: se descartan las escrituras anteriores a la detección");
		memset(n->savemem, 0xFF, sizeof n->savemem);
		configure(s, type, size);
		s->wel = wel_now; /* un WREN anterior sigue valiendo para la orden que se está por procesar */
		s->dirty = 0;
	}
	s->autod = 0;
	s->src = NDS_SAVE_SRC_DETECTADO;
	nds_logf(n, "guardado: detección: %s", why);
	say(n, "detectado");
	if (changes && replay) {
		s->wel = wel;
		for (unsigned i = 0; i < nrec; i++)
			(void)xfer_core(n, rec[i]);
	}
}

/* La orden dice qué chip es sin esperar al resto de la conversación. */
static void detect_by_command(struct nds *n, uint8_t cmd)
{
	switch (cmd) {
	case 0x9F: retype(n, NDS_SAVE_FLASH, 524288, 0, "RDID (9Fh): Flash"); break;
	case 0xD8: case 0xDB: case 0xC7: retype(n, NDS_SAVE_FLASH, 524288, 0, "borrado de Flash (D8h/DBh/C7h)"); break;
	case 0x0A: case 0x0B: retype(n, NDS_SAVE_EEPROM512, 512, 0, "orden con A8 (0Ah/0Bh): EEPROM de 512 B"); break;
	default: break;
	}
}

/* Una conversación terminó (o, en la escritura larga, ya pasó de 259 bytes): ¿dice algo del chip? */
static void detect_by_conversation(struct nds *n)
{
	struct nds_save *s = &n->save;
	const uint32_t c = s->nconv;

	if (!s->nrec)
		return;
	if (s->rec[0] == 0x03 && c == 2)
		retype(n, NDS_SAVE_EEPROM512, 512, 0, "lectura con un byte de dirección: EEPROM de 512 B");
	else if (s->rec[0] == 0x02 && c == 130)
		retype(n, NDS_SAVE_EEPROM16, 65536, 1, "escritura de página de 128 con 2 de dirección: EEPROM de 64 KB");
	else if (s->rec[0] == 0x02 && c >= 131 && c <= 259)
		retype(n, NDS_SAVE_EEPROM24, 131072, 1, "escritura de página de 256 con 3 de dirección: EEPROM de 128 KB");
	else if (s->rec[0] == 0x02 && c > 259)
		retype(n, NDS_SAVE_FRAM, 32768, 1, "escritura sin límite de página: FRAM de 32 KB");
}

uint8_t nds_save_xfer(struct nds *n, uint8_t in)
{
	struct nds_save *s = &n->save;
	uint8_t out;

	if (!s->autod)
		return xfer_core(n, in);
	if (s->state == ST_CMD) {
		s->nrec = 0;
		s->nconv = 0;
		s->rec_wel = s->wel;
		detect_by_command(n, in);
		if (!s->autod)
			return xfer_core(n, in);
		s->rec[s->nrec++] = in;
		return xfer_core(n, in);
	}
	if (s->nrec < NDS_SAVE_REC_MAX)
		s->rec[s->nrec++] = in;
	s->nconv++;
	out = xfer_core(n, in);
	if (s->rec[0] == 0x02 && s->nconv == 260) /* ya no puede ser una EEPROM: se decide a media conversación */
		detect_by_conversation(n);
	return out;
}

/* El chip se deselecciona: termina la conversación. Una escritura o un borrado apaga WEL. */
void nds_save_deselect(struct nds *n)
{
	struct nds_save *s = &n->save;

	if (s->autod && s->nrec && s->nconv <= 259) {
		deselect_core(n);
		detect_by_conversation(n);
		s->nrec = 0;
		deselect_core(n);
		return;
	}
	deselect_core(n);
	if (s->autod)
		s->nrec = 0;
}
