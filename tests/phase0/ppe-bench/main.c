/*
 * SalsaDS — Fase 0.3: microbenchmarks del PPE (PS3 real).
 *
 * Mide, con el timebase (mftb, 79,8 MHz = 1 tick cada 40 ciclos de 3,2 GHz):
 *
 *   1. memoria little-endian: lwbrx/stwbrx/lhbrx/sthbrx contra carga normal
 *      (la memoria de la DS es LE; el PPE es BE) y contra carga normal +
 *      rotaciones, en ciclos por acceso, en throughput (accesos
 *      independientes) y en latencia (cadena dependiente);
 *   2. banderas NZCV del ARM: tres estrategias más una cota inferior sin
 *      banderas, sobre un bucle guest con ADDS/CMP/BLT/CMP/BHI/SUBS/BNE;
 *   3. despacho de un intérprete ARM mínimo: switch, tabla de punteros a
 *      función y goto calculado, en MIPS guest.
 *
 * Cada medición se calibra a ≥ 60 ms, se repite 7 veces y se reporta la
 * mediana y el mínimo. Antes de medir, cada núcleo se verifica contra una
 * referencia en C: si algo no coincide, el resultado final es FALLO.
 *
 * Las líneas «DATO …» son las que se copian a docs/fase0.md y perf-log.md.
 * RPCS3 solo sirve para ver que arranca: sus números no valen.
 */
#include <malloc.h>
#include <stdio.h>
#include <ppu-types.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/process.h>
#include <sys/systime.h>

#include "interp.h"
#include "kernels.h"
#include "platform/ps3/log.h"

#ifndef BENCH_VERSION
#define BENCH_VERSION "desconocida"
#endif
#define BENCH_TITLE "SalsaDS bench"
#define BENCH_APPID "SLDS00001"

SYS_PROCESS_PARAM(1001, 0x10000);

#define CPU_HZ 3200000000.0
#define MIN_TICKS_MS 60
#define REPS 7

static double tb_hz;
static int failures;

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

/* Centésimas como «a.bc» sin usar printf de flotantes. */
static unsigned cent(double v)
{
	if (v < 0) /* ruido al restar el bucle: se muestra 0,00 */
		v = 0;
	return (unsigned)(v * 100.0 + 0.5);
}
#define F2(v) cent(v) / 100, cent(v) % 100

/* Contexto de una medición: `run` hace `iters` vueltas y devuelve un valor
 * que se acumula para que nada se optimice. */
struct meas {
	uint64_t (*run)(void *ctx, uint64_t iters);
	void *ctx;
};

static volatile uint64_t sink;

static double time_run(const struct meas *m, uint64_t iters)
{
	uint64_t t0 = tb_now();
	sink += m->run(m->ctx, iters);
	return (double)(tb_now() - t0);
}

static int cmp_d(const void *a, const void *b)
{
	double x = *(const double *)a, y = *(const double *)b;
	return x < y ? -1 : x > y;
}

/* Segundos por vuelta (mediana y mínimo) y las vueltas usadas. */
static void measure(const struct meas *m, double *med, double *min, uint64_t *iters_out)
{
	uint64_t iters = 1024;
	double need = tb_hz * MIN_TICKS_MS / 1000.0;
	double t;

	while ((t = time_run(m, iters)) < need)
		iters *= t < need / 16 ? 8 : 2;
	double per[REPS];
	for (int i = 0; i < REPS; i++)
		per[i] = time_run(m, iters) / (double)iters / tb_hz;
	qsort(per, REPS, sizeof per[0], cmp_d);
	*med = per[REPS / 2];
	*min = per[0];
	*iters_out = iters;
}

/* ---- 1. Memoria ------------------------------------------------------------- */

static uint8_t *bench_buf;

static uint64_t run_mem(void *ctx, uint64_t iters)
{
	struct kmem_desc *d = ctx;
	d->fn(bench_buf, iters);
	return iters;
}

static uint64_t run_membase(void *ctx, uint64_t iters)
{
	(void)ctx;
	kmem_base(bench_buf, iters);
	return iters;
}

struct chase_ctx {
	const struct kchase_desc *d;
	const void *table;
};

static uint64_t run_chase(void *ctx, uint64_t iters)
{
	struct chase_ctx *c = ctx;
	return c->d->fn(c->table, iters);
}

static const char *kind_name(enum kmem_kind k)
{
	static const char *const n[] = { "carga32", "carga16", "almac32", "almac16" };
	return n[k];
}

