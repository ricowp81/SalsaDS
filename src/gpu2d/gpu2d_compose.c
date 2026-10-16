/*
 * SalsaDS — compositor 2D por líneas (Fase 0.5). Ver gpu2d_compose.h.
 */
#include "gpu2d/gpu2d_compose.h"

static const uint8_t obj_dims[3][4][2] = {
	{ { 8, 8 }, { 16, 16 }, { 32, 32 }, { 64, 64 } },
	{ { 16, 8 }, { 32, 8 }, { 32, 16 }, { 64, 32 } },
	{ { 8, 16 }, { 8, 32 }, { 16, 32 }, { 32, 64 } },
};

#define TAG_OBJ(p)  (8u + (p))
#define TAG_OUT(p)  (10u + (p))
#define TAG_START   12u

static inline int pow2(uint32_t v)
{
	return v && !(v & (v - 1));
}

/* Trae `size` bytes de una región circular de `mask + 1` bytes desde
 * `off` (alineado a 32; la región, múltiplo de 32). */
static inline void get_wrapped(void *ls, uint64_t base, uint32_t off, uint32_t size, uint32_t mask, uint32_t tag)
{
	off &= mask;
	const uint32_t first = mask + 1 - off;
	if (size <= first) {
		g2d_get(ls, base + off, size, tag);
	} else {
		g2d_get(ls, base + off, first, tag);
		g2d_get((uint8_t *)ls + first, base, size - first, tag);
	}
}

static void decode_oam(gpu2d_ctx *c)
{
	for (int s = 0; s < GPU2D_NOBJ; s++) {
		const uint16_t a0 = g2d_ld16le(c->oamraw + s * 8);
		const uint16_t a1 = g2d_ld16le(c->oamraw + s * 8 + 2);
		const uint16_t a2 = g2d_ld16le(c->oamraw + s * 8 + 4);
		g2d_obj *o = &c->obj[s];
		const uint32_t shape = a0 >> 14;
		o->flags = 0;
		if ((a0 & 0x300) || ((a0 >> 10) & 3) >= 2 || shape == 3)
			continue;
		o->wt = (uint8_t)(obj_dims[shape][a1 >> 14][0] >> 3);
		o->h = obj_dims[shape][a1 >> 14][1];
		o->y = (uint8_t)(a0 & 255);
		int x = a1 & 511;
		if (x >= 256)
			x -= 512;
		o->x = (int16_t)x;
		/* Del todo fuera de la pantalla: no se dibuja nunca. */
		if (x >= GPU2D_W || x + o->wt * 8 <= 0)
			continue;
		o->flags = (uint8_t)(G2D_OBJ_VALID | (((a1 >> 12) & 1) ? G2D_OBJ_HFLIP : 0) |
				     (((a1 >> 13) & 1) ? G2D_OBJ_VFLIP : 0) | (((a0 >> 13) & 1) ? G2D_OBJ_C256 : 0));
		o->prio = (uint8_t)((a2 >> 10) & 3);
		o->bank = (uint8_t)((a2 >> 12) & 15);
		o->tile = a2 & 1023;
	}
}

static void apply_plan_change(gpu2d_ctx *c, const gpu2d_change *ch)
{
	if (ch->reg == GPU2D_REG_DISPCNT)
		c->pr_dispcnt = ch->value;
	else if (ch->reg >= GPU2D_REG_BGCNT && ch->reg < GPU2D_REG_BGCNT + GPU2D_NBG)
		c->pr_bgcnt[ch->reg - GPU2D_REG_BGCNT] = (uint16_t)ch->value;
	else if (ch->reg >= GPU2D_REG_HOFS && ch->reg < GPU2D_REG_HOFS + GPU2D_NBG)
		c->pr_hofs[ch->reg - GPU2D_REG_HOFS] = (uint16_t)ch->value;
	else if (ch->reg >= GPU2D_REG_VOFS && ch->reg < GPU2D_REG_VOFS + GPU2D_NBG)
		c->pr_vofs[ch->reg - GPU2D_REG_VOFS] = (uint16_t)ch->value;
	c->stats.chg_applied++;
}

