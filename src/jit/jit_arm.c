/*
 * SalsaDS — traductor ARM → PowerPC64 de la Fase 0.7. Ver jit_arm.h.
 *
 * Una pasada sobre el programa entero (son bucles chicos): bloques básicos,
 * un encabezado por bloque que comprueba que quepa en el presupuesto de
 * instrucciones hasta el próximo evento y lo descuenta, y los saltos del
 * guest como saltos del host. Registros: guest r0–r12 → r14–r26; r27 base de
 * la memoria del guest; r28 presupuesto; r29–r31 banderas perezosas; r9, r10
 * temporales. Sin formas con punto, `mfcr`, `mtcrf` ni desplazamientos por
 * registro.
 *
 * Las banderas se resuelven en tiempo de traducción: dentro de un bloque se
 * sabe cuál fue la última instrucción que las fijó (resta/comparación o
 * suma), así que el consumidor elige su comparación sin guardar un «tipo».
 * Toda condición que no sea N/V sola (MI, PL, VS, VC) y los signos tras una
 * suma quedan fuera del subconjunto: JA_E_FLAGS.
 */
#include <string.h>

#include "jit/armenc.h"
#include "jit/jit_arm.h"
#include "jit/ppcenc.h"

const char *const ja_flagmode_name[JA_FLAGS_COUNT] = {
	"perezosas", "CR por instrucción", "cmp+salto fusionados (perezosas si no)",
};
struct ja_tr_stats ja_last_tr;

#define MAXG 64
#define MAXFX 512
#define FRAME 0x120
#define STATE_SAVE 0x60
#define REG_SAVE 0x80

#define G(i) (14u + (i))
#define R_MEM 27u
#define R_BUD 28u
#define R_LA 29u
#define R_LB 30u
#define R_LR 31u
#define T0 9u
#define T1 10u

enum ft { FT_NONE, FT_SUB_LAZY, FT_ADD_LAZY, FT_SUB_CR, FT_ADD_CR };
enum { FX_BC, FX_B };
enum { TG_GUEST, TG_STUB, TG_EXIT };

struct fixup {
	size_t at;
	int kind, tkind, tid;
	unsigned bo, bi;
};

struct tr {
	uint32_t *code;
	size_t cap, n;
	int err;
	enum ja_flagmode mode;
	const uint32_t *prog;
	uint32_t ng;
	uint8_t leader[MAXG];
	uint32_t blen[MAXG];
	size_t host_at[MAXG], stub_at[MAXG];
	struct fixup fx[MAXFX];
	int nfx;
	enum ft ft;
};

static void put(struct tr *t, uint32_t w)
{
	if (t->n >= t->cap) {
		t->err = JA_E_NOSPACE;
		return;
	}
	t->code[t->n++] = w;
}

static void fixup(struct tr *t, int kind, unsigned bo, unsigned bi, int tkind, int tid)
{
	if (t->nfx >= MAXFX) {
		t->err = JA_E_NOSPACE;
		return;
	}
	t->fx[t->nfx++] = (struct fixup){ t->n, kind, tkind, tid, bo, bi };
	put(t, kind == FX_B ? ppc_b(0) : ppc_bc(bo, bi, 0));
}

static uint32_t ror32(uint32_t x, unsigned n)
{
	n &= 31;
	return n ? (x >> n) | (x << (32 - n)) : x;
}

static void load_imm(struct tr *t, unsigned rd, uint32_t v)
{
	if (v <= 0x7FFF) {
		put(t, ppc_li(rd, (int32_t)v));
	} else if (v <= 0xFFFF) {
		put(t, ppc_li(rd, 0));
		put(t, ppc_ori(rd, rd, v));
	} else {
		put(t, ppc_lis(rd, (int32_t)(int16_t)(v >> 16)));
		if (v & 0xFFFF)
			put(t, ppc_ori(rd, rd, v & 0xFFFF));
		if (v & 0x80000000u)
			put(t, ppc_clrldi(rd, rd, 32));
	}
}

