/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * SalsaDS — recompilador: despachador, caché de bloques, enlaces, invalidación y los ayudantes en C que llama el
 * código generado. Ver arec.h y docs/fase4.md. Código propio (GPL-2.0-or-later).
 */
#include <stdlib.h>
#include <string.h>
#ifdef AREC_DEBUG
#include <stdio.h>
static int dbg_left = -1;
#endif

#include "cpu/acore_priv.h"
#include "jit/arec_priv.h"
#include <stdio.h>

#include "jit/ppcenc.h"

#if defined(__powerpc64__) && defined(__BYTE_ORDER__) && __BYTE_ORDER__ == __ORDER_BIG_ENDIAN__
#define AREC_CAN_EXEC 1
#else
#define AREC_CAN_EXEC 0
#endif

#define MAX_BLOCKS 16384u
#define MAX_LINKS 65536u
#define LIVE_FLAGS(r) ((unsigned)((r) >> 60) & 15u)

const char *const arec_call_kind_name[AREC_CALL_KINDS] = {
	"MRS", "MSR", "SWI", "CP15", "LDRD/STRD", "SWP", "Q/SMUL", "LDM/STM^", "LDM/STM lento", "condicional con S",
	"indefinida", "SWI Thumb", "Thumb lento", "otra",
};

static unsigned call_kind(uint32_t insn, int thumb)
{
	if (thumb) {
		if ((insn >> 8) == 0xDF)
			return AREC_CK_TH_SWI;
		if ((insn >> 12) == 0xC || (insn >> 12) == 0xB)
			return AREC_CK_TH_SLOW;
		return AREC_CK_OTHER;
	}
	if ((insn >> 28) == 0xF)
		return AREC_CK_UNDEF;
	if ((insn & 0x0F000000u) == 0x0F000000u)
		return AREC_CK_SWI;
	if ((insn & 0x0F000010u) == 0x0E000010u)
		return AREC_CK_CP15;
	if ((insn & 0x0FBF0FFFu) == 0x010F0000u)
		return AREC_CK_MRS;
	if ((insn & 0x0DB0F000u) == 0x0120F000u)
		return AREC_CK_MSR;
	if ((insn & 0x0E0000D0u) == 0x000000D0u)
		return AREC_CK_LDRD;
	if ((insn & 0x0FB00FF0u) == 0x01000090u)
		return AREC_CK_SWP;
	if ((insn & 0x0F900090u) == 0x01000080u || (insn & 0x0F900FF0u) == 0x01000050u)
		return AREC_CK_QDSP;
	if ((insn & 0x0E000000u) == 0x08000000u)
		return (insn & (1u << 22)) ? AREC_CK_BLOCK_S : AREC_CK_BLOCK_SLOW;
	if ((insn >> 28) != 0xE)
		return AREC_CK_FLAGCOND;
	return AREC_CK_OTHER;
}

/* ---- ayudantes que llama el código generado -------------------------------------------------------------- */

#ifdef AREC_CALLHIST
/* Depuración (-DAREC_CALLHIST): las instrucciones que más llama el intérprete desde un bloque, al salir. */
#include <stdio.h>
#define HIST_N 4096u
static struct { uint32_t insn, at; uint64_t n; } hist[HIST_N];
static void hist_dump(void)
{
	for (unsigned k = 0; k < 40; k++) {
		unsigned best = 0;

		for (unsigned i = 1; i < HIST_N; i++)
			if (hist[i].n > hist[best].n)
				best = i;
		if (!hist[best].n)
			break;
		fprintf(stderr, "CALLHIST %10llu  %08x en %08x\n", (unsigned long long)hist[best].n, hist[best].insn, hist[best].at);
		hist[best].n = 0;
	}
}
static void hist_add(uint32_t insn, uint32_t at)
{
	static int reg;
	unsigned h = ((insn * 2654435761u) ^ at) & (HIST_N - 1);

	if (!reg) {
		reg = 1;
		atexit(hist_dump);
	}
	for (unsigned k = 0; k < 64; k++, h = (h + 1) & (HIST_N - 1))
		if (!hist[h].n || (hist[h].insn == insn && hist[h].at == at)) {
			hist[h].insn = insn;
			hist[h].at = at;
			hist[h].n++;
			return;
		}
}
#endif

static int must_exit(struct acore *c, struct arec *j)
{
	if (j->exit_req)
		return 1;
	if (c->attn) {
		if (c->halted)
			return 1;
		if (c->fiq && !(c->cpsr & ACORE_F))
			return 1;
		if (c->irq && !(c->cpsr & ACORE_I))
			return 1;
	}
	return 0;
}

/* Devuelve el valor en los 32 bits bajos y, en el bit 32, si el bloque tiene que salir (una lectura de la E/S puede
 * atender un desborde de temporizador vencido y levantar una IRQ). */
static void prof_slow_add(uint32_t a, unsigned cpu, unsigned wr);
static int prof_slow_on;

