/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * SalsaDS — juego de instrucciones ARM (ARMv4T y ARMv5TE) del intérprete.
 *
 * Despacho: una tabla de 4096 punteros por versión, indexada por los bits
 * 27..20 y 7..4 de la instrucción (todo lo que distingue la clase); la
 * condición se prueba antes, con una máscara por condición. La clasificación
 * (`classify`) es la única parte que conoce la codificación; los manejadores
 * leen el resto de los campos.
 *
 * Convención: al ejecutarse un manejador, r[15] vale la dirección de la
 * instrucción + 8 (lo que lee el programa); un salto deja en r[15] el destino
 * + 4 (ver acore.h). Código propio, escrito desde el manual de arquitectura de
 * ARM y GBATEK.
 */
#include "cpu/acore_priv.h"

uint16_t acore_cond_mask[16];
acore_op_fn acore_arm_table[2][4096];

static void op_undef(struct acore *c, uint32_t insn)
{
	(void)insn;
	acore_undefined(c);
}

/* ---- proceso de datos ----------------------------------------------------- */

/* form: 0 inmediato rotado, 1 registro con desplazamiento inmediato, 2 registro con desplazamiento por registro. */
ALWAYS_INLINE void dp(struct acore *c, uint32_t insn, const int op, const int s, const int form)
{
	const uint32_t rn = (insn >> 16) & 15, rd = (insn >> 12) & 15;
	const uint32_t cin = (c->cpsr >> 29) & 1;
	uint32_t a, b, sc = cin, r, cf = 0, vf = 0;

	a = c->r[rn];
	if (form == 0) {
		const uint32_t rot = (insn >> 7) & 0x1E;

		b = ror32(insn & 0xFF, rot);
		if (rot)
			sc = b >> 31;
	} else if (form == 1) {
		b = shift_by_imm((insn >> 5) & 3, c->r[insn & 15], (insn >> 7) & 31, cin, &sc);
	} else {
		uint32_t rm = c->r[insn & 15];

		/* con el desplazamiento en un registro, el PC se lee con un ciclo más: +12 */
		if ((insn & 15) == 15)
			rm += 4;
		if (rn == 15)
			a += 4;
		b = shift_by_reg((insn >> 5) & 3, rm, c->r[(insn >> 8) & 15], cin, &sc);
		c->cycles++;
	}

	switch (op) {
	case 0: case 8: r = a & b; break;
	case 1: case 9: r = a ^ b; break;
	case 2: case 10: r = sub_cv(a, b, 1, &cf, &vf); break;
	case 3: r = sub_cv(b, a, 1, &cf, &vf); break;
	case 4: case 11: r = add_cv(a, b, 0, &cf, &vf); break;
	case 5: r = add_cv(a, b, cin, &cf, &vf); break;
	case 6: r = sub_cv(a, b, cin, &cf, &vf); break;
	case 7: r = sub_cv(b, a, cin, &cf, &vf); break;
	case 12: r = a | b; break;
	case 13: r = b; break;
	case 14: r = a & ~b; break;
	default: r = ~b; break;
	}

	{
		const int logical = op == 0 || op == 1 || op == 8 || op == 9 || op >= 12;
		const int test = op >= 8 && op <= 11;

		if (!test && rd == 15) {
			if (s)
				restore_spsr(c); /* MOVS PC,LR y compañía: vuelta de excepción */
			jump(c, r);
			c->cycles += 2;
			return;
		}
		if (!test)
			c->r[rd] = r;
		if (s) {
			if (logical)
				c->cpsr = (c->cpsr & ~(ACORE_N | ACORE_Z | ACORE_C)) | (r & ACORE_N) |
					  (r == 0 ? ACORE_Z : 0u) | (sc ? ACORE_C : 0u);
			else
				set_nzcv(c, r, cf, vf);
		}
	}
}

#define DPF(op, s, f) \
	static void dp_##op##_##s##_##f(struct acore *c, uint32_t insn) { dp(c, insn, op, s, f); }