/* Etapa 1: decide y pide por DMA lo que necesita la línea y. */
static void plan_line(gpu2d_ctx *c, const gpu2d_engine *e, int y)
{
	const uint32_t p = (uint32_t)y & 1;
	g2d_job *j = &c->job[p];
	const uint32_t bg_mask = e->bg_size - 1, obj_mask = e->obj_size - 1;

	while (c->plan_cursor < e->nchg && c->chg[c->plan_cursor].line <= y) {
		if (c->chg[c->plan_cursor].reg != GPU2D_REG_PAL)
			apply_plan_change(c, &c->chg[c->plan_cursor]);
		c->plan_cursor++;
	}
	j->dispcnt = c->pr_dispcnt;
	j->waitmask = 0;
	j->blank = (c->pr_dispcnt & GPU2D_DISPCNT_BLANK) != 0;
	for (int b = 0; b < GPU2D_NBG; b++) {
		j->bg_on[b] = 0;
		j->bgcnt[b] = c->pr_bgcnt[b];
	}
	for (int k = 0; k < 4; k++)
		j->nvis[k] = 0;
	if (j->blank)
		return;

	/* Fondos. Etapa A: elegir ranura y pedir el mapa de los que cambian. */
	uint32_t need = 0, map_wait = 0;
	for (int b = 0; b < GPU2D_NBG; b++) {
		if (!(c->pr_dispcnt & GPU2D_DISPCNT_BG(b)))
			continue;
		const uint32_t cnt = c->pr_bgcnt[b], size = GPU2D_BGCNT_SIZE(cnt);
		const uint32_t wide = size & 1, tall = size >> 1;
		const uint32_t ty = (uint32_t)(y + c->pr_vofs[b]) & (tall ? 511u : 255u);
		const uint32_t hw = c->pr_hofs[b] & (wide ? 511u : 255u);
		const uint32_t row = ty >> 3, tx0 = hw >> 3;
		const uint32_t cbase = GPU2D_DISPCNT_CHAR_OFF(c->pr_dispcnt) + GPU2D_BGCNT_CHAR(cnt);
		const uint32_t sbase = GPU2D_DISPCNT_SCR_OFF(c->pr_dispcnt) + GPU2D_BGCNT_SCR(cnt);
		const uint32_t c256 = GPU2D_BGCNT_256(cnt);
		/* La clave es lo que determina las teselas traídas, sin la fila de
		 * pixeles ni el desplazamiento fino. */
		const uint32_t k0 = tx0 | row << 6 | size << 12 | c256 << 14;
		uint32_t cur = c->bg_cur[b];
		if (c->bg_k0[b][cur] != k0 || c->bg_cbase[b][cur] != cbase || c->bg_sbase[b][cur] != sbase) {
			cur ^= 1;
			c->bg_cur[b] = (uint8_t)cur;
			c->bg_k0[b][cur] = k0;
			c->bg_cbase[b][cur] = cbase;
			c->bg_sbase[b][cur] = sbase;
			/* La fila de 64 bytes del mapa de cada bloque de pantalla que
			 * toca la línea (uno o dos si el fondo es ancho). */
			const uint32_t by = tall ? row >> 5 : 0;
			const uint32_t tag = (uint32_t)b * 2 + cur;
			for (uint32_t bx = 0; bx <= wide; bx++) {
				const uint32_t block = bx + by * (wide ? 2u : 1u);
				const uint32_t off = (sbase + block * 0x800u + (row & 31) * 64u) & bg_mask;
				g2d_get(c->maps[b][cur] + bx * 64, e->bg_vram_ea + off, 64, tag);
			}
			need |= 1u << b;
			map_wait |= 1u << tag;
			c->stats.bg_fetches++;
		}
		j->bg_on[b] = 1;
		j->bg_slot[b] = (uint8_t)cur;
		j->bg_off[b] = (uint8_t)(hw & 7);
		j->bg_py[b] = (uint8_t)(ty & 7);
		j->bg_tx0[b] = (uint8_t)tx0;
		j->bg_wide[b] = (uint8_t)wide;
		j->bg_c256[b] = (uint8_t)c256;
		j->waitmask |= 1u << (b * 2 + cur);
	}
	/* Etapa B: con los mapas en el local store, pedir las teselas. */
	if (need) {
		g2d_wait(map_wait);
		c->stats.map_waits++;
		for (int b = 0; b < GPU2D_NBG; b++) {
			if (!(need & (1u << b)))
				continue;
			const uint32_t cur = j->bg_slot[b], tx0 = j->bg_tx0[b], wide = j->bg_wide[b];
			const uint32_t tsz = j->bg_c256[b] ? 64u : 32u;
			const uint32_t cbase = c->bg_cbase[b][cur];
			for (uint32_t i = 0; i < G2D_TILES; i++) {
				const uint32_t tx = (tx0 + i) & (wide ? 63u : 31u);
				const uint32_t me = g2d_ld16le(c->maps[b][cur] + tx * 2);
				c->offs[i] = (cbase + (me & 1023u) * tsz) & bg_mask;
			}
			g2d_gather(c->tiles[b][cur], e->bg_vram_ea, c->offs, G2D_TILES, tsz, (uint32_t)b * 2 + cur,
				   c->lists[b][cur]);
		}
	}

	/* OBJ: la fila de teselas de cada uno que cruza la línea. */
	if (c->pr_dispcnt & GPU2D_DISPCNT_OBJ) {
		const int one_d = (c->pr_dispcnt & GPU2D_DISPCNT_OBJ_1D) != 0;
		const uint32_t shift = 5 + GPU2D_DISPCNT_OBJ_SHIFT(c->pr_dispcnt);
		for (int s = GPU2D_NOBJ - 1; s >= 0; s--) {
			const g2d_obj *o = &c->obj[s];
			if (!(o->flags & G2D_OBJ_VALID))
				continue;
			const uint32_t dy = (uint32_t)(y - o->y) & 255u;
			if (dy >= o->h)
				continue;
			const uint32_t ty = (o->flags & G2D_OBJ_VFLIP) ? o->h - 1u - dy : dy;
			const uint32_t r = ty >> 3;
			const uint32_t tsz = (o->flags & G2D_OBJ_C256) ? 64u : 32u;
			const uint32_t addr = one_d ? ((uint32_t)o->tile << shift) + r * o->wt * tsz
						    : ((uint32_t)o->tile + r * 32u) * 32u;
			get_wrapped(c->objbuf[p][s], e->obj_vram_ea, addr, o->wt * tsz, obj_mask, TAG_OBJ(p));
			g2d_vis *v = &j->vis[o->prio][j->nvis[o->prio]++];
			v->s = (uint8_t)s;
			v->py = (uint8_t)(ty & 7);
			j->waitmask |= 1u << TAG_OBJ(p);
			c->stats.obj_fetches++;
		}
	}
}

