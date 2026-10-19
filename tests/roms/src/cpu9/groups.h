/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * SalsaDS — grupos de pruebas de CPU de las ROMs cpu9 (ARM9, ARM946E-S,
 * ARMv5TE) y cpuarm (ARM9 y ARM7TDMI, ARMv4T). Con ARM7 definido se saltan
 * las instrucciones que solo existen en ARMv5TE y no se comprueba el
 * acarreo de las multiplicaciones (en ARMv4 queda «destruido»).
 *
 * Cada grupo ejecuta una instrucción real con operandos de borde y compara
 * resultado y banderas con el modelo en C de model.h. Cubre proceso de datos
 * (las 16 operaciones, con las 4 combinaciones de banderas de entrada),
 * desplazador (inmediato y por registro), condiciones, multiplicaciones,
 * CLZ/saturación/multiplicaciones de 16 bits de ARMv5TE, carga y
 * almacenamiento (incluido el acceso no alineado), LDM/STM, SWP, el cambio
 * ARM/Thumb y las instrucciones de datos y ramas de Thumb.
 *
 * El veredicto sale por el marco de pruebas (common/sds.h). Todo el código
 * es propio de SalsaDS; el modelo se escribió desde el manual de ARM.
 */
#include <stdint.h>

#include "../common/sds.h"
#include "asm_ops.h"
#include "model.h"

static const uint32_t vals[] = {
	0x00000000, 0x00000001, 0x00000002, 0x7FFFFFFF, 0x80000000, 0x80000001,
	0xFFFFFFFE, 0xFFFFFFFF, 0x12345678, 0xDEADBEEF, 0x0000FFFF, 0xFFFF0000,
};
#define NV ((int)(sizeof vals / sizeof vals[0]))
static const uint32_t flag_in[] = { 0x0, 0x2, 0xD, 0xF };
#define NF ((int)(sizeof flag_in / sizeof flag_in[0]))
static const uint32_t amts[] = { 0, 1, 2, 31, 32, 33, 63, 64, 255 };
#define NA ((int)(sizeof amts / sizeof amts[0]))
#ifdef ARM7
#define MUL_CMASK (F_N | F_Z | F_V) /* en ARMv4, MULS deja C destruido: no se mira */
#else
#define MUL_CMASK (F_N | F_Z | F_C | F_V)
#endif

/* Compara resultado (si lo hay) y banderas de una operación contra el modelo. */
static void check_res_flags(uint32_t got_r, uint32_t got_f, uint32_t want_r, uint32_t want_f, int has_res)
{
	if (has_res)
		SDS_CHECK(got_r, want_r);
	SDS_CHECK(got_f, want_f);
}

static void group_shifter(void)
{
	sds_group("DESPLAZADOR");
	for (int i = 0; i < NV; i++)
		for (int fi = 0; fi < 2; fi++) {
			uint32_t fin = flag_in[fi] ? F_C : 0;
			for (unsigned k = 0; k < sizeof sh_imm_cases / sizeof sh_imm_cases[0]; k++) {
				const struct sh_imm_case *c = &sh_imm_cases[k];
				uint32_t res, fout, shc, want = m_shift_imm(c->type, vals[i], c->imm, fin >> 1, &shc);
				c->fn(0, vals[i], fin, &res, &fout);
				uint32_t wf = (want >> 31 ? F_N : 0) | (want == 0 ? F_Z : 0) | (shc ? F_C : 0);
				check_res_flags(res, fout, want, wf, 1);
			}
			for (int t = 0; t < 4; t++)
				for (int a = 0; a < NA; a++) {
					uint32_t res, fout, shc, want = m_shift_reg(t, vals[i], amts[a], fin >> 1, &shc);
					sh_reg_fns[t](amts[a], vals[i], fin, &res, &fout);
					uint32_t wf = (want >> 31 ? F_N : 0) | (want == 0 ? F_Z : 0) | (shc ? F_C : 0);
					check_res_flags(res, fout, want, wf, 1);
				}
		}
}

