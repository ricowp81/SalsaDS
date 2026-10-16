/*
 * SalsaDS — escenas sintéticas para la composición 2D (Fase 0.5).
 *
 * Todo sale de un generador xorshift y de aritmética entera: la misma escena
 * en la Mac (LE), en qemu-ppc64 (BE) y en la PS3. Las escenas no pretenden
 * parecerse a un juego: fijan cuánto trabajo de DMA y de composición pide
 * cada pantalla.
 */
#include <string.h>

#include "gpu2d/gpu2d.h"
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

static uint32_t rnd_n(rng *r, uint32_t n)
{
	return rnd(r) % n;
}

uint32_t gpu2d_scene_bg_size(int engine)
{
	return engine ? 128u * 1024 : 512u * 1024;
}

uint32_t gpu2d_scene_obj_size(int engine)
{
	return engine ? 128u * 1024 : 256u * 1024;
}

/* Triángulo de periodo `period` y amplitud ±amp, en enteros. */
static int tri(int i, int period, int amp)
{
	const int h = period / 2;
	const int m = ((i % period) + period) % period;
	const int d = m < h ? m : period - m;  /* 0..h */
	return (d * 2 * amp) / h - amp;
}

/* Bytes de teselas: ~40 % de píxeles transparentes (número de color 0) en
 * 4 y en 8 bits, para que los volteos y las prioridades se noten. */
static void fill_tiles(uint8_t *p, uint32_t n, rng *r)
{
	for (uint32_t i = 0; i < n; i++) {
		const uint32_t v = rnd(r);
		uint32_t b = v & 255;
		if (((v >> 8) % 5) < 2)
			b = 0;
		else {
			if (((v >> 12) & 3) == 0)
				b &= 0xF0;
			if (((v >> 14) & 3) == 0)
				b &= 0x0F;
		}
		p[i] = (uint8_t)b;
	}
}

static void fill_maps(uint8_t *p, uint32_t n, rng *r)
{
	for (uint32_t i = 0; i + 1 < n; i += 2) {
		const uint32_t v = rnd(r);
		wr16le(p + i, (uint16_t)(v & 0xFFFF));
	}
}

static void fill_pal(uint8_t *pal, rng *r)
{
	for (int i = 0; i < 512; i++)
		wr16le(pal + i * 2, (uint16_t)(rnd(r) & 0x7FFF));
}

static void put_oam(uint8_t *oam, int s, uint32_t shape, uint32_t size, int x, int y, uint32_t tile, uint32_t prio,
		    uint32_t bank, int c256, int hflip, int vflip)
{
	const uint16_t a0 = (uint16_t)((y & 255) | (c256 << 13) | (shape << 14));
	const uint16_t a1 = (uint16_t)((x & 511) | (hflip << 12) | (vflip << 13) | (size << 14));
	const uint16_t a2 = (uint16_t)((tile & 1023) | (prio << 10) | (bank << 12));
	wr16le(oam + s * 8, a0);
	wr16le(oam + s * 8 + 2, a1);
	wr16le(oam + s * 8 + 4, a2);
}

static void add_change(gpu2d_engine *e, const gpu2d_scene_mem *m, int line, int reg, uint32_t value)
{
	if (e->nchg >= m->chg_cap)
		return;
	gpu2d_change *c = &m->chg[e->nchg++];
	c->line = (uint16_t)line;
	c->reg = (uint16_t)reg;
	c->value = value;
}

static void build_sprites(const gpu2d_scene_mem *m, rng *r, int count, int big_only, int c256_mode, uint32_t tile_range,
			  int shift)
{
	memset(m->oam, 0, 1024);
	/* Los que no se usan: desactivados (bit 9). */
	for (int s = 0; s < GPU2D_NOBJ; s++)
		wr16le(m->oam + s * 8, 0x200);
	for (int s = 0; s < count; s++) {
		const uint32_t shape = big_only ? rnd_n(r, 3) : rnd_n(r, 3);
		const uint32_t size = big_only ? 3 : rnd_n(r, 4);
		const int c256 = c256_mode == 2 ? (int)(rnd(r) & 1) : c256_mode;
		const int x = (int)rnd_n(r, 300) - 40;
		const int y = (int)rnd_n(r, 230) - 20;
		put_oam(m->oam, s, shape, size, x, y, rnd_n(r, tile_range) << (c256 ? 1 : 0), rnd_n(r, 4), rnd_n(r, 16),
			c256, (int)(rnd(r) & 1), (int)(rnd(r) & 1));
	}
	(void)shift;
}

