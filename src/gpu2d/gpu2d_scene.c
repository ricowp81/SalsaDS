/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * SalsaDS — escenas sintéticas y aleatorias para el 2D (Fase 2.1). Ver gpu2d_scene.h.
 */
#include <string.h>

#include "gpu2d/gpu2d_scene.h"
#include "util/endian.h"

typedef struct {
	uint32_t s;
} rng;

static uint32_t rnd(rng *r)
{
	uint32_t x = r->s;

	x ^= x << 13;
	x ^= x >> 17;
	x ^= x << 5;
	return r->s = x;
}

static uint32_t rn(rng *r, uint32_t n)
{
	return rnd(r) % n;
}

static int chance(rng *r, uint32_t pct)
{
	return rn(r, 100) < pct;
}

/* Palabra con muchos ceros: teselas con huecos transparentes en 4, 8 y 16 bits */
static uint32_t sparse(rng *r)
{
	uint32_t w = rnd(r);

	if (rnd(r) & 1)
		w &= rnd(r);
	if (rnd(r) & 1)
		w &= rnd(r);
	return w;
}

static void fill_sparse(uint8_t *p, uint32_t bytes, rng *r)
{
	for (uint32_t i = 0; i + 3 < bytes; i += 4)
		wr32le(p + i, sparse(r));
}

static void set32(uint16_t *regs, int idx, uint32_t v)
{
	regs[idx] = (uint16_t)v;
	regs[idx + 1] = (uint16_t)(v >> 16);
}

static void add_change(gpu2d_engine *e, const gpu2d_scene_mem *m, int eng, int line, uint32_t reg, uint32_t value)
{
	gpu2d_change *c;

	if (e->nchg >= GPU2D_SCENE_CHG_CAP)
		return;
	c = &m->chg[eng][e->nchg++];
	c->line = (uint16_t)line;
	c->reg = (uint16_t)reg;
	c->value = value;
}

/* Triángulo de periodo `period` y amplitud ±amp, en enteros. */
static int tri(int i, int period, int amp)
{
	const int h = period / 2;
	const int mm = ((i % period) + period) % period;
	const int d = mm < h ? mm : period - mm;

	return (d * 2 * amp) / h - amp;
}

/* Páginas de las vistas: identidad (las fijas) */
static void identity_pages(gpu2d_engine *e, int isb)
{
	e->bgpages = isb ? G2D_BG_PAGES_B : G2D_BG_PAGES_A;
	e->objpages = isb ? G2D_OBJ_PAGES_B : G2D_OBJ_PAGES_A;
	for (uint32_t p = 0; p < G2D_BG_PAGES_A; p++)
		e->bgpg[p] = p < e->bgpages ? (isb ? 0x40000u : 0u) + p * 0x4000u : G2D_UNMAPPED;
	for (uint32_t p = 0; p < G2D_OBJ_PAGES_A; p++)
		e->objpg[p] = p < e->objpages ? (isb ? 0x60000u : 0u) + p * 0x4000u : G2D_UNMAPPED;
	for (int k = 0; k < 4; k++) {
		e->extbg[k] = G2D_UNMAPPED;
		e->vdisp[k] = 0x20000u * (uint32_t)k;
	}
	e->extobj = G2D_UNMAPPED;
}

static void random_pages(gpu2d_engine *e, int isb, rng *r)
{
	const uint32_t npages = G2D_VRAM_SIZE / 0x4000u;

	e->bgpages = isb ? G2D_BG_PAGES_B : G2D_BG_PAGES_A;
	e->objpages = isb ? G2D_OBJ_PAGES_B : G2D_OBJ_PAGES_A;
	for (uint32_t p = 0; p < G2D_BG_PAGES_A; p++)
		e->bgpg[p] = (p < e->bgpages && chance(r, 88)) ? rn(r, npages) * 0x4000u : G2D_UNMAPPED;
	for (uint32_t p = 0; p < G2D_OBJ_PAGES_A; p++)
		e->objpg[p] = (p < e->objpages && chance(r, 88)) ? rn(r, npages) * 0x4000u : G2D_UNMAPPED;
	for (int k = 0; k < 4; k++) {
		e->extbg[k] = chance(r, 85) ? rn(r, (G2D_VRAM_SIZE - 0x2000u) / 0x2000u) * 0x2000u : G2D_UNMAPPED;
		e->vdisp[k] = chance(r, 80) ? 0x20000u * (uint32_t)k : G2D_UNMAPPED;
	}
	e->extobj = chance(r, 85) ? rn(r, (G2D_VRAM_SIZE - 0x2000u) / 0x2000u) * 0x2000u : G2D_UNMAPPED;
}