/* ---- Operando 2 -------------------------------------------------------- */
struct op2 {
	int is_imm;
	uint32_t imm;
	unsigned reg;  /* registro del host con el valor (si !is_imm) */
};

static int op2_decode(struct tr *t, uint32_t w, struct op2 *o, unsigned scratch)
{
	if ((w >> 25) & 1) {
		o->is_imm = 1;
		o->imm = ror32(w & 0xFF, ((w >> 8) & 15) * 2);
		return 0;
	}
	if (w & 0x10)
		return JA_E_UNSUPPORTED;  /* desplazamiento por registro */
	const unsigned rm = w & 15, amt = (w >> 7) & 31, ty = (w >> 5) & 3;
	if (rm > 12)
		return JA_E_UNSUPPORTED;
	o->is_imm = 0;
	if (ty == ARM_LSL && amt == 0) {
		o->reg = G(rm);
		return 0;
	}
	o->reg = scratch;
	switch (ty) {
	case ARM_LSL:
		put(t, ppc_rlwinm(scratch, G(rm), amt, 0, 31 - amt));
		break;
	case ARM_LSR:
		if (amt == 0)
			put(t, ppc_li(scratch, 0));
		else
			put(t, ppc_rlwinm(scratch, G(rm), 32 - amt, amt, 31));
		break;
	case ARM_ASR:
		put(t, ppc_extsw(scratch, G(rm)));
		put(t, ppc_sradi(scratch, scratch, amt ? amt : 63));
		put(t, ppc_clrldi(scratch, scratch, 32));
		break;
	default:
		if (amt == 0)
			return JA_E_UNSUPPORTED;  /* RRX */
		put(t, ppc_rlwinm(scratch, G(rm), 32 - amt, 0, 31));
		break;
	}
	return 0;
}

/* Deja el operando en un registro y lo devuelve. */
static unsigned op2_to_reg(struct tr *t, const struct op2 *o, unsigned scratch)
{
	if (!o->is_imm)
		return o->reg;
	load_imm(t, scratch, o->imm);
	return scratch;
}

/* ---- Banderas ------------------------------------------------------------ */

static int flags_live_from(const struct tr *t, uint32_t pc, uint8_t *seen)
{
	while (pc < t->ng) {
		if (seen[pc])
			return 0;
		seen[pc] = 1;
		const uint32_t w = t->prog[pc];
		if ((w >> 25 & 7) == 5) {
			if ((w >> 28) != ARM_AL)
				return 1;
			pc = pc + 2 + (uint32_t)((int32_t)(w << 8) >> 8);
			continue;
		}
		if (w == ARM_SWI)
			return 0;
		if ((w >> 26 & 3) == 0) {
			const unsigned op = (w >> 21) & 15;
			if (op == ARM_ADC)
				return 1;
			if ((w >> 20) & 1) {
				if (op == ARM_ADD || op == ARM_SUB || op == ARM_CMP)
					return 0;
				return 1;  /* S lógico: deja C y V como estaban */
			}
		}
		pc++;
	}
	return 0;
}

/* Salta a `target` (invocando la condición `cc` según el último que fijó banderas).
 * En perezosas hace antes la comparación. Devuelve 0 o JA_E_FLAGS. */
