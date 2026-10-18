/*
 * SalsaDS — corre una ROM de prueba (tests/roms) en la máquina de src/core y lee su veredicto del bloque de
 * resultados (tests/roms/src/common/sds.h), como hace tools/melonds-run.py con melonDS.
 *
 *   salsads-cpurun [--cycles N] [--frames N] [--quantum N] [--stats] [--arm7] [--keys HEX] [--touch X,Y]
 *                  [--screens FILE.ppm] [--ref] [--vs-ref] [--expect] [--same-ab] [--unmapped] [--peek ADDR]... [--trace N [--trace7]] ROM.nds
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

/* Lista de píxeles esperados que dejan las ROMs de gráficos (tests/roms/src/common/sds.h: sds_expect_pixel). Se comprueban
 * contra la imagen de cada motor del último cuadro. Devuelve el número de fallos (y los imprime). */
static int check_expect(void)
{
	const uint32_t n = nds_peek32(&M, 0, 0x02380804u);
	int bad = 0;

	if (nds_peek32(&M, 0, 0x02380800u) != 0x45505847u) {
		printf("EXPECT: la ROM no dejó lista de píxeles esperados\n");
		return 1;
	}
	nds_video_render_host(&M);
	for (uint32_t i = 0; i < n && i < 1000; i++) {
		const uint32_t a = nds_peek32(&M, 0, 0x02380808u + 8 * i), b = nds_peek32(&M, 0, 0x0238080Cu + 8 * i);
		const int scr = (int)(a & 1), x = (int)(a >> 16), y = (int)(b & 0xFFFF);
		const uint16_t want = gpu2d_to_5551((uint16_t)(b >> 16)), got = M.vid.img[scr][y * NDS_SCREEN_W + x];

		if (got != want) {
			if (bad < 20)
				printf("EXPECT fallo: motor %c (%d,%d): obtuvo 0x%04x, esperaba 0x%04x\n", 'A' + scr, x, y, got, want);
			bad++;
		}
	}
	printf("EXPECT: %u comprobaciones, %d fallos\n", n, bad);
	return bad;
}

/* Las dos pantallas iguales (las ROMs de gráficos montan la misma escena en los dos motores) */
static int check_same_ab(void)
{
	int diffs = 0, first = -1;

	nds_video_render_host(&M);
	for (int i = 0; i < NDS_SCREEN_W * NDS_SCREEN_H; i++)
		if (M.vid.img[0][i] != M.vid.img[1][i]) {
			if (first < 0)
				first = i;
			diffs++;
		}
	if (diffs)
		printf("A==B: FALLO, %d píxeles distintos (primero en %d,%d: A 0x%04x, B 0x%04x)\n", diffs, first % NDS_SCREEN_W,
		       first / NDS_SCREEN_W, M.vid.img[0][first], M.vid.img[1][first]);
	else
		printf("A==B: OK\n");
	return diffs != 0;
}

/* El compositor (el del SPE, aquí en escalar) contra el renderizador de referencia, píxel por píxel, en los dos motores.
 * Si los mapas de los fondos afines no cupieron en la arena (el compositor no los dibuja), la comparación no vale. */
static int check_vs_ref(void)
{
	static uint16_t refimg[NDS_SCREEN_W * NDS_SCREEN_H];
	int set, diffs = 0, first = -1, fe = 0;
	const gpu2d_frame *fr = nds_video_last_frame(&M, &set);

	if (!fr || M.vid.use_ref) {
		printf("REF: nada que comparar\n");
		return 0;
	}
	nds_video_render_host(&M);
	for (int e = 0; e < 2; e++) {
		gpu2d_ref_engine(&fr->eng[e], refimg);
		for (int i = 0; i < NDS_SCREEN_W * NDS_SCREEN_H; i++)
			if (refimg[i] != M.vid.img[e][i]) {
				if (first < 0) {
					first = i;
					fe = e;
				}
				diffs++;
			}
	}
	if (diffs)
		printf("REF: FALLO, %d píxeles distintos (primero en motor %c %d,%d)\n", diffs, 'A' + fe, first % NDS_SCREEN_W,
		       first / NDS_SCREEN_W);
	else
		printf("REF: IGUAL\n");
	return diffs != 0;
}

