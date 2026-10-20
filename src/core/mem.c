/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * SalsaDS — mapa de memoria del ARM9 y el ARM7 (Fase 1.2).
 *
 * Ruta rápida: una tabla de punteros por región de 8 MB (`rgn_rd`/`rgn_wr`, 512 entradas por CPU) para la
 * RAM principal, la WRAM compartida y la propia del ARM7, la paleta y la OAM. Todo lo demás (E/S, VRAM por
 * bancos, ranura de GBA, BIOS) va por la ruta lenta. La tabla se rehace cuando cambia WRAMCNT.
 *
 * VRAM: nueve bancos (A–I) en un solo arreglo en el orden del modo LCDC. VRAMCNT los mapea a ventanas
 * (fondos y objetos de los motores A y B, ARM7, LCDC) en páginas de 16 KB; dos bancos sobre la misma página
 * se leen con OR y se escriben los dos (GBATEK). Los usos que no son visibles para la CPU (textura, paletas
 * extendidas) se anotan en `vramcnt` para las Fases 2 y 3.
 *
 * Reglas de escritura: ARM9, sin escrituras de 8 bits a paleta, OAM ni VRAM (se ignoran); el ARM7 sí puede
 * escribir bytes en la VRAM que tiene mapeada (GBATEK, «DS Memory Control - VRAM»).
 *
 * Fuente: GBATEK (mapas de memoria y control de WRAM/VRAM). Código propio.
 */
#include <string.h>

#include "core/nds_priv.h"

static const uint32_t bank_size[9] = { 0x20000, 0x20000, 0x20000, 0x20000, 0x10000, 0x4000, 0x4000, 0x8000, 0x4000 };
static const uint32_t bank_base[9] = { 0, 0x20000, 0x40000, 0x60000, 0x80000, 0x90000, 0x94000, 0x98000, 0xA0000 };
/* Tamaño de cada ventana de la VRAM que ve el ARM9 (se repite dentro de su región de 2 MB). */
static const uint32_t win_size[4] = { 0x80000, 0x20000, 0x40000, 0x20000 };

uint32_t nds_vram_bank_offset(unsigned bank)
{
	return bank_base[bank];
}

/* Desplazamiento (en el arreglo `vram`) de la página `pg` de una vista, o G2D_UNMAPPED. Si hay varios bancos sobre la
 * misma página, se toma el de menor índice (la lectura de la CPU los combina con OR; el renderizador no). */
uint32_t nds_vram_view_offset(const struct nds *n, unsigned win, unsigned pg)
{
	const uint16_t m = n->vpg_mask[win][pg];
	unsigned b;

	if (!m)
		return G2D_UNMAPPED;
	b = (unsigned)__builtin_ctz(m);
	return bank_base[b] + (pg * 0x4000u - n->vbank_off[b]);
}

static inline uint32_t rd_sz(const uint8_t *p, int sz)
{
	return sz == 4 ? rd32le(p) : sz == 2 ? rd16le(p) : *p;
}

static inline void wr_sz(uint8_t *p, int sz, uint32_t v)
{
	if (sz == 4)
		wr32le(p, v);
	else if (sz == 2)
		wr16le(p, (uint16_t)v);
	else
		*p = (uint8_t)v;
}

/* ---- tablas de la ruta rápida ----------------------------------------------------------------- */

static void set_rgn(struct nds_rgn *t, unsigned first, unsigned count, uint8_t *p, uint32_t mask)
{
	for (unsigned i = 0; i < count; i++) {
		t[first + i].p = p;
		t[first + i].mask = mask;
	}
}

