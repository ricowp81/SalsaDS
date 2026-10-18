/*
 * SalsaDS — compositor 2D por líneas (Fase 2.1). Ver gpu2d_compose.h.
 *
 * Mismo comportamiento que la referencia (gpu2d_ref.c), distinto método: en vez de preguntar píxel por píxel, trae por
 * DMA lo que cada línea necesita, dibuja cada capa como un vector de índices de la paleta unificada y las combina con
 * selecciones SIMD. Lo raro (fondos y OBJ afines, mosaico, efectos de color por píxel) va con bucles escalares.
 */
#include "gpu2d/gpu2d_compose.h"
#include "gpu2d/gpu2d_fx.h"
#ifndef __SPU__
uint64_t g2d_dma_cnt[G2D_ST_COUNT][G2D_C_COUNT];
int g2d_stage;
#endif
#ifdef G2D_DBG
#include <stdio.h>
int g2d_dbg_x = -1, g2d_dbg_y = -1, g2d_dbg_cur;

#endif

static const uint8_t obj_dims[3][4][2] = {
	{ { 8, 8 }, { 16, 16 }, { 32, 32 }, { 64, 64 } },
	{ { 16, 8 }, { 32, 8 }, { 32, 16 }, { 64, 32 } },
	{ { 8, 16 }, { 8, 32 }, { 16, 32 }, { 32, 64 } },
};

enum { BG_OFF, BG_TEXT, BG_AFFINE, BG_EXT16, BG_BMP8, BG_BMPD, BG_LARGE, BG_3D };

static inline int pow2(uint32_t v)
{
	return v && !(v & (v - 1));
}

/* Espera del DMA dentro de la etapa 1 (para el perfil por etapa). */
#define PW(c, mask)                                  \
	do {                                         \
		const uint32_t tw_ = G2D_T();        \
		g2d_wait(mask);                      \
		G2D_ACC(c, plan_wait, tw_);          \
	} while (0)

/* ---- vistas de la VRAM ------------------------------------------------------------------------------- */

/* Desplazamiento, dentro de la instantánea, de la posición `off` de la vista de fondos (obj = 0) o de OBJ (obj = 1);
 * G2D_VRAM_SIZE (el bloque de ceros) si la página no está mapeada. */
static inline uint32_t view_off(const gpu2d_engine *e, int obj, uint32_t off)
{
	const uint32_t pages = obj ? e->objpages : e->bgpages;
	uint32_t base;

	off &= pages * 0x4000u - 1u;
	base = (obj ? e->objpg : e->bgpg)[off >> 14];
	return base == G2D_UNMAPPED ? G2D_VRAM_SIZE : base + (off & 0x3FFFu);
}

static inline uint32_t view_mask(const gpu2d_engine *e, int obj)
{
	return (obj ? e->objpages : e->bgpages) * 0x4000u - 1u;
}

/* Trae `size` bytes (múltiplo de 16) que empiezan en `off` (múltiplo de 16) de una vista; parte el DMA en las páginas
 * y rellena de ceros lo que cae en páginas sin mapear. */
static void get_run(void *ls, const gpu2d_engine *e, int obj, uint32_t off, uint32_t size, uint32_t tag)
{
	uint8_t *dst = (uint8_t *)ls;
	const uint32_t mask = view_mask(e, obj);

	while (size) {
		const uint32_t o = off & mask, pg = o & 0x3FFFu;
		uint32_t n = 0x4000u - pg;
		const uint32_t base = (obj ? e->objpg : e->bgpg)[o >> 14];

		if (n > size)
			n = size;
		if (base == G2D_UNMAPPED)
			memset(dst, 0, n);
		else
			g2d_get(dst, e->vram_ea + base + pg, n, tag);
		dst += n;
		off += n;
		size -= n;
	}
}

/* ---- OAM ---------------------------------------------------------------------------------------------- */

static void decode_one(gpu2d_ctx *c, int s)
{
	const uint16_t a0 = g2d_ld16le(c->oamraw + s * 8), a1 = g2d_ld16le(c->oamraw + s * 8 + 2),
		       a2 = g2d_ld16le(c->oamraw + s * 8 + 4);
	g2d_obj *o = &c->obj[s];
	const int aff = (a0 >> 8) & 1, shape = a0 >> 14, size = a1 >> 14;
	int x;

	o->flags = 0;
	if (!aff && (a0 & 0x200))
		return;
	if (((a0 >> 10) & 3) == 3 && (a2 >> 12) == 0) /* mapa de bits con alfa 0: no se dibuja (melonDS 1.1) */
		return;
	o->w = (uint8_t)(shape == 3 ? 8 : obj_dims[shape][size][0]);
	o->h = (uint8_t)(shape == 3 ? 8 : obj_dims[shape][size][1]);
	x = a1 & 511;
	if (x >= 256)
		x -= 512;
	o->x = (int16_t)x;
	o->y = (uint8_t)(a0 & 255);
	{
		const int dbl = aff && (a0 & 0x200);
		const int bw = o->w << dbl;

		/* del todo fuera de la pantalla: no se dibuja nunca */
		if (x + bw <= 0)
			return;
	}
	o->mode = (uint8_t)((a0 >> 10) & 3);
	o->prio = (uint8_t)((a2 >> 10) & 3);
	o->tile = a2 & 1023;
	o->pal = (uint8_t)(a2 >> 12);
	o->group = (uint8_t)((a1 >> 9) & 31);
	o->flags = (uint8_t)(G2D_OBJ_VALID | (aff ? G2D_OBJ_AFF : 0) | ((aff && (a0 & 0x200)) ? G2D_OBJ_DBL : 0) |
			     (((a0 >> 12) & 1) ? G2D_OBJ_MOS : 0) | (((a0 >> 13) & 1) ? G2D_OBJ_C256 : 0) |
			     (!aff && ((a1 >> 12) & 1) ? G2D_OBJ_HFLIP : 0) | (!aff && ((a1 >> 13) & 1) ? G2D_OBJ_VFLIP : 0));
}

/* ---- registro de cambios (por trozos) ---------------------------------------------------------------- */

static void chg_fetch(gpu2d_ctx *c, const gpu2d_engine *e, uint32_t k)
{
	const uint32_t first = k * G2D_CHG_CHUNK;
	uint32_t n;

	if (first >= e->nchg)
		return;
	n = e->nchg - first;
	if (n > G2D_CHG_CHUNK)
		n = G2D_CHG_CHUNK;
	g2d_get(c->chg[k & 1], e->chg_ea + (uint64_t)first * sizeof(gpu2d_change), (n * (uint32_t)sizeof(gpu2d_change) + 15u) & ~15u,
		G2D_TAG_CHG + (k & 1));
	c->chg_fetched = k + 1;
}

static const gpu2d_change *chg_peek(gpu2d_ctx *c, const gpu2d_engine *e, int line)
{
	const gpu2d_change *ch;
	const uint32_t k = c->chg_next / G2D_CHG_CHUNK;

	if (c->chg_next >= e->nchg)
		return 0;
	if (k >= c->chg_fetched)
		chg_fetch(c, e, k);
	g2d_wait(1u << (G2D_TAG_CHG + (k & 1)));
	ch = &c->chg[k & 1][c->chg_next % G2D_CHG_CHUNK];
	return ch->line <= line ? ch : 0;
}

static void chg_pop(gpu2d_ctx *c, const gpu2d_engine *e)
{
	c->chg_next++;
	if (c->chg_next % G2D_CHG_CHUNK == 0 && c->chg_next < e->nchg)
		chg_fetch(c, e, c->chg_next / G2D_CHG_CHUNK + 1);
}

static int32_t sext28(uint32_t v)
{
	return (int32_t)(v << 4) >> 4;
}

static void reload_ref(gpu2d_ctx *c, int b)
{
	const int base = b ? G2D_R_BG3PA : G2D_R_BG2PA;

	c->ref[b][0] = sext28((uint32_t)c->regs[base + 4] | ((uint32_t)c->regs[base + 5] << 16));
	c->ref[b][1] = sext28((uint32_t)c->regs[base + 6] | ((uint32_t)c->regs[base + 7] << 16));
}

/* ---- ventanas verticales -------------------------------------------------------------------------------- */

static void win_step_v(uint16_t *regs, uint8_t *winv, int i, int line)
{
	const uint16_t reg = regs[G2D_R_WIN0V + i];
	const int y1 = reg >> 8, y2 = reg & 255;

	if ((line & 255) == y2)
		winv[i] = 0;
	if ((line & 255) == y1)
		winv[i] = 1;
}

/* ---- fondos: tipo ----------------------------------------------------------------------------------------- */

static int bg_type(const gpu2d_engine *e, uint32_t dc, const uint16_t *regs, int b)
{
	const int isb = (e->flags & G2D_F_ENGINE_B) != 0;
	const uint32_t mode = G2D_DC_BGMODE(dc);
	const uint32_t cnt = regs[G2D_R_BGCNT + b];
	int kind;

	if (!(dc & G2D_DC_BG(b)))
		return BG_OFF;
	if (b == 0 && !isb && ((dc & G2D_DC_3D) || mode == 6))
		return BG_3D;
	if (b < 2)
		return mode >= 6 ? BG_OFF : BG_TEXT;
	if (mode == 6)
		return (b == 2 && !isb) ? BG_LARGE : BG_OFF;
	if (mode == 7)
		return BG_OFF;
	if (b == 2) {
		static const uint8_t k2[6] = { 0, 0, 1, 0, 1, 2 };
		kind = k2[mode];
	} else {
		static const uint8_t k3[6] = { 0, 1, 1, 2, 2, 2 };
		kind = k3[mode];
	}
	if (kind == 0)
		return BG_TEXT;
	if (kind == 1)
		return BG_AFFINE;
	if (!(cnt & 0x80u))
		return BG_EXT16;
	return (cnt & 4u) ? BG_BMPD : BG_BMP8;
}

/* ---- paletas ---------------------------------------------------------------------------------------------- */