#define DPS(op, s) DPF(op, s, 0) DPF(op, s, 1) DPF(op, s, 2)
#define DPO(op) DPS(op, 0) DPS(op, 1)
DPO(0) DPO(1) DPO(2) DPO(3) DPO(4) DPO(5) DPO(6) DPO(7)
DPO(8) DPO(9) DPO(10) DPO(11) DPO(12) DPO(13) DPO(14) DPO(15)

#define DPT(op) { { dp_##op##_0_0, dp_##op##_0_1, dp_##op##_0_2 }, { dp_##op##_1_0, dp_##op##_1_1, dp_##op##_1_2 } }
static const acore_op_fn dp_tab[16][2][3] = {
	DPT(0), DPT(1), DPT(2), DPT(3), DPT(4), DPT(5), DPT(6), DPT(7),
	DPT(8), DPT(9), DPT(10), DPT(11), DPT(12), DPT(13), DPT(14), DPT(15),
};

/* ---- MRS / MSR --------------------------------------------------------------- */

static void op_mrs(struct acore *c, uint32_t insn)
{
	const uint32_t rd = (insn >> 12) & 15;

	if ((insn & (1u << 22)) && c->bank)
		c->r[rd] = c->spsr_bank[c->bank];
	else
		c->r[rd] = c->cpsr; /* en usuario/sistema no hay SPSR: devuelve el CPSR */
}

static void msr(struct acore *c, uint32_t insn, uint32_t v)
{
	const uint32_t fm = (insn >> 16) & 15;
	uint32_t mask = 0;

	if (fm & 8)
		mask |= 0xFF000000u;
	if (fm & 4)
		mask |= 0x00FF0000u;
	if (fm & 2)
		mask |= 0x0000FF00u;
	if (fm & 1)
		mask |= 0x000000FFu;
	mask &= c->v5 ? 0xF80000FFu : 0xF00000FFu;
	if (insn & (1u << 22)) {
		if (c->bank) {
			uint32_t *sp = &c->spsr_bank[c->bank];

			*sp = (*sp & ~mask) | (v & mask);
		}
		return;
	}
	if ((c->cpsr & ACORE_MODE_MASK) == ACORE_MODE_USR)
		mask &= 0xFF000000u; /* en modo usuario solo cambian las banderas */
	mask &= ~ACORE_T;       /* MSR no cambia el estado: eso es de BX */
	acore_set_cpsr(c, (c->cpsr & ~mask) | (v & mask));
}

static void op_msr_reg(struct acore *c, uint32_t insn)
{
	msr(c, insn, c->r[insn & 15]);
}

static void op_msr_imm(struct acore *c, uint32_t insn)
{
	msr(c, insn, ror32(insn & 0xFF, ((insn >> 8) & 15) * 2));
}

/* ---- ramas -------------------------------------------------------------------- */

static void op_b(struct acore *c, uint32_t insn)
{
	const int32_t off = (int32_t)(insn << 8) >> 6;

	c->r[15] = c->r[15] + (uint32_t)off + 4;
	c->cycles += 2;
}

static void op_bl(struct acore *c, uint32_t insn)
{
	const int32_t off = (int32_t)(insn << 8) >> 6;

	c->r[14] = c->r[15] - 4;
	c->r[15] = c->r[15] + (uint32_t)off + 4;
	c->cycles += 2;
}

static void op_bx(struct acore *c, uint32_t insn)
{
	jump_bx(c, c->r[insn & 15]);
	c->cycles += 2;
}

static void op_blx_reg(struct acore *c, uint32_t insn)
{
	const uint32_t target = c->r[insn & 15];

	c->r[14] = c->r[15] - 4;
	jump_bx(c, target);
	c->cycles += 2;
}

static void op_swi(struct acore *c, uint32_t insn)
{
	acore_swi(c, insn & 0x00FFFFFFu);
	c->cycles += 2;
}

static void op_bkpt(struct acore *c, uint32_t insn)
{
	(void)insn;
	acore_prefetch_abort(c);
}

/* ---- multiplicaciones ------------------------------------------------------------ */