static void rebuild_rgn(struct nds *n)
{
	memset(n->rgn_rd, 0, sizeof n->rgn_rd);
	memset(n->rgn_wr, 0, sizeof n->rgn_wr);
	for (int cpu = 0; cpu < 2; cpu++) {
		struct nds_rgn *rd = n->rgn_rd[cpu], *wr = n->rgn_wr[cpu];
		uint8_t *sp = NULL, *wp = NULL;
		uint32_t smask = 0, wmask = 0;

		set_rgn(rd, 4, 2, n->ram, NDS_RAM_SIZE - 1); /* 0x02000000-0x02FFFFFF, espejo cada 4 MB */
		set_rgn(wr, 4, 2, n->ram, NDS_RAM_SIZE - 1);
		if (cpu == NDS_CPU9) {
			switch (n->wramcnt) {
			case 0: sp = n->swram; smask = 0x7FFF; break;
			case 1: sp = n->swram + 0x4000; smask = 0x3FFF; break;
			case 2: sp = n->swram; smask = 0x3FFF; break;
			default: sp = NULL; break; /* sin WRAM para el ARM9: vacío */
			}
			if (sp) {
				set_rgn(rd, 6, 2, sp, smask);
				set_rgn(wr, 6, 2, sp, smask);
			}
			set_rgn(rd, 10, 2, n->pal, NDS_PAL_SIZE - 1); /* 0x05000000: paleta */
			set_rgn(rd, 14, 2, n->oam, NDS_OAM_SIZE - 1); /* 0x07000000: OAM */
		} else {
			switch (n->wramcnt) {
			case 0: sp = n->wram7; smask = 0xFFFF; break; /* nada compartido: espejo de la WRAM del ARM7 */
			case 1: sp = n->swram; smask = 0x3FFF; break;
			case 2: sp = n->swram + 0x4000; smask = 0x3FFF; break;
			default: sp = n->swram; smask = 0x7FFF; break;
			}
			wp = n->wram7;
			wmask = 0xFFFF;
			set_rgn(rd, 6, 1, sp, smask); /* 0x03000000-0x037FFFFF */
			set_rgn(wr, 6, 1, sp, smask);
			set_rgn(rd, 7, 1, wp, wmask); /* 0x03800000-0x03FFFFFF */
			set_rgn(wr, 7, 1, wp, wmask);
		}
	}
}

void nds_mem_wramcnt(struct nds *n, uint8_t v)
{
	n->wramcnt = v & 3;
	rebuild_rgn(n);
	nds_jit_wramcnt(n);
}

/* ---- VRAM por bancos ---------------------------------------------------------------------------- */

static void map_pages(struct nds *n, unsigned win, unsigned bank, uint32_t view_base)
{
	const uint32_t first = view_base >> 14, count = bank_size[bank] >> 14;

	n->vbank_off[bank] = view_base;
	for (uint32_t i = 0; i < count; i++) {
		const uint32_t idx = first + i;

		if (idx >= win_size[win] >> 14)
			break;
		n->vpg_mask[win][idx] |= (uint16_t)(1u << bank);
	}
}

static void vram_remap(struct nds *n)
{
	memset(n->vpg_mask, 0, sizeof n->vpg_mask);
	memset(n->vpg7_mask, 0, sizeof n->vpg7_mask);
	memset(n->vram_lcdc_mask, 0, sizeof n->vram_lcdc_mask);
	n->vramstat = 0;

	for (unsigned b = 0; b < 9; b++) {
		const uint8_t v = n->vramcnt[b];
		const unsigned ofs = (v >> 3) & 3;
		unsigned mst = v & 7;

		if (!(v & 0x80))
			continue;
		if (b <= 1 || b >= 7)
			mst &= 3; /* el bit 2 no existe en A, B, H, I */
		if (mst == 0) { /* LCDC */
			for (uint32_t pg = bank_base[b] >> 14; pg < (bank_base[b] + bank_size[b]) >> 14; pg++)
				n->vram_lcdc_mask[pg] |= (uint8_t)(1u << 0);
			continue;
		}
		switch (b) {
		case 0: /* A, B: 128 KB */
		case 1:
			if (mst == 1)
				map_pages(n, 0, b, 0x20000u * ofs);
			else if (mst == 2)
				map_pages(n, 2, b, 0x20000u * (ofs & 1));
			break;
		case 2: /* C */
		case 3: /* D */
			if (mst == 1) {
				map_pages(n, 0, b, 0x20000u * ofs);
			} else if (mst == 2) {
				const uint32_t base = 0x20000u * (ofs & 1);

				n->vbank_off[b] = base;
				for (uint32_t i = 0; i < bank_size[b] >> 14; i++)
					n->vpg7_mask[(base >> 14) + i] |= (uint16_t)(1u << b);
				n->vramstat |= 1u << (b - 2);
			} else if (mst == 4) {
				if (b == 2)
					map_pages(n, 1, b, 0); /* C como fondo de B */
				else
					map_pages(n, 3, b, 0); /* D como OBJ de B */
			}
			break;
		case 4: /* E */
			if (mst == 1)
				map_pages(n, 0, b, 0);
			else if (mst == 2)
				map_pages(n, 2, b, 0);
			break;
		case 5: /* F */
		case 6: /* G */
			if (mst == 1)
				map_pages(n, 0, b, 0x4000u * (ofs & 1) + 0x10000u * (ofs >> 1));
			else if (mst == 2)
				map_pages(n, 2, b, 0x4000u * (ofs & 1) + 0x10000u * (ofs >> 1));
			break;
		case 7: /* H */
			if (mst == 1)
				map_pages(n, 1, b, 0);
			break;
		case 8: /* I */
			if (mst == 1)
				map_pages(n, 1, b, 0x8000);
			else if (mst == 2)
				map_pages(n, 3, b, 0);
			break;
		}
	}

	/* punteros directos para las páginas que cubre un solo banco */
	for (unsigned w = 0; w < 4; w++)
		for (unsigned pg = 0; pg < 32; pg++) {
			const uint16_t m = n->vpg_mask[w][pg];

			n->vpg_ptr[w][pg] = NULL;
			if (m && !(m & (m - 1))) {
				const unsigned b = (unsigned)__builtin_ctz(m);

				n->vpg_ptr[w][pg] = n->vram + bank_base[b] + (pg * 0x4000u - n->vbank_off[b]);
			}
		}
	for (unsigned pg = 0; pg < 16; pg++) {
		const uint16_t m = n->vpg7_mask[pg];

		n->vpg7_ptr[pg] = NULL;
		if (m && !(m & (m - 1))) {
			const unsigned b = (unsigned)__builtin_ctz(m);

			n->vpg7_ptr[pg] = n->vram + bank_base[b] + (pg * 0x4000u - n->vbank_off[b]);
		}
	}
}

