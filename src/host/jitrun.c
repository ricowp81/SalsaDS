/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * SalsaDS — los recompiladores del ARM9 y del ARM7 en el corredor del host (salsads-cpurun): --jit, --jit-strict y
 * --lockstep (--jit-no7 y --jit-no9 dejan una de las dos CPUs con el intérprete).
 *
 * El código generado es PowerPC64 big-endian: corre bajo qemu-ppc64 (tools/qemu-ppc64.sh); en la Mac (arm64) el
 * recompilador no se puede crear y estas opciones fallan con un mensaje.
 *
 * --lockstep: una segunda máquina idéntica con el intérprete. Las dos avanzan tramo por tramo del planificador; dentro
 * del tramo de cada CPU, después de cada bloque traducido (sin enlaces, con los ciclos exactos del intérprete), la
 * referencia ejecuta las mismas instrucciones y se comparan los registros, el CPSR (las banderas que la salida dejó
 * exactas), los ciclos y las instrucciones. Al final de cada tramo, las dos CPUs enteras; cada 2048 tramos, la memoria.
 */
#include <stdio.h>
#include <string.h>

#include "core/nds.h"
#include "host/jitrun.h"
#include "jit/arec.h"

#if defined(__linux__)
#include <sys/mman.h>
#endif

#define CODE_BYTES (5u << 20) /* 4 MB del ARM9 y 1 MB del ARM7 */
#define CODE7_BYTES (1u << 20)

static uint8_t code_mem[CODE_BYTES] __attribute__((aligned(65536)));

static struct nds *ref;      /* máquina de referencia (--lockstep) */
static struct nds *jm;       /* la del JIT */
static uint64_t ls_checks, ls_slices;
static int cur_cpu; /* CPU del último aviso (0 = ARM9, 1 = ARM7) */
static int ls_failed;

int hj_fail(void)
{
	return ls_failed;
}

static int same_cpu(const struct acore *a, const struct acore *b, unsigned live, char *why, size_t wn)
{
	const uint32_t fm = (live & 8 ? 0x80000000u : 0) | (live & 4 ? 0x40000000u : 0) | (live & 2 ? 0x20000000u : 0) |
			    (live & 1 ? 0x10000000u : 0);

	for (int k = 0; k < 16; k++)
		if (a->r[k] != b->r[k]) {
			snprintf(why, wn, "r%d %08x (JIT) contra %08x", k, a->r[k], b->r[k]);
			return 0;
		}
	if ((a->cpsr & (0x0FFFFFFFu | fm)) != (b->cpsr & (0x0FFFFFFFu | fm))) {
		snprintf(why, wn, "cpsr %08x (JIT) contra %08x (banderas exactas %x)", a->cpsr, b->cpsr, live);
		return 0;
	}
	if (a->cycles != b->cycles) {
		snprintf(why, wn, "ciclos %llu (JIT) contra %llu", (unsigned long long)a->cycles, (unsigned long long)b->cycles);
		return 0;
	}
	if (a->icount != b->icount) {
		snprintf(why, wn, "instrucciones %llu (JIT) contra %llu", (unsigned long long)a->icount,
			 (unsigned long long)b->icount);
		return 0;
	}
	if (a->halted != b->halted || a->bank != b->bank) {
		snprintf(why, wn, "HALT %u/%u, banco %u/%u", a->halted, b->halted, a->bank, b->bank);
		return 0;
	}
	for (int k = 1; k < 6; k++)
		if (a->spsr_bank[k] != b->spsr_bank[k] || a->sp_bank[k] != b->sp_bank[k] || a->lr_bank[k] != b->lr_bank[k]) {
			snprintf(why, wn, "banco %d: spsr %08x/%08x sp %08x/%08x lr %08x/%08x", k, a->spsr_bank[k], b->spsr_bank[k],
				 a->sp_bank[k], b->sp_bank[k], a->lr_bank[k], b->lr_bank[k]);
			return 0;
		}
	return 1;
}

static void fail(const char *what, const char *why, uint32_t pc0)
{
	const struct acore *a = cur_cpu ? &jm->a7 : &jm->a9, *b = cur_cpu ? &ref->a7 : &ref->a9;

	if (ls_failed)
		return;
	ls_failed = 1;
	printf("LOCKSTEP: DIFERENCIA (ARM%d) después de %s (bloque en %08x), cuadro %llu, t=%llu: %s\n", cur_cpu ? 7 : 9, what, pc0,
	       (unsigned long long)jm->frames, (unsigned long long)jm->time, why);
	printf("  JIT: pc %08x cpsr %08x", acore_pc(a), a->cpsr);
	for (int k = 0; k < 15; k++)
		printf(" r%d=%08x", k, a->r[k]);
	printf("\n  ref: pc %08x cpsr %08x", acore_pc(b), b->cpsr);
	for (int k = 0; k < 15; k++)
		printf(" r%d=%08x", k, b->r[k]);
	printf("\n");
	jm->stop = ref->stop = 1;
}

static uint32_t blk_pc;

