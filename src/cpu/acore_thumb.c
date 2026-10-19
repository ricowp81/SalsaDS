/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * SalsaDS — juego de instrucciones Thumb (ARMv4T y ARMv5TE) del intérprete.
 *
 * Despacho por los bits 15..6 (1024 entradas por versión). Durante un
 * manejador, r[15] vale la dirección de la instrucción + 4 (lo que lee el
 * programa); un salto deja r[15] = destino + 2 (ver acore.h).
 *
 * Las dos mitades del BL son instrucciones independientes (la primera deja
 * en r14 la dirección parcial). Código propio, escrito desde el manual de
 * arquitectura de ARM y GBATEK.
 */
#include "cpu/acore_priv.h"

acore_op_fn acore_thumb_table[2][1024];

static void th_undef(struct acore *c, uint32_t insn)
{
	(void)insn;
	acore_undefined(c);
}

/* ---- formato 1: desplazamiento por inmediato ------------------------------------- */

ALWAYS_INLINE void th_shift_imm(struct acore *c, uint32_t insn, const unsigned type)
{
	uint32_t cf;
	const uint32_t r = shift_by_imm(type, c->r[(insn >> 3) & 7], (insn >> 6) & 31, (c->cpsr >> 29) & 1, &cf);

	c->r[insn & 7] = r;
	c->cpsr = (c->cpsr & ~(ACORE_N | ACORE_Z | ACORE_C)) | (r & ACORE_N) | (r == 0 ? ACORE_Z : 0u) |
		  (cf ? ACORE_C : 0u);
}

static void th_lsl_i(struct acore *c, uint32_t insn) { th_shift_imm(c, insn, 0); }
static void th_lsr_i(struct acore *c, uint32_t insn) { th_shift_imm(c, insn, 1); }
static void th_asr_i(struct acore *c, uint32_t insn) { th_shift_imm(c, insn, 2); }

/* ---- formato 2: suma y resta con registro o inmediato de 3 bits ---------------------- */

static void th_addsub(struct acore *c, uint32_t insn)
{
	const uint32_t a = c->r[(insn >> 3) & 7];
	const uint32_t b = (insn & (1u << 10)) ? (insn >> 6) & 7 : c->r[(insn >> 6) & 7];
	uint32_t cf, vf, r;

	if (insn & (1u << 9))
		r = sub_cv(a, b, 1, &cf, &vf);
	else
		r = add_cv(a, b, 0, &cf, &vf);
	c->r[insn & 7] = r;
	set_nzcv(c, r, cf, vf);
}

/* ---- formato 3: inmediato de 8 bits -------------------------------------------------------- */

ALWAYS_INLINE void th_imm8(struct acore *c, uint32_t insn, const int op)
{
	const uint32_t rd = (insn >> 8) & 7, imm = insn & 0xFF, a = c->r[rd];
	uint32_t cf, vf, r;

	switch (op) {
	case 0: /* MOV: N y Z; C y V quedan */
		c->r[rd] = imm;
		set_nz(c, imm);
		return;
	case 1:
		r = sub_cv(a, imm, 1, &cf, &vf);
		break;
	case 2:
		r = add_cv(a, imm, 0, &cf, &vf);
		c->r[rd] = r;
		break;
	default:
		r = sub_cv(a, imm, 1, &cf, &vf);
		c->r[rd] = r;
		break;
	}
	set_nzcv(c, r, cf, vf);
}

static void th_mov8(struct acore *c, uint32_t insn) { th_imm8(c, insn, 0); }
static void th_cmp8(struct acore *c, uint32_t insn) { th_imm8(c, insn, 1); }
static void th_add8(struct acore *c, uint32_t insn) { th_imm8(c, insn, 2); }
static void th_sub8(struct acore *c, uint32_t insn) { th_imm8(c, insn, 3); }

/* ---- formato 4: operaciones de ALU ------------------------------------------------------------- */

