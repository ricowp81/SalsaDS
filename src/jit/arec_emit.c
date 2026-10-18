/*
 * SalsaDS — recompilador: emisión de PowerPC64 (Power ISA 2.02, la del PPE) para un bloque decodificado.
 *
 * Registros del PPE mientras corre código generado (docs/fase4.md):
 *   r14–r28  r0–r14 del guest (siempre ahí: los bloques se enlazan sin tocar la memoria), extendidos con ceros
 *   r29      (ciclos − límite) << 16 | instrucciones contadas desde la entrada (16 bits)
 *   r30      tabla de páginas de la ruta rápida (lectura; la de escritura está 2 MB después)
 *   r31      struct acore
 *   r0, r3–r12  temporales; cr0, cr1, cr5–cr7 temporales; cr2–cr4 vistas de las banderas (no volátiles: sobreviven
 *            a las llamadas a C de los caminos lentos)
 *
 * Banderas NZCV: «CR por instrucción» (docs/fase0.md §0.7): cada instrucción que las fija deja comparaciones en
 * cr2–cr4 según lo que leen las instrucciones siguientes del bloque (análisis previo) y el consumidor salta por un
 * bit del CR. Donde tienen que estar en memoria (c->jit_nzcv: llamadas al intérprete y salidas con banderas vivas
 * según la vista adelante) se calculan en registros enteros en la propia instrucción que las fija, sin formas con
 * punto ni mfcr. Una salida fría (después de un almacenamiento lento) las saca del CR con mfcr y un ayudante.
 *
 * Código propio (GPL-2.0-or-later).
 */
#include <stddef.h>
#include <string.h>

#include "jit/arec_priv.h"
#include "jit/ppcenc.h"
#include "util/endian.h"

#define G(n) (14u + (n))
#define RCYC 29u
#define RPT 30u
#define RST 31u

#define OFF_R(n) ((int32_t)offsetof(struct acore, r) + 4 * (int32_t)(n))
#define OFF_CYCLES ((int32_t)offsetof(struct acore, cycles))
#define OFF_ICOUNT ((int32_t)offsetof(struct acore, icount))
#define OFF_NZCV ((int32_t)offsetof(struct acore, jit_nzcv))
#define OFF_UNTIL ((int32_t)offsetof(struct acore, jit_until))
#define OFF_PT ((int32_t)offsetof(struct acore, jit_pt))
#define OFF_JL ((int32_t)offsetof(struct acore, jit_jl))
#define OFF_FN(k) ((int32_t)offsetof(struct acore, jit_fn) + 8 * (int32_t)(k))
#define OFF_CPSR ((int32_t)offsetof(struct acore, cpsr))
#define OFF_BANK ((int32_t)offsetof(struct acore, bank))
#define OFF_SPSR ((int32_t)offsetof(struct acore, spsr_bank))
#define OFF_ATTN ((int32_t)offsetof(struct acore, attn))
#define OFF_CP15_CTRL ((int32_t)offsetof(struct acore, cp15_ctrl))
#define OFF_DTCM_REG ((int32_t)offsetof(struct acore, cp15_dtcm_reg))
#define OFF_ITCM_REG ((int32_t)offsetof(struct acore, cp15_itcm_reg))

#define CRB(f, b) ((f) * 4u + (b))
#define LT 0u
#define GT 1u
#define EQ 2u
#define SO 3u
#define BO_T 12u
#define BO_F 4u

/* ---- emisor con dos secciones (caliente y fría) y etiquetas ------------------------------------------- */

#define EM_HOT_CAP 6144
#define EM_COLD_CAP 8192
#define EM_LABELS 512
#define EM_FIX 1024

enum { SEC_HOT = 0, SEC_COLD = 1 };

struct efix {
	uint16_t at;
	uint8_t sec, kind; /* 0: b a una etiqueta, 1: bc a una etiqueta, 2: b a una palabra de la caché, 3: bl a una dirección */
	uint32_t lab;      /* etiqueta, palabra de la caché o índice del ayudante */
};

/* ubicación de una bandera (índice 0 = N, 1 = Z, 2 = C, 3 = V) */
enum { L_MEM, L_K0, L_K1, L_CR, L_XOR };
struct floc {
	uint8_t t, b, b2, inv;
};

uint32_t arec_bad_raw, arec_bad_addr; /* la última instrucción en la que falló el análisis (depuración) */
static uint32_t em_hot[EM_HOT_CAP], em_cold[EM_COLD_CAP]; /* un solo hilo traduce */

struct em {
	struct arec *j;
	uint32_t *hot, *cold;
	unsigned nh, nc, sec;
	int overflow;
	int bad;             /* el análisis de banderas falló: no usar el bloque */
	uint32_t lab_pos[EM_LABELS];
	uint8_t lab_sec[EM_LABELS], lab_set[EM_LABELS];
	unsigned nlab;
	struct efix fix[EM_FIX];
	unsigned nfix;
	uint32_t base;       /* índice de palabra en la caché donde va a quedar el bloque */
	uint32_t block_id;
	struct ablock *b;

	const struct ai *ins;
	unsigned n, i;       /* instrucción en curso */
	unsigned pend, ipend; /* ciclos e instrucciones que faltan sumar a r29 */
	unsigned xextra;      /* ciclos de más en las salidas desde el cuerpo de una instrucción condicional */
	int strict, no_link, inline_jl, far;

	/* banderas */
	struct floc fl[4];
	uint8_t in_mem;      /* banderas cuyo valor actual está en c->jit_nzcv */
	int8_t sc_hi, sc_lt, sc_gt; /* bits del CR con HI, LT (N≠V) y GT exactos de la última resta */
	uint8_t sub_mask;    /* banderas que todavía son de esa resta */

	/* análisis previo, por instrucción */
	uint8_t live_after[AREC_MAX_INSN], live_before[AREC_MAX_INSN], live_taken[AREC_MAX_INSN];
	uint8_t need_flags[AREC_MAX_INSN]; /* banderas que esta instrucción debe dejar en el CR */
	uint16_t need_cond[AREC_MAX_INSN];  /* condiciones que se leen con todas las banderas de esta instrucción */
	uint8_t need_cdata[AREC_MAX_INSN];  /* el acarreo se lee como dato */
	uint8_t mat[AREC_MAX_INSN];         /* calcular NZCV en memoria aquí */
	uint16_t rem_max[AREC_MAX_INSN];    /* ciclos máximos desde después de la instrucción hasta el final */
	uint8_t live_end;                    /* vivas al caer del final del bloque */
	uint32_t win_lo, win_hi;             /* ventana física que puede mirar la vista adelante (cubetas del bloque) */
};

static void E(struct em *e, uint32_t w)
{
	if (e->sec == SEC_HOT) {
		if (e->nh < EM_HOT_CAP)
			e->hot[e->nh++] = w;
		else
			e->overflow = 1;
	} else {
		if (e->nc < EM_COLD_CAP)
			e->cold[e->nc++] = w;
		else
			e->overflow = 1;
	}
}

static unsigned cur_pos(const struct em *e)
{
	return e->sec == SEC_HOT ? e->nh : e->nc;
}

static unsigned newlab(struct em *e)
{
	if (e->nlab >= EM_LABELS) {
		e->overflow = 1;
		return 0;
	}
	e->lab_set[e->nlab] = 0;
	return e->nlab++;
}

static void bind(struct em *e, unsigned l)
{
	e->lab_pos[l] = cur_pos(e);
	e->lab_sec[l] = (uint8_t)e->sec;
	e->lab_set[l] = 1;
}

static void addfix(struct em *e, unsigned kind, uint32_t l)
{
	if (e->nfix >= EM_FIX) {
		e->overflow = 1;
		return;
	}
	e->fix[e->nfix].at = (uint16_t)cur_pos(e);
	e->fix[e->nfix].sec = (uint8_t)e->sec;
	e->fix[e->nfix].kind = (uint8_t)kind;
	e->fix[e->nfix].lab = l;
	e->nfix++;
}

static void b_to(struct em *e, unsigned l)
{
	addfix(e, 0, l);
	E(e, ppc_b(0));
}

static void bc_to(struct em *e, unsigned bo, unsigned bi, unsigned l)
{
	addfix(e, 1, l);
	E(e, ppc_bc(bo, bi, 0));
}

/* posición final (índice de palabra dentro del bloque) */
static uint32_t final_pos(const struct em *e, unsigned sec, unsigned pos)
{
	return sec == SEC_HOT ? pos : e->nh + pos;
}

/* salto a una palabra de la caché (se resuelve al cerrar el bloque, cuando se conoce la posición final) */
static void b_abs(struct em *e, uint32_t word)
{
	addfix(e, 2, word);
	E(e, ppc_b(0));
}

/* bl a una rutina común de la caché */
static void bl_abs(struct em *e, uint32_t word)
{
	addfix(e, 4, word);
	E(e, ppc_bl(0));
}

/* ---- constantes y llamadas ---------------------------------------------------------------------------- */

/* rt = v (32 bits, extendido con ceros) */
static void li32(struct em *e, unsigned rt, uint32_t v)
{
	if (v < 0x8000u) {
		E(e, ppc_li(rt, (int32_t)v));
	} else if (v < 0x10000u) {
		E(e, ppc_li(rt, 0));
		E(e, ppc_ori(rt, rt, v));
	} else if (v < 0x80000000u) {
		E(e, ppc_lis(rt, (int32_t)(v >> 16)));
		if (v & 0xFFFF)
			E(e, ppc_ori(rt, rt, v & 0xFFFF));
	} else {
		E(e, ppc_li(rt, 0));
		E(e, ppc_oris(rt, rt, v >> 16));
		if (v & 0xFFFF)
			E(e, ppc_ori(rt, rt, v & 0xFFFF));
	}
}

static void call_fn(struct em *e, unsigned k)
{
	if (!e->far) {
		/* bl si llega (se decide al cerrar; si no llega, el cierre falla y se repite con llamadas lejanas) */
		addfix(e, 3, k);
		E(e, ppc_bl(0));
		return;
	}
	E(e, ppc_ld(0, RST, OFF_FN(k)));
	E(e, ppc_mtctr(0));
	E(e, PPC_BCTRL);
}

/* Modo estricto, después de una llamada lenta: si lo que queda del bloque ya no cabe antes del límite (el bus sumó
 * ciclos), saltar a `l`. `k` = ciclos hasta el final de la instrucción (desde el valor de r29). */
static void strict_overrun(struct em *e, unsigned k, unsigned l)
{
	E(e, ppc_sradi(10, RCYC, 16));
	E(e, ppc_addi(10, 10, (int32_t)(k + e->rem_max[e->i])));
	E(e, ppc_cmpdi(0, 10, 0));
	bc_to(e, BO_T, CRB(0, GT), l);
}

/* suma a r29 (sin tocar lo pendiente) */
static void add_cyc(struct em *e, unsigned c, unsigned ic)
{
	if (c)
		E(e, ppc_addis(RCYC, RCYC, (int32_t)c));
	if (ic)
		E(e, ppc_addi(RCYC, RCYC, (int32_t)ic));
}

/* ---- salidas -------------------------------------------------------------------------------------------- */

/* r4 = tipo | vivas << 4; r3 = valor. */
static void exit_pc_const(struct em *e, uint32_t pc, unsigned live)
{
	li32(e, 3, pc);
	E(e, ppc_li(4, (int32_t)(0 | live << 4)));
	b_abs(e, e->j->ent_exit);
}

/* Salida directa enlazable: suma lo pendiente (más `xc` ciclos e `xi` instrucciones) y salta al stub. */
static void exit_link(struct em *e, uint32_t target, unsigned xc, unsigned xi, unsigned live)
{
	struct ablock *b = e->b;
	unsigned k, stub;
	uint32_t site_pos;
	unsigned site_sec;

	add_cyc(e, e->pend + xc + e->xextra, e->ipend + xi);
	if (b->nexits >= AREC_MAX_EXITS) {
		exit_pc_const(e, target, live);
		return;
	}
	k = b->nexits++;
	stub = newlab(e);
	site_sec = e->sec;
	site_pos = cur_pos(e);
	b_to(e, stub);
	{
		const unsigned save = e->sec;

		e->sec = SEC_COLD;
		bind(e, stub);
		{
			const uint32_t code = (e->block_id << 8) | k;

			li32(e, 3, code);
		}
		E(e, ppc_li(4, (int32_t)(1 | live << 4)));
		b_abs(e, e->j->ent_exit);
		e->sec = save;
	}
	b->exits[k].target = target;
	/* posiciones finales: se completan en el cierre (la sección caliente se conoce al final) */
	b->exits[k].site = (uint32_t)(site_sec << 31 | site_pos);
	b->exits[k].stub = (uint32_t)stub;
}