static uint64_t h_rd(struct acore *c, uint32_t a, uint32_t kind)
{
	struct arec *j = c->jit;
	uint32_t v;

	j->pub.st.slow_rd++;
	if (prof_slow_on)
		prof_slow_add(a, c->v5 ? 0u : 1u, 0);
	j->exit_req = 0;
	switch (kind) {
	case MK_W:
		v = ld_word(c, a);
		break;
	case MK_H:
		v = ld_half(c, a);
		break;
	case MK_SH:
		v = ld_shalf(c, a);
		break;
	case MK_B:
		v = mem_rd8(c, a);
		break;
	default:
		v = (uint32_t)(int32_t)(int8_t)mem_rd8(c, a);
		break;
	}
	c->abort = 0;
	if (must_exit(c, j)) {
		j->pub.st.mem_exits++;
		return (uint64_t)1 << 32 | v;
	}
	return v;
}

static uint32_t h_wr(struct acore *c, uint32_t a, uint32_t v, uint32_t kind)
{
	struct arec *j = c->jit;

	j->pub.st.slow_wr++;
	if (prof_slow_on)
		prof_slow_add(a, c->v5 ? 0u : 1u, 1);
	j->exit_req = 0;
	switch (kind) {
	case MK_W:
		mem_wr32(c, a & ~3u, v);
		break;
	case MK_H:
		mem_wr16(c, a & ~1u, v);
		break;
	default:
		mem_wr8(c, a, v);
		break;
	}
	c->abort = 0;
	if (must_exit(c, j)) {
		j->pub.st.mem_exits++;
		return 1;
	}
	return 0;
}

/* El intérprete ejecuta `insn` (ARM o Thumb, la dirección en `at` con el bit 0 = Thumb). Distinto de 0: el bloque
 * tiene que salir con el estado en memoria (saltó, cambió de estado, hay una interrupción que tomar, se invalidó
 * código o, en modo estricto, lo que queda del bloque ya no cabe: `rem_max` ciclos). */
static uint32_t h_call(struct acore *c, uint32_t insn, uint32_t at, uint32_t rem_max)
{
	struct arec *j = c->jit;
	const uint32_t addr = at & ~1u;
	const int thumb = at & 1;
	const uint32_t r15 = addr + (thumb ? 4u : 8u);

	j->pub.st.callouts++;
	j->pub.st.call_by[call_kind(insn, thumb)]++;
#ifdef AREC_CALLHIST
	hist_add(insn, at);
#endif
	j->exit_req = 0;
	c->cpsr = (c->cpsr & 0x0FFFFFFFu & ~ACORE_T) | (c->jit_nzcv & 0xF0000000u) | (thumb ? ACORE_T : 0u);
	c->r[15] = r15;
	if (thumb)
		acore_exec_thumb(c, insn);
	else
		acore_exec_arm(c, insn);
	c->jit_nzcv = c->cpsr & 0xF0000000u;
	if (c->r[15] != r15 || (((c->cpsr & ACORE_T) != 0) != thumb))
		return 1;
	if (must_exit(c, j))
		return 1;
	if (rem_max != 0xFFFFu && c->cycles + rem_max > c->jit_until)
		return 1;
	return 0;
}

static uint64_t h_shift(uint32_t type, uint32_t v, uint32_t amt, uint32_t cin)
{
	uint32_t cout;
	const uint32_t r = shift_by_reg(type & 3, v, amt, cin & 1, &cout);

	return (uint64_t)cout << 32 | r;
}

/* Banderas desde el CR (camino frío): `cr` de mfcr y la ubicación de cada bandera (arec_emit.c, desc_of). */
static void h_flags(struct acore *c, uint32_t cr, uint32_t lo, uint32_t hi)
{
	struct arec *j = c->jit;
	uint32_t f = c->jit_nzcv;

	j->pub.st.cold_flags++;
	for (unsigned i = 0; i < 4; i++) {
		const uint32_t d = (i < 2 ? lo >> (16 * i) : hi >> (16 * (i - 2))) & 0xFFFFu;
		const unsigned t = d & 7, b = (d >> 3) & 31, b2 = (d >> 8) & 31, inv = (d >> 13) & 1;
		unsigned v;

		switch (t) {
		case 1: /* L_K0 */
			v = 0;
			break;
		case 2: /* L_K1 */
			v = 1;
			break;
		case 3: /* L_CR */
			v = ((cr >> (31 - b)) & 1u) ^ inv;
			break;
		case 4: /* L_XOR */
			v = ((cr >> (31 - b)) & 1u) ^ ((cr >> (31 - b2)) & 1u) ^ inv;
			break;
		default:
			continue;
		}
		f = (f & ~(1u << (31 - i))) | (v << (31 - i));
	}
	c->jit_nzcv = f & 0xF0000000u;
}

/* ---- plataforma ------------------------------------------------------------------------------------------- */

