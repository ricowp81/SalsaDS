/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * SalsaDS — lector del perfil del recompilador (salsads-cpurun --jit --jit-prof PREFIJO, en qemu-ppc64).
 *
 * Lee PREFIJO.blk (cada bloque traducido: instrucciones y palabras calientes por instrucción) y PREFIJO.tr (la traza:
 * la entrada de cada bloque que corrió, marcas de tramo y de cuadro) y cuenta, para un tramo de cuadros, lo que el
 * PPE tendría que ejecutar y captar: palabras calientes por instrucción guest y por clase, huella de código por
 * cuadro, y fallos de una L1 de instrucciones (32 KB, 2 vías, líneas de 128 B) y de una L2 (512 KB, 8 vías) que
 * siguen la traza. Es un modelo (sin datos, sin saltos mal predichos, cada bloque se cuenta entero): sirve para ordenar
 * cambios en el emisor, no para dar milisegundos de la PS3.
 *
 *   cc -O2 -o build/jit-perfil tools/jit-perfil.c
 *   build/jit-perfil PREFIJO [--from F] [--to F] [--series N] [--top N] [--ckb KB] [--dkb KB] [--cpu 0|1|2] [--packed]
 *
 * --series N: una línea cada N cuadros (para elegir escenas). --ckb: código en C que corre en cada tramo (planificador,
 * eventos; ensucia la L1 y ocupa la L2), 8 KB por omisión. --dkb: datos que pasan por la L2 en cada cuadro (memoria
 * emulada, otros hilos), 1024 KB por omisión. --cpu: 0 ARM9, 1 ARM7, 2 las dos (lo que se reporta; la caché siempre
 * ve las dos). --packed: las secciones calientes de los bloques juntas (en el orden en que se tradujeron), como si las
 * frías vivieran aparte; sirve para estimar qué daría separarlas. También cuenta fallos de una ERAT de instrucciones de 64
 * entradas de 4 KB.
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define ESC 0x003FFFFFu
#define MAXI 64
#define LINE 128u
#define CODE7_OFF (4u << 20)
#define C_OFF (64u << 20)
#define D_OFF (96u << 20)
#define NLINES ((128u << 20) / LINE)

enum { K_DP, K_MUL, K_MULL, K_CLZ, K_MEM, K_BLOCK, K_B, K_BX, K_BLLO, K_CALL, K_NOP, NK };
/* clases finas */
enum { C_PRO, C_END, C_DP, C_DPS, C_MUL, C_LD, C_ST, C_LDM, C_STM, C_B, C_BX, C_CALL, C_OTHER, C_COND, NC };
static const char *const cname[NC] = { "prólogo", "salida final", "datos", "datos con S", "MUL", "LDR*", "STR*",
				       "LDM/POP", "STM/PUSH", "B/BL", "BX/BLX", "intérprete", "otra", "(condicional)" };

struct blk {
	uint32_t key, pc;
	uint16_t n, hot, words, prof;
	uint8_t cls[MAXI], cond[MAXI];
	uint16_t w[MAXI];
	uint32_t raw[MAXI];
	uint64_t execs;
	uint64_t l1m, l2m;
	uint32_t packed; /* --packed: dirección (palabras) si las secciones calientes estuvieran juntas, sin las frías */
};

static struct blk *blks;
static uint32_t nblks, capblks;
static uint32_t *ht;
static uint32_t pack_next[2];
static uint32_t htmask;

static uint32_t hfun(uint32_t k)
{
	k ^= k >> 16;
	k *= 0x7feb352du;
	k ^= k >> 15;
	return k;
}

static void ht_put(uint32_t key, uint32_t idx)
{
	uint32_t h = hfun(key) & htmask;

	while (ht[h] != 0xFFFFFFFFu && blks[ht[h]].key != key)
		h = (h + 1) & htmask;
	ht[h] = idx; /* un bloque con la misma entrada (tras un vaciado sin cambio de generación) reemplaza al viejo */
}

static int ht_get(uint32_t key)
{
	uint32_t h = hfun(key) & htmask;

	while (ht[h] != 0xFFFFFFFFu) {
		if (blks[ht[h]].key == key)
			return (int)ht[h];
		h = (h + 1) & htmask;
	}
	return -1;
}