/* Salida por un destino calculado en r3 (ya con el bit 0 de Thumb y alineado): búsqueda en línea o despachador. */
static void exit_indirect(struct em *e, unsigned xc, unsigned xi, unsigned live)
{
	add_cyc(e, e->pend + xc + e->xextra, e->ipend + xi);
	if (e->inline_jl) {
		const unsigned miss = newlab(e);

		E(e, ppc_ld(4, RST, OFF_JL));
		E(e, ppc_rlwinm(5, 3, 3, 14, 27)); /* ((pc >> 1) & (AREC_JL_SIZE - 1)) * 16 */
		E(e, ppc_add(4, 4, 5));
		E(e, ppc_lwz(5, 4, 0));
		E(e, ppc_cmplw(0, 5, 3));
		bc_to(e, BO_F, CRB(0, EQ), miss);
		E(e, ppc_ld(5, 4, 8));
		E(e, ppc_mtctr(5));
		E(e, PPC_BCTR);
		bind(e, miss);
	}
	E(e, ppc_li(4, (int32_t)(0 | live << 4)));
	b_abs(e, e->j->ent_exit);
}

/* ---- banderas: ubicación, materialización -------------------------------------------------------------- */

static unsigned fmask(unsigned i)
{
	return 8u >> i;
}

static void fl_all_mem(struct em *e)
{
	for (int i = 0; i < 4; i++)
		e->fl[i].t = L_MEM;
	e->in_mem = F_ALL;
	e->sc_hi = e->sc_lt = e->sc_gt = -1;
	e->sub_mask = 0;
}

/* descriptor para AH_FLAGS: 16 bits por bandera (tipo, bit, bit2, inv); las que no están en `want` quedan como en memoria */
static uint32_t desc_of(const struct floc *f, int want)
{
	if (!want)
		return L_MEM;
	return (uint32_t)f->t | (uint32_t)f->b << 3 | (uint32_t)f->b2 << 8 | (uint32_t)f->inv << 13;
}

/* Deja en c->jit_nzcv las banderas `need` que no estén ya (mfcr + ayudante: para caminos fríos o raros). */
static void flags_to_mem_cr(struct em *e, unsigned need)
{
	unsigned want = need & ~e->in_mem & F_ALL;
	uint32_t lo, hi;

	if (!want)
		return;
	lo = desc_of(&e->fl[0], (want & F_N) != 0) | desc_of(&e->fl[1], (want & F_Z) != 0) << 16;
	hi = desc_of(&e->fl[2], (want & F_C) != 0) | desc_of(&e->fl[3], (want & F_V) != 0) << 16;
	E(e, ppc_mr(3, RST));
	E(e, ppc_mfcr(4));
	li32(e, 5, lo);
	li32(e, 6, hi);
	call_fn(e, AH_FLAGS);
}

/* Bit del CR libre (en cr5–cr7 no: tienen que sobrevivir; se usan cr2–cr4) que ninguna bandera viva use. */
static int free_bit(const struct em *e, unsigned live, unsigned avoid_fields)
{
	for (unsigned b = 8; b < 20; b++) {
		int used = 0;

		if (avoid_fields & (1u << (b / 4)))
			continue;
		for (int i = 0; i < 4; i++) {
			if (!(live & fmask((unsigned)i)))
				continue;
			if ((e->fl[i].t == L_CR || e->fl[i].t == L_XOR) && e->fl[i].b == b)
				used = 1;
			if (e->fl[i].t == L_XOR && e->fl[i].b2 == b)
				used = 1;
		}
		if (e->sc_hi == (int)b || e->sc_lt == (int)b || e->sc_gt == (int)b)
			used = 1;
		if (!used)
			return (int)b;
	}
	return -1;
}

static int loc_uses_field(const struct floc *f, unsigned field)
{
	if (f->t == L_CR)
		return f->b / 4 == field;
	if (f->t == L_XOR)
		return f->b / 4 == field || f->b2 / 4 == field;
	return 0;
}

/* Antes de que una instrucción escriba los campos `fields` (máscara de campos del CR): las banderas que sobreviven
 * (no las escribe, `keep`) y están vivas se mudan a otro bit, o vuelven a memoria si ya están ahí. */
static void protect_fields(struct em *e, unsigned fields, unsigned keep)
{
	const unsigned live = e->live_after[e->i] | e->live_before[e->i];

	for (int i = 0; i < 4; i++) {
		struct floc *f = &e->fl[i];
		int hit = 0;

		if (!(keep & fmask((unsigned)i)))
			continue;
		for (unsigned fl = 0; fl < 8; fl++)
			if ((fields & (1u << fl)) && loc_uses_field(f, fl))
				hit = 1;
		if (!hit)
			continue;
		if ((e->in_mem & fmask((unsigned)i)) || !(live & fmask((unsigned)i))) {
			f->t = L_MEM;
			continue;
		}
		{
			const int nb = free_bit(e, live, fields);

			if (nb < 0) { /* no hay bit: a memoria por el camino lento (raro) */
				flags_to_mem_cr(e, fmask((unsigned)i));
				e->in_mem |= (uint8_t)fmask((unsigned)i);
				f->t = L_MEM;
				continue;
			}
			if (f->t == L_CR) {
				E(e, ppc_cror((unsigned)nb, f->b, f->b));
				f->b = (uint8_t)nb;
			} else {
				E(e, ppc_crxor((unsigned)nb, f->b, f->b2));
				f->t = L_CR;
				f->b = (uint8_t)nb;
			}
		}
	}
	/* los atajos de la resta que usaban esos campos */
	if (e->sc_hi >= 0 && (fields & (1u << (e->sc_hi / 4))))
		e->sc_hi = -1;
	if (e->sc_lt >= 0 && (fields & (1u << (e->sc_lt / 4))))
		e->sc_lt = -1;
	if (e->sc_gt >= 0 && (fields & (1u << (e->sc_gt / 4))))
		e->sc_gt = -1;
}

/* Las banderas en memoria que se necesitan como bits del CR: se cargan juntas en un campo libre. */
static void load_mem_flags(struct em *e, unsigned need)
{
	unsigned want = 0, field, live = e->live_after[e->i] | e->live_before[e->i] | need;

	for (int i = 0; i < 4; i++)
		if ((need & fmask((unsigned)i)) && e->fl[i].t == L_MEM)
			want |= fmask((unsigned)i);
	if (want & ~e->in_mem)
		e->bad = 1; /* una bandera que nadie dejó ni en el CR ni en memoria: error del análisis; el bloque no se usa */
	if (!want)
		return;
	/* campo de cr2–cr4 que no use ninguna bandera viva */
	for (field = 2; field <= 4; field++) {
		int used = 0;

		for (int i = 0; i < 4; i++)
			if ((live & fmask((unsigned)i)) && e->fl[i].t != L_MEM && loc_uses_field(&e->fl[i], field))
				used = 1;
		if ((e->sc_hi >= 0 && e->sc_hi / 4 == (int)field) || (e->sc_lt >= 0 && e->sc_lt / 4 == (int)field) ||
		    (e->sc_gt >= 0 && e->sc_gt / 4 == (int)field))
			used = 1;
		if (!used)
			break;
	}
	if (field > 4) { /* todos ocupados: se usa cr4 y lo que hubiera ahí vuelve a memoria por el camino lento */
		field = 4;
		protect_fields(e, 1u << 4, F_ALL);
	}
	E(e, ppc_lwz(3, RST, OFF_NZCV));
	if (field)
		E(e, ppc_rlwinm(3, 3, 32 - 4 * field, 0, 31));
	E(e, ppc_mtocrf(field, 3));
	for (int i = 0; i < 4; i++) {
		struct floc *f = &e->fl[i];

		if (f->t != L_MEM)
			continue;
		f->t = L_CR;
		f->b = (uint8_t)CRB(field, (unsigned)i); /* N→LT, Z→GT, C→EQ, V→SO */
		f->inv = 0;
	}
}

/* Expresiones de un bit sobre el CR para condiciones compuestas: bit, inv (valor = CR[bit] ^ inv), o constante. */
struct cbit {
	int k;          /* -1 = bit del CR, 0/1 = constante */
	uint8_t b, inv;
};

static unsigned tmpb_next;

static unsigned tmp_bit(void)
{
	/* cr1 y cr0 (volátiles; la condición se consume enseguida) */
	static const uint8_t bits[6] = { 4, 5, 6, 7, 1, 3 };
	const unsigned b = bits[tmpb_next % 6];

	tmpb_next++;
	return b;
}

static struct cbit cb_of(struct em *e, unsigned i)
{
	struct cbit r;
	const struct floc *f = &e->fl[i];

	r.k = -1;
	r.inv = f->inv;
	r.b = f->b;
	if (f->t == L_K0 || f->t == L_K1) {
		r.k = f->t == L_K1;
		return r;
	}
	if (f->t == L_XOR) {
		const unsigned t = tmp_bit();

		E(e, ppc_crxor(t, f->b, f->b2));
		r.b = (uint8_t)t;
	}
	return r;
}

static struct cbit cb_not(struct cbit a)
{
	if (a.k >= 0)
		a.k = !a.k;
	else
		a.inv ^= 1;
	return a;
}

static struct cbit cb_and(struct em *e, struct cbit a, struct cbit b)
{
	struct cbit r = { -1, 0, 0 };

	if (a.k == 0 || b.k == 0) {
		r.k = 0;
		return r;
	}
	if (a.k == 1)
		return b;
	if (b.k == 1)
		return a;
	r.b = (uint8_t)tmp_bit();
	if (!a.inv && !b.inv)
		E(e, ppc_crand(r.b, a.b, b.b));
	else if (a.inv && !b.inv)
		E(e, ppc_crandc(r.b, b.b, a.b));
	else if (!a.inv && b.inv)
		E(e, ppc_crandc(r.b, a.b, b.b));
	else
		E(e, ppc_crnor(r.b, a.b, b.b));
	return r;
}

static struct cbit cb_xor(struct em *e, struct cbit a, struct cbit b)
{
	struct cbit r = { -1, 0, 0 };

	if (a.k >= 0 && b.k >= 0) {
		r.k = a.k ^ b.k;
		return r;
	}
	if (a.k >= 0)
		return a.k ? cb_not(b) : b;
	if (b.k >= 0)
		return b.k ? cb_not(a) : a;
	r.b = (uint8_t)tmp_bit();
	E(e, ppc_crxor(r.b, a.b, b.b));
	r.inv = a.inv ^ b.inv;
	return r;
}

/* Condición ARM → bit. Devuelve la expresión (constante si se sabe al traducir). */
static struct cbit cond_bit(struct em *e, unsigned cond)
{
	const unsigned base = cond & ~1u;
	struct cbit r;
	const int sh_hi = e->sc_hi >= 0 && (e->sub_mask & (F_C | F_Z)) == (F_C | F_Z);
	const int sh_lt = e->sc_lt >= 0 && (e->sub_mask & (F_N | F_V)) == (F_N | F_V);
	const int sh_gt = e->sc_gt >= 0 && (e->sub_mask & (F_N | F_Z | F_V)) == (F_N | F_Z | F_V);

	/* las que salen de un atajo de la resta no hace falta cargarlas */
	if (!((base == 8 && sh_hi) || (base == 10 && sh_lt) || (base == 12 && sh_gt)))
		load_mem_flags(e, arec_cond_reads(cond));
	switch (base) {
	case 0: /* EQ */
		r = cb_of(e, 1);
		break;
	case 2: /* CS */
		r = cb_of(e, 2);
		break;
	case 4: /* MI */
		r = cb_of(e, 0);
		break;
	case 6: /* VS */
		r = cb_of(e, 3);
		break;
	case 8: /* HI = C & !Z */
		if (sh_hi) {
			r.k = -1;
			r.b = (uint8_t)e->sc_hi;
			r.inv = 0;
		} else {
			r = cb_and(e, cb_of(e, 2), cb_not(cb_of(e, 1)));
		}
		break;
	case 10: /* GE = !(N ^ V) */
		if (sh_lt) {
			r.k = -1;
			r.b = (uint8_t)e->sc_lt;
			r.inv = 1;
		} else {
			r = cb_not(cb_xor(e, cb_of(e, 0), cb_of(e, 3)));
		}
		break;
	case 12: /* GT = !Z & !(N ^ V) */
		if (sh_gt) {
			r.k = -1;
			r.b = (uint8_t)e->sc_gt;
			r.inv = 0;
		} else {
			r = cb_and(e, cb_not(cb_of(e, 1)), cb_not(cb_xor(e, cb_of(e, 0), cb_of(e, 3))));
		}
		break;
	default:
		r.k = 1;
		r.b = 0;
		r.inv = 0;
		return r;
	}
	if (cond & 1)
		r = cb_not(r);
	return r;
}

