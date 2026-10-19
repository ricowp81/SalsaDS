/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * SalsaDS — programas ARM de prueba del JIT (Fase 0.7).
 *
 * Mapa del guest: datos de entrada en JA_DATA (1024 palabras = 4096 bytes),
 * zona de salida en JA_OUT (4096 bytes). r6 = pasadas.
 *
 *   mezcla  el programa del intérprete de la Fase 0.3 (9 instrucciones por
 *           elemento: suma, EOR con desplazamiento, AND, guardado)
 *   suma64  suma de 64 bits con banderas: ADDS + ADC y CMP/BNE
 *   ramas   dos comparaciones con salto por elemento, con datos casi
 *           aleatorios (HI/LS sin signo y GE/LT con signo)
 *   bytes   carga y guardado de bytes: 9 instrucciones por byte, 4096 bytes
 */
#include <string.h>

#include "jit/armenc.h"
#include "jit/jit_arm.h"
#include "util/endian.h"

#define MOVI(rd, rot, imm) arm_dp_imm(ARM_MOV, 0, 0, rd, rot, imm)
#define ADDI(rd, rn, rot, imm) arm_dp_imm(ARM_ADD, 0, rn, rd, rot, imm)

static void init_mix(uint32_t *c)
{
	const uint32_t p[] = {
	/* outer: */
	MOVI(1, 12, 0x10),                          /* 00 MOV r1,#0x1000 */
	ADDI(2, 1, 12, 0x10),                       /* 04 ADD r2,r1,#0x1000 */
	arm_dp_reg(ARM_MOV, 0, 0, 5, 2, ARM_LSL, 0), /* 08 MOV r5,r2 */
	/* loop: */
	0,                                          /* 0C LDR r3,[r1] (abajo) */
	arm_dp_reg(ARM_ADD, 0, 0, 0, 3, ARM_LSL, 0), /* 10 ADD r0,r0,r3 */
	arm_dp_reg(ARM_EOR, 0, 0, 4, 3, ARM_LSL, 3), /* 14 EOR r4,r0,r3,LSL #3 */
	arm_dp_imm(ARM_AND, 0, 4, 4, 0, 0xFF),      /* 18 AND r4,r4,#0xFF */
	0,                                          /* 1C STR r4,[r5] (abajo) */
	ADDI(1, 1, 0, 4),                           /* 20 ADD r1,r1,#4 */
	ADDI(5, 5, 0, 4),                           /* 24 ADD r5,r5,#4 */
	arm_dp_reg(ARM_CMP, 1, 1, 0, 2, ARM_LSL, 0), /* 28 CMP r1,r2 */
	0,                                          /* 2C BNE loop */
	arm_dp_imm(ARM_SUB, 1, 6, 6, 0, 1),         /* 30 SUBS r6,r6,#1 */
	0,                                          /* 34 BNE outer */
	ARM_SWI,                                    /* 38 SWI */
	};
	memcpy(c, p, sizeof p);
}

static void init_sum64(uint32_t *c)
{
	const uint32_t p[] = {
	MOVI(1, 12, 0x10),                          /* 00 MOV r1,#0x1000 */
	ADDI(2, 1, 12, 0x10),                       /* 04 ADD r2,r1,#0x1000 */
	0,                                          /* 08 LDR r3,[r1] */
	arm_dp_reg(ARM_ADD, 1, 0, 0, 3, ARM_LSL, 0), /* 0C ADDS r0,r0,r3 */
	arm_dp_imm(ARM_ADC, 0, 7, 7, 0, 0),         /* 10 ADC r7,r7,#0 */
	ADDI(1, 1, 0, 4),                           /* 14 ADD r1,r1,#4 */
	arm_dp_reg(ARM_CMP, 1, 1, 0, 2, ARM_LSL, 0), /* 18 CMP r1,r2 */
	0,                                          /* 1C BNE loop */
	arm_dp_imm(ARM_SUB, 1, 6, 6, 0, 1),         /* 20 SUBS r6,r6,#1 */
	0,                                          /* 24 BNE outer */
	ARM_SWI,                                    /* 28 SWI */
	};
	memcpy(c, p, sizeof p);
}