static unsigned classify(uint32_t raw, unsigned thumb, unsigned kind)
{
	switch (kind) {
	case K_DP:
		if (!thumb)
			return (raw >> 20 & 1) ? C_DPS : C_DP;
		if ((raw >> 10) == 0x11) /* hi reg: solo CMP fija banderas */
			return ((raw >> 8) & 3) == 1 ? C_DPS : C_DP;
		if ((raw >> 12) == 0xA || (raw >> 8) == 0xB0) /* ADD rd, pc/sp; ADD sp */
			return C_DP;
		return C_DPS;
	case K_MUL:
	case K_MULL:
	case K_CLZ:
		return C_MUL;
	case K_MEM:
		if (!thumb)
			return (raw >> 20 & 1) ? C_LD : C_ST;
		if ((raw >> 11) == 0x9) /* LDR pc */
			return C_LD;
		if ((raw >> 12) == 0x5)
			return ((raw >> 9) & 7) >= 3 ? C_LD : C_ST;
		return (raw >> 11 & 1) ? C_LD : C_ST;
	case K_BLOCK:
		if (!thumb)
			return (raw >> 20 & 1) ? C_LDM : C_STM;
		if ((raw >> 12) == 0xB)
			return (raw >> 11 & 1) ? C_LDM : C_STM; /* POP / PUSH */
		return (raw >> 11 & 1) ? C_LDM : C_STM;
	case K_B:
		return C_B;
	case K_BX:
	case K_BLLO:
		return C_BX;
	case K_CALL:
		return C_CALL;
	default:
		return C_OTHER;
	}
}

static void load_blk(const char *path)
{
	FILE *f = fopen(path, "r");
	char *line = NULL;
	size_t cap = 0;

	if (!f) {
		perror(path);
		exit(1);
	}
	capblks = 1 << 16;
	blks = calloc(capblks, sizeof *blks);
	htmask = (1u << 20) - 1;
	ht = malloc((htmask + 1) * 4);
	memset(ht, 0xFF, (htmask + 1) * 4);
	while (getline(&line, &cap, f) > 0) {
		unsigned cpu, gen, entry, pc, n, hot, words, prof;
		int off;
		char *p;
		struct blk *b;

		if (sscanf(line, "B %u %u %u %x %u %u %u %u%n", &cpu, &gen, &entry, &pc, &n, &hot, &words, &prof, &off) != 8)
			continue;
		if (nblks == capblks) {
			capblks *= 2;
			blks = realloc(blks, capblks * sizeof *blks);
		}
		b = &blks[nblks];
		memset(b, 0, sizeof *b);
		b->key = cpu << 31 | (gen & 0xFFu) << 22 | (entry & 0x3FFFFFu);
		b->pc = pc;
		b->n = (uint16_t)n;
		b->hot = (uint16_t)hot;
		b->words = (uint16_t)words;
		b->prof = (uint16_t)prof;
		b->packed = pack_next[cpu & 1];
		pack_next[cpu & 1] = (pack_next[cpu & 1] + (hot - prof)) % (1u << 20);
		p = line + off;
		for (unsigned i = 0; i < n && i < MAXI; i++) {
			unsigned raw, tk, cond, w;
			int o2;

			if (sscanf(p, " %x:%x:%x:%u%n", &raw, &tk, &cond, &w, &o2) != 4)
				break;
			p += o2;
			b->raw[i] = raw;
			b->cls[i] = (uint8_t)classify(raw, tk & 1, tk >> 1);
			b->cond[i] = (uint8_t)cond;
			b->w[i] = (uint16_t)w;
		}
		ht_put(b->key, nblks);
		nblks++;
	}
	free(line);
	fclose(f);
}

/* ---- cachés (LRU) ---- */

struct cache {
	unsigned sets, ways;
	uint32_t *tag;
	uint32_t *age;
	uint32_t clock;
	uint64_t miss;
};

static void cache_init(struct cache *c, unsigned bytes, unsigned ways)
{
	c->ways = ways;
	c->sets = bytes / LINE / ways;
	c->tag = malloc(c->sets * ways * 4);
	c->age = calloc(c->sets * ways, 4);
	memset(c->tag, 0xFF, c->sets * ways * 4);
}