static void group_alu(void)
{
	sds_group("DATOS");
	for (int op = 0; op < 16; op++)
		for (int i = 0; i < NV; i++)
			for (int j = 0; j < NV; j++)
				for (int f = 0; f < NF; f++) {
					uint32_t res, fout, wf, wr = m_alu(op, vals[i], vals[j], flag_in[f], (flag_in[f] >> 1) & 1, &wf);
					alu_by_opcode[op](vals[i], vals[j], flag_in[f], &res, &fout);
					check_res_flags(res, fout, wr, wf, op < 8 || op > 11);
				}
	/* operando 2 desplazado: el desplazador y la ALU juntos */
	for (unsigned k = 0; k < sizeof alush_cases / sizeof alush_cases[0]; k++)
		for (int i = 0; i < NV; i++)
			for (int j = 0; j < NV; j++)
				for (int f = 0; f < NF; f++) {
					const struct alush_case *c = &alush_cases[k];
					uint32_t shc, op2 = m_shift_imm(c->type, vals[j], c->imm, (flag_in[f] >> 1) & 1, &shc);
					uint32_t res, fout, wf, wr = m_alu(c->op, vals[i], op2, flag_in[f], shc, &wf);
					c->fn(vals[i], vals[j], flag_in[f], &res, &fout);
					check_res_flags(res, fout, wr, wf, 1);
				}
}

static void group_cond(void)
{
	sds_group("CONDICIONES");
	for (int cc = 0; cc < 14; cc++)
		for (uint32_t f = 0; f < 16; f++) {
			SDS_CHECK(cond_fns[cc](f), m_cond(cc, f));
		}
	/* Las ramas de Thumb, las mismas 14 condiciones. */
	static const thumb_fn th[14] = { th_beq, th_bne, th_bcs, th_bcc, th_bmi, th_bpl, th_bvs,
					 th_bvc, th_bhi, th_bls, th_bge, th_blt, th_bgt, th_ble };
	for (int cc = 0; cc < 14; cc++)
		for (uint32_t f = 0; f < 16; f++) {
			uint32_t res, fout;
			call_thumb(th[cc], 0, 0, f, &res, &fout);
			SDS_CHECK(res, m_cond(cc, f));
			(void)fout; /* el movs de la función ya cambió las banderas */
		}
}

static void group_mul(void)
{
	sds_group("MULTIPLICA");
	for (int i = 0; i < NV; i++)
		for (int j = 0; j < NV; j++) {
			uint32_t a = vals[i], b = vals[j], res, fout, want = a * b;
			op_mul(a, b, 0x5, &res, &fout);
			SDS_CHECK(res, want);
			SDS_CHECK(fout, 0x5); /* sin S no toca las banderas */
			for (int f = 0; f < NF; f++) {
				op_muls(a, b, flag_in[f], &res, &fout);
				SDS_CHECK(res, want);
				/* N y Z del resultado; C y V sin cambio (ARMv5; en ARMv4 C queda destruido) */
				SDS_CHECK(fout & MUL_CMASK, ((want >> 31 ? F_N : 0) | (want == 0 ? F_Z : 0) | (flag_in[f] & (F_C | F_V))) & MUL_CMASK);
			}
			SDS_CHECK(op_mla(a, b, 0x1357), a * b + 0x1357);
			uint32_t lo = 0, hi = 0;
			op_umull(a, b, &lo, &hi);
			uint64_t u = (uint64_t)a * b;
			SDS_CHECK(lo, (uint32_t)u);
			SDS_CHECK(hi, (uint32_t)(u >> 32));
			op_smull(a, b, &lo, &hi);
			int64_t s = (int64_t)(int32_t)a * (int32_t)b;
			SDS_CHECK(lo, (uint32_t)s);
			SDS_CHECK(hi, (uint32_t)((uint64_t)s >> 32));
			lo = 0xFFFFFFF0u;
			hi = 7;
			op_umlal(a, b, &lo, &hi);
			u += ((uint64_t)7 << 32) | 0xFFFFFFF0u;
			SDS_CHECK(lo, (uint32_t)u);
			SDS_CHECK(hi, (uint32_t)(u >> 32));
			lo = 0xFFFFFFF0u;
			hi = 7;
			op_smlal(a, b, &lo, &hi);
			s += (int64_t)(((uint64_t)7 << 32) | 0xFFFFFFF0u);
			SDS_CHECK(lo, (uint32_t)s);
			SDS_CHECK(hi, (uint32_t)((uint64_t)s >> 32));
		}
}