void gpu2d_scene_build(gpu2d_engine *e, const gpu2d_scene_mem *m, int engine, int scene, uint32_t seed)
{
	rng r = { 0x9E3779B9u ^ (seed * 2654435761u) ^ (uint32_t)(engine * 0x01000193u) ^ (uint32_t)(scene * 0x85EBCA6Bu) };
	if (!r.s)
		r.s = 1;
	memset(e, 0, sizeof *e);
	e->bg_vram_ea = (uint64_t)(uintptr_t)m->bg_vram;
	e->obj_vram_ea = (uint64_t)(uintptr_t)m->obj_vram;
	e->pal_ea = (uint64_t)(uintptr_t)m->pal;
	e->oam_ea = (uint64_t)(uintptr_t)m->oam;
	e->chg_ea = (uint64_t)(uintptr_t)m->chg;
	e->bg_size = m->bg_size;
	e->obj_size = m->obj_size;

	fill_tiles(m->bg_vram, m->bg_size, &r);
	fill_tiles(m->obj_vram, m->obj_size, &r);
	fill_pal(m->pal, &r);
	/* Mapas: las pantallas de los fondos se llenan de entradas válidas
	 * (la VRAM entera es aleatoria, así que los mapas ya lo son; se vuelven
	 * a escribir los bloques usados para que las teselas sean variadas). */
	const uint32_t scr_bases[GPU2D_NBG] = { 8, 12, 16, 24 };  /* en bloques de 2 KB */
	for (int b = 0; b < GPU2D_NBG; b++) {
		const uint32_t off = (scr_bases[b] * 0x800u) & (m->bg_size - 1);
		const uint32_t n = 0x2000u;  /* hasta 4 bloques */
		if (off + n <= m->bg_size)
			fill_maps(m->bg_vram + off, n, &r);
	}

	switch (scene) {
	case GPU2D_SCENE_TIPICA: {
		e->dispcnt = GPU2D_DISPCNT_BG(0) | GPU2D_DISPCNT_BG(1) | GPU2D_DISPCNT_BG(2) | GPU2D_DISPCNT_BG(3) |
			     GPU2D_DISPCNT_OBJ | GPU2D_DISPCNT_OBJ_1D;
		const uint16_t prio[4] = { 1, 0, 2, 3 };
		const uint16_t size[4] = { 0, 1, 0, 3 };
		for (int b = 0; b < GPU2D_NBG; b++) {
			e->bgcnt[b] = (uint16_t)(prio[b] | (b * 2) << 2 | (scr_bases[b]) << 8 | size[b] << 14);
			e->hofs[b] = (uint16_t)(rnd_n(&r, 512));
			e->vofs[b] = (uint16_t)(rnd_n(&r, 512));
		}
		build_sprites(m, &r, 40, 0, 0, 512, 0);
		for (int y = 0; y < GPU2D_H; y++)
			add_change(e, m, y, GPU2D_REG_HOFS + 1, (uint32_t)(e->hofs[1] + tri(y, 48, 6)) & 511u);
		break;
	}
	case GPU2D_SCENE_PESADA: {
		e->dispcnt = GPU2D_DISPCNT_BG(0) | GPU2D_DISPCNT_BG(1) | GPU2D_DISPCNT_BG(2) | GPU2D_DISPCNT_BG(3) |
			     GPU2D_DISPCNT_OBJ | GPU2D_DISPCNT_OBJ_1D | (1u << 20);
		const uint16_t prio[4] = { 0, 1, 1, 2 };
		const uint16_t size[4] = { 3, 1, 2, 3 };
		for (int b = 0; b < GPU2D_NBG; b++) {
			const uint16_t c256 = (b & 1) ? 0x80 : 0;
			e->bgcnt[b] = (uint16_t)(prio[b] | (b * 2) << 2 | c256 | (scr_bases[b]) << 8 | size[b] << 14);
			e->hofs[b] = (uint16_t)(rnd_n(&r, 512));
			e->vofs[b] = (uint16_t)(rnd_n(&r, 512));
		}
		build_sprites(m, &r, 128, 0, 2, 256, 1);
		for (int y = 0; y < GPU2D_H; y++) {
			for (int b = 0; b < GPU2D_NBG; b++)
				add_change(e, m, y, GPU2D_REG_HOFS + b, (uint32_t)(e->hofs[b] + tri(y + b * 11, 64, 12)) & 511u);
			if ((y & 3) == 0)
				add_change(e, m, y, GPU2D_REG_VOFS, (uint32_t)(e->vofs[0] + y / 4) & 511u);
			add_change(e, m, y, GPU2D_REG_PAL, (0u << 16) | (uint32_t)((y >> 1) | ((y >> 2) << 5)));
		}
		break;
	}
	case GPU2D_SCENE_EXTREMA: {
		e->dispcnt = GPU2D_DISPCNT_BG(0) | GPU2D_DISPCNT_BG(1) | GPU2D_DISPCNT_BG(2) | GPU2D_DISPCNT_BG(3) |
			     GPU2D_DISPCNT_OBJ | GPU2D_DISPCNT_OBJ_1D;
		for (int b = 0; b < GPU2D_NBG; b++) {
			e->bgcnt[b] = (uint16_t)((uint32_t)b | (b * 2) << 2 | 0x80 | (scr_bases[b]) << 8 | 3u << 14);
			e->hofs[b] = 0;
			e->vofs[b] = 0;
		}
		build_sprites(m, &r, 128, 1, 1, 128, 0);
		for (int y = 0; y < GPU2D_H; y++) {
			for (int b = 0; b < GPU2D_NBG; b++) {
				/* La fila de teselas cambia en cada línea (y × 8 + b) y el
				 * desplazamiento grueso, también. */
				add_change(e, m, y, GPU2D_REG_VOFS + b, (uint32_t)((7 * y + b * 5) & 511));
				add_change(e, m, y, GPU2D_REG_HOFS + b, (uint32_t)((8 * y + b * 3) & 511));
			}
			add_change(e, m, y, GPU2D_REG_PAL, (0u << 16) | (uint32_t)(y * 131 & 0x7FFF));
			add_change(e, m, y, GPU2D_REG_PAL, (300u << 16) | (uint32_t)(y * 17 & 0x7FFF));
		}
		break;
	}
	default: {  /* GPU2D_SCENE_ALEATORIA */
		static const uint8_t shapes[4] = { 0, 1, 2, 3 };
		(void)shapes;
		e->dispcnt = (uint32_t)(rnd(&r) & (0xFu << 8 | 1u << 12 | 1u << 4 | 3u << 20 | 7u << 24 | 7u << 27 | 1u << 5 | 1u << 6));
		if (rnd_n(&r, 16) == 0)
			e->dispcnt |= GPU2D_DISPCNT_BLANK;
		for (int b = 0; b < GPU2D_NBG; b++) {
			e->bgcnt[b] = (uint16_t)rnd(&r);
			e->hofs[b] = (uint16_t)(rnd(&r) & 0x1FF);
			e->vofs[b] = (uint16_t)(rnd(&r) & 0x1FF);
		}
		/* OAM: de todo, incluidos los afines, desactivados, formas
		 * inválidas y modos que no se dibujan. */
		for (int s = 0; s < GPU2D_NOBJ; s++) {
			const uint32_t v = rnd(&r);
			wr16le(m->oam + s * 8, (uint16_t)(((v & 0xFFFF) & ~0x300u) | (rnd_n(&r, 12) == 0 ? 0x100u : 0) |
							    (rnd_n(&r, 12) == 0 ? 0x200u : 0)));
			wr16le(m->oam + s * 8 + 2, (uint16_t)(rnd(&r) & 0xFFFF));
			wr16le(m->oam + s * 8 + 4, (uint16_t)(rnd(&r) & 0xFFFF));
			wr16le(m->oam + s * 8 + 6, (uint16_t)(rnd(&r) & 0xFFFF));
		}
		const uint32_t nchg = rnd_n(&r, 400);
		uint32_t line = 0;
		for (uint32_t i = 0; i < nchg; i++) {
			line += rnd_n(&r, 3);
			if (line >= GPU2D_H)
				break;
			const uint32_t k = rnd_n(&r, 20);
			if (k < 4)
				add_change(e, m, (int)line, GPU2D_REG_HOFS + (int)rnd_n(&r, 4), rnd(&r) & 0x1FF);
			else if (k < 8)
				add_change(e, m, (int)line, GPU2D_REG_VOFS + (int)rnd_n(&r, 4), rnd(&r) & 0x1FF);
			else if (k < 11)
				add_change(e, m, (int)line, GPU2D_REG_BGCNT + (int)rnd_n(&r, 4), rnd(&r) & 0xFFFF);
			else if (k < 14)
				add_change(e, m, (int)line, GPU2D_REG_DISPCNT,
					   (uint32_t)(rnd(&r) & (0xFu << 8 | 1u << 12 | 1u << 4 | 3u << 20 | 7u << 24 | 7u << 27)) |
						   (rnd_n(&r, 24) == 0 ? GPU2D_DISPCNT_BLANK : 0));
			else
				add_change(e, m, (int)line, GPU2D_REG_PAL, (rnd_n(&r, 512) << 16) | (rnd(&r) & 0xFFFF));
		}
		break;
	}
	}
}
