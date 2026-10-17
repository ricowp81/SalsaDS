/*
 * SalsaDS — máquina de pruebas para los intérpretes de CPU (src/cpu).
 *
 * Una memoria plana de 1 MB (espejada) en little-endian con bus de
 * callbacks que registra cada acceso (dirección, tamaño, lectura o
 * escritura) para comprobar la alineación que ve el mapa de memoria, con
 * abortos configurables, y los codificadores de instrucciones ARM y Thumb
 * que usan las pruebas.
 */
#ifndef SALSADS_TESTS_ACORE_TM_H
#define SALSADS_TESTS_ACORE_TM_H

#include <string.h>

#include "check.h"
#include "cpu/acore.h"
#include "util/endian.h"

#define TM_MEM 0x100000u
#define TM_CODE 0x1000u /* donde se pone el código de cada prueba */
#define TM_DATA 0x8000u /* zona de datos */

struct tm_access {
	uint32_t addr;
	uint8_t size; /* 1, 2, 4 */
	uint8_t wr;
	uint8_t fetch;
};

struct tm {
	struct acore c;
	uint8_t mem[TM_MEM];
	uint8_t itcm[ACORE_ITCM_SIZE], dtcm[ACORE_DTCM_SIZE];
	uint32_t abort_lo, abort_hi;  /* acceso de datos en [lo, hi) aborta */
	uint32_t fetch_abort;         /* captación en esta dirección aborta (0 = ninguna) */
	struct tm_access log[64];
	int nlog;
	int swi_calls;
	uint32_t swi_last;
	int swi_handle;
	uint32_t code_base;           /* dirección del código de las pruebas (TM_CODE; otra si hay TCM en esa zona) */
};

/* ---- bus ----------------------------------------------------------------- */

static inline void tm_log(struct tm *t, uint32_t a, int size, int wr, int fetch)
{
	if (t->nlog < 64) {
		t->log[t->nlog].addr = a;
		t->log[t->nlog].size = (uint8_t)size;
		t->log[t->nlog].wr = (uint8_t)wr;
		t->log[t->nlog].fetch = (uint8_t)fetch;
	}
	t->nlog++;
}

static inline int tm_data_abort(struct tm *t, uint32_t a)
{
	if (a >= t->abort_lo && a < t->abort_hi) {
		acore_signal_abort(&t->c, ACORE_ABORT_DATA);
		return 1;
	}
	return 0;
}

static inline uint32_t tm_r8(struct acore *c, uint32_t a)
{
	struct tm *t = c->user;
	tm_log(t, a, 1, 0, 0);
	if (tm_data_abort(t, a))
		return 0xDEAD;
	return t->mem[a & (TM_MEM - 1)];
}
static inline uint32_t tm_r16(struct acore *c, uint32_t a)
{
	struct tm *t = c->user;
	tm_log(t, a, 2, 0, 0);
	if (tm_data_abort(t, a))
		return 0xDEAD;
	return rd16le(&t->mem[a & (TM_MEM - 1)]);
}
static inline uint32_t tm_r32(struct acore *c, uint32_t a)
{
	struct tm *t = c->user;
	tm_log(t, a, 4, 0, 0);
	if (tm_data_abort(t, a))
		return 0xDEADBEEF;
	return rd32le(&t->mem[a & (TM_MEM - 1)]);
}
static inline void tm_w8(struct acore *c, uint32_t a, uint32_t v)
{
	struct tm *t = c->user;
	tm_log(t, a, 1, 1, 0);
	if (tm_data_abort(t, a))
		return;
	t->mem[a & (TM_MEM - 1)] = (uint8_t)v;
}
static inline void tm_w16(struct acore *c, uint32_t a, uint32_t v)
{
	struct tm *t = c->user;
	tm_log(t, a, 2, 1, 0);
	if (tm_data_abort(t, a))
		return;
	wr16le(&t->mem[a & (TM_MEM - 1)], (uint16_t)v);
}
static inline void tm_w32(struct acore *c, uint32_t a, uint32_t v)
{
	struct tm *t = c->user;
	tm_log(t, a, 4, 1, 0);
	if (tm_data_abort(t, a))
		return;
	wr32le(&t->mem[a & (TM_MEM - 1)], v);
}
static inline uint32_t tm_f16(struct acore *c, uint32_t a)
{
	struct tm *t = c->user;
	if (t->fetch_abort && a == t->fetch_abort) {
		acore_signal_abort(c, ACORE_ABORT_PREFETCH);
		return 0;
	}
	return rd16le(&t->mem[a & (TM_MEM - 1)]);
}
static inline uint32_t tm_f32(struct acore *c, uint32_t a)
{
	struct tm *t = c->user;
	if (t->fetch_abort && a == t->fetch_abort) {
		acore_signal_abort(c, ACORE_ABORT_PREFETCH);
		return 0;
	}
	return rd32le(&t->mem[a & (TM_MEM - 1)]);
}