static uint64_t fn_entry(void (*fn)(void))
{
#if defined(__powerpc64__) && (!defined(_CALL_ELF) || _CALL_ELF == 1)
	return *(const uint64_t *)(const void *)fn; /* ELFv1: el puntero es el descriptor */
#else
	return (uint64_t)(uintptr_t)fn;
#endif
}

static void icache(struct arec *j, const void *p, size_t n)
{
	if (j->cfg.icache_flush)
		j->cfg.icache_flush(p, n);
	else
		__builtin___clear_cache((char *)(uintptr_t)p, (char *)(uintptr_t)p + n);
}

typedef uint64_t (*enter_fn)(struct acore *c, const uint32_t *code);

static uint64_t enter(struct arec *j, const uint32_t *code)
{
#if AREC_CAN_EXEC
	uint64_t toc;

	__asm__ volatile("mr %0,2" : "=r"(toc));
	j->enter_opd[0] = (uint64_t)(uintptr_t)(j->code + j->ent_enter);
	j->enter_opd[1] = toc;
	j->enter_opd[2] = 0;
	__asm__ volatile("" : : "r"(j->enter_opd) : "memory");
	return ((enter_fn)(void *)j->enter_opd)(j->c, code);
#else
	(void)j;
	(void)code;
	return 0;
#endif
}

/* ---- perfil del host (arec_prof_open) ------------------------------------------------------------------- */

#define PROF_CAP (1u << 21)     /* palabras del búfer de la traza */
#define PROF_ROOM 70000u        /* lo que puede escribir una entrada (un tramo de hasta 60 000 ciclos) */
static FILE *prof_tr, *prof_bk;
static uint32_t *prof_buf;
static uint32_t prof_pos;

/* accesos lentos por dirección (palabra alineada) y tipo, para ver de dónde vienen */
#define PROF_SLOW 8192u
static struct { uint32_t key; uint32_t cpu_wr; uint64_t n; } prof_slow[PROF_SLOW];

static void prof_slow_add(uint32_t a, unsigned cpu, unsigned wr)
{
	const uint32_t key = (a & ~3u) | 1u; /* 0 = libre */
	uint32_t h = ((key >> 2) * 2654435761u) >> 19;

	for (unsigned k = 0; k < PROF_SLOW; k++, h = (h + 1) & (PROF_SLOW - 1)) {
		if (prof_slow[h].key == key && prof_slow[h].cpu_wr == (cpu << 1 | wr)) {
			prof_slow[h].n++;
			return;
		}
		if (!prof_slow[h].key) {
			prof_slow[h].key = key;
			prof_slow[h].cpu_wr = cpu << 1 | wr;
			prof_slow[h].n = 1;
			return;
		}
	}
}

static void prof_drain(void)
{
	if (prof_pos && prof_tr)
		fwrite(prof_buf, 4, prof_pos, prof_tr);
	prof_pos = 0;
}

static void prof_put(uint32_t a, uint32_t b)
{
	if (prof_pos + 3 > PROF_CAP)
		prof_drain();
	prof_buf[prof_pos++] = AREC_PROF_ESC;
	prof_buf[prof_pos++] = a;
	prof_buf[prof_pos++] = b;
}

int arec_prof_open(const char *prefix)
{
	char path[512];

	prof_buf = malloc(PROF_CAP * 4u + 64u);
	snprintf(path, sizeof path, "%s.tr", prefix);
	prof_tr = fopen(path, "wb");
	snprintf(path, sizeof path, "%s.blk", prefix);
	prof_bk = fopen(path, "w");
	if (!prof_buf || !prof_tr || !prof_bk)
		return -1;
	arec_prof_on = 1;
	return 0;
}

void arec_prof_frame(uint32_t frame, uint32_t icount9, uint32_t icount7)
{
	if (!arec_prof_on)
		return;
	{
		/* AREC_PROF_SLOW_FROM: los accesos lentos se cuentan desde ese cuadro (la escena que interesa) */
		static long from = -1;

		if (from < 0) {
			const char *e = getenv("AREC_PROF_SLOW_FROM");

			from = e ? atol(e) : 0;
		}
		prof_slow_on = (long)frame + 1 >= from;
	}
	prof_put(0xF0000000u | (frame & 0xFFFFFFu), icount9);
	prof_put(0xF1000000u, icount7);
}

void arec_prof_close(void)
{
	if (!arec_prof_on)
		return;
	prof_drain();
	fclose(prof_tr);
	for (unsigned k = 0; k < PROF_SLOW; k++)
		if (prof_slow[k].key)
			fprintf(prof_bk, "S %u %u %08x %llu\n", prof_slow[k].cpu_wr >> 1, prof_slow[k].cpu_wr & 1, prof_slow[k].key & ~3u,
				(unsigned long long)prof_slow[k].n);
	fclose(prof_bk);
	arec_prof_on = 0;
}

/* Describe un bloque recién traducido: cpu vaciado entrada pc instrucciones caliente total perfil, y por instrucción
 * palabra:thumb|tipo<<1:condición:palabras calientes. */