/* S pone N y Z; C y V quedan como estaban (en ARMv4 C es «destruido»: no se modela). */
static void op_mul(struct acore *c, uint32_t insn)
{
	const uint32_t rd = (insn >> 16) & 15, rn = (insn >> 12) & 15, rs = c->r[(insn >> 8) & 15];
	uint32_t r = c->r[insn & 15] * rs;

	if (insn & (1u << 21)) {
		r += c->r[rn];
		c->cycles++;
	}
	c->r[rd] = r;
	if (insn & (1u << 20))
		set_nz(c, r);
	c->cycles += mul_cycles(rs, 1);
}

static void op_mull(struct acore *c, uint32_t insn)
{
	const uint32_t lo = (insn >> 12) & 15, hi = (insn >> 16) & 15, rs = c->r[(insn >> 8) & 15], rm = c->r[insn & 15];
	uint64_t r;

	if (insn & (1u << 22))
		r = (uint64_t)((int64_t)(int32_t)rm * (int32_t)rs);
	else
		r = (uint64_t)rm * rs;
	if (insn & (1u << 21)) {
		r += ((uint64_t)c->r[hi] << 32) | c->r[lo];
		c->cycles++;
	}
	c->r[lo] = (uint32_t)r;
	c->r[hi] = (uint32_t)(r >> 32);
	if (insn & (1u << 20))
		c->cpsr = (c->cpsr & ~(ACORE_N | ACORE_Z)) | (uint32_t)(r >> 32 & ACORE_N) | (r == 0 ? ACORE_Z : 0u);
	c->cycles += mul_cycles(rs, (insn >> 22) & 1) + 1;
}

/* ---- cargas y almacenamientos de palabra / byte ----------------------------------- */

ALWAYS_INLINE void sdt(struct acore *c, uint32_t insn, const int load, const int byte)
{
	const uint32_t rn = (insn >> 16) & 15, rd = (insn >> 12) & 15;
	const uint32_t base = c->r[rn];
	const int pre = (insn >> 24) & 1, up = (insn >> 23) & 1;
	uint32_t off, nb, addr;

	if (insn & (1u << 25)) {
		uint32_t unused;

		off = shift_by_imm((insn >> 5) & 3, c->r[insn & 15], (insn >> 7) & 31, (c->cpsr >> 29) & 1, &unused);
	} else {
		off = insn & 0xFFF;
	}
	nb = up ? base + off : base - off;
	addr = pre ? nb : base;

	if (load) {
		const uint32_t v = byte ? mem_rd8(c, addr) : ld_word(c, addr);

		CHECK_DABORT();
		if (!pre || (insn & (1u << 21)))
			c->r[rn] = nb;
		c->cycles += 2;
		if (rd == 15) {
			if (c->v5 && !byte)
				jump_bx(c, v);
			else
				jump(c, v);
			c->cycles += 2;
		} else {
			c->r[rd] = v;
		}
	} else {
		uint32_t v = c->r[rd];

		if (rd == 15)
			v += 4; /* STR PC guarda la dirección de la instrucción + 12 */
		if (byte)
			mem_wr8(c, addr, v);
		else
			mem_wr32(c, addr & ~3u, v);
		CHECK_DABORT();
		if (!pre || (insn & (1u << 21)))
			c->r[rn] = nb;
		c->cycles++;
	}
}

static void op_ldr(struct acore *c, uint32_t insn) { sdt(c, insn, 1, 0); }
static void op_ldrb(struct acore *c, uint32_t insn) { sdt(c, insn, 1, 1); }
static void op_str(struct acore *c, uint32_t insn) { sdt(c, insn, 0, 0); }
static void op_strb(struct acore *c, uint32_t insn) { sdt(c, insn, 0, 1); }

/* ---- medias palabras, con signo y dobles palabras ---------------------------------- */

enum hs_kind { HS_H, HS_SB, HS_SH, HS_D };