static const struct acore_bus tm_bus = {
	tm_r8, tm_r16, tm_r32, tm_w8, tm_w16, tm_w32, tm_f16, tm_f32,
};

static inline int tm_swi_hook(struct acore *c, uint32_t comment)
{
	struct tm *t = c->user;

	t->swi_calls++;
	t->swi_last = comment;
	return t->swi_handle;
}

/* Arranca una máquina: modo sistema con I y F puestos, ARM, PC en TM_CODE, registros a 0. */
static inline void tm_init(struct tm *t, enum acore_kind kind)
{
	memset(t, 0, sizeof *t);
	acore_init(&t->c, kind, &tm_bus, t);
	if (kind == ACORE_ARM9)
		acore_attach_tcm(&t->c, t->itcm, t->dtcm);
	t->c.swi_hook = tm_swi_hook;
	acore_set_cpsr(&t->c, ACORE_MODE_SYS | ACORE_I | ACORE_F);
	for (int i = 0; i < 16; i++)
		t->c.r[i] = 0;
	t->c.vector_base = 0; /* los vectores de las pruebas viven en la memoria plana, en 0 */
	t->code_base = TM_CODE;
	acore_set_pc(&t->c, TM_CODE);
}

/* Reinicio barato (sin tocar la memoria): modo sistema, ARM, registros y bancos a 0, PC en TM_CODE. */
static inline void tm_soft(struct tm *t)
{
	struct acore *c = &t->c;

	c->cpsr &= ~ACORE_T;
	acore_set_cpsr(c, ACORE_MODE_SYS | ACORE_I | ACORE_F);
	memset(c->r, 0, sizeof c->r);
	memset(c->sp_bank, 0, sizeof c->sp_bank);
	memset(c->lr_bank, 0, sizeof c->lr_bank);
	memset(c->spsr_bank, 0, sizeof c->spsr_bank);
	memset(c->fiq_r8_12, 0, sizeof c->fiq_r8_12);
	memset(c->usr_r8_12, 0, sizeof c->usr_r8_12);
	c->abort = 0;
	c->halted = c->irq = c->fiq = c->attn = 0;
	c->cycles = c->icount = 0;
	t->abort_lo = t->abort_hi = 0;
	t->fetch_abort = 0;
	t->nlog = 0;
	acore_set_pc(c, t->code_base);
}

static inline void tm_clear_log(struct tm *t)
{
	t->nlog = 0;
}

static inline void tm_put32(struct tm *t, uint32_t a, uint32_t v)
{
	wr32le(&t->mem[a & (TM_MEM - 1)], v);
}
static inline uint32_t tm_get32(struct tm *t, uint32_t a)
{
	return rd32le(&t->mem[a & (TM_MEM - 1)]);
}
static inline void tm_put16(struct tm *t, uint32_t a, uint32_t v)
{
	wr16le(&t->mem[a & (TM_MEM - 1)], (uint16_t)v);
}
static inline uint32_t tm_get16(struct tm *t, uint32_t a)
{
	return rd16le(&t->mem[a & (TM_MEM - 1)]);
}

/* Pone una palabra de código en TM_CODE + 4*i. */
static inline void tm_code(struct tm *t, unsigned i, uint32_t insn)
{
	tm_put32(t, t->code_base + 4 * i, insn);
}
/* Media palabra de código Thumb en TM_CODE + 2*i. */
static inline void tm_code16(struct tm *t, unsigned i, uint32_t insn)
{
	tm_put16(t, t->code_base + 2 * i, insn);
}

/* Ejecuta n instrucciones desde el PC actual. */
static inline void tm_steps(struct tm *t, int n)
{
	for (int i = 0; i < n; i++)
		acore_step(&t->c);
}

/* Pone `insn` en TM_CODE, apunta el PC ahí en estado ARM y ejecuta una instrucción. */
static inline void tm_exec(struct tm *t, uint32_t insn)
{
	t->c.cpsr &= ~ACORE_T;
	tm_code(t, 0, insn);
	acore_set_pc(&t->c, t->code_base);
	acore_step(&t->c);
}