/* OAM aleatoria (como la ROM gfxrand) con las características que `off` no apaga */
static void random_oam(uint8_t *oam, rng *r, int profile, uint32_t off)
{
	for (int i = 0; i < GPU2D_NOBJ; i++) {
		uint32_t a0 = rn(r, 256), a1 = rn(r, 512), a2 = rnd(r) & 0xFFFF;
		const int sane = chance(r, 90);

		a0 = (a0 + 216) & 255;
		a1 = (a1 + 472) & 511;
		if (a1 > 300 && a1 < 460)
			a1 = rn(r, 256);
		a0 |= (sane ? rn(r, 3) : rn(r, 4)) << 14;
		a1 |= rn(r, 4) << 14;
		if (chance(r, profile == 2 || profile == 7 ? 55 : 20)) {
			a0 |= 0x100;
			if (chance(r, 50))
				a0 |= 0x200;
			a1 |= rn(r, 32) << 9;
		} else {
			if (chance(r, 6))
				a0 |= 0x200;
			a1 |= rn(r, 4) << 12;
		}
		if (chance(r, 12))
			a0 |= 1u << 10;
		if (chance(r, profile == 1 || profile == 7 ? 12 : 4))
			a0 |= 2u << 10;
		if (chance(r, profile == 3 || profile == 7 ? 20 : 4))
			a0 |= 3u << 10;
		if (chance(r, profile == 6 ? 60 : 12))
			a0 |= 1u << 12;
		if (chance(r, 45))
			a0 |= 1u << 13;
		{
			const uint32_t mode = (a0 >> 10) & 3;

			if (off & 0x010)
				a0 &= ~0x100u;
			if (off & 0x800)
				a0 &= ~0x1000u;
			if (((off & 0x020) && mode == 1) || ((off & 0x040) && mode == 2) || ((off & 0x080) && mode == 3))
				a0 &= ~0xC00u;
		}
		wr16le(oam + i * 8, (uint16_t)a0);
		wr16le(oam + i * 8 + 2, (uint16_t)a1);
		wr16le(oam + i * 8 + 4, (uint16_t)a2);
	}
	for (int g = 0; g < 32; g++) {
		const int rotg = chance(r, 70);
		const int sc = 0x80 + (int)rn(r, 0x180);

		wr16le(oam + (g * 4 + 0) * 8 + 6, (uint16_t)(rotg ? sc : 0x100));
		wr16le(oam + (g * 4 + 1) * 8 + 6, (uint16_t)(rotg ? (int)rn(r, 0x200) - 0x100 : 0));
		wr16le(oam + (g * 4 + 2) * 8 + 6, (uint16_t)(rotg ? (int)rn(r, 0x200) - 0x100 : 0));
		wr16le(oam + (g * 4 + 3) * 8 + 6, (uint16_t)(rotg ? sc : 0x100));
	}
}