ALWAYS_INLINE void hs(struct acore *c, uint32_t insn, const int load, const enum hs_kind kind)
{
	const uint32_t rn = (insn >> 16) & 15, rd = (insn >> 12) & 15;
	const uint32_t base = c->r[rn];
	const int pre = (insn >> 24) & 1, up = (insn >> 23) & 1;
	const uint32_t off = (insn & (1u << 22)) ? ((insn >> 4) & 0xF0u) | (insn & 0xFu) : c->r[insn & 15];
	const uint32_t nb = up ? base + off : base - off;
	const uint32_t addr = pre ? nb : base;
	const int wb = !pre || (insn & (1u << 21));

	if (kind == HS_D) {
		/* LDRD/STRD (solo ARMv5TE): par Rd, Rd+1 con Rd par y distinto de r14; accesos de palabra alineados. */
		const uint32_t a0 = addr & ~3u;

		if ((rd & 1) || rd == 14) {
			acore_undefined(c);
			return;
		}
		if (load) {
			const uint32_t lo = mem_rd32(c, a0);

			CHECK_DABORT();
			const uint32_t hi = mem_rd32(c, a0 + 4);

			CHECK_DABORT();
			if (wb)
				c->r[rn] = nb;
			c->r[rd] = lo;
			c->r[rd + 1] = hi;
			c->cycles += 3;
		} else {
			const uint32_t lo = c->r[rd], hi = c->r[rd + 1];

			mem_wr32(c, a0, lo);
			CHECK_DABORT();
			mem_wr32(c, a0 + 4, hi);
			CHECK_DABORT();
			if (wb)
				c->r[rn] = nb;
			c->cycles += 2;
		}
		return;
	}

	if (load) {
		uint32_t v;

		if (kind == HS_H)
			v = ld_half(c, addr);
		else if (kind == HS_SB)
			v = (uint32_t)(int32_t)(int8_t)mem_rd8(c, addr);
		else
			v = ld_shalf(c, addr);
		CHECK_DABORT();
		if (wb)
			c->r[rn] = nb;
		c->cycles += 2;
		if (rd == 15) { /* impredecible: se trata como un salto sin cambio de estado */
			jump(c, v);
			c->cycles += 2;
		} else {
			c->r[rd] = v;
		}
	} else {
		uint32_t v = c->r[rd];

		if (rd == 15)
			v += 4;
		mem_wr16(c, addr & ~1u, v);
		CHECK_DABORT();
		if (wb)
			c->r[rn] = nb;
		c->cycles++;
	}
}

static void op_ldrh(struct acore *c, uint32_t insn) { hs(c, insn, 1, HS_H); }
static void op_ldrsb(struct acore *c, uint32_t insn) { hs(c, insn, 1, HS_SB); }
static void op_ldrsh(struct acore *c, uint32_t insn) { hs(c, insn, 1, HS_SH); }
static void op_strh(struct acore *c, uint32_t insn) { hs(c, insn, 0, HS_H); }
static void op_ldrd(struct acore *c, uint32_t insn) { hs(c, insn, 1, HS_D); }
static void op_strd(struct acore *c, uint32_t insn) { hs(c, insn, 0, HS_D); }

/* ---- SWP ---------------------------------------------------------------------------- */

static void op_swp(struct acore *c, uint32_t insn)
{
	const uint32_t rn = (insn >> 16) & 15, rd = (insn >> 12) & 15, addr = c->r[rn], src = c->r[insn & 15];
	uint32_t old;

	if (insn & (1u << 22)) {
		old = mem_rd8(c, addr);
		CHECK_DABORT();
		mem_wr8(c, addr, src);
	} else {
		old = ld_word(c, addr);
		CHECK_DABORT();
		mem_wr32(c, addr & ~3u, src);
	}
	CHECK_DABORT();
	c->r[rd] = old;
	c->cycles += 3;
}

/* ---- LDM / STM ------------------------------------------------------------------------ */

static unsigned popcount16(uint32_t x)
{
	unsigned n = 0;

	for (x &= 0xFFFF; x; x &= x - 1)
		n++;
	return n;
}