/* Igual en Thumb. */
static inline void tm_exec16(struct tm *t, uint32_t insn)
{
	t->c.cpsr |= ACORE_T;
	tm_code16(t, 0, insn);
	acore_set_pc(&t->c, t->code_base);
	acore_step(&t->c);
}

static inline uint32_t tm_flags(const struct tm *t)
{
	return t->c.cpsr >> 28;
}
static inline void tm_set_flags(struct tm *t, uint32_t nzcv)
{
	t->c.cpsr = (t->c.cpsr & 0x0FFFFFFFu) | (nzcv << 28);
}

/* ---- codificadores ARM ------------------------------------------------------ */

enum { OP_AND, OP_EOR, OP_SUB, OP_RSB, OP_ADD, OP_ADC, OP_SBC, OP_RSC, OP_TST, OP_TEQ, OP_CMP, OP_CMN, OP_ORR, OP_MOV, OP_BIC, OP_MVN };
enum { LSL, LSR, ASR, ROR };
enum { CC_EQ, CC_NE, CC_CS, CC_CC, CC_MI, CC_PL, CC_VS, CC_VC, CC_HI, CC_LS, CC_GE, CC_LT, CC_GT, CC_LE, CC_AL, CC_NV };

#define AL 0xE0000000u

static inline uint32_t a_dp_i(unsigned op, unsigned s, unsigned rn, unsigned rd, unsigned imm8, unsigned rot)
{
	return AL | 0x02000000u | op << 21 | s << 20 | rn << 16 | rd << 12 | rot << 8 | imm8;
}
static inline uint32_t a_dp_rs(unsigned op, unsigned s, unsigned rn, unsigned rd, unsigned rm, unsigned type, unsigned amt)
{
	return AL | op << 21 | s << 20 | rn << 16 | rd << 12 | amt << 7 | type << 5 | rm;
}
static inline uint32_t a_dp_rr(unsigned op, unsigned s, unsigned rn, unsigned rd, unsigned rm, unsigned type, unsigned rs)
{
	return AL | op << 21 | s << 20 | rn << 16 | rd << 12 | rs << 8 | type << 5 | 0x10u | rm;
}
static inline uint32_t a_mov_i(unsigned rd, unsigned imm8) { return a_dp_i(OP_MOV, 0, 0, rd, imm8, 0); }
static inline uint32_t a_mov(unsigned rd, unsigned rm) { return a_dp_rs(OP_MOV, 0, 0, rd, rm, LSL, 0); }

static inline uint32_t a_mul(unsigned s, unsigned rd, unsigned rm, unsigned rs)
{
	return AL | s << 20 | rd << 16 | rs << 8 | 0x90u | rm;
}
static inline uint32_t a_mla(unsigned s, unsigned rd, unsigned rm, unsigned rs, unsigned rn)
{
	return AL | 1u << 21 | s << 20 | rd << 16 | rn << 12 | rs << 8 | 0x90u | rm;
}
/* signed, acc: bits 22 y 21 */
static inline uint32_t a_mull(unsigned sgn, unsigned acc, unsigned s, unsigned rdhi, unsigned rdlo, unsigned rm, unsigned rs)
{
	return AL | 1u << 23 | sgn << 22 | acc << 21 | s << 20 | rdhi << 16 | rdlo << 12 | rs << 8 | 0x90u | rm;
}