static void random_engine(gpu2d_engine *e, const gpu2d_scene_mem *m, int eng, rng *r, uint32_t seed)
{
	const int isb = eng == 1;
	const uint32_t off = seed >> 20;
	const int profile = (int)(seed & 7u);
	uint32_t dc, mode;
	uint16_t *regs = e->regs;

	if (chance(r, 10))
		random_pages(e, isb, r);
	else
		identity_pages(e, isb);
	if (chance(r, 40))
		random_pages(e, isb, r);
	switch (profile) {
	case 0: mode = 0; break;
	case 1: mode = rn(r, 2); break;
	case 2: mode = 1 + rn(r, 2) * (rn(r, 2) ? 3 : 1); break;
	case 3: mode = 3 + rn(r, 3); break;
	case 4: mode = 6; break;
	default: mode = rn(r, 8); break;
	}
	dc = mode;
	dc |= 1u << 16;
	if (chance(r, 8))
		dc = (dc & ~(3u << 16)) | (rn(r, 4) << 16);
	for (int b = 0; b < 4; b++)
		if (chance(r, 75))
			dc |= G2D_DC_BG(b);
	if (chance(r, 80))
		dc |= G2D_DC_OBJ;
	if (profile == 4 || chance(r, 8))
		dc |= G2D_DC_3D;
	if (chance(r, 50))
		dc |= G2D_DC_OBJ_1D;
	dc |= rn(r, 4) << 20;
	if (chance(r, 50))
		dc |= G2D_DC_OBJBMP_256;
	if (chance(r, 50))
		dc |= G2D_DC_OBJBMP_1D;
	if (chance(r, 50))
		dc |= G2D_DC_OBJBMP_BND;
	dc |= (rn(r, 4) << 24) | (rn(r, 4) << 27) | (rn(r, 4) << 18);
	if (profile == 1 || profile == 7 || chance(r, 15)) {
		if (chance(r, 60))
			dc |= G2D_DC_WIN0;
		if (chance(r, 60))
			dc |= G2D_DC_WIN1;
		if (chance(r, 50))
			dc |= G2D_DC_OBJWIN;
	}
	if (profile == 5 || chance(r, 15)) {
		if (chance(r, 85))
			dc |= G2D_DC_BGEXTPAL;
		if (chance(r, 85))
			dc |= G2D_DC_OBJEXTPAL;
	}
	if (chance(r, 3))
		dc |= G2D_DC_BLANK;
	if (off & 0x001)
		dc &= ~G2D_DC_OBJ;
	if (off & 0x002)
		dc &= ~(G2D_DC_WIN0 | G2D_DC_WIN1 | G2D_DC_OBJWIN);
	if (off & 0x100)
		dc &= ~7u;
	if (off & 0x200)
		dc &= ~(uint32_t)G2D_DC_3D;
	set32(regs, G2D_R_DISPCNT, dc);
	for (int b = 0; b < 4; b++) {
		uint32_t cnt = rn(r, 4) | (rn(r, 16) << 2) | (rn(r, 32) << 8) | (rn(r, 4) << 14);

		if (chance(r, 55))
			cnt |= 0x80;
		if (chance(r, profile == 6 ? 70 : 12))
			cnt |= 0x40;
		if (chance(r, 40))
			cnt |= 0x2000;
		if (chance(r, 30))
			cnt |= 4;
		if (off & 0x008)
			cnt &= ~0x40u;
		regs[G2D_R_BGCNT + b] = (uint16_t)cnt;
		regs[G2D_R_BGHOFS + 2 * b] = (uint16_t)rn(r, 512);
		regs[G2D_R_BGVOFS + 2 * b] = (uint16_t)rn(r, 512);
	}
	for (int k = 0; k < 2; k++) {
		const int base = k ? G2D_R_BG3PA : G2D_R_BG2PA;
		const int rot = profile == 2 || profile == 7 || chance(r, 40);

		regs[base] = (uint16_t)(rot ? (int)rn(r, 0x500) - 0x280 : 0x100 + (int)rn(r, 0x40) - 0x20);
		regs[base + 1] = (uint16_t)(rot ? (int)rn(r, 0x300) - 0x180 : 0);
		regs[base + 2] = (uint16_t)(rot ? (int)rn(r, 0x300) - 0x180 : 0);
		regs[base + 3] = (uint16_t)(rot ? (int)rn(r, 0x500) - 0x280 : 0x100 + (int)rn(r, 0x40) - 0x20);
		set32(regs, base + 4, chance(r, 50) ? rn(r, 0x40000) : (rnd(r) & 0x0FFFFFFFu));
		set32(regs, base + 6, chance(r, 50) ? rn(r, 0x40000) : (rnd(r) & 0x0FFFFFFFu));
	}
	regs[G2D_R_WIN0H] = (uint16_t)rnd(r);
	regs[G2D_R_WIN1H] = (uint16_t)rnd(r);
	regs[G2D_R_WIN0V] = (uint16_t)rnd(r);
	regs[G2D_R_WIN1V] = (uint16_t)rnd(r);
	if (chance(r, 70)) {
		const uint32_t x1 = rn(r, 200), y1 = rn(r, 150);

		regs[G2D_R_WIN0H] = (uint16_t)((x1 << 8) | (x1 + 10 + rn(r, 60)));
		regs[G2D_R_WIN0V] = (uint16_t)((y1 << 8) | (y1 + 10 + rn(r, 40)));
		regs[G2D_R_WIN1H] = (uint16_t)(((x1 + 30) << 8) | (x1 + 50 + rn(r, 100)));
		regs[G2D_R_WIN1V] = (uint16_t)(((y1 + 20) << 8) | (y1 + 30 + rn(r, 60)));
	}
	regs[G2D_R_WININ] = (uint16_t)(rnd(r) & 0x3F3F);
	regs[G2D_R_WINOUT] = (uint16_t)(rnd(r) & 0x3F3F);
	regs[G2D_R_MOSAIC] = (uint16_t)rnd(r);
	{
		uint32_t bld = rnd(r) & 0x3FFF;

		if (profile == 1 || profile == 7 || chance(r, 30))
			bld |= (1 + rn(r, 3)) << 6;
		if (off & 0x004)
			bld &= 0x3F3F;
		regs[G2D_R_BLDCNT] = (uint16_t)bld;
		regs[G2D_R_BLDALPHA] = (uint16_t)(rn(r, 20) | (rn(r, 20) << 8));
		regs[G2D_R_BLDY] = (uint16_t)rn(r, 20);
	}
	regs[G2D_R_MASTER_BRIGHT] = chance(r, 25) ? (uint16_t)(rn(r, 20) | ((1 + rn(r, 2)) << 14)) : 0;

	for (int i = 0; i < 512; i++)
		wr16le(m->pal[eng] + i * 2, (uint16_t)(rnd(r) & 0x7FFF));
	random_oam(m->oam[eng], r, profile, off);

	/* cambios por línea */
	if (!(off & 0x400)) {
		const uint32_t n = rn(r, 500);
		uint32_t line = 0;

		for (uint32_t i = 0; i < n; i++) {
			const uint32_t k = rn(r, 40);

			line += rn(r, 3);
			if (line >= GPU2D_H)
				break;
			if (k < 6)
				add_change(e, m, eng, (int)line, G2D_R_BGHOFS + 2 * rn(r, 4), rnd(r) & 0x1FF);
			else if (k < 12)
				add_change(e, m, eng, (int)line, G2D_R_BGVOFS + 2 * rn(r, 4), rnd(r) & 0x1FF);
			else if (k < 15)
				add_change(e, m, eng, (int)line, G2D_R_BGCNT + rn(r, 4), rnd(r) & 0xFFFF);
			else if (k < 17) {
				uint32_t d = (dc & 0xFFFFu) ^ (rnd(r) & 0xFF07u);

				if (off & 0x001)
					d &= ~(uint32_t)G2D_DC_OBJ;
				if (off & 0x002)
					d &= ~(uint32_t)(G2D_DC_WIN0 | G2D_DC_WIN1 | G2D_DC_OBJWIN);
				if (off & 0x100)
					d &= ~7u;
				if (off & 0x200)
					d &= ~(uint32_t)G2D_DC_3D;
				add_change(e, m, eng, (int)line, G2D_R_DISPCNT, d);
			} else if (k < 20) {
				add_change(e, m, eng, (int)line, G2D_R_BG2PA + rn(r, 4), rnd(r) & 0xFFFF);
			} else if (k < 23) {
				const uint32_t bx = (rn(r, 2) ? G2D_R_BG2PA : G2D_R_BG3PA) + 4 + rn(r, 4);

				add_change(e, m, eng, (int)line, bx, rnd(r) & 0xFFFF);
			} else if (k < 26) {
				add_change(e, m, eng, (int)line, G2D_R_WIN0H + rn(r, 6), rnd(r) & 0xFFFF);
			} else if (k < 28) {
				add_change(e, m, eng, (int)line, G2D_R_BLDCNT + rn(r, 3), rnd(r) & 0x3FFF);
			} else if (k < 29) {
				add_change(e, m, eng, (int)line, G2D_R_MASTER_BRIGHT, rn(r, 20) | (rn(r, 3) << 14));
			} else if (k < 30) {
				add_change(e, m, eng, (int)line, G2D_R_MOSAIC, rnd(r) & 0xFFFF);
			} else if (k < 36) {
				add_change(e, m, eng, (int)line, G2D_CHG_PAL, (rn(r, 512) << 16) | (rnd(r) & 0x7FFF));
			} else {
				add_change(e, m, eng, (int)line, G2D_CHG_OAM, (rn(r, 512) << 16) | (rnd(r) & 0xFFFF));
			}
		}
	}
}

