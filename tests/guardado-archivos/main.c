/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Las funciones bajo prueba se extraen de la aplicación: no se imita su lógica. */
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <sys/stat.h>
#include <unistd.h>
#include "core/nds_priv.h"

static struct nds machine;
static struct nds *nds = &machine;
static uint8_t rom_buf[512];
static const char *save_dir;
static char cfg_path[96];
static uint32_t frame_no;
#define SAVE_DIR save_dir
#define SALSADS_DATA_DIR save_dir
#define SAVE_CFG_PATH cfg_path
#define SAVE_FLUSH_FRAMES 120
static void salsads_log(const char *fmt, ...) { (void)fmt; }
void nds_logf(struct nds *n, const char *fmt, ...) { (void)n; (void)fmt; }
#include "guardado.inc"

#define REQUIRE(c) do { if (!(c)) { fprintf(stderr, "FALLO línea %d: %s\n", __LINE__, #c); return 1; } } while (0)

static void reset_game(void)
{
	memset(&machine, 0, sizeof machine);
	memcpy(rom_buf + 12, "AMHP", 4);
	machine.cart.rom = rom_buf;
	nds_save_default(&machine);
}

int main(int argc, char **argv)
{
	FILE *f;
	struct stat st;
	char original[96], alternate[96];
	uint8_t bytes[512];
	const uint8_t command[] = { 0x0A, 0x03, 0xDF, 0x00, 0x42, 0x43 };

	REQUIRE(argc == 2);
	save_dir = argv[1];
	snprintf(original, sizeof original, "%s/AMHP.sav", save_dir);
	snprintf(alternate, sizeof alternate, "%s/AMHP.flash256k.sav", save_dir);
	snprintf(cfg_path, sizeof cfg_path, "%s/guardado.cfg", save_dir);
	/* Rechazar un directorio usado evita sobrescribir archivos ajenos si se invoca a mano. */
	REQUIRE(lstat(original, &st) == -1 && errno == ENOENT);
	REQUIRE(lstat(alternate, &st) == -1 && errno == ENOENT);
	REQUIRE(lstat(cfg_path, &st) == -1 && errno == ENOENT);
	memset(bytes, 0xAA, sizeof bytes);
	f = fopen(original, "wb"); REQUIRE(f);
	REQUIRE(fwrite(bytes, 1, 512, f) == 512); REQUIRE(fclose(f) == 0);
	reset_game(); save_load();
	REQUIRE(!strcmp(save_path, alternate) && !save_blocked);
	REQUIRE(machine.save.size == 262144 && machine.savemem[0] == 0xFF);
	nds_save_xfer(nds, 0x06); nds_save_deselect(nds);
	for (size_t i = 0; i < sizeof command; i++)
		nds_save_xfer(nds, command[i]);
	nds_save_deselect(nds); save_flush(1);
	REQUIRE(stat(alternate, &st) == 0 && st.st_size == 262144);
	REQUIRE(stat(original, &st) == 0 && st.st_size == 512);
	f = fopen(original, "rb"); REQUIRE(f);
	REQUIRE(fread(bytes, 1, 512, f) == 512); fclose(f);
	for (size_t i = 0; i < 512; i++)
		REQUIRE(bytes[i] == 0xAA);
	reset_game(); save_load(); REQUIRE(!save_blocked);
	REQUIRE(machine.savemem[0x3DF00] == 0x42 && machine.savemem[0x3DF01] == 0x43);
	/* Un archivo alternativo inválido se conserva, aunque el juego vuelva a escribir. */
	f = fopen(alternate, "wb"); REQUIRE(f);
	REQUIRE(fwrite("mal", 1, 3, f) == 3); REQUIRE(fclose(f) == 0);
	reset_game(); save_load(); REQUIRE(save_blocked);
	machine.save.dirty = 1; save_flush(1);
	REQUIRE(stat(alternate, &st) == 0 && st.st_size == 3);
	f = fopen(alternate, "rb"); REQUIRE(f);
	REQUIRE(fread(bytes, 1, 3, f) == 3); fclose(f);
	REQUIRE(!memcmp(bytes, "mal", 3));
	/* Solo se retiran los archivos sintéticos creados por esta prueba. */
	REQUIRE(remove(original) == 0 && remove(alternate) == 0 && rmdir(save_dir) == 0);
	puts("OK recuperación: original intacto, Flash nueva, reapertura y bloqueo de archivo inválido");
	return 0;
}