static void prof_block(struct arec *j, const struct ablock *b, const struct ai *ins, unsigned n)
{
	fprintf(prof_bk, "B %u %u %u %08x %u %u %u %u %u", j->c->v5 ? 0u : 1u, j->prof_gen & 0xFFu, (unsigned)(b - j->blocks), b->pc,
		n, b->hot, b->words, arec_prof_words, b->entry);
	for (unsigned i = 0; i < n; i++)
		fprintf(prof_bk, " %x:%x:%x:%u", ins[i].raw, ins[i].thumb | ins[i].kind << 1, ins[i].cond,
			(unsigned)(arec_prof_ipos[i + 1] - arec_prof_ipos[i]));
	fprintf(prof_bk, "\n");
}

/* ---- tabla de bloques ------------------------------------------------------------------------------------ */

static unsigned hslot(uint32_t pc)
{
	return (pc >> 1) & (AREC_HASH - 1);
}

static struct ablock *lookup(struct arec *j, uint32_t pc)
{
	for (uint32_t id = j->hash[hslot(pc)]; id != AREC_NO_BLOCK; id = j->blocks[id].hnext)
		if (j->blocks[id].pc == pc)
			return &j->blocks[id];
	return NULL;
}

static void wr_page_update(struct arec *j, uint32_t ph16)
{
	const int has_code = j->phys_blocks[ph16] != 0;
	uint8_t **wr = j->pt + AREC_PAGES;

	for (uint32_t k = 0; k < j->phys_cnt[ph16]; k++) {
		const uint32_t g = j->phys_pages[j->phys_first[ph16] + k];

		wr[g] = has_code ? NULL : j->wr_real[g];
	}
}

static void reset_tables(struct arec *j)
{
	for (unsigned i = 0; i < AREC_HASH; i++)
		j->hash[i] = AREC_NO_BLOCK;
	for (unsigned i = 0; i < AREC_JL_SIZE; i++) {
		j->jl[i].key = 1;
		j->jl[i].code = 0;
	}
	for (uint32_t i = 0; i < j->nbuckets; i++)
		j->bucket_head[i] = AREC_NO_BLOCK;
	memset(j->pub.codemap, 0, j->cfg.phys_size >> AREC_LINE_SHIFT);
	memset(j->phys_blocks, 0, j->nphys16 * sizeof *j->phys_blocks);
	j->nblocks = 0;
	j->nlinks = 0;
	arec_set_window(j, (uint32_t)j->code_base_used);
}

void arec_flush(struct arec *j)
{
	reset_tables(j);
	/* la tabla de escritura vuelve a ser la real (ya no hay código) */
	memcpy(j->pt + AREC_PAGES, j->wr_real, j->npages * sizeof *j->pt);
	j->exit_req = 1;
	j->pub.st.flushes++;
	j->prof_gen++;
}

static void unlink_in(struct arec *j, struct ablock *b)
{
	for (uint32_t k = b->in_links; k != AREC_NO_BLOCK; k = j->links[k].next) {
		const struct alink *l = &j->links[k];

		j->code[l->site] = ppc_b((int32_t)(l->stub - l->site) * 4);
		icache(j, j->code + l->site, 4);
	}
	b->in_links = AREC_NO_BLOCK;
}

static void bucket_remove(struct arec *j, uint32_t id, unsigned which)
{
	struct ablock *b = &j->blocks[id];
	uint32_t *pp = &j->bucket_head[b->bucket[which]];

	while (*pp != AREC_NO_BLOCK) {
		struct ablock *x = &j->blocks[*pp];
		const unsigned w = x->bucket[0] == b->bucket[which] ? 0u : 1u;

		if (*pp == id) {
			*pp = x->bnext[w];
			return;
		}
		pp = &x->bnext[w];
	}
}

static void invalidate(struct arec *j, uint32_t id)
{
	struct ablock *b = &j->blocks[id];

	if (b->dead)
		return;
	b->dead = 1;
	/* tabla de bloques */
	for (uint32_t *pp = &j->hash[hslot(b->pc)]; *pp != AREC_NO_BLOCK; pp = &j->blocks[*pp].hnext)
		if (*pp == id) {
			*pp = b->hnext;
			break;
		}
	{
		struct ajl *e = &j->jl[(b->pc >> 1) & (AREC_JL_SIZE - 1)];

		if (e->key == b->pc && e->code == (uint64_t)(uintptr_t)(j->code + b->entry))
			e->key = 1;
	}
	unlink_in(j, b);
	for (unsigned w = 0; w < b->nbuckets; w++)
		bucket_remove(j, id, w);
	if (b->phys_lo != AREC_PHYS_NONE) {
		for (uint32_t p = b->phys_lo >> 14; p <= (b->phys_hi - 1) >> 14; p++)
			if (j->phys_blocks[p] && --j->phys_blocks[p] == 0)
				wr_page_update(j, p);
	}
	j->exit_req = 1;
	j->pub.st.inval_blocks++;
}