/* 1 si acierta */
static int cache_touch(struct cache *c, uint32_t line)
{
	const unsigned s = line % c->sets;
	uint32_t *t = c->tag + s * c->ways, *a = c->age + s * c->ways;
	unsigned victim = 0;

	c->clock++;
	for (unsigned w = 0; w < c->ways; w++) {
		if (t[w] == line) {
			a[w] = c->clock;
			return 1;
		}
		if (a[w] < a[victim])
			victim = w;
	}
	t[victim] = line;
	a[victim] = c->clock;
	c->miss++;
	return 0;
}

static struct cache l1, l2;
static uint32_t *seen; /* por línea: último cuadro + 1 en que se tocó (huella por cuadro) */
static uint32_t frame_now;
static uint64_t foot_lines[2];
static uint64_t l1m_cpu[2], l2m_cpu[2], l1m_other, l2m_other;
static int in_range;

/* ERAT de instrucciones: 64 entradas de 4 KB (cada una cubre 4 KB aunque la página sea grande), LRU */
static uint32_t erat_tag[64], erat_age[64], erat_clock;
static uint64_t erat_miss[2];
static int packed_mode;

static void erat_touch(uint32_t page, int cpu)
{
	unsigned victim = 0;

	erat_clock++;
	for (unsigned w = 0; w < 64; w++) {
		if (erat_tag[w] == page + 1) {
			erat_age[w] = erat_clock;
			return;
		}
		if (erat_age[w] < erat_age[victim])
			victim = w;
	}
	erat_tag[victim] = page + 1;
	erat_age[victim] = erat_clock;
	if (cpu >= 0 && in_range)
		erat_miss[cpu]++;
}


static void touch_code(uint32_t line, int cpu, struct blk *b)
{
	const int h1 = cache_touch(&l1, line);

	if (!h1) {
		const int h2 = cache_touch(&l2, line);

		if (in_range) {
			l1m_cpu[cpu]++;
			if (b)
				b->l1m++;
			if (!h2) {
				l2m_cpu[cpu]++;
				if (b)
					b->l2m++;
			}
		}
	}
	if (in_range && seen[line] != frame_now + 1) {
		seen[line] = frame_now + 1;
		foot_lines[cpu]++;
	}
}

static void touch_other(uint32_t line, int l1too)
{
	if (l1too && cache_touch(&l1, line))
		return;
	if (!cache_touch(&l2, line) && in_range)
		l2m_other++;
	if (l1too && in_range)
		l1m_other++;
}

static uint64_t rng = 88172645463325252ull;
static uint32_t xr(void)
{
	rng ^= rng << 13;
	rng ^= rng >> 7;
	rng ^= rng << 17;
	return (uint32_t)rng;
}