/* Salta a `l` si la condición NO se cumple (para saltarse el cuerpo de una instrucción condicional). Devuelve 0 si la
 * condición es siempre falsa (no hace falta emitir el cuerpo), 1 si hay que emitirlo. */
static int skip_unless(struct em *e, unsigned cond, unsigned l)
{
	struct cbit c;

	if (cond == AI_COND_AL)
		return 1;
	c = cond_bit(e, cond);
	if (c.k == 1)
		return 1;
	if (c.k == 0) {
		b_to(e, l);
		return 0;
	}
	/* condición = CR[b] ^ inv; saltar si es falsa: si inv = 0, saltar con el bit en 0 */
	bc_to(e, c.inv ? BO_T : BO_F, c.b, l);
	return 1;
}

/* Salta a `l` si la condición se cumple. Devuelve -1 nunca, 1 siempre (salto incondicional emitido), 0 condicional. */
static int jump_if(struct em *e, unsigned cond, unsigned l)
{
	struct cbit c = cond_bit(e, cond);

	if (c.k == 0)
		return -1;
	if (c.k == 1) {
		b_to(e, l);
		return 1;
	}
	bc_to(e, c.inv ? BO_F : BO_T, c.b, l);
	return 0;
}

/* El acarreo como dato (0/1) en rt. */
static void carry_gpr(struct em *e, unsigned rt)
{
	struct floc *f = &e->fl[2];

	if (f->t == L_MEM) {
		if (!(e->in_mem & F_C))
			e->bad = 1;
		E(e, ppc_lwz(rt, RST, OFF_NZCV));
		E(e, ppc_rlwinm(rt, rt, 3, 31, 31));
		return;
	}
	if (f->t == L_K0 || f->t == L_K1) {
		E(e, ppc_li(rt, f->t == L_K1));
		return;
	}
	{
		struct cbit c = cb_of(e, 2);

		/* rt = inv si el bit está en 0; si no, !inv */
		E(e, ppc_li(rt, c.inv));
		E(e, ppc_bc(BO_F, c.b, 8));
		E(e, ppc_li(rt, !c.inv));
	}
}

/* ---- materialización en registros enteros (en la instrucción que fija las banderas) ---------------------- */

/* N y Z de `res` (32 bits) en rt (bits 31 y 30), el resto en cero. Usa r0. */
static void nz_gpr(struct em *e, unsigned rt, unsigned res)
{
	E(e, ppc_cntlzw(0, res));
	E(e, ppc_rlwinm(rt, res, 0, 0, 0));
	E(e, ppc_rlwinm(0, 0, 25, 1, 1));
	E(e, ppc_or(rt, rt, 0));
}

/* Escribe NZCV: rt tiene N y Z (bits 31, 30); `cbit0` es un registro con C en el bit 0 (o -1: viene de memoria),
 * `vsign` un registro con V en el bit 31 (o -1: de memoria). */
static void store_flags(struct em *e, unsigned rt, int cbit0, int vsign)
{
	if (cbit0 < 0 || vsign < 0) {
		E(e, ppc_lwz(0, RST, OFF_NZCV));
		E(e, ppc_rlwimi(rt, 0, 0, 2, 3)); /* C y V de antes */
	}
	if (cbit0 >= 0)
		E(e, ppc_rlwimi(rt, (unsigned)cbit0, 29, 2, 2));
	if (vsign >= 0)
		E(e, ppc_rlwimi(rt, (unsigned)vsign, 29, 3, 3));
	E(e, ppc_stw(rt, RST, OFF_NZCV));
}

/* ---- análisis previo de banderas -------------------------------------------------------------------------- */

static uint8_t lookahead(struct em *e, uint32_t pc);

/* Banderas que una salida a mitad de bloque (después de un acceso lento o de CP15) deja exactas: las vivas; en modo
 * estricto todas (la interrupción que sigue guarda el CPSR entero en el SPSR y --lockstep lo compara). */
static unsigned cold_live(const struct em *e, unsigned i)
{
	return e->strict ? F_ALL : e->live_after[i];
}

static int is_indirect_end(const struct ai *x)
{
	return x->ends && x->kind != AI_B;
}

static uint32_t next_pc(const struct ai *x)
{
	return (x->addr + x->size) | x->thumb;
}

static void analyze(struct em *e)
{
	const unsigned n = e->n;
	const struct ai *last = &e->ins[n - 1];
	uint8_t live;
	int8_t prod[4] = { -1, -1, -1, -1 };

	memset(e->need_flags, 0, sizeof e->need_flags);
	memset(e->need_cond, 0, sizeof e->need_cond);
	memset(e->need_cdata, 0, sizeof e->need_cdata);
	memset(e->mat, 0, sizeof e->mat);

	/* hacia atrás: vivas */
	live = 0;
	if (!last->ends)
		live = lookahead(e, next_pc(last));
	e->live_end = live;
	for (int i = (int)n - 1; i >= 0; i--) {
		const struct ai *x = &e->ins[i];
		uint8_t out = live;

		e->live_after[i] = live;
		e->live_taken[i] = 0;
		if (x->kind == AI_B) {
			const uint8_t t = lookahead(e, x->target);

			e->live_taken[i] = t;
			out = x->cond == AI_COND_AL ? t : (uint8_t)(live | t);
			if (x->cond == AI_COND_AL)
				e->live_after[i] = t;
		} else if (is_indirect_end(x)) {
			out = F_ALL;
			e->live_after[i] = F_ALL;
		}
		if (x->cond == AI_COND_AL)
			live = (uint8_t)((out & ~x->fw) | x->fr);
		else
			live = (uint8_t)(out | x->fr);
		e->live_before[i] = live;
	}

	/* hacia adelante: quién produce cada bandera y qué necesitan de él */
	{
		unsigned rem = 0;

		for (int i = (int)n - 1; i >= 0; i--) {
			e->rem_max[i] = (uint16_t)rem;
			rem += e->ins[i].cyc + ((e->ins[i].kind == AI_MUL || e->ins[i].kind == AI_MULL) ? 4u : 0u);
		}
	}
	for (unsigned i = 0; i < n; i++) {
		const struct ai *x = &e->ins[i];
		const uint8_t creads = (x->cond != AI_COND_AL && x->cond != 0xF) ? arec_cond_reads(x->cond) : 0;

		if (creads) {
			int p = -2, same = 1;

			for (int f = 0; f < 4; f++)
				if (creads & fmask((unsigned)f)) {
					if (p == -2)
						p = prod[f];
					else if (prod[f] != p)
						same = 0;
				}
			if (same && p >= 0)
				e->need_cond[p] |= (uint16_t)(1u << x->cond);
			else
				for (int f = 0; f < 4; f++)
					if ((creads & fmask((unsigned)f)) && prod[f] >= 0)
						e->need_flags[prod[f]] |= (uint8_t)fmask((unsigned)f);
		}
		if (x->kind != AI_CALL && (x->fr & F_C & ~creads) && prod[2] >= 0)
			e->need_cdata[prod[2]] = 1;
		/* puntos con las banderas en memoria (calientes): llamada al intérprete (antes) */
		if (x->kind == AI_CALL && !x->call_ro)
			for (int f = 0; f < 4; f++)
				if ((e->live_before[i] & fmask((unsigned)f)) && prod[f] >= 0)
					e->mat[prod[f]] = 1;
		/* puntos fríos (salida a mitad de bloque después de un acceso lento o de CP15): en el CR basta */
		if (x->kind == AI_MEM || x->kind == AI_BLOCK || (x->kind == AI_CALL && x->call_ro))
			for (int f = 0; f < 4; f++)
				if ((cold_live(e, i) & fmask((unsigned)f)) && prod[f] >= 0)
					e->need_flags[prod[f]] |= (uint8_t)fmask((unsigned)f);
		/* salidas: las tomadas de los saltos y las del final */
		{
			uint8_t xl = 0;

			if (x->kind == AI_B)
				xl = e->live_taken[i];
			if (is_indirect_end(x))
				xl = F_ALL;
			for (int f = 0; f < 4; f++)
				if ((xl & fmask((unsigned)f)) && prod[f] >= 0)
					e->mat[prod[f]] = 1;
		}
		if (x->kind == AI_CALL && !x->call_ro) {
			for (int f = 0; f < 4; f++)
				prod[f] = -1; /* después de la llamada, todo en memoria */
		} else if (x->cond == AI_COND_AL || x->kind == AI_B) {
			for (int f = 0; f < 4; f++)
				if (x->fw & fmask((unsigned)f))
					prod[f] = (int8_t)i;
		}
	}
	/* la caída del final (después de la última instrucción, con lo que ella escribió) */
	{
		uint8_t xl = 0;

		if (!last->ends)
			xl = e->live_end;
		if (last->ends && last->cond != AI_COND_AL)
			xl |= e->live_after[n - 1];
		for (int f = 0; f < 4; f++)
			if ((xl & fmask((unsigned)f)) && prod[f] >= 0)
				e->mat[prod[f]] = 1;
	}
}

/* Banderas vivas en `pc`: decodifica hacia adelante en línea recta (siguiendo B y BL) hasta que las cuatro se escriban. */
static uint8_t lookahead(struct em *e, uint32_t pc)
{
	struct arec *j = e->j;
	const struct arec_src src = { j->cfg.code_ptr, j->cfg.ctx };
	uint8_t live = 0, written = 0;
	struct ablock *b = e->b;

	if (e->strict)
		return F_ALL; /* lockstep y modo estricto: banderas exactas en cada salida */
	for (unsigned k = 0; k < 24; k++) {
		struct ai x;
		uint32_t ph;

		if (!arec_decode_one(&src, pc, &x, &ph))
			return F_ALL;
		/* solo código dentro de las cubetas del bloque: así una escritura ahí invalida también este bloque (el rango
		 * del bloque se amplía a lo mirado) */
		if (b->phys_lo == AREC_PHYS_NONE) {
			if (ph != AREC_PHYS_NONE)
				return (uint8_t)(live | (F_ALL & ~written));
		} else {
			if (ph == AREC_PHYS_NONE || ph < e->win_lo || ph + x.size > e->win_hi)
				return (uint8_t)(live | (F_ALL & ~written));
			if (ph < b->phys_lo)
				b->phys_lo = ph;
			if (ph + x.size > b->phys_hi)
				b->phys_hi = ph + x.size;
		}
		live |= (uint8_t)(x.fr & ~written);
		if (x.kind == AI_CALL && !x.call_ro)
			return (uint8_t)(live | (F_ALL & ~written));
		if (x.cond == AI_COND_AL)
			written |= x.fw;
		if (written == F_ALL)
			return live;
		if (x.kind == AI_B) {
			if (x.cond != AI_COND_AL)
				return (uint8_t)(live | (F_ALL & ~written));
			pc = x.target;
			continue;
		}
		if (x.ends)
			return (uint8_t)(live | (F_ALL & ~written));
		pc = next_pc(&x);
	}
	return (uint8_t)(live | (F_ALL & ~written));
}

/* ---- operandos ------------------------------------------------------------------------------------------- */

/* registro del PPE con el valor del registro guest `r` (r15: la constante en `tmp`) */
static unsigned rget(struct em *e, const struct ai *x, unsigned r, unsigned tmp)
{
	if (r == 15) {
		li32(e, tmp, x->pcv);
		return tmp;
	}
	return G(r);
}

/* desplazamiento por inmediato del valor en `rm` a `rt` (sin acarreo). Devuelve el registro con el resultado
 * (puede ser rm si no hay desplazamiento). Para RRX lee el acarreo. */
static unsigned shift_imm(struct em *e, unsigned rt, unsigned rm, unsigned type, unsigned amt)
{
	switch (type) {
	case 0:
		if (amt == 0)
			return rm;
		E(e, ppc_rlwinm(rt, rm, amt, 0, 31 - amt));
		return rt;
	case 1:
		if (amt == 0) { /* LSR #32 */
			E(e, ppc_li(rt, 0));
			return rt;
		}
		E(e, ppc_rlwinm(rt, rm, 32 - amt, amt, 31));
		return rt;
	case 2:
		E(e, ppc_srawi(rt, rm, amt == 0 ? 31 : amt));
		E(e, ppc_clrldi(rt, rt, 32));
		return rt;
	default:
		if (amt == 0) { /* RRX */
			carry_gpr(e, 0);
			E(e, ppc_rlwinm(rt, rm, 31, 1, 31));
			E(e, ppc_rlwimi(rt, 0, 31, 0, 0));
			return rt;
		}
		E(e, ppc_rlwinm(rt, rm, 32 - amt, 0, 31));
		return rt;
	}
}