void arec_code_write_slow(struct arec *j, uint32_t phys, uint32_t size)
{
	const uint32_t end = phys + size, line = phys >> AREC_LINE_SHIFT;
	const uint32_t bk = phys >> AREC_BUCKET_SHIFT;
	int still = 0;

	if (phys >= j->cfg.phys_size)
		return;
	j->pub.st.code_wr++;
	for (uint32_t id = j->bucket_head[bk]; id != AREC_NO_BLOCK;) {
		struct ablock *b = &j->blocks[id];
		const unsigned w = b->bucket[0] == bk ? 0u : 1u;
		const uint32_t next = b->bnext[w];

		if (!b->dead && phys < b->phys_hi && end > b->phys_lo)
			invalidate(j, id);
		id = next;
	}
	/* ¿queda código en la línea? */
	for (uint32_t id = j->bucket_head[bk]; id != AREC_NO_BLOCK;) {
		const struct ablock *b = &j->blocks[id];
		const unsigned w = b->bucket[0] == bk ? 0u : 1u;

		if (!b->dead && (b->phys_lo >> AREC_LINE_SHIFT) <= line && ((b->phys_hi - 1) >> AREC_LINE_SHIFT) >= line)
			still = 1;
		id = b->bnext[w];
	}
	if (!still)
		j->pub.codemap[line] = 0;
}

/* ---- traducción ------------------------------------------------------------------------------------------ */

static struct ablock *compile(struct arec *j, uint32_t pc)
{
	static struct ai ins[AREC_MAX_INSN];
	const struct arec_src src = { j->cfg.code_ptr, j->cfg.ctx, j->c->v5 != 0 };
	uint32_t plo, phi;
	unsigned n;
	struct ablock *b;
	uint32_t id;
	int r, tries = 0;
	uint64_t t0 = j->pub.clock_ns ? j->pub.clock_ns() : 0;

	n = arec_decode(&src, pc, ins, AREC_MAX_INSN, &plo, &phi);
	if (!n)
		return NULL;
again:
	if (j->nblocks >= j->max_blocks || j->nlinks + AREC_MAX_EXITS >= j->max_links ||
	    j->code_used + 16384 > j->code_words)
		arec_flush(j);
	id = j->nblocks;
	b = &j->blocks[id];
	memset(b, 0, sizeof *b);
	b->pc = pc;
	b->phys_lo = plo;
	b->phys_hi = phi;
	b->in_links = AREC_NO_BLOCK;
	r = arec_emit_block(j, ins, n, b, id);
	if (r == -2 && !tries++) {
		arec_flush(j);
		goto again;
	}
	if (r < 0) {
		if (r == -6) {
			j->pub.st.bad_blocks++;
#ifdef AREC_DEBUG
			{
				extern uint32_t arec_bad_raw, arec_bad_addr;

				fprintf(stderr, "arec: análisis de banderas en %08x (bloque %08x): %08x\n", arec_bad_addr, pc, arec_bad_raw);
			}
#endif
		}
		return NULL;
	}
	if (b->phys_lo != AREC_PHYS_NONE) {
		plo = b->phys_lo; /* (la vista adelante de banderas puede ampliarlo) */
		phi = b->phys_hi;
	}
	j->nblocks++;
	b->ninsn = (uint16_t)n;
	icache(j, j->code + b->entry, (size_t)b->hot * 4);
	if (b->words > b->hot)
		icache(j, j->code + b->cold, (size_t)(b->words - b->hot) * 4);
	/* tablas */
	b->hnext = j->hash[hslot(pc)];
	j->hash[hslot(pc)] = id;
	{
		struct ajl *e = &j->jl[(pc >> 1) & (AREC_JL_SIZE - 1)];

		e->code = (uint64_t)(uintptr_t)(j->code + b->entry);
		e->key = pc;
	}
	if (plo != AREC_PHYS_NONE && phi > plo) {
		const uint32_t b0 = plo >> AREC_BUCKET_SHIFT, b1 = (phi - 1) >> AREC_BUCKET_SHIFT;

		b->nbuckets = 0;
		for (uint32_t k = b0; k <= b1 && b->nbuckets < 2; k++) {
			b->bucket[b->nbuckets] = k;
			b->bnext[b->nbuckets] = j->bucket_head[k];
			j->bucket_head[k] = id;
			b->nbuckets++;
		}
		for (uint32_t l = plo >> AREC_LINE_SHIFT; l <= (phi - 1) >> AREC_LINE_SHIFT; l++)
			j->pub.codemap[l] = 1;
		for (uint32_t p = plo >> 14; p <= (phi - 1) >> 14; p++)
			if (j->phys_blocks[p]++ == 0)
				wr_page_update(j, p);
	}
#ifdef AREC_DEBUG
	{
		const char *d = getenv("AREC_DUMP");

		if (d && (*d == 0x2A || strtoul(d, NULL, 16) == (pc & ~1u))) {
			fprintf(stderr, "bloque %08x: %u instr, %u palabras en %u (fría en %u)\n", pc, n, b->words, b->entry, b->cold);
			for (uint32_t k = 0; k < b->words; k++)
				fprintf(stderr, "%08x%c", j->code[(k < b->hot ? b->entry : b->cold - b->hot) + k], (k & 7) == 7 ? '\n' : ' ');
			fprintf(stderr, "\n");
		}
	}
#endif
	if (arec_prof_on)
		prof_block(j, b, ins, n);
	j->pub.st.blocks_made++;
	j->pub.st.guest_made += n;
	j->pub.st.host_words += (uint64_t)b->words;
	j->pub.st.host_hot += (uint64_t)b->hot;
	if (j->pub.clock_ns)
		j->pub.st.compile_ns += j->pub.clock_ns() - t0;
	return b;
}