void nds_vramcnt_write(struct nds *n, unsigned bank, uint8_t v)
{
	if (bank > 8)
		return;
	n->vramcnt[bank] = v;
	vram_remap(n);
}

/* Páginas de las ventanas: devuelve la máscara de bancos y, si hay uno solo, su puntero (con el desplazamiento
 * dentro de la página ya sumado en *p1). `off` es el desplazamiento dentro de la ventana. */
static uint32_t vram_read(struct nds *n, int cpu, uint32_t a, int sz)
{
	uint16_t mask;
	uint8_t *ptr;
	uint32_t pgoff = a & 0x3FFF, pgstart, v = 0;

	if (cpu == NDS_CPU7) {
		const unsigned pg = (a & 0x3FFFF) >> 14;

		mask = n->vpg7_mask[pg];
		ptr = n->vpg7_ptr[pg];
		pgstart = pg * 0x4000u;
	} else {
		const unsigned region = (a >> 21) & 7;

		if (region == 4) { /* LCDC */
			const uint32_t off = a - 0x06800000u;

			if (off < NDS_VRAM_SIZE && n->vram_lcdc_mask[off >> 14])
				return rd_sz(n->vram + off, sz);
			return 0;
		}
		if (region > 3)
			return 0;
		{
			const unsigned pg = (a & (win_size[region] - 1)) >> 14;

			mask = n->vpg_mask[region][pg];
			ptr = n->vpg_ptr[region][pg];
			pgstart = pg * 0x4000u;
		}
	}
	if (!mask)
		return 0;
	if (ptr)
		return rd_sz(ptr + pgoff, sz);
	for (unsigned b = 0; b < 9; b++)
		if (mask & (1u << b))
			v |= rd_sz(n->vram + bank_base[b] + (pgstart - n->vbank_off[b]) + pgoff, sz);
	return v;
}

static void vram_write(struct nds *n, int cpu, uint32_t a, int sz, uint32_t v)
{
	uint16_t mask;
	uint8_t *ptr;
	uint32_t pgoff = a & 0x3FFF, pgstart;

	if (cpu == NDS_CPU7) {
		const unsigned pg = (a & 0x3FFFF) >> 14;

		mask = n->vpg7_mask[pg];
		ptr = n->vpg7_ptr[pg];
		pgstart = pg * 0x4000u;
	} else {
		const unsigned region = (a >> 21) & 7;

		if (sz == 1)
			return; /* el ARM9 no escribe bytes en la VRAM (GBATEK) */
		if (region == 4) {
			const uint32_t off = a - 0x06800000u;

			if (off < NDS_VRAM_SIZE && n->vram_lcdc_mask[off >> 14]) {
				wr_sz(n->vram + off, sz, v);
				nds_video_vram_dirty(n, off);
			}
			return;
		}
		if (region > 3)
			return;
		{
			const unsigned pg = (a & (win_size[region] - 1)) >> 14;

			mask = n->vpg_mask[region][pg];
			ptr = n->vpg_ptr[region][pg];
			pgstart = pg * 0x4000u;
		}
	}
	if (!mask)
		return;
	if (ptr) {
		wr_sz(ptr + pgoff, sz, v);
		nds_video_vram_dirty(n, (uint32_t)(ptr + pgoff - n->vram));
		return;
	}
	for (unsigned b = 0; b < 9; b++)
		if (mask & (1u << b)) {
			const uint32_t o = bank_base[b] + (pgstart - n->vbank_off[b]) + pgoff;

			wr_sz(n->vram + o, sz, v);
			nds_video_vram_dirty(n, o);
		}
}