#ifdef ARM9
static const uint32_t hv[] = {
	0x00010002, 0x7FFF8000, 0x8000FFFF, 0xFFFFFFFF, 0x12345678, 0x80008000, 0x7FFF7FFF, 0x00000000,
};
#define NH ((int)(sizeof hv / sizeof hv[0]))

static void group_v5(void)
{
	sds_group("ARMV5TE");
	for (int i = 0; i < 40; i++) {
		uint32_t v = i < 32 ? 1u << i : (uint32_t)i * 0x01010101u;
		SDS_CHECK(op_clz(v), v ? (uint32_t)__builtin_clz(v) : 32);
	}
	SDS_CHECK(op_clz(0), 32);
	for (int i = 0; i < NV; i++)
		for (int j = 0; j < NV; j++) {
			uint32_t a = vals[i], b = vals[j], q, wq;
			int64_t sa = (int32_t)a, sb = (int32_t)b;
			wq = 0;
			SDS_CHECK(op_qadd(a, b, &q), m_sat(sa + sb, &wq));
			SDS_CHECK(q, wq);
			wq = 0;
			SDS_CHECK(op_qsub(a, b, &q), m_sat(sa - sb, &wq));
			SDS_CHECK(q, wq);
			wq = 0;
			uint32_t d = m_sat(2 * sb, &wq);
			SDS_CHECK(op_qdadd(a, b, &q), m_sat(sa + (int32_t)d, &wq));
			SDS_CHECK(q, wq);
			wq = 0;
			d = m_sat(2 * sb, &wq);
			SDS_CHECK(op_qdsub(a, b, &q), m_sat(sa - (int32_t)d, &wq));
			SDS_CHECK(q, wq);
		}
	for (int i = 0; i < NH; i++)
		for (int j = 0; j < NH; j++) {
			uint32_t a = hv[i], b = hv[j], q;
			SDS_CHECK(op_smulbb(a, b), (uint32_t)(m_lo16(a) * m_lo16(b)));
			SDS_CHECK(op_smulbt(a, b), (uint32_t)(m_lo16(a) * m_hi16(b)));
			SDS_CHECK(op_smultb(a, b), (uint32_t)(m_hi16(a) * m_lo16(b)));
			SDS_CHECK(op_smultt(a, b), (uint32_t)(m_hi16(a) * m_hi16(b)));
			SDS_CHECK(op_smulwb(a, b), (uint32_t)(((int64_t)(int32_t)a * m_lo16(b)) >> 16));
			SDS_CHECK(op_smulwt(a, b), (uint32_t)(((int64_t)(int32_t)a * m_hi16(b)) >> 16));
			/* con acumulación: Q si la suma desborda con signo */
			static const uint32_t accs[] = { 0, 0x7FFFFFFF, 0x80000000 };
			for (int k = 0; k < 3; k++) {
				uint32_t c = accs[k];
				int64_t p;
				uint32_t wr, wq;

#define SMLA_CHECK(fn, prod)                                                     \
	p = (prod);                                                              \
	wr = (uint32_t)(p + (int32_t)c);                                         \
	wq = (p + (int32_t)c) != (int32_t)(p + (int32_t)c);                      \
	SDS_CHECK(fn(a, b, c, &q), wr);                                          \
	SDS_CHECK(q, wq)
				SMLA_CHECK(op_smlabb, m_lo16(a) * m_lo16(b));
				SMLA_CHECK(op_smlabt, m_lo16(a) * m_hi16(b));
				SMLA_CHECK(op_smlatb, m_hi16(a) * m_lo16(b));
				SMLA_CHECK(op_smlatt, m_hi16(a) * m_hi16(b));
				SMLA_CHECK(op_smlawb, ((int64_t)(int32_t)a * m_lo16(b)) >> 16);
				SMLA_CHECK(op_smlawt, ((int64_t)(int32_t)a * m_hi16(b)) >> 16);
			}
			uint32_t lo = 0x90000000u, hi = 0xFFFFFFF0u;
			int64_t acc = (int64_t)(((uint64_t)hi << 32) | lo);
			op_smlalbb(a, b, &lo, &hi);
			SDS_CHECK(lo, (uint32_t)(acc + m_lo16(a) * m_lo16(b)));
			SDS_CHECK(hi, (uint32_t)((uint64_t)(acc + m_lo16(a) * m_lo16(b)) >> 32));
			lo = 0x90000000u;
			hi = 0xFFFFFFF0u;
			op_smlalbt(a, b, &lo, &hi);
			SDS_CHECK(lo, (uint32_t)(acc + m_lo16(a) * m_hi16(b)));
			lo = 0x90000000u;
			hi = 0xFFFFFFF0u;
			op_smlaltb(a, b, &lo, &hi);
			SDS_CHECK(lo, (uint32_t)(acc + m_hi16(a) * m_lo16(b)));
			lo = 0x90000000u;
			hi = 0xFFFFFFF0u;
			op_smlaltt(a, b, &lo, &hi);
			SDS_CHECK(hi, (uint32_t)((uint64_t)(acc + m_hi16(a) * m_hi16(b)) >> 32));
		}
}
#endif /* ARM9 */

