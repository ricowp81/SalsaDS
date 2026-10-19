/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * SalsaDS — prueba de abrir y cerrar juegos muchas veces (Fase 49).
 *
 * Repite, con la misma estructura `nds` y el mismo orden que el programa de la PS3 (tests/phase1/nucleo/main.c):
 * nds_init, cargar la ROM (entera o por fragmentos), arranque directo, unos cuadros, cerrar (nds_rom_free y soltar la
 * ROM). Comprueba que el heap en uso no crece entre cierres (tolerancia de unos KB) y que el tope del heap tampoco,
 * pasados los ciclos de calentamiento. Con --jit los recompiladores del ARM9 y del ARM7 se conectan una sola vez y sobreviven a los
 * ciclos (nds_init lo recrea), como en la consola; solo corre en PowerPC64 (qemu).
 *
 *   salsads-ciclos [--ciclos N] [--cuadros N] [--jit] ROM.nds
 *
 * Código propio (GPL-2.0-or-later).
 */
#include <malloc.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "core/nds.h"
#include "host/jitrun.h"

static struct nds M;

static uint8_t *slurp(const char *path, size_t *len)
{
	FILE *f = fopen(path, "rb");
	uint8_t *buf;
	long n;

	if (!f)
		return NULL;
	fseek(f, 0, SEEK_END);
	n = ftell(f);
	fseek(f, 0, SEEK_SET);
	buf = malloc((size_t)n);
	if (buf && fread(buf, 1, (size_t)n, f) != (size_t)n) {
		free(buf);
		buf = NULL;
	}
	fclose(f);
	*len = buf ? (size_t)n : 0;
	return buf;
}

static int file_rd(void *user, uint64_t off, uint8_t *dst, size_t len)
{
	FILE *f = user;

	if (fseek(f, (long)off, SEEK_SET) != 0)
		return -1;
	return (int)fread(dst, 1, len, f);
}

/* Heap en uso (bytes) y tamaño de la arena (lo que pidió al sistema) */
static void heap_now(size_t *used, size_t *arena)
{
#if defined(__GLIBC__) && (__GLIBC__ > 2 || __GLIBC_MINOR__ >= 33)
	const struct mallinfo2 mi = mallinfo2();
#else
	const struct mallinfo mi = mallinfo();
#endif
	*used = (size_t)mi.uordblks + (size_t)mi.hblkhd; /* hblkhd: bloques grandes por mmap */
	*arena = (size_t)mi.arena + (size_t)mi.hblkhd;
}

/* Un ciclo: abrir, correr, cerrar. paged: 0 ROM entera, 1 por fragmentos. */
static int cycle(const char *path, int paged, unsigned frames)
{
	size_t len = 0;
	uint8_t *rom = NULL;
	FILE *f = NULL;
	int rc = 0;

	nds_init(&M);
	if (paged) {
		f = fopen(path, "rb");
		if (!f)
			return -1;
		fseek(f, 0, SEEK_END);
		len = (size_t)ftell(f);
		rc = nds_load_rom_paged(&M, file_rd, f, len, 12 + 2, (size_t)1 << 20);
	} else {
		rom = slurp(path, &len);
		rc = rom ? nds_load_rom(&M, rom, len) : -1;
	}
	if (rc == 0)
		rc = nds_boot_direct(&M);
	if (rc == 0)
		for (unsigned i = 0; i < frames; i++)
			nds_run_frame(&M);
	nds_rom_free(&M);
	if (f)
		fclose(f);
	free(rom);
	return rc;
}

int main(int argc, char **argv)
{
	const char *path = NULL;
	unsigned cycles = 10, frames = 20;
	int jit = 0, bad = 0;

	setvbuf(stdout, NULL, _IOLBF, 0);
	for (int i = 1; i < argc; i++) {
		if (!strcmp(argv[i], "--ciclos") && i + 1 < argc)
			cycles = (unsigned)atoi(argv[++i]);
		else if (!strcmp(argv[i], "--cuadros") && i + 1 < argc)
			frames = (unsigned)atoi(argv[++i]);
		else if (!strcmp(argv[i], "--jit"))
			jit = 1;
		else
			path = argv[i];
	}
	if (!path) {
		fprintf(stderr, "uso: salsads-ciclos [--ciclos N] [--cuadros N] [--jit] ROM.nds\n");
		return 2;
	}
	nds_init(&M);
	if (jit && hj_attach(&M, 0, 0, 0, 0, 1, 1, NULL, NULL)) {
		fprintf(stderr, "sin recompilador en este host: se prueba con el intérprete\n");
		jit = 0;
	}
	for (int paged = 0; paged < 2; paged++) {
		size_t used0 = 0, arena0 = 0;

		for (unsigned c = 0; c < cycles; c++) {
			size_t used, arena;

			if (cycle(path, paged, frames) != 0) {
				printf("FALLO ciclo %u (%s): no abrió la ROM\n", c, paged ? "fragmentos" : "entera");
				return 1;
			}
			heap_now(&used, &arena);
			printf("ciclo %2u (%s%s): heap en uso %zu KB, arena %zu KB\n", c, paged ? "fragmentos" : "entera",
			       jit ? ", jit" : "", used >> 10, arena >> 10);
			if (c == 1) { /* tras dos aperturas ya está todo lo que el primer juego reserva para siempre */
				used0 = used;
				arena0 = arena;
			}
			if (c > 1 && (used > used0 + 4096 || arena > arena0 + 4096)) {
				printf("FALLO ciclo %u (%s): el heap creció (en uso %+ld bytes, arena %+ld bytes)\n", c,
				       paged ? "fragmentos" : "entera", (long)used - (long)used0, (long)arena - (long)arena0);
				bad = 1;
			}
		}
	}
	printf(bad ? "FALLO ciclos\n" : "OK ciclos: %u aperturas y cierres (ROM entera y por fragmentos), el heap no crece\n", cycles);
	return bad;
}