static void ext_pal_load(gpu2d_ctx *c, const gpu2d_engine *e)
{
	uint32_t pending = 0;

	G2D_STAGE(G2D_ST_EXTPAL);
	for (int k = 0; k < 5; k++) {
		const uint32_t src = k < 4 ? e->extbg[k] : e->extobj;
		uint16_t *dst = c->pal + (k < 4 ? G2D_PAL_BGEXT + k * 4096 : G2D_PAL_OBJEXT);

		if (src == G2D_UNMAPPED) {
			memset(dst, 0, 4096 * sizeof(uint16_t));
		} else {
			g2d_get(dst, e->vram_ea + src, 8192, G2D_TAG_EXT);
			pending = 1;
		}
	}
	if (pending)
		PW(c, 1u << G2D_TAG_EXT);
	for (int k = 0; k < 5; k++) {
		uint16_t *dst = c->pal + (k < 4 ? G2D_PAL_BGEXT + k * 4096 : G2D_PAL_OBJEXT);
		const uint32_t src = k < 4 ? e->extbg[k] : e->extobj;

		if (src == G2D_UNMAPPED)
			continue;
		for (int i = 0; i < 4096; i++)
			dst[i] = (uint16_t)(g2d_ld16le(dst + i) & 0x7FFFu);
	}
	c->ext_loaded = 1;
}

/* ---- etapa 1: planear una línea -------------------------------------------------------------------------- */

static void apply_change(gpu2d_ctx *c, g2d_job *j, const gpu2d_change *ch)
{
	c->stats.chg_applied++;
	if (ch->reg < G2D_NREGS) {
		c->regs[ch->reg] = (uint16_t)ch->value;
		if (ch->reg >= G2D_R_BG2PA + 4 && ch->reg < G2D_R_BG2PA + 8)
			reload_ref(c, 0);
		else if (ch->reg >= G2D_R_BG3PA + 4 && ch->reg < G2D_R_BG3PA + 8)
			reload_ref(c, 1);
	} else if (ch->reg == G2D_CHG_PAL) {
		if (j->npalchg < G2D_PALCHG_MAX)
			j->palchg[j->npalchg++] = ch->value;
		else
			c->stats.dropped++;
	} else if (ch->reg == G2D_CHG_OAM) {
		const uint32_t i = (ch->value >> 16) & 511u;
		uint8_t *p = c->oamraw + i * 2;

		p[0] = (uint8_t)ch->value;
		p[1] = (uint8_t)(ch->value >> 8);
		decode_one(c, (int)(i >> 2));
	}
}

static void plan_text(gpu2d_ctx *c, const gpu2d_engine *e, g2d_job *j, int b, int y, int *need, uint32_t *map_wait)
{
	const uint32_t cnt = c->regs[G2D_R_BGCNT + b], size = G2D_BGCNT_SIZE(cnt);
	const uint32_t wide = size & 1, tall = size >> 1;
	const int isb = (e->flags & G2D_F_ENGINE_B) != 0;
	const int mv = ((c->regs[G2D_R_MOSAIC] >> 4) & 15) + 1;
	const int ys = (cnt & G2D_BGCNT_MOSAIC) ? y - y % mv : y;
	const uint32_t ty = (uint32_t)(ys + (c->regs[G2D_R_BGVOFS + 2 * b] & 0x1FF)) & (tall ? 511u : 255u);
	const uint32_t hw = (uint32_t)(c->regs[G2D_R_BGHOFS + 2 * b] & 0x1FF) & (wide ? 511u : 255u);
	const uint32_t row = ty >> 3, tx0 = hw >> 3;
	const uint32_t cbase = (isb ? 0 : G2D_DC_CHAR_OFF(c->dc)) + G2D_BGCNT_CHAR(cnt);
	const uint32_t sbase = (isb ? 0 : G2D_DC_SCR_OFF(c->dc)) + G2D_BGCNT_SCR(cnt);
	const uint32_t c256 = G2D_BGCNT_256(cnt);
	/* La clave es lo que determina las teselas traídas, sin la fila de píxeles ni el desplazamiento fino. */
	const uint32_t k0 = tx0 | row << 6 | size << 12 | c256 << 14;
	uint32_t cur = c->bg_cur[b];

	if (c->bg_k0[b][cur] != k0 || c->bg_cbase[b][cur] != cbase || c->bg_sbase[b][cur] != sbase) {
		cur ^= 1;
		c->bg_cur[b] = (uint8_t)cur;
		c->bg_k0[b][cur] = k0;
		c->bg_cbase[b][cur] = cbase;
		c->bg_sbase[b][cur] = sbase;
		/* La fila de 64 bytes del mapa de cada bloque de pantalla que toca la línea (uno o dos si es ancho). */
		{
			const uint32_t by = tall ? row >> 5 : 0, tag = G2D_TAG_BG + (uint32_t)b * 2 + cur;

			for (uint32_t bx = 0; bx <= wide; bx++) {
				const uint32_t block = bx + by * (wide ? 2u : 1u);
				const uint32_t off = sbase + block * 0x800u + (row & 31) * 64u;

				g2d_get(c->maps[b][cur] + bx * 64, e->vram_ea + view_off(e, 0, off), 64, tag);
			}
			*map_wait |= 1u << tag;
		}
		*need |= 1 << b;
		c->stats.bg_fetches++;
	}
	j->bg_slot[b] = (uint8_t)cur;
	j->bg_off[b] = (uint8_t)(hw & 7);
	j->bg_py[b] = (uint8_t)(ty & 7);
	j->bg_tx0[b] = (uint8_t)tx0;
	j->bg_wide[b] = (uint8_t)wide;
	j->bg_c256[b] = (uint8_t)c256;
	j->bg_extbase[b] = 0;
	if (c256 && (c->dc & G2D_DC_BGEXTPAL)) {
		uint32_t slot = (uint32_t)b;

		if (b == 0 && (cnt & G2D_BGCNT_SLOT2))
			slot = 2;
		if (b == 1 && (cnt & G2D_BGCNT_SLOT2))
			slot = 3;
		j->bg_extbase[b] = (uint16_t)(G2D_PAL_BGEXT + slot * 4096);
	}
	j->waitmask |= 1u << (G2D_TAG_BG + (uint32_t)b * 2 + cur);
}

/* Trae los mapas residentes de los fondos afines y extendidos de 16 bits (BG2 y BG3 juntos en la arena). Devuelve 1
 * si el fondo `rb` quedó con su mapa en la arena. */
static int amap_prepare(gpu2d_ctx *c, const gpu2d_engine *e, int rb, int type, uint32_t sbase, uint32_t len)
{
	uint32_t key = sbase ^ (len << 20) ^ ((uint32_t)type << 29) ^ 0x1u;

	if (c->amap_key[rb] == key && c->amap_len[rb] == len)
		return 1;
	/* sitio: BG2 al principio de la arena; BG3 a continuación de la de BG2 (si BG2 usa mapa) */
	{
		uint32_t off = 0;

		if (rb == 1 && c->amap_len[0])
			off = (c->amap_off[0] + c->amap_len[0] + 127u) & ~127u;
		if (off + len > G2D_MAP_ARENA) {
			c->stats.map_dropped++;
			c->amap_key[rb] = 0xFFFFFFFFu;
			c->amap_len[rb] = 0;
			return 0;
		}
		c->amap_off[rb] = off;
		c->amap_len[rb] = len;
		c->amap_key[rb] = key;
		if (rb == 0)
			c->amap_key[1] = 0xFFFFFFFFu;
		get_run(c->amap + off, e, 0, sbase, (len + 15u) & ~15u, G2D_TAG_MAP);
		PW(c, 1u << G2D_TAG_MAP);
	}
	return 1;
}