/* ---- ruta lenta ------------------------------------------------------------------------------------ */

uint32_t nds_bus_read(struct nds *n, int cpu, uint32_t a, int sz)
{
	const struct nds_rgn *r = &n->rgn_rd[cpu][a >> 23];

	if (r->p)
		return rd_sz(r->p + (a & r->mask), sz);
	switch (a >> 24) {
	case 0x00:
		if (cpu == NDS_CPU7 && a < NDS_BIOS7_SIZE)
			return rd_sz(n->bios7 + a, sz);
		return 0;
	case 0x04:
		return nds_io_read(n, cpu, a, sz);
	case 0x06:
		return vram_read(n, cpu, a, sz);
	case 0x08:
	case 0x09: /* ranura de GBA, ROM: vacía (el que no la posee lee ceros) */
		return ((n->io[NDS_CPU9].exmem >> 7) & 1) == (unsigned)cpu ? 0xFFFFFFFFu >> (32 - 8 * sz) : 0;
	case 0x0A:
		return ((n->io[NDS_CPU9].exmem >> 7) & 1) == (unsigned)cpu ? 0xFFu * (sz == 4 ? 0x01010101u : sz == 2 ? 0x0101u : 1u) : 0;
	case 0xFF:
		if (cpu == NDS_CPU9 && (a & 0xFFFFF000u) == 0xFFFF0000u)
			return rd_sz(n->bios9 + (a & 0xFFF), sz);
		return 0;
	default:
		return 0;
	}
}

void nds_bus_write(struct nds *n, int cpu, uint32_t a, int sz, uint32_t v)
{
	const struct nds_rgn *r = &n->rgn_wr[cpu][a >> 23];

	if (r->p) {
		wr_sz(r->p + (a & r->mask), sz, v);
		NDS_CODE_WRITE(n, r->p + (a & r->mask), (unsigned)sz);
		return;
	}
	switch (a >> 24) {
	case 0x04:
		nds_io_write(n, cpu, a, sz, v);
		return;
	case 0x05:
		if (cpu == NDS_CPU9 && sz >= 2) {
			wr_sz(n->pal + (a & (NDS_PAL_SIZE - 1)), sz, v);
			for (int k = 0; k < sz; k += 2)
				nds_video_pal_written(n, (a + (uint32_t)k) & (NDS_PAL_SIZE - 1));
		}
		return;
	case 0x06:
		vram_write(n, cpu, a, sz, v);
		return;
	case 0x07:
		if (cpu == NDS_CPU9 && sz >= 2) {
			wr_sz(n->oam + (a & (NDS_OAM_SIZE - 1)), sz, v);
			for (int k = 0; k < sz; k += 2)
				nds_video_oam_written(n, (a + (uint32_t)k) & (NDS_OAM_SIZE - 1));
		}
		return;
	default:
		return;
	}
}

/* ---- callbacks del bus de cada CPU --------------------------------------------------------------- */

