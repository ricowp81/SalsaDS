/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * SalsaDS — Fase 0.3: intérprete ARM mínimo con tres despachos. Ver interp.h.
 */
#include "interp.h"

#include <string.h>

#include "util/endian.h"

const char *const arm_dispatch_name[DISPATCH_COUNT] = {
	"switch", "tabla de punteros a función", "goto calculado (enhebrado)",
};

enum kind { K_DPI, K_DPR, K_LS, K_B, K_SWI, K_UNDEF, K_COUNT };

static uint8_t kind_tbl[4096];
static uint16_t cond_mask[16]; /* bit nzcv (n<<3|z<<2|c<<1|v) -> cumple la condición */

#define KEY(w) ((((w) >> 16) & 0xFF0u) | (((w) >> 4) & 0xFu))

void arm_init(void)
{
	for (uint32_t k = 0; k < 4096; k++) {
		uint32_t hi = k >> 4, lo = k & 15; /* bits 27..20 y 7..4 */
		uint32_t top = hi >> 5;            /* bits 27..25 */
		int kind = K_UNDEF;

		if (top == 0) {
			if (!(lo & 1))
				kind = K_DPR;           /* registro con desplazamiento inmediato */
		} else if (top == 1) {
			kind = K_DPI;
		} else if (top == 2 || top == 3) {
			if (top == 2)
				kind = K_LS; /* 010: LDR/STR con desplazamiento inmediato */
		} else if (top == 5) {
			kind = K_B;
		} else if (top == 7 && (hi & 0x10)) {
			kind = K_SWI;
		}
		if (top == 0 && (lo & 1))
			kind = K_UNDEF;
		/* TST/TEQ/CMP/CMN sin S no son instrucciones de datos. */
		if ((top == 0 || top == 1) && ((hi >> 1) & 0xC) == 0x8 && !(hi & 1))
			kind = K_UNDEF;
		kind_tbl[k] = (uint8_t)kind;
	}
	for (uint32_t cc = 0; cc < 16; cc++) {
		uint16_t m = 0;
		for (uint32_t f = 0; f < 16; f++) {
			int n = f >> 3 & 1, z = f >> 2 & 1, c = f >> 1 & 1, v = f & 1, ok = 0;
			switch (cc) {
			case 0: ok = z; break;
			case 1: ok = !z; break;
			case 2: ok = c; break;
			case 3: ok = !c; break;
			case 4: ok = n; break;
			case 5: ok = !n; break;
			case 6: ok = v; break;
			case 7: ok = !v; break;
			case 8: ok = c && !z; break;
			case 9: ok = !c || z; break;
			case 10: ok = n == v; break;
			case 11: ok = n != v; break;
			case 12: ok = !z && n == v; break;
			case 13: ok = z || n != v; break;
			default: ok = 1; break;
			}
			if (ok)
				m |= (uint16_t)(1u << f);
		}
		cond_mask[cc] = m;
	}
}

/* ---- Operaciones comunes (iguales en los tres despachos) ------------------ */

static inline uint32_t ror32(uint32_t x, unsigned n)
{
	n &= 31;
	return n ? (x >> n) | (x << (32 - n)) : x;
}

/* Operando 2 por registro con desplazamiento inmediato; `*carry` = acarreo del desplazador. */
static inline uint32_t shift_imm(const struct arm_cpu *c, uint32_t w, uint32_t *carry)
{
	uint32_t rm = c->r[w & 15], amt = (w >> 7) & 31, cin = (c->flags >> 29) & 1;

	switch ((w >> 5) & 3) {
	case 0:
		if (!amt) {
			*carry = cin;
			return rm;
		}
		*carry = (rm >> (32 - amt)) & 1;
		return rm << amt;
	case 1:
		if (!amt) {
			*carry = rm >> 31;
			return 0;
		}
		*carry = (rm >> (amt - 1)) & 1;
		return rm >> amt;
	case 2:
		if (!amt) {
			*carry = rm >> 31;
			return (uint32_t)((int32_t)rm >> 31);
		}
		*carry = ((uint32_t)((int32_t)rm >> (amt - 1))) & 1;
		return (uint32_t)((int32_t)rm >> amt);
	default:
		if (!amt) { /* RRX */
			*carry = rm & 1;
			return (cin << 31) | (rm >> 1);
		}
		*carry = (rm >> (amt - 1)) & 1;
		return ror32(rm, amt);
	}
}