/* acarreo de salida del desplazamiento por inmediato (en el bit 0 de rt) */
static void shift_imm_carry(struct em *e, unsigned rt, unsigned rm, unsigned type, unsigned amt)
{
	unsigned bitn;

	switch (type) {
	case 0:
		bitn = 32 - amt;
		break;
	case 1:
	case 2:
		bitn = amt == 0 ? 31 : amt - 1;
		break;
	default:
		bitn = amt == 0 ? 0 : amt - 1;
		break;
	}
	E(e, ppc_rlwinm(rt, rm, (32 - bitn) & 31, 31, 31));
}

/* ---- instrucciones ------------------------------------------------------------------------------------- */

static void emit_cold_exit_mem(struct em *e, unsigned live, uint32_t next, unsigned cyc_after, unsigned ic_after)
{
	/* (en la sección fría) banderas vivas a memoria, ciclos e instrucciones, y salida con el pc siguiente */
	flags_to_mem_cr(e, live);
	add_cyc(e, cyc_after, ic_after);
	exit_pc_const(e, next, live);
}

static void page_lookup(struct em *e, unsigned ra, int wr, unsigned cold);

/* Salta a `l` si la CPU está en modo usuario (CP15 y MSR CPSR_c se comportan distinto: lo hace el intérprete). */
static void if_user(struct em *e, unsigned l)
{
	E(e, ppc_lwz(3, RST, OFF_CPSR));
	E(e, ppc_rlwinm(3, 3, 0, 27, 31));
	E(e, ppc_cmplwi(1, 3, 0x10));
	bc_to(e, BO_T, CRB(1, EQ), l);
}

/* AI_CALL con camino rápido (x->fast): el caso común en línea; lo demás, el intérprete en el camino frío (con los
 * mismos ciclos: x->cyc es el total que suma el intérprete; la llamada los deja en r29 y se descuentan los que ya están
 * en lo pendiente). */
static void emit_call_fast(struct em *e, const struct ai *x, unsigned P)
{
	const unsigned live = cold_live(e, e->i);
	const unsigned slow = newlab(e), join = newlab(e);
	const uint32_t at = x->addr | x->thumb, next = (x->addr + x->size) | x->thumb;
	unsigned xirq = 0;

	if (!x->call_ro)
		flags_to_mem_cr(e, F_ALL);
	switch (x->fast) {
	case FK_CP15NOP:
		if_user(e, slow);
		break;
	case FK_MRC:
		if_user(e, slow);
		if (x->rn == 1) {
			E(e, ppc_lwz(G(x->rd), RST, OFF_CP15_CTRL));
		} else if (x->op == 0) {
			E(e, ppc_lwz(3, RST, OFF_DTCM_REG));
			li32(e, 4, 0xFFFFF03Eu);
			E(e, ppc_and(G(x->rd), 3, 4));
		} else {
			E(e, ppc_lwz(3, RST, OFF_ITCM_REG));
			E(e, ppc_rlwinm(G(x->rd), 3, 0, 26, 30)); /* & 0x3E */
		}
		break;
	case FK_MRS:
		if (x->raw & (1u << 22)) { /* SPSR del banco; en usuario/sistema, el CPSR */
			const unsigned cp = newlab(e);

			E(e, ppc_lwz(3, RST, OFF_BANK));
			E(e, ppc_cmplwi(1, 3, 0));
			bc_to(e, BO_T, CRB(1, EQ), cp);
			E(e, ppc_rlwinm(3, 3, 2, 0, 29));
			E(e, ppc_addi(3, 3, OFF_SPSR));
			E(e, ppc_lwzx(G(x->rd), RST, 3));
			b_to(e, join);
			bind(e, cp);
		}
		/* CPSR: lo de memoria sin NZCV ni T (en ARM T = 0; en memoria puede estar viejo), más NZCV */
		E(e, ppc_lwz(3, RST, OFF_CPSR));
		E(e, ppc_lwz(4, RST, OFF_NZCV));
		/* & 0x0FFFFFDF sin máscara que dé la vuelta (en 64 bits, rlwinm con mb > me copia la palabra baja arriba) */
		E(e, ppc_rlwinm(5, 3, 0, 4, 25));
		E(e, ppc_rlwimi(5, 3, 0, 27, 31));
		E(e, ppc_rlwinm(4, 4, 0, 0, 3));
		E(e, ppc_or(G(x->rd), 5, 4));
		break;
	case FK_MSR: {
		const unsigned fm = (x->raw >> 16) & 15;
		unsigned v;

		if (x->o2 == O2_IMM) {
			li32(e, 5, x->imm);
			v = 5;
		} else {
			v = G(x->rm);
		}
		if (x->raw & (1u << 22)) { /* SPSR: solo si el modo tiene banco */
			uint32_t mask = 0;

			if (fm & 8)
				mask |= 0xFF000000u;
			if (fm & 4)
				mask |= 0x00FF0000u;
			if (fm & 2)
				mask |= 0x0000FF00u;
			if (fm & 1)
				mask |= 0x000000FFu;
			mask &= 0xF80000FFu;
			E(e, ppc_lwz(3, RST, OFF_BANK));
			E(e, ppc_cmplwi(1, 3, 0));
			bc_to(e, BO_T, CRB(1, EQ), join);
			E(e, ppc_rlwinm(3, 3, 2, 0, 29));
			E(e, ppc_addi(3, 3, OFF_SPSR));
			E(e, ppc_lwzx(4, RST, 3));
			li32(e, 6, mask);
			E(e, ppc_andc(4, 4, 6));
			E(e, ppc_and(7, v, 6));
			E(e, ppc_or(4, 4, 7));
			E(e, ppc_stwx(4, RST, 3));
			break;
		}
		if (!(fm & 1))
			break; /* sin el campo de control no cambia nada (s y x no tienen bits en el ARMv5) */
		/* CPSR_c: I, F y el modo; si el modo cambia (bancos) o es usuario, el intérprete */
		E(e, ppc_lwz(3, RST, OFF_CPSR));
		E(e, ppc_rlwinm(4, 3, 0, 27, 31));
		E(e, ppc_cmplwi(1, 4, 0x10));
		bc_to(e, BO_T, CRB(1, EQ), slow);
		E(e, ppc_mr(4, 3));
		E(e, ppc_rlwimi(4, v, 0, 24, 25));
		E(e, ppc_rlwimi(4, v, 0, 27, 31));
		E(e, ppc_xor(6, 3, 4));
		E(e, ppc_rlwinm(6, 6, 0, 27, 31));
		E(e, ppc_cmplwi(1, 6, 0));
		bc_to(e, BO_F, CRB(1, EQ), slow);
		E(e, ppc_stw(4, RST, OFF_CPSR));
		/* si se habilitó I o F con una línea pendiente, salir después de la instrucción (el despachador la atiende) */
		xirq = newlab(e);
		E(e, ppc_andc(6, 3, 4));
		E(e, ppc_rlwinm(6, 6, 0, 24, 25));
		E(e, ppc_cmplwi(1, 6, 0));
		bc_to(e, BO_T, CRB(1, EQ), join);
		E(e, ppc_lwz(6, RST, OFF_ATTN));
		E(e, ppc_cmplwi(1, 6, 0));
		bc_to(e, BO_F, CRB(1, EQ), xirq);
		break;
	}
	case FK_SWP: {
		const unsigned ra = G(x->rn);

		if (x->msz == 4) {
			E(e, ppc_rlwinm(8, ra, 0, 30, 31));
			E(e, ppc_cmplwi(6, 8, 0));
			bc_to(e, BO_F, CRB(6, EQ), slow);
		}
		page_lookup(e, ra, 0, slow);
		E(e, ppc_rlwinm(6, ra, 21, 11, 28));
		E(e, ppc_addis(6, 6, 0x20));
		E(e, ppc_ldx(9, RPT, 6));
		E(e, ppc_cmpdi(1, 9, 0));
		bc_to(e, BO_T, CRB(1, EQ), slow);
		E(e, ppc_rlwinm(8, ra, 0, 18, x->msz == 4 ? 29 : 31));
		if (x->msz == 4) {
			E(e, ppc_lwbrx(3, 7, 8));
			E(e, ppc_stwbrx(G(x->rm), 9, 8));
		} else {
			E(e, ppc_lbzx(3, 7, 8));
			E(e, ppc_stbx(G(x->rm), 9, 8));
		}
		E(e, ppc_mr(G(x->rd), 3));
		break;
	}
	case FK_LDRD: {
		const unsigned base = G(x->rn);

		if (x->o2 == O2_IMM)
			E(e, ppc_addi(11, base, x->up ? (int32_t)x->imm : -(int32_t)x->imm));
		else
			E(e, x->up ? ppc_add(11, base, G(x->rm)) : ppc_subf(11, G(x->rm), base));
		E(e, ppc_rlwinm(10, x->pre ? 11u : base, 0, 0, 29));
		page_lookup(e, 10, !x->load, slow);
		E(e, ppc_rlwinm(8, 10, 0, 18, 29));
		E(e, ppc_cmplwi(6, 8, 0x3FF8u));
		bc_to(e, BO_T, CRB(6, GT), slow);
		E(e, ppc_addi(9, 8, 4));
		if (x->load) {
			E(e, ppc_lwbrx(3, 7, 8));
			E(e, ppc_lwbrx(4, 7, 9));
			if (x->wb)
				E(e, ppc_clrldi(base, 11, 32));
			E(e, ppc_mr(G(x->rd), 3));
			E(e, ppc_mr(G(x->rd + 1u), 4));
		} else {
			E(e, ppc_stwbrx(G(x->rd), 7, 8));
			E(e, ppc_stwbrx(G(x->rd + 1u), 7, 9));
			if (x->wb)
				E(e, ppc_clrldi(base, 11, 32));
		}
		break;
	}
	default:
		break;
	}
	bind(e, join);
	{
		const unsigned save = e->sec;

		e->sec = SEC_COLD;
		bind(e, slow);
		li32(e, 4, x->raw);
		li32(e, 5, at);
		li32(e, 6, e->strict ? e->rem_max[e->i] : 0xFFFFu);
		E(e, ppc_li(7, (int32_t)(P + 1)));
		bl_abs(e, e->j->ent_call);
		if (x->cyc > 1)
			E(e, ppc_addis(RCYC, RCYC, -(int32_t)(x->cyc - 1)));
		E(e, ppc_cmpwi(0, 3, 0));
		bc_to(e, BO_T, CRB(0, EQ), join);
		if (x->call_ro)
			flags_to_mem_cr(e, live);
		add_cyc(e, P + x->cyc, e->ipend);
		E(e, ppc_li(3, 0));
		E(e, ppc_li(4, (int32_t)(2 | live << 4)));
		b_abs(e, e->j->ent_exit);
		if (xirq) {
			bind(e, xirq);
			emit_cold_exit_mem(e, live, next, P + x->cyc, e->ipend);
		}
		e->sec = save;
	}
	/* MRS: las banderas no cambian (quedaron también en memoria) */
	if (!x->call_ro)
		e->in_mem = F_ALL;
}

/* AI_CALL: el intérprete ejecuta la instrucción con los registros en memoria. */
static void emit_call(struct em *e, const struct ai *x, unsigned P)
{
	const unsigned live = cold_live(e, e->i);
	const unsigned ex = newlab(e);
	const uint32_t at = x->addr | x->thumb;

	if (x->fast) {
		emit_call_fast(e, x, P);
		return;
	}
	if (!x->call_ro)
		flags_to_mem_cr(e, F_ALL); /* las que no dejó la instrucción que las fijó (no debería pasar) */
	li32(e, 4, x->raw);
	li32(e, 5, at);
	li32(e, 6, e->strict ? e->rem_max[e->i] : 0xFFFFu);
	E(e, ppc_li(7, (int32_t)(P + 1)));
	bl_abs(e, e->j->ent_call);
	E(e, ppc_cmpwi(0, 3, 0));
	bc_to(e, BO_F, CRB(0, EQ), ex);
	{
		const unsigned save = e->sec;

		e->sec = SEC_COLD;
		bind(e, ex);
		if (x->call_ro)
			flags_to_mem_cr(e, live);
		add_cyc(e, P + 1, e->ipend);
		E(e, ppc_li(3, 0));
		E(e, ppc_li(4, (int32_t)(2 | live << 4)));
		b_abs(e, e->j->ent_exit);
		e->sec = save;
	}
	if (!x->call_ro)
		fl_all_mem(e);
}

/* ---- proceso de datos ----------------------------------------------------------------------------------- */

