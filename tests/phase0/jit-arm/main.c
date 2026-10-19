/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * SalsaDS — Fase 0.7: prueba mínima del JIT ARM → PPC64 (PS3 real).
 *
 * (a) Cuántos MHz de ARM9 saca el código traducido en bucles reales (suma con
 *     banderas, saltos condicionales, cargas y guardados), contra el
 *     intérprete de la Fase 0.3 y contra la meta de 67 MHz.
 * (b) Cómo salen las banderas NZCV en el PPE: perezosas, CR por instrucción y
 *     cmp+salto fusionados, con el costo medido del microcódigo del PPE
 *     (docs/fase0.md §0.3) como guía: nada de formas con punto ni mfcr.
 * (c) El ARM7 en el hilo 1: dos hilos a la vez y cuánto le quita al ARM9 que el
 *     del ARM7 espere girando o durmiendo con una primitiva de lv2, y la
 *     latencia de despertarlo (threads.c).
 *
 * El JIT escribe en una cola NOBITS al final del segmento de texto (mk/rec-cache.ld):
 * con HEN es lo único ejecutable. Antes de usarla, un hilo de prueba la
 * escribe y la ejecuta; si falla o no vuelve (CFW), el programa sigue con los
 * intérpretes (el respaldo). En RPCS3 el texto es de solo lectura: con el
 * archivo de prueba automática el JIT no se intenta y solo se mide lo demás.
 * Archivos en /dev_hdd0/salsads: jit-arm-nojit (fuerza el respaldo) y
 * jit-arm.marker (la marca de la prueba de la caché; ver rec_cache.h).
 *
 * Todo número sale del timebase (79,8 MHz) y se repite 5 veces (mediana y
 * mínimo). Antes de medir, cada traducción se verifica contra el intérprete
 * de referencia (registros, memoria y número de instrucciones); si algo
 * difiere, el resultado es FALLO.
 */
#include <ppu-types.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/process.h>
#include <sys/stat.h>
#include <sys/systime.h>

#include "fixed.h"
#include "interp.h"
#include "jit/jit_arm.h"
#include "platform/ps3/autotest.h"
#include "platform/ps3/log.h"
#include "rec_cache.h"
#include "threads.h"

#ifndef JIT_VERSION
#define JIT_VERSION "desconocida"
#endif
#define JIT_TITLE "SalsaDS JIT"
#define JIT_APPID "SLDS00007"
#define NOJIT_FLAG SALSADS_DATA_DIR "/jit-arm-nojit"
#define MARKER SALSADS_DATA_DIR "/jit-arm.marker"

SYS_PROCESS_PARAM(1001, 0x40000);

#define CPU_HZ 3200000000.0
#define ARM9_MHZ 67.0
#define REPS 5

static double tb_hz;
static int failures;
static double min_ms = 60;

static inline uint64_t tb_now(void)
{
	uint64_t t;
	__asm__ volatile("mftb %0" : "=r"(t));
	return t;
}

static void fail(const char *what)
{
	failures++;
	salsads_log("FALLO %s", what);
}

/* ---- Estado compartido ----------------------------------------------------- */

static uint8_t gmem[JA_MEM_SIZE] __attribute__((aligned(128)));
static uint8_t gmem_ref[JA_MEM_SIZE] __attribute__((aligned(128)));
static uint8_t mem03[ARM_MEM_SIZE] __attribute__((aligned(128)));

#define CODE_SLOT_WORDS 2048
static uint32_t *jit_code[8][JA_FLAGS_COUNT];
static int jit_on;

enum impl { IMP_REF, IMP_O3, IMP_LAZY, IMP_CR, IMP_FUSED, IMP_COUNT };
static const char *const impl_name[IMP_COUNT] = {
	"intérprete de referencia (switch, banderas completas)",
	"intérprete de la 0.3 (goto calculado)",
	"JIT, banderas perezosas",
	"JIT, CR por instrucción",
	"JIT, cmp+salto fusionados",
};

