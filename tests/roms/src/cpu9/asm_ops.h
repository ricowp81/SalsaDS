/*
 * SalsaDS — instrucciones ARM bajo prueba, una función por forma. Cada una
 * pone las banderas de entrada (msr cpsr_f), ejecuta la instrucción y lee el
 * CPSR (mrs). El valor de la bandera Q (bit 27) se lee aparte donde importa.
 */
#ifndef CPU9_ASM_OPS_H
#define CPU9_ASM_OPS_H

#include <stdint.h>

typedef void (*alu_fn)(uint32_t a, uint32_t b, uint32_t fin, uint32_t *res, uint32_t *fout);

#define PRE "msr cpsr_f, %[fi]\n\t"
#define POST "\n\tmrs %[f], cpsr"
#define CLOB : "cc"

/* ---- proceso de datos con dos registros (op2 = registro) ------------------ */
#define ALU_RR(name, insn)                                                       \
	static void name(uint32_t a, uint32_t b, uint32_t fin, uint32_t *res, uint32_t *fout) \
	{                                                                        \
		uint32_t r, f;                                                   \
		__asm__ volatile(PRE insn " %[r], %[a], %[b]" POST               \
				 : [r] "=&r"(r), [f] "=&r"(f)                   \
				 : [fi] "r"(fin << 28), [a] "r"(a), [b] "r"(b) CLOB); \
		*res = r;                                                        \
		*fout = f >> 28;                                                 \
	}
#define ALU_CMP(name, insn)                                                      \
	static void name(uint32_t a, uint32_t b, uint32_t fin, uint32_t *res, uint32_t *fout) \
	{                                                                        \
		uint32_t f;                                                      \
		__asm__ volatile(PRE insn " %[a], %[b]" POST                     \
				 : [f] "=&r"(f)                                  \
				 : [fi] "r"(fin << 28), [a] "r"(a), [b] "r"(b) CLOB); \
		*res = 0;                                                        \
		*fout = f >> 28;                                                 \
	}
#define ALU_MOV(name, insn)                                                      \
	static void name(uint32_t a, uint32_t b, uint32_t fin, uint32_t *res, uint32_t *fout) \
	{                                                                        \
		uint32_t r, f;                                                   \
		(void)a;                                                         \
		__asm__ volatile(PRE insn " %[r], %[b]" POST                     \
				 : [r] "=&r"(r), [f] "=&r"(f)                   \
				 : [fi] "r"(fin << 28), [b] "r"(b) CLOB);        \
		*res = r;                                                        \
		*fout = f >> 28;                                                 \
	}

ALU_RR(op_ands, "ands")
ALU_RR(op_eors, "eors")
ALU_RR(op_subs, "subs")
ALU_RR(op_rsbs, "rsbs")
ALU_RR(op_adds, "adds")
ALU_RR(op_adcs, "adcs")
ALU_RR(op_sbcs, "sbcs")
ALU_RR(op_rscs, "rscs")
ALU_CMP(op_tst, "tst")
ALU_CMP(op_teq, "teq")
ALU_CMP(op_cmp, "cmp")
ALU_CMP(op_cmn, "cmn")
ALU_RR(op_orrs, "orrs")
ALU_MOV(op_movs, "movs")
ALU_RR(op_bics, "bics")
ALU_MOV(op_mvns, "mvns")

/* Índice = campo opcode de la instrucción. */
static const alu_fn alu_by_opcode[16] = {
	op_ands, op_eors, op_subs, op_rsbs, op_adds, op_adcs, op_sbcs, op_rscs,
	op_tst, op_teq, op_cmp, op_cmn, op_orrs, op_movs, op_bics, op_mvns,
};

/* ---- movs con desplazador inmediato ---------------------------------------- */
#define SH_IMM(name, shift)                                                      \
	static void name(uint32_t a, uint32_t b, uint32_t fin, uint32_t *res, uint32_t *fout) \
	{                                                                        \
		uint32_t r, f;                                                   \
		(void)a;                                                         \
		__asm__ volatile(PRE "movs %[r], %[b], " shift POST              \
				 : [r] "=&r"(r), [f] "=&r"(f)                   \
				 : [fi] "r"(fin << 28), [b] "r"(b) CLOB);        \
		*res = r;                                                        \
		*fout = f >> 28;                                                 \
	}