static void link(struct arec *j, struct ablock *from, unsigned k, struct ablock *to)
{
	struct ablock_exit *x = &from->exits[k];
	uint32_t li;

	if (j->nlinks >= j->max_links)
		return;
	li = j->nlinks++;
	j->links[li].site = x->site;
	j->links[li].stub = x->stub;
	j->links[li].next = to->in_links;
	to->in_links = li;
	j->code[x->site] = ppc_b((int32_t)(to->entry - x->site) * 4);
	icache(j, j->code + x->site, 4);
	j->pub.st.links++;
}

/* ---- despachador ------------------------------------------------------------------------------------------- */

static void set_pc(struct acore *c, uint32_t pc)
{
	if (pc & 1) {
		c->cpsr |= ACORE_T;
		c->r[15] = (pc & ~1u) + 2;
	} else {
		c->cpsr &= ~ACORE_T;
		c->r[15] = (pc & ~3u) + 4;
	}
}

void arec_run(struct arec *j, uint64_t until)
{
	struct acore *c = j->c;
	void (*const ev)(void *, int, uint64_t) = j->cfg.on_exec;

	int jl_fill = 0;

	if (arec_prof_on)
		prof_put(0xA0000000u | (c->v5 ? 0u : 1u), (uint32_t)c->icount);
	while (c->cycles < until) {
		struct ablock *b;
		uint32_t pc;
		uint64_t r, lim;

		if (c->attn) {
			const int s = acore_service(c, until);

			if (ev)
				ev(j->cfg.ctx, AREC_EV_SERVICE, until);
			if (s)
				continue;
		}
		pc = acore_pc(c) | ((c->cpsr & ACORE_T) ? 1u : 0u);
		b = lookup(j, pc);
		if (!b)
			b = compile(j, pc);
		if (!b || (j->cfg.strict && c->cycles + b->maxcyc > until)) {
			if (!b)
				j->pub.st.no_code++;
			j->pub.st.interp_steps++;
			acore_step_insn(c);
			if (ev)
				ev(j->cfg.ctx, AREC_EV_STEP, until);
			continue;
		}
		if (jl_fill) {
			/* se llegó por un destino indirecto que no estaba en la tabla (otro bloque con el mismo índice, o se
			 * borró): el bloque vuelve a la tabla para que el próximo salto no pase por aquí */
			struct ajl *e = &j->jl[(pc >> 1) & (AREC_JL_SIZE - 1)];

			e->code = (uint64_t)(uintptr_t)(j->code + b->entry);
			e->key = pc;
			jl_fill = 0;
		}
		lim = until;
		if (lim - c->cycles > 60000u)
			lim = c->cycles + 60000u; /* las instrucciones se cuentan en 16 bits de r29 */
		c->jit_until = lim;
		c->jit_nzcv = c->cpsr & 0xF0000000u;
		j->exit_req = 0;
		j->pub.st.enters++;
		j->pub.last_pc = pc;
		j->pub.last_code = (uint64_t)(uintptr_t)(j->code + b->entry);
		if (arec_prof_on) {
			if (prof_pos + PROF_ROOM > PROF_CAP)
				prof_drain();
			c->jit_prof = (uint64_t)(uintptr_t)(prof_buf + prof_pos);
		}
		r = enter(j, j->code + b->entry);
		if (arec_prof_on)
			prof_pos = (uint32_t)((uint32_t *)(uintptr_t)c->jit_prof - prof_buf);
		c->cpsr = (c->cpsr & 0x0FFFFFFFu) | (c->jit_nzcv & 0xF0000000u);
#ifdef AREC_DEBUG
		if (dbg_left < 0) {
			const char *e = getenv("AREC_TRACE");
			dbg_left = e ? atoi(e) : 0;
		}
		if (dbg_left > 0) {
			dbg_left--;
			fprintf(stderr, "arec: pc %08x -> r=%016llx cyc=%llu until=%llu ic=%llu\n", pc, (unsigned long long)r,
				(unsigned long long)c->cycles, (unsigned long long)until, (unsigned long long)c->icount);
		}
#endif
		j->last_live = LIVE_FLAGS(r);
		switch ((r >> 56) & 3u) {
		case 0:
			set_pc(c, (uint32_t)r);
			if ((r >> 59) & 1u) {
				j->pub.st.exits_budget++;
			} else {
				j->pub.st.exits_pc++;
				jl_fill = 1;
#ifdef AREC_CALLHIST
				hist_add(0xEEEEEEEEu, (uint32_t)r);
#endif
			}
			break;
		case 1: {
			const uint32_t id = (uint32_t)(r >> 8) & 0xFFFFFFu, k = (uint32_t)r & 0xFFu;
			struct ablock *from;
			uint32_t target;

			j->pub.st.exits_link++;
			if (id >= j->nblocks) { /* no puede pasar */
				j->pub.st.exits_state++;
				break;
			}
			from = &j->blocks[id];
			target = from->exits[k].target;
			set_pc(c, target);
			if (!j->cfg.no_link && !from->dead) {
				const uint64_t fl = j->pub.st.flushes;
				struct ablock *to = lookup(j, target);

				if (!to)
					to = compile(j, target);
				if (to && j->pub.st.flushes == fl && !from->dead)
					link(j, from, k, to);
			}
			break;
		}
		default:
			j->pub.st.exits_state++;
			break;
		}
		if (ev)
			ev(j->cfg.ctx, AREC_EV_BLOCK, until);
	}
}

