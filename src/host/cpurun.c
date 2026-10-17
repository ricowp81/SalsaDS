/*
 * SalsaDS — corre una ROM de prueba (tests/roms) en la máquina de src/core y lee su veredicto del bloque de
 * resultados (tests/roms/src/common/sds.h), como hace tools/melonds-run.py con melonDS.
 *
 *   salsads-cpurun [--cycles N] [--frames N] [--quantum N] [--stats] [--arm7] [--keys HEX] [--touch X,Y]
 *                  [--screens FILE.ppm] [--trace N [--trace7]] ROM.nds
 *
 * Código de salida: 0 = OK, 1 = FALLO, 2 = sin veredicto, 3 = una CPU cayó en un vector de excepción de la BIOS
 * sin manejador. Con --frames corre ese número de cuadros y termina con 0 (para sacar imágenes).
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "core/nds.h"
#include "util/endian.h"

#define SDS_ADDR 0x02380000u
#define SDS_ARM7_ADDR 0x02390000u
#define SDS_MAGIC 0x52534453u

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
	*len = (size_t)n;
	return buf;
}

static double now(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (double)ts.tv_sec + (double)ts.tv_nsec * 1e-9;
}

static void log_to_stderr(void *ctx, const char *msg)
{
	(void)ctx;
	fprintf(stderr, "núcleo: %s\n", msg);
}

/* Las dos pantallas, una sobre la otra, en un PPM de 256×384. */
static void save_ppm(const char *path)
{
	static uint16_t top[NDS_SCREEN_W * NDS_SCREEN_H], bot[NDS_SCREEN_W * NDS_SCREEN_H];
	FILE *f = fopen(path, "wb");

	if (!f)
		return;
	nds_screens(&M, top, bot);
	fprintf(f, "P6\n256 384\n255\n");
	for (int s = 0; s < 2; s++)
		for (int i = 0; i < NDS_SCREEN_W * NDS_SCREEN_H; i++) {
			const uint32_t v = (s ? bot : top)[i];
			const uint8_t px[3] = { (uint8_t)((v & 31) * 255 / 31), (uint8_t)(((v >> 5) & 31) * 255 / 31),
						(uint8_t)(((v >> 10) & 31) * 255 / 31) };

			fwrite(px, 1, 3, f);
		}
	fclose(f);
}

/* ¿Una CPU quedó atrapada en un vector de excepción de la BIOS (sin manejador)? */
static int stuck_in_vector(const struct nds *n, const char **which, uint32_t *pc)
{
	const uint32_t p9 = acore_pc(&n->a9), p7 = acore_pc(&n->a7);

	if ((p9 & 0xFFFFFFE0u) == 0xFFFF0000u && (p9 & 0x1Fu) != 0x18) {
		*which = "ARM9";
		*pc = p9;
		return 1;
	}
	if (p7 < 0x20 && (p7 & 0x1Fu) != 0x18) {
		*which = "ARM7";
		*pc = p7;
		return 1;
	}
	return 0;
}