static void set_loc(struct em *e, unsigned i, unsigned t, unsigned b, unsigned b2, unsigned inv)
{
	e->fl[i].t = (uint8_t)t;
	e->fl[i].b = (uint8_t)b;
	e->fl[i].b2 = (uint8_t)b2;
	e->fl[i].inv = (uint8_t)inv;
}

#define NEEDC(m) ((need_cond & (m)) != 0)
#define C_EQNE ((1u << 0) | (1u << 1))
#define C_CSCC ((1u << 2) | (1u << 3))
#define C_MIPL ((1u << 4) | (1u << 5))
#define C_VSVC ((1u << 6) | (1u << 7))
#define C_HILS ((1u << 8) | (1u << 9))
#define C_GELT ((1u << 10) | (1u << 11))
#define C_GTLE ((1u << 12) | (1u << 13))

/*
 * Banderas de una resta a − b = res (CMP, SUBS, RSBS con los operandos cambiados). `bimm` >= 0: b es esa constante
 * (rb sin usar). Registros a, rb, res de 32 bits válidos en la parte baja.
 */
static void flags_sub(struct em *e, unsigned ra, unsigned rb, int64_t bimm, unsigned res)
{
	const unsigned i = e->i, need = e->need_flags[i], need_cond = e->need_cond[i];
	const int wantS = NEEDC(C_GELT | C_GTLE | C_VSVC) || (need & F_V);
	const int wantU = NEEDC(C_CSCC | C_HILS) || (need & F_C) || e->need_cdata[i];
	int wantN = NEEDC(C_MIPL | C_VSVC) || (need & (F_N | F_V));
	const int anyZ = NEEDC(C_EQNE) || (need & F_Z);
	unsigned rbx = rb;

	if (bimm >= 0) {
		if (wantS || wantU || e->mat[i]) {
			if (!((wantS && (bimm > 0x7FFF)) || (wantU && bimm > 0xFFFF) || e->mat[i])) {
				rbx = 99; /* comparar con inmediato */
			} else {
				li32(e, 9, (uint32_t)bimm);
				rbx = 9;
			}
		}
	}
	if (anyZ && !wantS && !wantU)
		wantN = 1;
	/* vistas */
	if (wantS) {
		if (rbx == 99)
			E(e, ppc_cmpwi(4, ra, (int32_t)bimm));
		else
			E(e, ppc_cmpw(4, ra, rbx));
	}
	if (wantU) {
		if (rbx == 99)
			E(e, ppc_cmplwi(3, ra, (uint32_t)bimm));
		else
			E(e, ppc_cmplw(3, ra, rbx));
	}
	if (wantN)
		E(e, ppc_cmpwi(2, res, 0));
	/* ubicaciones */
	e->sc_hi = e->sc_lt = e->sc_gt = -1;
	e->sub_mask = F_ALL;
	if (wantS) {
		e->sc_lt = (int8_t)CRB(4, LT);
		e->sc_gt = (int8_t)CRB(4, GT);
	}
	if (wantU)
		e->sc_hi = (int8_t)CRB(3, GT);
	if (wantS || wantU || wantN)
		set_loc(e, 1, L_CR, wantS ? CRB(4, EQ) : wantU ? CRB(3, EQ) : CRB(2, EQ), 0, 0);
	else
		set_loc(e, 1, L_MEM, 0, 0, 0);
	set_loc(e, 0, wantN ? L_CR : L_MEM, CRB(2, LT), 0, 0);
	set_loc(e, 2, wantU ? L_CR : L_MEM, CRB(3, LT), 0, 1);
	if (wantS && wantN)
		set_loc(e, 3, L_XOR, CRB(2, LT), CRB(4, LT), 0);
	else
		set_loc(e, 3, L_MEM, 0, 0, 0);
	e->in_mem = 0;
	if (e->mat[i]) {
		/* NZCV en memoria: N, Z de res; C = no hubo préstamo; V = ((a ^ b) & (a ^ res)) >> 31 */
		const unsigned rbv = rbx == 99 ? 9 : rbx;

		if (rbx == 99)
			li32(e, 9, (uint32_t)bimm);
		nz_gpr(e, 7, res);
		E(e, ppc_subf(8, rbv, ra));         /* a − b en 64 bits: el bit 63 es el préstamo */
		E(e, ppc_rldicl(8, 8, 1, 63));
		E(e, ppc_xori(8, 8, 1));
		E(e, ppc_xor(5, ra, rbv));
		E(e, ppc_xor(6, ra, res));
		E(e, ppc_and(5, 5, 6));
		store_flags(e, 7, 8, 5);
		e->in_mem = F_ALL;
	}
}

/* Banderas de una suma o de ADC/SBC/RSC hechas en 64 bits: `t64` = resultado en 64 bits (con el acarreo en el bit 32
 * para las sumas; con signo para las restas con acarreo: `sub` = 1), res = los 32 bits bajos; `vx` = registro con
 * V en el bit 31 ya calculado. */
static void flags_arith(struct em *e, unsigned res, unsigned t64, int sub, unsigned vx)
{
	const unsigned i = e->i, need = e->need_flags[i], need_cond = e->need_cond[i];
	const int wantC = NEEDC(C_CSCC | C_HILS) || (need & F_C) || e->need_cdata[i] || NEEDC(C_HILS);
	const int wantV = NEEDC(C_VSVC | C_GELT | C_GTLE) || (need & F_V);
	const int wantN = 1;

	(void)wantN;
	E(e, ppc_cmpwi(2, res, 0));
	if (wantC || e->mat[i]) {
		if (sub) { /* C = !(t64 < 0) */
			E(e, ppc_rldicl(8, t64, 1, 63));
			E(e, ppc_xori(8, 8, 1));
		} else {
			E(e, ppc_rldicl(8, t64, 32, 63));
		}
		if (wantC)
			E(e, ppc_cmpwi(3, 8, 0));
	}
	if (wantV)
		E(e, ppc_cmpwi(4, vx, 0));
	e->sc_hi = e->sc_lt = e->sc_gt = -1;
	e->sub_mask = 0;
	set_loc(e, 0, L_CR, CRB(2, LT), 0, 0);
	set_loc(e, 1, L_CR, CRB(2, EQ), 0, 0);
	set_loc(e, 2, wantC ? L_CR : L_MEM, CRB(3, GT), 0, 0);
	set_loc(e, 3, wantV ? L_CR : L_MEM, CRB(4, LT), 0, 0);
	e->in_mem = 0;
	if (e->mat[i]) {
		nz_gpr(e, 7, res);
		store_flags(e, 7, 8, (int)vx);
		e->in_mem = F_ALL;
	}
}

/* N y Z de res; C según `cmode`: -1 sin cambio, 0/1 constante, 2 = el bit 0 de `creg`. V sin cambio. */
static void flags_logic(struct em *e, unsigned res, int cmode, unsigned creg)
{
	const unsigned i = e->i, need = e->need_flags[i], need_cond = e->need_cond[i];
	const int wantC = cmode == 2 && (NEEDC(C_CSCC | C_HILS) || (need & F_C) || e->need_cdata[i]);
	const unsigned keep = F_V | (cmode == -1 ? F_C : 0u);
	const uint8_t old_in_mem = e->in_mem;

	protect_fields(e, (1u << 2) | (wantC ? 1u << 3 : 0u), keep);
	E(e, ppc_cmpwi(2, res, 0));
	if (wantC)
		E(e, ppc_cmpwi(3, creg, 0));
	e->sub_mask &= (uint8_t)~(F_N | F_Z | (cmode != -1 ? F_C : 0u));
	if (cmode != -1)
		e->sc_hi = -1;
	e->sc_lt = e->sc_gt = -1;
	set_loc(e, 0, L_CR, CRB(2, LT), 0, 0);
	set_loc(e, 1, L_CR, CRB(2, EQ), 0, 0);
	if (cmode == 0 || cmode == 1)
		set_loc(e, 2, cmode ? L_K1 : L_K0, 0, 0, 0);
	else if (cmode == 2)
		set_loc(e, 2, wantC ? L_CR : L_MEM, CRB(3, GT), 0, 0);
	e->in_mem = (uint8_t)(old_in_mem & keep);
	if (e->mat[i]) {
		/* (r7 puede ser el acarreo: N y Z en r8) */
		nz_gpr(e, 8, res);
		if (cmode == 0 || cmode == 1) {
			E(e, ppc_li(9, cmode));
			E(e, ppc_lwz(0, RST, OFF_NZCV));
			E(e, ppc_rlwimi(8, 0, 0, 3, 3));
			E(e, ppc_rlwimi(8, 9, 29, 2, 2));
			E(e, ppc_stw(8, RST, OFF_NZCV));
		} else if (cmode == 2) {
			E(e, ppc_lwz(0, RST, OFF_NZCV));
			E(e, ppc_rlwimi(8, 0, 0, 3, 3));
			E(e, ppc_rlwimi(8, creg, 29, 2, 2));
			E(e, ppc_stw(8, RST, OFF_NZCV));
		} else {
			store_flags(e, 8, -1, -1);
		}
		/* lo que no escribe viene de memoria: si estaba ahí, sigue; si no, se dejó como estaba (muerto) */
		e->in_mem = (uint8_t)(F_N | F_Z | (cmode != -1 ? F_C : 0u) | (old_in_mem & keep));
		if (cmode == 2 && !wantC)
			e->fl[2].t = L_MEM;
	}
	if (cmode == 2 && !wantC && !e->mat[i]) {
		/* el acarreo nuevo no se necesita en el bloque: queda «en memoria» pero sin escribir */
		e->fl[2].t = L_MEM;
		e->in_mem &= (uint8_t)~F_C;
	}
}

