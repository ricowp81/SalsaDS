/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * SalsaDS — los recompiladores del ARM9 y del ARM7 (src/jit/arec*) conectados a la máquina de la DS (Fase 4).
 *
 * Lo propio de la DS que el recompilador no sabe: de dónde sale el código (ITCM, RAM principal, WRAM compartida y la
 * BIOS en HLE; nada de VRAM ni E/S: eso lo ejecuta el intérprete), la tabla de páginas de la ruta rápida (RAM y WRAM
 * por las regiones del mapa, encima la DTCM y la ITCM como las ve la CPU según CP15, con su modo de carga) y los
 * avisos de escritura a memoria con código (bus, ITCM desde el núcleo, captura de sonido). El espacio físico para
 * invalidar es el tramo de `struct nds` que va de `itcm` al final de `swram` (ITCM, DTCM, RAM y WRAM seguidas).
 *
 * El ARM7 (ARMv4T) tiene su propio recompilador: otra caché de código (la cola de la memoria que se da), otras tablas
 * de páginas (su mapa: RAM principal, WRAM compartida y WRAM propia, sin TCM) y su vista de captación (esas memorias y la
 * BIOS del ARM7 en HLE). Los dos comparten el espacio físico para invalidar —que para el ARM7 se extiende hasta el final de
 * `wram7`— y toda escritura a memoria avisa a los dos (NDS_CODE_WRITE).
 *
 * Código propio (GPL-2.0-or-later).
 */
#include <string.h>

#include "core/nds_priv.h"
#include "jit/arec.h"

/* El espacio físico para invalidar: del primer byte de `itcm` hasta el final de `swram` (ARM9) o de `wram7` (ARM7). */
static uint32_t phys_size9(const struct nds *n)
{
	return (uint32_t)((const uint8_t *)n->swram + NDS_SWRAM_SIZE - (const uint8_t *)n->itcm);
}

static uint32_t phys_size7(const struct nds *n)
{
	return (uint32_t)((const uint8_t *)n->wram7 + NDS_WRAM7_SIZE - (const uint8_t *)n->itcm);
}

static uint32_t phys_of(const struct nds *n, const uint8_t *p, uint32_t size)
{
	const uintptr_t off = (uintptr_t)p - (uintptr_t)n->itcm;

	if (!p || off >= size)
		return AREC_PHYS_NONE;
	return (uint32_t)off;
}

/* Vista de captación del ARM9 (como mem_fetch32: ITCM y luego el bus). */
static const uint8_t *code_ptr(void *ctx, uint32_t a, uint32_t *phys, uint32_t *avail)
{
	struct nds *n = ctx;
	const struct acore *c = &n->a9;
	const struct nds_rgn *r;

	if ((uint64_t)a < c->itcm_rd_len) {
		const uint32_t off = a & (ACORE_ITCM_SIZE - 1);
		uint64_t av = ACORE_ITCM_SIZE - off;

		if (av > c->itcm_rd_len - a)
			av = c->itcm_rd_len - a;
		*phys = off;
		*avail = (uint32_t)av;
		return n->itcm + off;
	}
	r = &n->rgn_rd[NDS_CPU9][a >> 23];
	if (r->p) {
		const uint8_t *p = r->p + (a & r->mask);
		const uint32_t ph = phys_of(n, p, phys_size9(n));

		if (ph == AREC_PHYS_NONE)
			return NULL; /* paleta, OAM: no se ejecuta de ahí */
		*phys = ph;
		*avail = r->mask + 1 - (a & r->mask);
		return p;
	}
	if ((a & 0xFFFFF000u) == 0xFFFF0000u) {
		*phys = AREC_PHYS_NONE;
		*avail = 0x1000u - (a & 0xFFFu);
		return n->bios9 + (a & 0xFFFu);
	}
	return NULL;
}

/* Vista de captación del ARM7: RAM principal, WRAM compartida y WRAM propia por las regiones del mapa, y la BIOS (HLE). */
static const uint8_t *code_ptr7(void *ctx, uint32_t a, uint32_t *phys, uint32_t *avail)
{
	struct nds *n = ctx;
	const struct nds_rgn *r = &n->rgn_rd[NDS_CPU7][a >> 23];

	if (r->p) {
		const uint8_t *p = r->p + (a & r->mask);
		const uint32_t ph = phys_of(n, p, phys_size7(n));

		if (ph == AREC_PHYS_NONE)
			return NULL;
		*phys = ph;
		*avail = r->mask + 1 - (a & r->mask);
		return p;
	}
	if (a < NDS_BIOS7_SIZE) {
		*phys = AREC_PHYS_NONE;
		*avail = NDS_BIOS7_SIZE - a;
		return n->bios7 + a;
	}
	return NULL;
}

