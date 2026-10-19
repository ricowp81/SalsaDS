/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * SalsaDS — mide cuántas instrucciones por segundo da el intérprete de CPU
 * (src/cpu) en el host. No sirve para el rendimiento en la PS3: es un número
 * del host (la Mac o el contenedor) para ver el costo del intérprete y
 * comparar versiones. Los programas son los cuatro bucles de la Fase 0.7
 * (src/jit/loops.c, con su resultado calculado en C, que se comprueba antes de
 * medir) y dos núcleos propios, uno ARM y otro Thumb, con cargas y
 * almacenamientos, multiplicación, desplazamientos y ramas.
 *
 *   armbench [segundos por programa]
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "cpu/acore.h"
#include "jit/jit_arm.h"
#include "util/endian.h"

#define MEM_SIZE 0x10000u

static uint8_t mem[MEM_SIZE];
static int done;

static uint32_t b_r8(struct acore *c, uint32_t a) { (void)c; return mem[a & (MEM_SIZE - 1)]; }
static uint32_t b_r16(struct acore *c, uint32_t a) { (void)c; return rd16le(&mem[a & (MEM_SIZE - 1)]); }
static uint32_t b_r32(struct acore *c, uint32_t a) { (void)c; return rd32le(&mem[a & (MEM_SIZE - 1)]); }
static void b_w8(struct acore *c, uint32_t a, uint32_t v) { (void)c; mem[a & (MEM_SIZE - 1)] = (uint8_t)v; }
static void b_w16(struct acore *c, uint32_t a, uint32_t v) { (void)c; wr16le(&mem[a & (MEM_SIZE - 1)], (uint16_t)v); }
static void b_w32(struct acore *c, uint32_t a, uint32_t v) { (void)c; wr32le(&mem[a & (MEM_SIZE - 1)], v); }
static const struct acore_bus bus = { b_r8, b_r16, b_r32, b_w8, b_w16, b_w32, b_r16, b_r32 };

/* SWI de fin de programa */
static int swi_hook(struct acore *c, uint32_t comment)
{
	(void)comment;
	done = 1;
	acore_halt(c); /* acore_run no sigue ejecutando lo que haya después de la SWI */
	return 1;
}

static double now(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (double)ts.tv_sec + (double)ts.tv_nsec * 1e-9;
}

/* ---- codificadores mínimos -------------------------------------------------- */

#define AL 0xE0000000u
enum { OP_AND, OP_EOR, OP_SUB, OP_RSB, OP_ADD, OP_ADC, OP_SBC, OP_RSC, OP_TST, OP_TEQ, OP_CMP, OP_CMN, OP_ORR, OP_MOV, OP_BIC, OP_MVN };
enum { LSL, LSR, ASR, ROR };

static uint32_t a_dp_i(unsigned op, unsigned s, unsigned rn, unsigned rd, unsigned imm)
{
	return AL | 0x02000000u | op << 21 | s << 20 | rn << 16 | rd << 12 | imm;
}
static uint32_t a_dp_r(unsigned op, unsigned s, unsigned rn, unsigned rd, unsigned rm, unsigned type, unsigned amt)
{
	return AL | op << 21 | s << 20 | rn << 16 | rd << 12 | amt << 7 | type << 5 | rm;
}
/* STR rd, [rn, #off] */
static uint32_t a_str(unsigned rn, unsigned rd, unsigned off) { return AL | 0x05800000u | rn << 16 | rd << 12 | off; }
/* LDMIA rn!, {list} */
static uint32_t a_ldmia_wb(unsigned rn, unsigned list) { return AL | 0x08B00000u | rn << 16 | list; }
static uint32_t a_mla(unsigned rd, unsigned rm, unsigned rs, unsigned rn)
{
	return AL | 1u << 21 | rd << 16 | rn << 12 | rs << 8 | 0x90u | rm;
}
/* B<cc> con `dest` y `from` como índices de palabra */
static uint32_t a_bne(int from, int dest) { return 0x1A000000u | (uint32_t)((dest - (from + 2)) & 0xFFFFFF); }