static void draw_bg(gpu2d_ctx *c, const g2d_job *j, int b)
{
	const uint32_t slot = j->bg_slot[b], tx0 = j->bg_tx0[b], py = j->bg_py[b];
	const uint32_t tmask = j->bg_wide[b] ? 63u : 31u;
	const uint8_t *map = c->maps[b][slot], *tiles = c->tiles[b][slot];

	if (j->bg_c256[b]) {
		for (uint32_t i = 0; i < G2D_TILES; i++) {
			const uint32_t me = g2d_ld16le(map + ((tx0 + i) & tmask) * 2);
			const uint32_t row = (me & 0x800) ? 7 - py : py;
			c->tmp[i] = g2d_v8_expand8(tiles + i * 64 + row * 8, 0, (me >> 10) & 1);
		}
	} else {
		for (uint32_t i = 0; i < G2D_TILES; i++) {
			const uint32_t me = g2d_ld16le(map + ((tx0 + i) & tmask) * 2);
			const uint32_t row = (me & 0x800) ? 7 - py : py;
			c->tmp[i] = g2d_v8_expand4(tiles + i * 32 + row * 4, ((me >> 12) & 15) << 4, (me >> 10) & 1);
		}
	}
	const uint32_t off = j->bg_off[b];
	if (off == 0) {
		for (uint32_t v = 0; v < 32; v++)
			c->line[8 + v] = g2d_v8_merge(c->line[8 + v], c->tmp[v]);
	} else {
		for (uint32_t v = 0; v < 32; v++)
			c->line[8 + v] = g2d_v8_merge(c->line[8 + v], g2d_v8_window(c->tmp[v], c->tmp[v + 1], off));
	}
}

static void draw_obj(gpu2d_ctx *c, const g2d_job *j, uint32_t p, const g2d_vis *v)
{
	const g2d_obj *o = &c->obj[v->s];
	const uint8_t *row = c->objbuf[p][v->s];
	const int c256 = (o->flags & G2D_OBJ_C256) != 0, hflip = (o->flags & G2D_OBJ_HFLIP) != 0;
	const uint32_t pos = (uint32_t)(o->x + 64), off = pos & 7, vi = pos >> 3;
	const uint32_t wt = o->wt, tsz = c256 ? 64u : 32u;
	(void)j;
	for (uint32_t k = 0; k < wt; k++) {
		const uint32_t src = hflip ? wt - 1 - k : k;
		g2d_v8 t;
		if (c256)
			t = g2d_v8_expand8(row + src * tsz + v->py * 8, 256, hflip);
		else
			t = g2d_v8_expand4(row + src * tsz + v->py * 4, 256u + (o->bank << 4), hflip);
		if (off == 0) {
			c->line[vi + k] = g2d_v8_merge(c->line[vi + k], t);
		} else {
			g2d_v8 lo, hi;
			g2d_v8_place(t, off, &lo, &hi);
			c->line[vi + k] = g2d_v8_merge(c->line[vi + k], lo);
			c->line[vi + k + 1] = g2d_v8_merge(c->line[vi + k + 1], hi);
		}
	}
	c->stats.obj_drawn++;
}

