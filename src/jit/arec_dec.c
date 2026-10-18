/*
 * SalsaDS — recompilador: decodificación de ARM (ARMv5TE) y Thumb a una representación común (struct ai).
 *
 * Thumb se traduce a la instrucción ARM equivalente cuando la semántica (resultado, banderas y ciclos del
 * intérprete) es la misma; lo que no tiene equivalente exacto o es raro va como AI_CALL (lo ejecuta el
 * intérprete con la instrucción original). La clasificación sigue a la de src/cpu/acore_arm.c y acore_thumb.c,
 * incluidos los ciclos que suma cada manejador. Código propio (GPL-2.0-or-later).
 */
#include <string.h>

#include "jit/arec_priv.h"
#include "util/endian.h"

static const uint8_t cond_reads_tab[16] = {
	F_Z, F_Z, F_C, F_C, F_N, F_N, F_V, F_V, F_C | F_Z, F_C | F_Z, F_N | F_V, F_N | F_V,
	F_N | F_Z | F_V, F_N | F_Z | F_V, 0, 0,
};

uint8_t arec_cond_reads(unsigned cond)
{
	return cond_reads_tab[cond & 15];
}

static uint32_t ror32(uint32_t x, unsigned n)
{
	n &= 31;
	return n ? (x >> n) | (x << (32 - n)) : x;
}

static unsigned popcount16(uint32_t x)
{
	unsigned n = 0;

	for (x &= 0xFFFF; x; x &= x - 1)
		n++;
	return n;
}

static void callout(struct ai *a)
{
	a->kind = AI_CALL;
	a->cyc = 1;
	a->fr = F_ALL;
	a->fw = 0;
	a->ends = 0;
}

/* banderas de una operación de datos con S */
static void dp_flags(struct ai *a)
{
	const int logical = a->op == 0 || a->op == 1 || a->op == 8 || a->op == 9 || a->op >= 12;

	if (a->op == 5 || a->op == 6 || a->op == 7)
		a->fr |= F_C; /* ADC, SBC, RSC */
	if (a->o2 == O2_SHI && a->sht == 3 && a->sha == 0)
		a->fr |= F_C; /* RRX */
	if (!a->s)
		return;
	if (!logical) {
		a->fw = F_ALL;
		return;
	}
	a->fw = F_N | F_Z;
	if (a->o2 == O2_IMM) {
		if (a->ic >= 0)
			a->fw |= F_C;
	} else if (a->o2 == O2_SHI) {
		if (!(a->sht == 0 && a->sha == 0))
			a->fw |= F_C;
	} else {
		/* la cantidad 0 deja el acarreo: se lee el de antes y el nuevo sale siempre del ayudante (lo escribe) */
		a->fr |= F_C;
		a->fw |= F_C;
	}
}

/* ---- ARM ---------------------------------------------------------------------------------------------- */