static void bench_memory(void)
{
	salsads_log("== 1. Memoria little-endian en el PPE (L1 caliente, alineado)");
	bench_buf = memalign(128, 65536);
	memset(bench_buf, 0x5A, 65536);

	struct meas mb = { run_membase, NULL };
	double base_med, base_min;
	uint64_t it;
	measure(&mb, &base_med, &base_min, &it);
	salsads_log("DATO mem base bucle (4 nop + bdnz): %u.%02u ciclos por vuelta (min %u.%02u)",
		    F2(base_med * CPU_HZ), F2(base_min * CPU_HZ));

	salsads_log("DATO mem %-30s %-8s  bruto(ciclos/acc, mediana/min)  neto(restando bucle)", "instrucciones", "tipo");
	for (int i = 0; i < kmem_count; i++) {
		struct kmem_desc *d = (struct kmem_desc *)&kmem_table[i];
		struct meas m = { run_mem, d };
		double med, min;
		measure(&m, &med, &min, &it);
		double gross = med * CPU_HZ / KMEM_UNROLL, gmin = min * CPU_HZ / KMEM_UNROLL;
		double net = (med - base_med) * CPU_HZ / KMEM_UNROLL, nmin = (min - base_min) * CPU_HZ / KMEM_UNROLL;
		salsads_log("DATO mem %-30s %-8s  %u.%02u / %u.%02u   neto %u.%02u / %u.%02u",
			    d->name, kind_name(d->kind), F2(gross), F2(gmin), F2(net), F2(nmin));
	}

	/* Latencia: tabla de 1024 entradas en el orden de bytes de cada variante. */
	salsads_log("DATO mem latencia: cadena dependiente (rlwinm + carga [+ 3 instr. de swap]) por paso");
	uint8_t *table = memalign(128, 4096);
	for (int i = 0; i < kchase_count; i++) {
		const struct kchase_desc *d = &kchase_table[i];
		uint32_t want = kchase_fill(table, d->bits, d->impl, 4000);
		uint32_t got = d->fn(table, 1000);
		if (got != want) {
			char msg[96];
			snprintf(msg, sizeof msg, "latencia %s %d bits: %u en vez de %u", d->name, d->bits, got, want);
			fail(msg);
			continue;
		}
		struct chase_ctx cc = { d, table };
		struct meas m = { run_chase, &cc };
		double med, min;
		measure(&m, &med, &min, &it);
		salsads_log("DATO lat %-30s %2d bits  %u.%02u ciclos/paso (min %u.%02u)", d->name, d->bits,
			    F2(med * CPU_HZ / KCHASE_UNROLL), F2(min * CPU_HZ / KCHASE_UNROLL));
	}
	free(table);
	free(bench_buf);
}

/* ---- 2. Banderas ------------------------------------------------------------ */

struct flags_ctx {
	kflags_fn fn;
};

static uint64_t run_flags(void *ctx, uint64_t iters)
{
	struct flags_ctx *c = ctx;
	struct kguest g;
	kflags_init(&g, (uint32_t)iters);
	c->fn(&g);
	return g.r[0] + g.r[3] + g.r[6];
}

static void bench_flags(void)
{
	salsads_log("== 2. Banderas NZCV (bucle guest: ADDS, CMP, BLT, ADDS, CMP, BHI, ADDS, SUBS, BNE)");

	/* Verificación contra la referencia en C. */
	struct kguest ref, got;
	kflags_init(&ref, 5000);
	uint64_t guest_instr = kflags_ref(&ref);
	for (int k = 0; k < KF_COUNT; k++) {
		kflags_init(&got, 5000);
		kflags_table[k](&got);
		if (memcmp(&ref, &got, sizeof ref)) {
			char msg[96];
			snprintf(msg, sizeof msg, "banderas %s: estado final distinto de la referencia", kflags_name[k]);
			fail(msg);
		}
	}
	double per_iter_instr = (double)guest_instr / 5000.0;
	salsads_log("DATO flags guest: %u.%02u instrucciones ARM por vuelta (referencia en C)", F2(per_iter_instr));

	double base_per_instr = 0;
	for (int k = 0; k < KF_COUNT; k++) {
		struct flags_ctx fc = { kflags_table[k] };
		struct meas m = { run_flags, &fc };
		double med, min;
		uint64_t it;
		measure(&m, &med, &min, &it);
		double cyc_iter = med * CPU_HZ, cmin = min * CPU_HZ;
		double cyc_instr = cyc_iter / per_iter_instr;
		if (k == KF_NONE)
			base_per_instr = cyc_instr;
		salsads_log("DATO flags %-30s %u.%02u ciclos/vuelta (min %u.%02u) = %u.%02u ciclos por instr. ARM; sobre la cota: +%u.%02u",
			    kflags_name[k], F2(cyc_iter), F2(cmin), F2(cyc_instr), F2(cyc_instr - base_per_instr));
	}
}

/* ---- 4. Costo de instrucciones sueltas, ramas y mtctr/bctr ------------------- */

struct instr_ctx {
	kinstr_fn fn;
};

static uint64_t run_instr(void *ctx, uint64_t iters)
{
	struct instr_ctx *c = ctx;
	c->fn(iters);
	return iters;
}

