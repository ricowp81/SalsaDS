/*
 * SalsaDS — composición 2D: referencia píxel por píxel (Fase 0.5).
 *
 * Escrita para ser obvia, no rápida: por cada píxel pregunta a cada capa, en
 * el orden de prioridad de la DS, quién es el primero que no es
 * transparente. No comparte código con gpu2d_compose.c (ni la decodificación
 * del OAM ni el cálculo de direcciones): sirve para verificarlo.
 *
 * Orden, de adelante hacia atrás: por prioridad 0..3, primero los OBJ (el de
 * menor índice delante) y luego los fondos (el de menor número delante); al
 * final el color de fondo (la entrada 0 de la paleta de fondos). Un píxel
 * es transparente si su número de color es 0.
 */
#include <string.h>

#include "gpu2d/gpu2d.h"
#include "util/endian.h"

typedef struct {
	uint32_t dispcnt;
	uint16_t bgcnt[GPU2D_NBG], hofs[GPU2D_NBG], vofs[GPU2D_NBG];
	uint16_t pal[512];
} ref_regs;

typedef struct {
	int x, w, h, ys, hflip, vflip, c256, prio, palbank;
	uint32_t tile;
} ref_obj;

static const uint8_t obj_dims[3][4][2] = {
	{ { 8, 8 }, { 16, 16 }, { 32, 32 }, { 64, 64 } },
	{ { 16, 8 }, { 32, 8 }, { 32, 16 }, { 64, 32 } },
	{ { 8, 16 }, { 8, 32 }, { 16, 32 }, { 32, 64 } },
};

static void apply_change(ref_regs *r, const gpu2d_change *c)
{
	if (c->reg == GPU2D_REG_DISPCNT)
		r->dispcnt = c->value;
	else if (c->reg >= GPU2D_REG_BGCNT && c->reg < GPU2D_REG_BGCNT + GPU2D_NBG)
		r->bgcnt[c->reg - GPU2D_REG_BGCNT] = (uint16_t)c->value;
	else if (c->reg >= GPU2D_REG_HOFS && c->reg < GPU2D_REG_HOFS + GPU2D_NBG)
		r->hofs[c->reg - GPU2D_REG_HOFS] = (uint16_t)c->value;
	else if (c->reg >= GPU2D_REG_VOFS && c->reg < GPU2D_REG_VOFS + GPU2D_NBG)
		r->vofs[c->reg - GPU2D_REG_VOFS] = (uint16_t)c->value;
	else if (c->reg == GPU2D_REG_PAL)
		r->pal[(c->value >> 16) & 511] = (uint16_t)c->value;
}

/* Índice de paleta (0..511) del fondo b en (x, y) o -1 si es transparente. */
static int bg_pixel(const gpu2d_engine *e, const ref_regs *r, int b, int x, int y)
{
	const uint8_t *vram = (const uint8_t *)(uintptr_t)e->bg_vram_ea;
	const uint32_t mask = e->bg_size - 1;
	const uint32_t cnt = r->bgcnt[b], size = GPU2D_BGCNT_SIZE(cnt);
	const int wide = size & 1, tall = size >> 1;
	const int px = (x + r->hofs[b]) & (wide ? 511 : 255);
	const int py = (y + r->vofs[b]) & (tall ? 511 : 255);
	const int tx = px >> 3, ty = py >> 3;
	/* Bloque de pantalla de 32×32 teselas: los bloques de un fondo ancho
	 * van uno junto al otro; los de uno alto, uno debajo del otro. */
	const uint32_t block = (uint32_t)(tx >> 5) + (uint32_t)(ty >> 5) * (wide ? 2u : 1u);
	const uint32_t map = GPU2D_DISPCNT_SCR_OFF(r->dispcnt) + GPU2D_BGCNT_SCR(cnt) + block * 0x800u +
			     (uint32_t)((ty & 31) * 32 + (tx & 31)) * 2u;
	const uint16_t me = rd16le(vram + (map & mask));
	int ix = px & 7, iy = py & 7;
	if (me & 0x400)
		ix = 7 - ix;
	if (me & 0x800)
		iy = 7 - iy;
	const uint32_t cbase = GPU2D_DISPCNT_CHAR_OFF(r->dispcnt) + GPU2D_BGCNT_CHAR(cnt);
	int c, idx;
	if (GPU2D_BGCNT_256(cnt)) {
		c = vram[(cbase + (uint32_t)(me & 1023) * 64u + (uint32_t)(iy * 8 + ix)) & mask];
		idx = c;
	} else {
		const uint8_t byte = vram[(cbase + (uint32_t)(me & 1023) * 32u + (uint32_t)(iy * 4 + (ix >> 1))) & mask];
		c = (ix & 1) ? byte >> 4 : byte & 15;
		idx = ((me >> 12) & 15) * 16 + c;
	}
	return c ? idx : -1;
}