/* Lectura de diagnóstico por captación, sin tocar E/S ni la DTCM (no sirve para fetch). */
static uint32_t diag_word(struct nds *n, int cpu, uint32_t a)
{
	uint32_t value = 0;

	for (unsigned k = 0; k < 4; k++) {
		uint32_t ph, av;
		const uint32_t at = a + k;
		const uint8_t *p = cpu == NDS_CPU7 ? code_ptr7(n, at, &ph, &av) : code_ptr(n, at, &ph, &av);
		value |= (p ? *p : nds_bus_read(n, cpu, at, 1)) << (8 * k);
	}
	return value;
}

static void jit_nocode(void *ctx, const struct acore *c, const struct arec_nocode *d, uint32_t last, int error)
{
	struct nds *n = ctx;
	const int cpu = c->v5 ? NDS_CPU9 : NDS_CPU7;
	const uint32_t pc = acore_pc(c), region = pc >> 24;
	uint32_t ph = 0, av = 0;
	const uint8_t *p = c->v5 ? code_ptr(n, pc, &ph, &av) : code_ptr7(n, pc, &ph, &av);
	const char *name = "sin mapa", *why;

	if (c->v5 && (uint64_t)pc < c->itcm_rd_len) name = "ITCM";
	else if ((!c->v5 && pc < NDS_BIOS7_SIZE) || (c->v5 && (pc & 0xFFFFF000u) == 0xFFFF0000u)) name = "BIOS HLE";
	else if (region == 2) name = "RAM principal";
	else if (region == 3) name = !c->v5 && pc >= 0x03800000u ? "WRAM ARM7" : "WRAM compartida/espejo";
	else if (region == 4) name = pc >= 0x04800000u ? "Wi-Fi/E/S" : "E/S";
	else if (region == 5) name = "paleta";
	else if (region == 6) name = "VRAM";
	else if (region == 7) name = "OAM";
	else if (region >= 8 && region <= 10) name = "ranura GBA vacia";
	why = error ? "error del emisor" : !p ? (region == 6 ? "VRAM sin invalidacion de codigo" :
		"sin puntero de captacion traducible") : av < ((c->cpsr & ACORE_T) ? 2u : 4u) ?
		"instruccion cruza limite de memoria" : "captacion/decodificacion rechazada";
	nds_logf(n, "DATO jit-sin-codigo: ARM%u pasos=%u PC=%08x %s CPSR=%08x LR=%08x SP=%08x region=%s motivo=%s error=%d inicio=%08x ultimo-bloque=%08x",
		c->v5 ? 9u : 7u, d->steps, pc, (c->cpsr & ACORE_T) ? "Thumb" : "ARM", c->cpsr,
		c->r[14], c->r[13], name, why, error, d->first_pc, last);
	/* Nunca leer registros: FIFO, cartucho o Wi-Fi pueden tener efectos al leer. */
	for (unsigned k = 0; k < 8; k++) {
		const uint32_t a = pc + 4 * k;
		if ((a >> 24) == 4 || ((a + 3) >> 24) == 4)
			nds_logf(n, "DATO jit-palabra: %08x=???????? (E/S omitida)", a);
		else
			nds_logf(n, "DATO jit-palabra: %08x=%08x", a, diag_word(n, cpu, a));
	}
	nds_logf(n, "DATO jit-anillo: %u discontinuidades del episodio, antiguas primero; bit0=Thumb; no incluye saltos traducidos", d->count);
	for (unsigned k = 0; k < d->count; k++)
		nds_logf(n, "DATO jit-destino: %u %08x", k, d->targets[(d->next + 16 - d->count + k) & 15u]);
}

/* Tabla de páginas de la ruta rápida: lo que mapean las regiones de la CPU (RAM y WRAM; páginas de 16 KB con regiones de
 * al menos ese tamaño). `limit` es el espacio físico de ese recompilador. */
static void remap_bus(struct nds *n, struct arec *j, int cpu, uint32_t limit)
{
	arec_map_begin(j);
	for (uint32_t r = 0; r < 512; r++) {
		const struct nds_rgn *rr = &n->rgn_rd[cpu][r], *rw = &n->rgn_wr[cpu][r];
		const int ord = rr->p && rr->mask >= 0x3FFFu && phys_of(n, rr->p, limit) != AREC_PHYS_NONE;
		const int owr = rw->p && rw->mask >= 0x3FFFu && phys_of(n, rw->p, limit) != AREC_PHYS_NONE;

		if (!ord && !owr)
			continue;
		for (uint32_t pg = 0; pg < 512; pg++) {
			const uint32_t a = r << 23 | pg << 14;
			uint8_t *rd = ord ? rr->p + (a & rr->mask & ~0x3FFFu) : NULL;
			uint8_t *wr = owr ? rw->p + (a & rw->mask & ~0x3FFFu) : NULL;

			arec_map_set(j, a >> 14, rd, wr, phys_of(n, wr, limit));
		}
	}
}