static void plan_affine(gpu2d_ctx *c, const gpu2d_engine *e, g2d_job *j, int b, int y, uint32_t p, uint32_t *tagmask)
{
	const int rb = b - 2, type = j->bg_type[b];
	const int isb = (e->flags & G2D_F_ENGINE_B) != 0;
	const uint32_t cnt = c->regs[G2D_R_BGCNT + b], size = G2D_BGCNT_SIZE(cnt);
	const int pbase = b == 3 ? G2D_R_BG3PA : G2D_R_BG2PA;
	const int pa = (int16_t)c->regs[pbase], pc = (int16_t)c->regs[pbase + 2];
	const int mh = (cnt & G2D_BGCNT_MOSAIC) ? (c->regs[G2D_R_MOSAIC] & 15) + 1 : 1;
	const int32_t *ref = (cnt & G2D_BGCNT_MOSAIC) ? c->mref[rb] : c->ref[rb];
	const uint32_t cbase = (isb ? 0 : G2D_DC_CHAR_OFF(c->dc)) + G2D_BGCNT_CHAR(cnt);
	const uint32_t sbase = (isb ? 0 : G2D_DC_SCR_OFF(c->dc)) + G2D_BGCNT_SCR(cnt);
	const int wrap = (cnt & G2D_BGCNT_WRAP) != 0;
	const uint32_t vmask = view_mask(e, 0);
	g2d_abg *ab = &c->abg[rb][p];
	int w, h;
	uint32_t n = 0;
	int32_t prev_quad = -1;
	const uint8_t *map = 0;
	uint32_t slot_ext = 0;

	(void)y;
	switch (type) {
	case BG_BMP8:
	case BG_BMPD:
		w = size >= 2 ? 512 : (size ? 256 : 128);
		h = size == 3 ? 512 : (size == 2 ? 256 : (size ? 256 : 128));
		break;
	case BG_LARGE:
		w = size ? 1024 : 512;
		h = size ? 512 : 1024;
		break;
	default:
		w = h = 128 << size;
		break;
	}
	if (type == BG_AFFINE || type == BG_EXT16) {
		const uint32_t len = (uint32_t)((w >> 3) * (h >> 3)) * (type == BG_EXT16 ? 2u : 1u);

		if (!amap_prepare(c, e, rb, type, sbase, len)) {
			j->aff_ok[rb] = 0;
			return;
		}
		map = c->amap + c->amap_off[rb];
	}
	if (type == BG_EXT16 && (c->dc & G2D_DC_BGEXTPAL))
		slot_ext = G2D_PAL_BGEXT + (uint32_t)b * 4096u;
	if (mh == 1 && pa == 0x100 && pc == 0 && (type == BG_AFFINE || type == BG_EXT16)) {
		/*
		 * Fondo de teselas sin rotación ni escala en horizontal (PA = 1,0, PC = 0: lo que usan los juegos para un fondo que
		 * solo se desplaza): la fila de origen es una sola y x avanza de a uno, así que se recorre por teselas: una lectura
		 * del mapa y un bloque de 16 bytes (la fila de 8 texeles de una tesela cabe siempre en uno) por cada 8 píxeles, en
		 * lugar de una lectura del mapa y una dirección por píxel.
		 */
		const int lw3 = __builtin_ctz((unsigned)w) - 3;
		const int32_t wm = w - 1, hm = h - 1;
		const int32_t sx0 = ref[0] >> 8, sy0 = ref[1] >> 8;
		const int32_t sy = sy0 & hm;
		const int rowok = wrap || sy == sy0;
		int i = 0;

		while (i < GPU2D_W) {
			const int32_t x = sx0 + i;
			const int32_t xw = x & wm;
			const int ix = xw & 7, runlen = 8 - ix < GPU2D_W - i ? 8 - ix : GPU2D_W - i;
			const int ok = rowok && (wrap || xw == x);

			if (!ok) {
				for (int k = 0; k < runlen; k++)
					ab->off[i + k] = 0xFFFF;
			} else {
				uint32_t row;
				uint16_t base = 0;
				int hf = 0;
				const int mi = ((sy >> 3) << lw3) + (xw >> 3);

				if (type == BG_AFFINE) {
					row = cbase + (uint32_t)map[mi] * 64u + (uint32_t)((sy & 7) << 3);
				} else {
					const uint16_t me = g2d_ld16le(map + (mi << 1));
					const int iy = (me & 0x800) ? 7 - (sy & 7) : (sy & 7);

					hf = (me & 0x400) != 0;
					row = cbase + (uint32_t)(me & 1023) * 64u + (uint32_t)(iy << 3);
					if (slot_ext)
						base = (uint16_t)(slot_ext + ((me >> 12) & 15) * 256u);
				}
				row &= vmask;
				if ((int32_t)(row >> 4) != prev_quad) {
					prev_quad = (int32_t)(row >> 4);
					c->aoffs[n++] = view_off(e, 0, row & ~15u);
				}
				{
					const uint16_t qo = (uint16_t)((n - 1) * 16u + (row & 15u));

					for (int k = 0; k < runlen; k++) {
						ab->off[i + k] = (uint16_t)(qo + (hf ? 7 - (ix + k) : ix + k));
						ab->base[i + k] = base;
					}
				}
			}
			i += runlen;
		}
	} else if (mh == 1) {
		/*
		 * Sin mosaico, dos pasadas. 1ª: la dirección de cada texel, sin ramas ni dependencias entre píxeles (la posición avanza
		 * sumando PA y PC: el punto de referencia cabe en 28 bits y PA * 255 en 24, así que bastan enteros de 32 bits; los
		 * lados son potencias de 2: nada de multiplicar por w). 2ª: quitar los repetidos (un bloque de 16 bytes por cambio de
		 * bloque) y armar la lista del DMA. En el SPE un píxel de la versión de una sola pasada costaba ≈ 115 ciclos.
		 */
		const int lw = __builtin_ctz((unsigned)w), lw3 = lw - 3;
		int32_t px = ref[0], py = ref[1];
		const int32_t dx = (int32_t)pa, dy = (int32_t)pc;
		const int32_t wm = w - 1, hm = h - 1;
		const uint32_t scr = G2D_BGCNT_SCR_BMP(cnt), wrapm = wrap ? 0xFFFFFFFFu : 0u, none = 0xFFFFFFFFu;
		uint32_t *const aa = c->aaddr;

#define AFF_LOOP(...)                                                                                     \
	for (int i = 0; i < GPU2D_W; i++) {                                                               \
		const int32_t sx0 = px >> 8, sy0 = py >> 8;                                               \
		const int32_t sx = sx0 & wm, sy = sy0 & hm;                                               \
		const uint32_t ok = wrapm | (uint32_t)-(int32_t)((sx == sx0) & (sy == sy0));               \
		uint32_t a;                                                                               \
		uint16_t base = 0;                                                                        \
		(void)base;                                                                               \
		__VA_ARGS__                                                                               \
		aa[i] = (a & vmask) | ~ok;                                                                \
		ab->base[i] = base;                                                                       \
		px += dx;                                                                                 \
		py += dy;                                                                                 \
	}
		switch (type) {
		case BG_AFFINE:
			AFF_LOOP(a = cbase + (uint32_t)map[((sy >> 3) << lw3) + (sx >> 3)] * 64u + (uint32_t)(((sy & 7) << 3) + (sx & 7));)
			break;
		case BG_EXT16:
			AFF_LOOP({
				const uint16_t me = g2d_ld16le(map + ((((sy >> 3) << lw3) + (sx >> 3)) << 1));
				const int ix = (me & 0x400) ? 7 - (sx & 7) : (sx & 7), iy = (me & 0x800) ? 7 - (sy & 7) : (sy & 7);

				a = cbase + (uint32_t)(me & 1023) * 64u + (uint32_t)((iy << 3) + ix);
				base = slot_ext ? (uint16_t)(slot_ext + ((me >> 12) & 15) * 256u) : 0;
			})
			break;
		case BG_BMP8:
			AFF_LOOP(a = scr + (uint32_t)((sy << lw) + sx);)
			break;
		case BG_BMPD:
			AFF_LOOP(a = scr + (uint32_t)(((sy << lw) + sx) << 1);)
			break;
		default:
			AFF_LOOP(a = (uint32_t)((sy << lw) + sx);)
			break;
		}
#undef AFF_LOOP
		for (int i = 0; i < GPU2D_W; i++) {
			const uint32_t a = aa[i];

			if (a == none) {
				ab->off[i] = 0xFFFF;
				continue;
			}
			if ((int32_t)(a >> 4) != prev_quad) {
				prev_quad = (int32_t)(a >> 4);
				c->aoffs[n++] = view_off(e, 0, a & ~15u);
			}
			ab->off[i] = (uint16_t)((n - 1) * 16u + (a & 15u));
		}
	} else {
	/*
	 * Un píxel por grupo de mosaico. Sin divisiones ni productos de 64 bits (en el SPE son llamadas a rutinas de software
	 * de cientos de ciclos por píxel: esa era la causa principal de los 20 ms por pantalla con fondos afines).
	 */
	{
		const int lw = __builtin_ctz((unsigned)w), lw3 = lw - 3;
		int32_t px = ref[0], py = ref[1];
		const int32_t dx = (int32_t)pa * mh, dy = (int32_t)pc * mh;
		const uint16_t wm = (uint16_t)(w - 1), hm = (uint16_t)(h - 1);

		for (int i0 = 0; i0 < GPU2D_W; i0 += mh, px += dx, py += dy) {
			int32_t sx = px >> 8, sy = py >> 8;
			uint32_t a = 0;
			uint16_t base = 0, off;
			int ok = 1;

			if (wrap) {
				sx &= wm;
				sy &= hm;
			} else if ((uint32_t)sx >= (uint32_t)w || (uint32_t)sy >= (uint32_t)h) {
				ok = 0;
			}
			if (ok) {
				switch (type) {
				case BG_AFFINE:
					a = cbase + (uint32_t)map[((sy >> 3) << lw3) + (sx >> 3)] * 64u + (uint32_t)(((sy & 7) << 3) + (sx & 7));
					break;
				case BG_EXT16: {
					const uint16_t me = g2d_ld16le(map + ((((sy >> 3) << lw3) + (sx >> 3)) << 1));
					int ix = sx & 7, iy = sy & 7;

					if (me & 0x400)
						ix = 7 - ix;
					if (me & 0x800)
						iy = 7 - iy;
					a = cbase + (uint32_t)(me & 1023) * 64u + (uint32_t)((iy << 3) + ix);
					if (slot_ext)
						base = (uint16_t)(slot_ext + ((me >> 12) & 15) * 256u);
					break;
				}
				case BG_BMP8:
					a = G2D_BGCNT_SCR_BMP(cnt) + (uint32_t)((sy << lw) + sx);
					break;
				case BG_BMPD:
					a = G2D_BGCNT_SCR_BMP(cnt) + (uint32_t)(((sy << lw) + sx) << 1);
					break;
				default:
					a = (uint32_t)((sy << lw) + sx);
					break;
				}
				a &= vmask;
				if ((int32_t)(a >> 4) != prev_quad) {
					prev_quad = (int32_t)(a >> 4);
					c->aoffs[n++] = view_off(e, 0, a & ~15u);
				}
				off = (uint16_t)((n - 1) * 16u + (a & 15u));
			} else {
				off = 0xFFFF;
				base = 0;
			}
			{
				const int i1 = i0 + mh < GPU2D_W ? i0 + mh : GPU2D_W;

				for (int i = i0; i < i1; i++) {
					ab->off[i] = off;
					ab->base[i] = base;
				}
			}
		}
	}
	}
	if (n) {
		g2d_gather(ab->data, e->vram_ea, c->aoffs, n, 16, G2D_TAG_AFF + (uint32_t)rb * 2 + p, ab->list);
		*tagmask |= 1u << (G2D_TAG_AFF + (uint32_t)rb * 2 + p);
	}
	c->stats.aff_lines++;
	c->stats.aff_quads += n;
	j->aff_ok[rb] = 1;
}

/* ---- OBJ: plan ------------------------------------------------------------------------------------------- */

static void obj_clear_line(gpu2d_ctx *c, uint32_t p)
{
	const g2d_v8 tr = g2d_v8_splat(G2D_TRANSP), pr = g2d_v8_splat(7), zero = g2d_v8_splat(0);

	for (int v = 0; v < G2D_LINE_VECS; v++) {
		c->oidx[p][v] = tr;
		c->omet[p][v] = pr;
		c->owin[p][v] = zero;
		c->ocov[p][v] = zero;
	}
}

/* Pone un píxel de OBJ en la línea (bucles escalares): gana si su prioridad es igual o mejor que la que hay. */
static void obj_put(gpu2d_ctx *c, uint32_t p, int x, uint16_t idx, uint16_t meta)
{
	const int pos = x + 64, v = pos >> 3, k = pos & 7;
	const uint32_t old = g2d_v8_lane(c->omet[p][v], k);

	if ((uint32_t)(meta & 7) <= (old & 7)) {
		c->oidx[p][v] = g2d_v8_put(c->oidx[p][v], k, idx);
		c->omet[p][v] = g2d_v8_put(c->omet[p][v], k, meta);
	}
}

static void obj_putwin(gpu2d_ctx *c, uint32_t p, int x)
{
	const int pos = x + 64;

	c->owin[p][pos >> 3] = g2d_v8_put(c->owin[p][pos >> 3], pos & 7, 0xFFFF);
}

/* Un texel transparente de un OBJ con mosaico, dentro de su imagen: deja la marca en la línea de OBJ */
static void obj_putcover(gpu2d_ctx *c, uint32_t p, int x)
{
	const int pos = x + 64;

	c->ocov[p][pos >> 3] = g2d_v8_put(c->ocov[p][pos >> 3], pos & 7, 0xFFFF);
}

static inline uint16_t obj_meta(const g2d_obj *o)
{
	uint16_t m = o->prio;

	if (o->mode == 1)
		m |= 8;
	if (o->mode == 3)
		m |= 16 | (uint16_t)((o->pal & 15) << 5);
	if (o->flags & G2D_OBJ_MOS)
		m |= 0x200;
	return m;
}