static void dec_arm(uint32_t addr, uint32_t insn, struct ai *a)
{
	const uint32_t cond = insn >> 28;
	const unsigned hi = (insn >> 20) & 0xFF, lo = (insn >> 4) & 0xF;
	const unsigned b27_25 = hi >> 5;

	memset(a, 0, sizeof *a);
	a->addr = addr;
	a->raw = insn;
	a->size = 4;
	a->ninsn = 1;
	a->cond = (uint8_t)cond;
	a->cyc = 1;

	if (cond == 0xF) {
		a->cond = AI_COND_AL;
		if ((insn & 0x0E000000u) == 0x0A000000u) { /* BLX inmediato */
			const int32_t off = ((int32_t)(insn << 8) >> 6) + (int32_t)((insn >> 24) & 1) * 2;

			a->kind = AI_B;
			a->target = (addr + 8 + (uint32_t)off) | 1u;
			a->link = 1;
			a->lrv = addr + 4;
			a->cyc = 3;
			a->ends = 1;
			return;
		}
		if ((insn & 0x0C50F000u) == 0x0450F000u) { /* PLD */
			a->kind = AI_NOP;
			return;
		}
		a->cond = 0xF;
		callout(a);
		return;
	}

	switch (b27_25) {
	case 0:
	case 1:
		if (b27_25 == 0 && (lo & 9) == 9) {
			if ((lo & 6) == 0) {
				if ((hi & 0xFC) == 0x00) { /* MUL / MLA */
					a->kind = AI_MUL;
					a->rd = (insn >> 16) & 15;
					a->rn = (insn >> 12) & 15;
					a->rs = (insn >> 8) & 15;
					a->rm = insn & 15;
					a->acc = (insn >> 21) & 1;
					a->s = (insn >> 20) & 1;
					a->cyc = (uint8_t)(1 + a->acc);
					if (a->s)
						a->fw = F_N | F_Z;
					if (a->rd == 15 || a->rm == 15 || a->rs == 15 || (a->acc && a->rn == 15))
						callout(a);
					break;
				}
				if ((hi & 0xF8) == 0x08) { /* UMULL / UMLAL / SMULL / SMLAL */
					a->kind = AI_MULL;
					a->rd = (insn >> 16) & 15; /* hi */
					a->rn = (insn >> 12) & 15; /* lo */
					a->rs = (insn >> 8) & 15;
					a->rm = insn & 15;
					a->sgn = (insn >> 22) & 1;
					a->acc = (insn >> 21) & 1;
					a->s = (insn >> 20) & 1;
					a->cyc = (uint8_t)(2 + a->acc);
					if (a->s)
						a->fw = F_N | F_Z;
					if (a->rd == 15 || a->rn == 15 || a->rm == 15 || a->rs == 15 || a->rd == a->rn)
						callout(a);
					break;
				}
				callout(a); /* SWP y lo indefinido */
				if ((insn & 0x0FB00FF0u) == 0x01000090u) { /* SWP / SWPB */
					a->rn = (insn >> 16) & 15;
					a->rd = (insn >> 12) & 15;
					a->rm = insn & 15;
					if (a->rn != 15 && a->rd != 15 && a->rm != 15) {
						a->fast = FK_SWP;
						a->call_ro = 1;
						a->fr = 0;
						a->msz = (insn & (1u << 22)) ? 1 : 4;
						a->cyc = 4;
					}
				}
				break;
			}
			/* LDRH / STRH / LDRSB / LDRSH (LDRD / STRD al intérprete) */
			{
				const int load = hi & 1;

				if (!load && lo != 0xB) {
					callout(a);
					/* LDRD (lo = D) / STRD (lo = F): par par distinto de r14, sin r15 */
					a->rn = (insn >> 16) & 15;
					a->rd = (insn >> 12) & 15;
					a->pre = (insn >> 24) & 1;
					a->up = (insn >> 23) & 1;
					a->wb = !a->pre || ((insn >> 21) & 1);
					if (insn & (1u << 22)) {
						a->o2 = O2_IMM;
						a->imm = ((insn >> 4) & 0xF0u) | (insn & 0xFu);
					} else {
						a->o2 = O2_SHI;
						a->rm = insn & 15;
					}
					if (!(a->rd & 1) && a->rd != 14 && a->rn != 15 && !(a->o2 == O2_SHI && a->rm == 15)) {
						a->fast = FK_LDRD;
						a->call_ro = 1;
						a->fr = 0;
						a->load = lo == 0xD;
						a->cyc = a->load ? 4 : 3;
					}
					break;
				}
				a->kind = AI_MEM;
				a->load = (uint8_t)load;
				a->msz = lo == 0xB ? 2 : lo == 0xD ? 1 : 2;
				a->msign = load && lo != 0xB;
				a->rn = (insn >> 16) & 15;
				a->rd = (insn >> 12) & 15;
				a->pre = (insn >> 24) & 1;
				a->up = (insn >> 23) & 1;
				a->wb = !a->pre || ((insn >> 21) & 1);
				if (insn & (1u << 22)) {
					a->o2 = O2_IMM;
					a->imm = ((insn >> 4) & 0xF0u) | (insn & 0xFu);
				} else {
					a->o2 = O2_SHI;
					a->rm = insn & 15;
					a->sht = 0;
					a->sha = 0;
				}
				a->pcv = addr + 8;
				a->cyc = load ? 3 : 2;
				if (a->rd == 15 || (a->wb && a->rn == 15) || (a->o2 == O2_SHI && a->rm == 15))
					callout(a);
			}
			break;
		}
		if ((hi & 0xF9) == 0x10 || (b27_25 == 1 && (hi & 0xF9) == 0x30)) {
			/* MRS, MSR, BX, BLX, CLZ, QADD..., BKPT, SMULxy */
			if (b27_25 == 0 && lo == 0x1 && hi == 0x12) {
				a->kind = AI_BX;
				a->rm = insn & 15;
				a->cyc = 3;
				a->ends = 1;
				a->xchg = 1;
				if (a->rm == 15)
					callout(a);
				break;
			}
			if (b27_25 == 0 && lo == 0x3 && hi == 0x12) {
				a->kind = AI_BX;
				a->rm = insn & 15;
				a->link = 1;
				a->lrv = addr + 4;
				a->cyc = 3;
				a->ends = 1;
				a->xchg = 1;
				if (a->rm == 15)
					callout(a);
				break;
			}
			if (b27_25 == 0 && lo == 0x1 && hi == 0x16) {
				a->kind = AI_CLZ;
				a->rd = (insn >> 12) & 15;
				a->rm = insn & 15;
				if (a->rd == 15 || a->rm == 15)
					callout(a);
				break;
			}
			callout(a);
			if ((insn & 0x0FBF0FFFu) == 0x010F0000u && ((insn >> 12) & 15) != 15) { /* MRS */
				a->fast = FK_MRS;
				a->rd = (insn >> 12) & 15;
			} else if (((insn & 0x0FB0FFF0u) == 0x0120F000u && (insn & 15) != 15) ||
				   (insn & 0x0FB0F000u) == 0x0320F000u) { /* MSR con registro o inmediato */
				const unsigned fm = (insn >> 16) & 15;

				/* SPSR (cualquier campo) o CPSR sin el campo de banderas: no lee ni escribe NZCV */
				if ((insn & (1u << 22)) || !(fm & 8)) {
					a->fast = FK_MSR;
					a->call_ro = 1;
					a->fr = 0;
					a->o2 = (insn & (1u << 25)) ? O2_IMM : O2_SHI;
					a->rm = insn & 15;
					a->imm = ror32(insn & 0xFF, ((insn >> 8) & 15) * 2);
				}
			}
			break;
		}
		/* proceso de datos */
		a->kind = AI_DP;
		a->op = (hi >> 1) & 15;
		a->s = hi & 1;
		a->rn = (insn >> 16) & 15;
		a->rd = (insn >> 12) & 15;
		a->pcv = addr + 8;
		if (b27_25 == 1) {
			const unsigned rot = (insn >> 7) & 0x1E;

			a->o2 = O2_IMM;
			a->imm = ror32(insn & 0xFF, rot);
			a->ic = rot ? (int8_t)(a->imm >> 31) : -1;
		} else if (!(lo & 1)) {
			a->o2 = O2_SHI;
			a->rm = insn & 15;
			a->sht = (insn >> 5) & 3;
			a->sha = (insn >> 7) & 31;
		} else {
			a->o2 = O2_SHR;
			a->rm = insn & 15;
			a->rs = (insn >> 8) & 15;
			a->sht = (insn >> 5) & 3;
			a->pcv = addr + 12;
			a->cyc++;
			if (a->rs == 15)
				callout(a);
		}
		if (a->kind == AI_CALL)
			break;
		dp_flags(a);
		{
			const int test = a->op >= 8 && a->op <= 11;

			if (!test && a->rd == 15) {
				if (a->s) {
					callout(a);
					break;
				}
				a->cyc += 2;
				a->ends = 1;
			}
		}
		break;
	case 2:
	case 3:
		if (b27_25 == 3 && (lo & 1)) {
			callout(a);
			break;
		}
		a->kind = AI_MEM;
		a->load = hi & 1;
		a->msz = (hi & 4) ? 1 : 4;
		a->rn = (insn >> 16) & 15;
		a->rd = (insn >> 12) & 15;
		a->pre = (insn >> 24) & 1;
		a->up = (insn >> 23) & 1;
		a->wb = !a->pre || ((insn >> 21) & 1);
		a->pcv = addr + 8;
		if (b27_25 == 2) {
			a->o2 = O2_IMM;
			a->imm = insn & 0xFFF;
		} else {
			a->o2 = O2_SHI;
			a->rm = insn & 15;
			a->sht = (insn >> 5) & 3;
			a->sha = (insn >> 7) & 31;
			if (a->sht == 3 && a->sha == 0)
				a->fr |= F_C; /* RRX */
		}
		a->cyc = a->load ? 3 : 2;
		if ((a->wb && a->rn == 15) || (a->o2 == O2_SHI && a->rm == 15)) {
			callout(a);
			break;
		}
		if (a->load && a->rd == 15) {
			if (a->msz != 4) {
				callout(a);
				break;
			}
			a->cyc += 2;
			a->ends = 1;
			a->xchg = 1;
		}
		break;
	case 4: {
		const uint32_t list = insn & 0xFFFF;

		a->kind = AI_BLOCK;
		a->load = hi & 1;
		a->rn = (insn >> 16) & 15;
		a->list = (uint16_t)list;
		a->pcv = addr + 8; /* STM con pc guarda la dirección + 12 */
		a->pre = (insn >> 24) & 1;
		a->up = (insn >> 23) & 1;
		a->wb = (insn >> 21) & 1;
		if ((insn & (1u << 22)) || list == 0 || a->rn == 15 || (a->load && a->wb && ((list >> a->rn) & 1))) {
			callout(a);
			break;
		}
		{
			const unsigned n = popcount16(list);

			a->cyc = (uint8_t)(a->load ? 2 + n : 1 + n);
			if (a->load && (list & 0x8000u)) {
				a->cyc += 2;
				a->ends = 1;
				a->xchg = 1;
			}
		}
		break;
	}
	case 5: {
		const int32_t off = (int32_t)(insn << 8) >> 6;

		a->kind = AI_B;
		a->target = addr + 8 + (uint32_t)off;
		a->link = (hi & 0x10) != 0;
		a->lrv = addr + 4;
		a->cyc = 3;
		break;
	}
	case 6:
		callout(a);
		break;
	default:
		if (hi & 0x10) { /* SWI */
			callout(a);
			break;
		}
		callout(a);
		if (lo & 1) {   /* MCR / MRC */
			const uint32_t rd = (insn >> 12) & 15;

			if (!(insn & (1u << 20)) || rd != 15) { /* MRC a r15 escribe NZCV */
				a->call_ro = 1;
				a->fr = 0;
			}
			if (((insn >> 8) & 15) == 15 && ((insn >> 21) & 7) == 0) {
				const unsigned crn = (insn >> 16) & 15, crm = insn & 15, op2 = (insn >> 5) & 7;

				if (!(insn & (1u << 20))) {
					/* c7: operaciones de caché y del búfer de escritura (no-op sin caché), salvo la espera de IRQ */
					if (crn == 7 && !(crm == 0 && op2 == 4) && !(crm == 8 && op2 == 2)) {
						a->fast = FK_CP15NOP;
						a->cyc = 3;
					}
				} else if (rd != 15 && ((crn == 1 && crm == 0 && op2 == 0) || (crn == 9 && crm == 1 && op2 < 2))) {
					a->fast = FK_MRC;
					a->rd = (uint8_t)rd;
					a->rn = (uint8_t)crn;
					a->op = (uint8_t)op2;
					a->cyc = 3;
				}
			}
		}
		break;
	}
	if (a->cond != AI_COND_AL && a->cond != 0xF) {
		a->fr |= arec_cond_reads(a->cond);
		/* condicionales que fijan banderas: el intérprete (las dos ramas dejan banderas distintas) */
		if (a->kind != AI_CALL && a->fw) {
			callout(a);
			a->fr |= arec_cond_reads(a->cond);
		}
		if (a->kind != AI_B)
			a->fw = 0; /* si no se ejecuta no escribe: nada es seguro */
	}
	if (a->kind == AI_B && a->cond == AI_COND_AL)
		a->ends = 1;
}

