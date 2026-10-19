/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * SalsaDS — el recompilador del ARM9 (src/jit/arec*) conectado a la máquina de la DS (Fase 4).
 *
 * Lo propio de la DS que el recompilador no sabe: de dónde sale el código (ITCM, RAM principal, WRAM compartida y la
 * BIOS en HLE; nada de VRAM ni E/S: eso lo ejecuta el intérprete), la tabla de páginas de la ruta rápida (RAM y WRAM
 * por las regiones del mapa, encima la DTCM y la ITCM como las ve la CPU según CP15, con su modo de carga) y los
 * avisos de escritura a memoria con código (bus, ITCM desde el núcleo, captura de sonido). El espacio físico para
 * invalidar es el tramo de `struct nds` que va de `itcm` al final de `swram` (ITCM, DTCM, RAM y WRAM seguidas).
 *
 * Código propio (GPL-2.0-or-later).
 */
#include <string.h>

#include "core/nds_priv.h"
#include "jit/arec.h"

static uint32_t phys_size(const struct nds *n)
{
	return (uint32_t)((const uint8_t *)n->swram + NDS_SWRAM_SIZE - (const uint8_t *)n->itcm);
}

static uint32_t phys_of(const struct nds *n, const uint8_t *p)
{
	const uintptr_t off = (uintptr_t)p - (uintptr_t)n->itcm;

	if (!p || off >= phys_size(n))
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
		const uint32_t ph = phys_of(n, p);

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

/* Tabla de páginas de la ruta rápida, en el orden del intérprete: bus, encima DTCM, encima ITCM. */
static void remap(struct nds *n)
{
	struct arec *j = n->jit9;
	const struct acore *c = &n->a9;

	arec_map_begin(j);
	for (uint32_t r = 0; r < 512; r++) {
		const struct nds_rgn *rr = &n->rgn_rd[NDS_CPU9][r], *rw = &n->rgn_wr[NDS_CPU9][r];
		const int ord = rr->p && rr->mask >= 0x3FFFu && phys_of(n, rr->p) != AREC_PHYS_NONE;
		const int owr = rw->p && rw->mask >= 0x3FFFu && phys_of(n, rw->p) != AREC_PHYS_NONE;

		if (!ord && !owr)
			continue;
		for (uint32_t pg = 0; pg < 512; pg++) {
			const uint32_t a = r << 23 | pg << 14;
			uint8_t *rd = ord ? rr->p + (a & rr->mask & ~0x3FFFu) : NULL;
			uint8_t *wr = owr ? rw->p + (a & rw->mask & ~0x3FFFu) : NULL;

			arec_map_set(j, a >> 14, rd, wr, phys_of(n, wr));
		}
	}
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
					wph = phys_of(n, n->dtcm);
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
				wph = phys_of(n, pg);
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
	if (!n->jit9)
		return;
	remap(n);
	arec_flush(n->jit9);
}

void nds_code_write_range(struct nds *n, const uint8_t *p, size_t len)
{
	if (!n->jit9 || !len)
		return;
	for (size_t k = 0; k < len; k += 4) {
		const uint32_t ph = phys_of(n, p + k);

		if (ph != AREC_PHYS_NONE)
			arec_code_write(n->jit9, ph & ~3u, 4);
	}
}

void nds_code_write_slow(struct nds *n, const uint8_t *p, unsigned size)
{
	const uint32_t ph = phys_of(n, p);

	if (ph != AREC_PHYS_NONE)
		arec_code_write(n->jit9, ph, size);
}

int nds_jit_attach(struct nds *n, const struct nds_jit_cfg *jc)
{
	struct arec_cfg cfg;

	memset(&cfg, 0, sizeof cfg);
	cfg.code = jc->code;
	cfg.code_bytes = jc->code_bytes;
	cfg.strict = jc->strict;
	cfg.no_link = jc->no_link;
	cfg.no_inline_jl = jc->no_inline_jl;
	cfg.far_calls = jc->far_calls;
	cfg.icache_flush = jc->icache_flush;
	cfg.code_ptr = code_ptr;
	cfg.ctx = n;
	cfg.phys_size = phys_size(n);
	cfg.on_exec = jc->on_exec;
	if (n->jit9)
		arec_destroy(n->jit9);
	n->jit9 = arec_create(&n->a9, &cfg);
	if (!n->jit9)
		return -1;
	n->jit_cfg = *jc;
	n->jit_phys = phys_size(n);
	n->a9.tcm_hook = on_tcm;
	n->a9.itcm_hook = on_itcm;
	remap(n);
	return 0;
}

void nds_jit_detach(struct nds *n)
{
	if (!n->jit9)
		return;
	arec_destroy(n->jit9);
	n->jit9 = NULL;
	n->jit_phys = 0;
	n->a9.tcm_hook = NULL;
	n->a9.itcm_hook = NULL;
}

/* Después de nds_init (que borra la estructura entera, también lo que arec_create dejó en el ARM9): se crea de nuevo. */
void nds_jit_reinit(struct nds *n)
{
	struct arec *const j = n->jit9;
	const struct nds_jit_cfg jc = n->jit_cfg;

	if (!j)
		return;
	n->jit9 = NULL;
	arec_destroy(j);
	nds_jit_attach(n, &jc);
}