static inline uint32_t add_flags(uint32_t a, uint32_t b, uint32_t cin, uint32_t *fl)
{
	uint64_t wide = (uint64_t)a + b + cin;
	uint32_t r = (uint32_t)wide;
	uint32_t n = r >> 31, z = r == 0, cf = (uint32_t)(wide >> 32), v = (~(a ^ b) & (a ^ r)) >> 31;

	*fl = n << 31 | z << 30 | cf << 29 | v << 28;
	return r;
}

/* Instrucción de proceso de datos con operando 2 ya calculado. */
static inline void dp_exec(struct arm_cpu *c, uint32_t w, uint32_t op2, uint32_t shc)
{
	uint32_t op = (w >> 21) & 15, s = (w >> 20) & 1, rd = (w >> 12) & 15;
	uint32_t a = c->r[(w >> 16) & 15], res = 0, fl = c->flags, cin = (fl >> 29) & 1;
	int logical = 0, write = 1;

	switch (op) {
	case 0: res = a & op2; logical = 1; break;
	case 1: res = a ^ op2; logical = 1; break;
	case 2: res = add_flags(a, ~op2, 1, &fl); break;
	case 3: res = add_flags(op2, ~a, 1, &fl); break;
	case 4: res = add_flags(a, op2, 0, &fl); break;
	case 5: res = add_flags(a, op2, cin, &fl); break;
	case 6: res = add_flags(a, ~op2, cin, &fl); break;
	case 7: res = add_flags(op2, ~a, cin, &fl); break;
	case 8: res = a & op2; logical = 1; write = 0; break;
	case 9: res = a ^ op2; logical = 1; write = 0; break;
	case 10: res = add_flags(a, ~op2, 1, &fl); write = 0; break;
	case 11: res = add_flags(a, op2, 0, &fl); write = 0; break;
	case 12: res = a | op2; logical = 1; break;
	case 13: res = op2; logical = 1; break;
	case 14: res = a & ~op2; logical = 1; break;
	default: res = ~op2; logical = 1; break;
	}
	if (s) {
		if (logical)
			fl = (res & 0x80000000u) | (uint32_t)(res == 0) << 30 | shc << 29 | (c->flags & 0x10000000u);
		c->flags = fl;
	}
	if (write)
		c->r[rd] = res;
}

/* ---- Cuerpos de los manejadores (macros: PC es `pc` o `c->pc` según el despacho) */

#define BODY_DPI                                                                 \
	do {                                                                     \
		uint32_t rot_ = ((w >> 8) & 15) * 2, imm_ = ror32(w & 0xFF, rot_); \
		uint32_t shc_ = rot_ ? imm_ >> 31 : (c->flags >> 29) & 1;        \
		dp_exec(c, w, imm_, shc_);                                       \
	} while (0)

#define BODY_DPR                                                                 \
	do {                                                                     \
		uint32_t shc_, op2_ = shift_imm(c, w, &shc_);                    \
		dp_exec(c, w, op2_, shc_);                                       \
	} while (0)

#define BODY_LS                                                                  \
	do {                                                                     \
		uint32_t base_ = c->r[(w >> 16) & 15], off_ = w & 0xFFF;         \
		uint32_t addr_ = ((w >> 23) & 1 ? base_ + off_ : base_ - off_) % ARM_MEM_SIZE; \
		uint32_t rd_ = (w >> 12) & 15;                                   \
		if ((w >> 20) & 1)                                               \
			c->r[rd_] = (w >> 22) & 1 ? c->mem[addr_] : rd32le(c->mem + (addr_ & ~3u)); \
		else if ((w >> 22) & 1)                                          \
			c->mem[addr_] = (uint8_t)c->r[rd_];                      \
		else                                                             \
			wr32le(c->mem + (addr_ & ~3u), c->r[rd_]);              \
	} while (0)

/* pc ya apunta a la instrucción siguiente: destino = pc + 4 + offset * 4. */
#define BODY_B(PC)                                                               \
	do {                                                                     \
		int32_t off_ = (int32_t)(w << 8) >> 8;                           \
		if ((w >> 24) & 1)                                               \
			c->r[14] = (PC);                                         \
		(PC) = (PC) + 4 + (uint32_t)(off_ * 4);                          \
	} while (0)