/* Escenas fijas de medición: fondos de texto de 4 y 8 bits sobre las vistas identidad */
static void fixed_engine(gpu2d_engine *e, const gpu2d_scene_mem *m, int eng, int scene, rng *r)
{
	const int isb = eng == 1;
	const uint32_t bgsize = isb ? 0x20000u : 0x80000u, scr_bases[GPU2D_NBG] = { 8, 12, 16, 24 };
	uint16_t *regs = e->regs;
	uint32_t dc = 1u << 16;

	identity_pages(e, isb);
	for (int i = 0; i < 512; i++)
		wr16le(m->pal[eng] + i * 2, (uint16_t)(rnd(r) & 0x7FFF));
	/* mapas válidos en los bloques de los fondos (el resto de la VRAM ya es ruido) */
	for (int b = 0; b < GPU2D_NBG; b++) {
		const uint32_t o = ((isb ? 0x40000u : 0u) + (scr_bases[b] * 0x800u) % bgsize);

		for (uint32_t i = 0; i < 0x2000u && o + i + 1 < G2D_VRAM_SIZE; i += 2)
			wr16le(m->vram + o + i, (uint16_t)(rnd(r) & 0xFFFF));
	}
	switch (scene) {
	case GPU2D_SCENE_TIPICA: {
		static const uint16_t prio[4] = { 1, 0, 2, 3 }, size[4] = { 0, 1, 0, 3 };

		dc |= G2D_DC_BG(0) | G2D_DC_BG(1) | G2D_DC_BG(2) | G2D_DC_BG(3) | G2D_DC_OBJ | G2D_DC_OBJ_1D;
		for (int b = 0; b < GPU2D_NBG; b++) {
			regs[G2D_R_BGCNT + b] = (uint16_t)(prio[b] | (b * 2) << 2 | scr_bases[b] << 8 | size[b] << 14);
			regs[G2D_R_BGHOFS + 2 * b] = (uint16_t)rn(r, 512);
			regs[G2D_R_BGVOFS + 2 * b] = (uint16_t)rn(r, 512);
		}
		for (int y = 0; y < GPU2D_H; y++)
			add_change(e, m, eng, y, G2D_R_BGHOFS + 2, (uint32_t)(regs[G2D_R_BGHOFS + 2] + tri(y, 48, 6)) & 511u);
		random_oam(m->oam[eng], r, 0, 0xFFF); /* solo OBJ normales */
		break;
	}
	case GPU2D_SCENE_PESADA: {
		static const uint16_t prio[4] = { 0, 1, 1, 2 }, size[4] = { 3, 1, 2, 3 };

		dc |= G2D_DC_BG(0) | G2D_DC_BG(1) | G2D_DC_BG(2) | G2D_DC_BG(3) | G2D_DC_OBJ | G2D_DC_OBJ_1D | (1u << 20);
		for (int b = 0; b < GPU2D_NBG; b++) {
			regs[G2D_R_BGCNT + b] = (uint16_t)(prio[b] | (b * 2) << 2 | ((b & 1) ? 0 : 0x80) | scr_bases[b] << 8 | size[b] << 14);
			regs[G2D_R_BGHOFS + 2 * b] = (uint16_t)rn(r, 512);
			regs[G2D_R_BGVOFS + 2 * b] = (uint16_t)rn(r, 512);
		}
		random_oam(m->oam[eng], r, 0, 0xFFF);
		for (int y = 0; y < GPU2D_H; y++) {
			for (int b = 0; b < GPU2D_NBG; b++)
				add_change(e, m, eng, y, G2D_R_BGHOFS + 2 * b, (uint32_t)(regs[G2D_R_BGHOFS + 2 * b] + tri(y + b * 11, 64, 12)) & 511u);
			if ((y & 3) == 0)
				add_change(e, m, eng, y, G2D_R_BGVOFS, (uint32_t)(regs[G2D_R_BGVOFS] + y / 4) & 511u);
			add_change(e, m, eng, y, G2D_CHG_PAL, (0u << 16) | (uint32_t)((y >> 1) | ((y >> 2) << 5)));
		}
		break;
	}
	case GPU2D_SCENE_EXTREMA: {
		dc |= G2D_DC_BG(0) | G2D_DC_BG(1) | G2D_DC_BG(2) | G2D_DC_BG(3) | G2D_DC_OBJ | G2D_DC_OBJ_1D;
		for (int b = 0; b < GPU2D_NBG; b++)
			regs[G2D_R_BGCNT + b] = (uint16_t)((uint32_t)b | (b * 2) << 2 | 0x80 | scr_bases[b] << 8 | 3u << 14);
		random_oam(m->oam[eng], r, 0, 0xFFF);
		for (int y = 0; y < GPU2D_H; y++) {
			for (int b = 0; b < GPU2D_NBG; b++) {
				add_change(e, m, eng, y, G2D_R_BGVOFS + 2 * b, (uint32_t)((7 * y + b * 5) & 511));
				add_change(e, m, eng, y, G2D_R_BGHOFS + 2 * b, (uint32_t)((8 * y + b * 3) & 511));
			}
			add_change(e, m, eng, y, G2D_CHG_PAL, (0u << 16) | (uint32_t)(y * 131 & 0x7FFF));
			add_change(e, m, eng, y, G2D_CHG_PAL, (300u << 16) | (uint32_t)(y * 17 & 0x7FFF));
		}
		break;
	}
	case GPU2D_SCENE_AFIN: {
		dc = (dc & ~7u) | 2u; /* modo 2: BG2 y BG3 afines */
		dc |= G2D_DC_BG(0) | G2D_DC_BG(1) | G2D_DC_BG(2) | G2D_DC_BG(3) | G2D_DC_OBJ | G2D_DC_OBJ_1D;
		for (int b = 0; b < GPU2D_NBG; b++)
			regs[G2D_R_BGCNT + b] = (uint16_t)((uint32_t)b | (b * 2) << 2 | scr_bases[b] << 8 | (b >= 2 ? 3u << 14 | 0x2000 : 1u << 14));
		regs[G2D_R_BG2PA] = 0x00C0;
		regs[G2D_R_BG2PA + 1] = 0x0060;
		regs[G2D_R_BG2PA + 2] = 0xFFA0;
		regs[G2D_R_BG2PA + 3] = 0x00C0;
		regs[G2D_R_BG3PA] = 0x0180;
		regs[G2D_R_BG3PA + 1] = 0xFF80;
		regs[G2D_R_BG3PA + 2] = 0x0080;
		regs[G2D_R_BG3PA + 3] = 0x0180;
		set32(regs, G2D_R_BG2PA + 4, 0x00020000u);
		set32(regs, G2D_R_BG2PA + 6, 0x00010000u);
		set32(regs, G2D_R_BG3PA + 4, 0x00004000u);
		set32(regs, G2D_R_BG3PA + 6, 0x00008000u);
		random_oam(m->oam[eng], r, 2, 0xFFF & ~0x010);
		for (int y = 0; y < GPU2D_H; y++)
			add_change(e, m, eng, y, G2D_R_BG2PA + 1, (uint32_t)(0x60 + tri(y, 48, 0x40)) & 0xFFFF);
		break;
	}
	default: { /* GPU2D_SCENE_EFECTOS */
		dc |= G2D_DC_BG(0) | G2D_DC_BG(1) | G2D_DC_BG(2) | G2D_DC_BG(3) | G2D_DC_OBJ | G2D_DC_OBJ_1D | G2D_DC_WIN0 | G2D_DC_WIN1 | G2D_DC_OBJWIN;
		for (int b = 0; b < GPU2D_NBG; b++) {
			regs[G2D_R_BGCNT + b] = (uint16_t)((uint32_t)b | (b * 2) << 2 | scr_bases[b] << 8 | 0x40 | (1u << 14));
			regs[G2D_R_BGHOFS + 2 * b] = (uint16_t)rn(r, 512);
		}
		regs[G2D_R_WIN0H] = 0x2090;
		regs[G2D_R_WIN0V] = 0x2070;
		regs[G2D_R_WIN1H] = 0x6030;
		regs[G2D_R_WIN1V] = 0x5090;
		regs[G2D_R_WININ] = 0x3F1F;
		regs[G2D_R_WINOUT] = 0x2F3B;
		regs[G2D_R_MOSAIC] = 0x2233;
		regs[G2D_R_BLDCNT] = 0x0F41;
		regs[G2D_R_BLDALPHA] = 0x0A08;
		random_oam(m->oam[eng], r, 1, 0xFFF & ~0x020 & ~0x040 & ~0x080 & ~0x800);
		break;
	}
	}
	set32(regs, G2D_R_DISPCNT, dc);
}