static void init_ramas(uint32_t *c)
{
	const uint32_t p[] = {
	MOVI(1, 12, 0x10),                          /* 00 MOV r1,#0x1000 */
	ADDI(2, 1, 12, 0x10),                       /* 04 ADD r2,r1,#0x1000 */
	MOVI(8, 4, 0x80),                           /* 08 MOV r8,#0x80000000 */
	MOVI(9, 0, 0),                              /* 0C MOV r9,#0 */
	0,                                          /* 10 LDR r3,[r1] */
	arm_dp_reg(ARM_CMP, 1, 3, 0, 8, ARM_LSL, 0), /* 14 CMP r3,r8 */
	0,                                          /* 18 BLS +0 */
	ADDI(0, 0, 0, 1),                           /* 1C ADD r0,r0,#1 */
	arm_dp_reg(ARM_CMP, 1, 3, 0, 9, ARM_LSL, 0), /* 20 CMP r3,r9 */
	0,                                          /* 24 BGE +0 */
	ADDI(4, 4, 0, 1),                           /* 28 ADD r4,r4,#1 */
	ADDI(1, 1, 0, 4),                           /* 2C ADD r1,r1,#4 */
	arm_dp_reg(ARM_CMP, 1, 1, 0, 2, ARM_LSL, 0), /* 30 CMP r1,r2 */
	0,                                          /* 34 BNE loop */
	arm_dp_imm(ARM_SUB, 1, 6, 6, 0, 1),         /* 38 SUBS r6,r6,#1 */
	0,                                          /* 3C BNE outer */
	ARM_SWI,                                    /* 40 SWI */
	};
	memcpy(c, p, sizeof p);
}

static void init_bytes(uint32_t *c)
{
	const uint32_t p[] = {
	MOVI(1, 12, 0x10),                          /* 00 MOV r1,#0x1000 */
	MOVI(2, 12, 0x20),                          /* 04 MOV r2,#0x2000 */
	arm_dp_reg(ARM_MOV, 0, 0, 5, 2, ARM_LSL, 0), /* 08 MOV r5,r2 */
	0,                                          /* 0C LDRB r3,[r1] */
	ADDI(3, 3, 0, 1),                           /* 10 ADD r3,r3,#1 */
	arm_dp_reg(ARM_EOR, 0, 3, 3, 4, ARM_LSL, 1), /* 14 EOR r3,r3,r4,LSL #1 */
	0,                                          /* 18 STRB r3,[r5] */
	arm_dp_reg(ARM_ADD, 0, 4, 4, 3, ARM_LSL, 0), /* 1C ADD r4,r4,r3 */
	ADDI(1, 1, 0, 1),                           /* 20 ADD r1,r1,#1 */
	ADDI(5, 5, 0, 1),                           /* 24 ADD r5,r5,#1 */
	arm_dp_reg(ARM_CMP, 1, 1, 0, 2, ARM_LSL, 0), /* 28 CMP r1,r2 */
	0,                                          /* 2C BNE loop */
	arm_dp_imm(ARM_SUB, 1, 6, 6, 0, 1),         /* 30 SUBS r6,r6,#1 */
	0,                                          /* 34 BNE outer */
	ARM_SWI,                                    /* 38 SWI */
	};
	memcpy(c, p, sizeof p);
}

/* Las palabras con LDR/STR/B no entran en un inicializador constante cómodo:
 * se arman una vez (funciones constantes de arm_*), ver ja_init_loops. */
static uint32_t code_mix[15], code_sum64[11], code_ramas[17], code_bytes[15];
static int loops_ready;

/* off de B respecto de PC + 8, en palabras, de la palabra `from` a la `to`. */
static uint32_t br(unsigned cond, int from, int to)
{
	return arm_b(cond, to - (from + 2));
}