ALWAYS_INLINE void th_alu(struct acore *c, uint32_t insn, const int op)
{
	const uint32_t rd = insn & 7, rs = (insn >> 3) & 7, a = c->r[rd], b = c->r[rs];
	const uint32_t cin = (c->cpsr >> 29) & 1;
	uint32_t r = 0, cf = 0, vf = 0, sc = 0;

	switch (op) {
	case 0: r = a & b; break;
	case 1: r = a ^ b; break;
	case 2: case 3: case 4: case 7: { /* LSL, LSR, ASR, ROR por registro */
		static const uint8_t type_of[8] = { 0, 0, 0, 1, 2, 0, 0, 3 };

		r = shift_by_reg(type_of[op], a, b, cin, &sc);
		c->cycles++;
		break;
	}
	case 5: r = add_cv(a, b, cin, &cf, &vf); break;
	case 6: r = sub_cv(a, b, cin, &cf, &vf); break;
	case 8: r = a & b; break;
	case 9: r = sub_cv(0, b, 1, &cf, &vf); break;
	case 10: r = sub_cv(a, b, 1, &cf, &vf); break;
	case 11: r = add_cv(a, b, 0, &cf, &vf); break;
	case 12: r = a | b; break;
	case 13:
		r = a * b;
		c->cycles += mul_cycles(a, 1);
		break;
	case 14: r = a & ~b; break;
	default: r = ~b; break;
	}
	if (op != 8 && op != 10 && op != 11)
		c->r[rd] = r;
	switch (op) {
	case 5: case 6: case 9: case 10: case 11:
		set_nzcv(c, r, cf, vf);
		break;
	case 2: case 3: case 4: case 7:
		c->cpsr = (c->cpsr & ~(ACORE_N | ACORE_Z | ACORE_C)) | (r & ACORE_N) | (r == 0 ? ACORE_Z : 0u) |
			  (sc ? ACORE_C : 0u);
		break;
	default: /* lógicas y MUL: N y Z */
		set_nz(c, r);
		break;
	}
}

#define THALU(op) static void th_alu_##op(struct acore *c, uint32_t insn) { th_alu(c, insn, op); }
THALU(0) THALU(1) THALU(2) THALU(3) THALU(4) THALU(5) THALU(6) THALU(7)
THALU(8) THALU(9) THALU(10) THALU(11) THALU(12) THALU(13) THALU(14) THALU(15)

/* ---- formato 5: registros altos y BX / BLX ------------------------------------------------------ */

static void th_hi(struct acore *c, uint32_t insn)
{
	const uint32_t rd = (insn & 7) | ((insn >> 4) & 8), rs = (insn >> 3) & 15, b = c->r[rs];
	const uint32_t a = c->r[rd];

	switch ((insn >> 8) & 3) {
	case 0: /* ADD sin banderas */
		if (rd == 15) {
			jump(c, a + b);
			c->cycles += 2;
		} else {
			c->r[rd] = a + b;
		}
		break;
	case 1: { /* CMP */
		uint32_t cf, vf;
		const uint32_t r = sub_cv(a, b, 1, &cf, &vf);

		set_nzcv(c, r, cf, vf);
		break;
	}
	case 2: /* MOV sin banderas */
		if (rd == 15) {
			jump(c, b);
			c->cycles += 2;
		} else {
			c->r[rd] = b;
		}
		break;
	default: /* BX / BLX Rm */
		if (insn & 0x80) { /* BLX (solo ARMv5) */
			if (!c->v5) {
				acore_undefined(c);
				return;
			}
			c->r[14] = (c->r[15] - 2) | 1;
		}
		jump_bx(c, b);
		c->cycles += 2;
		break;
	}
}

/* ---- formato 6: carga relativa al PC ----------------------------------------------------------------- */

static void th_ldr_pc(struct acore *c, uint32_t insn)
{
	const uint32_t v = mem_rd32(c, (c->r[15] & ~3u) + (insn & 0xFF) * 4);

	CHECK_DABORT();
	c->r[(insn >> 8) & 7] = v;
	c->cycles += 2;
}

/* ---- formatos 7 y 8: carga y almacenamiento con registro de desplazamiento --------------------------- */