SH_IMM(sh_lsl0, "lsl #0")
SH_IMM(sh_lsl1, "lsl #1")
SH_IMM(sh_lsl31, "lsl #31")
SH_IMM(sh_lsr1, "lsr #1")
SH_IMM(sh_lsr31, "lsr #31")
SH_IMM(sh_lsr32, "lsr #32")
SH_IMM(sh_asr1, "asr #1")
SH_IMM(sh_asr31, "asr #31")
SH_IMM(sh_asr32, "asr #32")
SH_IMM(sh_ror1, "ror #1")
SH_IMM(sh_ror31, "ror #31")
SH_IMM(sh_rrx, "rrx")

struct sh_imm_case {
	alu_fn fn;
	int type;
	uint32_t imm; /* campo imm5: 0 = LSL #0 / LSR #32 / ASR #32 / RRX */
};
static const struct sh_imm_case sh_imm_cases[] = {
	{ sh_lsl0, 0, 0 },  { sh_lsl1, 0, 1 },  { sh_lsl31, 0, 31 }, { sh_lsr1, 1, 1 },
	{ sh_lsr31, 1, 31 }, { sh_lsr32, 1, 0 }, { sh_asr1, 2, 1 },   { sh_asr31, 2, 31 },
	{ sh_asr32, 2, 0 }, { sh_ror1, 3, 1 },  { sh_ror31, 3, 31 }, { sh_rrx, 3, 0 },
};

/* movs con desplazamiento por registro: a = cantidad. */
#define SH_REG(name, op)                                                         \
	static void name(uint32_t a, uint32_t b, uint32_t fin, uint32_t *res, uint32_t *fout) \
	{                                                                        \
		uint32_t r, f;                                                   \
		__asm__ volatile(PRE "movs %[r], %[b], " op " %[a]" POST         \
				 : [r] "=&r"(r), [f] "=&r"(f)                   \
				 : [fi] "r"(fin << 28), [a] "r"(a), [b] "r"(b) CLOB); \
		*res = r;                                                        \
		*fout = f >> 28;                                                 \
	}
SH_REG(shr_lsl, "lsl")
SH_REG(shr_lsr, "lsr")
SH_REG(shr_asr, "asr")
SH_REG(shr_ror, "ror")
static const alu_fn sh_reg_fns[4] = { shr_lsl, shr_lsr, shr_asr, shr_ror };

/* ---- proceso de datos con op2 desplazado (compone desplazador + ALU) ------- */
#define ALU_SH(name, insn, shift)                                                \
	static void name(uint32_t a, uint32_t b, uint32_t fin, uint32_t *res, uint32_t *fout) \
	{                                                                        \
		uint32_t r, f;                                                   \
		__asm__ volatile(PRE insn " %[r], %[a], %[b], " shift POST       \
				 : [r] "=&r"(r), [f] "=&r"(f)                   \
				 : [fi] "r"(fin << 28), [a] "r"(a), [b] "r"(b) CLOB); \
		*res = r;                                                        \
		*fout = f >> 28;                                                 \
	}
ALU_SH(alush_adds_lsl3, "adds", "lsl #3")
ALU_SH(alush_subs_lsr1, "subs", "lsr #1")
ALU_SH(alush_ands_ror5, "ands", "ror #5")
ALU_SH(alush_eors_asr32, "eors", "asr #32")
ALU_SH(alush_adcs_rrx, "adcs", "rrx")
ALU_SH(alush_orrs_lsl31, "orrs", "lsl #31")

struct alush_case {
	alu_fn fn;
	int op, type;
	uint32_t imm;
};
static const struct alush_case alush_cases[] = {
	{ alush_adds_lsl3, 4, 0, 3 },  { alush_subs_lsr1, 2, 1, 1 }, { alush_ands_ror5, 0, 3, 5 },
	{ alush_eors_asr32, 1, 2, 0 }, { alush_adcs_rrx, 5, 3, 0 },  { alush_orrs_lsl31, 12, 0, 31 },
};