static void bench_instr(void)
{
	salsads_log("== 4. Costo de instrucciones (throughput de %d copias; las de acarreo dependen entre sí por XER)",
		    KINSTR_UNROLL);
	double base_med[2] = { 0, 0 }, base_min[2] = { 0, 0 };
	kinstr_fn cur_base[2] = { 0, 0 };
	uint64_t it;

	for (int i = 0; i < kinstr_count; i++) {
		const struct kinstr_desc *d = &kinstr_table[i];
		kinstr_fn bf = d->base ? d->base : kinstr_base_nops;
		int slot = d->base ? 1 : 0;
		if (cur_base[slot] != bf) {
			struct instr_ctx bc = { bf };
			struct meas mb = { run_instr, &bc };
			measure(&mb, &base_med[slot], &base_min[slot], &it);
			cur_base[slot] = bf;
			salsads_log("DATO instr referencia (%s): %u.%02u ciclos por vuelta", d->base ? "salto directo" : "8 nops + bdnz",
				    F2(base_med[slot] * CPU_HZ));
		}
		struct instr_ctx ic = { d->fn };
		struct meas m = { run_instr, &ic };
		double med, min;
		measure(&m, &med, &min, &it);
		salsads_log("DATO instr %-8s %-38s bruto %u.%02u / %u.%02u  neto %u.%02u / %u.%02u ciclos por %s",
			    d->group, d->name, F2(med * CPU_HZ / d->items), F2(min * CPU_HZ / d->items),
			    F2((med - base_med[slot]) * CPU_HZ / d->items), F2((min - base_min[slot]) * CPU_HZ / d->items),
			    d->items == 1 ? "secuencia" : d->items == KINSTR_UNROLL ? "instrucción" : "par");
	}
}

/* ---- 3. Despacho ------------------------------------------------------------- */

struct interp_ctx {
	enum arm_dispatch how;
	uint8_t *mem;
	uint64_t instr;
};

static uint64_t run_interp(void *ctx, uint64_t reps)
{
	struct interp_ctx *c = ctx;
	struct arm_cpu cpu;
	arm_load_program(&cpu, c->mem, (uint32_t)reps);
	arm_run(&cpu, c->how);
	c->instr = cpu.icount;
	return cpu.r[0];
}

static void bench_interp(void)
{
	salsads_log("== 3. Despacho de un intérprete ARM mínimo (bucle de %u instrucciones por elemento)", 9u);
	arm_init();
	uint8_t *mem = memalign(128, ARM_MEM_SIZE);

	for (int h = 0; h < DISPATCH_COUNT; h++) {
		struct arm_cpu cpu;
		uint32_t want_r0, want_out;
		arm_load_program(&cpu, mem, 3);
		arm_run(&cpu, (enum arm_dispatch)h);
		arm_expected(3, &want_r0, &want_out);
		if (cpu.halt != 1 || cpu.r[0] != want_r0 || arm_out_sum(&cpu) != want_out ||
		    cpu.icount != arm_expected_icount(3)) {
			char msg[96];
			snprintf(msg, sizeof msg, "intérprete %s: resultado distinto del esperado", arm_dispatch_name[h]);
			fail(msg);
			continue;
		}
	}
	for (int h = 0; h < DISPATCH_COUNT; h++) {
		struct interp_ctx ic = { (enum arm_dispatch)h, mem, 0 };
		struct meas m = { run_interp, &ic };
		double med, min;
		uint64_t it;
		measure(&m, &med, &min, &it);
		double per_instr_ns = med * 1e9 / (double)arm_expected_icount(1);
		double mips = 1000.0 / per_instr_ns;
		salsads_log("DATO interp %-30s %u.%02u ns/instr. = %u.%02u MIPS guest = %u.%02u ciclos/instr. (min %u.%02u MIPS)",
			    arm_dispatch_name[h], F2(per_instr_ns), F2(mips), F2(per_instr_ns * 3.2),
			    F2(1000.0 / (min * 1e9 / (double)arm_expected_icount(1))));
	}
	salsads_log("DATO interp referencia: la DS pide ~100 MIPS (ARM9 67 + ARM7 33 MHz) para velocidad completa");
	free(mem);
}

int main(void)
{
	salsads_log_init("ppe-bench");
	salsads_log("ppe-bench %s (%s, %s)", BENCH_VERSION, BENCH_TITLE, BENCH_APPID);
	tb_hz = (double)sysGetTimebaseFrequency();
	salsads_log("timebase %u Hz; reloj del PPE supuesto %u MHz (%u ciclos por tick)", (unsigned)tb_hz,
		    (unsigned)(CPU_HZ / 1e6), (unsigned)(CPU_HZ / tb_hz + 0.5));
	salsads_log("calibración ≥ %d ms por medición, %d repeticiones; cifras: mediana / mínimo", MIN_TICKS_MS, REPS);

	bench_memory();
	bench_flags();
	bench_instr();
	bench_interp();

	salsads_log("RESULTADO ppe-bench: %s", failures ? "FALLO" : "OK");
	salsads_log_shutdown();
	return failures ? 1 : 0;
}