ALWAYS_INLINE void block(struct acore *c, uint32_t insn, const int load)
{
	const uint32_t rn = (insn >> 16) & 15;
	const int pre = (insn >> 24) & 1, up = (insn >> 23) & 1, s = (insn >> 22) & 1, w = (insn >> 21) & 1;
	uint32_t list = insn & 0xFFFF;
	const uint32_t base = c->r[rn];
	const unsigned n = popcount16(list);
	unsigned span = n;
	uint32_t start, final, addr, pcval = 0;

	if (n == 0) {
		/* lista vacía: la dirección se calcula como con 16 registros y la base cambia 0x40;
		 * ARMv4 transfiere además R15, ARMv5 no transfiere nada */
		span = 16;
		if (!c->v5)
			list = 1u << 15;
	}
	const int pc_in = (list >> 15) & 1;
	/* LDM con ^ y PC: restaura el CPSR y carga el banco actual; en los demás casos con ^ se usa el banco de usuario */
	const int user_bank = s && !(load && pc_in);

	if (up) {
		start = base + (pre ? 4u : 0u);
		final = base + 4u * span;
	} else {
		start = base - 4u * span + (pre ? 0u : 4u);
		final = base - 4u * span;
	}
	addr = start & ~3u;

	if (load) {
		for (uint32_t m = list, i = 0; m; m >>= 1, i++) {
			uint32_t v;

			if (!(m & 1))
				continue;
			v = mem_rd32(c, addr);
			CHECK_DABORT();
			addr += 4;
			if (i == 15)
				pcval = v;
			else if (user_bank)
				acore_set_user_reg(c, i, v);
			else
				c->r[i] = v;
		}
		if (w) {
			/* ARMv4: con la base en la lista no hay writeback; ARMv5: lo hay si es el único o no es el último */
			const int in_list = (list >> rn) & 1;
			const int only = list == (1u << rn);
			const int last = (list >> rn) == 1u;

			if (!in_list || (c->v5 && (only || !last)))
				c->r[rn] = final;
		}
		c->cycles += n + 1;
		if (pc_in) {
			if (s) {
				restore_spsr(c);
				jump(c, pcval);
			} else if (c->v5) {
				jump_bx(c, pcval);
			} else {
				jump(c, pcval);
			}
			c->cycles += 2;
		}
	} else {
		for (uint32_t m = list, i = 0; m; m >>= 1, i++) {
			uint32_t v;

			if (!(m & 1))
				continue;
			if (i == 15)
				v = c->r[15] + 4; /* STM guarda el PC como la dirección de la instrucción + 12 */
			else if (user_bank)
				v = acore_get_user_reg(c, i);
			else
				v = c->r[i];
			if (i == rn && w && !c->v5 && (list & ((1u << rn) - 1)) != 0)
				v = final; /* ARMv4: la base nueva si no es el primer registro de la lista */
			mem_wr32(c, addr, v);
			CHECK_DABORT();
			addr += 4;
		}
		if (w)
			c->r[rn] = final;
		c->cycles += n ? n : 1;
	}
}

static void op_ldm(struct acore *c, uint32_t insn) { block(c, insn, 1); }
static void op_stm(struct acore *c, uint32_t insn) { block(c, insn, 0); }

/* ---- ARMv5TE: CLZ, saturación, multiplicaciones de 16 bits ------------------------------- */

static void op_clz(struct acore *c, uint32_t insn)
{
	const uint32_t v = c->r[insn & 15];

	c->r[(insn >> 12) & 15] = v ? (uint32_t)__builtin_clz(v) : 32u;
}

static uint32_t sat32(int64_t v, struct acore *c)
{
	if (v > 0x7FFFFFFFLL) {
		c->cpsr |= ACORE_Q;
		return 0x7FFFFFFFu;
	}
	if (v < -0x80000000LL) {
		c->cpsr |= ACORE_Q;
		return 0x80000000u;
	}
	return (uint32_t)v;
}

/* QADD / QSUB / QDADD / QDSUB: Rd = sat(Rm ± [sat(2·]Rn[)]) */
static void op_qarith(struct acore *c, uint32_t insn)
{
	const int64_t m = (int32_t)c->r[insn & 15];
	int64_t n = (int32_t)c->r[(insn >> 16) & 15];
	const unsigned op = (insn >> 21) & 3;

	if (op & 2)
		n = (int32_t)sat32(2 * n, c);
	c->r[(insn >> 12) & 15] = sat32((op & 1) ? m - n : m + n, c);
}

ALWAYS_INLINE int32_t half(uint32_t v, uint32_t top)
{
	return top ? (int16_t)(v >> 16) : (int16_t)v;
}