/* Si el juego escribió en su memoria de guardado y hay archivo, lo deja actualizado. */
static void save_out(const char *path)
{
	size_t len;
	const uint8_t *d;
	FILE *f;

	if (!path || !nds_save_take_dirty(&M))
		return;
	d = nds_save_data(&M, &len);
	f = fopen(path, "wb");
	if (f) {
		fwrite(d, 1, len, f);
		fclose(f);
		fprintf(stderr, "núcleo: guardado escrito en %s (%zu bytes)\n", path, len);
	}
}

/* Registros que deciden la imagen, en el mismo formato que `tools/melonds-run.py --regs` (para compararlos). */
static void print_regs(void)
{
	printf("regs DISPCNT_A=%08x DISPCNT_B=%08x BRIGHT_A=%04x BRIGHT_B=%04x POWCNT1=%04x\n", nds_peek32(&M, 0, 0x04000000),
	       nds_peek32(&M, 0, 0x04001000), nds_peek32(&M, 0, 0x0400006C) & 0xFFFF, nds_peek32(&M, 0, 0x0400106C) & 0xFFFF,
	       nds_peek32(&M, 0, 0x04000304) & 0xFFFF);
	printf("regs VRAMCNT=");
	for (uint32_t a = 0x04000240; a <= 0x04000249; a++)
		printf("%02x", nds_peek8(&M, 0, a));
	printf("\n");
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
	uint64_t trace_n = 0, frames = 0, skip_frames = 0, hist_n = 0, ring_n = 0;
	uint32_t break_pc = 0, dbg = 0, watch_addr = 0, save_size = 0;
	const char *save_path = NULL;
	struct { uint32_t addr, len; const char *file; } dumps[8];
	uint32_t ndumps = 0;
	int watch_cpu = 0;
	uint64_t watch_max = 0;
	int break_cpu = -1;
	uint32_t peek[8], npeek = 0;
	int show_regs = 0, use_ref = 0, vs_ref = 0, expect = 0, same_ab = 0, unmapped = 0;
	size_t len;
	uint8_t *rom;
	double t0, t1;
	int rc_gfx = 0;

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
		else if (!strcmp(argv[i], "--skip") && i + 1 < argc)
			skip_frames = strtoull(argv[++i], NULL, 0);
		else if ((!strcmp(argv[i], "--watch9") || !strcmp(argv[i], "--watch7")) && i + 1 < argc) {
			watch_cpu = argv[i][7] == '7';
			watch_addr = (uint32_t)strtoul(argv[++i], NULL, 0);
		} else if (!strcmp(argv[i], "--watch-max") && i + 1 < argc)
			watch_max = strtoull(argv[++i], NULL, 0);
		else if (!strcmp(argv[i], "--dump") && i + 1 < argc && ndumps < 8) {
			/* DIR:LONG:ARCHIVO — al terminar, vuelca esa memoria tal como la ve el ARM9 */
			char *a = argv[++i], *b = strchr(a, ':'), *c = b ? strchr(b + 1, ':') : NULL;

			if (!c) {
				fprintf(stderr, "--dump DIR:LONG:ARCHIVO\n");
				return 2;
			}
			*b = *c = 0;
			dumps[ndumps].addr = (uint32_t)strtoul(a, NULL, 0);
			dumps[ndumps].len = (uint32_t)strtoul(b + 1, NULL, 0);
			dumps[ndumps++].file = c + 1;
		} else if (!strcmp(argv[i], "--save") && i + 1 < argc)
			save_path = argv[++i];
		else if (!strcmp(argv[i], "--save-size") && i + 1 < argc)
			save_size = (uint32_t)strtoul(argv[++i], NULL, 0);
		else if (!strcmp(argv[i], "--dbg") && i + 1 < argc)
			dbg = (uint32_t)strtoul(argv[++i], NULL, 0);
		else if (!strcmp(argv[i], "--hist") && i + 1 < argc)
			hist_n = strtoull(argv[++i], NULL, 0);
		else if ((!strcmp(argv[i], "--break9") || !strcmp(argv[i], "--break7")) && i + 1 < argc) {
			break_cpu = argv[i][7] == '7';
			break_pc = (uint32_t)strtoul(argv[++i], NULL, 0);
		} else if (!strcmp(argv[i], "--ring") && i + 1 < argc)
			ring_n = strtoull(argv[++i], NULL, 0);
		else if (!strcmp(argv[i], "--screens") && i + 1 < argc)
			screens_path = argv[++i];
		else if (!strcmp(argv[i], "--regs"))
			show_regs = 1;
		else if (!strcmp(argv[i], "--ref"))
			use_ref = 1;
		else if (!strcmp(argv[i], "--vs-ref"))
			vs_ref = 1;
		else if (!strcmp(argv[i], "--expect"))
			expect = 1;
		else if (!strcmp(argv[i], "--same-ab"))
			same_ab = 1;
		else if (!strcmp(argv[i], "--unmapped"))
			unmapped = 1;
		else if (!strcmp(argv[i], "--peek") && i + 1 < argc && npeek < 8)
			peek[npeek++] = strtoul(argv[++i], NULL, 0);
		else
			rom_path = argv[i];
	}
	if (!rom_path) {
		fprintf(stderr, "uso: salsads-cpurun [--cycles N] [--frames N] [--quantum N] [--stats] [--arm7] "
				"[--keys HEX] [--touch X,Y] [--screens FILE.ppm] [--ref] [--vs-ref] [--expect] [--same-ab] [--unmapped] [--peek ADDR] [--save FILE | --save-size N] ROM.nds\n");
		return 2;
	}
	rom = slurp(rom_path, &len);
	if (!rom) {
		fprintf(stderr, "no puedo leer %s\n", rom_path);
		return 2;
	}
	M.log = log_to_stderr;
	M.quantum = quantum;
	M.dbg = dbg;
	M.vid.use_ref = use_ref;
	nds_init(&M);
	if (nds_load_rom(&M, rom, len) || nds_boot_direct(&M)) {
		fprintf(stderr, "ROM inválida\n");
		return 2;
	}
	if (save_path) {
		size_t sl;
		uint8_t *sd = slurp(save_path, &sl);

		if (sd && nds_save_attach(&M, sd, sl) == 0)
			fprintf(stderr, "núcleo: guardado de %s: %zu bytes\n", save_path, sl);
		else if (sd)
			fprintf(stderr, "núcleo: %s no tiene el tamaño de ningún chip (%zu bytes): se ignora\n", save_path, sl);
		free(sd);
	} else if (save_size) {
		uint8_t *blank = malloc(save_size);

		if (blank) {
			memset(blank, 0xFF, save_size);
			if (nds_save_attach(&M, blank, save_size))
				fprintf(stderr, "--save-size %u no es el tamaño de ningún chip\n", save_size);
			free(blank);
		}
	}
	if (keys)
		nds_set_input(&M, keys, tx, ty);
	for (uint64_t f = 0; f < skip_frames; f++)
		nds_run_frame(&M);
	if (watch_addr) {
		/* paso a paso (1 ciclo del ARM9): imprime cada cambio de la palabra en watch_addr, vista por watch_cpu */
		uint32_t old = nds_peek32(&M, watch_cpu, watch_addr);
		uint64_t steps = 0, changes = 0;

		printf("vigilo 0x%08x (ARM%d): valor inicial %08x\n", watch_addr, watch_cpu ? 7 : 9, old);
		while (steps < max_cycles && changes < (watch_max ? watch_max : 50)) {
			const uint32_t v = nds_peek32(&M, watch_cpu, watch_addr);

			if (v != old) {
				printf("t=%llu cuadro %llu: %08x -> %08x  (ARM9 pc %08x lr %08x, ARM7 pc %08x lr %08x)\n",
				       (unsigned long long)M.time, (unsigned long long)M.frames, old, v, acore_pc(&M.a9), M.a9.r[14],
				       acore_pc(&M.a7), M.a7.r[14]);
				old = v;
				changes++;
			}
			nds_run(&M, 1);
			steps++;
		}
		return 0;
	}
	if (break_cpu >= 0) {
		/* corre de a un ciclo hasta que la CPU llegue a break_pc; luego imprime las últimas ring_n instrucciones
		 * de esa CPU (para ver cómo llegó) */
		struct acore *c = break_cpu ? &M.a7 : &M.a9;
		struct ent { uint32_t pc, insn, cpsr, r0, r1, r2, lr; } *ring;
		const uint64_t rn = ring_n ? ring_n : 256;
		uint64_t pos = 0, last_ic = c->icount, steps = 0;

		ring = calloc(rn, sizeof *ring);
		while (steps < max_cycles) {
			const uint32_t pc = acore_pc(c);

			if (c->icount != last_ic) {
				last_ic = c->icount;
			}
			if (pc == break_pc && !c->halted)
				break;
			{
				struct ent *e = &ring[pos++ % rn];
				const uint32_t insn = nds_peek32(&M, break_cpu, pc & ~3u);

				e->pc = pc;
				e->insn = (c->cpsr & ACORE_T) ? (insn >> ((pc & 2) * 8)) & 0xFFFF : insn;
				e->cpsr = c->cpsr;
				e->r0 = c->r[0];
				e->r1 = c->r[1];
				e->r2 = c->r[2];
				e->lr = c->r[14];
			}
			{
				const uint64_t ic = c->icount;

				do {
					nds_run(&M, 1);
					steps++;
				} while (c->icount == ic && steps < max_cycles && !c->halted);
				if (c->halted)
					nds_run(&M, 2);
			}
		}
		printf("llegó a %08x: ARM9 pc %08x, ARM7 pc %08x, cuadros %llu, ciclos %llu\n", break_pc, acore_pc(&M.a9),
		       acore_pc(&M.a7), (unsigned long long)M.frames, (unsigned long long)M.time);
		printf("pila (sp=%08x):", c->r[13]);
		for (uint32_t k = 0; k < 64; k++) {
			if (k % 8 == 0)
				printf("\n  %08x:", c->r[13] + 4 * k);
			printf(" %08x", nds_peek32(&M, break_cpu, c->r[13] + 4 * k));
		}
		printf("\nregistros:");
		for (int k = 0; k < 16; k++)
			printf(" r%d=%08x", k, c->r[k]);
		printf("\n");
		for (uint64_t i = pos > rn ? pos - rn : 0; i < pos; i++) {
			const struct ent *e = &ring[i % rn];

			printf("%08x: %08x  cpsr=%08x r0=%08x r1=%08x r2=%08x lr=%08x\n", e->pc, e->insn, e->cpsr, e->r0, e->r1, e->r2, e->lr);
		}
		return 0;
	}
	if (hist_n) {
		/* histograma de PC de las dos CPUs durante hist_n pasos de 1 ciclo del ARM9 (después de --skip cuadros) */
		struct { uint32_t pc; uint32_t n; } h[2][64];
		uint32_t nh[2] = { 0, 0 };

		for (uint64_t i = 0; i < hist_n; i++) {
			for (int c = 0; c < 2; c++) {
				const uint32_t pc = acore_pc(c ? &M.a7 : &M.a9);
				uint32_t k;

				for (k = 0; k < nh[c] && h[c][k].pc != pc; k++)
					;
				if (k == nh[c] && nh[c] < 64)
					h[c][nh[c]++].pc = pc, h[c][k].n = 0;
				if (k < nh[c])
					h[c][k].n++;
			}
			nds_run(&M, 2);
		}
		for (int c = 0; c < 2; c++) {
			printf("%s: %u PC distintos (máx 64)\n", c ? "ARM7" : "ARM9", nh[c]);
			for (uint32_t k = 0; k < nh[c]; k++)
				if (h[c][k].n * 50 >= hist_n)
					printf("  pc %08x  %5.1f %%\n", h[c][k].pc, 100.0 * h[c][k].n / (double)hist_n);
		}
		return 0;
	}
	if (trace_n) {
		/* traza de instrucciones de una CPU (la otra avanza a su ritmo) */
		struct acore *c = trace_cpu ? &M.a7 : &M.a9;

		for (uint64_t i = 0; i < trace_n; i++) {
			const uint32_t pc = acore_pc(c);
			const uint32_t insn = nds_peek32(&M, trace_cpu, pc & ~3u);

			printf("%08x: %08x  cpsr=%08x r0=%08x r1=%08x r2=%08x r3=%08x r4=%08x r5=%08x r6=%08x r7=%08x sp=%08x lr=%08x\n", pc,
			       (c->cpsr & ACORE_T) ? (insn >> ((pc & 2) * 8)) & 0xFFFF : insn, c->cpsr, c->r[0], c->r[1],
			       c->r[2], c->r[3], c->r[4], c->r[5], c->r[6], c->r[7], c->r[13], c->r[14]);
			nds_run(&M, 1);
		}
		return 0;
	}
	t0 = now();
	if (frames) {
		const char *who;
		uint32_t pc;

		for (uint64_t f = 0; f < frames; f++) {
			nds_run_frame(&M);
			if (stuck_in_vector(&M, &who, &pc)) {
				printf("EXCEPCIÓN %s sin manejador en el cuadro %llu: pc = 0x%08x (ARM9 pc 0x%08x, ARM7 pc 0x%08x)\n", who,
				       (unsigned long long)f, pc, acore_pc(&M.a9), acore_pc(&M.a7));
				break;
			}
		}
		t1 = now();
		printf("fin: ARM9 pc 0x%08x cpsr 0x%08x%s, ARM7 pc 0x%08x cpsr 0x%08x%s; cuadros %llu\n", acore_pc(&M.a9), M.a9.cpsr,
		       M.a9.halted ? " (en HALT)" : "", acore_pc(&M.a7), M.a7.cpsr, M.a7.halted ? " (en HALT)" : "",
		       (unsigned long long)M.frames);
		if (stats)
			printf("estadísticas: %llu cuadros, ARM9 %llu instr., ARM7 %llu instr., %.3f s, %.1f MIPS\n",
			       (unsigned long long)frames, (unsigned long long)M.a9.icount, (unsigned long long)M.a7.icount,
			       t1 - t0, (double)(M.a9.icount + M.a7.icount) / (t1 - t0) / 1e6);
		if (stats)
			printf("planificador: %.1f tramos, %.1f saltos ociosos y %.1f eventos por cuadro; %.1f us por cuadro\n",
			       (double)M.st.slices / (double)frames, (double)M.st.jumps / (double)frames,
			       (double)M.st.events / (double)frames, (t1 - t0) * 1e6 / (double)frames);
		if (screens_path)
			save_ppm(screens_path);
		if (unmapped) {
			static uint16_t tmp[NDS_SCREEN_W * NDS_SCREEN_H];
			int set;
			const gpu2d_frame *fr = nds_video_last_frame(&M, &set);

			gpu2d_ref_unmapped_reads = 0;
			if (fr)
				gpu2d_ref_engine(&fr->eng[0], tmp);
			printf("VRAM sin banco: %u lecturas del motor A\n", gpu2d_ref_unmapped_reads);
		}
		if (expect && check_expect())
			rc_gfx = 1;
		if (same_ab && check_same_ab())
			rc_gfx = 1;
		if (vs_ref && check_vs_ref())
			rc_gfx = 1;
		for (uint32_t k = 0; k < npeek; k++)
			printf("peek 0x%08x = 0x%08x\n", peek[k], nds_peek32(&M, 0, peek[k]));
		if (show_regs)
			print_regs();
		save_out(save_path);
		for (uint32_t k = 0; k < ndumps; k++) {
			FILE *f = fopen(dumps[k].file, "wb");

			if (!f)
				continue;
			for (uint32_t o = 0; o < dumps[k].len; o++)
				fputc((int)nds_peek8(&M, 0, dumps[k].addr + o), f);
			fclose(f);
		}
		return rc_gfx;
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
		save_out(save_path);
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
		if (unmapped) {
			static uint16_t tmp[NDS_SCREEN_W * NDS_SCREEN_H];
			int set;
			const gpu2d_frame *fr = nds_video_last_frame(&M, &set);

			gpu2d_ref_unmapped_reads = 0;
			if (fr)
				gpu2d_ref_engine(&fr->eng[0], tmp);
			printf("VRAM sin banco: %u lecturas del motor A\n", gpu2d_ref_unmapped_reads);
		}
		if (expect && check_expect())
			rc_gfx = 1;
		if (same_ab && check_same_ab())
			rc_gfx = 1;
		if (vs_ref && check_vs_ref())
			rc_gfx = 1;
		for (uint32_t k = 0; k < npeek; k++)
			printf("peek 0x%08x = 0x%08x\n", peek[k], nds_peek32(&M, 0, peek[k]));
		if (show_regs)
			print_regs();
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
			printf("SIN VEREDICTO %s (ARM9 pc 0x%08x, ARM7 pc 0x%08x, marca %u)\n", name, acore_pc(&M.a9),
			       acore_pc(&M.a7), nds_peek32(&M, 0, SDS_ADDR + 0x30));
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