void gpu2d_scene_build(gpu2d_frame *f, const gpu2d_scene_mem *m, int scene, uint32_t seed)
{
	rng r = { 0x9E3779B9u ^ ((seed & 0x000FFFFFu) * 2654435761u) ^ ((uint32_t)scene * 0x85EBCA6Bu) };

	if (!r.s)
		r.s = 1;
	for (int i = 0; i < 4; i++)
		rnd(&r);
	memset(f, 0, sizeof *f);
	fill_sparse(m->vram, G2D_VRAM_SIZE, &r);
	memset(m->vram + G2D_VRAM_SIZE, 0, G2D_VRAM_ZERO);
	for (uint32_t i = 0; i < GPU2D_H * 512u; i += 4)
		wr32le(m->mmem + i, rnd(&r) & 0x7FFF7FFFu);
	for (int eng = 0; eng < 2; eng++) {
		gpu2d_engine *e = &f->eng[eng];

		e->vram_ea = (uint64_t)(uintptr_t)m->vram;
		e->pal_ea = (uint64_t)(uintptr_t)m->pal[eng];
		e->oam_ea = (uint64_t)(uintptr_t)m->oam[eng];
		e->chg_ea = (uint64_t)(uintptr_t)m->chg[eng];
		e->mmem_ea = eng ? 0 : (uint64_t)(uintptr_t)m->mmem;
		e->flags = eng ? G2D_F_ENGINE_B : 0;
		e->nchg = 0;
		if (scene == GPU2D_SCENE_ALEATORIA)
			random_engine(e, m, eng, &r, seed);
		else
			fixed_engine(e, m, eng, scene, &r);
	}
}