/* Instrucciones y saltos tomados por pasada de cada bucle, del intérprete de referencia. */
static struct {
	double per_rep, fixed, taken_per_rep;
} lp[8];

/* ---- Ejecutar un bucle con una implementación ----------------------------------- */

/* Devuelve 0 si salió por el SWI. */
static int run_impl(enum impl im, unsigned loop, uint32_t reps, struct ja_state *s)
{
	if (im == IMP_REF) {
		ja_loop_setup(s, gmem_ref, reps);
		ja_ref_run(s, ja_loops[loop].code, ja_loops[loop].nwords);
		return s->ret == JA_EXIT_SWI ? 0 : -1;
	}
	if (im == IMP_O3) {
		struct arm_cpu c;
		arm_load_program(&c, mem03, reps);
		arm_run(&c, DISPATCH_GOTO);
		memset(s, 0, sizeof *s);
		for (int i = 0; i < 16; i++)
			s->r[i] = c.r[i];
		return c.halt == 1 ? 0 : -1;
	}
	ja_loop_setup(s, gmem, reps);
	return ja_call(jit_code[loop][im - IMP_LAZY], s) == JA_EXIT_SWI ? 0 : -1;
}

static int impl_available(enum impl im, unsigned loop)
{
	if (im == IMP_O3)
		return ja_loops[loop].in_interp03;
	if (im >= IMP_LAZY)
		return jit_on && jit_code[loop][im - IMP_LAZY] != NULL;
	return 1;
}

/* Segundos de una corrida de `reps` pasadas. */
static double time_run(enum impl im, unsigned loop, uint32_t reps)
{
	struct ja_state s;
	const uint64_t t0 = tb_now();
	if (run_impl(im, loop, reps, &s) != 0)
		return -1;
	return (double)(tb_now() - t0) / tb_hz;
}

static int cmp_dbl(const void *a, const void *b)
{
	const double x = *(const double *)a, y = *(const double *)b;
	return x < y ? -1 : x > y;
}

/* Calibra las pasadas hasta ≥ min_ms y mide REPS veces; devuelve segundos por pasada (mediana, mínimo). */
static int measure(enum impl im, unsigned loop, double *med, double *mn)
{
	uint32_t reps = 1;
	double t;
	for (;;) {
		t = time_run(im, loop, reps);
		if (t < 0)
			return -1;
		if (t * 1000.0 >= min_ms || reps > (1u << 24))
			break;
		reps *= 2;
	}
	double v[REPS];
	for (int i = 0; i < REPS; i++) {
		t = time_run(im, loop, reps);
		if (t < 0)
			return -1;
		v[i] = t / reps;
	}
	qsort(v, REPS, sizeof v[0], cmp_dbl);
	*med = v[REPS / 2];
	*mn = v[0];
	return 0;
}

/* ---- Verificación ----------------------------------------------------------------- */

static int verify(unsigned loop, enum impl im)
{
	struct ja_state r, s;
	struct ja_expect e;
	ja_loop_expect(loop, 3, &e);
	run_impl(IMP_REF, loop, 3, &r);
	if (r.r[0] != e.r0 || r.r[7] != e.r7 || (loop >= 2 && r.r[4] != e.r4) || ja_out_sum(gmem_ref) != e.out_sum) {
		fail("el intérprete de referencia no da lo esperado");
		return -1;
	}
	if (im == IMP_REF)
		return 0;
	if (run_impl(im, loop, 3, &s) != 0) {
		fail("la implementación no terminó en el SWI");
		return -1;
	}
	if (im == IMP_O3)
		return (s.r[0] == r.r[0] && (loop != 0 || s.r[4] == r.r[4])) ? 0 : (fail("el intérprete de la 0.3 difiere"), -1);
	for (int i = 0; i <= 12; i++) {
		if (s.r[i] != r.r[i]) {
			salsads_log("FALLO %s, %s: r%d = 0x%08x, esperado 0x%08x", ja_loops[loop].name, impl_name[im], i, s.r[i],
				    r.r[i]);
			failures++;
			return -1;
		}
	}
	if (memcmp(gmem, gmem_ref, JA_MEM_SIZE) != 0 || s.budget != r.budget) {
		salsads_log("FALLO %s, %s: la memoria o el número de instrucciones difieren", ja_loops[loop].name, impl_name[im]);
		failures++;
		return -1;
	}
	return 0;
}