/* LDR/STR: p pre, u arriba, b byte, w writeback, l carga */
static inline uint32_t a_ls_i(unsigned l, unsigned b, unsigned p, unsigned u, unsigned w, unsigned rn, unsigned rd, unsigned imm12)
{
	return AL | 0x04000000u | p << 24 | u << 23 | b << 22 | w << 21 | l << 20 | rn << 16 | rd << 12 | imm12;
}
static inline uint32_t a_ls_r(unsigned l, unsigned b, unsigned p, unsigned u, unsigned w, unsigned rn, unsigned rd,
			      unsigned rm, unsigned type, unsigned amt)
{
	return AL | 0x06000000u | p << 24 | u << 23 | b << 22 | w << 21 | l << 20 | rn << 16 | rd << 12 | amt << 7 |
	       type << 5 | rm;
}
/* media palabra / con signo / doble: sh = bits 6:5 (1 = H, 2 = SB, 3 = SH); l carga; con inmediato de 8 bits */
static inline uint32_t a_hs_i(unsigned l, unsigned sh, unsigned p, unsigned u, unsigned w, unsigned rn, unsigned rd, unsigned imm8)
{
	return AL | p << 24 | u << 23 | 1u << 22 | w << 21 | l << 20 | rn << 16 | rd << 12 | (imm8 >> 4) << 8 | 0x90u |
	       sh << 5 | (imm8 & 15);
}
static inline uint32_t a_hs_r(unsigned l, unsigned sh, unsigned p, unsigned u, unsigned w, unsigned rn, unsigned rd, unsigned rm)
{
	return AL | p << 24 | u << 23 | w << 21 | l << 20 | rn << 16 | rd << 12 | 0x90u | sh << 5 | rm;
}
static inline uint32_t a_swp(unsigned b, unsigned rn, unsigned rd, unsigned rm)
{
	return AL | 0x01000000u | b << 22 | rn << 16 | rd << 12 | 0x90u | rm;
}
/* LDM/STM */
static inline uint32_t a_block(unsigned l, unsigned p, unsigned u, unsigned s, unsigned w, unsigned rn, unsigned list)
{
	return AL | 0x08000000u | p << 24 | u << 23 | s << 22 | w << 21 | l << 20 | rn << 16 | list;
}
#define A_LDMIA(w, rn, list) a_block(1, 0, 1, 0, w, rn, list)
#define A_STMIA(w, rn, list) a_block(0, 0, 1, 0, w, rn, list)
#define A_LDMIB(w, rn, list) a_block(1, 1, 1, 0, w, rn, list)
#define A_STMIB(w, rn, list) a_block(0, 1, 1, 0, w, rn, list)
#define A_LDMDA(w, rn, list) a_block(1, 0, 0, 0, w, rn, list)
#define A_STMDA(w, rn, list) a_block(0, 0, 0, 0, w, rn, list)
#define A_LDMDB(w, rn, list) a_block(1, 1, 0, 0, w, rn, list)
#define A_STMDB(w, rn, list) a_block(0, 1, 0, 0, w, rn, list)

static inline uint32_t a_b(unsigned l, int32_t off_words)
{
	return AL | 0x0A000000u | l << 24 | ((uint32_t)off_words & 0x00FFFFFFu);
}
static inline uint32_t a_bx(unsigned rm) { return AL | 0x012FFF10u | rm; }
static inline uint32_t a_blx_r(unsigned rm) { return AL | 0x012FFF30u | rm; }
static inline uint32_t a_blx_i(int32_t off_words, unsigned h) { return 0xFA000000u | h << 24 | ((uint32_t)off_words & 0x00FFFFFFu); }
static inline uint32_t a_swi(uint32_t n) { return AL | 0x0F000000u | n; }
static inline uint32_t a_mrs(unsigned spsr, unsigned rd) { return AL | 0x010F0000u | spsr << 22 | rd << 12; }
static inline uint32_t a_msr_r(unsigned spsr, unsigned mask, unsigned rm) { return AL | 0x0120F000u | spsr << 22 | mask << 16 | rm; }
static inline uint32_t a_msr_i(unsigned spsr, unsigned mask, unsigned imm8, unsigned rot)
{
	return AL | 0x0320F000u | spsr << 22 | mask << 16 | rot << 8 | imm8;
}
static inline uint32_t a_clz(unsigned rd, unsigned rm) { return AL | 0x016F0F10u | rd << 12 | rm; }
/* QADD(0) QSUB(1) QDADD(2) QDSUB(3): Rd = Rm op Rn */
static inline uint32_t a_q(unsigned op, unsigned rd, unsigned rm, unsigned rn)
{
	return AL | 0x01000050u | op << 21 | rn << 16 | rd << 12 | rm;
}
/* familia 16 bits: fam 0 SMLAxy, 1 SMLAW/SMULW, 2 SMLALxy, 3 SMULxy */
static inline uint32_t a_smul(unsigned fam, unsigned x, unsigned y, unsigned rd, unsigned rn, unsigned rs, unsigned rm)
{
	return AL | 0x01000080u | fam << 21 | rd << 16 | rn << 12 | rs << 8 | y << 6 | x << 5 | rm;
}
static inline uint32_t a_mcr(unsigned cp, unsigned op1, unsigned rd, unsigned crn, unsigned crm, unsigned op2)
{
	return AL | 0x0E000010u | op1 << 21 | crn << 16 | rd << 12 | cp << 8 | op2 << 5 | crm;
}
static inline uint32_t a_mrc(unsigned cp, unsigned op1, unsigned rd, unsigned crn, unsigned crm, unsigned op2)
{
	return a_mcr(cp, op1, rd, crn, crm, op2) | 1u << 20;
}
#define A_NOP a_mov(0, 0)
#define A_UNDEF 0xE7F000F0u /* UDF */