static int emit_cond_branch(struct tr *t, unsigned cc, int tkind, int tid)
{
	const int lazy = t->ft == FT_SUB_LAZY || t->ft == FT_ADD_LAZY;
	const int sub = t->ft == FT_SUB_LAZY || t->ft == FT_SUB_CR;
	if (t->ft == FT_NONE)
		return JA_E_FLAGS;
	if (sub) {
		switch (cc) {
		case ARM_EQ: case ARM_NE: case ARM_GE: case ARM_LT: case ARM_GT: case ARM_LE:
			if (lazy)
				put(t, ppc_cmpw(0, R_LA, R_LB));
			switch (cc) {
			case ARM_EQ: fixup(t, FX_BC, PPC_BO_TRUE, PPC_BI(0, PPC_EQ), tkind, tid); break;
			case ARM_NE: fixup(t, FX_BC, PPC_BO_FALSE, PPC_BI(0, PPC_EQ), tkind, tid); break;
			case ARM_GE: fixup(t, FX_BC, PPC_BO_FALSE, PPC_BI(0, PPC_LT), tkind, tid); break;
			case ARM_LT: fixup(t, FX_BC, PPC_BO_TRUE, PPC_BI(0, PPC_LT), tkind, tid); break;
			case ARM_GT: fixup(t, FX_BC, PPC_BO_TRUE, PPC_BI(0, PPC_GT), tkind, tid); break;
			default: fixup(t, FX_BC, PPC_BO_FALSE, PPC_BI(0, PPC_GT), tkind, tid); break;
			}
			return 0;
		case ARM_CS: case ARM_CC: case ARM_HI: case ARM_LS:
			if (lazy)
				put(t, ppc_cmplw(1, R_LA, R_LB));
			switch (cc) {
			case ARM_CS: fixup(t, FX_BC, PPC_BO_FALSE, PPC_BI(1, PPC_LT), tkind, tid); break;
			case ARM_CC: fixup(t, FX_BC, PPC_BO_TRUE, PPC_BI(1, PPC_LT), tkind, tid); break;
			case ARM_HI: fixup(t, FX_BC, PPC_BO_TRUE, PPC_BI(1, PPC_GT), tkind, tid); break;
			default: fixup(t, FX_BC, PPC_BO_FALSE, PPC_BI(1, PPC_GT), tkind, tid); break;
			}
			return 0;
		default:
			return JA_E_FLAGS;
		}
	}
	/* tras una suma: Z con cr0 (resultado contra 0), C con cr1 (resultado contra un sumando) */
	switch (cc) {
	case ARM_EQ: case ARM_NE:
		if (lazy)
			put(t, ppc_cmpwi(0, R_LR, 0));
		fixup(t, FX_BC, cc == ARM_EQ ? PPC_BO_TRUE : PPC_BO_FALSE, PPC_BI(0, PPC_EQ), tkind, tid);
		return 0;
	case ARM_CS: case ARM_CC:
		if (lazy)
			put(t, ppc_cmplw(1, R_LR, R_LA));
		fixup(t, FX_BC, cc == ARM_CS ? PPC_BO_TRUE : PPC_BO_FALSE, PPC_BI(1, PPC_LT), tkind, tid);
		return 0;
	case ARM_HI: case ARM_LS:
		if (lazy) {
			put(t, ppc_cmpwi(0, R_LR, 0));
			put(t, ppc_cmplw(1, R_LR, R_LA));
		}
		if (cc == ARM_HI) {
			/* C y no Z: si Z, no salta; si no, salta si C */
			put(t, ppc_bc(PPC_BO_TRUE, PPC_BI(0, PPC_EQ), 8));
			fixup(t, FX_BC, PPC_BO_TRUE, PPC_BI(1, PPC_LT), tkind, tid);
		} else {
			fixup(t, FX_BC, PPC_BO_TRUE, PPC_BI(0, PPC_EQ), tkind, tid);
			fixup(t, FX_BC, PPC_BO_FALSE, PPC_BI(1, PPC_LT), tkind, tid);
		}
		return 0;
	default:
		return JA_E_FLAGS;
	}
}

/* ---- Instrucciones ----------------------------------------------------------- */