/* ---- Traducción ---------------------------------------------------------------------- */

static void translate_all(void)
{
	uint32_t *cache = salsads_rec_cache();
	size_t slot = 0;
	for (unsigned l = 0; l < ja_nloops; l++) {
		for (int m = 0; m < JA_FLAGS_COUNT; m++) {
			uint32_t *code = cache + slot * CODE_SLOT_WORDS;
			size_t n = 0;
			int rc = ja_translate(ja_loops[l].code, ja_loops[l].nwords, (enum ja_flagmode)m, code, CODE_SLOT_WORDS, &n);
			if (rc != 0) {
				salsads_log("FALLO ja_translate(%s, %s) = %d", ja_loops[l].name, ja_flagmode_name[m], rc);
				failures++;
				slot++;
				continue;
			}
			const struct ja_tr_stats st = ja_last_tr;
			/* Costo de traducir y de vaciar la caché de instrucciones: mediana de 20. */
			double tr[20], fl[20];
			for (int i = 0; i < 20; i++) {
				const uint64_t a = tb_now();
				ja_translate(ja_loops[l].code, ja_loops[l].nwords, (enum ja_flagmode)m, code, CODE_SLOT_WORDS, &n);
				const uint64_t b = tb_now();
				salsads_rec_flush(code, n * 4);
				const uint64_t c = tb_now();
				tr[i] = (double)(b - a) * 1e6 / tb_hz;
				fl[i] = (double)(c - b) * 1e6 / tb_hz;
			}
			qsort(tr, 20, sizeof tr[0], cmp_dbl);
			qsort(fl, 20, sizeof fl[0], cmp_dbl);
			jit_code[l][m] = code;
			salsads_log("DATO jit-traducción %-7s %-39s: %u instr. guest → %u del PPE (×%u.%02u), %u bloques, %u cmp+salto "
				    "fusionados; traducir %u.%02u µs, vaciar la caché de instrucciones %u.%02u µs",
				    ja_loops[l].name, ja_flagmode_name[m], st.guest_instr, st.host_instr,
				    F2((double)st.host_instr / (double)st.guest_instr), st.blocks, st.fused, F2(tr[10]), F2(fl[10]));
			slot++;
		}
	}
}

/* ---- Medición de (a) y (b) ---------------------------------------------------------- */

static double mips_tab[8][IMP_COUNT];