/* Base del índice de paleta de un OBJ de tesela */
static inline uint16_t obj_base(uint32_t dc, const g2d_obj *o)
{
	if (o->flags & G2D_OBJ_C256)
		return (dc & G2D_DC_OBJEXTPAL) ? (uint16_t)(G2D_PAL_OBJEXT + o->pal * 256u) : (uint16_t)G2D_PAL_OBJ;
	return (uint16_t)(G2D_PAL_OBJ + o->pal * 16u);
}

/* Dibuja las operaciones pendientes de la línea (desde `from`) en las capas de OBJ de la paridad p. */
static void obj_draw_ops(gpu2d_ctx *c, g2d_job *j, uint32_t p, uint32_t from)
{
	const uint8_t *arena = c->oarena[p];

	for (uint32_t k = from; k < j->nops; k++) {
		const g2d_op *op = &j->ops[k];
		const g2d_obj *o = &op->o;
		const uint16_t meta = obj_meta(o);
		const int win = o->mode == 2;
		const int hflip = (o->flags & G2D_OBJ_HFLIP) != 0;

		c->stats.obj_drawn++;
		if (op->kind == G2D_OP_ROW) {
			const uint32_t wt = (uint32_t)o->w >> 3, c256 = (o->flags & G2D_OBJ_C256) != 0, tsz = c256 ? 64u : 32u;
			const uint8_t *row = arena + op->data;
			const uint32_t pos = (uint32_t)(o->x + 64), off = pos & 7, vi = pos >> 3;
			const uint16_t base = obj_base(j->dc, o);

			for (uint32_t t = 0; t < wt; t++) {
				const uint32_t src = hflip ? wt - 1 - t : t;
				g2d_v8 v8;

				if (c256)
					v8 = g2d_v8_expand8(row + src * tsz + op->py * 8u, base, hflip);
				else
					v8 = g2d_v8_expand4(row + src * tsz + op->py * 4u, base, hflip);
				for (uint32_t half = 0; half < (off ? 2u : 1u); half++) {
					g2d_v8 lo, hi, s;
					const uint32_t vv = vi + t + half;

					if (off) {
						g2d_v8_place(v8, off, &lo, &hi);
						s = half ? hi : lo;
					} else {
						s = v8;
					}
					{
						const g2d_v8 opq = g2d_v8_andc(g2d_v8_splat(0xFFFF), g2d_v8_eq(s, g2d_v8_splat(G2D_TRANSP)));

						if ((o->flags & G2D_OBJ_MOS) && !win) {
							g2d_v8 in = g2d_v8_splat(0xFFFF);

							if (off) {
								g2d_v8 z0, z1;

								g2d_v8_place(g2d_v8_splat(0), off, &z0, &z1);
								in = g2d_v8_eq(half ? z1 : z0, g2d_v8_splat(0));
							}
							c->ocov[p][vv] = g2d_v8_or(c->ocov[p][vv], g2d_v8_andc(in, opq));
						}
						if (win) {
							c->owin[p][vv] = g2d_v8_or(c->owin[p][vv], opq);
						} else {
							const g2d_v8 le = g2d_v8_andc(g2d_v8_splat(0xFFFF),
										     g2d_v8_gt(g2d_v8_splat((uint16_t)(meta & 7)),
											       g2d_v8_and(c->omet[p][vv], g2d_v8_splat(7))));
							const g2d_v8 m = g2d_v8_and(opq, le);

							c->oidx[p][vv] = g2d_v8_sel(c->oidx[p][vv], s, m);
							c->omet[p][vv] = g2d_v8_sel(c->omet[p][vv], g2d_v8_splat(meta), m);
						}
					}
				}
			}
		} else if (op->kind == G2D_OP_BMPROW) {
			const uint32_t nv = (uint32_t)o->w >> 3;
			const uint8_t *row = arena + op->data;
			const uint32_t pos = (uint32_t)(o->x + 64), off = pos & 7, vi = pos >> 3;

			for (uint32_t t = 0; t < nv; t++) {
				const uint32_t src = hflip ? nv - 1 - t : t;
				const g2d_v8 v8 = g2d_v8_expand16(row + src * 16u, hflip);

				for (uint32_t half = 0; half < (off ? 2u : 1u); half++) {
					g2d_v8 lo, hi, s;
					const uint32_t vv = vi + t + half;

					if (off) {
						g2d_v8_place(v8, off, &lo, &hi);
						s = half ? hi : lo;
					} else {
						s = v8;
					}
					{
						const g2d_v8 opq = g2d_v8_andc(g2d_v8_splat(0xFFFF), g2d_v8_eq(s, g2d_v8_splat(G2D_TRANSP)));

						if (o->flags & G2D_OBJ_MOS) {
							g2d_v8 in = g2d_v8_splat(0xFFFF);

							if (off) {
								g2d_v8 z0, z1;

								g2d_v8_place(g2d_v8_splat(0), off, &z0, &z1);
								in = g2d_v8_eq(half ? z1 : z0, g2d_v8_splat(0));
							}
							c->ocov[p][vv] = g2d_v8_or(c->ocov[p][vv], g2d_v8_andc(in, opq));
						}
					}
					{
						const g2d_v8 opq = g2d_v8_andc(g2d_v8_splat(0xFFFF), g2d_v8_eq(s, g2d_v8_splat(G2D_TRANSP)));
						const g2d_v8 le = g2d_v8_andc(g2d_v8_splat(0xFFFF),
									     g2d_v8_gt(g2d_v8_splat((uint16_t)(meta & 7)),
										       g2d_v8_and(c->omet[p][vv], g2d_v8_splat(7))));
						const g2d_v8 m = g2d_v8_and(opq, le);

						c->oidx[p][vv] = g2d_v8_sel(c->oidx[p][vv], s, m);
						c->omet[p][vv] = g2d_v8_sel(c->omet[p][vv], g2d_v8_splat(meta), m);
					}
				}
			}
		} else { /* AFF, BMPAFF */
			const int bmp = op->kind == G2D_OP_BMPAFF;
			const uint8_t *data = arena + op->data;
			const uint16_t *offs = (const uint16_t *)(const void *)(arena + op->offs);
			const uint16_t base = bmp ? 0 : obj_base(j->dc, o);
			const int c256 = (o->flags & G2D_OBJ_C256) != 0;

			for (uint32_t i = 0; i < op->n; i++) {
				const uint16_t of = offs[i];
				uint16_t idx;

				if (of == 0xFFFF)
					continue;
				if (bmp) {
					const uint16_t v = g2d_ld16le(data + (of & 0x7FFF));

					if (!(v & 0x8000u)) {
						if (o->flags & G2D_OBJ_MOS)
							obj_putcover(c, p, (int)(op->x0 + i));
						continue;
					}
					idx = v;
				} else {
					const uint8_t byte = data[of & 0x7FFF];
					int col;

					if (c256)
						col = byte;
					else
						col = (of & 0x8000u) ? byte >> 4 : byte & 15;
					if (!col) {
						if ((o->flags & G2D_OBJ_MOS) && !win)
							obj_putcover(c, p, (int)(op->x0 + i));
						continue;
					}
					idx = (uint16_t)(base + col);
				}
				if (win)
					obj_putwin(c, p, (int)(op->x0 + i));
				else
					obj_put(c, p, (int)(op->x0 + i), idx, meta);
			}
		}
	}
}

/* Mosaico horizontal de OBJ sobre la línea de OBJ ya hecha (melonDS 1.1; ver la referencia). */
static void obj_mosaic_x(gpu2d_ctx *c, uint32_t p, int mh)
{
	uint16_t idx[GPU2D_W] __attribute__((aligned(16))), met[GPU2D_W] __attribute__((aligned(16)));
	uint16_t lidx = G2D_TRANSP, lmet = 7;
	int prev_flag = 0;

	uint16_t cov[GPU2D_W] __attribute__((aligned(16)));

	for (int v = 0; v < 32; v++) {
		memcpy(idx + v * 8, &c->oidx[p][8 + v], 16);
		memcpy(met + v * 8, &c->omet[p][8 + v], 16);
		memcpy(cov + v * 8, &c->ocov[p][8 + v], 16);
	}
	for (int x = 0, xm = 0; x < GPU2D_W; x++, xm = xm + 1 == mh ? 0 : xm + 1) { /* xm = x % mh sin dividir */
		/* un texel transparente de un OBJ con mosaico, donde no hay otro píxel, lleva la marca sin color */
		if (idx[x] == G2D_TRANSP && cov[x])
			met[x] = (uint16_t)(7 | 0x200);
		const int flag = (met[x] & 0x200) != 0;

		if (flag) {
			if (xm == 0 || !prev_flag) {
				lidx = idx[x];
				lmet = met[x];
			} else {
				idx[x] = lidx;
				met[x] = lmet;
			}
		}
		prev_flag = flag;
	}
	for (int v = 0; v < 32; v++) {
		memcpy(&c->oidx[p][8 + v], idx + v * 8, 16);
		memcpy(&c->omet[p][8 + v], met + v * 8, 16);
	}
}

static void obj_flush(gpu2d_ctx *c, g2d_job *j, uint32_t p)
{
	g2d_wait(1u << (G2D_TAG_OBJ + p));
	obj_draw_ops(c, j, p, j->op_done);
	j->op_done = (uint16_t)j->nops;
	c->oa_used = 0;
	c->stats.obj_flushes++;
}

/* Hay sitio para `bytes` más en el área de la línea? Si no, vacía lo pendiente. */
static uint32_t oa_alloc(gpu2d_ctx *c, g2d_job *j, uint32_t p, uint32_t bytes)
{
	bytes = (bytes + 15u) & ~15u;
	if (c->oa_used + bytes > G2D_OBJ_ARENA)
		obj_flush(c, j, p);
	{
		const uint32_t at = c->oa_used;

		c->oa_used += bytes;
		return at;
	}
}