static uint32_t t_ldrb(unsigned rd, unsigned rb, unsigned imm) { return 0x7800u | imm << 6 | rb << 3 | rd; }
static uint32_t t_strb(unsigned rd, unsigned rb, unsigned imm) { return 0x7000u | imm << 6 | rb << 3 | rd; }
static uint32_t t_adds_r(unsigned rd, unsigned rs, unsigned rn) { return 0x1800u | rn << 6 | rs << 3 | rd; }
static uint32_t t_adds_i8(unsigned rd, unsigned imm) { return 0x3000u | rd << 8 | imm; }
static uint32_t t_lsls(unsigned rd, unsigned rs, unsigned imm) { return imm << 6 | rs << 3 | rd; }
static uint32_t t_alu(unsigned op, unsigned rd, unsigned rs) { return 0x4000u | op << 6 | rs << 3 | rd; }
/* BNE con `dest` y `from` como índices de media palabra */
static uint32_t t_bne(int from, int dest) { return 0xD100u | (uint32_t)((dest - (from + 2)) & 0xFF); }

/* Núcleo ARM: LDM de 4 registros, ADD y EOR con desplazamientos, MLA, STR, SUBS y BNE:
 * 7 instrucciones por cada 4 palabras. r1 = origen, r8 = destino, r9 = cuenta. */
static uint32_t arm_kernel[8];
/* Núcleo Thumb: LDRB, ADDS, LSLS, EORS, STRB, ADDS, ADDS, CMP, BNE: 9 por byte. */
static uint16_t th_kernel[16];

static void build_kernels(void)
{
	unsigned n = 0;

	arm_kernel[n++] = a_ldmia_wb(1, 0x0078);                  /* 00 ldmia r1!, {r3-r6} */
	arm_kernel[n++] = a_dp_r(OP_ADD, 0, 0, 0, 3, LSL, 2);     /* 04 add r0, r0, r3, lsl #2 */
	arm_kernel[n++] = a_dp_r(OP_EOR, 0, 0, 0, 4, ROR, 7);     /* 08 eor r0, r0, r4, ror #7 */
	arm_kernel[n++] = a_mla(7, 5, 6, 0);                      /* 0C mla r7, r5, r6, r0 */
	arm_kernel[n++] = a_str(8, 7, 0);                         /* 10 str r7, [r8] */
	arm_kernel[n++] = a_dp_i(OP_SUB, 1, 9, 9, 1);             /* 14 subs r9, r9, #1 */
	arm_kernel[n] = a_bne((int)n, 0);                         /* 18 bne 00 */
	n++;
	arm_kernel[n++] = AL | 0x0F000000u;                       /* 1C swi */

	n = 0;
	th_kernel[n++] = (uint16_t)t_ldrb(3, 1, 0);               /* 00 ldrb r3, [r1] */
	th_kernel[n++] = (uint16_t)t_adds_r(0, 0, 3);             /* 02 adds r0, r0, r3 */
	th_kernel[n++] = (uint16_t)t_lsls(4, 0, 3);               /* 04 lsls r4, r0, #3 */
	th_kernel[n++] = (uint16_t)t_alu(1, 4, 3);                /* 06 eors r4, r3 */
	th_kernel[n++] = (uint16_t)t_strb(4, 5, 0);               /* 08 strb r4, [r5] */
	th_kernel[n++] = (uint16_t)t_adds_i8(1, 1);               /* 0A adds r1, #1 */
	th_kernel[n++] = (uint16_t)t_adds_i8(5, 1);               /* 0C adds r5, #1 */
	th_kernel[n++] = (uint16_t)t_alu(10, 1, 2);               /* 0E cmp r1, r2 */
	th_kernel[n] = (uint16_t)t_bne((int)n, 0);                /* 10 bne 00 */
	n++;
	th_kernel[n++] = 0xDF00;                                  /* 12 swi */
}

/* ---- ejecución ----------------------------------------------------------------------- */

struct result {
	double secs;
	uint64_t instr;
};

typedef void (*prep_fn)(struct acore *c, uint32_t reps);

static unsigned cur_loop;

static void prep_ja(struct acore *c, uint32_t reps)
{
	struct ja_state s;

	memset(&s, 0, sizeof s);
	ja_loop_setup(&s, mem, reps); /* deja la memoria a cero salvo los datos: se vuelve a cargar el programa */
	for (unsigned i = 0; i < ja_loops[cur_loop].nwords; i++)
		wr32le(&mem[4 * i], ja_loops[cur_loop].code[i]);
	for (int i = 0; i < 15; i++) /* r15 no: lo arma acore_set_pc */
		c->r[i] = s.r[i];
}

static void prep_arm(struct acore *c, uint32_t reps)
{
	for (int i = 0; i < 15; i++)
		c->r[i] = 0;
	for (uint32_t i = 0; i < 4096; i++)
		wr32le(&mem[0x1000 + 4 * i], i * 2654435761u);
	c->r[1] = 0x1000;
	c->r[8] = 0x8000;
	c->r[9] = reps; /* pasadas de 4 palabras */
}