static void bench_loops(void)
{
	for (unsigned l = 0; l < ja_nloops; l++) {
		struct ja_state s1, s2;
		run_impl(IMP_REF, l, 1, &s1);
		const double i1 = (double)(INT64_MAX / 2 - s1.budget), t1 = (double)s1.taken;
		run_impl(IMP_REF, l, 2, &s2);
		const double i2 = (double)(INT64_MAX / 2 - s2.budget), t2 = (double)s2.taken;
		lp[l].per_rep = i2 - i1;
		lp[l].fixed = i1 - lp[l].per_rep;
		lp[l].taken_per_rep = t2 - t1;
		salsads_log("bucle %s (%s): %u instr. guest por pasada, %u saltos tomados por pasada", ja_loops[l].name,
			    ja_loops[l].what, (unsigned)lp[l].per_rep, (unsigned)lp[l].taken_per_rep);
		for (int im = 0; im < IMP_COUNT; im++) {
			if (!impl_available((enum impl)im, l))
				continue;
			if (verify(l, (enum impl)im) != 0)
				continue;
			double med, mn;
			if (measure((enum impl)im, l, &med, &mn) != 0) {
				fail("la medición no terminó");
				continue;
			}
			const double ns_i = med * 1e9 / lp[l].per_rep;  /* ns por instrucción guest */
			const double mips = 1000.0 / ns_i;
			const double cyc = ns_i * CPU_HZ / 1e9;         /* ciclos del PPE por instrucción guest */
			/* Modelo de ciclos del ARM9: 1 por instrucción y 2 más por salto tomado. */
			const double model_mhz = (lp[l].per_rep + 2.0 * lp[l].taken_per_rep) / med / 1e6;
			mips_tab[l][im] = mips;
			salsads_log("DATO jit %-7s %-52s: %u.%02u ns/instr. guest = %u.%02u MIPS (mín %u.%02u), %u.%02u ciclos del PPE por instr.; "
				    "ARM9 equivalente %u.%02u MHz (con 1 ciclo por instr.), %u.%02u MHz (+2 por salto tomado); "
				    "meta de 67 MHz: %s",
				    ja_loops[l].name, impl_name[im], F2(ns_i), F2(mips), F2(1000.0 / (mn * 1e9 / lp[l].per_rep)),
				    F2(cyc), F2(mips), F2(model_mhz), mips >= ARM9_MHZ ? "SÍ" : "NO");
		}
	}
}

static void summarize(void)
{
	salsads_log("DATO jit resumen: ganancia del JIT sobre el intérprete de referencia (×) y de las banderas fusionadas sobre "
		    "las perezosas");
	for (unsigned l = 0; l < ja_nloops; l++) {
		if (mips_tab[l][IMP_REF] <= 0)
			continue;
		char o3[48] = "";
		if (mips_tab[l][IMP_O3] > 0)
			snprintf(o3, sizeof o3, "; contra el de la 0.3 ×%u.%02u", F2(mips_tab[l][IMP_LAZY] / mips_tab[l][IMP_O3]));
		if (mips_tab[l][IMP_LAZY] > 0)
			salsads_log("DATO jit resumen %-7s perezosas ×%u.%02u, CR ×%u.%02u, fusionadas ×%u.%02u (contra la referencia); "
				    "CR/perezosas %c%u.%02u %%, fusionadas/perezosas %c%u.%02u %%%s",
				    ja_loops[l].name, F2(mips_tab[l][IMP_LAZY] / mips_tab[l][IMP_REF]),
				    F2(mips_tab[l][IMP_CR] / mips_tab[l][IMP_REF]), F2(mips_tab[l][IMP_FUSED] / mips_tab[l][IMP_REF]),
				    SGN(mips_tab[l][IMP_CR] / mips_tab[l][IMP_LAZY] - 1), F2((mips_tab[l][IMP_CR] / mips_tab[l][IMP_LAZY] - 1) * 100),
				    SGN(mips_tab[l][IMP_FUSED] / mips_tab[l][IMP_LAZY] - 1), F2((mips_tab[l][IMP_FUSED] / mips_tab[l][IMP_LAZY] - 1) * 100), o3);
	}
}

/* ---- (c): trozo del ARM9 para el experimento de hilos ---------------------------------- */

static struct {
	struct ja_state s;
	const uint32_t *code;
	unsigned loop;
	double per_rep;
	uint8_t mem[JA_MEM_SIZE] __attribute__((aligned(128)));
} chunk_ctx;

static uint64_t arm9_chunk(void *ctx)
{
	(void)ctx;
	struct ja_state *s = &chunk_ctx.s;
	s->r[6] = 1;
	s->budget = INT64_MAX / 2;
	if (chunk_ctx.code)
		ja_call(chunk_ctx.code, s);
	else
		ja_ref_run(s, ja_loops[chunk_ctx.loop].code, ja_loops[chunk_ctx.loop].nwords);
	return (uint64_t)chunk_ctx.per_rep;
}