static void emit_dp(struct em *e, const struct ai *x)
{
	const unsigned op = x->op;
	const int logical = op == 0 || op == 1 || op == 8 || op == 9 || op >= 12;
	const int test = op >= 8 && op <= 11;
	const int uses_rn = op != 13 && op != 15;
	const int s = x->s;
	unsigned ra = 0, rb = 0, rt;
	int64_t bimm = -1;
	int cmode = -1; /* acarreo del desplazador para las lógicas con S */

	/* operando 2 */
	if (x->o2 == O2_IMM) {
		bimm = x->imm;
		if (s && logical)
			cmode = x->ic;
	} else if (x->o2 == O2_SHI) {
		const unsigned rm = rget(e, x, x->rm, 5);

		rb = shift_imm(e, 6, rm, x->sht, x->sha);
		if (s && logical && !(x->sht == 0 && x->sha == 0)) {
			shift_imm_carry(e, 7, rm, x->sht, x->sha);
			cmode = 2;
		}
	} else {
		/* por registro: ayudante (valor y acarreo) */
		const unsigned rm = rget(e, x, x->rm, 5);

		E(e, ppc_mr(4, rm));
		E(e, ppc_rlwinm(5, G(x->rs), 0, 24, 31));
		if (s && logical)
			carry_gpr(e, 6);
		else
			E(e, ppc_li(6, 0));
		E(e, ppc_li(3, (int32_t)x->sht));
		call_fn(e, AH_SHIFT);
		E(e, ppc_clrldi(6, 3, 32));
		rb = 6;
		if (s && logical) {
			E(e, ppc_rldicl(7, 3, 32, 63));
			cmode = 2;
		}
	}
	if (uses_rn)
		ra = rget(e, x, x->rn, 4);

	/* resultado: con S o con rd = r15, en r3; si no, directo en el registro guest */
	rt = (s || x->rd == 15 || test) ? 3u : G(x->rd);
	/* con S, directo en el registro guest si no es uno de los operandos (las banderas los necesitan después) */
	if (s && !test && x->rd != 15 && (!uses_rn || G(x->rd) != ra) && (bimm >= 0 || G(x->rd) != rb))
		rt = G(x->rd);

	/* inmediatos: llevarlos a registro si la operación no tiene forma con inmediato */
	if (bimm >= 0) {
		const uint32_t v = (uint32_t)bimm;
		int done = 0;

		switch (op) {
		case 13: /* MOV */
			li32(e, rt, v);
			done = 1;
			break;
		case 15: /* MVN */
			li32(e, rt, ~v);
			done = 1;
			break;
		case 12: /* ORR */
		case 1:  /* EOR */
		case 9:  /* TEQ */
			if (v <= 0xFFFF) {
				E(e, op == 12 ? ppc_ori(rt, ra, v) : ppc_xori(rt, ra, v));
				done = 1;
			} else if (!(v & 0xFFFF)) {
				E(e, op == 12 ? ppc_oris(rt, ra, v >> 16) : ppc_xoris(rt, ra, v >> 16));
				done = 1;
			}
			break;
		case 4:  /* ADD */
		case 11: /* CMN */
			if (!s && v <= 0x7FFF) {
				E(e, ppc_addi(rt, ra, (int32_t)v));
				E(e, ppc_clrldi(rt, rt, 32));
				done = 1;
			}
			break;
		case 2:  /* SUB */
			if (!s && v <= 0x8000) {
				E(e, ppc_addi(rt, ra, -(int32_t)v));
				E(e, ppc_clrldi(rt, rt, 32));
				done = 1;
			}
			break;
		default:
			break;
		}
		if (done) {
			if (s && logical)
				flags_logic(e, rt, cmode, 0);
			goto store_result;
		}
		/* SUB/CMP con S e inmediato: la resta en 64 bits y las vistas con inmediato */
		if (s && (op == 2 || op == 10) && v <= 0x8000) {
			E(e, ppc_addi(rt, ra, -(int32_t)v));
			E(e, ppc_clrldi(rt, rt, 32));
			flags_sub(e, ra, 0, (int64_t)v, rt);
			goto store_result;
		}
		li32(e, 6, v);
		rb = 6;
	}

	switch (op) {
	case 0: case 8:
		E(e, ppc_and(rt, ra, rb));
		break;
	case 1: case 9:
		E(e, ppc_xor(rt, ra, rb));
		break;
	case 12:
		E(e, ppc_or(rt, ra, rb));
		break;
	case 13:
		if (rt != rb)
			E(e, ppc_mr(rt, rb));
		break;
	case 14:
		E(e, ppc_andc(rt, ra, rb));
		break;
	case 15:
		E(e, ppc_nor(rt, rb, rb));
		E(e, ppc_clrldi(rt, rt, 32));
		break;
	case 2: case 10: /* SUB, CMP */
		E(e, ppc_subf(rt, rb, ra));
		E(e, ppc_clrldi(rt, rt, 32));
		if (s)
			flags_sub(e, ra, rb, -1, rt);
		goto store_result;
	case 3: /* RSB */
		E(e, ppc_subf(rt, ra, rb));
		E(e, ppc_clrldi(rt, rt, 32));
		if (s)
			flags_sub(e, rb, ra, -1, rt);
		goto store_result;
	case 4: case 11: /* ADD, CMN */
		if (s) {
			const unsigned i = e->i, need = e->need_flags[i], need_cond = e->need_cond[i];
			const int wantC = NEEDC(C_CSCC | C_HILS) || (need & F_C) || e->need_cdata[i];
			const int wantV = NEEDC(C_VSVC | C_GELT | C_GTLE) || (need & F_V);

			E(e, ppc_add(8, ra, rb)); /* 64 bits: el acarreo en el bit 32 */
			E(e, ppc_clrldi(rt, 8, 32));
			if (wantV || e->mat[i]) {
				E(e, ppc_eqv(5, ra, rb));
				E(e, ppc_xor(9, ra, rt));
				E(e, ppc_and(5, 5, 9));
			}
			{
				/* acarreo: cmplw res, a → LT = C; V en cr4 */
				E(e, ppc_cmpwi(2, rt, 0));
				if (wantC)
					E(e, ppc_cmplw(3, rt, ra));
				if (wantV)
					E(e, ppc_cmpwi(4, 5, 0));
				e->sc_hi = e->sc_lt = e->sc_gt = -1;
				e->sub_mask = 0;
				set_loc(e, 0, L_CR, CRB(2, LT), 0, 0);
				set_loc(e, 1, L_CR, CRB(2, EQ), 0, 0);
				set_loc(e, 2, wantC ? L_CR : L_MEM, CRB(3, LT), 0, 0);
				set_loc(e, 3, wantV ? L_CR : L_MEM, CRB(4, LT), 0, 0);
				e->in_mem = 0;
				if (e->mat[i]) {
					E(e, ppc_rldicl(8, 8, 32, 63));
					nz_gpr(e, 7, rt);
					store_flags(e, 7, 8, 5);
					e->in_mem = F_ALL;
				}
			}
		} else {
			E(e, ppc_add(rt, ra, rb));
			E(e, ppc_clrldi(rt, rt, 32));
		}
		goto store_result;
	case 5: /* ADC */
	case 6: /* SBC: a + ~b + c = a − b − 1 + c */
	case 7: /* RSC: b − a − 1 + c */
		carry_gpr(e, 10);
		if (op == 5) {
			E(e, ppc_add(8, ra, rb));
			E(e, ppc_add(8, 8, 10));
		} else {
			if (op == 6)
				E(e, ppc_subf(8, rb, ra));
			else
				E(e, ppc_subf(8, ra, rb));
			E(e, ppc_add(8, 8, 10));
			E(e, ppc_addi(8, 8, -1));
		}
		E(e, ppc_clrldi(rt, 8, 32));
		if (s) {
			/* V: suma ~(a ^ b) & (a ^ r); resta (a ^ b) & (a ^ r), con a y b de la operación */
			const unsigned va = op == 7 ? rb : ra, vb = op == 7 ? ra : rb;

			if (op == 5)
				E(e, ppc_eqv(5, va, vb));
			else
				E(e, ppc_xor(5, va, vb));
			E(e, ppc_xor(9, va, rt));
			E(e, ppc_and(5, 5, 9));
			flags_arith(e, rt, 8, op != 5, 5);
		}
		goto store_result;
	default:
		break;
	}
	/* lógicas */
	if (s)
		flags_logic(e, rt, cmode, 7);

store_result:
	if (test)
		return;
	if (x->rd == 15) {
		/* salto: ARM alinea a 4, Thumb a 2 (sin cambio de estado) */
		if (x->thumb) {
			E(e, ppc_rlwinm(3, rt, 0, 0, 30));
			E(e, ppc_ori(3, 3, 1));
		} else {
			E(e, ppc_rlwinm(3, rt, 0, 0, 29));
		}
		exit_indirect(e, 0, 0, F_ALL);
		return;
	}
	if (rt != G(x->rd))
		E(e, ppc_mr(G(x->rd), rt));
}

/* ---- multiplicaciones, CLZ ----------------------------------------------------------------------------- */

/* ciclos de mul_cycles(rs) (con signo) sumados a r29 */
static void mul_cyc(struct em *e, unsigned rs, int sgn)
{
	if (sgn) {
		E(e, ppc_srawi(0, rs, 31));
		E(e, ppc_xor(0, rs, 0));
		E(e, ppc_ori(0, 0, 1));
	} else {
		E(e, ppc_ori(0, rs, 1));
	}
	E(e, ppc_cntlzw(0, 0));
	E(e, ppc_subfic(0, 0, 39));
	E(e, ppc_rlwinm(0, 0, 29, 3, 31));
	E(e, ppc_sldi(0, 0, 16));
	E(e, ppc_add(RCYC, RCYC, 0));
}

static void emit_mul(struct em *e, const struct ai *x)
{
	const unsigned rs = G(x->rs), rm = G(x->rm);

	mul_cyc(e, rs, 1);
	E(e, ppc_mullw(3, rm, rs));
	if (x->acc)
		E(e, ppc_add(3, 3, G(x->rn)));
	E(e, ppc_clrldi(3, 3, 32));
	if (x->s)
		flags_logic(e, 3, -1, 0);
	E(e, ppc_mr(G(x->rd), 3));
}

static void emit_mull(struct em *e, const struct ai *x)
{
	const unsigned rs = G(x->rs), rm = G(x->rm);

	mul_cyc(e, rs, x->sgn);
	if (x->sgn) {
		E(e, ppc_extsw(4, rm));
		E(e, ppc_extsw(5, rs));
		E(e, ppc_mulld(3, 4, 5));
	} else {
		E(e, ppc_mulld(3, rm, rs));
	}
	if (x->acc) {
		E(e, ppc_sldi(4, G(x->rd), 32));
		E(e, ppc_or(4, 4, G(x->rn)));
		E(e, ppc_add(3, 3, 4));
	}
	if (x->s) {
		/* N = bit 63, Z = los 64 bits en cero */
		const unsigned i = e->i;

		protect_fields(e, 1u << 2, F_C | F_V);
		E(e, ppc_cmpdi(2, 3, 0));
		e->sub_mask &= (uint8_t)~(F_N | F_Z);
		e->sc_lt = e->sc_gt = -1;
		set_loc(e, 0, L_CR, CRB(2, LT), 0, 0);
		set_loc(e, 1, L_CR, CRB(2, EQ), 0, 0);
		e->in_mem &= (uint8_t)(F_C | F_V);
		if (e->mat[i]) {
			const uint8_t old = e->in_mem;

			E(e, ppc_rldicl(7, 3, 32, 32)); /* parte alta → baja: N del bit 31 */
			E(e, ppc_rlwinm(7, 7, 0, 0, 0));
			/* Z: ¿los 64 bits en cero? cntlzd no está en la lista: (hi | lo) == 0 */
			E(e, ppc_rldicl(0, 3, 32, 32));
			E(e, ppc_or(0, 0, 3));
			E(e, ppc_clrldi(0, 0, 32));
			E(e, ppc_cntlzw(0, 0));
			E(e, ppc_rlwinm(0, 0, 25, 1, 1));
			E(e, ppc_or(7, 7, 0));
			store_flags(e, 7, -1, -1);
			e->in_mem = (uint8_t)(F_N | F_Z | old);
		}
	}
	E(e, ppc_clrldi(G(x->rn), 3, 32));
	E(e, ppc_rldicl(G(x->rd), 3, 32, 32));
}

static void emit_clz(struct em *e, const struct ai *x)
{
	E(e, ppc_cntlzw(G(x->rd), G(x->rm)));
}

/* ---- memoria ------------------------------------------------------------------------------------------- */

/* Ruta rápida: página de `ra` en la tabla de lectura (o de escritura) → r7 = puntero (salta a `cold` si es NULL). */
static void page_lookup(struct em *e, unsigned ra, int wr, unsigned cold)
{
	E(e, ppc_rlwinm(6, ra, 21, 11, 28));
	if (wr)
		E(e, ppc_addis(6, 6, 0x20));
	E(e, ppc_ldx(7, RPT, 6));
	E(e, ppc_cmpdi(1, 7, 0));
	bc_to(e, BO_T, CRB(1, EQ), cold);
}

/* LDR rd, [pc, #±imm] (el «literal pool»): la dirección se conoce al traducir. Si la palabra está en la memoria del propio
 * código (la misma vista para datos y para captación) y dentro de las cubetas del bloque, se lee ahora y el bloque cubre
 * también esos bytes: una escritura ahí lo invalida, como al código. Devuelve 1 y el valor en *v. */
static int literal_value(struct em *e, const struct ai *x, uint32_t *v)
{
	struct arec *j = e->j;
	struct ablock *b = e->b;
	uint32_t a, ph, avail;
	const uint8_t *p, *d;

	if (!x->load || x->rn != 15 || x->o2 != O2_IMM || x->wb || x->rd == 15 || !x->pre)
		return 0;
	a = x->up ? x->pcv + x->imm : x->pcv - x->imm;
	if (a & (x->msz - 1u))
		return 0;
	p = j->cfg.code_ptr(j->cfg.ctx, a, &ph, &avail);
	if (!p || avail < x->msz)
		return 0;
	d = j->pt[a >> AREC_PAGE_SHIFT];
	if (!d || d + (a & ((1u << AREC_PAGE_SHIFT) - 1u)) != p)
		return 0;
	if (b->phys_lo == AREC_PHYS_NONE) {
		if (ph != AREC_PHYS_NONE)
			return 0;
	} else {
		if (ph == AREC_PHYS_NONE || ph < e->win_lo || ph + x->msz > e->win_hi)
			return 0;
		if (ph < b->phys_lo)
			b->phys_lo = ph;
		if (ph + x->msz > b->phys_hi)
			b->phys_hi = ph + x->msz;
	}
	switch (x->msz) {
	case 4:
		*v = rd32le(p);
		break;
	case 2:
		*v = x->msign ? (uint32_t)(int32_t)(int16_t)rd16le(p) : rd16le(p);
		break;
	default:
		*v = x->msign ? (uint32_t)(int32_t)(int8_t)p[0] : p[0];
		break;
	}
	return 1;
}

