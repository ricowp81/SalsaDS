/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * SalsaDS — intérprete de referencia del subconjunto ARM del JIT (Fase 0.7).
 * Banderas completas; es la verdad contra la que se compara el código
 * traducido, y un intérprete más para medir.
 */
#include <string.h>

#include "jit/armenc.h"
#include "jit/jit_arm.h"
#include "util/endian.h"

static inline uint32_t ror32(uint32_t x, unsigned n)
{
	n &= 31;
	return n ? (x >> n) | (x << (32 - n)) : x;
}

static int cond_ok(unsigned cc, uint32_t f)
{
	const int n = f >> 31 & 1, z = f >> 30 & 1, c = f >> 29 & 1, v = f >> 28 & 1;
	switch (cc) {
	case ARM_EQ: return z;
	case ARM_NE: return !z;
	case ARM_CS: return c;
	case ARM_CC: return !c;
	case ARM_MI: return n;
	case ARM_PL: return !n;
	case ARM_VS: return v;
	case ARM_VC: return !v;
	case ARM_HI: return c && !z;
	case ARM_LS: return !c || z;
	case ARM_GE: return n == v;
	case ARM_LT: return n != v;
	case ARM_GT: return !z && n == v;
	case ARM_LE: return z || n != v;
	default: return 1;
	}
}

static uint32_t add_f(uint32_t a, uint32_t b, uint32_t cin, uint32_t *fl)
{
	const uint64_t w = (uint64_t)a + b + cin;
	const uint32_t r = (uint32_t)w;
	*fl = (r >> 31) << 31 | (uint32_t)(r == 0) << 30 | (uint32_t)(w >> 32) << 29 | ((~(a ^ b) & (a ^ r)) >> 31) << 28;
	return r;
}

void ja_ref_run(struct ja_state *s, const uint32_t *prog, uint32_t nwords)
{
	uint32_t pc = 0;
	s->taken = 0;
	for (;;) {
		if (pc >= nwords * 4u) {
			s->ret = JA_EXIT_BAD;
			break;
		}
		if (s->budget <= 0) {
			s->ret = JA_EXIT_BUDGET;
			break;
		}
		const uint32_t w = prog[pc >> 2];
		s->budget--;
		s->pc = pc;
		const unsigned cc = w >> 28;
		const unsigned top = (w >> 25) & 7;
		if (top == 5) {
			if (cond_ok(cc, s->flags)) {
				int32_t off = (int32_t)(w << 8) >> 8;
				pc = pc + 8 + (uint32_t)(off * 4);
				s->taken++;
			} else {
				pc += 4;
			}
			continue;
		}
		if (top == 7 && (w & 0x0F000000u) == 0x0F000000u) {
			s->ret = JA_EXIT_SWI;
			break;
		}
		if (!cond_ok(cc, s->flags)) {
			pc += 4;
			continue;
		}
		if (top == 2) {  /* LDR/STR/LDRB/STRB, desplazamiento inmediato positivo */
			const unsigned rn = (w >> 16) & 15, rd = (w >> 12) & 15, byte = (w >> 22) & 1, load = (w >> 20) & 1;
			const uint32_t addr = (s->r[rn] + (w & 0xFFF)) & (JA_MEM_SIZE - 1);
			if (load)
				s->r[rd] = byte ? s->mem[addr] : rd32le(s->mem + (addr & ~3u));
			else if (byte)
				s->mem[addr] = (uint8_t)s->r[rd];
			else
				wr32le(s->mem + (addr & ~3u), s->r[rd]);
			pc += 4;
			continue;
		}
		/* datos */
		const unsigned op = (w >> 21) & 15, sbit = (w >> 20) & 1, rn = (w >> 16) & 15, rd = (w >> 12) & 15;
		uint32_t op2, shc = (s->flags >> 29) & 1;
		if (top == 1) {
			op2 = ror32(w & 0xFF, ((w >> 8) & 15) * 2);
		} else {
			const uint32_t rm = s->r[w & 15], amt = (w >> 7) & 31;
			switch ((w >> 5) & 3) {
			case ARM_LSL: op2 = amt ? rm << amt : rm; break;
			case ARM_LSR: op2 = amt ? rm >> amt : 0; break;
			case ARM_ASR: op2 = amt ? (uint32_t)((int32_t)rm >> amt) : (uint32_t)((int32_t)rm >> 31); break;
			default: op2 = ror32(rm, amt); break;
			}
		}
		(void)shc;
		const uint32_t a = s->r[rn];
		const uint32_t cin = (s->flags >> 29) & 1;
		uint32_t res = 0, fl = s->flags;
		int write = 1, arith = 0;
		switch (op) {
		case ARM_AND: res = a & op2; break;
		case ARM_EOR: res = a ^ op2; break;
		case ARM_ORR: res = a | op2; break;
		case ARM_MOV: res = op2; break;
		case ARM_ADD: res = add_f(a, op2, 0, &fl); arith = 1; break;
		case ARM_ADC: res = add_f(a, op2, cin, &fl); arith = 1; break;
		case ARM_SUB: res = add_f(a, ~op2, 1, &fl); arith = 1; break;
		case ARM_CMP: res = add_f(a, ~op2, 1, &fl); arith = 1; write = 0; break;
		default: s->ret = JA_EXIT_BAD; return;
		}
		if (write)
			s->r[rd] = res;
		if (sbit) {
			if (arith)
				s->flags = fl;
			else
				s->flags = (s->flags & 0x3FFFFFFFu) | (res & 0x80000000u) | (uint32_t)(res == 0) << 30;
		}
		pc += 4;
	}
}