int main(void)
{
	char banner[160];
	snprintf(banner, sizeof banner, "jit-arm %s (%s, %s)", JIT_VERSION, JIT_TITLE, JIT_APPID);
	salsads_log_init_banner("jit-arm", banner);
	tb_hz = (double)sysGetTimebaseFrequency();
	const int autotest = salsads_autotest();
	struct stat st;
	const int force_nojit = stat(NOJIT_FLAG, &st) == 0;
	if (autotest)
		min_ms = 2;
	salsads_log("timebase %u Hz; reloj del PPE supuesto %u MHz; calibración ≥ %u ms, %d repeticiones; meta: ARM9 a %u MHz",
		    (unsigned)tb_hz, (unsigned)(CPU_HZ / 1e6), (unsigned)min_ms, REPS, (unsigned)ARM9_MHZ);

	ja_loops_init();
	arm_init();

	/* Prueba de la caché con respaldo al intérprete. */
	if (force_nojit) {
		salsads_log("jit: %s existe: sin JIT, solo intérpretes (el respaldo)", NOJIT_FLAG);
	} else if (autotest) {
		salsads_log("jit: prueba automática (RPCS3): su texto es de solo lectura, no se intenta; solo intérpretes (el respaldo)");
	} else {
		char why[160];
		uint64_t us = 0;
		const enum rec_probe_status ps = salsads_rec_probe(MARKER, JIT_VERSION, 2000, why, sizeof why, &us);
		switch (ps) {
		case REC_PROBE_OK:
			jit_on = 1;
			salsads_log("jit: la caché de código de %u MB funciona (escribir, ejecutar, reescribir, hasta su último byte): "
				    "prueba de %u µs", REC_CACHE_BYTES >> 20, (unsigned)us);
			break;
		case REC_PROBE_SKIPPED:
			salsads_log("jit: sin JIT, solo intérpretes: %s", why);
			break;
		default:
			salsads_log("jit: la prueba de la caché %s: %s; sigo con los intérpretes (el respaldo)",
				    ps == REC_PROBE_HUNG ? "no volvió" : "falló", why);
			break;
		}
	}

	if (jit_on) {
		/* Mientras se usa código traducido, la marca dice «probando»: si el programa muere, el
		 * siguiente arranque de esta versión no usa el JIT. */
		salsads_rec_marker(MARKER, JIT_VERSION, 1);
		translate_all();
	}
	bench_loops();
	if (jit_on)
		summarize();

	/* (c) el ARM7 en el hilo 1: el ARM9 corre «suma64» en trozos. */
	{
		const unsigned loop = 1;
		ja_loop_setup(&chunk_ctx.s, chunk_ctx.mem, 1);
		chunk_ctx.loop = loop;
		chunk_ctx.per_rep = lp[loop].per_rep;
		chunk_ctx.code = jit_on ? jit_code[loop][IMP_FUSED - IMP_LAZY] : NULL;
		salsads_log("hilos: el ARM9 corre «%s» con %s en trozos de %u instr.; el hilo del ARM7 despierta, hace 2 µs de "
			    "trabajo y vuelve a esperar", ja_loops[loop].name, chunk_ctx.code ? "el JIT" : "el intérprete de referencia",
			    (unsigned)chunk_ctx.per_rep);
		if (threads_bench(arm9_chunk, NULL, tb_hz, autotest) != 0)
			salsads_log("hilos: alguna combinación no se pudo armar");
	}

	if (jit_on)
		salsads_rec_marker(MARKER, JIT_VERSION, 0);
	salsads_log("RESULTADO jit-arm: %s", failures ? "FALLO" : "OK");
	salsads_log_shutdown();
	return failures ? 1 : 0;
}