/* SMLAxy / SMLAWy / SMULWy / SMLALxy / SMULxy (bits 22:21 eligen la familia; bit 5 = x, bit 6 = y). */
static void op_smul(struct acore *c, uint32_t insn)
{
	const uint32_t rm = c->r[insn & 15], rs = c->r[(insn >> 8) & 15];
	const uint32_t x = (insn >> 5) & 1, y = (insn >> 6) & 1;
	const uint32_t rd = (insn >> 16) & 15, rn = (insn >> 12) & 15;

	switch ((insn >> 21) & 3) {
	case 0: { /* SMLAxy */
		const int64_t sum = (int64_t)(half(rm, x) * half(rs, y)) + (int32_t)c->r[rn];

		if (sum != (int32_t)sum)
			c->cpsr |= ACORE_Q;
		c->r[rd] = (uint32_t)sum;
		break;
	}
	case 1: { /* SMLAWy (x = 0) / SMULWy (x = 1) */
		const int64_t prod = ((int64_t)(int32_t)rm * half(rs, y)) >> 16;

		if (x) {
			c->r[rd] = (uint32_t)prod;
		} else {
			const int64_t sum = prod + (int32_t)c->r[rn];

			if (sum != (int32_t)sum)
				c->cpsr |= ACORE_Q;
			c->r[rd] = (uint32_t)sum;
		}
		break;
	}
	case 2: { /* SMLALxy: RdHi = bits 19:16, RdLo = bits 15:12 */
		int64_t acc = (int64_t)(((uint64_t)c->r[rd] << 32) | c->r[rn]);

		acc += (int64_t)(half(rm, x) * half(rs, y));
		c->r[rn] = (uint32_t)acc;
		c->r[rd] = (uint32_t)((uint64_t)acc >> 32);
		c->cycles++;
		break;
	}
	default: /* SMULxy */
		c->r[rd] = (uint32_t)(half(rm, x) * half(rs, y));
		break;
	}
}

/* ---- coprocesadores ------------------------------------------------------------------------ */

/* MCR / MRC: solo CP15 y solo en la ARM9 y en modos privilegiados; lo demás es indefinido. */
static void op_cp15(struct acore *c, uint32_t insn)
{
	uint32_t v;
	const uint32_t rd = (insn >> 12) & 15;

	if (!c->v5 || ((insn >> 8) & 15) != 15 || (c->cpsr & ACORE_MODE_MASK) == ACORE_MODE_USR) {
		acore_undefined(c);
		return;
	}
	c->cycles += 2;
	if (insn & (1u << 20)) { /* MRC */
		if (acore_cp15_read(c, (insn >> 16) & 15, insn & 15, (insn >> 21) & 7, (insn >> 5) & 7, &v)) {
			acore_undefined(c);
			return;
		}
		if (rd == 15)
			c->cpsr = (c->cpsr & 0x0FFFFFFFu) | (v & 0xF0000000u);
		else
			c->r[rd] = v;
	} else {
		v = c->r[rd];
		if (acore_cp15_write(c, (insn >> 16) & 15, insn & 15, (insn >> 21) & 7, (insn >> 5) & 7, v))
			acore_undefined(c);
	}
}

/* ---- espacio incondicional (cond = 1111), solo ARMv5TE ---------------------------------------- */

void acore_arm_uncond(struct acore *c, uint32_t insn)
{
	if ((insn & 0x0E000000u) == 0x0A000000u) { /* BLX con inmediato: salta a Thumb */
		const int32_t off = ((int32_t)(insn << 8) >> 6) + (int32_t)((insn >> 24) & 1) * 2;
		const uint32_t target = c->r[15] + (uint32_t)off;

		c->r[14] = c->r[15] - 4;
		c->cpsr |= ACORE_T;
		c->r[15] = target + 2;
		c->cycles += 2;
		return;
	}
	if ((insn & 0x0C50F000u) == 0x0450F000u) /* PLD: sin caché no hace nada */
		return;
	acore_undefined(c);
}

/* ---- clasificación y tablas ---------------------------------------------------------------------- */