/* ARM9: el bus, encima la DTCM y la ITCM como las ve la CPU según CP15 (en el orden del intérprete). */
static void remap(struct nds *n)
{
	struct arec *j = n->jit9;
	const struct acore *c = &n->a9;
	const uint32_t limit = phys_size9(n);

	remap_bus(n, j, NDS_CPU9, limit);
	/* DTCM: solo si la región está alineada a 16 KB y la cubre entera (si no, la ruta lenta) */
	{
		const uint64_t lr = c->dtcm_rd_len, lw = c->dtcm_wr_len;
		const uint32_t base = c->dtcm_base;

		if (!(base & 0x3FFFu)) {
			for (uint64_t o = 0; o < (lr > lw ? lr : lw); o += 0x4000u) {
				const uint32_t a = base + (uint32_t)o;
				uint8_t **const pt = (uint8_t **)(uintptr_t)c->jit_pt;
				uint8_t *rd = pt[a >> 14];
				uint8_t *wr = NULL;
				uint32_t wph = AREC_PHYS_NONE;

				(void)wr;
				if (o + 0x4000u <= lr)
					rd = n->dtcm;
				else if (o < lr)
					rd = NULL;
				/* la tabla de escritura real se rehace entera: aquí se pone lo de la DTCM */
				if (o + 0x4000u <= lw) {
					wr = n->dtcm;
					wph = phys_of(n, n->dtcm, limit);
				} else if (o < lw) {
					wr = NULL;
				} else {
					continue; /* fuera de la región de escritura: lo que haya del bus (no se toca) */
				}
				arec_map_set(j, a >> 14, rd, wr, wph);
			}
		}
	}
	/* ITCM: espejo de 32 KB en [0, largo) */
	{
		const uint64_t lr = c->itcm_rd_len, lw = c->itcm_wr_len, l = lr > lw ? lr : lw;

		for (uint64_t o = 0; o < l; o += 0x4000u) {
			uint8_t **const pt = (uint8_t **)(uintptr_t)c->jit_pt;
			uint8_t *pg = n->itcm + (o & (ACORE_ITCM_SIZE - 1));
			uint8_t *rd = pt[o >> 14], *wr;
			uint32_t wph;

			if (o + 0x4000u <= lr)
				rd = pg;
			else if (o < lr)
				rd = NULL;
			if (o + 0x4000u <= lw) {
				wr = pg;
				wph = phys_of(n, pg, limit);
			} else if (o < lw) {
				wr = NULL;
				wph = AREC_PHYS_NONE;
			} else {
				/* solo de lectura en la ITCM: la escritura va al bus como estaba */
				continue;
			}
			arec_map_set(j, (uint32_t)(o >> 14), rd, wr, wph);
		}
	}
	arec_map_end(j);
}

/* ARM7: solo el bus (sin TCM ni CP15). */
static void remap7(struct nds *n)
{
	remap_bus(n, n->jit7, NDS_CPU7, phys_size7(n));
	arec_map_end(n->jit7);
}

static void on_tcm(struct acore *c)
{
	struct nds *n = c->user;

	if (!n->jit9)
		return;
	remap(n);
	arec_flush(n->jit9); /* la vista de captación cambió */
}

static void on_itcm(struct acore *c, uint32_t off, uint32_t size)
{
	struct nds *n = c->user;

	if (n->jit9)
		arec_code_write(n->jit9, off, size);
}

void nds_jit_wramcnt(struct nds *n)
{
	if (n->jit9) {
		remap(n);
		arec_flush(n->jit9);
	}
	if (n->jit7) {
		remap7(n);
		arec_flush(n->jit7); /* la WRAM que ve el ARM7 cambió */
	}
}

void nds_code_write_range(struct nds *n, const uint8_t *p, size_t len)
{
	if ((!n->jit9 && !n->jit7) || !len)
		return;
	for (size_t k = 0; k < len; k += 4) {
		const uint32_t ph = phys_of(n, p + k, n->jit_phys);

		if (ph == AREC_PHYS_NONE)
			continue;
		if (n->jit9 && ph < n->jit_phys9)
			arec_code_write(n->jit9, ph & ~3u, 4);
		if (n->jit7)
			arec_code_write(n->jit7, ph & ~3u, 4);
	}
}

void nds_code_write_slow(struct nds *n, const uint8_t *p, unsigned size)
{
	const uint32_t ph = phys_of(n, p, n->jit_phys);

	if (ph == AREC_PHYS_NONE)
		return;
	if (n->jit9 && ph < n->jit_phys9)
		arec_code_write(n->jit9, ph, size);
	if (n->jit7)
		arec_code_write(n->jit7, ph, size);
}