/* ---- condiciones: devuelve 1 si la instrucción condicional se ejecutó ------ */
#define COND_FN(name, cc)                                                        \
	static uint32_t name(uint32_t fin)                                       \
	{                                                                        \
		uint32_t r;                                                      \
		__asm__ volatile("msr cpsr_f, %[fi]\n\t mov %[r], #0\n\t mov" cc " %[r], #1" \
				 : [r] "=&r"(r) : [fi] "r"(fin << 28) CLOB);   \
		return r;                                                        \
	}
COND_FN(c_eq, "eq") COND_FN(c_ne, "ne") COND_FN(c_cs, "cs") COND_FN(c_cc, "cc")
COND_FN(c_mi, "mi") COND_FN(c_pl, "pl") COND_FN(c_vs, "vs") COND_FN(c_vc, "vc")
COND_FN(c_hi, "hi") COND_FN(c_ls, "ls") COND_FN(c_ge, "ge") COND_FN(c_lt, "lt")
COND_FN(c_gt, "gt") COND_FN(c_le, "le")
static uint32_t (*const cond_fns[14])(uint32_t) = {
	c_eq, c_ne, c_cs, c_cc, c_mi, c_pl, c_vs, c_vc, c_hi, c_ls, c_ge, c_lt, c_gt, c_le,
};

/* ---- multiplicaciones ------------------------------------------------------- */
#define MULS_FN(name, insn)                                                      \
	static void name(uint32_t a, uint32_t b, uint32_t fin, uint32_t *res, uint32_t *fout) \
	{                                                                        \
		uint32_t r, f;                                                   \
		__asm__ volatile(PRE insn " %[r], %[a], %[b]" POST               \
				 : [r] "=&r"(r), [f] "=&r"(f)                   \
				 : [fi] "r"(fin << 28), [a] "r"(a), [b] "r"(b) CLOB); \
		*res = r;                                                        \
		*fout = f >> 28;                                                 \
	}
MULS_FN(op_mul, "mul")
MULS_FN(op_muls, "muls")

static uint32_t op_mla(uint32_t a, uint32_t b, uint32_t c)
{
	uint32_t r;
	__asm__ volatile("mla %[r], %[a], %[b], %[c]" : [r] "=&r"(r) : [a] "r"(a), [b] "r"(b), [c] "r"(c));
	return r;
}

#define LMUL(name, insn)                                                         \
	static void name(uint32_t a, uint32_t b, uint32_t *lo, uint32_t *hi)     \
	{                                                                        \
		uint32_t l = *lo, h = *hi;                                       \
		__asm__ volatile(insn " %[l], %[h], %[a], %[b]" : [l] "+&r"(l), [h] "+&r"(h) \
				 : [a] "r"(a), [b] "r"(b));                      \
		*lo = l;                                                         \
		*hi = h;                                                         \
	}
LMUL(op_umull, "umull")
LMUL(op_smull, "smull")
LMUL(op_umlal, "umlal")
LMUL(op_smlal, "smlal")

#ifdef ARM9
/* ---- ARMv5TE: CLZ, saturación (Q) y multiplicaciones de 16 bits ------------- */
static uint32_t op_clz(uint32_t a)
{
	uint32_t r;
	__asm__ volatile("clz %[r], %[a]" : [r] "=r"(r) : [a] "r"(a));
	return r;
}

/* Devuelve el resultado; *q = bit Q del CPSR tras la instrucción (Q parte de 0). */
#define QOP(name, insn)                                                          \
	static uint32_t name(uint32_t a, uint32_t b, uint32_t *q)                \
	{                                                                        \
		uint32_t r, f;                                                   \
		__asm__ volatile("msr cpsr_f, %[z]\n\t" insn " %[r], %[a], %[b]\n\t mrs %[f], cpsr" \
				 : [r] "=&r"(r), [f] "=&r"(f)                   \
				 : [z] "r"(0), [a] "r"(a), [b] "r"(b) CLOB);   \
		*q = (f >> 27) & 1;                                              \
		return r;                                                        \
	}
QOP(op_qadd, "qadd")
QOP(op_qsub, "qsub")
QOP(op_qdadd, "qdadd")
QOP(op_qdsub, "qdsub")