#define FETCH(PC) rd32le(c->mem + ((PC) % ARM_MEM_SIZE))
#define COND_OK(w) ((cond_mask[(w) >> 28] >> (c->flags >> 28)) & 1)

/* ---- 1. switch ------------------------------------------------------------- */

static void run_switch(struct arm_cpu *c)
{
	uint32_t pc = c->pc;
	uint64_t n = 0;

	while (!c->halt) {
		uint32_t w = FETCH(pc);
		pc += 4;
		n++;
		if (!COND_OK(w))
			continue;
		switch (kind_tbl[KEY(w)]) {
		case K_DPI: BODY_DPI; break;
		case K_DPR: BODY_DPR; break;
		case K_LS: BODY_LS; break;
		case K_B: BODY_B(pc); break;
		case K_SWI: c->halt = 1; break;
		default: c->halt = 2; break;
		}
	}
	c->pc = pc;
	c->icount += n;
}

/* ---- 2. tabla de punteros a función ---------------------------------------- */

typedef void (*handler_fn)(struct arm_cpu *c, uint32_t w);

static void h_dpi(struct arm_cpu *c, uint32_t w) { BODY_DPI; }
static void h_dpr(struct arm_cpu *c, uint32_t w) { BODY_DPR; }
static void h_ls(struct arm_cpu *c, uint32_t w) { BODY_LS; }
static void h_b(struct arm_cpu *c, uint32_t w) { BODY_B(c->pc); }
static void h_swi(struct arm_cpu *c, uint32_t w) { (void)w; c->halt = 1; }
static void h_undef(struct arm_cpu *c, uint32_t w) { (void)w; c->halt = 2; }

static const handler_fn handler_tbl[K_COUNT] = { h_dpi, h_dpr, h_ls, h_b, h_swi, h_undef };

static void run_call(struct arm_cpu *c)
{
	uint64_t n = 0;

	while (!c->halt) {
		uint32_t w = FETCH(c->pc);
		c->pc += 4;
		n++;
		if (!COND_OK(w))
			continue;
		handler_tbl[kind_tbl[KEY(w)]](c, w);
	}
	c->icount += n;
}

/* ---- 3. goto calculado ------------------------------------------------------ */

static void run_goto(struct arm_cpu *c)
{
	static const void *const labels[K_COUNT] = { &&l_dpi, &&l_dpr, &&l_ls, &&l_b, &&l_swi, &&l_undef };
	uint32_t pc = c->pc, w;
	uint64_t n = 0;

/* Siguiente instrucción: búsqueda, condición y salto directo al manejador. */
#define NEXT()                                                                   \
	do {                                                                     \
		w = FETCH(pc);                                                   \
		pc += 4;                                                         \
		n++;                                                             \
		if (__builtin_expect(!COND_OK(w), 0))                            \
			goto skip;                                               \
		goto *labels[kind_tbl[KEY(w)]];                                  \
	} while (0)

	if (c->halt)
		return;
	NEXT();
skip:
	NEXT();
l_dpi:
	BODY_DPI;
	NEXT();
l_dpr:
	BODY_DPR;
	NEXT();
l_ls:
	BODY_LS;
	NEXT();
l_b:
	BODY_B(pc);
	NEXT();
l_swi:
	c->halt = 1;
	goto done;
l_undef:
	c->halt = 2;
done:
	c->pc = pc;
	c->icount += n;
}

void arm_run(struct arm_cpu *c, enum arm_dispatch how)
{
	switch (how) {
	case DISPATCH_SWITCH: run_switch(c); break;
	case DISPATCH_CALL: run_call(c); break;
	default: run_goto(c); break;
	}
}

/* ---- Programa de prueba ----------------------------------------------------- */

#define AL 0xEu
#define NE 0x1u
#define OP_AND 0u
#define OP_EOR 1u
#define OP_ADD 4u
#define OP_CMP 10u
#define OP_MOV 13u
#define OP_SUB 2u
#define DP_IMM(cond, op, s, rn, rd, rot, imm8) \
	((cond) << 28 | 1u << 25 | (op) << 21 | (s) << 20 | (rn) << 16 | (rd) << 12 | (rot) << 8 | (imm8))
#define DP_REG(cond, op, s, rn, rd, shamt, rm) \
	((cond) << 28 | (op) << 21 | (s) << 20 | (rn) << 16 | (rd) << 12 | (shamt) << 7 | (rm))