static int decode_oam(const gpu2d_engine *e, const ref_regs *r, int line, ref_obj *out)
{
	const uint8_t *oam = (const uint8_t *)(uintptr_t)e->oam_ea;
	int n = 0;
	for (int s = 0; s < GPU2D_NOBJ; s++) {
		const uint16_t a0 = rd16le(oam + s * 8), a1 = rd16le(oam + s * 8 + 2), a2 = rd16le(oam + s * 8 + 4);
		const int shape = a0 >> 14;
		if ((a0 & 0x100) || (a0 & 0x200) || ((a0 >> 10) & 3) >= 2 || shape == 3)
			continue;
		ref_obj o = { 0 };
		o.w = obj_dims[shape][a1 >> 14][0];
		o.h = obj_dims[shape][a1 >> 14][1];
		o.ys = a0 & 255;
		if (((line - o.ys) & 255) >= o.h)
			continue;
		o.x = a1 & 511;
		if (o.x >= 256)
			o.x -= 512;
		o.hflip = (a1 >> 12) & 1;
		o.vflip = (a1 >> 13) & 1;
		o.c256 = (a0 >> 13) & 1;
		o.tile = a2 & 1023;
		o.prio = (a2 >> 10) & 3;
		o.palbank = (a2 >> 12) & 15;
		out[s] = o;
		n++;
	}
	(void)r;
	return n;
}

static int obj_pixel(const gpu2d_engine *e, const ref_regs *r, const ref_obj *o, int x, int y)
{
	const uint8_t *vram = (const uint8_t *)(uintptr_t)e->obj_vram_ea;
	const uint32_t mask = e->obj_size - 1;
	int dx = x - o->x;
	int dy = (y - o->ys) & 255;
	if (dx < 0 || dx >= o->w)
		return -1;
	if (o->hflip)
		dx = o->w - 1 - dx;
	if (o->vflip)
		dy = o->h - 1 - dy;
	const int tx = dx >> 3, ty = dy >> 3, ix = dx & 7, iy = dy & 7, wt = o->w >> 3;
	const uint32_t tsz = o->c256 ? 64u : 32u;
	uint32_t addr;
	if (r->dispcnt & GPU2D_DISPCNT_OBJ_1D)
		addr = (o->tile << (5 + GPU2D_DISPCNT_OBJ_SHIFT(r->dispcnt))) + (uint32_t)(ty * wt + tx) * tsz;
	else
		addr = (o->tile + (uint32_t)ty * 32u + (uint32_t)tx * (o->c256 ? 2u : 1u)) * 32u;
	int c;
	if (o->c256) {
		c = vram[(addr + (uint32_t)(iy * 8 + ix)) & mask];
		return c ? 256 + c : -1;
	}
	const uint8_t byte = vram[(addr + (uint32_t)(iy * 4 + (ix >> 1))) & mask];
	c = (ix & 1) ? byte >> 4 : byte & 15;
	return c ? 256 + o->palbank * 16 + c : -1;
}

void gpu2d_ref_engine(const gpu2d_engine *e, uint16_t *out)
{
	ref_regs r;
	const uint8_t *pal = (const uint8_t *)(uintptr_t)e->pal_ea;
	const gpu2d_change *chg = (const gpu2d_change *)(uintptr_t)e->chg_ea;
	uint32_t next = 0;

	r.dispcnt = e->dispcnt;
	memcpy(r.bgcnt, e->bgcnt, sizeof r.bgcnt);
	memcpy(r.hofs, e->hofs, sizeof r.hofs);
	memcpy(r.vofs, e->vofs, sizeof r.vofs);
	for (int i = 0; i < 512; i++)
		r.pal[i] = rd16le(pal + i * 2);

	for (int y = 0; y < GPU2D_H; y++) {
		while (next < e->nchg && chg[next].line <= y)
			apply_change(&r, &chg[next++]);
		uint16_t *line = out + y * GPU2D_W;
		if (r.dispcnt & GPU2D_DISPCNT_BLANK) {
			for (int x = 0; x < GPU2D_W; x++)
				line[x] = GPU2D_BLANK_PIXEL;
			continue;
		}
		ref_obj objs[GPU2D_NOBJ];
		int visible[GPU2D_NOBJ], nvis = 0;
		if (r.dispcnt & GPU2D_DISPCNT_OBJ) {
			ref_obj tmp[GPU2D_NOBJ];
			memset(tmp, 0, sizeof tmp);
			decode_oam(e, &r, y, tmp);
			for (int s = 0; s < GPU2D_NOBJ; s++) {
				if (tmp[s].w) {
					objs[s] = tmp[s];
					visible[nvis++] = s;
				}
			}
		}
		for (int x = 0; x < GPU2D_W; x++) {
			int idx = 0;  /* color de fondo */
			int found = 0;
			for (int p = 0; p < 4 && !found; p++) {
				for (int k = 0; k < nvis && !found; k++) {
					const ref_obj *o = &objs[visible[k]];
					if (o->prio != p)
						continue;
					const int v = obj_pixel(e, &r, o, x, y);
					if (v >= 0) {
						idx = v;
						found = 1;
					}
				}
				for (int b = 0; b < GPU2D_NBG && !found; b++) {
					if (!(r.dispcnt & GPU2D_DISPCNT_BG(b)) || (int)GPU2D_BGCNT_PRIO(r.bgcnt[b]) != p)
						continue;
					const int v = bg_pixel(e, &r, b, x, y);
					if (v >= 0) {
						idx = v;
						found = 1;
					}
				}
			}
			line[x] = gpu2d_to_5551(r.pal[idx]);
		}
	}
}

uint64_t gpu2d_hash_image(const uint16_t *img)
{
	uint64_t h = 1469598103934665603ull;
	for (int i = 0; i < GPU2D_W * GPU2D_H; i++) {
		h ^= img[i];
		h *= 1099511628211ull;
	}
	return h;
}