static void th_ldst_reg(struct acore *c, uint32_t insn)
{
	const uint32_t addr = c->r[(insn >> 3) & 7] + c->r[(insn >> 6) & 7], rd = insn & 7;
	uint32_t v;

	switch ((insn >> 9) & 7) {
	case 0:
		mem_wr32(c, addr & ~3u, c->r[rd]);
		CHECK_DABORT();
		c->cycles++;
		return;
	case 1:
		mem_wr16(c, addr & ~1u, c->r[rd]);
		CHECK_DABORT();
		c->cycles++;
		return;
	case 2:
		mem_wr8(c, addr, c->r[rd]);
		CHECK_DABORT();
		c->cycles++;
		return;
	case 3:
		v = (uint32_t)(int32_t)(int8_t)mem_rd8(c, addr);
		break;
	case 4:
		v = ld_word(c, addr);
		break;
	case 5:
		v = ld_half(c, addr);
		break;
	case 6:
		v = mem_rd8(c, addr);
		break;
	default:
		v = ld_shalf(c, addr);
		break;
	}
	CHECK_DABORT();
	c->r[rd] = v;
	c->cycles += 2;
}

/* ---- formato 9: palabra o byte con desplazamiento inmediato -------------------------------------------- */

static void th_ldst_imm(struct acore *c, uint32_t insn)
{
	const uint32_t rd = insn & 7, base = c->r[(insn >> 3) & 7], imm = (insn >> 6) & 31;
	uint32_t v;

	switch ((insn >> 11) & 3) {
	case 0:
		mem_wr32(c, (base + imm * 4) & ~3u, c->r[rd]);
		CHECK_DABORT();
		c->cycles++;
		return;
	case 1:
		v = ld_word(c, base + imm * 4);
		break;
	case 2:
		mem_wr8(c, base + imm, c->r[rd]);
		CHECK_DABORT();
		c->cycles++;
		return;
	default:
		v = mem_rd8(c, base + imm);
		break;
	}
	CHECK_DABORT();
	c->r[rd] = v;
	c->cycles += 2;
}

/* ---- formato 10: media palabra con desplazamiento inmediato ----------------------------------------------- */

static void th_ldst_half(struct acore *c, uint32_t insn)
{
	const uint32_t rd = insn & 7, addr = c->r[(insn >> 3) & 7] + ((insn >> 6) & 31) * 2;

	if (insn & (1u << 11)) {
		const uint32_t v = ld_half(c, addr);

		CHECK_DABORT();
		c->r[rd] = v;
		c->cycles += 2;
	} else {
		mem_wr16(c, addr & ~1u, c->r[rd]);
		CHECK_DABORT();
		c->cycles++;
	}
}

/* ---- formato 11: relativo a SP -------------------------------------------------------------------------------- */

static void th_ldst_sp(struct acore *c, uint32_t insn)
{
	const uint32_t rd = (insn >> 8) & 7, addr = c->r[13] + (insn & 0xFF) * 4;

	if (insn & (1u << 11)) {
		const uint32_t v = ld_word(c, addr);

		CHECK_DABORT();
		c->r[rd] = v;
		c->cycles += 2;
	} else {
		mem_wr32(c, addr & ~3u, c->r[rd]);
		CHECK_DABORT();
		c->cycles++;
	}
}

/* ---- formatos 12 y 13: direcciones relativas y ajuste de SP ----------------------------------------------------- */

static void th_add_pc(struct acore *c, uint32_t insn)
{
	c->r[(insn >> 8) & 7] = (c->r[15] & ~3u) + (insn & 0xFF) * 4;
}

static void th_add_sp(struct acore *c, uint32_t insn)
{
	c->r[(insn >> 8) & 7] = c->r[13] + (insn & 0xFF) * 4;
}

static void th_sp_adj(struct acore *c, uint32_t insn)
{
	const uint32_t off = (insn & 0x7F) * 4;

	c->r[13] = (insn & 0x80) ? c->r[13] - off : c->r[13] + off;
}

/* ---- formato 14: PUSH y POP ------------------------------------------------------------------------------------- */