static void on_exec_cpu(int cpu, int ev, uint64_t until)
{
	struct acore *b = cpu ? &ref->a7 : &ref->a9;
	struct acore *a = cpu ? &jm->a7 : &jm->a9;
	char why[160];
	unsigned live = 15;

	if (ls_failed)
		return;
	cur_cpu = cpu;
	/* la referencia corre dentro del tramo del JIT: el mismo final de tramo (los sondeos que saltan hasta ahí, nds_poll_skip) */
	ref->slice_end = jm->slice_end;
	switch (ev) {
	case AREC_EV_SERVICE:
		acore_service(b, until);
		break;
	case AREC_EV_STEP:
		acore_step_insn(b);
		break;
	default:
		while (b->icount < a->icount)
			acore_step_insn(b);
		live = arec_last_live(cpu ? jm->jit7 : jm->jit9);
		break;
	}
	ls_checks++;
	for (int e = 0; e < EV_COUNT; e++)
		if (jm->ev[e] != ref->ev[e] || jm->ev_min != ref->ev_min) {
			snprintf(why, sizeof why, "evento %d en %llu contra %llu (mínimo %llu contra %llu)", e,
				 (unsigned long long)jm->ev[e], (unsigned long long)ref->ev[e], (unsigned long long)jm->ev_min,
				 (unsigned long long)ref->ev_min);
			fail("un bloque (eventos)", why, blk_pc);
			return;
		}
	if (!same_cpu(a, b, live, why, sizeof why))
		fail(ev == AREC_EV_BLOCK ? "un bloque" : ev == AREC_EV_STEP ? "un paso del intérprete" : "una interrupción", why,
		     blk_pc);
	blk_pc = acore_pc(a);
}

static void on_exec9(void *ctx, int ev, uint64_t until)
{
	(void)ctx;
	on_exec_cpu(0, ev, until);
}

static void on_exec7(void *ctx, int ev, uint64_t until)
{
	(void)ctx;
	on_exec_cpu(1, ev, until);
}

int hj_attach(struct nds *n, int strict, int no_link, int far, int no_jl, int arm7, int arm9, void (*ev9)(void *, int, uint64_t),
	      void (*ev7)(void *, int, uint64_t))
{
	struct nds_jit_cfg cfg;

#if defined(__linux__)
	if (mprotect(code_mem, sizeof code_mem, PROT_READ | PROT_WRITE | PROT_EXEC)) {
		perror("mprotect");
		return -1;
	}
#endif
	memset(&cfg, 0, sizeof cfg);
	cfg.code = code_mem;
	cfg.code_bytes = sizeof code_mem;
	cfg.strict = strict;
	cfg.no_link = no_link;
	cfg.far_calls = far;
	cfg.no_inline_jl = no_jl;
	cfg.arm7 = arm7;
	cfg.no_arm9 = !arm9;
	cfg.code7_bytes = arm7 ? CODE7_BYTES : 0;
	cfg.on_exec = ev9;
	cfg.on_exec7 = ev7;
	if (nds_jit_attach(n, &cfg)) {
		fprintf(stderr, "el recompilador no corre en este host (el código es PowerPC64 big-endian: usar qemu-ppc64)\n");
		return -1;
	}
	return 0;
}

int hj_lockstep_attach(struct nds *m, struct nds *r, int arm7, int arm9)
{
	jm = m;
	ref = r;
	return hj_attach(m, 1, 1, 0, 1, arm7, arm9, arm9 ? on_exec9 : NULL, arm7 ? on_exec7 : NULL);
}