/* Etapa 2: compone la línea y y la manda a memoria principal. */
static void compose_line(gpu2d_ctx *c, const gpu2d_engine *e, int y)
{
	const uint32_t p = (uint32_t)y & 1;
	const g2d_job *j = &c->job[p];
	uint16_t *out = c->outbuf[p];

	while (c->comp_cursor < e->nchg && c->chg[c->comp_cursor].line <= y) {
		const gpu2d_change *ch = &c->chg[c->comp_cursor++];
		if (ch->reg == GPU2D_REG_PAL)
			c->pal[(ch->value >> 16) & 511] = gpu2d_to_5551((uint16_t)ch->value);
	}
	/* La salida de la línea y - 2 usó este búfer; los fondos y OBJ que
	 * necesita esta línea. */
	g2d_wait((1u << TAG_OUT(p)) | j->waitmask);

	if (j->blank) {
		for (int x = 0; x < GPU2D_W; x++)
			out[x] = GPU2D_BLANK_PIXEL;
	} else {
		const g2d_v8 zero = g2d_v8_splat(0);
		for (int v = 0; v < G2D_LINE_VECS; v++)
			c->line[v] = zero;
		for (int pr = 3; pr >= 0; pr--) {
			for (int b = GPU2D_NBG - 1; b >= 0; b--) {
				if (j->bg_on[b] && (int)GPU2D_BGCNT_PRIO(j->bgcnt[b]) == pr)
					draw_bg(c, j, b);
			}
			for (uint32_t k = 0; k < j->nvis[pr]; k++)
				draw_obj(c, j, p, &j->vis[pr][k]);
		}
		for (int v = 0; v < 32; v++) {
			const g2d_v8 px = c->line[8 + v];
			for (int k = 0; k < 8; k++)
				out[v * 8 + k] = c->pal[g2d_v8_lane(px, k)];
		}
	}
	g2d_put(out, e->out_ea + (uint64_t)y * GPU2D_W * 2, GPU2D_W * 2, TAG_OUT(p));
}

int gpu2d_compose_engine(gpu2d_ctx *c, const gpu2d_engine *e)
{
	if (!pow2(e->bg_size) || !pow2(e->obj_size) || e->bg_size < 0x10000 || e->obj_size < 0x10000)
		return -1;
	if (((e->bg_vram_ea + e->bg_size - 1) >> 32) != (e->bg_vram_ea >> 32) ||
	    ((e->obj_vram_ea + e->obj_size - 1) >> 32) != (e->obj_vram_ea >> 32))
		return -1;
	if (e->nchg > G2D_CHG_LS_CAP)
		return -2;

	memset(&c->stats, 0, sizeof c->stats);
	g2d_get(c->oamraw, e->oam_ea, 1024, TAG_START);
	g2d_get(c->palraw, e->pal_ea, 1024, TAG_START);
	if (e->nchg)
		g2d_get(c->chg, e->chg_ea, (e->nchg * (uint32_t)sizeof(gpu2d_change) + 15u) & ~15u, TAG_START);
	g2d_wait(1u << TAG_START);

	decode_oam(c);
	for (int i = 0; i < 512; i++)
		c->pal[i] = gpu2d_to_5551(g2d_ld16le(c->palraw + i * 2));
	c->pr_dispcnt = e->dispcnt;
	for (int b = 0; b < GPU2D_NBG; b++) {
		c->pr_bgcnt[b] = e->bgcnt[b];
		c->pr_hofs[b] = e->hofs[b];
		c->pr_vofs[b] = e->vofs[b];
		c->bg_cur[b] = 0;
		for (int s = 0; s < 2; s++) {
			c->bg_k0[b][s] = 0xFFFFFFFFu;
			c->bg_cbase[b][s] = 0;
			c->bg_sbase[b][s] = 0;
		}
	}
	c->plan_cursor = c->comp_cursor = 0;

	plan_line(c, e, 0);
	for (int y = 0; y < GPU2D_H; y++) {
		if (y + 1 < GPU2D_H)
			plan_line(c, e, y + 1);
		compose_line(c, e, y);
	}
	g2d_wait((1u << TAG_OUT(0)) | (1u << TAG_OUT(1)));
	return 0;
}