/* Ruta corta de las escrituras del ARM9 a GXFIFO y a los puertos de órdenes (arec_cfg.gx_wr): la palabra va directo al motor de
 * geometría. Con las trazas de la E/S activas (n->dbg) sigue por el bus para que queden en el registro. */
static void jit_gx_wr(void *ctx, uint32_t a, uint32_t v)
{
	struct nds *n = ctx;

	if (n->dbg)
		nds_bus_write(n, NDS_CPU9, a, 4, v);
	else
		nds_gx_cpu_write(n, a, v);
}

/* Cuántas páginas de 16 KB mapea el ARM7: de 0 al final de la WRAM propia (0x04000000). */
#define JIT7_MAP_PAGES (0x04000000u >> AREC_PAGE_SHIFT)
#define JIT7_MAX_BLOCKS 4096u

int nds_jit_attach(struct nds *n, const struct nds_jit_cfg *jc)
{
	struct arec_cfg cfg;
	size_t c9 = jc->code_bytes, c7 = 0;

	nds_jit_detach(n);
	if (jc->arm7 && jc->code7_bytes >= 65536 && jc->code7_bytes + 65536 <= jc->code_bytes) {
		c7 = jc->code7_bytes & ~(size_t)4095;
		c9 = jc->code_bytes - c7;
	}
	memset(&cfg, 0, sizeof cfg);
	cfg.code = jc->code;
	cfg.code_bytes = c9;
	cfg.strict = jc->strict;
	cfg.no_link = jc->no_link;
	cfg.no_inline_jl = jc->no_inline_jl;
	cfg.far_calls = jc->far_calls;
	cfg.opts = jc->opts;
	cfg.icache_flush = jc->icache_flush;
	cfg.code_ptr = code_ptr;
	cfg.on_nocode = jit_nocode;
	cfg.ctx = n;
	cfg.phys_size = phys_size9(n);
	cfg.on_exec = jc->on_exec;
	cfg.gx_wr = n->gx_lento ? NULL : jit_gx_wr;
	n->jit_phys9 = phys_size9(n);
	n->jit_phys = n->jit_phys9;
	if (!jc->no_arm9) {
		n->jit9 = arec_create(&n->a9, &cfg);
		if (!n->jit9)
			return -1;
		n->a9.tcm_hook = on_tcm;
		n->a9.itcm_hook = on_itcm;
	}
	if (c7) {
		memset(&cfg, 0, sizeof cfg);
		cfg.code = (uint8_t *)jc->code + c9;
		cfg.code_bytes = c7;
		cfg.strict = jc->strict;
		cfg.no_link = jc->no_link;
		cfg.no_inline_jl = jc->no_inline_jl;
		cfg.far_calls = jc->far_calls;
		cfg.opts = jc->opts;
		cfg.icache_flush = jc->icache_flush;
		cfg.code_ptr = code_ptr7;
		cfg.on_nocode = jit_nocode;
		cfg.ctx = n;
		cfg.phys_size = phys_size7(n);
		cfg.map_pages = JIT7_MAP_PAGES;
		cfg.max_blocks = JIT7_MAX_BLOCKS;
		cfg.on_exec = jc->on_exec7;
		n->jit7 = arec_create(&n->a7, &cfg);
		if (n->jit7)
			n->jit_phys = phys_size7(n);
	}
	if (!n->jit9 && !n->jit7) {
		n->jit_phys = n->jit_phys9 = 0;
		return -1;
	}
	n->jit_cfg = *jc;
	if (n->jit9)
		remap(n);
	if (n->jit7)
		remap7(n);
	return 0;
}

void nds_jit_detach(struct nds *n)
{
	if (n->jit9) {
		arec_destroy(n->jit9);
		n->jit9 = NULL;
		n->a9.tcm_hook = NULL;
		n->a9.itcm_hook = NULL;
	}
	if (n->jit7) {
		arec_destroy(n->jit7);
		n->jit7 = NULL;
	}
	n->jit_phys = n->jit_phys9 = 0;
}

/* Después de nds_init (que borra la estructura entera, también lo que arec_create dejó en las CPUs): se crean de nuevo. */
void nds_jit_reinit(struct nds *n)
{
	struct arec *const j9 = n->jit9, *const j7 = n->jit7;
	const struct nds_jit_cfg jc = n->jit_cfg;

	if (!j9 && !j7)
		return;
	n->jit9 = n->jit7 = NULL;
	arec_destroy(j9);
	arec_destroy(j7);
	nds_jit_attach(n, &jc);
}