#define SMUL(name, insn)                                                         \
	static uint32_t name(uint32_t a, uint32_t b)                             \
	{                                                                        \
		uint32_t r;                                                      \
		__asm__ volatile(insn " %[r], %[a], %[b]" : [r] "=r"(r) : [a] "r"(a), [b] "r"(b)); \
		return r;                                                        \
	}
SMUL(op_smulbb, "smulbb")
SMUL(op_smulbt, "smulbt")
SMUL(op_smultb, "smultb")
SMUL(op_smultt, "smultt")
SMUL(op_smulwb, "smulwb")
SMUL(op_smulwt, "smulwt")

#define SMLA(name, insn)                                                         \
	static uint32_t name(uint32_t a, uint32_t b, uint32_t c, uint32_t *q)    \
	{                                                                        \
		uint32_t r, f;                                                   \
		__asm__ volatile("msr cpsr_f, %[z]\n\t" insn " %[r], %[a], %[b], %[c]\n\t mrs %[f], cpsr" \
				 : [r] "=&r"(r), [f] "=&r"(f)                   \
				 : [z] "r"(0), [a] "r"(a), [b] "r"(b), [c] "r"(c) CLOB); \
		*q = (f >> 27) & 1;                                              \
		return r;                                                        \
	}
SMLA(op_smlabb, "smlabb")
SMLA(op_smlabt, "smlabt")
SMLA(op_smlatb, "smlatb")
SMLA(op_smlatt, "smlatt")
SMLA(op_smlawb, "smlawb")
SMLA(op_smlawt, "smlawt")

LMUL(op_smlalbb, "smlalbb")
LMUL(op_smlalbt, "smlalbt")
LMUL(op_smlaltb, "smlaltb")
LMUL(op_smlaltt, "smlaltt")
#endif /* ARM9 */

/* ---- memoria ---------------------------------------------------------------- */
#define LD1(name, insn)                                                          \
	static uint32_t name(const void *p)                                      \
	{                                                                        \
		uint32_t r;                                                      \
		__asm__ volatile(insn " %[r], [%[p]]" : [r] "=r"(r) : [p] "r"(p) : "memory"); \
		return r;                                                        \
	}
LD1(op_ldr, "ldr")
LD1(op_ldrb, "ldrb")
LD1(op_ldrh, "ldrh")
LD1(op_ldrsb, "ldrsb")
LD1(op_ldrsh, "ldrsh")

#define ST1(name, insn)                                                          \
	static void name(void *p, uint32_t v)                                    \
	{                                                                        \
		__asm__ volatile(insn " %[v], [%[p]]" : : [v] "r"(v), [p] "r"(p) : "memory"); \
	}
ST1(op_str, "str")
ST1(op_strb, "strb")
ST1(op_strh, "strh")

#ifdef ARM9
static void op_ldrd(const void *p, uint32_t *lo, uint32_t *hi)
{
	uint32_t l, h;
	__asm__ volatile("ldrd r2, r3, [%[p]]\n\t mov %[l], r2\n\t mov %[h], r3"
			 : [l] "=r"(l), [h] "=r"(h) : [p] "r"(p) : "r2", "r3", "memory");
	*lo = l;
	*hi = h;
}
static void op_strd(void *p, uint32_t lo, uint32_t hi)
{
	__asm__ volatile("mov r2, %[l]\n\t mov r3, %[h]\n\t strd r2, r3, [%[p]]"
			 : : [l] "r"(lo), [h] "r"(hi), [p] "r"(p) : "r2", "r3", "memory");
}
#endif /* ARM9 */
static uint32_t op_swp(void *p, uint32_t v)
{
	uint32_t r;
	__asm__ volatile("swp %[r], %[v], [%[p]]" : [r] "=&r"(r) : [v] "r"(v), [p] "r"(p) : "memory");
	return r;
}
static uint32_t op_swpb(void *p, uint32_t v)
{
	uint32_t r;
	__asm__ volatile("swpb %[r], %[v], [%[p]]" : [r] "=&r"(r) : [v] "r"(v), [p] "r"(p) : "memory");
	return r;
}