static uint32_t buf[16] __attribute__((aligned(32)));

static void fill_buf(void)
{
	for (int i = 0; i < 64; i++)
		((volatile uint8_t *)buf)[i] = (uint8_t)(i + 1);
	((volatile uint8_t *)buf)[16] = 0x80;
	((volatile uint8_t *)buf)[17] = 0x7F;
	((volatile uint8_t *)buf)[18] = 0xFF;
	((volatile uint8_t *)buf)[19] = 0x80;
}

static void group_loadstore(void)
{
	sds_group("MEMORIA");
	fill_buf();
	uint32_t w0 = 0x04030201u;
	for (uint32_t off = 0; off < 4; off++)
		SDS_CHECK(op_ldr((const uint8_t *)buf + off), m_ror(w0, 8 * off)); /* LDR no alineado rota */
	SDS_CHECK(op_ldrb((const uint8_t *)buf + 5), 6);
	SDS_CHECK(op_ldrh((const uint8_t *)buf + 2), 0x0403);
	SDS_CHECK(op_ldrsb((const uint8_t *)buf + 16), 0xFFFFFF80u);
	SDS_CHECK(op_ldrsb((const uint8_t *)buf + 17), 0x7F);
	SDS_CHECK(op_ldrsh((const uint8_t *)buf + 18), 0xFFFF80FFu);
	SDS_CHECK(op_ldrsh((const uint8_t *)buf + 16), 0x7F80);

	fill_buf();
	op_str((uint8_t *)buf + 6, 0xA1B2C3D4u); /* STR no alineado: ignora los 2 bits bajos */
	SDS_CHECK(buf[1], 0xA1B2C3D4u);
	SDS_CHECK(buf[0], 0x04030201u);
	op_strb((uint8_t *)buf + 9, 0x1234ABu);
	SDS_CHECK(buf[2], 0x0C0BABu << 8 | 0x09u);
	op_strh((uint8_t *)buf + 12, 0xFEDC9876u);
	SDS_CHECK(buf[3], 0x100F9876u);

#ifdef ARM9
	fill_buf();
	uint32_t lo, hi;
	op_ldrd(&buf[2], &lo, &hi);
	SDS_CHECK(lo, 0x0C0B0A09u);
	SDS_CHECK(hi, 0x100F0E0Du);
	op_strd(&buf[6], 0x11223344u, 0x55667788u);
	SDS_CHECK(buf[6], 0x11223344u);
	SDS_CHECK(buf[7], 0x55667788u);
#endif

	fill_buf();
	SDS_CHECK(op_swp(&buf[0], 0xCAFEF00Du), 0x04030201u);
	SDS_CHECK(buf[0], 0xCAFEF00Du);
	SDS_CHECK(op_swpb(&buf[1], 0x1FFu), 0x05u);
	SDS_CHECK(buf[1], 0x080706FFu);
}