static int emit_dp(struct tr *t, uint32_t pc, uint32_t w, unsigned *fuse_cc)
{
	const unsigned op = (w >> 21) & 15, s = (w >> 20) & 1, rn = (w >> 16) & 15, rd = (w >> 12) & 15;
	struct op2 o = { 0, 0, 0 };
	int rc;
	if (rn > 12 || rd > 12)
		return JA_E_UNSUPPORTED;
	if (op != ARM_MOV && op != ARM_CMP && op != ARM_AND && op != ARM_EOR && op != ARM_ORR && op != ARM_ADD &&
	    op != ARM_SUB && op != ARM_ADC)
		return JA_E_UNSUPPORTED;
	if ((s && op != ARM_ADD && op != ARM_SUB && op != ARM_CMP) || (op == ARM_CMP && !s))
		return JA_E_UNSUPPORTED;
	if ((rc = op2_decode(t, w, &o, T0)) != 0)
		return rc;
	const unsigned d = G(rd), a = G(rn);

	switch (op) {
	case ARM_MOV:
		if (o.is_imm)
			load_imm(t, d, o.imm);
		else if (o.reg != d)
			put(t, ppc_mr(d, o.reg));
		return 0;
	case ARM_AND:
		if (o.is_imm && o.imm && !(o.imm & (o.imm + 1)))
			put(t, ppc_rlwinm(d, a, 0, 32 - (unsigned)__builtin_popcount(o.imm), 31));
		else
			put(t, ppc_and(d, a, op2_to_reg(t, &o, T1)));
		return 0;
	case ARM_EOR:
		if (o.is_imm && o.imm <= 0xFFFF)
			put(t, ppc_xori(d, a, o.imm));
		else
			put(t, ppc_xor(d, a, op2_to_reg(t, &o, T1)));
		return 0;
	case ARM_ORR:
		if (o.is_imm && o.imm <= 0xFFFF)
			put(t, ppc_ori(d, a, o.imm));
		else
			put(t, ppc_or(d, a, op2_to_reg(t, &o, T1)));
		return 0;
	default:
		break;
	}

	/* ADD, SUB, ADC, CMP */
	const int is_sub = op == ARM_SUB || op == ARM_CMP;
	const int setter = s != 0;
	const int cr = t->mode == JA_FLAGS_CR;
	/* ¿Se puede fusionar con el salto siguiente? */
	unsigned fcc = 0;
	int fuse = 0;
	if (setter && t->mode == JA_FLAGS_FUSED && pc + 1 < t->ng && (t->prog[pc + 1] >> 25 & 7) == 5 &&
	    (t->prog[pc + 1] >> 28) != ARM_AL && !t->leader[pc + 1]) {
		const uint32_t b = t->prog[pc + 1];
		const unsigned cc = b >> 28;
		const uint32_t tgt = pc + 1 + 2 + (uint32_t)((int32_t)(b << 8) >> 8);
		uint8_t seen[MAXG];
		memset(seen, 0, sizeof seen);
		int live = flags_live_from(t, tgt, seen);
		memset(seen, 0, sizeof seen);
		live = live || flags_live_from(t, pc + 2, seen);
		const int cc_ok = is_sub ? 1 : (cc == ARM_EQ || cc == ARM_NE);
		if (!live && cc_ok) {
			fuse = 1;
			fcc = cc;
		}
	}
	*fuse_cc = fuse ? fcc : 0xFFu;

	if (is_sub && setter) {
		/* comparaciones antes de escribir rd (SUBS rd = rn) */
		if (cr || fuse) {
			const unsigned need_signed = cr ? 1 : (fcc == ARM_EQ || fcc == ARM_NE || fcc >= ARM_GE);
			const unsigned need_unsigned = cr ? 1 : (fcc == ARM_CS || fcc == ARM_CC || fcc == ARM_HI || fcc == ARM_LS);
			if (o.is_imm && o.imm <= 0x7FFF) {
				if (need_signed)
					put(t, ppc_cmpwi(0, a, (int32_t)o.imm));
				if (need_unsigned)
					put(t, ppc_cmplwi(1, a, o.imm));
			} else {
				const unsigned b = op2_to_reg(t, &o, T1);
				if (need_signed)
					put(t, ppc_cmpw(0, a, b));
				if (need_unsigned)
					put(t, ppc_cmplw(1, a, b));
			}
			t->ft = FT_SUB_CR;
		} else {
			put(t, ppc_mr(R_LA, a));
			if (o.is_imm)
				load_imm(t, R_LB, o.imm);
			else
				put(t, ppc_mr(R_LB, o.reg));
			t->ft = FT_SUB_LAZY;
		}
		if (op == ARM_CMP)
			return 0;
	}

	if (op == ARM_ADC) {
		if (t->ft != FT_SUB_CR && t->ft != FT_ADD_CR && t->ft != FT_SUB_LAZY && t->ft != FT_ADD_LAZY)
			return JA_E_FLAGS;
		const int lazy = t->ft == FT_SUB_LAZY || t->ft == FT_ADD_LAZY;
		const int sub = t->ft == FT_SUB_LAZY || t->ft == FT_SUB_CR;
		unsigned fcr = 1;
		if (lazy) {
			fcr = 7;
			if (sub)
				put(t, ppc_cmplw(7, R_LA, R_LB));
			else
				put(t, ppc_cmplw(7, R_LR, R_LA));
		}
		/* C de la suma: LR < LA; de la resta: !(LA < LB) */
		if (o.is_imm && o.imm <= 0x7FFF) {
			put(t, ppc_addi(d, a, (int32_t)o.imm));
		} else {
			put(t, ppc_add(d, a, op2_to_reg(t, &o, T1)));
		}
		put(t, ppc_bc(sub ? PPC_BO_TRUE : PPC_BO_FALSE, PPC_BI(fcr, PPC_LT), 8));
		put(t, ppc_addi(d, d, 1));
		put(t, ppc_clrldi(d, d, 32));
		return 0;
	}

	/* ADD / SUB (con o sin S): el resultado */
	if (op == ARM_ADD && setter) {
		/* el acarreo se mide con el resultado contra un sumando que sobreviva */
		if (!cr && !fuse) {
			put(t, ppc_mr(R_LA, a));
		}
	}
	if (op == ARM_ADD) {
		if (o.is_imm && o.imm <= 0x7FFF)
			put(t, ppc_addi(d, a, (int32_t)o.imm));
		else
			put(t, ppc_add(d, a, op2_to_reg(t, &o, T1)));
	} else {
		if (o.is_imm && o.imm <= 0x7FFF)
			put(t, ppc_addi(d, a, -(int32_t)o.imm));
		else
			put(t, ppc_subf(d, op2_to_reg(t, &o, T1), a));
	}
	put(t, ppc_clrldi(d, d, 32));
	if (op == ARM_ADD && setter) {
		if (cr || fuse) {
			/* Z: resultado contra 0; C: resultado contra un sumando que no sea rd */
			put(t, ppc_cmpwi(0, d, 0));
			if (!fuse || fcc != ARM_EQ) {
				if (!fuse && cr) {
					/* sumando que sobreviva: la otra fuente, o el inmediato */
					if (o.is_imm && o.imm <= 0xFFFF)
						put(t, ppc_cmplwi(1, d, o.imm));
					else if (!o.is_imm && o.reg != d)
						put(t, ppc_cmplw(1, d, o.reg));
					else if (a != d)
						put(t, ppc_cmplw(1, d, a));
					else
						return JA_E_UNSUPPORTED;  /* ADDS rd,rd,rd o inmediato grande: sin copia en este modo */
				}
			}
			t->ft = FT_ADD_CR;
		} else {
			put(t, ppc_mr(R_LR, d));
			t->ft = FT_ADD_LAZY;
		}
	}
	if (op == ARM_SUB && setter && !cr && !fuse) {
		/* ya guardados LA/LB arriba */
	}
	return 0;
}