static void th_push(struct acore *c, uint32_t insn)
{
	uint32_t list = insn & 0xFF, n = 0;
	const int lr = (insn >> 8) & 1;

	for (uint32_t m = list; m; m &= m - 1)
		n++;
	n += (uint32_t)lr;
	uint32_t sp = c->r[13] - 4 * n, addr = sp;

	for (uint32_t i = 0; i < 8; i++) {
		if (!(list & (1u << i)))
			continue;
		mem_wr32(c, addr & ~3u, c->r[i]);
		CHECK_DABORT();
		addr += 4;
	}
	if (lr) {
		mem_wr32(c, addr & ~3u, c->r[14]);
		CHECK_DABORT();
	}
	c->r[13] = sp;
	c->cycles += n ? n : 1;
}

static void th_pop(struct acore *c, uint32_t insn)
{
	const uint32_t list = insn & 0xFF;
	const int pc = (insn >> 8) & 1;
	uint32_t addr = c->r[13] & ~3u, n = 0, pcval = 0;

	for (uint32_t i = 0; i < 8; i++) {
		uint32_t v;

		if (!(list & (1u << i)))
			continue;
		v = mem_rd32(c, addr);
		CHECK_DABORT();
		c->r[i] = v;
		addr += 4;
		n++;
	}
	if (pc) {
		pcval = mem_rd32(c, addr);
		CHECK_DABORT();
		addr += 4;
		n++;
	}
	c->r[13] += 4 * n;
	if (pc) {
		if (c->v5)
			jump_bx(c, pcval);
		else
			jump(c, pcval);
		c->cycles += 2;
	}
	c->cycles += n + 1;
}

/* ---- formato 15: STMIA / LDMIA ------------------------------------------------------------------------------------ */

static void th_block(struct acore *c, uint32_t insn)
{
	const uint32_t rb = (insn >> 8) & 7, base = c->r[rb];
	uint32_t list = insn & 0xFF, addr = base & ~3u, n = 0;
	const int load = (insn >> 11) & 1;
	uint32_t final, pcval = 0;
	int pc_load = 0;

	for (uint32_t m = list; m; m &= m - 1)
		n++;
	if (n == 0) {
		/* lista vacía: la base cambia 0x40; ARMv4 transfiere además el PC */
		final = base + 0x40;
		if (!c->v5) {
			if (load) {
				pcval = mem_rd32(c, addr);
				CHECK_DABORT();
				pc_load = 1;
			} else {
				mem_wr32(c, addr, c->r[15] + 2);
				CHECK_DABORT();
			}
		}
	} else {
		final = base + 4 * n;
		if (load) {
			for (uint32_t i = 0; i < 8; i++) {
				uint32_t v;

				if (!(list & (1u << i)))
					continue;
				v = mem_rd32(c, addr);
				CHECK_DABORT();
				c->r[i] = v;
				addr += 4;
			}
		} else {
			for (uint32_t i = 0; i < 8; i++) {
				uint32_t v;

				if (!(list & (1u << i)))
					continue;
				v = c->r[i];
				if (i == rb && !c->v5 && (list & ((1u << rb) - 1)) != 0)
					v = final; /* ARMv4: la base nueva si no es el primero de la lista */
				mem_wr32(c, addr, v);
				CHECK_DABORT();
				addr += 4;
			}
		}
	}
	if (load) {
		const int in_list = (list >> rb) & 1;
		const int only = list == (1u << rb);
		const int last = (list >> rb) == 1u;

		if (!in_list || (c->v5 && (only || !last)))
			c->r[rb] = final;
		if (pc_load) {
			jump(c, pcval);
			c->cycles += 2;
		}
		c->cycles += n + 1;
	} else {
		c->r[rb] = final;
		c->cycles += n ? n : 1;
	}
}

/* ---- formatos 16 a 19: saltos, SWI y BL -------------------------------------------------------------------------------- */