static void emit_mem(struct em *e, const struct ai *x, unsigned P)
{
	{
		uint32_t lit;

		if (literal_value(e, x, &lit)) {
			li32(e, G(x->rd), lit);
			return;
		}
	}
	const unsigned cold = newlab(e), join = newlab(e);
	const unsigned live = cold_live(e, e->i);
	const unsigned mk = x->load ? (x->msz == 4 ? MK_W : x->msz == 2 ? (x->msign ? MK_SH : MK_H) : (x->msign ? MK_SB : MK_B))
				    : (x->msz == 4 ? MK_W : x->msz == 2 ? MK_H : MK_B);
	/* destino de la carga: r3 si va al pc o si rd = rn con escritura de vuelta (gana la carga) */
	const unsigned dst = x->load ? ((x->rd == 15 || (x->wb && x->rd == x->rn) || (x->o2 != O2_IMM && x->rd == x->rm)) ? 3u : G(x->rd)) : 0u;
	unsigned base, off = 0, ra, rv = 0;

	const int zero_off = x->o2 == O2_IMM && x->imm == 0;
	int wb = x->wb;

	base = rget(e, x, x->rn, 4);
	if (x->o2 != O2_IMM)
		off = shift_imm(e, 5, G(x->rm), x->sht, x->sha);
	/* nb = base ± desplazamiento en r11 (escritura de vuelta); ra = dirección del acceso (32 bits bajos válidos) */
	if (zero_off) {
		wb = 0; /* la base no cambia */
		ra = base;
	} else {
		if (x->o2 == O2_IMM) {
			const int32_t d = x->up ? (int32_t)x->imm : -(int32_t)x->imm;

			E(e, ppc_addi(11, base, d));
		} else {
			E(e, x->up ? ppc_add(11, base, off) : ppc_subf(11, off, base));
		}
		ra = x->pre ? 11u : base; /* sin pre, la base es G(rn): no cambia hasta la escritura de vuelta */
	}
	if (!x->load)
		rv = x->rd == 15 ? (li32(e, 10, x->pcv + 4), 10u) : G(x->rd);

	if (x->load && x->msz == 4) {
		/* LDR no alineado rota: por el camino lento */
		E(e, ppc_rlwinm(8, ra, 0, 30, 31));
		E(e, ppc_cmplwi(6, 8, 0));
		bc_to(e, BO_F, CRB(6, EQ), cold);
	}
	page_lookup(e, ra, !x->load, cold);
	E(e, ppc_rlwinm(8, ra, 0, 18, x->msz == 4 ? 29 : x->msz == 2 ? 30 : 31));
	if (x->load) {
		if (x->msz == 4)
			E(e, ppc_lwbrx(dst, 7, 8));
		else if (x->msz == 2)
			E(e, ppc_lhbrx(dst, 7, 8));
		else
			E(e, ppc_lbzx(dst, 7, 8));
		if (x->msign) {
			E(e, x->msz == 2 ? ppc_extsh(dst, dst) : ppc_extsb(dst, dst));
			E(e, ppc_clrldi(dst, dst, 32));
		}
	} else {
		if (x->msz == 4)
			E(e, ppc_stwbrx(rv, 7, 8));
		else if (x->msz == 2)
			E(e, ppc_sthbrx(rv, 7, 8));
		else
			E(e, ppc_stbx(rv, 7, 8));
	}
	bind(e, join);
	/* escritura de vuelta y destino (si rd = rn gana la carga) */
	if (wb)
		E(e, ppc_clrldi(G(x->rn), 11, 32));
	if (x->load && dst == 3 && x->rd != 15)
		E(e, ppc_mr(G(x->rd), 3));

	/* camino frío: el ayudante hace el acceso con las reglas del intérprete */
	{
		const unsigned save = e->sec;
		const unsigned mx = newlab(e);

		e->sec = SEC_COLD;
		bind(e, cold);
		E(e, ppc_clrldi(4, ra, 32));
		if (x->load) {
			E(e, ppc_li(5, (int32_t)mk));
		} else {
			E(e, ppc_mr(5, rv));
			E(e, ppc_li(6, (int32_t)mk));
		}
		E(e, ppc_li(7, (int32_t)(P + 1)));
		bl_abs(e, x->load ? e->j->ent_rd : e->j->ent_wr);
		if (x->load) {
			/* bit 32: hay que salir después de la instrucción (una IRQ que levantó la lectura) */
			E(e, ppc_srdi(10, 3, 32));
			E(e, ppc_clrldi(dst, 3, 32));
			if (!x->ends) {
				E(e, ppc_cmpwi(0, 10, 0));
				bc_to(e, BO_F, CRB(0, EQ), mx);
				if (e->strict)
					strict_overrun(e, P + x->cyc, mx);
			}
			b_to(e, join);
			bind(e, mx);
			if (!x->ends) {
				if (wb)
					E(e, ppc_clrldi(G(x->rn), 11, 32));
				if (dst == 3 && x->rd != 15)
					E(e, ppc_mr(G(x->rd), 3));
				emit_cold_exit_mem(e, live, (x->addr + x->size) | x->thumb, P + x->cyc, e->ipend);
			}
		} else {
			if (e->strict)
				strict_overrun(e, P + x->cyc, mx);
			E(e, ppc_cmpwi(0, 3, 0));
			bc_to(e, BO_T, CRB(0, EQ), join);
			bind(e, mx);
			if (wb)
				E(e, ppc_clrldi(G(x->rn), 11, 32));
			emit_cold_exit_mem(e, live, (x->addr + x->size) | x->thumb, P + x->cyc, e->ipend);
		}
		e->sec = save;
	}
	if (x->load && x->rd == 15) {
		/* LDR pc (ARMv5: con cambio de estado) */
		E(e, ppc_rlwinm(4, 3, 1, 30, 30));
		E(e, ppc_xori(4, 4, 2));
		E(e, ppc_andc(3, 3, 4));
		exit_indirect(e, 0, 0, F_ALL);
	}
}

static unsigned popc(uint32_t x)
{
	unsigned n = 0;

	for (; x; x &= x - 1)
		n++;
	return n;
}

static void emit_block(struct em *e, const struct ai *x, unsigned P)
{
	const unsigned n = popc(x->list), cold = newlab(e), join = newlab(e);
	const unsigned base = G(x->rn);
	const unsigned live = cold_live(e, e->i);
	const int pcload = x->load && (x->list & 0x8000u);
	int32_t start, fin;

	if (x->up) {
		start = x->pre ? 4 : 0;
		fin = 4 * (int32_t)n;
	} else {
		start = -4 * (int32_t)n + (x->pre ? 0 : 4);
		fin = -4 * (int32_t)n;
	}
	E(e, ppc_addi(11, base, start));
	E(e, ppc_rlwinm(11, 11, 0, 0, 29));
	page_lookup(e, 11, !x->load, cold);
	E(e, ppc_rlwinm(8, 11, 0, 18, 29));
	E(e, ppc_cmplwi(6, 8, 0x4000u - 4u * n));
	bc_to(e, BO_T, CRB(6, GT), cold);
	E(e, ppc_add(7, 7, 8));
	if (x->wb)
		E(e, ppc_addi(10, base, fin));
	{
		unsigned k = 0;

		for (unsigned r = 0; r < 16; r++) {
			if (!(x->list & (1u << r)))
				continue;
			if (k)
				E(e, ppc_addi(7, 7, 4));
			if (x->load) {
				E(e, ppc_lwbrx(r == 15 ? 3u : G(r), 0, 7));
			} else {
				unsigned v = G(r);

				if (r == 15) {
					li32(e, 9, x->pcv + 4);
					v = 9;
				}
				E(e, ppc_stwbrx(v, 0, 7));
			}
			k++;
		}
	}
	if (x->wb)
		E(e, ppc_clrldi(base, 10, 32));
	bind(e, join);
	{
		/* camino frío: el intérprete hace la instrucción entera */
		const unsigned save = e->sec;
		const unsigned ex = newlab(e);

		e->sec = SEC_COLD;
		bind(e, cold);
		li32(e, 4, x->raw & (x->thumb ? 0xFFFFu : 0xFFFFFFFFu));
		li32(e, 5, x->addr | x->thumb);
		li32(e, 6, e->strict ? (uint32_t)e->rem_max[e->i] : 0xFFFFu);
		/* r29 queda para que r29 + P + cyc sea c->cycles: K = P + cyc (el intérprete suma la captación y el resto) */
		E(e, ppc_li(7, (int32_t)(P + 1)));
		bl_abs(e, e->j->ent_call);
		if (x->cyc > 1)
			E(e, ppc_addis(RCYC, RCYC, -(int32_t)(x->cyc - 1)));
		E(e, ppc_cmpwi(0, 3, 0));
		if (!pcload)
			bc_to(e, BO_T, CRB(0, EQ), join);
		bind(e, ex);
		/* salida con el estado en memoria (el intérprete ya dejó r15 y el CPSR) */
		flags_to_mem_cr(e, live);
		add_cyc(e, P + x->cyc, e->ipend);
		E(e, ppc_li(3, 0));
		E(e, ppc_li(4, (int32_t)(2 | live << 4)));
		b_abs(e, e->j->ent_exit);
		e->sec = save;
	}
	if (pcload) {
		if (x->xchg) {
			E(e, ppc_rlwinm(4, 3, 1, 30, 30));
			E(e, ppc_xori(4, 4, 2));
			E(e, ppc_andc(3, 3, 4));
		} else {
			E(e, ppc_rlwinm(3, 3, 0, 0, 29));
		}
		exit_indirect(e, 0, 0, F_ALL);
	}
}

/* ---- saltos -------------------------------------------------------------------------------------------- */

static void emit_b(struct em *e, const struct ai *x)
{
	if (x->cond == AI_COND_AL) {
		if (x->link)
			li32(e, G(14), x->lrv);
		/* los ciclos de la instrucción ya están en lo pendiente */
		exit_link(e, x->target, 0, 0, e->live_after[e->i]);
		return;
	}
	{
		const unsigned taken = newlab(e);
		const int r = jump_if(e, x->cond, taken);
		const unsigned save = e->sec;

		if (r < 0)
			return; /* nunca: sigue (1 ciclo) */
		if (r == 0)
			e->sec = SEC_COLD;
		bind(e, taken);
		if (x->link)
			li32(e, G(14), x->lrv);
		exit_link(e, x->target, 2, 0, e->live_taken[e->i]);
		e->sec = save;
	}
}

static void emit_bx(struct em *e, const struct ai *x)
{
	E(e, ppc_mr(3, G(x->rm)));
	if (x->link)
		li32(e, G(14), x->lrv);
	/* destino: bit 0 = Thumb (alinea a 2) o ARM (alinea a 4) */
	E(e, ppc_rlwinm(4, 3, 1, 30, 30));
	E(e, ppc_xori(4, 4, 2));
	E(e, ppc_andc(3, 3, 4));
	exit_indirect(e, 0, 0, F_ALL);
}

static void emit_bllo(struct em *e, const struct ai *x)
{
	if (x->imm < 0x8000) {
		E(e, ppc_addi(3, G(14), (int32_t)x->imm));
	} else {
		li32(e, 3, x->imm);
		E(e, ppc_add(3, 3, G(14)));
	}
	if (x->xchg) { /* BL: Thumb */
		E(e, ppc_rlwinm(3, 3, 0, 0, 30));
		E(e, ppc_ori(3, 3, 1));
	} else { /* BLX: ARM */
		E(e, ppc_rlwinm(3, 3, 0, 0, 29));
	}
	li32(e, G(14), x->lrv);
	exit_indirect(e, 0, 0, F_ALL);
}

/* ---- bloque --------------------------------------------------------------------------------------------- */

static void emit_insn(struct em *e, const struct ai *x)
{
	const unsigned P = e->pend;
	const int cond = x->cond != AI_COND_AL && x->kind != AI_B;
	unsigned skip = 0;

	e->xextra = 0;
	if (cond) {
		/* una llamada al intérprete necesita las banderas en memoria en los dos caminos */
		if (x->kind == AI_CALL && !x->call_ro)
			flags_to_mem_cr(e, F_ALL);
		skip = newlab(e);
		e->pend += 1;
		e->ipend += x->ninsn;
		if (!skip_unless(e, x->cond, skip)) {
			bind(e, skip);
			return;
		}
		e->xextra = x->cyc - 1u;
	} else if (x->kind == AI_B && x->cond != AI_COND_AL) {
		e->pend += 1; /* no tomado; el tomado suma 2 en su salida */
		e->ipend += x->ninsn;
	} else {
		e->pend += x->cyc;
		e->ipend += x->ninsn;
	}
	switch (x->kind) {
	case AI_DP:
		emit_dp(e, x);
		break;
	case AI_MUL:
		emit_mul(e, x);
		break;
	case AI_MULL:
		emit_mull(e, x);
		break;
	case AI_CLZ:
		emit_clz(e, x);
		break;
	case AI_MEM:
		emit_mem(e, x, P);
		break;
	case AI_BLOCK:
		emit_block(e, x, P);
		break;
	case AI_B:
		emit_b(e, x);
		break;
	case AI_BX:
		emit_bx(e, x);
		break;
	case AI_BLLO:
		emit_bllo(e, x);
		break;
	case AI_CALL:
		emit_call(e, x, P);
		break;
	default:
		break;
	}
	e->xextra = 0;
	if (cond) {
		/* el resto del costo de la instrucción, solo si se ejecutó */
		if (x->cyc > 1 && !x->ends)
			E(e, ppc_addis(RCYC, RCYC, (int32_t)(x->cyc - 1)));
		bind(e, skip);
	}
}