/* ---- mapa de memoria -------------------------------------------------------------------------------------- */

void arec_map_begin(struct arec *j)
{
	memset(j->pt, 0, 2u * AREC_PAGES * sizeof *j->pt);
	memset(j->wr_real, 0, j->npages * sizeof *j->wr_real);
	for (uint32_t i = 0; i < j->npages; i++)
		j->wr_phys[i] = AREC_PHYS_NONE;
}

void arec_map_set(struct arec *j, uint32_t page, uint8_t *rd, uint8_t *wr, uint32_t wr_phys)
{
	if (page >= j->npages)
		return; /* fuera de lo que el dueño dijo que mapea (cfg.map_pages): ruta lenta */
	j->pt[page] = rd;
	j->wr_real[page] = wr;
	j->wr_phys[page] = (wr && wr_phys != AREC_PHYS_NONE && wr_phys < j->cfg.phys_size) ? wr_phys >> 14 : AREC_PHYS_NONE;
}

void arec_map_end(struct arec *j)
{
	uint8_t **wr = j->pt + AREC_PAGES;
	uint32_t pos = 0;

	memset(j->phys_cnt, 0, j->nphys16 * sizeof *j->phys_cnt);
	for (uint32_t g = 0; g < j->npages; g++)
		if (j->wr_phys[g] != AREC_PHYS_NONE)
			j->phys_cnt[j->wr_phys[g]]++;
	for (uint32_t p = 0; p < j->nphys16; p++) {
		j->phys_first[p] = pos;
		pos += j->phys_cnt[p];
		j->phys_cnt[p] = 0;
	}
	for (uint32_t g = 0; g < j->npages; g++) {
		const uint32_t p = j->wr_phys[g];

		if (p != AREC_PHYS_NONE && j->phys_first[p] + j->phys_cnt[p] < j->npages)
			j->phys_pages[j->phys_first[p] + j->phys_cnt[p]++] = g;
		wr[g] = (p != AREC_PHYS_NONE && j->phys_blocks[p]) ? NULL : j->wr_real[g];
	}
}

/* ---- creación ------------------------------------------------------------------------------------------------ */

static struct arec *arec_new(struct acore *c, const struct arec_cfg *cfg, int can_exec)
{
	struct arec *j;