void ja_loops_init(void)
{
	if (loops_ready)
		return;
	init_mix(code_mix);
	code_mix[3] = arm_ldr(0, 1, 3, 0);
	code_mix[7] = arm_str(0, 5, 4, 0);
	code_mix[11] = br(ARM_NE, 11, 3);
	code_mix[13] = br(ARM_NE, 13, 0);

	init_sum64(code_sum64);
	code_sum64[2] = arm_ldr(0, 1, 3, 0);
	code_sum64[7] = br(ARM_NE, 7, 2);
	code_sum64[9] = br(ARM_NE, 9, 0);

	init_ramas(code_ramas);
	code_ramas[4] = arm_ldr(0, 1, 3, 0);
	code_ramas[6] = br(ARM_LS, 6, 8);
	code_ramas[9] = br(ARM_GE, 9, 11);
	code_ramas[13] = br(ARM_NE, 13, 4);
	code_ramas[15] = br(ARM_NE, 15, 0);

	init_bytes(code_bytes);
	code_bytes[3] = arm_ldr(1, 1, 3, 0);
	code_bytes[6] = arm_str(1, 5, 3, 0);
	code_bytes[11] = br(ARM_NE, 11, 3);
	code_bytes[13] = br(ARM_NE, 13, 0);
	loops_ready = 1;
}

const struct ja_loop ja_loops[] = {
	{ "mezcla", "suma, EOR con desplazamiento, AND y guardado (el de la 0.3)", code_mix, 15, 1 },
	{ "suma64", "ADDS + ADC (acarreo) y CMP/BNE", code_sum64, 11, 0 },
	{ "ramas", "dos CMP + salto por elemento, datos casi aleatorios", code_ramas, 17, 0 },
	{ "bytes", "LDRB/STRB con aritmética: 9 instrucciones por byte", code_bytes, 15, 0 },
};
const unsigned ja_nloops = sizeof ja_loops / sizeof ja_loops[0];

static uint32_t data_word(uint32_t i)
{
	return i * 2654435761u + 0x9E3779B9u;
}

void ja_loop_setup(struct ja_state *s, uint8_t *mem, uint32_t reps)
{
	ja_loops_init();
	memset(mem, 0, JA_MEM_SIZE);
	memset(s, 0, sizeof *s);
	for (uint32_t i = 0; i < JA_DATA_WORDS; i++)
		wr32le(mem + JA_DATA + 4 * i, data_word(i));
	s->mem = mem;
	s->r[6] = reps;
	s->budget = INT64_MAX / 2;
}

static uint32_t ja_out_sum_buf(const uint8_t *out)
{
	uint32_t sum = 0;
	for (uint32_t i = 0; i < 0x1000; i++)
		sum += (uint32_t)out[i] * (i + 1);
	return sum;
}

uint32_t ja_out_sum(const uint8_t *mem)
{
	return ja_out_sum_buf(mem + JA_OUT);
}

void ja_loop_expect(unsigned loop, uint32_t reps, struct ja_expect *e)
{
	memset(e, 0, sizeof *e);
	uint32_t sum = 0, hi = 0, cnt_hi = 0, cnt_neg = 0, acc = 0;
	uint8_t outb[0x1000];
	memset(outb, 0, sizeof outb);
	for (uint32_t rep = 0; rep < reps; rep++) {
		if (loop == 3) {
			for (uint32_t i = 0; i < 0x1000; i++) {
				const uint32_t d = (data_word(i >> 2) >> ((i & 3) * 8)) & 0xFF;
				const uint32_t v = (d + 1) ^ (acc << 1);
				outb[i] = (uint8_t)v;
				acc += v;
			}
			continue;
		}
		for (uint32_t i = 0; i < JA_DATA_WORDS; i++) {
			const uint32_t d = data_word(i);
			switch (loop) {
			case 0:
				sum += d;
				outb[4 * i] = (uint8_t)((sum ^ (d << 3)) & 0xFF);
				break;
			case 1: {
				const uint32_t old = sum;
				sum += d;
				hi += sum < old;
				break;
			}
			default:
				cnt_hi += d > 0x80000000u;
				cnt_neg += (int32_t)d < 0;
				break;
			}
		}
	}
	e->out_sum = ja_out_sum_buf(outb);
	switch (loop) {
	case 0:
		e->r0 = sum;
		break;
	case 1:
		e->r0 = sum;
		e->r7 = hi;
		e->out_sum = 0;
		break;
	case 2:
		e->r0 = cnt_hi;
		e->r4 = cnt_neg;
		e->out_sum = 0;
		break;
	default:
		e->r4 = acc;
		break;
	}
}