/* Traduce; con far = 1 las llamadas a C van siempre por CTR. Devuelve palabras o < 0. */
static int emit_try(struct arec *j, const struct ai *ins, unsigned n, struct ablock *b, uint32_t block_id, int far)
{
	static struct em em; /* un solo hilo traduce */
	struct em *e = &em;
	unsigned maxcyc = 0;

	memset(e, 0, sizeof *e);
	e->hot = em_hot;
	e->cold = em_cold;
	e->j = j;
	e->b = b;
	e->block_id = block_id;
	e->base = (uint32_t)j->code_used;
	e->ins = ins;
	e->n = n;
	e->strict = j->cfg.strict;
	e->no_link = j->cfg.no_link;
	e->inline_jl = !j->cfg.no_link && !j->cfg.no_inline_jl;
	e->far = far;
	b->nexits = 0;
	fl_all_mem(e);
	tmpb_next = 0;

	for (unsigned i = 0; i < n; i++)
		maxcyc += ins[i].cyc + ((ins[i].kind == AI_MUL || ins[i].kind == AI_MULL) ? 4u : 0u);
	b->maxcyc = (uint16_t)(maxcyc > 0xFFFF ? 0xFFFF : maxcyc);
	if (b->phys_lo != AREC_PHYS_NONE) {
		e->win_lo = b->phys_lo >> AREC_BUCKET_SHIFT << AREC_BUCKET_SHIFT;
		e->win_hi = (((b->phys_hi - 1) >> AREC_BUCKET_SHIFT) + 1) << AREC_BUCKET_SHIFT;
		if (e->win_hi - e->win_lo > (2u << AREC_BUCKET_SHIFT))
			e->win_hi = e->win_lo + (2u << AREC_BUCKET_SHIFT);
	}
	analyze(e);

	/* prólogo: ¿hay presupuesto? */
	{
		const unsigned out = newlab(e);

		if (e->strict) {
			E(e, ppc_sradi(3, RCYC, 16));
			E(e, ppc_addi(3, 3, (int32_t)maxcyc));
			E(e, ppc_cmpdi(0, 3, 0));
			bc_to(e, BO_T, CRB(0, GT), out);
		} else {
			E(e, ppc_cmpdi(0, RCYC, 0));
			bc_to(e, BO_F, CRB(0, LT), out);
		}
		e->sec = SEC_COLD;
		bind(e, out);
		E(e, ppc_li(4, (int32_t)(0 | F_ALL << 4)));
		li32(e, 3, ins[0].addr | ins[0].thumb);
		E(e, ppc_ori(4, 4, 8)); /* bit 3 del tipo: salida por presupuesto (para las estadísticas) */
		b_abs(e, j->ent_exit);
		e->sec = SEC_HOT;
	}

	for (unsigned i = 0; i < n; i++) {
		e->i = i;
		emit_insn(e, &ins[i]);
		if (e->overflow)
			return -1;
		if (e->bad) {
			arec_bad_raw = ins[i].raw;
			arec_bad_addr = ins[i].addr | ins[i].thumb;
			return -6;
		}
	}
	/* caída del final */
	{
		const struct ai *last = &ins[n - 1];

		/* también si la última termina el bloque pero es condicional (BXEQ LR, LDMNE ..., pc): el camino sin ejecutar sigue */
		if (!last->ends || last->cond != AI_COND_AL) {
			e->i = n - 1;
			exit_link(e, next_pc(last), 0, 0, last->ends ? e->live_after[n - 1] : e->live_end);
		}
	}
	if (e->overflow)
		return -1;

	/* cierre: caliente + fría, arreglos de saltos */
	{
		const unsigned total = e->nh + e->nc;
		uint32_t *dst;

		if (j->code_used + total > j->code_words)
			return -2;
		dst = j->code + j->code_used;
		memcpy(dst, e->hot, e->nh * 4);
		memcpy(dst + e->nh, e->cold, e->nc * 4);
		for (unsigned k = 0; k < e->nfix; k++) {
			const struct efix *f = &e->fix[k];
			const uint32_t at = final_pos(e, f->sec, f->at);
			int64_t d;

			if (f->kind == 2 || f->kind == 4) {
				d = ((int64_t)f->lab - (int64_t)(e->base + at)) * 4;
				dst[at] = (dst[at] & ~0x03FFFFFCu) | ((uint32_t)d & 0x03FFFFFCu);
				continue;
			}
			if (f->kind == 3) {
				const uint64_t target = j->c->jit_fn[f->lab];
				const uint64_t here = (uint64_t)(uintptr_t)(dst + at);

				d = (int64_t)(target - here);
				if (d <= -0x2000000 || d >= 0x2000000)
					return -5; /* no llega: repetir con llamadas lejanas */
				dst[at] = (dst[at] & ~0x03FFFFFCu) | ((uint32_t)d & 0x03FFFFFCu);
				continue;
			}
			if (!e->lab_set[f->lab])
				return -3;
			d = ((int64_t)final_pos(e, e->lab_sec[f->lab], e->lab_pos[f->lab]) - (int64_t)at) * 4;
			if (f->kind == 0) {
				dst[at] = (dst[at] & ~0x03FFFFFCu) | ((uint32_t)d & 0x03FFFFFCu);
			} else {
				if (d < -32768 || d > 32767)
					return -4;
				dst[at] = (dst[at] & ~0xFFFCu) | ((uint32_t)d & 0xFFFCu);
			}
		}
		/* salidas: posiciones finales */
		for (unsigned k = 0; k < b->nexits; k++) {
			const uint32_t s = b->exits[k].site;

			b->exits[k].site = e->base + final_pos(e, s >> 31, s & 0x7FFFFFFFu);
			b->exits[k].stub = e->base + final_pos(e, e->lab_sec[b->exits[k].stub], e->lab_pos[b->exits[k].stub]);
		}
		b->entry = e->base;
		b->words = total;
		b->hot = e->nh;
		return (int)total;
	}
}

int arec_emit_block(struct arec *j, const struct ai *ins, unsigned n, struct ablock *b, uint32_t block_id)
{
	int r = emit_try(j, ins, n, b, block_id, j->cfg.far_calls || !j->can_exec);

	if (r == -5)
		r = emit_try(j, ins, n, b, block_id, 1);
	return r;
}


/* ---- rutinas comunes: entrada y salida ---------------------------------------------------------------------- */

#define FRAME 256

int arec_emit_common(struct arec *j)
{
	uint32_t *p = j->code;
	unsigned n = 0;

	/* entrada: r3 = c, r4 = código */
	j->ent_enter = 0;
	p[n++] = ppc_mflr(0);
	p[n++] = ppc_std(0, 1, 16);
	p[n++] = ppc_mfcr(0);
	p[n++] = ppc_stw(0, 1, 8);
	p[n++] = ppc_stdu(1, 1, -FRAME);
	for (unsigned r = 14; r < 32; r++)
		p[n++] = ppc_std(r, 1, (int32_t)(112 + 8 * (r - 14)));
	p[n++] = ppc_mr(RST, 3);
	for (unsigned r = 0; r < 15; r++)
		p[n++] = ppc_lwz(G(r), RST, OFF_R(r));
	p[n++] = ppc_ld(RCYC, RST, OFF_CYCLES);
	p[n++] = ppc_ld(0, RST, OFF_UNTIL);
	p[n++] = ppc_subf(RCYC, 0, RCYC);
	p[n++] = ppc_sldi(RCYC, RCYC, 16);
	p[n++] = ppc_ld(RPT, RST, OFF_PT);
	p[n++] = ppc_mtctr(4);
	p[n++] = PPC_BCTR;

	/* salida: r3 = valor, r4 = tipo | vivas << 4 */
	j->ent_exit = n;
	p[n++] = 30u << 26 | 4u << 21 | 3u << 16 | (56u & 31u) << 11 | (0u) << 6 | ((56u >> 5) & 1u) << 1 | 3u << 2; /* rldimi r3,r4,56,0 */
	for (unsigned r = 0; r < 15; r++)
		p[n++] = ppc_stw(G(r), RST, OFF_R(r));
	p[n++] = ppc_sradi(5, RCYC, 16);
	p[n++] = ppc_ld(6, RST, OFF_UNTIL);
	p[n++] = ppc_add(5, 5, 6);
	p[n++] = ppc_std(5, RST, OFF_CYCLES);
	p[n++] = ppc_rldicl(7, RCYC, 0, 48);
	p[n++] = ppc_ld(8, RST, OFF_ICOUNT);
	p[n++] = ppc_add(8, 8, 7);
	p[n++] = ppc_std(8, RST, OFF_ICOUNT);
	for (unsigned r = 14; r < 32; r++)
		p[n++] = ppc_ld(r, 1, (int32_t)(112 + 8 * (r - 14)));
	p[n++] = ppc_addi(1, 1, FRAME);
	p[n++] = ppc_ld(0, 1, 16);
	p[n++] = ppc_mtlr(0);
	p[n++] = ppc_lwz(0, 1, 8);
	p[n++] = ppc_mtcrf(0x38, 0);
	p[n++] = PPC_BLR;

	/* Caminos lentos compartidos: los llama el camino frío de cada acceso con bl (LR libre en el código generado).
	 * Entrada: r4 = dirección, r7 = K (ciclos del bloque hasta la captación de la instrucción: c->cycles = límite +
	 * (r29 >> 16) + K, como los deja el intérprete), y según la rutina r5/r6. Guardan r11 (la escritura de vuelta del
	 * acceso), r7 y LR en c->jit_spill; dejan r29 como corresponde al c->cycles que haya dejado el ayudante. */
	{
		const int32_t SP0 = (int32_t)offsetof(struct acore, jit_spill);
		const unsigned kinds[3] = { AH_RD, AH_WR, AH_CALL };
		uint32_t *ents[3] = { &j->ent_rd, &j->ent_wr, &j->ent_call };

		for (unsigned q = 0; q < 3; q++) {
			*ents[q] = n;
			p[n++] = ppc_mflr(0);
			p[n++] = ppc_std(0, RST, SP0 + 16);
			p[n++] = ppc_std(11, RST, SP0);
			p[n++] = ppc_std(7, RST, SP0 + 8);
			if (kinds[q] == AH_CALL) /* el intérprete trabaja con los registros en memoria */
				for (unsigned r = 0; r < 15; r++)
					p[n++] = ppc_stw(G(r), RST, OFF_R(r));
			p[n++] = ppc_sradi(8, RCYC, 16);
			p[n++] = ppc_ld(9, RST, OFF_UNTIL);
			p[n++] = ppc_add(8, 8, 9);
			p[n++] = ppc_add(8, 8, 7);
			p[n++] = ppc_std(8, RST, OFF_CYCLES);
			p[n++] = ppc_mr(3, RST);
			{
				const uint64_t target = j->c->jit_fn[kinds[q]];
				const int64_t d = (int64_t)(target - (uint64_t)(uintptr_t)(p + n));

				if (!j->cfg.far_calls && j->can_exec && d > -0x2000000 && d < 0x2000000) {
					p[n++] = ppc_bl((int32_t)d);
				} else {
					p[n++] = ppc_ld(0, RST, OFF_FN(kinds[q]));
					p[n++] = ppc_mtctr(0);
					p[n++] = PPC_BCTRL;
				}
			}
			if (kinds[q] == AH_CALL)
				for (unsigned r = 0; r < 15; r++)
					p[n++] = ppc_lwz(G(r), RST, OFF_R(r));
			p[n++] = ppc_ld(7, RST, SP0 + 8);
			p[n++] = ppc_ld(8, RST, OFF_CYCLES);
			p[n++] = ppc_ld(9, RST, OFF_UNTIL);
			p[n++] = ppc_subf(8, 9, 8);
			p[n++] = ppc_subf(8, 7, 8);
			p[n++] = ppc_sldi(8, 8, 16);
			p[n++] = ppc_rldimi(8, RCYC, 0, 48);
			p[n++] = ppc_mr(RCYC, 8);
			p[n++] = ppc_ld(11, RST, SP0);
			p[n++] = ppc_ld(0, RST, SP0 + 16);
			p[n++] = ppc_mtlr(0);
			p[n++] = PPC_BLR;
		}
	}
	j->code_used = n;
	j->code_base_used = n;
	return (int)n;
}