/* ---- Thumb ---------------------------------------------------------------------------------------------- */

static void th_dp(struct ai *a, unsigned op, unsigned s, unsigned rd, unsigned rn)
{
	a->kind = AI_DP;
	a->op = (uint8_t)op;
	a->s = (uint8_t)s;
	a->rd = (uint8_t)rd;
	a->rn = (uint8_t)rn;
}

/* `next` es la media palabra siguiente (para juntar las dos mitades de BL) o 0xFFFFFFFF si no hay. */
static void dec_thumb(uint32_t addr, uint32_t insn, uint32_t next, int v5, struct ai *a)
{
	const unsigned top5 = insn >> 11;

	memset(a, 0, sizeof *a);
	a->addr = addr;
	a->raw = insn;
	a->thumb = 1;
	a->size = 2;
	a->ninsn = 1;
	a->cond = AI_COND_AL;
	a->cyc = 1;
	a->pcv = addr + 4;

	switch (top5) {
	case 0: case 1: case 2: /* LSL, LSR, ASR inmediatos: MOVS rd, rm, sh #n */
		th_dp(a, 13, 1, insn & 7, 0);
		a->o2 = O2_SHI;
		a->rm = (insn >> 3) & 7;
		a->sht = (uint8_t)top5;
		a->sha = (insn >> 6) & 31;
		break;
	case 3: /* ADD/SUB con registro o inmediato de 3 bits */
		th_dp(a, (insn & (1u << 9)) ? 2 : 4, 1, insn & 7, (insn >> 3) & 7);
		if (insn & (1u << 10)) {
			a->o2 = O2_IMM;
			a->imm = (insn >> 6) & 7;
			a->ic = -1;
		} else {
			a->o2 = O2_SHI;
			a->rm = (insn >> 6) & 7;
		}
		break;
	case 4: case 5: case 6: case 7: { /* MOV, CMP, ADD, SUB con inmediato de 8 bits */
		static const uint8_t ops[4] = { 13, 10, 4, 2 };
		const unsigned rd = (insn >> 8) & 7;

		th_dp(a, ops[top5 - 4], 1, rd, rd);
		a->o2 = O2_IMM;
		a->imm = insn & 0xFF;
		a->ic = -1;
		break;
	}
	case 8:
		if (!(insn & (1u << 10))) { /* ALU */
			const unsigned op = (insn >> 6) & 15, rd = insn & 7, rs = (insn >> 3) & 7;

			switch (op) {
			case 2: case 3: case 4: case 7: { /* LSL, LSR, ASR, ROR por registro */
				static const uint8_t ty[8] = { 0, 0, 0, 1, 2, 0, 0, 3 };

				th_dp(a, 13, 1, rd, 0);
				a->o2 = O2_SHR;
				a->rm = (uint8_t)rd;
				a->rs = (uint8_t)rs;
				a->sht = ty[op];
				a->cyc = 2;
				break;
			}
			case 9: /* NEG = RSBS rd, rs, #0 */
				th_dp(a, 3, 1, rd, rs);
				a->o2 = O2_IMM;
				a->imm = 0;
				a->ic = -1;
				break;
			case 13: /* MUL: rd = rs * rd, ciclos del valor de rd */
				a->kind = AI_MUL;
				a->rd = (uint8_t)rd;
				a->rm = (uint8_t)rs;
				a->rs = (uint8_t)rd;
				a->s = 1;
				a->fw = F_N | F_Z;
				return;
			default: {
				/* AND EOR - - - ADC SBC - TST - CMP CMN ORR - BIC MVN */
				static const uint8_t ops[16] = { 0, 1, 0, 0, 0, 5, 6, 0, 8, 0, 10, 11, 12, 0, 14, 15 };

				th_dp(a, ops[op], 1, rd, rd);
				a->o2 = O2_SHI;
				a->rm = (uint8_t)rs;
				break;
			}
			}
			break;
		}
		{ /* registros altos, BX/BLX */
			const unsigned rd = (insn & 7) | ((insn >> 4) & 8), rs = (insn >> 3) & 15;

			switch ((insn >> 8) & 3) {
			case 0: /* ADD sin banderas */
				th_dp(a, 4, 0, rd, rd);
				a->o2 = O2_SHI;
				a->rm = (uint8_t)rs;
				if (rd == 15) {
					a->cyc = 3;
					a->ends = 1;
				}
				break;
			case 1: /* CMP */
				th_dp(a, 10, 1, rd, rd);
				a->o2 = O2_SHI;
				a->rm = (uint8_t)rs;
				break;
			case 2: /* MOV sin banderas */
				th_dp(a, 13, 0, rd, 0);
				a->o2 = O2_SHI;
				a->rm = (uint8_t)rs;
				if (rd == 15) {
					a->cyc = 3;
					a->ends = 1;
				}
				break;
			default:
				if ((insn & 0x80) && !v5) {
					callout(a);
					return;
				}
				a->kind = AI_BX;
				a->rm = (uint8_t)rs;
				a->xchg = 1;
				a->link = (insn & 0x80) != 0;
				a->lrv = (addr + 2) | 1u;
				a->cyc = 3;
				a->ends = 1;
				if (rs == 15) /* BX PC: salta a ARM en (dirección + 4) & ~3 */
					callout(a), a->ends = 0;
				break;
			}
		}
		break;
	case 9: /* LDR rd, [PC, #imm] */
		a->kind = AI_MEM;
		a->load = 1;
		a->msz = 4;
		a->rd = (insn >> 8) & 7;
		a->rn = 15;
		a->pcv = (addr + 4) & ~3u;
		a->pre = 1;
		a->up = 1;
		a->o2 = O2_IMM;
		a->imm = (insn & 0xFF) * 4;
		a->cyc = 3;
		break;
	case 10: case 11: { /* carga y almacenamiento con registro */
		static const uint8_t ld[8] = { 0, 0, 0, 1, 1, 1, 1, 1 }, sz[8] = { 4, 2, 1, 1, 4, 2, 1, 2 },
				     sg[8] = { 0, 0, 0, 1, 0, 0, 0, 1 };
		const unsigned k = (insn >> 9) & 7;

		a->kind = AI_MEM;
		a->load = ld[k];
		a->msz = sz[k];
		a->msign = sg[k];
		a->rd = insn & 7;
		a->rn = (insn >> 3) & 7;
		a->o2 = O2_SHI;
		a->rm = (insn >> 6) & 7;
		a->pre = 1;
		a->up = 1;
		a->cyc = a->load ? 3 : 2;
		break;
	}
	case 12: case 13: case 14: case 15: { /* palabra o byte con desplazamiento inmediato */
		const unsigned k = (insn >> 11) & 3, imm = (insn >> 6) & 31;

		a->kind = AI_MEM;
		a->load = k & 1;
		a->msz = (k & 2) ? 1 : 4;
		a->rd = insn & 7;
		a->rn = (insn >> 3) & 7;
		a->o2 = O2_IMM;
		a->imm = (k & 2) ? imm : imm * 4;
		a->pre = 1;
		a->up = 1;
		a->cyc = a->load ? 3 : 2;
		break;
	}
	case 16: case 17: /* media palabra con desplazamiento inmediato */
		a->kind = AI_MEM;
		a->load = (insn >> 11) & 1;
		a->msz = 2;
		a->rd = insn & 7;
		a->rn = (insn >> 3) & 7;
		a->o2 = O2_IMM;
		a->imm = ((insn >> 6) & 31) * 2;
		a->pre = 1;
		a->up = 1;
		a->cyc = a->load ? 3 : 2;
		break;
	case 18: case 19: /* relativo a SP */
		a->kind = AI_MEM;
		a->load = (insn >> 11) & 1;
		a->msz = 4;
		a->rd = (insn >> 8) & 7;
		a->rn = 13;
		a->o2 = O2_IMM;
		a->imm = (insn & 0xFF) * 4;
		a->pre = 1;
		a->up = 1;
		a->cyc = a->load ? 3 : 2;
		break;
	case 20: /* ADD rd, PC, #imm: constante */
		th_dp(a, 13, 0, (insn >> 8) & 7, 0);
		a->o2 = O2_IMM;
		a->imm = ((addr + 4) & ~3u) + (insn & 0xFF) * 4;
		a->ic = -1;
		break;
	case 21: /* ADD rd, SP, #imm */
		th_dp(a, 4, 0, (insn >> 8) & 7, 13);
		a->o2 = O2_IMM;
		a->imm = (insn & 0xFF) * 4;
		a->ic = -1;
		break;
	case 22: case 23: {
		const unsigned sub = (insn >> 8) & 15;

		if (sub == 0) { /* ADD/SUB SP, #imm */
			th_dp(a, (insn & 0x80) ? 2 : 4, 0, 13, 13);
			a->o2 = O2_IMM;
			a->imm = (insn & 0x7F) * 4;
			a->ic = -1;
			break;
		}
		if (sub == 4 || sub == 5) { /* PUSH = STMDB sp!, {lista[, lr]} */
			const uint32_t list = (insn & 0xFF) | ((insn & 0x100) ? 0x4000u : 0u);

			if (!list) {
				callout(a);
				break;
			}
			a->kind = AI_BLOCK;
			a->load = 0;
			a->rn = 13;
			a->list = (uint16_t)list;
			a->pre = 1;
			a->up = 0;
			a->wb = 1;
			a->cyc = (uint8_t)(1 + popcount16(list));
			break;
		}
		if (sub == 12 || sub == 13) { /* POP = LDMIA sp!, {lista[, pc]} */
			const uint32_t list = (insn & 0xFF) | ((insn & 0x100) ? 0x8000u : 0u);

			if (!list) {
				callout(a);
				break;
			}
			a->kind = AI_BLOCK;
			a->load = 1;
			a->rn = 13;
			a->list = (uint16_t)list;
			a->pre = 0;
			a->up = 1;
			a->wb = 1;
			a->cyc = (uint8_t)(2 + popcount16(list));
			if (list & 0x8000u) {
				a->cyc += 2;
				a->ends = 1;
				a->xchg = (uint8_t)v5;
			}
			break;
		}
		callout(a); /* BKPT y lo indefinido */
		break;
	}
	case 24: case 25: { /* STMIA / LDMIA rb!, {lista} */
		const unsigned rb = (insn >> 8) & 7;
		const uint32_t list = insn & 0xFF;

		if (!list || (((insn >> 11) & 1) && ((list >> rb) & 1))) {
			callout(a);
			break;
		}
		a->kind = AI_BLOCK;
		a->load = (insn >> 11) & 1;
		a->rn = (uint8_t)rb;
		a->list = (uint16_t)list;
		a->pre = 0;
		a->up = 1;
		a->wb = 1;
		a->cyc = (uint8_t)(a->load ? 2 + popcount16(list) : 1 + popcount16(list));
		break;
	}
	case 26: case 27: {
		const unsigned cond = (insn >> 8) & 15;

		if (cond >= 0xE) { /* SWI y lo indefinido */
			callout(a);
			break;
		}
		a->kind = AI_B;
		a->cond = (uint8_t)cond;
		a->target = (addr + 4 + (uint32_t)((int32_t)(int8_t)insn * 2)) | 1u;
		a->cyc = 3;
		a->fr = arec_cond_reads(cond);
		break;
	}
	case 28: /* B */
		a->kind = AI_B;
		a->target = (addr + 4 + (uint32_t)((int32_t)(insn << 21) >> 20)) | 1u;
		a->cyc = 3;
		a->ends = 1;
		break;
	case 29: /* BLX (segunda mitad) sola */
		if (!v5) {
			callout(a);
			break;
		}
		a->kind = AI_BLLO;
		a->imm = (insn & 0x7FF) << 1;
		a->link = 1;
		a->lrv = (addr + 2) | 1u;
		a->xchg = 0; /* a ARM */
		a->cyc = 3;
		a->ends = 1;
		break;
	case 30: { /* BL: primera mitad; si la sigue la segunda, salto directo */
		const uint32_t lr = addr + 4 + (uint32_t)(((int32_t)(insn << 21) >> 21) * 4096);

		if (next != 0xFFFFFFFFu && ((next >> 11) == 31 || ((next >> 11) == 29 && v5))) {
			a->kind = AI_B;
			a->size = 4;
			a->ninsn = 2;
			a->raw = insn | next << 16;
			a->link = 1;
			a->lrv = (addr + 4) | 1u;
			if ((next >> 11) == 31)
				a->target = ((lr + ((next & 0x7FF) << 1)) & ~1u) | 1u;
			else
				a->target = (lr + ((next & 0x7FF) << 1)) & ~3u;
			a->cyc = 4;
			a->ends = 1;
			break;
		}
		th_dp(a, 13, 0, 14, 0); /* lr = constante */
		a->o2 = O2_IMM;
		a->imm = lr;
		a->ic = -1;
		break;
	}
	default: /* 31: BL (segunda mitad) sola */
		a->kind = AI_BLLO;
		a->imm = (insn & 0x7FF) << 1;
		a->link = 1;
		a->lrv = (addr + 2) | 1u;
		a->xchg = 1; /* queda en Thumb */
		a->cyc = 3;
		a->ends = 1;
		break;
	}
	if (a->kind == AI_DP)
		dp_flags(a);
}