/* STM/LDM de r4-r6 con escritura de vuelta; *base entra y sale. */
#define STM(name, mode)                                                          \
	static void name(uint32_t **base, uint32_t a, uint32_t b, uint32_t c)    \
	{                                                                        \
		uint32_t *p = *base;                                             \
		__asm__ volatile("mov r4, %[a]\n\t mov r5, %[b]\n\t mov r6, %[c]\n\t stm" mode " %[p]!, {r4-r6}" \
				 : [p] "+r"(p) : [a] "r"(a), [b] "r"(b), [c] "r"(c) : "r4", "r5", "r6", "memory"); \
		*base = p;                                                       \
	}
#define LDM(name, mode)                                                          \
	static void name(uint32_t **base, uint32_t *a, uint32_t *b, uint32_t *c) \
	{                                                                        \
		uint32_t *p = *base, x, y, z;                                    \
		__asm__ volatile("ldm" mode " %[p]!, {r4-r6}\n\t mov %[x], r4\n\t mov %[y], r5\n\t mov %[z], r6" \
				 : [p] "+r"(p), [x] "=r"(x), [y] "=r"(y), [z] "=r"(z) : : "r4", "r5", "r6", "memory"); \
		*base = p;                                                       \
		*a = x;                                                          \
		*b = y;                                                          \
		*c = z;                                                          \
	}
STM(op_stmia, "ia")
STM(op_stmib, "ib")
STM(op_stmda, "da")
STM(op_stmdb, "db")
LDM(op_ldmia, "ia")
LDM(op_ldmib, "ib")
LDM(op_ldmda, "da")
LDM(op_ldmdb, "db")

/* ---- Thumb (thumb.S) --------------------------------------------------------- */
typedef uint32_t (*thumb_fn)(void);
#define TH(name) extern uint32_t name(void)
TH(th_ands); TH(th_eors); TH(th_lsls); TH(th_lsrs); TH(th_asrs); TH(th_adcs); TH(th_sbcs); TH(th_rors);
TH(th_tst); TH(th_negs); TH(th_cmp); TH(th_cmn); TH(th_orrs); TH(th_muls); TH(th_bics); TH(th_mvns);
TH(th_adds_rrr); TH(th_subs_rrr); TH(th_adds_i8); TH(th_subs_i8); TH(th_cmp_i8); TH(th_movs_i8);
TH(th_lsls_i5); TH(th_lsrs_i5); TH(th_asrs_i5); TH(th_lsls_i0); TH(th_lsrs_i32); TH(th_asrs_i32);
TH(th_getlr);
extern uint32_t th_add(uint32_t, uint32_t); /* ARM -> Thumb por bl/blx */
TH(th_beq); TH(th_bne); TH(th_bcs); TH(th_bcc); TH(th_bmi); TH(th_bpl); TH(th_bvs); TH(th_bvc);
TH(th_bhi); TH(th_bls); TH(th_bge); TH(th_blt); TH(th_bgt); TH(th_ble);

/* Llama a una función Thumb con las banderas puestas y las devuelve. */
static void call_thumb(thumb_fn fn, uint32_t a, uint32_t b, uint32_t fin, uint32_t *res, uint32_t *fout)
{
	uint32_t r, f;
#ifdef ARM9
	__asm__ volatile("msr cpsr_f, %[fi]\n\t mov r0, %[a]\n\t mov r1, %[b]\n\t blx %[fn]\n\t"
			 "mrs %[f], cpsr\n\t mov %[r], r0"
			 : [r] "=&r"(r), [f] "=&r"(f)
			 : [fi] "r"(fin << 28), [a] "r"(a), [b] "r"(b), [fn] "r"(fn)
			 : "r0", "r1", "r2", "r3", "lr", "cc", "memory");
#else
	__asm__ volatile("msr cpsr_f, %[fi]\n\t mov r0, %[a]\n\t mov r1, %[b]\n\t adr lr, 1f\n\t bx %[fn]\n\t"
			 "1: mrs %[f], cpsr\n\t mov %[r], r0"
			 : [r] "=&r"(r), [f] "=&r"(f)
			 : [fi] "r"(fin << 28), [a] "r"(a), [b] "r"(b), [fn] "r"(fn)
			 : "r0", "r1", "r2", "r3", "lr", "cc", "memory");
#endif
	*res = r;
	*fout = f >> 28;
}

#endif