static void group_ldm(void)
{
	sds_group("LDM/STM");
	uint32_t *p;
	uint32_t a, b, c;

	for (int k = 0; k < 16; k++)
		buf[k] = 0xEE000000u + (uint32_t)k;
	p = &buf[4];
	op_stmia(&p, 0x11, 0x22, 0x33);
	SDS_CHECK(buf[4], 0x11); SDS_CHECK(buf[5], 0x22); SDS_CHECK(buf[6], 0x33);
	SDS_CHECK(buf[7], 0xEE000007u); SDS_CHECK(p - buf, 7);
	p = &buf[4];
	op_stmib(&p, 0x44, 0x55, 0x66);
	SDS_CHECK(buf[4], 0x11); SDS_CHECK(buf[5], 0x44); SDS_CHECK(buf[6], 0x55); SDS_CHECK(buf[7], 0x66);
	SDS_CHECK(p - buf, 7);
	p = &buf[8];
	op_stmda(&p, 0x77, 0x88, 0x99);
	SDS_CHECK(buf[6], 0x77); SDS_CHECK(buf[7], 0x88); SDS_CHECK(buf[8], 0x99);
	SDS_CHECK(p - buf, 5);
	p = &buf[8];
	op_stmdb(&p, 0xAA, 0xBB, 0xCC);
	SDS_CHECK(buf[5], 0xAA); SDS_CHECK(buf[6], 0xBB); SDS_CHECK(buf[7], 0xCC);
	SDS_CHECK(p - buf, 5);

	for (int k = 0; k < 16; k++)
		buf[k] = 0xC0DE0000u + (uint32_t)k;
	p = &buf[2];
	op_ldmia(&p, &a, &b, &c);
	SDS_CHECK(a, 0xC0DE0002u); SDS_CHECK(b, 0xC0DE0003u); SDS_CHECK(c, 0xC0DE0004u); SDS_CHECK(p - buf, 5);
	p = &buf[2];
	op_ldmib(&p, &a, &b, &c);
	SDS_CHECK(a, 0xC0DE0003u); SDS_CHECK(b, 0xC0DE0004u); SDS_CHECK(c, 0xC0DE0005u); SDS_CHECK(p - buf, 5);
	p = &buf[8];
	op_ldmda(&p, &a, &b, &c);
	SDS_CHECK(a, 0xC0DE0006u); SDS_CHECK(b, 0xC0DE0007u); SDS_CHECK(c, 0xC0DE0008u); SDS_CHECK(p - buf, 5);
	p = &buf[8];
	op_ldmdb(&p, &a, &b, &c);
	SDS_CHECK(a, 0xC0DE0005u); SDS_CHECK(b, 0xC0DE0006u); SDS_CHECK(c, 0xC0DE0007u); SDS_CHECK(p - buf, 5);
}

static void group_interwork(void)
{
	sds_group("ARM/THUMB");
	for (int i = 0; i < NV; i++)
		for (int j = 0; j < NV; j++) {
			SDS_CHECK(th_add(vals[i], vals[j]), vals[i] + vals[j]); /* bl/blx a Thumb */
		}
	/* blx por registro (en ARMv4 mov lr + bx): el LR que ve la función es la dirección de la instrucción siguiente. */
	uint32_t lr, next, mode;
	thumb_fn fn = th_getlr;
#ifdef ARM9
	__asm__ volatile("adr %[n], 1f\n\t blx %[fn]\n\t 1: mov %[lr], r0\n\t mrs %[m], cpsr"
			 : [lr] "=r"(lr), [n] "=&r"(next), [m] "=r"(mode)
			 : [fn] "r"(fn) : "r0", "r1", "r2", "r3", "lr", "cc", "memory");
#else
	__asm__ volatile("adr %[n], 1f\n\t mov lr, %[n]\n\t bx %[fn]\n\t 1: mov %[lr], r0\n\t mrs %[m], cpsr"
			 : [lr] "=r"(lr), [n] "=&r"(next), [m] "=r"(mode)
			 : [fn] "r"(fn) : "r0", "r1", "r2", "r3", "lr", "cc", "memory");
#endif
	SDS_CHECK(lr, next);
	SDS_CHECK(mode & 0x20, 0); /* de vuelta en estado ARM */
}