int main(int argc, char **argv)
{
	uint64_t max_cycles = 4000000000ull;
	uint32_t keys = 0, tx = 0, ty = 0, quantum = 0;
	const char *rom_path = NULL, *screens_path = NULL;
	int stats = 0, show_arm7 = 0, trace_cpu = 0;
	uint64_t trace_n = 0, frames = 0;
	size_t len;
	uint8_t *rom;
	double t0, t1;

	for (int i = 1; i < argc; i++) {
		if (!strcmp(argv[i], "--cycles") && i + 1 < argc)
			max_cycles = strtoull(argv[++i], NULL, 0);
		else if (!strcmp(argv[i], "--frames") && i + 1 < argc)
			frames = strtoull(argv[++i], NULL, 0);
		else if (!strcmp(argv[i], "--quantum") && i + 1 < argc)
			quantum = (uint32_t)strtoul(argv[++i], NULL, 0);
		else if (!strcmp(argv[i], "--keys") && i + 1 < argc)
			keys = (uint32_t)strtoul(argv[++i], NULL, 16);
		else if (!strcmp(argv[i], "--touch") && i + 1 < argc) {
			sscanf(argv[++i], "%u,%u", &tx, &ty);
			keys |= NDS_KEY_PEN;
		} else if (!strcmp(argv[i], "--stats"))
			stats = 1;
		else if (!strcmp(argv[i], "--arm7"))
			show_arm7 = 1;
		else if (!strcmp(argv[i], "--trace") && i + 1 < argc)
			trace_n = strtoull(argv[++i], NULL, 0);
		else if (!strcmp(argv[i], "--trace7"))
			trace_cpu = 1;
		else if (!strcmp(argv[i], "--screens") && i + 1 < argc)
			screens_path = argv[++i];
		else
			rom_path = argv[i];
	}
	if (!rom_path) {
		fprintf(stderr, "uso: salsads-cpurun [--cycles N] [--frames N] [--quantum N] [--stats] [--arm7] "
				"[--keys HEX] [--touch X,Y] [--screens FILE.ppm] ROM.nds\n");
		return 2;
	}
	rom = slurp(rom_path, &len);
	if (!rom) {
		fprintf(stderr, "no puedo leer %s\n", rom_path);
		return 2;
	}
	M.log = log_to_stderr;
	M.quantum = quantum;
	nds_init(&M);
	if (nds_load_rom(&M, rom, len) || nds_boot_direct(&M)) {
		fprintf(stderr, "ROM inválida\n");
		return 2;
	}
	if (keys)
		nds_set_input(&M, keys, tx, ty);
	if (trace_n) {
		/* traza de instrucciones de una CPU (la otra avanza a su ritmo) */
		struct acore *c = trace_cpu ? &M.a7 : &M.a9;

		for (uint64_t i = 0; i < trace_n; i++) {
			const uint32_t pc = acore_pc(c);
			const uint32_t insn = nds_peek32(&M, trace_cpu, pc & ~3u);

			printf("%08x: %08x  cpsr=%08x r0=%08x r1=%08x r2=%08x r3=%08x sp=%08x lr=%08x\n", pc,
			       (c->cpsr & ACORE_T) ? (insn >> ((pc & 2) * 8)) & 0xFFFF : insn, c->cpsr, c->r[0], c->r[1],
			       c->r[2], c->r[3], c->r[13], c->r[14]);
			nds_run(&M, 1);
		}
		return 0;
	}
	t0 = now();
	if (frames) {
		for (uint64_t f = 0; f < frames; f++)
			nds_run_frame(&M);
		t1 = now();
		if (stats)
			printf("estadísticas: %llu cuadros, ARM9 %llu instr., ARM7 %llu instr., %.3f s, %.1f MIPS\n",
			       (unsigned long long)frames, (unsigned long long)M.a9.icount, (unsigned long long)M.a7.icount,
			       t1 - t0, (double)(M.a9.icount + M.a7.icount) / (t1 - t0) / 1e6);
		if (screens_path)
			save_ppm(screens_path);
		return 0;
	}
	{
		uint64_t done = 0;
		const uint64_t step = 1000000;
		uint32_t magic = 0, state = 0;
		const char *who;
		uint32_t pc;

		while (done < max_cycles) {
			nds_run(&M, step);
			done += step;
			magic = nds_peek32(&M, 0, SDS_ADDR);
			state = nds_peek32(&M, 0, SDS_ADDR + 8);
			if (magic == SDS_MAGIC && state == 1)
				break;
			if (stuck_in_vector(&M, &who, &pc)) {
				printf("EXCEPCIÓN %s sin manejador: pc = 0x%08x (ARM9 pc 0x%08x, ARM7 pc 0x%08x)\n", who, pc,
				       acore_pc(&M.a9), acore_pc(&M.a7));
				return 3;
			}
		}
	}
	t1 = now();

	{
		const char *name = strrchr(rom_path, '/');
		uint32_t w[12];
		int rc;

		name = name ? name + 1 : rom_path;
		for (int i = 0; i < 12; i++)
			w[i] = nds_peek32(&M, 0, SDS_ADDR + 4 * (uint32_t)i);
		if (stats) {
			const double secs = t1 - t0;
			const double mips = (double)(M.a9.icount + M.a7.icount) / secs / 1e6;

			printf("estadísticas: ARM9 %llu instr. (%llu ciclos), ARM7 %llu instr. (%llu ciclos), %.3f s, %.1f MIPS, "
			       "%llu cuadros\n",
			       (unsigned long long)M.a9.icount, (unsigned long long)M.a9.cycles,
			       (unsigned long long)M.a7.icount, (unsigned long long)M.a7.cycles, secs, mips,
			       (unsigned long long)M.frames);
		}
		if (screens_path)
			save_ppm(screens_path);
		if (show_arm7 && nds_peek32(&M, 0, SDS_ARM7_ADDR) == SDS_MAGIC) {
			uint32_t w7[12];

			for (int i = 0; i < 12; i++)
				w7[i] = nds_peek32(&M, 0, SDS_ARM7_ADDR + 4 * (uint32_t)i);
			fprintf(stderr, "  ARM7: estado %u, %u grupos, %u comprobaciones, %u fallos, %u divergencias\n", w7[2], w7[5],
				w7[3], w7[4], w7[11]);
			for (uint32_t i = 0; i < w7[4] && i < 16; i++) {
				const uint32_t a = SDS_ARM7_ADDR + 0x40 + 16 * i;

				fprintf(stderr, "  ARM7 fallo: grupo %u, id %u, obtuvo 0x%08x, esperaba 0x%08x\n",
					nds_peek32(&M, 0, a), nds_peek32(&M, 0, a + 4), nds_peek32(&M, 0, a + 8),
					nds_peek32(&M, 0, a + 12));
			}
			for (uint32_t i = 0; i < w7[11] && i < 32; i++) {
				const uint32_t a = SDS_ARM7_ADDR + 0x140 + 16 * i;

				fprintf(stderr, "  ARM7 divergencia: grupo %u, id %u, obtuvo 0x%08x, documentado 0x%08x\n",
					nds_peek32(&M, 0, a), nds_peek32(&M, 0, a + 4), nds_peek32(&M, 0, a + 8),
					nds_peek32(&M, 0, a + 12));
			}
		}
		for (uint32_t i = 0; i < w[4] && i < 16; i++) {
			const uint32_t a = SDS_ADDR + 0x40 + 16 * i;

			fprintf(stderr, "  fallo: grupo %u, id %u, obtuvo 0x%08x, esperaba 0x%08x\n", nds_peek32(&M, 0, a),
				nds_peek32(&M, 0, a + 4), nds_peek32(&M, 0, a + 8), nds_peek32(&M, 0, a + 12));
		}
		for (uint32_t i = 0; i < w[11] && i < 32; i++) {
			const uint32_t a = SDS_ADDR + 0x140 + 16 * i;

			fprintf(stderr, "  divergencia: grupo %u, id %u, obtuvo 0x%08x, documentado 0x%08x\n",
				nds_peek32(&M, 0, a), nds_peek32(&M, 0, a + 4), nds_peek32(&M, 0, a + 8),
				nds_peek32(&M, 0, a + 12));
		}
		if (w[0] != SDS_MAGIC || w[2] != 1) {
			printf("SIN VEREDICTO %s\n", name);
			return 2;
		}
		if (w[10] == 1 && w[4] == 0) {
			printf("OK %s: %u comprobaciones en %u grupos\n", name, w[3], w[5]);
			rc = 0;
		} else {
			printf("FALLO %s: %u de %u; primero en el grupo %u, id %u, obtuvo 0x%08x, esperaba 0x%08x\n", name,
			       w[4], w[3], w[6], w[7], w[8], w[9]);
			rc = 1;
		}
		return rc;
	}
}