static void plan_obj(gpu2d_ctx *c, const gpu2d_engine *e, g2d_job *j, int y, uint32_t p)
{
	const uint32_t dc = c->dc;
	const int shift = 5 + (int)G2D_DC_OBJ_SHIFT(dc);
	const int mv = ((c->regs[G2D_R_MOSAIC] >> 12) & 15) + 1;
	const uint32_t tag = G2D_TAG_OBJ + p;

	obj_clear_line(c, p);
	j->nops = 0;
	j->op_done = 0;
	c->oa_used = 0;
	if (!(dc & G2D_DC_OBJ))
		return;
	for (int s = GPU2D_NOBJ - 1; s >= 0; s--) {
		const g2d_obj *o = &c->obj[s];
		const int aff = (o->flags & G2D_OBJ_AFF) != 0, dbl = (o->flags & G2D_OBJ_DBL) != 0;
		const int bw = o->w << dbl, bh = o->h << dbl;
		const int c256 = (o->flags & G2D_OBJ_C256) != 0;
		const uint32_t tsz = c256 ? 64u : 32u;
		const int bmp = o->mode == 3;
		int dy;
		g2d_op *op;

		if (!(o->flags & G2D_OBJ_VALID))
			continue;
		if (((y - o->y) & 255) >= bh)
			continue;
		dy = (y - o->y) & 255;
		if (o->flags & G2D_OBJ_MOS) {
			dy = (dy - y % mv) & 255; /* sin acotar: ver gpu2d_ref.c */
		}
		if (j->nops >= G2D_NOPS)
			break;
		if (!aff) {
			const int ty = (o->flags & G2D_OBJ_VFLIP) ? o->h - 1 - dy : dy;
			uint32_t addr, size;

			if (bmp) {
				if (dc & G2D_DC_OBJBMP_1D) {
					addr = (uint32_t)o->tile * ((dc & G2D_DC_OBJBMP_BND) ? 256u : 128u) + (uint32_t)(ty * o->w) * 2u;
				} else {
					const uint32_t maskx = (dc & G2D_DC_OBJBMP_256) ? 0x1Fu : 0x0Fu;

					addr = ((uint32_t)o->tile & maskx) * 16u + ((uint32_t)o->tile & ~maskx) * 128u +
					       (uint32_t)ty * ((maskx + 1) * 16u);
				}
				size = (uint32_t)o->w * 2u;
			} else {
				const uint32_t wt = (uint32_t)o->w >> 3, r = (uint32_t)(ty >> 3);

				if (dc & G2D_DC_OBJ_1D)
					addr = ((uint32_t)o->tile << shift) + r * wt * tsz;
				else
					addr = ((uint32_t)o->tile + r * 32u) * 32u;
				size = wt * tsz;
			}
			{
				const uint32_t at = oa_alloc(c, j, p, size);

				get_run(c->oarena[p] + at, e, 1, addr, size, tag);
				op = &j->ops[j->nops++];
				op->o = *o;
				op->kind = (uint8_t)(bmp ? G2D_OP_BMPROW : G2D_OP_ROW);
				op->py = (uint8_t)(ty & 7);
				op->data = (uint16_t)at;
				c->stats.obj_fetches++;
			}
		} else {
			/* afín: parámetros de la OAM y un bloque de 16 bytes por cada trozo de origen distinto */
			const int pa = (int16_t)g2d_ld16le(c->oamraw + o->group * 32 + 6),
				  pb = (int16_t)g2d_ld16le(c->oamraw + o->group * 32 + 14),
				  pc = (int16_t)g2d_ld16le(c->oamraw + o->group * 32 + 22),
				  pd = (int16_t)g2d_ld16le(c->oamraw + o->group * 32 + 30);
			const int xa = o->x < 0 ? 0 : o->x;
			int xb = o->x + bw;
			uint32_t n, at_list, at_offs, at_data, nq = 0;
			const int cx = bw / 2, cy = bh / 2;
			uint16_t *offs;
			const uint32_t omask = view_mask(e, 1);
			const uint32_t maskx = (dc & G2D_DC_OBJBMP_256) ? 0x1Fu : 0x0Fu;
			int32_t prev_quad = -1;

			if (xb > 256)
				xb = 256;
			if (xb <= xa)
				continue;
			n = (uint32_t)(xb - xa);
			at_list = oa_alloc(c, j, p, n * 8u + n * 2u + 16u + n * 16u);
			at_offs = at_list + n * 8u;
			at_data = (at_offs + n * 2u + 15u) & ~15u;
			offs = (uint16_t *)(void *)(c->oarena[p] + at_offs);
			for (uint32_t i = 0; i < n; i++) {
				const int x = xa + (int)i;
				const int dx = x - o->x;
				int sx, sy;
				uint32_t a;

				offs[i] = 0xFFFF;
				if (dx < 0 || dx >= bw)
					continue;
				sx = ((pa * (dx - cx) + pb * (dy - cy)) >> 8) + o->w / 2;
				sy = ((pc * (dx - cx) + pd * (dy - cy)) >> 8) + o->h / 2;
				if (sx < 0 || sx >= o->w || sy < 0 || sy >= o->h)
					continue;
				if (bmp) {
					if (dc & G2D_DC_OBJBMP_1D)
						a = (uint32_t)o->tile * ((dc & G2D_DC_OBJBMP_BND) ? 256u : 128u) + (uint32_t)(sy * o->w + sx) * 2u;
					else
						a = ((uint32_t)o->tile & maskx) * 16u + ((uint32_t)o->tile & ~maskx) * 128u +
						    (uint32_t)sy * ((maskx + 1) * 16u) + (uint32_t)sx * 2u;
				} else {
					const uint32_t wt = (uint32_t)o->w >> 3;
					const int tx = sx >> 3, ty = sy >> 3, ix = sx & 7, iy = sy & 7;

					if (dc & G2D_DC_OBJ_1D)
						a = ((uint32_t)o->tile << shift) + (uint32_t)(ty * (int)wt + tx) * tsz;
					else
						a = ((uint32_t)o->tile + (uint32_t)ty * 32u + (uint32_t)tx * (c256 ? 2u : 1u)) * 32u;
					a += c256 ? (uint32_t)(iy * 8 + ix) : (uint32_t)(iy * 4 + (ix >> 1));
				}
				a &= omask;
				if ((int32_t)(a >> 4) != prev_quad) {
					prev_quad = (int32_t)(a >> 4);
					c->aoffs[nq++] = view_off(e, 1, a & ~15u);
				}
				offs[i] = (uint16_t)((nq - 1) * 16u + (a & 15u));
				if (!bmp && !c256 && (sx & 1))
					offs[i] |= 0x8000;
			}
			if (nq)
				g2d_gather(c->oarena[p] + at_data, e->vram_ea, c->aoffs, nq, 16, tag, (g2d_el *)(void *)(c->oarena[p] + at_list));
			c->stats.aff_quads += nq;
			op = &j->ops[j->nops++];
			op->o = *o;
			op->kind = (uint8_t)(bmp ? G2D_OP_BMPAFF : G2D_OP_AFF);
			op->data = (uint16_t)at_data;
			op->offs = (uint16_t)at_offs;
			op->x0 = (uint16_t)xa;
			op->n = (uint16_t)n;
			c->stats.obj_fetches++;
		}
	}
	j->waitmask |= 1u << tag;
}

/* ---- etapa 1 --------------------------------------------------------------------------------------------- */

static void plan_line(gpu2d_ctx *c, const gpu2d_engine *e, int y)
{
	const uint32_t p = (uint32_t)y & 1;
	g2d_job *j = &c->job[p];
	const int isb = (e->flags & G2D_F_ENGINE_B) != 0;
	const gpu2d_change *ch;

	G2D_STAGE(G2D_ST_PLAN);
	j->npalchg = 0;
	j->waitmask = 0;
	while ((ch = chg_peek(c, e, y)) != 0) {
		apply_change(c, j, ch);
		chg_pop(c, e);
	}
	c->dc = (uint32_t)c->regs[G2D_R_DISPCNT] | ((uint32_t)c->regs[G2D_R_DISPCNT + 1] << 16);
	if (isb)
		c->dc &= ~(G2D_DC_3D | G2D_DC_OBJBMP_BND | 0x000C0000u | 0x3F000000u);
	j->dc = c->dc;
	memcpy(j->regs, c->regs, sizeof j->regs);
	/* mosaico vertical: al empezar un grupo se fija el punto de referencia */
	if (y % (((c->regs[G2D_R_MOSAIC] >> 4) & 15) + 1) == 0)
		memcpy(c->mref, c->ref, sizeof c->mref);
	win_step_v(c->regs, c->winv, 0, y);
	win_step_v(c->regs, c->winv, 1, y);
	j->winv[0] = c->winv[0];
	j->winv[1] = c->winv[1];

	{
		const int mode = isb ? (int)(G2D_DC_DISPMODE(c->dc) & 1) : (int)G2D_DC_DISPMODE(c->dc);

		if ((c->dc & G2D_DC_BLANK) || mode == 0) {
			j->kind = 0;
		} else if (mode == 2) {
			const uint32_t base = e->vdisp[G2D_DC_VRAMBANK(c->dc)];

			j->kind = 2;
			if (base == G2D_UNMAPPED)
				memset(c->rawline[p], 0, 512);
			else
				g2d_get(c->rawline[p], e->vram_ea + base + (uint32_t)y * 512u, 512, G2D_TAG_RAW + p);
			j->waitmask |= 1u << (G2D_TAG_RAW + p);
		} else if (mode == 3) {
			j->kind = 3;
			if (!e->mmem_ea)
				memset(c->rawline[p], 0, 512);
			else
				g2d_get(c->rawline[p], e->mmem_ea + (uint64_t)y * 512u, 512, G2D_TAG_RAW + p);
			j->waitmask |= 1u << (G2D_TAG_RAW + p);
		} else {
			j->kind = 1;
		}
	}
	if (j->kind == 1) {
		int need = 0;
		uint32_t map_wait = 0;

		if (!c->ext_loaded && (c->dc & (G2D_DC_BGEXTPAL | G2D_DC_OBJEXTPAL)))
			ext_pal_load(c, e);
		for (int b = 0; b < GPU2D_NBG; b++)
			j->bg_type[b] = (uint8_t)bg_type(e, c->dc, c->regs, b);
		j->aff_ok[0] = j->aff_ok[1] = 0;
		for (int b = 0; b < GPU2D_NBG; b++) {
			const int t = j->bg_type[b];

			if (t == BG_TEXT) {
				const uint32_t t0_ = G2D_T();

				G2D_STAGE(G2D_ST_TEXT);
				plan_text(c, e, j, b, y, &need, &map_wait);
				G2D_ACC(c, plan_text, t0_);
			} else if (t >= BG_AFFINE && t <= BG_LARGE) {
				const uint32_t t0_ = G2D_T();

				G2D_STAGE(G2D_ST_AFFINE);
				plan_affine(c, e, j, b, y, p, &j->waitmask);
				G2D_ACC(c, plan_aff, t0_);
			} else if (t == BG_3D) {
				/* capa 3D (Fase 3.2): la línea y de la imagen del motor de render (1 KB) */
				if (e->img3d_ea) {
					g2d_get(c->l3d[p], e->img3d_ea + (uint64_t)y * GPU2D_W * 4u, GPU2D_W * 4u, G2D_TAG_3D + p);
					j->waitmask |= 1u << (G2D_TAG_3D + p);
				} else {
					memset(c->l3d[p], 0, sizeof c->l3d[p]);
				}
			}
		}
		/* Etapa B de los fondos de texto: con los mapas en el local store, pedir las teselas. */
		G2D_STAGE(G2D_ST_TEXT);
		if (need) {
			const uint32_t t1_ = G2D_T();

			PW(c, map_wait);
			c->stats.map_waits++;
			for (int b = 0; b < GPU2D_NBG; b++) {
				if (!(need & (1 << b)))
					continue;
				{
					const uint32_t cur = j->bg_slot[b], tx0 = j->bg_tx0[b], wide = j->bg_wide[b];
					const uint32_t tsz = j->bg_c256[b] ? 64u : 32u;
					const uint32_t cbase = c->bg_cbase[b][cur];

					for (uint32_t i = 0; i < G2D_TILES; i++) {
						const uint32_t tx = (tx0 + i) & (wide ? 63u : 31u);
						const uint32_t me = g2d_ld16le(c->maps[b][cur] + tx * 2);

						c->offs[i] = view_off(e, 0, cbase + (me & 1023u) * tsz);
					}
					g2d_gather(c->tiles[b][cur], e->vram_ea, c->offs, G2D_TILES, tsz, G2D_TAG_BG + (uint32_t)b * 2 + cur,
						   c->lists[b][cur]);
				}
			}
			G2D_ACC(c, plan_text, t1_);
		}
		G2D_STAGE(G2D_ST_OBJ);
		{
			const uint32_t t2_ = G2D_T();

			plan_obj(c, e, j, y, p);
			G2D_ACC(c, plan_obj, t2_);
		}
		G2D_STAGE(G2D_ST_PLAN);
	}
	/* al final de la línea: los puntos de referencia avanzan con PB y PD */
	for (int b = 0; b < 2; b++) {
		const int pbase = b ? G2D_R_BG3PA : G2D_R_BG2PA;

		c->ref[b][0] += (int16_t)c->regs[pbase + 1];
		c->ref[b][1] += (int16_t)c->regs[pbase + 3];
	}
}