#define BUS_FNS(NAME, CPU, GXFAST)                                                                                    \
	static uint32_t NAME##_r8(struct acore *c, uint32_t a)                                                \
	{                                                                                                     \
		struct nds *n = c->user;                                                                      \
		const struct nds_rgn *r = &n->rgn_rd[CPU][a >> 23];                                           \
		return NDS_LIKELY(r->p) ? r->p[a & r->mask] : nds_bus_read(n, CPU, a, 1);                     \
	}                                                                                                     \
	static uint32_t NAME##_r16(struct acore *c, uint32_t a)                                               \
	{                                                                                                     \
		struct nds *n = c->user;                                                                      \
		const struct nds_rgn *r = &n->rgn_rd[CPU][a >> 23];                                           \
		return NDS_LIKELY(r->p) ? rd16le(r->p + (a & r->mask)) : nds_bus_read(n, CPU, a, 2);          \
	}                                                                                                     \
	static uint32_t NAME##_r32(struct acore *c, uint32_t a)                                               \
	{                                                                                                     \
		struct nds *n = c->user;                                                                      \
		const struct nds_rgn *r = &n->rgn_rd[CPU][a >> 23];                                           \
		return NDS_LIKELY(r->p) ? rd32le(r->p + (a & r->mask)) : nds_bus_read(n, CPU, a, 4);          \
	}                                                                                                     \
	static void NAME##_w8(struct acore *c, uint32_t a, uint32_t v)                                        \
	{                                                                                                     \
		struct nds *n = c->user;                                                                      \
		const struct nds_rgn *r = &n->rgn_wr[CPU][a >> 23];                                           \
		if (NDS_LIKELY(r->p)) {                                                                       \
			r->p[a & r->mask] = (uint8_t)v;                                                       \
			NDS_CODE_WRITE(n, r->p + (a & r->mask), 1);                                                              \
		} else {                                                                                      \
			nds_bus_write(n, CPU, a, 1, v);                                                       \
		}                                                                                             \
	}                                                                                                     \
	static void NAME##_w16(struct acore *c, uint32_t a, uint32_t v)                                       \
	{                                                                                                     \
		struct nds *n = c->user;                                                                      \
		const struct nds_rgn *r = &n->rgn_wr[CPU][a >> 23];                                           \
		if (NDS_LIKELY(r->p)) {                                                                       \
			wr16le(r->p + (a & r->mask), (uint16_t)v);                                            \
			NDS_CODE_WRITE(n, r->p + (a & r->mask), 2);                                                              \
		} else {                                                                                      \
			nds_bus_write(n, CPU, a, 2, v);                                                       \
		}                                                                                             \
	}                                                                                                     \
	static void NAME##_w32(struct acore *c, uint32_t a, uint32_t v)                                       \
	{                                                                                                     \
		struct nds *n = c->user;                                                                      \
		const struct nds_rgn *r = &n->rgn_wr[CPU][a >> 23];                                           \
		if (NDS_LIKELY(r->p)) {                                                                       \
			wr32le(r->p + (a & r->mask), v);                                                      \
			NDS_CODE_WRITE(n, r->p + (a & r->mask), 4);                                                              \
		} else if (GXFAST && a - 0x04000400u < 0x200u && !(a & 3u) && !n->gx_lento && !n->dbg) {       \
			nds_gx_cpu_write(n, a, v); /* GXFIFO y puertos de órdenes del ARM9 por la ruta corta */ \
		} else {                                                                                      \
			nds_bus_write(n, CPU, a, 4, v);                                                       \
		}                                                                                             \
	}

BUS_FNS(b9, NDS_CPU9, 1)
BUS_FNS(b7, NDS_CPU7, 0)

const struct acore_bus nds_bus9 = { b9_r8, b9_r16, b9_r32, b9_w8, b9_w16, b9_w32, b9_r16, b9_r32 };
const struct acore_bus nds_bus7 = { b7_r8, b7_r16, b7_r32, b7_w8, b7_w16, b7_w32, b7_r16, b7_r32 };

void nds_mem_init(struct nds *n)
{
	n->wramcnt = 3;
	memset(n->vramcnt, 0, sizeof n->vramcnt);
	rebuild_rgn(n);
	vram_remap(n);
}

/* ---- depuración -------------------------------------------------------------------------------------- */

/* Las lecturas de depuración de la E/S solo valen para los registros sin efecto al leerse: no los de las FIFO ni
 * el dato del cartucho (0x04100000 en adelante). */
/* Las TCM del ARM9 se ven aquí tal como las ve la CPU (para trazas y volcados). */
static int peek_tcm(struct nds *n, int cpu, uint32_t a, uint8_t *out)
{
	const struct acore *c = &n->a9;

	if (cpu != NDS_CPU9)
		return 0;
	if (c->itcm && a < c->itcm_wr_len) {
		*out = c->itcm[a & 0x7FFFu];
		return 1;
	}
	if (c->dtcm && c->dtcm_wr_len && a - c->dtcm_base < c->dtcm_wr_len) {
		*out = c->dtcm[(a - c->dtcm_base) & 0x3FFFu];
		return 1;
	}
	return 0;
}

uint32_t nds_peek32(struct nds *n, int cpu, uint32_t a)
{
	uint8_t b[4];

	a &= ~3u;
	if (peek_tcm(n, cpu, a, &b[0]) && peek_tcm(n, cpu, a + 1, &b[1]) && peek_tcm(n, cpu, a + 2, &b[2]) &&
	    peek_tcm(n, cpu, a + 3, &b[3]))
		return (uint32_t)b[0] | (uint32_t)b[1] << 8 | (uint32_t)b[2] << 16 | (uint32_t)b[3] << 24;
	if ((a >> 24) == 4 && (a & 0xFFFFFF) >= 0x100000)
		return 0;
	return nds_bus_read(n, cpu, a, 4);
}

uint32_t nds_peek8(struct nds *n, int cpu, uint32_t a)
{
	uint8_t b;

	if (peek_tcm(n, cpu, a, &b))
		return b;
	if ((a >> 24) == 4 && (a & 0xFFFFFF) >= 0x100000)
		return 0;
	return nds_bus_read(n, cpu, a, 1);
}