enum th_kind { T_ALU, T_SHREG, T_NEG, T_MUL, T_ALUI, T_SHIMM };
struct th_case {
	thumb_fn fn;
	enum th_kind kind;
	int op;       /* opcode ARM (T_ALU, T_ALUI) o tipo de desplazamiento (T_SHREG, T_SHIMM) */
	uint32_t imm; /* inmediato (T_ALUI: el de 8 bits; T_SHIMM: el campo de 5 bits) */
};
static const struct th_case th_cases[] = {
	{ th_ands, T_ALU, 0, 0 },  { th_eors, T_ALU, 1, 0 },  { th_adcs, T_ALU, 5, 0 },  { th_sbcs, T_ALU, 6, 0 },
	{ th_tst, T_ALU, 8, 0 },   { th_cmp, T_ALU, 10, 0 },  { th_cmn, T_ALU, 11, 0 },  { th_orrs, T_ALU, 12, 0 },
	{ th_bics, T_ALU, 14, 0 }, { th_mvns, T_ALU, 15, 0 }, { th_adds_rrr, T_ALU, 4, 0 }, { th_subs_rrr, T_ALU, 2, 0 },
	{ th_lsls, T_SHREG, 0, 0 }, { th_lsrs, T_SHREG, 1, 0 }, { th_asrs, T_SHREG, 2, 0 }, { th_rors, T_SHREG, 3, 0 },
	{ th_negs, T_NEG, 0, 0 },  { th_muls, T_MUL, 0, 0 },
	{ th_adds_i8, T_ALUI, 4, 200 }, { th_subs_i8, T_ALUI, 2, 200 }, { th_cmp_i8, T_ALUI, 10, 200 },
	{ th_movs_i8, T_ALUI, 13, 200 },
	{ th_lsls_i5, T_SHIMM, 0, 5 }, { th_lsrs_i5, T_SHIMM, 1, 5 }, { th_asrs_i5, T_SHIMM, 2, 5 },
	{ th_lsls_i0, T_SHIMM, 0, 0 }, { th_lsrs_i32, T_SHIMM, 1, 0 }, { th_asrs_i32, T_SHIMM, 2, 0 },
};

static void group_thumb(void)
{
	sds_group("THUMB");
	for (unsigned k = 0; k < sizeof th_cases / sizeof th_cases[0]; k++)
		for (int i = 0; i < NV; i++)
			for (int j = 0; j < NV; j++)
				for (int f = 0; f < NF; f++) {
					const struct th_case *c = &th_cases[k];
					uint32_t a = vals[i], b = vals[j], fin = flag_in[f], cin = (fin >> 1) & 1;
					uint32_t want = 0, wf = 0, shc, res, fout, fmask = 0xF;
					int nowrite = c->kind == T_ALU && (c->op >= 8 && c->op <= 11);

					switch (c->kind) {
					case T_ALU:
						want = m_alu(c->op, a, b, fin, cin, &wf);
						if (nowrite)
							want = a;
						break;
					case T_SHREG:
						want = m_shift_reg(c->op, a, b, cin, &shc);
						wf = (want >> 31 ? F_N : 0) | (want == 0 ? F_Z : 0) | (shc ? F_C : 0) | (fin & F_V);
						break;
					case T_NEG:
						want = m_alu(3, b, 0, fin, cin, &wf);
						break;
					case T_MUL:
						want = a * b;
						wf = (want >> 31 ? F_N : 0) | (want == 0 ? F_Z : 0) | (fin & (F_C | F_V));
						wf &= MUL_CMASK;
						fmask = MUL_CMASK;
						break;
					case T_ALUI:
						want = m_alu(c->op, a, c->imm, fin, cin, &wf);
						if (c->op == 10)
							want = a;
						break;
					default:
						want = m_shift_imm(c->op, a, c->imm, cin, &shc);
						wf = (want >> 31 ? F_N : 0) | (want == 0 ? F_Z : 0) | (shc ? F_C : 0) | (fin & F_V);
						break;
					}
					call_thumb(c->fn, a, b, fin, &res, &fout);
					check_res_flags(res, fout & fmask, want, wf, 1);
				}
}