/* ---- codificadores Thumb ------------------------------------------------------ */

static inline uint32_t t_shift(unsigned type, unsigned rd, unsigned rs, unsigned imm5)
{
	return type << 11 | imm5 << 6 | rs << 3 | rd;
}
static inline uint32_t t_addsub(unsigned sub, unsigned imm, unsigned rd, unsigned rs, unsigned rn_or_imm3)
{
	return 0x1800u | imm << 10 | sub << 9 | rn_or_imm3 << 6 | rs << 3 | rd;
}
/* op: 0 MOV, 1 CMP, 2 ADD, 3 SUB */
static inline uint32_t t_imm8(unsigned op, unsigned rd, unsigned imm8) { return 0x2000u | op << 11 | rd << 8 | imm8; }
static inline uint32_t t_alu(unsigned op, unsigned rd, unsigned rs) { return 0x4000u | op << 6 | rs << 3 | rd; }
/* op: 0 ADD, 1 CMP, 2 MOV, 3 BX/BLX(l=1 en rd bit 3) */
static inline uint32_t t_hi(unsigned op, unsigned rd, unsigned rs)
{
	return 0x4400u | op << 8 | (rd >> 3) << 7 | (rs & 15) << 3 | (rd & 7);
}
static inline uint32_t t_bx(unsigned rm) { return 0x4700u | rm << 3; }
static inline uint32_t t_blx(unsigned rm) { return 0x4780u | rm << 3; }
static inline uint32_t t_ldr_pc(unsigned rd, unsigned imm8) { return 0x4800u | rd << 8 | imm8; }
/* op3: 0 STR 1 STRH 2 STRB 3 LDRSB 4 LDR 5 LDRH 6 LDRB 7 LDRSH */
static inline uint32_t t_ls_r(unsigned op3, unsigned rd, unsigned rb, unsigned ro) { return 0x5000u | op3 << 9 | ro << 6 | rb << 3 | rd; }
/* op2: 0 STR 1 LDR 2 STRB 3 LDRB */
static inline uint32_t t_ls_i(unsigned op2, unsigned rd, unsigned rb, unsigned imm5) { return 0x6000u | op2 << 11 | imm5 << 6 | rb << 3 | rd; }
static inline uint32_t t_ls_h(unsigned l, unsigned rd, unsigned rb, unsigned imm5) { return 0x8000u | l << 11 | imm5 << 6 | rb << 3 | rd; }
static inline uint32_t t_ls_sp(unsigned l, unsigned rd, unsigned imm8) { return 0x9000u | l << 11 | rd << 8 | imm8; }
static inline uint32_t t_add_pcsp(unsigned sp, unsigned rd, unsigned imm8) { return 0xA000u | sp << 11 | rd << 8 | imm8; }
static inline uint32_t t_sp_adj(unsigned sub, unsigned imm7) { return 0xB000u | sub << 7 | imm7; }
static inline uint32_t t_push(unsigned r, unsigned list) { return 0xB400u | r << 8 | list; }
static inline uint32_t t_pop(unsigned r, unsigned list) { return 0xBC00u | r << 8 | list; }
static inline uint32_t t_block(unsigned l, unsigned rb, unsigned list) { return 0xC000u | l << 11 | rb << 8 | list; }
static inline uint32_t t_bcond(unsigned cond, int off8) { return 0xD000u | cond << 8 | ((uint32_t)off8 & 0xFF); }
static inline uint32_t t_swi(unsigned n) { return 0xDF00u | n; }
static inline uint32_t t_b(int off11) { return 0xE000u | ((uint32_t)off11 & 0x7FF); }
static inline uint32_t t_blx_lo(unsigned off11) { return 0xE800u | off11; }
static inline uint32_t t_bl_hi(int off11) { return 0xF000u | ((uint32_t)off11 & 0x7FF); }
static inline uint32_t t_bl_lo(unsigned off11) { return 0xF800u | off11; }
static inline uint32_t t_bkpt(unsigned n) { return 0xBE00u | n; }

#endif