static acore_op_fn classify(unsigned hi, unsigned lo, int v5)
{
	const unsigned b27_25 = hi >> 5;
	const int s = hi & 1;

	switch (b27_25) {
	case 0:
		if ((lo & 9) == 9) { /* bit 7 y bit 4: multiplicaciones y transferencias extra */
			if ((lo & 6) == 0) {
				if ((hi & 0xFC) == 0x00)
					return op_mul;
				if ((hi & 0xF8) == 0x08)
					return op_mull;
				if ((hi & 0xFB) == 0x10)
					return op_swp;
				return op_undef;
			}
			if (hi & 1) { /* L = 1 */
				return lo == 0xB ? op_ldrh : lo == 0xD ? op_ldrsb : op_ldrsh;
			}
			if (lo == 0xB)
				return op_strh;
			return v5 ? (lo == 0xD ? op_ldrd : op_strd) : op_undef;
		}
		if ((hi & 0xF9) == 0x10) { /* TST/TEQ/CMP/CMN sin S: instrucciones varias */
			switch (lo) {
			case 0x0:
				return (hi & 2) ? op_msr_reg : op_mrs;
			case 0x1:
				if (hi == 0x12)
					return op_bx;
				return (hi == 0x16 && v5) ? op_clz : op_undef;
			case 0x3:
				return (hi == 0x12 && v5) ? op_blx_reg : op_undef;
			case 0x5:
				return v5 ? op_qarith : op_undef;
			case 0x7:
				return (hi == 0x12 && v5) ? op_bkpt : op_undef;
			default:
				if ((lo & 9) == 8) /* 1yx0: multiplicaciones de 16 bits */
					return v5 ? op_smul : op_undef;
				return op_undef;
			}
		}
		return dp_tab[(hi >> 1) & 15][s][(lo & 1) ? 2 : 1];
	case 1:
		if ((hi & 0xF9) == 0x10 + 0x20) { /* inmediato en el espacio de TST..CMN sin S: MSR */
			return (hi & 2) ? op_msr_imm : op_undef;
		}
		return dp_tab[(hi >> 1) & 15][s][0];
	case 2:
		return (hi & 1) ? ((hi & 4) ? op_ldrb : op_ldr) : ((hi & 4) ? op_strb : op_str);
	case 3:
		if (lo & 1)
			return op_undef;
		return (hi & 1) ? ((hi & 4) ? op_ldrb : op_ldr) : ((hi & 4) ? op_strb : op_str);
	case 4:
		return (hi & 1) ? op_ldm : op_stm;
	case 5:
		return (hi & 0x10) ? op_bl : op_b;
	case 6:
		return op_undef;
	default:
		if (hi & 0x10)
			return op_swi;
		return (lo & 1) ? op_cp15 : op_undef;
	}
}

void acore_tables_init(void)
{
	static int done;

	if (done)
		return;
	for (unsigned cc = 0; cc < 16; cc++) {
		uint16_t m = 0;

		for (unsigned f = 0; f < 16; f++) {
			const int n = (f >> 3) & 1, z = (f >> 2) & 1, cf = (f >> 1) & 1, v = f & 1;
			int ok;

			switch (cc) {
			case 0: ok = z; break;
			case 1: ok = !z; break;
			case 2: ok = cf; break;
			case 3: ok = !cf; break;
			case 4: ok = n; break;
			case 5: ok = !n; break;
			case 6: ok = v; break;
			case 7: ok = !v; break;
			case 8: ok = cf && !z; break;
			case 9: ok = !cf || z; break;
			case 10: ok = n == v; break;
			case 11: ok = n != v; break;
			case 12: ok = !z && n == v; break;
			case 13: ok = z || n != v; break;
			case 14: ok = 1; break;
			default: ok = 0; break; /* 1111: «nunca» en ARMv4; en ARMv5 lo atiende acore_arm_uncond */
			}
			if (ok)
				m |= (uint16_t)(1u << f);
		}
		acore_cond_mask[cc] = m;
	}
	for (int v5 = 0; v5 < 2; v5++)
		for (unsigned i = 0; i < 4096; i++)
			acore_arm_table[v5][i] = classify(i >> 4, i & 15, v5);
	acore_thumb_tables_init();
	done = 1;
}