/* Un tramo de las dos máquinas y la comparación de las dos enteras. */
static void ls_slice(uint64_t k)
{
	char why[160];

	jm->one_step = ref->one_step = 1;
	blk_pc = acore_pc(&jm->a9);
	/* la referencia decide como el JIT, con el estado del ARM9 de antes del tramo */
	ref->force_idle9 = (ref->a9.halted && !(ref->a9.irq | ref->a9.fiq)) ? 2 : 1;
	ref->force_idle7 = (ref->a7.halted && !(ref->a7.irq | ref->a7.fiq)) ? 2 : 1;
	ref->force_next = ref->ev_min;
	const uint64_t t0 = jm->time, em0 = jm->ev_min, em0r = ref->ev_min;
	const int fi9 = ref->force_idle9, fi7 = ref->force_idle7;
	const uint64_t c9 = jm->a9.cycles, c7 = jm->a7.cycles;

	nds_run(jm, k);
	if (ls_failed)
		return;
	nds_run(ref, k);
	ls_slices++;
	if (jm->time != ref->time) {
		printf("  antes: t=%llu ev_min %llu/%llu ocioso9 %d ocioso7 %d; a9 %llu a7 %llu; después a9 %llu/%llu a7 %llu/%llu; "
		       "halt9 %u/%u halt7 %u/%u\n",
		       (unsigned long long)t0, (unsigned long long)em0, (unsigned long long)em0r, fi9, fi7, (unsigned long long)c9,
		       (unsigned long long)c7, (unsigned long long)jm->a9.cycles, (unsigned long long)ref->a9.cycles,
		       (unsigned long long)jm->a7.cycles, (unsigned long long)ref->a7.cycles, jm->a9.halted, ref->a9.halted,
		       jm->a7.halted, ref->a7.halted);
		snprintf(why, sizeof why, "tiempo %llu contra %llu", (unsigned long long)jm->time, (unsigned long long)ref->time);
		fail("el tramo", why, blk_pc);
		return;
	}
	if (!same_cpu(&jm->a9, &ref->a9, 15, why, sizeof why)) {
		fail("el tramo (ARM9)", why, blk_pc);
		return;
	}
	for (int e = 0; e < EV_COUNT; e++)
		if (jm->ev[e] != ref->ev[e]) {
			snprintf(why, sizeof why, "evento %d en %llu contra %llu", e, (unsigned long long)jm->ev[e],
				 (unsigned long long)ref->ev[e]);
			fail("el tramo (eventos)", why, blk_pc);
			return;
		}
	if (!same_cpu(&jm->a7, &ref->a7, 15, why, sizeof why)) {
		fail("el tramo (ARM7)", why, blk_pc);
		return;
	}
	if (!(ls_slices & 2047u)) {
		if (memcmp(jm->ram, ref->ram, NDS_RAM_SIZE) || memcmp(jm->itcm, ref->itcm, sizeof jm->itcm) ||
		    memcmp(jm->dtcm, ref->dtcm, sizeof jm->dtcm) || memcmp(jm->swram, ref->swram, sizeof jm->swram) ||
		    memcmp(jm->wram7, ref->wram7, sizeof jm->wram7) ||
		    memcmp(jm->vram, ref->vram, sizeof jm->vram) || memcmp(jm->pal, ref->pal, sizeof jm->pal) ||
		    memcmp(jm->oam, ref->oam, sizeof jm->oam)) {
			uint32_t o = 0;

			while (o < NDS_RAM_SIZE && jm->ram[o] == ref->ram[o])
				o++;
			snprintf(why, sizeof why, "memoria distinta (RAM desde 0x%08x)", 0x02000000u + o);
			fail("la comparación de memoria", why, blk_pc);
		}
	}
}

void hj_lockstep_frame(void)
{
	const uint64_t f0 = jm->frames;

	while (jm->frames == f0 && !ls_failed)
		ls_slice(2u * NDS_FRAME_CYCLES);
}

void hj_lockstep_cycles(uint64_t k)
{
	const uint64_t t1 = jm->time + k;

	while (jm->time < t1 && !ls_failed)
		ls_slice(t1 - jm->time);
}

static void print_stats_one(struct arec *j, const char *who)
{
	const struct arec_stats *s;
	uint32_t blocks;
	size_t used, cap;

	if (!j)
		return;
	s = arec_stats(j);
	arec_usage(j, &blocks, &used, &cap);
	printf("recompilador %s: %llu bloques traducidos (%llu instr., %.1f palabras PPC por instr., %.1f calientes), %u vivos, código %zu de %zu KB; "
	       "%llu entradas, %llu enlaces; salidas: %llu pc, %llu enlace, %llu estado, %llu presupuesto, %llu tras almacenamiento; "
	       "%llu llamadas al intérprete, %llu pasos interpretados (%llu sin código); lentos %llu lect. %llu escr.; "
	       "%llu escrituras a código, %llu bloques invalidados, %llu vaciados; %llu banderas en frío\n",
	       who, (unsigned long long)s->blocks_made, (unsigned long long)s->guest_made,
	       s->guest_made ? (double)s->host_words / (double)s->guest_made : 0.0,
	       s->guest_made ? (double)s->host_hot / (double)s->guest_made : 0.0, blocks, used >> 10, cap >> 10,
	       (unsigned long long)s->enters, (unsigned long long)s->links, (unsigned long long)s->exits_pc,
	       (unsigned long long)s->exits_link, (unsigned long long)s->exits_state, (unsigned long long)s->exits_budget,
	       (unsigned long long)s->mem_exits, (unsigned long long)s->callouts, (unsigned long long)s->interp_steps,
	       (unsigned long long)s->no_code, (unsigned long long)s->slow_rd, (unsigned long long)s->slow_wr,
	       (unsigned long long)s->code_wr, (unsigned long long)s->inval_blocks, (unsigned long long)s->flushes,
	       (unsigned long long)s->cold_flags);
	printf("recompilador %s, llamadas al intérprete por clase:", who);
	for (int k = 0; k < AREC_CALL_KINDS; k++)
		if (s->call_by[k])
			printf(" %s %llu;", arec_call_kind_name[k], (unsigned long long)s->call_by[k]);
	printf("\n");
}

void hj_print_stats(struct nds *n)
{
	print_stats_one(n->jit9, "ARM9");
	print_stats_one(n->jit7, "ARM7");
	if (ref)
		printf("LOCKSTEP: %s, %llu comparaciones, %llu tramos\n", ls_failed ? "FALLO" : "sin diferencias",
		       (unsigned long long)ls_checks, (unsigned long long)ls_slices);
}