/* ---- etapa 2: dibujar capas ------------------------------------------------------------------------------- */

static void mosaic_h(g2d_v8 *line8, int mh)
{
	uint16_t px[GPU2D_W] __attribute__((aligned(16)));

	for (int v = 0; v < 32; v++)
		memcpy(px + v * 8, &line8[8 + v], 16);
	for (int x0 = 0; x0 < GPU2D_W; x0 += mh) { /* sin dividir por píxel */
		const int x1 = x0 + mh < GPU2D_W ? x0 + mh : GPU2D_W;

		for (int x = x0 + 1; x < x1; x++)
			px[x] = px[x0];
	}
	for (int v = 0; v < 32; v++)
		memcpy(&line8[8 + v], px + v * 8, 16);
}

static void draw_text(gpu2d_ctx *c, const g2d_job *j, int b)
{
	const uint32_t slot = j->bg_slot[b], tx0 = j->bg_tx0[b], py = j->bg_py[b];
	const uint32_t tmask = j->bg_wide[b] ? 63u : 31u;
	const uint8_t *map = c->maps[b][slot], *tiles = c->tiles[b][slot];
	const uint32_t off = j->bg_off[b];

	if (j->bg_c256[b]) {
		for (uint32_t i = 0; i < G2D_TILES; i++) {
			const uint32_t me = g2d_ld16le(map + ((tx0 + i) & tmask) * 2);
			const uint32_t row = (me & 0x800) ? 7 - py : py;
			const uint32_t base = j->bg_extbase[b] ? j->bg_extbase[b] + ((me >> 12) & 15) * 256u : 0;

			c->tmp[i] = g2d_v8_expand8(tiles + i * 64 + row * 8, base, (me >> 10) & 1);
		}
	} else {
		for (uint32_t i = 0; i < G2D_TILES; i++) {
			const uint32_t me = g2d_ld16le(map + ((tx0 + i) & tmask) * 2);
			const uint32_t row = (me & 0x800) ? 7 - py : py;

			c->tmp[i] = g2d_v8_expand4(tiles + i * 32 + row * 4, ((me >> 12) & 15) << 4, (me >> 10) & 1);
		}
	}
	if (off == 0) {
		for (uint32_t v = 0; v < 32; v++)
			c->bg[b][8 + v] = c->tmp[v];
	} else {
		for (uint32_t v = 0; v < 32; v++)
			c->bg[b][8 + v] = g2d_v8_window(c->tmp[v], c->tmp[v + 1], off);
	}
	if (j->regs[G2D_R_BGCNT + b] & G2D_BGCNT_MOSAIC) {
		const int mh = (j->regs[G2D_R_MOSAIC] & 15) + 1;

		if (mh > 1)
			mosaic_h(c->bg[b], mh);
	}
}

static void draw_affine(gpu2d_ctx *c, const g2d_job *j, int b, uint32_t p)
{
	const int rb = b - 2;
	const g2d_abg *ab = &c->abg[rb][p];
	const int bmpd = j->bg_type[b] == BG_BMPD;
	const uint8_t *data = &ab->data[0][0];
	const g2d_v8 none = g2d_v8_splat(0xFFFF), tr = g2d_v8_splat(G2D_TRANSP), ones = g2d_v8_splat(0xFFFF);

	/* ocho píxeles a la vez; la posición 0xFFFF (fuera del fondo) lee un byte cualquiera y su resultado se descarta */
	for (int v = 0; v < 32; v++) {
		g2d_v8 o, valid, m, px;

		memcpy(&o, &ab->off[v * 8], 16);
		valid = g2d_v8_andc(ones, g2d_v8_eq(o, none));
		if (bmpd) {
			const g2d_v8 val = g2d_v8_gather16le(data, g2d_v8_and(o, g2d_v8_splat(0xFFE)));

			m = g2d_v8_and(valid, g2d_v8_gt(val, g2d_v8_splat(0x7FFF)));
			px = g2d_v8_sel(tr, val, m);
		} else {
			const g2d_v8 col = g2d_v8_gather8(data, g2d_v8_and(o, g2d_v8_splat(0xFFF)));
			g2d_v8 base;

			memcpy(&base, &ab->base[v * 8], 16);
			m = g2d_v8_and(valid, g2d_v8_andc(ones, g2d_v8_eq(col, g2d_v8_splat(0))));
			px = g2d_v8_sel(tr, g2d_v8_add(base, col), m);
		}
		c->bg[b][8 + v] = px;
	}
}

/* Capa 3D: la línea del motor de render a colores directos (bit 15) o transparente, y su alfa (c->a3d), desplazada por
 * BG0HOFS (9 bits: lo que cae en 256..511 es transparente). */
static void draw_3d(gpu2d_ctx *c, const g2d_job *j, uint32_t p)
{
	const uint32_t hofs = j->regs[G2D_R_BGHOFS] & 0x1FFu;
	uint16_t px[8], al[8];
	uint32_t semi = 0;

	for (int v = 0; v < 32; v++) {
		for (int k = 0; k < 8; k++) {
			const uint32_t xs = ((uint32_t)(v * 8 + k) + hofs) & 0x1FFu;
			const uint32_t q = xs < GPU2D_W ? c->l3d[p][xs] : 0u;
			const uint32_t a = (q >> 24) & 31u;

			px[k] = a ? (uint16_t)(0x8000u | g2d_3d_555(q)) : (uint16_t)G2D_TRANSP;
			al[k] = (uint16_t)a;
			semi |= a != 0 && a != 31;
		}
		memcpy(&c->bg[0][8 + v], px, 16);
		memcpy(&c->a3d[v], al, 16);
	}
	c->l3d_semi = semi;
}

/* Máscaras de ventana: para cada capa (0..3 fondos, 4 OBJ, 5 efectos) un vector de 0xFFFF donde se permite */
static void build_windows(gpu2d_ctx *c, const g2d_job *j, uint32_t p)
{
	const uint32_t dc = j->dc;
	const uint16_t w0h = j->regs[G2D_R_WIN0H], w1h = j->regs[G2D_R_WIN1H];
	const uint16_t winin = j->regs[G2D_R_WININ], winout = j->regs[G2D_R_WINOUT];
	const g2d_v8 ones = g2d_v8_splat(0xFFFF), zero = g2d_v8_splat(0);
	static const uint16_t ramp_l[8] = { 0, 1, 2, 3, 4, 5, 6, 7 };
	g2d_v8 ramp;

	for (int k = 0; k < 8; k++)
		ramp = g2d_v8_put(k ? ramp : g2d_v8_splat(0), k, ramp_l[k]);
	for (int v = 0; v < 32; v++) {
		const g2d_v8 xv = g2d_v8_add(ramp, g2d_v8_splat((uint16_t)(v * 8)));
		g2d_v8 in[2], ow;

		for (int i = 0; i < 2; i++) {
			const uint16_t reg = i ? w1h : w0h;
			const int x1 = reg >> 8;
			int x2 = reg & 255;

			if (!(dc & (i ? G2D_DC_WIN1 : G2D_DC_WIN0)) || !j->winv[i] || (x1 == 0 && x2 == 0)) {
				in[i] = zero;
				continue;
			}
			if (x2 == 0)
				x2 = 256;
			{
				const g2d_v8 ge1 = g2d_v8_andc(ones, g2d_v8_gt(g2d_v8_splat((uint16_t)x1), xv)); /* x >= x1 */
				const g2d_v8 lt2 = g2d_v8_gt(g2d_v8_splat((uint16_t)x2), xv);                    /* x < x2 */

				in[i] = x1 <= x2 ? g2d_v8_and(ge1, lt2) : g2d_v8_or(ge1, lt2);
			}
		}
		ow = (dc & G2D_DC_OBJWIN) ? c->owin[p][8 + v] : zero;
		for (int l = 0; l < 6; l++) {
			const g2d_v8 c0 = ((winin >> l) & 1) ? ones : zero, c1 = ((winin >> (8 + l)) & 1) ? ones : zero,
				     cow = ((winout >> (8 + l)) & 1) ? ones : zero, cout = ((winout >> l) & 1) ? ones : zero;
			g2d_v8 a = g2d_v8_sel(cout, cow, ow);

			a = g2d_v8_sel(a, c1, in[1]);
			a = g2d_v8_sel(a, c0, in[0]);
			c->allow[l][v] = a;
		}
	}
}