static void th_bcond(struct acore *c, uint32_t insn)
{
	const uint32_t cond = (insn >> 8) & 15;

	if (cond == 0xF) {
		acore_swi(c, insn & 0xFF);
		c->cycles += 2;
		return;
	}
	if (cond == 0xE) {
		acore_undefined(c);
		return;
	}
	if ((acore_cond_mask[cond] >> (c->cpsr >> 28)) & 1u) {
		const int32_t off = (int32_t)(int8_t)insn * 2;

		c->r[15] = c->r[15] + (uint32_t)off + 2;
		c->cycles += 2;
	}
}

static void th_b(struct acore *c, uint32_t insn)
{
	const int32_t off = ((int32_t)(insn << 21) >> 20);

	c->r[15] = c->r[15] + (uint32_t)off + 2;
	c->cycles += 2;
}

static void th_bl_hi(struct acore *c, uint32_t insn)
{
	c->r[14] = c->r[15] + (uint32_t)(((int32_t)(insn << 21) >> 21) * 4096);
}

static void th_bl_lo(struct acore *c, uint32_t insn)
{
	const uint32_t target = c->r[14] + ((insn & 0x7FF) << 1);

	c->r[14] = (c->r[15] - 2) | 1;
	c->r[15] = (target & ~1u) + 2;
	c->cycles += 2;
}

/* BLX con inmediato (ARMv5): salta a ARM */
static void th_blx_lo(struct acore *c, uint32_t insn)
{
	const uint32_t target = (c->r[14] + ((insn & 0x7FF) << 1)) & ~3u;

	c->r[14] = (c->r[15] - 2) | 1;
	c->cpsr &= ~ACORE_T;
	c->r[15] = target + 4;
	c->cycles += 2;
}

static void th_swi(struct acore *c, uint32_t insn)
{
	acore_swi(c, insn & 0xFF);
	c->cycles += 2;
}

static void th_bkpt(struct acore *c, uint32_t insn)
{
	(void)insn;
	acore_prefetch_abort(c);
}

/* ---- clasificación --------------------------------------------------------------------------------------------------------- */

static acore_op_fn th_classify(unsigned idx, int v5)
{
	static const acore_op_fn alu[16] = {
		th_alu_0, th_alu_1, th_alu_2, th_alu_3, th_alu_4, th_alu_5, th_alu_6, th_alu_7,
		th_alu_8, th_alu_9, th_alu_10, th_alu_11, th_alu_12, th_alu_13, th_alu_14, th_alu_15,
	};
	const unsigned top5 = idx >> 5;

	switch (top5) {
	case 0: return th_lsl_i;
	case 1: return th_lsr_i;
	case 2: return th_asr_i;
	case 3: return th_addsub;
	case 4: return th_mov8;
	case 5: return th_cmp8;
	case 6: return th_add8;
	case 7: return th_sub8;
	case 8: /* 01000: ALU (bit 10 = 0) o registros altos (bit 10 = 1) */
		return (idx & 0x10) ? th_hi : alu[idx & 15];
	case 9: return th_ldr_pc;
	case 10: case 11: return th_ldst_reg;
	case 12: case 13: case 14: case 15: return th_ldst_imm;
	case 16: case 17: return th_ldst_half;
	case 18: case 19: return th_ldst_sp;
	case 20: return th_add_pc;
	case 21: return th_add_sp;
	case 22: case 23: { /* 1011: instrucciones varias, por los bits 11..8 */
		const unsigned sub = (idx >> 2) & 15;

		if (sub == 0)
			return th_sp_adj;
		if (sub == 4 || sub == 5)
			return th_push;
		if (sub == 12 || sub == 13)
			return th_pop;
		if (sub == 14 && v5)
			return th_bkpt;
		return th_undef;
	}
	case 24: case 25: return th_block;
	case 26: case 27: return ((idx >> 2) & 15) == 15 ? th_swi : th_bcond;
	case 28: return th_b;
	case 29: return v5 ? th_blx_lo : th_undef;
	case 30: return th_bl_hi;
	default: return th_bl_lo;
	}
}

void acore_thumb_tables_init(void)
{
	for (int v5 = 0; v5 < 2; v5++)
		for (unsigned i = 0; i < 1024; i++)
			acore_thumb_table[v5][i] = th_classify(i, v5);
}