static void prep_th(struct acore *c, uint32_t reps)
{
	for (int i = 0; i < 15; i++)
		c->r[i] = 0;
	for (uint32_t i = 0; i < 4096; i++)
		mem[0x1000 + i] = (uint8_t)(i * 37);
	c->r[1] = 0x1000;
	c->r[2] = 0x1000 + 4096;
	c->r[5] = 0x8000;
	(void)reps;
}

static struct result run_program(struct acore *c, prep_fn prep, uint32_t reps, int thumb)
{
	struct result r;
	double t0;

	done = 0;
	c->halted = 0;
	c->attn = 0;
	acore_set_cpsr(c, ACORE_MODE_SYS | ACORE_I | ACORE_F | (thumb ? ACORE_T : 0));
	acore_set_pc(c, 0);
	prep(c, reps);
	c->icount = 0;
	c->cycles = 0;
	t0 = now();
	while (!done)
		acore_run(c, c->cycles + 100000);
	r.secs = now() - t0;
	r.instr = c->icount;
	return r;
}

/* Calibra las pasadas para llegar a `target` segundos y devuelve la medida final. */
static struct result measure(struct acore *c, prep_fn prep, int thumb, double target, uint32_t reps_unit)
{
	uint32_t reps = reps_unit;
	struct result r;

	for (;;) {
		r = run_program(c, prep, reps, thumb);
		if (r.secs >= target / 4 || reps > (1u << 26))
			break;
		reps *= 4;
	}
	reps = (uint32_t)((double)reps * target / (r.secs > 0 ? r.secs : 1e-3));
	return run_program(c, prep, reps ? reps : reps_unit, thumb);
}

static void report(const char *name, struct result r)
{
	printf("%-10s %12llu %9.3f %9.1f %9.2f\n", name, (unsigned long long)r.instr, r.secs, (double)r.instr / r.secs / 1e6,
	       r.secs / (double)r.instr * 1e9);
}

int main(int argc, char **argv)
{
	const double target = argc > 1 ? atof(argv[1]) : 0.5;
	struct acore c;
	int bad = 0;

	setvbuf(stdout, NULL, _IOLBF, 0);
	build_kernels();
	acore_init(&c, ACORE_ARM9, &bus, NULL);
	c.swi_hook = swi_hook;
	ja_loops_init();

	printf("%-10s %12s %9s %9s %9s\n", "programa", "instr.", "segundos", "MIPS", "ns/instr");
	for (cur_loop = 0; cur_loop < ja_nloops; cur_loop++) {
		const struct ja_loop *l = &ja_loops[cur_loop];
		struct ja_expect e;

		/* el resultado se comprueba contra el calculado en C antes de medir */
		(void)run_program(&c, prep_ja, 3, 0);
		ja_loop_expect(cur_loop, 3, &e);
		if (c.r[0] != e.r0 || c.r[7] != e.r7 || (cur_loop >= 2 && c.r[4] != e.r4) || ja_out_sum(mem) != e.out_sum) {
			printf("FALLO %s: el resultado no coincide con el calculado en C (r0 %08x/%08x r4 %08x/%08x r7 %08x/%08x out %08x/%08x, %llu instr.)\n",
			       l->name, c.r[0], e.r0, c.r[4], e.r4, c.r[7], e.r7, ja_out_sum(mem), e.out_sum, (unsigned long long)c.icount);
			bad = 1;
			continue;
		}
		report(l->name, measure(&c, prep_ja, 0, target, 1));
	}

	memset(mem, 0, 0x1000);
	for (unsigned i = 0; i < sizeof arm_kernel / sizeof arm_kernel[0]; i++)
		wr32le(&mem[4 * i], arm_kernel[i]);
	report("arm-mix", measure(&c, prep_arm, 0, target, 64));

	memset(mem, 0, 0x1000);
	for (unsigned i = 0; i < sizeof th_kernel / sizeof th_kernel[0]; i++)
		wr16le(&mem[2 * i], th_kernel[i]);
	{
		struct result tot = { 0, 0 };

		while (tot.secs < target) {
			struct result r = run_program(&c, prep_th, 1, 1);

			tot.secs += r.secs;
			tot.instr += r.instr;
		}
		report("thumb-mix", tot);
	}
	return bad;
}