int main(int argc, char **argv)
{
	const char *prefix = NULL;
	uint32_t from = 0, to = 0xFFFFFFFFu, series = 0, top = 25, ckb = 8, dkb = 1024;
	int cpusel = 0;
	char path[512];
	FILE *tr;
	static uint32_t buf[1 << 16];
	size_t nr;
	int esc = 0, cpu_cur = 0;
	uint32_t pend_mark = 0;
	/* acumulados del tramo */
	uint64_t execs[2] = { 0 }, ginsn[2] = { 0 }, hotw[2] = { 0 }, cls_w[2][NC] = { { 0 } }, cls_n[2][NC] = { { 0 } };
	uint64_t slices[2] = { 0 }, ic9_0 = 0, ic9_1 = 0, ic7_0 = 0, ic7_1 = 0, nframes = 0;
	uint32_t last_ic9 = 0, last_ic7 = 0;
	/* serie */
	uint64_t s_execs = 0, s_hot = 0, s_l1 = 0, s_l2 = 0, s_foot = 0, s_ic = 0, s_sl = 0;
	uint32_t s_n = 0;

	for (int i = 1; i < argc; i++) {
		if (!strcmp(argv[i], "--from") && i + 1 < argc)
			from = (uint32_t)strtoul(argv[++i], NULL, 0);
		else if (!strcmp(argv[i], "--to") && i + 1 < argc)
			to = (uint32_t)strtoul(argv[++i], NULL, 0);
		else if (!strcmp(argv[i], "--series") && i + 1 < argc)
			series = (uint32_t)strtoul(argv[++i], NULL, 0);
		else if (!strcmp(argv[i], "--top") && i + 1 < argc)
			top = (uint32_t)strtoul(argv[++i], NULL, 0);
		else if (!strcmp(argv[i], "--ckb") && i + 1 < argc)
			ckb = (uint32_t)strtoul(argv[++i], NULL, 0);
		else if (!strcmp(argv[i], "--dkb") && i + 1 < argc)
			dkb = (uint32_t)strtoul(argv[++i], NULL, 0);
		else if (!strcmp(argv[i], "--packed"))
			packed_mode = 1;
		else if (!strcmp(argv[i], "--cpu") && i + 1 < argc)
			cpusel = atoi(argv[++i]);
		else
			prefix = argv[i];
	}
	if (!prefix) {
		fprintf(stderr, "uso: jit-perfil PREFIJO [--from F] [--to F] [--series N] [--top N] [--ckb KB] [--dkb KB] [--cpu 0|1|2]\n");
		return 1;
	}
	snprintf(path, sizeof path, "%s.blk", prefix);
	load_blk(path);
	snprintf(path, sizeof path, "%s.tr", prefix);
	tr = fopen(path, "rb");
	if (!tr) {
		perror(path);
		return 1;
	}
	cache_init(&l1, 32 << 10, 2);
	cache_init(&l2, 512 << 10, 8);
	seen = calloc(NLINES, 4);
	in_range = from == 0;
	const uint32_t dlines_frame = dkb * 1024u / LINE;
	const uint32_t clines = ckb * 1024u / LINE;
	uint32_t dacc = 0;

	while ((nr = fread(buf, 4, sizeof buf / 4, tr)) > 0) {
		for (size_t k = 0; k < nr; k++) {
			const uint32_t v = __builtin_bswap32(buf[k]); /* la traza es big-endian (qemu-ppc64) */

			if (esc == 1) {
				pend_mark = v;
				esc = 2;
				continue;
			}
			if (esc == 2) {
				esc = 0;
				if ((pend_mark >> 28) == 0xA) { /* tramo: v = icount de esa CPU */
					cpu_cur = pend_mark & 1;
					if (in_range)
						slices[cpu_cur]++;
					/* el código en C del planificador, los eventos y el despachador */
					for (uint32_t q = 0; q < clines; q++)
						touch_other(C_OFF / LINE + q, 1);
					for (uint32_t q = 0; q < (clines * LINE + 4095u) / 4096u; q++)
						erat_touch((C_OFF >> 12) + q, -1);
					/* datos que pasan por la L2 (repartidos por tramo) */
					dacc += dlines_frame;
					while (dacc >= 1000u) { /* ~1000 tramos por cuadro */
						touch_other(D_OFF / LINE + (xr() % ((16u << 20) / LINE)), 0);
						dacc -= 1000u;
					}
				} else if ((pend_mark >> 24) == 0xF0) {
					const uint32_t f = pend_mark & 0xFFFFFFu;

					if (in_range) {
						ic9_1 = v;
						nframes++;
					}
					last_ic9 = v;
					frame_now = f + 1;
					if (series && f >= from && f < to) {
						s_n++;
						if (s_n == series) {
							printf("cuadros %6u-%6u: ARM9 %7.0f instr  %6.0f bloques  %5.2f palabras/instr  huella %4.0f KB  "
							       "L1 %6.0f  L2 %6.0f  tramos %5.0f\n",
							       f + 1 - series, f, (double)(v - (uint32_t)s_ic) / series, (double)s_execs / series,
							       s_execs ? (double)s_hot / (double)(v - (uint32_t)s_ic) : 0.0,
							       (double)s_foot * LINE / 1024.0 / series, (double)s_l1 / series, (double)s_l2 / series,
							       (double)s_sl / series);
							s_n = 0;
							s_execs = s_hot = s_l1 = s_l2 = s_foot = 0;
							s_sl = 0;
							s_ic = v;
						}
					} else {
						s_ic = v;
					}
					{
						const int was = in_range;

						in_range = frame_now >= from && frame_now < to;
						if (!was && in_range) {
							ic9_0 = v;
							ic7_0 = last_ic7;
						}
					}
				} else if ((pend_mark >> 24) == 0xF1) {
					if (in_range || frame_now == to)
						ic7_1 = v;
					last_ic7 = v;
				}
				continue;
			}
			if (v == ESC) {
				esc = 1;
				continue;
			}
			{
				const int idx = ht_get(v);
				const int cpu = (int)(v >> 31);
				struct blk *b;
				uint32_t base, l0, l1x;

				if (idx < 0)
					continue;
				b = &blks[idx];
				base = (cpu ? CODE7_OFF : 0u) + (packed_mode ? b->packed : (v & 0x3FFFFFu)) * 4u;
				for (uint32_t pg = base >> 12; pg <= (base + (uint32_t)(b->hot - b->prof) * 4u - 1u) >> 12; pg++)
					erat_touch(pg, cpu);
				l0 = base / LINE;
				l1x = (base + (uint32_t)(b->hot - b->prof) * 4u - 1u) / LINE;
				{
					const uint64_t m1 = l1m_cpu[cpu], m2 = l2m_cpu[cpu], ft = foot_lines[cpu];

					for (uint32_t L = l0; L <= l1x; L++)
						touch_code(L, cpu, in_range ? b : NULL);
					if (in_range && cpu == 0) {
						s_l1 += l1m_cpu[cpu] - m1;
						s_l2 += l2m_cpu[cpu] - m2;
						s_foot += foot_lines[cpu] - ft;
					}
				}
				if (!in_range)
					continue;
				b->execs++;
				execs[cpu]++;
				ginsn[cpu] += b->n;
				hotw[cpu] += (uint32_t)(b->hot - b->prof);
				if (cpu == 0) {
					s_execs++;
					s_hot += (uint32_t)(b->hot - b->prof);
				}
				{
					unsigned sum = 0;

					for (unsigned i = 0; i < b->n && i < MAXI; i++) {
						cls_w[cpu][b->cls[i]] += b->w[i];
						cls_n[cpu][b->cls[i]]++;
						if (b->cond[i] != 0xE) {
							cls_n[cpu][C_COND]++;
							cls_w[cpu][C_COND] += b->w[i];
						}
						sum += b->w[i];
					}
					/* prólogo (sin el perfil) y la salida del final: lo que no es de ninguna instrucción */
					{
						const unsigned pro_end = (unsigned)(b->hot - b->prof) - sum;

						cls_w[cpu][C_PRO] += 2; /* cmpdi + bc del presupuesto */
						cls_w[cpu][C_END] += pro_end >= 2 ? pro_end - 2 : 0;
					}
				}
			}
		}
	}
	fclose(tr);
	(void)ic7_0;
	(void)ic7_1;
	(void)last_ic9;
	if (series)
		return 0;

	printf("cuadros %u a %u (%llu cuadros)\n", from, to, (unsigned long long)nframes);
	for (int cpu = 0; cpu < 2; cpu++) {
		if (cpusel != 2 && cpusel != cpu)
			continue;
		const double nf = nframes ? (double)nframes : 1.0;
		const double gi = (double)ginsn[cpu];
		const double icd = cpu == 0 ? (double)(uint32_t)(ic9_1 - ic9_0) : gi;

		printf("\n== %s ==\n", cpu ? "ARM7" : "ARM9");
		printf("por cuadro: %.0f tramos, %.0f bloques corridos, %.0f instrucciones guest según los bloques (%.0f según el "
		       "contador: %.2f)\n",
		       slices[cpu] / nf, execs[cpu] / nf, gi / nf, icd / nf, icd > 0 ? gi / icd : 0.0);
		printf("palabras calientes corridas: %.0f por cuadro, %.2f por instrucción (del contador), %.1f por bloque, %.2f "
		       "instrucciones por bloque\n",
		       hotw[cpu] / nf, icd > 0 ? (double)hotw[cpu] / icd : 0.0, execs[cpu] ? (double)hotw[cpu] / execs[cpu] : 0.0,
		       execs[cpu] ? gi / execs[cpu] : 0.0);
		printf("huella de código por cuadro: %.0f KB (%.0f líneas)\n", foot_lines[cpu] * LINE / 1024.0 / nf,
		       foot_lines[cpu] / nf);
		printf("fallos por cuadro: L1I %.0f, L2 %.0f (código); modelo: %.2f ms = palabras %.2f + L1 %.2f (41 c) + L2 %.2f "
		       "(380 c) a 3,2 GHz\n",
		       l1m_cpu[cpu] / nf, l2m_cpu[cpu] / nf,
		       (hotw[cpu] * 1.0 + l1m_cpu[cpu] * 41.0 + l2m_cpu[cpu] * 380.0) / 3.2e6 / nf, hotw[cpu] / 3.2e6 / nf,
		       l1m_cpu[cpu] * 41.0 / 3.2e6 / nf, l2m_cpu[cpu] * 380.0 / 3.2e6 / nf);
		printf("fallos de la ERAT de instrucciones (64 × 4 KB) por cuadro: %.0f (~24 c: %.2f ms)\n", erat_miss[cpu] / nf,
		       erat_miss[cpu] * 24.0 / 3.2e6 / nf);
		printf("palabras por clase (por cuadro; palabras por instrucción de la clase; %% del total):\n");
		for (int c = 0; c < NC; c++) {
			if (!cls_w[cpu][c] && !cls_n[cpu][c])
				continue;
			printf("  %-14s %9.0f instr  %9.0f palabras  %5.2f  %5.1f %%\n", cname[c], cls_n[cpu][c] / nf, cls_w[cpu][c] / nf,
			       cls_n[cpu][c] ? (double)cls_w[cpu][c] / cls_n[cpu][c] : 0.0,
			       hotw[cpu] ? 100.0 * cls_w[cpu][c] / hotw[cpu] : 0.0);
		}
	}
	printf("\notros: L1I %.0f, L2 %.0f fallos por cuadro (C y datos del modelo)\n", l1m_other / (double)(nframes ? nframes : 1),
	       l2m_other / (double)(nframes ? nframes : 1));
	if (top) {
		/* bloques más caros según el modelo */
		uint32_t *ord = malloc(nblks * 4);
		uint32_t m = 0;

		for (uint32_t i = 0; i < nblks; i++)
			if (blks[i].execs && (cpusel == 2 || (int)(blks[i].key >> 31) == cpusel))
				ord[m++] = i;
		for (uint32_t i = 0; i < m && i < top; i++) {
			uint32_t best = i;

			for (uint32_t k = i + 1; k < m; k++) {
				const struct blk *a = &blks[ord[k]], *bb = &blks[ord[best]];
				const double ca = a->execs * (double)(a->hot - a->prof) + a->l1m * 41.0 + a->l2m * 380.0;
				const double cb = bb->execs * (double)(bb->hot - bb->prof) + bb->l1m * 41.0 + bb->l2m * 380.0;

				if (ca > cb)
					best = k;
			}
			{
				const uint32_t t = ord[i];

				ord[i] = ord[best];
				ord[best] = t;
			}
		}
		printf("\nbloques más caros (modelo): pc, instr, palabras calientes, corridas por cuadro, L1/L2 por cuadro, %% del modelo\n");
		double tot = 0;
		for (uint32_t i = 0; i < m; i++) {
			const struct blk *a = &blks[ord[i]];

			tot += a->execs * (double)(a->hot - a->prof) + a->l1m * 41.0 + a->l2m * 380.0;
		}
		for (uint32_t i = 0; i < m && i < top; i++) {
			const struct blk *a = &blks[ord[i]];
			const double nf = nframes ? (double)nframes : 1.0;
			const double ca = a->execs * (double)(a->hot - a->prof) + a->l1m * 41.0 + a->l2m * 380.0;

			printf("  %08x %3u %4u  %8.1f  %6.1f %6.1f  %5.2f %%\n", a->pc, a->n, a->hot - a->prof, a->execs / nf, a->l1m / nf,
			       a->l2m / nf, 100.0 * ca / tot);
		}
	}
	return 0;
}