/* ---- etapa 2: componer una línea --------------------------------------------------------------------------- */

/* ---- color final de 8 píxeles a la vez ---------------------------------------------------------------------------- */

/* Colores BGR555 de ocho píxeles de la capa unificada: índice de paleta, o color directo si el bit 15 está. */
static inline g2d_v8 colors_of(const gpu2d_ctx *c, g2d_v8 idx)
{
	const g2d_v8 direct = g2d_v8_gt(idx, g2d_v8_splat(0x7FFF));
	const g2d_v8 pc = g2d_v8_lookup(c->pal, g2d_v8_andc(idx, direct));

	return g2d_v8_sel(pc, g2d_v8_and(idx, g2d_v8_splat(0x7FFF)), direct);
}

/* Los tres canales de 5 bits de ocho colores BGR555 en tres vectores (canal 0 = R). */
#define CH5(col, k) g2d_v8_and(g2d_v8_shr((col), 5 * (k)), g2d_v8_splat(31))

/* g2d_blend de gpu2d_fx.h, ocho píxeles a la vez y con factores por carril. */
static inline g2d_v8 v8_blend(g2d_v8 a, g2d_v8 b, g2d_v8 eva, g2d_v8 evb)
{
	g2d_v8 r = g2d_v8_splat(0);

	for (int k = 0; k < 3; k++) {
		g2d_v8 v = g2d_v8_add(g2d_v8_add(g2d_v8_mul(g2d_v8_shl(CH5(a, k), 1), eva), g2d_v8_mul(g2d_v8_shl(CH5(b, k), 1), evb)),
				      g2d_v8_splat(8));

		v = g2d_v8_shr(v, 4);
		v = g2d_v8_sel(v, g2d_v8_splat(63), g2d_v8_gt(v, g2d_v8_splat(63)));
		r = g2d_v8_or(r, g2d_v8_shl(g2d_v8_shr(v, 1), 5 * k));
	}
	return r;
}

static inline g2d_v8 v8_bright_up(g2d_v8 a, g2d_v8 evy)
{
	g2d_v8 r = g2d_v8_splat(0);

	for (int k = 0; k < 3; k++) {
		const g2d_v8 c6 = g2d_v8_shl(CH5(a, k), 1);
		const g2d_v8 t = g2d_v8_shr(g2d_v8_add(g2d_v8_mul(g2d_v8_sub(g2d_v8_splat(63), c6), evy), g2d_v8_splat(8)), 4);

		r = g2d_v8_or(r, g2d_v8_shl(g2d_v8_shr(g2d_v8_add(c6, t), 1), 5 * k));
	}
	return r;
}

static inline g2d_v8 v8_bright_down(g2d_v8 a, g2d_v8 evy)
{
	g2d_v8 r = g2d_v8_splat(0);

	for (int k = 0; k < 3; k++) {
		const g2d_v8 c6 = g2d_v8_shl(CH5(a, k), 1);
		const g2d_v8 t = g2d_v8_shr(g2d_v8_add(g2d_v8_mul(c6, evy), g2d_v8_splat(7)), 4);

		r = g2d_v8_or(r, g2d_v8_shl(g2d_v8_shr(g2d_v8_sub(c6, t), 1), 5 * k));
	}
	return r;
}

/* g2d_blend3d de gpu2d_fx.h (3D translúcido sobre el objetivo 2), con el alfa del 3D por carril. */
static inline g2d_v8 v8_blend3d(g2d_v8 a, g2d_v8 b, g2d_v8 alpha)
{
	const g2d_v8 fa = g2d_v8_add(alpha, g2d_v8_splat(1)), fb = g2d_v8_sub(g2d_v8_splat(31), alpha);
	g2d_v8 r = g2d_v8_splat(0);

	for (int k = 0; k < 3; k++) {
		const g2d_v8 v = g2d_v8_shr(g2d_v8_add(g2d_v8_mul(g2d_v8_shl(CH5(a, k), 1), fa), g2d_v8_mul(g2d_v8_shl(CH5(b, k), 1), fb)), 5);

		r = g2d_v8_or(r, g2d_v8_shl(g2d_v8_shr(v, 1), 5 * k));
	}
	return r;
}

/* BGR555 → RGBA5551 (gpu2d_to_5551) de ocho colores. */
static inline g2d_v8 v8_to_5551(g2d_v8 c)
{
	return g2d_v8_or(g2d_v8_or(g2d_v8_shl(CH5(c, 0), 11), g2d_v8_shl(CH5(c, 1), 6)),
			 g2d_v8_or(g2d_v8_shl(CH5(c, 2), 1), g2d_v8_splat(1)));
}

/* Máscara «distinto de cero» por carril. */
static inline g2d_v8 v8_nz(g2d_v8 a)
{
	return g2d_v8_andc(g2d_v8_splat(0xFFFF), g2d_v8_eq(a, g2d_v8_splat(0)));
}

static void compose_graphics(gpu2d_ctx *c, g2d_job *j, uint32_t p, uint16_t *out)
{
	const uint32_t dc = j->dc;
	const int use_win = (dc & (G2D_DC_WIN0 | G2D_DC_WIN1 | G2D_DC_OBJWIN)) != 0;
	const uint16_t bld = j->regs[G2D_R_BLDCNT], balpha = j->regs[G2D_R_BLDALPHA];
	const int t1 = bld & 63, t2 = (bld >> 8) & 63, emode = (bld >> 6) & 3;
	const int eva = g2d_ev(balpha & 31), evb = g2d_ev((balpha >> 8) & 31), evy = g2d_ev(j->regs[G2D_R_BLDY] & 31);
	const uint16_t mb = j->regs[G2D_R_MASTER_BRIGHT];
	const int mbon = ((mb >> 14) & 3) == 1 || ((mb >> 14) & 3) == 2;
	int has_semi = 0, need_fx;
	const g2d_v8 ones = g2d_v8_splat(0xFFFF), tr = g2d_v8_splat(G2D_TRANSP);

	const uint32_t tl_ = G2D_T();
	uint32_t tf_;

	/* capas */
	for (int b = 0; b < GPU2D_NBG; b++) {
		const int t = j->bg_type[b];

		if (t == BG_TEXT)
			draw_text(c, j, b);
		else if (t >= BG_AFFINE && t <= BG_LARGE && j->aff_ok[b - 2])
			draw_affine(c, j, b, p);
		else if (t == BG_3D)
			draw_3d(c, j, p);
	}
	obj_draw_ops(c, j, p, j->op_done);
	{
		int has_mos = 0;

		for (uint32_t k = 0; k < j->nops; k++) {
			if (j->ops[k].o.mode == 1 || j->ops[k].o.mode == 3)
				has_semi = 1;
			if ((j->ops[k].o.flags & G2D_OBJ_MOS) && j->ops[k].o.mode != 2)
				has_mos = 1;
		}
		if (has_mos)
			obj_mosaic_x(c, p, ((j->regs[G2D_R_MOSAIC] >> 8) & 15) + 1);
	}
	if (use_win)
		build_windows(c, j, p);
	if (j->bg_type[0] == BG_3D && c->l3d_semi)
		has_semi = 1; /* los píxeles 3D translúcidos se mezclan aunque BLDCNT no pida efectos */
	need_fx = (emode != 0) || has_semi;
	c->stats.fx_lines += need_fx ? 1u : 0u;
	G2D_ACC(c, cl_layers, tl_);
	tf_ = G2D_T();

	/* Capas en el orden en que se pintan: del fondo hacia el frente (prioridad 3 a 0; a igual prioridad, BG3 primero y el OBJ
	 * después de los fondos). Se arma una vez por línea: dentro del bucle de vectores no queda ninguna decisión por capa. */
	{
		int ent[16], nent = 0;

		for (int pr = 3; pr >= 0; pr--) {
			for (int b = GPU2D_NBG - 1; b >= 0; b--) {
				const int t = j->bg_type[b];

				if ((t == BG_TEXT || t == BG_3D || (t >= BG_AFFINE && t <= BG_LARGE && j->aff_ok[b - 2])) &&
				    (int)G2D_BGCNT_PRIO(j->regs[G2D_R_BGCNT + b]) == pr)
					ent[nent++] = b;
			}
			if (dc & G2D_DC_OBJ)
				ent[nent++] = 4 + pr;
		}
		{
			const g2d_v8 eva_v = g2d_v8_splat((uint16_t)eva), evb_v = g2d_v8_splat((uint16_t)evb), evy_v = g2d_v8_splat((uint16_t)evy);
			const g2d_v8 mbf = g2d_v8_splat((uint16_t)g2d_ev(mb & 31));
			const int mbmode = (mb >> 14) & 3;
			const g2d_v8 t1v = g2d_v8_splat((uint16_t)t1), t2v = g2d_v8_splat((uint16_t)t2), one = g2d_v8_splat(1);
			const g2d_v8 six = g2d_v8_splat(6), four = g2d_v8_splat(4), seven = g2d_v8_splat(7);

			for (int v = 0; v < 32; v++) {
				g2d_v8 top = g2d_v8_splat(0), topl = g2d_v8_splat(5), sec = g2d_v8_splat(0), secl = g2d_v8_splat(5),
				       topm = g2d_v8_splat(0);
				g2d_v8 col;

				for (int k = 0; k < nent; k++) {
					const int e = ent[k];
					g2d_v8 s, m;

					if (e < 4) {
						s = c->bg[e][8 + v];
						m = g2d_v8_andc(ones, g2d_v8_eq(s, tr));
						if (use_win)
							m = g2d_v8_and(m, c->allow[e][v]);
						if (need_fx) {
							sec = g2d_v8_sel(sec, top, m);
							secl = g2d_v8_sel(secl, topl, m);
						}
						top = g2d_v8_sel(top, s, m);
						if (e == 0 && j->bg_type[0] == BG_3D) {
							/* 3D: translúcido (alfa < 31) = capa 7, con el alfa en el meta */
							const g2d_v8 a3 = c->a3d[v];
							const g2d_v8 lid = g2d_v8_sel(g2d_v8_splat(0), seven, g2d_v8_gt(g2d_v8_splat(31), a3));

							topl = g2d_v8_sel(topl, lid, m);
							topm = g2d_v8_sel(topm, a3, m);
						} else {
							topl = g2d_v8_sel(topl, g2d_v8_splat((uint16_t)e), m);
							topm = g2d_v8_sel(topm, g2d_v8_splat(0), m);
						}
					} else {
						const g2d_v8 om = c->omet[p][8 + v];
						g2d_v8 semi, lid;

						s = c->oidx[p][8 + v];
						m = g2d_v8_andc(ones, g2d_v8_eq(s, tr));
						m = g2d_v8_and(m, g2d_v8_eq(g2d_v8_and(om, g2d_v8_splat(7)), g2d_v8_splat((uint16_t)(e - 4))));
						if (use_win)
							m = g2d_v8_and(m, c->allow[4][v]);
						semi = g2d_v8_andc(ones, g2d_v8_eq(g2d_v8_and(om, g2d_v8_splat(0x18)), g2d_v8_splat(0)));
						lid = g2d_v8_sel(four, six, semi);
						if (need_fx) {
							sec = g2d_v8_sel(sec, top, m);
							secl = g2d_v8_sel(secl, topl, m);
						}
						top = g2d_v8_sel(top, s, m);
						topl = g2d_v8_sel(topl, lid, m);
						topm = g2d_v8_sel(topm, om, m);
					}
				}
				/* color final de los 8 píxeles */
				col = colors_of(c, top);
				if (need_fx) {
					/* el 6 es un OBJ con mezcla propia y el 7 un píxel 3D translúcido: abajo valen como OBJ y como BG0 */
					const g2d_v8 slk = g2d_v8_sel(g2d_v8_sel(secl, four, g2d_v8_eq(secl, six)), g2d_v8_splat(0),
								      g2d_v8_eq(secl, seven));
					const g2d_v8 fxok = use_win ? v8_nz(c->allow[5][v]) : ones;
					const g2d_v8 is3d7 = g2d_v8_eq(topl, seven);
					const g2d_v8 isobj6 = g2d_v8_or(g2d_v8_eq(topl, six), is3d7); /* los dos fuera del camino normal */
					const g2d_v8 t1bit = v8_nz(g2d_v8_and(g2d_v8_shrv(t1v, topl), one));
					const g2d_v8 t2bit = v8_nz(g2d_v8_and(g2d_v8_shrv(t2v, slk), one));
					g2d_v8 res = col;

					/* lo caro (los dos colores y la aritmética de mezcla) solo si algún carril lo usa */
					if (emode == 1) {
						const g2d_v8 m = g2d_v8_and(g2d_v8_andc(fxok, isobj6), g2d_v8_and(t1bit, t2bit));

						if (g2d_v8_any(m))
							res = g2d_v8_sel(res, v8_blend(col, colors_of(c, sec), eva_v, evb_v), m);
					} else if (emode >= 2) {
						const g2d_v8 m = g2d_v8_and(g2d_v8_andc(fxok, isobj6), t1bit);

						if (g2d_v8_any(m))
							res = g2d_v8_sel(res, emode == 2 ? v8_bright_up(col, evy_v) : v8_bright_down(col, evy_v), m);
					}
					if (g2d_v8_any(is3d7)) {
						/* 3D translúcido: con el objetivo 2, mezcla con su alfa; si no, brillo si BG0 es objetivo 1 */
						const g2d_v8 m7 = g2d_v8_and(is3d7, t2bit);

						if (g2d_v8_any(m7))
							res = g2d_v8_sel(res, v8_blend3d(col, colors_of(c, sec), topm), m7);
						if ((t1 & 1) && emode >= 2) {
							const g2d_v8 mr = g2d_v8_and(g2d_v8_andc(is3d7, t2bit), fxok);

							res = g2d_v8_sel(res, emode == 2 ? v8_bright_up(col, evy_v) : v8_bright_down(col, evy_v), mr);
						}
					}
					if (g2d_v8_any(g2d_v8_andc(isobj6, is3d7))) {
						const g2d_v8 isobj6x = g2d_v8_andc(isobj6, is3d7);
						/* OBJ con mezcla propia: su factor viene en el meta (si no, EVA/EVB), y si el de abajo no es objetivo 2,
						 * puede subir o bajar el brillo si el OBJ es objetivo 1 y hay modo de brillo */
						const g2d_v8 hasb = v8_nz(g2d_v8_and(topm, g2d_v8_splat(16)));
						const g2d_v8 balp = g2d_v8_and(g2d_v8_shr(topm, 5), g2d_v8_splat(15));
						const g2d_v8 a = g2d_v8_sel(eva_v, g2d_v8_add(balp, one), hasb);
						const g2d_v8 bb = g2d_v8_sel(evb_v, g2d_v8_sub(g2d_v8_splat(16), a), hasb);
						const g2d_v8 mb6 = g2d_v8_and(isobj6x, t2bit);

						if (g2d_v8_any(mb6))
							res = g2d_v8_sel(res, v8_blend(col, colors_of(c, sec), a, bb), mb6);
						if ((t1 & 0x10) && emode >= 2) {
							const g2d_v8 mr = g2d_v8_and(g2d_v8_andc(isobj6x, t2bit), fxok);

							res = g2d_v8_sel(res, emode == 2 ? v8_bright_up(col, evy_v) : v8_bright_down(col, evy_v), mr);
						}
					}
					col = res;
				}
				if (mbon)
					col = mbmode == 1 ? v8_bright_up(col, mbf) : v8_bright_down(col, mbf);
				{
					const g2d_v8 o = v8_to_5551(col);

					memcpy(out + v * 8, &o, 16);
				}
			}
		}
	}
	G2D_ACC(c, cl_final, tf_);
}