#define LDR_IMM(rn, rd, off) (AL << 28 | 1u << 26 | 1u << 24 | 1u << 23 | 1u << 20 | (rn) << 16 | (rd) << 12 | (off))
#define STR_IMM(rn, rd, off) (AL << 28 | 1u << 26 | 1u << 24 | 1u << 23 | (rn) << 16 | (rd) << 12 | (off))
#define B_(cond, off) ((cond) << 28 | 0xAu << 24 | ((uint32_t)(off) & 0xFFFFFFu))
#define SWI_ 0xEF000000u

#define DATA_BASE 0x1000u
#define OUT_BASE (DATA_BASE + ARM_ARRAY_WORDS * 4)
#define INSTR_PER_ELEM 9u
#define INSTR_PER_REP (3u + INSTR_PER_ELEM * ARM_ARRAY_WORDS + 2u)

static uint32_t data_word(uint32_t i)
{
	return i * 2654435761u + 0x9E3779B9u;
}

void arm_load_program(struct arm_cpu *c, uint8_t *mem, uint32_t reps)
{
	static const uint32_t prog[] = {
		/* outer: */
		DP_IMM(AL, OP_MOV, 0, 0, 1, 12, 0x10),   /* 00 MOV r1,#0x1000 */
		DP_IMM(AL, OP_ADD, 0, 1, 2, 12, 0x10),   /* 04 ADD r2,r1,#0x1000 */
		DP_REG(AL, OP_MOV, 0, 0, 5, 0, 2),       /* 08 MOV r5,r2 */
		/* loop: */
		LDR_IMM(1, 3, 0),                        /* 0C LDR r3,[r1] */
		DP_REG(AL, OP_ADD, 0, 0, 0, 0, 3),       /* 10 ADD r0,r0,r3 */
		DP_REG(AL, OP_EOR, 0, 0, 4, 3, 3),       /* 14 EOR r4,r0,r3,LSL #3 */
		DP_IMM(AL, OP_AND, 0, 4, 4, 0, 0xFF),    /* 18 AND r4,r4,#0xFF */
		STR_IMM(5, 4, 0),                        /* 1C STR r4,[r5] */
		DP_IMM(AL, OP_ADD, 0, 1, 1, 0, 4),       /* 20 ADD r1,r1,#4 */
		DP_IMM(AL, OP_ADD, 0, 5, 5, 0, 4),       /* 24 ADD r5,r5,#4 */
		DP_REG(AL, OP_CMP, 1, 1, 0, 0, 2),       /* 28 CMP r1,r2 */
		B_(NE, -10),                             /* 2C BNE loop */
		DP_IMM(AL, OP_SUB, 1, 6, 6, 0, 1),       /* 30 SUBS r6,r6,#1 */
		B_(NE, -15),                             /* 34 BNE outer */
		SWI_,                                    /* 38 SWI */
	};

	memset(mem, 0, ARM_MEM_SIZE);
	memset(c, 0, sizeof *c);
	for (uint32_t i = 0; i < sizeof prog / sizeof prog[0]; i++)
		wr32le(mem + 4 * i, prog[i]);
	for (uint32_t i = 0; i < ARM_ARRAY_WORDS; i++)
		wr32le(mem + DATA_BASE + 4 * i, data_word(i));
	c->mem = mem;
	c->r[6] = reps;
}

void arm_expected(uint32_t reps, uint32_t *r0, uint32_t *out_sum)
{
	uint32_t sum = 0, outs = 0;

	for (uint32_t rep = 0; rep < reps; rep++) {
		outs = 0;
		for (uint32_t i = 0; i < ARM_ARRAY_WORDS; i++) {
			uint32_t d = data_word(i);
			sum += d;
			outs += ((sum ^ (d << 3)) & 0xFF) * (i + 1);
		}
	}
	*r0 = sum;
	*out_sum = outs;
}

uint32_t arm_out_sum(const struct arm_cpu *c)
{
	uint32_t outs = 0;

	for (uint32_t i = 0; i < ARM_ARRAY_WORDS; i++)
		outs += rd32le(c->mem + OUT_BASE + 4 * i) * (i + 1);
	return outs;
}

uint64_t arm_expected_icount(uint32_t reps)
{
	return (uint64_t)reps * INSTR_PER_REP + 1;
}