	if (!cfg->code || cfg->code_bytes < 65536 || !cfg->code_ptr || !cfg->phys_size)
		return NULL;
	j = calloc(1, sizeof *j);
	if (!j)
		return NULL;
	j->c = c;
	j->cfg = *cfg;
	j->can_exec = can_exec;
	j->code = cfg->code;
	j->code_words = cfg->code_bytes / 4;
	/* Las tablas crecen con la caché (4 MB: 16384 bloques; desde 8 MB, el doble), para que con la caché grande de
	 * PS3MAPI no se vacíe por bloques antes que por código (NSMB: ~540 bytes de código por bloque). Cada bloque son
	 * ~250 bytes del heap: 32768 son 8 MB. */
	{
		const uint32_t k = cfg->code_bytes >= (6u << 20) ? 2u : 1u;

		j->max_blocks = MAX_BLOCKS * k;
		j->max_links = MAX_LINKS * k;
		if (cfg->max_blocks) { /* el ARM7: poco código, tablas chicas */
			j->max_blocks = cfg->max_blocks;
			j->max_links = cfg->max_blocks * 4u;
		}
	}
	j->npages = cfg->map_pages && cfg->map_pages < AREC_PAGES ? cfg->map_pages : AREC_PAGES;
	j->blocks = calloc(j->max_blocks, sizeof *j->blocks);
	j->links = calloc(j->max_links, sizeof *j->links);
	j->jl = calloc(AREC_JL_SIZE, sizeof *j->jl);
	j->pt = calloc(2u * AREC_PAGES, sizeof *j->pt);
	j->wr_real = calloc(j->npages, sizeof *j->wr_real);
	j->wr_phys = calloc(j->npages, sizeof *j->wr_phys);
	j->nphys16 = (cfg->phys_size + 0x3FFFu) >> 14;
	j->phys_blocks = calloc(j->nphys16, sizeof *j->phys_blocks);
	j->phys_first = calloc(j->nphys16, sizeof *j->phys_first);
	j->phys_cnt = calloc(j->nphys16, sizeof *j->phys_cnt);
	j->phys_pages = calloc(j->npages, sizeof *j->phys_pages);
	j->nbuckets = (cfg->phys_size + (1u << AREC_BUCKET_SHIFT) - 1) >> AREC_BUCKET_SHIFT;
	j->bucket_head = calloc(j->nbuckets, sizeof *j->bucket_head);
	j->pub.codemap = calloc((cfg->phys_size >> AREC_LINE_SHIFT) + 1, 1);
	if (!j->blocks || !j->links || !j->jl || !j->pt || !j->wr_real || !j->wr_phys || !j->phys_blocks || !j->phys_first ||
	    !j->phys_cnt || !j->phys_pages || !j->bucket_head || !j->pub.codemap) {
		arec_destroy(j);
		return NULL;
	}
	c->jit = j;
	c->jit_pt = (uint64_t)(uintptr_t)j->pt;
	c->jit_jl = (uint64_t)(uintptr_t)j->jl;
	c->jit_fn[AH_RD] = fn_entry((void (*)(void))h_rd);
	c->jit_fn[AH_WR] = fn_entry((void (*)(void))h_wr);
	c->jit_fn[AH_CALL] = fn_entry((void (*)(void))h_call);
	c->jit_fn[AH_SHIFT] = fn_entry((void (*)(void))h_shift);
	c->jit_fn[AH_FLAGS] = fn_entry((void (*)(void))h_flags);
	arec_map_begin(j);
	arec_map_end(j);
	arec_emit_common(j);
	arec_dbg_first[0] = ((volatile uint32_t *)j->code)[0];
	icache(j, j->code, j->code_base_used * 4);
	arec_dbg_first[1] = ((volatile uint32_t *)j->code)[0];
	reset_tables(j);
	arec_dbg_first[2] = ((volatile uint32_t *)j->code)[0];
	return j;
}

struct arec *arec_create(struct acore *c, const struct arec_cfg *cfg)
{
	if (!AREC_CAN_EXEC)
		return NULL;
	return arec_new(c, cfg, 1);
}

uint32_t arec_dbg_first[3];

void arec_debug_layout(const struct arec *j, char *buf, size_t size)
{
	snprintf(buf, size, "código %p (%u KB), bloques %p (%u KB), enlaces %p, jl %p, pt %p, hash %p, codemap %p, j %p",
		 (const void *)j->code, (unsigned)(j->code_words / 256), (const void *)j->blocks,
		 (unsigned)(j->max_blocks * sizeof *j->blocks / 1024), (const void *)j->links, (const void *)j->jl,
		 (const void *)j->pt, (const void *)j->hash, (const void *)j->pub.codemap, (const void *)j);
}

void arec_destroy(struct arec *j)
{
	if (!j)
		return;
	if (j->c && j->c->jit == j)
		j->c->jit = NULL;
	free(j->blocks);
	free(j->links);
	free(j->jl);
	free(j->pt);
	free(j->wr_real);
	free(j->wr_phys);
	free(j->phys_blocks);
	free(j->phys_first);
	free(j->phys_cnt);
	free(j->phys_pages);
	free(j->bucket_head);
	free(j->pub.codemap);
	free(j);
}

unsigned arec_last_live(const struct arec *j)
{
	return j->last_live;
}

void arec_usage(const struct arec *j, uint32_t *blocks, size_t *code_used, size_t *code_cap)
{
	*blocks = j->nblocks;
	*code_used = j->code_used * 4;
	*code_cap = j->code_words * 4;
}

int arec_translate_only(struct acore *c, const struct arec_cfg *cfg, uint32_t pc, uint32_t *out, size_t cap)
{
	void *const old = c->jit;
	struct arec *j = arec_new(c, cfg, 0);
	struct ablock *b;
	int n = -1;

	if (!j)
		return -1;
	b = compile(j, pc);
	if (b && b->words <= cap) {
		memcpy(out, j->code + b->entry, (size_t)b->hot * 4);
		memcpy(out + b->hot, j->code + b->cold, (size_t)(b->words - b->hot) * 4);
		n = (int)b->words;
	}
	arec_destroy(j);
	c->jit = old;
	return n;
}