/* ---- bloques --------------------------------------------------------------------------------------------- */

int arec_decode_one(const struct arec_src *src, uint32_t pc, struct ai *out, uint32_t *phys_out)
{
	uint32_t phys, avail;
	const int thumb = pc & 1;
	const uint32_t a = pc & ~1u;
	const uint8_t *p = src->code_ptr(src->ctx, a, &phys, &avail);

	if (!p || avail < (thumb ? 2u : 4u))
		return 0;
	*phys_out = phys;
	if (thumb) {
		const uint32_t next = avail >= 4 ? rd16le(p + 2) : 0xFFFFFFFFu;

		dec_thumb(a, rd16le(p), next, 1, out);
	} else {
		if (a & 3)
			return 0;
		dec_arm(a, rd32le(p), out);
	}
	return 1;
}

unsigned arec_decode(const struct arec_src *src, uint32_t pc, struct ai *out, unsigned max, uint32_t *phys_lo,
		     uint32_t *phys_hi)
{
	const int thumb = pc & 1;
	uint32_t a = pc & ~1u, phys0 = AREC_PHYS_NONE, avail = 0;
	const uint8_t *p = NULL;
	unsigned n = 0;
	uint32_t used = 0;

	if (!thumb && (a & 3))
		return 0;
	p = src->code_ptr(src->ctx, a, &phys0, &avail);
	if (!p)
		return 0;
	while (n < max) {
		struct ai *x = &out[n];
		const uint32_t need = thumb ? 2u : 4u;

		if (used + need > avail)
			break;
		if (thumb) {
			const uint32_t next = used + 4 <= avail ? rd16le(p + used + 2) : 0xFFFFFFFFu;

			dec_thumb(a + used, rd16le(p + used), next, 1, x);
		} else {
			dec_arm(a + used, rd32le(p + used), x);
		}
		used += x->size;
		n++;
		if (x->ends)
			break;
		/* salto condicional hacia atrás: fin del bloque (los de adelante siguen dentro) */
		if (x->kind == AI_B && x->cond != AI_COND_AL && (x->target & ~1u) <= x->addr)
			break;
	}
	*phys_lo = phys0;
	*phys_hi = phys0 == AREC_PHYS_NONE ? AREC_PHYS_NONE : phys0 + used;
	return n;
}