static void compose_line(gpu2d_ctx *c, const gpu2d_engine *e, int y)
{
	const uint32_t p = (uint32_t)y & 1;
#ifdef G2D_DBG
	g2d_dbg_cur = y;
#endif
	g2d_job *j = &c->job[p];
	uint16_t *out = c->outbuf[p];

	const uint32_t tw_ = G2D_T();

	G2D_STAGE(G2D_ST_COMPOSE);
	/* La salida de la línea y - 2 usó este búfer; lo que necesita esta línea. */
	g2d_wait((1u << (G2D_TAG_OUT + p)) | j->waitmask);
	G2D_ACC(c, cl_wait, tw_);
	for (uint32_t i = 0; i < j->npalchg; i++)
		c->pal[(j->palchg[i] >> 16) & 511u] = (uint16_t)(j->palchg[i] & 0x7FFFu);

	if (j->kind == 0) {
		const uint16_t px = gpu2d_to_5551(g2d_master_bright(0x7FFF, j->regs[G2D_R_MASTER_BRIGHT]));

		for (int x = 0; x < GPU2D_W; x++)
			out[x] = px;
	} else if (j->kind == 2 || j->kind == 3) {
		for (int x = 0; x < GPU2D_W; x++)
			out[x] = gpu2d_to_5551(g2d_master_bright((uint16_t)(g2d_ld16le(c->rawline[p] + x * 2) & 0x7FFFu),
								 j->regs[G2D_R_MASTER_BRIGHT]));
	} else {
		compose_graphics(c, j, p, out);
	}
	{
		const uint32_t tp_ = G2D_T();

		g2d_put(out, e->out_ea + (uint64_t)y * GPU2D_W * 2, GPU2D_W * 2, G2D_TAG_OUT + p);
		G2D_ACC(c, cl_put, tp_);
	}
}

int gpu2d_compose_engine(gpu2d_ctx *c, const gpu2d_engine *e)
{
	if (!pow2(e->bgpages) || !pow2(e->objpages) || e->bgpages > G2D_BG_PAGES_A || e->objpages > G2D_OBJ_PAGES_A)
		return -1;
	if (((e->vram_ea + G2D_VRAM_SIZE + G2D_VRAM_ZERO) >> 32) != (e->vram_ea >> 32))
		return -1;

	const uint32_t ti_ = G2D_T();

	G2D_STAGE(G2D_ST_INIT);
	memset(&c->stats, 0, sizeof c->stats);
	memset(&c->prof, 0, sizeof c->prof);
	/* paleta y OAM crudas (1 KB cada una); la paleta pasa por rawline */
	g2d_get(c->oamraw, e->oam_ea, 1024, G2D_TAG_START);
	g2d_get(&c->rawline[0][0], e->pal_ea, 1024, G2D_TAG_START);
	g2d_wait(1u << G2D_TAG_START);
	for (int i = 0; i < 512; i++)
		c->pal[i] = (uint16_t)(g2d_ld16le(&c->rawline[0][0] + i * 2) & 0x7FFFu);
	for (int s = 0; s < GPU2D_NOBJ; s++)
		decode_one(c, s);
	memcpy(c->regs, e->regs, sizeof c->regs);
	reload_ref(c, 0);
	reload_ref(c, 1);
	memcpy(c->mref, c->ref, sizeof c->mref);
	c->ext_loaded = 0;
	for (int b = 0; b < GPU2D_NBG; b++) {
		c->bg_cur[b] = 0;
		for (int s = 0; s < 2; s++) {
			c->bg_k0[b][s] = 0xFFFFFFFFu;
			c->bg_cbase[b][s] = 0;
			c->bg_sbase[b][s] = 0;
		}
	}
	c->amap_key[0] = c->amap_key[1] = 0xFFFFFFFFu;
	c->amap_len[0] = c->amap_len[1] = 0;
	c->amap_off[0] = c->amap_off[1] = 0;
	c->chg_next = 0;
	c->chg_fetched = 0;
	if (e->nchg) {
		chg_fetch(c, e, 0);
		chg_fetch(c, e, 1);
	}
	/* El estado de las ventanas verticales al empezar el cuadro: dos vueltas por las 263 líneas con los registros de la
	 * instantánea (como la referencia). */
	c->winv[0] = c->winv[1] = 0;
	for (int pass = 0; pass < 2; pass++)
		for (int line = 0; line < 263; line++) {
			win_step_v(c->regs, c->winv, 0, line);
			win_step_v(c->regs, c->winv, 1, line);
		}

	G2D_ACC(c, init, ti_);
	{
		const uint32_t t0_ = G2D_T();

		plan_line(c, e, 0);
		G2D_ACC(c, plan, t0_);
	}
	for (int y = 0; y < GPU2D_H; y++) {
		if (y + 1 < GPU2D_H) {
			const uint32_t t0_ = G2D_T();

			plan_line(c, e, y + 1);
			G2D_ACC(c, plan, t0_);
		}
		{
			const uint32_t t0_ = G2D_T();

			compose_line(c, e, y);
			G2D_ACC(c, compose, t0_);
		}
	}
	{
		const uint32_t t0_ = G2D_T();

		g2d_wait((1u << G2D_TAG_OUT) | (1u << (G2D_TAG_OUT + 1)));
		G2D_ACC(c, end_wait, t0_);
	}
	return 0;
}