static int emit_mem(struct tr *t, uint32_t w)
{
	const unsigned rn = (w >> 16) & 15, rd = (w >> 12) & 15, byte = (w >> 22) & 1, load = (w >> 20) & 1;
	const uint32_t off = w & 0xFFF;
	if (rn > 12 || rd > 12 || !((w >> 24) & 1) || !((w >> 23) & 1) || ((w >> 21) & 1) || ((w >> 25) & 1))
		return JA_E_UNSUPPORTED;
	if (off)
		put(t, ppc_addi(T0, G(rn), (int32_t)off));
	put(t, ppc_rlwinm(T0, off ? T0 : G(rn), 0, 18, byte ? 31 : 29));
	if (load)
		put(t, byte ? ppc_lbzx(G(rd), R_MEM, T0) : ppc_lwbrx(G(rd), R_MEM, T0));
	else
		put(t, byte ? ppc_stbx(G(rd), R_MEM, T0) : ppc_stwbrx(G(rd), R_MEM, T0));
	return 0;
}

int ja_translate(const uint32_t *prog, uint32_t nwords, enum ja_flagmode mode, uint32_t *code, size_t cap,
		 size_t *nwords_out)
{
	struct tr t;
	memset(&t, 0, sizeof t);
	t.code = code;
	t.cap = cap;
	t.mode = mode;
	t.prog = prog;
	t.ng = nwords;
	if (nwords == 0 || nwords > MAXG)
		return JA_E_UNSUPPORTED;
	memset(&ja_last_tr, 0, sizeof ja_last_tr);

	/* bloques básicos */
	t.leader[0] = 1;
	for (uint32_t i = 0; i < nwords; i++) {
		const uint32_t w = prog[i];
		if ((w >> 25 & 7) == 5) {
			const uint32_t tg = i + 2 + (uint32_t)((int32_t)(w << 8) >> 8);
			if (tg >= nwords)
				return JA_E_UNSUPPORTED;
			t.leader[tg] = 1;
			if (i + 1 < nwords)
				t.leader[i + 1] = 1;
		} else if (w == ARM_SWI && i + 1 < nwords) {
			t.leader[i + 1] = 1;
		}
	}
	for (uint32_t i = 0; i < nwords; i++) {
		if (!t.leader[i])
			continue;
		uint32_t j = i + 1;
		while (j < nwords && !t.leader[j])
			j++;
		t.blen[i] = j - i;
		ja_last_tr.blocks++;
	}

	/* prólogo */
	put(&t, ppc_mflr(0));
	put(&t, ppc_std(0, 1, 16));
	put(&t, ppc_stdu(1, 1, -FRAME));
	for (unsigned r = 14; r <= 31; r++)
		put(&t, ppc_std(r, 1, REG_SAVE + 8 * (int32_t)(r - 14)));
	put(&t, ppc_std(3, 1, STATE_SAVE));
	put(&t, ppc_ld(R_MEM, 3, 88));
	put(&t, ppc_ld(R_BUD, 3, 80));
	for (unsigned i = 0; i <= 12; i++)
		put(&t, ppc_lwz(G(i), 3, (int32_t)(4 * i)));

	int err = 0;
	unsigned fuse_cc = 0xFF;
	for (uint32_t pc = 0; pc < nwords && !err && !t.err; pc++) {
		const uint32_t w = prog[pc];
		if (t.leader[pc]) {
			t.host_at[pc] = t.n;
			t.ft = FT_NONE;
			/* encabezado: ¿cabe el bloque en el presupuesto? */
			put(&t, ppc_cmpdi(7, R_BUD, (int32_t)t.blen[pc]));
			fixup(&t, FX_BC, PPC_BO_TRUE, PPC_BI(7, PPC_LT), TG_STUB, (int)pc);
			put(&t, ppc_addi(R_BUD, R_BUD, -(int32_t)t.blen[pc]));
		}
		if (w == ARM_SWI) {
			put(&t, ppc_li(4, (int32_t)(pc * 4)));
			put(&t, ppc_li(5, JA_EXIT_SWI));
			fixup(&t, FX_B, 0, 0, TG_EXIT, 0);
		} else if ((w >> 25 & 7) == 5) {
			const unsigned cc = w >> 28;
			const uint32_t tg = pc + 2 + (uint32_t)((int32_t)(w << 8) >> 8);
			if (cc == ARM_AL) {
				fixup(&t, FX_B, 0, 0, TG_GUEST, (int)tg);
			} else {
				err = emit_cond_branch(&t, cc, TG_GUEST, (int)tg);
				if (!err && fuse_cc != 0xFF)
					ja_last_tr.fused++;
			}
			fuse_cc = 0xFF;
		} else if ((w >> 28) != ARM_AL) {
			err = JA_E_UNSUPPORTED;
		} else if ((w >> 26 & 3) == 0) {
			err = emit_dp(&t, pc, w, &fuse_cc);
		} else if ((w >> 26 & 3) == 1) {
			err = emit_mem(&t, w);
		} else {
			err = JA_E_UNSUPPORTED;
		}
	}
	if (err)
		return err;
	if (t.err)
		return t.err;

	/* salidas por presupuesto, una por bloque */
	for (uint32_t pc = 0; pc < nwords; pc++) {
		if (!t.leader[pc])
			continue;
		t.stub_at[pc] = t.n;
		put(&t, ppc_li(4, (int32_t)(pc * 4)));
		put(&t, ppc_li(5, JA_EXIT_BUDGET));
		fixup(&t, FX_B, 0, 0, TG_EXIT, 0);
	}
	/* salida común */
	const size_t exit_at = t.n;
	put(&t, ppc_ld(3, 1, STATE_SAVE));
	for (unsigned i = 0; i <= 12; i++)
		put(&t, ppc_stw(G(i), 3, (int32_t)(4 * i)));
	put(&t, ppc_stw(4, 3, 68));
	put(&t, ppc_stw(5, 3, 72));
	put(&t, ppc_std(R_BUD, 3, 80));
	for (unsigned r = 14; r <= 31; r++)
		put(&t, ppc_ld(r, 1, REG_SAVE + 8 * (int32_t)(r - 14)));
	put(&t, ppc_mr(3, 5));
	put(&t, ppc_addi(1, 1, FRAME));
	put(&t, ppc_ld(0, 1, 16));
	put(&t, ppc_mtlr(0));
	put(&t, PPC_BLR);
	if (t.err)
		return t.err;

	for (int i = 0; i < t.nfx; i++) {
		const struct fixup *f = &t.fx[i];
		size_t dst = f->tkind == TG_EXIT ? exit_at : f->tkind == TG_STUB ? t.stub_at[f->tid] : t.host_at[f->tid];
		const int32_t off = (int32_t)((int64_t)dst - (int64_t)f->at) * 4;
		t.code[f->at] = f->kind == FX_B ? ppc_b(off) : ppc_bc(f->bo, f->bi, off);
	}
	ja_last_tr.guest_instr = nwords;
	ja_last_tr.host_instr = (uint32_t)t.n;
	*nwords_out = t.n;
	return 0;
}

int ja_call(const uint32_t *code, struct ja_state *s)
{
#if defined(__powerpc64__)
	uint64_t toc;
	__asm__ volatile("mr %0,2" : "=r"(toc));
	uint64_t opd[3] = { (uint64_t)(uintptr_t)code, toc, 0 };
	/* Sin esto el compilador descarta las escrituras del descriptor (solo se lee por el cast). */
	__asm__ volatile("" : : "r"(opd) : "memory");
	int (*fn)(struct ja_state *) = (int (*)(struct ja_state *))(void *)opd;
	return fn(s);
#else
	(void)code;
	(void)s;
	return -1;
#endif
}
